# The local object store

Status: **design of record for M1** (2026-08-27), implementation
policy under decisions.md D13; the one wire-level item it raises,
§14(11)'s `op=meta corrupt=1` response, is ratified (decisions.md
D14) and part of the layer-a contract. This is the design of
the per-instance local store that sits behind the Layer A 9P export,
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
- **This document is implementation policy in D1's sense**: D1 marks
  "everything about how a node stores its objects locally" as the
  part a conforming reimplementation may do differently. Nothing here
  is on any wire, with one carve-out: *which* of this store's
  refusals answers with a layer-a §2.6 wire error is client-visible
  through the 9P server, so §3.7's mapping rule is **normative** — a
  reimplementation must answer §2.6's string where §3.7's table names
  one, and must keep §2.6's prefixes off everything else. The text of
  every other error, like everything else here, is free to differ.
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
- **`blksz` and `Wunit` are two different things that the cluster
  default makes equal.** `blksz` is layer-a's: a map header
  attribute, a power of two, default 16384 (layer-a §1.4), fixed at
  cluster creation and recorded in this store's superblock. `Wunit`
  is the device's: the largest single write the device layer issues,
  which `sdvirtio`'s 32-sector split and `devsd`'s one request per
  `pwrite` put at 16 KiB on the reference unit
  (`docs/platform/9front-storage.md` §5). At the cluster default the
  two coincide, and that is what the default is for — one constant
  in three roles, a checksum block, an allocation unit and one device
  request, so that every block write, every checkpoint page and every
  log record that fits a unit is one request. That is a property of
  the default, **not a MUST**: this store MUST accept any `blksz`
  layer-a permits, within the format's own bounds (§2.1), and a grain
  larger than `Wunit` is written in `Wunit` pieces rather than
  refused. §15 records what a smaller grain would buy and cost.
- **`Wunit` governs writes only.** The store MUST NOT issue a single
  `pwrite` larger than `Wunit`, because a larger one buys nothing
  and obscures what one device round trip costs; the write wrapper
  below splits a longer one into `Wunit` pieces, so the rule holds
  where every write passes rather than at each call site. Nothing
  rests on how many requests a write takes: a record is valid only if
  its checksum verifies over its whole byte range, so a record that
  landed in part is rejected however many requests carried it, which
  is exactly the argument §3.2 makes for writing the header sector
  last. Reads have the
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
- **`interrupted` is not an I/O error.** `reqqueueflush` interrupts
  the proc running a flushed request — 9pqueue(2) says so, and the
  note it posts aborts whatever system call that proc is in — so any
  device call in the store can return `interrupted`. The wrappers
  MUST distinguish it from a media error: it is neither a short
  count to retry nor damage to report, it means this request has
  been flushed, and it unwinds into §3.3's step-7 exit. Treating it
  as an `Eio` would turn an ordinary client interrupt into §5's
  condemnation paths.
- **`Echange` is recognised specifically.** Every I/O on an open
  partition fid fails that way once anything re-declares the unit's
  partitions — an operator re-running `diskparts` is enough
  (`docs/platform/9front-storage.md` §5). The store MUST distinguish
  it from a media error, MUST NOT treat it as corruption, and MUST
  NOT keep serving on a stale fid, because the alternative is writing
  object data at offsets that now mean something else. The fid is
  what is condemned: every later read, write and flush on it fails
  with that condition, so the batch in flight fails and condemns the
  store through §3.2's `broken`, waking every waiter; every read and
  every checkpoint after it fails; and the condition reaches the
  caller, which is the one that reports it and exits non-zero. The
  device layer does not exit the process itself: §7 makes every
  device call from a proc that holds ordering state — a committer
  inside its batch, the checkpointer — and ending that proc alone
  would leave the batch in flight and every other committer waiting
  on it for ever.
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
  way. Verifying is a *read*: it MUST NOT write to the record, even
  transiently, because §7 has several procs reading one page at once
  and a reader that caught the zeroed field would report a checksum
  failure over a record that is perfectly good.
- **Reserved bytes are written zero and ignored on read.** Every
  header below has them; a reader MUST NOT reject a structure because
  a reserved byte is not zero, because there is no in-place format
  upgrade — a new field takes a `vers` bump, which is a reformat — so
  rejecting buys nothing and two decoders differing about it is a
  bug. The one exception is a *flags* field, whose undefined bits are
  a MUST-be-zero **every decoder checks**, because an unknown flag
  means the structure asks for something this build does not know how
  to do — which is not the same as a field it can ignore. There are
  four: §2.3's `Idxent.flags`, §2.7's `Lrec.flags`, §2.7's entry
  header `flags` (which defines no bit at all, so all eight are
  reserved) and §2.7's `Eobj` `oflags`.
- Every header begins `magic` then `vers`. A store MUST refuse to
  open a structure whose `vers` it does not implement, and MUST say
  so rather than guessing. There is no in-place format upgrade in
  v1: a `vers` bump means reformat and refill from peers.
- **The device is an interface, not a syscall.** Every device access
  in the store goes through one small vtable — read, write, flush,
  and the geometry — with three implementations: the real `sd(3)`
  partition plus its raw channel (§3.2), a simulated disk with a
  volatile write cache used by the T1 tests (§13), and a plain file,
  which is how the tools work on an image (§12). Nothing else in
  the store calls `pread`, `pwrite` or opens `/dev/sdXX/raw`. This
  is what makes the flush placements and the crash schedules of §3.4
  testable at all. A device carries what it can say about durability
  — `raw`, the operator's asserted write-through, none at all, or
  not examined by a reader that opened it read-only — and a tool
  reports that rather than inferring it from whether a flush would
  do anything.

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

The reserved run between copy 0 and the log is alignment, not spare
room: the log is the region every commit writes, so `shoalfmt` rounds
`logoff` up to a `blksz` boundary (32 sectors at the defaults) and
zeroes what is left over. Nothing reads it, and nothing may start
using it without a `vers` bump, since a store built before the change
would not know it was occupied.

Derived quantities, all recorded in the superblock so no reader
recomputes them from assumptions:

    nblkmax = objmax / blksz                    (1024 at the defaults)
    emapsz  = roundup(24 + 20*nblkmax, secsz)   (20992 at the defaults)
    ngrains = datasecs / (blksz / secsz)
    nbmpage = bmapsecs / (blksz / secsz)

`shoalfmt` sizes `bmapsecs` so that the bitmap covers the grains
that are left once it has taken its own pages. That is a fixed point
rather than a formula — a page taken shrinks the data region, which
shrinks `ngrains`, which can shrink the number of pages needed — and
`shoalfmt` MUST take the **smallest** page count that covers itself.
The valid counts are upward closed, since a bigger bitmap leaves
fewer grains to cover, so the smallest is well defined; and because
one more page costs exactly one grain, the smallest lands on
`ceil(ngrains / (8 * (blksz - 48)))` where a fixed point exists there
and **one page above it** where none does, which happens when `n-1`
pages need `n` and `n` pages need `n-1`. Both cases occur. So a
reader MUST
take `nbmpage` from the recorded `bmapsecs` rather than recomputing
it, and MUST NOT read a surplus page as a fault: its bits cover grain
numbers at or above `ngrains`, which nothing ever allocates. A sizing
that stopped at the first fixed point it found rather than the
smallest would not hold this bound: at `blksz` 512 it overshoots by
up to four pages on a 20 GiB partition, which is harmless on the disk
and false in this paragraph.

Every region start is rounded up to a `blksz` boundary. The reserved
run above requires that of `logoff`, and it costs at most
`blksz - secsz` bytes for each of the others, which is what makes
every checkpoint page write and every grain write aligned. The
alignment is `blksz` and not `Wunit` deliberately: **no geometry this
store accepts may depend on a build constant**, since `Wunit` is a
property of the unit a store happens to be opened on and `blksz` is
recorded on the disk.

**What bounds `blksz`.** `blksz` is layer-a's and layer-a §1.4 sets
no ceiling on it, so the ceiling here is this format's: a power of
two, at least `secsz` and **at most 1 MiB**. The reason is memory,
not the device — a grain, a bitmap page and a checkpoint page are
each one `blksz` buffer the store composes whole, and §7's queue
procs build them on their own stacks, which is why that section sets
`mainstacksize` explicitly. A `blksz` above the device's `Wunit` is
accepted and costs `ceil(blksz/Wunit)` requests per grain write
(§0); a store formatted on one unit therefore stays readable and
writable on a unit whose write unit differs.

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
supports, and the exhaustion is a distinct, named `disk full` (§6).
Splitting the two slot spaces is what keeps the extent-map region at
5.1 GiB while quadrupling block resolution, and the inline one-block
map (§2.3) is what keeps small objects out of it entirely; what
remains is that an object of two to `nblkmax` blocks consumes a full
`emapsz` slot. `nemap` is a format parameter, so an operator who
knows the workload can size it; the fix if a real workload defeats it
is §15's size-classed or grain-backed extent map, which is a format
change and therefore a `vers` bump and a reformat.

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

The third party an operator is most likely to create is shoal's own
monitor. `cmd/shoalmon` commits its map through the same kind of raw
flush (§10), so **the monitor's map partition MUST be on an `sd` unit
that no object-store instance is serving**, and `shoalmonfmt` warns
when the unit it is given already carries a shoal superblock. A
monitor co-located with an object server on a node is the ordinary
deployment; a monitor co-located with one on a *unit* is two
processes writing each other's cdbs, with nothing detecting it.

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
    260     4  csumalg   layer-a §3.2's algorithm name, as a number
    264   ...  reserved to `secsz`, zero (248 bytes at `secsz` 512)

**What the superblock carries, and why each field is here.** The
geometry, because no reader may recompute it from assumptions; the
`uuid`, because layer-a §3.4 requires an identity readable before
the instance has a map (R9); the checkpoint mark `ckseq`/`cklogoff`,
because replay must know where to start (§5); the three
per-instance facts layer-a requires to survive a restart and gives
no home — `qidnext` (R16), `epochhigh` and `monid`/`monidset` (R15);
and `csumalg`, because §14(8) makes it a MUST that the server refuse
to serve when the adopted map's `blksz`, `objmax` or `csumalg`
differs from what the disk was formatted with, and a mismatch it
cannot see is a mismatch it cannot refuse. `blksz` and `objmax` are
already here as geometry; `csumalg` is here for that check alone,
and it is the one whose mismatch invalidates every stored digest.
The number is this format's own — 1 is layer-a's `blake2s256` — so a
reader compares numbers and the map's spelling stays layer-a's.

**Two-slot rule, validity first.** The rule is stated in three
clauses, and the order matters:

1. If exactly one copy is valid, the update writes **the invalid
   one**.
2. If both are valid, it writes the one with the lower `gen`.
3. If neither is valid, the store MUST NOT write and MUST NOT serve.

In every case the new `gen` is `max(valid gen) + 1`, and the write is
followed by a flush. `gen` is a `u64` and is not treated as wrapping:
at one publish per checkpoint interval it is unreachable by many
orders of magnitude, so no reader compares generations modulo
anything. On start the store reads both copies and takes
the valid one with the greater `gen`. Clause 1 is the whole point of
keeping two copies: without it, a copy torn at a high `gen` steers
the next write onto the only good copy, and a second fault then
leaves no valid superblock and an instance whose data must be
discarded and refilled from peers. At format, copy 0 is written
first with `gen = 0` and copy 1 second with `gen = 1`, so `shoalck`
has a deterministic expectation on a fresh disk.

**Exactly one publisher.** Every superblock write carries *all*
fields, so a writer that builds its image from a stale snapshot
regresses whatever another writer advanced. The store therefore
publishes the superblock through **one function under one `QLock`**
(`publish()`, §7): it takes the lock, builds the whole image from
live in-memory state, issues one write and one flush, and releases.
The lock is what serialises publishes and what makes "built from
live state" mean something; no separate owner proc, no request
channel and no wake-up are needed for five events per checkpoint
interval, and the caller pays the write in its own proc. Five things
trigger a publish: format, each checkpoint (`ckseq`, `cklogoff`), a
`qidnext` batch advance, the first `monid` pin, and an `epochhigh`
advance. Without the single publisher a
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

**The checkpoint mark enters the image only when it is earned.**
Because any of the five triggers can publish at any moment, and every
publish carries `ckseq`/`cklogoff` along with everything else, the
checkpointer's new mark MUST NOT become part of the live image until
§2.8 step 2's flush has returned. Until then every publish carries
the *previous* mark. Otherwise an `epochhigh` publish landing between
§2.8's step 1 and step 2 writes the new `ckseq` with the pages it
describes still in a volatile device cache — and, worse, §2.8's
reclaim rule then licenses overwriting the log records that would
have rebuilt them. That is the stale-`ckseq` loss above, arriving
through a publish that was not the checkpoint's.

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
      2     1  flags     bit0 corrupt (layer-a §7.5); bits 1..7
                          reserved, MUST be zero (§0)
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

**A free entry is a valid record, not zeroes.** `state = 0`,
`vers = 1`, every other field zero, and the checksum computed like
any other entry's. Sixteen zero bytes are not the checksum of 240
zero bytes, so a region merely zeroed at format would fail every
entry's checksum and §5 step 10 would condemn every slot on the
first start. This is the same argument §2.5 makes for a bitmap page,
and `shoalfmt` (§12) discharges it the same way. A reader takes
`oidlen = 0` and `emapslot = 0` as part of what makes an entry free,
and rejects an entry that claims `state = 0` with either set.

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

At the defaults an entry is 20504 bytes in 20992 bytes of space —
41 sectors, and one read of the 64 KiB class rather than 41 separate
requests (§0). Array entries beyond `nblk` MUST be zero, and take no
part in the object's `csum`.

**A released entry's bytes are not to be trusted.** Freeing an
extent-map slot writes nothing: the bytes of the previous owner's map
survive in the region, and they still pass their own `csum128`. What
makes that safe is the other half of the rule — the commit that
*allocates* a slot zeroes the whole entry before setting the blocks
it names, and MUST name every block below `nblk`, holes included
(§2.7). So no reader ever reaches a byte of a released entry through
a live object, and `shoalck` treats an entry whose `emapslot` no live
index entry claims as free space rather than as a fault.

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
`blksz` bytes. At the default `blksz` a page is one device request;
above it a page is written in `Wunit` pieces like any other buffer
(§0). Each page carries its own header and checksum:

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

**The replay-coverage rule.** A page's `ckseq` is the checkpoint
generation whose effects the page materialises, so a page *ahead* of
the superblock is the ordinary state after a crash between §2.8's
step 1 and step 3: the pages landed and the superblock naming them
never did. A generation comparison therefore cannot be the test —
it would refuse to start after the commonest checkpoint crash there
is, which is exactly the one §3.4 calls benign. What has to be
checked is not the generation but whether the log still covers the
state already materialised. Let **`Pmax`** be the greatest `ckseq`
carried by any bitmap page that passes **its own checksum** — a page
that fails its checksum has an arbitrary `ckseq` and contributes
nothing. Then:

> After replay, `max(superblock.ckseq, the highest seq replay
> applied)` MUST be ≥ `Pmax`. If it is not, the log no longer covers
> state that is already on disk, and the store MUST NOT start.

The superblock's term is not slack. A page whose `ckseq` is at or
below `superblock.ckseq` is *settled*: the superblock names a
checkpoint that included it, so nothing has to be replayed behind it.
Only a page ahead of the superblock — `Pmax > superblock.ckseq` —
obliges replay to reach `Pmax`. Stated as "replay MUST have applied a
record" instead, the rule would refuse to start whenever replay
applies no record, which is every restart of a store that was idle
when it stopped.

§5 applies it in that order, after replay and not before. The test
discriminates the three cases that are byte-indistinguishable without
it:

- *benign* — a crash during checkpoint *N*. The superblock is
  *N−1*'s. Step 3 never returned, so §2.8's reclaim never happened
  and the log from `cklogoff(N−1)` is intact; replay covers
  `(ckseq(N−1), ckseq(N)]`, reaches `seq ≥ Pmax`, and the store
  starts and rewrites the pages at the next checkpoint.
- *malign* — checkpoint *N*'s superblock write returned and its log
  space was reclaimed, and that copy was later lost to a media fault,
  so start falls back to *N−1*'s copy over a log that has been
  overwritten. Replay stops at the first overwritten sector, short of
  `Pmax`, and `superblock.ckseq` is `ckseq(N−1)` — also short of it —
  so the store refuses. Replaying from a stale mark over
  already-materialised state is how grains that live objects
  reference get handed out again.
- *quiescent* — checkpoint *N* completes, nothing is dirtied
  afterwards, so §2.8's timer starts no further checkpoint, and the
  power goes at some later hour. Restart finds `superblock.ckseq =
  ckseq(N) = Pmax` and a log that begins at `cklogoff(N)` in unwritten
  space, so replay applies **no record at all**. The superblock's term
  satisfies the rule and the store starts. This is the commonest
  restart there is, and refusing it would be the same false refusal
  the generation comparison was replaced to remove.

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

A free record, like a free index entry (§2.3), is a valid record and
not zeroes: `state = 0`, `vers = 1`, the rest zero, and the checksum
computed. `shoalfmt` writes the region that way, and §5 step 6
rejects a record that claims `state = 0` with a non-zero `oidlen` or
`peerlen`.

72 bytes bounds an iid: layer-a §3.3 bounds a node name at 63
characters and an index is a decimal integer, so 63 + `.` + an
8-digit index reaches exactly 72. `peerlen` is a `u8` and can name
more than the field holds; the store MUST reject `peerlen > 72` and
`oidlen > 128` on read as well as on write.

`ndirty` is an implementation limit in exactly layer-a §7.1's sense.
When it is exhausted the store discards every fine-grained record for
the peer with the most records and marks that peer `fullsync`, which
layer-a §7.1 explicitly permits. **Ties go to the lowest peer name,
and the count is taken over the records rather than over the peers
the store has heard of.** Both are needed for the drop to be a
function of the region's contents alone. The order the store learned
its peers in is first-apply order while it runs and the region's own
slot order after a restart, so a tie broken by that order would drop
one peer's marks on the live path and another's when the same records
are replayed; and a peer whose name the store failed to register at
all still owns records, which a count taken over peers attributes to
nobody.

The region is read at start (§5) and the in-memory set is built from
it before replay, whose `Edirty` entries then add to and remove from
it. That is what makes the records worth their 16 MiB even though the
next paragraph re-syncs everything anyway: the coarse `fullsync` flag
says *this peer is behind on something*, and layer-a §7.1's
fine-grained records are what let the reconcile pass name the objects
instead of walking the disk. R7 is a durability requirement, and it is
met by these records surviving the crash, not by the restart flag.

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

The flag is half-built infrastructure today: everything that sets it
exists — start-up marks every peer, the exhaustion drop marks its
victim — and **nothing yet clears it**, because clearing belongs to
the reconcile pass the heal work will bring (layer-a §7.2). Until
that lands, `storefullsync` answers *behind* for every peer, known or
unknown, and only the fine-grained records carry information.

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
     52     2  flags  bit0 Fwrap (see below); bits 1..15 reserved,
                    MUST be zero (§0)
     54     2  pad
     56   ...  entries, a packed byte stream running to nsec*secsz

A record is **valid** iff `magic` and `vers` match, `nsec` is within
the log region from this offset, `seq` is the expected successor
(§5), and `csum128` verifies over the whole `nsec*secsz` bytes.
`nsec` MUST be bounds-checked before it is used to address anything:
a torn header can carry a garbage length, and hashing an unbounded
range on the strength of an unverified field is how a replay turns a
crash into a fault.

A record MUST NOT straddle the end of the region, and the
continuation rule is **modular**: a reader that has applied a record
continues at `+nsec`, and at the region start when `+nsec` reaches
the region end. A record that ends flush with the region end
therefore wraps by arithmetic and needs no marker. That case is not a
corner: a workload of one-sector records — the common commit —
counts the free tail down 3, 2, 1, 0 and reaches the end exactly, and
a rule that demanded a marker there would demand a sector that does
not exist. `Fwrap` covers the other case, where sectors remain before
the end but too few for the next record: the writer emits a one-sector
record with `nent=0` and `Fwrap` set and places the real record at the
region start, and a reader that has applied a record with `Fwrap` set
continues at the region start whatever `+nsec` says. The wrap record
carries a sequence number like any other, so sequence contiguity is
unbroken in both cases.

A wrapping commit is therefore two device writes and not one, which
is the one exception to §11's per-commit cost. The wrap record
belongs to the batch it precedes and is written **before** that
batch's post-flush, so one flush makes both durable. Written after
it, the wrap record would be a link in the log's own continuity that
a crash can drop — replay would apply the last record before the end
and continue at `+nsec` into the index region, discarding every
commit written after the wrap.

Entries are `{u8 kind, u8 flags, u16 pad, u32 len}` — `len` counting
the whole entry including this eight-byte header — followed by the
body. No bit of the entry header's `flags` is defined, so all eight
are reserved and MUST be zero (§0); `pad` is a reserved *byte* field
and is ignored on read like any other. The length is `u32` rather
than `u16` because a whole-object `Eobj` is
`~230 + 28*(objmax/blksz)` bytes: 28.2 KiB at the defaults,
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

`oflags` bit0 is **`Oslot`**: this commit changes `emapslot`, in
either direction (the slot rule below). Bit 1 is **`Ocorrupt`**: the
value of the index entry's `corrupt` flag (§2.3) that this commit
publishes. Bits 2..7 are reserved and MUST be zero.

`Ocorrupt` is in the record for the same reason everything else here
is: §8 makes the `corrupt` flag durable through an `Eobj` that
changes nothing else, and every clause of the apply below is a
function of the record alone. An apply that carried the entry's own
flag through instead would be a clause that reads live state, and
replay would silently clear what a scrub had found.

`nmap` names the blocks this commit changes; every other block below
`nblk` is unchanged, except under the slot rule below. `nfree` names
the grains this commit releases — the old grains of the blocks it
replaced, and every grain beyond a new shorter `len`.

Applying an `Eobj` sets absolute values, and it MUST, in this order:

1. set the four-tuple, `len` and the `corrupt` flag (from
   `Ocorrupt`) in the index entry, and derive `nblk = blkcount(len)`
   from that `len`;
2. if the record carries `Oslot`, allocate or release the extent-map
   slot `emapslot` names **and zero the whole target map** — all
   `emapsz` bytes of a newly allocated extent-map entry, or
   `grain0`/`dig0` when `emapslot` becomes 0;
3. set `grain[b]`/`dig[b]` for every block named by `nmap`;
4. for every block below `nblk` that `nmap` does not name and whose
   `grain[i]` is 0, set `dig[i]` to the digest of the zero bytes that
   block reads as (§2.4) — the precomputed full-block zero digest, or
   the short final block's, computed from `len`;
5. **clear `grain[i]` and `dig[i]` for every `i ≥ nblk`**;
6. free every grain in `nfree`.

Clause 5 is §2.4's invariant on the shrinking side, and it is what
makes a truncate that names no blocks correct rather than merely
cheap. Clause 4 is the same invariant over the holes: a sparse extend
across many blocks stays a metadata-only commit, and the write path
and replay agree on what the new blocks hash as, so `csum` (§4)
matches the digest array without the commit having to name 65535
holes. Where they disagreed, the object would fail its next scrub and
be routed to a whole-object repair it does not need. It is stated as
a walk of the digest array wherever `grain[i]` is 0, rather than over
the range a growth covers, because the range is a function of what
the entry held before and the walk is not — and the short final
block's zero digest changes with `len` whether or not `nblk` moved.

**Every clause is a function of the record and of `nblk` alone.**
None of them reads the entry's live state: clauses 1, 3, 5 and 6 set
absolute values, clause 2 branches on a bit the record carries, and
clause 4 is recomputed unconditionally over the map the others leave.
So applying a record twice — once live, once again in replay over a
checkpoint that already materialised part of its effect — gives the
same entry both times. That is the property §5 asserts when it calls
replay idempotent and the one §3.4's damage ⊆ repair argument rests
on. A clause that compared against the entry instead — zeroing the
target map only "if `emapslot` differs from the current value", or
making holes only of `[old nblk, nblk)` — is a clause a half-written
checkpoint disarms silently: the index page carrying the new
`emapslot` and `len` lands, the extent-map entry does not, and the
re-replay skips exactly the zeroing and the hole-filling that stood
between the object and the previous owner's grains.

**The extent-map slot rule.** A commit that changes `emapslot` in
either direction MUST set `Oslot`, MUST name in `nmap` **every block
below `nblk`** — holes included, a hole named as `grain = 0` with the
zero digest §2.4 requires for its length — and is applied by zeroing
the target map first (clause 2). Both halves are load-bearing and
neither is sufficient alone:

- Without the zeroing, a slot released by a deleted object still
  holds that object's map — §2.4 zeroes nothing on release — so a
  commit that allocates that slot and does not name every block
  inherits live grain numbers the allocator has since handed to other
  objects, and a read of the object serves another object's bytes.
  That is §2.4's invariant broken one slot-space over, and it is
  reachable without any crash.
- Without the naming, the zeroing turns those same blocks into holes:
  an object that grew past one block would read its own block 0 as
  zeros, and a shrink to one block would leave `grain0`/`dig0` empty
  rather than carrying block 0 out of the entry being released.

Naming the holes too is what makes a slot-changing record complete on
its own terms: the whole map below `nblk` is in the record, so the
entry the apply leaves does not depend on clause 4 having recomputed
what the zeroing erased. Exempt them, and a hole below `nblk` is
described by neither the record nor the map being replaced — the
zeroing leaves its `dig[i]` sixteen zero bytes, which is not the
digest of the zero bytes the block reads as, and only clause 4 stands
between the object and `hash(dig[]) != csum` the instant the
transition commits.

So the rule covers all three transitions: inline→slot (block 0 is
named even when the write did not touch it), slot→inline including
truncate (block 0 is named whether or not it is a hole), and the
reuse of a slot by a different object. When `emapslot` is 0 the map
being set is the index entry's inline `grain0`/`dig0`, and `nmap`
names at most block 0.

The cost is bounded and rare: a slot-changing commit carries at most
`nblkmax` map triples — the same 28.2 KiB the whole-object `op=full`
record already costs — and only on a transition, not on the writes
either side of it.

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

**A checkpoint materialises committed state only.** The bitmap pages
it writes carry the *committed* allocation state — the bits set by
records at or below `ckseq` — and never a staged reservation. Staged
grains (§3.1, §3.6) are held in a side reservation set that the
allocator consults and the checkpointer ignores (§6). Without that
split a checkpoint taken while a stage is live would write a durable
bit for a grain no record ever allocates: the stage's owner dies, the
grains go back to the in-memory free map, a crash follows, and replay
— which knows nothing of stages — leaves them marked allocated and
referenced by nothing until `shoalck -R`. It also breaks §3.4's
proof, which needs a page to be dirty at checkpoint *N* **iff** a
record in `(ckseq(N−1), ckseq(N)]` changed it; a stage that dirties a
page with no record behind it is damage replay cannot repair.

**A page whose write failed stays dirty.** The mark is cleared when
the page image is packed, so a change made after the pack re-marks the
page; if the write then fails, the mark is put back before the
checkpoint gives up. Losing it would not be an aborted checkpoint but
a silent one: the page is clean, so the *next* checkpoint skips it and
publishes a `ckseq` and a `cklogoff` past the records that dirtied it,
and reclaims their log space — and the committed state is then in
neither the log nor the region. It also breaks §3.4's proof, which
needs a page to be dirty at checkpoint *N* **iff** a record in
`(ckseq(N−1), ckseq(N)]` changed it; a page a record changed inside
that range and that is not dirty at *N* is the same hole from the
other side.

The new `ckseq`/`cklogoff` become publishable only after step 2's
flush has returned (§2.2), so a publish triggered by anything else
mid-checkpoint carries the old mark.

**A checkpoint that fails is counted and named.** A failing
checkpoint does **not** condemn the store: §3.2's condemnation is for
a failed *log* write, where committed state is already gone, while
here the state is still in the log and a later checkpoint over a
healed device materialises it. But it is also invisible from every
other angle — no client operation fails, and the only symptom is that
log space stops being reclaimed, which arrives at the operator as
§6's `disk full` on a store whose disk is not full. So the store
keeps a **stuck** flag — the last checkpoint failed and none has
succeeded since — together with that failure's text, reports both in
its statistics beside a lifetime count of failed attempts, and §6's
refusal for log space carries the cause **while the flag is set**.

A checkpoint that succeeds clears the flag and the text: a device
that heals is the expected end of a failure (above), and a store
whose log then fills for the ordinary reason must be answered §2.6's
bare `disk full` rather than an error it has recovered from. The
count is not cleared, and it counts *attempts*.

**A failed checkpoint backs off.** Neither trigger above paces a
retry: `ckhigh` is a level and not an interval, and a store whose
checkpoint cannot reclaim log space holds the log above `ckhigh` for
ever, so the checkpointer would re-attempt with no wait at all — and
a commit inside §6's wait asks for one every millisecond besides. A
failing checkpoint therefore sets a retry floor: `ckbackms` (policy,
default 100 ms), doubling per consecutive failure and capped at
`max(ckbackms, min(ckms, ckwaitms))` — never below the configured
floor, and never above §6's bounded wait — and reset to `ckbackms` by
any success. Neither trigger may re-arm inside that window, and
neither may a commit's request; an explicit checkpoint request — a
tool's, a test's — is not paced by it and resets it. The floor is
what makes the attempt count a rate an operator can read, and what
keeps a store that cannot free log space from contending on the log's
lock with the very commits waiting for it.

§6's wait is the upper bound because the floor is what a healed
device waits behind: a floor longer than the wait would leave a
commit refused for log space naming a failure a later checkpoint has
already cured, for as long as the floor ran. Capped there, a device
that heals is retried within one wait, and no commit carries a cured
error for longer than `ckwaitms` after the device came back.

**A checkpointer that cannot succeed again is named as such.** §0's
`Echange` condemns the *fid*: every later read, write and flush on it
fails without reaching the device, so no later checkpoint can
materialise anything and no healing device can cure it — the store
must be closed and opened again. So a **dead** checkpointer is
distinguished from a stuck one: the flag is set when the failing
checkpoint's fid is condemned, nothing short of a restart clears it,
the checkpointer stops attempting, and §6's refusal names that
condition instead. A stuck checkpointer is a device that may heal; a
dead one is a store that must be restarted, and answering the first
when it is the second costs the operator the outage. A dead
checkpointer still runs an explicit request — it fails, and a caller
waiting on one has to be answered — so only the paced paths stop.

A commit that carries data is answered §0's refusal directly on a
condemned fid, because it stages its grains before it commits; §6's
refusal below is what a commit that writes no data before its
reservation — a create, a delete — meets.

Its cost is therefore proportional to the state dirtied since the
last checkpoint and to nothing else, which is what lets §6 put a
number on how long a commit may wait for log space.

`ckseq` MUST NOT exceed the commit path's **durable watermark**
(§7), which is reached only when a batch is durable *and* has been
applied to in-memory state. Materialising in-memory state whose
backing batch has not reached the watermark, and then publishing a
`ckseq` past a record that never became durable, would leave the
checkpoint describing a write that does not exist; publishing a
`ckseq` past a record that is durable but whose effects the
checkpointer has not yet seen in memory is the same fault from the
other side — replay would start above the record, and the bits and
map entries it would have set are in neither place.

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

*Anything dirty* is **recounted at the end of each checkpoint that
completes, and never zeroed** — a checkpoint that fails returns before
the recount and keeps its pre-checkpoint count, which over-counts and
so cannot lose the trigger.
A page's count is raised on its clean-to-dirty edge only, so a page
dirtied while a checkpoint runs — after that checkpoint's own pass
packed it and cleared its mark — keeps the mark and would lose the
count. Almost everything that dirties a page also writes a record, so
the log trigger covers it; §5 step 10's condemnation is the exception,
being the one thing that dirties an index page without writing a byte
to the log, and on a store doing nothing but reads the dirty count is
then the only trigger there is.

## 3. Write path

*Policy for the mechanics; the invariants it establishes are
layer-a's.*

This maps layer-a §5.4 steps 3, 6 and 7 onto the format above. Steps
1, 2, 4 and 5a are not the store's business.

### 3.1 Stage — step 3

Serialised against every other operation on the same object by §7's
queue:

1. Read the extent-map entry if the object has one and it is not in
   the cache (41 sectors at the defaults, one read of the 64 KiB
   class); a one-block object's map is already in the in-memory index
   entry.
2. For each block the operation touches, reserve a **fresh** grain
   from the in-memory free map. The allocator never returns a grain
   that any committed map references, one released by a commit that
   is not yet durable (§3.5), or one another stage has reserved. A
   reservation goes into the **staged set** (§6), not into the
   bitmap: the checkpoint materialises committed allocation state
   only (§2.8), so a stage cannot leave a durable bit behind it. The
   grain becomes allocated in the free map when the commit that names
   it is applied (§3.2), and the reservation is dropped either way.
   This is what makes a stage invisible: it writes only where nothing
   is published, and it records nothing.
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

The committing proc takes the staged entries, batches them with any
other commits pending at that instant (§7), and performs:

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

**The raw channel has one owner at a time.** The flush is a SCSI
`SYNCHRONIZE CACHE` issued through `/dev/sdXX/raw` as a
write-cdb / read-data / read-status triple, 4.2 µs per round trip on
a held fd against 462 µs if the file is opened and closed around each
command. The triple is per-unit kernel state, so it MUST NOT be
interleaved: within the store **every flush goes through one
coalescing function** — the committers', the checkpointer's, the
superblock publisher's — which issues at most one device flush at a
time. A caller asks for "a flush that began after time *t*", and one
device flush satisfies every caller waiting at the moment it is
issued, which is what keeps `logdepth` concurrent committers from
costing `logdepth` flushes; a caller woken by a *later* round than
the one it asked for is answered by that round, because flushes are
cumulative. The exclusion is a lock rather than a dedicated proc,
because a proc buys nothing here that the lock does not: the caller
that would have sent the request is the caller that waits for the
answer either way. Outside the store the same exclusion is the
operator's (§2.1).

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

What `-w` changes is the sequence: with no raw channel there is no
flush to issue, so the commit above becomes body write, header write,
and the ordering argument rests entirely on the operator's assertion
that the unit is write-through — issue order is platter order only if
nothing is cached. Everything else is unchanged, and T1.13, which
asserts flush *placement*, does not apply to a `-w` store: there are
no flushes to place.

**A log write that fails condemns the store for commits.** The store
is then `broken`: the failing batch and every batch above it fail
with that device error, nothing above it acks, and no later commit is
accepted — in this process, ever. In-memory state is exactly the last
durable record's, because a batch that did not land is not applied
and neither is any batch above it, so reads go on being served and
they answer what a restart would answer. **There is no in-process
recovery**: the store is made whole by being closed and opened again,
which replays the log from the last checkpoint. That is a deliberate
choice over retrying — the failure is a device error on the one
structure whose ordering the whole design rests on, and a store that
kept committing over it would be writing records whose predecessors
are missing.

`interrupted` is not that failure. §0 makes it an ordinary outcome of
any device call, and §7 has a worker already inside a commit complete
that commit rather than unwind out of it, so the commit path
**re-issues** a log write or a flush that reports it — writing the
same bytes at the same offset again is idempotent — and only a real
device error fails the batch. Treating it as one would let an
ordinary client `Tflush` condemn the store and lose the next writer's
acked write.

**The apply cannot fail after the record is durable.** Everything the
apply can refuse — §2.7's range checks, the pinned-map rule of clause
2, the index entry's oid buffer and the dirty set's records — is
checked and allocated before the item joins a batch, which is before
any of its record is written, and a refusal there is an ordinary
error return with nothing durable and no sequence number spent. A
refusal *after* the post-flush would be a durable record the store
has chosen not to believe and the next start believes, which is the
one disagreement between memory and disk this design cannot arbitrate;
if one happens anyway the store stops answering reads as well as
writes, because it can no longer vouch for either.

**One apply function, two callers.** When the post-flush returns, the
committing proc applies **the whole batch's entries** — its own and
its batch-mates' — to in-memory state through the same apply function
§5's replay uses: the index entry, the extent map, the free map (both
the allocations the record names and the frees, which is where §3.5's
deferral is discharged), the staged set, and the dirty set. Applies
run in `seq` order, sequenced by the watermark (§7); only then are the
waiters woken and the operations released from their queues. Using one
function for the live path and for replay is not tidiness: the two
disagreeing about an unnamed block or an inherited grain number is a
bug that appears only after a crash, which is the most expensive place
for one to be.

### 3.3 Discard — step 7, `Tflush` and failure

Release the stage's grain reservations (§3.1), drop the composed
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

**A commit that became durable while it waited is not an exit at
all.** §6's wait for log space keeps the operation's entries on the
pending queue, so a committer may absorb them at any moment — the
expected end of the wait — and the wait re-tests that before it gives
up. An entry that is in a running batch waits for that batch, and one
that is already applied answers success. Answering `disk full` for a
write that is on the platter and published would make layer-a §5.4
step 7's "same content, same key, same `csum`" false, and taking the
entry back out of the batch it is in would strand its batch-mates
mid-record.

### 3.4 Crash points

The invariant is R2: the published four-tuple is the old one or the
new one. Taking the points in order:

| Point | What is on disk | Outcome |
|---|---|---|
| P0 before any grain write | nothing changed | old |
| P1 during or after grain writes, before enqueue | new bytes in grains nothing references; they are reserved in memory (§3.1), and the checkpoint materialises committed allocation state only (§2.8), so nothing durable marks them allocated and no log record allocates them | old |
| P2 after enqueue, before the pre-flush | as P1 | old |
| P3 during the body write, or during the header write | no valid header, or a header that fails its checksum or its sequence test | old |
| P4 after the header write returns, before the post-flush | the record is durable or it is not; if not, P3 | old or new, and nothing acked either way |
| P5 after the post-flush, before `Rwrite` | new | new; layer-a §5.4's "may or may not have been applied" covers exactly this |
| P6 after `Rwrite` | new | new, guaranteed — R1 |
| checkpoint, mid-way | half-written index/extent-map/bitmap pages, some stamped with the new `ckseq`; the superblock still names the *old* one | replay re-applies from the old mark over the half-written pages; entries carry absolute values, so replay is idempotent. Pages ahead of the superblock are expected here, and §2.5's replay-coverage rule is what tells this case from a lost superblock |
| superblock write | the other copy is valid and one generation older | replay from its (older) mark. The reclaim rule guarantees the log still covers it when the newer write simply did not land; when the newer copy landed and was *later* lost to a media fault, its log space may already be reclaimed, and §2.5's replay-coverage rule is what detects that and refuses to start |

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

The proof needs the dirty-iff-a-record-touched-it step in both
directions, and the bitmap is where it could fail: a page dirtied by
something no record names is damage replay cannot repair, because
there is nothing to replay. §2.8's rule that the checkpoint
materialises committed allocation state only — reservations live in
the staged set, not in the bitmap — is what keeps the "iff" true
there, and §2.5's coverage rule is what catches the one case where
replay's range is not the checkpoint's. The dirty region is the one
structure the step does not hold for: a torn page there loses
fine-grained marks below `ckseq` that no record replays. Nothing in
this proof covers them; §5 step 6 drops such a page, and the restart
`fullsync` (§2.6) is what makes the loss harmless.

### 3.5 The deferred-reuse rule

**A grain, an index slot or an extent-map slot released by a commit
MUST NOT be reallocated until that commit's post-flush has
returned.**

Without it: object A's commit frees grain *g* and is lost in the
crash; meanwhile *g* was handed to object B, staged into, and B's
commit was lost too. Replay restores A's old extent map, which still
points at *g* — whose bytes are now B's. A four-tuple that verifies
against nothing, produced by two writes that both correctly failed.

The implementation is §3.2's apply order and nothing else: a commit's
frees are part of applying its record, and a batch is applied when
its post-flush has returned. There is no separate deferred-free
structure to keep in step, and the rule holds for all three kinds of
release — grains, index slots and extent-map slots — because the
apply function handles all three.

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

- Each chunk reserves fresh grains (§3.1 steps 2–4) and writes them.
  Nothing is published and no log record is written.
- **`final=1`** takes the object's queue (§7), re-reads the
  receiver's current key, applies layer-a §5.5's comparison against
  it — strictly greater, or equal with `force=1` — and, if it passes,
  commits one `Eobj` naming every staged block and freeing every
  grain the object held before. If it fails it discards the stage
  exactly as below, whatever the reason — the transfer is over either
  way and its reservations must not outlive it — and where the reason
  is the comparison the error is layer-a §5.5's `stale version`.

  **A tombstone is a key, and an `op=full` may resurrect it.** A
  tombstone arbitrates normally (layer-a §1.5), so it is not one of
  the two receivers below: the comparison is made against its key
  exactly as against a live object's, and a push at a strictly
  greater key — or an equal one with `force=1` — replaces it with a
  live object at the key the push carries, reusing the tombstone's
  index slot and `qid.path`. That is deliberately weaker than §1.5's
  rule for a *client* create over a tombstone, which this store
  enforces as `ver` exactly one greater and `wepoch` no lower than
  the tombstone's. The two are not in tension:
  a create's version is this instance's to choose, so the rule that
  no older copy may outrank the new object can be enforced by
  choosing it, while an `op=full` carries a version assigned
  elsewhere that a receiver MUST adopt verbatim (§5.5) and can
  therefore defend only by arbitration. A version that is merely
  greater is what arbitration asks for and all it can ask for.

  **Two receivers have no key to defend, and the push applies to
  both whatever it carries.** Neither is a tombstone. The first is
  an object this instance does not hold: absence is not a key
  (layer-a §1.3), and it is the
  common case for a heal. The same `Eobj` carries the create — this
  commit reserves the index slot and the `qid.path` — because
  creating the object first and staging into it afterwards would
  publish a live zero-length object at the winning key before the
  content landed, and a crash between the two would leave it live,
  empty and outranking every good copy, so the resync it was meant to
  complete would never be attempted again. The second is a local copy
  whose `corrupt` flag is set (§8): it contributes no key at all
  (layer-a §1.3, and D14's rule in §5.5), so a holder that committed
  `(E, ver+1)` and then lost the content takes the serving primary's
  repair at the lower `(E, ver)`. **The commit clears the flag**: its
  blocks are the ones the transfer's own checks passed, so the verify
  that would run next finds every block matching by construction, and
  a flag left set would keep a whole copy out of arbitration — and
  keep it accepting a push at any key — until the scrubber's next
  pass, which is days (§8). The exemption ends with the push that
  used it, so a second push at a lower key is refused `stale version`
  like any other.
  A slot §5 step 10 condemned for a damaged extent map is the same
  case, reached the other way: it carries `corrupt` too, and the push
  is taken at any key. It rebuilds the map whole in a fresh
  extent-map slot (§2.7's slot rule), in the index slot and at the
  `qid.path` the object already had, and the grains the damaged entry
  named are unrecoverable and stay marked used until a bitmap rebuild
  (§8's online pass, §2.5, `shoalck -R`): writing the slot again does
  not reclaim them, because the rebuild is what recomputes the bitmap
  from the maps that are left. The rebuild does not depend on some
  earlier read having found the damage: a `final=1` that reads the
  map and finds it damaged condemns the slot and rebuilds it in the
  same call, because a repair that worked only for a slot condemned
  since the last restart is not a repair.

  **`op=create` is this path with `len=0`.** layer-a §5.5 makes
  `op=create` self-contained and arbitrated on `(wepoch, ver)` exactly
  as `op=full` is, and a created object carries no content — so the
  receiver's path for it is a stage of zero length whose `final=1`
  follows no chunk, which lands in the first of the two receivers
  above when the instance holds nothing and in the tombstone
  paragraph when it holds a tombstone. `objcreate` is **not** that
  path: it is layer-a §5.4's *client* create, which answers
  `object exists` for a live id and chooses the new version itself
  over a tombstone, where a replicated create must adopt the sender's
  version verbatim and defend an existing key with `stale version`
  (§3.7). The two agree on no refusal at all, which is why the
  receiver takes the stage.

  A **count-0 write** is not one of these and is not an extend
  either: layer-a §2.4 extends at a write *at* an offset above `len`,
  meaning bytes landing there, and a count of zero lands none. It
  commits no record and changes no key, so a replica that took one
  and a replica that did not stay at the same `len` (§4).
- **Lifetime.** A stage is discarded, and its reservations released,
  on `Tclunk` of the fid, on a `Tflush` of any of its chunks, when no
  chunk for it has arrived for `stagems` (policy, default
  30·`replms`), and at restart — which is free, because a stage is
  memory-only: its grains are *reserved* (§3.1) rather than
  allocated, no record names them, and §2.8 keeps reservations out of
  the checkpointed bitmap, so no restart path can resurrect them and
  none can leak across one.
  Were reservations written into the bitmap, a checkpoint taken while
  a stage was live would leak 32 MiB per abandoned maximal transfer
  across every subsequent restart.

  The idle trigger is *arrival*, as spelled above, and a chunk still
  in flight is not an absence of arrivals: a maximal chunk can take
  longer than `stagems` to land, and the reservations it is filling
  are the ones the sweep would otherwise return to the allocator
  while the write is still indexing them. What that sweep releases is
  the reservations alone. The handle is the fid's — only whatever
  owns the fid knows when the fid is done with it — so the sweep
  marks it expired and leaves the memory, and the `Tclunk`'s discard
  behind it finds nothing left to release. A chunk or a `final=1` on
  an expired stage is refused `stage expired`, and carries no §2.6
  prefix (§3.7): the chunks before it are gone, so finishing the
  transfer would publish holes in their place, and starting it over
  is free.
  **These are the triggers for a stage whose `final=1` has not been
  attempted, and for no other.** `final=1` consumes the handle on
  every outcome — a comparison that refused the push, a commit that
  could not be made, and a commit that succeeded alike — because the
  transfer is over either way and its reservations must not outlive
  it. So whatever owns the fid MUST forget the handle at `final=1`,
  before it knows whether the push was taken: a `Tclunk` behind a
  refused `final=1` would otherwise discard a stage that has already
  been discarded.
- **Bound, per fid and per process.** At most `stagemax` grains may
  be staged on one `/repl` fid (policy, default 2048, i.e. two
  maximal objects), and at most `stagetot` grains across the whole
  process (policy, default 16384, one eighth of a 4 TB disk's
  reservation capacity being far more than any heal needs at once).
  A chunk that would exceed either fails `disk full`, and `/status`
  reports `staged=<grains>`. The per-fid bound alone is not a bound:
  the number of `/repl` fids is not limited, so a hundred senders
  each below `stagemax` still reserve the disk. Without a bound, a
  sender that dies mid-transfer leaves grains held by nothing until
  the fid is clunked, and a heal of a whole disk can answer
  `disk full` with the disk nearly empty. Both counters are one
  integer each.

### 3.7 Error strings

*The spelling of a wire error is layer-a §2.6's and normative there.
Which of this store's refusals is a wire error is **normative** — the
carve-out §0 makes: the 9P server hands the client what the store
returns, so a condition §2.6 names MUST be answered with §2.6's
prefix and nothing else. The text of an internal-invariant error is
implementation policy; the rule that it never begins with a §2.6
prefix is normative.*

This section covers every error the library API (`lib/shoal.h`)
returns, from the write path, the read path and start-up alike, and
it exists because there is no way to build the 9P surface without a
mapping rule and no second place to put one.

**A wire error is one layer-a §2.6 names.** The store spells it
exactly as §2.6 spells it and MAY add detail after the prefix — §2.6's
own `not primary: n5.0` is the pattern. Callers can act on these:

| Condition | Answer |
|---|---|
| an id this store does not hold, on any path; and a **drop** of a tombstoned id, which is a record and not a copy (§3.8) | `no such object` |
| a read, write, truncate or delete of a tombstoned id — but **not** a drop, which answers `no such object` because a tombstone is a record and not a copy (§3.8) | `object deleted` |
| a create of a live id | `object exists` |
| an oid outside layer-a §1.1's `1*128` bound | `bad object name` |
| a write, truncate or stage past `objmax`, at either bound | `object too large` |
| an `op=full`, or the replicated `op=create` §3.6 serves as a zero-length stage, at a key the receiver's own key defends (§3.6); an adopted `op=delete` at a key its own tombstone defends (§3.8) | `stale version` |
| an `op=full` or `op=create` through the stage, or an adopted `op=delete`, at a version the object model forbids, and a chunk outside its stage's declared length | `bad ctl` |
| a read, verify or update through an extent-map entry that failed its `csum128` (§5 step 9) — block repair excepted, below; a read, write or truncate of a copy whose `corrupt` flag is set (§8); a block repair whose bytes do not hash to the stored `dig[i]` | `checksum mismatch` |
| a replicated operation whose resulting `csum` is not the one it named (layer-a §5.5, §3.8, D23) | `checksum mismatch` |
| a discard whose record fails layer-a §1.5's receiver checks: not a tombstone, not at exactly the named key, or its `wepoch` not strictly below the given epoch | `not discardable` |
| no grain, index slot, extent-map slot, staged-grain budget, or log space after §6's bounded wait | `disk full` |
| an enumeration-snapshot open past §9's `objsnapmax` | `disk full` |

**Everything else is an internal-invariant error**: a condition the
API's contract says a caller cannot produce, or one the media
produced. The record range checks (`Eobj:`, `Edirty:`, `Eslot:`), a
grain number outside `ngrains` read out of a map, a negative count, a
version of 0 — or, over a tombstone, a version that is not the
tombstone's plus one or a `wepoch` below the tombstone's — on a path
whose version this instance chooses (create,
write, truncate, delete), a tombstone adoption over a **live** copy
(§3.8), a
failed allocation, a chunk or `final=1` on a stage the idle sweep has
expired (§3.6), a block repair asked for on an object whose digest
array fails its `csum` or through an extent-map entry that failed its
own `csum128`, at a block the object does not have, at a count that is
not that block's covered length, or at a block whose bytes already
hash to their stored digest — a block whose grain the device refuses
to read is repaired rather than refused (§8), a slot cursor's
index outside `nslots`, a device error carried out of the commit
path, a
geometry that does not check out at start, and the two condemnations
— the `broken` store of §3.2 and the store whose apply failed after
its record was durable. **None of these begins with a §2.6 prefix**,
and that is the whole of what the server is promised: §2.6's set is
prefix-free, so a client parsing a prefix out of one of these would
read a bug or a media fault as an ordinary refusal. What the server
then does with one — log it, count it, answer something of its own —
is the server's decision and not this document's.

Three consequences are worth stating, because the list does not make
them obvious:

- **A version of 0 is refused on every publishing path, and which
  kind of refusal it is follows from the call, not from where the
  version came.** The key is one layer-a §1.3 forbids — `ver` starts
  at 1 and absence is not `(0, 0)` — and this is where that rule
  lives. `stagefinal` and `objadopt` answer `bad ctl`: their version
  arrives in an `op=full`, `op=create` or `op=delete` header, so a
  value §1.3 forbids is a malformed header — layer-a §5.5's common
  set, for an operation a conforming sender cannot send.
  `objcreate`, `objwrite`, `objtrunc` and `objremove` answer the
  internal `… at version 0`, which carries no §2.6 prefix, and they
  answer it whatever the version's origin: a server relaying a peer's
  `op=write`, `op=trunc` or `op=delete` over a live copy reaches the
  store through those same calls and gets the same internal refusal.
  That is not a hole in the mapping. §3.8 makes it the server's
  obligation to refuse a wire header carrying `ver=0` with `bad ctl`
  before it calls the engine at all, so a 0 reaching a delta call is
  a caller bug by construction — which is what the internal refusal
  says. The tombstone rule rides the same principle: a client create
  over a tombstone takes the tombstone's version plus one at a
  `wepoch` no lower (§3.6), and since choosing that key is the
  caller's job, any other key is the same internal kind of refusal.
  The two calls that make layer-a §5.5's comparison themselves are
  also the two that answer a defended key with §2.6's
  `stale version`: `stagefinal` (§3.6) and `objadopt` (§3.8).
- **`no such object` for a discard of an id this store does not
  hold.** layer-a §1.5's receiver checks judge a record; an id this
  store holds no record for has nothing for check (i) to judge, and
  §1.5 and §5.6's table both answer it `no such object` (D15). To
  the sender it is the same non-`ok` as `not discardable`: the
  discard is incomplete and retried either way.
- **`checksum mismatch` is answered for local damage as well as for a
  transfer that failed its check.** §2.6 defines it as "content fails
  verification, or a replicated op's resulting `csum` does not match
  the sender's"; an extent-map entry that fails its own `csum128` is
  content that failed verification, and D14 requires such a holder to
  say so rather than to answer as though the object were absent. A
  copy whose `corrupt` flag is set is that same statement made
  durably, so client access to it is answered the same way (§8), a
  count-0 write included — it commits nothing, but answering it `ok`
  is client access served. The other block-repair refusals are
  deliberately *not* this one: a repair asked for on an object whose
  digest array does not hash to its `csum`, or through an extent-map
  entry that failed its own `csum128`, is a caller that ignored §8's
  two kinds of mismatch. Those are one condition told two ways — in
  both, the stored `dig[i]` is not an acceptance test and the repair
  is `op=full` — so they are spelled as one family of
  internal-invariant error, `block repair: slot N: …`, rather than
  splitting on which structure carried the damage.

### 3.8 The peer-channel primitives

*Policy for the mechanics; layer-a §1.5, §5.5, §5.6 and §7.4 own the
rules.*

What layer-a's peer channels require of a receiver that the write
path above does not reach — because each of these does something no
client operation does — is described here; §3.7 carries the error
strings and D23 the checksum rule. Three of the four are below:
tombstone adoption, drop and the resulting-`csum` check. The fourth,
layer-a §5.6's `op=list`, is an enumeration, so it is described with
the other enumerations in **§9**.

**Tombstone adoption — `op=delete` for an id the receiver holds no
live record of.** layer-a §5.5 makes `op=delete` self-contained and
applicable "or the receiver holds no copy", and §1.5 says the adopter
"takes `state=tomb`, the key and `len=0` … and commits that as its
record". The delete path above cannot do it: it opens an existing
record and answers `no such object` for an absent id and
`object deleted` for a tombstone, which are exactly the two records
an adoption is for. So adoption is its own commit. For an absent id
it reserves an index slot and a `qid.path` and publishes
`state=tomb`, `len=0` and §1.4's zero-length `csum` in **one** record,
for the reason §3.6 gives for the absent `op=full` receiver: a slot
reserved by one commit and published by another is a window a crash
lands in. For an id whose record is already a tombstone it re-keys
that tombstone in place, keeping §2.3's stable `qid.path`, and only
at a key **strictly greater** than that tombstone's: layer-a §5.5's
comparison is made here, under the hold that read the record, because
the key arrived from elsewhere and a caller could make it only with a
second read an `op=delete` can overtake. An equal key is refused with
the lower ones — §1.3 makes equal keys equal content, a tombstone
holds none, and there is no `force=1` on a path that carries none
either — and the refusal is §3.7's `stale version`, as `stagefinal`'s
is. An absent id has no key to defend and takes any key §1.3
permits. Neither commit frees space: an adoption publishes
`state=tomb` but releases no grain, no slot and no length, so it is
not one of the space-freeing commits §6's reserved log tail is for.

Over a **live** copy it refuses, and the refusal is §3.7's internal
kind. A live copy holds content; replacing it with metadata is the
delete path's work, and the server reaches that path having
arbitrated under the object's queue. There is no safe two-call
substitute for any of this: a create followed by a delete publishes a
live object at a key the sender never sent, and a crash between the
two leaves it live — layer-a §1.3's copy that wins arbitration and
overwrites a good one, manufactured by the very call meant to
converge.

A flag on the record being re-keyed — §8's `corrupt` or §5 step 10's
condemnation — does not stand in the way, for the reason a delete
ignores both: a tombstone holds no content for either to describe,
and a record stranded at a key the cluster has moved past would block
§1.5's discard for ever. `mtime` is set to now. layer-a §1.5 does not
permit that in as many words; it **prices** it, for the neighbouring
case of a tombstone re-adopted after a missed discard — "(If the pull
resets the record's `mtime`, condition 2 delays that second discard
by `tombdays`; that costs space, not correctness.)" — and the price
is the same one this pays. There is nothing to carry instead:
`op=delete` has no `mtime` field on the wire.

**Drop — `op=drop`, layer-a §5.6 and §7.4's drop guard.** A stray
holder is told to delete its copy with **no** tombstone. A tombstone
would be wrong: it arbitrates, so one published by a stray would
travel back out and delete the copies the guard exists to protect.
`objdiscard` cannot do it either — it refuses anything that is not a
tombstone at exactly the named key.

The whole of it is **one record**: an `Eobj` that releases the copy's
grains and, by §2.7's `Oslot` rule, its extent-map slot, and an
`Eslot` for the same index slot, in one item and therefore packed in
that order into one record. §3.2's apply and §5's replay both apply an
item's `Eobj` before its `Eslot`, so the tombstone the `Eobj` would
otherwise publish is never visible: the state after the record is the
state with no record, and the `qid.path` is gone. §3.5's deferred
reuse covers all three releases exactly as it covers any commit's.
Two commits would not do: the first publishes a tombstone this holder
has no authority to create, and a crash between them leaves it
durable. The drop needs no new record kind and no `Storevers` bump:
the format carries both entries already, and an item carries both.

A `corrupt`-flagged or condemned copy is droppable, for the reason a
delete is: such a copy contributes no key (layer-a §1.3), so there is
nothing here for the flag to defend, and a stray that could not be
dropped would hold its grains for the life of the disk. The grains a
condemned map named are not recovered by the drop — nothing knows
which they were — and stay marked until a bitmap rebuild, which is
what `applyrec` counts in `grainleak` (§6). A tombstoned id answers
`no such object`, the same as an absent one (§3.7). It is the one
place the store answers that for an id it does hold a record of, and
the reason is that the record is not a copy: layer-a §5.6's `op=drop`
table and §2.5's `drop` verb allow no `object deleted`, because a
drop asks a stray *holder* to remove its copy and there is no copy
here to remove. layer-a §1.5's discard, with its cluster-wide
conditions, is the only thing that takes a tombstone away.

**The resulting-`csum` check.** layer-a §5.5 requires the receiver of
a replicated operation to compute the `csum` the object will have and
fail `checksum mismatch` before the divergent state exists. The check
is made inside the commit path, at the one point where the new `csum`
has been computed and no byte of the record has been written, so a
mismatch costs a discard (§3.3) and leaves the published state
exactly as it was. For a multi-request `op=full` the check runs over
the digests the transfer staged, and a failure discards the stage as
§3.6 says every `final=1` outcome does. A zero-byte write is the one
operation that runs the check outside the commit path: it commits
nothing and adopts no key, so there is no commit to run it in, and
the `csum` such a write results in is the one the object already
carries — which is what the named `csum` is compared against. D23
argues why the check sits where it does, and marks what is normative
here and what is this library's shape.

**What the server still owes.** The engine holds no map, so layer-a's
epoch check, the delta ops' predecessor rule, §5.6's re-check that a
dropping instance is not in `P(oid)` (`still placed`), and §1.5's
cluster-wide discard conditions are all the caller's, made under the
object's queue (§7) before it calls.

Two more are the server's because the engine's surface is not the
wire's:

- **A wire header carrying `ver=0` MUST be refused `bad ctl` before
  any engine call.** The engine answers a version of 0 by the call
  reached rather than by where the version came from (§3.7), so the
  delta calls answer it with an internal-invariant error even when
  the server is relaying a peer's `op=write`, `op=trunc` or
  `op=delete` over a live copy. Refusing the header first is what
  makes that internal refusal a caller bug by construction, and it is
  what puts layer-a §5.5's `bad ctl` on the wire for every operation
  a conforming sender cannot send.
- **A listing response is rendered by the server.** layer-a §5.6
  makes the requested `n=` a maximum the server MUST clamp so that
  the whole response fits the negotiated `msize` less `IOHDRSZ`, and
  makes `lines=` a count of advert lines and `more=1` the answer both
  to a clamp and to inventory that follows. The engine's `k` counts
  entries rather than bytes and knows nothing of `msize`, and its
  `more` out-parameter answers only the second of those two
  conditions (§9).
- **A zero-byte replicated write publishes nothing, so a sender MUST
  NOT have bumped `ver` for one.** An `n=0` write commits no record
  and adopts no key — it is the operation the `csum` check above runs
  outside the commit path for — so a sender that bumped `ver` for an
  `n=0` `op=write` and replicated it would leave every receiver a key
  behind with nothing to catch up on. A conforming primary cannot
  produce that: it reaches the store through the same `objwrite`,
  which commits nothing for `n=0`, so it has no new key to send and
  sends nothing. A receiver handed a foreign `n=0` write at a bumped
  key answers `ok` — the operation applies, having written no byte —
  and stays at its own key.

**Arbitration is split by call**, and not by whether the key arrived
from elsewhere:

- `op=full`, and the replicated `op=create` §3.6 serves as a
  zero-length stage, are **`stagefinal`'s**: it takes the object's
  queue at `final=1`, re-reads the receiver's then-current key and
  makes layer-a §5.5's comparison itself (§3.6).
- An `op=delete` over an existing **tombstone** is **`objadopt`'s**,
  made under the hold that read the record, above.
- An `op=delete` over a **live** copy is the **caller's**: it is the
  delete path's work, and that path compares nothing — it applies the
  key it is given — while the adoption refuses a live copy outright.
  So the server arbitrates it under the object's queue before it
  calls, as it does for every delta op.


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
sufficient. Every reader of an object's grains depends on that
serialisation, which is why the scrubber reads through the object's
queue like anything else (§8) rather than walking the disk beside
the write path.

**Write of a whole block.** No read. Compose, hash, allocate, write.

**Partial write.** The block's new content is (old grain bytes, or
zeros for a hole) with the new bytes merged in, hashed as one block.
Only that block's digest changes; `csum` is then re-derived from the
whole digest array, which is why the array is stored contiguously.

**Changing `len` changes digests without changing bytes.** Layer-a
§1.4 hashes the final partial block over its *actual* length, so a
change to `len` alone changes the digest of **at most two blocks: the
one that held the old `len` and the one that holds the new one.**
Every block between them is wholly covered both times, and every
block outside them is a hole or is cleared. So:

- extending `len` within the last block re-hashes that block over
  more bytes (the extension reads as zeros);
- extending across blocks makes every wholly-new block a hole whose
  digest is the precomputed full-block zero digest, and re-hashes the
  block that contained the old `len` — which the operation need not
  have named, and which is exactly the case a rule that re-hashes
  only the final block gets wrong: the map and the `csum` then agree
  with each other and disagree with the bytes;
- truncating re-hashes the block containing the new `len` over fewer
  bytes, frees every grain beyond it, and clears every map entry at
  or beyond the new `nblk` (§2.4).

**The bytes above `len` are not the object's content.** A read clamps
to `len`, so they read as zeros — but a truncate within a block
leaves the old bytes in the grain, because the cheap truncate below
rewrites no data. Two rules follow and both are needed. Wherever the
store composes or hashes a block it takes the bytes at and beyond the
block's covered length as the zeros they read as, so a partial write
above a truncated length cannot merge the old bytes back into the
object. And an extension that grows a block's covered length writes
that block **afresh, into a fresh grain**, rather than re-hashing what
the grain holds — the digest would otherwise say zeros while the
grain served the old bytes. The grain has to be a fresh one for
§3.5's reason: the published state still points at the old one until
this commit is durable. So a truncate within a block costs a read and
no write, and the extension that later covers those bytes costs a
read and one write.

A truncate that lands on a block boundary therefore costs no data
read and no data write: it is an `Eobj` with `nfree` naming the
released grains and the apply rule doing the clearing, and `nmap`
empty — unless it takes the object to one block or fewer, where it
releases the extent-map slot and so must name block 0 unless block 0
is a hole (§2.7's slot rule), which costs one map triple and still no
device read. A delete is the same shape with `len = 0`. What neither
is, is a commit that leaves the map alone — that is the bug §2.4
exists to forbid.

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
   region inside the partition, no overlaps, `ngrains` < 2^32, and
   `cklogoff` sector-aligned inside the log region — step 7 addresses
   from it, and an unchecked offset out of the region is the same
   fault an unchecked `nsec` would be (§2.7).
4. Read the index region in 64 KiB requests and build the in-memory
   index (§9) **tolerantly**: verify every entry's checksum and
   range-check `oidlen` (1..128), `state` (0..2) and `emapslot`
   (< `nemap`), and record which slots failed — but condemn nothing
   yet.
5. Read the bitmap. A page that fails its checksum sets the rebuild
   flag; it is not a refusal, and it contributes no `ckseq`. Record
   `Pmax`, the greatest `ckseq` over the pages that pass their own
   checksum (§2.5); nothing is judged by it yet.
6. Read the dirty region in 64 KiB requests and build the in-memory
   dirty set: verify each record's `csum128`, range-check `oidlen`
   (1..128) and `peerlen` (1..72), and take the `state=used` records
   that pass. A record that fails its checksum is dropped and counted
   in `/status`; it is at worst one peer's fine-grained mark, and the
   restart's `fullsync` (step 12) covers it. This is the region's
   only reader, and R7 is what it is for (§2.6).
7. Replay. Start at `cklogoff` with the expected sequence seeded at
   **`ckseq + 1`** — the seed is what makes an earlier lap's record
   unacceptable however plausible its bytes — and, for each record:
   bounds-check `nsec` against the region before using it, verify the
   checksum over `nsec*secsz`, check `seq` against the expectation,
   apply the entries, then continue at the region start if `Fwrap` is
   set, at the region start if `+nsec` reaches the region end, and at
   `+nsec` otherwise (§2.7). Stop at the first record that is invalid
   or out of sequence. **A sector the device cannot read is not one
   of those, and the store MUST NOT start.** Every other reason to
   stop is a statement about the bytes at that offset and each of
   them says the log ends there; a read error says nothing about
   them, so treating it as the end would discard whatever is past the
   fault — acked writes included — and then hand the tail back to be
   overwritten. Steps 4, 5 and 6 already refuse to start on a device
   error, and this is the same rule. **A record that cannot be
   applied refuses the start the same way**: the apply fails on a
   device error under an extent map, on an allocation failure, or on
   any entry it cannot decode or believe — an entry header or body
   that does not parse, an entry kind this build does not know, or a
   field §2.7's range checks refuse — none of which a conforming
   writer produces, because the commit path packs and checks them
   before writing (§3.2). None of those says the log ends at a record that is
   checksummed and in sequence; and, entries being applied one at a
   time, stopping there would also leave the store on a half-applied
   record no crash could produce. Applying an entry means setting absolute
   values — this slot's four-tuple becomes these bytes, block *i*
   becomes grain *g* with digest *d*, blocks at or beyond `nblk`
   become holes, this grain becomes allocated, a record carrying
   `Oslot` zeroes the target map before its blocks are set,
   this dirty record exists or is gone — so replay is idempotent and
   a partially checkpointed region is corrected by it. Replay uses
   the apply function the commit path uses (§3.2). Replay ends by
   writing back the extent-map entries the applied records dirtied,
   and a log naming more maps than the cache holds is written back
   mid-replay as well, so that replay's footprint is a function of
   the cache and not of the log. Both are safe because start-up is
   single-proc and `cklogoff` has not moved, so the records behind
   those bytes are still in the log; and the closing one is what
   makes a device error under the extent-map region a refusal of the
   *start*, named there, rather than a store that opens and whose
   every checkpoint then fails behind it. A store opened read-only
   (§12) can write nothing at all: it skips the closing write-back
   and holds the maps in the cache for the checkpoint a later
   writable open makes, exactly as a live commit's are held, and a
   read-only replay that fills the cache refuses naming *that*
   rather than letting the device answer with a bare write refusal.
   Neither refusal is log damage, so neither carries the remedy for
   it.
8. Check replay coverage. The greater of the superblock's `ckseq`
   (step 2) and the highest `seq` replay applied MUST be ≥ `Pmax`
   from step 5; if it is not, the log no longer covers state the
   bitmap has already materialised and the store MUST NOT start
   (§2.5). Replay applying no record is not itself a failure: after a
   quiescent restart the superblock's term carries the test. A store
   with no valid bitmap page has no `Pmax` and this step passes
   vacuously — the rebuild in step 11 is what covers it.
9. Read the extent-map entry of each replayed slot that has one, and
   apply its deltas. `nblk` is recomputed from the replayed `len`,
   never trusted from the entry's header sector. **A failing
   `csum128` on a replayed entry is not fatal**: a record carrying
   `Oslot` zeroes the entry and then names every block below `nblk`
   (§2.7), so none of the previous owner's map survives the apply and
   the entry is rebuilt whole; a record that leaves `emapslot` alone
   names blocks of the same object, and the bytes it does not name
   are that object's, byte-identical old-or-new and therefore intact.
   Either way, applying the deltas and recomputing the checksum
   restores the entry. Every grain number read out of such an entry is
   bounds-checked against `ngrains`; one out of range is damage
   replay did not cover, and the slot goes to `/lost`. A failing
   `csum128` on an entry replay did *not* touch is fatal for that
   slot in the same way. Step 9 reads only the entries replay
   touched, so an entry no record named is judged when the object is
   first read rather than at start: the read refuses, and the slot is
   condemned exactly as step 10 condemns an index entry. Serving it
   instead would answer from grain numbers and digests that are the
   damaged bytes'.
10. Condemn what is left. An index entry that still fails its
    checksum after replay, or fails a range check, is genuine media
    damage: it is listed in `/lost` with `kind=corrupt`, its slot is
    not reused, and it is not served. The same list takes the slots
    step 9's rule condemns later, when a damaged extent-map entry is
    first read. The two differ in what the next checkpoint writes
    back. An entry that could not be read is written back as a free
    entry — there are no bytes worth preserving. An entry condemned
    for a damaged extent map is intact, and is written back as it
    stands: its slot stays allocated across a restart, the grains the
    object holds stay accounted for, and the object keeps its oid, its
    key and its `qid.path`. Its `/lost` line is restored at start from
    the entry's own `corrupt` flag, which the checkpoint wrote back
    with it — `/lost` is every copy that fails local verification
    (§8), so a copy the store has already condemned is on it whether
    or not this run has read the damaged map. What start-up does not
    do is *re-establish the damage*: step 9 reads only the entries
    replay touched, so the extent map is judged again at the next read
    of the object, by the same rule that found it the first time.
    Such a slot also stays **hashed**, and takes the
    index entry's `corrupt` flag: what "not served" means for it is
    that nothing reads through the damaged map — content reads,
    verifies and every update but §5.5's `op=full` refuse — while
    `objstat` still answers with the key and `corrupt=1`, which is
    what D14 requires of a holder that cannot vouch for its copy and
    therefore MUST NOT answer as absent. The flag is written back with
    the entry, so a restart still knows the copy is not to be trusted.
    An `op=full` that heals it (§3.6) drops it from `/lost` and
    clears the `corrupt` flag with it, as it does for every other
    receiver §3.6 names: the pushed content is what the transfer's
    own checks passed, so there is nothing left for the flag to
    describe (§8). The line carries `slot=<n>`
    and **omits `oid=`**, rather than printing 128 bytes the store
    does not trust or inventing an oid layer-a §1.2's grammar would
    not admit; §14(15) records the deviation from layer-a §2.2's
    field list.
11. Complete the free map: the bitmap, plus every allocation and free
    replay applied, minus nothing — there are no reservations at
    start (§3.6). If step 5 set the rebuild flag, rebuild it instead
    by scanning every live object's map (§2.5), log the event, and
    report `bmaprebuild=yes`. Build the two slot free lists (§6)
    here as well: the extent-map list is read from the `emapslot`
    fields, which step 7 may have changed.
12. Set `fullsync` for every peer (§2.6). Clear `cur` for every
    object — which costs nothing, because `cur` is never on disk
    (§14(1)).
13. Set the log tail after the last valid record, take `qidnext` from
    the superblock, and start serving.

**Cost bound (R17).** Steps 4–9 read the index region, the bitmap,
the dirty region and at most the log, and hash everything they read.
At the defaults and `nslots = 2^20` that is 256 MiB + 32 MiB + 16 MiB
+ ≤ 64 MiB in 64 KiB requests — 4.6 s at the measured 80 MB/s — plus
the hashing, which is the larger term: 2^20 index entries at 4.86 µs
each is 5.1 s of CPU, and the other regions add ~2 s. Call it ten to
twelve seconds at the recommended sizing, half of it hashing. Step 9
adds one extent-map read per multi-block object touched since the
last checkpoint. The bound is a function of format constants and of
nothing else — in particular not of the number of objects that were
being written when the crash happened, and not of their sizes. The
rebuild path of step 11 is the exception and says so.

**What an operator sees when it fails.** Every refusal above prints
one line naming the structure, the offset, and the tool that
addresses it, and exits non-zero — the store never starts in a
degraded mode it did not name. A corrupt index entry is a running
store with an object in `/lost`. A bitmap page that fails its
checksum is a slow start, not a refusal. A bitmap page ahead of the
superblock is *not* a refusal either — it is the ordinary
mid-checkpoint crash — but a `Pmax` that neither the superblock nor
replay reaches is, because the log no longer describes what the disk
already holds; the tools for it are `shoalck` and, if the log is
genuinely gone, refill from peers. Two invalid superblocks is a store
that will not start, and there the honest answer is `shoalfmt -r`
plus refill from peers, since the disk's identity is gone, which by
layer-a §1.5 makes it a reformat-before-rejoin case anyway.

## 6. Space management

*Policy.*

**Allocator.** Grains are fixed-size and interchangeable, so
allocation is "find a clear bit". The in-memory free map is a bitmap
plus a rotating cursor and a free count; allocation is O(1)
amortised and there is **no external fragmentation and no
coalescing**, because there is nothing of a different size to
coalesce.

Beside it is the **staged set**: the grains stages have reserved and
no record has yet named (§3.1, §3.6). The allocator skips them; the
checkpointer does not see them (§2.8); a grain leaves the set for the
bitmap when the commit naming it is applied, or leaves it for nothing
when the stage is discarded. It is a small hash of grain numbers,
bounded by `stagetot` plus the writes in flight, rather than a second
32 MiB bit array, because that is what it holds.

Fixed grains are the whole payoff of tying the grain to `blksz`: one
number per block locates the bytes and indexes the digest, one bit
per grain manages space, and the allocator is thirty lines.

The cost is internal fragmentation: an object of one byte occupies
one grain (16 KiB), and every object statically reserves 256 bytes of
index. At the recommended sizing that is 0.13% of the partition for
metadata plus up to `blksz-1` per object's final block.

**Slots.** Two free lists in memory, rebuilt at start (§5 step 11,
which is after replay has settled every `emapslot`): index slots from
the index region, extent-map slots from the `emapslot` fields of live
entries. A slot of either kind is reused only after the commit
that freed it is durable (§3.5).

**Reclaim of tombstones.** A tombstone's content is released at
delete time — the `Eobj` that sets `state=tomb` carries `len=0`,
`nmap` empty, `nfree` naming every grain the object held, and
`emapslot=0`, so the extent-map slot is released with the content.
A slot §5 step 10 condemned is the exception, and it is the point of
allowing the delete at all: the entry that named its grains is the
damaged bytes, so the delete reads no map and its `nfree` names
**nothing**. The extent-map slot is still released and the tombstone
is still clean. The grains come back at §8's **online rebuild**, whose
engine half the store has: a walk folds every live map into a shadow
bitmap and the swap installs it behind a write barrier, on a store
that never stops serving. The offline **bitmap rebuild** — `shoalck
-R` (§12), or §5 step 11 when step 5 set the flag — is the same
reclaim on a store that is not serving, and the count below is what
says how much either would return (§3.6, §8, §12).

The store counts what it left behind. A delete of a condemned slot,
and §3.6's `op=full` over one, each add `blkcount(len)` to a
**leaked-grain count** that `/status` reports as `grainleak=`: the
index entry's `len` is intact — it is the extent-map entry that is
damaged — so the count is an upper bound, and exact for an object
with no holes. §8's swap discharges it, because the swap is what
returns the grains; what it leaves standing is what leaked in a slot
the pass had already folded, which that swap installs rather than
returns. The number is what is outstanding rather than what has ever
leaked. It is memory only and starts at zero at every start,
because it is one session's observation of what that session left
marked and not a property of the disk: an ordinary restart does not
rebuild the bitmap — §5 step 11 rebuilds only when step 5 set the
flag — so after one the grains are still marked and the count still
reads zero. The standing number over a disk's life is §12's `shoalck`
cross-check, and the two need not agree: this count is
`blkcount(len)`, an upper bound for a sparse object, while the
cross-check reports the grains actually marked and unreferenced. Not
every leak is countable: a slot §5 step 10 condemned for an index
entry that does not unpack has no readable `len`, so it raises
`lost=` and nothing else.

What survives is the 256-byte index entry. Layer-a §1.5's discard, once
its three cluster-wide conditions hold, commits an `Eslot` and the slot
returns to the free list. The discard names the tombstone's key and the
caller's current map epoch, and the store re-checks §1.5's two receiver
conditions inside the call, under one hold of the state lock — the
record is a tombstone at exactly that key, its `wepoch` strictly below
the epoch — answering `not discardable` otherwise (§3.7). Check (i)'s
two halves are answered **state first**, so a live record is refused as
not a tombstone whether or not the key matches; the detail after the
prefix is implementation policy (§3.7), and pointing a caller at a key
when the state is the objection would send it to re-read the wrong
thing. The checks are atomic among themselves, so they judge one record
where a separate stat-then-discard could race an `op=delete`; the window
between the checks and the `Eslot` commit is closed by the caller's
per-object queue (§7), as for every mutation. `tombdays` is evaluated
against the entry's `mtime`, which is why the tombstone keeps one.

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
  and then answers `disk full`. If the **last** checkpoint failed
  (§2.8) the log will not drain at all, and the disk may be nearly
  empty, so that refusal names the cause behind the wire error:
  `disk full: log full and the checkpoint fails: <error>`. A failure
  a later checkpoint has cured does not: the store's log drains
  again, and this refusal is then the ordinary one. The changeover is
  bounded by this bullet's own wait — §2.8's retry floor is capped at
  it — so the first attempt after the device heals falls inside one
  `ckwaitms` and no commit names a cured failure for longer than that.
  If the checkpointer is **dead** rather than stuck (§2.8) — its fid
  condemned, so no later checkpoint can succeed — the refusal says so
  instead: `disk full: log full and the checkpointer is dead:
  <error>`.
  Layer-a §2.6's prefix is what the client matches on and does not
  move; what follows it is for the operator reading the log.

**The log's reserved tail.** The last `logresv` sectors of free log
space (policy, default one sixteenth of `logsecs`) are usable only by
commits that free space: an `Eobj` that releases something and
allocates nothing — delete, truncate, drop — and an `Eslot`. What
counts as a release is grains freed, a live copy becoming a
tombstone, a length shrink, or the extent-map slot §2.7's `Oslot`
rule gives back; publishing `state=tomb` is not one by itself, so a
re-keying tombstone adoption (§3.8) and a `corrupt` flag set over a
record that is already a tombstone (§8) free nothing and may not draw
on the reserve. An
`Edirty` is not one of them in either direction: adding or removing a
fine-grained dirty record frees no log space, so a remove may no more
draw on the reserve than an add may. The
reservation is a property of a *record*, and §7 batches many commits
into one record, so the rule is on the batch: a batch that draws on
the reserve MUST contain only space-freeing commits, and an ordinary
commit that would take the log into the reserve waits instead of
joining. Otherwise the reserve is spent by exactly the traffic it
exists to exclude. So
"delete always works" is true rather than nearly true: without the
reservation, log exhaustion blocks the delete and the tombstone
discard that would have relieved the slot exhaustion, and the two
exhaustions lock each other in exactly the direction that removes the
escape.

**The wait, and its bound.** Waiting is right because log exhaustion
is normally transient: the checkpointer is already running and the
space is already reclaimable. The waiting operation's entries stay on
§7's pending queue for the whole wait, so the ordinary end of the
wait is another committer absorbing them; §3.3 states what that means
for the answer. The bound is `ckwaitms` (policy,
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

Work that names no object but is **not** short has nowhere to go in
that scheme: a render or a directory read that takes `qlstate` would
block the loop, and the loop is what answers `Tflush`. So the pool
carries one **reserved queue** beside the hashed ones and outside the
hash, for exactly such a request — a queue of its own, so that
offloading one neither waits behind an object's operations nor
reorders them against each other. It is otherwise an ordinary push:
counted in the depth, flushable, and unwound through the same single
exit. Six rows of the served surface push to it: the `/dirty`,
`/lost`, `/tombs` and `/advert` opens, each of which renders its file
out of engine state, and the `/obj` and `/meta` directory opens and
reads, which take a §9 snapshot and walk it. The server's own test
point puts work there too.

**The pool size is a ceiling, not just a collision parameter.** A
queue proc runs one pushed request at a time, and a client write
occupies its queue from layer-a §5.4 step 2 to step 6 or 7 — across
the replication round trip, up to 3·`replms`, three seconds at the
default. So the pool size is the maximum number of client object
operations that can be in flight at once, full stop; the 65th waits
even if it hashes to an idle queue's neighbour. The collision case —
a read of object *A* waiting behind a write to object *B* with the
same hash — is the milder half of the same number. The sizing rule
is therefore **queues ≥ expected concurrent object operations**, not
≥ expected objects, and `/status` reports the queues' depth so
saturation is visible rather than folklore. Nothing else in the store
inherits the ceiling: the eight-way concurrency §11's `op=full` row
depends on is grain writes issued by the I/O procs below, which one
queue proc can have in flight at once.

**`Tflush`.** `Srv.flush` calls `reqqueueflush` on the queue the
flushed request was pushed to; the handler is given the *flush*
request, so the store keeps the `Reqqueue*` in `r->aux` at push time
to find it, and answers `respond(r, nil)` — `lib9p` then parks the
`Rflush` until the flushed request itself responds.

What `reqqueueflush` does is worth stating plainly, because two of
its properties are load-bearing:

- A request still **queued** is removed and answered `interrupted`,
  then the parked `Rflush` follows. That is an `Rerror` before the
  `Rflush`; it is legal 9P — the client discards the reply to a
  request it flushed — and layer-a §5.4.1 as amended says so (§14(14)).
- A request already **running** is interrupted with a note, which
  aborts the system call its proc is in. So the check point is a test
  of the queue's public `flush` flag *and* an `interrupted` return
  from a device call (§0), and either one unwinds into the whole of
  step 7 (§3.3) before the request responds.
- A worker already inside a commit batch cannot be pulled out of it,
  and this is by construction rather than by care: the batch's device
  writes are issued by procs the note does not reach, and libc's
  `QLock`/`Rendez` wait loops resume across an interrupted rendezvous
  (`qlock`(2)). It completes the commit, then still performs the
  invalidate half of step 7, and answers without answering the write
  — the "MAY have been applied" case layer-a §5.4.1 contemplates.

**Shared in-memory state, and the locks over it.** Deleting the
per-object lock does not delete the need to protect the structures
every proc touches. Four `QLock`s cover the shared *state* — beside
the one `Reqqueue` keeps for its own queue, which the store does not
touch:

| Lock | Covers | Taken by |
|---|---|---|
| `qlstate` | the index array, the oid arena and hash table (§9), the index-slot and extent-map-slot free lists, the free-grain bitmap and its cursor, the staged set (§6), the dirty set, and §9's open-snapshot count and `closed` flag | every queue proc (stage, apply), the committer applying a batch, the checkpointer, the scrubber's commits, the service loop taking an enumeration snapshot or rendering one of its entries, `storeclose` giving up the store's own reference after its proc wait, and `objsnapclose` giving back a snapshot's |
| `qlemap` | the extent-map cache: which entries are present, their loading state and pin counts, and the LRU (§9) — not a pinned entry's contents, which its pin covers | every queue proc, on a map read, a pin and an unpin |
| `qllog` | the log tail and free space, the pending-commit queue, batch numbering, the durable watermark, and the two flags that condemn the store — `broken` and the failed-apply flag beside it (§3.2) | every committer, and every entry point that refuses on a condemned store |
| `qlsuper` | the five publishable superblock fields and the publish itself (§2.2) | the checkpointer, a `qidnext` batch advance, the first `monid` pin, an `epochhigh` advance |

Three more `QLock`s are not over state but over one activity each,
and they are **leaves**: they serialise a thing being done, and
nothing is ever taken under them.

| Lock | Covers |
|---|---|
| the flush lock | the coalescing flusher's ticket counters (§3.2): who is issuing the one device flush and who is waiting for it |
| the checkpoint lock | the checkpointer's request and completion counters — the paced pair a trigger or a committer advances and the exempt pair an explicit `storecheckpoint` advances (§2.8) — its wake-up, and the failure state a checkpoint leaves behind (§2.8): the stuck flag, the dead flag, the retry floor in force and the earliest time a paced attempt may run behind it, the count of failed attempts and the last failure's text, which §6's refusal reads under it. The checkpoint itself runs with it released |
| the proc lock | the count of procs the store has started, so `storeclose` can wait for them |

**Releasing the store itself is under no lock at all**, and is ordered
rather than locked. §9 lets an object snapshot outlive `storeclose`, so
the free runs from `storeclose` or from the last `objsnapclose`,
whichever observes `closed && nobjsnap == 0` under `qlstate` — and it
runs *after* that hold is dropped, because the `QLock` is a field of the
memory being freed. That is safe on the ordering **given §9's contract
on the caller**, and not on the ordering alone. The parties that block
on the lock holding a claim are `objsnapent` and `objsnapclose` of a
snapshot whose count is not yet given back, an `objsnapopen` that has
taken §9's slot, and `storeclose` itself — and the predicate being true
says there is none of those. Every other caller of the lock holds
nothing: `objsnapopen` before the slot, `dirtysnap`, `lostsnap`,
`fullsyncsnap`, `storestat` and the object API, each of which would wake
in released memory if it were queued here when the free is decided. §9
makes that the caller's obligation — quiesce, then close — rather than
an ordering the engine can enforce, because a waiter would have to be
counted under the lock it is waiting for. The engine's own procs are
excluded by the wait above, which is also why `storeclose` sets `closed`
only after that wait has returned — it is the store's own claim, and
giving it up while the call is still asleep on `procrz` inside the
`Store` would let the last `objsnapclose` free it underneath.

Three rules make that discipline checkable rather than aspirational:

1. **No proc holds two of the four state locks at once**, and the
   only nesting anywhere is `qlsuper` over the flush lock — which is
   forced, because the publisher's write must be serialised against
   other publishers *and* its flush must go through the one flusher
   (§2.2, §3.2). Nothing takes `qlsuper`, or any state lock, under a
   leaf. There is therefore one edge in the whole lock order and no
   cycle. The commit path is the one that looks like it needs nesting
   and does not: it takes `qllog` to join a batch, releases it, does
   its I/O, then takes `qlstate` to apply — and the extent maps the
   apply mutates are pinned, not held under `qlemap` (below).
2. **None is held across a device I/O, a flush wait or a `Rendez`
   sleep**, with exactly one exception: `qlsuper` *is* held across
   the superblock write and its flush, because serialising that write
   is the lock's whole purpose (§2.2). Nothing in the client path
   waits behind it except a `qidnext` batch advance, once per 1024
   creates. The rule matters most for `qlstate`: the service loop
   takes it to snapshot `/obj` and to render `/status`, so a
   `qlstate` held across an 8.4 ms write would block `/status`,
   `/ctl` and `Tflush` — the failure layer-a §5.4.1 forbids.
3. **Everything a shared structure points at is in shared memory.**
   The engine's procs are `proccreate` procs in the server and
   `rfork(RFPROC|RFMEM)` procs in a T1 program, and the second kind
   share the data and bss segments and **not** the stack. That is
   worse than unshared: every proc's stack is mapped at the same
   virtual address, so a shared list holding a pointer into one
   proc's stack does not fault when another proc follows it — it
   lands on that proc's *own* object at the same address. A pending
   queue built from callers' stack-allocated items therefore aliases
   silently: the second committer links an item to itself and walks
   that list for ever while its batch-mates sleep on a batch that
   never completes. So the pending queue's items and the entries they
   carry are the store's own allocations, and a caller passes a
   template that is copied into one. The `Eobj`'s `map` and `freed`
   arrays are the exception: they stay the caller's *heap*, which is
   shared for both kinds of proc, and they are safe because the caller
   stays inside `logcommit` until its batch has been applied.

The extent-map cache is where rule 2 needs a mechanism rather than a
promise. A miss inserts an entry marked *loading* under `qlemap` and
releases it; the missing proc reads the 41 sectors through an I/O
proc; it then fills the entry and wakes any other proc that found it
loading. So one read serves concurrent readers of the same map and no
lock spans it.

**A staged map is pinned from the stage that read it to the apply
that changes it.** The queue proc that reads an object's map at
§3.1's staging step pins the cache entry, and the pin is dropped when
the batch carrying that object's commit has been applied — up to
3·`replms` later, which is the whole reason the entry cannot be left
to the LRU: the cache is a few thousand entries and the apply comes
long after the read. A pinned entry is not in the eviction set, so
the apply always finds the map in memory. It therefore never faults,
never reads the device under a lock, and mutates the map **under the
pin** rather than under `qlemap` — which is what lets the committer
apply its whole batch under `qlstate` alone and keeps rule 1 true as
stated. Without the pin the apply would have to take `qlemap` inside
`qlstate` and might read 41 sectors there: a lock order to get wrong
and a device read under the lock the service loop needs for
`/status`, which is both rules broken at once. The pins cost nothing
to bound: one object operation holds at most one, and the queue
pool's size is the ceiling on operations in flight, so at 64 queues
at most 64 of a few thousand entries are pinned.

**Group commit: the committer writes its own batch.** A worker
reaching layer-a §5.4 step 6 becomes the committer of a batch or a
member of one, and there is no separate assigner or writer proc:

1. It takes `qllog`. If a batch is forming, it appends its entries to
   the pending queue, sleeps on a `Rendez`, and is now a member. If
   not, it becomes the committer: it absorbs whatever is already
   pending into its batch — as much as fits **§2.7's maximal
   record**, which is the largest record this geometry can hold and
   replay will accept — stamps the batch with the next `seq` and the
   next log offset, and releases the lock. The lock is held for
   microseconds and never across I/O. There is exactly one bound on a
   record's size and every part of the store computes it the same
   way: a batch cap of its own would be a second bound that says
   nothing about the first, and a batch of enough small items would
   make a record that is written, flushed and acked and then refused
   by replay, which stops there and discards it and every commit
   after it. A single operation whose entries already exceed the
   maximal record is refused before it joins a queue, so the head of
   the pending queue always fits.
2. It writes the batch's body sectors, asks the flusher for a flush,
   writes the header sector, and asks for another (§3.2). A record
   larger than `blksz` is written in `ceil(nsec*secsz/blksz)` pieces,
   each of which the device layer splits again if `blksz` exceeds the
   unit's `Wunit` (§0). The write is split at `blksz` and not at
   `Wunit` for §2.1's reason — what a store does must not turn on a
   build constant — and it costs nothing: how many requests a record
   takes is not something §3.2's argument depends on.
3. When its post-flush returns and every lower-numbered batch has
   released the ordering — which a batch that failed does without
   applying — it applies its whole batch under `qlstate` (§3.2) —
   over pinned extent maps, so no part of the apply faults —
   advances the watermark, and wakes its members, which answer
   `Rwrite`.

Concurrency comes from the committers themselves: at 64 queues there
can be 64 of them, and `logdepth` (policy, default 4, maximum 8) is a
semaphore under `qllog` capping how many batches are in flight at
once. That is what puts several writes in flight, and it is the whole
reason the design can claim more than the 114 durable writes/s a
single serial writer gets — the measured 409/s came from eight
independent writers, and one proc executing a serial loop is depth 1
and gets 114/s no matter how cleverly the queue is drained. What the
shape removes, against a dedicated assigner plus a writer pool, is a
proc, a message type, a handoff and a free-writer allocation in the
most crash-critical loop in the store; what it keeps is every
property the §3.2 and watermark arguments rest on.

The flusher (§3.2) owns the raw channel and coalesces: `logdepth`
committers asking for a flush at the same instant cost one device
flush, not `logdepth`.

**The durable watermark.** A batch's members are woken only when that
batch's post-flush has returned, **every lower-numbered batch's has**,
and the batch has been applied to in-memory state. Without the
ordering, a crash after batch *n+1* landed and batch *n* did not would
leave replay stopping at *n* and discarding *n+1* — an acked write
lost. With it, *n+1* was never acked.

The same argument holds with no crash in it, and that is the failure
case: if batch *n*'s record does not become durable — one device
error on one log write — replay stops at *n*, so *n+1* is discarded
however well its own bytes landed. So a batch whose write failed
records the lowest sequence number that did not land **before** it
releases the ordering, and every batch at or above that number fails
with that device error rather than acking (§3.2's `broken` store).
Two counters carry it: a **release order** every batch advances when
it is done with, whether it landed or not, and the durable watermark,
which only a batch that landed and was applied advances. §2.8 binds
`ckseq` to the second, so no checkpoint can publish a mark past a
record that never became durable. Including the apply in the
watermark is what lets §2.8 bind `ckseq` to it: a checkpoint may
materialise state for record *n* only if record *n*'s effects are in
the memory it is materialising from. Batch membership is fixed before
any of its writes are issued, so no member's grain writes escape the
pre-flush.

There is no timer and no artificial delay, so **a lone writer pays
exactly one flush, one write and one flush** — the pending queue is
empty when it arrives. Batching happens only among commits that
coincide with a write already in flight, which is precisely the
coincidence worth exploiting.

**Procs, and where they come from.** The engine is a library, and it
must run both inside the libthread 9P server and inside a plain-libc
T1 program (§12, §13). So it **takes a `spawn(void(*fn)(void*),
void*)` callback at open** and uses `QLock`, `Rendez` and `Lock` and
nothing else: T1 passes an `rfork(RFPROC|RFMEM)` wrapper, the server
passes `proccreate`, and libc's `qlock`(2) primitives mean the same
thing under both. There is no `<thread.h>` anywhere in the library.

The procs it makes for itself are the checkpointer (§2.8) and nothing
else; the concurrency the throughput figures in §11 depend on is the
callers' — a queue proc issues its own grain writes, and a committer
writes its own record — so the requests in flight are the procs
already in flight, and no pool of I/O slaves stands between them and
the device. Each of those procs calls the `Dev` vtable (§0) directly,
which is proc-safe: `pread`/`pwrite` carry their own offsets, so
concurrent requests on one fd are safe, and the simulated disk holds
one lock over all of it. This is also what settles the question of
whether an `Ioproc` belongs in the vtable: it does not, and the
vtable stays four calls and a geometry.

**How many procs, and how big.** The service loop, 64 queue procs
and the reserved one, the checkpointer and the scrubber: about 68,
which is unremarkable on
9front but is a number worth having written down, since the queue
count is a tunable and each queue is a proc. In the server every one
of them is created by `proccreate` — `reqqueuecreate` included — so
the program sets `mainstacksize` explicitly: a queue proc composes a
`blksz` block and builds a record on its stack, and the default is
not sized for that.

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
`verify` ctl verb and `op=verify` on `/rpc`. Verify commits nothing:
it is also what `op=verify` and `shoalck -v` answer with, and neither
may write.

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

**What the engine builds, and what the server still owes.** The
engine holds the per-object primitives and the durable state; the
pass that drives them — the proc, its rate limit, the queue it pushes
through and the peer fetch — is the server's. All of it but the peer
fetch is built: `srv/job.c` runs the pass from layer-a §2.5's `scrub`
verb, in a proc holding one of the server's background jobs, and
paces itself over the bytes it hashes (§14(31)). The primitives are:

- **verify** one object, as above, mutating nothing.
- **scrub** one object: verify, then the one durable transition that
  verdict licenses. A mismatch on a copy the index calls whole sets
  the `corrupt` flag; every block matching on a copy the index calls
  corrupt clears it; an unchanged verdict commits nothing, because a
  scrubber that wrote a record per object per pass would put the
  whole disk through the log every `scrubdays`. It answers the same
  three states verify does, because which repair to ask for is what
  they say.
- **block repair** of one block, given the bytes a caller fetched
  from a holder at the same key. It refuses unless `hash(dig[]) ==
  csum` — the acceptance test's own precondition, and a caller that
  asks here for an object whose array fails is a server bug, so that
  refusal is §3.7's internal kind and carries no §2.6 prefix. A slot
  §5 step 10 condemned is the same condition reached the other way,
  since the entry naming the block's grain and digest is itself the
  damage, and is refused in the same internal words. It then accepts
  the bytes only against the stored `dig[i]`, answering `checksum
  mismatch` if they do not hash to it, and refuses — internally again
  — a block whose *own* bytes already hash to that digest: repair is
  driven by the set verify answers, and a block outside that set is
  whole, so the commit would change nothing, cost a grain, and, where
  the block is a hole (§4), leave the object one grain heavier with
  the same content. A grain the device will not **read** is not that
  case and does not refuse: the offered bytes have already passed the
  acceptance test, the read was only ever asking whether the repair
  was needed, and a grain that cannot be read is the plainest case of
  its being needed. On acceptance one `Eobj` publishes the block with
  the four-tuple unchanged, freeing the grain it replaced under §3.5
  like any other commit. It does **not** clear the flag: one block
  matching says nothing about the others, and the clearing belongs to
  the verify that finds every block matching.
- **a slot cursor**, so a pass can walk the index in order. It
  answers what one slot holds — live or tomb, the oid and the
  four-tuple — and copies the oid out, because the entry's own copy
  is freed by the apply of a commit that releases the slot. It holds
  the state lock for that copy and not across the caller's verify.
  What it answers is a snapshot of a slot and not a lease on it, so
  every call the caller then makes names the oid rather than the
  slot.

**The `corrupt` flag, and what a flagged copy answers.** The flag is
durable — an `Eobj` that changes nothing else but its `Ocorrupt` bit
(§2.7), so a restart does not forget it — and the object is listed in
`/lost`, together with every slot §5 step 10 condemned: `/lost` is
every copy this instance holds that fails local verification, which
is layer-a §7.5's definition of it, and it is maintained by the set,
the clear, the condemnation and start-up alike rather than built
once.

A flagged copy fails client access with `checksum mismatch` (§3.7's
row): read, write and truncate refuse, and so does a write of zero
bytes, which commits nothing but is client access all the same. Three
calls do not refuse, and each is how the flag is meant to be got rid
of: `objstat`, because that is where the flag is read; verify and
scrub, because they are what clears it; and **delete**, because
`op=delete` is self-contained — it arbitrates on the key it carries
and replaces the content with none — so there is nothing left for the
flag to defend, and the tombstone it commits holds no content to be
suspect of and so carries the flag cleared. A create over a live
flagged copy is still `object exists`. A slot §5 step 10 condemned is
in the delete's set for the same reason and by the same rule: a
delete arbitrates on the key it carries, and a copy that fails local
verification has none to defend. The grains its damaged map named are
unrecoverable whichever way the delete goes — refusing does not
reclaim them, since §5 step 11's rebuild re-marks whatever a live
entry's map still says (§3.6) — and refusing costs the object its
only exit: `op=full` is a condemned copy's other repair, and an
object being deleted cluster-wide has no live copy left to push one,
so layer-a §1.5's tombstone discard would wait on this witness
forever. The tombstone releases the extent-map slot, so the next
rebuild returns the grains — the online pass below on a store that is
serving, or `shoalck -R` (§2.5, §12) and §5 step 11 when step 5 set
the flag on one that is not. Until one runs they are marked and
referenced by nothing, and §6's `grainleak=` is the store's own count
of them.

A corrupt copy loses arbitration against everything including absence
(layer-a §1.3), which the server enforces by refusing to advertise
it.

**Scrub runs inside the queues.** *This is the half the server
builds, and it is built (§14(31)).*
A background proc walks slots in order, but it does not read grains
itself: for each object it pushes one verify request onto that
object's `Reqqueue` and waits for the answer, exactly as a client
read would, and the repair commits below go the same way. The queue
is the store's only object-level serialisation, and §3.5 defers a
freed grain's reuse only until the freeing commit's flush returns —
which says nothing about a reader that started earlier. A scrubber
reading outside the queue would therefore hit grains freed,
reallocated and staged into under it, and would durably flag a live,
correct object `corrupt`: a background consistency checker that
manufactures corruption is worse than none. One object per push keeps
the pause it imposes on a client to one object's verify, and it
rate-limits itself to the configured KiB/s so a full pass takes about
`scrubdays`. At layer-a §7.5's ~4 MiB/s on a 4 TB disk that is ~7% of
one CPU spent hashing, continuously, which is worth knowing on a
two-vCPU node that also runs the write path. The engine's own calls
make that discipline available rather than enforce it: each is one
object's worth of work, serialised by the caller exactly as every
other call in §7 is.

**The pass is also where a condemned slot's grains come back.** *The
engine's half is built — `bmpassbegin`, `bmpassfold`, `bmpassend` and
`bmpassabort`; the pass that drives them is the server's, like the
scrub above, and is not.* The pass already reads every live entry's
extent map, so a walk that accumulates those grains into a **shadow
bitmap** and swaps it in costs almost no I/O beyond the pass itself,
and it takes the takedown out of the answer. It breaks no §7 rule.
The bitmap has exactly two mutators after start, the mark and the
clear of §6's allocator, and both are already called under `qlstate`
at apply time, so a **write barrier** that mirrors each into the
shadow while a pass is live keeps the two copies current. The map
reads happen outside the lock under `qlemap`'s pin, as every other
map read does (§7 rules 1 and 2), and the per-slot hold is an entry
copy plus at most `nblkmax` bit-sets — tens of microseconds, far
under §9's ~23 ms enumeration hold. The **swap goes page by page**
under `qlstate`, installing and dirtying only the pages that differ:
§5 step 11's rebuild dirties every page, which on a serving store is
§2.5's 32 MiB and ~2036 writes and would turn §6's `ckwaitms` wait
into a spurious `disk full`. Chunking is safe because the barrier
keeps both copies current until the last page lands. The staged set
needs no barrier — a staged grain carries no bitmap bit (§6) — and
needs no term in the free count either: `grainfree` moves by what
each installed page changed, and a staged grain is clear in the old
page and in the new one, so the difference passes it over. Recounting
the whole bitmap at the end instead would be one `qlstate` hold
proportional to the disk, which is the hold chunking the swap exists
to avoid; D24 has the cost.

**What a fold folds, and what it reclaims.** A free slot folds to
nothing; so does a slot §5 step 10 condemned, and that is the whole
reclaim — its map is the damage, so the grains it named are named by
nothing the store may believe, and a swap that never marked them
returns them. §3.6's `op=full` over a condemned slot leaks the old
grains exactly as a delete does and clears the entry's condemnation,
so the store stops remembering the slot was ever condemned; that
costs the walk nothing, because it folds what the live maps say now
and the rebuilt map is what the entry names either way. A map whose
entry fails its checksum is condemned by the fold that read it, as it
is by every other reader of a map (§5 step 10) — but only while the
entry still names the map those bytes came from. A stamp that moved
sends the fold round again instead, because §3.6's `op=full` may have
published a fresh map over the damage while the fold held the old
bytes, and condemning on those would condemn the repair.

**A fold's re-reads are bounded.** A slot whose stamp moved under the
fold is read again with a fresh pin, eight times (policy). Past that
the fold takes the grains off the pinned entry under `qlstate`
itself, which is a memory read under the lock the apply mutates the
map under and not a device read; the one case those bytes cannot
answer is an entry that has stopped naming the extent-map slot the
round pinned — freed, condemned, or moved to another slot — and that
starts over like any other round. Every round counts against the
bound, the fallback's own included, under a second ceiling of sixteen
rounds in all (policy): a fold that reaches it folds nothing and
answers `the map will not hold still`, which the driver retries on
that slot rather than reading as a failed pass. Without the bound one
object under a continuous write rate starves the walk on its slot for
ever.

`/status` reports what a pass is doing: `bmpass=` whether one is
live, `bmfolded=` the completed folds of live slots, `bmfolding=` the
folds in flight, `bmreread=` the re-reads the stamp forced and
`bmswapped=` the pages the last swap installed. §6's `grainleak=` is
what says how much a pass would return, and so what says when the
work is worth doing.

**The engine enforces the walk's coverage.** The swap frees every
grain the shadow does not mark, so a shadow the walk did not finish
frees grains a live map still names — the one way this mechanism can
destroy data, and a rate-limited walk over a serving store is exactly
where a slot gets missed. The pass therefore keeps a mark per index
slot, and `bmpassend` refuses unless every `live` slot carries one
and no fold is in flight at the moment it looks. A fold begun after
that check is accepted: it runs beside the swap and writes only into
the shadow, so it contributes nothing to the pages already installed,
and it loses no bit either — coverage passed, so every grain its slot
named then is in the shadow, and every grain that slot has named
since went into the live bitmap through the barrier. A fold sets the
mark for the slot it completes. So does an apply whose record
rebuilds the slot's map whole — an `Oslot` record (§2.7), a record
into a slot that was free,
or one that leaves the slot naming no block — because every grain
such a record names goes through the barrier; an ordinary write does
not, since the blocks it leaves alone are still the old map's. A
tombstone needs no mark of its own: its `Eobj` carries `len=0`, an
empty `nmap` and `emapslot=0`, so it names no grain, and neither does
a free slot. A refused end installs nothing and leaves the pass live,
so the driver folds the slots the refusal names and ends again; a
fold that could not read a map says so, and that slot is the driver's
to fold again rather than a reason to drop the pass.

**What the swap leaves of `grainleak`.** A leak recorded while the
pass was live, in a slot the pass had already folded, is one the swap
does *not* return: the fold put those grains in the shadow before the
condemnation, so the swap installs them marked and named by nothing.
The end leaves that much of the count standing and discharges the
rest, and the next pass — which folds the condemned slot to nothing —
is what returns them. A slot condemned after its fold and not yet
deleted is the same shape without the count: its grains stay marked
through the swap, because the fold put them in the shadow, and
`grainleak` does not name them, because by D18 the leak is made by
the delete — which then counts it under the pass, where the rule
above keeps it, or after the pass has ended, where D18's upper bound
is what the count is: a delete of a condemned slot whose grains a
completed pass has already returned counts them once more, and the
next pass — which folds that slot to nothing — is what takes the
count back down.

**A pass and `storeclose`.** Every call on a closed store is
undefined (D16), so a pass MUST be ended or aborted before one. A
pass still live when `storeclose` runs is aborted by it — the shadow
is the store's memory and goes with the rest — which is the engine
tidying up after a caller rather than a way to leave a pass open: a
call in flight in another proc when the close runs is undefined
exactly as any other call in flight is, a swap among them, and what
the bitmap holds afterwards is however many of its pages had landed.

**An abort under a swap does nothing.** The swap drops `qlstate`
between pages, so an abort landing in that gap would free the shadow
out from under a half-installed bitmap and leave §6's free count
moved by the pages that did land — the one state this mechanism has
no name for. The end is about to drop the pass itself, so the abort
has nothing left to do, which is what lets it stay a call that cannot
fail.

What the walk needs, and what the index entry carries, is a
**per-slot generation stamp**. The four-tuple is not a sufficient
validation of an entry re-read outside `qlstate`: the block repair
above and the `corrupt`-flag commit both publish with the four-tuple
unchanged while the map changes (§2.7), so a walk that compared keys
would OR in a stale map's grains. The stamp is in memory only, is
bumped by every apply that creates, frees or re-states a slot or
changes its map, and by the condemnation that stops a slot's map
being read at all, and it outlives a slot's release — a released slot
is stamped one past what it held, so a slot freed and re-created
under a walk cannot present the stamp the walk recorded. §16a(11)
names the same counter for §9's enumeration, and this is that
counter; what that enumeration would still owe is the chunked scan
itself, which is not built. D24 records why the stamp rather than the
alternative to it.

**What a corrupt object answers to `op=meta`.** No available answer
is right: reporting the key claims an arbitration position layer-a
§1.3 forbids a failing copy, `absent=1` is a lie that layer-a §1.5
counts as a positive confirmation for tombstone discard, and an
error is not a response at all. The third of those is the one that
cannot be shipped: layer-a §5.2's currency check requires an
`op=meta` **response** from every witness that is `up=yes` or
`up=heal`, and a corrupt holder is neither `up=no` nor absent, so an
error leaves the check permanently incompletable — every client read
and write of the object answers `not ready`, cluster-wide, forever,
on one media fault, with a good copy on the primary and a repairable
one on the holder. So this store answers, and the answer is the
ordinary `meta` line for the key it holds with **`corrupt=1`
appended**, which is the grammar layer-a §5.6 defines. shoal's own
callers honour that rule: the response satisfies the currency check
and contributes no key, so it loses arbitration against everything
including absence. *The `/rpc` surface that carries it is the
server's and is not built yet; what the engine answers is the flag,
through `objstat` and the cursor.*

**The repair path.** Because the check completes, the object has a
serving primary again, and that primary does what layer-a §1.3
prescribes for a holder whose key already equals its own: it pushes
`op=full force=1` at an equal key to the corrupt holder, replacing
the whole object without bumping the key. **That commit clears the
flag**, and the object leaves `/lost` with it. Every block the commit
names was staged from bytes checked against the sender's `dcsum` and
the whole against its `csum` (layer-a §5.5), and the digests were
computed here from those same bytes, so the verify that would run
next finds every block matching by construction: there is nothing
left for the flag to describe. Leaving it set until a scrub came
round would keep a copy that is now whole out of arbitration for as
long as a full pass takes — `scrubdays`, days — and, because a copy
with no key to defend takes a push at any key (§3.6), would go on
accepting a *lower*-keyed push for exactly as long. If the corrupt
copy is the only copy, nothing repairs it and layer-a §7.5's `object
lost` is the honest outcome.

**The repair of a corrupt holder that outranks the winner.** A
corrupt holder whose own stored key is *greater* than the winner's —
it committed `(E, ver+1)` and the content then went bad while the
primary kept `(E, ver)` — contributes no key, so the primary wins
arbitration at the lower key and its `op=full force=1` arrives as
neither greater nor equal. Layer-a §5.5 accepts that push: a receiver
whose own copy fails local verification treats it as absent for the
comparison and takes the push at any key (D14). §3.6's `final=1`
comparison is where the exemption lives and it applies it: a stage
committed against a copy whose `corrupt` flag is set is not compared
at all. Because the commit clears the flag, the exemption ends with
the push that used it, and the next push is compared like any other —
a second one at a lower key is refused `stale version`.

A commit that does not advance the key is a first-class case in this
store, and there are three of them: block repair, whole-object
`op=full force=1`, and the `corrupt` flag itself. All go through the
same `Eobj` path; nothing in the format assumes a commit bumps a
version.

## 9. In-memory index and directory snapshots

*Policy.*

The in-memory index is an array of `nslots` entries plus the oids
plus a hash table. Per entry:

    qidpath 8, len 8, ver 8, wepoch 8, mtime 8, csum 32,
    cur 4, oidoff 4, emapslot 4, grain0 4, dig0 16,
    oidlen 1, state 1, flags 1, hashnext 4

which is 128 bytes rounded. There is no lock in the entry: §7's
queues serialise per object, so the 32 bytes an `RWLock` costs on
amd64 — on every slot, occupied or not — are not spent. The oids
average ~24 bytes an object and are allocated one at a time, so a
discarded tombstone gives its bytes back; the hash table is 2^19
`u32` buckets with chaining through `hashnext`.

| objects | index | oids | buckets | total |
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
repeated writes to one object free; §7's `qlemap` is the lock over
it, and a miss is served once for every proc that wants the same map.
One read of the 64 KiB class is ~300 µs against an 8.75 ms commit,
3% overhead; §16a asks for it to be measured under the striping
workload.

**Snapshot-at-open (R12).** Layer-a §2.2 makes snapshot-at-open a
MUST for `/status`, `/map`, `/dirty`, `/stale`, `/lost` and `/jobs`
and a SHOULD for `/obj` and `/tombs`. The six MUSTs are small — a
few hundred lines at the envelope — and are rendered into a buffer
at open, which is the one-line implementation.

For `/obj`, `/tombs` and `/advert` an open takes the vector of
`{u32 slot, u64 qidpath}` of every entry whose state it asked for —
live for `/obj`, tomb for `/tombs`, both for `/advert` — and holds no
lock once it returns. That is 12 bytes an entry, **3.1 MB at 2.6·10^5
objects and 12 MB at `nslots = 2^20`**, plus the slack below — a
sixteenth and 16 entries, so 13 MB rather than 12 at `nslots = 2^20`
— held as two parallel arrays rather than one array of a struct,
because a `{u32, u64}` struct is 16 bytes on amd64 and the 4 in every
16 buys nothing. It takes
**two** holds of `qlstate` (§7) to do that on the quiet path, one per
step — the count under one, the 12 MB allocated outside any, the fill
under the next — and two more for every re-count the index forces.
The bound below costs no hold of its own: it is tested and taken
inside the first count's. So the holds an open takes are
`2·(fill attempts)` on any path that yields a snapshot, and
`2·Snaptries + 1` = **17** on the refusal, whose last hold gives the
bound's slot back. It can fail: see "What the open costs" below. The
vector is a list of names and not a reference the engine must honour:
a discard of an entry it names is neither refused nor delayed by it.

Entries are addressed **by position**, not by slot. That is what lets
a server map a `Tread` offset onto an entry and restart from 0 on a
re-read, which is how a Plan 9 directory read works. Entry *i* is
rendered from the *live* index under the same short hold of `qlstate`
§8's cursor takes, so no lock spans a caller's use of an entry: a
full walk of 2.6·10^5 entries is 2.6·10^5 short holds and blocks
`/status`, `/ctl` or `Tflush` for no longer than one of them (§7
rule 2). Nothing shifts under the reader, so no entry is skipped or
duplicated because of an index shift, and an entry created after the
open is not in the vector at all.

**What the open costs.** The walk stops at the count, so the cost is
the highest occupied slot and not `nslots`: a lightly-used 2^20 index
opens in microseconds, and an index whose last entry sits near slot
2^20 costs a scan of them all — measured at **~22 ns a slot on the
reference machine, ≈23 ms at `nslots = 2^20`** — under one hold of
`qlstate`. §2.3's 4× over-provision puts the envelope's 2.6·10^5
objects on an `nslots` near 10^6, so that is the envelope case rather
than a corner, and it is the one place in the store where a state
lock is held for milliseconds. §7 rule 2's letter holds — no device
call, flush wait or `Rendez` sleep is reachable under the hold, and
the vector's 12 MB is allocated *outside* it — but its number, an
8.4 ms write, is the thing this is comparable to rather than the
thing it avoids. §16(a) carries the chunked scan that would bound
the hold if T2 shows the 23 ms matters; the generation counter it
would scan under is built (§8), and the scan is not.

**The second hold, and the one way the open can fail.** The open
counts the index under `qlstate` — testing and taking the bound in
that same hold, the first time round — releases it, allocates, and
re-takes it to fill. The index moves in between as a matter of course, because
releasing `qlstate` puts the open *behind* every apply already queued
for it; this is the common case, not a corner. Three of the four
things that can have happened cost nothing:

- the count **shrank** — the fill walks until it has taken every
  matching entry and then stops, so an over-sized vector yields the
  true count and a *complete* snapshot;
- the count **grew** but still fits — the vector is allocated with
  **slack**, a sixteenth of the count and never fewer than 16
  entries, so ordinary churn needs no second attempt at all. The
  flat 16 is what T1's four-proc churn needs, since the net growth
  between the count and the fill is bounded by the procs holding an
  object absent rather than by the index's size; the sixteenth is
  that same hedge at a server's hundreds of procs. No churn at T1
  scale tells the two apart, so §13's `snapstale`/`snapshort` point
  is what does;
- the count did not move.

The fourth is an index that grew past the slack. The vector cannot
hold what the index now has, and a short vector is `objsnap=partial`,
which the engine does not have as an escape — so the open counts and
allocates again, up to `Snaptries` (8) times, and then **fails** with
`object snapshot: the index moved under 8 counts`. That is the only
failure `objsnapopen` has beyond the bound's `disk full` and out of
memory, and it is **pathological rather than ordinary**: it needs a
create rate that outruns a `malloc` eight times in a row. Sizing the
vector to the count with no slack is what makes it ordinary — that
refused about 6% of opens with four procs churning 1200 objects over
4096 slots, and with the slack the same churn refuses none. Nothing
is full and nothing is broken, so §3.7 makes it an internal-invariant
error carrying no §2.6 prefix: no §2.6 condition describes it, and
`not ready` is normative for handoff and the currency check (§5.2)
rather than for this. A server SHOULD retry the open once before
answering the client at all.

**An entry is gone under either of two conditions, and the second is
not a refinement of the first.** Either its slot's `qidpath` no
longer matches the vector's — the object was discarded and the slot
freed, or freed and handed to a different object — or the slot's
state is no longer one the snapshot asked for. §2.3 keeps an object's
`qid.path` across delete, tombstone and re-create, so an object
deleted after a `/obj` open still matches on `qidpath` and is now a
tombstone, which layer-a §2.2 says `/obj` MUST NOT list; a tombstone
created over after a `/tombs` open matches too and is now live. A
snapshot that tested `qidpath` alone would list both.

The store reports `objsnap=full` and never uses layer-a §2.2's
`objsnap=partial` escape: the engine takes the whole vector or
refuses the open. Reporting that field in `/status`, and the open
count beside it, is the server's half and waits on the 9P surface.

The cost is per open fid, so the store bounds how many snapshots may
be open at once (`objsnapmax`, policy, default 8) rather than growing
without limit; at 2^20 slots one fid is the 12 MB vector plus the
slack above — a sixteenth and 16 entries, so 12.75 MiB — and eight of
them are 102 MB, which is the number §14(9) says is answered for the
Layer B envelope and not for this design's own maximum. An open past
the bound answers `disk full: <n> object snapshots open, objsnapmax
<max>` — layer-a §2.6's `disk full`, whose entry covers any operation
that needs space, with the detail naming the space and the knob. It
is **not** an internal-invariant error (§3.7): a ninth open is a legal
call and not a caller's bug, so it is a refusal a client library may
key on; and
it is not a new prefix, because §2.6's set is normative and
prefix-free and this condition is reachable only by an admin listing
or the store's own reconcile and reclaim walks, on an enumeration
layer-a §2.2 already makes advisory. The detail is what stops it
misdirecting an operator, exactly as §10's map-too-big refusal
answers `disk full` and reports the sizes rather than answering
`bad map`.

The bound's test and its count are **one step under one hold** of
`qlstate` — the open's first count takes that hold anyway, and the
open takes its slot the moment it passes the bound, so two opens
racing cannot both find room. The count the refusal names is that
same count, read under the hold that tested it rather than as the
text is formatted, so it cannot come out below the maximum it is
being refused against. An open that then fails gives the slot back in
a hold of its own, so `/status` counts an open in flight along with
the opens that completed. A close releases the count.
**Giving the slot back is releasing a claim**, so a failing open's
bail-out carries the same free predicate an `objsnapclose` does: the
slot is the open's claim from the moment the bound is passed, and an
open in flight when `storeclose` runs is therefore the store's last
claim — `storeclose` finds the count non-zero, defers, and the
bail-out is what releases the memory.

**A snapshot MAY outlive `storeclose`.** The store's memory is not
released while one names it: `storeclose` stops the procs, then takes
`qlstate` and sets `closed`, and the `Store` is freed by whoever then
observes `closed && nobjsnap == 0` — `storeclose` itself when no
snapshot is open, and otherwise the **last** `objsnapclose`, which
tests the same predicate after giving its count back. `closed` *is*
the store's own reference; there is no second counter beside
`nobjsnap` to drift out of step with it. Reads through a snapshot
taken before the close then **fail** `store closed` (a local error,
no layer-a §2.6 prefix — nothing is full and nothing is broken, §3.7)
rather than answering *gone*: `objsnapent` tests `closed` as the
first statement inside the hold it already takes, ahead of the
`qidpath` and state tests that would otherwise answer *gone* for an
entry deleted or discarded since the open. That position is what
makes the refusal win over the lie, and it is a **contract refusal
and not a memory guard**: `storefree` is the only thing that frees
`s->idx`, and it cannot have run while this snapshot holds the
store, so the index underneath is whole and still matching. What the
test guards is that a store which has stopped serving — its procs
gone, its device the caller's to have closed already — answers
nothing out of what it happens to still hold in memory. `objsnapcount` still
answers, because it reads the handle and not the store. A store that
is condemned (§3.2) *and* closed answers `store condemned`, because
`storeserving` runs ahead of the hold; both are true, and neither is
the lie.

That is the whole of what may outlive the call. `storeclose` must give
up the store's reference only **after** its proc wait has returned —
setting `closed` earlier is what lets a concurrent last `objsnapclose`
free the `Store` while `storeclose` is still asleep inside it, so that
both free it. And a `Store*` is invalid the moment `storeclose` returns:
**only an `Objsnap` handle may outlive one**, and `objsnapopen`,
`dirtysnap`, `lostsnap`, `fullsyncsnap` and `storestat` on a closed
store are undefined exactly as they were. `objsnapopen` does refuse
`store closed` when it is reached on a store some other snapshot is
holding alive — it is inside the hold it takes anyway, on each of the
open's count passes, so a close landing while the open re-counts is seen
rather than skipped — but that is a courtesy inside an undefined call,
not a guarantee the pointer can keep; the other four are given no such
check, because advertising one there would promise what a dangling
pointer cannot deliver.

**The caller quiesces, then closes.** The obligation is not only
that a `Store*` is dead once `storeclose` returns; it is that no call
taking one may still be **in flight** when the close runs. A call
that holds no claim — `objsnapopen` before it reaches the bound, and
`dirtysnap`, `lostsnap`, `fullsyncsnap`, `storestat` and the object
API throughout — blocks on `qlstate` with nothing keeping the
`Store` alive, so if the last `objsnapclose` evaluates the free
predicate while one of them is queued on that very `QLock`, the
waiter wakes inside memory the free has released. The engine cannot
close that window: a waiter would have to be counted under the lock
it is waiting for. So the caller MUST have stopped issuing such
calls **before** it calls `storeclose`, and MUST make none after it.
What is allowed after the close is exactly `objsnapent`,
`objsnapcount` and `objsnapclose` on handles taken before it — the
three that carry a claim of their own. The shutdown order of the
server that will export this store (§8) follows from that
rule and not from taste: it stops accepting requests, lets the ones
in flight drain, waits for the work inside the engine that is not a
request at all — a pass proc a `ctl` verb started, which no request
count can see — and gives every fid still open the chance to hand
back what it is holding, which is where a stage has to be discarded,
a stage being none of the three. Only then does it close the store,
its surviving `/obj` fids holding the snapshots that are the one
thing the close leaves valid.

What this buys over simply deleting the fatal is more than the
refusal. A freed `Store` address can be handed straight back to the
next `storeopen` — §0's `Echange` close-and-reopen is precisely that
shape — and an old `/obj` fid would then render entries out of the
**new** store, which is worse than *gone*. `Objsnap.s` is a bare
pointer with no generation, so the refcount is what makes the
address-reuse confusion unreachable: the old `Store` cannot be freed
while a snapshot names it.

Closing a snapshot twice is **undefined**, exactly as freeing the
same pointer twice is, and for the same reason: the second call reads
a handle the first freed, whose first word — the store pointer
everything in the close goes through — the allocator has already
overwritten with its own free-list links. There is nothing a guard in
the close could test, so there is none.

**`/dirty` and `/lost` are copies rather than cursors.** Both sets are
bounded — by the dirty region (§2.6) and by what fails local
verification (§8) — so a copy taken under one hold of the lock that
guards each is the whole of what a renderer needs, and layer-a §2.2's
MUST for these two costs nothing. **`/dirty` is two copies**, because
layer-a §2.2's file is two kinds of line: the record lines, and one
`fullsync peer=<iid>` line per peer carrying §7.1's coarse flag. No
record names those peers — §2.6's exhaustion drop sets the flag on
exactly the peer whose fine-grained records it has just thrown away, so
the peers that most need the line are the ones with no record left to
name them — so the peer list is enumerated by a second call under the
same lock, and a renderer takes both. Today nothing clears the flag
until the reconcile pass exists and a peer is registered with it already
set, so that enumeration names every peer the store knows of; a peer it
has never seen has no line, which is what `storefullsync`'s answer of 1
for an unknown name already means. The `/dirty` record copy carries
every record's `(oid, peer, epoch)`; the `/lost` copy carries every slot
the membership list names, with that slot's oid and published record
beside it, so a renderer never goes back to an index the scrub has moved
under it. The slot-at-a-time accessor stays beside the copy: it is what
a walker that wants the live list uses.

The `/lost` copy names **every** slot the membership list names,
including §5 step 10's: an index entry that would not unpack leaves
its slot marked bad with its state still free, and that slot is on
the list and in `/status`'s `lost=` count. It has no oid to give —
the entry that would have carried one is the damage — so its copy
carries an oid length of 0 and an `Objinfo` that is the slot number,
state free and zeroes, and a renderer emits `slot=<n> kind=lost`
with no `oid=`. Layer-a §2.2 fixes only `oid=` and `kind=` for that
file, and does so for the fields a line *has*: a slot with no
readable oid has none to give, and the rest of the line is
implementation policy. Dropping it instead would make the copy and
`/status`'s own count disagree on precisely the damage `/lost`
exists for.

**The oid-ordered listing is not a snapshot.** layer-a §5.6's
`op=list` pages an instance's whole inventory — live and tomb — in
`oid` byte order, resuming after `after=`, and it is the peer
enumeration path: a reconcile pass runs it against every instance.
The snapshot above is the wrong shape for it three times over. It is
slot-ordered, so resuming after an `oid` through one would mean
sorting the whole index per page; it costs a vector of the whole
index; and its count is bounded by `objsnapmax`, which is sized for
the admin fids, so a reconcile paging through the inventory would
spend that bound against `/obj` and `/tombs`.

So `op=list` is served by a **k-smallest selection over a chunked
scan** instead: one pass of the slot array per page, holding `k`
entries of state, taking `qlstate` for `Listchunk` slots at a time
(policy, 256) and releasing it between chunks. Each entry the
selection keeps is copied — `oid` and `Objinfo` both — under the hold
it was seen in, so no entry mixes two states of one object. What
layer-a §5.6's "internally consistent" is read to require of a page,
and what it is read not to require, is §14(17). The whole point of
the chunking is that this walk, unlike the
snapshot open, is taken by every peer's reconcile rather than by an
operator's open, so it must not be the second place a state lock is
held for the 23 ms a full index costs.

**What a page costs** is measured for this scan rather than derived
from the ~22 ns a slot above, which prices the snapshot walk's
per-slot work and not this one's. A page is one pass of the slot
array: **O(`nslots`) comparisons for the candidates the selection
rejects — one each, whatever `k` is** — plus O(`k`) to place each
candidate it accepts, so `k` shows in the price through the
acceptances rather than through the pass. Rejecting in one comparison
is a test the selection makes before its insertion scan, against the
largest entry a full buffer holds; without it every slot past the
first `k` runs the insertion scan to completion and the per-slot cost
is O(`k`). Measured on the reference machine at `nslots` = 8192 over
8184 objects, for a page from the start of the inventory:

| `k` | per hold of 256 slots | per page | per hold without the test |
|---|---|---|---|
| 1 | ~35 µs | ~1.1 ms | ~37 µs |
| 256 | ~160 µs | ~5.1 ms | ~4.4 ms |
| 1024 | ~1.15 ms | ~37 ms | ~12.5 ms |

(A per-hold figure is a page divided by its 32 holds.) 256 slots a
hold keeps the hold under the 8.4 ms write §7 rule 2 measures holds
against across that range, while a chunk small enough to matter for
latency would pay a `qlock` round trip per handful of slots.

`k` is the caller's and the **engine puts no bound on it**: it is
bounded in practice by the server's clamp of layer-a §5.6's `n=` to
the negotiated `msize`, below. A caller that asks for a `k` far above
what a response can carry pays the table's right-hand column for a
page it cannot send.

What that costs is stated rather than hidden: an object created into
a chunk the scan has already passed is missed by that page, and one
created into a chunk ahead of it is included. layer-a §5.6 tolerates
exactly that — "a reconcile pass MUST tolerate an object created or
deleted between pages" — and the next pass or an `/advert` catches
it. Paging by `after=` therefore neither repeats nor skips an object
that stayed put across both pages, which is the contract §5.6 states.

An oid can be seen **twice** by one page: its slot is released
between two chunks and the id is re-created into a chunk the scan has
not reached. It is answered once, at one of the two renders, because
a page that answered it twice would not be ascending and would hand
the caller an `after=` it had already paged past.

**How the caller learns whether more follows** is an out-parameter
(policy): the scan counts the candidates above `after` it saw, and
answers `more` when that count exceeds what it returned. It is
answered to the page's own tolerance, since it is the same scan, and
within that tolerance it is one-sided: never falsely 0, because every
candidate the page did not answer is counted, and so a caller told 0
has the whole inventory above `after`. It can be a false 1, because
an oid the scan saw twice counts twice and is answered once: a page
that ends the inventory can still say `more`, and the cost is one
further page that answers nothing rather than an object the caller
stops short of. The
`lines=` and `more=` of §5.6's response line are the server's to
render, and so is clamping the requested `n=` to the negotiated
`msize`: the engine's `k` counts entries, not bytes.

§6's tombstone reclaim is the enumeration's first caller, and it is
the caller's walk rather than the engine's: the engine holds no
`tombdays` policy, because layer-a §3.1 makes `tombdays` a map-header
attribute. The caller opens a `/tombs` snapshot and tests each entry's
`mtime` against its own cutoff and the entry's `wepoch` against its
own map epoch. In the 9P server that caller is a background job of its
own, on a timer and under layer-a §2.5's `reclaim` verb (§14(39)); it
holds the snapshot for the whole walk, so the walk sees the
tombstones the open saw and no others. It **counts** what passes both
and removes nothing
(§14(31)): layer-a §1.5's third condition has nothing to answer it
while there is no peer client. The discard is addressed by the
entry's **own key** and not by its slot — that is what `objdiscard`
takes and what §6's receiver checks enforce, so a discard refuses a
record that is not the one the walk inspected instead of removing
whatever the slot came to hold — and it is what the walk will use
when the replication surface brings condition 1.

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
either after format, so they are identical and the start-time rule is
"take either valid copy, refuse if neither" — there is no generation
to compare and nothing to choose between.

The monitor's partition is committed with the same raw flush the
object store uses, so §2.1's deployment rule applies to it: it MUST
be on an `sd` unit no object-store instance is serving.

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

**What a format leaves.** `shoalmonfmt` zeroes both header sectors
and flushes before it writes anything else, for the reason §12 gives
`shoalfmt`: a format cut short must leave no valid header rather than
a valid one locating slots that were never written — which, after a
reformat at a different `slotsz`, would be the previous store's
header over this one's bytes. It then writes **both current-map slots
as valid empty slots** — `len=0`, `seq=0`, `epoch=0` — and the header
sector of every history slot as zeroes, and writes the two header
copies last, with a flush before the format is reported complete.

The empty current slots are what make the third clause of the commit
rule mean anything. §2.2's "if neither is valid, refuse" is copied
into step 2 below, and nothing else would give a freshly formatted
store a valid slot, so a fresh store and a doubly damaged one would be
indistinguishable at the first commit. With them, **neither current
slot valid always means damage**, and it is refused at start and at
commit alike. Zeroing the history headers is the other half: no slot
an earlier format on the same bytes left behind survives as valid.
"The store holds no map" is then the chosen current slot carrying
`len=0`, and nothing else — an accessor answers that as no map rather
than as an empty map text.

**Commit.** In this order:

1. Write the history ring slot for the new epoch, **stamped with the
   `seq` step 2 is about to carry**, and flush. The ring is not
   optional and this is not last: the slot the ring is writing is the
   *newest* entry, and at the next publish that entry is the `E−1`
   layer-a §8.2 requires the monitor to keep and layer-a §5.2 clause
   2 depends on. Writing it first means a torn ring write damages
   only the map being published, which fails the commit, rather than
   the previous map, which nothing else can supply. If it cannot be
   written, the commit fails. The slot it overwrites is the phantom
   below if there is one, then any invalid slot, and failing both the
   valid slot with the lowest `seq` — one `seq` space for the whole
   store is what makes "oldest" and "newer than the current map" both
   well defined.
2. Write the current-map slot, choosing by the same three-clause rule
   as §2.2: if exactly one slot is valid, write the invalid one; if
   both are valid, write the one with the lower `seq`; if neither is
   valid, refuse. `seq` is `max(valid seq) + 1` over both slots and
   the ring. Flush.

The third clause is **§2.2's symmetry and not a path**: a store the
start accepted has a valid current slot, and after one failed write
here the selection re-targets the slot it has just invalidated, so
the valid one is never written and cannot become invalid. It stays
because the rule it states is the rule, and the code says it is
unreachable where it makes it.

**Each slot is read back after its flush** and checked — magic,
`vers`, `len` within the slot, the checksum over `secsz+len`, and the
`seq`, `len` and `epoch` just written. It is for exactly one fault: a
device that **reports a successful write, acknowledges the flush after
it, and does not hold the bytes at the offset the write named** —
nothing landed, or part of it did. That is the empty or partial case
of the torn write §3.2 already allows the device, and neither return
value says anything about it; a read through the same device is the
only thing that does.

Both slots are read back, and the **current-map** slot is the one that
pays for it. A ring slot the platter does not hold costs layer-a
§8.2's retention MUST: position 0 stops being the current map, the
entry the next publish owes as `E−1` is missing, and an instance falls
back to layer-a §5.2 clause 2's substitution — correct, and wider than
it needs to be. A **current** slot the platter does not hold costs the
cluster. The monitor has acknowledged epoch `E`, every instance has
adopted it and made `epochhigh = E` durable (layer-a §6.3), and this
monitor's next start serves `E−1`; every instance then rejects the map
as an epoch regression and stays fenced until an operator runs
`forceepoch` (layer-a §8.3). No later publish, restart or crash rule
recovers it, because the fault is that the acknowledgement was given.

A slot that **reads back and is not the one written** says the map is
not durable, and the commit fails exactly as a failed write does:
step 1's victim stays invalid and a phantom, step 2's slot is left
invalid and the ring entry becomes a phantom, and nothing the monitor
serves claims a map the platter does not hold.

A read-back whose **read fails** is a different fact, and this store
does not confuse the two: a read that failed says nothing about the
write under it, so the slot may be on the platter. The read is
**retried once**; if it fails again the commit fails with the publish
declared *indeterminate* — written and flushed, and not readable
back. The in-memory state is then made safe against whatever landed:
the slot is not served, its `seq` is **spent** so that the next
commit's is above it — a retry outranks anything that did land, and
no two entries share one `seq` — and a ring victim is phantom-first
for reuse besides. A failed commit therefore means **the map is not
served by this monitor process**. It may be on the platter, and then
the next open serves it; that is exactly the outcome of a crash
between step 2's flush and the monitor's acknowledgement, which the
crash argument above already covers, because layer-a §8.2 requires
durable-before-ack and not ack-iff-durable. The operator's retry
publishes above it either way.

The read-back is **two device reads per slot** — the header sector, then
the text — and not one, because it goes through the same slot reader the
start does, which must bounds-check `len` before it reads the `len`
bytes that field names (a torn length field must not drive a read past
the slot). It is one read for a `len = 0` map, the empty map a fresh
format leaves in both current slots (§2.2's tie): there are no text
bytes to read and the reader does not ask for any. Reading
`roundup(secsz+len, secsz)` in one request would be possible here, where
`len` is known, at the price of a second reader; the four extra requests
a commit makes are noise beside its two flushes, so it keeps the one
reader.

What the read-back proves is that the device **accepted** the bytes,
not that they are on the platter: the read is answered by the same
write cache the flush was meant to drain. So it catches a device that
loses the write *before* acknowledging the flush, which is the case
it exists for. A device that acknowledges a flush and then loses the
bytes anyway is outside this store's model, exactly as it is outside
the object store's: §13's simulated disk makes durability after a
flush its contract, and §3.2's `-w` assertion is what an operator
gives for a unit whose flush does not reach the platter.

Nothing on the target platform is known to drop an accepted write
this way: `docs/platform/9front-storage.md` §5 has `devsd` do no
caching of its own and issue one request per `pwrite`, §6 lost none
of 7519 acknowledged raw writes and tore none, and the one lying
mechanism that document names — the legacy IDE driver's faked
`SYNCHRONIZE CACHE` — is precisely the case a read-back cannot catch.
The monitor pays the four reads anyway and §3.2's log does not, and
the difference is not the device: an instance that loses an
acknowledged log record is one of `R` copies and layer-a repairs it by
arbitration and heal, while the monitor's map is the cluster's only
copy (layer-a §6.5). The guard is bought for the single copy.

**Choose on start:** read both current-map slots, take the valid one
with the greater `seq`; two valid slots at equal `seq` — which is what
a fresh format leaves — are §2.2's tie too, so the start is slot 0 and
the next write is slot 1. A torn write to the slot being written fails
its checksum and the other slot is untouched, so the previous map
survives; that is the entire crash argument, and it is §2.2's rule
again — including the clause that keeps a torn slot from steering the
next write onto the only good one.

**Then erase the phantom.** A crash between the two steps leaves a
history entry for an epoch that was never published: the current map
is still `E−1`, so the monitor's next publish is epoch `E` again with
different content — recomputed `up` marks, different stale records —
and a ring holding two entries claiming `E`, one of which never
existed. Served the phantom, an instance computing `W(o)` from
`/maps/<E−1>` (layer-a §5.2 clause 2) would complete a currency check
against a placement that never existed. The `seq` stamp is what makes
this decidable without a durable ring cursor: at start the monitor
**ignores, and marks reusable, every history slot whose `seq` exceeds
the chosen current slot's**. Exactly the entries written by publishes
that did not complete are erased, and no committed history is. The
start itself writes nothing: a phantom is erased by the write that
reuses its slot, which is what lets a monitor store be opened on a
device opened read-only.

A phantom is the **first** slot the next commit overwrites, ahead of
an invalid one, and that ordering is what bounds the disk to one of
them: a phantom's `seq` is above the current map's only until the next
map is published, after which the same bytes would read back as an
ordinary entry for a map that never existed. Consuming it at the next
commit — which either completes, making the entry real, or does not,
making it the phantom again — is what keeps both halves of the rule
above true.

**A ring write that fails leaves its victim a phantom.** The slot is
invalid in memory, but the platter was never told: a write that landed
nothing leaves the victim's own bytes there, and a victim that was a
phantom is still a phantom on the disk. So the failed write marks the
slot *both* invalid and a phantom, which keeps it first in line for
reuse whatever it now holds — its old bytes, or the half of the new
map that did land. Dropping the mark instead would send the next
commit's victim search past it to some other slot, the next published
map would raise `seq` above it, and the following start would read the
never-published map back as ordinary history: precisely the outcome
the rule above exists to prevent.

There is a third thing the victim can hold, and the failure path
**reads the slot once to find out**. When the ring is full the victim
is not an invalid slot or a phantom but an ordinary committed entry,
and a write that landed nothing left it there intact. An entry read
back valid whose `seq` is not above the current map's is history this
store can still answer, so it goes back into memory as it is found:
the live store keeps answering that epoch and counts no phantom it
does not hold, instead of both until the next open. Three kinds of
slot take the invalid-and-phantom mark above instead: one that reads
back unreadable or invalid; one that reads back **valid at a `seq`
above the current map's**, which is a phantom on the platter and the
case the mark exists for, since leaving it as history would let the
next published map raise `seq` over a map that was never published;
and one whose write was indeterminate, where the read-back has
already failed twice and this read is not attempted at all.

**The store never compares epochs.** It records the epoch it is given
beside the map and orders nothing by it: layer-a §8.3's `forceepoch`
and §8.6's rebuild path can each legitimately publish an epoch that is
not above the last, so a rule here would refuse a recovery the cluster
depends on. `seq` is the only order. Where two ring entries carry one
epoch, a lookup by epoch answers the one with the greater `seq`, which
is the later publish.

**A map that does not fit.** If `secsz + len` exceeds `slotsz` the
commit fails with `disk full` (layer-a §2.6: any operation that must
store bytes may return it), and the monitor reports the sizes in
`/status`. It is not `bad map`: the text is valid, the partition is
too small, and telling an operator the map is malformed would send
them to the wrong place.

**Crash points.** §13's named points inside a commit, in the order a
commit reaches them: `monhist`, after the history slot's write returns
and before its flush; `monhistflush`, after that flush returns and
before the current slot's write — the phantom window above; and
`moncur`, after the current slot's write returns and before its flush.
A format has one of its own, `monfmthdr`, after the flush that zeroes
both header sectors and before any other write: a crash there is the
durable state the prologue above exists to leave, and the open must
refuse it.

**Sizing.** A map at twelve instances is a few KiB; `slotsz` 65536 is
a twentyfold margin and a whole number of 16 KiB units. With
`retain=8` the store needs 2 header sectors + 10 slots ≈ 640 KiB;
the partition is sized at 4 MiB, and `shoalmonfmt` refuses less than
1 MiB. `retain` MUST be at least 2 — layer-a §5.2 clause 2 reads
epoch `E−1`, so one history slot is a floor rather than a preference
— and `shoalmonfmt -R` refuses less.

**Cost.** One 16 KiB write plus one flush per slot written: **8.6 ms**
for the current-map slot, and the same again for the history slot
that precedes it, so a publish is ~17 ms whether it carries a
placement change or a single `stale` mark. The read-back of each slot
is two reads — the checksum's sector, then `len` bytes rounded up, or
the sector alone for a `len = 0` map — so a commit issues four beside
its two flushes, which is noise beside them. That is what
makes layer-a §5.4 step 5a affordable — the alternative
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
commit that wraps the log adds one 512-byte write and no extra
flush — the wrap record rides before the batch's post-flush (§2.7).

| Operation | Reads | Data writes | Commit | Total |
|---|---|---|---|---|
| create, delete, truncate to a block boundary | map: 0 inline, 0.3 ms out-of-line, 0 cached | none | 1 | **~9 ms** |
| 16 KiB write, block-aligned | 1 map | 1 grain: 8.4 ms + 0.27 ms hash | 1 | **~17.5 ms** |
| 4 KiB write into a populated block | 1 map + 1 grain (0.24 ms) + 0.27 ms hash | 1 grain | 1 | **~18 ms** |
| 4 KiB write into a hole | 1 map | 1 grain | 1 | **~17.5 ms** |
| 16 MiB `op=full` | 1 map | 1024 grains | 1 (3 writes) | **8.6 s serial, ~2.5 s at 8-way (409/s)** |
| scrub-verify one 16 MiB object | 16 MiB, 200 ms | none | none | **~0.5 s, hash-bound** |
| incremental checkpoint | none | dirty pages only: 8.4 ms per index or bitmap page, 16.8 ms per extent-map entry | 1 superblock | proportional to dirty state |

Three things this table says that are worth saying in words.

**A 4 KiB client write costs a 16 KiB grain write.** Copy-on-write at
`blksz` granularity is what buys the atomicity of §3, and it makes a
small write cost four times its size. The cluster default puts
`blksz` at the device's `Wunit`, which is what keeps that factor at
four rather than sixteen and makes every block write exactly one
device request, so there is no "issue the grain four ways" question
to answer and no proc pool to size for it. A cluster that chose a
larger `blksz` would pay `ceil(blksz/Wunit)` requests per grain and
every row below scales with it; the table is the default's. The
workload this store is built for — Layer B striping through an
`msize`-sized 9P path — writes whole blocks.

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

*Policy.* Three commands under `cmd/`, each an `mkone` directory
listed in `cmd/mkfile`'s `DIRS`. All three are built; the two flags of
`shoalck` that write or read object content are marked below.

**Where the code lives, and why the test tier decides it.** Every
T1 case in §13 drives format, commit, replay, checkpoint, allocation
and enumeration against the simulated disk, and `AGENTS.md` requires
a T1 test to be a C program in `test/` linking `libshoal`. So the
store engine — the device vtable (§0), the on-disk structures, the
write path, the log and its apply function, replay, the index, the
allocator and the enumeration snapshot — lives in `lib/libshoal.a`
behind `lib/shoal.h`. The same argument carries one step further: the
9P surface layer-a §2 defines — attach, the file tree, the `Reqqueue`
pool and the procs of §7, `Tflush`, the ctl framework, start-up and
shutdown — is itself what T1 has to drive, so it lives in
`srv/libshoalsrv.a` behind `srv/srv.h`, with `srv/dat.h` and
`srv/fns.h` private to it. What is left in `cmd/shoalsrv` is argument
parsing, opening the device and reading the map file, posting the
service, and the trigger of the shutdown sequence; the three tools
below are thin front ends over `lib/` alone. `lib/libshoal.a` depends
on neither lib9p nor libthread, and must not come to: that
independence is what lets one engine run under a plain-libc T1
program and under the libthread server (§7). This is a real
constraint on the code layout rather than a preference, and it is
expensive to undo once the engine has grown roots in a command.

**A path is an sd(3) partition when the directory holding it is an
sd unit's, and a plain file otherwise.** What is asked is whether
that directory holds the unit's own `ctl` and `raw` files, not how
the path is spelled: the kernel binds `#S` wherever the namespace
puts it, and in a `cpu` or `rcpu` namespace `#S/sdF0/shoal` is often
the only way to name a partition at all. A partition mistaken for a
file is silent and expensive — the sector size falls back to the
default, so every write becomes a read-modify-write of the sectors
it touches (§0), and there is no flush channel to refuse to open —
so the question is settled by what is there. All three take either
kind, and the classification is not a preference: a path the
directory says is a partition is opened as one, and a failure there
— no `ctl`, no permission, a raw channel that will not open — is
reported and the tool exits rather than retrying it as a file, since
falling back would be the silent misreading the question exists to
prevent. A file image is not a deployment target — D13 makes that a raw
partition — but it is what lets an operator inspect a copy, and it
is what lets the T1 cases of §13 drive format and check with no disk
at all. `shoalfmt -z` and `shoalmonfmt -z` size such an image;
nothing else in any of them depends on which kind of device it was
given, because §0's vtable is the only thing they call.

**A `-z` never runs ahead of the refusal that would have stopped the
run** — of *any* of them, not only the reformat guard. Resizing an
image truncates what it already holds, so both commands run in one
order:

1. validate the flags;
2. open the image **at its own length** — a first open that fails for
   any reason but the path not being there is the end of the run, and
   never a fall-through to the create `-z` would do, because a path
   that exists and will not open read-write would be truncated by it
   with no guard run at all;
3. run **both reformat guards**: an image carrying a valid
   object-store superblock (§2.2) or a valid monitor header (§10) is
   a store, and **either command refuses to format over — or shorten
   — either kind without `-r`**;
4. size the geometry against the length **`-z` asks for**, not the
   one the image has: `shoalfmt`'s `geometry`, `shoalmonfmt`'s
   `monfmtcheck`;
5. only then resize, and format.

A run refused at any step leaves the file **byte-identical, its
length included**, and the lengths a refusal quotes are the file's
own rather than the sector-rounded device size. `-z` on a path with
no file there creates it at that size, where there is nothing to
destroy — and the hint that `-z` is what sizes a new image belongs to
that refusal alone, not to a path that is there and will not open. A
run that creates the image at step 2 and is then refused at step 3 or
4 **removes what it created**, so that path holds no file again: the
byte-identical rule is kept for a path that had a file, and for a
path that had none the only length any refusal could leave behind is
the one the run was refused for. An operator who corrects the flag
and re-runs without `-z` then gets the same "no image there yet"
refusal rather than a format at the wrong length. `-z` against an
`sd` partition is refused by both.

**`shoalfmt`** — format or ream an object-store partition.

    shoalfmt [-rw] [-b blksz] [-o objmax] [-c csumalg] [-n nslots]
             [-e nemap] [-d ndirty] [-L logbytes] [-u uuid]
             [-z size] /dev/sdXX/name

Zeroes **both superblock sectors and flushes before it writes
anything else**, so that a format or a ream cut short leaves no valid
superblock rather than a valid one naming regions that were never
written or have just been half overwritten. That invalidation is the
half of the ordering `-r` needs: writing the superblocks last makes a
*first* format safe, but a ream interrupted before them would
otherwise leave the **previous** instance's superblocks valid — its
`uuid`, its `ckseq`/`cklogoff` over a log that has just been zeroed,
its `qidnext` and its `epochhigh` — which defeats the very thing `-r`
exists to make loud, and which `shoalck` would report as a healthy
store.

It then zeroes the log; writes every index entry and every dirty
record as a valid **free** record, and every bitmap page with a valid
header at `ckseq = 0` — sixteen zero bytes are not the checksum of a
zeroed record, so a region merely zeroed would make §5 step 10
condemn every slot and §5 step 5 report `bmaprebuild=yes`; sets bit 0
of the bitmap (grain 0 is never allocatable); and writes both
superblocks **last**, copy 0 at `gen = 0` and copy 1 at `gen = 1`.

The extent-map region is **not** zeroed. §2.4 puts the zeroing of an
entry on the commit that allocates its slot, precisely because a
released entry's bytes are not to be trusted; no reader reaches a
slot no live index entry claims, and at format that is every slot. It
is also 5.1 GiB of the 5.5 GiB of metadata on a 4 TB disk, which at
8.4 ms per `Wunit` write is about 47 minutes of an otherwise
50-minute format.

It prints the geometry it chose — including how many multi-block
objects `nemap` supports, which is the number an operator needs to
size a workload that is not Layer B's. It
generates a random `uuid` unless given one, and **refuses a partition
that already carries a valid superblock unless `-r`** — reaming a
disk destroys an instance's identity, and layer-a §1.5 makes that a
reformat-before-rejoin event, so it should take a flag. It refuses a
partition that carries a valid **monitor** header (§10) unless `-r`
for the same reason: that is a store too, and one this command would
destroy just as completely. It refuses a
geometry whose maximal `Eobj` record exceeds an eighth of the log
region, one whose `ngrains` reaches 2^32, one whose `nblkmax`
(`objmax`/`blksz`) reaches 2^32, one whose `blksz` is not a power of
two between `secsz` and §2.1's 1 MiB ceiling, one whose `emapsz`
(`24 + 20*nblkmax`, §2.4) reaches 2^32, and one whose
log region does not fit the `u32` a record length is computed in
(§2.7); and it warns
when the metadata it has sized comes to more than 1% of the
partition. It does **not** refuse a `blksz` above the device's write
unit: that unit is the device's property and `blksz` is the format's
(§0, §2.1). `-w` is §3.2's operator assertion, which is what lets it
format a unit whose raw channel it cannot open.

**`shoalck`** — inspect and check. Every flag but `-R` reads and
never writes, and the device is then opened read-only so the kernel
enforces that rather than the code promising it — which also lets it
run against a disk its user may only read. Such a run opens no raw
channel, so on an sd unit it reports the device's flush channel as
*not examined* rather than claiming the operator asserted
write-through. A file image has no flush channel to examine at all
and is reported as *none*, read-only or not.

    shoalck [-lqvRw] [-o oid] /dev/sdXX/name

Default: print both superblocks and which one §2.2's three clauses
select, which copy the next update would write and under which
clause; the geometry and the region table; the log's record count and
sequence range from the checkpoint mark; slot, extent-map-slot and
grain occupancy, and the dirty-record count. It verifies every index
entry's checksum and every bitmap page's, reports `Pmax` and whether
the bitmap is stamped ahead of the superblock, and cross-checks the
bitmap against the grains every live map references, scanning each
object to `nblk` and not beyond; it exits non-zero on any
inconsistency. A slot §5 step 10 condemned is reported twice over,
and both reports are the state of the disk rather than a second
fault: its extent-map entry does not unpack, so the cross-check finds
nothing referencing the grains it held and calls them marked and
unreferenced. They stay that way — a tombstone over such a slot frees
none of them either (§6) — until a rebuild recomputes the bitmap from
the maps that do unpack: `-R` here, or §8's online pass on a store
that is serving. `-l` dumps the log records and their entries; a second
`-l` dumps each `Eobj`'s block map. `-q` prints the problems and
nothing else. `-o` dumps one object's index entry and extent map.

**`-v` and `-R` work on the replayed state, and every pass above
works on the checkpoint.** The difference is not a refinement. §2.8
makes the log the durable authority for everything since `ckseq`, and
§3.5 defers a released grain's reuse only until the freeing commit's
flush has returned — so a grain freed by a committed-but-not-
checkpointed record may already hold another object's bytes.
Verifying an object against the checkpointed index would read those
bytes and report a mismatch on an object that is perfectly well; a
bitmap rebuilt from the checkpointed index would clear grains the log
has since handed out, and the checkpoint `-R` writes publishes a
`ckseq` past the records that would have corrected it. So both flags
replay the log first. `storeopen` with no `spawn` callback and no
checkpointer proc is that replay and nothing else: the engine makes
no proc and commits are synchronous in the caller. Every flag but
`-R` opens the device **read-only**, and such a run writes nothing at
all: §5 step 11's rebuild only marks pages dirty, and replay holds
the extent maps it applied in the cache instead of writing them back,
exactly as a live commit's are held. There is then no durability to
assert and no raw channel to want, and the flush mode stays as the
device reported it. `-R` takes the read-write open `shoalfmt` takes,
and there replay's closing write-back runs (§5 step 7), so a device
error under the extent-map region refuses the start and says so;
`storecheckpoint` is the write `-R` is for. The one thing a read-only
replay cannot do is spill the extent-map cache — §5 step 7's in-loop
write-back is how a log naming more maps than the cache holds gets
through — so a read-only replay that fills the cache is refused
naming *that*, and not as the bare write refusal the device would
answer with. Neither refusal is the log's damage, so neither is
wrapped in the remedy for that; the way past a full read-only cache
is a writable open, whose checkpoint materialises the maps, or a
larger cache.

**`-v`** verifies every object's content against its digests — §8's
verify, offline, over every slot rather than over one object. For
each live object it reports the mismatching block indices, whether
the digest array itself is suspect (`arraybad`), and whether the
entry is flagged `corrupt`; an object that fails is a problem and the
exit is non-zero. A tombstone holds no content, so it verifies
vacuously and is counted rather than read. An object that is flagged
`corrupt` and verifies clean is reported as information and not as a
problem: the flag is durable and it is §8's online scrub that clears
it, with a key-preserving `Eobj` this tool does not write. `-q`
prints the problems and nothing else.

**`-R`** rebuilds the free-grain bitmap from the live maps and rewrites
the checkpoint — the offline form of §5 step 11's automatic rebuild. A
page that fails its checksum is already rebuilt at every start (§2.5);
`-R` is for the page that is **valid and wrong**, which no start
repairs, and for the operator who wants the scan done now rather than at
the next one. It prints how many grains the on-disk bitmap left free and
how many the rebuild leaves, so what changed is visible — the first of
those numbers comes from the checker's own bitmap pass, so it is printed
only when every bitmap page was read and passed its checksum, and
otherwise the line says how many pages did not read **or** did not pass
their checksum — both are counted, and a page that reads cleanly and
fails its checksum is the commoner — and gives no number. It reports
`bmaprebuild` and any refusal from the store in the store's own words.
An extent-map entry that fails its own `csum128` is not rebuilt from:
every grain number in it is the damaged bytes', so §5 step 10 condemns
the slot and the rebuild skips its map. The **slot** stays out of the
allocator — `completemaps` counts a condemned slot as used — but the
**grains** the damaged map named are not marked and so return to the
free set, because nothing knows which they were. That is safe and it is
the only answer available: the copy is unrecoverable (§3.6, D14), the
grains it held are named by no readable structure, and holding an
unknown set of grains out of the allocator for ever would leak the disk
instead. Until such a rebuild runs they stay marked from the bitmap as
it was found, which is what §3.6 means by a condemned copy's grains
staying marked used until a rebuild. It opens the device read-write —
the open `shoalfmt` takes, with the flush channel, and `-w` as §3.2's
operator assertion for a unit whose raw channel will not open — so `-w`
without `-R` is refused rather than ignored. `-R` with `-v` rebuilds
first and then verifies. `-R` with `-o` is refused: `-o` dumps one
object, and a rebuild driven from one object's map would clear every
grain the rest of the store holds. The passes above run first and report
the bitmap they found, so a `-R` run that repairs a wrong bitmap still
exits non-zero on what it repaired; the run after it is the clean one.
`-R -v` can exit non-zero for either reason at once — the bitmap it
repaired, an object that failed its verify, or both — so the exit code
alone does not say which, and the report is what does.

**`shoalmonfmt`** — format a monitor map partition. §10 is the format
it writes.

    shoalmonfmt [-r] [-s slotsz] [-R retain] [-z size] /dev/sdXX/name

`-s` sets the slot size and `-R` the ring length, defaulting to §10's
65536 and 8. `-z` sizes a file image and has no default: a partition
carries its own length, and an image that already exists is formatted
at the length it has. Formatting an image that does not exist yet
therefore needs `-z`, and is refused without it. It prints the
geometry it chose — the two values, the sectors the header copies, the
current slots and the ring start at, and the bytes the format occupies
— the way `shoalfmt` prints its own.

It refuses a `slotsz` that is not a multiple of the device's sector
or is under two sectors, a `retain` under 2 (layer-a §5.2 clause 2
reads epoch `E−1`, so one history slot is a floor rather than a
preference), a device under §10's 1 MiB, one too small for 2 header
sectors and 2+`retain` slots, and **a partition that already carries a
valid monitor header unless `-r`** — reformatting discards every
published map the partition holds. It refuses a target that carries a
valid **object-store superblock** unless `-r`, by the rule above: that
is a store, and `-r` is how an operator says to destroy one. When `-r`
is given and the superblock is there, the format proceeds and
**warns**, because §2.1's deployment rule forbids the monitor's
partition being an object-store instance's unit — but an operator
reclaiming a decommissioned unit is doing exactly this on purpose, and
that is a warning about the unit rather than about these bytes.

Every one of those decisions is `monfmt`'s or `monfmtcheck`'s rather
than the command's, so that a T1 program drives them without exec'ing
anything — the same constraint on the code layout that puts the store
engine in `libshoal` above. Five are the command's, and all five are
about a file image the library is never handed: the length to open it
at, whether `-z` may shorten it, the refusal over an object-store
superblock — which `monfmt` reports to its caller as §2.1's warning
and leaves the caller to decide — the refusal over a valid monitor
header, which `monfmt` makes again for a library caller but which the
command reaches first and is therefore the one an operator reads, and
the removal of an image the run's own `-z` created before a later
step refused it.

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
layer-a's subject, not this document's, but two of `shoalsrv`'s flags
carry behaviour this document defines, so it gets a synopsis here:

    shoalsrv [-w] [-X point[,n]] [-q queues] [-s srvname]
             -m mapfile /dev/sdXX/name

`-w` is §3.2's operator assertion that the unit is write-through and
is reported in `/status`; `-X` is §13's fault-injection point,
present in every build and inert without the flag; `-q` sets the
queue-pool size, whose sizing rule is §7's; `-s` names the posted
service. `-m` is the cluster map, as a file: this build has no
monitor client, so the map is read once at start and never refreshed
(§14(18)). The monitor is `cmd/shoalmon`.

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
  a flush; a crash leaves each sector written since the last flush
  holding either its durable bytes or its cached ones, and which of
  the two is the test's to choose — all, none, a subset drawn from
  the seed, or a named set of sectors. That is what lets a torn
  commit header survive to be read back, and what stages the hazard
  §3.2's pre-flush exists to prevent: the commit record on the
  platter with a staged grain still in the cache;
- **torn and partial writes**: a write may land as any byte-wise
  mixture of old and new bytes within any sector it covers, and may
  land in any subset of its sectors;
- **short counts** on every read and write;
- `Echange`, `Eio` and `interrupted` on demand, on a read, a write or
  a flush; several faults may be armed at once and each may be aimed
  at a byte range, since one schedule wants a short count and a tear
  together and §8 wants an `Eio` on one named sector;
- **crash at a chosen point**, by name, with the flush and write
  sequence recorded so a test can assert the *order* as well as the
  outcome;
- **one lock over all of it**, so that the procs §7 puts on one
  device do not lose each other's operations out of the trace or
  race the seeded generator: each operation reaches the trace whole,
  and a single-proc run stays reproducible from its seed. The lock
  makes each operation atomic; it does not order them, so under
  §7's procs the draw sequence from the shared generator still
  varies run to run and a concurrent schedule is not reproducible.
  The lock is itself testable: `simslow` yields inside the two
  critical sections that carry shared counters, so removing the lock
  fails the many-procs case on every run rather than on some of
  them.

Points: `stage` (after the last staged grain write), `body:n` (after
*n* of the record's **body** sectors — the wrap record and the header
sector are not body, so the common one-sector record has none of
these points at all; and *n* counts sectors but is emitted per
**device write**, because that is the granularity at which the body
can be interrupted: §3.2 issues the body in `blksz`-bounded pieces,
the point fires after each piece carrying the body sectors written so
far, and a body that fits one piece therefore emits only its own
total. A schedule that arms a value between two piece boundaries
never fires, so a test that arms `body:n` must assert that the crash
it expected actually happened rather than reading a completed
operation as a pass), `precommit` (after the body write and **before**
the pre-flush), `commit` (after the pre-flush and immediately before
the header write, so the commit point is not reached), `postwrite`
(after the header write returns, before the post-flush), `preack`,
`ckpt:n` (after *n* checkpoint page writes), `super` (after a
superblock write returns, before its flush — the two copies are
written in sequence only by `shoalfmt`), and the monitor store's four
(§10): `monfmthdr`, inside `monfmt` after the flush that zeroes both
header sectors and before any other write, and `monhist`,
`monhistflush` and `moncur` inside a commit. A crash at a point is the
end of a run, so the simulated disk can be told to **stop the
device** at the crash: every subsequent read, write and flush fails
until the
test brings the machine back. Without that the writes a schedule
places *after* its crash point would still land, and a crash at
`commit` would still leave a committed record. The schedules that
examine what a partly completed sequence left behind — §2.2's two
superblock writes — keep the run going instead, so it is a choice the
test makes.

The torn-header sweep of T1.2 is staged rather than crashed at: the
record is written and then a byte-wise mixture of its old and its new
header bytes is placed on the platter, which is what a torn write
leaves and what the sweep must be exhaustive over.

Ten of §13's points are *mutations* or schedules rather than
crashes, and are built into the store as hooks that are inert unless
a test asks for them: `reclaim` (reclaim log space before the checkpoint's
superblock write returns), `publish` (force an `epochhigh` publish
after the *n*'th checkpoint page write, so it can be combined with
`ckpt:n`), `batch:n` (hold batch *n*'s record write and let *n+1*
complete), `fullwait` (park the next commit in §6's wait for log
space, with its entries still on the pending queue, until the hook is
cleared — which is what makes the interleaving where a committer
absorbs a waiting item, and the wait then elapses under it, the same
on every run), and `flush:n` (hold the *n*'th device flush issued
since the hook was set, until it is cleared, so a test can ask what a
proc may answer between the header write and the post-flush that
makes the record durable — a crash point cannot ask that, because the
question is about what the *other* procs in a batch are allowed to do
while the committer is still inside the flush), and `fatal` (put the
store into §3.2's condemned state, which the commit path itself
reaches only from an apply that failed after its record was durable —
a case §3.2 makes unreachable, so a test cannot arrive at it any
other way), and `bmfold:n` (park the next *n* rounds of a §8 fold in
the window between the map read and the validations the fold makes of
it, so a test lands a commit where the stamp exists to catch it
instead of racing for it — the sleep drops `qlstate`, which is what
lets that commit apply, and arming also releases a round already
parked, so a test walks a fold round by round by re-arming with 1
each time and drives it to its re-read bound), and `bmswap:n` (park
the next *n* pages of a swap in the `qlstate` hold that installed
them, which is the gap between two pages an abort or a `storeclose`
would land in; armed and released exactly as `bmfold:n` is. The point
parks on every page the swap walks, while `/status`'s `bmswapped=`
counts only the pages the swap installed — the ones that differ — so
a test that waits on that counter must arrange a differing page, not
expect one count per parked page), and `snapstale:n` with
`snapshort:k` (arm the next *n* enumeration fill attempts to find the
index *k* entries larger than the count did, so a test can drive §9's
re-count — and the vector's
slack, which is what decides whether a given growth needs one —
without racing for either; one arming is spent per fill attempt
rather than per open, an open makes up to `Snaptries` of them, and
the point is inert while `snapshort` is 0), and `snaphold` (park the
next enumeration open with the bound's slot taken until `storeclose`
has set `closed`, and let it go on again there — one arming parks one
open, and the sleep drops `qlstate` so the close can take it). The
last is what makes §9's *bail-out* free deterministic: an open that
holds the slot when the store is closed is the store's last claim, so
its failure path is the one that frees the `Store`, and without the
point a test could only race for that window.

**The freed hook is not one of those points.** `Storecfg.freed` and
`freedarg` are a callback rather than an `-X` name, because what they
observe is not an injected fault but the engine releasing the
`Store`'s own memory: the engine calls it as its last act before that
memory goes, on every path that releases a store — a `storeopen` that
failed part-way, `storeclose`, and the last `objsnapclose` of a
snapshot that outlived one (§9) — and it is inert while nil, which is
what everything but a test leaves it. It exists because §9's deferred
free has **no other observable**: a read through a snapshot whose
`Store` was freed early answers *correctly* out of freed memory, and
so does `storestat`, so a test that watched the answers alone would
pass a use-after-free and only the allocator would notice, later and
somewhere else.

Each T1 test names the requirement it discriminates and the mutation
that must break it; **each mutation is run**, per
`AGENTS.md`.

T1 formats a **small geometry** — a partition image of a few MiB with
`-n` and `-e` in the hundreds — so that `mk test` stays within
`AGENTS.md`'s seconds. The sweeps that are exhaustive are exhaustive
over one header sector, not over the whole store, and the cases that
need `nslots = 2^20` are T2's.

**What T1 covers today.** Seventeen programs. All but `maptest`, which
is pure text and reaches no device at all, run against the simulated
disk except where a file-backed device is the point:
`csumtest` (layer-a §1.4's block digests and object checksums against
known-answer vectors), `structtest` (§2's byte layouts against
known-answer vectors, a flipped byte caught in every structure,
§2.7's `Eobj` at its extremes, and §0's verify rule under two procs
sharing one record), `geomtest` (§2.1's arithmetic at the 4 TiB
worked example and at the small geometry above, every refusal §2.1
and §12 make a MUST, the bitmap sizing swept at `blksz` 512 and 1024
over partitions from 1 MiB to 24 GiB of simulated size, and
the maximal-record bound the log sizing rests on), `devtest` (the
simulated disk's own semantics — the volatile cache, torn and subset
writes, short counts, the error classes wrapped as a caller wraps
them, aimed and multiple faults, the crash victim policies, a crash
that stops the device, the recorded trace and eight procs sharing one
device — and the file-backed device, including the read-only open and
the `Wunit` split), `supertest` (§2.2's three clauses under torn
superblock writes and under the `super` crash point, which is T1.9's
first half), `montest` (§10's monitor map slot store and §12's
`shoalmonfmt`: what a format leaves and that a fresh store holds no
map, the format's own write and flush order read off the trace — both
header sectors zeroed and flushed before any other write, the slots
flushed before the real header copies land — with a format cut short
at `monfmthdr` leaving no valid header for the open to take; the
header's byte layout, a commit and the restart that finds it,
the ring newest-first with layer-a §5.2 clause 2's `E−1` entry present
at every length and across a wrap, T1.13's flush shape for §10 read
off the trace — one write and one flush into the ring, then one write
and one flush into a current slot, and nothing else written in a
commit — §10's read-back under a write that reports success and lands
nothing (`Sfdrop`) or lands a mix of old and new bytes
(`Sftearbyte`), on the ring slot and on the current slot in turn,
each failing the commit and leaving the restart on the previous map
with position 0 still equal to it; the read-back's own READ failing,
on each slot in turn and on each of a read-back's two reads, the
header sector and the text — once, where the retry carries the commit
through, and twice, where the publish is indeterminate: the commit
fails saying so, the slot is not served, its `seq` is spent, and the
retry of the same session is what the restart serves rather than the
orphan, with no two ring entries carrying one `seq`;
**T1.9's second half**, a crash at
`moncur` leaving the previous map current and the failed publish's
ring entry a phantom, and a slot torn
at a high `seq` not steering the next write onto the only good one;
**T2.7's phantom case at T1 scale**, a crash at `monhistflush` after
which the unpublished epoch answers nothing and the next commit
reuses the phantom's slot; the whole crash argument swept — each of
§10's three commit points against each of the sim's four crash
policies, twelve cells, every one asserting that the current map is
exactly the old one or exactly the new one, that position 0 is the
current map, and that no ring entry sits above the current map's
`seq`; a ring write that fails and a crash at
`monhist`, each leaving the current map untouched; a ring write that
fails *over a phantom victim*, after which the retry reuses that same
slot and the never-published epoch is still unanswerable at the next
start; a ring write that fails *over a valid victim*, with the ring
full, after which the live store still answers the oldest epoch,
counts no phantom, and agrees with the restart; the same ring full
under an *indeterminate* ring write that landed nothing, where the
victim is on the platter and is nonetheless dropped from history and
made a phantom, because a read-back that would not read cannot tell
that case from an unacknowledged map having landed; a current-slot write
that fails and is retried *in the same
session*, where the phantom mark has to hold in memory with no restart
to rebuild it; `disk full` on an
oversize map with the store unchanged, and the largest map that fits
read back byte-exact; the header copies — one damaged, both damaged,
and two valid copies that differ, and a pair whose `curoff`/`histoff`
do not describe the geometry, refused by name before a slot is located
through them — a current slot whose `len` does not
fit its slot, an open that writes nothing, a store reopened over a
read-only file image, §10's equal-`seq` tie — a fresh store opens on
slot 0 and its first commit writes slot 1 — an epoch published twice
with a regression between, and §12's refusals and its object-store
superblock warning, driven through `monfmt` as `fmtcktest` drives
`fmtstore`),
`fmtcktest` (`shoalfmt` to `shoalck` over both a
simulated disk and a file image; a store with a live one-block object
and a live three-block one, built through the codecs, with each fault
§2 and §5 name poked into it in turn and the checker's own words read
back; a store whose `blksz` is four device write units, whose every
page and grain write must go out in `Wunit` pieces; a ream cut
short, which must leave no valid superblock; §12's `-v` over the
replayed state — a multi-block object, a hole and a tombstone
verified clean, a poked grain named with its object and block index
where the checkpoint passes see nothing, a damaged digest array
reported as `arraybad` and a slot whose extent map fails its
`csum128` reported as a failure rather than skipped, an object flagged
`corrupt` that verifies clean reported as information, a grain freed
by an un-checkpointed truncate and handed to another object, which
verifying from the checkpointed index would report as a mismatch,
and a store closed on an un-checkpointed multi-block commit, over
which a read-only device — the open every flag but `-R` takes —
records no write at all;
and §12's `-R` — a bitmap page that is valid and wrong, which no
start repairs and which `-R` corrects to a full scan of the live
maps, both free-grain counts asserted as numbers, a bitmap page that
reads and fails its checksum, over which the `as found` count is not
printed at all,
an extent map that fails its `csum128`, which `-R` condemns rather
than rebuilds from, a rebuild that counts the objects committed since
the last checkpoint, `-R -v` writing the rebuild's lines before the
verify's, and the refusals of `-R` with `-o` and of `-R` on a device
opened read-only, which `-v` opens. `cmd/shoalck`'s own flag layer is
covered only through `ckstore`, which is what T1 drives: every
refusal `main` makes it makes again, and `-R -w` hands the no-flush
assertion to a file image exactly as to an sd unit, so the two opens
differ in the device and in nothing else), `storetest` (§5's
ordered start-up: the tolerant index read, replay and its
idempotence, §2.5's replay-coverage rule in all three of the
cases it exists to tell apart, the automatic bitmap rebuild, §2.2's
publisher and its durability orderings, §5 step 10's condemnation
after — and only after — replay and again when a damaged extent map
is first read, the grains a condemned slot's delete and its `op=full`
leave marked, counted in `grainleak=` and returned by a rebuild,
§2.6's exhaustion dropping one peer's records on the
live path and on replay alike, §3.2's refusal to start without a
flush channel, §4's re-hashing over every shape of write that changes
a block's covered length, replay's closing write-back of its extent
maps — a writable start over a map region the device refuses is
refused and names the region, a read-only one writes nothing, and a
read-only replay too big for its cache is refused naming the cache —
a checkpointer that cannot write, whose failures are counted, named
in the commit refused for log space, and dropped from that refusal by
a device that heals — the same store's next full log, with nothing
checkpointing, answering the bare `disk full` — §2.8's retry floor
under a live checkpointer proc, read as a rate of failed attempts
across a second and as the speed of the explicit requests it exempts,
and read again over a floor configured well below its cap — the
doubling and the cap as attempt counts over two windows, and the cap
as a healed device the paced checkpointer picks up inside one §6 wait
— and a fid condemned under the checkpointer, which is dead rather
than stuck, stops the retries and is what §6's refusal then names —
§2.8's dirty-page trigger surviving a condemnation that lands while a
checkpoint runs, and a store opened, written and replayed at a `blksz`
four times the device's `Wunit`), `objtest` (§2.7's extent-map slot rule
over all three transitions and both the crash and the re-replay
schedules, §2.4's invariant on the shrinking side, §3.5's deferred
reuse of grains and of slots under a held batch, §3.6's stage lifetimes,
bounds and `final=1` arbitration including D14's corrupt receiver,
§6's four exhaustions with delete working throughout on the reserved
tail, R7's dirty records across a restart and on every write-path
commit, layer-a §1.2's `object too large` at the bounds where a sum
would wrap, layer-a §2.6's tombstone errors, §1.5's create over a
tombstone and each of its discard receiver checks refused on its
own — the state check at the entry's own key, so that the key check
cannot answer for it — §3.7's rule that every refusal the API makes
is either §2.6's prefix or plainly not one, and the key-preserving
`corrupt` flag), `scrubtest` (§8's engine half: what a corrupt-flagged copy
answers on every path §3.7's row covers — the count-0 write
included — and that a delete applies and clears the flag; the scrub's
two durable transitions, each across a restart taken over a
checkpoint so that the index bit and not the replayed record is what
carries it; block repair, its two refusals told apart by whether they
carry a §2.6 prefix, its acceptance of a block whose old grain the
device will not read, and the grain it frees; the slot cursor over
live, tomb and free slots; and `/lost` through the set, the clear,
the delete and §5 step 10's condemnation) and `committest` (§3.2's flush
placement read off the device trace, the torn-header sweep over a
whole sector, short counts on every call, §3.4's crash matrix at
every point × every operation shape, several laps of the log
including its wrap record, eight concurrent committers and the
durable watermark under a held batch, the pending queue under §6's
wait — an item absorbed while it waits, and the wait elapsing under a
running batch — a log write that fails while later batches are in
flight, a checkpoint page write that fails followed by a second
checkpoint, §0's `interrupted` completed and `Echange` refused, §13's
own named points, §6's reserved tail against an `Edirty`, §2.8's
reclaim rule run both ways, the checkpoint mark against a concurrent
publish and under a held batch, a group commit at the largest record
replay accepts, a multi-sector record at the region boundary, a
header naming more sectors than that record, durable-before-ack for a
batch's members, and `qid.path` across restarts) and `enumtest` (§9's
snapshot-at-open enumeration: a `/obj`, a `/tombs` and an `/advert`
snapshot each walked by position with an entry created, deleted,
created over and discarded under it, both halves of the gone rule
discriminated one at a time, a live copy condemned under an open
`/obj` and still answered with `corrupt=1` rather than dropped (D14),
the bound on open snapshots at its default and at `objsnapmax = 2`,
the whole `disk full` text past it and the refusal of a `kinds` the
engine has no state for, three snapshots outliving
a `storeclose` — every read through them refused `store closed`,
an entry deleted before the close included, so that the refusal is
watched where the *gone* answer was available; `objsnapcount` still
answering; a further `objsnapopen` refused; and §13's freed hook
fired exactly once and only at the third `objsnapclose` — beside a
store closed with nothing open, which the same hook shows freed
inside `storeclose`, and a store condemned and then closed, which
answers `store condemned`; an open parked at §13's `snaphold` point
with the bound's slot taken while the store is closed under it,
refused `store closed` on the count pass it wakes into and its
bail-out shown to be what frees the `Store`; the same hook fired
once by a `storeopen` that failed on a device with no superblock;
that close raced by four procs on four snapshots in each of its
three shapes, thirty runs apiece — the procs only rendering, with
the caller's device closed the instant `storeclose` returns; each
proc closing its own snapshot against `storeclose`'s own decision;
and each proc closing the instant it is told `store closed`, which
is what catches a `closed` set before the proc wait — with the store
freed exactly once every time, an open whose vector the index
outgrows between the count and the fill, and the two terms of that
vector's slack told
apart — one growth refused by a ten-entry index, whose slack is the
flat sixteen, and absorbed by an 800-entry one, whose sixteenth is
fifty besides — two thousand opens under four churning procs with
not one refused and no count outside what the churn can produce,
every one of the five enumerations refusing on a condemned store, a
checkpoint taken mid-walk, the `/dirty` copy against a moving set and
the `fullsync` peer enumeration beside it after §2.6's exhaustion
drop, the `/lost` copy against a moving list and over the one slot
whose own index entry is the damage, and §6's tombstone reclaim walk
— single-proc, with the record replaced under it, with the record put
back at a higher key under it, and under concurrent churn with one
churn proc parked on a tombstone of its own making, so that the
walk's epoch condition is what holds it off and not its cutoff),
`maptest` (the cluster-map library of layer-a §3–§6 and §8.1, over
map text alone: §3.2's header read back field by field, §3.1's rule
that an unknown attribute and an unknown record — continuation lines
and all — are ignored, §0's comments and blank lines, D21's required
attributes, its defaults and its caps with each refusal read back by
its own detail, §4.2 and §4.3's placement against known-answer
vectors computed outside this codebase and the tie-break no vector
can reach, an under-replicated `P` and the primary an `up` change
promotes without moving a byte, §5.2's witness set with D22's
clause-2 substitution, its skip rule against the map's stale ledger
and `dead` excluded outright, §6.3's adoption decision with both
refusals reported when both hold, §6.4's fence state on a synthetic
clock, and §8.1's commit validation with §8.6's `forceepoch`
exemption), `bmrebuildtest` (§8's online bitmap rebuild, T1.28 and
T1.29: the walk over a serving store with a §5 step 10 condemned
slot, whose rebuilt bitmap equals a full scan of the live maps and
whose `grainleak` returns to zero with a stage's grains outstanding;
the commit that lands in a folded slot and in one not yet reached;
the write barrier and the per-slot generation stamp that validates an
entry the fold re-read outside the state lock — a map moved at an
unchanged four-tuple, a damaged map an `op=full` replaced, and a
writer that drives the fold to its eight-re-read bound, each parked
at §13's `bmfold` point; the swap dirtying only the page that
differs and the checkpoint after it writing that one page, an end
refused for a live slot the walk skipped, a leak recorded after its
slot was folded surviving the swap, and the pass's lifetime — the
abort that leaves the live bitmap byte for byte as it was, the abort
that lands inside a swap and does nothing, and the `storeclose` that
aborts a live pass and lets go of a fold parked in it) and
`peeropstest` (§3.8's peer-channel primitives and §9's oid-ordered
listing: the adoption over an absent id, over a lower-keyed tombstone
and over a live copy, each refusal told apart by whether it carries a
§2.6 prefix, the index slot and the `qid.path` it reserves, and the
tombstone it publishes read back across a restart; the drop's one
durable step asserted before the restart as well as after it, the
same freed state after a crash at `postwrite`, the two ids a drop is
not for, and a condemned copy dropped with its unrecoverable grains
counted in `grainleak`; the resulting-`csum` check over each of the
five calls a peer's key can reach, a wrong `csum` leaving the log's
`seqnext` and watermark where they were and a restart replaying to
the state before the call, and the zero-byte write that has no commit
to carry the check; and the listing over layer-a §1.1's byte order
with tombstones in the inventory, paged by `after` with `more`, its
refusals, and its pages under a proc creating objects beside the walk
and dropping and re-creating them behind itself, over a store
formatted with more than `Listchunk` slots so that a page is more
than one hold).

Two more drive the 9P surface rather than the engine, over
`test/srv9p.h`'s in-process client (`AGENTS.md`): `srvtest` (layer-a
§2's framework — start-up and identity, §2.1's attach and role
matrix, §2.2's tree, §2.3's qids, the render-at-open files, §2.5's
ctl gates, §5.4.1's `Tflush` and step 7, the per-fid state hooks, and
D16's shutdown order) and `srvenumtest` (what that framework carries:
§2.5's `scrub`, `reclaim`, `forget`, `drop` and `newmonid`, §2.2's `/dirty`,
`/stale`, `/tombs`, `/lost`, `/advert` and `/jobs`, the `/obj` and
`/meta` directory reads over a §9 snapshot with entries going
live→tomb mid-read, D20's retry and the `objsnapmax` refusal, §7.5's
scrub pass with T1.27 below, and §9's tombstone reclaim at both sides
of its cutoff, under its verb and under its timer). The split is by layer and not by size: a failure in
one says which half broke.

Against the list below that is T1.1–T1.33. Three of those rows are
covered at less than their full width, and each says so in its own
place below as well. **T1.21** is covered for the orderings and the
fields, but drives the four publish triggers in sequence rather than
from concurrent procs. **T1.27** is covered at the property its
argument rests on — the unit of scrub work is pushed to the object's
`Reqqueue` — and not at the race, which needs a client write path to
issue the racing commit with and the server has none. **T1.15** is
covered at T1's geometry and not
at the scale its row names: `enumtest` walks a snapshot of 1500
entries over 4096 slots while four procs create, delete and discard
beside it, and reads `/dirty` under the same churn, which is the shape
of the case in about 0.3 s. The 2.6·10^5 entries the row asks for are
T2's, for the same reason §13's small geometry is: `mk test` stays
within `AGENTS.md`'s seconds.

One thing below the tier line is **§12's ordering inside each tool's
`main`** — where `-z` sits relative to the guards and the geometry.
Every refusal it orders is a library entry a T1 program drives
directly (`geometry`, `superselect`, `monhdrsel`, `monfmtcheck`,
`monfmt`), but the order itself lives in code no T1 program execs,
so it is not covered here. A T2 case that runs the two commands over
an image and compares its length before and after a refused run is
what would close it.

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
  crash at `batch:n`; no acked write is missing after restart. Then
  the same shape with no crash at all: a device error on one batch's
  log write while later batches are in flight, after which no batch
  above it acks and a restart finds none of them. *Mutations:* wake
  waiters when their own batch returns; let the watermark pass a
  batch whose record did not land; take a waiting item out of the
  batch that absorbed it; cap a batch at a size the record bound
  replay enforces does not admit; answer a batch's members before its
  post-flush returns — which needs `flush:n`, because the question is
  what the *other* procs in a batch may do while the committer is
  still inside the flush, and no crash point asks that.
- **T1.9 two-slot validity (R2).** Tear a superblock copy, restart,
  commit again, tear again: the store still starts. The same for the
  monitor's map slots, in `montest`. *Mutation:* choose the write
  victim by `gen` alone — by `seq` alone, for the map slots.
- **T1.10 log reclaim (R2).** The `reclaim` point, then a restart
  that lands on the older superblock copy: its records must still be
  in the log. *Mutation:* reclaim before the checkpoint's superblock
  write returns — the one way §2.8 says this format can lose data.
- **T1.11 log wrap (R2).** Drive the log several times around the
  ring, crashing on and around the wrap record, with a record mix
  that includes one ending **flush with the region end**: the record
  after it is at the region start, carries the next `seq`, and is
  applied by replay. *Mutations:* continue at `+nsec` unconditionally
  — which walks replay into the index region and discards every
  commit since the wrap; let a record straddle the region end
  instead of emitting `Fwrap`; write the wrap record after the
  batch's post-flush rather than before it.
- **T1.12 stage lifetime (R3, R4).** Abandon an `op=full` part way
  through and end it by clunk, by `Tflush` and by timeout in turn;
  the free-grain count returns to its pre-transfer value each time,
  and `stagemax` and `stagetot` are both enforced. Then the restart
  case: force a checkpoint while the stage is live, abandon it, crash,
  restart, and assert the free-grain count is the pre-transfer one —
  a stage leaves nothing durable behind. Then `final=1`'s
  arbitration (§3.6): a push at a lower key and one at an equal key
  without `force` are refused `stale version` and leave the object,
  its key and its `csum` exactly as they were; an equal key with
  `force=1` and a strictly greater key apply; an object this instance
  does not hold is created by the same commit; and a copy whose
  `corrupt` flag is set takes the push at any key (D14).
  *Mutations:* keep a stage
  alive past its fid; allocate staged grains in the bitmap the
  checkpointer writes rather than in the staged set, which leaks them
  across the restart; bound stages per fid only; compare the sender's
  key against nothing; refuse the push to a receiver whose own copy
  contributes no key; require the object to exist.
- **T1.13 flush sequence (R1).** From the recorded device trace,
  assert that a flush precedes the header write and another follows
  it, for every commit shape including a wrapping one and one whose
  body spans several `Wunit`s. *Mutation:* either flush removed, or
  the two reordered. This is the test that makes T1.1's flush
  mutations detectable as a sequence even where they are not
  detectable as a loss. It does not apply to a `-w` store, which has
  no flushes to place (§3.2).
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
  changed. Include the **sparse extend across many blocks**, both
  live and after a crash-and-replay at `commit:0`: the blocks the
  commit does not name carry the zero-block digest, so `csum`
  verifies and the object does not fail its next scrub. Include also
  every shape of write that changes a block's covered length without
  naming it: a growth past a partial final block, by write and by
  truncate; a growth within the block that holds `len`; a truncate
  within a block and back up again; and a partial write above a
  truncated length. Each is checked by comparing the stored `csum`
  against the `csum` of the byte image the object reads back.
  *Mutations:* hash the final partial block over `blksz` rather than
  its actual length; hash a hole as absent rather than as the zero
  bytes it reads as; leave newly covered blocks' digests at sixteen
  zero bytes when applying the commit; re-hash only the block that
  holds the new `len`; compose and hash a block from the bytes its
  grain holds above `len` rather than from the zeros they read as;
  re-hash a block whose coverage grew instead of writing it afresh.
- **T1.17 corrupt digest array (§8).** Damage an extent-map entry so
  that `hash(dig[]) != csum`, and assert the repair takes the
  whole-object path. *Mutation:* accept a peer's block against the
  stored `dig[i]` when the array itself fails. Covered by `scrubtest`,
  which damages a stored digest and repairs the entry's own `csum128`
  so that the entry is served and the array is the suspect, then
  asserts that a block repair is refused even for a block whose bytes
  are right and whose own digest is intact — and refused with an
  error carrying no §2.6 prefix, because that case is a caller's bug
  and not a peer's.
- **T1.18 start after a mid-checkpoint crash (§2.5, R17).** Crash at
  `ckpt:n` with bitmap pages stamped ahead of the superblock, restart,
  and assert the store **starts**, replay reaches `Pmax`, and the free
  map matches a full scan. Then the media-fault variant: let the
  checkpoint complete and reclaim, damage the newer superblock copy so
  start falls back to the older one over a log that has been
  overwritten, and assert the store **refuses**. Then the quiescent
  variant: let a checkpoint complete, write nothing after it, crash,
  restart, and assert the store **starts** with replay applying no
  record at all. *Mutations:* refuse when a page's `ckseq` exceeds the
  superblock's, which fails the first case; drop the coverage test,
  which fails the second; drop the `superblock.ckseq` term and require
  a replayed record, which fails the third; read a torn page's `ckseq`
  into `Pmax` without checking its checksum first.
- **T1.19 extent-map slot transitions (R2, R13).** Give a
  multi-block object a slot with content in every block, delete it,
  then grow a one-block object past `blksz` so it allocates the same
  slot, with a write that does not touch block 0. Run it twice over:
  once with block 0 holding its own bytes, and once with **block 0 a
  hole** — create, truncate to `blksz`, then write past it — which is
  the case a rule that exempts holes from `nmap` gets wrong once
  clause 4 is wrong as well. Crash at `commit:0`; restart. Block 0
  must read what it held — its own bytes in the first variant, zeros
  in the second — every unwritten block must read zeros, and
  `verify` must pass, which is what catches a hole left with sixteen
  zero bytes for a digest. Then the re-replay schedule: with the hole
  variant, crash at `ckpt:n` with the
  object's index page written and its extent-map entry not, restart,
  and assert the same — replay must zero the map it inherits even
  though the entry it is applying to already carries the record's
  `emapslot`. Then the mirror: truncate a multi-block object to one
  block and assert block 0 survives the release of the slot.
  *Mutations:* name only the blocks the write changed; make holes of
  `[old nblk, nblk)` rather than of every unnamed block whose grain
  is 0; and, as a **pair**, exempt holes from `nmap` together with
  dropping clause 4.

  The pair is not a convenience. Four single-clause mutations of this
  rule — exempting holes from `nmap`, keying the zeroing off
  `emapslot` differing from the entry's current value rather than off
  the record's `Oslot`, skipping the zeroing of the newly allocated
  entry, and leaving `grain0`/`dig0` unset on the shrink — are
  **equivalent to the code they mutate under this writer**, and no
  test can discriminate any of them. Two mechanisms each fully
  determine the result: whenever this writer sets `Oslot` it names
  every block below `nblk`, so clause 3 overwrites `[0, nblk)` and
  clause 5 overwrites `[nblk, nblkmax)` — the whole map — leaving
  clause 2's zeroing nothing to change; and a hole omitted from
  `nmap` is restored identically by clause 4 and by the `csum` the
  writer computes from the same rules. The redundancy is deliberate
  and stays, because the format permits a record that sets `Oslot`
  without naming every block and replay accepts records from any
  build. What it is not is a discriminating test, and a plan that
  claims otherwise sends the next implementer looking for a schedule
  that does not exist.
- **T1.20 dirty records across a restart (R7).** Commit writes that
  create fine-grained dirty records for several peers, crash at
  `preack`, restart, and assert every record whose write is visible is
  in `/dirty` — and that the reconcile pass can name those objects
  rather than walking the disk. *Mutation:* skip the dirty region at
  start and build the set from replay alone.
- **T1.21 the superblock publisher (§2.2).** Drive `qidnext`,
  `epochhigh`, `monid` and a checkpoint publish concurrently from
  several procs, crash at `super`, restart, and assert no field
  regressed. *Mutation:* build the image from a snapshot taken when
  the publish was requested rather than from live state — which
  re-issues `qid.path` values, un-pins `monid`, or lowers `ckseq`
  depending on the interleaving.
- **T1.22 the checkpoint mark and a concurrent publish (§2.2, §2.8).**
  `ckpt:n` combined with `publish`: an `epochhigh` publish between the
  checkpoint's page writes and its flush must carry the *old*
  `ckseq`/`cklogoff`, and a crash straight after it must still replay
  the records those pages materialise. *Mutation:* let the checkpoint
  advance the publishable mark before its flush returns.
- **T1.23 durable before issue (R15, R16).** Hand out `qid.path`
  values until a batch advance is due, crash at `super`, restart, and
  assert no path is ever re-issued; the same shape for `epochhigh`
  and `monid`, which must be durable before the instance acts under
  them. *Mutations:* advance the in-memory counter past the recorded
  high-water; act on an adopted epoch before its publish returns.
- **T1.24 bitmap rebuild (§2.5).** Corrupt a bitmap page, restart,
  and assert the store starts, `bmaprebuild=yes` is reported, and the
  rebuilt free map equals a full scan of every live object's map.
  *Mutation:* trust the page that failed its checksum.
- **T1.25 deferred reuse of slots (§3.5).** T1.7's schedule for index
  slots and extent-map slots: free one by a commit, crash at
  `commit:0` before its post-flush, restart, and assert the slot is
  still the old object's and its extent map intact. *Mutation:*
  return freed slots to their free lists at commit time.
- **T1.26 the flush channel (§3.2).** With the simulated raw channel
  refused, the store MUST NOT start; with `-w` it starts and
  `/status` reports `flush=asserted-writethrough`; with the channel
  present it reports `flush=raw`. *Mutation:* start anyway and report
  `flush=unavailable`.
- **T1.27 scrub inside the queue (§8).** Hold a scrub read of one
  object while a commit on the same object frees that grain and
  another object stages into it; the scrub must not flag the object
  `corrupt`. *Mutation:* have the scrubber read grains directly
  instead of pushing through the object's `Reqqueue`. Covered in
  `srvenumtest` at the property the row rests on rather than at the
  race: with §13's `objhold` point set, a client's `verify <oid>`
  parks inside that oid's queue, and the pass's `/jobs` `done=` stops
  at that object's slot with slots still ahead of it and moves again
  only when the hold is cleared. That property and the mutation above
  are the whole of this row's coverage: the mutation makes the case
  fail and the queue push makes it pass, which is what a scrubber
  reading beside the queue would and would not do. The commit racing
  the read is not driven at all — issuing one needs a client write
  path, and the server has none — so the row stays open at the race
  until something can.
- **T1.28 the online rebuild walk (§8, D18).** A store with a slot §5
  step 10 condemned, walked while it serves: the rebuilt bitmap
  equals a full scan of the live maps read off the media, `grainleak`
  returns to zero, and `grainfree` is right with a stage's grains
  outstanding — before the condemned record is deleted and after.
  Then a commit that lands *during* the walk, in a slot already
  folded and in one not yet reached, is in the bitmap after the swap;
  and a map that moves at an **unchanged four-tuple** — a block
  repair, landed in the window between a fold's map read and its
  validation, which §13's `bmfold` hold point parks the fold in —
  forces the fold to read again. So does a **damaged** map that
  §3.6's `op=full` replaced while the fold sat in that window: the
  fold goes round again rather than condemn the repair, and the
  grains the fresh map names are in the bitmap after the swap. And a
  writer that moves the map inside *every* round drives the fold to
  its re-read bound — eight re-reads, then a ninth round that folds
  from the pinned entry and agrees with the full scan — which the
  `bmfold` point makes deterministic by arming a round at a time.
  *Mutations:* drop the write barrier; validate the re-read entry by
  the four-tuple instead of the stamp; do not bump the stamp in the
  apply; fold the grains a damaged map names; check the map's
  checksum before the stamp; reset the round count at a re-read. The
  fourth is all the fold's `bad`-entry fast path can be shown to do:
  every condemned slot the engine makes is one whose extent-map entry
  failed its checksum, so a mutation that dropped only that fast path
  would fall into the checksum arm and answer the same.
- **T1.29 the swap and the pass's lifetime (§8, D18).** Over a
  geometry of several bitmap pages whose objects all sit in the
  first: the swap installs and dirties the one page that differs, and
  the checkpoint after it writes one bitmap page and not the whole
  bitmap. An abort leaves the live bitmap byte for byte as it was and
  the barrier disarmed, so an ordinary commit follows it and a second
  pass returns the grains. A `storeclose` under a live pass aborts
  it, and lets go of a fold parked in the pass it dropped — driven
  under an open snapshot, which is what keeps the `Store`'s memory
  alive across the close (§9). An end whose walk skipped one live
  slot is **refused**: it installs nothing, the pass stays live, the
  bitmap is byte for byte as it was, and the end after the missing
  fold succeeds — while an ordinary write under the walk does not
  stand in for that fold and a slot created under the walk needs
  none. A leak recorded *after* the pass had folded the slot survives
  the swap, which installs those grains marked and leaves `grainleak`
  counting them; the map is damaged after its fold and read again
  through a one-entry extent-map cache, which is what puts the
  condemnation on the far side of the fold. And an abort that lands
  between two pages of a swap, where §13's `bmswap` point puts it,
  does nothing at all and the swap runs to its end. *Mutations:* swap
  the whole bitmap at once; make the abort install the shadow; leave
  a live pass alone at `storeclose`; skip the coverage check; mark a
  slot covered for any apply; zero `grainleak` at the end whatever
  the pass folded; let an abort inside a swap drop the pass.
- **T1.30 tombstone adoption (§3.8).** Adopt over an id the store
  holds no record of and assert a tombstone at the key that arrived,
  `len` 0 and layer-a §1.4's zero-length `csum`, visible through
  `objstat` and §8's slot cursor, with a fresh `qid.path`, surviving
  a restart. Adopt over a lower-keyed tombstone and assert it is
  re-keyed in its own slot at its own `qid.path`. Adopt over a live
  copy and assert the refusal, that it carries no §2.6 prefix (§3.7)
  and that the copy is untouched. Adopt at version 0 and assert
  `bad ctl` and no record. *Mutations:* let the adoption take a live
  copy, leaving its grains marked under a tombstone; publish the
  tombstone at a `len` other than 0.
- **T1.31 drop (§3.8).** Drop a multi-block live copy and assert the
  grains, the extent-map slot and the index slot are all free again
  (`Storestat`), and that no record is left — **before** the restart
  as well as after, because replay repairs an apply that ran the item
  the wrong way round and a check made only after a restart cannot
  see the live path's order at all. Then the crash-point discipline:
  with the simulated device stopped at `postwrite` and the written
  sector kept, so the record is durable and its post-flush never
  returns, assert the call fails — the crash fired — restart and
  assert the same freed state: one record, one outcome. Assert the
  two ids a drop is not for, both `no such object` (§3.7), and that a
  `corrupt`-flagged copy is droppable and leaves `/lost`.
  *Mutations:* leave the copy's grains marked (compare `grainfree`);
  commit the `Eobj` without the `Eslot`, leaving a tombstone behind;
  apply an item's `Eslot` before its `Eobj`, which only the
  before-restart assertions catch.
- **T1.32 the resulting-`csum` check (§3.8, D23).** For each of
  `objwrite`, `objtrunc`, `objremove`, the adoption and `op=full` —
  the five calls a peer's key can reach — learn the `csum` the
  operation produces on one store,
  then on an identical one offer a wrong `csum` and assert
  `checksum mismatch` and that **nothing is durable** — the log's
  `seqnext` and watermark have not moved, and a restart replays to
  the state before the call — and then offer the right one and assert
  it commits the same four-tuple, all four of it. A zero-byte write
  is its own case, since it has no commit to carry the check: a wrong
  `csum` is `checksum mismatch`, the right one is `ok`, and neither
  writes a record or moves the key. *Mutation:* make the check after
  `logcommit` rather than before it, which leaves the refusal in
  place and the record on the platter.
- **T1.33 the oid-ordered listing (§9).** An inventory chosen for
  layer-a §1.1's byte order over mixed lengths — an id that is a
  prefix of four others, and `-`, `.`, `_` and letters straddling
  each other — with two of its entries tombstones. Assert the whole
  inventory in order with both kinds present and the `Objinfo`
  matching `objstat`'s; three pages of three resumed by `after` with
  no duplicate and no gap and `more` set only while inventory
  follows; a page of exactly the inventory and one an entry short of
  it; `k` of 0; `after` at the last oid; and the refusals — a
  negative `k`, an `after` past §1.1's bound, an `after` length with
  no `after`, and a store that has stopped serving.
  Then a proc creating objects beside the walk — and dropping and
  re-creating them behind itself, which is what can put one oid in a
  page twice — under which every page must still be strictly
  ascending and above its `after`.
  The walk is repeated for as long as that proc is in the engine
  rather than run once beside it, since one pass costs less than one
  of the proc's commits; the properties are accumulated and asserted
  once, so the count of checks does not depend on how the race fell,
  and one assertion is that the walk saw an object the proc made —
  without it a run in which the two never overlapped is
  indistinguishable from one in which they did. The store it runs
  against is formatted with more than `Listchunk` slots, because at
  §13's small geometry a page is one hold of `qlstate` across the
  whole index and nothing can move under it. *Mutations:* resume at
  `>= after` rather than `> after`; skip tombstones. **Not covered,**
  two things, and one reason for both: no `-X` point parks a listing
  between chunks. §13's hook framework is there and `snaphold` is the
  precedent for a point of exactly that shape, so what is missing is
  the point and not an observable. Without it, that the scan releases
  `qlstate` between chunks rather than holding it across the index is
  indistinguishable from outside — a create that blocks on the lock
  and a create that lands between chunks leave the same page. And the
  case where an oid a page sees twice is answered once needs the
  release and the re-creation to fall either side of a chunk boundary
  with the new slot ahead of the scan, which nothing here can place:
  a mutation removing the de-duplication runs green under this churn.
  Both are read off the code rather than driven.

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
  torn publish. Including the phantom case: crash between the history
  write and the current-map write, restart, and assert no history
  entry survives with a `seq` above the current map's, so `/maps/<E>`
  can never serve a placement that was never published. Both halves
  are discriminated at T1 scale in `montest` against the simulated
  disk; what stays open here is the real device, which is the point
  of a T2 row.
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

Thirty-nine places where layer-a is silent, self-defeating, or
contradicted by the measurements or by the platform. Each entry
states the tension, its resolution, and where the argument for it
lives; nothing here repeats an argument made in a section above.
Items 1–5, 8, 9, 11, 12, 13, 14, 26 and 39 are amendments **made** to
`docs/design/layer-a.md`; items 6, 7, 15, 17, 18–25 and 27–38 are
recorded here and not made there; items 10 and 16 are **proposals** rather
than amendments, because they touch the wire.

Items 18 to 39 are the object server's, and they describe what
`srv/libshoalsrv.a` and `cmd/shoalsrv` **do today**. Several of them
name a half that is not built; each says which.

1. **`cur` cannot usefully be durable (layer-a §5.2).** Layer-a
   required currency recorded "durably as `cur=<epoch>`" and, two
   lines later, invalidated for every object on every process start:
   the only moment a durable value could be read is the moment it
   must be discarded. *Made:* "durably" is gone; both invalidation
   MUSTs and `cur=` in `/meta` stay, as an in-memory value (§5, R5).

2. **Step 5b's dirty record is committed *with* the update, not
   before it (layer-a §5.4).** The `Edirty` entries ride in the same
   record as the `Eobj`, so either both are durable or neither.
   That is not strictly stronger — layer-a's stated order covers one
   window this order does not — and §2.6 is where the window and the
   restart `fullsync` that closes it are argued. *Made:* step 5b
   reads "durable no later than the update", and the restart
   obligation is written into layer-a §7.1 so the licence cannot be
   taken without the rescue.

3. **Layer-a assigned no home to state the store must hold (layer-a
   §2.3, §6.3, §8.6).** The pinned `monid`, the highest adopted epoch
   and the monotonic `qid.path` counter must all survive a restart,
   and layer-a named no place for them. *Made:* they live in the
   superblock, and layer-a now states the two orderings that make
   them mean anything — the epoch durable before the instance acts
   under it, the `qid.path` high-water durable before any value in
   its batch is issued. §2.2 argues both, including why the counter
   must be a stored high-water rather than a maximum over live
   records.

4. **Layer-a §5.5's resync arithmetic omits the disk.** 185–545 ms is
   wire time; the receiving disk costs seconds (§11). *Made:*
   qualitatively, pointing here. It does not carry this document's
   2.5 s figure, because that figure rests on eight-way concurrency
   read off a table of independent writers (§16a(1)), and a number in
   the ratified contract should be one somebody measured for the case
   it describes.

5. **Layer-a §5.4's "plus local I/O" is the dominant term, not a
   rounding error.** The honest sum is 3·`replms` plus 9–18 ms
   ordinarily, plus the object's queue, plus — when the log is full —
   a wait bounded by `ckwaitms` rather than by `replms` (§6). At a
   `replms` tuned toward the sub-millisecond LAN the local term is
   the whole cost. *Made:* layer-a §5.4 names the checkpoint wait.

6. **`disk full` stays definitive, and no retryable error is added.**
   Adding a retryable `busy` to layer-a §2.6 would be a wire change
   made to paper over a store that cannot bound its own checkpoint;
   §6 fixes the cause instead and bounds the wait. *Not made,* and
   §16b(2) records the product call.

7. **The flush channel is not exclusive, and the store requires it
   anyway.** `/dev/sdXX/raw` admits any number of openers and the
   cdb→data→status state is per unit, so concurrent users corrupt
   each other's commands rather than being excluded. *Not made:*
   layer-a never mentions the `sd` unit and D4 says only "one
   instance per disk", so the deployment rule (§2.1) and the
   single-owner flusher (§3.2) are written down here, together with
   the refusal to start without the channel and `-w` as the only way
   past it.

8. **Formatting precedes the map that defines the format (layer-a
   §3.4).** Step 1 there has a server generate its identity before it
   has a map, so before it knows `blksz` and `objmax`, which this
   store's geometry depends on. *Made:* layer-a §3.4 says the format
   step needs them, and the server MUST refuse to serve if the
   adopted map's `blksz`, `objmax` or `csumalg` differs from what the
   disk was formatted with — `csumalg` because a mismatch invalidates
   every stored digest.

9. **Layer-a §10.2's evidence items: two answered, one settled, one
   not.** The local layout and monitor durability questions are this
   document. `blksz` at `Wunit` is settled by measurement and layer-a
   §1.4's default is 16384. Not settled: whether a full `/obj`
   snapshot is affordable — 3.1 MB at 2.6·10^5 objects but 12 MB per
   open fid at `nslots = 2^20`, so `objsnap=partial` is unnecessary at
   the Layer B envelope and not demonstrated unnecessary at this
   design's own maximum (§9). That 12 MB is the vector alone; §9's
   102 MB for eight fids is the same vector with its allocation
   slack, 12.75 MiB each.

10. **A `Tflush` after the commit still owes the cleanup half of step
    7.** Once the commit is durable the discard half is vacuous but
    the invalidate half is not (§3.3). *Proposed, not made:* layer-a
    §5.4.1 would read better if it split the obligation explicitly
    into a discard half (pre-commit only) and an invalidate half
    (always). That is a clarification of an existing MUST, and it sits
    beside the amendment item 14 did make.

11. **`op=meta` had no way to say "I hold a copy that fails
    verification".** *Made:* the owner ratified the `corrupt=1`
    response form and its companion receiver rule on 2026-08-28
    (decisions.md D14); the grammar and the rules live in layer-a
    §5.6, the receiver half in layer-a §5.5 and — for a copy this
    store has condemned — in §3.6 and §5 step 10, and what this store
    answers in §8. The engine holds the flag, the transitions that
    set and clear it and the `/lost` accounting; the `op=meta`
    response itself is the `/rpc` surface's, which is the server's
    and is not built — §8 marks that half where it falls.

12. **A tombstone's cost.** Layer-a §1.5 said a tombstone "occupies a
    metadata record and nothing else". True here — 256 bytes, because
    a tombstone holds no extent-map slot (§2.3) — but true by design
    rather than by nature. *Made:* layer-a §1.5 now says the cost is
    implementation-dependent and may include a reserved per-object
    metadata extent.

13. **Layer-a §5.5's multi-request `op=full` stage had no lifetime.**
    Nothing said when an abandoned stage is released, so a sender that
    died mid-transfer left the receiver holding staged space forever.
    *Made:* layer-a §5.5 now requires an owner, a lifetime and a
    bound, matching §3.6's owner, lifetime and per-fid bound. The
    process-wide `stagetot` §3.6 argues for is this store's own and
    is not required of layer-a.

14. **layer-a §5.4.1 forbade the reply order every 9P server
    actually produces, and prescribed a mechanism this store does not
    use.** It required "no `Rwrite`, no `Rerror`" before the
    `Rflush`, which `lib9p` cannot do — a parked `Rflush` is sent from
    inside the flushed request's own `respond`, after that request's
    reply is already on the wire, and `reqqueueflush` answers a
    still-queued request `interrupted` first. It also described
    `srvrelease`/`srvacquire` and "the per-object lock", which D12 and
    §7 replaced with a `Reqqueue` pool and no per-object lock at all.
    *Made:* §5.4.1 now requires `Rflush` and the whole of step 7,
    permits the flushed request to have been answered first (the
    client discards that reply), and states the ordering as a property
    of the queue rather than of a lock. §5.4 step 2 now enters "the
    object's ordering point" rather than taking a per-object lock, so
    the contract names no mechanism it also says is optional.

15. **`/lost` names a condemned slot without an `oid=`.** Layer-a
    §2.2 fixes `oid=` as a field of every `/lost` line, and layer-a
    §1.2 constrains what an oid may be; an index entry damaged badly
    enough to be condemned can supply neither. *Not made:* the line
    carries `slot=<n>` and omits `oid=` (§5 step 10), which is a
    deviation from a named field rather than an invented oid. Format
    beyond `oid=` and `kind=` is implementation policy there, so the
    omission is the smaller of the two departures.

    **`kind=` reads `corrupt` on every line that carries an `oid=`.**
    §2.2's three values are not three states of one flag. `corrupt`
    is layer-a §7.5's local verification failure, which is the only
    one an instance decides by itself; `lost` is §7.5(4)'s "no peer
    holds a verifying copy" and `diverged` is §1.3's equal key with
    differing content, and both are verdicts about what peers hold.
    This build has no peer client (§14(18)), so neither has been
    reached, and the engine's flags say nothing about them: a slot
    joins the lost list only when it is bad or carries `Icorrupt`
    (§8), and the one path that sets either on an entry with an oid
    sets both. *Not made:* an oid-bearing line renders `corrupt`
    unconditionally, and the two peer verdicts land with the
    replication surface. A condemned slot's line keeps `kind=lost`,
    which is §9's wording for it and is §7.5(4)'s meaning with
    nothing left to ask about: the copy cannot be read at all.

16. **layer-a §5.5 requires the resulting-`csum` check but not that
    it precede the update.** §5.5 has the receiver "compute its own
    and MUST fail with `checksum mismatch` if they differ" and fixes
    no moment for it, so a receiver that commits the operation and
    then reports the mismatch meets the letter while leaving exactly
    the divergent state the check exists to prevent — durable, for
    the next start to replay, with the caller holding an error and no
    way to know. *Proposed, not made:* §5.5 would say the receiver
    MUST fail **before the operation becomes durable**. It is a
    proposal rather than an amendment because it strengthens a
    receiver obligation on the wire. This store already meets it
    (§3.8), and D23 marks the timing as this store's implementation
    policy for as long as §5.5 does not carry it.

17. **"A page MUST be internally consistent" is undefined for
    `op=list` (layer-a §5.6).** §5.6 requires it of a page and does
    not say what it means, and a chunked scan must read it one way or
    the other. *Not made; recorded here as this store's reading:* a
    page is consistent **per entry** — each entry is rendered whole
    under one hold of the state lock, so no entry mixes two states of
    one object — and it is duplicate-free, so it ascends strictly in
    `oid`. It is **not** a snapshot of the instance at one instant:
    an object created or deleted while the page is being built may or
    may not appear in it. What §5.6 tolerates between pages — "a
    reconcile pass MUST tolerate an object created or deleted between
    pages" — this store may therefore also do within one. The other
    reading would cost a hold of the state lock across the whole
    index, which is the 23 ms §9 measures and the cost the chunking
    exists to avoid. §9 describes what the scan does.

18. **The object server has no network (layer-a §6.3, §5.5, §5.6).**
    layer-a has every party poll the monitor's `/map` every `pollms`,
    and has a primary reach its peers over `/repl` and `/rpc`. This
    build has neither client. The cluster map is a file named by
    `-m`, parsed with `mapparse`, adopted once at start-up and never
    refreshed; there are no peers, so nothing is replicated, no
    currency check is made and no stale mark is registered. *Not
    made:* layer-a is unchanged and this is a build that does not yet
    conform to it. Items 19 to 23, 34 and 35 are the consequences
    that are visible on the wire. `/repl` and `/rpc` are files that
    exist, gate by role, and refuse with `shoalsrv: not built`
    (§14(29)). `/advert` is built: it renders this instance's own
    inventory in §7.2's line grammar, live and tomb, for a peer to
    read. Nothing sends it, because §7.2's sender — and its rate
    limit — is the peer client this item is about; what is built is
    the readable half.

19. **§6.4 F1's lease fence is inert while the map is static.** F1
    fences an instance that has not refreshed its map within
    `leasems`. With no monitor client there is no refresh, so a live
    lease clock would fence the instance one `leasems` after start
    and nothing could ever clear it. *Not made:* the server evaluates
    `fencekind` against the time of its last successful refresh —
    the start-up adoption — rather than against the monotonic clock,
    so the lease half can never elapse and `fence=` in `/status`
    never reads `lease`. F4, the operator fence of `fence on|off`, is
    live and is what the fenced-verb gate and the object rows' gate
    are tested against. A refresh loop restores F1 by passing the
    real clock at that one call site. F3 is not inert in the same
    way: it asks this instance's own record in that static map, so an
    instance started under a map that says `up=no` or `status=out`
    for itself refuses `role=client` object I/O with `down` for as
    long as it runs (§14(24)).

20. **The msize floor is enforced at `Tattach`, not at `Tversion`.**
    layer-a §5.5 sizes the forwarded-write payload off the negotiated
    `msize` and §2.2 reports it in `/status`; the floor this server
    requires is 8192 + `IOHDRSZ`. `lib9p` answers `Tversion` itself
    and `Srv` carries no hook for it (9p(2)), so the first point at
    which a server sees the negotiated size is a request that carries
    a `Srv*` — `Tattach`. *Not made:* a connection below the floor
    completes its version negotiation and is refused at attach. The
    refusal is not a layer-a §2.6 condition, so it carries this
    server's own prefix (§14(29)). The value is read from
    `Srv.msize`, which 9p(2) marks implementation-specific; there is
    no other reader of it.

21. **`future epoch` triggers no map fetch.** §2.1 has an attach at
    an epoch above the instance's fail `future epoch` and SHOULD
    trigger an immediate map fetch. *Not made:* the refusal is the
    whole of what happens, because the map is static (§14(18)). The
    instance stays at the epoch of the map it was started with, and
    an operator who wants it higher restarts it with a newer map
    file.

22. **A map this instance may not adopt is refused at start.** §6.3
    has an instance reject a map whose epoch regresses or whose
    `monid` differs from its pin, and report the condition in
    `/status` as `epochregress=yes` or `monidmismatch=yes`. That
    reporting assumes a later refresh can still deliver a good map.
    Here there is no later map. *Not made:* `srvnew` refuses to start
    on a map `mapadoptable` rejects, naming the flags it set, so the
    instance never serves under a map it has refused — and
    `/status`'s two flags therefore always read `no` while the server
    is running. The identity comes from the same place: layer-a §3.4
    has the disk carry the identity, so the instance is the map
    record whose `uuid=` is the superblock's, and a map with no such
    record is refused too. §14(8)'s geometry check runs over the
    superblock `superselect` reads before the store is opened.

23. **Four `/status` fields are omitted.** §2.2 makes `epoch=`
    normative and the rest SHOULD-present. `chunk=` is the smallest
    peer `msize` less headers; `underrep=`, `strays=` and `marks=`
    count local objects against the placement, against the peers that
    hold them and against the stale ledger's marks on this instance.
    None of the four can be computed without the peers and the
    reconcile pass §14(18) says are not built, and a zero would be a
    measurement this instance has not made. *Not made:* they are
    absent from the file rather than present and wrong. Two fields
    beyond §2.2's list are present because nothing else reports them:
    `objsnapopen=`, which §9 makes the server's half of `objsnap=`,
    and the queue pool's `queues=`, `qdepth=`, `qpushed=` and
    `qdone=`, which §7 asks `/status` to report and which `Reqqueue`
    does not count for itself. `queues=` is the size of the hash the
    object ids land in — the ceiling §7 is about — and does not count
    the one reserved queue an operation that names no object is
    offloaded to; the other three count every request the pool took
    on, the reserved queue's included.

24. **§2.1's role matrix has cells §2.1 does not state.** §2.1 grants
    `/repl`, `/rpc`, `/advert` and object reads to `role=repl`, and
    `/ctl`, `/rpc`, the status files and read-only `/obj`, `/meta`
    and `/tombs` to `role=admin`; it says nothing about which roles
    may reach the rest. *Not made; recorded here as this server's
    matrix*, which is one table in `srv/tree.c` with a column each
    for walk, open-for-read and open-for-write:

    - The status files — `/status`, `/map`, `/dirty`, `/stale`,
      `/tombs`, `/lost`, `/jobs` — are `role=admin` alone, walk
      included, because that is the only role §2.1 grants them to. A
      client learns the epoch from the monitor (§6.3), not from here.
    - `/obj` and `/meta` may be **walked** by every role and **read**
      — the directory read, which is the enumeration — by
      `role=admin` alone, which is §2.1's read-only grant said
      exactly. A client that could not walk through `/obj` could not
      reach an object at all.
    - `/ctl` may be walked, opened and written by **every** role, and
      the gate that refuses is §2.5's per-verb one. §2.5 requires
      that "a verb issued on a fid whose role does not permit it MUST
      fail with `permission denied`", which has meaning only if a
      non-admin fid can hold `/ctl` open and write to it; §2.1's
      grant of `/ctl` to `role=admin` is that same gate said the
      other way round, since every row of §2.5's table is `admin`.
    - `/advert` is `role=repl` alone, read-only, walk included. §2.1
      grants it to `role=repl` and says nothing about the other two:
      it is the bulk version advertisement peers reconcile from
      (§7.2), and what an operator wants of it is in `/dirty` and
      `/stale`, which are admin-only.
    - `/obj/<oid>` and `/meta/<oid>` may be walked and opened for
      reading by every role, and opened for writing — `/obj/<oid>`
      alone, since §2.2 makes `/meta` read-only — by `role=client`
      and `role=admin`.

    A matrix by role and file cannot state §2.1's operator rule,
    which is about the **name**: `role=admin` may create and write
    reserved `shoal.` ids and no others. Each row therefore carries a
    gate, run right after the role gate on open, create, remove and
    wstat, and before the row's cell on read and write — where there
    is no role gate to run after, 9P having settled the role at the
    open. A read and a write are gated because §6.4 F1 fences
    **operations**, and the operator fence F4 can go on while a fid is
    open: a fid opened before `fence on` would otherwise carry its
    grant past it. The gate of `/obj`, `/obj/<oid>` and `/meta/<oid>`
    answers three rules in this order:

    1. §2.1's operator rule: a `role=admin` create, write, remove or
       wstat of an id that is not a reserved one is `permission
       denied`. It is first because it reads the fid and the name
       alone, and neither changes while the fid lives, so no later
       state can make an operation §2.1 forbids permissible.
    2. §6.4 F3: `role=client` I/O on an instance whose own map record
       says `up=no` or `status=out` is `down`. F3 is **live** here,
       read off the static map (§14(18)), so it is a standing state
       of the instance rather than one that moves under an open fid;
       `up=heal` is not in F3's list, and the attach is not gated,
       since §2.1 names no such refusal for it.
    3. §6.4 F1 and F4's fence: `fenced`, with the sole exemption §2.1
       and F1 both name — a `role=admin` **read** of a reserved
       `shoal.` id, which is what makes §8.6's monitor rebuild
       executable. §2.1 grants admin *writes* of reserved ids only
       while unfenced, so the exemption is the read alone.

       A read of the `/obj` or `/meta` **directory** is not fenced
       either, and that is a ruling rather than a quotation: F1
       fences "every `role=repl` and `role=admin` read of an object
       through `/obj` or `/meta`", and a listing is not a read of an
       object. `/tombs` is the same operator inspection path (§2.2)
       and F1 does not name it at all, so fencing the listing and not
       the tombstones would be a distinction with nothing behind it.
       Every **read** of a row that names an object stays fenced,
       which is what F1 is for: a deposed primary must not serve an
       object's bytes. A `Tstat` of `/obj/<oid>` is not one — it
       serves the length, mtime and qid version §2.3 defines, and
       runs no gate at all.

    Only the first of those three positions is derivable. Rule 1 is:
    §2.1's operator rule reads the fid's role and the name alone, and
    neither changes while the fid lives, so no later state can make an
    operation §2.1 forbids permissible, and placing it anywhere else
    would only answer a differently-spelled refusal to an operation
    that is refused either way. Rules 2 and 3 are a collision of two
    MUSTs: an instance that is `up=no` or `status=out` for itself
    **and** fenced owes a `role=client` read both F3's `down` and
    F1's `fenced`, and layer-a settles neither above the other.
    *Settled here as implementation policy:* `down` wins. It is the
    instance's own standing state, read off the map it was started
    with (§14(18)), while the fence is the one gate an operator moves
    under an open fid — and the two answers send the client to
    different places. layer-a §0 classes `fenced` as **retryable**,
    which a client library must retry with bounded backoff after
    re-reading the map, and `down` as a **redirect**, which says only
    that this instance may not serve and has the client re-evaluate
    placement. F3's condition is standing state that no retry can
    clear, so `down` is the answer that moves the client to an
    instance that can serve it; answering `fenced` there would cost it
    a backoff loop against an instance the map has taken out of
    service. A conforming implementation may answer `fenced` instead.

    `/repl` and `/rpc` carry a gate of their own, and it is the fence
    alone: F1 fences "every `/repl` and `/rpc` operation", which is
    carried here as every open, read and write of those two rows —
    and the remove and wstat that reach the same gate — rather than
    the open alone. A `Twalk`, a `Tstat` and a `Tclunk` run no gate,
    so F1's "every operation" reaches as far as the gate does and no
    further. The other two rules are not theirs — the operator rule
    is about an object's name, and F3's `down` is about `role=client`
    I/O, which neither row admits.

    A `role=client` create of a reserved id is §1.1's `reserved
    name`, and the create body is what answers it: the gate's
    operator rule asks about `role=admin` alone, so a client's create
    passes the gate and is refused on the name below it (§2.1, §2.6).

25. **An attach specifier missing a required attribute answers `bad
    aname`.** §2.1 makes `epoch` REQUIRED for `role=client` and
    `peer=` REQUIRED for `role=repl`, and gives the attach no error
    for their absence; the only attach error it names for the
    specifier is `bad aname`, for one that is unparseable. *Not
    made:* an absent required attribute, a repeated attribute, an
    unknown attribute, an unknown role spelling and a malformed
    `epoch` are all `bad aname`. The grammar is closed, so an unknown
    attribute is not ignored the way an unknown map attribute is
    (§3.1). A `peer=` on a role that does not need it parses and is
    ignored, because the grammar permits it on any attr list. The
    order of the attach's refusals is: the specifier, then the msize
    floor, then the epoch compare, then the `role=repl` membership
    check.

26. **`fence off` is refused only under a lease fence.** §2.5 listed
    `fence off` among the verbs that MUST fail `fenced` while the
    instance is fenced, and §6.4 F4 makes `fence off` the only way to
    clear an operator fence. Read together they made an operator
    fence permanent: the verb that clears it was refused because it
    was in force. *Made:* §2.5 now scopes that refusal to the
    **lease** fence, which is the fence a deposed instance carries
    and the one `fence off` MUST NOT clear anyway, so the rule §2.5
    is protecting survives without F4 becoming one-way.
    `decisions.md` D25 carries the argument. The refusal itself is
    unreachable in this build: the lease half of the fence can never
    be raised while the map is static (§14(19)), so `fence off` always
    clears the operator fence here. The check that carries the rule is
    in place and answers `fenced` the moment a refresh loop gives F1 a
    real clock.

27. **A ctl verb naming a malformed oid answers `bad object
    name`.** §2.5 answers a known verb with bad arguments `bad ctl`;
    §2.6 makes `bad object name` the answer to "any operation naming
    an oid that violates §1.1". *Not made:* the more specific string
    wins, so `verify` and `drop` — and every later verb that names an
    object — answer `bad object name` for an id §1.1 forbids and `bad
    ctl` for every other argument fault, including the wrong number
    of them. §2.5's error column for `verify` and `drop` lists `bad
    ctl` and not `bad object name`, so a client reading that column
    alone would not expect the more specific one.

28. **A multi-element walk cannot carry a §2.6 error.** §2.6 makes
    `no such object` the answer to a walk of an id nothing holds, and
    `object deleted` the answer to a walk of a tombstone. 9P answers
    a walk that failed after its first element with a **partial**
    `Rwalk` and no error at all, and `lib9p` implements exactly that.
    *Not made:* a one-element walk from an `/obj` or `/meta` fid —
    which is what a client library holding such a fid issues, and
    what an operator's `ls` resolves to — answers §2.6's string; a
    two-element walk from the root answers a partial `Rwalk`, and the
    client learns only that the name did not resolve. The rule is
    9P's and no server can hold both halves at once.

29. **Two local error strings, and what marks them.** §3.7 makes the
    mapping rule normative — a §2.6 condition answered with §2.6's
    prefix and nothing else, an internal or device error never
    beginning with one — and leaves what the server does with an
    internal string to the server. *Not made; recorded here as this
    server's policy:* every error that is not a §2.6 condition goes
    on the wire under the prefix `shoalsrv: `, which shares no prefix
    with any §2.6 entry, so the marked set stays prefix-free against
    §2.6's and the rule is visible to a client rather than merely
    intended. `store closed`, `store condemned: …`, `i/o error`,
    `stage expired` and the `Eobj:` family are that case. A file or a
    ctl verb whose content is not built answers the single local
    string `shoalsrv: not built`, **after** its role gate, its row's
    gate and the fence, so the gates are complete and testable before
    the content is. A `Tread` and a `Twrite` have no role gate of
    their own — 9P settles the role at the open, which is where
    §2.1's matrix is applied — and the row's gate runs on them as it
    does on an open (§14(24)). `/repl` and `/rpc`, a `Tremove` or a
    `Twstat` of the `/obj` and `/meta` directories themselves — no
    cell of either row answers one, which §2.4 does not define — and
    the ctl verbs `pull`, `push`, `reconcile`, `advert`, `refresh`
    and `register` answer it today; the status files, the two
    directories' opens, reads and creates, the object rows entire and
    the other ctl verbs are built and answer their own. A caller sees
    it only where those gates pass: a `role=admin` create or write of
    an id that is not reserved never reaches it, because §2.1 makes
    that `permission denied` (§14(24)), and neither does anything F3
    or the fence refuses.

    The same marking is what keeps §5.4.1's `interrupted` apart from
    the device's. A flushed request is answered `interrupted`, the
    word `reqqueueflush` gives a request it removed from a queue; a
    device call aborted by a note with no `Tflush` behind it is an
    error this server did not anticipate like any other, so it is
    answered `shoalsrv: interrupted`. §7 unwinds both into the whole
    of step 7 — what the request had staged is discarded either
    way — but only the queue's flush flag says a request was
    flushed, and the two answers keep that distinction where a
    client can see it.

30. **`forget` is background work, which §2.5 does not say it is.**
    layer-a §2.5 marks `scrub` as starting a job and says nothing of
    the kind about `forget <iid>`, whose plain reading is that the
    records are gone when the `Rwrite` comes back. Each record it
    discards is one `dirtydel`, which is one durable commit (§2.6),
    and the dirty region holds as many records as the disk was
    formatted for — so a `forget` answered on the service loop parks
    that loop for that many commits, and layer-a §5.4.1 requires a
    `Tflush` to be answerable throughout. It cannot be a queued row
    either: a queued row's `argv[0]` is an oid the framework hashes,
    and `forget`'s is an instance id. *Not made:* the verb answers
    success once the pass is accepted, in §2.5's own words for the
    verbs that do start work, and `/jobs` carries a `job=forget` line
    until the records are gone. An operator who reads `/dirty`
    immediately after the `Rwrite` may still see them. What the verb
    achieves is the discard alone: §7.1 has it mark the peer
    `fullsync`, the engine has no setter for that flag, and §9
    records that nothing clears it and that a peer is registered with
    it already set — so the coarse half of §7.1's meaning is in force
    for every peer before the verb runs.

    Being a pass gives `forget` no bound of its own: `scrub` is kept
    to one pass per index by a flag, but `forget <iid>` names a peer,
    and a client could write it once per id it can spell — each write
    a proc holding a dirty snapshot and one of the jobs the shutdown
    waits on. *Not made:* **twelve passes may run at once**, counted
    over every verb that starts one — `scrub`, `reclaim` and
    `forget`, so `job=` at `/jobs` carries one of those three — and a
    verb that would start a thirteenth is refused with the local
    `shoalsrv: too many jobs` rather than accepted. So is a pass the
    reclaim timer would start (§14(39)), which is the one pass no
    verb asked for. §2.5's error column for these verbs names
    `fenced` and `bad ctl` and neither covers this, so the refusal is
    a local string (§14(29)); the number is implementation policy.
    `/jobs` renders **every** job on the list, which §2.2's "one line
    per running or queued background job" requires and which the cap
    bounds the cost of.

    **One line shape for all three passes.** §2.2 fixes one record
    per job and leaves the fields to the implementation, and the
    fields here are one set rather than one per verb — so a reader
    parses one line and a pass's counters are read the same way
    whichever verb started it. What that costs is fields that stand
    still: a `job=reclaim` line's `rate=` is the scrubber's pace and
    the walk reads no grains, and its `bad=`, `skipped=` and
    `dropped=` stay 0 for the whole run, because the walk verifies
    nothing, pushes nothing through a queue and discards nothing
    (§14(31)). `done=`, `total=`, `reclaimable=` and `err=` are the
    four that move on it. The alternative — a line shape per verb —
    buys an operator nothing a constant field does not already tell
    them, and costs every reader of `/jobs` a second parse. Policy,
    like the rest of the line.

    A pass has no client to answer and this build has no operator log,
    so a walk that broke off — a failed index read, a failed
    `dirtydel`, a snapshot that would not open, an object whose grains
    would not read — reported success by saying nothing. *Not made:* a
    pass records what it gave up with, and `/jobs` renders it as a
    trailing **`err=<string>`** on that job's line, present only when
    there is one and last on the line because the string may hold
    spaces. §2.2 makes `/jobs`'s format implementation policy beyond
    its being one record per line, so the field is policy entire. The
    same record is what says a count on the line is not the whole
    store's: a reclaim pass cut short leaves `reclaimable=` over a
    prefix of its snapshot, which is indistinguishable from
    `reclaimable=` over the whole of it without the mark (§14(31)).

    A **per-object** read failure is a failure of the pass and not the
    end of it: the scrub records it and walks on, since one object
    that would not read says nothing about the next and stopping would
    leave the rest of the index unverified as well. That is the one
    way a pass reaches the end of its walk carrying an `err=`. An
    object that has merely **gone** between the index read
    and the queue is not a failure at all: the walk reads the index
    outside every queue and its unit is ordered behind whatever that
    queue held, so a delete or a drop in between is the ordering
    working. The engine's two answers for such an id — `no such
    object` and `object deleted` — are told apart from a read failure
    by the string, and `/jobs` counts them at **`skipped=<n>`**
    (policy, like the rest of the line).

31. **The scrub's pace is this server's (§8; layer-a §2.5,
    §7.5).** layer-a §7.5 leaves
    `scrubdays` and the rate to the implementation and sizes its own
    example at "a default near 14 days ... ~4 MiB/s on a 4 TB disk".
    *Not made:* `scrub rate=<n>` is in KiB/s as §2.5 says, the
    default is 4096 KiB/s so that an operator who sets nothing gets
    layer-a's own sizing, and the pace is over the bytes hashed —
    each object charged its own `len` plus a 1 KiB floor, because an
    object of no bytes still costs a queue push and an index read.
    `scrub` with neither `start` nor `stop` changes nothing and
    succeeds; a `start` while a pass runs is accepted and starts
    nothing, since the job asked for is already running; the two
    words are read in §2.5's own order and each at most once, so a
    line that reorders or repeats them is `bad ctl`.

    A `start` while a pass is **stopping** — told to stop and not yet
    wound down — is refused instead, with the local `shoalsrv: scrub
    stopping` (§14(29)), because the job asked for is not running and
    is not going to be: that pass reads the flag between two objects
    and gives up. *Considered and rejected:* clearing the stop flag so
    that the pass carries on, which races the pass's own read of it —
    told before, it walks on; told after, it has already broken off —
    and a verb whose effect turns on that race cannot be described to
    a client. The `rate=` on a refused line stands, as it does for the
    other refusals.

    §9's tombstone reclaim is a pass of its own, on its own timer and
    under its own verb; §14(39) has the decision and what it costs.
    What stays here is the walk itself, which is the same walk either
    way.

    **The walk counts and discards nothing.** layer-a §1.5 licenses a
    discard only when **all three** of its conditions hold. Two are
    local and are what the walk tests: condition 2, `tombdays` of
    retention since the entry's `mtime`, read from the map header
    (§3.1) because the engine does not hold it; and condition 3, the
    map epoch strictly above the entry's `wepoch`. Condition 1, full
    confirmation from every instance in the map whose `status` is not
    `dead`, is **open**: this build has no outbound peer client
    (§14(18)), so no instance has confirmed anything and condition 1
    blocks every discard. §1.5 names discarding on 2 and 3 alone as
    the first draft's unsound rule and the resurrection hole it
    leaves — a holder absent since before the delete returns with the
    live copy, and absence loses arbitration (§1.3). *Not made:* the
    walk reports instead. `/jobs` carries `reclaimable=<n>`, the
    tombstones past conditions 2 and 3, which is the number of
    discards the replication surface will have to confirm; every
    record stays where it is, and `/tombs` still lists it.

    A reclaim walk that is itself cut short — `reclaim stop`, or the
    shutdown, read between its entries — has counted a prefix of the
    snapshot. *Not made:* such a walk records `shoalsrv: stopped`
    through the
    same `err=` §14(30) gives a pass that gave up, so `reclaimable=`
    is read beside an error rather than as the whole store's answer.
    The count it did reach is left on the line, since a prefix marked
    as one is worth more to an operator than no number at all. One
    string covers both causes: what has to be known is that the
    number is a prefix, not which of the two cut it short. `done=`
    and `total=` are the walk's own entries and not the index's
    slots, so a prefix shows there too — but a `done=` short of
    `total=` is also what a walk still running reads, and the mark is
    what says the walk is over.

    The walk
    reads the tombstones through a §9 snapshot of its own and holds it
    for its whole run, so it occupies one of `objsnapmax`'s slots and
    `/status`'s `objsnapopen=` counts it: an operator who watches that
    field climb by one with a `job=reclaim` line at `/jobs` is
    watching the walk, not a client. When the replication
    surface lands it adds condition 1 and §1.5's execution — `op=
    discard` to every confirming instance, this instance's own record
    removed last — and every discard then goes through that oid's
    queue, as the scrub's own reads do.

32. **`newmonid` cannot replace a pinned `monid`, and nothing logs
    it (layer-a §2.5, §6.3).** §2.5 has the verb *replace* this
    instance's pinned `monid` and MUST log it. The engine's publisher
    for that value is `monidpin`, which pins a value on a store that
    has none and refuses a **different** one on a store that has
    (§2.2); every serving instance has one, since start-up pins the
    map's. *Not made:* the verb is built as far as that call reaches
    — the spelling, §2.5's gates, the durable publish and `/status`'s
    `monid=` — and a value that differs from the pinned one is
    refused with the engine's own words, which carry no layer-a §2.6
    prefix. What §2.5 asks for needs a re-pin in `lib/` that does not
    exist. The log is the second half of the gap: this build has no
    operator log at all, so the record of the verb is the change to
    `/status`'s `monid=`.

33. **A directory read's cursor is the server's, behind lib9p's own
    offset guard (§9).** §9 addresses a snapshot's entries by
    position so that a server can map a `Tread` offset onto an entry
    and restart from 0 on a re-read. lib9p's `dirread9p` cannot do
    that here: it regenerates entry *n* per `Tread` from the
    generator's index, and §9's two *gone* conditions mean the *n*'th
    entry is not the entry it was, so a listing addressed by entry
    number would skip or repeat around a discarded one. *Not made:*
    the `/obj` and `/meta` rows carry their own (offset, position)
    pair on the fid. lib9p refuses a directory read at an offset that
    is neither 0 nor where the fid left off before the row's read
    cell is reached (`/sys/src/lib9p/srv.c`'s `bad offset`, against
    its own `Fid.diroffset`), so the cursor's own refusal is behind
    that guard on the wire — reachable only where the two can
    disagree, which is a read this server answered `interrupted`
    after it had advanced the cursor. The cursor keeps the offset the
    previous read started at and rewinds to it for exactly that case.

34. **The write path with no peers: one placement member acks alone,
    and any other member is `degraded`.** layer-a §5.4 step 4 sends
    the operation to every acker and step 5 resolves the ack set;
    §14(18) leaves this build with no peer client, so nothing is ever
    sent. *Not made; recorded here as what the server does:* `M` —
    every member of `P(o)` other than this instance that has not
    durably committed the update — is computed from the map and is
    every other member there is. With `replicas=1`, or wherever
    placement yields this instance alone, `M` is empty and step 5's
    fast path commits: the write is acked on one disk, which is what
    `mincopies=1` licenses. With any other member in `P(o)`, `k` is 1
    and step 5a owes a durable stale mark at the monitor **before**
    the primary may proceed; there is no monitor client, so that
    round trip cannot be made and step 5a's own rule sends the
    operation to step 7 — `degraded` (§6.5), whatever `mincopies`
    says. A placement with any member other than this instance
    therefore refuses every client write, create, truncate and remove
    until the monitor client exists: liveness is not consulted, so a
    mirror whose peer is `up=no` and a healthy `replicas=2` pair are
    refused alike. Reads are unaffected, not being on that path. T1
    maps use `replicas=1` for that reason.

    The four client mutations take that path; an operator's write of
    a reserved `shoal.` id (§2.1) does not, being §8.6's rebuild path
    and not §5.4's client write. §2.4's `create` is the one of the
    four that stages nothing on its fid: the fid it is issued on is a
    directory fid whose state is the enumeration's until the create
    succeeds, so the key it chooses travels to the one engine call
    that publishes it and no stage of it exists for step 7 to find.

    **A `Tcreate` and a `Topen` CAN be pipelined on one `/obj` fid,
    and the cells refuse the second.** `lib9p` refuses each of them on
    an open fid from `Fid.omode`, which its `ropen` sets only once the
    open has answered; this server offloads the `/obj` open to a
    queue, so a `Tcreate` sent behind a `Topen` on the same fid — or a
    `Topen` sent behind a `Tcreate` — passes that guard and both cells
    run, on two queue procs at once. Accepting both would move the fid
    to the created object *and* install a listing's snapshot on it,
    and `lib9p` would then write the loser's `qid` and mode over the
    winner's. *Not made; recorded here as what the server does:* the
    create cell refuses a fid an open has already answered on, a fid
    that holds a listing's state and a fid another create is moving,
    and the open cell refuses to install over a fid a create has moved
    or is moving; both answer `lib9p`'s own `9P protocol botch`, which
    is the string `lib9p` answers when it *can* see the conflict, so a
    client cannot tell the two refusals apart. The create claims the
    fid before its first engine call and the open tests that claim
    under the lock it installs under, so exactly one of the two wins
    whichever way the procs interleave; an open that arrives while a
    create that then fails holds the claim is refused too, which is
    the same client that pipelined the pair. A create that succeeds
    gives the directory fid's snapshot back through `srvfidgive` — the
    fid's own hooks, run before the fid is the object's — and a create
    that fails leaves the fid holding what it held.

    A second `Topen` is not that case and is not refused: it leaves
    the fid a directory fid, and the open cell's own give-back is
    what releases the first open's snapshot before the second's is
    installed. That give-back is why the create reads `Fid.omode` too
    and not the fid's state alone: it runs outside the fid's state
    lock, so a second open leaves the fid holding nothing between the
    give-back and the install, and a fid with an empty state slot and
    no create moving it is what a create sees whether it arrived
    first or third. `Fid.omode` is set once an open on that fid has
    answered, so it is what tells the third message from the first —
    `lib9p`'s own guard, read one message later than `lib9p` reads
    it, which is what offloading the open costs this row.

35. **No currency check is made, so `cur=` is 0 and `ready=` is
    `no`.** layer-a §5.1 serves a `role=client` read only from an
    instance that is (a) the serving primary, (b) past the handoff
    grace and (c) current for the object, and §5.4 step 1 asks the
    same of a write. A currency check is `op=meta` to every witness
    (§5.2), which needs the peer client §14(18) says is not built.
    *Not made:* clause (a) is computed from the static map and IS
    enforced — a `role=client` operation on an object this instance
    is not the serving primary for answers `not primary: <iid>`,
    §2.6's own detail form — and clauses (b) and (c) are not
    evaluated at all. The instance serves the copy it holds. So
    `/meta`'s `cur=` reads 0 for every object, which is the value
    §2.4 provides for ("or `0`"), and `ready=` reads `no`: an
    instance that has completed no check is not ready under (c), and
    the field is advisory. The two are the same fact said twice, and
    both become live with the first peer client.

    `/meta`'s two computed fields have an empty case §2.4 does not
    spell. A map in which no node places at all leaves `P(o)` empty,
    and one in which no member of `P(o)` is `up` has no serving
    primary (§4.3) — `object unavailable` to a client, and still a
    line an operator may read. *Not made:* `placement=` and
    `primary=` render `-` there, because §0 makes the record one
    `attr=value` line and an attribute with no value at all is not
    one. A `-` names no instance: §3.3's node names carry no `.`, so
    every iid has one.

    §5.2's own escape hatch is unreachable here for the same reason.
    Clause 2 has an instance that does not hold the `E−1` map either
    fetch `/maps/<E−1>` from the monitor or substitute every instance
    with `status` ∈ {new,in,out}; clause 4 and the skip rule take no
    substitute at all. That fetch is the **server's** obligation and
    not the map library's — `mapwitness` (D22) answers on the maps it
    is handed and substitutes for clause 2 alone — and with no
    monitor client there is no fetch. Were a check made here it would
    evaluate clause 4 on `P(o)` at `E` alone, which is what §5.2
    prescribes when the `E−1` map is absent.

36. **§2.4's "a write on a fid opened `OREAD`" cannot be answered
    `bad open mode`.** §2.4 makes that refusal a MUST. `lib9p`
    answers a `Twrite` on a fid whose `omode` is not `OWRITE` or
    `ORDWR` itself, with its own `9P protocol botch`, before
    `Srv.write` is reached (`/sys/src/lib9p/srv.c`, `swrite`), so no
    server built on `lib9p` can hold this half of the rule. *Not
    made:* the client is told the same thing in `lib9p`'s words. The
    rest of §2.4's mode rules are the row's own and are answered as
    §2.4 spells them: `ORCLOSE` and any mode that is not `OREAD`,
    `OWRITE` or `ORDWR` with or without `OTRUNC` are `bad open
    mode`, and `DMDIR`, `DMAPPEND`, `DMEXCL` and `DMTMP` on a create
    are `bad create mode`. This is §14(28)'s shape: a rule 9P's own
    layer answers first.

37. **The per-fid stage bound shortens a client write rather than
    refusing it.** §3.6 bounds one `/repl` fid to `stagemax` staged
    grains and fails a chunk that would exceed it with `disk full`.
    A client `Twrite` is not a chunk: §2.4 lets a server answer a
    **short write** and requires clients to loop, and §5.5 makes that
    how a primary bounds one operation to what fits one peer message.
    *Not made; recorded here as this server's policy:* one accepted
    write covers at most `stagemax` checksum blocks and is answered
    short at that boundary, so the bound is back-pressure and not a
    refusal. `disk full` on that bound is still reachable, and is
    what a fid that already holds a stage is told — one stage to a
    fid — which is the replication surface's case, where a chunk's
    offset is the sender's and shortening would publish a hole.

    **The quantity bounded is not §3.6's.** §3.6's `stagemax` bounds
    the grains a `/repl` fid holds RESERVED, and a client `Twrite`
    reserves none: it stages the key §5.4 step 3 chose and commits
    from the request's own buffer, so the stage of a client write
    holds no grain at all. What the 9P server bounds with that same
    configured number is the count of **checksum blocks one accepted
    write may cover** — its own policy, named as such, sharing the
    value because the two bound the same appetite for one operation.
    The process-wide `stagetot` bounds reservations and so has
    nothing yet to bound in this server: it is the engine's, enforced
    in the store over the handles `stageopen` makes, and it and
    `/status`'s `staged=<grains>` become live together with the
    `/repl` surface (§5.5) whose stages reserve. `/status` carries no
    `staged=` row until then.

    **Which string a client sees when its staged update goes.** A
    `Twrite` whose own request was flushed is answered `interrupted`
    (§14(14)). One that finds its stage gone for any other reason —
    step 7 for a *different* request on the same fid, or the idle
    sweep — is answered this server's own
    `shoalsrv: staged update discarded`, which carries no §2.6 prefix
    because nothing §2.6 names has happened: the update was neither
    applied nor refused on its merits, and the client retries. The
    engine's `stage expired` is the same condition said to the other
    kind of owner — a `/repl` sender CONTINUING a transfer whose
    earlier chunks are gone (§3.6) — and a client operation continues
    nothing, so the two never reach one caller.

38. **A `Twstat` that sets a field other than `length` carries no
    §2.6 string.** §2.4 requires every other settable field to be
    rejected and names a condition for one of them alone —
    `no rename` for `name`. Nothing in §2.6 fits `mode`, `mtime`,
    `atime`, `uid`, `gid` or `muid`, nor the `type` and `dev` that
    `stat`(5) makes don't-touch on every `Twstat`. *Not made:* the
    rename is `no rename` and the rest are refused with this server's
    own `shoalsrv: only length may be set` (§14(29)); a request that
    carries a `type` or a `dev` is asking for something this server
    will not do, not naming a field to ignore. `qid` is the third
    don't-touch field and is not this row's to refuse: `lib9p`
    answers a `Twstat` whose `qid` differs from the fid's own with
    its own string before `Srv.wstat` is reached
    (`/sys/src/lib9p/srv.c`, `swstat`), which is §14(28)'s shape
    again, and a `qid` equal to the fid's sets nothing. A `Twstat`
    that sets nothing at all is 9P's own sync of a fid and succeeds,
    changing nothing.

39. **The tombstone reclaim walk is a job of its own, with a verb
    and a timer (§9; layer-a §2.5, §1.5).** D26 tied the walk to the
    end of a `scrub` pass so that §2.5's ctl grammar would not have
    to change. That couples a walk measured in seconds to a pass
    layer-a §7.5 itself sizes at "a default near 14 days": a mass
    delete's space is reported only after a whole scrub, a `scrub
    stop` forfeits the walk with the pass that carried it, and no
    tier below a full pass can exercise it at all. *Made:* layer-a
    §2.5 carries a `reclaim [start|stop]` row, admin, with `start` in
    the fenced set, and the walk is a background job like the other
    two — `jobstart`, a `job=reclaim` line at `/jobs` with its
    `reclaimable=` and its `err=`, the same twelve-pass cap
    (§14(30)), the same stop mark and the same duty to test the
    shutdown between entries. D26 carries the decision and the wire
    change it is.

    **`start` is fenced and `stop` is not.** §2.5's fenced set is
    "every verb that mutates data or replication state" and the walk a
    `start` asks for will: the discard is what it is a walk for, and a
    fenced instance is one whose map may be stale — which is the map
    `tombdays` and the epoch are read from. `start` is fenced today,
    while it only counts, so that the set does not have to change
    under a client when condition 1 becomes answerable. `stop` mutates
    nothing — it raises a flag a running walk reads between two
    entries — and an instance just fenced is where an operator most
    wants the walk it started stopped, so it is answered while fenced
    as `scrub stop` is. layer-a §2.5's fenced set names the two forms
    apart for that reason, and the ctl row here is outside the set
    with the gate for `start` in the verb's own body, which is how
    `fence off`'s carve-out is built (§14(26)).

    **The timer, and its period.** A pass runs every `tombdays`/2,
    which is layer-a §8.3's own cadence for the `bump` that makes
    §1.5's condition 3 reachable: the walk waits on the epoch moving
    past each tombstone's `wepoch`, so looking oftener than the epoch
    moves asks a question whose answer cannot have changed. The
    period is **implementation policy**, and so is the floor under
    it: a map may carry `tombdays=0` — the cutoff is then the present
    (§14(31)) — and `tombdays`/2 is then no period at all, so the
    timer is floored at twelve hours, which is what the shortest
    non-zero retention a map can carry gives. The first pass comes
    one period after start-up rather than at it, so that an instance
    restarted often does not walk its index at every start.

    **What the verb does not do, and the half that is open.**
    `reclaim stop` stops the pass that is running; it does not turn
    the timer off, and the next tick starts a pass as a `reclaim
    start` would. §2.5's form has two words and neither names a
    schedule, and an operator who wants the walk off for good has no
    verb for it — which costs a walk of the tombstone snapshot per
    period and no durable change, since the walk discards nothing.
    That is the **open half**: `scrub` has no schedule of its own yet
    (layer-a §7.5's "continuously" is not built), so `scrub stop`
    means "stop this pass" and nothing else, and the two verbs agree
    today by accident. When the scrub gets a scheduler, a `stop` that
    holds the schedule until the next `start` is the reading that
    verb will want, and this one's meaning has to be settled against
    it — either the same reading here, or a form of §2.5's own for a
    schedule. Nothing on the wire commits either way today: the two
    `stop`s do the same thing. The timer is not gated by the fence
    either: it starts no discard while condition 1 is unanswerable,
    and what the fence governs is `start`, which is the surface §2.5
    has.

    **What it is not.** The timer proc holds none of §9's background
    jobs, because it makes no engine call — the pass it starts holds
    one for its own run — so the shutdown waits for the proc
    separately and before the jobs, since it is the one thing that
    could still start one. layer-a §7.5's "continuously" for the
    scrub is a **separate** matter and is not built: `scrub` still
    runs only when an operator asks for it.

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

- **A larger `blksz` than `Wunit` as the default** — the previous
  default of 64 KiB made a block four device requests and a 4 KiB
  client write cost a 64 KiB rewrite, and it made the "one commit
  record, one device request" property hold only for the log.
  Rejected as the *default* once `Wunit` was measured: there is no
  property a block larger than one device request buys, and the
  metadata it saves (a quarter of the map and digest arrays) is 0.4%
  of a partition. The geometry itself is still accepted — §2.1
  bounds `blksz` by the format and not by any device (§0).

- **Preallocating a full `objmax` extent per object** — no block map
  at all, one base grain per object. Rejected: 2.6·10^5 objects ×
  16 MiB is the whole 4 TB disk, with no sparseness and no holes.

- **A size-classed or grain-backed extent map** — extent-map entries
  sized to the object rather than to `objmax`, or held in ordinary
  grains allocated on demand. This is the recorded fix for §2.1's
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

- **A superblock-owner proc, and a tail assigner feeding a writer
  pool** — the shape of the previous draft: one proc owning the
  superblock image and answering publish requests, and one proc
  assigning `seq` and log offsets to batches that a pool of writer
  procs then wrote. Rejected as procs that buy nothing a lock does
  not: a `QLock` plus a publish function gives "one write at a time,
  built from live state" exactly (§2.2), and the classic group-commit
  shape — the arriving committer absorbs the queue and writes its own
  batch — gives the microsecond lock hold, the fixed batch
  membership, the watermark and the lone writer's one-flush-one-write-
  one-flush, with `logdepth` as a semaphore rather than a proc count
  (§7). Two procs, a message type and a handoff removed from the most
  crash-critical loop in the store.

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
   (§2.1). The parameter is cheap to change at format time and
   expensive to change afterwards.
10. **BLAKE2s throughput on the fleet**, to confirm the 50–59 MB/s
    measured here — every hashing term in §5, §8 and §11 scales with
    it.
11. **Whether the enumeration open's `qlstate` hold has to be
    chunked.** §9 measures it at ~22 ns a slot up to the highest
    occupied one, so ≈23 ms at `nslots = 2^20` — the one state-lock
    hold in the store of the same order as a commit's write. If T2
    shows it delaying `/status`, `/ctl` or a `Tflush` behind a
    `/obj` open, the shape that fixes it is a **chunked scan under a
    generation counter**: take the index in bounded runs, dropping
    and re-taking `qlstate` between them, and restart the scan when
    a counter bumped by every apply that creates, frees or re-states
    a slot shows the index moved under it. That keeps `objsnap=full`
    — a restart is not a partial vector — at the price of a scan
    that can be made to starve by a continuous create rate, which is
    why it is not built on speculation. **The counter itself is
    there**: §8's online bitmap rebuild wants it for a different
    reason — validating an extent map it re-reads outside `qlstate`,
    which the four-tuple cannot do — and carries it as a per-slot
    stamp in the index entry. So what is open here is the chunked
    scan alone, and not the counter under it. T2.

### (b) Product calls

1. **Metadata budget — the `nslots` and `nemap` defaults.**
   *Recommend* `nslots = min(2^20, 4 × partsize/objmax)` and
   `nemap = partsize/objmax`: 0.13% of a 4 TB disk, four times as
   many index slots as `objmax`-sized objects would need because
   Layer C's MDS objects will be small and numerous, and extent-map
   slots only for objects too big to carry their map inline. The
   alternative is a tighter default plus an operator who has to
   think; the cost of thinking wrongly is a disk that reports
   `disk full` with terabytes free, which is also §2.1's residual.
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
   restore, and refuse to start on two bad superblocks or on state
   the bitmap has already materialised that neither the superblock
   nor replay covers (§2.5) — but *not* on a bitmap page merely
   stamped ahead of the superblock, which is the ordinary
   mid-checkpoint crash. The
   alternative, starting anyway and letting the cluster arbitrate, is
   tempting because layer-a can in fact heal it; it is rejected
   because a store that starts in a mode it did not name is how an
   operator loses a day.
7. **libthread.** *Recommend* the `Reqqueue` pool (§7), which makes
   the whole server a libthread program using `threadpostmountsrv`.
   The alternative is `srvrelease`/`srvacquire` with explicit
   per-object locks, which is plain libc — at the price of writing
   the per-object ordering, the flush path and the lock's memory in
   by hand. D12 records the choice.
