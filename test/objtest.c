#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"
#include "t1.h"

/*
 * T1 for the object-level invariants of docs/design/store.md: §2.7's
 * extent-map slot rule and the transitions it covers, §2.4's
 * invariant on the shrinking side, §3.5's deferred reuse of grains
 * and slots, §3.6's stage lifetime, §6's four exhaustions and R7's
 * dirty records.
 */

enum
{
	Blk	= 4096,
};

static void
mk(Store *s, char *name)
{
	uchar o[Oidmax];

	oidof(o, name);
	if(objcreate(s, o, strlen(name), 1, 1, nil) < 0)
		fail("objcreate %s: %r", name);
}

static int
wr(Store *s, char *name, void *a, long n, uvlong off)
{
	uchar o[Oidmax];

	oidof(o, name);
	if(objwrite(s, o, strlen(name), a, n, off, 2, 1, nil, 0) < 0)
		return -1;
	return 0;
}

static int
trunc(Store *s, char *name, uvlong len, uvlong ver)
{
	uchar o[Oidmax];

	oidof(o, name);
	return objtrunc(s, o, strlen(name), len, ver, 1);
}

static int
rmv(Store *s, char *name, uvlong ver)
{
	uchar o[Oidmax];

	oidof(o, name);
	return objremove(s, o, strlen(name), ver, 1);
}

static int
ostat(Store *s, char *name, Objinfo *oi)
{
	uchar o[Oidmax];

	oidof(o, name);
	return objstat(s, o, strlen(name), oi);
}

static void
mustwr(Store *s, char *name, void *a, long n, uvlong off)
{
	if(wr(s, name, a, n, off) < 0)
		fail("objwrite %s %ld at %llud: %r", name, n, off);
}

static void
rd(Store *s, char *name, void *a, long n, uvlong off, char *what)
{
	uchar o[Oidmax];
	long m;

	oidof(o, name);
	if((m = objread(s, o, strlen(name), a, n, off)) != n)
		fail("%s: objread %s: %ld of %ld: %r", what, name, m, n);
}

static void
mustverify(Store *s, char *name, char *what)
{
	Vfy v;
	uchar o[Oidmax];

	oidof(o, name);
	if(objverify(s, o, strlen(name), &v) < 0){
		fail("%s: objverify %s: %r", what, name);
		return;
	}
	checks++;
	if(v.arraybad || v.nbad != 0)
		fail("%s: %s: verify: arraybad=%d nbad=%lud", what, name,
			v.arraybad, v.nbad);
	vfyfree(&v);
}

static ulong
emapslotof(Store *s, char *name)
{
	Objinfo oi;

	if(ostat(s, name, &oi) < 0){
		fail("objstat %s: %r", name);
		return 0;
	}
	return oi.emapslot;
}

static void
zeroes(char *what, uchar *p, long n)
{
	long i;

	checks++;
	for(i = 0; i < n; i++)
		if(p[i] != 0){
			fail("%s: byte %ld of a hole is %#ux, want 0", what, i,
				p[i]);
			return;
		}
}

/*
 * §2.7's extent-map slot rule, in the three transitions it covers.  A
 * slot released by a deleted object still holds that object's map —
 * §2.4 zeroes nothing on release — so a commit that allocates that
 * slot and does not name every block below nblk inherits live grain
 * numbers the allocator has since handed to other objects.  Run twice
 * over: once with block 0 holding its own bytes, and once with block
 * 0 a hole, which is the case a rule that exempts holes from nmap
 * gets wrong with no crash at all.
 */
static void
tslot(int holevariant, int crash, int reckpt)
{
	Dev *d;
	Store *s;
	uchar *big, *one, *two, *got;
	ulong slot;
	char *what;

	what = holevariant ? "slot reuse, block 0 a hole"
		: "slot reuse, block 0 with content";
	d = newdisk();
	if((s = mustopen(d, what)) == nil)
		return;
	big = mkbuf(3*Blk, 11);
	one = mkbuf(Blk, 13);
	two = mkbuf(Blk, 17);
	got = malloc(3*Blk);
	if(got == nil)
		sysfatal("malloc: %r");

	/* a multi-block object with content in every block */
	mk(s, "big");
	mustwr(s, "big", big, 3*Blk, 0);
	slot = emapslotof(s, "big");
	istrue("a three-block object holds an extent-map slot", slot != 0);

	/* delete it: the slot is released and its bytes are left behind */
	if(rmv(s, "big", 3) < 0)
		fail("%s: objremove: %r", what);

	mk(s, "small");
	if(holevariant){
		/* create, truncate to blksz: block 0 is a hole */
		if(trunc(s, "small", Blk, 2) < 0)
			fail("%s: objtrunc: %r", what);
	}else
		mustwr(s, "small", one, Blk, 0);

	/*
	 * Grow past one block with a write that does not touch block 0.
	 * The commit changes emapslot, so it must set Oslot, zero the
	 * target map and name every block below nblk — holes included.
	 */
	mustwr(s, "small", two, Blk, Blk);
	eqv("the released slot is the one reused", emapslotof(s, "small"), slot);

	if(crash){
		simcrashdead(d, 1);
		simcrashmode(d, Scdrop);
		simarm(d, "commit", 0);
		if(wr(s, "small", two, 16, 2*Blk) >= 0)
			fail("%s: a commit whose header never landed reported "
				"success", what);
		storeclose(s);
		simrevive(d);
		if((s = mustopen(d, "after the lost commit")) == nil){
			devclose(d);
			return;
		}
	}else if(reckpt){
		/*
		 * The re-replay schedule: the object's index page is
		 * written and its extent-map entry is not.  Replay must
		 * zero the map it inherits even though the entry it is
		 * applying to already carries the record's emapslot —
		 * which is why the zeroing is keyed off the record's
		 * Oslot and not off a comparison with live state.
		 */
		simcrashdead(d, 1);
		simcrashmode(d, Sckeep);
		simarm(d, "ckpt", 1);
		storecheckpoint(s);
		storeclose(s);
		simrevive(d);
		if((s = mustopen(d, "after the half-written checkpoint")) == nil){
			devclose(d);
			return;
		}
	}else{
		storeclose(s);
		if((s = mustopen(d, "replayed")) == nil){
			devclose(d);
			return;
		}
	}

	rd(s, "small", got, 2*Blk, 0, what);
	checks++;
	if(holevariant)
		zeroes(what, got, Blk);
	else if(memcmp(got, one, Blk) != 0)
		fail("%s: block 0 does not hold its own bytes", what);
	checks++;
	if(memcmp(got + Blk, two, Blk) != 0)
		fail("%s: block 1 does not hold its own bytes", what);
	mustverify(s, "small", what);

	/* the mirror: a truncate to one block carries block 0 out */
	if(trunc(s, "small", Blk, 4) < 0)
		fail("%s: objtrunc to one block: %r", what);
	eqv("a one-block object holds no extent-map slot",
		emapslotof(s, "small"), 0);
	rd(s, "small", got, Blk, 0, what);
	if(holevariant)
		zeroes(what, got, Blk);
	else{
		checks++;
		if(memcmp(got, one, Blk) != 0)
			fail("%s: block 0 did not survive the slot release",
				what);
	}
	mustverify(s, "small", "after the shrink");
	storeclose(s);
	devclose(d);
	free(big);
	free(one);
	free(two);
	free(got);
}

/*
 * T1.6: truncate across a block boundary, force the freed grain's
 * reallocation to another object, extend, and read zeros.  Without
 * §2.7 clause 5 the truncate leaves the old grain numbers in place,
 * the grains are freed and reallocated, a later extend turns those
 * blocks into holes only in the digest array — and a read of the
 * extended object serves the other object's bytes.
 */
static void
ttruncextend(void)
{
	Dev *d;
	Store *s;
	uchar *a, *b, *got;

	d = newdisk();
	if((s = mustopen(d, "truncate then extend")) == nil)
		return;
	a = mkbuf(5*Blk, 19);
	b = mkbuf(2*Blk, 23);
	if((got = mallocz(5*Blk, 1)) == nil)
		sysfatal("malloc: %r");

	/*
	 * Five blocks down to three and back to five, so the object
	 * keeps its extent-map slot across the shrink: that is where
	 * clause 5 does its work, because a shrink that releases the
	 * slot has the whole entry zeroed by clause 2 instead.
	 */
	mk(s, "a");
	mustwr(s, "a", a, 5*Blk, 0);
	if(trunc(s, "a", 3*Blk, 3) < 0)
		fail("objtrunc: %r");

	/* the freed grains go to another object, which writes over them */
	mk(s, "b");
	mustwr(s, "b", b, 2*Blk, 0);

	/* extend a again: the blocks it grows across must read as zero */
	if(trunc(s, "a", 5*Blk, 4) < 0)
		fail("objtrunc extend: %r");
	rd(s, "a", got, 5*Blk, 0, "truncate then extend");
	checks++;
	if(memcmp(got, a, 3*Blk) != 0)
		fail("truncate then extend: the surviving blocks changed");
	zeroes("the blocks a truncate released", got + 3*Blk, 2*Blk);
	mustverify(s, "a", "truncate then extend");
	mustverify(s, "b", "the other object");

	storeclose(s);
	if((s = mustopen(d, "truncate then extend, replayed")) == nil){
		devclose(d);
		return;
	}
	rd(s, "a", got, 5*Blk, 0, "replayed");
	zeroes("the blocks a truncate released, after replay", got + 3*Blk,
		2*Blk);
	mustverify(s, "a", "replayed");
	storeclose(s);
	devclose(d);
	free(a);
	free(b);
	free(got);
}

/*
 * §3.5's deferred reuse, for grains and for slots alike.  A grain, an
 * index slot or an extent-map slot released by a commit MUST NOT be
 * reallocated until that commit's post-flush has returned — otherwise
 * object A's commit frees grain g and is lost in the crash while g
 * has been handed to object B, staged into, and B's commit lost too,
 * and replay restores A's map pointing at bytes that are now B's.
 *
 * The schedule holds one batch before its header write (§13's
 * batch:n) and asks what the allocator can see meanwhile.
 */
typedef struct Held Held;
struct Held
{
	Store	*s;
	int	done;
	int	err;
	char	kind;
};

static void
heldwrite(void *a)
{
	Held *h;
	uchar *buf;
	

	uchar o[Oidmax];

	h = a;
	buf = mkbuf(Blk, 29);
	if(h->kind == 'g'){
		oidof(o, "held");
		if(objwrite(h->s, o, 4, buf, Blk, 0, 5, 1, nil, 0) < 0)
			h->err = 1;
	}else{
		oidof(o, "gone");
		if(objdiscard(h->s, o, 4) < 0)
			h->err = 1;
	}
	free(buf);
	h->done = 1;
}

static void
tdeferred(char kind)
{
	Dev *d;
	Store *s;
	Storestat st;
	Held *h;
	uchar *buf;
	uvlong before, seq;
	int i;

	d = newdisk();
	if((s = mustopen(d, "deferred reuse")) == nil)
		return;
	buf = mkbuf(Blk, 31);
	if(kind == 'g'){
		mk(s, "held");
		mustwr(s, "held", buf, Blk, 0);
	}else{
		mk(s, "gone");
		if(rmv(s, "gone", 2) < 0)
			fail("objremove: %r");
	}
	storestat(s, &st);
	before = kind == 'g' ? st.grainfree : st.slotfree;
	seq = st.seqnext;

	if((h = mallocz(sizeof *h, 1)) == nil)
		sysfatal("mallocz: %r");
	h->s = s;
	h->kind = kind;
	storehook(s, "batch", seq);
	if(spawnproc(heldwrite, h) < 0){
		fail("spawn: %r");
		storehook(s, "batch", 0);
		storeclose(s);
		devclose(d);
		return;
	}
	/* wait for the held proc to have staged and reached the hold */
	for(i = 0; i < 2000; i++){
		storestat(s, &st);
		if(st.seqnext > seq)
			break;
		sleep(1);
	}
	storestat(s, &st);
	checks++;
	if(kind == 'g'){
		if(st.grainfree != before - 1)
			fail("a held commit's stage should hold one grain: "
				"grainfree %llud, was %llud", st.grainfree,
				before);
	}else{
		if(st.slotfree != before)
			fail("a held commit must not have freed its slot yet: "
				"slotfree %llud, was %llud", st.slotfree,
				before);
	}
	storehook(s, "batch", 0);
	for(i = 0; i < 4000 && !h->done; i++)
		sleep(1);
	istrue("the held commit completed", h->done && !h->err);
	storestat(s, &st);
	checks++;
	if(kind == 'g'){
		if(st.grainfree != before)
			fail("the freed grain returns only at the apply: "
				"grainfree %llud, want %llud", st.grainfree,
				before);
	}else{
		if(st.slotfree != before + 1)
			fail("the freed slot returns at the apply: slotfree "
				"%llud, want %llud", st.slotfree, before + 1);
	}
	free(h);
	free(buf);
	storeclose(s);
	devclose(d);
}

/*
 * §3.6's stages.  A stage is memory-only: its grains are reserved
 * rather than allocated, no record names them, and §2.8 keeps
 * reservations out of the checkpointed bitmap — so no restart path
 * can resurrect them and none can leak across one.  Were
 * reservations written into the bitmap, a checkpoint taken while a
 * stage was live would leak them across every subsequent restart.
 */
static void
tstage(void)
{
	Dev *d;
	Store *s;
	Stage *g;
	Storestat st, st2;
	uchar *buf, *got, o[Oidmax];
	uvlong before;
	int i;

	d = newdisk();
	if((s = mustopen(d, "stages")) == nil)
		return;
	buf = mkbuf(2*Blk, 37);
	if((got = malloc(2*Blk)) == nil)
		sysfatal("malloc: %r");
	mk(s, "full");
	storestat(s, &st);
	before = st.grainfree;

	/* clunk: the free-grain count returns to its pre-transfer value */
	oidof(o, "full");
	if((g = stageopen(s, o, 4, 2*Blk, 0)) == nil){
		fail("stageopen: %r");
		storeclose(s);
		devclose(d);
		return;
	}
	if(stagewrite(g, buf, Blk, 0) < 0)
		fail("stagewrite: %r");
	storestat(s, &st2);
	eqv("a staged grain is reserved, not allocated", st2.staged, 1);
	eqv("a reservation costs a free grain", st2.grainfree, before - 1);
	stagediscard(g);
	storestat(s, &st2);
	eqv("a discarded stage releases its reservations", st2.grainfree,
		before);
	eqv("and leaves nothing staged", st2.staged, 0);

	/* timeout */
	if((g = stageopen(s, o, 4, 2*Blk, 0)) == nil)
		fail("stageopen: %r");
	else if(stagewrite(g, buf, Blk, 0) < 0)
		fail("stagewrite: %r");
	sleep(80);
	stagesweep(s, nsec());
	storestat(s, &st2);
	eqv("a stage that has gone quiet is swept", st2.grainfree, before);

	/*
	 * The restart case: a checkpoint while the stage is live, then
	 * abandon it and crash.  The free-grain count must be the
	 * pre-transfer one.
	 */
	if((g = stageopen(s, o, 4, 2*Blk, 0)) == nil)
		fail("stageopen: %r");
	else if(stagewrite(g, buf, 2*Blk, 0) < 0)
		fail("stagewrite: %r");
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storeclose(s);
	if((s = mustopen(d, "after a stage was abandoned")) == nil){
		devclose(d);
		return;
	}
	storestat(s, &st2);
	eqv("a stage leaves nothing durable behind", st2.grainfree, before);
	eqv("and nothing staged", st2.staged, 0);

	/* and a stage that completes publishes the whole object */
	oidof(o, "full");
	if((g = stageopen(s, o, 4, 2*Blk, 0)) == nil)
		fail("stageopen: %r");
	else{
		if(stagewrite(g, buf, Blk, 0) < 0
		|| stagewrite(g, buf + Blk, Blk, Blk) < 0)
			fail("stagewrite: %r");
		if(stagefinal(g, 9, 1) < 0)
			fail("stagefinal: %r");
	}
	rd(s, "full", got, 2*Blk, 0, "op=full");
	checks++;
	if(memcmp(got, buf, 2*Blk) != 0)
		fail("op=full: content differs");
	mustverify(s, "full", "op=full");

	/* the per-fid and per-process bounds are both enforced */
	if((g = stageopen(s, o, 4, 64*1024, 0)) == nil)
		fail("stageopen: %r");
	else{
		for(i = 0; i < 16; i++)
			if(stagewrite(g, buf, Blk, (uvlong)i*Blk) < 0)
				break;
		checks++;
		if(i == 16)
			fail("stagemax was not enforced");
		stagediscard(g);
	}
	storeclose(s);
	devclose(d);
	free(buf);
	free(got);
}

/*
 * §6's four exhaustions, mapped deliberately: no free grain, no free
 * index slot, no free extent-map slot, and no free log space — the
 * first three `disk full' at once, the fourth after a bounded wait.
 * Delete and tombstone discard keep working throughout, on the
 * reserved log tail, because otherwise log exhaustion blocks the
 * delete that would have relieved the slot exhaustion and the two
 * lock each other in the direction that removes the escape.
 */
static void
texhaust(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	uchar *buf, o[Oidmax];
	char name[32];
	int i, n;

	/* index slots */
	d = newdisk();
	if((s = mustopen(d, "exhaust slots")) == nil)
		return;
	n = -1;
	for(i = 0; i < 200; i++){
		snprint(name, sizeof name, "s%d", i);
		oidof(o, name);
		if(objcreate(s, o, strlen(name), 1, 1, nil) < 0){
			n = i;
			break;
		}
		if(i % 32 == 31 && storecheckpoint(s) < 0)
			fail("storecheckpoint: %r");
	}
	checks++;
	if(n < 0)
		fail("index slots never ran out");
	storestat(s, &st);
	eqv("no index slot is left", st.slotfree, 0);
	/* a delete still works, and its discard returns the slot */
	snprint(name, sizeof name, "s%d", 0);
	oidof(o, name);
	if(objremove(s, o, strlen(name), 2, 1) < 0)
		fail("delete under slot exhaustion: %r");
	if(objdiscard(s, o, strlen(name)) < 0)
		fail("tombstone discard under slot exhaustion: %r");
	storestat(s, &st);
	eqv("the discard returned the slot", st.slotfree, 1);
	storeclose(s);
	devclose(d);

	/* extent-map slots */
	d = newdisk();
	if((s = mustopen(d, "exhaust extent maps")) == nil)
		return;
	buf = mkbuf(2*Blk, 41);
	n = -1;
	for(i = 0; i < 64; i++){
		snprint(name, sizeof name, "e%d", i);
		mk(s, name);
		if(wr(s, name, buf, 2*Blk, 0) < 0){
			n = i;
			break;
		}
		if(i % 8 == 7 && storecheckpoint(s) < 0)
			fail("storecheckpoint: %r");
	}
	checks++;
	if(n < 0)
		fail("extent-map slots never ran out");
	storestat(s, &st);
	eqv("no extent-map slot is left", st.emapfree, 0);
	istrue("grains are not what ran out", st.grainfree > 0);
	istrue("index slots are not what ran out", st.slotfree > 0);
	storeclose(s);
	devclose(d);
	free(buf);

	/*
	 * Log space, with no checkpointer running: the commit waits
	 * ckwaitms and then answers disk full, while a space-freeing
	 * commit still goes through on the reserved tail.
	 */
	d = newdisk();
	if((s = mustopen(d, "exhaust the log")) == nil)
		return;
	buf = mkbuf(64, 43);
	mk(s, "l0");
	mk(s, "l1");
	mustwr(s, "l1", buf, 64, 0);
	n = -1;
	for(i = 0; i < 400; i++)
		if(wr(s, "l0", buf, 64, 0) < 0){
			n = i;
			break;
		}
	checks++;
	if(n < 0)
		fail("log space never ran out");
	storestat(s, &st);
	istrue("the log is full", st.logfree <= 16);
	checks++;
	if(rmv(s, "l1", 2) < 0)
		fail("delete must work on the reserved log tail: %r");
	/* and a checkpoint frees the log again */
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	checks++;
	if(wr(s, "l0", buf, 64, 0) < 0)
		fail("a checkpoint did not free the log: %r");
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * R7 and §2.6: each (oid, peer, epoch) survives a crash, and none is
 * lost while the write that caused it is visible.  The records ride
 * in the same log record as the Eobj they belong to (§14(2)), so
 * either both are durable or neither, and the region is read at start
 * before replay adds to and removes from the set.
 */
static void
tdirty(void)
{
	Dev *d;
	Store *s;
	Dirtyrec dr[2];
	uchar *buf, oid[Oidmax];

	d = newdisk();
	if((s = mustopen(d, "dirty records")) == nil)
		return;
	buf = mkbuf(64, 47);
	mk(s, "dobj");
	memset(dr, 0, sizeof dr);
	dr[0].op = 1;
	dr[0].epoch = 5;
	dr[0].oidlen = 4;
	memmove(dr[0].oid, "dobj", 4);
	dr[0].peerlen = 7;
	memmove(dr[0].peer, "node7.1", 7);
	dr[1] = dr[0];
	dr[1].peerlen = 7;
	memmove(dr[1].peer, "node8.2", 7);

	oidof(oid, "dobj");
	if(objwrite(s, oid, 4, buf, 64, 0, 2, 1, dr, 2) < 0)
		fail("objwrite with dirty records: %r");
	eqv("both dirty records are in the set", dirtycount(s), 2);

	/* crash before any checkpoint: the region is rebuilt by replay */
	storeclose(s);
	if((s = mustopen(d, "dirty replayed")) == nil){
		devclose(d);
		return;
	}
	eqv("the dirty set survived replay", dirtycount(s), 2);
	istrue("the record names its peer", dirtyhas(s, oid, 4, "node7.1"));
	istrue("and the other peer", dirtyhas(s, oid, 4, "node8.2"));
	istrue("every peer is fullsync after a restart",
		storefullsync(s, "node7.1"));

	/* checkpoint, restart: now the region itself is the source */
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storeclose(s);
	if((s = mustopen(d, "dirty from the region")) == nil){
		devclose(d);
		return;
	}
	eqv("the dirty region is what start-up reads", dirtycount(s), 2);
	istrue("the record survived the checkpoint",
		dirtyhas(s, oid, 4, "node7.1"));
	if(dirtydel(s, oid, 4, "node7.1") < 0)
		fail("dirtydel: %r");
	eqv("a removal takes it out", dirtycount(s), 1);
	storeclose(s);
	if((s = mustopen(d, "dirty after a removal")) == nil){
		devclose(d);
		return;
	}
	eqv("and the removal survives a restart", dirtycount(s), 1);
	istrue("the removed record is gone",
		!dirtyhas(s, oid, 4, "node7.1"));
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * R8: a tombstone is metadata — state=tomb, len=0, content released,
 * mtime retained — and a create over it reuses the slot and the
 * qid.path, which is what layer-a §2.3 means by stable across delete,
 * tombstone and re-create.
 */
static void
ttomb(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Objinfo oi, oi2;
	uchar *buf, oid[Oidmax];
	uvlong g0, path;

	d = newdisk();
	if((s = mustopen(d, "tombstones")) == nil)
		return;
	buf = mkbuf(2*Blk, 53);
	mk(s, "t");
	oidof(oid, "t");
	if(ostat(s, "t", &oi) < 0)
		fail("objstat: %r");
	path = oi.qidpath;
	storestat(s, &st);
	g0 = st.grainfree;
	mustwr(s, "t", buf, 2*Blk, 0);
	if(rmv(s, "t", 3) < 0)
		fail("objremove: %r");
	if(ostat(s, "t", &oi) < 0)
		fail("objstat after remove: %r");
	eqv("a tombstone has no length", oi.len, 0);
	eqv("a tombstone is a tombstone", oi.state, Stomb);
	eqv("a tombstone holds no extent-map slot", oi.emapslot, 0);
	storestat(s, &st);
	eqv("a tombstone releases its content", st.grainfree, g0);

	oidof(oid, "t");
	if(objcreate(s, oid, 1, 4, 1, &oi2) < 0)
		fail("create over a tombstone: %r");
	eqv("a create over a tombstone keeps the qid.path", oi2.qidpath, path);
	eqv("and its slot", oi2.slot, oi.slot);
	storeclose(s);
	if((s = mustopen(d, "tombstone replayed")) == nil){
		devclose(d);
		return;
	}
	if(objstat(s, oid, 1, &oi2) < 0)
		fail("objstat: %r");
	eqv("the qid.path survived the restart", oi2.qidpath, path);
	eqv("the object is live again", oi2.state, Slive);
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * §8's key-preserving commits.  The corrupt flag is durable, so a
 * restart does not forget it; §2.7's Ocorrupt is where the record
 * carries it, and an apply that carried the entry's own flag through
 * would let replay clear what a scrub had found.
 */
static void
tcorrupt(void)
{
	Dev *d;
	Store *s;
	Objinfo oi;
	uchar *buf, oid[Oidmax];

	d = newdisk();
	if((s = mustopen(d, "corrupt flag")) == nil)
		return;
	buf = mkbuf(2*Blk, 59);
	mk(s, "c");
	mustwr(s, "c", buf, 2*Blk, 0);
	oidof(oid, "c");
	if(objcorrupt(s, oid, 1, 1) < 0)
		fail("objcorrupt: %r");
	if(objstat(s, oid, 1, &oi) < 0)
		fail("objstat: %r");
	eqv("the corrupt flag is set", oi.corrupt, 1);
	eqv("and nothing else changed", oi.len, 2*Blk);
	mustverify(s, "c", "after the corrupt flag");
	storeclose(s);
	if((s = mustopen(d, "corrupt flag replayed")) == nil){
		devclose(d);
		return;
	}
	if(objstat(s, oid, 1, &oi) < 0)
		fail("objstat: %r");
	eqv("replay does not forget the corrupt flag", oi.corrupt, 1);
	mustverify(s, "c", "replayed");
	if(objcorrupt(s, oid, 1, 0) < 0)
		fail("objcorrupt clear: %r");
	if(objstat(s, oid, 1, &oi) < 0)
		fail("objstat: %r");
	eqv("and a repair clears it", oi.corrupt, 0);
	storeclose(s);
	devclose(d);
	free(buf);
}

void
main(int argc, char **argv)
{
	USED(argc); USED(argv);
	tslot(0, 0, 0);
	tslot(1, 0, 0);
	tslot(0, 1, 0);
	tslot(1, 1, 0);
	tslot(1, 0, 1);
	ttruncextend();
	tdeferred('g');
	tdeferred('s');
	tstage();
	texhaust();
	tdirty();
	ttomb();
	tcorrupt();
	if(fails > 0){
		fprint(2, "objtest: %d of %d checks failed\n", fails, checks);
		exits("failed");
	}
	print("objtest: %d checks ok\n", checks);
	exits(nil);
}
