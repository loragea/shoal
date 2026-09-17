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
 * for a string that did not already carry one.
 */
char Enotbuilt[]	= "shoalsrv: not built";
char Ebadaname[]	= "bad aname";
char Estaleepoch[]	= "stale epoch";
char Efutureepoch[]	= "future epoch";
char Eperm[]		= "permission denied";
char Efenced[]		= "fenced";
char Ebadctl[]		= "bad ctl";
char Eunknownctl[]	= "unknown ctl";
char Ebadname[]		= "bad object name";
char Enoobj[]		= "no such object";
char Ecsum[]		= "checksum mismatch";

/*
 * Not a §2.6 string: what lib9p's reqqueueflush answers a request it
 * removed from a queue, and what a handler answers one it found
 * flushed (layer-a §5.4.1, store.md §14(14)).  It is also what the
 * device answers a system call a note interrupted (store.md §0), and
 * the two must not be confused: the queue's flush flag is what says a
 * request was flushed, never the text of an error.  srvqcheck is the
 * only thing that reads the flag and the only thing that produces this
 * string; a device `interrupted' arriving anywhere else is an ordinary
 * internal error and is marked like one.
 */
char Einterrupted[]	= "interrupted";

/* layer-a §2.6, exactly.  The set is prefix-free; this is the check. */
static char *e26[] =
{
	"no such object",
	"object exists",
	"object deleted",
	"object too large",
	"object lost",
	"object unavailable",
	"not ready",
	"bad object name",
	"reserved name",
	"bad create mode",
	"bad open mode",
	"no rename",
	"permission denied",
	"stale epoch",
	"future epoch",
	"not primary",
	"not discardable",
	"fenced",
	"down",
	"degraded",
	"stale version",
	"out of sequence",
	"checksum mismatch",
	"still placed",
	"disk full",
	"bad ctl",
	"unknown ctl",
	"bad aname",
	"bad map",
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

void
srvrerror(Req *r)
{
	char buf[ERRMAX], err[ERRMAX];

	rerrstr(err, sizeof err);
	respond(r, srverrs(buf, sizeof buf, err));
}
