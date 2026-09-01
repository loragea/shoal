#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"
#include "store.h"

/*
 * The checkpointer (docs/design/store.md §2.8) and the superblock
 * publisher (§2.2).
 *
 * The log is the durable authority for everything since the last
 * checkpoint.  The index, extent-map, dirty and bitmap regions are a
 * checkpoint: a materialisation of the log's effect up to ckseq,
 * written incrementally, and re-derivable by replay.  Nothing in the
 * write path writes them.
 *
 * A checkpoint writes every dirty page — only the dirty ones, each in
 * a piece no larger than one device request — issues one device
 * flush, and then writes the superblock with the new ckseq and
 * cklogoff.  Its cost is proportional to the state dirtied since the
 * last checkpoint and to nothing else, which is what lets §6 put a
 * number on how long a commit may wait for log space.
 *
 * Two rules do the load bearing:
 *
 *   - **A checkpoint materialises committed state only.**  The bitmap
 *     pages carry the committed allocation state and never a staged
 *     reservation (§6 keeps those in the staged set), because a
 *     durable bit for a grain no record ever allocates is damage
 *     replay cannot repair.
 *   - **ckseq MUST NOT exceed the durable watermark** (§7), which is
 *     reached only when a batch is durable *and* applied.  A ckseq
 *     past a record that never became durable would leave the
 *     checkpoint describing a write that does not exist; a ckseq past
 *     a record whose effects the checkpointer has not seen in memory
 *     is the same fault from the other side.
 *
 * and one more that is easy to lose: the new ckseq/cklogoff become
 * *publishable* only once the flush has returned (§2.2), so a publish
 * triggered by anything else mid-checkpoint carries the old mark.
 *
 * **A page whose write failed stays dirty.**  The mark is cleared
 * under qlstate when the page image is packed, so a change made after
 * the pack re-marks the page and is not lost; if the write then
 * fails, the mark is put back before the checkpoint gives up.  Losing
 * it is not an aborted checkpoint but a silent one: the page is
 * clean, so the *next* checkpoint skips it and publishes a ckseq and
 * a cklogoff past the records that dirtied it, and reclaims their log
 * space.  The committed state is then in neither the log nor the
 * region.  An extent-map entry is the same rule with an extra step,
 * because it is off the dirty list as well as unmarked: it is held
 * out of the eviction set (wb) for as long as the write is in flight,
 * so there is still an entry to put back.
 */

/*
 * The checkpointer proc runs on a tick and not on a wake-up: two of
 * §2.8's three triggers are the log's fill and the clock, so it has to
 * look at intervals whatever a requester does, and a Rendez beside
 * that would remove no sleep — it would only make the request appear
 * to be delivered rather than polled.  A request is therefore a
 * counter under cklk that the tick reads, and it is observed within
 * Cktickms.  Completion is the other way about: storecheckpoint sleeps
 * on ckrz, which the proc wakes.
 */
enum
{
	Cktickms	= 5,	/* how often the checkpointer looks */
};

/*
 * §2.2's publisher.  Every superblock write carries all fields, so a
 * writer that built its image from a stale snapshot would regress
 * whatever another writer had advanced.  There is therefore one
 * function under one QLock: it takes the lock, builds the whole image
 * from live in-memory state, issues one write and one flush, and
 * releases.  Caller holds qlsuper.
 */
int
publishlocked(Store *s)
{
	Sbsel sel;
	Super im;
	uchar *p;
	vlong off;

	if(superselect(s->d, &sel) < 0){
		/* clause 3: neither copy valid — MUST NOT write */
		werrstr("no valid superblock: refusing to publish");
		return -1;
	}
	im = s->sb;
	im.gen = sel.nextgen;
	im.ckseq = s->pub.ckseq;
	im.cklogoff = s->pub.cklogoff;
	im.qidnext = s->pub.qidnext;
	im.epochhigh = s->pub.epochhigh;
	memmove(im.monid, s->pub.monid, 16);
	im.monidset = s->pub.monidset;
	if((p = mallocz(im.hdrlen, 1)) == nil)
		return -1;
	superpack(p, &im);
	off = sel.victim == 0 ? 0 : super1off(s->d);
	if(devwriteretry(s->d, p, s->sb.secsz, off) < 0){
		free(p);
		return -1;
	}
	free(p);
	devpoint(s->d, "super", sel.victim);
	if(flushnow(s) < 0)
		return -1;
	s->pub.gen = im.gen;
	return 0;
}

/*
 * R16.  The qidnext high-water MUST be durable before any path in its
 * batch is issued: the counter advances in batches of Qidbatch, the
 * in-memory counter is handed out one at a time, and when it reaches
 * the recorded high-water the superblock write is issued and must
 * return before the next path is given out.  So a create costs no
 * superblock write 1023 times out of 1024, the recorded value is
 * always at least any value ever handed out, and a crash wastes at
 * most 1023 paths out of 2^64.
 */
uvlong
qidalloc(Store *s)
{
	uvlong q;

	qlock(&s->qlsuper);
	if(s->qidcur >= s->pub.qidnext){
		s->pub.qidnext = s->qidcur + Qidbatch;
		if(publishlocked(s) < 0){
			s->pub.qidnext = s->qidcur;
			qunlock(&s->qlsuper);
			return 0;
		}
	}
	q = s->qidcur++;
	qunlock(&s->qlsuper);
	return q;
}

/*
 * R15.  epochhigh MUST be durable before the instance takes any
 * action under that epoch, because layer-a §8.6 has a rebuilding
 * monitor read this value out of /status and publish above it.
 */
int
epochadopt(Store *s, uvlong epoch)
{
	uvlong old;

	qlock(&s->qlsuper);
	if(epoch <= s->pub.epochhigh){
		qunlock(&s->qlsuper);
		return 0;
	}
	old = s->pub.epochhigh;
	s->pub.epochhigh = epoch;
	if(publishlocked(s) < 0){
		s->pub.epochhigh = old;
		qunlock(&s->qlsuper);
		return -1;
	}
	qunlock(&s->qlsuper);
	return 0;
}

/* monid MUST be durable before the instance acts on the map that pinned it */
int
monidpin(Store *s, uchar id[16])
{
	uchar old[16];
	int wasset;

	qlock(&s->qlsuper);
	if(s->pub.monidset){
		if(memcmp(s->pub.monid, id, 16) != 0){
			qunlock(&s->qlsuper);
			werrstr("monitor identity already pinned");
			return -1;
		}
		qunlock(&s->qlsuper);
		return 0;
	}
	memmove(old, s->pub.monid, 16);
	wasset = s->pub.monidset;
	memmove(s->pub.monid, id, 16);
	s->pub.monidset = 1;
	if(publishlocked(s) < 0){
		memmove(s->pub.monid, old, 16);
		s->pub.monidset = wasset;
		qunlock(&s->qlsuper);
		return -1;
	}
	qunlock(&s->qlsuper);
	return 0;
}

/* the free-entry image §2.3 requires of a slot past nslots in a page */
static void
freeidx(uchar *p)
{
	Idxent e;

	memset(&e, 0, sizeof e);
	e.vers = Storevers;
	idxpack(p, &e);
}

static void
freedirt(uchar *p)
{
	Dirtent e;

	memset(&e, 0, sizeof e);
	e.vers = Storevers;
	dirtpack(p, &e);
}

int
checkpoint(Store *s)
{
	uchar *buf, *ebuf;
	Emape *c;
	Bmpage bh;
	Dirtent *t;
	uvlong ckseq, cklogoff, bits, i;
	ulong j, per, slot, npage;
	int r;

	qlock(&s->qllog);
	ckseq = s->watermark;
	cklogoff = s->sb.logoff + s->wateroff;
	qunlock(&s->qllog);

	if((buf = mallocz(s->sb.blksz, 1)) == nil)
		return -1;
	if((ebuf = mallocz(s->sb.emapsz, 1)) == nil){
		free(buf);
		return -1;
	}
	npage = 0;
	r = 0;

	/* index pages */
	per = s->sb.blksz / Idxentsz;
	for(i = 0; i < s->nidxpage && r == 0; i++){
		qlock(&s->qlstate);
		if(!s->idxdirty[i]){
			qunlock(&s->qlstate);
			continue;
		}
		s->idxdirty[i] = 0;
		for(j = 0; j < per; j++){
			slot = i*per + j;
			if(slot < s->sb.nslots)
				ientpack(s, slot, buf + j*Idxentsz);
			else
				freeidx(buf + j*Idxentsz);
		}
		qunlock(&s->qlstate);
		if(devwriteretry(s->d, buf, s->sb.blksz,
			s->sb.idxoff*(uvlong)s->sb.secsz + i*(uvlong)s->sb.blksz) < 0){
			r = -1;
			qlock(&s->qlstate);
			s->idxdirty[i] = 1;
			qunlock(&s->qlstate);
		}
		devpoint(s->d, "ckpt", ++npage);
		if(s->pubatpage != 0 && npage == s->pubatpage)
			epochadopt(s, s->pub.epochhigh + 1);
	}

	/* extent-map entries */
	while(r == 0){
		qlock(&s->qlstate);
		if((c = s->edirty) == nil){
			qunlock(&s->qlstate);
			break;
		}
		s->edirty = c->dnext;
		c->dnext = nil;
		/*
		 * wb rises before dirty falls, and falls after dirty rises
		 * again: etrim reads the pair under qlemap while these run
		 * under qlstate (§7 rule 1 is that the two are never held
		 * together), so the entry must never be seen with both
		 * clear.  It would then be freed under the write-back, and
		 * the bytes memmove'd below would be freed memory sealed
		 * with a fresh csum128 and written to the entry's own
		 * offset — checksum-valid garbage, which §5 step 9 cannot
		 * condemn.
		 */
		c->wb = 1;
		c->dirty = 0;
		slot = c->slot;
		memmove(ebuf, c->p, s->sb.emapsz);
		qunlock(&s->qlstate);
		if(emapwrite(s, slot, ebuf) < 0)
			r = -1;
		qlock(&s->qlstate);
		if(r < 0 && !c->dirty){
			c->dirty = 1;
			c->dnext = s->edirty;
			s->edirty = c;
		}
		c->wb = 0;
		qunlock(&s->qlstate);
		devpoint(s->d, "ckpt", ++npage);
		if(s->pubatpage != 0 && npage == s->pubatpage)
			epochadopt(s, s->pub.epochhigh + 1);
	}

	/* dirty-record pages */
	per = s->sb.blksz / Dirtentsz;
	for(i = 0; i < s->ndirtpage && r == 0; i++){
		qlock(&s->qlstate);
		if(!s->dirtdirty[i]){
			qunlock(&s->qlstate);
			continue;
		}
		s->dirtdirty[i] = 0;
		for(j = 0; j < per; j++){
			slot = i*per + j;
			if(slot < s->sb.ndirty && (t = s->dirt[slot]) != nil)
				dirtpack(buf + j*Dirtentsz, t);
			else
				freedirt(buf + j*Dirtentsz);
		}
		qunlock(&s->qlstate);
		if(devwriteretry(s->d, buf, s->sb.blksz,
			s->sb.dirtoff*(uvlong)s->sb.secsz + i*(uvlong)s->sb.blksz) < 0){
			r = -1;
			qlock(&s->qlstate);
			s->dirtdirty[i] = 1;
			qunlock(&s->qlstate);
		}
		devpoint(s->d, "ckpt", ++npage);
		if(s->pubatpage != 0 && npage == s->pubatpage)
			epochadopt(s, s->pub.epochhigh + 1);
	}

	/* bitmap pages, stamped with the generation they materialise */
	bits = bmbits(s->sb.blksz);
	for(i = 0; i < s->nbmpage && r == 0; i++){
		qlock(&s->qlstate);
		if(!s->bmdirty[i]){
			qunlock(&s->qlstate);
			continue;
		}
		s->bmdirty[i] = 0;
		memset(buf, 0, s->sb.blksz);
		memmove(buf + Bmhdrsz, s->bmap + i*(bits/8), bits/8);
		qunlock(&s->qlstate);
		memset(&bh, 0, sizeof bh);
		bh.vers = Storevers;
		bh.page = i;
		bh.ckseq = ckseq;
		bmpack(buf, s->sb.blksz, &bh);
		if(devwriteretry(s->d, buf, s->sb.blksz,
			s->sb.bmapoff*(uvlong)s->sb.secsz + i*(uvlong)s->sb.blksz) < 0){
			r = -1;
			qlock(&s->qlstate);
			s->bmdirty[i] = 1;
			qunlock(&s->qlstate);
		}
		devpoint(s->d, "ckpt", ++npage);
		if(s->pubatpage != 0 && npage == s->pubatpage)
			epochadopt(s, s->pub.epochhigh + 1);
	}
	free(buf);
	free(ebuf);
	if(r < 0)
		return -1;

	if(flushnow(s) < 0)
		return -1;

	/*
	 * §13's reclaim point: reclaiming log space before step 3 has
	 * returned is the one way this format can lose data, so the
	 * mutation is built in and inert unless a test asks for it.
	 */
	if(s->reclaimearly){
		qlock(&s->qllog);
		s->logstart = cklogoff - s->sb.logoff;
		rwakeupall(&s->roomrz);
		qunlock(&s->qllog);
	}

	qlock(&s->qlsuper);
	s->pub.ckseq = ckseq;
	s->pub.cklogoff = cklogoff;
	r = publishlocked(s);
	qunlock(&s->qlsuper);
	if(r < 0)
		return -1;

	/* log space before the newly published cklogoff is now reclaimed */
	qlock(&s->qllog);
	s->logstart = cklogoff - s->sb.logoff;
	rwakeupall(&s->roomrz);
	qunlock(&s->qllog);
	qlock(&s->qlstate);
	s->ndirtypage = 0;
	qunlock(&s->qlstate);
	return 0;
}

/*
 * Checkpoints run when the log passes ckhigh (default one quarter
 * full) and every ckms if anything is dirty — both policy, both
 * tunable without a format change.  A quarter rather than a half
 * because the checkpointer's job is to keep the log from ever being
 * full, and starting earlier is what keeps §6's wait rare.
 */
static int
ckdue(Store *s)
{
	uvlong used;
	vlong now;

	qlock(&s->qllog);
	used = logused(s);
	qunlock(&s->qllog);
	if(s->cfg.ckhigh != 0 && used > s->sb.logsecs/s->cfg.ckhigh)
		return 1;
	now = nsec();
	if(s->cfg.ckms != 0 && s->ndirtypage > 0
	&& now - s->cklast > (vlong)s->cfg.ckms*1000000LL)
		return 1;
	return 0;
}

void
ckptproc(void *a)
{
	Store *s;
	uvlong req;
	int r;

	s = a;
	for(;;){
		qlock(&s->cklk);
		for(;;){
			if(s->stop){
				qunlock(&s->cklk);
				goto out;
			}
			if(s->ckreq > s->ckdone)
				break;
			qunlock(&s->cklk);
			if(ckdue(s)){
				qlock(&s->cklk);
				break;
			}
			sleep(Cktickms);
			qlock(&s->cklk);
		}
		req = s->ckreq;
		s->ckbusy = 1;
		qunlock(&s->cklk);
		r = checkpoint(s);
		qlock(&s->cklk);
		s->ckbusy = 0;
		s->ckerr = r;
		if(req > s->ckdone)
			s->ckdone = req;
		s->cklast = nsec();
		rwakeupall(&s->ckrz);
		qunlock(&s->cklk);
	}
out:
	storeprocdone(s);
}

int
storecheckpoint(Store *s)
{
	uvlong gen;
	int r;

	if(!s->ckproc){
		qlock(&s->cklk);
		while(s->ckbusy)
			rsleep(&s->ckrz);
		s->ckbusy = 1;
		qunlock(&s->cklk);
		r = checkpoint(s);
		qlock(&s->cklk);
		s->ckbusy = 0;
		s->cklast = nsec();
		rwakeupall(&s->ckrz);
		qunlock(&s->cklk);
		return r;
	}
	qlock(&s->cklk);
	gen = ++s->ckreq;
	while(s->ckdone < gen)
		rsleep(&s->ckrz);
	r = s->ckerr;
	qunlock(&s->cklk);
	return r;
}
