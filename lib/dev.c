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
 * therefore loops until the whole range is done, and a write longer
 * than the device's Wunit is issued as Wunit pieces rather than
 * refused (§0).  Two error strings
 * are not media errors and are reported as their own classes:
 * `interrupted', which means reqqueueflush aborted the system call
 * this proc was in, and Echange, which means the unit's partitions
 * were re-declared under an open fid.
 */

/*
 * Classify the current error string.  Both strings are matched inside
 * whatever the caller has wrapped them in: a device call reports which
 * of its three syscalls failed and on which device, so by the time an
 * error reaches a classifier it is `%s: flush: interrupted' rather
 * than the kernel's bare word, and an exact match would read an
 * ordinary client interrupt as media damage (§0).
 *
 * Only the last `: '-separated segment is matched, because everything
 * before it is text the operator chose: a partition may be named
 * `interrupted' — partition names are free text — and matching the
 * whole string would classify a media error on it as a flushed
 * request, which §0 unwinds into §3.3's step-7 exit instead of
 * reporting.  The kernel's own word is what follows the last wrap.
 */
int
deverr(void)
{
	char err[ERRMAX], *p;

	rerrstr(err, sizeof err);
	if(err[0] == '\0')
		return Denone;
	if((p = strrchr(err, ':')) != nil && p[1] == ' ')
		p += 2;
	else
		p = err;
	if(strstr(p, "interrupted") != nil)
		return Deintr;
	/* the kernel's Echange is "media or partition has changed" */
	if(strstr(p, "has changed") != nil)
		return Dechange;
	return Deio;
}

/*
 * Classify the error a device call the store just made left behind,
 * and take the one class the store cannot carry on from.  §0:
 * Echange means the unit's partitions were re-declared under an open
 * fid.  It is not media damage and MUST NOT be treated as
 * corruption, and there is nothing to unwind to: every offset the
 * store holds may now name something else.
 *
 * It condemns the *fid*, not the proc.  §7 makes every device call
 * from a proc that holds ordering state — a committer inside its
 * batch, the checkpointer — and exits() there ends that proc alone,
 * leaving nflight raised and relseq never advanced, so every other
 * committer waits for ever and storeclose never returns: a hang,
 * which is the one failure §0's rule exists to avoid.  Refusing
 * every later access instead fails the batch in flight, which
 * broken's the store and wakes every waiter (§3.2), and fails every
 * read and every checkpoint after it — so the condition reaches the
 * caller, which is the one that can exit non-zero.
 */
static int
condemned(Dev *d)
{
	if(!d->changed)
		return 0;
	werrstr("%s: the unit's partitions have been re-declared under an "
		"open fid; refusing to serve on a stale fid", d->name);
	return 1;
}

int
devclass(Dev *d)
{
	int e;

	e = deverr();
	if(e == Dechange)
		d->changed = 1;
	return e;
}

/*
 * The store's own write and flush, for the paths that MUST complete:
 * the commit record (§3.2), the checkpoint's pages and the
 * superblock.  A note posted by reqqueueflush aborts whatever system
 * call the proc is in, and §0 says that is not an I/O error — the
 * request has been flushed.  A commit already writing its record
 * cannot be unwound, though: a record half in the log is a record
 * whose successor can never be acked, so §7 has the committer
 * complete the commit and perform only the invalidate half of §3.3's
 * step 7.  Re-issuing the same bytes at the same offset is what
 * completing it means, and it is idempotent.  The count is a bound
 * rather than a policy: a proc noted Nintr times running is not
 * being flushed by a client.
 */
enum
{
	Nintr	= 16,
};

int
devwriteretry(Dev *d, void *a, long n, vlong off)
{
	int i;

	for(i = 0; i <= Nintr; i++){
		if(devwrite(d, a, n, off) == 0)
			return 0;
		if(devclass(d) != Deintr)
			break;
	}
	return -1;
}

int
devflushretry(Dev *d)
{
	int i;

	for(i = 0; i <= Nintr; i++){
		if(devflush(d) == 0)
			return 0;
		if(devclass(d) != Deintr)
			break;
	}
	return -1;
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
	if(condemned(d))
		return -1;
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
	if(condemned(d))
		return -1;
	if(n % d->secsz != 0 || off % d->secsz != 0){
		/*
		 * §0: every write's length is a sector multiple, because
		 * devsd silently turns a partial one into a
		 * read-modify-write of every sector it touches.
		 */
		werrstr("%s: unaligned write %ld at %lld", d->name, n, off);
		return -1;
	}
	/*
	 * §0: the store MUST NOT issue a single pwrite larger than
	 * Wunit — a larger one buys nothing, since devsd issues one
	 * request per pwrite and the drivers split it again, and it
	 * obscures what one device round trip costs.  A longer write
	 * is split here rather than refused: Wunit is a property of
	 * the device and blksz is a property of the format (§2.1), so
	 * a grain larger than the unit is written in unit pieces.
	 */
	p = a;
	while(n > 0){
		m = n;
		if((ulong)m > d->wunit)
			m = d->wunit;
		m = (*d->ops->write)(d, p, m, off);
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
	if(condemned(d))
		return -1;
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
 * caller's buffer size — a grain, at every call site — and need not
 * be the device's Wunit: devwrite splits a piece longer than that.
 */
int
devzero(Dev *d, vlong off, vlong n, ulong unit)
{
	uchar *buf;
	long m;

	if(unit == 0 || unit % d->secsz != 0){
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
