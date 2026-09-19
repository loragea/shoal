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
 * docs/design/store.md §3.7's mapping rule, which is normative: a
 * condition layer-a §2.6 names MUST be answered with §2.6's prefix and
 * nothing else, and an internal-invariant or device error MUST NEVER
 * begin with one.  §2.6's set is prefix-free, and §2.6 says in terms
 * that it is ONE set shared by storage instances and the monitor —
 * "`bad map' is only ever emitted by the monitor" — so the monitor's
 * own local strings need a prefix of their own beside `shoalsrv: ',
 * and it is `shoalmon: '.  Neither shares a prefix with any §2.6 entry
 * or with the other, so the three sets stay prefix-free against each
 * other and a client library parses errors from both servers with one
 * rule.  store.md §14(59) records the prefix and the four local
 * strings below.
 *
 * This file is srv/err.c's classifier over §2.6's set again, under
 * this library's own names.  The two libraries are independent by
 * design (mon.h) and neither links the other, so §2.6's block appears
 * twice; what keeps the two copies honest is that §2.6 is a closed
 * normative list and e26 below is that list read as data.
 *
 * Who calls it.  Every wire error this server composes rather than
 * names — the allocation failures in the attach, the walk and the
 * render-at-open — goes out through monsrverrs, so the prefix is the
 * classifier's decision and not a string typed by hand; and
 * monsrvnew's two start-up refusals go through monsrverr, because
 * what they carry is libshoal's (mon.c).  The strings this file
 * declares are answered by name, which is why the classifier passes
 * an already-marked string through unchanged.
 */

/*
 * The local strings.  `not built' is what a file or a ctl verb whose
 * body is not built answers after its gates — the whole of §8.3 and
 * the /map.next write in this unit.
 *
 * `no such file' is a name in the root, or in /maps, that this tree
 * does not have; `no such epoch' is a /maps element that IS a u64 and
 * names a map this store no longer retains (§8.2's `retain' ring).
 * They are two strings and not one on purpose: layer-a §5.2 clause 2
 * has an instance walk to /maps/<E−1>, and an instance that is told
 * the epoch has aged out of the ring has learned something different
 * from one that spelled the name wrong.  store.md §14(55) records it.
 *
 * `no map' is what a /map open answers on a store that holds no map at
 * all — a monitor formatted and not yet published to (store.md
 * §14(54)).
 */
char Emonnotbuilt[]	= "shoalmon: not built";
char Emonnofile[]	= "shoalmon: no such file";
char Emonnomap[]	= "shoalmon: no map";
char Emonnoepoch[]	= "shoalmon: no such epoch";

/*
 * layer-a §2.6's set, whole and in §2.6's order — the strings the
 * monitor has no handler for yet included, so that a unit adding one
 * of those handlers names a string that is already here rather than
 * appending to this block.  e26 below is the same set read as data for
 * the classifier, so the two cannot drift apart.
 *
 * Six of them are the monitor's own to emit: `permission denied' for
 * a role violation (§8.1), `bad ctl' and `unknown ctl' for §8.3,
 * `bad aname' for §8.1's attach grammar, `bad map' for §8.1's commit
 * validation, and `disk full' for an oversize map (store.md §10).
 * The other twenty-three are a storage instance's.
 */
char Emnoobj[]		= "no such object";
char Emexists[]		= "object exists";
char Emdeleted[]	= "object deleted";
char Emtoobig[]		= "object too large";
char Emlost[]		= "object lost";
char Emunavail[]	= "object unavailable";
char Emnotready[]	= "not ready";
char Embadname[]	= "bad object name";
char Emreserved[]	= "reserved name";
char Embadcreate[]	= "bad create mode";
char Embadopen[]	= "bad open mode";
char Emnorename[]	= "no rename";
char Emperm[]		= "permission denied";
char Emstaleepoch[]	= "stale epoch";
char Emfutureepoch[]	= "future epoch";
char Emnotprimary[]	= "not primary";
char Emnotdisc[]	= "not discardable";
char Emfenced[]		= "fenced";
char Emdown[]		= "down";
char Emdegraded[]	= "degraded";
char Emstalever[]	= "stale version";
char Emoutofseq[]	= "out of sequence";
char Emcsum[]		= "checksum mismatch";
char Emstillplaced[]	= "still placed";
char Emdiskfull[]	= "disk full";
char Embadctl[]		= "bad ctl";
char Emunknownctl[]	= "unknown ctl";
char Embadaname[]	= "bad aname";
char Embadmap[]		= "bad map";

static char *e26[] =
{
	Emnoobj,
	Emexists,
	Emdeleted,
	Emtoobig,
	Emlost,
	Emunavail,
	Emnotready,
	Embadname,
	Emreserved,
	Embadcreate,
	Embadopen,
	Emnorename,
	Emperm,
	Emstaleepoch,
	Emfutureepoch,
	Emnotprimary,
	Emnotdisc,
	Emfenced,
	Emdown,
	Emdegraded,
	Emstalever,
	Emoutofseq,
	Emcsum,
	Emstillplaced,
	Emdiskfull,
	Embadctl,
	Emunknownctl,
	Embadaname,
	Embadmap,
};

/*
 * The §2.6 prefix e carries, or nil.  A prefix matches when it is the
 * whole string or is followed by ": " — §2.6's detail form.  A string
 * that begins with a prefix and then runs on in any other way is not
 * that condition: `bad maps here' is not `bad map'.
 */
char*
monsrv26(char *e)
{
	char *p;
	int i, n;

	if(e == nil)
		return nil;
	for(i = 0; i < nelem(e26); i++){
		p = e26[i];
		n = strlen(p);
		if(strncmp(e, p, n) != 0)
			continue;
		if(e[n] == 0 || (e[n] == ':' && e[n+1] == ' '))
			return p;
	}
	return nil;
}

/*
 * Map one error string onto what goes on the wire.  buf is written
 * only when the string has to be marked; the answer is buf or e.
 *
 * The one thing this will not do under any input is manufacture a
 * §2.6 prefix for a string that did not already carry one.  lib/mon.c
 * and lib/map.c both answer strings that are not §2.6 conditions —
 * `no valid current-map slot: …', an allocation failure out of
 * mapparse — and those go out marked.
 */
char*
monsrverrs(char *buf, int nbuf, char *e)
{
	if(e == nil || *e == 0)
		e = "unknown error";
	if(monsrv26(e) != nil)
		return e;
	/* already marked: marking twice would say nothing twice */
	if(strncmp(e, "shoalmon: ", 10) == 0)
		return e;
	snprint(buf, nbuf, "shoalmon: %s", e);
	return buf;
}

/*
 * The answer is always in buf here, even for a string that passes
 * verbatim: the caller's buffer is what outlives this frame, and a
 * handler holds the answer across its respond.
 */
char*
monsrverr(char *buf, int nbuf)
{
	char err[ERRMAX], *e;

	rerrstr(err, sizeof err);
	e = monsrverrs(buf, nbuf, err);
	if(e != buf)
		utfecpy(buf, buf+nbuf, e);
	return buf;
}
