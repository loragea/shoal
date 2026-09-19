#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"

/*
 * T1: object checksum, docs/design/layer-a.md §1.4.
 *
 * Every expected value below was computed independently of this
 * codebase, with Python's hashlib.blake2s(digest_size=16) per block
 * and hashlib.blake2s(digest_size=32) over the concatenated block
 * digests.  Content is the deterministic pattern fill() produces.
 */

typedef struct Vec Vec;
struct Vec
{
	char	*name;
	ulong	blksz;
	uvlong	len;
	char	*csum;
};

static Vec vec[] =
{
	{ "empty",	64,	0,
	  "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9" },
	{ "onebyte",	64,	1,
	  "801d1900064542dc3489a32ea7325fe9156759d5805a2537e265d933f5877bdc" },
	{ "oneblock",	64,	64,
	  "fe2d19042e7a49f9162880f3717289b99c06fca62e5d4a511d932148773b60f6" },
	{ "blockplus1",	64,	65,
	  "2d114776ebea2fce47617501bf552fb56cf3de3921e0ed00caeb577a39edbb45" },
	{ "threepartial", 64,	199,
	  "56de5a1f458c4c8c00a7a45c26b099bf03ad88acb107e9e07413d3718b27bfe4" },
	{ "blksz64k",	65536,	2*65536 + 1234,
	  "2b03f34254f29e65d28dcd03b79511d15d42dd440cec48d69c568d5f1b952aba" },
};

/* the threepartial object with one byte flipped, per block flipped in */
typedef struct Rvec Rvec;
struct Rvec
{
	uvlong	off;
	char	*csum;
};

static Rvec rvec[] =
{
	{ 70,	"a8849a68efcb0bb348b1488057c9301252c8bb80f574f58f21a941d6f9e12012" },
	{ 195,	"a2bd29bca16d3883d7d087048fc0e717f39b2fb7d212d963543ed9d706b3ecfc" },
};

static int fails;
static int checks;

static void
fail(char *fmt, ...)
{
	char buf[512];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf + sizeof buf, fmt, arg);
	va_end(arg);
	fprint(2, "FAIL: %s\n", buf);
	fails++;
}

/* deterministic content: byte i is (i*37 + 11) mod 256 */
static void
fill(uchar *p, uvlong len)
{
	uvlong i;

	for(i = 0; i < len; i++)
		p[i] = (uchar)(i*37 + 11);
}

static void*
emalloc(uvlong n)
{
	void *p;

	if((p = malloc(n + 1)) == nil)
		sysfatal("malloc %llud: %r", n);
	return p;
}

/* length of block b of an object of len bytes */
static ulong
blklen(uvlong b, uvlong len, ulong blksz)
{
	uvlong off;

	off = b * (uvlong)blksz;
	if(off + blksz > len)
		return len - off;
	return blksz;
}

/* known answers, both through objcsum and through the digest array */
static void
tvectors(void)
{
	uchar *buf, *digs, csum[Csumlen];
	char hex[Csumhexlen];
	uvlong nblk;
	int i;

	for(i = 0; i < nelem(vec); i++){
		buf = emalloc(vec[i].len);
		fill(buf, vec[i].len);

		objcsum(buf, vec[i].len, vec[i].blksz, csum);
		if(strcmp(csumfmt(hex, csum), vec[i].csum) != 0)
			fail("%s: objcsum %s, want %s",
				vec[i].name, hex, vec[i].csum);
		checks++;

		nblk = blkcount(vec[i].len, vec[i].blksz);
		digs = emalloc(nblk * Blkdlen);
		if(objdigests(buf, vec[i].len, vec[i].blksz, digs) != nblk)
			fail("%s: objdigests count", vec[i].name);
		csumdigests(digs, nblk, csum);
		if(strcmp(csumfmt(hex, csum), vec[i].csum) != 0)
			fail("%s: csumdigests %s, want %s",
				vec[i].name, hex, vec[i].csum);
		checks += 2;

		free(digs);
		free(buf);
	}
}

/* csum rendering is 64 lower-case hex characters, bare */
static void
tfmt(void)
{
	uchar csum[Csumlen];
	char hex[Csumhexlen];
	int i;

	for(i = 0; i < Csumlen; i++)
		csum[i] = i * 8;
	csumfmt(hex, csum);
	if(strlen(hex) != 2*Csumlen)
		fail("csumfmt: length %d, want %d", (int)strlen(hex), 2*Csumlen);
	else if(strcmp(hex, "00081018202830384048505860687078"
			"80889098a0a8b0b8c0c8d0d8e0e8f0f8") != 0)
		fail("csumfmt: %s", hex);
	checks++;
}

/*
 * Re-hashing one block and replaying the digest array must give the
 * same csum as a fresh whole-object computation, and must give the
 * independently computed answer for the mutated object.
 */
static void
trehash(void)
{
	enum { Blksz = 64, Len = 199, Nblk = 4 };
	uchar buf[Len], digs[Nblk*Blkdlen], csum[Csumlen], want[Csumlen];
	char hex[Csumhexlen], wanthex[Csumhexlen];
	uvlong b;
	int i;

	for(i = 0; i < nelem(rvec); i++){
		fill(buf, Len);
		if(objdigests(buf, Len, Blksz, digs) != Nblk){
			fail("rehash: objdigests count");
			return;
		}

		buf[rvec[i].off] ^= 0x5a;
		b = rvec[i].off / Blksz;
		blkdigest(buf + b*Blksz, blklen(b, Len, Blksz),
			digs + b*Blkdlen);
		csumdigests(digs, Nblk, csum);

		objcsum(buf, Len, Blksz, want);
		if(memcmp(csum, want, Csumlen) != 0)
			fail("rehash off %llud: %s, fresh gives %s",
				rvec[i].off, csumfmt(hex, csum),
				csumfmt(wanthex, want));
		if(strcmp(csumfmt(hex, csum), rvec[i].csum) != 0)
			fail("rehash off %llud: %s, want %s",
				rvec[i].off, hex, rvec[i].csum);
		checks += 2;
	}
}

void
main(int, char**)
{
	tvectors();
	tfmt();
	trehash();
	if(fails > 0)
		exits("failed");
	print("csumtest: %d checks ok\n", checks);
	exits(nil);
}
