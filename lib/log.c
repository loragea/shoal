#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"

/*
 * The log record and its entries, docs/design/store.md §2.7.
 *
 * A record is nsec contiguous sectors whose first sector is the
 * header, and it is valid iff magic and vers match, nsec is within
 * the log region from this offset, seq is the expected successor, and
 * csum128 verifies over the whole nsec*secsz bytes.  nsec is
 * bounds-checked before it is used to address anything: a torn header
 * can carry a garbage length, and hashing an unbounded range on the
 * strength of an unverified field is how a replay turns a crash into
 * a fault.
 */

static char logmagic[8] = { 's','h','o','a','l','l','o','g' };

enum
{
	Dirtyfixed	= 12,	/* Edirty bytes before peer[] */
};

/*
 * Pack the header and seal the record.  The entry bytes must already
 * be in place: the checksum covers the whole nsec*secsz range.
 */
void
lrecpack(uchar *p, Lrec *r, ulong secsz)
{
	memmove(p + 0, logmagic, 8);
	PBIT32(p + 8, r->vers);
	PBIT32(p + 12, r->nsec);
	/* 16..31 csum128 */
	PBIT64(p + 32, r->seq);
	PBIT64(p + 40, r->time);
	PBIT32(p + 48, r->nent);
	PBIT16(p + 52, r->flags);
	PBIT16(p + 54, 0);			/* pad */
	reccsumset(p, r->nsec*secsz, 16);
}

int
lrecunpack(Lrec *r, uchar *p)
{
	memset(r, 0, sizeof *r);
	if(memcmp(p, logmagic, 8) != 0){
		werrstr("bad magic");
		return -1;
	}
	r->vers = GBIT32(p + 8);
	if(r->vers != Storevers){
		werrstr("log record version %lud", r->vers);
		return -1;
	}
	r->nsec = GBIT32(p + 12);
	r->seq = GBIT64(p + 32);
	r->time = GBIT64(p + 40);
	r->nent = GBIT32(p + 48);
	r->flags = GBIT16(p + 52);
	return 0;
}

/*
 * The full validity test, in the order §2.7 and §5 step 7 require:
 * header, then the bounds check on nsec, then the checksum over the
 * range nsec names, then the sequence expectation.  off and logsecs
 * are sector counts relative to the start of the log region.
 */
int
lrecvalid(uchar *p, ulong secsz, Lrec *r, uvlong off, uvlong logsecs,
	uvlong seq)
{
	if(lrecunpack(r, p) < 0)
		return -1;
	if(r->nsec < 1 || off + r->nsec > logsecs){
		werrstr("nsec %lud at log sector %llud, log is %llud sectors",
			r->nsec, off, logsecs);
		return -1;
	}
	if(logsecs*(uvlong)secsz >= (1ULL<<32)){
		/*
		 * The record length is computed in a u32, and shoalfmt
		 * refuses a log region that does not fit one (§2.1); a
		 * superblock claiming otherwise is not one to hash on.
		 */
		werrstr("log region of %llud bytes exceeds a u32 length",
			logsecs*(uvlong)secsz);
		return -1;
	}
	if(!reccsumok(p, r->nsec*secsz, 16)){
		werrstr("checksum mismatch");
		return -1;
	}
	if(r->seq != seq){
		werrstr("seq %llud, expected %llud", r->seq, seq);
		return -1;
	}
	return 0;
}

void
lentpack(uchar *p, int kind, int flags, ulong len)
{
	p[0] = kind;
	p[1] = flags;
	PBIT16(p + 2, 0);
	PBIT32(p + 4, len);
}

int
lentunpack(Lent *e, uchar *p, long n)
{
	memset(e, 0, sizeof *e);
	if(n < Lenthdrsz){
		werrstr("entry header runs off the record");
		return -1;
	}
	e->kind = p[0];
	e->flags = p[1];
	e->len = GBIT32(p + 4);
	if(e->len < Lenthdrsz || e->len > (ulong)n){
		werrstr("entry length %lud, %ld bytes left", e->len, n);
		return -1;
	}
	e->body = p + Lenthdrsz;
	return 0;
}

ulong
objreclen(Objrec *o)
{
	return Lenthdrsz + Objfixed + o->oidlen + 4 + 24*o->nmap
		+ 4 + 4*o->nfree;
}

long
objrecpack(uchar *p, long max, Objrec *o)
{
	uchar *q;
	ulong n, i;

	n = objreclen(o);
	if(o->oidlen < 1 || o->oidlen > Oidmax){
		werrstr("Eobj: oidlen %d", o->oidlen);
		return -1;
	}
	if((long)n > max){
		werrstr("Eobj: %lud bytes into %ld", n, max);
		return -1;
	}
	lentpack(p, Kobj, 0, n);
	q = p + Lenthdrsz;
	PBIT32(q + 0, o->slot);
	PBIT32(q + 4, o->emapslot);
	PBIT64(q + 8, o->qidpath);
	q[16] = o->state;
	q[17] = o->oidlen;
	q[18] = o->oflags;
	q[19] = 0;
	PBIT64(q + 20, o->len);
	PBIT64(q + 28, o->ver);
	PBIT64(q + 36, o->wepoch);
	PBIT64(q + 44, o->mtime);
	memmove(q + 52, o->csum, Csumlen);
	memmove(q + Objfixed, o->oid, o->oidlen);
	q += Objfixed + o->oidlen;
	PBIT32(q, o->nmap);
	q += 4;
	for(i = 0; i < o->nmap; i++){
		PBIT32(q + 0, o->map[i].blk);
		PBIT32(q + 4, o->map[i].grain);
		memmove(q + 8, o->map[i].dig, Blkdlen);
		q += 24;
	}
	PBIT32(q, o->nfree);
	q += 4;
	for(i = 0; i < o->nfree; i++){
		PBIT32(q, o->freed[i]);
		q += 4;
	}
	return n;
}

/*
 * Decode an Eobj body.  The map and freed arrays are allocated here
 * and released by objrecfree; the commit path (§3.2) builds a record
 * by packing directly and never uses this.
 */
int
objrecunpack(Objrec *o, uchar *p, long n)
{
	uchar *q, *end;
	ulong i;

	memset(o, 0, sizeof *o);
	end = p + n;
	if(n < Objfixed + 1){
		werrstr("Eobj: body of %ld bytes", n);
		return -1;
	}
	q = p;
	o->slot = GBIT32(q + 0);
	o->emapslot = GBIT32(q + 4);
	o->qidpath = GBIT64(q + 8);
	o->state = q[16];
	o->oidlen = q[17];
	o->oflags = q[18];
	o->len = GBIT64(q + 20);
	o->ver = GBIT64(q + 28);
	o->wepoch = GBIT64(q + 36);
	o->mtime = GBIT64(q + 44);
	memmove(o->csum, q + 52, Csumlen);
	if(o->state > Stomb){
		werrstr("Eobj: state %d", o->state);
		return -1;
	}
	if(o->oidlen < 1 || o->oidlen > Oidmax){
		werrstr("Eobj: oidlen %d", o->oidlen);
		return -1;
	}
	if(o->oflags & ~Oslot){
		werrstr("Eobj: reserved oflags bit set (%#ux)", o->oflags);
		return -1;
	}
	q += Objfixed;
	if(q + o->oidlen + 4 > end){
		werrstr("Eobj: oid runs off the entry");
		return -1;
	}
	memmove(o->oid, q, o->oidlen);
	q += o->oidlen;
	o->nmap = GBIT32(q);
	q += 4;
	if(o->nmap > (ulong)(end - q)/24){
		werrstr("Eobj: nmap %lud runs off the entry", o->nmap);
		return -1;
	}
	if(o->nmap > 0 && (o->map = malloc(o->nmap*sizeof *o->map)) == nil)
		return -1;
	for(i = 0; i < o->nmap; i++){
		o->map[i].blk = GBIT32(q + 0);
		o->map[i].grain = GBIT32(q + 4);
		memmove(o->map[i].dig, q + 8, Blkdlen);
		q += 24;
	}
	if(q + 4 > end){
		werrstr("Eobj: nfree runs off the entry");
		goto bad;
	}
	o->nfree = GBIT32(q);
	q += 4;
	if(o->nfree > (ulong)(end - q)/4){
		werrstr("Eobj: nfree %lud runs off the entry", o->nfree);
		goto bad;
	}
	if(o->nfree > 0 && (o->freed = malloc(o->nfree*sizeof *o->freed)) == nil)
		goto bad;
	for(i = 0; i < o->nfree; i++){
		o->freed[i] = GBIT32(q);
		q += 4;
	}
	return 0;
bad:
	objrecfree(o);
	return -1;
}

void
objrecfree(Objrec *o)
{
	free(o->map);
	free(o->freed);
	o->map = nil;
	o->freed = nil;
	o->nmap = 0;
	o->nfree = 0;
}

ulong
dirtyreclen(Dirtyrec *d)
{
	return Lenthdrsz + Dirtyfixed + d->peerlen + d->oidlen;
}

long
dirtyrecpack(uchar *p, long max, Dirtyrec *d)
{
	uchar *q;
	ulong n;

	if(d->op > 1){
		werrstr("Edirty: op %d", d->op);
		return -1;
	}
	if(d->oidlen < 1 || d->oidlen > Oidmax || d->peerlen < 1
	|| d->peerlen > Peermax){
		werrstr("Edirty: oidlen %d peerlen %d", d->oidlen, d->peerlen);
		return -1;
	}
	n = dirtyreclen(d);
	if((long)n > max){
		werrstr("Edirty: %lud bytes into %ld", n, max);
		return -1;
	}
	lentpack(p, Kdirty, 0, n);
	q = p + Lenthdrsz;
	q[0] = d->op;
	q[1] = d->peerlen;
	q[2] = d->oidlen;
	q[3] = 0;
	PBIT64(q + 4, d->epoch);
	memmove(q + Dirtyfixed, d->peer, d->peerlen);
	memmove(q + Dirtyfixed + d->peerlen, d->oid, d->oidlen);
	return n;
}

int
dirtyrecunpack(Dirtyrec *d, uchar *p, long n)
{
	memset(d, 0, sizeof *d);
	if(n < Dirtyfixed){
		werrstr("Edirty: body of %ld bytes", n);
		return -1;
	}
	d->op = p[0];
	d->peerlen = p[1];
	d->oidlen = p[2];
	d->epoch = GBIT64(p + 4);
	if(d->op > 1){
		werrstr("Edirty: op %d", d->op);
		return -1;
	}
	if(d->oidlen < 1 || d->oidlen > Oidmax || d->peerlen < 1
	|| d->peerlen > Peermax){
		werrstr("Edirty: oidlen %d peerlen %d", d->oidlen, d->peerlen);
		return -1;
	}
	if(Dirtyfixed + d->peerlen + d->oidlen > n){
		werrstr("Edirty: names run off the entry");
		return -1;
	}
	memmove(d->peer, p + Dirtyfixed, d->peerlen);
	memmove(d->oid, p + Dirtyfixed + d->peerlen, d->oidlen);
	return 0;
}

long
slotrecpack(uchar *p, long max, ulong slot)
{
	if(max < Lenthdrsz + 4){
		werrstr("Eslot: %d bytes into %ld", Lenthdrsz + 4, max);
		return -1;
	}
	lentpack(p, Kslot, 0, Lenthdrsz + 4);
	PBIT32(p + Lenthdrsz, slot);
	return Lenthdrsz + 4;
}

int
slotrecunpack(ulong *slot, uchar *p, long n)
{
	if(n < 4){
		werrstr("Eslot: body of %ld bytes", n);
		return -1;
	}
	*slot = GBIT32(p);
	return 0;
}
