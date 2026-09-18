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
 * The background passes, /jobs, and the two ctl verbs that start one:
 * `scrub' (layer-a §2.5, §7.5, store.md §8) and `forget' (§2.5, §7.1).
 *
 * §2.5: "Commands that start background work return success once the
 * job is accepted; progress is read from /jobs."  Both verbs here do
 * exactly that, and both run in a proc of their own outside every
 * queue, holding one of srv.h's jobs for the whole run so that the
 * shutdown waits for them: store.md §9 forbids storeclose while
 * anything is still inside the engine, and the request drain cannot
 * see a proc that is not a request.  A pass tests srvstopping between
 * objects and gives up rather than leave the shutdown waiting.
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

static void	scrubpass(Sjob*);
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
	 * §13's point, while the record is still on the list: /jobs lists
	 * a pass only while it is running or queued, so what one finished
	 * with is readable here and nowhere later (srv.h).
	 */
	srvjobhold(c);
	lock(&c->joblk);
	if(strcmp(j->verb, "scrub") == 0)	/* the pass is over */
		c->scrubbing = 0;
	unlock(&c->joblk);
	jobunlink(j);
	srvjobend(c);
	threadexits(nil);
}

/*
 * Accept a pass: take the job the shutdown waits on, link the record
 * /jobs shows, and spawn the proc.  The job is taken before the proc
 * exists so that a shutdown starting in the window cannot slip past
 * it; every way out from here gives it back.
 */
static char*
jobstart(Srvctx *c, char *verb, void (*fn)(Sjob*), char *arg)
{
	Sjob *j;

	if(srvjobstart(c) < 0)
		return Eshutting;
	if((j = mallocz(sizeof *j, 1)) == nil){
		srvjobend(c);
		return Enomem;
	}
	j->ctx = c;
	j->verb = verb;
	j->fn = fn;
	if(arg != nil)
		strecpy(j->arg, j->arg + sizeof j->arg, arg);
	/*
	 * The cap and the link are one hold of the lock, so that two
	 * verbs cannot both find room for the last job.
	 */
	lock(&c->joblk);
	if(joblen(c) >= Njobmax){
		unlock(&c->joblk);
		free(j);
		srvjobend(c);
		return Ejobs;
	}
	j->next = c->jobs;
	c->jobs = j;
	unlock(&c->joblk);
	if(proccreate(jobproc, j, Srvstack) < 0){
		jobunlink(j);
		srvjobend(c);
		return Enomem;
	}
	return nil;
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

static int
scrubstopped(Srvctx *c)
{
	int n;

	lock(&c->joblk);
	n = c->scrubstop;
	unlock(&c->joblk);
	return n;
}

static int
passover(Srvctx *c)
{
	return srvstopping(c) || scrubstopped(c);
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
		if(passover(c))
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
 * It runs from the scrub pass and has no verb of its own.  layer-a
 * §2.5 fixes the ctl grammar and has no verb for it, so a new one
 * would be a wire change; `scrub' is the only verb §2.5 gives an
 * instance for walking its own index on a schedule, and store.md §8
 * already makes that pass the place other whole-index work rides on.
 */
static void
reclaim(Srvctx *c, Sjob *j)
{
	char buf[ERRMAX];
	Objsnap *sn;
	Objinfo oi;
	uchar oid[Oidmax];
	uvlong epoch;
	vlong cutoff;
	ulong i, n;
	int oidlen, rc;

	cutoff = time(0) - (vlong)c->map->tombdays*86400;
	epoch = c->map->epoch;
	if((sn = srvsnapopen(c->store, Snaptomb, buf, sizeof buf)) == nil){
		werrstr("%s", buf);
		joberr(j);
		return;
	}
	n = objsnapcount(sn);
	for(i = 0; i < n; i++){
		if(passover(c))
			break;
		rc = objsnapent(sn, i, oid, &oidlen, &oi);
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
		if(passover(c))
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
		 * index it says it walked, so the reclaim below must not
		 * ride on it.  The walk goes on all the same: one object
		 * that would not read says nothing about the next, and
		 * stopping here would leave the rest of the index
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
	/*
	 * The reclaim walk rides on a scrub that COMPLETED and found
	 * everything it walked readable, and on no other: it counts what
	 * a whole walk of the index found, and a walk that stopped
	 * part-way — told to stop, shutting down, or broken off by an
	 * index read that failed — has counted a prefix.  Reporting that
	 * prefix as the pass's answer would make a partial pass
	 * indistinguishable from a whole one.  The `err' test is the
	 * live one of the three for a walk that ran to the end with an
	 * object it could not read.
	 */
	if(slot >= st.nslots && !passover(c) && j->err[0] == 0)
		reclaim(c, j);
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
 * running.  A `stop' with no pass running is accepted too and clears
 * itself at the next `start'.
 */
char*
srvctlscrub(Srvctx *c, Sfid *f, int argc, char **argv)
{
	char *e;
	ulong rate;
	int i, start, stop, wasstop;

	USED(f);
	i = 0;
	start = stop = 0;
	rate = 0;
	wasstop = 0;
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
	if(start && c->scrubbing)
		start = 0;
	if(start){
		wasstop = c->scrubstop;
		c->scrubbing = 1;
		c->scrubstop = 0;
	}
	unlock(&c->joblk);
	if(!start)
		return nil;
	if((e = jobstart(c, "scrub", scrubpass, nil)) == nil)
		return nil;
	/*
	 * The flag is raised above, before it is known that a pass will
	 * run, because raising it after the proc exists would race the
	 * proc's own clearing of it.  So a jobstart that refuses — the
	 * shutdown, the job cap, no memory — has to put it back: a flag
	 * left raised makes every later `scrub start' answer success and
	 * start nothing, which is the silent half of the failure.  The
	 * stop flag goes back with it, so the refused line has changed
	 * nothing about the pass; a `rate=' on the same line stands,
	 * since it is what the NEXT pass starts at and this line's
	 * refusal does not unsay it.
	 */
	lock(&c->joblk);
	c->scrubbing = 0;
	c->scrubstop = wasstop;
	unlock(&c->joblk);
	return e;
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

