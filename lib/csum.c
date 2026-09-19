#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"

/*
 * A zero-length call into libsec still hands the input pointer to
 * memmove, so pass this rather than nil.
 */
static uchar nothing[1];

/* number of checksum blocks covering an object of len bytes */
uvlong
blkcount(uvlong len, ulong blksz)
{
	if(blksz == 0)
		sysfatal("blkcount: blksz 0");
	return (len + blksz - 1) / blksz;
}

void
blkdigest(uchar *p, ulong n, uchar dig[Blkdlen])
{
	if(p == nil)
		p = nothing;
	blake2s_128(p, n, dig, nil);
}

void
csuminit(Csum *c)
{
	c->s = nil;
	c->n = 0;
}

void
csumadd(Csum *c, uchar dig[Blkdlen])
{
	c->s = blake2s_256(dig, Blkdlen, nil, c->s);
	c->n++;
}

void
csumfinal(Csum *c, uchar csum[Csumlen])
{
	blake2s_256(nothing, 0, csum, c->s);
	c->s = nil;
	c->n = 0;
}

/*
 * csum over a stored concatenation of nblk block digests.  nblk 0
 * hashes the empty concatenation, which is the zero-length object's
 * fixed csum.
 */
void
csumdigests(uchar *digs, uvlong nblk, uchar csum[Csumlen])
{
	Csum c;
	uvlong i;

	csuminit(&c);
	for(i = 0; i < nblk; i++)
		csumadd(&c, digs + i*Blkdlen);
	csumfinal(&c, csum);
}

/*
 * Fill digs with the block digests of an object held contiguously in
 * memory; digs must have room for blkcount(len, blksz) digests.
 * Returns the number written.
 */
uvlong
objdigests(uchar *p, uvlong len, ulong blksz, uchar *digs)
{
	uvlong i, off, nblk;
	ulong n;

	nblk = blkcount(len, blksz);
	for(i = 0; i < nblk; i++){
		off = i * (uvlong)blksz;
		n = blksz;
		if(off + n > len)
			n = len - off;
		blkdigest(p + off, n, digs + i*Blkdlen);
	}
	return nblk;
}

/* csum of an object held contiguously in memory */
void
objcsum(uchar *p, uvlong len, ulong blksz, uchar csum[Csumlen])
{
	Csum c;
	uchar dig[Blkdlen];
	uvlong i, off, nblk;
	ulong n;

	nblk = blkcount(len, blksz);
	csuminit(&c);
	for(i = 0; i < nblk; i++){
		off = i * (uvlong)blksz;
		n = blksz;
		if(off + n > len)
			n = len - off;
		blkdigest(p + off, n, dig);
		csumadd(&c, dig);
	}
	csumfinal(&c, csum);
}

/* render as 64 lower-case hex characters, bare (§1.4) */
char*
csumfmt(char *buf, uchar csum[Csumlen])
{
	static char hex[] = "0123456789abcdef";
	int i;

	for(i = 0; i < Csumlen; i++){
		buf[2*i] = hex[csum[i] >> 4];
		buf[2*i + 1] = hex[csum[i] & 0xf];
	}
	buf[2*Csumlen] = '\0';
	return buf;
}
