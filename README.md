# shoal

A Plan 9-native distributed storage system, written for 9front.

The target: any client mounts a live read-write filesystem that
stripes data across N storage nodes with R-way replication, survives
the loss of any single node or disk, holds files larger than any one
disk, and supports delete with space reclaim. Coherence comes from
cacheless clients and server-side serialization, not from 9P
extensions. The full contract is `docs/target.md`.

The name is the fish sense: many small bodies moving as one, no
leader.

## Status: early development, not usable

**There is nothing to mount yet, and there will not be for some
time.** What exists is the per-disk local object store (the engine
under milestone M1) and its offline tools, exercised by a unit-test
tier on simulated disks. The 9P server that exports a disk's objects
is the next piece of work; placement, replication, striping and the
namespace server come after it.

- On-disk formats change between commits with no migration path.
- Nothing has run on a physical disk under load.
- Do not point any tool here at a partition you care about.

Milestones, in order: M1 single-node object server; M2 cluster map,
monitor, replication and heal; M3 striping; M4 the metadata server
and a mountable tree. `docs/target.md` § Milestones has the demos
each one gates on.

## Layout

| Path | Holds |
|---|---|
| `docs/target.md` | The ratified target contract and milestones. |
| `docs/decisions.md` | Every design decision with its rationale, each marked normative vs implementation policy. |
| `docs/design/layer-a.md` | The distributed object layer: object model, 9P export, cluster map, placement, write/read paths, fencing, heal. |
| `docs/design/store.md` | The per-disk local store: on-disk format, recovery, space, concurrency, tools, test plan. |
| `docs/platform/9front-storage.md` | Verified 9front storage facts the design rests on. |
| `lib/` | `libshoal`: the store engine and everything shared. |
| `cmd/` | Offline tools: `shoalfmt`, `shoalmonfmt`, `shoalck`, `shoalcsum`. |
| `test/` | The T1 unit-test programs. |

`AGENTS.md` holds the working conventions for anyone changing the
repo, including the build and test tiers.

## Building

Plan 9 C, built on 9front with `mk` from the repo root: `mk` builds
the library, the commands and the tests; `mk test` runs the T1
tier, which needs no disk, no network and no second machine.

## AI disclosure

Nearly all of the code and prose in this repository was written by
AI coding agents: Anthropic's Claude models, run through Claude Code.
Under the three-level vocabulary several projects now use
(human-written / AI-assisted / AI-generated), this repository is
**AI-generated**.

What the human does: sets the target, answers every product and
design question, and ratifies each decision recorded in
`docs/decisions.md`. The maintainer is accountable for everything
here. What the human does not do: review every line. Line-level
review is itself performed by further AI agents in adversarial
rounds, backed by the rule that a test counts only when it has been
watched failing against the mutated code. Commits name the model in
a `Co-Authored-By` trailer.

Read the design docs as a serious attempt that has been argued over
at length, and the code as unproven until the integration tiers
exist. If you contribute, say in the pull request whether and how AI
was involved.

## Licence

MIT. See `LICENSE`.
