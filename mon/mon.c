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
 * Start-up, the Srv glue, the shutdown and layer-a §8.4's evidence
 * registry.
 *
 * Start-up is here rather than in cmd/shoalmon because its refusals
 * are rules from the design — the slot store's own two (store.md §10:
 * no valid header, no valid current slot) and this server's refusal of
 * a current map that does not parse — and a T1 program must be able to
 * drive them.  What is left in the command is argument parsing,
 * opening the device and posting the service.
 *
 * There are no procs here.  decisions.md D13 and store.md §10 put the
 * monitor's durability in a small raw partition that monopen reads
 * WHOLE into memory, and every accessor this unit uses — moncurrent,
 * monhistory, monlookup, monstat — answers out of that memory.  So no
 * handler of this service can block on the device, and lib9p's own
 * service loop is the whole of the concurrency: no Reqqueue pool, no
 * offload, no Tflush handler.  That is the one structural difference
 * from srv/, where an object read is a disk read and store.md §7's
 * pool exists to keep it off the loop.  The unit that builds the
 * durable publish adds the one call that does reach the device,
 * moncommit, under monlk.
 */

static void
monend(Srv *s)
{
	monsrvshutdown(s->aux);
}

/*
 * Srv.free, which lib9p calls at the very end of its own srvclose —
 * after the fid pool and the request pool have been freed, and so
 * after the last destroyfid.  It is the only moment at which this
 * context is certainly no longer in use, which is what monsrvfree
 * waits for.
 */
static void
monfreed(Srv *s)
{
	Monctx *c;

	c = s->aux;
	lock(&c->statelk);
	c->released = 1;
	unlock(&c->statelk);
}

/*
 * Re-parse the current map.  The caller holds monlk.
 *
 * A store that holds no map leaves the parse nil, which is not a
 * failure: that is a monitor formatted and not yet published to, and
 * §8.1's files answer it — /map with a refusal, the record files with
 * no records, /status with `hasmap=no' (store.md §14(54)).  A map text
 * that is THERE and does not parse is a failure, because every file of
 * §8.1 but /map is rendered out of the parse and a monitor that cannot
 * read its own map has no business publishing epochs against it.
 */
int
monsrvremap(Monctx *c)
{
	Monmap mm;
	Cmap *m;

	mapfree(c->map);
	c->map = nil;
	if(!moncurrent(c->mon, &mm))
		return 0;
	if((m = mapparse((char*)mm.text, mm.len)) == nil)
		return -1;
	c->map = m;
	return 0;
}

/*
 * Start-up's two refusals go out through §3.7's classifier, because
 * what they carry comes from libshoal and not from here.  The slot
 * store answers `no valid monitor header: …' and `no valid
 * current-map slot: …', which name no §2.6 condition and are marked
 * `shoalmon: '; it also answers `disk full: …', which IS one and
 * passes verbatim.  mapparse answers `bad map: …' for a text that
 * does not conform — §2.6's own — and whatever mallocz left for an
 * allocation failure, which lib/shoal.h warns is NOT `bad map' and
 * which §3.7 forbids dressing as one.  One classifier decides which
 * of those a string is (store.md §14(59)).
 */
Monctx*
monsrvnew(Moncfg *cfg)
{
	char err[ERRMAX];
	Monctx *c;

	if((c = mallocz(sizeof *c, 1)) == nil)
		return nil;
	c->cfg = *cfg;
	c->dev = cfg->dev;
	c->t0 = time(0);

	/*
	 * store.md §10 and decisions.md D13: the service opens the
	 * partition and serves what is there.  monopen writes nothing,
	 * works on a read-only device and marks the phantoms of §10 —
	 * the ring entries of a publish that never completed — so that
	 * every accessor ignores them.  This unit never calls moncommit,
	 * so a monitor started here changes not one byte of its own
	 * partition.
	 */
	if((c->mon = monopen(c->dev)) == nil){
		monsrverr(err, sizeof err);
		free(c);
		werrstr("%s", err);
		return nil;
	}
	if(monsrvremap(c) < 0){
		monsrverr(err, sizeof err);
		monclose(c->mon);
		mapfree(c->map);
		free(c);
		werrstr("%s", err);
		return nil;
	}

	c->srv.aux = c;
	c->srv.attach = monsrvattach;
	c->srv.walk = monsrvwalk;
	c->srv.open = monsrvopen;
	c->srv.read = monsrvread;
	c->srv.write = monsrvwrite;
	c->srv.stat = monsrvstat;
	c->srv.create = monsrvcreate;
	c->srv.remove = monsrvremove;
	c->srv.wstat = monsrvwstat;
	c->srv.destroyfid = monsrvdestroyfid;
	c->srv.end = monend;
	c->srv.free = monfreed;
	/*
	 * Srv.flush is deliberately nil.  lib9p answers a Tflush itself
	 * when no handler is installed, and with every handler running to
	 * completion on the service loop there is never a request
	 * outstanding for a Tflush to find: layer-a §5.4.1's step 7,
	 * which is what srv/'s flush hook exists for, is about an
	 * operation parked on an object's queue, and this service has
	 * neither.
	 */
	return c;
}

Srv*
monsrv9p(Monctx *c)
{
	return &c->srv;
}

static void
monserved(Monctx *c)
{
	lock(&c->statelk);
	c->served = 1;
	unlock(&c->statelk);
}

void
monsrvrun(Monctx *c, int infd, int outfd)
{
	c->srv.infd = infd;
	c->srv.outfd = outfd;
	monserved(c);
	threadsrv(&c->srv);
}

/*
 * Post the service and return.  threadpostmountsrv writes the
 * directory entry and leaves the loop to a proc of its own, so this
 * returns with nothing served yet and the context in that proc's hands
 * until the connection it serves ends — which is why it marks nothing
 * served and monsrvfree on a posted context waits for nothing.
 */
void
monsrvpost(Monctx *c, char *name)
{
	threadpostmountsrv(&c->srv, name, nil, 0);
}

/*
 * The shutdown, from Srv.end — which lib9p calls once the connection
 * has gone and the service loop has returned.  Every handler of this
 * service runs to completion on that loop, so by here there is no
 * request in flight and nothing to drain: the whole of it is closing
 * the slot store and letting the parsed map go.
 *
 * The fids outlive this and that is safe, which is the other half of
 * having no queues: what a fid holds is a copy of bytes and a copy of
 * ring positions (dat.h), so destroyfid after the store has closed is
 * the same call as destroyfid before it.  srv/'s shutdown has to sweep
 * its live fids before storeclose because a fid there can hold an
 * engine handle; nothing here can.
 *
 * The seam this leaves is `mon' going nil under the lock.  No handler
 * can see it today — Srv.end runs after the loop has returned, so
 * there is no request in flight — but every accessor in lib/mon.c
 * dereferences its Mon unguarded, so a later unit that runs a
 * shutdown while a handler is on another proc (a Reqqueue for the
 * durable publish is the obvious one) would fault rather than answer.
 * Every reader of `mon' in this library therefore takes it under
 * monlk and treats nil as a store holding no map: /map and
 * /maps/<epoch> refuse, /maps lists nothing, /status renders
 * `hasmap=no' and a /map read renews no lease.  That is the cheap
 * half of the guarantee, taken now; the expensive half — whether a
 * shutdown may run at all while a handler is outstanding — belongs
 * to the unit that creates the second proc.
 */
void
monsrvshutdown(Monctx *c)
{
	lock(&c->statelk);
	if(c->stopping){
		unlock(&c->statelk);
		return;
	}
	c->stopping = 1;
	unlock(&c->statelk);
	qlock(&c->monlk);
	mapfree(c->map);
	c->map = nil;
	monclose(c->mon);
	c->mon = nil;
	c->closed = 1;
	qunlock(&c->monlk);
}

/*
 * Wait for lib9p to let go of the Srv this context begins with.  The
 * shutdown above is not that moment: it runs from Srv.end, which lib9p
 * calls while the loop's own reference is still held, and the release
 * then takes lib9p through freefidpool, which runs this library's
 * destroyfid over every fid still open.  Freeing the context before
 * that would pull the table those calls read out from under them.
 *
 * Srv.free is lib9p's last act, so `released' is the observable; a
 * context that never ran a loop in this proc waits for nothing, which
 * is what a caller that posted the service gets (monsrvpost).
 */
int
monsrvreleased(Monctx *c)
{
	int n;

	lock(&c->statelk);
	n = !c->served || c->released;
	unlock(&c->statelk);
	return n;
}

void
monsrvfree(Monctx *c)
{
	if(c == nil)
		return;
	monsrvshutdown(c);
	while(!monsrvreleased(c))
		sleep(5);
	free(c->seen);
	free(c);
}

Mon*
monsrvmon(Monctx *c)
{
	return c->mon;
}

void
monsrvlock(Monctx *c)
{
	qlock(&c->monlk);
}

void
monsrvunlock(Monctx *c)
{
	qunlock(&c->monlk);
}

/*
 * layer-a §8.4's evidence, recorded and not acted on.
 *
 * "lastseen(i) is the time of instance i's most recent successful read
 * of the monitor's /map, or its most recent `register', on an attach
 * with role=instance,peer=i."  The read half is built and is
 * tree.c's; the `register' half is the next unit's, and it records
 * evidence by calling monsrvseen from that verb's body.
 *
 * store.md §14(56) fixes what a successful read is and why, and is
 * what the demotion rule of the unit after next will stand on: it
 * compares now − lastseen(i) against the map's `deadms' and publishes
 * up=no fenced=yes for an instance whose channel has been silent that
 * long.  Nothing here demotes anything, and nothing here consults the
 * map: an iid is recorded whoever it names, because §3.4 step 2 has an
 * unregistered disk attach role=instance before any map knows it
 * (attach.c).  Evidence for an iid no map carries is held and rendered
 * nowhere until the map does carry it.
 *
 * The table is small — one entry per instance that has ever refreshed
 * here — and is searched linearly, which at §4.1's 3–12 node envelope
 * is the whole of the data structure question.  It is under its own
 * Lock and not under monlk, because a /map read must not wait behind a
 * publish to record that it happened; the two renders that need both
 * take monlk first (mon.h).
 *
 * The clock is time(2)'s seconds, which is what §8.1's
 * `lastseen=<u64 seconds>' and `lastrefresh=<u64 seconds>' ask for, so
 * /health's `silent=<ms>' is a multiple of 1000 (store.md §14(61)).
 */
void
monsrvseen(Monctx *c, char *iid)
{
	Mseen *p;
	int i, n;

	if(iid == nil || *iid == 0)
		return;
	lock(&c->seenlk);
	for(i = 0; i < c->nseen; i++)
		if(strcmp(c->seen[i].iid, iid) == 0){
			c->seen[i].t = time(0);
			unlock(&c->seenlk);
			return;
		}
	if(c->nseen >= c->maxseen){
		n = c->maxseen == 0 ? 8 : 2*c->maxseen;
		if((p = realloc(c->seen, n*sizeof *p)) == nil){
			unlock(&c->seenlk);
			return;
		}
		c->seen = p;
		c->maxseen = n;
	}
	p = &c->seen[c->nseen++];
	memset(p, 0, sizeof *p);
	strecpy(p->iid, p->iid + sizeof p->iid, iid);
	p->t = time(0);
	unlock(&c->seenlk);
}

uvlong
monsrvlastseen(Monctx *c, char *iid)
{
	uvlong t;
	int i;

	t = 0;
	lock(&c->seenlk);
	for(i = 0; i < c->nseen; i++)
		if(strcmp(c->seen[i].iid, iid) == 0){
			t = c->seen[i].t;
			break;
		}
	unlock(&c->seenlk);
	return t;
}
