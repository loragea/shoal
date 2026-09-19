/*
 * Private to srv/.  The two tables here — the file table and the ctl
 * verb table — are the contract the rest of the 9P surface is added
 * to: a file grows handler pointers in its own row, a verb is a row of
 * its own, and neither touches the other's lines.
 */

typedef struct Sfid Sfid;
typedef struct Sfile Sfile;
typedef struct Sctl Sctl;
typedef struct Qreq Qreq;
typedef struct Smap Smap;
typedef struct Sstage Sstage;
typedef struct Qjob Qjob;
typedef struct Sjob Sjob;

/*
 * layer-a §2.2's tree, one entry per file, in the order it is listed
 * there.  Qroot is the root directory; Qobjfile and Qmetafile are the
 * two rows with no fixed name — /obj/<oid> and /meta/<oid> — which a
 * walk reaches through their directory and which carry the same role
 * matrix and handler cells as every other row.
 */
enum
{
	Qroot	= 0,
	Qctl,
	Qstatus,
	Qmap,
	Qobj,
	Qmeta,
	Qrepl,
	Qrpc,
	Qadvert,
	Qdirty,
	Qstale,
	Qtombs,
	Qlost,
	Qjobs,
	Qobjfile,
	Qmetafile,
	Nfile,
};

/*
 * qid.path, layer-a §2.3.  An object's path is the engine's allocated
 * per-instance counter value, which starts at 1 and rises; /meta/<oid>
 * is a second name for the same object and needs a path of its own, so
 * it takes the object's with Pmeta set; and the fixed files take paths
 * above every value the counter can reach in this design.  Nothing
 * outside one fid's lifetime compares paths (§2.3), but two names in
 * one tree must still differ.
 */
enum
{
	Pmeta	= 1ULL<<62,	/* /meta/<oid>, over the object's own path */
	Pfixed	= 1ULL<<63,	/* Pfixed+Q<name> for the fixed files */
};

/*
 * What a row's gate is asked about.  The request carries the rest: the
 * mode of an open or a create is r->ifcall.mode, and the name a create
 * names is r->ifcall.name.  A read and a write carry no mode — the
 * open settled that — so the op is the whole of what the gate is told
 * about which column they are in.
 */
enum
{
	Gopen	= 0,
	Gcreate,
	Gremove,
	Gwstat,
	Gread,
	Gwrite,
};

/*
 * One file of §2.2.  walk/rd/wr are §2.1's role matrix: which roles
 * may walk to this file, open it for reading, and open it for writing.
 * §2.1 states the matrix by role rather than by file, and is silent
 * about several cells; store.md §14(24) records what those cells are
 * here and why.
 *
 * The handler cells are what the rest of the surface fills in.  Each
 * row of the table names one field per line, so a field added to this
 * struct touches no existing row and two hands filling different cells
 * of one row do not meet in the same line.
 *
 *	gate	the row's own rules, run right after the role gate on
 *		open, create, remove and wstat, and before the cell on
 *		read and write — where there is no role gate, the open
 *		having settled the role, and where the gate runs because
 *		layer-a §6.4 F1 fences operations and the operator fence
 *		can go on under an open fid.  It answers nil, or the
 *		error string the operation is refused with.  What the
 *		object rows' gate asks, and in what order, is beside it
 *		in tree.c and in store.md §14(24).
 *	render	the file is a render-at-open text file (§2.2's MUST):
 *		open composes its bytes once into the fid's Text and
 *		every read is served from them.  It answers nil, or an
 *		error string.
 *	read	a read that is not served from a snapshot — a channel
 *		(/repl, /rpc) or a directory read.  It responds.  A row
 *		with a read cell gets every read, whether or not the fid
 *		also holds a rendered Text, and may serve that Text
 *		itself with textread(r, f->text); only a row with render
 *		and no read takes the automatic text path.
 *	write	a write.  It responds.
 *	open	a file whose open takes checks of its own (the object
 *		rows' mode rules, layer-a §2.4).  It responds.
 *	create	a Tcreate in this directory.  It responds.
 *	remove	a Tremove of this file.  It responds.
 *	wstat	a Twstat of this file.  It responds.
 *
 * A nil handler cell is an operation whose content is not built:
 * after the role gate and the row's gate, it answers the local
 * Enotbuilt.  That is deliberate, so the gate matrix is complete and
 * testable before the content is, and a row with every cell nil is a
 * file that is not built at all.
 *
 * A render, read or open cell MAY leave the service loop.  Anything
 * that takes an engine snapshot or a lock the engine holds has to —
 * lib9p's loop is single-threaded, so a cell that blocks there blocks
 * the Tflush layer-a §5.4.1 requires to be answerable — and the way
 * to do it is srvqpushany (fns.h), which puts the request on a queue
 * of its own outside the pool the oids hash into, so that nothing an
 * offload does can reorder one object's operations against another's.
 * A row opts in by filling the cell that gets the request first: the
 * open cell for an open (a row with only a render cell is answered on
 * the loop, which is what the fixed status files want), the read cell
 * for a read.  Opting in is expected of a row whose render takes an
 * engine snapshot or a lock the engine holds; /status has not, and is
 * still rendered on the service loop although it calls storestat and
 * dirtycount, which take the engine's state lock.  It stays there
 * while it is the only caller and the lock is uncontended; the row
 * moves to the offload path when that stops being true, and nothing
 * outside this table has to change when it does.  Such a cell pushes
 * and returns; what runs on the queue obeys the pool's rules entire —
 * it tests srvqcheck if it has work worth skipping, and it MUST leave
 * through srvqdone, which is where the flush is answered and step 7
 * performed.  srvopentext is the standard render-at-open body for a
 * row that wants it on a queue.
 *
 * Which cells belong with which body of work.  The /obj directory
 * row's open and read cells — and the aux a fid of that row carries
 * while it is a directory fid, which is that read's snapshot — belong
 * with the enumeration of that directory.  That row's create cell, and
 * the /obj/<oid> and /meta/<oid> rows entire, belong with object I/O
 * and are built (obj.c).  The two meet in one place: a Tcreate that
 * SUCCEEDS turns the directory fid it is issued on into a fid for the
 * created object, so the create cell is what gives the directory fid's
 * aux back — auxclose, then auxfree — as it sets the fid's file, oid
 * and qid, since one fid cannot hold an enumeration's snapshot and an
 * object's state at once.  A create that FAILS leaves the fid where it
 * was, holding what it held: 9P moves a fid only on a create that
 * succeeded, and layer-a §2.4 keeps the same rule the other way round
 * for remove, where 9P clunks the fid whether or not the remove
 * succeeded.  So the give-back goes after the last refusal the cell
 * can answer, not before it; a cell that gave it back first would drop
 * the directory fid's Objsnap on a create the client will retry.
 * srvfidgive (fns.h) is that give-back: it runs auxclose under the
 * FID's state lock, with the cells cleared under it, and auxfree
 * behind it; `auxclosed', set there, is what keeps it and the
 * shutdown's own sweep from both closing one state.  The registry lock
 * is not what excludes the two — it covers the list and the fid's
 * rendered Text, and fidgive drops it before either hook runs
 * (tree.c).
 *
 * The two cells also meet on a fid where 9P does NOT keep them apart,
 * and that is what `moving' below is for.  lib9p refuses a Tcreate on
 * an open fid and a Topen on an open one from `Fid.omode', which its
 * `ropen' sets only once the open has ANSWERED; the /obj open is
 * offloaded, so a Tcreate pipelined behind a Topen on one fid — in
 * either order — passes that guard and both cells run, on two queue
 * procs at once.  Left alone both would succeed: the create moves the
 * fid to Qobjfile while the open installs a listing's snapshot on it,
 * and lib9p then writes the loser's qid and mode over the winner's.
 * So the cells refuse the second themselves, under this fid's state
 * lock, with lib9p's own Ebotch: the create refuses a fid an open has
 * ANSWERED for, one that holds a listing's state, and one another
 * create is moving; the open refuses to install over a fid a create
 * has moved or is moving.  A create claims the fid with `moving'
 * before its engine work and clears it at whichever exit it takes, so
 * exactly one of the two can win however the two procs interleave;
 * the price is that an open that arrives while a create that then
 * FAILS holds the claim is refused as well, which is a client that
 * pipelined the two.  A second Topen is not this case and is not
 * refused: it leaves the fid a directory fid, and srvfidgive in the
 * open cell is what gives the first open's snapshot back before the
 * second's is installed.
 *
 * That give-back is why the create tests `Fid.omode' as well as the
 * state slot.  It runs outside this lock, so a second open leaves the
 * fid holding NOTHING between the give-back and the install: the slot
 * is empty and no create is moving, which is the same fid a create
 * that got here first sees, and the state slot alone cannot tell the
 * two apart.  `Fid.omode' can: lib9p sets it in `ropen', so a fid
 * that has one is a fid an open has already answered on, and a create
 * on it is the losing half of the pipelined pair however far the
 * second open has got.  It is lib9p's own guard, read a message late
 * — which is the whole of what the offload costs this row.
 *
 * The /meta directory row's open and read cells — and the aux a fid
 * of that row carries while it is a directory fid — are the
 * enumeration's too, and are the same two cells: it lists the same
 * objects under a second name.  Both rows are built (enum.c).
 *
 * The six files that report state — /dirty, /stale, /tombs, /lost,
 * /advert and /jobs — are render-at-open text like /status and /map
 * and are built: /dirty, /stale and /lost in status.c beside the
 * other two, /tombs and /advert in enum.c, because each is that same
 * index snapshot rendered over a different set of states (layer-a
 * §7.2's line grammar), and /jobs in job.c, which is what starts the
 * background passes it lists.  The four whose render reaches the
 * engine — /dirty, /lost, /tombs and /advert — fill an open cell that
 * puts the render on the reserved queue; /stale reads the adopted map
 * and /jobs the job list, so both stay on the service loop.
 *
 * /repl and /rpc are the peer channels and are built (peer.c): /repl
 * fills a write cell and a read cell — §5.5 defines no read, so a
 * Tread of it is end of data — and /rpc fills all three, its open
 * cell being where §5.6's ORDWR-only rule is applied.  The per-fid
 * state a multi-request op stages is Sstage below, which the client
 * operations share: §5.5's op=full is the one kind of stage that
 * outlives its request, and it is the one that fills `g'.  A /rpc fid
 * uses the same slot for the request it has outstanding and the
 * response it has buffered.  The two channels' gate is this file's,
 * because the fence is (tree.c's chgate); /advert has none, because
 * F1's list names /repl and /rpc alone.
 *
 * The srvctls table below says the same for the verbs: a verb is
 * built by filling its row's fn or qfn, and the body of work that
 * verb names owns that cell.  `fence' and `newmonid' are ctl.c's and
 * `drop' is too, under the oid's queue; `verify' is object I/O's and
 * is built; `scrub', `reclaim' and `forget' are job.c's, and each
 * starts a pass /jobs lists.  `pull', `push', `reconcile', `advert',
 * `refresh' and `register' wait on the peer and monitor clients, so
 * their rows hold ctlnotbuilt and gate without a body (store.md
 * §14(18)).
 */
struct Sfile
{
	char	*name;		/* nil for the two object rows */
	int	isdir;
	ulong	perm;		/* the mode stat and open report */
	int	walk;
	int	rd;
	int	wr;
	char*	(*gate)(Srvctx*, Sfid*, Req*, int op);
	char*	(*render)(Srvctx*, Sfid*, Text*);
	void	(*read)(Req*);
	void	(*write)(Req*);
	void	(*open)(Req*);
	void	(*create)(Req*);
	void	(*remove)(Req*);
	void	(*wstat)(Req*);
};

extern Sfile srvfiles[Nfile];

/*
 * Per-fid state.  A fid is a file of the table plus, for the two
 * object rows, the oid it named; the attach attributes ride on every
 * fid derived from that attach (layer-a §2.1: the role and the epoch
 * travel in aname and a walk clones them).
 *
 * aux is the rest of the surface's — an /obj directory fid will hold
 * its Objsnap there and a /obj/<oid> fid its staged write — and the
 * three hooks beside it are when it is called upon.  Nothing in this
 * file's own handlers touches aux.
 *
 * `lk' is that state's lock, and it is the one a builder of a row has
 * to hold in mind.  It covers aux, the three cells below it, auxclosed
 * and `moving', and every access a handler makes to what aux names is
 * under it: the hooks below run under it, so a handler that holds it
 * across a step of its own work — the engine call that appends to a
 * stage, say — is a handler no hook can run in the middle of.  The
 * registry lock (Srvctx.fidlk) is a different lock over different
 * things: the list the fids are on, the fid-state point, and the
 * rendered Text.  A caller may take `lk' while holding fidlk, never
 * the other way round.  One caller holds both: the shutdown's sweep
 * (srvfidsclose) runs auxclose with fidlk held, which it may because
 * the service loop has ended and the drain has finished by then, so
 * no attach, clunk or clone-walk is behind it.  Nothing else in srv/
 * holds fidlk across a hook or across an engine call.
 *
 *	auxflush  runs from srvstep7, on the fid of a request that is
 *		  unwinding flushed (layer-a §5.4.1 step 7), once per
 *		  such request and before it responds.  The store is
 *		  open and the fid lives on: this is where a stage the
 *		  flushed request staged is discarded, not where the
 *		  fid's own state is given back.
 *
 *		  Where it runs from is the rest of what it must
 *		  tolerate.  It runs on the queue proc that is unwinding
 *		  the request (from srvqdone), OR on the service loop
 *		  (from srvqflush, for a request flushed while it was
 *		  still queued — with that request's own Qreq.lk held, so
 *		  the hook takes neither that lock nor the registry lock,
 *		  and blocks on nothing a queue proc holds across device
 *		  I/O or a park).
 *
 *		  Which ENGINE calls that leaves it.  store.md §9's three
 *		  — objsnapent, objsnapcount and objsnapclose, the ones
 *		  §9 also allows after the store has closed — are the ones
 *		  a hook may make, and enum.c's makes the last of them.
 *		  They take the engine's state lock and nothing else, and
 *		  store.md §6 rule 2 keeps every state lock off the device
 *		  and out of a park, so a hook on the service loop waits
 *		  for one queue proc's hold of that lock and no longer.
 *		  The lock order is one-directional and has no cycle: a
 *		  hook takes the fid's state lock and then the engine's,
 *		  a queued handler takes the same two in the same order,
 *		  and no engine path takes a fid's state lock or a Qreq's
 *		  — the engine knows nothing of either.  What a hook may
 *		  NOT do is make a call from inside a LEAF lock of this
 *		  server's, which is the rule the staged update's discard
 *		  is parked by (Sstage below), or make one that reaches
 *		  the device.
 *		  9P allows two requests to be outstanding on one fid,
 *		  and two requests naming one object share a queue, so
 *		  another request may be part-way through this same fid
 *		  when the hook runs: `lk' is what keeps the hook between
 *		  two steps of that handler rather than inside one, and
 *		  the handler sees the discarded state at its next access.
 *		  A handler must therefore tolerate finding its fid's
 *		  state already discarded — including the flushed
 *		  request's own handler, which lib9p may start right after
 *		  a loop-side step 7 has run for it (/sys/src/lib9p/queue.c:
 *		  _reqqueueproc sets q->cur before the handler marks the
 *		  request running, so a Tflush in that window takes the
 *		  step-7-here path and reqqueueflush then interrupts the
 *		  proc instead of unlinking the request).
 *	auxclose  runs before the store closes, and at clunk; it may
 *		  call the engine.  A stage handle MUST be discarded
 *		  here: store.md §9 allows only objsnapent, objsnapcount
 *		  and objsnapclose after the store has closed.
 *	auxfree	  runs last, after the store may already have closed
 *		  (D16): it may only release memory and close an
 *		  Objsnap, which is the one thing §9 lets outlive it.
 *
 * So a fid that stages a write may see auxflush any number of times
 * while it lives, and sees auxclose and then auxfree exactly once for
 * each state it holds.
 *
 * A fid that moves — a walk of a fid onto itself that resolves — gives
 * its state back the same way before it takes the new file's, so no
 * state and no hook survives the move.  The server keeps its own
 * registry of the live fids (ctx, prev, next), because lib9p exposes
 * no way to iterate them and srvfidsclose must reach every one.
 */
struct Sfid
{
	int	file;		/* index into srvfiles */
	int	role;
	int	hasepoch;
	uvlong	epoch;		/* the attach epoch, when one was given */
	char	peer[Iidlen+1];	/* role=repl's peer= */
	uchar	oid[Oidmax];
	int	oidlen;
	uvlong	qidpath;
	uvlong	qidvers;
	Text	*text;		/* the render-at-open snapshot, once open */
	QLock	lk;		/* over aux, the three cells, auxclosed, moving */
	void	*aux;
	void	(*auxflush)(Sfid*, Req*);
	void	(*auxclose)(void*);
	void	(*auxfree)(void*);
	int	auxclosed;	/* auxclose has run for this state */
	int	auxbusy;	/* srvauxpoint: a handler is mid-step on it */
	int	moving;		/* a create cell is moving this fid (above) */
	Srvctx	*ctx;		/* the registry's, and the hooks' */
	Sfid	*prev;
	Sfid	*next;
};

/*
 * What srvqpush leaves in Req.aux.  store.md §7 has the server keep
 * the Reqqueue* there so Srv.flush can find the queue the request was
 * pushed to; this keeps the handler and the ctl row beside it, so a
 * queued ctl verb can be parsed on the service loop and run on the
 * queue.  It is freed by Srv.destroyreq, which lib9p runs when the Req
 * itself goes — after any parked Rflush has been answered, so a Tflush
 * that finds the request through the pool always finds this too.
 *
 * `done' and the lock over it are what keep a Tflush and the request's
 * own completion apart: lib9p's reqqueueflush answers a request it
 * does not find running, whether or not it found it queued either, so
 * a flush that reached it after it had answered would answer it twice.
 * srvqdone marks the request under this lock before it responds and
 * srvqflush holds the lock across reqqueueflush, so exactly one of the
 * two runs (queue.c).
 *
 * `pushed' says the request was handed to that Reqqueue.  It is not a
 * formality: Reqqueue.flush is per QUEUE, raised for the queue's
 * current request and cleared when its proc takes the next one, so a
 * request that was prepared here and then answered on the service loop
 * would otherwise read a flag raised for a stranger and answer itself
 * `interrupted'.  `running' says the queue's proc is inside the
 * handler, which is what decides where step 7 runs on a flush, and
 * `step7' records that it has run, so the two places cannot both do it.
 */
struct Qreq
{
	Srvctx	*ctx;
	Reqqueue *q;
	QLock	lk;		/* over the four flags, against srvqflush */
	int	done;		/* srvqdone has taken this request's exit */
	int	pushed;		/* handed to the queue: its flush flag is ours */
	int	running;	/* the queue's proc is inside the handler */
	int	step7;		/* layer-a §5.4.1 step 7 has run for it */
	void	(*f)(Req*);
	Sctl	*ctl;		/* the verb row, for a queued ctl write */
	Cmdbuf	*cb;		/* its parsed line */
	uchar	oid[Oidmax];
	int	oidlen;
};

/*
 * One unit of work a caller that is NOT a request runs on a queue —
 * the background passes' entry to the pool, srvqjob (queue.c).  The
 * Req is the pool's to carry and so comes first; the Qreq beside it is
 * this unit's own, because the push path allocates nothing.
 *
 * What such a unit is not.  It has no tag, so no Tflush can name it
 * and reqqueueflush cannot reach it; it is not in lib9p's Req pool, so
 * it takes no reference to the Srv and lib9p never frees it.  It
 * therefore never enters respond, and its handler is a plain function
 * of the caller's rather than a Req handler: it MUST NOT respond, MUST
 * NOT leave through srvqdone, and MUST NOT be flushed.  srvqjob is
 * what leaves through srvqended instead, which is the completion the
 * pool is owed and srvdestroyreq would have taken for a real request.
 *
 * `done' and the two beside it are the caller's wait: the queue's proc
 * marks it and wakes the caller, which is what holds this structure —
 * the caller's own stack — alive for as long as the queue is in it.
 */
struct Qjob
{
	Req	r;		/* the pool carries this; keep it first */
	Qreq	qr;		/* this unit's own: no malloc on the push */
	QLock	lk;
	Rendez	rz;
	int	done;
	void	(*fn)(void*);
	void	*arg;
};

/*
 * One ctl verb, layer-a §2.5.  The table is extended by adding rows;
 * a row is gated before it runs, in §2.5's order — the role, then the
 * fence, then the arguments — so a verb whose body is not built still
 * has a complete and testable gate.
 *
 * narg is the number of arguments after the verb: a row accepts
 * nargmin..nargmax of them.  `fenced' marks §2.5's fenced set: the
 * verbs that mutate data or replication state, refused with `fenced'
 * while the instance is fenced.
 *
 * A row fills exactly one of fn and qfn.  fn runs on the service loop
 * and answers nil or an error string.  qfn runs on the oid's Reqqueue
 * — argv[0] of such a verb is the oid, which the framework validates
 * and hashes before the push — and responds through srvqdone.
 *
 * A qfn row therefore has nargmin >= 1: argv[0] must be there for the
 * framework to read.  That invariant is enforced where the table is
 * consumed, and a row that breaks it answers `bad ctl'.  No row here
 * can reach that refusal — §2.5 gives every verb that names an object
 * at least one argument — so it is a check on a row added wrong, not
 * a path a client can drive.
 */
/*
 * One background pass, as /jobs reports it (layer-a §2.2: "one line
 * per running or queued background job").  A pass is a proc of its
 * own holding one of srv.h's jobs for its whole run, so the shutdown
 * waits for it; this record is what the pass is visible as while it
 * runs, and it is on Srvctx.jobs from the moment the verb that starts
 * it is accepted — §2.5 has such a verb "return success once the job
 * is accepted", so the line is there before the proc has run a step,
 * which is what `queued' means in it.
 *
 * The counters and `err' are the pass's own to write and /jobs' to
 * read under Srvctx.joblk.  Nothing outside a line of /jobs consumes
 * them: §2.2 makes that file's format implementation policy beyond
 * its being one record per line.
 *
 * `err' is what a pass gave up with, and it is the only record of it:
 * a pass has no client to answer and no log to write to, so a walk
 * that broke off silently was a walk that reported success.  The
 * first failure wins, since it is the one that stopped the walk where
 * a walk stops at all — an object that would not read does not stop
 * one — and `err' is also what says a count on the line is not the
 * whole store's: a reclaim pass cut short carries `shoalsrv: stopped'
 * beside the `reclaimable=' it did reach (store.md §14(31)).
 */
struct Sjob
{
	Sjob	*next;
	Srvctx	*ctx;
	char	*verb;		/* the ctl verb that started it */
	void	(*fn)(Sjob*);
	int	running;	/* the proc has started: `queued' until then */
	uvlong	done;		/* units walked: index slots, tomb entries */
	uvlong	total;		/* units to walk */
	uvlong	bad;		/* objects the pass found mismatching */
	uvlong	skipped;	/* ... gone between the index and the queue */
	uvlong	reclaimable;	/* tombstones past layer-a §1.5's local two */
	uvlong	dropped;	/* dirty records `forget' discarded */
	char	arg[Iidlen+1];	/* `forget's peer, an instance id (§3.3) */
	char	err[ERRMAX];	/* what the pass gave up with, or empty */
};

struct Sctl
{
	char	*verb;
	int	roles;
	int	fenced;
	int	nargmin;
	int	nargmax;
	char*	(*fn)(Srvctx*, Sfid*, int, char**);
	void	(*qfn)(Req*);
};

extern Sctl srvctls[];
extern int nsrvctls;

/*
 * The staged operation a fid holds, layer-a §5.4 step 3 and store.md
 * §3.6 — what `aux' above names on a fid that is staging, and the one
 * thing the three hooks beside it were written for.  A fid holds at
 * most one, which is why the slot is the fid's rather than a list.
 *
 * Two surfaces stage, and they differ only in what the handle owns.
 *
 *	A CLIENT operation on /obj/<oid> — a write, a create, a truncate
 *	or a remove — stages at §5.4 step 3 and gives the stage back at
 *	step 6 or step 7, inside the one request.  What it holds is the
 *	key step 3 chose and nothing else: the bytes of a write stay the
 *	Req's, and the commit reads them from it.  They can, because such
 *	a stage never outlives its request and lib9p holds the Req's
 *	buffer until the handler responds — and they must, because a
 *	stage that owned them would take a commit's argument with it when
 *	step 7 discarded the stage under the call.  The commit is an
 *	engine call that stages and publishes in one (objwrite and
 *	friends), so a discard that lands while that call is in flight
 *	does not unmake it — layer-a §5.4.1's "MAY or MAY NOT have been
 *	applied" — and what the handler owes is not to commit AFTER a
 *	discard, which is what the look before the call is for.
 *
 *	An op=full or op=create on a /repl fid (§5.5, store.md §3.6)
 *	stages across MANY Twrites and the handle holds the engine's
 *	Stage: created by the first chunk, added to by each one, and
 *	consumed by final=1 — which consumes it on every outcome, so
 *	whatever owns the fid forgets the handle there (§3.6).  That is
 *	the lifetime this state exists for, and it is the replication
 *	surface's to fill in; nothing in the client paths above produces
 *	a stage that outlives its request.
 *
 * The rules are the same for both, and they are the fid's:
 *
 *	one per fid.  A second is refused `disk full', the pick §2.6's
 *		set offers for a bound §3.6 does not define (store.md
 *		§14(42)), and so is an update covering more than
 *		`stagemax' allows — grains for a /repl stage, which is
 *		§3.6's own quantity and which the ENGINE charges against
 *		the handle for every chunk but the first; checksum blocks
 *		for a client write, which is this server's own quantity
 *		(store.md §14(37)).  A client write is SHORTENED to that
 *		bound rather than refused (layer-a §2.4's short write), so
 *		only a fid that already holds a stage reaches the refusal.
 *	discarded by step 7, through auxflush, whichever of the fid's
 *		requests was flushed: the stage is the fid's, and a stage
 *		spanning several Twrites has no one request to belong to.
 *		The hook does not make the ENGINE call that releases the
 *		handle, and a builder filling `g' must not give it one.
 *		Not because a hook may make no engine call — it may make
 *		store.md §9's three, and the enumeration's hook makes one
 *		of them (Sfid above) — but because of WHERE this one would
 *		be made: the hook reaches the handle under `stagelk', the
 *		context's leaf lock, and store.md §6 rule 1 takes no state
 *		lock under a leaf.  The hook therefore marks the stage dead
 *		and leaves the release to someone else; which someone is
 *		what `busy' decides.
 *
 *		A stage no handler is inside has its handle taken out of
 *		the slot and parked, and obj.c's drain — at the head of
 *		every queued object operation, and once at the shutdown
 *		while the store is still open — is where the discard is
 *		made, outside every lock.  A park that cannot take the
 *		handle puts it BACK in the slot, dead but not released,
 *		rather than make the call there: auxclose below reaches
 *		the slot whatever the sweep has done with the stage.
 *
 *		A stage that IS busy keeps what it holds, and that chunk's
 *		own handler is what releases it — that handler and NOBODY
 *		ELSE (store.md §14(44)).  Where in the handler depends on
 *		what the chunk still owes: the look its engine call returns
 *		to is the release for a chunk that is not the last, and for
 *		a final=1 chunk whose write went through the look keeps
 *		`busy' set and the give-back behind it is the release
 *		(obj.c's srvstagelive and srvstagefinal), since that chunk
 *		holds the stage across the arbitration between the two.
 *		That is the chunk that gets PAST its look: the look answers
 *		on whether the stage is still the fid's and still live
 *		BEFORE it consults the mark it was asked to keep, so a
 *		final=1 chunk the hook reached while its write was in
 *		flight is answered `stage expired' there and gives the
 *		stage back at its look like any other.  A final=1 chunk
 *		whose write FAILED asks for no mark either: its look clears
 *		`busy' and leaves the stage in the slot for a later chunk
 *		or the clunk (peer.c).  Not the hook, whose park would be a
 *		discard made under the stagewrite the handle is an argument
 *		of; not the idle sweep, which `busy' holds off (§3.6); and
 *		not a refusal running on another queue — a chunk naming a
 *		second object finds the stage dead, answers `stage expired'
 *		and leaves the slot to that handler for the same reason,
 *		which is the exception §14(43) grants its own rule (obj.c's
 *		srvstagemore).  So what the fid owes is that the handle is
 *		released by someone that is not the hook, and while a
 *		handler is inside a step on it, that someone is the
 *		handler itself.
 *
 *		auxclose is the exception to the NOBODY ELSE, and it is
 *		one because it cannot land there.  stageclosehook
 *		releases whatever the slot holds, busy or not: the clunk
 *		and the shutdown are the last hands an engine stage can be
 *		given back by (store.md §9), so one that waited on a
 *		handler's look would be one the store outlives.  Neither
 *		caller runs while a chunk is inside.  lib9p holds the Fid
 *		across an outstanding Twrite, so the destroy behind a
 *		Tclunk runs only once that chunk has responded; the
 *		shutdown's own sweep runs behind srvqdrain, with every
 *		request in flight already finished (srv.c); and the third
 *		caller, the create cell's give-back, runs over an
 *		enumeration's snapshot and never over a stage (tree.c's
 *		srvfidgive).
 *
 *		`busy' says a handler is inside a step on the stage, handle
 *		or NOT: the opening chunk of a transfer is busy from the
 *		moment it stages, ACROSS the arm that fills `g' — which
 *		leaves the mark set for that caller (obj.c's srvstagefull)
 *		— and until the look behind it.  stageopen runs in that
 *		window with no lock held.  A stage taken out of the slot
 *		there is one the arm, and the look behind it, reach after
 *		it has been freed — so what the qualifier is on is the
 *		handler and not the handle.
 *	discarded at clunk and before the store closes, through
 *		auxclose, because releasing an engine stage is an engine
 *		call (store.md §9).
 *	discarded by the idle sweep after `stagems' of no arrivals
 *		(§3.6), which strips the handle and leaves the memory to
 *		the clunk behind it.  `busy' says a handler is inside a
 *		step on it, which is not an absence of arrivals.
 *	freed by auxfree, which may run after the store has closed
 *		(D16) and therefore releases memory and nothing else.
 *
 * The fid's own state lock (Sfid.lk) covers the slot; the list below
 * is the server's, under Srvctx.stagelk, so that the sweep walks it
 * without touching the fid registry.  A fid's lock may be held over
 * stagelk and never the other way round, and NEITHER is held across a
 * park: a request parked under the fid's lock wedges the service loop,
 * which takes that lock to perform step 7 for another request on the
 * same fid (queue.c), and one parked under stagelk stalls every queued
 * object operation, each of which sweeps at its head.  What keeps the
 * sweep off a stage a handler is between two steps of is `busy'.
 */
enum
{
	Stwrite	= 0,		/* layer-a §2.4's write */
	Stcreate,		/* ... create */
	Sttrunc,		/* ... truncate, extend and OTRUNC */
	Stremove,		/* ... remove */
	Stfull,			/* §5.5's op=full/op=create, through Stage */
	Stpoint,		/* srvstagepoint's: an engine handle, no update */
};

struct Sstage
{
	Srvctx	*ctx;
	int	kind;
	uchar	oid[Oidmax];
	int	oidlen;
	uvlong	ver;		/* the key §5.4 step 3 chose */
	uvlong	wepoch;
	uvlong	off;
	/*
	 * §5.5's op=full declares the final length and `force' on EVERY
	 * chunk and requires them identical on each, so the transfer's
	 * pair is kept here and every later chunk is checked against it;
	 * they are the two values stageopen fixed the engine handle with.
	 * A client stage has neither and leaves both zero.
	 */
	uvlong	flen;
	int	force;
	vlong	last;		/* nsec of the last arrival */
	int	busy;		/* a handler is inside a step on it */
	int	dead;		/* the sweep expired it, or step 7 took it */
	int	released;	/* what it held has been given back */
	int	linked;		/* it is on the context's list */
	Stage	*g;		/* §5.5's engine handle, when it has one */
	Sstage	*prev;
	Sstage	*next;
};

/*
 * The adopted cluster map, layer-a §6.3 — and the handle every reader
 * of it takes.
 *
 * A snapshot is IMMUTABLE once published: the bytes the map was
 * adopted from, the `Cmap' they parsed to, and this instance's own
 * record resolved inside that `Cmap'.  Nothing ever writes a field of
 * a published snapshot, so a reader needs no lock over the contents —
 * only over the pointer that says which snapshot is in force, and over
 * the count that says how many readers still hold it.
 *
 * That pointer and that count are what `Srvctx.maplk' covers, and
 * nothing else.  It is a LEAF and a spin `Lock': it is held across a
 * pointer copy and an int, never across an engine call, a device
 * access, a park or another lock, and no lock of this server or of the
 * engine is ever taken under it (store.md §7 rules 1 and 2).  It may
 * itself be taken under anything, which is what lets a render on the
 * service loop and a handler on a queue proc reach the map the same
 * way.
 *
 *	srvmapget	answers the snapshot in force with a reference of
 *		the caller's.  It never answers nil: a context that has
 *		been built has a snapshot in force for as long as it
 *		exists, and the start-up refusals free the context
 *		outright (srv.c).
 *	srvmapput	gives that reference back.  The LAST holder of a
 *		snapshot no longer in force is what frees it — which may
 *		be the swap that replaced it, or a request that has been
 *		parked across the swap; the free runs outside the lock,
 *		and it is safe there because a snapshot at zero is one
 *		nothing can reach to count up again.
 *	srvmapswap	publishes a new snapshot and gives the old one's
 *		installed reference back (srv.h).  It is T1's alone: this
 *		build has no monitor client, so nothing swaps in
 *		production (store.md §14(18), §14(52)).
 *
 * What a swap guarantees to a request already in flight is that its
 * own snapshot does not change under it.  A holder therefore reads
 * `map', `text' and `self' as ONE map for as long as it holds — the
 * epoch and the placement it admitted under, and the `self' it
 * compares a placement member against, are the same map's — and a
 * pointer into `map->inst[]', which is what mapplace and mapprimary
 * answer and what `self' is, stays live for exactly that long.
 *
 * Who may hold one, and for how long:
 *
 *	A QUEUED OBJECT OPERATION takes one at the head of its handler
 *		and holds it to the end of that handler's BODY, across its
 *		engine calls and across every -X hold point it parks at
 *		(srv.h).  That is the hold the rule above is for: §5.4
 *		step 1's admission, the epoch the stage is keyed with and
 *		step 5's placement are three reads of one map, and a swap
 *		landing between any two of them moves none of them.
 *	A RENDER takes one for the whole of what it renders and gives it
 *		back before it answers: /map writes `text' entire, so the
 *		file is one snapshot's bytes and never a mix of two
 *		(status.c), and /status's `status=' and `up=' are one
 *		record's pair.
 *	A PASS or a TIMER TICK takes one per pass or per tick and not
 *		per read, so a walk's `tombdays' and its epoch are one
 *		map's (job.c).
 *	NOBODY holds one beyond the request, render, pass or tick that
 *		took it.  A hold is not a lock — it blocks no swap and
 *		bars no other reader — but it does keep the memory alive,
 *		so a hold left behind is a leak, and one kept in a fid or
 *		a stage would be a snapshot outliving its reader.
 *
 * A HOLD NEVER SPANS THE REPLY.  Every hold above ends before the
 * respond that answers the message it was taken for — the queued
 * handlers give theirs back in the wrapper and answer from what the
 * body returned (obj.c), a render gives it back before it returns to
 * srvopentext, and the loop's own readers (tree.c's F3, attach.c,
 * peer.c's epoch check) get and put strictly before they answer.  The
 * reason is that the reply is where lib9p counts the request complete
 * — srvdestroyreq, from closereq, INSIDE respond — and where it then
 * releases the service, which is what srvqdrain converges on and what
 * srvfree waits for.  A hold that outlived the respond would therefore
 * be a hold outliving the context: srvfree's put would leak the
 * snapshot and the holder's own put would run on freed memory (srv.c).
 *
 * One 9P message may be more than one reader, and where it is, it may
 * read two maps.  Three places: a row's GATE runs on the service loop
 * before the queued handler behind it (tree.c's F3), /meta's open
 * admits on the queue and then calls a RENDER that takes its own
 * (obj.c), and a /repl or /rpc write is epoch-checked on the loop
 * before the operation is pushed (peer.c).  Each half is one map's,
 * which is what the rule above buys; the pair is not.  Nothing swaps
 * in this build, so nothing observes it — store.md §14(52) records it
 * as the refresh loop's to close.
 *
 * The WRITE PATH's epoch is the rule this exists for.  A write is
 * keyed with the epoch of the map it was ADMITTED under: read once, at
 * the stage of §5.4 step 3, and carried in `Sstage.wepoch' to the
 * engine call that commits it.  Nothing between the stage and the
 * commit re-reads the map's epoch, so a swap landing in that window —
 * including one landing while the request is parked at `objprelook' or
 * `objstage' — produces a commit stamped with the epoch the write was
 * admitted under and not with the epoch now in force.  The next write
 * on that object takes the new epoch, because its own admission does.
 *
 * `self' points INTO `map->inst[]' of this snapshot and nowhere else.
 * It is resolved once, when the snapshot is made, from this
 * instance's own uuid, which layer-a §3.4 makes the disk's (srv.c) —
 * so it is the record every placement member is compared against, and
 * it is valid for exactly as long as the hold that answered it.
 */
struct Smap
{
	Cmap	*map;		/* the parsed map; never written after this */
	char	*text;		/* the bytes it was parsed from, for /map */
	long	len;		/* ... and how many of them */
	Cinst	*self;		/* our own record, inside map->inst[] */
	int	ref;		/* holders, under Srvctx.maplk */
};

struct Srvctx
{
	Srv	srv;
	Srvcfg	cfg;
	Dev	*dev;
	Store	*store;
	Super	sb;		/* the copy superselect chose */
	/*
	 * §6.3's adopted map, behind the handle above.  The lock covers
	 * these three fields and nothing else: which snapshot is in
	 * force, and how many exist for srvmapcount to report.
	 */
	Lock	maplk;
	Smap	*smap;
	int	nsmap;		/* snapshots made and not yet freed */
	char	iid[Iidlen+1];
	char	uuid[Uuidlen+1];
	char	monid[Monidlen+1];
	int	adoptflags;	/* layer-a §6.3's, for /status */

	Fence	fence;		/* §6.4; F1 is inert here, F4 is live */
	QLock	fencelk;

	/*
	 * The live fids, and the T1 fid-state point over them.  Every
	 * Sfid is on this list from the attach or walk that made it
	 * until destroyfid.  This lock covers the list, the point below
	 * it and the fids' rendered Text — NOT the per-fid state the
	 * three hooks are called on, which has a lock of its own
	 * (Sfid.lk) so that no hook and no engine call runs under this
	 * one: a hook may reach the engine, and a lock held across
	 * device I/O here would block every attach, clunk and
	 * clone-walk with it (docs/design/store.md §7 rule 2).
	 */
	QLock	fidlk;
	Sfid	*fids;
	int	fidaux;		/* srvauxpoint: fids carry a test state */
	Lock	auxlk;		/* not fidlk: the hooks run under Sfid.lk */
	uvlong	nauxclose;
	uvlong	nauxfree;
	uvlong	nauxopen;	/* ... of those that found the store open */
	uvlong	nauxflush;
	uvlong	nauxlate;	/* auxflush ran after the request responded */
	uvlong	nauxbusy;	/* ... ran while a handler was mid-step */

	Reqqueue **q;
	int	nq;
	Reqqueue *anyq;		/* srvqpushany's, outside the oid pool */

	Lock	cntlk;
	uvlong	npush;
	uvlong	ndone;
	uvlong	ndiverged;	/* §1.3's repairs, applied here (peer.c) */

	QLock	holdlk;
	uvlong	hold;		/* srvhook("objhold") */
	uvlong	prelookhold;	/* srvhook("objprelook") */
	uvlong	stagehold;	/* srvhook("objstage") */
	uvlong	lookhold;	/* srvhook("objlook") */
	uvlong	armhold;	/* srvhook("objarm") */
	uvlong	exithold;	/* srvhook("objexit") */
	uvlong	flushhold;	/* srvhook("flushhold") */
	uvlong	fullhold;	/* srvhook("fullhold") */
	uvlong	openhold;	/* srvhook("openhold") */
	uvlong	finalhold;	/* srvhook("finalhold") */
	uvlong	newhold;	/* srvhook("newhold") */
	/*
	 * How many requests have parked at the three points of the /repl
	 * transfer and at the stage slot's own, counted where they park
	 * and never reset: a test waits on one rather than on a sleep, and
	 * the window each point holds open is exactly what the case is
	 * about (srv.h's srvheld).
	 */
	uvlong	fullheld;
	uvlong	openheld;
	uvlong	finalheld;
	uvlong	newheld;
	/*
	 * ... and at the client write path's own, which is the window a
	 * map swap has to be driven against: a write parked there has
	 * been admitted and keyed and has not yet read the placement
	 * (srv.h's objprelook, and the map handle below).
	 */
	uvlong	prelookheld;
	uvlong	mapopen;	/* srvhook("mapopen") */
	uvlong	walkhold;	/* srvhook("walkhold") */
	uvlong	anyexit;	/* srvhook("anyexit") */
	uvlong	step7hold;	/* srvhook("step7") */
	uvlong	jobhold;	/* srvhook("jobhold") */
	uvlong	slotfail;	/* srvhook("slotfail") */
	uvlong	reclaimhold;	/* srvhook("reclaimhold") */
	uvlong	tickhold;	/* srvhook("tickhold") */
	uvlong	dirhold;	/* srvhook("dirhold") */
	uvlong	givehold;	/* srvhook("dirgive") */
	uvlong	givecnt;	/* opens that have reached that window */
	uvlong	claimhold;	/* srvhook("objclaim") */
	uvlong	endhold;	/* srvendpoint: ms held in srvqended */

	/*
	 * The background jobs of §9's quiesce: work that is inside the
	 * engine and is not a Req, so the drain above cannot see it.
	 * A pass proc takes one of these for its whole run.
	 */
	Lock	joblk;
	int	njob;
	/*
	 * The passes /jobs lists, and the scrubber's own policy: the
	 * rate `scrub rate=' sets, in KiB/s, and the flag `scrub stop'
	 * raises, which a running pass tests between objects exactly as
	 * it tests srvstopping.  `scrubbing' is what keeps a second
	 * `scrub start' from putting two passes over one index.
	 *
	 * The tombstone reclaim walk carries the same pair of flags, for
	 * the same two jobs — one pass over one index, and a stop a
	 * running pass reads between entries — and they are its own, not
	 * the scrub's: the two walks run at once and neither stops the
	 * other (job.c).  `reclaimms' is the T1 knob over the timer's
	 * period and `reclaimup' says the timer proc is still reading
	 * this context, which is what the shutdown waits for; the timer
	 * holds no job, since it makes no engine call.
	 */
	Sjob	*jobs;
	int	scrubbing;
	int	scrubstop;
	ulong	scrubrate;
	int	reclaiming;
	int	reclaimstop;
	int	reclaimup;
	uvlong	reclaimms;
	int	stopping;	/* the shutdown has begun: no new jobs */
	int	served;		/* a service loop was started over this context */
	int	released;	/* lib9p has let go of the Srv (Srv.free) */

	int	closed;		/* the store has been closed */

	/*
	 * The stages the live fids hold (obj.c).  A leaf lock: nothing is
	 * taken under it and nothing parks under it, and a fid's own state
	 * lock is the one that may be held over it.
	 */
	QLock	stagelk;
	Sstage	*stages;
	int	nstage;
	int	stagept;	/* srvstagepoint */
	uvlong	nstagedone;	/* stages given back */
	uvlong	nstageopen;	/* ... of those that found the store open */
	Stage	**pend;		/* engine handles awaiting their discard */
	int	npend;
	int	apend;
	int	pendfull;	/* srvstagependfull: the park refuses */
	uvlong	nstagepend;	/* how many have been parked */
};
