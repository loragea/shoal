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
| `docs/design/store.md` | The per-instance local object store and the monitor's map store: on-disk format, write/read paths, recovery, space, concurrency and group commit, tooling, and the store's test plan. On-disk truth lives here. |
| `docs/platform/9front-storage.md` | Verified 9front storage facts: file-server durability, the sd(3) raw path and flush behaviour, measured costs. |

`docs/platform/` holds facts about the target platform rather than
about shoal — each such doc cites 9front sources by path and line,
naming the release those line numbers were read from.

## Building

shoal is written in the Plan 9 C dialect and built on 9front with
`6c`/`6l` under `mk` (decisions.md D12). From the repo root:

- `mk` — build `lib/libshoal.a$O`, then `srv/libshoalsrv.a$O`, then
  every command in `cmd/`, then the T1 test programs in `test/`.
- `mk test` — build everything, then run T1 (below).
- `mk clean`, `mk nuke` — remove build products in every
  subdirectory.

| Path | Holds |
|---|---|
| `mkfile` | Root. Iterates `lib srv cmd test` for `all`, `clean` and `nuke`, and runs T1 for `test`. |
| `lib/` | `libshoal.a$O`: code shared by servers, commands and tests — the store engine, the monitor's map slot store, the cluster map, and the 9P client (`lib/ninep.c`, `docs/design/store.md` §12), which is the transport every outbound connection is made through; nothing in this build dials it (§14(18)). `lib/shoal.h` is its header; includers name it by relative path after `<u.h>`, `<libc.h>`, `<libsec.h>` and `<fcall.h>` — the last for the GBIT/PBIT macros every on-disk integer is packed with and for the 9P types the client's declarations name. `lib/store.h` is private to `lib/`: it holds the store engine's own structures, which are opaque to everything else. Built by `/sys/src/cmd/mklib`. libshoal depends on neither lib9p nor libthread and must not come to: the same code runs under a plain-libc T1 program and under the libthread 9P server (`docs/design/store.md` §7). |
| `srv/` | `libshoalsrv.a$O`: the storage instance's 9P service (`docs/design/layer-a.md` §2) — attach, the file tree, the `Reqqueue` pool, `Tflush`, the ctl framework, start-up and shutdown. `srv/srv.h` is its header, included after `<thread.h>`, `<9p.h>` and `lib/shoal.h`; `srv/dat.h` and `srv/fns.h` are private to `srv/`. It is a library for the same reason `lib/` is: a T1 test links libraries and execs nothing, and §2 is what T1 has to drive. Built by `/sys/src/cmd/mklib`. |
| `cmd/` | One directory per command, each built by `/sys/src/cmd/mkone` — so it produces `$O.out` and installs as `$TARG` in `/$objtype/bin`. `cmd/mkfile` lists them in `DIRS`. |
| `test/` | T1 test programs. |

Every mkfile starts with `</$objtype/mkfile`. A new command is a
directory under `cmd/` with an `mkone` mkfile plus its name in
`cmd/mkfile`'s `DIRS`; a new library source file is a name in
`lib/mkfile`'s or `srv/mkfile`'s `OFILES`.

## Test tiers

**T1 — unit.** `mk test` at the repo root. Runs on any single
9front machine: no disks, no network, no second node, seconds to
run. Each test is a C program in `test/` linking `libshoal`, and — if
it drives the 9P surface — `libshoalsrv`, lib9p and libthread as
well; it exits non-zero if any check failed and prints one
`FAIL: <reason>` line per failed check to standard error. `mk test`
fails on the first failing program. To add one: write `test/foo.c`
and add `foo` to `TESTS` in `test/mkfile`. Known-answer vectors are
computed outside this codebase, and the test source says how they
were computed.

A test that drives the 9P surface runs a whole storage instance
inside itself: `test/srv9p.h` is an in-process 9P client that puts
the server on one end of a pair of pipes and speaks raw 9P
(`convS2M`/`convM2S`) on the other, over the simulated disk. It is
what lets a case pipeline tags, flush any tag, propose an `msize`
below the attach floor and assert exact `Rerror` strings. Such a
program is a libthread program — `threadmain`, its own
`mainstacksize` — because the queue pool is `9pqueue`(2)'s, and it
arms `srv9p.h`'s watchdog proc so that a wedged server fails the
test instead of hanging `mk test`.

One program drives that same instance through `libshoal`'s own 9P
client (`lib/ninep.c`) instead of through `srv9p.h`, because the
client is what it is testing: `test/clienttest.c`. It is a libthread
program for the same reason and carries a watchdog of its own.

T1 needs no partition because every device access in the store goes
through one vtable (`docs/design/store.md` §0). Two of its three
implementations are what T1 drives: a **simulated disk**, which is a
test program's own memory and models a volatile write cache, torn and
partial writes, short counts, the error classes, and a crash at a
named point — optionally stopping the device, as a crash does — with
every operation recorded in issue order; and a **file-backed
device**, which is how a test drives `shoalfmt` and `shoalck` over an
image. A test that wants the second creates and removes its own file
under `/tmp`; nothing else in T1 touches the file system.

The store engine takes a `spawn` callback rather than making procs
itself (`docs/design/store.md` §7), so a T1 program that drives it
passes an `rfork(RFPROC|RFMEM)` wrapper and the engine's own procs —
and the concurrent committers a test spawns — are ordinary procs
sharing the program's memory. `test/t1.h` holds that wrapper and the
small geometry the store tests format.

**T2 — single-node integration** and **T3 — multi-node grid** are
not yet defined. `docs/design/store.md` §13 proposes T2's shape for
the local store, which is a proposal and not a ratified tier.
