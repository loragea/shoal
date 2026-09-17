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
 * layer-a §2.2's file tree and §2.3's qids, in one table.
 *
 * Every file of §2.2 is a row, in §2.2's order, carrying its name, the
 * mode a stat and an open report, §2.1's role matrix and the handler
 * cells the rest of the surface fills in.  Walk, stat, open and clunk
 * work for every row now, whether or not its content is built, which
 * is what makes the role matrix testable ahead of the content: a row
 * with no handler at all answers the role gate first and the local
 * `not built' second.
 *
 * The role matrix.  §2.1 states its grants by role and is silent about
 * several cells; store.md §14(24) records what is chosen here and why.
 * The columns are walk, open-for-read and open-for-write, because 9P
 * separates them: a client must reach /obj/<oid> through /obj without
 * being able to list /obj, which §2.1 grants to role=admin alone.
 *
 * /ctl is open to every role here, and the gate that matters for it is
 * §2.5's per-verb one.  That is what §2.5 asks for — "a verb issued on
 * a fid whose role does not permit it MUST fail with permission
 * denied" has meaning only if a non-admin fid can hold /ctl open and
 * write to it — and §2.1's grant of /ctl to role=admin is that gate
 * said the other way round, since every row of §2.5 is admin.
 */
static char Enofile[] = "shoalsrv: no such file";
static char Edeleted[] = "object deleted";

static void rootread(Req*);

Sfile srvfiles[Nfile] =
{
/*	 name	   dir  perm	  walk		    rd		  wr		  render	  read	   write   open */
[Qroot]	{nil,	   1, DMDIR|0555, Aall,		    Aall,	  0,		  nil,		  rootread, nil, nil},
[Qctl]	{"ctl",	   0, 0666,	  Aall,		    Aall,	  Aall,		  srvemptytext,	  nil, srvctlwrite, nil},
[Qstatus]{"status",0, 0444,	  Aadmin,	    Aadmin,	  0,		  srvstatustext,  nil, nil, nil},
[Qmap]	{"map",	   0, 0444,	  Aadmin,	    Aadmin,	  0,		  srvmaptext,	  nil, nil, nil},
[Qobj]	{"obj",	   1, DMDIR|0555, Aall,		    Aadmin,	  Aclient|Aadmin, nil,		  nil, nil, nil},
[Qmeta]	{"meta",   1, DMDIR|0555, Aall,		    Aadmin,	  0,		  nil,		  nil, nil, nil},
[Qrepl]	{"repl",   0, 0600,	  Arepl,	    Arepl,	  Arepl,	  nil,		  nil, nil, nil},
[Qrpc]	{"rpc",	   0, 0600,	  Arepl|Aadmin,	    Arepl|Aadmin, Arepl|Aadmin,	  nil,		  nil, nil, nil},
[Qadvert]{"advert",0, 0400,	  Arepl,	    Arepl,	  0,		  nil,		  nil, nil, nil},
[Qdirty]{"dirty",  0, 0444,	  Aadmin,	    Aadmin,	  0,		  nil,		  nil, nil, nil},
[Qstale]{"stale",  0, 0444,	  Aadmin,	    Aadmin,	  0,		  nil,		  nil, nil, nil},
[Qtombs]{"tombs",  0, 0444,	  Aadmin,	    Aadmin,	  0,		  nil,		  nil, nil, nil},
[Qlost]	{"lost",   0, 0444,	  Aadmin,	    Aadmin,	  0,		  nil,		  nil, nil, nil},
[Qjobs]	{"jobs",   0, 0444,	  Aadmin,	    Aadmin,	  0,		  nil,		  nil, nil, nil},
[Qobjfile]{nil,	   0, 0666,	  Aall,		    Aall,	  Aclient|Aadmin, nil,		  nil, nil, nil},
[Qmetafile]{nil,   0, 0444,	  Aall,		    Aall,	  0,		  nil,		  nil, nil, nil},
};

static int
rolebit(int role)
{
	return 1<<role;
}

void
srvfileqid(int file, Qid *q)
{
	q->path = Pfixed + file;
	q->vers = 0;
	q->type = srvfiles[file].isdir ? QTDIR : QTFILE;
}

void
srvobjqid(Sfid *f, Objinfo *oi, Qid *q)
{
	q->path = oi->qidpath;
	if(f->file == Qmetafile)
		q->path |= Pmeta;
	q->vers = (ulong)(oi->ver & 0xFFFFFFFFULL);	/* §2.3: the low 32 */
	q->type = QTFILE;
}

/*
 * layer-a §1.1: 1*128 of ALPHA / DIGIT / "." / "-" / "_", and not "."
 * or "..".  An id that violates it answers `bad object name' on any
 * operation naming it — a walk included (§2.6).
 */
int
srvoidok(uchar *oid, int oidlen)
{
	int i, c;

	if(oidlen < 1 || oidlen > Oidmax)
		return 0;
	for(i = 0; i < oidlen; i++){
		c = oid[i];
		if(c >= 'a' && c <= 'z')
			continue;
		if(c >= 'A' && c <= 'Z')
			continue;
		if(c >= '0' && c <= '9')
			continue;
		if(c == '.' || c == '-' || c == '_')
			continue;
		return 0;
	}
	if(oidlen == 1 && oid[0] == '.')
		return 0;
	if(oidlen == 2 && oid[0] == '.' && oid[1] == '.')
		return 0;
	return 1;
}

/*
 * The Dir a stat answers.  The fixed files are synthetic and carry
 * length 0, which is the Plan 9 convention for a file whose bytes are
 * composed when it is opened; an object carries §2.3's length = len,
 * its mtime, and mode 0666.
 */
void
srvdir(Srvctx *c, Sfid *f, Dir *d)
{
	char buf[Oidmax+1];
	Sfile *e;

	USED(c);
	e = &srvfiles[f->file];
	memset(d, 0, sizeof *d);
	if(f->file == Qobjfile || f->file == Qmetafile){
		memmove(buf, f->oid, f->oidlen);
		buf[f->oidlen] = 0;
		d->name = estrdup9p(buf);
	}else if(e->name == nil)
		d->name = estrdup9p("/");
	else
		d->name = estrdup9p(e->name);
	d->uid = estrdup9p("shoal");
	d->gid = estrdup9p("shoal");
	d->muid = estrdup9p("shoal");
	d->mode = e->perm;
	d->qid.path = f->qidpath;
	d->qid.vers = f->qidvers;
	d->qid.type = e->isdir ? QTDIR : QTFILE;
	d->length = 0;
	d->atime = d->mtime = 0;
}

/*
 * The root directory's listing: the rows this fid's role may walk to.
 * The table is fixed, so nothing can tear between two Treads of it.
 */
static int
rootgen(int i, Dir *d, void *aux)
{
	Sfid *f, g;
	int n;

	f = aux;
	for(n = Qroot+1; n < Qobjfile; n++){
		if(srvfiles[n].name == nil)
			continue;
		if((srvfiles[n].walk & rolebit(f->role)) == 0)
			continue;
		if(i-- == 0)
			break;
	}
	if(n >= Qobjfile)
		return -1;
	memset(&g, 0, sizeof g);
	g.file = n;
	g.role = f->role;
	g.qidpath = Pfixed + n;
	srvdir(nil, &g, d);
	return 0;
}

static void
rootread(Req *r)
{
	dirread9p(r, rootgen, r->fid->aux);
	respond(r, nil);
}

/*
 * One walk element.  f is a working copy of the fid's state; on
 * success it is advanced onto the named file.
 */
static char*
walk1(Srvctx *c, Sfid *f, char *name, Qid *q, char *buf, int nbuf)
{
	Objinfo oi;
	Sfile *e;
	uchar oid[Oidmax];
	int n, i;

	if(strcmp(name, "..") == 0){
		switch(f->file){
		case Qobjfile:
			f->file = Qobj;
			break;
		case Qmetafile:
			f->file = Qmeta;
			break;
		default:
			f->file = Qroot;
			break;
		}
		f->oidlen = 0;
		f->qidpath = Pfixed + f->file;
		f->qidvers = 0;
		srvfileqid(f->file, q);
		return nil;
	}
	if(f->file == Qobj || f->file == Qmeta){
		n = strlen(name);
		if(!srvoidok((uchar*)name, n))
			return Ebadname;
		memmove(oid, name, n);
		if(objstat(c->store, oid, n, &oi) < 0)
			return srverr(buf, nbuf);
		if(oi.state != Slive)
			return Edeleted;
		f->file = f->file == Qobj ? Qobjfile : Qmetafile;
		memmove(f->oid, oid, n);
		f->oidlen = n;
		srvobjqid(f, &oi, q);
		f->qidpath = q->path;
		f->qidvers = q->vers;
		return nil;
	}
	if(f->file != Qroot)
		return Enofile;
	for(i = Qroot+1; i < Qobjfile; i++){
		e = &srvfiles[i];
		if(e->name != nil && strcmp(e->name, name) == 0)
			break;
	}
	if(i >= Qobjfile)
		return Enofile;
	if((srvfiles[i].walk & rolebit(f->role)) == 0)
		return Eperm;
	f->file = i;
	f->oidlen = 0;
	srvfileqid(i, q);
	f->qidpath = q->path;
	f->qidvers = q->vers;
	return nil;
}

/*
 * The walk itself.  A partial walk leaves newfid untouched, which is
 * what 9P asks and what lib9p's own walkandclone does not do; the fid
 * is advanced only when every element resolved, except that a walk of
 * a fid onto itself commits what it managed, because lib9p's rwalk
 * moves that Fid's qid whatever this answers.
 */
static void
dowalk(Req *r)
{
	char buf[ERRMAX], *e;
	Srvctx *c;
	Sfid *f, *nf, g;
	int i;

	c = r->srv->aux;
	f = r->fid->aux;
	g = *f;
	g.text = nil;
	g.aux = nil;
	g.auxfree = nil;
	e = nil;
	for(i = 0; i < r->ifcall.nwname; i++){
		e = walk1(c, &g, r->ifcall.wname[i], &r->ofcall.wqid[i],
			buf, sizeof buf);
		if(e != nil)
			break;
	}
	r->ofcall.nwqid = i;
	if(e != nil && i == 0){
		srvqdone(r, e);
		return;
	}
	if(i == r->ifcall.nwname || r->fid == r->newfid){
		if(r->fid == r->newfid)
			*f = g;
		else if(i == r->ifcall.nwname){
			if((nf = mallocz(sizeof *nf, 1)) == nil){
				srvqdone(r, "shoalsrv: out of memory");
				return;
			}
			*nf = g;
			r->newfid->aux = nf;
		}
	}
	srvqdone(r, nil);
}

static void
walkq(Req *r)
{
	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	dowalk(r);
}

/*
 * Does this walk reach an object?  Every operation on an oid goes
 * through that oid's queue, a walk included, so the service loop
 * settles that question from the table alone — without touching the
 * store — and pushes when the answer is yes.  A walk that names more
 * than one object runs wholly on the first one's queue; nothing in
 * layer-a issues such a walk, and no ordering is claimed for it beyond
 * that it, too, is ordered against that object.
 */
static int
walkoid(Sfid *f, Req *r, uchar *oid, int *oidlen)
{
	char *name;
	int at, i, n;

	at = f->file;
	for(i = 0; i < r->ifcall.nwname; i++){
		name = r->ifcall.wname[i];
		if(strcmp(name, "..") == 0){
			switch(at){
			case Qobjfile:
				at = Qobj;
				break;
			case Qmetafile:
				at = Qmeta;
				break;
			default:
				at = Qroot;
				break;
			}
			continue;
		}
		if(at == Qobj || at == Qmeta){
			/*
			 * An id §1.1 forbids names no object and so has no
			 * queue: it is answered `bad object name' on the
			 * service loop rather than cut to Oidmax, which would
			 * order the walk against a different object.
			 */
			n = strlen(name);
			if(!srvoidok((uchar*)name, n))
				return 0;
			memmove(oid, name, n);
			*oidlen = n;
			return 1;
		}
		if(at != Qroot)
			return 0;
		for(n = Qroot+1; n < Qobjfile; n++)
			if(srvfiles[n].name != nil && strcmp(srvfiles[n].name, name) == 0)
				break;
		if(n >= Qobjfile)
			return 0;
		at = n;
	}
	return 0;
}

void
srvwalk(Req *r)
{
	Srvctx *c;
	uchar oid[Oidmax];
	int oidlen;

	c = r->srv->aux;
	if(walkoid(r->fid->aux, r, oid, &oidlen)){
		srvqpush(c, oid, oidlen, r, walkq);
		return;
	}
	dowalk(r);
}

/*
 * Open.  §2.1's role gate first; then, for the two object rows, §6.4's
 * fence, because F1 fences every role=client read and write and every
 * role=repl or role=admin read of an object through /obj or /meta;
 * then the row's own hook, or its render-at-open snapshot, or the
 * local `not built'.  layer-a §2.4's mode rules — ORCLOSE and a write
 * on an OREAD fid — belong to the object rows' open hook, which is the
 * object-I/O surface's.
 */
void
srvopen(Req *r)
{
	char buf[ERRMAX], *e;
	Srvctx *c;
	Sfid *f;
	Sfile *file;
	int need, b;

	c = r->srv->aux;
	f = r->fid->aux;
	file = &srvfiles[f->file];
	b = rolebit(f->role);
	need = 0;
	switch(r->ifcall.mode & OMASK){
	case OREAD:
	case OEXEC:
		need = file->rd;
		break;
	case OWRITE:
		need = file->wr;
		break;
	case ORDWR:
		need = (file->rd & file->wr);
		break;
	}
	if(r->ifcall.mode & OTRUNC)
		need &= file->wr;
	if((need & b) == 0){
		respond(r, Eperm);
		return;
	}
	if(f->file == Qobjfile || f->file == Qmetafile){
		if(srvfencekind(c) != Fencenone){
			respond(r, Efenced);
			return;
		}
	}
	if(file->open != nil){
		file->open(r);
		return;
	}
	if(file->render != nil){
		if((f->text = textnew()) == nil){
			respond(r, "shoalsrv: out of memory");
			return;
		}
		if((e = file->render(c, f, f->text)) != nil){
			textfree(f->text);
			f->text = nil;
			respond(r, srverrs(buf, sizeof buf, e));
			return;
		}
		if(f->text->err){
			textfree(f->text);
			f->text = nil;
			respond(r, "shoalsrv: out of memory");
			return;
		}
		respond(r, nil);
		return;
	}
	if(file->read != nil || file->write != nil){
		respond(r, nil);
		return;
	}
	respond(r, Enotbuilt);
}

void
srvread(Req *r)
{
	Sfid *f;
	Sfile *file;

	f = r->fid->aux;
	file = &srvfiles[f->file];
	if(f->text != nil){
		textread(r, f->text);
		return;
	}
	if(file->read != nil){
		file->read(r);
		return;
	}
	respond(r, Enotbuilt);
}

void
srvwrite(Req *r)
{
	Sfid *f;
	Sfile *file;

	f = r->fid->aux;
	file = &srvfiles[f->file];
	if(file->write != nil){
		file->write(r);
		return;
	}
	respond(r, Enotbuilt);
}

static void
statq(Req *r)
{
	char buf[ERRMAX];
	Objinfo oi;
	Srvctx *c;
	Sfid *f;
	Qid q;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	f = r->fid->aux;
	if(objstat(c->store, f->oid, f->oidlen, &oi) < 0){
		srvqdone(r, srverr(buf, sizeof buf));
		return;
	}
	if(oi.state != Slive){
		srvqdone(r, Edeleted);
		return;
	}
	srvobjqid(f, &oi, &q);
	srvdir(c, f, &r->d);
	r->d.qid = q;
	r->d.length = oi.len;
	r->d.mtime = oi.mtime;
	r->d.atime = oi.mtime;
	srvqdone(r, nil);
}

void
srvstat(Req *r)
{
	Srvctx *c;
	Sfid *f;

	c = r->srv->aux;
	f = r->fid->aux;
	if(f->file == Qobjfile || f->file == Qmetafile){
		srvqpush(c, f->oid, f->oidlen, r, statq);
		return;
	}
	srvdir(c, f, &r->d);
	respond(r, nil);
}

void
srvcreate(Req *r)
{
	Sfid *f;

	f = r->fid->aux;
	if((srvfiles[f->file].wr & rolebit(f->role)) == 0){
		respond(r, Eperm);
		return;
	}
	respond(r, Enotbuilt);
}

void
srvremove(Req *r)
{
	Sfid *f;

	f = r->fid->aux;
	if((srvfiles[f->file].wr & rolebit(f->role)) == 0){
		respond(r, Eperm);
		return;
	}
	respond(r, Enotbuilt);
}

void
srvwstat(Req *r)
{
	Sfid *f;

	f = r->fid->aux;
	if((srvfiles[f->file].wr & rolebit(f->role)) == 0){
		respond(r, Eperm);
		return;
	}
	respond(r, Enotbuilt);
}

/*
 * A fid's state goes when lib9p frees the fid, which is after
 * Srv.end — so after the shutdown sequence has closed the store.  That
 * ordering is D16's: an Objsnap a fid holds in aux is the one thing
 * that may outlive a storeclose, and auxfree is where the rest of the
 * surface gives it back.
 */
void
srvdestroyfid(Fid *fid)
{
	Sfid *f;

	if((f = fid->aux) == nil)
		return;
	fid->aux = nil;
	if(f->auxfree != nil)
		f->auxfree(f->aux);
	textfree(f->text);
	free(f);
}

void
srvdestroyreq(Req *r)
{
	Qreq *qr;

	if((qr = r->aux) == nil)
		return;
	r->aux = nil;
	srvqended(qr);
	free(qr->cb);
	free(qr);
}
