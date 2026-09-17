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
 * One file of §2.2.  walk/rd/wr are §2.1's role matrix: which roles
 * may walk to this file, open it for reading, and open it for writing.
 * §2.1 states the matrix by role rather than by file, and is silent
 * about several cells; store.md §14(24) records what those cells are
 * here and why.
 *
 * The three handler cells are what the rest of the surface fills in:
 *
 *	render	the file is a render-at-open text file (§2.2's MUST):
 *		open composes its bytes once into the fid's Text and
 *		every read is served from them.  It answers nil, or an
 *		error string.
 *	read	a read that is not served from a snapshot — a channel
 *		(/repl, /rpc) or a directory read.  It responds.
 *	write	a write.  It responds.
 *	open	a file whose open takes checks of its own (the object
 *		rows' mode rules, layer-a §2.4).  It responds.
 *
 * A row with render, read, write and open all nil is a file whose
 * content is not built: after the role gate, its open answers the
 * local Enotbuilt.  That is deliberate, so the gate matrix is complete
 * and testable before the content is.
 */
struct Sfile
{
	char	*name;		/* nil for the two object rows */
	int	isdir;
	ulong	perm;		/* the mode stat and open report */
	int	walk;
	int	rd;
	int	wr;
	char*	(*render)(Srvctx*, Sfid*, Text*);
	void	(*read)(Req*);
	void	(*write)(Req*);
	void	(*open)(Req*);
};

extern Sfile srvfiles[Nfile];

/*
 * Per-fid state.  A fid is a file of the table plus, for the two
 * object rows, the oid it named; the attach attributes ride on every
 * fid derived from that attach (layer-a §2.1: the role and the epoch
 * travel in aname and a walk clones them).
 *
 * aux and auxfree are the rest of the surface's: an /obj directory fid
 * will hold its Objsnap there and a /obj/<oid> fid its staged write,
 * and destroyfid calls auxfree after the store has closed, which is
 * what D16 lets an Objsnap outlive.  Nothing in this file touches aux.
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
	void	(*auxfree)(void*);
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

	Reqqueue **q;
	int	nq;

	Lock	cntlk;
	uvlong	npush;
	uvlong	ndone;

	QLock	holdlk;
	uvlong	hold;		/* srvhook("objhold") */

	int	closed;		/* the shutdown sequence has run */
};
