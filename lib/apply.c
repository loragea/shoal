#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"
#include "store.h"

/*
 * The one apply function, docs/design/store.md §2.7 and §3.2.
 *
 * Applying an Eobj sets absolute values, and it MUST, in this order:
 *
 *  1. set the four-tuple and len in the index entry, and derive
 *     nblk = blkcount(len) from that len;
 *  2. if the record carries Oslot, allocate or release the extent-map
 *     slot emapslot names and zero the whole target map;
 *  3. set grain[b]/dig[b] for every block named by nmap;
 *  4. for every block below nblk that nmap does not name and whose
 *     grain[i] is 0, set dig[i] to the digest of the zero bytes that
 *     block reads as;
 *  5. clear grain[i] and dig[i] for every i >= nblk;
 *  6. free every grain in nfree.
 *
 * **Every clause is a function of the record and of nblk alone.**
 * None of them reads the entry's live state: clauses 1, 3, 5 and 6
 * set absolute values, clause 2 branches on a bit the record carries,
 * and clause 4 is recomputed unconditionally over the map the others
 * leave.  So applying a record twice — once live, once again in
 * replay over a checkpoint that already materialised part of its
 * effect — gives the same entry both times.  A clause that compared
 * against the entry instead is one a half-written checkpoint disarms
 * silently.
 *
 * One function, two callers: the commit path of §3.2 and the replay
 * of §5 step 7.  The two disagreeing about an unnamed block or an
 * inherited grain number is a bug that appears only after a crash,
 * which is the most expensive place for one to be.
 *
 * Every function here is called with qlstate held, over an extent map
 * its caller has pinned (§7).
 */

/* the in-memory index hash, §9 */
static ulong
oidhash(uchar *oid, int n)
{
	ulong h;
	int i;

	h = 2166136261UL;
	for(i = 0; i < n; i++){
		h ^= oid[i];
		h *= 16777619UL;
	}
	return h;
}

void
ienthash(Store *s, ulong slot)
{
	Ient *e;
	ulong b;

	e = &s->idx[slot];
	if(e->state == Sfree || e->oid == nil)
		return;
	b = oidhash(e->oid, e->oidlen) % s->nhash;
	e->hashnext = s->hash[b];
	s->hash[b] = slot;
}

void
ientunhash(Store *s, ulong slot)
{
	Ient *e;
	ulong b, k;

	e = &s->idx[slot];
	if(e->oid == nil)
		return;
	b = oidhash(e->oid, e->oidlen) % s->nhash;
	if(s->hash[b] == slot){
		s->hash[b] = e->hashnext;
		return;
	}
	for(k = s->hash[b]; k != ~0UL; k = s->idx[k].hashnext)
		if(s->idx[k].hashnext == slot){
			s->idx[k].hashnext = e->hashnext;
			return;
		}
}

long
ientfind(Store *s, uchar *oid, int oidlen)
{
	Ient *e;
	ulong b, k;

	b = oidhash(oid, oidlen) % s->nhash;
	for(k = s->hash[b]; k != ~0UL; k = e->hashnext){
		e = &s->idx[k];
		if(e->oidlen == oidlen && e->oid != nil
		&& memcmp(e->oid, oid, oidlen) == 0)
			return k;
	}
	return -1;
}

/*
 * The digest of the zero bytes block i of an object of this length
 * reads as (§2.4).  The full-block value is computed once at start;
 * the short final block's is computed when needed, because it changes
 * with len whether or not nblk moved.
 */
void
zerodigest(Store *s, uvlong len, ulong i, uchar *dig)
{
	uvlong off;
	ulong n;

	off = (uvlong)i * s->sb.blksz;
	n = s->sb.blksz;
	if(off >= len)
		n = 0;
	else if(off + n > len)
		n = len - off;
	if(n == s->sb.blksz){
		memmove(dig, s->zerodig, Blkdlen);
		return;
	}
	blkdigest(s->zeroblk, n, dig);
}

/*
 * A block map, uniformly: the inline grain0/dig0 of a one-block
 * object and the extent-map entry otherwise.  Block 0 is the only
 * block an inline map has.
 */
void
mapopen(Store *s, Omap *m, Ient *e, Emape *c)
{
	m->s = s;
	m->e = e;
	m->c = e->emapslot != 0 ? c : nil;
}

ulong
mapgrain(Omap *m, ulong i)
{
	if(m->c == nil)
		return i == 0 ? m->e->grain0 : 0;
	if(i >= m->s->sb.nblkmax)
		return 0;
	return emapgrain(m->c->p, i);
}

uchar*
mapdig(Omap *m, ulong i)
{
	if(m->c == nil)
		return i == 0 ? m->e->dig0 : nil;
	if(i >= m->s->sb.nblkmax)
		return nil;
	return emapdig(m->c->p, m->s->sb.nblkmax, i);
}

void
mapset(Omap *m, ulong i, ulong grain, uchar *dig)
{
	uchar *d;

	if(m->c == nil){
		if(i != 0)
			return;
		m->e->grain0 = grain;
		if(dig != nil)
			memmove(m->e->dig0, dig, Blkdlen);
		else
			memset(m->e->dig0, 0, Blkdlen);
		return;
	}
	if(i >= m->s->sb.nblkmax)
		return;
	emapsetgrain(m->c->p, i, grain);
	d = emapdig(m->c->p, m->s->sb.nblkmax, i);
	if(dig != nil)
		memmove(d, dig, Blkdlen);
	else
		memset(d, 0, Blkdlen);
}

/* the last block an object's map can name, inline or out of line */
static ulong
maplimit(Store *s, Ient *e)
{
	return e->emapslot != 0 ? s->sb.nblkmax : 1;
}

/*
 * Range-check a record before any of it is believed.  A record that
 * passed its checksum can still name a slot, a block or a grain this
 * geometry does not have if it was written by a different build or
 * read from a lap this store never wrote; applying half of it and
 * then refusing is worse than refusing all of it.
 */
static int
objrecok(Store *s, Objrec *o)
{
	uvlong nblk;
	ulong i;

	if(o->slot >= s->sb.nslots){
		werrstr("Eobj: slot %lud, nslots %lud", o->slot, s->sb.nslots);
		return -1;
	}
	if(o->emapslot >= s->sb.nemap){
		werrstr("Eobj: emapslot %lud, nemap %lud", o->emapslot,
			s->sb.nemap);
		return -1;
	}
	if(o->len > s->sb.objmax){
		werrstr("Eobj: len %llud, objmax %llud", o->len, s->sb.objmax);
		return -1;
	}
	nblk = blkcount(o->len, s->sb.blksz);
	USED(nblk);
	for(i = 0; i < o->nmap; i++){
		if(o->map[i].blk >= s->sb.nblkmax){
			werrstr("Eobj: block %lud, nblkmax %lud", o->map[i].blk,
				s->sb.nblkmax);
			return -1;
		}
		if(o->map[i].grain >= s->sb.ngrains){
			werrstr("Eobj: grain %lud, ngrains %llud",
				o->map[i].grain, s->sb.ngrains);
			return -1;
		}
	}
	for(i = 0; i < o->nfree; i++)
		if(o->freed[i] >= s->sb.ngrains){
			werrstr("Eobj: freed grain %lud, ngrains %llud",
				o->freed[i], s->sb.ngrains);
			return -1;
		}
	return 0;
}

int
applyrec(Store *s, Objrec *o, Emape *c)
{
	Ient *e;
	Omap m;
	uvlong nblk;
	ulong i, lim;
	uchar dig[Blkdlen];

	if(objrecok(s, o) < 0)
		return -1;
	e = &s->idx[o->slot];

	/* clause 1: the four-tuple and len, and nblk derived from that len */
	ientunhash(s, o->slot);
	if(e->oidlen != o->oidlen || e->oid == nil
	|| memcmp(e->oid, o->oid, o->oidlen) != 0){
		free(e->oid);
		if((e->oid = malloc(o->oidlen)) == nil)
			return -1;
		memmove(e->oid, o->oid, o->oidlen);
	}
	if(e->state == Slive)
		s->nlive--;
	else if(e->state == Stomb)
		s->ntomb--;
	e->oidlen = o->oidlen;
	e->state = o->state;
	e->flags = 0;
	if(o->oflags & Ocorrupt)
		e->flags |= Icorrupt;
	e->bad = 0;
	e->qidpath = o->qidpath;
	e->len = o->len;
	e->ver = o->ver;
	e->wepoch = o->wepoch;
	e->mtime = o->mtime;
	e->cur = 0;
	memmove(e->csum, o->csum, Csumlen);
	if(e->state == Slive)
		s->nlive++;
	else if(e->state == Stomb)
		s->ntomb++;
	ienthash(s, o->slot);
	slotmark(s, o->slot);
	idxdirty(s, o->slot);
	nblk = blkcount(o->len, s->sb.blksz);

	/*
	 * Clause 2: the extent-map slot rule.  A commit that changes
	 * emapslot in either direction sets Oslot, names every block
	 * below nblk, and is applied by zeroing the target map first.
	 * Without the zeroing a slot released by a deleted object still
	 * holds that object's map — §2.4 zeroes nothing on release — so
	 * the new owner would inherit live grain numbers the allocator
	 * has since handed to other objects.
	 */
	if(o->oflags & Oslot){
		if(o->emapslot != 0){
			if(c == nil || c->slot != o->emapslot){
				werrstr("Eobj: slot %lud: no pinned map for "
					"emapslot %lud", o->slot, o->emapslot);
				return -1;
			}
			emapmark(s, o->emapslot);
			memset(c->p, 0, s->sb.emapsz);
			c->bad = 0;
		}else{
			if(e->emapslot != 0)
				emapclear(s, e->emapslot);
			e->grain0 = 0;
			memset(e->dig0, 0, Blkdlen);
		}
	}
	e->emapslot = o->emapslot;
	if(e->emapslot != 0){
		if(c == nil || c->slot != e->emapslot){
			werrstr("Eobj: slot %lud: no pinned map for emapslot "
				"%lud", o->slot, e->emapslot);
			return -1;
		}
		emapmark(s, e->emapslot);
		PBIT32(c->p + 16, (ulong)nblk);
		PBIT32(c->p + 20, Storevers);
		emapdirty(s, c);
	}
	mapopen(s, &m, e, c);

	/* clause 3: the blocks this commit names */
	for(i = 0; i < o->nmap; i++){
		mapset(&m, o->map[i].blk, o->map[i].grain, o->map[i].dig);
		grainmark(s, o->map[i].grain);
	}

	/*
	 * Clause 4: every hole below nblk hashes as the zero bytes it
	 * reads as.  Stated as a walk of the map wherever grain[i] is 0
	 * rather than over the range a growth covers, because the range
	 * is a function of what the entry held before and the walk is
	 * not — and the short final block's zero digest changes with len
	 * whether or not nblk moved.
	 */
	lim = maplimit(s, e);
	for(i = 0; i < nblk && i < lim; i++)
		if(mapgrain(&m, i) == 0){
			zerodigest(s, o->len, i, dig);
			mapset(&m, i, 0, dig);
		}

	/* clause 5: §2.4's invariant on the shrinking side */
	for(i = nblk; i < lim; i++)
		mapset(&m, i, 0, nil);

	/* clause 6: the grains this commit releases */
	for(i = 0; i < o->nfree; i++)
		grainclear(s, o->freed[i]);
	return 0;
}

/*
 * Edirty (§2.7 kind 2).  layer-a §7.1's fine-grained dirty set, which
 * §14(2) folds into the same record as the update it belongs to, so
 * that either both are durable or neither.
 */
int
applydirty(Store *s, Dirtyrec *d)
{
	Dirtent *t;
	ulong i;

	if(d->oidlen < 1 || d->oidlen > Oidmax || d->peerlen < 1
	|| d->peerlen > Peermax){
		werrstr("Edirty: oidlen %d peerlen %d", d->oidlen, d->peerlen);
		return -1;
	}
	for(i = 0; i < s->sb.ndirty; i++){
		if((t = s->dirt[i]) == nil)
			continue;
		if(t->oidlen == d->oidlen && t->peerlen == d->peerlen
		&& memcmp(t->oid, d->oid, d->oidlen) == 0
		&& memcmp(t->peer, d->peer, d->peerlen) == 0){
			if(d->op == 0){
				free(t);
				s->dirt[i] = nil;
				s->ndirtused--;
			}else
				t->epoch = d->epoch;
			dirtdirty(s, i);
			return 0;
		}
	}
	if(d->op == 0)
		return 0;			/* removing what is not there */
	for(i = 0; i < s->sb.ndirty; i++)
		if(s->dirt[i] == nil)
			break;
	if(i == s->sb.ndirty){
		/*
		 * §2.6: ndirty is an implementation limit in layer-a
		 * §7.1's sense.  Exhausting it is a fullsync for the peer
		 * that owns the most records, which layer-a permits; the
		 * records are dropped, not the write.
		 */
		werrstr("dirty region full");
		return -1;
	}
	if((t = mallocz(sizeof *t, 1)) == nil)
		return -1;
	t->epoch = d->epoch;
	t->state = 1;
	t->oidlen = d->oidlen;
	t->peerlen = d->peerlen;
	t->vers = Storevers;
	memmove(t->oid, d->oid, d->oidlen);
	memmove(t->peer, d->peer, d->peerlen);
	s->dirt[i] = t;
	s->ndirtused++;
	dirtdirty(s, i);
	return 0;
}

/*
 * Eslot (§2.7 kind 3): free the index slot and its extent-map slot if
 * it holds one — a tombstone discard or an op=drop.
 */
int
applyslot(Store *s, ulong slot)
{
	Ient *e;

	if(slot >= s->sb.nslots){
		werrstr("Eslot: slot %lud, nslots %lud", slot, s->sb.nslots);
		return -1;
	}
	e = &s->idx[slot];
	ientunhash(s, slot);
	if(e->emapslot != 0)
		emapclear(s, e->emapslot);
	if(e->state == Slive)
		s->nlive--;
	else if(e->state == Stomb)
		s->ntomb--;
	free(e->oid);
	memset(e, 0, sizeof *e);
	e->hashnext = ~0UL;
	slotclear(s, slot);
	idxdirty(s, slot);
	return 0;
}

/* the on-disk image of one index entry, for the checkpointer */
void
ientpack(Store *s, ulong slot, uchar *p)
{
	Idxent d;
	Ient *e;

	e = &s->idx[slot];
	memset(&d, 0, sizeof d);
	d.vers = Storevers;
	if(e->state == Sfree || e->bad){
		/*
		 * §2.3: a free entry is a valid record, not zeroes.  A
		 * condemned slot is written free too — §5 step 10 keeps it
		 * out of the free list in memory, and the damaged bytes on
		 * the disk are not worth preserving.
		 */
		idxpack(p, &d);
		return;
	}
	d.state = e->state;
	d.oidlen = e->oidlen;
	d.flags = e->flags;
	d.emapslot = e->emapslot;
	d.qidpath = e->qidpath;
	d.len = e->len;
	d.ver = e->ver;
	d.wepoch = e->wepoch;
	d.mtime = e->mtime;
	memmove(d.csum, e->csum, Csumlen);
	if(e->oid != nil)
		memmove(d.oid, e->oid, e->oidlen);
	d.grain0 = e->grain0;
	memmove(d.dig0, e->dig0, Blkdlen);
	idxpack(p, &d);
}
