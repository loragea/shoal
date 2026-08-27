# 9front storage: durability, the sd(3) raw path, and what it costs

What the target platform gives a program that must not lose an
acknowledged write. A reference for whoever implements shoal's local
storage; the decision it feeds is `../decisions.md` D13.

Citations are `/sys/src/...:line` as of **9front release 11952**;
every line number below was read out of that release's tree.
Measurements were taken on a two-vCPU KVM guest with virtio-blk
disks running it — §6 says what that does and does not establish.

## 1. There is no fsync

9P has no flush-my-file operation and 9front supplies no
replacement. An `Rwrite` from a file server means the server has the
bytes, not the disk.

The one primitive that comes close is the **null `Twstat`** — the
9P "change nothing" wstat, every field at its don't-touch value. It
is a per-file request a file server is free to treat as a commit
point; of the three file servers 9front ships, only gefs does (§4).
On cwfs and hjfs it is a no-op (§2, §3), whose only durability verb
is a console `sync` that is a whole-file-system barrier.

## 2. cwfs

`/sys/src/cmd/cwfs`, config `cwfs64x`. The default root file
system of a 9front install.

- **A `Twrite` copies the payload into an in-memory `Iobuf`, marks
  it `Bmod`, and returns.** `putbuf` writes a block out only when
  `Bimm` is also set, and the write path never sets it.
  `9p2.c:1284-1286`, `iobuf.c:184-197`.
- Dirty blocks reach the fscache partition when the `synccopy`
  background proc gets to them — **one block per hash line per
  second**, sleeping 10 s when it finds nothing — or when the
  buffer is evicted as an LRU victim, or on a console `sync`.
  `main.c:570-591`, started at `main.c:377`; eviction at
  `iobuf.c:84-93`. The worm sees the data only at dump
  (`main.c:527-568`).
- **`sync` and `halt` are the only durability verbs.** `sync` takes
  the global `mainlock` for writing and loops `syncblock()` until a
  pass finds nothing dirty; each pass writes at most one dirty
  block per hash line. It is synchronous down to `pwrite` on
  `/dev/sdXX/data`. `halt` is `sync` plus exit. `con.c:346-352`,
  `iobuf.c:125-181`, `sub.c:1037-1051`, `cw.c:511-513`,
  `wren.c:103-118`, `con.c:119-125`; the installed command set is
  `con.c:778-812`.
- **`sync`'s cost is set by the whole file system's dirty set**, so
  no single writer can bound it: 60–120 ms with a clean cache,
  ~145 ms after one 4 KiB file, 2.96 s after 4 MB dirty, **25.3 s
  after 40 MB dirty**. Write-a-file-then-`sync` sustains ~6 durable
  file writes/s.
- **A null `Twstat` is a no-op**: it sets `tsync=1`, whose only
  effect is to skip the atime update. `9p2.c:1546`,
  `9p2.c:1790-1791`; measured, it adds one round trip (~34 µs) and
  no I/O.
- **Metadata and data are not ordered against each other.** Free
  list, superblock and several dentry updates are `Bmod|Bimm`, i.e.
  written inside `putbuf`; file data is `Bmod` only.
  `sub.c:401,533,574,716,738`, `dentry.c:77,96,207,315,339`,
  `cw.c:1138,1248,1515,1526,1652,1673`, `9p2.c:900`. §7 is the
  consequence.
- Block size of `cwfs64x` is 16 KiB and the buffer cache defaults
  to 25 % of free user memory (`fsmempercent`) — that is how much
  acknowledged data can be sitting in RAM. `cwfs64x/dat.h:9-10`,
  `malloc.c:97-118`.
- **A hard stop leaves cwfs structurally damaged, and it does not
  notice.** Measured after power-off with writes in flight: 34 and
  59 acknowledged records lost in two runs; on reboot cwfs prints
  its usual banner, no recovery, no complaint, and serves files.
  Meanwhile `check` reports dentries pointing at block numbers
  outside the file system (`range 57506`), `ls` and `walk`
  disagree about which files exist, reads of some files return
  `phase error -- cannot happen` while their `stat` looks fine,
  `du` fails the same way, and `rm -rf` of the damaged directory
  silently does nothing — the directory cannot be removed.

## 3. hjfs

`/sys/src/cmd/hjfs`. Same shape as cwfs, smaller.

- Writes go to delayed-write (`BDELWRI`) buffers; a background proc
  calls `sync(0)` every 10 s. Block size 4 KiB, default cache 1000
  buffers (~4 MB). `fs2.c:153,178,277,387,654,766`,
  `main.c:80-85,109`, `dat.h:3,16`.
- The console `sync` calls `sync(1)`: it queues every delayed-write
  buffer, pushes a barrier request through each device's worker
  queue and waits, so it returns after the `pwrite`s complete. It
  is global, not per-file. `cons.c:67-71`, `buf.c:172-188,296-320`,
  `dev.c:10-60`.
- The barrier is a **queue barrier only** — hjfs issues no
  device-level flush anywhere. `dev.c:27-40`.
- **A null `Twstat` is not special-cased**; `chanwstat` applies
  only the non-`~0` fields. `9p.c:253-255`, `fs2.c:679-760`.
- `sync` cost, again unbounded by the caller: 0.5 ms on a clean fs,
  ~48 ms after one file, 241 ms after 4 MB, **3.3 s after 40 MB**.
  Write-then-`sync` sustains ~14 durable file writes/s. Ordinary
  4 KiB writes have a 9.9 s tail when the 4 MB buffer pool fills.
- **A hard stop loses acknowledged writes and breaks the file
  system**: 183 acknowledged records lost, and on restart hjfs
  enters a permanent
  `type mismatch, dev ..., got raw, want dentry` loop (~900 KB of
  it in a few minutes) and cannot list the damaged directory.

## 4. gefs

`/sys/src/cmd/gefs`, shipped in 11952 as `/bin/gefs`. Copy-on-write,
committing at explicit points; a crash rolls back to the last
commit.

- **A null `Twstat` (`nulldir`, no rename) is turned into an
  `AOsync`, and the `Rwstat` is withheld until that sync has
  completed.** This is the only fsync-equivalent on 9front.
  `fs.c:1585,1626-1736` (the `if(nulldir && rename == 0)` at
  `fs.c:1736` sets `op = AOsync` and attaches the pending message),
  `fs.c:2959-3016` (`case AOsync`), `fs.c:3147-3161` (the withheld
  `respond`).
- The same sync is reachable by writing `sync` to gefs's `ctl`
  file, which lives in the **adm** root, not in a mounted snapshot
  — `mount /srv/gefs /n/x` gives no `ctl`. `ctl.c:241-275`,
  `fs.c:2440-2441,2705-2707`, `ream.c:28`. The gefs *console*
  (`/srv/<name>.cmd`) has no sync command at all: `cons.c:112-160`.
- A background task syncs every 5 s regardless (`fs.c:3206-3224`),
  and an `AOsync` on a clean file system short-circuits
  (`fs.c:2966`), which is why concurrent syncs amortise.
- **It works and it is slow: 530–620 ms per durable write**,
  amortising to ~7.7 durable writes/s with 8 concurrent writers
  (1.9/s with one). Without the null wstat, everything written
  since the last commit is lost on a crash — measured, all 1096
  acknowledged records.
- **gefs survives a crash cleanly.** It rolled back to its last
  commit and restarted with no errors, no check failures and no
  repair; the rolled-back writes vanished as a unit, not as a
  prefix. Records made durable with a null wstat all survived
  (50/50). "Crash safe" here means consistent, and durable only at
  the commit points you ask for.

## 5. Raw partitions through sd(3)

- **`devsd` does no caching of its own.** A write to
  `/dev/sdXX/data` (or any partition) builds a request in `sdbio`
  and calls the driver's `bio` entry synchronously.
  `/sys/src/9/port/devsd.c:796-918`.
- **`sdvirtio` issues a real device flush** — but only when asked
  with a SCSI `SYNCHRONIZE CACHE` opcode. `viorio` intercepts
  `0x35`/`0x91` *before* `sdfakescsi` and sends a `vioblkreq` of
  type 4, `VIRTIO_BLK_T_FLUSH`, waiting for completion.
  `sdvirtio.c:532-536,335-395`; the same code in the 1.0 driver at
  `sdvirtio10.c:576`.
- **No feature negotiation happens.** The driver reads the device
  feature word and never writes `Drvfeat`, so
  `VIRTIO_BLK_F_FLUSH`/`WCE` is never negotiated.
  `sdvirtio.c:41-42,232`.
- **`sdvirtio` splits a request at 32 sectors (16 KiB)** and issues
  the pieces serially, so a 64 KiB write costs 4× a 16 KiB one.
  `sdvirtio.c:496-517`.
- **A user process reaches the flush through `devsd`'s raw
  interface.** The protocol on `/dev/sdXX/raw` is: *write* the
  10-byte cdb, then *read* — the read is the data phase and is
  where the command is actually issued — then *read* again for
  status. The file is listed `-lrw-------` but is **not exclusive**:
  `devsd.c:749-758` tests `unit->rawinuse` on open, and nothing ever
  sets it (`devsd.c:786` only clears it), so two processes — or two
  procs of one process — can hold `/dev/sdXX/raw` open at once
  (verified: concurrent opens succeed). The cdb→data→status state is
  **per unit** (`unit->state`, `unit->req`), so concurrent raw
  commands on one unit corrupt each other's exchange; every user of
  the raw file on a unit MUST serialise its own commands, and only
  one process per unit should issue them. `devsd.c:1466-1519`
  (write), `devsd.c:1310-1341` (read), `devsd.c:919-983` (`sdrio`).
- **On AHCI the same opcode becomes a real ATA `FLUSH CACHE`.**
  `sdiahci.c:1857-1861` → `sdiahci.c:1664-1672` →
  `sdiahci.c:378-392`.
- **On the legacy IDE driver a flush is silently faked.**
  `sdide.c` defines `Mflush`/`Mflush48` (`sdide.c:245-246`) and
  never uses them, so `0x35` falls through to `devsd`'s
  `sdfakescsi`, which returns `SDok` having done nothing
  (`devsd.c:1128-1130`). **Deployment consequence: on a machine
  whose disks are claimed by `sdide`, durability additionally
  requires the drive's write cache to be off**, because nothing
  will ever flush it. (Raw ATA passthrough — a cdb prefixed with
  `0xff`, `sdide.c:2431-2450` — is available there if an ATA
  `FLUSH CACHE` must be sent by hand.) `cat /dev/sdctl` reports
  which driver claims each disk; a hardware qualification should
  check it.
- **Nothing on the platform flushes on its own**: no 9front file
  system and no code in `devsd` ever issues a flush. Their `sync`
  is `pwrite` and nothing more.

## 6. Measured cost of one durable write

4 KiB records, latency from `nsec()` around the write plus whatever
commit operation the mechanism needs.

| mechanism | per durable write | 1 writer | 8 writers | acked writes survive a crash? |
|---|---|---|---|---|
| raw partition `pwrite` | **8.4 ms** | 114/s | **409/s** | yes — 7519 of 7519, none torn |
| raw partition `pwrite` + SCSI flush | 8.6 ms | ~114/s | — | yes |
| gefs file + null `Twstat` | **530–620 ms** | 1.9/s | 7.7/s | yes — 50 of 50 |
| hjfs file + console `sync` | 48 ms (global barrier) | 14/s | does not scale | yes, when synced |
| cwfs file + console `sync` | 145 ms (global barrier) | 6/s | does not scale | yes, when synced |
| any file, no sync | 24–100 µs | — | — | **no** — 34, 59, 183 and 1096 acked records lost |

Raw-partition writes cost the same 8.4 ms for anything up to
16 KiB and exactly 4× that at 64 KiB (the request split, §5), so
**writing in units of ≤ 16 KiB is free relative to 512 B, and
writing more than 16 KiB in one call buys nothing.** An explicit
SCSI flush costs 175 µs and changed neither survival nor latency
here. Reads are ~280 µs.

The two `sync` rows are worse than the numbers suggest: their cost
is a function of the *whole file system's* dirty set, not the
caller's, so one writer cannot bound them.

**What these numbers are.** One virtualised machine — a two-vCPU
KVM guest with virtio-blk — not a fleet, and one release. Treat the
ratios as the finding and the absolute figures as indicative.

**What "crash" tested.** The crashes were guest power-offs: the
guest died, the host did not. So every survival result above is a
test of *"did the bytes leave the guest kernel before the ack"*.
**Host power loss was not tested**, and the host's storage cache
mode was not inspected. Two guest-side observations bear on it: no
virtio feature is negotiated (§5), and a 4 KiB raw write costs
8.4 ms while an explicit flush costs 175 µs and changes nothing —
consistent with a write-through backend. That is inference, not
confirmation.

## 7. Four-tuple atomicity

Making `(content, ver, wepoch, csum)` visible as one unit across a
crash (`../design/layer-a.md` §1.3) is a property of the mechanism
underneath, not of 9P.

- **Sidecar-then-publish on cwfs is unsafe.** Write the content to
  one file, then write the key file that publishes it, and cwfs
  gives no guarantee the content precedes the key on disk:
  dentry and allocation updates are `Bimm` and go out immediately,
  file data is `Bmod` and does not (§2). The failure was observed
  directly: after a crash, the content file was correct on disk
  while the key file existed with the right `stat` size and **read
  back as 4096 zero bytes** — a published key whose content never
  landed, indistinguishable from a good file except by checksum.
  The only ordering barrier cwfs offers is the global `sync` at
  ≥ 100 ms, and the crash after it still leaves the file system
  damaged.
- **hjfs is the same shape** — delayed writes, global barrier only
  — with worse post-crash damage.
- **gefs gives it by construction**: the rollback is to a commit
  point, so a sidecar pair either both exist or neither does, and a
  null wstat makes the pair durable. The price is §4's 530–620 ms.
- **On a raw partition it is the program's own to build, and every
  ingredient was verified.** Writes are unbuffered and synchronous,
  a write is acknowledged only after the device has taken it (no
  acknowledged record was ever lost, and none of 7519 was torn),
  and ordering between two writes is the program's because it
  issues the second only after the first returns. The discipline
  that follows: make the commit point a **single write of one
  sector** carrying a sequence number and a checksum over the
  record, replay-validate on start, and never assume atomicity of a
  multi-sector write. No tearing was seen at 4 KiB, but that is a
  device property, not a promise.
