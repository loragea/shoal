#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"
#include "store.h"

/*
 * docs/design/store.md §9's snapshot-at-open enumeration and the two
 * copies beside it: what a server's /obj, /tombs, /advert, /dirty and
 * /lost fids read.  R12, layer-a §2.2's MUST for the last two and its
 * SHOULD for /obj and /tombs.
 *
 * The object snapshot is a vector of names taken under one hold of
 * qlstate — {slot, qid.path} for every entry whose state the open
 * asked for — and nothing more.  It pins nothing, so a discard of an
 * entry it names is not refused or delayed; the entry simply becomes
 * gone.  Rendering happens afterwards, per entry, under the same
 * short hold of qlstate §8's cursor takes, so no lock spans a
 * caller's use of an entry: a full walk of 2.6·10^5 entries is
 * 2.6·10^5 short holds and blocks nothing for longer than one of
 * them.
 *
 * **What the OPEN costs is not small, and §7 rule 2's letter is what
 * it satisfies rather than its number.**  The walk stops at the
 * count, so the cost is the highest occupied slot and not nslots: a
 * lightly-used 2^20 index opens in microseconds, but an index whose
 * last entry sits at slot 2^20 costs a scan of them all, measured at
 * ~22 ns a slot on the reference machine — **≈23 ms** — held under
 * qlstate.  §2.3's 4x over-provision puts the envelope's 2.6·10^5
 * objects on an nslots near 10^6, so that is the envelope case and
 * not a corner.  Rule 2's letter holds: no device call, flush wait or
 * Rendez sleep is reachable under the hold (the walk touches s->idx
 * alone, and storeserving takes and releases qllog before it), and
 * the allocation — 12 MB at nslots = 2^20 — is taken outside it.
 * §16(a) carries the chunked scan under a generation counter that
 * would bound the hold if T2 shows the 23 ms matters.
 *
 * §9 sizes the vector at 12 bytes an entry, so it is two parallel
 * arrays rather than one array of a padded struct: a {ulong, uvlong}
 * struct is 16 bytes on amd64, and the 4 in every 16 buys nothing.
 */

enum
{
	/*
	 * How many times an open re-counts when the index moves under
	 * its allocation.  A partial vector is not an option (§9's
	 * objsnap=full), and an index that moves under eight counts in
	 * a row is a caller that will do better opening again than
	 * spinning here under a lock every apply wants.
	 */
	Snaptries	= 8,
};

static int
inkinds(int state, int kinds)
{
	if(state == Slive)
		return (kinds & Snaplive) != 0;
	if(state == Stomb)
		return (kinds & Snaptomb) != 0;
	return 0;
}

/*
 * How many entries the open will name.  nlive and ntomb are the
 * index's own counts of the two states, maintained by every apply
 * under qlstate, so the vector's size is known without a walk.
 * Caller holds qlstate.
 */
static ulong
snapwant(Store *s, int kinds)
{
	ulong want;

	want = 0;
	if(kinds & Snaplive)
		want += s->nlive;
	if(kinds & Snaptomb)
		want += s->ntomb;
	return want;
}

Objsnap*
objsnapopen(Store *s, int kinds)
{
	Objsnap *sn;
	Ient *e;
	ulong i, n, want, try;

	if(!storeserving(s))
		return nil;
	if(kinds == 0 || (kinds & ~Snapboth) != 0){
		werrstr("object snapshot: kinds %#x", kinds);
		return nil;
	}
	if((sn = mallocz(sizeof *sn, 1)) == nil){
		werrstr("out of memory");
		return nil;
	}
	sn->s = s;
	sn->kinds = kinds;
	for(try = 0; try < Snaptries; try++){
		/*
		 * The count, under the lock.  §9's bound is checked here
		 * too, so that an open past it costs no allocation; the
		 * check that decides it is the one under the fill below,
		 * because that is where the count is taken.
		 */
		qlock(&s->qlstate);
		if(s->nobjsnap >= s->cfg.objsnapmax){
			qunlock(&s->qlstate);
			werrstr("disk full: %lud object snapshots already open",
				s->nobjsnap);
			goto bad;
		}
		want = snapwant(s, kinds);
		/*
		 * §13's snapstale point: take the count one short, which is
		 * what a create between this hold and the fill's leaves.
		 * Inert unless a test asks for it.
		 */
		if(s->snapstale > 0 && want > 0){
			s->snapstale--;
			want--;
		}
		qunlock(&s->qlstate);

		/*
		 * The allocation, outside the lock.  12 MB at nslots = 2^20,
		 * and §7 rule 2's whole point is that nothing a caller waits
		 * on happens under qlstate.
		 */
		free(sn->slot);
		free(sn->qidpath);
		sn->slot = nil;
		sn->qidpath = nil;
		if(want > 0){
			sn->slot = malloc(want*sizeof *sn->slot);
			sn->qidpath = malloc(want*sizeof *sn->qidpath);
			if(sn->slot == nil || sn->qidpath == nil){
				werrstr("out of memory");
				goto bad;
			}
		}

		qlock(&s->qlstate);
		if(s->nobjsnap >= s->cfg.objsnapmax){
			qunlock(&s->qlstate);
			werrstr("disk full: %lud object snapshots already open",
				s->nobjsnap);
			goto bad;
		}
		/*
		 * The index may have moved while the allocation ran.  A
		 * vector short of what the index now holds would be a
		 * PARTIAL snapshot, which §9's objsnap=full refuses to
		 * serve, so the answer is to count again rather than to
		 * truncate.  The counts are exact, so this one comparison
		 * settles it: equal counts mean the walk below finds
		 * exactly want entries.
		 */
		if(snapwant(s, kinds) != want){
			qunlock(&s->qlstate);
			continue;
		}
		n = 0;
		for(i = 0; i < s->sb.nslots && n < want; i++){
			e = &s->idx[i];
			if(!inkinds(e->state, kinds))
				continue;
			sn->slot[n] = i;
			sn->qidpath[n] = e->qidpath;
			n++;
		}
		sn->n = n;
		s->nobjsnap++;
		qunlock(&s->qlstate);
		return sn;
	}
	/*
	 * An index that moved under every attempt.  A local error, not a
	 * §2.6 wire one: nothing is full and nothing is broken, the
	 * caller may simply open again.
	 */
	werrstr("object snapshot: the index moved under %d counts", Snaptries);
bad:
	free(sn->slot);
	free(sn->qidpath);
	free(sn);
	return nil;
}

ulong
objsnapcount(Objsnap *sn)
{
	return sn->n;
}

/*
 * Entry i, rendered from the live index.  "Gone" is two conditions
 * and not one, and the second is not a refinement of the first: §2.3
 * keeps an object's qid.path across delete, tombstone and re-create,
 * so an object deleted after a /obj open still matches on qid.path
 * and is now a tombstone — which layer-a §2.2 says /obj MUST NOT list
 * — and a tombstone re-created over after a /tombs open matches too
 * and is now live.  What the qid.path test catches is the other half:
 * the slot was freed by a discard, or freed and handed to a different
 * object, so the entry the snapshot named no longer exists at all.
 */
int
objsnapent(Objsnap *sn, ulong i, uchar *oid, int *oidlen, Objinfo *oi)
{
	Store *s;
	Ient *e;
	ulong slot;

	if(i >= sn->n){
		werrstr("entry %lud, the snapshot holds %lud", i, sn->n);
		return -1;
	}
	s = sn->s;
	if(!storeserving(s))
		return -1;
	slot = sn->slot[i];
	qlock(&s->qlstate);
	e = &s->idx[slot];
	if(e->qidpath != sn->qidpath[i] || !inkinds(e->state, sn->kinds)){
		qunlock(&s->qlstate);
		*oidlen = 0;
		return 0;
	}
	memmove(oid, e->oid, e->oidlen);
	*oidlen = e->oidlen;
	ientinfo(s, slot, oi);
	qunlock(&s->qlstate);
	return 1;
}

void
objsnapclose(Objsnap *sn)
{
	Store *s;

	if(sn == nil)
		return;
	s = sn->s;
	qlock(&s->qlstate);
	if(s->nobjsnap > 0)
		s->nobjsnap--;
	qunlock(&s->qlstate);
	free(sn->slot);
	free(sn->qidpath);
	free(sn);
}

/*
 * /dirty, layer-a §2.2's snapshot MUST and §7.1's records.  The set
 * is bounded by the dirty region, so a copy is the whole of what a
 * renderer needs and there is no cursor to tear: everything below is
 * read under one hold of the lock that guards the set.  The array is
 * indexed by dirty-record slot and holed, so the walk counts what it
 * copies rather than trusting the array to be dense.
 */
int
dirtysnap(Store *s, Dirtyrec **dp, ulong *np)
{
	Dirtyrec *d;
	Dirtent *t;
	ulong i, n, used;

	*dp = nil;
	*np = 0;
	if(!storeserving(s))
		return -1;
	qlock(&s->qlstate);
	used = s->ndirtused;
	d = nil;
	if(used > 0 && (d = mallocz(used*sizeof *d, 1)) == nil){
		qunlock(&s->qlstate);
		werrstr("out of memory");
		return -1;
	}
	n = 0;
	for(i = 0; i < s->sb.ndirty && n < used; i++){
		if((t = s->dirt[i]) == nil)
			continue;
		d[n].op = 1;		/* a record in the set is an add */
		d[n].oidlen = t->oidlen;
		d[n].peerlen = t->peerlen;
		d[n].epoch = t->epoch;
		memmove(d[n].oid, t->oid, t->oidlen);
		memmove(d[n].peer, t->peer, t->peerlen);
		n++;
	}
	qunlock(&s->qlstate);
	*dp = d;
	*np = n;
	return 0;
}

/*
 * /lost, layer-a §7.5 and §2.2's snapshot MUST: every copy this
 * instance holds that fails local verification, with the oid and the
 * Objinfo beside the slot so a renderer need not go back to the index
 * — which it could not do consistently anyway, since storelost's list
 * moves under a concurrent scrub.  Taken under one hold of qlstate,
 * which is the lock storecondemn reallocs the list under.
 *
 * The copy names EVERY slot storelost names, including §5 step 10's:
 * an index entry that would not unpack leaves its slot `bad' with its
 * state still Sfree (store.c's readindex), and that slot is on the
 * list and counted in Storestat.nlost.  It has no oid to give — the
 * entry that would have held one is the damage — so its Lostent
 * carries oidlen 0 and an Objinfo that is the slot number and
 * nothing else, and a renderer emits a line with no `oid=' (layer-a
 * §2.2 fixes only `oid=' and `kind=' when they are present).  Any
 * other rule would make the copy disagree with /status's own lost
 * count on precisely the damage /lost exists for.
 */
int
lostsnap(Store *s, Lostent **lp, ulong *np)
{
	Lostent *l;
	Ient *e;
	ulong i, n, slot;

	*lp = nil;
	*np = 0;
	if(!storeserving(s))
		return -1;
	qlock(&s->qlstate);
	l = nil;
	if(s->nlost > 0 && (l = mallocz(s->nlost*sizeof *l, 1)) == nil){
		qunlock(&s->qlstate);
		werrstr("out of memory");
		return -1;
	}
	n = 0;
	for(i = 0; i < s->nlost; i++){
		slot = s->lost[i];
		if(slot >= s->sb.nslots)
			continue;
		e = &s->idx[slot];
		/*
		 * A free slot on the list is §5 step 10's: readindex left it
		 * bad with no oid unpacked, so oidlen is 0 and ientinfo's
		 * fill is the slot number, state Sfree and zeroes.  Nothing
		 * else reaches the list free: lostupdate delists a slot the
		 * moment an apply frees it.
		 */
		l[n].oidlen = e->oidlen;
		memmove(l[n].oid, e->oid, e->oidlen);
		ientinfo(s, slot, &l[n].oi);
		n++;
	}
	qunlock(&s->qlstate);
	*lp = l;
	*np = n;
	return 0;
}
