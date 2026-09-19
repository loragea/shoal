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
 * Start-up, the Srv glue and D16's shutdown.
 *
 * Start-up is here rather than in cmd/shoalsrv because every one of
 * its refusals is a rule from the design — the geometry check of
 * store.md §14(8), layer-a §3.4's identity, §6.3's adoption decision
 * and its two durable values — and a T1 program must be able to drive
 * them.  What is left in the command is argument parsing, opening the
 * device, reading the map file, posting the service and the shutdown's
 * trigger.
 */

static void	srvserved(Srvctx*);

static void
hexof(char *out, uchar *p, int n)
{
	static char hex[] = "0123456789abcdef";
	int i;

	for(i = 0; i < n; i++){
		out[2*i] = hex[p[i]>>4];
		out[2*i+1] = hex[p[i]&15];
	}
	out[2*n] = 0;
}

static int
unhex(uchar *out, char *s, int n)
{
	int i, c, v;

	for(i = 0; i < 2*n; i++){
		c = s[i];
		if(c >= '0' && c <= '9')
			v = c - '0';
		else if(c >= 'a' && c <= 'f')
			v = c - 'a' + 10;
		else if(c >= 'A' && c <= 'F')
			v = c - 'A' + 10;
		else
			return -1;
		if((i & 1) == 0)
			out[i/2] = v<<4;
		else
			out[i/2] |= v;
	}
	return s[2*n] == 0 ? 0 : -1;
}

/*
 * The adopted map's snapshots (dat.h).  smapmk takes a map already
 * parsed and the text it was parsed from, resolves this instance's own
 * record in it by layer-a §3.4's uuid, and wraps the three in a
 * snapshot with one reference — the installed one.  On success the
 * snapshot owns the map and the text; on failure the caller still does.
 */
static Smap*
smapmk(Cmap *m, char *text, long len, char *uuid)
{
	Smap *s;
	int i;

	for(i = 0; i < m->ninst; i++)
		if(strcmp(m->inst[i].uuid, uuid) == 0)
			break;
	if(i == m->ninst){
		werrstr("no instance record in the map carries uuid %s", uuid);
		return nil;
	}
	if((s = mallocz(sizeof *s, 1)) == nil)
		return nil;
	s->map = m;
	s->text = text;
	s->len = len;
	s->self = &m->inst[i];
	s->ref = 1;
	return s;
}

static void
smapfree(Smap *s)
{
	if(s == nil)
		return;
	mapfree(s->map);
	free(s->text);
	free(s);
}

/*
 * A reference on the snapshot in force, and the giving back of one.
 * The lock is held across a pointer copy and an int and nothing else;
 * the free below it runs OUTSIDE the lock, which is safe because a
 * snapshot whose count has reached zero is one that is no longer in
 * force and that nobody can reach to count up again (dat.h).
 */
Smap*
srvmapget(Srvctx *c)
{
	Smap *s;

	lock(&c->maplk);
	s = c->smap;
	s->ref++;
	unlock(&c->maplk);
	return s;
}

void
srvmapput(Srvctx *c, Smap *s)
{
	int n;

	if(s == nil)
		return;
	lock(&c->maplk);
	n = --s->ref;
	if(n == 0)
		c->nsmap--;
	unlock(&c->maplk);
	if(n == 0)
		smapfree(s);
}

/*
 * The swap, T1's alone (srv.h).  It parses, resolves `self' exactly as
 * start-up does and refuses a text that fails either, leaving the old
 * snapshot in force; nothing durable is written, because §6.3's
 * adoption decision belongs to the refresh loop that is not built
 * (store.md §14(18), §14(52)).
 *
 * The error string is this call's own buffer, valid until the next
 * call — like status.c's render error, and for the same reason: there
 * is one caller and it is a test program.
 */
static char swaperr[ERRMAX];

static char*
swapwhy(void)
{
	rerrstr(swaperr, sizeof swaperr);
	return swaperr;
}

char*
srvmapswap(Srvctx *c, char *text, long len)
{
	Cmap *m;
	Smap *s, *old;
	char *t;

	if(len <= 0){
		snprint(swaperr, sizeof swaperr, "shoalsrv: empty map text");
		return swaperr;
	}
	if((t = malloc(len)) == nil)
		return swapwhy();
	memmove(t, text, len);
	if((m = mapparse(t, len)) == nil){
		free(t);
		return swapwhy();
	}
	if((s = smapmk(m, t, len, c->uuid)) == nil){
		mapfree(m);
		free(t);
		return swapwhy();
	}
	/*
	 * `iid' is a copy of the record's, taken at start-up and read
	 * from everywhere without a hold, so a map that gives this uuid
	 * a different iid is refused rather than served under a name
	 * half the server no longer agrees with.  A refresh has to
	 * decide what such a map means (store.md §14(52)).
	 */
	if(strcmp(s->self->iid, c->iid) != 0){
		snprint(swaperr, sizeof swaperr,
			"shoalsrv: map names uuid %s as %s, not %s",
			c->uuid, s->self->iid, c->iid);
		smapfree(s);
		return swaperr;
	}
	lock(&c->maplk);
	old = c->smap;
	c->smap = s;
	c->nsmap++;
	unlock(&c->maplk);
	srvmapput(c, old);
	return nil;
}

/* the T1 observable over the two (srv.h) */
void
srvmapcount(Srvctx *c, int *nsnap, int *nref)
{
	lock(&c->maplk);
	if(nsnap != nil)
		*nsnap = c->nsmap;
	if(nref != nil)
		*nref = c->smap->ref;
	unlock(&c->maplk);
}

/*
 * store.md §7: in the server every proc is a proccreate, the Reqqueue
 * procs included, and the engine takes its spawn callback rather than
 * making procs itself so that the same engine runs under a plain-libc
 * T1 program.  This is the engine's half; the queue procs are lib9p's
 * and take the program's mainstacksize instead, which is why srv.h
 * makes setting that the program's job.
 */
static int
srvspawn(void (*fn)(void*), void *a)
{
	if(proccreate(fn, a, Srvstack) < 0)
		return -1;
	return 0;
}

static void
srvend(Srv *s)
{
	srvshutdown(s->aux);
}

/*
 * Srv.free, which lib9p calls at the very end of its own srvclose —
 * after the fid pool and the request pool have been freed, and so
 * after the last srvdestroyfid and srvdestroyreq.  It is the only
 * moment at which this context is certainly no longer in use, which is
 * what srvfree waits for.
 */
static void
srvfreed(Srv *s)
{
	Srvctx *c;

	c = s->aux;
	lock(&c->joblk);
	c->released = 1;
	unlock(&c->joblk);
}

Srvctx*
srvnew(Srvcfg *cfg)
{
	char want[64], *text;
	Srvctx *c;
	Sbsel sel;
	Adopt ad;
	Cmap *map;
	uchar monid[16];
	long len;
	int fl;

	if((c = mallocz(sizeof *c, 1)) == nil)
		return nil;
	c->cfg = *cfg;
	c->dev = cfg->dev;
	map = nil;
	len = cfg->maplen;
	if((text = malloc(len)) == nil){
		free(c);
		return nil;
	}
	memmove(text, cfg->maptext, len);
	if((map = mapparse(text, len)) == nil)
		goto Fail;

	/*
	 * The superblock, read before the store is opened: superselect
	 * writes nothing, and the geometry and the identity it carries
	 * are what decide whether this instance may serve this map at
	 * all.
	 */
	if(superselect(c->dev, &sel) < 0)
		goto Fail;
	if(sel.start < 0){
		werrstr("neither superblock copy is valid: %s; %s",
			sel.why[0], sel.why[1]);
		goto Fail;
	}
	c->sb = sel.sb[sel.start];

	/*
	 * store.md §14(8): the server MUST refuse to serve if the adopted
	 * map's blksz, objmax or csumalg differs from what the disk was
	 * formatted with — csumalg because a mismatch invalidates every
	 * stored digest.
	 */
	if(map->blksz != c->sb.blksz){
		werrstr("map blksz %lud is not the disk's %lud",
			map->blksz, c->sb.blksz);
		goto Fail;
	}
	if(map->objmax != c->sb.objmax){
		werrstr("map objmax %llud is not the disk's %llud",
			map->objmax, c->sb.objmax);
		goto Fail;
	}
	snprint(want, sizeof want, "%s", csumalgname(c->sb.csumalg));
	if(strcmp(map->csumalg, want) != 0){
		werrstr("map csumalg %s is not the disk's %s",
			map->csumalg, want);
		goto Fail;
	}

	/*
	 * layer-a §3.4: the disk carries the identity, and the map's
	 * instance record for it is the one whose uuid= matches.  There
	 * is no other way for an instance to learn its own iid, and the
	 * whole of §6.4's self-state and §4.3's primaryship is asked of
	 * that record.
	 *
	 * That resolution is what makes the first snapshot (dat.h), and
	 * from here on the map is reached through it: the snapshot owns
	 * the parse and the text, and the installed reference is the
	 * context's until the shutdown gives it back.
	 */
	hexof(c->uuid, c->sb.uuid, 16);
	if((c->smap = smapmk(map, text, len, c->uuid)) == nil)
		goto Fail;
	c->nsmap = 1;
	map = nil;
	text = nil;
	strcpy(c->iid, c->smap->self->iid);
	strcpy(c->monid, c->smap->map->monid);

	/*
	 * layer-a §6.3's adoption decision, over the pair the superblock
	 * holds (store.md §2.2).  A map this instance may not adopt is
	 * refused at start rather than served: there is no refresh in
	 * this wave, so there is no later map to replace it with, and an
	 * instance that served under a map it had refused would be the
	 * split brain the rule exists to prevent.  /status's
	 * epochregress= and monidmismatch= therefore always read `no'
	 * while this server is running (store.md §14(22)).
	 */
	memset(&ad, 0, sizeof ad);
	ad.pinned = c->sb.monidset != 0;
	hexof(ad.monid, c->sb.monid, 16);
	ad.epoch = c->sb.epochhigh;
	/*
	 * An unpinned instance has no epoch to regress from, so
	 * mapadoptable passes every map it is shown; a disk that carries
	 * an adopted epoch and no pinned monid would therefore adopt a
	 * map below that epoch without a word.  This server never writes
	 * that pair — the pin below is made durable before the epoch is,
	 * and a freshly formatted store carries neither — so a disk that
	 * holds it is one no adoption decision can be made about, and it
	 * is refused rather than served.
	 *
	 * A T1 image can hold it all the same, and not only where a test
	 * calls epochadopt itself: lib/ckpt.c's `pubatpage' point adopts
	 * an epoch from inside a checkpoint, and on a store whose monid
	 * was never pinned that leaves exactly this pair.
	 */
	if(!ad.pinned && c->sb.epochhigh != 0){
		werrstr("the disk has adopted epoch %llud with no pinned "
			"monitor identity", c->sb.epochhigh);
		goto Fail;
	}
	if((fl = mapadoptable(&ad, c->smap->map)) != Mapok){
		werrstr("map may not be adopted: %s%s%s",
			(fl&Mapregress) ? adoptwhy(Mapregress) : "",
			(fl&Mapregress) && (fl&Mapmonid) ? " " : "",
			(fl&Mapmonid) ? adoptwhy(Mapmonid) : "");
		goto Fail;
	}

	c->cfg.store.spawn = srvspawn;
	c->cfg.store.noflush = cfg->noflush;
	if((c->store = storeopen(c->dev, &c->cfg.store)) == nil)
		goto Fail;

	/*
	 * §6.3: both facts must survive a restart, and the epoch MUST be
	 * durable before the instance acts under it.  Nothing is served
	 * until these have returned.
	 *
	 * The order of the two is load-bearing, not cosmetic: mapadoptable
	 * answers Mapok for an unpinned instance whatever the map's epoch,
	 * so the pin must be durable before the epoch it was decided
	 * against is.  A crash between them leaves the pin with no epoch,
	 * which is a state that adopts nothing it should not; the reverse
	 * order would leave an epoch with no pin, which is the state
	 * refused above.
	 */
	if(!ad.pinned){
		if(unhex(monid, c->smap->map->monid, 16) < 0){
			werrstr("map monid %s is not 32 hex digits",
				c->smap->map->monid);
			goto Fail;
		}
		if(monidpin(c->store, monid) < 0)
			goto Fail;
	}
	if(epochadopt(c->store, c->smap->map->epoch) < 0)
		goto Fail;

	/*
	 * §6.4's fence state.  The adoption above is this instance's one
	 * successful refresh; F4 starts clear.  `leasems' is copied out
	 * of the map here and read from the Fence afterwards, so it is
	 * the one map value a refresh would have to carry across a swap
	 * as well; srvmapswap does not, because F1's clock is inert
	 * while nothing refreshes (store.md §14(19), §14(52)).
	 */
	c->fence.leasems = c->smap->map->leasems;
	fencerefresh(&c->fence, 0);

	if(srvqinit(c, cfg->nqueue) < 0)
		goto Fail;

	c->srv.aux = c;
	c->srv.attach = srvattach;
	c->srv.walk = srvwalk;
	c->srv.open = srvopen;
	c->srv.read = srvread;
	c->srv.write = srvwrite;
	c->srv.stat = srvstat;
	c->srv.create = srvcreate;
	c->srv.remove = srvremove;
	c->srv.wstat = srvwstat;
	c->srv.flush = srvqflush;
	c->srv.destroyfid = srvdestroyfid;
	c->srv.destroyreq = srvdestroyreq;
	c->srv.end = srvend;
	c->srv.free = srvfreed;

	/*
	 * The tombstone reclaim walk's timer (job.c), started last: it
	 * reads this context for as long as it runs, and every refusal
	 * above it frees the context.  It needs the adopted map, whose
	 * `tombdays' is its period, and the store, which its passes walk.
	 */
	if(srvreclaimproc(c) < 0)
		goto Fail;
	return c;

Fail:
	if(c->store != nil){
		srvqfree(c);
		storeclose(c->store);
		c->store = nil;
	}
	/*
	 * Either the snapshot was made and owns the two, or it was not
	 * and the locals still do.
	 */
	srvmapput(c, c->smap);
	mapfree(map);
	free(text);
	free(c);
	return nil;
}

Srv*
srv9p(Srvctx *c)
{
	return &c->srv;
}

void
srvrun(Srvctx *c, int infd, int outfd)
{
	c->srv.infd = infd;
	c->srv.outfd = outfd;
	srvserved(c);
	threadsrv(&c->srv);
}

/*
 * Post the service and return.  threadpostmountsrv writes the
 * directory entry and leaves the loop to a proc of its own, so this
 * returns with nothing served yet and the context in that proc's
 * hands until the connection it serves ends (srv.h): it is srvrun,
 * where the loop is this proc's, that marks the context served and so
 * makes srvfree's wait mean something.
 */
void
srvpost(Srvctx *c, char *name)
{
	threadpostmountsrv(&c->srv, name, nil, 0);
}

/*
 * A background job: work inside the engine that is not a Req and so is
 * invisible to the drain below.  A pass that walks the store — the
 * scrub of layer-a §2.5 is the first — runs in a proc of its own,
 * outside every queue, and store.md §9 forbids storeclose while
 * anything is still inside the engine.  So such a proc takes a job for
 * its whole run and the shutdown waits for the count to fall to zero,
 * exactly as it waits for the requests in flight.
 *
 * srvjobstart answers -1 once the shutdown has begun, which is what a
 * caller asks before starting one.  The passes of job.c take the same
 * count under this same lock instead, because their refusal has to be
 * decided in the hold that raises the verb's own flag (jobadmit);
 * srvstopping is the same fact for a pass already running, which
 * SHOULD test it between units of work and return rather than leave
 * the shutdown waiting for it.
 */
int
srvjobstart(Srvctx *c)
{
	lock(&c->joblk);
	if(c->stopping){
		unlock(&c->joblk);
		werrstr("shoalsrv: shutting down");
		return -1;
	}
	c->njob++;
	unlock(&c->joblk);
	return 0;
}

void
srvjobend(Srvctx *c)
{
	lock(&c->joblk);
	if(c->njob > 0)
		c->njob--;
	unlock(&c->joblk);
}

int
srvstopping(Srvctx *c)
{
	int n;

	lock(&c->joblk);
	n = c->stopping;
	unlock(&c->joblk);
	return n;
}

/*
 * How many jobs are held, which is what the shutdown waits for.  It is
 * read back for the same reason srvcount is: a caller that has taken
 * the service down asks what is left rather than inferring it from a
 * call that would refuse for a different reason.
 */
int
srvjobcount(Srvctx *c)
{
	int n;

	lock(&c->joblk);
	n = c->njob;
	unlock(&c->joblk);
	return n;
}

/*
 * The reclaim timer is not one of the jobs above — it makes no engine
 * call — but it reads the context, and srvfree frees that as soon as
 * the shutdown is over.  So the shutdown waits for the proc to see
 * `stopping' and end.  The wait is bounded by the proc's own slice,
 * which is half a second (job.c), and no pass can be started behind
 * it: jobadmit reads `stopping' under joblk and refuses once the
 * shutdown has begun, which is the same gate every verb meets.
 */
static void
reclaimwait(Srvctx *c)
{
	while(srvreclaimlive(c))
		sleep(5);
}

static void
jobwait(Srvctx *c)
{
	int n;

	for(;;){
		lock(&c->joblk);
		n = c->njob;
		unlock(&c->joblk);
		if(n == 0)
			return;
		sleep(5);
	}
}

/*
 * D16's order, which store.md §9 derives from the close contract
 * rather than from taste: stop accepting requests, let the ones in
 * flight drain, wait for the reclaim timer and then for the background
 * jobs, and only then close the store.  The timer goes before the jobs
 * because it is the one thing that could still start one; once it has
 * ended, nothing can add to what jobwait waits for.  The loop is what
 * stops first here — this runs from Srv.end,
 * which lib9p calls once the connection has gone — and the drain and
 * the job wait
 * are what make the engine's "quiesce, then close" true: no call
 * taking the Store* may still be in flight when storeclose runs,
 * because such a call blocks on the state lock holding nothing that
 * keeps the Store alive.
 *
 * Nothing new is taken on from here — the loop has ended and
 * jobadmit refuses — and the §13 points that can hold a request are
 * cleared first, so a request left holding cannot hold the drain up.
 * The same clearing is what frees a pass parked at `jobhold' before
 * jobwait reaches it, which matters more there: that wait is unbounded
 * by design, because §9 forbids closing the store while a pass is
 * still inside the engine (srv.h).  The cell point goes with them: its cells are the file table's, which
 * is the program's and not this context's, so a context that ends
 * without clearing them would leave them to the next server started in
 * the same program.
 *
 * The fids outlive this, but what they are holding may not: a fid open
 * when the connection dropped can be holding a stage, and §9 allows
 * nothing but the Objsnap calls once the store has closed.  So every
 * live fid's auxclose runs here — after the two waits, because a hook
 * may call the engine, and before the store closes, because that is
 * the whole point of it.  It runs before `closed' is set for the same
 * reason: that flag is what tells the hooks the engine has gone.
 *
 * A stage that step 7 stripped from its fid is in no fid's slot to be
 * found by that sweep: the flush hook may make no engine call, so it
 * parks the handle for obj.c's drain instead (dat.h).  srvstagedrain
 * therefore runs behind the sweep and still before `closed', which is
 * the last moment §9 allows the release; what a drain after that point
 * would leave is memory the process frees and an engine stage nothing
 * can give back.
 *
 * It cannot deadlock against a clunk: srvfidsclose sweeps with the
 * registry lock held and a hook may call the engine but never the
 * registry (dat.h), and by here the service loop has ended and the
 * drain has finished, so nothing else is walking or clunking a fid.
 *
 * lib9p then frees the fid pool, after Srv.end, so srvdestroyfid runs
 * with the store already closed: auxclose is spent by then and
 * auxfree is what runs — which is exactly what §9 permits an Objsnap
 * taken before the close, and is how the /obj directory fids of the
 * enumeration surface will end their lives.
 */
void
srvshutdown(Srvctx *c)
{
	if(srvstopping(c))
		return;
	lock(&c->joblk);
	c->stopping = 1;
	unlock(&c->joblk);
	srvholdclear(c);
	srvcellpoint(c, 0);
	srvqdrain(c);
	reclaimwait(c);
	jobwait(c);
	srvfidsclose(c);
	srvstagedrain(c);		/* the last moment §9 allows one */
	c->closed = 1;
	srvqfree(c);
	if(c->store != nil){
		storeclose(c->store);
		c->store = nil;
	}
}

static void
srvserved(Srvctx *c)
{
	lock(&c->joblk);
	c->served = 1;
	unlock(&c->joblk);
}

/*
 * Wait for lib9p to let go of the Srv this context begins with.  The
 * shutdown above is not that moment: it runs from Srv.end, which lib9p
 * calls while the loop's own reference is still held, and a queue proc
 * that has just answered a request is counted complete by
 * srvdestroyreq — from closereq, inside respond — before respond
 * releases the Srv.  So the drain converges, the loop ends and srvrun
 * returns with that reference still outstanding; the release then
 * takes lib9p through freefidpool, which runs srvdestroyfid over the
 * server's own fid registry.  Freeing the context before that walks a
 * registry whose lock is no longer there.
 *
 * Srv.free is lib9p's last act, so `released' is the observable; a
 * context that never ran a loop in this proc waits for nothing, which
 * is what a caller that posted the service rather than running it
 * gets (srvpost).
 */
int
srvreleased(Srvctx *c)
{
	int n;

	lock(&c->joblk);
	n = !c->served || c->released;
	unlock(&c->joblk);
	return n;
}

static void
waitreleased(Srvctx *c)
{
	while(!srvreleased(c))
		sleep(5);
}

void
srvfree(Srvctx *c)
{
	if(c == nil)
		return;
	srvshutdown(c);
	waitreleased(c);
	/*
	 * The installed reference, given back last: lib9p has let go of
	 * the Srv, so every request that could have been holding one has
	 * finished (srvfreed above), and this put is therefore the one
	 * that frees the snapshot.
	 */
	srvmapput(c, c->smap);
	c->smap = nil;
	free(c);
}

Store*
srvstore(Srvctx *c)
{
	return c->store;
}

Cmap*
srvmap(Srvctx *c)
{
	Cmap *m;

	lock(&c->maplk);
	m = c->smap->map;
	unlock(&c->maplk);
	return m;
}

char*
srviid(Srvctx *c)
{
	return c->iid;
}
