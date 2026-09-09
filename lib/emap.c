#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"
#include "store.h"

/*
 * The extent-map cache, docs/design/store.md §9, and the pinning rule
 * §7 states.
 *
 * The extent maps of multi-block objects are deliberately not all in
 * memory — 21 KiB times a quarter of a million objects is 5.5 GB — so
 * the write path reads one entry per multi-block object it touches,
 * backed by an LRU.  A miss inserts an entry marked *busy* under
 * qlemap and releases it; the missing proc reads the entry; it then
 * fills the entry and wakes anyone who found it busy.  So one read
 * serves concurrent readers of the same map and no lock spans it.
 *
 * **A staged map is pinned from the stage that read it to the apply
 * that changes it.**  A pinned entry is not in the eviction set, so
 * the apply always finds the map in memory: it never faults, never
 * reads the device under a lock, and mutates the map under the pin
 * rather than under qlemap — which is what lets the committer apply
 * its whole batch under qlstate alone.
 *
 * Who may touch what:
 *
 *   - an entry's *bytes* and its dirty flag are mutated only by the
 *     apply function, which runs under qlstate and holds a pin.  The
 *     checkpointer reads them under qlstate for the same reason.
 *   - the hash table, the LRU and the pin counts are qlemap's.
 *   - eviction takes only unpinned, clean entries, and an unpinned
 *     entry's dirty flag is stable because only a pin holder sets it.
 *     An entry the checkpointer has taken off the dirty list is
 *     clean and may be unpinned, so it carries wb for as long as its
 *     write-back is in flight: a failed write-back has to have an
 *     entry to mark dirty again (§2.8), and freeing it under the
 *     checkpointer would leave the on-disk entry half written and
 *     nothing in memory to rewrite it.
 *   - wb is written under qlstate and read here under qlemap, which
 *     is legal only because ckpt.c orders the two flags so that one
 *     of dirty and wb is always set while an entry is in write-back.
 *     etrim's test reads them in that order and so can never see the
 *     handover.
 */

static Emape*
elookup(Store *s, ulong slot)
{
	Emape *c;

	for(c = s->ehash[slot % s->nehash]; c != nil; c = c->hnext)
		if(c->slot == slot)
			return c;
	return nil;
}

/* caller holds qlemap */
static void
lruout(Store *s, Emape *c)
{
	if(c->prev != nil)
		c->prev->next = c->next;
	else
		s->elru = c->next;
	if(c->next != nil)
		c->next->prev = c->prev;
	else
		s->elrutail = c->prev;
	c->prev = c->next = nil;
}

static void
lrufront(Store *s, Emape *c)
{
	c->prev = nil;
	c->next = s->elru;
	if(s->elru != nil)
		s->elru->prev = c;
	s->elru = c;
	if(s->elrutail == nil)
		s->elrutail = c;
}

/* caller holds qlemap */
static void
efree(Store *s, Emape *c)
{
	Emape **pp, *t;

	for(pp = &s->ehash[c->slot % s->nehash]; (t = *pp) != nil; pp = &t->hnext)
		if(t == c){
			*pp = c->hnext;
			break;
		}
	lruout(s, c);
	s->nemapc--;
	free(c->p);
	free(c);
}

/*
 * Trim the cache to its capacity.  Only clean, unpinned, idle entries
 * go: a dirty one is state the checkpointer has not yet materialised
 * and a pinned one is a stage's map.  Caller holds qlemap.
 */
static void
etrim(Store *s)
{
	Emape *c, *p;

	for(c = s->elrutail; c != nil && s->nemapc > s->emapcap; c = p){
		p = c->prev;
		if(c->pin == 0 && !c->dirty && !c->busy && !c->wb)
			efree(s, c);
	}
}

/*
 * Fetch an extent-map entry and pin it.  fresh means the caller is
 * about to zero the whole entry under §2.7 clause 2, so the device
 * read would be read of bytes that are about to be thrown away — and
 * §2.4 says a released entry's bytes are not to be trusted anyway.
 *
 * Called with no store lock held: it reads the device.
 */
Emape*
emapget(Store *s, ulong slot, int fresh)
{
	Emape *c;
	uvlong off;
	ulong n, m;
	int err;

	if(slot == 0 || slot >= s->sb.nemap){
		werrstr("extent-map slot %lud out of range", slot);
		return nil;
	}
	qlock(&s->qlemap);
	for(;;){
		if((c = elookup(s, slot)) == nil)
			break;
		if(c->busy){
			rsleep(&s->emaprz);
			continue;
		}
		c->pin++;
		lruout(s, c);
		lrufront(s, c);
		qunlock(&s->qlemap);
		return c;
	}
	if((c = mallocz(sizeof *c, 1)) == nil
	|| (c->p = mallocz(s->sb.emapsz, 1)) == nil){
		free(c);
		qunlock(&s->qlemap);
		werrstr("out of memory");
		return nil;
	}
	c->slot = slot;
	c->busy = 1;
	c->pin = 1;
	c->hnext = s->ehash[slot % s->nehash];
	s->ehash[slot % s->nehash] = c;
	lrufront(s, c);
	s->nemapc++;
	qunlock(&s->qlemap);

	err = 0;
	if(!fresh){
		/*
		 * §0: bulk reads use 64 KiB requests where they can.  An
		 * entry is 41 sectors at the defaults, so this is one
		 * request there and never more than a handful.
		 */
		off = emapentoff(&s->sb, slot);
		for(n = 0; n < s->sb.emapsz; n += m){
			m = s->sb.emapsz - n;
			if(m > 64*1024)
				m = 64*1024;
			if(devread(s->d, c->p + n, m, off + n) < 0){
				err = 1;
				break;
			}
		}
		if(!err && !reccsumok(c->p, s->sb.emapsz, 0))
			c->bad = 1;
	}
	qlock(&s->qlemap);
	c->busy = 0;
	rwakeupall(&s->emaprz);
	if(err){
		c->pin = 0;
		efree(s, c);
		qunlock(&s->qlemap);
		werrstr("extent-map slot %lud: %r", slot);
		return nil;
	}
	etrim(s);
	qunlock(&s->qlemap);
	return c;
}

/*
 * Seal an entry and write it, in pieces of at most blksz: an entry is
 * 41 sectors at the defaults, which is more than one device request,
 * and §0 forbids a single write larger than one (§2.8 writes the
 * checkpoint in Wunit pieces for the same reason).
 */
int
emapwrite(Store *s, ulong slot, uchar *p)
{
	uvlong off;
	ulong n, m;

	reccsumset(p, s->sb.emapsz, 0);
	off = emapentoff(&s->sb, slot);
	for(n = 0; n < s->sb.emapsz; n += m){
		m = s->sb.emapsz - n;
		if(m > s->sb.blksz)
			m = s->sb.blksz;
		if(devwriteretry(s->d, p + n, m, off + n) < 0)
			return -1;
	}
	return 0;
}

void
emapunpin(Store *s, Emape *c)
{
	if(c == nil)
		return;
	qlock(&s->qlemap);
	if(c->pin > 0)
		c->pin--;
	etrim(s);
	qunlock(&s->qlemap);
}

/* the pin holder marks its entry for the next checkpoint; under qlstate */
void
emapdirty(Store *s, Emape *c)
{
	if(c->dirty)
		return;
	c->dirty = 1;
	c->dnext = s->edirty;
	s->edirty = c;
	s->ndirtypage++;
}

/*
 * Write back every dirty entry and drop what the cache no longer
 * needs.  This is replay's escape hatch under cache pressure and
 * nothing else's: replay touches one extent map per multi-block
 * object the log names, which is a function of logsecs and not of the
 * cache, so without a write-back the cache would have to hold them
 * all.  It is safe here because start-up is single-proc — no other
 * proc holds a pin, and nothing is mutating an entry's bytes — and
 * because materialising applied state early changes nothing replay
 * depends on: cklogoff has not moved, so the records behind these
 * bytes are still in the log.
 *
 * A read-only store writes nothing at all (§12), so there the escape
 * hatch is not available and the maps are held instead — etrim never
 * evicts a dirty entry, so holding them is all it takes.  What that
 * cannot survive is a log dirtying more entries than the cache is
 * sized for, and that condition is named here: a devwrite refusal
 * would answer `opened read-only', which tells the operator nothing
 * about what was actually hit.
 */
int
emapreclaim(Store *s)
{
	Emape *c, *next;
	ulong ndirty;

	if(s->d->rdonly){
		ndirty = 0;
		for(c = s->edirty; c != nil; c = c->dnext)
			ndirty++;
		if(ndirty > s->emapcap){
			/*
			 * Short on purpose: replay wraps this in its own
			 * refusal and ERRMAX cuts the tail, so the
			 * condition has to fit in front of the numbers.
			 */
			werrstr("extent-map cache is full and the store is "
				"read-only: %lud dirty, room for %lud",
				ndirty, s->emapcap);
			return -1;
		}
		for(c = s->elrutail; c != nil && s->nemapc > s->emapcap;
			c = next){
			next = c->prev;
			if(c->pin == 0 && !c->dirty && !c->busy && !c->wb)
				efree(s, c);
		}
		return 0;
	}
	for(c = s->edirty; c != nil; c = c->dnext){
		if(emapwrite(s, c->slot, c->p) < 0)
			return -1;
		c->dirty = 0;
		c->bad = 0;
	}
	s->edirty = nil;
	for(c = s->elrutail; c != nil && s->nemapc > s->emapcap; c = next){
		next = c->prev;
		if(c->pin == 0 && !c->busy && !c->wb)
			efree(s, c);
	}
	return 0;
}

void
emapfreeall(Store *s)
{
	Emape *c, *next;

	for(c = s->elru; c != nil; c = next){
		next = c->next;
		free(c->p);
		free(c);
	}
	s->elru = s->elrutail = s->edirty = nil;
	s->nemapc = 0;
	if(s->ehash != nil)
		memset(s->ehash, 0, s->nehash*sizeof *s->ehash);
}
