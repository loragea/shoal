#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"

/*
 * The simulated disk, docs/design/store.md §13.
 *
 * The mechanism this design most depends on — the placement of the two
 * flushes — cannot be discriminated on the reference hardware at all,
 * so it is discriminated here instead.  The sim models what a real
 * device is allowed to do:
 *
 *   - a volatile write cache.  A written sector is visible to reads
 *     at once and is durable only after a flush; a crash leaves each
 *     sector written since the last one holding either its durable
 *     bytes or the cached ones, and which of the two is the crash
 *     policy simcrashmode and simcrashkeep set.
 *   - torn and partial writes.  A write may land in any subset of the
 *     sectors it covers (Sftearsec) and any sector it lands may hold
 *     a byte-wise mixture of old and new bytes (Sftearbyte) — the
 *     weakest assumption §3.2 gives the device.
 *   - short counts on every read and write (Sfshort).
 *   - Eio, Echange and `interrupted' on demand, on a read, a write or
 *     a flush.
 *   - a crash at a named point (simarm), with every read, write,
 *     flush and crash recorded in issue order so a test can assert
 *     the *sequence* and not only the outcome.
 *
 * Several faults are armed at once and each may be aimed at a byte
 * range (simfaultat), because §13 wants a short count and a tear in
 * one schedule and §8 wants an Eio on one named sector.
 *
 * Everything random here comes from one seeded generator, so a run is
 * reproducible from its seed.  §7 puts many procs on one Dev, so
 * every entry point takes one QLock: the sim makes no system call, so
 * holding it across a whole operation costs nothing and is what keeps
 * the trace and the generator deterministic under concurrency.
 */

enum
{
	Nfault	= 8,		/* faults armed at once */

	/* the operation classes a fault can apply to */
	Oread	= 1<<0,
	Owrite	= 1<<1,
	Oflush	= 1<<2,
};

typedef struct Fault Fault;
struct Fault
{
	int	kind;
	int	n;		/* operations left; <= 0 is sticky */
	int	all;		/* aimed at the whole disk */
	uvlong	lo, hi;		/* else the sectors [lo, hi) it is aimed at */
};

typedef struct Sim Sim;
struct Sim
{
	QLock	lk;

	ulong	secsz;
	uvlong	nsec;
	uchar	*stable;	/* the durable image */
	uchar	*live;		/* what a read sees: durable plus the cache */
	uchar	*dirty;		/* per sector: written since the last flush */
	uvlong	ndirty;

	ulong	rand;

	Fault	fault[Nfault];

	int	crashmode;
	int	dieoncrash;	/* a crash stops the device until simrevive */
	int	dead;
	uchar	*keep;		/* per sector, under Scnamed: survives */

	char	point[64];	/* armed crash point, empty if none */
	int	pointn;

	Simop	*trace;
	long	ntrace;
	long	atrace;
};

static ulong
simrand(Sim *s)
{
	s->rand = s->rand*1103515245 + 12345;
	return (s->rand >> 8) & 0xffffff;
}

/*
 * The trace grows without bound and a test program is what runs out
 * of memory if it does; there is nothing this library could return
 * a failure to.
 */
static void
record(Sim *s, int op, vlong off, long n)
{
	Simop *t;

	if(s->ntrace >= s->atrace){
		s->atrace = s->atrace ? 2*s->atrace : 64;
		if((t = realloc(s->trace, s->atrace*sizeof *t)) == nil)
			sysfatal("simdisk: trace: %r");
		s->trace = t;
	}
	t = &s->trace[s->ntrace++];
	t->op = op;
	t->off = off;
	t->n = n;
}

/* which operations a fault of this kind can be taken by */
static int
faultops(int kind)
{
	switch(kind){
	case Sfshort:
		return Oread|Owrite;
	case Sftearsec:
	case Sftearbyte:
	case Sfdrop:
		return Owrite;
	case Sfeio:
	case Sfechange:
	case Sfintr:
		return Oread|Owrite|Oflush;
	}
	return 0;
}

/*
 * The first armed fault this operation can take, consumed if it is
 * not sticky.  A fault aimed at a range is taken by an operation that
 * overlaps it; a flush has no range of its own, so only a fault aimed
 * at the whole disk applies to one.
 */
static int
takefault(Sim *s, int op, vlong off, long n)
{
	Fault *f;
	uvlong lo, hi;
	int i, kind;

	lo = off/s->secsz;
	hi = (off + n + s->secsz - 1)/s->secsz;
	for(i = 0; i < Nfault; i++){
		f = &s->fault[i];
		if(f->kind == Sfnone || (faultops(f->kind) & op) == 0)
			continue;
		if(!f->all && (op == Oflush || hi <= f->lo || lo >= f->hi))
			continue;
		kind = f->kind;
		if(f->n > 0 && --f->n == 0)
			f->kind = Sfnone;
		return kind;
	}
	return Sfnone;
}

static void
land(Sim *s, uvlong sec, uchar *src, int mix)
{
	uchar *dst;
	ulong i;

	dst = s->live + sec*s->secsz;
	if(mix){
		for(i = 0; i < s->secsz; i++)
			if(simrand(s) & 1)
				dst[i] = src[i];
	}else
		memmove(dst, src, s->secsz);
	if(!s->dirty[sec]){
		s->dirty[sec] = 1;
		s->ndirty++;
	}
}

static long
simrd(Dev *d, void *a, long n, vlong off)
{
	Sim *s;

	s = d->aux;
	qlock(&s->lk);
	if(s->dead){
		record(s, Sopread, off, -1);
		qunlock(&s->lk);
		werrstr("i/o error");
		return -1;
	}
	switch(takefault(s, Oread, off, n)){
	case Sfeio:
		record(s, Sopread, off, -1);
		qunlock(&s->lk);
		werrstr("i/o error");
		return -1;
	case Sfechange:
		record(s, Sopread, off, -1);
		qunlock(&s->lk);
		werrstr("media or partition has changed");
		return -1;
	case Sfintr:
		record(s, Sopread, off, -1);
		qunlock(&s->lk);
		werrstr("interrupted");
		return -1;
	case Sfshort:
		if(n > 1)
			n = 1 + simrand(s) % n;
		break;
	}
	record(s, Sopread, off, n);
	memmove(a, s->live + off, n);
	qunlock(&s->lk);
	return n;
}

static long
simwr(Dev *d, void *a, long n, vlong off)
{
	Sim *s;
	uchar *p;
	uvlong sec;
	long i, nsec;
	int f;

	s = d->aux;
	qlock(&s->lk);
	if(s->dead){
		record(s, Sopwrite, off, -1);
		qunlock(&s->lk);
		werrstr("i/o error");
		return -1;
	}
	f = takefault(s, Owrite, off, n);
	switch(f){
	case Sfeio:
		record(s, Sopwrite, off, -1);
		qunlock(&s->lk);
		werrstr("i/o error");
		return -1;
	case Sfechange:
		record(s, Sopwrite, off, -1);
		qunlock(&s->lk);
		werrstr("media or partition has changed");
		return -1;
	case Sfintr:
		record(s, Sopwrite, off, -1);
		qunlock(&s->lk);
		werrstr("interrupted");
		return -1;
	case Sfshort:
		nsec = n / s->secsz;
		if(nsec > 1)
			n = (1 + simrand(s) % nsec) * s->secsz;
		break;
	}
	record(s, Sopwrite, off, n);
	if(f == Sfdrop){
		qunlock(&s->lk);
		return n;
	}
	nsec = n / s->secsz;
	sec = off / s->secsz;
	p = a;
	for(i = 0; i < nsec; i++){
		if(f == Sftearsec && (simrand(s) & 1))
			continue;
		land(s, sec + i, p + i*s->secsz, f == Sftearbyte);
	}
	qunlock(&s->lk);
	return n;
}

static int
simflush(Dev *d)
{
	Sim *s;
	uvlong i;
	char *e;

	s = d->aux;
	qlock(&s->lk);
	if(s->dead){
		record(s, Sopflush, 0, -1);
		qunlock(&s->lk);
		werrstr("i/o error");
		return -1;
	}
	e = nil;
	switch(takefault(s, Oflush, 0, 0)){
	case Sfeio:
		e = "i/o error";
		break;
	case Sfechange:
		e = "media or partition has changed";
		break;
	case Sfintr:
		e = "interrupted";
		break;
	}
	if(e != nil){
		/* a failed flush makes nothing durable */
		record(s, Sopflush, 0, -1);
		qunlock(&s->lk);
		werrstr("%s", e);
		return -1;
	}
	record(s, Sopflush, 0, 0);
	if(s->ndirty > 0){
		for(i = 0; i < s->nsec; i++)
			if(s->dirty[i]){
				memmove(s->stable + i*s->secsz,
					s->live + i*s->secsz, s->secsz);
				s->dirty[i] = 0;
			}
		s->ndirty = 0;
	}
	qunlock(&s->lk);
	return 0;
}

/*
 * Every sector written since the last flush holds either its durable
 * bytes or its cached ones, sector by sector: a crash may leave a
 * torn write torn, and may leave a commit header on the platter with
 * the grain it describes still in the cache (§3.2).  The crash policy
 * says which, and is reset afterwards because a crash is one event.
 * Caller holds the lock.
 */
static void
crash(Sim *s)
{
	uvlong i;
	int keep;

	record(s, Sopcrash, 0, 0);
	for(i = 0; i < s->nsec; i++){
		if(!s->dirty[i])
			continue;
		switch(s->crashmode){
		case Sckeep:
			keep = 1;
			break;
		case Scsome:
			keep = simrand(s) & 1;
			break;
		case Scnamed:
			keep = s->keep != nil && s->keep[i];
			break;
		default:
			keep = 0;
			break;
		}
		if(keep)
			memmove(s->stable + i*s->secsz, s->live + i*s->secsz,
				s->secsz);
		else
			memmove(s->live + i*s->secsz, s->stable + i*s->secsz,
				s->secsz);
		s->dirty[i] = 0;
	}
	s->ndirty = 0;
	memset(s->fault, 0, sizeof s->fault);
	s->crashmode = Scdrop;
	if(s->dieoncrash)
		s->dead = 1;
	if(s->keep != nil)
		memset(s->keep, 0, s->nsec);
}

static void
simpointf(Dev *d, char *name, int n)
{
	Sim *s;

	s = d->aux;
	qlock(&s->lk);
	if(s->point[0] != '\0' && strcmp(name, s->point) == 0 && n == s->pointn){
		s->point[0] = '\0';
		crash(s);
	}
	qunlock(&s->lk);
}

static void
simclose(Dev *d)
{
	Sim *s;

	s = d->aux;
	free(s->stable);
	free(s->live);
	free(s->dirty);
	free(s->keep);
	free(s->trace);
	free(s);
}

static Devops simops =
{
	simrd,
	simwr,
	simflush,
	simpointf,
	simclose,
};

Dev*
simopen(ulong secsz, uvlong nsec, ulong seed)
{
	Dev *d;
	Sim *s;

	if(secsz == 0 || (secsz & (secsz - 1)) != 0 || nsec == 0){
		werrstr("simopen: bad geometry %lud x %llud", secsz, nsec);
		return nil;
	}
	if((s = mallocz(sizeof *s, 1)) == nil)
		return nil;
	s->secsz = secsz;
	s->nsec = nsec;
	s->rand = seed;
	s->stable = mallocz(secsz*nsec, 1);
	s->live = mallocz(secsz*nsec, 1);
	s->dirty = mallocz(nsec, 1);
	if(s->stable == nil || s->live == nil || s->dirty == nil){
		free(s->stable);
		free(s->live);
		free(s->dirty);
		free(s);
		werrstr("simopen: out of memory");
		return nil;
	}
	if((d = mallocz(sizeof *d, 1)) == nil){
		free(s->stable);
		free(s->live);
		free(s->dirty);
		free(s);
		return nil;
	}
	d->ops = &simops;
	d->name = strdup("simdisk");
	d->secsz = secsz;
	d->size = (vlong)secsz * nsec;
	d->wunit = Blkszstore;
	d->flushmode = Fraw;
	d->aux = s;
	return d;
}

/* caller holds the lock */
static Fault*
armfault(Sim *s, int kind, int n)
{
	int i;

	if(kind == Sfnone){
		memset(s->fault, 0, sizeof s->fault);
		return nil;
	}
	for(i = 0; i < Nfault; i++)
		if(s->fault[i].kind == Sfnone){
			s->fault[i].kind = kind;
			s->fault[i].n = n;
			return &s->fault[i];
		}
	sysfatal("simfault: more than %d faults armed", Nfault);
}

/* arm a fault over the whole disk; Sfnone disarms every armed fault */
void
simfault(Dev *d, int kind, int n)
{
	Sim *s;
	Fault *f;

	s = d->aux;
	qlock(&s->lk);
	if((f = armfault(s, kind, n)) != nil)
		f->all = 1;
	qunlock(&s->lk);
}

/*
 * Arm a fault taken only by an operation overlapping [off, off+n).  A
 * flush has no range, so it never takes one of these.
 */
void
simfaultat(Dev *d, int kind, int n, vlong off, vlong len)
{
	Sim *s;
	Fault *f;

	s = d->aux;
	qlock(&s->lk);
	if((f = armfault(s, kind, n)) != nil){
		f->all = 0;
		f->lo = off/s->secsz;
		f->hi = (off + len + s->secsz - 1)/s->secsz;
	}
	qunlock(&s->lk);
}

void
simcrash(Dev *d)
{
	Sim *s;

	s = d->aux;
	qlock(&s->lk);
	crash(s);
	qunlock(&s->lk);
}

/* what the next crash does with the sectors written since the last flush */
void
simcrashmode(Dev *d, int mode)
{
	Sim *s;

	s = d->aux;
	qlock(&s->lk);
	s->crashmode = mode;
	if(s->keep != nil)
		memset(s->keep, 0, s->nsec);
	qunlock(&s->lk);
}

/*
 * Name sectors that survive the next crash, which puts the crash in
 * Scnamed mode.  Everything not named reverts to its durable bytes.
 */
void
simcrashkeep(Dev *d, vlong off, vlong len)
{
	Sim *s;
	uvlong i, hi;

	s = d->aux;
	qlock(&s->lk);
	if(s->keep == nil && (s->keep = mallocz(s->nsec, 1)) == nil)
		sysfatal("simcrashkeep: %r");
	s->crashmode = Scnamed;
	hi = (off + len + s->secsz - 1)/s->secsz;
	if(hi > s->nsec)
		hi = s->nsec;
	for(i = off/s->secsz; i < hi; i++)
		s->keep[i] = 1;
	qunlock(&s->lk);
}

/*
 * A crash is the end of a run.  Where a test needs that — every §13
 * schedule inside the commit path does, because the writes after the
 * crash point would otherwise still land — simcrashdead makes the
 * crash stop the device: every read, write and flush then fails until
 * simrevive brings the machine back.  It is opt-in because the
 * schedules that examine what a *partly* completed sequence left
 * behind, like §2.2's two superblock writes, need the run to carry
 * on.
 */
void
simcrashdead(Dev *d, int on)
{
	Sim *s;

	s = d->aux;
	qlock(&s->lk);
	s->dieoncrash = on;
	if(!on)
		s->dead = 0;
	qunlock(&s->lk);
}

void
simrevive(Dev *d)
{
	Sim *s;

	s = d->aux;
	qlock(&s->lk);
	s->dead = 0;
	s->dieoncrash = 0;
	qunlock(&s->lk);
}

void
simarm(Dev *d, char *point, int n)
{
	Sim *s;

	s = d->aux;
	qlock(&s->lk);
	if(point == nil)
		s->point[0] = '\0';
	else{
		strecpy(s->point, s->point + sizeof s->point, point);
		s->pointn = n;
	}
	qunlock(&s->lk);
}

/* poke bytes into durable storage, which is how a media fault is staged */
void
simpoke(Dev *d, vlong off, void *buf, long n)
{
	Sim *s;

	s = d->aux;
	if(off < 0 || off + n > d->size)
		sysfatal("simpoke: %lld+%ld out of range", off, n);
	qlock(&s->lk);
	memmove(s->stable + off, buf, n);
	memmove(s->live + off, buf, n);
	qunlock(&s->lk);
}

void
simpeek(Dev *d, vlong off, void *buf, long n)
{
	Sim *s;

	s = d->aux;
	if(off < 0 || off + n > d->size)
		sysfatal("simpeek: %lld+%ld out of range", off, n);
	qlock(&s->lk);
	memmove(buf, s->stable + off, n);
	qunlock(&s->lk);
}

uvlong
simdirty(Dev *d)
{
	Sim *s;
	uvlong n;

	s = d->aux;
	qlock(&s->lk);
	n = s->ndirty;
	qunlock(&s->lk);
	return n;
}

/*
 * The trace array is reallocated as it grows, so the pointer this
 * hands back is good only until the next device operation and only
 * while no other proc is using the device.
 */
long
simtrace(Dev *d, Simop **t)
{
	Sim *s;
	long n;

	s = d->aux;
	qlock(&s->lk);
	*t = s->trace;
	n = s->ntrace;
	qunlock(&s->lk);
	return n;
}

void
simtracereset(Dev *d)
{
	Sim *s;

	s = d->aux;
	qlock(&s->lk);
	s->ntrace = 0;
	qunlock(&s->lk);
}
