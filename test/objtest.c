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
	if(objcreate(s, o, strlen(name), 1, 1, nil, 0, nil) < 0)
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
	return objtrunc(s, o, strlen(name), len, ver, 1, nil, 0);
}

static int
rmv(Store *s, char *name, uvlong ver)
{
	uchar o[Oidmax];

	oidof(o, name);
	return objremove(s, o, strlen(name), ver, 1, nil, 0);
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

/* a call that must fail, with layer-a §2.6's prefix */
static void
refused(char *what, int r, char *want)
{
	char e[ERRMAX];

	checks++;
	if(r >= 0){
		fail("%s was accepted", what);
		return;
	}
	rerrstr(e, sizeof e);
	if(strncmp(e, want, strlen(want)) != 0)
		fail("%s: %s, want %s", what, e, want);
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
		/* the tombstone rmv left is at (wepoch 1, ver 2) */
		if(objdiscard(h->s, o, 4, 2, 1, 2) < 0)
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
	Stage *g, *g2;
	Storestat st, st2;
	Objinfo oi, oi2;
	uchar *buf, *got, *other, o[Oidmax], o2[Oidmax];
	uvlong before, before2;
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
		if(stagefinal(g, 9, 1, nil, 0) < 0)
			fail("stagefinal: %r");
	}
	rd(s, "full", got, 2*Blk, 0, "op=full");
	checks++;
	if(memcmp(got, buf, 2*Blk) != 0)
		fail("op=full: content differs");
	mustverify(s, "full", "op=full");

	/*
	 * §3.3 and layer-a §5.4 step 7: a discarded stage leaves the
	 * object untouched — same content, same key, same csum, still
	 * verifying — because the published state was never modified.
	 * The free-grain count returning is not that property; this is.
	 */
	if(ostat(s, "full", &oi) < 0)
		fail("objstat full: %r");
	other = mkbuf(2*Blk, 71);
	if((g = stageopen(s, o, 4, 2*Blk, 0)) == nil)
		fail("stageopen: %r");
	else{
		if(stagewrite(g, other, 2*Blk, 0) < 0)
			fail("stagewrite: %r");
		stagediscard(g);
	}
	if(ostat(s, "full", &oi2) < 0)
		fail("objstat full after a discard: %r");
	else{
		eqv("a discarded stage leaves the key", oi2.ver, oi.ver);
		eqv("and the wepoch", oi2.wepoch, oi.wepoch);
		eqv("and the length", oi2.len, oi.len);
		checks++;
		if(memcmp(oi2.csum, oi.csum, Csumlen) != 0)
			fail("a discarded stage changed the csum");
	}
	rd(s, "full", got, 2*Blk, 0, "after a discarded stage");
	checks++;
	if(memcmp(got, buf, 2*Blk) != 0)
		fail("a discarded stage changed the content");
	mustverify(s, "full", "after a discarded stage");
	free(other);

	/* the per-fid bound */
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

	/*
	 * ... and the per-process one, which the per-fid bound is not.
	 * The number of /repl fids is not limited, so a hundred senders
	 * each below stagemax still reserve the disk.  T1's stagemax is 8
	 * and its stagetot 12, so two handles at 8 and 5 grains is the
	 * smallest case that reaches the process bound with neither
	 * handle reaching its own: the refusal below can only be
	 * stagetot's.
	 */
	oidof(o, "full");
	oidof(o2, "full2");
	storestat(s, &st2);
	before2 = st2.grainfree;
	g = stageopen(s, o, 4, 8*Blk, 0);
	g2 = stageopen(s, o2, 5, 8*Blk, 0);
	if(g == nil || g2 == nil)
		fail("stageopen: %r");
	else{
		for(i = 0; i < 8; i++)
			if(stagewrite(g, buf, Blk, (uvlong)i*Blk) < 0)
				break;
		checks++;
		if(i != 8)
			fail("the first handle reached stagemax at %d grains", i);
		for(i = 0; i < 8; i++)
			if(stagewrite(g2, buf, Blk, (uvlong)i*Blk) < 0)
				break;
		checks++;
		if(i == 8)
			fail("stagetot was not enforced: two handles staged "
				"16 grains where stagetot is 12");
		else if(i >= 8)
			fail("the second handle reached its own stagemax");
		storestat(s, &st2);
		eqv("the process bound is where the grains stopped",
			st2.staged, 12);
		stagediscard(g);
		stagediscard(g2);
		storestat(s, &st2);
		eqv("and both handles release what they held", st2.grainfree,
			before2);
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
/*
 * §3.6's two bounds and §0's error classes together: a chunk whose
 * grain write fails must leave the handle exactly as it found it.
 * `interrupted' is an ordinary outcome of any device call, not media
 * damage, and T1's stagetot is 12 and its stagemax 8 — so twelve
 * failed chunks are exactly enough to spend the store's whole stage
 * budget, and eight are exactly enough to spend one /repl fid's, if a
 * failure charges.  The other half is the rewrite: releasing the
 * block's old grain before the replacement lands would leave the
 * handle naming a grain the allocator has taken back, and stagefinal
 * publishes that grain under this object's oid.
 */
static void
tstagefault(void)
{
	Dev *d;
	Store *s;
	Stage *g;
	Storestat st, st2;
	uchar *buf, *got, o[Oidmax];
	int i;

	d = newdisk();
	if((s = mustopen(d, "a chunk that fails")) == nil)
		return;
	buf = mkbuf(2*Blk, 41);
	if((got = malloc(2*Blk)) == nil)
		sysfatal("malloc: %r");
	mk(s, "heal");
	oidof(o, "heal");
	storestat(s, &st);

	/* the process-wide bound, over both classes §0 names */
	for(i = 0; i < 12; i++){
		if((g = stageopen(s, o, 4, Blk, 0)) == nil){
			fail("stageopen: %r");
			break;
		}
		simfault(d, i & 1 ? Sfintr : Sfeio, 1);
		checks++;
		if(stagewrite(g, buf, Blk, 0) == 0)
			fail("a chunk whose grain write failed reported "
				"success");
		simfault(d, Sfnone, 0);
		stagediscard(g);
	}
	storestat(s, &st2);
	eqv("a failed chunk leaves nothing staged", st2.staged, 0);
	eqv("and gives back the grain it allocated", st2.grainfree,
		st.grainfree);
	if((g = stageopen(s, o, 4, Blk, 0)) == nil)
		fail("stageopen: %r");
	else{
		checks++;
		if(stagewrite(g, buf, Blk, 0) < 0)
			fail("twelve failed chunks spent stagetot: %r");
		stagediscard(g);
	}

	/* ... and the per-fid one, on a single handle */
	if((g = stageopen(s, o, 4, 2*Blk, 0)) == nil)
		fail("stageopen: %r");
	else{
		for(i = 0; i < 8; i++){
			simfault(d, Sfintr, 1);
			checks++;
			if(stagewrite(g, buf, Blk, 0) == 0)
				fail("a chunk whose grain write failed "
					"reported success");
			simfault(d, Sfnone, 0);
		}
		checks++;
		if(stagewrite(g, buf, Blk, 0) < 0)
			fail("eight failed chunks spent the handle's "
				"stagemax: %r");
		stagediscard(g);
	}

	/* the rewrite: the grain the handle still names stays reserved */
	if((g = stageopen(s, o, 4, 2*Blk, 0)) == nil)
		fail("stageopen: %r");
	else{
		if(stagewrite(g, buf, Blk, 0) < 0)
			fail("stagewrite: %r");
		storestat(s, &st2);
		eqv("a good chunk stages one grain", st2.staged, 1);
		simfault(d, Sfintr, 1);
		checks++;
		if(stagewrite(g, buf, Blk, 0) == 0)
			fail("a rewrite whose grain write failed reported "
				"success");
		simfault(d, Sfnone, 0);
		storestat(s, &st2);
		eqv("a failed rewrite keeps the block's grain reserved",
			st2.staged, 1);
		if(stagewrite(g, buf + Blk, Blk, Blk) < 0)
			fail("stagewrite: %r");
		if(stagefinal(g, 9, 1, nil, 0) < 0)
			fail("stagefinal after a failed rewrite: %r");
		else{
			rd(s, "heal", got, 2*Blk, 0, "after a failed rewrite");
			checks++;
			if(memcmp(got, buf, 2*Blk) != 0)
				fail("after a failed rewrite: content differs");
			mustverify(s, "heal", "after a failed rewrite");
		}
	}
	storeclose(s);
	devclose(d);
	free(buf);
	free(got);
}

/*
 * §3.6: the sweep's triggers are for a stage whose final=1 has not
 * been attempted, and for no other.  A final=1 parked in the commit —
 * held here at §13's batch hook, as it would be waiting on a
 * checkpoint — gets older than stagems by nothing but bad luck, and a
 * sweep that still saw the handle would return its grains to the
 * allocator under the commit that is about to publish them: another
 * object can be handed the same grain (two objects sharing a grain,
 * undetectable by arbitration), and the committing proc then
 * double-frees.  stagefinal therefore takes the handle off the sweep's
 * list before it drops qlstate.
 */
typedef struct Fin Fin;
struct Fin
{
	Stage	*g;
	int	done;
	int	err;
};

static Fin fin;

static void
finproc(void *a)
{
	Fin *f;

	f = a;
	if(stagefinal(f->g, 9, 1, nil, 0) < 0)
		f->err = 1;
	f->done = 1;
}

static void
tsweepfinal(void)
{
	Dev *d;
	Store *s;
	Stage *g;
	Storestat st;
	uchar *buf, *got, o[Oidmax];
	uvlong seq, before;
	int i;

	d = newdisk();
	if((s = mustopen(d, "a sweep against a parked final")) == nil)
		return;
	buf = mkbuf(2*Blk, 73);
	if((got = malloc(2*Blk)) == nil)
		sysfatal("malloc: %r");
	oidof(o, "sw");
	storestat(s, &st);
	before = st.grainfree;
	seq = st.seqnext;
	if((g = stageopen(s, o, 2, 2*Blk, 0)) == nil){
		fail("stageopen: %r");
		storeclose(s);
		devclose(d);
		free(buf);
		free(got);
		return;
	}
	if(stagewrite(g, buf, 2*Blk, 0) < 0)
		fail("stagewrite: %r");
	storestat(s, &st);
	eqv("two grains are staged", st.staged, 2);

	/* park the final's commit, let the stage age past stagems, sweep */
	storehook(s, "batch", seq);
	fin.g = g;
	fin.done = 0;
	fin.err = 0;
	if(spawnproc(finproc, &fin) < 0){
		fail("spawn: %r");
		storehook(s, "batch", 0);
		storeclose(s);
		devclose(d);
		free(buf);
		free(got);
		return;
	}
	for(i = 0; i < 4000; i++){
		storestat(s, &st);
		if(st.seqnext > seq)
			break;
		sleep(1);
	}
	sleep(80);			/* t1.h's stagems is 50 */
	stagesweep(s, nsec());
	storestat(s, &st);
	eqv("the sweep leaves a parked final's grains reserved",
		st.grainfree, before - 2);

	storehook(s, "batch", 0);
	for(i = 0; i < 4000 && !fin.done; i++)
		sleep(1);
	istrue("the parked final completed", fin.done && !fin.err);
	storestat(s, &st);
	eqv("and its grains were published, not released", st.grainfree,
		before - 2);
	eqv("nothing is left staged", st.staged, 0);
	rd(s, "sw", got, 2*Blk, 0, "after the sweep");
	checks++;
	if(memcmp(got, buf, 2*Blk) != 0)
		fail("a swept final's content differs");
	mustverify(s, "sw", "after the sweep");
	storeclose(s);
	devclose(d);
	free(buf);
	free(got);
}

/*
 * layer-a §1.5's receiver checks, made inside objdiscard: the record
 * must be a tombstone at exactly the key the discard names, with
 * wepoch strictly below the given epoch.  Checked in the call rather
 * than by a separate objstat because the two-step is not atomic: an
 * op=delete between them replaces the tombstone, and the replacement
 * would be dropped unconfirmed — §1.5's resurrection hole.
 */
static void
tdiscard(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	uchar oid[Oidmax];

	d = newdisk();
	if((s = mustopen(d, "tombstone discard")) == nil)
		return;
	mk(s, "dd");
	oidof(oid, "dd");
	if(objremove(s, oid, 2, 3, 1, nil, 0) < 0)
		fail("objremove: %r");
	/* the tombstone is at (wepoch 1, ver 3) */
	refused("a discard naming the wrong ver",
		objdiscard(s, oid, 2, 2, 1, 5), "not discardable");
	refused("a discard naming the wrong wepoch",
		objdiscard(s, oid, 2, 3, 2, 5), "not discardable");
	refused("a discard at an epoch the tombstone's wepoch reaches",
		objdiscard(s, oid, 2, 3, 1, 1), "not discardable");
	refused("a discard of an id nothing holds",
		objdiscard(s, (uchar*)"zz", 2, 3, 1, 5), "no such object");
	storestat(s, &st);
	eqv("a refused discard keeps the tombstone", st.ntomb, 1);
	checks++;
	if(objdiscard(s, oid, 2, 3, 1, 5) < 0)
		fail("a discard naming the key exactly: %r");
	storestat(s, &st);
	eqv("the discard freed the tombstone", st.ntomb, 0);
	storeclose(s);
	devclose(d);
}

/*
 * §3.2: a store whose apply failed after its record was durable is
 * serving in-memory state that its own log no longer describes, so it
 * "answers nothing until it has been opened again".  Nothing is
 * every entry point, not the read ones alone: the update calls run
 * updopen, read extent maps and can condemn a slot on the way before
 * the commit path refuses them with a device error rather than with
 * §3.2's reason.
 */
static void
tcondemned(void)
{
	Dev *d;
	Store *s;
	Stage *g;
	Objinfo oi;
	uchar *buf, o[Oidmax];
	char *w;

	d = newdisk();
	if((s = mustopen(d, "a condemned store")) == nil)
		return;
	buf = mkbuf(Blk, 67);
	mk(s, "z");
	oidof(o, "z");
	mustwr(s, "z", buf, Blk, 0);
	if((g = stageopen(s, o, 1, Blk, 0)) == nil){
		fail("stageopen: %r");
		storeclose(s);
		devclose(d);
		free(buf);
		return;
	}
	storehook(s, "fatal", 1);
	w = "store condemned";
	refused("objstat on a condemned store", objstat(s, o, 1, &oi), w);
	refused("objread on a condemned store",
		objread(s, o, 1, buf, Blk, 0), w);
	refused("objwrite on a condemned store",
		objwrite(s, o, 1, buf, Blk, 0, 3, 1, nil, 0), w);
	refused("objtrunc on a condemned store",
		objtrunc(s, o, 1, 0, 3, 1, nil, 0), w);
	refused("objremove on a condemned store",
		objremove(s, o, 1, 3, 1, nil, 0), w);
	refused("objdiscard on a condemned store",
		objdiscard(s, o, 1, 2, 1, 2), w);
	refused("objcorrupt on a condemned store",
		objcorrupt(s, o, 1, 1, nil, 0), w);
	refused("stagewrite on a condemned store",
		stagewrite(g, buf, Blk, 0), w);
	refused("stagefinal on a condemned store",
		stagefinal(g, 9, 1, nil, 0), w);
	storeclose(s);
	devclose(d);
	free(buf);
}

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
		if(objcreate(s, o, strlen(name), 1, 1, nil, 0, nil) < 0){
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
	if(objremove(s, o, strlen(name), 2, 1, nil, 0) < 0)
		fail("delete under slot exhaustion: %r");
	if(objdiscard(s, o, strlen(name), 2, 1, 2) < 0)
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

	/*
	 * §14(2) folds the Edirty entries into the same record as the
	 * update they belong to, so that either both are durable or
	 * neither — and that has to hold for every mutating call, not
	 * only for objwrite.  A truncate, a delete, a heal and the
	 * corrupt flag can each leave a peer stale; committed as a
	 * second record, a crash between the two leaves the update
	 * durable and the stale mark absent, which is layer-a §5.4's
	 * `degraded' arrived at silently.
	 */
	mk(s, "dtr");
	mk(s, "drm");
	mk(s, "dck");
	mustwr(s, "dtr", buf, 64, 0);
	mustwr(s, "dck", buf, 64, 0);
	dr[0].peerlen = 7;
	memmove(dr[0].peer, "node9.3", 7);
	dr[0].oidlen = 3;
	memmove(dr[0].oid, "dtr", 3);
	oidof(oid, "dtr");
	if(objtrunc(s, oid, 3, 16, 3, 1, dr, 1) < 0)
		fail("objtrunc with a dirty record: %r");
	memmove(dr[0].oid, "drm", 3);
	oidof(oid, "drm");
	if(objremove(s, oid, 3, 3, 1, dr, 1) < 0)
		fail("objremove with a dirty record: %r");
	memmove(dr[0].oid, "dck", 3);
	oidof(oid, "dck");
	if(objcorrupt(s, oid, 3, 1, dr, 1) < 0)
		fail("objcorrupt with a dirty record: %r");
	eqv("each write-path call carried its record", dirtycount(s), 4);
	storeclose(s);
	if((s = mustopen(d, "dirty from every write path")) == nil){
		devclose(d);
		return;
	}
	eqv("and each is in the same record as its update", dirtycount(s), 4);
	oidof(oid, "dtr");
	istrue("a truncate's stale mark is durable",
		dirtyhas(s, oid, 3, "node9.3"));
	oidof(oid, "drm");
	istrue("a delete's stale mark is durable",
		dirtyhas(s, oid, 3, "node9.3"));
	oidof(oid, "dck");
	istrue("a corrupt flag's stale mark is durable",
		dirtyhas(s, oid, 3, "node9.3"));

	/*
	 * A create is replicated like any other write (layer-a §2.4), so
	 * it can leave a peer stale in exactly the same way and needs the
	 * mark in exactly the same record.  Registered afterwards it would
	 * be a second record, and a crash between the two leaves a live
	 * object here that no peer is recorded as missing.
	 */
	dr[0].oidlen = 3;
	memmove(dr[0].oid, "dcr", 3);
	oidof(oid, "dcr");
	if(objcreate(s, oid, 3, 1, 1, dr, 1, nil) < 0)
		fail("objcreate with a dirty record: %r");
	eqv("the create carried its record", dirtycount(s), 5);
	storeclose(s);
	if((s = mustopen(d, "dirty from a create")) == nil){
		devclose(d);
		return;
	}
	eqv("which is in the same record as the create", dirtycount(s), 5);
	oidof(oid, "dcr");
	istrue("a create's stale mark is durable",
		dirtyhas(s, oid, 3, "node9.3"));
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * §3.6's final=1: the arbitration comparison layer-a §5.5 makes once,
 * at commit time, against the receiver's then-current key.  Three
 * receivers have to be told apart — one holding a lower key, one
 * holding an equal key, and one holding no key at all — and the last
 * is the common case for a heal, because layer-a §1.3 makes absence
 * lose arbitration against any verifying copy.
 */
static Stage*
fullstage(Store *s, char *name, uchar *content, uvlong len, int force)
{
	Stage *g;
	uchar o[Oidmax];

	oidof(o, name);
	if((g = stageopen(s, o, strlen(name), len, force)) == nil){
		fail("stageopen %s: %r", name);
		return nil;
	}
	if(stagewrite(g, content, len, 0) < 0){
		fail("stagewrite %s: %r", name);
		stagediscard(g);
		return nil;
	}
	return g;
}

static void
tfull(void)
{
	Dev *d;
	Store *s;
	Stage *g;
	Objinfo oi;
	Dirtyrec dr;
	Storestat st;
	uchar *a, *b, *got, oid[Oidmax];

	d = newdisk();
	if((s = mustopen(d, "op=full")) == nil)
		return;
	a = mkbuf(2*Blk, 61);
	b = mkbuf(2*Blk, 67);
	if((got = malloc(2*Blk)) == nil)
		sysfatal("malloc: %r");

	/*
	 * The heal of a copy this instance does not hold.  One Eobj
	 * carries the create and the content, so there is no moment at
	 * which the object is live, empty and at the winning key.
	 */
	memset(&dr, 0, sizeof dr);
	dr.op = 1;
	dr.epoch = 9;
	dr.oidlen = 1;
	dr.oid[0] = 'f';
	dr.peerlen = 7;
	memmove(dr.peer, "node3.1", 7);
	storestat(s, &st);
	if((g = fullstage(s, "f", a, 2*Blk, 0)) != nil){
		checks++;
		if(stagefinal(g, 4, 2, &dr, 1) < 0)
			fail("op=full to an instance holding no copy: %r");
	}
	if(ostat(s, "f", &oi) < 0)
		fail("objstat f: %r");
	else{
		eqv("the heal published the sender's ver", oi.ver, 4);
		eqv("and its wepoch", oi.wepoch, 2);
		eqv("and the whole object's length", oi.len, 2*Blk);
		istrue("and it took a qid.path", oi.qidpath != 0);
	}
	rd(s, "f", got, 2*Blk, 0, "op=full to an absent object");
	checks++;
	if(memcmp(got, a, 2*Blk) != 0)
		fail("op=full to an absent object: content differs");
	mustverify(s, "f", "op=full to an absent object");
	oidof(oid, "f");
	istrue("the heal's stale mark rode in the same record",
		dirtyhas(s, oid, 1, "node3.1"));

	/*
	 * layer-a §1.3: ver starts at 1 on create and absence is not
	 * (0, 0), so a live object at version 0 is a key that cannot
	 * exist.  The comparison below does not catch it — a receiver
	 * with no key to defend skips the comparison altogether — so it
	 * is refused in its own right, against an object this store does
	 * not hold, which is exactly the case that would otherwise
	 * publish one.
	 */
	if((g = fullstage(s, "z0", b, 2*Blk, 0)) != nil)
		refused("an op=full at version 0 to an absent object",
			stagefinal(g, 0, 2, nil, 0), "bad ctl");
	checks++;
	if(ostat(s, "z0", &oi) >= 0)
		fail("an op=full at version 0 published a live (0, 0) object");
	/*
	 * The same rule on objcreate is §3.7's internal kind — a client
	 * create's version is this instance's own to choose, so a 0 is a
	 * caller bug and carries no §2.6 prefix (unlike the op=full's
	 * above, whose version arrives in a wire header).
	 */
	oidof(oid, "z1");
	refused("a create at version 0", objcreate(s, oid, 2, 0, 2, nil, 0, nil),
		"create at version 0");

	/* layer-a §5.5's comparison, made against that key */
	if((g = fullstage(s, "f", b, 2*Blk, 0)) != nil)
		refused("an op=full at a lower key", stagefinal(g, 3, 2, nil, 0),
			"stale version");
	if((g = fullstage(s, "f", b, 2*Blk, 0)) != nil)
		refused("an op=full at an equal key without force",
			stagefinal(g, 4, 2, nil, 0), "stale version");
	if(ostat(s, "f", &oi) < 0)
		fail("objstat f: %r");
	else
		eqv("a refused op=full changes nothing", oi.ver, 4);
	rd(s, "f", got, 2*Blk, 0, "after a refused op=full");
	checks++;
	if(memcmp(got, a, 2*Blk) != 0)
		fail("a refused op=full changed the content");
	storestat(s, &st);
	eqv("and it stages nothing", st.staged, 0);

	/* equal key with force=1 is §1.3's divergence repair */
	if((g = fullstage(s, "f", b, 2*Blk, 1)) != nil){
		checks++;
		if(stagefinal(g, 4, 2, nil, 0) < 0)
			fail("op=full force=1 at an equal key: %r");
	}
	rd(s, "f", got, 2*Blk, 0, "op=full force=1");
	checks++;
	if(memcmp(got, b, 2*Blk) != 0)
		fail("op=full force=1 did not replace the content");
	mustverify(s, "f", "op=full force=1");

	/* and a strictly greater key needs no flag */
	if((g = fullstage(s, "f", a, 2*Blk, 0)) != nil){
		checks++;
		if(stagefinal(g, 5, 2, nil, 0) < 0)
			fail("op=full at a greater key: %r");
	}
	if(ostat(s, "f", &oi) < 0)
		fail("objstat f: %r");
	else
		eqv("a greater key applies", oi.ver, 5);

	/*
	 * D14: a copy that fails local verification contributes no key
	 * (layer-a §1.3), so it has none to defend and takes the push at
	 * any key.  Without the exemption a holder that committed
	 * (E, ver+1) and then lost the content refuses the serving
	 * primary's repair at the lower (E, ver) — by the very copy that
	 * asked for it — and is unrepairable for the life of the disk.
	 */
	oidof(oid, "f");
	if(objcorrupt(s, oid, 1, 1, nil, 0) < 0)
		fail("objcorrupt: %r");
	if((g = fullstage(s, "f", b, 2*Blk, 0)) != nil){
		checks++;
		if(stagefinal(g, 2, 1, nil, 0) < 0)
			fail("op=full to a corrupt copy at a lower key: %r");
	}
	if(ostat(s, "f", &oi) < 0)
		fail("objstat f: %r");
	else{
		eqv("the push applied at the lower key", oi.ver, 2);
		eqv("and the flag survives it, for §8's verify to clear",
			oi.corrupt, 1);
	}
	rd(s, "f", got, 2*Blk, 0, "op=full to a corrupt copy");
	checks++;
	if(memcmp(got, b, 2*Blk) != 0)
		fail("op=full to a corrupt copy did not replace the content");
	mustverify(s, "f", "op=full to a corrupt copy");

	storeclose(s);
	if((s = mustopen(d, "op=full replayed")) == nil){
		devclose(d);
		free(a);
		free(b);
		free(got);
		return;
	}
	if(ostat(s, "f", &oi) < 0)
		fail("objstat f: %r");
	else{
		eqv("every op=full replays", oi.ver, 2);
		eqv("with its corrupt flag", oi.corrupt, 1);
	}
	rd(s, "f", got, 2*Blk, 0, "op=full replayed");
	checks++;
	if(memcmp(got, b, 2*Blk) != 0)
		fail("op=full replayed: content differs");
	mustverify(s, "f", "op=full replayed");
	storeclose(s);
	devclose(d);
	free(a);
	free(b);
	free(got);
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

	/*
	 * layer-a §2.6: access to a tombstone is `object deleted'.  The
	 * set is prefix-free by design and this is a different fact from
	 * `no such object', which is an id a completed currency check
	 * found nowhere; layer-a §5.2's machinery distinguishes them.
	 */
	oidof(oid, "t");
	refused("a read of a tombstone", objread(s, oid, 1, buf, 16, 0),
		"object deleted");
	refused("a write to a tombstone",
		objwrite(s, oid, 1, buf, 16, 0, 4, 1, nil, 0),
		"object deleted");
	refused("a truncate of a tombstone", objtrunc(s, oid, 1, 16, 4, 1, nil, 0),
		"object deleted");
	refused("a delete of a tombstone", objremove(s, oid, 1, 4, 1, nil, 0),
		"object deleted");
	refused("a read of an id nothing holds",
		objread(s, (uchar*)"zz", 2, buf, 16, 0), "no such object");

	/*
	 * layer-a §1.5: a create of a tombstoned id produces a fresh
	 * live object with ver one greater than the tombstone's, so as
	 * long as the tombstone exists no older copy can outrank the new
	 * object.  A straggler still holding this tombstone at ver 3
	 * would outrank a live object created at ver 2 and re-delete it,
	 * so the store enforces the value rather than trusting it.  The
	 * spelling is layer-a §5.5's for op=create: a create is
	 * self-contained, so the tombstone's key refusing it is `stale
	 * version' — §2.6 keeps `out of sequence' for delta ops.
	 */
	refused("a create over a tombstone at the tombstone's own ver",
		objcreate(s, oid, 1, 3, 1, nil, 0, nil), "stale version");
	refused("a create over a tombstone below its ver",
		objcreate(s, oid, 1, 2, 1, nil, 0, nil), "stale version");
	refused("a create over a tombstone two above its ver",
		objcreate(s, oid, 1, 5, 1, nil, 0, nil), "stale version");
	refused("a create over a tombstone below its wepoch",
		objcreate(s, oid, 1, 4, 0, nil, 0, nil), "stale version");
	if(ostat(s, "t", &oi2) < 0)
		fail("objstat: %r");
	eqv("a refused create leaves the tombstone a tombstone", oi2.state,
		Stomb);

	if(objcreate(s, oid, 1, 4, 1, nil, 0, &oi2) < 0)
		fail("create over a tombstone: %r");
	eqv("the create takes the tombstone's ver plus one", oi2.ver, 4);
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
	Storestat st;
	uvlong lf;
	uchar *buf, oid[Oidmax];

	d = newdisk();
	if((s = mustopen(d, "corrupt flag")) == nil)
		return;
	buf = mkbuf(16*Blk, 59);
	mk(s, "c");
	mustwr(s, "c", buf, 16*Blk, 0);
	oidof(oid, "c");
	storestat(s, &st);
	lf = st.logfree;
	if(objcorrupt(s, oid, 1, 1, nil, 0) < 0)
		fail("objcorrupt: %r");
	/*
	 * §2.7's slot rule is what makes a commit name every block, and
	 * this one changes no emapslot: an empty nmap leaves the map it
	 * inherits exactly as it is, so §8's "an Eobj that changes
	 * nothing but the corrupt flag" is one sector however large the
	 * object.
	 */
	storestat(s, &st);
	eqv("a commit that changes nothing but the corrupt flag is one "
		"sector", lf - st.logfree, 1);
	if(objstat(s, oid, 1, &oi) < 0)
		fail("objstat: %r");
	eqv("the corrupt flag is set", oi.corrupt, 1);
	eqv("and nothing else changed", oi.len, 16*Blk);
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
	if(objcorrupt(s, oid, 1, 0, nil, 0) < 0)
		fail("objcorrupt clear: %r");
	if(objstat(s, oid, 1, &oi) < 0)
		fail("objstat: %r");
	eqv("and a repair clears it", oi.corrupt, 0);
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * Every offset and length bound the object and the stage APIs make,
 * tested where a bound written as a sum stops being one.  off is a
 * client's or a peer's u64: off+n wraps at 2^64-4, so a sum admits
 * the call and what follows is a block index of 4.5e15 — an
 * out-of-range grain-array index in the stage, and on the write path
 * a record whose range checks can only fail after it is written.
 * layer-a §1.2 makes the answer normative: `object too large', and
 * never a silent truncation.
 */
static void
tbounds(void)
{
	Dev *d;
	Store *s;
	Stage *g;
	Storestat st, st2;
	Objinfo oi;
	uchar *buf, o[Oidmax];
	uvlong objmax;

	d = newdisk();
	if((s = mustopen(d, "bounds")) == nil)
		return;
	objmax = 65536;			/* t1.h's small geometry */
	buf = mkbuf(Blk, 61);
	mk(s, "w");
	oidof(o, "w");
	storestat(s, &st);

	refused("a write of 4 bytes at 2^64-4",
		objwrite(s, o, 1, buf, 4, ~0ULL - 3, 2, 1, nil, 0),
		"object too large");
	refused("a write one byte past objmax",
		objwrite(s, o, 1, buf, 1, objmax, 2, 1, nil, 0),
		"object too large");
	refused("a write straddling objmax",
		objwrite(s, o, 1, buf, 2, objmax - 1, 2, 1, nil, 0),
		"object too large");
	refused("a truncate past objmax",
		objtrunc(s, o, 1, objmax + 1, 2, 1, nil, 0), "object too large");
	refused("a read of a negative count",
		objread(s, o, 1, buf, -1, 0), "negative read");

	/*
	 * layer-a §1.3 forbids version 0, and every publishing path
	 * refuses it — not objcreate and stagefinal alone.  objremove is
	 * the sharp one: a delete bumps (wepoch, ver) like any write
	 * (§1.5), so a tombstone at (E, 0) would force the re-create to
	 * ver 1, and a straggler live copy at (E, 1) with different
	 * content then ties it — layer-a §1.3's I3.  The spelling is
	 * §3.7's internal kind: on these paths the version is this
	 * instance's own to choose, so a 0 is a caller bug and carries
	 * no §2.6 prefix.
	 */
	refused("a write at version 0",
		objwrite(s, o, 1, buf, 16, 0, 0, 1, nil, 0),
		"write at version 0");
	refused("a truncate at version 0",
		objtrunc(s, o, 1, 0, 0, 1, nil, 0), "truncate at version 0");
	refused("a delete at version 0",
		objremove(s, o, 1, 0, 1, nil, 0), "delete at version 0");

	/*
	 * §3.7: an oid outside layer-a §1.1's 1*128 bound is that
	 * section's `bad object name' and not a string of this store's
	 * own, because the 9P server returns what the store hands it and
	 * §2.6's set is what a client parses.
	 */
	refused("a create of a zero-length oid",
		objcreate(s, o, 0, 1, 1, nil, 0, nil), "bad object name");
	refused("a create of an oid past Oidmax",
		objcreate(s, o, Oidmax + 1, 1, 1, nil, 0, nil),
		"bad object name");
	checks++;
	if((g = stageopen(s, o, 0, Blk, 0)) != nil){
		fail("a stage of a zero-length oid was accepted");
		stagediscard(g);
	}else{
		char e[ERRMAX];

		rerrstr(e, sizeof e);
		istrue("a stage of a zero-length oid says bad object name",
			strncmp(e, "bad object name", 15) == 0);
	}
	/* ... and a discard of something that is not a tombstone */
	refused("a discard of a live object", objdiscard(s, o, 1, 2, 1, 2),
		"not discardable");

	/*
	 * The refusal is the whole of what happened: no record was
	 * written, no grain was taken and the object is as it was.
	 */
	storestat(s, &st2);
	eqv("a refused write takes no grain", st2.grainfree, st.grainfree);
	eqv("a refused write writes no record", st2.logfree, st.logfree);
	eqv("the store is not condemned", st2.nlost, 0);
	if(objstat(s, o, 1, &oi) < 0)
		fail("objstat: %r");
	eqv("and the object is untouched", oi.len, 0);

	/*
	 * A count-0 write changes nothing.  layer-a §2.4 extends at a
	 * write *at* offset > len — bytes landing above it — and 9P
	 * clients issue count-0 Twrites, so taking one as an extend would
	 * resize the object and publish a new key for a call that wrote
	 * nothing, leaving a replica that took it at a different len from
	 * one that did not, at the same key.
	 */
	checks++;
	if(objwrite(s, o, 1, buf, 0, 5000, 3, 1, nil, 0) < 0)
		fail("a count-0 write was refused: %r");
	storestat(s, &st2);
	eqv("a count-0 write writes no record", st2.logfree, st.logfree);
	if(objstat(s, o, 1, &oi) < 0)
		fail("objstat: %r");
	else{
		eqv("a count-0 write past len does not extend", oi.len, 0);
		eqv("and publishes no key", oi.ver, 1);
	}
	refused("a count-0 write past objmax",
		objwrite(s, o, 1, buf, 0, objmax + 1, 3, 1, nil, 0),
		"object too large");

	/* the last byte objmax admits is still an ordinary write */
	checks++;
	if(objwrite(s, o, 1, buf, 1, objmax - 1, 2, 1, nil, 0) < 0)
		fail("a write of the last byte objmax admits: %r");
	mustverify(s, "w", "after the refusals");

	/* §3.6's chunk bound, from the same u64 on a /repl fid */
	if((g = stageopen(s, o, 1, 2*Blk, 0)) == nil)
		fail("stageopen: %r");
	else{
		refused("a chunk of 4 bytes at 2^64-4",
			stagewrite(g, buf, 4, ~0ULL - 3), "bad ctl");
		refused("a chunk of a negative count",
			stagewrite(g, buf, -1, 0), "negative chunk");
		refused("a chunk one byte past the declared length",
			stagewrite(g, buf, 1, 2*Blk), "bad ctl");
		storestat(s, &st2);
		eqv("a refused chunk stages nothing", st2.staged, 0);
		stagediscard(g);
	}
	checks++;
	if((g = stageopen(s, o, 1, objmax + 1, 0)) != nil){
		fail("a stage longer than objmax was accepted");
		stagediscard(g);
	}
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
	tstagefault();
	tsweepfinal();
	tdiscard();
	tcondemned();
	texhaust();
	tdirty();
	tfull();
	ttomb();
	tcorrupt();
	tbounds();
	if(fails > 0){
		fprint(2, "objtest: %d of %d checks failed\n", fails, checks);
		exits("failed");
	}
	print("objtest: %d checks ok\n", checks);
	exits(nil);
}
