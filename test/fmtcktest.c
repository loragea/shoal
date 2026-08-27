#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"

/*
 * T1: shoalfmt to shoalck, docs/design/store.md §12, over both the
 * simulated disk and a file-backed image.  Both tools are thin front
 * ends over fmtstore and ckstore, which is what §12 requires of the
 * code layout so that a T1 program in test/ can drive them.
 *
 * A formatted store must check clean.  That is a stronger claim than
 * it looks: a zeroed index entry, dirty record or bitmap page fails
 * its own checksum, so a format that merely zeroed those regions
 * would leave a store that condemns every slot at §5 step 10 and
 * reports bmaprebuild=yes at §5 step 5.
 */

enum
{
	Secsz	= 512,
	Nsec	= 16384,		/* an 8 MiB image */
	Seed	= 0xf00d,
};

static int fails;
static int checks;
static int null;

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
smallcfg(Fmtcfg *c)
{
	memset(c, 0, sizeof *c);
	c->secsz = Secsz;
	c->blksz = 4096;
	c->objmax = 65536;
	c->nslots = 512;
	c->nemap = 256;
	c->ndirty = 256;
	c->logbytes = 256*1024;
	c->csumalg = Csumblake2s;
}

static int
check(Dev *d)
{
	Ckcfg c;

	memset(&c, 0, sizeof c);
	c.out = null;
	c.quiet = 1;
	return ckstore(d, &c);
}

/* a formatted store checks clean, and every free record is a valid one */
static void
tround(Dev *d, char *what)
{
	Super s, t;
	Fmtcfg c;
	Sbsel sel;
	Idxent e;
	Dirtent de;
	Bmpage bh;
	uchar *p;
	int n;

	smallcfg(&c);
	if(geometry(&s, &c, d->size) < 0){
		fail("%s: geometry: %r", what);
		return;
	}
	if(fmtstore(d, &s) < 0){
		fail("%s: fmtstore: %r", what);
		return;
	}
	checks++;
	if((n = check(d)) != 0)
		fail("%s: a freshly formatted store reported %d problem(s)",
			what, n);

	if(superselect(d, &sel) < 0){
		fail("%s: superselect: %r", what);
		return;
	}
	t = sel.sb[sel.start];
	eqv("blksz survives the round trip", t.blksz, s.blksz);
	eqv("objmax survives the round trip", t.objmax, s.objmax);
	eqv("csumalg survives the round trip", t.csumalg, Csumblake2s);
	eqv("nslots survives the round trip", t.nslots, s.nslots);
	eqv("nemap survives the round trip", t.nemap, s.nemap);
	eqv("ngrains survives the round trip", t.ngrains, s.ngrains);
	eqv("dataoff survives the round trip", t.dataoff, s.dataoff);
	checks++;
	if(memcmp(t.uuid, s.uuid, 16) != 0)
		fail("%s: the uuid did not survive the round trip", what);

	if((p = malloc(s.blksz)) == nil)
		sysfatal("malloc: %r");

	/* §2.3: every index slot is a valid free entry, not sixteen zeros */
	if(devread(d, p, Idxentsz, idxentoff(&s, s.nslots - 1)) < 0)
		fail("%s: index read: %r", what);
	checks++;
	if(idxunpack(&e, p, s.nemap) < 0)
		fail("%s: the last index entry of a fresh store: %r", what);
	else
		eqv("a fresh index entry is free", e.state, Sfree);

	/* §2.6: the same for a dirty record */
	if(devread(d, p, Dirtentsz, dirtentoff(&s, s.ndirty - 1)) < 0)
		fail("%s: dirty read: %r", what);
	checks++;
	if(dirtunpack(&de, p) < 0)
		fail("%s: the last dirty record of a fresh store: %r", what);
	else
		eqv("a fresh dirty record is free", de.state, 0);

	/* §2.5: every bitmap page has a valid header at ckseq 0 */
	if(devread(d, p, s.blksz, (vlong)s.bmapoff*s.secsz) < 0)
		fail("%s: bitmap read: %r", what);
	checks++;
	if(bmunpack(&bh, p, s.blksz, 0) < 0)
		fail("%s: bitmap page 0 of a fresh store: %r", what);
	else{
		eqv("a fresh bitmap page is at ckseq 0", bh.ckseq, 0);
		/* §2.1: grain 0 is reserved and never allocatable */
		eqv("grain 0 is marked allocated", bmget(p, 0), 1);
		eqv("grain 1 is free", bmget(p, 1), 0);
	}
	free(p);
}

/*
 * §2.1: blksz is layer-a's, bounded by the format and not by the
 * device's write unit, so a store formatted at a grain above that
 * unit must format and check like any other — every region, bitmap
 * page and grain write goes out in Wunit pieces (§0).
 */
static void
tbigblk(void)
{
	Dev *d;
	Super s;
	Fmtcfg c;
	Simop *t;
	Bmpage bh;
	uchar *p;
	long i, n, nw, big;
	int nbad;

	if((d = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	memset(&c, 0, sizeof c);
	c.secsz = Secsz;
	c.blksz = 4*Wunitdflt;			/* 64 KiB: four write units */
	c.objmax = 1024*1024;
	c.nslots = 512;
	c.nemap = 256;
	c.ndirty = 256;
	c.logbytes = 256*1024;
	c.csumalg = Csumblake2s;
	if(geometry(&s, &c, d->size) < 0){
		fail("a geometry at a blksz above the write unit: %r");
		devclose(d);
		return;
	}
	eqv("a grain above the device write unit", s.blksz,
		4*(uvlong)Wunitdflt);
	simtracereset(d);
	if(fmtstore(d, &s) < 0){
		fail("a format at a blksz above the write unit: %r");
		devclose(d);
		return;
	}
	nw = big = 0;
	n = simtrace(d, &t);
	for(i = 0; i < n; i++)
		if(t[i].op == Sopwrite){
			nw++;
			if(t[i].n > (long)d->wunit)
				big++;
		}
	eqv("format requests above the device write unit", big, 0);
	checks++;
	if(nw < (long)(s.blksz/d->wunit))
		fail("a format at a 64 KiB blksz took %ld requests", nw);
	checks++;
	if((nbad = check(d)) != 0)
		fail("a store formatted at a 64 KiB blksz reported %d "
			"problem(s)", nbad);
	if((p = malloc(s.blksz)) == nil)
		sysfatal("malloc: %r");
	if(devread(d, p, s.blksz, (vlong)s.bmapoff*s.secsz) < 0)
		fail("bitmap read: %r");
	checks++;
	if(bmunpack(&bh, p, s.blksz, 0) < 0)
		fail("bitmap page 0 at a 64 KiB blksz: %r");
	else
		eqv("grain 0 is marked allocated at a 64 KiB blksz",
			bmget(p, 0), 1);
	free(p);
	devclose(d);
}

/* the checker earns its keep: it must find what a fault leaves behind */
static void
tdamage(void)
{
	Dev *d;
	Super s;
	Fmtcfg c;
	uchar *p;
	uvlong off;

	if((d = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	smallcfg(&c);
	if(geometry(&s, &c, d->size) < 0 || fmtstore(d, &s) < 0)
		sysfatal("format: %r");
	if((p = malloc(s.blksz)) == nil)
		sysfatal("malloc: %r");

	/* a damaged index entry */
	simpeek(d, idxentoff(&s, 3), p, Idxentsz);
	p[16] ^= 0x80;
	simpoke(d, idxentoff(&s, 3), p, Idxentsz);
	checks++;
	if(check(d) == 0)
		fail("the checker passed a store with a damaged index entry");

	/*
	 * §2.4 and §12: the format does not zero the extent-map region,
	 * because nothing reads a slot no live index entry claims and
	 * the commit that allocates one zeroes it.  So a store formatted
	 * over a region full of another life's bytes checks clean.
	 */
	memset(p, 0xa5, s.blksz);
	for(off = emapentoff(&s, 0); off + s.blksz <= emapentoff(&s, s.nemap);
		off += s.blksz)
		simpoke(d, off, p, s.blksz);
	if(geometry(&s, &c, d->size) < 0 || fmtstore(d, &s) < 0)
		sysfatal("reformat: %r");
	checks++;
	if(check(d) != 0)
		fail("a format over a dirty extent-map region did not check "
			"clean");
	simpeek(d, emapentoff(&s, 1), p, s.blksz);
	for(off = 0; off < s.blksz; off++)
		if(p[off] != 0xa5)
			break;
	checks++;
	if(off < s.blksz)
		fail("the format wrote %llud bytes into the extent-map region",
			(uvlong)s.emapsecs*s.secsz);

	/* undo it, and damage a bitmap page instead */
	if(geometry(&s, &c, d->size) < 0 || fmtstore(d, &s) < 0)
		sysfatal("reformat: %r");
	checks++;
	if(check(d) != 0)
		fail("the reformatted store did not check clean");
	simpeek(d, (vlong)s.bmapoff*s.secsz, p, s.blksz);
	p[64] ^= 0x80;
	simpoke(d, (vlong)s.bmapoff*s.secsz, p, s.blksz);
	checks++;
	if(check(d) == 0)
		fail("the checker passed a store with a damaged bitmap page");

	free(p);
	devclose(d);
}

/* §12: shoalfmt refuses a partition that already carries a superblock */
static void
treformat(void)
{
	Dev *d;
	Super s;
	Fmtcfg c;
	Sbsel sel;
	uchar first[16];

	if((d = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	smallcfg(&c);
	if(geometry(&s, &c, d->size) < 0 || fmtstore(d, &s) < 0)
		sysfatal("format: %r");
	memmove(first, s.uuid, 16);

	/* the tool's guard is superselect succeeding on an unreamed disk */
	checks++;
	if(superselect(d, &sel) != 0)
		fail("a formatted store did not present a valid superblock");

	/* a reformat generates a fresh identity unless one is given */
	if(geometry(&s, &c, d->size) < 0 || fmtstore(d, &s) < 0)
		sysfatal("reformat: %r");
	checks++;
	if(memcmp(first, s.uuid, 16) == 0)
		fail("a reformat reused the previous instance's uuid");

	c.uuidset = 1;
	memmove(c.uuid, first, 16);
	if(geometry(&s, &c, d->size) < 0)
		sysfatal("geometry: %r");
	checks++;
	if(memcmp(first, s.uuid, 16) != 0)
		fail("-u did not set the uuid");
	devclose(d);
}

/*
 * §12: a format or a ream cut short leaves no valid superblock.  The
 * ream is the case that matters — without the invalidation that
 * begins a format, a ream interrupted before its superblock writes
 * leaves the previous instance's superblocks valid over regions that
 * have just been overwritten, so the disk comes back empty wearing
 * the identity its peers still believe in (layer-a §3.4), naming a
 * checkpoint over a log that has been zeroed, and shoalck sees
 * nothing wrong with it.
 */
static void
tcutream(void)
{
	Dev *d;
	Super s;
	Fmtcfg c;
	Sbsel sel;
	uchar uuid[16];

	if((d = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	smallcfg(&c);
	if(geometry(&s, &c, d->size) < 0 || fmtstore(d, &s) < 0)
		sysfatal("format: %r");
	memmove(uuid, s.uuid, 16);
	checks++;
	if(superselect(d, &sel) != 0 || memcmp(sel.sb[sel.start].uuid, uuid, 16) != 0)
		fail("the first format left no superblock to ream over");

	/* the ream dies on its first index write, and the power goes */
	if(geometry(&s, &c, d->size) < 0)
		sysfatal("geometry: %r");
	simfaultat(d, Sfeio, 1, idxentoff(&s, 0), Idxentsz);
	checks++;
	if(fmtstore(d, &s) == 0)
		fail("a format whose region write failed reported success");
	simfault(d, Sfnone, 0);
	simcrash(d);

	checks++;
	if(superselect(d, &sel) == 0)
		fail("a ream cut short left copy %d valid, gen %llud",
			sel.start, sel.sb[sel.start].gen);
	checks++;
	if(check(d) == 0)
		fail("shoalck passed a store left by a ream cut short");
	devclose(d);
}

/*
 * The live-object half of the checker, §12: the bitmap cross-check,
 * the extent maps a live entry claims, and the log scan from the
 * checkpoint mark.  A formatted store has no live object in it, so
 * none of that runs unless a test builds one — and the entries are
 * built through the codecs, at the offsets §2 gives them, rather than
 * by the write path, which does not exist yet.
 */

static char *ckpath = "/tmp/shoalcktest.out";
static char ckbuf[65536];

/* run the checker, keeping its report for what it said as well as how much */
static int
report(Dev *d, char *oid)
{
	Ckcfg c;
	int fd, bad;
	long n;

	memset(&c, 0, sizeof c);
	if((fd = create(ckpath, ORDWR, 0666)) < 0)
		sysfatal("create %s: %r", ckpath);
	c.out = fd;
	c.verbose = 1;
	c.oid = oid;
	bad = ckstore(d, &c);
	seek(fd, 0, 0);
	if((n = readn(fd, ckbuf, sizeof ckbuf - 1)) < 0)
		n = 0;
	ckbuf[n] = '\0';
	close(fd);
	remove(ckpath);
	return bad;
}

static void
said(char *what, char *want)
{
	checks++;
	if(strstr(ckbuf, want) == nil)
		fail("%s: the report does not say `%s'", what, want);
}

static void
putidx(Dev *d, Super *s, ulong slot, Idxent *e)
{
	uchar p[Idxentsz];

	idxpack(p, e);
	simpoke(d, idxentoff(s, slot), p, Idxentsz);
}

/* an extent map of nblk grains starting at g0, sealed */
static void
putemap(Dev *d, Super *s, ulong slot, ulong nblk, ulong g0, ulong past)
{
	uchar *p;
	Emap m;
	ulong i;

	if((p = mallocz(s->emapsz, 1)) == nil)
		sysfatal("malloc: %r");
	for(i = 0; i < nblk; i++){
		emapsetgrain(p, i, g0 + i);
		memset(emapdig(p, s->nblkmax, i), 0x11 + i, Blkdlen);
	}
	if(past != 0)
		emapsetgrain(p, nblk, past);
	m.nblk = nblk;
	m.vers = Storevers;
	emappack(p, s->emapsz, &m);
	simpoke(d, emapentoff(s, slot), p, s->emapsz);
	free(p);
}

static void
markgrain(Dev *d, Super *s, uvlong g, int on)
{
	uchar *p;
	Bmpage h;
	uvlong page, bpp, off;

	bpp = bmbits(s->blksz);
	page = g/bpp;
	off = (uvlong)s->bmapoff*s->secsz + page*(uvlong)s->blksz;
	if((p = malloc(s->blksz)) == nil)
		sysfatal("malloc: %r");
	simpeek(d, off, p, s->blksz);
	if(on)
		bmset(p, g % bpp);
	else
		bmclr(p, g % bpp);
	memset(&h, 0, sizeof h);
	h.vers = Storevers;
	h.page = page;
	h.ckseq = 0;
	bmpack(p, s->blksz, &h);
	simpoke(d, off, p, s->blksz);
	free(p);
}

/* one log record at the checkpoint mark, carrying no entries */
static void
putlog(Dev *d, Super *s, uvlong seq)
{
	uchar *p;
	Lrec r;

	if((p = mallocz(s->secsz, 1)) == nil)
		sysfatal("malloc: %r");
	memset(&r, 0, sizeof r);
	r.vers = Storevers;
	r.nsec = 1;
	r.seq = seq;
	r.nent = 0;
	lrecpack(p, &r, s->secsz);
	simpoke(d, (uvlong)s->cklogoff*s->secsz, p, s->secsz);
	free(p);
}

/* patch a u32 field in both superblock copies and re-seal them */
static void
sbpoke32(Dev *d, Super *s, ulong off, ulong v)
{
	uchar *sb;
	int i;

	if((sb = malloc(s->secsz)) == nil)
		sysfatal("malloc: %r");
	for(i = 0; i < 2; i++){
		vlong o;

		o = i == 0 ? 0 : super1off(d);
		simpeek(d, o, sb, s->secsz);
		PBIT32(sb + off, v);
		reccsumset(sb, s->secsz, 16);
		simpoke(d, o, sb, s->secsz);
	}
	free(sb);
}

/* the same for a u64 field */
static void
sbpoke64(Dev *d, Super *s, ulong off, uvlong v)
{
	uchar *sb;
	int i;

	if((sb = malloc(s->secsz)) == nil)
		sysfatal("malloc: %r");
	for(i = 0; i < 2; i++){
		vlong o;

		o = i == 0 ? 0 : super1off(d);
		simpeek(d, o, sb, s->secsz);
		PBIT64(sb + off, v);
		reccsumset(sb, s->secsz, 16);
		simpoke(d, o, sb, s->secsz);
	}
	free(sb);
}

/*
 * A store with two live objects in it: a one-block object whose map
 * is inline (§2.3), and a three-block one with an extent map (§2.4).
 * Grains 1 and 2..4 are theirs; grain 0 is the reserved one.
 */
static void
live(Dev *d, Super *s, Fmtcfg *c)
{
	Idxent e;
	int i;

	if(geometry(s, c, d->size) < 0 || fmtstore(d, s) < 0)
		sysfatal("format: %r");

	memset(&e, 0, sizeof e);
	e.state = Slive;
	e.oidlen = 5;
	e.vers = Storevers;
	e.qidpath = 11;
	e.len = 1000;
	e.ver = 1;
	e.grain0 = 1;
	memmove(e.oid, "small", 5);
	memset(e.dig0, 0x22, Blkdlen);
	putidx(d, s, 1, &e);

	memset(&e, 0, sizeof e);
	e.state = Slive;
	e.oidlen = 3;
	e.vers = Storevers;
	e.qidpath = 12;
	e.len = 2*(uvlong)s->blksz + 100;	/* three blocks */
	e.ver = 1;
	e.emapslot = 5;
	memmove(e.oid, "big", 3);
	putidx(d, s, 2, &e);
	putemap(d, s, 5, 3, 2, 0);

	for(i = 1; i <= 4; i++)
		markgrain(d, s, i, 1);
}

static void
tlive(void)
{
	Dev *d;
	Super s;
	Fmtcfg c;
	Idxent e;

	if((d = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	smallcfg(&c);

	/* the clean case: two live objects, four grains, one log record */
	live(d, &s, &c);
	putlog(d, &s, s.ckseq + 1);
	checks++;
	if(report(d, nil) != 0)
		fail("a store with two live objects reported problems");
	said("live objects", "index: 2 live");
	said("the grain cross-check", "4 referenced");
	said("the extent maps", "extent maps: 1 claimed, 0 bad");
	said("the log scan", "log: 1 valid records");

	/* §5 step 7: a record whose seq is not the successor is not one */
	putlog(d, &s, s.ckseq + 7);
	checks++;
	if(report(d, nil) != 0)
		fail("a log record out of sequence was a problem, not a stop");
	said("an out-of-sequence log record", "log: 0 valid records");

	/* -o dumps the object's entry and every block of its map */
	live(d, &s, &c);
	report(d, "big");
	said("-o", "block 2: grain 4");

	/* a bit set under no live map at all */
	live(d, &s, &c);
	markgrain(d, &s, 9, 1);
	checks++;
	if(report(d, nil) == 0)
		fail("the checker passed a leaked grain");
	said("a leaked grain", "referenced by nothing");

	/* a bit cleared under a grain a live map names */
	live(d, &s, &c);
	markgrain(d, &s, 3, 0);
	checks++;
	if(report(d, nil) == 0)
		fail("the checker passed a phantom grain");
	said("a phantom grain", "clear in the bitmap");

	/* two live entries naming one grain */
	live(d, &s, &c);
	memset(&e, 0, sizeof e);
	e.state = Slive;
	e.oidlen = 5;
	e.vers = Storevers;
	e.len = 1000;
	e.grain0 = 3;			/* block 1 of the big object */
	memmove(e.oid, "small", 5);
	putidx(d, &s, 1, &e);
	checks++;
	if(report(d, nil) == 0)
		fail("the checker passed a grain referenced twice");
	said("a grain referenced twice", "is referenced twice");

	/* a grain number the data region does not have */
	live(d, &s, &c);
	memset(&e, 0, sizeof e);
	e.state = Slive;
	e.oidlen = 5;
	e.vers = Storevers;
	e.len = 1000;
	e.grain0 = s.ngrains;
	memmove(e.oid, "small", 5);
	putidx(d, &s, 1, &e);
	checks++;
	if(report(d, nil) == 0)
		fail("the checker passed a grain at ngrains");
	said("a grain past the data region", "ngrains is");

	/* §2.4: an extent-map slot naming a grain at or beyond nblk */
	live(d, &s, &c);
	putemap(d, &s, 5, 3, 2, 8);
	checks++;
	if(report(d, nil) == 0)
		fail("the checker passed a grain beyond nblk");
	said("a grain beyond nblk", "at or beyond nblk");

	/* §2.4: nblk is blkcount(len) and nothing else */
	live(d, &s, &c);
	putemap(d, &s, 5, 2, 2, 0);
	checks++;
	if(report(d, nil) == 0)
		fail("the checker passed an nblk that len contradicts");
	said("an nblk len contradicts", "says nblk");

	/* §2.3: a multi-block entry must have an extent-map slot */
	live(d, &s, &c);
	memset(&e, 0, sizeof e);
	e.state = Slive;
	e.oidlen = 3;
	e.vers = Storevers;
	e.len = 2*(uvlong)s.blksz + 100;
	e.emapslot = 0;
	memmove(e.oid, "big", 3);
	putidx(d, &s, 2, &e);
	checks++;
	if(report(d, nil) == 0)
		fail("the checker passed a multi-block entry with no map");
	said("a multi-block entry with no map", "no extent-map slot");

	/* §2.1: grain 0 is reserved and MUST be marked allocated */
	live(d, &s, &c);
	markgrain(d, &s, 0, 0);
	checks++;
	if(report(d, nil) == 0)
		fail("the checker passed a store with grain 0 free");
	said("grain 0 free", "grain 0 is not marked allocated");

	/*
	 * §14(8): a digest is meaningless without the algorithm behind
	 * it, so the checker says so rather than believing every digest
	 * it then verifies.
	 */
	live(d, &s, &c);
	sbpoke32(d, &s, 260, 99);		/* csumalg */
	checks++;
	if(report(d, nil) == 0)
		fail("the checker passed an unknown csumalg");
	said("an unknown csumalg", "csumalg 99");

	/* §2.1: every region start is a blksz boundary */
	live(d, &s, &c);
	sbpoke32(d, &s, 112, s.logoff + 1);	/* logoff */
	checks++;
	if(report(d, nil) == 0)
		fail("the checker passed a misaligned region start");
	said("a misaligned region", "not a 4096-byte boundary");

	/*
	 * §2.4: emapsz must cover 24 + 20*nblkmax, and nblkmax is a
	 * u32 the superblock supplies, so the sum reaches 2^36 and a
	 * ulong comparison wraps it.  A superblock naming 2^30 blocks
	 * with the 512-byte emapsz this geometry formatted must be
	 * caught here and by name: past this check ckindex allocates
	 * emapsz bytes and ckemap walks nblkmax entries of it, which
	 * is 4 GiB beyond the buffer.
	 */
	live(d, &s, &c);
	sbpoke64(d, &s, 72, 1ULL<<42);		/* objmax */
	sbpoke32(d, &s, 80, 1UL<<30);		/* nblkmax */
	checks++;
	if(report(d, nil) == 0)
		fail("the checker passed an emapsz too small for nblkmax");
	said("an emapsz too small for its nblkmax", "is too small for");

	/*
	 * A checker is the one tool run against hostile bytes: a
	 * checksum-valid superblock with a zero blksz must be reported,
	 * not divided by.
	 */
	live(d, &s, &c);
	sbpoke32(d, &s, 68, 0);			/* blksz */
	checks++;
	if(report(d, nil) == 0)
		fail("the checker passed a zero blksz");
	said("a zero blksz", "are not a geometry");
	devclose(d);
}

void
main(int, char**)
{
	Dev *d;
	char *path;

	if((null = open("/dev/null", OWRITE)) < 0)
		null = 2;

	if((d = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	tround(d, "simulated disk");
	devclose(d);

	path = "/tmp/shoalfmtcktest.img";
	remove(path);
	if((d = fileopen(path, Secsz, (vlong)Nsec*Secsz, 0)) == nil)
		sysfatal("fileopen: %r");
	tround(d, "file image");
	devclose(d);
	/* and it still checks clean after a close and reopen */
	if((d = fileopen(path, Secsz, 0, Drdonly)) == nil)
		sysfatal("reopen: %r");
	checks++;
	if(check(d) != 0)
		fail("file image: a reopened store reported problems");
	devclose(d);
	remove(path);

	tbigblk();
	tdamage();
	treformat();
	tcutream();
	tlive();
	if(fails > 0)
		exits("failed");
	print("fmtcktest: %d checks ok\n", checks);
	exits(nil);
}
