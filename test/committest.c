#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"
#include "t1.h"

/*
 * T1 for the commit path: docs/design/store.md §3.2's write sequence
 * and its two flushes, §3.4's crash points, §2.7's log continuity and
 * its wrap record, §7's group commit and durable watermark, and
 * §2.8's reclaim rule and checkpoint mark.
 *
 * The mechanism this design most depends on — the placement of the
 * two flushes — cannot be discriminated on the reference hardware at
 * all, so everything here runs against the simulated disk.
 */

enum
{
	Blk	= 4096,
};

/* what an object looked like, so a crash can be judged against it */
typedef struct Snap Snap;
struct Snap
{
	uvlong	len, ver, wepoch;
	uchar	csum[Csumlen];
	int	state;
};

static void
snap(Store *s, char *name, Snap *sn)
{
	Objinfo oi;
	uchar o[Oidmax];

	oidof(o, name);
	memset(sn, 0, sizeof *sn);
	if(objstat(s, o, strlen(name), &oi) < 0)
		return;
	sn->len = oi.len;
	sn->ver = oi.ver;
	sn->wepoch = oi.wepoch;
	sn->state = oi.state;
	memmove(sn->csum, oi.csum, Csumlen);
}

static int
sameas(Snap *a, Snap *b)
{
	return a->len == b->len && a->ver == b->ver && a->wepoch == b->wepoch
		&& a->state == b->state
		&& memcmp(a->csum, b->csum, Csumlen) == 0;
}

static void
mk(Store *s, char *name)
{
	uchar o[Oidmax];

	oidof(o, name);
	if(objcreate(s, o, strlen(name), 1, 1, nil) < 0)
		fail("objcreate %s: %r", name);
}

static int
wr(Store *s, char *name, void *a, long n, uvlong off, uvlong ver)
{
	uchar o[Oidmax];

	oidof(o, name);
	return objwrite(s, o, strlen(name), a, n, off, ver, 1, nil, 0);
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

/*
 * T1.13.  From the recorded device trace, a flush precedes the header
 * write and another follows it, for every commit shape.  This is the
 * test that makes T1.1's flush mutations detectable as a *sequence*
 * even where they are not detectable as a loss — on the reference
 * hardware removing a flush changes nothing a process crash can see.
 */
static void
tflushseq(void)
{
	Dev *d;
	Store *s;
	Simop *t;
	Super sb;
	Sbsel sel;
	uchar *buf;
	long n, i, last;
	vlong lo, hi;

	d = newdisk();
	if((s = mustopen(d, "flush sequence")) == nil)
		return;
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	sb = sel.sb[sel.start];
	lo = (vlong)sb.logoff*sb.secsz;
	hi = lo + (vlong)sb.logsecs*sb.secsz;
	mk(s, "f");
	buf = mkbuf(3*Blk, 61);

	/* a lone one-sector commit: flush, write, flush */
	simtracereset(d);
	if(wr(s, "f", buf, 64, 0, 2) < 0)
		fail("objwrite: %r");
	n = simtrace(d, &t);
	last = -1;
	for(i = 0; i < n; i++)
		if(t[i].op == Sopwrite && t[i].off >= lo && t[i].off < hi)
			last = i;
	checks++;
	if(last < 1)
		fail("no log write in the trace of a commit");
	else{
		eqv("the header write is one sector", t[last].n, sb.secsz);
		checks++;
		if(t[last-1].op != Sopflush)
			fail("no flush precedes the header write");
		checks++;
		if(last+1 >= n || t[last+1].op != Sopflush)
			fail("no flush follows the header write");
	}

	/*
	 * A commit whose record needs a body: the body sectors go
	 * first, then the pre-flush, then the header, then the
	 * post-flush.  The header goes last unconditionally, which is
	 * what makes the commit point one sector for any record size.
	 */
	simtracereset(d);
	if(wr(s, "f", buf, 3*Blk, 0, 3) < 0)
		fail("objwrite: %r");
	n = simtrace(d, &t);
	last = -1;
	for(i = 0; i < n; i++)
		if(t[i].op == Sopwrite && t[i].off >= lo && t[i].off < hi)
			last = i;
	checks++;
	if(last < 1)
		fail("no log write in the trace of a multi-sector commit");
	else{
		eqv("the header write is still one sector", t[last].n, sb.secsz);
		eqv("and it is the record's first sector", t[last].off < t[last-2].off
			|| t[last-2].op != Sopwrite, 1);
		checks++;
		if(t[last-1].op != Sopflush)
			fail("no flush precedes the header write");
		checks++;
		if(last+1 >= n || t[last+1].op != Sopflush)
			fail("no flush follows the header write");
	}
	free(buf);
	storeclose(s);
	devclose(d);
}

/*
 * T1.2, the torn-header sweep.  A record is valid only if its
 * checksum verifies over its whole byte range and its sequence number
 * is the expected successor.  A header torn anywhere leaves old bytes
 * or new bytes in every field, and either way the stored digest and
 * the hashed range disagree — so every mixture but the whole new
 * header must leave the *old* four-tuple, and the whole new header
 * must leave the new one.
 */
static void
ttorn(void)
{
	Dev *d;
	Store *s;
	Super sb;
	Sbsel sel;
	Snap before, after, got;
	uchar *buf, *old, *new, *mix;
	vlong off;
	ulong k;
	int bad;

	d = newdisk();
	if((s = mustopen(d, "torn header")) == nil)
		return;
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	sb = sel.sb[sel.start];
	buf = mkbuf(64, 67);
	mk(s, "t");
	if(wr(s, "t", buf, 64, 0, 2) < 0)
		fail("objwrite: %r");
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	snap(s, "t", &before);

	/* the sector the next commit's header will land in */
	{
		Storestat st;

		storestat(s, &st);
		off = (vlong)st.cklogoff*sb.secsz;
	}
	old = malloc(sb.secsz);
	new = malloc(sb.secsz);
	mix = malloc(sb.secsz);
	if(old == nil || new == nil || mix == nil)
		sysfatal("malloc: %r");
	simpeek(d, off, old, sb.secsz);
	if(wr(s, "t", buf, 64, 128, 3) < 0)
		fail("objwrite: %r");
	snap(s, "t", &after);
	simpeek(d, off, new, sb.secsz);
	storeclose(s);
	checks++;
	if(memcmp(old, new, sb.secsz) == 0)
		fail("the commit did not land where the checkpoint mark said");

	bad = 0;
	for(k = 0; k <= sb.secsz && !bad; k++){
		memmove(mix, old, sb.secsz);
		memmove(mix, new, k);
		simpoke(d, off, mix, sb.secsz);
		if((s = openstore(d)) == nil){
			fail("a torn header must not stop the store starting "
				"(%lud new bytes): %r", k);
			bad = 1;
			break;
		}
		/*
		 * A prefix long enough to cover every byte the record
		 * differs in is not a tear at all — it is the record.
		 * Anything short of that must leave the old four-tuple,
		 * because the checksum covers the whole sector.
		 */
		snap(s, "t", &got);
		if(!sameas(&got, memcmp(mix, new, sb.secsz) == 0 ? &after
			: &before)){
			fail("a header with %lud new bytes left neither the "
				"old nor the new four-tuple", k);
			bad = 1;
		}
		storeclose(s);
	}
	checks++;
	free(old);
	free(new);
	free(mix);
	free(buf);
	devclose(d);
}

/*
 * T1.3.  devsd truncates a request rather than splitting or failing,
 * so a short count is normal and is never an error; every access in
 * the store loops until the whole range is done.
 */
static void
tshort(void)
{
	Dev *d;
	Store *s;
	uchar *buf, *got;

	d = newdisk();
	simfault(d, Sfshort, 0);		/* sticky: every call */
	if((s = mustopen(d, "short counts")) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(3*Blk, 71);
	if((got = malloc(3*Blk)) == nil)
		sysfatal("malloc: %r");
	mk(s, "sh");
	if(wr(s, "sh", buf, 3*Blk, 0, 2) < 0)
		fail("objwrite under short counts: %r");
	mustverify(s, "sh", "short counts");
	storeclose(s);
	if((s = mustopen(d, "short counts, replayed")) == nil){
		devclose(d);
		return;
	}
	mustverify(s, "sh", "short counts, replayed");
	storeclose(s);
	simfault(d, Sfnone, 0);
	devclose(d);
	free(buf);
	free(got);
}

/*
 * T1.1, the crash matrix.  R2 is the invariant: after any crash an
 * object's published four-tuple is exactly the pre-update or exactly
 * the post-update value, never a mix.  Every point of §3.4 against
 * every shape of operation, with the object re-verified after the
 * restart.
 */
static void
onecrash(char *point, int mode, char *op, int expectnew)
{
	Dev *d;
	Store *s;
	Snap before, after, got;
	uchar *buf, o[Oidmax];
	char what[128];
	int r;

	snprint(what, sizeof what, "%s at %s", op, point);
	d = newdisk();
	if((s = mustopen(d, what)) == nil)
		return;
	buf = mkbuf(3*Blk, 73);
	mk(s, "m");
	if(wr(s, "m", buf, 2*Blk, 0, 2) < 0)
		fail("%s: setup write: %r", what);
	if(storecheckpoint(s) < 0)
		fail("%s: storecheckpoint: %r", what);
	snap(s, "m", &before);

	simcrashdead(d, 1);
	simcrashmode(d, mode);
	simarm(d, point, 0);
	oidof(o, "m");
	if(strcmp(op, "create") == 0)
		r = objcreate(s, (uchar*)"n", 1, 3, 1, nil);
	else if(strcmp(op, "whole-block write") == 0)
		r = wr(s, "m", buf, Blk, 0, 3);
	else if(strcmp(op, "partial write") == 0)
		r = wr(s, "m", buf, 100, 7, 3);
	else if(strcmp(op, "truncate") == 0)
		r = objtrunc(s, o, 1, Blk, 3, 1);
	else
		r = objremove(s, o, 1, 3, 1);
	USED(r);
	storeclose(s);
	simrevive(d);

	if((s = mustopen(d, what)) == nil){
		devclose(d);
		free(buf);
		return;
	}
	if(strcmp(op, "create") == 0){
		/* the four-tuple under test is the new object's existence */
		Objinfo oi;

		checks++;
		if((objstat(s, (uchar*)"n", 1, &oi) == 0) != expectnew)
			fail("%s: the create %s survive", what,
				expectnew ? "should" : "should not");
	}else{
		snap(s, "m", &got);
		checks++;
		if(expectnew){
			if(sameas(&got, &before))
				fail("%s: the update was lost after its "
					"post-flush returned", what);
		}else if(!sameas(&got, &before)){
			after = got;
			fail("%s: the four-tuple is neither the old one nor "
				"unchanged (len %llud ver %llud)", what,
				after.len, after.ver);
		}
		mustverify(s, "m", what);
	}
	storeclose(s);
	devclose(d);
	free(buf);
}

static void
tmatrix(void)
{
	static char *ops[] = {
		"create", "whole-block write", "partial write",
		"truncate", "delete",
	};
	int i;

	for(i = 0; i < nelem(ops); i++){
		/*
		 * before the record is durable: the old value, always.
		 * The stage point is where the last staged grain write
		 * returns, so it exists only for the operations that
		 * write content; a create, a truncate to a block boundary
		 * and a delete are metadata-only commits (§11).
		 */
		if(i == 1 || i == 2)
			onecrash("stage", Scdrop, ops[i], 0);
		onecrash("precommit", Scdrop, ops[i], 0);
		onecrash("commit", Scdrop, ops[i], 0);
		onecrash("postwrite", Scdrop, ops[i], 0);
		/* the header landed and survived: the new value */
		onecrash("postwrite", Sckeep, ops[i], 1);
		/* after the post-flush: the new value, guaranteed */
		onecrash("preack", Scdrop, ops[i], 1);
	}
}

/*
 * T1.11 and T1.4.  A record MUST NOT straddle the end of the region:
 * one that ends flush with the end wraps by arithmetic, and Fwrap
 * covers the case where sectors remain but too few for the next
 * record.  Driving the log several times round the ring exercises
 * both, and the records left ahead of the tail from earlier laps are
 * what the sequence seed exists to reject.
 */
static void
twrap(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	uchar *small, *big;
	char name[32];
	int i, j;

	d = newdisk();
	if((s = mustopen(d, "log wrap")) == nil)
		return;
	small = mkbuf(64, 79);
	big = mkbuf(16*Blk, 83);
	for(i = 0; i < 4; i++){
		snprint(name, sizeof name, "w%d", i);
		mk(s, name);
	}
	/*
	 * A mix of one-sector and multi-sector records, so the tail
	 * reaches the region end exactly and also lands with too few
	 * sectors left for the next record.
	 */
	for(i = 0; i < 60; i++){
		for(j = 0; j < 4; j++){
			snprint(name, sizeof name, "w%d", j);
			if(wr(s, name, small, 64, (uvlong)j*64, 2 + i) < 0)
				fail("small write %d: %r", i);
		}
		snprint(name, sizeof name, "w%d", i % 4);
		if(wr(s, name, big, 16*Blk, 0, 200 + i) < 0)
			fail("big write %d: %r", i);
		if(storecheckpoint(s) < 0)
			fail("storecheckpoint: %r");
	}
	storestat(s, &st);
	istrue("the log wrapped several times", st.seqnext > 128);
	for(i = 0; i < 4; i++){
		snprint(name, sizeof name, "w%d", i);
		mustverify(s, name, "after several laps");
	}

	/* now a crash mid-lap: replay must stop at the first bad record */
	snprint(name, sizeof name, "w0");
	if(wr(s, name, small, 64, 0, 999) < 0)
		fail("write before the crash: %r");
	simcrashdead(d, 1);
	simcrashmode(d, Scdrop);
	simarm(d, "commit", 0);
	wr(s, name, small, 64, 0, 1000);
	storeclose(s);
	simrevive(d);
	if((s = mustopen(d, "after a lap and a crash")) == nil){
		devclose(d);
		return;
	}
	for(i = 0; i < 4; i++){
		snprint(name, sizeof name, "w%d", i);
		mustverify(s, name, "after a lap and a crash");
	}
	{
		Objinfo oi;

		if(objstat(s, (uchar*)"w0", 2, &oi) < 0)
			fail("objstat: %r");
		else
			eqv("the last acked write survived the lap",
				oi.ver, 999);
	}
	storeclose(s);
	devclose(d);
	free(small);
	free(big);
}

/*
 * A geometry whose maximal record is four sectors.  At the small
 * geometry's two, a record that does not fit the tail always leaves
 * exactly one sector free, so the modular continuation rule reaches
 * the region start whether or not Fwrap is honoured and Fwrap is
 * never the only thing that gets replay there — which is why the
 * three mutations about a multi-sector record at the region boundary
 * need a geometry of their own.
 */
static Dev*
wrapdisk(void)
{
	Dev *d;
	Super s;
	Fmtcfg c;

	if((d = simopen(Tsecsz, Tnsec, Tseed)) == nil)
		sysfatal("simopen: %r");
	smallcfg(&c);
	c.objmax = 4*65536;		/* 64 blocks, so 64 map triples */
	c.nslots = 32;
	c.nemap = 16;
	if(geometry(&s, &c, d->size) < 0)
		sysfatal("geometry: %r");
	if(fmtstore(d, &s) < 0)
		sysfatal("fmtstore: %r");
	return d;
}

/* the last log write, and the last flush, in the recorded trace */
static void
lastlog(Dev *d, vlong lo, vlong hi, int *lastw, int *lastf)
{
	Simop *t;
	long n, i;

	*lastw = -1;
	*lastf = -1;
	n = simtrace(d, &t);
	for(i = 0; i < n; i++){
		if(t[i].op == Sopwrite && t[i].off >= lo && t[i].off < hi)
			*lastw = i;
		if(t[i].op == Sopflush)
			*lastf = i;
	}
}

/*
 * T1.11 at the region boundary, where a record needs more sectors
 * than the tail has left.  Three things are true there and none of
 * them is true by arithmetic:
 *
 * - the writer emits a one-sector Fwrap record and places the real
 *   record at the region start, rather than letting it straddle the
 *   end and run into the index region;
 * - that wrap record rides *before* the batch's post-flush, so one
 *   flush makes both durable — written after it, it is a link in the
 *   log's own continuity that a crash can drop, and replay would
 *   continue at +nsec into the index region and discard every commit
 *   since;
 * - replay honours Fwrap rather than continuing at +nsec, which here
 *   is a sector short of the region end and so is not the region
 *   start.
 *
 * The schedule parks the tail two sectors from the end with the
 * checkpoint mark just behind it, and then commits a four-sector
 * record.
 */
static void
twrapbig(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Super sb;
	Sbsel sel;
	Objinfo oi;
	uchar *small, *big, o[Oidmax];
	char name[32];
	uvlong ver[4], seq0, p;
	vlong lo, hi;
	int i, j, k, wraps, lastw, lastf;

	d = wrapdisk();
	if((s = openstore(d)) == nil){
		fail("the wrap geometry: storeopen: %r");
		devclose(d);
		return;
	}
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	sb = sel.sb[sel.start];
	lo = (vlong)sb.logoff*sb.secsz;
	hi = lo + (vlong)sb.logsecs*sb.secsz;
	checks++;
	if(maxrecbytes(&sb) <= 2*(uvlong)sb.secsz)
		fail("the wrap geometry's maximal record is %llud bytes, "
			"which is not more than two sectors",
			maxrecbytes(&sb));
	small = mkbuf(64, 127);
	big = mkbuf(64*Blk, 131);
	for(j = 0; j < 4; j++){
		snprint(name, sizeof name, "x%d", j);
		mk(s, name);
		ver[j] = 1;
	}

	/*
	 * Several laps with a mix of one-sector and four-sector records,
	 * so the tail meets the region end in every configuration.
	 */
	wraps = 0;
	for(i = 0; i < 120; i++){
		j = i % 4;
		snprint(name, sizeof name, "x%d", j);
		storestat(s, &st);
		seq0 = st.seqnext;
		simtracereset(d);
		ver[j]++;
		if(i % 5 == 4){
			if(wr(s, name, big, 64*Blk, 0, ver[j]) < 0)
				fail("big write %d: %r", i);
		}else if(wr(s, name, small, 64, 0, ver[j]) < 0)
			fail("small write %d: %r", i);
		storestat(s, &st);
		if(st.seqnext - seq0 == 2){
			wraps++;
			lastlog(d, lo, hi, &lastw, &lastf);
			checks++;
			if(lastw < 0 || lastf < 0 || lastw > lastf)
				fail("a log write follows the batch's "
					"post-flush: the wrap record is not "
					"covered by it");
		}
		if(i % 10 == 9 && storecheckpoint(s) < 0)
			fail("storecheckpoint: %r");
	}
	for(j = 0; j < 4; j++){
		snprint(name, sizeof name, "x%d", j);
		mustverify(s, name, "after several laps of the wrap geometry");
	}

	/*
	 * Park the tail two sectors from the region end.  A checkpoint
	 * with nothing in flight leaves cklogoff at the tail, so the
	 * distance is known; the mark then sits just behind the wrap,
	 * which is what puts the wrap in replay's path.
	 */
	for(k = 0; k < 20; k++){
		if(storecheckpoint(s) < 0)
			fail("storecheckpoint: %r");
		storestat(s, &st);
		p = st.cklogoff - sb.logoff;
		if(sb.logsecs - p <= 48)
			break;
		for(i = 0; i < 40; i++){
			ver[0]++;
			if(wr(s, "x0", small, 64, 0, ver[0]) < 0)
				fail("parking write: %r");
		}
	}
	checks++;
	if(sb.logsecs - p > 48 || p + 2 > sb.logsecs)
		fail("the tail could not be parked near the region end "
			"(%llud of %lud sectors)", p, sb.logsecs);
	while(p + 2 < sb.logsecs){
		ver[1]++;
		if(wr(s, "x1", small, 64, 0, ver[1]) < 0){
			fail("parking write: %r");
			break;
		}
		p++;
	}
	storestat(s, &st);
	seq0 = st.seqnext;
	simtracereset(d);
	ver[2]++;
	if(wr(s, "x2", big, 64*Blk, 0, ver[2]) < 0)
		fail("the record at the region boundary: %r");
	storestat(s, &st);
	checks++;
	if(st.seqnext - seq0 != 2)
		fail("a four-sector record two sectors from the region end "
			"did not emit a wrap record");
	else{
		wraps++;
		lastlog(d, lo, hi, &lastw, &lastf);
		checks++;
		if(lastw < 0 || lastf < 0 || lastw > lastf)
			fail("the wrap record was written after the batch's "
				"post-flush");
	}
	istrue("a wrap record was emitted and its placement read off the "
		"trace", wraps > 0);

	/* commits after the wrap, at the region start, all acked */
	for(i = 0; i < 3; i++){
		ver[3]++;
		if(wr(s, "x3", small, 64, 0, ver[3]) < 0)
			fail("write after the wrap: %r");
	}
	storeclose(s);

	if((s = openstore(d)) == nil){
		fail("after a record at the region boundary: storeopen: %r");
		devclose(d);
		free(small);
		free(big);
		return;
	}
	for(j = 0; j < 4; j++){
		snprint(name, sizeof name, "x%d", j);
		oidof(o, name);
		if(objstat(s, o, strlen(name), &oi) < 0)
			fail("objstat %s: %r", name);
		else
			eqv("every acked write across the wrap replays",
				oi.ver, ver[j]);
		mustverify(s, name, "after a record at the region boundary");
	}
	storeclose(s);
	devclose(d);
	free(small);
	free(big);
}

/*
 * T1.8, the durable watermark.  A batch's members are woken only when
 * that batch's post-flush has returned, every lower-numbered batch's
 * has, and the batch has been applied.  Without the ordering, a crash
 * after batch n+1 landed and batch n did not would leave replay
 * stopping at n and discarding n+1 — an acked write lost.
 */
typedef struct Writer Writer;
struct Writer
{
	Store	*s;
	char	name[16];
	uvlong	ver;
	int	nwrite;
	int	done;
	int	err;
	char	e[ERRMAX];
};

static void
writerproc(void *a)
{
	Writer *w;
	uchar *buf, o[Oidmax];
	int i;

	w = a;
	buf = mkbuf(Blk, 89);
	oidof(o, w->name);
	for(i = 0; i < w->nwrite; i++)
		if(objwrite(w->s, o, strlen(w->name), buf, Blk, 0,
			w->ver + i, 1, nil, 0) < 0){
			rerrstr(w->e, sizeof w->e);
			w->err = 1;
			break;
		}
	free(buf);
	w->done = 1;
}

/*
 * §6's wait, and the pending queue under it.  A commit with no log
 * space waits for the checkpointer with its item still on the pending
 * queue, so a committer may absorb that item at any moment — the
 * expected end of the wait.  The wait must therefore re-check the
 * item's state under the lock before it gives up, or it unlinks an
 * item that is by then a link in a batch's list: the entries after it
 * are in the durable record and are never applied and never woken,
 * and the item's own commit is answered `disk full' though it is on
 * the platter and published.  §13's fullwait point stages both
 * interleavings, so the schedule is the same on every run.
 */
static void
tabsorb(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Writer *w;
	Objinfo oi;
	uchar o[Oidmax];
	int i, k;

	d = newdisk();
	if((s = mustopen(d, "absorbed during the wait")) == nil)
		return;
	/*
	 * On the heap, not on this proc's stack: §7's T1 procs share the
	 * data and bss segments and not the stack, and every proc's
	 * stack is at the same virtual address, so a writer would set a
	 * done flag its parent cannot see.
	 */
	if((w = mallocz(2*sizeof *w, 1)) == nil)
		sysfatal("mallocz: %r");
	mk(s, "a0");
	mk(s, "a1");

	/* park one commit in the wait, its item still pending */
	for(i = 0; i < 2; i++){
		w[i].s = s;
		snprint(w[i].name, sizeof w[i].name, "a%d", i);
		w[i].ver = 30;
		w[i].nwrite = 1;
	}
	storehook(s, "fullwait", 1);
	if(spawnproc(writerproc, &w[0]) < 0)
		fail("spawn: %r");
	for(k = 0; k < 4000; k++){
		storestat(s, &st);
		if(st.logwait > 0)
			break;
		sleep(1);
	}
	istrue("a commit with no log space waits for the checkpointer",
		st.logwait > 0);

	/* a committer absorbs it, and the batch is durable and applied */
	if(spawnproc(writerproc, &w[1]) < 0)
		fail("spawn: %r");
	for(k = 0; k < 4000 && !w[1].done; k++)
		sleep(1);
	istrue("the committer took the waiting item with it", w[1].done);
	storehook(s, "fullwait", 0);
	for(k = 0; k < 4000 && !w[0].done; k++)
		sleep(1);
	istrue("the waiting writer returned", w[0].done);
	checks++;
	if(w[0].err)
		fail("a commit that became durable during the wait was "
			"answered `disk full'");

	/*
	 * And again with the batch still running when the wait elapses:
	 * the item is batched, so it waits for its batch rather than
	 * being taken out of the middle of it.
	 */
	memset(w, 0, 2*sizeof *w);
	for(i = 0; i < 2; i++){
		w[i].s = s;
		snprint(w[i].name, sizeof w[i].name, "a%d", i);
		w[i].ver = 31;
		w[i].nwrite = 1;
	}
	storestat(s, &st);
	storehook(s, "batch", st.seqnext);
	storehook(s, "fullwait", 1);
	if(spawnproc(writerproc, &w[0]) < 0)
		fail("spawn: %r");
	for(k = 0; k < 4000; k++){
		storestat(s, &st);
		if(st.logwait > 0)
			break;
		sleep(1);
	}
	if(spawnproc(writerproc, &w[1]) < 0)
		fail("spawn: %r");
	for(k = 0; k < 4000; k++){
		storestat(s, &st);
		if(st.seqnext > st.watermark + 1)
			break;
		sleep(1);
	}
	storehook(s, "fullwait", 0);
	sleep(100);
	storehook(s, "batch", 0);
	for(k = 0; k < 4000 && !(w[0].done && w[1].done); k++)
		sleep(1);
	istrue("the wait elapsing under a running batch severs nobody",
		w[0].done && w[1].done);
	checks++;
	if(w[0].err || w[1].err)
		fail("a commit in a running batch was answered `disk full'");
	for(i = 0; i < 2; i++){
		snprint(w[i].name, sizeof w[i].name, "a%d", i);
		oidof(o, w[i].name);
		if(objstat(s, o, strlen(w[i].name), &oi) < 0)
			fail("objstat %s: %r", w[i].name);
		else
			eqv("every entry in the batch was applied", oi.ver, 31);
		mustverify(s, w[i].name, "absorbed during the wait");
	}
	storeclose(s);
	devclose(d);
	free(w);
}

static void
tgroup(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Writer *w;
	Objinfo oi;
	uchar o[Oidmax];
	char name[16];
	int i, k, alldone;

	d = newdisk();
	if((s = openstoreck(d)) == nil){
		fail("group commit: storeopen: %r");
		devclose(d);
		return;
	}
	if((w = mallocz(8*sizeof *w, 1)) == nil)
		sysfatal("mallocz: %r");
	for(i = 0; i < 8; i++){
		snprint(name, sizeof name, "g%d", i);
		mk(s, name);
		w[i].s = s;
		strcpy(w[i].name, name);
		w[i].ver = 10;
		w[i].nwrite = 12;
	}
	for(i = 0; i < 8; i++)
		if(spawnproc(writerproc, &w[i]) < 0)
			fail("spawn: %r");
	for(k = 0; k < 20000; k++){
		alldone = 1;
		for(i = 0; i < 8; i++)
			if(!w[i].done)
				alldone = 0;
		if(alldone)
			break;
		sleep(1);
	}
	checks++;
	for(i = 0; i < 8; i++)
		if(!w[i].done || w[i].err){
			fail("writer %d did not finish cleanly", i);
			break;
		}
	storestat(s, &st);
	istrue("every batch was applied", st.watermark + 1 == st.seqnext);
	for(i = 0; i < 8; i++){
		snprint(name, sizeof name, "g%d", i);
		oidof(o, name);
		if(objstat(s, o, strlen(name), &oi) < 0)
			fail("objstat %s: %r", name);
		else
			eqv("every acked write is visible", oi.ver, 21);
		mustverify(s, name, "eight committers");
	}
	storeclose(s);

	if((s = openstoreck(d)) == nil){
		fail("eight committers, replayed: storeopen: %r");
		devclose(d);
		free(w);
		return;
	}
	for(i = 0; i < 8; i++){
		snprint(name, sizeof name, "g%d", i);
		oidof(o, name);
		if(objstat(s, o, strlen(name), &oi) < 0)
			fail("objstat %s: %r", name);
		else
			eqv("and survives the restart", oi.ver, 21);
		mustverify(s, name, "eight committers, replayed");
	}

	/*
	 * Now hold one batch and let the next one land.  The later
	 * batch's writer must not be woken: it was never acked, and
	 * after the restart its record is behind a gap in the sequence
	 * and is correctly discarded.
	 */
	storestat(s, &st);
	memset(w, 0, 2*sizeof *w);
	for(i = 0; i < 2; i++){
		w[i].s = s;
		snprint(w[i].name, sizeof w[i].name, "g%d", i);
		w[i].ver = 40 + i;
		w[i].nwrite = 1;
	}
	storehook(s, "batch", st.seqnext);
	if(spawnproc(writerproc, &w[0]) < 0)
		fail("spawn: %r");
	for(k = 0; k < 4000; k++){
		storestat(s, &st);
		if(st.seqnext > st.watermark + 1)
			break;
		sleep(1);
	}
	if(spawnproc(writerproc, &w[1]) < 0)
		fail("spawn: %r");
	sleep(200);
	checks++;
	if(w[1].done)
		fail("a batch was acked before every lower-numbered batch "
			"had been applied");
	storehook(s, "batch", 0);
	for(k = 0; k < 4000 && !(w[0].done && w[1].done); k++)
		sleep(1);
	istrue("both writers finished once the hold was released",
		w[0].done && w[1].done);
	storeclose(s);
	devclose(d);
	free(w);
	tabsorb();
}

/*
 * T1.8 and §2.7: a batch's record is bounded by the largest record
 * this geometry can hold and replay will accept, and by nothing else.
 * A batch cap of its own is a second bound that says nothing about
 * the first: at the geometry mk test formats, one blksz of record
 * body is four times the maximal record, so a batch of enough small
 * items makes a record that is written, flushed and acked — and then
 * refused by replay, which stops there and discards it and every
 * commit after it.
 *
 * The schedule forces the batch rather than hoping for it.  §13's
 * batch:n holds the first batch; logdepth-1 more start and park
 * waiting for it; every writer after those has already put its
 * entries on the pending queue before it sleeps for room.  Releasing
 * the hold wakes one of them, and it absorbs the whole queue.
 */
static void
tmaxbatch(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Writer *w;
	Objinfo oi;
	uchar o[Oidmax];
	char name[16];
	uvlong seq0;
	int i, k;

	enum { Nw = 20 };

	d = newdisk();
	if((s = openstore(d)) == nil){
		fail("a maximal group commit: storeopen: %r");
		devclose(d);
		return;
	}
	if((w = mallocz(Nw*sizeof *w, 1)) == nil)
		sysfatal("mallocz: %r");
	for(i = 0; i < Nw; i++){
		snprint(name, sizeof name, "b%d", i);
		mk(s, name);
		w[i].s = s;
		strcpy(w[i].name, name);
		w[i].ver = 50;
		w[i].nwrite = 1;
	}
	storestat(s, &st);
	seq0 = st.seqnext;
	storehook(s, "batch", st.seqnext);
	for(i = 0; i < Nw; i++)
		if(spawnproc(writerproc, &w[i]) < 0)
			fail("spawn: %r");
	sleep(300);
	storehook(s, "batch", 0);
	for(k = 0; k < 20000; k++){
		for(i = 0; i < Nw && w[i].done; i++)
			;
		if(i == Nw)
			break;
		sleep(1);
	}
	checks++;
	for(i = 0; i < Nw; i++)
		if(!w[i].done || w[i].err){
			fail("writer %d did not finish cleanly", i);
			break;
		}
	storestat(s, &st);
	checks++;
	if(st.seqnext - seq0 >= Nw)
		fail("the writers were never grouped: %llud batches for %d "
			"writers", st.seqnext - seq0, Nw);
	istrue("every batch was applied", st.watermark + 1 == st.seqnext);
	storeclose(s);

	if((s = openstore(d)) == nil){
		fail("a maximal group commit, replayed: storeopen: %r");
		devclose(d);
		free(w);
		return;
	}
	for(i = 0; i < Nw; i++){
		snprint(name, sizeof name, "b%d", i);
		oidof(o, name);
		if(objstat(s, o, strlen(name), &oi) < 0)
			fail("objstat %s: %r", name);
		else
			eqv("a maximal group commit replays", oi.ver, 50);
		mustverify(s, name, "a maximal group commit, replayed");
	}
	storeclose(s);
	devclose(d);
	free(w);
}

/*
 * T1.4 and §2.8: the checkpoint mark is bound to the *durable*
 * watermark and not to the next sequence number.  A checkpoint taken
 * while a batch is in flight is the case that tells them apart: the
 * batch has a seq and a log offset stamped and nothing on the platter,
 * so publishing seqnext-1 and the log tail names a record the log does
 * not carry — and then reclaims the space of the records before it.
 * The batch lands afterwards, below the published mark, and the next
 * start replays from above it, so an acked write is in neither the log
 * replay reads nor the pages the checkpoint wrote.
 *
 * In a single-proc run seqnext-1 is the watermark always, which is why
 * this needs §13's batch:n and a second proc.
 */
static void
tckptmark(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Writer *w;
	Objinfo oi;
	uchar *buf, o[Oidmax];
	int k;

	d = newdisk();
	if((s = openstore(d)) == nil){
		fail("the checkpoint mark: storeopen: %r");
		devclose(d);
		return;
	}
	buf = mkbuf(64, 113);
	mk(s, "k0");
	if(wr(s, "k0", buf, 64, 0, 2) < 0)
		fail("setup write: %r");
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");

	/*
	 * On the heap, not on this proc's stack: §7's procs are
	 * rfork(RFPROC|RFMEM) procs, which share the data and bss
	 * segments and not the stack — and every proc's stack is mapped
	 * at the same address, so a child writing through a pointer into
	 * its parent's stack writes its own and nothing faults.
	 */
	if((w = mallocz(sizeof *w, 1)) == nil)
		sysfatal("mallocz: %r");
	w->s = s;
	strcpy(w->name, "k0");
	w->ver = 60;
	w->nwrite = 1;
	storestat(s, &st);
	storehook(s, "batch", st.seqnext);
	if(spawnproc(writerproc, w) < 0)
		fail("spawn: %r");
	for(k = 0; k < 4000; k++){
		storestat(s, &st);
		if(st.seqnext > st.watermark + 1)
			break;
		sleep(1);
	}
	istrue("the batch is in flight", st.seqnext > st.watermark + 1);
	if(storecheckpoint(s) < 0)
		fail("a checkpoint under a held batch: %r");
	storestat(s, &st);
	checks++;
	if(st.ckseq > st.watermark)
		fail("the checkpoint published ckseq %llud above the durable "
			"watermark %llud", st.ckseq, st.watermark);

	storehook(s, "batch", 0);
	for(k = 0; k < 4000 && !w->done; k++)
		sleep(1);
	checks++;
	if(!w->done)
		fail("the held batch was never woken");
	else if(w->err)
		fail("the held batch failed: %s", w->e);
	storeclose(s);

	if((s = openstore(d)) == nil){
		fail("after a checkpoint under a held batch: storeopen: %r");
		devclose(d);
		free(buf);
		free(w);
		return;
	}
	oidof(o, "k0");
	if(objstat(s, o, 2, &oi) < 0)
		fail("objstat k0: %r");
	else
		eqv("the acked write below the published mark replays",
			oi.ver, 60);
	mustverify(s, "k0", "after a checkpoint under a held batch");
	storeclose(s);
	devclose(d);
	free(buf);
	free(w);
}

/*
 * T1.2 again, and this time nsec is a memory-safety bound before it
 * is anything else: replay sizes one buffer at maxrecbytes and reads
 * nsec*secsz bytes into it, so a header naming more sectors than the
 * geometry's maximal record MUST be refused before the read and not
 * after it.  A torn header can carry any nsec at all, and this one is
 * otherwise perfectly plausible — right magic, right version, the
 * expected successor for a sequence number, and a checksum that
 * verifies over the whole range it names.
 */
static void
tbignsec(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Super sb;
	Sbsel sel;
	Lrec r;
	Snap before, got;
	uchar *buf, *rec;
	uvlong rel, nsec;
	vlong off;

	d = newdisk();
	if((s = mustopen(d, "an oversized nsec")) == nil)
		return;
	buf = mkbuf(64, 73);
	mk(s, "n");
	if(wr(s, "n", buf, 64, 0, 2) < 0)
		fail("objwrite: %r");
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	snap(s, "n", &before);
	storestat(s, &st);
	storeclose(s);
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	sb = sel.sb[sel.start];
	rel = st.cklogoff - sb.logoff;
	nsec = sb.logsecs - rel;
	if(nsec > 64)
		nsec = 64;
	checks++;
	if(nsec*sb.secsz <= maxrecbytes(&sb)){
		fail("the log leaves no room for a record above maxrecbytes");
		free(buf);
		devclose(d);
		return;
	}
	if((rec = mallocz(nsec*sb.secsz, 1)) == nil)
		sysfatal("mallocz: %r");
	memset(&r, 0, sizeof r);
	r.vers = Storevers;
	r.nsec = nsec;
	r.seq = st.ckseq + 1;
	r.time = 0;
	r.nent = 0;
	r.flags = 0;
	lrecpack(rec, &r, sb.secsz);
	off = (vlong)st.cklogoff*sb.secsz;
	simpoke(d, off, rec, nsec*sb.secsz);
	free(rec);
	if((s = openstore(d)) == nil)
		fail("a record naming more sectors than the maximal record "
			"must not stop the store starting: %r");
	else{
		storestat(s, &st);
		eqv("and it is not replayed", st.nreplay, 0);
		snap(s, "n", &got);
		checks++;
		if(!sameas(&got, &before))
			fail("an oversized record changed the store");
		storeclose(s);
	}
	free(buf);
	devclose(d);
}

/*
 * T1.8's other half, and it needs no crash.  A batch whose record did
 * not become durable MUST NOT let a later batch ack: replay stops at
 * the first record that is not there, so an acked write above it is
 * lost.  A device error on one log write while §7's logdepth batches
 * are in flight is enough to reach it — §0 makes `interrupted' an
 * ordinary outcome of any device call, so this is a Tflush away — and
 * the store must condemn itself for commits rather than ack.
 */
static void
tfailed(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Writer *w;
	Objinfo oi;
	Sbsel sel;
	Super sb;
	uchar *buf, o[Oidmax];
	uvlong wm;
	vlong lo;
	int i, k;

	d = newdisk();
	if((s = mustopen(d, "a failed log write")) == nil)
		return;
	if((w = mallocz(2*sizeof *w, 1)) == nil)
		sysfatal("mallocz: %r");
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	sb = sel.sb[sel.start];
	lo = (vlong)sb.logoff*sb.secsz;
	buf = mkbuf(64, 103);
	mk(s, "b0");
	mk(s, "b1");
	if(wr(s, "b0", buf, 64, 0, 2) < 0 || wr(s, "b1", buf, 64, 0, 2) < 0)
		fail("setup write: %r");
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storestat(s, &st);
	wm = st.watermark;

	/* hold batch n, let batch n+1 land and park behind it */
	for(i = 0; i < 2; i++){
		w[i].s = s;
		snprint(w[i].name, sizeof w[i].name, "b%d", i);
		w[i].ver = 40;
		w[i].nwrite = 1;
	}
	storehook(s, "batch", st.seqnext);
	if(spawnproc(writerproc, &w[0]) < 0)
		fail("spawn: %r");
	for(k = 0; k < 4000; k++){
		storestat(s, &st);
		if(st.seqnext > st.watermark + 1)
			break;
		sleep(1);
	}
	if(spawnproc(writerproc, &w[1]) < 0)
		fail("spawn: %r");
	for(k = 0; k < 4000; k++){
		storestat(s, &st);
		if(st.seqnext > st.watermark + 2)
			break;
		sleep(1);
	}
	sleep(100);

	/* now batch n's own header write fails */
	simfaultat(d, Sfeio, 1, lo, (vlong)sb.logsecs*sb.secsz);
	storehook(s, "batch", 0);
	for(k = 0; k < 4000 && !(w[0].done && w[1].done); k++)
		sleep(1);
	simfault(d, Sfnone, 0);
	istrue("both writers returned", w[0].done && w[1].done);
	istrue("the batch whose log write failed fails", w[0].err != 0);
	istrue("and so does the batch above it, which replay would discard",
		w[1].err != 0);
	storestat(s, &st);
	istrue("the store commits no more", st.broken != 0);
	eqv("the watermark did not pass a batch that did not land",
		st.watermark, wm);
	checks++;
	if(wr(s, "b0", buf, 64, 0, 50) >= 0)
		fail("a condemned store accepted another commit");
	storeclose(s);

	if((s = mustopen(d, "after a failed log write")) == nil){
		devclose(d);
		free(buf);
		free(w);
		return;
	}
	for(i = 0; i < 2; i++){
		snprint(w[i].name, sizeof w[i].name, "b%d", i);
		oidof(o, w[i].name);
		if(objstat(s, o, strlen(w[i].name), &oi) < 0)
			fail("objstat %s: %r", w[i].name);
		else
			eqv("no write above the failed batch survived", oi.ver,
				2);
		mustverify(s, w[i].name, "after a failed log write");
	}
	storeclose(s);
	devclose(d);
	free(buf);
	free(w);
}

/*
 * §2.8: a checkpoint page whose write failed stays dirty.  Clearing
 * the mark and then failing is not an aborted checkpoint but a silent
 * one — the page is clean, so the next checkpoint skips it and
 * publishes a ckseq and a cklogoff past the records that dirtied it,
 * and reclaims their log space.  The committed state is then in
 * neither the log nor the region, with no crash anywhere.
 */
static void
tckptfail(void)
{
	Dev *d;
	Store *s;
	Objinfo oi;
	Sbsel sel;
	Super sb;
	uchar *buf, o[Oidmax];
	ulong slot;

	d = newdisk();
	if((s = mustopen(d, "a failed checkpoint page")) == nil)
		return;
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	sb = sel.sb[sel.start];
	buf = mkbuf(3*Blk, 107);
	mk(s, "c0");
	if(wr(s, "c0", buf, 64, 0, 2) < 0)
		fail("objwrite: %r");

	/* the index page holding slot 0 */
	simfaultat(d, Sfeio, 1, (vlong)sb.idxoff*sb.secsz, sb.blksz);
	checks++;
	if(storecheckpoint(s) >= 0)
		fail("a checkpoint whose page write failed reported success");
	simfault(d, Sfnone, 0);
	if(storecheckpoint(s) < 0)
		fail("the checkpoint after a failed one: %r");
	storeclose(s);
	if((s = mustopen(d, "after a failed index page")) == nil){
		devclose(d);
		free(buf);
		return;
	}
	oidof(o, "c0");
	checks++;
	if(objstat(s, o, 2, &oi) < 0)
		fail("an object whose index page write failed is gone: %r");
	else
		eqv("and it is the version that was acked", oi.ver, 2);

	/*
	 * The same for an extent-map entry, which is taken off the dirty
	 * list as well as unmarked, so a failed write-back has to put the
	 * entry back to have anything left to write.
	 */
	mk(s, "c1");
	if(wr(s, "c1", buf, 3*Blk, 0, 2) < 0)
		fail("objwrite: %r");
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	oidof(o, "c1");
	if(objstat(s, o, 2, &oi) < 0)
		fail("objstat c1: %r");
	slot = oi.emapslot;
	istrue("a three-block object has an extent-map slot", slot != 0);
	if(wr(s, "c1", buf, 3*Blk, 0, 3) < 0)
		fail("objwrite: %r");
	simfaultat(d, Sfeio, 1, (vlong)emapentoff(&sb, slot), sb.emapsz);
	checks++;
	if(storecheckpoint(s) >= 0)
		fail("a checkpoint whose extent-map write failed reported "
			"success");
	simfault(d, Sfnone, 0);
	if(storecheckpoint(s) < 0)
		fail("the checkpoint after a failed extent-map write: %r");
	storeclose(s);
	if((s = mustopen(d, "after a failed extent-map write")) == nil){
		devclose(d);
		free(buf);
		return;
	}
	mustverify(s, "c1", "after a failed extent-map write");
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * §0's error classes, used rather than merely told apart.
 * `interrupted' means reqqueueflush aborted the system call this proc
 * was in: it is not media damage, and §7 has a worker already inside
 * a commit complete that commit — a record half in the log is a
 * record whose successor can never be acked.  Treating it as an Eio
 * would let an ordinary client interrupt condemn the store.
 */
static void
tintr(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Objinfo oi;
	Sbsel sel;
	Super sb;
	Waitmsg *wm;
	uchar *buf, o[Oidmax];
	vlong lo, len;
	int fd;

	d = newdisk();
	if((s = mustopen(d, "an interrupted log write")) == nil)
		return;
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	sb = sel.sb[sel.start];
	lo = (vlong)sb.logoff*sb.secsz;
	len = (vlong)sb.logsecs*sb.secsz;
	buf = mkbuf(64, 109);
	mk(s, "i0");
	simfaultat(d, Sfintr, 1, lo, len);
	checks++;
	if(wr(s, "i0", buf, 64, 0, 2) < 0)
		fail("an interrupted log write was not completed: %r");
	simfault(d, Sfnone, 0);
	storestat(s, &st);
	istrue("an interrupted log write does not condemn the store",
		st.broken == 0);
	mustverify(s, "i0", "after an interrupted log write");
	storeclose(s);
	if((s = mustopen(d, "an interrupted log write, replayed")) == nil){
		devclose(d);
		free(buf);
		return;
	}
	oidof(o, "i0");
	if(objstat(s, o, 2, &oi) < 0)
		fail("objstat i0: %r");
	else
		eqv("and the write it carried is durable", oi.ver, 2);

	/*
	 * Echange is the class the store cannot carry on from: the
	 * unit's partitions were re-declared under the fid, so every
	 * offset it holds may now name something else.  "MUST NOT keep
	 * serving on a stale fid" means the proc does not come back,
	 * which is what a child proc is for.
	 */
	simfaultat(d, Sfechange, 1, lo, len);
	/*
	 * RFFDG as well as RFMEM: the child gets its own file
	 * descriptors, so quietening the exit line does not quieten this
	 * program's own.
	 */
	switch(rfork(RFPROC|RFMEM|RFFDG)){
	case -1:
		fail("rfork: %r");
		break;
	case 0:
		if((fd = open("/dev/null", OWRITE)) >= 0){
			dup(fd, 2);	/* the exit line is the point, not noise */
			close(fd);
		}
		wr(s, "i0", buf, 64, 0, 3);
		exits("served on");
	default:
		checks++;
		if((wm = wait()) == nil)
			fail("wait: %r");
		else{
			if(wm->msg[0] == '\0')
				fail("a device that reported Echange was served "
					"on");
			free(wm);
		}
	}
	simfault(d, Sfnone, 0);
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * §13's named points mean what they say.  body:n is "after n body
 * sectors", so the common one-sector commit — whose body is empty —
 * has none: a schedule arming body:1 must not be crashed after the
 * commit point instead of before it.  precommit and commit are two
 * points and not one, and the pre-flush is what lies between them.
 */
static void
tpoints(void)
{
	Dev *d;
	Store *s;
	Simop *t;
	Sbsel sel;
	Super sb;
	uchar *buf;
	vlong lo, hi;
	long n, i, lw, lf;

	d = newdisk();
	if((s = mustopen(d, "the named points")) == nil)
		return;
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	sb = sel.sb[sel.start];
	lo = (vlong)sb.logoff*sb.secsz;
	hi = lo + (vlong)sb.logsecs*sb.secsz;
	buf = mkbuf(16*Blk, 113);
	mk(s, "p0");

	simcrashdead(d, 1);
	simcrashmode(d, Scdrop);
	simarm(d, "body", 1);
	checks++;
	if(wr(s, "p0", buf, 64, 0, 2) < 0)
		fail("body:1 fired on a record with no body sectors: %r");
	simarm(d, nil, 0);

	/*
	 * The pre-flush follows the precommit point, so at that point
	 * none has been issued for this record.  It takes a record with
	 * body sectors to say so: at this geometry that is a rewrite of a
	 * whole maximal object, whose entry names sixteen blocks and
	 * frees sixteen grains.
	 */
	if(wr(s, "p0", buf, 16*Blk, 0, 3) < 0)
		fail("objwrite: %r");
	simtracereset(d);
	simarm(d, "precommit", 0);
	wr(s, "p0", buf, 16*Blk, 0, 4);
	simarm(d, nil, 0);
	n = simtrace(d, &t);
	lw = -1;
	lf = -1;
	for(i = 0; i < n; i++){
		if(t[i].n < 0)
			continue;
		if(t[i].op == Sopwrite && t[i].off >= lo && t[i].off < hi)
			lw = i;
		else if(t[i].op == Sopflush)
			lf = i;
	}
	checks++;
	if(lw < 0 || lf > lw)
		fail("the pre-flush was issued before §13's precommit point");
	simrevive(d);
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * §6's reserved tail is for commits that free space: an Eobj that
 * releases grains without allocating any, and an Eslot.  An Edirty
 * frees no log space in either direction, so neither an add nor a
 * remove may draw on the reserve — otherwise the reserve is spent by
 * exactly the traffic it exists to exclude, and the delete that would
 * relieve the exhaustion is the commit that cannot be written.
 */
static void
tresv(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Sbsel sel;
	Super sb;
	uchar *buf, o[Oidmax];
	uvlong resv;
	int i;

	d = newdisk();
	if((s = mustopen(d, "the reserved tail")) == nil)
		return;
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	sb = sel.sb[sel.start];
	resv = sb.logsecs/Logresvdiv;
	if(resv < 1)
		resv = 1;
	buf = mkbuf(64, 127);
	mk(s, "r0");
	oidof(o, "r0");
	if(dirtyadd(s, o, 2, "peer.0", 7) < 0)
		fail("dirtyadd: %r");

	/* fill the log down to the reserve */
	for(i = 0; i < 400; i++){
		storestat(s, &st);
		if(st.logfree <= resv)
			break;
		if(wr(s, "r0", buf, 64, 0, 3 + i) < 0)
			break;
	}
	storestat(s, &st);
	istrue("the log is down to its reserved tail", st.logfree <= resv);

	checks++;
	if(dirtydel(s, o, 2, "peer.0") >= 0)
		fail("an Edirty drew on §6's reserved tail");
	checks++;
	if(objremove(s, o, 2, 900, 1) < 0)
		fail("delete does not always work on the reserved tail: %r");
	storeclose(s);
	devclose(d);
	free(buf);
}

/*
 * T1.10.  Log space before the newly published cklogoff is reclaimed
 * only after the superblock write has returned.  Reclaiming earlier
 * lets a crash leave a superblock naming an older checkpoint whose
 * log has already been overwritten, which is the one way this format
 * can lose data.  §13 puts the mutation in the store as a hook, so
 * both halves of the argument are run here.
 */
static void
treclaim(int early)
{
	Dev *d;
	Store *s;
	Sbsel sel;
	uchar *buf;
	vlong sboff;
	int i, n;
	char *what;

	what = early ? "reclaim before the publish" : "reclaim after it";
	d = newdisk();
	if((s = mustopen(d, what)) == nil)
		return;
	buf = mkbuf(64, 97);
	mk(s, "r");
	if(wr(s, "r", buf, 64, 0, 2) < 0)
		fail("%s: objwrite: %r", what);
	if(storecheckpoint(s) < 0)
		fail("%s: storecheckpoint: %r", what);
	/*
	 * Commits between the two checkpoints, so that the two marks
	 * are further apart than §6's reserved log tail: the whole
	 * difference the rule makes is the space between them, and the
	 * reserve would otherwise absorb it.
	 */
	for(i = 0; i < 16; i++)
		if(wr(s, "r", buf, 64, 64, 3 + i) < 0)
			fail("%s: objwrite: %r", what);

	/*
	 * A checkpoint whose superblock write fails: the pages and the
	 * flush are done, so the mark is earned, but the disk still
	 * names the previous checkpoint — and step 3 never returned, so
	 * the correct rule reclaims nothing.
	 */
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	sboff = sel.victim == 0 ? 0 : super1off(d);
	storehook(s, "reclaim", early);
	/* two: the publisher reads both copies before it writes one */
	simfaultat(d, Sfeio, 2, sboff, sel.sb[sel.start].secsz);
	if(storecheckpoint(s) >= 0)
		fail("%s: a checkpoint whose superblock write failed "
			"reported success", what);
	storehook(s, "reclaim", 0);
	simfault(d, Sfnone, 0);

	/* fill the log: with the correct rule there is nothing to reuse */
	n = 0;
	for(i = 0; i < 400; i++)
		if(wr(s, "r", buf, 64, 0, 100 + i) < 0)
			break;
		else
			n++;
	storeclose(s);

	s = openstore(d);
	checks++;
	if(early){
		if(s != nil){
			Objinfo oi;

			/*
			 * If it starts at all, the older superblock's log
			 * must still describe what the disk holds — which
			 * it cannot, because the commits above overwrote
			 * it.  Either the store refuses or an acked write
			 * is gone.
			 */
			if(objstat(s, (uchar*)"r", 1, &oi) == 0
			&& oi.ver == (uvlong)(100 + n - 1))
				fail("reclaiming before the publish lost "
					"nothing, so the rule is untested");
			storeclose(s);
		}
	}else{
		if(s == nil)
			fail("%s: the store must start: %r", what);
		else{
			Objinfo oi;

			if(objstat(s, (uchar*)"r", 1, &oi) < 0)
				fail("%s: objstat: %r", what);
			else
				eqv("every acked write survived", oi.ver,
					100 + n - 1);
			mustverify(s, "r", what);
			storeclose(s);
		}
	}
	devclose(d);
	free(buf);
}

/*
 * T1.22.  The new ckseq/cklogoff become publishable only after the
 * checkpoint's flush has returned, so a publish triggered by
 * something else mid-checkpoint carries the *old* mark.  Otherwise an
 * epochhigh publish landing between §2.8's step 1 and step 2 writes
 * the new ckseq with the pages it describes still in a volatile cache
 * — and the reclaim rule then licenses overwriting the log records
 * that would have rebuilt them.
 */
static void
tckmark(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Sbsel sel;
	uchar *buf;
	vlong sboff;
	uvlong ck0;
	int i;

	d = newdisk();
	if((s = mustopen(d, "checkpoint mark")) == nil)
		return;
	buf = mkbuf(64, 101);
	mk(s, "p");
	if(wr(s, "p", buf, 64, 0, 2) < 0)
		fail("objwrite: %r");
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storestat(s, &st);
	ck0 = st.ckseq;
	if(wr(s, "p", buf, 64, 64, 3) < 0)
		fail("objwrite: %r");

	/*
	 * Force an epochhigh publish after the first checkpoint page
	 * write, and drop the checkpoint's own superblock write, so the
	 * mid-checkpoint publish is the newest superblock on the disk.
	 */
	if(superselect(d, &sel) < 0)
		sysfatal("superselect: %r");
	storehook(s, "publish", 1);
	sboff = sel.victim == 0 ? super1off(d) : 0;
	simfaultat(d, Sfdrop, 1, sboff, sel.sb[sel.start].secsz);
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storehook(s, "publish", 0);
	storeclose(s);

	if(superselect(d, &sel) < 0){
		fail("superselect: %r");
		devclose(d);
		free(buf);
		return;
	}
	eqv("a publish mid-checkpoint carries the old checkpoint mark",
		sel.sb[sel.start].ckseq, ck0);
	istrue("and its own field advanced", sel.sb[sel.start].epochhigh > 0);

	if((s = mustopen(d, "after a mid-checkpoint publish")) == nil){
		devclose(d);
		free(buf);
		return;
	}
	storestat(s, &st);
	istrue("replay still covers the pages that publish described",
		st.pmax <= (st.nreplay > 0 ? st.watermark : st.ckseq));
	mustverify(s, "p", "after a mid-checkpoint publish");
	storeclose(s);
	devclose(d);
	free(buf);
	USED(i);
}

/*
 * T1.23.  The qidnext high-water is durable before any path in its
 * batch is issued, and epochhigh before the instance acts under it.
 * No path is ever re-issued, across any number of restarts.
 */
static void
tqid(void)
{
	Dev *d;
	Store *s;
	Storestat st;
	Objinfo oi;
	uchar o[Oidmax];
	uvlong seen[64];
	char name[32];
	int i, j, n;

	d = newdisk();
	n = 0;
	for(j = 0; j < 3; j++){
		if((s = mustopen(d, "qid.path")) == nil){
			devclose(d);
			return;
		}
		if(j == 0 && epochadopt(s, 11) < 0)
			fail("epochadopt: %r");
		for(i = 0; i < 8; i++){
			snprint(name, sizeof name, "q%d.%d", j, i);
			oidof(o, name);
			if(objcreate(s, o, strlen(name), 1, 1, &oi) < 0){
				fail("objcreate: %r");
				continue;
			}
			seen[n++] = oi.qidpath;
		}
		storestat(s, &st);
		istrue("the recorded high-water is above every path issued",
			st.qidnext > seen[n-1]);
		eqv("epochhigh survives every restart", st.epochhigh, 11);
		storeclose(s);
	}
	checks++;
	for(i = 0; i < n; i++)
		for(j = i+1; j < n; j++)
			if(seen[i] == seen[j]){
				fail("qid.path %llud was issued twice",
					seen[i]);
				i = n;
				break;
			}
	devclose(d);
}

void
main(int argc, char **argv)
{
	USED(argc); USED(argv);
	tflushseq();
	ttorn();
	tshort();
	tmatrix();
	twrap();
	twrapbig();
	tgroup();
	tmaxbatch();
	tbignsec();
	tckptmark();
	tfailed();
	tckptfail();
	tintr();
	tpoints();
	tresv();
	treclaim(0);
	treclaim(1);
	tckmark();
	tqid();
	if(fails > 0){
		fprint(2, "committest: %d of %d checks failed\n", fails, checks);
		exits("failed");
	}
	print("committest: %d checks ok\n", checks);
	exits(nil);
}
