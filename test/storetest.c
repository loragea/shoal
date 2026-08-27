#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"
#include "t1.h"

/*
 * T1 for the store engine's start-up half: docs/design/store.md §5's
 * ordered recovery, §2.7's apply function and its idempotence, §2.8's
 * checkpoint, §2.5's replay-coverage rule, and §2.2's publisher.
 *
 * The write path is exercised only as far as these need it; the crash
 * schedules are committest, and the object-level invariants are
 * objtest.
 */

enum
{
	Blk	= 4096,			/* the small geometry's blksz */
};

/* a shadow copy of what an object should hold */
typedef struct Shadow Shadow;
struct Shadow
{
	uchar	*p;
	uvlong	len;
};

static void
shwrite(Shadow *sh, void *a, long n, uvlong off)
{
	uchar *p;

	if(off + n > sh->len){
		if((p = realloc(sh->p, off + n)) == nil)
			sysfatal("realloc: %r");
		memset(p + sh->len, 0, off + n - sh->len);
		sh->p = p;
		sh->len = off + n;
	}
	memmove(sh->p + off, a, n);
}

static void
shtrunc(Shadow *sh, uvlong len)
{
	uchar *p;

	if(len > sh->len){
		if((p = realloc(sh->p, len)) == nil)
			sysfatal("realloc: %r");
		memset(p + sh->len, 0, len - sh->len);
		sh->p = p;
	}
	sh->len = len;
}

/*
 * The object reads back exactly what was written, its stored csum is
 * the csum of that byte image (layer-a §1.4, computed here the way
 * shoalcsum computes it over a file), and verify agrees with both.
 */
static void
checkobj(Store *s, char *name, Shadow *sh, char *what)
{
	Objinfo oi;
	Vfy v;
	uchar oid[Oidmax], csum[Csumlen], *buf;
	long n;

	oidof(oid, name);
	if(objstat(s, oid, strlen(name), &oi) < 0){
		fail("%s: objstat %s: %r", what, name);
		return;
	}
	eqv("len", oi.len, sh->len);
	if(oi.len != sh->len)
		return;
	objcsum(sh->p, sh->len, Blk, csum);
	checks++;
	if(memcmp(csum, oi.csum, Csumlen) != 0)
		fail("%s: %s: stored csum is not the csum of its content",
			what, name);
	if(sh->len > 0){
		if((buf = malloc(sh->len)) == nil)
			sysfatal("malloc: %r");
		n = objread(s, oid, strlen(name), buf, sh->len, 0);
		checks++;
		if(n != (long)sh->len)
			fail("%s: %s: read %ld of %llud: %r", what, name, n,
				sh->len);
		else if(memcmp(buf, sh->p, sh->len) != 0)
			fail("%s: %s: content differs", what, name);
		free(buf);
	}
	if(objverify(s, oid, strlen(name), &v) < 0){
		fail("%s: objverify %s: %r", what, name);
		return;
	}
	checks++;
	if(v.arraybad || v.nbad != 0)
		fail("%s: %s: verify: arraybad=%d nbad=%lud", what, name,
			v.arraybad, v.nbad);
	vfyfree(&v);
}

static void
mkobj(Store *s, char *name, uvlong ver)
{
	uchar oid[Oidmax];

	oidof(oid, name);
	if(objcreate(s, oid, strlen(name), ver, 1, nil) < 0)
		fail("objcreate %s: %r", name);
}

static void
wr(Store *s, char *name, Shadow *sh, void *a, long n, uvlong off, uvlong ver)
{
	uchar oid[Oidmax];

	oidof(oid, name);
	if(objwrite(s, oid, strlen(name), a, n, off, ver, 1, nil, 0) < 0){
		fail("objwrite %s %ld at %llud: %r", name, n, off);
		return;
	}
	shwrite(sh, a, n, off);
}

/*
 * A store that was closed cleanly and one that was not must come up
 * with the same state: the log is the durable authority for
 * everything since the last checkpoint (§2.8), so replay is what
 * makes the second true.
 */
static void
tbasic(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Shadow sh;
	uchar *buf;

	memset(&sh, 0, sizeof sh);
	d = newdisk();
	if((s = mustopen(d, "basic")) == nil)
		return;
	storestat(s, &st);
	eqv("a fresh store has no live objects", st.nlive, 0);
	eqv("a fresh store condemns nothing", st.nlost, 0);
	eqv("a fresh store replays nothing", st.nreplay, 0);
	eqv("a fresh store needs no bitmap rebuild", st.bmaprebuild, 0);
	istrue("a fresh store has grains free", st.grainfree > 100);

	mkobj(s, "alpha", 1);
	buf = mkbuf(100, 1);
	wr(s, "alpha", &sh, buf, 100, 0, 2);
	checkobj(s, "alpha", &sh, "live");
	free(buf);

	/* a crash with no checkpoint at all: replay rebuilds everything */
	storeclose(s);
	if((s = mustopen(d, "replayed")) == nil)
		return;
	storestat(s, &st);
	istrue("replay applied the create and the write", st.nreplay >= 2);
	eqv("replay condemned nothing", st.nlost, 0);
	checkobj(s, "alpha", &sh, "replayed");

	/*
	 * §5 step 7 calls replay idempotent because every clause of the
	 * apply sets absolute values.  Replaying the same log twice must
	 * therefore leave the same store.
	 */
	storeclose(s);
	if((s = mustopen(d, "replayed twice")) == nil)
		return;
	checkobj(s, "alpha", &sh, "replayed twice");

	/* a checkpoint materialises it, and then replay has nothing to do */
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storeclose(s);
	if((s = mustopen(d, "quiescent")) == nil)
		return;
	storestat(s, &st);
	eqv("a quiescent restart replays no record at all", st.nreplay, 0);
	istrue("the store still starts", 1);
	checkobj(s, "alpha", &sh, "quiescent");
	storeclose(s);
	devclose(d);
	free(sh.p);
}

/*
 * §4 and R13: bytes never written below len read as zero and hash as
 * the zero bytes they read as, and a sparse extend across many blocks
 * stays a metadata-only commit whose csum still verifies.
 */
static void
tholes(void)
{
	Dev *d;
	Store *s;
	Shadow sh;
	uchar *buf, oid[Oidmax];

	memset(&sh, 0, sizeof sh);
	d = newdisk();
	if((s = mustopen(d, "holes")) == nil)
		return;
	mkobj(s, "sparse", 1);
	buf = mkbuf(16, 3);
	/* one byte range far out: every block below it is a hole */
	wr(s, "sparse", &sh, buf, 16, 5*Blk + 7, 2);
	checkobj(s, "sparse", &sh, "sparse extend");

	/* and after a crash and replay, where clause 4 must agree */
	storeclose(s);
	if((s = mustopen(d, "sparse replayed")) == nil)
		return;
	checkobj(s, "sparse", &sh, "sparse extend replayed");

	/* truncate within a block re-hashes it over fewer bytes */
	oidof(oid, "sparse");
	if(objtrunc(s, oid, 6, 5*Blk + 3, 3, 1) < 0)
		fail("objtrunc: %r");
	else
		shtrunc(&sh, 5*Blk + 3);
	checkobj(s, "sparse", &sh, "truncate within a block");
	storeclose(s);
	if((s = mustopen(d, "truncate replayed")) == nil)
		return;
	checkobj(s, "sparse", &sh, "truncate replayed");
	storeclose(s);
	devclose(d);
	free(sh.p);
	free(buf);
}

/*
 * §2.5's replay-coverage rule, in all three cases it exists to tell
 * apart.  The benign case is a crash during a checkpoint: pages
 * stamped ahead of the superblock are the ordinary state there, and
 * refusing to start would refuse the commonest checkpoint crash there
 * is.  The malign case is a checkpoint whose superblock was later
 * lost to a media fault over a log that has been reclaimed.  The
 * quiescent case is a store that was idle when it stopped, where
 * replay applies no record at all and the superblock's term carries
 * the test.
 */
static void
tcoverage(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Shadow sh;
	Sbsel sel;
	uchar *buf, *sb, junk[512];
	uvlong ck, oldck;

	memset(&sh, 0, sizeof sh);
	d = newdisk();
	if((s = mustopen(d, "coverage")) == nil)
		return;
	mkobj(s, "cov", 1);
	buf = mkbuf(3*Blk, 5);
	wr(s, "cov", &sh, buf, 3*Blk, 0, 2);

	/*
	 * Benign: the checkpoint's pages and its flush land, its
	 * superblock write does not.  The pages are then stamped ahead
	 * of the superblock, which is the ordinary state after a crash
	 * between §2.8's step 1 and step 3 — a generation comparison
	 * would refuse the commonest checkpoint crash there is.
	 */
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	simcrashdead(d, 1);
	simcrashmode(d, Scdrop);
	simarm(d, "super", sel.victim);
	storecheckpoint(s);		/* the crash stops it: expected */
	storeclose(s);
	simrevive(d);
	if((s = mustopen(d, "mid-checkpoint")) == nil){
		fail("a bitmap page ahead of the superblock must not stop the "
			"store starting");
		devclose(d);
		return;
	}
	storestat(s, &st);
	istrue("replay reached Pmax after a mid-checkpoint crash",
		st.pmax > st.ckseq);
	checkobj(s, "cov", &sh, "mid-checkpoint");

	/* a complete checkpoint, then another with a commit between them */
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storestat(s, &st);
	oldck = st.ckseq;
	wr(s, "cov", &sh, buf, Blk, 3*Blk, 3);
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storestat(s, &st);
	ck = st.ckseq;
	istrue("the second checkpoint advanced the mark", ck > oldck);
	storeclose(s);

	/* quiescent: a complete checkpoint and nothing after it */
	if((s = mustopen(d, "quiescent")) == nil)
		return;
	storestat(s, &st);
	eqv("a quiescent restart applies no record", st.nreplay, 0);
	eqv("the checkpoint mark survived", st.ckseq, ck);
	checkobj(s, "cov", &sh, "quiescent");
	storeclose(s);

	/*
	 * Malign: the newer superblock copy is lost to a media fault, so
	 * start falls back to the older one — whose log space the
	 * completed checkpoint reclaimed and something else has since
	 * overwritten.  Replay stops at the first overwritten sector,
	 * short of Pmax, and so does the superblock's own term, so the
	 * store MUST NOT start: replaying from a stale mark over
	 * already-materialised state is how grains that live objects
	 * reference get handed out again.
	 */
	if(superselect(d, &sel) < 0){
		fail("superselect: %r");
		devclose(d);
		return;
	}
	memset(junk, 0xa5, sizeof junk);
	simpoke(d, (vlong)sel.sb[sel.victim].cklogoff*sel.sb[sel.victim].secsz,
		junk, sel.sb[sel.victim].secsz);
	if((sb = mallocz(sel.sb[sel.start].secsz, 1)) == nil)
		sysfatal("mallocz: %r");
	simpoke(d, sel.start == 0 ? 0 : super1off(d), sb,
		sel.sb[sel.start].secsz);
	free(sb);
	checks++;
	if((s = openstore(d)) != nil){
		fail("a store whose log no longer covers Pmax must refuse "
			"to start");
		storeclose(s);
	}
	devclose(d);
	free(sh.p);
	free(buf);
}

/*
 * §2.5's automatic rebuild.  The bitmap is not authoritative — the
 * truth is the set of grains committed state references — so a page
 * that fails its checksum is a slow start and not a refusal, and the
 * rebuilt free map must equal a full scan.
 */
static void
trebuild(void)
{
	Dev *d;
	Store *s;
	Storestat st, st2;
	Shadow sh;
	Sbsel sel;
	uchar *buf, junk[64];

	memset(&sh, 0, sizeof sh);
	d = newdisk();
	if((s = mustopen(d, "rebuild")) == nil)
		return;
	mkobj(s, "one", 1);
	mkobj(s, "many", 1);
	buf = mkbuf(3*Blk, 7);
	wr(s, "many", &sh, buf, 3*Blk, 0, 2);
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storestat(s, &st);
	storeclose(s);

	memset(junk, 0xa5, sizeof junk);
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	simpoke(d, (vlong)sel.sb[sel.start].bmapoff*sel.sb[sel.start].secsz + 64,
		junk, sizeof junk);
	if((s = mustopen(d, "after a corrupt bitmap page")) == nil){
		devclose(d);
		return;
	}
	storestat(s, &st2);
	eqv("a corrupt bitmap page is rebuilt, not refused", st2.bmaprebuild, 1);
	eqv("the rebuilt free map equals the committed one", st2.grainfree,
		st.grainfree);
	checkobj(s, "many", &sh, "after rebuild");
	storeclose(s);
	devclose(d);
	free(sh.p);
	free(buf);
}

/*
 * §2.2's publisher and its durability orderings.  Every superblock
 * write carries all fields and is built from live state, so no field
 * regresses whatever the interleaving; and the qidnext high-water is
 * durable before any path in its batch is issued, so no path is ever
 * re-issued across a restart.
 */
static void
tpublish(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Objinfo oi;
	uchar id[16], oid[Oidmax];
	uvlong first, last;
	char name[32];
	int i;

	d = newdisk();
	if((s = mustopen(d, "publish")) == nil)
		return;
	memset(id, 0x5a, sizeof id);
	if(epochadopt(s, 7) < 0)
		fail("epochadopt: %r");
	if(monidpin(s, id) < 0)
		fail("monidpin: %r");
	storestat(s, &st);
	eqv("epochhigh is durable before the instance acts under it",
		st.epochhigh, 7);
	eqv("monid is pinned", st.monidset, 1);

	mkobj(s, "q0", 1);
	oidof(oid, "q0");
	if(objstat(s, oid, 2, &oi) < 0)
		fail("objstat: %r");
	first = oi.qidpath;
	for(i = 1; i < 12; i++){
		snprint(name, sizeof name, "q%d", i);
		mkobj(s, name, 1);
	}
	snprint(name, sizeof name, "q%d", 11);
	oidof(oid, name);
	if(objstat(s, oid, strlen(name), &oi) < 0)
		fail("objstat: %r");
	last = oi.qidpath;
	istrue("qid.path values are handed out in order", last > first);
	storestat(s, &st);
	istrue("the recorded high-water is at least any path handed out",
		st.qidnext > last);

	/* a checkpoint publish must not regress what the others advanced */
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storestat(s, &st);
	eqv("a checkpoint publish keeps epochhigh", st.epochhigh, 7);
	eqv("a checkpoint publish keeps monid", st.monidset, 1);
	istrue("a checkpoint publish keeps qidnext", st.qidnext > last);

	storeclose(s);
	if((s = mustopen(d, "after restart")) == nil)
		return;
	storestat(s, &st);
	eqv("epochhigh survived the restart", st.epochhigh, 7);
	eqv("monid survived the restart", st.monidset, 1);
	mkobj(s, "qz", 1);
	oidof(oid, "qz");
	if(objstat(s, oid, 2, &oi) < 0)
		fail("objstat: %r");
	istrue("no qid.path is re-issued across a restart", oi.qidpath > last);
	storeclose(s);
	devclose(d);
}

/*
 * §5 step 10, and the order it depends on: a store that judged the
 * index before replaying would put a live object into /lost after an
 * ordinary crash, because a crashed checkpoint damages exactly the
 * structures replay repairs (§3.4).
 */
static void
tcondemn(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Shadow sh;
	Sbsel sel;
	Super sup;
	uchar *buf, junk[8];
	uvlong off;

	memset(&sh, 0, sizeof sh);
	d = newdisk();
	if((s = mustopen(d, "condemn")) == nil)
		return;
	mkobj(s, "keep", 1);
	buf = mkbuf(64, 9);
	wr(s, "keep", &sh, buf, 64, 0, 2);
	storeclose(s);

	/*
	 * Damage the index entry the log still describes.  Replay must
	 * restore it, so nothing is condemned.
	 */
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	sup = sel.sb[sel.start];
	memset(junk, 0xff, sizeof junk);
	off = idxentoff(&sup, 0);
	simpoke(d, off, junk, sizeof junk);
	if((s = mustopen(d, "damaged index, log intact")) == nil){
		devclose(d);
		return;
	}
	storestat(s, &st);
	eqv("replay repairs what a crashed checkpoint damaged", st.nlost, 0);
	checkobj(s, "keep", &sh, "after repair");

	/* now damage a slot the log does not describe: it is condemned */
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storeclose(s);
	simpoke(d, off, junk, sizeof junk);
	if((s = mustopen(d, "damaged index, log settled")) == nil){
		devclose(d);
		return;
	}
	storestat(s, &st);
	eqv("genuine media damage is condemned", st.nlost, 1);
	eqv("the condemned slot is named", storelost(s, 0), 0);
	storeclose(s);
	devclose(d);
	free(sh.p);
	free(buf);
}

/*
 * §3.2's flush channel.  If it cannot be opened the store MUST NOT
 * start, unless the operator passes -w, which asserts that the unit
 * is write-through or its write cache disabled.  -w is a claim, not
 * an observation, so it is reported in those words and never as
 * flush=raw: there is no silent downgrade, because the property being
 * downgraded is normative in layer-a and unverified on the platform.
 */
static void
tflushchan(void)
{
	Dev *d;
	Store *s;
	Storecfg c;
	Storestat st;

	d = newdisk();
	d->flushmode = Fnone;		/* as a device with no raw channel */
	tcfg(&c);
	checks++;
	if((s = storeopen(d, &c)) != nil){
		fail("a store with no flush channel must not start without -w");
		storeclose(s);
	}
	c.noflush = 1;
	if((s = storeopen(d, &c)) == nil)
		fail("-w must let the store start: %r");
	else{
		storestat(s, &st);
		eqv("-w is reported as the operator's assertion",
			st.flushmode, Fasserted);
		mkobj(s, "w", 1);
		storeclose(s);
	}
	d->flushmode = Fraw;
	if((s = mustopen(d, "with the channel back")) != nil){
		storestat(s, &st);
		eqv("with the channel it reports raw", st.flushmode, Fraw);
		storeclose(s);
	}
	devclose(d);
}

void
main(int argc, char **argv)
{
	USED(argc); USED(argv);
	tbasic();
	tholes();
	tcoverage();
	trebuild();
	tpublish();
	tcondemn();
	tflushchan();
	if(fails > 0){
		fprint(2, "storetest: %d of %d checks failed\n", fails, checks);
		exits("failed");
	}
	print("storetest: %d checks ok\n", checks);
	exits(nil);
}
