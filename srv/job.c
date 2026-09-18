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

	/* what qjob runs on the object's queue */
	Jscrub		= 0,
	Jdiscard,
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
 * correct object `corrupt'.  The same goes for the reclaim walk's
 * discards: the queue is the object's ordering point (§5.4 step 2),
 * and a mutation outside it is ordered against nothing.
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
	uvlong	ver, wepoch, epoch;
	int	rc;
	int	bad;
	char	err[ERRMAX];
};

static char Eshutting[] = "shoalsrv: shutting down";
static char Enomem[] = "shoalsrv: out of memory";

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
	case Jdiscard:
		if(objdiscard(j->ctx->store, j->oid, j->oidlen, j->ver,
			j->wepoch, j->epoch) < 0){
			j->rc = -1;
			rerrstr(j->err, sizeof j->err);
		}
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
 * The job list.  A verb that starts a pass links the record before it
 * spawns the proc, so /jobs shows the job from the moment §2.5 says
 * it is accepted; the proc unlinks it as its last act before giving
 * the job back.
 */
static Sjob*
joblink(Srvctx *c, char *verb, void (*fn)(Sjob*), char *arg)
{
	Sjob *j;

	if((j = mallocz(sizeof *j, 1)) == nil)
		return nil;
	j->ctx = c;
	j->verb = verb;
	j->fn = fn;
	if(arg != nil)
		strecpy(j->arg, j->arg + sizeof j->arg, arg);
	lock(&c->joblk);
	j->next = c->jobs;
	c->jobs = j;
	unlock(&c->joblk);
	return j;
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
	if((j = joblink(c, verb, fn, arg)) == nil){
		srvjobend(c);
		return Enomem;
	}
	if(proccreate(jobproc, j, Srvstack) < 0){
		jobunlink(j);
		srvjobend(c);
		return Enomem;
	}
	return nil;
}

/*
 * /jobs, layer-a §2.2: one line per running or queued background job.
 * It reads the server's own list and touches the engine, so unlike
 * the other status files it is rendered on the service loop.
 *
 * The lines are copied out under the lock and formatted after it:
 * joblk is a spin lock, and textprint allocates.
 */
char*
srvjobstext(Srvctx *c, Sfid *f, Text *t)
{
	Sjob cp[8], *j;
	ulong rate;
	int i, n;

	USED(f);
	n = 0;
	rate = scrubrate(c);
	lock(&c->joblk);
	for(j = c->jobs; j != nil && n < nelem(cp); j = j->next)
		cp[n++] = *j;
	unlock(&c->joblk);
	for(i = 0; i < n; i++)
		textprint(t, "job=%s state=%s rate=%lud done=%llud/%llud "
			"bad=%llud tombs=%llud dropped=%llud\n",
			cp[i].verb, cp[i].running ? "running" : "queued",
			rate, cp[i].done, cp[i].total, cp[i].bad,
			cp[i].tombs, cp[i].dropped);
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
 */
static void
scrubpace(Srvctx *c, uvlong *bytes, vlong t0, uvlong len)
{
	vlong want;
	ulong rate;

	rate = scrubrate(c);
	*bytes += len + Scrubfloor;
	want = ((vlong)*bytes * 1000) / ((vlong)rate * 1024);
	while(nsec()/1000000 - t0 < want){
		if(passover(c))
			return;
		sleep(Scrubslicems);
	}
}

/*
 * store.md §9's tombstone reclaim, which is the caller's walk and not
 * the engine's: the engine holds no `tombdays', because layer-a §3.1
 * makes it a map-header attribute.  The walk opens a /tombs snapshot,
 * tests each entry's mtime against the cutoff that attribute names
 * and its wepoch against this instance's map epoch, and discards by
 * the entry's OWN key rather than by its slot — which is what makes
 * it safe under concurrent mutation, since §1.5's receiver checks
 * then refuse a record that is not the one the walk inspected instead
 * of removing whatever the slot came to hold.
 *
 * It runs from the scrub pass and has no verb of its own.  layer-a
 * §2.5 fixes the ctl grammar and has no verb for it, so a new one
 * would be a wire change; `scrub' is the only verb §2.5 gives an
 * instance for walking its own index on a schedule, and store.md §8
 * already makes that pass the place other whole-index work rides on.
 * store.md §14(31) records it.
 */
static void
reclaim(Srvctx *c, Sjob *j)
{
	char buf[ERRMAX];
	Objsnap *sn;
	Objinfo oi;
	Qwork w;
	uchar oid[Oidmax];
	vlong cutoff;
	ulong i, n;
	int oidlen, rc;

	/*
	 * `tombdays' is the map's, layer-a §3.1, and 0 is a value it may
	 * carry: the cutoff is then the present, and a tombstone written
	 * before this second is reclaimable.  Nothing here special-cases
	 * it — the comparison says what the attribute means.
	 */
	cutoff = time(0) - (vlong)c->map->tombdays*86400;
	if((sn = srvsnapopen(c->store, Snaptomb, buf, sizeof buf)) == nil)
		return;
	n = objsnapcount(sn);
	memset(&w, 0, sizeof w);
	w.ctx = c;
	w.op = Jdiscard;
	w.epoch = c->map->epoch;
	for(i = 0; i < n; i++){
		if(passover(c))
			break;
		rc = objsnapent(sn, i, oid, &oidlen, &oi);
		if(rc < 0)
			break;
		if(rc == 0)
			continue;
		if(oi.mtime >= cutoff)
			continue;
		if(oi.wepoch >= w.epoch)
			continue;
		memmove(w.oid, oid, oidlen);
		w.oidlen = oidlen;
		w.ver = oi.ver;
		w.wepoch = oi.wepoch;
		if(qjob(&w) == 0)
			j->tombs++;
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
	t0 = nsec()/1000000;
	for(slot = 0; slot < st.nslots; slot++){
		if(passover(c))
			break;
		rc = objslot(c->store, (ulong)slot, oid, &oidlen, &oi);
		lock(&c->joblk);
		j->done = slot+1;
		unlock(&c->joblk);
		if(rc < 0)
			break;
		if(rc == 0 || oi.state != Slive)
			continue;
		memmove(w.oid, oid, oidlen);
		w.oidlen = oidlen;
		if(qjob(&w) == 0 && w.bad){
			lock(&c->joblk);
			j->bad++;
			unlock(&c->joblk);
		}
		scrubpace(c, &bytes, t0, oi.len);
	}
	if(!passover(c))
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
	if(dirtysnap(c->store, &dr, &n) < 0)
		return;
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
		if(dirtydel(c->store, dr[i].oid, dr[i].oidlen, j->arg) < 0)
			break;
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
	if(start && c->scrubbing)
		start = 0;
	if(start){
		c->scrubbing = 1;
		c->scrubstop = 0;
	}
	unlock(&c->joblk);
	if(!start)
		return nil;
	return jobstart(c, "scrub", scrubpass, nil);
}

/*
 * §2.5's `forget <iid>'.  The argument is an instance id and not an
 * oid, so the row is an `fn' and the work is a pass (see the head of
 * this file).  An id no dirty record can carry is `bad ctl': §2.5's
 * error column for this verb names `fenced' and `bad ctl' and
 * nothing else, and a peer name is an argument rather than an object.
 * A well-formed id this store has no record for is not an error —
 * §7.1's meaning of forget for such a peer is already in force.
 */
char*
srvctlforget(Srvctx *c, Sfid *f, int argc, char **argv)
{
	int n;

	USED(f);
	USED(argc);
	n = strlen(argv[0]);
	if(n < 1 || n > Peermax)
		return Ebadctl;
	return jobstart(c, "forget", forgetpass, argv[0]);
}

