#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"
#include "t1.h"

/*
 * T1 for docs/design/store.md §9's snapshot-at-open enumeration: the
 * {slot, qid.path} vector a /obj, /tombs or /advert open takes, the
 * two conditions that make one of its entries gone, the bound on how
 * many may be open at once, and the /dirty and /lost copies beside
 * them.  R12, and layer-a §2.2's snapshot rules.
 *
 * §13's T1.15 is tconc below, at T1's geometry — thousands of slots
 * rather than 2.6·10^5 — and treclaim is layer-a §1.5's tombstone
 * reclaim walk over a /tombs snapshot, which is the first caller the
 * enumeration has.
 */

enum
{
	Blk	= 4096,

	/*
	 * §13's small geometry with nslots turned right down, so that a
	 * discarded slot is handed out again within a handful of
	 * creates: that is the only way to build the case where the
	 * snapshot's qid.path test — and not its state test — is what
	 * makes an entry gone.
	 */
	Tinyslots	= 24,

	/*
	 * T1.15's shape: nslots in the thousands.  A zero-length object
	 * occupies no grain (§2.3's inline map), so thousands of them
	 * need index and log space and no data region to speak of.
	 */
	Bigsecsz	= 512,
	Bignsec		= 32768,		/* a 16 MiB image */
	Bigslots	= 4096,
	Nconc		= 1000,			/* untouched at open */
	Nchurn		= 500,			/* churned under the walk */
	Nproc		= 4,
};

static Dev*
makedisk(uvlong nsec, ulong nslots, ulong ndirty, uvlong logbytes)
{
	Dev *d;
	Super s;
	Fmtcfg c;

	if((d = simopen(Bigsecsz, nsec, Tseed)) == nil)
		sysfatal("simopen: %r");
	smallcfg(&c);
	c.nslots = nslots;
	c.ndirty = ndirty;
	c.logbytes = logbytes;
	if(geometry(&s, &c, d->size) < 0)
		sysfatal("geometry: %r");
	if(fmtstore(d, &s) < 0)
		sysfatal("fmtstore: %r");
	return d;
}

static Dev*
tinydisk(void)
{
	return makedisk(Tnsec, Tinyslots, 64, 64*1024);
}

static Dev*
bigdisk(void)
{
	return makedisk(Bignsec, Bigslots, 256, 512*1024);
}

/* the current geometry, for reaching past the API at the media */
static void
geom(Dev *d, Super *sup)
{
	Sbsel sel;

	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	*sup = sel.sb[sel.start];
}

static void
mk(Store *s, char *name)
{
	uchar o[Oidmax];

	oidof(o, name);
	if(objcreate(s, o, strlen(name), 1, 1, nil, 0, nil) < 0)
		fail("objcreate %s: %r", name);
}

/* a create over the tombstone name already holds: §2.3 keeps the path */
static void
remk(Store *s, char *name, uvlong ver)
{
	uchar o[Oidmax];

	oidof(o, name);
	if(objcreate(s, o, strlen(name), ver, 1, nil, 0, nil) < 0)
		fail("objcreate %s over its tombstone: %r", name);
}

static void
rmv(Store *s, char *name, uvlong ver)
{
	uchar o[Oidmax];

	oidof(o, name);
	if(objremove(s, o, strlen(name), ver, 1, nil, 0) < 0)
		fail("objremove %s: %r", name);
}

static void
disc(Store *s, char *name, uvlong ver, uvlong epoch)
{
	uchar o[Oidmax];

	oidof(o, name);
	if(objdiscard(s, o, strlen(name), ver, 1, epoch) < 0)
		fail("objdiscard %s: %r", name);
}

static void
corrupt(Store *s, char *name, int set)
{
	uchar o[Oidmax];

	oidof(o, name);
	if(objcorrupt(s, o, strlen(name), set, nil, 0) < 0)
		fail("objcorrupt %s %d: %r", name, set);
}

static int
ostat(Store *s, char *name, Objinfo *oi)
{
	uchar o[Oidmax];

	oidof(o, name);
	return objstat(s, o, strlen(name), oi);
}

/* a call that must fail, and with the error the argument names */
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

static Objsnap*
mustsnap(Store *s, int kinds, char *what)
{
	Objsnap *sn;

	if((sn = objsnapopen(s, kinds)) == nil)
		fail("%s: objsnapopen: %r", what);
	return sn;
}

/*
 * The position of the entry naming name, and -1 if the snapshot does
 * not answer it.  A server maps a Tread offset onto a position; a
 * test that wants to mutate one entry has to find it the same way.
 */
static long
posof(Objsnap *sn, char *name)
{
	Objinfo oi;
	uchar got[Oidmax];
	ulong i;
	int oidlen, n;

	n = strlen(name);
	for(i = 0; i < objsnapcount(sn); i++){
		if(objsnapent(sn, i, got, &oidlen, &oi) != 1)
			continue;
		if(oidlen == n && memcmp(got, name, n) == 0)
			return i;
	}
	return -1;
}

/*
 * The position an entry must be at.  A lookup that fails is one
 * failed check and not the end of the test: a mutation that makes
 * every entry gone would otherwise take the checks below it out of
 * the run as well, so a run's FAIL count would understate what the
 * mutation broke.  Position 0 is always in range here — every caller
 * has opened a snapshot of several entries — so the checks below
 * still judge something.
 */
static ulong
mustpos(Objsnap *sn, char *name, char *what)
{
	long p;

	checks++;
	if((p = posof(sn, name)) < 0){
		fail("%s does not answer %s", what, name);
		return 0;
	}
	return p;
}

/*
 * One full position walk.  seen[] is indexed by the digits at the end
 * of each oid, which is how every name below is built; nother counts
 * entries whose name is not of that shape, and ngone the positions
 * that answered gone.  No entry may be answered twice, which is the
 * half of layer-a §2.2's SHOULD that a cursor gets wrong.
 */
typedef struct Walk Walk;
struct Walk
{
	uchar	*seen;
	ulong	nseen;			/* size of seen[] */
	ulong	nlive;			/* positions that answered 1 */
	ulong	ngone;
	ulong	nother;
	ulong	ndup;
	ulong	nbadidx;
};

static void
walkstep(Walk *w, char pfx, uchar *oid, int oidlen)
{
	ulong k;
	int i;

	if(oidlen < 2 || oid[0] != pfx){
		w->nother++;
		return;
	}
	k = 0;
	for(i = 1; i < oidlen; i++){
		if(oid[i] < '0' || oid[i] > '9'){
			w->nother++;
			return;
		}
		k = k*10 + (oid[i] - '0');
	}
	if(k >= w->nseen){
		w->nbadidx++;
		return;
	}
	if(w->seen[k]++ != 0)
		w->ndup++;
}

static void
walkall(Objsnap *sn, Walk *w, char pfx)
{
	Objinfo oi;
	uchar got[Oidmax];
	ulong i;
	int oidlen, r;

	memset(w->seen, 0, w->nseen);
	w->nlive = w->ngone = w->nother = w->ndup = w->nbadidx = 0;
	for(i = 0; i < objsnapcount(sn); i++){
		r = objsnapent(sn, i, got, &oidlen, &oi);
		if(r < 0){
			fail("objsnapent %lud: %r", i);
			return;
		}
		if(r == 0){
			w->ngone++;
			continue;
		}
		w->nlive++;
		walkstep(w, pfx, got, oidlen);
	}
}

static Walk*
newwalk(ulong n)
{
	Walk *w;

	if((w = mallocz(sizeof *w, 1)) == nil)
		sysfatal("mallocz: %r");
	if((w->seen = mallocz(n, 1)) == nil)
		sysfatal("mallocz: %r");
	w->nseen = n;
	return w;
}

static void
walkfree(Walk *w)
{
	free(w->seen);
	free(w);
}

/*
 * §9's /obj snapshot: live entries only, answered by position, each
 * exactly once.  The four ways an entry stops being answerable are
 * here — created after the open, deleted after it, discarded after
 * it, and its slot handed to another object — and the last two are
 * the two halves of the gone rule, one each.
 */
static void
tobj(void)
{
	Dev *d;
	Store *s;
	Objsnap *sn;
	Objinfo oi, oi5;
	Walk *w;
	uchar got[Oidmax];
	char nm[32];
	long p2, p5;
	ulong i;
	int oidlen, found;

	d = tinydisk();
	if((s = mustopen(d, "the /obj snapshot")) == nil)
		return;
	/*
	 * Tombstones among the live objects, so that a position in the
	 * snapshot is not the slot at that index: what the vector holds
	 * is the slot, and rendering entry i from the live index by
	 * position is §13's named mutation.
	 */
	for(i = 0; i < 10; i++){
		snprint(nm, sizeof nm, "o%lud", i);
		mk(s, nm);
		if(i < 5){
			snprint(nm, sizeof nm, "t%lud", i);
			mk(s, nm);
			rmv(s, nm, 2);
		}
	}

	if((sn = mustsnap(s, Snaplive, "/obj")) == nil)
		goto out;
	eqv("/obj lists every live object and no tombstone",
		objsnapcount(sn), 10);
	w = newwalk(16);
	walkall(sn, w, 'o');
	eqv("every entry present at open is answered", w->nlive, 10);
	eqv("and none of them twice", w->ndup, 0);
	eqv("and nothing else is answered", w->nother, 0);
	eqv("and none is gone yet", w->ngone, 0);
	for(i = 0; i < 10; i++)
		eqv("each live object exactly once", w->seen[i], 1);

	/* an object created after the open is not in the vector at all */
	mk(s, "o11");
	eqv("the count is the open-time count", objsnapcount(sn), 10);
	walkall(sn, w, 'o');
	eqv("an object created after the open is absent", w->seen[11], 0);
	eqv("and the walk still answers ten", w->nlive, 10);

	/*
	 * Deleted after the open.  §2.3 keeps the qid.path across the
	 * delete, so the state half of the gone rule is the whole of
	 * what makes this entry gone — and layer-a §2.2 says /obj MUST
	 * NOT list a tombstone.
	 */
	p2 = mustpos(sn, "o2", "/obj");
	if(ostat(s, "o2", &oi) < 0)
		fail("objstat o2: %r");
	rmv(s, "o2", 2);
	eqv("a delete after the open answers gone",
		objsnapent(sn, p2, got, &oidlen, &oi5), 0);
	if(ostat(s, "o2", &oi5) < 0)
		fail("objstat o2 after the delete: %r");
	eqv("though the delete kept its qid.path (§2.3)", oi5.qidpath,
		oi.qidpath);
	eqv("and the count has not moved", objsnapcount(sn), 10);

	/*
	 * Discarded after the open, and the slot handed to a different
	 * object.  The entry is live again and in the snapshot's kinds,
	 * so only the qid.path half can make it gone.
	 */
	p5 = mustpos(sn, "o5", "/obj");
	if(ostat(s, "o5", &oi5) < 0)
		fail("objstat o5: %r");
	rmv(s, "o5", 2);
	disc(s, "o5", 2, 2);
	eqv("a discard after the open answers gone",
		objsnapent(sn, p5, got, &oidlen, &oi), 0);
	found = 0;
	for(i = 0; i < Tinyslots && !found; i++){
		snprint(nm, sizeof nm, "n%lud", i);
		mk(s, nm);
		if(ostat(s, nm, &oi) < 0){
			fail("objstat %s: %r", nm);
			break;
		}
		if(oi.slot == oi5.slot)
			found = 1;
	}
	istrue("a discarded slot is handed out again", found);
	if(found){
		istrue("to an object with a fresh qid.path (§2.3)",
			oi.qidpath != oi5.qidpath);
		eqv("and the snapshot still answers that entry gone",
			objsnapent(sn, p5, got, &oidlen, &oi), 0);
	}
	eqv("the count is the open-time count throughout",
		objsnapcount(sn), 10);
	walkall(sn, w, 'o');
	eqv("the two gone entries are the only ones gone", w->ngone, 2);
	eqv("and nothing new has crept into the walk", w->nother, 0);
	eqv("and still nothing is answered twice", w->ndup, 0);
	walkfree(w);
	objsnapclose(sn);
out:
	storeclose(s);
	devclose(d);
}

/*
 * §9's /tombs snapshot, the mirror.  The case this file exists to
 * pin down is the create over a tombstone: §2.3 keeps the qid.path,
 * so an entry that matches on qid.path exactly is nonetheless not a
 * tombstone any more and /tombs must not answer it.
 */
static void
ttombs(void)
{
	Dev *d;
	Store *s;
	Objsnap *sn;
	Objinfo oi, before;
	Walk *w;
	uchar got[Oidmax];
	char nm[32];
	long p1, p4;
	ulong i;
	int oidlen;

	d = newdisk();
	if((s = mustopen(d, "the /tombs snapshot")) == nil)
		return;
	/* live objects among the tombstones, so a position is not a slot */
	for(i = 0; i < 6; i++){
		snprint(nm, sizeof nm, "t%lud", i);
		mk(s, nm);
		rmv(s, nm, 2);
		if(i < 3){
			snprint(nm, sizeof nm, "l%lud", i);
			mk(s, nm);
		}
	}
	if((sn = mustsnap(s, Snaptomb, "/tombs")) == nil)
		goto out;
	eqv("/tombs lists every tombstone and no live object",
		objsnapcount(sn), 6);
	w = newwalk(8);
	walkall(sn, w, 't');
	eqv("every tombstone is answered", w->nlive, 6);
	eqv("and none of them twice", w->ndup, 0);
	eqv("and no live object is", w->nother, 0);

	/* a tombstone made after the open is not in the vector */
	rmv(s, "l0", 2);
	eqv("the count is the open-time count", objsnapcount(sn), 6);
	walkall(sn, w, 't');
	eqv("a tombstone made after the open is absent", w->nlive, 6);

	/* created over after the open: same qid.path, no longer a tomb */
	p1 = mustpos(sn, "t1", "/tombs");
	if(ostat(s, "t1", &before) < 0)
		fail("objstat t1: %r");
	remk(s, "t1", 3);
	if(ostat(s, "t1", &oi) < 0)
		fail("objstat t1 after the create: %r");
	eqv("a create over a tombstone keeps the slot", oi.slot, before.slot);
	eqv("and its qid.path (§2.3)", oi.qidpath, before.qidpath);
	eqv("and /tombs answers that entry gone",
		objsnapent(sn, p1, got, &oidlen, &oi), 0);

	/* discarded after the open: the qid.path half */
	p4 = mustpos(sn, "t4", "/tombs");
	disc(s, "t4", 2, 2);
	eqv("a discard after the open answers gone",
		objsnapent(sn, p4, got, &oidlen, &oi), 0);
	walkall(sn, w, 't');
	eqv("and those two are the only ones gone", w->ngone, 2);
	eqv("the count never moved", objsnapcount(sn), 6);
	walkfree(w);
	objsnapclose(sn);
out:
	storeclose(s);
	devclose(d);
}

/*
 * §9's /advert snapshot asks for both kinds, so the state half of the
 * gone rule stops discriminating a delete: layer-a §7.2's advert line
 * carries state=live|tomb and a tombstone belongs in it.  A discard
 * still makes the entry gone, because there is no record left.
 */
static void
tadvert(void)
{
	Dev *d;
	Store *s;
	Objsnap *sn;
	Objinfo oi;
	uchar got[Oidmax];
	char nm[32];
	long pa, pb;
	ulong i;
	int oidlen;

	d = newdisk();
	if((s = mustopen(d, "the /advert snapshot")) == nil)
		return;
	/*
	 * /advert asks for both kinds, so every occupied slot is in it:
	 * a freed slot below them is what makes a position differ from a
	 * slot here.  The gap is discarded LAST, after the entries above
	 * it exist — a slot freed before them is simply handed back to
	 * the next create (alloc.c's cursor follows the free), which
	 * would leave every position equal to its slot and this test
	 * blind to a snapshot rendered by position.
	 */
	mk(s, "gap");
	for(i = 0; i < 5; i++){
		snprint(nm, sizeof nm, "a%lud", i);
		mk(s, nm);
	}
	for(i = 0; i < 3; i++){
		snprint(nm, sizeof nm, "b%lud", i);
		mk(s, nm);
		rmv(s, nm, 2);
	}
	rmv(s, "gap", 2);
	disc(s, "gap", 2, 2);
	if((sn = mustsnap(s, Snapboth, "/advert")) == nil)
		goto out;
	eqv("/advert lists live objects and tombstones alike",
		objsnapcount(sn), 8);
	pa = mustpos(sn, "a0", "/advert");
	pb = mustpos(sn, "b0", "/advert");
	/*
	 * Every entry the snapshot names sits a slot above its position,
	 * because the discarded gap is below them all: a snapshot
	 * rendered from the live index by position answers the wrong
	 * object for every one of them.
	 */
	if(objsnapent(sn, 0, got, &oidlen, &oi) != 1)
		fail("/advert does not answer its first entry: %r");
	else
		eqv("no position in this snapshot is its own slot", oi.slot, 1);
	rmv(s, "a0", 2);
	eqv("a delete after the open is still in kinds, so not gone",
		objsnapent(sn, pa, got, &oidlen, &oi), 1);
	eqv("and answers as the tombstone it now is", oi.state, Stomb);
	disc(s, "b0", 2, 2);
	eqv("a discard after the open is gone",
		objsnapent(sn, pb, got, &oidlen, &oi), 0);
	eqv("the count is the open-time count", objsnapcount(sn), 8);
	objsnapclose(sn);
out:
	storeclose(s);
	devclose(d);
}

/*
 * §9's bound.  The vector costs real memory per open fid — 12 MB at
 * nslots = 2^20 — so the store answers layer-a §2.6's `disk full'
 * rather than growing without limit, and a close gives the count
 * back.
 */
static void
tbound(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Objsnap *sn[Objsnapmaxdflt+1], *over;
	int i;

	d = newdisk();
	if((s = mustopen(d, "the snapshot bound")) == nil)
		return;
	mk(s, "x0");
	storestat(s, &st);
	eqv("a fresh store has no snapshot open", st.nobjsnap, 0);
	for(i = 0; i < Objsnapmaxdflt; i++)
		if((sn[i] = objsnapopen(s, Snaplive)) == nil){
			fail("objsnapopen %d of %d: %r", i, Objsnapmaxdflt);
			break;
		}
	storestat(s, &st);
	eqv("the store reports every open snapshot", st.nobjsnap,
		Objsnapmaxdflt);
	over = objsnapopen(s, Snaplive);
	refused("an open past the bound", over != nil ? 0 : -1, "disk full");
	objsnapclose(over);		/* nil unless the bound failed */
	objsnapclose(sn[0]);
	storestat(s, &st);
	eqv("a close releases the count", st.nobjsnap, Objsnapmaxdflt-1);
	if((sn[0] = objsnapopen(s, Snaplive)) == nil)
		fail("objsnapopen after a close: %r");
	storestat(s, &st);
	eqv("and the next open is admitted", st.nobjsnap, Objsnapmaxdflt);
	over = objsnapopen(s, 0);
	refused("a snapshot of no kinds at all", over != nil ? 0 : -1,
		"object snapshot");
	objsnapclose(over);
	/*
	 * kinds is a set of states and not a bag of bits: a caller that
	 * asks for a state the engine does not have is asking for
	 * something it will not get, and a snapshot that quietly dropped
	 * the bit would answer a different question from the one asked.
	 */
	over = objsnapopen(s, 1<<4);
	refused("a snapshot of a state that does not exist",
		over != nil ? 0 : -1, "object snapshot: kinds");
	objsnapclose(over);
	over = objsnapopen(s, Snapboth|1<<4);
	refused("both kinds and one that does not exist",
		over != nil ? 0 : -1, "object snapshot: kinds");
	objsnapclose(over);
	for(i = 0; i < Objsnapmaxdflt; i++)
		objsnapclose(sn[i]);
	storestat(s, &st);
	eqv("closing them all releases every count", st.nobjsnap, 0);
	storeclose(s);
	devclose(d);
}

/*
 * §3.2's condemned store, which answers nothing until it has been
 * opened again and replayed: every one of the five enumerations
 * refuses through storeserving, the open included, and a snapshot
 * taken before the condemnation stops answering entries.  Serving a
 * listing out of memory the store has itself declared untrustworthy
 * is exactly what the flag exists to stop.
 */
static void
tsnapcondemned(void)
{
	Dev *d;
	Store *s;
	Objsnap *sn, *over;
	Objinfo oi;
	Dirtyrec *dr;
	Lostent *l;
	uchar got[Oidmax];
	char **pe, *w;
	ulong n;
	int oidlen;

	d = newdisk();
	if((s = mustopen(d, "the enumerations on a condemned store")) == nil)
		return;
	mk(s, "y0");
	mk(s, "y1");
	if((sn = mustsnap(s, Snaplive, "a condemned store")) == nil)
		goto out;
	eqv("the snapshot answers while the store serves",
		objsnapent(sn, 0, got, &oidlen, &oi), 1);
	storehook(s, "fatal", 1);
	w = "store condemned";
	over = objsnapopen(s, Snaplive);
	refused("objsnapopen on a condemned store", over != nil ? 0 : -1, w);
	objsnapclose(over);
	refused("objsnapent on a condemned store",
		objsnapent(sn, 0, got, &oidlen, &oi), w);
	refused("dirtysnap on a condemned store", dirtysnap(s, &dr, &n), w);
	refused("lostsnap on a condemned store", lostsnap(s, &l, &n), w);
	refused("fullsyncsnap on a condemned store",
		fullsyncsnap(s, &pe, &n), w);
	objsnapclose(sn);
out:
	storeclose(s);
	devclose(d);
}

/*
 * §9's open counts the index under qlstate, releases it to allocate
 * the vector — 12 MB at nslots = 2^20, which §7 rule 2 will not have
 * under a state lock — and re-takes it to fill.  The count can be
 * stale by then, and a vector short of the index is objsnap=partial,
 * which the engine does not have: it counts again.  §13's snapstale
 * point takes the count one short, which is what a create in that
 * window leaves, so this does not have to race for it.
 */
static void
tsnapstale(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Objsnap *sn;
	Walk *w;
	char nm[32];
	ulong i;

	d = newdisk();
	if((s = mustopen(d, "a stale count under an open")) == nil)
		return;
	for(i = 0; i < 10; i++){
		snprint(nm, sizeof nm, "s%lud", i);
		mk(s, nm);
	}
	storehook(s, "snapstale", 1);
	if((sn = mustsnap(s, Snaplive, "a stale count")) == nil)
		goto out;
	eqv("an open whose count went stale names every entry",
		objsnapcount(sn), 10);
	w = newwalk(16);
	walkall(sn, w, 's');
	eqv("and the walk answers them all", w->nlive, 10);
	eqv("none of them twice", w->ndup, 0);
	eqv("and nothing else", w->nother, 0);
	for(i = 0; i < 10; i++)
		eqv("each object exactly once", w->seen[i], 1);
	walkfree(w);
	objsnapclose(sn);

	/*
	 * The re-count is bounded: an index that moves under every
	 * attempt is refused rather than spun on under a lock every
	 * apply wants, and the refusal is local — nothing is full.
	 */
	storehook(s, "snapstale", 1000);
	sn = objsnapopen(s, Snaplive);
	refused("an open whose count never settles", sn != nil ? 0 : -1,
		"object snapshot: the index moved");
	objsnapclose(sn);		/* nil unless the refusal failed */
	storestat(s, &st);
	eqv("and the refusal took no count with it", st.nobjsnap, 0);
	storehook(s, "snapstale", 0);
	if((sn = objsnapopen(s, Snaplive)) == nil)
		fail("objsnapopen once the index settles: %r");
	else{
		eqv("an open after it is whole again", objsnapcount(sn), 10);
		objsnapclose(sn);
	}
out:
	storeclose(s);
	devclose(d);
}

/*
 * §9: every snapshot MUST be closed before the store is, and a
 * storeclose that finds one open says so out loud.  Without that the
 * entries are rendered from a freed Store, which does not fault: the
 * walk finds no qid.path match and answers `gone' for every one of
 * them, so a fid-lifetime bug in a server serves a silently short
 * /obj listing.  The child is the proc that dies, so this one can
 * watch it: RFMEM so it shares the store, RFFDG so the standard
 * error it redirects is its own.
 */
static void
tclosesnap(void)
{
	Dev *d;
	Store *s;
	Objsnap *sn;
	Waitmsg *w;
	int pid, fd, ok;

	d = newdisk();
	if((s = mustopen(d, "storeclose under an open snapshot")) == nil)
		return;
	mk(s, "e0");
	mk(s, "e1");
	if((sn = mustsnap(s, Snaplive, "storeclose")) == nil){
		storeclose(s);
		devclose(d);
		return;
	}
	switch(pid = rfork(RFPROC|RFMEM|RFFDG)){
	case -1:
		fail("rfork: %r");
		objsnapclose(sn);
		storeclose(s);
		devclose(d);
		return;
	case 0:
		close(2);
		if((fd = open("/dev/null", OWRITE)) >= 0 && fd != 2)
			dup(fd, 2);
		storeclose(s);
		exits("storeclose returned");
	}
	ok = 0;
	checks++;
	if((w = wait()) == nil)
		fail("wait for the closing child: %r");
	else{
		if(w->pid != pid)
			fail("waited on pid %d, want the closing child %d",
				w->pid, pid);
		else if(w->msg[0] == '\0')
			fail("storeclose under an open snapshot returned");
		else if(strstr(w->msg, "object snapshot") == nil)
			fail("storeclose under an open snapshot died with "
				"`%s', want the snapshot count", w->msg);
		else
			ok = 1;
		free(w);
	}
	/*
	 * Only if the child really did die before storefree: under the
	 * unguarded close it freed this proc's store, and touching it
	 * again would fault over the FAIL line above.
	 */
	if(ok){
		Storestat st;

		objsnapclose(sn);
		storestat(s, &st);
		eqv("and the store closes once the count is back to nothing",
			st.nobjsnap, 0);
		storeclose(s);
	}
	devclose(d);
}

/* ---- /dirty ---- */

static void
dadd(Store *s, char *name, char *peer, uvlong epoch)
{
	uchar o[Oidmax];

	oidof(o, name);
	if(dirtyadd(s, o, strlen(name), peer, epoch) < 0)
		fail("dirtyadd %s %s: %r", name, peer);
}

static void
ddel(Store *s, char *name, char *peer)
{
	uchar o[Oidmax];

	oidof(o, name);
	if(dirtydel(s, o, strlen(name), peer) < 0)
		fail("dirtydel %s %s: %r", name, peer);
}

static int
hasrec(Dirtyrec *d, ulong n, char *name, char *peer)
{
	ulong i, seen;
	int on, pn;

	on = strlen(name);
	pn = strlen(peer);
	seen = 0;
	for(i = 0; i < n; i++)
		if(d[i].oidlen == on && d[i].peerlen == pn
		&& memcmp(d[i].oid, name, on) == 0
		&& memcmp(d[i].peer, peer, pn) == 0)
			seen++;
	return seen;
}

typedef struct Dchurn Dchurn;
struct Dchurn
{
	Store	*s;
	int	id;
	int	stop;
	int	done;
	int	err;
	int	ops;
};

static void
dchurnproc(void *a)
{
	Dchurn *c;
	uchar o[Oidmax];
	char nm[32], peer[32];
	int k, lap;

	c = a;
	snprint(peer, sizeof peer, "n%d.0", c->id);
	for(lap = 0; lap < 200 && !c->stop; lap++)
		for(k = 0; k < 4 && !c->stop; k++){
			snprint(nm, sizeof nm, "q%d%d", c->id, k);
			oidof(o, nm);
			if(dirtyadd(c->s, o, strlen(nm), peer, 7) < 0)
				c->err++;
			if(dirtydel(c->s, o, strlen(nm), peer) < 0)
				c->err++;
			c->ops++;
		}
	c->done = 1;
}

/*
 * /dirty is one of layer-a §2.2's snapshot MUSTs, and the set it
 * renders is small and bounded by the dirty region — so the store
 * answers a copy rather than a cursor.  The copy is taken under the
 * lock that guards the set, it holds each record once, and nothing
 * done to the set afterwards reaches it.
 */
static void
tdirty(void)
{
	Dev *d;
	Store *s;
	Dirtyrec *c1, *c2;
	Dchurn *w[Nproc];
	char nm[32];
	ulong n1, n2, i;
	int k, live;

	/*
	 * A log the churn below cannot outrun, with §2.8's checkpointer
	 * running to drain it: what this tests is the copy, not §6's
	 * exhaustion.
	 */
	d = makedisk(Tnsec, 128, 64, 512*1024);
	if((s = openstoreck(d)) == nil){
		fail("the /dirty copy: storeopen: %r");
		devclose(d);
		return;
	}
	if(dirtysnap(s, &c1, &n1) < 0)
		fail("dirtysnap of an empty set: %r");
	eqv("an empty dirty set copies as nothing", n1, 0);
	istrue("and hands back no array", c1 == nil);

	for(i = 0; i < 10; i++){
		snprint(nm, sizeof nm, "d%lud", i);
		mk(s, nm);
		dadd(s, nm, "n1.0", 5);
	}
	/*
	 * A removal leaves a hole in the dirty region, and the next add
	 * fills it: the copy must count what it copies rather than take
	 * the array to be dense.
	 */
	ddel(s, "d3", "n1.0");
	if(dirtysnap(s, &c1, &n1) < 0)
		fail("dirtysnap: %r");
	eqv("the copy holds every record in the set", n1, dirtycount(s));
	eqv("which is nine", n1, 9);
	for(i = 0; i < 10; i++){
		snprint(nm, sizeof nm, "d%lud", i);
		eqv("each record appears exactly once", hasrec(c1, n1, nm,
			"n1.0"), i == 3 ? 0 : 1);
	}
	eqv("and the epoch comes with it", c1[0].epoch, 5);
	/*
	 * A record that is IN the set is one that was added: layer-a
	 * §7.1's op is what a renderer would emit, and a copy that said
	 * remove would describe the set it is not.
	 */
	for(i = 0; i < n1; i++)
		eqv("every record in the copy is an add", c1[i].op, 1);

	/* the copy is the caller's; later mutation cannot reach it */
	dadd(s, "d3", "n1.0", 9);
	ddel(s, "d7", "n1.0");
	eqv("a record added after the copy is not in it",
		hasrec(c1, n1, "d3", "n1.0"), 0);
	eqv("and one removed after it still is",
		hasrec(c1, n1, "d7", "n1.0"), 1);
	if(dirtysnap(s, &c2, &n2) < 0)
		fail("second dirtysnap: %r");
	eqv("a fresh copy sees the change", hasrec(c2, n2, "d3", "n1.0"), 1);
	eqv("and the removal", hasrec(c2, n2, "d7", "n1.0"), 0);
	free(c2);

	/*
	 * Under concurrent mutation: four procs add and remove records
	 * of their own while copies are taken.  Every record present
	 * before they started and untouched by them must be in each
	 * copy, exactly once.
	 */
	spawnforget();
	for(k = 0; k < Nproc; k++){
		if((w[k] = mallocz(sizeof *w[k], 1)) == nil)
			sysfatal("mallocz: %r");
		w[k]->s = s;
		w[k]->id = k;
		if(spawnproc(dchurnproc, w[k]) < 0){
			fail("spawn: %r");
			w[k]->done = 1;
		}
	}
	for(i = 0; i < 2000; i++){
		live = 0;
		for(k = 0; k < Nproc; k++)
			if(w[k]->ops > 0)
				live++;
		if(live == Nproc)
			break;
		sleep(1);
	}
	for(i = 0; i < 20; i++){
		if(dirtysnap(s, &c2, &n2) < 0){
			fail("dirtysnap under churn: %r");
			break;
		}
		for(k = 0; k < 10; k++){
			if(k == 7)
				continue;
			snprint(nm, sizeof nm, "d%d", k);
			if(hasrec(c2, n2, nm, "n1.0") != 1){
				fail("a record untouched by the churn is not "
					"in the copy exactly once: %s", nm);
				break;
			}
		}
		free(c2);
	}
	checks++;			/* the loop above is one check */
	for(k = 0; k < Nproc; k++)
		w[k]->stop = 1;
	for(i = 0; i < 20000; i++){
		live = 0;
		for(k = 0; k < Nproc; k++)
			if(!w[k]->done)
				live++;
		if(live == 0)
			break;
		sleep(1);
	}
	for(k = 0; k < Nproc; k++){
		istrue("the churn proc finished", w[k]->done);
		eqv("with no failed dirty record", w[k]->err, 0);
		free(w[k]);
	}
	free(c1);
	killspawned();
	storeclose(s);
	devclose(d);
}

static int
haspeer(char **p, ulong n, char *name)
{
	ulong i, seen;

	seen = 0;
	for(i = 0; i < n; i++)
		if(strcmp(p[i], name) == 0)
			seen++;
	return seen;
}

/*
 * The coarse half of /dirty (layer-a §2.2): one `fullsync peer=' line
 * per peer carrying §7.1's flag.  §2.6's exhaustion drop is what
 * makes this a separate enumeration rather than a field of a record
 * — it throws away every fine-grained record for the peer it marks,
 * so the peer that most needs the line is the one the record copy
 * cannot name.
 */
static void
tfullsync(void)
{
	Dev *d;
	Store *s;
	Dirtyrec *c;
	char **pe, nm[32];
	ulong n, np, i;

	d = makedisk(Tnsec, 128, 64, 512*1024);
	if((s = openstoreck(d)) == nil){
		fail("the fullsync copy: storeopen: %r");
		devclose(d);
		return;
	}
	if(fullsyncsnap(s, &pe, &np) < 0)
		fail("fullsyncsnap of a store with no peers: %r");
	eqv("a store that has heard of no peer names none", np, 0);
	istrue("and hands back no array", pe == nil);

	/* fill the region: 40 records for one peer, 24 for the other */
	for(i = 0; i < 64; i++){
		snprint(nm, sizeof nm, "f%lud", i);
		mk(s, nm);
		dadd(s, nm, i < 40 ? "n1.0" : "n2.0", 5);
	}
	eqv("the dirty region is full", dirtycount(s), 64);
	if(fullsyncsnap(s, &pe, &np) < 0)
		fail("fullsyncsnap: %r");
	eqv("both peers are named", np, 2);
	free(pe);

	/*
	 * One record past the region.  §2.6 drops every record of the
	 * peer holding the most of them and marks that peer fullsync,
	 * which is layer-a §7.1's explicit licence.
	 */
	mk(s, "f64");
	dadd(s, "f64", "n2.0", 5);
	if(dirtysnap(s, &c, &n) < 0)
		fail("dirtysnap after the drop: %r");
	eqv("the drop took every record of its victim", hasrec(c, n,
		"f0", "n1.0"), 0);
	eqv("and left the other peer's alone", hasrec(c, n, "f40", "n2.0"),
		1);
	eqv("so the record copy names one peer and not two", n, 25);
	free(c);
	if(fullsyncsnap(s, &pe, &np) < 0)
		fail("fullsyncsnap after the drop: %r");
	else{
		istrue("the copy names the peer the drop marked, which no "
			"record left names", haspeer(pe, np, "n1.0") == 1);
		istrue("and the peer whose records survived, which carries "
			"the flag too", haspeer(pe, np, "n2.0") == 1);
		eqv("and nothing the store has never heard of", np, 2);
		free(pe);
	}
	eqv("storefullsync agrees about the victim",
		storefullsync(s, "n1.0"), 1);
	storeclose(s);
	devclose(d);
}

/*
 * /lost, layer-a §2.2's other snapshot MUST.  The copy names the same
 * slots storelost does and carries the oid and the Objinfo beside
 * each, so a renderer never goes back to an index that has moved
 * under it — and the copy does not move when the list does.
 */
static void
tlost(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Lostent *l1, *l2;
	Objinfo o1, o2, o3;
	Super sup;
	uchar b3[Oidmax], *buf, junk[8];
	ulong n1, n2, slot;

	d = newdisk();
	if((s = mustopen(d, "the /lost copy")) == nil)
		return;
	buf = mkbuf(3*Blk, 41);
	mk(s, "k1");
	mk(s, "k2");
	mk(s, "k3");
	oidof(b3, "k3");
	if(objwrite(s, b3, 2, buf, 3*Blk, 0, 2, 1, nil, 0) < 0)
		fail("objwrite k3: %r");
	if(lostsnap(s, &l1, &n1) < 0)
		fail("lostsnap of a clean store: %r");
	eqv("a clean store loses nothing", n1, 0);
	istrue("and hands back no array", l1 == nil);

	if(ostat(s, "k1", &o1) < 0 || ostat(s, "k2", &o2) < 0)
		fail("objstat: %r");
	corrupt(s, "k1", 1);
	if(lostsnap(s, &l1, &n1) < 0)
		fail("lostsnap: %r");
	eqv("a flagged copy is listed", n1, 1);
	eqv("and named by its slot", l1[0].oi.slot, o1.slot);
	eqv("with its oid beside it", l1[0].oidlen, 2);
	istrue("which is the object's", memcmp(l1[0].oid, "k1", 2) == 0);
	eqv("and its Objinfo", l1[0].oi.corrupt, 1);
	eqv("carrying the qid.path a renderer needs", l1[0].oi.qidpath,
		o1.qidpath);

	/*
	 * The list moves: another object is flagged and the first is
	 * cleared.  storelost answers the new list; the copy does not.
	 */
	corrupt(s, "k2", 1);
	corrupt(s, "k1", 0);
	storestat(s, &st);
	eqv("the live list has moved", st.nlost, 1);
	eqv("and storelost answers the new member", storelost(s, 0), o2.slot);
	eqv("while the copy still holds one entry", n1, 1);
	eqv("naming the slot it named at the copy", l1[0].oi.slot, o1.slot);
	if(lostsnap(s, &l2, &n2) < 0)
		fail("second lostsnap: %r");
	eqv("a fresh copy names the new member", n2, 1);
	eqv("by its slot", l2[0].oi.slot, o2.slot);
	istrue("and by its oid", memcmp(l2[0].oid, "k2", 2) == 0);
	free(l1);
	free(l2);

	/* §5 step 10's condemnation is the other way into the list */
	corrupt(s, "k2", 0);
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	geom(d, &sup);
	if(ostat(s, "k3", &o3) < 0)
		fail("objstat k3: %r");
	slot = o3.slot;
	memset(junk, 0xa5, sizeof junk);
	simpoke(d, emapentoff(&sup, o3.emapslot) + sup.emapsz - sizeof junk,
		junk, sizeof junk);
	storeclose(s);
	if((s = mustopen(d, "the /lost copy, condemned")) == nil){
		devclose(d);
		free(buf);
		return;
	}
	if(objread(s, b3, 2, buf, Blk, 0) >= 0)
		fail("a damaged extent map was served");
	if(lostsnap(s, &l1, &n1) < 0)
		fail("lostsnap after a condemnation: %r");
	eqv("a condemned slot is copied too", n1, 1);
	eqv("by its slot", l1[0].oi.slot, slot);
	istrue("with its oid", memcmp(l1[0].oid, "k3", 2) == 0);
	eqv("and answers corrupt rather than absent", l1[0].oi.corrupt, 1);
	free(l1);
	free(buf);
	storeclose(s);
	devclose(d);
}

/*
 * The other condemnation, §5 step 10's: an index entry that does not
 * unpack.  The slot is left bad with its state still free, and it is
 * the one slot on the /lost list whose own entry is the damage — so
 * it has no oid, and a copy that skipped it would disagree with
 * /status's own lost count on exactly the case /lost exists for.
 * The geometry is storetest's: damage an entry the log no longer
 * describes, so replay does not repair it.
 */
static void
tlostbadent(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Lostent *l;
	Super sup;
	uchar *buf, o[Oidmax], junk[8];
	ulong n;

	d = newdisk();
	if((s = mustopen(d, "the /lost copy of a damaged entry")) == nil)
		return;
	buf = mkbuf(64, 9);
	mk(s, "g0");
	oidof(o, "g0");
	if(objwrite(s, o, 2, buf, 64, 0, 2, 1, nil, 0) < 0)
		fail("objwrite g0: %r");
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storeclose(s);
	geom(d, &sup);
	memset(junk, 0xff, sizeof junk);
	simpoke(d, idxentoff(&sup, 0), junk, sizeof junk);
	if((s = mustopen(d, "a store with a damaged index entry")) == nil){
		devclose(d);
		free(buf);
		return;
	}
	storestat(s, &st);
	eqv("the damaged entry is condemned", st.nlost, 1);
	eqv("and storelost names its slot", storelost(s, 0), 0);
	if(lostsnap(s, &l, &n) < 0)
		fail("lostsnap: %r");
	else{
		eqv("the copy names it too", n, st.nlost);
		if(n == 1){
			eqv("by the slot storelost named", l[0].oi.slot, 0);
			eqv("with no oid, because the entry is the damage",
				l[0].oidlen, 0);
			eqv("and a state of free", l[0].oi.state, Sfree);
			eqv("with nothing else invented", l[0].oi.qidpath, 0);
			eqv("not even a corrupt flag it cannot read",
				l[0].oi.corrupt, 0);
		}
		free(l);
	}
	storeclose(s);
	devclose(d);
	free(buf);
}

/* ---- T1.15: a full walk under concurrent creates and deletes ---- */

typedef struct Churn Churn;
struct Churn
{
	Store	*s;
	int	lo, hi;
	uvlong	we;			/* the wepoch its deletes carry */
	int	stop;
	int	done;
	int	err;
	int	ops;
};

/*
 * One proc per object, as §7 requires of every caller of the object
 * API: each proc owns a disjoint range of the m-objects and runs the
 * whole delete/discard/create cycle on them, so the index churns —
 * slots freed, slots handed out again, qid.paths retired — under the
 * walker.
 */
static void
churnproc(void *a)
{
	Churn *c;
	uchar o[Oidmax];
	char nm[32];
	int k, lap;

	c = a;
	for(lap = 0; lap < 100 && !c->stop; lap++)
		for(k = c->lo; k < c->hi && !c->stop; k++){
			snprint(nm, sizeof nm, "m%04d", k);
			oidof(o, nm);
			if(objremove(c->s, o, 5, 2, c->we, nil, 0) < 0)
				c->err++;
			else if(objdiscard(c->s, o, 5, 2, c->we, c->we+1) < 0)
				c->err++;
			else if(objcreate(c->s, o, 5, 1, c->we, nil, 0, nil) < 0)
				c->err++;
			c->ops++;
		}
	c->done = 1;
}

static void
tconc(void)
{
	Dev *d;
	Store *s;
	Objsnap *sn;
	Walk *w;
	Churn *c[Nproc];
	char nm[32];
	vlong t0;
	ulong i, per;
	int k, live;

	live = 0;

	d = bigdisk();
	if((s = openstoreck(d)) == nil){
		fail("the concurrent walk: storeopen: %r");
		devclose(d);
		return;
	}
	spawnforget();
	t0 = nsec();
	/*
	 * Tombstones in the slots below, so that no position in the
	 * snapshot is the slot at that index.
	 */
	for(i = 0; i < 200; i++){
		snprint(nm, sizeof nm, "x%04lud", i);
		mk(s, nm);
		rmv(s, nm, 2);
	}
	for(i = 0; i < Nconc; i++){
		snprint(nm, sizeof nm, "c%04lud", i);
		mk(s, nm);
	}
	for(i = 0; i < Nchurn; i++){
		snprint(nm, sizeof nm, "m%04lud", i);
		mk(s, nm);
	}
	if((sn = mustsnap(s, Snaplive, "the concurrent walk")) == nil)
		goto out;
	eqv("the snapshot names every live object at open",
		objsnapcount(sn), Nconc + Nchurn);

	per = Nchurn / Nproc;
	for(k = 0; k < Nproc; k++){
		if((c[k] = mallocz(sizeof *c[k], 1)) == nil)
			sysfatal("mallocz: %r");
		c[k]->s = s;
		c[k]->lo = k*per;
		c[k]->hi = (k == Nproc-1) ? Nchurn : (k+1)*per;
		c[k]->we = 1;
		if(spawnproc(churnproc, c[k]) < 0){
			fail("spawn: %r");
			c[k]->done = 1;
		}
	}
	/* let every proc get going, so the walk really does overlap it */
	for(i = 0; i < 5000; i++){
		live = 0;
		for(k = 0; k < Nproc; k++)
			if(c[k]->ops > 0)
				live++;
		if(live == Nproc)
			break;
		sleep(1);
	}
	eqv("every churn proc is running", live, Nproc);

	w = newwalk(Nconc);
	walkall(sn, w, 'c');
	for(k = 0; k < Nproc; k++)
		c[k]->stop = 1;
	eqv("no entry is answered twice", w->ndup, 0);
	eqv("and none is answered outside the vector", w->nbadidx, 0);
	eqv("the count did not move under the walk", objsnapcount(sn),
		Nconc + Nchurn);
	eqv("every object live at open and untouched since is answered",
		w->nlive - w->nother, Nconc);
	for(i = 0; i < Nconc; i++)
		if(w->seen[i] != 1){
			fail("c%04lud answered %d times, want 1", i,
				w->seen[i]);
			break;
		}
	checks++;			/* the loop above is one check */
	istrue("and what the churn touched is answered at most once",
		w->nother <= Nchurn);
	for(i = 0; i < 20000; i++){
		live = 0;
		for(k = 0; k < Nproc; k++)
			if(!c[k]->done)
				live++;
		if(live == 0)
			break;
		sleep(1);
	}
	for(k = 0; k < Nproc; k++){
		istrue("the churn proc finished", c[k]->done);
		eqv("with no failed operation", c[k]->err, 0);
	}
	print("enumtest: T1.15 at %d slots, %d entries: %lldms\n", Bigslots,
		Nconc + Nchurn, (nsec() - t0)/1000000);
	for(k = 0; k < Nproc; k++)
		free(c[k]);
	walkfree(w);
	objsnapclose(sn);
out:
	killspawned();
	storeclose(s);
	devclose(d);
}

/*
 * A checkpoint under a walk.  §2.8's checkpoint materialises durable
 * state and touches no index entry's identity, so a snapshot's
 * semantics cannot depend on when one runs.
 */
static void
tckpt(void)
{
	Dev *d;
	Store *s;
	Objsnap *sn;
	Objinfo oi;
	uchar got[Oidmax];
	char nm[32];
	ulong i, half;
	int oidlen, n;

	d = newdisk();
	if((s = mustopen(d, "a checkpoint under a walk")) == nil)
		return;
	for(i = 0; i < 20; i++){
		snprint(nm, sizeof nm, "p%lud", i);
		mk(s, nm);
	}
	if((sn = mustsnap(s, Snaplive, "checkpoint")) == nil)
		goto out;
	half = objsnapcount(sn)/2;
	n = 0;
	for(i = 0; i < half; i++)
		if(objsnapent(sn, i, got, &oidlen, &oi) == 1)
			n++;
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint mid-walk: %r");
	for(i = half; i < objsnapcount(sn); i++)
		if(objsnapent(sn, i, got, &oidlen, &oi) == 1)
			n++;
	eqv("a checkpoint under a walk changes nothing about it", n, 20);
	eqv("nor the count", objsnapcount(sn), 20);
	objsnapclose(sn);
out:
	storeclose(s);
	devclose(d);
}

/* ---- layer-a §1.5's reclaim walk over a /tombs snapshot ---- */

typedef struct Reclaim Reclaim;
struct Reclaim
{
	ulong	nseen;			/* entries the snapshot answered */
	ulong	ngone;
	ulong	nskip;			/* answered, failed the test */
	ulong	ndisc;			/* discarded */
	ulong	nrefused;		/* objdiscard said no */
	char	lasterr[ERRMAX];
};

/*
 * The walk the store does NOT do.  The engine has no tombdays policy:
 * layer-a §3.1 makes tombdays a map-header attribute, so the caller
 * evaluates it against the tombstone's mtime (§6), which the Objinfo
 * carries, and the caller's own map epoch against its wepoch.  All
 * the engine owes it is a /tombs snapshot and objdiscard.
 *
 * Every discard names the entry's OWN key, which is what makes a
 * concurrent mutation safe: if the record has been replaced since the
 * entry was rendered, layer-a §1.5's receiver checks refuse it rather
 * than remove a record that is not the one this walk inspected.
 */
static void
reclaimwalk(Store *s, vlong cutoff, uvlong epoch, Reclaim *r)
{
	Objsnap *sn;
	Objinfo oi;
	uchar oid[Oidmax];
	ulong i;
	int oidlen, rr;

	memset(r, 0, sizeof *r);
	if((sn = objsnapopen(s, Snaptomb)) == nil){
		fail("the reclaim walk: objsnapopen: %r");
		return;
	}
	for(i = 0; i < objsnapcount(sn); i++){
		rr = objsnapent(sn, i, oid, &oidlen, &oi);
		if(rr < 0){
			fail("objsnapent %lud: %r", i);
			break;
		}
		if(rr == 0){
			r->ngone++;
			continue;
		}
		r->nseen++;
		if(oi.mtime > cutoff || oi.wepoch >= epoch){
			r->nskip++;
			continue;
		}
		if(objdiscard(s, oid, oidlen, oi.ver, oi.wepoch, epoch) < 0){
			r->nrefused++;
			rerrstr(r->lasterr, sizeof r->lasterr);
			continue;
		}
		r->ndisc++;
	}
	objsnapclose(sn);
}

static void
gone(Store *s, char *name)
{
	Objinfo oi;

	refused("the discarded tombstone", ostat(s, name, &oi),
		"no such object");
}

static void
stilltomb(Store *s, char *name)
{
	Objinfo oi;

	checks++;
	if(ostat(s, name, &oi) < 0)
		fail("%s should still be a tombstone: %r", name);
	else if(oi.state != Stomb)
		fail("%s is state %d, want a tombstone", name, oi.state);
}

/*
 * The reclaim walk, single-proc.  Two groups of tombstones a second
 * apart in mtime, one of them at a wepoch the caller's epoch does not
 * supersede: the cutoff must select on mtime and the epoch condition
 * on wepoch, and nothing else may be touched.
 */
static void
treclaim(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Objinfo oi;
	Reclaim r;
	vlong t0;
	uchar o[Oidmax];
	char nm[32];
	ulong i;

	d = newdisk();
	if((s = mustopen(d, "the reclaim walk")) == nil)
		return;
	/* group A: r0..r4, deleted first.  r3 at a wepoch of 9. */
	for(i = 0; i < 5; i++){
		snprint(nm, sizeof nm, "r%lud", i);
		mk(s, nm);
	}
	for(i = 0; i < 5; i++){
		snprint(nm, sizeof nm, "r%lud", i);
		oidof(o, nm);
		if(objremove(s, o, 2, 2, i == 3 ? 9 : 1, nil, 0) < 0)
			fail("objremove r%lud: %r", i);
	}
	if(ostat(s, "r0", &oi) < 0)
		fail("objstat r0: %r");
	t0 = oi.mtime;
	/* group B: r5..r8, at least a second later */
	sleep(1100);
	for(i = 5; i < 9; i++){
		snprint(nm, sizeof nm, "r%lud", i);
		mk(s, nm);
		rmv(s, nm, 2);
	}
	/* two live objects, which /tombs must not name at all */
	mk(s, "v0");
	mk(s, "v1");
	if(ostat(s, "r5", &oi) < 0)
		fail("objstat r5: %r");
	istrue("the second group is later in mtime", oi.mtime > t0);

	reclaimwalk(s, t0, 5, &r);
	eqv("/tombs names every tombstone and nothing else", r.nseen, 9);
	eqv("none of them is gone", r.ngone, 0);
	eqv("the walk discards the four the cutoff and the epoch admit",
		r.ndisc, 4);
	eqv("and skips the rest", r.nskip, 5);
	eqv("refusing none", r.nrefused, 0);
	gone(s, "r0");
	gone(s, "r1");
	gone(s, "r2");
	gone(s, "r4");
	stilltomb(s, "r3");		/* wepoch 9, epoch 5 */
	for(i = 5; i < 9; i++){
		snprint(nm, sizeof nm, "r%lud", i);
		stilltomb(s, nm);	/* mtime past the cutoff */
	}
	if(ostat(s, "v0", &oi) < 0)
		fail("objstat v0: %r");
	else
		eqv("and the live objects are untouched", oi.state, Slive);

	/* the cutoff is evaluated against mtime and nothing else */
	storestat(s, &st);
	eqv("five tombstones remain", st.ntomb, 5);
	reclaimwalk(s, t0 - 1, 5, &r);
	eqv("a cutoff one second earlier selects nothing", r.ndisc, 0);
	eqv("though the walk saw them all", r.nseen, 5);
	storestat(s, &st);
	eqv("so every tombstone is still there", st.ntomb, 5);
	storeclose(s);
	devclose(d);
}

/*
 * The same walk with the record changing under it.  The interference
 * is applied between the render of an entry and the discard of it,
 * which is the window §6 says the caller's per-object queue closes
 * and which a server's queue would close here — so the test performs
 * it directly rather than racing for it, and then runs the racing
 * version for the aggregate.
 *
 * What must hold: a tombstone re-created over during the walk is
 * skipped as gone; one the walk rendered and something else then
 * replaced or removed is refused by name rather than removed as
 * whatever the slot now holds.
 */
static void
treclaimrace(void)
{
	Dev *d;
	Store *s;
	Objsnap *sn;
	Objinfo oi;
	uchar oid[Oidmax];
	char nm[32];
	ulong i, ndisc, ngone, nrefused;
	vlong cutoff;
	int oidlen, r;

	d = newdisk();
	if((s = mustopen(d, "the reclaim walk under mutation")) == nil)
		return;
	for(i = 0; i < 6; i++){
		snprint(nm, sizeof nm, "w%lud", i);
		mk(s, nm);
		rmv(s, nm, 2);
	}
	if(ostat(s, "w0", &oi) < 0)
		fail("objstat w0: %r");
	cutoff = oi.mtime;
	if((sn = mustsnap(s, Snaptomb, "the reclaim walk")) == nil)
		goto out;

	/* w1 is created over before the walk reaches it */
	remk(s, "w1", 3);

	ndisc = ngone = nrefused = 0;
	for(i = 0; i < objsnapcount(sn); i++){
		r = objsnapent(sn, i, oid, &oidlen, &oi);
		if(r < 0){
			fail("objsnapent %lud: %r", i);
			break;
		}
		if(r == 0){
			ngone++;
			continue;
		}
		if(oi.mtime > cutoff || oi.wepoch >= 5)
			continue;
		/*
		 * The interference, in the window a per-object queue
		 * closes: w2 is created over after this walk rendered it,
		 * and w3 is discarded by something else entirely.
		 */
		if(oidlen == 2 && memcmp(oid, "w2", 2) == 0)
			remk(s, "w2", 3);
		if(oidlen == 2 && memcmp(oid, "w3", 2) == 0)
			disc(s, "w3", 2, 2);
		if(objdiscard(s, oid, oidlen, oi.ver, oi.wepoch, 5) < 0){
			nrefused++;
			if(oidlen == 2 && memcmp(oid, "w2", 2) == 0)
				refused("a discard of a record created over "
					"since the walk rendered it", -1,
					"not discardable");
			else if(oidlen == 2 && memcmp(oid, "w3", 2) == 0)
				refused("a discard of a record another proc "
					"already discarded", -1,
					"no such object");
			else
				fail("objdiscard: %r");
			continue;
		}
		ndisc++;
	}
	eqv("a tombstone created over before the walk reached it is gone",
		ngone, 1);
	eqv("the two interfered-with entries are refused", nrefused, 2);
	eqv("and the other three are discarded", ndisc, 3);
	/* the refusals removed nothing that was not the record inspected */
	if(ostat(s, "w1", &oi) < 0)
		fail("objstat w1: %r");
	else
		eqv("the re-created w1 is still live", oi.state, Slive);
	if(ostat(s, "w2", &oi) < 0)
		fail("objstat w2: %r");
	else
		eqv("and so is w2, which the refusal did not remove",
			oi.state, Slive);
	gone(s, "w0");
	gone(s, "w3");
	objsnapclose(sn);
out:
	storeclose(s);
	devclose(d);
}

/*
 * And the racing version: procs churn a disjoint set of objects while
 * one proc reclaims over a /tombs snapshot.  The invariant is that
 * the walk removes exactly the records it inspected and nothing else,
 * so the untouched tombstones must all be gone and every live object
 * must survive.
 */
static void
treclaimconc(void)
{
	Dev *d;
	Store *s;
	Churn *c[Nproc];
	Reclaim r;
	Objinfo oi;
	Storestat st;
	char nm[32];
	ulong i;
	vlong cutoff;
	int k, live;

	d = bigdisk();
	if((s = openstoreck(d)) == nil){
		fail("the concurrent reclaim: storeopen: %r");
		devclose(d);
		return;
	}
	spawnforget();
	/* live objects below, so a position in the snapshot is not a slot */
	for(i = 0; i < 50; i++){
		snprint(nm, sizeof nm, "y%04lud", i);
		mk(s, nm);
	}
	for(i = 0; i < 200; i++){
		snprint(nm, sizeof nm, "z%04lud", i);
		mk(s, nm);
		rmv(s, nm, 2);
	}
	for(i = 0; i < 200; i++){
		snprint(nm, sizeof nm, "m%04lud", i);
		mk(s, nm);
	}
	/*
	 * The cutoff is the LAST tombstone's mtime, not the first's:
	 * mtime is a whole second (obj.c takes time(nil)), so a run that
	 * straddles a second boundary while these 200 are made would
	 * leave the later ones above a cutoff taken from the first and
	 * silently halve what the walk is given.
	 */
	if(ostat(s, "z0199", &oi) < 0)
		fail("objstat z0199: %r");
	cutoff = oi.mtime;
	for(k = 0; k < Nproc; k++){
		if((c[k] = mallocz(sizeof *c[k], 1)) == nil)
			sysfatal("mallocz: %r");
		c[k]->s = s;
		c[k]->lo = k*50;
		c[k]->hi = (k+1)*50;
		/*
		 * Not below the epoch the reclaim walk below runs at, so
		 * that walk skips whatever tombstone the churn is holding
		 * when its snapshot is taken: layer-a §1.5's condition 3 is
		 * the caller's, and this caller is not the churn's primary.
		 * The epoch is what separates the two sets and not the
		 * cutoff, because the churn's mtimes are this same second.
		 */
		c[k]->we = 5;
		if(spawnproc(churnproc, c[k]) < 0){
			fail("spawn: %r");
			c[k]->done = 1;
		}
	}
	for(i = 0; i < 5000; i++){
		live = 0;
		for(k = 0; k < Nproc; k++)
			if(c[k]->ops > 0)
				live++;
		if(live == Nproc)
			break;
		sleep(1);
	}
	reclaimwalk(s, cutoff, 5, &r);
	for(k = 0; k < Nproc; k++)
		c[k]->stop = 1;
	eqv("the walk discarded every tombstone it was given", r.ndisc, 200);
	eqv("and skipped every one the churn's epoch reserves", r.nskip,
		r.nseen - 200);
	eqv("and was refused none of them", r.nrefused, 0);
	for(i = 0; i < 200; i += 37){
		snprint(nm, sizeof nm, "z%04lud", i);
		gone(s, nm);
	}
	for(i = 0; i < 20000; i++){
		live = 0;
		for(k = 0; k < Nproc; k++)
			if(!c[k]->done)
				live++;
		if(live == 0)
			break;
		sleep(1);
	}
	for(k = 0; k < Nproc; k++){
		istrue("the churn proc finished", c[k]->done);
		eqv("with no failed operation", c[k]->err, 0);
		free(c[k]);
	}
	storestat(s, &st);
	eqv("and the churned objects are all live again", st.nlive, 250);
	eqv("with no tombstone left anywhere", st.ntomb, 0);
	killspawned();
	storeclose(s);
	devclose(d);
}

void
main(int argc, char **argv)
{
	USED(argc); USED(argv);
	tobj();
	ttombs();
	tadvert();
	tbound();
	tsnapcondemned();
	tsnapstale();
	tclosesnap();
	tdirty();
	tfullsync();
	tlost();
	tlostbadent();
	tckpt();
	treclaim();
	treclaimrace();
	tconc();
	treclaimconc();
	killspawned();
	if(fails > 0){
		print("enumtest: %d of %d checks FAILED\n", fails, checks);
		exits("failed");
	}
	print("enumtest: %d checks ok\n", checks);
	exits(nil);
}
