#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"

/*
 * The superblock, docs/design/store.md §2.2, and the record-checksum
 * rule of §0: a record's checksum is BLAKE2s-128 over the record's
 * whole byte range with the checksum field itself zeroed, and is
 * verified the same way.
 */

static char sbmagic[8] = { 's','h','o','a','l','s','b','\0' };

void
reccsumset(uchar *p, ulong n, ulong csumoff)
{
	memset(p + csumoff, 0, Recsumlen);
	blkdigest(p, n, p + csumoff);
}

int
reccsumok(uchar *p, ulong n, ulong csumoff)
{
	uchar saved[Recsumlen], want[Recsumlen];
	int ok;

	memmove(saved, p + csumoff, Recsumlen);
	memset(p + csumoff, 0, Recsumlen);
	blkdigest(p, n, want);
	memmove(p + csumoff, saved, Recsumlen);
	ok = memcmp(saved, want, Recsumlen) == 0;
	return ok;
}

void
superpack(uchar *p, Super *s)
{
	memset(p, 0, s->hdrlen);
	memmove(p + 0, sbmagic, 8);
	PBIT32(p + 8, s->vers);
	PBIT32(p + 12, s->hdrlen);
	/* 16..31 csum, filled below */
	PBIT64(p + 32, s->gen);
	memmove(p + 40, s->uuid, 16);
	PBIT64(p + 56, s->ctime);
	PBIT32(p + 64, s->secsz);
	PBIT32(p + 68, s->blksz);
	PBIT64(p + 72, s->objmax);
	PBIT32(p + 80, s->nblkmax);
	PBIT32(p + 84, s->emapsz);
	PBIT32(p + 88, s->nslots);
	PBIT32(p + 92, s->nemap);
	PBIT32(p + 96, s->ndirty);
	PBIT32(p + 100, 0);			/* pad */
	PBIT64(p + 104, s->ngrains);
	PBIT64(p + 112, s->logoff);
	PBIT64(p + 120, s->logsecs);
	PBIT64(p + 128, s->idxoff);
	PBIT64(p + 136, s->idxsecs);
	PBIT64(p + 144, s->emapoff);
	PBIT64(p + 152, s->emapsecs);
	PBIT64(p + 160, s->dirtoff);
	PBIT64(p + 168, s->dirtsecs);
	PBIT64(p + 176, s->bmapoff);
	PBIT64(p + 184, s->bmapsecs);
	PBIT64(p + 192, s->dataoff);
	PBIT64(p + 200, s->datasecs);
	PBIT64(p + 208, s->ckseq);
	PBIT64(p + 216, s->cklogoff);
	PBIT64(p + 224, s->qidnext);
	PBIT64(p + 232, s->epochhigh);
	memmove(p + 240, s->monid, 16);
	PBIT32(p + 256, s->monidset);
	PBIT32(p + 260, s->csumalg);
	reccsumset(p, s->hdrlen, 16);
}

/*
 * Decode and validate one superblock image.  Every refusal §5 step 2
 * and step 3 name is reported here by name, so that shoalck can say
 * which copy is invalid and why.
 */
int
superunpack(Super *s, uchar *p, ulong secsz)
{
	memset(s, 0, sizeof *s);
	if(memcmp(p, sbmagic, 8) != 0){
		werrstr("bad magic");
		return -1;
	}
	s->vers = GBIT32(p + 8);
	if(s->vers != Storevers){
		werrstr("format version %lud, this build implements %d",
			s->vers, Storevers);
		return -1;
	}
	s->hdrlen = GBIT32(p + 12);
	if(s->hdrlen != secsz){
		werrstr("hdrlen %lud, device sector %lud", s->hdrlen, secsz);
		return -1;
	}
	if(!reccsumok(p, s->hdrlen, 16)){
		werrstr("checksum mismatch");
		return -1;
	}
	s->gen = GBIT64(p + 32);
	memmove(s->uuid, p + 40, 16);
	s->ctime = GBIT64(p + 56);
	s->secsz = GBIT32(p + 64);
	s->blksz = GBIT32(p + 68);
	s->objmax = GBIT64(p + 72);
	s->nblkmax = GBIT32(p + 80);
	s->emapsz = GBIT32(p + 84);
	s->nslots = GBIT32(p + 88);
	s->nemap = GBIT32(p + 92);
	s->ndirty = GBIT32(p + 96);
	s->ngrains = GBIT64(p + 104);
	s->logoff = GBIT64(p + 112);
	s->logsecs = GBIT64(p + 120);
	s->idxoff = GBIT64(p + 128);
	s->idxsecs = GBIT64(p + 136);
	s->emapoff = GBIT64(p + 144);
	s->emapsecs = GBIT64(p + 152);
	s->dirtoff = GBIT64(p + 160);
	s->dirtsecs = GBIT64(p + 168);
	s->bmapoff = GBIT64(p + 176);
	s->bmapsecs = GBIT64(p + 184);
	s->dataoff = GBIT64(p + 192);
	s->datasecs = GBIT64(p + 200);
	s->ckseq = GBIT64(p + 208);
	s->cklogoff = GBIT64(p + 216);
	s->qidnext = GBIT64(p + 224);
	s->epochhigh = GBIT64(p + 232);
	memmove(s->monid, p + 240, 16);
	s->monidset = GBIT32(p + 256);
	s->csumalg = GBIT32(p + 260);
	if(s->secsz != secsz){
		werrstr("secsz %lud, device sector %lud", s->secsz, secsz);
		return -1;
	}
	return 0;
}

/* derived quantities, §2.1 */
uvlong
bmbits(ulong pagesz)
{
	return 8ULL * (pagesz - Bmhdrsz);
}

uvlong
nbmpage(Super *s)
{
	return s->bmapsecs / (s->blksz / s->secsz);
}

vlong
grainoff(Super *s, ulong grain)
{
	return (vlong)s->dataoff*s->secsz + (vlong)grain*s->blksz;
}

uvlong
idxentoff(Super *s, ulong slot)
{
	return s->idxoff*(uvlong)s->secsz + (uvlong)slot*Idxentsz;
}

uvlong
emapentoff(Super *s, ulong slot)
{
	return s->emapoff*(uvlong)s->secsz + (uvlong)slot*s->emapsz;
}

uvlong
dirtentoff(Super *s, ulong slot)
{
	return s->dirtoff*(uvlong)s->secsz + (uvlong)slot*Dirtentsz;
}

/* copy 1 sits at the last sector of the partition (§2.1) */
vlong
super1off(Dev *d)
{
	return d->size - d->secsz;
}

/*
 * Read both copies and apply §2.2's two-slot rule.  The rule is
 * stated in three clauses and the order matters: exactly one valid
 * copy means the update writes the invalid one, both valid means it
 * writes the one with the lower gen, and neither valid means the
 * store MUST NOT write and MUST NOT serve.  Clause 1 is the whole
 * point of keeping two copies — without it a copy torn at a high gen
 * steers the next write onto the only good one.
 */
int
superselect(Dev *d, Sbsel *sel)
{
	uchar *buf;
	vlong off;
	int i;

	memset(sel, 0, sizeof *sel);
	sel->start = -1;
	sel->victim = -1;
	if((buf = malloc(d->secsz)) == nil)
		return -1;
	for(i = 0; i < 2; i++){
		off = i == 0 ? 0 : super1off(d);
		if(devread(d, buf, d->secsz, off) < 0){
			snprint(sel->why[i], sizeof sel->why[i],
				"unreadable: %r");
			continue;
		}
		if(superunpack(&sel->sb[i], buf, d->secsz) < 0){
			snprint(sel->why[i], sizeof sel->why[i], "%r");
			continue;
		}
		sel->valid[i] = 1;
	}
	free(buf);

	if(sel->valid[0] && sel->valid[1]){
		sel->clause = 2;
		sel->start = sel->sb[0].gen >= sel->sb[1].gen ? 0 : 1;
		sel->victim = sel->sb[0].gen < sel->sb[1].gen ? 0 : 1;
		sel->nextgen = (sel->sb[sel->start].gen) + 1;
	}else if(sel->valid[0] || sel->valid[1]){
		sel->clause = 1;
		sel->start = sel->valid[0] ? 0 : 1;
		sel->victim = sel->valid[0] ? 1 : 0;
		sel->nextgen = sel->sb[sel->start].gen + 1;
	}else{
		sel->clause = 3;
		werrstr("no valid superblock");
		return -1;
	}
	return 0;
}
