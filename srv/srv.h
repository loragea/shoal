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
	int	scrubdays;	/* -d; 0 takes layer-a §7.5's default of 14 */
	/*
	 * The engine's; spawn is filled in here.  Two of its stage fields
	 * are read by this library as well: `stagems' is how long a stage
	 * this server holds may be idle, and `stagemax' is the bound the
	 * object rows apply to a client write — over checksum blocks
	 * rather than over §3.6's reserved grains, which store.md §14(37)
	 * records.  `stagetot' is the engine's alone.
	 */
	Storecfg store;
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
 * refusal.  The passes this library starts take the same count with
 * the rest of their admission, under the one lock (job.c), and give
 * it back through srvjobend like any other caller.  A pass already
 * running SHOULD test srvstopping between units of work and give up
 * rather than leave the shutdown waiting.
 *
 * The two timers — the scrub's (layer-a §7.5) and the tombstone
 * reclaim walk's (§1.5) — are the procs here that are NOT jobs: each
 * starts passes and makes no engine call of its own, so neither holds
 * anything the store's close must wait behind.  What they do hold is
 * the context they read, and the shutdown waits for each of them
 * separately, and before the jobs.
 */
int	srvjobstart(Srvctx*);
void	srvjobend(Srvctx*);
int	srvstopping(Srvctx*);
/* srvjobcount, below, is how many are held right now */

/* what a caller and the tests read back */
Store*	srvstore(Srvctx*);
Cmap*	srvmap(Srvctx*);
char*	srviid(Srvctx*);
void	srvcount(Srvctx*, uvlong *pushed, uvlong *done);
int	srvjobcount(Srvctx*);	/* jobs held: what the shutdown waits for */
int	srvreclaimlive(Srvctx*);	/* ... and the timers, which hold none */
int	srvscrublive(Srvctx*);

/*
 * store.md §13's -X shape, for this library's own points: inert until
 * set, present in every build.
 *
 *	objhold	n != 0 holds every queued object request at its check
 *		point, so a test can have a request that is running and
 *		one that is still queued at a known moment.
 *	objprelook
 *		n != 0 holds a queued object write between §5.4 step 3's
 *		stage and the look that follows it.  A step 7 that lands
 *		in this window — a Tflush of another request on the same
 *		fid, or the idle sweep — takes the fid's stage before the
 *		handler has looked at it, which is what the look is for:
 *		the write is answered `shoalsrv: staged update discarded'
 *		and commits nothing (store.md §14(37)).
 *	objstage
 *		n != 0 holds a queued object request between §5.4 step 3's
 *		stage and the commit of it: after the look that says the
 *		fid's stage is still this handler's and before the engine
 *		call that publishes it.  That is the window a request owns
 *		a stage it has finished looking at, so it is where a test
 *		drives step 7 — a Tflush of another request on the same
 *		fid — against a commit that is about to happen anyway.
 *	objlook	n != 0 holds a queued object request AT that look, before
 *		it takes the fid's state lock and the stage list's — a
 *		request parked under either of those wedges the service
 *		loop or stalls every other queue (obj.c).  What holds the
 *		sweep off the stage across the park is the stage's own
 *		`busy' mark, a handler between two steps of one operation
 *		being no absence of arrivals (store.md §3.6), so this is
 *		where a test drives the idle sweep against a stage a
 *		handler is still inside — and where it drives a step 7 for
 *		another request on the same fid against one.
 *	objarm	n != 0 holds the open that the stage point stages on
 *		between the stage and the engine handle it arms it with.
 *		The handle is taken from the engine with no lock held, so
 *		a step 7 for another request on the same fid can strip the
 *		stage in that window — which is what the arm has to find
 *		rather than store a handle nothing would reach (obj.c).
 *	objclaim
 *		n != 0 holds a queued /obj create at its claim test, before
 *		it reads the fid's state at all.  objhold above parks it
 *		there too, but parks every other queued request with it —
 *		including the directory open this point exists to let run
 *		first, which is the request a create has to be driven
 *		against (obj.c).
 *	objexit	n != 0 holds every queued request that has reached the
 *		other end of its handler — an object read or write on
 *		/obj/<oid> or /meta/<oid>, a ctl verb run on an oid's
 *		queue, and the /obj and /meta directory open and read on
 *		the reserved one — after its engine call and before the
 *		exit, so a test can flush a request whose work is done and
 *		require it to leave through srvqdone all the same.  For
 *		the two directory cells that is the only place a flush
 *		can land after the work: the open's snapshot is taken and
 *		installed by then, and the read's cursor is committed, so
 *		this is the point that drives the give-back a flushed open
 *		owes (enum.c) and the rewind a flushed read leaves behind.
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
 *		unlink for it.  This point and reclaimhold below are the
 *		two whose park the shutdown WAITS for rather than steps
 *		over — both park a pass proc, which holds one of the jobs
 *		— and the hazard is the same for each: the wait for the
 *		jobs is unbounded, because store.md §9 forbids closing the
 *		store while a pass is still inside the engine, and the
 *		shutdown clears every point before that wait.  So a
 *		program that raises either of them again after the
 *		shutdown has begun parks a pass the shutdown then waits on
 *		for good; a program clears them before it stops the
 *		server.
 *	slotfail
 *		the one point here that refuses rather than holds: n != 0
 *		makes a walk over this instance's own index treat its n-1'th
 *		read as having failed — the scrub pass's read of index slot
 *		n-1, and the /obj and /meta directory read's objsnapent of
 *		snapshot position n-1.  It is how such a walk is broken off
 *		part-way with the store under it still healthy — the
 *		engine's own way of refusing an index read is to be
 *		condemned, which refuses the rest of the walk's calls too,
 *		and a walk broken off that way cannot be told from one
 *		whose store has gone.  The two walks are separate cases,
 *		so the one number serves both.
 *	dirhold	n != 0 holds the /obj and /meta directory read inside its
 *		entry walk, before its n-1'th entry and with the entries
 *		before it already converted.  That walk is the one stretch
 *		of a queued handler that runs over what the fid holds with
 *		the fid's state lock NOT held (enum.c), so it is where a
 *		test drives the service loop at a fid a queue proc is
 *		part-way through a listing of: a Tflush of a sibling
 *		request queued on the same fid, which the loop performs
 *		step 7 for and must answer without waiting for the walk.
 *		Set to n+1, like slotfail.
 *	dirgive	n != 0 holds the n-1'th /obj or /meta open of this
 *		instance to reach its give-back window — after srvfidgive
 *		has emptied the fid's state slot and before the open
 *		retakes the fid's state lock to install its own snapshot
 *		(enum.c).  A fid whose first open has ANSWERED holds
 *		nothing there, which is the window the create cell's
 *		`Fid.omode' test refuses a Tcreate in, so a test holds it
 *		open under the SECOND open of a fid (dirgive=2) and drives
 *		a create at the fid inside it (obj.c).  Set to n+1, like
 *		slotfail.  The park is outside the fid's state lock, as
 *		every point here is.
 *	reclaimhold
 *		n != 0 holds the tombstone reclaim walk before its n-1'th
 *		entry, with the entries before that one already counted.
 *		Nothing else can hold that walk still: it is paced by
 *		nothing and asks no queue, so a walk over any index a test
 *		can build is over before the next ctl write lands.  So it
 *		is where a test raises `reclaim stop', or takes the server
 *		down, over a walk that has counted a prefix of the
 *		snapshot — and where it holds one still long enough to
 *		write the next verb at all.  Set to n+1, like slotfail.  It
 *		parks a pass proc, so it carries jobhold's hazard above
 *		entire.
 *	tickhold
 *		n != 0 holds a TIMER's own start of a pass — the reclaim
 *		timer's and the scrub timer's alike — after it has
 *		decided to start one and before the pass's proc
 *		exists.  That window is where a `reclaim start' written on
 *		the service loop meets a tick in flight, and it is too
 *		narrow to write into without a hold.  Only the timer's call
 *		parks here, never a verb's: a verb's call IS the loop, and
 *		a loop parked answers nothing else either.
 *
 *		One point serves both timers, so a case that parks a tick
 *		MUST have only one of the two ticking.  Nothing enforces
 *		it and nothing could tell a case otherwise: the hold
 *		counts no parks (qhold is called with a nil counter,
 *		queue.c), so a second tick parked here is invisible, and
 *		the one srvhook(...,0) that releases the tick the case
 *		meant releases the other with it.  Two timers park only if
 *		a case shortens both periods; the defaults are days and
 *		half a day apart, and a case shortens the one it is about.
 *
 *		The proc it parks does hold a job — the admission has
 *		counted and linked the pass before the timer reaches this
 *		point, so /jobs lists it while it is parked — but it
 *		carries none of jobhold's hazard, and not because a
 *		shutdown cannot have begun behind it: one can, the hold
 *		being taken off the service loop with nothing to stop the
 *		loop ending behind it.  What makes it safe is the two ends
 *		the shutdown holds.  srvholdclear (srv.c) releases this
 *		point with the rest before any of the waits, so the parked
 *		tick is let go rather than waited for; and the job the
 *		admission counted is one jobwait sees, so the pass it then
 *		launches is waited for like any other.  A tick that has
 *		NOT reached the admission gets nothing at all: jobadmit
 *		reads `stopping' under the same joblk hold the shutdown
 *		sets it in and refuses.
 *	fullhold
 *		n != 0 holds an op=full chunk on a /repl fid between the
 *		stage it continued or opened and the engine write through
 *		its handle (peer.c).  That is the window in which the
 *		fid's stage is marked `busy' and the handle is an argument
 *		of a call the handler is about to make, so it is where a
 *		test drives the two things that may find such a stage
 *		while another queue proc is inside it: step 7, for a
 *		sibling chunk flushed on the same fid, and a chunk naming
 *		a second object, which runs on another queue and is
 *		refused (store.md §14(44), §14(42)).  Neither may release
 *		the handle; the busy chunk's own look is what does
 *		(obj.c).  The hold is taken with no lock held, the stage
 *		call having returned, so the service loop is free to
 *		perform step 7 on that fid across it.
 *	openhold
 *		n != 0 holds the OPENING chunk of an op=full transfer
 *		between the stage it made and the stageopen that fills the
 *		handle (obj.c's srvstagefull).  Through that window the
 *		fid's stage is in the slot and marked `busy' with no handle
 *		in it yet, which is the state the two actors above must
 *		leave alone as surely as they leave a stage mid-write alone:
 *		a stage taken from under an opening chunk is one the arm and
 *		the look behind it reach after it has been freed.  Like
 *		fullhold it is taken with no lock held, so the service loop
 *		performs step 7 on that fid across it, and srvheld below is
 *		how a test waits for a chunk to reach it.
 *	finalhold
 *		n != 0 holds a final=1 chunk between the look that followed
 *		its engine write and the give-back that ends the transfer
 *		(peer.c).  The look keeps the stage marked `busy' across
 *		that window because the chunk arbitrates and commits
 *		through the same handle, so this is where a test drives the
 *		two actors above — step 7 for a sibling on the fid, and a
 *		chunk naming a second object — against a stage whose owner
 *		is past its look and still inside its step.  Taken with no
 *		lock held, like the two above.
 *	newhold	n != 0 holds the first n requests that reach the call which
 *		takes the fid's stage slot, before that call's lock
 *		(obj.c's stagenew).  Two chunks naming different objects
 *		can both find the slot empty and both come here for a stage
 *		of their own; the winner fills the slot and the loser wakes
 *		into the clause that decides whether the slot may be taken
 *		from what is in it.  So this point parks the loser and lets
 *		everything behind it through — a point that held both would
 *		leave nobody to fill the slot — and srvheld below is how a
 *		test waits for the loser to be parked.  It is the only
 *		point of the set that does not hold every arrival.  Two
 *		things about it that its name does not say.  The call it
 *		sits on is the one EVERY staging path takes, the client's
 *		included — a Twrite, a Twstat truncation and a Tremove all
 *		make their stage there — so what this point parks is
 *		whichever n requests reach stagenew first, and a case that
 *		wants two /repl chunks in the window must have no other
 *		staging request in flight.  And `newheld' is both the count
 *		srvheld answers and this point's whole memory of how many
 *		it has parked, and it never resets (queue.c's qholdfirst):
 *		raising the point to the same n a second time in one
 *		context parks nobody, so a case that wants another park
 *		raises it to a larger n.
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
 * How many requests have reached a named point, counted where they
 * park and never reset.  It is what a test waits on instead of
 * sleeping: a case built around a window is a case about what runs
 * WHILE a request is parked, and a sleep long enough today is a wedge
 * or a silent pass tomorrow.  A point counts only where a case needs
 * the wait — today the three of the /repl transfer, fullhold, openhold
 * and finalhold, and the stage slot's newhold — and an unnamed point
 * answers 0.
 */
uvlong	srvheld(Srvctx*, char *name);

/*
 * Which queue of the pool an oid lands in.  Two operations on ONE oid
 * are serialized by the pool (store.md §7), so a case that needs two
 * requests running at once needs two oids that hash apart — and the
 * hash is this server's own and free to change (queue.c), so the case
 * asks rather than assumes.  An observable: it reads what srvqinit
 * built and changes nothing.
 */
int	srvqindex(Srvctx*, uchar *oid, int oidlen);

/*
 * Where a new point goes, and what the shutdown does with it.
 *
 * srvholdclear, which the shutdown runs before it drains, clears the
 * whole of srvhook's set and nothing else: objhold, objprelook,
 * objstage, objlook, objarm, objexit, flushhold, fullhold, openhold,
 * finalhold, newhold, mapopen, walkhold, anyexit, step7, jobhold,
 * slotfail, reclaimhold, tickhold, dirhold, dirgive and objclaim.  A
 * HOLD therefore belongs in srvhook — a program that set a point and
 * stopped watching must not be able to hold the store's close.  (The
 * shutdown also turns srvcellpoint off, by its own call and for its
 * own reason: the file table those cells are in outlives the context
 * that was given them.)  Clearing a point is not the same as reaching
 * the proc that is parked in it: a parked QUEUE proc wakes when its
 * point is cleared, but a parked SERVICE LOOP never reaches
 * srvholdclear at all, because the shutdown runs from Srv.end, which
 * lib9p calls on the loop.  A point that can park the loop — the
 * flush hold, and the step 7 hold when the flushed request was still
 * queued — therefore bounds its own park as well as being cleared
 * here.
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

/*
 * The period between the tombstone reclaim walk's passes, in ms,
 * overriding the map's own.  That period is `tombdays'/2 — layer-a
 * §8.3's epoch-bump cadence, since §1.5's condition 3 is what the walk
 * is waiting on — and the shortest a map can ask for is half a day,
 * which is longer than any test can wait for.  Set it to a few tens of
 * milliseconds to see the timer fire, and to 0 to put the map's own
 * period back.  The value is re-read as the timer waits, so it
 * shortens a wait already in progress — but the wait is slept in half-
 * second slices, so a period below that is one tick per slice and the
 * first tick comes within a slice of the call.  Like srvendpoint and
 * unlike the srvhook holds, the shutdown does not clear it: it holds
 * nothing up, since a pass the timer starts once the shutdown has
 * begun is refused the job it needs and the timer proc itself ends
 * with the shutdown either way.
 *
 * srvreclaimperiod is the period in force, which is what the knob
 * overrides: the map's `tombdays'/2, or the floor under it for a map
 * that retains nothing, or the knob's own value.  Half a day is longer
 * than a test can wait, so the floor is asserted by reading it rather
 * than by watching for a tick.
 */
void	srvreclaimms(Srvctx*, uvlong ms);
uvlong	srvreclaimperiod(Srvctx*);

/*
 * The same knob over the scrub's timer, which runs layer-a §7.5's
 * "continuously": the period there is `scrubdays' — 14 unless
 * `shoalsrv -d' said otherwise (store.md §12) — so a T1 that wants to
 * see a tick sets this to a few tens of milliseconds, and 0 puts
 * `scrubdays' back.  It is re-read as the timer waits, like the one
 * above, and the wait is slept in half-second slices, so a period
 * below that is one tick per slice and the first tick comes within a
 * slice of the call.  The shutdown does not clear it, for the reasons
 * it does not clear that one.
 *
 * srvscrubperiod is the period in force, which is what a test asserts
 * the default by: 14 days is longer than a test can wait.  What a tick
 * DOES is not this knob's: a tick that lands on a running pass starts
 * nothing, so a case that wants a second pass gives the first one time
 * to end (job.c).  /status's `scrubnext=' is how long is left of the
 * wait, which is the same schedule read from the wire.
 */
void	srvscrubms(Srvctx*, uvlong ms);
uvlong	srvscrubperiod(Srvctx*);

/*
 * The knobs over the stretch each of those two timers sleeps its
 * period in — half a second in this server (job.c) — where the knobs
 * above are over the period itself.  0 puts the server's own back.
 *
 * The two are named after the constants they override, which are not
 * the same word: the scrub timer's is Scrubtickms, because job.c has
 * a Scrubslicems as well and that one is something else entirely —
 * the twenty milliseconds a scrub PASS sleeps between rate charges.
 * The reclaim timer has no such twin, so its constant and its knob
 * are both `slice'.
 *
 * That stretch is what bounds the shutdown's wait for a timer proc,
 * and that is the whole of what these are for: at half a second the
 * wait is shorter than the work srvshutdown does after it — jobwait,
 * the fid close, the stage drain and the queue free — so a shutdown
 * that did not wait at all would still, most times, reach storeclose
 * with the proc already gone, and a case asserting that it has gone
 * would pass either way.  A case sets it to seconds, gives the timer
 * one old stretch to enter a new one, and then takes the server down:
 * the remaining sleep is now longer than everything downstream of the
 * wait, so the proc is demonstrably still up when the wait begins and
 * the assertion at storeclose fails if the wait is not made.
 *
 * The period is set back to the server's own before the stretch is
 * lengthened: a long one under a period of tens of milliseconds is a
 * tick apiece, and a tick during a shutdown is not what the case is
 * about.  Like the two above, the shutdown does not clear these.
 */
void	srvscrubtickms(Srvctx*, uvlong ms);
void	srvreclaimslicems(Srvctx*, uvlong ms);

/*
 * The stage point, over the per-fid staged operation layer-a §5.4 step
 * 3 creates and §5.4.1 step 7 discards.  With it on, an open of
 * /obj/<oid> for writing leaves a stage on the fid — one that stages
 * nothing — so that the lifetime rules can be driven on a fid holding
 * one while no request is in flight: the clunk's discard, the
 * shutdown's discard before the store closes, the idle sweep, and the
 * `disk full' a second stage on one fid is refused with.  No client
 * operation leaves a stage behind, because each gives its own back
 * inside its request; the stage that outlives its request is §5.5's
 * op=full, whose own lifetimes are driven through /repl (peer.c).
 *
 * srvstagecount answers how many stages the live fids hold, how many
 * have been given back and how many of those found the store still
 * open — which every one of them must, since releasing an engine stage
 * is an engine call.  Like srvauxpoint and unlike the srvhook holds,
 * the shutdown does not clear this point: it is per context, and what
 * it is about is what the shutdown itself does with a fid's state.
 *
 * srvstagepend answers how many engine handles have been parked for
 * obj.c's drain, which is where the flush hook leaves the one call it
 * may not make, and srvstagewaiting how many of them are still there.
 * The second is what says the shutdown emptied the list while the
 * store was open, which the drain it runs behind its fid sweep is for:
 * nothing walks that list once the service loop has ended, and a
 * handle left on it is an engine stage nothing can give back.
 * srvstagependfull makes that park REFUSE, which is the
 * path a failing allocation would take: the hook must still make no
 * engine call, so the handle goes back on the fid and the clunk or the
 * shutdown releases it.  It is a point like the one above and the
 * shutdown does not clear it either.
 */
void	srvstagepoint(Srvctx*, int on);
void	srvstagependfull(Srvctx*, int on);
void	srvstagecount(Srvctx*, uvlong *live, uvlong *done, uvlong *openat);
uvlong	srvstagepend(Srvctx*);
uvlong	srvstagewaiting(Srvctx*);

void	srvauxpoint(Srvctx*, int on);
void	srvauxcount(Srvctx*, uvlong *flushed, uvlong *closed, uvlong *freed);
uvlong	srvauxlate(Srvctx*);
uvlong	srvauxbusy(Srvctx*);
uvlong	srvauxopen(Srvctx*);
int	srvfidcount(Srvctx*);
