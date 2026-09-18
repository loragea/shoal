#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../lib/shoal.h"
#include "../srv/srv.h"

/*
 * T1: layer-a §2.4's object I/O over the 9P surface — the open mode
 * rules, read with holes and the clamp to `len', write with its short
 * count and its bounds, the Tcreate in /obj, Tremove, the Twstat that
 * truncates, /meta/<oid>, and the per-fid stage layer-a §5.4 step 3
 * creates and §5.4.1 step 7 discards.
 *
 * A whole instance runs inside this program, as in srvtest: a
 * simulated disk, the store engine over it, srv/libshoalsrv.a over
 * that, and a raw 9P client on the other end of a pipe (srv9p.h).
 * What a case asserts is the exact bytes of a reply.
 *
 * The maps are this program's own, because what §5.4 steps 4 and 5
 * answer depends on the placement the map produces (store.md §14(30)):
 * a map with one placement member acks alone, and one whose placement
 * holds a member this instance cannot have replicated to answers
 * `degraded'.  T1 maps are therefore replicas=1 unless the case is
 * about the other answer.
 */

int mainstacksize = Srvstack;

static int fails;
static int checks;

static void
fail(char *fmt, ...)
{
	char buf[512];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf + sizeof buf, fmt, arg);
	va_end(arg);
	fprint(2, "FAIL: %s\n", buf);
	fails++;
}

static void
eqv(char *what, uvlong got, uvlong want)
{
	checks++;
	if(got != want)
		fail("%s: %llud, want %llud", what, got, want);
}

static void
istrue(char *what, int ok)
{
	checks++;
	if(!ok)
		fail("%s", what);
}

static void
eqs(char *what, char *got, char *want)
{
	checks++;
	if(got == nil)
		got = "";
	if(want == nil)
		want = "";
	if(strcmp(got, want) != 0)
		fail("%s: %#q, want %#q", what, got, want);
}

#include "srv9p.h"

enum
{
	Tsecsz	= 512,
	Tnsec	= 16384,		/* an 8 MiB image */
	Tseed	= 0x5ea1,

	Tblksz	= 4096,
	Tobjmax	= 1<<20,
	Tepoch	= 7,
	Tnslots	= 128,

	/* fids the cases use */
	Froot	= 1,
	Ffile	= 2,
	Ffile2	= 3,
	Fdir	= 4,
	Froot2	= 5,
	Fmeta	= 6,
	Fctl	= 7,
};

static char Tuuid[] = "0000000000000000000000000000000a";
static char Tmonid[] = "00112233445566778899aabbccddeeff";
static char Nclient[] = "role=client,epoch=7";
static char Nadmin[] = "role=admin";

/*
 * The maps.  n1.0 is this instance (its uuid is the disk's); the
 * second record is what decides §5.4 step 5's answer, so each case
 * takes the map that puts it in the case it is about.
 *
 *	peer=absent	replicas=1 and one placeable instance: P(o) is
 *			this instance alone, M is empty, and a write acks
 *			alone (§5.4 step 5's fast path).
 *	peer=down	replicas=2 with the other instance up=no: P(o)
 *			holds a member that took nothing, so the write
 *			owes step 5a a stale mark at a monitor that is not
 *			built and answers `degraded'.
 *	peer=up		replicas=1 with two placeable instances: HRW
 *			sends some ids to the other one, and a client
 *			operation on those is `not primary'.
 *	self=out	replicas=1 with this instance status=out and the
 *			other dead: no instance places, so P(o) is empty and
 *			the object has no primary (§4.3).  role=client I/O
 *			is F3's `down' here; the operator's read is not
 *			(§2.1), and is what reaches the render.
 */
enum
{
	Palone	= 0,
	Pdown,
	Pup,
	Pout,
};

static char*
mkmap(int kind, ulong blksz, uvlong objmax, char *uuid)
{
	char *p;

	p = smprint(
		"map=t epoch=%d\n"
		"\tmonid=%s\n"
		"\tobjmax=%llud blksz=%lud replicas=%d\n"
		"\tcsumalg=blake2s256 placehash=blake2s256-64\n"
		"\tpollms=1000 leasems=3000 replms=1000 deadms=10000\n"
		"\toutmins=60 tombdays=7 mincopies=1 retain=8\n"
		"\n"
		"instance=n1.0 onnode=n1 addr=tcp!10.0.0.1!17011\n"
		"\tuuid=%s\n"
		"\tclass=ssd weight=100 status=%s up=yes since=1 fenced=no\n"
		"instance=n2.0 onnode=n2 addr=tcp!10.0.0.2!17011\n"
		"\tuuid=0000000000000000000000000000000c\n"
		"\tclass=ssd weight=100 status=%s up=%s since=1 fenced=no\n",
		Tepoch, Tmonid, objmax, blksz, kind == Pdown ? 2 : 1, uuid,
		kind == Pout ? "out" : "in",
		kind == Palone || kind == Pout ? "dead" : "in",
		kind == Pup ? "yes" : "no");
	if(p == nil)
		sysfatal("smprint: %r");
	return p;
}

static void
fmtcfg(Fmtcfg *c, ulong nslots)
{
	memset(c, 0, sizeof *c);
	c->secsz = Tsecsz;
	c->blksz = Tblksz;
	c->objmax = Tobjmax;
	c->nslots = nslots;
	c->nemap = 32;
	c->ndirty = 64;
	c->logbytes = 256*1024;
	c->csumalg = Csumblake2s;
	memset(c->uuid, 0, 16);
	c->uuid[15] = 0x0a;		/* Tuuid */
	c->uuidset = 1;
}

static Dev*
newdisk(ulong nslots)
{
	Dev *d;
	Super s;
	Fmtcfg c;

	if((d = simopen(Tsecsz, Tnsec, Tseed)) == nil)
		sysfatal("simopen: %r");
	fmtcfg(&c, nslots);
	if(geometry(&s, &c, d->size) < 0)
		sysfatal("geometry: %r");
	if(fmtstore(d, &s) < 0)
		sysfatal("fmtstore: %r");
	return d;
}

static int
tspawn(void (*fn)(void*), void *a)
{
	if(proccreate(fn, a, Srvstack) < 0)
		return -1;
	return 0;
}

static Srvctx*
startsrv(Dev *d, char *maptext, ulong stagemax, ulong stagems)
{
	Srvcfg cfg;
	Srvctx *c;

	memset(&cfg, 0, sizeof cfg);
	cfg.dev = d;
	cfg.maptext = maptext;
	cfg.maplen = strlen(maptext);
	cfg.nqueue = 4;
	cfg.store.spawn = tspawn;
	cfg.store.nockptproc = 1;
	cfg.store.ckwaitms = 200;
	cfg.store.emapcache = 16;
	cfg.store.stagemax = stagemax;
	cfg.store.stagetot = 64;
	cfg.store.stagems = stagems;
	if((c = srvnew(&cfg)) == nil)
		fail("srvnew: %r");
	return c;
}

/* create an object through the engine, which is not what is under test */
static void
mkobj(Store *s, char *name, void *data, long n)
{
	Objinfo oi;
	uchar oid[Oidmax];
	int len;

	len = strlen(name);
	memmove(oid, name, len);
	if(objcreate(s, oid, len, 1, Tepoch, nil, 0, &oi) < 0){
		fail("objcreate %s: %r", name);
		return;
	}
	if(n > 0 && objwrite(s, oid, len, data, n, 0, 2, Tepoch, nil, 0) < 0)
		fail("objwrite %s: %r", name);
}

static int
statof(Store *s, char *name, Objinfo *oi)
{
	uchar oid[Oidmax];
	int len;

	len = strlen(name);
	memmove(oid, name, len);
	return objstat(s, oid, len, oi);
}

/* a Twrite of arbitrary bytes; srv9p.h's clwrite takes a string */
static int
clwriteb(Cl *c, ulong fid, vlong off, void *a, long n, Fcall *r)
{
	Fcall t;

	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = cltag(c);
	t.fid = fid;
	t.offset = off;
	t.count = n;
	t.data = a;
	return clrpc(c, &t, r);
}

/*
 * Wait for the pool to be quiet — every request it has taken has
 * completed — and then for it to have taken n more.  A case that
 * pipelines requests has to know the server reached a given point
 * before it sends the next message, and the pool's counters are what
 * say so; a sleep only hopes (store.md §7).
 */
static void
waitidle(Srvctx *ctx, uvlong *np, uvlong *nd)
{
	int i;

	for(i = 0; i < 400; i++){
		srvcount(ctx, np, nd);
		if(*np == *nd)
			return;
		sleep(5);
	}
}

static void
waitpush(Srvctx *ctx, uvlong np0, uvlong n, uvlong *np, uvlong *nd)
{
	int i;

	for(i = 0; i < 400; i++){
		srvcount(ctx, np, nd);
		if(*np - np0 >= n)
			return;
		sleep(5);
	}
}

/* walk a fresh fid to /obj/<name> or /meta/<name> */
static int
clwalkobj(Cl *c, ulong root, ulong fid, char *dir, char *name, Fcall *r)
{
	char *w[2];

	w[0] = dir;
	w[1] = name;
	return clwalk(c, root, fid, 2, w, r);
}

/*
 * One attr=value out of the /meta line, which is one physical line
 * with its fields separated by spaces (§2.4) — srv9p.h's clfield is
 * for the status files, whose fields are one to a line.
 */
static char*
metafield(char *text, char *attr, char *buf, int nbuf)
{
	char *p, *e;
	int n;

	n = strlen(attr);
	for(p = text; p != nil && *p != 0; p = e){
		if(strncmp(p, attr, n) == 0 && p[n] == '='){
			p += n+1;
			for(e = p; *e != 0 && *e != ' ' && *e != '\n'; e++)
				;
			if(e - p >= nbuf)
				return nil;
			memmove(buf, p, e-p);
			buf[e-p] = 0;
			return buf;
		}
		for(e = p; *e != 0 && *e != ' ' && *e != '\n'; e++)
			;
		while(*e == ' ' || *e == '\n')
			e++;
	}
	return nil;
}

static long
statlen(Cl *c, ulong fid, Fcall *r)
{
	char buf[64];
	Dir d;

	if(clstat(c, fid, r) != Rstat)
		return -1;
	if(convM2D(r->stat, r->nstat, &d, buf) <= BIT16SZ)
		return -1;
	return d.length;
}

/*
 * §2.4's read, write and the two bounds that are not §5.4's: the clamp
 * to `len', the zeroes a hole reads as, and `object too large'.  The
 * map is the one that acks alone, so what is asserted is the I/O and
 * not the replication round.
 */
static void
tio(void)
{
	char buf[64], sbuf[512], *m;
	uchar data[2048], want[8192];
	Objinfo oi;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	Dir dir;
	int i;

	clstage = "io";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	for(i = 0; i < sizeof data; i++)
		data[i] = 0xA5 ^ (uchar)i;
	mkobj(srvstore(ctx), "alpha", data, sizeof data);
	mkobj(srvstore(ctx), "empty", nil, 0);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, ORDWR, &r) != Ropen){
		fail("open /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	/* a read of what the object holds */
	if(clread(&cl, Ffile, 0, 512, &r) != Rread)
		fail("read /obj/alpha: %s", clerr(&r));
	else{
		eqv("a read answers the count it asked for", r.count, 512);
		istrue("... and the bytes the object holds",
			memcmp(r.data, data, 512) == 0);
	}
	/* §2.4: a read crossing len answers only the bytes below it */
	if(clread(&cl, Ffile, sizeof data - 16, 512, &r) != Rread)
		fail("read across len: %s", clerr(&r));
	else
		eqv("a read crossing len stops at len", r.count, 16);
	/* ... and one at or past len answers 0 */
	if(clread(&cl, Ffile, sizeof data, 512, &r) != Rread)
		fail("read at len: %s", clerr(&r));
	else
		eqv("a read at len answers count 0", r.count, 0);
	if(clread(&cl, Ffile, sizeof data + 4096, 512, &r) != Rread)
		fail("read past len: %s", clerr(&r));
	else
		eqv("a read past len answers count 0", r.count, 0);

	/* a write, and the read that sees it */
	if(clwriteb(&cl, Ffile, 64, "shoal", 5, &r) != Rwrite)
		fail("write /obj/alpha: %s", clerr(&r));
	else
		eqv("a write answers the count it took", r.count, 5);
	if(clread(&cl, Ffile, 64, 5, &r) != Rread)
		fail("read back the write: %s", clerr(&r));
	else{
		memmove(buf, r.data, r.count);
		buf[r.count] = 0;
		eqs("a read answers what the write left", buf, "shoal");
	}
	if(statof(srvstore(ctx), "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	else{
		eqv("a write bumps ver by one", oi.ver, 3);
		eqv("... at the epoch the instance adopted", oi.wepoch, Tepoch);
	}

	/*
	 * §2.4: a write at an offset above len extends the object and the
	 * gap becomes a hole, which MUST read as zero.
	 */
	if(clwriteb(&cl, Ffile, 6000, "hole", 4, &r) != Rwrite)
		fail("write past len: %s", clerr(&r));
	eqv("an extending write answers its own count", r.count, 4);
	if(clread(&cl, Ffile, sizeof data, 6000 - sizeof data, &r) != Rread)
		fail("read the hole: %s", clerr(&r));
	else{
		memset(want, 0, sizeof want);
		eqv("a read of the hole answers the bytes it asked for",
			r.count, 6000 - sizeof data);
		istrue("... and every one of them is zero",
			memcmp(r.data, want, r.count) == 0);
	}
	if(clread(&cl, Ffile, sizeof data, 8192, &r) != Rread)
		fail("read across the new len: %s", clerr(&r));
	else
		eqv("a read of hole and bytes stops at the new len", r.count,
			6004 - sizeof data);
	eqv("stat length is the extended len", statlen(&cl, Ffile, &r), 6004);

	/* §2.3's stat of an object, in full */
	if(clstat(&cl, Ffile, &r) != Rstat)
		fail("stat /obj/alpha: %s", clerr(&r));
	else if(convM2D(r.stat, r.nstat, &dir, sbuf) <= BIT16SZ)
		fail("a stat of /obj/alpha that convM2D will not read");
	else if(statof(srvstore(ctx), "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	else{
		eqs("a stat names the object", dir.name, "alpha");
		eqv("... with the object's own length", dir.length, oi.len);
		eqv("... its mtime", dir.mtime, (ulong)oi.mtime);
		eqv("... mode 0666, which is §2.2's row", dir.mode, 0666);
		eqv("... and qid.vers, the low 32 of ver (§2.3)", dir.qid.vers,
			(ulong)(oi.ver & 0xFFFFFFFFULL));
		eqv("... over the object's own qid.path", dir.qid.path,
			oi.qidpath);
	}

	/* §2.6: a write past objmax at either bound */
	if(clwriteb(&cl, Ffile, Tobjmax, "x", 1, &r) == Rwrite)
		fail("a write at objmax was taken");
	clerris("a write at objmax", &r, "object too large");
	if(clwriteb(&cl, Ffile, Tobjmax - 2, "xyz", 3, &r) == Rwrite)
		fail("a write across objmax was taken");
	clerris("a write crossing objmax", &r, "object too large");

	/* a count-0 write commits nothing and is not an extend (§3.6) */
	if(clwriteb(&cl, Ffile, 9999, "", 0, &r) != Rwrite)
		fail("a count-0 write: %s", clerr(&r));
	else
		eqv("a count-0 write answers 0", r.count, 0);
	eqv("... and does not extend the object", statlen(&cl, Ffile, &r), 6004);
	clclunk(&cl, Ffile, &r);

	/* a read of an object with no content at all */
	if(clwalkobj(&cl, Froot, Ffile, "obj", "empty", &r) != Rwalk
	|| clopen(&cl, Ffile, OREAD, &r) != Ropen)
		fail("open /obj/empty: %s", clerr(&r));
	else if(clread(&cl, Ffile, 0, 512, &r) != Rread)
		fail("read /obj/empty: %s", clerr(&r));
	else
		eqv("a read of a zero-length object answers 0", r.count, 0);
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §2.4's mode rules, and the wstat that truncates.  ORCLOSE MUST be
 * `bad open mode'; so is a mode §2.4 does not admit.  A Twstat rename
 * MUST be `no rename', and every other settable field is refused —
 * with this server's own string, since §2.6 names no condition for it
 * (store.md §14(34)).
 */
static void
tmodes(void)
{
	char *m;
	uchar data[2048];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	Dir dir;
	int i;

	clstage = "modes";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	for(i = 0; i < sizeof data; i++)
		data[i] = i;
	mkobj(srvstore(ctx), "alpha", data, sizeof data);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk){
		fail("walk /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	clopen(&cl, Ffile, OREAD|ORCLOSE, &r);
	clerris("an open with ORCLOSE", &r, "bad open mode");
	clopen(&cl, Ffile, OEXEC, &r);
	clerris("an open for execution", &r, "bad open mode");
	/*
	 * A mode bit 9P carries in its one-byte mode field and §2.4 does
	 * not admit; OEXCL is not one of them, being above that byte.
	 */
	clopen(&cl, Ffile, OREAD|0x08, &r);
	clerris("an open with a mode bit §2.4 does not admit", &r,
		"bad open mode");

	/*
	 * §2.4's other mode rule — a write on a fid opened OREAD — never
	 * reaches this server: lib9p refuses it before Srv.write with its
	 * own protocol botch (store.md §14(32)).
	 */
	if(clopen(&cl, Ffile, OREAD, &r) != Ropen)
		fail("open /obj/alpha for reading: %s", clerr(&r));
	clwriteb(&cl, Ffile, 0, "x", 1, &r);
	clerris("a write on a fid opened OREAD", &r, "9P protocol botch");
	clclunk(&cl, Ffile, &r);

	/* OTRUNC is a truncate to zero, and it takes §5.4's path */
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, OWRITE|OTRUNC, &r) != Ropen)
		fail("open /obj/alpha OTRUNC: %s", clerr(&r));
	eqv("OTRUNC truncates the object at the open",
		statlen(&cl, Ffile, &r), 0);
	if(clwriteb(&cl, Ffile, 0, "0123456789", 10, &r) != Rwrite)
		fail("write after OTRUNC: %s", clerr(&r));
	clclunk(&cl, Ffile, &r);

	/* §2.4's wstat: length, and nothing else */
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk){
		fail("walk /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	nulldir(&dir);
	dir.name = "beta";
	clwstat(&cl, Ffile, &dir, &r);
	clerris("a wstat that renames", &r, "no rename");
	nulldir(&dir);
	dir.mode = 0600;
	clwstat(&cl, Ffile, &dir, &r);
	clerris("a wstat that sets the mode", &r,
		"shoalsrv: only length may be set");
	nulldir(&dir);
	dir.mtime = 1;
	clwstat(&cl, Ffile, &dir, &r);
	clerris("a wstat that sets mtime", &r,
		"shoalsrv: only length may be set");
	/*
	 * stat(5) makes `type', `dev' and `qid' don't-touch on every
	 * Twstat.  The first two are this row's to refuse; a qid that
	 * differs from the fid's own is lib9p's, answered in its words
	 * before Srv.wstat (store.md §14(34)).
	 */
	nulldir(&dir);
	dir.type = 1;
	clwstat(&cl, Ffile, &dir, &r);
	clerris("a wstat that sets the server type", &r,
		"shoalsrv: only length may be set");
	nulldir(&dir);
	dir.dev = 1;
	clwstat(&cl, Ffile, &dir, &r);
	clerris("a wstat that sets the server subtype", &r,
		"shoalsrv: only length may be set");
	nulldir(&dir);
	dir.qid.vers = 0xdeadbeef;
	clwstat(&cl, Ffile, &dir, &r);
	clerris("a wstat that sets qid.vers", &r,
		"wstat -- attempt to change qid.vers");
	nulldir(&dir);
	clwstat(&cl, Ffile, &dir, &r);
	checks++;
	if(r.type != Rwstat)
		fail("a wstat that sets nothing: %s", clerr(&r));
	nulldir(&dir);
	dir.length = 4;
	clwstat(&cl, Ffile, &dir, &r);
	checks++;
	if(r.type != Rwstat)
		fail("a wstat that truncates: %s", clerr(&r));
	eqv("... and the object is that long", statlen(&cl, Ffile, &r), 4);
	nulldir(&dir);
	dir.length = 8192;
	clwstat(&cl, Ffile, &dir, &r);
	checks++;
	if(r.type != Rwstat)
		fail("a wstat that extends: %s", clerr(&r));
	eqv("... and the object is that long", statlen(&cl, Ffile, &r), 8192);
	if(clopen(&cl, Ffile, OREAD, &r) != Ropen)
		fail("open after the extend: %s", clerr(&r));
	else if(clread(&cl, Ffile, 4, 64, &r) != Rread)
		fail("read the extension: %s", clerr(&r));
	else{
		memset(data, 0, 64);
		eqv("an extend leaves bytes below the new len", r.count, 64);
		istrue("... and they read as the zeroes of a hole",
			memcmp(r.data, data, 64) == 0);
	}
	nulldir(&dir);
	dir.length = Tobjmax + 1;
	clwstat(&cl, Ffile, &dir, &r);
	clerris("a wstat past objmax", &r, "object too large");
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §2.4's Tcreate in /obj, §1.5's tombstone rule for the version it
 * chooses, §1.1's reserved ids, and §2.3's qid.path across delete and
 * re-create — asserted here through I/O on the fid the create moved,
 * which is the half srvtest's walk cases cannot reach.
 */
static void
tcreate(void)
{
	char buf[64], *m;
	Objinfo oi;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	uvlong path, tombver;

	clstage = "create";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalk1(&cl, Froot, Fdir, "obj", &r) != Rwalk){
		fail("walk /obj: %s", clerr(&r));
		goto Out;
	}
	/* the mode and perm rules of §2.4 */
	clcreate(&cl, Fdir, "dir", DMDIR|0777, OREAD, &r);
	clerris("a create with DMDIR", &r, "bad create mode");
	clcreate(&cl, Fdir, "app", DMAPPEND|0666, OWRITE, &r);
	clerris("a create with DMAPPEND", &r, "bad create mode");
	clcreate(&cl, Fdir, "exc", DMEXCL|0666, OWRITE, &r);
	clerris("a create with DMEXCL", &r, "bad create mode");
	clcreate(&cl, Fdir, "tmp", DMTMP|0666, OWRITE, &r);
	clerris("a create with DMTMP", &r, "bad create mode");
	clcreate(&cl, Fdir, "rc", 0666, OWRITE|ORCLOSE, &r);
	clerris("a create with ORCLOSE", &r, "bad open mode");
	clcreate(&cl, Fdir, "not!a!name", 0666, OWRITE, &r);
	clerris("a create of an id §1.1 forbids", &r, "bad object name");
	clcreate(&cl, Fdir, "shoal.map.8", 0666, OWRITE, &r);
	clerris("a client create of a reserved id", &r, "reserved name");
	clcreate(&cl, Fdir, "alpha", 0666, OWRITE, &r);
	clerris("a create of a live id", &r, "object exists");

	/* the create that works, and the fid it moved */
	clcreate(&cl, Fdir, "brandnew", 0666, ORDWR, &r);
	checks++;
	if(r.type != Rcreate){
		fail("a client create: %s", clerr(&r));
		goto Out;
	}
	if(statof(srvstore(ctx), "brandnew", &oi) < 0)
		fail("objstat brandnew: %r");
	else{
		eqv("a create answers the object's own qid.path", r.qid.path,
			oi.qidpath);
		eqv("a created object starts at ver 1", oi.ver, 1);
		eqv("... at the epoch the instance adopted", oi.wepoch, Tepoch);
		eqv("... and holds no content", oi.len, 0);
	}
	path = oi.qidpath;
	if(clwriteb(&cl, Fdir, 0, "new", 3, &r) != Rwrite)
		fail("write through the fid a create moved: %s", clerr(&r));
	else
		eqv("the fid a create moved is the object's", r.count, 3);
	clclunk(&cl, Fdir, &r);

	/*
	 * §1.5: a create over a tombstone takes the tombstone's version
	 * plus one, and §2.3 keeps the qid.path across the delete.
	 */
	if(clwalkobj(&cl, Froot, Ffile, "obj", "brandnew", &r) != Rwalk)
		fail("walk /obj/brandnew: %s", clerr(&r));
	clremove(&cl, Ffile, &r);
	checks++;
	if(r.type != Rremove)
		fail("remove /obj/brandnew: %s", clerr(&r));
	if(statof(srvstore(ctx), "brandnew", &oi) < 0)
		fail("objstat the tombstone: %r");
	tombver = oi.ver;
	eqv("a remove leaves a tombstone", oi.state, Stomb);
	if(clwalk1(&cl, Froot, Fdir, "obj", &r) != Rwalk)
		fail("walk /obj: %s", clerr(&r));
	clcreate(&cl, Fdir, "brandnew", 0666, OWRITE, &r);
	checks++;
	if(r.type != Rcreate)
		fail("a create over a tombstone: %s", clerr(&r));
	else if(statof(srvstore(ctx), "brandnew", &oi) < 0)
		fail("objstat the re-created object: %r");
	else{
		eqv("a create over a tombstone takes its ver plus one",
			oi.ver, tombver+1);
		eqv("... and keeps the qid.path (§2.3)", oi.qidpath, path);
		eqv("... which is the qid the create answered", r.qid.path,
			path);
	}
	/*
	 * The new incarnation serves I/O like any other object, and its
	 * qid.vers is §2.3's low 32 bits of the version the create chose —
	 * which is what tells a client holding the old one that the object
	 * it is looking at is not the object it had.
	 */
	if(clwriteb(&cl, Fdir, 0, "again", 5, &r) != Rwrite)
		fail("write to the re-created object: %s", clerr(&r));
	else
		eqv("the re-created object takes bytes", r.count, 5);
	clclunk(&cl, Fdir, &r);
	if(statof(srvstore(ctx), "brandnew", &oi) < 0)
		fail("objstat the re-written object: %r");
	else if(clwalkobj(&cl, Froot, Ffile, "obj", "brandnew", &r) != Rwalk
	|| clopen(&cl, Ffile, OREAD, &r) != Ropen)
		fail("open the re-created object: %s", clerr(&r));
	else{
		eqv("an open answers qid.vers, the low 32 of ver (§2.3)",
			r.qid.vers, (ulong)(oi.ver & 0xFFFFFFFFULL));
		eqv("... over the qid.path the id has kept", r.qid.path, path);
		if(clread(&cl, Ffile, 0, 16, &r) != Rread)
			fail("read the re-created object: %s", clerr(&r));
		else{
			memmove(buf, r.data, r.count);
			buf[r.count] = 0;
			eqs("... and it reads back what was written to it", buf,
				"again");
		}
	}
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot, &r);

	/* §2.1: the operator may create a reserved id, and a client may not */
	if(clattach(&cl, Froot2, Nadmin, &r) != Rattach)
		fail("attach admin: %s", clerr(&r));
	else if(clwalk1(&cl, Froot2, Fdir, "obj", &r) != Rwalk)
		fail("walk /obj as admin: %s", clerr(&r));
	else{
		clcreate(&cl, Fdir, "shoal.map.8", 0666, OWRITE, &r);
		checks++;
		if(r.type != Rcreate)
			fail("an admin create of a reserved id: %s", clerr(&r));
		clclunk(&cl, Fdir, &r);
	}
	clclunk(&cl, Froot2, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §2.4's remove: §1.5's delete, and 9P's rule that the fid is clunked
 * whether or not the remove succeeded.  A tombstone is `object
 * deleted' to everything but a create.
 */
static void
tremove(void)
{
	char *m;
	Objinfo oi;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;

	clstage = "remove";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", "bytes", 5);
	mkobj(srvstore(ctx), "beta", nil, 0);
	mkobj(srvstore(ctx), "gamma", nil, 0);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	/*
	 * A tombstone reached through a fid that was walked while the
	 * object was live: the walk cannot refuse what was not a
	 * tombstone when it ran, so the open is where §2.6's
	 * `object deleted' is answered — by this server, since 9P's open
	 * reaches no engine call that would refuse it.
	 */
	if(clwalkobj(&cl, Froot, Ffile2, "obj", "gamma", &r) != Rwalk)
		fail("walk /obj/gamma: %s", clerr(&r));
	if(objremove(srvstore(ctx), (uchar*)"gamma", 5, 2, Tepoch, nil, 0) < 0)
		fail("objremove gamma: %r");
	clopen(&cl, Ffile2, OREAD, &r);
	clerris("an open of an object deleted under the fid", &r,
		"object deleted");
	clclunk(&cl, Ffile2, &r);

	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk){
		fail("walk /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	clremove(&cl, Ffile, &r);
	checks++;
	if(r.type != Rremove)
		fail("remove /obj/alpha: %s", clerr(&r));
	if(statof(srvstore(ctx), "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	else{
		eqv("a remove makes the record a tombstone", oi.state, Stomb);
		eqv("... with len 0 (§1.5)", oi.len, 0);
		eqv("... at a bumped version", oi.ver, 3);
	}
	/* the fid is gone: 9P clunks it, and the server must not hold it */
	clstat(&cl, Ffile, &r);
	clerris("a fid a remove clunked", &r, "unknown fid");
	/*
	 * ... and the tombstone is `object deleted' to every other path.
	 * The walk is one element from an /obj fid, which is where §2.6's
	 * strings reach a client (store.md §14(28)).
	 */
	if(clwalk1(&cl, Froot, Fdir, "obj", &r) != Rwalk)
		fail("walk /obj: %s", clerr(&r));
	clwalk1(&cl, Fdir, Ffile, "alpha", &r);
	clerris("a walk to the tombstone", &r, "object deleted");
	clclunk(&cl, Fdir, &r);

	/*
	 * A remove that FAILS clunks the fid too.  The fid is walked to a
	 * live object and the object is deleted under it through the
	 * engine, so the remove finds a tombstone.
	 */
	if(clwalkobj(&cl, Froot, Ffile, "obj", "beta", &r) != Rwalk)
		fail("walk /obj/beta: %s", clerr(&r));
	if(objremove(srvstore(ctx), (uchar*)"beta", 4, 2, Tepoch, nil, 0) < 0)
		fail("objremove beta: %r");
	clremove(&cl, Ffile, &r);
	clerris("a remove of a tombstone", &r, "object deleted");
	clstat(&cl, Ffile, &r);
	clerris("a fid a failed remove clunked", &r, "unknown fid");
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §2.4's /meta/<oid>.  It renders at open like every other status file
 * (§2.2), on the object's queue because it reads the object's record;
 * `blksz=' is the geometry's and `cur=' is 0, which is what an
 * instance that can complete no currency check has (store.md §14(31)).
 */
static void
tmeta(void)
{
	char buf[1024], val[128], val2[128], *m;
	Objinfo oi;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	int n;

	clstage = "meta";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", "0123456789", 10);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Fmeta, "meta", "alpha", &r) != Rwalk
	|| clopen(&cl, Fmeta, OREAD, &r) != Ropen){
		fail("open /meta/alpha: %s", clerr(&r));
		goto Out;
	}
	if(clslurp(&cl, Fmeta, buf, sizeof buf) < 0)
		fail("read /meta/alpha: %r");
	else if(statof(srvstore(ctx), "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	else{
		eqs("/meta names the object", metafield(buf, "oid", val,
			sizeof val), "alpha");
		snprint(val, sizeof val, "%llud", oi.len);
		eqs("/meta reports len", metafield(buf, "len", val2,
			sizeof val2), val);
		snprint(val, sizeof val, "%llud", oi.ver);
		eqs("/meta reports ver", metafield(buf, "ver", val2,
			sizeof val2), val);
		snprint(val, sizeof val, "%llud", oi.wepoch);
		eqs("/meta reports wepoch", metafield(buf, "wepoch", val2,
			sizeof val2), val);
		csumfmt(val, oi.csum);
		eqs("/meta reports the object's csum", metafield(buf, "csum",
			val2, sizeof val2), val);
		eqs("/meta reports the state", metafield(buf, "state", val2,
			sizeof val2), "live");
		snprint(val, sizeof val, "%d", Tblksz);
		eqs("/meta reports the geometry's blksz",
			metafield(buf, "blksz", val2, sizeof val2), val);
		eqs("/meta reports cur=0, no currency check being possible",
			metafield(buf, "cur", val2, sizeof val2), "0");
		eqs("/meta reports the placement", metafield(buf, "placement",
			val2, sizeof val2), "n1.0");
		eqs("/meta reports the serving primary", metafield(buf,
			"primary", val2, sizeof val2), "n1.0");
		eqs("... and that the instance is not ready to serve it",
			metafield(buf, "ready", val2, sizeof val2), "no");
		eqs("/meta is one physical line", strchr(buf, '\n') ==
			buf + strlen(buf) - 1 ? "one" : "more", "one");
	}
	clclunk(&cl, Fmeta, &r);

	/* the render is a snapshot: a write after the open does not tear it */
	if(clwalkobj(&cl, Froot, Fmeta, "meta", "alpha", &r) != Rwalk
	|| clopen(&cl, Fmeta, OREAD, &r) != Ropen)
		fail("re-open /meta/alpha: %s", clerr(&r));
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, OWRITE, &r) != Ropen)
		fail("open /obj/alpha: %s", clerr(&r));
	if(clwriteb(&cl, Ffile, 0, "zzzz", 4, &r) != Rwrite)
		fail("write /obj/alpha: %s", clerr(&r));
	/*
	 * The return is checked because buf still holds the first read: a
	 * re-read that answered nothing would leave the snapshot check
	 * comparing the old bytes with themselves and passing.
	 */
	memset(buf, 0, sizeof buf);
	n = clslurp(&cl, Fmeta, buf, sizeof buf);
	checks++;
	if(n <= 0)
		fail("re-read /meta/alpha: %d", n);
	else if(statof(srvstore(ctx), "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	else{
		snprint(val, sizeof val, "%llud", oi.ver);
		istrue("a read of /meta is served from the snapshot its open"
			" took", metafield(buf, "ver", val2, sizeof val2) != nil
			&& strcmp(val2, val) != 0);
	}
	clclunk(&cl, Fmeta, &r);
	clclunk(&cl, Ffile, &r);

	/*
	 * §2.4 admits OREAD on /meta and nothing else.  A mode in the write
	 * column never reaches the row's cell — the matrix has no write
	 * grant for /meta at all (store.md §14(24)) — so OEXEC is the mode
	 * the cell itself refuses.
	 */
	if(clwalkobj(&cl, Froot, Ffile2, "meta", "alpha", &r) != Rwalk)
		fail("walk /meta/alpha: %s", clerr(&r));
	clopen(&cl, Ffile2, OEXEC, &r);
	clerris("an open of /meta for execution", &r, "bad open mode");

	/*
	 * §2.6: a tombstone is `object deleted', through /meta as well — at
	 * the walk, and, on a fid walked while the object was still live, at
	 * the open, which is where the render reads the record.
	 */
	if(objremove(srvstore(ctx), (uchar*)"alpha", 5, oi.ver+1, Tepoch,
		nil, 0) < 0)
		fail("objremove alpha: %r");
	clopen(&cl, Ffile2, OREAD, &r);
	clerris("an open of /meta for an object deleted under the fid", &r,
		"object deleted");
	clclunk(&cl, Ffile2, &r);
	if(clwalk1(&cl, Froot, Fdir, "meta", &r) != Rwalk)
		fail("walk /meta: %s", clerr(&r));
	clwalk1(&cl, Fdir, Fmeta, "alpha", &r);
	clerris("a walk to a deleted object through /meta", &r,
		"object deleted");
	clclunk(&cl, Fdir, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §2.4's `placement=' and `primary=' when the map has nothing to name.
 * This instance is status=out and the other is dead, so no node places
 * at all: P(o) is empty and the object has no serving primary.  Both
 * fields render `-' rather than an attribute with no value, which §0's
 * one attr=value record per line does not admit (store.md §14(31)).
 * The operator's read is what reaches the render: role=client I/O on
 * an instance whose own record says status=out is F3's `down' (§6.4).
 */
static void
tmetaplace(void)
{
	char buf[1024], val[128], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;

	clstage = "metaplace";
	m = mkmap(Pout, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", "0123456789", 10);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Fmeta, "meta", "alpha", &r) != Rwalk)
		fail("walk /meta/alpha: %s", clerr(&r));
	clopen(&cl, Fmeta, OREAD, &r);
	clerris("a client open of /meta on an instance that is out", &r,
		"down");
	clclunk(&cl, Fmeta, &r);
	clclunk(&cl, Froot, &r);

	if(clattach(&cl, Froot2, Nadmin, &r) != Rattach){
		fail("attach admin: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot2, Fmeta, "meta", "alpha", &r) != Rwalk
	|| clopen(&cl, Fmeta, OREAD, &r) != Ropen){
		fail("open /meta/alpha as admin: %s", clerr(&r));
		goto Out;
	}
	if(clslurp(&cl, Fmeta, buf, sizeof buf) < 0)
		fail("read /meta/alpha: %r");
	else{
		eqs("/meta names an empty placement `-'", metafield(buf,
			"placement", val, sizeof val), "-");
		eqs("... and an absent primary `-'", metafield(buf, "primary",
			val, sizeof val), "-");
	}
	clclunk(&cl, Fmeta, &r);
	clclunk(&cl, Froot2, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §5.4 steps 4 and 5 on a build with no peer client (store.md
 * §14(30)).  The map places every object on this instance and on one
 * that is up=no: nothing was sent to it, so M is not empty, step 5a
 * owes a durable stale mark to a monitor this build has no client for,
 * and every client mutation answers `degraded'.  A read is not on that
 * path and is served; an operator's write of a reserved id is not
 * either (§2.1).
 */
static void
tdegraded(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	Dir dir;

	clstage = "degraded";
	m = mkmap(Pdown, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", "bytes", 5);
	mkobj(srvstore(ctx), "shoal.map.7", nil, 0);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, ORDWR, &r) != Ropen){
		fail("open /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	if(clread(&cl, Ffile, 0, 5, &r) != Rread)
		fail("a read on the degraded path: %s", clerr(&r));
	else
		eqv("a read is served while a placement member is down",
			r.count, 5);
	clwriteb(&cl, Ffile, 0, "x", 1, &r);
	clerris("a client write with a placement member that took nothing",
		&r, "degraded");
	nulldir(&dir);
	dir.length = 1;
	clwstat(&cl, Ffile, &dir, &r);
	clerris("a client truncate on the same placement", &r, "degraded");
	clclunk(&cl, Ffile, &r);
	if(clwalk1(&cl, Froot, Fdir, "obj", &r) != Rwalk)
		fail("walk /obj: %s", clerr(&r));
	clcreate(&cl, Fdir, "brandnew", 0666, OWRITE, &r);
	clerris("a client create on the same placement", &r, "degraded");
	clclunk(&cl, Fdir, &r);
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk)
		fail("walk /obj/alpha: %s", clerr(&r));
	clremove(&cl, Ffile, &r);
	clerris("a client remove on the same placement", &r, "degraded");
	clclunk(&cl, Froot, &r);

	/* §2.1's operator write of a reserved id is not §5.4's client path */
	if(clattach(&cl, Froot2, Nadmin, &r) != Rattach)
		fail("attach admin: %s", clerr(&r));
	else if(clwalkobj(&cl, Froot2, Ffile2, "obj", "shoal.map.7", &r)
		!= Rwalk || clopen(&cl, Ffile2, OWRITE, &r) != Ropen)
		fail("open /obj/shoal.map.7: %s", clerr(&r));
	else{
		clwriteb(&cl, Ffile2, 0, "map", 3, &r);
		checks++;
		if(r.type != Rwrite)
			fail("an operator write of a reserved id: %s",
				clerr(&r));
		clclunk(&cl, Ffile2, &r);
	}
	clclunk(&cl, Froot2, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §5.1 and §5.4 step 1: a role=client operation is served only by the
 * object's serving primary, and every other instance answers
 * `not primary' with the iid in §2.6's detail — through /meta as well
 * as through /obj, both being reads of the object (§6.4 F1).  The map
 * here places with two instances up, so HRW sends some ids elsewhere;
 * the case finds one and one that stays here, so that both answers are
 * asserted against the same map.
 */
static void
tnotprimary(void)
{
	char name[32], want[64], *m, *mine, *theirs;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	Cinst *p;
	int i;

	clstage = "notprimary";
	m = mkmap(Pup, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mine = theirs = nil;
	for(i = 0; i < 64 && (mine == nil || theirs == nil); i++){
		snprint(name, sizeof name, "obj%d", i);
		p = mapprimary(srvmap(ctx), name);
		if(p == nil)
			continue;
		if(strcmp(p->iid, srviid(ctx)) == 0){
			if(mine == nil)
				mine = strdup(name);
		}else if(theirs == nil){
			theirs = strdup(name);
			snprint(want, sizeof want, "not primary: %s", p->iid);
		}
	}
	if(mine == nil || theirs == nil){
		fail("the map places every id the same way: no case to drive");
		goto Out0;
	}
	mkobj(srvstore(ctx), mine, "bytes", 5);
	mkobj(srvstore(ctx), theirs, "bytes", 5);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", theirs, &r) != Rwalk){
		fail("walk /obj/%s: %s", theirs, clerr(&r));
		goto Out;
	}
	clopen(&cl, Ffile, OREAD, &r);
	clerris("a client open of an object placed elsewhere", &r, want);
	clclunk(&cl, Ffile, &r);
	/* the same instance serves the ids it IS the primary for */
	if(clwalkobj(&cl, Froot, Ffile, "obj", mine, &r) != Rwalk
	|| clopen(&cl, Ffile, ORDWR, &r) != Ropen)
		fail("open /obj/%s: %s", mine, clerr(&r));
	else if(clread(&cl, Ffile, 0, 5, &r) != Rread)
		fail("read /obj/%s: %s", mine, clerr(&r));
	else
		eqv("... and serves the ids it is the primary for", r.count, 5);
	clclunk(&cl, Ffile, &r);
	/*
	 * /meta/<oid> is the same object under a second name, and §5.1 is
	 * asked of a client read of it too: §2.1 exempts the operator's
	 * read alone, and §6.4 F1 names /obj and /meta in one breath.
	 */
	if(clwalkobj(&cl, Froot, Fmeta, "meta", theirs, &r) != Rwalk)
		fail("walk /meta/%s: %s", theirs, clerr(&r));
	clopen(&cl, Fmeta, OREAD, &r);
	clerris("a client open of /meta for an object placed elsewhere", &r,
		want);
	clclunk(&cl, Fmeta, &r);
	if(clwalkobj(&cl, Froot, Fmeta, "meta", mine, &r) != Rwalk
	|| clopen(&cl, Fmeta, OREAD, &r) != Ropen)
		fail("open /meta/%s: %s", mine, clerr(&r));
	clclunk(&cl, Fmeta, &r);
	/* an operator is not a client: §5.1's rule is not asked of it */
	clclunk(&cl, Froot, &r);
	if(clattach(&cl, Froot2, Nadmin, &r) != Rattach)
		fail("attach admin: %s", clerr(&r));
	else if(clwalkobj(&cl, Froot2, Ffile2, "obj", theirs, &r) != Rwalk)
		fail("walk as admin: %s", clerr(&r));
	else{
		clopen(&cl, Ffile2, OREAD, &r);
		checks++;
		if(r.type != Ropen)
			fail("an operator read of an object placed elsewhere:"
				" %s", clerr(&r));
		clclunk(&cl, Ffile2, &r);
		if(clwalkobj(&cl, Froot2, Fmeta, "meta", theirs, &r) != Rwalk)
			fail("walk /meta as admin: %s", clerr(&r));
		clopen(&cl, Fmeta, OREAD, &r);
		checks++;
		if(r.type != Ropen)
			fail("an operator read of /meta for an object placed"
				" elsewhere: %s", clerr(&r));
		clclunk(&cl, Fmeta, &r);
	}
	clclunk(&cl, Froot2, &r);
Out:
	clstop(&cl);
Out0:
	free(mine);
	free(theirs);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §2.4's short write.  What bounds one accepted write is the per-fid
 * staged-grain budget (store.md §3.6's stagemax), so a write covering
 * more checksum blocks than the budget is answered with the count that
 * fits and the client loops.  stagemax is 1 here, which makes the
 * budget one block.
 */
static void
tshort(void)
{
	char *m;
	uchar data[Tblksz+512];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	int i;

	clstage = "short";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 1, 0)) == nil)
		return;
	for(i = 0; i < sizeof data; i++)
		data[i] = 0x33 ^ (uchar)i;
	mkobj(srvstore(ctx), "alpha", nil, 0);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, ORDWR, &r) != Ropen){
		fail("open /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	/* a write of two blocks' worth, from the start of a block */
	if(clwriteb(&cl, Ffile, 0, data, 6000, &r) != Rwrite)
		fail("a write across the budget: %s", clerr(&r));
	else
		eqv("a write past the per-fid stage budget is answered short",
			r.count, Tblksz);
	/* ... and from inside one: what fits is the rest of that block */
	if(clwriteb(&cl, Ffile, 1000, data, 6000, &r) != Rwrite)
		fail("an unaligned write across the budget: %s", clerr(&r));
	else
		eqv("an unaligned short write ends at the block boundary",
			r.count, Tblksz-1000);
	/* a write within the budget is taken whole */
	if(clwriteb(&cl, Ffile, 0, data, 512, &r) != Rwrite)
		fail("a write within the budget: %s", clerr(&r));
	else
		eqv("a write within the budget is taken whole", r.count, 512);
	if(clread(&cl, Ffile, 0, 512, &r) != Rread)
		fail("read back: %s", clerr(&r));
	else
		istrue("and what a short write took is what landed",
			memcmp(r.data, data, 512) == 0);
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * `disk full', §2.6 and store.md §3.7: the engine's, when no index
 * slot is left, and D20's detail form, which goes to the wire verbatim
 * because §2.6 permits detail after the prefix (§3.7).  The store is
 * formatted with a handful of slots, so a client create runs out of
 * them.
 */
static void
tdiskfull(void)
{
	char name[32], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	int i, full;

	clstage = "diskfull";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(8);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	full = 0;
	for(i = 0; i < 64 && !full; i++){
		if(clwalk1(&cl, Froot, Fdir, "obj", &r) != Rwalk){
			fail("walk /obj: %s", clerr(&r));
			goto Out;
		}
		snprint(name, sizeof name, "obj%d", i);
		clcreate(&cl, Fdir, name, 0666, OWRITE, &r);
		if(r.type == Rerror){
			eqs("a create with no index slot left", clerr(&r),
				"disk full");
			full = 1;
		}
		clclunk(&cl, Fdir, &r);
	}
	istrue("the index runs out before 64 creates do", full);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The per-fid stage (dat.h's Sstage): one to a fid, discarded at
 * clunk, by step 7 on a flush, by the idle sweep, and by the
 * shutdown's sweep before the store closes.  The stage point is what
 * puts one on a fid with no request in flight (srv.h), which is the
 * shape §5.5's op=full stage has and which no client operation
 * produces: a client operation gives its own stage back inside its
 * request.
 */
static void
tstage(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	uvlong live, done, openat;

	clstage = "stage";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0);
	mkobj(srvstore(ctx), "beta", nil, 0);
	srvstagepoint(ctx, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, OWRITE, &r) != Ropen){
		fail("open /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	srvstagecount(ctx, &live, &done, nil);
	eqv("a fid that is staging holds one stage", live, 1);
	/* §3.6's per-fid bound: a second stage on one fid is `disk full' */
	clwriteb(&cl, Ffile, 0, "x", 1, &r);
	clerris("a second stage on one fid", &r, "disk full");
	/*
	 * §2.4 makes Tclunk a MUST succeed "whatever the instance's epoch,
	 * fence or role state", and the fid it is asked of here holds a
	 * stage while the instance is fenced — the state that could make a
	 * server want to answer something else.  lib9p answers a Tclunk
	 * itself and offers no cell to answer it with, so what these three
	 * checks hold is that it stays that way: a row that came to gate a
	 * clunk, or a give-back that could fail one, would be seen here.
	 */
	if(clattach(&cl, Froot2, Nadmin, &r) != Rattach)
		fail("attach admin: %s", clerr(&r));
	else if(clwalk1(&cl, Froot2, Fctl, "ctl", &r) != Rwalk
	|| clopen(&cl, Fctl, OWRITE, &r) != Ropen)
		fail("open /ctl: %s", clerr(&r));
	else if(clwrite(&cl, Fctl, 0, "fence on", &r) != Rwrite)
		fail("fence on: %s", clerr(&r));
	clclunk(&cl, Ffile, &r);
	checks++;
	if(r.type != Rclunk)
		fail("the clunk of a staging fid on a fenced instance: %s",
			clerr(&r));
	srvstagecount(ctx, &live, &done, &openat);
	eqv("a clunk discards the fid's stage", live, 0);
	eqv("... exactly once", done, 1);
	eqv("... with the store still open", openat, 1);
	clclunk(&cl, Fctl, &r);
	checks++;
	if(r.type != Rclunk)
		fail("the clunk of /ctl on a fenced instance: %s", clerr(&r));
	clclunk(&cl, Froot2, &r);
	clclunk(&cl, Froot, &r);
	checks++;
	if(r.type != Rclunk)
		fail("the clunk of the attach's own fid: %s", clerr(&r));
Out:
	srvstagepoint(ctx, 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The idle sweep, §3.6: a stage no chunk has arrived for in `stagems'
 * is stripped, and the handle stays the fid's until its clunk.  The
 * sweep runs at the head of every queued object operation, so an
 * operation on another object is what drives it here.
 */
static void
tstagesweep(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	uvlong live, done;

	clstage = "stagesweep";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 50)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0);
	mkobj(srvstore(ctx), "beta", nil, 0);
	srvstagepoint(ctx, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, OWRITE, &r) != Ropen){
		fail("open /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	srvstagepoint(ctx, 0);
	srvstagecount(ctx, &live, nil, nil);
	eqv("the staging fid holds its stage", live, 1);
	sleep(200);			/* longer than stagems */
	/* an operation on another object, which is where the sweep runs */
	if(clwalkobj(&cl, Froot, Ffile2, "obj", "beta", &r) != Rwalk
	|| clopen(&cl, Ffile2, OREAD, &r) != Ropen)
		fail("open /obj/beta: %s", clerr(&r));
	srvstagecount(ctx, &live, &done, nil);
	eqv("the idle sweep strips a stage nothing has arrived for", live, 0);
	eqv("... and gives back what it held", done, 1);
	/* the fid lives on, and stages again */
	if(clwriteb(&cl, Ffile, 0, "after", 5, &r) != Rwrite)
		fail("a write after the sweep: %s", clerr(&r));
	else
		eqv("a fid whose stage was swept stages again", r.count, 5);
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Ffile2, &r);
	clclunk(&cl, Froot, &r);
Out:
	srvstagepoint(ctx, 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The sweep runs at the head of every queued operation that NAMES an
 * object, which is more than this file's own handlers: a walk to
 * /obj/<oid> and a stat of one are queued against that object too
 * (queue.c), so each of them is a place an abandoned stage is
 * collected.
 */
static void
tsweepsites(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	uvlong live;

	clstage = "sweepsites";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 50)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0);
	mkobj(srvstore(ctx), "beta", nil, 0);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	/* a Tstat of an object is where this stage is collected */
	srvstagepoint(ctx, 1);
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, OWRITE, &r) != Ropen){
		fail("open /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	srvstagepoint(ctx, 0);
	if(clwalkobj(&cl, Froot, Ffile2, "obj", "beta", &r) != Rwalk){
		fail("walk /obj/beta: %s", clerr(&r));
		goto Out;
	}
	sleep(200);			/* longer than stagems */
	if(clstat(&cl, Ffile2, &r) != Rstat)
		fail("stat /obj/beta: %s", clerr(&r));
	srvstagecount(ctx, &live, nil, nil);
	eqv("a stat of an object sweeps at its head", live, 0);
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Ffile2, &r);

	/* ... and so is a walk to one */
	srvstagepoint(ctx, 1);
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, OWRITE, &r) != Ropen)
		fail("re-open /obj/alpha: %s", clerr(&r));
	srvstagepoint(ctx, 0);
	sleep(200);
	if(clwalkobj(&cl, Froot, Ffile2, "obj", "beta", &r) != Rwalk)
		fail("re-walk /obj/beta: %s", clerr(&r));
	srvstagecount(ctx, &live, nil, nil);
	eqv("a walk to an object sweeps at its head", live, 0);
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Ffile2, &r);
	clclunk(&cl, Froot, &r);
Out:
	srvstagepoint(ctx, 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The sweep against the clunk of the fid it is sweeping.  The sweep
 * runs on a queue proc and holds no reference to a stage, while the
 * clunk runs on the service loop and frees whatever its fid holds, so
 * the two meet on one Sstage: the sweep must be done with a stage
 * before it lets go of the lock the clunk takes.  Both are driven
 * here at once — the clunk is sent while the open that drives the
 * sweep is still in flight — and what is asserted is that the stage
 * is given back exactly once and the server serves on.
 */
static void
tsweepclunk(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	uvlong live, done;
	ushort ot, ct;

	clstage = "sweepclunk";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 50)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0);
	mkobj(srvstore(ctx), "beta", nil, 0);
	srvstagepoint(ctx, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, OWRITE, &r) != Ropen){
		fail("open /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	srvstagepoint(ctx, 0);
	if(clwalkobj(&cl, Froot, Ffile2, "obj", "beta", &r) != Rwalk){
		fail("walk /obj/beta: %s", clerr(&r));
		goto Out;
	}
	sleep(200);			/* the stage is idle past stagems */
	memset(&t, 0, sizeof t);
	t.type = Topen;
	t.tag = ot = cltag(&cl);
	t.fid = Ffile2;
	t.mode = OREAD;
	clput(&cl, &t);			/* the sweep runs at the head of this */
	sleep(100);			/* ... and the clunk lands behind it */
	memset(&t, 0, sizeof t);
	t.type = Tclunk;
	t.tag = ct = cltag(&cl);
	t.fid = Ffile;
	clput(&cl, &t);			/* and this frees the stage it holds */
	if(clgettag(&cl, ct, &r) < 0 || r.type != Rclunk)
		fail("the clunk of the swept fid: %s", clerr(&r));
	cltagfree(&cl, ct);
	if(clgettag(&cl, ot, &r) < 0 || r.type != Ropen)
		fail("the open that drove the sweep: %s", clerr(&r));
	cltagfree(&cl, ot);
	srvstagecount(ctx, &live, &done, nil);
	eqv("the sweep and the clunk leave no stage behind", live, 0);
	eqv("... and give it back exactly once", done, 1);
	/* the server is still serving: nothing of the list was torn */
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, OWRITE, &r) != Ropen)
		fail("open /obj/alpha after the sweep: %s", clerr(&r));
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Ffile2, &r);
	clclunk(&cl, Froot, &r);
Out:
	srvstagepoint(ctx, 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The window between the stage point's stage and the engine handle it
 * arms that stage with.  The handle is taken with no lock held, because
 * the call blocks, and a step 7 for another request on this same fid
 * strips the stage while it is in flight — finding `g' still nil, so it
 * releases nothing.  The arm therefore has to FIND the stage gone: a
 * handle stored into a stripped stage is one every later strip returns
 * early past, so it goes to the drain instead, exactly as the flush
 * hook's does.  The objarm point is what parks the open in that window
 * (srv.h), and srvstagepend is what says the handle reached the drain.
 */
static void
tstagearm(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	uvlong live, done, openat, np0, nd0, np, nd;
	ushort ta, tb, tf;
	int i;

	clstage = "stagearm";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk){
		fail("walk /obj/alpha: %s", clerr(&r));
		goto Out;
	}

	/* the open stages, then parks short of the arm */
	waitidle(ctx, &np0, &nd0);
	srvstagepoint(ctx, 1);
	srvhook(ctx, "objarm", 1);
	memset(&t, 0, sizeof t);
	t.type = Topen;
	t.tag = ta = cltag(&cl);
	t.fid = Ffile;
	t.mode = OWRITE;
	clput(&cl, &t);
	for(i = 0; i < 400; i++){
		srvstagecount(ctx, &live, nil, nil);
		if(live == 1)
			break;
		sleep(5);
	}
	eqv("the parked open holds the fid's stage", live, 1);

	/* a stat of the same object on the same fid, queued behind it */
	memset(&t, 0, sizeof t);
	t.type = Tstat;
	t.tag = tb = cltag(&cl);
	t.fid = Ffile;
	clput(&cl, &t);
	waitpush(ctx, np0, 2, &np, &nd);
	eqv("the stat is queued behind the parked open", np - np0, 2);

	/* flushed where it waits, so step 7 strips the stage on the loop */
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = tb;
	clput(&cl, &t);
	if(clgettag(&cl, tb, &r) < 0)
		fail("no answer for the flushed stat");
	else
		eqs("the flushed queued stat", clerr(&r), "interrupted");
	cltagfree(&cl, tb);
	if(clgettag(&cl, tf, &r) < 0)
		fail("no Rflush");
	else{
		checks++;
		if(r.type != Rflush)
			fail("the Rflush after it: %s", clerr(&r));
	}
	cltagfree(&cl, tf);
	srvstagecount(ctx, &live, &done, &openat);
	eqv("step 7 strips the stage the open is arming", live, 0);
	eqv("... and gives back what it held, which is nothing yet", done, 1);
	eqv("... owing one discard the store is still open for", openat, 1);
	eqv("and the strip found no handle to park", srvstagepend(ctx), 0);

	/* the arm now meets a stage that is gone, and the handle is real */
	srvhook(ctx, "objarm", 0);
	if(clgettag(&cl, ta, &r) < 0)
		fail("no answer for the parked open");
	else{
		checks++;
		if(r.type != Ropen)
			fail("open /obj/alpha: %s", clerr(&r));
	}
	cltagfree(&cl, ta);
	srvstagepoint(ctx, 0);
	eqv("the handle armed onto a stripped stage goes to the drain",
		srvstagepend(ctx), 1);
	srvstagecount(ctx, &live, &done, &openat);
	eqv("... and no second stage is left behind", live, 0);
	eqv("... nor a second discard owed", openat, 1);

	/* the drain at the head of the next queued operation makes the call */
	if(clstat(&cl, Ffile, &r) != Rstat)
		fail("stat /obj/alpha after the arm: %s", clerr(&r));
	eqv("... and parks nothing behind it", srvstagepend(ctx), 1);
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot, &r);
Out:
	srvhook(ctx, "objarm", 0);
	srvstagepoint(ctx, 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * layer-a §5.4.1 step 7's discard half: a Tflush discards the STAGE
 * THE FID HOLDS, whichever of the fid's requests was flushed.  Two
 * requests are outstanding on one fid — one running and held at its
 * check point, one still queued — and the queued one is flushed, so
 * step 7 runs on the service loop while a queue proc is inside a
 * handler on the same fid.  That is the case the fid's state lock
 * exists for (dat.h), and srvauxbusy is what says the hook never ran
 * inside a step rather than between two.
 */
static void
tstageflush(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	uvlong live, done, np0, nd0, np, nd;
	ushort ta, tb, tf;

	clstage = "stageflush";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0);
	srvstagepoint(ctx, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, OWRITE, &r) != Ropen){
		fail("open /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	srvstagepoint(ctx, 0);
	srvstagecount(ctx, &live, nil, nil);
	eqv("the fid holds a stage", live, 1);

	/* one request running and held, one queued behind it, on this fid */
	waitidle(ctx, &np0, &nd0);
	srvhook(ctx, "objhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Ffile;
	t.offset = 0;
	t.data = "a";
	t.count = 1;
	clput(&cl, &t);
	waitpush(ctx, np0, 1, &np, &nd);
	eqv("the first write is in the pool", np - np0, 1);
	t.tag = tb = cltag(&cl);
	clput(&cl, &t);
	waitpush(ctx, np0, 2, &np, &nd);
	eqv("the second is queued behind it", np - np0, 2);
	eqv("... and neither has answered", nd, nd0);

	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = tb;
	clput(&cl, &t);
	clget(&cl, &r);
	checks++;
	if(r.type != Rerror || r.tag != tb
	|| strcmp(r.ename, "interrupted") != 0)
		fail("the flushed queued write: type %d tag %ud %s", r.type,
			r.tag, r.type == Rerror ? r.ename : "");
	clget(&cl, &r);
	checks++;
	if(r.type != Rflush || r.tag != tf)
		fail("the Rflush after it: type %d tag %ud", r.type, r.tag);
	srvstagecount(ctx, &live, &done, nil);
	eqv("step 7 discards the stage the fid held", live, 0);
	eqv("... and gives back what it held", done, 1);
	/*
	 * ... and the engine half of the discard was PARKED rather than
	 * made: this step 7 ran on the service loop under the flushed
	 * request's own Qreq.lk, where dat.h forbids blocking on anything a
	 * queue proc needs, and an engine stage's discard takes the lock a
	 * queue proc holds across its work.
	 */
	eqv("... with the engine call it owes left to the drain",
		srvstagepend(ctx), 1);
	eqv("and no hook ran inside a handler's step", srvauxbusy(ctx), 0);

	srvhook(ctx, "objhold", 0);
	clget(&cl, &r);
	checks++;
	if(r.type != Rwrite || r.tag != ta)
		fail("the held write completes: type %d tag %ud %s", r.type,
			r.tag, r.type == Rerror ? r.ename : "");
	cltagfree(&cl, ta);
	srvstagecount(ctx, &live, &done, nil);
	eqv("the write that ran after it staged and gave its own back",
		live, 0);
	eqv("... which is the second stage given back", done, 2);
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot, &r);
Out:
	srvhook(ctx, "objhold", 0);
	srvstagepoint(ctx, 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The same hook when the park will not take the handle.  dat.h has the
 * hook make no ENGINE call — it can run on the service loop under the
 * flushed request's own Qreq.lk, and every engine call takes the state
 * lock a queue proc holds across its work — so a park that refuses
 * leaves it holding a handle it may not discard.  It puts the handle
 * back on the fid instead, dead but not released, and the clunk is what
 * makes the call.  srvstagecount is what says nobody made it in
 * between: `openat' counts the discards that found the store open,
 * which is every one that is really made.
 */
static void
tpendfull(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	uvlong live, done, openat, np0, nd0, np, nd;
	ushort ta, tb, tf;

	clstage = "pendfull";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0);
	srvstagepoint(ctx, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, OWRITE, &r) != Ropen){
		fail("open /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	srvstagepoint(ctx, 0);
	srvstagecount(ctx, &live, nil, nil);
	eqv("the fid holds a stage with an engine handle", live, 1);

	/* one stat of this object running and held, one queued behind it */
	srvstagependfull(ctx, 1);
	waitidle(ctx, &np0, &nd0);
	srvhook(ctx, "objhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Tstat;
	t.tag = ta = cltag(&cl);
	t.fid = Ffile;
	clput(&cl, &t);
	waitpush(ctx, np0, 1, &np, &nd);
	t.tag = tb = cltag(&cl);
	clput(&cl, &t);
	waitpush(ctx, np0, 2, &np, &nd);
	eqv("the second stat is queued behind the held one", np - np0, 2);

	/* flushed where it waits, so the hook runs on the service loop */
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = tb;
	clput(&cl, &t);
	if(clgettag(&cl, tb, &r) < 0)
		fail("no answer for the flushed stat");
	else
		eqs("the flushed queued stat", clerr(&r), "interrupted");
	cltagfree(&cl, tb);
	if(clgettag(&cl, tf, &r) < 0)
		fail("no Rflush");
	else{
		checks++;
		if(r.type != Rflush)
			fail("the Rflush after it: %s", clerr(&r));
	}
	cltagfree(&cl, tf);
	eqv("the park took nothing", srvstagepend(ctx), 0);
	srvstagecount(ctx, &live, &done, &openat);
	eqv("the hook takes the stage off the list all the same", live, 0);
	eqv("... gives the handle back to nobody", done, 0);
	eqv("... and makes no engine call of its own", openat, 0);

	srvhook(ctx, "objhold", 0);
	if(clgettag(&cl, ta, &r) < 0)
		fail("no answer for the held stat");
	else{
		checks++;
		if(r.type != Rstat)
			fail("stat /obj/alpha: %s", clerr(&r));
	}
	cltagfree(&cl, ta);

	/* the fid holds it still, so the clunk is what makes the call */
	clclunk(&cl, Ffile, &r);
	srvstagecount(ctx, &live, &done, &openat);
	eqv("the clunk reaches the handle the park refused", done, 1);
	eqv("... and makes the one call it owes", openat, 1);
	eqv("... leaving no stage behind", live, 0);
	clclunk(&cl, Froot, &r);
Out:
	srvhook(ctx, "objhold", 0);
	srvstagependfull(ctx, 0);
	srvstagepoint(ctx, 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The other side of step 7's discard: what it must NOT take.  A write
 * that is past §5.4 step 3 and about to commit holds the fid's stage,
 * and a Tflush of a second write QUEUED on that same fid runs step 7
 * on the service loop and discards it — the stage is the fid's, and
 * that is the rule.  The running write was never flushed, so §5.4.1's
 * licence to have applied the update or not does not cover it: it must
 * still commit the bytes its client sent.  The objstage point is what
 * holds it in exactly that window (srv.h).
 */
static void
tstagecommit(void)
{
	char buf[64], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	uvlong live, np, np2, nd;
	ushort ta, tb, tf;
	int i, n;

	clstage = "stagecommit";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", "orig!", 5);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, ORDWR, &r) != Ropen){
		fail("open /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	/* the first write stages, then waits between the stage and the commit */
	srvhook(ctx, "objstage", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Ffile;
	t.offset = 0;
	t.data = "NEW!!";
	t.count = 5;
	clput(&cl, &t);
	for(i = 0; i < 400; i++){
		srvstagecount(ctx, &live, nil, nil);
		if(live == 1)
			break;
		sleep(5);
	}
	eqv("the running write holds the fid's stage", live, 1);
	sleep(100);			/* ... and has reached the hold */

	/*
	 * The second write and the Tflush of it are read by the service
	 * loop in the order they were sent, and the loop is what queues the
	 * second one, so no wait is needed between them: by the time the
	 * flush is read the write it names is on the queue behind the
	 * request that is held.
	 */
	srvcount(ctx, &np, &nd);
	t.tag = tb = cltag(&cl);
	clput(&cl, &t);
	for(i = 0; i < 400; i++){
		srvcount(ctx, &np2, &nd);
		if(np2 > np)
			break;
		sleep(5);
	}
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = tb;
	clput(&cl, &t);
	if(clgettag(&cl, tb, &r) < 0)
		fail("no answer for the flushed write");
	else
		eqs("the flushed queued write", clerr(&r), "interrupted");
	cltagfree(&cl, tb);
	if(clgettag(&cl, tf, &r) < 0)
		fail("no Rflush");
	else{
		checks++;
		if(r.type != Rflush)
			fail("the Rflush after it: %s", clerr(&r));
	}
	cltagfree(&cl, tf);

	srvhook(ctx, "objstage", 0);
	if(clgettag(&cl, ta, &r) < 0)
		fail("no answer for the running write");
	else{
		checks++;
		if(r.type != Rwrite)
			fail("the write that was not flushed: %s", clerr(&r));
		else
			eqv("... answers the count it took", r.count, 5);
	}
	cltagfree(&cl, ta);
	n = clslurp(&cl, Ffile, buf, sizeof buf - 1);
	if(n < 0)
		n = 0;
	buf[n] = 0;
	eqs("a write step 7 did not name commits its own bytes", buf, "NEW!!");
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot, &r);
Out:
	srvhook(ctx, "objstage", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The look §5.4 step 3 owes before its commit, and the two answers
 * store.md §14(33) keeps apart.  Step 7 discards the stage the FID
 * holds, whichever of the fid's requests was flushed, so a Tflush of a
 * second write QUEUED on this fid takes the running write's stage —
 * and the objprelook point parks that write in exactly the window
 * between its stage and its look (srv.h), so the look is what it comes
 * back to.  It commits nothing and answers `staged update discarded'.
 * A Tflush of the running write's OWN tag is the other answer:
 * `interrupted', with §5.4.1's licence to have applied the update or
 * not, which is why nothing is asserted of the bytes there.
 */
static void
tstagediscard(void)
{
	char buf[64], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	uvlong live, done, np, np2, nd;
	ushort ta, tb, tf;
	int i, n;

	clstage = "stagediscard";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", "orig!", 5);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, ORDWR, &r) != Ropen){
		fail("open /obj/alpha: %s", clerr(&r));
		goto Out;
	}

	/* the write stages, then waits short of the look */
	srvhook(ctx, "objprelook", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Ffile;
	t.offset = 0;
	t.data = "NEW!!";
	t.count = 5;
	clput(&cl, &t);
	for(i = 0; i < 400; i++){
		srvstagecount(ctx, &live, nil, nil);
		if(live == 1)
			break;
		sleep(5);
	}
	eqv("the parked write holds the fid's stage", live, 1);

	/*
	 * A second write on the same fid, and the Tflush of it: the loop
	 * reads the two in the order they were sent and queues the write
	 * behind the request that is parked, so the flush finds it waiting
	 * and performs step 7 for it there (queue.c).
	 */
	srvcount(ctx, &np, &nd);
	t.tag = tb = cltag(&cl);
	clput(&cl, &t);
	for(i = 0; i < 400; i++){
		srvcount(ctx, &np2, &nd);
		if(np2 > np)
			break;
		sleep(5);
	}
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = tb;
	clput(&cl, &t);
	if(clgettag(&cl, tb, &r) < 0)
		fail("no answer for the flushed write");
	else
		eqs("the flushed queued write", clerr(&r), "interrupted");
	cltagfree(&cl, tb);
	if(clgettag(&cl, tf, &r) < 0)
		fail("no Rflush");
	else{
		checks++;
		if(r.type != Rflush)
			fail("the Rflush after it: %s", clerr(&r));
	}
	cltagfree(&cl, tf);
	srvstagecount(ctx, &live, &done, nil);
	eqv("step 7 took the stage the parked write staged", live, 0);
	eqv("... and gave back what it held", done, 1);

	srvhook(ctx, "objprelook", 0);
	if(clgettag(&cl, ta, &r) < 0)
		fail("no answer for the parked write");
	else
		eqs("a write whose stage step 7 took", clerr(&r),
			"shoalsrv: staged update discarded");
	cltagfree(&cl, ta);
	n = clslurp(&cl, Ffile, buf, sizeof buf - 1);
	if(n < 0)
		n = 0;
	buf[n] = 0;
	eqs("... and commits none of its bytes", buf, "orig!");

	/* the other answer: the write's own request is what was flushed */
	srvhook(ctx, "objprelook", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Ffile;
	t.offset = 0;
	t.data = "AGAIN";
	t.count = 5;
	clput(&cl, &t);
	for(i = 0; i < 400; i++){
		srvstagecount(ctx, &live, nil, nil);
		if(live == 1)
			break;
		sleep(5);
	}
	eqv("the second parked write holds a stage of its own", live, 1);
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = ta;
	clput(&cl, &t);
	if(clgettag(&cl, ta, &r) < 0)
		fail("no answer for the flushed running write");
	else
		eqs("a write whose own request was flushed", clerr(&r),
			"interrupted");
	cltagfree(&cl, ta);
	if(clgettag(&cl, tf, &r) < 0)
		fail("no Rflush for the running write");
	else{
		checks++;
		if(r.type != Rflush)
			fail("the Rflush after the running write: %s", clerr(&r));
	}
	cltagfree(&cl, tf);
	srvhook(ctx, "objprelook", 0);
	srvstagecount(ctx, &live, &done, nil);
	eqv("the flushed write leaves no stage behind", live, 0);
	eqv("... and that is the second stage given back", done, 2);
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot, &r);
Out:
	srvhook(ctx, "objprelook", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §3.6's sweep trigger is an absence of ARRIVALS, and a handler that
 * is between two steps of one operation is not one.  The look a
 * handler takes at its own stage before the commit (§5.4 step 3) is
 * that moment, and the objlook point is what parks a request at it: a
 * sweep driven by an operation on another object — which is where the
 * sweep runs — must not expire the stage the look is about to call
 * live.  `gamma' is the other object because it hashes to a different
 * queue than `alpha' does, so its operation is not behind the parked
 * one.
 *
 * What holds the sweep off is the stage's `busy' mark and nothing else:
 * the parked request holds no lock, so the sweep really does walk the
 * list while the look is parked in the middle of it.  The open of
 * `gamma' is therefore waited for BEFORE the point is cleared, and the
 * stage counted while the write is still parked — a stage stripped
 * there is one the sweep took out from under a handler, whatever the
 * write answers afterwards.
 */
static void
tstagelook(void)
{
	char buf[64], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	uvlong live, done;
	ushort ta, tg;
	int n;

	clstage = "stagelook";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 50)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", "orig!", 5);
	mkobj(srvstore(ctx), "gamma", nil, 0);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, ORDWR, &r) != Ropen){
		fail("open /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile2, "obj", "gamma", &r) != Rwalk){
		fail("walk /obj/gamma: %s", clerr(&r));
		goto Out;
	}
	srvhook(ctx, "objlook", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Ffile;
	t.offset = 0;
	t.data = "NEW!!";
	t.count = 5;
	clput(&cl, &t);
	sleep(200);			/* parked at the look, and idle past stagems */

	/* the sweep runs at the head of this, on the other object's queue */
	memset(&t, 0, sizeof t);
	t.type = Topen;
	t.tag = tg = cltag(&cl);
	t.fid = Ffile2;
	t.mode = OREAD;
	clput(&cl, &t);
	if(clgettag(&cl, tg, &r) < 0)
		fail("no answer for the open that drove the sweep");
	else{
		checks++;
		if(r.type != Ropen)
			fail("open /obj/gamma: %s", clerr(&r));
	}
	cltagfree(&cl, tg);
	/* the sweep has run, and the write is still parked in its look */
	srvstagecount(ctx, &live, &done, nil);
	eqv("the sweep leaves the stage a handler is inside", live, 1);
	eqv("... and gives nothing back", done, 0);
	srvhook(ctx, "objlook", 0);

	if(clgettag(&cl, ta, &r) < 0)
		fail("no answer for the write parked in its look");
	else{
		checks++;
		if(r.type != Rwrite)
			fail("the sweep took a stage its handler was inside:"
				" %s", clerr(&r));
		else
			eqv("... and the write commits", r.count, 5);
	}
	cltagfree(&cl, ta);
	n = clslurp(&cl, Ffile, buf, sizeof buf - 1);
	if(n < 0)
		n = 0;
	buf[n] = 0;
	eqs("... with the bytes it was carrying", buf, "NEW!!");
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Ffile2, &r);
	clclunk(&cl, Froot, &r);
Out:
	srvhook(ctx, "objlook", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The same park, against a Tflush of a SIBLING request on the same fid.
 * Step 7 for a request that was still queued runs on the service loop
 * and takes the flushed fid's state lock (queue.c's srvstep7), and the
 * loop is both what clears a point and what the shutdown runs on — so a
 * request parked in its look must hold no lock at all.  Two answers are
 * owed here and both must arrive: `interrupted' for the flushed write,
 * and `staged update discarded' for the parked one, whose stage step 7
 * took while it was parked.
 */
static void
tlookflush(void)
{
	char buf[64], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	uvlong live, done, np, np2, nd;
	ushort ta, tb, tf;
	int i, n;

	clstage = "lookflush";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", "orig!", 5);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, ORDWR, &r) != Ropen){
		fail("open /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	srvhook(ctx, "objlook", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Ffile;
	t.offset = 0;
	t.data = "NEW!!";
	t.count = 5;
	clput(&cl, &t);
	for(i = 0; i < 400; i++){
		srvstagecount(ctx, &live, nil, nil);
		if(live == 1)
			break;
		sleep(5);
	}
	eqv("the parked write holds the fid's stage", live, 1);
	sleep(100);			/* ... and has reached the look */

	/* a second write on the same fid, queued behind it, and flushed */
	srvcount(ctx, &np, &nd);
	t.tag = tb = cltag(&cl);
	clput(&cl, &t);
	for(i = 0; i < 400; i++){
		srvcount(ctx, &np2, &nd);
		if(np2 > np)
			break;
		sleep(5);
	}
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = tb;
	clput(&cl, &t);
	if(clgettag(&cl, tb, &r) < 0)
		fail("no answer for the write flushed behind the parked one");
	else
		eqs("the flushed queued write", clerr(&r), "interrupted");
	cltagfree(&cl, tb);
	if(clgettag(&cl, tf, &r) < 0)
		fail("no Rflush: the loop is parked behind the look");
	else{
		checks++;
		if(r.type != Rflush)
			fail("the Rflush after it: %s", clerr(&r));
	}
	cltagfree(&cl, tf);
	srvstagecount(ctx, &live, &done, nil);
	eqv("step 7 reaches the fid of a write parked in its look", live, 0);
	eqv("... and gives back what it held", done, 1);

	srvhook(ctx, "objlook", 0);
	if(clgettag(&cl, ta, &r) < 0)
		fail("no answer for the parked write");
	else
		eqs("the write comes back to a stage step 7 took", clerr(&r),
			"shoalsrv: staged update discarded");
	cltagfree(&cl, ta);
	n = clslurp(&cl, Ffile, buf, sizeof buf - 1);
	if(n < 0)
		n = 0;
	buf[n] = 0;
	eqs("... and commits none of its bytes", buf, "orig!");
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot, &r);
Out:
	srvhook(ctx, "objlook", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * D16's shutdown: a fid holding a stage when the connection drops has
 * it discarded by the shutdown's sweep, BEFORE the store closes —
 * store.md §9 allows nothing but the snapshot calls afterwards, so a
 * discard that ran later would be no use to a fid holding an engine
 * stage.  srvstagecount's third number is what says it ran in time.
 */
static void
tstageclose(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	uvlong live, done, openat;

	clstage = "stageclose";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0);
	srvstagepoint(ctx, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk
	|| clopen(&cl, Ffile, OWRITE, &r) != Ropen){
		fail("open /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	srvstagepoint(ctx, 0);
	srvstagecount(ctx, &live, nil, nil);
	eqv("the fid holds a stage at the hangup", live, 1);
Out:
	clstop(&cl);			/* the hangup is the shutdown's trigger */
	srvstagecount(ctx, &live, &done, &openat);
	eqv("the shutdown discards the stage of a fid still open", live, 0);
	eqv("... exactly once", done, 1);
	eqv("... and before the store closed", openat, 1);
	srvstagepoint(ctx, 0);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * Every operation that names an object goes through that object's
 * queue (§5.4.1, store.md §7), the pool's counters being where that is
 * visible: an open, a read, a write, a create, a wstat and a remove
 * each cost one push.  A read of /meta after its open does not, being
 * served from the fid's rendered text on the service loop.
 */
static void
tqueued(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	Dir dir;
	uvlong np, np2, nd;

	clstage = "queued";
	m = mkmap(Palone, Tblksz, Tobjmax, Tuuid);
	d = newdisk(Tnslots);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", "bytes", 5);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Ffile, "obj", "alpha", &r) != Rwalk){
		fail("walk /obj/alpha: %s", clerr(&r));
		goto Out;
	}
	srvcount(ctx, &np, &nd);
	if(clopen(&cl, Ffile, ORDWR, &r) != Ropen)
		fail("open /obj/alpha: %s", clerr(&r));
	srvcount(ctx, &np2, &nd);
	eqv("an open of /obj/<oid> is queued against the object", np2-np, 1);
	np = np2;
	clread(&cl, Ffile, 0, 4, &r);
	srvcount(ctx, &np2, &nd);
	eqv("a read of /obj/<oid> is", np2-np, 1);
	np = np2;
	clwriteb(&cl, Ffile, 0, "x", 1, &r);
	srvcount(ctx, &np2, &nd);
	eqv("a write of /obj/<oid> is", np2-np, 1);
	np = np2;
	nulldir(&dir);
	dir.length = 2;
	clwstat(&cl, Ffile, &dir, &r);
	srvcount(ctx, &np2, &nd);
	eqv("a wstat of /obj/<oid> is", np2-np, 1);
	clclunk(&cl, Ffile, &r);

	if(clwalkobj(&cl, Froot, Fmeta, "meta", "alpha", &r) != Rwalk)
		fail("walk /meta/alpha: %s", clerr(&r));
	srvcount(ctx, &np, &nd);
	if(clopen(&cl, Fmeta, OREAD, &r) != Ropen)
		fail("open /meta/alpha: %s", clerr(&r));
	srvcount(ctx, &np2, &nd);
	eqv("an open of /meta/<oid> is queued against the object", np2-np, 1);
	np = np2;
	clread(&cl, Fmeta, 0, 64, &r);
	srvcount(ctx, &np2, &nd);
	eqv("a read of the rendered /meta text is not queued", np2-np, 0);
	clclunk(&cl, Fmeta, &r);

	if(clwalk1(&cl, Froot, Fdir, "obj", &r) != Rwalk)
		fail("walk /obj: %s", clerr(&r));
	srvcount(ctx, &np, &nd);
	clcreate(&cl, Fdir, "brandnew", 0666, OWRITE, &r);
	srvcount(ctx, &np2, &nd);
	eqv("a create in /obj is queued against the id it names", np2-np, 1);
	clclunk(&cl, Fdir, &r);
	if(clwalkobj(&cl, Froot, Ffile, "obj", "brandnew", &r) != Rwalk)
		fail("walk /obj/brandnew: %s", clerr(&r));
	srvcount(ctx, &np, &nd);
	clremove(&cl, Ffile, &r);
	srvcount(ctx, &np2, &nd);
	eqv("a remove of /obj/<oid> is", np2-np, 1);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

void
threadmain(int argc, char **argv)
{
	USED(argc);
	USED(argv);
	quotefmtinstall();		/* the FAIL lines quote what they got */
	clwatchms = 120*1000;		/* this program's own budget */
	clwatchon();

	tio();
	tmodes();
	tcreate();
	tremove();
	tmeta();
	tmetaplace();
	tdegraded();
	tnotprimary();
	tshort();
	tdiskfull();
	tstage();
	tstagesweep();
	tsweepsites();
	tsweepclunk();
	tstagearm();
	tstageflush();
	tpendfull();
	tstagecommit();
	tstagediscard();
	tstagelook();
	tlookflush();
	tstageclose();
	tqueued();

	clwatchoff();
	if(fails > 0){
		fprint(2, "srviotest: %d of %d checks failed\n", fails, checks);
		threadexitsall("fail");
	}
	print("srviotest: %d checks ok\n", checks);
	threadexitsall(nil);
}
