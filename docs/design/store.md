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
slot store, the tools, and the T2 test plan. Out of scope: the 9P
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
  unpacked with the `GBIT16`/`GBIT32`/`GBIT64` and `PBIT*` macros in
  the 9P convention. 9front's libc defines them per translation unit
  rather than exporting them, so `lib/shoal.h` defines them once for
  shoal and every store source uses those.
- Fixed-width byte fields (checksums, uuids, oids) are byte strings,
  not integers, and are not byte-swapped.
- A **sector** is the unit the device reports; `secsz` is recorded in
  the superblock at format time. All region offsets are in sectors;
  all sizes named `*secs` are in sectors, everything else in bytes.
- A **grain** is the store's allocation unit for object content. It
  is exactly `blksz`, so grain *i* of an object holds checksum block
  *i* (layer-a §1.4) and one number in one array locates both the
  bytes and the digest. §15 records the alternative.
- `Wunit` = 16384 bytes. The store MUST NOT issue a single `pread` or
  `pwrite` larger than `Wunit`, because `sdvirtio` splits a request
  at 32 sectors and issues the pieces serially, so a larger call buys
  nothing and only obscures what one device round trip costs
  (`docs/platform/9front-storage.md`). It is also comfortably under
  any `SDmaxio`.
- **Every persistent record carries a checksum**, and it is
  BLAKE2s-128 — a 16-byte unkeyed BLAKE2s digest, the same primitive
  and the same length as a block digest (D7, layer-a §1.4). Chosen
  over a 32-bit CRC because D7 already puts BLAKE2s in the build and
  one primitive means one implementation and one vector set; chosen
  over the 256-bit variant because the 128-bit digest is what
  layer-a §9 already calls ample for non-adversarial corruption
  detection, and because 16 bytes fits a record header without
  pushing it into a second sector. Hashing 16 KiB costs tens of
  microseconds against an 8.4 ms device write.
- A record's checksum is computed over the record's whole byte range
  **with the checksum field itself zeroed**, and verified the same
  way.
- Every header begins `magic` then `vers`. A store MUST refuse to
  open a structure whose `vers` it does not implement, and MUST say
  so rather than guessing. There is no in-place format upgrade in
  v1: a `vers` bump means reformat and refill from peers.

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
  `blksz` default 65536, both powers of two, both immutable
  cluster-wide; a write past `objmax` fails `object too large`.
- **R11 object count.** ~2.6·10^5 objects on a 4 TB disk at the
  defaults; the design is sized to ≤ 2^20 per disk and says where that
  bound is used to buy simplicity.
- **R12 snapshot-at-open enumeration** (layer-a §2.2). A sequential
  read of `/obj` or `/tombs` neither skips nor duplicates an entry
  because of concurrent creates and deletes.
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
    emapoff  .. +emapsecs   extent maps, nslots × emapsz
    dirtoff  .. +dirtsecs   dirty records, ndirty × 256 bytes
    bmapoff  .. +2*bmapsecs free-grain bitmap, two copies
    dataoff  .. +datasecs   data grains, ngrains × blksz
    last sector             superblock, copy 1

Copy 1 sits at the last sector of the partition so that no single
device request, and no plausible localised media fault, can reach
both copies.

Derived quantities, all recorded in the superblock so no reader
recomputes them from assumptions:

    nblkmax = objmax / blksz                    (256 at the defaults)
    emapsz  = roundup(24 + 20*nblkmax, secsz)   (5632 at the defaults)
    ngrains = datasecs / (blksz / secsz)

`ngrains` MUST be < 2^32: grain numbers are `u32`, and grain 0 is
reserved to mean *no grain* — a hole — at the cost of one unusable
grain at `dataoff`. At `blksz` 65536 that bounds one instance at
256 TiB of data, far beyond the envelope.

**Sizing.** `shoalfmt` defaults `nslots` to
`min(2^20, 4 * ceil(partsize/objmax))` and `logsecs` to 64 MiB. On a
4 TB partition at the defaults that is `nslots = 2^20`, and the
metadata regions cost 256 MiB (index) + 5.5 GiB (extent maps) +
16 MiB (dirty) + 16 MiB (bitmap) + 64 MiB (log) ≈ 5.9 GiB, **0.15% of
the partition**. The factor of 4 over `partsize/objmax` exists
because Layer B fills objects to `objmax` but Layer C's MDS objects
will not; an operator who knows better overrides it with `-n`.

**One instance per `sd` unit.** The device flush (§3.2) is issued
through `/dev/sdXX/raw`, which `devsd` opens exclusively per unit. An
instance therefore holds its unit's raw channel for its whole life,
and two shoal processes MUST NOT be configured on partitions of one
unit. §14 records that D4 says "one instance per disk" without
saying this.

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
     92     4  ndirty
     96     8  ngrains
    104     8  logoff
    112     8  logsecs
    120     8  idxoff
    128     8  idxsecs
    136     8  emapoff
    144     8  emapsecs
    152     8  dirtoff
    160     8  dirtsecs
    168     8  bmapoff
    176     8  bmapsecs  per copy
    184     8  dataoff
    192     8  datasecs
    200     8  ckseq     highest log seq the checkpoint covers
    208     8  cklogoff  log sector where replay must begin
    216     4  ckbmap    which bitmap copy is current, 0 or 1
    220     4  pad
    224     8  qidnext   qid.path high-water, layer-a §2.3
    232     8  epochhigh highest map epoch adopted, layer-a §6.3
    240    16  monid     pinned monitor identity, layer-a §6.3
    256     4  monidset  0 until the first map is adopted
    260   252  reserved, zero

**Two-slot rule.** An update writes the copy with the *lower* `gen`,
setting `gen` to `max+1`, then flushes. On start the store reads both
copies and takes the valid one with the greater `gen`. A torn or lost
superblock write therefore always leaves the other copy valid and
one generation behind, which §5 shows is recoverable. This is the
same rule as the monitor's map slots (§10) — one construction, used
twice.

The superblock is written rarely: at format, at each checkpoint (to
publish `ckseq`, `cklogoff`, `ckbmap`), when `qidnext` advances a
batch, when the pinned `monid` is first set, and when `epochhigh`
advances. It is never in the object write path.

`qidnext` advances in batches of 1024: the in-memory counter is
handed out one at a time, and only when it reaches the recorded
high-water is the superblock rewritten with `qidnext += 1024`. So a
create costs no superblock write 1023 times out of 1024, the counter
is monotonic across restarts (the recorded value is always ≥ any
value ever handed out), and a crash wastes at most 1023 paths out of
2^64. This is what makes R16 true without a durable write per create.

`epochhigh` is written when the instance adopts a map with a higher
epoch. Epoch bumps are bounded by map changes and the `tombdays`/2
heartbeat, not by write rate (layer-a §6.1), so this is a rare 8.4 ms
write and never inside a client operation.

### 2.3 Index region

`nslots` fixed-size entries of **256 bytes**, indexed by slot number.
The entry is the object's published record: everything a `/meta`
line, an `/advert` line or an `op=meta` response needs, and nothing
that scales with object size.

    off  size  field
      0     1  state     0 free, 1 live, 2 tomb
      1     1  oidlen    1..128
      2     1  flags     bit0 corrupt (layer-a §7.5)
      3     1  vers      entry format version, 1
      4     4  pad
      8     8  qidpath
     16     8  len
     24     8  ver
     32     8  wepoch
     40     8  mtime
     48    32  csum      the object checksum, layer-a §1.4
     80   128  oid
    208    16  csum128   BLAKE2s-128 over the entry, this field zeroed
    224    32  reserved, zero

A slot's number is not its `qid.path`: slots are recycled when a
tombstone is discarded, `qid.path` values are not.

Per-entry rather than per-page checksums, at a 6% cost in the index
region, so that a localised fault damages one object rather than 64.
A slot whose checksum fails at start is not silently dropped: it is
listed in `/lost` with `kind=corrupt` and its slot is not reused
(§5).

### 2.4 Extent-map region

`nslots` entries of `emapsz` bytes, at the same slot index as the
index entry. This is the per-object block map and digest array
together, so one device read gets both.

    off       size            field
      0         16            csum128, this field zeroed
     16          4            nblk, blocks covered = blkcount(len)
     20          4            vers, 1
     24    4*nblkmax          grain[i], u32; 0 means hole
     24+4*nblkmax
           16*nblkmax         dig[i], BLAKE2s-128 of block i
    ... zero to emapsz

Two parallel arrays rather than an array of 20-byte structs, so both
are naturally aligned and the digest array is a contiguous byte range
that can be fed to `csumdigests` in one call.

At the defaults an entry is 5144 bytes in 5632 bytes of space — 11
sectors, one device request. Array entries beyond `nblk` are zero
and take no part in the object's `csum`.

`grain[i] == 0` is a hole: block *i* has no grain, reads as zeros,
and `dig[i]` is the digest of `min(blksz, len - i*blksz)` zero bytes.
That value for a full block is a constant the store computes once at
start; the short final block's zero digest is computed when needed.
Holes therefore cost no space and no special case in `csum`
(layer-a §1.4, R13).

### 2.5 Free-grain bitmap

Two copies of `bmapsecs` sectors each. A copy is one header sector
followed by `ceil(ngrains/8)` bytes, bit *g* set meaning grain *g* is
allocated.

    off  size  field
      0     8  magic  "shoalbm\0"
      8     4  vers
     12     4  pad
     16    16  csum128 over the whole copy, this field zeroed
     32     8  ckseq  the checkpoint generation this copy belongs to
     40     8  ngrains
     48   ...  reserved to secsz, then the bits

A checkpoint writes the copy that is *not* named by the superblock's
`ckbmap`, then the superblock names it. The bitmap is authoritative
only as a checkpoint: the truth is the set of grains referenced by
committed index/extent-map state, and §5 says what happens when the
named copy fails its checksum.

### 2.6 Dirty-record region

`ndirty` entries of 256 bytes (default `ndirty` = 65536, 16 MiB). This
is layer-a §7.1's fine-grained dirty set, which layer-a §5.4 step 5b
requires durable before the ack.

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
characters and an index is a decimal integer.

`ndirty` is an implementation limit in exactly layer-a §7.1's sense.
When it is exhausted the store discards every fine-grained record for
the peer with the most records and marks that peer `fullsync`, which
layer-a §7.1 explicitly permits.

**`fullsync` flags are not persisted.** On start the store sets
`fullsync` for every peer, because that is the safe default and a
restart has to run a reconcile pass anyway. This is stricter than
layer-a §7.1 requires and removes a durable structure entirely; the
cost is one reconcile pass after a restart, which is availability in
the conservative direction and never correctness.

### 2.7 Log region and record format

*Format.* A circular region of `logsecs` sectors. It carries every
durable state change other than object content: the published
four-tuple, extent-map deltas, grain allocations and frees, dirty
records, and slot frees.

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
the log region from this offset, and `csum128` verifies over the
whole `nsec*secsz` bytes. `nsec` MUST be bounds-checked before it is
used to address anything: a torn header can carry a garbage length,
and hashing an unbounded range on the strength of an unverified
field is how a replay turns a crash into a fault.

A record MUST NOT straddle the end of the region. If the next record
does not fit before the end, the writer emits a one-sector record
with `nent=0` and `Fwrap` set, and places the real record at the
region start. The wrap record carries a sequence number like any
other, so sequence contiguity is unbroken.

Entries are `{u8 kind, u8 flags, u16 len}` — `len` counting the whole
entry including this header — followed by the body:

**`Eobj` (kind 1)** — publish an object's state.

    u32  slot
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

`nmap` names only the blocks this commit changes; the rest of the
extent map is unchanged. `nfree` names the grains this commit
releases — the old grains of the blocks it replaced, and every grain
beyond a new shorter `len`.

**`Edirty` (kind 2)** — `{u8 op (0 remove, 1 add), u8 peerlen,
u8 oidlen, u8 pad, u64 epoch, peer[], oid[]}`.

**`Eslot` (kind 3)** — `{u32 slot}`, free the slot: a tombstone
discard (layer-a §1.5) or an `op=drop` (layer-a §7.4).

**Record sizes at the defaults, which is the point of this format.**
An `Eobj` for a 64 KiB single-block write is 140 bytes and fits in the
header sector: **a lone small commit is one 512-byte write.** An
`Eobj` for a whole-object 16 MiB `op=full` — 256 changed blocks and
256 freed grains — is a little over 7 KiB, so the record is fifteen
sectors: **still one device request under `Wunit`**. Every commit this
store can be asked for at the default `objmax` and `blksz` is
therefore one device write plus one flush. That property is why grains
are `blksz`-sized and why the map and digests are packed as one array
of triples: it is what makes §3.2's atomicity argument reach every
operation rather than just the small ones. At `blksz` below 64 KiB it
stops holding — a 4 KiB `blksz` puts a full `op=full` record at ~115
KiB, eight device writes — which is a reason to keep `blksz` at or
above the default and a warning `shoalfmt` emits.

### 2.8 What is checkpointed and what is authoritative

*Policy.*

The log is the durable authority for everything since the last
checkpoint. The index, extent-map, dirty and bitmap regions are a
**checkpoint**: a materialisation of the log's effect up to
`ckseq`, written lazily by a checkpointer proc, and re-derivable by
replay. Nothing in the write path writes them.

A checkpoint:

1. writes every dirty index page, extent-map entry, dirty record and
   the inactive bitmap copy — all in `Wunit`-sized pieces, coalesced
   per 16 KiB index page where slots happen to be adjacent;
2. issues one device flush;
3. writes the superblock with the new `ckseq`, `cklogoff` and
   `ckbmap`, and flushes.

Log space before the *newly published* `cklogoff` is reclaimed only
after step 3 has returned. Reclaiming earlier would let a crash leave
a superblock naming an older checkpoint whose log has already been
overwritten, which is the one way this format can lose data.

Checkpoints run when the log is half full or every `ckms` (default
30000) if anything is dirty — both policy, both tunable without a
format change.

## 3. Write path

*Policy for the mechanics; the invariants it establishes are
layer-a's.*

This maps layer-a §5.4 steps 3, 6 and 7 onto the format above. Steps
1, 2, 4 and 5a are not the store's business.

### 3.1 Stage — step 3

Under the object's exclusive lock (§7):

1. Read the extent-map entry if it is not in the cache (one device
   request, 11 sectors at the defaults).
2. For each block the operation touches, allocate a **fresh** grain
   from the in-memory free map. The allocator never returns a grain
   that any committed extent map references, and never returns one
   released by a commit that is not yet durable (§3.5). This is what
   makes a stage invisible: it writes only where nothing is
   published.
3. Compose each touched block's new content. A write covering a whole
   block needs no read. A partial write reads the old grain (or takes
   zeros for a hole), merges the new bytes, and re-hashes the block —
   §4 works this through.
4. Write the new grains. These writes are durable but unreachable: no
   index entry, no extent map and no bitmap names them.
5. Compute the new block digests, then the new `csum` by hashing the
   whole digest array with the changed digests substituted
   (`csumdigests`; 4 KiB of hashing at the defaults, tens of
   microseconds).
6. Build the `Eobj` entry in memory, together with any `Edirty`
   entries layer-a §5.4 step 5b calls for.

Nothing in this step is published. Nothing in it is a commit.

### 3.2 Commit — step 6

The staged entries are handed to the log writer, which batches them
with any other commits pending at that instant (§7) and performs:

    one device flush              — covers every stager's grain writes
    one pwrite of the log record  — the commit point
    one device flush              — makes the record durable

and then wakes the waiters, which answer `Rwrite`.

**Why one write plus a flush is the atomicity point.** The record is
valid only if its checksum verifies over its whole byte range and its
sequence number is the expected successor of the previous valid
record. A record that landed only in part fails the checksum. A
record whose header sector was torn fails the checksum too — a torn
header leaves either the old bytes or the new in every field, and
either way the stored digest and the hashed range disagree. A record
that did not land at all leaves the previous lap's record at that
offset, whose sequence number is exactly `logsecs`-worth of records
too low, and replay stops. So the design assumes **no multi-sector
atomicity at all**, and does not even need the single-sector
atomicity `docs/platform/9front-storage.md` leaves open: a torn sector
is caught by the same checksum as a missing one. What it does assume
is that a sector either has old bytes or new bytes and not, say,
bytes from a third write — which is the weakest assumption a block
device can be given.

The first flush is not optional and its position matters. `sdvirtio`
does not negotiate `VIRTIO_BLK_F_FLUSH` and the reference device
behaved write-through, but on AHCI with a volatile cache the commit
record could reach the platter while a staged grain sits in cache. A
device flush is device-wide, so one flush after every stager in the
batch has finished its grain writes covers all of them; it costs
175 µs against an 8.4 ms write. On a legacy IDE unit the flush is
silently acknowledged and never issued
(`docs/platform/9front-storage.md`), so on such hardware durability
additionally requires the drive's write cache to be off — the store
reports which it has in `/status`.

After the commit returns, the in-memory index entry, extent map,
free map and dirty set are updated, the grains the commit released
are put on the batch's deferred-free list (§3.5), and the object's
lock is released.

### 3.3 Discard — step 7 and `Tflush`

Return the staged grains to the in-memory free map, drop the
composed entries, release the lock. **No durable write.** The bytes
written into those grains in step 3.1(4) are simply not referenced by
anything and are overwritten by whoever allocates them next.

This is what makes layer-a §5.4 step 7's "the local object is
untouched: same content, same key, same `csum`, still verifying"
true by construction rather than by care: the published state was
never modified, so there is nothing to roll back.

### 3.4 Crash points

The invariant is R2: the published four-tuple is the old one or the
new one. Taking the points in order:

| Point | What is on disk | Outcome |
|---|---|---|
| P0 before any grain write | nothing changed | old |
| P1 during or after grain writes, before enqueue | new bytes in grains nothing references; the checkpointed bitmap does not mark them allocated and no log record allocates them | old |
| P2 after enqueue, before the pre-flush | as P1 | old |
| P3 during the log write | the record fails its checksum, or its sequence is not the expected successor | old |
| P4 after the log write returns, before the post-flush | the record is durable or it is not; if not, P3 | old or new, and nothing acked either way |
| P5 after the post-flush, before `Rwrite` | new | new; layer-a §5.4's "may or may not have been applied" covers exactly this |
| P6 after `Rwrite` | new | new, guaranteed — R1 |
| checkpoint, mid-way | half-written index/extent-map/bitmap pages; the superblock still names the *old* `ckseq` | replay re-applies from the old mark over the half-written pages; entries carry absolute values, so replay is idempotent |
| superblock write | the other copy is valid and one generation older | replay from its (older) mark, which the reclaim rule guarantees the log still covers |

A batch carrying several objects' commits is one record, so the whole
batch is at the same point at every instant. §7 explains why a batch
that lands while a lower-numbered batch does not is never acked.

### 3.5 The deferred-reuse rule

**A grain or index slot released by a commit MUST NOT be reallocated
until that commit's post-flush has returned.**

Without it: object A's commit frees grain *g* and is lost in the
crash; meanwhile *g* was handed to object B, staged into, and B's
commit was lost too. Replay restores A's old extent map, which still
points at *g* — whose bytes are now B's. A four-tuple that verifies
against nothing, produced by two writes that both correctly failed.

The implementation is a per-batch deferred-free list drained when the
batch's flush returns. It costs one pointer per batch and it is the
only ordering rule in the write path that is not obvious from the
log.

## 4. Read path, holes and re-hashing

*Policy.*

**Read.** Under the object's shared lock: clamp to `len` (a read at
or past `len` returns 0, a read crossing `len` returns only the bytes
below it), then for each block in range either `pread` from
`dataoff + grain[i]*blksz` or, when `grain[i]` is 0, deliver zeros.
The lock is held across the device reads so that no concurrent commit
can free a grain under the reader; §3.5 makes that sufficient and
§7 explains why holding it costs nothing that layer-a does not
already require.

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
  bytes and frees every grain beyond it.

A truncate that lands on a block boundary therefore costs no data
read and no data write at all: it is an `Eobj` with `nmap` empty and
`nfree` naming the released grains. So is a delete.

**The digest array is consistent with content by construction.** The
new grain and the new digest for block *i* are published by the same
`Eobj`, in the same record, at the same commit point; there is no
window in which one is visible without the other. That is R2 applied
to layer-a §1.4's second level, and it is why the digests live in the
same commit as the block map rather than in a structure of their own.

## 5. Start-up: recovery and replay

*Policy.*

1. Open the partition and `/dev/sdXX/raw`. If the raw channel cannot
   be opened — another process holds it — the store starts, reports
   `flush=unavailable` in `/status`, and says so on standard error:
   durability then rests on the device's cache being write-through.
2. Read both superblocks. Take the valid one with the greater `gen`.
   If neither is valid the store MUST NOT start; §12's tools are what
   the operator reaches for.
3. Check `secsz` against the device, and `vers` against the build.
   Refuse a mismatch.
4. Read the index region (`nslots` × 256 B) and build the in-memory
   index (§9). Verify every entry's checksum; a failure lists the
   slot in `/lost` with `kind=corrupt` and takes the slot out of
   service until an operator drops it.
5. Read the bitmap copy named by `ckbmap`. If it fails its checksum
   the store MUST NOT start and MUST print the one instruction that
   fixes it: `shoalck -R`, which rebuilds the free map by scanning
   every live extent map. That scan reads up to the whole extent-map
   region — 5.5 GiB at `nslots = 2^20` — and is the one operation in
   this design whose cost is a whole metadata region rather than a
   format constant. It is a repair path, not a start path.
6. Replay: scan the log from `cklogoff`, applying every valid record
   whose `seq` is the expected successor, and stop at the first
   record that is invalid or out of sequence. Applying an entry means
   setting absolute values — this slot's four-tuple becomes these
   bytes, block *i* becomes grain *g* with digest *d*, this grain
   becomes allocated, this dirty record exists — so replay is
   idempotent and a partially checkpointed region is corrected by it.
7. Read each replayed slot's extent map, apply its deltas, and mark
   the entry dirty for the next checkpoint.
8. Set `fullsync` for every peer (§2.6). Clear `cur` for every object
   — which costs nothing, because `cur` is never on disk (§14).
9. Set the log tail after the last valid record, take `qidnext` from
   the superblock, and start serving.

**Cost bound.** Steps 4–6 read the index region, one bitmap copy and
at most the log: at the defaults and `nslots = 2^20` that is 256 MiB
+ 8 MiB + ≤ 64 MiB of sequential reads, in `Wunit` requests issued
from a small pool. It is bounded by format constants and by nothing
else — in particular not by the number of objects that were being
written when the crash happened, and not by the object sizes. Step 7
adds one extent-map read per object touched since the last
checkpoint. The wall-clock figure is an open question (§16a): the
platform doc measured 4 KiB reads at 280 µs and never measured bulk
sequential read throughput.

**What an operator sees when it fails.** Every refusal above prints
one line naming the structure, the offset, and the tool that
addresses it, and exits non-zero — the store never starts in a
degraded mode it did not name. A corrupt index entry is a running
store with an object in `/lost`. A corrupt bitmap is a store that
will not start until `shoalck -R` runs. Two invalid superblocks is a
store that will not start at all, and the honest answer there is
`shoalfmt -r` plus refill from peers: the disk's identity is gone,
which by layer-a §1.5 makes it a reformat-before-rejoin case anyway.

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
one grain, and every slot statically reserves 256 bytes of index and
`emapsz` of extent map whether or not it holds anything. At the
recommended sizing that is 0.15% of the partition for metadata plus
up to `blksz-1` per object's final block. On a disk full of Layer B
stripe objects at `objmax` the waste is nil; on a disk full of tiny
MDS objects it is the dominant term, which is what the `-n` override
and §16a's sizing question are for.

**Slots.** A free-slot list in memory, rebuilt at start from the
index region. A slot is reused only after the commit that freed it
is durable (§3.5).

**Reclaim of tombstones.** A tombstone's content is released at
delete time — the `Eobj` that sets `state=tomb` carries `len=0`,
`nmap` empty and `nfree` naming every grain the object held. What
survives is the index entry and its statically reserved extent-map
slot. Layer-a §1.5's discard, once its three cluster-wide conditions
hold, commits an `Eslot` and the slot returns to the free list.
`tombdays` is evaluated against the entry's `mtime`, which is why
the tombstone keeps one.

**Disk full.** Three distinct exhaustions, mapped deliberately:

- *No free grain*, or a write that would need more grains than
  remain: `disk full` (layer-a §2.6). The store reserves nothing for
  writes; a delete never needs a grain, so delete always works.
- *No free slot*: `disk full` on create. A tombstone occupies a slot,
  so a cluster that cannot discard tombstones (layer-a §1.5's
  absent-instance case) can exhaust slots before it exhausts grains.
  `/status` reports both counts so the cause is visible.
- *No free log space*: the commit **waits** for the checkpointer,
  bounded by `replms`, and then answers `disk full`. Waiting is
  right because log exhaustion is transient by construction — the
  checkpointer is already running and the space is already
  reclaimable — and failing a client write on a checkpoint hiccup
  would be a self-inflicted outage. The bound is there because
  layer-a §5.4's client-latency budget must stay finite. §14 records
  that layer-a has no retryable "busy" error for this, so the
  definitive `disk full` is the closest honest answer.

## 7. Concurrency and group commit

*Policy, except where it realises layer-a §5.4.1, which is
normative there.*

The server is `lib9p`'s `Srv` in multi-proc mode (D12), so requests
arrive on independent procs.

**Per-object lock.** One reader-writer lock per live object, in the
in-memory index entry. The write path holds it exclusively from
layer-a §5.4 step 2 to step 6 or 7 — across the replication round
trip, which is what makes writes to one object totally ordered and
what layer-a §5.4.1 requires. The read path holds it shared across
its device reads. Operations on distinct objects never touch the same
lock and never share anything but the log writer and the allocator,
both of which are held for microseconds.

Two consequences worth stating: a read of an object with a write in
flight waits up to the write's bound (≤ 3·`replms` plus local I/O),
which is primary ordering doing its job and not a defect; and the
lock is never held across a client's think time, because it is taken
and released inside one request.

**`Tflush`.** `Srv.flush` marks the request abandoned. The worker
notices at its next check point and performs the whole of step 7 —
discard, unlock, clear per-peer sync state, invalidate `cur` — and
answers `Rflush` with no `Rwrite` and no `Rerror`. A worker already
inside the log writer's batch cannot be pulled out of it; it
completes the commit and then answers `Rflush` without answering the
write, which is the "MAY have been applied" case layer-a §5.4.1
already contemplates.

**Group commit.** One **log-writer proc** owns the log tail. A worker
reaching step 6 appends its entries to a pending queue and sleeps on
a `Rendez`. The writer loops:

1. Sleep until the queue is non-empty.
2. Take as much of the queue as fits in one `Wunit` as a batch — a
   batch is never larger than one device request — and assign it the
   next `seq` and the next log offset.
3. Device flush (covers every batch member's staged grain writes).
4. One `pwrite` of the batch's record.
5. When the batch's write returns *and every lower-numbered batch's
   write has returned*, device flush, advance the durable watermark,
   and wake every waiter at or below it.

There is no timer and no artificial delay, so **a lone writer pays
exactly one flush, one write and one flush** — the queue is empty
when it arrives, the writer takes it immediately. Batching happens
only among commits that coincide with a write already in flight,
which is precisely the coincidence worth exploiting.

Up to `logdepth` batches (default 4, maximum 8) may be in flight, to
reach the measured concurrency scaling — 114 durable writes/s at one
writer, 409/s at eight. The watermark in step 5 is what makes that
safe: a batch is acked only when it and every lower-numbered batch
are durable. Without it a crash after batch *n+1* landed and batch
*n* did not would leave replay stopping at *n* and discarding *n+1* —
an acked write lost. With it, *n+1* was never acked.

Throughput follows: one batch holds 32 small commits in one `Wunit`
write, so the commit path's ceiling is thousands of commits per
second and the binding constraint is the data writes, not the log.

## 8. Verify, scrub and repair

*Policy; layer-a §7.5's outcomes are normative there.*

**Verify one object.** Read each block, hash it, compare to `dig[i]`;
then hash the digest array and compare to `csum`. Returns the *set of
mismatching block indices*, not a boolean, because that set is what
makes partial repair possible. A hole is verified against the zero
digest without reading anything. Cost at `objmax`: 16 MiB of reads
plus 16 MiB of hashing, no writes. This backs the `verify` ctl verb
and `op=verify` on `/rpc`.

**Scrub.** A background proc walks slots in order, verifies each, and
rate-limits itself to the configured KiB/s so a full pass takes about
`scrubdays`. On mismatch it sets the index entry's `corrupt` flag —
durably, via an `Eobj` that changes nothing else, so a restart does
not forget — lists the object in `/lost`, and fails client access
with `checksum mismatch`. A corrupt copy loses arbitration against
everything including absence (layer-a §1.3), which the server
enforces by refusing to advertise it.

**Repair one block from a peer.** `op=get` the range
`[i*blksz, min((i+1)*blksz, len))` from a holder of a copy with the
same key, hash it, and accept it only if it matches the *stored*
`dig[i]`. Then allocate a fresh grain, write it, and commit an `Eobj`
whose four-tuple is **unchanged** — same `ver`, same `wepoch`, same
`csum` — and whose `nmap` names the one block.

A commit that does not advance the key is a first-class case in this
store, and there are two of them: this repair, and layer-a §1.3's
`op=full force=1` divergence repair, which applies a whole object at
a key equal to the receiver's. Both go through the same `Eobj` path;
nothing in the format assumes a commit bumps a version.

When every block verifies, the `corrupt` flag is cleared by another
key-preserving `Eobj` and the object leaves `/lost`. When no peer has
a good copy, it stays there and reads fail `object lost`.

## 9. In-memory index and directory snapshots

*Policy.*

The in-memory index is an array of `nslots` entries plus an oid arena
plus a hash table. Per entry:

    qidpath 8, len 8, ver 8, wepoch 8, mtime 8, csum 32,
    cur 4, oidoff 4, oidlen 1, state 1, flags 1, hashnext 4,
    lock and padding

which is 96 bytes rounded. The oid arena averages ~24 bytes an
object; the hash table is 2^19 `u32` buckets with chaining through
`hashnext`.

| objects | index | arena | buckets | total |
|---|---|---|---|---|
| 2.6·10^5 | 25 MB | 6 MB | 2 MB | **33 MB** |
| 2^20 | 96 MB* | 24 MB | 8 MB | **128 MB** |

(*the array is `nslots` entries whether or not they are occupied.)

The extent maps are deliberately **not** in memory — 5.6 KiB × 2^20
is 5.5 GiB — so the write path reads one 11-sector extent map per
object touched, backed by an LRU of a few thousand entries (4096
entries is 23 MB) which makes repeated writes to one object free.
One device read against an 8.4 ms commit is 4% overhead; §16a asks
for it to be measured.

**Snapshot-at-open (R12).** Opening `/obj`, `/tombs` or `/advert`
takes, under the index lock, a vector of `{u32 slot, u64 qidpath}` —
12 bytes an entry, **3.1 MB at 2.6·10^5 objects and 12 MB at 2^20**.
Each `Tread` renders entries from the *live* index, skipping any
whose `qidpath` no longer matches the snapshot: that is an object
deleted since the open, which a listing of live objects should not
show anyway. Nothing shifts under the reader, so no entry is skipped
or duplicated because of an index shift, which is what layer-a §2.2
asks for.

At 3.1 MB per open fid the snapshot is affordable and the store
reports `objsnap=full`. Layer-a §2.2's `objsnap=partial` escape
exists because a full snapshot was thought to cost real memory; at
these numbers it does not, and this store never uses it. That settles
one of layer-a §10.2's evidence items on the memory side (§14).

## 10. The monitor's map slot store

*Format.*

The monitor's requirement (layer-a §8.2) is one sentence: the map text
including its `stale` records must be durable against power loss
before the acknowledgement that publishes it. It is small, it changes
rarely, and one of those acknowledgements sits inside a client write
bounded by `replms` (layer-a §5.4 step 5a). It gets the same treatment
in its simplest possible form.

A partition of a few MiB, formatted by `shoalmonfmt`:

    sector 0        header
    hdr.curoff      current-map slot 0
                    current-map slot 1
    hdr.histoff     retain history slots, a ring

Header, one sector:

    off  size  field
      0     8  magic  "shoalmon"
      8     4  vers
     12     4  pad
     16    16  csum128, this field zeroed
     32     4  slotsz    bytes per slot, default 65536
     36     4  retain    history slots
     40     8  curoff    sector of slot 0
     48     8  histoff   sector of history slot 0
     56    16  monid     the cluster identity, layer-a §3.2
     72   ...  reserved, zero

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
text, not the whole slot, so a commit writes and reads only what the
map occupies — one device request for any map under 16 KiB. Bytes
left beyond `len` by a previously longer map fall outside the
checksum and are ignored.

**Commit.** Write the slot whose `seq` is lower, with
`seq = max+1`; flush. **Choose on start:** read both, take the valid
one with the greater `seq`. A torn write to the inactive slot fails
its checksum and the active slot is untouched, so the previous map
survives; that is the entire crash argument, and it is the two-slot
rule of §2.2 again.

The history ring is written the same way, one slot per published
epoch, and serves `/maps/<epoch>`. Losing it is not a correctness
event: layer-a §5.2 clause 2 already has a fallback for an
unavailable `/maps/<E−1>`.

**Sizing.** A map at twelve instances is a few KiB; `slotsz` 65536 is
a twentyfold margin and a whole number of 16 KiB units. With
`retain=8` the store needs 1 header sector + 10 slots ≈ 640 KiB;
`shoalmonfmt` defaults the partition to 4 MiB and refuses less than
1 MiB.

**Cost.** One 16 KiB write plus one flush per commit: **8.6 ms**,
whether it publishes a placement change or a single `stale` mark.
That is what makes layer-a §5.4 step 5a affordable — the alternative
`docs/platform/9front-storage.md` measured, a file plus a gefs sync,
costs 530–620 ms and would blow `replms` regularly.

## 11. Cost model

*Policy.* All figures from `docs/platform/9front-storage.md`: 8.4 ms
per device write of ≤ 16 KiB, four times that for 64 KiB in one call,
175 µs per flush, ~280 µs per 4 KiB read, 114 durable writes/s at one
writer and 409/s at eight.

Per commit, unconditionally: **one flush + one write + one flush =
8.75 ms**, shared across everything in the batch.

| Operation | Reads | Data writes | Commit | Total |
|---|---|---|---|---|
| create, delete, truncate to a block boundary | 1 extent map (0.4 ms, cached: 0) | none | 1 | **~9 ms** |
| 64 KiB write, block-aligned | 1 extent map | 1 grain: 4×16 KiB | 1 | **42 ms serial, ~26 ms with the grain issued 4-way** |
| 4 KiB write into a populated block | 1 extent map + 1 grain (~2 ms) | 1 grain: 4×16 KiB | 1 | **45 ms serial, ~28 ms 4-way** |
| 4 KiB write into a hole | 1 extent map | 1 grain | 1 | as above less the 2 ms |
| 16 MiB `op=full` | 1 extent map | 256 grains = 1024×16 KiB | 1 | **8.6 s serial, ~2.5 s at 8-way (409/s)** |
| scrub-verify one 16 MiB object | 16 MiB | none | none | read-bound |

Three things this table says that are worth saying in words.

**A 4 KiB client write costs a 64 KiB grain write.** Copy-on-write at
`blksz` granularity is what buys the atomicity of §3, and it makes a
small write cost sixteen times its size. The workload this store is
built for — Layer B striping through an `msize`-sized 9P path —
writes whole blocks, and layer-a §10.2 already lists the per-4 KiB
re-hash cost as evidence work. §15 records the two ways to fix it if
the measurement says it matters, and §16a asks for the measurement.

**A 16 MiB `op=full` is disk-bound, not wire-bound.** Layer-a §5.5
puts it at 185–545 ms of wire time; the receiving disk costs ~2.5 s at
the measured concurrency. The conclusion layer-a draws from that
arithmetic — never pull inside a client request (layer-a §5.2) — is
reinforced, not weakened, but the number in layer-a §5.5 is five to
fifteen times optimistic on this platform and layer-a §10.2's sizing
item should be re-derived with the disk term in it (§14).

**Group commit removes the commit from the throughput equation.** At
32 small commits per `Wunit` record and four batches in flight the
commit path sustains thousands of commits per second. What it cannot
remove is the data writes: sustained durable throughput on the
reference device extrapolates to 409 × 16 KiB ≈ **6.5 MB/s** — the
409/s was measured at 4 KiB, and a 16 KiB write costs the same 8.4 ms,
so the extrapolation is the one §16a(2) asks to confirm. Everything
above follows from it. On hardware where a durable write costs 100 µs
rather than 8.4 ms every row of the table scales by the same factor —
the shape of the design does not change, only the constant.

## 12. Tooling

*Policy.* Three new commands under `cmd/`, each an `mkone`
directory listed in `cmd/mkfile`'s `DIRS`.

**`shoalfmt`** — format or ream an object-store partition.

    shoalfmt [-r] [-b blksz] [-o objmax] [-n nslots]
             [-d ndirty] [-L logbytes] [-u uuid] /dev/sdXX/name

Writes both superblocks, zeroes the log, index, dirty and bitmap
regions, and prints the geometry it chose. It generates a random
`uuid` unless given one, and **refuses a partition that already
carries a valid superblock unless `-r`** — reaming a disk destroys an
instance's identity, and layer-a §1.5 makes that a
reformat-before-rejoin event, so it should take a flag. It warns when
`blksz` is below 65536 (§2.7) and when `nslots` implies more than 1%
of the partition in metadata.

**`shoalck`** — inspect and check.

    shoalck [-v] [-l] [-R] [-o oid] /dev/sdXX/name

Default: print both superblocks, the geometry, log head/tail and
sequence range, slot and grain occupancy, and the dirty-record count;
verify every index entry's checksum and the bitmap's; cross-check the
bitmap against the grains every live extent map references; exit
non-zero on any inconsistency. `-l` dumps the log records. `-o` dumps
one object's index entry and extent map. `-v` verifies every object's
content against its digests — an offline scrub. `-R` rebuilds the
free-grain bitmap from the extent maps and rewrites the checkpoint,
which is §5 step 5's repair path.

**`shoalmonfmt`** — format a monitor map partition.

    shoalmonfmt [-r] [-s slotsz] [-R retain] [-m monid] /dev/sdXX/name

**Carving the partitions** is the operator's step and uses stock
tools. On a whole disk: `disk/fdisk -p /dev/sdXX/data` to create or
confirm a `plan9` partition, then `disk/prep -w -p /dev/sdXX/plan9`
to add a named partition — `shoal` on a data disk, `shoalmon` where
the monitor runs. `-w` writes the partition table to the disk so the
names reappear after a reboot; without it the partition must be
re-declared into `/dev/sdXX/ctl` at every boot, which is a good way
to start a store on the wrong bytes. `cat /dev/sdctl` names the
driver, which §3.2's flush caveat depends on.

The servers themselves — the object server and the monitor — are
layer-a's subject, not this document's; where they need names,
`cmd/shoalsrv` and `cmd/shoalmon`.

## 13. Test plan for T2

*This section is a plan.* T2 is the single-node integration tier that
`AGENTS.md` leaves undefined; this is what the store contributes to
it. T2 runs on one 9front machine with one scratch partition. Each
test is a program plus an `rc` script that formats a partition, runs
a server against it, drives it, and checks. None of it is T1: T1 stays
diskless.

Every test below names the requirement from §1 it discriminates, and
**each one must be shown failing** against a deliberately broken
build before it is believed — the both-ways rule. The mutations to
break are named per test.

**The fault-injection hook.** The server takes `-X <point>[,<n>]`,
present in every build and inert without the flag. It is a command
line flag rather than a `/ctl` verb because layer-a §2.5 fixes the
ctl grammar and requires an unknown verb to fail `unknown ctl`; a
debug verb would be a wire change. At the named point the server
calls `exits("crash")` with no unwinding, no flush and no further
write. Points:

- `stage` — after the last staged grain write, before enqueue (P1).
- `precommit` — after the pre-flush, before the log write (P2).
- `commit:n` — write only the first *n* sectors of the batch's
  record, then exit. This is the **torn-write simulator** (P3), and
  `n` sweeps 0 … `nsec`.
- `postwrite` — after the log write returns, before the post-flush
  (P4).
- `preack` — after the post-flush, before `Rwrite` (P5).
- `ckpt:n` — after *n* checkpoint page writes, before the superblock
  (checkpoint row of §3.4).
- `super` — between the two superblock copies.
- `batch:n` — hold batch *n*'s write and let batch *n+1* complete,
  then exit: the group-commit watermark case (§7).

**T2.1 crash matrix (R1, R2, R3, R4).** For each point above, and for
each of create / whole-block write / partial write / truncate /
delete / 16 MiB `op=full`: record the object's four-tuple and content
before, run the operation under the hook, restart, and assert that
`/meta` reports exactly the old or exactly the new four-tuple, that
`verify` passes, and that a full re-read matches the reported `csum`.
For `preack` and later, assert *new*. For `stage`, `precommit` and
every `commit:n` with `n < nsec`, assert *old*. Mutations that must
break it: skip the post-flush; accept a log record without checking
its checksum; make `Eobj` carry the four-tuple in one record and the
block map in another.

**T2.2 durable-before-ack (R1).** A writer records each acked
`(oid, ver)` to a file on another machine's disk, or to a serial
line, so the ack log survives; the machine is hard-powered-off mid
run; on restart every acked `(oid, ver)` is present and verifies.
This is the test `docs/platform/9front-storage.md` ran against a
bare partition, lifted to the store. Mutation: ack before the flush
returns.

**T2.3 torn-record rejection (R2).** Offline: `shoalck -l` on a
partition whose last log record has had a random sector overwritten
must report the record invalid, and a restart must stop replay before
it. Sweep the damaged sector over the whole record including the
header. Mutation: trust `nsec` without bounds-checking it — the test
must show the unmutated build refusing a header whose `nsec` points
outside the region.

**T2.4 deferred reuse (§3.5).** Object A commits a truncate that
frees grain *g*; object B is staged into *g* immediately; crash at
`commit:0` for both. On restart A must still read its old content.
Mutation: return freed grains to the allocator at commit time rather
than at flush completion — the test must fail.

**T2.5 group-commit watermark (§7).** Drive eight concurrent writers,
crash at `batch:n`, and assert no acked write is missing after
restart. Mutation: wake waiters when their own batch returns rather
than at the watermark.

**T2.6 discard costs nothing (R4).** Configure a peer that never
replies, drive a write to step 7, and assert: the object's four-tuple
and content are byte-identical to before, `verify` passes, the free
grain count is back to its pre-write value, and the log's sequence
number did not advance.

**T2.7 restart invalidates currency (R5).** Restart cleanly; every
object's `/meta` must show `cur=0`.

**T2.8 digests and holes (R6, R13).** Write at a large offset,
read back the hole as zeros, and check that the object's `csum`
equals `shoalcsum` over the same byte image. Truncate within a block
and re-check. Partial-write one block of a many-block object and
assert only that block's digest changed.

**T2.9 recovery cost (R11).** Fill a partition to 2.6·10^5 objects,
crash with a full log, and measure restart time and peak memory
against §5's bound and §9's table.

**T2.10 enumeration (R12).** Open `/obj` on 2.6·10^5 objects, then
create and delete concurrently while reading the listing to the end;
no entry may appear twice and no entry that existed for the whole
read may be missing.

**T2.11 exhaustion (§6).** Fill grains, then slots, then the log, and
assert `disk full` in the first two cases and a bounded wait followed
by `disk full` in the third — with the store still serving reads and
deletes throughout.

**T2.12 monitor slots (§10).** Commit maps under `-X` at each point,
restart, and assert the monitor comes up at the newer map or the
older one, never a torn one, and that a slot with a damaged sector is
rejected in favour of the other.

## 14. Deviations from layer-a, and tensions with the platform

*Policy, but read it before implementing anything.*

Nine places where layer-a is silent, self-defeating, or contradicted
by the measurements.

1. **`cur` cannot usefully be durable (layer-a §5.2).** Layer-a §5.2
   requires the instance to record currency "durably as `cur=<epoch>`"
   and, two lines later, to invalidate `cur` for every object **on
   process start, after a clean exit as much as after a crash**. The
   only moment a durable value could be read is the moment it must be
   discarded. This store keeps `cur` in memory only. That satisfies
   every observable requirement layer-a §5.2 states and removes a
   durable write from the currency path; it deviates from the letter
   of one sentence, which should be struck.

2. **Step 5b's dirty record is committed *with* the update, not before
   it (layer-a §5.4).** Step 5 says the primary must durably record
   `(oid, peer, epoch)` "before proceeding" to step 6. This store puts
   the `Edirty` entries in the same log record as the `Eobj`. That is
   strictly stronger than the stated order: either both are durable or
   neither is, and if neither is, no write happened and no dirty
   record is owed. Reading the sentence as a strict ordering costs a
   whole extra 8.75 ms commit on the degraded path for no guarantee.
   Layer-a §5.4 should say "durable no later than the update".

3. **Layer-a assigns no home to state the store must hold (layer-a
   §2.3, §6.3, §8.6).** Three per-instance facts must survive a
   restart and the document never says where they live: the pinned
   `monid`, whose whole purpose as a tripwire evaporates if a restart
   forgets it; the highest adopted epoch, which layer-a §8.6 step (b)
   has the rebuilding monitor read out of `/status` and which cannot
   be reported if it resets to zero; and layer-a §2.3's monotonic
   `qid.path` counter. All three are in the superblock (§2.2). Layer-a
   §2.3 additionally states that the counter is monotonic *and* that
   discarding a tombstone frees the path for reallocation — those only
   compose if the counter is a stored high-water rather than a maximum
   over live records, which is what §2.2 does.

4. **Layer-a §5.5's resync arithmetic omits the disk (§11).** 185–545
   ms is wire time. The receiving disk costs ~2.5 s for a 16 MiB
   `op=full` at eight-way concurrency, 8.6 s serial. Nothing in
   layer-a's reasoning breaks — the conclusion "do not pull inside a
   client request" gets stronger — but layer-a §5.5's numbers and
   layer-a §10.2's `objmax` sizing item are five to fifteen times
   optimistic on the measured platform.

5. **Layer-a §5.4's "plus local I/O" is the dominant term, not a
   rounding error.** The honest sum for one client write is 3·`replms`
   plus 26–45 ms of local I/O, plus whatever the object's lock queue
   holds. At the default `replms` of 1 s that is invisible; at a
   `replms` tuned down toward the sub-millisecond LAN it is the whole
   cost. Whoever tunes `replms` (layer-a §10.2) needs this number in
   front of them.

6. **`disk full` is a definitive error and log exhaustion is
   transient.** Layer-a's retryable set has no "busy". §6 waits
   `replms` and then answers `disk full`, which is the closest honest
   answer available, but a client that treats it as definitive will
   give up on a condition that clears in milliseconds. A retryable
   `busy` would be the right addition; it is a wire change and this
   document does not make one.

7. **The flush channel is exclusive per `sd` unit.** `/dev/sdXX/raw`
   admits one opener per unit, so at most one shoal process per unit
   can issue a device flush. D4 says "one instance per disk" and
   layer-a never mentions the unit. §2.1 makes it a deployment rule
   and §5 degrades gracefully with `flush=unavailable` rather than
   refusing to start, because on a write-through device the flush
   changes nothing — but an operator who co-locates the monitor and
   an instance on one unit should be told, and is.

8. **Formatting precedes the map that defines the format (layer-a
   §3.4).** Step 1 there has the operator start a server on an
   unformatted disk, which generates a uuid and writes a superblock —
   before it registers, so before it has a map, so before it knows
   `blksz` and `objmax`, which this store's geometry depends on.
   Resolution: `shoalfmt` takes them from the operator, defaulting to
   the map defaults, and the server MUST refuse to serve if the
   adopted map's `blksz` or `objmax` differ from the superblock's.
   They are immutable cluster-wide (layer-a §8.5), so the operator
   does know them at format time; the document should say the format
   step needs them.

9. **Two of layer-a §10.2's evidence items are answered here, one is
   not.** Answered: the local layout question ("the single largest
   unknown") is this document, and the monitor's durability question
   ("the second-largest") is §10 — 8.6 ms, comfortably inside
   `replms`. Answered on the memory side: a full `/obj` snapshot at
   2.6·10^5 entries costs 3.1 MB (§9), so `objsnap=partial` is not
   needed at this envelope. Not answered: whether `blksz=65536` is at
   the right point on the metadata-size vs re-hash-cost curve — §11
   shows it is also the *write-amplification* curve, which layer-a
   §10.2 does not mention, and §16a asks for both.

One smaller correction. Layer-a §1.5 says a tombstone "occupies a
metadata record and nothing else". In this layout it occupies an index
slot *and* the extent-map slot statically reserved beside it — 5.9
KiB, not 256 bytes. Still nothing next to a 16 MiB object, but discard
reclaims more than layer-a §1.5 implies, and slot exhaustion (§6) is a
real way for a cluster that cannot discard to run out of room.

## 15. Alternatives considered

*Policy.*

- **Log-structured whole store** — every write appends to a segmented
  log, an index maps object blocks to log positions, a cleaner
  reclaims segments. Attractive here: every write is sequential, and
  a 4 KiB write costs 4 KiB rather than a 64 KiB grain. Rejected:
  the cleaner is the hard part, its write amplification goes
  non-linear near full, and it must be crash-safe in its own right —
  a second recovery story on top of the one we need anyway. The
  measured device also shows random and sequential writes at
  identical cost, so the sequential-append premise buys nothing on
  this platform. In-place plus a write-ahead log is a construction a
  9front reviewer has seen; a cleaner is one they will have to audit.

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
  file. A store on them would need a repair story for the container
  as well as for its own records.

- **Packed or extent allocation** — variable-length extents, best
  fit, coalescing on free. Less internal fragmentation for small
  objects, contiguity for large reads. Rejected: it brings external
  fragmentation and a real allocator, object growth needs relocation,
  and the block map stops being parallel to the digest array, so two
  structures replace one. Fixed grains at `blksz` make allocation a
  bit test and make one `u32` locate both the bytes and the digest.
  The envelope pays for it: ≤ 256 blocks per object and ≤ 2^20
  objects mean the arrays are small and the linear scans are cheap.

- **Preallocating a full `objmax` extent per object** — no block map
  at all, one base grain per object. Rejected: 2.6·10^5 objects ×
  16 MiB is the whole 4 TB disk, with no sparseness and no holes.

- **A finer allocation grain than `blksz`** — `grainsz` = 16 KiB with
  `blksz` = 4·`grainsz`, so a partial write rewrites 16 KiB rather
  than 64 KiB. Would cut the 4 KiB-write cost from ~28 ms to ~19 ms.
  Not taken: it quadruples the block map, splits one array into two
  of different lengths, and adds a parameter — for a 1.5× gain on a
  case §11 argues is not the workload. Recorded as the first thing to
  try if §16a's measurement says otherwise; it is a format change,
  so it costs a `vers` bump and a reformat.

- **Carrying small write payloads in the log record** — a 4 KiB write
  becomes one commit with the bytes inline, folded into its grain at
  checkpoint. Cuts the same case to ~11 ms. Not taken: the read path
  must then overlay unfolded log data on the grain, and `verify`
  must consult the log to see the content its committed digest
  describes. The digest-matches-content property of §4 stops being
  obvious, which is too high a price for one case.

- **A separate log device** — rejected for v1. The measured device
  shows random and sequential writes at identical cost, so there is
  no seek argument, and a second partition is a second operator step
  and a second thing to lose.

- **CRC-32 for record checksums** — rejected. D7 already puts
  BLAKE2s in the build and `lib/csum.c` already implements it;
  hashing 16 KiB costs microseconds against an 8.4 ms write; and one
  primitive means one set of known-answer vectors.

- **Per-page rather than per-entry index checksums** — cheaper by 6%
  of the index region and coarser by 64×. Rejected: a localised fault
  should cost one object, not a page of them.

## 16. Open questions

### (a) Settleable by evidence

1. **Bulk sequential read throughput on the platform.** Only 4 KiB
   reads were measured (280 µs). §5's recovery bound and §9's index
   build are both stated in bytes because nobody has the rate. Needed
   before anyone claims a restart time.
2. **Does splitting one grain write across concurrent procs actually
   help?** §11 assumes a 64 KiB grain written as four concurrent
   16 KiB `pwrite`s costs ~18 ms rather than 33.6 ms, read off the
   four-writer row of the concurrency table — 800 writes over four
   procs in 3.59 s is ~18 ms per round of four. That row measured
   independent writers, not pieces of one logical write. Measure it;
   §11's "4-way" column depends on it, and note the gain is 1.9×,
   not the 4× a naive reading of 114→409/s suggests.
3. **The cost of a partial-block write**, and therefore whether the
   finer grain in §15 is wanted. Sweep write size against `blksz` and
   compare to the model.
4. **Recovery time and memory at 2^20 slots** (T2.9), against §5 and
   §9.
5. **Whether the extent-map read per write shows up**, and what LRU
   size makes it disappear for the striping workload.
6. **Whether the SCSI flush is needed on the fleet's units.** `cat
   /dev/sdctl` per machine; a legacy IDE unit needs its write cache
   disabled because the flush is silently faked there.
7. **Log size against checkpoint frequency** — 64 MiB and 30 s are
   guesses, both tunable without a format change.
8. **Whether single-sector writes are atomic on the fleet.** §3.2
   argues the design does not need it. Worth confirming that the
   argument is not load-bearing anywhere it was not noticed.

### (b) Product calls

1. **Metadata budget — the `nslots` default.** *Recommend*
   `min(2^20, 4 × partsize/objmax)`: 0.15% of a 4 TB disk, and four
   times as many slots as `objmax`-sized objects would need, because
   Layer C's MDS objects will be small and numerous. The alternative
   is a tighter default plus an operator who has to think; the cost
   of thinking wrongly is a disk that reports `disk full` with
   terabytes free.
2. **Behaviour when the log is full.** *Recommend* bounded wait then
   `disk full`, as §6 specifies. The alternatives are failing
   immediately (turns a checkpoint hiccup into a client error) and
   waiting indefinitely (breaks layer-a §5.4's latency bound). If a
   retryable `busy` is ever added to layer-a §2.6, switch to it —
   see §14(6).
3. **`cur` in memory rather than on disk.** *Recommend* in memory,
   and amend layer-a §5.2. A durable value that must be discarded
   before it can be read is a durable write nobody benefits from.
4. **Folding the dirty record into the update's commit.**
   *Recommend* yes, and amend layer-a §5.4 step 5b to "no later
   than". It is strictly stronger and saves an 8.75 ms commit on
   every degraded write.
5. **One shoal process per `sd` unit.** *Recommend* making it a
   documented deployment rule rather than an enforced one: the store
   already reports `flush=unavailable` and starts, because on a
   write-through unit the flush is insurance rather than the
   mechanism. Enforcing it would stop a perfectly workable test
   configuration for a reason that does not always apply.
6. **What a store does when its own disk fails verification at
   start.** *Recommend* what §5 specifies — refuse to start on a bad
   bitmap or two bad superblocks, serve with the object in `/lost` on
   a bad index entry. The alternative, starting anyway and letting
   the cluster arbitrate, is tempting because layer-a can in fact
   heal it; it is rejected because a disk that cannot read its own
   metadata is a disk whose next write should not be trusted either,
   and because a store that starts in a mode it did not name is how
   an operator loses a day.
