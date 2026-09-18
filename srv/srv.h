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
	 * (/sys/src/lib9p/queue.c; 9pqueue(2) does not say so), so this
	 * library cannot size them and a program that leaves the default
	 * in place gets queue procs too small for the code they run — such
	 * a proc composes a blksz block and builds a record on its stack
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
 *	srvintr		is this string the interrupted class?  store.md §0's
 *			rule, applied as lib/dev.c applies it: the last
 *			`: '-separated segment is taken and the word looked
 *			for inside it, so a bare `interrupted', this
 *			server's own `shoalsrv: interrupted' and a segment
 *			that wraps the word once more are all that class,
 *			and a string that merely mentions it earlier is
 *			not.  srvqdone is what acts on it; it is here
 *			because the rule is what tells a flush and a device
 *			interrupt apart (err.c).
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
int	srvintr(char*);

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
 * srvpost is the other half of that pair and is NOT a pairing with
 * srvfree: it posts the service and returns at once, with the loop in
 * a proc of lib9p's making and the context in that loop's hands for as
 * long as it serves.  A caller that must know when serving is over
 * runs the loop itself with srvrun; there is nothing here to wait on a
 * posted one with, and freeing the context under it would pull the
 * server out from under its own clients.
 *
 * srvfree releases what is left after a loop this proc ran has ended.
 * It runs the shutdown if the caller has not, then waits for lib9p to
 * let go of the service — the loop returning is not that moment, since
 * lib9p frees its fid and request pools after it, and those run this
 * library's destroy hooks over the context.  srvreleased is that
 * moment, answered rather than waited for, and it is also what makes
 * srvfree's wait no wait at all on a context that never ran a loop
 * here: a posted service marks nothing, so srvfree on one returns
 * straight away rather than blocking until the last client unmounts.
 * srvfree does NOT close the store — the shutdown sequence did — and
 * it does not close the device, which stays the caller's.
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
int	srvreleased(Srvctx*);
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
 *	step7	n != 0 holds a flushed request inside step 7, between the
 *		read of the flushed fid's cell and the call through it —
 *		which is under that fid's own state lock, so it is where a
 *		test drives a clunk, a moving walk or another request on
 *		that same fid against the proc that is discarding its
 *		state.  Step 7 runs on a queue proc for a request that was
 *		running when it was flushed and on the SERVICE LOOP for one
 *		that was still queued, so this point can park the loop; it
 *		then ends on its own deadline, like the flush hold below.
 *	anyexit	n != 0 holds an offloaded request that has found itself
 *		flushed, before it leaves through srvqdone.  Its proc is
 *		then still inside the handler and has not looped round to
 *		clear the reserved queue's flush flag, which is the one
 *		window in which the loop can prepare a second request for
 *		that queue while the first one's flag is up — and a
 *		request that was never pushed must not read it.
 *	walkhold
 *		n != 0 holds a queued walk that moves its fid at the
 *		commit: after the fid's old state has been given back and
 *		before the new one is written.  That is the window in
 *		which the service loop is making and unmaking fids on the
 *		same registry, so it is where a test drives an attach
 *		against a walk.
 *	jobhold	n != 0 holds a background pass at the end of its run:
 *		after its walk, while its record is still on the job list
 *		and still holds the job the shutdown waits on, and before
 *		the proc unlinks it.  A pass's counters and the error it
 *		gave up with are read from /jobs, and /jobs lists a pass
 *		only while it is running or queued, so this is where a
 *		test reads what a pass finished with instead of racing the
 *		unlink for it.
 *	slotfail
 *		the one point here that refuses rather than holds: n != 0
 *		makes the scrub pass treat its read of index slot n-1 as
 *		having failed.  It is how a whole-index walk is broken off
 *		part-way with the store under it still healthy — the
 *		engine's own way of refusing an index read is to be
 *		condemned, which refuses the rest of the pass's calls too.
 *	flushhold
 *		n != 0 holds a Tflush of a pooled request between the
 *		lookup that found it and the flush itself, which is the
 *		window a completion racing a flush lives in.  This one
 *		holds the SERVICE LOOP, not a queue proc, so a test that
 *		leaves it set answers nothing else either — and it is the
 *		one point the shutdown cannot clear, because the shutdown
 *		runs on the loop it is holding.  It therefore lets go of
 *		its own accord after a few seconds rather than wedging
 *		the server for as long as the program lives.
 *
 * A held request leaves a queue proc's hold when the point is cleared
 * or, for the holds that name the request, when its queue's flush flag
 * is set, whichever is first; the shutdown clears every point, so a
 * request left holding cannot hold it up.  A hold that parks the
 * service loop is the exception — the shutdown runs on the loop it
 * would be parking — and ends on its own deadline instead: the flush
 * hold always, and the step 7 hold when step 7 is being performed on
 * the loop.
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
 * Where a new point goes, and what the shutdown does with it.
 *
 * srvholdclear, which the shutdown runs before it drains, clears the
 * whole of srvhook's set and nothing else: objhold, objexit,
 * flushhold, mapopen, walkhold, anyexit, step7, jobhold and
 * slotfail.  A HOLD therefore
 * belongs in srvhook — a program that set a point and stopped watching
 * must not be able to hold the store's close.  (The shutdown also
 * turns srvcellpoint off, by its own call and for its own reason: the
 * file table those cells are in outlives the context that was given
 * them.)  Clearing a point is not the same as reaching the proc
 * that is parked in it: a parked QUEUE proc wakes when its point is
 * cleared, but a parked SERVICE LOOP never reaches srvholdclear at all,
 * because the shutdown runs from Srv.end, which lib9p calls on the
 * loop.  A point that can park the loop — the flush hold, and the
 * step 7 hold when the flushed request was still queued — therefore
 * bounds its own park as well as being cleared here.
 *
 * What srvholdclear does NOT touch belongs beside srvauxpoint below:
 * the fid-state point and its counts, and the end point, whose whole
 * subject is what happens after the shutdown has run and which holds
 * for the count of milliseconds its caller gave.  An OBSERVABLE goes
 * there too, having nothing to clear.
 */

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
 * is what step 7 running too late would look like.  srvauxbusy answers
 * how many ran while a handler was part-way through a step on the same
 * fid's state — what step 7 running inside another request's handler
 * would look like, which the fid's own state lock is what rules out,
 * and the point's handler side is the check point's hold.  srvauxopen
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
 * The shutdown clears it as well, like the holds above and unlike the
 * observables below, because the table outlives the context: cells a
 * program forgot would otherwise be served by the next server it
 * starts.
 */
void	srvcellpoint(Srvctx*, int on);

/*
 * The point over the gap between the two: a pushed request is counted
 * complete from lib9p's closereq, which runs inside respond and before
 * respond releases the service, so the drain can converge and the loop
 * end while a queue proc is still inside lib9p.  Set to a count of
 * milliseconds, it holds each completion there for that long, which is
 * how a test drives the window a caller freeing the context at that
 * moment falls into; setting it to 0 releases a completion already
 * held, so a caller that has to name the moment lib9p is let go of
 * sets a long hold and ends it where it means rather than timing one.
 * Like srvauxpoint and unlike the srvhook holds, the shutdown does not
 * clear it: what it is about happens after the shutdown has run, so
 * the count is the only bound a program that forgets it gets.
 */
void	srvendpoint(Srvctx*, uvlong ms);

void	srvauxpoint(Srvctx*, int on);
void	srvauxcount(Srvctx*, uvlong *flushed, uvlong *closed, uvlong *freed);
uvlong	srvauxlate(Srvctx*);
uvlong	srvauxbusy(Srvctx*);
uvlong	srvauxopen(Srvctx*);
int	srvfidcount(Srvctx*);
