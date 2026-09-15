# Design decisions

Each row states the decision, its rationale, and which parts are
**normative** (a reimplementation must match) vs **implementation
policy** (a conforming implementation may differ). Newest last.

## D1 — Native build; server authority; mutable objects (2026-08-23)

**Decision:** Build a Plan 9-native system rather than porting
Ceph/GlusterFS/pNFS, with authoritative server-side state and
mutable objects as the foundation. Content addressing is demoted to
a later per-tree archival mode.
**Rationale:** Ports fail structurally (C++ toolchain; xattr/FUSE/
hardlink dependencies; the NFSv4.1 state machine). Content
addressing only dissolves the consistency problem for read-mostly
data; the target workload (live mutable tree, delete, concurrent
writers) requires authority somewhere, and per-object primaries are
the cheapest honest place.
**Normative:** mutable-object model, primary-ordered replication.
**Implementation policy:** everything about how a node stores its
objects locally.

## D2 — Coherence by cacheless clients, not protocol extensions (2026-08-23)

**Decision:** v1 clients cache nothing; every operation reaches a
server, which is the serialization point. Stock 9P only. Leases/
callbacks/batching are performance upgrades considered only after
measurement on the real grid.
**Rationale:** Callback machinery in CephFS/pNFS exists to make
client caching safe. On a sub-ms LAN, not caching is viable (cf.
diod's cacheless HPC clients) and makes concurrent multi-node
writers coherent by construction. The published 9P latency lessons
(Op/ZX) are WAN lessons; our envelope is LAN.
**Normative:** the coherence contract (a completed write is visible
to every subsequent read through the servers).
**Implementation policy:** any future caching layer, provided the
contract holds.

## D3 — Canonical object naming for file data (2026-08-23)

**Decision:** File bytes are ALWAYS stored as Layer A objects named
by the canonical striping scheme (`fileid.N` with the file's stripe
parameters), including when the v1 MDS proxies all data I/O. The MDS
never invents a private data layout.
**Rationale:** Keeps the future direct client→storage data path
purely additive (a client computes the same names and talks to
storage nodes directly, plus one new size/attr-commit operation) —
no migration, no format break. Data-through-MDS is accepted for v1
as a correctness-first bottleneck.
**Normative:** the naming scheme and stripe-parameter semantics
(exact grammar fixed in the Layer A design round).
**Implementation policy:** whether a given deployment ever enables
the direct path.

## D4 — One object-server instance per disk (2026-08-23)

**Decision:** Each disk a node contributes runs its own object-server
instance. The cluster map is a fixed two-level hierarchy
(node → disk); the placement rule puts replicas on distinct nodes.
On-node disk redundancy is NOT a separate subsystem — disk loss and
node loss are healed by the same replication machinery.
**Rationale:** One mechanism instead of two; per-disk failure
isolation; and a block-layer mirror below the store cannot arbitrate
a corrupt copy, whereas the object layer's checksums can (the gefs
argument for putting redundancy next to the checksums).
**Normative:** two-level map hierarchy; replicas-on-distinct-nodes
rule; per-disk instance identity in the map.
**Implementation policy:** a deployment MAY still run local RAID
beneath a single instance; shoal neither requires nor manages it.

## D5 — Device classes reserved; dynamic tiering out of scope (2026-08-23)

**Decision:** The cluster-map format carries a per-disk `class` tag
from day one. Static class-filtered placement (a pool/tree pinned to
a class) is a permitted later milestone. Automatic tiering,
promote/demote migration, and cache tiers are out of scope.
**Rationale:** The tag and filter are cheap and map-format changes
are the expensive kind, so reserve now. The dynamic behaviors are a
known complexity sink (Ceph deprecated its cache tiering).
Hybrid-by-static-classes ≈ separate clusters composed by namespace,
sharing one monitor and codebase.
**Normative:** presence of the `class` field in the map format.
**Implementation policy:** class names and any placement policy
using them.

## D6 — No SHA-1 (2026-08-23)

**Decision:** SHA-1 appears nowhere. Object checksums and any future
content-addressed mode use a modern hash; the specific algorithm
(BLAKE2/3 vs SHA-256) is chosen in the Layer A design round.
**Rationale:** SHA-1 is collision-broken; venti's choice is
heritage, not a spec to inherit.
**Normative:** the eventual algorithm choice, once fixed in the wire
and map formats.

## D7 — BLAKE2s everywhere (2026-08-24)

**Decision:** Object checksums are BLAKE2s-128 per block and
BLAKE2s-256 over the block digests; transfer digests are BLAKE2s-128;
the placement hash is the first 8 bytes of BLAKE2s-256. Exact
definitions: `design/layer-a.md` §1.4, §4.2, §5.5. Closes D6's open
half.
**Rationale:** Present in stock 9front libsec (verified on the
fleet); faster than SHA-256 on hardware without SHA extensions;
digest length is a BLAKE2 parameter, not a truncation; one primitive
to implement and test. Cheap non-crypto placement hashes rejected —
the cost they save is sub-microsecond against a LAN RTT; `placehash=`
stays reserved for a swap.
**Normative:** algorithms, digest lengths, encodings.

## D8 — Layer A protocol design ratified (2026-08-24)

**Decision:** `design/layer-a.md` is the ratified Layer A contract
that M1–M4 build against, with its own per-section
normative/implementation-policy markings governing. Ratified by the
owner after an adversarial design round (draft → two reviews →
redesign → two delta reviews → verification; the git log of that file
is the record). Defaults `mincopies=1` and `deadms=10000` stand;
like every timer they are mutable map attributes, configurable per
cluster.
**Normative:** per the document's own markings.

## D9 — Cheap-now requires a recorded good-later (2026-08-24, Victor)

**Decision:** v1 may ship the cheap variant of a mechanism only when
the good variant is recorded as the committed target and the upgrade
is additive (no format or wire break). Instances bound by this row:
(a) **monitor** — v1 single-process with a fast-restart story; a
replicated monitor (or equivalent availability) is the committed
design, and layer-a §8.7's additivity rules are normative to keep it
so; (b) **authentication** — v1 unauthenticated, permitted ONLY on an
isolated network; production assumes threat actors on the network, so
factotum/p9any auth (role derived from authenticated identity) is
committed and REQUIRED before any non-isolated deployment; (c)
**staleness ledger** — per-pair in v1; a finer ledger is the recorded
refinement if per-pair promotion-blocking hurts in practice
(additive record kinds). Acked writes are never discarded without an
operator, in v1 or later.
**Why:** Victor 2026-08-24: "the good solution is the target… begin
cheap… as long as it is properly noted and intended to be replaced."
**How to apply:** any future cheap-variant shipping decision cites
this row and records its target the same way.

## D10 — Recovery overrides are not operator UI (2026-08-24, Victor)

**Decision:** The override family (`promote force`, `forcesync`,
`commit force`, `forceepoch`, `retire` of a mark's reporter,
`newmonid`) is post-mortem/data-recovery tooling. It exists, is
always logged as a data-loss event, and in any end-user product MUST
be separated from the regular operator surface (separate tool, role,
or gating). v1 may co-locate it on the ctl surfaces. This is a
general principle for all future data-loss-capable tooling, not a
list frozen to these six verbs.
**Normative:** the logging requirement; the separation requirement is
a product obligation, not a v1 wire property.

## D11 — Zone failure domain reserved (2026-08-24)

**Decision:** The map format carries `zone=` per instance and a
`placerule=` header attribute (v1 defines and accepts only `nodes`);
zone-aware three-level placement — replicas on distinct zones via a
domain-separated HRW round — is reserved with exact byte strings in
`design/layer-a.md` §4.5, not implemented. Enabling it later is a
staged placement-rule change that moves data, not a redesign.
**Rationale:** Owner requirement for future datacenter deployments
(buildings, power feeds as failure domains); map-format changes are
the expensive kind, so the hook lands now — same treatment as device
class (D5) and weights.
**Normative:** the attributes and the reserved rule's byte strings.
**Open at enable time:** the scarce-zone spill question (layer-a
§4.5).

## D12 — 9front native C toolchain; lib9p for 9P (2026-08-27)

**Decision:** shoal is implemented in 9front's native C — the Plan 9
C dialect, built with `6c`/`6l` under `mk` — and builds and tests run
on 9front itself; a Linux host, if any, is git hosting only. The 9P
server side uses stock `lib9p` (9p(2)). Its `srv` loop is
single-threaded, so object operations are pushed to a pool of
`9pqueue`(2) `Reqqueue`s hashed by oid — which makes the server a
libthread program started with `threadpostmountsrv` — and
`Srv.flush` is `reqqueueflush`, as `design/layer-a.md` §5.4.1
requires. One queue per oid hash and one proc per queue give that
section's per-object total order and cross-object concurrency by
construction; `srvrelease`/`srvacquire` with explicit per-object
locks is the plain-libc alternative and was not taken
(`design/store.md` §7). Hashing uses libsec's BLAKE2s (D7).
**Rationale:** Native C is the only toolchain 9front ships and
maintains, so it is the only one whose breakage is anyone's problem
but ours. `lib9p` is what every 9front file server is written
against; its idioms — `Srv`, `srv`, `respond`, `Reqqueue` — are the
ones a 9front reviewer expects, and §5.4.1's concurrency and
`Tflush` requirements are stated in its terms. Building on the
target means every test exercises the real kernel: `devmnt`'s
`Tflush`-on-interrupt behaviour, real `msize` negotiation, real
`ERRMAX` truncation.
**Considered and rejected:** Go's plan9 port — a secondary-tier port
with no maintained 9P *server* library, so the 9P surface would be
ours to write and maintain anyway. plan9port or a Linux cross-build
— exercises a userspace 9P client, not the kernel's `devmnt`, which
is exactly the component §5.4.1 and §2.1 are written against.
**Implementation policy — the whole row.** A reimplementation may
use any language, library and build system it likes; what a
conforming implementation must match is the wire and the formats
(`design/layer-a.md`), not this.

## D13 — Durable state lives on raw partitions shoal manages itself (2026-08-27)

**Decision:** shoal's durability point is a raw disk partition it
manages itself, not a file on a 9front file system. Each
storage-server instance owns a raw partition — one instance per disk
(D4) — and implements its own on-disk store with write-ahead commit,
so that `design/layer-a.md` §1.3's four-tuple atomicity and §5.4's
durability-before-ack are properties of code shoal owns. The monitor
likewise owns a small raw partition, a slot store, for the map (§8.2).
**Rationale:** `platform/9front-storage.md` is the evidence. No 9front
file system gives durable-before-ack at a price the write path can
pay. cwfs and hjfs acknowledge writes that are only in their own
buffer cache, lose them on power loss, and are left structurally
damaged by the crash — unremovable directories, out-of-range block
pointers, files whose `stat` succeeds and whose `read` does not — and
cwfs reboots reporting nothing wrong. gefs is correct, and the only
one that survives a crash intact, but its null-`Twstat` commit costs
530–620 ms per durable write: ~60× `replms`, which also breaks §5.4
step 5a's bound on the monitor round trip inside a client write. A raw
partition costs 8.4 ms per commit and scales with concurrency.
**Considered and rejected:** cwfs or hjfs plus a console `sync`. The
`sync` does make prior writes durable, but it is a whole-file-system
barrier whose cost is set by other writers (25.3 s measured after
40 MB of unrelated dirty data), there is no per-file alternative, and
the container itself does not survive the crash — a store built on it
would need a repair story for the file system as well as for its own
records.
**Recorded fallback:** gefs plus a null `Twstat` is a correct,
zero-implementation answer at ~8 durable writes/s aggregate. It is the
fallback if the raw store is not built, and taking it means
renegotiating the throughput target.
**Deployment consequence, as fact:** shoal data disks are raw
partitions, not cwfs, and the monitor needs a partition of its own (a
few MB) on whichever disk it runs; carving them is an operator step.
A SCSI `SYNCHRONIZE CACHE` is issued after each commit write: real on
virtio and on AHCI, a silent no-op on the legacy IDE driver, where
durability therefore additionally requires the drive's write cache to
be off (`platform/9front-storage.md` §5).
**Implementation policy — the whole row.** The mechanism is ours; a
conforming implementation may store objects however it likes. The
normative requirements are unchanged and live in `design/layer-a.md`
§1.3, §5.4 and §8.2.

## D14 — `op=meta corrupt=1` and the corrupt-receiver rule (2026-08-28, Victor)

**Decision:** `op=meta` gains a third response form — the ordinary
`meta` line with `corrupt=1` appended — which an instance whose copy
fails local verification MUST answer, in place of a key or
`absent=1`. It contributes no key, satisfies a currency check as a
response, counts as neither kind of tombstone-discard confirmation,
and licenses the serving primary's `op=full force=1` at an equal
key. Its companion receiver rule: an instance whose own copy fails
local verification treats that copy as absent for the `op=full`
comparison and accepts the push at any key. Both live in
`design/layer-a.md` §5.6 and §5.5.
**Rationale:** Every answer available without it is wrong — a key
claims the arbitration position §1.3 forbids a failing copy,
`absent=1` is a lie that §1.5 counts as a positive confirmation
licensing a discard the holder cannot vouch for, and an error is not
a response at all, so §5.2's currency check becomes permanently
incompletable and every read and write of the object answers
`not ready` cluster-wide, forever, on one media fault. The receiver
rule closes the case the response form alone leaves open: a holder
that committed `(E, ver+1)` and then lost the content contributes no
key, so the primary wins arbitration at the lower `(E, ver)` and its
repair push arrives neither greater nor equal — refused
`stale version` by the very copy that asked for it, unrepairable for
the life of the disk and blocking tombstone discard for as long.
A response form rather than an error because `op=meta` is answered
whatever the instance's `up`/`status`, and a caller must be able to
tell a corrupt holder from an unreachable one.
**Owner's note on why not `absent=1`:** "I would have thought it
ought to be solvable with absent=1, but that might have made the
complexity and latency rise sharply."
**Normative:** the response form and the receiver rule.
**Implementation policy:** how an instance detects a failing copy and
records the condition across a restart, and whether a repair
transfers whole objects or only the mismatching blocks (layer-a
§7.5; `design/store.md` §8).

## D15 — An absent id answers `op=discard` with `no such object` (2026-09-09, Victor)

**Decision:** A receiver of `op=discard` that holds no record at all
for the named id answers `no such object`, not `not discardable`.
`design/layer-a.md` §1.5's receiver rule now says so; §5.6's table
already listed it.
**Rationale:** §1.5's check (i) judges the receiver's record and had
no wording for there being none, while §5.6's table listed
`no such object` for `op=discard` as it does for `op=drop` and
`op=verify`; the store (`design/store.md` §3.7) followed the table
from wave 1c-α and the contract text is reconciled to match.
Nothing distinguishes the two answers: §1.5's sender rule removes the
primary's own record only after every holder answered `ok`, and
treats any other answer as an incomplete discard to retry on a later
pass. A receiver lacks the record either because an earlier pass
discarded it and the `ok` was lost, or because a §7.4 drop removed a
stray's copy between its confirmation and the discard; in both cases
it holds nothing that could resurrect the object, and on the next
pass it confirms `absent=1` and is not sent the discard at all.
**Normative:** the answer.
**Implementation policy:** none.

## D16 — A snapshot outlives `storeclose`; neither fatal nor "gone" (2026-09-15, Victor)

**Decision:** `storeclose` under an open object snapshot no longer
`sysfatal`s. It stops the store's procs, then sets `closed` under
`qlstate`; the `Store`'s memory is released by whoever observes
`closed && nobjsnap == 0` — `storeclose` itself when no snapshot is
open, otherwise the last `objsnapclose`. A snapshot taken before the
close still answers `objsnapcount`, and `objsnapent` through it
fails `store closed` (a local error, no layer-a §2.6 prefix, §3.7).
An `Objsnap` handle is the **only** thing that may outlive a
`storeclose`: the `Store*` is invalid as before, so `dirtysnap`,
`lostsnap`, `fullsyncsnap` and `storestat` are given no such check.
The caller **quiesces, then closes**: none of those calls may be in
flight when `storeclose` runs either, because each waits on the
state lock holding nothing that keeps the `Store` alive, so one
queued there when the last release frees it wakes in freed memory —
a window the engine cannot close, since a waiter would have to be
counted under the lock it is waiting for. After the close, only
`objsnapent`, `objsnapcount` and `objsnapclose` on handles taken
before it. `design/store.md` §9 and §7 now say so.
**Owner's direction (Victor):** "a fs that just dies" is the wrong
shape; serving wrong data is worse, but neither is acceptable.
**Rationale:** Both prior answers were rejected. The *lie* — delete
the fatal and let a freed `Store` be rendered — is not a fault but a
plausible short listing: the walk finds no `qidpath` match and
answers *gone* for every entry, so a fid-lifetime bug in a server
surfaces as a silently truncated `/obj`. The *fatal* answers a
caller's bug by killing a file server. Deferring the free by a
reference costs one `int` and closes a third hazard neither answer
touches: a freed `Store` address can be handed straight back to the
next `storeopen` — §0's `Echange` close-and-reopen is that shape —
and `Objsnap.s` is a bare pointer with no generation, so an old
`/obj` fid would render entries out of the *new* store. The old
`Store` cannot be freed while a snapshot names it, so the reuse is
unreachable. `closed` doubles as the store's own reference rather
than sitting beside a second counter, so nothing can drift out of
step with the bound's `nobjsnap`; and it is set only after the proc
wait, since a last `objsnapclose` during that wait would otherwise
free the `Store` while `storeclose` slept inside it.
**Normative:** none. Nothing here reaches a wire or an on-disk
format.
**Implementation policy:** all of it — that a snapshot may outlive
the close at all, the `store closed` text, which calls are given the
check and which stay undefined, and the refcount as the mechanism. A
conforming implementation may refuse the close, or defer it, or hold
the store alive some other way, so long as no caller is served an
entry that is not there.
