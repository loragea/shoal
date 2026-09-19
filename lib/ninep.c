#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"

/*
 * A 9P client, docs/design/store.md §12.
 *
 * It speaks stock 9P2000 over a pair of file descriptors with
 * convS2M/convM2S, and it holds no shoal semantics at all: there is no
 * map here, no `op=' grammar, no retry policy and no idea what a peer
 * is.  Those belong to the monitor poll loop and the peer clients that
 * layer-a §5.5, §5.6 and §6.3 define; this is the engine each of them
 * needs first.
 *
 * **Nothing here dials.**  A connection is made by a caller-supplied
 * callback that answers a pair of fds — the network's equivalent of
 * the device vtable (§0), and for the same reason: it is what lets a
 * T1 program run two instances and a monitor inside itself over pipes
 * with no network at all, and a real dial answers the same fd twice.
 * Reconnection is the caller's too.  What this client owes the caller
 * is to tell a dead connection from a §2.6 refusal, which it does with
 * distinct outcomes rather than with a string the caller must parse.
 *
 * **Every exchange is bounded.**  layer-a §5.4 bounds each peer
 * operation by `replms' and §5.4's write path must not block a client
 * on a dead peer, so every call takes a deadline in milliseconds and
 * answers Ninetimeout when it passes.  Getting that bound out of plain
 * libc is this file's one real design problem, and the shape it takes
 * is below.
 *
 * **The bounded wait.**  Two procs per connection, made through a
 * spawn callback exactly as the store engine makes its own (§7): a
 * T1 program passes an rfork(RFPROC|RFMEM) wrapper and the server
 * passes proccreate.
 *
 *	the reader	parks in read(2), demultiplexes each reply by tag
 *			into that tag's slot and wakes the waiter.  It
 *			reads only while a request is ON THE WIRE — with
 *			nothing outstanding it parks on `rdrz' instead, so
 *			an idle connection can be closed without waiting
 *			for the peer to say anything.  The wake is in
 *			nineput and not where the slot is armed, because a
 *			request that never reaches the wire is one the
 *			reader must not have left the park for.
 *	the timer	wakes every `tickms', and settles every slot whose
 *			deadline has passed.
 *
 * A waiter, the reader and the timer coordinate under one QLock, and
 * the slot's state is what makes exactly one of them the waker: a slot
 * is Nsent from the moment its request is written until whichever of
 * the reader and the timer reaches it first moves it to Ndone under
 * the lock and calls rwakeup.  The other then finds a slot that is no
 * longer Nsent and does nothing.  The waiter parks in rsleep on the
 * slot's own Rendez, which releases the lock while it sleeps, and
 * re-tests the state on waking, so a reply that lands before the
 * waiter gets there is not lost.
 *
 * **Why QLock and Rendez rather than rendezvous(2).**  The engine
 * states the rule this library lives under (§7): QLock, Rendez and
 * Lock mean the same thing under plain libc and under libthread, so
 * one library serves the T1 programs and the 9P server.
 * `rendezvous' does not: libthread supplies its own, which rendezvous
 * BETWEEN THREADS of one program and answers ~0 to a broken sleep,
 * while libc's is the kernel call between procs of a rendezvous group.
 * The two would have this file mean different things in its two
 * homes.  Neither is `alarm' or a note used here, for the reason the
 * engine gives for its own procs: a note delivered to a proc of a
 * libthread program lands in libthread's handler, and an alarm is one
 * timer shared with whatever else the program is doing.
 *
 * **What a timeout leaves behind** is store.md §14(49).  In one line:
 * the call answers Ninetimeout, the client sends a Tflush naming the
 * tag, and the tag is not reused until the Rflush for it comes back —
 * a late reply for it is read off the wire and discarded (nineheld and
 * ninelate are what a caller sees of both).
 *
 * **One outstanding request per fid** (layer-a §5.6) is enforced here,
 * over every fid and not only over an `/rpc' one: a second request on
 * a fid that already has one outstanding is refused Ninebusy before
 * anything is written.  §14(51) argues the widening.
 *
 * **Closing, and the one thing plain libc cannot do.**  nineclose
 * answers every exchange in flight Ninedead and stops both procs; the
 * last of the caller, the two procs and any exchange still unwinding
 * releases the memory and the fds, and the `freed' callback is the
 * observation of that moment (§13's hook, as the engine has one).  A
 * caller must not START a call after nineclose, exactly as storeclose
 * requires; a call already in flight when it runs is safe, and that is
 * what the reference count is for.
 *
 * A proc parked in read(2) or write(2) cannot be recalled from inside
 * this library: the only mechanism is a note, and notes are ruled out
 * above for exactly these procs.  The transport's owner CAN recall
 * one, because it is the owner of the fds — a network connection
 * takes `hangup' written to its ctl file — so that is a third
 * callback, `Ninecfg.hangup', whose contract is in lib/shoal.h,
 * including what a pipe pair has to do instead of a close to avoid
 * killing the parked proc outright.  nineclose calls it, a nineopen
 * that fails calls it through nineclose, and the timer calls it when
 * a write has stalled past its deadline.  Without one, a close waits
 * on the peer for as long as the peer takes (§14(50)).
 */

enum
{
	/* slot states */
	Nfree	= 0,	/* the slot is nobody's */
	Nsent,		/* a request is out and a waiter is in the exchange */
	Ndone,		/* the outcome is in; the waiter has not taken it */
	Ngone,		/* the waiter gave up: a late reply lands here */
	Nflushing,	/* a Tflush naming `old', which nobody waits for */

	Nminmsize	= 512 + IOHDRSZ,	/* the smallest this will work at */
	Nfidmax		= 2,			/* fids one request names: Twalk's two */

	/*
	 * The bound on the Tflush write a timed-out exchange leaves
	 * behind.  It has no exchange of its own to take a deadline from
	 * and nobody waits for it, so it takes a fixed one: long enough
	 * that an ordinary busy peer is never killed for a slow read of
	 * six bytes, short enough that a peer which has stopped reading
	 * altogether does not hold the write lock for longer than a call.
	 */
	Nflushwms	= 1000,
};

static char Eclosed[] = "ninep: the connection was closed";
static char Ehangup[] = "ninep: the peer hung up";
static char Etimeout[] = "ninep: no reply before the deadline";
static char Enomem[] = "ninep: out of memory";

typedef struct Nreq Nreq;

/*
 * One tag.  Slot 0 carries NOTAG, which is Tversion's alone; slots
 * 1..nreq are the request tags, and slot i+nreq is the tag the Tflush
 * for slot i is sent under, so a timed-out exchange always has a tag
 * to flush with and the two are found from each other by arithmetic.
 */
struct Nreq
{
	int	state;
	int	out;		/* Nineok … Ninelocal, once Ndone */
	int	owed;		/* the peer still owes a reply for this tag */
	int	nfid;		/* fids this request holds busy */
	ulong	fid[Nfidmax];
	ushort	tag;
	ushort	old;		/* Nflushing: the slot its Tflush names */
	vlong	deadline;	/* ms on the monotonic clock */
	uchar	*m;		/* the reply, as it came off the wire */
	int	nm;
	char	err[ERRMAX];
	Rendez	rz;		/* on Nine.lk: the waiter parks here */
};

struct Nine
{
	QLock	lk;		/* everything below, and every slot */
	Rendez	rdrz;		/* on lk: the reader waits for work here */
	QLock	wlk;		/* one writer on the fd at a time */

	int	infd, outfd;
	ulong	msize;		/* negotiated (§5.5), settled before we escape */
	ulong	bufsz;		/* proposed: what the buffers are sized for */
	ulong	tickms;
	int	nreq;		/* request tags: 1..nreq */
	int	nslot;		/* 2*nreq + 1 */
	Nreq	*req;
	uchar	*wbuf;		/* under wlk */
	uchar	*rbuf;		/* the reader's own */

	int	nexpect;	/* replies the peer still owes */
	int	dead;
	int	deadout;	/* Ninedead, or Ninebotch if 9P was broken */
	int	closed;
	char	deaderr[ERRMAX];
	uvlong	nlate;		/* replies discarded after a timeout */
	int	writing;	/* a write is inside write(2) right now */
	int	wtype;		/* ... of this T-message */
	vlong	wdeadline;	/* ... and it is stalled past this */
	uvlong	wseq;		/* ... and the mark is that write's, not another's */
	ulong	widenms;	/* the writewiden point: 0 unless a test set it */
	int	ref;
	int	nproc;		/* procs of this connection still running */
	void	(*hangup)(void*);	/* break the fds; set once, then read-only */
	void	*hanguparg;
	void	(*freed)(void*);
	void	*freedarg;
};

/*
 * Break every blocked read and write on the fds, so that a proc this
 * library parked in one comes back.  The callback may be called more
 * than once and is called with no lock of ours held, since what it
 * does is the transport owner's business and may block.  With no
 * callback there is nothing to do and the parked proc waits for the
 * peer.
 */
static void
ninehangup(Nine *c)
{
	if(c->hangup != nil)
		(*c->hangup)(c->hanguparg);
}

static vlong
nowms(void)
{
	return nsec()/1000000;
}

static void
setstr(char *buf, char *s)
{
	utfecpy(buf, buf+ERRMAX, s);
}

/*
 * The outcome of a call that never reached the wire, or that came back
 * as something other than the reply it asked for.  The string is also
 * left in the error string, so a caller that prints %r sees the same
 * thing a caller that reads Ninerep.err does.
 */
static int
nineout(Ninerep *r, int out, char *fmt, ...)
{
	va_list arg;

	va_start(arg, fmt);
	vseprint(r->err, r->err + sizeof r->err, fmt, arg);
	va_end(arg);
	r->out = out;
	werrstr("%s", r->err);
	return out;
}

/*
 * Give the memory back.  Only the last reference reaches here, so the
 * reader is out of its read and the timer out of its sleep by now and
 * the fds are nobody's but ours.  The `freed' callback is the last
 * act before the memory goes, which is what the header promises and
 * what makes the release observable to a test that has no other way
 * to see it.  It is nil until nineopen has a handle to hand back, so
 * a nineopen that answers nil is silent: the caller never held the
 * connection and has nothing to be told the end of (§14(50)).
 */
static void
ninefree(Nine *c)
{
	int i;

	if(c->infd >= 0)
		close(c->infd);
	if(c->outfd >= 0 && c->outfd != c->infd)
		close(c->outfd);
	if(c->req != nil)
		for(i = 0; i < c->nslot; i++)
			free(c->req[i].m);
	free(c->req);
	free(c->wbuf);
	free(c->rbuf);
	if(c->freed != nil)
		(*c->freed)(c->freedarg);
	free(c);
}

static void
ninedrop(Nine *c)
{
	int last;

	qlock(&c->lk);
	last = --c->ref == 0;
	qunlock(&c->lk);
	if(last)
		ninefree(c);
}

/*
 * The connection is gone: a read or a write failed, or the peer broke
 * the protocol.  Every waiter in an exchange is answered with the same
 * outcome and the same string, and nothing more is expected off the
 * wire.  `out' is what tells transport death from a protocol
 * violation, which is a distinction a caller acts on: a peer that
 * hangs up is one to dial again, and a peer that answers on a tag
 * nobody sent is not.  Called under the lock.
 */
static void
ninedied(Nine *c, int out, char *err)
{
	Nreq *q;
	int i;

	if(!c->dead){
		c->dead = 1;
		c->deadout = out;
		setstr(c->deaderr, err);
	}
	for(i = 0; i < c->nslot; i++){
		q = &c->req[i];
		q->owed = 0;
		if(q->state == Nsent){
			q->state = Ndone;
			q->out = c->deadout;
			setstr(q->err, c->deaderr);
			rwakeup(&q->rz);
		}
	}
	c->nexpect = 0;
	rwakeupall(&c->rdrz);
}

/*
 * Is a request outstanding on any of these fids?  A Twalk names two —
 * the fid it walks from and the newfid it walks to — and both are
 * held for the exchange, so that two concurrent walks cannot target
 * one newfid (§14(51)).  Answers 1 with the offending fid in *busy.
 * Called under the lock.
 */
static int
ninefidbusy(Nine *c, ulong *fid, int nfid, ulong *busy)
{
	Nreq *q;
	int i, j, k;

	for(i = 0; i < c->nslot; i++){
		q = &c->req[i];
		if(q->state == Nfree)
			continue;
		for(j = 0; j < q->nfid; j++)
			for(k = 0; k < nfid; k++)
				if(q->fid[j] == fid[k]){
					*busy = fid[k];
					return 1;
				}
	}
	return 0;
}

/* a free request slot, or -1.  Called under the lock. */
static int
ninetake(Nine *c, int notag)
{
	int i;

	if(notag)
		return c->req[0].state == Nfree ? 0 : -1;
	for(i = 1; i <= c->nreq; i++)
		if(c->req[i].state == Nfree && c->req[i+c->nreq].state == Nfree)
			return i;
	return -1;
}

/*
 * One message off the wire, already read into rbuf and already judged
 * a legal length by ninereadmsg.  Called under the lock; answers 0 to
 * go on reading and -1 once the connection is gone.
 */
static int
ninegot(Nine *c, int n)
{
	char buf[ERRMAX];
	Nreq *q, *f;
	uchar *m;
	ushort tag;
	int i;

	tag = GBIT16(c->rbuf+BIT32SZ+BIT8SZ);
	i = tag == NOTAG ? 0 : tag;
	if(i < 0 || i >= c->nslot || c->req[i].tag != tag
		|| c->req[i].state == Nfree){
		ninedied(c, Ninebotch, "ninep: a reply nothing is waiting for");
		return -1;
	}
	q = &c->req[i];
	switch(q->state){
	case Nsent:
		if((m = malloc(n)) == nil){
			ninedied(c, Ninedead, Enomem);
			return -1;
		}
		memmove(m, c->rbuf, n);
		q->m = m;
		q->nm = n;
		q->out = Nineok;	/* the waiter decodes it and judges */
		q->state = Ndone;
		q->owed = 0;
		c->nexpect--;
		rwakeup(&q->rz);
		break;
	case Ndone:
	case Ngone:
		/*
		 * A reply for a tag the timer settled.  It is discarded
		 * where it arrives (§14(49)); the tag stays held until the
		 * Rflush for it, so nothing else can have claimed this
		 * reply.
		 */
		if(q->owed){
			q->owed = 0;
			c->nexpect--;
		}
		c->nlate++;
		break;
	case Nflushing:
		if(c->rbuf[BIT32SZ] != Rflush){
			snprint(buf, sizeof buf, "ninep: a Tflush answered"
				" with type %d", c->rbuf[BIT32SZ]);
			ninedied(c, Ninebotch, buf);
			return -1;
		}
		/*
		 * 9P has the server answer a flushed request before the
		 * Rflush or not at all, so the Rflush is where both tags
		 * come back: this one, and the one it named.
		 */
		f = &c->req[q->old];
		if(f->owed){
			f->owed = 0;
			c->nexpect--;
		}
		free(f->m);
		f->m = nil;
		f->state = Nfree;
		f->nfid = 0;
		q->owed = 0;
		c->nexpect--;
		q->state = Nfree;
		break;
	}
	return 0;
}

/*
 * One message into rbuf: read9pmsg(2)'s job, done here instead.
 * read9pmsg judges the length against the buffer it was given and
 * answers a message longer than it with the same -1 a broken
 * transport gives, and those are two different outcomes to a caller
 * (§14(50)): a peer that hangs up is one to dial again, and a peer
 * that sends more than the negotiated msize has broken 9P and is not.
 * Judging the length here also makes the judgement the NEGOTIATED
 * msize's rather than the proposed buffer's, so a peer that lowered
 * the msize and one that did not are answered alike.
 *
 * Answers the message's length, 0 at end of file, or -1 with the
 * reason in e and *botch set for a protocol violation and clear for
 * transport death.  `msize' is settled once, in nineopen, before the
 * handle escapes to its caller, and is read without the lock
 * everywhere afterwards; this one is passed in because the reader
 * has it to hand from the loop it came out of.
 */
static int
ninereadmsg(Nine *c, ulong msize, int *botch, char *e, int ne)
{
	ulong len;
	int n;

	*botch = 0;
	if((n = readn(c->infd, c->rbuf, BIT32SZ)) != BIT32SZ){
		if(n == 0)
			return 0;
		if(n < 0)
			snprint(e, ne, "ninep: %r");
		else
			snprint(e, ne, "ninep: %d bytes of a reply header", n);
		return -1;
	}
	len = GBIT32(c->rbuf);
	if(len > msize){
		*botch = 1;
		snprint(e, ne, "ninep: a reply of %lud bytes over the negotiated"
			" msize %lud", len, msize);
		return -1;
	}
	if(len < BIT32SZ+BIT8SZ+BIT16SZ){
		*botch = 1;
		snprint(e, ne, "ninep: a short reply");
		return -1;
	}
	n = readn(c->infd, c->rbuf+BIT32SZ, len-BIT32SZ);
	if(n < 0){
		snprint(e, ne, "ninep: %r");
		return -1;
	}
	if((ulong)n < len-BIT32SZ)	/* end of file inside a message */
		return 0;
	return len;
}

/*
 * The reader proc.  It reads only while a request is on the wire, so a
 * connection with nothing outstanding is one whose reader is parked on
 * a Rendez and can be stopped at once; a reader inside read(2) comes
 * back only when the peer speaks or `hangup' breaks the fds (§14(50)).
 */
static void
ninereader(void *a)
{
	char e[ERRMAX];
	Nine *c;
	ulong msize;
	int n, botch;

	c = a;
	qlock(&c->lk);
	for(;;){
		while(c->nexpect == 0 && !c->closed && !c->dead)
			rsleep(&c->rdrz);
		if(c->closed || c->dead)
			break;
		msize = c->msize;
		qunlock(&c->lk);
		n = ninereadmsg(c, msize, &botch, e, sizeof e);
		qlock(&c->lk);
		/*
		 * Dead as well as closed: the timer kills a connection whose
		 * write stalled while this proc is inside the read, and
		 * ninegot on a dead connection would count a reply against
		 * slots ninedied has already settled and a nexpect it has
		 * already zeroed.
		 */
		if(c->closed || c->dead)
			break;
		if(n == 0){
			ninedied(c, Ninedead, Ehangup);
			break;
		}
		if(n < 0){
			ninedied(c, botch ? Ninebotch : Ninedead, e);
			break;
		}
		if(ninegot(c, n) < 0)
			break;
	}
	c->nproc--;
	qunlock(&c->lk);
	ninedrop(c);
}

/*
 * The timer proc.  It owns no deadline of its own: it looks every
 * tickms and settles whatever has expired, so a call comes back within
 * its deadline plus one tick plus the cost of writing the Tflush.
 *
 * A write stalled past its deadline is settled first and differently.
 * A peer that will not ACCEPT a message within the exchange's deadline
 * cannot be waited on — there is no tag to flush, since nothing was
 * ever sent, and every other exchange is piled up behind the write
 * lock — so it is dead for this client's purposes: the connection
 * dies Ninedead, and `hangup' is what brings the writer back out of
 * write(2) to find that out (§14(49)).  The mark this reads is the
 * outstanding write's own (nineput), so what it judges is never a
 * deadline left behind by a write that already came back.
 */
static void
ninetimer(void *a)
{
	char buf[ERRMAX];
	Nine *c;
	Nreq *q;
	vlong now;
	int i, hang;

	c = a;
	for(;;){
		sleep(c->tickms);
		qlock(&c->lk);
		if(c->closed || c->dead)
			break;
		now = nowms();
		hang = 0;
		if(c->writing && c->wdeadline <= now){
			snprint(buf, sizeof buf, "ninep: a T%d the peer would not"
				" accept before the deadline", c->wtype);
			ninedied(c, Ninedead, buf);
			hang = 1;
		}
		for(i = 0; i < c->nslot; i++){
			q = &c->req[i];
			if(q->state == Nsent && q->deadline <= now){
				q->state = Ndone;
				q->out = Ninetimeout;
				setstr(q->err, Etimeout);
				rwakeup(&q->rz);
			}
		}
		qunlock(&c->lk);
		if(hang)
			ninehangup(c);
	}
	c->nproc--;
	qunlock(&c->lk);
	ninedrop(c);
}

/*
 * Put one message on the wire, under the write lock so that two procs
 * cannot interleave their bytes.  Answers 0, or -1 with the reason in
 * buf: a message that will not fit the negotiated msize is a local
 * refusal and a failed write is the connection's death, and the caller
 * tells them apart by whether the connection is dead afterwards.
 *
 * The reader is woken HERE and not where the slot was armed: until
 * the bytes are on the wire the peer owes nothing, and a reader sent
 * into read(2) for a request that then never leaves is a reader
 * nothing can recall.
 *
 * `deadline' is the point past which this write is stalled and the
 * peer is dead for this client's purposes (§14(49)).  It is recorded
 * under lk before the write, because write(2) blocks for as long as
 * the peer declines to read and nothing else here would bound it:
 * the timer is what notices, kills the connection and breaks the fds
 * under it, and this proc then finds a write that failed.
 *
 * The mark it is recorded in is one per connection — there is one
 * writer at a time — but the connection has many writers over its
 * life, and they hand the write lock from one to the next, so the
 * mark is OWNED: it carries this write's generation, it is settled
 * before the write lock goes rather than after, and a writer clears
 * it only while it is still the writer's own.  Both halves are
 * needed.  Without the first, a writer that released the lock can be
 * overtaken by the next one and clear ITS mark, and a stalled write
 * with no mark is the unbounded write this bound exists to prevent —
 * which is every time two exchanges overlap.  Without the second, a
 * clear from further off does the same.  What the timer sees is the
 * deadline of the write outstanding now, or no write at all.
 *
 * `widenms' is §13's writewiden point (ninehook), inert unless a T1
 * case set it: it parks a writer between its write(2) and the
 * settling of its mark, which is the one place two writers can be
 * ordered through this window from outside.
 */
static int
nineput(Nine *c, Fcall *t, char *buf, int nbuf, vlong deadline)
{
	uvlong seq;
	ulong widen;
	int n, ok;

	qlock(&c->wlk);
	if((n = convS2M(t, c->wbuf, c->msize)) <= 0){
		qunlock(&c->wlk);
		snprint(buf, nbuf, "ninep: a T%d does not fit the negotiated"
			" msize %lud", t->type, c->msize);
		return -1;
	}
	qlock(&c->lk);
	seq = ++c->wseq;
	c->writing = 1;
	c->wtype = t->type;
	c->wdeadline = deadline;
	widen = c->widenms;
	qunlock(&c->lk);
	ok = write(c->outfd, c->wbuf, n) == n;
	if(!ok)
		snprint(buf, nbuf, "ninep: writing a T%d: %r", t->type);
	if(widen > 0)
		sleep(widen);
	qlock(&c->lk);
	if(c->wseq == seq)		/* the mark is still this write's */
		c->writing = 0;
	if(ok)
		rwakeup(&c->rdrz);	/* now the peer owes a reply */
	else
		ninedied(c, Ninedead, buf);	/* keeps an earlier reason */
	qunlock(&c->lk);
	qunlock(&c->wlk);
	return ok ? 0 : -1;
}

/*
 * The Tflush a timed-out exchange leaves behind.  Its slot is armed
 * under the lock by the waiter, so the reader can already see it when
 * the Rflush arrives; the write itself is outside the lock like every
 * other.  A failed write is the connection's death, which releases the
 * tags with everything else, and a write the peer will not accept
 * within Nflushwms is the same death by the timer's hand — without
 * that bound this call, which is on the timeout path, could itself
 * block for ever and hold the write lock while it did.
 */
static void
nineflushtag(Nine *c, int i)
{
	char buf[ERRMAX];
	Fcall t;

	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = c->req[i + c->nreq].tag;
	t.oldtag = c->req[i].tag;
	nineput(c, &t, buf, sizeof buf, nowms() + Nflushwms);
}

/*
 * One exchange: write the request, wait for its reply until the
 * deadline, and answer which of the outcomes it came to.  On Nineok
 * *mp is the reply as it came off the wire and f points into it — the
 * decoded strings live in those bytes — so the wrapper copies out what
 * its caller asked for and frees it.
 */
static int
ninerpc(Nine *c, Fcall *t, ulong *fid, int nfid, int ms, Ninerep *r,
	Fcall *f, uchar **mp)
{
	char buf[ERRMAX];
	ulong busy;
	Nreq *q;
	uchar *m;
	int i, j, out, nm;

	memset(r, 0, sizeof *r);
	*mp = nil;
	qlock(&c->lk);
	if(c->closed || c->dead){
		out = nineout(r, c->dead ? c->deadout : Ninedead, "%s",
			c->dead ? c->deaderr : Eclosed);
		qunlock(&c->lk);
		return out;
	}
	c->ref++;			/* this exchange holds the handle up */
	if(nfid > 0 && ninefidbusy(c, fid, nfid, &busy)){
		out = nineout(r, Ninebusy, "ninep: fid %lud already has a"
			" request outstanding", busy);
		goto Drop;
	}
	if((i = ninetake(c, t->type == Tversion)) < 0){
		out = nineout(r, Ninelocal, "ninep: no free tag");
		goto Drop;
	}
	q = &c->req[i];
	q->nfid = nfid;
	for(j = 0; j < nfid; j++)
		q->fid[j] = fid[j];
	q->deadline = nowms() + ms;
	q->out = Nineok;
	q->err[0] = 0;
	q->state = Nsent;
	q->owed = 1;
	c->nexpect++;
	t->tag = q->tag;
	r->tag = q->tag;
	qunlock(&c->lk);

	if(nineput(c, t, buf, sizeof buf, q->deadline) < 0){
		qlock(&c->lk);
		/*
		 * A message the negotiated msize will not hold never
		 * reached the wire, so nothing is owed for this tag and the
		 * refusal is local.  A write that FAILED killed the
		 * connection, and ninedied has already settled this slot
		 * with every other waiter's, so that case waits below and
		 * collects the Ninedead it left.
		 */
		if(!c->dead){
			if(q->owed){
				q->owed = 0;
				c->nexpect--;
			}
			q->state = Nfree;
			q->nfid = 0;
			out = nineout(r, Ninelocal, "%s", buf);
			goto Drop;
		}
	}else
		qlock(&c->lk);
	while(q->state != Ndone)
		rsleep(&q->rz);
	out = q->out;
	setstr(r->err, q->err);
	m = q->m;
	nm = q->nm;
	q->m = nil;
	if(out == Ninetimeout){
		/*
		 * The tag is not free: the peer may still answer it, and a
		 * reply read against a tag handed out again would be
		 * collected as the next exchange's.  It comes back with the
		 * Rflush (§14(49)), and so does the fid.
		 */
		q->state = Ngone;
		if(i > 0){
			c->req[i + c->nreq].state = Nflushing;
			c->req[i + c->nreq].old = i;
			c->req[i + c->nreq].owed = 1;
			c->req[i + c->nreq].nfid = 0;
			c->nexpect++;
		}else
			ninedied(c, Ninedead,
				"ninep: no Rversion before the deadline");
		qunlock(&c->lk);
		if(i > 0)
			nineflushtag(c, i);
		werrstr("%s", r->err);
		r->out = out;
		ninedrop(c);
		return out;
	}
	q->state = Nfree;
	q->nfid = 0;
	qunlock(&c->lk);

	if(out != Nineok){
		free(m);
		r->out = out;
		werrstr("%s", r->err);
		ninedrop(c);
		return out;
	}
	if(convM2S(m, nm, f) != nm){
		free(m);
		snprint(buf, sizeof buf, "ninep: a reply that will not decode");
		qlock(&c->lk);
		ninedied(c, Ninebotch, buf);
		qunlock(&c->lk);
		out = nineout(r, Ninebotch, "%s", buf);
		ninedrop(c);
		return out;
	}
	if(f->type == Rerror){
		/* §2.6's string, verbatim: nothing here adds to it */
		setstr(r->err, f->ename);
		free(m);
		r->out = Nineerr;
		werrstr("%s", r->err);
		ninedrop(c);
		return Nineerr;
	}
	if(f->type != t->type+1){
		snprint(buf, sizeof buf, "ninep: a reply of type %d to a T%d",
			f->type, t->type);
		free(m);
		qlock(&c->lk);
		ninedied(c, Ninebotch, buf);
		qunlock(&c->lk);
		out = nineout(r, Ninebotch, "%s", buf);
		ninedrop(c);
		return out;
	}
	*mp = m;
	r->out = Nineok;
	ninedrop(c);
	return Nineok;

Drop:
	qunlock(&c->lk);
	ninedrop(c);
	return out;
}

Nine*
nineopen(Ninecfg *cfg)
{
	char e[ERRMAX];
	Nine *c;
	Ninerep r;
	Fcall t, f;
	uchar *m;
	int i, ms;

	if(cfg == nil || cfg->connect == nil || cfg->spawn == nil){
		werrstr("ninep: no connect or spawn callback");
		return nil;
	}
	if((c = mallocz(sizeof *c, 1)) == nil)
		return nil;
	c->infd = c->outfd = -1;
	c->rdrz.l = &c->lk;
	c->ref = 1;
	c->hangup = cfg->hangup;
	c->hanguparg = cfg->hanguparg;
	c->bufsz = cfg->msize != 0 ? cfg->msize : Ninemsizedflt;
	if(c->bufsz < Nminmsize)
		c->bufsz = Nminmsize;
	c->msize = c->bufsz;
	c->tickms = cfg->tickms != 0 ? cfg->tickms : Ninetickmsdflt;
	c->nreq = cfg->nreq != 0 ? cfg->nreq : Ninereqdflt;
	if(c->nreq > Ninereqmax)
		c->nreq = Ninereqmax;
	c->nslot = 2*c->nreq + 1;
	c->req = mallocz(c->nslot*sizeof(Nreq), 1);
	c->wbuf = malloc(c->bufsz);
	c->rbuf = malloc(c->bufsz);
	if(c->req == nil || c->wbuf == nil || c->rbuf == nil){
		ninefree(c);
		werrstr("%s", Enomem);
		return nil;
	}
	for(i = 0; i < c->nslot; i++){
		c->req[i].rz.l = &c->lk;
		c->req[i].tag = i == 0 ? NOTAG : i;
	}
	if((*cfg->connect)(cfg->connectarg, &c->infd, &c->outfd) < 0){
		rerrstr(e, sizeof e);
		ninefree(c);
		werrstr("%s", e);
		return nil;
	}

	/*
	 * Both procs hold a reference of their own, so a spawn that
	 * fails after the other succeeded is unwound by closing rather
	 * than by unpicking it here.  The reader is already running by the
	 * time the timer is spawned, so from here the count moves under
	 * the lock, as it does everywhere else.
	 */
	qlock(&c->lk);
	c->ref++;
	c->nproc++;
	qunlock(&c->lk);
	if((*cfg->spawn)(ninereader, c) < 0){
		qlock(&c->lk);
		c->ref--;
		c->nproc--;
		qunlock(&c->lk);
		rerrstr(e, sizeof e);
		nineclose(c);
		werrstr("ninep: cannot start the reader proc: %s", e);
		return nil;
	}
	qlock(&c->lk);
	c->ref++;
	c->nproc++;
	qunlock(&c->lk);
	if((*cfg->spawn)(ninetimer, c) < 0){
		qlock(&c->lk);
		c->ref--;
		c->nproc--;
		qunlock(&c->lk);
		rerrstr(e, sizeof e);
		nineclose(c);
		werrstr("ninep: cannot start the timer proc: %s", e);
		return nil;
	}

	ms = cfg->openms != 0 ? cfg->openms : Nineopenmsdflt;
	memset(&t, 0, sizeof t);
	t.type = Tversion;
	t.msize = c->bufsz;
	t.version = "9P2000";
	if(ninerpc(c, &t, nil, 0, ms, &r, &f, &m) != Nineok){
		nineclose(c);
		werrstr("%s", r.err);
		return nil;
	}
	/*
	 * layer-a §5.5 makes the floor an instance's own policy to
	 * refuse at; what is owed here is the negotiated number, so a
	 * size below the floor is reported and not refused (§14(48)).
	 * A size ABOVE what was proposed, or a version this client did
	 * not offer, is the peer breaking 9P and is refused.
	 */
	if(strcmp(f.version, "9P2000") != 0 || f.msize > c->bufsz
		|| f.msize < Nminmsize){
		snprint(e, sizeof e, "ninep: the peer offers version %s at"
			" msize %ud", f.version, f.msize);
		free(m);
		nineclose(c);
		werrstr("%s", e);
		return nil;
	}
	c->msize = f.msize;
	free(m);
	/*
	 * The handle is the caller's from here, and only from here is
	 * there anything for `freed' to observe the end of.  Every path
	 * above answers nil, and a caller that never held a connection is
	 * told nothing about its release (§14(50)).
	 */
	qlock(&c->lk);
	c->freed = cfg->freed;
	c->freedarg = cfg->freedarg;
	qunlock(&c->lk);
	return c;
}

void
nineclose(Nine *c)
{
	Nreq *q;
	int i;

	if(c == nil)
		return;
	qlock(&c->lk);
	c->closed = 1;
	for(i = 0; i < c->nslot; i++){
		q = &c->req[i];
		q->owed = 0;
		if(q->state == Nsent){
			q->state = Ndone;
			q->out = Ninedead;
			setstr(q->err, Eclosed);
			rwakeup(&q->rz);
		}
	}
	c->nexpect = 0;
	rwakeupall(&c->rdrz);
	qunlock(&c->lk);
	/*
	 * The reader may be inside read(2) and a writer inside write(2);
	 * neither is recalled by anything this library can do, so the
	 * transport's owner is asked to break the fds under them.  With no
	 * callback they come back when the peer lets them, which is what
	 * defers the free (§14(50)).
	 */
	ninehangup(c);
	ninedrop(c);
}

ulong
ninemsize(Nine *c)
{
	return c->msize;
}

/*
 * Tags this connection cannot hand out: the exchanges in flight, plus
 * every tag a timed-out exchange is still holding against its Rflush
 * and the tag that Rflush will arrive on.  It is 0 on a connection
 * with nothing in flight, which is what a test asserts to see that a
 * timeout gave its tags back.
 */
int
nineheld(Nine *c)
{
	int i, n;

	n = 0;
	qlock(&c->lk);
	for(i = 0; i < c->nslot; i++)
		if(c->req[i].state != Nfree)
			n++;
	qunlock(&c->lk);
	return n;
}

/* replies that arrived for a tag whose exchange had already given up */
uvlong
ninelate(Nine *c)
{
	uvlong n;

	qlock(&c->lk);
	n = c->nlate;
	qunlock(&c->lk);
	return n;
}

/* §13's -X points, this library's set; inert unless a case sets one */
void
ninehook(Nine *c, char *name, uvlong n)
{
	if(strcmp(name, "writewiden") == 0){
		qlock(&c->lk);
		c->widenms = n;
		qunlock(&c->lk);
	}
}

int
nineattach(Nine *c, ulong fid, ulong afid, char *uname, char *aname, int ms,
	Ninerep *r)
{
	uchar *m;
	Fcall t, f;

	memset(&t, 0, sizeof t);
	t.type = Tattach;
	t.fid = fid;
	t.afid = afid;
	t.uname = uname;
	t.aname = aname;
	if(ninerpc(c, &t, &fid, 1, ms, r, &f, &m) != Nineok)
		return r->out;
	r->qid = f.qid;
	free(m);
	return Nineok;
}

/*
 * §2's Twalk.  Both fids are held busy for the exchange — the one
 * walked from and the newfid walked to — so that two walks cannot
 * target one newfid at once (§14(51)).
 *
 * A SHORT walk, which 9P answers with an Rwalk carrying fewer qids
 * than there were names, is not Nineok: nothing was created under
 * newfid, so there is nothing for the caller to clunk, and r->qid is
 * promised to be the last qid of a FULL walk.  Nor is it Nineerr,
 * which is §2.6's own string and nothing was refused in those words.
 * It is Ninelocal, naming how far the walk got, with the qids that
 * did come back in r->wqid and their number in r->nwqid for a caller
 * that wants the partial result.
 */
int
ninewalk(Nine *c, ulong fid, ulong newfid, char **name, int nname, int ms,
	Ninerep *r)
{
	ulong w[Nfidmax];
	uchar *m;
	Fcall t, f;
	int i, nw;

	if(nname < 0 || nname > MAXWELEM){
		memset(r, 0, sizeof *r);
		return nineout(r, Ninelocal, "ninep: a walk of %d elements",
			nname);
	}
	memset(&t, 0, sizeof t);
	t.type = Twalk;
	t.fid = fid;
	t.newfid = newfid;
	t.nwname = nname;
	for(i = 0; i < nname; i++)
		t.wname[i] = name[i];
	w[0] = fid;
	w[1] = newfid;
	nw = newfid == fid ? 1 : 2;	/* a walk in place names one fid */
	if(ninerpc(c, &t, w, nw, ms, r, &f, &m) != Nineok)
		return r->out;
	r->nwqid = f.nwqid;
	for(i = 0; i < f.nwqid && i < MAXWELEM; i++)
		r->wqid[i] = f.wqid[i];
	free(m);
	if((int)f.nwqid < nname)
		return nineout(r, Ninelocal, "ninep: a walk of %d names got %d",
			nname, f.nwqid);
	if(f.nwqid > 0)
		r->qid = f.wqid[f.nwqid-1];
	return Nineok;
}

/*
 * §2's Topen.  It is spelled with the fid in the name because this
 * library's own open is the connection's.
 */
int
nineopenfid(Nine *c, ulong fid, int mode, int ms, Ninerep *r)
{
	uchar *m;
	Fcall t, f;

	memset(&t, 0, sizeof t);
	t.type = Topen;
	t.fid = fid;
	t.mode = mode;
	if(ninerpc(c, &t, &fid, 1, ms, r, &f, &m) != Nineok)
		return r->out;
	r->qid = f.qid;
	r->iounit = f.iounit;
	free(m);
	return Nineok;
}

int
ninecreate(Nine *c, ulong fid, char *name, ulong perm, int mode, int ms,
	Ninerep *r)
{
	uchar *m;
	Fcall t, f;

	memset(&t, 0, sizeof t);
	t.type = Tcreate;
	t.fid = fid;
	t.name = name;
	t.perm = perm;
	t.mode = mode;
	if(ninerpc(c, &t, &fid, 1, ms, r, &f, &m) != Nineok)
		return r->out;
	r->qid = f.qid;
	r->iounit = f.iounit;
	free(m);
	return Nineok;
}

/*
 * A read of at most n bytes into a, and a write of n bytes out of it.
 * Neither chunks: a count that will not fit the negotiated msize is
 * refused Ninelocal rather than shortened, because which of §2.4's
 * short forms the caller wants is the caller's to decide — the write
 * path shortens a write and a reader of `/rpc' MUST offer a whole
 * msize−IOHDRSZ (§5.6).  r->count is what the reply reported.
 */
/*
 * An Rread or Rwrite whose count overruns the request's is the peer
 * breaking 9P, not a refusal, so it kills the connection like every
 * other botch: a client that answered the NEXT call Nineok would be
 * handing the caller replies from a stream it no longer understands.
 * Called with the reply already freed, under no lock.
 */
static int
ninecountbotch(Nine *c, Ninerep *r, char *rep, ulong count, char *req, long n)
{
	char buf[ERRMAX];

	snprint(buf, sizeof buf, "ninep: an %s of %lud bytes for a %s of %ld",
		rep, count, req, n);
	qlock(&c->lk);
	ninedied(c, Ninebotch, buf);
	qunlock(&c->lk);
	return nineout(r, Ninebotch, "%s", buf);
}

int
nineread(Nine *c, ulong fid, vlong off, void *a, long n, int ms, Ninerep *r)
{
	uchar *m;
	Fcall t, f;

	if(n < 0 || (ulong)n > c->msize - IOHDRSZ){
		memset(r, 0, sizeof *r);
		return nineout(r, Ninelocal, "ninep: a read of %ld bytes over"
			" the negotiated msize %lud", n, c->msize);
	}
	memset(&t, 0, sizeof t);
	t.type = Tread;
	t.fid = fid;
	t.offset = off;
	t.count = n;
	if(ninerpc(c, &t, &fid, 1, ms, r, &f, &m) != Nineok)
		return r->out;
	if(f.count > (ulong)n){
		free(m);
		return ninecountbotch(c, r, "Rread", f.count, "Tread", n);
	}
	if(f.count > 0)
		memmove(a, f.data, f.count);
	r->count = f.count;
	free(m);
	return Nineok;
}

int
ninewrite(Nine *c, ulong fid, vlong off, void *a, long n, int ms, Ninerep *r)
{
	uchar *m;
	Fcall t, f;

	if(n < 0 || (ulong)n > c->msize - IOHDRSZ){
		memset(r, 0, sizeof *r);
		return nineout(r, Ninelocal, "ninep: a write of %ld bytes over"
			" the negotiated msize %lud", n, c->msize);
	}
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.fid = fid;
	t.offset = off;
	t.count = n;
	t.data = a;
	if(ninerpc(c, &t, &fid, 1, ms, r, &f, &m) != Nineok)
		return r->out;
	if(f.count > (ulong)n){
		free(m);
		return ninecountbotch(c, r, "Rwrite", f.count, "Twrite", n);
	}
	r->count = f.count;
	free(m);
	return Nineok;
}

int
nineclunk(Nine *c, ulong fid, int ms, Ninerep *r)
{
	uchar *m;
	Fcall t, f;

	memset(&t, 0, sizeof t);
	t.type = Tclunk;
	t.fid = fid;
	if(ninerpc(c, &t, &fid, 1, ms, r, &f, &m) != Nineok)
		return r->out;
	free(m);
	return Nineok;
}

int
nineremove(Nine *c, ulong fid, int ms, Ninerep *r)
{
	uchar *m;
	Fcall t, f;

	memset(&t, 0, sizeof t);
	t.type = Tremove;
	t.fid = fid;
	if(ninerpc(c, &t, &fid, 1, ms, r, &f, &m) != Nineok)
		return r->out;
	free(m);
	return Nineok;
}

/* the stat message into a, whose length lands in r->count */
int
ninestat(Nine *c, ulong fid, uchar *a, int n, int ms, Ninerep *r)
{
	uchar *m;
	Fcall t, f;

	memset(&t, 0, sizeof t);
	t.type = Tstat;
	t.fid = fid;
	if(ninerpc(c, &t, &fid, 1, ms, r, &f, &m) != Nineok)
		return r->out;
	if(n < 0 || f.nstat > (uint)n){
		free(m);
		return nineout(r, Ninelocal, "ninep: a stat of %ud bytes into"
			" %d", f.nstat, n);
	}
	memmove(a, f.stat, f.nstat);
	r->count = f.nstat;
	free(m);
	return Nineok;
}

/*
 * §5.4.1's Tflush, for a tag the caller names.  A timed-out exchange
 * flushes its own tag and needs nothing here (§14(49)); this is for a
 * caller that wants to flush a tag of its own choosing, and 9P has the
 * Rflush answered whether or not anything was outstanding under it.
 */
int
nineflush(Nine *c, ushort oldtag, int ms, Ninerep *r)
{
	uchar *m;
	Fcall t, f;

	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.oldtag = oldtag;
	if(ninerpc(c, &t, nil, 0, ms, r, &f, &m) != Nineok)
		return r->out;
	free(m);
	return Nineok;
}
