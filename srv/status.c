#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../lib/shoal.h"
#include "srv.h"
#include "dat.h"
#include "fns.h"

/*
 * The attr=value files of layer-a §2.2 that report this instance's
 * own state: /status, /map, /dirty, /stale and /lost.  (/tombs and
 * /advert are §7.2's enumeration and are in enum.c; /jobs is the job
 * list's and is in job.c.)  Every one is rendered at open — §2.2's
 * MUST for these files — into the fid's Text, so a read cannot be
 * torn by a concurrent mutation.
 *
 * /status, /map and /stale are answered on the service loop: none
 * names an object, so none is queued (§5.4.1), and none reaches the
 * engine for more than the two locks dat.h weighs /status against.
 * /dirty and /lost do reach it — each takes a copy under the lock
 * that guards its set (store.md §9) — so their rows fill an open cell
 * that puts the render on the reserved queue, and these two renders
 * therefore run on a queue proc rather than on the loop.
 *
 * /status and /map first.
 *
 * Of §2.2's field list, `epoch=' is normative — §8.6's monitor rebuild
 * reads it — and the rest SHOULD be present.  Four of them are not
 * rendered here and their absence is deliberate rather than an
 * oversight: `chunk=' is the smallest peer msize less headers, and
 * `underrep=', `strays=' and `marks=' count objects against peers and
 * against the stale ledger's marks on this instance.  This wave has no
 * peer client and no monitor client, so there is no peer msize to take
 * the smallest of and no reconcile pass to have counted the rest; a
 * zero would be a measurement this instance has not made.  store.md
 * §14(23) records the omission.  Three fields beyond §2.2's list are
 * rendered because something here has no other place to report them:
 * the open enumeration-snapshot count, which store.md §9 makes the
 * server's half of `objsnap=', the queue pool's depth, which §7 asks
 * /status to report so saturation is visible rather than folklore —
 * Reqqueue keeps no count, so these are the server's own — and
 * `diverged=', the count of §1.3's repairs this process has applied
 * (peer.c).  §1.3 requires a repair to be "reported in /status" and
 * names no field, so the name is this server's; the count is in
 * memory and per process, and store.md §14(15) has why the durable
 * half of the same record is not built.
 */
char*
srvstatustext(Srvctx *c, Sfid *f, Text *t)
{
	Storestat st;
	uvlong np, nd;
	int kind;

	USED(f);
	storestat(c->store, &st);
	srvcount(c, &np, &nd);
	kind = srvfencekind(c);
	textprint(t, "iid=%s\n", c->iid);
	textprint(t, "uuid=%s\n", c->uuid);
	textprint(t, "monid=%s\n", c->monid);
	textprint(t, "monidmismatch=%s\n",
		(c->adoptflags & Mapmonid) ? "yes" : "no");
	textprint(t, "status=%s\n", statusname(c->self->status));
	textprint(t, "up=%s\n", upname(c->self->up));
	textprint(t, "fence=%s\n", fencename(kind));
	textprint(t, "epoch=%llud\n", st.epochhigh);
	textprint(t, "epochregress=%s\n",
		(c->adoptflags & Mapregress) ? "yes" : "no");
	textprint(t, "msize=%ud\n", c->srv.msize);
	textprint(t, "objsnap=full\n");
	textprint(t, "objsnapopen=%llud\n", st.nobjsnap);
	textprint(t, "dirty=%lud\n", dirtycount(c->store));
	textprint(t, "lost=%llud\n", st.nlost);
	textprint(t, "diverged=%llud\n", srvdiverged(c));
	textprint(t, "staged=%llud\n", st.staged);
	textprint(t, "queues=%d\n", c->nq);
	textprint(t, "qdepth=%llud\n", np - nd);
	textprint(t, "qpushed=%llud\n", np);
	textprint(t, "qdone=%llud\n", nd);
	textprint(t, "writethrough=%s\n", c->cfg.noflush ? "yes" : "no");
	textprint(t, "flush=%s\n", flushname(st.flushmode));
	textprint(t, "broken=%s\n", st.broken ? "yes" : "no");
	return nil;
}

/*
 * /map is the instance's cached cluster map, read-only: the bytes it
 * adopted, verbatim.  A client's own map comes from the monitor
 * (§6.3); this is the operator's view of what this instance believes.
 */
char*
srvmaptext(Srvctx *c, Sfid *f, Text *t)
{
	USED(f);
	if(textwrite(t, c->maptext, c->maplen) < 0)
		return "shoalsrv: out of memory";
	return nil;
}

/*
 * The error string /dirty's and /lost's renders answer with.  Both
 * run on the reserved queue, which lib9p gives one proc, so the two
 * have one writer between them; the renders above answer literals and
 * never come here.  A render cell answers a char*, which is why the
 * engine's own text cannot be composed in the caller's frame.
 */
static char rendererr[ERRMAX];

static char*
renderr(void)
{
	rerrstr(rendererr, sizeof rendererr);
	return rendererr;
}

/*
 * /dirty, layer-a §2.2 and §7.1.  Two kinds of line, and so two
 * copies: the fine-grained records, `oid= peer= epoch=' each, and one
 * `fullsync peer=' line per peer carrying §7.1's coarse flag.  The
 * second is not derivable from the first — store.md §2.6's exhaustion
 * drop sets the flag on exactly the peer whose records it has just
 * thrown away, so the peers that most need the line are the ones with
 * no record left to name them — which is why the engine answers them
 * separately (§9).
 *
 * Nothing clears the coarse flag yet and a peer is registered with it
 * already set (store.md §9), so today the second half names every
 * peer this store has heard of.
 */
char*
srvdirtytext(Srvctx *c, Sfid *f, Text *t)
{
	char oid[Oidmax+1], peer[Peermax+1];
	Dirtyrec *dr;
	char **pp;
	ulong n, i;

	USED(f);
	if(dirtysnap(c->store, &dr, &n) < 0)
		return renderr();
	for(i = 0; i < n; i++){
		memmove(oid, dr[i].oid, dr[i].oidlen);
		oid[dr[i].oidlen] = 0;
		memmove(peer, dr[i].peer, dr[i].peerlen);
		peer[dr[i].peerlen] = 0;
		textprint(t, "oid=%s peer=%s epoch=%llud\n", oid, peer,
			dr[i].epoch);
	}
	free(dr);
	if(fullsyncsnap(c->store, &pp, &n) < 0)
		return renderr();
	for(i = 0; i < n; i++)
		textprint(t, "fullsync peer=%s\n", pp[i]);
	free(pp);
	return nil;
}

/*
 * /stale, layer-a §2.2 and §7.1: "each instance's /stale shows the
 * marks it is party to", in the map's own ledger grammar, so the same
 * parser reads the map and this file.  A mark this instance is party
 * to is one naming it as the subject or as the reporter.
 *
 * The marks come from the map and from nowhere else — the ledger is
 * the monitor's and travels inside the map (§3.1) — so this render
 * reaches no engine state and stays on the service loop.  With no
 * monitor client the adopted map never changes (store.md §14(18)), so
 * what this file answers is fixed for the life of the instance: the
 * marks the map it was started with carried.  A refresh loop makes it
 * move without changing anything here.
 */
char*
srvstaletext(Srvctx *c, Sfid *f, Text *t)
{
	Cstale *m;
	int i;

	USED(f);
	for(i = 0; i < c->map->nstale; i++){
		m = &c->map->stale[i];
		if(strcmp(m->subject, c->iid) != 0
		&& strcmp(m->reporter, c->iid) != 0)
			continue;
		textprint(t, "stale=%s reporter=%s since=%llud\n",
			m->subject, m->reporter, m->since);
	}
	return nil;
}

/*
 * /lost, layer-a §2.2 and §7.5: every copy this instance holds that
 * fails local verification — store.md §8's corrupt-flagged entries
 * and §5 step 10's condemned slots alike.
 *
 * A condemned slot has no oid to give: the index entry that would
 * have carried one is the damage.  store.md §9 fixes what that line
 * is — `slot=<n> kind=lost', with no `oid=' — and why dropping it
 * instead would be wrong: §2.2 fixes `oid=' and `kind=' for the
 * fields a line HAS, and a copy dropped here would make this file and
 * /status's `lost=' count disagree about precisely the damage the
 * file exists for.
 *
 * A line that HAS an oid carries `kind=corrupt' and nothing else.
 * §2.2's three values are not three states of one flag: `corrupt' is
 * §7.5's local verification failure, `lost' is §7.5(4)'s "no peer
 * holds a verifying copy" and `diverged' is §1.3's equal key with
 * differing content.  `lost' is a verdict about what PEERS hold and
 * this build has no peer client (store.md §14(18)), so it cannot have
 * been reached.  `diverged' can be: it is a verdict about a repair
 * this receiver applied ITSELF, and /repl's op=full force=1 applies
 * one (peer.c).  What records that today is /status's `diverged='
 * count, because the engine holds no per-record divergence flag for a
 * line here to render — a slot joins the lost list only when it is
 * bad or carries Icorrupt, and the one path that sets either on an
 * entry with an oid sets both.  So no line of this file reads
 * `diverged' and none reads `lost'.  store.md §14(15) records it.
 */
char*
srvlosttext(Srvctx *c, Sfid *f, Text *t)
{
	char oid[Oidmax+1];
	Lostent *lp;
	ulong n, i;

	USED(f);
	if(lostsnap(c->store, &lp, &n) < 0)
		return renderr();
	for(i = 0; i < n; i++){
		if(lp[i].oidlen == 0){
			textprint(t, "slot=%lud kind=lost\n", lp[i].oi.slot);
			continue;
		}
		memmove(oid, lp[i].oid, lp[i].oidlen);
		oid[lp[i].oidlen] = 0;
		textprint(t, "oid=%s kind=corrupt slot=%lud\n", oid,
			lp[i].oi.slot);
	}
	free(lp);
	return nil;
}

/* /ctl reads answer no bytes; the verbs are a write surface (§2.5) */
char*
srvemptytext(Srvctx *c, Sfid *f, Text *t)
{
	USED(c);
	USED(f);
	USED(t);
	return nil;
}
