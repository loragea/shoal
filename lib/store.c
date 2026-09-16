#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"
#include "store.h"

/*
 * Start-up: recovery and replay, docs/design/store.md §5, and the
 * store's lifetime.
 *
 * The order below is load-bearing in one place: **nothing is
 * condemned before replay has run.**  A crashed checkpoint damages
 * exactly the structures replay repairs (§3.4), so a store that
 * judges the index before replaying puts live objects into /lost
 * after an ordinary power cut — and layer-a §7.5 then makes them fail
 * client access and lose arbitration against everything including
 * absence.
 *
 * Every refusal prints one line naming the structure and exits
 * non-zero: the store never starts in a degraded mode it did not
 * name.  A corrupt index entry is a running store with an object in
 * /lost; a bitmap page that fails its checksum is a slow start, not a
 * refusal; a Pmax that neither the superblock nor replay reaches is a
 * refusal, because the log no longer describes what the disk already
 * holds.
 */

enum
{
	Bulkio	= 64*1024,	/* §0: bulk reads use 64 KiB requests */
};

static int
bitget(uchar *p, uvlong i)
{
	return (p[i/8] >> (i%8)) & 1;
}

static void
bitset(uchar *p, uvlong i)
{
	p[i/8] |= 1 << (i%8);
}

/*
 * §3.2: a store whose apply failed after its record was durable is
 * serving in-memory state that its own log no longer describes, so it
 * answers nothing until it has been opened again and replayed.  The
 * commit path refuses through the same flag (broken).  Both are
 * qllog's, which is where the batch that condemns the store sets them
 * and where §3.2's failseq is read beside them.  It is taken alone and
 * released before this call takes any other, so §7 rule 1 — no proc
 * holds two state locks at once — still holds as stated.
 */
int
storeserving(Store *s)
{
	int f;

	qlock(&s->qllog);
	f = s->fatal;
	qunlock(&s->qllog);
	if(f){
		werrstr("store condemned: in-memory state no longer matches "
			"the log; open it again");
		return 0;
	}
	return 1;
}

int
storeproc(Store *s, void (*fn)(void*), void *a)
{
	if(s->cfg.spawn == nil){
		werrstr("no spawn callback: the store needs procs of its own");
		return -1;
	}
	qlock(&s->proclk);
	s->nproc++;
	qunlock(&s->proclk);
	if((*s->cfg.spawn)(fn, a) < 0){
		qlock(&s->proclk);
		s->nproc--;
		rwakeupall(&s->procrz);
		qunlock(&s->proclk);
		return -1;
	}
	return 0;
}

void
storeprocdone(Store *s)
{
	qlock(&s->proclk);
	s->nproc--;
	rwakeupall(&s->procrz);
	qunlock(&s->proclk);
}

/* §13's -X points that are hooks rather than crashes; inert unless set */
void
storehook(Store *s, char *name, uvlong n)
{
	if(strcmp(name, "batch") == 0){
		qlock(&s->qllog);
		s->holdseq = n;
		rwakeupall(&s->holdrz);
		qunlock(&s->qllog);
	}else if(strcmp(name, "fullwait") == 0){
		qlock(&s->qllog);
		s->fullwait = n != 0;
		rwakeupall(&s->roomrz);
		qunlock(&s->qllog);
	}else if(strcmp(name, "flush") == 0){
		qlock(&s->fllk);
		s->flcount = 0;
		s->flhold = n;
		rwakeupall(&s->flrz);
		qunlock(&s->fllk);
	}else if(strcmp(name, "fatal") == 0){
		/*
		 * §3.2's condemnation, which the commit path reaches only
		 * from an apply that failed after its record was durable —
		 * a case itemok and itemprep are there to make unreachable.
		 * The hook is how §13 drives what the store answers once it
		 * is in that state.
		 */
		qlock(&s->qllog);
		s->fatal = n != 0;
		s->broken = n != 0;
		qunlock(&s->qllog);
	}else if(strcmp(name, "snapstale") == 0){
		/*
		 * §9's snapshot open counts the index, allocates the vector
		 * outside the lock and fills it under a second hold, so the
		 * index can have grown by the time the fill runs.  This arms
		 * the next n fill attempts to behave as if it had grown by
		 * snapshort entries since the count, so a test can drive the
		 * re-count — and the vector's slack, which is what decides
		 * whether a given growth needs one — without racing for
		 * either.  One is spent per fill attempt, not per open, and
		 * an open makes up to Snaptries of those.  Inert while
		 * snapshort is 0.
		 */
		qlock(&s->qlstate);
		s->snapstale = n;
		qunlock(&s->qlstate);
	}else if(strcmp(name, "snapshort") == 0){
		/* how far the count is behind the index, for the above */
		qlock(&s->qlstate);
		s->snapshort = n;
		qunlock(&s->qlstate);
	}else if(strcmp(name, "snaphold") == 0){
		/*
		 * §9's open takes the bound's slot under its first count
		 * and gives it back at its bail-out, so an open in flight
		 * when storeclose runs can be holding the store's last
		 * claim — and then the bail-out is what frees the Store.
		 * This parks the next open with the slot taken until
		 * storeclose has set `closed', so a test drives that
		 * interleaving instead of racing for it.  One arming
		 * parks one open; inert while 0.
		 */
		qlock(&s->qlstate);
		s->snaphold = n != 0;
		qunlock(&s->qlstate);
	}else if(strcmp(name, "bmfold") == 0){
		/*
		 * §8's fold reads a slot's map outside qlstate and then
		 * validates the entry by its generation stamp, and the
		 * window between the two is what the stamp exists for.
		 * This arms the next n rounds of a fold to park in that
		 * window, so a test lands its commit there instead of
		 * racing for it.  Arming also lets a parked round go, and
		 * a round parks by disarming the go flag itself, so a test
		 * that drives a fold round after round re-arms with n=1
		 * each time; 0 disarms the point and releases whatever is
		 * parked at it.  Inert while 0.
		 */
		qlock(&s->qlstate);
		s->bmfoldhold = n;
		s->bmfoldgo = 1;
		rwakeupall(&s->bmrz);
		qunlock(&s->qlstate);
	}else if(strcmp(name, "bmswap") == 0){
		/*
		 * §8's swap installs the shadow a page at a time and drops
		 * qlstate between pages, and what may happen in that gap
		 * is a rule of its own (D24).  This arms the next n pages
		 * to park once installed, in the hold that installed them,
		 * so a test drives the gap instead of racing for it.  It
		 * is armed and released exactly as bmfold is.
		 */
		qlock(&s->qlstate);
		s->bmswaphold = n;
		s->bmswapgo = 1;
		rwakeupall(&s->bmrz);
		qunlock(&s->qlstate);
	}else if(strcmp(name, "reclaim") == 0)
		s->reclaimearly = n != 0;
	else if(strcmp(name, "publish") == 0)
		s->pubatpage = n;
}

static ulong
pow2ge(ulong n)
{
	ulong k;

	for(k = 1; k < n && k < (1UL<<30); k <<= 1)
		;
	return k;
}

/*
 * §5 step 3: the geometry must be self-consistent before anything is
 * addressed from it.  Every region inside the partition, in order and
 * without overlap; ngrains below 2^32; cklogoff sector-aligned inside
 * the log region — step 7 addresses from it, and an unchecked offset
 * out of the region is the same fault an unchecked nsec would be.
 */
static int
geomok(Store *s, Dev *d)
{
	Super *sb;
	uvlong nsec, last, pagesecs;

	sb = &s->sb;
	nsec = d->size / d->secsz;
	if(sb->secsz != d->secsz){
		werrstr("superblock secsz %lud, device sector %lud",
			sb->secsz, d->secsz);
		return -1;
	}
	/*
	 * §2.1 bounds blksz by the format, not by the device: a grain
	 * larger than the unit's Wunit is written in Wunit pieces (§0),
	 * so a store formatted on one unit opens on another.
	 */
	if(sb->blksz == 0 || (sb->blksz & (sb->blksz - 1)) != 0
	|| sb->blksz < sb->secsz || sb->blksz > Blkszmax){
		werrstr("blksz %lud is not a power of two in [%lud, %d]",
			sb->blksz, sb->secsz, Blkszmax);
		return -1;
	}
	if(sb->nblkmax == 0 || sb->objmax == 0
	|| sb->objmax / sb->blksz != sb->nblkmax){
		werrstr("objmax %llud over blksz %lud is not nblkmax %lud",
			sb->objmax, sb->blksz, sb->nblkmax);
		return -1;
	}
	if(sb->emapsz < Emaphdrsz + 20*(uvlong)sb->nblkmax
	|| sb->emapsz % sb->secsz != 0){
		werrstr("emapsz %lud does not hold %lud blocks", sb->emapsz,
			sb->nblkmax);
		return -1;
	}
	pagesecs = sb->blksz / sb->secsz;
	last = nsec - 1;
	if(sb->logoff < 1 || sb->logsecs < 1
	|| sb->logoff + sb->logsecs > sb->idxoff
	|| sb->idxoff + sb->idxsecs > sb->emapoff
	|| sb->emapoff + sb->emapsecs > sb->dirtoff
	|| sb->dirtoff + sb->dirtsecs > sb->bmapoff
	|| sb->bmapoff + sb->bmapsecs > sb->dataoff
	|| sb->dataoff + sb->datasecs > last){
		werrstr("regions overlap or run past the partition");
		return -1;
	}
	if((uvlong)sb->nslots*Idxentsz > sb->idxsecs*(uvlong)sb->secsz
	|| (uvlong)sb->nemap*sb->emapsz > sb->emapsecs*(uvlong)sb->secsz
	|| (uvlong)sb->ndirty*Dirtentsz > sb->dirtsecs*(uvlong)sb->secsz){
		werrstr("a metadata region is smaller than its own count");
		return -1;
	}
	/*
	 * §2.6's region is load-bearing even when empty of records:
	 * applydirty can drop a peer's records to make room, but a
	 * region of no slots at all leaves it nothing to drop, so a
	 * store opened over ndirty == 0 commits no Edirty (itemok) and
	 * replay refuses the first record that carries one — a brick,
	 * on the first fine-grained mark.  shoalfmt never writes such a
	 * geometry; one that arrives anyway is refused whole, here.
	 */
	if(sb->ndirty == 0){
		werrstr("the geometry has no dirty region");
		return -1;
	}
	if(sb->bmapsecs % pagesecs != 0 || nbmpage(sb) == 0){
		werrstr("bitmap region of %llud sectors is not whole pages",
			sb->bmapsecs);
		return -1;
	}
	if(nbmpage(sb)*bmbits(sb->blksz) < sb->ngrains){
		werrstr("bitmap of %llud pages does not cover %llud grains",
			nbmpage(sb), sb->ngrains);
		return -1;
	}
	if(sb->ngrains < 2 || sb->ngrains >= (1ULL<<32)){
		werrstr("ngrains %llud", sb->ngrains);
		return -1;
	}
	if(sb->ngrains > sb->datasecs/pagesecs){
		werrstr("ngrains %llud past the data region", sb->ngrains);
		return -1;
	}
	if(sb->cklogoff < sb->logoff || sb->cklogoff >= sb->logoff + sb->logsecs){
		werrstr("cklogoff %llud outside the log region", sb->cklogoff);
		return -1;
	}
	if(sb->csumalg != Csumblake2s){
		werrstr("csumalg %lud", sb->csumalg);
		return -1;
	}
	return 0;
}

/*
 * §5 step 4: read the index region in 64 KiB requests and build the
 * in-memory index tolerantly — verify every entry's checksum and
 * range-check its own fields, record which slots failed, and condemn
 * nothing yet.
 */
static int
readindex(Store *s)
{
	uchar *buf;
	Idxent d;
	Ient *e;
	uvlong off, end;
	ulong n, per, i, slot;

	per = Bulkio / Idxentsz;
	if((buf = malloc(per*Idxentsz)) == nil)
		return -1;
	off = s->sb.idxoff*(uvlong)s->sb.secsz;
	end = off + (uvlong)s->sb.nslots*Idxentsz;
	slot = 0;
	while(off < end){
		n = per;
		if((uvlong)n > (end - off)/Idxentsz)
			n = (end - off)/Idxentsz;
		if(devread(s->d, buf, n*Idxentsz, off) < 0){
			free(buf);
			return -1;
		}
		for(i = 0; i < n; i++, slot++){
			e = &s->idx[slot];
			if(idxunpack(&d, buf + i*Idxentsz, s->sb.nemap) < 0){
				e->bad = 1;
				slotmark(s, slot);
				continue;
			}
			if(d.state == Sfree)
				continue;
			if((e->oid = malloc(d.oidlen)) == nil){
				free(buf);
				return -1;
			}
			memmove(e->oid, d.oid, d.oidlen);
			e->oidlen = d.oidlen;
			e->oidcap = d.oidlen;
			e->state = d.state;
			e->flags = d.flags;
			e->emapslot = d.emapslot;
			e->qidpath = d.qidpath;
			e->len = d.len;
			e->ver = d.ver;
			e->wepoch = d.wepoch;
			e->mtime = d.mtime;
			memmove(e->csum, d.csum, Csumlen);
			e->grain0 = d.grain0;
			memmove(e->dig0, d.dig0, Blkdlen);
			if(e->state == Slive)
				s->nlive++;
			else
				s->ntomb++;
			ienthash(s, slot);
			slotmark(s, slot);
		}
		off += n*Idxentsz;
	}
	free(buf);
	return 0;
}

/*
 * §5 step 5: read the bitmap.  A page that fails its checksum sets
 * the rebuild flag; it is not a refusal, and it contributes no ckseq
 * — a page that fails its checksum has an arbitrary ckseq, so Pmax is
 * taken over the pages that pass their own checksum and nothing else.
 */
static int
readbitmap(Store *s)
{
	uchar *buf;
	Bmpage h;
	uvlong i, bits;

	bits = bmbits(s->sb.blksz);
	if((buf = malloc(s->sb.blksz)) == nil)
		return -1;
	for(i = 0; i < s->nbmpage; i++){
		if(devread(s->d, buf, s->sb.blksz,
			s->sb.bmapoff*(uvlong)s->sb.secsz
			+ i*(uvlong)s->sb.blksz) < 0){
			free(buf);
			return -1;
		}
		if(bmunpack(&h, buf, s->sb.blksz, i) < 0){
			s->bmaprebuild = 1;
			continue;
		}
		memmove(s->bmap + i*(bits/8), buf + Bmhdrsz, bits/8);
		if(h.ckseq > s->pmax)
			s->pmax = h.ckseq;
	}
	free(buf);
	return 0;
}

/*
 * §5 step 6: read the dirty region and build the in-memory dirty set.
 * A record that fails its checksum is dropped and counted; it is at
 * worst one peer's fine-grained mark, and the restart fullsync covers
 * it.  R7 is what the region is for.
 */
static int
readdirty(Store *s)
{
	uchar *buf;
	Dirtent d, *t;
	uvlong off, end;
	ulong n, per, i, slot;

	per = Bulkio / Dirtentsz;
	if((buf = malloc(per*Dirtentsz)) == nil)
		return -1;
	off = s->sb.dirtoff*(uvlong)s->sb.secsz;
	end = off + (uvlong)s->sb.ndirty*Dirtentsz;
	slot = 0;
	while(off < end){
		n = per;
		if((uvlong)n > (end - off)/Dirtentsz)
			n = (end - off)/Dirtentsz;
		if(devread(s->d, buf, n*Dirtentsz, off) < 0){
			free(buf);
			return -1;
		}
		for(i = 0; i < n; i++, slot++){
			if(dirtunpack(&d, buf + i*Dirtentsz) < 0){
				s->ndirtydrop++;
				continue;
			}
			if(d.state == 0)
				continue;
			if((t = mallocz(sizeof *t, 1)) == nil){
				free(buf);
				return -1;
			}
			*t = d;
			s->dirt[slot] = t;
			s->ndirtused++;
			addpeer(s, d.peer, d.peerlen);
		}
		off += n*Dirtentsz;
	}
	free(buf);
	return 0;
}

/* apply one record's entries, through the same function §3.2 uses */
static int
applyents(Store *s, uchar *p, Lrec *r)
{
	uchar *q, *end;
	Lent e;
	Objrec o;
	Dirtyrec d;
	Emape *c;
	ulong i, slot;
	int rc;

	q = p + Lrechdrsz;
	end = p + (uvlong)r->nsec*s->sb.secsz;
	for(i = 0; i < r->nent; i++){
		if(lentunpack(&e, q, end - q) < 0)
			return -1;
		switch(e.kind){
		case Kobj:
			if(objrecunpack(&o, e.body, e.len - Lenthdrsz) < 0)
				return -1;
			c = nil;
			if(o.emapslot != 0
			&& (c = emapget(s, o.emapslot,
				(o.oflags & Oslot) != 0)) == nil){
				objrecfree(&o);
				return -1;
			}
			qlock(&s->qlstate);
			rc = applyrec(s, &o, c);
			qunlock(&s->qlstate);
			emapunpin(s, c);
			objrecfree(&o);
			if(rc < 0)
				return -1;
			break;
		case Kdirty:
			if(dirtyrecunpack(&d, e.body, e.len - Lenthdrsz) < 0)
				return -1;
			qlock(&s->qlstate);
			rc = applydirty(s, &d, nil);
			qunlock(&s->qlstate);
			if(rc < 0)
				return -1;
			break;
		case Kslot:
			if(slotrecunpack(&slot, e.body, e.len - Lenthdrsz) < 0)
				return -1;
			qlock(&s->qlstate);
			rc = applyslot(s, slot);
			qunlock(&s->qlstate);
			if(rc < 0)
				return -1;
			break;
		default:
			werrstr("log entry kind %d", e.kind);
			return -1;
		}
		q += e.len;
	}
	return 0;
}

/*
 * §5 step 7.  Start at cklogoff with the expected sequence seeded at
 * **ckseq + 1** — the seed is what makes an earlier lap's record
 * unacceptable however plausible its bytes — bounds-check nsec before
 * using it to address anything, verify the checksum over nsec*secsz,
 * check seq against the expectation, apply the entries, and continue
 * at the region start if Fwrap is set, at the region start if +nsec
 * reaches the region end, and at +nsec otherwise.  Stop at the first
 * record that is invalid or out of sequence.
 *
 * **A sector that cannot be read is not the end of the log.**  Every
 * other reason to stop is a statement about the bytes at rel — no
 * valid header, the wrong sequence, a length this geometry cannot
 * hold — and each of them says the log ends there.  A device error
 * says nothing about them: the records past the fault may be perfectly
 * good, and stopping would discard every one of them, acked writes
 * included, and then hand the tail back to the allocator to overwrite.
 * That is a silent truncation of exactly the kind §2.5's coverage rule
 * refuses to start on, so replay refuses too — as steps 4, 5 and 6
 * already do for the index, the bitmap and the dirty region.
 *
 * **Neither is a record that cannot be applied.**  applyents fails on
 * a device error under an extent map (emapget reads the extent-map
 * region; emapreclaim writes it), on an allocation failure, and on
 * any entry it cannot decode or that its checks refuse — an entry
 * header or body that does not parse, a kind this build does not
 * know, a field §2.7's range checks reject — and none of those says
 * anything about the bytes at rel: the record is valid,
 * checksummed and in sequence.  Worse than the truncation, applyents
 * applies entries one at a time, so a mid-record failure leaves the
 * store on a half-applied record no crash could produce.  Both refuse
 * the start, exactly as a read error does.
 */
static int
replay(Store *s)
{
	uchar *hdr, *buf;
	Lrec r, r2;
	uvlong seq, rel, scanned, off, n, m, bad;
	char e[ERRMAX];

	if((hdr = malloc(s->sb.secsz)) == nil)
		return -1;
	if((buf = malloc(maxrecbytes(&s->sb))) == nil){
		free(hdr);
		return -1;
	}
	seq = s->sb.ckseq + 1;
	rel = s->sb.cklogoff - s->sb.logoff;
	bad = s->sb.cklogoff;		/* until a record is applied */
	scanned = 0;
	for(;;){
		if(scanned >= s->sb.logsecs)
			break;
		off = s->sb.logoff*(uvlong)s->sb.secsz
			+ rel*(uvlong)s->sb.secsz;
		if(devread(s->d, hdr, s->sb.secsz, off) < 0){
			bad = s->sb.logoff + rel;
			goto refuse;
		}
		if(lrecunpack(&r, hdr) < 0)
			break;
		if(r.nsec < 1 || rel + r.nsec > s->sb.logsecs)
			break;
		if((uvlong)r.nsec*s->sb.secsz > maxrecbytes(&s->sb))
			break;
		for(n = 0; n < (uvlong)r.nsec*s->sb.secsz; n += m){
			m = (uvlong)r.nsec*s->sb.secsz - n;
			if(m > Bulkio)
				m = Bulkio;
			if(devread(s->d, buf + n, m, off + n) < 0){
				/* the sector the failed read began at */
				bad = s->sb.logoff + rel + n/s->sb.secsz;
				goto refuse;
			}
		}
		if(lrecvalid(buf, s->sb.secsz, &r2, rel, s->sb.logsecs, seq) < 0)
			break;
		if(applyents(s, buf, &r2) < 0){
			bad = s->sb.logoff + rel;
			goto refuse;
		}
		s->replayhigh = seq;
		s->nreplay++;
		seq++;
		bad = s->sb.logoff + rel;	/* the record just applied */
		scanned += r2.nsec;
		if(r2.flags & Fwrap)
			rel = 0;
		else{
			rel += r2.nsec;
			if(rel >= s->sb.logsecs)
				rel = 0;
		}
		if(s->nemapc > s->emapcap && emapreclaim(s) < 0)
			goto relay;
	}
	USED(bad);			/* only the refusals below read it */
	/*
	 * Replay ends by writing back the maps the applied records
	 * dirtied.  That is what makes a device error under the
	 * extent-map region a refusal of the *start*, named while the
	 * operator is looking at it, rather than a store that opens and
	 * whose every checkpoint then fails behind it.  A read-only
	 * store cannot take the write (§12), and there is nothing to
	 * lose by not taking it: the maps stay dirty in the cache, as a
	 * live commit's do, for the checkpoint a later writable open
	 * makes.  The one condition that cannot survive being held —
	 * a log dirtying more entries than the cache holds — is what
	 * the in-loop write-back above refuses by name.  emapreclaim
	 * would hold them anyway on such a store; the test is here so
	 * that replay says what it does without being read through
	 * another file.
	 */
	if(!s->d->rdonly && emapreclaim(s) < 0)
		goto relay;
	free(hdr);
	free(buf);
	s->logtail = rel;
	s->watermark = s->nreplay > 0 ? s->replayhigh : s->sb.ckseq;
	s->relseq = s->watermark;
	s->wateroff = rel;
	s->seqnext = s->watermark + 1;
	return 0;

refuse:
	rerrstr(e, sizeof e);
	free(hdr);
	free(buf);
	/*
	 * The remedy leads because ERRMAX cuts the tail: with a real
	 * device path in front and a device error's own text inside,
	 * a remedy at the end of the message is the part the operator
	 * never sees.
	 */
	werrstr("the log cannot be replayed; shoalck, then refill from "
		"peers; log sector %llud: %s", bad, e);
	return -1;

	/*
	 * A refusal replay merely relays — the extent-map cache's, or a
	 * device error under the extent-map region — is not log damage,
	 * and the remedy above would send the operator to wipe a store
	 * whose log is intact.  It is passed through as it stands, which
	 * is also what keeps the condition itself inside ERRMAX: the
	 * wrapper is what the tail that gets cut used to be.
	 */
relay:
	free(hdr);
	free(buf);
	return -1;
}

/*
 * §5 step 11's rebuild: the bitmap is not authoritative — the truth
 * is the set of grains referenced by committed index and extent-map
 * state, and the bitmap is a cache of that truth so start-up need not
 * scan it.  A page that fails its checksum is repaired automatically
 * by scanning every live object's map, which costs one read of every
 * live multi-block object's extent map and nothing at all on a disk
 * of one-block objects.
 */
static int
rebuildbitmap(Store *s)
{
	Ient *e;
	Emape *c;
	uvlong nblk, i;
	ulong slot, g;

	memset(s->bmap, 0, s->nbmpage*(bmbits(s->sb.blksz)/8));
	bitset(s->bmap, 0);		/* grain 0 is never allocatable */
	for(slot = 0; slot < s->sb.nslots; slot++){
		e = &s->idx[slot];
		if(e->state == Sfree || e->bad)
			continue;
		nblk = blkcount(e->len, s->sb.blksz);
		if(e->emapslot == 0){
			if(nblk > 0 && e->grain0 != 0
			&& e->grain0 < s->sb.ngrains)
				bitset(s->bmap, e->grain0);
			continue;
		}
		if((c = emapget(s, e->emapslot, 0)) == nil)
			return -1;
		/*
		 * §5 step 10, as every other reader of a map applies it:
		 * an entry that failed its csum128 and that replay did not
		 * touch is media damage the log cannot repair, and every
		 * grain number in it is the damaged bytes'.  Marking those
		 * numbers would free grains a live entry still names and
		 * publish a checkpoint calling the slot healthy, so the
		 * slot is condemned and its map skipped — which is also
		 * what keeps it out of the allocator, since completemaps
		 * counts a bad slot as used.
		 */
		if(c->bad){
			emapunpin(s, c);
			storecondemn(s, slot);
			continue;
		}
		for(i = 0; i < nblk && i < s->sb.nblkmax; i++){
			g = emapgrain(c->p, i);
			if(g != 0 && g < s->sb.ngrains)
				bitset(s->bmap, g);
		}
		emapunpin(s, c);
	}
	for(i = 0; i < s->nbmpage; i++)
		s->bmdirty[i] = 1;
	s->ndirtypage += s->nbmpage;
	return 0;
}

/* §5 step 11: complete the free map and build the two slot free lists */
static void
completemaps(Store *s)
{
	Ient *e;
	uvlong i;
	ulong slot;

	if(!bitget(s->bmap, 0))
		bitset(s->bmap, 0);
	s->grainfree = 0;
	for(i = 0; i < s->sb.ngrains; i++)
		if(!bitget(s->bmap, i))
			s->grainfree++;

	memset(s->slotused, 0, (s->sb.nslots + 7)/8);
	memset(s->slotresv, 0, (s->sb.nslots + 7)/8);
	memset(s->emapused, 0, (s->sb.nemap + 7)/8);
	memset(s->emapresv, 0, (s->sb.nemap + 7)/8);
	s->slotfree = s->sb.nslots;
	s->emapfree = s->sb.nemap;
	bitset(s->emapused, 0);		/* slot 0 is reserved (§2.3) */
	s->emapfree--;
	for(slot = 0; slot < s->sb.nslots; slot++){
		e = &s->idx[slot];
		if(e->state == Sfree && !e->bad)
			continue;
		bitset(s->slotused, slot);
		s->slotfree--;
		if(e->state != Sfree && e->emapslot != 0
		&& e->emapslot < s->sb.nemap
		&& !bitget(s->emapused, e->emapslot)){
			bitset(s->emapused, e->emapslot);
			s->emapfree--;
		}
	}
}

/*
 * /lost, layer-a §7.5 and §8: every copy this instance holds that
 * fails local verification.  Two conditions put a slot on it and both
 * are the same statement about the copy — §5 step 10's condemnation
 * (Ient.bad, an extent map the log cannot repair) and §8's durable
 * corrupt flag (Icorrupt, a scrub that found a block that does not
 * hash to its digest).  A condemned slot carries both.  The list is
 * therefore membership rather than a log: lostadd and lostdel are
 * idempotent, so a slot that reaches both conditions appears once and
 * a transition that changes only one of them cannot lose the other.
 *
 * Caller holds qlstate: storecondemn reallocs the array from any
 * worker proc, on the first read of a damaged extent map.
 */
static int
lostadd(Store *s, ulong slot)
{
	ulong i, *l;

	for(i = 0; i < s->nlost; i++)
		if(s->lost[i] == slot)
			return 0;
	if((l = realloc(s->lost, (s->nlost+1)*sizeof *l)) == nil){
		werrstr("out of memory");
		return -1;
	}
	s->lost = l;
	s->lost[s->nlost++] = slot;
	return 0;
}

static void
lostdel(Store *s, ulong slot)
{
	ulong i;

	for(i = 0; i < s->nlost; i++)
		if(s->lost[i] == slot){
			memmove(&s->lost[i], &s->lost[i+1],
				(s->nlost - i - 1)*sizeof *s->lost);
			s->nlost--;
			return;
		}
}

/*
 * An apply is the one thing that changes an entry's Icorrupt, and it
 * rebuilds a condemned slot's map (§3.6), so it decides the slot's
 * membership afresh from the entry it just wrote rather than from the
 * transition it made.
 */
void
lostupdate(Store *s, ulong slot)
{
	Ient *e;

	if(slot >= s->sb.nslots)
		return;
	e = &s->idx[slot];
	if(e->state != Sfree && (e->bad || (e->flags & Icorrupt) != 0))
		lostadd(s, slot);
	else
		lostdel(s, slot);
}

/*
 * §5 step 10, at run time.  An extent-map entry that fails its
 * csum128 and that replay did not touch is media damage the log
 * cannot repair, and it is found when the object is first read rather
 * than at start (§5 step 9 reads only the entries replay touched).
 * The slot goes to /lost with kind=corrupt, and it is not reused,
 * because completemaps counts a bad slot as used.
 *
 * The entry stays hashed and takes Icorrupt.  D14: a copy that fails
 * local verification MUST answer with corrupt=1 and MUST NOT answer
 * absent=1, because absence is a §1.5 positive confirmation this
 * holder cannot vouch for — and unhashing the slot is exactly
 * absence, for the life of the disk, since /lost carries no oid.
 * What "not served" means is that no path reads through the damaged
 * map: objread, objverify and every update but §5.5's op=full refuse
 * on bad, and op=full rebuilds the map whole in the slot and at the
 * qid.path the object already had.
 */
void
storecondemn(Store *s, ulong slot)
{
	if(slot >= s->sb.nslots || s->idx[slot].bad)
		return;
	s->idx[slot].bad = 1;
	s->idx[slot].flags |= Icorrupt;
	/*
	 * §8's stamp moves here too, although no map changed: what
	 * changed is that this slot's map may no longer be READ, and a
	 * walk that had already copied it would otherwise fold the
	 * damaged bytes' grain numbers into its shadow.  The stamp is
	 * "what this slot says has moved", not "the four-tuple has".
	 */
	s->idx[slot].gen++;
	/*
	 * Ient.bad is memory only; Icorrupt is the half §2.3 writes, and
	 * the checkpoint writes an index page only when something
	 * dirtied it.  Without this the condemnation reaches the disk
	 * only if some other commit happened to touch the same page, so
	 * a restart would drop a copy known to fail local verification
	 * out of layer-a §7.5's /lost until something read it again.
	 * The caller holds qlstate, which is what idxdirty wants.
	 */
	idxdirty(s, slot);
	lostadd(s, slot);
}

/* §5 step 10: condemn what replay did not restore, and §8's flag as
 * the index carries it — an apply during replay has already updated
 * the slots it touched, and lostadd is idempotent. */
static int
condemn(Store *s)
{
	Ient *e;
	ulong slot;

	for(slot = 0; slot < s->sb.nslots; slot++){
		e = &s->idx[slot];
		if((e->bad || (e->state != Sfree
			&& (e->flags & Icorrupt) != 0))
		&& lostadd(s, slot) < 0)
			return -1;
	}
	return 0;
}

/*
 * §8's online bitmap rebuild (D18), as the four engine calls shoal.h
 * describes.  The pass that drives them — the proc, its rate limit,
 * the queue each fold is pushed through, the peer fetch beside it —
 * is the server's and is not built; what is here is one slot's worth
 * of work at a time and the swap, which is what lets the reclaim
 * happen on a store that is serving instead of on one taken down for
 * rebuildbitmap above.
 *
 * Three things make it safe, and none of them bends §7:
 *
 *   - the map reads happen outside qlstate under the cache's pin, as
 *     every other map read does (§7 rules 1 and 2), so the per-slot
 *     hold is an entry copy plus at most nblkmax bit-sets;
 *   - the entry a fold re-reads is validated by its generation stamp
 *     and not by its four-tuple, which block repair and the
 *     corrupt-flag commit leave unchanged while the map moves (§2.7);
 *   - the barrier in alloc.c keeps the shadow current from the begin
 *     to the last page of the swap, which is what makes installing
 *     the pages one at a time safe.
 */

/* the shadow's bit for grain g; caller holds qlstate */
static void
shadowgrain(Store *s, ulong g)
{
	if(g != 0 && g < s->sb.ngrains)
		bitset(s->bmshadow, g);
}

/*
 * The coverage mark: this pass's shadow holds every grain the slot
 * names.  A fold sets it when it completes; the apply sets it for a
 * record that rebuilds the slot's map whole, because those grains
 * reached the shadow through the barrier and no fold is owed.  Only
 * Slive slots are ever asked for it (bmpassend), but every completed
 * fold sets it, so a slot that was free when the walk passed it and
 * is filled later is marked twice rather than not at all.
 *
 * Caller holds qlstate.  Inert with no pass live, which is what lets
 * the apply call it unconditionally.
 */
static void
foldmark(Store *s, ulong slot)
{
	if(s->bmfoldmark != nil)
		s->bmfoldmark[slot] = 1;
}

void
bmcovered(Store *s, ulong slot)
{
	if(slot < s->sb.nslots)
		foldmark(s, slot);
}

/*
 * §6's leak, recorded while a pass is live.  If the slot is already
 * marked, the fold that marked it put the grains this apply is
 * leaking into the shadow, so the swap installs them marked and named
 * by nothing: the count outlives the swap.  If it is not marked, the
 * fold that comes will not fold a condemned slot's map and the swap
 * returns the grains, which is what discharges the count.
 *
 * Caller holds qlstate.
 */
void
bmleaked(Store *s, ulong slot, uvlong n)
{
	if(s->bmfoldmark != nil && slot < s->sb.nslots
	&& s->bmfoldmark[slot])
		s->bmleakafter += n;
}

/* drop a live pass; caller holds qlstate */
static void
bmpassdrop(Store *s)
{
	free(s->bmshadow);
	s->bmshadow = nil;
	free(s->bmfoldmark);
	s->bmfoldmark = nil;
	/*
	 * A fold parked at §13's hold point is woken whatever released
	 * the pass, so an abort — or a storeclose — cannot be made to
	 * wait for a hook that nobody is going to set.
	 */
	s->bmfoldhold = 0;
	s->bmfoldgo = 1;
	s->bmswaphold = 0;
	s->bmswapgo = 1;
	s->bmswapping = 0;
	rwakeupall(&s->bmrz);
}

int
bmpassbegin(Store *s)
{
	uchar *p, *m;

	if(!storeserving(s))
		return -1;
	if((p = mallocz(s->nbmpage*(bmbits(s->sb.blksz)/8), 1)) == nil){
		werrstr("out of memory");
		return -1;
	}
	if((m = mallocz(s->sb.nslots, 1)) == nil){
		free(p);
		werrstr("out of memory");
		return -1;
	}
	qlock(&s->qlstate);
	if(s->bmshadow != nil){
		qunlock(&s->qlstate);
		free(p);
		free(m);
		werrstr("bitmap rebuild: a pass is already running");
		return -1;
	}
	s->bmshadow = p;
	s->bmfoldmark = m;
	bitset(s->bmshadow, 0);		/* grain 0 is never allocatable */
	s->bmnfold = 0;
	s->bmnreread = 0;
	s->bmleakafter = 0;
	qunlock(&s->qlstate);
	return 0;
}

void
bmpassabort(Store *s)
{
	qlock(&s->qlstate);
	/*
	 * An abort while bmpassend is installing pages does nothing.
	 * The swap drops qlstate between pages, and an abort landing in
	 * that gap would free the shadow out from under a half-installed
	 * bitmap and leave §6's free count moved by the pages that did
	 * land — the one state this mechanism has no name for.  The end
	 * drops the pass itself a moment later, so there is nothing left
	 * for the abort to do, which is what lets it stay a call that
	 * cannot fail.
	 */
	if(!s->bmswapping)
		bmpassdrop(s);
	qunlock(&s->qlstate);
}

enum
{
	/*
	 * How many times a fold re-reads a slot that moved under it
	 * before it stops re-reading.  A slot under a continuous write
	 * rate would otherwise starve the walk on that one object; past
	 * this the fold takes the grains from the pinned entry under
	 * qlstate itself, which is not a device read and cannot race the
	 * apply because the apply mutates the map under that same lock.
	 * The stamp is still what the ordinary path validates, and this
	 * is only how the loop is made to terminate.  EVERY round counts
	 * against it, including the rounds the fallback itself sends
	 * round again, because a bound a round can reset is not one.
	 */
	Bmfoldtries	= 8,
	/*
	 * And the rounds in all.  Past the bound a fold still starts
	 * over when the entry has stopped naming the map it pinned,
	 * which is the one case the fallback cannot answer from those
	 * bytes; this is what keeps a slot whose extent-map slot moves
	 * under every round from looping for ever.  Reaching it is a
	 * refusal the caller retries, not a failed pass.
	 */
	Bmfoldmax	= 16,
};

/* what one round of a fold decided */
enum { Fdone, Fagain, Ffail };

int
bmpassfold(Store *s, ulong slot)
{
	Ient *e;
	Emape *c;
	ulong gen, eslot, i, n, try;
	uvlong nblk;
	int r, act;

	if(!storeserving(s))
		return -1;
	if(slot >= s->sb.nslots){
		werrstr("bitmap rebuild: slot %lud, nslots %lud", slot,
			s->sb.nslots);
		return -1;
	}
	r = -1;
	for(try = 0; try < Bmfoldmax; try++){
		qlock(&s->qlstate);
		if(s->bmshadow == nil){
			qunlock(&s->qlstate);
			werrstr("bitmap rebuild: no pass is running");
			break;
		}
		e = &s->idx[slot];
		/*
		 * A free slot names nothing, and a slot §5 step 10
		 * condemned names nothing the store may believe: its extent
		 * map is the damage, and the grains it named are exactly
		 * what this pass is here to reclaim.  §3.6's op=full over a
		 * condemned slot leaks the old grains as a delete does and
		 * clears e->bad, so the store no longer remembers the slot
		 * was condemned — which costs this walk nothing, because it
		 * folds what the live maps say now and the rebuilt map is
		 * what the entry names either way.
		 */
		if(e->state == Sfree || e->bad){
			if(e->state == Slive)
				s->bmnfold++;
			foldmark(s, slot);
			qunlock(&s->qlstate);
			r = 0;
			break;
		}
		gen = e->gen;
		eslot = e->emapslot;
		if(eslot == 0){
			/* §2.3's inline map: the entry is its own map */
			if(blkcount(e->len, s->sb.blksz) > 0)
				shadowgrain(s, e->grain0);
			if(e->state == Slive)
				s->bmnfold++;
			foldmark(s, slot);
			qunlock(&s->qlstate);
			r = 0;
			break;
		}
		qunlock(&s->qlstate);

		if((c = emapget(s, eslot, 0)) == nil){
			/*
			 * A device error or an exhausted cache, not a dead
			 * pass: this slot is the caller's to fold again,
			 * and the text is what tells the two apart.
			 */
			werrstr("bitmap rebuild: slot %lud: map read: %r",
				slot);
			break;
		}

		qlock(&s->qlstate);
		/*
		 * In flight from here: this fold holds a map read for the
		 * pass, and bmpassend installs nothing while one does.  A
		 * fold that has not got this far has written nothing into
		 * the shadow and finds the pass gone, which it reports as
		 * a retryable refusal rather than as coverage.
		 */
		s->bmnflight++;
		/*
		 * §13's bmfold point: park this fold between the map read
		 * and every validation it makes — the stamp's and the
		 * checksum's alike — so a test can land a commit in the
		 * window the stamp exists for instead of racing for it.
		 * rsleep drops qlstate, which is what lets that commit
		 * apply; inert unless the hook armed it.
		 */
		if(s->bmfoldhold > 0){
			s->bmfoldhold--;
			s->bmfoldgo = 0;
			while(!s->bmfoldgo)
				rsleep(&s->bmrz);
		}
		act = Fdone;
		if(s->bmshadow == nil){
			werrstr("bitmap rebuild: no pass is running");
			act = Ffail;
		}else if(e->gen != gen && try < Bmfoldtries){
			s->bmnreread++;
			act = Fagain;
		}else if(e->gen != gen
		&& (e->state == Sfree || e->bad || e->emapslot != eslot)){
			/*
			 * The loop's bound, reached, and the entry no longer
			 * names the map this round pinned: round again with
			 * a fresh pin, and count this round like any other.
			 */
			act = Fagain;
		}else if(c->bad){
			/*
			 * The entry failed its csum128: media damage the log
			 * cannot repair, exactly as rebuildbitmap finds it,
			 * and every grain number in it is the damaged bytes'.
			 * So this reader condemns the slot as every other
			 * reader of a map does, and folds nothing.
			 *
			 * The stamp is checked first, above, and a mismatch
			 * sends the fold round again rather than here: a
			 * repair or §3.6's op=full may have published a
			 * fresh, good map over the damage since the read,
			 * and condemning on that would condemn the new map
			 * while folding nothing would let the swap clear
			 * the grains it names.
			 */
			storecondemn(s, slot);
			if(e->state == Slive)
				s->bmnfold++;
			foldmark(s, slot);
			r = 0;
		}else{
			/*
			 * The fold, and at the bound the fallback with the
			 * stamp moved: either way the grains come straight
			 * off the pinned entry, as §5 step 11's rebuild
			 * reads them, under the lock the apply mutates them
			 * under.  Copying them out first would buy nothing —
			 * the hold is the same nblkmax bit-sets §7 costs it
			 * either way — and cost an allocation per slot.
			 */
			nblk = blkcount(e->len, s->sb.blksz);
			if(nblk > s->sb.nblkmax)
				nblk = s->sb.nblkmax;
			n = nblk;
			for(i = 0; i < n; i++)
				shadowgrain(s, emapgrain(c->p, i));
			if(e->state == Slive)
				s->bmnfold++;
			foldmark(s, slot);
			r = 0;
		}
		s->bmnflight--;
		qunlock(&s->qlstate);
		emapunpin(s, c);
		if(act != Fagain)
			break;
	}
	if(try >= Bmfoldmax)
		werrstr("bitmap rebuild: slot %lud: the map will not hold "
			"still", slot);
	return r;
}

/*
 * The clear bits in one page of a bitmap, over the first nbit bits of
 * it.  §2.5's last page is padded out to the page size and §5 step
 * 11's count stops at ngrains, so a page counted to its end would
 * make the swap's count and the start-up count disagree by however
 * much padding the geometry leaves.  Caller holds qlstate.
 */
static uvlong
pagefree(uchar *p, uvlong nbit)
{
	uvlong i, n;
	int b;

	n = 0;
	for(i = 0; i + 8 <= nbit; i += 8)
		if(p[i/8] != 0xff)
			for(b = 0; b < 8; b++)
				if((p[i/8] & (1<<b)) == 0)
					n++;
	for(; i < nbit; i++)
		if((p[i/8] & (1<<(i%8))) == 0)
			n++;
	return n;
}

/*
 * §8's coverage interlock: the swap frees every grain the shadow does
 * not mark, so a walk that skipped a live slot would free the grains
 * that slot names.  A pass therefore ends only when every live slot
 * is marked — by the fold that folded it, or by an apply that
 * rebuilt its map whole under the barrier — and no fold is holding a
 * map read.  A tomb slot needs no mark: the Eobj that made it one
 * carries len=0, an empty nmap and emapslot=0 (§6), so it names no
 * grain and folds to nothing; a free slot names nothing either.
 *
 * The refusal installs nothing and leaves the pass live, so the
 * caller folds what it missed and ends again.  Caller holds qlstate.
 */
static int
coverok(Store *s)
{
	ulong slot;

	if(s->bmnflight != 0){
		werrstr("bitmap rebuild: %lud folds in flight",
			s->bmnflight);
		return -1;
	}
	for(slot = 0; slot < s->sb.nslots; slot++)
		if(s->idx[slot].state == Slive && !s->bmfoldmark[slot]){
			werrstr("bitmap rebuild: slot %lud not folded", slot);
			return -1;
		}
	return 0;
}

int
bmpassend(Store *s, uvlong *npage)
{
	uvlong bits, bytes, nbit, i, n, was, now;

	if(npage != nil)
		*npage = 0;
	if(!storeserving(s))
		return -1;
	bits = bmbits(s->sb.blksz);
	bytes = bits/8;
	qlock(&s->qlstate);
	if(s->bmshadow == nil){
		qunlock(&s->qlstate);
		werrstr("bitmap rebuild: no pass is running");
		return -1;
	}
	if(coverok(s) < 0){
		qunlock(&s->qlstate);
		return -1;
	}
	s->bmswapping = 1;
	s->bmswapped = 0;
	qunlock(&s->qlstate);
	n = 0;
	for(i = 0; i < s->nbmpage; i++){
		qlock(&s->qlstate);
		if(s->bmshadow == nil){
			s->bmswapping = 0;
			qunlock(&s->qlstate);
			werrstr("bitmap rebuild: no pass is running");
			return -1;
		}
		if(memcmp(s->bmap + i*bytes, s->bmshadow + i*bytes,
			bytes) != 0){
			/*
			 * §6's free count moves by what this page changed
			 * rather than being recomputed over the whole
			 * bitmap at the end: the recount would be a qlstate
			 * hold proportional to the disk — 2.6*10^8 bits on
			 * a 4 TB one — which is the hold chunking the swap
			 * exists to avoid.  The staged set needs no term of
			 * its own here for the same reason it needs no
			 * barrier: a staged grain carries no bit in either
			 * copy (§6), so it contributes to neither count and
			 * the difference passes it over.
			 */
			nbit = bits;
			if(i*bits >= s->sb.ngrains)
				nbit = 0;
			else if(s->sb.ngrains - i*bits < bits)
				nbit = s->sb.ngrains - i*bits;
			was = pagefree(s->bmap + i*bytes, nbit);
			now = pagefree(s->bmshadow + i*bytes, nbit);
			memmove(s->bmap + i*bytes, s->bmshadow + i*bytes,
				bytes);
			s->grainfree += now - was;
			if(!s->bmdirty[i]){
				s->bmdirty[i] = 1;
				s->ndirtypage++;
			}
			n++;
		}
		s->bmswapped = n;
		/*
		 * §13's bmswap point: park in the hold that installed this
		 * page, which is the gap between two pages an abort or a
		 * close would land in.  Inert unless the hook armed it.
		 */
		if(s->bmswaphold > 0){
			s->bmswaphold--;
			s->bmswapgo = 0;
			while(!s->bmswapgo)
				rsleep(&s->bmrz);
		}
		qunlock(&s->qlstate);
	}
	qlock(&s->qlstate);
	if(s->bmshadow == nil){
		s->bmswapping = 0;
		qunlock(&s->qlstate);
		werrstr("bitmap rebuild: no pass is running");
		return -1;
	}
	/*
	 * Every grain a live map names is marked and nothing else is, so
	 * §6's count of what a condemned slot left behind is discharged:
	 * this is the rebuild D18 says the grains come back at.  What is
	 * left is what leaked AFTER the pass had folded the slot — the
	 * fold put those grains in the shadow, so the swap installs them
	 * marked and named by nothing, and the count carries over to the
	 * next pass, which is the one that returns them.
	 */
	s->grainleak = s->bmleakafter;
	s->bmswapped = n;
	bmpassdrop(s);
	qunlock(&s->qlstate);
	if(npage != nil)
		*npage = n;
	return 0;
}

/*
 * The Store's own memory, released by whichever path gave up the last
 * claim on it: a storeopen that failed part-way, storeclose, or — for
 * a store closed under an open snapshot (§9) — the last objsnapclose.
 * It touches s->d nowhere, so the caller's device may already be
 * closed; and it destroys exactly what objsnapent renders, which is
 * why §9 defers it rather than letting a snapshot read through it.
 */
void
storefree(Store *s)
{
	Peer *p, *pn;
	ulong i;

	if(s == nil)
		return;
	if(s->idx != nil){
		for(i = 0; i < s->sb.nslots; i++)
			free(s->idx[i].oid);
		free(s->idx);
	}
	if(s->dirt != nil){
		for(i = 0; i < s->sb.ndirty; i++)
			free(s->dirt[i]);
		free(s->dirt);
	}
	for(p = s->peers; p != nil; p = pn){
		pn = p->next;
		free(p);
	}
	emapfreeall(s);
	free(s->ehash);
	free(s->hash);
	free(s->slotused);
	free(s->slotresv);
	free(s->emapused);
	free(s->emapresv);
	free(s->bmap);
	free(s->bmdirty);
	free(s->bmshadow);		/* §8's shadow, if a pass was live */
	free(s->bmfoldmark);
	free(s->idxdirty);
	free(s->dirtdirty);
	free(s->stagebuck);
	free(s->zeroblk);
	free(s->lost);
	/*
	 * §13's freed hook, last: the Store's fields are still readable
	 * here, and this is the one place any of them stops being so, so
	 * a test can say "the memory is gone" and mean exactly that.
	 */
	if(s->cfg.freed != nil)
		(*s->cfg.freed)(s->cfg.freedarg);
	free(s);
}

static void
setdefaults(Storecfg *c)
{
	if(c->logdepth == 0)
		c->logdepth = Logdepthdflt;
	if(c->logdepth > Logdepthmax)
		c->logdepth = Logdepthmax;
	if(c->ckwaitms == 0)
		c->ckwaitms = Ckwaitmsdflt;
	if(c->stagemax == 0)
		c->stagemax = Stagemaxdflt;
	if(c->stagetot == 0)
		c->stagetot = Stagetotdflt;
	if(c->stagems == 0)
		c->stagems = Stagemsdflt;
	if(c->emapcache == 0)
		c->emapcache = Emapcachedflt;
	if(c->objsnapmax == 0)
		c->objsnapmax = Objsnapmaxdflt;
	/*
	 * §2.8's triggers.  They only ever fire in the checkpointer
	 * proc, so a caller that wants no automatic checkpoint at all —
	 * a T1 program driving one by hand — asks for no proc rather
	 * than for zero triggers.
	 */
	if(c->ckhigh == 0)
		c->ckhigh = Ckhighdflt;
	if(c->ckms == 0)
		c->ckms = Ckmsdflt;
	/*
	 * §2.8's retry floor.  Zero is the unset value and not `no
	 * floor': a store that retried a failing checkpoint with no
	 * wait at all is the condition the floor exists to remove, so
	 * there is no way to ask for one.
	 */
	if(c->ckbackms == 0)
		c->ckbackms = Ckbackmsdflt;
}

Store*
storeopen(Dev *d, Storecfg *cfg)
{
	Store *s;
	Sbsel sel;
	Peer *p;
	uvlong bits, hi;
	ulong i;

	if((s = mallocz(sizeof *s, 1)) == nil)
		return nil;
	s->d = d;
	s->cfg = *cfg;
	setdefaults(&s->cfg);
	s->emaprz.l = &s->qlemap;
	s->roomrz.l = &s->qllog;
	s->relrz.l = &s->qllog;
	s->donerz.l = &s->qllog;
	s->holdrz.l = &s->qllog;
	s->snaprz.l = &s->qlstate;
	s->bmrz.l = &s->qlstate;
	s->flrz.l = &s->fllk;
	s->ckrz.l = &s->cklk;
	s->procrz.l = &s->proclk;

	/*
	 * §5 step 1 and §3.2.  If the flush channel cannot be opened the
	 * store MUST NOT start unless -w was given, which asserts that
	 * the unit is write-through or its write cache disabled.  -w is
	 * an operator claim, not an observation, so it is reported as
	 * flush=asserted-writethrough and never as flush=raw.
	 */
	if(d->rdonly)
		/*
		 * A device opened read-only writes nothing at all — §0's
		 * devwrite refuses on the flag — so there is no
		 * durability to assert and no raw channel to want, and
		 * §12's shoalck -v opens the store this way to replay it
		 * in memory.  The mode stays what the device reported,
		 * which for a read-only open is `not examined': calling
		 * it asserted would be a claim no operator made.
		 */
		s->flushmode = d->flushmode;
	else if(d->flushmode == Fraw)
		s->flushmode = Fraw;
	else if(cfg->noflush)
		s->flushmode = Fasserted;
	else{
		werrstr("%s: no flush channel: refusing to start without -w",
			d->name);
		storefree(s);
		return nil;
	}

	/* §5 step 2: take the valid superblock with the greater gen */
	if(superselect(d, &sel) < 0){
		werrstr("%s: no valid superblock: shoalck, then shoalfmt -r "
			"and refill from peers", d->name);
		storefree(s);
		return nil;
	}
	s->sb = sel.sb[sel.start];
	s->pub = s->sb;
	if(geomok(s, d) < 0){
		werrstr("%s: geometry: %r", d->name);
		storefree(s);
		return nil;
	}

	s->nbmpage = nbmpage(&s->sb);
	bits = bmbits(s->sb.blksz);
	s->nidxpage = (uvlong)s->sb.nslots*Idxentsz / s->sb.blksz;
	if((uvlong)s->sb.nslots*Idxentsz % s->sb.blksz != 0)
		s->nidxpage++;
	s->ndirtpage = (uvlong)s->sb.ndirty*Dirtentsz / s->sb.blksz;
	if((uvlong)s->sb.ndirty*Dirtentsz % s->sb.blksz != 0)
		s->ndirtpage++;
	s->emapcap = s->cfg.emapcache;
	s->nhash = pow2ge(s->sb.nslots);
	s->nehash = pow2ge(s->emapcap);
	s->nstagebuck = pow2ge(s->cfg.stagetot + 64);
	s->logresv = s->sb.logsecs/Logresvdiv;
	if(s->logresv < 1)
		s->logresv = 1;
	s->logdepth = s->cfg.logdepth;

	s->idx = mallocz(s->sb.nslots*sizeof *s->idx, 1);
	s->hash = malloc(s->nhash*sizeof *s->hash);
	s->slotused = mallocz((s->sb.nslots + 7)/8, 1);
	s->slotresv = mallocz((s->sb.nslots + 7)/8, 1);
	s->emapused = mallocz((s->sb.nemap + 7)/8, 1);
	s->emapresv = mallocz((s->sb.nemap + 7)/8, 1);
	s->bmap = mallocz(s->nbmpage*(bits/8), 1);
	s->bmdirty = mallocz(s->nbmpage, 1);
	s->idxdirty = mallocz(s->nidxpage, 1);
	s->dirtdirty = mallocz(s->ndirtpage, 1);
	s->dirt = mallocz(s->sb.ndirty*sizeof *s->dirt, 1);
	s->ehash = mallocz(s->nehash*sizeof *s->ehash, 1);
	s->stagebuck = mallocz(s->nstagebuck*sizeof *s->stagebuck, 1);
	s->zeroblk = mallocz(s->sb.blksz, 1);
	if(s->idx == nil || s->hash == nil || s->slotused == nil
	|| s->slotresv == nil || s->emapused == nil || s->emapresv == nil
	|| s->bmap == nil || s->bmdirty == nil || s->idxdirty == nil
	|| s->dirtdirty == nil || s->dirt == nil || s->ehash == nil
	|| s->stagebuck == nil || s->zeroblk == nil){
		werrstr("out of memory");
		storefree(s);
		return nil;
	}
	for(i = 0; i < s->nhash; i++)
		s->hash[i] = ~0UL;
	for(i = 0; i < s->sb.nslots; i++)
		s->idx[i].hashnext = ~0UL;
	blkdigest(s->zeroblk, s->sb.blksz, s->zerodig);
	s->slotfree = s->sb.nslots;
	s->emapfree = s->sb.nemap;
	s->grainfree = s->sb.ngrains;

	/* steps 4, 5 and 6 */
	if(readindex(s) < 0 || readbitmap(s) < 0 || readdirty(s) < 0){
		werrstr("%s: %r", d->name);
		storefree(s);
		return nil;
	}

	/* step 7 */
	if(replay(s) < 0){
		werrstr("%s: replay: %r", d->name);
		storefree(s);
		return nil;
	}

	/*
	 * Step 8: the replay-coverage rule (§2.5).  A page ahead of the
	 * superblock is the ordinary state after a crash between §2.8's
	 * step 1 and step 3, so a generation comparison cannot be the
	 * test; what has to be checked is whether the log still covers
	 * the state already materialised.  Replay applying no record is
	 * not itself a failure — after a quiescent restart the
	 * superblock's term carries the test.
	 */
	hi = s->sb.ckseq;
	if(s->nreplay > 0 && s->replayhigh > hi)
		hi = s->replayhigh;
	if(hi < s->pmax){
		werrstr("%s: bitmap page at ckseq %llud, log covers only "
			"%llud: the log no longer covers what the disk holds; "
			"shoalck, then refill from peers", d->name, s->pmax,
			hi);
		storefree(s);
		return nil;
	}

	/*
	 * Steps 10 and 11.  §12's shoalck -R is step 5's flag set by
	 * hand: a page that fails its checksum is rebuilt here anyway,
	 * and -R is for the page that is valid but wrong and for the
	 * operator who wants the scan done now rather than at the next
	 * start.  The rebuild scans the *replayed* maps, so a grain a
	 * committed-but-not-checkpointed record allocated is counted.
	 */
	if(s->cfg.forcerebuild)
		s->bmaprebuild = 1;
	if(condemn(s) < 0){
		storefree(s);
		return nil;
	}
	if(s->bmaprebuild && rebuildbitmap(s) < 0){
		werrstr("%s: bitmap rebuild: %r", d->name);
		storefree(s);
		return nil;
	}
	completemaps(s);

	/*
	 * Step 12: fullsync for every peer — the safe default, and a
	 * restart has to run a reconcile pass anyway.  cur is cleared
	 * for every object, which costs nothing because cur is never on
	 * disk (R5, §14(1)).
	 */
	for(p = s->peers; p != nil; p = p->next)
		p->fullsync = 1;
	for(i = 0; i < s->sb.nslots; i++)
		s->idx[i].cur = 0;

	/* step 13 */
	s->logstart = s->sb.cklogoff - s->sb.logoff;
	s->qidcur = s->pub.qidnext;
	s->cklast = nsec();
	if(s->cfg.spawn != nil && !s->cfg.nockptproc){
		if(storeproc(s, ckptproc, s) < 0){
			werrstr("%s: checkpointer: %r", d->name);
			storefree(s);
			return nil;
		}
		s->ckproc = 1;
	}
	return s;
}

/*
 * Stop the procs and give up the store's own claim on its memory; it
 * writes nothing, and the device is the caller's.  §9: an object
 * snapshot MAY still be open, and then this frees nothing — the
 * snapshot's reads answer `store closed' from a Store that is still
 * there, and the last objsnapclose releases it.  Neither the fatal
 * this used to take nor the lie the fatal was chosen over: a store
 * freed under a snapshot leaves objsnapent rendering from freed
 * memory, where it finds no qid.path match and answers 0, "that
 * entry is gone", so a fid-lifetime bug in a server would surface as
 * a silently short /obj listing.
 */
void
storeclose(Store *s)
{
	int last;

	if(s == nil)
		return;
	qlock(&s->cklk);
	s->stop = 1;
	rwakeupall(&s->ckrz);
	qunlock(&s->cklk);
	qlock(&s->qllog);
	rwakeupall(&s->holdrz);
	rwakeupall(&s->roomrz);
	rwakeupall(&s->donerz);
	rwakeupall(&s->relrz);
	qunlock(&s->qllog);
	qlock(&s->fllk);
	rwakeupall(&s->flrz);
	qunlock(&s->fllk);
	qlock(&s->proclk);
	while(s->nproc > 0)
		rsleep(&s->procrz);
	qunlock(&s->proclk);
	/*
	 * The store's own reference, given up only now — AFTER the proc
	 * wait, not at the top of the call.  `closed' is what makes the
	 * free reachable by anyone else, so setting it early would let a
	 * last objsnapclose free the Store while this call is still
	 * asleep inside it on s->procrz, and both would then free: a
	 * double free and a fault, measured.
	 *
	 * Free after unlocking, and no party this call is answerable for
	 * is waiting on the qlstate inside the memory about to go: the
	 * ones that block on it holding a claim are an objsnapent or
	 * objsnapclose of a snapshot whose count is not yet given back,
	 * an objsnapopen that has taken §9's slot, and this call — and
	 * the procs are gone — so a true predicate says there is none.
	 * The calls that block on it holding no claim at all (dirtysnap,
	 * lostsnap, fullsyncsnap, storestat, the object API, an
	 * objsnapopen before the slot) would wake in freed memory, and
	 * §9 makes it the caller's obligation that none of them is still
	 * in flight here: quiesce, then close.
	 */
	qlock(&s->qlstate);
	s->closed = 1;
	/*
	 * §8's rebuild pass, if one is still live.  D16 makes every call
	 * on a closed store undefined, so a pass MUST have been ended or
	 * aborted before this call; dropping one here is what keeps the
	 * shadow from outliving the Store's other memory, not a licence
	 * to leave a pass open.  A pass with no call in flight is left
	 * with its live bitmap exactly as it was, which is an abort.  A
	 * bmpassend in flight in another proc is undefined exactly as
	 * any other call in flight is: the swap installs a page at a
	 * time under this same lock, so what the bitmap holds afterwards
	 * is however many pages had landed.
	 */
	bmpassdrop(s);
	rwakeupall(&s->snaprz);		/* §13's snaphold point, if one parked */
	last = s->nobjsnap == 0;
	qunlock(&s->qlstate);
	if(last)
		storefree(s);
}

void
storestat(Store *s, Storestat *st)
{
	memset(st, 0, sizeof *st);
	st->flushmode = s->flushmode;
	st->bmaprebuild = s->bmaprebuild;
	qlock(&s->qlsuper);
	st->ckseq = s->pub.ckseq;
	st->cklogoff = s->pub.cklogoff;
	st->qidnext = s->pub.qidnext;
	st->epochhigh = s->pub.epochhigh;
	st->monidset = s->pub.monidset;
	qunlock(&s->qlsuper);
	qlock(&s->qllog);
	st->watermark = s->watermark;
	st->seqnext = s->seqnext;
	st->logfree = logfree(s);
	st->logwait = s->nlogwait;
	st->broken = s->broken;
	qunlock(&s->qllog);
	qlock(&s->qlstate);
	st->grainfree = s->grainfree;
	st->staged = s->nstaged;
	st->grainleak = s->grainleak;
	st->bmpass = s->bmshadow != nil;
	st->bmfolded = s->bmnfold;
	st->bmfolding = s->bmnflight;
	st->bmreread = s->bmnreread;
	st->bmswapped = s->bmswapped;
	st->slotfree = s->slotfree;
	st->emapfree = s->emapfree;
	st->nslots = s->sb.nslots;
	st->nlive = s->nlive;
	st->ntomb = s->ntomb;
	st->nobjsnap = s->nobjsnap;
	st->ndirty = s->ndirtused;
	st->nlost = s->nlost;
	qunlock(&s->qlstate);
	qlock(&s->cklk);
	st->ckfailed = s->ckfailed;
	st->ckstuck = s->ckstuck;
	st->ckdead = s->ckdead;
	strecpy(st->ckerr, st->ckerr + sizeof st->ckerr, s->ckerrstr);
	qunlock(&s->cklk);
	st->ndirtydrop = s->ndirtydrop;
	st->nreplay = s->nreplay;
	st->pmax = s->pmax;
}

/*
 * /lost's i'th slot: every copy that fails local verification, which
 * is §5 step 10's condemned slots and §8's corrupt-flagged entries
 * alike (layer-a §7.5).  storecondemn reallocs s->lost from any
 * worker proc, on the first read of a damaged extent map, so both the
 * count and the array are qlstate's: reading them unlocked — as this
 * could while the list was built once at start — indexes a freed
 * array.
 */
ulong
storelost(Store *s, ulong i)
{
	ulong slot;

	qlock(&s->qlstate);
	slot = i < s->nlost ? s->lost[i] : ~0UL;
	qunlock(&s->qlstate);
	return slot;
}

/*
 * §2.6's coarse flag, half-built: everything that sets it exists —
 * start-up marks every peer, and the dirty-region exhaustion drop
 * marks its victim — and nothing yet clears it, because the clearing
 * belongs to the reconcile pass the heal work will bring.  So today
 * this answers 1 for every peer, known or not, and the exhaustion
 * drop's safety argument leans on exactly that: dropping a peer's
 * records can never make this answer less cautious.
 */
int
storefullsync(Store *s, char *peer)
{
	Peer *p;

	qlock(&s->qlstate);
	for(p = s->peers; p != nil; p = p->next)
		if(strcmp(p->name, peer) == 0){
			qunlock(&s->qlstate);
			return p->fullsync;
		}
	qunlock(&s->qlstate);
	return 1;			/* an unknown peer is behind */
}

/* the dirty set, layer-a §7.1 */
static int
dirtycommit(Store *s, uchar *oid, int oidlen, char *peer, uvlong epoch, int op)
{
	Dirtyrec d;
	Item it;

	if(oidlen < 1 || oidlen > Oidmax || strlen(peer) < 1
	|| strlen(peer) > Peermax){
		werrstr("dirty record: oidlen %d peer %s", oidlen, peer);
		return -1;
	}
	memset(&d, 0, sizeof d);
	d.op = op;
	d.oidlen = oidlen;
	d.peerlen = strlen(peer);
	d.epoch = epoch;
	memmove(d.oid, oid, oidlen);
	memmove(d.peer, peer, d.peerlen);
	memset(&it, 0, sizeof it);
	it.dirty = &d;
	it.ndirty = 1;
	/*
	 * §6's reserved tail is for commits that release space and take
	 * none — an Eobj that frees grains, and an Eslot.  An Edirty
	 * frees no log space in either direction, so neither an add nor
	 * a remove may draw on the reserve: the reserve exists to keep
	 * the traffic that cannot relieve exhaustion out of the last
	 * sectors of the log.
	 */
	it.freeing = 0;
	return logcommit(s, &it);
}

int
dirtyadd(Store *s, uchar *oid, int oidlen, char *peer, uvlong epoch)
{
	return dirtycommit(s, oid, oidlen, peer, epoch, 1);
}

int
dirtydel(Store *s, uchar *oid, int oidlen, char *peer)
{
	return dirtycommit(s, oid, oidlen, peer, 0, 0);
}

int
dirtyhas(Store *s, uchar *oid, int oidlen, char *peer)
{
	Dirtent *t;
	ulong i, n;

	n = strlen(peer);
	qlock(&s->qlstate);
	for(i = 0; i < s->sb.ndirty; i++){
		if((t = s->dirt[i]) == nil)
			continue;
		if(t->oidlen == oidlen && t->peerlen == n
		&& memcmp(t->oid, oid, oidlen) == 0
		&& memcmp(t->peer, peer, n) == 0){
			qunlock(&s->qlstate);
			return 1;
		}
	}
	qunlock(&s->qlstate);
	return 0;
}

ulong
dirtycount(Store *s)
{
	ulong n;

	qlock(&s->qlstate);
	n = s->ndirtused;
	qunlock(&s->qlstate);
	return n;
}
