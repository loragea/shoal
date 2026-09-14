#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"

/*
 * T1: the monitor's map slot store, docs/design/store.md §10, and the
 * shoalmonfmt decisions of §12, against the simulated disk and a
 * file-backed image.
 *
 * This is T1.9's second half — §2.2's two-slot rule as the
 * current-map slots use it, keyed by seq — and T2.7's phantom case at
 * T1 scale: a crash between §10's two steps leaves a ring entry for a
 * map that was never published, and the seq stamp is what makes that
 * decidable without a durable ring cursor.  T2.7 itself, on a real
 * device under -X, is still open.
 *
 * Every crash schedule runs with the device stopped at the crash
 * (simcrashdead).  The named cases drop the dirty sectors, which is
 * the crash a device that lost its whole cache performs; tcrashmatrix
 * sweeps each of the three commit points against all four of the
 * sim's crash policies.
 */

enum
{
	Secsz	= 512,
	Nsec	= 4096,			/* a 2 MiB image: §10's floor is 1 */
	Bigsec	= 16384,		/* 8 MiB, for the object-store case */
	Seed	= 0x9a1b,
	Slotsz	= 2048,			/* four sectors */
	Retain	= 4,
	Maxmap	= Slotsz - Secsz,	/* the largest map that fits a slot */
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

/* the same two, for a case that runs under several names */
static void
eqv2(char *pre, char *what, uvlong got, uvlong want)
{
	checks++;
	if(got != want)
		fail("%s: %s: %llud, want %llud", pre, what, got, want);
}

static void
eqi2(char *pre, char *what, int got, int want)
{
	checks++;
	if(got != want)
		fail("%s: %s: %d, want %d", pre, what, got, want);
}

static void
istrue(char *what, int ok)
{
	checks++;
	if(!ok)
		fail("%s", what);
}

static void
moncfg(Monfmtcfg *c)
{
	memset(c, 0, sizeof *c);
	c->slotsz = Slotsz;
	c->retain = Retain;
}

static Dev*
fresh(void)
{
	Dev *d;
	Monfmtcfg c;

	if((d = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	moncfg(&c);
	if(monfmt(d, &c) < 0)
		sysfatal("monfmt: %r");
	return d;
}

static Mon*
mustopen(Dev *d, char *what)
{
	Mon *m;

	checks++;
	if((m = monopen(d)) == nil){
		fail("%s: monopen: %r", what);
		return nil;
	}
	return m;
}

/* a map text of n bytes whose every byte depends on the seed */
static uchar*
mktext(long n, int seed)
{
	uchar *p;
	long i;

	if((p = malloc(n)) == nil)
		sysfatal("malloc: %r");
	for(i = 0; i < n; i++)
		p[i] = (uchar)(seed*7 + i*31 + (i>>8)*13);
	return p;
}

static int
commit(Mon *m, char *what, char *text, uvlong epoch)
{
	checks++;
	if(moncommit(m, text, strlen(text), epoch) < 0){
		fail("%s: moncommit: %r", what);
		return -1;
	}
	return 0;
}

static void
eqtext(char *what, Monmap *mm, char *want)
{
	checks++;
	if(mm->len != strlen(want) || memcmp(mm->text, want, mm->len) != 0)
		fail("%s: the map text did not come back", what);
}

static vlong
histoffs(Monstat *st, int i)
{
	return (vlong)st->histoff*Secsz + (vlong)i*st->slotsz;
}

static vlong
curoffs(Monstat *st, int i)
{
	return (vlong)st->curoff*Secsz + (vlong)i*st->slotsz;
}

/*
 * A shim device over the simulated disk that fails a run of reads by
 * ORDINAL.  The sim aims a fault at a byte range, and a slot's write
 * and its read-back cover the same range, so a range fault cannot
 * pick out the read-back — which is the one §10 distinguishes from
 * the write before it.  Counting is what can: a commit issues its
 * reads only as read-backs, two per slot (the header sector, then the
 * text), so reads 1 and 2 are the ring slot's and 3 and 4 the current
 * slot's.  Everything else passes through, so the sim's faults,
 * crashes, trace and simpeek all still work underneath.
 */
typedef struct Shim Shim;
struct Shim
{
	Dev	*s;		/* the sim underneath */
	int	nrd;		/* reads since shimarm */
	int	rd0, rdn;	/* fail rdn reads from ordinal rd0 */
};

static Shim shim;

static long
shimrd(Dev *d, void *a, long n, vlong off)
{
	Shim *sh;

	sh = d->aux;
	sh->nrd++;
	if(sh->rdn > 0 && sh->nrd >= sh->rd0 && sh->nrd < sh->rd0 + sh->rdn){
		werrstr("i/o error");
		return -1;
	}
	return (*sh->s->ops->read)(sh->s, a, n, off);
}

static long
shimwr(Dev *d, void *a, long n, vlong off)
{
	Shim *sh;

	sh = d->aux;
	return (*sh->s->ops->write)(sh->s, a, n, off);
}

static int
shimfl(Dev *d)
{
	Shim *sh;

	sh = d->aux;
	return (*sh->s->ops->flush)(sh->s);
}

static void
shimpoint(Dev *d, char *name, int n)
{
	Shim *sh;

	sh = d->aux;
	(*sh->s->ops->point)(sh->s, name, n);
}

static void
shimclose(Dev*)
{
}

static Devops shimops = { shimrd, shimwr, shimfl, shimpoint, shimclose };

static Dev*
shimopen(Dev *s)
{
	Dev *d;

	if((d = mallocz(sizeof *d, 1)) == nil)
		sysfatal("malloc: %r");
	memset(&shim, 0, sizeof shim);
	shim.s = s;
	d->ops = &shimops;
	d->name = strdup("shim");
	d->secsz = s->secsz;
	d->size = s->size;
	d->wunit = s->wunit;
	d->flushmode = s->flushmode;
	d->aux = &shim;
	return d;
}

/* fail n reads from the next commit's read number first */
static void
shimarm(int first, int n)
{
	shim.nrd = 0;
	shim.rd0 = first;
	shim.rdn = n;
}

/*
 * Every valid ring entry carries a seq of its own — one seq space for
 * the whole store — so a publish that landed and was then retried
 * must not leave two entries claiming one number.  Read off the
 * platter, not out of memory.
 */
static void
noseqdups(Dev *sim, Monstat *st, char *what)
{
	uchar hdr[Secsz];
	uvlong seq[16];
	int i, j, n;

	n = 0;
	for(i = 0; i < (int)st->retain && n < nelem(seq); i++){
		simpeek(sim, histoffs(st, i), hdr, Secsz);
		if(memcmp(hdr, "shoalmap", 8) == 0)
			seq[n++] = GBIT64(hdr + 32);
	}
	for(i = 0; i < n; i++)
		for(j = i + 1; j < n; j++){
			checks++;
			if(seq[i] == seq[j])
				fail("%s: ring slots %d and %d both carry "
					"seq %llud", what, i, j, seq[i]);
		}
}

/*
 * Break one slot's checksum without touching its seq, which is what a
 * torn write leaves and what §2.2's clause 1 is about: a slot torn at
 * a HIGH seq must not steer the next write onto the only good one.
 * The byte flipped is in the slot header's reserved run, past epoch
 * and before the map text, so neither seq nor len moves.
 */
static void
tearslot(Dev *d, vlong off)
{
	uchar buf[Secsz];

	simpeek(d, off, buf, Secsz);
	buf[100] ^= 0x40;
	simpoke(d, off, buf, Secsz);
}

/* a fresh format, and what §10 says a fresh store holds */
static void
tfresh(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Monmap mm;
	uchar hdr[Secsz];

	d = fresh();
	if((m = mustopen(d, "fresh")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("fresh: slotsz", st.slotsz, Slotsz);
	eqv("fresh: retain", st.retain, Retain);
	eqv("fresh: current slots at sector 1", st.curoff, 1);
	eqv("fresh: history slots follow the two current ones", st.histoff,
		1 + 2*(Slotsz/Secsz));
	eqi("fresh: both header copies are valid", st.hdrother, 1);
	eqi("fresh: the open took copy 0", st.hdr, 0);
	eqv("fresh: no history entries", st.nhist, 0);
	eqv("fresh: no phantoms", st.nphantom, 0);
	eqv("fresh: seq 0", st.seq, 0);

	/* §10: a fresh store holds no map, and that is len 0 */
	eqi("fresh: the store holds no map", st.hasmap, 0);
	eqi("fresh: moncurrent answers no map", moncurrent(m, &mm), 0);
	eqi("fresh: the ring is empty", monhistory(m, 0, &mm), 0);
	eqi("fresh: no epoch is published", monlookup(m, 0, &mm), 0);

	/* the header's byte layout, §10 */
	simpeek(d, 0, hdr, Secsz);
	istrue("fresh: header magic", memcmp(hdr, "shoalmon", 8) == 0);
	eqv("fresh: header vers", GBIT32(hdr + 8), Monvers);
	eqv("fresh: header slotsz at offset 32", GBIT32(hdr + 32), Slotsz);
	eqv("fresh: header retain at offset 36", GBIT32(hdr + 36), Retain);
	eqv("fresh: header curoff at offset 40", GBIT64(hdr + 40), 1);
	eqv("fresh: header histoff at offset 48", GBIT64(hdr + 48),
		1 + 2*(Slotsz/Secsz));
	simpeek(d, (vlong)st.curoff*Secsz, hdr, Secsz);
	istrue("fresh: current slot 0 is a valid empty map",
		memcmp(hdr, "shoalmap", 8) == 0);
	eqv("fresh: current slot 0 len", GBIT32(hdr + 12), 0);
	simpeek(d, histoffs(&st, 0), hdr, Secsz);
	eqv("fresh: history slot 0 is zeroed", GBIT64(hdr), 0);
	monclose(m);

	/* both copies of the header are identical (§10: written once) */
	{
		uchar a[Secsz], b[Secsz];

		simpeek(d, 0, a, Secsz);
		simpeek(d, d->size - Secsz, b, Secsz);
		istrue("fresh: the two header copies are identical",
			memcmp(a, b, Secsz) == 0);
	}
	devclose(d);
}

/*
 * §10's format prologue: both header sectors are zeroed and flushed
 * BEFORE anything else is written, and the slots are flushed before
 * the real header copies are written over them.  A format cut short
 * anywhere in between must leave no valid header rather than a valid
 * one locating slots that were never written.
 */
static void
tfmtprologue(void)
{
	Dev *d;
	Mon *m;
	Monfmtcfg c;
	Simop *t, w[64];
	vlong off1;
	long n, j;
	int nw;

	if((d = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	off1 = d->size - Secsz;
	moncfg(&c);
	simtracereset(d);
	checks++;
	if(monfmt(d, &c) < 0){
		fail("prologue: monfmt: %r");
		devclose(d);
		return;
	}
	n = simtrace(d, &t);
	nw = 0;
	for(j = 0; j < n; j++)
		if(t[j].op != Sopread && nw < nelem(w))
			w[nw++] = t[j];
	/*
	 * 2 header sectors + a flush, retain history headers, 2 current
	 * slots + a flush, 2 header copies + a flush.
	 */
	eqi("prologue: writes and flushes in a format", nw, Retain + 9);
	if(nw == Retain + 9){
		eqi("prologue: header copy 0 is zeroed first", w[0].op,
			Sopwrite);
		eqv("prologue: at sector 0", w[0].off, 0);
		eqv("prologue: one sector of it", w[0].n, Secsz);
		eqi("prologue: then header copy 1", w[1].op, Sopwrite);
		eqv("prologue: at the last sector", w[1].off, off1);
		eqv("prologue: one sector of it", w[1].n, Secsz);
		eqi("prologue: and the pair is flushed before anything else",
			w[2].op, Sopflush);
		/* and the slots are flushed before the real headers land */
		eqi("prologue: the slots are flushed", w[nw-4].op, Sopflush);
		eqi("prologue: then header copy 0", w[nw-3].op, Sopwrite);
		eqv("prologue: at sector 0", w[nw-3].off, 0);
		eqi("prologue: then header copy 1", w[nw-2].op, Sopwrite);
		eqv("prologue: at the last sector", w[nw-2].off, off1);
		eqi("prologue: and the format ends with a flush", w[nw-1].op,
			Sopflush);
	}
	devclose(d);

	/*
	 * A reformat at a different geometry that dies at monfmthdr —
	 * right after the zeroing flush — leaves the durable state the
	 * prologue exists to leave: no valid header at all.  Without the
	 * prologue the PREVIOUS store's header survives and locates
	 * slots this format never wrote.
	 */
	d = fresh();
	if((m = mustopen(d, "prologue store")) == nil){
		devclose(d);
		return;
	}
	commit(m, "prologue", "map=A", 1);
	monclose(m);

	simcrashdead(d, 1);
	simarm(d, "monfmthdr", 0);
	moncfg(&c);
	c.slotsz = 4*Slotsz;
	c.retain = Retain;
	c.ream = 1;
	checks++;
	if(monfmt(d, &c) == 0)
		fail("prologue: a format whose machine died reported success");
	simrevive(d);
	checks++;
	if((m = monopen(d)) != nil){
		fail("prologue: a format cut short left a valid header");
		monclose(m);
	}
	devclose(d);
}

/* the first commit, a restart, and the ring newest-first */
static void
tcommit(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Monmap mm;

	d = fresh();
	if((m = mustopen(d, "commit")) == nil){
		devclose(d);
		return;
	}
	commit(m, "commit", "map=c epoch=1", 1);
	monstat(m, &st);
	eqv("commit: seq 1", st.seq, 1);
	eqv("commit: epoch 1", st.epoch, 1);
	eqi("commit: there is a map now", st.hasmap, 1);
	eqv("commit: one history entry", st.nhist, 1);
	monclose(m);

	/* restart: the same map, at seq 1 */
	if((m = mustopen(d, "commit restart")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("restart: seq 1", st.seq, 1);
	eqv("restart: epoch 1", st.epoch, 1);
	eqi("restart: moncurrent answers the map", moncurrent(m, &mm), 1);
	eqtext("restart: the map text", &mm, "map=c epoch=1");
	eqv("restart: the current map's seq", mm.seq, 1);

	/* §10 step 1 writes the ring entry for every published map */
	eqi("restart: the ring holds the current map at position 0",
		monhistory(m, 0, &mm), 1);
	eqv("restart: position 0 is the current map", mm.seq, 1);
	eqi("restart: nothing behind it", monhistory(m, 1, &mm), 0);

	commit(m, "commit 2", "map=c epoch=2", 2);
	commit(m, "commit 3", "map=c epoch=3", 3);
	monclose(m);
	if((m = mustopen(d, "commit restart 2")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("restart 2: the newest seq is current", st.seq, 3);
	eqv("restart 2: the newest epoch is current", st.epoch, 3);
	eqv("restart 2: three history entries", st.nhist, 3);
	eqi("restart 2: position 0", monhistory(m, 0, &mm), 1);
	eqv("restart 2: position 0 is the current map", mm.epoch, 3);
	/* layer-a §5.2 clause 2 reads E-1, so it must be here */
	eqi("restart 2: position 1", monhistory(m, 1, &mm), 1);
	eqv("restart 2: position 1 is E-1", mm.epoch, 2);
	eqi("restart 2: position 2", monhistory(m, 2, &mm), 1);
	eqv("restart 2: position 2", mm.epoch, 1);
	eqi("restart 2: nothing behind it", monhistory(m, 3, &mm), 0);
	eqi("restart 2: by epoch", monlookup(m, 2, &mm), 1);
	eqv("restart 2: by epoch answers E-1's seq", mm.seq, 2);
	eqi("restart 2: an epoch never published", monlookup(m, 9, &mm), 0);
	monclose(m);
	devclose(d);
}

/*
 * T1.13's shape for §10: from the recorded trace, one flush after the
 * history write and another after the current write, in that order,
 * and nothing else WRITTEN in a commit.  The reads between them are
 * §10's read-back, which is checked by its own case below; here only
 * their placement matters, so the writes and flushes are picked out
 * of the trace in order.
 */
static void
tflushes(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Simop *t, w[8];
	long n, j;
	int i, nw;

	d = fresh();
	if((m = mustopen(d, "flushes")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	for(i = 1; i <= 3; i++){
		simtracereset(d);
		commit(m, "flushes", "map=c", i);
		n = simtrace(d, &t);
		nw = 0;
		for(j = 0; j < n; j++)
			if(t[j].op != Sopread && nw < nelem(w))
				w[nw++] = t[j];
		checks++;
		if(nw != 4){
			fail("flushes: commit %d wrote and flushed %d times, "
				"want 4", i, nw);
			continue;
		}
		eqi("flushes: the history slot is written first", w[0].op,
			Sopwrite);
		istrue("flushes: written into the ring",
			w[0].off >= histoffs(&st, 0)
			&& w[0].off < histoffs(&st, Retain));
		eqi("flushes: then a flush", w[1].op, Sopflush);
		eqi("flushes: then the current slot", w[2].op, Sopwrite);
		istrue("flushes: written into a current slot",
			w[2].off >= curoffs(&st, 0)
			&& w[2].off < curoffs(&st, 2));
		eqi("flushes: then a flush", w[3].op, Sopflush);
		/* and the last thing a commit does is read its slot back */
		eqi("flushes: the commit ends with a read", t[n-1].op, Sopread);
	}
	monclose(m);
	devclose(d);
}

/*
 * §10's read-back.  A device that takes a write, acknowledges the
 * flush and lands nothing (Sfdrop), or lands a mix of old and new
 * bytes (Sftearbyte), must fail the commit rather than leave the
 * running monitor holding a map the platter does not.
 */
static void
tlostwrite(int kind, char *kindname, int onring)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Monmap mm;
	char what[64];
	vlong off;

	snprint(what, sizeof what, "lost %s on the %s write", kindname,
		onring ? "ring" : "current");
	d = fresh();
	if((m = mustopen(d, what)) == nil){
		devclose(d);
		return;
	}
	commit(m, what, "map=A", 1);
	commit(m, what, "map=B", 2);
	monstat(m, &st);

	if(onring)
		simfault(d, kind, 1);
	else{
		/* the slot §2.2's clause 2 will choose: the lower seq */
		off = curoffs(&st, st.cur == 0 ? 1 : 0);
		simfaultat(d, kind, 1, off, st.slotsz);
	}
	checks++;
	if(moncommit(m, "map=C", 5, 3) == 0)
		fail("%s: a commit whose write was lost reported success",
			what);
	monstat(m, &st);
	eqv2(what, "the live store is still B", st.epoch, 2);
	monclose(m);

	if((m = mustopen(d, what)) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv2(what, "the restart finds B", st.epoch, 2);
	eqv2(what, "at its own seq", st.seq, 2);
	eqi2(what, "moncurrent answers it", moncurrent(m, &mm), 1);
	eqtext("the previous map's text", &mm, "map=B");
	eqi2(what, "position 0 is present", monhistory(m, 0, &mm), 1);
	eqv2(what, "position 0 is the current map", mm.seq, st.seq);
	eqi2(what, "the unpublished epoch answers nothing",
		monlookup(m, 3, &mm), 0);
	monclose(m);
	devclose(d);
}

/*
 * T1.9's second half.  A crash at moncur with the dirty sectors
 * dropped leaves the previous map current and a ring entry for a
 * publish that never happened; a slot torn at a HIGH seq must not
 * steer the next write onto the only good one.
 */
static void
tslots(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Monmap mm;
	Simop *t;
	vlong torn;
	long n;
	int i, victim;

	d = fresh();
	if((m = mustopen(d, "slots")) == nil){
		devclose(d);
		return;
	}
	commit(m, "slots", "map=A", 1);
	commit(m, "slots", "map=B", 2);
	monstat(m, &st);
	eqv("slots: B is current", st.seq, 2);

	/* the publish of C dies after the current write and before its flush */
	simcrashdead(d, 1);
	simarm(d, "moncur", 0);
	checks++;
	if(moncommit(m, "map=C", 5, 3) == 0)
		fail("slots: a commit whose machine died reported success");
	monclose(m);
	simrevive(d);

	if((m = mustopen(d, "slots after the crash")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("slots: the previous map is current", st.seq, 2);
	eqv("slots: at its own epoch", st.epoch, 2);
	eqi("slots: moncurrent", moncurrent(m, &mm), 1);
	eqtext("slots: the previous map's text", &mm, "map=B");
	eqv("slots: the failed publish left a phantom", st.nphantom, 1);
	eqv("slots: and it is not a history entry", st.nhist, 2);
	eqi("slots: the phantom is absent by epoch", monlookup(m, 3, &mm), 0);
	eqi("slots: position 0 is still B", monhistory(m, 0, &mm), 1);
	eqv("slots: position 0's epoch", mm.epoch, 2);

	/* commit D, then tear the slot it landed in, at its high seq */
	commit(m, "slots", "map=D", 4);
	monstat(m, &st);
	eqv("slots: D is current", st.seq, 4);
	torn = curoffs(&st, st.cur);
	monclose(m);
	tearslot(d, torn);

	if((m = mustopen(d, "slots after the tear")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("slots: the torn slot is not taken", st.seq, 2);
	eqi("slots: the store starts from the other slot",
		curoffs(&st, st.cur) != torn, 1);
	eqi("slots: and on a valid map", moncurrent(m, &mm), 1);
	eqtext("slots: which is still B", &mm, "map=B");

	/*
	 * The next commit MUST land on the torn slot.  Choosing by seq
	 * alone would write the only good one, and the next fault would
	 * then leave no valid current slot at all.
	 */
	simtracereset(d);
	commit(m, "slots", "map=E", 5);
	n = simtrace(d, &t);
	victim = -1;
	for(i = 0; i < n; i++)
		if(t[i].op == Sopwrite && t[i].off == torn)
			victim = i;
	checks++;
	if(victim < 0)
		fail("slots: the commit after a tear did not write the "
			"torn slot");
	monclose(m);

	/* and now tear the other one: the store must still start */
	if((m = mustopen(d, "slots after E")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("slots: E is current", st.seq, 5);
	torn = curoffs(&st, st.cur);
	monclose(m);
	tearslot(d, torn);
	if((m = mustopen(d, "slots after the second tear")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqi("slots: the store still starts at a valid map",
		moncurrent(m, &mm), 1);
	eqtext("slots: on the surviving map", &mm, "map=B");
	monclose(m);
	devclose(d);
}

/*
 * The whole crash argument, swept: each of §10's three commit points
 * against each of the simulated disk's four crash policies.  The
 * cases above take the Scdrop column, which is the crash a device
 * that lost its whole cache performs; Sckeep, Scsome and Scnamed are
 * the other things a cache can do with the sectors written since the
 * last flush, and §10 claims all three invariants against every one
 * of them.
 *
 * Whatever the schedule, after the restart: the current map is
 * EXACTLY the old one or EXACTLY the new one and nothing in between;
 * position 0 of the ring is the current map; and no ring entry sits
 * above the current map's seq, since any that did is a phantom.
 */
static void
tcrashcell(char *point, int mode, char *modename)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Monmap mm, h;
	char what[64];
	ulong i;
	int old, new;

	snprint(what, sizeof what, "crash %s %s", point, modename);
	d = fresh();
	if((m = mustopen(d, what)) == nil){
		devclose(d);
		return;
	}
	commit(m, what, "map=A", 1);
	commit(m, what, "map=B", 2);
	monstat(m, &st);

	simcrashdead(d, 1);
	if(mode == Scnamed){
		/* keep only the first sector of the slot being written */
		if(strcmp(point, "moncur") == 0)
			simcrashkeep(d, curoffs(&st, st.cur == 0 ? 1 : 0),
				Secsz);
		else
			simcrashkeep(d, histoffs(&st, 2), Secsz);
	}else
		simcrashmode(d, mode);
	simarm(d, point, 0);
	moncommit(m, "map=C", 5, 3);
	monclose(m);
	simrevive(d);

	if((m = mustopen(d, what)) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	old = new = 0;
	if(moncurrent(m, &mm)){
		old = mm.len == 5 && memcmp(mm.text, "map=B", 5) == 0
			&& st.epoch == 2 && st.seq == 2;
		new = mm.len == 5 && memcmp(mm.text, "map=C", 5) == 0
			&& st.epoch == 3 && st.seq == 3;
	}
	eqi2(what, "the current map is exactly the old or the new",
		old || new, 1);
	eqi2(what, "position 0 is the current map",
		monhistory(m, 0, &h) && h.seq == st.seq
		&& h.epoch == st.epoch, 1);
	for(i = 0; ; i++){
		if(!monhistory(m, i, &h))
			break;
		if(h.seq > st.seq){
			fail("%s: a ring entry sits above the current map: "
				"seq %llud over %llud", what, h.seq, st.seq);
			break;
		}
	}
	checks++;
	monclose(m);
	devclose(d);
}

static void
tcrashmatrix(void)
{
	static char *points[] = { "monhist", "monhistflush", "moncur" };
	static int modes[] = { Scdrop, Sckeep, Scsome, Scnamed };
	static char *modenames[] = { "Scdrop", "Sckeep", "Scsome", "Scnamed" };
	int i, j;

	for(i = 0; i < nelem(points); i++)
		for(j = 0; j < nelem(modes); j++)
			tcrashcell(points[i], modes[j], modenames[j]);
}

/*
 * T2.7's phantom case at T1 scale.  A crash at monhistflush is the
 * phantom window: the ring entry is durable and the map was never
 * published.  The next commit reuses that slot.
 */
static void
tphantom(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Monmap mm;
	Simop *t;
	vlong slot;
	long n;
	int i, reused;

	d = fresh();
	if((m = mustopen(d, "phantom")) == nil){
		devclose(d);
		return;
	}
	commit(m, "phantom", "map=A", 1);
	commit(m, "phantom", "map=B", 2);

	simcrashdead(d, 1);
	simarm(d, "monhistflush", 0);
	checks++;
	if(moncommit(m, "map=C", 5, 3) == 0)
		fail("phantom: a commit whose machine died reported success");
	monclose(m);
	simrevive(d);

	if((m = mustopen(d, "phantom restart")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("phantom: the current map is still B", st.epoch, 2);
	eqv("phantom: one phantom", st.nphantom, 1);
	eqi("phantom: the unpublished epoch answers nothing",
		monlookup(m, 3, &mm), 0);
	eqi("phantom: it is not in the ring either",
		monhistory(m, 0, &mm) && mm.epoch == 3, 0);
	eqv("phantom: the ring holds only what was published", st.nhist, 2);

	/* find the phantom's slot, then watch the next commit reuse it */
	slot = -1;
	for(i = 0; i < Retain; i++){
		uchar hdr[Secsz];

		simpeek(d, histoffs(&st, i), hdr, Secsz);
		if(memcmp(hdr, "shoalmap", 8) == 0 && GBIT64(hdr + 32) == 3)
			slot = histoffs(&st, i);
	}
	checks++;
	if(slot < 0)
		fail("phantom: the phantom's ring slot is not on the disk");
	simtracereset(d);
	commit(m, "phantom", "map=D", 4);
	n = simtrace(d, &t);
	reused = 0;
	for(i = 0; i < n; i++)
		if(t[i].op == Sopwrite && t[i].off == slot)
			reused = 1;
	istrue("phantom: the next commit reuses the phantom's slot", reused);
	monstat(m, &st);
	eqv("phantom: and no phantom is left", st.nphantom, 0);
	eqv("phantom: the ring holds three published maps", st.nhist, 3);
	monclose(m);

	if((m = mustopen(d, "phantom restart 2")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("phantom: no phantom after the restart", st.nphantom, 0);
	eqi("phantom: the unpublished epoch is still absent",
		monlookup(m, 3, &mm), 0);
	eqi("phantom: D is published", monlookup(m, 4, &mm), 1);
	monclose(m);
	devclose(d);
}

/*
 * A history write that cannot be made durable fails the commit and
 * leaves the current map untouched (§10 step 1).  Two shapes: the
 * device refuses the write, and a crash at monhist, whose dirty
 * sector reverts.
 */
static void
thistfail(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Monmap mm;

	d = fresh();
	if((m = mustopen(d, "hist fail")) == nil){
		devclose(d);
		return;
	}
	commit(m, "hist fail", "map=A", 1);
	commit(m, "hist fail", "map=B", 2);

	simfault(d, Sfeio, 1);
	checks++;
	if(moncommit(m, "map=C", 5, 3) == 0)
		fail("hist fail: a commit whose ring write failed reported "
			"success");
	monstat(m, &st);
	eqv("hist fail: the current map is untouched", st.epoch, 2);
	monclose(m);
	if((m = mustopen(d, "hist fail restart")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("hist fail: the restart finds the same current map", st.epoch, 2);
	eqv("hist fail: at the same seq", st.seq, 2);
	eqi("hist fail: and no entry for the failed epoch",
		monlookup(m, 3, &mm), 0);

	/* the same through a crash before the ring write's flush */
	simcrashdead(d, 1);
	simarm(d, "monhist", 0);
	checks++;
	if(moncommit(m, "map=C", 5, 3) == 0)
		fail("hist fail: a commit whose machine died at monhist "
			"reported success");
	monclose(m);
	simrevive(d);
	if((m = mustopen(d, "monhist restart")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("monhist: the current map is unchanged", st.epoch, 2);
	eqv("monhist: at the same seq", st.seq, 2);
	eqi("monhist: no entry for the failed epoch", monlookup(m, 3, &mm), 0);
	eqv("monhist: no phantom either", st.nphantom, 0);
	monclose(m);
	devclose(d);
}

/*
 * A ring write that fails over a PHANTOM victim.  The write landed
 * nothing, so the phantom is still on the platter; if the store
 * forgets that, the victim search walks past it, the next published
 * map raises seq above it, and at the next open it is served as
 * ordinary history for a map that was never published (§10's
 * phantom-first rule is exactly what this defends).
 *
 * The schedule needs a second damaged ring slot, so that "the first
 * invalid slot" and "the phantom's slot" are different slots and the
 * retry's choice between them is visible.
 */
static void
tringfailphantom(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Monmap mm;
	uchar hdr[Secsz];
	vlong phoff;
	int i;

	d = fresh();
	if((m = mustopen(d, "ringfail")) == nil){
		devclose(d);
		return;
	}
	commit(m, "ringfail", "map=A", 1);
	commit(m, "ringfail", "map=B", 2);
	commit(m, "ringfail", "map=C", 3);
	monstat(m, &st);

	/* the publish of epoch 9 dies in the phantom window */
	simcrashdead(d, 1);
	simarm(d, "monhistflush", 0);
	checks++;
	if(moncommit(m, "map=PHANTOM", 11, 9) == 0)
		fail("ringfail: a commit whose machine died reported success");
	monclose(m);
	simrevive(d);

	/* an unrelated media fault damages ring slot 0 */
	tearslot(d, histoffs(&st, 0));

	if((m = mustopen(d, "ringfail restart")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("ringfail: the unpublished map is a phantom", st.nphantom, 1);
	eqv("ringfail: two entries survive", st.nhist, 2);
	eqv("ringfail: the current map is still C", st.epoch, 3);

	/* the phantom's slot is the one carrying the unpublished seq */
	phoff = -1;
	for(i = 0; i < Retain; i++){
		simpeek(d, histoffs(&st, i), hdr, Secsz);
		if(memcmp(hdr, "shoalmap", 8) == 0 && GBIT64(hdr + 40) == 9)
			phoff = histoffs(&st, i);
	}
	checks++;
	if(phoff < 0){
		fail("ringfail: the phantom's ring slot is not on the disk");
		monclose(m);
		devclose(d);
		return;
	}

	/* the next commit's ring write is aimed at it, and fails */
	simfaultat(d, Sfeio, 1, phoff, st.slotsz);
	checks++;
	if(moncommit(m, "map=D", 5, 4) == 0)
		fail("ringfail: a commit whose ring write failed reported "
			"success");
	monstat(m, &st);
	eqv("ringfail: the current map is untouched", st.epoch, 3);
	eqv("ringfail: the phantom is still counted", st.nphantom, 1);

	/* the monitor retries, as it must: the phantom's slot is reused */
	checks++;
	if(moncommit(m, "map=D", 5, 4) < 0)
		fail("ringfail: the retry: %r");
	monstat(m, &st);
	eqv("ringfail: the retry consumed the phantom", st.nphantom, 0);
	eqv("ringfail: three entries after the retry", st.nhist, 3);
	monclose(m);

	if((m = mustopen(d, "ringfail restart 2")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("ringfail: D is current after the restart", st.epoch, 4);
	eqv("ringfail: no phantom is left", st.nphantom, 0);
	eqv("ringfail: and three published maps", st.nhist, 3);
	eqi("ringfail: the unpublished epoch answers nothing",
		monlookup(m, 9, &mm), 0);
	eqi("ringfail: position 0 is the current map",
		monhistory(m, 0, &mm), 1);
	eqv("ringfail: at the current seq", mm.seq, st.seq);
	monclose(m);
	devclose(d);
}

/*
 * A ring write that fails over a VALID victim.  With the ring full
 * the victim is an ordinary committed entry, and a write that landed
 * nothing left it on the platter: the live store must keep answering
 * that epoch and must not count a phantom the disk does not hold.
 * The failure path re-reads the slot to find that out.
 */
static void
tringfailvalid(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Monmap mm;
	uchar hdr[Secsz];
	char text[32];
	vlong voff;
	int i;

	d = fresh();
	if((m = mustopen(d, "ringfull")) == nil){
		devclose(d);
		return;
	}
	/* retain commits: every ring slot valid, none a phantom */
	for(i = 1; i <= Retain; i++){
		snprint(text, sizeof text, "map=%d", i);
		commit(m, "ringfull", text, i);
	}
	monstat(m, &st);
	eqv("ringfull: the ring is full", st.nhist, Retain);
	eqv("ringfull: with no phantom", st.nphantom, 0);

	/* the victim is the oldest entry: seq 1, epoch 1 */
	voff = -1;
	for(i = 0; i < Retain; i++){
		simpeek(d, histoffs(&st, i), hdr, Secsz);
		if(memcmp(hdr, "shoalmap", 8) == 0 && GBIT64(hdr + 32) == 1)
			voff = histoffs(&st, i);
	}
	checks++;
	if(voff < 0){
		fail("ringfull: the oldest entry is not on the disk");
		monclose(m);
		devclose(d);
		return;
	}

	simfaultat(d, Sfeio, 1, voff, st.slotsz);
	checks++;
	if(moncommit(m, "map=E", 5, 5) == 0)
		fail("ringfull: a commit whose ring write failed reported "
			"success");
	monstat(m, &st);
	eqv("ringfull: the current map is untouched", st.epoch, Retain);
	eqv("ringfull: the victim is still committed history", st.nhist,
		Retain);
	eqv("ringfull: and is no phantom", st.nphantom, 0);
	eqi("ringfull: the oldest epoch still answers", monlookup(m, 1, &mm),
		1);
	eqtext("ringfull: with its own text", &mm, "map=1");

	/* and a restart agrees with the live store */
	monclose(m);
	if((m = mustopen(d, "ringfull restart")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("ringfull: the restart finds the same current map", st.epoch,
		Retain);
	eqv("ringfull: the same history", st.nhist, Retain);
	eqv("ringfull: and no phantom", st.nphantom, 0);
	eqi("ringfull: the oldest epoch answers after it",
		monlookup(m, 1, &mm), 1);
	monclose(m);
	devclose(d);
}

/*
 * The monitor retries in the SAME session after a current-slot write
 * fails.  Nothing restarts here, so the phantom mark that failure
 * leaves has to hold in memory: the ring entry for the map that was
 * never published must stay out of every accessor, and the retry must
 * reuse that slot rather than spend a fresh one on it.  The crash
 * cases all restart, so this path was never exercised.
 */
static void
tcurfailretry(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Monmap mm;

	d = fresh();
	if((m = mustopen(d, "curfail")) == nil){
		devclose(d);
		return;
	}
	commit(m, "curfail", "map=A", 1);
	commit(m, "curfail", "map=B", 2);
	monstat(m, &st);

	/* the current write, and only it, takes an i/o error */
	simfaultat(d, Sfeio, 1, curoffs(&st, st.cur == 0 ? 1 : 0), st.slotsz);
	checks++;
	if(moncommit(m, "map=GHOST", 9, 7) == 0)
		fail("curfail: a commit whose current write failed reported "
			"success");
	monstat(m, &st);
	eqv("curfail: the current map is still B", st.epoch, 2);
	eqv("curfail: the unpublished ring entry is a phantom", st.nphantom,
		1);
	eqv("curfail: and is not a history entry", st.nhist, 2);
	eqi("curfail: the unpublished epoch answers nothing",
		monlookup(m, 7, &mm), 0);

	/* the retry, with no restart between */
	checks++;
	if(moncommit(m, "map=C", 5, 3) < 0)
		fail("curfail: the retry: %r");
	monstat(m, &st);
	eqv("curfail: the retry is current", st.epoch, 3);
	eqv("curfail: it consumed the phantom", st.nphantom, 0);
	eqv("curfail: three published maps", st.nhist, 3);
	eqi("curfail: the ghost is still unanswerable",
		monlookup(m, 7, &mm), 0);
	monclose(m);

	if((m = mustopen(d, "curfail restart")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("curfail: the restart finds the retry", st.epoch, 3);
	eqv("curfail: no phantom after it", st.nphantom, 0);
	eqv("curfail: three published maps after it", st.nhist, 3);
	eqi("curfail: and the ghost is gone from the disk",
		monlookup(m, 7, &mm), 0);
	monclose(m);
	devclose(d);
}

/*
 * §10's read-back, when the READ is what fails.  The write and the
 * flush under it both reported success, so the slot may be on the
 * platter and a media read has its own transients: the read is
 * retried once, and a commit whose read-back needed the retry is an
 * ordinary successful commit.
 */
static void
treadretry(int onring)
{
	Dev *sim, *d;
	Mon *m;
	Monstat st;
	char what[64];

	snprint(what, sizeof what, "readretry %s",
		onring ? "ring" : "current");
	sim = fresh();
	d = shimopen(sim);
	if((m = mustopen(d, what)) == nil){
		devclose(d);
		devclose(sim);
		return;
	}
	commit(m, what, "map=A", 1);
	commit(m, what, "map=B", 2);

	/* one read of the slot's read-back fails; the retry does not */
	shimarm(onring ? 1 : 3, 1);
	checks++;
	if(moncommit(m, "map=C", 5, 3) < 0)
		fail("%s: a read-back that read on the retry failed the "
			"commit: %r", what);
	shimarm(0, 0);
	monstat(m, &st);
	eqv2(what, "the retried read-back published the map", st.epoch, 3);
	eqv2(what, "at its own seq", st.seq, 3);
	eqv2(what, "with no phantom", st.nphantom, 0);
	eqv2(what, "and three published maps", st.nhist, 3);
	monclose(m);

	if((m = mustopen(d, what)) == nil){
		devclose(d);
		devclose(sim);
		return;
	}
	monstat(m, &st);
	eqv2(what, "the restart finds it", st.epoch, 3);
	eqv2(what, "and no phantom", st.nphantom, 0);
	monclose(m);
	devclose(d);
	devclose(sim);
}

/*
 * The same read failing twice: §10's INDETERMINATE publish.  The
 * commit fails — the monitor never acknowledges it (layer-a §8.2) —
 * but the bytes may be on the platter, so the store makes itself safe
 * against whatever landed rather than pretending nothing did: the
 * slot is not served, its seq is spent so the next commit outranks
 * it, and a ring victim is phantom-first for reuse.  The restart then
 * serves the retry and never the orphan, and no two ring entries
 * share a seq.
 */
static void
tindeterminate(int onring)
{
	Dev *sim, *d;
	Mon *m;
	Monstat st;
	Monmap mm;
	char what[64], err[ERRMAX];

	snprint(what, sizeof what, "indeterminate %s",
		onring ? "ring" : "current");
	sim = fresh();
	d = shimopen(sim);
	if((m = mustopen(d, what)) == nil){
		devclose(d);
		devclose(sim);
		return;
	}
	commit(m, what, "map=A", 1);
	commit(m, what, "map=B", 2);

	/* the read-back and its one retry both fail */
	shimarm(onring ? 1 : 3, 2);
	checks++;
	if(moncommit(m, "map=C", 5, 3) == 0)
		fail("%s: a commit whose read-back would not read reported "
			"success", what);
	rerrstr(err, sizeof err);
	istrue("the refusal says the publish is indeterminate",
		strstr(err, "indeterminate") != nil);
	shimarm(0, 0);
	monstat(m, &st);
	eqv2(what, "the live store is still B", st.epoch, 2);
	eqv2(what, "at seq 2", st.seq, 2);
	eqv2(what, "the unserved slot leaves one phantom", st.nphantom, 1);
	eqv2(what, "and two published maps", st.nhist, 2);
	eqi2(what, "the indeterminate epoch answers nothing",
		monlookup(m, 3, &mm), 0);

	/* the retry, in the same session: its seq is above the orphan's */
	checks++;
	if(moncommit(m, "map=D", 5, 4) < 0)
		fail("%s: the retry: %r", what);
	monstat(m, &st);
	eqv2(what, "the retry is current", st.epoch, 4);
	eqv2(what, "at a seq above the indeterminate one", st.seq, 4);
	eqv2(what, "which consumed the phantom", st.nphantom, 0);
	eqv2(what, "leaving three published maps", st.nhist, 3);
	monclose(m);

	if((m = mustopen(d, what)) == nil){
		devclose(d);
		devclose(sim);
		return;
	}
	monstat(m, &st);
	eqv2(what, "the restart serves the retry", st.epoch, 4);
	eqv2(what, "at its seq", st.seq, 4);
	eqi2(what, "and not the orphan", monlookup(m, 3, &mm), 0);
	eqv2(what, "with no phantom left", st.nphantom, 0);
	eqv2(what, "and three published maps", st.nhist, 3);
	eqi2(what, "position 0 is the current map", monhistory(m, 0, &mm), 1);
	eqv2(what, "at the current seq", mm.seq, st.seq);
	noseqdups(sim, &st, what);
	monclose(m);
	devclose(d);
	devclose(sim);
}

/*
 * Ring wrap.  More commits than retain: the oldest seq is the victim
 * each time, so the ring always holds the newest retain maps and
 * layer-a §5.2 clause 2's E-1 entry is always one of them.
 */
static void
tring(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Monmap mm;
	char text[32];
	int i, j;

	d = fresh();
	if((m = mustopen(d, "ring")) == nil){
		devclose(d);
		return;
	}
	for(i = 1; i <= 3*Retain; i++){
		snprint(text, sizeof text, "map=%d", i);
		if(commit(m, "ring", text, i) < 0)
			break;
		monstat(m, &st);
		eqv("ring: the ring never holds more than retain", st.nhist,
			i < Retain ? i : Retain);
		/* the E-1 entry is always present */
		if(i > 1){
			eqi("ring: E-1 is in the ring",
				monhistory(m, 1, &mm), 1);
			eqv("ring: E-1's epoch", mm.epoch, i - 1);
		}
		/* and the ring is exactly the newest retain seqs */
		for(j = 0; j < Retain && j < i; j++){
			eqi("ring: position present",
				monhistory(m, j, &mm), 1);
			eqv("ring: position's seq", mm.seq, i - j);
		}
		eqi("ring: nothing past the ring",
			monhistory(m, Retain, &mm), 0);
	}
	monclose(m);
	if((m = mustopen(d, "ring restart")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("ring: the newest map survives the restart", st.seq, 3*Retain);
	eqv("ring: retain entries after the restart", st.nhist, Retain);
	eqi("ring: E-1 after the restart", monhistory(m, 1, &mm), 1);
	eqv("ring: E-1's epoch after the restart", mm.epoch, 3*Retain - 1);
	eqi("ring: the oldest epochs are gone",
		monlookup(m, 1, &mm), 0);
	monclose(m);
	devclose(d);
}

/* §10's disk full, and the largest map that fits */
static void
tfull(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Monmap mm;
	uchar *big;
	char err[ERRMAX];

	d = fresh();
	if((m = mustopen(d, "full")) == nil){
		devclose(d);
		return;
	}
	commit(m, "full", "map=A", 1);
	big = mktext(Maxmap + 1, 3);
	checks++;
	if(moncommit(m, big, Maxmap + 1, 2) == 0)
		fail("full: a map larger than a slot committed");
	rerrstr(err, sizeof err);
	istrue("full: the refusal is layer-a §2.6's disk full",
		strncmp(err, "disk full", 9) == 0);
	monstat(m, &st);
	eqv("full: the store is unchanged", st.epoch, 1);
	eqv("full: at the same seq", st.seq, 1);

	/* the largest map that fits commits and reads back byte-exact */
	checks++;
	if(moncommit(m, big, Maxmap, 2) < 0)
		fail("full: the largest map that fits: %r");
	monclose(m);
	if((m = mustopen(d, "full restart")) == nil){
		free(big);
		devclose(d);
		return;
	}
	eqi("full: the largest map comes back", moncurrent(m, &mm), 1);
	eqv("full: at its full length", mm.len, Maxmap);
	checks++;
	if(mm.len != Maxmap || memcmp(mm.text, big, Maxmap) != 0)
		fail("full: the largest map did not come back byte-exact");
	monstat(m, &st);
	eqv("full: and it is the current map", st.epoch, 2);
	free(big);
	monclose(m);
	devclose(d);
}

/*
 * The header copies.  Nothing writes either after format, so they are
 * identical: take either valid one, refuse if neither, and refuse a
 * pair that differs rather than choosing between them.
 */
static void
theader(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Monhsel sel;
	uchar hdr[Secsz];
	char err[ERRMAX];
	vlong off1;

	d = fresh();
	off1 = d->size - Secsz;

	/* copy 0 damaged: the store opens from copy 1 and says so */
	simpeek(d, 0, hdr, Secsz);
	hdr[100] ^= 0x40;
	simpoke(d, 0, hdr, Secsz);
	if((m = mustopen(d, "header copy 1")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqi("header: the open took copy 1", st.hdr, 1);
	eqi("header: copy 0 was not valid", st.hdrother, 0);
	monclose(m);

	/* both damaged: refuse */
	simpeek(d, off1, hdr, Secsz);
	hdr[100] ^= 0x40;
	simpoke(d, off1, hdr, Secsz);
	checks++;
	if((m = monopen(d)) != nil){
		fail("header: a store with no valid header opened");
		monclose(m);
	}
	devclose(d);

	/*
	 * Two valid copies that differ: a refusal naming the field, not
	 * a choice.  The operator has mixed two partitions' halves, or
	 * the media is lying about what it wrote.
	 */
	d = fresh();
	simpeek(d, 0, hdr, Secsz);
	PBIT32(hdr + 36, Retain + 1);		/* retain */
	reccsumset(hdr, Secsz, 16);
	simpoke(d, off1, hdr, Secsz);
	checks++;
	if(monhdrsel(d, &sel) == 0)
		fail("header: two header copies that differ were accepted");
	rerrstr(err, sizeof err);
	istrue("header: the refusal names the field",
		strstr(err, "retain") != nil);
	eqi("header: both copies are valid", sel.valid[0] && sel.valid[1], 1);
	checks++;
	if((m = monopen(d)) != nil){
		fail("header: a store whose header copies differ opened");
		monclose(m);
	}
	devclose(d);
}

/*
 * A header whose curoff/histoff do not describe the geometry is
 * refused, naming them.  Such a header locates the slots anywhere at
 * all — every offset below it is meaningless — so it is caught in
 * hdrsane before a byte is read through it, and not left to whatever
 * the wrong sectors happen to hold.  Both copies carry it, so the
 * "copies differ" refusal is not what answers.
 */
static void
thdroffsets(void)
{
	Dev *d;
	Mon *m;
	uchar hdr[Secsz];
	char err[ERRMAX];

	d = fresh();
	simpeek(d, 0, hdr, Secsz);
	PBIT64(hdr + 40, (uvlong)2);		/* curoff, which must be 1 */
	reccsumset(hdr, Secsz, 16);
	simpoke(d, 0, hdr, Secsz);
	simpoke(d, d->size - Secsz, hdr, Secsz);
	checks++;
	if((m = monopen(d)) != nil){
		fail("hdroffsets: a header whose offsets are wrong opened");
		monclose(m);
	}
	rerrstr(err, sizeof err);
	istrue("hdroffsets: the refusal names curoff",
		strstr(err, "curoff") != nil);
	devclose(d);

	/* and the same for histoff, which must follow the two slots */
	d = fresh();
	simpeek(d, 0, hdr, Secsz);
	PBIT64(hdr + 48, (uvlong)3);		/* histoff */
	reccsumset(hdr, Secsz, 16);
	simpoke(d, 0, hdr, Secsz);
	simpoke(d, d->size - Secsz, hdr, Secsz);
	checks++;
	if((m = monopen(d)) != nil){
		fail("hdroffsets: a header whose histoff is wrong opened");
		monclose(m);
	}
	rerrstr(err, sizeof err);
	istrue("hdroffsets: the refusal names histoff",
		strstr(err, "histoff") != nil);
	devclose(d);
}

/*
 * §10's tie: a fresh format leaves both current slots valid at seq 0,
 * so the open takes slot 0 and the first commit writes slot 1.  With
 * the tie the other way a fresh store would open on the very slot the
 * first commit overwrites.
 */
static void
topentie(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Simop *t;
	vlong slot1;
	long n, i;
	int wrote;

	d = fresh();
	if((m = mustopen(d, "opentie")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqi("opentie: a fresh store opens on slot 0", st.cur, 0);
	slot1 = curoffs(&st, 1);
	simtracereset(d);
	commit(m, "opentie", "map=A", 1);
	n = simtrace(d, &t);
	wrote = 0;
	for(i = 0; i < n; i++)
		if(t[i].op == Sopwrite && t[i].off == slot1)
			wrote = 1;
	istrue("opentie: the first commit writes current slot 1", wrote);
	monstat(m, &st);
	eqi("opentie: which is then the current slot", st.cur, 1);
	monclose(m);
	devclose(d);
}

/* neither current slot valid is damage, and it is refused */
static void
tnocur(void)
{
	Dev *d;
	Mon *m;
	Monstat st;

	d = fresh();
	if((m = mustopen(d, "nocur")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	monclose(m);
	tearslot(d, curoffs(&st, 0));
	if((m = mustopen(d, "nocur with one slot torn")) != nil)
		monclose(m);
	tearslot(d, curoffs(&st, 1));
	checks++;
	if((m = monopen(d)) != nil){
		fail("nocur: a store with neither current slot valid opened");
		monclose(m);
	}
	devclose(d);
}

/*
 * §10: a slot's len is bounds-checked BEFORE the len bytes it names
 * are read, so a torn length field cannot drive a read past the slot.
 * The slot below carries a len of two slots and a checksum that is
 * correct over exactly those bytes, so only the bound refuses it.
 */
static void
tbadlen(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	uchar *p;
	ulong len, n;
	vlong off;

	d = fresh();
	if((m = mustopen(d, "badlen")) == nil){
		devclose(d);
		return;
	}
	commit(m, "badlen", "map=A", 1);
	commit(m, "badlen", "map=B", 2);
	monstat(m, &st);
	monclose(m);

	/* the slot that is NOT current, so only its len decides */
	off = curoffs(&st, st.cur == 0 ? 1 : 0);
	len = 2*Slotsz;
	n = Secsz + len;
	if((p = malloc(n)) == nil)
		sysfatal("malloc: %r");
	simpeek(d, off, p, n);
	memmove(p, "shoalmap", 8);
	PBIT32(p + 8, Monvers);
	PBIT32(p + 12, len);
	PBIT64(p + 32, (uvlong)99);		/* a seq above every other */
	PBIT64(p + 40, (uvlong)99);
	reccsumset(p, n, 16);
	simpoke(d, off, p, Secsz);
	free(p);

	if((m = mustopen(d, "badlen")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("badlen: a slot whose len does not fit is not taken", st.seq, 2);
	eqv("badlen: the current map is the real one", st.epoch, 2);
	monclose(m);
	devclose(d);
}

/*
 * Open writes nothing, even with a phantom to ignore: a phantom is
 * marked reusable in memory and erased by the write that reuses its
 * slot.  That is what lets an open work on a Drdonly device, which
 * timage takes over a file image.
 */
static void
treadonly(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Simop *t;
	long i, n, nw;

	d = fresh();
	if((m = mustopen(d, "readonly")) == nil){
		devclose(d);
		return;
	}
	commit(m, "readonly", "map=A", 1);
	commit(m, "readonly", "map=B", 2);

	/* leave a phantom behind, so the open has something to ignore */
	simcrashdead(d, 1);
	simarm(d, "monhistflush", 0);
	moncommit(m, "map=C", 5, 3);
	monclose(m);
	simrevive(d);

	simtracereset(d);
	if((m = mustopen(d, "readonly open")) == nil){
		devclose(d);
		return;
	}
	monstat(m, &st);
	eqv("readonly: the open found the phantom", st.nphantom, 1);
	eqv("readonly: and the previous map", st.epoch, 2);
	n = simtrace(d, &t);
	nw = 0;
	for(i = 0; i < n; i++)
		if(t[i].op != Sopread)
			nw++;
	eqv("readonly: an open writes and flushes nothing", nw, 0);
	checks++;
	if(n == 0)
		fail("readonly: the open read nothing at all");
	monclose(m);
	devclose(d);
}

static char imgpath[64];

/* the format tool's path: a file image through fileopen */
static void
timage(void)
{
	Dev *d;
	Mon *m;
	Monfmtcfg c;
	Monstat st;
	Monmap mm;

	remove(imgpath);
	if((d = fileopen(imgpath, Secsz, (vlong)Nsec*Secsz, 0)) == nil){
		fail("image: fileopen: %r");
		return;
	}
	moncfg(&c);
	checks++;
	if(monfmt(d, &c) < 0){
		fail("image: monfmt: %r");
		devclose(d);
		return;
	}
	eqv("image: the format reports its slotsz", c.slotsz, Slotsz);
	eqv("image: and its retain", c.retain, Retain);
	eqv("image: and what it used", c.used,
		2*Secsz + (2 + Retain)*(uvlong)Slotsz);
	eqi("image: no object-store superblock here", c.warnsuper, 0);
	if((m = mustopen(d, "image")) == nil){
		devclose(d);
		return;
	}
	commit(m, "image", "map=image epoch=7", 7);
	monclose(m);
	devclose(d);

	/* reopened read-only: the same choices, and no write to make */
	if((d = fileopen(imgpath, Secsz, 0, Drdonly)) == nil){
		fail("image: read-only fileopen: %r");
		remove(imgpath);
		return;
	}
	if((m = mustopen(d, "image read-only")) == nil){
		devclose(d);
		remove(imgpath);
		return;
	}
	monstat(m, &st);
	eqv("image: the map survived the close", st.epoch, 7);
	eqv("image: at seq 1", st.seq, 1);
	eqi("image: and reads back", moncurrent(m, &mm), 1);
	eqtext("image: the map text", &mm, "map=image epoch=7");
	monclose(m);
	devclose(d);
	remove(imgpath);
}

/*
 * §12's refusals and its one warning.  shoalmonfmt is argument
 * parsing over monfmt, so these are driven where the tool drives
 * them, exactly as fmtcktest drives fmtstore.
 */
static void
trefuse(void)
{
	Dev *d;
	Monfmtcfg c;
	Fmtcfg fc;
	Super s;
	char err[ERRMAX];

	/* a valid monitor header without -r */
	d = fresh();
	moncfg(&c);
	checks++;
	if(monfmt(d, &c) == 0)
		fail("refuse: a format over a valid monitor header");
	rerrstr(err, sizeof err);
	istrue("refuse: and it says why",
		strstr(err, "valid monitor header") != nil);
	moncfg(&c);
	c.ream = 1;
	checks++;
	if(monfmt(d, &c) < 0)
		fail("refuse: -r over a valid monitor header: %r");
	devclose(d);

	/* a device under §10's 1 MiB floor */
	if((d = simopen(Secsz, 1024, Seed)) == nil)
		sysfatal("simopen: %r");
	moncfg(&c);
	checks++;
	if(monfmt(d, &c) == 0)
		fail("refuse: a format of a partition under 1 MiB");
	devclose(d);

	/* retain below layer-a §5.2 clause 2's floor */
	if((d = simopen(Secsz, Nsec, Seed)) == nil)
		sysfatal("simopen: %r");
	moncfg(&c);
	c.retain = 1;
	checks++;
	if(monfmt(d, &c) == 0)
		fail("refuse: a format at retain 1");
	rerrstr(err, sizeof err);
	istrue("refuse: and it names retain", strstr(err, "retain") != nil);

	/* a slotsz that is not a sector multiple, and one under two sectors */
	moncfg(&c);
	c.slotsz = Slotsz + 1;
	checks++;
	if(monfmt(d, &c) == 0)
		fail("refuse: a slotsz that is not a sector multiple");
	moncfg(&c);
	c.slotsz = Secsz;
	checks++;
	if(monfmt(d, &c) == 0)
		fail("refuse: a slotsz of one sector");

	/* a geometry that does not fit the device */
	moncfg(&c);
	c.slotsz = 256*1024;
	c.retain = 8;
	checks++;
	if(monfmt(d, &c) == 0)
		fail("refuse: a geometry larger than the device");
	devclose(d);

	/*
	 * §12: the target already carries a valid object-store
	 * superblock, so the unit is an instance's and §2.1 forbids
	 * sharing it.  A warning, not a refusal.
	 */
	if((d = simopen(Secsz, Bigsec, Seed)) == nil)
		sysfatal("simopen: %r");
	memset(&fc, 0, sizeof fc);
	fc.secsz = Secsz;
	fc.blksz = 4096;
	fc.objmax = 65536;
	fc.nslots = 512;
	fc.nemap = 256;
	fc.ndirty = 256;
	fc.logbytes = 256*1024;
	fc.csumalg = Csumblake2s;
	if(geometry(&s, &fc, d->size) < 0)
		sysfatal("geometry: %r");
	if(fmtstore(d, &s) < 0)
		sysfatal("fmtstore: %r");
	moncfg(&c);
	checks++;
	if(monfmt(d, &c) < 0)
		fail("refuse: a format over an object store was refused: %r");
	eqi("refuse: the object-store superblock is reported", c.warnsuper, 1);
	/* and once formatted, it is a monitor store like any other */
	moncfg(&c);
	c.ream = 1;
	checks++;
	if(monfmt(d, &c) < 0)
		fail("refuse: reformatting it: %r");
	eqi("refuse: and the superblock is gone with it", c.warnsuper, 0);
	devclose(d);
}

/*
 * Duplicate epochs.  layer-a §8.3's forceepoch and §8.6's rebuild
 * path can publish an epoch that is not above the last, so this store
 * never compares epochs: it records what it is given.  Where two ring
 * entries carry one epoch, the by-epoch lookup answers the newer one.
 */
static void
tepochs(void)
{
	Dev *d;
	Mon *m;
	Monstat st;
	Monmap mm;

	d = fresh();
	if((m = mustopen(d, "epochs")) == nil){
		devclose(d);
		return;
	}
	commit(m, "epochs", "map=five", 5);
	commit(m, "epochs", "map=four", 4);
	monstat(m, &st);
	eqv("epochs: a regression is recorded, not refused", st.epoch, 4);
	eqv("epochs: at the next seq", st.seq, 2);
	commit(m, "epochs", "map=five-again", 5);
	monclose(m);

	if((m = mustopen(d, "epochs restart")) == nil){
		devclose(d);
		return;
	}
	eqi("epochs: epoch 5 is found", monlookup(m, 5, &mm), 1);
	eqv("epochs: and it is the newer publish of it", mm.seq, 3);
	eqtext("epochs: whose text is the newer one", &mm, "map=five-again");
	eqi("epochs: epoch 4 is found", monlookup(m, 4, &mm), 1);
	eqv("epochs: at its own seq", mm.seq, 2);
	monclose(m);
	devclose(d);
}

static void
cleanup(void)
{
	remove(imgpath);
}

void
main(int, char**)
{
	snprint(imgpath, sizeof imgpath, "/tmp/shoalmon.%d.img", getpid());
	atexit(cleanup);

	tfresh();
	tfmtprologue();
	tcommit();
	tflushes();
	tlostwrite(Sfdrop, "Sfdrop", 1);
	tlostwrite(Sfdrop, "Sfdrop", 0);
	tlostwrite(Sftearbyte, "Sftearbyte", 1);
	tlostwrite(Sftearbyte, "Sftearbyte", 0);
	tslots();
	tphantom();
	tcrashmatrix();
	thistfail();
	tringfailphantom();
	tringfailvalid();
	tcurfailretry();
	treadretry(1);
	treadretry(0);
	tindeterminate(1);
	tindeterminate(0);
	tring();
	tfull();
	theader();
	thdroffsets();
	topentie();
	tnocur();
	tbadlen();
	treadonly();
	timage();
	trefuse();
	tepochs();
	if(fails > 0)
		exits("failed");
	print("montest: %d checks ok\n", checks);
	exits(nil);
}
