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
 * names is r->ifcall.name.
 */
enum
{
	Gopen	= 0,
	Gcreate,
	Gremove,
	Gwstat,
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
 *		open, create, remove and wstat.  It answers nil, or the
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
 * Which cells belong with which body of work.  The /obj directory
 * row's open and read cells — and the aux a fid of that row carries
 * while it is a directory fid, which is that read's snapshot — belong
 * with the enumeration of that directory.  That row's create cell, and
 * the /obj/<oid> and /meta/<oid> rows entire, belong with object I/O.
 * The two meet in one place: a Tcreate turns the directory fid it is
 * issued on into a fid for the created object, so the create cell is
 * what gives the directory fid's aux back — auxclose, then auxfree —
 * before it sets the fid's file, oid and qid, since one fid cannot
 * hold an enumeration's snapshot and an object's state at once.
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
 *	auxflush  runs from srvstep7, on the fid of a request that is
 *		  unwinding flushed (layer-a §5.4.1 step 7), once per
 *		  such request and before it responds.  The store is
 *		  open and the fid lives on: this is where a stage the
 *		  flushed request staged is discarded, not where the
 *		  fid's own state is given back.
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
	void	*aux;
	void	(*auxflush)(Sfid*, Req*);
	void	(*auxclose)(void*);
	void	(*auxfree)(void*);
	int	auxclosed;	/* auxclose has run for this state */
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
 */
struct Qreq
{
	Srvctx	*ctx;
	Reqqueue *q;
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
 * consumed, and a row that breaks it answers `bad ctl'.
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
	 * until destroyfid; auxclose runs with fidlk held, so a hook
	 * may reach the engine but must not reach back in here.
	 */
	QLock	fidlk;
	Sfid	*fids;
	int	fidaux;		/* srvauxpoint: fids carry a test state */
	Lock	auxlk;		/* not fidlk: the hooks run under that one */
	uvlong	nauxclose;
	uvlong	nauxfree;
	uvlong	nauxflush;
	uvlong	nauxlate;	/* auxflush ran after the request responded */

	Reqqueue **q;
	int	nq;

	Lock	cntlk;
	uvlong	npush;
	uvlong	ndone;

	QLock	holdlk;
	uvlong	hold;		/* srvhook("objhold") */
	uvlong	exithold;	/* srvhook("objexit") */

	int	closed;		/* the shutdown sequence has run */
};
