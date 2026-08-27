#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"

/*
 * T1: the two-slot superblock rule of docs/design/store.md §2.2,
 * driven against the simulated disk's torn writes (§13, T1.9).
 *
 * The rule is three clauses and the order matters:
 *
 *	1. exactly one copy valid -> the update writes the invalid one;
 *	2. both valid -> it writes the one with the lower gen;
 *	3. neither valid -> the store MUST NOT write and MUST NOT serve.
 *
 * Clause 1 is the whole point of keeping two copies, and the case
 * that discriminates it is a copy torn at a *high* gen: choosing the
 * victim by gen alone then steers the next write onto the only good
 * copy, and a second fault leaves no valid superblock and an instance
 * whose data must be discarded and refilled from peers.
 */

enum
{
	Secsz	= 512,
	Nsec	= 16384,		/* an 8 MiB image */
	Seed	= 0x51ab,
};

static int fails;
static int checks;

static void
fail(char *fmt, ...)
{
	char buf[512];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf + sizeof buf, fmt, arg);
	va_end(arg);
	fprint(2, "FAIL: %s\n", buf);
	fails++;
}

static void
eqv(char *what, uvlong got, uvlong want)
{
	checks++;
	if(got != want)
		fail("%s: %llud, want %llud", what, got, want);
}

static void
eqi(char *what, int got, int want)
{
	checks++;
	if(got != want)
		fail("%s: %d, want %d", what, got, want);
}

/* the §13 T1 geometry: a few MiB, nslots and nemap in the hundreds */
static void
smallcfg(Fmtcfg *c)
{
	memset(c, 0, sizeof *c);
	c->secsz = Secsz;
	c->blksz = 4096;
	c->objmax = 65536;
	c->nslots = 512;
	c->nemap = 256;
	c->ndirty = 256;
	c->logbytes = 256*1024;
	c->csumalg = Csumblake2s;
}

static Dev*
fresh(Super *s)
{
	Dev *d;
	Fmtcfg c;

	if((d = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	smallcfg(&c);
	if(geometry(s, &c, d->size) < 0)
		sysfatal("geometry: %r");
	if(fmtstore(d, s) < 0)
		sysfatal("fmtstore: %r");
	return d;
}

/* write one superblock copy at gen g, as §2.2's publish would */
static void
publish(Dev *d, Super *s, int copy, uvlong g, int tear)
{
	uchar *p;

	if((p = mallocz(s->secsz, 1)) == nil)
		sysfatal("malloc: %r");
	s->gen = g;
	superpack(p, s);
	if(tear)
		simfault(d, Sftearbyte, 1);
	if(devwrite(d, p, s->secsz, copy == 0 ? 0 : super1off(d)) < 0)
		fail("publish copy %d: %r", copy);
	if(devflush(d) < 0)
		fail("flush: %r");
	free(p);
}

/* a fresh format leaves copy 0 at gen 0 and copy 1 at gen 1 (§2.2) */
static void
tfresh(void)
{
	Dev *d;
	Super s;
	Sbsel sel;

	d = fresh(&s);
	eqi("fresh: superselect", superselect(d, &sel), 0);
	eqi("fresh: copy 0 valid", sel.valid[0], 1);
	eqi("fresh: copy 1 valid", sel.valid[1], 1);
	eqv("fresh: copy 0 gen", sel.sb[0].gen, 0);
	eqv("fresh: copy 1 gen", sel.sb[1].gen, 1);
	eqi("fresh: clause 2 decides", sel.clause, 2);
	eqi("fresh: start from copy 1", sel.start, 1);
	eqi("fresh: the update writes copy 0", sel.victim, 0);
	eqv("fresh: next gen", sel.nextgen, 2);
	devclose(d);
}

/*
 * Clause 1, and the case that discriminates it: copy 1 is torn while
 * carrying the higher gen.  Choosing by gen alone writes copy 0 - the
 * only valid copy - and a second fault then leaves nothing.
 */
static void
ttornhigh(void)
{
	Dev *d;
	Super s;
	Sbsel sel;
	uchar *p;

	d = fresh(&s);
	/* copy 0 at gen 5, valid */
	publish(d, &s, 0, 5, 0);
	/* copy 1 at gen 6, its checksum broken but its gen intact */
	publish(d, &s, 1, 6, 0);
	if((p = mallocz(s.secsz, 1)) == nil)
		sysfatal("malloc: %r");
	simpeek(d, super1off(d), p, s.secsz);
	p[100] ^= 0x40;			/* the pad word: not gen, not csum */
	simpoke(d, super1off(d), p, s.secsz);

	eqi("torn-high: superselect", superselect(d, &sel), 0);
	eqi("torn-high: copy 0 valid", sel.valid[0], 1);
	eqi("torn-high: copy 1 valid", sel.valid[1], 0);
	eqv("torn-high: the torn copy still reads gen 6",
		GBIT64(p + 32), 6);
	eqi("torn-high: clause 1 decides", sel.clause, 1);
	eqi("torn-high: start from the valid copy 0", sel.start, 0);
	eqi("torn-high: the update writes the invalid copy 1", sel.victim, 1);
	eqv("torn-high: next gen", sel.nextgen, 6);

	/* the update lands on copy 1, and the store is whole again */
	publish(d, &s, sel.victim, sel.nextgen, 0);
	eqi("torn-high: superselect after the repair",
		superselect(d, &sel), 0);
	eqi("torn-high: both copies valid again",
		sel.valid[0] && sel.valid[1], 1);
	eqi("torn-high: start from copy 1", sel.start, 1);
	eqv("torn-high: copy 1 gen", sel.sb[1].gen, 6);
	free(p);
	devclose(d);
}

/* the same rule through a real torn write of the copy being published */
static void
ttornwrite(void)
{
	Dev *d;
	Super s;
	Sbsel sel;

	d = fresh(&s);
	publish(d, &s, 0, 4, 0);		/* copy 0 valid at gen 4 */
	publish(d, &s, 1, 5, 1);		/* copy 1 torn at gen 5 */

	eqi("torn write: superselect", superselect(d, &sel), 0);
	eqi("torn write: copy 0 valid", sel.valid[0], 1);
	eqi("torn write: the torn copy is invalid", sel.valid[1], 0);
	eqi("torn write: clause 1 decides", sel.clause, 1);
	eqi("torn write: start from copy 0", sel.start, 0);
	eqi("torn write: the update writes copy 1", sel.victim, 1);
	eqv("torn write: copy 0 still reads gen 4", sel.sb[0].gen, 4);
	devclose(d);
}

/* clause 3: neither copy valid, and the store must refuse */
static void
tneither(void)
{
	Dev *d;
	Super s;
	Sbsel sel;
	Ckcfg ck;
	uchar *p;
	int null;

	d = fresh(&s);
	if((p = mallocz(s.secsz, 1)) == nil)
		sysfatal("malloc: %r");
	simpeek(d, 0, p, s.secsz);
	p[100] ^= 0x40;
	simpoke(d, 0, p, s.secsz);
	simpeek(d, super1off(d), p, s.secsz);
	p[100] ^= 0x40;
	simpoke(d, super1off(d), p, s.secsz);

	eqi("neither: superselect refuses", superselect(d, &sel), -1);
	eqi("neither: clause 3 decides", sel.clause, 3);
	eqi("neither: no copy to start from", sel.start, -1);
	eqi("neither: no copy to write", sel.victim, -1);

	/* and the checker says so rather than reporting a clean store */
	memset(&ck, 0, sizeof ck);
	if((null = open("/dev/null", OWRITE)) < 0)
		null = 2;
	ck.out = null;
	ck.quiet = 1;
	checks++;
	if(ckstore(d, &ck) == 0)
		fail("neither: the checker passed a store with no valid "
			"superblock");
	if(null != 2)
		close(null);
	free(p);
	devclose(d);
}

/*
 * A crash between a superblock write and its flush leaves the other
 * copy: §13's `super' point, which shoalfmt is the one thing that
 * drives today.  Armed at copy 1, the format's second write never
 * becomes durable and the store comes up on copy 0 under clause 1.
 */
static void
tsuperpoint(void)
{
	Dev *d;
	Super s;
	Fmtcfg c;
	Sbsel sel;

	if((d = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	smallcfg(&c);
	if(geometry(&s, &c, d->size) < 0)
		sysfatal("geometry: %r");
	simarm(d, "super", 1);
	fmtstore(d, &s);

	eqi("super point: superselect", superselect(d, &sel), 0);
	eqi("super point: copy 0 survived", sel.valid[0], 1);
	eqi("super point: copy 1 did not land", sel.valid[1], 0);
	eqi("super point: clause 1 decides", sel.clause, 1);
	eqi("super point: start from copy 0", sel.start, 0);
	eqi("super point: the update writes copy 1", sel.victim, 1);
	eqv("super point: copy 0 gen", sel.sb[0].gen, 0);
	devclose(d);

	/*
	 * Armed at copy 0 instead, the first write is the one that is
	 * lost and copy 1 lands: the same clause, the other way round,
	 * and the store starts on the copy at the *higher* gen.
	 */
	if((d = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	simarm(d, "super", 0);
	fmtstore(d, &s);
	eqi("super point 0: superselect", superselect(d, &sel), 0);
	eqi("super point 0: copy 0 did not land", sel.valid[0], 0);
	eqi("super point 0: copy 1 survived", sel.valid[1], 1);
	eqi("super point 0: clause 1 decides", sel.clause, 1);
	eqi("super point 0: start from copy 1", sel.start, 1);
	eqi("super point 0: the update writes copy 0", sel.victim, 0);
	eqv("super point 0: copy 1 gen", sel.sb[1].gen, 1);
	devclose(d);
}

void
main(int, char**)
{
	tfresh();
	ttornhigh();
	ttornwrite();
	tneither();
	tsuperpoint();
	if(fails > 0)
		exits("failed");
	print("supertest: %d checks ok\n", checks);
	exits(nil);
}
