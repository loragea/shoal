#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"
#include "t1.h"

/*
 * T1 for docs/design/store.md §8's online bitmap rebuild (D18): the
 * engine half of the scrub pass — begin, the per-slot fold, the swap,
 * the abort — the write barrier under it, and the per-slot generation
 * stamp that validates an entry the fold re-read outside the state
 * lock.
 *
 * §13's T1.28 is the walk itself (tonline, tbarrier, treread); T1.29
 * is the swap and the pass's lifetime (tswap, tabort, tclose).
 *
 * Every check that says "the rebuilt bitmap equals a full scan of the
 * live maps" is scanok below, which compares the bitmap a checkpoint
 * materialised against the grains the index and the extent maps on
 * the media name — the same statement §5 step 11's rebuild is judged
 * by, made about a store that never stopped serving.
 */

enum
{
	Blk	= 4096,			/* t1.h's small geometry */
	Bigsec	= 512,			/* the multi-page geometry below */
	Bigblk	= 512,
	Bignsec	= 16384,
};

static int nfreed;

static void
sawfree(void *a)
{
	USED(a);
	nfreed++;
}

/*
 * One opener for every store here: t1.h's, plus the free hook tclose
 * watches and a stage lifetime long enough that a walk cannot outlast
 * it — §3.6's sweep releasing a stage's grains under the pass would
 * be a different test from the one tonline is making.  cap, when not
 * zero, is §9's extent-map LRU bound: a store opened with one entry
 * drops a map as soon as another is read, which is how tleak makes an
 * eviction happen at a point of its choosing rather than waiting for
 * one.
 */
static Store*
openbmcap(Dev *d, char *what, ulong cap)
{
	Storecfg c;
	Store *s;

	nfreed = 0;
	tcfg(&c);
	c.stagems = 30000;
	c.emapcache = cap;
	c.freed = sawfree;
	if((s = storeopen(d, &c)) == nil)
		fail("%s: storeopen: %r", what);
	return s;
}

static Store*
openbm(Dev *d, char *what)
{
	return openbmcap(d, what, 0);
}

static void
mk(Store *s, char *name)
{
	uchar o[Oidmax];

	oidof(o, name);
	if(objcreate(s, o, strlen(name), 1, 1, nil, 0, nil) < 0)
		fail("objcreate %s: %r", name);
}

static void
mustwr(Store *s, char *name, void *a, long n, uvlong off, uvlong ver)
{
	uchar o[Oidmax];

	oidof(o, name);
	if(objwrite(s, o, strlen(name), a, n, off, ver, 1, nil, 0) < 0)
		fail("objwrite %s %ld at %llud: %r", name, n, off);
}

static int
ostat(Store *s, char *name, Objinfo *oi)
{
	uchar o[Oidmax];

	oidof(o, name);
	return objstat(s, o, strlen(name), oi);
}

/* the object verifies: no block the reclaim took away from it */
static void
whole(Store *s, char *name, char *what)
{
	Vfy v;
	uchar o[Oidmax];

	memset(&v, 0, sizeof v);
	oidof(o, name);
	checks++;
	if(objverify(s, o, strlen(name), &v) < 0)
		fail("%s: objverify %s: %r", what, name);
	else{
		checks++;
		if(v.nbad != 0 || v.arraybad)
			fail("%s: %s has %lud bad blocks and arraybad %d",
				what, name, v.nbad, v.arraybad);
	}
	vfyfree(&v);
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

/* the grain holding block blk of an object, read off the media */
static ulong
grainof(Dev *d, Super *sup, Objinfo *oi, ulong blk)
{
	uchar *p;
	ulong g;

	if(oi->emapslot == 0){
		fail("grainof: object has no extent-map slot");
		return 0;
	}
	if((p = malloc(sup->emapsz)) == nil)
		sysfatal("malloc: %r");
	simpeek(d, emapentoff(sup, oi->emapslot), p, sup->emapsz);
	g = emapgrain(p, blk);
	free(p);
	return g;
}

/* flip every byte of n bytes at off, so the change is never a no-op */
static void
flipbytes(Dev *d, vlong off, long n)
{
	uchar b[16];
	long i;

	if(n > (long)sizeof b)
		n = sizeof b;
	simpeek(d, off, b, n);
	for(i = 0; i < n; i++)
		b[i] = ~b[i];
	simpoke(d, off, b, n);
}

/*
 * Damage an extent-map entry without repairing its csum128, so the
 * entry itself fails and §5 step 10 condemns the slot on the first
 * read: scrubtest's recipe, and the leak path §6 counts.
 */
static void
damageentry(Dev *d, Super *sup, ulong emapslot)
{
	uchar junk[8];

	memset(junk, 0xa5, sizeof junk);
	simpoke(d, emapentoff(sup, emapslot) + sup->emapsz - sizeof junk,
		junk, sizeof junk);
}

/*
 * The full scan: every grain the live maps name, read off the media
 * rather than through the engine, so the answer owes the bitmap
 * nothing.  A map whose entry fails its checksum is §5 step 10's
 * damage and names nothing a store may believe, which is exactly what
 * the pass reclaims.
 */
static uchar*
scanmaps(Dev *d, Super *sup)
{
	Idxent ie;
	uchar *bits, *p, *ep;
	uvlong nblk, i;
	ulong slot, g;

	if((bits = mallocz((sup->ngrains + 7)/8, 1)) == nil)
		sysfatal("malloc: %r");
	if((p = malloc(Idxentsz)) == nil || (ep = malloc(sup->emapsz)) == nil)
		sysfatal("malloc: %r");
	bits[0] |= 1;			/* grain 0 is never allocatable */
	for(slot = 0; slot < sup->nslots; slot++){
		simpeek(d, idxentoff(sup, slot), p, Idxentsz);
		if(idxunpack(&ie, p, sup->nemap) < 0 || ie.state == Sfree)
			continue;
		nblk = blkcount(ie.len, sup->blksz);
		if(nblk > sup->nblkmax)
			nblk = sup->nblkmax;
		if(ie.emapslot == 0){
			if(nblk > 0 && ie.grain0 != 0
			&& ie.grain0 < sup->ngrains)
				bits[ie.grain0/8] |= 1 << (ie.grain0%8);
			continue;
		}
		simpeek(d, emapentoff(sup, ie.emapslot), ep, sup->emapsz);
		if(!reccsumok(ep, sup->emapsz, 0))
			continue;
		for(i = 0; i < nblk; i++){
			g = emapgrain(ep, i);
			if(g != 0 && g < sup->ngrains)
				bits[g/8] |= 1 << (g%8);
		}
	}
	free(p);
	free(ep);
	return bits;
}

/* the bitmap as the last checkpoint materialised it, page by page */
static uchar*
diskbits(Dev *d, Super *sup)
{
	uchar *bits, *page;
	uvlong npage, bytes, i;

	bytes = bmbits(sup->blksz)/8;
	npage = nbmpage(sup);
	if((bits = mallocz(npage*bytes, 1)) == nil)
		sysfatal("malloc: %r");
	if((page = malloc(sup->blksz)) == nil)
		sysfatal("malloc: %r");
	for(i = 0; i < npage; i++){
		simpeek(d, (vlong)sup->bmapoff*sup->secsz
			+ i*(vlong)sup->blksz, page, sup->blksz);
		memmove(bits + i*bytes, page + Bmhdrsz, bytes);
	}
	free(page);
	return bits;
}

static uvlong
countbits(uchar *bits, uvlong n)
{
	uvlong i, k;

	k = 0;
	for(i = 0; i < n; i++)
		if((bits[i/8] >> (i%8)) & 1)
			k++;
	return k;
}

/*
 * The rebuilt bitmap equals a full scan of the live maps, and §6's
 * free count agrees with both: every grain no live map names and no
 * stage holds is free, and nothing else is.  A checkpoint first,
 * because the bitmap reaches the media nowhere else — so this also
 * says the swap dirtied the pages it changed.
 */
static void
scanok(Store *s, Dev *d, char *what)
{
	Storestat st;
	Super sup;
	uchar *want, *got;
	uvlong g, nbad, named;

	if(storecheckpoint(s) < 0){
		fail("%s: storecheckpoint: %r", what);
		return;
	}
	geom(d, &sup);
	want = scanmaps(d, &sup);
	got = diskbits(d, &sup);
	nbad = 0;
	for(g = 0; g < sup.ngrains; g++)
		if(((got[g/8] >> (g%8)) & 1) != ((want[g/8] >> (g%8)) & 1)){
			if(nbad == 0)
				fail("%s: grain %llud is %d in the bitmap and "
					"%d in the live maps", what,
					g, (got[g/8] >> (g%8)) & 1,
					(want[g/8] >> (g%8)) & 1);
			nbad++;
		}
	checks++;
	if(nbad > 1)
		fail("%s: and %llud grains in all disagree", what, nbad);
	named = countbits(want, sup.ngrains);
	storestat(s, &st);
	checks++;
	if(st.grainfree != sup.ngrains - named - st.staged)
		fail("%s: grainfree %llud, want %llud (%llud grains, %llud "
			"named, %llud staged)", what, st.grainfree,
			sup.ngrains - named - st.staged, sup.ngrains, named,
			st.staged);
	free(want);
	free(got);
}

/*
 * The bitmap after a swap that returned less than everything: `want'
 * grains are marked that no live map names, §6's grainleak says so,
 * and nothing a live map names is clear.
 */
static void
leakok(Store *s, Dev *d, uvlong want, char *what)
{
	Storestat st;
	Super sup;
	uchar *named, *got;
	uvlong g, extra, missing;

	if(storecheckpoint(s) < 0){
		fail("%s: storecheckpoint: %r", what);
		return;
	}
	geom(d, &sup);
	named = scanmaps(d, &sup);
	got = diskbits(d, &sup);
	extra = missing = 0;
	for(g = 0; g < sup.ngrains; g++){
		if(((got[g/8] >> (g%8)) & 1) != 0
		&& ((named[g/8] >> (g%8)) & 1) == 0)
			extra++;
		if(((got[g/8] >> (g%8)) & 1) == 0
		&& ((named[g/8] >> (g%8)) & 1) != 0)
			missing++;
	}
	eqv("the grains the pass folded before the leak stay marked",
		extra, want);
	eqv("and no grain a live map names is clear", missing, 0);
	storestat(s, &st);
	eqv("and grainleak is what stands", st.grainleak, want);
	free(named);
	free(got);
	USED(what);
}

/* fold every slot but one; ~0 skips nothing */
static int
foldall(Store *s, ulong skip, char *what)
{
	Storestat st;
	ulong slot;

	storestat(s, &st);
	for(slot = 0; slot < st.nslots; slot++){
		if(slot == skip)
			continue;
		if(bmpassfold(s, slot) < 0){
			fail("%s: bmpassfold %lud: %r", what, slot);
			return -1;
		}
	}
	return 0;
}

/*
 * The fold a test parks at §13's bmfold point, in a proc of its own:
 * storehook has to run while the fold is inside the engine, and
 * errstr is per-proc, so what it was answered is copied out here.
 */
static Store *foldstore;
static ulong foldslot;
static int folddone, foldr;
static char folderr[ERRMAX];

static void
foldproc(void *a)
{
	USED(a);
	folderr[0] = '\0';
	if((foldr = bmpassfold(foldstore, foldslot)) < 0)
		rerrstr(folderr, sizeof folderr);
	folddone = 1;
}

static void
foldstart(Store *s, ulong slot)
{
	foldstore = s;
	foldslot = slot;
	folddone = 0;
	foldr = 0;
	if(spawnproc(foldproc, nil) < 0)
		fail("spawn: %r");
}

/*
 * A fold counts itself in flight under qlstate once its map read is
 * done and parks inside that same hold, so a storestat that comes
 * back with a fold in flight is a fold that is parked: there is no
 * other way for it to have released the lock.  nre is how many
 * re-reads the fold must have behind it, which is what tells one
 * parked round of a fold from the next.
 */
static int
waitpark(Store *s, uvlong nre, char *what)
{
	Storestat st;
	int k;

	memset(&st, 0, sizeof st);
	for(k = 0; k < 2000; k++){
		storestat(s, &st);
		if(st.bmfolding >= 1 && st.bmreread >= nre)
			break;
		sleep(5);
	}
	istrue(what, st.bmfolding >= 1 && st.bmreread >= nre);
	return st.bmfolding >= 1 && st.bmreread >= nre;
}

static int
waitfold(char *what)
{
	int k;

	for(k = 0; k < 2000 && !folddone; k++)
		sleep(5);
	istrue(what, folddone);
	return folddone;
}

/*
 * Condemn one object's extent map, and delete it when del is set,
 * which is §6's leak: the delete's nfree names nothing, the grains
 * stay marked and named by nothing, and grainleak counts them.  With
 * del clear the slot is left condemned and live, which is the same
 * grains reached the other way — the map is damage either way, and
 * the pass folds neither.  Answers the store; sup is the geometry.
 */
static Store*
withleak(Dev *d, Super *sup, char *name, long blk, int del, char *what)
{
	Store *s;
	Storestat st;
	Objinfo oi;
	uchar *buf, oid[Oidmax];
	ulong emapslot;

	if((s = openbm(d, what)) == nil)
		return nil;
	buf = mkbuf(3*blk, 113);
	mk(s, "live");
	mustwr(s, "live", buf, 3*blk, 0, 2);
	mk(s, "one");
	mustwr(s, "one", buf, blk, 0, 2);	/* §2.3's inline map */
	mk(s, name);
	mustwr(s, name, buf, 3*blk, 0, 2);
	if(storecheckpoint(s) < 0)
		fail("%s: storecheckpoint: %r", what);
	geom(d, sup);
	if(ostat(s, name, &oi) < 0)
		fail("%s: objstat %s: %r", what, name);
	emapslot = oi.emapslot;
	storeclose(s);
	damageentry(d, sup, emapslot);
	free(buf);

	if((s = openbm(d, what)) == nil)
		return nil;
	oidof(oid, name);
	if((buf = malloc(blk)) == nil)
		sysfatal("malloc: %r");
	checks++;
	if(objread(s, oid, strlen(name), buf, blk, 0) >= 0)
		fail("%s: a damaged extent map was served", what);
	free(buf);
	storestat(s, &st);
	eqv("the first read condemns the slot", st.nlost, 1);
	if(!del)
		return s;
	checks++;
	if(objremove(s, oid, strlen(name), 3, 1, nil, 0) < 0)
		fail("%s: objremove of a condemned slot: %r", what);
	storestat(s, &st);
	eqv("and the delete leaks the damaged map's grains", st.grainleak, 3);
	return s;
}

/*
 * §8 and D18: a slot §5 step 10 condemned is damage, its map is never
 * read, and the grains that map named are exactly what the pass
 * reclaims — whether the record over it has been deleted yet or not.
 * This is the undeleted half; tonline is the deleted one.
 */
static void
tlive(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Super sup;
	uvlong gf, np;

	np = 0;
	d = newdisk();
	if((s = withleak(d, &sup, "d", Blk, 0, "a live condemned slot"))
	== nil){
		devclose(d);
		return;
	}
	storestat(s, &st);
	gf = st.grainfree;
	eqv("nothing has counted a leak yet", st.grainleak, 0);
	checks++;
	if(bmpassbegin(s) < 0)
		fail("bmpassbegin: %r");
	if(foldall(s, ~0UL, "the walk") == 0){
		checks++;
		if(bmpassend(s, &np) < 0)
			fail("bmpassend: %r");
	}
	storestat(s, &st);
	eqv("the damaged map's grains come back without a delete",
		st.grainfree, gf + 3);
	eqv("and the copy is still in /lost", st.nlost, 1);
	scanok(s, d, "a pass over a live condemned slot");
	whole(s, "live", "a pass over a live condemned slot");
	whole(s, "one", "a pass over a live condemned slot");
	storeclose(s);
	devclose(d);
}

/*
 * T1.28's first half: a store with a condemned slot, walked online.
 * The rebuilt bitmap equals a full scan of the live maps, grainleak
 * returns to zero, and grainfree is right with a stage's grains
 * outstanding — which the shadow must not mark, because a staged
 * grain carries no bitmap bit (§6).
 */
static void
tonline(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Stage *stg;
	Super sup;
	uchar *buf, sid[Oidmax];
	uvlong gf, np;

	np = 0;
	d = newdisk();
	if((s = withleak(d, &sup, "d", Blk, 1, "an online rebuild")) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(2*Blk, 71);
	oidof(sid, "staged");
	if((stg = stageopen(s, sid, 6, 2*Blk, 0)) == nil)
		fail("stageopen: %r");
	else if(stagewrite(stg, buf, 2*Blk, 0) < 0)
		fail("stagewrite: %r");
	storestat(s, &st);
	eqv("the stage holds two grains", st.staged, 2);
	gf = st.grainfree;

	checks++;
	if(bmpassbegin(s) < 0)
		fail("bmpassbegin: %r");
	storestat(s, &st);
	eqv("a pass is live", st.bmpass, 1);
	checks++;
	if(bmpassbegin(s) >= 0)
		fail("a second pass was begun under the first");
	if(foldall(s, ~0UL, "the walk") == 0){
		checks++;
		if(bmpassend(s, &np) < 0)
			fail("bmpassend: %r");
	}
	storestat(s, &st);
	eqv("the pass ends", st.bmpass, 0);
	eqv("the walk folded every slot that holds a record", st.bmfolded, 3);
	eqv("nothing moved under it", st.bmreread, 0);
	istrue("and the swap installed a page", np >= 1 && st.bmswapped == np);
	eqv("the condemned map's grains come back", st.grainfree, gf + 3);
	eqv("so the leak is discharged", st.grainleak, 0);
	eqv("and the stage's grains are still the stage's", st.staged, 2);
	scanok(s, d, "an online rebuild over a condemned slot");
	whole(s, "live", "an online rebuild");
	whole(s, "one", "an online rebuild");

	/*
	 * The swap left the stage alone, so the stage still commits and
	 * the object it commits is whole: a shadow that had marked the
	 * staged grains would have handed them to the next allocation.
	 */
	checks++;
	if(stagefinal(stg, 1, 1, nil, 0) < 0)
		fail("stagefinal after a swap: %r");
	storestat(s, &st);
	eqv("the staged grains become the object's", st.staged, 0);
	whole(s, "staged", "an online rebuild");
	scanok(s, d, "a stage committed after a swap");
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * T1.28's second half, the barrier: a commit that lands during the
 * walk is in the bitmap after the swap — in a slot the walk has
 * already folded, where only the barrier can carry it, and in one it
 * has not reached, where the fold does.
 */
static void
tbarrier(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Objinfo oa, oc;
	uchar *buf;
	uvlong np;

	np = 0;
	d = newdisk();
	if((s = openbm(d, "the write barrier")) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(3*Blk, 23);
	mk(s, "a");
	mustwr(s, "a", buf, 3*Blk, 0, 2);
	mk(s, "b");
	mustwr(s, "b", buf, 3*Blk, 0, 2);
	mk(s, "c");
	mustwr(s, "c", buf, 3*Blk, 0, 2);
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	if(ostat(s, "a", &oa) < 0 || ostat(s, "c", &oc) < 0)
		fail("objstat: %r");

	checks++;
	if(bmpassbegin(s) < 0)
		fail("bmpassbegin: %r");
	checks++;
	if(bmpassfold(s, oa.slot) < 0)
		fail("bmpassfold a: %r");
	/*
	 * One commit in a slot the walk has folded and one in a slot it
	 * has not.  The first is the barrier's: nothing folds that slot
	 * again, so the only way its new grain reaches the shadow — and
	 * the only way the grain it replaced leaves it — is the mirror
	 * in grainmark and grainclear.
	 */
	mustwr(s, "a", buf, Blk, Blk, 3);
	mustwr(s, "c", buf, Blk, Blk, 3);
	if(foldall(s, oa.slot, "the rest of the walk") == 0){
		checks++;
		if(bmpassend(s, &np) < 0)
			fail("bmpassend: %r");
	}
	storestat(s, &st);
	eqv("the pass ends", st.bmpass, 0);
	scanok(s, d, "a commit under the walk");
	whole(s, "a", "a commit in a folded slot");
	whole(s, "c", "a commit in an unfolded slot");
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * T1.28's third half, the stamp.  A block repair publishes with the
 * four-tuple unchanged and the map changed (§2.7, §8), so a fold that
 * validated the entry it re-read by the four-tuple would OR a stale
 * map's grains into the shadow.  The fold is parked between its map
 * read and its validation, the repair lands in that window, and the
 * stamp is what sends the fold round again.
 */
static void
treread(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Objinfo oi, o2;
	Super sup;
	uchar *buf, oid[Oidmax];
	uvlong np;
	ulong g;

	np = 0;
	spawnforget();
	d = newdisk();
	if((s = openbm(d, "the generation stamp")) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(3*Blk, 59);
	mk(s, "r");
	mustwr(s, "r", buf, 3*Blk, 0, 2);
	oidof(oid, "r");
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	geom(d, &sup);
	if(ostat(s, "r", &oi) < 0)
		fail("objstat r: %r");
	g = grainof(d, &sup, &oi, 1);
	istrue("block 1 has a grain", g != 0);
	flipbytes(d, grainoff(&sup, g), 16);

	checks++;
	if(bmpassbegin(s) < 0)
		fail("bmpassbegin: %r");
	storehook(s, "bmfold", 1);
	foldstart(s, oi.slot);
	if(waitpark(s, 0, "the fold parks with its map read and unvalidated")){
		checks++;
		if(objrepair(s, oid, 1, 1, buf + Blk, Blk) < 0)
			fail("objrepair under the fold: %r");
		if(ostat(s, "r", &o2) < 0)
			fail("objstat r after the repair: %r");
		else{
			eqv("the repair leaves ver unchanged", o2.ver, oi.ver);
			eqv("and wepoch", o2.wepoch, oi.wepoch);
			eqv("and len", o2.len, oi.len);
			checks++;
			if(memcmp(o2.csum, oi.csum, Csumlen) != 0)
				fail("the repair moved the csum: the "
					"four-tuple is not the case any more");
		}
	}
	storehook(s, "bmfold", 0);
	if(waitfold("the parked fold finishes")){
		checks++;
		if(foldr < 0)
			fail("the parked fold: %s", folderr);
	}
	storestat(s, &st);
	eqv("the stamp sent the fold round again", st.bmreread, 1);
	eqv("and it counts the slot once", st.bmfolded, 1);
	if(foldall(s, oi.slot, "the rest of the walk") == 0){
		checks++;
		if(bmpassend(s, &np) < 0)
			fail("bmpassend: %r");
	}
	scanok(s, d, "a map that moved at an unchanged four-tuple");
	whole(s, "r", "a repair under the walk");
	storeclose(s);
	killspawned();
	devclose(d);
	free(buf);
}

/*
 * §8's coverage interlock.  An end whose walk skipped a live slot
 * would free the grains that slot's map names, so it is refused: it
 * installs nothing, the pass stays live, and the end after the
 * missing fold succeeds.  A slot created *under* the walk owes no
 * fold — the create names nothing and every grain written into it
 * afterwards reaches the shadow through the barrier.
 */
static void
tcover(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Super sup;
	Objinfo oi;
	uchar *buf, *was, *now;
	uvlong gf, np, bytes;

	np = 0;
	d = newdisk();
	if((s = openbm(d, "the coverage interlock")) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(3*Blk, 17);
	mk(s, "kept");
	mustwr(s, "kept", buf, 3*Blk, 0, 2);
	mk(s, "skipped");
	mustwr(s, "skipped", buf, 3*Blk, 0, 2);
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	geom(d, &sup);
	if(ostat(s, "skipped", &oi) < 0)
		fail("objstat skipped: %r");
	bytes = nbmpage(&sup)*(bmbits(sup.blksz)/8);
	was = diskbits(d, &sup);
	storestat(s, &st);
	gf = st.grainfree;

	checks++;
	if(bmpassbegin(s) < 0)
		fail("bmpassbegin: %r");
	if(foldall(s, oi.slot, "a walk that skips one live slot") == 0){
		checks++;
		if(bmpassend(s, &np) >= 0)
			fail("an end with a live slot unfolded installed "
				"the shadow");
	}
	storestat(s, &st);
	eqv("a refused end leaves the pass live", st.bmpass, 1);
	eqv("and installs nothing", st.grainfree, gf);
	eqv("and no page", np, 0);
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint after the refusal: %r");
	now = diskbits(d, &sup);
	checks++;
	if(memcmp(was, now, bytes) != 0)
		fail("a refused end changed the live bitmap");
	free(was);
	free(now);

	/*
	 * An ordinary write does not cover the slot: the blocks it
	 * leaves alone are still the old map's, and those grains reach
	 * the shadow only through a fold.
	 */
	mustwr(s, "skipped", buf, Blk, Blk, 3);
	checks++;
	if(bmpassend(s, &np) >= 0)
		fail("a write under the walk covered the slot it did not "
			"fold");

	mk(s, "fresh");			/* created under the walk */
	mustwr(s, "fresh", buf, Blk, 0, 2);
	checks++;
	if(bmpassfold(s, oi.slot) < 0)
		fail("bmpassfold of the slot the walk skipped: %r");
	checks++;
	if(bmpassend(s, &np) < 0)
		fail("bmpassend once every live slot is folded: %r");
	storestat(s, &st);
	eqv("the pass ends", st.bmpass, 0);
	scanok(s, d, "an end whose walk covered the store");
	whole(s, "kept", "the coverage interlock");
	whole(s, "skipped", "the coverage interlock");
	whole(s, "fresh", "the coverage interlock");
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * §6's leak, recorded after the fold that put those grains in the
 * shadow.  The swap installs them marked and named by nothing, so the
 * count stands rather than being discharged — tonline is the other
 * case, where the leak precedes the fold and the swap returns the
 * grains.  The engine's only path to a condemnation is the first read
 * of a damaged map, so the map is damaged on the media after the fold
 * has read it and driven out of §9's LRU before the read that finds
 * the damage.
 */
static void
tleak(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Super sup;
	Objinfo oi;
	uchar *buf, oid[Oidmax];
	uvlong gf, np;

	np = 0;
	d = newdisk();
	if((s = openbmcap(d, "a leak after the fold", 1)) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(3*Blk, 113);
	mk(s, "live");
	mustwr(s, "live", buf, 3*Blk, 0, 2);
	mk(s, "d");
	mustwr(s, "d", buf, 3*Blk, 0, 2);
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	geom(d, &sup);
	if(ostat(s, "d", &oi) < 0)
		fail("objstat d: %r");
	storestat(s, &st);
	gf = st.grainfree;

	checks++;
	if(bmpassbegin(s) < 0)
		fail("bmpassbegin: %r");
	if(foldall(s, ~0UL, "the walk") < 0){
		storeclose(s);
		devclose(d);
		free(buf);
		return;
	}
	damageentry(d, &sup, oi.emapslot);
	oidof(oid, "live");
	checks++;
	if(objread(s, oid, strlen("live"), buf, Blk, 0) < 0)
		fail("objread live: %r");	/* and d's map leaves the LRU */
	oidof(oid, "d");
	checks++;
	if(objread(s, oid, strlen("d"), buf, Blk, 0) >= 0)
		fail("a damaged extent map was served");
	storestat(s, &st);
	eqv("the read condemns the slot the walk had folded", st.nlost, 1);
	checks++;
	if(objremove(s, oid, strlen("d"), 3, 1, nil, 0) < 0)
		fail("objremove of a condemned slot: %r");
	storestat(s, &st);
	eqv("and the delete leaks the folded map's grains", st.grainleak, 3);

	checks++;
	if(bmpassend(s, &np) < 0)
		fail("bmpassend: %r");
	storestat(s, &st);
	eqv("the pass ends", st.bmpass, 0);
	eqv("a leak recorded after the fold is not discharged",
		st.grainleak, 3);
	eqv("and the swap returns none of those grains", st.grainfree, gf);
	leakok(s, d, 3, "a leak recorded after the fold");
	whole(s, "live", "a leak after the fold");
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * A fold that read a damaged map, parked, and woke to find a fresh
 * good one in its place.  §3.6's op=full rebuilds a condemned copy's
 * map whole in a new extent-map slot, which moves the stamp with the
 * four-tuple unchanged, so the fold must go round again: condemning
 * on what it read would condemn the repair, and folding nothing would
 * let the swap clear the grains the new map names.
 */
static void
tmoved(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Stage *stg;
	Super sup;
	Objinfo oi;
	uchar *buf, *fresh, oid[Oidmax];
	ulong emapslot;
	uvlong np;

	np = 0;
	spawnforget();
	d = newdisk();
	if((s = openbm(d, "a map that moved under a damaged read")) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(3*Blk, 149);
	fresh = mkbuf(3*Blk, 151);
	mk(s, "live");
	mustwr(s, "live", buf, 3*Blk, 0, 2);
	mk(s, "d");
	mustwr(s, "d", buf, 3*Blk, 0, 2);
	oidof(oid, "d");
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	geom(d, &sup);
	if(ostat(s, "d", &oi) < 0)
		fail("objstat d: %r");
	emapslot = oi.emapslot;
	storeclose(s);
	damageentry(d, &sup, emapslot);

	if((s = openbm(d, "a damaged map, reopened")) == nil){
		devclose(d);
		free(buf);
		free(fresh);
		return;
	}
	storestat(s, &st);
	eqv("nothing has read the map, so nothing is condemned yet",
		st.nlost, 0);
	checks++;
	if(bmpassbegin(s) < 0)
		fail("bmpassbegin: %r");
	storehook(s, "bmfold", 1);
	foldstart(s, oi.slot);
	if(waitpark(s, 0, "the fold parks with a damaged map read")){
		/* §3.6's repair of the copy, in the window the fold sits in */
		if((stg = stageopen(s, oid, 1, 3*Blk, 0)) == nil)
			fail("stageopen over the damaged copy: %r");
		else{
			if(stagewrite(stg, fresh, 3*Blk, 0) < 0)
				fail("stagewrite: %r");
			checks++;
			if(stagefinal(stg, 3, 1, nil, 0) < 0)
				fail("an op=full over the damaged copy: %r");
		}
	}
	storehook(s, "bmfold", 0);
	if(waitfold("the parked fold finishes")){
		checks++;
		if(foldr < 0)
			fail("the parked fold: %s", folderr);
	}
	storestat(s, &st);
	eqv("the stamp sent the fold round again", st.bmreread, 1);
	eqv("and the fold condemned nothing", st.nlost, 0);
	if(foldall(s, oi.slot, "the rest of the walk") == 0){
		checks++;
		if(bmpassend(s, &np) < 0)
			fail("bmpassend: %r");
	}
	storestat(s, &st);
	eqv("the pass ends", st.bmpass, 0);
	eqv("and returns what the repair left behind", st.grainleak, 0);
	scanok(s, d, "a fresh map published under a damaged read");
	whole(s, "d", "a repair under the walk");
	whole(s, "live", "a repair under the walk");
	storeclose(s);
	killspawned();
	devclose(d);
	free(buf);
	free(fresh);
}

/* the multi-page geometry: §2.5's swap is page by page, so it needs
 * more than one page to be a swap of anything but the whole */
static Dev*
bigdisk(void)
{
	Dev *d;
	Super s;
	Fmtcfg c;

	if((d = simopen(Bigsec, Bignsec, Tseed)) == nil)
		sysfatal("simopen: %r");
	memset(&c, 0, sizeof c);
	c.secsz = Bigsec;
	c.blksz = Bigblk;
	c.objmax = 65536;
	c.nslots = 128;
	c.nemap = 32;
	c.ndirty = 64;
	c.logbytes = 128*1024;
	c.csumalg = Csumblake2s;
	if(geometry(&s, &c, d->size) < 0)
		sysfatal("geometry: %r");
	if(fmtstore(d, &s) < 0)
		sysfatal("fmtstore: %r");
	return d;
}

/*
 * T1.29's first half: the swap installs and dirties the pages that
 * differ and no others.  §5 step 11's rebuild dirties every page,
 * which on a serving store is §2.5's whole bitmap through the
 * checkpoint; the walk's objects all sit in the first page, so the
 * swap that follows is one page of the store's several and the
 * checkpoint after it writes exactly one.
 */
static void
tswap(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Simop *t;
	Super sup;
	vlong lo, hi;
	uvlong np;
	long i, nw;

	np = 0;
	d = bigdisk();
	if((s = withleak(d, &sup, "d", Bigblk, 1, "the page-by-page swap"))
	== nil){
		devclose(d);
		return;
	}
	istrue("the store has more than one bitmap page", nbmpage(&sup) > 1);
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	checks++;
	if(bmpassbegin(s) < 0)
		fail("bmpassbegin: %r");
	if(foldall(s, ~0UL, "the walk") == 0){
		simtracereset(d);
		checks++;
		if(bmpassend(s, &np) < 0)
			fail("bmpassend: %r");
	}
	eqv("the swap installs the one page that differs", np, 1);
	storestat(s, &st);
	eqv("and says so", st.bmswapped, np);
	eqv("the leak is discharged", st.grainleak, 0);

	if(storecheckpoint(s) < 0)
		fail("storecheckpoint after the swap: %r");
	lo = (vlong)sup.bmapoff*sup.secsz;
	hi = (vlong)sup.dataoff*sup.secsz;
	nw = 0;
	for(i = 0; i < simtrace(d, &t); i++)
		if(t[i].op == Sopwrite && t[i].off >= lo && t[i].off < hi)
			nw++;
	eqv("so the checkpoint writes one bitmap page", nw, 1);
	scanok(s, d, "a page-by-page swap");
	whole(s, "live", "a page-by-page swap");
	storeclose(s);
	devclose(d);
}

/*
 * T1.29's second half: an abort leaves the live bitmap exactly as it
 * was and the barrier disarmed, so an ordinary commit follows it and
 * a second pass may be begun.
 */
static void
tabort(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Super sup;
	uchar *buf, *was, *now;
	uvlong gf, np, bytes;

	np = 0;
	d = newdisk();
	if((s = withleak(d, &sup, "d", Blk, 1, "an aborted pass")) == nil){
		devclose(d);
		return;
	}
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	bytes = nbmpage(&sup)*(bmbits(sup.blksz)/8);
	was = diskbits(d, &sup);
	storestat(s, &st);
	gf = st.grainfree;

	checks++;
	if(bmpassbegin(s) < 0)
		fail("bmpassbegin: %r");
	if(foldall(s, ~0UL, "the aborted walk") == 0)
		bmpassabort(s);
	storestat(s, &st);
	eqv("an abort ends the pass", st.bmpass, 0);
	eqv("and installs nothing", st.grainfree, gf);
	eqv("so the leak still stands", st.grainleak, 3);
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint after the abort: %r");
	now = diskbits(d, &sup);
	checks++;
	if(memcmp(was, now, bytes) != 0)
		fail("an aborted pass changed the live bitmap");
	free(was);
	free(now);

	/*
	 * The barrier is disarmed, so this commit is an ordinary one —
	 * and a second pass finds the same leak and returns it.
	 */
	buf = mkbuf(Blk, 91);
	mustwr(s, "live", buf, Blk, 0, 3);
	checks++;
	if(bmpassbegin(s) < 0)
		fail("bmpassbegin after an abort: %r");
	if(foldall(s, ~0UL, "the second walk") == 0){
		checks++;
		if(bmpassend(s, &np) < 0)
			fail("bmpassend: %r");
	}
	storestat(s, &st);
	eqv("the pass after an abort returns the grains", st.grainfree,
		gf + 3);
	eqv("and discharges the leak", st.grainleak, 0);
	scanok(s, d, "a pass after an abort");
	whole(s, "live", "a pass after an abort");
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * T1.29's third half: a pass MUST be ended or aborted before
 * storeclose, because every call on a closed store is undefined
 * (D16).  A pass still live when one runs is aborted by it, and a
 * fold parked at §13's hold point is let go rather than left asleep
 * in a pass that no longer exists.  The snapshot is what keeps the
 * Store's memory alive across the close (§9), so the woken fold reads
 * memory that is still there and the window is a driven one.
 */
static void
tclose(void)
{
	Dev *d;
	Store *s;
	Objsnap *sn;
	Objinfo oi;
	uchar *buf;

	spawnforget();
	d = newdisk();
	if((s = openbm(d, "a close under a live pass")) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(3*Blk, 37);
	mk(s, "x");
	mustwr(s, "x", buf, 3*Blk, 0, 2);
	if(ostat(s, "x", &oi) < 0)
		fail("objstat x: %r");
	if((sn = objsnapopen(s, Snaplive)) == nil)
		fail("objsnapopen: %r");
	checks++;
	if(bmpassbegin(s) < 0)
		fail("bmpassbegin: %r");
	storehook(s, "bmfold", 1);
	foldstart(s, oi.slot);
	if(waitpark(s, 0, "the fold parks under the live pass")){
		storeclose(s);
		eqv("a store closed under an open snapshot is not freed",
			nfreed, 0);
		if(waitfold("the close lets the parked fold go")){
			checks++;
			if(foldr >= 0)
				fail("a fold in a pass the close dropped "
					"answered success");
		}
	}else{
		storehook(s, "bmfold", 0);
		waitfold("the fold finishes");
		storeclose(s);
	}
	if(sn != nil)
		objsnapclose(sn);
	eqv("and the last snapshot frees the store", nfreed, 1);
	killspawned();
	devclose(d);
	free(buf);
}

void
main(int argc, char **argv)
{
	USED(argc); USED(argv);
	tonline();
	tlive();
	tbarrier();
	treread();
	tcover();
	tleak();
	tmoved();
	tswap();
	tabort();
	tclose();
	killspawned();
	if(fails > 0){
		print("bmrebuildtest: %d of %d checks FAILED\n", fails, checks);
		exits("failed");
	}
	print("bmrebuildtest: %d checks ok\n", checks);
	exits(nil);
}
