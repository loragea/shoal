# shoal — target contract

**Status: ratified target, 2026-08-23.** This doc is the contract
the design rounds and milestones build toward; `decisions.md` holds the rationale and the
normative/implementation-policy split for each decision.

## Target, in one sentence

Any client mounts a live read-write filesystem that stripes data
across N storage nodes with R-way replication, survives loss of any
single storage node or disk, holds files larger than any single
disk, and supports delete with space reclaim.

## Non-goals (v1)

- Client-side caching, leases, callbacks, or any 9P protocol
  extension — coherence comes from cacheless clients and server-side
  serialization; extensions are considered only after measured need.
- Dynamic tiering, cache tiers, or data migration between device
  classes (static class-filtered placement may come later; the map
  format reserves a per-disk `class` tag from day one).
- Metadata-server failover (the MDS is restartable anywhere because
  its state lives in the object layer; live failover comes later).
- Content addressing as the foundation (a per-tree immutable/
  archival mode is a planned later milestone, not the core).
- WAN operation. Design envelope: LAN, sub-millisecond RTT,
  ≤ ~12 nodes.

## Architecture: three layers

**Layer A — distributed object store.** A flat space of mutable,
fixed-max-size objects. Each *disk* runs its own object-server
instance: a 9P server exporting that disk's objects plus a ctl
surface for replication/heal operations. Placement is a
deterministic function over a versioned cluster map (two-level
hierarchy: node → disk; replicas always on distinct nodes) served by
a monitor; every client operation carries the map epoch so stale
placement is detected during rebalance. Writes go to the object's
primary, which orders them and replicates to the other copies before
acknowledging. Node or disk loss → map epoch bump → new primaries →
background heal by whole-object resync. Objects carry checksums;
heal verifies against them. Delete is object removal at all
replicas.

**Layer B — striping.** A file is data striped over
deterministically named objects (`fileid.0, fileid.1, …`). Given a
file id and stripe parameters, any party *computes* where every byte
lives — no per-chunk metadata, no layout lookup. This is what makes
a file larger than any single disk, and what keeps the future direct
client→storage data path purely additive.

**Layer C — namespace.** A metadata server (MDS): a normal 9P file
server holding the directory tree, its own state stored as Layer A
objects. All namespace operations serialize through it; file data in
v1 also flows through it (accepted bottleneck — see decisions.md).
Because its state is in the object store, the MDS can be restarted
on any node.

## Semantics contract

- Writes to a single object are linearizable (primary-ordered).
- Writes spanning stripe/object boundaries are not atomic.
- Clients do not cache; every operation reaches a server, so
  concurrent writers on one live tree are coherent by construction.
- Delete and space reclaim are core operations.
- A per-tree immutable/archival flag (later milestone) provides
  write-once semantics where wanted; no-delete is never global.

## Milestones

- **M1** — single-node object server: object + ctl grammar live on
  one disk.
- **M2** — placement function, cluster map + monitor, client
  library; N-node put/get with replication; demo: kill a node (and,
  separately, a disk) and heal.
- **M3** — striping; demo: store and read back a file larger than
  any single disk in the cluster.
- **M4** — MDS; mount the live tree from every node; demo:
  concurrent writers from two nodes.
- **M5+** (each gated on measurement or demonstrated need): direct
  client→storage data path (adds a size/attr commit operation),
  client caching + leases, MDS failover, per-tree archival/
  content-addressed mode, snapshots, static class-based placement.
