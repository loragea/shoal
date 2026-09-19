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
 * layer-a §8.1's attach:
 *
 *	aname = attr *("," attr)
 *	attr  = "role=" ("reader" / "instance" / "admin") / "peer=" iid
 *
 * role defaults to reader.  role=instance MUST carry peer= and is the
 * attach a storage instance uses; it is the only role whose reads
 * count as liveness evidence (§8.4).  **No epoch appears here** — "the
 * monitor is where epochs come from" — which is the one difference in
 * the grammar from §2.1's, and an `epoch=' attr is therefore an
 * unknown attr and not a parsed-and-ignored one.
 *
 * The grammar is closed, so an unknown attribute, a repeated one, a
 * role spelling that is not one of the three, and a `role=instance'
 * with no `peer=' are all unparseable and answer §2.6's `bad aname' —
 * the same reading srv/attach.c takes of §2.1, and for the same
 * reason: §2.6 gives the attach no other error for a specifier the
 * server cannot use.  store.md §14(53) records it.
 *
 * An ABSENT or empty aname is role=reader and not a refusal.  That is
 * what "role defaults to reader" has to mean for the caller that sends
 * no attributes at all — a plain mount by an operator, or a client
 * library that only ever reads /map — and it is the one place this
 * grammar is more permissive than §2.1's, where an empty aname omits
 * an epoch that role=client REQUIRES.
 *
 * `peer=' is NOT checked against the map.  §3.4 step 2 has a disk that
 * has never been registered attach role=instance to write `register',
 * so it is by construction not an instance record in any map, and a
 * membership check here would make registration impossible — the
 * opposite of srv/'s §6.4 F3 check on role=repl, where a peer that is
 * not in the map is exactly the attach to refuse.  A peer= on a role
 * that does not need it parses and is ignored, because the grammar
 * permits it on any attr list.
 */
static int
miid(char *s)
{
	return *s != 0 && strlen(s) <= Iidlen;
}

int
monaname(Mfid *f, char *aname)
{
	char *fields[8], *p, *v, buf[256];
	int i, n, seen;

	f->role = Mreader;
	f->peer[0] = 0;
	if(aname == nil || *aname == 0)
		return 0;
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
		if(strcmp(p, "role") == 0){
			if(seen & 1)
				return -1;
			seen |= 1;
			if(strcmp(v, "reader") == 0)
				f->role = Mreader;
			else if(strcmp(v, "instance") == 0)
				f->role = Minstance;
			else if(strcmp(v, "admin") == 0)
				f->role = Madmin;
			else
				return -1;
		}else if(strcmp(p, "peer") == 0){
			if(seen & 2)
				return -1;
			seen |= 2;
			if(!miid(v))
				return -1;
			strcpy(f->peer, v);
		}else
			return -1;
	}
	if(f->role == Minstance && f->peer[0] == 0)
		return -1;
	return 0;
}

void
monsrvattach(Req *r)
{
	char errb[ERRMAX];
	Mfid *f;

	if((f = mallocz(sizeof *f, 1)) == nil){
		respond(r, monsrverrs(errb, sizeof errb, "out of memory"));
		return;
	}
	r->fid->aux = f;
	if(monaname(f, r->ifcall.aname) < 0){
		respond(r, Embadaname);
		return;
	}
	/*
	 * No msize floor and no epoch compare.  §8.1's aname carries no
	 * epoch, and nothing the monitor serves is sized off the
	 * negotiated msize the way §5.5's forwarded write is (srv/'s
	 * floor, store.md §14(20)): a map is read in as many Treads as
	 * the connection's msize takes.
	 */
	f->file = Qmroot;
	f->qidpath = Pmfixed + Qmroot;
	f->qidvers = 0;
	r->ofcall.qid.path = f->qidpath;
	r->ofcall.qid.vers = 0;
	r->ofcall.qid.type = QTDIR;
	r->fid->qid = r->ofcall.qid;
	respond(r, nil);
}
