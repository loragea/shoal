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

/* the checker earns its keep: it must find what a fault leaves behind */
static void
tdamage(void)
{
	Dev *d;
	Super s;
	Fmtcfg c;
	uchar *p;

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

	tdamage();
	treformat();
	tcutream();
	if(fails > 0)
		exits("failed");
	print("fmtcktest: %d checks ok\n", checks);
	exits(nil);
}
