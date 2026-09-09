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
istrue(char *what, int ok)
{
	checks++;
	if(!ok)
		fail("%s", what);
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

/*
 * §13 says a test that wants a file drives it through its own file
 * under /tmp.  The name carries this program's pid because /tmp is
 * shared: two runs of fmtcktest at once — one mk test beside another,
 * or a mutant being timed against the tree — otherwise create, write,
 * read back and remove the *same* four files.  What that looks like
 * is not a collision but a scatter of content assertions failing
 * against a report of zero bytes: the other run's create truncated it,
 * or its remove took it out from under the fd this one is reading.
 */
static char ckpath[64];
static char ckbuf[65536];

/* run the checker, keeping its report for what it said as well as how much */
static int
runck(Dev *d, Ckcfg *c)
{
	int fd, bad;
	long n;

	if((fd = create(ckpath, ORDWR, 0666)) < 0)
		sysfatal("create %s: %r", ckpath);
	c->out = fd;
	bad = ckstore(d, c);
	seek(fd, 0, 0);
	n = readn(fd, ckbuf, sizeof ckbuf - 1);
	/*
	 * A report this cannot read is a fault in the harness, and one
	 * loud line is the honest way to say so: mapping it onto an empty
	 * buffer turns it into whichever content assertions happen to
	 * come next, which is a misdiagnosis of every one of them.
	 */
	if(n <= 0)
		sysfatal("%s: the checker's report reads back as %ld bytes: %r",
			ckpath, n);
	if(n >= (long)sizeof ckbuf - 1)
		sysfatal("%s: the checker's report fills the %d-byte buffer",
			ckpath, (int)sizeof ckbuf);
	ckbuf[n] = '\0';
	close(fd);
	remove(ckpath);
	return bad;
}

static int
report(Dev *d, char *oid)
{
	Ckcfg c;

	memset(&c, 0, sizeof c);
	c.verbose = 1;
	c.oid = oid;
	return runck(d, &c);
}

/* §12's -v and -R, whose reports the tests below read back */
static int
scrub(Dev *d, int verify, int rebuild)
{
	Ckcfg c;

	memset(&c, 0, sizeof c);
	c.verify = verify;
	c.rebuild = rebuild;
	return runck(d, &c);
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

	/*
	 * §2.6: the store refuses to open over ndirty == 0 — applydirty
	 * would have nothing to drop for the first Edirty — so the
	 * checker flags the geometry by the same rule.
	 */
	live(d, &s, &c);
	sbpoke32(d, &s, 96, 0);			/* ndirty */
	checks++;
	if(report(d, nil) == 0)
		fail("the checker passed a geometry with no dirty region");
	said("no dirty region", "no dirty region");
	devclose(d);
}

/*
 * §5 step 7 refuses to start on a checksummed, in-sequence record
 * whose entries fail the apply's own decode and range checks — and
 * the refusal's message names shoalck as the way out, so shoalck
 * MUST flag the very record the store refuses on.  Built on a
 * file-backed image, as an operator would run it, and judged under
 * -q as well: a problem is counted, not merely printed.
 *
 * The record carries the two §2.7 refusals a conforming writer never
 * produces: an Eobj naming two blocks with no extent-map slot, and
 * an Eslot at nslots.
 *
 * Mutation: drop cklog's entry validation (print-only, as before),
 * and both reports below come back clean.
 */
static char badpath[64];

static void
tbadlog(void)
{
	Dev *d;
	Super s;
	Fmtcfg c;
	Lrec r;
	Objrec o;
	uchar *rec;
	long n;

	remove(badpath);
	if((d = fileopen(badpath, Secsz, (vlong)Nsec*Secsz, 0)) == nil)
		sysfatal("fileopen: %r");
	smallcfg(&c);
	if(geometry(&s, &c, d->size) < 0 || fmtstore(d, &s) < 0)
		sysfatal("format: %r");
	if((rec = mallocz(s.secsz, 1)) == nil)
		sysfatal("mallocz: %r");
	memset(&o, 0, sizeof o);
	o.slot = 1;
	o.emapslot = 0;
	o.qidpath = 99;
	o.state = Slive;
	o.oidlen = 2;
	memmove(o.oid, "q1", 2);
	o.len = 2*(uvlong)s.blksz;	/* two blocks, inline map */
	o.ver = 1;
	o.wepoch = 1;
	n = objrecpack(rec + Lrechdrsz, s.secsz - Lrechdrsz, &o);
	checks++;
	if(n < 0)
		fail("objrecpack: %r");
	else if(slotrecpack(rec + Lrechdrsz + n,
		s.secsz - Lrechdrsz - n, s.nslots) < 0)
		fail("slotrecpack: %r");
	else{
		memset(&r, 0, sizeof r);
		r.vers = Storevers;
		r.nsec = 1;
		r.seq = s.ckseq + 1;
		r.nent = 2;
		lrecpack(rec, &r, s.secsz);
		if(devwrite(d, rec, s.secsz, (vlong)s.cklogoff*s.secsz) < 0)
			fail("writing the bad record: %r");
		checks++;
		if(check(d) == 0)
			fail("shoalck -q counted no problem on the record "
				"replay refuses to start on");
		checks++;
		if(report(d, nil) == 0)
			fail("shoalck passed the record replay refuses to "
				"start on");
		else{
			said("the unappliable Eobj", "no extent-map slot");
			said("the out-of-range Eslot", "Eslot: slot");
		}
	}
	free(rec);
	devclose(d);
	remove(badpath);
}

/*
 * §12's -v and -R, the two flags that work on the REPLAYED state
 * rather than on the checkpoint every pass above reads.  §2.8 makes
 * the log the authority for everything since ckseq and §3.5 defers a
 * released grain's reuse only until the freeing commit's flush, so a
 * grain a committed-but-not-checkpointed record freed may already
 * hold another object's bytes: verifying from the checkpointed index
 * would report a mismatch on a sound object, and a rebuild from it
 * would clear grains the log has since handed out.
 *
 * These are the first cases here that drive the store engine, because
 * they are the first that need content on the disk rather than
 * entries poked into the regions.
 */

enum
{
	Vblk	= 4096,		/* smallcfg's blksz */
};

/* the engine with no procs at all: storeopen's replay is all that runs */
static Store*
opens(Dev *d, char *what)
{
	Storecfg c;
	Store *s;

	memset(&c, 0, sizeof c);
	c.nockptproc = 1;		/* spawn is nil, so there is no proc */
	if((s = storeopen(d, &c)) == nil)
		fail("%s: storeopen: %r", what);
	return s;
}

static uchar*
mkbuf(long n, int seed)
{
	uchar *p;
	long i;

	if((p = malloc(n)) == nil)
		sysfatal("malloc: %r");
	for(i = 0; i < n; i++)
		p[i] = (uchar)(seed*7 + i*31 + (i>>8)*13);
	return p;
}

static void
mk(Store *s, char *name, uvlong ver)
{
	if(objcreate(s, (uchar*)name, strlen(name), ver, 1, nil, 0, nil) < 0)
		fail("objcreate %s: %r", name);
}

static void
wr(Store *s, char *name, void *a, long n, uvlong off, uvlong ver)
{
	if(objwrite(s, (uchar*)name, strlen(name), a, n, off, ver, 1, nil, 0) < 0)
		fail("objwrite %s %ld at %llud: %r", name, n, off);
}

static void
tr(Store *s, char *name, uvlong len, uvlong ver)
{
	if(objtrunc(s, (uchar*)name, strlen(name), len, ver, 1, nil, 0) < 0)
		fail("objtrunc %s to %llud: %r", name, len);
}

static void
rm(Store *s, char *name, uvlong ver)
{
	if(objremove(s, (uchar*)name, strlen(name), ver, 1, nil, 0) < 0)
		fail("objremove %s: %r", name);
}

static void
ckpt(Store *s)
{
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
}

/* the checkpointed index entry of one object, and one block's grain */
static ulong
slotof(Dev *d, Super *s, char *name, Idxent *e)
{
	uchar p[Idxentsz];
	ulong slot;
	int n;

	n = strlen(name);
	for(slot = 0; slot < s->nslots; slot++){
		if(devread(d, p, Idxentsz, idxentoff(s, slot)) < 0)
			break;
		if(idxunpack(e, p, s->nemap) < 0 || e->state == Sfree)
			continue;
		if(e->oidlen == n && memcmp(e->oid, name, n) == 0)
			return slot;
	}
	return ~0UL;
}

static ulong
grainof(Dev *d, Super *s, Idxent *e, ulong blk)
{
	uchar *p;
	ulong g;

	if(e->emapslot == 0)
		return e->grain0;
	if((p = malloc(s->emapsz)) == nil)
		sysfatal("malloc: %r");
	g = 0;
	if(devread(d, p, s->emapsz, emapentoff(s, e->emapslot)) == 0)
		g = emapgrain(p, blk);
	free(p);
	return g;
}

/*
 * Move one bit of the on-disk bitmap and re-seal the page with the
 * engine's own packer, keeping the ckseq the page already carried.
 * The result is a page that PASSES its checksum and is wrong, which
 * is the one §2.5's automatic rebuild never fires on and the one -R
 * exists for.
 */
static void
pokebit(Dev *d, Super *s, uvlong g, int on)
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
	if(bmunpack(&h, p, s->blksz, page) < 0)
		sysfatal("bitmap page %llud: %r", page);
	if(on)
		bmset(p, g % bpp);
	else
		bmclr(p, g % bpp);
	bmpack(p, s->blksz, &h);
	simpoke(d, off, p, s->blksz);
	free(p);
}

static Dev*
newstore(Super *s)
{
	Dev *d;
	Fmtcfg c;

	if((d = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	smallcfg(&c);
	if(geometry(s, &c, d->size) < 0 || fmtstore(d, s) < 0)
		sysfatal("format: %r");
	return d;
}

/*
 * -v over a store with a multi-block object, a hole and a tombstone.
 * It must pass a sound store, write nothing at all, and name the
 * object and the block index when a grain is overwritten under it —
 * which the checkpoint passes cannot see, because they never read
 * content.
 *
 * Mutation: objverify records no mismatch (mut verify-nocompare).
 */
static void
tverify(void)
{
	Dev *d;
	Super s;
	Store *st;
	Idxent e;
	uchar *buf, blk[16];
	ulong slot, g;
	int bad;

	d = newstore(&s);
	if((st = opens(d, "verify")) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(3*Vblk, 3);
	mk(st, "one", 1);
	wr(st, "one", buf, 1000, 0, 2);
	mk(st, "many", 1);
	wr(st, "many", buf, 3*Vblk, 0, 2);
	mk(st, "holed", 1);
	wr(st, "holed", buf, 100, 2*Vblk, 2);	/* blocks 0 and 1 are holes */
	mk(st, "gone", 1);
	rm(st, "gone", 2);
	ckpt(st);
	storeclose(st);

	simtracereset(d);
	checks++;
	if((bad = scrub(d, 1, 0)) != 0)
		fail("-v reported %d problem(s) on a sound store", bad);
	said("-v", "3 objects verified");
	said("-v skips tombstones", "1 tombstones skipped");
	/*
	 * §12's "reads and never writes" is asserted in tvdirty below,
	 * on the store that can make it false: this one was checkpointed
	 * before it was closed, so its replay applies nothing and an
	 * empty trace here would prove nothing about -v.
	 */
	slot = slotof(d, &s, "many", &e);
	istrue("the checkpointed index has many", slot != ~0UL);
	g = grainof(d, &s, &e, 1);
	istrue("block 1 of many has a grain", g != 0);
	simpeek(d, grainoff(&s, g), blk, sizeof blk);
	blk[0] ^= 0x5a;
	simpoke(d, grainoff(&s, g), blk, sizeof blk);
	checks++;
	if(scrub(d, 0, 0) != 0)
		fail("the checkpoint passes must not see a poked grain: they "
			"never read content");
	checks++;
	if(scrub(d, 1, 0) == 0)
		fail("-v passed an object whose grain was overwritten");
	said("-v names the object", "oid many");
	said("-v names the block and clears the array",
		"(blocks 1), arraybad=0");
	free(buf);
	devclose(d);

	/*
	 * §8: an object flagged corrupt that verifies clean is
	 * information and not a problem — the flag is durable and it is
	 * the online scrub's key-preserving Eobj that clears it.
	 */
	d = newstore(&s);
	if((st = opens(d, "corrupt flag")) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(Vblk, 9);
	mk(st, "flagged", 1);
	wr(st, "flagged", buf, Vblk, 0, 2);
	if(objcorrupt(st, (uchar*)"flagged", 7, 1, nil, 0) < 0)
		fail("objcorrupt: %r");
	ckpt(st);
	storeclose(st);
	checks++;
	if((bad = scrub(d, 1, 0)) != 0)
		fail("-v made a problem of %d corrupt-flagged object(s) that "
			"verify clean", bad);
	said("the corrupt flag", "flagged corrupt and verifies clean");
	said("the corrupt count", "1 flagged corrupt but clean");
	free(buf);
	devclose(d);
}

/*
 * -v works on the replayed state.  A checkpoint, then a truncate that
 * frees a grain and a create that is handed it, and no second
 * checkpoint: the checkpointed index still names the freed grain as
 * A's third block, and the bytes there are now B's.
 *
 * Mutation: storeopen skips replay (mut verify-noreplay), which is
 * exactly "verify from the checkpointed index".
 */
static void
tvreplay(void)
{
	Dev *d;
	Super s;
	Store *st;
	Idxent e;
	uchar *abuf, *bbuf, *gbuf;
	ulong g2;
	int bad;

	d = newstore(&s);
	if((st = opens(d, "replayed verify")) == nil){
		devclose(d);
		return;
	}
	abuf = mkbuf(3*Vblk, 11);
	bbuf = mkbuf(Vblk, 29);
	mk(st, "A", 1);
	wr(st, "A", abuf, 3*Vblk, 0, 2);
	ckpt(st);
	istrue("the checkpointed index has A", slotof(d, &s, "A", &e) != ~0UL);
	g2 = grainof(d, &s, &e, 2);
	istrue("block 2 of A has a grain", g2 != 0);
	tr(st, "A", 2*Vblk, 3);		/* frees it; §6 parks the cursor there */
	mk(st, "B", 1);
	wr(st, "B", bbuf, Vblk, 0, 2);	/* ... and B is handed it */
	storeclose(st);			/* no second checkpoint */

	if((gbuf = malloc(Vblk)) == nil)
		sysfatal("malloc: %r");
	simpeek(d, grainoff(&s, g2), gbuf, Vblk);
	istrue("B was handed the grain A's truncate freed",
		memcmp(gbuf, bbuf, Vblk) == 0);
	checks++;
	if((bad = scrub(d, 1, 0)) != 0)
		fail("-v on the replayed state reported %d problem(s)", bad);
	said("both objects", "2 objects verified");
	free(gbuf);
	free(abuf);
	free(bbuf);
	devclose(d);
}

/*
 * §12's "reads and never writes", on the store that can make it
 * false: a checkpoint, then a multi-block commit that is NOT
 * checkpointed, then a close.  Replay applies that record, which
 * dirties an extent map, and a write-back at the end of replay would
 * both break the claim on a writable device and strand the whole
 * store on a read-only one — which is every store -v exists for,
 * since a store with nothing in its log since the checkpoint is a
 * store that stopped cleanly.
 *
 * Mutation: replay writes the maps it dirtied back before returning
 * (mut replay-writes-emaps).
 */
static void
tvdirty(void)
{
	Dev *d;
	Super s;
	Store *st;
	Simop *t;
	uchar *buf;
	long i, n, nw;
	int bad;

	d = newstore(&s);
	if((st = opens(d, "an un-checkpointed multi-block commit")) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(3*Vblk, 43);
	mk(st, "before", 1);
	wr(st, "before", buf, Vblk, 0, 2);
	ckpt(st);
	mk(st, "many", 1);
	wr(st, "many", buf, 3*Vblk, 0, 2);
	storeclose(st);				/* no second checkpoint */

	/*
	 * Dev.rdonly is what a read-only open sets and what devwrite
	 * refuses on, whichever device opened it, so setting it here is
	 * the same store shoalck -v is handed.
	 */
	d->rdonly = 1;
	simtracereset(d);
	checks++;
	if((bad = scrub(d, 1, 0)) != 0)
		fail("-v on a read-only store with an un-checkpointed "
			"multi-block commit reported %d problem(s)", bad);
	said("-v after an unclean stop", "2 objects verified");
	n = simtrace(d, &t);
	nw = 0;
	for(i = 0; i < n; i++)
		if(t[i].op == Sopwrite)
			nw++;
	eqv("-v writes nothing on a read-only store", nw, 0);
	d->rdonly = 0;

	simtracereset(d);
	checks++;
	if((bad = scrub(d, 1, 0)) != 0)
		fail("-v on the same store opened writable reported %d "
			"problem(s)", bad);
	n = simtrace(d, &t);
	nw = 0;
	for(i = 0; i < n; i++)
		if(t[i].op == Sopwrite)
			nw++;
	eqv("-v writes nothing on a writable store either", nw, 0);
	free(buf);
	devclose(d);
}

/*
 * -R on a bitmap page that is valid and wrong.  §2.5 repairs a page
 * that fails its checksum automatically, so the page a plain start
 * believes is this one, and -R is what corrects it offline.
 *
 * Mutation: storeopen ignores Storecfg.forcerebuild (mut rebuild-skip).
 */
static void
trebuildoff(void)
{
	Dev *d;
	Super s;
	Store *st;
	Storestat s0, s1;
	Idxent e;
	uchar *buf;
	ulong g;
	int bad;

	d = newstore(&s);
	if((st = opens(d, "offline rebuild")) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(3*Vblk, 5);
	mk(st, "one", 1);
	wr(st, "one", buf, 1000, 0, 2);
	mk(st, "many", 1);
	wr(st, "many", buf, 3*Vblk, 0, 2);
	ckpt(st);
	storestat(st, &s0);
	storeclose(st);

	istrue("the checkpointed index has many",
		slotof(d, &s, "many", &e) != ~0UL);
	g = grainof(d, &s, &e, 1);
	istrue("block 1 of many has a grain", g != 0);
	pokebit(d, &s, g, 0);			/* a used grain, cleared */
	pokebit(d, &s, s.ngrains - 1, 1);	/* two free grains, set */
	pokebit(d, &s, s.ngrains - 2, 1);

	if((st = opens(d, "a valid but wrong bitmap")) != nil){
		storestat(st, &s1);
		eqv("a page that passes its checksum is not rebuilt at start",
			s1.bmaprebuild, 0);
		eqv("and its wrong free count is what the store believes",
			s1.grainfree, s0.grainfree - 1);
		storeclose(st);
	}
	checks++;
	if(scrub(d, 0, 0) == 0)
		fail("the cross-check passed a valid but wrong bitmap");

	checks++;
	if(scrub(d, 0, 1) == 0)
		fail("-R reported nothing about the bitmap it found wrong");
	said("-R rebuilds", "bmaprebuild=yes");
	said("-R prints the two counts", "the rebuild leaves");
	said("-R rewrites the checkpoint", "checkpoint rewritten at ckseq");

	checks++;
	if((bad = scrub(d, 0, 0)) != 0)
		fail("the store reported %d problem(s) after -R", bad);
	if((st = opens(d, "after -R")) != nil){
		storestat(st, &s1);
		eqv("the rebuilt free map equals a full scan of the live maps",
			s1.grainfree, s0.grainfree);
		storeclose(st);
	}
	free(buf);
	devclose(d);
}

/*
 * -R's rebuild scans the replayed maps, so an object committed after
 * the last checkpoint keeps its grains.  The checkpoint -R then
 * writes publishes a ckseq past those records, so a rebuild that had
 * read the checkpointed index would leave their grains free with
 * nothing left in the log to correct it.
 *
 * Mutation: rebuildbitmap scans the on-disk index region instead of
 * the replayed one (mut rebuild-ckpt-index).
 */
static void
tRlog(void)
{
	Dev *d;
	Super s;
	Store *st;
	Storestat s0, s1;
	uchar *buf;
	int bad;

	d = newstore(&s);
	if((st = opens(d, "rebuild over the log")) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(3*Vblk, 17);
	mk(st, "old", 1);
	wr(st, "old", buf, 3*Vblk, 0, 2);
	ckpt(st);
	mk(st, "new", 1);
	wr(st, "new", buf, 2*Vblk, 0, 2);	/* committed, not checkpointed */
	storestat(st, &s0);
	storeclose(st);

	checks++;
	if((bad = scrub(d, 0, 1)) != 0)
		fail("-R on a sound store reported %d problem(s)", bad);
	checks++;
	if((bad = scrub(d, 0, 0)) != 0)
		fail("after -R the bitmap disagrees with the live maps: %d "
			"problem(s)", bad);
	if((st = opens(d, "after -R over the log")) != nil){
		storestat(st, &s1);
		eqv("the rebuild counted the un-checkpointed records",
			s1.grainfree, s0.grainfree);
		storeclose(st);
	}
	free(buf);
	devclose(d);
}

static char ropath[64];

/*
 * The two refusals.  -o dumps one object and -R rebuilds from every
 * live map, so the pair is a contradiction rather than a narrowing;
 * and -R on a device opened read-only must say so rather than replay
 * the whole log and fall over on the first write.  -v on the same
 * read-only image must work, which is what §12's read-only open is
 * for.
 *
 * Mutation: both guards dropped (mut rebuild-norefuse).
 */
static void
trefuse(void)
{
	Dev *d;
	Super s;
	Fmtcfg c;
	Ckcfg k;
	Store *st;
	uchar *buf;

	d = newstore(&s);
	if((st = opens(d, "refusals")) != nil){
		buf = mkbuf(Vblk, 23);
		mk(st, "one", 1);
		wr(st, "one", buf, Vblk, 0, 2);
		ckpt(st);
		storeclose(st);
		free(buf);
	}
	memset(&k, 0, sizeof k);
	k.rebuild = 1;
	k.oid = "one";
	checks++;
	if(runck(d, &k) == 0)
		fail("-R with -o was not refused");
	said("-R with -o", "work over the whole store");
	devclose(d);

	remove(ropath);
	if((d = fileopen(ropath, Secsz, (vlong)Nsec*Secsz, 0)) == nil)
		sysfatal("fileopen: %r");
	smallcfg(&c);
	if(geometry(&s, &c, d->size) < 0 || fmtstore(d, &s) < 0)
		sysfatal("format: %r");
	devclose(d);

	if((d = fileopen(ropath, Secsz, 0, Drdonly)) == nil)
		sysfatal("reopen read-only: %r");
	memset(&k, 0, sizeof k);
	k.rebuild = 1;
	checks++;
	if(runck(d, &k) == 0)
		fail("-R on a read-only device reported nothing");
	said("-R read-only", "is open read-only");
	/*
	 * ... and -v opens the same image: a device that cannot write
	 * has no durability to assert and no flush channel to want, so
	 * §5 step 1's refusal does not apply to it.
	 */
	memset(&k, 0, sizeof k);
	k.verify = 1;
	checks++;
	if(runck(d, &k) != 0)
		fail("-v on a read-only image reported problems");
	said("-v on a read-only image", "0 objects verified");
	devclose(d);
	remove(ropath);
}

static char imgpath[64];

/*
 * The success paths below remove their own files; a sysfatal or a
 * mutant that dies inside ckstore does not, so the removals are also
 * registered here and run however this program exits.
 */
static void
cleanup(void)
{
	remove(ckpath);
	remove(imgpath);
	remove(badpath);
	remove(ropath);
}

void
main(int, char**)
{
	Dev *d;
	char *path;

	snprint(ckpath, sizeof ckpath, "/tmp/shoalcktest.%d.out", getpid());
	snprint(badpath, sizeof badpath, "/tmp/shoalckbadlog.%d.img", getpid());
	snprint(ropath, sizeof ropath, "/tmp/shoalckro.%d.img", getpid());
	snprint(imgpath, sizeof imgpath, "/tmp/shoalfmtck.%d.img", getpid());
	atexit(cleanup);
	if((null = open("/dev/null", OWRITE)) < 0)
		null = 2;

	if((d = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	tround(d, "simulated disk");
	devclose(d);

	path = imgpath;
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
	tbadlog();
	tverify();
	tvreplay();
	tvdirty();
	trebuildoff();
	tRlog();
	trefuse();
	if(fails > 0)
		exits("failed");
	print("fmtcktest: %d checks ok\n", checks);
	exits(nil);
}
