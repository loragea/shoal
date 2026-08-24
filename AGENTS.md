# AGENTS.md — shoal

shoal is a Plan 9-native distributed storage system: horizontal
scaling across nodes and disks with replication, striping, and a
mountable live filesystem. Target platform: 9front. Nothing is
implemented yet; see `docs/target.md` for the ratified target.

## Ground rules

- This repo stands alone. Never reference private workspaces,
  orchestration tooling, or machines outside this repo's docs.
- **One home per fact** — the docs table below decides where a fact
  lives; link, never restate.
- **Docs state what IS, never the plan.** The one exception is
  `docs/target.md`, which is explicitly the ratified target contract
  and says so. When infrastructure lands ahead of behavior, docs
  describe current behavior and mark the open half.
- At wire and protocol boundaries (9P dialects, ctl-file grammars,
  map/on-disk formats) think in MAY/SHOULD/MUST. Every decisions.md
  row marks which parts are **normative** (a reimplementation must
  match) vs **implementation policy** (a conforming implementation
  may differ).
- The git log is the record of what's done — no changelogs in docs;
  rationale goes in `docs/decisions.md`.
- **A discriminating test is verified both ways or not at all**: the
  fix passes AND the reverted/mutated code fails, each actually run.
- Commit subjects: imperative mood, capitalised, ≤50 chars, no
  trailing period; body (when needed) wraps at 72 and explains what
  and why.

## Docs table

| Doc | Holds |
|---|---|
| `docs/target.md` | The ratified deliverable target: layers, semantics contract, milestones, design envelope. |
| `docs/decisions.md` | Design decisions with rationale; each row marked normative vs implementation policy. |
| `docs/design/layer-a.md` | The ratified Layer A contract: object model, storage-server 9P export, cluster map, placement, write/read path, epoch/fencing, heal, monitor. Wire truth lives here. |

## Test tiers

TBD — defined when the first code lands. Expected shape: unit
(host-side, no VMs), single-node integration, multi-node grid.
