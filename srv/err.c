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
 * docs/design/store.md §3.7's mapping rule, which is normative: a
 * condition layer-a §2.6 names MUST be answered with §2.6's prefix and
 * nothing else, and an internal-invariant or device error MUST NEVER
 * begin with one.  §2.6's set is prefix-free, so a client that parses
 * a prefix out of an internal string would read a bug or a media fault
 * as an ordinary refusal; the whole point of the rule is that it
 * cannot.
 *
 * The engine already spells its wire errors exactly as §2.6 spells
 * them, so the mapping is a classification and not a translation:
 *
 *	a string that begins with a §2.6 prefix — either the prefix
 *	alone, or the prefix followed by ": " and detail, which is
 *	§2.6's own `not primary: n5.0' and D20's `disk full: <n> object
 *	snapshots open, objsnapmax <max>' — goes to the wire verbatim;
 *
 *	everything else is internal, and goes to the wire under this
 *	server's own prefix, `shoalsrv: '.  `store closed', `store
 *	condemned: …', `i/o error', `stage expired' and `Eobj: …' are
 *	that case, and so is anything a library answers that this server
 *	did not anticipate — an out-of-memory from mapparse, say, which
 *	lib/shoal.h warns is not `bad map'.
 *
 * Marking rather than passing through is a choice: §3.7 leaves what
 * the server does with an internal string to the server, and a fixed
 * local prefix makes the rule visible on the wire and testable from a
 * client.  `shoalsrv' shares no prefix with any §2.6 entry, so the
 * marked set stays prefix-free against §2.6's.  The one thing this
 * function will not do under any input is manufacture a §2.6 prefix
 * for a string that did not already carry one.  store.md §14(29)
 * records the marking and the single `not built' string beside it.
 */
char Enotbuilt[]	= "shoalsrv: not built";

/*
 * layer-a §2.6's set, whole and in §2.6's order — the strings no
 * handler is built for yet included, so that a unit adding one of
 * those handlers names a string that is already here rather than
 * appending to this block.  e26 below is the same set, read as data
 * for the classifier, so the two cannot drift apart.
 */
char Enoobj[]		= "no such object";
char Eexists[]		= "object exists";
char Edeleted[]		= "object deleted";
char Etoobig[]		= "object too large";
char Elost[]		= "object lost";
char Eunavail[]		= "object unavailable";
char Enotready[]	= "not ready";
char Ebadname[]		= "bad object name";
char Ereserved[]	= "reserved name";
char Ebadcreate[]	= "bad create mode";
char Ebadopen[]		= "bad open mode";
char Enorename[]	= "no rename";
char Eperm[]		= "permission denied";
char Estaleepoch[]	= "stale epoch";
char Efutureepoch[]	= "future epoch";
char Enotprimary[]	= "not primary";
char Enotdisc[]		= "not discardable";
char Efenced[]		= "fenced";
char Edown[]		= "down";
char Edegraded[]	= "degraded";
char Estalever[]	= "stale version";
char Eoutofseq[]	= "out of sequence";
char Ecsum[]		= "checksum mismatch";
char Estillplaced[]	= "still placed";
char Ediskfull[]	= "disk full";
char Ebadctl[]		= "bad ctl";
char Eunknownctl[]	= "unknown ctl";
char Ebadaname[]	= "bad aname";
char Ebadmap[]		= "bad map";

/*
 * Not a §2.6 string: what lib9p's reqqueueflush answers a request it
 * removed from a queue, and what srvqdone answers one it found flushed
 * (layer-a §5.4.1, store.md §14(14)).  It is also what the device
 * answers a system call a note interrupted (store.md §0), and the two
 * causes must stay apart on the wire even though §7 unwinds both into
 * the whole of step 7: the queue's flush flag is what says a request
 * was flushed, never the text of an error, so a device `interrupted'
 * with no flush pending is answered under this server's own prefix
 * like any other error it did not anticipate.
 *
 * srvqdone is what reads the flag, what classifies the error and what
 * produces both strings; srvqcheck only reports the flag to a handler
 * that wants to stop early.
 */
char Einterrupted[]	= "interrupted";
char Edevintr[]		= "shoalsrv: interrupted";

/*
 * Not a §2.6 string either: lib9p's own refusal of a message that does
 * not belong on this fid.  lib9p answers it from `Fid.omode' and so
 * cannot see a conflict between two requests on one fid while the
 * first of them is still offloaded and has not set that field — a
 * Topen and a Tcreate pipelined on one /obj fid, which the two cells
 * refuse between them instead (dat.h).  They answer this, because the
 * condition is the one lib9p names and a client that could tell the
 * two refusals apart would be reading which of them got there first.
 */
char Ebotch[]		= "9P protocol botch";

/*
 * Is e the device's interrupted class (store.md §0)?  The rule is
 * deverr's, applied the same way, because this is the same condition
 * arriving one layer up: the last `: '-separated segment is taken —
 * a device call reports which syscall failed on which device, and
 * this server marks what it passes on, so the kernel's own word is
 * what follows the last wrap — and the word is looked for inside it,
 * as lib/dev.c does, rather than matched whole.  `shoalsrv:
 * interrupted', a bare `interrupted' and a segment that wraps the
 * word once more are all that class; `no such object' is not.
 */
int
srvintr(char *e)
{
	char *p;

	if(e == nil)
		return 0;
	if((p = strrchr(e, ':')) != nil && p[1] == ' ')
		p += 2;
	else
		p = e;
	return strstr(p, Einterrupted) != nil;
}

/* layer-a §2.6, exactly: the block above, read as data.  Prefix-free. */
static char *e26[] =
{
	Enoobj,
	Eexists,
	Edeleted,
	Etoobig,
	Elost,
	Eunavail,
	Enotready,
	Ebadname,
	Ereserved,
	Ebadcreate,
	Ebadopen,
	Enorename,
	Eperm,
	Estaleepoch,
	Efutureepoch,
	Enotprimary,
	Enotdisc,
	Efenced,
	Edown,
	Edegraded,
	Estalever,
	Eoutofseq,
	Ecsum,
	Estillplaced,
	Ediskfull,
	Ebadctl,
	Eunknownctl,
	Ebadaname,
	Ebadmap,
};

/*
 * The §2.6 prefix e carries, or nil.  A prefix matches when it is the
 * whole string or is followed by ": " — §2.6's detail form.  A string
 * that begins with a prefix and then runs on in any other way is not
 * that condition: `no such objects here' is not `no such object'.
 */
char*
srv26(char *e)
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
 */
char*
srverrs(char *buf, int nbuf, char *e)
{
	if(e == nil || *e == 0)
		e = "unknown error";
	if(srv26(e) != nil)
		return e;
	/* lib9p's own, which goes out as lib9p writes it (above) */
	if(strcmp(e, Ebotch) == 0)
		return e;
	/* already marked: marking twice would say nothing twice */
	if(strncmp(e, "shoalsrv: ", 10) == 0)
		return e;
	snprint(buf, nbuf, "shoalsrv: %s", e);
	return buf;
}

/*
 * The answer is always in buf here, even for a string that passes
 * verbatim: the caller's buffer is what outlives this frame, and a
 * handler holds the answer across its respond.
 */
char*
srverr(char *buf, int nbuf)
{
	char err[ERRMAX], *e;

	rerrstr(err, sizeof err);
	e = srverrs(buf, nbuf, err);
	if(e != buf)
		utfecpy(buf, buf+nbuf, e);
	return buf;
}

/*
 * A request that was pushed to a queue has ONE exit, srvqdone, which
 * is where the flush flag is tested and step 7 performed (queue.c).
 * This function is the error API's own exit and is exported, so it is
 * reached from a queue proc as readily as from the service loop; it
 * therefore delegates rather than responding, whenever the request is
 * one the pool is carrying — r->aux is the Qreq srvqprep armed.  A
 * handler that answered here directly would skip the flush test and
 * step 7 and leave a flushed request answered `no such object'.
 */
void
srvrerror(Req *r)
{
	char buf[ERRMAX], err[ERRMAX];

	rerrstr(err, sizeof err);
	if(r->aux != nil){
		srvqdone(r, err);
		return;
	}
	respond(r, srverrs(buf, sizeof buf, err));
}
