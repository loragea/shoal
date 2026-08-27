#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"

/*
 * T1: the geometry arithmetic of docs/design/store.md §2.1, at the
 * worked example §2.1 states and at the T1 geometry §13 asks for.
 *
 * The worked example is a 4 TiB partition at the defaults, where §2.1
 * says nslots = 2^20, nemap = 2.6e5, the metadata regions cost
 * 256 MiB (index) + 5.1 GiB (extent maps) + 16 MiB (dirty) + 32 MiB
 * (bitmap) + 64 MiB (log), and the whole of it is 0.13% of the
 * partition. Every one of those numbers is asserted below, computed
 * from the doc's own formulas rather than from this implementation:
 *
 *	nblkmax = objmax / blksz
 *	emapsz  = roundup(24 + 20*nblkmax, secsz)
 *	ngrains = datasecs / (blksz / secsz)
 *	nbmpage = ceil(ngrains / (8 * (Wunit - 48)))
 *	nslots  = min(2^20, 4 * ceil(partsize/objmax))
 *	nemap   = ceil(partsize/objmax)
 */

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

static void
eqv(char *what, uvlong got, uvlong want)
{
	checks++;
	if(got != want)
		fail("%s: %llud, want %llud", what, got, want);
}

static void
dflt(Fmtcfg *c)
{
	memset(c, 0, sizeof *c);
	c->secsz = Secszdflt;
	c->blksz = Blkszstore;
	c->objmax = Objmaxdflt;
	c->csumalg = Csumblake2s;
}

/* the 4 TiB worked example of §2.1 */
static void
tworked(void)
{
	Fmtcfg c;
	Super s;
	vlong part;
	uvlong meta, pct;

	part = 4LL*1024*1024*1024*1024;
	dflt(&c);
	if(geometry(&s, &c, part) < 0){
		fail("4 TiB defaults: %r");
		return;
	}
	eqv("nblkmax", s.nblkmax, 1024);
	eqv("emapsz", s.emapsz, 20992);
	eqv("nslots", s.nslots, 1<<20);
	eqv("nemap", s.nemap, 262144);
	eqv("ndirty", s.ndirty, Ndirtydflt);

	eqv("index region bytes", s.idxsecs*(uvlong)s.secsz, 256*1024*1024);
	eqv("extent-map region bytes", s.emapsecs*(uvlong)s.secsz,
		262144ULL*20992);
	eqv("dirty region bytes", s.dirtsecs*(uvlong)s.secsz, 16*1024*1024);
	eqv("log region bytes", s.logsecs*(uvlong)s.secsz, 64*1024*1024);

	/* the bitmap covers the grains that are left once it has its pages */
	eqv("bitmap pages", nbmpage(&s),
		(s.ngrains + bmbits(s.blksz) - 1)/bmbits(s.blksz));
	eqv("bits per bitmap page", bmbits(s.blksz), 8*(16384 - 48));
	eqv("ngrains", s.ngrains, s.datasecs/(s.blksz/s.secsz));

	/* every region inside the partition, in order, without overlap */
	eqv("logoff", s.logoff, s.blksz/s.secsz);
	eqv("index follows the log", s.idxoff >= s.logoff + s.logsecs, 1);
	eqv("emap follows the index", s.emapoff >= s.idxoff + s.idxsecs, 1);
	eqv("dirty follows emap", s.dirtoff >= s.emapoff + s.emapsecs, 1);
	eqv("bitmap follows dirty", s.bmapoff >= s.dirtoff + s.dirtsecs, 1);
	eqv("data follows the bitmap",
		s.dataoff == s.bmapoff + s.bmapsecs, 1);
	eqv("copy 1 follows the data",
		s.dataoff + s.datasecs == (uvlong)part/s.secsz - 1, 1);

	/* §2.1: 0.13% of the partition */
	meta = (uvlong)(s.dataoff - 1)*s.secsz;
	pct = meta*10000/(uvlong)part;
	eqv("metadata, hundredths of a percent", pct, 13);

	/* the checkpoint mark starts at the log's first sector */
	eqv("cklogoff", s.cklogoff, s.logoff);
	eqv("ckseq", s.ckseq, 0);
	eqv("gen", s.gen, 0);
	eqv("hdrlen", s.hdrlen, s.secsz);
	eqv("csumalg", s.csumalg, Csumblake2s);
}

/* the small T1 geometry of §13: a few MiB, nslots and nemap small */
static void
tsmall(void)
{
	Fmtcfg c;
	Super s;
	vlong part;

	part = 8*1024*1024;
	dflt(&c);
	c.blksz = 4096;
	c.objmax = 65536;
	c.nslots = 512;
	c.nemap = 256;
	c.ndirty = 256;
	c.logbytes = 256*1024;
	if(geometry(&s, &c, part) < 0){
		fail("small T1 geometry: %r");
		return;
	}
	eqv("small nblkmax", s.nblkmax, 16);
	eqv("small emapsz", s.emapsz, 512);
	eqv("small nslots", s.nslots, 512);
	eqv("small index bytes", s.idxsecs*(uvlong)s.secsz, 512*256);
	eqv("small emap bytes", s.emapsecs*(uvlong)s.secsz, 256*512);
	eqv("small log bytes", s.logsecs*(uvlong)s.secsz, 256*1024);
	eqv("small ngrains", s.ngrains, s.datasecs/8);
	eqv("small bitmap pages", nbmpage(&s), 1);
}

/*
 * The refusals §2.1 and §12 make MUSTs.  Each one is a geometry that
 * this arithmetic must reject, not merely one it computes oddly.
 */
static void
trefuse(void)
{
	Fmtcfg c;
	Super s;

	dflt(&c);
	c.blksz = 12288;			/* not a power of two */
	checks++;
	if(geometry(&s, &c, 1024*1024*1024) == 0)
		fail("a blksz that is not a power of two was accepted");

	dflt(&c);
	c.objmax = 24576;			/* not a power of two */
	checks++;
	if(geometry(&s, &c, 1024*1024*1024) == 0)
		fail("an objmax that is not a power of two was accepted");

	dflt(&c);
	c.objmax = 1024;			/* below blksz */
	checks++;
	if(geometry(&s, &c, 1024*1024*1024) == 0)
		fail("an objmax below blksz was accepted");

	dflt(&c);
	c.csumalg = 99;
	checks++;
	if(geometry(&s, &c, 1024*1024*1024) == 0)
		fail("an unknown csumalg was accepted");

	/*
	 * §2.7 and §12: a geometry whose maximal Eobj record does not
	 * fit an eighth of the log region is refused.  At the default
	 * blksz and objmax that record is ~28.2 KiB, so a 64 KiB log
	 * is too small and a 1 MiB log is not.
	 */
	dflt(&c);
	c.logbytes = 64*1024;
	checks++;
	if(geometry(&s, &c, 64LL*1024*1024*1024) == 0)
		fail("a log too small for the maximal Eobj record was accepted");
	dflt(&c);
	c.logbytes = 1024*1024;
	checks++;
	if(geometry(&s, &c, 64LL*1024*1024*1024) < 0)
		fail("a 1 MiB log was refused at the default objmax: %r");

	/*
	 * §2.1: ngrains MUST be below 2^32.  At a 512-byte blksz a
	 * 4 TiB partition reaches it.
	 */
	dflt(&c);
	c.blksz = 512;
	c.objmax = 65536;
	c.nslots = 1024;
	c.nemap = 1024;
	c.logbytes = 1024*1024;
	checks++;
	if(geometry(&s, &c, 4LL*1024*1024*1024*1024) == 0)
		fail("a geometry whose ngrains reaches 2^32 was accepted");

	/* a partition with no room for a data region */
	dflt(&c);
	c.nslots = 1024;
	c.nemap = 1024;
	checks++;
	if(geometry(&s, &c, 4*1024*1024) == 0)
		fail("a partition too small for a data region was accepted");

	/*
	 * §2.1: blksz is layer-a's, so every power of two layer-a
	 * permits between secsz and the format's 1 MiB ceiling is
	 * accepted — the device's write unit does not bound it, since
	 * a larger grain is written in Wunit pieces (§0).
	 */
	dflt(&c);
	c.blksz = 4*Blkszstore;			/* 64 KiB, above Wunit */
	c.objmax = 4*1024*1024;
	checks++;
	if(geometry(&s, &c, 64LL*1024*1024*1024) < 0)
		fail("a blksz above the device write unit was refused: %r");
	dflt(&c);
	c.blksz = Blkszmax;
	c.objmax = 64*1024*1024;
	checks++;
	if(geometry(&s, &c, 64LL*1024*1024*1024) < 0)
		fail("a blksz at the format ceiling was refused: %r");
	dflt(&c);
	c.blksz = 2*Blkszmax;
	c.objmax = 64*1024*1024;
	checks++;
	if(geometry(&s, &c, 64LL*1024*1024*1024) == 0)
		fail("a blksz above the format ceiling was accepted");

	/* §2.2: nblkmax is u32, so objmax/blksz must fit one */
	dflt(&c);
	c.objmax = 1ULL<<46;			/* 2^32 blocks at blksz 2^14 */
	checks++;
	if(geometry(&s, &c, 64LL*1024*1024*1024) == 0)
		fail("an objmax whose nblkmax reaches 2^32 was accepted");

	/* §2.7: a record's length is u32, so the log region must fit one */
	dflt(&c);
	c.logbytes = 4LL*1024*1024*1024;
	checks++;
	if(geometry(&s, &c, 64LL*1024*1024*1024) == 0)
		fail("a log region that does not fit a u32 length was accepted");
	dflt(&c);
	c.logbytes = 2LL*1024*1024*1024;
	checks++;
	if(geometry(&s, &c, 64LL*1024*1024*1024) < 0)
		fail("a 2 GiB log was refused: %r");
}

/*
 * §2.7 and §12: the bound the log sizing is checked against is the
 * largest Eobj this geometry can be asked for — a full-length oid,
 * every block named, every old grain freed — and it must be the
 * length the log path would actually emit for that record, rounded
 * up to a sector.
 */
static void
tmaxrec(void)
{
	Fmtcfg c;
	Super s;
	Objrec o;
	uvlong want;

	dflt(&c);
	if(geometry(&s, &c, 64LL*1024*1024*1024) < 0){
		fail("64 GiB defaults: %r");
		return;
	}
	memset(&o, 0, sizeof o);
	o.oidlen = Oidmax;
	o.nmap = s.nblkmax;
	o.nfree = s.nblkmax;
	/* §2.7: 8 + 84 + 128 + 4 + 24*nblkmax + 4 + 4*nblkmax */
	eqv("the maximal Eobj entry", objreclen(&o),
		228 + 28*(uvlong)s.nblkmax);
	want = Lrechdrsz + objreclen(&o);
	want = (want + s.secsz - 1)/s.secsz*s.secsz;
	eqv("the record bound covers it exactly", maxrecbytes(&s), want);
}

/*
 * §2.1: the bitmap covers the grains that are left once it has taken
 * its own pages, which is a fixed point rather than a formula.  It
 * lands on the ceiling wherever a fixed point exists there and one
 * page above it where none does, so the property to hold the sizing
 * to is the bound and the coverage, not an equality — and a reader
 * takes nbmpage from the recorded bmapsecs either way.
 */
static void
tbitmap(void)
{
	Fmtcfg c;
	Super s;
	vlong part;
	uvlong bpp, ceil, over;
	int i;

	over = 0;
	for(i = 1; i <= 300; i++){
		dflt(&c);
		c.blksz = 512;
		c.objmax = 65536;
		c.nslots = 512;
		c.nemap = 256;
		c.ndirty = 256;
		c.logbytes = 256*1024;
		part = (vlong)i*1024*1024 + 512*(i%7);
		if(geometry(&s, &c, part) < 0)
			continue;
		bpp = bmbits(s.blksz);
		ceil = (s.ngrains + bpp - 1)/bpp;
		if(nbmpage(&s) > ceil)
			over++;
		checks++;
		if(nbmpage(&s) < ceil || nbmpage(&s) > ceil + 1)
			fail("a %lld-byte partition took %llud bitmap pages "
				"for %llud grains, ceiling %llud", part,
				nbmpage(&s), s.ngrains, ceil);
		checks++;
		if(nbmpage(&s)*bpp < s.ngrains)
			fail("a %lld-byte partition's bitmap does not cover "
				"its %llud grains", part, s.ngrains);
		checks++;
		if(s.dataoff != s.bmapoff + s.bmapsecs)
			fail("a %lld-byte partition's data does not follow "
				"its bitmap", part);
	}
	/*
	 * And the surplus is real rather than hypothetical: at least
	 * one of those partitions has no fixed point at the ceiling.
	 */
	checks++;
	if(over == 0)
		fail("no swept geometry needed a page above the ceiling");
}

void
main(int, char**)
{
	tworked();
	tsmall();
	trefuse();
	tmaxrec();
	tbitmap();
	if(fails > 0)
		exits("failed");
	print("geomtest: %d checks ok\n", checks);
	exits(nil);
}
