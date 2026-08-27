#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"

/*
 * The index entry (§2.3), the extent-map entry (§2.4), a free-grain
 * bitmap page (§2.5) and a dirty record (§2.6), byte for byte as
 * docs/design/store.md §2 states them.
 *
 * Every unpack validates before it believes: the structure's own
 * checksum first, then its vers, then the range checks §5 steps 4 and
 * 6 require — because a damaged entry's own fields are the first
 * thing an in-memory index build would trust.
 */

static char bmmagic[8] = { 's','h','o','a','l','b','m','\0' };

char*
csumalgname(ulong alg)
{
	if(alg == Csumblake2s)
		return "blake2s256";
	return "unknown";
}

ulong
csumalgno(char *name)
{
	if(strcmp(name, "blake2s256") == 0)
		return Csumblake2s;
	return 0;
}

void
idxpack(uchar *p, Idxent *e)
{
	memset(p, 0, Idxentsz);
	p[0] = e->state;
	p[1] = e->oidlen;
	p[2] = e->flags;
	p[3] = e->vers;
	PBIT32(p + 4, e->emapslot);
	PBIT64(p + 8, e->qidpath);
	PBIT64(p + 16, e->len);
	PBIT64(p + 24, e->ver);
	PBIT64(p + 32, e->wepoch);
	PBIT64(p + 40, e->mtime);
	memmove(p + 48, e->csum, Csumlen);
	memmove(p + 80, e->oid, Oidmax);
	/* 208..223 csum128 */
	PBIT32(p + 224, e->grain0);
	memmove(p + 228, e->dig0, Blkdlen);
	reccsumset(p, Idxentsz, 208);
}

int
idxunpack(Idxent *e, uchar *p, ulong nemap)
{
	memset(e, 0, sizeof *e);
	if(!reccsumok(p, Idxentsz, 208)){
		werrstr("checksum mismatch");
		return -1;
	}
	e->state = p[0];
	e->oidlen = p[1];
	e->flags = p[2];
	e->vers = p[3];
	e->emapslot = GBIT32(p + 4);
	e->qidpath = GBIT64(p + 8);
	e->len = GBIT64(p + 16);
	e->ver = GBIT64(p + 24);
	e->wepoch = GBIT64(p + 32);
	e->mtime = GBIT64(p + 40);
	memmove(e->csum, p + 48, Csumlen);
	memmove(e->oid, p + 80, Oidmax);
	e->grain0 = GBIT32(p + 224);
	memmove(e->dig0, p + 228, Blkdlen);
	if(e->vers != Storevers){
		werrstr("entry version %d", e->vers);
		return -1;
	}
	if(e->state > Stomb){
		werrstr("state %d", e->state);
		return -1;
	}
	/*
	 * §0: a flags field's undefined bits are a MUST-be-zero the
	 * reader checks, because an unknown flag means the entry
	 * asserts something this build does not know how to honour —
	 * here, some other reason to keep an object out of a read.
	 */
	if(e->flags & ~Icorrupt){
		werrstr("reserved flags bit set (%#ux)", e->flags);
		return -1;
	}
	if(e->state == Sfree){
		if(e->oidlen != 0 || e->emapslot != 0){
			werrstr("free slot with oidlen %d emapslot %lud",
				e->oidlen, e->emapslot);
			return -1;
		}
		return 0;
	}
	if(e->oidlen < 1 || e->oidlen > Oidmax){
		werrstr("oidlen %d", e->oidlen);
		return -1;
	}
	if(e->emapslot >= nemap){
		werrstr("emapslot %lud, nemap %lud", e->emapslot, nemap);
		return -1;
	}
	return 0;
}

/*
 * The extent-map entry.  nblk here is what the header sector says;
 * §2.4 requires a reader that also has len to recompute it with
 * blkcount and to trust that, because the header sector may be the
 * torn one.
 */
void
emappack(uchar *p, ulong emapsz, Emap *m)
{
	PBIT32(p + 16, m->nblk);
	PBIT32(p + 20, m->vers);
	reccsumset(p, emapsz, 0);
}

int
emapunpack(Emap *m, uchar *p, ulong emapsz, ulong nblkmax)
{
	memset(m, 0, sizeof *m);
	if(!reccsumok(p, emapsz, 0)){
		werrstr("checksum mismatch");
		return -1;
	}
	m->nblk = GBIT32(p + 16);
	m->vers = GBIT32(p + 20);
	if(m->vers != Storevers){
		werrstr("extent-map version %lud", m->vers);
		return -1;
	}
	if(m->nblk > nblkmax){
		werrstr("nblk %lud, nblkmax %lud", m->nblk, nblkmax);
		return -1;
	}
	return 0;
}

ulong
emapgrain(uchar *p, ulong i)
{
	return GBIT32(p + Emaphdrsz + 4*i);
}

void
emapsetgrain(uchar *p, ulong i, ulong grain)
{
	PBIT32(p + Emaphdrsz + 4*i, grain);
}

uchar*
emapdig(uchar *p, ulong nblkmax, ulong i)
{
	return p + Emaphdrsz + 4*nblkmax + Blkdlen*i;
}

/*
 * A bitmap page.  Its checksum decides how much has to be rewritten
 * at a checkpoint rather than how much a fault destroys, which is why
 * it is per page and not per copy (§2.5).
 */
void
bmpack(uchar *p, ulong pagesz, Bmpage *h)
{
	memmove(p + 0, bmmagic, 8);
	PBIT32(p + 8, h->vers);
	PBIT32(p + 12, h->page);
	/* 16..31 csum128 */
	PBIT64(p + 32, h->ckseq);
	PBIT64(p + 40, 0);			/* pad */
	reccsumset(p, pagesz, 16);
}

int
bmunpack(Bmpage *h, uchar *p, ulong pagesz, ulong page)
{
	memset(h, 0, sizeof *h);
	if(memcmp(p, bmmagic, 8) != 0){
		werrstr("bad magic");
		return -1;
	}
	h->vers = GBIT32(p + 8);
	if(h->vers != Storevers){
		werrstr("bitmap page version %lud", h->vers);
		return -1;
	}
	h->page = GBIT32(p + 12);
	if(h->page != page){
		werrstr("page number %lud, want %lud", h->page, page);
		return -1;
	}
	if(!reccsumok(p, pagesz, 16)){
		werrstr("checksum mismatch");
		return -1;
	}
	h->ckseq = GBIT64(p + 32);
	return 0;
}

int
bmget(uchar *p, uvlong bit)
{
	return (p[Bmhdrsz + bit/8] >> (bit%8)) & 1;
}

void
bmset(uchar *p, uvlong bit)
{
	p[Bmhdrsz + bit/8] |= 1 << (bit%8);
}

void
bmclr(uchar *p, uvlong bit)
{
	p[Bmhdrsz + bit/8] &= ~(1 << (bit%8));
}

/*
 * A dirty record.  peerlen is a u8 and can name more than the field
 * holds, so §2.6 requires peerlen > 72 and oidlen > 128 to be
 * rejected on read as well as on write.
 */
void
dirtpack(uchar *p, Dirtent *e)
{
	memset(p, 0, Dirtentsz);
	/* 0..15 csum128 */
	PBIT64(p + 16, e->epoch);
	p[24] = e->state;
	p[25] = e->oidlen;
	p[26] = e->peerlen;
	p[27] = e->vers;
	memmove(p + 28, e->oid, Oidmax);
	memmove(p + 156, e->peer, Peermax);
	reccsumset(p, Dirtentsz, 0);
}

int
dirtunpack(Dirtent *e, uchar *p)
{
	memset(e, 0, sizeof *e);
	if(!reccsumok(p, Dirtentsz, 0)){
		werrstr("checksum mismatch");
		return -1;
	}
	e->epoch = GBIT64(p + 16);
	e->state = p[24];
	e->oidlen = p[25];
	e->peerlen = p[26];
	e->vers = p[27];
	memmove(e->oid, p + 28, Oidmax);
	memmove(e->peer, p + 156, Peermax);
	if(e->vers != Storevers){
		werrstr("dirty record version %d", e->vers);
		return -1;
	}
	if(e->state > 1){
		werrstr("state %d", e->state);
		return -1;
	}
	if(e->state == 0){
		if(e->oidlen != 0 || e->peerlen != 0){
			werrstr("free record with oidlen %d peerlen %d",
				e->oidlen, e->peerlen);
			return -1;
		}
		return 0;
	}
	if(e->oidlen < 1 || e->oidlen > Oidmax){
		werrstr("oidlen %d", e->oidlen);
		return -1;
	}
	if(e->peerlen < 1 || e->peerlen > Peermax){
		werrstr("peerlen %d", e->peerlen);
		return -1;
	}
	return 0;
}
