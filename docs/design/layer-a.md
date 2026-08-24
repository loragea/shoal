# Layer A — distributed object store

Status: design proposal, not ratified, nothing implemented.

Scope: the object model, the per-disk storage server's 9P export,
the cluster map, the placement function, the write/read path, epoch
and fencing, heal/scrub/rebalance, and the monitor. `docs/target.md`
is the contract this designs toward; `docs/decisions.md` D1–D6 are
binding. Section 10 reports every place this proposal constrains or
strains them.

## 0. Conventions

*Normative.*

- MUST/SHOULD/MAY are RFC 2119 senses. Each section is marked
  **normative** (a reimplementation must match, bit for bit at wire
  and format boundaries) or **implementation policy** (a conforming
  implementation may differ).
- All textual formats in this design are 7-bit ASCII, LF-terminated
  lines, no CR. Tokens are separated by one or more spaces or tabs.
  Values MUST NOT contain white space or NUL.
- **One physical line per record.** Every record in every grammar
  defined here — `/meta`, `/repl` headers, `/rpc` requests and
  responses, `/advert` lines, `/tombs` lines, `/ctl` commands, and
  every status file on either server (`/status`, `/dirty`, `/stale`,
  `/lost`, `/jobs`, `/instances`, `/health`) — is exactly one
  LF-terminated physical line. There is no continuation. Wherever an example in this
  document appears on two lines, the wrap is typographic only and
  the real record is one line.
  The **cluster map (§3) is the single exception**: it is an ndb(6)
  style file in which a record continues through following indented
  lines. That exception exists so the map stays comfortable to edit
  by hand; nothing else in the design is hand-edited.
- A `#` at the start of a line begins a comment. Comments and blank
  lines appear only in the cluster map and are ignored by parsers.
  Comment preservation is **not** required anywhere: a writer MAY
  preserve them, the monitor's generated map contains only comments
  the monitor generates, and `propose` (§8.3) regenerates the map
  text from parsed state, discarding operator comments. The map
  carries no information in its comments.
- Error strings are 9P `Rerror` bodies. Each error below is defined
  by its **prefix**, which is terminated by end-of-string or by
  `": "`. A server MUST emit exactly the given prefix and MAY append
  `": "` plus detail. The prefix set defined in §2.6 is prefix-free
  and MUST stay so. Clients MUST match on the prefix and MUST NOT
  parse the detail, **except** where an individual error definition
  gives its detail a normative grammar; exactly one error does
  (`not primary`, §2.6). Error prefixes are lower case, no trailing
  punctuation, in the Plan 9 style.
  9front's `ERRMAX` is 128 bytes and `Rerror` bodies are truncated at
  the kernel boundary, so every prefix plus its normative detail MUST
  fit in 128 bytes; free-form detail MAY be truncated and is
  therefore never load-bearing.
- Integers on the wire are unsigned decimal unless a field is
  defined as hexadecimal (lower case, no `0x`).
- A **retryable** error is one whose meaning is "ask again shortly";
  the retryable set is `not ready`, `object unavailable`, `fenced`,
  `stale epoch`, `future epoch`, `degraded`. A client library MUST
  distinguish these from definitive errors and MUST retry them with
  bounded backoff after re-reading the map.
  `stale epoch` is retryable because it is the canonical
  refresh-and-retry signal: every client attach in the ≤ `pollms`
  window after any epoch bump gets it, and bumps happen on every `up`
  flap, every placement change, every stale-mark change and the
  `tombdays`/2 heartbeat. Treating it as definitive would break a
  conforming client at every bump.
  `degraded` is retryable in the sense that the condition may clear,
  but it is subject to §5.4's outcome ambiguity: the operation MAY
  already have been applied. A client MAY retry it blindly only for
  an idempotent operation; a `create` retried after `degraded` can
  legitimately return `object exists`, and a re-`write` of the same
  bytes at the same offset is safe while a read-modify-write is not.
- Two errors are **redirects** rather than retries: `not primary`
  (§2.6) names the correct instance, and `down` (§2.6, F3) says only
  that this instance may not serve. A client that receives either
  MUST re-read the map and re-evaluate placement; `down` differs from
  `not primary` solely in carrying no iid to redirect to.

## 1. Object model

*Normative except where marked.*

### 1.1 Identifiers

An **object id** (`oid`) is an opaque byte string. Layer A MUST NOT
parse it, MUST NOT infer relationships between ids, and MUST NOT
give any id special placement treatment. Layer B's `fileid.N`
convention (D3) is invisible here; that a file's stripes scatter
across all disks is a consequence of hashing the whole id, and is
the property that makes a file larger than a disk work.

An oid MUST match:

    oid  = 1*128( ALPHA / DIGIT / "." / "-" / "_" )

and MUST NOT be `.` or `..`. The bound of 128 keeps an id a legal
9P path element under every 9front file server, and keeps a
directory entry small. Ids are compared as byte strings; comparison
is case-sensitive.

Ids beginning with `shoal.` are **reserved** for system use (the
monitor's map snapshots, §8.2; the map carries the stale ledger
inside it, §7.1, so there is no separate ledger object). A storage
server MUST reject a `role=client` create of a reserved id with
`reserved name`; `role=admin` MAY create and write reserved ids
through the normal write path (§5.4 — primaryship and replication
apply, so `shoal.` objects are placed and replicated like any
other), which is how the monitor writes its `shoal.map.<epoch>`
mirror (§8.2). Layer B's grammar (fixed in the Layer B round)
MUST therefore constrain `fileid` so that no file id can begin with
`shoal.`; requiring `fileid` to be a lower-case hex string suffices.

### 1.2 Size

One immutable cluster-wide limit `objmax`, in the map header, fixed
at creation (§8.5). It MUST be a power of two and ≥ 2^20.
**Default: 16 MiB (16777216).** It is Layer B's stripe-unit ceiling
so it must be uniform and stable; 16 MiB keeps a 4 TB disk near
2.6·10^5 objects, small enough that full enumeration and local scans
stay cheap, and keeps a whole-object resync inside a few hundred
milliseconds (§5.5 does that arithmetic honestly, including the
per-message cost the first draft omitted).

A write whose `offset + count` would exceed `objmax` MUST fail with
`object too large`. Servers MUST NOT silently truncate.

### 1.3 Per-object metadata

Each object carries, durably:

| Field | Type | Meaning |
|---|---|---|
| `oid` | string | the id |
| `len` | u64 | current length in bytes, ≤ `objmax` |
| `ver` | u64 | version counter, starts at 1 on create |
| `wepoch` | u64 | map epoch of the primary that produced `ver` |
| `csum` | hex64 | object checksum (§1.4) |
| `state` | `live` \| `tomb` | tombstone flag (§1.5) |
| `mtime` | u64 | seconds since the epoch, last content change |

**Arbitration key.** Copies of an object are ordered by the pair
`(wepoch, ver)`, compared lexicographically, greater wins. `ver`
alone is not sufficient: a copy that survived from a fenced era must
lose to a copy produced under a later map even if a counter raced.

- **Absence is not a key.** An instance that holds no copy of `o`
  has no key for `o`. It is not `(0, 0)`, it is not comparable, and
  it MUST NOT be used as the predecessor of a delta (§5.3). Absence
  loses arbitration against any verifying copy.
- **A copy that fails local verification (§7.5) MUST NOT win
  arbitration against anything**, including absence. If it is the
  only copy, the object is `object lost` (§7.5). This replaces the
  first draft's "treat it as `(0,0)`", which had a corrupt copy
  beating absence.
- **Equal keys, differing content.** Equal keys MUST imply equal
  content (invariant I3, §5.7); the delta rule (§5.3) and the resync
  rule (§7.2) are what make that true. If a primary nevertheless
  observes two copies with the same key and different `csum`, that
  is a violated invariant, not a normal race. The primary MUST
  (a) make both holders re-verify (`op=verify` on `/rpc`, §5.6, for
  the peer; the `verify` ctl verb is the operator's spelling of the
  same thing); a copy that fails verification is out by the rule
  above; (b) if both verify, **the serving primary's own copy wins**,
  it MUST be pushed to the other holder as `op=full force=1` — the
  ordinary `op=full` applies only on a strictly greater key and would
  be refused `stale version` here, which made this rule
  unimplementable in the previous revision — and the event MUST be
  recorded in `/lost` as a divergence and reported in `/status`.
  Silent arbitrary tie-breaking is forbidden because it hides a bug.

`ver` and `wepoch` are assigned by the object's primary (§5) and
copied verbatim to replicas; a replica MUST NOT invent either.

**Local atomicity is normative, not implementation policy.** An
instance MUST NOT expose (in `/meta`, `/obj`, `/advert`, `/tombs`,
`op=meta`, `op=get`, or arbitration) a key it does not hold the
matching content and `csum` for. Concretely, the four-tuple
`(content, ver, wepoch, csum)` MUST become visible as one unit
across a crash: an object's published state after any crash MUST be
either the state before an update or the state after it, never a
mix. *How* that is achieved is implementation policy (write-ahead
record, sidecar-then-rename, log-structured commit); *that* it holds
is not. Without it, a crash that advances the key but not the
content produces a copy that wins arbitration and overwrites a good
one, and a crash that advances the content but not the `csum` sends
a healthy object to `/lost`.

**Durability before ack is normative.** See §5.4: the content, key
and checksum of an accepted write MUST be durable on every acking
instance, and on the primary, before the primary answers `Rwrite`.

### 1.4 Checksums

Object content is divided into fixed-size **checksum blocks** of
`blksz` bytes (map header, immutable, power of two, default
65536). Block *i* covers bytes `[i*blksz, min((i+1)*blksz, len))`.
The final partial block is hashed over its actual length. A hole
(§2.4) hashes as the zero bytes it reads as.

- Block digest: BLAKE2s with a **16-byte** digest, unkeyed, over
  the block's bytes.
- Object checksum `csum`: BLAKE2s with a **32-byte** digest,
  unkeyed, over the concatenation of all block digests in ascending
  block order. For `len == 0` the concatenation is empty, so a
  zero-length object has a fixed, well-defined `csum`.
- `csum` is rendered as 64 lower-case hex characters, **bare** — no
  algorithm prefix. The algorithm is named once, cluster-wide, by
  the map header's `csumalg` attribute (§3.2). Three distinct things
  used the attribute name `csum` in the first draft; they are now
  `csum` (this value), `csumalg` (the map header's algorithm name),
  and `dcsum` (the per-payload transfer digest, §5.5).

Block digests are stored locally alongside the object (format:
implementation policy) so a small write re-hashes one block, not
the whole object; at the defaults that costs 16 bytes per 64 KiB,
~0.025% of capacity. The two-level construction keeps partial
writes cheap while leaving `csum` a single value two instances can
compare in one line of text.

### 1.5 Delete and tombstones

Delete MUST NOT simply remove the object: a replica absent during
the delete would resurrect it at the next heal. Delete sets
`state=tomb`, `len=0`, releases the content, and bumps
`(wepoch, ver)` like any other write; a tombstone arbitrates
normally and is replicated by `op=delete`, which is self-contained
(§5.3) and so needs no predecessor.

Reads of a tombstoned object MUST fail with `object deleted`. A
create of a tombstoned id succeeds and produces a fresh `live`
object with `ver` one greater than the tombstone's — so as long as
the tombstone exists, no older copy can outrank the new object.

**Adopting a tombstone never transfers content.** A tombstone is its
metadata: an instance that must adopt one — a primary arbitrating an
advert, a `pull`, a reconcile — takes `state=tomb`, the key and
`len=0` directly from the `/advert` line or `op=meta` response and
commits that as its record. `op=get` on a tombstone fails
`object deleted` (§5.6) and is never needed; anything phrased as
"pull the winner" reduces, for a tombstone winner, to this
metadata-only adoption.

**Discard.** A tombstone occupies a metadata record and nothing
else, so discarding it is a space optimisation, not a requirement.
The first draft's discard rule (confirmation from the current
placement set plus `tombdays`) was unsound: strays, `out` disks and
long-absent disks are by definition outside the placement set, are
never asked, and resurrect the object when they return. The rule
here is deliberately stricter and easy to check at this scale.

A tombstone for `o` MAY be discarded by `o`'s serving primary only
when **all** of the following hold:

1. **Full confirmation.** Every instance in the map whose `status`
   is not `dead` has confirmed, at a time after the tombstone was
   created, either that it holds a copy of `o` with key ≥ the
   tombstone's, or that it holds no copy of `o` at all. Confirmation
   is an `op=meta` response (§5.6) or an `/advert` line (§7.2).
   This is a cluster-wide confirmation, not a placement-set
   confirmation. At ≤12 instances it is ≤12 round trips.
   Being `up=no`, `up=heal` or `status=out` does not excuse an
   instance from answering: F3 (§6.4) bars such an instance from
   serving clients and from acking, never from answering `op=meta`.
   Without that, an evacuation would make this condition unreachable.
2. **Retention.** `tombdays` (map header, default 7) have elapsed
   since the tombstone's `mtime`.
3. **Epoch supersession.** The current epoch is strictly greater
   than the tombstone's `wepoch`. The monitor MUST publish a new
   epoch at least once every `tombdays`/2 (a `bump`, §8.3) even on a
   completely idle cluster, so this condition is always reachable.
   It guarantees that an object created after a discard carries a
   `wepoch` strictly greater than any copy that predates the
   discard, so `ver` restarting at 1 cannot be outranked.

**Why 1–3 close the resurrection hole on their own.** Condition 1 is
a continuing obligation, not a one-time poll. A confirmation counts
only if it was made *after* the tombstone was created, and an absent
instance makes none — so for the whole time an instance is absent,
however long, it blocks the discard. There is no window in which a
discard happens behind an absent holder's back, and therefore no
state an instance can return in that lets it resurrect `o`:

- A returning holder of a stale `live` copy reports that copy. Its
  key is below the tombstone's, so it is not a confirmation and the
  discard stays blocked. Ordinary arbitration then pushes the
  tombstone onto it (`op=delete` is self-contained and applies iff
  its key is greater, §5.3), after which it can confirm.
- An instance that confirmed "no copy" cannot acquire a stale `live`
  copy afterwards, because the only source would be another instance
  still holding one — and any such holder is itself unconfirmed and
  still blocking the discard.

So at the moment a discard is permitted, every non-`dead` instance in
the cluster holds the tombstone, something newer, or nothing: there
is nothing left that could resurrect `o`. Condition 3 covers the
re-creation case — an object created after a discard carries a
`wepoch` strictly greater than the tombstone's, so a straggler still
holding the tombstone loses to the new object rather than deleting
it.

An earlier revision of this design added a *quarantine* state on top
of this (`quar=yes` for an instance absent longer than `tombdays`,
whose adverts were discounted). It forbade nothing that the argument
above does not already forbid, while adding a terminal map state — a
`quar=yes` instance could never reach `up=yes`, so `outmins` never
fired — and making the newest copy on a returning disk unpullable
until an operator intervened. It is removed; §10.4 records the
removal and its residual.

**Executing a discard.** Discarding is a cluster-wide operation,
because a tombstone the primary drops alone comes straight back: the
peers still hold it, the next reconcile finds the primary holding
nothing for `o` and a peer advertising `state=tomb` at a real key,
and absence loses arbitration (§1.3). Once 1–3 hold, the serving
primary MUST therefore send `op=discard oid=<oid> epoch=<u64>
ver=<u64> wepoch=<u64>` (§5.6) to every instance that confirmed holding the
tombstone, and MUST remove its own record **last**, only after every
one of them has answered `ok`.

A receiver checks, locally and without a round trip, that (i) its own
record for `o` is a tombstone whose key is exactly the one named, and
(ii) that tombstone's `wepoch` is strictly less than the receiver's
own current map epoch — condition 3, the one condition that bears on
the receiver's own safety. If either check fails it MUST answer
`not discardable` and keep the record. Conditions 1 and 2 are the
primary's to establish; re-deriving them at every receiver would cost
a round trip per pair of instances — quadratic, for a space
optimisation.

**A holder that misses the discard.** If any `op=discard` fails or
times out, the primary keeps its own record: the discard is simply
incomplete and is retried on a later pass. If a holder loses the
discard some other way — it crashes between answering and committing
the removal, or is restored from a backup — it re-advertises
`state=tomb`. The serving primary, which by then may hold no record
for `o`, treats that advert like any other: it adopts the tombstone
(absence loses arbitration), and the tombstone it now holds re-enters
the rule above and is discarded again, this time including the
advertiser. A missed discard therefore costs one extra discard cycle,
not a churn loop, and never a resurrection. (If the pull resets the
record's `mtime`, condition 2 delays that second discard by
`tombdays`; that costs space, not correctness.)

**Residual, stated plainly.** Two windows remain.

- An instance declared `status=dead` (§3.3) is excluded from
  condition 1 because `dead` is an operator assertion that the
  disk's data is permanently gone. If such a disk is later
  re-attached with its data intact, it can resurrect deletes. A disk
  declared `dead` MUST be reformatted — new `uuid` — before it may
  re-enter a cluster. The monitor MUST refuse a `register` from a
  `uuid` bound to a `dead` instance with `bad ctl`. Register-refusal
  alone does not make a returning dead disk inert — it never needs
  to register to advertise — so two further rules complete the
  closure: a `dead`-or-unmapped instance MUST NOT advertise, push or
  answer `/rpc` (§6.4, F3's carve-out scope), and receivers MUST
  refuse `role=repl` attaches from peers their map does not count.
- An instance that is `up=no` but not `dead` blocks condition 1 and
  therefore blocks all tombstone discard cluster-wide until it
  returns or is retired. That is a capacity cost, never a
  correctness one, and it is visible in `/status`. It is also the
  mechanism the resurrection argument above rests on, so it is not a
  defect to be optimised away casually.
- Restoring an instance's disk from a **backup** can reintroduce a
  pre-delete `live` copy after the tombstone has been discarded —
  the same class of hazard as a re-attached `dead` disk, arriving by
  a different door. A restored disk MUST be treated as a `dead`
  re-entry: reformat and fresh `uuid`, never restored-in-place,
  unless the backup demonstrably post-dates every discard (which an
  operator generally cannot demonstrate).

## 2. Storage-server 9P export

*Normative. The local on-disk representation behind it is
implementation policy (D1).*

One instance per disk (D4), serving stock 9P2000 — no extension.

### 2.1 Attach

The caller's role and the caller's epoch travel in `aname`: the only
place they can travel without extending 9P (D2).

    aname = attr *("," attr)
    attr  = "epoch=" u64 / "role=" role / "peer=" iid
    role  = "client" / "repl" / "admin"

- `role` defaults to `client`.
- `epoch` is REQUIRED for `role=client`, OPTIONAL for `role=repl`
  (where the per-operation `epoch=` field is what fences, §5.5), and
  MAY be omitted by `role=admin`.
- `role=repl` MUST be used by another storage instance acting on
  behalf of a primary, and MUST carry `peer=` naming the attaching
  instance. It grants access to `/repl`, `/rpc` and `/advert`, and
  read access to any locally held object regardless of primaryship.
  Those object reads are fenced exactly as `op=get` is (F1, §6.4):
  they are the same bytes by another path.
- `role=admin` grants `/ctl`, `/rpc` and the status files, and
  **read-only** walk, open and read of `/obj`, `/meta` and `/tombs`
  — the operator inspection path §2.2 describes. It MUST NOT grant
  create, write, wstat or remove on `/obj` — which MUST fail with
  `permission denied` — **except for reserved `shoal.` ids** (§1.1),
  which `role=admin` MAY create and write through the normal §5.4
  write path while unfenced; without that grant no role could write
  the monitor's `shoal.map.<epoch>` mirror at all. Admin reads are
  not `role=client` I/O: an admin read
  is not subject to the primaryship, handoff-grace or currency rules
  (§5.1), and MUST NOT be used as evidence of anything by another
  instance. It MAY read reserved `shoal.` objects (§1.1) even while
  the instance is fenced; that narrow exemption is what makes monitor
  rebuild executable (§8.6). All other `role=admin` object reads are
  fenced normally.
- An unparseable `aname` MUST fail the attach with `bad aname`. A
  role violation on any later operation MUST fail with
  `permission denied`.

**Epoch is checked at attach, not per operation.** At `Tattach`:
`epoch` less than the instance's current map epoch MUST fail with
`stale epoch`; greater MUST fail with `future epoch` and SHOULD
trigger an immediate map fetch by the instance. After a successful
attach, the fid's epoch is **not** re-checked; the authority for
every subsequent operation is the instance's *own current map*
(primaryship, `up`, `status`, fenced state), which it evaluates per
operation anyway. §6.2 justifies dropping the per-operation check
and re-verifies that nothing F1 fences escapes.

**On the aname idiom.** In 9front, `aname` conventionally selects a
tree, not an attribute block; using it as a key=value list is an
abuse of the field, chosen because D2 forbids protocol extension and
because the alternative — a stateful ctl write per connection — is
racy. `role` arguably belongs in `uname`, since authentication is
what should bind a role; if §10.3(5) is answered with "authenticate",
the natural refactor is to derive `role` from the authenticated
identity and keep only `epoch` and `peer` in `aname`.

### 2.2 File tree

    /ctl            write verbs (§2.5)
    /status         instance state, attr=value lines
    /map            the instance's cached cluster map, read-only
    /obj/<oid>      object contents
    /meta/<oid>     object metadata, attr=value, read-only
    /repl           replication push channel (§5.5)
    /rpc            replication request/response channel (§5.6)
    /advert         bulk version advertisement (§7.2)
    /dirty          objects known out of sync, one per line
    /stale          this instance's outstanding stale marks (§7.1)
    /tombs          tombstones held here, advert-format (§7.2)
    /lost           locally corrupt, diverged, or unrecoverable
    /jobs           background jobs (resync, scrub), one per line

`/obj` and `/meta` are directories; everything else is a file.

**Enumeration is snapshot-at-open.** A read of any of the status
files above MUST reflect a snapshot taken when the fid was opened, in
the Plan 9 convention, so that a concurrent mutation cannot tear a
read. A directory read of `/obj`, and a read of `/tombs`, SHOULD do
the same: concurrent creates and deletes SHOULD NOT cause an entry to
be skipped or duplicated part-way through a sequential read. Without
that, `lib9p`'s index-based `dirread9p` regenerates entry *n* per
`Tread`, and ~2.6·10^5 entries take thousands of sequential reads
over seconds — long enough for indices to shift.

It is SHOULD rather than MUST for `/obj` and `/tombs` because a full
snapshot of 2.6·10^5 entries costs real memory on a path no protocol
depends on. An instance that cannot afford one MAY serve these two
enumerations from a bounded window, in which case an entry MAY be
skipped or duplicated under concurrent mutation; it MUST then report
`objsnap=partial` in `/status` (§2.2 field list) so an operator
knows the listing is advisory. Everything else is unchanged: a
directory read of `/obj` MUST enumerate every `live` object and MUST
NOT enumerate tombstones; order is unspecified and clients MUST NOT
depend on it.

Tombstones are enumerable at `/tombs`, in `/advert` format (§7.2).
That is the audit path for §1.5's confirmation rule: without it,
nothing in the system could list what tombstones exist.

Peers do not use either of these: they use `op=list` on `/rpc`
(§5.6), which is paged, covers `live` and `tomb` in one pass, is
internally consistent per page, and does not depend on directory-read
semantics. §1.5's confirmations likewise come from `op=meta` and
`/advert`, never from a directory read — which is why weakening the
`/obj` snapshot rule costs no correctness. `/obj` enumeration and
`/tombs` exist for `role=admin` inspection, which §2.1 grants
read-only — and, because attaches now survive epoch bumps (§2.1), a
plain `mount` and `ls` by an operator is a real debugging path, not a
theoretical one.

`object exists` and `no such object` deliberately diverge from
9front's usual `file exists` / `file does not exist`. Layer A is
consumed through a client library, never mounted as a user-visible
tree, and the divergence makes it obvious in a log which layer
produced an error.

**Status-file contents.** One `attr=value` record per physical line
(§0) in every file below.

- `/status`, one line per attribute. The field `epoch=<u64>` — the
  highest map epoch this instance has adopted — is **normative**:
  §8.6's monitor rebuild reads it. The rest SHOULD be present under
  these names, because operators and the rules that mention them need
  somewhere to look:
  `iid=`, `uuid=`, `monid=` (§6.3), `monidmismatch=yes|no` (§6.3),
  `status=`/`up=` as this instance's own map states them,
  `fence=none|lease|operator|both` (§6.4),
  `epochregress=yes|no` (a lower-epoch map was seen and refused,
  §6.3), `msize=<u32>` (negotiated, §5.5), `chunk=<u32>` (the
  effective forwarded-write payload, i.e. the smallest peer `msize`
  less header and `IOHDRSZ`), `underrep=<n>` (local objects whose
  `|P(o)| < replicas`, §4.3), `strays=<n>` (locally held objects not
  in `P(o)` and not yet dropped, §7.4), `dirty=<n>`, `marks=<n>`
  (unresolved marks naming this instance as reporter),
  `lost=<n>`, `objsnap=full|partial`.
- `/dirty`: `oid=<oid> peer=<iid> epoch=<u64>` per record, plus one
  `fullsync peer=<iid>` line per peer carrying the coarse flag
  (§7.1).
- `/stale`: the marks this instance is party to, in the map's own
  ledger grammar — `stale=<subject-iid> reporter=<iid> since=<epoch>`
  (§3.1, §7.1) — so the same parser reads both.
- `/lost`: `oid=<oid> kind=corrupt|diverged|lost` plus whatever
  detail the implementation wants. `/jobs`: one line per running or
  queued background job. Both are diagnostic surfaces with no
  protocol consumer: beyond the `oid=` and `kind=` fields, **format
  is implementation policy**.

### 2.3 Qids and stat

- `/obj/<oid>`: `qid.type = QTFILE`. `qid.path` MUST be stable for
  the lifetime of the object id on that instance — including across
  delete, tombstone and re-create — and MUST differ between distinct
  ids on that instance. It MUST therefore be **allocated**, from a
  durable monotonically increasing per-instance counter, and
  recorded with the object; it MUST NOT be derived by hashing the
  id, because a hash cannot guarantee distinctness. A path value
  MUST NOT be reused for a different id while the first id's record
  exists. (The first draft suggested a 64-bit hash, which contradicts
  its own MUST.) Discarding a tombstone (§1.5) destroys the record,
  so a later create of the same id on that instance allocates a fresh
  path — the id's `qid.path` is stable for the lifetime of the
  *record*, not for all time. That is harmless: nothing outside a
  single fid's lifetime compares paths, and the counter is
  monotonic, so the two values can never be confused.
- `qid.vers` MUST be the low 32 bits of `ver`.
- `stat` reports `length = len`, `mtime`, `uid`/`gid`/`muid` of the
  server's choosing, `mode` `0666`.

### 2.4 Object I/O

- **create.** `Tcreate` in `/obj` with the oid as `name`, `perm`
  `0666`. Any of `DMDIR`, `DMAPPEND`, `DMEXCL`, `DMTMP` MUST be
  rejected with `bad create mode`; `ORCLOSE` MUST be rejected with
  `bad open mode`. Creating an existing `live` id MUST fail with
  `object exists`. A successful create yields `len=0`, `ver`
  assigned per §1.3/§1.5, and is replicated like any other write
  (§5.4) before it is acknowledged.
- **open.** `OREAD`, `OWRITE`, `ORDWR`, with or without `OTRUNC`.
  A write on a fid opened `OREAD` MUST fail with `bad open mode`.
- **read.** Reads at `offset ≥ len` MUST return count 0. Reads
  crossing `len` MUST return only the bytes below `len`. Bytes
  never written below `len` (holes) MUST read as zero.
- **write.** A write at `offset > len` extends the object, the gap
  becoming a hole. A server MAY return a **short write** (an
  `Rwrite` count smaller than requested); clients MUST handle short
  writes by looping. Short writes are how the primary bounds a
  single replicated operation to what fits one peer message (§5.5).
  A single accepted `Twrite` is applied atomically with respect to
  other operations on the same object, and is either fully applied
  everywhere it is applied at all or applied nowhere — there is no
  partial application of one `Twrite` on any copy.
- **truncate/extend.** `Twstat` with `length` set. Any other
  settable stat field, including `name`, MUST be rejected;
  `Twstat` rename MUST fail with `no rename`.
- **remove.** `Tremove` on `/obj/<oid>` performs §1.5 delete. Per
  9P, the fid is clunked whether or not the remove succeeds.
- **clunk.** `Tclunk` MUST always succeed and free the fid, whatever
  the instance's epoch, fence or role state. A server that can error
  a clunk leaks fids on one side or the other.
- **metadata.** `/meta/<oid>` reads as one `attr=value` line per
  object (wrapped here typographically only):

        oid=f3a91c.7 len=1048576 ver=42 wepoch=17 csum=9f...c1
        state=live mtime=1755990000 blksz=65536 cur=44
        placement=n2.1,n5.0 primary=n2.1 ready=yes

  `placement`, `primary` and `ready` are computed from the
  instance's current map and are advisory; the map is the authority.
  `cur=` is the epoch of this instance's most recent completed
  currency check for the object (§5.2), or `0`.

### 2.5 ctl grammar

One command per `Twrite`, one physical line; a partial line MUST be
rejected with `bad ctl`. The write's offset is ignored. Success
returns an `Rwrite` count equal to the bytes written; failure
returns `Rerror`. Commands that start background work return success
once the job is *accepted*; progress is read from `/jobs`.

| Verb | Role | Form | Effect | Errors |
|---|---|---|---|---|
| `refresh` | admin | `refresh` | Fetch the map from the monitor now. | `bad ctl` |
| `register` | admin | `register` | Re-register with the monitor (§3.4). | `bad ctl` |
| `pull` | admin | `pull <oid> <iid>` | Whole-object fetch from `<iid>` via `op=get`; adopt iff its key is greater (§1.3). | `fenced`, `bad ctl`, `no such object`, `stale version`, `checksum mismatch` |
| `push` | admin | `push <oid> <iid>` | Send the local copy to `<iid>` as `op=full`. | `fenced`, `bad ctl`, `no such object`, `stale version` |
| `reconcile` | admin | `reconcile [<oid>]` | Run §7.2 for one object, or for all local objects. | `fenced`, `bad ctl` |
| `advert` | admin | `advert [<iid>]` | Re-send version advertisements. | `fenced`, `bad ctl` |
| `drop` | admin | `drop <oid>` | Delete a local copy with no tombstone (§7.4). | `fenced`, `bad ctl`, `no such object`, `still placed` |
| `verify` | admin | `verify <oid>` | Re-hash and compare now. | `bad ctl`, `no such object`, `checksum mismatch` |
| `scrub` | admin | `scrub [start\|stop] [rate=<n>]` | Control the scrubber; `rate` in KiB/s. | `bad ctl` |
| `forget` | admin | `forget <iid>` | Discard the fine-grained dirty set for a peer, marking it `fullsync` (§7.1). Does **not** clear the peer's stale mark at the monitor. | `fenced`, `bad ctl` |
| `fence` | admin | `fence on\|off` | Set or clear an **operator** fence (§6.4). `fence off` MUST NOT clear a lease-derived fence; only a successful refresh does. | `bad ctl` |
| `newmonid` | admin | `newmonid <hex32>` | Replace this instance's pinned `monid` (§6.3), so its next refresh may adopt a map carrying the new value. The instance-side half of `forceepoch <e> monid=`. MUST be logged. Available while fenced — a `monid` mismatch is precisely what keeps refresh failing. | `bad ctl` |

The role column is uniformly `admin`, and that is the point: `/ctl`
is an operator surface, and no peer may drive another instance
through it. The two verbs a peer genuinely needs — `drop` (the §7.4
drop guard) and `verify` (§1.3's divergence procedure) — exist
*separately* as `op=drop` and `op=verify` on `/rpc` (§5.6), where
`role=repl` reaches them and the answer comes back on the same fid.
The ctl spellings above stay for operators. Every verb also fails
`unknown ctl` on an unknown spelling and `permission denied` on a fid
whose role does not permit it; those two are omitted from the table
because they apply to every row.

**ctl is inside the fence.** While the instance is fenced (§6.4),
every verb that mutates data or replication state — `pull`, `push`,
`drop`, `forget`, `reconcile`, `advert`, `fence off` — MUST fail with
`fenced`. Only `refresh`, `register`, `fence on`, `verify`, `scrub`
and `newmonid` remain available. The first draft left `/ctl` outside both
the epoch check and the fence, so a deposed instance could be driven
to overwrite, delete, discard replication state, or unfence itself.

Unknown verbs MUST fail with `unknown ctl`; a known verb with bad
arguments MUST fail with `bad ctl`; a verb issued on a fid whose
role does not permit it MUST fail with `permission denied`.

### 2.6 Error strings

*Normative.* Prefixes, exactly. The set is prefix-free.

    no such object          walk/open/read of an id that a completed
                            currency check found nowhere (§5.2)
    object exists           create of a live id
    object deleted          access to a tombstone
    object too large        write beyond objmax
    object lost             no good copy exists anywhere (§7.5)
    object unavailable      no up=yes member of P(oid) at this epoch
    not ready               this instance is the serving primary but
                            has not completed handoff grace or the
                            currency check for this object (§5.2).
                            RETRYABLE — never answer `no such object`
                            in its place
    bad object name         oid violates §1.1
    reserved name           client create under shoal.
    bad create mode         DMDIR/DMAPPEND/DMEXCL/DMTMP
    bad open mode           ORCLOSE, or write on an OREAD fid
    no rename               Twstat name change
    permission denied       role not permitted this operation
    stale epoch             attach epoch < server epoch, or /repl
                            op epoch < server epoch (§6.2, §5.5)
    future epoch            attach epoch > server epoch, or /repl
                            op epoch > server epoch
    not primary             client I/O to a non-primary. Detail is
                            NORMATIVE and is exactly the correct
                            iid: "not primary: n5.0". Clients MAY
                            parse it; they MUST tolerate its absence
    not discardable         op=discard whose key does not match the
                            receiver's tombstone, or whose wepoch is
                            not below the receiver's epoch (§1.5)
    fenced                  instance has no fresh map (§6.4), or a
                            fenced-state ctl verb (§2.5)
    down                    map says up=no or status=out for this
                            instance (F3). A client MUST treat it as
                            `not primary` without a redirect: re-read
                            the map and re-evaluate placement (§0)
    degraded                the write could not reach mincopies
                            instances, or the stale mark it would
                            require could not be registered (§5.4)
    stale version           /repl or /rpc op older than the local
                            copy, or op=get whose expected key does
                            not match
    out of sequence         delta op whose predecessor key is not
                            the receiver's current key (§5.3)
    checksum mismatch       content fails verification, or a
                            replicated op's resulting csum does not
                            match the sender's
    still placed            drop of an object still in placement
    disk full               no space; any operation that must store
                            bytes or a record MAY return it
    bad ctl                 malformed command or arguments
    unknown ctl             unknown verb
    bad aname               unparseable attach specifier
    bad map                 map text fails validation (monitor)

This is one prefix set, shared by storage instances and the monitor:
`bad map` is only ever emitted by the monitor and `object too large`
only by a storage instance, but a single set is what keeps the
prefix-free property checkable in one place, and a client library
parses errors from both. Two entries are deliberately not tied to a
single named rule: `disk full` may answer any operation that needs
space, and `bad object name` any operation naming an oid that
violates §1.1 — including a walk, a `/repl` header, or an `/rpc`
request.

## 3. Cluster map

*Normative: the grammar, the attribute names below, and the
requirement to ignore unknown attributes and record kinds.
Implementation policy: attribute order, comments, whitespace.*

### 3.1 Format

ndb(6) *style*. A **record** begins at a line whose first character
is not white space and continues through any following indented
lines. Each line is a sequence of `attr=value` tuples separated by
white space. The record's kind is its first attribute. Record kinds:
`map`, `node`, `instance`, `stale`. Readers MUST ignore attributes
they do not know and MUST ignore records whose kind they do not know;
this is what keeps a later replicated monitor (§8.7) additive.

    # shoal cluster map
    map=cluster0 epoch=41
        monid=8c1d0f5a9b2e47c3a6d180fe37b45219
        objmax=16777216 blksz=65536 replicas=2
        csumalg=blake2s256 placehash=blake2s256-64
        pollms=1000 leasems=3000 replms=1000 deadms=10000
        outmins=60 tombdays=7 mincopies=1 retain=8

    node=n2

    instance=n2.1 onnode=n2
        addr=tcp!10.0.0.2!17011
        uuid=3f1c9a20b47e4d18a0c6e5721b93df04
        class=ssd weight=100
        status=in up=yes since=41 fenced=no

    stale=n5.0 reporter=n2.1 since=39

Exactly one `map` record MUST be present.

**The stale ledger travels in the map.** `stale` records are the
ledger of §7.1: the record kind is `stale` and its value is the
**subject** instance, exactly as `instance=` names an instance, so
the record parses under the same rule as every other. They are
published inside the map text rather than beside it,
so that one refresh delivers placement and staleness **atomically and
at the same epoch**. That is not cosmetic: §5.2's witness set and its
`up=no` skip rule are evaluated against the ledger by every instance
on every currency check, and an instance that evaluated a fresh map
against a stale ledger could complete a check it should have failed.
Instances MUST evaluate marks as of their own current map and MUST
NOT cache a ledger across a map adoption. A `stale` record is
ignorable by construction (a reader that does not know the kind skips
it), which is what makes it a compatible addition to the format.

**How ndb-like this really is.** The file is deliberately readable
by eye and by `grep`, and `ndb/query` will parse the tuples. It is
*not* an ndb database in the useful sense and the design does not
pretend otherwise:

- A shoal-specific parser is mandatory. The first draft required the
  `map` record to be "first", which has no meaning in an order-free
  ndb file; the requirement here is only that exactly one exists.
- The instance back-pointer to its node is `onnode=`, not `node=`.
  Using `node=` for both a record kind and an attribute inside
  another record makes `ndb/query` unable to distinguish them.
- `node` records carry only the node name and are OPTIONAL and
  informational. The node set used by placement is derived from the
  `onnode=` of `status=in` instances (§4.3), never from the presence
  of a `node` record. The first draft gave node records an `addr=`
  meaning something different from an instance's `addr=`; nothing
  consumed it, so it is gone.
- Comments carry nothing and are not preserved across
  `propose`/`commit` (§0).

### 3.2 Header attributes

| Attr | Mutable | Meaning |
|---|---|---|
| `epoch` | monitor only | u64, strictly increasing (§6.1) |
| `monid` | **no** | 32 hex characters, a random 128-bit value generated once at cluster creation; the identity of the authority (§6.3, §8.6) |
| `objmax`, `blksz`, `csumalg`, `placehash` | **no** | fixed at creation; the monitor MUST reject changes |
| `replicas` | yes | R, target replica count, ≥ 1 |
| `pollms` | yes | map refresh period, all parties |
| `leasems` | yes | self-fence deadline, MUST be > `pollms` |
| `replms` | yes | peer-operation timeout in the write path, MUST be < `deadms` (§5.4) |
| `deadms` | yes | unreachability before `up=no`, MUST be > `leasems` |
| `outmins` | yes | continuous `up=no` before automatic `in`→`out` |
| `tombdays` | yes | tombstone retention and absence bound (§1.5) |
| `mincopies` | yes | refuse a write that would land on fewer than this many copies (§5.4) |
| `retain` | yes | number of past maps the monitor keeps (§8.2) |

`retain` is denominated in **epochs**, and epochs are now bumped by
`up` flaps, by placement changes, and by the `tombdays`/2 heartbeat
bump (§1.5), so a given `retain` covers less wall-clock time than
the first draft implied. That mattered when reconcile needed the
last `retain` placement sets; it no longer does. The redesign needs
exactly **one** historical map — the immediately previous one — for
the handoff test (§5.2), and instances MUST cache it themselves.
`retain` is now a history depth for operators and for an instance
that missed epochs while fenced, not a correctness parameter.

### 3.3 Node and instance records

Node names MUST match `1*63(ALPHA / DIGIT / "-" / "_")` and MUST NOT
contain `.`, which keeps node-id and instance-id byte strings
disjoint (§4.2).

`instance` records:

- `instance=<node>"."<index>` — the instance id (`iid`), index a
  decimal integer unique within the node, assigned by the monitor.
- `onnode=` — owning node; MUST equal the id's prefix (redundant on
  purpose: the map stays readable when grepped line by line).
- `addr=` — 9P dial string for this instance's export.
- `uuid=` — 32 hex characters, generated once by the instance when
  it first formats its disk. The durable identity: the iid is a
  label, the uuid is the disk.
- `class=` — device class tag (D5). Present from day one; v1
  placement MUST ignore it.
- `weight=` — capacity weight. Present from day one; v1 placement
  MUST ignore it, and a v1 monitor MUST reject any value other than
  `100` (§4.4) so that no operator can believe weighting works.
- `status=` — placement state, one of `new`, `in`, `out`, `dead`.
  **Only `status=in` participates in placement.**
- `up=` — liveness, one of `yes`, `heal`, `no`. Does **not** affect
  placement; affects primary selection and the ack set (§5).
- `since=` — epoch at which `status`/`up` last changed.
- `fenced=` — `yes` only when the instance was demoted to `up=no`
  because its refresh lease expired (§8.4), which by F1+F2 means it
  has certainly self-fenced. `no` otherwise, including for operator
  demotions, where the instance is alive and unfenced. Consumers:
  §5.2's handoff grace.

**Legal combinations.** `status=dead` MUST imply `up=no` and
`fenced=no`. `status=new` MUST imply `up` ∈ {`yes`,`no`} (a `new`
instance is in no placement set, so there is nothing to heal).
`fenced=yes` MUST imply `up=no`. Everything else in the cross product
is legal.

**`fenced=` is cleared by leaving `up=no`.** Any transition out of
`up=no` — to `heal` (§7.3) or straight to `yes` by operator override
— MUST set `fenced=no` in the same commit. Otherwise the monitor's
own "heard from again → `up=heal`" transition would stage
`fenced=yes up=heal`, which is illegal above and which §8.1's
`commit` validation MUST reject: the monitor would refuse its own
map. `fenced=` records why an instance was demoted, and once it is
back it is no longer demoted.

**`status=dead` has meaning.** It is an operator assertion, made
with `retire` (§8.3), that this disk's data is permanently gone.
Consequences, all normative: it leaves every placement set, is
excluded from tombstone confirmation (§1.5) and from currency
witness sets (§5.2), its outstanding stale marks may be cleared by
`forcesync` (§8.3), and **its iid is retired forever**. In the first
draft `dead` appeared in the enum and nowhere else.

**iids are never reused.** The monitor allocates
`<node>.<smallest index never yet used on that node>` and MUST NOT
reuse an index, even for a `dead` instance. Placement hashes the
iid (§4.2), so reusing an index would silently hand a new disk the
retired disk's placement share and its strays. A replacement disk
therefore gets a fresh iid and HRW moves data onto it — minimal
movement, and correct, which at this scale is the right trade
against the "replace in place, no movement" alternative.

The `status`/`up` split is the load-bearing idea of the map: a
transient failure changes only `up`, so no object moves; data moves
only when an operator or the `outmins` timer changes `status`.

### 3.4 Identity acquisition when a disk is added

1. The operator starts an object server on the new disk with the
   node name, the monitor's address, and a class tag. On an
   unformatted disk it generates a random 128-bit `uuid`, writes it
   into a local superblock, and never changes it again.
2. It attaches to the monitor with `role=instance` and writes
   `register uuid=<hex> node=<name> addr=<dial> class=<tag>`.
   Registration is idempotent and is repeated at every start.
3. The monitor binds the uuid to an iid — reusing the existing
   binding if the uuid is known, otherwise allocating a fresh index
   — and records the instance with `status=new up=yes`. A `register`
   from a uuid bound to a `dead` instance MUST be rejected with
   `bad ctl` (§1.5's reformat-before-rejoin rule).
   `status=new` does **not** enter placement, so registering a disk
   moves no data. A long-absent disk that re-registers is treated
   like any other: it is `up=heal` if it has a placement history and
   `status=new` if it does not, and its adverts are believed. §1.5
   explains why no quarantine of a long-absent disk is needed.
4. The operator inspects `/instances`, then writes `enable <iid>`
   and `commit` (§8.3). The epoch bumps, placement changes, and
   rebalance (§7.4) begins.
5. Moving a disk to a different node: the instance registers with a
   different `node=`; the monitor MUST NOT silently re-home it,
   because that changes placement. It reports the conflict in
   `/instances`; an operator writes `rehome <uuid> node=<name>` and
   `commit`. The disk keeps its objects; they become strays under
   the new map and are resolved by §7.

## 4. Placement

*Normative in full. Two implementations MUST agree bit for bit.*

### 4.1 Choice of algorithm

Two-level **rendezvous hashing (HRW)**: R distinct nodes by highest
node score, then one instance within each chosen node by a second
HRW round.

| | movement on map change | weights | cost per lookup | simplicity |
|---|---|---|---|---|
| HRW (chosen) | minimal: only the departing/arriving member's share moves | via virtual copies (§4.4), not enabled in v1 | O(nodes + disks-on-chosen-nodes); ~15 hashes at 12 nodes | ~30 lines, no tunables, no bucket types |
| jump hash | minimal, but only for tail growth | none | O(ln n) | disqualified: it returns an *index* into a dense range; removing a disk from the middle is not expressible |
| straw2 / CRUSH | minimal | native, exact | O(n) with a log per candidate | needs bucket types, rules, tunables, and a float `log`; famously subtle. Solves scale and heterogeneity we do not have |

At 3–12 nodes the cost column is irrelevant and the simplicity
column decides. HRW also hands us primary selection and failover
order for free (§4.3), which straw2 would not.

### 4.2 Hash

    H(x) = the first 8 bytes of BLAKE2s-256(x), unkeyed, 32-byte
           digest, interpreted as a big-endian unsigned 64-bit
           integer.

The map header's `placehash=blake2s256-64` names exactly this
function. A different value is a format change requiring a new
epoch and a cluster-wide flush; the attribute exists so the swap
can be additive (§9).

Score inputs, as byte strings with no separator beyond what is
shown, where `oid`, node ids and iids are their ASCII bytes:

    node score:      Sn = H( oid || 0x00 || 'N' || nid )
    instance score:  Si = H( oid || 0x00 || 'D' || iid )

`oid` contains no NUL (§1.1), so the concatenation is unambiguous;
`'N'`/`'D'` domain-separate the two rounds; node ids contain no `.`
and iids always do, so the two id spaces cannot collide.

### 4.3 Algorithm

Given the map at epoch `e` and an `oid`:

1. Let `I` = instances with `status=in`. Let `V` = the set of node
   names appearing as `onnode=` on at least one member of `I`.
2. For each `n ∈ V` compute `Sn`. Sort descending by `Sn`;
   **ties break in favour of the byte-wise greater node id**
   (unsigned byte comparison; if one id is a prefix of the other,
   the longer wins). Take the first `min(R, |V|)` nodes, in order.
3. For each chosen node, over its members of `I`, compute `Si`;
   take the maximum, same tie-break. The result is that node's
   selected instance.
4. The ordered list `P(oid) = [i0, i1, …]` is the **placement
   order**. `|P| < R` is legal and means the object is
   structurally under-replicated; instances MUST report the
   condition in `/status` and MUST still serve.

Replicas land on distinct nodes by construction (D4). Placement is
a pure function of `(oid, map)` — it MUST NOT consult liveness,
reachability, load, or free space, or clients and servers would
disagree.

**Serving primary.** The primary is the first instance in `P(oid)`
with `up=yes`. If no member of `P(oid)` is `up=yes`, the object has
no primary at this epoch and every access MUST fail with
`object unavailable`. Because `P` is stable across an `up` change, a
node failure promotes `i1` without moving a byte.

Being the serving primary is **necessary but not sufficient** to
serve. §5.2 adds the two conditions the first draft was missing: the
handoff grace, and a completed per-object currency check. The first
draft treated "first `up=yes` member of `P`" as licence to answer
client operations immediately, which is what made an operator's
`enable` produce wrong answers.

### 4.4 Weights: reserved, not implemented

`weight` is in the format (§3.3) and ignored. When it is enabled,
the rule SHALL be integer-exact virtual copies, hashing these exact
byte strings:

- Let `k(i) = weight(i)/100` for instance `i`, a positive integer.
- **Instance round.** Instance `i` contributes `k(i)` candidate byte
  strings: for `j = 0`, the bare `iid`; for `1 ≤ j < k(i)`,
  `iid || 0x00 || dec(j)`, where `dec(j)` is `j` in ASCII decimal
  with no leading zeros. `Si` for the instance is the maximum of
  `H(oid || 0x00 || 'D' || c)` over its candidates `c`.
- **Node round.** Let `K(n) = Σ k(i)` over `i ∈ I` with
  `onnode(i) = n`. Node `n` contributes `K(n)` candidates: for
  `j = 0`, the bare node id; for `1 ≤ j < K(n)`,
  `nid || 0x00 || dec(j)`. `Sn` is the maximum of
  `H(oid || 0x00 || 'N' || c)` over those candidates.
- Tie-breaks are on the *candidate* byte string, byte-wise greater
  wins, as in §4.3.

No `log`, no `pow`, no floating point, so the result is reproducible
across libm implementations — the reason weighted HRW's usual
`-w/ln(u)` form is rejected outright. The first draft reserved this
rule without saying what was hashed, which made it unimplementable;
the strings above are the reservation. Until it is ratified, a v1
monitor MUST reject `weight` ≠ 100 with `bad ctl`.

Consequence to state plainly: v1 fills disks in proportion to
*count*, not capacity. A cluster with a 1 TB and an 8 TB disk on
one node will fill the small one first. The envelope assumes
roughly homogeneous disks.

## 5. Write and read path

*Normative.*

The first draft had exactly one notion of "in the replica set but
not yet current": the per-instance `up=heal` state. Per-object
staleness has three other sources — a placement change hands an
instance objects it has never held; a delta write applied to a
lagging copy corrupts it; and the record that a peer was left behind
dies with the primary that wrote it. This section replaces the write
path with three per-object mechanisms (currency, the delta rule, and
the durable stale mark) and one global one (the handoff grace).

### 5.1 Reads

A `role=client` read MUST be served only by an instance that is
(a) the serving primary for the object under its own current map
(§4.3), (b) past the handoff grace for that object, and (c) current
for that object (§5.2). Any other instance MUST answer `not primary`
with the correct iid in the normative detail (§2.6).

Why not replicas: with degraded writes (§5.4) a replica can
legitimately lag and a client cannot cheaply prove otherwise.
Primary-only reads make D2's contract hold by construction — the
primary is both the single serialization point for the object and
its only reader. It costs no aggregate bandwidth either:
parallelism comes from striping across objects (Layer B), which
spreads a file over every disk, not from fanning one object's reads
across its R copies. Replica reads become considerable only
alongside a per-object cleanliness proof; deferred, gated on
measurement, and needing no format reservation today.

### 5.2 Per-object currency and the handoff grace

This is the mechanism the first draft lacked.

**Currency.** An instance is **current** for object `o` at epoch `E`
when it has completed a *currency check* for `o` at `E` and has held
serving primaryship for `o` continuously since. An instance records
the result durably as `cur=<epoch>` beside the object (visible in
`/meta`). It MUST invalidate `cur`:

- for every object, when it adopts a new epoch; and
- for every object, **on process start** — after a clean exit as
  much as after a crash.

The second rule is not belt-and-braces. A crash can lose exactly the
state that makes `cur` meaningful: the primary stages a write, an
acker durably commits it, the primary dies before its own commit
(§5.4 step 6). Nothing bumps the epoch, so on restart a durable
`cur=E` would license the primary to serve — and, worse, to re-issue
the *same* key `(E, ver+1)` over different content, producing two
copies with equal keys and different `csum` and violating I3 by an
ordinary crash. Re-running the check finds the acker's higher copy
and adopts it. Restart is cheap and rare; a wrong `cur` after a
restart is neither.

Revalidation after a bump is cheap — one `op=meta` round trip per
witness, on a sub-ms LAN — and the bulk paths (§7.2, §7.3)
revalidate in the background so that first client touches mostly
find `cur` already set.

**The witness set.** `W(o)` at epoch `E`, computed from the map, is
every instance that is not `status=dead` and satisfies any of:

1. it is a member of `P(o)` under the map at `E`;
2. it is a member of `P(o)` under the map at epoch `E−1`
   specifically. Instances MUST cache the map they most recently
   adopted; if that map is not `E−1` — the instance was fenced or
   partitioned across several epochs — the instance MUST either fetch
   `/maps/<E−1>` from the monitor, which §8.2 requires the monitor to
   keep, or substitute *every* instance with `status` ∈
   {`new`,`in`,`out`} for this clause. Keying the clause to "the
   previous epoch I happened to adopt" was wrong: with two
   placement-changing commits inside the gap, the holder of the
   current copy drops out of `W(o)` and the check completes on stale
   data. The same substitution MUST be used whenever this instance's
   own reconcile pass (§7.2) for the most recent placement-changing
   epoch has not yet completed: until it has, clause 3's local
   knowledge of stray holders is not populated, and one epoch of
   placement memory is measurably not enough to find a copy that
   moved twice;
3. it is known locally, from an `/advert` line or an `op=meta`
   response, to hold a copy of `o` (a stray holder);
4. it is named as the **reporter** of an unresolved stale mark
   (§3.1, §7.1) whose **subject** is a member of `P(o)` at `E` or at
   `E−1`.

Clause 4 is scoped to the subject's placement, and the scoping is
load-bearing in the other direction too. Unscoped — "the reporter of
any unresolved mark, whatever the subject" — a single reporter that
dies while a mark is outstanding lands in the witness set of *every*
object, and by the skip rule below fails every currency check in the
cluster: every read and every write everywhere answers `not ready`
until the reporter returns or an operator runs `forcesync`. A
second disk failure during recovery from the first would take a
cluster with every byte present completely dark. Scoped, the blast
radius of a dead reporter is the objects whose placement contains its
subject — about `1/|V|` of the cluster at R=2 — which is the same
order as the failure itself.

**Marks are read from the map.** Clauses 4 and the skip rule are
evaluated against the `stale` records in the instance's **own current
map** (§3.1). There is no separate ledger fetch and no freshness
rule to get wrong: a refresh delivers placement and ledger together,
at one epoch, and a mark registered at the monitor is visible to
everyone at the next epoch, which §6.1 makes a mark change produce.

**The check.** A currency check for `o` at `E` succeeds when the
instance has obtained an `op=meta` response for `o` (§5.6) from
every member of `W(o)` whose `up` is `yes` or `heal`, and every
member `u` of `W(o)` with `up=no` satisfies "`u` is the reporter of
no unresolved stale mark whose subject is in `P(o)` at `E` or `E−1`".
It then arbitrates (§1.3) over the responses and its own copy.

- If the winner is its own copy, it sets `cur=E`.
- If the winner is elsewhere, it MUST pull it — and MUST NOT do so
  inside a client request. It answers `not ready`, runs the pull as
  a background job (`/jobs`), and sets `cur=E` when the pull
  completes. A whole-object pull is up to `objmax`; §5.5's own
  arithmetic puts that at 185–545 ms, which is not something a
  `Twrite` may sit on (§5.4's blocking bound). If the pull fails the
  check is re-run from the start.

**`cur=E` is only valid if the check ran after the handoff grace.**
Every `op=meta` of a check that sets `cur` MUST be *sent* after the
object's handoff grace for `E` (below) has expired on the checking
instance's clock. Where no grace applies — the instance was already
the serving primary for `o` at `E−1`, or the grace is exempted below
— there is nothing to wait for and the check may run at once.
Background pre-warming MAY run a check earlier and MAY prefetch the
winner's content, but MUST NOT set `cur`. Otherwise
the grace buys nothing: the incoming primary Z can check at the
instant it adopts `E`, see the old primary X's copy at `(E−1, 10)`,
pull it, set `cur=E`, and wait out the grace — while X, which has not
refreshed yet, legitimately acks `(E−1, 11)`. At grace expiry Z
serves version 10, and Z's next write assigns `(E, 11)`, which beats
`(E−1, 11)` and destroys an acked write. The grace bounds when X can
last ack; the check must therefore look *after* that bound, not
before it.

If any required response cannot be obtained within `replms`, the
check FAILS and the instance MUST answer `not ready` — a retryable
error. It MUST NOT answer `no such object`, and MUST NOT serve its
own possibly-stale copy. **`no such object` is only a legal answer
after a currency check has completed and found no holder.**

*Why skipping `up=no` witnesses is safe.* An instance `u` goes
`up=no` only after it stopped refreshing (§8.4) or was demoted by an
operator; either way, by the write rules below, every write `u` acked
as serving primary for `o` was either

 (i) durably committed by every other member of `P(o)` at that epoch
     — §5.4 step 5 now quantifies over *every* member of `P(o)` other
     than the primary, `up=no` members included, not merely over the
     candidates — so a witness holding an equal key exists, and by
     induction on each handoff's own currency check it is reachable
     from `W(o)`; or
 (ii) accompanied by a stale mark that `u` registered, as reporter,
     against each member that did not commit, *before* the ack. That
     mark is still unresolved, because only the reporter (`synced`)
     or an operator (`forcesync`, `retire`) can resolve it, and both
     of the operator paths are logged data-loss acknowledgements.

In case (ii) the mark's subject was a member of `P(o)` at the epoch
of the ack, so clause 4 keeps `u` in `W(o)` and the skip rule refuses
to skip it — *provided the subject is still in `P(o)` at `E` or
`E−1`*. Two rules keep that true rather than hoping for it:

- §7.4 forbids a `status`-changing commit while any instance is
  `up=no` and is the reporter of an unresolved mark (except under
  `commit force`, logged). So placement cannot drift away from a
  down reporter's marks while they are outstanding.
- §7.4 also forbids the *next* placement-changing commit until every
  `up=yes` instance has reported its reconcile pass for the previous
  one complete (`rebalanced`, §7.2). So while the reporter was still
  up, one placement change at a time was resolved by a reconcile pass
  whose witness sets included the old placement, which is what
  propagated `u`'s uniquely-held writes onto the new placement
  members before any further move.

The lemma is load-bearing; besides those two, it rests on the drop
guard (§7.4: a copy is dropped only after the primary confirms every
placement member holds the current version) and on the stale-mark
rule (§5.4 step 5). All four are normative for that reason.

**The handoff grace.** An instance MUST NOT serve `role=client`
operations on `o` at epoch `E` if it was **not** the serving primary
for `o` under the map at epoch `E−1`, until `leasems` has elapsed on
its own clock since it adopted `E`. Until then it answers
`not ready`. An instance that does not hold the `E−1` map, and does
not fetch it, MUST assume the grace applies: the conservative answer
costs `leasems` of `not ready`, the optimistic one costs an acked
write.

- The previous serving primary either refreshed within that window,
  learned `E`, and stood down (it answers `not primary`), or failed
  to refresh and self-fenced by F1. Either way it is no longer
  acking writes by the time the grace expires. Formally: any
  successful refresh returning an epoch < `E` happened before `E`
  was published, which is before this instance adopted `E`; F1 gives
  the old primary `leasems` from that read.
- **Exemption.** The grace MAY be skipped for `o` if the instance
  that was serving primary for `o` at `E−1` is `up=no fenced=yes` at
  `E` (§3.3) — the monitor demoted it on
  refresh-lease expiry, so F1+F2 already guarantee it is fenced.
  This is the common failover case and is why a node death costs
  `deadms`, not `deadms + leasems`.
- The grace is **not** exempted for placement changes or operator
  demotions, because in both the deposed primary is alive and
  unfenced. (Lease expiry is now the only automatic demotion there
  is, §8.4, so these two are the whole of the non-exempt set.)

The grace is what stops the "operator adds a disk, HRW moves
primaryship, the old primary keeps acking at the old epoch, the new
primary serves a snapshot taken before those acks" failure. The
currency check is what stops the "new primary holds nothing and says
`no such object`" failure. They are separate mechanisms because they
close separate holes: the grace bounds *time*, the check bounds
*knowledge* — and the ordering rule above is what makes them
compose instead of merely coexist.

**What a check costs, and on how much of the cluster.** A check is
`|W(o)|` `op=meta` round trips, ≤ 12 on this envelope, paid on the
first touch of an object after a bump unless the background sweep got
there first. The witness set is not the cost that matters, though;
the failure modes are. A `up=no` witness that is an in-scope reporter
fails the check outright, and that is *intended* — but it now costs
only the objects whose placement contains that mark's subject, about
`1/|V|` of the cluster, instead of all of it. A witness that is
`up=yes` but unreachable costs `replms` per check and `not ready`
until the monitor demotes it at `deadms`, again for `1/|V|` of
objects. Both are stated in §10.1 as consequences, because both are
visible to whoever runs this.

### 5.3 The ack set and the delta rule

`P(o)` is placement; `up` decides who is eligible to take a write;
**per-object sync state decides who actually takes it.**

- **Candidates** `C(o)` = every member of `P(o)` whose `up` is `yes`
  or `heal`, other than the primary. Computed from the map.
- A candidate is an **acker** for a given write iff the primary
  knows its committed key for `o` is exactly the primary's current
  key for `o` — i.e. it is the exact predecessor of the write being
  sent. The primary learns this from a completed `op=full` resync,
  from the acknowledgement of the immediately preceding delta, or
  from an `op=meta` response; it MUST assume "not synced" for any
  candidate it has no such evidence for, including after a restart,
  after any epoch adoption, and after any failed operation to that
  peer. This per-(object, peer) sync state is instance-local, need
  not be durable (its safe default is "unsynced"), and its
  representation is implementation policy.
- A candidate that is not an acker MUST be brought current by
  `op=full` (§5.5) before it takes any delta, or excluded from the
  write with a stale mark (§5.4). **A delta MUST NOT be sent to a
  candidate whose key is not the exact predecessor.**
- **Candidacy is about who may take the write, not about who may be
  left behind.** A member of `P(o)` that is `up=no` is not a
  candidate — nothing is sent to it — but it is still a placement
  member that will not hold this write, and §5.4 step 5 registers a
  stale mark for it exactly as for a candidate that failed. Reading
  the mark obligation as covering only candidates is what made the
  canonical degraded write (peer down, `mincopies=1`, primary acks
  alone) record nothing at all, and let the returning peer be
  promoted over acked writes.

**Delta ops carry their predecessor.** `op=write` and `op=trunc`
carry `pver=`/`pwepoch=`, and the receiver MUST reject them with
`out of sequence` unless its own committed key equals that pair
exactly, and MUST reject them with `out of sequence` if it holds no
copy at all (absence is not a key, §1.3). `op=create`, `op=delete`
and `op=full` are **self-contained** — they define the whole object
state — and apply iff their key is greater than the receiver's, or
the receiver holds nothing.

This one rule kills the corruption the first draft's "apply iff the
key is greater" produced: a replica that missed writes 11–15 could
apply write 16 at offset 0 and end up holding a self-consistent
object with a winning key and missing bytes, undetectable by
arbitration. Under the rule it answers `out of sequence`, the
primary resyncs it or marks it stale, and equal keys keep implying
equal content (I3).

### 5.4 The write path

A `role=client` write, create, truncate or remove on the primary:

1. **Admission.** Check fenced state (§6.4) and that the map does
   not say `up=no`/`status=out` for this instance (F3). Check that
   this instance is the serving primary for `o`; else `not primary`.
   Check the handoff grace and currency (§5.2); else `not ready`.
2. **Order.** Take the per-object lock. All concurrent operations on
   one object are totally ordered here; this is where D1's
   "primary-ordered" is realised. The lock covers one object only:
   operations on other objects MUST proceed concurrently (§5.4.1).
3. **Prepare.** Compute the new key and the resulting `csum`, and
   **stage** the update durably in local storage. The new key is
   `(E, ver+1)` where `ver` is the object's current version; for a
   create of an id with no record it is `(E, 1)`, and for a create
   over an existing tombstone it is `(E, tombstone ver + 1)` (§1.5),
   which is what stops an older copy outranking the new object. A
   staged update is invisible: it MUST NOT appear in `/meta`,
   `/obj`, `/advert`, `op=meta`, `op=get`, or arbitration, and the
   object's published key is unchanged. This is the "never expose an
   unacked key" rule (invariant I2).
4. **Replicate.** Send the operation to every acker (§5.3) in
   parallel and wait up to `replms`. Each receiver applies and
   **durably commits** it before replying (§5.5); a reply means
   committed.
5. **Resolve the ack set.** Let `M` = every member of `P(o)` other
   than the primary that has **not durably committed this write** —
   a candidate that is not an acker, a candidate that did not reply
   within `replms` or replied with an error, and every member that is
   `up=no` and so was never sent anything at all.
   - `M` is empty → go to 6. This fast path applies only when
     `C(o) = P(o)\{primary}` and every candidate committed; a
     placement member that is `up=no` puts the write on the degraded
     path even when every candidate acked.
   - Otherwise, let `k` = 1 (the primary) plus the number of members
     that did commit. If `k < mincopies`, go to 7 (fail). Otherwise
     the primary MUST, **before** proceeding:
     a. durably register a stale mark with the monitor (§7.1) for
        each member of `M` — **unless** a mark for that (subject,
        this instance) pair is already registered and unresolved, in
        which case nothing is sent: one mark per divergence episode,
        not one per write — and receive the monitor's
        acknowledgement. The primary MUST bound that round trip by
        `replms`; if the monitor cannot be reached, or answers
        nothing in `replms`, go to 7.
     b. durably record the fine-grained dirty record
        `(oid, peer, epoch)` locally (§7.1) for each member of `M`.
     It SHOULD additionally report `unreachable <iid>` (§8.3) for a
     member that timed out. That report is a `/health` diagnostic
     only: it MUST NOT influence `up` for anybody (§8.4), and the
     write path MUST NOT wait for it.
   The `up=no` case is the one the previous revision got wrong. It
   quantified this step over *candidates*, and an `up=no` placement
   member is not a candidate — so the canonical degraded write (R=2,
   peer down, `mincopies=1`, primary acks alone) registered no mark
   and no dirty record. When the peer came back, nothing named it as
   a subject, no primary held a dirty record for it, the heal gate
   opened, and it was promoted over every write it had missed. §5.7's
   I1 argument always said "any placement member left behind"; this
   step now says the same thing.
6. **Commit.** Commit the staged update locally, atomically
   switching content, `ver`, `wepoch` and `csum` together (§1.3),
   durably. Release the lock. Answer `Rwrite`.
7. **Fail.** Discard the staged update. The local object is
   untouched: same content, same key, same `csum`, still verifying.
   Release the lock, clear the sync state for every candidate
   involved, invalidate `cur` for `o`, and answer `degraded`.

**Durability.** "Commit" means durable against power loss on that
instance before it is reported — on every acker in step 4 and on the
primary in step 6. What the underlying 9front file system must be
asked for to make that true is §10.2 evidence work; that it must be
asked for is normative here.

**What a failed write means.** The first draft applied the write
locally in step 6 and then refused to ack, so a failed write mutated
the primary, advanced its key, and left its `csum` stale — a single
network timeout could make a healthy object permanently
`object lost`. Under the rule above the primary's copy is never
mutated by a write it does not ack. But ackers that already
committed in step 4 hold `(E, ver+1)` while the primary holds
`(E, ver)`, so:

> **An operation that returns an error, or that is flushed, MAY or
> MAY NOT have been applied. It is never partially applied, and
> whichever outcome occurs, every copy converges to that same single
> outcome.**

That ambiguity is inherent without consensus and is compatible with
target.md's linearizability (an operation whose response is lost may
or may not take effect); it is **not** compatible with a caller that
assumes an error means "nothing happened". Layers B and C MUST treat
a failed or flushed object write as possibly applied — re-read
before acting on it. §10.1 records this as a constraint this round
imposes upward.

After a step-7 failure the primary MUST re-run the currency check
for `o` before serving it again, which is what adopts a
higher-keyed copy left on an acker.

**No unbounded stall — the bound, honestly summed.** The first draft
blocked the client for up to `deadms` (10 s) while a failure was
confirmed. Nothing here waits on a failure detector. But "no
`role=client` operation may block longer than `replms`" was too glib:
one `Twrite` can pay several bounded waits in series, and the honest
statement is their sum. In the worst case one client operation costs

    (admission) currency-check op=meta fan-out    ≤ replms
  + (step 3)    durable stage                     local I/O
  + (step 4)    replicate to the ackers           ≤ replms
  + (step 5a)   stale-mark round trip to monitor  ≤ replms
  + (step 6)    durable commit                    local I/O
  + lock wait behind the queue of earlier operations on the same
    object (each itself bounded by the terms above)

so **≤ 3·`replms` plus local I/O, plus the queue ahead of it on the
same object**. Every term is named and bounded, and the only
unbounded work in the write path — the currency check's pull of a
winning copy from elsewhere, up to `objmax` — is explicitly *not*
inside a client request: the request answers `not ready` and the pull
runs in the background (§5.2). With defaults that is a 3 s worst case
against a 1 ms LAN, and the common case is one `replms` term at most.
The multi-second *exposure* of a dead peer is a retry window the
client library manages, not a wedged 9P request: by `deadms` the
monitor has published `up=no`, `C(o)` shrinks, and the retry proceeds
normally.

**`mincopies` unchanged in meaning.** With `mincopies=1` a write can
be acked onto a single disk. What differs from the first draft is
that the fact is durably known outside that disk before the ack
(§7.1, step 5a) — including the case that draft missed, where the
other placement member is `up=no` and takes nothing. So if the acking
disk then dies permanently, the surviving peer does not silently
serve over the writes it never saw: a mark names it as subject, and
either the heal gate holds it at `up=heal` (§7.3) or — if it stayed
`up=yes`, which is the common case after a mere timeout — the dead
disk's unresolved mark fails the currency check for the affected
objects (§5.2). Either way the answer is a retryable refusal until an
operator runs `forcesync` and accepts the loss in the log. That is
availability traded for an honest answer, and it is a product call,
§10.3(b2). §5.7 works the two cases through.

### 5.4.1 Concurrency and Tflush

*Normative; the 9front mechanics behind it are why.*

- An instance MUST serve requests on one connection **concurrently**:
  a request blocked in step 4 MUST NOT block requests on other
  objects, or reads of `/status`, `/ctl` or `/map`. In `lib9p` terms
  this means a `Srv` with per-request worker procs, not the default
  single-threaded loop, which serialises all traffic on the
  connection.
- A `Tflush` naming a pending object operation MUST be answered with
  `Rflush`. `devmnt` sends `Tflush` on interrupt and waits for
  `Rflush`; a server that never answers leaves the client process
  wedged and unkillable. `lib9p` requires `Srv.flush` for any slow
  request; implementing it is not optional here.
- On flush the instance MUST abandon the client-visible request
  (answer `Rflush`, no `Rwrite`, no `Rerror`) and MUST then perform
  **the whole of step 7**: discard the staged update, release the
  per-object lock, clear the per-(object, peer) sync state for every
  candidate involved, invalidate `cur` for `o`, and re-run the
  currency check before serving `o` again. The only difference from
  step 7 is that no error is returned to the client. The replication
  attempt already in flight MAY complete, which is exactly why the
  cleanup is not optional: a peer that durably committed
  `(E, ver+1)` while the primary discarded its stage leaves the
  primary holding `(E, ver)` with stale sync state and a `cur` it has
  no right to. Without the re-check the primary would later re-issue
  `(E, ver+1)` over different content and violate I3 — the same
  defect as a crash between steps 4 and 6 (§5.2's restart rule).
- The per-object lock MUST NOT be held across a client's think time:
  it is taken in step 2 and released in step 6 or 7, never spanning
  more than the sum bounded above.

### 5.5 The `/repl` push channel

`/repl` is a **single file**, not a directory: a sender opens one
fid per peer and streams operations for many objects through it,
which makes bulk heal cheap (no walk per object). The write offset
is ignored. Each `Twrite` MUST be exactly one operation: a header
line — **one physical line**, §0 — followed, where a payload is
defined, by exactly `n` bytes in the same `Twrite`.

    op=create oid=<oid> epoch=<u64> ver=<u64> wepoch=<u64>
              csum=<hex64>
    op=write  oid=<oid> epoch=<u64> ver=<u64> wepoch=<u64>
              pver=<u64> pwepoch=<u64> off=<u64> n=<u32>
              dcsum=<hex32> csum=<hex64>
    op=trunc  oid=<oid> epoch=<u64> ver=<u64> wepoch=<u64>
              pver=<u64> pwepoch=<u64> len=<u64> csum=<hex64>
    op=delete oid=<oid> epoch=<u64> ver=<u64> wepoch=<u64>
              csum=<hex64>
    op=full   oid=<oid> epoch=<u64> ver=<u64> wepoch=<u64>
              len=<u64> off=<u64> n=<u32> dcsum=<hex32>
              csum=<hex64> final=<0|1> [force=<0|1>]

- `epoch=` is the **sender's** current map epoch and is what fences
  cross-epoch replication. Carrying it per operation, rather than in
  the attach, is what lets one `/repl` fid live across epoch bumps;
  the first draft's attach-epoch coupling would have failed every
  replicated write after the first bump, cluster-wide, until
  something forced a re-attach.
- `csum=` is the object checksum (§1.4, `hex64`) the object MUST
  have **after** the operation is applied. The receiver MUST compute
  its own and MUST fail with `checksum mismatch` if they differ.
  This is the check that catches divergence at the moment it would
  be created.
- `dcsum=` is BLAKE2s-128 (32 hex characters) over the payload bytes
  only — an integrity check on the transfer, a different function
  over different bytes from `csum`, which is why it has a different
  name.
- `op=full` carries a whole-object resync in as many chunks as
  needed; the receiver stages them and commits the object only when
  `final=1` arrives, so a resync is never half-applied. `len=` is
  the final object length and MUST be identical on every chunk.
- `force=` defaults to `0` and MUST be identical on every chunk of
  one object's transfer. `force=1` is the **divergence repair** of
  §1.3: it applies when the sender's key is *equal to* the
  receiver's and the `csum` differs, as well as when it is greater.
  It MUST NOT apply when the sender's key is lower — that is
  `stale version` regardless of `force`. A sender MUST NOT set
  `force=1` unless it is the serving primary for the object under
  its own current map and is executing §1.3's procedure after both
  copies verified; a receiver that applies one MUST record the event
  in `/lost` as a divergence, so the count of times an invariant was
  repaired is visible rather than silent.

  Why plain `op=full` refuses an equal key: equal keys are supposed
  to *imply* equal content (I3), so applying would be work with no
  effect, and refusing is what keeps two instances that hold the same
  version from pushing identical copies at each other forever. That
  makes the equal-key/differing-`csum` case a bug by construction —
  which is precisely why repairing it needs a flag the ordinary
  replication path never sets, and an audit record when it is used.
  Without the flag §1.3's rule was unimplementable: the primary's
  `op=full` came back `stale version` and the divergence could never
  be repaired.

Receiver rules:

- The attach MUST be `role=repl` with `peer=`; any other role MUST
  fail with `permission denied` (not `bad open mode`, which §2.6
  defines as an open-mode error).
- Epoch: if `epoch` < the receiver's current map epoch, fail
  `stale epoch` — this is what stops a fenced-era primary from
  writing. If `epoch` > the receiver's, fail `future epoch` and
  fetch the map immediately; the sender retries.
- Verify `dcsum` over the payload; mismatch → `checksum mismatch`.
- **Delta ops** (`op=write`, `op=trunc`): apply iff the local
  committed key equals `(pwepoch, pver)` exactly; else
  `out of sequence` (§5.3).
- **Self-contained ops** (`op=create`, `op=delete`, `op=full`):
  apply iff `(wepoch, ver)` is greater than the local key, or the
  receiver holds no copy; else `stale version`. The one exception is
  `op=full force=1` below. For a multi-chunk `op=full` the comparison
  is made **once, at `final=1` commit time, against the receiver's
  then-current key** — earlier chunks stage without comparing, and a
  concurrent local update between chunks is what the commit-time
  check exists to catch.
- Adopt `ver`/`wepoch` verbatim; never invent either.
- **Commit before replying.** The reply to the `Twrite` (or to the
  `final=1` chunk of an `op=full`) MUST NOT be sent until the update
  is durable locally and atomically visible (§1.3). A reply is a
  durability claim; §5.4 depends on it.
- The receiver MUST NOT require that it be a member of `P(oid)`
  under its own map; during rebalance the sender's map is
  legitimately ahead.
- `Rwrite` count is the full bytes written, or an `Rerror`. There is
  no partial application.

Summary of the channel, so that every operation defined in this
design appears exactly once in a table with its role and its errors:

| Op | Role | Payload | Errors beyond the common set |
|---|---|---|---|
| `op=create` | repl | none | `stale version` (never `object exists`: a replicated create is self-contained and arbitrates) |
| `op=write` | repl | `n` bytes | `out of sequence`, `object too large` |
| `op=trunc` | repl | none | `out of sequence`, `object too large` |
| `op=delete` | repl | none | `stale version` |
| `op=full` | repl | `n` bytes | `stale version` (unless `force=1` and keys are equal), `object too large` |

The common set, which any `/repl` operation may return, is
`permission denied` (wrong role), `stale epoch`, `future epoch`,
`fenced`, `checksum mismatch` (payload `dcsum` or resulting `csum`),
`bad object name`, `bad ctl` (malformed header) and `disk full`.

**Ordering and pipelining.** 9P does not order concurrent tags, so:

- At most **one outstanding operation per object per `/repl` fid**.
  Two concurrent deltas for one object could arrive out of order,
  and the second would be rejected `out of sequence` and dropped;
  the predecessor rule makes that safe but useless.
- `op=full` chunks for one object MAY be pipelined, because each
  carries an explicit `off=` and they are staged, not applied. The
  `final=1` chunk MUST NOT be sent until every earlier chunk of that
  object has been acknowledged.

**Message sizing.** An instance MUST negotiate `msize` ≥ 8192 +
`IOHDRSZ` and SHOULD offer ≥ 65536 + `IOHDRSZ`; it MUST refuse to
operate below the floor rather than halving cluster throughput
silently, and MUST report its negotiated `msize` in `/status` so an
operator can see the real chunk size. A primary MUST size a
forwarded write to fit the smallest peer `msize` less the header and
`IOHDRSZ`, and MUST use a short `Rwrite` to the client for anything
larger — that is why §2.4 mandates short-write handling. The longest
header defined above is under 512 bytes.

**Resync arithmetic, corrected.** A 16 MiB `op=full` at a 64 KiB
payload is 256 messages; at the 8 KiB floor it is 2048. The first
draft's "~150 ms" counted wire time only (16 MiB at 1 Gb/s ≈ 134
ms). With one object's chunks pipelined the per-message RTT
overlaps, so ~150 ms is right at 64 KiB; **strictly serialised**, at
a 0.2 ms LAN RTT, it is 134 ms + 256·0.2 ms ≈ 185 ms at 64 KiB and
134 ms + 2048·0.2 ms ≈ 545 ms at the 8 KiB floor. Sizing decisions
that rest on this belong in §10.2.

### 5.6 The `/rpc` request/response channel

Reconcile, `pull`, currency checks and tombstone confirmation all
need to *ask* — the first draft's five `/repl` operations are all
push-shaped, so `pull` and "collect `(wepoch, ver, csum, len)` from
every member" invoked wire operations that did not exist. `/rpc`
supplies them, in the idiom of factotum's `/mnt/factotum/rpc`: write
a request, read the response on the same fid.

- `role=repl` or `role=admin`. **At most one outstanding request per
  fid**; a second `Twrite` before the response is read MUST fail
  with `bad ctl`.
- The fid MUST be opened `ORDWR`; an `OREAD` or `OWRITE` open MUST
  fail with `bad open mode`, since every exchange uses both
  directions. `ORCLOSE` is refused as everywhere else.
- **Offsets are ignored in both directions.** The `Twrite` offset is
  ignored, as on `/ctl`; the `Tread` offset is ignored, and a read
  always returns the buffered response from its start. `/rpc` is a
  channel, not a file, and honouring offsets would invite a caller to
  seek within a response that no longer exists.
- The server MUST **prepare the complete response at `Twrite` time**,
  under whatever lock makes it consistent, and buffer it. Request
  errors are `Rerror` to the `Twrite`. The response is then
  delivered by exactly one `Tread`; the caller MUST offer a read of
  at least the negotiated `msize` − `IOHDRSZ`. Preparing at write
  time is what makes the response atomic with respect to concurrent
  object writes — the defect that made "read `/obj` and `/meta`
  separately" unusable.
- **Response lifetime.** A buffered response is destroyed by the
  next `Twrite` on that fid and by the `Tclunk`, and by nothing else
  — no timeout. A `Tread` when no response is buffered returns count
  0; a `Tread` after the response has been delivered returns count 0
  (it is not an error, so a caller that reads twice sees end of
  data). A fid may therefore be reused for request after request,
  which is the point of a channel.
- Requests and responses are one physical line, plus a payload where
  defined.

Requests:

    op=meta    oid=<oid> epoch=<u64>
    op=get     oid=<oid> epoch=<u64> ver=<u64> wepoch=<u64>
               off=<u64> n=<u32>
    op=list    epoch=<u64> [after=<oid>] [n=<u32>]
    op=drop    oid=<oid> epoch=<u64>
    op=verify  oid=<oid> epoch=<u64>
    op=discard oid=<oid> epoch=<u64> ver=<u64> wepoch=<u64>

Responses:

    meta oid=<oid> ver=<u64> wepoch=<u64> csum=<hex64> len=<u64>
         state=live|tomb cur=<u64>
    meta oid=<oid> absent=1
    get  oid=<oid> ver=<u64> wepoch=<u64> off=<u64> n=<u32>
         dcsum=<hex32>                      followed by n bytes
    list lines=<u32> more=<0|1>      followed by lines advert lines
    ok   op=drop|verify|discard oid=<oid>

- `op=meta` is the primitive behind currency checks (§5.2),
  tombstone confirmation (§1.5) and arbitration (§7.2). `absent=1`
  is a positive statement that the instance holds no copy, which is
  what tombstone confirmation needs. An instance answers `op=meta`
  whatever its own `up`/`status` (§6.4 F3), which is what keeps
  §1.5's confirmation and §7.4's evacuation reachable.
- `op=get` carries the **expected key**. If the object's committed
  key is not exactly `(wepoch, ver)`, the server MUST fail with
  `stale version`; the puller re-reads `op=meta` and retries. That
  is how a multi-chunk pull stays consistent without holding a lock
  across the transfer: any concurrent write aborts it. `n` MUST fit
  the negotiated `msize`.
- `op=list` pages the instance's full inventory — `live` and `tomb`
  — in `oid` byte order, resuming after `after=`. It is the peer
  enumeration path and does not depend on directory-read semantics
  (§2.2). A page MUST be internally consistent; pages are not a
  cluster-wide snapshot, and a reconcile pass MUST tolerate an
  object created or deleted between pages (it will be caught by the
  next pass or by an `/advert`). The response count field is
  `lines=`, counting **advert lines**, not payload bytes — `n=`
  elsewhere in this design always counts bytes, and one name for two
  units is how off-by-one bugs are written. The requested `n=` is a
  maximum; the server MUST clamp it so that the whole response fits
  the negotiated `msize` less `IOHDRSZ`, and MUST set `more=1` when
  it clamped or when more inventory follows.
- `op=drop` is the wire form of §7.4's drop guard: the serving
  primary tells a stray holder to delete its copy with no tombstone.
  The receiver MUST re-check, against its **own** current map, that
  it is not in `P(oid)`, and MUST answer `still placed` if it is.
- `op=verify` makes the receiver re-hash the object now and compare
  against its stored `csum` (§7.5), answering `ok` or
  `checksum mismatch`. It is what §1.3's equal-key/differing-`csum`
  procedure calls for when it says "make both holders re-verify".
- `op=discard` is §1.5's tombstone discard. The receiver applies the
  two local checks §1.5 defines and answers `ok` or
  `not discardable`.
- `epoch=` is checked exactly as on `/repl`. One consequence worth
  stating because implementers will see it: immediately after a bump,
  the first `op=meta` a lagging instance receives fails
  `future epoch`, which makes it fetch the map at once and the caller
  retry. So the first post-bump touch of an object commonly costs one
  extra round trip. It is self-healing, bounded by one fetch per
  instance per epoch, and not a defect.

The channel in one table, roles and errors together:

| Op | Role | Answers | Errors beyond the common set |
|---|---|---|---|
| `op=meta` | repl, admin | `meta` | — |
| `op=get` | repl, admin | `get` + payload | `no such object`, `object deleted`, `stale version` |
| `op=list` | repl, admin | `list` + lines | — |
| `op=drop` | repl, admin | `ok` | `no such object`, `still placed` |
| `op=verify` | repl, admin | `ok` | `no such object`, `checksum mismatch` |
| `op=discard` | repl, admin | `ok` | `no such object`, `not discardable` |

Common set for `/rpc`: `permission denied`, `stale epoch`,
`future epoch`, `fenced`, `bad ctl` (malformed request, or a second
request before the response was read) and `bad object name`.

### 5.7 Invariants

The first draft asserted:

> **I1 (as drafted).** For every object, every instance whose map
> state is `up=yes` holds every write that has been acked at any
> epoch.

That is false under the redesign and was false under the draft: an
instance can be `up=yes`, healthy, and newly placed on an object it
has never held. It is also stronger than the system needs — nothing
reads from a non-primary. It is replaced by three narrower
invariants that are actually maintained.

> **I1 (currency).** The instance serving `role=client` operations
> on object `o` at epoch `E` holds every write acked for `o` at any
> epoch, in the order the acking primaries assigned.

> **I2 (no phantom keys).** No instance ever exposes — in `/meta`,
> `/obj`, `/advert`, `/tombs`, `op=meta`, `op=get` or arbitration —
> a key whose content and checksum it has not durably committed.

> **I3 (key determines content).** Two copies of `o` with equal
> `(wepoch, ver)` have equal content.

**Why they hold.**

- I2 is §5.4 step 3 (stage invisibly), step 6 (commit atomically),
  and §1.3's local-atomicity rule. A crash cannot expose a key
  without its content.
- I3 is §5.3's delta rule (a delta applies only onto its exact
  predecessor) plus §5.5's `csum=` check on every replicated
  operation (the receiver recomputes and refuses on mismatch) plus
  §1.3's equal-key/differing-`csum` procedure, which treats a
  violation as a bug to surface rather than a race to break.
- I1 is the composition of six rules. Read them as a chain: every
  way an instance can come to serve `o` is covered by one of them.
  1. *Ack discipline* (§5.4): a write is acked only after the
     primary and every acker have durably committed it, and the
     primary commits last, so an acked write exists on ≥ `mincopies`
     copies and the primary is always one of them.
  2. *Delta safety* (§5.3): an acker's copy is byte-identical to the
     primary's at the key it reports, so promoting an acker cannot
     serve a hole.
  3. *Durable staleness* (§5.4 step 5, §7.1): **every member of
     `P(o)` other than the primary that did not durably commit the
     write** — a candidate that failed, and equally a member that is
     `up=no` and was sent nothing — is registered stale with the
     monitor before the ack, and a marked instance is never promoted
     past `up=heal` (§8.3), so it never becomes a serving primary
     while it might be behind. The quantifier is the whole content of
     this rule: over candidates only, the commonest degraded write in
     the system recorded nothing, and the peer that missed 41 writes
     was promoted over them. It survives the death of the primary
     that recorded it, because the mark lives at the monitor and
     travels in the map (§3.1).
  4. *Heal completeness* (§7.3): an instance entering `up=heal` sets
     the `fullsync` flag at every other instance, so no `synced` can
     be sent for it until an actual reconcile pass has run against
     it. Gate (b) is then a statement about work done, not about an
     empty data structure that was never populated.
  5. *Handoff* (§5.2): an instance that was not serving primary at
     the previous epoch does not serve until the deposed primary has
     provably stood down or self-fenced, and until a currency check
     — run *after* that grace expired, over the whole witness set,
     and re-run after every restart — has arbitrated. Rules 3 and 5
     meet here: the witness set keeps a down reporter in scope for
     the objects its marks concern, and §7.4 keeps placement from
     drifting away from those marks while they are outstanding.
  6. *Fencing* (§6.4): F1's lease, F2's ordering, and F3's
     self-check, with the monitor's demotion evidence tied to the
     refresh channel (§8.4) — the only automatic demotion evidence
     there is — so the lease and the demotion are measured on the
     same connection.

**What I1 does not promise, stated plainly.**

- With `mincopies=1` a write can be acked onto a single disk. If
  that disk is permanently lost, the write is lost, and what the
  system does about it depends on which peer is which. Stated
  exactly, because the previous revision told only the tidy half of
  the story:
  - The peer that missed the writes is the **subject** of the mark
    the acking primary registered before the ack (step 5a — which
    now fires whether the peer merely timed out or was `up=no`).
    What holds it back depends on where it is. If it is `up=no`, or
    returns to `up=heal`, gate (a) refuses to promote it while the
    mark is unresolved. But if it merely timed out at `replms` it is
    still **`up=yes`** — nothing in this design demotes an instance
    for failing to take one write (§8.4) — and it may become the
    serving primary the instant the acking disk dies. In that, the
    common, case the heal gate is not what protects the data: §5.2
    is. The dead disk is in `P(o)` and is the reporter of an
    unresolved mark whose subject is this peer, so every currency
    check for those objects fails and they answer `not ready`. Either
    way the client gets a retryable refusal rather than a rolled-back
    write, but the two paths have different blast radii and different
    exits, and the previous revision told only the first one.
  - The dead disk is the **reporter**. Its marks are unresolvable by
    anyone else, so they stay outstanding, which by §5.2 clause 4
    fails the currency check for every object whose placement
    contains one of its subjects — about `1/|V|` of the cluster, not
    one object and not all of them.
  - Recovery is an operator running `forcesync` (§8.3), an explicit
    logged acknowledgement that acked writes are being discarded, or
    `retire`ing the dead reporter, which does the same thing and
    MUST be logged the same way.
  The alternative — promoting the peer silently — is what the first
  draft did, and it rolled acked writes back with no record anywhere.
- Five operator actions knowingly break I1 and MUST each be logged
  as a data-loss event: `promote <iid> force`, `forcesync`,
  `commit force` on a placement change while an instance with
  unresolved marks is unreachable (§7.4, §8.3), `forceepoch` (§8.6),
  and `retire` of an instance that is the **reporter** of an
  unresolved mark (§7.1), which discards its acked writes exactly as
  `forcesync` does and was previously listed in neither place.
  §10.1(7) carries the same five.
- I1 says nothing about a *failed or flushed* operation: see §5.4.
  Such an operation may or may not have been applied, and Layers B
  and C must handle that.
- I1 is about the serving primary only. Non-primaries may lag
  arbitrarily; nothing reads them.

## 6. Epoch and fencing

*Normative.*

### 6.1 The epoch

`epoch` is a u64 in the map header, assigned only by the monitor,
strictly increasing, bumped on any change to membership, `status`,
`up`, `fenced`, a mutable header attribute, **or the set of `stale`
records** (§3.1), and at least once every `tombdays`/2 regardless
(§1.5). An epoch bump is cheap: every party re-reads a small text
file and invalidates its `cur` marks. No epoch bump by itself moves
data; only a `status` change does.

**Why mark changes bump the epoch now.** The previous revision
exempted them, on the grounds that "making every degraded write bump
the epoch would be ruinous". That objection does not survive the
ledger moving into the map. Marks are per ordered pair of instances
and registered **once per divergence episode** (§5.4 step 5a, §7.1),
not once per write: at ≤12 instances the ledger has at most 132
possible marks, so register/resolve traffic is bounded by pair count
and by how often links actually break, not by write rate. A busy
primary writing a million times to a down peer registers exactly one
mark and bumps exactly one epoch. Against that bounded cost, exempting
mark changes would mean the ledger and the map could disagree about
which epoch they describe — and §5.2's witness rule reads both in the
same breath. One version number for one document is worth more than
the bumps it costs.

### 6.2 Where the epoch is checked

The first draft checked the fid's attach epoch on **every**
operation, which made every epoch bump — including one caused by a
single peer timeout — tear down every fid held by every client to
every instance at the worst possible moment, and would tax the
future direct client→storage path with thousands of stripe-object
fids. That check is gone. What replaces it:

- **At `Tattach`** (§2.1), `epoch` is compared: less → `stale
  epoch`; greater → `future epoch` and the instance fetches the map.
  This gives a client an early, cheap signal that it is stale, and
  gives an instance a signal that it is behind.
- **Per operation**, the authority is the instance's own current
  map: fenced state (F1), self-state (F3), serving primaryship,
  handoff grace and currency (§5.2). A client's opinion of the epoch
  is not consulted, because it is not authoritative for any of them.
- **On `/repl` and `/rpc`**, the epoch is carried per operation
  (§5.5) and compared per operation. This is the cross-epoch fence
  that actually matters, and it is now independent of fid lifetime.
- `Tclunk` MUST always succeed; `Tstat` and `Tremove` are never
  refused for epoch reasons (`Tremove` clunks the fid regardless).

**Does anything F1 fenced now escape?** The three hazards the
per-operation check was credited with:

1. *A deposed primary serving reads at an old epoch.* Closed by F1
   (it self-fences if it cannot refresh) and F3 (if it can refresh,
   it learns `up=no`/`status=out` for itself and refuses with
   `down`), and by the per-operation primaryship check against its
   own map. The client's epoch was never what closed this.
2. *A client at an old epoch writing to the wrong instance.* The
   instance evaluates primaryship against its own current map and
   answers `not primary` with the correct iid. A stale client is
   redirected, not obeyed.
3. *A client at an old epoch writing to an instance that is still
   the right primary.* Harmless — the instance is authoritative and
   current. Refusing it bought nothing.

The one thing the per-operation check did buy is the *window*
between an epoch being published and the old primary noticing it —
at most `pollms`. That window is now closed on the other side, by
the incoming primary's handoff grace (`leasems` > `pollms`, §5.2),
which is a stronger guarantee because it does not depend on the
client having a fresh map either.

A consequence worth stating: because attaches now survive epoch
bumps, a plain `mount` of a storage instance no longer breaks at the
next bump. Layer A is still consumed through a client library — for
map polling, primary redirection and retryable-error handling — but
"stock 9P" no longer carries a hidden asterisk about fid lifetime.
§10.1 is updated accordingly.

### 6.3 Map propagation without server-initiated messages

9P has no server→client message, so propagation is pull-only, in
two mutually reinforcing modes:

- **Refresh.** Every party — storage instances, the MDS, any client
  library — reads the monitor's `/map` every `pollms` (default 1 s).
  Steady-state staleness is bounded by `pollms`. A storage
  instance's refresh is **normative and dual-purpose**: it is also
  the sole liveness evidence the monitor may act on (§8.4). Because
  the stale ledger travels inside the map (§3.1), one refresh
  delivers placement and staleness together, at one epoch; there is
  no second fetch and no second freshness rule.
- **Fetch on rejection.** Any `stale epoch` or `future epoch`
  triggers an immediate fetch by the party that is behind. This is
  what makes correctness independent of `pollms`: the interval
  affects only how often work is wasted, never whether a stale party
  can act.

A monitor MUST answer a read of `/map` without requiring an epoch,
otherwise nobody could learn the epoch. A storage instance MUST
attach to the monitor with `role=instance,peer=<iid>` (§8.1) so its
refreshes are attributable; other parties attach as readers and
their reads are evidence of nothing.

**Epoch regression.** An instance MUST reject a map whose `epoch` is
lower than the epoch it currently holds: it MUST NOT adopt it, MUST
NOT treat the read as a successful refresh for F1 purposes, and MUST
report the condition in `/status` (`epochregress=yes`). A monitor
rebuilt from a lagging backup therefore leaves the cluster fenced
rather than splitting its brain, and the operator resolves it with
`forceepoch` (§8.6). The first draft left this undefined, where
either choice — accept or reject — was unsafe by default.

**Monitor identity.** The map header carries `monid=` (§3.2), a
random 128-bit value generated once when the cluster is created and
never changed. An instance caches the `monid` of the first map it
ever adopts and MUST refuse — not adopt, not count as a refresh —
any map whose `monid` differs, reporting `monidmismatch=yes` in
`/status`. An instance that has never adopted a map accepts whatever
`monid` its first map carries.

This is a tripwire, not consensus. Epoch regression alone does not
catch the dangerous case: a second monitor started while the first is
still running, at an epoch *above* the first, is accepted by every
rule in this design and quietly becomes a second authority — and
because §8.6's rebuild procedure exists to be run when the old
monitor is unreachable-but-possibly-alive, the design itself
advertises the way to create one. Two monitors that share a `monid`
remain operator error and this rule does not detect them (§8.6 states
the consequences); what it detects is the far commoner accident of a
freshly created monitor — a new cluster's map, a mistyped path to an
empty state directory — being pointed at a live cluster. The escape
hatch is deliberate and narrow: `forceepoch <e> monid=<hex>` (§8.3)
sets both at once and is logged, and it is the only way a
cluster's `monid` ever changes. The refusal rule above would leave
every existing instance permanently fenced against the new value, so
the escape has an instance-side half: the operator writes
`newmonid <hex>` (§2.5, logged, available while fenced) at each
instance, replacing its pinned value so the next refresh can adopt
the forceepoch'd map. That the escape takes one logged write **per
instance** is deliberate — re-identifying the cluster's authority
should not be one keystroke.

### 6.4 Fencing a deposed primary

The hazard: instance X is primary at epoch 41; the monitor cannot
reach X, publishes 42 with `up=no` for X, and Y takes over. X is
alive and partitioned from the monitor but reachable by a client
still at epoch 41. Without fencing, X would ack writes that Y never
sees.

Rules:

- **F1.** An instance MUST successfully refresh its map from the
  monitor at least every `leasems`. If `leasems` elapses with no
  successful refresh, it MUST enter the **fenced** state and fail
  every `role=client` read and write, every `role=repl` and
  `role=admin` read of an object through `/obj` or `/meta`, every
  `/repl` and `/rpc` operation, and every data- or
  replication-mutating ctl verb (§2.5), with `fenced`, until a
  refresh succeeds. Reads are fenced too: a deposed primary serving
  reads at an old epoch is exactly the D2 violation we are
  preventing, and `/obj` under `role=repl` serves the same bytes as
  `op=get` — fencing one path and not the other would be a hole with
  a different name. A read of a reserved `shoal.` object by
  `role=admin` is the sole exemption (§2.1, §8.6).
- **F2.** The monitor MUST NOT publish `up=no fenced=yes` for an
  instance until that instance's **refresh channel** has been silent
  for `deadms` (§8.4), and `deadms` MUST be greater than `leasems`.
  A freshly started monitor MUST wait `deadms` after start before
  publishing any map that demotes an instance, because it has no
  history.
- **F3.** An instance whose own map says `up=no` or `status=out` for
  itself MUST refuse `role=client` I/O with `down`, even though it
  is manifestly reachable, and MUST NOT act as an acker, until the
  map says otherwise. A client that receives `down` re-reads the map
  and re-evaluates placement, exactly as for `not primary` but with
  no iid to redirect to (§0).

  **What F3 does not bar.** It MUST NOT stop the instance answering
  `op=meta`, `op=get`, `op=list` or `op=verify`, accepting an
  incoming `op=full`/`op=delete` push, or completing a pull or push
  it is the source of. Those are how data leaves a disk that is on
  its way out, and how §1.5's cluster-wide tombstone confirmation
  reaches an instance that is `out`. The previous wording — "MUST NOT
  act as an acker **or a reconcile source**" — made `disable`, the
  evacuation verb, self-defeating: the disk keeps its objects as
  strays, the new primaries must `op=get` them from it, and the rule
  forbade exactly that, stranding the data until a `retire` declared
  it lost. F3 is about *serving clients and taking acks*, which is
  what deposing an instance is for; it is not a quarantine.

  The carve-out applies only to an instance the map still counts. An
  instance whose own current map shows `status=dead` for itself, or
  no record for itself at all, MUST NOT advertise, push, answer any
  `/rpc` request, or attach `role=repl` anywhere — it is not a
  member, and a "conforming zombie" that kept those paths would
  resurrect discarded deletes (§1.5). Receivers close the other
  half: an instance MUST refuse, with `permission denied`, any
  `role=repl` attach whose `peer=` is not a non-`dead` instance in
  its own current map.

  `up=heal` is deliberately not in F3's list. A `heal` instance
  legitimately takes replication and answers queries, and it can
  never serve clients anyway: §4.3 selects the serving primary as the
  first `up=yes` member of `P(o)`, so `heal` fails the primaryship
  test before F3 is ever consulted.
- **F4.** An operator fence (`fence on`, §2.5) is a separate flag
  with the same effect as F1's; `fence off` clears only that flag and
  MUST NOT clear a lease-derived fence. `/status` reports which are
  in force (`fence=none|lease|operator|both`).

F1 + F2 give: X self-fences at `t0 + leasems`, where `t0` is its
last successful refresh; the earliest the monitor can depose it on
lease evidence is `t0 + deadms > t0 + leasems`. That is why
`fenced=yes` may be published with a lease demotion and why §5.2's
grace may be skipped for it. Operator demotions (`disable`) carry
`fenced=no` precisely because this argument does not apply to them:
the instance is alive, refreshing and unfenced, so the incoming
primary must serve out the full grace.

Assumptions, stated honestly: bounded clock *rate* drift on each
node (no clock synchronisation is required — only elapsed-time
measurement), and a monitor RTT small against `deadms − leasems`.
With defaults that margin is 7 s against a sub-ms LAN.

### 6.5 Single monitor: what it costs

v1 runs one monitor. The consequences must be said plainly:

- No monitor → no epoch bumps → no failover, no rebalance, no
  re-enable, and no way to register a stale mark, so degraded writes
  fail `degraded` rather than proceeding (§5.4 step 5a).
- No monitor → after `leasems` (3 s) every instance self-fences and
  **the cluster stops serving reads and writes**.

There is no safe way to soften F1: an instance cannot distinguish
"the monitor is down" from "I am partitioned and have been
deposed". Cluster unavailability from monitor loss therefore equals
monitor restart time. The mitigations are operational, not
architectural: the map — the stale ledger inside it — is one small
plain file, made durable before every ack by whatever mechanism §8.2
settles on, and mirrored into the object store
(§8.2), so a monitor restarts anywhere in seconds at the same epoch
and un-fences everyone — and §8.6 makes that path actually
executable, which in the first draft it was not. A replicated
monitor is the fix and is deliberately deferred (§8.7, §10.3(b1)).

## 7. Heal, scrub, and rebalance

*Normative: the two staleness records, the arbitration rule, the
advertise/reconcile protocol, the heal gate, and the drop rule.
Implementation policy: scheduling, rate limits, parallelism,
partial-block repair.*

### 7.1 Two records of staleness, at two scopes

The first draft had one: a local, fine-grained dirty set, which died
with the disk that held it. There are now two, with different owners
and different jobs.

**The dirty set** — local, fine-grained, an efficiency structure.
Durable records `(oid, peer-iid, epoch)`, written before the ack
that created them (§5.4 step 5b), readable at `/dirty`. It is
bounded: if it exceeds an implementation limit the instance MAY
discard the fine-grained records for a peer and mark that peer
`fullsync`, meaning "reconcile every object I hold against this peer
before I may claim to be synced with it". `forget <iid>` does this
by hand, and §7.3 step 1 sets the same flag on every instance when a
peer enters `up=heal` — the coarse flag is how "I have nothing
recorded for you" is kept distinct from "I have checked". This bound
is why D4's whole-object resync is enough and no per-write log is
needed.

An instance MUST work its dirty records for a peer whenever that
peer is reachable, **regardless of whether it is still the serving
primary** for the objects concerned. A record is resolved by making
the peer's key for that object ≥ the local key: `op=meta`, then
`op=full` if the peer is behind, then clear. Tying the obligation to
the holder rather than to current primaryship is what stops a
primaryship change from orphaning records with nobody responsible
for them.

**The stale ledger** — owned by the monitor, coarse, durable, the
thing that must survive the reporter's death, and **published inside
the cluster map** (§3.1). One mark per ordered pair:

    stale=<subject-iid> reporter=<iid> since=<epoch>

meaning "instance `reporter` has acked at least one write that the
subject did not take". The kind's value is the subject because that
is the field consumers index on — the heal gate asks "is anything
marked against this subject?" and §5.2 asks "is this subject in
`P(o)`?". At ≤12 instances the ledger is at most 132
marks; it is a bounded, persisted, per-peer-pair structure, not
per-object state. It is registered synchronously by the reporter
**before** its first degraded ack for that pair (§5.4 step 5a) —
one extra monitor round trip, bounded by `replms`, on the first
divergence of an episode, and none thereafter: a reporter MUST NOT
re-register a mark for a pair that is already registered and
unresolved. That is what makes the cost per *episode* rather than per
write, and it is what §6.1 relies on when it makes mark changes bump
the epoch.

Every instance therefore holds the whole ledger, as of its own
current map, for free. That matters because §5.2's witness set and
skip rule are evaluated against the ledger on every currency check by
every instance — the previous revision left the ledger monitor-only
and specified no way for an instance to read it, making the
load-bearing lemma of §5.2 unevaluable by the party required to
evaluate it. The monitor exposes `/stale` as a convenience view of
the same records, and each instance's `/stale` shows the marks it is
party to (§2.2).

A mark is resolved only by:

- `synced <subject>` from the reporter (§8.3), which the reporter
  MUST NOT send until its dirty set for that subject is empty and no
  `fullsync` flag for it remains; or
- `forcesync <subject> from=<reporter>` by an operator, which is an
  explicit acknowledgement that the reporter's data is gone and that
  acked writes are being discarded, and MUST be logged; or
- the reporter or the subject being `retire`d to `status=dead`
  (§3.3). For the **subject** the mark is moot: the disk is gone and
  nothing will be promoted. For the **reporter** this discards acked
  writes exactly as `forcesync` does, and the monitor MUST log it as
  a data-loss event in the same terms — it is the fifth member of
  §5.7's list of overrides that knowingly break I1, and it used to
  be the one nobody had written down.

It is **not** resolved by the reporter dying, by an epoch bump, by
`forget`, or by any other instance's opinion. That is the whole
point: the knowledge that a peer is behind is the one piece of
replication state the first draft never replicated.

**Why coarse.** Per-object monitor state is unbounded and would put
the monitor in the write path for every object. Per-pair state costs
one round trip per divergence episode and blocks the subject's
promotion until reconciliation completes, which is a disk scan.
The price is that a single stale object blocks a whole disk's return
to `up=yes`; at ≤12 nodes with R=2 that is the right trade, and it
is exactly the conservative direction. §10.3(b6) records the
question of whether a finer ledger is wanted.

### 7.2 Reconcile

After an epoch bump, and periodically, every instance scans its
**local** objects. Purely local scanning cannot discover objects
that *should* be here but are not, so the protocol is
advertisement-driven in one direction and query-driven in the other.

For each local object `o`, with `P(o)` at the current epoch:

- **If this instance is the serving primary of `o`:** run **full
  reconcile** — `op=meta` every member of the witness set `W(o)`
  (§5.2), arbitrate (§1.3), `op=get` the winner if it is not local,
  `op=full` it to every member of `P(o)` whose key differs, set
  `cur` to the current epoch, and clear the dirty records the
  operation resolved. This is the background sweep, so the pull is
  allowed here and forbidden only inside a client request; `cur` is
  set only if the pass obeyed §5.2's grace-ordering rule, and a pass
  that ran early is a pre-warm that leaves `cur` alone.
- **Otherwise:** `/advert` to the serving primary and let it
  arbitrate. This includes the case where this instance is not in
  `P(o)` at all — a **stray** left by a rebalance. Advertising is
  how a new primary that holds no copy learns the object exists.
  For pass-completion purposes this branch's item for `o` is
  complete only when the holder has obtained an `op=meta` response
  from the serving primary showing a key **≥ its own local key** —
  i.e. the primary demonstrably knows about a copy at least as new
  as this one. Until then the holder MUST re-advertise on each pass.
  Advert *sent* is not transfer *confirmed*, and `rebalanced` (below)
  certifies the latter.

`/advert` is a single file taking one line per object, one physical
line each. It is one-way: the receiver records and then initiates
pulls or drops itself; nothing needs to come back down the write. A
bulk advertisement of a whole disk is a few tens of megabytes and
SHOULD be rate-limited. The pull-shaped equivalent is `op=list`
(§5.6), which a primary uses when it wants an inventory now rather
than when the holder gets round to it.

    oid=<oid> ver=<u64> wepoch=<u64> csum=<hex64> len=<u64>
      state=live|tomb

An advert of `state=tomb` for an object the receiver has no record of
is handled by §1.5's missed-discard rule, not by silent adoption
plus silent re-discard.

**Reporting a pass complete.** An instance's reconcile pass for a
placement-changing epoch `e` is finished only when **every** local
object's item is complete under the rules above — for the
non-primary branch, primary-confirmed, not merely advertised. Only
then does it send `rebalanced epoch=<e>` to the monitor (§8.3). The
monitor uses those reports to refuse the *next* placement-changing
commit until the previous one has been worked through everywhere
(§7.4), which is what keeps §5.2's one-epoch placement memory
sufficient. Reports are monotone in `e`, like `synced` and `healed`.

The first draft additionally required reconcile to recompute
placement over the last `retain` maps. The redesign does not: the
witness set covers `P(o)` at the current and previous epoch, plus
whatever advertises, plus the reporters of in-scope unresolved marks
— and §5.2's lemma shows that suffices *given* the completion rule
above. Recomputing eight historical placement sets was real machinery
for a ≤12-node cluster where a bulk advert is strictly simpler.

### 7.3 Heal: returning an instance to `up=yes`

An instance that has been `up=no` re-enters at `up=heal` when the
monitor hears from it again. `up=heal` means: **in the ack-set
candidate list (§5.3), eligible to receive replication, never a
serving primary.**

The healer's obligation is to advertise everything it holds, so that
every primary can arbitrate against it and push what it is missing:

1. **Every other instance sets `fullsync` for the healer.** An
   instance that observes, in a map it adopts, that some subject has
   entered `up=heal` MUST set the `fullsync` flag for that subject in
   its own dirty set (§7.1) — meaning "reconcile every object I hold
   against this instance before I may claim to be synced with it".
   This is not an optimisation hint; it is what stops step 4's
   `synced` from being satisfiable by an empty data structure that
   was never populated. Without it, an
   instance that never wrote to the healer, or whose dirty records
   for the healer were lost with a dead primary, reports `synced`
   immediately and gate (b) opens on no work at all.
2. On entering `heal` at epoch `e`, the healer MUST run a complete
   `/advert` pass — every local object, `live` and `tomb` — to every
   reachable instance with `status` ∈ {`in`,`out`}.
3. Every serving primary reconciles its own objects against the
   healer as in §7.2, pushing `op=full` where the healer is behind
   or absent. Because the healer is in `P(o)` for exactly the
   objects that matter, and each of those has a primary that scans
   it, this covers the healer's whole obligation.
4. **Every** instance that is `up=yes` MUST send `synced <subject>`
   to the monitor once its dirty set for that subject is empty *and*
   its `fullsync` flag for that subject has been cleared by an actual
   reconcile pass. The obligation is edge-triggered on observing
   `up=heal` for the subject in an adopted map, and it applies to an
   instance that has never written to the subject and to one that
   holds no objects at all — such an instance clears its `fullsync`
   flag by running the (possibly trivial) pass and then reports. An
   instance promoted to `up=yes` *while* a heal is in progress
   inherits the obligation on its first refresh that shows the
   subject at `up=heal`, so joining the quantifier mid-heal cannot
   leave a silent hole.
5. The healer sends `healed epoch=<e>` when its advert pass is
   complete.

The monitor promotes `heal` → `yes` when **both**:

- **(a)** no unresolved stale mark names the instance as subject;
  and
- **(b)** every instance currently `up=yes` has reported
  `synced <subject>` at an epoch ≥ the epoch at which the subject
  entered `heal`, and the subject has reported `healed` at an epoch
  ≥ that epoch.

Reports are **monotone**: the monitor stores, per (reporter,
subject) pair, the highest epoch reported, and an unrelated epoch
bump does not invalidate it. The first draft required every primary
to report clean *at the current epoch* while the promotion itself
bumped the epoch — a livelock in which any unrelated bump reset the
gate and failover redundancy was silently never restored.

(a) is the correctness gate and survives the death of any reporter.
(b) is the completeness gate, and is deliberately quantified over
instances that are `up=yes` *now*, so a permanently dead instance
cannot deadlock it — the thing a dead instance can still block is
(a), which is the case where blocking is correct. With step 1 in
place, (b) now means "every live instance has looked", where before
it could mean "every live instance had nothing recorded".

### 7.4 Rebalance and stray deletion

A `status` change moves the placement of roughly `1/|V|` of objects.
There is no separate rebalance mechanism: the epoch bump invalidates
`cur` everywhere, triggers the §7.2 scan, adverts inform the new
holders, and the new primaries arbitrate and pull. Movement is
bounded by HRW's minimal-disruption property — only the arriving or
departing member's share moves. Client operations on objects whose
primaryship moved answer `not ready` for `leasems` plus one currency
check — plus, for the objects whose winning copy is not yet local,
the background pull that check schedules (§5.2) — which is the
honest, retryable cost of a rebalance.

**The drop guard.** A stray copy MUST NOT be deleted by its holder.
Only after the serving primary has confirmed that every member of
`P(o)` holds the current version does it send `op=drop oid=<oid>`
on `/rpc` (§5.6) to the stray holder — instance to instance, under
`role=repl`, which is why the operation exists there and not only as
the operator's `drop` ctl verb. The holder MUST verify, against its
own current map, that it is not in `P(o)`; if it is, it MUST answer
`still placed`. A stray that has never been dropped MUST be counted
in `/status` (`strays=`) and MUST NOT be reclaimed automatically.
Losing capacity to strays is recoverable; deleting the last copy is
not. §5.2's witness lemma depends on this rule, which is why it is
normative.

**Orphans.** An object whose holders have all drifted out of every
current and previous placement set, and which nothing advertises, is
findable only by a full-cluster inventory comparison — `op=list`
against every instance, or `reconcile` with no argument everywhere.
This is accepted; at ≤12 instances that comparison is an operator
tool, not a protocol. The first draft tied orphanhood to the
`retain` window; with reconcile no longer using historical maps, the
`retain` window is not what creates orphans — an advert that never
happens is.

**Placement changes while an instance is down.** The monitor MUST
refuse a `status`-changing `commit` while any instance is `up=no`
*and* named as the reporter of an unresolved stale mark, unless the
operator writes `commit force`, which MUST be logged as a data-loss
risk. Such a reporter may hold the only current copy of objects
whose placement is about to move, and no witness set can reach it.
Instances that are `up=no` with no unresolved marks are not blocked:
by §5.2's lemma they hold nothing uniquely current, so moving
placement without them is safe. (This is deliberately narrower than
"no placement changes while anything is down", which would forbid
exactly the case — evacuating a dead disk — that placement changes
exist for.)

**One placement change at a time.** The monitor MUST also refuse a
`status`-changing `commit` until **every** instance that is `up=yes`
has reported `rebalanced epoch=<e>` (§7.2) for the most recent
previous placement-changing epoch `e`. `commit force` overrides this
too, and MUST be logged.

The reason is §5.2's witness memory, which is exactly one epoch deep.
With two placement changes in quick succession — `P(o)` = [A,B] at
41, [C,D] at 42, [E,F] at 43 — the new primary at 43 has a witness
set of `P(o)@43 ∪ P(o)@42`, which contains neither A nor B; if the
adverts from A and B have not reached anybody yet, the check
*completes*, finds no holder, and licenses `no such object` for data
sitting on two healthy disks. A client that reacts by creating gets
`(43, 1)`, which outranks `(41, 50)`, and the real data is dropped as
an arbitration loser. The completion gate makes the second change
wait for the first to be worked through; §5.2's substitute-all
fallback covers the instance-local version of the same gap.

The trade is honest and worth stating: an operator who wants two
disks enabled must now wait for the first rebalance to be reported
complete, and a cluster with an instance stuck mid-pass cannot change
placement until it finishes, is demoted, or the operator forces the
commit. That is availability given up for the guarantee that a
currency check never completes on an empty witness set.

**An operator ordering trap, stated so nobody hits it.** The verbs
compose in one order and not the other. To retire an instance that is
`up=no` and is the reporter of unresolved marks — the dead-primary
case — the sequence is:

1. `forcesync <subject> from=<reporter>` for each of its outstanding
   marks (each logged as a data-loss event), **then**
2. `disable <reporter>` + `commit`, then `retire <reporter>` +
   `commit`.

Starting at step 2 does not work: `disable` is a `status` change, and
the rule above refuses the commit precisely because that instance is
a down reporter. The alternatives are `commit force` (same data loss,
one log line instead of one per mark) or resolving the marks first,
which is step 1. `retire` of a reporter resolves its marks as a side
effect (§7.1) — but you cannot reach `retire` without passing
through `disable` first, which is the whole trap.

### 7.5 Scrub

Each instance continuously re-reads its objects and verifies block
digests and `csum`, rate-limited so that a full pass completes in
about `scrubdays` (implementation policy; a default near 14 days
sizes to ~4 MiB/s on a 4 TB disk).

On mismatch:

1. Mark the object locally corrupt, list it in `/lost`, and fail
   client access with `checksum mismatch`.
2. It can no longer win arbitration against anything, including
   absence (§1.3).
3. The serving primary reconciles it from a peer. A repair MAY
   transfer only the mismatching blocks (implementation policy);
   the normative repair is whole-object `op=full`.
4. If no peer holds a verifying copy, the object is **lost**: it
   stays in `/lost` and client access fails with `object lost`.
   Losses are never silently papered over.

`/lost` also carries divergence events (§1.3: equal key, differing
content), which are bugs rather than media faults and must be
visible as such.

This is D4's argument in action: the checksum and the redundancy
are in the same layer, so a corrupt copy can be arbitrated away —
which a block-layer mirror underneath could not do.

## 8. Monitor service

*Normative: the file tree, the ctl grammar, the map format (§3), the
liveness-evidence rule, the promotion gate, and the
forward-compatibility rules. Implementation policy: how it stores
its state locally.*

### 8.1 Attach and file tree

    aname = attr *("," attr)
    attr  = "role=" ("reader" / "instance" / "admin") / "peer=" iid

`role` defaults to `reader`. `role=instance` MUST carry `peer=` and
is the attach a storage instance uses; it is the only role whose
reads count as liveness evidence (§8.4) and the only role that may
write the instance-reported ctl verbs. `role=admin` is the operator
role. `reader` and `instance` may read `/map`, any `/maps/<epoch>`
— §5.2 clause 2 needs `/maps/<E−1>` — and the status files, and
nothing else. No epoch appears in the monitor's `aname`: the monitor
is where epochs come from.

    /ctl            verbs (§8.3)
    /map            current map, text (§3), stale ledger included
    /map.next       staged map under construction, text
    /maps/<epoch>   the last `retain` published maps
    /instances      registry: uuid, iid, node, last registration
    /stale          view of the map's stale records (§7.1)
    /health         liveness observations, diagnostic only
    /status         monitor state: epoch, uptime, timers, retain

All status files are **snapshot-at-open** (§2.2), so a concurrent
mutation cannot produce a torn read.

Field lists, one `attr=value` record per line:

- `/instances`, one line per registered disk:
  `uuid=<hex> iid=<iid> node=<name> addr=<dial> class=<tag>
  registered=<u64 seconds> lastseen=<u64 seconds>`, plus
  `conflict=node` on a disk that registered from a node other than
  the one it is bound to (§3.4 step 5, the operator's
  `rehome` cue). This is the operator's conflict-resolution surface,
  so the field names are normative; extra fields are permitted.
- `/health`, one line per instance:
  `iid=<iid> lastrefresh=<u64 seconds> silent=<ms>
  reports=<iid>,<iid>,…` — the last field being the instances that
  currently claim they cannot reach this one (§8.4). **Format beyond
  those names is implementation policy**, and so is the whole file's
  content in the sense that matters: nothing in this design reads it
  except a human.
- `/status`: `epoch=`, `monid=`, `uptime=`, the timer attributes in
  force, `retain=`, `ledger=ok|lost` (§8.6), and
  `pending=<n>` staged edits. Format otherwise implementation policy.

`/health` is *not* authoritative for anything: it is a diagnostic
window, and since D-a (§10.4) it is not input to any automatic
transition either. Placement, the candidate set, the promotion gate
and every demotion come from `/map` (ledger included) and from the
refresh channel only.

`/map.next` framing, which the first draft left undefined: it is an
ordinary file. Writes **honour their offset**; opening it with
`OTRUNC` empties it; `propose` also empties it. `commit` parses the
complete text and MUST reject a map that does not validate — bad
grammar, missing `map` record, an immutable attribute changed
(including `monid`), an illegal `status`/`up`/`fenced` combination
(§3.3), `weight` ≠ 100, an epoch not exactly `current+1` — with
`bad map`, leaving the current map untouched. Without this rule a
>8 KiB map arriving as successive `Twrite`s through a kernel mount
could be silently accepted half-written.

Two exemptions from that validation, both explicit: `forceepoch`
(§8.6) sets the next epoch to an arbitrary higher value and MAY
change `monid`, so the "exactly `current+1`" and "`monid` immutable"
checks do not apply to the commit it governs — that is the entire
purpose of the verb, and without the exemption the rebuild path would
be refused by the rule meant to protect it. Everything else in the
validation still applies to it.

### 8.2 Durability and retention

The monitor MUST make `/map` durable before acknowledging a
`commit`, and MUST make a stale-ledger change durable before
acknowledging the
`stale` verb that created it — §5.4 step 5a treats that
acknowledgement as a durability claim, and the whole dead-primary
defence rests on it. Since the ledger lives in the map (§3.1), both
are one requirement: **the map text, including its `stale` records,
MUST be durable against power loss before the acknowledgement that
publishes it.**

That requirement is normative; the mechanism is not, and this design
does not pretend a mechanism exists. 9front has no `fsync(2)`, and
cwfs and hjfs commit on their own schedule, so "the monitor MUST
fsync `/map`" — as the previous revision put it — named a primitive
the target platform does not have. The candidates are a raw partition
the monitor writes and reads itself, a file server `ctl` sync
operation where one is offered, or a small dedicated log device.
Which of them actually gives power-loss durability on 9front, and at
what cost per commit, is evidence work: §10.2 carries it as the
**second-largest unknown** in the design, behind the equivalent
question for the object store (§5.4). Both are the same question
asked of two different processes, and neither may be answered by
assumption.

It MUST keep the last `retain` (default 8) published maps under
`/maps`, and MUST keep at least the immediately previous one, which
§5.2 clause 2 depends on.

It SHOULD additionally write each published map into the object
store as a reserved object `shoal.map.<epoch>`. Because the ledger
travels inside the map, that one object is the whole backup; the
separate `shoal.stale` object of the previous revision is gone. These
copies are a backup for rebuilding a monitor, not an authority — the
local files are authoritative, because reading the object store
requires a map.

### 8.3 ctl grammar

One command per `Twrite`, one physical line. Map edits are **staged
then committed**, so an operator can read the diff before anything
moves. Alternatively an operator MAY write a complete map text to
`/map.next` directly and `commit` it — the format is small and
human-editable on purpose.

Reported by instances (`role=instance`; the reporter is the
attach's `peer=`, never an argument):

| Verb | Role | Form | Effect | Errors |
|---|---|---|---|---|
| `register` | instance | `register uuid=<hex> node=<n> addr=<a> class=<c>` | Idempotent instance registration (§3.4). | `bad ctl` (malformed, or a uuid bound to a `dead` instance, §1.5) |
| `unreachable` | instance | `unreachable <iid>` | "I cannot reach this peer." Recorded in `/health` and **nothing else**: it MUST NOT influence `up`, `status` or any promotion (§8.4). A level, not an edge; the reporter SHOULD repeat it while the condition holds. | `bad ctl` |
| `reachable` | instance | `reachable <iid>` | Clears the reporter's outstanding `/health` report. | `bad ctl` |
| `stale` | instance | `stale <iid>` | Register a stale mark with this reporter and `<iid>` as subject (§7.1). MUST be durable before the reply. Bumps the epoch (§6.1). Registering an already-unresolved mark for the same pair is a no-op that still succeeds. | `bad ctl` |
| `synced` | instance | `synced <iid>` | The reporter's dirty set for `<iid>` is empty and its `fullsync` flag cleared; resolves its mark and records the epoch (§7.3). | `bad ctl` |
| `healed` | instance | `healed epoch=<e>` | The reporter has completed its heal advert pass (§7.3). | `bad ctl` |
| `rebalanced` | instance | `rebalanced epoch=<e>` | The reporter has completed its §7.2 reconcile pass for placement epoch `<e>`; input to §7.4's one-change-at-a-time rule. | `bad ctl` |

Operator verbs (`role=admin`). Every one either stages a change or
publishes; `bad ctl` answers a malformed or inapplicable one
throughout, and is omitted from the last column except where the
verb has a more specific failure:

| Verb | Role | Form | Effect | Errors |
|---|---|---|---|---|
| `propose` | admin | `propose` | Start (or reset) a staged map from the current one. | |
| `enable` | admin | `enable <iid>` | Staged: `new`\|`out` → `in`. | |
| `disable` | admin | `disable <iid>` | Staged: `in` → `out`. Data will move. | |
| `retire` | admin | `retire <iid>` | Staged: `out` → `dead`. Asserts the disk's data is permanently gone; retires the iid forever (§3.3). Retiring an instance that is the **reporter** of unresolved marks discards acked writes and MUST be logged as such (§7.1). | |
| `rehome` | admin | `rehome <uuid> node=<n>` | Staged: re-bind a moved disk. | |
| `setclass` | admin | `setclass <iid> <class>` | Staged. | |
| `set` | admin | `set <attr> <value>` | Staged header attr; immutable attrs rejected. | `bad ctl` on `objmax`, `blksz`, `csumalg`, `placehash`, `monid` (§8.5) |
| `commit` | admin | `commit [force]` | Publish the staged map at `epoch+1`. `force` overrides both §7.4 refusals — the down-reporter one and the one-placement-change-at-a-time one — and MUST be logged. | `bad map` (validation, §8.1); refusal without `force` (§7.4) |
| `abort` | admin | `abort` | Discard the staged map. | |
| `bump` | admin | `bump` | Publish an unchanged map at `epoch+1`. Forces a cluster-wide refresh; also the mechanism behind §1.5's mandatory `tombdays`/2 heartbeat. | |
| `promote` | admin | `promote <iid> force` | Override: `heal`/`no` → `yes` without satisfying §7.3's gate. Knowingly serves possibly-stale data. MUST be logged. | |
| `forcesync` | admin | `forcesync <iid> from=<iid>` | Resolve a stale mark whose reporter is permanently gone. Knowingly discards acked writes. MUST be logged. | |
| `forceepoch` | admin | `forceepoch <u64> [monid=<hex>]` | Set the next epoch explicitly, and optionally a new `monid`, for the rebuild path (§8.6). Exempt from §8.1's `current+1` and immutable-`monid` checks. MUST be logged. | |

The first draft's `dirty <iid>` and `clean <iid> epoch=<e>` verbs are
gone. `dirty` was unreachable as specified — the draft created dirty
records only for `up=no` peers, so a `dirty` report could never
concern an `up=yes` peer, while the verb's stated effect was to
demote an `up=yes` peer — and the natural way for an implementer to
resolve that contradiction (ack first, report dirty after) broke the
invariant outright. `stale`/`synced` are its honest replacement:
they name the reporter implicitly, are registered *before* the ack
rather than after, and live in a durable ledger rather than in a live
primary's memory.

`admit` is gone with quarantine (§1.5, §10.4).

Automatic transitions the monitor performs itself, each publishing
a new epoch:

- refresh channel silent for `deadms` → `up=no fenced=yes` (never
  sooner: F2). **This is the only automatic demotion.**
- heard from again → `up=heal fenced=no` (never straight to `yes`;
  any transition out of `up=no` clears `fenced=`, §3.3).
- §7.3's gate satisfied → `up=yes`.
- continuously `up=no` for `outmins` → `status=out`, the only
  automatic transition that moves data.
- every `tombdays`/2 → a plain `bump` (§1.5).

### 8.4 Failure detection

*Normative.* The first draft's fencing argument silently assumed
that the monitor's "last heard from X" was X's last successful map
refresh, while §8.1 advertised RTT probing — the opposite direction,
a different socket, a different failure mode. If the monitor probes
X's instance address while X pulls `/map` successfully, the monitor
can depose X while X never fences, and X keeps serving reads at the
old epoch. The channel identity is therefore made normative.

**Lease evidence, and nothing else.** `lastseen(i)` is the time of
instance `i`'s most recent successful read of the monitor's `/map`,
or its most recent `register`, on an attach with
`role=instance,peer=i`. An instance is demoted automatically when,
and only when, that channel has been silent for `deadms`. **Nothing
else demotes**: not a peer's opinion, not a client's, not a probe.
The monitor MAY probe instance addresses for `/health` diagnostics
and MUST confine what it learns from them to `/health`. An instance
MUST refresh at least every `pollms`, and F1 obliges it to fence
itself if it cannot within `leasems`; a lease demotion at
`deadms > leasems` is therefore provably behind a self-fence, which
is what licenses `fenced=yes`.

**Peer reports are diagnostics.** An instance that cannot reach a
peer SHOULD report `unreachable <iid>`, and `reachable <iid>` when
the condition clears (§8.3, `role=instance`, so the reporter is
identified by the attach's `peer=`). The monitor MUST record those in
`/health` and MUST NOT let them influence `up`, `status`, placement
or any promotion gate. Client libraries do **not** report at all:
there is no client-facing reporting verb in this design.

**The partial partition, and what actually happens now.** A one-way
link, a wedged filter or an exhausted listen queue can leave peer Y
reachable from the monitor but not from primary X. Both keep
refreshing, so neither is demoted, and the residual is real and MUST
be stated rather than engineered around:

- With `mincopies=1`, X's **first** degraded write registers a stale
  mark (subject Y, reporter X) and completes. But that registration
  bumps the epoch (§6.1), the bump invalidates `cur` everywhere, and
  every subsequent currency check for an {X,Y}-placed object needs an
  `op=meta` from Y (`up=yes`, witness clause 1) — which fails at
  `replms`. Admission (§5.4 step 1) requires currency, so after that
  one write **reads and writes for the pair's placement share go
  dark with `not ready`**: the mark's own bump closes the degraded
  path that registered it. This is a pair-share outage, not a slow
  path.
- With `mincopies=2` and R=2 the writes fail `degraded` immediately
  (`k = 1 < mincopies`), and reads follow at the next epoch bump —
  the `tombdays`/2 heartbeat guarantees one — for the same currency
  reason. Both settings converge on the same outage.
- The marks X registered against Y hold Y at `up=heal` if Y is ever
  demoted and returns, and they hold up placement changes if X ever
  goes down while they are outstanding (§7.4).
- **Nothing resolves this automatically.** It persists until the link
  is repaired, or an operator intervenes: `disable <iid>` + `commit`
  to move placement off one of them, or `fence on` at the instance
  that should stop serving. Both are visible, logged, human decisions.

An earlier revision let sustained `unreachable` reports demote a
subject whose own refresh channel was healthy. That is removed, and
§10.4 records why in full: it did not fix the case it cited (with
`mincopies=1` the degraded path already completes the write; with
`mincopies=2` and R=2 the demotion leaves `k=1 < mincopies`, so the
write still fails), it could evacuate a perfectly healthy disk from a
single broken link once `outmins` elapsed, its own "never leave fewer
than `replicas` instances `up=yes`" floor reinstated a permanent
stall in the very case it existed for, and unattributable client
reports let any host on an unauthenticated LAN help demote a healthy
instance. Removing it makes the failure detector one rule with one
evidence channel, which is what F1+F2's argument needs to compose.

### 8.5 Immutable attributes

`objmax`, `blksz`, `csumalg`, `placehash` and `monid` are fixed at
cluster creation. `set` on any of them MUST fail with `bad ctl`, and
a `/map.next` that changes one MUST fail `commit` with `bad map`.
Changing `objmax` would silently break Layer B's arithmetic (D3);
changing `csumalg` or `placehash` would invalidate every stored
checksum or every placement decision at once; changing `monid` would
disown every instance in the cluster (§6.3). `monid` has the one
documented escape, `forceepoch <e> monid=<hex>` (§8.3), which exists
so that a deliberate re-identification is possible and logged rather
than impossible.

### 8.6 Rebuilding a monitor, and epoch regression

The first draft said the map was mirrored into the object store for
rebuild, and separately said that with no monitor every instance is
fenced and `role=admin` may not do object I/O — so at the moment the
backup is needed, **no role could read it**. Two rules fix that.

1. **`role=admin` may read reserved `shoal.` objects even while the
   instance is fenced** (§2.1, F1). Nothing else is exempt: no
   writes, no non-reserved reads, no ctl mutation. That is enough to
   recover `shoal.map.<epoch>` — which now carries the stale ledger
   inside it (§3.1) — from any instance that holds it.
2. **A monitor MUST NOT publish an epoch it cannot prove is the
   highest.** On start it:
   a. loads its local map (epoch `E0`), ledger included;
   b. attaches `role=admin` to every registered instance and reads
      `/status`, which reports the highest epoch that instance has
      adopted, in the normative field `epoch=` (§2.2);
   c. waits `deadms` before publishing anything that demotes an
      instance (F2);
   d. publishes at `max(E0, observed maximum) + 1`, keeping the
      cluster's `monid` unchanged.
   If any registered, non-`dead` instance is unreachable in (b), the
   monitor MUST NOT publish at all — it may be holding a higher
   epoch — and MUST report the condition in `/status`. The operator
   resolves it with `forceepoch <e>`, choosing an epoch above
   anything that could exist, which is logged.
   A monitor rebuilt from a map older than the cluster's true epoch
   holds a ledger that is old in the same way: marks registered after
   that map was written are missing. It MUST therefore, in step (b),
   also read each reachable instance's `/stale` and take the **union**
   of those marks with its own before publishing. Instances mirror
   the marks they are party to (§2.2), so a mark survives as long as
   either of its two instances does.
   A rebuilt monitor with **no** recoverable map at all MUST behave
   as though every pair were marked: no instance may be promoted past
   `up=heal` until §7.3(b) is satisfied afresh, and the condition
   MUST be reported in `/status` (`ledger=lost`). It cannot check
   §7.3(a), so the dead-primary protection is degraded until an
   operator reasons about it — which is why the map is mirrored into
   the object store at all (§8.2).
3. **The previous monitor must be stopped, or unreachable from every
   instance, before a new one publishes.** This design does not
   detect two live monitors that share a `monid`, and cannot: the
   `monid` tripwire (§6.3) catches a *different* authority, which is
   the accident, not the deliberate second copy of the same one.
   Two monitors sharing a `monid` is operator error, and the
   consequences are stated so the error is not made casually: each
   publishes epochs the other does not know about, instances adopt
   whichever they last read and refuse the other as an epoch
   regression, refresh channels look silent to whichever monitor an
   instance is not talking to, and each monitor can therefore publish
   `up=no fenced=yes` for an instance that is refreshing happily from
   the other. That last one breaks the F1+F2 composition outright: an
   incoming primary skips the handoff grace on a `fenced=yes` it has
   no right to trust, and the deposed primary is not fenced at all.
   Before starting a replacement monitor an operator MUST stop the
   old one, or establish that it cannot reach any instance; where the
   old process cannot be reached to be stopped, `fence on` at every
   instance is the blunt instrument that makes the question moot.

Instances complete the fix from their side by refusing an epoch
regression outright and by pinning `monid` (§6.3).

### 8.7 Forward compatibility with a replicated monitor

The v1 monitor is one process. Everything above is chosen so that
replication is additive, never a format break:

- Readers MUST ignore unknown map attributes and unknown record
  kinds (§3.1), so a `mon=` record kind and a `term=`/`leader=`
  header attribute can be added later without touching any v1
  parser.
- The epoch is already the only ordering authority in the system,
  and it is already required to be strictly increasing and assigned
  in one place. A consensus protocol replaces *how* an epoch is
  assigned, not *what* it means.
- Clients already reach the monitor only by reading `/map`, so a
  replicated monitor can serve `/map` from any replica, and the
  staged-map ctl surface can be restricted to the leader with a
  redirect error, with no change to the client path.
- The stale ledger is the one piece of new durable monitor state
  this round adds, and it is now literally part of the map (§3.1):
  small, bounded by the square of the instance count, updated by a
  single idempotent register/resolve per pair. Replicating it is not
  merely the *same problem* as replicating the map — it is the same
  bytes, ordered by the same epoch.
- `monid` (§6.3) is a cluster identity, not a monitor-process
  identity, so a replicated monitor keeps publishing the same one.
  It stays a tripwire against a foreign authority and imposes
  nothing on how many processes serve the map.

## 9. Hash choice — settling D6

*Normative: the algorithms, digest lengths, and encodings, all as
already specified in §1.4 and §4.2. This section is the
justification.*

Stock 9front libsec provides blake2s, sha2_64 (SHA-224/256),
sha2_128 (SHA-384/512), and sha3. BLAKE2s and SHA-256 are both
available with no new code.

**Recommendation: BLAKE2s everywhere.**

- **Object checksums:** BLAKE2s-128 per block, BLAKE2s-256 over the
  block digests, named once in the map header as
  `csumalg=blake2s256`. BLAKE2s beats SHA-256 on hardware without
  SHA extensions — the common case on a 9front fleet — and
  checksumming sits in the write path, so the margin is real
  throughput. Digest length is a BLAKE2 parameter, not a truncation,
  so BLAKE2s-128 is a specified function rather than a chopped-off
  hash; 128 bits is ample for corruption detection, which is not
  adversarial, while the 256-bit value that crosses the wire stays
  full strength.
- **Transfer digests:** the same BLAKE2s-128, as `dcsum` on `/repl`
  and `/rpc` payloads (§5.5). Different bytes, different purpose,
  different attribute name.
- **Placement:** the *same* primitive (§4.2). Placement needs no
  cryptographic strength, so cheap hashes were considered: FNV-1a
  is rejected outright — its weak avalanche correlates adjacent ids
  like `f.0` and `f.1`, precisely the pattern Layer B generates;
  SipHash-1-3 would fit in ~60 lines but is rejected for v1 because
  the cost it saves does not exist. A 12-node lookup is ~15 hashes
  of ~80-byte inputs, on the order of a microsecond, against a LAN
  RTT three orders of magnitude larger. One primitive means one
  implementation to get right and one vector set to test.
- **Reserved escape.** `placehash=blake2s256-64` sits in the map
  header from day one (D5-style cheap reservation). If placement
  ever appears in a profile, SipHash-1-3 enters as a new *value* of
  an attribute that already exists and is already checked — not a
  change of format shape.

This settles D6's open half and should be ratified as a new
decisions.md row. This document does not edit `docs/decisions.md`.

## 10. Conflicts, deviations, and open questions

### 10.1 Conflicts and deviations

Re-derived against the revised design; two items from the first
draft are gone, three are new.

1. **"Stock 9P", `mount(1)`, and fid lifetime (D2).** Carrying
   `epoch`, `role` and `peer` in `aname` (§2.1) is stock 9P2000 — no
   new messages, no new fields — but it is an acknowledged abuse of
   a field that conventionally selects a tree, and `role` arguably
   belongs to whatever authenticates. The first draft additionally
   checked the epoch on every operation, so a raw kernel mount broke
   at the next epoch bump; §6.2 removes that check, and attaches now
   survive epoch bumps. Layer A is still consumed through a client
   library — for map polling, `not primary` redirection and
   retryable-error handling — but the constraint is now a client
   *behaviour* requirement, not a fid-lifetime trap, and `mount` +
   `ls` is a usable operator debugging path.
2. **Primary-only reads (§5.1)** are an addition, not a restriction,
   in target.md's terms: target.md fixes the write path and is
   silent on reads. The choice follows from D2 and is reversible
   later without a format change.
3. **`weight` present but rejected unless 100 (§4.4).** target.md's
   brief left weighting as a conclusion to reach; the conclusion is
   "not in v1". Rejecting non-default values is stricter than merely
   ignoring them, on the grounds that a silently ignored weight is a
   worse failure than a refused one. The cost is explicit: unequal
   disks fill unevenly.
4. **Reserved `shoal.` id prefix (§1.1)** constrains D3's not-yet-
   fixed `fileid` grammar. D3 marks the naming scheme normative but
   defers the exact grammar to this round; this is that constraint
   being registered, not a deviation from it.
5. **NEW — outcome ambiguity is propagated upward (§5.4).** An
   object operation that returns an error or is flushed MAY or MAY
   NOT have been applied. This is compatible with target.md's
   "writes to a single object are linearizable" (linearizability has
   always permitted an operation whose response is lost to take
   effect) but it is a real obligation on Layers B and C: a failed
   stripe write must be re-read before anything is concluded from
   it, and the MDS must not treat "write failed" as "state
   unchanged". target.md does not say this; it needs to be true
   before Layer B is designed.
6. **NEW — the monitor holds durable replication state, and it is on
   the read path as well as the write path (§7.1, §3.1).** The stale
   ledger is a second durable authority, now carried inside the map
   itself. It is small and bounded, and it is what makes failover
   safe when the primary that knew about a lagging peer is gone. Two
   consequences the previous revision understated:
   - The ledger is consulted by **every currency check**, which is on
     the path of the first client touch of every object after every
     epoch bump (§5.2 clauses 4 and the skip rule) — not only by
     degraded writes. An unresolved mark whose reporter is `up=no`
     makes those objects answer `not ready` until it is resolved.
     The blast radius is scoped to objects whose placement contains
     the mark's *subject*, about `1/|V|` of the cluster; unscoped, as
     it was, it was the whole cluster.
   - Losing the monitor's state degrades safety (§8.6) in a way that
     losing only placement would not. D1 said authority is "cheapest
     and most honest" at the per-object primary; this puts one bit
     per ordered pair of instances somewhere else, and the doc owns
     that rather than burying it.
7. **NEW — operator actions that knowingly break the invariant.**
   `promote force`, `forcesync`, `commit force`, `forceepoch`, and
   `retire` of an instance that is the reporter of unresolved marks
   each exist because the safe automatic behaviour is
   unavailability, and unavailability sometimes has to be traded
   away by a human. Every one MUST be logged as a data-loss event.
   This list and §5.7's are the same five, deliberately. (`admit` was
   in this list; it is gone with quarantine, §10.4.)

Two first-draft entries are retired: the fid-lifetime asterisk
folded into (1), and `retain`-window reconciliation, which no longer
exists (§7.2).

Consequences of the design that target.md does not promise and the
owner should see:

- **Losing the single monitor takes the cluster read-unavailable
  within `leasems` (3 s by default)** (§6.5), not merely "no
  failover until it returns". That falls out of the fencing lease
  and cannot be softened without reintroducing split-brain.
- **A dead node costs `deadms` (10 s) of retries** for the ~1/|V| of
  objects it was primary for — for reads as much as writes, since a
  dead primary is `up=yes` until the monitor demotes it. No single
  request blocks that long (§5.4 bounds one request at ≤ 3·`replms`
  plus local I/O), but the affected objects are
  unavailable-and-retrying for the window. Where the demotion is
  lease-based the handoff grace is skipped (§5.2), so failover
  completes at `deadms` and not later; for a placement change or an
  operator `disable` it costs a further `leasems`.
- **An operator adding or removing a disk makes the objects whose
  primaryship moves answer `not ready` for `leasems` plus one
  currency check** — and, if the winning copy is elsewhere, for the
  background pull as well, since §5.2 forbids pulling inside the
  client request. Retryable, bounded, visible in `/jobs`, and the
  price of not answering `no such object` for data that exists.
- **A second disk enable must wait for the first rebalance to be
  reported complete** (§7.4). Operators used to serial rebalances
  will not notice; operators scripting a fleet expansion will.
- **A wedged one-way link between two instances is not repaired
  automatically and never will be by this design** (§8.4, D-a in
  §10.4). Objects placed on the affected pair go dark: at any
  `mincopies`, once an epoch bump lands — the first degraded write's
  own mark registration causes one — currency checks that need the
  unreachable peer fail and the pair's placement share answers
  `not ready` for reads and writes alike, with all copies healthy.
  It clears when the link is fixed or when an operator runs
  `disable` or `fence on`. This is the price of having exactly one
  automatic demotion rule, and it is a price, not a free lunch.
- **A flushed or failed write leaves no equal-key divergence**, but
  it does force the primary to re-run a currency check for that
  object before serving it again (§5.4 step 7, §5.4.1), so an
  interrupted client can make its own next request answer
  `not ready` once.

### 10.2 (a) Settleable from evidence by another engineer

- Measured `msize` on 9front, which sets the forwarded-write chunk,
  how often clients see short writes, and the real resync time
  (§5.5's arithmetic assumes 64 KiB payloads and a 0.2 ms RTT).
- Whether `blksz=65536` and BLAKE2s-128 block digests sit at the
  right point on the metadata-size vs re-hash-cost curve, and what
  a per-4 KiB-write re-hash actually costs on the fleet.
- Whether `objmax=16 MiB` is right, against resync time, object
  counts, and Layer B's striping efficiency.
- Timer defaults (`pollms`, `leasems`, `replms`, `deadms`,
  `outmins`, `tombdays`, `retain`, scrub rate). All are map
  attributes, so changing them is never a format change. `replms` is
  new and its default (1 s) is a guess: it trades a false
  degraded-path entry against how long a client blocks.
- What a currency check actually costs: `op=meta` to |W(o)| peers on
  the first touch of each object after a bump, and whether the
  background sweep keeps `cur` warm enough that clients rarely pay
  it.
- Whether the handoff grace can safely be shortened by having the
  monitor publish, per instance, the time of its last successful
  refresh — letting an incoming primary compute the deposed
  primary's actual fence deadline instead of assuming the worst.
  That would cut rebalance `not ready` windows; it needs the clock
  assumptions re-checked before it is proposed.
- Whether a directory read of `/obj` at ~2.6·10^5 entries is
  comfortable on 9front with the snapshot-at-open requirement, and
  what the snapshot costs in memory.
- Local object-store layout: file per object vs packed store; where
  block digests and staged updates live; how `(content, ver,
  wepoch, csum)` atomicity (§1.3) and durability-before-ack (§5.4)
  are actually obtained on 9front, and what they cost per write.
  This is the single largest unknown in the design.
- **How the monitor makes a map durable before acknowledging a
  commit or a `stale` registration (§8.2).** 9front has no
  `fsync(2)`; cwfs and hjfs commit on their own schedule. The
  candidates are a raw partition the monitor manages itself, a file
  server `ctl` sync where one exists, or a small dedicated log
  device — with the cost per commit measured, because §5.4 step 5a
  puts one such acknowledgement inside a client write on the
  degraded path. This is the **second-largest unknown**, and it is
  the same question as the item above asked of a different process;
  neither may be answered by assumption.
- Whether partial-block repair (§7.5 step 3) is worth implementing.
- BLAKE2s vs SHA-256 throughput on the fleet, to confirm §9's
  premise with numbers rather than a general claim.

### 10.3 (b) Product and design calls for the project owner

The first five are unchanged in substance; their framing is updated
where the redesign moved the trade-off. The sixth is new from this
round.

1. **Monitor availability.** Monitor loss stops the cluster within
   `leasems`. The redesign adds two further reasons to care, and the
   second is stronger than the previous revision let on: the monitor
   holds the stale ledger, so (i) it is on the degraded-write path —
   a write that would leave a placement member behind fails
   `degraded` if the mark cannot be registered within `replms` — and
   (ii) because the ledger travels in the map and every currency
   check reads it, the monitor is on the **read** path too, in the
   ordinary sense that a stale or unavailable map makes an instance
   fence itself and a scoped unresolved mark makes `1/|V|` of objects
   answer `not ready`. Its loss also degrades the heal gate (§8.6).
   Acceptable for v1 with a fast-restart story, or does v1 need a
   replicated monitor?
2. **`mincopies` default.** Default 1 accepts a write onto a single
   surviving disk (available, but a second failure loses it).
   Default 2 refuses writes to affected objects during any
   single-disk outage (durable, but less available than one node).
   The redesign changes the consequence of losing that single disk:
   the loss is now *surfaced* — the peer that missed the writes is
   either held at `up=heal` by the heal gate or, if it stayed
   `up=yes`, refused by the currency check, so the affected objects
   answer `not ready` rather than serving a rollback (§5.7) — until
   an operator runs `forcesync` or retires the dead reporter. That
   makes `mincopies=1` more defensible and less comfortable at the
   same time. Note that the wedged-link residual (§8.4) is a
   pair-share read/write outage at *either* setting — `mincopies`
   does not soften it. Which is the product?
3. **Failure-detection and handoff windows.** Affected objects are
   unavailable-and-retrying for up to `deadms` (10 s) on a node
   death, and for `leasems` plus a currency check (plus a background
   pull, if the winning copy moved) on a rebalance or an operator
   `disable`. Individual requests no longer block for the whole
   window (§5.4's summed bound caps them at ≤ 3·`replms` plus local
   I/O), so the question is now about client-visible retry latency,
   not wedged processes. Shrink `deadms` at the cost of flappier
   `up=no` transitions?
4. **`promote force`.** Should the operator override that serves
   possibly-stale data exist at all in v1, given it knowingly breaks
   D2's contract? Note it now has four siblings with the same
   character (`forcesync`, `commit force`, `forceepoch`, and
   `retire` of a reporter) — the question is really whether the whole
   family is wanted, or whether v1 should simply stay unavailable and
   make the operator restore from elsewhere. With report-based
   demotion removed (§10.4), the operator verbs are also now the
   *only* way out of a wedged-link partial partition, which raises
   the price of removing them.
5. **Authentication.** Is Layer A a trusted-network service in v1
   (no auth on attach), or must attaches authenticate via
   factotum/p9any? This is not admin convenience: with an
   unauthenticated `role=repl` attach, any host on the LAN can write
   `/repl` with an attacker-chosen `(wepoch, ver)` high enough to
   win arbitration everywhere, or `op=full force=1` a target object
   to arbitrary content, or register itself as an instance. It is a
   data-integrity boundary, and `role=admin` additionally reaches
   `forcesync`, `promote force` and `forceepoch`. Deciding "trusted
   network" is deciding that the storage LAN is a security domain.
   Removing client reports (§10.4, D-a) closed one unauthenticated
   influence channel — any host could previously help demote a
   healthy instance — but it closed one hole in a wall, not the wall.
6. **NEW — how coarse may durable staleness be, and what happens
   when its reporter never comes back?** The ledger is per pair of
   instances (§7.1), so one stale object blocks a whole disk's
   return to `up=yes`, and a reporter that is permanently lost
   leaves its subject unpromotable until an operator accepts the
   loss with `forcesync`. The alternatives are a finer ledger
   (per-object, unbounded, monitor in the write path — rejected
   here) or an automatic timeout that promotes the subject after
   some interval, which is `forcesync` without a human. Should v1
   ever discard acked writes without an operator saying so?

### 10.4 Alternatives considered and rejected

Two mechanisms present in the previous revision were **removed** in
this one. They are recorded first, in full, because removing a
mechanism moves risk rather than deleting it, and the residual
belongs next to the rationale.

**D-a — report-based demotion is removed.** The previous revision let
the monitor demote an instance whose own refresh channel was healthy,
on the strength of sustained `unreachable` reports from peers and
clients, to address the wedged-one-way-link partial partition. It is
gone; lease evidence — silence on the refresh channel for `deadms` —
is now the only automatic demotion path (§8.4). Four reasons, each
sufficient on its own:

- *It did not fix the case it cited.* With `mincopies=1` the degraded
  path already completes the write without any demotion. With
  `mincopies=2` and R=2, demoting the unreachable peer leaves
  `k = 1 < mincopies`, so the write fails exactly as before — the
  demotion changes the error's provenance, not its occurrence.
- *It could evacuate a healthy disk.* Reports never clear while the
  link stays wedged, so the demoted instance sits `up=no` until
  `outmins` elapses and `status=out` fires — a full evacuation of a
  perfectly good disk caused by one broken link in one direction.
- *Its own safety floor reinstated the stall it existed to prevent.*
  "Never demote so that fewer than `replicas` instances are `up=yes`"
  means that in a three-instance cluster with one already down, no
  report-based demotion may happen at all; with `mincopies=2` the
  affected objects are permanently write-unavailable with two healthy
  copies present. The mechanism's guard rail and its purpose were in
  direct contradiction.
- *Client reports were unattributable.* They arrived as `role=admin`
  with no `peer=`, yet the rule counted distinct reporters; on an
  unauthenticated LAN (§10.3(5)) any host could help demote a healthy
  instance. Client reporting is removed entirely.

Instance-reported `unreachable`/`reachable` survive as `/health`
diagnostics and MUST NOT influence `up` (§8.3, §8.4).

*Residual, stated plainly:* a wedged link between two instances is
now never resolved automatically. Objects whose placement contains
both run degraded — writes paying `replms` and leaving an unresolved
mark at `mincopies=1`, failing `degraded` at `mincopies=2` with R=2 —
and currency checks that need the unreachable peer answer
`not ready`. The condition persists until the link is repaired or an
operator intervenes with `disable` (moving placement off one of them)
or `fence on` (stopping one of them serving). §8.4 and §10.1 both say
so. At ≤12 nodes on one LAN, a human noticing a broken link is an
acceptable substitute for a mechanism that did not work.

**D-b — quarantine is removed.** The previous revision published
`quar=yes` for an instance absent longer than `tombdays`, discounted
its adverts and `op=meta` responses, refused `pull` from it with a
`quarantined` error, and had an operator clear it with `admit`. All
of that is gone: the attribute, the error, the verb, the legal
combination rules that mentioned it, and the advert/`op=meta`
discount rules.

The reason is that §1.5's discard condition 1 already does the work.
It requires confirmation from **every** non-`dead` instance, made at
a time *after* the tombstone was created — so an instance that is
absent for any period blocks the discard for the whole of that
period, and when it returns it is asked, and either holds the
tombstone (or something newer) or holds nothing. There is no window
in which a discard completes behind an absent holder's back, so there
is no state a returning disk can be in that resurrects a deleted
object. §1.5 carries the argument in full, including why a
confirmed-empty instance cannot re-acquire a stale copy afterwards.
Quarantine forbade nothing that was possible, and cost: it was a
terminal state (`quar=yes` implies `up ≠ yes`, so `outmins` never
fires and the disk can never heal back), it made the newest acked
copy on a returning disk unpullable until a human ran `admit`, and it
produced an illegal map tuple when the monitor tried to quarantine a
re-registering disk (`quar=yes` with `status=new` forced `up=no`,
which then aged into `status=out` for a disk nobody had enabled).
That last defect dissolves with the mechanism.

*Residual, stated plainly:* the `dead` gap remains, closed only by
the reformat-before-rejoin rule — a disk declared `status=dead` is
excluded from condition 1, so if it is re-attached with its data
intact it can resurrect deletes, and the monitor MUST refuse a
`register` from a `uuid` bound to a `dead` instance (§1.5, §3.4).
And the flip side of condition 1 remains a capacity cost: one absent
instance blocks *all* tombstone discard cluster-wide until it returns
or is retired. That cost is now load-bearing rather than incidental —
it is the mechanism the resurrection argument rests on — so it must
not be optimised away without replacing the argument.

The rest of this section records alternatives considered and rejected
in earlier rounds, so the design's shape can be argued with:

- **Per-object monitor state instead of per-pair.** Exact, no
  false blocking, but unbounded and puts the monitor in the write
  path for every degraded write. Rejected on the envelope: ≤12
  nodes, R=2, and a coarse mark costs one round trip per divergence
  episode.
- **Two-phase commit on the write path.** Would remove the "may or
  may not have been applied" ambiguity. Rejected: it needs a
  recovery protocol and a coordinator log, i.e. consensus, which is
  exactly what the single monitor is not. The chosen order
  (replicate-and-commit, then commit locally, never publish an
  unacked key) gives I2 and I3 without it, and leaves an ambiguity
  linearizability already permits.
- **Inline full-resync of a lagging candidate inside a client
  write.** Correct, and permitted (§5.3), but as the *only* option
  it would put up to `objmax` of transfer inside one `Twrite`.
  Excluding the candidate with a durable stale mark is the bounded
  path, and the choice between them is implementation policy.
- **A per-object "clean" proof carried to clients, enabling replica
  reads.** Attractive later; it needs a format reservation this
  round does not want to spend and measurements nobody has (§5.1).
- **Refusing all placement changes while any instance is `up=no`.**
  Simple, and it would make advert sweeps trivially complete — but
  it forbids evacuating a dead disk, which is the main reason
  placement changes exist. Replaced by the narrower §7.4 refusal.
- **Keeping the per-operation epoch check.** Rejected in §6.2 after
  re-verifying that nothing F1 fences escapes without it; the cost
  was a cluster-wide fid flush on every transient `up` flap.
- **Reconciling against the last `retain` placement sets.** Real
  machinery for a cluster this size; the witness set plus adverts
  covers it (§7.2).
- **A separate liveness probe from the monitor to instances.** The
  fencing argument only composes if the monitor's evidence and the
  instance's lease are the same channel (§8.4); a second channel
  reintroduces exactly the split it was meant to close.
- **A stale ledger distributed beside the map, with its own
  freshness rule.** Considered when §5.2 was found to require an
  instance to read a ledger nothing gave it. Rejected: two documents
  describing one cluster state, each with its own version and its own
  staleness window, is how an instance ends up evaluating a fresh map
  against an old ledger and completing a check it should have failed.
  Publishing the ledger inside the map (§3.1) makes one refresh
  deliver both at one epoch, at the cost of bumping the epoch on mark
  changes — bounded by pair count, not write rate (§6.1).
- **Letting a currency check pull the winning copy inside the client
  request.** Simplest to write, and it is what the previous revision
  implied. Rejected: a pull is up to `objmax`, i.e. 185–545 ms by
  §5.5's own arithmetic, inside a `Twrite` that the same section
  claims is bounded by `replms`. Answering `not ready` and pulling in
  the background keeps the client's bound honest (§5.2, §5.4).
