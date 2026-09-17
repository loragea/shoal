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
 * /ctl, layer-a §2.5.  One command per Twrite, one physical line; a
 * partial line MUST be rejected with `bad ctl'; the write's offset is
 * ignored; success returns an Rwrite count equal to the bytes written.
 *
 * The framework is the gates and the table.  Every verb of §2.5 has a
 * row, whether or not its body is built, because the gates are the
 * part a client can observe and they are complete now: the role gate
 * (`permission denied'), the fence gate (`fenced'), the spelling
 * (`unknown ctl') and the arguments (`bad ctl') all answer before a
 * body runs.  A verb is built by filling its row's fn or qfn; nothing
 * else in this file changes.
 *
 * `fence is inside the fence' (§2.5) is the one rule with an edge.
 * §2.5 lists `fence off' among the verbs that MUST fail `fenced' while
 * the instance is fenced, and §6.4 F4 makes `fence off' the only way
 * to clear an operator fence.  Read literally the two make an operator
 * fence permanent — the verb that clears it is refused because it is
 * in force.  What §2.5 is protecting is a deposed instance unfencing
 * itself, which is the lease fence F1 raises, so `fence off' here is
 * refused while a LEASE fence is in force and clears the operator flag
 * otherwise.  store.md §14(26) and decisions.md D25 record it.
 */

enum
{
	Nctlarg	= 8,
};

static char*	ctlfence(Srvctx*, Sfid*, int, char**);
static char*	ctlnotbuilt(Srvctx*, Sfid*, int, char**);
static void	ctlverify(Req*);

/*
 *	verb	    roles   fenced nargmin nargmax  fn		qfn
 */
Sctl srvctls[] =
{
	{"refresh",	Aadmin,	0,	0, 0,	ctlnotbuilt,	nil},
	{"register",	Aadmin,	0,	0, 0,	ctlnotbuilt,	nil},
	{"pull",	Aadmin,	1,	2, 2,	ctlnotbuilt,	nil},
	{"push",	Aadmin,	1,	2, 2,	ctlnotbuilt,	nil},
	{"reconcile",	Aadmin,	1,	0, 1,	ctlnotbuilt,	nil},
	{"advert",	Aadmin,	1,	0, 1,	ctlnotbuilt,	nil},
	{"drop",	Aadmin,	1,	1, 1,	ctlnotbuilt,	nil},
	{"verify",	Aadmin,	0,	1, 1,	nil,		ctlverify},
	{"scrub",	Aadmin,	0,	0, 2,	ctlnotbuilt,	nil},
	{"forget",	Aadmin,	1,	1, 1,	ctlnotbuilt,	nil},
	{"fence",	Aadmin,	0,	1, 1,	ctlfence,	nil},
	{"newmonid",	Aadmin,	0,	1, 1,	ctlnotbuilt,	nil},
};
int nsrvctls = nelem(srvctls);

int
srvfencekind(Srvctx *c)
{
	int k;

	qlock(&c->fencelk);
	/*
	 * §6.4 F1 is inert while the map is static: there is no refresh
	 * to succeed, so a real clock would fence this instance one
	 * `leasems' after it started and it would never come back.  The
	 * clock handed to fencekind is therefore the time of the last
	 * successful refresh itself, which is the start-up adoption, and
	 * the lease can never elapse.  F4, the operator fence, is live.
	 * A refresh loop replaces this argument with the monotonic clock
	 * and nothing else here changes.  store.md §14(19) records it.
	 */
	k = fencekind(&c->fence, c->fence.last);
	qunlock(&c->fencelk);
	return k;
}

static char*
ctlnotbuilt(Srvctx *c, Sfid *f, int argc, char **argv)
{
	USED(c);
	USED(f);
	USED(argc);
	USED(argv);
	return Enotbuilt;
}

static char*
ctlfence(Srvctx *c, Sfid *f, int argc, char **argv)
{
	USED(f);
	USED(argc);
	if(strcmp(argv[0], "on") == 0){
		qlock(&c->fencelk);
		fenceoperator(&c->fence, 1);
		qunlock(&c->fencelk);
		return nil;
	}
	if(strcmp(argv[0], "off") == 0){
		if(srvfencekind(c) & Fencelease)
			return Efenced;
		qlock(&c->fencelk);
		fenceoperator(&c->fence, 0);
		qunlock(&c->fencelk);
		return nil;
	}
	return Ebadctl;
}

/*
 * §2.5's `verify <oid>': re-hash and compare now.  It is object work,
 * so it runs on the oid's queue like every other operation on that
 * object (§5.4.1), and it is not in the fenced set — §6.4 leaves
 * `verify' available to an operator whose instance is fenced, which is
 * how a fence is diagnosed.
 *
 * objverify answers the set of mismatching blocks and, separately,
 * whether the digest array itself is suspect; which of those is a wire
 * error is this server's decision, and §2.5's row makes both of them
 * `checksum mismatch'.
 */
static void
ctlverify(Req *r)
{
	char buf[ERRMAX];
	Srvctx *c;
	Qreq *qr;
	Vfy v;
	int bad;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	qr = r->aux;
	if(objverify(c->store, qr->oid, qr->oidlen, &v) < 0){
		srvqdone(r, srverr(buf, sizeof buf));
		return;
	}
	bad = v.arraybad || v.nbad > 0;
	vfyfree(&v);
	if(bad){
		srvqdone(r, Ecsum);
		return;
	}
	r->ofcall.count = r->ifcall.count;
	srvqdone(r, nil);
}

/*
 * One physical line, and nothing else.  A write whose bytes hold a
 * newline anywhere but at the very end is more than one line or the
 * start of a second, and a write with no content at all is not a
 * command: both are §2.5's `bad ctl'.  A single trailing newline is
 * accepted, because that is what an operator's echo sends.
 */
static Cmdbuf*
ctlparse(void *a, long n)
{
	Cmdbuf *cb;
	char *p;
	char **f;
	int i;

	if(n <= 0)
		return nil;
	p = a;
	for(i = 0; i < n-1; i++)
		if(p[i] == '\n')
			return nil;
	if(p[n-1] == '\n')
		n--;
	if(n <= 0)
		return nil;
	cb = mallocz(sizeof *cb + Nctlarg*sizeof(char*) + n+1, 1);
	if(cb == nil)
		return nil;
	f = (char**)(cb + 1);
	cb->f = f;
	cb->buf = (char*)(f + Nctlarg);
	memmove(cb->buf, a, n);
	cb->buf[n] = 0;
	cb->nf = tokenize(cb->buf, cb->f, Nctlarg);
	if(cb->nf < 1){
		free(cb);
		return nil;
	}
	return cb;
}

void
srvctlwrite(Req *r)
{
	char *e;
	Srvctx *c;
	Sfid *f;
	Sctl *ct;
	Cmdbuf *cb;
	Qreq *qr;
	uchar oid[Oidmax];
	int i, oidlen;

	c = r->srv->aux;
	f = r->fid->aux;
	if((cb = ctlparse(r->ifcall.data, r->ifcall.count)) == nil){
		respond(r, Ebadctl);
		return;
	}
	for(i = 0; i < nsrvctls; i++)
		if(strcmp(srvctls[i].verb, cb->f[0]) == 0)
			break;
	if(i >= nsrvctls){
		free(cb);
		respond(r, Eunknownctl);
		return;
	}
	ct = &srvctls[i];
	if((ct->roles & (1<<f->role)) == 0){
		free(cb);
		respond(r, Eperm);
		return;
	}
	if(ct->fenced && srvfencekind(c) != Fencenone){
		free(cb);
		respond(r, Efenced);
		return;
	}
	if(cb->nf-1 < ct->nargmin || cb->nf-1 > ct->nargmax){
		free(cb);
		respond(r, Ebadctl);
		return;
	}
	if(ct->qfn != nil){
		/*
		 * A queued verb names its object in argv[0], so its row must
		 * ask for at least one argument; a row that does not is
		 * refused here rather than read past the end of the line
		 * (dat.h's Sctl).
		 */
		if(ct->nargmin < 1){
			free(cb);
			respond(r, Ebadctl);
			return;
		}
		/*
		 * layer-a §2.6
		 * makes `bad object name' the answer to "any operation
		 * naming an oid that violates §1.1"; §2.5's rows answer bad
		 * arguments `bad ctl'.  The more specific string wins here
		 * (store.md §14(27)).
		 *
		 * An over-long id is one of those violations and is refused
		 * whole.  Cutting it to Oidmax first would leave the verb
		 * naming a different object — one that may well exist — so
		 * the length is the id's, never the buffer's.
		 */
		oidlen = strlen(cb->f[1]);
		if(!srvoidok((uchar*)cb->f[1], oidlen)){
			free(cb);
			respond(r, Ebadname);
			return;
		}
		memmove(oid, cb->f[1], oidlen);
		if((qr = srvqprep(c, oid, oidlen, r, ct->qfn)) == nil){
			free(cb);
			return;
		}
		qr->ctl = ct;
		qr->cb = cb;
		srvqgo(c, r);
		return;
	}
	e = ct->fn(c, f, cb->nf-1, cb->f+1);
	free(cb);
	if(e != nil){
		respond(r, e);
		return;
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}
