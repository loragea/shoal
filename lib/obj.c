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
		if((m = realloc(u->map, u->amap*sizeof *m)) == nil)
			return -1;
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
		if((f = realloc(u->freed, u->afree*sizeof *f)) == nil)
			return -1;
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
	if((digs = malloc(u->nblk*Blkdlen)) == nil)
		return -1;
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
 * commit path refuses through the same flag (broken).
 */
static int
serving(Store *s)
{
	if(s->fatal){
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
 * class neither of them can carry on from, and devclass does not
 * return on it.
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
		werrstr("slot %lud: extent map failed its checksum", slot);
		return nil;
	}
	return c;
}

/*
 * Start an update of an existing object: take a copy of the entry,
 * work out which extent-map slot the new length needs, and pin both
 * the map being read and the map the apply will change.
 */
static int
updopen(Upd *u, Store *s, uchar *oid, int oidlen, uvlong newlen, int wanttomb)
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
	if(e->state != Slive && !(wanttomb && e->state == Stomb)){
		qunlock(&s->qlstate);
		werrstr("no such object");
		return -1;
	}
	u->slot = slot;
	u->e = *e;
	u->e.oid = malloc(e->oidlen);
	if(u->e.oid == nil){
		qunlock(&s->qlstate);
		return -1;
	}
	memmove(u->e.oid, e->oid, e->oidlen);
	u->newlen = newlen;
	u->nblk = blkcount(newlen, s->sb.blksz);
	u->oldnblk = blkcount(u->e.len, s->sb.blksz);
	u->newslot = u->e.emapslot;
	if(u->nblk > 1 && u->e.emapslot == 0){
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

	if(u->e.emapslot != 0
	&& (u->cold = mapread(s, u->slot, u->e.emapslot)) == nil){
		qlock(&s->qlstate);
		if(u->emapresv)
			emapresvclr(s, u->newslot);
		qunlock(&s->qlstate);
		free(u->e.oid);
		return -1;
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
	if(grainalloc(s, &g) < 0){
		qunlock(&s->qlstate);
		werrstr("disk full");
		return -1;
	}
	qunlock(&s->qlstate);
	if(grainwrite(s, buf, g) < 0){
		qlock(&s->qlstate);
		grainstageclr(s, g);
		qunlock(&s->qlstate);
		return -1;
	}
	if(addfree(u, mapgrain(mold, blk)) < 0)
		return -1;
	return addmap(u, blk, g, dig);
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
	if(grainalloc(s, &ng) < 0){
		qunlock(&s->qlstate);
		werrstr("disk full");
		return -1;
	}
	qunlock(&s->qlstate);
	if(grainwrite(s, buf, ng) < 0){
		qlock(&s->qlstate);
		grainstageclr(s, ng);
		qunlock(&s->qlstate);
		return -1;
	}
	if(addfree(u, g) < 0)
		return -1;
	return addmap(u, blk, ng, dig);
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
	Objinfo *oi)
{
	Upd u;
	Ient *e;
	long slot;
	uvlong qid;
	int reuse;

	if(oidlen < 1 || oidlen > Oidmax){
		werrstr("oid length %d", oidlen);
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
	if((u.e.oid = malloc(oidlen)) == nil)
		return -1;
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
	if(updcommit(&u, Slive, ver, wepoch, time(nil), 0, nil, 0) < 0){
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
	if(objstat(s, oid, oidlen, &oi) < 0)
		return -1;
	if(oi.state != Slive){
		werrstr("no such object");
		return -1;
	}
	newlen = oi.len;
	if(off + n > newlen)
		newlen = off + n;
	if(updopen(&u, s, oid, oidlen, newlen, 0) < 0)
		return -1;
	mapopen(s, &mold, &u.e, u.cold);
	if((buf = malloc(s->sb.blksz)) == nil){
		updabort(&u);
		updclose(&u);
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
	uvlong wepoch)
{
	Upd u;
	Omap mold;
	uchar *buf;

	if(len > s->sb.objmax){
		werrstr("object too large");
		return -1;
	}
	if(updopen(&u, s, oid, oidlen, len, 0) < 0)
		return -1;
	mapopen(s, &mold, &u.e, u.cold);
	if((buf = malloc(s->sb.blksz)) == nil){
		updabort(&u);
		updclose(&u);
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
	if(updcommit(&u, Slive, ver, wepoch, time(nil), 0, nil, 0) < 0){
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
objremove(Store *s, uchar *oid, int oidlen, uvlong ver, uvlong wepoch)
{
	Upd u;
	Omap mold;

	if(updopen(&u, s, oid, oidlen, 0, 0) < 0)
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
	if(updcommit(&u, Stomb, ver, wepoch, time(nil), 0, nil, 0) < 0){
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
objcorrupt(Store *s, uchar *oid, int oidlen, int set)
{
	Upd u;
	Omap mold;
	Objinfo oi;

	if(objstat(s, oid, oidlen, &oi) < 0)
		return -1;
	if(updopen(&u, s, oid, oidlen, oi.len, 1) < 0)
		return -1;
	mapopen(s, &mold, &u.e, u.cold);
	if(u.oldnblk > 0 && nameall(&u, &mold) < 0){
		updabort(&u);
		updclose(&u);
		return -1;
	}
	if(updcommit(&u, u.e.state, u.e.ver, u.e.wepoch, u.e.mtime, set,
		nil, 0) < 0){
		updclose(&u);
		return -1;
	}
	updclose(&u);
	return 0;
}

/*
 * layer-a §1.5's discard: once the cluster-wide conditions hold, the
 * tombstone's index slot returns to the free list.  One Eslot, and
 * §3.5 is what keeps the slot out of the allocator until the commit
 * that freed it is durable.
 */
int
objdiscard(Store *s, uchar *oid, int oidlen)
{
	Item it;
	long slot;

	qlock(&s->qlstate);
	if((slot = ientfind(s, oid, oidlen)) < 0){
		qunlock(&s->qlstate);
		werrstr("no such object");
		return -1;
	}
	if(s->idx[slot].state != Stomb){
		qunlock(&s->qlstate);
		werrstr("not a tombstone");
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
		qunlock(&s->qlstate);
		werrstr("no such object");
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
		return -1;
	}
	if(nblk > 0 && (v->bad = malloc(nblk*sizeof *v->bad)) == nil){
		free(buf);
		free(digs);
		emapunpin(s, c);
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
		werrstr("oid length %d", oidlen);
		return nil;
	}
	if(len > s->sb.objmax){
		werrstr("object too large");
		return nil;
	}
	nblk = blkcount(len, s->sb.blksz);
	if((g = mallocz(sizeof *g, 1)) == nil)
		return nil;
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
			return nil;
		}
	}
	qlock(&s->qlstate);
	g->next = s->stages;
	s->stages = g;
	qunlock(&s->qlstate);
	return g;
}

int
stagewrite(Stage *g, void *a, long n, uvlong off)
{
	Store *s;
	uchar *buf, *src;
	ulong blk, boff, bn, gr, k;
	long left;

	s = g->s;
	/*
	 * §3.6: off is a peer's u64 straight off a /repl fid, so the
	 * bound is a difference — off+n wraps at off = 2^64-4, and the
	 * grain array this indexes has g->nblk elements and no more.  A
	 * negative count is refused in its own right rather than by that
	 * same accident.
	 */
	if(n < 0 || off > g->len || (uvlong)n > g->len - off){
		werrstr("chunk past the declared length");
		return -1;
	}
	if((buf = malloc(s->sb.blksz)) == nil)
		return -1;
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
		if(grainalloc(s, &gr) < 0){
			qunlock(&s->qlstate);
			free(buf);
			werrstr("disk full");
			return -1;
		}
		if(g->grain[blk] != 0)
			grainstageclr(s, g->grain[blk]);
		else{
			g->ngrain++;
			s->nstagegrain++;
		}
		qunlock(&s->qlstate);
		if(grainwrite(s, buf, gr) < 0){
			qlock(&s->qlstate);
			grainstageclr(s, gr);
			qunlock(&s->qlstate);
			free(buf);
			return -1;
		}
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
	g->last = nsec();
	devpoint(s->d, "stage", 0);
	return 0;
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
	qlock(&s->qlstate);
	for(i = 0; i < g->nblk; i++)
		if(g->grain[i] != 0){
			grainstageclr(s, g->grain[i]);
			s->nstagegrain--;
		}
	stageunlink(s, g);
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
int
stagefinal(Stage *g, uvlong ver, uvlong wepoch)
{
	Store *s;
	Upd u;
	Omap mold;
	uvlong i;
	uchar dig[Blkdlen];

	s = g->s;
	if(updopen(&u, s, g->oid, g->oidlen, g->len, 0) < 0)
		return -1;
	mapopen(s, &mold, &u.e, u.cold);
	for(i = 0; i < u.oldnblk; i++)
		if(addfree(&u, mapgrain(&mold, i)) < 0){
			updabort(&u);
			updclose(&u);
			return -1;
		}
	for(i = 0; i < g->nblk; i++){
		if(g->grain[i] != 0)
			memmove(dig, g->dig + i*Blkdlen, Blkdlen);
		else
			zerodigest(s, g->len, i, dig);
		if(addmap(&u, i, g->grain[i], dig) < 0){
			updabort(&u);
			updclose(&u);
			return -1;
		}
	}
	if(updcommit(&u, Slive, ver, wepoch, time(nil), 0, nil, 0) < 0){
		updclose(&u);
		return -1;
	}
	updclose(&u);
	qlock(&s->qlstate);
	for(i = 0; i < g->nblk; i++)
		if(g->grain[i] != 0)
			s->nstagegrain--;
	stageunlink(s, g);
	qunlock(&s->qlstate);
	free(g->grain);
	free(g->dig);
	free(g);
	return 0;
}

void
stagesweep(Store *s, vlong now)
{
	Stage *g, *next, *dead;
	uvlong i;

	dead = nil;
	qlock(&s->qlstate);
	for(g = s->stages; g != nil; g = next){
		next = g->next;
		if(now - g->last <= (vlong)s->cfg.stagems*1000000LL)
			continue;
		stageunlink(s, g);
		for(i = 0; i < g->nblk; i++)
			if(g->grain[i] != 0){
				grainstageclr(s, g->grain[i]);
				s->nstagegrain--;
			}
		g->next = dead;
		dead = g;
	}
	qunlock(&s->qlstate);
	for(g = dead; g != nil; g = next){
		next = g->next;
		free(g->grain);
		free(g->dig);
		free(g);
	}
}
