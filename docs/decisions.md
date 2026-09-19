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
when it was built and the contract text is reconciled to match.
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

## D17 — A failing checkpointer backs off, and a dead one is named (2026-09-15)

**Decision:** A failed checkpoint keeps §2.8's reaction — no
condemnation, no self-check, no device probe, no page rewrite, no
full-checkpoint fallback — and gains two things it lacked. First, a
retry floor (`ckbackms`, default 100 ms, doubling per consecutive
failure and capped at `max(ckbackms, min(ckms, ckwaitms))` — never
below the configured floor, never above §6's bounded wait — and reset
to `ckbackms` by any success), obeyed by both of §2.8's triggers and
by a committer's request inside §6's wait, and exempted only for an
explicit `storecheckpoint`. Second, a `dead` condition distinct from
`stuck`, set when the failing checkpoint's device fid is condemned
(§0's `Echange`), never cleared short of a restart, stopping the
paced retries and named in §6's refusal.
**Rationale:** Nothing a checkpoint failure damages is at risk — the
log is the authority above `cklogoff`, §3.4's rule makes replay cover
every partial landing, and the damaged bytes are never read while the
store is up — so a self-check protects nothing, a rewrite is what the
next checkpoint already does, and a probe can classify nothing the
failing write's own error string did not already carry. What was
wrong was the cadence: `ckhigh` is a level with no time term and the
checkpointer skips its tick whenever a trigger is true, so a store
that cannot reclaim log space re-attempted with no sleep at all —
measured at 66 715 attempts per second — taking the log's lock twice
per attempt against the very commits waiting for the space. The cap
is §6's wait and not `ckms` alone because the floor is what a healed
device waits behind: at the shipped 30 s `ckms` against a 5 s
`ckwaitms` a floor allowed to reach `ckms` would go on refusing
commits with a cured error for up to 30 s after the device came back,
where the same floor capped at the wait is retried inside it. And
`Echange` is not a device that may heal; reporting it as one sends
the operator to look at a disk instead of restarting the store.
**Normative:** none. `disk full` remains layer-a §2.6's prefix and is
unchanged; what follows it is implementation policy (§3.7).
**Implementation policy:** all of it — the floor and its defaults,
the doubling and its cap, the stuck/dead split, the refusal wording,
and the statistics. A conforming implementation may pace a failing
checkpointer any other way, or name a permanently failed one
differently, so long as a store that cannot checkpoint does not spin
and an operator can tell a device that may heal from one that cannot.

## D18 — A condemned slot's grains are reclaimed online by the scrubber; the offline rebuild is the not-serving store's reclaim (2026-09-15, Victor)

**Decision:** A slot §5 step 10 condemned leaks the grains its
damaged extent map named: `op=delete` over it commits a tombstone
whose `nfree` names nothing, and §3.6's `op=full` over it rebuilds
the map in a fresh slot over fresh grains. The permanent reclaim is
**online and scrub-driven** — the storage server's scrubber already
reads every live entry's map, so its pass accumulates a shadow
bitmap and swaps it in page by page under `qlstate`, behind a write
barrier on the two bitmap mutators — and it needs a per-slot
generation stamp, because the block repair and the `corrupt`-flag
commit both publish with the four-tuple unchanged while the map
changes. The count stands beside it: each leaking exit adds
`blkcount(len)` to a memory-only **leaked-grain count**, reported as
`grainleak=`, starting at zero at every start and discharged by the
swap that returns the grains. The offline reclaim is not an interim:
`shoalck -R`, and §5 step 11 when start found a damaged bitmap page,
stay as the same reclaim on a store that is not serving — the one
state a scrubber cannot reach. `design/store.md` §6 and §8 hold the
behaviour and say which half of it is built; §13 has the test plan.
**The one rule.** A reclaim strategy may be any of these so long as
it **never frees a grain a map it read named**. That is the one way
this mechanism destroys data rather than a number, and everything
under it — the barrier, the stamp, §8's coverage interlock, the
offline rebuild's own full scan, and D24's engine contract — is in
service of it.
**Owner's direction (Victor):** "manual intervention needed? That
sounds VERY bad … should be folded into scrub."
**Rationale:** Keeping the offline rebuild as the *permanent* answer
was rejected even though the quantity is bounded and small — one
media fault in one 41-sector extent-map entry condemns one slot and
leaks at most `objmax`, 16 MiB at the defaults, and `-R` is about
three minutes on one instance of a replicated cluster. An operator
procedure for a condition the store can fix itself is the wrong
shape: the walk pays the same full map scan either way, so what an
online rebuild buys is not the work but the downtime, and the scrub
is the one pass that pays the I/O anyway and that gives the per-slot
validation for free by running inside the object's `Reqqueue`. The
alternatives were rejected on cost: a durable "rebuild at next
start" flag needs a new superblock field, hence a `Storevers` bump
and a reformat, and pays out as a surprise slow start months later;
a `/ctl` verb spends a change to layer-a §2.5's normative grammar,
on a file that does not exist yet; `-R` against a live store is two
allocators and two superblock publishers over one partition, which
§2.2 forbids outright. The counter earns its place on its own
merits: without it the leak is visible only through `shoalck`'s
offline cross-check, so a serving store cannot distinguish space in
use from space marked and referenced by nothing, and that number is
what says when a pass would be worth its I/O.
**Normative:** none. Nothing here reaches a wire or an on-disk
format; `/status`'s only normative field is `epoch=` (layer-a §2.2).
The one rule above is not normative by that test either — it reaches
no format — but it is not one of the choices this row leaves open:
an implementation that frees a grain a map it read named does not
implement this store, whatever else it does.
**Implementation policy:** all of it — the count and its name, that
it is memory-only and an upper bound, the offline rebuild's standing
as the not-serving store's reclaim, and the scrub-driven
shadow-bitmap mechanism with its generation stamp. A conforming
implementation may reclaim the grains some other way, or not report
them at all, within the one rule above.

## D19 — The monitor reads every map slot back; the object store does not (2026-09-15)

**Decision:** A monitor map commit reads each slot back through the
ordinary slot reader after that slot's flush and fails the commit if
the slot is not the one written; a read-back whose *read* fails is
retried once and then fails the commit with the publish declared
indeterminate (`design/store.md` §10). §3.2's log commit and §2.2's
superblock publish keep no such check.
**Rationale:** The check is for one fault — a device that reports a
successful write, acknowledges the flush after it, and does not hold
the bytes: the empty or partial case of the torn write §3.2 already
allows. It proves acceptance, not durability, and cannot see a device
that loses bytes after acknowledging a flush. It is not a documented
failure mode of 9front's sd(3) path (`platform/9front-storage.md` §5,
§6). It is guarded here and not in the object store because the
monitor's map has no replica (`design/layer-a.md` §6.5): a lost log
record is one of `R` copies and layer-a repairs it, while a
current-map slot the platter does not hold means the monitor has
acknowledged an epoch it will not serve after a restart, and layer-a
§6.3's regression rule then leaves the cluster fenced until an
operator runs `forceepoch`. Cost is ~0.8 ms against a ~17 ms publish
and is not an argument.
**Considered and rejected:** dropping the check (leaves the
acknowledgement unchecked on the only copy); the ring slot alone
(drops the worse of the two cases); the current slot alone (demotes
§8.2's retention MUST to best-effort); the header sector alone
(cannot evaluate the checksum, which is what catches a torn text);
making it conditional on `-w` (backwards — under `-w` no flush is
issued, so the read-back is the only check there is).
**Normative:** none. No on-disk field, offset or wire string changes;
layer-a §8.2 requires durable-before-ack and not ack-iff-durable, so
a monitor that publishes without the read-back still conforms.
**Implementation policy:** all of it — the read-back, the retry-once,
the indeterminate outcome and its spent `seq`, and the decision not
to extend the check to §3.2 and §2.2.

## D20 — The enumeration-snapshot bound answers `disk full` (2026-09-15)

**Decision:** An `objsnapopen` refused by §9's `objsnapmax` answers
layer-a §2.6's `disk full`, with detail naming the count and the
knob: `disk full: <n> object snapshots open, objsnapmax <max>`.
`design/store.md` §3.7's table now carries the condition and §9
states the text.
**Rationale:** The vector is space the instance must find to serve
the open, and §2.6's `disk full` entry is deliberately open-ended —
"any operation that needs space". The alternatives lose on their own
terms: a new §2.6 prefix is a wire change for a condition only an
admin listing and the store's own reconcile and reclaim walks can
reach, on an enumeration §2.2 already makes advisory and whose peer
equivalent (`op=list`) takes no snapshot; `not ready` is normative
for handoff and currency and is RETRYABLE; an internal-invariant
string is what §3.7 reserves for conditions a caller cannot produce,
which a ninth open is not; queueing the open behind the oldest close
parks a call holding no claim on the `Store`, which D16's quiesce
rule forbids being in flight at `storeclose`, so a bounded shutdown
would wait on an admin's fid. §10's map-too-big refusal settled the
same question the same way: the exhaustion prefix, with the
specifics in the detail and in `/status`, because the more specific
string was the misdirecting one.
**Normative:** that a refusal of an enumeration open for want of
room carries `disk full` and no other §2.6 prefix — the set is
prefix-free and §3.7's carve-out makes the mapping normative.
**Implementation policy:** that the bound exists at all, its default
of 8, the detail after the prefix, and reporting the open count in
`/status`. An implementation that never refuses such an open is
conforming.

## D21 — Cluster-map validation policy (2026-09-15)

**Decision:** `mapparse` validates a map text against `design/layer-a.md`
§3 and answers §2.6's `bad map`, with a detail after a colon, for every
refusal and for no other condition. Beyond the rules §3 states, it
refuses the following; each is a choice this implementation makes where
layer-a is silent, and the reasons are one apiece:

- **Every header attribute of §3.2 is required** except `retain`, which
  is 8 when absent, and `placerule`, which is `nodes`. Only `retain`'s
  default is layer-a's own: §8.2 keeps "the last `retain` (default 8)
  published maps". §3.2 prints no default for `placerule` — it defines
  `nodes`, reserves `zones` and makes a v1 monitor reject it, which
  leaves `nodes` the only value this build could choose but leaves the
  choosing to this build. §3.2 gives no other default at all, and a
  timer this build invented a value for would be a timer the operator
  did not choose.
- **An instance record requires `onnode`, `addr`, `uuid`, `status`,
  `up`**; `class` is empty when absent, `zone` is `default` (§3.3 says
  so), `weight` 100, `fenced` `no` (§3.3's "`no` otherwise"), `since` 0.
  Nothing normative here reads `class` or `since`, and the conservative
  value is the one that costs a grace rather than skips one.
- **An empty value** (`map=`, `addr=`, `class=`) is refused: §0 forbids
  white space in a value and an empty one names nothing.
- **Length caps**: 63 bytes of cluster name, 127 of `addr`, 31 of
  `class`, 63 of node and zone name (§3.3's own bound), 10 digits of
  instance index. They make every record a fixed-size struct; only the
  `addr` cap could refuse a conforming map, and a 127-byte dial string
  is already past anything 9front dials.
- **An instance index is a u32 with no leading zero.** Placement hashes
  the iid's bytes (§4.2), so `n2.01` beside `n2.1` would be one disk
  with two placement shares.
- **`replicas` over `Maxplace` (64)** is refused: §3.2 bounds R only
  below, and a fixed bound is what makes every `Cinst *p[Maxplace]` in
  this library safe. It is two orders above §4.1's 3–12 node envelope.
- **`pollms` and `mincopies` of 0** are refused as degenerate: a zero
  refresh period and a write that must land on no copies are not
  configurations, and §3.2's `leasems` > `pollms` half-says the first.
- **A duplicate known attribute inside one record**, and a second
  `map`, `node`, `instance` or `stale` record for the same subject, are
  refused; ndb would silently take the first, and §3.1's "exactly one
  `map` record" and §7.1's one mark per ordered pair say the rest.
- **A `stale` record whose subject or reporter names no instance** of
  this map is refused: §5.2 indexes the ledger on both and cannot
  evaluate a dangling mark.
- **A self-mark (`stale=X reporter=X`)** is refused: §7.1 gives a mark
  one meaning — the reporter acked a write the subject did not take —
  which X cannot say of itself, and X,X is not an ordered pair.
- **`retain` under 2** is refused, reusing the monitor store's
  `Monretainmin`: §8.2 must keep `/maps/<E−1>`, which §5.2 clause 2
  depends on.
- **A `csumalg` or `placehash` this build cannot compute** is refused:
  §3.4 requires an instance to refuse to serve a map that disagrees
  with either, and a build that cannot compute the named function must
  not place with a different one.
- **A leading indented line** is refused: §3.1's record begins at an
  unindented line, so a continuation with nothing to continue is not a
  record.
- **An indented `#`** is a continuation line whose first token is not
  `attr=value`, and is refused. §0 puts a comment's `#` "at the start
  of a line" and §3's grammar is normative, so the widening the first
  implementation made would have let two conforming implementations
  disagree about a hand-edited map. A `#` in column 0 and a line of
  nothing but white space stay transparent.
- **A byte outside 0x20–0x7e**, `\n` and `\t` excepted, is refused
  anywhere in the text (§0: 7-bit ASCII, LF-terminated, no CR), and no
  error string this library produces carries one either — an `Rerror`
  body is ERRMAX-bounded and would otherwise fail this very rule.

`zone` sharing one value across a node's instances is layer-a's own
(§3.3, validated at `commit`), not an invention, and is checked here
even though v1 placement ignores zones (D11 makes them inert for
placement, not for validation).

**Rationale:** §8.1 makes `commit` reject a map that does not validate
and lists some of what that means; the rest is left to the
implementation, and a monitor that accepts a map no consumer can
evaluate publishes an unevaluable cluster. Refusing at parse is the
only place with the whole text in hand, and `bad map` is the only
§2.6 prefix a map text may produce (`design/store.md` §3.7's mapping
rule is one-directional).

**Normative:** §3's grammar and attribute names, and that a map text
that fails validation is refused with `bad map` and no other §2.6
prefix. That an indented line is a continuation whose tokens are
`attr=value`, and a comment starts at column 0, is §0 and §3, not a
choice made here. So are three rules the list above repeats rather
than invents, each of them wire-visible: node and zone names match
`1*63(ALPHA / DIGIT / "-" / "_")` (§3.3, "Zone names share the
node-name grammar"), exactly one `map` record is present (§3.1), and
every byte of a map text is 7-bit ASCII (§0). A reimplementation that
took a 64-byte node name, a two-`map` text or a high byte would
disagree with this one about what a map is.

**Implementation policy:** every refusal in the list above except the
three the paragraph before names — which attributes are required and
what an absent one defaults to, the caps on the cluster name, `addr`,
`class` and the instance index, `Maxplace = 64`, the degenerate-value
refusals, the refusal of a duplicate attribute inside one record and
of a second `node`, `instance` or `stale` record for one subject, the
dangling-mark and self-mark refusals, and the wording of every detail
after `bad map: `. A conforming implementation may accept any of
them, or refuse more.

## D22 — Witness scoping without the E−1 map, and the fence and adoption edges (2026-09-15)

**Decision:** Four edges at the map library's boundary, settled in
`lib/map.c` — two are layer-a's own rules read precisely, two are
ours:

- **§5.2's substitution belongs to clause 2 alone.** `mapwitness`
  applies "substitute every instance with `status` ∈ {new,in,out}" to
  clause 2, which is what §5.2 attaches it to ("for this clause"), and
  to nothing else. Clause 4 and the skip rule scope on the mark's
  subject being in `P(o)` at `E`, plus `P(o)` at `E−1` when the caller
  passed the `E−1` map, and on `P(o)` at `E` alone when it did not.
  The `subst` flag does not reach them.
- **With no `E−1` map, clause 4's `E−1` half is not evaluated.** §5.2
  offers the instance two ways out of not holding that map — fetch
  `/maps/<E−1>`, which §8.2 requires the monitor to keep, or substitute
  — and the fetch is the caller's obligation; this library answers over
  the maps it is given.
- **A clock that has gone backwards fences.** `now < last` counts as
  `leasems` having elapsed: kind `lease`, until the next successful
  refresh.
- **Both §6.3 refusals are reported when both hold.** `mapadoptable`
  answers the OR of the refusals: a map from another authority at an
  epoch below the one held sets `monidmismatch=yes` and
  `epochregress=yes` both. `adoptwhy` names the flag of one bit and
  the caller renders each bit it finds set.
- **`forceepoch` is exempt from "exactly `current+1`", not from
  increasing.** `mapnextok(cur, next, 1)` requires
  `next->epoch > cur->epoch` and lifts only the `monid` check.

**Rationale:** Substituting for clause 4 makes its subject test "any
instance that is not `dead`", which is precisely the unscoped reading
§5.2 spends a paragraph ruling out: one reporter that dies with a mark
outstanding then lands in the witness set of every object and, by the
skip rule, fails every currency check in the cluster — "a second disk
failure during recovery from the first would take a cluster with every
byte present completely dark". Scoping on `P(o)` at `E` alone is the
narrower loss: §5.2's case-(ii) lemma keeps its `E` half, and the
instance that wants the `E−1` half has a documented way to get it.
A backwards clock breaks the one assumption §6.4 makes about clocks —
that elapsed time can be measured — and F1 is the only thing between a
deposed primary and the D2 violation fencing exists to prevent; one
poll interval of `not ready` against an acked write lost is not a close
call. §6.3 states its two refusals as independent MUSTs, each naming its
own flag: an instance "MUST reject a map whose `epoch` is lower than
the epoch it currently holds … and MUST report the condition in
`/status` (`epochregress=yes`)", and it "MUST refuse … any map whose
`monid` differs, reporting `monidmismatch=yes` in `/status`". Neither
sentence is conditioned on the other, and the accident §6.3 says the
tripwire exists for — "a freshly created monitor … being pointed at a
live cluster" — has both properties, so answering one code left
`epochregress=yes` unset for exactly the case the rule was written
for. Nothing in §6.3 orders the two conditions, so the order they are
tested in is free; dropping one is not. And §8.1's
exemption is worded "sets the next epoch to an arbitrary **higher**
value", §6.1 makes the epoch strictly increasing and §8.6.2 forbids
publishing an epoch a monitor cannot prove is the highest — an
exemption from the `+1` only.

**Normative:** that the substitution is clause 2's alone, and clause 4's
scoping, are layer-a's (§5.2) and a reimplementation must match them.
So is the `force` epoch relation: §6.1's strictly increasing epoch and
§8.6.2's proof obligation, of which §8.1's exemption lifts only the
`+1`. So is reporting both §6.3 conditions when both hold: its two
sentences are separate MUSTs, each naming its own `/status` flag.

**Implementation policy:** what this library does when it holds no
`E−1` map (evaluate clause 4's `E` half alone, and leave the
`/maps/<E−1>` fetch to the caller) — an implementation that fetches
inside the check, or blocks until it has the map, conforms equally,
while one that never fetches does not: §5.2's case-(ii) lemma needs
clause 4's `E−1` half, so obtaining that map is an obligation of the
caller and not an option this library's silence grants it;
the order in which the two §6.3 conditions are tested, and this
library's shape for them — one `int` of flag bits, `adoptwhy` naming
one bit at a time, the rendering of `/status` left to the server;
and the backwards-clock reading, since §6.4's assumption makes
the case undefined rather than decided. Also policy, and a known cost:
`mapprimary` and `mapunderrep` each recompute the whole placement, so a
`/status` path reporting both runs the HRW twice — measured against
nothing yet, and cheap at §4.1's envelope.

## D23 — The resulting-`csum` check, and a csum-taking variant per call (2026-09-16)

**Decision:** layer-a §5.5's receiver check — "the receiver MUST
compute its own and MUST fail with `checksum mismatch` if they
differ" — is made inside the store's commit path, at the point where
the new `csum` has been computed and no byte of the log record has
been written, and it answers §2.6's `checksum mismatch`.
`design/store.md` §3.8 states it and §3.7's table carries the row.
The library exposes it as one extra argument on a parallel entry
point per call a peer's key can reach — `objwritecsum`,
`objtrunccsum`, `objremovecsum`, `objadoptcsum`, `stagefinalcsum` —
where nil means no check and each plain call is its variant with nil.
`objcreate` has no variant: `op=create`'s receiver is a zero-length
stage that arbitrates in `stagefinal`, and a client create's key is
the instance's own to choose, so nothing names a `csum` for it.
For a multi-request `op=full` the check runs over the digests the
transfer staged and a failure discards the stage, as §3.6 says every
`final=1` outcome does. A zero-byte replicated write is the one
exception to "inside the commit path": it commits nothing and adopts
no key (layer-a §2.4 makes a count of 0 not an extend), and the check
still runs, against the `csum` the object already carries.
**Rationale:** The check's whole value is that it is a bar rather
than a report. Made after `logcommit` it would refuse the operation
and leave its record on the platter, so the next start would replay
into precisely the divergent state layer-a §5.5 says the check exists
to prevent — and the caller, holding an error, would have no way to
know. The one place where the resulting `csum` exists and nothing is
durable is inside the commit, which is also the only place that sees
the `csum` of an `op=full`'s staged digests and of a tombstone alike,
so one check covers six operations. Separate entry points were
chosen over widening the existing ones because those have hundreds of
call sites and the argument is meaningful on none of them: a client
write has no sender to check against. They were chosen over a
store-wide "expected csum" set before the call because §7 runs many
committing procs over one `Store` and such a value would belong to
none of them; and over returning the computed `csum` for the caller
to compare because by the time the caller could compare, the record
is durable, which is the failure this row exists to rule out.
**Normative:** that the check is made, and that it answers
`checksum mismatch` — the spelling is layer-a §2.6's and §3.7's
carve-out makes the mapping normative. A receiver that answers
something else does not conform.
**Implementation policy:** *when* the check is made. layer-a §5.5
fixes the check and the string and says nothing about the moment, so
a receiver that commits and then reports conforms to §5.5 as written
— badly, for the reason above, which is why this store makes the
check before the record and why `design/store.md` §14(16) proposes
that §5.5 require it. Also the API shape — a parallel call per
operation, nil for no check, the plain call defined as the variant
with nil — and the detail after the prefix. An implementation that
passes the expected `csum` on one widened signature, or that carries
it in a per-operation handle, conforms equally.

## D24 — The rebuild engine enforces the walk's coverage, bounds a fold's re-reads, and moves the free count by the swap's own difference (2026-09-16)

**Decision:** The four calls' contract under D18's mechanism,
settled in `lib/store.c` and `lib/alloc.c`:

- **The free count moves by the swap's own difference, not by a
  recount.** Each installed page moves `grainfree` by the clear bits
  it gained or lost; the whole-bitmap recount §5 step 11's offline
  rebuild ends with is not taken here. A staged grain is clear in
  both copies (§6), so it needs no term of its own either way.
- **A fold validates by the stamp and re-reads a bounded number of
  times.** Past that bound it takes the grains from the pinned entry
  under `qlstate` itself — a memory read under the lock the apply
  mutates the map under, not a device read — and a slot whose
  extent-map slot has moved under it goes round again with a fresh
  pin, under a second and larger ceiling past which the fold refuses
  with an error the caller retries.
- **The stamp outlives the slot.** `applyslot` bumps it rather than
  zeroing it, and a condemnation bumps it although no map changed,
  because what it records is "what this slot says has moved" and not
  "the four-tuple has".
- **`storeclose` aborts a live pass.** The contract is still that a
  caller ends or aborts one first (D16), and a fold parked at §13's
  hold point is woken by the drop rather than left asleep in a pass
  that no longer exists.
- **The engine enforces the walk's coverage rather than trusting the
  driver.** A mark per index slot, set by the fold that completes a
  slot and by an apply whose record rebuilds that slot's map whole,
  and an end that refuses while a `live` slot is unmarked or a fold
  is in flight. A refusal installs nothing and leaves the pass live.
- **An abort under an in-flight swap is a no-op.**  A `bmswapping`
  flag under `qlstate` says so; abort stays a call that cannot fail,
  and a `storeclose` under an in-flight end is D16-undefined like any
  other call in flight rather than a defined half-swap.
- **The swap leaves standing what it did not reclaim.** A leak
  recorded in a slot the pass had already folded outlives the swap,
  because the fold put those grains in the shadow; the end discharges
  the rest of `grainleak` and keeps that much.

**Rationale:** The recount is the only one of these with a cost
argument behind it: at `ngrains` on a 4 TB disk it is ~2.6*10^8 bit
tests under `qlstate`, which is the hold that chunking the swap
exists to avoid — §7 rule 2's `/status`, `/ctl` and `Tflush` are
behind exactly that lock — so taking it at the end would give back
what the page-by-page swap bought. The difference is exact rather
than approximate: the barrier keeps both copies current, so a page's
two counts are taken under one hold of the lock every mutation of
either copy is made under. The re-read bound is a liveness
obligation, not a correctness one: without it one object under a
continuous write rate starves the walk on that slot for ever, and the
fallback is safe because a pinned entry's bytes are readable under
`qlstate` by construction (§7); the second ceiling is what a round
the fallback itself cannot answer — the slot naming a different map —
would otherwise loop without. The stamp's survival across a release
is the case that would otherwise be silently wrong: a slot freed and
re-created under a walk is the freshest possible entry, and zeroing
the stamp would make it look unchanged to precisely the reader the
stamp exists for. And a pass that a close left armed would be a
barrier writing into freed memory on the next commit, which is why
the close drops it rather than trusting the caller. The coverage
interlock is the one rule here that guards against losing data rather
than against a wrong number: everything else a broken walk can do
costs a count, while a shadow the walk did not finish frees grains a
live map still names — D18's one rule, broken — so the engine refuses
rather than leaving that obligation with a driver that is not built
yet. The leak the swap keeps follows from the same mark: the count
means "marked and named by nothing", and a swap that installed such
grains has not stopped them being that. The abort rule is the same
argument once more: the gap between two pages is a real window, and
the only two things that can be in it are an abort, which the end
makes redundant, and a close, which D16 already leaves undefined.
**Normative:** none. Nothing here reaches a wire or an on-disk
format; the stamp is memory only and no format field carries it.
**Implementation policy:** all of it. A conforming implementation may
recount, may re-read without a bound or a ceiling or run the fold
inside the object's queue instead, may leave the walk's coverage to
its driver and refuse nothing, may discharge the whole leak count at
the swap or recompute it, may refuse an abort under a swap or leave
it undefined rather than ignoring it, may refuse a close under a live
pass rather than aborting it, and may reclaim a condemned slot's
grains some other way entirely — all within D18's one rule.

## D25 — `fence off` is refused only under a lease fence (2026-09-17)

**Decision:** `design/layer-a.md` §2.5 scopes the `fence off` refusal
to a lease-derived fence, and this row is the argument for that
amendment; the rule itself is read there, not here.
`design/store.md` §14(26) records the amendment.
**Rationale:** §2.5 listed `fence off` among the verbs that MUST fail
`fenced` while the instance is fenced, and §6.4 F4 makes `fence off`
the only thing that clears an operator fence — "a separate flag with
the same effect as F1's; `fence off` clears only that flag". Read
literally the two made an operator fence permanent: the operator set
it, and the verb that would clear it was thereafter refused because
it was in force, so the instance served nothing until it was
restarted. That cannot have been what either sentence meant, since F4
exists to be used and §6.4 describes it as a flag an operator sets
and clears. What §2.5's rule protects is named in its own paragraph:
"a deposed instance could be driven to overwrite, delete, discard
replication state, or unfence itself" — and the fence a deposed
instance carries is F1's lease fence, which `fence off` MUST NOT
clear anyway. Scoping the refusal to the lease fence keeps every word
of that protection and costs nothing: an instance that has lost its
map still cannot unfence itself, and one an operator fenced can still
be unfenced by the operator.
**Considered and rejected:** refusing `fence off` under any fence,
which is the literal reading and makes F4 one-way — an operator
fence would then be cleared only by restarting the instance, and
§6.4's "separate flag" would be a one-shot kill switch. Also
rejected: dropping `fence off` from the fenced set entirely, which
would let a lease-fenced instance answer it `ok` while clearing
nothing, telling the operator the fence is gone when it is not.
**Normative:** the amended §2.5 sentence and §6.4 F4, which carry the
rule; a reimplementation matches those, not this row.
**Implementation policy:** what a `fence off` refused under a `both`
fence does to the operator half. A lease fence and an operator fence
can be in force at once, and such a `fence off` owes §2.5's refusal
and F4's clearing at the same time; this server refuses and changes
nothing, so the operator flag is still set when the lease returns and
the operator's next `fence off` is what clears it. An implementation
that clears the operator half and answers `fenced` all the same
conforms too: the fence still in force is the lease one either way,
and neither reading lets a deposed instance unfence itself.

## D26 — A whole-store pass is a background job, and the tombstone reclaim is one of them (2026-09-18)

**Decision:** Every `ctl` verb whose work walks the whole store runs
in a proc of its own that holds one of the server's background jobs,
answers success once the job is accepted, and is read from `/jobs`.
That is `scrub` (`design/layer-a.md` §2.5, §7.5) and also `forget`,
which §2.5 does not describe as background work. `design/store.md`
§9's tombstone reclaim is a third such pass, on a timer of its own
and under a new `reclaim [start|stop]` verb — **a wire change**, since
§2.5 fixes the ctl grammar (Victor, 2026-09-18, amending this row's
first form, which had the walk ride on the end of a `scrub` pass to
avoid changing that grammar). It **counts without
discarding**: `design/layer-a.md` §1.5 licenses a discard only when
all three of its conditions hold, the walk tests the two local ones
(retention and epoch supersession) and reports the count at `/jobs`,
and the third — full confirmation from every non-`dead` instance —
has nothing to answer it while this build has no peer client.
§14(30), §14(31) and §14(39) record all three.
**Rationale:** lib9p's service loop is single-threaded, and layer-a
§5.4.1 requires a `Tflush` to be answerable while anything else is in
flight; a verb that made durable commits on that loop would park it
for as long as the walk took. The queue pool is the other place work
can go, and it is addressed by oid — a queued `ctl` row's `argv[0]`
is the oid the framework hashes — so a verb naming an instance id or
naming nothing has no queue to go to. A proc holding a job is what is
left, and the shutdown already waits for those after it drains the
requests, because `design/store.md` §9 forbids closing the store
while anything is inside the engine and the drain cannot see a proc
that is not a request.

For the reclaim walk the first form of this row said the opposite: no
verb, because §2.5 fixes the ctl grammar and a `reclaim` verb would be
a wire change for a walk no protocol consumer drives — the same
argument §13 makes for `-X` being a command-line flag — and §8 already
makes the scrub pass the walk other whole-index work rides on. What
that costs is the scrub's own timescale: layer-a §7.5 sizes a full
pass at "a default near 14 days", the reclaim walk is an index walk
measured in seconds, and tying the two means a mass delete's space is
reported a fortnight late, a `scrub stop` forfeits the reclaim with
the pass that carried it, and no tier below a full 14-day pass can
exercise the walk at all. The walk's cost has nothing to do with the
scrub's, so neither should its schedule. Victor authorised the
grammar change on 2026-09-18: the walk is its own job on its own
timer, and `reclaim [start|stop]` is a new admin verb in §2.5, fenced
because the walk will discard (§14(39)). The scrub's own "continuously"
(§7.5) is a separate matter, settled separately: it has a timer of its
own on a period of `scrubdays` (§8), and the two `stop`s mean the same
thing — stop the pass that is running, leave the schedule alone.
**Considered and rejected:** answering `forget` synchronously, which
is the plain reading of §2.5 and costs the loop one durable commit
per record, unbounded by anything but the disk's dirty region; and
keeping the walk on the scrub's back, which is what the paragraph
above weighs — the grammar stays fixed, and the operator waits a
scrub for a count that takes seconds to make.
**Normative:** §2.5's new `reclaim` row — its spelling, its `admin`
role, its two optional words, and its place in the fenced set, which
is per form: `reclaim start` is in that set and `reclaim stop` is
not, because `stop` mutates nothing and an instance just fenced is
where an operator most wants a running walk stopped. All of it is a
ctl grammar a client writes, so a reimplementation must match it.
The row also carries a **SHOULD**, which is normative as a
recommendation: an instance that discards at all should run the walk
on a schedule of its own rather than wait to be asked, since the verb
is there to ask for a pass early and not to be the only thing that
runs one. What the schedule's period is remains implementation
policy — `tombdays`/2 is this server's, because it is §8.3's own
cadence for the `bump` that condition 3 waits on (§14(39)) — and an
implementation whose operators run the verb on a clock of their own
answers the SHOULD's purpose too. §2.5's rule that a verb starting
background work returns on acceptance is unchanged and governs it.
§1.5's three discard conditions are normative and are layer-a's, not
this row's — what this row settles is that a walk which can test only
two of the three discards nothing.

*Amended 2026-09-19*, the scrub having gained a schedule of its own
(`design/store.md` §8): **what `stop` means is normative, and it means
the same for both verbs.** `scrub stop` and `reclaim stop` stop the
pass that is running and hold no schedule back — the next tick of that
verb's timer starts a pass exactly as a `start` would, and `start`
asks for a pass now without moving the timer. A client writes those
two words and acts on the answer, so a reimplementation must match the
reading; and §2.5's form, which has no word for a schedule, leaves an
operator no way to spell the other intent, which is what settles it
this way rather than the other (§14(39) has the argument and what it
costs). Until the scrub had a timer the two `stop`s agreed by
accident, this row's first form having built one schedule and not the
other.
**Implementation policy:** the rest — which verbs are passes, each
walk's timer and its period (§14(39)), the scrub's rate default and
the bytes it charges itself (§14(31)), `/jobs`'s line format, the
`reclaimable=` count it reports, and that a second `scrub start` or
`reclaim start` while a pass runs starts nothing. The same amendment
adds the scrub schedule's own policy: that there is a timer at all,
its period of `scrubdays` — 14 days by default, `shoalsrv -d`
otherwise — that the first pass comes one period after start-up
rather than at it, and that a tick landing on a running pass starts
nothing rather than being caught up on, since the pass still walking
is the continuity `design/layer-a.md` §7.5 asks for. §7.5 calls the
pace implementation policy in as many words, and `/status`'s
`scrubnext=` (§14(23)) is this server's way of reporting a schedule
§2.2 names no field for. An implementation that answers `forget`
synchronously, or that runs either walk on a period of its own
choosing, conforms; one that discards nothing —
§1.5 makes the discard an optimisation and not a requirement — still
owes the verb its answer, and answers it with a walk that counts.
