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
 * The enumerations, layer-a §2.2: the /obj and /meta directory
 * listings, and the two files that are that same listing rendered as
 * text — /tombs over the tombstones and /advert over both states, in
 * §7.2's advert grammar.  All four read one index snapshot
 * (store.md §9's objsnapopen), which is what §2.2's "concurrent
 * creates and deletes SHOULD NOT cause an entry to be skipped or
 * duplicated part-way through a sequential read" asks for.
 *
 * Everything here runs on the reserved queue.  A render or a read
 * that takes an engine snapshot MUST leave the service loop (dat.h),
 * and every cell in this file does: objsnapopen takes two holds of
 * the engine's state lock and objsnapent one per entry, and §9
 * measures the open's walk at ~23 ms on a full index, which is the
 * one hold in the engine comparable to a write.  A row opts in by
 * filling the cell that gets the request first, so the two directory
 * rows fill open AND read, and the four text rows fill open (which
 * offloads) and render (which srvopentext then runs on the queue).
 */

/*
 * A directory fid's state: the snapshot its reads walk, and the
 * cursor over it.
 *
 * The cursor is this file's because lib9p's dirread9p cannot be
 * used.  dirread9p re-generates entry n for each Tread from the
 * generator's index, so it assumes the n'th entry is the same entry
 * it was at the previous read; §9's snapshot holds a vector of
 * {slot, qid.path} whose entries GO AWAY under it — an object
 * discarded, or deleted and so no longer `live' — and objsnapent
 * answers 0 for those.  An entry that has gone is not listed, so the
 * position of every later entry in the BYTE stream moves, and a
 * generator addressed by entry number would skip or repeat around
 * it.  So the fid carries the pair that a Plan 9 directory read
 * really needs: the byte offset it has served up to, and the
 * snapshot position that offset stands at.  A read at offset 0
 * restarts from position 0 — which is how a re-read works (§9) — a
 * read at the recorded offset continues, and any other offset is a
 * seek in a directory and is refused.  The offset the read BEFORE
 * this one started at is kept beside it and rewinds to it, which is
 * what a client that never received a reply needs: a Tread this
 * server answered `interrupted' had already advanced the cursor when
 * step 7 took its exit, and 9P has the client discard that reply and
 * ask again from where it was.
 *
 * The Objsnap is given back by auxfree and NOT by auxclose: a
 * snapshot is the one thing store.md §9 lets outlive storeclose, and
 * D16 is what makes a fid still open when the connection dropped
 * able to give it back after the store has gone.  So this row's
 * auxclose stays nil, and the shutdown's sweep has nothing to do for
 * it.
 *
 * auxflush is filled, and is the open's own undoing: the Topen that
 * takes the snapshot installs it here and then leaves through
 * srvqdone, which MAY answer `interrupted' (layer-a §5.4.1 step 7).
 * lib9p does not run its `ropen' on an error, so such a fid would
 * hold a snapshot — one of store.md §9's objsnapmax slots — with
 * omode still -1, and a client that flushes opens could drive other
 * clients to `disk full'.  The hook gives it back at exactly that
 * point, and only there: it discards nothing for a flushed READ,
 * whose cursor and snapshot the next read needs (§14(33)), and
 * nothing for a fid whose open completed, which lib9p has given an
 * omode.
 */
typedef struct Objdir Objdir;
struct Objdir
{
	Objsnap	*sn;
	uvlong	off;		/* the byte offset the cursor stands at */
	ulong	pos;		/* the snapshot position that offset names */
	uvlong	prevoff;	/* where the read before this one started */
	ulong	prevpos;
};

static char Eseek[] = "shoalsrv: seek in a directory read";

/*
 * store.md §9's one pathological refusal from objsnapopen, which a
 * server SHOULD retry once before answering the client at all (D20).
 * Matched by prefix, because the text names Snaptries and that is the
 * engine's number rather than this file's.
 */
static char Emoved[] = "object snapshot: the index moved";

/*
 * The error string an offloaded render answers with.  A render cell
 * answers a char*, and the engine's own text has to outlive the call,
 * so it is composed here rather than in the caller's frame.  One
 * buffer is enough because every render that can fill it runs on the
 * reserved queue and lib9p gives a Reqqueue exactly one proc, so
 * these renders have one writer between them; /status and /map, which
 * render on the service loop, answer literals and never come here.
 */
static char rendererr[ERRMAX];

static void	objdirfree(void*);
static void	objdirflush(Sfid*, Req*);

static void
hexof(char *out, uchar *p, int n)
{
	static char hex[] = "0123456789abcdef";
	int i;

	for(i = 0; i < n; i++){
		out[2*i] = hex[p[i]>>4];
		out[2*i+1] = hex[p[i]&15];
	}
	out[2*n] = 0;
}

/*
 * One snapshot, with D20's retry.  buf takes the engine's error
 * string verbatim — `disk full: <n> object snapshots open, objsnapmax
 * <max>' is layer-a §2.6's `disk full' and goes to the wire as it
 * stands (err.c), so nothing here reformats it.
 */
Objsnap*
srvsnapopen(Store *s, int kinds, char *buf, int nbuf)
{
	Objsnap *sn;

	if((sn = objsnapopen(s, kinds)) != nil)
		return sn;
	rerrstr(buf, nbuf);
	if(strncmp(buf, Emoved, sizeof Emoved - 1) != 0)
		return nil;
	if((sn = objsnapopen(s, kinds)) != nil)
		return sn;
	rerrstr(buf, nbuf);
	return nil;
}

/*
 * §7.2's line, which /tombs and /advert share: /tombs is the tomb
 * half of it and /advert is both halves, and §2.2 says /tombs is in
 * /advert format for exactly that reason.  A tombstone holds no
 * content (§1.5), so its line carries len=0 and state=tomb; the csum
 * is the one the record kept, which is what an advert is compared on.
 */
static char*
snaptext(Srvctx *c, Text *t, int kinds)
{
	char name[Oidmax+1], csum[2*Csumlen+1];
	Objsnap *sn;
	Objinfo oi;
	uchar oid[Oidmax];
	ulong i, n;
	int oidlen, rc;

	if((sn = srvsnapopen(c->store, kinds, rendererr, sizeof rendererr)) == nil)
		return rendererr;
	n = objsnapcount(sn);
	for(i = 0; i < n; i++){
		rc = objsnapent(sn, i, oid, &oidlen, &oi);
		if(rc < 0){
			rerrstr(rendererr, sizeof rendererr);
			objsnapclose(sn);
			return rendererr;
		}
		if(rc == 0)		/* §9's two gone conditions */
			continue;
		memmove(name, oid, oidlen);
		name[oidlen] = 0;
		hexof(csum, oi.csum, Csumlen);
		textprint(t, "oid=%s ver=%llud wepoch=%llud csum=%s len=%llud "
			"state=%s\n", name, oi.ver, oi.wepoch, csum, oi.len,
			oi.state == Stomb ? "tomb" : "live");
	}
	objsnapclose(sn);
	return nil;
}

/* /tombs: the tombstones this instance holds (§2.2, §7.2) */
char*
srvtombstext(Srvctx *c, Sfid *f, Text *t)
{
	USED(f);
	return snaptext(c, t, Snaptomb);
}

/*
 * /advert: §7.2's bulk advertisement, live and tomb in one pass —
 * which is what the receiver needs, since an advert of state=tomb is
 * how §1.5's missed-discard rule is reached.  It is composed once at
 * open like every other file here; §7.2's rate limit is the sender's
 * and there is no sender yet (store.md §14(18)).
 */
char*
srvadverttext(Srvctx *c, Sfid *f, Text *t)
{
	USED(f);
	return snaptext(c, t, Snapboth);
}

/*
 * The open cell every row whose render reaches the engine fills: push
 * to the reserved queue and render there.  srvopentext is the body
 * either way, so a row that offloads and one that does not compose
 * their bytes through the same code (dat.h).
 */
static void
renderq(Req *r)
{
	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	srvopentext(r);
}

void
srvopenq(Req *r)
{
	srvqpushany(r->srv->aux, r, renderq);
}

/*
 * A directory fid gives its snapshot back here.  auxfree may run
 * after storeclose (dat.h, D16), and objsnapclose is one of the three
 * calls store.md §9 allows there — it is also what frees the Store's
 * memory when it is the last snapshot of a closed one.
 */
static void
objdirfree(void *a)
{
	Objdir *d;

	d = a;
	if(d == nil)
		return;
	objsnapclose(d->sn);
	free(d);
}

/*
 * layer-a §5.4.1 step 7 on a fid of this row, which is where a flushed
 * OPEN gives its snapshot back (above).  It runs under the fid's state
 * lock, from srvqdone on the queue proc that is unwinding the open, so
 * it does the give-back inline rather than through srvfidgive, which
 * takes that lock itself.
 *
 * The two tests are what keep it to the open it is for.  A Tread is
 * not it: a flushed read has advanced the cursor over a snapshot the
 * next read continues from, and discarding it would turn a flush into
 * a clunk.  An omode that is not -1 is not it either: lib9p sets the
 * mode in `ropen', which it runs only after a successful open, so a
 * fid that has one held this state before the flushed request arrived.
 */
static void
objdirflush(Sfid *f, Req *r)			/* f->lk held */
{
	Objdir *d;

	if(r->ifcall.type != Topen || r->fid == nil || r->fid->omode != -1)
		return;
	if(f->auxfree != objdirfree || (d = f->aux) == nil)
		return;
	f->aux = nil;
	f->auxflush = nil;
	f->auxclose = nil;
	f->auxfree = nil;
	f->auxclosed = 0;
	objdirfree(d);
}

/*
 * The /obj and /meta open, on the reserved queue.  The fid gives back
 * whatever it was holding first: a fid holds one state, and taking
 * the snapshot without giving the old one back would lose its hooks
 * (dat.h).
 */
static void
objdiropenq(Req *r)
{
	char buf[ERRMAX];
	Srvctx *c;
	Sfid *f;
	Objdir *d;
	Objsnap *sn;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	f = r->fid->aux;
	if((sn = srvsnapopen(c->store, Snaplive, buf, sizeof buf)) == nil){
		srvqdone(r, buf);
		return;
	}
	if((d = mallocz(sizeof *d, 1)) == nil){
		objsnapclose(sn);
		srvqdone(r, "shoalsrv: out of memory");
		return;
	}
	d->sn = sn;
	srvfidgive(f);
	qlock(&f->lk);
	f->aux = d;
	f->auxflush = objdirflush;
	f->auxclose = nil;
	f->auxfree = objdirfree;
	f->auxclosed = 0;
	qunlock(&f->lk);
	srvqexit(r);
	srvqdone(r, nil);
}

void
srvobjdiropen(Req *r)
{
	srvqpushany(r->srv->aux, r, objdiropenq);
}

/*
 * One directory entry, as the object rows' stat answers it (§2.3):
 * the oid is the name, the qid is the object's — with Pmeta set under
 * /meta, since §2.3 gives the second name a path of its own — and the
 * length and mtime are the object's own.  srvdir builds the rest, so
 * a listing and a stat of the same object cannot disagree about the
 * mode or the ownership.
 */
static void
objdirent(Srvctx *c, Sfid *f, uchar *oid, int oidlen, Objinfo *oi, Dir *d)
{
	Sfid g;
	Qid q;

	memset(&g, 0, sizeof g);
	g.file = f->file == Qmeta ? Qmetafile : Qobjfile;
	g.role = f->role;
	memmove(g.oid, oid, oidlen);
	g.oidlen = oidlen;
	srvobjqid(&g, oi, &q);
	g.qidpath = q.path;
	g.qidvers = q.vers;
	srvdir(c, &g, d);
	d->length = oi->len;
	d->mtime = d->atime = oi->mtime;
}

static void
dirfree(Dir *d)
{
	free(d->name);
	free(d->uid);
	free(d->gid);
	free(d->muid);
}

/*
 * The directory read, on the reserved queue.  The fid's state lock is
 * held across the whole of it, which is what dat.h asks of a handler
 * that works on what aux names: a clunk or a walk that moves the fid
 * then waits for this read rather than freeing the snapshot under it.
 *
 * An entry that has gone is skipped and not listed, so no tombstone
 * appears in /obj and nothing is listed twice; the cursor advances
 * past it either way, because the snapshot's positions do not move
 * (§9: "an object created after the open is not in the vector at
 * all").  An entry that will not fit in what the client asked for
 * leaves the cursor standing on it for the next Tread.
 *
 * The cursor is a PAIR and is committed as one.  A read that gives up
 * part-way — objsnapent refusing, which §9 has it do for a store that
 * has stopped serving — has consumed entries without serving their
 * bytes, and lib9p does not advance `Fid.diroffset' on an error
 * (/sys/src/lib9p/srv.c's rread), so the client's retry arrives at the
 * offset this read started at.  Leaving `pos' where the failure left
 * it would make that retry continue past every entry the failed read
 * consumed, and drop them silently — the one thing layer-a §2.2 asks
 * a sequential read not to do.  So the position goes back with the
 * offset it never left.
 */
static void
objdirreadq(Req *r)
{
	char buf[ERRMAX], *e;
	Srvctx *c;
	Sfid *f;
	Objdir *d;
	Objinfo oi;
	Dir dir;
	uchar oid[Oidmax], *p;
	uvlong soff;
	ulong nent, spos;
	long n, m, cnt;
	int oidlen, rc;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	f = r->fid->aux;
	qlock(&f->lk);
	if(f->auxfree != objdirfree || (d = f->aux) == nil){
		qunlock(&f->lk);
		srvqdone(r, Enotbuilt);
		return;
	}
	e = nil;
	if(r->ifcall.offset == 0){
		d->off = 0;
		d->pos = 0;
	}else if(r->ifcall.offset == d->prevoff && d->prevoff != d->off){
		d->off = d->prevoff;
		d->pos = d->prevpos;
	}else if(r->ifcall.offset != d->off)
		e = Eseek;
	n = 0;
	if(e == nil){
		soff = d->off;
		spos = d->pos;
		cnt = r->ifcall.count;
		nent = objsnapcount(d->sn);
		p = (uchar*)r->ofcall.data;
		while(d->pos < nent){
			rc = objsnapent(d->sn, d->pos, oid, &oidlen, &oi);
			if(rc >= 0 && srvslotfail(c, d->pos)){
				werrstr("snapshot entry refused at the point");
				rc = -1;
			}
			if(rc < 0){
				rerrstr(buf, sizeof buf);
				e = buf;
				break;
			}
			d->pos++;
			if(rc == 0)
				continue;
			objdirent(c, f, oid, oidlen, &oi, &dir);
			m = convD2M(&dir, p+n, cnt-n);
			dirfree(&dir);
			if(m <= BIT16SZ){
				d->pos--;
				break;
			}
			n += m;
		}
		if(e == nil){
			d->prevoff = soff;
			d->prevpos = spos;
			d->off = soff + n;
		}else
			d->pos = spos;
	}
	qunlock(&f->lk);
	srvqexit(r);
	if(e != nil){
		srvqdone(r, e);
		return;
	}
	r->ofcall.count = n;
	srvqdone(r, nil);
}

void
srvobjdirread(Req *r)
{
	srvqpushany(r->srv->aux, r, objdirreadq);
}
