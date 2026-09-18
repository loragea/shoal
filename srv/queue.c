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
 * The queue pool, docs/design/store.md §7 and layer-a §5.4.1.
 *
 * lib9p's service loop is single-threaded and has no mode flag, so a
 * handler that blocks blocks everything, the Tflush layer-a §5.4.1
 * requires to be answerable included.  Every operation that names an
 * object is therefore pushed to one of `-q' Reqqueues hashed by oid,
 * each with a proc of its own, and the loop goes on serving while it
 * blocks.  There are no fast paths: a walk to /obj/<oid> or
 * /meta/<oid>, a stat of one, and a ctl verb naming an oid all go
 * through the oid's queue, because the queue is the object's ordering
 * point (§5.4 step 2) and a read that skipped it would not be ordered
 * against the write beside it.  /status, /map and the ctl verbs that
 * name no object are answered on the loop.
 *
 * Two operations on one object share a queue and so are totally
 * ordered by construction, and operations on distinct objects run in
 * different queues.  The pool size is a ceiling on concurrent object
 * operations rather than a collision parameter (§7), and /status
 * reports the depth so that saturation is visible: Reqqueue exposes no
 * count of its own, so this file counts the requests the pool takes on
 * — at the preparation, which is where the Req is armed and therefore
 * where its completion is counted from — and srvdestroyreq counts them
 * off again.
 */

static void	qhold(Srvctx*, Qreq*, uvlong*, uvlong*);
static void	holdms(Srvctx*, uvlong*);

enum
{
	/*
	 * How long a hold that parks the SERVICE LOOP waits before it
	 * lets go of its own accord.  Such a hold is the one the shutdown
	 * cannot clear — srvholdclear runs from Srv.end, which lib9p
	 * calls on the loop — so an unbounded park there would wedge the
	 * server for as long as the program lived.  Two points can park
	 * the loop: the flush hold, which is only ever reached from
	 * Srv.flush, and the step 7 hold when the flushed request was
	 * still queued and the loop performs step 7 for it.
	 */
	Loopholdms	= 5000,
};

/*
 * The hash is this server's, not layer-a §4.2's: nothing on the wire
 * depends on which queue an oid lands in, so it is an FNV-1a over the
 * id's bytes and implementation policy entire.
 */
static ulong
oidhash(uchar *oid, int oidlen)
{
	ulong h;
	int i;

	h = 2166136261UL;
	for(i = 0; i < oidlen; i++){
		h ^= oid[i];
		h *= 16777619UL;
	}
	return h;
}

int
srvqinit(Srvctx *c, int nq)
{
	int i;

	if(nq <= 0)
		nq = Nqueuedflt;
	if(nq > Nqueuemax)
		nq = Nqueuemax;
	if((c->q = mallocz(nq*sizeof *c->q, 1)) == nil)
		return -1;
	for(i = 0; i < nq; i++){
		if((c->q[i] = reqqueuecreate()) == nil){
			werrstr("reqqueuecreate: %r");
			return -1;
		}
		c->nq = i+1;
	}
	if((c->anyq = reqqueuecreate()) == nil){
		werrstr("reqqueuecreate: %r");
		return -1;
	}
	return 0;
}

Qreq*
srvqreq(Req *r)
{
	return r->aux;
}

static void
qrun(Req *r)
{
	Qreq *qr;

	qr = r->aux;
	/*
	 * Marked before the handler and under the lock srvqflush reads it
	 * under, so that a Tflush either finds the request still waiting
	 * — and performs step 7 itself, since lib9p answers a request it
	 * unlinks from the queue without this handler running at all — or
	 * finds it started and leaves step 7 to srvqdone.  A Tflush in
	 * the window between lib9p taking the request off the queue and
	 * this mark takes the first branch and this handler then runs all
	 * the same (srvqflush), which is why step 7 is the fid's state's
	 * and not this request's to hold.
	 */
	qlock(&qr->lk);
	qr->running = 1;
	qunlock(&qr->lk);
	qr->f(r);
	/*
	 * Nothing of the request is touched after the handler: it has
	 * responded, and lib9p may already have freed the Req.  The
	 * completion is counted by srvdestroyreq, which lib9p calls on
	 * exactly that free — and which is the only count that is right
	 * for BOTH ways a pushed request can end, since a request
	 * reqqueueflush removes from the queue is answered by lib9p
	 * without this handler ever running.
	 */
}

/*
 * Called by srvdestroyreq, once per pushed request, whichever way it
 * ended.  The Req is gone by then, so the drain it feeds waits for
 * every parked Rflush to have been answered too, which is what makes
 * it a safe point to close the store at.
 */
void
srvqended(Qreq *qr)
{
	Srvctx *c;
	uvlong ms, n;

	c = qr->ctx;
	lock(&c->cntlk);
	c->ndone++;
	unlock(&c->cntlk);
	/*
	 * The §13 point that widens the gap this count opens: the count is
	 * taken from lib9p's closereq, inside respond and before respond's
	 * own trailing release of the Srv, so the drain can converge and
	 * the service loop end while this proc is still inside lib9p.
	 * Nothing of the context is touched while it waits.
	 *
	 * The point is re-read as it waits, so clearing it ends the wait
	 * as well: a test that has to say exactly when lib9p is let go of
	 * — rather than guess a count of milliseconds and race it — sets a
	 * long hold and clears it at the moment it means.  The count is
	 * still the bound, because nothing clears this point for a program
	 * that set it and stopped watching; the shutdown does not (srv.h).
	 */
	qlock(&c->holdlk);
	ms = c->endhold;
	qunlock(&c->holdlk);
	for(; ms >= 5; ms -= 5){
		sleep(5);
		qlock(&c->holdlk);
		n = c->endhold;
		qunlock(&c->holdlk);
		if(n == 0)
			break;
	}
}

void
srvendpoint(Srvctx *c, uvlong ms)
{
	qlock(&c->holdlk);
	c->endhold = ms;
	qunlock(&c->holdlk);
}

/*
 * Push r onto the queue oid hashes to.  The Reqqueue is kept in
 * Req.aux (§7) so Srv.flush can find it; the handler and, for a ctl
 * verb, its parsed line ride along, and srvdestroyreq frees the lot
 * when lib9p frees the Req — after any parked Rflush has been
 * answered, so a Tflush that finds the request in the pool always
 * finds this beside it.
 */
static Qreq*
qprep(Srvctx *c, Reqqueue *q, uchar *oid, int oidlen, Req *r,
	void (*f)(Req*))
{
	Qreq *qr;

	if((qr = mallocz(sizeof *qr, 1)) == nil){
		respond(r, "shoalsrv: out of memory");
		return nil;
	}
	qr->ctx = c;
	qr->f = f;
	qr->q = q;
	if(oidlen > 0 && oidlen <= Oidmax){
		memmove(qr->oid, oid, oidlen);
		qr->oidlen = oidlen;
	}
	/*
	 * Counted here rather than at the push, because this is the line
	 * the completion is counted against: srvdestroyreq counts every
	 * Req that carries a Qreq, whether it was pushed or answered on
	 * the service loop after all.  Counting at the push would let a
	 * caller that prepares a request and then answers it here take
	 * the pool's depth below zero — where it is unsigned, so /status
	 * prints 2^64-1 and the drain never converges.
	 */
	lock(&c->cntlk);
	c->npush++;
	unlock(&c->cntlk);
	r->aux = qr;
	return qr;
}

Qreq*
srvqprep(Srvctx *c, uchar *oid, int oidlen, Req *r, void (*f)(Req*))
{
	return qprep(c, c->q[oidhash(oid, oidlen) % c->nq], oid, oidlen, r, f);
}

/*
 * The same for an operation that names no object: a status render, a
 * directory read, anything that takes an engine snapshot and so must
 * not be run on the service loop.  It goes to a queue of its own,
 * outside the pool the oids hash into, so that offloading it neither
 * waits behind an object's operations nor delays them — the pool is
 * the objects' ordering point (§5.4 step 2) and nothing else may
 * change what is ordered against what.  One queue, because there is
 * nothing to order here and a second would only add a proc.
 *
 * Everything else about such a request is an ordinary pushed one: it
 * is counted by the pool's depth, it is flushable, and it MUST leave
 * through srvqdone.
 */
Qreq*
srvqprepany(Srvctx *c, Req *r, void (*f)(Req*))
{
	return qprep(c, c->anyq, nil, 0, r, f);
}

void
srvqpushany(Srvctx *c, Req *r, void (*f)(Req*))
{
	if(srvqprepany(c, r, f) != nil)
		srvqgo(c, r);
}

/*
 * The push is separate from the preparation so that a caller with more
 * to say — a ctl verb parsed on the service loop and run on the queue
 * — can fill the Qreq before the queue proc can see it.  After this
 * call the request is the queue's and nothing here may touch it.
 *
 * `pushed' is marked before the push and under the lock the exit reads
 * it under, because the queue's proc can be inside the handler before
 * reqqueuepush has returned.  Until it is marked, the queue's flush
 * flag is not this request's to read (dat.h).
 */
void
srvqgo(Srvctx *c, Req *r)
{
	Qreq *qr;

	USED(c);
	qr = r->aux;
	qlock(&qr->lk);
	qr->pushed = 1;
	qunlock(&qr->lk);
	reqqueuepush(qr->q, r, qrun);
}

void
srvqpush(Srvctx *c, uchar *oid, int oidlen, Req *r, void (*f)(Req*))
{
	if(srvqprep(c, oid, oidlen, r, f) != nil)
		srvqgo(c, r);
}

static void
qjobrun(Req *r)
{
	Qjob *j;

	j = (Qjob*)r;		/* the Req is Qjob's first member (dat.h) */
	j->fn(j->arg);
	qlock(&j->lk);
	j->done = 1;
	rwakeup(&j->rz);
	qunlock(&j->lk);
}

/*
 * The pool's entry for a caller that is not a request: the background
 * passes, which run work on an object's queue because the queue is
 * that object's ordering point (§5.4 step 2) and a mutation or a read
 * outside it is ordered against nothing.
 *
 * THE RULE, which is why this is an entry point of its own and not a
 * Req threaded through srvqprep: a non-Req caller never enters
 * `respond'.  It has no tag, so nothing can flush it; it is not in
 * lib9p's Req pool, so it holds no reference to the Srv and no pool
 * freed it; and its Req is the caller's stack.  respond on such a Req
 * would compute an Rmsg type from an ifcall that was never filled,
 * free a stack address, and drop a reference the caller never took.
 * So this path allocates nothing — the Qreq rides in the Qjob — and
 * there is no failure for it to answer: it returns once the work has
 * run.  The completion the pool is owed is given back here through
 * srvqended, which is the count srvdestroyreq takes for a real Req;
 * until it is given back, the shutdown's drain counts this unit as in
 * flight, which is what makes the store outlive it (D16).
 */
int
srvqjob(Srvctx *c, uchar *oid, int oidlen, void (*fn)(void*), void *arg)
{
	Qjob j;

	memset(&j, 0, sizeof j);
	j.r.srv = srv9p(c);
	j.fn = fn;
	j.arg = arg;
	j.rz.l = &j.lk;
	j.qr.ctx = c;
	j.qr.f = qjobrun;
	j.qr.q = c->q[oidhash(oid, oidlen) % c->nq];
	if(oidlen > 0 && oidlen <= Oidmax){
		memmove(j.qr.oid, oid, oidlen);
		j.qr.oidlen = oidlen;
	}
	/*
	 * Counted where srvqprep counts a request's: at the arming, so
	 * that the push and the completion below are the same unit to the
	 * drain (srvqdrain) and to /status's depth.
	 */
	lock(&c->cntlk);
	c->npush++;
	unlock(&c->cntlk);
	j.r.aux = &j.qr;
	srvqgo(c, &j.r);
	qlock(&j.lk);
	while(!j.done)
		rsleep(&j.rz);
	qunlock(&j.lk);
	srvqended(&j.qr);
	return 0;
}

/*
 * Srv.flush.  layer-a §5.4.1: a Tflush naming a pending object
 * operation MUST be answered with Rflush, because devmnt sends one on
 * interrupt and waits for it, and a server that never answers leaves
 * the client process wedged and unkillable.
 *
 * reqqueueflush does the two halves: a request still queued is removed
 * and answered `interrupted' — an Rerror before the Rflush, which is
 * legal 9P and what store.md §14(14) amended §5.4.1 to permit — and a
 * running one has its queue's flush flag raised and its proc
 * interrupted.  respond(r, nil) then hands the Rflush to lib9p, which
 * parks it until the flushed request itself responds.
 *
 * A Tflush naming a request that was answered on the service loop
 * finds no Qreq: the loop answers before it reads the next message, so
 * such a request has already responded and lib9p is only waiting to be
 * told to send the Rflush.
 *
 * What reqqueueflush must not be shown is a request that has already
 * answered.  Its second branch — the one for a request it does not
 * find running — responds `interrupted' whether or not it found the
 * request queued either, and lib9p's respond asserts that a request
 * has not responded before, so a second answer aborts the server
 * rather than being ignored.  The window is real: sflush looks the
 * request up (and holds a reference to it, which is why the Qreq is
 * still here) before this runs, and the queue proc can finish it in
 * between.  The Qreq's `done' closes it — set by srvqdone under the
 * lock this holds across reqqueueflush, so either the flush reaches
 * lib9p before the request answers or it is not made at all.  Either
 * way the Rflush follows, which is all 9P asks of a flush whose
 * request has already been answered.
 *
 * A request that carries a Qreq and was never pushed — prepared for a
 * queue and answered on the service loop after all, which queue.c's
 * head blesses — is not the queue's to flush either: reqqueueflush
 * would not find it running, would find nothing to unlink, and would
 * answer it `interrupted' behind the back of the loop that is
 * answering it.  The loop is this proc, so the two cannot really
 * overlap; `pushed' says so rather than leaving it to be re-derived.
 *
 * Step 7 is performed here for a request that is still WAITING on the
 * queue, and only for one.  layer-a §5.4.1 makes the whole of step 7 a
 * MUST on flush however far the request had got, and a fid's stage
 * spans several Twrites, so a Tflush of the next queued write on a
 * staging fid must still discard that fid's stage; but reqqueueflush
 * unlinks such a request and answers it itself, and then neither the
 * handler nor srvqdone — the one exit that performs step 7 — runs.
 * `running' is what tells the two apart, and `step7' is what keeps
 * them from both doing it: a request whose handler has started leaves
 * through srvqdone, which does it there.  The fid is live either way —
 * sflush holds a reference to the flushed Req, which holds one to its
 * Fid (/sys/src/lib9p/req.c, fid.c) — so r->oldreq->fid is safe to
 * read.
 *
 * Step 7 run here runs on the SERVICE LOOP, which is what the fid's
 * state lock is for (srvstep7): the flushed request's fid may be one
 * another outstanding request is working through on a queue proc, and
 * the flushed request's own handler may still run after this.  That
 * last is lib9p's window, not this server's: _reqqueueproc unlinks the
 * request and sets q->cur under one qlock and only then calls the
 * handler, which is where `running' is marked, so a Tflush that lands
 * in between finds running == 0, performs step 7 here, and is then
 * taken by reqqueueflush's q->cur branch — an interrupt of the queue
 * proc, with no unlink and no answer — after which the handler runs
 * with its fid's state already discarded.  A handler tolerates that
 * (dat.h); it is why step 7 belongs to the fid's state and not to the
 * request.
 */
void
srvqflush(Req *r)
{
	Qreq *qr;

	if(r->oldreq == nil || (qr = r->oldreq->aux) == nil){
		respond(r, nil);
		return;
	}
	holdms(qr->ctx, &qr->ctx->flushhold);
	qlock(&qr->lk);
	if(!qr->done && qr->pushed){
		if(!qr->running && !qr->step7){
			qr->step7 = 1;
			srvstep7(r->oldreq, 1);
		}
		reqqueueflush(qr->q, r->oldreq);
	}
	qunlock(&qr->lk);
	respond(r, nil);
}

/*
 * The check point a queued handler tests.  store.md §7: the check
 * point is a test of the queue's flush flag and an `interrupted'
 * return from a device call — and the two strings are the same, which
 * is the hazard this file exists to contain.  The flag says a request
 * was flushed; an error string never does.  This function reports the
 * flag to a handler that wants to stop before doing the work; srvqdone
 * is what tests it on the way out, classifies the error beside it and
 * answers the two causes apart (err.c).
 *
 * The §13 holds are here because this is where a request already
 * running can be made to stay running.  A hold of a queue proc ends
 * when its point is cleared or, for one that names the request, when
 * this queue's flush flag is set, whichever is first — so a held
 * request is still flushable and the shutdown that clears every point
 * (srvholdclear) is not held up by one.  The flush hold below is the
 * exception, and says why.
 *
 * `cnt', where a point has one, counts the ARRIVALS at it — under the
 * same lock, before the park, and only while the point is set, so it
 * says a request got here and not that one is still here.  That is what
 * a test waits on: the request it wants parked is the one it just
 * pushed, and a wait on the count is a wait on the window this park
 * opens (srv.h's srvheld).  nil for a point nothing waits on.
 */
static void
qhold(Srvctx *c, Qreq *qr, uvlong *pt, uvlong *cnt)
{
	qlock(&c->holdlk);
	if(cnt != nil && *pt != 0)
		(*cnt)++;
	while(*pt != 0 && (qr == nil || qr->q->flush == 0)){
		qunlock(&c->holdlk);
		sleep(5);
		qlock(&c->holdlk);
	}
	qunlock(&c->holdlk);
}

/*
 * The same park, for a point that holds the SERVICE LOOP rather than a
 * queue proc.  srvholdclear cannot reach such a point: the shutdown
 * runs from Srv.end, which lib9p calls on the loop, so a loop parked
 * here never gets there and nothing else would ever clear it.  It
 * therefore releases itself after Loopholdms, which is what keeps a
 * point a program set and stopped watching from wedging the server for
 * good.
 */
static void
holdms(Srvctx *c, uvlong *pt)
{
	int i;

	qlock(&c->holdlk);
	for(i = 0; *pt != 0 && i < Loopholdms/5; i++){
		qunlock(&c->holdlk);
		sleep(5);
		qlock(&c->holdlk);
	}
	qunlock(&c->holdlk);
}

int
srvqcheck(Req *r)
{
	Qreq *qr;

	qr = r->aux;
	if(qr == nil)
		return 0;
	/*
	 * The hold is a step of the handler, so the fid-state point takes
	 * the fid's state lock across it and marks the state mid-step:
	 * that is what a row's own handler does around the engine call
	 * that works on what its fid holds (dat.h), and what the point is
	 * modelling here is exactly that handler.  Both calls are inert
	 * while the point is off.
	 */
	srvauxstep(r, 1);
	qhold(qr->ctx, qr, &qr->ctx->hold, nil);
	srvauxstep(r, 0);
	return qr->q->flush != 0;
}

/*
 * The point that keeps the reserved queue's flush flag raised: an
 * offloaded request that has found itself flushed is held here, before
 * it leaves through srvqdone, so its proc is still inside the handler
 * and has not looped round to clear the flag.  That is the one window
 * in which the loop can prepare a second request for that queue while
 * the flag of the first is still up, which is what a request that was
 * never pushed must not read.
 */
void
srvqanyexit(Srvctx *c)
{
	qhold(c, nil, &c->anyexit, nil);
}

/*
 * The point at the end of a background pass, which is not a request
 * and has no queue: a pass is parked here with its record still on the
 * job list, so that what it finished with is readable at /jobs rather
 * than raced against the unlink.  It holds a pass proc, so the
 * shutdown's srvholdclear reaches it before jobwait.
 */
void
srvjobhold(Srvctx *c)
{
	qhold(c, nil, &c->jobhold, nil);
}

/*
 * The one point here that refuses rather than holds: whether this read
 * of an index walk's n'th unit is to be treated as having failed — the
 * scrub pass's read of an index slot (job.c) and the directory read's
 * of a snapshot position (enum.c) both ask.  It is the only way to
 * break such a walk off part-way while the store underneath it stays
 * healthy — the engine's own way of refusing an index read is to be
 * condemned, which refuses every other call the walk would make as
 * well, so a walk broken off that way cannot be told from one whose
 * store has gone.  Set to n+1; 0 is off, and srvholdclear turns it off
 * with the rest.
 */
int
srvslotfail(Srvctx *c, uvlong slot)
{
	uvlong n;

	qlock(&c->holdlk);
	n = c->slotfail;
	qunlock(&c->holdlk);
	return n != 0 && slot == n-1;
}

/*
 * The point inside the tombstone reclaim walk (job.c), which nothing
 * else here can stop part-way: the scrub that carries it is already
 * past its index walk when the walk begins, and the walk itself is
 * paced by nothing and asks no queue.  n != 0 parks it before its
 * n-1'th entry, with the entries before that one counted, so a test
 * can raise `scrub stop' or take the server down over a walk that
 * has counted a prefix of the snapshot.  Set to n+1, like slotfail; 0
 * is off, and srvholdclear turns it off with the rest.
 */
void
srvreclaimhold(Srvctx *c, uvlong i)
{
	uvlong n;

	qlock(&c->holdlk);
	n = c->reclaimhold;
	qunlock(&c->holdlk);
	if(n != 0 && i == n-1)
		qhold(c, nil, &c->reclaimhold, nil);
}

/*
 * The point inside the /obj and /meta directory read's entry walk
 * (enum.c), which is the one stretch of a queued handler that runs
 * over the fid's state with the fid's state lock NOT held.  n != 0
 * parks the read before its n-1'th entry, with the entries before it
 * already converted, so a test can drive the service loop at a fid a
 * queue proc is part-way through a listing of — a Tflush of a sibling
 * request on that same fid, which the loop performs step 7 for.  Set
 * to n+1, like slotfail; the hold ends on this queue's flush flag as
 * the other request holds do, so the read itself stays flushable.
 */
void
srvdirhold(Req *r, uvlong i)
{
	Qreq *qr;
	uvlong n;

	if((qr = r->aux) == nil)
		return;
	qlock(&qr->ctx->holdlk);
	n = qr->ctx->dirhold;
	qunlock(&qr->ctx->holdlk);
	if(n != 0 && i == n-1)
		qhold(qr->ctx, qr, &qr->ctx->dirhold, nil);
}

/*
 * The third point, at a queued walk's commit: the moment a walk that
 * moves its fid has given the old state back and is about to write
 * the new one.  It is where the service loop is concurrent with the
 * fid a queue proc is rewriting, so it is where a test drives an
 * attach against a walk.  A walk answered on the service loop carries
 * no Qreq and cannot be held.
 */
void
srvqwalkhold(Req *r)
{
	Qreq *qr;

	if((qr = r->aux) == nil)
		return;
	qhold(qr->ctx, qr, &qr->ctx->walkhold, nil);
}

/*
 * A point of the context's, named by the field it is set in, held by
 * the queue proc that is carrying this request: the park and its two
 * exits — the point cleared, or this queue's flush flag raised — are
 * this file's, and which windows are worth a point is the handler's
 * (obj.c holds two of the object write path's).  A request answered on
 * the service loop carries no Qreq and cannot be held.
 *
 * A background unit is not held here either, although it carries a
 * Qreq: srvqjob's handler is the caller's own function and calls
 * nothing in this file, and the flush flag that is one of the two
 * exits above belongs to a tag — which a unit with no tag can neither
 * raise nor be named by.  A point that parked one would therefore have
 * only the clearing to wake it, which is what the shutdown does before
 * it waits (srv.h).
 */
void
srvqhold(Req *r, uvlong *pt, uvlong *cnt)
{
	Qreq *qr;

	if((qr = r->aux) == nil)
		return;
	qhold(qr->ctx, qr, pt, cnt);
}

/*
 * The second point, at the other end of a handler: after its engine
 * call and before its exit, so a test can flush a request whose work
 * is already done and watch it leave through srvqdone all the same.
 * The error string the handler is carrying survives the hold, since
 * the exit below it is the one that reads %r.
 */
void
srvqexit(Req *r)
{
	char err[ERRMAX];
	Qreq *qr;

	if((qr = r->aux) == nil)
		return;
	rerrstr(err, sizeof err);
	qhold(qr->ctx, qr, &qr->ctx->exithold, nil);
	errstr(err, sizeof err);
}

/*
 * layer-a §5.4.1's step 7, in one place.  On a flush the instance MUST
 * perform the whole of it: discard the staged update, clear the
 * per-(object, peer) sync state for every candidate involved,
 * invalidate `cur' for the object, and re-run the currency check
 * before serving it again.
 *
 * It takes the flushed request, because that is what reaches the three
 * things the halves need: the fid through r->fid (a stage is per-fid),
 * the context through r->srv->aux, and the oid through the Qreq.
 *
 * Which halves exist today, and where the rest hook in:
 *
 *	discard the stage — the flushed fid's auxflush cell, which the
 *		object-I/O surface fills with the discard of whatever that
 *		fid staged (obj.c, dat.h's Sstage).  It is the FID's stage
 *		and not this request's: a stage can span several Twrites,
 *		so a Tflush of the next queued write on a staging fid
 *		discards it too, and the handler that staged it finds it
 *		gone at its next look.  The engine's own half of the
 *		release (lib/shoal.h's stagediscard) hangs off that cell,
 *		so nothing here had to change for it — and is not made
 *		from the cell either: the cell reaches the handle under
 *		the context's leaf lock, so it parks the handle and
 *		obj.c's drain makes the call outside every lock (dat.h's
 *		Sstage).  A hook MAY call the engine — the enumeration's
 *		makes one of store.md §9's three from both call sites
 *		below — so it is where this one would be made and not
 *		that it is one.
 *	clear the sync state — this instance has no peers: there is no
 *		outbound peer client in this wave, so no candidate was
 *		ever told anything and the dirty set (lib/shoal.h's
 *		dirtyadd, dirtydel) has nothing to retract.  The
 *		replication surface hooks its retraction in here.
 *	invalidate `cur' and re-check currency — `cur' is the epoch of
 *		the last completed currency check (layer-a §5.2), which
 *		is an in-memory value the engine does not hold
 *		(store.md §14(1)) and which nothing computes without
 *		peers to query.  The currency check hooks in here.
 *
 * So the only half this function performs today is the per-fid one,
 * and it is still called from the one exit below whether or not a fid
 * has filled its cell, because the call site is the contract: the
 * halves are added to this function, not to the handlers.
 */
/*
 * §13's point, wherever step 7 parks: on the loop it ends on its own
 * deadline, because the shutdown that would clear it runs on the loop
 * it is parking (srv.h).
 */
static void
holdstep7(Srvctx *c, int onloop)
{
	if(onloop)
		holdms(c, &c->step7hold);
	else
		qhold(c, nil, &c->step7hold, nil);
}

void
srvstep7(Req *r, int onloop)
{
	Srvctx *c;
	Sfid *f;

	if(r->fid == nil || (f = r->fid->aux) == nil)
		return;
	c = r->srv->aux;
	/*
	 * A fid with no flush cell has nothing here to do, and this runs
	 * on the SERVICE LOOP for a request that was still queued — where
	 * the state lock is a lock queue procs hold across work of their
	 * own, and the loop must block on nothing a queue proc needs
	 * (dat.h).  So the cell is read before the lock and the lock taken
	 * only when there is a call to make under it; the read under the
	 * lock is still what decides, so a cell cleared in between calls
	 * nothing.  A cell INSTALLED in between is missed, which is the
	 * race the lock leaves in any case — a flush that landed a moment
	 * earlier misses it too, and the handler that installed it is the
	 * one that gives it back.
	 *
	 * The point parks either way: with no cell there is no state for a
	 * test to drive a clunk or a walk against, so the park outside the
	 * lock is the same park.
	 */
	if(f->auxflush == nil){
		holdstep7(c, onloop);
		return;
	}
	/*
	 * Under the FID'S STATE lock, over the read of the cell and the
	 * call through it.  lib9p's own reference keeps the Fid alive
	 * across this, but not what the fid is holding, and two things
	 * reach the same state: a clunk or a moving walk runs fidgive,
	 * which clears the cells and frees what they named, and another
	 * request on this same fid may be part-way through the state the
	 * hook is discarding — 9P allows two requests to be outstanding
	 * on one fid, and this can run on the service loop (srvqflush)
	 * while a queue proc is inside such a handler.  All of them take
	 * this lock, so the hook runs between two of that handler's steps
	 * and never inside one, and fidgive waits for it rather than
	 * freeing under it (dat.h).
	 *
	 * The registry lock is NOT held here: a hook may call the engine,
	 * and fidlk held across device I/O would block every attach,
	 * clunk and clone-walk behind this one discard (store.md §7
	 * rule 2).  Nothing of the registry is read, either — the context
	 * comes from the Srv and the fid from the request.
	 *
	 * `closed' is not tested, as auxclose1 tests it, because nothing
	 * can reach here after the store has closed: the shutdown drains
	 * every request in flight before it closes the store (D16), and a
	 * request in flight is what this runs for.
	 */
	/*
	 * The hold is inside the state lock, which is what a test drives a
	 * clunk or a moving walk of this fid against, and outside the
	 * registry lock, which a parked proc holding would wedge the loop
	 * and with it the shutdown that clears the point.  On the service
	 * loop the point cannot be cleared at all — srvholdclear runs from
	 * Srv.end, which lib9p calls on the loop — so there it ends on the
	 * same deadline the flush hold does.
	 */
	qlock(&f->lk);
	holdstep7(c, onloop);
	if(f->auxflush != nil)
		f->auxflush(f, r);
	qunlock(&f->lk);
}

/*
 * The one exit from a queued handler.  A handler that found its
 * request flushed unwinds through here, which performs step 7 and
 * answers `interrupted' — the same string reqqueueflush gives a
 * request it removed from the queue, so a client sees one answer for
 * one condition however far the request had got.  The reply may
 * precede the Rflush; 9P has the client discard the reply to a request
 * it flushed (§5.4.1, store.md §14(14)).
 *
 * A request flushed before its handler started has had step 7 run by
 * srvqflush, which is the only place it can run before lib9p's own
 * answer; the Qreq records that, so this exit does not repeat it.
 *
 * store.md §7 gives a device `interrupted' the same unwind: "either
 * one unwinds into the whole of step 7".  A note aborts a system call
 * whether or not a Tflush sent it, so a handler can be told
 * `interrupted' by the device with this queue's flush flag still
 * clear, and a step 7 skipped there would leave a stage behind and an
 * object whose currency check was never re-run.  The two causes stay
 * distinguishable on the wire — `interrupted' for the flush, this
 * server's own `shoalsrv: interrupted' for the other — because only
 * the flag says a request was flushed (err.c).
 */
void
srvqdone(Req *r, char *err)
{
	char buf[ERRMAX];
	Qreq *qr;
	int flushed, did7;

	if((qr = r->aux) != nil){
		/*
		 * Marked answered before it answers, and the flag read
		 * under the same lock: a Tflush that arrives after this
		 * takes the Rflush alone (srvqflush), and one that got in
		 * first is seen here.
		 *
		 * The flag is the QUEUE's, raised for the request its proc
		 * is carrying and cleared when that proc takes the next one,
		 * so it says nothing about a request the queue never took:
		 * without `pushed' a request prepared for a queue and
		 * answered on the loop reads whatever flag the queue's own
		 * current request left raised, and answers itself
		 * `interrupted' with step 7 run on a fid whose stage the
		 * client still owns.
		 */
		qlock(&qr->lk);
		qr->done = 1;
		flushed = qr->pushed && qr->q->flush != 0;
		did7 = qr->step7;
		qr->step7 = 1;
		qunlock(&qr->lk);
		if(flushed || srvintr(err)){
			if(!did7)
				srvstep7(r, 0);
			respond(r, flushed ? Einterrupted : Edevintr);
			return;
		}
	}
	if(err != nil)
		err = srverrs(buf, sizeof buf, err);
	respond(r, err);
}

/*
 * D16's drain: the requests in flight are let finish before the store
 * is closed.  Nothing new can be pushed by then — the 9P loop has
 * ended — so the counters converge.
 */
void
srvqdrain(Srvctx *c)
{
	uvlong np, nd;

	for(;;){
		lock(&c->cntlk);
		np = c->npush;
		nd = c->ndone;
		unlock(&c->cntlk);
		if(np == nd)
			return;
		sleep(5);
	}
}

void
srvqfree(Srvctx *c)
{
	int i;

	for(i = 0; i < c->nq; i++)
		reqqueuefree(c->q[i]);
	free(c->q);
	c->q = nil;
	c->nq = 0;
	if(c->anyq != nil){
		reqqueuefree(c->anyq);
		c->anyq = nil;
	}
}

void
srvcount(Srvctx *c, uvlong *pushed, uvlong *done)
{
	lock(&c->cntlk);
	*pushed = c->npush;
	*done = c->ndone;
	unlock(&c->cntlk);
}

void
srvhook(Srvctx *c, char *name, uvlong n)
{
	qlock(&c->holdlk);
	if(strcmp(name, "objhold") == 0)
		c->hold = n;
	else if(strcmp(name, "objprelook") == 0)
		c->prelookhold = n;
	else if(strcmp(name, "objstage") == 0)
		c->stagehold = n;
	else if(strcmp(name, "objlook") == 0)
		c->lookhold = n;
	else if(strcmp(name, "objarm") == 0)
		c->armhold = n;
	else if(strcmp(name, "objexit") == 0)
		c->exithold = n;
	else if(strcmp(name, "flushhold") == 0)
		c->flushhold = n;
	else if(strcmp(name, "fullhold") == 0)
		c->fullhold = n;
	else if(strcmp(name, "openhold") == 0)
		c->openhold = n;
	else if(strcmp(name, "finalhold") == 0)
		c->finalhold = n;
	else if(strcmp(name, "mapopen") == 0)
		c->mapopen = n;
	else if(strcmp(name, "walkhold") == 0)
		c->walkhold = n;
	else if(strcmp(name, "anyexit") == 0)
		c->anyexit = n;
	else if(strcmp(name, "step7") == 0)
		c->step7hold = n;
	else if(strcmp(name, "jobhold") == 0)
		c->jobhold = n;
	else if(strcmp(name, "slotfail") == 0)
		c->slotfail = n;
	else if(strcmp(name, "reclaimhold") == 0)
		c->reclaimhold = n;
	else if(strcmp(name, "dirhold") == 0)
		c->dirhold = n;
	qunlock(&c->holdlk);
}

/*
 * Clear every point that can hold a request, which the shutdown does
 * before it drains: a point is a T1 thing and the program that set it
 * is not necessarily watching when the connection drops, and a
 * request left holding would hold the drain — and with it the store's
 * close — for as long as the program lived.
 *
 * This reaches the queue procs' holds.  It does not reach a loop
 * parked in the flush hold, because it is the loop that gets here;
 * that hold ends on its own deadline instead (holdflush).
 */
void
srvholdclear(Srvctx *c)
{
	qlock(&c->holdlk);
	c->hold = 0;
	c->prelookhold = 0;
	c->stagehold = 0;
	c->lookhold = 0;
	c->armhold = 0;
	c->exithold = 0;
	c->flushhold = 0;
	c->fullhold = 0;
	c->openhold = 0;
	c->finalhold = 0;
	c->mapopen = 0;
	c->walkhold = 0;
	c->anyexit = 0;
	c->step7hold = 0;
	c->jobhold = 0;
	c->slotfail = 0;
	c->reclaimhold = 0;
	c->dirhold = 0;
	qunlock(&c->holdlk);
}

/*
 * How many requests have reached a point (srv.h).  The set is the
 * points a case has to wait on rather than sleep before, which are the
 * three of the /repl transfer; anything else answers 0.
 */
uvlong
srvheld(Srvctx *c, char *name)
{
	uvlong n;

	n = 0;
	qlock(&c->holdlk);
	if(strcmp(name, "fullhold") == 0)
		n = c->fullheld;
	else if(strcmp(name, "openhold") == 0)
		n = c->openheld;
	else if(strcmp(name, "finalhold") == 0)
		n = c->finalheld;
	qunlock(&c->holdlk);
	return n;
}

/* which queue of the pool an oid lands in (srv.h) */
int
srvqindex(Srvctx *c, uchar *oid, int oidlen)
{
	if(c->nq <= 0)
		return -1;
	return oidhash(oid, oidlen) % c->nq;
}

/* what a point is set to, for the one place that acts on the value */
uvlong
srvpoint(Srvctx *c, char *name)
{
	uvlong n;

	n = 0;
	qlock(&c->holdlk);
	if(strcmp(name, "mapopen") == 0)
		n = c->mapopen;
	qunlock(&c->holdlk);
	return n;
}
