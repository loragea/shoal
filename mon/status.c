#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../lib/shoal.h"
#include "mon.h"
#include "dat.h"
#include "fns.h"

/*
 * The files of layer-a §8.1 that report what the monitor holds: /map,
 * /maps/<epoch>, /instances, /stale, /health and /status, plus the
 * empty render /ctl and /map.next share.  Every one is composed at
 * open — §8.1's MUST for all of them — into the fid's Mtext, so a read
 * cannot be torn by a publish.
 *
 * Every render here runs with the slot store's lock held (tree.c), so
 * the map it reads and the ring it reads next are one state, and the
 * Cmap it walks is the parse of the very text /map would answer.
 */

/*
 * /map: the current map text, verbatim, which is what every instance
 * and client of the cluster reads to learn the epoch and the placement
 * (§6.3).  The bytes are copied out of the slot store rather than
 * pointed at, because a Monmap's text is valid only until the next
 * commit (lib/shoal.h).
 *
 * A store that holds NO map is refused, with this server's own
 * `shoalmon: no map', rather than answered with zero bytes.  §8.1
 * gives no answer for the case and the choice is this server's
 * (store.md §14(54)): zero bytes is indistinguishable from a
 * successful read of an empty file, an instance would feed it to
 * mapparse and be told `bad map' — a lie about the monitor's state —
 * and, because §8.4's evidence is a read that transferred bytes, an
 * empty answer would leave the refreshing instance's lease silently
 * unrenewed with nothing to tell it why.  A refusal says the one true
 * thing: this monitor has not published yet.
 */
char*
monmaptext(Monctx *c, Mfid *f, Mtext *t)
{
	Monmap mm;

	USED(f);
	if(!moncurrent(c->mon, &mm))
		return Emonnomap;
	if(montextwrite(t, mm.text, mm.len) < 0)
		return "shoalmon: out of memory";
	return nil;
}

/*
 * /maps/<epoch>: one of the last `retain' published maps (§8.2).  The
 * lookup is made again here and not taken from the walk, because the
 * ring can turn between the two: an epoch that aged out in that window
 * is refused rather than served from whatever now occupies the slot.
 * layer-a §5.2 clause 2 is the consumer that matters — it reads
 * /maps/<E−1> — and §8.2 makes the immediately previous map the one
 * retention MUST keep.
 */
char*
monmapfiletext(Monctx *c, Mfid *f, Mtext *t)
{
	Monmap mm;

	if(!monlookup(c->mon, f->epoch, &mm))
		return Emonnoepoch;
	if(montextwrite(t, mm.text, mm.len) < 0)
		return "shoalmon: out of memory";
	return nil;
}

/*
 * /instances, §8.1: one line per instance record in the current map.
 * The field names are normative — this is the operator's
 * conflict-resolution surface (§3.4 step 5) — and extra fields are
 * permitted:
 *
 *	uuid= iid= node= addr= class= registered= lastseen=
 *
 * Two of §8.1's fields are not measurements this monitor has made, and
 * store.md §14(57) records both.
 *
 *	`registered=' is the time of the instance's last registration.
 *	Nothing durable records it: §3.1's instance record has no such
 *	attribute, and `register' is not built in this unit, so there is
 *	no registration to time.  It is rendered as 0, which is not a
 *	time and MUST be read as "this monitor holds no registration
 *	record", because the field NAME is normative and a parser of
 *	this file is entitled to find it.
 *
 *	`conflict=node' is absent from every line, which by §8.1's own
 *	grammar means "no conflict": the field is conditional there.
 *	Detecting one needs the node a disk registered FROM, which is
 *	registration state the next unit owns; until then this monitor
 *	cannot detect one and never claims to have.
 *
 * `lastseen=' is §8.4's evidence and is live: it is the seconds
 * reading of lastseen(iid), 0 for an instance never seen.
 */
char*
moninstancestext(Monctx *c, Mfid *f, Mtext *t)
{
	Cinst *in;
	int i;

	USED(f);
	if(c->map == nil)
		return nil;
	for(i = 0; i < c->map->ninst; i++){
		in = &c->map->inst[i];
		montextprint(t, "uuid=%s iid=%s node=%s addr=%s class=%s "
			"registered=0 lastseen=%llud\n",
			in->uuid, in->iid, in->node, in->addr, in->class,
			monsrvlastseen(c, in->iid));
	}
	return nil;
}

/*
 * /stale, §8.1: "view of the map's stale records (§7.1)", in the map's
 * own ledger grammar — `stale=<subject> reporter=<iid> since=<epoch>'
 * (§3.1) — so the same parser reads the map, an instance's /stale
 * (§2.2) and this file.
 *
 * An instance's /stale shows the marks IT is party to and filters on
 * its own iid; the monitor is party to none and filters nothing, so
 * this is the whole ledger.
 */
char*
monstaletext(Monctx *c, Mfid *f, Mtext *t)
{
	Cstale *m;
	int i;

	USED(f);
	if(c->map == nil)
		return nil;
	for(i = 0; i < c->map->nstale; i++){
		m = &c->map->stale[i];
		montextprint(t, "stale=%s reporter=%s since=%llud\n",
			m->subject, m->reporter, m->since);
	}
	return nil;
}

/*
 * /health, §8.1: one line per instance,
 * `iid= lastrefresh= silent= reports=', "liveness observations,
 * diagnostic only".  Format beyond those names is implementation
 * policy and so is the content: nothing in this design reads it except
 * a human, and since D-a it is not input to any automatic transition.
 *
 * `lastrefresh=' is §8.4's lastseen(iid) in seconds and `silent=' is
 * the milliseconds since — which is the quantity §8.4's demotion
 * compares against `deadms', so this file is where that rule will be
 * able to show its working.  An instance never seen has no lastrefresh
 * at all, and its silence is rendered as the milliseconds since this
 * service started: that is a true lower bound on how long the channel
 * has been quiet, and a 0 there would read as "heard from just now",
 * which is the one thing it must not say.
 *
 * `reports=' is the instances that currently claim they cannot reach
 * this one.  It is empty on every line and will stay so until the
 * `unreachable'/`reachable' verbs of §8.3 are built, which is the unit
 * after next; the field is present because §8.1 names it (store.md
 * §14(61)).
 */
char*
monhealthtext(Monctx *c, Mfid *f, Mtext *t)
{
	Cinst *in;
	uvlong now, seen, silent;
	int i;

	USED(f);
	if(c->map == nil)
		return nil;
	now = time(0);
	for(i = 0; i < c->map->ninst; i++){
		in = &c->map->inst[i];
		seen = monsrvlastseen(c, in->iid);
		silent = seen > 0 ? now - seen : now - c->t0;
		montextprint(t, "iid=%s lastrefresh=%llud silent=%llud "
			"reports=\n", in->iid, seen, silent*1000);
	}
	return nil;
}

/*
 * /status, §8.1: `epoch=', `monid=', `uptime=', the timer attributes
 * in force, `retain=', `ledger=ok|lost' (§8.6) and `pending=<n>'
 * staged edits.  Format otherwise implementation policy.
 *
 * The map-derived fields are present only when this store holds a map,
 * and `hasmap=' says which case a reader is in — a monitor formatted
 * and not yet published to has no epoch, no monid and no timers to
 * report, and a zero for each would be a claim about a cluster that
 * does not exist yet (store.md §14(54)).
 *
 * `epoch=' is the map text's, not the slot's stamp: §10 records the
 * stamped epoch "for the operator's benefit" and the text is the
 * authority.  `retain=' is the map header's attribute — the retention
 * §8.2 requires of the monitor — and `slots=' is the ring the
 * partition was formatted with, which is what this monitor can
 * actually keep; the two are set independently and an operator who
 * raised `retain' above `slots' can see it here.
 *
 * `ledger=' reads `ok' always.  §8.6's `lost' is the verdict of a
 * monitor rebuilt with no recoverable map, and the rebuild path — the
 * §8.6 start-up that reads every instance's /status and /stale — is
 * not built, so nothing here can have reached that verdict.  An empty
 * store is NOT it: a fresh cluster and a lost ledger are different
 * facts and `hasmap=no' is the one this monitor knows.
 *
 * `pending=' reads 0 always: staging is /map.next's, which the next
 * unit builds (store.md §14(61)).
 */
char*
monstatustext(Monctx *c, Mfid *f, Mtext *t)
{
	Monstat st;
	Cmap *m;

	USED(f);
	monstat(c->mon, &st);
	m = c->map;
	montextprint(t, "hasmap=%s\n", m != nil ? "yes" : "no");
	if(m != nil){
		montextprint(t, "epoch=%llud\n", m->epoch);
		montextprint(t, "monid=%s\n", m->monid);
	}
	montextprint(t, "uptime=%llud\n", (uvlong)time(0) - c->t0);
	if(m != nil){
		montextprint(t, "pollms=%lud leasems=%lud replms=%lud "
			"deadms=%lud\n", m->pollms, m->leasems, m->replms,
			m->deadms);
		montextprint(t, "outmins=%lud tombdays=%lud mincopies=%lud "
			"replicas=%lud\n", m->outmins, m->tombdays,
			m->mincopies, m->replicas);
		montextprint(t, "retain=%lud\n", m->retain);
	}
	montextprint(t, "slots=%lud\n", st.retain);
	montextprint(t, "maps=%lud\n", st.nhist);
	montextprint(t, "seq=%llud\n", st.seq);
	montextprint(t, "ledger=ok\n");
	montextprint(t, "pending=0\n");
	return nil;
}

/*
 * /ctl and /map.next read as no bytes.  /ctl's verbs are a write
 * surface (§8.3); /map.next is the staged map, and nothing is staged
 * until the next unit builds the staging.
 */
char*
monemptytext(Monctx *c, Mfid *f, Mtext *t)
{
	USED(c);
	USED(f);
	USED(t);
	return nil;
}
