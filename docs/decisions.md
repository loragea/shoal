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
