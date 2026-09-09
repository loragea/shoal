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

/* the current geometry, for reaching past the API at the media */
static void
geom(Dev *d, Super *sup)
{
	Sbsel sel;

	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	*sup = sel.sb[sel.start];
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
	checks++;
	if(objdiscard(s, oid, 1, 3, 1, 2) < 0)
		fail("objdiscard of the tombstone: %r");

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
	tlost();
	killspawned();
	if(fails > 0){
		print("scrubtest: %d of %d checks FAILED\n", fails, checks);
		exits("failed");
	}
	print("scrubtest: %d checks ok\n", checks);
	exits(nil);
}
