#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"

/*
 * T1: the device interface of docs/design/store.md §0 and the
 * simulated disk of §13.
 *
 * The simulated disk is the thing the rest of T1 stands on, so it is
 * tested for the semantics §13 specifies rather than assumed:
 *
 *	- a written sector is visible at once but durable only after a
 *	  flush, and a crash discards everything written since the last
 *	  one;
 *	- a torn write leaves a sector that fails its own checksum;
 *	- a write may land in an arbitrary subset of its sectors;
 *	- short counts on every read and write, which §0's
 *	  loop-until-complete wrappers must absorb;
 *	- Eio, Echange and `interrupted' are told apart, because §0
 *	  makes the last two not media errors;
 *	- the flush and write sequence is recorded in issue order, so a
 *	  test can assert the order and not only the outcome.
 */

enum
{
	Secsz	= 512,
	Nsec	= 512,
	Seed	= 0x5ee1,
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

static void
eqv(char *what, uvlong got, uvlong want)
{
	checks++;
	if(got != want)
		fail("%s: %llud, want %llud", what, got, want);
}

static void
pat(uchar *p, long n, int k)
{
	long i;

	for(i = 0; i < n; i++)
		p[i] = (uchar)(i*k + 11);
}

static Dev*
sim(void)
{
	Dev *d;

	if((d = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	return d;
}

/* a written sector is durable only after a flush */
static void
tcache(void)
{
	Dev *d;
	uchar w[Secsz], r[Secsz];

	d = sim();
	pat(w, Secsz, 3);
	if(devwrite(d, w, Secsz, 4*Secsz) < 0)
		fail("write: %r");
	eqv("one unflushed sector", simdirty(d), 1);
	if(devread(d, r, Secsz, 4*Secsz) < 0)
		fail("read: %r");
	checks++;
	if(memcmp(w, r, Secsz) != 0)
		fail("an unflushed write was not visible to a read");
	simcrash(d);
	eqv("nothing dirty after a crash", simdirty(d), 0);
	if(devread(d, r, Secsz, 4*Secsz) < 0)
		fail("read: %r");
	checks++;
	if(memcmp(w, r, Secsz) == 0)
		fail("a crash kept a write that was never flushed");

	if(devwrite(d, w, Secsz, 4*Secsz) < 0)
		fail("write: %r");
	if(devflush(d) < 0)
		fail("flush: %r");
	eqv("nothing dirty after a flush", simdirty(d), 0);
	simcrash(d);
	if(devread(d, r, Secsz, 4*Secsz) < 0)
		fail("read: %r");
	checks++;
	if(memcmp(w, r, Secsz) != 0)
		fail("a crash dropped a flushed write");
	devclose(d);
}

/*
 * A torn write leaves a sector that fails its own checksum: that is
 * the property §3.2's commit point rests on, and the reason a record
 * needs no single-sector atomicity.
 */
static void
ttear(void)
{
	Dev *d;
	uchar w[Secsz], r[Secsz], *big;
	Idxent e;
	int i, torn;

	d = sim();
	memset(&e, 0, sizeof e);
	e.state = Slive;
	e.oidlen = 4;
	e.vers = Storevers;
	e.len = 1;
	pat(e.oid, 4, 3);
	idxpack(w, &e);
	memset(w + Idxentsz, 0, Secsz - Idxentsz);
	if(devwrite(d, w, Secsz, 8*Secsz) < 0 || devflush(d) < 0)
		fail("write: %r");

	/* the same entry with a different len, landing torn */
	e.len = 2;
	idxpack(w, &e);
	simfault(d, Sftearbyte, 1);
	if(devwrite(d, w, Secsz, 8*Secsz) < 0)
		fail("torn write: %r");
	if(devflush(d) < 0)
		fail("flush: %r");
	if(devread(d, r, Secsz, 8*Secsz) < 0)
		fail("read: %r");
	checks++;
	if(idxunpack(&e, r, 16) == 0)
		fail("a torn index entry passed its checksum");

	/* a write that lands in an arbitrary subset of its sectors */
	if((big = malloc(8*Secsz)) == nil)
		sysfatal("malloc: %r");
	pat(big, 8*Secsz, 9);
	simfault(d, Sftearsec, 1);
	torn = 0;
	if(devwrite(d, big, 8*Secsz, 16*Secsz) < 0)
		fail("subset write: %r");
	for(i = 0; i < 8; i++){
		if(devread(d, r, Secsz, (16+i)*Secsz) < 0)
			fail("read: %r");
		if(memcmp(big + i*Secsz, r, Secsz) != 0)
			torn++;
	}
	free(big);
	checks++;
	if(torn == 0 || torn == 8)
		fail("a torn multi-sector write landed %s", torn ?
			"whole" : "nowhere");

	/* a dropped write reports success and changes nothing */
	simfault(d, Sfdrop, 1);
	pat(w, Secsz, 13);
	if(devwrite(d, w, Secsz, 32*Secsz) < 0)
		fail("dropped write: %r");
	if(devread(d, r, Secsz, 32*Secsz) < 0)
		fail("read: %r");
	checks++;
	if(memcmp(w, r, Secsz) == 0)
		fail("a dropped write landed");
	devclose(d);
}

/*
 * §0: devsd truncates a request rather than splitting or failing, so
 * a short count is normal and the wrappers loop until the whole range
 * is done.
 */
static void
tshort(void)
{
	Dev *d;
	uchar *w, *r;
	Simop *t;
	long n, i, nw;

	d = sim();
	n = Blkszstore;			/* §0's Wunit: the largest write */
	if((w = malloc(n)) == nil || (r = malloc(n)) == nil)
		sysfatal("malloc: %r");
	pat(w, n, 7);

	simfault(d, Sfshort, 0);		/* sticky: every call is short */
	simtracereset(d);
	if(devwrite(d, w, n, 64*Secsz) < 0)
		fail("short write: %r");
	if(devread(d, r, n, 64*Secsz) < 0)
		fail("short read: %r");
	simfault(d, Sfnone, 0);
	checks++;
	if(memcmp(w, r, n) != 0)
		fail("a short-count write or read lost bytes");

	/* it really did take more than one request each way */
	nw = 0;
	for(i = 0; i < simtrace(d, &t); i++)
		if(t[i].op == Sopwrite)
			nw++;
	checks++;
	if(nw < 2)
		fail("the short-count write completed in one request");

	/* every sector of it is durable after one flush */
	if(devflush(d) < 0)
		fail("flush: %r");
	simcrash(d);
	if(devread(d, r, n, 64*Secsz) < 0)
		fail("read: %r");
	checks++;
	if(memcmp(w, r, n) != 0)
		fail("a flush did not cover a looped short write");
	free(w);
	free(r);
	devclose(d);
}

/* §0: interrupted and Echange are not media errors */
static void
terrors(void)
{
	Dev *d;
	uchar buf[Secsz];

	d = sim();
	simfault(d, Sfeio, 1);
	checks++;
	if(devread(d, buf, Secsz, 0) == 0)
		fail("an Eio read reported success");
	eqv("Eio classified", deverr(), Deio);

	simfault(d, Sfechange, 1);
	checks++;
	if(devread(d, buf, Secsz, 0) == 0)
		fail("an Echange read reported success");
	eqv("Echange classified", deverr(), Dechange);

	simfault(d, Sfintr, 1);
	checks++;
	if(devwrite(d, buf, Secsz, 0) == 0)
		fail("an interrupted write reported success");
	eqv("interrupted classified", deverr(), Deintr);

	/* §0: a write's length and offset are sector multiples */
	checks++;
	if(devwrite(d, buf, 100, 0) == 0)
		fail("an unaligned write was accepted");
	checks++;
	if(devwrite(d, buf, Secsz, 100) == 0)
		fail("a write at an unaligned offset was accepted");
	checks++;
	if(devread(d, buf, Secsz, d->size) == 0)
		fail("a read past the end of the device was accepted");
	devclose(d);
}

/*
 * §13: the flush and write sequence is recorded, so a test can assert
 * the order.  This is the shape T1.13 asserts of a commit: a flush,
 * then the header write, then a flush.
 */
static void
ttrace(void)
{
	Dev *d;
	Simop *t;
	uchar buf[Secsz];
	long n;

	d = sim();
	simtracereset(d);
	memset(buf, 0, sizeof buf);
	devwrite(d, buf, Secsz, 2*Secsz);	/* a body sector */
	devflush(d);
	devwrite(d, buf, Secsz, Secsz);		/* the header: the commit point */
	devflush(d);
	n = simtrace(d, &t);
	eqv("trace length", n, 4);
	if(n == 4){
		eqv("trace 0 is the body write", t[0].op, Sopwrite);
		eqv("trace 0 offset", t[0].off, 2*Secsz);
		eqv("trace 1 is the pre-flush", t[1].op, Sopflush);
		eqv("trace 2 is the header write", t[2].op, Sopwrite);
		eqv("trace 2 offset", t[2].off, Secsz);
		eqv("trace 3 is the post-flush", t[3].op, Sopflush);
	}
	devclose(d);
}

/* a crash at a named point, which is how §13 drives every schedule */
static void
tpoint(void)
{
	Dev *d;
	uchar w[Secsz], r[Secsz];

	d = sim();
	pat(w, Secsz, 17);
	simarm(d, "super", 1);
	if(devwrite(d, w, Secsz, 3*Secsz) < 0)
		fail("write: %r");
	devpoint(d, "super", 0);		/* not the armed number */
	eqv("an unarmed point did not crash", simdirty(d), 1);
	devpoint(d, "super", 1);
	eqv("the armed point crashed", simdirty(d), 0);
	if(devread(d, r, Secsz, 3*Secsz) < 0)
		fail("read: %r");
	checks++;
	if(memcmp(w, r, Secsz) == 0)
		fail("the crash at the armed point kept an unflushed write");
	devclose(d);
}

/* the file-backed device: §12's tools work on an image, so T1 can too */
static void
tfile(void)
{
	Dev *d;
	uchar w[Secsz], r[Secsz];
	char *path;

	path = "/tmp/shoaldevtest.img";
	remove(path);
	if((d = fileopen(path, Secsz, 32*Secsz, 0)) == nil){
		fail("fileopen: %r");
		return;
	}
	eqv("file device size", d->size, 32*Secsz);
	eqv("file device sector", d->secsz, Secsz);
	eqv("a file has no flush channel", d->flushmode, Fnone);
	pat(w, Secsz, 23);
	if(devwrite(d, w, Secsz, 8*Secsz) < 0)
		fail("write: %r");
	if(devflush(d) < 0)
		fail("flush: %r");
	devclose(d);

	if((d = fileopen(path, Secsz, 0, 0)) == nil){
		fail("reopen: %r");
		remove(path);
		return;
	}
	eqv("reopened size", d->size, 32*Secsz);
	if(devread(d, r, Secsz, 8*Secsz) < 0)
		fail("read: %r");
	checks++;
	if(memcmp(w, r, Secsz) != 0)
		fail("a file-backed write did not survive a close and reopen");
	devclose(d);
	remove(path);
}

/*
 * §0: the store MUST NOT issue a single pwrite larger than Wunit, and
 * the device layer is where that is enforced rather than at each call
 * site.  A read has the opposite rule and is not capped.
 */
static void
tlimits(void)
{
	Dev *d;
	uchar *big;

	d = sim();
	eqv("the write unit", d->wunit, Blkszstore);
	if((big = mallocz(2*Blkszstore, 1)) == nil)
		sysfatal("malloc: %r");
	checks++;
	if(devwrite(d, big, Blkszstore, 0) < 0)
		fail("a write of exactly Wunit was refused: %r");
	checks++;
	if(devwrite(d, big, Blkszstore + Secsz, 0) == 0)
		fail("a write larger than Wunit was accepted");
	checks++;
	if(devread(d, big, 2*Blkszstore, 0) < 0)
		fail("a read larger than Wunit was refused: %r");
	checks++;
	if(devzero(d, 0, 4*Blkszstore, 2*Blkszstore) == 0)
		fail("devzero with a unit larger than Wunit was accepted");
	free(big);
	devclose(d);
}

/* §12: shoalck opens read-only, so the write is refused twice over */
static void
trdonly(void)
{
	Dev *d;
	Dir *dir, nd;
	uchar w[Secsz], r[Secsz];
	char *path;

	/* the wrapper refuses on a device that would otherwise take it */
	d = sim();
	pat(w, Secsz, 29);
	d->rdonly = 1;
	checks++;
	if(devwrite(d, w, Secsz, 4*Secsz) == 0)
		fail("a write to a read-only device was accepted");
	d->rdonly = 0;
	devclose(d);

	path = "/tmp/shoaldevro.img";
	remove(path);
	if((d = fileopen(path, Secsz, 32*Secsz, 0)) == nil){
		fail("fileopen: %r");
		return;
	}
	if(devwrite(d, w, Secsz, 4*Secsz) < 0)
		fail("write: %r");
	devclose(d);

	if((d = fileopen(path, Secsz, 0, Drdonly)) == nil){
		fail("read-only fileopen: %r");
		remove(path);
		return;
	}
	eqv("a read-only device says so", d->rdonly, 1);
	if(devread(d, r, Secsz, 4*Secsz) < 0)
		fail("read-only read: %r");
	checks++;
	if(memcmp(w, r, Secsz) != 0)
		fail("a read-only device read the wrong bytes");
	checks++;
	if(devwrite(d, w, Secsz, 8*Secsz) == 0)
		fail("a read-only file device accepted a write");
	devclose(d);

	/*
	 * And the kernel enforces it rather than this code promising it:
	 * a device the caller has no permission to write opens read-only
	 * and no other way.  §12 wants the checker to run against a disk
	 * its user may only read.
	 */
	if((dir = dirstat(path)) == nil){
		fail("dirstat: %r");
		remove(path);
		return;
	}
	nulldir(&nd);
	nd.mode = dir->mode & ~0222;
	free(dir);
	if(dirwstat(path, &nd) < 0){
		fail("dirwstat: %r");
		remove(path);
		return;
	}
	checks++;
	if((d = fileopen(path, Secsz, 0, 0)) != nil){
		fail("a writable open of an unwritable image succeeded");
		devclose(d);
	}
	checks++;
	if((d = fileopen(path, Secsz, 0, Drdonly)) == nil)
		fail("a read-only open of an unwritable image failed: %r");
	else
		devclose(d);
	remove(path);
}

/*
 * §12: what makes a path an sd(3) partition is the unit directory it
 * lies in, not the spelling of the path.  The two files a unit always
 * has are what this asks for, so the test can build one under /tmp.
 */
static void
tclassify(void)
{
	char *dir, *nm[3];
	int i, fd;

	dir = "/tmp/shoalsdunit";
	nm[0] = "/tmp/shoalsdunit/ctl";
	nm[1] = "/tmp/shoalsdunit/raw";
	nm[2] = "/tmp/shoalsdunit/shoal";
	for(i = 0; i < 3; i++)
		remove(nm[i]);
	remove(dir);
	if((fd = create(dir, OREAD, DMDIR|0777)) < 0){
		fail("create %s: %r", dir);
		return;
	}
	close(fd);
	for(i = 0; i < 3; i++){
		if((fd = create(nm[i], OWRITE, 0666)) < 0){
			fail("create %s: %r", nm[i]);
			break;
		}
		if(i == 0)
			fprint(fd, "geometry 4096 512\n");
		close(fd);
	}
	checks++;
	if(!sdpart(nm[2]))
		fail("a partition of a unit directory was taken for a file");
	checks++;
	if(sdpart("/tmp/shoalsdunit/ctl") == 0)
		fail("the rule is the directory a path lies in, not its name");
	remove(nm[0]);
	checks++;
	if(sdpart(nm[2]))
		fail("a directory with no ctl file was taken for an sd unit");
	remove(nm[1]);
	remove(nm[2]);
	remove(dir);

	/* the old rule was the path's spelling, and it is not enough */
	checks++;
	if(sdpart("/dev/null"))
		fail("a /dev path that is not a partition was taken for one");
}

void
main(int, char**)
{
	tcache();
	ttear();
	tshort();
	terrors();
	tlimits();
	trdonly();
	ttrace();
	tpoint();
	tfile();
	tclassify();
	if(fails > 0)
		exits("failed");
	print("devtest: %d checks ok\n", checks);
	exits(nil);
}
