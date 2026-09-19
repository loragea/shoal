#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../lib/shoal.h"
#include "srv.h"
#include "dat.h"
#include "fns.h"

/*
 * The background passes, /jobs, and the three ctl verbs that start
 * one: `scrub' (layer-a §2.5, §7.5, store.md §8), `reclaim' (§2.5,
 * store.md §9) and `forget' (§2.5, §7.1).  Two of the three have a
 * timer of their own, started at srvnew: the scrub's is layer-a
 * §7.5's "continuously", and the reclaim's is the cadence §1.5's
 * condition 3 moves at.  Both run their pass with no verb having
 * asked for one, and `forget' is the pass that has no timer, since
 * it names a peer and nothing but an operator knows which.
 *
 * §2.5: "Commands that start background work return success once the
 * job is accepted; progress is read from /jobs."  Every verb here does
 * exactly that, and every pass runs in a proc of its own outside every
 * queue, holding one of srv.h's jobs for the whole run so that the
 * shutdown waits for them: store.md §9 forbids storeclose while
 * anything is still inside the engine, and the request drain cannot
 * see a proc that is not a request.  A pass tests srvstopping between
 * units of work and gives up rather than leave the shutdown waiting.
 * The two timers are the procs here that hold no job, because they
 * walk nothing themselves: they only start passes (srv.h).
 *
 * WHY `forget' IS A PASS.  §2.5 does not call it background work, and
 * this server makes it so.  Each record it discards is one dirtydel,
 * which is one durable commit (store.md §2.6); the dirty region holds
 * as many records as the disk was formatted for, so doing them on the
 * service loop would park the loop for that many commits and layer-a
 * §5.4.1 requires a Tflush to be answerable throughout.  The verb is
 * not a `qfn' row either — a queued row's argv[0] must be an oid and
 * `forget's is an iid — so the pass is what is left.  store.md
 * §14(30) records it.
 */

enum
{
	/*
	 * The scrub rate, in KiB/s, and its default.  layer-a §7.5 makes
	 * the pace implementation policy and sizes its own example at
	 * "a default near 14 days ... ~4 MiB/s on a 4 TB disk"; that
	 * rate is this server's default, so the sizing layer-a states is
	 * the one an operator who sets nothing gets.
	 *
	 * The policy in full.  The pass charges itself the object's own
	 * `len' plus Scrubfloor for every object it pushes, and sleeps
	 * until the bytes it has charged would have taken that long at
	 * the rate — so the pace is over the bytes hashed, which is what
	 * §7.5's KiB/s means and what §8 measures the cost in.  The floor
	 * is what keeps a store of empty objects from spinning: an object
	 * of no bytes still costs a queue push and an index read.  A rate
	 * of 0 is not spellable; `rate=' takes a positive decimal, and
	 * the verb refuses anything else with `bad ctl'.  The pass sleeps
	 * in slices so that a `scrub stop' or a shutdown is seen inside
	 * one rather than at the end of a long sleep.
	 */
	Scrubratedflt	= 4096,
	Scrubfloor	= 1024,
	Scrubslicems	= 20,

	/*
	 * The scrub's own timer (store.md §8, §14(39)).  layer-a §7.5 has
	 * each instance re-read its objects CONTINUOUSLY, "rate-limited so
	 * that a full pass completes in about `scrubdays'" — so the period
	 * between one pass starting and the next is that same `scrubdays',
	 * and a pass still running when its period is up is the continuity
	 * §7.5 asks for rather than a tick to be caught up on: the tick is
	 * a no-op and the walk goes on.  The default is 14 days, which is
	 * the default §7.5 sizes its own example at, and `shoalsrv -d'
	 * overrides it (store.md §12).  Both are implementation policy,
	 * which §7.5 says in as many words.
	 *
	 * The wait is slept in half-second slices for the reasons the
	 * reclaim timer's are (Reclaimslicems below): what it paces is a
	 * period of days, the proc is not a job, and what the length costs
	 * is the shutdown's wait for the proc, which is bounded by one
	 * slice.  It is NOT Scrubslicems above: that one paces a walk of a
	 * disk and is twenty milliseconds.
	 *
	 * The first pass comes one period after start-up rather than at
	 * it, which is reclaim's argument (reclaimtimer below) reaching
	 * the same place from the other end: a pass costs a walk of the
	 * whole index at `scrubdays' of pacing, so an instance restarted
	 * often would spend its life in the first tenth of one.
	 */
	Scrubdaysdflt	= 14,
	Scrubperiodms	= Scrubdaysdflt*86400*1000,
	Scrubtickms	= 500,

	/*
	 * How many passes may run at once.  layer-a §2.5 bounds neither
	 * verb: `scrub' is bounded here by `scrubbing', which keeps a
	 * second pass off one index, but `forget <iid>' names a peer and
	 * nothing stopped a client writing it once per id it could spell
	 * — each write a proc of its own, each holding a dirtysnap and a
	 * job the shutdown waits on.  So a pass beyond this many is
	 * refused rather than accepted, which is the one place §2.5's
	 * "return success once the job is accepted" leaves for saying no.
	 * The number is implementation policy (store.md §14(30)): the
	 * passes are whole-store walks and an operator has no use for
	 * many at once, so it is small enough to bound the procs and the
	 * snapshots and larger than any sane operator's use.
	 */
	Njobmax		= 12,

	/*
	 * The tombstone reclaim walk's own timer (store.md §14(39)).  The
	 * period is `tombdays'/2, the cadence at which layer-a §8.3 has
	 * the monitor bump the epoch even on an idle cluster: §1.5's
	 * condition 3 wants the current epoch strictly above the
	 * tombstone's, so a walk that looks more often than the epoch
	 * moves asks a question whose answer cannot have changed.
	 *
	 * The floor is what a map with `tombdays=0' gets.  Such a map
	 * retains nothing — the cutoff is the present (§14(31)) — and
	 * `tombdays'/2 is then no period at all, so the walk would spin
	 * over the index for as long as the instance served.  Twelve
	 * hours is what the shortest non-zero retention a map can carry
	 * gives, and it is orders above the walk's own cost, which is one
	 * /tombs snapshot and a walk of it.
	 *
	 * The wait is slept in slices, as the scrub's pace is, so that a
	 * shutdown is seen inside one rather than at the end of a period
	 * measured in days: the timer proc is not a job, and the context
	 * it reads outlives it only until srvshutdown says otherwise.
	 * The slice is half a second rather than the scrub's twenty
	 * milliseconds, because what it paces is a period of days and not
	 * a walk of a disk: at twenty it would wake fifty times a second
	 * for the life of the instance — some fifteen million times over
	 * one default period — and all it does on waking is read two
	 * words under joblk.  What the length costs is the shutdown's
	 * wait for the proc (srv.c), which is bounded by one slice.
	 */
	Reclaimminms	= 12*3600*1000,
	Reclaimslicems	= 500,

	/* what qjob runs on the object's queue */
	Jscrub		= 0,
};

/*
 * One unit of a pass's work, run on the object's own queue.
 *
 * store.md §8 is explicit that the scrubber "does not read grains
 * itself: for each object it pushes one verify request onto that
 * object's Reqqueue and waits for the answer, exactly as a client
 * read would".  §3.5 defers a freed grain's reuse only until the
 * freeing commit's flush returns, which says nothing about a reader
 * that started earlier, so a scrubber reading beside the queue would
 * hash grains freed and staged into under it and durably flag a live,
 * correct object `corrupt'.  The queue is the object's ordering point
 * (§5.4 step 2), and a read or a mutation outside it is ordered
 * against nothing — so a discard, when the replication surface brings
 * one, belongs here too; the reclaim walk below makes none.
 *
 * Most of the pool's entry points take a Req, because every other
 * caller is one.  A pass is not, so it enters through srvqjob
 * (queue.c), which is the pool's entry for a caller with no tag, no
 * Req pool and no reference to the Srv: it pushes a unit of work,
 * waits for it, and gives the pool its completion back through
 * srvqended.  Nothing on this path allocates, and nothing on it
 * responds — dat.h's Qjob states the rule.  This structure is the
 * work itself, which srvqjob carries as an opaque argument.
 */
typedef struct Qwork Qwork;
struct Qwork
{
	Srvctx	*ctx;
	int	op;
	uchar	oid[Oidmax];
	int	oidlen;
	int	rc;
	int	bad;
	char	err[ERRMAX];
};

static char Eshutting[] = "shoalsrv: shutting down";
static char Enomem[] = "shoalsrv: out of memory";
static char Ejobs[] = "shoalsrv: too many jobs";
static char Escrubstopping[] = "shoalsrv: scrub stopping";
static char Ereclaimstopping[] = "shoalsrv: reclaim stopping";

static void	scrubpass(Sjob*);
static void	reclaimpass(Sjob*);
static void	forgetpass(Sjob*);
static ulong	scrubrate(Srvctx*);

static void
qworkrun(void *a)
{
	Qwork *j;
	Vfy v;

	j = a;
	switch(j->op){
	case Jscrub:
		/*
		 * store.md §8's scrub, not its verify: the pass is what
		 * sets the corrupt flag on a mismatch and clears it when
		 * every block matches, which is what puts an object into
		 * /lost and takes it out again (layer-a §7.5).
		 */
		memset(&v, 0, sizeof v);
		if(objscrub(j->ctx->store, j->oid, j->oidlen, &v) < 0){
			j->rc = -1;
			rerrstr(j->err, sizeof j->err);
		}else
			j->bad = v.arraybad || v.nbad > 0;
		vfyfree(&v);
		break;
	}
}

static int
qjob(Qwork *j)
{
	j->rc = 0;
	j->bad = 0;
	j->err[0] = 0;
	srvqjob(j->ctx, j->oid, j->oidlen, qworkrun, j);
	return j->rc;
}

/*
 * An object the index walk found live and the object's own queue
 * found gone.  The walk reads the index outside every queue (objslot
 * holds the state lock and no more), and the unit it pushes is
 * ordered behind whatever that queue was already holding — so a
 * delete or a drop landing in between is the ordering working, not a
 * failure of this pass.  The engine has two answers for an id it no
 * longer holds live, `no such object' for one it holds nothing for
 * and `object deleted' for a tombstone (lib/obj.c), and both are
 * that case.  Anything else is a read that failed.
 */
static int
objgone(char *err)
{
	return strcmp(err, "no such object") == 0
		|| strcmp(err, "object deleted") == 0;
}

/*
 * The job list.  A verb that starts a pass links the record before it
 * spawns the proc, so /jobs shows the job from the moment §2.5 says
 * it is accepted; the proc unlinks it as its last act before giving
 * the job back.
 */
static int
joblen(Srvctx *c)			/* joblk held */
{
	Sjob *j;
	int n;

	n = 0;
	for(j = c->jobs; j != nil; j = j->next)
		n++;
	return n;
}

/*
 * What a pass gave up with, from %r, recorded once: the pass has no
 * client to answer, so /jobs is where the failure is reported and
 * this is the only place it is written.  The first failure wins,
 * because it is the one that stopped the walk.
 */
static void
joberr(Sjob *j)
{
	char buf[ERRMAX];
	Srvctx *c;

	rerrstr(buf, sizeof buf);
	c = j->ctx;
	lock(&c->joblk);
	if(j->err[0] == 0)
		strecpy(j->err, j->err + sizeof j->err, buf);
	unlock(&c->joblk);
}

static void
jobunlink(Sjob *j)
{
	Srvctx *c;
	Sjob **p;

	c = j->ctx;
	lock(&c->joblk);
	for(p = &c->jobs; *p != nil; p = &(*p)->next)
		if(*p == j){
			*p = j->next;
			break;
		}
	unlock(&c->joblk);
	free(j);
}

static void
jobproc(void *a)
{
	Sjob *j;
	Srvctx *c;

	j = a;
	c = j->ctx;
	lock(&c->joblk);
	j->running = 1;
	unlock(&c->joblk);
	j->fn(j);
	/*
	 * The pass is over, so the flag that keeps a second pass off one
	 * index goes back BEFORE the point below rather than after it: a
	 * `scrub start' written while the pass is parked there would
	 * otherwise be refused `scrub stopping' for a pass that has
	 * finished walking, which is a job that is not running and is not
	 * going to be.  The record stays linked either way, so /jobs
	 * still lists what the pass finished with.  `reclaim' carries the
	 * same flag for the same reason, and each verb's timer reads its
	 * own: a tick that finds it raised starts nothing.
	 */
	lock(&c->joblk);
	if(strcmp(j->verb, "scrub") == 0)
		c->scrubbing = 0;
	else if(strcmp(j->verb, "reclaim") == 0)
		c->reclaiming = 0;
	unlock(&c->joblk);
	/*
	 * §13's point, while the record is still on the list: /jobs lists
	 * a pass only while it is running or queued, so what one finished
	 * with is readable here and nowhere later (srv.h).
	 */
	srvjobhold(c);
	jobunlink(j);
	srvjobend(c);
	threadexits(nil);
}

/*
 * Accepting a pass is three steps, because a caller can have a flag of
 * its own to raise in the same hold of joblk that decides the refusal
 * (reclaimgo below): the record is made, then admitted, then launched.
 *
 * jobnew allocates it outside the lock, since joblk is a spin lock and
 * malloc allocates.
 */
static Sjob*
jobnew(Srvctx *c, char *verb, void (*fn)(Sjob*), char *arg)
{
	Sjob *j;

	if((j = mallocz(sizeof *j, 1)) == nil)
		return nil;
	j->ctx = c;
	j->verb = verb;
	j->fn = fn;
	if(arg != nil)
		strecpy(j->arg, j->arg + sizeof j->arg, arg);
	return j;
}

/*
 * jobadmit decides the two refusals that are not out of memory — the
 * shutdown and the cap — takes the job the shutdown waits on, and
 * links the record /jobs shows, all in the caller's hold of joblk: so
 * two verbs cannot both find room for the last job, and a caller that
 * raises a flag in that same hold cannot raise it for a pass that is
 * about to be refused.  The job is taken before the proc exists so
 * that a shutdown starting in the window cannot slip past it.
 *
 * It reads `stopping' and takes the count itself rather than calling
 * srvjobstart, which takes this same lock (srv.c).  On a refusal the
 * record is the caller's to free.
 */
static char*
jobadmit(Srvctx *c, Sjob *j)		/* joblk held */
{
	if(c->stopping)
		return Eshutting;
	if(joblen(c) >= Njobmax)
		return Ejobs;
	c->njob++;
	j->next = c->jobs;
	c->jobs = j;
	return nil;
}

/*
 * joblaunch spawns the proc for a record already admitted, so the one
 * way out it has is out of memory — which is what leaves the caller's
 * own flags to be put back.
 */
static char*
joblaunch(Sjob *j)
{
	Srvctx *c;

	c = j->ctx;
	if(proccreate(jobproc, j, Srvstack) < 0){
		jobunlink(j);
		srvjobend(c);
		return Enomem;
	}
	return nil;
}

static char*
jobstart(Srvctx *c, char *verb, void (*fn)(Sjob*), char *arg)
{
	char *e;
	Sjob *j;

	if((j = jobnew(c, verb, fn, arg)) == nil)
		return Enomem;
	lock(&c->joblk);
	e = jobadmit(c, j);
	unlock(&c->joblk);
	if(e != nil){
		free(j);
		return e;
	}
	return joblaunch(j);
}

/*
 * /jobs, layer-a §2.2: one line per running or queued background job,
 * and EVERY such job — a file that listed some of them would answer a
 * §2.2 MUST with a sample.  It reads the server's own list and
 * touches no engine, so unlike the status files whose render does
 * reach it — /dirty, /lost, /tombs and /advert, which fill an open
 * cell that puts the render on the reserved queue — this one is
 * rendered on the service loop, as /stale is (dat.h).
 *
 * The list is sized under the lock, the room for it taken outside the
 * lock, and the lines formatted outside it too: joblk is a spin lock,
 * and both malloc and textprint allocate.  The list can only shrink
 * between the two holds — a pass ending unlinks itself, and the verbs
 * that link one run on this same loop — so `n' is an upper bound and
 * `k' is what was there to copy.  Njobmax bounds both.
 */
char*
srvjobstext(Srvctx *c, Sfid *f, Text *t)
{
	Sjob *cp, *j;
	ulong rate;
	int i, k, n;

	USED(f);
	rate = scrubrate(c);
	lock(&c->joblk);
	n = joblen(c);
	unlock(&c->joblk);
	if(n == 0)
		return nil;
	if((cp = mallocz(n*sizeof *cp, 1)) == nil)
		return Enomem;
	k = 0;
	lock(&c->joblk);
	for(j = c->jobs; j != nil && k < n; j = j->next)
		cp[k++] = *j;
	unlock(&c->joblk);
	for(i = 0; i < k; i++)
		textprint(t, "job=%s state=%s rate=%lud done=%llud/%llud "
			"bad=%llud skipped=%llud reclaimable=%llud "
			"dropped=%llud%s%s\n",
			cp[i].verb, cp[i].running ? "running" : "queued",
			rate, cp[i].done, cp[i].total, cp[i].bad,
			cp[i].skipped, cp[i].reclaimable, cp[i].dropped,
			cp[i].err[0] != 0 ? " err=" : "", cp[i].err);
	free(cp);
	return nil;
}

/*
 * The rate in force.  A context that has never been told one runs at
 * the default, which is where Scrubratedflt is read rather than at
 * start-up: nothing in srvnew has to know this file exists.
 */
static ulong
scrubrate(Srvctx *c)
{
	ulong n;

	lock(&c->joblk);
	n = c->scrubrate;
	unlock(&c->joblk);
	return n != 0 ? n : Scrubratedflt;
}

/*
 * What ends a pass early: the shutdown, or that pass's own stop flag.
 * The two passes that can be stopped have a flag each, because a
 * `scrub stop' and a `reclaim stop' name different work — the walks
 * run at once and neither is the other's — and the timer below starts
 * a reclaim whether or not a scrub is running.
 */
static int
stopflag(Srvctx *c, int *fl)
{
	int n;

	lock(&c->joblk);
	n = *fl;
	unlock(&c->joblk);
	return n;
}

static int
scrubover(Srvctx *c)
{
	return srvstopping(c) || stopflag(c, &c->scrubstop);
}

static int
reclaimover(Srvctx *c)
{
	return srvstopping(c) || stopflag(c, &c->reclaimstop);
}

/*
 * The rate limit.  See Scrubratedflt above for the policy; this is
 * the arithmetic: the bytes charged so far, at `rate' KiB/s, would
 * have taken bytes*1000/(rate*1024) ms, and the pass waits until that
 * much has passed since it started.
 *
 * The charge is cumulative and the rate is read fresh each object, so
 * a `scrub rate=' written mid-pass would otherwise be applied to the
 * bytes already charged as well as to the bytes to come — and that is
 * not a rate change, it is a re-billing.  A raised rate would make
 * the deadline for everything charged so far fall into the past, and
 * the pass would run flat out until it caught up; a lowered one would
 * park it for as long as the new rate says the whole pass should have
 * taken.  So the charge and the clock start again from the rate the
 * pass is now told to run at: the bytes before the change were paced
 * at the rate in force when they were read, which is all the rate can
 * mean.
 */
static void
scrubpace(Srvctx *c, uvlong *bytes, vlong *t0, ulong *last, uvlong len)
{
	vlong want;
	ulong rate;

	rate = scrubrate(c);
	if(rate != *last){
		*last = rate;
		*bytes = 0;
		*t0 = nsec()/1000000;
	}
	*bytes += len + Scrubfloor;
	want = ((vlong)*bytes * 1000) / ((vlong)rate * 1024);
	while(nsec()/1000000 - *t0 < want){
		if(scrubover(c))
			return;
		sleep(Scrubslicems);
	}
}

/*
 * store.md §9's tombstone reclaim walk, which COUNTS and discards
 * nothing.
 *
 * layer-a §1.5 licenses a discard only when all three of its
 * conditions hold: (1) full confirmation — every instance in the map
 * whose status is not `dead' has said, since the tombstone was
 * written, that it holds a copy at a key at least the tombstone's or
 * no copy at all; (2) `tombdays' of retention since the tombstone's
 * mtime; and (3) the current epoch strictly above the tombstone's
 * wepoch.  Conditions 2 and 3 are local, and this walk is where they
 * are tested: `tombdays' is a map-header attribute (§3.1) the engine
 * does not hold, and 0 is a value it may carry — the cutoff is then
 * the present, and a tombstone written before this second has served
 * its retention.
 *
 * Condition 1 is not local and is not reachable in this build: there
 * is no outbound peer client, so no instance has confirmed anything
 * and every tombstone is blocked by an unanswered instance.  §1.5 is
 * explicit about what discarding on 2 and 3 alone costs — it is the
 * first draft's rule, and the resurrection hole it names: a peer that
 * has been down since before the delete comes back holding the live
 * copy, the primary holds nothing for the object, and absence loses
 * arbitration (§1.3).  So the walk reports what it finds and leaves
 * every record where it is: `reclaimable=' at /jobs is the count of
 * tombstones past conditions 2 and 3, and it is the number of
 * discards the replication surface will have to confirm.  The discard
 * itself — §1.5's op=discard to every confirming instance, this
 * instance's own record removed last — lands with that surface.
 * store.md §14(31) and decisions.md D26 record it.
 *
 * It is a pass of its own, on its own timer and under §2.5's `reclaim'
 * verb (store.md §14(39)).  It walks the index in seconds and the
 * scrub takes about `scrubdays' — layer-a §7.5 sizes its own example
 * at 14 — so a walk that rode on the scrub gave a mass delete's space
 * back only after a whole pass, forfeited the count to a `scrub stop',
 * and could not be driven at all without one.
 */
static void
reclaimpass(Sjob *j)
{
	char buf[ERRMAX];
	Srvctx *c;
	Objsnap *sn;
	Objinfo oi;
	uchar oid[Oidmax];
	uvlong epoch;
	vlong cutoff;
	ulong i, n;
	int oidlen, rc;

	c = j->ctx;
	cutoff = time(0) - (vlong)c->map->tombdays*86400;
	epoch = c->map->epoch;
	if((sn = srvsnapopen(c->store, Snaptomb, buf, sizeof buf)) == nil){
		werrstr("%s", buf);
		joberr(j);
		return;
	}
	n = objsnapcount(sn);
	lock(&c->joblk);
	j->total = n;
	unlock(&c->joblk);
	for(i = 0; i < n; i++){
		srvreclaimhold(c, i);
		/*
		 * A walk told to stop, or one the shutdown broke off, has
		 * counted a PREFIX of the snapshot.  `done=' and `total='
		 * are this walk's own — the snapshot's entries, not the
		 * index's slots, since this pass walks no index — so a
		 * prefix shows there as well; the pass is marked all the
		 * same, because `done=' short of `total=' is also what a
		 * walk still running reads, and the mark is what says the
		 * walk is over and its count is not the whole store's.  One
		 * string covers both causes — what an operator has to know
		 * is that the number is a prefix, not which of the two cut
		 * it short.
		 */
		if(reclaimover(c)){
			werrstr("shoalsrv: stopped");
			joberr(j);
			break;
		}
		rc = objsnapent(sn, i, oid, &oidlen, &oi);
		lock(&c->joblk);
		j->done = i+1;
		unlock(&c->joblk);
		if(rc < 0){
			joberr(j);
			break;
		}
		if(rc == 0)
			continue;
		if(oi.mtime >= cutoff)		/* §1.5 condition 2 */
			continue;
		if(oi.wepoch >= epoch)		/* §1.5 condition 3 */
			continue;
		lock(&c->joblk);
		j->reclaimable++;
		unlock(&c->joblk);
	}
	objsnapclose(sn);
}

/*
 * Start a reclaim pass, for the verb and for the timer alike: the two
 * share one "a pass is running" flag, so a tick that lands on a pass
 * an operator started starts nothing, and a `reclaim start' written
 * over a pass the timer started is the same no-op as one written over
 * a pass the verb started.
 *
 * The flag is raised before the proc exists, because raising it after
 * would race the proc's own clearing of it — but it is raised in the
 * SAME hold of joblk that admits the job, because the refusals a pass
 * can meet are the answer this verb owes.  A flag raised ahead of them
 * would have the caller that finds it up answered success for a pass
 * the admission then refuses: the timer calls this off the service
 * loop, so a `reclaim start' lands inside that window, and both would
 * come away having started nothing.  What is left below the admission
 * is out of memory alone, and that still puts the flag back — a flag
 * left raised makes every later start answer success and start
 * nothing, and stops the timer for good besides.  The stop flag goes
 * back with it.
 *
 * `bytimer' is the timer's own call.  It is what §13's `tickhold'
 * parks (srv.h): the window between the decision and the proc is the
 * timer's to be caught in, since a verb's own call is the service loop
 * and parking that answers nothing else either.
 */
static char*
reclaimgo(Srvctx *c, int bytimer)
{
	char *e;
	Sjob *j;
	int wasstop;

	if((j = jobnew(c, "reclaim", reclaimpass, nil)) == nil)
		return Enomem;
	lock(&c->joblk);
	if(c->reclaiming){
		/*
		 * A pass that has been told to stop is not the job a start
		 * asks for: it will read the flag between two entries and
		 * give up.  This is `scrub's rule (§14(31)) for the same
		 * reason — clearing the stop flag instead races the pass's
		 * own read of it — and the timer is answered it too, which
		 * costs a tick: the pass winding down is about to end, and
		 * the next tick starts a whole walk.
		 */
		if(c->reclaimstop){
			unlock(&c->joblk);
			free(j);
			return Ereclaimstopping;
		}
		unlock(&c->joblk);
		free(j);
		return nil;
	}
	if((e = jobadmit(c, j)) != nil){
		unlock(&c->joblk);
		free(j);
		return e;
	}
	wasstop = c->reclaimstop;
	c->reclaiming = 1;
	c->reclaimstop = 0;
	unlock(&c->joblk);
	if(bytimer)
		srvtickhold(c);
	if((e = joblaunch(j)) == nil)
		return nil;
	lock(&c->joblk);
	c->reclaiming = 0;
	c->reclaimstop = wasstop;
	unlock(&c->joblk);
	return e;
}

/*
 * The period between reclaim passes, in ms: `tombdays'/2, which is
 * layer-a §8.3's own cadence for the epoch bump §1.5's condition 3
 * needs, floored so that a map retaining nothing does not spin
 * (Reclaimminms above).  It is read fresh each slice, so a test that
 * sets the knob after srvnew shortens the wait it is already in.
 *
 * It is public for the same reason srvreclaimms is (srv.h): the floor
 * and the period are minutes and days, so what a test can assert about
 * them is the number itself and not a tick it waited for.
 */
uvlong
srvreclaimperiod(Srvctx *c)
{
	uvlong ms;

	lock(&c->joblk);
	ms = c->reclaimms;
	unlock(&c->joblk);
	if(ms != 0)
		return ms;
	ms = (uvlong)c->map->tombdays * (86400000/2);
	if(ms < Reclaimminms)
		ms = Reclaimminms;
	return ms;
}

/*
 * The timer.  It is not a job — it makes no engine call, and a job
 * the shutdown waits for would have to end before jobwait rather than
 * with it — so the shutdown waits for this proc separately, and the
 * proc reads srvstopping between slices to be there to be waited for.
 * The context is this proc's to read until then and not after: nothing
 * else keeps it alive.
 *
 * The first pass comes one period after start-up rather than at it.
 * An instance restarted often would otherwise walk its index at every
 * start, and the walk answers a question — which tombstones are past
 * §1.5's retention and epoch — whose answer moves at the period, not
 * at the restart.
 */
static void
reclaimtimer(void *a)
{
	Srvctx *c;
	uvlong t;

	c = a;
	for(;;){
		for(t = 0; t < srvreclaimperiod(c); t += Reclaimslicems){
			if(srvstopping(c)){
				lock(&c->joblk);
				c->reclaimup = 0;
				unlock(&c->joblk);
				threadexits(nil);
			}
			sleep(Reclaimslicems);
		}
		reclaimgo(c, 1);
	}
}

/*
 * Start it, from srvnew once the map is adopted and the store is open
 * (srv.h).  It is the last thing start-up does, so no refusal below it
 * can free the context under a proc that is reading it.
 */
int
srvreclaimproc(Srvctx *c)
{
	lock(&c->joblk);
	c->reclaimup = 1;
	unlock(&c->joblk);
	if(proccreate(reclaimtimer, c, Srvstack) < 0){
		lock(&c->joblk);
		c->reclaimup = 0;
		unlock(&c->joblk);
		return -1;
	}
	return 0;
}

/* whether that proc is still reading the context: the shutdown's wait */
int
srvreclaimlive(Srvctx *c)
{
	int n;

	lock(&c->joblk);
	n = c->reclaimup;
	unlock(&c->joblk);
	return n;
}

/*
 * The T1 knob over the period (srv.h).  0 puts the map's own period
 * back.
 */
void
srvreclaimms(Srvctx *c, uvlong ms)
{
	lock(&c->joblk);
	c->reclaimms = ms;
	unlock(&c->joblk);
}

/*
 * layer-a §7.5's scrub: walk the index in order and re-verify every
 * live object, each through its own queue, at the configured rate.
 * The verdict's durable half is objscrub's — a mismatch flags the
 * copy and puts it in /lost, every block matching clears the flag.
 */
static void
scrubpass(Sjob *j)
{
	Srvctx *c;
	Storestat st;
	Qwork w;
	uchar oid[Oidmax];
	Objinfo oi;
	uvlong bytes, slot;
	ulong lastrate;
	vlong t0;
	int oidlen, rc;

	c = j->ctx;
	storestat(c->store, &st);
	lock(&c->joblk);
	j->total = st.nslots;
	unlock(&c->joblk);
	memset(&w, 0, sizeof w);
	w.ctx = c;
	w.op = Jscrub;
	bytes = 0;
	lastrate = scrubrate(c);
	t0 = nsec()/1000000;
	for(slot = 0; slot < st.nslots; slot++){
		if(scrubover(c))
			break;
		rc = objslot(c->store, (ulong)slot, oid, &oidlen, &oi);
		if(rc >= 0 && srvslotfail(c, slot)){
			werrstr("shoalsrv: index read refused at the point");
			rc = -1;
		}
		lock(&c->joblk);
		j->done = slot+1;
		unlock(&c->joblk);
		if(rc < 0){
			joberr(j);
			break;
		}
		if(rc == 0 || oi.state != Slive)
			continue;
		memmove(w.oid, oid, oidlen);
		w.oidlen = oidlen;
		/*
		 * An object whose read failed is the pass's failure and is
		 * recorded as one — it is the only record there is, and a
		 * scrub that could not read an object has not verified the
		 * index it says it walked.  The walk goes on all the same:
		 * one object that would not read says nothing about the
		 * next, and stopping here would leave the rest of the index
		 * unverified as well.  An object that has merely gone is
		 * not a failure (objgone) and is counted apart.
		 */
		if(qjob(&w) < 0){
			if(objgone(w.err)){
				lock(&c->joblk);
				j->skipped++;
				unlock(&c->joblk);
			}else{
				werrstr("%s", w.err);
				joberr(j);
			}
		}else if(w.bad){
			lock(&c->joblk);
			j->bad++;
			unlock(&c->joblk);
		}
		scrubpace(c, &bytes, &t0, &lastrate, oi.len);
	}
}

/*
 * Start a scrub pass, for the verb and for the timer alike.  It is
 * reclaimgo's twin and every word of that function's comment applies
 * here: one "a pass is running" flag between the two callers, raised
 * in the SAME hold of joblk that admits the job so that a caller which
 * finds it up is never answered success for a pass the admission then
 * refuses, put back with the stop flag by the one refusal below the
 * admission, and `bytimer' parking §13's `tickhold' for the timer's
 * call alone.
 *
 * What differs is only which pass it is.  A tick that lands on a pass
 * an operator started starts nothing, and a `scrub start' written over
 * a pass the timer started is the same no-op as one written over a
 * pass the verb started — which is what layer-a §7.5's "continuously"
 * wants of a tick: the walk already running IS the pass the tick would
 * have asked for.
 */
static char*
scrubgo(Srvctx *c, int bytimer)
{
	char *e;
	Sjob *j;
	int wasstop;

	if((j = jobnew(c, "scrub", scrubpass, nil)) == nil)
		return Enomem;
	lock(&c->joblk);
	if(c->scrubbing){
		/*
		 * A pass that has been told to stop is not the job a start
		 * asks for: it reads the flag between two objects and gives
		 * up (store.md §14(31)).  The timer is answered that too,
		 * which costs a tick — the pass winding down is about to
		 * end, and the next tick starts a whole pass.
		 */
		if(c->scrubstop){
			unlock(&c->joblk);
			free(j);
			return Escrubstopping;
		}
		unlock(&c->joblk);
		free(j);
		return nil;
	}
	if((e = jobadmit(c, j)) != nil){
		unlock(&c->joblk);
		free(j);
		return e;
	}
	wasstop = c->scrubstop;
	c->scrubbing = 1;
	c->scrubstop = 0;
	unlock(&c->joblk);
	if(bytimer)
		srvtickhold(c);
	if((e = joblaunch(j)) == nil)
		return nil;
	lock(&c->joblk);
	c->scrubbing = 0;
	c->scrubstop = wasstop;
	unlock(&c->joblk);
	return e;
}

/*
 * The period between scrub passes, in ms: `scrubdays', which is
 * layer-a §7.5's own knob and is 14 days unless `shoalsrv -d' said
 * otherwise (store.md §12).  It is read fresh each slice, so a test
 * that sets the knob after srvnew shortens the wait it is already in.
 *
 * It is public for the same reason srvreclaimperiod is (srv.h): the
 * period is measured in days, so what a test can assert about it is
 * the number itself and not a tick it waited for.
 */
uvlong
srvscrubperiod(Srvctx *c)
{
	uvlong ms;

	lock(&c->joblk);
	ms = c->scrubms;
	unlock(&c->joblk);
	if(ms != 0)
		return ms;
	if(c->cfg.scrubdays > 0)
		return (uvlong)c->cfg.scrubdays * 86400000;
	return Scrubperiodms;
}

/*
 * When the next tick is due, as an absolute millisecond on the same
 * clock srvscrubnextms reads: the timer writes it under joblk at every
 * slice and at every re-arm, so /status can report the schedule
 * without asking the proc anything.  It is armed before the proc
 * exists (srvscrubproc), so the field is never the zero of a timer
 * that has not run yet.
 */
static void
scrubarm(Srvctx *c, uvlong left)
{
	uvlong now;

	now = nsec()/1000000;
	lock(&c->joblk);
	c->scrubnext = now + left;
	unlock(&c->joblk);
}

/*
 * What /status renders as `scrubnext=' (status.c): milliseconds from
 * now until the timer's next tick, 0 once that moment has passed and
 * the tick has not yet re-armed.  nsec() is read outside joblk, which
 * is a spin lock.
 *
 * A tick that finds a pass running starts nothing and re-arms, so the
 * field answers when the schedule will next LOOK, which is the only
 * thing the timer decides; whether that look starts a pass depends on
 * what is running when it lands.
 */
uvlong
srvscrubnextms(Srvctx *c)
{
	uvlong now, next;

	now = nsec()/1000000;
	lock(&c->joblk);
	next = c->scrubnext;
	unlock(&c->joblk);
	return next > now ? next - now : 0;
}

/*
 * The scrub's timer, which is reclaimtimer's twin: not a job, waited
 * for separately by the shutdown and before the jobs because it is one
 * of the two things that could still start one, and reading
 * srvstopping between slices so as to be there to be waited for.
 */
static void
scrubtimer(void *a)
{
	Srvctx *c;
	uvlong left, period, t;

	c = a;
	for(;;){
		for(t = 0;; t += Scrubtickms){
			period = srvscrubperiod(c);
			left = period > t ? period - t : 0;
			scrubarm(c, left);
			if(left == 0)
				break;
			if(srvstopping(c)){
				lock(&c->joblk);
				c->scrubup = 0;
				unlock(&c->joblk);
				threadexits(nil);
			}
			sleep(Scrubtickms);
		}
		scrubgo(c, 1);
	}
}

/*
 * Start it, from srvnew once the map is adopted and the store is open
 * (srv.h), beside the reclaim timer and under the same rule: the two
 * are the last thing start-up does, so no refusal below them can free
 * the context under a proc that is reading it.
 */
int
srvscrubproc(Srvctx *c)
{
	scrubarm(c, srvscrubperiod(c));
	lock(&c->joblk);
	c->scrubup = 1;
	unlock(&c->joblk);
	if(proccreate(scrubtimer, c, Srvstack) < 0){
		lock(&c->joblk);
		c->scrubup = 0;
		unlock(&c->joblk);
		return -1;
	}
	return 0;
}

/* whether that proc is still reading the context: the shutdown's wait */
int
srvscrublive(Srvctx *c)
{
	int n;

	lock(&c->joblk);
	n = c->scrubup;
	unlock(&c->joblk);
	return n;
}

/*
 * The T1 knob over the period (srv.h).  0 puts `scrubdays' back.
 */
void
srvscrubms(Srvctx *c, uvlong ms)
{
	lock(&c->joblk);
	c->scrubms = ms;
	unlock(&c->joblk);
}

/*
 * layer-a §2.5's `forget <iid>': discard the fine-grained dirty
 * records for one peer (§7.1).
 *
 * §7.1 has the discard mark the peer `fullsync' — "reconcile every
 * object I hold against this peer before I may claim to be synced
 * with it".  There is no setter for that flag, and none is needed
 * here: store.md §9 records that nothing clears it until the
 * reconcile pass exists, and a peer is registered with it already
 * set, so `fullsync peer=' is already true of every peer this store
 * knows of and `storefullsync' answers 1 for a name it has never
 * seen.  What `forget' achieves today is therefore the discard alone
 * — the records go, /dirty loses their lines, /status's `dirty='
 * falls — and the coarse half of §7.1's meaning is already in force.
 * The stale mark at the monitor is untouched, as §2.5 says.
 */
static void
forgetpass(Sjob *j)
{
	Srvctx *c;
	Dirtyrec *dr;
	ulong n, i;
	int plen;

	c = j->ctx;
	plen = strlen(j->arg);
	if(dirtysnap(c->store, &dr, &n) < 0){
		joberr(j);
		return;
	}
	lock(&c->joblk);
	j->total = n;
	unlock(&c->joblk);
	/*
	 * The delete names the VERB's peer and not the record's, which is
	 * what keeps another peer's records out of it; the match above it
	 * is a filter over engine calls rather than the rule, and a
	 * mutation that removes it changes nothing a client can see.
	 */
	for(i = 0; i < n; i++){
		if(srvstopping(c))
			break;
		lock(&c->joblk);
		j->done = i+1;
		unlock(&c->joblk);
		if(dr[i].peerlen != plen
		|| memcmp(dr[i].peer, j->arg, plen) != 0)
			continue;
		if(dirtydel(c->store, dr[i].oid, dr[i].oidlen, j->arg) < 0){
			joberr(j);
			break;
		}
		lock(&c->joblk);
		j->dropped++;
		unlock(&c->joblk);
	}
	free(dr);
}

static int
ratearg(char *s, ulong *rate)
{
	char *e;
	ulong n;

	if(*s < '0' || *s > '9')
		return -1;
	n = strtoul(s, &e, 10);
	if(*e != 0 || n == 0)
		return -1;
	*rate = n;
	return 0;
}

/*
 * §2.5's `scrub [start|stop] [rate=<n>]', in that order and with each
 * word at most once — the grammar as the table spells it, so a line
 * that reorders or repeats them is `bad ctl' rather than half
 * understood.
 *
 * `scrub' with neither word changes nothing and succeeds: the table
 * makes both optional, /jobs is where progress is read, and a verb
 * that started a pass for a line that asked for none would make the
 * empty form the dangerous one.  `scrub rate=' alone sets the rate a
 * running pass reads on its next object and the next pass starts at.
 * A `start' while a pass is running is accepted and starts nothing:
 * §2.5 has the verb "return success once the job is accepted", and
 * the job asked for — that the index is being scrubbed — is already
 * running.  A `start' while a pass is STOPPING is refused instead,
 * with the local `shoalsrv: scrub stopping' (§14(29)): the job asked
 * for is not running and is not going to be.  A `stop' with no pass
 * running is accepted too and clears itself at the next `start'.
 *
 * NEITHER WORD TOUCHES THE SCHEDULE.  `stop' stops the pass that is
 * running and nothing more: the next tick starts a pass exactly as a
 * `start' would, because the flag it raises is the pass's and §2.5's
 * form has no word for a schedule.  `start' asks for a pass now and
 * leaves the timer where it was — it does not re-arm it, and the tick
 * it lands before is the no-op scrubgo describes.  That is the reading
 * §2.5's own `reclaim' row states for the reclaim walk, and the two
 * verbs agree by design rather than by accident (store.md §14(39)).
 * An operator who wants the scrub off has no verb for it; `scrub
 * rate=' is the knob for making it cheap.
 *
 * `reclaim' below reads the same rules off the same table row shape;
 * what differs is the fence, not the timer.
 */
char*
srvctlscrub(Srvctx *c, Sfid *f, int argc, char **argv)
{
	ulong rate;
	int i, start, stop;

	USED(f);
	i = 0;
	start = stop = 0;
	rate = 0;
	if(i < argc && strcmp(argv[i], "start") == 0){
		start = 1;
		i++;
	}else if(i < argc && strcmp(argv[i], "stop") == 0){
		stop = 1;
		i++;
	}
	if(i < argc && strncmp(argv[i], "rate=", 5) == 0){
		if(ratearg(argv[i]+5, &rate) < 0)
			return Ebadctl;
		i++;
	}
	if(i != argc)
		return Ebadctl;
	lock(&c->joblk);
	if(rate != 0)
		c->scrubrate = rate;
	if(stop)
		c->scrubstop = 1;
	unlock(&c->joblk);
	if(!start)
		return nil;
	/*
	 * The start itself is scrubgo's, which the timer calls too: one
	 * admission, one flag, one refusal for each of the three things
	 * that can go wrong.  A `rate=' on a line scrubgo refuses stands
	 * for the next pass, since it is what that pass starts at and
	 * this line's refusal does not unsay it.
	 */
	return scrubgo(c, 0);
}

/*
 * §2.5's `reclaim [start|stop]' (store.md §14(39)): the tombstone
 * reclaim walk, started by hand.  The grammar is `scrub's without the
 * `rate=' — the walk reads no grains and is paced by nothing — and
 * the three answers are the same three: a line with neither word
 * changes nothing and succeeds, a `start' over a running pass is
 * accepted and starts nothing, and a `start' over a pass that has
 * been told to stop is refused `shoalsrv: reclaim stopping'.
 *
 * `stop' stops the pass that is running.  It does not turn the timer
 * off: the next tick starts a pass exactly as a `reclaim start'
 * would, since the flag it clears is the pass's and §2.5's form has
 * no word for a schedule.  An operator who wants the walk off has no
 * verb for it (§14(39)).
 *
 * The verb is admin, and `start' alone is FENCED.  §2.5's fenced set
 * is "every verb that mutates data or replication state", and the
 * walk a `start' asks for will: §1.5's discard is what it is a walk
 * for, and it removes records as soon as the replication surface can
 * answer condition 1.  A fenced instance is one whose map may be
 * stale, which is exactly the state in which a discard decided
 * against `tombdays' and an epoch must not be made.
 *
 * `stop' is not in that set and is answered while fenced, the way
 * `fence off' is carved out of its own row (ctl.c): it mutates
 * nothing — it raises a flag a walk reads between two entries — and
 * an instance that has just been fenced is exactly where an operator
 * wants the walk it started stopped.  A row's `fenced' cell is per
 * verb, so the gate for `start' is here rather than there.
 *
 * The timer is not gated either — it starts no discard while
 * condition 1 is unanswerable, and a count taken under a fence is a
 * count and not a mutation — so what the fence refuses is the form
 * §2.5 governs.
 */
char*
srvctlreclaim(Srvctx *c, Sfid *f, int argc, char **argv)
{
	int start, stop;

	USED(f);
	start = stop = 0;
	if(argc > 1)
		return Ebadctl;
	if(argc == 1){
		if(strcmp(argv[0], "start") == 0)
			start = 1;
		else if(strcmp(argv[0], "stop") == 0)
			stop = 1;
		else
			return Ebadctl;
	}
	if(start && srvfencekind(c) != Fencenone)
		return Efenced;
	if(stop){
		lock(&c->joblk);
		c->reclaimstop = 1;
		unlock(&c->joblk);
	}
	if(!start)
		return nil;
	return reclaimgo(c, 0);
}

/*
 * §2.5's `forget <iid>'.  The argument is an instance id and not an
 * oid, so the row is an `fn' and the work is a pass (see the head of
 * this file).  An id no instance id can be is `bad ctl': §2.5's
 * error column for this verb names `fenced' and `bad ctl' and
 * nothing else, and a peer name is an argument rather than an object.
 * A well-formed id this store has no record for is not an error —
 * §7.1's meaning of forget for such a peer is already in force.
 *
 * So the bound is Iidlen, layer-a §3.3's node name plus a dot plus
 * the instance index, and NOT Peermax, which is the width of the
 * `peer' field a dirty record carries and is two digits narrower.
 * Bounding by the record would refuse the longest well-formed ids —
 * precisely the ids this store has no record for, which the sentence
 * above says are not an error.
 */
char*
srvctlforget(Srvctx *c, Sfid *f, int argc, char **argv)
{
	int n;

	USED(f);
	USED(argc);
	n = strlen(argv[0]);
	if(n < 1 || n > Iidlen)
		return Ebadctl;
	return jobstart(c, "forget", forgetpass, argv[0]);
}

