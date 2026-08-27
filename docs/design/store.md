# The local object store

Status: **design proposal**, not ratified. This is the design of the
per-instance local store that sits behind the Layer A 9P export,
plus the much smaller store the monitor uses for its map. The
contract it makes true locally is `docs/design/layer-a.md`; the
platform evidence every cost and every durability claim rests on is
`docs/platform/9front-storage.md`.

Scope: the on-disk format of one instance's disk, the write and read
paths, recovery, space management, concurrency and group commit,
verify/scrub/repair support, the in-memory index, the monitor's map
slot store, the tools, and the test plan. Out of scope: the 9P
surface itself (layer-a §2), replication (layer-a §5.5–5.6), the
placement function, and everything the monitor does with a map once
it has one.

## 0. Conventions and how this document is marked

*Normative.*

- MUST/SHOULD/MAY are RFC 2119 senses.
- **Section references.** A bare `§n` is a section of *this*
  document; a section of `docs/design/layer-a.md` is always written
  `layer-a §n`. The two numbering schemes overlap (both documents
  have a §2.2 and a §3.4), so the prefix is not decoration.
- **The whole of this document is implementation policy in D1's
  sense**: D1 marks "everything about how a node stores its objects
  locally" as the part a conforming reimplementation may do
  differently. Nothing here is on any wire.
- **Within this implementation the on-disk format is a format
  boundary**, and is written that way: byte-exact layouts, MUST/SHOULD
  language, a version field in every header, and a crash-consistency
  argument. Each section below is marked either **format** (a change
  requires a `vers` bump and a reformat; two builds of shoal must
  agree byte for byte) or **policy** (a change is free, no reformat,
  no interoperability consequence).
- Integers on disk are **unsigned little-endian**, packed and
  unpacked with the `GBIT16`/`GBIT32`/`GBIT64` and `PBIT*` macros
  9front exports from `<fcall.h>`, in the 9P convention. Every lib9p
  program already includes that header; shoal uses those definitions
  and MUST NOT redefine them.
- Fixed-width byte fields (checksums, uuids, oids) are byte strings,
  not integers, and are not byte-swapped.
- A **sector** is the unit the device reports; `secsz` is recorded in
  the superblock at format time. All region offsets are in sectors;
  all sizes named `*secs` are in sectors, everything else in bytes.
- **Every byte offset is computed in `vlong`, with the cast on the
  first operand of every multiply.** Grain numbers are `u32` and
  `blksz` is `u32`, so `grain*blksz` evaluates in 32 bits and wraps
  4 GiB into the data region; the offset of grain *g* is
  `(vlong)dataoff*secsz + (vlong)g*blksz` and nothing shorter.
  `ngrains` is `u64` in the superblock but a grain number is `u32`:
  the superblock field is authoritative for capacity and
  `shoalfmt` MUST refuse a geometry whose `ngrains` reaches 2^32.
- A **grain** is the store's allocation unit for object content. It
  is exactly `blksz`, so grain *i* of an object holds checksum block
  *i* (layer-a §1.4) and one number in one array locates both the
  bytes and the digest.
- **`blksz` = `Wunit` = the grain = 16384 bytes**, and that is the
  cluster default `blksz` (layer-a §1.4). One constant, three roles:
  a checksum block, an allocation unit, and one device request.
  `sdvirtio` splits a request at 32 sectors and issues the pieces
  serially, and `devsd` issues exactly one request per `pwrite`, so
  16 KiB is the largest write that is one device round trip
  (`docs/platform/9front-storage.md` §5). Every block write, every log
  record sector run and every checkpoint page is therefore one
  request by construction. §15 records what a smaller grain would
  buy and cost.
- **`Wunit` governs writes only.** The store MUST NOT issue a single
  `pwrite` larger than `Wunit`, because a larger one buys nothing
  and obscures what one device round trip costs. Reads have the
  opposite shape — the driver's split happens inside one syscall, so
  a 64 KiB `pread` is ~24 % faster per byte than four 16 KiB ones
  (`docs/platform/9front-storage.md` §6). **Bulk reads — recovery,
  checkpoint read-back, scrub, `shoalck` — SHOULD use 64 KiB
  requests**; a client-path block read is one grain and one request.
- **All device I/O goes through loop-until-complete wrappers**, and
  every write's length is a sector multiple. `devsd` truncates a
  request to `SDmaxio` and to the partition end rather than splitting
  or failing, so a short count is normal and is never an error; and a
  write whose length is not a sector multiple silently becomes a
  read-modify-write of every sector it touches
  (`docs/platform/9front-storage.md` §5).
- **`Echange` is recognised specifically.** Every I/O on an open
  partition fid fails that way once anything re-declares the unit's
  partitions — an operator re-running `diskparts` is enough
  (`docs/platform/9front-storage.md` §5). The store MUST distinguish
  it from a media error, MUST NOT treat it as corruption, and MUST
  NOT keep serving on a stale fid: it reports the condition and exits
  non-zero, because the alternative is writing object data at offsets
  that now mean something else.
- **Every persistent record carries a checksum**, and it is
  BLAKE2s-128 — a 16-byte unkeyed BLAKE2s digest, the same primitive
  and the same length as a block digest (D7, layer-a §1.4). Chosen
  over a 32-bit CRC because D7 already puts BLAKE2s in the build and
  one primitive means one implementation and one vector set; chosen
  over the 256-bit variant because the 128-bit digest is what
  layer-a §9 already calls ample for non-adversarial corruption
  detection, and because 16 bytes fits a record header without
  pushing it into a second sector. libsec's BLAKE2s runs at
  50–59 MB/s — 4.86 µs for a 256-byte index entry, 265 µs for a
  16 KiB block (`docs/platform/9front-storage.md` §6) — so a commit
  pays ~3 % of its 8.4 ms device write in hashing, but any pass that
  reads and hashes a whole region is bounded by the hashing rather
  than by the disk. §5's start-up bound and §8's verify cost are CPU
  terms for that reason, and both say so.
- A record's checksum is computed over the record's whole byte range
  **with the checksum field itself zeroed**, and verified the same
  way.
- Every header begins `magic` then `vers`. A store MUST refuse to
  open a structure whose `vers` it does not implement, and MUST say
  so rather than guessing. There is no in-place format upgrade in
  v1: a `vers` bump means reformat and refill from peers.
- **The device is an interface, not a syscall.** Every device access
  in the store goes through one small vtable — read, write, flush,
  and the geometry — with two implementations: the real `sd(3)`
  partition plus its raw channel (§3.2), and a simulated disk with a
  volatile write cache used by the T1 tests (§13). Nothing else in
  the store calls `pread`, `pwrite` or opens `/dev/sdXX/raw`. This
  is what makes the flush placements and the crash schedules of §3.4
  testable at all.

## 1. What the store must do

*Policy for this section's wording; the obligations themselves are
layer-a's and normative there.* Restated here as testable statements
so §13 can cite them. Each names its home; none of them is
re-legislated here.

- **R1 durable before ack** (layer-a §5.4, §5.5). Content, key and
  checksum of an accepted write are durable against power loss before
  the primary answers `Rwrite`, and before an acker replies to a
  `/repl` `Twrite`. *Test:* kill power after the ack; the record is
  there.
- **R2 four-tuple atomicity** (layer-a §1.3). After any crash an
  object's published `(content, ver, wepoch, csum)` is exactly the
  pre-update or post-update value, never a mix. *Test:* crash at every
  point in §3.4 and compare `/meta` plus a full re-hash against both
  candidates.
- **R3 staged updates are invisible** (layer-a §5.4 step 3, I2). A
  staged update never appears in `/meta`, `/obj`, `/advert`,
  `op=meta`, `op=get` or arbitration, and never survives a crash as
  anything.
- **R4 discard is free and total** (layer-a §5.4 step 7). Discarding a
  stage leaves content, key and `csum` exactly as they were, still
  verifying, and costs no durable write.
- **R5 restart invalidates currency** (layer-a §5.2). After every
  start, clean or not, no object is `cur` for any epoch.
- **R6 digests are durable** (layer-a §1.4). Per-block digests survive
  restart, so a small write re-hashes one block, and `verify` needs no
  peer.
- **R7 dirty records are durable** (layer-a §7.1, §5.4 step 5b). Each
  `(oid, peer, epoch)` survives a crash, and none is lost while the
  write that caused it is visible.
- **R8 tombstones** (layer-a §1.5). A tombstone is metadata:
  `state=tomb`, `len=0`, content released, key bumped, `mtime`
  retained for the `tombdays` test, enumerable at `/tombs`, and
  removable by a discard that reclaims its space.
- **R9 instance identity on the disk** (layer-a §3.4). A `uuid`
  generated once at format and never changed, readable before the
  instance has a map.
- **R10 size limits** (layer-a §1.2, §1.4). `objmax` default 16 MiB,
  `blksz` default 16384, both powers of two, both immutable
  cluster-wide; a write past `objmax` fails `object too large`.
- **R11 object count.** ~2.6·10^5 objects on a 4 TB disk at the
  defaults; the design is sized to ≤ 2^20 per disk and says where that
  bound is used to buy simplicity.
- **R12 snapshot-at-open enumeration** (layer-a §2.2). A read of
  `/status`, `/map`, `/dirty`, `/stale`, `/lost` or `/jobs` reflects a
  snapshot taken when the fid was opened — that is a MUST there. A
  sequential read of `/obj` or `/tombs` neither skips nor duplicates
  an entry because of concurrent creates and deletes — a SHOULD there,
  which §9 meets.
- **R13 holes** (layer-a §2.4). Bytes never written below `len` read
  as zero and hash as the zero bytes they read as.
- **R14 per-object ordering** (layer-a §5.4 step 2, §5.4.1). All
  operations on one object are totally ordered; operations on distinct
  objects proceed concurrently; a flushed operation performs the whole
  of step 7.
Two obligations layer-a states without giving them a home, which
this store adopts because there is nowhere else for them (§14):

- **R15 pinned `monid` and highest adopted epoch** (layer-a §6.3,
  §8.6). Both must survive a restart or the tripwire and the
  monitor-rebuild procedure do not work.
- **R16 allocated `qid.path`** (layer-a §2.3). From a durable,
  monotonically increasing per-instance counter.

One requirement is this store's own, because nothing above states it
and §5 would otherwise be unfalsifiable:

- **R17 bounded start-up.** Time and memory to open a partition are
  functions of the format constants — `nslots`, `nemap`, `logsecs`,
  `ngrains` — and of nothing about the workload: not the number of
  objects in flight at the crash, not their sizes. §5 states the
  bound and its CPU term; §13 measures it.

## 2. On-disk layout

*Format.*

### 2.1 Geometry and regions

One instance owns one **partition**, opened as `/dev/sdXX/<name>`
through `sd(3)`, and no file system is involved. Writes through
`devsd` are unbuffered and synchronous; ordering between two writes
is ours because the second is issued only after the first returns
(`docs/platform/9front-storage.md`). Random and sequential writes
measured identical on the reference device, so nothing in this layout
is arranged for locality.

The partition is divided into eight regions, in this order:

    sector 0                superblock, copy 0
    sector 1 .. logoff-1    reserved (zero at format)
    logoff   .. +logsecs    write-ahead log, circular
    idxoff   .. +idxsecs    object index, nslots × 256 bytes
    emapoff  .. +emapsecs   extent maps, nemap × emapsz
    dirtoff  .. +dirtsecs   dirty records, ndirty × 256 bytes
    bmapoff  .. +bmapsecs   free-grain bitmap, one copy, paged
    dataoff  .. +datasecs   data grains, ngrains × blksz
    last sector             superblock, copy 1

Copy 1 sits at the last sector of the partition so that no single
device request, and no plausible localised media fault, can reach
both copies.

Derived quantities, all recorded in the superblock so no reader
recomputes them from assumptions:

    nblkmax = objmax / blksz                    (1024 at the defaults)
    emapsz  = roundup(24 + 20*nblkmax, secsz)   (20992 at the defaults)
    ngrains = datasecs / (blksz / secsz)
    nbmpage = ceil(ngrains / (8 * (Wunit - 48)))

`ngrains` MUST be < 2^32: grain numbers are `u32`, and grain 0 is
reserved to mean *no grain* — a hole — at the cost of one unusable
grain at `dataoff`. `shoalfmt` MUST set bit 0 in the bitmap so no
allocator can ever hand it out; an allocator that did would make
every hole in every object alias `dataoff`. At `blksz` 16384 the
`u32` bounds one instance at 64 TiB of data, far beyond the envelope.

**Two slot spaces, not one.** An object occupies an **index slot**
(§2.3) always, and an **extent-map slot** (§2.4) only when it holds
more than one block. `nslots` and `nemap` are sized separately
because their consumers are different: `nslots` is over-provisioned
for Layer C's small, numerous MDS objects, and `nemap` is sized for
objects big enough to need an out-of-line block map. The index entry
carries the whole map of a one-block object inline, so a small
object, a tombstone and a zero-length object cost 256 bytes and
nothing else.

**Sizing.** `shoalfmt` defaults `nslots` to
`min(2^20, 4 * ceil(partsize/objmax))`, `nemap` to
`ceil(partsize/objmax)`, and `logsecs` to 64 MiB worth of sectors.
On a 4 TB partition at the defaults that is `nslots = 2^20`,
`nemap = 2.6·10^5`, and the metadata regions cost 256 MiB (index) +
5.1 GiB (extent maps) + 16 MiB (dirty) + 32 MiB (bitmap) + 64 MiB
(log) ≈ 5.5 GiB, **0.13% of the partition**. The factor of 4 on
`nslots` exists because Layer B fills objects to `objmax` but Layer
C's MDS objects will not; an operator who knows better overrides
both with `-n` and `-e`.

`nemap` is the one sizing in this design that a workload can defeat:
a disk filled with two-block objects needs one extent-map slot per
32 KiB of data and will exhaust `nemap` with the data region nearly
empty. `/status` reports `emapfree=` beside `slotfree=` and
`grainfree=` so the cause of a `disk full` is never ambiguous,
`shoalfmt` prints how many multi-block objects the geometry it chose
supports, and §14 records the residual and the format change that
would remove it.

**One shoal process per `sd` unit.** The device flush (§3.2) is
issued through `/dev/sdXX/raw`. That file is **not** exclusive: the
kernel's interlock is unreachable, two processes can hold it open at
once, and the cdb→data→status exchange is per *unit* state, so two
users on one unit interleave and one command is issued with the
other's cdb bytes (`docs/platform/9front-storage.md` §5). Nothing
detects it. So the rule is a deployment rule enforced by the
operator, not by the kernel: **at most one process per `sd` unit may
issue raw commands**, and third parties that open the raw file —
`scuzz`(8), `disk/smart`, `nusb` — are a hazard on a running store's
unit for the same reason. Inside the process the same hazard is
handled by construction: §3.2 gives the raw channel a single owner.

### 2.2 Superblock

One sector. `hdrlen` is the byte range the checksum covers and is
`secsz`; the remainder of the sector is zero.

    off  size  field
      0     8  magic     "shoalsb\0"
      8     4  vers      format version, 1
     12     4  hdrlen    bytes covered by csum
     16    16  csum      BLAKE2s-128, this field zeroed
     32     8  gen       superblock generation, u64
     40    16  uuid      instance identity, layer-a §3.4
     56     8  ctime     format time, seconds
     64     4  secsz
     68     4  blksz
     72     8  objmax
     80     4  nblkmax
     84     4  emapsz
     88     4  nslots
     92     4  nemap
     96     4  ndirty
    100     4  pad
    104     8  ngrains
    112     8  logoff
    120     8  logsecs
    128     8  idxoff
    136     8  idxsecs
    144     8  emapoff
    152     8  emapsecs
    160     8  dirtoff
    168     8  dirtsecs
    176     8  bmapoff
    184     8  bmapsecs
    192     8  dataoff
    200     8  datasecs
    208     8  ckseq     highest log seq the checkpoint covers
    216     8  cklogoff  log sector where replay must begin
    224     8  qidnext   qid.path high-water, layer-a §2.3
    232     8  epochhigh highest map epoch adopted, layer-a §6.3
    240    16  monid     pinned monitor identity, layer-a §6.3
    256     4  monidset  0 until the first map is adopted
    260   252  reserved, zero

**What the superblock carries, and why each field is here.** The
geometry, because no reader may recompute it from assumptions; the
`uuid`, because layer-a §3.4 requires an identity readable before
the instance has a map (R9); the checkpoint mark `ckseq`/`cklogoff`,
because replay must know where to start (§5); and the three
per-instance facts layer-a requires to survive a restart and gives
no home — `qidnext` (R16), `epochhigh` and `monid`/`monidset` (R15).

**Two-slot rule, validity first.** The rule is stated in three
clauses, and the order matters:

1. If exactly one copy is valid, the update writes **the invalid
   one**.
2. If both are valid, it writes the one with the lower `gen`.
3. If neither is valid, the store MUST NOT write and MUST NOT serve.

In every case the new `gen` is `max(valid gen) + 1`, and the write is
followed by a flush. On start the store reads both copies and takes
the valid one with the greater `gen`. Clause 1 is the whole point of
keeping two copies: without it, a copy torn at a high `gen` steers
the next write onto the only good copy, and a second fault then
leaves no valid superblock and an instance whose data must be
discarded and refilled from peers. At format, copy 0 is written
first with `gen = 0` and copy 1 second with `gen = 1`, so `shoalck`
has a deterministic expectation on a fresh disk.

**Exactly one writer.** Every superblock write carries *all* fields,
so a writer that builds its image from a stale snapshot regresses
whatever another writer advanced. The store therefore has **one
superblock owner** — a single proc; every other proc asks it to
publish. The owner builds the image under its own lock from live
in-memory state, issues one write at a time, and does not overlap
two writes. Five things trigger a publish: format, each checkpoint
(`ckseq`, `cklogoff`), a `qidnext` batch advance, the first `monid`
pin, and an `epochhigh` advance. Without the single owner a
checkpoint publishing a stale `qidnext` at a higher `gen` re-issues
`qid.path` values that layer-a §2.3 forbids reusing, a stale
`epochhigh` makes layer-a §8.6's rebuild republish an epoch that
already exists, a stale `monidset=0` silently un-pins the §6.3
tripwire, and a stale `ckseq` makes replay start after records whose
effects are already materialised — the last of which hands out
grains that live objects reference.

**Durability ordering.** Three MUSTs, each of which is what makes a
field mean anything:

- `epochhigh` MUST be durable **before** the instance takes any
  action under that epoch — before it serves, acks, replicates or
  answers `/status` under it. layer-a §8.6 step (b) has a rebuilding
  monitor read this value out of `/status` and publish above it.
- The `qidnext` high-water MUST be durable **before any path in its
  batch is issued**. The counter advances in batches of 1024: the
  in-memory counter is handed out one at a time, and when it reaches
  the recorded high-water the superblock write is issued and *must
  return* before the next path is given out. So a create costs no
  superblock write 1023 times out of 1024, the recorded value is
  always ≥ any value ever handed out, and a crash wastes at most
  1023 paths out of 2^64.
- `monid` MUST be durable before the instance acts on the map that
  pinned it, for the same reason as `epochhigh`.

`epochhigh` changes only when the instance adopts a higher epoch;
epoch bumps are bounded by map changes and the `tombdays`/2
heartbeat (layer-a §6.1), not by write rate, so this is a rare
8.4 ms write and never inside a client operation.

### 2.3 Index region

`nslots` fixed-size entries of **256 bytes**, indexed by slot number.
The entry is the object's published record: everything a `/meta`
line, an `/advert` line or an `op=meta` response needs, plus the
inline block map of a one-block object, and nothing that scales with
object size.

    off  size  field
      0     1  state     0 free, 1 live, 2 tomb
      1     1  oidlen    1..128
      2     1  flags     bit0 corrupt (layer-a §7.5)
      3     1  vers      entry format version, 1
      4     4  emapslot  extent-map slot, 0 = none (inline map)
      8     8  qidpath
     16     8  len
     24     8  ver
     32     8  wepoch
     40     8  mtime
     48    32  csum      the object checksum, layer-a §1.4
     80   128  oid
    208    16  csum128   BLAKE2s-128 over the entry, this field zeroed
    224     4  grain0    inline map: grain of block 0, 0 = hole
    228    16  dig0      inline map: digest of block 0
    244    12  reserved, zero

**The inline map.** An object with `nblk ≤ 1` — every object of at
most `blksz` bytes, every zero-length object, every tombstone —
carries its whole block map in `grain0`/`dig0` and holds no
extent-map slot (`emapslot = 0`; slot 0 of the extent-map region is
reserved and never allocated). This is what makes the small-object
case, which is the case `nslots`' 4× over-provision exists to serve,
cost 256 bytes of metadata rather than 21 KiB, and it is what makes
layer-a §1.5's "a tombstone occupies a metadata record and nothing
else" true here. A commit that grows an object past one block
allocates an extent-map slot, and one that shrinks it to one block
or fewer frees it; both happen in the same commit as the length
change (§2.7).

A slot's number is not its `qid.path`: slots are recycled when a
tombstone is discarded, `qid.path` values are not. A create over an
existing tombstone reuses the tombstone's slot **and** its
`qidpath`, which is what layer-a §2.3 means by stable across delete,
tombstone and re-create; discarding the tombstone is what releases
both, and the next create of that id allocates a fresh path.

Per-entry rather than per-page checksums, at a 6% cost in the index
region, so that a localised fault damages one object rather than 64.
A slot whose checksum fails **after replay** is listed in `/lost`
with `kind=corrupt` and is not reused; §5 sequences that, and §5 also
requires `oidlen` (1..128), `state` (0..2) and `emapslot`
(< `nemap`) to be range-checked before an entry is believed, because
a damaged entry's own fields are the first thing an in-memory index
build would trust.

### 2.4 Extent-map region

`nemap` entries of `emapsz` bytes, addressed by the index entry's
`emapslot`. This is the per-object block map and digest array
together, so one device read gets both.

    off       size            field
      0         16            csum128, this field zeroed
     16          4            nblk, blocks covered = blkcount(len)
     20          4            vers, 1
     24    4*nblkmax          grain[i], u32; 0 means hole
     24+4*nblkmax
           16*nblkmax         dig[i], BLAKE2s-128 of block i
    ... zero to emapsz

Two parallel arrays rather than an array of 20-byte structs, so the
digest array is a contiguous byte range that can be fed to
`csumdigests` in one call.

At the defaults an entry is 20504 bytes in 20992 bytes of space — 41
sectors, one 64 KiB read. Array entries beyond `nblk` MUST be zero,
and take no part in the object's `csum`.

**No live grain number above `nblk`.** `nblk` is a derived value:
it is `blkcount(len)` and nothing else, and a reader that has both
MUST recompute it from `len` rather than trust the header sector,
which may be the torn one. Applying a commit that reduces `nblk`
MUST clear `grain[i]` and `dig[i]` for every `i ≥ nblk`, whether or
not the commit names those blocks (§2.7). Without that rule a
truncate leaves the old grain numbers in place, the grains are freed
and reallocated to another object, a later extend turns those blocks
into holes only in the digest array — and a read of the extended
object serves the other object's bytes. The rule also keeps
`shoalck`'s bitmap cross-check and `shoalck -R`'s rebuild from
double-counting or leaking grains, since both scan to `nblk`.

`grain[i] == 0` is a hole: block *i* has no grain, reads as zeros,
and `dig[i]` is the digest of `min(blksz, len - i*blksz)` zero bytes.
That value for a full block is a constant the store computes once at
start; the short final block's zero digest is computed when needed.
Holes therefore cost no space and no special case in `csum`
(layer-a §1.4, R13).

### 2.5 Free-grain bitmap

One copy, `bmapsecs` sectors, divided into `nbmpage` pages of
`Wunit` bytes. Each page is one device request and carries its own
header and checksum:

    off  size  field
      0     8  magic  "shoalbm\0"
      8     4  vers
     12     4  page   page number, 0-based
     16    16  csum128 over the whole page, this field zeroed
     32     8  ckseq  the checkpoint generation this page belongs to
     40     8  pad
     48   ...  bits: bit g of the page's range, set = allocated

Per page rather than per copy, because the checksum decides how much
has to be rewritten: a checkpoint writes **only the pages whose bits
changed**, and a page is one device write. A single whole-bitmap
checksum would force a full rewrite of the whole bitmap at every
checkpoint — 32 MiB, 2036 device writes, ~17 s of device time at the
recommended sizing — which is both invisible in any cost model and
fatal to §6's bound on a commit waiting for log space.

**One copy, rebuilt when it fails.** The bitmap is not authoritative:
the truth is the set of grains referenced by committed index and
extent-map state, and the bitmap is a cache of that truth so start-up
need not scan it. A second copy would not be redundancy — the
inactive copy belongs to the previous checkpoint, and §2.8's reclaim
rule only guarantees that the log still covers the *newly published*
`cklogoff`, so the older copy can never be rolled forward. So there
is one copy, and a page that fails its checksum at start is repaired
automatically: the store rebuilds the whole free map by scanning
every live object's map — the inline map in the index entry for
one-block objects, the extent-map entry for the rest — logs the
event, reports `bmaprebuild=yes` in `/status`, and writes a fresh
bitmap. **The rebuild costs one read of every live multi-block
object's extent map**: 5.5 GB and ~70 s of I/O plus ~100 s of
hashing on a full 4 TB disk, proportionally less on an emptier one,
and nothing at all on a disk of one-block objects. That is a slow
start; it is not an operator procedure at three in the morning, and
`shoalck -R` remains for the offline case.

A page's `ckseq` MUST be ≤ the superblock's `ckseq`. A page carrying
a **higher** `ckseq` proves the superblock the store chose is stale —
a media fault on the newer copy, or a lost superblock write — and the
store MUST NOT start, because replaying from a stale mark over
already-materialised state is how grains that live objects reference
get handed out again.

### 2.6 Dirty-record region

`ndirty` entries of 256 bytes (default `ndirty` = 65536, 16 MiB). This
is layer-a §7.1's fine-grained dirty set, which layer-a §5.4 step 5b
requires durable no later than the ack (§14(2)).

    off  size  field
      0    16  csum128, this field zeroed
     16     8  epoch
     24     1  state   0 free, 1 used
     25     1  oidlen
     26     1  peerlen
     27     1  vers
     28   128  oid
    156    72  peer    the iid
    228    28  reserved, zero

72 bytes bounds an iid: layer-a §3.3 bounds a node name at 63
characters and an index is a decimal integer, so 63 + `.` + an
8-digit index reaches exactly 72. `peerlen` is a `u8` and can name
more than the field holds; the store MUST reject `peerlen > 72` and
`oidlen > 128` on read as well as on write.

`ndirty` is an implementation limit in exactly layer-a §7.1's sense.
When it is exhausted the store discards every fine-grained record for
the peer with the most records and marks that peer `fullsync`, which
layer-a §7.1 explicitly permits.

**`fullsync` flags are not persisted.** On start the store sets
`fullsync` for every peer, because that is the safe default and a
restart has to run a reconcile pass anyway. This removes a durable
structure entirely, and it is also what makes §14(2)'s folding of
the dirty record into the commit safe rather than merely cheap: the
one window layer-a's stated ordering covers and the folded ordering
does not is a crash between the acker's commit and the primary's,
after which the primary has no record that the peer is behind. The
restart flag covers exactly that window, so §14(2) proposes the
layer-a amendment and the restart obligation together.

### 2.7 Log region and record format

*Format.* A circular region of `logsecs` sectors. It carries every
durable state change other than object content: the published
four-tuple, extent-map deltas, extent-map slot allocation and
release, grain allocations and frees, dirty records, and slot frees.

A **record** is `nsec` contiguous sectors whose first sector is the
header:

    off  size  field
      0     8  magic  "shoallog"
      8     4  vers
     12     4  nsec   sectors in this record, header included
     16    16  csum128 over the whole record, this field zeroed
     32     8  seq    u64, strictly increasing, never reused
     40     8  time   seconds, diagnostic only
     48     4  nent   entries in this record
     52     2  flags  bit0 Fwrap (see below)
     54     2  pad
     56   ...  entries, a packed byte stream running to nsec*secsz

A record is **valid** iff `magic` and `vers` match, `nsec` is within
the log region from this offset, `seq` is the expected successor
(§5), and `csum128` verifies over the whole `nsec*secsz` bytes.
`nsec` MUST be bounds-checked before it is used to address anything:
a torn header can carry a garbage length, and hashing an unbounded
range on the strength of an unverified field is how a replay turns a
crash into a fault.

A record MUST NOT straddle the end of the region. If the next record
does not fit before the end, the writer emits a one-sector record
with `nent=0` and `Fwrap` set, and places the real record at the
region start. The wrap record carries a sequence number like any
other, so sequence contiguity is unbroken; a reader that has applied
a record with `Fwrap` set continues at the region start rather than
at `+nsec`. A wrapping commit is therefore two device writes and not
one, which is the one exception to §11's per-commit cost.

Entries are `{u8 kind, u8 flags, u16 pad, u32 len}` — `len` counting
the whole entry including this eight-byte header — followed by the
body. The length is `u32` rather than `u16` because a whole-object
`Eobj` is `~230 + 28*(objmax/blksz)` bytes: 28.2 KiB at the defaults,
and 112 KiB at a `blksz` of 4096, which a `u16` cannot encode at all.
The format does not bound the block count per object; `shoalfmt`
does, by refusing a geometry whose maximal record does not fit an
eighth of the log region.

**`Eobj` (kind 1)** — publish an object's state.

    u32  slot
    u32  emapslot     0 = inline map / none
    u64  qidpath
    u8   state, u8 oidlen, u8 oflags, u8 pad
    u64  len
    u64  ver
    u64  wepoch
    u64  mtime
    u8   csum[32]
    u8   oid[oidlen]
    u32  nmap
    nmap × { u32 blk; u32 grain; u8 dig[16] }
    u32  nfree
    nfree × u32 grain

`nmap` names only the blocks this commit changes; every other block
below `nblk` is unchanged. `nfree` names the grains this commit
releases — the old grains of the blocks it replaced, and every grain
beyond a new shorter `len`. Applying an `Eobj` sets absolute values,
and it MUST, in this order: set the four-tuple and `len` in the index
entry; derive `nblk = blkcount(len)`; set `grain[b]`/`dig[b]` for
each named block; **clear `grain[i]` and `dig[i]` for every
`i ≥ nblk`**; free every grain in `nfree`; and, if `emapslot` differs
from the entry's current value, allocate or release the extent-map
slot accordingly. The clearing clause is §2.4's invariant, and it is
what makes a truncate that names no blocks correct rather than
merely cheap. When `emapslot` is 0 the map being set is the index
entry's inline `grain0`/`dig0`, and `nmap` names at most block 0.

**`Edirty` (kind 2)** — `{u8 op (0 remove, 1 add), u8 peerlen,
u8 oidlen, u8 pad, u64 epoch, peer[], oid[]}`.

**`Eslot` (kind 3)** — `{u32 slot}`, free the index slot and its
extent-map slot if it holds one: a tombstone discard (layer-a §1.5)
or an `op=drop` (layer-a §7.4).

**Record sizes at the defaults, which is the point of this format.**
An `Eobj` for a one-block write with a 20-byte oid is 148 bytes and
fits in the header sector: **a lone small commit is one 512-byte
write.** An `Eobj` for a whole-object 16 MiB `op=full` — 1024 changed
blocks and 1024 freed grains — is 28.2 KiB, so the record is 57
sectors: two `Wunit` writes of body and one of header (§3.2). Every
commit this store can be asked for is therefore at most three device
writes and two flushes, and the common one is one write and two
flushes.

### 2.8 What is checkpointed and what is authoritative

*Policy.*

The log is the durable authority for everything since the last
checkpoint. The index, extent-map, dirty and bitmap regions are a
**checkpoint**: a materialisation of the log's effect up to
`ckseq`, written incrementally by a checkpointer proc, and
re-derivable by replay. Nothing in the write path writes them.

A checkpoint:

1. writes every **dirty** index page, extent-map entry, dirty record
   page and bitmap page — only the dirty ones, each in `Wunit`-sized
   pieces, coalesced where slots happen to be adjacent in a page;
2. issues one device flush;
3. writes the superblock with the new `ckseq` and `cklogoff`, and
   flushes.

Its cost is therefore proportional to the state dirtied since the
last checkpoint and to nothing else, which is what lets §6 put a
number on how long a commit may wait for log space.

`ckseq` MUST NOT exceed the log writer's **durable watermark** (§7).
Materialising in-memory state whose backing batch has not reached
the watermark, and then publishing a `ckseq` past a record that never
became durable, would leave the checkpoint describing a write that
does not exist.

Log space before the *newly published* `cklogoff` is reclaimed only
after step 3 has returned. Reclaiming earlier would let a crash leave
a superblock naming an older checkpoint whose log has already been
overwritten, which is the one way this format can lose data; §13
tests it with a hook that reclaims early.

Checkpoints run when the log passes `ckhigh` (default one quarter
full) and every `ckms` (default 30000) if anything is dirty — both
policy, both tunable without a format change. A quarter rather than a
half because the checkpointer's job is to keep the log from ever
being full, and starting earlier is what keeps §6's wait rare.

## 3. Write path

*Policy for the mechanics; the invariants it establishes are
layer-a's.*

This maps layer-a §5.4 steps 3, 6 and 7 onto the format above. Steps
1, 2, 4 and 5a are not the store's business.

### 3.1 Stage — step 3

Serialised against every other operation on the same object by §7's
queue:

1. Read the extent-map entry if the object has one and it is not in
   the cache (one 64 KiB read, 41 sectors at the defaults); a
   one-block object's map is already in the in-memory index entry.
2. For each block the operation touches, allocate a **fresh** grain
   from the in-memory free map. The allocator never returns a grain
   that any committed map references, and never returns one released
   by a commit that is not yet durable (§3.5). This is what makes a
   stage invisible: it writes only where nothing is published.
3. Compose each touched block's new content. A write covering a whole
   block needs no read. A partial write reads the old grain (or takes
   zeros for a hole), merges the new bytes, and re-hashes the block —
   §4 works this through.
4. Write the new grains, one device request each. These writes are
   durable but unreachable: no index entry, no extent map and no
   bitmap names them.
5. Compute the new block digests, then the new `csum` by hashing the
   whole digest array with the changed digests substituted
   (`csumdigests`; 16 KiB of digests for a maximal object, 265 µs,
   and 16 bytes for a one-block object).
6. Build the `Eobj` entry in memory, together with any `Edirty`
   entries layer-a §5.4 step 5b calls for, and — if the object
   crosses one block in either direction — the extent-map slot
   allocation or release.

Nothing in this step is published. Nothing in it is a commit.

### 3.2 Commit — step 6

The staged entries are handed to the log writer, which batches them
with any other commits pending at that instant (§7) and performs:

    write the record's body sectors  — every sector but the first
    one device flush                 — covers the batch's grain
                                       writes and the body
    one pwrite of the header sector  — the commit point
    one device flush                 — makes the record durable

and then wakes the waiters, which answer `Rwrite`. For a record of
one sector — the common commit — the body is empty and the sequence
is flush, write, flush.

**Why one sector plus a flush is the atomicity point.** A record is
valid only if its checksum verifies over its whole byte range and its
sequence number is the expected successor. A record that landed only
in part fails the checksum; a record whose header sector was torn
fails it too, because a torn sector leaves old bytes or new bytes in
every field and either way the stored digest and the hashed range
disagree; a header that did not land at all leaves bytes that are not
a valid header, and even if they were, the sequence test rejects
them. So the design assumes **no multi-sector atomicity and no
single-sector atomicity**: writing the header last makes the commit
point one sector for any record size, and a torn sector is caught by
the same checksum as a missing one. What it does assume is that a
byte holds either its old value or its new one and not, say, a value
from a third write — the weakest assumption a block device can be
given, and the one §16a(7) still asks the fleet to confirm.

That argument is why the header goes last unconditionally
rather than only for records that exceed `Wunit`: it removes any
dependence on the order in which a multi-request write reaches the
platter, at the price of nothing, since the flush that covers the
body is the flush the grains needed anyway.

**Sequence numbers, not offsets.** A record that did not land at all
leaves whatever the previous lap wrote at that offset. Records are
variable length, so lap boundaries do not coincide and that is
generally a *body* sector of an older record rather than a header.
It is rejected because it is not a valid header and because its
sequence number is not the expected successor — not because anything
about the geometry anchors it. Sequence numbers are never reused, so
no record from any earlier lap can be accepted, and a non-record byte
range fails validation with probability 1 − 2^−128.

**The flushes are not optional and their position matters.**
`sdvirtio` does not negotiate `VIRTIO_BLK_F_FLUSH` and the reference
device behaved write-through, but on AHCI with a volatile cache the
commit record could reach the platter while a staged grain sits in
cache — which is not only R1 at risk but R2, layer-a §1.3's normative
four-tuple atomicity: a published key whose content never landed is
exactly the copy layer-a §1.3 describes as winning arbitration and
overwriting a good one. A device flush is device-wide, so one flush
after every stager in the batch has finished its grain writes covers
all of them; it costs 175 µs against an 8.4 ms write.

**The raw channel has one owner.** The flush is a SCSI
`SYNCHRONIZE CACHE` issued through `/dev/sdXX/raw` as a
write-cdb / read-data / read-status triple, 4.2 µs per round trip on
a held fd against 462 µs if the file is opened and closed around each
command. The triple is per-unit kernel state, so it MUST NOT be
interleaved: within the store, **one flusher proc owns the raw fd**
and every flush — the log writers', the checkpointer's, the
superblock owner's — is a request to it. The flusher coalesces:
a caller asks for "a flush that began after time *t*", and one
device flush satisfies every caller waiting at the moment it is
issued, which is what keeps `logdepth` concurrent log writers from
costing `logdepth` flushes. Outside the store the same exclusion is
the operator's (§2.1).

**Starting without the flush channel is refused.** If the raw
channel cannot be opened — no raw file, or permissions — the store
MUST NOT start, unless the operator passes **`-w`**, which asserts
that the unit is write-through or that its write cache is disabled.
`-w` is an operator claim, not an observation: it is logged at start
in those words, and `/status` reports `flush=raw` or
`flush=asserted-writethrough` so the claim is visible to anyone
reading the instance later. There is no silent downgrade, because
the property being downgraded is normative in layer-a and unverified
on the platform: `docs/platform/9front-storage.md` is explicit that
write-through on the reference device is inference rather than
confirmation and that host power loss was never tested. On a legacy
IDE unit the flush is silently faked by the driver, so `-w` is also
the honest flag there, together with disabling the drive's write
cache; `cat /dev/sdctl` names the driver and `/status` reports it.

After the commit returns, the in-memory index entry, extent map, free
map and dirty set are updated, the grains the commit released are put
on the batch's deferred-free list (§3.5), and the operation leaves
its queue.

### 3.3 Discard — step 7, `Tflush` and failure

Return the staged grains to the in-memory free map, drop the composed
entries, leave the queue. **No durable write.** The bytes written
into those grains in §3.1(4) are simply not referenced by anything
and are overwritten by whoever allocates them next.

This is what makes layer-a §5.4 step 7's "the local object is
untouched: same content, same key, same `csum`, still verifying"
true by construction rather than by care: the published state was
never modified, so there is nothing to roll back.

**Every step-7 exit runs the whole of step 7.** Discard the stage,
clear the per-(object, peer) sync state for every candidate involved,
invalidate `cur` for the object, and re-run the currency check before
serving it again. That includes the two exits that are easy to miss:
a `disk full` raised at *commit* time, which happens after layer-a
§5.4 step 4 and so can leave ackers holding `(E, ver+1)` while this
instance holds `(E, ver)`; and a `Tflush` that arrives after the
commit is already durable, where the discard half is vacuous but the
invalidate half is not (§14(10)).

### 3.4 Crash points

The invariant is R2: the published four-tuple is the old one or the
new one. Taking the points in order:

| Point | What is on disk | Outcome |
|---|---|---|
| P0 before any grain write | nothing changed | old |
| P1 during or after grain writes, before enqueue | new bytes in grains nothing references; the checkpointed bitmap does not mark them allocated and no log record allocates them | old |
| P2 after enqueue, before the pre-flush | as P1 | old |
| P3 during the body write, or during the header write | no valid header, or a header that fails its checksum or its sequence test | old |
| P4 after the header write returns, before the post-flush | the record is durable or it is not; if not, P3 | old or new, and nothing acked either way |
| P5 after the post-flush, before `Rwrite` | new | new; layer-a §5.4's "may or may not have been applied" covers exactly this |
| P6 after `Rwrite` | new | new, guaranteed — R1 |
| checkpoint, mid-way | half-written index/extent-map/bitmap pages; the superblock still names the *old* `ckseq` | replay re-applies from the old mark over the half-written pages; entries carry absolute values, so replay is idempotent |
| superblock write | the other copy is valid and one generation older | replay from its (older) mark, which the reclaim rule guarantees the log still covers |

A batch carrying several objects' commits is one record, so the whole
batch is at the same point at every instant. §7 explains why a batch
that lands while a lower-numbered batch does not is never acked.

The checkpoint row is the reason §5 replays *before* it condemns
anything. A crashed checkpoint can leave a torn 512-byte sector
holding two 256-byte index entries, or a torn extent-map entry; both
fail their checksums and both are restored byte for byte by replay,
because a slot is dirty at checkpoint *N* only if some record in
`(ckseq(N-1), ckseq(N)]` touched it, and replay after a crash before
checkpoint *N*'s superblock write starts at `cklogoff(N-1)` and so
covers exactly that range. **The set of structures a crashed
checkpoint can damage is a subset of the set replay repairs.**
Entries in a written page that did *not* change are rewritten with
byte-identical content, so any mixture of old and new bytes in their
sectors is still their correct image.

### 3.5 The deferred-reuse rule

**A grain, an index slot or an extent-map slot released by a commit
MUST NOT be reallocated until that commit's post-flush has
returned.**

Without it: object A's commit frees grain *g* and is lost in the
crash; meanwhile *g* was handed to object B, staged into, and B's
commit was lost too. Replay restores A's old extent map, which still
points at *g* — whose bytes are now B's. A four-tuple that verifies
against nothing, produced by two writes that both correctly failed.

The implementation is a per-batch deferred-free list drained when the
batch's flush returns. It costs one pointer per batch and it is the
only ordering rule in the write path that is not obvious from the
log.

### 3.6 Multi-request stages — `op=full`

*Policy.* layer-a §5.5 stages a whole-object resync across many
`Twrite`s, pipelined, with the arbitration comparison made once at
`final=1` against the receiver's then-current key. So the object is
explicitly not held across the transfer, and a stage outlives the
request that created it. That is the one stage in this design with a
lifetime longer than one operation, and it gets an explicit one.

A **stage handle** is created by the first chunk of an `op=full` for
an object on a `/repl` fid, and holds: the oid, the declared final
`len`, `force`, the grains staged so far with their digests, and the
time of the last chunk. It is owned by the fid.

- Each chunk allocates fresh grains from the in-memory free map and
  writes them (§3.1 steps 2–4). Nothing is published and no log
  record is written.
- **`final=1`** takes the object's queue (§7), re-reads the
  receiver's current key, applies layer-a §5.5's comparison against
  it — greater, or equal with `force=1` — and, if it passes, commits
  one `Eobj` naming every staged block and freeing every grain the
  object held before. If it fails, the stage is discarded exactly as
  below and the error is layer-a §5.5's.
- **Lifetime.** A stage is discarded, and its grains returned to the
  free map, on `Tclunk` of the fid, on a `Tflush` of any of its
  chunks, when no chunk for it has arrived for `stagems` (policy,
  default 30·`replms`), and at restart — which is free, because a
  stage is memory-only: its grains are allocated from the in-memory
  free map and named by no record, so no restart path can resurrect
  them and none can leak across one.
- **Bound.** At most `stagemax` grains may be staged on one `/repl`
  fid (policy, default 2048, i.e. two maximal objects). A chunk that
  would exceed it fails `disk full`, and `/status` reports
  `staged=<grains>`. Without a bound, a sender that dies mid-transfer
  leaves grains held by nothing until the fid is clunked, and a heal
  of a whole disk can answer `disk full` with the disk nearly empty.

## 4. Read path, holes and re-hashing

*Policy.*

**Read.** Serialised against writes to the same object by §7's queue:
clamp to `len` (a read at or past `len` returns 0, a read crossing
`len` returns only the bytes below it), then for each block in range
either `pread` from `(vlong)dataoff*secsz + (vlong)grain[i]*blksz`
or, when `grain[i]` is 0, deliver zeros. The map is the index
entry's inline map for a one-block object and the extent-map entry
otherwise. Serialising the read against commits on the same object
is what stops a grain being freed under the reader; §3.5 makes that
sufficient.

**Write of a whole block.** No read. Compose, hash, allocate, write.

**Partial write.** The block's new content is (old grain bytes, or
zeros for a hole) with the new bytes merged in, hashed as one block.
Only that block's digest changes; `csum` is then re-derived from the
whole digest array, which is why the array is stored contiguously.

**Changing `len` changes digests without changing bytes.** Layer-a
§1.4 hashes the final partial block over its *actual* length, so:

- extending `len` within the last block re-hashes that block over
  more bytes (the extension reads as zeros);
- extending across blocks makes every wholly-new block a hole whose
  digest is the precomputed full-block zero digest, and re-hashes the
  block containing the old `len`;
- truncating re-hashes the block containing the new `len` over fewer
  bytes, frees every grain beyond it, and clears every map entry at
  or beyond the new `nblk` (§2.4).

A truncate that lands on a block boundary therefore costs no data
read and no data write: it is an `Eobj` with `nmap` empty, `nfree`
naming the released grains, and the apply rule doing the clearing.
So is a delete. What it is not is a commit that leaves the map
alone — that is the bug §2.4 exists to forbid.

**The digest array is consistent with content by construction.** The
new grain and the new digest for block *i* are published by the same
`Eobj`, in the same record, at the same commit point; there is no
window in which one is visible without the other. That is R2 applied
to layer-a §1.4's second level, and it is why the digests live in the
same commit as the block map rather than in a structure of their own.

## 5. Start-up: recovery and replay

*Policy.*

The order below is load-bearing in one place: **nothing is condemned
before replay has run.** A crashed checkpoint damages exactly the
structures replay repairs (§3.4), so a store that judges the index
before replaying puts live objects into `/lost` after an ordinary
power cut — and layer-a §7.5 then makes them fail client access and
lose arbitration against everything including absence.

1. Open the partition and `/dev/sdXX/raw`. If the raw channel cannot
   be opened the store MUST NOT start unless `-w` was given (§3.2).
2. Read both superblocks. Take the valid one with the greater `gen`.
   If neither is valid the store MUST NOT start; §12's tools are what
   the operator reaches for.
3. Check `secsz` against the device, and `vers` against the build.
   Refuse a mismatch. Check the geometry for self-consistency: every
   region inside the partition, no overlaps, `ngrains` < 2^32.
4. Read the index region in 64 KiB requests and build the in-memory
   index (§9) **tolerantly**: verify every entry's checksum and
   range-check `oidlen` (1..128), `state` (0..2) and `emapslot`
   (< `nemap`), and record which slots failed — but condemn nothing
   yet.
5. Read the bitmap. A page whose `ckseq` exceeds the superblock's
   means the superblock is stale and the store MUST NOT start (§2.5).
   A page that fails its checksum sets the rebuild flag; it is not a
   refusal.
6. Replay. Start at `cklogoff` with the expected sequence seeded at
   **`ckseq + 1`** — the seed is what makes an earlier lap's record
   unacceptable however plausible its bytes — and, for each record:
   bounds-check `nsec` against the region before using it, verify the
   checksum over `nsec*secsz`, check `seq` against the expectation,
   apply the entries, then continue at the region start if `Fwrap` is
   set and at `+nsec` otherwise. Stop at the first record that is
   invalid or out of sequence. Applying an entry means setting
   absolute values — this slot's four-tuple becomes these bytes,
   block *i* becomes grain *g* with digest *d*, blocks at or beyond
   `nblk` become holes, this grain becomes allocated, this
   extent-map slot becomes this object's, this dirty record exists —
   so replay is idempotent and a partially checkpointed region is
   corrected by it.
7. Read the extent-map entry of each replayed slot that has one, and
   apply its deltas. `nblk` is recomputed from the replayed `len`,
   never trusted from the entry's header sector. **A failing
   `csum128` on a replayed entry is not fatal**: the bytes replay
   does not name are byte-identical old-or-new and therefore intact,
   so applying the deltas and recomputing the checksum restores the
   entry. Every grain number read out of such an entry is
   bounds-checked against `ngrains`; one out of range is damage
   replay did not cover, and the slot goes to `/lost`. A failing
   `csum128` on an entry replay did *not* touch is fatal for that
   slot in the same way.
8. Condemn what is left. An index entry that still fails its checksum
   after replay, or fails a range check, is genuine media damage: it
   is listed in `/lost` with `kind=corrupt`, its slot is not reused,
   and it is not served. `/lost` requires an `oid=` field, which a
   damaged entry cannot supply honestly — the store reports the slot
   number as the `oid=` value prefixed `slot:` and says so in the
   line, rather than printing 128 bytes it does not trust.
9. Complete the free map: the bitmap, plus every allocation and free
   replay applied. If step 5 set the rebuild flag, rebuild
   it instead by scanning every live object's map (§2.5), log the
   event, and report `bmaprebuild=yes`.
10. Set `fullsync` for every peer (§2.6). Clear `cur` for every
    object — which costs nothing, because `cur` is never on disk
    (§14(1)).
11. Set the log tail after the last valid record, take `qidnext` from
    the superblock, and start serving.

**Cost bound (R17).** Steps 4–7 read the index region, the bitmap and
at most the log, and hash everything they read. At the defaults and
`nslots = 2^20` that is 256 MiB + 32 MiB + ≤ 64 MiB in 64 KiB
requests — 4.4 s at the measured 80 MB/s — plus the hashing, which is
the larger term: 2^20 index entries at 4.86 µs each is 5.1 s of CPU.
Call it ten seconds at the recommended sizing, half of it hashing.
Step 7 adds one extent-map read per multi-block object touched since
the last checkpoint. The bound is a function of format constants and
of nothing else — in particular not of the number of objects that
were being written when the crash happened, and not of their sizes.
The rebuild path of step 9 is the exception and says so.

**What an operator sees when it fails.** Every refusal above prints
one line naming the structure, the offset, and the tool that
addresses it, and exits non-zero — the store never starts in a
degraded mode it did not name. A corrupt index entry is a running
store with an object in `/lost`. A bitmap page that fails its
checksum is a slow start, not a refusal. A bitmap page from a future
checkpoint, or two invalid superblocks, is a store that will not
start: for the latter the honest answer is `shoalfmt -r` plus refill
from peers, since the disk's identity is gone, which by layer-a §1.5
makes it a reformat-before-rejoin case anyway.

## 6. Space management

*Policy.*

**Allocator.** Grains are fixed-size and interchangeable, so
allocation is "find a clear bit". The in-memory free map is a bitmap
plus a rotating cursor and a free count; allocation is O(1)
amortised and there is **no external fragmentation and no
coalescing**, because there is nothing of a different size to
coalesce. That is the whole payoff of tying the grain to `blksz`:
one number per block locates the bytes and indexes the digest, one
bit per grain manages space, and the allocator is thirty lines.

The cost is internal fragmentation: an object of one byte occupies
one grain (16 KiB), and every object statically reserves 256 bytes of
index. At the recommended sizing that is 0.13% of the partition for
metadata plus up to `blksz-1` per object's final block.

**Slots.** Two free lists in memory, rebuilt at start: index slots
from the index region, extent-map slots from the `emapslot` fields of
live entries. A slot of either kind is reused only after the commit
that freed it is durable (§3.5).

**Reclaim of tombstones.** A tombstone's content is released at
delete time — the `Eobj` that sets `state=tomb` carries `len=0`,
`nmap` empty, `nfree` naming every grain the object held, and
`emapslot=0`, so the extent-map slot is released with the content.
What survives is the 256-byte index entry. Layer-a §1.5's discard,
once its three cluster-wide conditions hold, commits an `Eslot` and
the slot returns to the free list. `tombdays` is evaluated against
the entry's `mtime`, which is why the tombstone keeps one.

**Disk full.** Four distinct exhaustions, mapped deliberately:

- *No free grain*, or a write that would need more grains than
  remain: `disk full` (layer-a §2.6), raised at stage time — before
  layer-a §5.4 step 4 — so it never leaves an acker ahead of the
  primary.
- *No free index slot*: `disk full` on create. A tombstone occupies
  a slot, so a cluster that cannot discard tombstones (layer-a
  §1.5's absent-instance case) can exhaust slots before it exhausts
  grains.
- *No free extent-map slot*: `disk full` on the write that would
  take the object past one block. This is the exhaustion that can
  arrive with the data region far from full (§2.1), which is why
  `/status` reports `grainfree=`, `slotfree=` and `emapfree=`
  separately and why the tools print all three.
- *No free log space*: the commit **waits** for the checkpointer,
  and then answers `disk full`.

**The log's reserved tail.** The last `logresv` sectors of free log
space (policy, default one sixteenth of `logsecs`) are usable only by
commits that free space: an `Eobj` that releases grains without
allocating any — delete, truncate, tombstone — and an `Eslot`. So
"delete always works" is true rather than nearly true: without the
reservation, log exhaustion blocks the delete and the tombstone
discard that would have relieved the slot exhaustion, and the two
exhaustions lock each other in exactly the direction that removes the
escape.

**The wait, and its bound.** Waiting is right because log exhaustion
is normally transient: the checkpointer is already running and the
space is already reclaimable. The bound is `ckwaitms` (policy,
default 5000), and it is derived from what a checkpoint costs rather
than from `replms`, which a checkpoint cannot be made to fit inside.
What makes it meetable is §2.8's incremental checkpoint: the work is
proportional to *dirty state*, so each object touched since the last
checkpoint costs one index page, at most one extent-map entry (two
`Wunit` writes) and at most one bitmap page — **once**, however many
times it was written in that interval. Repeated writes to the same
objects therefore amortise to nearly nothing, and `ckhigh` at a
quarter full starts the drain long before the log fills. The
expensive shape is a workload that touches a fresh object with every
commit and never returns to it, where the checkpoint costs up to
three device writes per commit on top of the commit itself (§11); a
wait that reaches `ckwaitms` means the offered rate has exceeded the
device's sustained commit-plus-checkpoint rate for a whole log's
worth of commits. That is saturation, not a hiccup, so `disk full` —
a definitive error — is by then a definitive condition. §14(6)
records why no retryable error is added to the wire for this, and
§16a(6) asks for the dirty-state measurements that would turn
`ckwaitms`' default from a guess into a number.

## 7. Concurrency and group commit

*Policy, except where it realises layer-a §5.4.1, which is
normative there.*

**The service loop.** `lib9p`'s `srv` loop is single-threaded — 9p(2)
says so, and `Srv` has no mode flag. A handler that blocks blocks
everything, including the `Tflush` that layer-a §5.4.1 requires to be
answerable. 9front offers two ways out: `srvrelease`/`srvacquire`
around a blocking handler, and `9pqueue`(2)'s `Reqqueue`, which needs
libthread. The store takes the second (D12): it is a libthread
program using `threadpostmountsrv`, and object operations are pushed
to a **pool of `Reqqueue`s hashed by oid** (policy; default 64
queues).

That gives R14 for free and deletes a structure. Each `Reqqueue` has
one proc and runs its pushed requests serially, so two operations on
one object are totally ordered by construction — same oid, same
queue — and operations on distinct objects run concurrently, in
different queues. There is **no per-object lock**: the per-object
lock the write path would otherwise hold from layer-a §5.4 step 2 to
step 6 or 7, across the replication round trip, is the request
sitting in its queue instead. Short, non-object work — `/status`,
`/map`, `/ctl` reads — is answered on the service loop itself and
never queued.

The pool size is the one trade: a read of object *A* waits behind a
write to object *B* that hashes to the same queue. At 64 queues and
the write path's ≤ 3·`replms` bound that is rare and bounded, and
`/status` reports the queues' depth so it is visible rather than
folklore. Bucket collisions are why the number is a policy tunable.

**`Tflush`.** `Srv.flush` calls `reqqueueflush`. A request still
queued is removed and answered `Rflush` without ever having run. A
request already running is marked abandoned; the worker notices at
its next check point and performs the whole of step 7 (§3.3), then
answers `Rflush` with no `Rwrite` and no `Rerror`. A worker already
inside the log writer's batch cannot be pulled out of it: it
completes the commit, then still performs the invalidate half of step
7, and answers `Rflush` without answering the write — the "MAY have
been applied" case layer-a §5.4.1 contemplates.

**Group commit.** Three kinds of proc, and the split matters:

1. **The tail assigner.** One `QLock` held for microseconds. A worker
   reaching step 6 appends its entries to the pending queue and
   sleeps on a `Rendez`. The assigner takes as much of the queue as
   fits one `Wunit` of record body as a **batch**, stamps it with the
   next `seq` and the next log offset, and hands it to a free writer
   proc. A single commit whose record exceeds `Wunit` forms a batch
   of one and is written in `ceil(nsec*secsz/Wunit)` pieces; the
   batch rule is about latency, not about correctness, and §3.2's
   argument is indifferent to how many requests a record takes.
2. **A pool of `logdepth` writer procs** (default 4, maximum 8). Each
   writes its batch's body, asks the flusher for a flush, writes its
   header sector, asks for another flush. They run concurrently:
   that is what puts `logdepth` writes in flight, and it is the whole
   reason the design can claim more than the 114 durable writes/s a
   single serial writer gets. The measured 409/s came from eight
   independent writers; one proc executing a serial loop is depth 1
   and gets 114/s no matter how cleverly the queue is drained.
3. **The flusher** (§3.2), which owns the raw channel and coalesces:
   `logdepth` writers asking for a flush at the same instant cost one
   device flush, not `logdepth`.

**The durable watermark.** A batch's waiters are woken only when that
batch's post-flush has returned **and every lower-numbered batch's
has**. Without it a crash after batch *n+1* landed and batch *n* did
not would leave replay stopping at *n* and discarding *n+1* — an
acked write lost. With it, *n+1* was never acked. Batch membership is
fixed by the assigner before any of its writes are issued, so no
member's grain writes escape the pre-flush.

There is no timer and no artificial delay, so **a lone writer pays
exactly one flush, one write and one flush** — the queue is empty
when it arrives. Batching happens only among commits that coincide
with a write already in flight, which is precisely the coincidence
worth exploiting.

**The I/O pool.** Grain writes for one operation, and the pieces of a
record body larger than `Wunit`, are issued by a bounded pool of I/O
procs (policy; default 8) sharing the partition fd — `pwrite` carries
its own offset, so `rfork(RFPROC|RFMEM)` procs sharing one fd are
safe. The pool exists because the sustained-throughput figures in §11
are measured with several requests in flight; without it every one of
them is a single-writer figure.

Throughput follows: one batch holds a hundred-odd small commits in one
`Wunit` of body, so the commit path's ceiling is thousands of commits
per second and the binding constraint is the data writes, not the
log.

## 8. Verify, scrub and repair

*Policy; layer-a §7.5's outcomes are normative there.*

**Verify one object.** Read each block, hash it, compare to `dig[i]`;
then hash the digest array and compare to `csum`. Returns the *set of
mismatching block indices*, not a boolean, because that set is what
makes partial repair possible. A hole is verified against the zero
digest without reading anything. Cost at `objmax`: 16 MiB of reads
(~200 ms) and 16 MiB of hashing (~290 ms), no writes — half a second,
and **hash-bound rather than read-bound**, which is what bounds the
`verify` ctl verb and `op=verify` on `/rpc`.

**Two kinds of mismatch, and they need different repairs.** Verify
answers three states, not two, and the material to tell them apart is
already there:

- `hash(dig[]) == csum` and some block's bytes disagree with its
  `dig[i]`: **the content is suspect**. Repair block by block —
  `op=get` the range `[i*blksz, min((i+1)*blksz, len))` from a holder
  of a copy with the same key, hash it, accept it only if it matches
  the *stored* `dig[i]`, then allocate a fresh grain, write it, and
  commit an `Eobj` whose four-tuple is **unchanged** and whose `nmap`
  names the one block.
- `hash(dig[]) != csum`: **the digest array is suspect**, so the
  stored `dig[i]` cannot be the acceptance test for anything. A media
  fault in an extent-map entry is exactly this. Block repair here
  would reject every correct byte a peer sent and leave the object
  `object lost` forever with good copies all over the cluster. The
  repair is layer-a §1.3's whole-object `op=full force=1` at an equal
  key from a holder whose copy verifies — the same first-class
  key-preserving path, applied to the whole object rather than one
  block.
- Both consistent: the object verifies.

**Scrub.** A background proc walks slots in order, verifies each, and
rate-limits itself to the configured KiB/s so a full pass takes about
`scrubdays`. At layer-a §7.5's ~4 MiB/s on a 4 TB disk that is ~7% of
one CPU spent hashing, continuously, which is worth knowing on a
two-vCPU node that also runs the write path. On mismatch it sets the
index entry's `corrupt` flag — durably, via an `Eobj` that changes
nothing else, so a restart does not forget — lists the object in
`/lost`, and fails client access with `checksum mismatch`. A corrupt
copy loses arbitration against everything including absence (layer-a
§1.3), which the server enforces by refusing to advertise it.

**What a corrupt object answers to `op=meta`.** Neither available
answer is right: reporting the key claims an arbitration position
layer-a §1.3 forbids a failing copy, and `absent=1` is a lie that
layer-a §1.5 counts as a positive confirmation for tombstone discard.
Until the wire has a third answer, this store fails the request with
`checksum mismatch` — a defined error string (layer-a §2.6), though
layer-a §5.6's table does not list it for `op=meta`. §14(11) records
the deviation and proposes the grammar that would remove it.

When every block verifies again, the `corrupt` flag is cleared by
another key-preserving `Eobj` and the object leaves `/lost`. When no
peer has a good copy, it stays there and reads fail `object lost`.

A commit that does not advance the key is a first-class case in this
store, and there are three of them: block repair, whole-object
`op=full force=1`, and the `corrupt` flag itself. All go through the
same `Eobj` path; nothing in the format assumes a commit bumps a
version.

## 9. In-memory index and directory snapshots

*Policy.*

The in-memory index is an array of `nslots` entries plus an oid arena
plus a hash table. Per entry:

    qidpath 8, len 8, ver 8, wepoch 8, mtime 8, csum 32,
    cur 4, oidoff 4, emapslot 4, grain0 4, dig0 16,
    oidlen 1, state 1, flags 1, hashnext 4

which is 128 bytes rounded. There is no lock in the entry: §7's
queues serialise per object, so the 32 bytes an `RWLock` costs on
amd64 — on every slot, occupied or not — are not spent. The oid arena
averages ~24 bytes an object; the hash table is 2^19 `u32` buckets
with chaining through `hashnext`.

| objects | index | arena | buckets | total |
|---|---|---|---|---|
| 2.6·10^5 | 33 MB | 6 MB | 2 MB | **41 MB** |
| 2^20 | 128 MB* | 24 MB | 8 MB | **160 MB** |

(*the array is `nslots` entries whether or not they are occupied.)

Keeping `grain0`/`dig0` in the entry is what makes a one-block object
readable and writable with no map read at all, which at Layer C's
object sizes is most of them.

The extent maps of multi-block objects are deliberately **not** in
memory — 21 KiB × 2.6·10^5 is 5.5 GB — so the write path reads one
41-sector extent map per multi-block object touched, backed by an LRU
of a few thousand entries (4096 entries is 86 MB) which makes
repeated writes to one object free. One 64 KiB-request read is ~300 µs
against an 8.75 ms commit, 3% overhead; §16a asks for it to be
measured under the striping workload.

**Snapshot-at-open (R12).** Layer-a §2.2 makes snapshot-at-open a
MUST for `/status`, `/map`, `/dirty`, `/stale`, `/lost` and `/jobs`
and a SHOULD for `/obj` and `/tombs`. The six MUSTs are small — a
few hundred lines at the envelope — and are rendered into a buffer
at open, which is the one-line implementation. For `/obj`, `/tombs`
and `/advert` the store takes, under the index lock, a vector of
`{u32 slot, u64 qidpath}` — 12 bytes an entry, **3.1 MB at 2.6·10^5
objects and 12 MB at `nslots = 2^20`**. Each `Tread` renders entries
from the *live* index, skipping any whose `qidpath` no longer matches
the snapshot: that is an object deleted since the open, which a
listing of live objects should not show anyway. Nothing shifts under
the reader, so no entry is skipped or duplicated because of an index
shift.

The store reports `objsnap=full` and never uses layer-a §2.2's
`objsnap=partial` escape. The cost is per open fid, so the server
bounds the number of concurrently open enumeration fids (policy,
default 8) and answers further opens `disk full` rather than growing
without limit; at 2^20 slots eight of them are 96 MB, which is the
number §14(9) says is answered for the Layer B envelope and not for
this design's own maximum.

## 10. The monitor's map slot store

*Format.*

The monitor's requirement (layer-a §8.2) is one sentence: the map text
including its `stale` records must be durable against power loss
before the acknowledgement that publishes it. It is small, it changes
rarely, and one of those acknowledgements sits inside a client write
bounded by `replms` (layer-a §5.4 step 5a). It gets the same
treatment in its simplest possible form.

A partition of a few MiB, formatted by `shoalmonfmt`:

    sector 0        header, copy 0
    hdr.curoff      current-map slot 0
                    current-map slot 1
    hdr.histoff     retain history slots, a ring
    last sector     header, copy 1

Header, one sector, **written only at format**:

    off  size  field
      0     8  magic  "shoalmon"
      8     4  vers
     12     4  pad
     16    16  csum128, this field zeroed
     32     4  slotsz    bytes per slot, default 65536
     36     4  retain    history slots
     40     8  curoff    sector of slot 0
     48     8  histoff   sector of history slot 0
     56   ...  reserved, zero

The header carries no `monid`. The cluster identity is an attribute
of the map text (layer-a §3.2), so layer-a §8.3's
`forceepoch <e> monid=<hex>` — the one documented escape for
re-identifying a cluster — is an ordinary map commit here and needs
no way to rewrite a header. That is deliberate: a header rewrite has
no safe torn-write story, because a torn header makes every slot
unlocatable and the whole store unreadable. Two copies, at the first
and last sector, cover the media-fault case; nothing ever writes
either after format.

Slot, `slotsz` bytes, first sector the header:

    off  size  field
      0     8  magic  "shoalmap"
      8     4  vers
     12     4  len    map text bytes
     16    16  csum128 over secsz+len bytes, this field zeroed
     32     8  seq    u64, strictly increasing
     40     8  epoch  the map's epoch, for the operator's benefit
     48   ...  reserved to secsz, then len bytes of map text

The checksum covers the header sector plus exactly `len` bytes of map
text. Bytes left beyond `len` by a previously longer map fall outside
the checksum and are ignored. The **write** is rounded up to
`roundup(secsz+len, secsz)` and zero-padded, because `devsd` turns a
write whose byte count is not a sector multiple into a
read-modify-write: it pre-reads every sector of the request before
writing them back, so the unrounded write costs a hidden device read
and rewrites the tail sector with bytes the store never chose. One
map commit is then one device request for any map under 16 KiB.

**Commit.** In this order:

1. Write the history ring slot for the new epoch, and flush. The ring
   is not optional and this is not last: the slot the ring is writing
   is the *newest* entry, and at the next publish that entry is the
   `E−1` layer-a §8.2 requires the monitor to keep and layer-a §5.2
   clause 2 depends on. Writing it first means a torn ring write
   damages only the map being published, which fails the commit,
   rather than the previous map, which nothing else can supply. If it
   cannot be written, the commit fails.
2. Write the current-map slot, choosing by the same three-clause rule
   as §2.2: if exactly one slot is valid, write the invalid one; if
   both are valid, write the one with the lower `seq`; if neither is
   valid, refuse. `seq` is `max(valid seq) + 1`. Flush.

**Choose on start:** read both, take the valid one with the greater
`seq`. A torn write to the slot being written fails its checksum and
the other slot is untouched, so the previous map survives; that is
the entire crash argument, and it is §2.2's rule again — including
the clause that keeps a torn slot from steering the next write onto
the only good one.

**A map that does not fit.** If `secsz + len` exceeds `slotsz` the
commit fails with `disk full` (layer-a §2.6: any operation that must
store bytes may return it), and the monitor reports the sizes in
`/status`. It is not `bad map`: the text is valid, the partition is
too small, and telling an operator the map is malformed would send
them to the wrong place.

**Sizing.** A map at twelve instances is a few KiB; `slotsz` 65536 is
a twentyfold margin and a whole number of 16 KiB units. With
`retain=8` the store needs 2 header sectors + 10 slots ≈ 640 KiB;
`shoalmonfmt` defaults the partition to 4 MiB and refuses less than
1 MiB.

**Cost.** One 16 KiB write plus one flush per slot written: **8.6 ms**
for the current-map slot, and the same again for the history slot
that precedes it, so a publish is ~17 ms whether it carries a
placement change or a single `stale` mark. That is what makes
layer-a §5.4 step 5a affordable — the alternative
`docs/platform/9front-storage.md` measured, a file plus a gefs sync,
costs 530–620 ms and would blow `replms` regularly.

## 11. Cost model

*Policy.* Device figures from `docs/platform/9front-storage.md`:
8.4 ms per write of ≤ 16 KiB, 175 µs per flush, 114 durable writes/s
at one writer and 409/s at eight. Read and hash figures measured on
the same class of machine: 80 MB/s sequential at 64 KiB requests,
65 MB/s at 16 KiB, and BLAKE2s at 50–59 MB/s (4.86 µs per 256-byte
index entry, 265 µs per 16 KiB block).

Per commit, unconditionally: **one flush + one write + one flush =
8.75 ms**, shared across everything in the batch. A record whose body
exceeds one sector adds one `Wunit` write per 16 KiB of body; a
commit that wraps the log adds one 512-byte write and one flush.

| Operation | Reads | Data writes | Commit | Total |
|---|---|---|---|---|
| create, delete, truncate to a block boundary | map: 0 inline, 0.3 ms out-of-line, 0 cached | none | 1 | **~9 ms** |
| 16 KiB write, block-aligned | 1 map | 1 grain: 8.4 ms | 1 | **~17.5 ms** |
| 4 KiB write into a populated block | 1 map + 1 grain (0.24 ms) + 0.27 ms hash | 1 grain | 1 | **~18 ms** |
| 4 KiB write into a hole | 1 map | 1 grain | 1 | **~17.5 ms** |
| 16 MiB `op=full` | 1 map | 1024 grains | 1 (3 writes) | **8.6 s serial, ~2.5 s at 8-way (409/s)** |
| scrub-verify one 16 MiB object | 16 MiB, 200 ms | none | none | **~0.5 s, hash-bound** |
| incremental checkpoint | none | dirty pages only: 8.4 ms per index or bitmap page, 16.8 ms per extent-map entry | 1 superblock | proportional to dirty state |

Three things this table says that are worth saying in words.

**A 4 KiB client write costs a 16 KiB grain write.** Copy-on-write at
`blksz` granularity is what buys the atomicity of §3, and it makes a
small write cost four times its size. Setting `blksz` = `Wunit` = the
grain is what keeps that factor at four rather than sixteen, and it
makes every block write exactly one device request, so there is no
"issue the grain four ways" question to answer and no proc pool to
size for it. The workload this store is built for — Layer B striping
through an `msize`-sized 9P path — writes whole blocks.

**A 16 MiB `op=full` is disk-bound, not wire-bound.** Layer-a §5.5
puts it at 185–545 ms of wire time; the receiving disk costs 8.6 s of
grain writes serially, ~2.5 s if they are issued eight ways, plus
290 ms of hashing. The conclusion layer-a draws from that arithmetic
— never pull inside a client request (layer-a §5.2) — is reinforced,
not weakened. The eight-way figure is read off a table of
*independent* writers and is the one §16a still asks to confirm,
which is why §14(4) puts the disk term into layer-a qualitatively and
leaves the number here.

**The checkpoint is a second write of the metadata, and it is
per object, not per commit.** Between two checkpoints an object that
was touched costs one 16 KiB index page, at most one extent-map
entry, and at most one bitmap page, however many commits touched it.
A workload that returns to the same objects pays that once per
interval; one that touches a fresh object with every commit pays up
to three device writes per commit on top of the commit's own, which
is the shape §6's wait bound is written against.

**Group commit removes the commit from the throughput equation.** At
roughly a hundred small commits per `Wunit` of record body and four
batches in flight, the commit path sustains thousands of commits per
second. What it cannot remove is the data writes: sustained durable
throughput on the reference device extrapolates to 409 × 16 KiB ≈
**6.7 MB/s** — the 409/s was measured at 4 KiB, and a 16 KiB write
costs the same 8.4 ms, so the extrapolation is the one §16a asks to
confirm. Everything above follows from it. On hardware where a
durable write costs 100 µs rather than 8.4 ms every row of the table
scales by the same factor — the shape of the design does not change,
only the constant, and the hashing terms stop being negligible.

## 12. Tooling

*Policy.* Three new commands under `cmd/`, each an `mkone`
directory listed in `cmd/mkfile`'s `DIRS`.

**`shoalfmt`** — format or ream an object-store partition.

    shoalfmt [-r] [-b blksz] [-o objmax] [-n nslots] [-e nemap]
             [-d ndirty] [-L logbytes] [-u uuid] /dev/sdXX/name

Writes both superblocks, zeroes the log, index, extent-map, dirty and
bitmap regions, sets bit 0 of the bitmap (grain 0 is never
allocatable), and prints the geometry it chose — including how many
multi-block objects `nemap` supports, which is the number an operator
needs to size a workload that is not Layer B's. It generates a random
`uuid` unless given one, and **refuses a partition that already
carries a valid superblock unless `-r`** — reaming a disk destroys an
instance's identity, and layer-a §1.5 makes that a
reformat-before-rejoin event, so it should take a flag. It refuses a
geometry whose maximal `Eobj` record exceeds an eighth of the log
region, and warns when `nslots` implies more than 1% of the partition
in metadata.

**`shoalck`** — inspect and check.

    shoalck [-v] [-l] [-R] [-o oid] /dev/sdXX/name

Default: print both superblocks, the geometry, log head/tail and
sequence range, slot, extent-map-slot and grain occupancy, and the
dirty-record count; verify every index entry's checksum and every
bitmap page's; cross-check the bitmap against the grains every live
map references, scanning each object to `nblk` and not beyond; exit
non-zero on any inconsistency. `-l` dumps the log records. `-o` dumps
one object's index entry and extent map. `-v` verifies every object's
content against its digests — an offline scrub. `-R` rebuilds the
free-grain bitmap from the live maps and rewrites the checkpoint,
which is the offline form of §5 step 9's automatic rebuild.

**`shoalmonfmt`** — format a monitor map partition.

    shoalmonfmt [-r] [-s slotsz] [-R retain] /dev/sdXX/name

**Carving the partitions** is the operator's step and uses stock
tools. On a whole disk, `disk/fdisk -aw /dev/sdXX/data` creates a
`plan9` partition in the largest free area, doing nothing if one
already exists — `-p` only *prints* the ctl commands and exits, so it
creates nothing. Then a named sub-partition inside it: `disk/prep`'s
`-a` flag only accepts names from its own table (`other`, `swap`,
`fscache`, …), which does not include `shoal`, so an arbitrary name
comes from the editor's `a` command on standard input:

    echo 'a shoal
    
    
    w
    q' | disk/prep -w /dev/sdXX/plan9

(the two blank lines take the default start and end). `-w` writes the
partition table to the disk, which is what makes the name reappear
after a reboot: the kernel creates only `data` plus whatever a
`sdXXpart=` plan9.ini variable supplies, and it is `/rc/bin/diskparts`
— run from `termrc` and `cpurc` — that replays the on-disk table into
`/dev/sdXX/ctl` at every boot. Without `-w` the partition must be
re-declared by hand at every boot, which is a good way to start a
store on the wrong bytes. Note that re-running `diskparts` against a
unit with a store already serving it invalidates that store's open
fids (§0, `Echange`).

`cat /dev/sdctl` names the driver claiming each unit, which §3.2's
flush caveat depends on: `virtio` and `ahci` issue a real flush,
`ata` (the legacy IDE driver) fakes it silently.

The servers themselves — the object server and the monitor — are
layer-a's subject, not this document's; where they need names,
`cmd/shoalsrv` and `cmd/shoalmon`.

## 13. Test plan

*This section is a plan.* It has two tiers, and the split is not
about convenience: **the mechanism this design most depends on — the
placement of the two flushes — cannot be discriminated on the
reference hardware at all.** The `-X` hook below kills a process, and
every byte the store wrote has already returned from `pwrite` into a
kernel that does no caching, so removing a flush changes nothing a
process crash can see. On power-off the reference device behaved
write-through — 7519 of 7519 acked writes survived with no flush at
all — so removing a flush changes nothing there either. A test plan
that lives only on that hardware would ship the flushes untested by
`AGENTS.md`'s own both-ways rule.

So the flushes, and every crash schedule, are discriminated against a
**simulated disk** in T1, and the real device is where the platform
assumptions themselves are checked.

**T1 — the simulated disk.** §0 puts every device access behind one
small interface. The T1 implementation is an in-memory disk that
models what the real one is allowed to do:

- a **volatile write cache**: a written sector is durable only after
  a flush; a simulated crash discards every sector written since the
  last flush, chosen adversarially rather than at random;
- **torn and partial writes**: a write may land as any byte-wise
  mixture of old and new bytes within any sector it covers, and may
  land in any subset of its sectors;
- **short counts** on every read and write;
- `Echange` and `Eio` on demand;
- **crash at a chosen point**, by name, with the flush and write
  sequence recorded so a test can assert the *order* as well as the
  outcome.

Points: `stage` (after the last staged grain write), `precommit`
(after the pre-flush, before the body write), `body:n` (after *n*
body sectors), `commit:n` (after *n* header bytes — the torn-header
sweep), `postwrite` (after the header write returns, before the
post-flush), `preack`, `ckpt:n` (after *n* checkpoint page writes),
`reclaim` (reclaim log space before the checkpoint's superblock
write), `super` (between the two superblock copies), `batch:n` (hold
batch *n*'s write and let *n+1* complete). Each T1 test names the
requirement it discriminates and the mutation that must break it;
each mutation is run.

Each test names the requirement it discriminates and the mutation
that must break it; **each mutation is run**, per `AGENTS.md`.

- **T1.1 crash matrix (R1–R4).** Every point above × {create,
  whole-block write, partial write, truncate, delete, 16 MiB
  `op=full`}: record the four-tuple and content before, crash,
  restart, and assert `/meta` reports exactly the old or exactly the
  new four-tuple, `verify` passes, and a full re-read matches the
  reported `csum`. *Mutations:* skip the pre-flush; skip the
  post-flush; ack before the post-flush returns; write the header
  before the body; accept a record without checking its checksum;
  split the four-tuple and the block map into two records.
- **T1.2 torn header sweep (R2).** `commit:n` over the whole header
  sector, with byte-wise mixtures. *Mutations:* trust `nsec` without
  bounds-checking it; accept a record whose `seq` is not the expected
  successor.
- **T1.3 short I/O (R1, R2).** The simulated disk returns a short
  count on every call. *Mutation:* assume `pwrite` completes fully.
- **T1.4 replay (R2).** A record from an earlier lap at an aligned
  offset; a `Fwrap` continuation; a `ckseq` published past the
  watermark. *Mutations:* seed the sequence expectation from whatever
  is at `cklogoff` instead of `ckseq+1`; continue at `+nsec` after a
  wrap record; let the checkpointer publish a `ckseq` above the
  durable watermark.
- **T1.5 condemn only after replay (R2, R17).** `ckpt:n` with a
  sub-sector tear across an index page must leave **zero** objects in
  `/lost`. *Mutation:* condemn index entries before replay runs.
- **T1.6 truncate then extend (R2, R13).** Truncate across a block
  boundary, force the freed grain's reallocation to another object,
  extend, and read zeros. *Mutation:* apply an `Eobj` without
  clearing map entries at or beyond `nblk`.
- **T1.7 deferred reuse (R2).** A frees grain *g*, B stages into *g*,
  crash at `commit:0` for both; A still reads its old content.
  *Mutation:* return freed grains to the allocator at commit time
  rather than at flush completion.
- **T1.8 group-commit watermark (R1).** Eight concurrent writers,
  crash at `batch:n`; no acked write is missing after restart.
  *Mutation:* wake waiters when their own batch returns.
- **T1.9 two-slot validity (R2).** Tear a superblock copy, restart,
  commit again, tear again: the store still starts. The same for the
  monitor's map slots. *Mutation:* choose the write victim by `gen`
  alone.
- **T1.10 log reclaim (R2).** The `reclaim` point, then a restart
  that lands on the older superblock copy: its records must still be
  in the log. *Mutation:* reclaim before the checkpoint's superblock
  write returns — the one way §2.8 says this format can lose data.
- **T1.11 log wrap (R2).** Drive the log several times around the
  ring, crashing on and around the wrap record. *Mutation:* let a
  record straddle the region end instead of emitting `Fwrap`.
- **T1.12 stage lifetime (R3, R4).** Abandon an `op=full` part way
  through and end it by clunk, by `Tflush` and by timeout in turn;
  the free-grain count returns to its pre-transfer value each time,
  and `stagemax` is enforced. *Mutation:* keep a stage alive past its
  fid.
- **T1.13 flush sequence (R1).** From the recorded device trace,
  assert that a flush precedes the header write and another follows
  it, for every commit shape including a wrapping one and one whose
  body spans several `Wunit`s. *Mutation:* either flush removed, or
  the two reordered. This is the test that makes T1.1's flush
  mutations detectable as a sequence even where they are not
  detectable as a loss.
- **T1.14 exhaustion (R8).** Grains, index slots, extent-map slots
  and the log: `disk full` in the first three, a bounded wait then
  `disk full` in the fourth, with delete and tombstone discard
  working throughout on the reserved tail. *Mutation:* drop the
  reserved log tail.
- **T1.15 enumeration (R12).** 2.6·10^5 entries with concurrent
  creates and deletes across a full read of `/obj`; and a read of
  `/dirty` while the write path mutates it. *Mutation:* render `/obj`
  by index position from the live index.
- **T1.16 digests and holes (R6, R13).** Write at a large offset,
  read the hole as zeros, and check `csum` against `shoalcsum` over
  the same byte image; truncate within a block; partial-write one
  block of a many-block object and assert only that block's digest
  changed. *Mutations:* hash the final partial block over `blksz`
  rather than its actual length; hash a hole as absent rather than as
  the zero bytes it reads as.
- **T1.17 corrupt digest array (§8).** Damage an extent-map entry so
  that `hash(dig[]) != csum`, and assert the repair takes the
  whole-object path. *Mutation:* accept a peer's block against the
  stored `dig[i]` when the array itself fails.

T1 stays diskless and is `mk test` at the repo root, as `AGENTS.md`
requires: the simulated disk is a T1 program's own memory.

**T2 — single-node integration, on a real partition.** T2 runs on one
9front machine with one scratch partition. Each test is a program
plus an `rc` script that formats a partition, runs a server against
it, drives it, and checks. What T2 adds is everything that is a
property of the platform rather than of the store:

- **T2.1 durable-before-ack (R1).** A writer records each acked
  `(oid, ver)` to another machine's disk or a serial line, so the ack
  log survives; the machine is hard-powered-off mid run; on restart
  every acked `(oid, ver)` is present and verifies. This is the test
  `docs/platform/9front-storage.md` ran against a bare partition,
  lifted to the store.
- **T2.2 the crash matrix on real hardware**, a subset of T1.1 at the
  `-X` points, to confirm the simulated disk is not the only thing
  the store is correct against.
- **T2.3 flush observability.** The one configuration in which a
  flush is *observable*: a guest whose backing store is
  `cache=writeback` (so the host page cache is the volatile cache)
  with the host process killed, or a real AHCI unit with its write
  cache enabled and a physical power cut. This is the test that
  prices the flushes; until it is run, the reference platform's
  write-through behaviour is inference and the flushes are insurance
  nobody has costed.
- **T2.4 recovery cost and memory (R17)** at `nslots = 2^20` against
  §5's bound and §9's table, with the hashing term separated from the
  I/O term.
- **T2.5 `Echange`.** Re-run `diskparts` against a unit with a store
  serving it; the store must name the condition and exit non-zero,
  not report media corruption.
- **T2.6 the raw channel.** Two processes issuing raw commands to one
  unit, to demonstrate the interleaving §2.1's deployment rule
  exists to prevent, and to confirm the single-owner discipline
  inside one process.
- **T2.7 monitor slots (§10)** under `-X` at each point, restarting
  and asserting the monitor comes up at the newer map or the older
  one, never a torn one, and that the `E−1` history entry survives a
  torn publish.
- **T2.8 sustained throughput**, to confirm or refute §11's 6.7 MB/s
  extrapolation and the eight-way `op=full` figure.

**The fault-injection hook.** The server takes `-X <point>[,<n>]`,
present in every build and inert without the flag; it is what drives
the same named points on the real device. It is a command-line flag
rather than a `/ctl` verb because layer-a §2.5 fixes the ctl grammar
and requires an unknown verb to fail `unknown ctl`; a debug verb
would be a wire change.

## 14. Deviations from layer-a, and tensions with the platform

*Policy, but read it before implementing anything.*

Fifteen places where layer-a is silent, self-defeating, or
contradicted by the measurements. Items 1–5, 8, 9, 12 and 13 are
amendments made to `docs/design/layer-a.md`; items 6, 7, 10, 11, 14
and 15 are recorded here and not made there — two of them (10 and
11) as proposals, because they touch the wire.

1. **`cur` cannot usefully be durable (layer-a §5.2).** Layer-a §5.2
   required the instance to record currency "durably as `cur=<epoch>`"
   and, two lines later, to invalidate `cur` for every object **on
   process start, after a clean exit as much as after a crash**. The
   only moment a durable value could be read is the moment it must be
   discarded. This store keeps `cur` in memory only; layer-a §5.2 no
   longer says "durably", and keeps both invalidation MUSTs and
   `cur=` in `/meta` as an in-memory value.

2. **Step 5b's dirty record is committed *with* the update, not
   before it (layer-a §5.4).** This store puts the `Edirty` entries
   in the same log record as the `Eobj`, so either both are durable
   or neither is. That is **not** strictly stronger, which is the
   easy mistake: layer-a's stated order covers one window this order
   does not — a crash after an acker committed and before the
   primary did leaves layer-a's ordering with a durable dirty record
   naming the peer that fell behind, and leaves this one with
   nothing, so the primary could later report `synced` for a subject
   that missed an acked write. It is *equivalent* only because §2.6
   sets `fullsync` for every peer at restart, which covers exactly
   that window. Layer-a §5.4 step 5b now reads "durable no later than
   the update", and the restart obligation is written into layer-a
   §7.1 so that a different conforming implementation cannot take the
   licence without the rescue.

3. **Layer-a assigned no home to state the store must hold (layer-a
   §2.3, §6.3, §8.6).** Three per-instance facts must survive a
   restart: the pinned `monid`, whose whole purpose as a tripwire
   evaporates if a restart forgets it; the highest adopted epoch,
   which layer-a §8.6 step (b) has the rebuilding monitor read out of
   `/status`; and layer-a §2.3's monotonic `qid.path` counter. All
   three are in the superblock (§2.2), and layer-a now states the two
   ordering rules that make them mean anything: the epoch durable
   before the instance acts under it, and the `qid.path` high-water
   durable before any value in its batch is issued. Layer-a §2.3 also
   states that the counter is monotonic *and* that discarding a
   tombstone frees the path for reallocation — those only compose if
   the counter is a stored high-water rather than a maximum over live
   records, which is what §2.2 does.

4. **Layer-a §5.5's resync arithmetic omits the disk (§11).** 185–545
   ms is wire time; the receiving disk costs seconds. Layer-a §5.5
   now says so qualitatively and points here. It does **not** carry
   this document's 2.5 s figure, because that figure rests on
   eight-way concurrency read off a table of independent writers
   (§16a): a number in the ratified contract should be one somebody
   measured for the case it describes.

5. **Layer-a §5.4's "plus local I/O" is the dominant term, not a
   rounding error.** The honest sum for one client write is
   3·`replms` plus 9–18 ms of local I/O in the ordinary case, plus
   whatever the object's queue holds — and, in the rare case where
   the log is full, plus a wait bounded by `ckwaitms` (§6) rather
   than by `replms`. At the default `replms` of 1 s the ordinary case
   is invisible; at a `replms` tuned down toward the sub-millisecond
   LAN it is the whole cost. Layer-a §5.4 now names the checkpoint
   wait as part of that term.

6. **`disk full` stays definitive, and no retryable error is added.**
   A commit that finds no log space waits and then answers
   `disk full` (§6). Adding a retryable `busy` to layer-a §2.6 would
   be a wire change made to paper over a store that cannot bound its
   own checkpoint; the cause is fixed instead — incremental
   checkpointing (§2.8), a reserved log tail so space-freeing commits
   never block (§6), and a wait bound derived from the real
   checkpoint cost. What is left is genuine exhaustion, for which a
   definitive answer is the correct one.

7. **The flush channel is not exclusive, and the store requires it
   anyway.** `/dev/sdXX/raw` admits any number of openers; the
   kernel's interlock is unreachable and the cdb→data→status state is
   per unit, so concurrent users corrupt each other's commands rather
   than being excluded (`docs/platform/9front-storage.md` §5). Two
   consequences, both in §2.1 and §3.2: the deployment rule is that
   one process per `sd` unit issues raw commands, and inside the
   process one flusher proc owns the channel. The store does not
   start without the channel unless the operator asserts
   write-through with `-w`, because the property being given up is
   layer-a §1.3's normative four-tuple atomicity, not a nicety, and
   the platform's write-through behaviour is inference rather than
   confirmation. D4 says "one instance per disk" and layer-a never
   mentions the unit; that gap is why the rule is written down here.

8. **Formatting precedes the map that defines the format (layer-a
   §3.4).** Step 1 there has the operator start a server on an
   unformatted disk, which generates a uuid and writes a superblock —
   before it registers, so before it has a map, so before it knows
   `blksz` and `objmax`, which this store's geometry depends on.
   Resolution: `shoalfmt` takes them from the operator, defaulting to
   the map defaults, and the server MUST refuse to serve if the
   adopted map's `blksz`, `objmax` **or `csumalg`** differs from what
   the disk was formatted with — `csumalg` because layer-a §8.5 makes
   it immutable for exactly this reason and a mismatch invalidates
   every stored digest. Layer-a §3.4 now says the format step needs
   them.

9. **Layer-a §10.2's evidence items: two answered, one settled, one
   not.** Answered: the local layout question and the monitor's
   durability question are this document. Settled by measurement:
   `blksz` sits at `Wunit`, which makes a block one device request
   and a partial write cost four times its size rather than sixteen —
   layer-a §10.2 records that item as settled and layer-a §1.4's
   default is 16384. Not settled: whether a full `/obj` snapshot is
   affordable. It costs 3.1 MB at 2.6·10^5 objects and 12 MB per open
   fid at the `nslots = 2^20` this document itself recommends for a
   4 TB disk, so `objsnap=partial` is unnecessary at the Layer B
   envelope and not demonstrated unnecessary at this design's own
   maximum. §9 bounds the number of open enumeration fids for that
   reason.

10. **A `Tflush` after the commit still owes the cleanup half of step
    7.** Layer-a §5.4.1 requires a flushed request to perform "the
    whole of step 7", and its "the replication attempt MAY complete"
    clause is about the attempt in flight, not about the local
    commit. Once the commit is durable the discard half is vacuous
    but the invalidate half is not: the primary holds `(E, ver+1)`
    with sync state and a `cur` derived from an operation whose
    outcome the client never learned. §3.3 and §7 do the whole of it.
    Layer-a §5.4.1 would read better if it split the obligation
    explicitly into a discard half (pre-commit only) and an
    invalidate half (always); that is a clarification of an existing
    MUST rather than a new rule, and is proposed rather than made.

11. **`op=meta` has no way to say "I hold a copy that fails
    verification".** A corrupt holder must either claim a key that
    layer-a §1.3 forbids a failing copy from claiming, or answer
    `absent=1`, which layer-a §1.5 counts as a positive confirmation
    licensing a tombstone discard it cannot vouch for. This is a wire
    change, so it is proposed here and not made. The exact grammar
    proposed, as a third form of the `meta` response in layer-a §5.6:

        meta oid=<oid> corrupt=1

    with these rules: an instance whose copy of `<oid>` fails local
    verification (layer-a §7.5) MUST answer `corrupt=1` and MUST NOT
    answer with a key or with `absent=1`; a `corrupt=1` response is
    neither a key nor an absence, so it MUST NOT be used in
    arbitration, MUST NOT count as either kind of layer-a §1.5
    confirmation — so it blocks a tombstone discard exactly as an
    unreachable instance does — and MUST NOT satisfy a layer-a §5.2
    currency check; and it licenses the serving primary to push
    `op=full force=1` at an equal key to the reporting holder, which
    is layer-a §1.3's repair path and the only way to repair a holder
    whose key already equals the sender's. A response form is
    proposed rather than an error because `op=meta` is answered
    whatever the instance's `up`/`status` (layer-a §6.4 F3), and a
    caller must be able to tell a corrupt holder from an unreachable
    one. Until it is ratified, §8 says what this store answers.

12. **A tombstone's cost.** Layer-a §1.5 said a tombstone "occupies a
    metadata record and nothing else". That is true here — 256 bytes,
    because a tombstone has no blocks and so holds no extent-map slot
    (§2.3) — but it is true by design rather than by nature, and a
    store that reserved a fully-sized block map per object would pay
    21 KiB. Layer-a §1.5 now says the cost is implementation-
    dependent and may include a reserved per-object metadata extent.

13. **Layer-a §5.5's multi-request `op=full` stage had no lifetime.**
    The receiver stages chunks across many `Twrite`s and commits at
    `final=1`; nothing said when an abandoned stage is released, so a
    sender that dies mid-transfer left the receiver holding staged
    space forever and a heal of a whole disk could answer `disk full`
    with the disk nearly empty. §3.6 gives the stage an owner, a
    lifetime and a bound, and layer-a §5.5 now requires one.

14. **`nemap` is the sizing a workload can defeat.** Splitting the
    extent-map slot space from the index slot space (§2.1) is what
    keeps the extent-map region at 5.1 GiB while quadrupling block
    resolution, and the inline one-block map is what keeps small
    objects out of it entirely. What remains is that an object of two
    to `nblkmax` blocks consumes a full `emapsz` slot, so a disk
    filled with 32 KiB objects exhausts `nemap` with the data region
    almost empty. Mitigations in place: `nemap` is a format
    parameter, `shoalfmt` prints what it supports, `/status` reports
    `emapfree=`, and the exhaustion is a distinct, named `disk full`.
    The recorded fix, if a real workload hits it, is a size-classed or
    grain-backed extent map (§15) — a format change, so a `vers` bump
    and a reformat.

15. **Two defensible answers, and the ones taken.** Three questions
    in this design had a good argument on each side; the choices are
    recorded so they are not re-opened by accident.
    - *The bitmap.* Incremental per-page checkpointing keeps the
      bitmap cheap; dropping the persistent bitmap entirely and
      rebuilding at every start makes the format smaller but every
      start slow. Taken: both halves of the cheap answer — one
      paged, incrementally checkpointed copy (§2.5), rebuilt
      automatically when a page fails, with `shoalck -R` kept for the
      offline case. A second copy was dropped because it is not
      redundancy: the older copy belongs to a checkpoint whose log
      may already be reclaimed.
    - *The flush channel.* Since the raw file is not exclusive, the
      "another process holds it" degraded start had no referent at
      all; but a genuine open failure can still happen. Taken:
      refuse, with an explicit operator assertion (`-w`) as the only
      way past it, rather than a permissive default (§3.2).
    - *The entry length field.* A `u16` length with `shoalfmt`
      refusing configurations it cannot encode would have been
      smaller. Taken: `u32`, because the bound a `u16` imposes
      (~2300 blocks per object) is invisible at the point where an
      operator chooses `blksz`, and this format has already spent
      more than two bytes on being explicit.

## 15. Alternatives considered

*Policy.*

- **Log-structured whole store** — every write appends to a segmented
  log, an index maps object blocks to log positions, a cleaner
  reclaims segments. Attractive here: every write is sequential, and
  a 4 KiB write costs 4 KiB rather than a grain. Rejected: the
  cleaner is the hard part, its write amplification goes non-linear
  near full, and it must be crash-safe in its own right — a second
  recovery story on top of the one we need anyway. The measured
  device also shows random and sequential writes at identical cost,
  so the sequential-append premise buys nothing on this platform.
  In-place plus a write-ahead log is a construction a 9front reviewer
  has seen; a cleaner is one they will have to audit.

- **File per object on gefs** — the fallback. gefs is the one 9front
  file system that survives a crash intact, its copy-on-write commits
  give a sidecar pair four-tuple atomicity for free, and a null
  `Twstat` is a real fsync. It costs nothing to implement. Rejected
  as the primary: 530–620 ms per durable write, 1.9–7.7 durable
  writes/s, sixty times `replms`. It stays recorded as the answer if
  the raw store slips, at a throughput the target would have to be
  renegotiated around.

- **File per object on cwfs or hjfs** — rejected outright. Neither
  offers a per-file flush, both lose acked writes, and both are left
  structurally damaged by a hard stop. The crash that damaged cwfs
  produced precisely the failure layer-a §1.3 forbids: a key file
  present at the right size reading as zeros beside a correct content
  file.

- **Packed or extent allocation** — variable-length extents, best
  fit, coalescing on free. Less internal fragmentation for small
  objects, contiguity for large reads. Rejected: it brings external
  fragmentation and a real allocator, object growth needs relocation,
  and the block map stops being parallel to the digest array, so two
  structures replace one. Fixed grains at `blksz` make allocation a
  bit test and make one `u32` locate both the bytes and the digest.

- **A grain smaller than `blksz`** — `grainsz` = 4 KiB with
  `blksz` = 4·`grainsz`, so a partial write rewrites 4 KiB rather
  than 16 KiB. Not taken: it quadruples the block map, splits one
  array into two of different lengths, adds a parameter, and — the
  decisive point on this platform — buys nothing in *time*, because a
  4 KiB write and a 16 KiB write both cost 8.4 ms. It would only pay
  on hardware where a write's cost tracks its size.

- **A larger `blksz` than `Wunit`** — the previous default of 64 KiB
  made a block four device requests and a 4 KiB client write cost a
  64 KiB rewrite, and it made the "one commit record, one device
  request" property hold only for the log. Rejected once `Wunit` was
  measured: there is no property a block larger than one device
  request buys, and the metadata it saves (a quarter of the map and
  digest arrays) is 0.4% of a partition.

- **Preallocating a full `objmax` extent per object** — no block map
  at all, one base grain per object. Rejected: 2.6·10^5 objects ×
  16 MiB is the whole 4 TB disk, with no sparseness and no holes.

- **A size-classed or grain-backed extent map** — extent-map entries
  sized to the object rather than to `objmax`, or held in ordinary
  grains allocated on demand. This is the recorded fix for §14(14)'s
  `nemap` residual, and the reason it is not taken now is that a
  grain-backed map has to be published by the same commit as the
  content it describes, which puts map-grain allocation into the
  `Eobj` and a second level of indirection into replay — real
  complexity against a sizing that the inline one-block map already
  keeps off the common workload.

- **Carrying small write payloads in the log record** — a 4 KiB write
  becomes one commit with the bytes inline, folded into its grain at
  checkpoint. Cuts that case from ~18 ms to ~9 ms. Not taken: the
  read path must then overlay unfolded log data on the grain, and
  `verify` must consult the log to see the content its committed
  digest describes. The digest-matches-content property of §4 stops
  being obvious, which is too high a price for one case.

- **A separate log device** — rejected for v1. The measured device
  shows random and sequential writes at identical cost, so there is
  no seek argument, and a second partition is a second operator step
  and a second thing to lose.

- **CRC-32 for record checksums** — rejected, but the cost basis is
  worth stating honestly: BLAKE2s is ~55 MB/s here, not "microseconds
  for anything", and the one place a CRC would genuinely win is the
  start-up index verify, where 2^20 entries cost 5.1 s of hashing and
  a CRC would cost five to ten times less. It is still rejected: D7
  already puts BLAKE2s in the build, one primitive means one set of
  known-answer vectors, and 5 s of one-time start-up cost does not
  buy a second checksum implementation to test and get wrong.

- **Per-page rather than per-entry index checksums** — cheaper by 6%
  of the index region and coarser by 64×. Rejected: a localised fault
  should cost one object, not a page of them. The bitmap goes the
  other way — per page, not per copy — because there the checksum
  granularity decides how much must be *rewritten* at every
  checkpoint, not how much a fault destroys.

- **Two bitmap copies** — rejected. Alternating writes are what keep
  a torn write from destroying the live copy, and that is a property
  of alternating, not of keeping two readable copies: the older copy
  belongs to a checkpoint whose log space §2.8 may already have let
  the writer reclaim, so it can never be rolled forward. Keeping it
  would cost 32 MiB and an operator procedure for no recoverable
  state.

- **A per-object reader-writer lock** — the obvious way to spell
  layer-a §5.4.1's per-object ordering, held from step 2 to step 6 or
  7 across the replication round trip. It needs a service loop that
  can run handlers concurrently, and `lib9p` has none: `srv` is
  single-threaded and `Srv` has no mode flag, so the lock has to be
  paired with `srvrelease`/`srvacquire` at every blocking point.
  Rejected: `9pqueue`'s `Reqqueue` pool gives the same ordering by
  construction, `reqqueueflush` is the `Tflush` path, and the lock
  would cost 32 bytes in every slot of the in-memory index whether
  occupied or not (§7, §9).

## 16. Open questions

### (a) Settleable by evidence

1. **Sustained durable throughput at 16 KiB**, and how much
   concurrency an `op=full` receiver's grain writes actually get.
   §11's 6.7 MB/s extrapolates a 4 KiB measurement, and its eight-way
   `op=full` column is read off a table of *independent* writers.
   T2.8.
2. **The cost of a partial-block write** against the model, and
   therefore whether the smaller grain in §15 is ever wanted on
   hardware where a write's cost tracks its size.
3. **Recovery time and memory at 2^20 slots** (T2.4), against §5's
   bound, with the hashing term separated from the I/O term — the
   model says they are roughly equal and that is worth checking
   before anyone quotes a restart time.
4. **Whether the extent-map read per write shows up**, and what LRU
   size makes it disappear for the striping workload.
5. **Which driver claims each unit on the fleet.** `cat /dev/sdctl`
   per machine: `virtio` and `ahci` issue a real flush, `ata` fakes
   it silently, and a unit on the last of those needs its write cache
   disabled and `-w` (§3.2).
6. **Log size against checkpoint frequency** — 64 MiB, `ckhigh` at a
   quarter and `ckms` at 30 s are guesses, all tunable without a
   format change. What is wanted is the dirty-state figure that makes
   `ckwaitms` (§6) a real bound rather than a plausible one.
7. **Whether a byte in a sector always holds its old or its new
   value** under a torn write. §3.2 shows the design needs no
   single-sector *atomicity* anywhere — but it does need that
   byte-level property, and the index region is where it is
   load-bearing, because two 256-byte entries share a 512-byte
   sector and each carries its own checksum. §3.4 shows why a
   violation there is contained (replay covers exactly the entries a
   checkpoint can change); a violation of the byte-level property
   itself would not be.
8. **Host-side durability.** `docs/platform/9front-storage.md` tested
   guest power-off, not host power loss, and did not inspect the
   host's cache mode; no virtio flush feature is negotiated, and an
   explicit flush costs 175 µs and changed nothing — consistent with
   a write-through backend, but inference. Until T2.3 runs, nobody
   can say what the two flushes in §3.2 are worth on the deployed
   configuration, only that removing them is unsafe on any
   configuration that does have a volatile cache.
9. **The real object-size mix** Layer C produces, against `nemap`
   (§14(14)). The parameter is cheap to change at format time and
   expensive to change afterwards.
10. **BLAKE2s throughput on the fleet**, to confirm the 50–59 MB/s
    measured here — every hashing term in §5, §8 and §11 scales with
    it.

### (b) Product calls

1. **Metadata budget — the `nslots` and `nemap` defaults.**
   *Recommend* `nslots = min(2^20, 4 × partsize/objmax)` and
   `nemap = partsize/objmax`: 0.13% of a 4 TB disk, four times as
   many index slots as `objmax`-sized objects would need because
   Layer C's MDS objects will be small and numerous, and extent-map
   slots only for objects too big to carry their map inline. The
   alternative is a tighter default plus an operator who has to
   think; the cost of thinking wrongly is a disk that reports
   `disk full` with terabytes free, which is also §14(14)'s residual.
2. **Behaviour when the log is full.** *Recommend* a bounded wait
   then `disk full`, as §6 specifies, with no new wire error. The
   alternatives are failing immediately (turns a checkpoint hiccup
   into a client error) and waiting indefinitely (breaks layer-a
   §5.4's latency bound). A retryable `busy` in layer-a §2.6 was
   considered and rejected: it is a wire change that would let the
   store keep an unbounded checkpoint.
3. **`cur` in memory rather than on disk.** *Recommend* in memory —
   §14(1), now amended in layer-a.
4. **Folding the dirty record into the update's commit.**
   *Recommend* yes, with the restart-`fullsync` obligation that makes
   it equivalent rather than weaker — §14(2), now amended in layer-a.
5. **Refusing to start without a flush channel.** *Recommend* refuse,
   with `-w` as the operator's explicit assertion that the unit is
   write-through or its cache disabled, logged at start and reported
   in `/status`. The alternative — start and report
   `flush=unavailable` — converts a normative durability requirement
   into a property of a device cache nobody has confirmed, and the
   test configuration it was meant to serve is served by the flag.
6. **What a store does when its own disk fails verification at
   start.** *Recommend* what §5 specifies: rebuild the bitmap
   automatically (it is derived state, and a 3 a.m. operator
   procedure for a derivable structure is a bad trade), serve with
   the object in `/lost` on a bad index entry that replay did not
   restore, and refuse to start on two bad superblocks or a bitmap
   page from a future checkpoint. The alternative, starting anyway
   and letting the cluster arbitrate, is tempting because layer-a can
   in fact heal it; it is rejected because a store that starts in a
   mode it did not name is how an operator loses a day.
7. **libthread.** *Recommend* the `Reqqueue` pool (§7), which makes
   the whole server a libthread program using `threadpostmountsrv`.
   The alternative is `srvrelease`/`srvacquire` with explicit
   per-object locks, which is plain libc — at the price of writing
   the per-object ordering, the flush path and the lock's memory in
   by hand. D12 records the choice.
