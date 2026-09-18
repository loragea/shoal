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
 * /status and /map, layer-a §2.2.  Both are rendered at open (§2.2's
 * MUST for the status files) into the fid's Text, so a read cannot be
 * torn by a concurrent mutation, and both are answered on the service
 * loop: neither names an object, so neither is queued (§5.4.1).
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
 * §14(23) records the omission.  Two fields beyond §2.2's list are
 * rendered because something here has no other place to report them:
 * the open enumeration-snapshot count, which store.md §9 makes the
 * server's half of `objsnap=', and the queue pool's depth, which §7
 * asks /status to report so saturation is visible rather than folklore
 * — Reqqueue keeps no count, so these are the server's own.
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

/* /ctl reads answer no bytes; the verbs are a write surface (§2.5) */
char*
srvemptytext(Srvctx *c, Sfid *f, Text *t)
{
	USED(c);
	USED(f);
	USED(t);
	return nil;
}
