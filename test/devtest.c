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
 *	  makes the last two not media errors, and are told apart
 *	  through whatever a caller wrapped them in;
 *	- several faults are armed at once and one can be aimed at a
 *	  named sector;
 *	- a crash leaves each dirty sector holding either its durable
 *	  bytes or its cached ones, as the test chooses, which is what
 *	  lets a torn commit header survive one;
 *	- the flush and write sequence is recorded in issue order, so a
 *	  test can assert the order and not only the outcome;
 *	- procs sharing one Dev do not lose each other's operations.
 */

enum
{
	Secsz	= 512,
	Nsec	= 512,
	Seed	= 0x5ee1,

	Nproc	= 8,		/* procs sharing one Dev */
	Npwrite	= 400,		/* writes each of them issues */
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
			"nowhere" : "whole");

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
	n = Blkszstore;			/* §2.1's default blksz */
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

/*
 * A crash is the end of a run, so a test may ask for it to stop the
 * device: every read, write and flush then fails until the machine
 * comes back.  Without it the writes a §13 schedule places after its
 * crash point would still land.
 */
static void
tdead(void)
{
	Dev *d;
	uchar w[Secsz], r[Secsz];

	d = sim();
	pat(w, Secsz, 23);
	simcrashdead(d, 1);
	simarm(d, "commit", 0);
	if(devwrite(d, w, Secsz, 5*Secsz) < 0)
		fail("write: %r");
	devpoint(d, "commit", 0);
	checks++;
	if(devwrite(d, w, Secsz, 6*Secsz) >= 0)
		fail("a write after the crash still landed");
	checks++;
	if(devread(d, r, Secsz, 0) >= 0)
		fail("a read after the crash still worked");
	checks++;
	if(devflush(d) >= 0)
		fail("a flush after the crash still worked");
	simrevive(d);
	checks++;
	if(devread(d, r, Secsz, 0) < 0)
		fail("the machine did not come back: %r");
	devclose(d);
}

/*
 * §13 says a test that wants a file drives it through its own file
 * under /tmp.  The names carry this program's pid because /tmp is
 * shared: two runs of devtest at once — one mk test beside another,
 * or a mutant being timed against the tree — would otherwise create,
 * write, read back and remove the *same* files, and what that looks
 * like is not a collision but one run's create truncating the image
 * the other is reading, or its remove taking the file out from under
 * an open fd.  The unit directory needs the pid for the same reason.
 */
static char imgpath[64], ropath[64];
static char sddir[64], sdnm[3][64];

/* the file-backed device: §12's tools work on an image, so T1 can too */
static void
tfile(void)
{
	Dev *d;
	uchar w[Secsz], r[Secsz];
	char *path;

	path = imgpath;
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

	/*
	 * Growing an image that is already there: shoalfmt -z over an
	 * existing file is exactly this path, and the bytes below the
	 * old end must survive it.
	 */
	if((d = fileopen(path, Secsz, 64*Secsz, 0)) == nil){
		fail("grow: %r");
		remove(path);
		return;
	}
	eqv("a grown image is the size asked for", d->size, 64*Secsz);
	if(devread(d, r, Secsz, 8*Secsz) < 0)
		fail("read: %r");
	checks++;
	if(memcmp(w, r, Secsz) != 0)
		fail("growing an image lost what was below the old end");
	devclose(d);
	remove(path);
}

/*
 * §0: the two error strings that are not media errors are recognised
 * inside whatever the caller wrapped them in.  devsd reports which of
 * the flush's three system calls failed, so this is the form deverr
 * actually sees from a real device.
 */
static void
twrapped(void)
{
	werrstr("/dev/sdF0/shoal: flush: interrupted");
	eqv("a wrapped interrupt is not a media error", deverr(), Deintr);
	werrstr("/dev/sdF0/shoal: flush status: media or partition has changed");
	eqv("a wrapped Echange", deverr(), Dechange);
	werrstr("/dev/sdF0/shoal: flush: i/o error");
	eqv("a wrapped media error is one", deverr(), Deio);
	werrstr("");
	eqv("no error string is no error class", deverr(), Denone);

	/*
	 * §0: only the last `: '-separated segment is classified,
	 * because everything before it is the operator's text.
	 * Partition names are free text, so a partition named
	 * `interrupted' must not turn a media error into a flushed
	 * request — the direction that discards damage silently.
	 */
	werrstr("/dev/sdF0/interrupted: read 512 at 0: i/o error");
	eqv("a media error on a partition named interrupted",
		deverr(), Deio);
	werrstr("/dev/sdF0/interrupted: flush: interrupted");
	eqv("a real interrupt on that partition is still one",
		deverr(), Deintr);
	werrstr("interrupted");
	eqv("an unwrapped interrupt", deverr(), Deintr);
}

/*
 * §0: the store MUST NOT issue a single pwrite larger than Wunit, and
 * the device layer is where that holds rather than at each call site
 * — but Wunit is the device's and blksz is the format's (§2.1), so a
 * longer write is split into unit pieces rather than refused.  A read
 * has the opposite rule and is not split.
 */
static void
tlimits(void)
{
	Dev *d;
	Simop *t;
	uchar *big, *back;
	long i, n, nw;

	d = sim();
	eqv("the write unit", d->wunit, Wunitdflt);
	n = 3*Wunitdflt;
	if((big = malloc(n)) == nil || (back = malloc(n)) == nil)
		sysfatal("malloc: %r");
	pat(big, n, 9);
	checks++;
	if(devwrite(d, big, Wunitdflt, 0) < 0)
		fail("a write of exactly Wunit was refused: %r");

	/* a write of three units is three requests, and all of it lands */
	simtracereset(d);
	checks++;
	if(devwrite(d, big, n, 0) < 0)
		fail("a write larger than Wunit was refused: %r");
	nw = 0;
	for(i = 0; i < simtrace(d, &t); i++)
		if(t[i].op == Sopwrite){
			nw++;
			checks++;
			if(t[i].n > Wunitdflt)
				fail("a single request of %ld bytes exceeds "
					"the %lud-byte write unit", t[i].n,
					d->wunit);
		}
	eqv("requests a three-unit write took", nw, 3);
	if(devread(d, back, n, 0) < 0)
		fail("read: %r");
	checks++;
	if(memcmp(big, back, n) != 0)
		fail("a split write lost bytes");

	checks++;
	if(devread(d, back, n, 0) < 0)
		fail("a read larger than Wunit was refused: %r");

	/* devzero's unit is the caller's grain, not the device's unit */
	simtracereset(d);
	checks++;
	if(devzero(d, 0, 4*(vlong)Wunitdflt, 2*Wunitdflt) < 0)
		fail("devzero with a unit larger than Wunit was refused: %r");
	nw = 0;
	for(i = 0; i < simtrace(d, &t); i++)
		if(t[i].op == Sopwrite){
			nw++;
			checks++;
			if(t[i].n > Wunitdflt)
				fail("devzero issued a %ld-byte request",
					t[i].n);
		}
	eqv("requests a two-unit devzero of four units took", nw, 4);
	free(big);
	free(back);
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

	path = ropath;
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
 * devzero is what fmtstore zeroes the log with, in pieces of one
 * Wunit, from an offset that need not be a Wunit boundary.  A devzero
 * that stopped after its first piece would leave a store starting on
 * whatever the disk held.
 */
static void
tzero(void)
{
	Dev *d;
	Simop *t;
	uchar buf[Secsz], r[Secsz];
	vlong off, len;
	long i, n, nw;
	int bad;

	d = sim();
	pat(buf, Secsz, 5);
	for(i = 0; i < Nsec; i++)
		if(devwrite(d, buf, Secsz, (vlong)i*Secsz) < 0)
			fail("fill: %r");
	off = Secsz;			/* as fmtstore zeroes the reserved run */
	len = 5*4096 + 2*Secsz;
	simtracereset(d);
	if(devzero(d, off, len, 4096) < 0)
		fail("devzero: %r");
	bad = 0;
	for(i = 1; i*Secsz < off + len; i++){
		if(devread(d, r, Secsz, (vlong)i*Secsz) < 0)
			fail("read: %r");
		for(n = 0; n < Secsz; n++)
			if(r[n] != 0)
				bad++;
	}
	eqv("bytes left unzeroed inside the range", bad, 0);
	if(devread(d, r, Secsz, 0) < 0)
		fail("read: %r");
	checks++;
	if(memcmp(r, buf, Secsz) != 0)
		fail("devzero wrote below the range it was given");
	if(devread(d, r, Secsz, off + len) < 0)
		fail("read: %r");
	checks++;
	if(memcmp(r, buf, Secsz) != 0)
		fail("devzero wrote past the range it was given");
	nw = 0;
	for(i = 0; i < simtrace(d, &t); i++)
		if(t[i].op == Sopwrite)
			nw++;
	eqv("devzero requests", nw, len/4096 + 1);
	devclose(d);
}

/*
 * §13 wants a short count and a tear in one schedule and §8 wants an
 * Eio on one named sector, so faults are armed several at a time and
 * each may be aimed.
 */
static void
tfaults(void)
{
	Dev *d;
	Simop *t;
	uchar w[Secsz], r[Secsz], *big;
	long i, n, nw, got;
	int torn;

	d = sim();
	/* aimed at one sector, it fires there and nowhere else */
	simfaultat(d, Sfeio, 1, 40*Secsz, Secsz);
	checks++;
	if(devread(d, r, Secsz, 41*Secsz) < 0)
		fail("a fault aimed at one sector hit another: %r");
	checks++;
	if(devread(d, r, Secsz, 40*Secsz) == 0)
		fail("a fault aimed at a sector did not fire there");

	/* two of them armed at once, each in its own place */
	simfault(d, Sfnone, 0);
	simfaultat(d, Sfdrop, 1, 8*Secsz, Secsz);
	simfaultat(d, Sfeio, 1, 9*Secsz, Secsz);
	pat(w, Secsz, 5);
	if(devwrite(d, w, Secsz, 8*Secsz) < 0)
		fail("the dropped write reported failure: %r");
	checks++;
	if(devwrite(d, w, Secsz, 9*Secsz) == 0)
		fail("the second armed fault did not fire");
	if(devread(d, r, Secsz, 8*Secsz) < 0)
		fail("read: %r");
	checks++;
	if(memcmp(w, r, Secsz) == 0)
		fail("a dropped write landed");

	/* a read does not consume a write-only fault */
	simfault(d, Sfnone, 0);
	simfault(d, Sftearsec, 1);
	if(devread(d, r, Secsz, 0) < 0)
		fail("read: %r");
	if((big = malloc(8*Secsz)) == nil)
		sysfatal("malloc: %r");
	pat(big, 8*Secsz, 21);
	if(devwrite(d, big, 8*Secsz, 24*Secsz) < 0)
		fail("write: %r");
	torn = 0;
	for(i = 0; i < 8; i++){
		if(devread(d, r, Secsz, (24+i)*Secsz) < 0)
			fail("read: %r");
		if(memcmp(big + i*Secsz, r, Secsz) != 0)
			torn++;
	}
	free(big);
	checks++;
	if(torn == 0)
		fail("a read consumed a write-only fault");

	/* the trace records the count that landed, not the one asked for */
	simfault(d, Sfnone, 0);
	simtracereset(d);
	simfault(d, Sfshort, 1);
	if(devwrite(d, w, Secsz, 48*Secsz) < 0)
		fail("write: %r");
	if((big = malloc(8*Secsz)) == nil)
		sysfatal("malloc: %r");
	pat(big, 8*Secsz, 3);
	simfault(d, Sfnone, 0);
	simfault(d, Sfshort, 1);
	simtracereset(d);
	if(devwrite(d, big, 8*Secsz, 48*Secsz) < 0)
		fail("short write: %r");
	n = simtrace(d, &t);
	got = 0;
	nw = 0;
	for(i = 0; i < n; i++)
		if(t[i].op == Sopwrite){
			got += t[i].n;
			nw++;
		}
	free(big);
	checks++;
	if(nw < 2 || t[0].n >= 8*Secsz)
		fail("the trace recorded the requested count, not the short one");
	eqv("the trace's write counts add up", got, 8*Secsz);

	/* a flush can fail, and a failed one makes nothing durable */
	simfault(d, Sfnone, 0);
	if(devflush(d) < 0)
		fail("flush: %r");
	pat(w, Secsz, 41);
	if(devwrite(d, w, Secsz, 56*Secsz) < 0)
		fail("write: %r");
	simfault(d, Sfeio, 1);
	checks++;
	if(devflush(d) == 0)
		fail("an armed Eio did not reach the flush");
	eqv("a failed flush is classified", deverr(), Deio);
	eqv("a failed flush made nothing durable", simdirty(d), 1);

	/* a crash disarms whatever was armed */
	simfault(d, Sfeio, 0);
	simcrash(d);
	checks++;
	if(devread(d, r, Secsz, 0) < 0)
		fail("a crash left the armed fault armed");
	devclose(d);
}

/*
 * §13: a crash leaves each sector written since the last flush holding
 * either its durable bytes or its cached ones.  Two schedules depend
 * on it: a torn commit header that survives to be read back (§3.2's
 * atomicity point), and a commit header on the platter with the grain
 * it describes still in the cache — the hazard the pre-flush exists to
 * prevent.
 */
static void
tcrash(void)
{
	Dev *d, *e;
	uchar w[Secsz], r[Secsz], g[Secsz];
	Idxent ie;
	uvlong kept;
	int i;

	d = sim();
	memset(&ie, 0, sizeof ie);
	ie.state = Slive;
	ie.oidlen = 4;
	ie.vers = Storevers;
	ie.len = 1;
	pat(ie.oid, 4, 3);
	idxpack(w, &ie);
	memset(w + Idxentsz, 0, Secsz - Idxentsz);
	if(devwrite(d, w, Secsz, 8*Secsz) < 0 || devflush(d) < 0)
		fail("write: %r");
	ie.len = 2;
	idxpack(w, &ie);
	simfault(d, Sftearbyte, 1);
	if(devwrite(d, w, Secsz, 8*Secsz) < 0)
		fail("torn write: %r");
	simcrashkeep(d, 8*Secsz, Secsz);
	simcrash(d);
	simpeek(d, 8*Secsz, r, Secsz);
	checks++;
	if(idxunpack(&ie, r, 16) == 0)
		fail("a crash undid a torn write instead of keeping it");

	/* the header lands, the grain it describes stays in the cache */
	pat(g, Secsz, 7);
	pat(w, Secsz, 9);
	if(devwrite(d, g, Secsz, 20*Secsz) < 0
	|| devwrite(d, w, Secsz, 21*Secsz) < 0)
		fail("write: %r");
	simcrashkeep(d, 21*Secsz, Secsz);
	simcrash(d);
	simpeek(d, 21*Secsz, r, Secsz);
	checks++;
	if(memcmp(r, w, Secsz) != 0)
		fail("the sector named as a survivor did not survive");
	simpeek(d, 20*Secsz, r, Secsz);
	checks++;
	if(memcmp(r, g, Secsz) == 0)
		fail("a sector not named as a survivor survived");

	/* every dirty sector survives, and the mode does not stick */
	simcrashmode(d, Sckeep);
	for(i = 0; i < 16; i++)
		if(devwrite(d, g, Secsz, (100+i)*Secsz) < 0)
			fail("write: %r");
	simcrash(d);
	kept = 0;
	for(i = 0; i < 16; i++){
		simpeek(d, (100+i)*Secsz, r, Secsz);
		if(memcmp(r, g, Secsz) == 0)
			kept++;
	}
	eqv("sectors kept by a keep-all crash", kept, 16);
	if(devwrite(d, w, Secsz, 200*Secsz) < 0)
		fail("write: %r");
	simcrash(d);
	simpeek(d, 200*Secsz, r, Secsz);
	checks++;
	if(memcmp(r, w, Secsz) == 0)
		fail("the crash policy outlived the crash it was set for");

	/*
	 * A subset chosen from the seed: neither all nor none, and the
	 * same subset for the same seed, which is what makes a failing
	 * schedule reproducible.
	 */
	simcrashmode(d, Scsome);
	for(i = 0; i < 32; i++)
		if(devwrite(d, g, Secsz, (300+i)*Secsz) < 0)
			fail("write: %r");
	simcrash(d);
	kept = 0;
	for(i = 0; i < 32; i++){
		simpeek(d, (300+i)*Secsz, r, Secsz);
		if(memcmp(r, g, Secsz) == 0)
			kept++;
	}
	checks++;
	if(kept == 0 || kept == 32)
		fail("a seeded subset crash kept %llud of 32 sectors", kept);

	if((e = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	devclose(d);
	d = sim();
	for(i = 0; i < 32; i++){
		if(devwrite(d, g, Secsz, (300+i)*Secsz) < 0
		|| devwrite(e, g, Secsz, (300+i)*Secsz) < 0)
			fail("write: %r");
	}
	simcrashmode(d, Scsome);
	simcrashmode(e, Scsome);
	simcrash(d);
	simcrash(e);
	kept = 0;
	for(i = 0; i < 32; i++){
		simpeek(d, (300+i)*Secsz, r, Secsz);
		simpeek(e, (300+i)*Secsz, w, Secsz);
		if(memcmp(r, w, Secsz) != 0)
			kept++;
	}
	eqv("sectors two runs of one seed disagree about", kept, 0);
	devclose(e);
	devclose(d);
}

/*
 * §13: the named crash set and the crash mode are set by two calls,
 * and neither order may lose the set.  simcrashkeep selects Scnamed
 * itself, so the reverse order is the one that can go wrong.
 */
static void
tkeeporder(void)
{
	Dev *d;
	uchar w[Secsz], r[Secsz];

	d = sim();
	pat(w, Secsz, 13);
	if(devwrite(d, w, Secsz, 8*Secsz) < 0)
		fail("write: %r");
	simcrashkeep(d, 8*Secsz, Secsz);
	simcrashmode(d, Scnamed);
	simcrash(d);
	simpeek(d, 8*Secsz, r, Secsz);
	checks++;
	if(memcmp(r, w, Secsz) != 0)
		fail("simcrashmode(Scnamed) after simcrashkeep lost the "
			"named set");

	/* and every other mode does discard it */
	if(devwrite(d, w, Secsz, 9*Secsz) < 0)
		fail("write: %r");
	simcrashkeep(d, 9*Secsz, Secsz);
	simcrashmode(d, Scdrop);
	simcrash(d);
	simpeek(d, 9*Secsz, r, Secsz);
	checks++;
	if(memcmp(r, w, Secsz) == 0)
		fail("a crash kept a sector no policy named");
	devclose(d);
}

/*
 * §7 puts the queue procs, the I/O procs, the flusher and the
 * checkpointer on one Dev, all of them proccreate'd and genuinely
 * parallel.  Every operation of every proc must reach the trace, or a
 * crash test asserting an order asserts it of a record with holes in
 * it.
 *
 * simslow is what makes this discriminating rather than lucky.  The
 * sim's two shared counters — the trace index and the dirty count —
 * are each read, then yielded across, then stored; with the lock
 * held that is invisible, and with it removed eight procs lose
 * records on every run.  Without the yields the same mutation ships
 * green most of the time, which is a test that has caught nothing.
 *
 * The trace array is grown past what the procs will need before the
 * first one is forked, so that the trace is not reallocated under
 * them.  A build without the lock then fails on the counters, which
 * is the property being tested, rather than inside the allocator,
 * which would leave a broken proc and a parent in waitpid.
 */
static void
tprocs(void)
{
	Dev *d;
	Simop *t;
	uchar *buf, *seen;
	uchar pre[Secsz];
	long i, n;
	int j;

	if((d = simopen(Secsz, Nproc*Npwrite + 8, Seed)) == nil)
		sysfatal("simopen: %r");
	pat(pre, Secsz, 1);
	for(i = 0; i < 2*Nproc*Npwrite; i++)
		if(devwrite(d, pre, Secsz, (vlong)(Nproc*Npwrite)*Secsz) < 0)
			fail("prefill: %r");
	if(devflush(d) < 0)
		fail("prefill flush: %r");
	eqv("the dirty set is empty before the procs start", simdirty(d), 0);
	simslow(d, 1);
	simtracereset(d);
	for(j = 0; j < Nproc; j++){
		switch(rfork(RFPROC|RFMEM)){
		case -1:
			sysfatal("rfork: %r");
		case 0:
			if((buf = malloc(Secsz)) == nil)
				sysfatal("malloc: %r");
			for(i = 0; i < Npwrite; i++){
				pat(buf, Secsz, j + 1);
				if(devwrite(d, buf, Secsz,
					(vlong)(j*Npwrite + i)*Secsz) < 0)
					sysfatal("write: %r");
			}
			exits(nil);
		}
	}
	for(j = 0; j < Nproc; j++)
		if(waitpid() < 0)
			fail("waitpid: %r");

	n = simtrace(d, &t);
	eqv("operations recorded", n, Nproc*Npwrite);
	eqv("sectors dirtied", simdirty(d), Nproc*Npwrite);
	if((seen = mallocz(Nproc*Npwrite, 1)) == nil)
		sysfatal("malloc: %r");
	for(i = 0; i < n; i++)
		if(t[i].op == Sopwrite && t[i].off/Secsz < Nproc*Npwrite)
			seen[t[i].off/Secsz]++;
	j = 0;
	for(i = 0; i < Nproc*Npwrite; i++)
		if(seen[i] != 1)
			j++;
	eqv("sectors written other than once", j, 0);
	free(seen);
	simslow(d, 0);
	devclose(d);
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

	dir = sddir;
	nm[0] = sdnm[0];
	nm[1] = sdnm[1];
	nm[2] = sdnm[2];
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
	if(sdpart(nm[0]) == 0)
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

/*
 * The success paths above remove their own files; a sysfatal or a
 * mutant that dies inside one does not, so the removals are also
 * registered here and run however this program exits.  A fault is
 * the one exit they do not reach.
 */
static void
cleanup(void)
{
	int i;

	remove(imgpath);
	remove(ropath);
	for(i = 0; i < 3; i++)
		remove(sdnm[i]);
	remove(sddir);
}

void
main(int, char**)
{
	int i;

	snprint(imgpath, sizeof imgpath, "/tmp/shoaldevtest.%d.img", getpid());
	snprint(ropath, sizeof ropath, "/tmp/shoaldevro.%d.img", getpid());
	snprint(sddir, sizeof sddir, "/tmp/shoalsdunit.%d", getpid());
	for(i = 0; i < 3; i++)
		snprint(sdnm[i], sizeof sdnm[i], "%s/%s", sddir,
			i == 0 ? "ctl" : i == 1 ? "raw" : "shoal");
	atexit(cleanup);
	tcache();
	ttear();
	tshort();
	terrors();
	twrapped();
	tlimits();
	trdonly();
	tzero();
	tfaults();
	tcrash();
	ttrace();
	tpoint();
	tdead();
	tkeeporder();
	tprocs();
	tfile();
	tclassify();
	if(fails > 0)
		exits("failed");
	print("devtest: %d checks ok\n", checks);
	exits(nil);
}
