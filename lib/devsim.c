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
 *     at once and is durable only after a flush; simcrash discards
 *     every sector written since the last one.
 *   - torn and partial writes.  A write may land in any subset of the
 *     sectors it covers (Sftearsec) and any sector it lands may hold
 *     a byte-wise mixture of old and new bytes (Sftearbyte) — the
 *     weakest assumption §3.2 gives the device.
 *   - short counts on every read and write (Sfshort).
 *   - Eio, Echange and `interrupted' on demand.
 *   - a crash at a named point (simarm), with every read, write,
 *     flush and crash recorded in issue order so a test can assert
 *     the *sequence* and not only the outcome.
 *
 * Everything random here comes from one seeded generator, so a run is
 * reproducible from its seed.
 */

typedef struct Sim Sim;
struct Sim
{
	ulong	secsz;
	uvlong	nsec;
	uchar	*stable;	/* the durable image */
	uchar	*live;		/* what a read sees: durable plus the cache */
	uchar	*dirty;		/* per sector: written since the last flush */
	uvlong	ndirty;

	ulong	rand;

	int	fault;
	int	faultn;		/* operations left; <= 0 is sticky */

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

/* the armed fault, consumed if it applies to this kind of operation */
static int
takefault(Sim *s, int iswrite)
{
	int f;

	f = s->fault;
	if(f == Sfnone)
		return Sfnone;
	if(!iswrite && (f == Sftearsec || f == Sftearbyte || f == Sfdrop))
		return Sfnone;
	if(s->faultn > 0 && --s->faultn == 0)
		s->fault = Sfnone;
	return f;
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
	switch(takefault(s, 0)){
	case Sfeio:
		record(s, Sopread, off, -1);
		werrstr("i/o error");
		return -1;
	case Sfechange:
		record(s, Sopread, off, -1);
		werrstr("media or partition has changed");
		return -1;
	case Sfintr:
		record(s, Sopread, off, -1);
		werrstr("interrupted");
		return -1;
	case Sfshort:
		if(n > 1)
			n = 1 + simrand(s) % n;
		break;
	}
	record(s, Sopread, off, n);
	memmove(a, s->live + off, n);
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
	f = takefault(s, 1);
	switch(f){
	case Sfeio:
		record(s, Sopwrite, off, -1);
		werrstr("i/o error");
		return -1;
	case Sfechange:
		record(s, Sopwrite, off, -1);
		werrstr("media or partition has changed");
		return -1;
	case Sfintr:
		record(s, Sopwrite, off, -1);
		werrstr("interrupted");
		return -1;
	case Sfshort:
		nsec = n / s->secsz;
		if(nsec > 1)
			n = (1 + simrand(s) % nsec) * s->secsz;
		break;
	}
	record(s, Sopwrite, off, n);
	if(f == Sfdrop)
		return n;
	nsec = n / s->secsz;
	sec = off / s->secsz;
	p = a;
	for(i = 0; i < nsec; i++){
		if(f == Sftearsec && (simrand(s) & 1))
			continue;
		land(s, sec + i, p + i*s->secsz, f == Sftearbyte);
	}
	return n;
}

static int
simflush(Dev *d)
{
	Sim *s;
	uvlong i;

	s = d->aux;
	record(s, Sopflush, 0, 0);
	if(s->ndirty == 0)
		return 0;
	for(i = 0; i < s->nsec; i++)
		if(s->dirty[i]){
			memmove(s->stable + i*s->secsz, s->live + i*s->secsz,
				s->secsz);
			s->dirty[i] = 0;
		}
	s->ndirty = 0;
	return 0;
}

static void
simpointf(Dev *d, char *name, int n)
{
	Sim *s;

	s = d->aux;
	if(s->point[0] == '\0' || strcmp(name, s->point) != 0 || n != s->pointn)
		return;
	s->point[0] = '\0';
	simcrash(d);
}

static void
simclose(Dev *d)
{
	Sim *s;

	s = d->aux;
	free(s->stable);
	free(s->live);
	free(s->dirty);
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

void
simfault(Dev *d, int kind, int n)
{
	Sim *s;

	s = d->aux;
	s->fault = kind;
	s->faultn = n;
}

/* discard every sector written since the last flush */
void
simcrash(Dev *d)
{
	Sim *s;
	uvlong i;

	s = d->aux;
	record(s, Sopcrash, 0, 0);
	for(i = 0; i < s->nsec; i++)
		if(s->dirty[i]){
			memmove(s->live + i*s->secsz, s->stable + i*s->secsz,
				s->secsz);
			s->dirty[i] = 0;
		}
	s->ndirty = 0;
	s->fault = Sfnone;
	s->faultn = 0;
}

void
simarm(Dev *d, char *point, int n)
{
	Sim *s;

	s = d->aux;
	if(point == nil){
		s->point[0] = '\0';
		return;
	}
	strecpy(s->point, s->point + sizeof s->point, point);
	s->pointn = n;
}

/* poke bytes into durable storage, which is how a media fault is staged */
void
simpoke(Dev *d, vlong off, void *buf, long n)
{
	Sim *s;

	s = d->aux;
	if(off < 0 || off + n > d->size)
		sysfatal("simpoke: %lld+%ld out of range", off, n);
	memmove(s->stable + off, buf, n);
	memmove(s->live + off, buf, n);
}

void
simpeek(Dev *d, vlong off, void *buf, long n)
{
	Sim *s;

	s = d->aux;
	if(off < 0 || off + n > d->size)
		sysfatal("simpeek: %lld+%ld out of range", off, n);
	memmove(buf, s->stable + off, n);
}

uvlong
simdirty(Dev *d)
{
	Sim *s;

	s = d->aux;
	return s->ndirty;
}

long
simtrace(Dev *d, Simop **t)
{
	Sim *s;

	s = d->aux;
	*t = s->trace;
	return s->ntrace;
}

void
simtracereset(Dev *d)
{
	Sim *s;

	s = d->aux;
	s->ntrace = 0;
}
