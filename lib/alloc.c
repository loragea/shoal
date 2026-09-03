#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"
#include "store.h"

/*
 * Space management, docs/design/store.md §6.
 *
 * Grains are fixed-size and interchangeable, so allocation is "find a
 * clear bit": a bitmap, a rotating cursor and a free count, with no
 * external fragmentation and nothing to coalesce.
 *
 * Beside the bitmap is the **staged set** — the grains a stage has
 * reserved and no record has yet named (§3.1, §3.6).  The two are
 * kept apart because the checkpoint materialises committed state
 * only (§2.8): a reservation that reached the bitmap would be written
 * durably by a checkpoint taken while the stage was live, and after
 * the stage died and the power went, replay — which knows nothing of
 * stages — would leave the grain marked allocated and referenced by
 * nothing.  So a grain leaves the set for the bitmap when the commit
 * naming it is applied, and leaves it for nothing when the stage is
 * discarded.
 *
 * The two slot spaces are the same shape, with a reservation bit
 * each: a create holds an index slot from the moment it allocates one
 * until its commit is applied, and a growth past one block holds an
 * extent-map slot the same way, and neither reservation is durable.
 *
 * Every function here is called with qlstate held (§7).
 */

static int
bitget(uchar *p, uvlong i)
{
	return (p[i/8] >> (i%8)) & 1;
}

static void
bitset(uchar *p, uvlong i)
{
	p[i/8] |= 1 << (i%8);
}

static void
bitclr(uchar *p, uvlong i)
{
	p[i/8] &= ~(1 << (i%8));
}

/* mark the bitmap page holding grain g for the next checkpoint */
static void
bmpagedirty(Store *s, ulong g)
{
	uvlong page;

	page = g / bmbits(s->sb.blksz);
	if(page < s->nbmpage && !s->bmdirty[page]){
		s->bmdirty[page] = 1;
		s->ndirtypage++;
	}
}

void
idxdirty(Store *s, ulong slot)
{
	uvlong page;

	page = (uvlong)slot*Idxentsz / s->sb.blksz;
	if(page < s->nidxpage && !s->idxdirty[page]){
		s->idxdirty[page] = 1;
		s->ndirtypage++;
	}
}

void
dirtdirty(Store *s, ulong slot)
{
	uvlong page;

	page = (uvlong)slot*Dirtentsz / s->sb.blksz;
	if(page < s->ndirtpage && !s->dirtdirty[page]){
		s->dirtdirty[page] = 1;
		s->ndirtypage++;
	}
}

static int
stagedhas(Store *s, ulong g)
{
	Sgrain *n;

	for(n = s->stagebuck[g % s->nstagebuck]; n != nil; n = n->next)
		if(n->g == g)
			return 1;
	return 0;
}

static int
stagedadd(Store *s, ulong g)
{
	Sgrain *n;

	if((n = s->stagefree) != nil)
		s->stagefree = n->next;
	else if((n = malloc(sizeof *n)) == nil){
		/*
		 * §3.7: a failed allocation is an internal error and never
		 * a §2.6 `disk full' — the disk may be nearly empty.  The
		 * allocator leaves errstr alone on failure, so it is set
		 * here, where exhaustion and OOM part ways, and grainalloc's
		 * callers rely on the distinction being already spelled.
		 */
		werrstr("out of memory");
		return -1;
	}
	n->g = g;
	n->next = s->stagebuck[g % s->nstagebuck];
	s->stagebuck[g % s->nstagebuck] = n;
	s->nstaged++;
	s->grainfree--;
	return 0;
}

static int
stageddel(Store *s, ulong g)
{
	Sgrain **pp, *n;

	for(pp = &s->stagebuck[g % s->nstagebuck]; (n = *pp) != nil;
	    pp = &n->next)
		if(n->g == g){
			*pp = n->next;
			n->next = s->stagefree;
			s->stagefree = n;
			s->nstaged--;
			s->grainfree++;
			return 1;
		}
	return 0;
}

/*
 * Reserve a fresh grain: one no committed map references, none a
 * commit has released without its post-flush having returned (§3.5 —
 * a commit's frees are part of applying it, and a batch is applied
 * only after its post-flush), and none another stage has reserved.
 *
 * Every failure return sets errstr itself — `disk full' for the two
 * exhaustion exits, `out of memory' from stagedadd — and callers MUST
 * NOT overwrite it: folding OOM into `disk full' answers a memory
 * failure with a §2.6 wire error (§3.7).
 */
int
grainalloc(Store *s, ulong *gp)
{
	uvlong i, g, n;

	n = s->sb.ngrains;
	if(s->grainfree == 0){
		werrstr("disk full");
		return -1;
	}
	g = s->graincur;
	for(i = 0; i < n; i++){
		if(g == 0 || g >= n)
			g = 1;
		if(!bitget(s->bmap, g) && !stagedhas(s, g)){
			if(stagedadd(s, g) < 0)
				return -1;
			s->graincur = g + 1;
			*gp = g;
			return 0;
		}
		g++;
	}
	werrstr("disk full");
	return -1;
}

/* release a reservation: §3.3's discard, which costs no durable write */
void
grainstageclr(Store *s, ulong g)
{
	if(g == 0 || g >= s->sb.ngrains)
		return;
	stageddel(s, g);
}

/* the commit naming a grain is what makes it allocated (§3.1 step 2) */
void
grainmark(Store *s, ulong g)
{
	if(g == 0 || g >= s->sb.ngrains)
		return;
	stageddel(s, g);
	if(!bitget(s->bmap, g)){
		bitset(s->bmap, g);
		s->grainfree--;
	}
	bmpagedirty(s, g);
}

void
grainclear(Store *s, ulong g)
{
	if(g == 0 || g >= s->sb.ngrains)
		return;
	if(bitget(s->bmap, g)){
		bitclr(s->bmap, g);
		s->grainfree++;
	}
	/*
	 * The cursor lands on what was just released, so the next
	 * allocation reuses it.  That is what §3.5's deferred-reuse rule
	 * is written against — a grain released by a commit is the one
	 * most likely to be handed out next — and it is what makes the
	 * rule testable rather than merely stated.
	 */
	s->graincur = g;
	bmpagedirty(s, g);
}

/*
 * Index slots.  A slot is free when it is neither used — live, a
 * tombstone, or condemned by §5 step 10 and so never reused — nor
 * reserved by a create whose commit has not been applied.
 */
int
slotalloc(Store *s, ulong *sp)
{
	ulong i, k, n;

	n = s->sb.nslots;
	if(s->slotfree == 0){
		werrstr("disk full");
		return -1;
	}
	k = s->slotcur;
	for(i = 0; i < n; i++){
		if(k >= n)
			k = 0;
		if(!bitget(s->slotused, k) && !bitget(s->slotresv, k)){
			s->slotcur = k + 1;
			bitset(s->slotresv, k);
			s->slotfree--;
			*sp = k;
			return 0;
		}
		k++;
	}
	werrstr("disk full");
	return -1;
}

void
slotresvclr(Store *s, ulong slot)
{
	if(slot >= s->sb.nslots)
		return;
	if(bitget(s->slotresv, slot)){
		bitclr(s->slotresv, slot);
		s->slotfree++;
	}
}

void
slotmark(Store *s, ulong slot)
{
	if(slot >= s->sb.nslots)
		return;
	if(bitget(s->slotresv, slot)){
		bitclr(s->slotresv, slot);
		s->slotfree++;
	}
	if(!bitget(s->slotused, slot)){
		bitset(s->slotused, slot);
		s->slotfree--;
	}
}

void
slotclear(Store *s, ulong slot)
{
	if(slot >= s->sb.nslots)
		return;
	if(bitget(s->slotused, slot)){
		bitclr(s->slotused, slot);
		s->slotfree++;
	}
	s->slotcur = slot;
}

/*
 * Extent-map slots.  Slot 0 is reserved and never allocated: §2.3
 * spells "this object's map is inline" as emapslot = 0.
 */
int
emapalloc(Store *s, ulong *sp)
{
	ulong i, k, n;

	n = s->sb.nemap;
	if(s->emapfree == 0){
		werrstr("disk full");
		return -1;
	}
	k = s->emapcur;
	for(i = 0; i < n; i++){
		if(k == 0 || k >= n)
			k = 1;
		if(!bitget(s->emapused, k) && !bitget(s->emapresv, k)){
			s->emapcur = k + 1;
			bitset(s->emapresv, k);
			s->emapfree--;
			*sp = k;
			return 0;
		}
		k++;
	}
	werrstr("disk full");
	return -1;
}

void
emapresvclr(Store *s, ulong slot)
{
	if(slot == 0 || slot >= s->sb.nemap)
		return;
	if(bitget(s->emapresv, slot)){
		bitclr(s->emapresv, slot);
		s->emapfree++;
	}
}

void
emapmark(Store *s, ulong slot)
{
	if(slot == 0 || slot >= s->sb.nemap)
		return;
	if(bitget(s->emapresv, slot)){
		bitclr(s->emapresv, slot);
		s->emapfree++;
	}
	if(!bitget(s->emapused, slot)){
		bitset(s->emapused, slot);
		s->emapfree--;
	}
}

void
emapclear(Store *s, ulong slot)
{
	if(slot == 0 || slot >= s->sb.nemap)
		return;
	if(bitget(s->emapused, slot)){
		bitclr(s->emapused, slot);
		s->emapfree++;
	}
	s->emapcur = slot;
}
