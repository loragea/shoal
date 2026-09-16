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
	uchar *buf;

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
	checks++;
	if(drop(s, "gonesoon") >= 0)
		fail("objdrop of a tombstoned id was taken");
	else
		errsays("objdrop of a tombstoned id", "object deleted");
	if(ostat(s, "gonesoon", &oi) < 0)
		fail("the refused drop removed the tombstone: %r");

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

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */

void
main(int argc, char **argv)
{
	int i;

	USED(argc); USED(argv);
	csumdigests(nil, 0, emptycsum);

	tadoptabsent();
	tadoptover();
	tdrop(0);
	tdrop(1);
	tdropedges();
	for(i = Cwrite; i <= Cadopt; i++)
		tcsum(i);

	killspawned();
	if(fails > 0){
		print("peeropstest: %d of %d checks FAILED\n", fails, checks);
		exits("failed");
	}
	print("peeropstest: %d checks ok\n", checks);
	exits(nil);
}
