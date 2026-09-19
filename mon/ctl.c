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
 * layer-a §8.3's ctl grammar: one command per Twrite, one physical
 * line.  This unit builds the framework and none of the bodies — the
 * verb table, the per-verb role gate, the argument-count gate and the
 * two §2.6 refusals — so every known verb answers this server's local
 * `shoalmon: not built' and an unknown one answers `unknown ctl'.
 *
 * What fills the cells, and in what order:
 *
 *	the unit that builds /map.next and the durable publish fills
 *	`propose', `enable', `disable', `retire', `rehome', `setclass',
 *	`set', `commit', `abort' and `bump' — the operator's staging
 *	surface — and `register', which is §3.4's registration and the
 *	one instance-reported verb that unit needs;
 *
 *	the unit after it fills `unreachable', `reachable', `stale',
 *	`synced', `healed' and `rebalanced' — the instance-reported
 *	verbs — together with §8.4's demotion, the promotion gate and
 *	the `bump' timer.
 *
 * `promote', `forcesync' and `forceepoch' are operator overrides that
 * each rest on a body those units build, and each MUST be logged
 * (§8.3); this server has no operator log, which is the same gap
 * store.md §14(32) records for the instance's `newmonid'.
 *
 * The Role column is §8.3's own and is gated here rather than at the
 * open: §8.3's two tables are role=instance and role=admin, and a verb
 * issued on a fid whose role does not permit it MUST fail
 * `permission denied' (§2.5's rule, which §8.3 inherits along with
 * `bad ctl' and `unknown ctl').  That is what makes the /ctl row open
 * for writing to BOTH roles worth having (tree.c): each can reach the
 * other's verbs and be refused, which is a testable rule rather than
 * an unreachable one.  `reader' has no verb in either table and is
 * refused at the open instead.
 */

enum
{
	Nctlarg	= 8,
};

static char*	ctlnotbuilt(Monctx*, Mfid*, int, char**);

/*
 * One row per verb, one field per line: a verb is built by naming the
 * cell it fills, and a row grows without its neighbours moving.  The
 * order is §8.3's — the instance-reported table first, then the
 * operator table — so the two read against the design side by side.
 */
Mctl monctls[] =
{
{
	.verb	= "register",
	.roles	= Ainstance,
	.nargmin= 4,
	.nargmax= 4,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "unreachable",
	.roles	= Ainstance,
	.nargmin= 1,
	.nargmax= 1,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "reachable",
	.roles	= Ainstance,
	.nargmin= 1,
	.nargmax= 1,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "stale",
	.roles	= Ainstance,
	.nargmin= 1,
	.nargmax= 1,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "synced",
	.roles	= Ainstance,
	.nargmin= 1,
	.nargmax= 1,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "healed",
	.roles	= Ainstance,
	.nargmin= 1,
	.nargmax= 1,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "rebalanced",
	.roles	= Ainstance,
	.nargmin= 1,
	.nargmax= 1,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "propose",
	.roles	= Amadmin,
	.nargmin= 0,
	.nargmax= 0,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "enable",
	.roles	= Amadmin,
	.nargmin= 1,
	.nargmax= 1,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "disable",
	.roles	= Amadmin,
	.nargmin= 1,
	.nargmax= 1,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "retire",
	.roles	= Amadmin,
	.nargmin= 1,
	.nargmax= 1,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "rehome",
	.roles	= Amadmin,
	.nargmin= 2,
	.nargmax= 2,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "setclass",
	.roles	= Amadmin,
	.nargmin= 2,
	.nargmax= 2,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "set",
	.roles	= Amadmin,
	.nargmin= 2,
	.nargmax= 2,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "commit",
	.roles	= Amadmin,
	.nargmin= 0,
	.nargmax= 1,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "abort",
	.roles	= Amadmin,
	.nargmin= 0,
	.nargmax= 0,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "bump",
	.roles	= Amadmin,
	.nargmin= 0,
	.nargmax= 0,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "promote",
	.roles	= Amadmin,
	.nargmin= 2,
	.nargmax= 2,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "forcesync",
	.roles	= Amadmin,
	.nargmin= 2,
	.nargmax= 2,
	.fn	= ctlnotbuilt,
},
{
	.verb	= "forceepoch",
	.roles	= Amadmin,
	.nargmin= 1,
	.nargmax= 2,
	.fn	= ctlnotbuilt,
},
};

int nmonctls = nelem(monctls);

static char*
ctlnotbuilt(Monctx *c, Mfid *f, int argc, char **argv)
{
	USED(c);
	USED(f);
	USED(argc);
	USED(argv);
	return Emonnotbuilt;
}

/*
 * One physical line, and nothing else.  A write whose bytes hold a
 * newline anywhere but at the very end is more than one line or the
 * start of a second, and a write with no content at all is not a
 * command: both are `bad ctl'.  A single trailing newline is
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

/*
 * The order of the refusals: the line, then the verb, then the verb's
 * role, then its argument count, then its body.  A malformed line and
 * a known verb with the wrong arguments are both `bad ctl', an unknown
 * spelling is `unknown ctl', and a verb the fid's role does not permit
 * is `permission denied' — whether or not the body behind it is built,
 * which is what keeps the role gate testable now and unchanged when
 * the bodies land.
 */
void
monctlwrite(Req *r)
{
	char *e;
	Monctx *c;
	Mfid *f;
	Mctl *ct;
	Cmdbuf *cb;
	int i;

	c = r->srv->aux;
	f = r->fid->aux;
	if((cb = ctlparse(r->ifcall.data, r->ifcall.count)) == nil){
		respond(r, Embadctl);
		return;
	}
	for(i = 0; i < nmonctls; i++)
		if(strcmp(monctls[i].verb, cb->f[0]) == 0)
			break;
	if(i >= nmonctls){
		free(cb);
		respond(r, Emunknownctl);
		return;
	}
	ct = &monctls[i];
	if((ct->roles & (1<<f->role)) == 0){
		free(cb);
		respond(r, Emperm);
		return;
	}
	if(cb->nf-1 < ct->nargmin || cb->nf-1 > ct->nargmax){
		free(cb);
		respond(r, Embadctl);
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
