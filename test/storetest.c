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

/*
 * The blksz of the geometry under test.  Layer-a §1.4's checksum
 * block is the grain, so the shadow's csum has to be computed at the
 * size the store was formatted with; every test but tbigblk formats
 * the small geometry.
 */
static ulong csumblk = Blk;

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
	objcsum(sh->p, sh->len, csumblk, csum);
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
	if(objtrunc(s, oid, 6, 5*Blk + 3, 3, 1, nil, 0) < 0)
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

	/*
	 * The damage above lands in the page's payload, which leaves the
	 * ckseq field at offset 32 holding its own correct value — so it
	 * says nothing about where Pmax comes from.  §2.5 takes Pmax over
	 * the pages that pass their own checksum and nothing else: a page
	 * that failed one has an arbitrary ckseq, and believing it makes
	 * the coverage rule refuse to start a store whose log covers
	 * everything it must.  Damaging the field itself is what tells
	 * the two apart.
	 */
	memset(junk, 0xa5, sizeof junk);
	simpoke(d, (vlong)sel.sb[sel.start].bmapoff*sel.sb[sel.start].secsz + 32,
		junk, 8);
	if((s = openstore(d)) == nil){
		fail("a torn bitmap page's own ckseq must not stop the store "
			"starting: %r");
		devclose(d);
		free(sh.p);
		free(buf);
		return;
	}
	storestat(s, &st2);
	istrue("a page that failed its checksum contributes no ckseq",
		st2.pmax <= st2.ckseq);
	eqv("and it is rebuilt like any other", st2.bmaprebuild, 1);
	eqv("the rebuilt free map still equals the committed one",
		st2.grainfree, st.grainfree);
	checkobj(s, "many", &sh, "after a torn page ckseq");
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
 * §2.8's mark is publishable only once the flush has returned, and a
 * publish that fails leaves it where it was — the same rule the other
 * three publishers follow.  /status reads the publishable image, so a
 * mark left advanced by a failed publish is a `ckseq` this store would
 * report while the disk still carries the older one.
 *
 * §2.2 clause 3 is the deterministic way to make the publish fail:
 * with neither copy valid the publisher MUST NOT write.  The pages
 * and the flush before it all succeed, so what is under test is the
 * last step alone.
 *
 * Mutation: drop the roll-back, and /status reports the new mark over
 * a superblock that never took it.
 */
static void
tckmark(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Shadow sh;
	uchar *buf, junk[64];
	uvlong ckseq, cklogoff;

	memset(&sh, 0, sizeof sh);
	d = newdisk();
	if((s = mustopen(d, "a checkpoint that cannot publish")) == nil)
		return;
	buf = mkbuf(Blk, 91);
	mkobj(s, "cm", 1);
	wr(s, "cm", &sh, buf, Blk, 0, 2);
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storestat(s, &st);
	ckseq = st.ckseq;
	cklogoff = st.cklogoff;
	istrue("the first checkpoint published a mark", ckseq > 0);

	/* more records, so the next mark would differ */
	wr(s, "cm", &sh, buf, 64, 0, 3);
	wr(s, "cm", &sh, buf, 64, 128, 4);
	memset(junk, 0x5a, sizeof junk);
	simpoke(d, 0, junk, sizeof junk);
	simpoke(d, super1off(d), junk, sizeof junk);
	checks++;
	if(storecheckpoint(s) >= 0)
		fail("a checkpoint published over two invalid superblocks");
	storestat(s, &st);
	eqv("a failed publish leaves ckseq where it was", st.ckseq, ckseq);
	eqv("and cklogoff with it", st.cklogoff, cklogoff);
	storeclose(s);
	devclose(d);
	free(buf);
	free(sh.p);
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
 * §5 step 9 and step 10 at run time.  An extent-map entry that fails
 * its csum128 and that replay did not touch is media damage the log
 * does not cover: the slot goes to /lost with kind=corrupt and is not
 * served.  Step 9 reads only the entries replay touched, so nothing
 * is condemned at start — it is the first read of the object that
 * finds it, and serving that read from the damaged bytes is exactly
 * the silent corruption the step exists to prevent.
 */
static void
tbadmap(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Shadow sh;
	Objinfo oi, oi2;
	Vfy vfy;
	Sbsel sel;
	Super sup;
	Stage *g;
	uchar *buf, *other, rd[64], junk[8], oid[Oidmax];
	ulong slot, gf, sf;
	uvlong nl;

	memset(&sh, 0, sizeof sh);
	d = newdisk();
	if((s = mustopen(d, "a damaged extent map")) == nil)
		return;
	mkobj(s, "wide", 1);
	buf = mkbuf(3*Blk, 21);
	wr(s, "wide", &sh, buf, 3*Blk, 0, 2);
	oidof(oid, "wide");
	if(objstat(s, oid, 4, &oi) < 0)
		fail("objstat wide: %r");
	slot = oi.emapslot;
	istrue("a three-block object has an extent-map slot", slot != 0);
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storeclose(s);

	/*
	 * Damage a digest rather than a grain number, so that a store
	 * that served the entry would answer with real bytes: what is
	 * being tested is that it does not serve it at all.
	 */
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	sup = sel.sb[sel.start];
	memset(junk, 0xa5, sizeof junk);
	simpoke(d, emapentoff(&sup, slot) + sup.emapsz - sizeof junk, junk,
		sizeof junk);

	if((s = mustopen(d, "a damaged extent map, replayed")) == nil){
		devclose(d);
		free(buf);
		free(sh.p);
		return;
	}
	storestat(s, &st);
	eqv("start condemns nothing: replay never read the entry", st.nlost, 0);
	checks++;
	if(objread(s, oid, 4, rd, sizeof rd, 0) >= 0)
		fail("an extent map that failed its checksum was served");
	storestat(s, &st);
	eqv("the slot the damaged map belongs to is condemned", st.nlost, 1);
	eqv("and it is named", storelost(s, 0), oi.slot);
	/*
	 * Condemning a slot does not change what the object *is*.  The
	 * index entry is intact — the damage is in the extent map — so it
	 * stays `live', the checkpoint writes it back that way and
	 * readindex counts it again at the next start.  A condemned copy
	 * is reported by `nlost' and by `corrupt=1' (D14), not by
	 * dropping out of the live count on one side of a restart.
	 */
	eqv("a condemned copy is still a live object", st.nlive, 1);
	/*
	 * D14: the copy fails local verification, so it MUST answer with
	 * corrupt=1 and MUST NOT answer as absent — absence is a §1.5
	 * positive confirmation this holder cannot vouch for, and /lost
	 * carries no oid, so an unhashed slot would be absent for the
	 * life of the disk.  What "not served" means is the content: no
	 * path reads through the damaged map.
	 */
	checks++;
	if(objstat(s, oid, 4, &oi2) < 0)
		fail("a condemned slot answers as absent: %r");
	else{
		istrue("a condemned slot answers corrupt", oi2.corrupt != 0);
		eqv("with the key it had", oi2.ver, 2);
		eqv("and the qid.path it had", oi2.qidpath, oi.qidpath);
	}
	checks++;
	if(objverify(s, oid, 4, &vfy) >= 0)
		fail("a damaged extent map was verified through");
	checks++;
	if(objwrite(s, oid, 4, buf, 64, 0, 3, 1, nil, 0) >= 0)
		fail("a write was served through a damaged extent map");

	/*
	 * The condemnation has to survive a restart, or step 10's "its
	 * slot is not reused" holds for one run only.  The entry here is
	 * intact — the damage is in the extent map — so the checkpoint
	 * writes it back as it stands; writing it back free would hand
	 * the slot out again, leave the object's grains set in the bitmap
	 * with nothing naming them, and lose the store's only record that
	 * it ever held the object.  storecondemn does not itself dirty
	 * the index page, so a second object is what gets it written.
	 */
	mkobj(s, "narrow", 1);
	wr(s, "narrow", &sh, buf, 64, 0, 2);
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storestat(s, &st);
	gf = st.grainfree;
	sf = st.slotfree;
	nl = st.nlive;
	storeclose(s);
	if((s = mustopen(d, "a damaged extent map, restarted")) == nil){
		devclose(d);
		free(buf);
		free(sh.p);
		return;
	}
	storestat(s, &st);
	eqv("the condemned slot is still allocated after a restart",
		st.slotfree, sf);
	eqv("and its grains are still accounted for", st.grainfree, gf);
	eqv("and the live count is the one it had", st.nlive, nl);
	checks++;
	if(objstat(s, oid, 4, &oi2) < 0)
		fail("a condemned slot lost its object across a restart: %r");
	else{
		eqv("the restarted slot keeps its key", oi2.ver, 2);
		eqv("and its length", oi2.len, 3*Blk);
	}
	eqv("start condemns nothing: replay never read the entry",
		st.nlost, 0);
	checks++;
	if(objread(s, oid, 4, rd, sizeof rd, 0) >= 0)
		fail("a damaged extent map was served after a restart");
	storestat(s, &st);
	eqv("and the next read condemns the slot again", st.nlost, 1);

	/*
	 * D14 again, from the sending side: a copy that fails local
	 * verification has no key to defend, so §5.5's op=full MUST be
	 * accepted at any key — here the copy's own — and the heal must
	 * land in the slot and at the qid.path the object already had,
	 * which layer-a §2.3 wants stable.
	 */
	if((g = stageopen(s, oid, 4, 3*Blk, 0)) == nil)
		fail("stageopen: %r");
	else{
		other = mkbuf(3*Blk, 55);
		if(stagewrite(g, other, 3*Blk, 0) < 0)
			fail("stagewrite: %r");
		checks++;
		if(stagefinal(g, 2, 1, nil, 0) < 0)
			fail("op=full to a condemned copy: %r");
		else{
			shwrite(&sh, other, 3*Blk, 0);
			if(objstat(s, oid, 4, &oi2) < 0)
				fail("objstat after the heal: %r");
			else{
				eqv("the heal keeps the slot", oi2.slot,
					oi.slot);
				eqv("and the qid.path", oi2.qidpath,
					oi.qidpath);
			}
			checkobj(s, "wide", &sh, "after the heal");
			storestat(s, &st);
			eqv("and the slot is no longer lost", st.nlost, 0);
		}
		free(other);
	}
	storeclose(s);
	devclose(d);
	free(buf);
	free(sh.p);
}

/*
 * §5 step 7: a log sector the device cannot read is not the end of
 * the log.  Every other reason replay stops is a statement about the
 * bytes at that offset; a device error is not, so stopping there
 * discards whatever is past the fault — acked writes included — and
 * then hands the tail back to be overwritten.  Steps 4, 5 and 6
 * refuse to start on a device error and this must too.
 *
 * Mutation: take the read error as the end of the log, and the store
 * starts with the objects committed past the fault silently gone.
 */
static void
tlogread(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Sbsel sel;
	Super sb;
	Objinfo oi;
	uchar oid[Oidmax];
	char name[8];
	int i;

	d = newdisk();
	if((s = mustopen(d, "an unreadable log sector")) == nil)
		return;
	for(i = 0; i < 5; i++){
		snprint(name, sizeof name, "lr%d", i);
		mkobj(s, name, 1);
	}
	storestat(s, &st);
	eqv("five creates are five live objects", st.nlive, 5);
	storeclose(s);

	/*
	 * One sector, one record in from the replay start: a create is a
	 * one-sector record at this geometry, so the fault lands on a
	 * record with three more committed behind it.  The fault is
	 * sticky and aimed, so nothing outside the log region sees it.
	 */
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	sb = sel.sb[sel.start];
	simfaultat(d, Sfeio, 0, (vlong)(sb.cklogoff + 1)*sb.secsz, sb.secsz);
	checks++;
	if((s = openstore(d)) != nil){
		fail("a store started over a log it could not read");
		storeclose(s);
	}

	/* and with the fault gone it starts and has all five */
	simfault(d, Sfnone, 0);
	if((s = mustopen(d, "the log, readable again")) == nil){
		devclose(d);
		return;
	}
	storestat(s, &st);
	eqv("replay reaches every record past the fault", st.nlive, 5);
	oidof(oid, "lr4");
	checks++;
	if(objstat(s, oid, 3, &oi) < 0)
		fail("the last object committed is gone: %r");
	storeclose(s);
	devclose(d);
}

/*
 * §2.6: ndirty is an implementation limit in exactly layer-a §7.1's
 * sense.  When it is exhausted the store discards every fine-grained
 * record for the peer with the most records and marks that peer
 * fullsync — the records are dropped, not the write.  It is done in
 * the apply, so §3.2's commit path and §5's replay answer the same
 * thing: a full region the live path refused and replay ignored is a
 * store whose memory differs from what its own log rebuilds, and the
 * refusal itself arrives after the record is already durable.
 */
static void
tdirtyfull(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	uchar oid[Oidmax];
	char name[32];
	ulong i, n;

	d = newdisk();
	if((s = mustopen(d, "a full dirty region")) == nil)
		return;
	n = 0;
	for(i = 0; i < 64; i++){
		snprint(name, sizeof name, "dp%lud", i);
		oidof(oid, name);
		if(dirtyadd(s, oid, strlen(name), "peer.a", 7 + i) < 0)
			break;
		n++;
	}
	eqv("the dirty region holds ndirty records", dirtycount(s), n);
	istrue("the small geometry's region was filled", n >= 64);

	oidof(oid, "dq");
	checks++;
	if(dirtyadd(s, oid, 2, "peer.b", 9) < 0)
		fail("a full dirty region failed a durable write instead of "
			"dropping a peer's records: %r");
	storestat(s, &st);
	istrue("the store still commits", st.broken == 0);
	eqv("the peer with the most records lost them", dirtycount(s), 1);
	istrue("that peer is marked fullsync", storefullsync(s, "peer.a"));
	istrue("and the new record is the one that is there",
		dirtyhas(s, oid, 2, "peer.b"));

	/* replay reaches the same set, by the same rule */
	storeclose(s);
	if((s = mustopen(d, "a full dirty region, replayed")) == nil){
		devclose(d);
		return;
	}
	eqv("replay reaches the same dirty set", dirtycount(s), 1);
	istrue("with the same record in it", dirtyhas(s, oid, 2, "peer.b"));
	storeclose(s);
	devclose(d);
}

/*
 * §2.6's tie, which is where the exhaustion rule's own argument is
 * won or lost.  The rule keeps the live path and replay answering the
 * same thing, so which peer it drops must be a function of the
 * records and of nothing else — and the peer list is not that: it is
 * first-apply order while the store runs and readdirty's slot order
 * reversed after a restart.
 *
 * The two orders are made to differ here: peer.a's records are added
 * first and then deleted, so peer.b's take the low slots and peer.a
 * is still the peer the running store heard of first.  A running
 * store therefore has [b, a] and a restarted one [a, b], over exactly
 * the same 32 records each.  Both must drop peer.a, which is the
 * lower name.
 *
 * Mutation: break the tie by list order (`n > best` alone), and the
 * live store drops peer.b where the restarted one drops peer.a.
 */
static void
tdirtytie(int restart)
{
	Dev *d;
	Store *s;
	uchar oid[Oidmax];
	char name[32];
	ulong i;
	int ok;

	d = newdisk();
	if((s = mustopen(d, "a tie in the dirty region")) == nil)
		return;
	ok = 1;
	for(i = 0; i < 32; i++){
		snprint(name, sizeof name, "ta%lud", i);
		oidof(oid, name);
		if(dirtyadd(s, oid, strlen(name), "peer.a", 7) < 0)
			ok = 0;
	}
	for(i = 0; i < 32; i++){
		snprint(name, sizeof name, "ta%lud", i);
		oidof(oid, name);
		if(dirtydel(s, oid, strlen(name), "peer.a") < 0)
			ok = 0;
	}
	if(storecheckpoint(s) < 0)
		ok = 0;
	for(i = 0; i < 32; i++){
		snprint(name, sizeof name, "tb%lud", i);
		oidof(oid, name);
		if(dirtyadd(s, oid, strlen(name), "peer.b", 7) < 0)
			ok = 0;
	}
	if(storecheckpoint(s) < 0)
		ok = 0;
	for(i = 0; i < 32; i++){
		snprint(name, sizeof name, "tc%lud", i);
		oidof(oid, name);
		if(dirtyadd(s, oid, strlen(name), "peer.a", 7) < 0)
			ok = 0;
	}
	if(storecheckpoint(s) < 0)
		ok = 0;
	istrue("the two peers filled the region between them", ok);
	eqv("with half the records each", dirtycount(s), 64);

	/*
	 * The restart is what re-derives the peer list from the region
	 * itself; the checkpoint above is what leaves replay nothing to
	 * add to it in first-apply order.
	 */
	if(restart){
		storeclose(s);
		if((s = mustopen(d, "a tie, after a restart")) == nil){
			devclose(d);
			return;
		}
		eqv("the restarted store holds the same records",
			dirtycount(s), 64);
	}

	oidof(oid, "tz");
	checks++;
	if(dirtyadd(s, oid, 2, "peer.c", 9) < 0)
		fail("a tied region failed a durable write: %r");
	oidof(oid, "tc0");
	istrue("the lower name's records are the ones dropped",
		!dirtyhas(s, oid, 3, "peer.a"));
	oidof(oid, "tb0");
	istrue("and the higher name's are the ones kept",
		dirtyhas(s, oid, 3, "peer.b"));
	eqv("32 of them, and the record that displaced them",
		dirtycount(s), 33);
	storeclose(s);
	devclose(d);
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

/*
 * §0 and §2.1: `blksz` is the format's and `Wunit` is the device's,
 * so a store formatted with a grain larger than the unit's write
 * unit opens and runs — devwrite splits each grain write into
 * `Wunit` pieces (§0).  Nothing above the device layer may consult
 * `wunit`, which is a build constant here and a property of whatever
 * unit the store is carried to next.
 */
static void
tbigblk(void)
{
	Dev *d;
	Store *s;
	Super sb;
	Fmtcfg c;
	Shadow sh;
	uchar *buf;

	memset(&sh, 0, sizeof sh);
	if((d = simopen(Tsecsz, Tnsec, Tseed)) == nil)
		sysfatal("simopen: %r");
	smallcfg(&c);
	c.blksz = 4*Wunitdflt;			/* 64 KiB: four write units */
	c.objmax = 4*(uvlong)c.blksz;
	c.logbytes = 4*(uvlong)c.blksz;
	if(geometry(&sb, &c, d->size) < 0){
		fail("a geometry at a blksz above the write unit: %r");
		devclose(d);
		return;
	}
	eqv("the grain is above the device write unit", sb.blksz,
		4*(uvlong)d->wunit);
	if(fmtstore(d, &sb) < 0){
		fail("fmtstore at a blksz above the write unit: %r");
		devclose(d);
		return;
	}
	if((s = mustopen(d, "a grain above the write unit")) == nil){
		devclose(d);
		return;
	}
	csumblk = c.blksz;
	mkobj(s, "wide", 1);
	buf = mkbuf(c.blksz + 100, 3);
	wr(s, "wide", &sh, buf, c.blksz + 100, 0, 2);
	checkobj(s, "wide", &sh, "a grain above the write unit");

	/* and the log it wrote replays from the same disk */
	storeclose(s);
	if((s = mustopen(d, "replayed above the write unit")) != nil){
		checkobj(s, "wide", &sh, "replayed above the write unit");
		storeclose(s);
	}
	csumblk = Blk;
	free(buf);
	free(sh.p);
	devclose(d);
}

/*
 * §4's re-hashing rules, over every shape of write that changes a
 * block's covered length without naming it.  layer-a §1.4 hashes the
 * final partial block over its actual length, so `len' alone changes
 * digests: a growth past a partial final block re-hashes the block
 * that held the old len, a truncate re-hashes the block that holds
 * the new one, and an extension over bytes a truncate left behind
 * reads — and so hashes — as zeros.
 *
 * Each case asserts the whole of what §4 promises through checkobj:
 * the object reads back what was written, its stored csum is the csum
 * of that byte image, and verify agrees with both.  The failure this
 * catches is the expensive one: a stored csum that matches the digest
 * array while neither matches the bytes gives verify arraybad=0 with
 * one bad block, which routes §8 to a block repair over bytes that
 * were never wrong, and leaves two instances that took the same
 * writes publishing one key with two csums (layer-a §1.3's I3).
 */
static void
textend(void)
{
	Dev *d;
	Store *s;
	Shadow p, q, r, t, v, h;
	uchar *buf, oid[Oidmax];

	memset(&p, 0, sizeof p);
	memset(&q, 0, sizeof q);
	memset(&r, 0, sizeof r);
	memset(&t, 0, sizeof t);
	memset(&v, 0, sizeof v);
	memset(&h, 0, sizeof h);
	d = newdisk();
	if((s = mustopen(d, "extend")) == nil)
		return;
	buf = mkbuf(4*Blk, 71);

	/* a partial block 0, then a sparse extend far past it */
	mkobj(s, "p", 1);
	wr(s, "p", &p, buf, 100, 0, 2);
	checkobj(s, "p", &p, "a partial first block");
	wr(s, "p", &p, buf + 100, 16, 2*Blk, 3);
	checkobj(s, "p", &p, "grown past the partial block");

	/* the same growth by truncate, which names no block at all */
	mkobj(s, "q", 1);
	wr(s, "q", &q, buf, 100, 0, 2);
	oidof(oid, "q");
	if(objtrunc(s, oid, 1, 3*Blk + 7, 3, 1, nil, 0) < 0)
		fail("objtrunc extend: %r");
	else
		shtrunc(&q, 3*Blk + 7);
	checkobj(s, "q", &q, "extended by truncate");

	/* and within the block that holds the old len */
	mkobj(s, "r", 1);
	wr(s, "r", &r, buf, 100, 0, 2);
	oidof(oid, "r");
	if(objtrunc(s, oid, 1, 300, 3, 1, nil, 0) < 0)
		fail("objtrunc extend within a block: %r");
	else
		shtrunc(&r, 300);
	checkobj(s, "r", &r, "extended within the block");

	/*
	 * A truncate down within a block and back up: the bytes between
	 * the two lengths are not the object's content and §4 has the
	 * extension read them as zeros, so the block is composed afresh
	 * rather than re-hashed over what the grain still holds.
	 */
	mkobj(s, "t", 1);
	wr(s, "t", &t, buf, 200, 0, 2);
	oidof(oid, "t");
	if(objtrunc(s, oid, 1, 40, 3, 1, nil, 0) < 0)
		fail("objtrunc shrink: %r");
	else
		shtrunc(&t, 40);
	checkobj(s, "t", &t, "truncated within a block");
	if(objtrunc(s, oid, 1, 400, 4, 1, nil, 0) < 0)
		fail("objtrunc re-extend: %r");
	else
		shtrunc(&t, 400);
	checkobj(s, "t", &t, "truncated within a block and re-extended");

	/* the same, reached by a partial write above the new len */
	mkobj(s, "v", 1);
	wr(s, "v", &v, buf, 200, 0, 2);
	oidof(oid, "v");
	if(objtrunc(s, oid, 1, 40, 3, 1, nil, 0) < 0)
		fail("objtrunc shrink: %r");
	else
		shtrunc(&v, 40);
	wr(s, "v", &v, buf + 300, 8, 150, 4);
	checkobj(s, "v", &v, "written above a truncated length");

	/*
	 * The final block a *hole*, truncated and extended within it.
	 * Nothing re-hashes it — clause 4 of the apply recomputes the
	 * zero digest from len — which is the case a clause 4 restricted
	 * to the range a growth covers gets wrong with no crash at all.
	 */
	mkobj(s, "h", 1);
	oidof(oid, "h");
	if(objtrunc(s, oid, 1, Blk, 2, 1, nil, 0) < 0)
		fail("objtrunc to a hole: %r");
	else
		shtrunc(&h, Blk);
	if(objtrunc(s, oid, 1, 40, 3, 1, nil, 0) < 0)
		fail("objtrunc a hole down: %r");
	else
		shtrunc(&h, 40);
	checkobj(s, "h", &h, "a hole truncated within itself");
	if(objtrunc(s, oid, 1, 80, 4, 1, nil, 0) < 0)
		fail("objtrunc a hole up: %r");
	else
		shtrunc(&h, 80);
	checkobj(s, "h", &h, "a hole extended within itself");

	/* every one of them survives the restart the log describes */
	storeclose(s);
	if((s = mustopen(d, "extend replayed")) == nil){
		devclose(d);
		free(buf);
		return;
	}
	checkobj(s, "p", &p, "grown past the partial block, replayed");
	checkobj(s, "q", &q, "extended by truncate, replayed");
	checkobj(s, "r", &r, "extended within the block, replayed");
	checkobj(s, "t", &t, "re-extended, replayed");
	checkobj(s, "v", &v, "written above a truncated length, replayed");
	checkobj(s, "h", &h, "a hole extended within itself, replayed");
	storeclose(s);
	devclose(d);
	free(buf);
	free(p.p);
	free(q.p);
	free(r.p);
	free(t.p);
	free(v.p);
	free(h.p);
}

void
main(int argc, char **argv)
{
	USED(argc); USED(argv);
	tbasic();
	tholes();
	textend();
	tcoverage();
	trebuild();
	tpublish();
	tckmark();
	tcondemn();
	tbadmap();
	tlogread();
	tdirtyfull();
	tdirtytie(0);
	tdirtytie(1);
	tflushchan();
	tbigblk();
	if(fails > 0){
		fprint(2, "storetest: %d of %d checks failed\n", fails, checks);
		exits("failed");
	}
	print("storetest: %d checks ok\n", checks);
	exits(nil);
}
