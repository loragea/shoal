/*
 * shoalsrv: the storage instance's 9P service, docs/design/layer-a.md
 * §2.  This is the library; cmd/shoalsrv/main.c is argument parsing
 * and start-up over it, and test/srvtest.c drives the same calls over
 * a pipe.  The 9P surface is here rather than in the command because
 * a T1 test links libraries and execs nothing (AGENTS.md), and the
 * whole of §2 — attach, the tree, the queue pool, Tflush, the ctl
 * framework — is what T1 has to drive.
 *
 * Include after <u.h>, <libc.h>, <libsec.h>, <fcall.h>, <thread.h>,
 * <9p.h> and "../lib/shoal.h".
 *
 * libshoal itself knows nothing of lib9p or libthread and must not
 * learn: the engine takes a spawn callback and uses QLock, Rendez and
 * Lock alone (docs/design/store.md §7), so the same engine runs under
 * a plain-libc T1 program and under this libthread server.  The
 * dependency goes one way, from here to there.
 *
 * srv/dat.h and srv/fns.h are private to srv/.  What a builder of the
 * rest of the surface needs is here and in dat.h's tables.
 */

typedef struct Srvctx Srvctx;
typedef struct Srvcfg Srvcfg;
typedef struct Text Text;

/*
 * Srvctx is opaque outside srv/: its definition is in srv/dat.h.
 * 2c(1)'s type signatures are computed from the C signof operator, so
 * a function taking a Srvctx* signs differently in a file that has the
 * definition and one that has not; the pragma is what that mechanism
 * provides for exactly this case, as lib/shoal.h does for Store.
 */
#pragma incomplete Srvctx

enum
{
	/* layer-a §2.1's roles, and the access matrix's bit per role */
	Rclient		= 0,
	Rrepl,
	Radmin,
	Nrole,

	Aclient		= 1<<Rclient,
	Arepl		= 1<<Rrepl,
	Aadmin		= 1<<Radmin,
	Aall		= Aclient|Arepl|Aadmin,

	/*
	 * The msize floor.  lib9p answers Tversion itself and offers no
	 * hook (9p(2): Srv has none), so the only place a server can see
	 * the negotiated size is the first request that carries a Srv*,
	 * which is Tattach.  store.md §14(20) records that.
	 */
	Msizemin	= 8192+IOHDRSZ,

	Nqueuedflt	= 64,		/* store.md §7's default pool */
	Nqueuemax	= 4096,

	/*
	 * The stack every proc of this service gets, and the value a
	 * program linking this library MUST set `mainstacksize' to before
	 * it makes a Srvctx.  That is not a convention: lib9p creates the
	 * Reqqueue procs itself, with the program's mainstacksize
	 * (9pqueue(2), /sys/src/lib9p/queue.c), so this library cannot
	 * size them and a program that leaves the default in place gets
	 * queue procs too small for the engine code they run — a queue
	 * proc composes a blksz block and builds a record on its stack
	 * (store.md §7).  The procs this library does create, the
	 * engine's among them, take this size through srvspawn.
	 */
	Srvstack	= 256*1024,
};

/*
 * A render-at-open snapshot.  layer-a §2.2 makes snapshot-at-open a
 * MUST for the small attr=value files: the bytes are composed once,
 * when the fid is opened, and every Tread on that fid is served out
 * of them, so a concurrent mutation cannot tear a read.  /status and
 * /map are rendered this way; so are the status files that are not
 * built yet, which is why the helper is public.
 */
struct Text
{
	char	*p;
	long	n;
	long	max;
	int	err;		/* an allocation failed: the text is short */
};

Text*	textnew(void);
void	textfree(Text*);
int	textprint(Text*, char*, ...);
int	textwrite(Text*, void*, long);
#pragma	varargck	argpos	textprint	2

/*
 * The error API, docs/design/store.md §3.7.  Its mapping rule is
 * normative: a condition layer-a §2.6 names MUST be answered with
 * §2.6's prefix and nothing else, and an internal-invariant or device
 * error MUST NEVER begin with one.  Every handler turns an engine
 * failure into an Rerror through these and through nothing else.
 *
 *	srv26		the §2.6 prefix a string carries, or nil.  A
 *			prefix matches when it is the whole string or is
 *			followed by ": " — §2.6's own detail form, which
 *			`not primary: n5.0' and D20's `disk full: <n>
 *			object snapshots open, objsnapmax <max>' use.
 *	srverrs		what goes on the wire for one error string: a
 *			§2.6 string verbatim, anything else under this
 *			server's own `shoalsrv: ' prefix.  buf is
 *			written only in the second case.
 *	srverr		srverrs over the current %r.
 *	srvrerror	the answer to the current %r.  A request the queue
 *			pool is carrying leaves through srvqdone instead,
 *			so this is safe to call from a queue proc.
 *
 * Enotbuilt is the local refusal a file or a ctl verb whose body is
 * not built answers after its gates.  It is deliberately not a §2.6
 * condition: nothing layer-a names has happened.
 */
extern char Enotbuilt[];

char*	srv26(char*);
char*	srverrs(char *buf, int nbuf, char *e);
char*	srverr(char *buf, int nbuf);
void	srvrerror(Req*);

struct Srvcfg
{
	Dev	*dev;		/* the store's device, already open */
	char	*maptext;	/* the static cluster map's bytes (-m) */
	long	maplen;
	int	nqueue;		/* -q; 0 takes Nqueuedflt */
	int	noflush;	/* -w, store.md §3.2, reported in /status */
	Storecfg store;		/* the engine's; spawn is filled in here */
};

/*
 * srvnew is the whole of start-up that is not argument parsing: it
 * reads the superblock with superselect (which writes nothing), parses
 * the map, settles this instance's identity from the map record whose
 * uuid= is the disk's (layer-a §3.4), refuses a map whose geometry is
 * not the disk's (store.md §14(8)), refuses one this instance may not
 * adopt (layer-a §6.3), opens the store, makes the adopted epoch and
 * the pinned monid durable before anything is served, and starts the
 * queue pool.  It answers nil with an error string for every one of
 * those refusals.
 *
 * srv9p is the lib9p Srv: a caller that posts the service hands it to
 * threadpostmountsrv, and a caller that speaks 9P over a pipe sets
 * infd/outfd and calls srvrun.  srvrun returns when the connection
 * closes, by which time the shutdown sequence below has run.
 *
 * srvfree releases what is left after the loop has ended.  It does NOT
 * close the store — the shutdown sequence did — and it does not close
 * the device, which stays the caller's.
 *
 * A Srvctx serves ONE service loop.  The loop ending is the shutdown's
 * trigger and the shutdown closes the store, so a second srvrun over
 * the same context would serve a closed one; a caller that wants
 * another connection makes another context.  (lib9p's own multiplexing
 * is inside one loop and is unaffected: threadpostmountsrv serves
 * every client of the posted service over the one channel.)
 */
Srvctx*	srvnew(Srvcfg*);
Srv*	srv9p(Srvctx*);
void	srvrun(Srvctx*, int infd, int outfd);
void	srvpost(Srvctx*, char *name);
void	srvfree(Srvctx*);

/*
 * D16's shutdown, in the order §9's close contract fixes: stop
 * accepting, let the requests in flight drain, stop the 9P loop, and
 * only then close the store.  It is wired to Srv.end, so it runs when
 * the connection closes and a caller does not call it; the trigger is
 * the only part of it that lives in cmd/shoalsrv.
 *
 * After it, the one thing that may still be used is an Objsnap taken
 * before it, through a fid that outlived the loop — which is how the
 * /obj directory fids of the enumeration surface will survive their
 * own store's close.  Per-fid state is freed by Srv.destroyfid, which
 * lib9p runs after Srv.end: a fid's aux is released there, after the
 * store has closed, which is exactly what §9 permits an Objsnap.
 */
void	srvshutdown(Srvctx*);

/*
 * A background job: work inside the engine that is not a Req, which is
 * what a ctl verb that starts a pass proc runs.  The shutdown drains
 * the requests in flight and then waits for these, because store.md §9
 * forbids closing the store while anything is still inside the engine
 * and the drain cannot see a proc that is not a request.
 *
 * A proc takes a job for its whole run: srvjobstart before it touches
 * the engine, srvjobend when it is done, and it answers -1 once the
 * shutdown has begun, which is the answer a verb turns into its
 * refusal.  A pass already running SHOULD test srvstopping between
 * units of work and give up rather than leave the shutdown waiting.
 */
int	srvjobstart(Srvctx*);
void	srvjobend(Srvctx*);
int	srvstopping(Srvctx*);

/* what a caller and the tests read back */
Store*	srvstore(Srvctx*);
Cmap*	srvmap(Srvctx*);
char*	srviid(Srvctx*);
void	srvcount(Srvctx*, uvlong *pushed, uvlong *done);

/*
 * store.md §13's -X shape, for this library's own points: inert until
 * set, present in every build.
 *
 *	objhold	n != 0 holds every queued object request at its check
 *		point, so a test can have a request that is running and
 *		one that is still queued at a known moment.
 *	objexit	n != 0 holds every queued object request at the other
 *		end of its handler: after the engine call and before the
 *		exit, so a test can flush a request whose work is done
 *		and require it to leave through srvqdone all the same.
 *	mapopen	1 offloads a Topen of /map to the reserved queue
 *		srvqpushany uses and holds it there until the point is
 *		cleared; 2 prepares that open for a queue and then
 *		answers it on the service loop after all.  It is how a
 *		test drives the offload path before a row of the tree
 *		really needs one.
 *	walkhold
 *		n != 0 holds a queued walk that moves its fid at the
 *		commit: after the fid's old state has been given back and
 *		before the new one is written.  That is the window in
 *		which the service loop is making and unmaking fids on the
 *		same registry, so it is where a test drives an attach
 *		against a walk.
 *	flushhold
 *		n != 0 holds a Tflush of a pooled request between the
 *		lookup that found it and the flush itself, which is the
 *		window a completion racing a flush lives in.  This one
 *		holds the SERVICE LOOP, not a queue proc, so a test that
 *		leaves it set answers nothing else either.
 *
 * A held request leaves either hold when the point is cleared or when
 * its queue's flush flag is set, whichever is first; the shutdown
 * clears every point, so a request left holding cannot hold it up.
 *
 * These points are reachable in-process only.  store.md §13's -X flag
 * names the points of the device under the store — it is what drives
 * on the real device the same named points a test drives on the
 * simulated one — and cmd/shoalsrv routes the flag there; a server
 * point is set by the program that holds the Srvctx, which is a T1
 * program.  Nothing on the wire reaches one either: layer-a §2.5 fixes
 * the ctl grammar, so a debug verb would be a wire change.
 */
void	srvhook(Srvctx*, char *name, uvlong n);

/*
 * The fid-state point, in the same shape and with the same reach.
 * With it on, every fid this server makes carries a per-fid state of
 * the server's own whose three hooks count themselves, so a test can
 * drive all three — the one step 7 calls on a flushed request, the one
 * that runs before the store closes and the one that runs last —
 * before any row of the tree fills that state with something of its
 * own.  srvauxcount answers how many times each has run, and takes nil
 * for a count the caller does not want; srvauxlate answers how many of
 * the flush hooks ran after their request had already responded, which
 * is what step 7 running too late would look like.  srvauxopen
 * answers how many of the close hooks found the engine still open,
 * which every one of them must.
 *
 * srvfidcount answers how many fids the server's own registry holds —
 * the list the shutdown's sweep follows — which a caller compares
 * against the fids it knows are live.
 */
/*
 * The cell point, which fills handler cells of the file table with
 * cells of the server's own: a read on /ctl, which renders at open as
 * well, and a write on /obj/<oid>.  It is how the rules this library
 * holds around those cells — the gate before a read and a write, a
 * read cell's precedence over a rendered Text, the state a create
 * gives back — are driven before the rows that will carry them are
 * built.  The cells are the file table's, so the point is the
 * program's rather than one context's: set it, drive it, clear it.
 */
void	srvcellpoint(Srvctx*, int on);

void	srvauxpoint(Srvctx*, int on);
void	srvauxcount(Srvctx*, uvlong *flushed, uvlong *closed, uvlong *freed);
uvlong	srvauxlate(Srvctx*);
uvlong	srvauxopen(Srvctx*);
int	srvfidcount(Srvctx*);
