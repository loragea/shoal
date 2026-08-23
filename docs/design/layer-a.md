# Layer A — distributed object store

Status: design proposal, not ratified, nothing implemented.

Scope: the object model, the per-disk storage server's 9P export,
the cluster map, the placement function, the write/read path, epoch
and fencing, heal/scrub/rebalance, and the monitor. `docs/target.md`
is the contract this designs toward; `docs/decisions.md` D1–D6 are
binding. Section 10 reports every place this proposal constrains or
strains them.

## 0. Conventions

*Normative.*

- MUST/SHOULD/MAY are RFC 2119 senses. Each section is marked
  **normative** (a reimplementation must match, bit for bit at wire
  and format boundaries) or **implementation policy** (a conforming
  implementation may differ).
- All textual formats in this design are 7-bit ASCII, LF-terminated
  lines, no CR. Tokens are separated by one or more spaces or tabs.
  Values MUST NOT contain white space or NUL. A `#` at the start of
  a line begins a comment; comments and blank lines are ignored by
  parsers and MUST be preserved by writers where a human-edited file
  is round-tripped (the cluster map).
- Error strings are 9P `Rerror` bodies. Each error below is defined
  by its **prefix**: a server MUST emit exactly the given prefix and
  MAY append `": "` plus free-form detail. Clients MUST match on the
  prefix and MUST NOT parse the detail. Error prefixes are lower
  case, no trailing punctuation, in the Plan 9 style.
- Integers on the wire are unsigned decimal unless a field is
  defined as hexadecimal (lower case, no `0x`).

## 1. Object model

*Normative except where marked.*

### 1.1 Identifiers

An **object id** (`oid`) is an opaque byte string. Layer A MUST NOT
parse it, MUST NOT infer relationships between ids, and MUST NOT
give any id special placement treatment. Layer B's `fileid.N`
convention (D3) is invisible here; that a file's stripes scatter
across all disks is a consequence of hashing the whole id, and is
the property that makes a file larger than a disk work.

An oid MUST match:

    oid  = 1*128( ALPHA / DIGIT / "." / "-" / "_" )

and MUST NOT be `.` or `..`. The bound of 128 keeps an id a legal
9P path element under every 9front file server, and keeps a
directory entry small. Ids are compared as byte strings; comparison
is case-sensitive.

Ids beginning with `shoal.` are **reserved** for system use (the
monitor's map snapshots, future internal bookkeeping). A storage
server MUST reject a `role=client` create of a reserved id with
`reserved name`. Layer B's grammar (fixed in the Layer B round)
MUST therefore constrain `fileid` so that no file id can begin with
`shoal.`; requiring `fileid` to be a lower-case hex string suffices.

### 1.2 Size

One immutable cluster-wide limit `objmax`, in the map header, fixed
at creation (§8.4). It MUST be a power of two and ≥ 2^20.
**Default: 16 MiB (16777216).** It is Layer B's stripe-unit ceiling
so it must be uniform and stable; 16 MiB keeps a whole-object
resync near 150 ms of a 1 Gb/s link and a 4 TB disk near 2.6·10^5
objects, small enough that full enumeration and local scans stay
cheap.

A write whose `offset + count` would exceed `objmax` MUST fail with
`object too large`. Servers MUST NOT silently truncate.

### 1.3 Per-object metadata

Each object carries, durably:

| Field | Type | Meaning |
|---|---|---|
| `oid` | string | the id |
| `len` | u64 | current length in bytes, ≤ `objmax` |
| `ver` | u64 | version counter, starts at 1 on create |
| `wepoch` | u64 | map epoch of the primary that produced `ver` |
| `csum` | hex | object checksum (§1.4) |
| `state` | `live` \| `tomb` | tombstone flag (§1.5) |
| `mtime` | u64 | seconds since the epoch, last content change |

**Arbitration key.** Copies of an object are ordered by the pair
`(wepoch, ver)`, compared lexicographically, greater wins. `ver`
alone is not sufficient: a copy that survived from a fenced era
must lose to a copy produced under a later map even if a counter
raced. A copy that fails local checksum verification (§7.4) MUST be
treated as `(0, 0)` — unconditionally the loser — regardless of its
recorded key.

`ver` and `wepoch` are assigned by the object's primary (§5) and
copied verbatim to replicas; a replica MUST NOT invent either.

### 1.4 Checksums

Object content is divided into fixed-size **checksum blocks** of
`blksz` bytes (map header, immutable, power of two, default
65536). Block *i* covers bytes `[i*blksz, min((i+1)*blksz, len))`.
The final partial block is hashed over its actual length. A hole
(§2.4) hashes as the zero bytes it reads as.

- Block digest: BLAKE2s with a **16-byte** digest, unkeyed, over
  the block's bytes.
- Object checksum `csum`: BLAKE2s with a **32-byte** digest,
  unkeyed, over the concatenation of all block digests in ascending
  block order. For `len == 0` the concatenation is empty.
- `csum` is rendered as 64 lower-case hex characters.

Block digests are stored locally alongside the object (format:
implementation policy) so a small write re-hashes one block, not
the whole object; at the defaults that costs 16 bytes per 64 KiB,
~0.025% of capacity. The two-level construction keeps partial
writes cheap while leaving `csum` a single value two instances can
compare in one line of text.

### 1.5 Delete and tombstones

Delete MUST NOT simply remove the object: a replica absent during
the delete would resurrect it at the next heal. Delete sets
`state=tomb`, `len=0`, releases the content, and bumps
`(wepoch, ver)` like any other write; a tombstone arbitrates
normally.

A tombstone MAY be discarded by the object's primary once **both**
every instance in the placement set (§4) under the current map has
confirmed it holds that tombstone version **and** `tombdays` (map
header, default 7) have elapsed since `mtime`. The delay covers a
stray copy returning after the confirmations.

Reads of a tombstoned object MUST fail with `object deleted`. A
create of a tombstoned id succeeds and produces a fresh `live`
object with `ver` one greater than the tombstone's.

## 2. Storage-server 9P export

*Normative. The local on-disk representation behind it is
implementation policy (D1).*

One instance per disk (D4), serving stock 9P2000 — no extension.

### 2.1 Attach

The epoch and the caller's role travel in `aname`: the only place
they can travel without extending 9P (D2), and it usefully makes a
map change invalidate every outstanding fid at once (§6).

    aname = attr *("," attr)
    attr  = "epoch=" u64 / "role=" role / "peer=" iid
    role  = "client" / "repl" / "admin"

- `role` defaults to `client`. `epoch` is REQUIRED for
  `role=client` and `role=repl`; `role=admin` MAY omit it.
- `role=repl` MUST be used by another storage instance acting on
  behalf of a primary, and MUST carry `peer=` naming the attaching
  instance. It grants access to `/repl`, `/advert`, and read access
  to any locally held object regardless of primaryship.
- `role=admin` grants `/ctl` and status files only; it MUST NOT
  grant object I/O.
- An unparseable `aname` MUST fail the attach with `bad aname`.

Every operation on a fid is checked against the fid's attach epoch
(§6.2). Clients therefore re-attach on epoch change; Layer A is
consumed through a client library, not a raw `mount(1)` (§10).

### 2.2 File tree

    /ctl            write verbs (§2.5)
    /status         instance state, attr=value lines
    /map            the instance's cached cluster map, read-only
    /obj/<oid>      object contents
    /meta/<oid>     object metadata, attr=value, read-only
    /repl           replication command channel (§5.3)
    /advert         bulk version advertisement (§7.2)
    /dirty          objects known out of sync, one per line
    /lost           locally corrupt or unrecoverable objects
    /jobs           background jobs (resync, scrub)

`/obj` and `/meta` are directories. A directory read of `/obj` MUST
enumerate every `live` object, and MUST NOT enumerate tombstones;
order is unspecified and clients MUST NOT depend on it. At §1.2's
sizing a full enumeration is a few tens of megabytes, which is why
a separate "list objects" ctl verb is not needed.

### 2.3 Qids and stat

- `/obj/<oid>`: `qid.type = QTFILE`. `qid.path` MUST be stable for
  the lifetime of the object id on that instance and MUST differ
  between distinct ids on that instance (implementation policy how;
  the low 64 bits of BLAKE2s-256 of the id is adequate).
  `qid.vers` MUST be the low 32 bits of `ver`.
- `stat` reports `length = len`, `mtime`, `uid`/`gid`/`muid` of the
  server's choosing, `mode` `0666`.

### 2.4 Object I/O

- **create.** `Tcreate` in `/obj` with the oid as `name`, `perm`
  `0666`. Any of `DMDIR`, `DMAPPEND`, `DMEXCL`, `DMTMP` MUST be
  rejected with `bad create mode`; `ORCLOSE` MUST be rejected with
  `bad open mode`. Creating an existing `live` id MUST fail with
  `object exists`. A successful create yields `len=0`, `ver`
  assigned per §1.3.
- **open.** `OREAD`, `OWRITE`, `ORDWR`, with or without `OTRUNC`.
- **read.** Reads at `offset ≥ len` MUST return count 0. Reads
  crossing `len` MUST return only the bytes below `len`. Bytes
  never written below `len` (holes) MUST read as zero.
- **write.** A write at `offset > len` extends the object, the gap
  becoming a hole. A server MAY return a **short write** (an
  `Rwrite` count smaller than requested); clients MUST handle short
  writes by looping. Short writes are how the primary bounds a
  single replicated operation to what fits one peer message
  (§5.3). A single accepted `Twrite` is applied atomically with
  respect to other operations on the same object.
- **truncate/extend.** `Twstat` with `length` set. Any other
  settable stat field, including `name`, MUST be rejected:
  `Twstat` rename MUST fail with `no rename`.
- **remove.** `Tremove` on `/obj/<oid>` performs §1.5 delete.
- **metadata.** `/meta/<oid>` reads as, e.g.:

        oid=f3a91c.7 len=1048576 ver=42 wepoch=17
        csum=blake2s256:9f...c1 state=live mtime=1755990000
        blksz=65536 placement=n2.1,n5.0 primary=n2.1

  `placement` and `primary` are computed from the instance's
  current map and are advisory; the map is the authority.

### 2.5 ctl grammar

One command per `Twrite`; a partial line MUST be rejected with
`bad ctl`. The write's offset is ignored. Success returns an
`Rwrite` count equal to the bytes written; failure returns
`Rerror`. Commands that start background work return success once
the job is *accepted*; progress is read from `/jobs`.

| Verb | Form | Effect |
|---|---|---|
| `refresh` | `refresh` | Fetch the map from the monitor now. |
| `register` | `register` | Re-register with the monitor (§3.4). |
| `pull` | `pull <oid> <iid>` | Whole-object fetch from `<iid>`; adopt iff its key is greater (§1.3). |
| `push` | `push <oid> <iid>` | Send the local copy to `<iid>`. |
| `reconcile` | `reconcile [<oid>]` | Run §7.2 for one object, or for all local objects. |
| `advert` | `advert [<iid>]` | Re-send version advertisements. |
| `drop` | `drop <oid>` | Delete a local copy with no tombstone (§7.3). |
| `verify` | `verify <oid>` | Re-hash and compare now. |
| `scrub` | `scrub [start\|stop] [rate=<n>]` | Control the scrubber; `rate` in KiB/s. |
| `forget` | `forget <iid>` | Discard the dirty set for a peer, marking it `fullsync` (§7.1). |
| `fence` | `fence on\|off` | Force/release the fenced state (§6.4). Maintenance and testing. |

Unknown verbs MUST fail with `unknown ctl`; a known verb with bad
arguments MUST fail with `bad ctl`.

### 2.6 Error strings

*Normative.* Prefixes, exactly:

    no such object          walk/open/read of an absent id
    object exists           create of a live id
    object deleted          access to a tombstone
    object too large        write beyond objmax
    object lost             no good copy exists anywhere (§7.4)
    object unavailable      no up=yes replica at this epoch (§5.1)
    bad object name         oid violates §1.1
    reserved name           client create under shoal.
    bad create mode         DMDIR/DMAPPEND/DMEXCL/DMTMP
    bad open mode           ORCLOSE, or write on OREAD
    no rename               Twstat name change
    stale epoch             fid epoch < server epoch (§6.2)
    future epoch            fid epoch > server epoch (§6.2)
    not primary             client I/O to a non-primary; detail
                            SHOULD name the correct instance
    fenced                  instance has no fresh map (§6.4)
    down                    map says up=no for this instance
    degraded                a map-live replica would not take the
                            write (§5.2)
    stale version           /repl op older than the local copy
    checksum mismatch       local content fails verification
    still placed            drop of an object still in placement
    disk full               no space
    bad ctl                 malformed command or arguments
    unknown ctl             unknown verb
    bad aname               unparseable attach specifier

## 3. Cluster map

*Normative: the grammar, the attribute names below, and the
requirement to ignore unknown attributes and record kinds.
Implementation policy: attribute order, comments, whitespace.*

### 3.1 Format

ndb(6) style. A **record** begins at a line whose first character is
not white space and continues through any following indented lines.
Each line is a sequence of `attr=value` tuples separated by white
space. The record's kind is its first attribute. Record kinds:
`map`, `node`, `instance`. Readers MUST ignore attributes they do
not know and MUST ignore records whose kind they do not know; this
is what keeps a later replicated monitor (§8.5) additive.

    # shoal cluster map
    map=cluster0 epoch=41
        objmax=16777216 blksz=65536 replicas=2
        csum=blake2s256 placehash=blake2s256-64
        pollms=1000 leasems=3000 deadms=10000
        outmins=60 tombdays=7 mincopies=1 retain=8

    node=n2
        addr=tcp!10.0.0.2!17009

    instance=n2.1 node=n2
        addr=tcp!10.0.0.2!17011
        uuid=3f1c9a20b47e4d18a0c6e5721b93df04
        class=ssd weight=100
        status=in up=yes since=41

Exactly one `map` record MUST be present and MUST be first.

### 3.2 Header attributes

| Attr | Mutable | Meaning |
|---|---|---|
| `epoch` | monitor only | u64, strictly increasing (§6.1) |
| `objmax`, `blksz`, `csum`, `placehash` | **no** | fixed at creation; the monitor MUST reject changes |
| `replicas` | yes | R, target replica count, ≥ 1 |
| `pollms` | yes | map poll period, all parties |
| `leasems` | yes | self-fence deadline, MUST be > `pollms` |
| `deadms` | yes | unreachability before `up=no`, MUST be > `leasems` |
| `outmins` | yes | continuous `up=no` before automatic `in`→`out` |
| `tombdays` | yes | tombstone retention (§1.5) |
| `mincopies` | yes | refuse a write that would land on fewer than this many copies (§5.2) |
| `retain` | yes | number of past maps the monitor keeps (§8.2) |

### 3.3 Node and instance records

`node` records carry `node=<name>` and `addr=` (the node's monitor-
facing service address). Node names MUST match `1*63(ALPHA / DIGIT
/ "-" / "_")` and MUST NOT contain `.`, which keeps node-id and
instance-id byte strings disjoint (§4.2).

`instance` records:

- `instance=<node>"."<index>` — the instance id (`iid`), index a
  decimal integer unique within the node. Assigned by the monitor.
- `node=` — owning node; MUST equal the id's prefix (redundant on
  purpose: the map stays readable when grepped line by line).
- `addr=` — 9P dial string for this instance's export.
- `uuid=` — 32 hex characters, generated once by the instance when
  it first formats its disk. The durable identity: the iid is a
  label, the uuid is the disk.
- `class=` — device class tag (D5). Present from day one; v1
  placement MUST ignore it.
- `weight=` — capacity weight. Present from day one; v1 placement
  MUST ignore it, and a v1 monitor MUST reject any value other than
  `100` (§4.4) so that no operator can believe weighting works.
- `status=` — placement state, one of `new`, `in`, `out`, `dead`.
  **Only `status=in` participates in placement.**
- `up=` — liveness, one of `yes`, `heal`, `no`. Does **not** affect
  placement; affects primary selection and the ack set (§5).
- `since=` — epoch at which `status`/`up` last changed.

The `status`/`up` split is the load-bearing idea of the map: a
transient failure changes only `up`, so no object moves; data moves
only when an operator or the `outmins` timer changes `status`.

### 3.4 Identity acquisition when a disk is added

1. The operator starts an object server on the new disk with the
   node name, the monitor's address, and a class tag. On an
   unformatted disk it generates a random 128-bit `uuid`, writes it
   into a local superblock, and never changes it again.
2. It attaches to the monitor and writes
   `register uuid=<hex> node=<name> addr=<dial> class=<tag>`.
   Registration is idempotent and is repeated at every start.
3. The monitor binds the uuid to an iid — reusing the existing
   binding if the uuid is known, otherwise allocating
   `<node>.<smallest unused index>` — and records the instance with
   `status=new up=yes`. `status=new` does **not** enter placement,
   so registering a disk moves no data.
4. The operator inspects `/instances`, then writes `enable <iid>`
   and `commit` (§8.3). The epoch bumps, placement changes, and
   rebalance (§7.3) begins.
5. Moving a disk to a different node: the instance registers with a
   different `node=`; the monitor MUST NOT silently re-home it,
   because that changes placement. It reports the conflict in
   `/instances`; an operator writes `rehome <uuid> node=<name>` and
   `commit`. The disk keeps its objects; they become strays under
   the new map and are resolved by §7.

## 4. Placement

*Normative in full. Two implementations MUST agree bit for bit.*

### 4.1 Choice of algorithm

Two-level **rendezvous hashing (HRW)**: R distinct nodes by highest
node score, then one instance within each chosen node by a second
HRW round.

| | movement on map change | weights | cost per lookup | simplicity |
|---|---|---|---|---|
| HRW (chosen) | minimal: only the departing/arriving member's share moves | via virtual copies (§4.4), not enabled in v1 | O(nodes + disks-on-chosen-nodes); ~15 hashes at 12 nodes | ~30 lines, no tunables, no bucket types |
| jump hash | minimal, but only for tail growth | none | O(ln n) | disqualified: it returns an *index* into a dense range; removing a disk from the middle is not expressible |
| straw2 / CRUSH | minimal | native, exact | O(n) with a log per candidate | needs bucket types, rules, tunables, and a float `log`; famously subtle. Solves scale and heterogeneity we do not have |

At 3–12 nodes the cost column is irrelevant and the simplicity
column decides. HRW also hands us primary selection and failover
order for free (§4.3), which straw2 would not.

### 4.2 Hash

    H(x) = the first 8 bytes of BLAKE2s-256(x), unkeyed, 32-byte
           digest, interpreted as a big-endian unsigned 64-bit
           integer.

The map header's `placehash=blake2s256-64` names exactly this
function. A different value is a format change requiring a new
epoch and a cluster-wide flush; the attribute exists so the swap
can be additive (§9).

Score inputs, as byte strings with no separator beyond what is
shown, where `oid`, node ids and iids are their ASCII bytes:

    node score:      Sn = H( oid || 0x00 || 'N' || nid )
    instance score:  Si = H( oid || 0x00 || 'D' || iid )

`oid` contains no NUL (§1.1), so the concatenation is unambiguous;
`'N'`/`'D'` domain-separate the two rounds; node ids contain no `.`
and iids always do, so the two id spaces cannot collide.

### 4.3 Algorithm

Given the map at epoch `e` and an `oid`:

1. Let `I` = instances with `status=in`. Let `V` = the set of nodes
   owning at least one member of `I`.
2. For each `n ∈ V` compute `Sn`. Sort descending by `Sn`;
   **ties break in favour of the byte-wise greater node id**
   (unsigned byte comparison; if one id is a prefix of the other,
   the longer wins). Take the first `min(R, |V|)` nodes, in order.
3. For each chosen node, over its members of `I`, compute `Si`;
   take the maximum, same tie-break. The result is that node's
   selected instance.
4. The ordered list `P(oid) = [i0, i1, …]` is the **placement
   order**. `|P| < R` is legal and means the object is
   structurally under-replicated; instances MUST report the
   condition in `/status` and MUST still serve.

Replicas land on distinct nodes by construction (D4). Placement is
a pure function of `(oid, map)` — it MUST NOT consult liveness,
reachability, load, or free space, or clients and servers would
disagree.

**Serving primary.** The primary is the first instance in `P(oid)`
with `up=yes`. If no member of `P(oid)` is `up=yes`, the object has
no primary at this epoch and every access MUST fail with `object
unavailable`. Because `P` is stable across an `up` change, a node
failure promotes `i1` without moving a byte.

### 4.4 Weights: reserved, not implemented

`weight` is in the format (§3.3) and ignored. When it is enabled,
the rule SHALL be integer-exact virtual copies: an instance of
weight `w` (a multiple of 100, `k = w/100`) contributes candidate
ids `iid`, `iid || 0x00 || "1"`, … `iid || 0x00 || (k-1)` to the
instance round, and a node contributes `k` copies equal to the sum
over its instances. No `log`, no `pow`, no floating point, so the
result is reproducible across libm implementations — the reason
weighted HRW's usual `-w/ln(u)` form is rejected outright. Until
that is ratified, a v1 monitor MUST reject `weight` ≠ 100 with
`bad ctl`.

Consequence to state plainly: v1 fills disks in proportion to
*count*, not capacity. A cluster with a 1 TB and an 8 TB disk on
one node will fill the small one first. The envelope assumes
roughly homogeneous disks.

## 5. Write and read path

*Normative.*

### 5.1 Reads

A `role=client` read MUST be served only by the serving primary
(§4.3). Any other instance MUST answer `not primary`, with the
correct iid in the detail.

Why not replicas: with degraded writes (§5.2) a replica can
legitimately lag and a client cannot cheaply prove otherwise.
Primary-only reads make D2's contract hold by construction — the
primary is both the single serialization point for the object and
its only reader. It costs no aggregate bandwidth either:
parallelism comes from striping across objects (Layer B), which
spreads a file over every disk, not from fanning one object's reads
across its R copies. Replica reads become considerable only
alongside a per-object cleanliness proof; deferred, gated on
measurement, and needing no format reservation today.

### 5.2 Writes

The **ack set** `A(oid)` is defined by the *map*, not by observed
reachability: every member of `P(oid)` whose `up` is `yes` or
`heal`. This is the pivot of the whole design — see §5.4.

On a `role=client` write, the primary MUST, in order:

1. Check the fid's epoch (§6.2) and its own fenced state (§6.4).
2. Check that it is the serving primary for `oid`; else
   `not primary`.
3. Take the per-object lock. All concurrent writes to one object
   are totally ordered here; this is where D1's "primary-ordered"
   is realised.
4. If `|A(oid)| < mincopies`, fail with `degraded`.
5. Assign `ver := ver + 1`, `wepoch := current epoch`.
6. Send the operation to every other member of `A(oid)` over
   `/repl` (§5.3), in parallel, and apply it locally.
7. If every member of `A(oid)` acked and the local apply succeeded,
   update `csum` for the touched blocks, release the lock, and
   return `Rwrite`.
8. If a member of `A(oid)` failed or timed out, the primary MUST
   NOT ack. It fails the write with `degraded`, reports the peer to
   the monitor (`unreachable <iid>`, §8.3), and refreshes its map.
   The client retries; within `deadms` the monitor publishes
   `up=no` for that peer, `A(oid)` shrinks, and the write proceeds.

A write is thus stalled — not lost, not silently single-copied —
for at most about `deadms` while a failure is confirmed. That
bounded stall buys the invariant in §5.4.

**Dirty records.** When `A(oid)` is smaller than `P(oid)` because
the map says a member is `up=no`, the primary MUST durably record
`(oid, peer-iid, epoch)` in its dirty set **before** returning
`Rwrite`; recording after the ack would let a primary crash erase
the knowledge that a replica is behind, and heal would then see two
copies it believes are both clean. The record is per (object, peer)
pair, not per write, so a hot object costs one record.

### 5.3 The `/repl` channel

`/repl` is a **single file**, not a directory: a primary opens one
fid per peer and streams operations for many objects through it,
which makes bulk heal cheap (no walk per object). The write offset
is ignored. Each `Twrite` MUST be exactly one operation: a header
line, then, where a payload is defined, exactly `n` bytes.

    op=write  oid=<oid> ver=<u64> wepoch=<u64> off=<u64> n=<u32>
              csum=<hex32>
    op=create oid=<oid> ver=<u64> wepoch=<u64>
    op=trunc  oid=<oid> ver=<u64> wepoch=<u64> len=<u64>
    op=delete oid=<oid> ver=<u64> wepoch=<u64>
    op=full   oid=<oid> ver=<u64> wepoch=<u64> len=<u64>
              off=<u64> n=<u32> csum=<hex32> final=<0|1>

`csum` on `op=write`/`op=full` is BLAKE2s-128 (32 hex characters)
over the payload bytes only, an integrity check on the transfer.
`op=full` carries a whole-object resync in as many chunks as
needed; the receiver stages it and commits when `final=1`, so a
resync is never half-applied.

Receiver rules:

- The attach MUST be `role=repl` with `peer=`; otherwise `bad open
  mode`.
- The receiver MUST verify the payload `csum` and MUST fail with
  `checksum mismatch` on mismatch.
- The receiver MUST apply the operation iff `(wepoch, ver)` is
  greater than its local key, and MUST fail with `stale version`
  otherwise. It MUST adopt `ver`/`wepoch` verbatim.
- The receiver MUST NOT require that it be a replica of `oid` under
  its own map; during rebalance the sender's map is legitimately
  ahead. It MUST reject if its own epoch is greater than the
  attach epoch (`stale epoch`), which is what stops a fenced-era
  primary from writing.
- `Rwrite` count is the full bytes written, or an `Rerror`. There
  is no partial application.

Peer message size bounds `n`. A primary MUST size a forwarded write
to fit the smallest peer `msize` less header and `IOHDRSZ`, and
MUST use a short `Rwrite` to the client for anything larger. That
is why §2.4 mandates short-write handling.

### 5.4 The coherence invariant

> **I1.** For every object, every instance whose map state is
> `up=yes` holds every write that has been acked at any epoch.

Proof sketch: (a) a write is acked only after every member of
`A(oid)` holds it (§5.2 step 7); (b) `A(oid)` contains every
`up=yes|heal` member of `P(oid)`; (c) a dirty record is created
only for peers the map says are `up=no` (§5.2 step 8 refuses the
write otherwise), so a `up=yes|heal` instance never falls behind;
(d) an instance moves `no`→`heal`→`yes`, and the `heal`→`yes`
promotion happens only when every primary reports no dirty records
for it (§8.3), while it is already in the ack set for new writes.

I1 is exactly what makes failover safe: the promoted primary
(§4.3, first `up=yes`) is current by construction, so D2's contract
survives a node loss with no reconciliation on the read path.

Two honest consequences:

- With `R=2`, if the primary dies while its replica is `up=heal`,
  no member of `P(oid)` is `up=yes` and the object is
  **unavailable** (not lost) until either returns. The escape hatch
  is an explicit operator action (`promote <iid> force`, §8.3)
  which knowingly serves possibly-stale data and therefore breaks
  D2's contract for those objects. It MUST be logged as such.
- With `mincopies=1` (default) a write can be acked onto a single
  disk while the other replica is `up=no`. If that disk then dies
  before heal, the write is lost. Setting `mincopies=2` trades that
  away for unavailability during any single-disk outage. This is a
  product call — §10 (b2).

## 6. Epoch and fencing

*Normative.*

### 6.1 The epoch

`epoch` is a u64 in the map header, assigned only by the monitor,
strictly increasing, bumped on any change to membership, `status`,
`up`, or a mutable header attribute. An epoch bump is cheap: every
party re-reads a small text file. No epoch bump by itself moves
data; only a `status` change does.

### 6.2 Per-operation epoch checks

The fid's attach epoch is `Ea`; the server's current map epoch is
`Es`.

- `Ea == Es` → proceed.
- `Ea < Es` → `stale epoch`. The client MUST re-read the map and
  re-attach; it MUST NOT retry on the old fid.
- `Ea > Es` → `future epoch`. The server is behind. It MUST refuse
  the operation and SHOULD immediately fetch the map from the
  monitor. The client SHOULD retry with bounded backoff (SHOULD be
  ≥ 3 attempts spanning ≥ `pollms`) before failing upward.

`role=admin` fids are exempt; status and ctl remain reachable on a
stale-epoch instance, which is what makes debugging possible.

### 6.3 Map propagation without server-initiated messages

9P has no server→client message, so propagation is pull-only, in
two mutually reinforcing modes:

- **Poll.** Every party — storage instances, the MDS, any client
  library — reads the monitor's `/map` every `pollms` (default
  1 s). Steady-state staleness is bounded by `pollms`.
- **Fetch on rejection.** Any `stale epoch` or `future epoch`
  triggers an immediate fetch by the party that is behind. This is
  what makes correctness independent of `pollms`: the poll interval
  affects only how often work is wasted, never whether a stale
  party can act.

A monitor MUST answer a read of `/map` without requiring an epoch
in `aname` — otherwise nobody could learn the epoch.

### 6.4 Fencing a deposed primary

The hazard: instance X is primary at epoch 41; the monitor cannot
reach X, publishes 42 with `up=no` for X, and Y takes over. X is
alive and partitioned from the monitor but reachable by a client
still at epoch 41. Without fencing, X would ack writes that Y never
sees.

Rules:

- **F1.** An instance MUST refresh its map from the monitor at
  least every `leasems`. If `leasems` elapses with no successful
  refresh, it MUST enter the **fenced** state and fail every
  `role=client` read and write, and every `role=repl` operation,
  with `fenced`, until a refresh succeeds. Reads are fenced too: a
  deposed primary serving reads at an old epoch is exactly the D2
  violation we are preventing.
- **F2.** The monitor MUST NOT publish `up=no` for an instance
  until it has been unreachable for `deadms`, and `deadms` MUST be
  greater than `leasems`. A freshly started monitor MUST wait
  `deadms` after start before publishing any map that demotes an
  instance, because it has no history.
- **F3.** An instance whose own map says `up=no` or `status=out`
  for itself MUST refuse `role=client` I/O with `down`, even though
  it is manifestly reachable.

F1 + F2 give: X self-fences at `t0 + leasems`, where `t0` is its
last successful refresh; the earliest the monitor can depose it is
`t0 + deadms > t0 + leasems`. Assumptions, stated honestly: bounded
clock *rate* drift on each node (no clock synchronisation is
required — only elapsed-time measurement), and a monitor RTT that
is small against `deadms − leasems`. With defaults that margin is
7 s against a sub-ms LAN.

### 6.5 Single monitor: what it costs

v1 runs one monitor. The consequences must be said plainly:

- No monitor → no epoch bumps → no failover, no rebalance, no
  re-enable.
- No monitor → after `leasems` (3 s) every instance self-fences and
  **the cluster stops serving reads and writes**.

There is no safe way to soften F1: an instance cannot distinguish
"the monitor is down" from "I am partitioned and have been
deposed". Cluster unavailability from monitor loss therefore equals
monitor restart time. The mitigations are operational, not
architectural: the map is a small plain file, fsynced before every
ack, and mirrored into the object store (§8.2), so a monitor
restarts anywhere in seconds at the same epoch and un-fences
everyone. A replicated monitor is the fix and is deliberately
deferred (§8.5, §10 (b1)).

## 7. Heal, scrub, and rebalance

*Normative: the arbitration rule, the advertise/reconcile protocol,
and the drop rule. Implementation policy: scheduling, rate limits,
parallelism, partial-block repair.*

### 7.1 Dirty tracking

The dirty set is a durable list of `(oid, peer-iid, epoch)`
records, written before the ack that created them (§5.2), readable
at `/dirty`. It is bounded: if it exceeds an implementation limit,
the instance MAY discard the fine-grained records for a peer and
mark that peer `fullsync`, meaning "reconcile every object I am
primary for against this peer when it returns". `forget <iid>`
does this by hand. This bound is why D4's whole-object resync is
enough and no per-write log is needed.

### 7.2 Reconcile

After an epoch bump, and periodically, every instance scans its
**local** objects. Purely local scanning cannot discover objects
that *should* be here but are not, so the protocol is
advertisement-driven: holders announce, and the primary arbitrates.

For each local object `o`, with `P(o)` at the current epoch:

- If this instance is the serving primary of `o`: run **full
  reconcile** — collect `(wepoch, ver, csum, len)` from every
  member of `P(o)` and from the placement sets of the retained
  older maps (§8.2), take the greatest key, `pull` it if it is not
  local, then `push` (`op=full`) to every member of `P(o)` whose
  key or `csum` differs, then clear the relevant dirty records.
- Otherwise: **advertise** to the serving primary and let it
  arbitrate. This includes the case where this instance is not in
  `P(o)` at all (a **stray** left by a rebalance) — advertising is
  how a new primary that holds no copy learns the object exists.

`/advert` is a single file taking one line per object; a bulk
advertisement of a whole disk is a few tens of megabytes and
SHOULD be rate-limited. It is one-way: the receiver records and
then initiates pulls or drops itself; nothing needs to come back
down the write.

    oid=<oid> ver=<u64> wepoch=<u64> csum=<hex64> len=<u64>
      state=live|tomb

### 7.3 Rebalance and stray deletion

A `status` change moves the placement of roughly `1/|V|` of
objects. There is no separate rebalance mechanism: the epoch bump
triggers the §7.2 scan, adverts inform the new holders, and the new
primaries pull. Movement is bounded by HRW's minimal-disruption
property — only the arriving or departing member's share moves.

A stray copy MUST NOT be deleted by its holder. Only after the
serving primary has confirmed that every member of `P(o)` holds the
current version does it send `drop <oid>` to the stray holder. The
holder MUST verify, against its own current map, that it is not in
`P(o)`; if it is, it MUST answer `still placed`. A stray that has
never been dropped MUST be reported in `/status` and MUST NOT be
reclaimed automatically. Losing capacity to strays is recoverable;
deleting the last copy is not.

If more than `retain` epochs pass, a stray's old placement set is
no longer computable and it becomes an **orphan**, findable only by
a full-cluster list comparison (`reconcile` with no argument on
every instance, or an operator tool). This is accepted.

### 7.4 Scrub

Each instance continuously re-reads its objects and verifies block
digests and `csum`, rate-limited so that a full pass completes in
about `scrubdays` (implementation policy; a default near 14 days
sizes to ~4 MiB/s on a 4 TB disk).

On mismatch:

1. Mark the object locally corrupt, list it in `/lost`, and fail
   client access with `checksum mismatch`.
2. Its arbitration key becomes `(0, 0)` (§1.3), so it loses to any
   peer copy unconditionally.
3. The serving primary reconciles it from a peer. A repair MAY
   transfer only the mismatching blocks (implementation policy);
   the normative repair is whole-object `op=full`.
4. If no peer holds a verifying copy, the object is **lost**: it
   stays in `/lost` and client access fails with `object lost`.
   Losses are never silently papered over.

This is D4's argument in action: the checksum and the redundancy
are in the same layer, so a corrupt copy can be arbitrated away —
which a block-layer mirror underneath could not do.

## 8. Monitor service

*Normative: the file tree, the ctl grammar, the map format
(§3), and the forward-compatibility rules. Implementation policy:
how it stores its state locally.*

### 8.1 File tree

    /ctl            verbs (§8.3)
    /map            current map, text (§3)
    /map.next       staged map under construction, text
    /maps/<epoch>   the last `retain` published maps
    /instances      registry: uuid, iid, node, last registration
    /health         liveness observations: last-heard times, RTTs
    /status         monitor state: epoch, uptime, timers, retain

`/health` is *not* authoritative for anything a client computes; it
is what the monitor is about to act on. Placement and the ack set
come from `/map` only.

### 8.2 Durability and retention

The monitor MUST fsync `/map` before acknowledging a `commit`, and
MUST keep the last `retain` (default 8) published maps under
`/maps`, since reconcile needs older placement sets (§7.2). It
SHOULD additionally write each published map into the object store
as a reserved object `shoal.map.<epoch>`; that copy is a backup for
rebuilding a monitor, not an authority — the local file is
authoritative, because reading the object store requires a map.

### 8.3 ctl grammar

Map edits are **staged then committed**, so an operator can read
the diff before anything moves. Alternatively an operator MAY write
a complete map text to `/map.next` directly and `commit` it — the
format is small and human-editable on purpose.

| Verb | Form | Effect |
|---|---|---|
| `register` | `register uuid=<hex> node=<n> addr=<a> class=<c>` | Idempotent instance registration (§3.4); result in `/instances`. |
| `propose` | `propose` | Start (or reset) a staged map from the current one. |
| `enable` | `enable <iid>` | Staged: `new`\|`out` → `in`. |
| `disable` | `disable <iid>` | Staged: `in` → `out`. Data will move. |
| `rehome` | `rehome <uuid> node=<n>` | Staged: re-bind a moved disk. |
| `setclass` | `setclass <iid> <class>` | Staged. |
| `set` | `set <attr> <value>` | Staged header attr; immutable attrs rejected. |
| `commit` | `commit` | Publish the staged map at `epoch+1`. |
| `abort` | `abort` | Discard the staged map. |
| `bump` | `bump` | Publish an unchanged map at `epoch+1`. Forces a cluster-wide refresh; used for testing fencing. |
| `unreachable` | `unreachable <iid>` | Reported by a primary (§5.2 step 8). Advisory input to failure detection; MUST NOT depose on its own. |
| `clean` | `clean <iid> epoch=<e>` | A primary reports no dirty records for `<iid>` as of epoch `<e>`. |
| `dirty` | `dirty <iid>` | A primary reports it has created a dirty record for `<iid>`; the monitor MUST demote `up=yes` → `heal` at a new epoch. |
| `promote` | `promote <iid> force` | Operator override: `heal`/`no` → `yes` without clean reports. Knowingly breaks D2 for objects that instance is stale for; MUST be logged. |

Automatic transitions the monitor performs itself, each publishing
a new epoch:

- unreachable for `deadms` → `up=no` (never sooner: F2).
- reachable again → `up=heal` (never straight to `yes`).
- every primary has reported `clean` for it at the current epoch →
  `up=yes`.
- continuously `up=no` for `outmins` → `status=out`, which is the
  only automatic transition that moves data.

The `heal` state, and the `clean`/`dirty` reports that gate it, are
what make invariant I1 (§5.4) hold across a node returning to
service.

### 8.4 Immutable attributes

`objmax`, `blksz`, `csum`, `placehash` are fixed at cluster
creation. `set` on any of them MUST fail with `bad ctl`. Changing
`objmax` would silently break Layer B's arithmetic (D3); changing
`csum` or `placehash` would invalidate every stored checksum or
every placement decision at once.

### 8.5 Forward compatibility with a replicated monitor

The v1 monitor is one process. Everything above is chosen so that
replication is additive, never a format break:

- Readers MUST ignore unknown map attributes and unknown record
  kinds (§3.1), so a `mon=` record kind and a `term=`/`leader=`
  header attribute can be added later without touching any v1
  parser.
- The epoch is already the only ordering authority in the system,
  and it is already required to be strictly increasing and assigned
  in one place. A consensus protocol replaces *how* an epoch is
  assigned, not *what* it means.
- Clients already reach the monitor only by reading `/map`, so a
  replicated monitor can serve `/map` from any replica, and the
  staged-map ctl surface can be restricted to the leader with a
  redirect error, with no change to the client path.

## 9. Hash choice — settling D6

*Normative: the algorithms, digest lengths, and encodings, all as
already specified in §1.4 and §4.2. This section is the
justification.*

Stock 9front libsec provides blake2s, sha2_64 (SHA-224/256),
sha2_128 (SHA-384/512), and sha3. BLAKE2s and SHA-256 are both
available with no new code.

**Recommendation: BLAKE2s everywhere.**

- **Object checksums:** BLAKE2s-128 per block, BLAKE2s-256 over the
  block digests. BLAKE2s beats SHA-256 on hardware without SHA
  extensions — the common case on a 9front fleet — and checksumming
  sits in the write path, so the margin is real throughput. Digest
  length is a BLAKE2 parameter, not a truncation, so BLAKE2s-128 is
  a specified function rather than a chopped-off hash; 128 bits is
  ample for corruption detection, which is not adversarial, while
  the 256-bit value that crosses the wire stays full strength.
- **Placement:** the *same* primitive (§4.2). Placement needs no
  cryptographic strength, so cheap hashes were considered: FNV-1a
  is rejected outright — its weak avalanche correlates adjacent ids
  like `f.0` and `f.1`, precisely the pattern Layer B generates;
  SipHash-1-3 would fit in ~60 lines but is rejected for v1 because
  the cost it saves does not exist. A 12-node lookup is ~15 hashes
  of ~80-byte inputs, on the order of a microsecond, against a LAN
  RTT three orders of magnitude larger. One primitive means one
  implementation to get right and one vector set to test.
- **Reserved escape.** `placehash=blake2s256-64` sits in the map
  header from day one (D5-style cheap reservation). If placement
  ever appears in a profile, SipHash-1-3 enters as a new *value* of
  an attribute that already exists and is already checked — not a
  change of format shape.

This settles D6's open half and should be ratified as a new
decisions.md row. This document does not edit `docs/decisions.md`.

## 10. Conflicts, deviations, and open questions

### 10.1 Conflicts and deviations

Nothing here contradicts D1–D6. Four places where this proposal
adds to, constrains, or strains the ratified material, each stated
so it can be rejected:

1. **"Stock 9P" and `mount(1)` (D2).** Carrying the epoch in
   `aname` (§2.1) is stock 9P2000 — no new messages, no new fields.
   But it means a raw kernel mount of a storage instance breaks at
   the next epoch bump, so Layer A must be consumed through a
   client library that re-attaches. Layer A is never what a user
   mounts (that is Layer C), so no user-visible property is lost;
   the alternative — a per-operation epoch — would need either a
   protocol extension (forbidden by D2) or a stateful ctl write per
   operation (racy). Flagging it because "stock 9P only" could be
   read as "mountable by the kernel".
2. **Primary-only reads (§5.1)** are an addition, not a
   restriction, in target.md's terms: target.md fixes the write
   path and is silent on reads. The choice follows from D2 and is
   reversible later without a format change.
3. **`weight` present but rejected unless 100 (§4.4).** target.md's
   brief left weighting as a conclusion to reach; the conclusion is
   "not in v1". Rejecting non-default values is stricter than
   merely ignoring them, on the grounds that a silently ignored
   weight is a worse failure than a refused one. The cost is
   explicit: unequal disks fill unevenly.
4. **Reserved `shoal.` id prefix (§1.1)** constrains D3's not-yet-
   fixed `fileid` grammar. D3 marks the naming scheme normative but
   defers the exact grammar to this round; this is that
   constraint being registered, not a deviation from it.

One consequence of the design that target.md does not promise and
the owner should see (§6.5): **losing the single monitor takes the
cluster read-unavailable within `leasems` (3 s by default)**, not
merely "no failover until it returns". That falls out of the
fencing lease and cannot be softened without reintroducing
split-brain.

### 10.2 (a) Settleable from evidence by another engineer

- Measured `msize` on 9front, which sets the forwarded-write chunk
  and therefore how often clients see short writes.
- Whether `blksz=65536` and BLAKE2s-128 block digests sit at the
  right point on the metadata-size vs re-hash-cost curve, and what
  a per-4 KiB-write re-hash actually costs on the fleet.
- Whether `objmax=16 MiB` is right, against resync time, object
  counts, and Layer B's striping efficiency.
- Timer defaults (`pollms`, `leasems`, `deadms`, `outmins`,
  `tombdays`, `retain`, scrub rate). All are map attributes, so
  changing them is never a format change.
- Whether a directory read of `/obj` at ~2.6·10^5 entries is
  comfortable on 9front, or whether enumeration needs streaming.
- Local object-store layout: file per object vs packed store; where
  block digests live; how the dirty set is made crash-safe; and
  what the underlying 9front file system guarantees about
  durability on write, which sets what §5.2's "durably record"
  costs.
- Whether partial-block repair (§7.4 step 3) is worth implementing.
- BLAKE2s vs SHA-256 throughput on the fleet, to confirm §9's
  premise with numbers rather than a general claim.

### 10.3 (b) Product and design calls for the project owner

1. **Monitor availability.** Monitor loss stops the cluster within
   `leasems`. Acceptable for v1 with a fast-restart story, or does
   v1 need a replicated monitor?
2. **`mincopies` default.** Default 1 accepts a write onto a single
   surviving disk (available, but a second failure loses it).
   Default 2 refuses writes to affected objects during any single-
   disk outage (durable, but less available than one node). Which
   is the product?
3. **Failure-detection stall.** Writes to affected objects stall
   for up to `deadms` (10 s) while a failure is confirmed — the
   price of invariant I1. Is a 10 s stall acceptable, or should
   `deadms` shrink at the cost of flappier `up=no` transitions?
4. **`promote force`.** Should the operator override that serves
   possibly-stale data (§5.4) exist at all in v1, given it
   knowingly breaks D2's contract?
5. **Authentication.** Is Layer A a trusted-network service in v1
   (no auth on attach), or must attaches authenticate via
   factotum/p9any? This decides whether `role=admin` is a real
   privilege boundary or a convention.
