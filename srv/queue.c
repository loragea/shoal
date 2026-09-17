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
 * count of its own, so this file counts the pushes and the completions.
 */

static void	qhold(Srvctx*, Qreq*, uvlong*);

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

	c = qr->ctx;
	lock(&c->cntlk);
	c->ndone++;
	unlock(&c->cntlk);
}

/*
 * Push r onto the queue oid hashes to.  The Reqqueue is kept in
 * Req.aux (§7) so Srv.flush can find it; the handler and, for a ctl
 * verb, its parsed line ride along, and srvdestroyreq frees the lot
 * when lib9p frees the Req — after any parked Rflush has been
 * answered, so a Tflush that finds the request in the pool always
 * finds this beside it.
 */
Qreq*
srvqprep(Srvctx *c, uchar *oid, int oidlen, Req *r, void (*f)(Req*))
{
	Qreq *qr;

	if((qr = mallocz(sizeof *qr, 1)) == nil){
		respond(r, "shoalsrv: out of memory");
		return nil;
	}
	qr->ctx = c;
	qr->f = f;
	qr->q = c->q[oidhash(oid, oidlen) % c->nq];
	if(oidlen > 0 && oidlen <= Oidmax){
		memmove(qr->oid, oid, oidlen);
		qr->oidlen = oidlen;
	}
	r->aux = qr;
	return qr;
}

/*
 * The push is separate from the preparation so that a caller with more
 * to say — a ctl verb parsed on the service loop and run on the queue
 * — can fill the Qreq before the queue proc can see it.  After this
 * call the request is the queue's and nothing here may touch it.
 */
void
srvqgo(Srvctx *c, Req *r)
{
	Qreq *qr;

	qr = r->aux;
	lock(&c->cntlk);
	c->npush++;
	unlock(&c->cntlk);
	reqqueuepush(qr->q, r, qrun);
}

void
srvqpush(Srvctx *c, uchar *oid, int oidlen, Req *r, void (*f)(Req*))
{
	if(srvqprep(c, oid, oidlen, r, f) != nil)
		srvqgo(c, r);
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
 */
void
srvqflush(Req *r)
{
	Qreq *qr;

	if(r->oldreq == nil || (qr = r->oldreq->aux) == nil){
		respond(r, nil);
		return;
	}
	qhold(qr->ctx, nil, &qr->ctx->flushhold);
	qlock(&qr->lk);
	if(!qr->done)
		reqqueueflush(qr->q, r->oldreq);
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
 * running can be made to stay running.  A hold ends when its point is
 * cleared or when this queue's flush flag is set, whichever is first,
 * so a held request is still flushable and the shutdown that clears
 * every point (srvholdclear) is not held up by one.
 */
static void
qhold(Srvctx *c, Qreq *qr, uvlong *pt)
{
	qlock(&c->holdlk);
	while(*pt != 0 && (qr == nil || qr->q->flush == 0)){
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
	qhold(qr->ctx, qr, &qr->ctx->hold);
	return qr->q->flush != 0;
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
	qhold(qr->ctx, qr, &qr->ctx->exithold);
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
 *	discard the stage — nothing is staged here.  The stage belongs
 *		to a fid's write path (lib/shoal.h's stageopen and
 *		stagediscard), which is the object-I/O surface; that
 *		surface fills the flushed fid's auxflush cell (dat.h) and
 *		this function calls it, so the discard is added without
 *		touching this file.
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
void
srvstep7(Req *r)
{
	Sfid *f;

	if(r->fid == nil || (f = r->fid->aux) == nil)
		return;
	if(f->auxflush != nil)
		f->auxflush(f, r);
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
	int flushed;

	if((qr = r->aux) != nil){
		/*
		 * Marked answered before it answers, and the flag read
		 * under the same lock: a Tflush that arrives after this
		 * takes the Rflush alone (srvqflush), and one that got in
		 * first is seen here.
		 */
		qlock(&qr->lk);
		qr->done = 1;
		flushed = qr->q->flush != 0;
		qunlock(&qr->lk);
		if(flushed || srvintr(err)){
			srvstep7(r);
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
	else if(strcmp(name, "objexit") == 0)
		c->exithold = n;
	else if(strcmp(name, "flushhold") == 0)
		c->flushhold = n;
	qunlock(&c->holdlk);
}
