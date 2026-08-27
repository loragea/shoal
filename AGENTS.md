# AGENTS.md — shoal

shoal is a Plan 9-native distributed storage system: horizontal
scaling across nodes and disks with replication, striping, and a
mountable live filesystem. Target platform: 9front. See
`docs/target.md` for the ratified target and `docs/decisions.md` for
the decisions it rests on.

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
| `docs/platform/9front-storage.md` | Verified 9front storage facts: file-server durability, the sd(3) raw path and flush behaviour, measured costs. |

`docs/platform/` holds facts about the target platform rather than
about shoal — each such doc cites 9front sources by path and line,
naming the release those line numbers were read from.

## Building

shoal is written in the Plan 9 C dialect and built on 9front with
`6c`/`6l` under `mk` (decisions.md D12). From the repo root:

- `mk` — build `lib/libshoal.a$O`, then every command in `cmd/`,
  then the T1 test programs in `test/`.
- `mk test` — build everything, then run T1 (below).
- `mk clean`, `mk nuke` — remove build products in every
  subdirectory.

| Path | Holds |
|---|---|
| `mkfile` | Root. Iterates `lib cmd test` for `all`, `clean` and `nuke`, and runs T1 for `test`. |
| `lib/` | `libshoal.a$O`: code shared by servers, commands and tests. `lib/shoal.h` is its header; includers name it by relative path after `<u.h>`, `<libc.h>` and `<libsec.h>`. Built by `/sys/src/cmd/mklib`. |
| `cmd/` | One directory per command, each built by `/sys/src/cmd/mkone` — so it produces `$O.out` and installs as `$TARG` in `/$objtype/bin`. `cmd/mkfile` lists them in `DIRS`. |
| `test/` | T1 test programs. |

Every mkfile starts with `</$objtype/mkfile`. A new command is a
directory under `cmd/` with an `mkone` mkfile plus its name in
`cmd/mkfile`'s `DIRS`; a new library source file is a name in
`lib/mkfile`'s `OFILES`.

## Test tiers

**T1 — unit.** `mk test` at the repo root. Runs on any single
9front machine: no disks, no network, no second node, seconds to
run. Each test is a C program in `test/` linking `libshoal`; it
exits non-zero if any check failed and prints one
`FAIL: <reason>` line per failed check to standard error. `mk test`
fails on the first failing program. To add one: write `test/foo.c`
and add `foo` to `TESTS` in `test/mkfile`. Known-answer vectors are
computed outside this codebase, and the test source says how they
were computed.

**T2 — single-node integration** and **T3 — multi-node grid** are
not yet defined.
