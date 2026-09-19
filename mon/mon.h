/*
 * shoalmon: the monitor's 9P service, docs/design/layer-a.md §8.  This
 * is the library; cmd/shoalmon/main.c is argument parsing and start-up
 * over it, and test/montreetest.c drives the same calls over a pipe.
 * The 9P surface is here rather than in the command for the reason
 * srv/srv.h gives for its own: a T1 test links libraries and execs
 * nothing (AGENTS.md), and §8 is what T1 has to drive.
 *
 * Include after <u.h>, <libc.h>, <libsec.h>, <fcall.h>, <thread.h>,
 * <9p.h> and "../lib/shoal.h".
 *
 * mon/dat.h and mon/fns.h are private to mon/.  What a builder of the
 * rest of the surface needs is here and in dat.h's tables.
 *
 * This library and srv/libshoalsrv.a are independent and share no
 * code: they serve two different trees with two different role
 * matrices, and the one thing they would share — the render-at-open
 * snapshot — is forty lines.  Mtext below is text.c's copy of srv/'s
 * Text under a name of its own, so a program may link both libraries;
 * folding the two into a common helper is a later change, and this
 * note is the record that there are two.
 *
 * libshoal knows nothing of lib9p or libthread and must not learn
 * (docs/design/store.md §7); the dependency goes one way, from here to
 * there.  The monitor's own durable state is lib/mon.c's map slot
 * store (store.md §10), which this library opens with monopen and — in
 * this unit — never writes.
 */

typedef struct Monctx Monctx;
typedef struct Moncfg Moncfg;
typedef struct Mtext Mtext;

/*
 * Monctx is opaque outside mon/: its definition is in mon/dat.h.
 * 2c(1)'s type signatures are computed from the C signof operator, so
 * a function taking a Monctx* signs differently in a file that has the
 * definition and one that has not; the pragma is what that mechanism
 * provides for exactly this case, as lib/shoal.h does for Store and
 * srv/srv.h for Srvctx.
 */
#pragma incomplete Monctx

enum
{
	/* layer-a §8.1's roles, and the access matrix's bit per role */
	Mreader		= 0,
	Minstance,
	Madmin,
	Nmrole,

	Areader		= 1<<Mreader,
	Ainstance	= 1<<Minstance,
	Amadmin		= 1<<Madmin,
	Amall		= Areader|Ainstance|Amadmin,

	/*
	 * The stack every proc of this service gets, and the value a
	 * program linking this library MUST set `mainstacksize' to before
	 * it posts or runs the service: threadpostmountsrv makes the
	 * proc that carries the loop with the program's mainstacksize.
	 * This service creates no procs of its own — see monsrvnew — so
	 * this is the only size it asks for.
	 */
	Monstack	= 64*1024,
};

/*
 * A render-at-open snapshot.  layer-a §8.1's last paragraph makes
 * snapshot-at-open a MUST for every status file of §8.1, the same
 * discipline §2.2 imposes on the instance's: the bytes are composed
 * once, when the fid is opened, and every read on that fid is served
 * out of them, so a concurrent mutation cannot tear a read.
 *
 * Here it is also what keeps the bytes alive.  A Monmap's text is the
 * slot store's own and is valid only until the next commit or
 * monclose (lib/shoal.h), so a fid that held the pointer rather than a
 * copy would serve freed memory after a publish.
 */
struct Mtext
{
	char	*p;
	long	n;
	long	max;
	int	err;		/* an allocation failed: the text is short */
};

Mtext*	montextnew(void);
void	montextfree(Mtext*);
int	montextprint(Mtext*, char*, ...);
int	montextwrite(Mtext*, void*, long);
#pragma	varargck	argpos	montextprint	2

/*
 * The error API, docs/design/store.md §3.7.  Its mapping rule is
 * normative and is the same rule srv/err.c applies: a condition
 * layer-a §2.6 names MUST be answered with §2.6's prefix and nothing
 * else, and an internal or device error MUST NEVER begin with one.
 * §2.6 is one prefix set shared by the instance and the monitor, so
 * the monitor's local strings need a prefix of their own, and it is
 * `shoalmon: ' (store.md §14(54)).
 *
 *	monsrv26	the §2.6 prefix a string carries, or nil.
 *	monsrverrs	what goes on the wire for one error string: a
 *			§2.6 string verbatim, anything else under this
 *			server's own prefix.  buf is written only in
 *			the second case.
 *	monsrverr	monsrverrs over the current %r.
 *
 * Emonnotbuilt is the local refusal a file or a ctl verb whose body is
 * not built answers after its gates.  It is deliberately not a §2.6
 * condition: nothing layer-a names has happened.
 */
extern char Emonnotbuilt[];
extern char Emonnofile[];
extern char Emonnomap[];
extern char Emonnoepoch[];

char*	monsrv26(char*);
char*	monsrverrs(char *buf, int nbuf, char *e);
char*	monsrverr(char *buf, int nbuf);

struct Moncfg
{
	Dev	*dev;		/* the monitor's partition, already open */
};

/*
 * monsrvnew is the whole of start-up that is not argument parsing: it
 * opens the map slot store with monopen — which writes nothing, works
 * on a read-only device and erases the phantoms of store.md §10 — and
 * parses the current map, if the store holds one.  It answers nil with
 * an error string for both refusals monopen makes and for a current
 * map that does not parse; a store that holds NO map is not a refusal,
 * because that is a monitor that has not published yet (store.md
 * §14(49)).
 *
 * It creates no procs.  Every handler of this service runs on the
 * service loop and none reaches the device: monopen read the whole
 * partition into memory, and moncurrent, monhistory, monlookup and
 * monstat answer out of it.  So there is no Reqqueue pool here and
 * lib9p's own loop is the whole of the concurrency — which is the
 * difference from srv/, where an object operation blocks on the disk
 * and must be offloaded (store.md §7).  The unit that adds the durable
 * publish adds the one call that does reach the device, moncommit; it
 * runs under the lock below, and whether that publish needs a queue of
 * its own is that unit's question.
 *
 * monsrv9p is the lib9p Srv: a caller that posts the service hands it
 * to threadpostmountsrv, and a caller that speaks 9P over a pipe sets
 * infd/outfd and calls monsrvrun.  monsrvrun returns when the
 * connection closes, by which time the shutdown has run.
 *
 * monsrvpost posts the service and returns at once, with the loop in a
 * proc of lib9p's making; it is NOT a pairing with monsrvfree, for the
 * reason srv/srv.h gives.
 *
 * monsrvfree runs the shutdown if the caller has not, waits for lib9p
 * to let go of the service, and frees the context.  It does not close
 * the device, which stays the caller's.
 *
 * A Monctx serves ONE service loop: the loop ending is the shutdown's
 * trigger and the shutdown closes the slot store.
 */
Monctx*	monsrvnew(Moncfg*);
Srv*	monsrv9p(Monctx*);
void	monsrvrun(Monctx*, int infd, int outfd);
void	monsrvpost(Monctx*, char *name);
void	monsrvshutdown(Monctx*);
int	monsrvreleased(Monctx*);
void	monsrvfree(Monctx*);

/*
 * The slot store this service serves, and the lock every access to it
 * is under.
 *
 * Mon has no lock of its own (lib/shoal.h), and moncommit mutates the
 * current-map slots and the ring that moncurrent, monhistory and
 * monlookup read.  This lock is what orders the two, and it is
 * exported because the caller that publishes is outside this library
 * until the unit that builds the durable publish moves it in: a caller
 * that commits to this Mon MUST hold the lock across moncommit and
 * MUST re-take the parsed map afterwards (monsrvremap), or the renders
 * will answer from a Cmap describing a map that is no longer current.
 *
 * Lock order: this lock first, the liveness registry's Lock second
 * (/instances and /health take both).  Never the other way round.
 */
Mon*	monsrvmon(Monctx*);
void	monsrvlock(Monctx*);
void	monsrvunlock(Monctx*);
int	monsrvremap(Monctx*);		/* re-parse the current map; lock held */

/*
 * layer-a §8.4's evidence, recorded and not acted on.  lastseen(i) is
 * the time of instance i's most recent successful read of /map on an
 * attach carrying role=instance,peer=i; store.md §14(51) fixes what
 * "successful read" is.  monsrvlastseen answers it in seconds, or 0
 * for an instance this service has never seen.
 *
 * The consumer is the unit that builds §8.4's demotion: it compares
 * now − lastseen(i) against the map's `deadms' and publishes
 * up=no fenced=yes.  Nothing here demotes anything.
 */
uvlong	monsrvlastseen(Monctx*, char *iid);
