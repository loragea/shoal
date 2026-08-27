#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"

/*
 * Geometry (docs/design/store.md §2.1) and format (§12).
 *
 * Every derived quantity is recorded in the superblock so that no
 * reader recomputes it from assumptions.  Region starts are rounded
 * up to a Wunit boundary — §2.1 requires it of the log and it costs
 * at most blksz-secsz bytes for each of the others, which is what
 * keeps every checkpoint page write and every grain write aligned as
 * well as sized to one device request.
 */

static uvlong
roundup(uvlong n, uvlong m)
{
	return (n + m - 1)/m * m;
}

static int
pow2(uvlong n)
{
	return n > 0 && (n & (n-1)) == 0;
}

/* the largest Eobj record this geometry can be asked for (§2.7) */
static uvlong
maxrecbytes(Super *s)
{
	uvlong ent;

	ent = Lenthdrsz + 84 + Oidmax + 4 + 24*(uvlong)s->nblkmax
		+ 4 + 4*(uvlong)s->nblkmax;
	return roundup(Lrechdrsz + ent, s->secsz);
}

int
geometry(Super *s, Fmtcfg *c, vlong partbytes)
{
	uvlong nsec, lastsec, pagesecs, avail, nbm, bpp, need, nobj;
	uvlong logbytes;

	memset(s, 0, sizeof *s);
	if(!pow2(c->secsz)){
		werrstr("secsz %lud is not a power of two", c->secsz);
		return -1;
	}
	if(!pow2(c->blksz) || c->blksz < c->secsz){
		werrstr("blksz %lud is not a power of two at least secsz",
			c->blksz);
		return -1;
	}
	if(!pow2(c->objmax) || c->objmax < c->blksz){
		werrstr("objmax %llud is not a power of two at least blksz",
			c->objmax);
		return -1;
	}
	if(c->csumalg != Csumblake2s){
		werrstr("unknown csumalg %lud", c->csumalg);
		return -1;
	}
	if(partbytes < 0 || (uvlong)partbytes < 16*(uvlong)c->blksz){
		werrstr("partition of %lld bytes is too small", partbytes);
		return -1;
	}

	s->vers = Storevers;
	s->hdrlen = c->secsz;
	s->secsz = c->secsz;
	s->blksz = c->blksz;
	s->objmax = c->objmax;
	s->csumalg = c->csumalg;
	s->nblkmax = c->objmax / c->blksz;
	s->emapsz = roundup(Emaphdrsz + 20*(uvlong)s->nblkmax, c->secsz);

	/*
	 * The defaults need no u32 range check of their own: nslots is
	 * capped at Nslotsmax, and nemap is nobj, which is at most
	 * ngrains because objmax is at least blksz - so the ngrains
	 * bound below is the one that binds.  An operator's own -n and
	 * -e are checked where they are parsed.
	 */
	nobj = (partbytes + c->objmax - 1) / c->objmax;
	s->nslots = c->nslots;
	if(s->nslots == 0){
		s->nslots = 4*nobj > Nslotsmax ? Nslotsmax : 4*nobj;
		if(s->nslots == 0)
			s->nslots = 1;
	}
	s->nemap = c->nemap;
	if(s->nemap == 0)
		s->nemap = nobj < 1 ? 1 : nobj;
	s->ndirty = c->ndirty ? c->ndirty : Ndirtydflt;
	logbytes = c->logbytes ? c->logbytes : Logbytesdflt;

	nsec = (uvlong)partbytes / c->secsz;
	pagesecs = c->blksz / c->secsz;
	lastsec = nsec - 1;			/* superblock copy 1 */

	/*
	 * The reserved run between copy 0 and the log is alignment,
	 * not spare room; nothing reads it and nothing may start using
	 * it without a vers bump.
	 */
	s->logoff = pagesecs;
	s->logsecs = roundup(logbytes, c->blksz) / c->secsz;

	s->idxoff = roundup(s->logoff + s->logsecs, pagesecs);
	s->idxsecs = roundup((uvlong)s->nslots*Idxentsz, c->blksz) / c->secsz;

	s->emapoff = roundup(s->idxoff + s->idxsecs, pagesecs);
	s->emapsecs = roundup((uvlong)s->nemap*s->emapsz, c->blksz) / c->secsz;

	s->dirtoff = roundup(s->emapoff + s->emapsecs, pagesecs);
	s->dirtsecs = roundup((uvlong)s->ndirty*Dirtentsz, c->blksz) / c->secsz;

	s->bmapoff = roundup(s->dirtoff + s->dirtsecs, pagesecs);
	if(s->bmapoff + 2*pagesecs >= lastsec){
		werrstr("partition of %lld bytes leaves no data region",
			partbytes);
		return -1;
	}

	/*
	 * The bitmap sizes itself: it must cover the grains that are
	 * left once it has taken its own pages.  The fixed point is
	 * reached in two or three rounds.
	 */
	avail = lastsec - s->bmapoff;
	bpp = bmbits(c->blksz);
	nbm = 1;
	for(;;){
		s->datasecs = avail - nbm*pagesecs;
		s->ngrains = s->datasecs / pagesecs;
		need = (s->ngrains + bpp - 1) / bpp;
		if(need <= nbm)
			break;
		nbm = need;
		if(nbm*pagesecs >= avail){
			werrstr("partition of %lld bytes leaves no data region",
				partbytes);
			return -1;
		}
	}
	s->bmapsecs = nbm*pagesecs;
	s->dataoff = s->bmapoff + s->bmapsecs;
	s->datasecs = lastsec - s->dataoff;
	s->ngrains = s->datasecs / pagesecs;

	/*
	 * Grain numbers are u32 and grain 0 is reserved to mean "no
	 * grain" — a hole — so a store needs at least two grains and
	 * the superblock field must stay below 2^32.
	 */
	if(s->ngrains < 2){
		werrstr("geometry leaves %llud grains", s->ngrains);
		return -1;
	}
	if(s->ngrains >= (1ULL<<32)){
		werrstr("ngrains %llud reaches 2^32", s->ngrains);
		return -1;
	}
	if(maxrecbytes(s) > (uvlong)s->logsecs*c->secsz/8){
		werrstr("the largest Eobj record (%llud bytes) exceeds an "
			"eighth of a %llud-byte log",
			maxrecbytes(s), (uvlong)s->logsecs*c->secsz);
		return -1;
	}

	s->gen = 0;
	s->ckseq = 0;
	s->cklogoff = s->logoff;
	s->qidnext = 1;
	s->epochhigh = 0;
	s->monidset = 0;
	s->ctime = time(nil);
	if(c->uuidset)
		memmove(s->uuid, c->uuid, 16);
	else
		genrandom(s->uuid, 16);
	return 0;
}

/*
 * Write a formatted store.  Regions first, superblocks last: a crash
 * part way through then leaves no valid superblock rather than a
 * valid one naming regions that were never written.
 *
 * A zeroed index entry, dirty record or bitmap page fails its own
 * checksum, so each is written as a valid *free* record: otherwise a
 * formatted store would condemn every slot at §5 step 10 and start
 * with bmaprebuild=yes at §5 step 5.
 */
int
fmtstore(Dev *d, Super *s)
{
	uchar *buf, *sb;
	Idxent ie;
	Dirtent de;
	Bmpage bh;
	uvlong off, end, npage, i, per;
	ulong j;

	if(s->secsz != d->secsz){
		werrstr("secsz %lud, device sector %lud", s->secsz, d->secsz);
		return -1;
	}
	if((buf = mallocz(s->blksz, 1)) == nil)
		return -1;

	/* the reserved alignment run, and the log */
	if(devzero(d, (vlong)s->secsz, (vlong)(s->logoff - 1)*s->secsz,
		s->blksz) < 0)
		goto bad;
	if(devzero(d, (vlong)s->logoff*s->secsz,
		(vlong)s->logsecs*s->secsz, s->blksz) < 0)
		goto bad;

	/* free index entries */
	memset(&ie, 0, sizeof ie);
	ie.state = Sfree;
	ie.vers = Storevers;
	per = s->blksz / Idxentsz;
	for(j = 0; j < per; j++)
		idxpack(buf + j*Idxentsz, &ie);
	off = (uvlong)s->idxoff*s->secsz;
	end = off + (uvlong)s->idxsecs*s->secsz;
	for(; off < end; off += s->blksz)
		if(devwrite(d, buf, s->blksz, off) < 0)
			goto bad;

	/* the extent-map region: nothing reads a slot no live entry claims */
	if(devzero(d, (vlong)s->emapoff*s->secsz,
		(vlong)s->emapsecs*s->secsz, s->blksz) < 0)
		goto bad;

	/* free dirty records */
	memset(&de, 0, sizeof de);
	de.state = 0;
	de.vers = Storevers;
	per = s->blksz / Dirtentsz;
	for(j = 0; j < per; j++)
		dirtpack(buf + j*Dirtentsz, &de);
	off = (uvlong)s->dirtoff*s->secsz;
	end = off + (uvlong)s->dirtsecs*s->secsz;
	for(; off < end; off += s->blksz)
		if(devwrite(d, buf, s->blksz, off) < 0)
			goto bad;

	/*
	 * Bitmap pages, all at ckseq 0.  Bit 0 of page 0 is set so no
	 * allocator can ever hand out grain 0: an allocator that did
	 * would make every hole in every object alias dataoff (§2.1).
	 */
	npage = nbmpage(s);
	for(i = 0; i < npage; i++){
		memset(buf, 0, s->blksz);
		if(i == 0)
			bmset(buf, 0);
		memset(&bh, 0, sizeof bh);
		bh.vers = Storevers;
		bh.page = i;
		bh.ckseq = 0;
		bmpack(buf, s->blksz, &bh);
		off = (uvlong)s->bmapoff*s->secsz + i*(uvlong)s->blksz;
		if(devwrite(d, buf, s->blksz, off) < 0)
			goto bad;
	}
	if(devflush(d) < 0)
		goto bad;

	/* copy 0 at gen 0, copy 1 at gen 1: shoalck has a fresh-disk expectation */
	if((sb = mallocz(s->secsz, 1)) == nil)
		goto bad;
	s->gen = 0;
	superpack(sb, s);
	if(devwrite(d, sb, s->secsz, 0) < 0){
		free(sb);
		goto bad;
	}
	devpoint(d, "super", 0);
	if(devflush(d) < 0){
		free(sb);
		goto bad;
	}
	s->gen = 1;
	superpack(sb, s);
	if(devwrite(d, sb, s->secsz, super1off(d)) < 0){
		free(sb);
		goto bad;
	}
	devpoint(d, "super", 1);
	if(devflush(d) < 0){
		free(sb);
		goto bad;
	}
	free(sb);
	free(buf);
	return 0;
bad:
	free(buf);
	return -1;
}
