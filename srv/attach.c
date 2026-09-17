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
 * layer-a §2.1's attach.  The caller's role and epoch travel in aname,
 * which is the only place they can travel without extending 9P (D2):
 *
 *	aname = attr *("," attr)
 *	attr  = "epoch=" u64 / "role=" role / "peer=" iid
 *	role  = "client" / "repl" / "admin"
 *
 * role defaults to client; epoch is REQUIRED for role=client, OPTIONAL
 * for role=repl and MAY be omitted by role=admin; role=repl MUST carry
 * peer= naming the attaching instance.  An unparseable aname MUST fail
 * with `bad aname'.
 *
 * The grammar is closed, so an unknown attribute, a repeated one, a
 * value that is not a u64 where a u64 is required, and a role spelling
 * that is not one of the three are all unparseable.  A required
 * attribute that is absent is answered the same way: §2.1 gives the
 * attach no other error for a specifier it cannot use, and the two
 * cases are one to a caller — it sent an aname this server cannot act
 * on.  store.md §14(25) records that reading.  peer= on a role that
 * does not need it parses and is ignored, because the grammar permits
 * it on any attr list.
 *
 * A run of digits too long to be a u64 is one of those values: the
 * grammar admits u64 and nothing else, so it is `bad aname' and not an
 * epoch compare against a saturated value, which would answer `future
 * epoch' for a specifier that never named an epoch at all.
 */
static int
u64(char *s, uvlong *vp)
{
	uvlong v;
	int d;

	if(*s == 0)
		return -1;
	v = 0;
	for(; *s != 0; s++){
		if(*s < '0' || *s > '9')
			return -1;
		d = *s - '0';
		if(v > (~(uvlong)0 - d)/10)
			return -1;
		v = v*10 + d;
	}
	*vp = v;
	return 0;
}

int
srvaname(Sfid *f, char *aname)
{
	char *fields[8], *p, *v, buf[256];
	int i, n, seen;

	f->role = Rclient;
	f->hasepoch = 0;
	f->epoch = 0;
	f->peer[0] = 0;
	if(aname == nil || *aname == 0)
		return -1;
	if(strlen(aname) >= sizeof buf)
		return -1;
	strcpy(buf, aname);
	n = getfields(buf, fields, nelem(fields), 0, ",");
	if(n < 1)
		return -1;
	seen = 0;
	for(i = 0; i < n; i++){
		p = fields[i];
		if((v = strchr(p, '=')) == nil)
			return -1;
		*v++ = 0;
		if(strcmp(p, "epoch") == 0){
			if(seen & 1)
				return -1;
			seen |= 1;
			if(u64(v, &f->epoch) < 0)
				return -1;
			f->hasepoch = 1;
		}else if(strcmp(p, "role") == 0){
			if(seen & 2)
				return -1;
			seen |= 2;
			if(strcmp(v, "client") == 0)
				f->role = Rclient;
			else if(strcmp(v, "repl") == 0)
				f->role = Rrepl;
			else if(strcmp(v, "admin") == 0)
				f->role = Radmin;
			else
				return -1;
		}else if(strcmp(p, "peer") == 0){
			if(seen & 4)
				return -1;
			seen |= 4;
			if(*v == 0 || strlen(v) > Iidlen)
				return -1;
			strcpy(f->peer, v);
		}else
			return -1;
	}
	if(f->role == Rclient && !f->hasepoch)
		return -1;
	if(f->role == Rrepl && f->peer[0] == 0)
		return -1;
	return 0;
}

void
srvattach(Req *r)
{
	char buf[ERRMAX];
	Srvctx *c;
	Sfid *f;

	c = r->srv->aux;
	if((f = mallocz(sizeof *f, 1)) == nil){
		respond(r, "shoalsrv: out of memory");
		return;
	}
	r->fid->aux = f;
	if(srvaname(f, r->ifcall.aname) < 0){
		respond(r, Ebadaname);
		return;
	}
	/*
	 * The msize floor.  lib9p answers Tversion itself and offers no
	 * hook for it (9p(2)), so the negotiated size is first visible to
	 * this server at Tattach, through the Srv the request carries.
	 * layer-a §5.5's forwarded-write payload is sized off it and
	 * /status reports it, so a connection below the floor is refused
	 * here rather than half-served later.  store.md §14(20) records
	 * the deviation: the refusal is late and it is not a §2.6
	 * condition, so it carries this server's own prefix.
	 */
	if(r->srv->msize < Msizemin){
		snprint(buf, sizeof buf,
			"shoalsrv: msize %ud below the %d-byte floor",
			r->srv->msize, Msizemin);
		respond(r, buf);
		return;
	}
	/*
	 * §2.1: epoch is checked at attach and not per operation.  Less
	 * than this instance's current map epoch is `stale epoch';
	 * greater is `future epoch', which SHOULD also trigger an
	 * immediate map fetch — there is no monitor client in this wave,
	 * so the refusal is the whole of what happens and the instance
	 * stays at the epoch of the map it was started with (store.md
	 * §14(21)).
	 */
	if(f->hasepoch){
		if(f->epoch < c->map->epoch){
			respond(r, Estaleepoch);
			return;
		}
		if(f->epoch > c->map->epoch){
			respond(r, Efutureepoch);
			return;
		}
	}
	/*
	 * §6.4 F3's receiver half: an instance MUST refuse, with
	 * `permission denied', any role=repl attach whose peer= is not a
	 * non-dead instance in its own current map.  That map is the
	 * static one here, and the check is live against it.
	 */
	if(f->role == Rrepl && !mapmember(c->map, f->peer)){
		respond(r, Eperm);
		return;
	}
	f->file = Qroot;
	f->qidpath = Pfixed + Qroot;
	f->qidvers = 0;
	r->ofcall.qid.path = f->qidpath;
	r->ofcall.qid.vers = 0;
	r->ofcall.qid.type = QTDIR;
	r->fid->qid = r->ofcall.qid;
	respond(r, nil);
}
