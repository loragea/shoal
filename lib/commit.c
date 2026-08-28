#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"
#include "store.h"

/*
 * The log commit path (docs/design/store.md §3.2), group commit and
 * the durable watermark (§7), and the coalescing flusher.
 *
 * A committing proc performs:
 *
 *	write the record's body sectors  — every sector but the first
 *	one device flush                 — covers the batch's grain
 *					   writes and the body
 *	one pwrite of the header sector  — the commit point
 *	one device flush                 — makes the record durable
 *
 * and then wakes the waiters.  For a record of one sector — the
 * common commit — the body is empty and the sequence is flush, write,
 * flush.  The header goes last unconditionally, which is what makes
 * the commit point one sector for any record size: a record that
 * landed only in part fails its checksum, a torn header fails it too,
 * and a header that did not land at all is not a valid header and
 * carries the wrong sequence number besides.
 *
 * **Group commit: the committer writes its own batch.**  There is no
 * assigner and no writer pool.  A worker takes qllog; if a batch can
 * be formed it becomes the committer, absorbing whatever is already
 * pending, stamping the batch with the next seq and log offset, and
 * releasing the lock — held for microseconds and never across I/O.
 * logdepth is a semaphore under qllog capping the batches in flight,
 * and that concurrency is the whole reason this design can claim more
 * than the durable writes per second one serial writer gets.
 *
 * **The durable watermark.**  A batch's members are woken only when
 * that batch's post-flush has returned, every lower-numbered batch's
 * has, and the batch has been applied to in-memory state.  Without
 * the ordering, a crash after batch n+1 landed and batch n did not
 * would leave replay stopping at n and discarding n+1 — an acked
 * write lost.  Including the apply is what lets §2.8 bind ckseq to
 * the watermark.
 *
 * **A batch that did not land condemns every batch above it.**  The
 * same argument holds with no crash at all: if batch n's record does
 * not become durable, replay stops at n, so n+1 is discarded however
 * well its own bytes landed.  So a failed log write records failseq
 * — the lowest seq that did not land — under qllog *before* it
 * releases the ordering, and every batch at or above failseq fails
 * with that device error instead of acking.  The store is then
 * `broken': it commits no more, in this process, ever.  Reads go on
 * being served, because in-memory state is exactly the last durable
 * record's — the failed batch and everything above it was never
 * applied — and the store is made whole by being closed and opened
 * again, which replays the log from the last checkpoint.
 *
 * Two counters, not one: relseq is the release order every batch
 * advances, and watermark is the highest *durably applied* seq, which
 * only a batch that landed advances.  §2.8 binds ckseq to the second,
 * so a checkpoint can never publish a mark past a record that never
 * became durable.
 */

/*
 * The flusher.  The raw channel's cdb/data/status triple is per-unit
 * kernel state, so it MUST NOT be interleaved: one flush at a time
 * owns it, and every flush — the committers', the checkpointer's, the
 * superblock publisher's — goes through here.  It coalesces: a caller
 * asks for "a flush that began after now", and one device flush
 * satisfies every caller waiting at the moment it is issued, which is
 * what keeps logdepth concurrent committers from costing logdepth
 * flushes.
 *
 * Flushes are cumulative, so a waiter woken by a later round than the
 * one it asked for is answered by that round: everything dirty when
 * it asked is durable once a later flush returns.
 */
int
flushnow(Store *s)
{
	uvlong my, round;
	int r;

	if(s->flushmode != Fraw){
		/*
		 * §3.2's -w: the operator asserts the unit is
		 * write-through or its cache disabled, so there is no
		 * flush to issue and the ordering argument rests entirely
		 * on that assertion.  T1.13, which asserts flush
		 * placement, does not apply to such a store.
		 */
		return 0;
	}
	qlock(&s->fllk);
	my = ++s->flasked;
	for(;;){
		if(s->fldone >= my){
			r = s->flerr;
			qunlock(&s->fllk);
			return r;
		}
		if(!s->flbusy){
			round = s->flasked;
			s->flbusy = 1;
			qunlock(&s->fllk);
			r = devflushretry(s->d);
			qlock(&s->fllk);
			if(round > s->fldone){
				s->fldone = round;
				s->flerr = r;
			}
			s->flbusy = 0;
			rwakeupall(&s->flrz);
			continue;
		}
		rsleep(&s->flrz);
	}
}

uvlong
logused(Store *s)
{
	if(s->logtail >= s->logstart)
		return s->logtail - s->logstart;
	return s->sb.logsecs - (s->logstart - s->logtail);
}

uvlong
logfree(Store *s)
{
	uvlong u;

	u = logused(s);
	if(u + 1 >= s->sb.logsecs)
		return 0;
	return s->sb.logsecs - u - 1;
}

static ulong
itembytes(Store *s, Item *it)
{
	ulong n;
	int i;

	USED(s);
	n = 0;
	if(it->obj != nil)
		n += objreclen(it->obj);
	for(i = 0; i < it->ndirty; i++)
		n += dirtyreclen(&it->dirty[i]);
	if(it->haseslot)
		n += Lenthdrsz + 4;
	return n;
}

/* caller holds qllog */
static void
unlink(Store *s, Item *it)
{
	Item **pp, *t;

	/*
	 * Only an item this call actually removed from the queue has its
	 * link cleared.  An item that is no longer pending is a link in
	 * some batch's item list, and clearing it there severs the
	 * batch: the entries after it are in the durable record and
	 * would never be applied or woken (§7).
	 */
	for(pp = &s->pend; (t = *pp) != nil; pp = &t->next)
		if(t == it){
			*pp = t->next;
			it->next = nil;
			break;
		}
	s->pendtail = nil;
	for(t = s->pend; t != nil; t = t->next)
		s->pendtail = t;
}

/*
 * Form a batch under qllog.  §6's reserved tail is a property of the
 * batch: the last logresv sectors of free log space are usable only
 * by commits that free space, so a batch that draws on the reserve
 * must contain only space-freeing commits.  Otherwise the reserve is
 * spent by exactly the traffic it exists to exclude, and a delete
 * blocked for log space cannot relieve the exhaustion that blocked
 * it — which is why a head that does not fit is stepped over rather
 * than waited on: "delete always works" is true only if a delete
 * further down the queue can still be committed.
 *
 * Returns nil, with *full set, when no pending item can be committed,
 * and nil with *oom set when it could not allocate — which is not a
 * wait for anything, so the caller must not sleep on it.
 */
static Batch*
formbatch(Store *s, int *full, int *oom)
{
	Batch *b;
	Item *it, *next, *last;
	uvlong avail, room, nsec, consume;
	ulong bytes;
	int freeing;

	*full = 0;
	*oom = 0;
	if(s->pend == nil)
		return nil;
	if((b = mallocz(sizeof *b, 1)) == nil){
		*oom = 1;
		return nil;
	}
	last = nil;
	bytes = Lrechdrsz;
	freeing = 1;
	for(it = s->pend; it != nil; it = next){
		next = it->next;
		if(b->items != nil && bytes + it->nbyte > s->sb.blksz)
			break;			/* one blksz of record body */
		nsec = (bytes + it->nbyte + s->sb.secsz - 1)/s->sb.secsz;
		room = s->sb.logsecs - s->logtail;
		consume = nsec <= room ? nsec : room + nsec;
		avail = logfree(s);
		if(!(freeing && it->freeing))
			avail = avail > s->logresv ? avail - s->logresv : 0;
		if(consume > avail){
			if(b->items != nil)
				break;
			continue;		/* step over: a freeing item
						 * further down may still fit */
		}
		unlink(s, it);
		if(last != nil)
			last->next = it;
		else
			b->items = it;
		last = it;
		it->batch = b;
		it->state = Ibatched;
		bytes += it->nbyte;
		if(!it->freeing)
			freeing = 0;
	}
	if(b->items == nil){
		*full = 1;
		free(b);
		return nil;
	}
	b->nsec = (bytes + s->sb.secsz - 1)/s->sb.secsz;
	room = s->sb.logsecs - s->logtail;
	/*
	 * A record MUST NOT straddle the end of the region, and the
	 * continuation rule is modular: a record that ends flush with
	 * the region end wraps by arithmetic and needs no marker.
	 * Fwrap covers the other case, where sectors remain but too few
	 * for the record.
	 */
	if(b->nsec <= room){
		b->wrap = 0;
		b->off = s->logtail;
		b->endoff = (s->logtail + b->nsec) % s->sb.logsecs;
	}else{
		b->wrap = 1;
		b->wrapoff = s->logtail;
		b->off = 0;
		b->endoff = b->nsec % s->sb.logsecs;
	}
	b->seqlo = s->seqnext;
	if(b->wrap)
		s->seqnext++;
	b->seqhi = s->seqnext++;
	s->logtail = b->endoff;
	return b;
}

/* pack a batch's entries after the record header */
static long
packbatch(Store *s, Batch *b, uchar *p, long max, ulong *nent)
{
	Item *it;
	long n, m;
	int i;

	USED(s);
	n = 0;
	*nent = 0;
	for(it = b->items; it != nil; it = it->next){
		if(it->obj != nil){
			if((m = objrecpack(p + n, max - n, it->obj)) < 0)
				return -1;
			n += m;
			(*nent)++;
		}
		for(i = 0; i < it->ndirty; i++){
			if((m = dirtyrecpack(p + n, max - n, &it->dirty[i])) < 0)
				return -1;
			n += m;
			(*nent)++;
		}
		if(it->haseslot){
			if((m = slotrecpack(p + n, max - n, it->eslot)) < 0)
				return -1;
			n += m;
			(*nent)++;
		}
	}
	return n;
}

/*
 * blksz-bounded pieces, which devwrite splits again if need be (§0).
 * body marks the one call that writes the record's body sectors:
 * §13's body:n is "after n body sectors", so the wrap record and the
 * header sector — which is the commit point itself — do not emit it.
 */
static int
logwrite(Store *s, uchar *p, ulong n, uvlong sec, int body)
{
	uvlong off;
	ulong m, done;

	off = s->sb.logoff*(uvlong)s->sb.secsz + sec*(uvlong)s->sb.secsz;
	for(done = 0; done < n; done += m){
		m = n - done;
		if(m > s->sb.blksz)
			m = s->sb.blksz;
		if(devwriteretry(s->d, p + done, m, off + done) < 0)
			return -1;
		if(body)
			devpoint(s->d, "body", (done + m)/s->sb.secsz);
	}
	return 0;
}

/*
 * Write one batch's record.  The wrap record, when one is needed,
 * belongs to the batch it precedes and rides before the batch's
 * post-flush, so one flush makes both durable.  Written after it, the
 * wrap record would be a link in the log's own continuity that a
 * crash can drop: replay would apply the last record before the end,
 * continue at +nsec into the index region, and discard every commit
 * written after the wrap.
 */
static int
writerec(Store *s, Batch *b, uchar *p)
{
	uchar *w;
	Lrec r;

	if(b->wrap){
		if((w = mallocz(s->sb.secsz, 1)) == nil)
			return -1;
		memset(&r, 0, sizeof r);
		r.vers = Storevers;
		r.nsec = 1;
		r.seq = b->seqlo;
		r.time = time(nil);
		r.nent = 0;
		r.flags = Fwrap;
		lrecpack(w, &r, s->sb.secsz);
		if(logwrite(s, w, s->sb.secsz, b->wrapoff, 0) < 0){
			free(w);
			return -1;
		}
		free(w);
	}
	if(b->nsec > 1
	&& logwrite(s, p + s->sb.secsz, (b->nsec-1)*s->sb.secsz, b->off+1, 1) < 0)
		return -1;
	devpoint(s->d, "precommit", 0);
	if(flushnow(s) < 0)
		return -1;
	devpoint(s->d, "commit", 0);
	if(logwrite(s, p, s->sb.secsz, b->off, 0) < 0)
		return -1;
	devpoint(s->d, "postwrite", 0);
	if(flushnow(s) < 0)
		return -1;
	return 0;
}

static void
seterr(Batch *b, char *e)
{
	Item *it;

	for(it = b->items; it != nil; it = it->next)
		strecpy(it->err, it->err + sizeof it->err, e);
}

/*
 * Apply the whole batch's entries — the committer's own and its
 * batch-mates' — through the same function §5's replay uses, over
 * extent maps the stages pinned, so no part of the apply faults.
 *
 * **The apply cannot fail here.**  Everything applyrec, applydirty
 * and applyslot can refuse — every range check, the pinned-map rule
 * of §2.7 clause 2, the entry's oid buffer and the dirty set's
 * records — is checked and allocated by itemok/itemprep before the
 * item joins a batch, which is before any of the record is written.
 * That is the whole reason those checks are there and not here: a
 * refusal after the post-flush is a durable record the store has
 * chosen not to believe, and the next start believes it.
 */
static int
applybatch(Store *s, Batch *b)
{
	Item *it;
	int i, r;

	r = 0;
	for(it = b->items; it != nil; it = it->next){
		if(it->obj != nil && applyrec(s, it->obj, it->emap) < 0)
			r = -1;
		for(i = 0; i < it->ndirty; i++)
			if(applydirty(s, &it->dirty[i],
				it->spare != nil ? &it->spare[i] : nil) < 0)
				r = -1;
		if(it->haseslot && applyslot(s, it->eslot) < 0)
			r = -1;
	}
	return r;
}

static void
askcheckpoint(Store *s)
{
	qlock(&s->cklk);
	s->ckreq++;
	rwakeupall(&s->ckwork);
	qunlock(&s->cklk);
}

static void
runbatch(Store *s, Batch *b)
{
	Item *m, *next;
	uchar *p;
	Lrec r;
	ulong bytes, nent;
	char e[ERRMAX];
	int err;

	bytes = Lrechdrsz;
	for(m = b->items; m != nil; m = m->next)
		bytes += m->nbyte;
	err = 0;
	strecpy(e, e + sizeof e, "log record could not be built");
	if((p = mallocz(b->nsec*s->sb.secsz, 1)) == nil)
		err = -1;
	if(err == 0
	&& packbatch(s, b, p + Lrechdrsz, b->nsec*s->sb.secsz - Lrechdrsz,
		&nent) < 0)
		err = -1;
	if(err == 0){
		memset(&r, 0, sizeof r);
		r.vers = Storevers;
		r.nsec = b->nsec;
		r.seq = b->seqhi;
		r.time = time(nil);
		r.nent = nent;
		r.flags = 0;
		lrecpack(p, &r, s->sb.secsz);
		/* §13's batch:n — hold this batch and let a later one land */
		qlock(&s->qllog);
		while(s->holdseq != 0 && s->holdseq == b->seqhi && !s->stop)
			rsleep(&s->holdrz);
		qunlock(&s->qllog);
		if(writerec(s, b, p) < 0){
			err = -1;
			rerrstr(e, sizeof e);
		}
	}
	free(p);

	/*
	 * A batch that did not become durable condemns the store and
	 * every batch above it: replay stops at the first record that is
	 * not there, so an acked write above it would be lost.  failseq
	 * is recorded before the release order moves, so a higher batch
	 * parked below cannot slip past it.
	 */
	qlock(&s->qllog);
	if(err < 0){
		s->broken = 1;
		if(s->failseq == 0 || b->seqlo < s->failseq){
			s->failseq = b->seqlo;
			strecpy(s->failerr, s->failerr + sizeof s->failerr, e);
		}
	}
	while(s->relseq + 1 != b->seqlo)
		rsleep(&s->relrz);
	if(err == 0 && s->failseq != 0 && b->seqhi >= s->failseq){
		err = -1;
		strecpy(e, e + sizeof e, s->failerr);
	}
	qunlock(&s->qllog);
	if(err < 0)
		seterr(b, e);

	if(err == 0){
		qlock(&s->qlstate);
		if(applybatch(s, b) < 0){
			/*
			 * Unreachable: itemok and itemprep ran before the
			 * record was written.  If it happens anyway, memory
			 * no longer describes what a restart would produce,
			 * so the store stops serving what it cannot vouch
			 * for and must be opened again.
			 */
			s->fatal = 1;
			s->broken = 1;
			seterr(b, "apply failed after the record was durable");
		}
		qunlock(&s->qlstate);
	}

	qlock(&s->qllog);
	if(err == 0 && !s->fatal){
		s->watermark = b->seqhi;
		s->wateroff = b->endoff;
	}
	s->relseq = b->seqhi;
	s->nflight--;
	for(m = b->items; m != nil; m = next){
		next = m->next;
		m->state = Idone;
		m->next = nil;
	}
	rwakeupall(&s->relrz);
	rwakeupall(&s->donerz);
	rwakeupall(&s->roomrz);
	qunlock(&s->qllog);
	free(b);
}

/*
 * §3.2: everything the apply can refuse is refused here, before the
 * item is stamped into a batch — because from the moment it is
 * stamped its entries are going into a record whose seq the next
 * record depends on, and a batch that is not written is a gap replay
 * stops at.  So the range checks §2.7 makes and the pinned-map rule
 * of clause 2 are tested on the caller's own error return, where a
 * refusal costs nothing.
 */
static int
itemok(Store *s, Item *it)
{
	Objrec *o;
	int i;

	if((o = it->obj) != nil){
		if(objrecok(s, o) < 0)
			return -1;
		if(o->emapslot != 0
		&& (it->emap == nil || it->emap->slot != o->emapslot)){
			werrstr("Eobj: slot %lud: no pinned map for emapslot "
				"%lud", o->slot, o->emapslot);
			return -1;
		}
	}
	for(i = 0; i < it->ndirty; i++)
		if(dirtyrecok(s, &it->dirty[i]) < 0)
			return -1;
	if(it->haseslot && it->eslot >= s->sb.nslots){
		werrstr("Eslot: slot %lud, nslots %lud", it->eslot,
			s->sb.nslots);
		return -1;
	}
	return 0;
}

/*
 * ... and everything the apply must allocate is allocated here, for
 * the same reason: the entry's oid buffer, which only a record whose
 * oid differs from the slot's needs, and one dirty-set record per
 * Edirty the item carries.  Growing the buffer leaves its contents
 * alone, so the index hash still finds the object it holds now.
 */
static int
itemprep(Store *s, Item *it)
{
	Objrec *o;
	Ient *e;
	uchar *p;
	int i;

	if((o = it->obj) != nil){
		qlock(&s->qlstate);
		e = &s->idx[o->slot];
		if(e->oidcap < o->oidlen){
			if((p = malloc(o->oidlen)) == nil){
				qunlock(&s->qlstate);
				return -1;
			}
			if(e->oid != nil)
				memmove(p, e->oid, e->oidlen);
			free(e->oid);
			e->oid = p;
			e->oidcap = o->oidlen;
		}
		qunlock(&s->qlstate);
	}
	if(it->ndirty > 0){
		if((it->spare = mallocz(it->ndirty*sizeof *it->spare, 1)) == nil)
			return -1;
		for(i = 0; i < it->ndirty; i++)
			if((it->spare[i] = mallocz(sizeof **it->spare, 1)) == nil)
				return -1;
	}
	return 0;
}

/*
 * A queued item is the store's own, not the caller's, and this is the
 * whole reason for the copy below.  A caller builds its Item and its
 * Eobj on its own stack; §7's procs are rfork(RFPROC|RFMEM) procs in
 * a T1 program and proccreate procs in the server, and the first kind
 * share the data and bss segments and **not** the stack.  Worse than
 * unshared: every proc's stack is mapped at the same virtual address,
 * so a pending queue of pointers into callers' stacks does not fault,
 * it *aliases* — a second committer following s->pend lands on its
 * own item, links that item to itself, and then walks the list for
 * ever while its batch-mates sleep on a batch that will never
 * complete.  So the queue holds nodes allocated where every proc can
 * see them, carrying a copy of the caller's Eobj and of its Edirty
 * entries.  What the copy need not reach is the map and freed arrays
 * an Eobj names: those are already in the caller's heap, and the
 * caller is inside logcommit until its batch has been applied.
 */
static Item*
itemnew(Store *s, Item *ci)
{
	Item *it;

	if((it = mallocz(sizeof *it, 1)) == nil)
		return nil;
	it->state = Ipending;
	it->freeing = ci->freeing;
	it->emap = ci->emap;
	it->eslot = ci->eslot;
	it->haseslot = ci->haseslot;
	it->ndirty = ci->ndirty;
	if(ci->obj != nil){
		it->objb = *ci->obj;
		it->obj = &it->objb;
	}
	if(ci->ndirty > 0){
		if((it->dirty = mallocz(ci->ndirty*sizeof *it->dirty, 1)) == nil){
			free(it);
			return nil;
		}
		memmove(it->dirty, ci->dirty, ci->ndirty*sizeof *it->dirty);
	}
	it->nbyte = itembytes(s, it);
	return it;
}

static void
itemfree(Item *it)
{
	int i;

	if(it == nil)
		return;
	for(i = 0; it->spare != nil && i < it->ndirty; i++)
		free(it->spare[i]);
	free(it->spare);
	free(it->dirty);
	free(it);
}

/*
 * Commit one operation's entries.  The caller is either the committer
 * of the batch that carries them or a member of it; either way it
 * returns only once that batch is durable and applied.
 */
int
logcommit(Store *s, Item *ci)
{
	Item *it;
	Batch *b;
	vlong t0;
	int full, oom, forced, r;

	ci->err[0] = '\0';
	if(Lrechdrsz + itembytes(s, ci) > maxrecbytes(&s->sb)){
		werrstr("commit record of %lud bytes exceeds the geometry's "
			"maximum", Lrechdrsz + itembytes(s, ci));
		return -1;
	}
	if(itemok(s, ci) < 0)
		return -1;
	if((it = itemnew(s, ci)) == nil || itemprep(s, it) < 0){
		itemfree(it);
		werrstr("out of memory");
		return -1;
	}

	forced = 0;
	qlock(&s->qllog);
	if(s->broken){
		qunlock(&s->qllog);
		itemfree(it);
		werrstr("%s", s->failerr[0] != '\0' ? s->failerr
			: "store condemned by an earlier log failure");
		return -1;
	}
	if(s->pendtail != nil)
		s->pendtail->next = it;
	else
		s->pend = it;
	s->pendtail = it;
	t0 = nsec();
	for(;;){
		if(it->state == Idone)
			break;
		if(it->state == Ibatched){
			rsleep(&s->donerz);
			continue;
		}
		if(s->nflight >= s->logdepth){
			rsleep(&s->roomrz);
			continue;
		}
		b = nil;
		full = 0;
		oom = 0;
		if(s->fullwait == 1 || forced){
			/*
			 * §13's fullwait point: one commit takes §6's wait
			 * whether or not the log is full, and stays in it
			 * until the test releases it — the schedule where a
			 * committer absorbs a waiting item and the wait then
			 * elapses under it.
			 */
			s->fullwait = 2;
			forced = 1;
			full = 1;
		}else
			b = formbatch(s, &full, &oom);
		if(b != nil){
			s->nflight++;
			rwakeupall(&s->roomrz);
			qunlock(&s->qllog);
			runbatch(s, b);
			qlock(&s->qllog);
			continue;
		}
		if(oom){
			unlink(s, it);
			qunlock(&s->qllog);
			itemfree(it);
			werrstr("out of memory");
			return -1;
		}
		if(!full){
			rsleep(&s->roomrz);
			continue;
		}
		/*
		 * §6: no free log space.  Wait for the checkpointer,
		 * bounded by ckwaitms, and then answer `disk full', which
		 * is §3.3's step-7 exit.  The item stays on the pending
		 * queue for the whole wait, so a committer may absorb it
		 * at any moment — the expected end of the wait: its state
		 * is re-checked under the lock after every sleep and
		 * before the elapsed test, so a commit that became durable
		 * while it waited returns success rather than an error for
		 * a write that is on the platter and published, and one
		 * that is in a running batch waits for that batch rather
		 * than being unlinked out of the middle of it.
		 */
		s->nlogwait++;
		if(forced)
			while(s->fullwait != 0 && !s->stop)
				rsleep(&s->roomrz);
		else{
			qunlock(&s->qllog);
			askcheckpoint(s);
			sleep(1);
			qlock(&s->qllog);
		}
		s->nlogwait--;
		if(it->state != Ipending)
			continue;
		if(!forced && (nsec() - t0)/1000000 < (vlong)s->cfg.ckwaitms)
			continue;
		unlink(s, it);
		qunlock(&s->qllog);
		itemfree(it);
		werrstr("disk full");
		return -1;
	}
	qunlock(&s->qllog);
	devpoint(s->d, "preack", 0);
	r = 0;
	if(it->err[0] != '\0'){
		werrstr("%s", it->err);
		r = -1;
	}
	itemfree(it);
	return r;
}
