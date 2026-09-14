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

static void
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
	free(s->idxdirty);
	free(s->dirtdirty);
	free(s->stagebuck);
	free(s->zeroblk);
	free(s->lost);
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
		free(s);
		return nil;
	}

	/* §5 step 2: take the valid superblock with the greater gen */
	if(superselect(d, &sel) < 0){
		werrstr("%s: no valid superblock: shoalck, then shoalfmt -r "
			"and refill from peers", d->name);
		free(s);
		return nil;
	}
	s->sb = sel.sb[sel.start];
	s->pub = s->sb;
	if(geomok(s, d) < 0){
		werrstr("%s: geometry: %r", d->name);
		free(s);
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

void
storeclose(Store *s)
{
	ulong n;

	if(s == nil)
		return;
	/*
	 * §9: a snapshot is the caller's, so this frees none of them —
	 * and a store freed under one leaves every later objsnapent
	 * reading the freed Store, where it finds no qid.path match and
	 * answers 0, "this entry is gone", rather than faulting.  That
	 * is a plausible lie: a fid-lifetime bug in a server becomes a
	 * silently short /obj listing.  There is no answer this can give
	 * that is not one, so it says so out loud instead.
	 */
	qlock(&s->qlstate);
	n = s->nobjsnap;
	qunlock(&s->qlstate);
	if(n > 0)
		sysfatal("storeclose: %lud object snapshot%s still open",
			n, n == 1 ? "" : "s");
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
