#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"

/*
 * T1: the on-disk structures of docs/design/store.md §2, byte for
 * byte, plus the checksum rule of §0.
 *
 * Every vector below was computed outside this codebase, in Python:
 * struct.pack('<I'/'<Q'/'<H') for each integer at the offset §2's
 * table gives it, and hashlib.blake2s(digest_size=16) over the
 * record's whole byte range with the checksum field itself zeroed.
 * Byte patterns are pat(n, k)[i] = (i*k + 11) mod 256, the same
 * deterministic fill csumtest.c uses.
 *
 * The three things each vector discriminates: that a field is at the
 * offset the doc gives it, that it is packed little-endian, and that
 * the checksum covers what the doc says it covers.
 */

static char sbvec[] =
	"73686f616c7362000100000000020000e31344e0d8d083eed7dcf5a4768ff942"
	"0700000000000000101112131415161718191a1b1c1d1e1f00f1536500000000"
	"0002000000400000000000010000000000040000005200000000100000000400"
	"0000010000000000005bf90f0000000020000000000000000000020000000000"
	"2000020000000000000008000000000020000a00000000000002a40000000000"
	"2002ae000000000000800000000000002082ae0000000000c000010000000000"
	"e082af000000000000602bff01000000a1bb0d00000000002100000000000000"
	"00100000000000002c00000000000000a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
	"0100000001000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000";

static char idxvec[] =
	"0114010144332211080706050403020187d61200000000000900000000000000"
	"2c0000000000000001f15365000000000b121920272e353c434a51585f666d74"
	"7b828990979ea5acb3bac1c8cfd6dde40b0e1114171a1d202326292c2f323538"
	"3b3e414400000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"000000000000000000000000000000001ead873a3e844be2c9c335d95dd41f19"
	"efcdab000b10151a1f24292e33383d42474c5156000000000000000000000000";

static char emapvec[] =
	"04ab745c1b17d31db0a99632c9b754a503000000010000000500000000000000"
	"0900000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000b16212c37424d58"
	"636e79848f9aa5b00b17232f3b47535f6b77838f9ba7b3bf0b1825323f4c5966"
	"73808d9aa7b4c1ce000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000";

static char bmvec[] =
	"73686f616c626d00010000000300000063022bcae9a73443570f8a5a26bcba4b"
	"3930000000000000000000000000000021000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000080";

static char dirtvec[] =
	"ed5d963ccba1a8f349d577a313c2deac2c0000000000000001140c010b0e1114"
	"171a1d202326292c2f3235383b3e414400000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"000000000000000000000000000000000000000000000000000000006e6f6465"
	"372e303030303132000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000";

static char logvec[] =
	"73686f616c6c6f670100000001000000db020a39434c248d02c14deada8e7eaf"
	"2a0000000000000002f153650000000003000000010000000100000094000000"
	"64000000070000002b0200000000000001140100204e00000000000009000000"
	"000000002c0000000000000003f15365000000000b121920272e353c434a5158"
	"5f666d747b828990979ea5acb3bac1c8cfd6dde40b0e1114171a1d202326292c"
	"2f3235383b3e414401000000000000000c0000000b10151a1f24292e33383d42"
	"474c515601000000210000000200000034000000010c14002c00000000000000"
	"6e6f6465372e3030303031320b0e1114171a1d202326292c2f3235383b3e4144"
	"030000000c000000640000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000"
	"0000000000000000000000000000000000000000000000000000000000000000";

enum
{
	Nverify	= 20000,	/* verifications per proc in tshared */
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

static uchar*
unhex(char *s, long *np)
{
	uchar *p;
	long n, i;
	int c, v, hi;

	n = strlen(s);
	if(n % 2 != 0)
		sysfatal("odd hex vector");
	n /= 2;
	if((p = malloc(n)) == nil)
		sysfatal("malloc: %r");
	for(i = 0; i < 2*n; i++){
		c = s[i];
		if(c >= '0' && c <= '9')
			v = c - '0';
		else if(c >= 'a' && c <= 'f')
			v = c - 'a' + 10;
		else
			sysfatal("bad hex vector character %c", c);
		hi = (i & 1) == 0;
		if(hi)
			p[i/2] = v << 4;
		else
			p[i/2] |= v;
	}
	*np = n;
	return p;
}

/* deterministic content: byte i of pattern k is (i*k + 11) mod 256 */
static void
pat(uchar *p, long n, int k)
{
	long i;

	for(i = 0; i < n; i++)
		p[i] = (uchar)(i*k + 11);
}

static void
cmpbytes(char *what, uchar *got, uchar *want, long n)
{
	long i;

	checks++;
	for(i = 0; i < n; i++)
		if(got[i] != want[i]){
			fail("%s: byte %ld is %#.2ux, want %#.2ux", what, i,
				got[i], want[i]);
			return;
		}
}

static void
eqv(char *what, uvlong got, uvlong want)
{
	checks++;
	if(got != want)
		fail("%s: %llud, want %llud", what, got, want);
}

/*
 * Flip one byte in a structure and assert its decoder rejects it.
 * The byte is chosen inside the checksummed range and outside the
 * checksum field, so nothing but the checksum can catch it.
 */
static void
flip(char *what, uchar *img, long n, long off, int (*dec)(uchar*, long))
{
	uchar *p;

	if((p = malloc(n)) == nil)
		sysfatal("malloc: %r");
	memmove(p, img, n);
	checks++;
	if((*dec)(p, n) != 0)
		fail("%s: the intact image was rejected: %r", what);
	p[off] ^= 0x80;
	checks++;
	if((*dec)(p, n) == 0)
		fail("%s: a byte flipped at %ld was accepted", what, off);
	free(p);
}

static int
decsuper(uchar *p, long n)
{
	Super s;

	return superunpack(&s, p, n);
}

static int
decidx(uchar *p, long)
{
	Idxent e;

	return idxunpack(&e, p, 0x11223345);
}

static int
decemap(uchar *p, long n)
{
	Emap m;

	return emapunpack(&m, p, n, 16);
}

static int
decbm(uchar *p, long n)
{
	Bmpage h;

	return bmunpack(&h, p, n, 3);
}

static int
decdirt(uchar *p, long)
{
	Dirtent e;

	return dirtunpack(&e, p);
}

static int
declog(uchar *p, long n)
{
	Lrec r;

	return lrecvalid(p, n, &r, 0, 8, 42);
}

static uchar uuidvec[16] =
{
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

static uchar monidvec[16] =
{
	0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
	0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
};

/* §2.2: the superblock */
static void
tsuper(void)
{
	Super s, t;
	uchar *want, *got;
	long n;

	want = unhex(sbvec, &n);
	if(n != 512)
		sysfatal("sbvec is %ld bytes", n);
	if((got = malloc(n)) == nil)
		sysfatal("malloc: %r");

	memset(&s, 0, sizeof s);
	s.vers = 1;
	s.hdrlen = 512;
	s.gen = 7;
	memmove(s.uuid, uuidvec, 16);
	s.ctime = 1700000000;
	s.secsz = 512;
	s.blksz = 16384;
	s.objmax = 16777216;
	s.nblkmax = 1024;
	s.emapsz = 20992;
	s.nslots = 1048576;
	s.nemap = 262144;
	s.ndirty = 65536;
	s.ngrains = 268000000;
	s.logoff = 32;
	s.logsecs = 131072;
	s.idxoff = 131104;
	s.idxsecs = 524288;
	s.emapoff = 655392;
	s.emapsecs = 10748416;
	s.dirtoff = 11403808;
	s.dirtsecs = 32768;
	s.bmapoff = 11436576;
	s.bmapsecs = 65728;
	s.dataoff = 11502304;
	s.datasecs = 8576000000ULL;
	s.ckseq = 900001;
	s.cklogoff = 33;
	s.qidnext = 4096;
	s.epochhigh = 44;
	memmove(s.monid, monidvec, 16);
	s.monidset = 1;
	s.csumalg = Csumblake2s;

	superpack(got, &s);
	cmpbytes("superblock image", got, want, n);

	if(superunpack(&t, want, 512) < 0)
		fail("superblock: the known-answer image was rejected: %r");
	else{
		eqv("superblock gen", t.gen, 7);
		eqv("superblock ngrains", t.ngrains, 268000000);
		eqv("superblock datasecs", t.datasecs, 8576000000ULL);
		eqv("superblock ckseq", t.ckseq, 900001);
		eqv("superblock qidnext", t.qidnext, 4096);
		eqv("superblock csumalg", t.csumalg, Csumblake2s);
		eqv("superblock monidset", t.monidset, 1);
		checks++;
		if(memcmp(t.uuid, want + 40, 16) != 0)
			fail("superblock uuid");
		checks++;
		if(memcmp(t.monid, want + 240, 16) != 0)
			fail("superblock monid");
	}
	/* gen, in the middle of the checksummed range */
	flip("superblock", want, n, 32, decsuper);
	free(got);
	free(want);
}

/* §2.3: the index entry, including its inline one-block map */
static void
tidx(void)
{
	Idxent e, f;
	uchar *want, *got;
	long n;

	want = unhex(idxvec, &n);
	if(n != Idxentsz)
		sysfatal("idxvec is %ld bytes", n);
	if((got = malloc(n)) == nil)
		sysfatal("malloc: %r");

	memset(&e, 0, sizeof e);
	e.state = Slive;
	e.oidlen = 20;
	e.flags = Icorrupt;
	e.vers = 1;
	e.emapslot = 0x11223344;
	e.qidpath = 0x0102030405060708ULL;
	e.len = 1234567;
	e.ver = 9;
	e.wepoch = 44;
	e.mtime = 1700000001;
	pat(e.csum, Csumlen, 7);
	pat(e.oid, 20, 3);
	e.grain0 = 0x00abcdef;
	pat(e.dig0, Blkdlen, 5);

	idxpack(got, &e);
	cmpbytes("index entry image", got, want, n);

	if(idxunpack(&f, want, 0x11223345) < 0)
		fail("index entry: the known-answer image was rejected: %r");
	else{
		eqv("index state", f.state, Slive);
		eqv("index oidlen", f.oidlen, 20);
		eqv("index flags", f.flags, Icorrupt);
		eqv("index emapslot", f.emapslot, 0x11223344);
		eqv("index qidpath", f.qidpath, 0x0102030405060708ULL);
		eqv("index len", f.len, 1234567);
		eqv("index grain0", f.grain0, 0x00abcdef);
		checks++;
		if(memcmp(f.dig0, e.dig0, Blkdlen) != 0)
			fail("index dig0");
	}
	/* §2.3: emapslot is range-checked against nemap before it is believed */
	checks++;
	if(idxunpack(&f, want, 0x11223344) == 0)
		fail("index entry: emapslot == nemap was accepted");
	flip("index entry", want, n, 16, decidx);
	free(got);
	free(want);
}

/* §2.4: the extent-map entry, two parallel arrays */
static void
temap(void)
{
	Emap m, t;
	uchar *want, *got;
	long n;
	int i;

	want = unhex(emapvec, &n);
	if(n != 512)
		sysfatal("emapvec is %ld bytes", n);
	if((got = mallocz(n, 1)) == nil)
		sysfatal("malloc: %r");

	emapsetgrain(got, 0, 5);
	emapsetgrain(got, 1, 0);
	emapsetgrain(got, 2, 9);
	for(i = 0; i < 3; i++)
		pat(emapdig(got, 16, i), Blkdlen, 11 + i);
	m.nblk = 3;
	m.vers = 1;
	emappack(got, n, &m);
	cmpbytes("extent-map image", got, want, n);

	if(emapunpack(&t, want, n, 16) < 0)
		fail("extent map: the known-answer image was rejected: %r");
	else{
		eqv("extent map nblk", t.nblk, 3);
		eqv("extent map grain 0", emapgrain(want, 0), 5);
		eqv("extent map grain 1 (a hole)", emapgrain(want, 1), 0);
		eqv("extent map grain 2", emapgrain(want, 2), 9);
		checks++;
		if(memcmp(emapdig(want, 16, 2), emapdig(got, 16, 2),
			Blkdlen) != 0)
			fail("extent map dig 2");
	}
	flip("extent map", want, n, 24, decemap);
	free(got);
	free(want);
}

/* §2.5: a free-grain bitmap page */
static void
tbm(void)
{
	Bmpage h, t;
	uchar *want, *got;
	long n;

	want = unhex(bmvec, &n);
	if(n != 512)
		sysfatal("bmvec is %ld bytes", n);
	if((got = mallocz(n, 1)) == nil)
		sysfatal("malloc: %r");

	bmset(got, 0);
	bmset(got, 5);
	bmset(got, 3711);
	h.vers = 1;
	h.page = 3;
	h.ckseq = 12345;
	bmpack(got, n, &h);
	cmpbytes("bitmap page image", got, want, n);

	if(bmunpack(&t, want, n, 3) < 0)
		fail("bitmap page: the known-answer image was rejected: %r");
	else{
		eqv("bitmap page number", t.page, 3);
		eqv("bitmap ckseq", t.ckseq, 12345);
		eqv("bitmap bit 0", bmget(want, 0), 1);
		eqv("bitmap bit 5", bmget(want, 5), 1);
		eqv("bitmap bit 6", bmget(want, 6), 0);
		eqv("bitmap bit 3711", bmget(want, 3711), 1);
		eqv("bitmap bits per 512-byte page", bmbits(512), 3712);
	}
	/* a page whose number is not the one it sits at is not this page */
	checks++;
	if(bmunpack(&t, want, n, 4) == 0)
		fail("bitmap page: a wrong page number was accepted");
	flip("bitmap page", want, n, 48, decbm);
	free(got);
	free(want);
}

/* §2.6: a dirty record */
static void
tdirt(void)
{
	Dirtent e, f;
	uchar *want, *got;
	long n;

	want = unhex(dirtvec, &n);
	if(n != Dirtentsz)
		sysfatal("dirtvec is %ld bytes", n);
	if((got = malloc(n)) == nil)
		sysfatal("malloc: %r");

	memset(&e, 0, sizeof e);
	e.epoch = 44;
	e.state = 1;
	e.oidlen = 20;
	e.peerlen = 12;
	e.vers = 1;
	pat(e.oid, 20, 3);
	memmove(e.peer, "node7.000012", 12);
	dirtpack(got, &e);
	cmpbytes("dirty record image", got, want, n);

	if(dirtunpack(&f, want) < 0)
		fail("dirty record: the known-answer image was rejected: %r");
	else{
		eqv("dirty epoch", f.epoch, 44);
		eqv("dirty state", f.state, 1);
		eqv("dirty peerlen", f.peerlen, 12);
		checks++;
		if(memcmp(f.peer, "node7.000012", 12) != 0)
			fail("dirty peer");
	}
	/*
	 * §2.6: peerlen is a u8 and can name more than the field
	 * holds, so peerlen > 72 and oidlen > 128 are rejected on read.
	 */
	memmove(got, want, n);
	got[26] = 73;
	reccsumset(got, Dirtentsz, 0);
	checks++;
	if(dirtunpack(&f, got) == 0)
		fail("dirty record: peerlen 73 was accepted");
	memmove(got, want, n);
	got[25] = 129;
	reccsumset(got, Dirtentsz, 0);
	checks++;
	if(dirtunpack(&f, got) == 0)
		fail("dirty record: oidlen 129 was accepted");
	flip("dirty record", want, n, 16, decdirt);
	free(got);
	free(want);
}

/* §2.7: the log record header and its three entry kinds */
static void
tlog(void)
{
	Lrec r, t;
	Lent e;
	Objrec o, u;
	Dirtyrec dr, du;
	Mapent map;
	uchar *want, *got;
	ulong freed, slot;
	long n, m;

	want = unhex(logvec, &n);
	if(n != 512)
		sysfatal("logvec is %ld bytes", n);
	if((got = mallocz(n, 1)) == nil)
		sysfatal("malloc: %r");

	memset(&o, 0, sizeof o);
	o.slot = 100;
	o.emapslot = 7;
	o.qidpath = 555;
	o.state = Slive;
	o.oidlen = 20;
	o.oflags = Oslot;
	o.len = 20000;
	o.ver = 9;
	o.wepoch = 44;
	o.mtime = 1700000003;
	pat(o.csum, Csumlen, 7);
	pat(o.oid, 20, 3);
	map.blk = 0;
	map.grain = 12;
	pat(map.dig, Blkdlen, 5);
	o.nmap = 1;
	o.map = &map;
	freed = 33;
	o.nfree = 1;
	o.freed = &freed;

	/*
	 * §2.7: an Eobj for a one-block write with a 20-byte oid is
	 * 148 bytes, which is what makes a lone small commit one
	 * 512-byte write.
	 */
	eqv("Eobj entry length", objreclen(&o), 148);
	m = objrecpack(got + Lrechdrsz, n - Lrechdrsz, &o);
	if(m != 148)
		fail("Eobj: packed %ld bytes: %r", m);

	memset(&dr, 0, sizeof dr);
	dr.op = 1;
	dr.peerlen = 12;
	dr.oidlen = 20;
	dr.epoch = 44;
	memmove(dr.peer, "node7.000012", 12);
	pat(dr.oid, 20, 3);
	m = dirtyrecpack(got + Lrechdrsz + 148, n - Lrechdrsz - 148, &dr);
	if(m != 52)
		fail("Edirty: packed %ld bytes, want 52: %r", m);

	m = slotrecpack(got + Lrechdrsz + 200, n - Lrechdrsz - 200, 100);
	if(m != 12)
		fail("Eslot: packed %ld bytes, want 12: %r", m);

	r.vers = 1;
	r.nsec = 1;
	r.seq = 42;
	r.time = 1700000002;
	r.nent = 3;
	r.flags = Fwrap;
	lrecpack(got, &r, 512);
	cmpbytes("log record image", got, want, n);

	/* the record is valid only at its expected sequence number */
	checks++;
	if(lrecvalid(want, 512, &t, 0, 8, 42) < 0)
		fail("log record: the known-answer image was rejected: %r");
	checks++;
	if(lrecvalid(want, 512, &t, 0, 8, 43) == 0)
		fail("log record: an out-of-sequence record was accepted");

	/*
	 * §2.7: nsec*secsz is a u32, and shoalfmt refuses a log region
	 * that does not fit one (§2.1).  A superblock claiming a bigger
	 * one is not something to hash a range on: a corrupt nsec could
	 * wrap it.
	 */
	checks++;
	if(lrecvalid(want, 512, &t, 0, (1ULL<<32)/512, 42) == 0)
		fail("log record: a log region too big for a u32 length "
			"was accepted");
	eqv("log record nent", t.nent, 3);
	eqv("log record Fwrap", t.flags & Fwrap, Fwrap);

	/* nsec is bounds-checked against the region before it is used */
	memmove(got, want, n);
	PBIT32(got + 12, 1000000);
	checks++;
	if(lrecvalid(got, 512, &t, 0, 8, 42) == 0)
		fail("log record: nsec past the region end was accepted");

	/* walk the entry stream */
	checks++;
	if(lentunpack(&e, want + Lrechdrsz, n - Lrechdrsz) < 0)
		fail("Eobj: %r");
	else{
		eqv("entry 0 kind", e.kind, Kobj);
		eqv("entry 0 len", e.len, 148);
		if(objrecunpack(&u, e.body, e.len - Lenthdrsz) < 0)
			fail("Eobj: %r");
		else{
			eqv("Eobj slot", u.slot, 100);
			eqv("Eobj emapslot", u.emapslot, 7);
			eqv("Eobj oflags Oslot", u.oflags & Oslot, Oslot);
			eqv("Eobj len", u.len, 20000);
			eqv("Eobj nmap", u.nmap, 1);
			eqv("Eobj map grain", u.map[0].grain, 12);
			eqv("Eobj nfree", u.nfree, 1);
			eqv("Eobj freed grain", u.freed[0], 33);
			objrecfree(&u);
		}
	}
	checks++;
	if(lentunpack(&e, want + Lrechdrsz + 148, n - Lrechdrsz - 148) < 0)
		fail("Edirty: %r");
	else{
		eqv("entry 1 kind", e.kind, Kdirty);
		if(dirtyrecunpack(&du, e.body, e.len - Lenthdrsz) < 0)
			fail("Edirty: %r");
		else{
			eqv("Edirty op", du.op, 1);
			eqv("Edirty epoch", du.epoch, 44);
			eqv("Edirty peerlen", du.peerlen, 12);
		}
	}
	checks++;
	if(lentunpack(&e, want + Lrechdrsz + 200, n - Lrechdrsz - 200) < 0)
		fail("Eslot: %r");
	else{
		eqv("entry 2 kind", e.kind, Kslot);
		if(slotrecunpack(&slot, e.body, e.len - Lenthdrsz) < 0)
			fail("Eslot: %r");
		else
			eqv("Eslot slot", slot, 100);
	}

	/* §2.7: bits 1..7 of oflags are reserved and MUST be zero */
	memmove(got, want, n);
	got[Lrechdrsz + Lenthdrsz + 18] = 0x02;
	checks++;
	if(objrecunpack(&u, got + Lrechdrsz + Lenthdrsz, 140) == 0)
		fail("Eobj: a reserved oflags bit was accepted");

	flip("log record", want, n, Lrechdrsz + 8, declog);
	free(got);
	free(want);
}

/*
 * §0: a record's checksum is computed over its whole byte range with
 * the checksum field zeroed, and verified the same way — so sealing
 * an image twice is idempotent and reccsumok agrees with reccsumset.
 */
static void
tcsumrule(void)
{
	uchar buf[256], first[Recsumlen];

	pat(buf, sizeof buf, 13);
	reccsumset(buf, sizeof buf, 0);
	memmove(first, buf, Recsumlen);
	checks++;
	if(!reccsumok(buf, sizeof buf, 0))
		fail("reccsumok rejected what reccsumset sealed");
	reccsumset(buf, sizeof buf, 0);
	checks++;
	if(memcmp(first, buf, Recsumlen) != 0)
		fail("sealing twice gave a different checksum");
	buf[100] ^= 1;
	checks++;
	if(reccsumok(buf, sizeof buf, 0))
		fail("reccsumok accepted a flipped byte");
}

/*
 * §0's verify rule is a read.  The obvious implementation zeroes the
 * checksum field in place while it hashes, which is invisible to one
 * proc and wrong for §7's: several procs read the same index and
 * extent-map pages at once, and a reader that catches the transient
 * sixteen zero bytes reports a checksum failure over a record that is
 * perfectly good — which §5 step 10 turns into a lost object.  Two
 * procs verifying one record is the schedule that shows it.
 */
static void
tshared(void)
{
	Idxent e;
	uchar *p;
	int *bad, i, j;

	if((p = malloc(Idxentsz)) == nil || (bad = mallocz(sizeof *bad, 1)) == nil)
		sysfatal("malloc: %r");
	memset(&e, 0, sizeof e);
	e.state = Slive;
	e.oidlen = 8;
	e.vers = Storevers;
	e.len = 4096;
	e.grain0 = 9;
	pat(e.oid, 8, 3);
	idxpack(p, &e);
	for(j = 0; j < 2; j++)
		switch(rfork(RFPROC|RFMEM)){
		case -1:
			sysfatal("rfork: %r");
		case 0:
			for(i = 0; i < Nverify; i++)
				if(!reccsumok(p, Idxentsz, 208))
					(*bad)++;
			exits(nil);
		}
	for(j = 0; j < 2; j++)
		if(waitpid() < 0)
			fail("waitpid: %r");
	checks++;
	if(*bad != 0)
		fail("%d of %d concurrent verifications of one good record "
			"failed", *bad, 2*Nverify);
	free(bad);
	free(p);
}

void
main(int, char**)
{
	tcsumrule();
	tshared();
	tsuper();
	tidx();
	temap();
	tbm();
	tdirt();
	tlog();
	if(fails > 0)
		exits("failed");
	print("structtest: %d checks ok\n", checks);
	exits(nil);
}
