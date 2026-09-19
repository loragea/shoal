#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../lib/shoal.h"
#include "../srv/srv.h"

/*
 * T1: the adopted map's handle — store.md §14(48), and srv/dat.h's
 * contract beside the Smap it describes.
 *
 * The map this server serves under is an immutable snapshot behind a
 * reference count, and srvmapswap installs a new one under a running
 * instance.  Nothing in this build swaps on its own (store.md
 * §14(18)), so this program is the only thing that ever does, and
 * what it is about is what a swap does to the readers that are inside
 * the map while it happens:
 *
 *	a request already admitted goes on reading the map it was
 *		admitted under — its epoch and its placement both, since
 *		a reader that took one from each would be reading a
 *		cluster no map describes;
 *	the write it commits carries the epoch of THAT map, read once at
 *		layer-a §5.4 step 3's stage and carried to the commit;
 *	the old snapshot is freed by its last reader and not by the
 *		swap;
 *	a text that does not parse, or that names no record with this
 *		instance's uuid, is refused and leaves the map in force
 *		untouched;
 *	/map answers the new text in full once the swap has landed;
 *	a handler has let go of its snapshot BEFORE it answers, which is
 *		what keeps the last put srvfree makes the last one there
 *		is (srv/dat.h).
 *
 * A whole instance runs inside this program, as in srvtest: a
 * simulated disk, the store engine over it, srv/libshoalsrv.a over
 * that, and a raw 9P client on the other end of a pipe (srv9p.h).
 * The window a swap has to be driven against is srv.h's objprelook
 * point, which parks a client write between its stage and the look
 * behind it — after the admission and after the key, and before the
 * placement read of §5.4 step 5.
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
	Tnslots	= 128,
	Tnqueue	= 4,

	/*
	 * The epoch the instance starts and adopts under, and the one the
	 * swapped-in map carries.  They differ so that a commit says which
	 * map it was keyed by.
	 */
	Tepoch	= 7,
	Tepoch2	= 9,

	/* fids the cases use */
	Froot	= 1,
	Froot2	= 2,
	Fa	= 3,
	Fb	= 4,
	Fmap	= 5,
	Fmeta	= 6,
	Fmap2	= 7,
};

static char Tuuid[] = "0000000000000000000000000000000a";
static char Tmonid[] = "00112233445566778899aabbccddeeff";
static char Tmonid2[] = "ffeeddccbbaa99887766554433221100";
static char Nclient[] = "role=client,epoch=7";
static char Nadmin[] = "role=admin";

/*
 * The maps.  n1.0 is this instance — its uuid is the disk's — and the
 * second record is what decides layer-a §5.4 step 5's answer, which is
 * the observable that says WHICH map a request read its placement
 * from:
 *
 *	Palone	replicas=1 with the other instance dead.  P(o) is this
 *		instance alone for every id, M is empty, and a client
 *		write acks alone.
 *	Ppeer	replicas=2 with the other instance in and up.  P(o) holds
 *		a second member this build can replicate nothing to, so
 *		the same write owes step 5a a stale mark at a monitor that
 *		is not built and is answered `degraded' (store.md
 *		§14(34)).
 *
 * So a write admitted under Palone and answered `degraded' is a write
 * that read its placement out of a map it was not admitted under.
 */
enum
{
	Palone	= 0,
	Ppeer,
};

static char*
mkmap(uvlong epoch, int kind, char *iid, char *uuid, char *monid)
{
	char *p;

	p = smprint(
		"map=t epoch=%llud\n"
		"\tmonid=%s\n"
		"\tobjmax=%llud blksz=%d replicas=%d\n"
		"\tcsumalg=blake2s256 placehash=blake2s256-64\n"
		"\tpollms=1000 leasems=3000 replms=1000 deadms=10000\n"
		"\toutmins=60 tombdays=7 mincopies=1 retain=8\n"
		"\n"
		"instance=%s onnode=n1 addr=tcp!10.0.0.1!17011\n"
		"\tuuid=%s\n"
		"\tclass=ssd weight=100 status=in up=yes since=1 fenced=no\n"
		"instance=n2.0 onnode=n2 addr=tcp!10.0.0.2!17011\n"
		"\tuuid=0000000000000000000000000000000c\n"
		"\tclass=ssd weight=100 status=%s up=%s since=1 fenced=no\n",
		epoch, monid, (uvlong)Tobjmax, Tblksz,
		kind == Ppeer ? 2 : 1, iid, uuid,
		kind == Ppeer ? "in" : "dead", kind == Ppeer ? "yes" : "no");
	if(p == nil)
		sysfatal("smprint: %r");
	return p;
}

/* the ordinary one: this instance under its own iid and uuid */
static char*
mkself(uvlong epoch, int kind)
{
	return mkmap(epoch, kind, "n1.0", Tuuid, Tmonid);
}

static void
fmtcfg(Fmtcfg *c)
{
	memset(c, 0, sizeof *c);
	c->secsz = Tsecsz;
	c->blksz = Tblksz;
	c->objmax = Tobjmax;
	c->nslots = Tnslots;
	c->nemap = 32;
	c->ndirty = 64;
	c->logbytes = 256*1024;
	c->csumalg = Csumblake2s;
	memset(c->uuid, 0, 16);
	c->uuid[15] = 0x0a;		/* Tuuid */
	c->uuidset = 1;
}

static Dev*
newdisk(void)
{
	Dev *d;
	Super s;
	Fmtcfg c;

	if((d = simopen(Tsecsz, Tnsec, Tseed)) == nil)
		sysfatal("simopen: %r");
	fmtcfg(&c);
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
startsrv(Dev *d, char *maptext)
{
	Srvcfg cfg;
	Srvctx *c;

	memset(&cfg, 0, sizeof cfg);
	cfg.dev = d;
	cfg.maptext = maptext;
	cfg.maplen = strlen(maptext);
	cfg.nqueue = Tnqueue;
	cfg.store.spawn = tspawn;
	cfg.store.nockptproc = 1;
	cfg.store.ckwaitms = 200;
	cfg.store.emapcache = 16;
	cfg.store.stagemax = 8;
	cfg.store.stagetot = 64;
	cfg.store.stagems = 0;
	if((c = srvnew(&cfg)) == nil)
		fail("srvnew: %r");
	return c;
}

/* create an object through the engine, which is not what is under test */
static void
mkobj(Store *s, char *name)
{
	static uchar data[512];
	Objinfo oi;
	uchar oid[Oidmax];
	int len;

	len = strlen(name);
	memmove(oid, name, len);
	if(objcreate(s, oid, len, 1, Tepoch, nil, 0, &oi) < 0){
		fail("objcreate %s: %r", name);
		return;
	}
	if(objwrite(s, oid, len, data, sizeof data, 0, 2, Tepoch, nil, 0) < 0)
		fail("objwrite %s: %r", name);
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

/* one attr=value out of the /meta line, whose fields are space-separated */
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

/*
 * Wait until `n' requests have reached a named point (srv.h's
 * srvheld).  A case built around a window waits here rather than
 * sleeping: what it needs is the request PARKED, and a sleep long
 * enough on one machine is a wedge or a silent pass on the next.
 */
static void
waitheld(Srvctx *ctx, char *name, uvlong n)
{
	int i;

	for(i = 0; i < 400; i++){
		if(srvheld(ctx, name) >= n)
			return;
		sleep(5);
	}
	fail("no request reached the %s point", name);
}

/*
 * Wait for the snapshot count to fall back to `want'.  The reply to a
 * request comes out of the handler's body and the handler gives its
 * snapshot back behind that, so a case that has just read a reply has
 * not yet been told the reader let go; this is that wait, bounded, and
 * the caller asserts on what it answers.
 */
static int
waitsnaps(Srvctx *ctx, int want)
{
	int i, ns;

	ns = -1;
	for(i = 0; i < 400; i++){
		srvmapcount(ctx, &ns, nil);
		if(ns <= want)
			return ns;
		sleep(5);
	}
	return ns;
}

/* a Twrite of arbitrary bytes, pipelined: the tag is the caller's */
static void
putwrite(Cl *c, ushort tag, ulong fid, vlong off, void *a, long n)
{
	Fcall t;

	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = tag;
	t.fid = fid;
	t.offset = off;
	t.count = n;
	t.data = a;
	clput(c, &t);
}

/*
 * Two ids on different queues of the pool.  Two requests on ONE id are
 * serialized by the pool (store.md §7), so a case that needs two
 * parked at once needs two that hash apart; the hash is this server's
 * own and free to change (queue.c), so the case asks rather than
 * assumes.  Both nil is a failed check.
 */
static void
pickpair(Srvctx *ctx, char **a, char **b)
{
	char name[32];
	int i, qa;

	*a = *b = nil;
	for(i = 0; i < 64; i++){
		snprint(name, sizeof name, "w%d", i);
		if(*a == nil){
			*a = strdup(name);
			continue;
		}
		qa = srvqindex(ctx, (uchar*)*a, strlen(*a));
		if(srvqindex(ctx, (uchar*)name, strlen(name)) != qa){
			*b = strdup(name);
			break;
		}
	}
	checks++;
	if(*a == nil || *b == nil){
		fail("no two of 64 ids hash apart: no case to drive");
		free(*a);
		free(*b);
		*a = *b = nil;
	}
}

/* the wepoch /meta reports for an object, or ~0 */
static uvlong
metawepoch(Cl *cl, Srvctx *ctx, char *name)
{
	char buf[4096], val[64], *v;
	Fcall r;

	USED(ctx);
	if(clwalkobj(cl, Froot2, Fmeta, "meta", name, &r) != Rwalk
	|| clopen(cl, Fmeta, OREAD, &r) != Ropen){
		fail("open /meta/%s: %s", name, clerr(&r));
		return ~0ULL;
	}
	if(clslurp(cl, Fmeta, buf, sizeof buf) < 0){
		fail("read /meta/%s", name);
		clclunk(cl, Fmeta, &r);
		return ~0ULL;
	}
	clclunk(cl, Fmeta, &r);
	if((v = metafield(buf, "wepoch", val, sizeof val)) == nil){
		fail("/meta/%s has no wepoch=", name);
		return ~0ULL;
	}
	return strtoull(v, nil, 10);
}

/* the whole of /map, on a fid of the caller's choosing */
static long
readmap(Cl *cl, ulong fid, char *buf, long max)
{
	char *w[1];
	Fcall r;
	long n;

	w[0] = "map";
	if(clwalk(cl, Froot2, fid, 1, w, &r) != Rwalk
	|| clopen(cl, fid, OREAD, &r) != Ropen){
		fail("open /map: %s", clerr(&r));
		return -1;
	}
	n = clslurp(cl, fid, buf, max);
	clclunk(cl, fid, &r);
	if(n < 0)
		fail("read /map");
	return n;
}

/*
 * (i) A swap under load.  Two client writes on two object queues are
 * parked at objprelook — past §5.4 step 1's admission and past the key
 * step 3 chose, and short of step 5's placement read — and a map with
 * a different epoch AND a different placement answer is installed
 * under them.  What each write must show is one map and not a pair of
 * them: the placement it reads after the park is the one it was
 * admitted with (so the write is not `degraded'), and the epoch its
 * commit carries is the one it was keyed with (so /meta reads the old
 * epoch).  A reader that re-read either would answer the new map's.
 */
static void
tswapload(void)
{
	char *m0, *m1, *a, *b;
	uchar data[64];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	ushort ta, tb;
	uvlong np, nd;
	int ns, nref;

	clstage = "swapload";
	m0 = mkself(Tepoch, Palone);
	m1 = mkself(Tepoch2, Ppeer);
	d = newdisk();
	if((ctx = startsrv(d, m0)) == nil)
		return;
	pickpair(ctx, &a, &b);
	if(a == nil){
		srvfree(ctx);
		devclose(d);
		free(m0);
		free(m1);
		return;
	}
	mkobj(srvstore(ctx), a);
	mkobj(srvstore(ctx), b);
	memset(data, 0x5a, sizeof data);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach
	|| clattach(&cl, Froot2, Nadmin, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Fa, "obj", a, &r) != Rwalk
	|| clopen(&cl, Fa, ORDWR, &r) != Ropen
	|| clwalkobj(&cl, Froot, Fb, "obj", b, &r) != Rwalk
	|| clopen(&cl, Fb, ORDWR, &r) != Ropen){
		fail("open the two objects: %s", clerr(&r));
		goto Out;
	}
	waitidle(ctx, &np, &nd);

	srvhook(ctx, "objprelook", 1);
	putwrite(&cl, ta = cltag(&cl), Fa, 0, data, sizeof data);
	putwrite(&cl, tb = cltag(&cl), Fb, 0, data, sizeof data);
	waitheld(ctx, "objprelook", 2);
	srvmapcount(ctx, &ns, &nref);
	eqv("with two writes parked there is one snapshot", ns, 1);
	istrue("... and its readers are the two of them and the context",
		nref >= 3);

	eqs("the swap under them is accepted",
		srvmapswap(ctx, m1, strlen(m1)), nil);
	srvmapcount(ctx, &ns, nil);
	eqv("the swap leaves the old snapshot alive for its readers", ns, 2);
	eqv("... and the map in force is the new one",
		srvmap(ctx)->epoch, Tepoch2);

	srvhook(ctx, "objprelook", 0);
	if(clgettag(&cl, ta, &r) < 0)
		fail("no reply for the first write");
	else{
		checks++;
		if(r.type != Rwrite)
			fail("the first write, admitted under epoch %d, read "
				"the swapped-in placement: %s", Tepoch,
				clerr(&r));
		else
			eqv("... and wrote what it asked to", r.count,
				sizeof data);
	}
	if(clgettag(&cl, tb, &r) < 0)
		fail("no reply for the second write");
	else{
		checks++;
		if(r.type != Rwrite)
			fail("the second write, admitted under epoch %d, read "
				"the swapped-in placement: %s", Tepoch,
				clerr(&r));
	}
	eqv("the first write commits with the epoch it was admitted under",
		metawepoch(&cl, ctx, a), Tepoch);
	eqv("... and so does the second", metawepoch(&cl, ctx, b), Tepoch);
	eqv("the old snapshot is freed once its last reader lets go",
		waitsnaps(ctx, 1), 1);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(a);
	free(b);
	free(m0);
	free(m1);
}

/*
 * (ii) The old snapshot's lifetime.  One write is parked at
 * objprelook, holding the snapshot it was admitted under; the swap
 * installs another; the old one must still be there, and must go only
 * once the parked write has released it.  The swapped-in map keeps the
 * same placement, so the release itself is uneventful and what the
 * case is about is the count.
 */
static void
tsnaplife(void)
{
	char *m0, *m1, *a;
	uchar data[64];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	ushort ta;
	uvlong np, nd;
	int ns, nref;

	clstage = "snaplife";
	m0 = mkself(Tepoch, Palone);
	m1 = mkself(Tepoch2, Palone);
	d = newdisk();
	if((ctx = startsrv(d, m0)) == nil)
		return;
	a = "life";
	mkobj(srvstore(ctx), a);
	memset(data, 0x33, sizeof data);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach
	|| clattach(&cl, Froot2, Nadmin, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Fa, "obj", a, &r) != Rwalk
	|| clopen(&cl, Fa, ORDWR, &r) != Ropen){
		fail("open /obj/%s: %s", a, clerr(&r));
		goto Out;
	}
	waitidle(ctx, &np, &nd);
	srvmapcount(ctx, &ns, nil);
	eqv("an idle server holds one snapshot", ns, 1);

	srvhook(ctx, "objprelook", 1);
	putwrite(&cl, ta = cltag(&cl), Fa, 0, data, sizeof data);
	waitheld(ctx, "objprelook", 1);
	srvmapcount(ctx, &ns, &nref);
	eqv("a parked write makes no second snapshot", ns, 1);
	istrue("... and holds the one in force", nref >= 2);

	eqs("the swap is accepted", srvmapswap(ctx, m1, strlen(m1)), nil);
	srvmapcount(ctx, &ns, nil);
	eqv("the snapshot a parked reader holds outlives the swap", ns, 2);
	/* and it is still there a moment later: nothing frees it early */
	sleep(50);
	srvmapcount(ctx, &ns, nil);
	eqv("... for as long as that reader is parked", ns, 2);

	srvhook(ctx, "objprelook", 0);
	if(clgettag(&cl, ta, &r) < 0 || r.type != Rwrite)
		fail("the parked write: %s",
			r.type == Rerror ? r.ename : "no reply");
	eqv("and is freed by its last reader letting go", waitsnaps(ctx, 1), 1);
	eqv("the write still carries the epoch it was admitted under",
		metawepoch(&cl, ctx, a), Tepoch);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m0);
	free(m1);
}

/*
 * (iii) What a swap refuses.  A text that does not parse, one that
 * names no record with this instance's uuid, one that gives that uuid
 * another iid, and one carrying another monid are each refused, and
 * each leaves the map in force serving: /map still answers the old
 * bytes, srvmap still answers the old epoch, no snapshot is made, and
 * a client write still goes through on the old placement.
 *
 * The last two are the values Srvctx keeps a COPY of, and /status
 * renders them beside the snapshot's own record (srv/dat.h): a swap
 * that moved either would have that one file answering two maps.
 */
static void
trefuse(void)
{
	static char junk[] = "this is not a map\n";
	char buf[8192], *m0, *m1, *m2, *m3, *e;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	long n;
	int ns;

	clstage = "refuse";
	m0 = mkself(Tepoch, Palone);
	/* well-formed, but no record carries this instance's uuid */
	m1 = mkmap(Tepoch2, Palone, "n1.0",
		"0000000000000000000000000000000d", Tmonid);
	/* well-formed, this instance's uuid under another iid */
	m2 = mkmap(Tepoch2, Palone, "n1.7", Tuuid, Tmonid);
	/* well-formed and ours, but under another monitor identity */
	m3 = mkmap(Tepoch2, Palone, "n1.0", Tuuid, Tmonid2);
	d = newdisk();
	if((ctx = startsrv(d, m0)) == nil)
		return;
	mkobj(srvstore(ctx), "refuse");

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach
	|| clattach(&cl, Froot2, Nadmin, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}

	e = srvmapswap(ctx, junk, sizeof junk - 1);
	istrue("a text that does not parse is refused", e != nil);
	istrue("... with the parser's own reason",
		e != nil && strstr(e, "bad map") != nil);

	e = srvmapswap(ctx, m1, strlen(m1));
	istrue("a map naming no record with this uuid is refused", e != nil);
	istrue("... saying which uuid it found nothing for",
		e != nil && strstr(e, Tuuid) != nil);

	e = srvmapswap(ctx, m2, strlen(m2));
	istrue("a map giving this uuid another iid is refused", e != nil);

	/*
	 * `monid' is the other value Srvctx holds a copy of and /status
	 * renders from that copy rather than from the snapshot (srv/dat.h),
	 * and layer-a §6.3 adopts no map under another monitor identity.
	 */
	e = srvmapswap(ctx, m3, strlen(m3));
	istrue("a map under another monitor identity is refused", e != nil);
	istrue("... naming the monid it was pinned to",
		e != nil && strstr(e, Tmonid) != nil);

	srvmapcount(ctx, &ns, nil);
	eqv("a refused swap makes no snapshot", ns, 1);
	eqv("... and leaves the epoch in force", srvmap(ctx)->epoch, Tepoch);

	if((n = readmap(&cl, Fmap, buf, sizeof buf)) >= 0){
		eqv("/map still answers the adopted text, whole", n,
			strlen(m0));
		istrue("... byte for byte", n == strlen(m0)
			&& memcmp(buf, m0, n) == 0);
	}

	/* and the old map still serves: a client write goes through on it */
	if(clwalkobj(&cl, Froot, Fa, "obj", "refuse", &r) != Rwalk
	|| clopen(&cl, Fa, ORDWR, &r) != Ropen)
		fail("open /obj/refuse: %s", clerr(&r));
	else if(clwrite(&cl, Fa, 0, "abc", &r) != Rwrite)
		fail("a write under the map a refused swap left: %s",
			clerr(&r));
	else
		eqv("a client write is served by the map that stayed",
			metawepoch(&cl, ctx, "refuse"), Tepoch);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m0);
	free(m1);
	free(m2);
	free(m3);
}

/*
 * (iv) /map after a swap.  The file is rendered at open out of one
 * snapshot's text, so once the swap has landed an open answers the new
 * text in full — and /status's `status=' and `up=', which are one
 * record's pair, come from that same map.
 */
static void
tmaptext(void)
{
	char buf[8192], val[64], *m0, *m1, *w[1];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	long n;

	clstage = "maptext";
	m0 = mkself(Tepoch, Palone);
	m1 = mkself(Tepoch2, Ppeer);
	d = newdisk();
	if((ctx = startsrv(d, m0)) == nil)
		return;

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot2, Nadmin, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if((n = readmap(&cl, Fmap, buf, sizeof buf)) >= 0){
		eqv("/map answers the adopted text before a swap", n,
			strlen(m0));
		istrue("... byte for byte",
			n == strlen(m0) && memcmp(buf, m0, n) == 0);
	}

	eqs("the swap is accepted", srvmapswap(ctx, m1, strlen(m1)), nil);

	if((n = readmap(&cl, Fmap2, buf, sizeof buf)) >= 0){
		eqv("/map answers the swapped-in text, whole", n, strlen(m1));
		istrue("... byte for byte",
			n == strlen(m1) && memcmp(buf, m1, n) == 0);
	}

	/* the same map's own record, through /status */
	w[0] = "status";
	if(clwalk(&cl, Froot2, Fmeta, 1, w, &r) != Rwalk
	|| clopen(&cl, Fmeta, OREAD, &r) != Ropen)
		fail("open /status: %s", clerr(&r));
	else if(clslurp(&cl, Fmeta, buf, sizeof buf) < 0)
		fail("read /status");
	else{
		eqs("/status reads the swapped-in record's status",
			clfield(buf, "status", val, sizeof val), "in");
		eqs("... and its up", clfield(buf, "up", val, sizeof val),
			"yes");
		clclunk(&cl, Fmeta, &r);
	}
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m0);
	free(m1);
}

/*
 * (v) A hold never spans the reply (srv/dat.h).  A queued handler's
 * snapshot must be given back BEFORE the handler answers, because the
 * answer is where lib9p counts the request complete — srvdestroyreq,
 * from closereq, inside respond and before the reply is even written —
 * and where respond then releases the service.  A handler still
 * holding one across its respond is therefore a handler the drain
 * converges past and srvfree's wait ends under: srvfree's own put
 * leaks the snapshot it was meant to free, and the handler's put,
 * arriving after free(c), takes `maplk' and decrements `nsmap' on
 * freed memory.
 *
 * The window is microseconds wide against waitreleased's 5 ms poll, so
 * it is not a thing to race.  Two points open it instead:
 *
 *	objprelook parks a client write between its stage and step 5's
 *		placement read, and a swap under it leaves the OLD
 *		snapshot alive with exactly one holder — that write.  The
 *		snapshot count is then the observable: 2 while the write
 *		holds it, 1 the moment it lets go.
 *	the END point (srv.h's srvendpoint) parks the completion inside
 *		respond, just past the count the drain converges on.  With
 *		the write's proc parked there, the case can ask the
 *		question at exactly the moment the reply is going out,
 *		with no clock in it.
 *
 * So: park the write, swap under it, arm the end point, and hang the
 * client up — which is how the window is reached in earnest, D16's
 * drain releasing a queued request the client is no longer there for.
 * The shutdown's srvholdclear frees the write, it runs to its answer,
 * and its proc stops inside respond.  A handler that gave its snapshot
 * back before answering has already freed the old one and the count
 * reads 1; one that still holds it reads 2.
 */
static void
treplyhold(void)
{
	char *m0, *m1, *a;
	uchar data[64];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	ushort ta;
	uvlong np, nd;
	int ns;

	clstage = "replyhold";
	m0 = mkself(Tepoch, Palone);
	m1 = mkself(Tepoch2, Palone);
	d = newdisk();
	if((ctx = startsrv(d, m0)) == nil)
		return;
	a = "reply";
	mkobj(srvstore(ctx), a);
	memset(data, 0x77, sizeof data);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalkobj(&cl, Froot, Fa, "obj", a, &r) != Rwalk
	|| clopen(&cl, Fa, ORDWR, &r) != Ropen){
		fail("open /obj/%s: %s", a, clerr(&r));
		goto Out;
	}
	waitidle(ctx, &np, &nd);

	srvhook(ctx, "objprelook", 1);
	putwrite(&cl, ta = cltag(&cl), Fa, 0, data, sizeof data);
	waitheld(ctx, "objprelook", 1);
	eqs("the swap under the parked write is accepted",
		srvmapswap(ctx, m1, strlen(m1)), nil);
	srvmapcount(ctx, &ns, nil);
	eqv("the parked write is the old snapshot's one holder", ns, 2);

	/*
	 * Nothing but the shutdown releases the write from here: the point
	 * is left set, and srvholdclear is what clears it.
	 */
	srvendpoint(ctx, 60*1000);
	clhangup(&cl);
	waitidle(ctx, &np, &nd);
	istrue("the drain converges with the write's proc still in lib9p",
		!srvreleased(ctx));
	srvmapcount(ctx, &ns, nil);
	eqv("the handler let go of its snapshot before it answered", ns, 1);

	srvendpoint(ctx, 0);
	checks++;
	if(clgettag(&cl, ta, &r) < 0 || r.type != Rwrite)
		fail("the write the shutdown released: %s",
			r.type == Rerror ? r.ename : "no reply");
	else
		eqv("... having written what it asked to", r.count,
			sizeof data);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m0);
	free(m1);
}

void
threadmain(int argc, char **argv)
{
	USED(argc);
	USED(argv);
	quotefmtinstall();		/* the FAIL lines quote what they got */
	clwatchms = 60*1000;		/* this program's own budget */
	clwatchon();

	tswapload();
	tsnaplife();
	trefuse();
	tmaptext();
	treplyhold();

	clwatchoff();
	if(fails > 0){
		fprint(2, "maphandletest: %d of %d checks failed\n",
			fails, checks);
		threadexitsall("fail");
	}
	print("maphandletest: %d checks ok\n", checks);
	threadexitsall(nil);
}
