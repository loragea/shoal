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
 * There is one exception, and it is forced: clause 2's *release*
 * branch reads the entry's own emapslot, because the record carries
 * no field naming the slot being released.  It is still idempotent —
 * releasing a slot twice is releasing it — and §5 step 11 rebuilds
 * both slot free lists from the entries themselves, so a release
 * replay cannot see leaks nothing across a restart.
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
int
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
	/*
	 * A block at or beyond the record's own nblk is not a block this
	 * record has: clause 3 would mark its grain allocated and clause
	 * 5 would then clear the entry that names it, leaking the grain
	 * with nothing left pointing at it.
	 */
	nblk = blkcount(o->len, s->sb.blksz);
	/*
	 * An object of more than one block keeps its map out of line, so
	 * a record that claims otherwise names blocks the entry cannot
	 * hold.  This writer never produces one, but replay accepts
	 * records from any build, and the entry such a record leaves has
	 * objverify copy from the nil mapdig returns for i >= 1 on an
	 * inline map.
	 */
	if(nblk > 1 && o->emapslot == 0){
		werrstr("Eobj: len %llud is %llud blocks with no extent-map "
			"slot", o->len, nblk);
		return -1;
	}
	for(i = 0; i < o->nmap; i++){
		if(o->map[i].blk >= s->sb.nblkmax || o->map[i].blk >= nblk){
			werrstr("Eobj: block %lud, nblk %llud, nblkmax %lud",
				o->map[i].blk, nblk, s->sb.nblkmax);
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
		/*
		 * §3.2's commit path grew this buffer before the record
		 * was written, so the allocation below is replay's alone
		 * — and there a failure stops the replay, which is a
		 * refusal to start rather than a half-applied record.
		 */
		if(e->oid == nil || e->oidcap < o->oidlen){
			free(e->oid);
			if((e->oid = malloc(o->oidlen)) == nil){
				e->oidcap = 0;
				return -1;
			}
			e->oidcap = o->oidlen;
		}
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
		/*
		 * §5 step 9: an entry a record touches is restored by
		 * applying the deltas and recomputing its checksum — the
		 * bytes the record does not name are this object's own,
		 * old-or-new and therefore intact — so it is no longer the
		 * entry that failed its csum.
		 */
		c->bad = 0;
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
 * The peer set, §2.6.  A peer is remembered as soon as one of its
 * fine-grained records exists, because the exhaustion rule below has
 * to be able to name the peer whose records it drops.  fullsync is
 * never persisted: it is set for every peer at start (§5 step 12).
 */
Peer*
addpeer(Store *s, uchar *name, int n)
{
	Peer *p;

	for(p = s->peers; p != nil; p = p->next)
		if(strlen(p->name) == (ulong)n
		&& memcmp(p->name, name, n) == 0)
			return p;
	if((p = mallocz(sizeof *p, 1)) == nil)
		return nil;
	memmove(p->name, name, n);
	p->name[n] = '\0';
	p->fullsync = 1;
	p->next = s->peers;
	s->peers = p;
	return p;
}

static int
peerowns(Dirtent *t, Peer *p)
{
	return t->peerlen == strlen(p->name)
		&& memcmp(t->peer, p->name, t->peerlen) == 0;
}

/*
 * §2.6: ndirty is an implementation limit in exactly layer-a §7.1's
 * sense.  When it is exhausted the store discards every fine-grained
 * record for the peer with the most records and marks that peer
 * fullsync, which layer-a §7.1 explicitly permits — the records are
 * dropped, not the write.  Dropping them here, in the apply, is what
 * keeps §3.2's commit path and §5's replay answering the same thing:
 * a full region that the live path refused and replay ignored would
 * be a store whose memory differs from what its own log rebuilds.
 */
static int
dropworstpeer(Store *s)
{
	Dirtent *t;
	Peer *p, *worst;
	ulong i, n, best;

	worst = nil;
	best = 0;
	for(p = s->peers; p != nil; p = p->next){
		n = 0;
		for(i = 0; i < s->sb.ndirty; i++)
			if((t = s->dirt[i]) != nil && peerowns(t, p))
				n++;
		if(n > best){
			best = n;
			worst = p;
		}
	}
	if(worst == nil)
		return -1;
	for(i = 0; i < s->sb.ndirty; i++)
		if((t = s->dirt[i]) != nil && peerowns(t, worst)){
			free(t);
			s->dirt[i] = nil;
			s->ndirtused--;
			dirtdirty(s, i);
		}
	worst->fullsync = 1;
	return 0;
}

/* the field checks §2.6 makes on a record, before anything is believed */
int
dirtyrecok(Store *s, Dirtyrec *d)
{
	USED(s);
	if(d->oidlen < 1 || d->oidlen > Oidmax || d->peerlen < 1
	|| d->peerlen > Peermax){
		werrstr("Edirty: oidlen %d peerlen %d", d->oidlen, d->peerlen);
		return -1;
	}
	return 0;
}

/*
 * Edirty (§2.7 kind 2).  layer-a §7.1's fine-grained dirty set, which
 * §14(2) folds into the same record as the update it belongs to, so
 * that either both are durable or neither.
 *
 * spare, when the caller passes one, is a record the commit path
 * allocated before its log record was written (§3.2): the apply of a
 * durable record does not allocate.  Replay passes none and allocates
 * here, where a failure stops the replay rather than half-applying a
 * record.
 */
int
applydirty(Store *s, Dirtyrec *d, Dirtent **spare)
{
	Dirtent *t;
	ulong i;

	if(dirtyrecok(s, d) < 0)
		return -1;
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
	addpeer(s, d->peer, d->peerlen);
	for(;;){
		for(i = 0; i < s->sb.ndirty; i++)
			if(s->dirt[i] == nil)
				break;
		if(i < s->sb.ndirty)
			break;
		if(dropworstpeer(s) < 0){
			werrstr("dirty region full");
			return -1;
		}
	}
	if(spare != nil && *spare != nil){
		t = *spare;
		*spare = nil;
		memset(t, 0, sizeof *t);
	}else if((t = mallocz(sizeof *t, 1)) == nil)
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
	if(e->state == Sfree){
		/*
		 * §2.3: a free entry is a valid record, not zeroes.  A slot
		 * condemned because its own entry failed its csum128 is one
		 * of these — readindex leaves it Sfree — and writing a valid
		 * free record over bytes that could not be read preserves
		 * nothing.  A slot condemned by §5 step 9's later rule is
		 * not: its entry is intact and only the extent map it names
		 * is damaged, so it is written back as it stands.  Erasing
		 * it would free the slot at the next start, leak every grain
		 * the object held, and lose the store's only record that it
		 * ever held that object — §5 step 10's "not reused" would
		 * hold for one run.
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
