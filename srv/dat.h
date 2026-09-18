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
 * A row with every handler cell nil is a file whose content is not
 * built: after the role gate and the row's gate, the operation answers
 * the local Enotbuilt.  That is deliberate, so the gate matrix is
 * complete and testable before the content is.
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
 * the /obj/<oid> and /meta/<oid> rows entire, belong with object I/O.
 * The two meet in one place: a Tcreate that SUCCEEDS turns the
 * directory fid it is issued on into a fid for the created object, so
 * the create cell is what gives the directory fid's aux back —
 * auxclose, then auxfree — as it sets the fid's file, oid and qid,
 * since one fid cannot hold an enumeration's snapshot and an object's
 * state at once.  A create that FAILS leaves the fid where it was,
 * holding what it held: 9P moves a fid only on a create that
 * succeeded, and layer-a §2.4 keeps the same rule the other way round
 * for remove, where 9P clunks the fid whether or not the remove
 * succeeded.  So the give-back goes after the last refusal the cell
 * can answer, not before it; a cell that gave it back first would drop
 * the directory fid's Objsnap on a create the client will retry.
 * srvfidgive (fns.h) is that give-back: it runs the two hooks in
 * order under the registry lock, which is where the shutdown's own
 * sweep runs them, so the two cannot both close one state.
 *
 * The rows this file leaves unnamed above, by the same rule.  The
 * /meta directory row's open and read cells — and the aux a fid of
 * that row carries while it is a directory fid, which is that read's
 * snapshot — are the enumeration's too: it lists the same objects
 * under a second name.  The five status files —
 * /dirty, /stale, /tombs, /lost and /jobs — are render-at-open text
 * like /status and /map, and each belongs with the state it reports:
 * /dirty and /stale with the dirty set and the stale marks (layer-a
 * §7.1), /tombs with the enumeration, since it is that listing over
 * tombstones (§7.2), /lost with scrub and repair (§7.5), /jobs with
 * whatever starts background passes.  /repl and /rpc are the peer
 * channels: their read and write cells, and the per-fid state a
 * multi-request op stages, belong with the replication surface
 * (§5.5, §5.6), and /advert's read cell — or its render cell, if that
 * advertisement is composed once at open — is that surface's bulk
 * advertisement.  The two channels' gate is already filled, because
 * the fence is this file's (tree.c's chgate); /advert has none,
 * because F1's list names /repl and /rpc alone.
 *
 * The srvctls table below says the same for the verbs: a verb is
 * built by filling its row's fn or qfn, and the body of work that
 * verb names owns that cell — `pull', `push', `reconcile', `advert',
 * `drop' and `forget' with replication, `verify' with object I/O,
 * `scrub' with the scrub pass, and `refresh', `register' and
 * `newmonid' with the monitor client.  `fence' is this file's and is
 * built.
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
 * to hold in mind.  It covers aux, the three cells below it and
 * auxclosed, and every access a handler makes to what aux names is
 * under it: the hooks below run under it, so a handler that holds it
 * across a step of its own work — the engine call that appends to a
 * stage, say — is a handler no hook can run in the middle of.  The
 * registry lock (Srvctx.fidlk) is a different lock over different
 * things: the list the fids are on, the fid-state point, and the
 * rendered Text.  A caller may take `lk' while holding fidlk, never
 * the other way round, and nothing in srv/ holds fidlk across a hook
 * or across an engine call.
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
 *		  the hook takes neither that lock nor the registry lock
 *		  and must not block on anything a queue proc needs).
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
	QLock	lk;		/* over aux, the three cells and auxclosed */
	void	*aux;
	void	(*auxflush)(Sfid*, Req*);
	void	(*auxclose)(void*);
	void	(*auxfree)(void*);
	int	auxclosed;	/* auxclose has run for this state */
	int	auxbusy;	/* srvauxpoint: a handler is mid-step on it */
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

struct Srvctx
{
	Srv	srv;
	Srvcfg	cfg;
	Dev	*dev;
	Store	*store;
	Super	sb;		/* the copy superselect chose */
	char	*maptext;
	long	maplen;
	Cmap	*map;
	Cinst	*self;		/* our own instance record in map */
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

	QLock	holdlk;
	uvlong	hold;		/* srvhook("objhold") */
	uvlong	exithold;	/* srvhook("objexit") */
	uvlong	flushhold;	/* srvhook("flushhold") */
	uvlong	mapopen;	/* srvhook("mapopen") */
	uvlong	walkhold;	/* srvhook("walkhold") */
	uvlong	anyexit;	/* srvhook("anyexit") */
	uvlong	step7hold;	/* srvhook("step7") */
	uvlong	endhold;	/* srvendpoint: ms held in srvqended */

	/*
	 * The background jobs of §9's quiesce: work that is inside the
	 * engine and is not a Req, so the drain above cannot see it.
	 * A pass proc takes one of these for its whole run.
	 */
	Lock	joblk;
	int	njob;
	int	stopping;	/* the shutdown has begun: no new jobs */
	int	served;		/* a service loop was started over this context */
	int	released;	/* lib9p has let go of the Srv (Srv.free) */

	int	closed;		/* the store has been closed */
};
