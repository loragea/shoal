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
 * layer-a §8.1's file tree and its qids, in one table.
 *
 * Every file of §8.1 is a row, in §8.1's order, carrying its name, the
 * mode a stat and an open report, §8.1's role matrix and the handler
 * cells the rest of the surface fills in.  Walk, stat, open and clunk
 * work for every row now, whether or not its content is built, which
 * is what makes the role matrix testable ahead of the content:
 * /map.next is a row with a render and no write cell, so it is present
 * and empty and answers the role gate first and the local `not built'
 * second.
 *
 * The role matrix.  §8.1 says `reader' and `instance' "may read /map,
 * any /maps/<epoch> … and the status files, and nothing else", and
 * calls `admin' the operator role without listing what it may reach.
 * store.md §14(58) records every cell that fills in and why; the
 * columns are walk, open-for-read and open-for-write, because 9P
 * separates them.
 *
 * Three of those cells are worth naming here.  /ctl is open for
 * WRITING to `instance' and `admin' and not to `reader': §8.3's two
 * verb tables are exactly those two roles, `reader' has no verb in
 * either, and the gate that matters for the two that do is §8.3's
 * per-verb Role column — which has meaning only if an `instance' fid
 * can hold /ctl open and be refused an operator verb.  /ctl is open
 * for READING to `admin' alone: §8.1 grants `reader' and `instance'
 * the map, the retained maps and the status files "and nothing else",
 * and /ctl is not a status file — an OWRITE open is not a read, so
 * the write cell above is untouched by that.  /map.next is `admin'
 * alone in all three columns: it is the operator's staging surface
 * (§8.3), not a status file, and a reader has no business seeing an
 * uncommitted map.
 */

static void	rootread(Req*);
static void	mapsopen(Req*);
static void	mapsread(Req*);
static void	mapread(Req*);

/*
 * One row per file, one field per line: a cell is filled by naming it,
 * so a row grows without its neighbours moving.  A field left out is
 * zero, which for a handler cell is `not built' and for a role column
 * is `no role'.
 */
Mfile monfiles[Nmfile] =
{
[Qmroot] = {
	.name	= nil,
	.isdir	= 1,
	.perm	= DMDIR|0555,
	.walk	= Amall,
	.rd	= Amall,
	.wr	= 0,
	.read	= rootread,
},
[Qctl] = {
	.name	= "ctl",
	.perm	= 0666,
	.walk	= Amall,
	.rd	= Amadmin,
	.wr	= Ainstance|Amadmin,
	.render	= monemptytext,
	.write	= monctlwrite,
},
[Qmap] = {
	.name	= "map",
	.perm	= 0444,
	.walk	= Amall,
	.rd	= Amall,
	.wr	= 0,
	.render	= monmaptext,
	.read	= mapread,
},
[Qmapnext] = {
	.name	= "map.next",
	.perm	= 0666,
	.walk	= Amadmin,
	.rd	= Amadmin,
	.wr	= Amadmin,
	.render	= monemptytext,
},
[Qmaps] = {
	.name	= "maps",
	.isdir	= 1,
	.perm	= DMDIR|0555,
	.walk	= Amall,
	.rd	= Amall,
	.wr	= 0,
	.open	= mapsopen,
	.read	= mapsread,
},
[Qinstances] = {
	.name	= "instances",
	.perm	= 0444,
	.walk	= Amall,
	.rd	= Amall,
	.wr	= 0,
	.render	= moninstancestext,
},
[Qmstale] = {
	.name	= "stale",
	.perm	= 0444,
	.walk	= Amall,
	.rd	= Amall,
	.wr	= 0,
	.render	= monstaletext,
},
[Qhealth] = {
	.name	= "health",
	.perm	= 0444,
	.walk	= Amall,
	.rd	= Amall,
	.wr	= 0,
	.render	= monhealthtext,
},
[Qmstatus] = {
	.name	= "status",
	.perm	= 0444,
	.walk	= Amall,
	.rd	= Amall,
	.wr	= 0,
	.render	= monstatustext,
},
[Qmapfile] = {
	.name	= nil,
	.perm	= 0444,
	.walk	= Amall,
	.rd	= Amall,
	.wr	= 0,
	.render	= monmapfiletext,
},
};

static int
rolebit(int role)
{
	return 1<<role;
}

/*
 * A /maps element.  The same u64 rule srv/attach.c reads an `epoch='
 * with: a run of decimal digits and nothing else, and leading zeros
 * are part of the number, so /maps/007 names epoch 7 exactly as
 * /maps/7 does.  A name that is not a u64 at all is not an epoch and
 * answers `no such file'; one that is answers `no such epoch' when the
 * ring no longer holds it (store.md §14(55)).
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

void
monfileqid(int file, Qid *q)
{
	q->path = Pmfixed + file;
	q->vers = 0;
	q->type = monfiles[file].isdir ? QTDIR : QTFILE;
}

/*
 * The Dir a stat answers.  Every file of this tree is synthetic — its
 * bytes are composed when the fid is opened — so every one carries
 * length 0, which is the Plan 9 convention for exactly that.  A
 * /maps/<epoch> entry carries it too, although its length is known and
 * fixed: one rule for the whole tree is worth more than a byte count
 * on one row, and a reader that wants the size reads the file.
 * store.md §14(60) records it.
 */
void
mondir(Monctx *c, Mfid *f, Dir *d)
{
	char buf[32];
	Mfile *e;

	USED(c);
	e = &monfiles[f->file];
	memset(d, 0, sizeof *d);
	if(f->file == Qmapfile){
		snprint(buf, sizeof buf, "%llud", f->epoch);
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
 * The table is fixed, so nothing can tear between two Treads of it —
 * which is why the root needs no snapshot and /maps does.
 */
static int
rootgen(int i, Dir *d, void *aux)
{
	Mfid *f, g;
	int n;

	f = aux;
	for(n = Qmroot+1; n < Qmapfile; n++){
		if(monfiles[n].name == nil)
			continue;
		if((monfiles[n].walk & rolebit(f->role)) == 0)
			continue;
		if(i-- == 0)
			break;
	}
	if(n >= Qmapfile)
		return -1;
	memset(&g, 0, sizeof g);
	g.file = n;
	g.role = f->role;
	g.qidpath = Pmfixed + n;
	mondir(nil, &g, d);
	return 0;
}

static void
rootread(Req *r)
{
	dirread9p(r, rootgen, r->fid->aux);
	respond(r, nil);
}

/*
 * /maps is snapshot-at-open like every other file of §8.1, and for the
 * same reason: lib9p's dirread9p regenerates entry n per Tread, so a
 * publish landing between two reads of a listing generated live would
 * shift every index below it and the reader would skip or repeat an
 * epoch.  The snapshot is the ring's positions, not its bytes — eight
 * pairs of u64 by default — and a walk to one of the names still asks
 * the store, so an epoch that aged out between the listing and the
 * walk is refused rather than served from a stale copy.
 *
 * `forceepoch' and §8.6's rebuild path may publish one epoch twice
 * (lib/shoal.h), and two ring entries carrying one epoch would put one
 * name in this listing twice.  monhistory walks newest-first, so the
 * first occurrence is the one monlookup answers — the greater seq —
 * and the rest are dropped: the listing and the walk agree, which is
 * what a directory has to do.  store.md §14(55) records it.
 */
static void
mapsopen(Req *r)
{
	Monctx *c;
	Mfid *f;
	Mdirent *dir;
	Monmap mm;
	Monstat st;
	ulong i, n;
	int j, dup;

	c = r->srv->aux;
	f = r->fid->aux;
	monsrvlock(c);
	monstat(c->mon, &st);
	n = 0;
	if((dir = mallocz((st.retain+1)*sizeof *dir, 1)) == nil){
		monsrvunlock(c);
		respond(r, "shoalmon: out of memory");
		return;
	}
	for(i = 0; i <= st.retain; i++){
		if(!monhistory(c->mon, i, &mm))
			break;
		dup = 0;
		for(j = 0; j < (int)n; j++)
			if(dir[j].epoch == mm.epoch)
				dup = 1;
		if(dup)
			continue;
		dir[n].epoch = mm.epoch;
		dir[n].seq = mm.seq;
		n++;
	}
	monsrvunlock(c);
	free(f->dir);
	f->dir = dir;
	f->ndir = n;
	respond(r, nil);
}

static int
mapsgen(int i, Dir *d, void *aux)
{
	Mfid *f, g;

	f = aux;
	if(i < 0 || i >= f->ndir)
		return -1;
	memset(&g, 0, sizeof g);
	g.file = Qmapfile;
	g.role = f->role;
	g.epoch = f->dir[i].epoch;
	g.qidpath = f->dir[i].seq;
	mondir(nil, &g, d);
	return 0;
}

static void
mapsread(Req *r)
{
	dirread9p(r, mapsgen, r->fid->aux);
	respond(r, nil);
}

/*
 * Is the snapshot this fid serves still the current map?  The render
 * stamped the fid with the `seq' of the map it copied (status.c), and
 * `seq' is the slot store's own: one space for the whole store,
 * strictly increasing and never reused, so the comparison is exact
 * across a ring wrap and across an epoch published twice.  A store
 * that now holds no map answers no, and so does a fid that never
 * rendered a map, whose stamp is 0 and which no published seq matches.
 *
 * monlk is taken per Tread for this.  Nothing in this service blocks
 * under that lock — every accessor answers out of memory (mon.c) —
 * so the cost is a QLock round trip on the one read that records.
 */
static int
mapcurrent(Monctx *c, Mfid *f)
{
	Monmap mm;
	int ok;

	monsrvlock(c);
	ok = moncurrent(c->mon, &mm) && mm.seq == f->mapseq;
	monsrvunlock(c);
	return ok;
}

/*
 * /map's read, which is the one read in this tree that records
 * anything.  layer-a §8.4 makes lastseen(i) "the time of instance i's
 * most recent successful read of the monitor's /map … on an attach
 * with role=instance,peer=i", and store.md §14(56) fixes what a
 * successful read is: a Tread of /map, on a fid whose ATTACH carried
 * role=instance, answered with a count greater than zero, from a
 * snapshot that is still the current map.  A count of zero is end of
 * data and moves nothing, and no other role's read of anything moves
 * anything.
 *
 * The currency clause is what keeps §6.4's F1 and F2 composable.
 * /map is snapshot-at-open, so a fid opened at epoch E answers E's
 * bytes for as long as it is held; without the clause an instance
 * that never reopens renews its lease forever off a map the cluster
 * has left behind — it received bytes, so it does not self-fence
 * under F1, and the monitor will not demote it at `deadms' because
 * the channel looks alive.  §6.3 already refuses to count an
 * epoch-regressed map as a refresh, and this is the monitor's side of
 * the same judgement (store.md §14(56)).
 *
 * It is recorded immediately before the reply, so the time is the time
 * the instance was answered.
 */
static void
mapread(Req *r)
{
	Monctx *c;
	Mfid *f;

	c = r->srv->aux;
	f = r->fid->aux;
	if(f->text == nil){
		respond(r, Emonnotbuilt);
		return;
	}
	readbuf(r, f->text->p, f->text->n);
	if(r->ofcall.count > 0 && f->role == Minstance && mapcurrent(c, f))
		monsrvseen(c, f->peer);
	respond(r, nil);
}

/*
 * One walk element.  f is a working copy of the fid's state; on
 * success it is advanced onto the named file.
 *
 * Every element, `..' included, needs a directory to walk out of.
 * lib9p's own Ewalknodir covers only the fid a Twalk STARTS from, so
 * an element that lands on a file and is followed by another is this
 * server's to refuse — and `..' is such an element, not an exemption
 * from the rule: without this /ctl/.. would answer the root and
 * /maps/<epoch>/.. would answer /maps.  The refusal is the same
 * `no such file' every other element of a non-directory gets, which
 * is what srv/tree.c answers for the same case.
 */
static char*
walk1(Monctx *c, Mfid *f, char *name, Qid *q)
{
	Monmap mm;
	Mfile *e;
	uvlong epoch;
	int i, ok;

	if(!monfiles[f->file].isdir)
		return Emonnofile;
	if(strcmp(name, "..") == 0){
		f->file = f->file == Qmapfile ? Qmaps : Qmroot;
		f->epoch = 0;
		f->qidpath = Pmfixed + f->file;
		f->qidvers = 0;
		monfileqid(f->file, q);
		return nil;
	}
	if(f->file == Qmaps){
		if(u64(name, &epoch) < 0)
			return Emonnofile;
		monsrvlock(c);
		ok = monlookup(c->mon, epoch, &mm);
		monsrvunlock(c);
		if(!ok)
			return Emonnoepoch;
		f->file = Qmapfile;
		f->epoch = epoch;
		f->qidpath = mm.seq;
		f->qidvers = 0;
		q->path = f->qidpath;
		q->vers = 0;
		q->type = QTFILE;
		return nil;
	}
	if(f->file != Qmroot)
		return Emonnofile;
	for(i = Qmroot+1; i < Qmapfile; i++){
		e = &monfiles[i];
		if(e->name != nil && strcmp(e->name, name) == 0)
			break;
	}
	if(i >= Qmapfile)
		return Emonnofile;
	if((monfiles[i].walk & rolebit(f->role)) == 0)
		return Emperm;
	f->file = i;
	f->epoch = 0;
	monfileqid(i, q);
	f->qidpath = q->path;
	f->qidvers = q->vers;
	return nil;
}

/*
 * The walk itself.  A walk that does not resolve every element leaves
 * both fids exactly where they were — which is what 9P asks and what
 * lib9p's own walkandclone does not do — and that holds for a walk of
 * a fid onto itself too.  The Mfid is therefore committed only when
 * every element resolved.
 */
void
monsrvwalk(Req *r)
{
	Monctx *c;
	Mfid *f, *nf, g;
	char *e;
	int i;

	c = r->srv->aux;
	f = r->fid->aux;
	g = *f;
	g.text = nil;
	g.mapseq = 0;
	g.dir = nil;
	g.ndir = 0;
	e = nil;
	for(i = 0; i < r->ifcall.nwname; i++){
		e = walk1(c, &g, r->ifcall.wname[i], &r->ofcall.wqid[i]);
		if(e != nil)
			break;
	}
	r->ofcall.nwqid = i;
	if(e != nil && i == 0){
		respond(r, e);
		return;
	}
	if(i == r->ifcall.nwname){
		if(r->fid == r->newfid && r->ifcall.nwname > 0){
			/*
			 * The fid moves, and what it held does not move with
			 * it: a fid names one file, and the snapshot it
			 * carries is that file's.  A walk of a fid onto
			 * itself that names nothing is 9P's probe of that
			 * fid and leaves what it holds alone.
			 */
			montextfree(f->text);
			f->text = nil;
			f->mapseq = 0;
			free(f->dir);
			f->dir = nil;
			f->ndir = 0;
			f->file = g.file;
			f->epoch = g.epoch;
			f->qidpath = g.qidpath;
			f->qidvers = g.qidvers;
		}else if(r->fid != r->newfid){
			if((nf = mallocz(sizeof *nf, 1)) == nil){
				respond(r, "shoalmon: out of memory");
				return;
			}
			*nf = g;
			r->newfid->aux = nf;
		}
	}
	respond(r, nil);
}

/*
 * The render-at-open body.  It runs under the slot store's lock, so a
 * publish cannot land between the map a render reads and the ring it
 * reads next — which is the whole of what §8.1's snapshot-at-open is
 * for here — and the bytes are copied into the fid's own Mtext, which
 * is what keeps them alive past the next commit (mon.h).
 */
static void
opentext(Req *r)
{
	char *e;
	Monctx *c;
	Mfid *f;
	Mfile *file;
	Mtext *t;

	c = r->srv->aux;
	f = r->fid->aux;
	file = &monfiles[f->file];
	if((t = montextnew()) == nil){
		respond(r, "shoalmon: out of memory");
		return;
	}
	monsrvlock(c);
	e = file->render(c, f, t);
	monsrvunlock(c);
	if(e != nil){
		montextfree(t);
		respond(r, e);
		return;
	}
	if(t->err){
		montextfree(t);
		respond(r, "shoalmon: out of memory");
		return;
	}
	montextfree(f->text);
	f->text = t;
	respond(r, nil);
}

/*
 * Open.  §8.1's role gate first, then the row's open hook, or its
 * render-at-open snapshot, or the local `not built'.  There is no
 * per-row gate here and no fence: the monitor is where the epochs come
 * from, so nothing in §6.4 fences it, and the only state that could
 * refuse an open is the map's own absence, which /map's render
 * answers.
 */
void
monsrvopen(Req *r)
{
	Monctx *c;
	Mfid *f;
	Mfile *file;
	int need, b;

	c = r->srv->aux;
	USED(c);
	f = r->fid->aux;
	file = &monfiles[f->file];
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
		respond(r, Emperm);
		return;
	}
	if(file->open != nil){
		file->open(r);
		return;
	}
	if(file->render != nil){
		opentext(r);
		return;
	}
	if(file->read != nil || file->write != nil){
		respond(r, nil);
		return;
	}
	respond(r, Emonnotbuilt);
}

/*
 * Read and write.  There is no role gate here — 9P settles the role at
 * the open, which monsrvopen gated — and no row of this tree has a
 * gate of its own.
 *
 * A row with a read cell gets every read, whether or not the fid also
 * holds a rendered Mtext: /map is such a row, because §8.4's evidence
 * is recorded on the read and not on the open.  Only a row with a
 * render and no read takes the automatic text path.
 */
void
monsrvread(Req *r)
{
	Mfid *f;
	Mfile *file;

	f = r->fid->aux;
	file = &monfiles[f->file];
	if(file->read != nil){
		file->read(r);
		return;
	}
	if(f->text != nil){
		montextread(r, f->text);
		return;
	}
	respond(r, Emonnotbuilt);
}

void
monsrvwrite(Req *r)
{
	Mfid *f;
	Mfile *file;

	f = r->fid->aux;
	file = &monfiles[f->file];
	if(file->write != nil){
		file->write(r);
		return;
	}
	respond(r, Emonnotbuilt);
}

void
monsrvstat(Req *r)
{
	Monctx *c;
	Mfid *f;

	c = r->srv->aux;
	f = r->fid->aux;
	mondir(c, f, &r->d);
	respond(r, nil);
}

/*
 * Create, remove and wstat, in the write column: §8.1's role gate,
 * then the row's own cell, which for every row of this tree is nil.
 * Nothing in §8.1 or §8.3 creates, removes or renames anything in this
 * tree — a map is published by a ctl verb — so the three exist to
 * answer, not to act.
 */
static void
wrop(Req *r, void (*cell)(Req*))
{
	Mfid *f;
	Mfile *file;

	f = r->fid->aux;
	file = &monfiles[f->file];
	if((file->wr & rolebit(f->role)) == 0){
		respond(r, Emperm);
		return;
	}
	if(cell != nil){
		cell(r);
		return;
	}
	respond(r, Emonnotbuilt);
}

void
monsrvcreate(Req *r)
{
	wrop(r, nil);
}

void
monsrvremove(Req *r)
{
	wrop(r, nil);
}

void
monsrvwstat(Req *r)
{
	wrop(r, nil);
}

/*
 * A fid's state goes when lib9p frees the fid.  Nothing here reaches
 * the slot store, so it is safe before the shutdown and after it
 * alike: what a fid holds is a copy of bytes and a copy of ring
 * positions, and neither outlives this call (dat.h).
 */
void
monsrvdestroyfid(Fid *fid)
{
	Mfid *f;

	if((f = fid->aux) == nil)
		return;
	fid->aux = nil;
	montextfree(f->text);
	free(f->dir);
	free(f);
}
