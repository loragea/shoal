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
 * any other member answers `degraded', which is §14(30).
 *
 * Not everything step 1 asks can be asked here either.  The currency
 * check (§5.2) needs peers, so no check is ever completed, `cur='
 * renders 0 and §5.1's clause (c) is not evaluated — §14(31) records
 * what that costs.  Primaryship is computed from the static map and IS
 * enforced, for role=client alone: §5.1 and §5.4 are about a client
 * read and a client write, and an operator's access to a reserved
 * `shoal.' id (§2.1) is neither.
 *
 * The stage is per FID, one at a time (dat.h's Sstage).  For a client
 * operation it is created at step 3 and consumed at step 6 or
 * discarded at step 7, inside the one request; for the /repl fid of
 * the replication surface it will hold an engine Stage across many
 * Twrites, which is the lifetime §3.6 gives one and the reason the
 * state is the fid's rather than the request's.
 */

static char Estagegone[] = "shoalsrv: staged update discarded";
static char Eoom[] = "shoalsrv: out of memory";
static char Ewstatfield[] = "shoalsrv: only length may be set";
static char Efidstate[] = "shoalsrv: the fid holds state of its own";

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
 * operation, and store.md §14(33) records that.  The process-wide
 * stagetot bounds reservations, so it is the engine's alone until the
 * /repl surface makes stages that hold them.  The engine reads a zero
 * as "the default" and so does this.
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
	s->ngrain = 0;
	c->nstagedone++;
	if(c->store == nil || c->closed)
		return 0;
	c->nstageopen++;
	return 1;
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
 * Under stagelk.  A push that cannot grow the array answers 0 and its
 * caller discards where it stands: parking is what this exists for,
 * but a handle dropped on the floor holds its reservations for as long
 * as the process lives.
 */
static int
stagepend(Srvctx *c, Stage *g)
{
	Stage **p;

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
	s->dead = 1;
	qlock(&c->stagelk);
	open = stagestrip(c, s, &g);
	if(g != nil && open && stagepend(c, g))
		g = nil;
	qunlock(&c->stagelk);
	if(g != nil && open)
		stagediscard(g);
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
 * handles, which stagesweep strips, and the server's, which are
 * released whole because nothing else owns their bytes.  The trigger
 * is ARRIVAL — a stage a handler is inside right now is not an absence
 * of arrivals, and sweeping under one would take the bytes out from
 * under the commit that is reading them (§3.6).
 *
 * There is no sweeper proc.  A stage holds a reservation, and the only
 * thing that can be waiting on one is another operation on an object,
 * so the sweep runs at the head of every queued operation that names
 * an object — this file's handlers, and the walk and the stat in
 * tree.c — a store with nothing running having nothing waiting for
 * what an abandoned stage holds.  The handle is never
 * freed here — it is the fid's, and the clunk behind it is what frees
 * it (§3.6) — so the fid's next look finds it expired.
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
 * one is refused `disk full', which is §3.6's refusal for its per-fid
 * bound; so is an update covering more grains than that bound allows.
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
stagenew(Srvctx *c, Sfid *f, int kind, uchar *oid, int oidlen, uvlong ver,
	uvlong wepoch, long n, uvlong off, char **err)
{
	Sstage *s, *old;

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
	s->ngrain = stagegrains(c, off, n);
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
	 */
	qlock(&f->lk);
	old = f->aux;
	if(old != nil && f->auxflush != stageflushhook){
		/*
		 * The slot holds state that is not a stage.  Nothing on the
		 * served surface can reach here — the rows that stage are
		 * this file's, and a create, whose fid's state is the
		 * enumeration's, stages nothing — so this is the T1 fid-state
		 * point (srv.h), which fills every fid it makes.  It is not a
		 * §2.6 condition and does not pretend to be one.
		 */
		qunlock(&f->lk);
		free(s);
		*err = Efidstate;
		return nil;
	}
	if(old != nil && !old->dead && !old->released){
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
 */
static int
stagelive(Srvctx *c, Sfid *f, Sstage *s, Req *r)
{
	int ok;

	qlock(&f->lk);
	qlock(&c->stagelk);
	srvqhold(r, &c->lookhold);
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
 */
static void
stagepointarm(Srvctx *c, Sstage *s, Stage *g)
{
	qlock(&c->stagelk);
	s->g = g;
	s->busy = 0;
	qunlock(&c->stagelk);
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
 * It exists because no operation in THIS half of the surface leaves a
 * stage behind: a client write stages at §5.4 step 3 and gives the
 * stage back at step 6 or 7, inside the one request.  The stage that
 * outlives its request is the op=full stage of the replication surface
 * (§3.6, §5.5), which is the next unit's; the lifetime rules are this
 * unit's and are testable now.  The point is per context, like
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
 * How many stages the live fids hold, how many have been given back,
 * and how many of those found the store still open — which every one
 * of them must, since an engine stage's discard is an engine call
 * (dat.h).  nil for a count the caller does not want.
 */
/*
 * How many engine handles the flush-side discard has parked for the
 * drain — which is every one it has met, since it makes no engine call
 * of its own.
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
 * layer-a §5.4 step 1's admission, less the currency check this build
 * cannot make (store.md §14(31)).  §5.1 and §5.4 are about role=client
 * operations: an operator reading or writing a reserved `shoal.' id
 * (§2.1) is not one, and neither is a peer's read.
 *
 * The detail after `not primary' is §2.6's own form, and the iid in it
 * is the instance the map sends the client to.
 */
static char*
admit(Srvctx *c, Sfid *f, uchar *oid, int oidlen, char *buf, int nbuf)
{
	char id[Oidmax+1];
	Cinst *p;

	if(f->role != Rclient)
		return nil;
	oidstr(id, oid, oidlen);
	p = mapprimary(c->map, id);
	if(p == c->self)
		return nil;
	if(p == nil){
		/*
		 * No member of P(o) is up, which §4.3 makes the object's
		 * `object unavailable' at this epoch — unless placement
		 * could not be computed at all, which is never a placement
		 * answer (lib/shoal.h) and is answered as the internal
		 * error it is.
		 */
		if(mapunderrep(c->map, id) < 0)
			return srverr(buf, nbuf);
		return Eunavail;
	}
	snprint(buf, nbuf, "%s: %s", Enotprimary, p->iid);
	return buf;
}

/*
 * §5.4 steps 4 and 5, as far as a build with no peer client can take
 * them (store.md §14(30)).  M is every member of P(o) other than this
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
replicate(Srvctx *c, Sfid *f, uchar *oid, int oidlen, char *buf, int nbuf)
{
	char id[Oidmax+1];
	Cinst *p[Maxplace];
	int n;

	if(f->role != Rclient)
		return nil;
	oidstr(id, oid, oidlen);
	if((n = mapplace(c->map, id, p, nelem(p))) < 0)
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
 */
static void
objreadq(Req *r)
{
	char buf[ERRMAX], *e;
	Srvctx *c;
	Sfid *f;
	long n;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	f = r->fid->aux;
	srvstagesweep(c);
	if((e = admit(c, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
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
objwriteq(Req *r)
{
	char buf[ERRMAX], *e;
	Srvctx *c;
	Sfid *f;
	Sstage *s;
	Objinfo oi;
	uvlong off, max;
	long n;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	f = r->fid->aux;
	srvstagesweep(c);
	if((e = admit(c, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
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
	if((s = stagenew(c, f, Stwrite, f->oid, f->oidlen, oi.ver+1,
		c->map->epoch, n, off, &e)) == nil){
		srvqdone(r, e);
		return;
	}
	if((e = replicate(c, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
		stagedone(f, s);
		srvqdone(r, e);
		return;
	}
	if(!stagelive(c, f, s, r)){
		stagedone(f, s);
		srvqdone(r, Estagegone);
		return;
	}
	srvqhold(r, &c->stagehold);
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
 * the row carries an open cell at all (dat.h): ORCLOSE MUST be
 * rejected with `bad open mode', and so is anything that is not OREAD,
 * OWRITE or ORDWR with or without OTRUNC — OEXEC on an object is not a
 * mode §2.4 admits.  §2.4's other mode rule, a write on a fid opened
 * OREAD, is lib9p's to answer and never reaches here (store.md
 * §14(32)).
 *
 * OTRUNC is a truncate to zero and so is a write: it takes §5.4's path
 * entire, key and all, which is one reason the open is on the object's
 * queue like every other operation naming it.
 */
static void
objopenq(Req *r)
{
	char buf[ERRMAX], *e;
	Srvctx *c;
	Sfid *f;
	Sstage *s;
	Objinfo oi;
	Qid q;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	f = r->fid->aux;
	srvstagesweep(c);
	if((e = admit(c, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	if((e = objkey(c, f, &oi, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	if((r->ifcall.mode & OTRUNC) != 0 && oi.len != 0){
		if((s = stagenew(c, f, Sttrunc, f->oid, f->oidlen, oi.ver+1,
			c->map->epoch, 0, 0, &e)) == nil){
			srvqdone(r, e);
			return;
		}
		if((e = replicate(c, f, f->oid, f->oidlen, buf,
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
		if((s = stagenew(c, f, Stpoint, f->oid, f->oidlen, 0, 0,
			0, 0, &e)) != nil)
			stagepointarm(c, s, stageopen(c->store, f->oid,
				f->oidlen, 0, 0));
	srvqdone(r, nil);
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
	if((m & ORCLOSE) != 0){
		respond(r, Ebadopen);
		return;
	}
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
objremoveq(Req *r)
{
	char buf[ERRMAX], *e;
	Srvctx *c;
	Sfid *f;
	Sstage *s;
	Objinfo oi;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	f = r->fid->aux;
	srvstagesweep(c);
	if((e = admit(c, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	if((e = objkey(c, f, &oi, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	if((s = stagenew(c, f, Stremove, f->oid, f->oidlen, oi.ver+1,
		c->map->epoch, 0, 0, &e)) == nil){
		srvqdone(r, e);
		return;
	}
	if((e = replicate(c, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
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
 * so they carry this server's own string (§3.7, store.md §14(34)).
 *
 * convM2D leaves a field the client did not set at its null value
 * (nulldir), so what the client asked for is what differs from those.
 */
static void
objwstatq(Req *r)
{
	char buf[ERRMAX], *e;
	Srvctx *c;
	Sfid *f;
	Sstage *s;
	Objinfo oi;
	uvlong len;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	f = r->fid->aux;
	srvstagesweep(c);
	if((e = admit(c, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
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
	if((s = stagenew(c, f, Sttrunc, f->oid, f->oidlen, oi.ver+1,
		c->map->epoch, 0, 0, &e)) == nil){
		srvqdone(r, e);
		return;
	}
	if((e = replicate(c, f, f->oid, f->oidlen, buf, sizeof buf)) != nil){
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
	|| (d->muid != nil && d->muid[0] != 0)){
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
 */
static void
objcreateq(Req *r)
{
	char buf[ERRMAX], err[ERRMAX], *e;
	Srvctx *c;
	Sfid *f;
	Sstage *s;
	Objinfo oi;
	Qreq *qr;
	uvlong ver;
	Qid q;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	f = r->fid->aux;
	qr = r->aux;
	srvstagesweep(c);
	if((e = admit(c, f, qr->oid, qr->oidlen, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	if(objstat(c->store, qr->oid, qr->oidlen, &oi) < 0){
		rerrstr(err, sizeof err);
		if(srv26(err) != Enoobj){
			srvqdone(r, srverrs(buf, sizeof buf, err));
			return;
		}
		ver = 1;
	}else if(oi.state == Stomb)
		ver = oi.ver + 1;
	else{
		srvqdone(r, Eexists);
		return;
	}
	if((e = replicate(c, f, qr->oid, qr->oidlen, buf, sizeof buf)) != nil){
		srvqdone(r, e);
		return;
	}
	if(objcreate(c->store, qr->oid, qr->oidlen, ver, c->map->epoch, nil, 0,
		&oi) < 0){
		srvqexit(r);
		srvrerror(r);
		return;
	}
	/*
	 * The create succeeded, so the fid moves: whatever it held as a
	 * directory fid goes back through its own hooks first (dat.h), and
	 * only then is the fid the object's.
	 */
	srvfidgive(f);
	f->file = Qobjfile;
	memmove(f->oid, qr->oid, qr->oidlen);
	f->oidlen = qr->oidlen;
	srvobjqid(f, &oi, &q);
	f->qidpath = q.path;
	f->qidvers = q.vers;
	r->ofcall.qid = q;
	srvqexit(r);
	srvqdone(r, nil);
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
	if((mode & ORCLOSE) != 0){
		respond(r, Ebadopen);
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
 *		store.md §14(31) records it.
 *	ready=	the same fact said the other way round: an instance that
 *		has completed no currency check is not ready to serve the
 *		object under §5.1's clause (c), so this reads `no'.
 *	placement=, primary=
 *		computed from the map, which §2.4 marks advisory; they are
 *		the part of the line a static map answers in full.
 */
char*
srvmetatext(Srvctx *c, Sfid *f, Text *t)
{
	char buf[ERRMAX], id[Oidmax+1], csum[Csumhexlen];
	Objinfo oi;
	Cinst *p[Maxplace], *pr;
	int i, n;

	if(objstat(c->store, f->oid, f->oidlen, &oi) < 0)
		return srverr(buf, sizeof buf);
	if(oi.state != Slive)
		return Edeleted;
	oidstr(id, f->oid, f->oidlen);
	csumfmt(csum, oi.csum);
	if((n = mapplace(c->map, id, p, nelem(p))) < 0)
		return srverr(buf, sizeof buf);
	if(n > nelem(p))
		n = nelem(p);
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
	for(i = 0; i < n; i++)
		textprint(t, "%s%s", i > 0 ? "," : "", p[i]->iid);
	pr = mapprimary(c->map, id);
	textprint(t, " primary=%s ready=no\n", pr != nil ? pr->iid : "");
	return nil;
}

static void
metaopenq(Req *r)
{
	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	srvstagesweep(r->srv->aux);
	srvopentext(r);
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
	if((r->ifcall.mode & ORCLOSE) != 0){
		respond(r, Ebadopen);
		return;
	}
	if(r->ifcall.mode != OREAD){
		respond(r, Ebadopen);
		return;
	}
	srvqpush(c, f->oid, f->oidlen, r, metaopenq);
}
