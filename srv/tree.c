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

static void rootread(Req*);
static char* objgate(Srvctx*, Sfid*, Req*, int);
static char* chgate(Srvctx*, Sfid*, Req*, int);
static void mapopenq(Req*);

/*
 * One row per file, one field per line: a cell is filled by naming it,
 * so a row grows without its neighbours moving and two cells of one row
 * are two separate lines.  A field left out is zero, which for a
 * handler cell is `not built' and for a role column is `no role'.
 */
Sfile srvfiles[Nfile] =
{
[Qroot] = {
	.name	= nil,
	.isdir	= 1,
	.perm	= DMDIR|0555,
	.walk	= Aall,
	.rd	= Aall,
	.wr	= 0,
	.read	= rootread,
},
[Qctl] = {
	.name	= "ctl",
	.perm	= 0666,
	.walk	= Aall,
	.rd	= Aall,
	.wr	= Aall,
	.render	= srvemptytext,
	.write	= srvctlwrite,
},
[Qstatus] = {
	.name	= "status",
	.perm	= 0444,
	.walk	= Aadmin,
	.rd	= Aadmin,
	.wr	= 0,
	.render	= srvstatustext,
},
[Qmap] = {
	.name	= "map",
	.perm	= 0444,
	.walk	= Aadmin,
	.rd	= Aadmin,
	.wr	= 0,
	.render	= srvmaptext,
},
[Qobj] = {
	.name	= "obj",
	.isdir	= 1,
	.perm	= DMDIR|0555,
	.walk	= Aall,
	.rd	= Aadmin,
	.wr	= Aclient|Aadmin,
	.gate	= objgate,
},
[Qmeta] = {
	.name	= "meta",
	.isdir	= 1,
	.perm	= DMDIR|0555,
	.walk	= Aall,
	.rd	= Aadmin,
	.wr	= 0,
	.gate	= objgate,
},
[Qrepl] = {
	.name	= "repl",
	.perm	= 0600,
	.walk	= Arepl,
	.rd	= Arepl,
	.wr	= Arepl,
	.gate	= chgate,
},
[Qrpc] = {
	.name	= "rpc",
	.perm	= 0600,
	.walk	= Arepl|Aadmin,
	.rd	= Arepl|Aadmin,
	.wr	= Arepl|Aadmin,
	.gate	= chgate,
},
[Qadvert] = {
	.name	= "advert",
	.perm	= 0400,
	.walk	= Arepl,
	.rd	= Arepl,
	.wr	= 0,
},
[Qdirty] = {
	.name	= "dirty",
	.perm	= 0444,
	.walk	= Aadmin,
	.rd	= Aadmin,
	.wr	= 0,
},
[Qstale] = {
	.name	= "stale",
	.perm	= 0444,
	.walk	= Aadmin,
	.rd	= Aadmin,
	.wr	= 0,
},
[Qtombs] = {
	.name	= "tombs",
	.perm	= 0444,
	.walk	= Aadmin,
	.rd	= Aadmin,
	.wr	= 0,
},
[Qlost] = {
	.name	= "lost",
	.perm	= 0444,
	.walk	= Aadmin,
	.rd	= Aadmin,
	.wr	= 0,
},
[Qjobs] = {
	.name	= "jobs",
	.perm	= 0444,
	.walk	= Aadmin,
	.rd	= Aadmin,
	.wr	= 0,
},
[Qobjfile] = {
	.name	= nil,
	.perm	= 0666,
	.walk	= Aall,
	.rd	= Aall,
	.wr	= Aclient|Aadmin,
	.gate	= objgate,
},
[Qmetafile] = {
	.name	= nil,
	.perm	= 0444,
	.walk	= Aall,
	.rd	= Aall,
	.wr	= 0,
	.gate	= objgate,
},
};

static int
rolebit(int role)
{
	return 1<<role;
}

/* layer-a §1.1's reserved ids: the system's own, spelled `shoal.…' */
static int
reservedid(uchar *oid, int oidlen)
{
	return oidlen >= 6 && memcmp(oid, "shoal.", 6) == 0;
}

/*
 * The object rows' gate, run right after the role gate on every open,
 * create, remove and wstat that reaches an object or the directory one
 * is created in — and on every read and write of one, where there is
 * no role gate to run after, the open having settled the role.  A read
 * and a write are gated because §6.4 F1 fences OPERATIONS and the
 * fence can go on under an open fid: a fid opened before `fence on'
 * would otherwise carry its grant past it.  It asks three things, in
 * this order:
 *
 *	§2.1's operator rule.  role=admin's grant of /obj and /meta is
 *	read-only, and a create, write, remove or wstat of an id that is
 *	not a reserved `shoal.' one MUST fail `permission denied'.  It
 *	goes first because it reads the fid and the name alone, and
 *	neither can change while the fid lives: no later state can make
 *	an operation §2.1 forbids permissible.
 *
 *	§6.4 F3's `down'.  An instance whose own map record says up=no
 *	or status=out MUST refuse role=client I/O with `down'.  up=heal
 *	is deliberately not in F3's list.  This is the instance's own
 *	standing state — the map it serves is the one it was started
 *	with (store.md §14(18)) — so it answers before the one gate an
 *	operator can change while a fid is open.
 *
 *	§6.4 F1 and F4's fence, with §2.1's sole exemption: a role=admin
 *	READ of a reserved `shoal.' id passes while fenced, which is
 *	what makes §8.6's monitor rebuild executable.  §2.1 grants admin
 *	writes of reserved ids only while unfenced, so the exemption is
 *	the read alone; every other operation on an object is `fenced'.
 *	A read of the /obj or /meta DIRECTORY is not an operation on an
 *	object: F1 fences "read of an object through /obj or /meta", and
 *	a listing is neither — /tombs, which is the same operator
 *	inspection path (§2.2), is not fenced at all.  store.md §14(24)
 *	records the ruling.
 *
 * store.md §14(24) carries the same order for a reader outside srv/.
 */
static char*
objgate(Srvctx *c, Sfid *f, Req *r, int op)
{
	uchar *oid;
	int oidlen, wr, rsvd, m;

	oid = f->oid;
	oidlen = f->oidlen;
	switch(op){
	case Gcreate:
		oid = (uchar*)r->ifcall.name;
		oidlen = strlen(r->ifcall.name);
		wr = 1;
		break;
	case Gread:
		wr = 0;
		break;
	case Gwrite:
		wr = 1;
		break;
	case Gopen:
		/*
		 * The mode says which column an open is in, and ORCLOSE is
		 * in the write one: §2.4 makes it a remove at the clunk, and
		 * §2.1 refuses role=admin the remove of an id that is not
		 * reserved, so an OREAD|ORCLOSE open admitted as a read would
		 * be that remove arranged one message ahead.  (§2.4 refuses
		 * ORCLOSE on an object outright, with `bad open mode'; that
		 * belongs to the rows' open cell, which is not built, and
		 * this rule does not wait for it.)
		 */
		m = r->ifcall.mode;
		wr = (m&OMASK) == OWRITE || (m&OMASK) == ORDWR
			|| (m&OTRUNC) != 0 || (m&ORCLOSE) != 0;
		break;
	default:			/* Gremove, Gwstat */
		wr = 1;
		break;
	}
	rsvd = reservedid(oid, oidlen);
	if(wr && f->role == Radmin && !rsvd)
		return Eperm;
	if(f->role == Rclient
	&& (c->self->up == Uno || c->self->status == Sout))
		return Edown;
	if(srvfencekind(c) != Fencenone){
		if(!wr && (f->file == Qobj || f->file == Qmeta))
			return nil;
		if(!wr && rsvd && f->role == Radmin)
			return nil;
		return Efenced;
	}
	return nil;
}

/*
 * The channel rows' gate.  §6.4 F1 fences "every /repl and /rpc
 * operation", which is carried here as every open, read and write of
 * these two rows — and the remove and wstat that reach the same gate —
 * and not the open alone: the work a channel carries is the
 * replication work F1 exists to stop, and the fence can go on under a
 * fid a peer already holds open.  A walk, a stat and a clunk run no
 * gate, so F1 reaches as far as the gate does and no further.
 *
 * Neither of §2.1's other two rules is theirs.  The operator rule is
 * about an object's name and these rows name none; F3's `down' is
 * about role=client I/O, and neither row admits role=client — a
 * channel between instances is not a client serving path, which is
 * what F3 is about (§6.4 "What F3 does not bar").
 */
static char*
chgate(Srvctx *c, Sfid *f, Req *r, int op)
{
	USED(f);
	USED(r);
	USED(op);
	if(srvfencekind(c) != Fencenone)
		return Efenced;
	return nil;
}

/*
 * The T1 fid-state point.  With it on, every fid this server makes
 * carries a state object of the server's own whose three hooks count
 * themselves, which is how the hooks are driven before a row fills
 * aux with anything.  The flush hook also counts the times it ran
 * after its request had already responded, which is never: step 7
 * runs before the reply (layer-a §5.4.1) — and the times it ran while
 * a handler was mid-step on the same fid's state, which is never
 * either: the state lock the hook is called under is the one such a
 * handler holds across its step (dat.h).
 */
static void
auxpointflush(Sfid *f, Req *r)
{
	Srvctx *c;

	c = f->ctx;
	if(c == nil)
		return;
	lock(&c->auxlk);
	c->nauxflush++;
	if(r->responded)
		c->nauxlate++;
	if(f->auxbusy)
		c->nauxbusy++;
	unlock(&c->auxlk);
}

static void
auxpointclose(void *a)
{
	char err[ERRMAX];
	Objinfo oi;
	Srvctx *c;
	int open;

	c = a;
	/*
	 * This hook may call the engine, and whether it still can is the
	 * fact worth recording: a sweep run after the store had closed
	 * would be no use at all to a fid holding a stage.  The id is one
	 * nothing creates, so the answer is `no such object' while the
	 * store is open and `store closed' after it is not.
	 */
	open = 0;
	if(c->store != nil){
		if(objstat(c->store, (uchar*)"shoal.auxprobe", 14, &oi) == 0)
			open = 1;
		else{
			rerrstr(err, sizeof err);
			open = strstr(err, "store closed") == nil;
		}
	}
	lock(&c->auxlk);
	c->nauxclose++;
	if(open)
		c->nauxopen++;
	unlock(&c->auxlk);
}

static void
auxpointfree(void *a)
{
	Srvctx *c;

	c = a;
	lock(&c->auxlk);
	c->nauxfree++;
	unlock(&c->auxlk);
}

static void
auxpoint(Srvctx *c, Sfid *f)
{
	if(!c->fidaux)
		return;
	f->aux = c;
	f->auxflush = auxpointflush;
	f->auxclose = auxpointclose;
	f->auxfree = auxpointfree;
	f->auxclosed = 0;
	f->auxbusy = 0;
}

/*
 * The point's handler side: a queued handler is between two steps of
 * its own work on the fid's state, or inside one.  A row's handler
 * marks that by holding the fid's state lock across the step it takes
 * on what aux names; this marks it for the state the point itself
 * filled in, so that the flush hook can say whether step 7 ever ran
 * inside such a step.  It is the check point's hold that stands in for
 * the step (queue.c), because that is the moment a test can hold a
 * handler at.
 *
 * Inert unless the point filled this fid's cells: a row that fills
 * aux with something of its own takes the lock in its own handler.
 */
void
srvauxstep(Req *r, int on)
{
	Sfid *f;

	if(r->fid == nil || (f = r->fid->aux) == nil)
		return;
	if(f->auxflush != auxpointflush)
		return;
	if(on)
		qlock(&f->lk);
	f->auxbusy = on;
	if(!on)
		qunlock(&f->lk);
}

/*
 * The live-fid registry.  lib9p's fid pool is not iterable from a
 * server (9p(2) exposes no walk over it), so the fids this server
 * makes are kept on a list of their own, from the attach or walk that
 * made one until destroyfid.  srvfidsclose is what a caller that must
 * reach every pending auxclose — before the store closes — uses.
 *
 * A hook runs under the FID'S state lock and never under this one: it
 * may call the engine, and a registry lock held across device I/O
 * would block every attach, clunk and clone-walk behind it (dat.h,
 * store.md §7 rule 2).  auxclose runs once per state a fid holds, and
 * not at all once the store has closed: store.md §9 allows nothing but
 * the Objsnap calls after that, which is auxfree's half.
 *
 * A clunk and a walk that moves a fid run their own fid's hook, so the
 * only caller srvfidsclose has is the shutdown, which must reach the
 * fids still open when the service loop ends: srvshutdown sweeps here
 * after the drain and the job wait, because a hook may call the
 * engine, and before the store closes, because that is what the sweep
 * is for.  No row fills a fid with state that must be given back
 * before the store closes yet; the sweep is what will reach it.
 */
void
srvfidnew(Srvctx *c, Sfid *f)
{
	f->ctx = c;
	qlock(&c->fidlk);
	auxpoint(c, f);
	f->prev = nil;
	f->next = c->fids;
	if(c->fids != nil)
		c->fids->prev = f;
	c->fids = f;
	qunlock(&c->fidlk);
}

static void
auxclose1(Srvctx *c, Sfid *f)		/* f->lk held */
{
	if(f->auxclosed)
		return;
	f->auxclosed = 1;
	if(f->auxclose != nil && !c->closed && c->store != nil)
		f->auxclose(f->aux);
}

/*
 * The shutdown's sweep.  It is the one place that holds the registry
 * lock while it takes a fid's state lock — the order the two are
 * always taken in (dat.h) — and the one place a hook runs with fidlk
 * held: by here the service loop has ended and the drain has finished,
 * so there is no attach, clunk or walk left for it to hold up.
 */
void
srvfidsclose(Srvctx *c)
{
	Sfid *f;

	qlock(&c->fidlk);
	for(f = c->fids; f != nil; f = f->next){
		qlock(&f->lk);
		auxclose1(c, f);
		qunlock(&f->lk);
	}
	qunlock(&c->fidlk);
}

/*
 * Give back what a fid holds.  The order is the two hooks' own:
 * auxclose while the store is still there, then auxfree, which may run
 * after it has gone.  gone says the fid itself is ending, so it also
 * leaves the registry.
 *
 * Two locks, over two different things.  The registry lock covers the
 * list and the rendered Text, which srvopentext writes from a queue
 * proc; the FID'S state lock covers the state and its cells, and is
 * held across the close hook and the clearing of the cells, so that a
 * give-back cannot free a state out from under a call that is already
 * in it.  There are two such calls: srvstep7, which discards this
 * fid's stage on a flush from a queue proc or from the service loop,
 * and any handler of another request outstanding on this same fid.
 * This waits for them; it does not run beside them (dat.h).
 *
 * What the state lock is NOT held across is the free itself, which is
 * safe because the cells are cleared under it: anything that would
 * call into the state has to take the lock and finds nothing there.
 */
static void
fidgive(Sfid *f, int gone)
{
	Srvctx *c;
	void (*fr)(void*);
	void *a;
	Text *t;

	c = f->ctx;
	if(c != nil){
		qlock(&c->fidlk);
		if(gone){
			if(f->prev != nil)
				f->prev->next = f->next;
			else
				c->fids = f->next;
			if(f->next != nil)
				f->next->prev = f->prev;
			f->prev = f->next = nil;
			f->ctx = nil;
		}
		t = f->text;
		f->text = nil;
		qunlock(&c->fidlk);
	}else{
		t = f->text;
		f->text = nil;
	}
	qlock(&f->lk);
	if(c != nil)
		auxclose1(c, f);
	fr = f->auxfree;
	a = f->aux;
	f->aux = nil;
	f->auxflush = nil;
	f->auxclose = nil;
	f->auxfree = nil;
	f->auxclosed = 0;
	f->auxbusy = 0;
	qunlock(&f->lk);
	if(fr != nil)
		fr(a);
	textfree(t);
}

/*
 * The same, for a fid that lives on: what a handler calls when the
 * fid it is running on is about to hold something else, which is the
 * /obj create cell turning a directory fid into the created object's
 * (dat.h).  It is exported because the alternative another file has
 * is to set auxclosed itself and call the hooks unlocked, which races
 * the shutdown's sweep into a second auxclose on the same state.
 */
void
srvfidgive(Sfid *f)
{
	fidgive(f, 0);
}

/*
 * Under the registry lock, which is the lock the fids' own points use:
 * every fid this server makes reads it as it is linked in, and the
 * queue procs are making fids while the program that sets this is
 * running.
 */
void
srvauxpoint(Srvctx *c, int on)
{
	qlock(&c->fidlk);
	c->fidaux = on;
	qunlock(&c->fidlk);
}

void
srvauxcount(Srvctx *c, uvlong *flushed, uvlong *closed, uvlong *freed)
{
	lock(&c->auxlk);
	if(flushed != nil)
		*flushed = c->nauxflush + c->nauxlate;
	if(closed != nil)
		*closed = c->nauxclose;
	if(freed != nil)
		*freed = c->nauxfree;
	unlock(&c->auxlk);
}

/*
 * How many flush hooks ran while a handler was mid-step on the same
 * fid's state; the answer is always zero, because the hook and such a
 * handler take the same lock (dat.h).
 */
uvlong
srvauxbusy(Srvctx *c)
{
	uvlong n;

	lock(&c->auxlk);
	n = c->nauxbusy;
	unlock(&c->auxlk);
	return n;
}

/* how many close hooks found the engine still there; all of them */
uvlong
srvauxopen(Srvctx *c)
{
	uvlong n;

	lock(&c->auxlk);
	n = c->nauxopen;
	unlock(&c->auxlk);
	return n;
}

/*
 * How many fids the registry holds.  It is the list srvfidsclose
 * follows, counted where a test can compare it against the fids that
 * are really live: an entry lost off the list is a fid whose state is
 * never given back and whose own destroy then writes through a
 * neighbour that has been freed.
 */
int
srvfidcount(Srvctx *c)
{
	Sfid *f;
	int n;

	n = 0;
	qlock(&c->fidlk);
	for(f = c->fids; f != nil; f = f->next)
		n++;
	qunlock(&c->fidlk);
	return n;
}

/* how many of those flush hooks ran late; the answer is always zero */
uvlong
srvauxlate(Srvctx *c)
{
	uvlong n;

	lock(&c->auxlk);
	n = c->nauxlate;
	unlock(&c->auxlk);
	return n;
}

/*
 * The T1 cell point.  A row's handler cells are what the rest of the
 * surface fills in (dat.h), and the rules this library holds around
 * them — the gate that runs before a read and a write, the precedence
 * a read cell has over a rendered Text, the give-back a create owes
 * the directory fid it is issued on — are testable before any of
 * those bodies exist.  With the point on, rows carry a cell of the
 * server's own:
 *
 *	[Qctl].read	answers fixed bytes.  /ctl also renders at open,
 *			so a read of it says which of the two serves it.
 *	[Qobjfile].write
 *			answers an Rwrite counting the bytes, so a write
 *			the fence refuses is distinguishable from one no
 *			cell would have taken anyway.
 *	[Qobj].create	retargets the directory fid onto the named object
 *			and answers Rcreate, giving the fid's state back
 *			with srvfidgive first, which is what a create cell
 *			owes the fid it moves (dat.h).  Nothing is created
 *			in the store — the create body is §2.4's — so the
 *			qid it answers is the object row's own and says
 *			nothing about an object.  A name §1.1 forbids is
 *			refused with `bad object name', and that refusal is
 *			what a failed create looks like here: the fid does
 *			not move, so it gives nothing back.
 *
 * The cells are the table's and the table is the program's, so this
 * point is global rather than per-context: a T1 program sets it,
 * drives what it wants and clears it — and the shutdown clears it too,
 * so cells a context was given cannot be inherited by the next server
 * the program starts.  The write is under the points' lock, which is
 * where the shutdown's own clearing runs; the table is read unlocked
 * on the service loop and on the queue procs, so the point is moved
 * when nothing is driving the rows it fills.
 */
static char Celltext[] = "cell\n";

static void
cellread(Req *r)
{
	readstr(r, Celltext);
	respond(r, nil);
}

static void
cellwrite(Req *r)
{
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

static void
cellcreate(Req *r)
{
	Sfid *f;
	int n;

	f = r->fid->aux;
	n = strlen(r->ifcall.name);
	if(!srvoidok((uchar*)r->ifcall.name, n)){
		respond(r, Ebadname);
		return;
	}
	srvfidgive(f);
	f->file = Qobjfile;
	memmove(f->oid, r->ifcall.name, n);
	f->oidlen = n;
	srvfileqid(Qobjfile, &r->ofcall.qid);
	f->qidpath = r->ofcall.qid.path;
	f->qidvers = r->ofcall.qid.vers;
	respond(r, nil);
}

void
srvcellpoint(Srvctx *c, int on)
{
	qlock(&c->holdlk);
	srvfiles[Qctl].read = on ? cellread : nil;
	srvfiles[Qobjfile].write = on ? cellwrite : nil;
	srvfiles[Qobj].create = on ? cellcreate : nil;
	qunlock(&c->holdlk);
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
 * The walk itself.  A walk that does not resolve every element leaves
 * both fids exactly where they were — which is what 9P asks and what
 * lib9p's own walkandclone does not do — and that holds for a walk of
 * a fid onto itself too: lib9p's rwalk leaves the Fid's qid untouched
 * whenever it answers fewer qids than were named, so a server that
 * advanced its own state there would leave the two disagreeing about
 * where the client's fid points.  The Sfid is therefore committed only
 * when every element resolved.
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
	g.auxflush = nil;
	g.auxclose = nil;
	g.auxfree = nil;
	g.auxclosed = 0;
	g.ctx = nil;
	g.prev = g.next = nil;
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
	if(i == r->ifcall.nwname){
		/*
		 * A walk of a fid onto itself that names nothing is 9P's
		 * probe of that fid: it resolves, and the fid stays where it
		 * is.  Only a fid that MOVES gives its state back (dat.h), so
		 * the probe must leave what the fid holds alone.
		 */
		if(r->fid == r->newfid && r->ifcall.nwname > 0){
			/*
			 * The fid moves, and what it held does not move with
			 * it: a fid names one file, and the state it carries
			 * is that file's.  It is given back here — the close
			 * hook first, while the store is certainly open —
			 * rather than dropped, which would leak it and lose
			 * the hook (dat.h's Sfid).  Its place on the registry
			 * is the fid's own and stays.
			 *
			 * The commit names the fields a walk changes and no
			 * others, because the registry links beside them are
			 * not this proc's to write: a walk that names an
			 * object runs on that object's queue, while an attach
			 * or a clone on the service loop links a new fid in
			 * at the head under fidlk.  Links read before that
			 * and written back after it would put the new fid's
			 * neighbour where the new fid is — and the list is
			 * how srvfidsclose reaches every live fid.  A whole
			 * structure copy is what carried them; walk1 changes
			 * the file, the oid and the qid alone, and the role,
			 * the epoch and the peer are the attach's for the
			 * fid's whole life.
			 */
			fidgive(f, 0);
			srvqwalkhold(r);
			f->file = g.file;
			memmove(f->oid, g.oid, sizeof f->oid);
			f->oidlen = g.oidlen;
			f->qidpath = g.qidpath;
			f->qidvers = g.qidvers;
		}else if(r->fid != r->newfid){
			if((nf = mallocz(sizeof *nf, 1)) == nil){
				srvqdone(r, "shoalsrv: out of memory");
				return;
			}
			*nf = g;
			srvfidnew(c, nf);
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
 * Open.  §2.1's role gate first, then the row's own gate, then the
 * row's open hook, or its render-at-open snapshot, or the local `not
 * built'.  layer-a §2.4's mode rules — ORCLOSE and a write on an OREAD
 * fid — belong to the object rows' open hook, which is the object-I/O
 * surface's.
 */
void
srvopen(Req *r)
{
	char *e;
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
	if(file->gate != nil && (e = file->gate(c, f, r, Gopen)) != nil){
		respond(r, e);
		return;
	}
	if(file->open != nil){
		file->open(r);
		return;
	}
	if(file->render != nil){
		switch(f->file == Qmap ? srvpoint(c, "mapopen") : 0){
		case 1:
			srvqpushany(c, r, mapopenq);
			return;
		case 2:
			if(srvqprepany(c, r, mapopenq) == nil)
				return;
			break;
		}
		srvopentext(r);
		return;
	}
	if(file->read != nil || file->write != nil){
		srvqdone(r, nil);
		return;
	}
	srvqdone(r, Enotbuilt);
}

/*
 * The render-at-open body, on its own so that a row whose open was
 * offloaded runs the same one: it is reached from the service loop and
 * from a queue proc, and it answers through the pool's one exit either
 * way (srvqdone answers directly for a request that was never pushed).
 */
void
srvopentext(Req *r)
{
	char *e;
	Srvctx *c;
	Sfid *f;
	Sfile *file;
	Text *t;

	c = r->srv->aux;
	f = r->fid->aux;
	file = &srvfiles[f->file];
	if((t = textnew()) == nil){
		srvqdone(r, "shoalsrv: out of memory");
		return;
	}
	if((e = file->render(c, f, t)) != nil){
		textfree(t);
		srvqdone(r, e);
		return;
	}
	if(t->err){
		textfree(t);
		srvqdone(r, "shoalsrv: out of memory");
		return;
	}
	/*
	 * Composed into a text of its own and hung on the fid under the
	 * registry lock, which is the lock the rendered Text is under
	 * (dat.h): this runs on a queue proc when the row's open was
	 * offloaded, and fidgive takes the fid's Text under that same lock
	 * on the service loop.
	 */
	qlock(&c->fidlk);
	textfree(f->text);
	f->text = t;
	qunlock(&c->fidlk);
	srvqdone(r, nil);
}

/*
 * The T1 offload point.  A row MAY answer an open, a read or a render
 * on a queue rather than on the service loop (dat.h), and this is that
 * path driven from a test: with the point at 1 a Topen of /map is
 * pushed to the reserved queue and held there, so a case can hold one
 * request that names no object while it asks the loop for another and
 * while the object queues go on running; with it at 2 the open is
 * prepared for a queue and then answered on the loop after all, which
 * is the caller the pool's counting has to survive.
 *
 * /map is the row that takes it because its render reads nothing an
 * offload would change; the rows that will really need one — the /obj
 * directory's read and the status files whose renders take an engine
 * snapshot — are not built.
 */
static void
mapopenq(Req *r)
{
	Srvctx *c;

	c = r->srv->aux;
	while(srvpoint(c, "mapopen") == 1 && !srvqcheck(r))
		sleep(5);
	if(srvqcheck(r)){
		/*
		 * Held here, still inside the handler, so that the reserved
		 * queue's flush flag stays raised while the service loop
		 * prepares another request for that queue (queue.c).
		 */
		srvqanyexit(c);
		srvqdone(r, nil);
		return;
	}
	srvopentext(r);
}

/*
 * Read and write.  The row's gate runs first on both, for the reason
 * objgate gives: §6.4 F1 fences operations rather than opens, and the
 * operator fence can go on while a fid is open, so the fid's grant is
 * not the answer to the operation.  There is no role gate to run
 * before it — 9P settles the role at the open, which srvopen gated —
 * and none of these rows has one of its own.  It is done here rather
 * than in each row's cell so that a row cannot be built without it.
 *
 * Then the cells.  A row with a read cell gets every read on it,
 * whether or not the fid also holds a rendered Text — a row that
 * renders bytes at open AND wants the read itself serves them with
 * textread — and only a row with render and no read takes the
 * automatic path (dat.h).
 */
void
srvread(Req *r)
{
	char *e;
	Srvctx *c;
	Sfid *f;
	Sfile *file;

	c = r->srv->aux;
	f = r->fid->aux;
	file = &srvfiles[f->file];
	if(file->gate != nil && (e = file->gate(c, f, r, Gread)) != nil){
		respond(r, e);
		return;
	}
	if(file->read != nil){
		file->read(r);
		return;
	}
	if(f->text != nil){
		textread(r, f->text);
		return;
	}
	srvqdone(r, Enotbuilt);
}

void
srvwrite(Req *r)
{
	char *e;
	Srvctx *c;
	Sfid *f;
	Sfile *file;

	c = r->srv->aux;
	f = r->fid->aux;
	file = &srvfiles[f->file];
	if(file->gate != nil && (e = file->gate(c, f, r, Gwrite)) != nil){
		respond(r, e);
		return;
	}
	if(file->write != nil){
		file->write(r);
		return;
	}
	srvqdone(r, Enotbuilt);
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

/*
 * Create, remove and wstat, in the write column: §2.1's role gate,
 * then the row's gate, then the row's own cell, which for a row whose
 * content is not built is nil and answers the local `not built'.  The
 * three are one shape, so a row is built by filling one cell.
 */
static void
wrop(Req *r, int op, void (*cell)(Req*))
{
	char *e;
	Srvctx *c;
	Sfid *f;
	Sfile *file;

	c = r->srv->aux;
	f = r->fid->aux;
	file = &srvfiles[f->file];
	if((file->wr & rolebit(f->role)) == 0){
		respond(r, Eperm);
		return;
	}
	if(file->gate != nil && (e = file->gate(c, f, r, op)) != nil){
		respond(r, e);
		return;
	}
	if(cell != nil){
		cell(r);
		return;
	}
	respond(r, Enotbuilt);
}

void
srvcreate(Req *r)
{
	Sfid *f;

	f = r->fid->aux;
	wrop(r, Gcreate, srvfiles[f->file].create);
}

void
srvremove(Req *r)
{
	Sfid *f;

	f = r->fid->aux;
	wrop(r, Gremove, srvfiles[f->file].remove);
}

void
srvwstat(Req *r)
{
	Sfid *f;

	f = r->fid->aux;
	wrop(r, Gwstat, srvfiles[f->file].wstat);
}

/*
 * A fid's state goes when lib9p frees the fid.  At a clunk that is at
 * once, with the store open, and both hooks run here; after the
 * service loop has ended it is lib9p freeing the fid pool, which is
 * after Srv.end and so after the shutdown has closed the store — D16's
 * ordering, and the reason auxclose does not run then and auxfree
 * still does: an Objsnap a fid holds in aux is the one thing that may
 * outlive a storeclose (store.md §9).
 */
void
srvdestroyfid(Fid *fid)
{
	Sfid *f;

	if((f = fid->aux) == nil)
		return;
	fid->aux = nil;
	fidgive(f, 1);
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
