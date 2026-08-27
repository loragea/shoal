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
server side uses stock `lib9p` (9p(2)) with `srv` in multi-proc mode
and an implemented `Srv.flush`, as `design/layer-a.md` §5.4.1
requires. Hashing uses libsec's BLAKE2s (D7).
**Rationale:** Native C is the only toolchain 9front ships and
maintains, so it is the only one whose breakage is anyone's problem
but ours. `lib9p` is what every 9front file server is written
against; its idioms — `Srv`, `srv`, `respond`, per-request procs —
are the ones a 9front reviewer expects, and §5.4.1's concurrency and
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
