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
 * Object I/O: layer-a §2.4's /obj/<oid> and /meta/<oid>, and the
 * Tcreate in /obj that makes one.  Every operation here names an
 * object, so every one of them runs on that object's queue (§5.4.1,
 * store.md §7) and leaves through srvqdone.
 *
 * What a client operation is, in layer-a's terms, is §5.4's write
 * path: admission (step 1), the object's ordering point (step 2), a
 * staged update (step 3), the replication round (steps 4 and 5), the
 * commit (step 6) and the discard (step 7).  This build has no peer
 * client (store.md §14(18)), so steps 4 and 5 are evaluated from the
 * map alone: a placement of one member acks alone and a placement with
 * any other member answers `degraded', which is §14(34).
 *
 * Not everything step 1 asks can be asked here either.  The currency
 * check (§5.2) needs peers, so no check is ever completed, `cur='
 * renders 0 and §5.1's clause (c) is not evaluated — §14(35) records
 * what that costs.  Primaryship is computed from the static map and IS
 * enforced, for role=client alone: §5.1 and §5.4 are about a client
 * read and a client write, and an operator's access to a reserved
 * `shoal.' id (§2.1) is neither.
 *
 * The stage is per FID, one at a time (dat.h's Sstage).  For a client
 * operation it is created at step 3 and consumed at step 6 or
 * discarded at step 7, inside the one request; on the /repl fid of
 * the replication surface it holds an engine Stage across many
 * Twrites, which is the lifetime §3.6 gives one and the reason the
 * state is the fid's rather than the request's.  The four calls the
 * channel drives that stage through are below, beside the client
 * paths that share the slot.
 */

/*
 * The two strings store.md §14(37) pairs: one condition said to the two
 * kinds of owner a stage has.  A CLIENT whose staged update went —
 * step 7 for another request on its fid, or the idle sweep — is told
 * `staged update discarded'; a /repl sender CONTINUING a transfer whose
 * earlier chunks are gone is told the engine's own `stage expired',
 * which is also what the engine answers a chunk on a handle its sweep
 * has stripped.  Neither carries a §2.6 prefix: nothing §2.6 names has
 * happened, and the operation is retried rather than refused on its
 * merits.
 */
static char Estagegone[] = "shoalsrv: staged update discarded";
char Estageexp[] = "stage expired";
static char Eoom[] = "shoalsrv: out of memory";
static char Ewstatfield[] = "shoalsrv: only length may be set";
char Efidstate[] = "shoalsrv: the fid holds state of its own";

static long	wclamp(Srvctx*, uvlong off, long n);

/*
 * The object's own numbers, as the server keeps them.  layer-a §2.4's
 * /meta line reports `blksz=' from the geometry, which is the disk's
 * rather than the map's: srvnew refuses a map whose blksz is not the
 * disk's (store.md §14(8)), so the superblock read at start-up is the
 * one both agree on.
 */
static ulong
objblksz(Srvctx *c)
{
	return c->sb.blksz;
}

static uvlong
objmaxof(Srvctx *c)
{
	return c->sb.objmax;
}

/* an oid as the map library takes it: §1.1 ids are text */
static char*
oidstr(char *buf, uchar *oid, int oidlen)
{
	memmove(buf, oid, oidlen);
	buf[oidlen] = 0;
	return buf;
}

/*
 * What one accepted client write may cover, in CHECKSUM BLOCKS.  It is
 * not §3.6's quantity: §3.6's stagemax bounds the grains a /repl fid
 * holds reserved, and a client write reserves none — it stages a key
 * and commits from the Req's buffer.  This server's own policy shares
 * the configured value because the two bound the same appetite for one
 * operation, and store.md §14(37) records that.  The process-wide
 * stagetot bounds reservations and is the engine's alone; what this
 * same number bounds on a /repl fid is that fid's staged grains,
 * which is §3.6's own quantity.  The engine reads a zero as "the
 * default" and so does this.
 */
static ulong
stagemaxof(Srvctx *c)
{
	ulong n;

	n = c->cfg.store.stagemax;
	if(n == 0)
		n = Stagemaxdflt;
	return n;
}

static vlong
stagemsof(Srvctx *c)
{
	vlong ms;

	ms = c->cfg.store.stagems;
	if(ms == 0)
		ms = Stagemsdflt;
	return ms;
}

/*
 * The grains a write of n bytes at off covers: the checksum blocks it
 * touches, which is what a stage reserves (store.md §3.1 step 2).
 */
static uvlong
stagegrains(Srvctx *c, uvlong off, long n)
{
	ulong blksz;

	if(n <= 0)
		return 0;
	blksz = objblksz(c);
	return (off + n - 1)/blksz - off/blksz + 1;
}

/*
 * The staged operations, on a list of the server's own.  It is not the
 * fid registry: the idle sweep walks this without taking a fid's state
 * lock or the registry lock, so it is never behind an attach or a
 * clunk.  stagelk is a LEAF — nothing is taken under it — and the one
 * order that exists is a fid's state lock over it, which is the order
 * the three hooks take.
 */
static void
stagelink(Srvctx *c, Sstage *s)
{
	qlock(&c->stagelk);
	s->linked = 1;
	s->prev = nil;
	s->next = c->stages;
	if(c->stages != nil)
		c->stages->prev = s;
	c->stages = s;
	c->nstage++;
	qunlock(&c->stagelk);
}

/* under stagelk */
static void
stageunlink(Srvctx *c, Sstage *s)
{
	if(!s->linked)
		return;
	s->linked = 0;
	if(s->prev != nil)
		s->prev->next = s->next;
	else
		c->stages = s->next;
	if(s->next != nil)
		s->next->prev = s->prev;
	s->prev = s->next = nil;
	c->nstage--;
}

/*
 * Release what a stage holds; the memory of the handle itself is the
 * owner's and goes elsewhere.  A client stage holds only the key it
 * chose — the bytes of a write are the Req's throughout (dat.h) — so
 * what there is to release is the engine handle of the op=full stage
 * of the replication surface, whose discard is an engine call and so
 * MUST happen while the store is still open (dat.h's auxclose,
 * store.md §9).
 *
 * Idempotent, because the sweep strips a stage the clunk behind it
 * still discards (§3.6), and counted once: `openat' is how many
 * discards found the store still there, which every one of them must.
 */
static int
stagestrip(Srvctx *c, Sstage *s, Stage **gp)		/* under stagelk */
{
	*gp = nil;
	stageunlink(c, s);
	if(s->released)
		return 0;
	s->released = 1;
	*gp = s->g;
	s->g = nil;
	c->nstagedone++;
	if(c->store == nil || c->closed)
		return 0;
	c->nstageopen++;
	return 1;
}

/*
 * Put a handle back where the strip found it, which is what the flush
 * hook does when the park below will not take it.  The hook may make no
 * engine call, so a handle it cannot park has to stay somewhere the
 * clunk and the shutdown still reach, and the fid's own slot is that
 * place: auxclose goes through it whatever the sweep has done.  The
 * strip's accounting goes back with it, so the discard is counted where
 * it is finally made and not twice.
 *
 * The stage stays OFF the list.  It is the fid's from here — `dead' is
 * set, so no handler will look at it again and the sweep has nothing
 * further to do with it — and re-linking it would only offer the sweep
 * a second strip of the same handle.
 *
 * Under stagelk.  `open' is what the strip answered: it says whether
 * the strip counted the discard it owed.
 */
static void
stageunstrip(Srvctx *c, Sstage *s, Stage *g, int open)
{
	s->released = 0;
	s->g = g;
	c->nstagedone--;
	if(open)
		c->nstageopen--;
}

static void
stagerelease(Srvctx *c, Sstage *s)
{
	Stage *g;
	int open;

	qlock(&c->stagelk);
	open = stagestrip(c, s, &g);
	qunlock(&c->stagelk);
	if(g != nil && open)
		stagediscard(g);
}

static void
stagefree(Srvctx *c, Sstage *s)
{
	stagerelease(c, s);
	free(s);
}

/*
 * Where the flush-side discard of an ENGINE handle waits for a place
 * it may be made.  stageflushhook runs under the fid's state lock and,
 * for a request flushed while it was still queued, on the service loop
 * with that request's Qreq.lk held (queue.c's srvqflush) — where dat.h
 * forbids it to block on anything a queue proc needs.  stagediscard
 * takes the engine's state lock, which a queue proc holds across its
 * work, so the hook takes the handle out of the stage and parks it
 * here instead; the drain below is where the call is made, with no fid
 * lock, no request lock and the store still open.
 *
 * Under stagelk.  A push that cannot grow the array answers 0, and
 * what its caller does with the handle then is the caller's: the hook
 * puts it back in the stage it came from (stageunstrip), because the
 * one thing it may not do is make the call itself.  `pendfull' is the
 * T1 knob that drives that path (srv.h), a failing realloc being
 * nothing a test can arrange.
 */
static int
stagepend(Srvctx *c, Stage *g)
{
	Stage **p;

	if(c->pendfull)
		return 0;
	if(c->npend >= c->apend){
		if((p = realloc(c->pend, (c->apend+8)*sizeof *p)) == nil)
			return 0;
		c->pend = p;
		c->apend += 8;
	}
	c->pend[c->npend++] = g;
	c->nstagepend++;
	return 1;
}

/*
 * Make the engine calls the flush hook could not.  It runs at the head
 * of every queued object operation, beside the sweep, and once more
 * from the shutdown while the store is still open — which is the last
 * chance, since store.md §9 allows no such call after the close and
 * what is left then can only be freed with the process.
 *
 * The array is taken whole under stagelk, so the handles are this
 * proc's from that moment and nothing of the context is held across
 * the calls.
 */
void
srvstagedrain(Srvctx *c)
{
	Stage **p;
	int i, n, open;

	qlock(&c->stagelk);
	p = c->pend;
	n = c->npend;
	c->pend = nil;
	c->npend = 0;
	c->apend = 0;
	open = c->store != nil && !c->closed;
	qunlock(&c->stagelk);
	if(open)
		for(i = 0; i < n; i++)
			stagediscard(p[i]);
	free(p);
}

/*
 * The three hooks dat.h declares, filled on a fid the moment it takes
 * a stage.  They run under the fid's state lock, which is what keeps
 * step 7 between two steps of a handler working on the same fid rather
 * than inside one.
 *
 *	auxflush  layer-a §5.4.1 step 7's discard half.  The stage is the
 *		  FID's and not the flushed request's — 9P allows two
 *		  requests on one fid and a stage can span several Twrites
 *		  (queue.c) — so what is discarded is whatever the fid
 *		  holds, whichever of its requests was flushed.  A handler
 *		  whose stage goes this way finds it gone at its next look
 *		  and answers rather than commits.
 *	auxclose  the clunk, and the shutdown's sweep before the store
 *		  closes.  An engine stage MUST be released here.
 *	auxfree	  the handle's memory, which may go after the store has
 *		  (D16).
 */
static void
stageflushhook(Sfid *f, Req *r)
{
	Srvctx *c;
	Sstage *s;
	Stage *g;
	int open;

	USED(r);
	if((s = f->aux) == nil)
		return;
	c = s->ctx;
	qlock(&c->stagelk);
	s->dead = 1;
	/*
	 * A handler inside an engine call THROUGH this handle is the one
	 * case where the handle may not be taken here (store.md §14(44)):
	 * §5.5's chunk passes the Stage* to stagewrite, and a discard made
	 * while that call is in flight is a discard made under it.  `dead'
	 * is enough — the look that handler takes when its call returns
	 * finds it and releases the handle itself (srvstagelive below),
	 * which is an engine call made from a queue proc holding no lock,
	 * where it belongs.  A client stage never reaches this: it holds a
	 * key and no handle, so `g' is nil for every one of them, and so is
	 * an opening chunk's, which is why the slot is what the rule is on
	 * elsewhere and the handle is what it is on here.
	 */
	if(s->busy && s->g != nil){
		qunlock(&c->stagelk);
		return;
	}
	open = stagestrip(c, s, &g);
	if(g != nil && open && !stagepend(c, g))
		stageunstrip(c, s, g, open);
	qunlock(&c->stagelk);
}

static void
stageclosehook(void *a)
{
	Sstage *s;

	if((s = a) == nil)
		return;
	s->dead = 1;
	stagerelease(s->ctx, s);
}

static void
stagefreehook(void *a)
{
	Sstage *s;

	if((s = a) == nil)
		return;
	stagerelease(s->ctx, s);
	free(s);
}

/*
 * §3.6's idle sweep, over both kinds of stage: the engine's own
 * handles, which stagesweep strips, and this file's, which are
 * released whole because a key is all they hold.  The trigger is
 * ARRIVAL — a stage a handler is inside right now is not an absence of
 * arrivals, and sweeping under one would take the reservations out
 * from under the transfer that is filling them (§3.6).
 *
 * There is no sweeper proc.  A stage holds a reservation, and the only
 * thing that can be waiting on one is another operation on an object,
 * so the sweep runs at the head of every queued operation that names
 * an object — this file's handlers, and the walk and the stat in
 * tree.c — a store with nothing running having nothing waiting for
 * what an abandoned stage holds.  The stage itself is never freed
 * here: it is the fid's, and the clunk behind it is what frees it
 * (§3.6), so the fid's next look finds it expired.
 *
 * Which is exactly why an expired stage is stripped WHERE IT IS FOUND,
 * under stagelk, one at a time, rather than chained onto a list the
 * sweep walks once the lock is down.  The sweep holds no reference to
 * a stage and does not own it: in the gap between the lock and such a
 * walk, a Tclunk of the staging fid runs the free hook on the service
 * loop and the memory is gone.  Nothing of `s' is read after the
 * unlock below — the pointer is tested against nil and no more — and
 * the one thing the strip hands back is the engine handle, which is
 * the context's the moment it leaves the stage and whose discard is an
 * engine call and so may not be made under a leaf lock.
 */
void
srvstagesweep(Srvctx *c)
{
	Sstage *s;
	Stage *g;
	vlong now, ms;
	int open;

	srvstagedrain(c);
	if(c->store != nil && !c->closed)
		stagesweep(c->store, nsec());
	ms = stagemsof(c);
	for(;;){
		now = nsec();
		g = nil;
		open = 0;
		qlock(&c->stagelk);
		for(s = c->stages; s != nil; s = s->next)
			if(!s->busy && now - s->last > ms*1000000LL)
				break;
		if(s != nil){
			s->dead = 1;
			open = stagestrip(c, s, &g);
		}
		qunlock(&c->stagelk);
		if(s == nil)
			break;
		if(g != nil && open)
			stagediscard(g);
	}
}

/*
 * §5.4 step 3: stage the update on the fid.  The slot is single — a
 * fid stages one operation at a time — and a fid that already holds
 * one is refused `disk full', the pick §2.6's set offers for it
 * (store.md §14(42)).  So is an update covering more grains than
 * §3.6's bound allows, which no client path can reach: a client write
 * is SHORTENED to the bound before it gets here (wclamp, store.md
 * §14(37)), and the opening chunk of a /repl transfer, which cannot
 * be shortened, is the one that does reach it — every later chunk is
 * charged against the handle by the engine instead (dat.h's Sstage).
 *
 * The bytes of a write are NOT copied here: they stay the Req's, and
 * the commit reads them from it.  A client stage lives inside its one
 * request — it is created at §5.4 step 3 and given back at step 6 or
 * step 7 — so the Req's buffer outlives it, while a stage that step 7
 * discarded under a commit would otherwise take the commit's argument
 * with it.  `n' is still wanted here, for the grains the update covers
 * against §3.6's bound.  The key is chosen here rather than at the
 * commit because §5.4 step 3 computes it before the replication round,
 * and spanning that round is what the stage is for.
 */
static Sstage*
stagenew(Req *r, Srvctx *c, Sfid *f, int kind, uchar *oid, int oidlen,
	uvlong ver, uvlong wepoch, long n, uvlong off, char **err)
{
	Sstage *s, *old;
	int inuse;

	*err = nil;
	if(stagegrains(c, off, n) > stagemaxof(c)){
		*err = Ediskfull;
		return nil;
	}
	if((s = mallocz(sizeof *s, 1)) == nil){
		*err = Eoom;
		return nil;
	}
	s->ctx = c;
	s->kind = kind;
	memmove(s->oid, oid, oidlen);
	s->oidlen = oidlen;
	s->ver = ver;
	s->wepoch = wepoch;
	s->off = off;
	s->last = nsec();
	s->busy = 1;
	/*
	 * A stage the sweep expired, or one step 7 discarded, is still in
	 * the slot: §3.6 has neither free the handle, because the handle
	 * is the fid's and the fid's owner is what knows the transfer is
	 * over.  It holds nothing any more, so it does not stand in the
	 * way of a new one — the accessor that must be told is the one
	 * CONTINUING a staged transfer, which answers `stage expired'
	 * (§3.6), and a client operation continues nothing.  A stage that
	 * is still live does stand in the way: one to a fid.
	 *
	 * So does a stage another queue proc is inside a STEP on, dead or
	 * not (store.md §14(44)): that step ends in a look of its own, and
	 * this is the only place the SLOT is taken from under such a stage
	 * — which would leave that look nothing to find.  `busy' alone
	 * says so, handle or not: the opening chunk of a transfer is busy
	 * from here until the arm that fills `g', with stageopen running in
	 * between under no lock, and a stage freed in that window is armed
	 * and looked at after it has gone.
	 *
	 * One arrival reaches that clause, and it is a race rather than a
	 * sequence: a chunk finds the slot EMPTY in srvstagemore and comes
	 * here for a stage of its own, and a chunk naming another object —
	 * another queue, so the two run together — fills the slot in
	 * between.  A live stage is refused by the clause above it; this
	 * one decides when a step 7 for the fid killed the winner's stage
	 * in the same gap, which is the window where the loser would
	 * otherwise free a stage its owner is still inside.  What cannot
	 * reach it is the sequence it reads like: while the slot holds
	 * anything, every chunk on the fid goes through srvstagemore, which
	 * answers one whose stage is dead and busy — leaving the slot where
	 * it is (§14(43)) — without ever asking for a new stage.
	 *
	 * §13's point over exactly that race (srv.h's newhold), before the
	 * lock so that the winner can take it and fill the slot while the
	 * loser waits.  It parks the first arrivals only, for the same
	 * reason: a point that parked the winner too would leave nobody to
	 * be raced against.
	 */
	srvqholdfirst(r, &c->newhold, &c->newheld);
	qlock(&f->lk);
	old = f->aux;
	if(old != nil && f->auxflush != stageflushhook){
		/*
		 * The slot holds state that is not a stage.  Nothing on the
		 * served surface can reach here — the rows that stage are
		 * this file's, and a create, whose fid's state is the
		 * enumeration's, stages nothing; the one way an enumeration's
		 * snapshot could land on a fid of an object row is a Topen
		 * and a Tcreate pipelined on one fid, and the two cells
		 * refuse the second (dat.h).  So this is the T1 fid-state
		 * point (srv.h), which fills every fid it makes.  It is not a
		 * §2.6 condition and does not pretend to be one.
		 */
		qunlock(&f->lk);
		free(s);
		*err = Efidstate;
		return nil;
	}
	inuse = 0;
	if(old != nil){
		qlock(&c->stagelk);
		inuse = (!old->dead && !old->released) || old->busy;
		qunlock(&c->stagelk);
	}
	if(inuse){
		qunlock(&f->lk);
		free(s);
		*err = Ediskfull;
		return nil;
	}
	f->aux = s;
	f->auxflush = stageflushhook;
	f->auxclose = stageclosehook;
	f->auxfree = stagefreehook;
	f->auxclosed = 0;
	qunlock(&f->lk);
	if(old != nil)
		stagefree(c, old);
	stagelink(c, s);
	return s;
}

/*
 * Take the fid's stage out of the slot, if it is still the one this
 * handler staged.  It may not be: step 7 may have discarded it from
 * the service loop or from another queue proc while this handler was
 * in an engine call, which is exactly what a handler has to tolerate
 * (dat.h).
 */
static Sstage*
stagetake(Sfid *f, Sstage *s)
{
	Sstage *t;

	qlock(&f->lk);
	t = f->aux;
	if(t != nil && t == s){
		f->aux = nil;
		f->auxflush = nil;
		f->auxclose = nil;
		f->auxfree = nil;
		f->auxclosed = 0;
	}else
		t = nil;
	qunlock(&f->lk);
	return t;
}

/*
 * Is the fid's stage still this handler's, and still live?  The look a
 * handler takes before the step that must not happen once step 7 has
 * run for this fid.
 *
 * `busy' stays SET across it.  It says a handler is inside a step on
 * this stage, and a handler between two steps of one operation is not
 * an absence of arrivals (store.md §3.6): a sweep landing in a window
 * this look cleared would expire the stage the look was about to call
 * live.  The two fields the sweep writes, `dead' and `released', are
 * read under stagelk, which is where it writes them, and the slot
 * under the fid's state lock, which is where the hooks write it — in
 * that order, the only order the two locks are ever taken in (dat.h).
 *
 * The point is held BEFORE either lock, holding none of its own, and
 * `busy' rather than a lock is what the sweep is kept off by.  A park
 * under the fid's state lock wedges the server: a Tflush of another
 * request QUEUED on this same fid performs step 7 on the service loop,
 * which takes that lock (queue.c's srvstep7), and the loop is both what
 * clears the point and what the shutdown runs on.  A park under stagelk
 * stalls every other queue besides, since the sweep at the head of every
 * queued object operation takes it.
 */
static int
stagelive(Srvctx *c, Sfid *f, Sstage *s, Req *r)
{
	int ok;

	srvqhold(r, &c->lookhold, nil);
	qlock(&f->lk);
	qlock(&c->stagelk);
	ok = f->aux == s && !s->dead && !s->released;
	s->last = nsec();
	qunlock(&c->stagelk);
	qunlock(&f->lk);
	return ok;
}

/*
 * The stage point's stage, once it is made.  It takes an engine handle
 * of its own, because §5.5's op=full stage is the one that holds one
 * and this point is what has that shape before that surface exists —
 * so the rules around a handle, the flush-side discard's included, are
 * driven on a real one.  And it stops being a handler's step: from
 * here the sweep may have it (§3.6).  Both under the lock the sweep
 * reads them under.
 *
 * The stage may be gone by now.  The handle is taken from the engine
 * with no lock held, because that call blocks, and a step 7 for another
 * request on this same fid strips the stage in that window — finding
 * `g' still nil, so it releases nothing.  A handle stored into a
 * stripped stage is one nothing ever reaches, every later strip
 * returning early, so it goes to the drain instead: the handle is the
 * context's from the moment it has no stage, exactly as it is when the
 * flush hook parks one (dat.h's Sstage).
 */
static int
stagearm(Srvctx *c, Sstage *s, Stage *g, int keepbusy)
{
	int open, live;

	qlock(&c->stagelk);
	open = c->store != nil && !c->closed;
	live = !s->dead && !s->released;
	if(g != nil && !live){
		if(open && stagepend(c, g))
			g = nil;
	}else{
		s->g = g;
		g = nil;
	}
	if(!keepbusy)
		s->busy = 0;
	qunlock(&c->stagelk);
	if(g != nil && open)
		stagediscard(g);
	return live;
}

static void
stagepointarm(Srvctx *c, Sstage *s, Stage *g)
{
	stagearm(c, s, g, 0);
}

static void
stagedone(Sfid *f, Sstage *s)
{
	Sstage *t;

	if((t = stagetake(f, s)) != nil)
		stagefree(t->ctx, t);
}

/*
 * The T1 stage point, in the shape srv.h's other points take.  With it
 * on, an open of /obj/<oid> for writing leaves a stage on the fid — a
 * stage that stages nothing — so that the rules around the state can
 * be driven on a fid that holds one while no request is in flight: the
 * clunk's discard, the shutdown sweep's discard while the store is
 * still open, the idle sweep, and the `disk full' a second stage on
 * one fid is refused with.
 *
 * It exists because no CLIENT operation leaves a stage behind: a
 * client write stages at §5.4 step 3 and gives the stage back at step
 * 6 or 7, inside the one request.  The stage that outlives its request
 * is the op=full stage of the replication surface (§3.6, §5.5), and
 * the point is what drives the lifetime rules with no transfer in
 * flight and no engine handle to carry.  The point is per context, like
 * srvauxpoint and unlike the cell point, because what it fills is a
 * fid rather than a row of the file table.
 */
void
srvstagepoint(Srvctx *c, int on)
{
	qlock(&c->stagelk);
	c->stagept = on;
	qunlock(&c->stagelk);
}

/*
 * The other T1 knob on this state: with it on, the park refuses every
 * handle, which is the path a failing realloc would take and which
 * nothing else in a test can arrange (srv.h).
 */
void
srvstagependfull(Srvctx *c, int on)
{
	qlock(&c->stagelk);
	c->pendfull = on;
	qunlock(&c->stagelk);
}

static int
stagepointon(Srvctx *c)
{
	int n;

	qlock(&c->stagelk);
	n = c->stagept;
	qunlock(&c->stagelk);
	return n;
}

/*
 * How many engine handles have been parked for the drain: every one
 * the flush-side discard has met and could park, since it makes no
 * engine call of its own, and every one the stage point armed onto a
 * stage that had already been stripped.
 */
uvlong
srvstagepend(Srvctx *c)
{
	uvlong n;

	qlock(&c->stagelk);
	n = c->nstagepend;
	qunlock(&c->stagelk);
	return n;
}

/*
 * How many parked handles are still waiting for a drain.  It is the
 * depth of the list srvstagepend counts the arrivals at, and what says
 * the shutdown made the calls the flush hook could not: 0 once
 * srvshutdown's own drain has run, which is the last moment store.md
 * §9 allows one.
 */
uvlong
srvstagewaiting(Srvctx *c)
{
	uvlong n;

	qlock(&c->stagelk);
	n = c->npend;
	qunlock(&c->stagelk);
	return n;
}

/*
 * How many stages the live fids hold, how many have been given back,
 * and how many of those found the store still open — which every one
 * of them must, since an engine stage's discard is an engine call
 * (dat.h).  nil for a count the caller does not want.
 */
void
srvstagecount(Srvctx *c, uvlong *live, uvlong *done, uvlong *openat)
{
	qlock(&c->stagelk);
	if(live != nil)
		*live = c->nstage;
	if(done != nil)
		*done = c->nstagedone;
	if(openat != nil)
		*openat = c->nstageopen;
	qunlock(&c->stagelk);
}

/*
 * §5.5's op=full, the one stage whose lifetime is longer than the
 * request that made it (§3.6).  Everything about the slot, the list
 * and the three hooks is what a client stage uses; what differs is
 * that `g' is filled, so the four calls below are where the engine
 * handle is taken, carried across the chunks and given up.  The
 * channel that drives them is peer.c's.
 *
 * The first chunk of a transfer.  The handle is taken with no lock
 * held, because stageopen allocates, so a step 7 for another request
 * on this fid can strip the stage in that window — which the arm has
 * to find rather than store a handle nothing would reach (the stage
 * point above takes the same care for the same reason).  `busy' is
 * kept SET across the arm and stays set until the caller's look:
 * a handler between two steps of one chunk is not an absence of
 * arrivals (§3.6).  It is set from the stage below, so the whole of
 * that window is a stage nobody else may take the slot from — the
 * stage is in the slot and the handle is not in it yet, and a stage
 * freed here is one this function arms and the caller looks at after
 * it has gone.
 *
 * §13's point over exactly that window (srv.h's openhold) is held
 * with no lock of the fid's or the stage list's, the stage call having
 * returned, so the service loop is free to perform step 7 on this fid
 * across it.
 */
Stage*
srvstagefull(Req *r, Srvctx *c, Sfid *f, uchar *oid, int oidlen, uvlong flen,
	int force, uvlong ver, uvlong wepoch, uvlong off, long n, Sstage **sp,
	char *buf, int nbuf, char **err)
{
	Sstage *s;
	Stage *g;

	*sp = nil;
	if((s = stagenew(r, c, f, Stfull, oid, oidlen, ver, wepoch, n,
		off, err)) == nil)
		return nil;
	qlock(&c->stagelk);
	s->flen = flen;
	s->force = force;
	qunlock(&c->stagelk);
	srvqhold(r, &c->openhold, &c->openheld);
	if((g = stageopen(c->store, oid, oidlen, flen, force)) == nil){
		*err = srverr(buf, nbuf);
		stagedone(f, s);
		return nil;
	}
	if(!stagearm(c, s, g, 1)){
		stagedone(f, s);
		*err = Estageexp;
		return nil;
	}
	*sp = s;
	return g;
}

/*
 * A later chunk of a transfer this fid is already staging.  Answers
 * the handle to write through, or nil — with *err nil when the fid
 * holds no stage at all, which is the first chunk's case and the
 * caller's to take to srvstagefull.
 *
 * The refusals are the per-fid ones.  A chunk naming another object is
 * a SECOND stage on one fid and is `disk full' — a pick from layer-a
 * §2.6's set rather than a refusal §3.6 defines, because what §3.6
 * bounds is the SPACE a fid holds staged and not how many transfers
 * may be in flight on one (store.md §14(42)).  §3.6's grain bound is
 * not counted here either: the engine charges `stagemax' against the
 * handle, exactly and grain by grain, and one handle is all a fid may
 * hold, so a count on this side would only be a coarser one over the
 * same reservations.  A `len' or `force' that differs from the
 * transfer's own is a header §5.5 forbids a conforming sender to send,
 * so it is `bad ctl'.
 *
 * A stage the idle sweep expired, or step 7 discarded, is refused
 * `stage expired' — and the refusal is what takes it out of the slot.
 * §3.6 has the owner discard an expired stage and start the transfer
 * over, which is free; leaving the handle in the slot would refuse
 * that restart with the same string for as long as the fid lived
 * (store.md §14(43)).
 *
 * With one exemption, which store.md §14(43) grants and §14(44) is
 * the flush hook's half of: a dead stage another queue proc is inside
 * a STEP on is left where it is.
 * This runs on a chunk naming a SECOND object, which hashes to another
 * queue and so runs beside a chunk that is inside stageopen or
 * stagewrite on this stage — and taking it here would free the handle
 * under the write, or free the stage itself under the arm that is
 * about to store the handle in it.  The refusal is answered all the
 * same; what §14(43) asks of it, that the dead stage leave the slot
 * before the next transfer, is done a moment later by that chunk
 * itself: its look when the write returns (srvstagelive below), the
 * give-back that look leaves to when the chunk carries final=1
 * (srvstagefinal below), or, for an opening chunk, the give-back
 * behind an arm that finds the stage already gone (srvstagefull
 * above).
 */
Stage*
srvstagemore(Srvctx *c, Sfid *f, uchar *oid, int oidlen, uvlong flen,
	int force, Sstage **sp, char **err)
{
	Sstage *s;
	Stage *g;
	int dead, inside;

	*err = nil;
	*sp = nil;
	g = nil;
	dead = 0;
	inside = 0;
	qlock(&f->lk);
	if((s = f->aux) == nil){
		qunlock(&f->lk);
		return nil;
	}
	if(f->auxflush != stageflushhook){
		qunlock(&f->lk);
		*err = Efidstate;		/* the T1 fid-state point */
		return nil;
	}
	qlock(&c->stagelk);
	if(s->dead || s->released){
		dead = 1;
		inside = s->busy;
	}else if(s->kind != Stfull || s->oidlen != oidlen
	|| memcmp(s->oid, oid, oidlen) != 0)
		*err = Ediskfull;
	else if(s->flen != flen || s->force != force)
		*err = Ebadctl;
	else{
		s->busy = 1;
		s->last = nsec();
		g = s->g;
		*sp = s;
	}
	qunlock(&c->stagelk);
	qunlock(&f->lk);
	if(dead){
		if(!inside)
			stagedone(f, s);
		*err = Estageexp;
	}
	return g;
}

/*
 * The look a chunk takes when its engine call returns: is the stage
 * still this fid's, and still live?  It clears `busy', which is what
 * held the sweep off across the call, and a stage that went while the
 * call was in flight is given back here — the flush hook leaves the
 * handle of a busy stage alone precisely so that this is where it is
 * released, outside every lock and on a queue proc.
 *
 * `keepbusy' is for the caller whose look is NOT the end of its step:
 * a final=1 chunk goes on from here to the arbitration and the commit,
 * still holding this Sstage*, and a look that cleared the mark would
 * leave the sweep, a refusal on another queue and the flush hook free
 * to take the slot and free the stage under it (store.md §14(44)).
 * Such a caller owes the clear to the give-back it is on its way to,
 * which is srvstagefinal below.  A look that answers 0 clears the mark
 * and gives the stage back here whatever the caller asked for, the
 * transfer being over: `keepbusy' asks to hold a stage this look has
 * just found gone, and there is nothing left to hold it for.
 */
int
srvstagelive(Srvctx *c, Sfid *f, Sstage *s, int keepbusy)
{
	int ok;

	qlock(&f->lk);
	qlock(&c->stagelk);
	ok = f->aux == s && !s->dead && !s->released;
	if(!ok || !keepbusy)
		s->busy = 0;
	s->last = nsec();
	qunlock(&c->stagelk);
	qunlock(&f->lk);
	if(!ok)
		stagedone(f, s);
	return ok;
}

/*
 * final=1: the fid forgets the handle BEFORE the outcome is known,
 * because stagefinal consumes it on every one of them (§3.6) — a
 * Tclunk behind a refused final=1 would otherwise discard a stage that
 * has already been discarded.  The handle answered here is the
 * caller's to pass to stagefinalcsum, or nil for a stage that was
 * already stripped, whose transfer is over either way.
 *
 * This is also where the final chunk's `busy' is cleared.  Its look
 * kept the mark set (srvstagelive above) so that nothing could take
 * the slot between the two, and what ends that step is the give-back
 * here rather than the look.
 */
Stage*
srvstagefinal(Srvctx *c, Sfid *f, Sstage *s)
{
	Stage *g;

	qlock(&c->stagelk);
	g = s->g;
	s->g = nil;
	s->busy = 0;
	s->dead = 1;
	if(!s->released){
		s->released = 1;
		c->nstagedone++;
		if(c->store != nil && !c->closed)
			c->nstageopen++;
	}
	stageunlink(c, s);
	qunlock(&c->stagelk);
	if(stagetake(f, s) != nil)
		free(s);
	return g;
}

/*
 * layer-a §5.4 step 1's admission, less the currency check this build
 * cannot make (store.md §14(35)).  §5.1 and §5.4 are about role=client
 * operations: an operator reading or writing a reserved `shoal.' id
 * (§2.1) is not one, and neither is a peer's read.
 *
 * The detail after `not primary' is §2.6's own form, and the iid in it
 * is the instance the map sends the client to.
 *
 * The map is the caller's snapshot and not one taken here: the
 * admission, the epoch the stage is keyed with and step 5's placement
 * are three reads of ONE map, and the handler is what holds it across
 * all three (dat.h).
 */
static char*
admit(Srvctx *c, Smap *m, Sfid *f, uchar *oid, int oidlen, char *buf, int nbuf)
{
	char id[Oidmax+1];
	Cinst *p;

	USED(c);
	if(f->role != Rclient)
		return nil;
	oidstr(id, oid, oidlen);
	p = mapprimary(m->map, id);
	if(p == m->self)
		return nil;
	if(p == nil){
		/*
		 * No member of P(o) is up, which §4.3 makes the object's
		 * `object unavailable' at this epoch — unless placement
		 * could not be computed at all, which is never a placement
		 * answer (lib/shoal.h) and is answered as the internal
		 * error it is.
		 */
		if(mapunderrep(m->map, id) < 0)
			return srverr(buf, nbuf);
		return Eunavail;
	}
	snprint(buf, nbuf, "%s: %s", Enotprimary, p->iid);
	return buf;
}

/*
 * §5.4 steps 4 and 5, as far as a build with no peer client can take
 * them (store.md §14(34)).  M is every member of P(o) other than this
 * instance that has not durably committed the update, which — nothing
 * having been sent to anybody — is every one of them.
 *
 * So a placement of one member is step 5's empty M and the operation
 * commits; a placement with any other member puts k at 1, and whether
 * or not mincopies is met the primary owes step 5a a durable stale
 * mark at the monitor before it may proceed.  There is no monitor
 * client, so that round trip cannot be made and step 5a sends the
 * operation to step 7 — `degraded'.
 *
 * role=client alone, for admit's reason: §5.4 is the client write
 * path, and an operator's write of a reserved id is not on it.
 */
static char*
replicate(Srvctx *c, Smap *m, Sfid *f, uchar *oid, int oidlen, char *buf,
	int nbuf)
{
	char id[Oidmax+1];
	Cinst *p[Maxplace];
	int n;

	USED(c);
	if(f->role != Rclient)
		return nil;
	oidstr(id, oid, oidlen);
	if((n = mapplace(m->map, id, p, nelem(p))) < 0)
		return srverr(buf, nbuf);
	if(n <= 1)
		return nil;
	return Edegraded;
}

/*
 * The current key, and the refusals §2.6 makes of the record itself: a
 * tombstone is `object deleted' for every path but a create, and an id
 * this store holds nothing for is the engine's `no such object'.
 */
static char*
objkey(Srvctx *c, Sfid *f, Objinfo *oi, char *buf, int nbuf)
{
	if(objstat(c->store, f->oid, f->oidlen, oi) < 0)
		return srverr(buf, nbuf);
	if(oi->state != Slive)
		return Edeleted;
	return nil;
}

/*
 * layer-a §2.4's read.  The clamp to `len', the zeroes a hole reads as
 * and the short read at the end are the engine's (store.md §4); what
 * is here is the queue, the gate having already run, and the one exit.
 *
 * Every queued object handler below has this shape: the pushed
 * function takes the map snapshot at the head and gives it back when
 * the body has returned, so the body reads one map from its admission
 * to its last exit — engine calls, -X hold points and all (dat.h).
 */
static void
objreadrun(Req *r, Srvctx *c, Smap *m)
{
	char buf[ERRMAX], *e;
	Sfid *f;
	long n;

	f = r->fid->aux;
	srvstagesweep(c);
	if((e = admit(c, m, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	n = objread(c->store, f->oid, f->oidlen, r->ofcall.data,
		r->ifcall.count, r->ifcall.offset);
	if(n < 0){
		srvqexit(r);
		srvrerror(r);
		return;
	}
	r->ofcall.count = n;
	srvqexit(r);
	srvqdone(r, nil);
}

static void
objreadq(Req *r)
{
	Srvctx *c;
	Smap *m;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	m = srvmapget(c);
	objreadrun(r, c, m);
	srvmapput(c, m);
}

void
srvobjread(Req *r)
{
	Srvctx *c;
	Sfid *f;

	c = r->srv->aux;
	f = r->fid->aux;
	srvqpush(c, f->oid, f->oidlen, r, objreadq);
}

/*
 * layer-a §2.4's write, on §5.4's path.  Three bounds are answered
 * before anything is staged:
 *
 *	`object too large'  a write past objmax at either bound (§2.6,
 *		store.md §3.7).  The engine answers it too; answering it
 *		here as well is what keeps a refused write from costing a
 *		stage and a key.
 *	the short write  §2.4 MAY answer an Rwrite count below the
 *		request's, and clients MUST loop.  What bounds one
 *		accepted write here is the per-fid staged-grain budget
 *		(§3.6's stagemax): a write covering more blocks than the
 *		budget is SHORTENED to it rather than refused, which is
 *		§2.4's short write doing the job §5.5 gives it on a build
 *		that has a peer message to bound it to.
 *	`disk full'  the same budget where shortening cannot help: a fid
 *		that already holds a stage stages nothing more (§3.6).
 *
 * A count of 0 is none of them: §3.6 makes it not an extend, so it
 * commits no record and changes no key — and the engine is what says
 * so, since a copy whose corrupt flag is set answers one
 * `checksum mismatch' like any other client access (store.md §3.7).
 */
static void
objwriterun(Req *r, Srvctx *c, Smap *m)
{
	char buf[ERRMAX], *e;
	Sfid *f;
	Sstage *s;
	Objinfo oi;
	uvlong off, max;
	long n;

	f = r->fid->aux;
	srvstagesweep(c);
	if((e = admit(c, m, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	off = r->ifcall.offset;
	n = r->ifcall.count;
	max = objmaxof(c);
	if(off > max || (uvlong)n > max - off){
		srvqdone(r, Etoobig);
		return;
	}
	n = wclamp(c, off, n);
	if((e = objkey(c, f, &oi, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	if((s = stagenew(r, c, f, Stwrite, f->oid, f->oidlen, oi.ver+1,
		m->map->epoch, n, off, &e)) == nil){
		srvqdone(r, e);
		return;
	}
	/*
	 * The window this handler owns a stage it has not yet looked at:
	 * from here to the look below, a step 7 for another request on this
	 * fid takes the stage out from under it, and the look is what turns
	 * that into an answer instead of a commit (srv.h's objprelook).
	 *
	 * The snapshot is held across the park, so a swap landing here
	 * moves neither the epoch this write was keyed with nor the
	 * placement step 5 is about to read (dat.h).
	 */
	srvqhold(r, &c->prelookhold, nil);
	if((e = replicate(c, m, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
		stagedone(f, s);
		srvqdone(r, e);
		return;
	}
	if(!stagelive(c, f, s, r)){
		stagedone(f, s);
		srvqdone(r, Estagegone);
		return;
	}
	srvqhold(r, &c->stagehold, nil);
	/*
	 * The bytes are the Req's and the key is the stage's.  lib9p keeps
	 * the Req's buffer until this handler responds, so a step 7 that
	 * lands between the look above and this call takes the stage and
	 * leaves the argument standing — layer-a §5.4.1's "MAY or MAY NOT
	 * have been applied" for a commit already in flight, rather than a
	 * commit of no bytes at all.
	 */
	if(objwrite(c->store, s->oid, s->oidlen, r->ifcall.data, n, s->off,
		s->ver, s->wepoch, nil, 0) < 0){
		stagedone(f, s);
		srvqexit(r);
		srvrerror(r);
		return;
	}
	stagedone(f, s);
	r->ofcall.count = n;
	srvqexit(r);
	srvqdone(r, nil);
}

static void
objwriteq(Req *r)
{
	Srvctx *c;
	Smap *m;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	m = srvmapget(c);
	objwriterun(r, c, m);
	srvmapput(c, m);
}

/*
 * What one accepted write may cover: the per-fid staged-grain budget,
 * in bytes from off.  A write within the budget is taken whole.
 */
static long
wclamp(Srvctx *c, uvlong off, long n)
{
	ulong blksz;
	uvlong end;

	if(n <= 0 || stagegrains(c, off, n) <= stagemaxof(c))
		return n;
	blksz = objblksz(c);
	end = (off/blksz + stagemaxof(c)) * (uvlong)blksz;
	return (long)(end - off);
}

void
srvobjwrite(Req *r)
{
	Srvctx *c;
	Sfid *f;

	c = r->srv->aux;
	f = r->fid->aux;
	srvqpush(c, f->oid, f->oidlen, r, objwriteq);
}

/*
 * layer-a §2.4's open.  The mode rules are the row's own, which is why
 * the row carries an open cell at all (dat.h): anything that is not
 * OREAD, OWRITE or ORDWR with or without OTRUNC is `bad open mode',
 * which is ORCLOSE — §2.4's own MUST, and a bit outside the mask like
 * any other — and OEXEC, which is not a mode §2.4 admits.  §2.4's other mode rule, a write on a fid opened
 * OREAD, is lib9p's to answer and never reaches here (store.md
 * §14(36)).
 *
 * OTRUNC is a truncate to zero and so is a write: it takes §5.4's path
 * entire, key and all, which is one reason the open is on the object's
 * queue like every other operation naming it.
 */
static void
objopenrun(Req *r, Srvctx *c, Smap *m)
{
	char buf[ERRMAX], *e;
	Sfid *f;
	Sstage *s;
	Objinfo oi;
	Qid q;

	f = r->fid->aux;
	srvstagesweep(c);
	if((e = admit(c, m, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	if((e = objkey(c, f, &oi, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	if((r->ifcall.mode & OTRUNC) != 0 && oi.len != 0){
		if((s = stagenew(r, c, f, Sttrunc, f->oid, f->oidlen, oi.ver+1,
			m->map->epoch, 0, 0, &e)) == nil){
			srvqdone(r, e);
			return;
		}
		if((e = replicate(c, m, f, f->oid, f->oidlen, buf,
			sizeof buf)) != nil){
			stagedone(f, s);
			srvqdone(r, e);
			return;
		}
		if(!stagelive(c, f, s, r)){
			stagedone(f, s);
			srvqdone(r, Estagegone);
			return;
		}
		if(objtrunc(c->store, s->oid, s->oidlen, 0, s->ver, s->wepoch,
			nil, 0) < 0){
			stagedone(f, s);
			srvqexit(r);
			srvrerror(r);
			return;
		}
		stagedone(f, s);
		if(objstat(c->store, f->oid, f->oidlen, &oi) < 0){
			srvqexit(r);
			srvrerror(r);
			return;
		}
	}
	srvobjqid(f, &oi, &q);
	f->qidpath = q.path;
	f->qidvers = q.vers;
	r->ofcall.qid = q;
	if(stagepointon(c) && (r->ifcall.mode&3) != OREAD)
		if((s = stagenew(r, c, f, Stpoint, f->oid, f->oidlen, 0, 0,
			0, 0, &e)) != nil){
			/*
			 * The window the stage exists in and the handle does
			 * not: the call below blocks, and a step 7 for another
			 * request on this fid strips the stage while it does
			 * (srv.h's objarm).
			 */
			srvqhold(r, &c->armhold, nil);
			stagepointarm(c, s, stageopen(c->store, f->oid,
				f->oidlen, 0, 0));
		}
	srvqdone(r, nil);
}

static void
objopenq(Req *r)
{
	Srvctx *c;
	Smap *m;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	m = srvmapget(c);
	objopenrun(r, c, m);
	srvmapput(c, m);
}

void
srvobjopen(Req *r)
{
	Srvctx *c;
	Sfid *f;
	int m;

	c = r->srv->aux;
	f = r->fid->aux;
	m = r->ifcall.mode;
	if((m & ~(3|OTRUNC)) != 0){
		respond(r, Ebadopen);
		return;
	}
	switch(m & 3){
	case OREAD:
	case OWRITE:
	case ORDWR:
		break;
	default:
		respond(r, Ebadopen);
		return;
	}
	srvqpush(c, f->oid, f->oidlen, r, objopenq);
}

/*
 * layer-a §2.4's remove: §1.5's delete, which sets state=tomb, len=0
 * and bumps the key like any other write.  Per 9P the fid is clunked
 * whether or not the remove succeeds, and lib9p has taken it out of
 * the fid pool before this runs, so nothing here arranges that.
 */
static void
objremoverun(Req *r, Srvctx *c, Smap *m)
{
	char buf[ERRMAX], *e;
	Sfid *f;
	Sstage *s;
	Objinfo oi;

	f = r->fid->aux;
	srvstagesweep(c);
	if((e = admit(c, m, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	if((e = objkey(c, f, &oi, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	if((s = stagenew(r, c, f, Stremove, f->oid, f->oidlen, oi.ver+1,
		m->map->epoch, 0, 0, &e)) == nil){
		srvqdone(r, e);
		return;
	}
	if((e = replicate(c, m, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
		stagedone(f, s);
		srvqdone(r, e);
		return;
	}
	if(!stagelive(c, f, s, r)){
		stagedone(f, s);
		srvqdone(r, Estagegone);
		return;
	}
	if(objremove(c->store, s->oid, s->oidlen, s->ver, s->wepoch,
		nil, 0) < 0){
		stagedone(f, s);
		srvqexit(r);
		srvrerror(r);
		return;
	}
	stagedone(f, s);
	srvqexit(r);
	srvqdone(r, nil);
}

static void
objremoveq(Req *r)
{
	Srvctx *c;
	Smap *m;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	m = srvmapget(c);
	objremoverun(r, c, m);
	srvmapput(c, m);
}

void
srvobjremove(Req *r)
{
	Srvctx *c;
	Sfid *f;

	c = r->srv->aux;
	f = r->fid->aux;
	srvqpush(c, f->oid, f->oidlen, r, objremoveq);
}

/*
 * layer-a §2.4's truncate and extend: a Twstat with `length' set.  Any
 * other settable field MUST be rejected, and a rename MUST be rejected
 * with `no rename' specifically.  Nothing §2.6 names fits the others,
 * so they carry this server's own string (§3.7, store.md §14(38)).
 * `type' and `dev' are refused with them.  stat(5) makes them
 * don't-touch on every wstat, so a request that carries a value is
 * asking for something this server will not do — a refusal, not a
 * field to ignore.  `qid' is the third of that set and is not tested
 * here: lib9p refuses a Twstat whose qid differs from the fid's own
 * before Srv.wstat is reached, with its own string, and a qid equal to
 * the fid's sets nothing (store.md §14(38)).
 *
 * convM2D leaves a field the client did not set at its null value
 * (nulldir), so what the client asked for is what differs from those.
 */
static void
objwstatrun(Req *r, Srvctx *c, Smap *m)
{
	char buf[ERRMAX], *e;
	Sfid *f;
	Sstage *s;
	Objinfo oi;
	uvlong len;

	f = r->fid->aux;
	srvstagesweep(c);
	if((e = admit(c, m, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	len = r->d.length;
	if(len > objmaxof(c)){
		srvqdone(r, Etoobig);
		return;
	}
	if((e = objkey(c, f, &oi, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	if(len == oi.len){
		srvqdone(r, nil);
		return;
	}
	if((s = stagenew(r, c, f, Sttrunc, f->oid, f->oidlen, oi.ver+1,
		m->map->epoch, 0, 0, &e)) == nil){
		srvqdone(r, e);
		return;
	}
	if((e = replicate(c, m, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
		stagedone(f, s);
		srvqdone(r, e);
		return;
	}
	if(!stagelive(c, f, s, r)){
		stagedone(f, s);
		srvqdone(r, Estagegone);
		return;
	}
	if(objtrunc(c->store, s->oid, s->oidlen, len, s->ver, s->wepoch,
		nil, 0) < 0){
		stagedone(f, s);
		srvqexit(r);
		srvrerror(r);
		return;
	}
	stagedone(f, s);
	srvqexit(r);
	srvqdone(r, nil);
}

static void
objwstatq(Req *r)
{
	Srvctx *c;
	Smap *m;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	m = srvmapget(c);
	objwstatrun(r, c, m);
	srvmapput(c, m);
}

void
srvobjwstat(Req *r)
{
	Srvctx *c;
	Sfid *f;
	Dir *d;

	c = r->srv->aux;
	f = r->fid->aux;
	d = &r->d;
	if(d->name != nil && d->name[0] != 0){
		respond(r, Enorename);
		return;
	}
	if(d->mode != (ulong)~0 || d->mtime != (ulong)~0
	|| d->atime != (ulong)~0
	|| (d->uid != nil && d->uid[0] != 0)
	|| (d->gid != nil && d->gid[0] != 0)
	|| (d->muid != nil && d->muid[0] != 0)
	|| d->type != (ushort)~0 || d->dev != (ulong)~0){
		respond(r, Ewstatfield);
		return;
	}
	/*
	 * A wstat that sets nothing at all is 9P's own sync of a fid: it
	 * names no field this row refuses and changes nothing.
	 */
	if(d->length == (vlong)~0){
		respond(r, nil);
		return;
	}
	srvqpush(c, f->oid, f->oidlen, r, objwstatq);
}

/*
 * Give the fid back to whatever asks for it next: a create that is not
 * going to move it after all (below).  The error the caller is
 * carrying survives, since one of the callers is the path that answers
 * from %r.
 */
static void
createdrop(Sfid *f)
{
	char err[ERRMAX];

	rerrstr(err, sizeof err);
	qlock(&f->lk);
	f->moving = 0;
	qunlock(&f->lk);
	errstr(err, sizeof err);
}

/*
 * layer-a §2.4's create: a Tcreate in /obj naming the oid.  The mode
 * rules are §2.4's own — DMDIR, DMAPPEND, DMEXCL and DMTMP are
 * `bad create mode', ORCLOSE is `bad open mode' — and §1.1's reserved
 * ids are `reserved name' for a client, while role=admin may create
 * them (§2.1, which the row's gate has already applied).
 *
 * The version is this instance's own to choose (§5.4 step 3): (E, 1)
 * for an id with no record, and (E, tombstone ver + 1) over a
 * tombstone, which is §1.5's rule that no older copy can outrank the
 * new object.  A create over a LIVE id is `object exists'.
 *
 * A create that SUCCEEDS turns the directory fid it was issued on into
 * the created object's, which is where this cell and the enumeration
 * meet: the fid's own state — an /obj directory fid's snapshot — is
 * given back with srvfidgive, after the last refusal and never before,
 * because a create that fails leaves the fid holding what it held
 * (dat.h).
 *
 * That is also why a create stages nothing on its fid, alone among
 * §5.4's four mutations.  The fid it runs on is a DIRECTORY fid until
 * the moment the create succeeds, and that fid's state is the
 * enumeration's snapshot; a stage in the same slot would either
 * displace it or be refused by it, on a fid 9P has not moved yet.  So
 * the key this instance chooses (§5.4 step 3) is carried to the one
 * engine call that publishes it, which is atomic — and there is no
 * window in which a stage of this operation exists for step 7 to
 * discard.  A flush lands before the call, where nothing is staged, or
 * after it, where the discard half is vacuous (store.md §14(10)).
 *
 * The claim is the other half of that meeting.  9P does not keep a
 * Tcreate and a Topen apart on one fid while the open is offloaded
 * (dat.h), so this cell refuses a fid that an open has answered for,
 * that holds a listing's state or that another create is moving, and
 * claims the fid for itself before its first engine call: the open
 * cell tests the claim under the same lock as it installs, so the two
 * cells cannot both win, and the claim is dropped again at whichever
 * exit this cell takes.  The three refusals are one test because none
 * of them alone covers the fid a SECOND open is part-way through: its
 * give-back has emptied the state slot and it has not installed its
 * own snapshot yet, so `Fid.omode' — which lib9p wrote when the first
 * open answered, and which is exactly the record that an open has won
 * on this fid — is what the create sees there (dat.h).
 */
static void
objcreaterun(Req *r, Srvctx *c, Smap *m)
{
	char buf[ERRMAX], err[ERRMAX], *e;
	Sfid *f;
	Sstage *s;
	Objinfo oi;
	Qreq *qr;
	uvlong ver;
	Qid q;

	f = r->fid->aux;
	qr = r->aux;
	srvqhold(r, &c->claimhold, nil);
	qlock(&f->lk);
	if(f->file != Qobj || r->fid->omode != -1 || f->moving ||
		srvobjdirheld(f)){
		qunlock(&f->lk);
		srvqdone(r, Ebotch);
		return;
	}
	f->moving = 1;
	qunlock(&f->lk);
	srvstagesweep(c);
	if((e = admit(c, m, f, qr->oid, qr->oidlen, buf, sizeof buf)) != nil){
		createdrop(f);
		srvqdone(r, e);
		return;
	}
	if(objstat(c->store, qr->oid, qr->oidlen, &oi) < 0){
		rerrstr(err, sizeof err);
		if(srv26(err) != Enoobj){
			createdrop(f);
			srvqdone(r, srverrs(buf, sizeof buf, err));
			return;
		}
		ver = 1;
	}else if(oi.state == Stomb)
		ver = oi.ver + 1;
	else{
		createdrop(f);
		srvqdone(r, Eexists);
		return;
	}
	if((e = replicate(c, m, f, qr->oid, qr->oidlen, buf,
		sizeof buf)) != nil){
		createdrop(f);
		srvqdone(r, e);
		return;
	}
	/*
	 * The create commits with the epoch of the map it was admitted
	 * under: one snapshot has answered the admission, step 5 and this
	 * stamp, and §14(10) is why there is no stage between them to
	 * carry it (dat.h).
	 */
	if(objcreate(c->store, qr->oid, qr->oidlen, ver, m->map->epoch, nil, 0,
		&oi) < 0){
		createdrop(f);
		srvqexit(r);
		srvrerror(r);
		return;
	}
	/*
	 * The create succeeded, so the fid moves: whatever it held as a
	 * directory fid goes back through its own hooks first (dat.h), and
	 * only then is the fid the object's.  The claim is dropped in the
	 * same hold of the lock that writes the fid's new identity, so
	 * nothing sees the fid half moved.
	 */
	srvfidgive(f);
	qlock(&f->lk);
	f->file = Qobjfile;
	memmove(f->oid, qr->oid, qr->oidlen);
	f->oidlen = qr->oidlen;
	srvobjqid(f, &oi, &q);
	f->qidpath = q.path;
	f->qidvers = q.vers;
	f->moving = 0;
	qunlock(&f->lk);
	r->ofcall.qid = q;
	srvqexit(r);
	srvqdone(r, nil);
}

static void
objcreateq(Req *r)
{
	Srvctx *c;
	Smap *m;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	m = srvmapget(c);
	objcreaterun(r, c, m);
	srvmapput(c, m);
}

/*
 * The /obj directory row's create cell.  The fid it runs on is a
 * directory fid, so the id it names is the request's and travels in
 * the Qreq; f->oid is written only by a create that succeeded.
 *
 * The gate ran before this (§2.1's operator rule, F3 and the fence),
 * with the created name as the id it asked about.  What is left is
 * §2.4's mode rules, §1.1's name rule and §1.1's reserved ids.
 */
void
srvobjcreate(Req *r)
{
	Srvctx *c;
	Sfid *f;
	uchar oid[Oidmax];
	ulong perm;
	int mode, n;

	c = r->srv->aux;
	f = r->fid->aux;
	perm = r->ifcall.perm;
	mode = r->ifcall.mode;
	if((perm & (DMDIR|DMAPPEND|DMEXCL|DMTMP)) != 0){
		respond(r, Ebadcreate);
		return;
	}
	if((mode & ~(3|OTRUNC)) != 0){
		respond(r, Ebadopen);
		return;
	}
	switch(mode & 3){
	case OREAD:
	case OWRITE:
	case ORDWR:
		break;
	default:
		respond(r, Ebadopen);
		return;
	}
	n = strlen(r->ifcall.name);
	if(!srvoidok((uchar*)r->ifcall.name, n)){
		respond(r, Ebadname);
		return;
	}
	memmove(oid, r->ifcall.name, n);
	if(n >= 6 && memcmp(oid, "shoal.", 6) == 0 && f->role != Radmin){
		respond(r, Ereserved);
		return;
	}
	srvqpush(c, oid, n, r, objcreateq);
}

/*
 * layer-a §2.4's /meta/<oid>: one attr=value line per object, composed
 * at open like every other render-at-open file (§2.2) and on the
 * object's queue, because it reads that object's record.
 *
 *	blksz=	the geometry's, which is the disk's (objblksz).
 *	cur=	the epoch of this instance's most recent COMPLETED
 *		currency check for the object, or 0.  With no peer client
 *		no check can be made at all, so this reads 0 for every
 *		object for as long as this build runs — which is the
 *		honest answer §2.4 provides for rather than a stale one.
 *		store.md §14(35) records it.
 *	ready=	the same fact said the other way round: an instance that
 *		has completed no currency check is not ready to serve the
 *		object under §5.1's clause (c), so this reads `no'.
 *	placement=, primary=
 *		computed from the map, which §2.4 marks advisory; they are
 *		the part of the line a static map answers in full.  Either
 *		may have nothing to name — a map in which no node places at
 *		all leaves P(o) empty, and one in which no member of P(o)
 *		is up has no serving primary (§4.3) — and both then read
 *		`-', because §0 makes this one attr=value record and an
 *		attribute with no value at all is not one.  A `-' is no
 *		iid: §3.3's node names carry no `.', so every iid has one
 *		(store.md §14(35)).
 *
 * The placement array is Maxplace long and mapplace answers at most
 * `replicas' members, which §3.2 refuses above Maxplace (lib/map.c),
 * so the whole of P(o) is rendered and nothing is dropped.
 */
char*
srvmetatext(Srvctx *c, Sfid *f, Text *t)
{
	char buf[ERRMAX], id[Oidmax+1], csum[Csumhexlen];
	Objinfo oi;
	Cinst *p[Maxplace], *pr;
	Smap *m;
	int i, n;

	if(objstat(c->store, f->oid, f->oidlen, &oi) < 0)
		return srverr(buf, sizeof buf);
	if(oi.state != Slive)
		return Edeleted;
	oidstr(id, f->oid, f->oidlen);
	csumfmt(csum, oi.csum);
	/*
	 * `placement=' and `primary=' are two answers of one map, so they
	 * come out of one snapshot held to the end of the line (dat.h):
	 * a primary that is in no rendered placement would be a record of
	 * a cluster that never existed.
	 */
	m = srvmapget(c);
	if((n = mapplace(m->map, id, p, nelem(p))) < 0){
		srvmapput(c, m);
		return srverr(buf, sizeof buf);
	}
	/*
	 * One physical line, which is what §2.4's "one attr=value line per
	 * object" asks for: the example there is wrapped typographically
	 * and the fields run on.
	 */
	textprint(t, "oid=%s len=%llud ver=%llud wepoch=%llud csum=%s",
		id, oi.len, oi.ver, oi.wepoch, csum);
	textprint(t, " state=live mtime=%lld blksz=%lud cur=0",
		oi.mtime, objblksz(c));
	textprint(t, " placement=");
	if(n == 0)
		textprint(t, "-");
	for(i = 0; i < n; i++)
		textprint(t, "%s%s", i > 0 ? "," : "", p[i]->iid);
	pr = mapprimary(m->map, id);
	textprint(t, " primary=%s ready=no\n", pr != nil ? pr->iid : "-");
	srvmapput(c, m);
	return nil;
}

/*
 * The render happens at the open, so the open is where §5.4 step 1's
 * admission is asked — in objreadq's order, the admission before the
 * record, so that an instance the client should not be asking answers
 * `not primary' rather than the object's metadata.
 *
 * A client read of /meta is a role=client read like any other: §2.1
 * exempts an ADMIN read from primaryship, handoff grace and currency
 * and no one else, §6.4 F1 names "every role=client read … through
 * /obj or /meta" in one breath, and the line this row renders is the
 * object's own record.  admit is role=client's alone (it answers nil
 * for every other role), so the operator's inspection path through
 * /meta is untouched.
 */
static void
metaopenrun(Req *r, Srvctx *c, Smap *m)
{
	char buf[ERRMAX], *e;
	Sfid *f;

	f = r->fid->aux;
	srvstagesweep(c);
	if((e = admit(c, m, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	srvopentext(r);
}

static void
metaopenq(Req *r)
{
	Srvctx *c;
	Smap *m;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	m = srvmapget(c);
	metaopenrun(r, c, m);
	srvmapput(c, m);
}

/*
 * /meta/<oid>'s open.  The row renders, so it fills the open cell
 * rather than taking the automatic path: the render reads the object's
 * record, which belongs on that object's queue (dat.h).  The reads
 * that follow are served from the fid's Text on the service loop, like
 * every other rendered file.
 */
void
srvmetaopen(Req *r)
{
	Srvctx *c;
	Sfid *f;

	c = r->srv->aux;
	f = r->fid->aux;
	if(r->ifcall.mode != OREAD){
		respond(r, Ebadopen);
		return;
	}
	srvqpush(c, f->oid, f->oidlen, r, metaopenq);
}
