#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"
#include "t1.h"

/*
 * T1 for the engine calls layer-a §5.5/§5.6's peer channels need and
 * the store did not have: tombstone adoption (§1.5), op=drop (§5.6,
 * §7.4), the resulting-csum check §5.5 makes the receiver's duty, and
 * the oid-ordered op=list enumeration.  docs/design/store.md §13's
 * T1.30 to T1.33.
 */

enum
{
	Blk	= 4096,
};

static uchar emptycsum[Csumlen];

/* ------------------------------------------------------------------ */

static void
errsays(char *what, char *pfx)
{
	char e[ERRMAX];

	rerrstr(e, sizeof e);
	checks++;
	if(strncmp(e, pfx, strlen(pfx)) != 0)
		fail("%s: error `%s', wanted `%s'", what, e, pfx);
}

/*
 * §3.7's normative carve-out: an internal-invariant error MUST NOT
 * begin with one of layer-a §2.6's prefixes, because that set is
 * prefix-free and a client parsing one out of a bug would read it as
 * an ordinary refusal.
 */
static void
errnotwire(char *what)
{
	static char *w[] = {
		"no such object", "object deleted", "object exists",
		"bad object name", "object too large", "stale version",
		"bad ctl", "checksum mismatch", "not discardable",
		"disk full", "not ready", "permission denied",
		"still placed", "bad map", "stale epoch", "future epoch",
		"fenced", nil,
	};
	char e[ERRMAX];
	int i;

	rerrstr(e, sizeof e);
	checks++;
	for(i = 0; w[i] != nil; i++)
		if(strncmp(e, w[i], strlen(w[i])) == 0){
			fail("%s: `%s' begins with a layer-a 2.6 prefix, `%s'",
				what, e, w[i]);
			return;
		}
}

/* layer-a §1.1's byte order, computed here rather than borrowed */
static int
oidcmptest(uchar *a, int na, uchar *b, int nb)
{
	int n, c;

	n = na < nb ? na : nb;
	if(n > 0 && (c = memcmp(a, b, n)) != 0)
		return c;
	if(na == nb)
		return 0;
	return na < nb ? -1 : 1;
}

static int
ostat(Store *s, char *name, Objinfo *oi)
{
	uchar o[Oidmax];

	oidof(o, name);
	return objstat(s, o, strlen(name), oi);
}

static void
mk(Store *s, char *name)
{
	uchar o[Oidmax];

	oidof(o, name);
	if(objcreate(s, o, strlen(name), 1, 1, nil, 0, nil) < 0)
		fail("objcreate %s: %r", name);
}

static int
wr(Store *s, char *name, void *a, long n, uvlong off, uvlong ver)
{
	uchar o[Oidmax];

	oidof(o, name);
	return objwrite(s, o, strlen(name), a, n, off, ver, 1, nil, 0);
}

static int
rmv(Store *s, char *name, uvlong ver)
{
	uchar o[Oidmax];

	oidof(o, name);
	return objremove(s, o, strlen(name), ver, 1, nil, 0);
}

static int
adopt(Store *s, char *name, uvlong ver, uvlong we)
{
	uchar o[Oidmax];

	oidof(o, name);
	return objadopt(s, o, strlen(name), ver, we, nil, 0);
}

static int
drop(Store *s, char *name)
{
	uchar o[Oidmax];

	oidof(o, name);
	return objdrop(s, o, strlen(name));
}

static Store*
restart(Store *s, Dev *d, char *what)
{
	storeclose(s);
	return mustopen(d, what);
}

/* ------------------------------------------------------------------ */

/*
 * T1.30, adoption.  An id this instance holds no record of becomes a
 * tombstone at the key that arrived, with len 0 and the csum layer-a
 * §1.4 gives a zero-length object, and it survives a restart.
 */
static void
tadoptabsent(void)
{
	Dev *d;
	Store *s;
	Objinfo oi, oi2;
	Storestat st0, st1;
	uchar oid[Oidmax];
	int oidlen;
	ulong slot;

	spawnforget();
	d = newdisk();
	if((s = mustopen(d, "adopt absent")) == nil){
		devclose(d);
		return;
	}
	storestat(s, &st0);
	if(adopt(s, "ghost", 5, 3) < 0)
		fail("objadopt of an absent id: %r");
	if(ostat(s, "ghost", &oi) < 0){
		fail("objstat after adopting an absent id: %r");
		goto out;
	}
	eqv("an adopted tombstone is state tomb", oi.state, Stomb);
	eqv("an adopted tombstone has len 0", oi.len, 0);
	eqv("an adopted tombstone keeps the sender's ver", oi.ver, 5);
	eqv("an adopted tombstone keeps the sender's wepoch", oi.wepoch, 3);
	checks++;
	if(memcmp(oi.csum, emptycsum, Csumlen) != 0)
		fail("an adopted tombstone does not carry §1.4's "
			"zero-length csum");
	istrue("an adopted tombstone has a qid.path", oi.qidpath != 0);
	eqv("an adopted tombstone holds no extent-map slot", oi.emapslot, 0);

	/* visible through §8's slot cursor, which is what a scrub walks */
	slot = oi.slot;
	if(objslot(s, slot, oid, &oidlen, &oi2) != 1)
		fail("objslot of an adopted tombstone's slot: %r");
	else{
		eqv("objslot answers the adopted oid's length", oidlen, 5);
		checks++;
		if(memcmp(oid, "ghost", 5) != 0)
			fail("objslot answers another oid for the adopted slot");
		eqv("objslot agrees on the state", oi2.state, Stomb);
	}
	storestat(s, &st1);
	eqv("adopting an absent id takes one index slot",
		st0.slotfree - st1.slotfree, 1);
	eqv("adopting an absent id takes no grain", st0.grainfree,
		st1.grainfree);
	eqv("adopting an absent id makes a tombstone", st1.ntomb, 1);

	if((s = restart(s, d, "adopt absent, replayed")) == nil){
		devclose(d);
		return;
	}
	if(ostat(s, "ghost", &oi2) < 0){
		fail("objstat after a restart: %r");
		goto out;
	}
	eqv("the adopted tombstone survives a restart", oi2.state, Stomb);
	eqv("... at its len", oi2.len, 0);
	eqv("... at its ver", oi2.ver, 5);
	eqv("... at its wepoch", oi2.wepoch, 3);
	eqv("... at its qid.path", oi2.qidpath, oi.qidpath);
	checks++;
	if(memcmp(oi2.csum, emptycsum, Csumlen) != 0)
		fail("the adopted tombstone's csum does not survive a restart");
out:
	storeclose(s);
	devclose(d);
	killspawned();
}

/*
 * T1.30 continued: a lower-keyed tombstone is re-keyed in place, a
 * live copy is refused with an error carrying no §2.6 prefix, and a
 * version of 0 is `bad ctl' because the key came from elsewhere.
 */
static void
tadoptover(void)
{
	Dev *d;
	Store *s;
	Objinfo oi, tomb, live;
	Storestat st0, st1;
	uchar o[Oidmax], *buf;

	spawnforget();
	d = newdisk();
	if((s = mustopen(d, "adopt over")) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(2*Blk, 7);

	/* a tombstone of this instance's own making, at (1, 2) */
	mk(s, "old");
	if(rmv(s, "old", 2) < 0)
		fail("objremove: %r");
	if(ostat(s, "old", &tomb) < 0)
		fail("objstat of the tombstone: %r");

	if(adopt(s, "old", 9, 4) < 0)
		fail("objadopt over a tombstone: %r");
	if(ostat(s, "old", &oi) < 0)
		fail("objstat after re-keying: %r");
	else{
		eqv("a re-keyed tombstone is still a tombstone", oi.state, Stomb);
		eqv("a re-keyed tombstone takes the new ver", oi.ver, 9);
		eqv("a re-keyed tombstone takes the new wepoch", oi.wepoch, 4);
		eqv("a re-keyed tombstone keeps its slot", oi.slot, tomb.slot);
		eqv("a re-keyed tombstone keeps its qid.path", oi.qidpath,
			tomb.qidpath);
		eqv("a re-keyed tombstone keeps len 0", oi.len, 0);
	}

	/*
	 * layer-a §5.5's comparison, which this call makes itself over a
	 * tombstone (§3.8): only a strictly greater key applies.  The
	 * record is at (4, 9) now.
	 */
	checks++;
	if(adopt(s, "old", 8, 4) >= 0)
		fail("objadopt at a lower ver over a tombstone was taken");
	else
		errsays("objadopt at a lower ver", "stale version");
	checks++;
	if(adopt(s, "old", 99, 3) >= 0)
		fail("objadopt at a lower wepoch over a tombstone was taken");
	else
		errsays("objadopt at a lower wepoch", "stale version");
	checks++;
	if(adopt(s, "old", 9, 4) >= 0)
		fail("objadopt at the tombstone's own key was taken");
	else
		errsays("objadopt at an equal key", "stale version");
	if(ostat(s, "old", &oi) < 0)
		fail("objstat after the refused adoptions: %r");
	else{
		eqv("a refused adoption leaves the ver", oi.ver, 9);
		eqv("... and the wepoch", oi.wepoch, 4);
		eqv("... and the slot", oi.slot, tomb.slot);
	}
	/*
	 * ... and the record the refusals left is still the one §1.5's
	 * discard names, so a discard at the key the adoption published
	 * works after them.
	 */
	oidof(o, "old");
	checks++;
	if(objdiscard(s, o, 3, 9, 4, 5) < 0)
		fail("objdiscard at the re-keyed tombstone's own key: %r");
	checks++;
	if(ostat(s, "old", &oi) >= 0)
		fail("the discarded tombstone still has a record");

	/* a live copy: refused, and nothing about it moves */
	mk(s, "alive");
	if(wr(s, "alive", buf, 2*Blk, 0, 2) < 0)
		fail("objwrite: %r");
	if(ostat(s, "alive", &live) < 0)
		fail("objstat of the live copy: %r");
	storestat(s, &st0);
	checks++;
	if(adopt(s, "alive", 99, 9) >= 0)
		fail("objadopt over a live copy was taken");
	else
		errnotwire("objadopt over a live copy");
	if(ostat(s, "alive", &oi) < 0)
		fail("objstat after the refused adopt: %r");
	else{
		eqv("the refused adopt leaves the copy live", oi.state, Slive);
		eqv("... at its length", oi.len, live.len);
		eqv("... at its ver", oi.ver, live.ver);
	}
	storestat(s, &st1);
	eqv("the refused adopt frees no grain", st1.grainfree, st0.grainfree);

	/* layer-a §1.3 forbids ver 0, and §3.7 makes this one `bad ctl' */
	checks++;
	if(adopt(s, "nought", 0, 1) >= 0)
		fail("objadopt at version 0 was taken");
	else
		errsays("objadopt at version 0", "bad ctl");
	checks++;
	if(ostat(s, "nought", &oi) >= 0)
		fail("the refused adopt published a record");

	free(buf);
	storeclose(s);
	devclose(d);
	killspawned();
}

/*
 * §6's reserved log tail carries commits that release space, so that
 * a delete blocked for log space is never the commit that cannot be
 * written.  A re-keying adoption releases nothing — the record is a
 * tombstone before and after, at the same slot and len 0 — so it is
 * ordinary traffic and waits like any other, rather than spending the
 * reserve the delete below depends on.
 */
static void
tadoptresv(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Sbsel sel;
	Super sb;
	Objinfo oi;
	uchar *buf, o[Oidmax];
	uvlong resv;
	int i;

	spawnforget();
	d = newdisk();
	if((s = mustopen(d, "adoption on the reserved tail")) == nil){
		devclose(d);
		killspawned();
		return;
	}
	buf = mkbuf(64, 127);
	mk(s, "t");			/* the tombstone to re-key */
	if(rmv(s, "t", 2) < 0)
		fail("objremove t: %r");
	mk(s, "w");			/* what fills the log, and the delete */
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	sb = sel.sb[sel.start];
	resv = sb.logsecs/Logresvdiv;
	if(resv < 1)
		resv = 1;
	for(i = 0; i < 400; i++){
		storestat(s, &st);
		if(st.logfree <= resv)
			break;
		if(wr(s, "w", buf, 64, 0, 3 + i) < 0)
			break;
	}
	storestat(s, &st);
	istrue("the log is down to its reserved tail", st.logfree <= resv);

	checks++;
	if(adopt(s, "t", 9, 4) >= 0)
		fail("a re-keying adoption drew on §6's reserved tail");
	else
		errsays("an adoption on the reserved tail", "disk full");
	if(ostat(s, "t", &oi) < 0)
		fail("objstat t after the refused adoption: %r");
	else{
		eqv("the refused adoption leaves the tombstone's ver",
			oi.ver, 2);
		eqv("... and its wepoch", oi.wepoch, 1);
	}
	/* and the delete the reserve is FOR still goes through */
	oidof(o, "w");
	checks++;
	if(objremove(s, o, 1, 900, 1, nil, 0) < 0)
		fail("delete does not always work on the reserved tail: %r");

	free(buf);
	storeclose(s);
	devclose(d);
	killspawned();
}

/* ------------------------------------------------------------------ */

/*
 * T1.31, drop: grains, extent-map slot and index slot all come back
 * in one durable step, no record is left, and a crash after that
 * record is durable replays to the same freed state.
 */
static void
tdrop(int crash)
{
	Dev *d;
	Store *s;
	Objinfo oi;
	Storestat st0, st1;
	uchar *buf;
	char *what;

	what = crash ? "drop, crashed after the record landed" : "drop";
	spawnforget();
	d = newdisk();
	if((s = mustopen(d, what)) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(3*Blk, 23);
	storestat(s, &st0);

	mk(s, "stray");
	if(wr(s, "stray", buf, 3*Blk, 0, 2) < 0)
		fail("%s: objwrite: %r", what);
	if(ostat(s, "stray", &oi) < 0)
		fail("%s: objstat: %r", what);
	istrue("a three-block object holds an extent-map slot",
		oi.emapslot != 0);
	storestat(s, &st1);
	istrue("the object took grains", st1.grainfree < st0.grainfree);

	if(crash){
		/*
		 * §3.4's P4/P5: the header write has returned, so the
		 * record is durable; the post-flush never does.  Sckeep is
		 * what makes the sector that landed survive the crash.
		 */
		simcrashdead(d, 1);
		simcrashmode(d, Sckeep);
		simarm(d, "postwrite", 0);
		drop(s, "stray");	/* the flush after it cannot answer */
		storeclose(s);
		simrevive(d);
		if((s = mustopen(d, what)) == nil){
			devclose(d);
			free(buf);
			return;
		}
	}else{
		if(drop(s, "stray") < 0)
			fail("%s: objdrop: %r", what);
		if((s = restart(s, d, what)) == nil){
			devclose(d);
			free(buf);
			return;
		}
	}

	checks++;
	if(ostat(s, "stray", &oi) >= 0)
		fail("%s: the dropped object still has a record", what);
	else
		errsays("objstat of a dropped id", "no such object");
	storestat(s, &st1);
	eqv("a drop returns every grain", st1.grainfree, st0.grainfree);
	eqv("a drop returns the index slot", st1.slotfree, st0.slotfree);
	eqv("a drop returns the extent-map slot", st1.emapfree, st0.emapfree);
	eqv("a drop leaves no live object", st1.nlive, st0.nlive);
	eqv("a drop leaves no tombstone", st1.ntomb, st0.ntomb);
	eqv("a drop leaks no grain", st1.grainleak, st0.grainleak);

	free(buf);
	storeclose(s);
	devclose(d);
	killspawned();
}

/*
 * The two ids a drop is not for, and the one flag it ignores.
 */
static void
tdropedges(void)
{
	Dev *d;
	Store *s;
	Objinfo oi;
	Storestat st0, st1;
	uchar o[Oidmax], *buf;

	spawnforget();
	d = newdisk();
	if((s = mustopen(d, "drop edges")) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(Blk, 31);

	checks++;
	if(drop(s, "nothing") >= 0)
		fail("objdrop of an absent id was taken");
	else
		errsays("objdrop of an absent id", "no such object");

	mk(s, "gonesoon");
	if(rmv(s, "gonesoon", 2) < 0)
		fail("objremove: %r");
	/*
	 * layer-a §5.6's op=drop table and §2.5's drop verb allow no
	 * `object deleted': a tombstone is a record and not a copy, so
	 * there is nothing for a drop to remove (§3.7, §3.8).
	 */
	checks++;
	if(drop(s, "gonesoon") >= 0)
		fail("objdrop of a tombstoned id was taken");
	else
		errsays("objdrop of a tombstoned id", "no such object");
	if(ostat(s, "gonesoon", &oi) < 0)
		fail("the refused drop removed the tombstone: %r");
	else
		eqv("the refused drop leaves the tombstone", oi.state, Stomb);

	/*
	 * §8's flag does not defend a stray: the copy contributes no key
	 * (layer-a §1.3), so there is nothing here for the flag to hold
	 * on to, and a stray that could not be dropped would keep its
	 * grains for the life of the disk.
	 */
	storestat(s, &st0);
	mk(s, "flagged");
	if(wr(s, "flagged", buf, Blk, 0, 2) < 0)
		fail("objwrite: %r");
	oidof(o, "flagged");
	if(objcorrupt(s, o, 7, 1, nil, 0) < 0)
		fail("objcorrupt: %r");
	if(drop(s, "flagged") < 0)
		fail("objdrop of a corrupt-flagged copy: %r");
	storestat(s, &st1);
	eqv("dropping a corrupt-flagged copy returns its grains",
		st1.grainfree, st0.grainfree);
	eqv("... and its index slot", st1.slotfree, st0.slotfree);
	eqv("... and takes it out of /lost", st1.nlost, st0.nlost);

	free(buf);
	storeclose(s);
	devclose(d);
	killspawned();
}

/* ------------------------------------------------------------------ */

/*
 * T1.32, the resulting-csum check.  Each case is run twice over two
 * identical stores: the first learns the csum the operation produces,
 * the second is offered a wrong one — which must publish nothing at
 * all, shown by the log's own watermark and by a restart — and then
 * the right one, which must commit.
 */
enum
{
	Cwrite	= 0,
	Ctrunc,
	Cdelete,
	Ccreate,
	Cfull,
	Cadopt,
};

static char *cname[] = {
	"objwrite", "objtrunc", "objremove", "objcreate", "op=full",
	"objadopt",
};

/* everything the case needs in place before the operation under test */
static void
csumsetup(Store *s, int kind, uchar *buf)
{
	switch(kind){
	case Cwrite:
	case Ctrunc:
	case Cdelete:
		mk(s, "sub");
		if(wr(s, "sub", buf, 2*Blk, 0, 2) < 0)
			fail("%s: setup objwrite: %r", cname[kind]);
		break;
	case Ccreate:
	case Cfull:
	case Cadopt:
		break;
	}
}

/* the operation under test, with the expected csum (nil = no check) */
static int
csumop(Store *s, int kind, uchar *buf, uchar *csum)
{
	Stage *g;
	uchar o[Oidmax];

	switch(kind){
	case Cwrite:
		oidof(o, "sub");
		return objwritecsum(s, o, 3, buf, Blk, 2*Blk, 3, 1, csum,
			nil, 0);
	case Ctrunc:
		oidof(o, "sub");
		return objtrunccsum(s, o, 3, Blk/2, 3, 1, csum, nil, 0);
	case Cdelete:
		oidof(o, "sub");
		return objremovecsum(s, o, 3, 3, 1, csum, nil, 0);
	case Ccreate:
		oidof(o, "new");
		return objcreatecsum(s, o, 3, 1, 1, csum, nil, 0, nil);
	case Cfull:
		oidof(o, "new");
		if((g = stageopen(s, o, 3, 2*Blk, 0)) == nil){
			fail("op=full: stageopen: %r");
			return -1;
		}
		if(stagewrite(g, buf, 2*Blk, 0) < 0){
			fail("op=full: stagewrite: %r");
			stagediscard(g);
			return -1;
		}
		/* stagefinalcsum consumes the handle on every outcome */
		return stagefinalcsum(g, 7, 2, csum, nil, 0);
	case Cadopt:
		oidof(o, "new");
		return objadoptcsum(s, o, 3, 4, 2, csum, nil, 0);
	}
	return -1;
}

/* the object the case ends on */
static char*
csumobj(int kind)
{
	return kind == Cwrite || kind == Ctrunc || kind == Cdelete
		? "sub" : "new";
}

static void
tcsum(int kind)
{
	Dev *d;
	Store *s;
	Objinfo want, oi;
	Storestat st0, st1;
	uchar *buf, bad[Csumlen];
	char *what;

	what = cname[kind];

	/* pass one: what does the operation produce? */
	spawnforget();
	d = newdisk();
	if((s = mustopen(d, what)) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(2*Blk, 41 + kind);
	csumsetup(s, kind, buf);
	if(csumop(s, kind, buf, nil) < 0){
		fail("%s: the unchecked operation: %r", what);
		storeclose(s);
		devclose(d);
		free(buf);
		return;
	}
	if(ostat(s, csumobj(kind), &want) < 0)
		fail("%s: objstat after the unchecked operation: %r", what);
	storeclose(s);
	devclose(d);

	/* pass two: the same store, offered a wrong csum and then a right one */
	spawnforget();
	d = newdisk();
	if((s = mustopen(d, what)) == nil){
		devclose(d);
		free(buf);
		return;
	}
	csumsetup(s, kind, buf);
	if(ostat(s, csumobj(kind), &oi) < 0)
		memset(&oi, 0, sizeof oi);
	storestat(s, &st0);
	memset(bad, 0xa5, sizeof bad);
	checks++;
	if(csumop(s, kind, buf, bad) >= 0)
		fail("%s: a wrong expected csum was taken", what);
	else
		errsays(what, "checksum mismatch");
	/*
	 * Nothing durable: the log's next sequence number has not moved,
	 * so no record was written — the check that a later objstat
	 * alone could not make, since an in-memory refusal and a durable
	 * record followed by an in-memory refusal look the same there.
	 */
	storestat(s, &st1);
	eqv("a failed csum check writes no log record", st1.seqnext,
		st0.seqnext);
	eqv("... and moves no watermark", st1.watermark, st0.watermark);
	if((s = restart(s, d, what)) == nil){
		devclose(d);
		free(buf);
		return;
	}
	if(kind == Ccreate || kind == Cadopt || kind == Cfull){
		checks++;
		if(ostat(s, "new", &oi) >= 0)
			fail("%s: the refused operation is durable", what);
	}else if(ostat(s, "sub", &oi) < 0)
		fail("%s: objstat after the replay: %r", what);
	else{
		eqv("a refused operation leaves the len alone", oi.len, 2*Blk);
		eqv("... and the ver", oi.ver, 2);
		eqv("... and the state", oi.state, Slive);
	}

	/* and the right csum commits */
	if(csumop(s, kind, buf, want.csum) < 0)
		fail("%s: the right expected csum was refused: %r", what);
	else if(ostat(s, csumobj(kind), &oi) < 0)
		fail("%s: objstat after the checked operation: %r", what);
	else{
		eqv("a checked operation commits the same len", oi.len,
			want.len);
		eqv("... the same ver", oi.ver, want.ver);
		eqv("... the same state", oi.state, want.state);
		checks++;
		if(memcmp(oi.csum, want.csum, Csumlen) != 0)
			fail("%s: the checked operation published another csum",
				what);
	}

	free(buf);
	storeclose(s);
	devclose(d);
	killspawned();
}

/*
 * T1.32's exception (D23, §3.8): a replicated write of count 0
 * commits nothing and adopts no key, and the resulting-csum check
 * still runs — against the csum the object already carries, since
 * that is the csum such a write results in.
 */
static void
tcsumzero(void)
{
	Dev *d;
	Store *s;
	Objinfo oi, oi2;
	Storestat st0, st1;
	uchar *buf, bad[Csumlen], o[Oidmax];

	spawnforget();
	d = newdisk();
	if((s = mustopen(d, "count-0 csum")) == nil){
		devclose(d);
		killspawned();
		return;
	}
	buf = mkbuf(2*Blk, 53);
	mk(s, "z");
	if(wr(s, "z", buf, 2*Blk, 0, 2) < 0)
		fail("count-0 csum: setup objwrite: %r");
	if(ostat(s, "z", &oi) < 0){
		fail("count-0 csum: objstat: %r");
		goto out;
	}
	oidof(o, "z");
	storestat(s, &st0);

	memset(bad, 0xa5, sizeof bad);
	checks++;
	if(objwritecsum(s, o, 1, buf, 0, 0, 9, 4, bad, nil, 0) >= 0)
		fail("a count-0 write took a wrong expected csum");
	else
		errsays("a count-0 write at a wrong csum",
			"checksum mismatch");
	checks++;
	if(objwritecsum(s, o, 1, buf, 0, 0, 9, 4, oi.csum, nil, 0) < 0)
		fail("a count-0 write at the right csum was refused: %r");

	/* neither of them wrote a record: §3.8's "commits nothing" */
	storestat(s, &st1);
	eqv("a count-0 write commits no log record", st1.seqnext,
		st0.seqnext);
	eqv("... and moves no watermark", st1.watermark, st0.watermark);
	if(ostat(s, "z", &oi2) < 0)
		fail("count-0 csum: objstat after: %r");
	else{
		eqv("a count-0 write adopts no ver", oi2.ver, oi.ver);
		eqv("... and no wepoch", oi2.wepoch, oi.wepoch);
		eqv("... and leaves the len alone", oi2.len, oi.len);
	}
out:
	free(buf);
	storeclose(s);
	devclose(d);
	killspawned();
}

/* ------------------------------------------------------------------ */

/*
 * T1.33, op=list.  The inventory below is chosen for layer-a §1.1's
 * byte order over mixed lengths: "a" is a prefix of four of the
 * others and sorts before them, and '-' (0x2d), '.' (0x2e), '_'
 * (0x5f) and the letters straddle each other in a way a comparison
 * that ignored length or compared signed chars would get wrong.
 */
static char *inv[] = {
	"a", "a-b", "a.b", "a_b", "aa", "ab", "b", "ba", "z",
};
enum { Ninv = 9 };

static Objent*
newpage(int k)
{
	Objent *e;

	if((e = malloc(k*sizeof *e)) == nil)
		sysfatal("malloc: %r");
	return e;
}

static int
sameoid(Objent *e, char *name)
{
	return e->oidlen == (int)strlen(name)
		&& memcmp(e->oid, name, e->oidlen) == 0;
}

static void
fillinv(Store *s)
{
	int i;

	/* created out of order, so nothing about slot order helps */
	for(i = Ninv - 1; i >= 0; i--)
		mk(s, inv[i]);
	/* two of them are tombstones: op=list pages live and tomb alike */
	if(rmv(s, "ab", 2) < 0)
		fail("objremove ab: %r");
	if(rmv(s, "b", 2) < 0)
		fail("objremove b: %r");
}

static void
tlist(void)
{
	Dev *d;
	Store *s;
	Objent *e;
	Objinfo oi;
	int i, n, more;

	spawnforget();
	d = newdisk();
	if((s = mustopen(d, "list")) == nil){
		devclose(d);
		return;
	}
	fillinv(s);
	e = newpage(32);

	/* the whole inventory, in order, live and tomb both present */
	n = objlist(s, nil, 0, e, 32, &more);
	eqv("op=list answers the whole inventory", n, Ninv);
	eqv("... and says nothing follows", more, 0);
	if(n == Ninv)
		for(i = 0; i < Ninv; i++){
			checks++;
			if(!sameoid(&e[i], inv[i]))
				fail("op=list entry %d is `%.*s', wanted `%s'",
					i, e[i].oidlen, (char*)e[i].oid, inv[i]);
		}
	/* the two tombstones are in it, as tombstones */
	for(i = 0; i < n; i++)
		if(sameoid(&e[i], "ab") || sameoid(&e[i], "b"))
			eqv("op=list renders a tombstone as tomb",
				e[i].oi.state, Stomb);

	/* the Objinfo is the live index's, not a guess */
	if(ostat(s, "aa", &oi) < 0)
		fail("objstat aa: %r");
	else
		for(i = 0; i < n; i++)
			if(sameoid(&e[i], "aa")){
				eqv("op=list renders the object's slot",
					e[i].oi.slot, oi.slot);
				eqv("... its qid.path", e[i].oi.qidpath,
					oi.qidpath);
				eqv("... its ver", e[i].oi.ver, oi.ver);
				checks++;
				if(memcmp(e[i].oi.csum, oi.csum, Csumlen) != 0)
					fail("op=list renders another csum");
			}

	/* k larger than the inventory is not more */
	n = objlist(s, nil, 0, e, 32, &more);
	eqv("k past the inventory answers all of it", n, Ninv);
	eqv("... with more 0", more, 0);

	/* after the last oid there is nothing */
	n = objlist(s, (uchar*)"z", 1, e, 32, &more);
	eqv("after the last oid the page is empty", n, 0);
	eqv("... and nothing follows", more, 0);

	/* k of 0 is a question about whether anything is there */
	n = objlist(s, nil, 0, e, 0, &more);
	eqv("k of 0 answers no entries", n, 0);
	eqv("... but says more follows", more, 1);

	free(e);
	storeclose(s);
	devclose(d);
	killspawned();
}

/*
 * Three pages of three, resumed by `after': no duplicates, no gaps,
 * and `more' 0 only on the last.
 */
static void
tlistpage(void)
{
	Dev *d;
	Store *s;
	Objent *e;
	uchar after[Oidmax];
	int afterlen, i, j, n, more, got;

	spawnforget();
	d = newdisk();
	if((s = mustopen(d, "list paging")) == nil){
		devclose(d);
		return;
	}
	fillinv(s);
	e = newpage(3);
	afterlen = 0;
	got = 0;
	for(i = 0; i < 3; i++){
		n = objlist(s, afterlen > 0 ? after : nil, afterlen, e, 3,
			&more);
		if(n < 0){
			fail("objlist page %d: %r", i);
			break;
		}
		eqv("a full page answers k entries", n, 3);
		eqv("more is set while inventory follows", more, i < 2);
		for(j = 0; j < n; j++){
			if(got >= Ninv){
				fail("paging answered more than the inventory");
				break;
			}
			got++;
			checks++;
			if(!sameoid(&e[j], inv[got-1]))
				fail("page %d entry %d is `%.*s', wanted `%s'",
					i, j, e[j].oidlen, (char*)e[j].oid,
					inv[got-1]);
		}
		if(n > 0){
			afterlen = e[n-1].oidlen;
			memmove(after, e[n-1].oid, afterlen);
		}
	}
	eqv("three pages of three cover the inventory once", got, Ninv);

	free(e);
	storeclose(s);
	devclose(d);
	killspawned();
}

/*
 * A create during the scan.  layer-a §5.6 tolerates an object created
 * between pages, and the chunked scan makes that true within a page
 * too: an object created into a chunk the scan has passed is missed.
 * What a page MUST still be is internally consistent, so that is what
 * is asserted here — every entry above `after', strictly ascending,
 * never repeated — under a proc creating objects beside the walk.
 *
 * NOT covered: that the scan releases the state lock BETWEEN chunks
 * rather than holding it across the whole index.  The difference is
 * invisible from outside — a create that blocks on the lock and a
 * create that lands between two chunks leave the same page — and the
 * engine offers no counter or -X hook that would expose it, so no
 * check here discriminates the two.  §13's T1.33 row says so.
 */
/*
 * A listing test that means to exercise the CHUNKED scan needs more
 * than Listchunk slots: t1.h's geometry has 128, so a page over it is
 * one hold of qlstate across the whole index and nothing can move
 * under it.  This is that geometry with nslots turned up past
 * Listchunk and the image and log grown to match.
 */
enum
{
	Cnsec	= 40960,		/* 20 MiB */
	Cnslots	= 1024,			/* four chunks to a page */
};

static Dev*
churndisk(void)
{
	Dev *d;
	Super sb;
	Fmtcfg c;

	if((d = simopen(Tsecsz, Cnsec, Tseed)) == nil)
		sysfatal("simopen: %r");
	smallcfg(&c);
	c.nslots = Cnslots;
	c.logbytes = 256*1024;
	if(geometry(&sb, &c, d->size) < 0)
		sysfatal("geometry: %r");
	if(fmtstore(d, &sb) < 0)
		sysfatal("fmtstore: %r");
	return d;
}

static struct
{
	Store	*s;
	int	stop;
	int	done;			/* the proc is out of the engine */
	char	err[ERRMAX];		/* what stopped it early, if anything */
} churn;

/*
 * Creates, and drops-and-re-creates behind itself.  The second is
 * what can put one oid in a page twice — a slot released between two
 * chunks and the id re-created into a chunk the scan has not reached
 * — which the strictly-ascending check below is what catches.  Like
 * everything else here it is opportunistic: nothing synchronises the
 * drop with a chunk boundary, and the engine offers no hook that
 * would (§13's T1.33 row).
 */
static void
churnproc(void*)
{
	char name[16];
	uchar o[Oidmax];
	int i;

	for(i = 0; !churn.stop && i < 40; i++){
		snprint(name, sizeof name, "c%d", i);
		oidof(o, name);
		if(objcreate(churn.s, o, strlen(name), 1, 1, nil, 0, nil) < 0)
			goto stopped;
		if(i > 0){
			snprint(name, sizeof name, "c%d", i - 1);
			oidof(o, name);
			if(objdrop(churn.s, o, strlen(name)) < 0)
				goto stopped;
			if(objcreate(churn.s, o, strlen(name), 1, 1, nil, 0,
				nil) < 0)
				goto stopped;
		}
	}
	if(0){
stopped:
		rerrstr(churn.err, sizeof churn.err);
	}
	churn.stop = 1;
	churn.done = 1;
}

static void
tlistchurn(void)
{
	Dev *d;
	Store *s;
	Objent *e;
	uchar after[Oidmax], fo[Oidmax];
	char fname[16];
	int afterlen, i, j, n, more, pages, pass;
	int ascok, afterok, sawchurn;

	spawnforget();
	d = churndisk();
	if((s = openstoreck(d)) == nil){
		fail("list under churn: storeopen: %r");
		devclose(d);
		killspawned();
		return;
	}
	fillinv(s);
	/*
	 * Enough objects to spread the index across more than one chunk,
	 * so that a slot released under the walk and the slot its oid
	 * comes back in can fall either side of a chunk boundary.  With
	 * only the inventory above they are both in the first chunk,
	 * which one hold covers.
	 */
	for(i = 0; i < 300; i++){
		snprint(fname, sizeof fname, "f%03d", i);
		oidof(fo, fname);
		if(objcreate(s, fo, strlen(fname), 1, 1, nil, 0, nil) < 0){
			fail("filling the index: %r");
			break;
		}
	}
	e = newpage(4);
	churn.s = s;
	churn.stop = 0;
	churn.done = 0;
	churn.err[0] = 0;
	if(spawnproc(churnproc, nil) < 0){
		fail("spawnproc: %r");
		churn.stop = 1;
		churn.done = 1;
	}
	/*
	 * Page the whole inventory over and over for as long as the
	 * churn proc is in the engine, rather than once beside it: one
	 * pass costs under a millisecond and the proc's commits cost
	 * more than that each, so a single pass overlaps almost nothing.
	 * The properties are accumulated and asserted once at the end,
	 * so the count of checks this program reports does not depend on
	 * how the race fell.
	 */
	ascok = 1;
	afterok = 1;
	sawchurn = 0;
	pages = 0;
	for(pass = 0; pass < 400 && (!churn.done || pass < 20); pass++){
		afterlen = 0;
		for(i = 0; i < 400; i++){
			n = objlist(s, afterlen > 0 ? after : nil, afterlen,
				e, 4, &more);
			if(n < 0){
				fail("objlist under churn: %r");
				goto stop;
			}
			pages++;
			for(j = 0; j < n; j++){
				if(afterlen > 0
				&& oidcmptest(e[j].oid, e[j].oidlen, after,
					afterlen) <= 0)
					afterok = 0;
				if(j > 0
				&& oidcmptest(e[j].oid, e[j].oidlen,
					e[j-1].oid, e[j-1].oidlen) <= 0)
					ascok = 0;
				if(e[j].oidlen >= 2 && e[j].oid[0] == 'c')
					sawchurn = 1;
			}
			if(n == 0)
				break;
			afterlen = e[n-1].oidlen;
			memmove(after, e[n-1].oid, afterlen);
		}
	}
stop:
	churn.stop = 1;
	/*
	 * shoal.h's quiesce rule (D16): no call taking the Store may be
	 * in flight when storeclose runs, or it wakes inside freed
	 * memory.  Stopping the churn proc is not the same as waiting
	 * for it to leave the engine, and a kill at the end of the
	 * program is later still.
	 */
	while(!churn.done)
		sleep(1);
	checks++;
	if(churn.err[0] != 0)
		fail("the churn proc stopped early: %s", churn.err);
	istrue("every page under churn is above its `after'", afterok);
	istrue("every page under churn is strictly ascending", ascok);
	istrue("the walk under churn overlapped the proc", sawchurn);
	istrue("the walk under churn made progress", pages > 1);
	free(e);
	storeclose(s);
	devclose(d);
	killspawned();
}

/* ------------------------------------------------------------------ */

void
main(int argc, char **argv)
{
	int i;

	USED(argc); USED(argv);
	csumdigests(nil, 0, emptycsum);

	tadoptabsent();
	tadoptover();
	tadoptresv();
	tdrop(0);
	tdrop(1);
	tdropedges();
	for(i = Cwrite; i <= Cadopt; i++)
		tcsum(i);
	tcsumzero();
	tlist();
	tlistpage();
	tlistchurn();

	killspawned();
	if(fails > 0){
		print("peeropstest: %d of %d checks FAILED\n", fails, checks);
		exits("failed");
	}
	print("peeropstest: %d checks ok\n", checks);
	exits(nil);
}
