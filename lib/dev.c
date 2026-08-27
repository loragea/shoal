#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"

/*
 * The device interface's generic half, docs/design/store.md §0.
 *
 * devsd truncates a request to SDmaxio and to the partition end
 * rather than splitting or failing, so a short count is normal and is
 * never an error (docs/platform/9front-storage.md §5).  Every access
 * therefore loops until the whole range is done.  Two error strings
 * are not media errors and are reported as their own classes:
 * `interrupted', which means reqqueueflush aborted the system call
 * this proc was in, and Echange, which means the unit's partitions
 * were re-declared under an open fid.
 */

int
deverr(void)
{
	char err[ERRMAX];

	rerrstr(err, sizeof err);
	if(strcmp(err, "interrupted") == 0)
		return Deintr;
	/* the kernel's Echange is "media or partition has changed" */
	if(strstr(err, "has changed") != nil)
		return Dechange;
	return Deio;
}

char*
flushname(int mode)
{
	switch(mode){
	case Fraw:
		return "raw";
	case Fasserted:
		return "asserted-writethrough";
	case Funknown:
		return "not-examined";
	}
	return "none";
}

int
devread(Dev *d, void *a, long n, vlong off)
{
	uchar *p;
	long m;

	if(n < 0 || off < 0 || off + n > d->size){
		werrstr("%s: read %ld at %lld out of range", d->name, n, off);
		return -1;
	}
	p = a;
	while(n > 0){
		m = (*d->ops->read)(d, p, n, off);
		if(m < 0)
			return -1;
		if(m == 0){
			werrstr("%s: read %ld at %lld: no progress", d->name, n, off);
			return -1;
		}
		p += m;
		off += m;
		n -= m;
	}
	return 0;
}

int
devwrite(Dev *d, void *a, long n, vlong off)
{
	uchar *p;
	long m;

	if(n < 0 || off < 0 || off + n > d->size){
		werrstr("%s: write %ld at %lld out of range", d->name, n, off);
		return -1;
	}
	if(d->rdonly){
		werrstr("%s: write %ld at %lld: opened read-only", d->name,
			n, off);
		return -1;
	}
	if(n % d->secsz != 0 || off % d->secsz != 0){
		/*
		 * §0: every write's length is a sector multiple, because
		 * devsd silently turns a partial one into a
		 * read-modify-write of every sector it touches.
		 */
		werrstr("%s: unaligned write %ld at %lld", d->name, n, off);
		return -1;
	}
	if((ulong)n > d->wunit){
		/*
		 * §0: the store MUST NOT issue a single pwrite larger
		 * than Wunit.  A larger one buys nothing — devsd issues
		 * one request per pwrite and the drivers split it again
		 * — and obscures what one device round trip costs.
		 */
		werrstr("%s: write %ld at %lld exceeds the %lud-byte write "
			"unit", d->name, n, off, d->wunit);
		return -1;
	}
	p = a;
	while(n > 0){
		m = (*d->ops->write)(d, p, n, off);
		if(m < 0)
			return -1;
		if(m == 0){
			werrstr("%s: write %ld at %lld: no progress", d->name, n, off);
			return -1;
		}
		p += m;
		off += m;
		n -= m;
	}
	return 0;
}

int
devflush(Dev *d)
{
	return (*d->ops->flush)(d);
}

void
devpoint(Dev *d, char *name, int n)
{
	if(d->ops->point != nil)
		(*d->ops->point)(d, name, n);
}

void
devclose(Dev *d)
{
	if(d == nil)
		return;
	(*d->ops->close)(d);
	free(d->name);
	free(d);
}

/*
 * Zero a byte range, writing in pieces of unit bytes.  unit is the
 * caller's Wunit: §0 forbids a single pwrite larger than it.
 */
int
devzero(Dev *d, vlong off, vlong n, ulong unit)
{
	uchar *buf;
	long m;

	if(unit == 0 || unit % d->secsz != 0 || unit > d->wunit){
		werrstr("devzero: bad unit %lud", unit);
		return -1;
	}
	if((buf = mallocz(unit, 1)) == nil)
		return -1;
	while(n > 0){
		m = unit;
		if(m > n)
			m = n;
		if(devwrite(d, buf, m, off) < 0){
			free(buf);
			return -1;
		}
		off += m;
		n -= m;
	}
	free(buf);
	return 0;
}
