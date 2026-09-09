#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"
#include "store.h"

/*
 * The library-level object API: stage (docs/design/store.md §3.1),
 * commit (§3.2), discard (§3.3), the read path and re-hashing (§4),
 * and the multi-request op=full stages of §3.6.
 *
 * Nothing in the stage is published and nothing in it is a commit.
 * The grains it writes are durable but unreachable — no index entry,
 * no extent map and no bitmap names them — so discarding a stage
 * leaves content, key and csum exactly as they were, still verifying,
 * and costs no durable write.  That is layer-a §5.4 step 7's
 * guarantee by construction rather than by care.
 *
 * Per-object ordering is the caller's (§7's Reqqueue pool in the
 * server, one proc per object in T1): the functions here serialise
 * only the state every proc shares.
 */

typedef struct Upd Upd;
struct Upd
{
	Store	*s;
	ulong	slot;
	Ient	e;			/* a copy of the entry as it was */
	Emape	*cold;			/* the old map, pinned */
	Emape	*cnew;			/* the map the apply mutates, pinned */
	ulong	newslot;
	int	oslot;
	uvlong	newlen;
	uvlong	nblk, oldnblk;
	Mapent	*map;
	ulong	nmap, amap;
	ulong	*freed;
	ulong	nfree, afree;
	int	slotresv;		/* this update reserved an index slot */
	int	emapresv;		/* ... and an extent-map slot */
};

static void updclose(Upd*);

static void
updfree(Upd *u)
{
	free(u->map);
	free(u->freed);
	u->map = nil;
	u->freed = nil;
}

static int
addmap(Upd *u, ulong blk, ulong grain, uchar *dig)
{
	Mapent *m;

	if(u->nmap == u->amap){
		u->amap = u->amap ? 2*u->amap : 8;
		/*
		 * The allocator leaves errstr alone on failure, so every
		 * failed allocation on these paths says so itself — an
		 * untouched return would answer with whatever this proc
		 * last said, which under §7's Reqqueue pool can be another
		 * request's §2.6 wire error (§3.7).
		 */
		if((m = realloc(u->map, u->amap*sizeof *m)) == nil){
			werrstr("out of memory");
			return -1;
		}
		u->map = m;
	}
	m = &u->map[u->nmap++];
	m->blk = blk;
	m->grain = grain;
	if(dig != nil)
		memmove(m->dig, dig, Blkdlen);
	else
		memset(m->dig, 0, Blkdlen);
	return 0;
}

static int
addfree(Upd *u, ulong grain)
{
	ulong *f;

	if(grain == 0)
		return 0;
	if(u->nfree == u->afree){
		u->afree = u->afree ? 2*u->afree : 8;
		if((f = realloc(u->freed, u->afree*sizeof *f)) == nil){
			werrstr("out of memory");
			return -1;
		}
		u->freed = f;
	}
	u->freed[u->nfree++] = grain;
	return 0;
}

/* the digest this update leaves on block i, given what it names */
static void
digof(Upd *u, Omap *mold, ulong i, uchar *dig)
{
	ulong k;
	uchar *d;

	for(k = 0; k < u->nmap; k++)
		if(u->map[k].blk == i){
			memmove(dig, u->map[k].dig, Blkdlen);
			return;
		}
	if(i < u->oldnblk && mapgrain(mold, i) != 0){
		d = mapdig(mold, i);
		if(d != nil){
			memmove(dig, d, Blkdlen);
			return;
		}
	}
	zerodigest(u->s, u->newlen, i, dig);
}

/*
 * The object checksum this update publishes: layer-a §1.4's hash of
 * the whole digest array, with the changed digests substituted.  It
 * is computed from the same rules the apply function will follow, so
 * the digests and the csum a single Eobj carries agree by
 * construction — there is no window in which one is visible without
 * the other (§4).
 */
static int
updcsum(Upd *u, Omap *mold, uchar csum[Csumlen])
{
	uchar *digs;
	uvlong i;

	if(u->nblk == 0){
		csumdigests(nil, 0, csum);
		return 0;
	}
	if((digs = malloc(u->nblk*Blkdlen)) == nil){
		werrstr("out of memory");
		return -1;
	}
	for(i = 0; i < u->nblk; i++)
		digof(u, mold, i, digs + i*Blkdlen);
	csumdigests(digs, u->nblk, csum);
	free(digs);
	return 0;
}

/* release everything a failed or discarded update reserved (§3.3) */
static void
updabort(Upd *u)
{
	Store *s;
	ulong i;

	s = u->s;
	qlock(&s->qlstate);
	for(i = 0; i < u->nmap; i++)
		grainstageclr(s, u->map[i].grain);
	if(u->emapresv)
		emapresvclr(s, u->newslot);
	if(u->slotresv)
		slotresvclr(s, u->slot);
	qunlock(&s->qlstate);
	if(u->cnew != nil && u->cnew != u->cold)
		emapunpin(s, u->cnew);
	if(u->cold != nil)
		emapunpin(s, u->cold);
	u->cnew = u->cold = nil;
	updfree(u);
}

/*
 * Build the Eobj and commit it, then release the pins.  An item is
 * space-freeing — and so may draw on §6's reserved log tail — when it
 * releases grains and allocates none: a delete, a truncate, a
 * tombstone.
 */
static int
updcommit(Upd *u, int state, uvlong ver, uvlong wepoch, vlong mtime,
	int corrupt, Dirtyrec *dr, int ndr)
{
	Store *s;
	Objrec o;
	Item it;
	Omap mold;
	uchar csum[Csumlen];
	ulong i;
	int r, alloc;

	s = u->s;
	mapopen(s, &mold, &u->e, u->cold);
	if(updcsum(u, &mold, csum) < 0){
		updabort(u);
		return -1;
	}
	memset(&o, 0, sizeof o);
	o.slot = u->slot;
	o.emapslot = u->newslot;
	o.qidpath = u->e.qidpath;
	o.state = state;
	o.oidlen = u->e.oidlen;
	o.oflags = 0;
	if(u->oslot)
		o.oflags |= Oslot;
	if(corrupt)
		o.oflags |= Ocorrupt;
	o.len = u->newlen;
	o.ver = ver;
	o.wepoch = wepoch;
	o.mtime = mtime;
	memmove(o.csum, csum, Csumlen);
	memmove(o.oid, u->e.oid, u->e.oidlen);
	o.nmap = u->nmap;
	o.map = u->map;
	o.nfree = u->nfree;
	o.freed = u->freed;

	/*
	 * §6's reserved log tail is for commits that release space and
	 * take none: a delete, a truncate, a tombstone.  A commit that
	 * allocates a grain or an extent-map slot is ordinary traffic
	 * and waits instead of joining a batch that draws on it.
	 */
	alloc = 0;
	for(i = 0; i < u->nmap; i++)
		if(u->map[i].grain != 0)
			alloc = 1;
	if(u->oslot && u->newslot != 0)
		alloc = 1;

	memset(&it, 0, sizeof it);
	it.obj = &o;
	it.dirty = dr;
	it.ndirty = ndr;
	it.emap = u->cnew;
	it.freeing = !alloc && (u->nfree > 0 || state == Stomb
		|| u->newlen < u->e.len);
	r = logcommit(s, &it);
	if(r < 0){
		updabort(u);
		return -1;
	}
	if(u->cnew != nil && u->cnew != u->cold)
		emapunpin(s, u->cnew);
	if(u->cold != nil)
		emapunpin(s, u->cold);
	u->cnew = u->cold = nil;
	updfree(u);
	return 0;
}

/*
 * §3.2: a store whose apply failed after its record was durable is
 * serving in-memory state that its own log no longer describes, so it
 * answers nothing until it has been opened again and replayed.  The
 * commit path refuses through the same flag (broken).  Both are
 * qllog's, which is where the batch that condemns the store sets them
 * and where §3.2's failseq is read beside them.  It is taken alone and
 * released before this call takes any other, so §7 rule 1 — no proc
 * holds two state locks at once — still holds as stated.
 */
static int
serving(Store *s)
{
	int f;

	qlock(&s->qllog);
	f = s->fatal;
	qunlock(&s->qllog);
	if(f){
		werrstr("store condemned: in-memory state no longer matches "
			"the log; open it again");
		return 0;
	}
	return 1;
}

/*
 * Every grain access goes through these two.  §0: `interrupted' on
 * one of them is a flushed request and unwinds into §3.3's step-7
 * exit — the stage is discarded and nothing durable was touched — so
 * it is reported as it is and never retried, unlike the commit
 * record's own writes, which MUST complete (§3.2).  Echange is the
 * class neither of them can carry on from: devclass condemns the fid,
 * so this read or write and every later one fail (§0).
 */
static int
grainread(Store *s, uchar *buf, ulong g)
{
	if(devread(s->d, buf, s->sb.blksz, grainoff(&s->sb, g)) < 0){
		devclass(s->d);
		return -1;
	}
	return 0;
}

static int
grainwrite(Store *s, uchar *buf, ulong g)
{
	if(devwrite(s->d, buf, s->sb.blksz, grainoff(&s->sb, g)) < 0){
		devclass(s->d);
		return -1;
	}
	return 0;
}

/*
 * The extent map of an object about to be read or updated.  §5 step
 * 9: an entry that fails its csum128 and that replay did not touch is
 * damage the log does not cover, so the slot is condemned (§5 step
 * 10) rather than served — every grain number in it is the damaged
 * bytes', and a block read through one is another object's content
 * returned as this object's.
 */
static Emape*
mapread(Store *s, ulong slot, ulong emapslot)
{
	Emape *c;

	if((c = emapget(s, emapslot, 0)) == nil)
		return nil;
	if(c->bad){
		emapunpin(s, c);
		qlock(&s->qlstate);
		storecondemn(s, slot);
		qunlock(&s->qlstate);
		werrstr("checksum mismatch: slot %lud, extent map", slot);
		return nil;
	}
	return c;
}

/*
 * Start an update of an existing object: take a copy of the entry,
 * work out which extent-map slot the new length needs, and pin both
 * the map being read and the map the apply will change.
 */
enum
{
	Utomb	= 1,	/* a tombstone may be opened */
	Ubad	= 2,	/* ... and so may a slot §5 step 10 condemned */
	Ucorrupt = 4,	/* ... and one whose §8 corrupt flag is set */
};

static int
updopen(Upd *u, Store *s, uchar *oid, int oidlen, uvlong newlen, int flags)
{
	long slot;
	Ient *e;

	memset(u, 0, sizeof *u);
	u->s = s;
	qlock(&s->qlstate);
	if((slot = ientfind(s, oid, oidlen)) < 0){
		qunlock(&s->qlstate);
		werrstr("no such object");
		return -1;
	}
	e = &s->idx[slot];
	if(e->state != Slive && !((flags & Utomb) && e->state == Stomb)){
		qunlock(&s->qlstate);
		/*
		 * layer-a §2.6's set is prefix-free by design and these two
		 * are different facts: `object deleted' is access to a
		 * tombstone, `no such object' an id a completed currency
		 * check found nowhere.  Folding them leaves the server no
		 * way to tell them apart without a second, racy objstat,
		 * and a client that sees `no such object' for a tombstoned
		 * id may re-create it.
		 */
		werrstr(e->state == Stomb ? "object deleted"
			: "no such object");
		return -1;
	}
	/*
	 * §5 step 10's "not served": every path but §5.5's op=full
	 * refuses a slot whose extent map is damaged, because every grain
	 * number and digest it would read is the damaged bytes'.
	 */
	if(e->bad && !(flags & Ubad)){
		qunlock(&s->qlstate);
		werrstr("checksum mismatch: slot %lud, extent map", slot);
		return -1;
	}
	/*
	 * §8: a copy whose corrupt flag is set fails client access with
	 * layer-a §2.6's `checksum mismatch' — the content is what failed
	 * verification, and §3.7's row for that condition is where the
	 * spelling is fixed.  The exceptions pass Ucorrupt and each has a
	 * reason: §5.5's op=full and §8's block repair are the repairs, a
	 * delete is self-contained and has no key to defend (its
	 * tombstone holds no content, so its commit clears the flag), and
	 * objcorrupt is the flag's own setter.
	 */
	if((e->flags & Icorrupt) && !(flags & Ucorrupt)){
		qunlock(&s->qlstate);
		werrstr("checksum mismatch: slot %lud, corrupt flag set",
			slot);
		return -1;
	}
	u->slot = slot;
	u->e = *e;
	u->e.oid = malloc(e->oidlen);
	if(u->e.oid == nil){
		qunlock(&s->qlstate);
		werrstr("out of memory");
		return -1;
	}
	memmove(u->e.oid, e->oid, e->oidlen);
	u->newlen = newlen;
	u->nblk = blkcount(newlen, s->sb.blksz);
	u->oldnblk = blkcount(u->e.len, s->sb.blksz);
	/*
	 * A condemned entry's map is rebuilt whole in a *fresh* slot: the
	 * old one's bytes are the damage, so §2.7's Oslot rule is what
	 * this update needs and clause 2 zeroes the new entry before
	 * naming every block.
	 */
	u->newslot = u->e.emapslot;
	if(u->nblk > 1 && (u->e.emapslot == 0 || u->e.bad)){
		if(emapalloc(s, &u->newslot) < 0){
			qunlock(&s->qlstate);
			free(u->e.oid);
			werrstr("disk full");
			return -1;
		}
		u->emapresv = 1;
	}else if(u->nblk <= 1)
		u->newslot = 0;
	u->oslot = u->newslot != u->e.emapslot;
	qunlock(&s->qlstate);

	/*
	 * D14: a copy that fails local verification contributes no key
	 * and has no old map to read — the entry that named its grains
	 * *is* the damaged bytes.  Those grains are unrecoverable and
	 * stay marked used until the slot's map is written again; the
	 * update names none of them, so oldnblk is 0 and nothing reads
	 * through mold.
	 */
	if(u->e.bad)
		u->oldnblk = 0;
	else if(u->e.emapslot != 0
	&& (u->cold = mapread(s, u->slot, u->e.emapslot)) == nil){
		if(!(flags & Ubad)){
			qlock(&s->qlstate);
			if(u->emapresv)
				emapresvclr(s, u->newslot);
			qunlock(&s->qlstate);
			free(u->e.oid);
			return -1;
		}
		/*
		 * mapread has just condemned this slot (§5 step 10): the
		 * damage was not known when the slot decision above was
		 * made, so make it again on what is known now.  §5.5's
		 * op=full is the one caller that may take a condemned copy,
		 * and it must be able to whether or not some earlier read
		 * happened to be the one that found the damage — a repair
		 * that works only for a slot condemned since the last
		 * restart is not a repair.  The map is rebuilt whole in a
		 * fresh slot (§2.7's Oslot rule) and the grains the damaged
		 * entry named stay marked used (§3.6).
		 */
		qlock(&s->qlstate);
		u->e.bad = 1;
		u->oldnblk = 0;
		if(u->nblk > 1 && !u->emapresv){
			if(emapalloc(s, &u->newslot) < 0){
				qunlock(&s->qlstate);
				free(u->e.oid);
				werrstr("disk full");
				return -1;
			}
			u->emapresv = 1;
		}
		qunlock(&s->qlstate);
		u->oslot = u->newslot != u->e.emapslot;
	}
	if(u->newslot == u->e.emapslot)
		u->cnew = u->cold;
	else if(u->newslot != 0
	&& (u->cnew = emapget(s, u->newslot, 1)) == nil){
		emapunpin(s, u->cold);
		qlock(&s->qlstate);
		if(u->emapresv)
			emapresvclr(s, u->newslot);
		qunlock(&s->qlstate);
		free(u->e.oid);
		return -1;
	}
	return 0;
}

/*
 * Start an update of an object this instance does not hold: reserve
 * an index slot and a qid.path so that one Eobj carries the create
 * and the content together.  layer-a §5.5's op=full is the
 * whole-object resync and the receiver holding nothing is its common
 * case — §1.3 makes absence lose arbitration against any verifying
 * copy, so absence is what a heal is usually repairing.  Creating the
 * object first and staging into it afterwards would publish a live
 * zero-length object at the winning key before the content landed,
 * which is what §3.1 and §3.3 exist to prevent: a crash between the
 * two leaves the object live, empty and at the key that wins, so the
 * resync it was meant to complete is never attempted again.
 */
static int
updnew(Upd *u, Store *s, uchar *oid, int oidlen, uvlong newlen)
{
	uvlong qid;

	memset(u, 0, sizeof *u);
	u->s = s;
	u->newlen = newlen;
	u->nblk = blkcount(newlen, s->sb.blksz);
	u->oldnblk = 0;
	qlock(&s->qlstate);
	if(slotalloc(s, &u->slot) < 0){
		qunlock(&s->qlstate);
		werrstr("disk full");
		return -1;
	}
	u->slotresv = 1;
	if(u->nblk > 1){
		if(emapalloc(s, &u->newslot) < 0){
			slotresvclr(s, u->slot);
			qunlock(&s->qlstate);
			werrstr("disk full");
			return -1;
		}
		u->emapresv = 1;
	}
	qunlock(&s->qlstate);
	u->oslot = u->newslot != 0;
	if((u->e.oid = malloc(oidlen)) == nil){
		updabort(u);
		werrstr("out of memory");
		return -1;
	}
	memmove(u->e.oid, oid, oidlen);
	u->e.oidlen = oidlen;
	if((qid = qidalloc(s)) == 0){
		werrstr("qid.path: %r");
		updabort(u);
		updclose(u);
		return -1;
	}
	u->e.qidpath = qid;
	if(u->newslot != 0 && (u->cnew = emapget(s, u->newslot, 1)) == nil){
		updabort(u);
		updclose(u);
		return -1;
	}
	return 0;
}

static void
updclose(Upd *u)
{
	free(u->e.oid);
	u->e.oid = nil;
}

/* the bytes of block i an object of this length covers (layer-a §1.4) */
static ulong
blkcover(Store *s, uvlong len, ulong i)
{
	uvlong lo, n;

	lo = (uvlong)i*s->sb.blksz;
	if(len <= lo)
		return 0;
	n = len - lo;
	return n > s->sb.blksz ? s->sb.blksz : (ulong)n;
}

/*
 * Read block i of an object of length len into blksz bytes of buf, as
 * the object reads it: a read clamps to len (§4), so the bytes at and
 * beyond the block's covered length are not this object's content and
 * MUST compose and hash as the zeros they read as.  A truncate within
 * a block leaves the old bytes in the grain, and without this the next
 * partial write of that block would merge them back into the object.
 */
static int
readblk(Store *s, Omap *m, ulong i, uchar *buf, uvlong len)
{
	ulong g, n;

	g = mapgrain(m, i);
	if(g == 0){
		memset(buf, 0, s->sb.blksz);
		return 0;
	}
	if(g >= s->sb.ngrains){
		werrstr("grain %lud out of range", g);
		return -1;
	}
	if(grainread(s, buf, g) < 0)
		return -1;
	n = blkcover(s, len, i);
	if(n < s->sb.blksz)
		memset(buf + n, 0, s->sb.blksz - n);
	return 0;
}

/*
 * Stage one block: compose its new content, reserve a fresh grain and
 * write it.  A write covering a whole block needs no read; a partial
 * write reads the old grain (or takes zeros for a hole), merges the
 * new bytes and re-hashes the block (§4).
 */
static int
stageblk(Upd *u, Omap *mold, ulong blk, uchar *src, ulong boff, ulong bn,
	uchar *buf)
{
	Store *s;
	ulong g, n;
	uchar dig[Blkdlen];

	s = u->s;
	if(boff == 0 && bn == s->sb.blksz)
		memmove(buf, src, bn);
	else{
		if(readblk(s, mold, blk, buf, u->e.len) < 0)
			return -1;
		memmove(buf + boff, src, bn);
	}
	n = s->sb.blksz;
	if((uvlong)(blk+1)*s->sb.blksz > u->newlen)
		n = u->newlen - (uvlong)blk*s->sb.blksz;
	blkdigest(buf, n, dig);
	qlock(&s->qlstate);
	/* grainalloc spells its own failure: disk full, or out of memory */
	if(grainalloc(s, &g) < 0){
		qunlock(&s->qlstate);
		return -1;
	}
	qunlock(&s->qlstate);
	if(grainwrite(s, buf, g) < 0){
		qlock(&s->qlstate);
		grainstageclr(s, g);
		qunlock(&s->qlstate);
		return -1;
	}
	/*
	 * Until addmap names it, this grain is reserved and unreferenced:
	 * updabort walks u->map to release what the update staged, so a
	 * failure between the write and the naming would leave the
	 * reservation held for the life of the process.  The grain is
	 * released here instead, which is the same thing updabort would
	 * have done for it.
	 */
	if(addfree(u, mapgrain(mold, blk)) < 0
	|| addmap(u, blk, g, dig) < 0){
		qlock(&s->qlstate);
		grainstageclr(s, g);
		qunlock(&s->qlstate);
		return -1;
	}
	return 0;
}

/*
 * Name every block below nblk that this update has not already named,
 * as §2.7's extent-map slot rule requires of a commit that changes
 * emapslot — holes included, a hole named as grain 0 with the zero
 * digest §2.4 requires for its length.  Without the naming the
 * zeroing turns those blocks into holes; without the zeroing the new
 * owner inherits the previous owner's grain numbers.
 */
static int
nameall(Upd *u, Omap *mold)
{
	uvlong i;
	ulong k, g;
	uchar dig[Blkdlen], *d;
	int named;

	for(i = 0; i < u->nblk; i++){
		named = 0;
		for(k = 0; k < u->nmap; k++)
			if(u->map[k].blk == i){
				named = 1;
				break;
			}
		if(named)
			continue;
		g = i < u->oldnblk ? mapgrain(mold, i) : 0;
		if(g != 0 && (d = mapdig(mold, i)) != nil)
			memmove(dig, d, Blkdlen);
		else
			zerodigest(u->s, u->newlen, i, dig);
		if(addmap(u, i, g, dig) < 0)
			return -1;
	}
	return 0;
}

/*
 * Re-hash one block whose covered length this update changes although
 * the update names no byte of it (§4): layer-a §1.4 hashes the final
 * partial block over its actual length, so a change to len changes
 * that block's digest without a byte of the write touching it.
 *
 * Growing the coverage also changes what the block *reads* as, and
 * this is where the grain is brought back into line with it: the bytes
 * a truncate left above the old len are not the object's content and
 * §4 requires the extension that covers them to read as zeros, so the
 * block is composed afresh, into a fresh grain — the old one still
 * carries the published state until this commit is durable (§3.5).
 * Shrinking changes no byte the store may ever serve again, so it is a
 * re-hash and nothing more, which is what keeps a truncate a
 * space-freeing commit that §6's reserved tail can carry.
 */
static int
reblk(Upd *u, Omap *mold, ulong blk, uchar *buf)
{
	Store *s;
	ulong oldn, newn, k, g, ng;
	uchar dig[Blkdlen];

	s = u->s;
	if(blk >= u->nblk)
		return 0;
	g = blk < u->oldnblk ? mapgrain(mold, blk) : 0;
	if(g == 0)
		return 0;			/* clause 4 recomputes a hole */
	for(k = 0; k < u->nmap; k++)
		if(u->map[k].blk == blk)
			return 0;		/* already named */
	oldn = blkcover(s, u->e.len, blk);
	newn = blkcover(s, u->newlen, blk);
	if(oldn == newn)
		return 0;
	if(readblk(s, mold, blk, buf, u->e.len) < 0)
		return -1;
	blkdigest(buf, newn, dig);
	if(newn < oldn)
		return addmap(u, blk, g, dig);
	qlock(&s->qlstate);
	/* grainalloc spells its own failure: disk full, or out of memory */
	if(grainalloc(s, &ng) < 0){
		qunlock(&s->qlstate);
		return -1;
	}
	qunlock(&s->qlstate);
	if(grainwrite(s, buf, ng) < 0){
		qlock(&s->qlstate);
		grainstageclr(s, ng);
		qunlock(&s->qlstate);
		return -1;
	}
	/* the same window as stageblk's, and released the same way */
	if(addfree(u, g) < 0 || addmap(u, blk, ng, dig) < 0){
		qlock(&s->qlstate);
		grainstageclr(s, ng);
		qunlock(&s->qlstate);
		return -1;
	}
	return 0;
}

/*
 * The blocks whose covered length an update can change without naming
 * them are the block holding the old len and the block holding the new
 * one: an extend across a boundary makes the old final block whole and
 * a truncate makes the new final block short.  Everything between them
 * is either wholly covered both times or cleared by clause 5.  Naming
 * only the block that holds the *new* len publishes a csum that is not
 * the csum of the content whenever an object grows past a partial
 * final block — the map and the csum agree with each other and
 * disagree with the bytes, so verify reports arraybad=0 with one bad
 * block and §8 sends the object to a block repair it does not need.
 */
static int
relast(Upd *u, Omap *mold, uchar *buf)
{
	if(u->oldnblk > 0 && reblk(u, mold, u->oldnblk - 1, buf) < 0)
		return -1;
	if(u->nblk > 0 && u->nblk != u->oldnblk
	&& reblk(u, mold, u->nblk - 1, buf) < 0)
		return -1;
	return 0;
}

/* free every grain at or beyond the new nblk (§2.4, §4) */
static int
freetail(Upd *u, Omap *mold)
{
	uvlong i;

	for(i = u->nblk; i < u->oldnblk; i++)
		if(addfree(u, mapgrain(mold, i)) < 0)
			return -1;
	return 0;
}

int
objstat(Store *s, uchar *oid, int oidlen, Objinfo *oi)
{
	long slot;
	Ient *e;

	if(!serving(s))
		return -1;
	qlock(&s->qlstate);
	if((slot = ientfind(s, oid, oidlen)) < 0){
		qunlock(&s->qlstate);
		werrstr("no such object");
		return -1;
	}
	e = &s->idx[slot];
	memset(oi, 0, sizeof *oi);
	oi->slot = slot;
	oi->emapslot = e->emapslot;
	oi->qidpath = e->qidpath;
	oi->len = e->len;
	oi->ver = e->ver;
	oi->wepoch = e->wepoch;
	oi->mtime = e->mtime;
	memmove(oi->csum, e->csum, Csumlen);
	oi->state = e->state;
	oi->corrupt = (e->flags & Icorrupt) != 0;
	qunlock(&s->qlstate);
	return 0;
}

int
objcreate(Store *s, uchar *oid, int oidlen, uvlong ver, uvlong wepoch,
	Dirtyrec *dr, int ndr, Objinfo *oi)
{
	Upd u;
	Ient *e;
	long slot;
	uvlong qid;
	int reuse;

	/*
	 * First, like every other mutating entry point: a condemned store
	 * answers nothing (§3.2), and without this a create would read
	 * s->idx below and answer `object exists' — a §2.6 wire error —
	 * out of memory the store itself has declared untrustworthy.
	 */
	if(!serving(s))
		return -1;
	if(oidlen < 1 || oidlen > Oidmax){
		werrstr("bad object name: oid length %d", oidlen);
		return -1;
	}
	/*
	 * layer-a §1.3 forbids the key: ver starts at 1 and absence is not
	 * (0, 0).  Unlike stagefinal's, this refusal is §3.7's internal
	 * kind and carries no §2.6 prefix — a client create's version is
	 * this instance's own to choose (layer-a §5.4 step 3), and the
	 * op=create receiver arbitrates rather than calling here (§3.6) —
	 * so a version of 0 on this path is a caller bug.
	 */
	if(ver == 0){
		werrstr("create at version 0");
		return -1;
	}
	memset(&u, 0, sizeof u);
	u.s = s;
	qlock(&s->qlstate);
	slot = ientfind(s, oid, oidlen);
	reuse = 0;
	if(slot >= 0){
		e = &s->idx[slot];
		if(e->state == Slive){
			qunlock(&s->qlstate);
			werrstr("object exists");
			return -1;
		}
		/*
		 * §2.3: a create over an existing tombstone reuses the
		 * tombstone's slot *and* its qidpath, which is what
		 * layer-a §2.3 means by stable across delete, tombstone
		 * and re-create.  A tombstone holds no content, no
		 * extent-map slot and no grain, so there is nothing here
		 * to free.
		 */
		/*
		 * layer-a §1.5: the fresh object's ver is one greater than
		 * the tombstone's, so as long as the tombstone exists no
		 * older copy can outrank the new object.  Taking the
		 * caller's value would lose exactly the guarantee the rule
		 * is for: a straggler still holding the tombstone at
		 * (1, 6) outranks a live object created at (1, 2) and
		 * re-deletes it.  The decision is made here, under
		 * qlstate, because that is where the tombstone's own key
		 * is known not to be racing a commit.
		 *
		 * Like the ver==0 refusal above, this is §3.7's internal
		 * kind and carries no §2.6 prefix: objcreate is the client
		 * create path (§3.6), on which the version is this
		 * instance's own to choose (layer-a §5.4 step 3) — chosen
		 * by the rule this branch enforces — so any other value is
		 * a caller bug.  The op=create receiver arbitrates before
		 * calling here (§3.6), and an op=full over a tombstone
		 * arbitrates in stagefinal, where the refusal is §2.6's
		 * `stale version'.
		 */
		if(ver != e->ver + 1 || wepoch < e->wepoch){
			qunlock(&s->qlstate);
			werrstr("create at (%llud, %llud) over a tombstone "
				"at (%llud, %llud)", wepoch, ver, e->wepoch,
				e->ver);
			return -1;
		}
		reuse = 1;
		u.slot = slot;
		u.e = *e;
	}else{
		if(slotalloc(s, &u.slot) < 0){
			qunlock(&s->qlstate);
			werrstr("disk full");
			return -1;
		}
		u.slotresv = 1;
		memset(&u.e, 0, sizeof u.e);
	}
	qunlock(&s->qlstate);
	if((u.e.oid = malloc(oidlen)) == nil){
		qlock(&s->qlstate);
		if(u.slotresv)
			slotresvclr(s, u.slot);
		qunlock(&s->qlstate);
		werrstr("out of memory");
		return -1;
	}
	memmove(u.e.oid, oid, oidlen);
	u.e.oidlen = oidlen;
	if(!reuse){
		if((qid = qidalloc(s)) == 0){
			qlock(&s->qlstate);
			slotresvclr(s, u.slot);
			qunlock(&s->qlstate);
			free(u.e.oid);
			werrstr("qid.path: %r");
			return -1;
		}
		u.e.qidpath = qid;
	}
	u.newlen = 0;
	u.nblk = 0;
	u.oldnblk = 0;
	u.newslot = 0;
	u.oslot = 0;
	if(updcommit(&u, Slive, ver, wepoch, time(nil), 0, dr, ndr) < 0){
		updclose(&u);
		return -1;
	}
	updclose(&u);
	if(oi != nil)
		return objstat(s, oid, oidlen, oi);
	return 0;
}

int
objwrite(Store *s, uchar *oid, int oidlen, void *a, long n, uvlong off,
	uvlong ver, uvlong wepoch, Dirtyrec *dr, int ndr)
{
	Upd u;
	Omap mold;
	Objinfo oi;
	uchar *buf, *src;
	uvlong newlen;
	ulong blk, boff, bn;
	long left;

	if(n < 0){
		werrstr("negative write");
		return -1;
	}
	/*
	 * R10 and layer-a §1.2: a write whose offset+count would exceed
	 * objmax MUST fail `object too large', and MUST NOT be silently
	 * truncated.  The bound is a difference and not a sum because off
	 * is a client's or a peer's u64 and off+n wraps: at off = 2^64-4
	 * and n = 4 the sum is 0, which passes every later test and stages
	 * a block index of 4.5e15 into a record the commit path then has
	 * to refuse.
	 */
	if(off > s->sb.objmax || (uvlong)n > s->sb.objmax - off){
		werrstr("object too large");
		return -1;
	}
	/* objcreate's rule: a publish at version 0 is a caller bug */
	if(ver == 0){
		werrstr("write at version 0");
		return -1;
	}
	if(objstat(s, oid, oidlen, &oi) < 0)
		return -1;
	if(oi.state != Slive){
		werrstr(oi.state == Stomb ? "object deleted" : "no such object");
		return -1;
	}
	/*
	 * §8's refusal, made here rather than left to updopen because it
	 * MUST precede the count-0 shortcut below: a zero-count write
	 * commits nothing and would otherwise answer ok on a copy that
	 * fails verification, which is client access served.
	 */
	if(oi.corrupt){
		werrstr("checksum mismatch: slot %lud, corrupt flag set",
			oi.slot);
		return -1;
	}
	/*
	 * layer-a §2.4 extends an object at "a write at offset > len" —
	 * bytes landing above it.  A count of zero lands none, and 9P
	 * clients issue count-0 Twrites, so taking one as an extend would
	 * resize the object, re-derive csum, bump ver and commit a record
	 * for a call that wrote nothing.  A replica that took the
	 * zero-count write would then sit at a different len from one
	 * that did not, at the same key: layer-a §1.3's I3 through a
	 * legal client call.  The existence and bounds tests above still
	 * run, so a count-0 write to a tombstone or past objmax fails as
	 * it should.
	 */
	if(n == 0)
		return 0;
	newlen = oi.len;
	if(off + n > newlen)
		newlen = off + n;
	if(updopen(&u, s, oid, oidlen, newlen, 0) < 0)
		return -1;
	mapopen(s, &mold, &u.e, u.cold);
	if((buf = malloc(s->sb.blksz)) == nil){
		updabort(&u);
		updclose(&u);
		werrstr("out of memory");
		return -1;
	}
	src = a;
	left = n;
	blk = off / s->sb.blksz;
	boff = off % s->sb.blksz;
	while(left > 0){
		bn = s->sb.blksz - boff;
		if((long)bn > left)
			bn = left;
		if(stageblk(&u, &mold, blk, src, boff, bn, buf) < 0){
			free(buf);
			updabort(&u);
			updclose(&u);
			return -1;
		}
		src += bn;
		left -= bn;
		blk++;
		boff = 0;
	}
	devpoint(s->d, "stage", 0);
	if(relast(&u, &mold, buf) < 0
	|| (u.oslot && nameall(&u, &mold) < 0)){
		free(buf);
		updabort(&u);
		updclose(&u);
		return -1;
	}
	free(buf);
	if(updcommit(&u, Slive, ver, wepoch, time(nil), 0, dr, ndr) < 0){
		updclose(&u);
		return -1;
	}
	updclose(&u);
	return 0;
}

int
objtrunc(Store *s, uchar *oid, int oidlen, uvlong len, uvlong ver,
	uvlong wepoch, Dirtyrec *dr, int ndr)
{
	Upd u;
	Omap mold;
	uchar *buf;

	if(!serving(s))
		return -1;
	if(len > s->sb.objmax){
		werrstr("object too large");
		return -1;
	}
	/* objcreate's rule: a publish at version 0 is a caller bug */
	if(ver == 0){
		werrstr("truncate at version 0");
		return -1;
	}
	if(updopen(&u, s, oid, oidlen, len, 0) < 0)
		return -1;
	mapopen(s, &mold, &u.e, u.cold);
	if((buf = malloc(s->sb.blksz)) == nil){
		updabort(&u);
		updclose(&u);
		werrstr("out of memory");
		return -1;
	}
	if(freetail(&u, &mold) < 0
	|| relast(&u, &mold, buf) < 0
	|| (u.oslot && nameall(&u, &mold) < 0)){
		free(buf);
		updabort(&u);
		updclose(&u);
		return -1;
	}
	free(buf);
	if(updcommit(&u, Slive, ver, wepoch, time(nil), 0, dr, ndr) < 0){
		updclose(&u);
		return -1;
	}
	updclose(&u);
	return 0;
}

/*
 * A tombstone is metadata (R8): state=tomb, len=0, content released,
 * key bumped by the caller, mtime retained for the tombdays test.  It
 * holds no extent-map slot, so the slot is released with the content.
 */
int
objremove(Store *s, uchar *oid, int oidlen, uvlong ver, uvlong wepoch,
	Dirtyrec *dr, int ndr)
{
	Upd u;
	Omap mold;

	if(!serving(s))
		return -1;
	/*
	 * objcreate's rule, and here it is the sharp one: a delete bumps
	 * (wepoch, ver) like any write (layer-a §1.5), so a tombstone's
	 * ver is always >= 2 — a tombstone published at (E, 0) forces the
	 * re-create to ver 1, and a straggler live copy at (E, 1) with
	 * different content then ties it (layer-a §1.3's I3).
	 */
	if(ver == 0){
		werrstr("delete at version 0");
		return -1;
	}
	/*
	 * §8: a delete applies to a corrupt-flagged copy.  op=delete is
	 * self-contained — it arbitrates on the key it carries and
	 * replaces the content with none — so there is nothing here for
	 * the flag to protect, and the tombstone this commits holds no
	 * content to be suspect of: updcommit publishes it with the flag
	 * clear, which is what takes the object out of /lost.  A slot §5
	 * step 10 condemned is not in this set: its grains are named by
	 * the damaged map alone, so a delete would leak every one of them
	 * (§3.6), and op=full remains its only repair.
	 */
	if(updopen(&u, s, oid, oidlen, 0, Ucorrupt) < 0)
		return -1;
	mapopen(s, &mold, &u.e, u.cold);
	if(freetail(&u, &mold) < 0){
		updabort(&u);
		updclose(&u);
		return -1;
	}
	if(u.e.emapslot == 0 && u.e.grain0 != 0 && addfree(&u, u.e.grain0) < 0){
		updabort(&u);
		updclose(&u);
		return -1;
	}
	if(updcommit(&u, Stomb, ver, wepoch, time(nil), 0, dr, ndr) < 0){
		updclose(&u);
		return -1;
	}
	updclose(&u);
	return 0;
}

/*
 * §8's key-preserving commit: an Eobj that changes nothing but the
 * corrupt flag.  The flag is durable so a restart does not forget it,
 * and §2.7's oflags is where the record carries it — an apply that
 * kept the entry's own flag would not be a function of the record
 * alone, and replay would clear what a scrub had found.
 */
int
objcorrupt(Store *s, uchar *oid, int oidlen, int set, Dirtyrec *dr, int ndr)
{
	Upd u;
	Omap mold;
	Objinfo oi;

	if(objstat(s, oid, oidlen, &oi) < 0)
		return -1;
	if(updopen(&u, s, oid, oidlen, oi.len, Utomb|Ucorrupt) < 0)
		return -1;
	mapopen(s, &mold, &u.e, u.cold);
	/*
	 * §2.7's slot rule is what makes a commit name every block, and
	 * this commit changes no emapslot: clause 2 zeroes nothing, so
	 * the map the apply inherits is this object's own and an empty
	 * nmap leaves it exactly as it is.  Naming them anyway would put
	 * nblkmax map triples — 28.2 KiB at the defaults — into a record
	 * §8 describes as "an Eobj that changes nothing but the corrupt
	 * flag".
	 */
	if(u.oslot && nameall(&u, &mold) < 0){
		updabort(&u);
		updclose(&u);
		return -1;
	}
	if(updcommit(&u, u.e.state, u.e.ver, u.e.wepoch, u.e.mtime, set,
		dr, ndr) < 0){
		updclose(&u);
		return -1;
	}
	updclose(&u);
	return 0;
}

/*
 * layer-a §1.5's discard.  The primary establishes the cluster-wide
 * conditions; the receiver re-checks, locally, the two that bear on
 * its own safety — its record is a tombstone whose key is exactly the
 * one the discard names, and that tombstone's wepoch is strictly
 * below the receiver's current map epoch.  Both checks are made here,
 * inside the call and under one hold of qlstate, because a separate
 * objstat is a second read of a record an op=delete can replace in
 * between: the tombstone the caller inspected is then not the one
 * dropped, and the replacement goes unconfirmed — §1.5's resurrection
 * hole, through the API.  Once the checks hold, the tombstone's index
 * slot returns to the free list: one Eslot, and §3.5 is what keeps
 * the slot out of the allocator until the commit that freed it is
 * durable.
 */
int
objdiscard(Store *s, uchar *oid, int oidlen, uvlong ver, uvlong wepoch,
	uvlong epoch)
{
	Item it;
	Ient *e;
	long slot;

	if(!serving(s))
		return -1;
	qlock(&s->qlstate);
	if((slot = ientfind(s, oid, oidlen)) < 0){
		qunlock(&s->qlstate);
		/* an absent id is §5.6's `no such object', not check (i); §3.7 */
		werrstr("no such object");
		return -1;
	}
	e = &s->idx[slot];
	if(e->state != Stomb){
		qunlock(&s->qlstate);
		werrstr("not discardable: not a tombstone");
		return -1;
	}
	if(e->wepoch != wepoch || e->ver != ver){
		qunlock(&s->qlstate);
		werrstr("not discardable: tombstone at (%llud, %llud), "
			"discard names (%llud, %llud)", e->wepoch, e->ver,
			wepoch, ver);
		return -1;
	}
	if(e->wepoch >= epoch){
		qunlock(&s->qlstate);
		werrstr("not discardable: wepoch %llud not below epoch %llud",
			e->wepoch, epoch);
		return -1;
	}
	qunlock(&s->qlstate);
	memset(&it, 0, sizeof it);
	it.eslot = slot;
	it.haseslot = 1;
	it.freeing = 1;
	return logcommit(s, &it);
}

long
objread(Store *s, uchar *oid, int oidlen, void *a, long n, uvlong off)
{
	Omap m;
	Ient e;
	Emape *c;
	uchar *buf, *dst;
	long done, bn;
	uvlong len;
	ulong blk, boff, g;
	long slot;

	if(!serving(s))
		return -1;
	if(n < 0){
		werrstr("negative read");
		return -1;
	}
	qlock(&s->qlstate);
	if((slot = ientfind(s, oid, oidlen)) < 0 || s->idx[slot].state != Slive){
		werrstr(slot >= 0 && s->idx[slot].state == Stomb
			? "object deleted" : "no such object");
		qunlock(&s->qlstate);
		return -1;
	}
	/*
	 * §8: a copy whose corrupt flag is set fails client access with
	 * `checksum mismatch' (§3.7's row).  The flag says a block does
	 * not hash to its digest, so what this read would return is the
	 * damaged bytes — the one thing layer-a §1.4's checksums exist to
	 * stop — and a corrupt copy is out of arbitration anyway (§1.3),
	 * so there is no caller left that wants them.
	 */
	if(s->idx[slot].flags & Icorrupt){
		qunlock(&s->qlstate);
		werrstr("checksum mismatch: slot %lud, corrupt flag set",
			slot);
		return -1;
	}
	e = s->idx[slot];
	e.oid = nil;
	qunlock(&s->qlstate);
	len = e.len;
	if(off >= len)
		return 0;
	if((uvlong)n > len - off)
		n = len - off;
	c = nil;
	if(e.emapslot != 0 && (c = mapread(s, slot, e.emapslot)) == nil)
		return -1;
	mapopen(s, &m, &e, c);
	if((buf = malloc(s->sb.blksz)) == nil){
		emapunpin(s, c);
		werrstr("out of memory");
		return -1;
	}
	dst = a;
	done = 0;
	blk = off / s->sb.blksz;
	boff = off % s->sb.blksz;
	while(done < n){
		bn = s->sb.blksz - boff;
		if(bn > n - done)
			bn = n - done;
		g = mapgrain(&m, blk);
		if(g == 0)
			memset(dst + done, 0, bn);	/* R13: a hole reads zero */
		else{
			if(g >= s->sb.ngrains){
				werrstr("grain %lud out of range", g);
				free(buf);
				emapunpin(s, c);
				return -1;
			}
			if(grainread(s, buf, g) < 0){
				free(buf);
				emapunpin(s, c);
				return -1;
			}
			memmove(dst + done, buf + boff, bn);
		}
		done += bn;
		blk++;
		boff = 0;
	}
	free(buf);
	emapunpin(s, c);
	return done;
}

/*
 * §8's verify.  It answers the *set* of mismatching block indices,
 * not a boolean, because that set is what makes partial repair
 * possible — and it answers separately whether the digest array
 * itself is suspect, because the two need different repairs: a block
 * whose bytes disagree with a good digest array is repaired block by
 * block, while hash(dig[]) != csum means the stored dig[i] cannot be
 * the acceptance test for anything and the repair is layer-a §1.3's
 * whole-object push at an equal key.
 */
int
objverify(Store *s, uchar *oid, int oidlen, Vfy *v)
{
	Omap m;
	Ient e;
	Emape *c;
	uchar *buf, *digs, dig[Blkdlen], csum[Csumlen];
	uvlong i, nblk;
	ulong n, g;
	long slot;

	memset(v, 0, sizeof *v);
	if(!serving(s))
		return -1;
	qlock(&s->qlstate);
	if((slot = ientfind(s, oid, oidlen)) < 0){
		qunlock(&s->qlstate);
		werrstr("no such object");
		return -1;
	}
	e = s->idx[slot];
	e.oid = nil;
	qunlock(&s->qlstate);
	nblk = blkcount(e.len, s->sb.blksz);
	c = nil;
	if(e.emapslot != 0 && (c = mapread(s, slot, e.emapslot)) == nil)
		return -1;
	mapopen(s, &m, &e, c);
	buf = malloc(s->sb.blksz);
	digs = nblk > 0 ? malloc(nblk*Blkdlen) : nil;
	if(buf == nil || (nblk > 0 && digs == nil)){
		free(buf);
		free(digs);
		emapunpin(s, c);
		werrstr("out of memory");
		return -1;
	}
	if(nblk > 0 && (v->bad = malloc(nblk*sizeof *v->bad)) == nil){
		free(buf);
		free(digs);
		emapunpin(s, c);
		werrstr("out of memory");
		return -1;
	}
	for(i = 0; i < nblk; i++){
		memmove(digs + i*Blkdlen, mapdig(&m, i), Blkdlen);
		n = s->sb.blksz;
		if((i+1)*(uvlong)s->sb.blksz > e.len)
			n = e.len - i*(uvlong)s->sb.blksz;
		g = mapgrain(&m, i);
		if(g == 0){
			/* a hole is verified against the zero digest */
			zerodigest(s, e.len, i, dig);
		}else{
			if(grainread(s, buf, g) < 0){
				free(buf);
				free(digs);
				emapunpin(s, c);
				return -1;
			}
			blkdigest(buf, n, dig);
		}
		if(memcmp(dig, digs + i*Blkdlen, Blkdlen) != 0)
			v->bad[v->nbad++] = i;
	}
	csumdigests(digs, nblk, csum);
	v->arraybad = memcmp(csum, e.csum, Csumlen) != 0;
	free(buf);
	free(digs);
	emapunpin(s, c);
	return 0;
}

void
vfyfree(Vfy *v)
{
	free(v->bad);
	v->bad = nil;
	v->nbad = 0;
}

/*
 * Multi-request op=full stages, §3.6.  layer-a §5.5 stages a whole
 * object across many writes, pipelined, with the arbitration
 * comparison made once at final=1 against the receiver's then-current
 * key — so the object is explicitly not held across the transfer, and
 * the stage outlives the request that created it.  That is the one
 * stage in this design with a lifetime longer than one operation, and
 * it gets an explicit one: clunk, flush, stagems of silence, and
 * restart, which is free because nothing about a stage is durable.
 */
Stage*
stageopen(Store *s, uchar *oid, int oidlen, uvlong len, int force)
{
	Stage *g;
	uvlong nblk;

	if(oidlen < 1 || oidlen > Oidmax){
		werrstr("bad object name: oid length %d", oidlen);
		return nil;
	}
	if(len > s->sb.objmax){
		werrstr("object too large");
		return nil;
	}
	nblk = blkcount(len, s->sb.blksz);
	if((g = mallocz(sizeof *g, 1)) == nil){
		werrstr("out of memory");
		return nil;
	}
	g->s = s;
	memmove(g->oid, oid, oidlen);
	g->oidlen = oidlen;
	g->len = len;
	g->force = force;
	g->nblk = nblk;
	g->last = nsec();
	if(nblk > 0){
		g->grain = mallocz(nblk*sizeof *g->grain, 1);
		g->dig = mallocz(nblk*Blkdlen, 1);
		if(g->grain == nil || g->dig == nil){
			free(g->grain);
			free(g->dig);
			free(g);
			werrstr("out of memory");
			return nil;
		}
	}
	qlock(&s->qlstate);
	g->next = s->stages;
	s->stages = g;
	qunlock(&s->qlstate);
	return g;
}

static int
stagewrite1(Stage *g, void *a, long n, uvlong off)
{
	Store *s;
	uchar *buf, *src;
	ulong blk, boff, bn, gr, k;
	long left;

	s = g->s;
	if(!serving(s))
		return -1;
	/*
	 * §3.6: off is a peer's u64 straight off a /repl fid, so the
	 * bound is a difference — off+n wraps at off = 2^64-4, and the
	 * grain array this indexes has g->nblk elements and no more.  A
	 * negative count is refused in its own right rather than by that
	 * same accident.
	 */
	if(n < 0){
		werrstr("negative chunk");
		return -1;
	}
	if(off > g->len || (uvlong)n > g->len - off){
		werrstr("bad ctl: chunk past the declared length");
		return -1;
	}
	if((buf = malloc(s->sb.blksz)) == nil){
		werrstr("out of memory");
		return -1;
	}
	src = a;
	left = n;
	blk = off / s->sb.blksz;
	boff = off % s->sb.blksz;
	while(left > 0){
		bn = s->sb.blksz - boff;
		if((long)bn > left)
			bn = left;
		if(boff == 0 && bn == s->sb.blksz)
			memmove(buf, src, bn);
		else{
			memset(buf, 0, s->sb.blksz);
			if(g->grain[blk] != 0
			&& grainread(s, buf, g->grain[blk]) < 0){
				free(buf);
				return -1;
			}
			memmove(buf + boff, src, bn);
		}
		qlock(&s->qlstate);
		if(g->ngrain >= s->cfg.stagemax
		|| s->nstagegrain >= s->cfg.stagetot){
			qunlock(&s->qlstate);
			free(buf);
			werrstr("disk full");
			return -1;
		}
		/* grainalloc spells its own failure: disk full, or OOM */
		if(grainalloc(s, &gr) < 0){
			qunlock(&s->qlstate);
			free(buf);
			return -1;
		}
		qunlock(&s->qlstate);
		/*
		 * §3.6's two bounds are charged, and the block's previous
		 * grain released, only once the replacement is on the
		 * platter.  A chunk that fails — `interrupted' is an
		 * ordinary outcome here (§0) — must leave the handle exactly
		 * as it found it: a charge the discard cannot see (it counts
		 * g->grain[i], and this block's is still 0) would be
		 * permanent, and a release before the write would leave the
		 * handle naming a grain the allocator has taken back.
		 */
		if(grainwrite(s, buf, gr) < 0){
			qlock(&s->qlstate);
			grainstageclr(s, gr);
			qunlock(&s->qlstate);
			free(buf);
			return -1;
		}
		qlock(&s->qlstate);
		if(g->grain[blk] != 0)
			grainstageclr(s, g->grain[blk]);
		else{
			g->ngrain++;
			s->nstagegrain++;
		}
		qunlock(&s->qlstate);
		g->grain[blk] = gr;
		k = s->sb.blksz;
		if((uvlong)(blk+1)*s->sb.blksz > g->len)
			k = g->len - (uvlong)blk*s->sb.blksz;
		blkdigest(buf, k, g->dig + blk*Blkdlen);
		src += bn;
		left -= bn;
		blk++;
		boff = 0;
	}
	free(buf);
	devpoint(s->d, "stage", 0);
	return 0;
}

/*
 * §3.6's sweep trigger is "no chunk has *arrived* for stagems", so the
 * clock is refreshed at entry, under qlstate, and busy pins the handle
 * for the chunk's whole flight: the grain I/O above runs with qlstate
 * released, and a sweep firing in one of those windows would return
 * every grain the handle names to the allocator and strip the arrays
 * the write is still indexing.  A handle the sweep has already
 * stripped is spent — the chunks before this one are gone, so carrying
 * on would publish holes in their place at final=1 — and the refusal
 * tells the owner to discard it and restart the transfer, which is
 * free (§3.6).
 */
int
stagewrite(Stage *g, void *a, long n, uvlong off)
{
	Store *s;
	int r;

	s = g->s;
	qlock(&s->qlstate);
	if(g->dead){
		qunlock(&s->qlstate);
		werrstr("stage expired");
		return -1;
	}
	g->busy = 1;
	g->last = nsec();
	qunlock(&s->qlstate);
	r = stagewrite1(g, a, n, off);
	qlock(&s->qlstate);
	g->busy = 0;
	g->last = nsec();
	qunlock(&s->qlstate);
	return r;
}

/* caller holds qlstate */
static void
stageunlink(Store *s, Stage *g)
{
	Stage **pp, *t;

	for(pp = &s->stages; (t = *pp) != nil; pp = &t->next)
		if(t == g){
			*pp = t->next;
			break;
		}
}

void
stagediscard(Stage *g)
{
	Store *s;
	uvlong i;

	if(g == nil)
		return;
	s = g->s;
	/*
	 * Unlink first, and tolerate a handle that is already off the
	 * list: stagesweep strips an expired handle — releases its
	 * grains, zeroes its entries and unlinks it — but the memory
	 * stays the owner's, so the discard the clunk or flush issues
	 * afterwards finds nothing left to release and only frees.
	 * That is what keeps the sweep and a clunk from releasing the
	 * same reservation twice — a double grainstageclr removes
	 * whatever reservation the allocator has since handed out.
	 */
	qlock(&s->qlstate);
	stageunlink(s, g);
	for(i = 0; i < g->nblk; i++)
		if(g->grain[i] != 0){
			grainstageclr(s, g->grain[i]);
			s->nstagegrain--;
		}
	qunlock(&s->qlstate);
	free(g->grain);
	free(g->dig);
	free(g);
}

/*
 * final=1: take the object's ordering point, commit one Eobj naming
 * every staged block and freeing every grain the object held before.
 * The stage's grains move from the staged set to the bitmap in that
 * commit's apply, so nothing about the transfer was ever durable
 * until this moment.
 */
/* layer-a §1.3's arbitration key, compared lexicographically */
static int
keycmp(uvlong we, uvlong ver, uvlong we2, uvlong ver2)
{
	if(we != we2)
		return we < we2 ? -1 : 1;
	if(ver != ver2)
		return ver < ver2 ? -1 : 1;
	return 0;
}

/*
 * Every exit from stagefinal releases the stage, which is what §3.6
 * means by "the stage is discarded exactly as below": a comparison
 * that refuses the push has ended the transfer, and so has a commit
 * that could not be made, so the handle is spent either way and its
 * reservations must not outlive it.  The caller's error is preserved
 * across the release.  A grain the handle has already handed to the
 * update (below) is not the handle's any more, so the release here
 * releases only what the handle still owns.
 */
static int
stagefail(Stage *g)
{
	char e[ERRMAX];

	rerrstr(e, sizeof e);
	stagediscard(g);
	werrstr("%s", e);
	return -1;
}

/*
 * Hand the stage's grains below lim to the update whose map now names
 * them: from this moment updabort is their releaser, so the handle
 * stops counting them and stops naming them.  Ownership must transfer
 * exactly once — after updabort has released a grain, the allocator
 * may hand it to another proc's stage at any moment, and a second
 * grainstageclr from stagediscard would remove *that* reservation:
 * two objects sharing a grain, undetectable by arbitration.
 *
 * The nstagegrain charge is dropped here, but the grains stay in the
 * staged *set* until the commit's apply moves them to the bitmap — so
 * between handoff and apply the counter under-counts the set by this
 * stage's grains, and concurrent chunks can briefly push the set past
 * stagetot by that amount.  Deliberate: §3.6's bound is back-pressure
 * on reservations, and these grains are still reserved either way, so
 * there is no reuse risk — only a bound read low for the moments a
 * commit is in flight.
 */
static void
stagehandoff(Stage *g, uvlong lim)
{
	Store *s;
	uvlong i;

	s = g->s;
	qlock(&s->qlstate);
	for(i = 0; i < lim; i++)
		if(g->grain[i] != 0){
			g->grain[i] = 0;
			s->nstagegrain--;
		}
	qunlock(&s->qlstate);
}

int
stagefinal(Stage *g, uvlong ver, uvlong wepoch, Dirtyrec *dr, int ndr)
{
	Store *s;
	Upd u;
	Omap mold;
	uvlong i;
	uchar dig[Blkdlen];
	long slot;
	int absent, nokey, c;

	s = g->s;
	/*
	 * The handle leaves s->stages before anything here drops qlstate:
	 * stagesweep strips any handle whose last chunk arrived more than
	 * stagems ago, and a final=1 parked in the commit — waiting on a
	 * checkpoint, say — gets older than stagems by nothing more than
	 * bad luck.  Swept mid-commit, the stage's grains would return to
	 * the allocator while the commit was about to publish them.  §3.6:
	 * the sweep's triggers are for a stage whose final=1 has not been
	 * attempted, and for no other.  stagediscard's unlink of an
	 * already-unlinked handle is a harmless no-op.
	 */
	qlock(&s->qlstate);
	stageunlink(s, g);
	if(g->dead){
		/*
		 * The sweep stripped this handle: its chunks are gone, so
		 * committing it would publish holes in their place.  The
		 * handle is spent like any other final=1 outcome — the
		 * discard below finds nothing to release and frees it.
		 */
		qunlock(&s->qlstate);
		werrstr("stage expired");
		return stagefail(g);
	}
	qunlock(&s->qlstate);
	if(!serving(s))
		return stagefail(g);
	/*
	 * layer-a §1.3: ver starts at 1 on create, and absence is not
	 * (0, 0) — so a live object at version 0 is a key the contract
	 * says cannot exist.  Nothing further down would refuse one: the
	 * comparison is skipped entirely for a receiver with no key to
	 * defend, which is both of §3.6's cases, so an absent or corrupt
	 * copy would take the push and be published at (wepoch, 0).  On
	 * the wire the version comes out of the op=full header, so a
	 * value the model forbids is a malformed header — layer-a §5.5's
	 * common set, `bad ctl'.
	 */
	if(ver == 0){
		werrstr("bad ctl: op=full at version 0");
		return stagefail(g);
	}
	qlock(&s->qlstate);
	slot = ientfind(s, g->oid, g->oidlen);
	absent = slot < 0 || s->idx[slot].state == Sfree;
	qunlock(&s->qlstate);
	if(absent){
		if(updnew(&u, s, g->oid, g->oidlen, g->len) < 0)
			return stagefail(g);
	}else if(updopen(&u, s, g->oid, g->oidlen, g->len,
		Utomb|Ubad|Ucorrupt) < 0)
		return stagefail(g);
	nokey = !absent && ((u.e.flags & Icorrupt) != 0 || u.e.bad);
	/*
	 * layer-a §5.5's comparison, made once here and against the
	 * receiver's then-current key: strictly greater, or equal with
	 * force=1, which is §1.3's divergence repair.  Earlier chunks
	 * stage without comparing, and a concurrent local update between
	 * chunks is what this exists to catch.
	 *
	 * D14 is the third case.  A copy that fails local verification
	 * contributes no key (§1.3) — on the wire it is §5.6's corrupt=1
	 * meta response — so it has no key to defend and the push
	 * applies at any key, greater, equal or lower.  Without the
	 * exemption a holder that committed (E, ver+1) and then lost the
	 * content to a media fault refuses the serving primary's repair
	 * push at the lower (E, ver) as `stale version', by the very
	 * copy that asked for it, and is unrepairable for the life of
	 * the disk.
	 *
	 * The commit clears the flag.  Every block this record names was
	 * staged from bytes the sender's dcsum covered and the whole
	 * against its csum (§5.5), and the digests were computed from
	 * those bytes here, so the verify §8 would run next finds every
	 * block matching by construction: there is nothing left for the
	 * flag to describe.  Leaving it set would keep a copy that is now
	 * whole out of arbitration, and — because a copy with no key to
	 * defend takes any push — would go on accepting a push at any key
	 * until the scrubber's next pass, which is days (§8).
	 */
	if(!absent && !nokey){
		c = keycmp(wepoch, ver, u.e.wepoch, u.e.ver);
		if(c < 0 || (c == 0 && !g->force)){
			updabort(&u);
			updclose(&u);
			werrstr("stale version");
			return stagefail(g);
		}
	}
	mapopen(s, &mold, &u.e, u.cold);
	for(i = 0; i < u.oldnblk; i++)
		if(addfree(&u, mapgrain(&mold, i)) < 0){
			updabort(&u);
			updclose(&u);
			return stagefail(g);
		}
	for(i = 0; i < g->nblk; i++){
		if(g->grain[i] != 0)
			memmove(dig, g->dig + i*Blkdlen, Blkdlen);
		else
			zerodigest(s, g->len, i, dig);
		if(addmap(&u, i, g->grain[i], dig) < 0){
			/*
			 * Grains below i are the update's — updabort releases
			 * them, once — and grains from i on are still the
			 * handle's, which stagefail's discard releases.
			 */
			stagehandoff(g, i);
			updabort(&u);
			updclose(&u);
			return stagefail(g);
		}
	}
	stagehandoff(g, g->nblk);
	if(updcommit(&u, Slive, ver, wepoch, time(nil), 0, dr, ndr) < 0){
		updclose(&u);
		return stagefail(g);
	}
	updclose(&u);
	/* nothing left in the handle: this frees it and releases no grain */
	stagediscard(g);
	return 0;
}

void
stagesweep(Store *s, vlong now)
{
	Stage *g, *next;
	uvlong i;

	qlock(&s->qlstate);
	for(g = s->stages; g != nil; g = next){
		next = g->next;
		/*
		 * §3.6: the trigger is a stage no chunk has *arrived* for in
		 * stagems.  busy is a chunk in flight right now — its arrival
		 * refreshed g->last at entry, but a chunk can be in flight
		 * longer than stagems, and sweeping under it frees grains a
		 * write is still filling.
		 */
		if(g->busy || now - g->last <= (vlong)s->cfg.stagems*1000000LL)
			continue;
		/*
		 * Strip the handle; never free it.  The memory is the owner's
		 * (store.h) — the /repl fid still holds the pointer, and its
		 * clunk's stagediscard is what frees it.  Freeing here is a
		 * use-after-free the moment the owner's next call arrives.
		 * The strip zeroes every grain entry as it releases it, so
		 * that later discard releases nothing twice; dead is what
		 * makes a later chunk or final=1 refuse instead of finishing
		 * a transfer whose earlier chunks are gone.
		 */
		stageunlink(s, g);
		for(i = 0; i < g->nblk; i++)
			if(g->grain[i] != 0){
				grainstageclr(s, g->grain[i]);
				g->grain[i] = 0;
				s->nstagegrain--;
			}
		g->ngrain = 0;
		g->dead = 1;
	}
	qunlock(&s->qlstate);
}
