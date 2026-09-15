#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"
#include "t1.h"

/*
 * T1 for docs/design/store.md §8 — verify, scrub and repair as the
 * engine builds them: what a corrupt-flagged copy answers (§3.7's
 * row), the durable transition objscrub makes, block repair and the
 * two refusals it owes, the slot cursor, and /lost's accounting over
 * every way a copy comes to fail local verification.
 *
 * §13's T1.17 is the arraybad refusal in trepair below.
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

/*
 * §3.7: an internal-invariant error MUST NOT begin with one of layer-a
 * §2.6's prefixes, because §2.6's set is prefix-free and a client
 * parsing a prefix out of one of these reads a bug as an ordinary
 * refusal.  The one that would be reached for by mistake on the
 * block-repair path is `checksum mismatch', which is what the *other*
 * refusal there answers, so the two are told apart on the wire.
 */
static void
notwire(char *what, int r)
{
	char e[ERRMAX];

	checks++;
	if(r >= 0){
		fail("%s was accepted", what);
		return;
	}
	rerrstr(e, sizeof e);
	checks++;
	if(strncmp(e, "checksum mismatch", 17) == 0
	|| strncmp(e, "no such object", 14) == 0
	|| strncmp(e, "object ", 7) == 0
	|| strncmp(e, "stale version", 13) == 0
	|| strncmp(e, "bad ctl", 7) == 0
	|| strncmp(e, "not discardable", 15) == 0
	|| strncmp(e, "disk full", 9) == 0)
		fail("%s: %s, want an error with no layer-a 2.6 prefix", what,
			e);
}

static void
refusedinternal(char *what, int r)
{
	char e[ERRMAX];

	notwire(what, r);
	if(r >= 0)
		return;
	rerrstr(e, sizeof e);
	checks++;
	if(strncmp(e, "block repair", 12) != 0)
		fail("%s: %s, want the block-repair refusal", what, e);
}

static void
scrub(Store *s, char *name, Vfy *v, char *what)
{
	uchar o[Oidmax];

	memset(v, 0, sizeof *v);
	oidof(o, name);
	if(objscrub(s, o, strlen(name), v) < 0)
		fail("%s: objscrub %s: %r", what, name);
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
 * Damage one stored block digest and leave the extent-map entry's own
 * csum128 correct, so the entry is served and hash(dig[]) != csum:
 * §8's second kind of mismatch, which is T1.17's.  Damaging the entry
 * without repairing its checksum is the *other* fault — §5 step 10's
 * condemnation — and tlost drives that one.
 */
static void
damagedigest(Dev *d, Super *sup, ulong emapslot, ulong blk)
{
	uchar *p, *dig;
	int i;

	if((p = malloc(sup->emapsz)) == nil)
		sysfatal("malloc: %r");
	simpeek(d, emapentoff(sup, emapslot), p, sup->emapsz);
	dig = emapdig(p, sup->nblkmax, blk);
	for(i = 0; i < Blkdlen; i++)
		dig[i] = ~dig[i];
	reccsumset(p, sup->emapsz, 0);
	simpoke(d, emapentoff(sup, emapslot), p, sup->emapsz);
	free(p);
}

/*
 * Damage an extent-map entry without repairing its csum128, so the
 * entry itself fails and §5 step 10 condemns the slot on the first
 * read — the other kind of damage from damagedigest above.
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
 * Decision (1) of §8, through §3.7's row: a copy whose corrupt flag
 * is set fails client access with layer-a §2.6's `checksum mismatch'.
 * Read, write and truncate refuse; objstat, objverify and objdiscard
 * do not, because they are how the flag is seen, how it is cleared
 * and how a tombstone is dropped; a create of the live id is still
 * `object exists'; and a delete applies, because op=delete is
 * self-contained and a tombstone holds no content to be suspect of —
 * which is also what takes the object out of /lost.
 */
static void
taccess(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Objinfo oi;
	Vfy v;
	uchar *buf, oid[Oidmax];

	d = newdisk();
	if((s = mustopen(d, "corrupt-flagged access")) == nil)
		return;
	buf = mkbuf(2*Blk, 31);
	mk(s, "a");
	mustwr(s, "a", buf, 2*Blk, 0, 2);
	oidof(oid, "a");
	if(objcorrupt(s, oid, 1, 1, nil, 0) < 0)
		fail("objcorrupt: %r");

	refused("objread of a corrupt copy", objread(s, oid, 1, buf, Blk, 0),
		"checksum mismatch");
	refused("objwrite of a corrupt copy",
		objwrite(s, oid, 1, buf, Blk, 0, 3, 1, nil, 0),
		"checksum mismatch");
	/*
	 * A count-0 write commits nothing, so the refusal has to be made
	 * before the shortcut that answers it ok — a zero-count Twrite is
	 * an ordinary 9P client call, and answering it ok is client
	 * access served on a copy that fails verification.
	 */
	refused("a count-0 objwrite of a corrupt copy",
		objwrite(s, oid, 1, buf, 0, 0, 3, 1, nil, 0),
		"checksum mismatch");
	refused("objtrunc of a corrupt copy",
		objtrunc(s, oid, 1, Blk, 3, 1, nil, 0), "checksum mismatch");
	refused("objcreate over a live corrupt copy",
		objcreate(s, oid, 1, 3, 1, nil, 0, nil), "object exists");

	checks++;
	if(objstat(s, oid, 1, &oi) < 0)
		fail("objstat of a corrupt copy: %r");
	else
		eqv("objstat answers the flag", oi.corrupt, 1);
	checks++;
	if(objverify(s, oid, 1, &v) < 0)
		fail("objverify of a corrupt copy: %r");
	else{
		eqv("objverify is not refused by the flag", v.nbad, 0);
		eqv("and finds the content whole", v.arraybad, 0);
		vfyfree(&v);
	}
	storestat(s, &st);
	eqv("the corrupt copy is in /lost", st.nlost, 1);

	/*
	 * The delete applies and its tombstone clears the flag.  A
	 * tombstone holds no content, so there is nothing left for the
	 * flag to describe, and leaving it set would keep a slot in
	 * /lost that names no failing copy.
	 */
	checks++;
	if(objremove(s, oid, 1, 3, 1, nil, 0) < 0)
		fail("objremove of a corrupt copy: %r");
	if(objstat(s, oid, 1, &oi) < 0)
		fail("objstat after the delete: %r");
	else{
		eqv("the delete tombstoned it", oi.state, Stomb);
		eqv("and the tombstone clears the flag", oi.corrupt, 0);
	}
	storestat(s, &st);
	eqv("so it leaves /lost", st.nlost, 0);
	/*
	 * A tombstone can carry the flag only because objcorrupt will set
	 * one — a scrub cannot, since a tombstone has no content to
	 * mismatch — and the discard that then releases the slot has to
	 * take it back out of /lost: the list is slots, and a released
	 * slot holds no copy at all.
	 */
	checks++;
	if(objcorrupt(s, oid, 1, 1, nil, 0) < 0)
		fail("objcorrupt of the tombstone: %r");
	storestat(s, &st);
	eqv("a flagged tombstone is listed", st.nlost, 1);
	checks++;
	if(objdiscard(s, oid, 1, 3, 1, 2) < 0)
		fail("objdiscard of the tombstone: %r");
	storestat(s, &st);
	eqv("and the discard delists the slot it released", st.nlost, 0);

	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * Decision (3): objscrub is objverify plus the durable transition.
 * A mismatch on a copy the index calls whole sets the flag, every
 * block matching on a copy it calls corrupt clears it, and an
 * unchanged verdict commits nothing.  Both transitions survive a
 * restart, which is the whole reason the flag is an Eobj and not a
 * bit in memory.
 */
static void
tscrub(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Objinfo oi;
	Super sup;
	Vfy v;
	uchar *buf, oid[Oidmax];
	uvlong lf, gf;
	ulong g;

	d = newdisk();
	if((s = mustopen(d, "scrub")) == nil)
		return;
	buf = mkbuf(3*Blk, 41);
	mk(s, "s");
	mustwr(s, "s", buf, 3*Blk, 0, 2);
	oidof(oid, "s");

	storestat(s, &st);
	lf = st.logfree;
	scrub(s, "s", &v, "a whole copy");
	eqv("a scrub of a whole copy finds nothing", v.nbad, 0);
	eqv("and no bad digest array", v.arraybad, 0);
	vfyfree(&v);
	storestat(s, &st);
	eqv("and commits nothing", st.logfree, lf);
	eqv("and lists nothing", st.nlost, 0);

	/*
	 * Damage the bytes of block 1 and nothing else: the digest array
	 * still hashes to csum, so this is §8's first kind of mismatch —
	 * the content is suspect and block repair is the repair.
	 */
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	geom(d, &sup);
	if(ostat(s, "s", &oi) < 0)
		fail("objstat s: %r");
	g = grainof(d, &sup, &oi, 1);
	istrue("block 1 has a grain", g != 0);
	flipbytes(d, grainoff(&sup, g), 16);

	scrub(s, "s", &v, "a damaged block");
	eqv("the scrub finds one bad block", v.nbad, 1);
	if(v.nbad == 1)
		eqv("and names it", v.bad[0], 1);
	eqv("the digest array is not the suspect", v.arraybad, 0);
	vfyfree(&v);
	if(ostat(s, "s", &oi) < 0)
		fail("objstat s: %r");
	else
		eqv("the scrub set the corrupt flag", oi.corrupt, 1);
	storestat(s, &st);
	eqv("and listed the object in /lost", st.nlost, 1);

	/*
	 * Durable: a restart must not forget it.  The checkpoint is what
	 * makes this discriminating — without it the restart replays the
	 * flag's own Eobj and would list the object however start-up read
	 * the index, so the flag would look durable while the index bit
	 * it is written into was ignored.
	 */
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storeclose(s);
	if((s = mustopen(d, "scrub replayed")) == nil){
		devclose(d);
		free(buf);
		return;
	}
	if(ostat(s, "s", &oi) < 0)
		fail("objstat s: %r");
	else
		eqv("the flag survives a restart", oi.corrupt, 1);
	storestat(s, &st);
	eqv("and so does /lost", st.nlost, 1);

	/* repair the block, then let the scrub clear the flag */
	storestat(s, &st);
	gf = st.grainfree;
	checks++;
	if(objrepair(s, oid, 1, 1, buf + Blk, Blk) < 0)
		fail("objrepair: %r");
	storestat(s, &st);
	eqv("a repair frees the grain it replaced", st.grainfree, gf);
	if(ostat(s, "s", &oi) < 0)
		fail("objstat s: %r");
	else
		eqv("and does not itself clear the flag", oi.corrupt, 1);

	scrub(s, "s", &v, "after the repair");
	eqv("the next scrub finds every block matching", v.nbad, 0);
	eqv("with a good digest array", v.arraybad, 0);
	vfyfree(&v);
	if(ostat(s, "s", &oi) < 0)
		fail("objstat s: %r");
	else
		eqv("so it clears the flag", oi.corrupt, 0);
	storestat(s, &st);
	eqv("and the object leaves /lost", st.nlost, 0);

	/* and the clear is durable too */
	storeclose(s);
	if((s = mustopen(d, "scrub cleared, replayed")) == nil){
		devclose(d);
		free(buf);
		return;
	}
	if(ostat(s, "s", &oi) < 0)
		fail("objstat s: %r");
	else
		eqv("the cleared flag survives a restart", oi.corrupt, 0);
	storestat(s, &st);
	eqv("and /lost is empty", st.nlost, 0);
	checks++;
	if(objread(s, oid, 1, buf, Blk, 0) < 0)
		fail("a repaired object is not served: %r");
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * Decision (5): block repair, and the two refusals §8 owes.  The
 * second of them is §13's T1.17: with hash(dig[]) != csum the stored
 * dig[i] cannot be the acceptance test for anything, so a block
 * repair — even of a block whose own digest is intact and whose bytes
 * are right — must be refused, and refused as a caller bug rather
 * than as a media fault, because §8's repair for that object is the
 * whole-object op=full.  Accepting there rejects every correct byte a
 * peer sends for the damaged blocks and leaves the object lost with
 * good copies all over the cluster.
 */
static void
trepair(void)
{
	Dev *d;
	Store *s;
	Objinfo oi;
	Super sup;
	Storestat sst;
	Vfy v;
	uchar *buf, *bad, *zeros, *tail, *dirty;
	uchar oid[Oidmax], hid[Oidmax], tid[Oidmax], csum[Csumlen];
	uvlong gf;
	ulong g, emapslot;
	long i;

	d = newdisk();
	if((s = mustopen(d, "block repair")) == nil)
		return;
	buf = mkbuf(3*Blk, 71);
	bad = mkbuf(3*Blk, 72);
	zeros = mkbuf(Blk, 0);
	tail = mkbuf(Blk, 0);
	mk(s, "r");
	mustwr(s, "r", buf, 3*Blk, 0, 2);
	oidof(oid, "r");
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	geom(d, &sup);
	if(ostat(s, "r", &oi) < 0)
		fail("objstat r: %r");
	emapslot = oi.emapslot;

	/*
	 * The Vfy is the caller's on success only.  A verify that fails
	 * part-way — a media error under a grain, which is exactly the
	 * region a wave-1d scrubber is crossing when it finds one —
	 * answers a zeroed Vfy, so the bad-block array it had already
	 * allocated is not left for nobody to free.  vfyfree is safe
	 * afterwards either way, which is what shoal.h promises.
	 */
	g = grainof(d, &sup, &oi, 0);
	memset(&v, 0xff, sizeof v);
	simfaultat(d, Sfeio, 1, grainoff(&sup, g), Blk);
	checks++;
	if(objverify(s, oid, 1, &v) >= 0)
		fail("objverify read a grain the device refused");
	istrue("a failed verify leaves no bad-block array", v.bad == nil);
	eqv("and answers a zeroed Vfy", v.nbad, 0);
	eqv("and no verdict on the digest array", v.arraybad, 0);
	vfyfree(&v);				/* safe, whatever it returned */
	simfault(d, Sfnone, 0);

	g = grainof(d, &sup, &oi, 2);
	flipbytes(d, grainoff(&sup, g), 16);

	/* bytes that do not hash to the stored digest are the peer's fault */
	refused("a block repair with the wrong bytes",
		objrepair(s, oid, 1, 2, bad + 2*Blk, Blk),
		"checksum mismatch");
	/* and a count that is not the block's covered length is the caller's */
	refusedinternal("a block repair of the wrong length",
		objrepair(s, oid, 1, 2, buf + 2*Blk, Blk - 1));
	refusedinternal("a block repair past the last block",
		objrepair(s, oid, 1, 3, buf, Blk));

	/*
	 * Block 0 is whole, and the bytes offered are the ones it already
	 * holds.  §8 drives repair off the set verify answers, so a block
	 * outside that set is a caller bug: the commit would change
	 * nothing and cost a grain.
	 */
	refusedinternal("a block repair of a block that already matches",
		objrepair(s, oid, 1, 0, buf, Blk));

	checks++;
	if(objrepair(s, oid, 1, 2, buf + 2*Blk, Blk) < 0)
		fail("objrepair: %r");
	checks++;
	if(objverify(s, oid, 1, &v) < 0)
		fail("objverify after the repair: %r");
	else{
		eqv("the repaired object verifies", v.nbad, 0);
		eqv("with a good digest array", v.arraybad, 0);
		vfyfree(&v);
	}
	if(ostat(s, "r", &oi) < 0)
		fail("objstat r: %r");
	else{
		eqv("the repair left the version alone", oi.ver, 2);
		eqv("and the length", oi.len, 3*Blk);
	}

	/*
	 * The same repair with the block's grain UNREADABLE.  A media
	 * error under a block is the case §8's repair exists for, so the
	 * read that asks whether the block needs repairing must not turn
	 * its own failure into a refusal: the offered bytes have already
	 * passed the acceptance test above, and a grain that cannot be
	 * read is the plainest case of the repair being needed.
	 *
	 * Mutation: a failed grainread refuses (mut repair-refuses-unread),
	 * and the repair answers the media error it was called to fix.
	 */
	if(ostat(s, "r", &oi) < 0)
		fail("objstat r: %r");
	g = grainof(d, &sup, &oi, 1);
	flipbytes(d, grainoff(&sup, g), 16);
	checks++;
	if(objverify(s, oid, 1, &v) < 0)
		fail("objverify of the flipped block: %r");
	else{
		eqv("the flipped block is the only mismatch", v.nbad, 1);
		eqv("and the digest array is sound", v.arraybad, 0);
		vfyfree(&v);
	}
	simfaultat(d, Sfeio, 1, grainoff(&sup, g), Blk);
	checks++;
	if(objrepair(s, oid, 1, 1, buf + Blk, Blk) < 0)
		fail("a block repair whose old grain cannot be read: %r");
	simfault(d, Sfnone, 0);
	checks++;
	if(objverify(s, oid, 1, &v) < 0)
		fail("objverify after a repair over an unreadable grain: %r");
	else{
		eqv("the object verifies after that repair", v.nbad, 0);
		eqv("and its digest array is sound", v.arraybad, 0);
		vfyfree(&v);
	}

	/*
	 * §4's merge-back: the bytes of the grain above the block's
	 * covered length are not the object's content and MUST read as
	 * zeros, or the next write that covers them merges them in.  A
	 * repair writes a whole grain from a short buffer, so this is
	 * where that rule is easiest to break.
	 */
	mk(s, "t");
	mustwr(s, "t", buf, 2*Blk + 100, 0, 2);
	oidof(tid, "t");
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	if(ostat(s, "t", &oi) < 0)
		fail("objstat t: %r");
	g = grainof(d, &sup, &oi, 2);
	istrue("the short final block has a grain", g != 0);
	/*
	 * Damage the block's content, and the bytes of its grain above
	 * the covered length with it: a media fault does not stop at a
	 * boundary the object model draws, and those bytes are what a
	 * repair that does not zero-fill carries forward — it reads the
	 * grain to decide whether the block needs repairing at all.
	 */
	flipbytes(d, grainoff(&sup, g), 16);
	dirty = mkbuf(Blk - 100, 0);
	memset(dirty, 0xd7, Blk - 100);
	simpoke(d, grainoff(&sup, g) + 100, dirty, Blk - 100);
	free(dirty);
	checks++;
	if(objrepair(s, tid, 1, 2, buf + 2*Blk, 100) < 0)
		fail("objrepair of a short final block: %r");
	/*
	 * The claim is about the bytes on the disk, so it is read off the
	 * disk: every path that reads a block back through the API
	 * zero-fills above the covered length itself, so the grain could
	 * hold anything and no read would say so — until the day one
	 * does not.
	 */
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	if(ostat(s, "t", &oi) < 0)
		fail("objstat t: %r");
	g = grainof(d, &sup, &oi, 2);
	istrue("the repaired block has a grain", g != 0);
	simpeek(d, grainoff(&sup, g) + 100, tail, Blk - 100);
	for(i = 0; i < Blk - 100; i++)
		if(tail[i] != 0)
			break;
	istrue("a repair zeroes the grain above the covered length",
		i == Blk - 100);

	/*
	 * A hole is the sharp form of the same refusal.  Its stored
	 * digest is the zero digest (§4), so the zeros it already reads
	 * as pass the acceptance test — and taking them would allocate a
	 * grain while freeing grain 0, which frees nothing: the object
	 * would come out one grain heavier with the same content and one
	 * hole fewer.
	 */
	mk(s, "h");
	mustwr(s, "h", buf, Blk, 2*Blk, 2);	/* blocks 0 and 1 are holes */
	oidof(hid, "h");
	memset(zeros, 0, Blk);
	storestat(s, &sst);
	gf = sst.grainfree;
	refusedinternal("a block repair of a hole with the zeros it reads as",
		objrepair(s, hid, 1, 0, zeros, Blk));
	storestat(s, &sst);
	eqv("and the refused hole repair costs no grain", sst.grainfree, gf);
	checks++;
	if(objverify(s, hid, 1, &v) < 0)
		fail("objverify of the holed object: %r");
	else{
		eqv("the holed object still verifies", v.nbad, 0);
		vfyfree(&v);
	}

	/*
	 * T1.17.  Damage a stored digest and leave the extent-map entry's
	 * own csum128 right, so the entry is served and the array is the
	 * suspect.  The store must re-read it, so this goes through a
	 * restart.
	 */
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storeclose(s);
	damagedigest(d, &sup, emapslot, 1);
	if((s = mustopen(d, "a damaged digest array")) == nil){
		devclose(d);
		free(buf);
		free(bad);
		return;
	}
	checks++;
	if(objverify(s, oid, 1, &v) < 0)
		fail("objverify over a damaged digest array: %r");
	else{
		eqv("verify calls the digest array suspect", v.arraybad, 1);
		eqv("and block 1 mismatching with it", v.nbad, 1);
		vfyfree(&v);
	}
	/*
	 * Block 0's bytes are right and its stored digest is intact, so
	 * a repair that tested only dig[i] would take them.  §8 refuses:
	 * the array is not an acceptance test, and this object's repair
	 * is the whole-object push.
	 */
	refusedinternal("a block repair of a good block under a bad array",
		objrepair(s, oid, 1, 0, buf, Blk));
	refusedinternal("a block repair of the mismatching block",
		objrepair(s, oid, 1, 1, buf + Blk, Blk));

	/* and the scrub flags it, for the whole-object repair to find */
	if(ostat(s, "r", &oi) < 0)
		fail("objstat r: %r");
	memmove(csum, oi.csum, Csumlen);
	scrub(s, "r", &v, "a damaged digest array");
	eqv("the scrub calls the array suspect", v.arraybad, 1);
	vfyfree(&v);
	if(ostat(s, "r", &oi) < 0)
		fail("objstat r: %r");
	else
		eqv("and sets the flag", oi.corrupt, 1);
	/*
	 * §8's flag commit changes nothing but the flag, and the csum is
	 * part of "nothing but".  Recomputing it from the stored digest
	 * array — the array that is what failed — would publish a csum
	 * that agrees with the damage: arraybad would clear, objrepair's
	 * precondition would pass, and every correct byte a peer sent for
	 * the damaged block would be refused for the life of the disk.
	 * It would also move the four-tuple's csum at an unchanged key,
	 * which is layer-a §1.3's I3 fed a divergence made here.
	 */
	checks++;
	if(memcmp(oi.csum, csum, Csumlen) != 0)
		fail("the scrub rewrote the object's csum");
	checks++;
	if(objverify(s, oid, 1, &v) < 0)
		fail("objverify after the scrub: %r");
	else{
		eqv("the digest array is still the suspect after the scrub",
			v.arraybad, 1);
		vfyfree(&v);
	}
	refusedinternal("a block repair of a good block after the scrub",
		objrepair(s, oid, 1, 0, buf, Blk));
	refusedinternal("a block repair of the mismatching block after the "
		"scrub", objrepair(s, oid, 1, 1, buf + Blk, Blk));
	storeclose(s);
	devclose(d);
	free(buf);
	free(bad);
	free(zeros);
	free(tail);
}

/*
 * A copy §5 step 10 condemned for a damaged extent map: what a block
 * repair answers, what a delete does with it, and where the grains
 * the damaged map named end up.
 */
static void
tcondemned(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Objinfo oi;
	Super sup;
	uchar *buf, oid[Oidmax];
	uvlong gf;
	ulong emapslot;

	d = newdisk();
	if((s = mustopen(d, "a condemned slot")) == nil)
		return;
	buf = mkbuf(3*Blk, 113);
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
	if((s = mustopen(d, "a condemned slot, reopened")) == nil){
		devclose(d);
		free(buf);
		return;
	}
	checks++;
	if(objread(s, oid, 1, buf, Blk, 0) >= 0)
		fail("a damaged extent map was served");
	storestat(s, &st);
	eqv("the first read condemns the slot", st.nlost, 1);
	gf = st.grainfree;

	/*
	 * §3.7: one caller bug, one spelling.  There is no acceptance
	 * test here — the entry that names this block's digest is itself
	 * the damage — which is exactly the arraybad case, so the refusal
	 * is the same internal one and not §2.6's `checksum mismatch',
	 * which a /repl peer reads as a media fault worth retrying.
	 */
	refusedinternal("a block repair of a condemned slot",
		objrepair(s, oid, 1, 0, buf, Blk));

	/*
	 * §8: the delete applies.  op=delete carries its own key and a
	 * condemned copy has none to defend, and refusing would leave the
	 * object with no exit at all — op=full is its other repair, and
	 * an object deleted cluster-wide has no live copy left to push
	 * one, so layer-a §1.5's discard would wait on this witness for
	 * ever.
	 */
	checks++;
	if(objremove(s, oid, 1, 3, 1, nil, 0) < 0)
		fail("objremove of a condemned slot: %r");
	if(objstat(s, oid, 1, &oi) < 0)
		fail("objstat after the delete: %r");
	else{
		eqv("the delete tombstones the condemned copy", oi.state,
			Stomb);
		eqv("and commits a clean tombstone", oi.corrupt, 0);
	}
	storestat(s, &st);
	eqv("so the slot leaves /lost", st.nlost, 0);
	eqv("and the damaged map's grains are still marked used",
		st.grainfree, gf);

	/*
	 * §3.6: those grains come back at a bitmap rebuild and at nothing
	 * else.  The tombstone released the extent-map slot, so §5 step
	 * 11's scan no longer finds them named by anything.
	 */
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storeclose(s);
	if((s = openrebuild(d, "after the delete")) == nil){
		devclose(d);
		free(buf);
		return;
	}
	storestat(s, &st);
	eqv("a bitmap rebuild reclaims the condemned map's grains",
		st.grainfree, gf + 3);
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * D14, and the case §5.5's exemption exists for: a copy that fails
 * local verification contributes no key, so an op=full at a key LOWER
 * than the one it stores must be taken.  Nothing reads the object
 * first, so stagefinal is what finds the damage — the entry it copied
 * out still has the corrupt flag clear, and Ient.bad is the only thing
 * that says the copy has no key.
 */
static void
tlowerkey(void)
{
	Dev *d;
	Store *s;
	Stage *g;
	Storestat st;
	Objinfo oi;
	Super sup;
	Vfy v;
	uchar *buf, *other, oid[Oidmax];
	ulong emapslot;

	d = newdisk();
	if((s = mustopen(d, "a lower-keyed push")) == nil)
		return;
	buf = mkbuf(3*Blk, 127);
	other = mkbuf(3*Blk, 131);
	mk(s, "n");
	mustwr(s, "n", buf, 3*Blk, 0, 5);
	oidof(oid, "n");
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	geom(d, &sup);
	if(ostat(s, "n", &oi) < 0)
		fail("objstat n: %r");
	emapslot = oi.emapslot;
	storeclose(s);
	damageentry(d, &sup, emapslot);
	if((s = mustopen(d, "a lower-keyed push, reopened")) == nil){
		devclose(d);
		free(buf);
		free(other);
		return;
	}
	storestat(s, &st);
	eqv("nothing has read the object, so nothing is condemned yet",
		st.nlost, 0);
	if((g = stageopen(s, oid, 1, 3*Blk, 0)) == nil)
		fail("stageopen: %r");
	else{
		if(stagewrite(g, other, 3*Blk, 0) < 0)
			fail("stagewrite: %r");
		checks++;
		if(stagefinal(g, 3, 1, nil, 0) < 0)
			fail("an op=full at a lower key onto a condemned "
				"copy: %r");
		else if(ostat(s, "n", &oi) < 0)
			fail("objstat n: %r");
		else{
			eqv("the push at the lower key is what the object "
				"now holds", oi.ver, 3);
			eqv("and the repair clears the flag", oi.corrupt, 0);
		}
	}
	checks++;
	if(objverify(s, oid, 1, &v) < 0)
		fail("objverify after the lower-keyed repair: %r");
	else{
		eqv("the repaired copy verifies", v.nbad, 0);
		eqv("with a good digest array", v.arraybad, 0);
		vfyfree(&v);
	}
	storestat(s, &st);
	eqv("and it has left /lost", st.nlost, 0);
	storeclose(s);
	devclose(d);
	free(buf);
	free(other);
}

/*
 * Decision (4): the slot cursor.  It answers what a slot holds, in
 * slot order, copying the oid out — which is what lets a scrubber
 * walk the index without holding the state lock across a verify.
 */
static void
tcursor(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Objinfo oi;
	uchar *buf, oid[Oidmax], got[Oidmax];
	ulong slot;
	int oidlen, r, nlive, ntomb, nfree, seen;

	d = newdisk();
	if((s = mustopen(d, "the slot cursor")) == nil)
		return;
	buf = mkbuf(Blk, 83);
	mk(s, "c1");
	mustwr(s, "c1", buf, Blk, 0, 2);
	mk(s, "c2");
	mk(s, "c3");
	oidof(oid, "c3");
	if(objremove(s, oid, 2, 2, 1, nil, 0) < 0)
		fail("objremove c3: %r");

	storestat(s, &st);
	eqv("the cursor's bound is the index's size", st.nslots, 128);
	nlive = ntomb = nfree = seen = 0;
	for(slot = 0; slot < st.nslots; slot++){
		r = objslot(s, slot, got, &oidlen, &oi);
		if(r < 0){
			fail("objslot %lud: %r", slot);
			break;
		}
		if(r == 0){
			nfree++;
			continue;
		}
		if(oi.state == Slive)
			nlive++;
		else if(oi.state == Stomb)
			ntomb++;
		eqv("the cursor reports the slot it was asked for", oi.slot,
			slot);
		if(oidlen == 2 && memcmp(got, "c1", 2) == 0){
			seen++;
			eqv("and the object's length", oi.len, Blk);
			eqv("and its key", oi.ver, 2);
			eqv("and that it is whole", oi.corrupt, 0);
		}
	}
	eqv("the cursor finds every live object", nlive, st.nlive);
	eqv("and every tombstone", ntomb, st.ntomb);
	eqv("and calls the rest free", nfree, st.nslots - st.nlive - st.ntomb);
	eqv("and named c1 once", seen, 1);
	notwire("a cursor past the last slot",
		objslot(s, (ulong)st.nslots, got, &oidlen, &oi));

	/* it reports a corrupt copy as corrupt rather than hiding it */
	oidof(oid, "c1");
	if(objcorrupt(s, oid, 2, 1, nil, 0) < 0)
		fail("objcorrupt c1: %r");
	seen = 0;
	for(slot = 0; slot < st.nslots; slot++){
		if(objslot(s, slot, got, &oidlen, &oi) != 1)
			continue;
		if(oidlen == 2 && memcmp(got, "c1", 2) == 0){
			seen++;
			eqv("the cursor reports the corrupt flag", oi.corrupt,
				1);
		}
	}
	eqv("c1 is still there", seen, 1);
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * Decision (6): /lost is every copy that fails local verification —
 * §8's corrupt-flagged entries and §5 step 10's condemned slots alike
 * (layer-a §7.5) — and it is maintained by the set, the clear and the
 * condemnation rather than built once.
 */
static void
tlost(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Objinfo oi1, oi2, oi;
	Super sup;
	uchar *buf, o1[Oidmax], o2[Oidmax], o3[Oidmax];
	char err[ERRMAX];
	uchar junk[8];
	ulong slot;

	d = newdisk();
	if((s = mustopen(d, "/lost")) == nil)
		return;
	buf = mkbuf(3*Blk, 97);
	mk(s, "l1");
	mustwr(s, "l1", buf, Blk, 0, 2);
	mk(s, "l2");
	mustwr(s, "l2", buf, Blk, 0, 2);
	mk(s, "l3");
	mustwr(s, "l3", buf, 3*Blk, 0, 2);
	oidof(o1, "l1");
	oidof(o2, "l2");
	oidof(o3, "l3");
	if(ostat(s, "l1", &oi1) < 0 || ostat(s, "l2", &oi2) < 0)
		fail("objstat: %r");
	storestat(s, &st);
	eqv("a fresh store lists nothing", st.nlost, 0);

	if(objcorrupt(s, o1, 2, 1, nil, 0) < 0)
		fail("objcorrupt l1: %r");
	storestat(s, &st);
	eqv("setting the flag lists the object", st.nlost, 1);
	eqv("and names its slot", storelost(s, 0), oi1.slot);
	if(objcorrupt(s, o2, 2, 1, nil, 0) < 0)
		fail("objcorrupt l2: %r");
	storestat(s, &st);
	eqv("a second one is listed beside it", st.nlost, 2);
	/* setting a flag that is already set does not list it twice */
	if(objcorrupt(s, o2, 2, 1, nil, 0) < 0)
		fail("objcorrupt l2 again: %r");
	storestat(s, &st);
	eqv("and a repeat does not double-list it", st.nlost, 2);

	if(objcorrupt(s, o1, 2, 0, nil, 0) < 0)
		fail("objcorrupt l1 clear: %r");
	storestat(s, &st);
	eqv("clearing the flag delists it", st.nlost, 1);
	eqv("leaving the other", storelost(s, 0), oi2.slot);

	/* a delete of the remaining one takes it out through its tombstone */
	if(objremove(s, o2, 2, 3, 1, nil, 0) < 0)
		fail("objremove l2: %r");
	storestat(s, &st);
	eqv("a delete delists it too", st.nlost, 0);

	/*
	 * §5 step 10's condemnation is the other way in.  Damage the
	 * extent-map entry without repairing its csum128, so the entry
	 * itself fails and the slot is condemned on the first read.
	 */
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	geom(d, &sup);
	if(ostat(s, "l3", &oi) < 0)
		fail("objstat l3: %r");
	slot = oi.slot;
	memset(junk, 0xa5, sizeof junk);
	simpoke(d, emapentoff(&sup, oi.emapslot) + sup.emapsz - sizeof junk,
		junk, sizeof junk);
	storeclose(s);
	if((s = mustopen(d, "/lost, condemned")) == nil){
		devclose(d);
		free(buf);
		return;
	}
	storestat(s, &st);
	eqv("start condemns nothing: replay never read the entry", st.nlost,
		0);
	checks++;
	if(objread(s, o3, 2, buf, Blk, 0) >= 0)
		fail("a damaged extent map was served");
	else{
		rerrstr(err, sizeof err);
		istrue("a damaged map is refused as a checksum mismatch",
			strncmp(err, "checksum mismatch", 17) == 0);
	}
	storestat(s, &st);
	eqv("the condemned slot is listed", st.nlost, 1);
	eqv("and named", storelost(s, 0), slot);
	if(objstat(s, o3, 2, &oi) < 0)
		fail("objstat l3: %r");
	else
		eqv("and answers corrupt rather than absent", oi.corrupt, 1);
	storeclose(s);
	devclose(d);
	free(buf);
}

void
main(int argc, char **argv)
{
	USED(argc); USED(argv);
	taccess();
	tscrub();
	trepair();
	tcondemned();
	tlowerkey();
	tcursor();
	tlost();
	killspawned();
	if(fails > 0){
		print("scrubtest: %d of %d checks FAILED\n", fails, checks);
		exits("failed");
	}
	print("scrubtest: %d checks ok\n", checks);
	exits(nil);
}
