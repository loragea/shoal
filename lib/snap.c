#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"
#include "store.h"

/*
 * docs/design/store.md §9's snapshot-at-open enumeration: what a
 * server's /obj, /tombs and /advert fids read.  R12, and layer-a
 * §2.2's SHOULD for /obj and /tombs.
 *
 * The object snapshot is a vector of names taken under one hold of
 * qlstate — {slot, qid.path} for every entry whose state the open
 * asked for — and nothing more.  It pins nothing, so a discard of an
 * entry it names is not refused or delayed; the entry simply becomes
 * gone.  Rendering happens afterwards, per entry, under the same
 * short hold of qlstate §8's cursor takes, so no lock spans a
 * caller's use of an entry and a walk of 2.6·10^5 entries never
 * blocks /status, /ctl or Tflush (§7 rule 2).
 *
 * §9 sizes the vector at 12 bytes an entry, so it is two parallel
 * arrays rather than one array of a padded struct: a {ulong, uvlong}
 * struct is 16 bytes on amd64, and the 4 in every 16 buys nothing.
 */

static int
inkinds(int state, int kinds)
{
	if(state == Slive)
		return (kinds & Snaplive) != 0;
	if(state == Stomb)
		return (kinds & Snaptomb) != 0;
	return 0;
}

Objsnap*
objsnapopen(Store *s, int kinds)
{
	Objsnap *sn;
	Ient *e;
	ulong i, n, want;

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
	qlock(&s->qlstate);
	/*
	 * §9's bound.  The cost is per open fid — 12 MB at nslots =
	 * 2^20 — so an open past the configured maximum answers layer-a
	 * §2.6's `disk full' rather than growing without limit.  The
	 * detail after the prefix names the cause for an operator; the
	 * prefix is what a client matches on (§6).
	 */
	if(s->nobjsnap >= s->cfg.objsnapmax){
		qunlock(&s->qlstate);
		free(sn);
		werrstr("disk full: %lud object snapshots already open",
			s->nobjsnap);
		return nil;
	}
	/*
	 * nlive and ntomb are the index's own counts of the two states,
	 * maintained by every apply under this lock, so the vector's
	 * size is known before the walk and one allocation serves.  The
	 * walk stops at that count as well as at nslots, so a count that
	 * ever disagreed with the index would truncate the snapshot
	 * rather than run off the end of the array.
	 */
	want = 0;
	if(kinds & Snaplive)
		want += s->nlive;
	if(kinds & Snaptomb)
		want += s->ntomb;
	if(want > 0){
		sn->slot = malloc(want*sizeof *sn->slot);
		sn->qidpath = malloc(want*sizeof *sn->qidpath);
		if(sn->slot == nil || sn->qidpath == nil){
			qunlock(&s->qlstate);
			free(sn->slot);
			free(sn->qidpath);
			free(sn);
			werrstr("out of memory");
			return nil;
		}
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
