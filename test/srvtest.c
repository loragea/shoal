#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../lib/shoal.h"
#include "../srv/srv.h"

/*
 * T1: the storage instance's 9P surface, docs/design/layer-a.md §2 —
 * start-up and identity, §2.1's attach, §2.2's tree and its role
 * matrix, §2.3's qids, the render-at-open status files, §2.5's ctl
 * framework and §5.4.1's Tflush — and D16's shutdown order.
 *
 * A whole instance runs inside this program: a simulated disk, the
 * store engine over it, srv/libshoalsrv.a over that, and a raw 9P
 * client on the other end of a pipe (test/srv9p.h).  Nothing is
 * mounted and nothing is exec'd, so what a case asserts is the exact
 * bytes of a reply — which is what most of §2's rules are about.
 *
 * The geometry is §13's small one with objmax raised to 2^20, because
 * layer-a §1.2 makes 2^20 the floor for a legal map and store.md
 * §14(8) makes the server refuse a map whose objmax is not the disk's.
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

/* what a reply says went wrong, for a case that compares the string */
static char*
errof(Fcall *r)
{
	if(r->type == Rerror)
		return r->ename;
	return "ok";
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

	/* fids the cases use */
	Froot	= 1,
	Ffile	= 2,
	Ffile2	= 3,
	Fctl	= 4,
	Froot2	= 5,
	Fctl2	= 6,
};

static char Tuuid[] = "0000000000000000000000000000000a";
static char Tmonid[] = "00112233445566778899aabbccddeeff";

/*
 * The one instance whose store this program formats, plus two others.
 * status and up are this instance's own record's, which is what §6.4
 * F3 reads; every case but F3's passes the serving pair.
 */
static char *
mkmapself(uvlong epoch, ulong blksz, uvlong objmax, char *csumalg, char *uuid,
	char *status, char *up)
{
	char *p;

	p = smprint(
		"map=t epoch=%llud\n"
		"\tmonid=%s\n"
		"\tobjmax=%llud blksz=%lud replicas=2\n"
		"\tcsumalg=%s placehash=blake2s256-64\n"
		"\tpollms=1000 leasems=3000 replms=1000 deadms=10000\n"
		"\toutmins=60 tombdays=7 mincopies=1 retain=8\n"
		"\n"
		"instance=n1.0 onnode=n1 addr=tcp!10.0.0.1!17011\n"
		"\tuuid=%s\n"
		"\tclass=ssd weight=100 status=%s up=%s since=1 fenced=no\n"
		"instance=n1.1 onnode=n1 addr=tcp!10.0.0.1!17012\n"
		"\tuuid=0000000000000000000000000000000b\n"
		"\tclass=ssd weight=100 status=in up=yes since=1 fenced=no\n"
		"instance=n2.0 onnode=n2 addr=tcp!10.0.0.2!17011\n"
		"\tuuid=0000000000000000000000000000000c\n"
		"\tclass=ssd weight=100 status=dead up=no since=1 fenced=no\n",
		epoch, Tmonid, objmax, blksz, csumalg, uuid, status, up);
	if(p == nil)
		sysfatal("smprint: %r");
	return p;
}

static char *
mkmap(uvlong epoch, ulong blksz, uvlong objmax, char *csumalg, char *uuid)
{
	return mkmapself(epoch, blksz, objmax, csumalg, uuid, "in", "yes");
}

static void
fmtcfg(Fmtcfg *c)
{
	memset(c, 0, sizeof *c);
	c->secsz = Tsecsz;
	c->blksz = Tblksz;
	c->objmax = Tobjmax;
	c->nslots = 128;
	c->nemap = 32;
	c->ndirty = 64;
	c->logbytes = 64*1024;
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

/* D16's observable: the engine's last act before the Store's memory goes */
static int freedseen;
static int freedqdepth;
static Srvctx *freedctx;

static void
onfreed(void*)
{
	uvlong np, nd;

	freedseen++;
	np = nd = 0;
	if(freedctx != nil)
		srvcount(freedctx, &np, &nd);
	freedqdepth = np - nd;
}

static void
srvcfg(Srvcfg *cfg, Dev *d, char *maptext, int nq)
{
	memset(cfg, 0, sizeof *cfg);
	cfg->dev = d;
	cfg->maptext = maptext;
	cfg->maplen = strlen(maptext);
	cfg->nqueue = nq;
	cfg->store.spawn = tspawn;
	cfg->store.nockptproc = 1;
	cfg->store.ckwaitms = 200;
	cfg->store.emapcache = 16;
	cfg->store.stagemax = 8;
	cfg->store.stagetot = 12;
	cfg->store.stagems = 50;
	cfg->store.freed = onfreed;
}

static Srvctx*
startsrv(Dev *d, char *maptext, int nq)
{
	Srvcfg cfg;
	Srvctx *c;

	srvcfg(&cfg, d, maptext, nq);
	if((c = srvnew(&cfg)) == nil)
		fail("srvnew: %r");
	freedctx = c;
	return c;
}

/* §2.4's Tcreate, which every gate case issues on a directory fid */
static int
clcreate(Cl *c, ulong fid, char *name, int mode, Fcall *r)
{
	Fcall t;

	memset(&t, 0, sizeof t);
	t.type = Tcreate;
	t.tag = cltag(c);
	t.fid = fid;
	t.name = name;
	t.perm = 0666;
	t.mode = mode;
	return clrpc(c, &t, r);
}

/* create and fill an object through the engine, as §2.4's create is not built */
static void
mkobj(Store *s, char *name, void *data, long n, uvlong ver)
{
	Objinfo oi;
	uchar oid[Oidmax];
	int len;

	len = strlen(name);
	memmove(oid, name, len);
	if(objcreate(s, oid, len, ver, Tepoch, nil, 0, &oi) < 0){
		fail("objcreate %s: %r", name);
		return;
	}
	if(n > 0 && objwrite(s, oid, len, data, n, 0, ver+1, Tepoch, nil, 0) < 0)
		fail("objwrite %s: %r", name);
}

static int
objinfoof(Store *s, char *name, Objinfo *oi)
{
	uchar oid[Oidmax];
	int len;

	len = strlen(name);
	memmove(oid, name, len);
	return objstat(s, oid, len, oi);
}

/*
 * start-up, layer-a §3.4's identity, store.md §14(8)'s geometry check
 * and §6.3's adoption decision.  Every one of these refusals happens
 * before anything is served, which is why they are srvnew's and not a
 * handler's.
 */
static void
tstartup(void)
{
	char *m, *good;
	Srvcfg cfg;
	Srvctx *c;
	Store *st;
	Dev *d;

	clstage = "startup";
	good = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);

	d = newdisk();
	m = mkmap(Tepoch, 8192, Tobjmax, "blake2s256", Tuuid);
	srvcfg(&cfg, d, m, 4);
	istrue("a map whose blksz is not the disk's is refused",
		srvnew(&cfg) == nil);
	free(m);

	m = mkmap(Tepoch, Tblksz, 2*Tobjmax, "blake2s256", Tuuid);
	srvcfg(&cfg, d, m, 4);
	istrue("a map whose objmax is not the disk's is refused",
		srvnew(&cfg) == nil);
	free(m);

	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256",
		"000000000000000000000000000000ff");
	srvcfg(&cfg, d, m, 4);
	istrue("a map with no record for this disk's uuid is refused",
		srvnew(&cfg) == nil);
	free(m);

	m = "map=t epoch=1\n";
	srvcfg(&cfg, d, m, 4);
	istrue("an invalid map text is refused", srvnew(&cfg) == nil);

	/*
	 * §6.3's epoch regression: an instance that has adopted epoch 9
	 * must not adopt a map at 7.  The engine holds the high-water
	 * (store.md §2.2), so the refusal survives a restart.
	 */
	srvcfg(&cfg, d, good, 4);
	if((st = storeopen(d, &cfg.store)) == nil)
		fail("storeopen: %r");
	else{
		uchar mid[16];

		memset(mid, 0, sizeof mid);
		mid[0] = 0x00; mid[1] = 0x11; mid[2] = 0x22; mid[3] = 0x33;
		mid[4] = 0x44; mid[5] = 0x55; mid[6] = 0x66; mid[7] = 0x77;
		mid[8] = 0x88; mid[9] = 0x99; mid[10] = 0xaa; mid[11] = 0xbb;
		mid[12] = 0xcc; mid[13] = 0xdd; mid[14] = 0xee; mid[15] = 0xff;
		if(monidpin(st, mid) < 0)
			fail("monidpin: %r");
		if(epochadopt(st, 9) < 0)
			fail("epochadopt: %r");
		storeclose(st);
	}
	srvcfg(&cfg, d, good, 4);
	istrue("a map below the adopted epoch is refused",
		srvnew(&cfg) == nil);
	devclose(d);

	/*
	 * An epoch adopted with no monid pinned is a pair this server
	 * never writes — it makes the pin durable first — and one no
	 * adoption decision can be made about, since an unpinned instance
	 * has no epoch to regress from and adopts whatever it is shown.
	 * The map here is above that epoch, so nothing else refuses it.
	 */
	d = newdisk();
	srvcfg(&cfg, d, good, 4);
	if((st = storeopen(d, &cfg.store)) == nil)
		fail("storeopen: %r");
	else{
		if(epochadopt(st, 5) < 0)
			fail("epochadopt: %r");
		storeclose(st);
	}
	srvcfg(&cfg, d, good, 4);
	istrue("a disk carrying an adopted epoch and no pinned monid is "
		"refused", srvnew(&cfg) == nil);
	devclose(d);

	/* and the same map on a fresh disk is served */
	d = newdisk();
	c = startsrv(d, good, 4);
	if(c != nil){
		eqs("the instance's iid comes from the map's uuid",
			srviid(c), "n1.0");
		eqv("the adopted epoch", srvmap(c)->epoch, Tepoch);
		srvfree(c);
	}
	devclose(d);
	free(good);
}

/* layer-a §2.1's aname grammar, roles, epoch compare and membership */
static void
tattach(void)
{
	static struct {
		char	*aname;
		char	*err;		/* nil: the attach succeeds */
	} cases[] = {
		{"epoch=7",			nil},
		{"role=client,epoch=7",		nil},
		{"epoch=7,role=client",		nil},
		{"role=admin",			nil},
		{"role=admin,epoch=7",		nil},
		{"role=repl,peer=n1.1",		nil},
		{"role=repl,peer=n1.1,epoch=7",	nil},
		{"",				"bad aname"},
		{"role=client",			"bad aname"},
		{"epoch=",			"bad aname"},
		{"epoch=x",			"bad aname"},
		{"epoch=7,epoch=7",		"bad aname"},
		{"role=admin,role=admin",	"bad aname"},
		{"role=repl,peer=n1.1,peer=n1.1", "bad aname"},
		{"role=",			"bad aname"},
		{"peer=,epoch=7",		"bad aname"},
		{"role=bogus,epoch=7",		"bad aname"},
		{"frob=1,epoch=7",		"bad aname"},
		{"epoch7",			"bad aname"},
		{"role=repl",			"bad aname"},
		/* an iid is at most Iidlen bytes; this one is 80 */
		{"role=repl,peer=nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn"
		 "nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn.0",	"bad aname"},
		/*
		 * §2.1's epoch is a u64.  A run of digits above that names no
		 * epoch — it is an unparseable specifier, not an epoch in the
		 * future — while the largest u64 there is parses and compares.
		 */
		{"epoch=18446744073709551616",	"bad aname"},
		{"epoch=99999999999999999999",	"bad aname"},
		{"epoch=184467440737095516150",	"bad aname"},
		{"epoch=18446744073709551615",	"future epoch"},
		{"epoch=6",			"stale epoch"},
		{"epoch=8",			"future epoch"},
		{"role=repl,peer=n2.0",		"permission denied"},
		{"role=repl,peer=n9.9",		"permission denied"},
	};
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	int i;

	clstage = "attach";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	clstart(&cl, ctx, Clmsize);
	for(i = 0; i < nelem(cases); i++){
		clattach(&cl, Froot, cases[i].aname, &r);
		if(cases[i].err == nil){
			checks++;
			if(r.type != Rattach)
				fail("attach %#q: %s", cases[i].aname,
					r.type == Rerror ? r.ename : "not Rattach");
			else{
				eqv("the root qid is a directory",
					r.qid.type, QTDIR);
				clclunk(&cl, Froot, &r);
			}
		}else{
			checks++;
			if(r.type != Rerror){
				fail("attach %#q: no error, want %s",
					cases[i].aname, cases[i].err);
				clclunk(&cl, Froot, &r);
			}else
				eqs("attach error", r.ename, cases[i].err);
		}
	}
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §2.1's msize floor.  lib9p answers Tversion itself, so the floor is
 * enforced at Tattach off the negotiated size, and /status reports the
 * same number.
 */
static void
tmsize(void)
{
	char buf[4096], val[64], val2[64], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	char *path[1];
	long n;

	clstage = "msize";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;

	clstart(&cl, ctx, 4096);
	eqv("a low msize is negotiated as proposed", cl.msize, 4096);
	clattach(&cl, Froot, "role=admin", &r);
	checks++;
	if(r.type != Rerror)
		fail("attach under the msize floor: no error");
	else
		istrue("the msize refusal carries no §2.6 prefix",
			srv26(r.ename) == nil);
	clstop(&cl);
	srvfree(ctx);

	/* a Srvctx serves one loop: the shutdown closed that store */
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	clstart(&cl, ctx, Clmsize);
	eqv("the floor msize is accepted", cl.msize, Clmsize);
	clattach(&cl, Froot, "role=admin", &r);
	eqv("attach at the floor", r.type, Rattach);
	path[0] = "status";
	if(clopenpath(&cl, Froot, Ffile, 1, path, OREAD, &r) == Ropen){
		n = clslurp(&cl, Ffile, buf, sizeof buf);
		istrue("/status reads", n > 0);
		snprint(val2, sizeof val2, "%d", Clmsize);
		eqs("/status msize=", clfield(buf, "msize", val, sizeof val),
			val2);
	}else
		fail("open /status: %s", r.type == Rerror ? r.ename : "?");
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §2.2's tree under §2.1's role matrix, in full: every file, every
 * role, walk and open.  The table here is written out rather than
 * derived from the server's, so that the two have to agree.
 */
static void
tmatrix(void)
{
	static char *anames[3] = {
		"role=client,epoch=7",
		"role=repl,peer=n1.1",
		"role=admin",
	};
	static struct {
		char	*name;
		char	*walk[3];
		char	*open[3];
	} files[] = {
	/*	 file	  walk: client       repl	   admin     open: client	repl		admin */
		{"ctl",	  {nil, nil, nil},		{nil, nil, nil}},
		{"status",{"permission denied", "permission denied", nil},
			  {nil, nil, nil}},
		{"map",	  {"permission denied", "permission denied", nil},
			  {nil, nil, nil}},
		{"obj",	  {nil, nil, nil},
			  {"permission denied", "permission denied", "shoalsrv: not built"}},
		{"meta",  {nil, nil, nil},
			  {"permission denied", "permission denied", "shoalsrv: not built"}},
		{"repl",  {"permission denied", nil, "permission denied"},
			  {nil, "shoalsrv: not built", nil}},
		{"rpc",	  {"permission denied", nil, nil},
			  {nil, "shoalsrv: not built", "shoalsrv: not built"}},
		{"advert",{"permission denied", nil, "permission denied"},
			  {nil, "shoalsrv: not built", nil}},
		{"dirty", {"permission denied", "permission denied", nil},
			  {nil, nil, "shoalsrv: not built"}},
		{"stale", {"permission denied", "permission denied", nil},
			  {nil, nil, "shoalsrv: not built"}},
		{"tombs", {"permission denied", "permission denied", nil},
			  {nil, nil, "shoalsrv: not built"}},
		{"lost",  {"permission denied", "permission denied", nil},
			  {nil, nil, "shoalsrv: not built"}},
		{"jobs",  {"permission denied", "permission denied", nil},
			  {nil, nil, "shoalsrv: not built"}},
	};
	static int nwalkable[3] = {3, 6, 11};
	char buf[8192], *m, *want;
	uchar *p, *ep;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	Dir dir;
	int i, role, nent;
	long n;

	clstage = "matrix";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	clstart(&cl, ctx, Clmsize);
	for(role = 0; role < 3; role++){
		if(clattach(&cl, Froot, anames[role], &r) != Rattach){
			fail("attach %s: %s", anames[role],
				r.type == Rerror ? r.ename : "?");
			continue;
		}
		for(i = 0; i < nelem(files); i++){
			clwalk1(&cl, Froot, Ffile, files[i].name, &r);
			want = files[i].walk[role];
			checks++;
			if(want != nil){
				if(r.type != Rerror || strcmp(r.ename, want) != 0)
					fail("walk %s as %s: %s, want %s",
						files[i].name, anames[role],
						r.type == Rerror ? r.ename : "ok",
						want);
				continue;
			}
			if(r.type != Rwalk || r.nwqid != 1){
				fail("walk %s as %s: %s", files[i].name,
					anames[role],
					r.type == Rerror ? r.ename : "short");
				continue;
			}
			clopen(&cl, Ffile, OREAD, &r);
			want = files[i].open[role];
			checks++;
			if(want == nil){
				if(r.type != Ropen)
					fail("open %s as %s: %s",
						files[i].name, anames[role],
						r.type == Rerror ? r.ename : "?");
			}else if(r.type != Rerror || strcmp(r.ename, want) != 0)
				fail("open %s as %s: %s, want %s",
					files[i].name, anames[role],
					r.type == Rerror ? r.ename : "ok", want);
			clclunk(&cl, Ffile, &r);
		}
		/* the root listing carries exactly the rows this role may walk to */
		if(clopen(&cl, Froot, OREAD, &r) != Ropen)
			fail("open / as %s: %s", anames[role],
				r.type == Rerror ? r.ename : "?");
		else{
			n = clslurp(&cl, Froot, buf, sizeof buf);
			nent = 0;
			p = (uchar*)buf;
			ep = p + n;
			while(p < ep){
				i = convM2D(p, ep-p, &dir, (char*)p+BIT16SZ);
				if(i <= BIT16SZ)
					break;
				p += i;
				nent++;
			}
			eqv("root entries for this role", nent, nwalkable[role]);
		}
		clclunk(&cl, Froot, &r);
	}
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The same matrix over the three operations that are not walk and
 * open: §2.4's create in /obj, remove of /obj/<oid> and wstat of one.
 * Each is gated on the row's write column and then answers the local
 * `not built', because §2.4's content is the object-I/O surface's.
 */
static void
tmodes(void)
{
	static char *anames[3] = {
		"role=client,epoch=7",
		"role=repl,peer=n1.1",
		"role=admin",
	};
	/*
	 * The write column of /obj and /obj/<oid> admits client and
	 * admin; §2.1's operator rule then refuses the admin half for
	 * every id that is not a reserved one, which `newobj' and
	 * `alpha' are not.
	 */
	static char *want[3] = {
		"shoalsrv: not built",
		"permission denied",
		"permission denied",
	};
	uchar stat[STATMAX];
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	Dir dir;
	int role, n;

	clstage = "modes";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0, 1);
	clstart(&cl, ctx, Clmsize);
	for(role = 0; role < 3; role++){
		if(clattach(&cl, Froot, anames[role], &r) != Rattach){
			fail("attach %s: %s", anames[role],
				r.type == Rerror ? r.ename : "?");
			continue;
		}
		/* Tcreate in /obj */
		if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk)
			fail("walk /obj: %s", r.type == Rerror ? r.ename : "?");
		memset(&t, 0, sizeof t);
		t.type = Tcreate;
		t.tag = cltag(&cl);
		t.fid = Ffile;
		t.name = "newobj";
		t.perm = 0666;
		t.mode = OWRITE;
		clrpc(&cl, &t, &r);
		checks++;
		if(r.type != Rerror || strcmp(r.ename, want[role]) != 0)
			fail("create in /obj as %s: %s, want %s", anames[role],
				r.type == Rerror ? r.ename : "ok", want[role]);
		clclunk(&cl, Ffile, &r);

		/* Twstat and Tremove on /obj/<oid> */
		if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk
		|| clwalk1(&cl, Ffile, Ffile2, "alpha", &r) != Rwalk)
			fail("walk /obj/alpha: %s",
				r.type == Rerror ? r.ename : "?");
		nulldir(&dir);
		dir.length = 4096;
		n = convD2M(&dir, stat, sizeof stat);
		memset(&t, 0, sizeof t);
		t.type = Twstat;
		t.tag = cltag(&cl);
		t.fid = Ffile2;
		t.stat = stat;
		t.nstat = n;
		clrpc(&cl, &t, &r);
		checks++;
		if(r.type != Rerror || strcmp(r.ename, want[role]) != 0)
			fail("wstat /obj/alpha as %s: %s, want %s",
				anames[role], r.type == Rerror ? r.ename : "ok",
				want[role]);
		memset(&t, 0, sizeof t);
		t.type = Tremove;
		t.tag = cltag(&cl);
		t.fid = Ffile2;
		clrpc(&cl, &t, &r);
		checks++;
		if(r.type != Rerror || strcmp(r.ename, want[role]) != 0)
			fail("remove /obj/alpha as %s: %s, want %s",
				anames[role], r.type == Rerror ? r.ename : "ok",
				want[role]);
		clclunk(&cl, Ffile, &r);
		clclunk(&cl, Froot, &r);
	}
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/* §2.3's qids, and walk and stat of /obj/<oid> and /meta/<oid> */
static void
tobjects(void)
{
	char buf[64], longname[Oidmax+2], *m;
	uchar oid[Oidmax], data[2048];
	uvlong path1, path2, np, np2, nd;
	Objinfo oi;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	Dir dir;
	char *w[2];
	int i;

	clstage = "objects";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	st = srvstore(ctx);
	for(i = 0; i < sizeof data; i++)
		data[i] = 0xA5 ^ (uchar)i;
	memset(longname, 'z', Oidmax+1);
	longname[Oidmax+1] = 0;
	mkobj(st, "alpha", data, sizeof data, 1);
	mkobj(st, "beta", nil, 0, 1);
	if(objinfoof(st, "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	path1 = oi.qidpath;

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", r.type == Rerror ? r.ename : "?");
		goto Out;
	}
	w[0] = "obj";
	w[1] = "alpha";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk || r.nwqid != 2)
		fail("walk /obj/alpha: %s", r.type == Rerror ? r.ename : "short");
	else{
		eqv("an object's qid.path is the engine's allocated one",
			r.wqid[1].path, path1);
		eqv("an object's qid.vers is the low 32 bits of ver",
			r.wqid[1].vers, oi.ver & 0xFFFFFFFFULL);
		eqv("an object's qid is a file", r.wqid[1].type, QTFILE);
	}
	if(clstat(&cl, Ffile, &r) != Rstat)
		fail("stat /obj/alpha: %s", r.type == Rerror ? r.ename : "?");
	else if(convM2D(r.stat, r.nstat, &dir, buf) <= BIT16SZ)
		fail("stat /obj/alpha: undecodable");
	else{
		eqv("stat length is len", dir.length, oi.len);
		eqv("stat mode is 0666", dir.mode, 0666);
		eqs("stat name is the oid", dir.name, "alpha");
	}
	clclunk(&cl, Ffile, &r);

	/* /meta/<oid> is a second name and takes a qid.path of its own */
	w[0] = "meta";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk || r.nwqid != 2)
		fail("walk /meta/alpha: %s", r.type == Rerror ? r.ename : "short");
	else
		istrue("/meta/<oid> and /obj/<oid> have distinct qid.paths",
			r.wqid[1].path != path1);
	clclunk(&cl, Ffile, &r);

	/*
	 * A one-element walk from an /obj fid is where §2.6's errors
	 * reach the client: 9P suppresses the error of a walk that got
	 * past its first element, and lib9p's rwalk does exactly that,
	 * so a two-element /obj/<oid> walk answers a partial Rwalk
	 * instead (store.md §14(28)).  Both are checked.
	 */
	w[0] = "obj";
	if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk)
		fail("walk /obj: %s", r.type == Rerror ? r.ename : "?");
	clwalk1(&cl, Ffile, Ffile2, "nosuch", &r);
	checks++;
	if(r.type != Rerror || strcmp(r.ename, "no such object") != 0)
		fail("walk to an absent id: %s",
			r.type == Rerror ? r.ename : "ok");
	clwalk1(&cl, Ffile, Ffile2, "not!a!name", &r);
	checks++;
	if(r.type != Rerror || strcmp(r.ename, "bad object name") != 0)
		fail("walk to a bad id: %s", r.type == Rerror ? r.ename : "ok");
	/*
	 * An over-long id names no object, so it is not ordered against
	 * one either: the pushed count does not move.  Cutting it to
	 * Oidmax would put the walk on the queue of whatever object its
	 * first 128 bytes name.
	 */
	srvcount(ctx, &np, &nd);
	clwalk1(&cl, Ffile, Ffile2, longname, &r);
	checks++;
	if(r.type != Rerror || strcmp(r.ename, "bad object name") != 0)
		fail("walk to an over-long id: %s",
			r.type == Rerror ? r.ename : "ok");
	srvcount(ctx, &np2, &nd);
	eqv("a walk to an over-long id is queued against no object",
		np2 - np, 0);
	/* ".." is the parent, not an oid */
	if(clwalk1(&cl, Ffile, Ffile2, "..", &r) != Rwalk || r.nwqid != 1)
		fail("walk .. from /obj: %s", r.type == Rerror ? r.ename : "short");
	else
		eqv("..  from /obj is the root", r.wqid[0].type, QTDIR);
	clclunk(&cl, Ffile2, &r);
	w[1] = "nosuch";
	clwalk(&cl, Froot, Ffile2, 2, w, &r);
	checks++;
	if(r.type != Rwalk || r.nwqid != 1)
		fail("a two-element walk to an absent id: %s",
			r.type == Rerror ? r.ename : "not a partial walk");
	clclunk(&cl, Ffile, &r);

	/*
	 * §2.3: qid.path is stable across delete, tombstone and
	 * re-create.  The tombstone itself is not walkable: §2.6 makes
	 * access to one `object deleted'.
	 */
	memmove(oid, "beta", 4);
	if(objremove(st, oid, 4, 2, Tepoch, nil, 0) < 0)
		fail("objremove beta: %r");
	if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk)
		fail("walk /obj: %s", r.type == Rerror ? r.ename : "?");
	clwalk1(&cl, Ffile, Ffile2, "beta", &r);
	checks++;
	if(r.type != Rerror || strcmp(r.ename, "object deleted") != 0)
		fail("walk to a tombstone: %s", r.type == Rerror ? r.ename : "ok");
	clclunk(&cl, Ffile, &r);
	if(objinfoof(st, "beta", &oi) < 0)
		fail("objstat beta: %r");
	path2 = oi.qidpath;
	if(objcreate(st, oid, 4, oi.ver+1, Tepoch, nil, 0, &oi) < 0)
		fail("objcreate beta again: %r");
	w[0] = "obj";
	w[1] = "beta";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk || r.nwqid != 2)
		fail("walk the re-created beta: %s",
			r.type == Rerror ? r.ename : "short");
	else
		eqv("qid.path survives delete and re-create",
			r.wqid[1].path, path2);
	clclunk(&cl, Ffile, &r);

	/*
	 * A walk that does not resolve every element leaves the fid where
	 * it was, and a walk of a fid onto itself is no exception: lib9p
	 * answers a partial Rwalk without touching the Fid's qid, so the
	 * client still holds the root and the next walk from it must
	 * behave like one from the root.
	 */
	if(clattach(&cl, Froot2, "role=admin", &r) != Rattach)
		fail("attach: %s", r.type == Rerror ? r.ename : "?");
	w[0] = "obj";
	w[1] = "nosuch";
	clwalk(&cl, Froot2, Froot2, 2, w, &r);
	checks++;
	if(r.type != Rwalk || r.nwqid != 1)
		fail("a partial walk of a fid onto itself: %s",
			r.type == Rerror ? r.ename : "not a partial walk");
	clwalk1(&cl, Froot2, Ffile, "ctl", &r);
	checks++;
	if(r.type != Rwalk || r.nwqid != 1)
		fail("walk to /ctl after a partial self-walk: %s",
			r.type == Rerror ? r.ename : "short");
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot2, &r);

Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/* /status, /map, and §2.2's render-at-open */
static void
tstatus(void)
{
	char buf[8192], val[64], *m;
	static char *want[] = {
		"iid", "uuid", "monid", "monidmismatch", "status", "up",
		"fence", "epoch", "epochregress", "msize", "objsnap",
		"dirty", "lost", "queues", "qdepth",
	};
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	char *w[1];
	long n;
	int i;

	clstage = "status";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", r.type == Rerror ? r.ename : "?");
		goto Out;
	}
	w[0] = "status";
	if(clopenpath(&cl, Froot, Ffile, 1, w, OREAD, &r) != Ropen){
		fail("open /status: %s", r.type == Rerror ? r.ename : "?");
		goto Out;
	}
	n = clslurp(&cl, Ffile, buf, sizeof buf);
	istrue("/status renders", n > 0);
	for(i = 0; i < nelem(want); i++){
		checks++;
		if(clfield(buf, want[i], val, sizeof val) == nil)
			fail("/status has no %s=", want[i]);
	}
	eqs("/status epoch=", clfield(buf, "epoch", val, sizeof val), "7");
	eqs("/status iid=", clfield(buf, "iid", val, sizeof val), "n1.0");
	eqs("/status uuid=", clfield(buf, "uuid", val, sizeof val), Tuuid);
	eqs("/status monid=", clfield(buf, "monid", val, sizeof val), Tmonid);
	eqs("/status fence=", clfield(buf, "fence", val, sizeof val), "none");
	eqs("/status status=", clfield(buf, "status", val, sizeof val), "in");
	eqs("/status up=", clfield(buf, "up", val, sizeof val), "yes");
	eqs("/status objsnap=", clfield(buf, "objsnap", val, sizeof val),
		"full");
	eqs("/status queues=", clfield(buf, "queues", val, sizeof val), "4");
	eqs("/status epochregress=",
		clfield(buf, "epochregress", val, sizeof val), "no");
	eqs("/status monidmismatch=",
		clfield(buf, "monidmismatch", val, sizeof val), "no");
	istrue("/status omits chunk=",
		clfield(buf, "chunk", val, sizeof val) == nil);
	istrue("/status omits underrep=",
		clfield(buf, "underrep", val, sizeof val) == nil);

	/*
	 * §2.2's snapshot-at-open: this fid was opened before the fence
	 * went on, so its bytes do not change under it.
	 */
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen)
		fail("open /ctl: %s", r.type == Rerror ? r.ename : "?");
	else if(clwrite(&cl, Fctl, 0, "fence on", &r) != Rwrite)
		fail("fence on: %s", r.type == Rerror ? r.ename : "?");
	n = clslurp(&cl, Ffile, buf, sizeof buf);
	istrue("the snapshot still reads", n > 0);
	eqs("a read after the open still shows the open's bytes",
		clfield(buf, "fence", val, sizeof val), "none");
	clclunk(&cl, Ffile, &r);
	/* a fid opened after it sees the new state */
	w[0] = "status";
	if(clopenpath(&cl, Froot, Ffile2, 1, w, OREAD, &r) != Ropen)
		fail("re-open /status: %s", r.type == Rerror ? r.ename : "?");
	else{
		istrue("the later snapshot reads",
			clslurp(&cl, Ffile2, buf, sizeof buf) > 0);
		eqs("a later open sees the operator fence",
			clfield(buf, "fence", val, sizeof val), "operator");
		eqs("/status qdepth= is back to zero",
			clfield(buf, "qdepth", val, sizeof val), "0");
	}
	clclunk(&cl, Ffile2, &r);

	/* a clunk succeeds while the instance is fenced (§2.4, §6.2) */
	eqv("clunk while fenced", clclunk(&cl, Fctl, &r), Rclunk);

	/* offsets and short reads */
	w[0] = "map";
	if(clopenpath(&cl, Froot, Ffile, 1, w, OREAD, &r) != Ropen)
		fail("open /map: %s", r.type == Rerror ? r.ename : "?");
	else{
		n = clslurp(&cl, Ffile, buf, sizeof buf);
		eqv("/map is the map text's length", n, strlen(m));
		istrue("/map is the map text", strcmp(buf, m) == 0);
		if(clread(&cl, Ffile, 5, 3, &r) == Rread){
			eqv("a short read answers what was asked", r.count, 3);
			istrue("a read at an offset answers those bytes",
				memcmp(r.data, m+5, 3) == 0);
		}else
			fail("short read: %s", r.type == Rerror ? r.ename : "?");
		if(clread(&cl, Ffile, n+100, 16, &r) == Rread)
			eqv("a read past the end answers count 0", r.count, 0);
		else
			fail("read past the end: %s",
				r.type == Rerror ? r.ename : "?");
		if(clread(&cl, Ffile, n-2, 16, &r) == Rread)
			eqv("a read crossing the end is clamped", r.count, 2);
		else
			fail("read crossing the end: %s",
				r.type == Rerror ? r.ename : "?");
	}
	clclunk(&cl, Ffile, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/* §2.5's ctl framework: the four gates over every verb row */
static void
tctl(void)
{
	static struct {
		char	*line;
		char	*unfenced;	/* nil: an Rwrite */
		char	*fenced;
	} verbs[] = {
		{"refresh",	"shoalsrv: not built",	"shoalsrv: not built"},
		{"register",	"shoalsrv: not built",	"shoalsrv: not built"},
		{"pull alpha n1.1", "shoalsrv: not built", "fenced"},
		{"push alpha n1.1", "shoalsrv: not built", "fenced"},
		{"reconcile",	"shoalsrv: not built",	"fenced"},
		{"advert",	"shoalsrv: not built",	"fenced"},
		{"drop alpha",	"shoalsrv: not built",	"fenced"},
		{"verify alpha", nil,			nil},
		{"scrub",	"shoalsrv: not built",	"shoalsrv: not built"},
		{"forget n1.1",	"shoalsrv: not built",	"fenced"},
		{"newmonid 00112233445566778899aabbccddeeff",
				"shoalsrv: not built",	"shoalsrv: not built"},
	};
	static struct {
		char	*line;
		char	*err;
	} bad[] = {
		{"",			"bad ctl"},
		{" ",			"bad ctl"},
		{"\n",			"bad ctl"},
		{"fence on\nfence off",	"bad ctl"},
		{"frobnicate",		"unknown ctl"},
		{"fence",		"bad ctl"},
		{"fence on off",	"bad ctl"},
		{"verify",		"bad ctl"},
		{"verify alpha beta",	"bad ctl"},
		{"pull alpha",		"bad ctl"},
		{"verify not!a!name",	"bad object name"},
	};
	char *m, oid[Oidmax+2], line[Oidmax+16];
	uchar data[1024];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	char *w[1];
	int i;

	clstage = "ctl";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	memset(data, 0x5a, sizeof data);
	mkobj(srvstore(ctx), "alpha", data, sizeof data, 1);
	clstart(&cl, ctx, Clmsize);

	/* every verb is refused a non-admin fid (§2.5's role column) */
	if(clattach(&cl, Froot, "role=client,epoch=7", &r) != Rattach)
		fail("attach client: %s", r.type == Rerror ? r.ename : "?");
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen)
		fail("client open /ctl: %s", r.type == Rerror ? r.ename : "?");
	else for(i = 0; i < nelem(verbs); i++){
		clwrite(&cl, Fctl, 0, verbs[i].line, &r);
		checks++;
		if(r.type != Rerror || strcmp(r.ename, "permission denied") != 0)
			fail("%#q as client: %s, want permission denied",
				verbs[i].line,
				r.type == Rerror ? r.ename : "ok");
	}
	clclunk(&cl, Fctl, &r);
	clclunk(&cl, Froot, &r);

	if(clattach(&cl, Froot, "role=admin", &r) != Rattach)
		fail("attach admin: %s", r.type == Rerror ? r.ename : "?");
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("admin open /ctl: %s", r.type == Rerror ? r.ename : "?");
		goto Out;
	}
	for(i = 0; i < nelem(bad); i++){
		clwrite(&cl, Fctl, 0, bad[i].line, &r);
		checks++;
		if(r.type != Rerror || strcmp(r.ename, bad[i].err) != 0)
			fail("%#q: %s, want %s", bad[i].line,
				r.type == Rerror ? r.ename : "ok", bad[i].err);
	}
	/*
	 * An id one byte over §1.1's bound, with the object its first 128
	 * bytes name sitting right there in the store: the verb names
	 * neither object and is refused, because an id is never cut to fit.
	 */
	memset(oid, 'a', Oidmax);
	oid[Oidmax] = 0;
	mkobj(srvstore(ctx), oid, nil, 0, 1);
	snprint(line, sizeof line, "verify %s", oid);
	clwrite(&cl, Fctl, 0, line, &r);
	checks++;
	if(r.type != Rwrite)
		fail("verify of a 128-byte id: %s",
			r.type == Rerror ? r.ename : "?");
	oid[Oidmax] = 'a';
	oid[Oidmax+1] = 0;
	snprint(line, sizeof line, "verify %s", oid);
	clwrite(&cl, Fctl, 0, line, &r);
	checks++;
	if(r.type != Rerror || strcmp(r.ename, "bad object name") != 0)
		fail("verify of a 129-byte id: %s",
			r.type == Rerror ? r.ename : "ok");

	/* a trailing newline is one line, not a partial one */
	clwrite(&cl, Fctl, 0, "verify alpha\n", &r);
	checks++;
	if(r.type != Rwrite)
		fail("verify with a trailing newline: %s",
			r.type == Rerror ? r.ename : "?");
	else
		eqv("an Rwrite counts the bytes written", r.count,
			strlen("verify alpha\n"));

	for(i = 0; i < nelem(verbs); i++){
		clwrite(&cl, Fctl, 0, verbs[i].line, &r);
		checks++;
		if(verbs[i].unfenced == nil){
			if(r.type != Rwrite)
				fail("%#q unfenced: %s", verbs[i].line,
					r.type == Rerror ? r.ename : "?");
		}else if(r.type != Rerror
		|| strcmp(r.ename, verbs[i].unfenced) != 0)
			fail("%#q unfenced: %s, want %s", verbs[i].line,
				r.type == Rerror ? r.ename : "ok",
				verbs[i].unfenced);
	}

	/* §2.5's fenced set, under an operator fence (§6.4 F4) */
	clwrite(&cl, Fctl, 0, "fence on", &r);
	checks++;
	if(r.type != Rwrite)
		fail("fence on: %s", r.type == Rerror ? r.ename : "?");
	for(i = 0; i < nelem(verbs); i++){
		clwrite(&cl, Fctl, 0, verbs[i].line, &r);
		checks++;
		if(verbs[i].fenced == nil){
			if(r.type != Rwrite)
				fail("%#q fenced: %s", verbs[i].line,
					r.type == Rerror ? r.ename : "?");
		}else if(r.type != Rerror
		|| strcmp(r.ename, verbs[i].fenced) != 0)
			fail("%#q fenced: %s, want %s", verbs[i].line,
				r.type == Rerror ? r.ename : "ok",
				verbs[i].fenced);
	}
	/* an object open is fenced too (§6.4 F1's list) */
	w[0] = "obj";
	clwalk1(&cl, Froot, Ffile, "obj", &r);
	clwalk1(&cl, Ffile, Ffile2, "alpha", &r);
	checks++;
	if(r.type != Rwalk)
		fail("walk to an object while fenced: %s",
			r.type == Rerror ? r.ename : "?");
	clopen(&cl, Ffile2, OREAD, &r);
	checks++;
	if(r.type != Rerror || strcmp(r.ename, "fenced") != 0)
		fail("open an object while fenced: %s",
			r.type == Rerror ? r.ename : "ok");
	clclunk(&cl, Ffile2, &r);
	clclunk(&cl, Ffile, &r);

	/* F4 is clearable; a lease fence would not be (§2.5, D25) */
	clwrite(&cl, Fctl, 0, "fence off", &r);
	checks++;
	if(r.type != Rwrite)
		fail("fence off under an operator fence: %s",
			r.type == Rerror ? r.ename : "?");
	clwrite(&cl, Fctl, 0, "drop alpha", &r);
	checks++;
	if(r.type != Rerror || strcmp(r.ename, "shoalsrv: not built") != 0)
		fail("a fenced-set verb after fence off: %s",
			r.type == Rerror ? r.ename : "ok");
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §2.5's `verify <oid>' end to end, on a good object and on one whose
 * bytes were changed under the store.
 */
static void
tverify(void)
{
	char *m;
	uchar data[4096], probe[4096];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	Super sb;
	Sbsel sel;
	char *w[1];
	vlong off, end;
	int i, found;

	clstage = "verify";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	for(i = 0; i < sizeof data; i++)
		data[i] = (uchar)(0x31 + (i & 0x3f));
	mkobj(srvstore(ctx), "alpha", data, sizeof data, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach)
		fail("attach: %s", r.type == Rerror ? r.ename : "?");
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", r.type == Rerror ? r.ename : "?");
		goto Out;
	}
	clwrite(&cl, Fctl, 0, "verify alpha", &r);
	checks++;
	if(r.type != Rwrite)
		fail("verify a good object: %s",
			r.type == Rerror ? r.ename : "?");
	clwrite(&cl, Fctl, 0, "verify nosuch", &r);
	checks++;
	if(r.type != Rerror || strcmp(r.ename, "no such object") != 0)
		fail("verify an absent object: %s",
			r.type == Rerror ? r.ename : "ok");

	/*
	 * Damage one object's bytes on the platter.  The grain holding
	 * them is found by looking for the pattern in the data region,
	 * which is what a scrub would otherwise have to be driven to
	 * produce; simpoke writes durable storage, so the next read of
	 * that grain sees the damage.
	 */
	if(superselect(d, &sel) < 0 || sel.start < 0){
		fail("superselect: %r");
		goto Out;
	}
	sb = sel.sb[sel.start];
	off = (vlong)sb.dataoff * sb.secsz;
	end = off + (vlong)sb.datasecs * sb.secsz;
	found = 0;
	for(; off + Tblksz <= end; off += Tblksz){
		simpeek(d, off, probe, Tblksz);
		if(memcmp(probe, data, Tblksz) == 0){
			probe[17] ^= 0xff;
			simpoke(d, off, probe, Tblksz);
			found++;
			break;
		}
	}
	istrue("an object's bytes were found in the data region", found > 0);
	clwrite(&cl, Fctl, 0, "verify alpha", &r);
	checks++;
	if(r.type != Rerror || strcmp(r.ename, "checksum mismatch") != 0)
		fail("verify a damaged object: %s",
			r.type == Rerror ? r.ename : "ok");
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * store.md §7's other unwind: a note aborts a system call whether or
 * not a Tflush sent it, so a handler can be told `interrupted' by the
 * device with its queue's flush flag clear.  §7 puts both causes
 * through the whole of step 7, and the wire keeps them apart — the
 * flush answers `interrupted', the device answers under this server's
 * own prefix.  The store is not condemned by one (§0), so the next
 * verify of the same object succeeds.
 */
static void
tdevintr(void)
{
	char *m;
	uchar data[4096];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	char *w[1];
	uvlong n7, np, nd;
	int i;

	clstage = "devintr";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	srvauxpoint(ctx, 1);
	for(i = 0; i < sizeof data; i++)
		data[i] = (uchar)(0x31 + (i & 0x3f));
	mkobj(srvstore(ctx), "alpha", data, sizeof data, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach)
		fail("attach: %s", errof(&r));
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", errof(&r));
		goto Out;
	}
	simfault(d, Sfintr, 1);
	clwrite(&cl, Fctl, 0, "verify alpha", &r);
	eqs("a device interrupt with no flush pending", errof(&r),
		"shoalsrv: interrupted");
	srvauxcount(ctx, &n7, nil, nil);
	eqv("a device interrupt unwinds into step 7", n7, 1);
	eqv("step 7 ran before the reply", srvauxlate(ctx), 0);
	sleep(100);			/* the Req is freed after its reply */
	srvcount(ctx, &np, &nd);
	eqv("the interrupted request is counted complete", np - nd, 0);

	clwrite(&cl, Fctl, 0, "verify alpha", &r);
	checks++;
	if(r.type != Rwrite)
		fail("verify after a device interrupt: %s", errof(&r));
	srvauxcount(ctx, &n7, nil, nil);
	eqv("a request that was not interrupted runs no step 7", n7, 1);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The object rows' gate, layer-a §2.1 and §6.4: the operator rule and
 * its reserved-id exemption, and the fence with the one read §2.1 lets
 * through it.  Every answer below that is not a refusal is the local
 * `not built', because §2.4's content is the object-I/O surface's.
 */
static void
tobjgate(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	char *w[2];

	clstage = "objgate";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0, 1);
	mkobj(srvstore(ctx), "shoal.map.7", nil, 0, 1);
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach admin: %s", errof(&r));
		goto Out;
	}

	/* §2.1: role=admin's grant of /obj is read-only … */
	if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk)
		fail("walk /obj: %s", errof(&r));
	clcreate(&cl, Ffile, "brandnew", OWRITE, &r);
	eqs("admin create of an id that is not reserved", errof(&r),
		"permission denied");
	/* … except for §1.1's reserved ids, which it may create and write */
	clcreate(&cl, Ffile, "shoal.map.8", OWRITE, &r);
	eqs("admin create of a reserved id", errof(&r), "shoalsrv: not built");
	clclunk(&cl, Ffile, &r);

	w[0] = "obj";
	w[1] = "alpha";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
		fail("walk /obj/alpha: %s", errof(&r));
	clopen(&cl, Ffile, OWRITE, &r);
	eqs("admin open of an unreserved object for writing", errof(&r),
		"permission denied");
	clopen(&cl, Ffile, OREAD, &r);
	eqs("admin open of an unreserved object for reading", errof(&r),
		"shoalsrv: not built");
	clclunk(&cl, Ffile, &r);

	w[1] = "shoal.map.7";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
		fail("walk /obj/shoal.map.7: %s", errof(&r));
	clopen(&cl, Ffile, OWRITE, &r);
	eqs("admin open of a reserved object for writing", errof(&r),
		"shoalsrv: not built");
	clclunk(&cl, Ffile, &r);

	/* the fence, and §2.1's sole exemption from it */
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", errof(&r));
		goto Out;
	}
	if(clwrite(&cl, Fctl, 0, "fence on", &r) != Rwrite)
		fail("fence on: %s", errof(&r));
	w[0] = "obj";
	w[1] = "shoal.map.7";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
		fail("walk /obj/shoal.map.7 while fenced: %s", errof(&r));
	clopen(&cl, Ffile, OREAD, &r);
	eqs("fenced admin read of a reserved id", errof(&r),
		"shoalsrv: not built");
	clopen(&cl, Ffile, OWRITE, &r);
	eqs("fenced admin write of a reserved id", errof(&r), "fenced");
	clclunk(&cl, Ffile, &r);
	w[0] = "meta";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
		fail("walk /meta/shoal.map.7 while fenced: %s", errof(&r));
	clopen(&cl, Ffile, OREAD, &r);
	eqs("fenced admin read of a reserved id through /meta", errof(&r),
		"shoalsrv: not built");
	clclunk(&cl, Ffile, &r);
	w[0] = "obj";
	w[1] = "alpha";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
		fail("walk /obj/alpha while fenced: %s", errof(&r));
	clopen(&cl, Ffile, OREAD, &r);
	eqs("fenced admin read of an unreserved id", errof(&r), "fenced");
	clclunk(&cl, Ffile, &r);
	if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk)
		fail("walk /obj while fenced: %s", errof(&r));
	clcreate(&cl, Ffile, "shoal.map.9", OWRITE, &r);
	eqs("fenced admin create of a reserved id", errof(&r), "fenced");
	clclunk(&cl, Ffile, &r);
	if(clwrite(&cl, Fctl, 0, "fence off", &r) != Rwrite)
		fail("fence off: %s", errof(&r));
	clclunk(&cl, Fctl, &r);
	clclunk(&cl, Froot, &r);

	/* a client is not the operator: its writes are §2.4's, not §2.1's */
	if(clattach(&cl, Froot, "role=client,epoch=7", &r) != Rattach){
		fail("attach client: %s", errof(&r));
		goto Out;
	}
	if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk)
		fail("walk /obj as client: %s", errof(&r));
	clcreate(&cl, Ffile, "brandnew", OWRITE, &r);
	eqs("client create of an id that is not reserved", errof(&r),
		"shoalsrv: not built");
	clclunk(&cl, Ffile, &r);
	w[1] = "alpha";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
		fail("walk /obj/alpha as client: %s", errof(&r));
	clopen(&cl, Ffile, OWRITE, &r);
	eqs("client open of an object for writing", errof(&r),
		"shoalsrv: not built");
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §6.4 F3: an instance whose own map record says up=no or status=out
 * refuses role=client I/O with `down', and answers everything else as
 * it otherwise would.  The map is the static one (store.md §14(18)),
 * so this is the state the instance was started in.
 */
static void
tdown(char *status, char *up)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	char *w[2];

	clstage = "down";
	m = mkmapself(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid, status, up);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0, 1);
	clstart(&cl, ctx, Clmsize);

	/* the attach is not gated: §2.1 names no `down' among its refusals */
	if(clattach(&cl, Froot, "role=client,epoch=7", &r) != Rattach){
		fail("attach client on a %s/%s instance: %s", status, up,
			errof(&r));
		goto Out;
	}
	w[0] = "obj";
	w[1] = "alpha";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
		fail("walk /obj/alpha: %s", errof(&r));
	clopen(&cl, Ffile, OREAD, &r);
	eqs("a client read on an instance the map does not serve with",
		errof(&r), "down");
	clopen(&cl, Ffile, OWRITE, &r);
	eqs("a client write on an instance the map does not serve with",
		errof(&r), "down");
	clclunk(&cl, Ffile, &r);
	if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk)
		fail("walk /obj: %s", errof(&r));
	clcreate(&cl, Ffile, "brandnew", OWRITE, &r);
	eqs("a client create on an instance the map does not serve with",
		errof(&r), "down");
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot, &r);

	/* F3 is about serving clients: an operator still reaches the disk */
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach admin: %s", errof(&r));
		goto Out;
	}
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
		fail("walk /obj/alpha as admin: %s", errof(&r));
	clopen(&cl, Ffile, OREAD, &r);
	eqs("an admin read on the same instance", errof(&r),
		"shoalsrv: not built");
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * A fid's own state and its two hooks, through the fid-state point.
 * The close hook runs while the store is open — at a clunk, and at the
 * walk that moves a fid off the file whose state it is — and the free
 * hook runs last, which for a fid that outlives the service loop is
 * after the store has closed (D16, store.md §9).
 */
static void
tfidstate(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	uvlong nc, nf;

	clstage = "fidstate";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	srvauxpoint(ctx, 1);
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", errof(&r));
		goto Out;
	}
	srvauxcount(ctx, nil, &nc, &nf);
	eqv("a live fid's state is not given back", nc + nf, 0);

	clclunk(&cl, Froot, &r);
	srvauxcount(ctx, nil, &nc, &nf);
	eqv("a clunk closes the fid's state", nc, 1);
	eqv("a clunk frees the fid's state", nf, 1);

	/* a walk that moves a fid gives back what that fid was holding */
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", errof(&r));
		goto Out;
	}
	if(clwalk1(&cl, Froot, Froot, "ctl", &r) != Rwalk)
		fail("walk /ctl onto the same fid: %s", errof(&r));
	srvauxcount(ctx, nil, &nc, &nf);
	eqv("a walk that moves a fid closes the state it held", nc, 2);
	eqv("a walk that moves a fid frees the state it held", nf, 2);
	clclunk(&cl, Froot, &r);
	srvauxcount(ctx, nil, &nc, &nf);
	eqv("the moved fid had nothing left to close", nc, 2);
	eqv("the moved fid had nothing left to free", nf, 2);

	/* a fid still open when the loop ends: the store closes first */
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", errof(&r));
		goto Out;
	}
Out:
	clstop(&cl);
	srvauxcount(ctx, nil, &nc, &nf);
	eqv("a fid outliving the loop runs no close hook", nc, 2);
	eqv("a fid outliving the loop still runs its free hook", nf, 3);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * layer-a §5.4.1's Tflush, both halves: a request still queued is
 * removed and answered `interrupted', then the Rflush follows; a
 * request already running is interrupted, unwinds through step 7 and
 * answers, and the Rflush follows that.  One queue makes which is
 * which deterministic.
 *
 * Step 7 is observed through the fid-state point's flush hook, which
 * is the cell the object-I/O surface will discard its stage from: it
 * runs for the request that was running and not for the one that was
 * never started, and never after the reply.
 */
static void
tflush(void)
{
	char buf[8192], val[64], *m;
	uchar data[1024];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	char *w[1];
	uvlong n7, np, nd;
	ushort ta, tb, tf;

	clstage = "flush";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 1)) == nil)
		return;
	srvauxpoint(ctx, 1);
	memset(data, 0x71, sizeof data);
	mkobj(srvstore(ctx), "alpha", data, sizeof data, 1);
	mkobj(srvstore(ctx), "beta", data, sizeof data, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach)
		fail("attach: %s", r.type == Rerror ? r.ename : "?");
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", r.type == Rerror ? r.ename : "?");
		goto Out;
	}
	if(clwalk1(&cl, Froot, Fctl2, "ctl", &r) != Rwalk
	|| clopen(&cl, Fctl2, OWRITE, &r) != Ropen){
		fail("second /ctl fid: %s", r.type == Rerror ? r.ename : "?");
		goto Out;
	}

	/*
	 * One request that has been through the pool already, so that the
	 * depth below and the number of pushes are different numbers.
	 */
	clwrite(&cl, Fctl, 0, "verify alpha", &r);
	checks++;
	if(r.type != Rwrite)
		fail("verify before the hold: %s", errof(&r));
	sleep(100);			/* its Req is freed after its reply */

	/* a QUEUED request, flushed */
	srvhook(ctx, "objhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Fctl;
	t.offset = 0;
	t.data = "verify alpha";
	t.count = strlen(t.data);
	clput(&cl, &t);
	sleep(200);			/* it is now running, and held */
	t.tag = tb = cltag(&cl);
	t.fid = Fctl2;
	t.data = "verify beta";
	t.count = strlen(t.data);
	clput(&cl, &t);
	sleep(200);			/* ... and this one is behind it */

	/*
	 * The pool's depth, with one request running and one queued: what
	 * /status reports is the two of them and not the three that have
	 * been pushed since the server started (store.md §7).
	 */
	srvcount(ctx, &np, &nd);
	eqv("pushes counted while two requests are in flight", np, 3);
	eqv("completions counted while two requests are in flight", nd, 1);
	w[0] = "status";
	if(clopenpath(&cl, Froot, Ffile, 1, w, OREAD, &r) != Ropen)
		fail("open /status under the hold: %s", errof(&r));
	else{
		clslurp(&cl, Ffile, buf, sizeof buf);
		eqs("/status qdepth= under the hold",
			clfield(buf, "qdepth", val, sizeof val), "2");
		clclunk(&cl, Ffile, &r);
	}

	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = tb;
	clput(&cl, &t);

	clget(&cl, &r);
	checks++;
	if(r.type != Rerror || r.tag != tb
	|| strcmp(r.ename, "interrupted") != 0)
		fail("a flushed queued request: type %d tag %ud %s", r.type,
			r.tag, r.type == Rerror ? r.ename : "");
	clget(&cl, &r);
	checks++;
	if(r.type != Rflush || r.tag != tf)
		fail("the Rflush after it: type %d tag %ud", r.type, r.tag);

	srvhook(ctx, "objhold", 0);
	clget(&cl, &r);
	checks++;
	if(r.type != Rwrite || r.tag != ta)
		fail("the held request completes: type %d tag %ud", r.type,
			r.tag);
	srvauxcount(ctx, &n7, nil, nil);
	eqv("a request that never started runs no step 7", n7, 0);

	/* a RUNNING request, flushed */
	srvhook(ctx, "objhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Fctl;
	t.offset = 0;
	t.data = "verify alpha";
	t.count = strlen(t.data);
	clput(&cl, &t);
	sleep(200);
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = ta;
	clput(&cl, &t);

	clget(&cl, &r);
	checks++;
	if(r.type != Rerror || r.tag != ta
	|| strcmp(r.ename, "interrupted") != 0)
		fail("a flushed running request: type %d tag %ud %s", r.type,
			r.tag, r.type == Rerror ? r.ename : "");
	clget(&cl, &r);
	checks++;
	if(r.type != Rflush || r.tag != tf)
		fail("the Rflush after the running one: type %d tag %ud",
			r.type, r.tag);
	srvauxcount(ctx, &n7, nil, nil);
	eqv("a flushed running request runs step 7 once", n7, 1);
	eqv("step 7 ran before the reply", srvauxlate(ctx), 0);
	srvhook(ctx, "objhold", 0);

	/*
	 * A request flushed at the OTHER end of its handler: past the
	 * engine call, with its answer in hand.  Both of the handler's
	 * exits must still be the queue's one exit — the plain one, and
	 * the error API's, which a handler reaches with %r set.
	 */
	srvhook(ctx, "objexit", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Fctl;
	t.offset = 0;
	t.data = "verify alpha";
	t.count = strlen(t.data);
	clput(&cl, &t);
	sleep(200);			/* it is now held at its exit */
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = ta;
	clput(&cl, &t);
	clget(&cl, &r);
	checks++;
	if(r.type != Rerror || r.tag != ta
	|| strcmp(r.ename, "interrupted") != 0)
		fail("a request flushed at its exit: type %d tag %ud %s",
			r.type, r.tag, r.type == Rerror ? r.ename : "");
	clget(&cl, &r);
	checks++;
	if(r.type != Rflush || r.tag != tf)
		fail("the Rflush after the exit-flushed one: type %d tag %ud",
			r.type, r.tag);
	srvauxcount(ctx, &n7, nil, nil);
	eqv("a request flushed at its exit runs step 7", n7, 2);

	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Fctl;
	t.offset = 0;
	t.data = "verify nosuch";		/* the error exit */
	t.count = strlen(t.data);
	clput(&cl, &t);
	sleep(200);
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = ta;
	clput(&cl, &t);
	clget(&cl, &r);
	checks++;
	if(r.type != Rerror || r.tag != ta
	|| strcmp(r.ename, "interrupted") != 0)
		fail("a request flushed at its error exit: type %d tag %ud %s",
			r.type, r.tag, r.type == Rerror ? r.ename : "");
	clget(&cl, &r);
	checks++;
	if(r.type != Rflush || r.tag != tf)
		fail("the Rflush after the error exit: type %d tag %ud",
			r.type, r.tag);
	srvauxcount(ctx, &n7, nil, nil);
	eqv("a request flushed at its error exit runs step 7", n7, 3);
	eqv("no step 7 ran after its reply", srvauxlate(ctx), 0);
	srvhook(ctx, "objexit", 0);

	/* a Tflush naming a request answered on the loop is still answered */
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = 31337;
	clput(&cl, &t);
	clget(&cl, &r);
	checks++;
	if(r.type != Rflush || r.tag != tf)
		fail("a Tflush of an unknown tag: type %d", r.type);
	sleep(200);			/* the parked Rflushes have been answered */
	srvcount(ctx, &np, &nd);
	eqv("every request the flush cases pushed is accounted for",
		np - nd, 0);
	istrue("and the count is not zero either way", np > 0);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * An operation that names no object, run off the service loop.  The
 * status renders and the /obj directory read that are to come take
 * engine snapshots, and lib9p's loop is single-threaded, so they need
 * an offload that the pool's oid hash cannot give them.  The reserved
 * queue is that offload, and what this asks of it is that it is a
 * queue of its own: the pool here is ONE queue, so an object's verb
 * would wait behind the held request if the two shared one.
 */
static void
tanyq(void)
{
	char buf[8192], val[64], *m;
	uchar data[1024];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	char *w[1];
	uvlong np, nd;
	ushort ta, tf;
	long n;

	clstage = "anyq";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 1)) == nil)
		return;
	memset(data, 0x33, sizeof data);
	mkobj(srvstore(ctx), "alpha", data, sizeof data, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach)
		fail("attach: %s", errof(&r));
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", errof(&r));
		goto Out;
	}
	if(clwalk1(&cl, Froot, Ffile, "map", &r) != Rwalk){
		fail("walk /map: %s", errof(&r));
		goto Out;
	}
	srvhook(ctx, "mapopen", 1);
	memset(&t, 0, sizeof t);
	t.type = Topen;
	t.tag = ta = cltag(&cl);
	t.fid = Ffile;
	t.mode = OREAD;
	clput(&cl, &t);
	sleep(200);			/* pushed, and held on the reserved queue */
	srvcount(ctx, &np, &nd);
	eqv("a request that names no object is counted by the pool",
		np - nd, 1);

	/* the service loop is free, and so is the one queue the oids share */
	w[0] = "status";
	if(clopenpath(&cl, Froot, Ffile2, 1, w, OREAD, &r) != Ropen)
		fail("open /status while a request is held off the loop: %s",
			errof(&r));
	else{
		n = clslurp(&cl, Ffile2, buf, sizeof buf);
		istrue("/status renders while a request is held off the loop",
			n > 0);
		clclunk(&cl, Ffile2, &r);
	}
	clwrite(&cl, Fctl, 0, "verify alpha", &r);
	checks++;
	if(r.type != Rwrite)
		fail("an object's verb while a request is held off the loop: "
			"%s", errof(&r));
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = 31337;
	clput(&cl, &t);
	clget(&cl, &r);
	checks++;
	if(r.type != Rflush || r.tag != tf)
		fail("an unrelated Tflush while a request is held off the "
			"loop: type %d tag %ud", r.type, r.tag);

	/* and it is flushable, like every other pushed request */
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = ta;
	clput(&cl, &t);
	clget(&cl, &r);
	checks++;
	if(r.type != Rerror || r.tag != ta
	|| strcmp(r.ename, "interrupted") != 0)
		fail("a flushed request on the reserved queue: type %d tag "
			"%ud %s", r.type, r.tag, r.type == Rerror ? r.ename : "");
	clget(&cl, &r);
	checks++;
	if(r.type != Rflush || r.tag != tf)
		fail("the Rflush after it: type %d tag %ud", r.type, r.tag);
	clclunk(&cl, Ffile, &r);

	/* released rather than flushed, it answers what the loop would have */
	if(clwalk1(&cl, Froot, Ffile, "map", &r) != Rwalk)
		fail("walk /map again: %s", errof(&r));
	memset(&t, 0, sizeof t);
	t.type = Topen;
	t.tag = ta = cltag(&cl);
	t.fid = Ffile;
	t.mode = OREAD;
	clput(&cl, &t);
	sleep(200);
	srvhook(ctx, "mapopen", 0);
	clget(&cl, &r);
	checks++;
	if(r.type != Ropen || r.tag != ta)
		fail("an open answered off the loop: type %d tag %ud %s",
			r.type, r.tag, r.type == Rerror ? r.ename : "");
	else{
		n = clslurp(&cl, Ffile, buf, sizeof buf);
		eqv("it rendered the same bytes", n, strlen(m));
		istrue("which are the map text", strcmp(buf, m) == 0);
	}
	clclunk(&cl, Ffile, &r);
	sleep(100);
	srvcount(ctx, &np, &nd);
	eqv("the pool is empty again", np - nd, 0);

	/*
	 * A caller that prepares a request for a queue and then answers
	 * it on the loop after all: the pool must count that request the
	 * same way, since the completion is counted from the Req being
	 * armed.  A depth that went below zero here would print as
	 * 2^64-1 and the shutdown's drain would never converge.
	 */
	srvhook(ctx, "mapopen", 2);
	if(clwalk1(&cl, Froot, Ffile, "map", &r) != Rwalk)
		fail("walk /map a third time: %s", errof(&r));
	clopen(&cl, Ffile, OREAD, &r);
	checks++;
	if(r.type != Ropen)
		fail("an open prepared for a queue and answered on the loop: "
			"%s", errof(&r));
	clclunk(&cl, Ffile, &r);
	srvhook(ctx, "mapopen", 0);
	sleep(100);
	srvcount(ctx, &np, &nd);
	eqv("a preparation without a push leaves the pool empty", np - nd, 0);
	w[0] = "status";
	if(clopenpath(&cl, Froot, Ffile2, 1, w, OREAD, &r) != Ropen)
		fail("open /status: %s", errof(&r));
	else{
		clslurp(&cl, Ffile2, buf, sizeof buf);
		eqs("/status qdepth= after it",
			clfield(buf, "qdepth", val, sizeof val), "0");
		clclunk(&cl, Ffile2, &r);
	}
Out:
	srvhook(ctx, "mapopen", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * A Tflush that loses the race to the request it names.  lib9p's
 * reqqueueflush answers a request it does not find running — whether
 * or not it found it queued either — and lib9p's respond asserts that
 * a request has not answered before, so a flush arriving after the
 * queue proc finished the request would abort the whole server.  The
 * window is forced rather than raced for: the point parks the service
 * loop between the lookup and the flush, and the held request is
 * released inside it.
 */
static void
tflushrace(void)
{
	char *m;
	uchar data[1024];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	char *w[1];
	ushort ta, tf;

	clstage = "flushrace";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 1)) == nil)
		return;
	memset(data, 0x44, sizeof data);
	mkobj(srvstore(ctx), "alpha", data, sizeof data, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach)
		fail("attach: %s", errof(&r));
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", errof(&r));
		goto Out;
	}
	srvhook(ctx, "objhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Fctl;
	t.offset = 0;
	t.data = "verify alpha";
	t.count = strlen(t.data);
	clput(&cl, &t);
	sleep(200);			/* running, and held */
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = ta;
	srvhook(ctx, "flushhold", 1);
	clput(&cl, &t);
	sleep(200);			/* the loop holds the flush it looked up */
	srvhook(ctx, "objhold", 0);	/* ... and now the request answers */
	clget(&cl, &r);
	checks++;
	if(r.type != Rwrite || r.tag != ta)
		fail("the request the flush lost to: type %d tag %ud %s",
			r.type, r.tag, r.type == Rerror ? r.ename : "");
	sleep(200);			/* its queue proc is past it now */
	srvhook(ctx, "flushhold", 0);
	clget(&cl, &r);
	checks++;
	if(r.type != Rflush || r.tag != tf)
		fail("the Rflush for a request that had answered: type %d "
			"tag %ud", r.type, r.tag);
	/* and the server is still serving */
	clwrite(&cl, Fctl, 0, "verify alpha", &r);
	checks++;
	if(r.type != Rwrite)
		fail("verify after the lost flush: %s", errof(&r));
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * store.md §3.7's mapping rule.  The two halves are checked where each
 * lives: the classifier, over the strings the engine actually
 * produces, and the wire, over an internal error driven through a
 * handler.
 */
static void
terrors(void)
{
	char buf[ERRMAX], *m;
	static char *wire[] = {
		"no such object",
		"object deleted",
		"disk full: 8 object snapshots open, objsnapmax 8",
		"not primary: n5.0",
		"checksum mismatch",
		"bad ctl",
	};
	static char *internal[] = {
		"store closed",
		"store condemned: a log write failed",
		"i/o error",
		"stage expired",
		"Eobj: slot 3 out of range",
		"out of memory",
		"no such objects here",		/* a prefix is not a match */
		"bad ctlx",
	};
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	char *w[1];
	int i;

	clstage = "errors";
	for(i = 0; i < nelem(wire); i++){
		eqs("a §2.6 string passes verbatim",
			srverrs(buf, sizeof buf, wire[i]), wire[i]);
		checks++;
		if(srv26(wire[i]) == nil)
			fail("%#q is not recognised as §2.6's", wire[i]);
	}
	for(i = 0; i < nelem(internal); i++){
		checks++;
		if(srv26(internal[i]) != nil)
			fail("%#q was taken for a §2.6 condition", internal[i]);
		srverrs(buf, sizeof buf, internal[i]);
		checks++;
		if(srv26(buf) != nil)
			fail("%#q was answered as %#q, which carries a §2.6 "
				"prefix", internal[i], buf);
		checks++;
		if(strncmp(buf, "shoalsrv: ", 10) != 0)
			fail("%#q was answered as %#q", internal[i], buf);
	}
	eqs("a marked string is not marked twice",
		srverrs(buf, sizeof buf, "shoalsrv: not built"),
		"shoalsrv: not built");

	/*
	 * And on the wire: a store condemned under §3.2 answers the
	 * engine's own internal string, which must reach the client
	 * carrying no §2.6 prefix.
	 */
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0, 1);
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach)
		fail("attach: %s", r.type == Rerror ? r.ename : "?");
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen)
		fail("open /ctl: %s", r.type == Rerror ? r.ename : "?");
	else{
		storehook(srvstore(ctx), "fatal", 1);
		clwrite(&cl, Fctl, 0, "verify alpha", &r);
		checks++;
		if(r.type != Rerror)
			fail("verify on a condemned store: no error");
		else{
			istrue("an internal error carries no §2.6 prefix",
				srv26(r.ename) == nil);
			istrue("an internal error is marked as this server's",
				strncmp(r.ename, "shoalsrv: ", 10) == 0);
		}
		storehook(srvstore(ctx), "fatal", 0);
	}
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * D16's shutdown order: stop accepting, drain what is in flight, stop
 * the loop, then close the store.  The engine's `freed' callback is
 * the only observable of the close, and what it is asked here is
 * whether anything was still in flight when it ran.
 */
static void
tshutdown(void)
{
	char *m;
	uchar data[1024];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	char *w[1];
	ushort ta;

	clstage = "shutdown";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	freedseen = 0;
	freedqdepth = -1;
	if((ctx = startsrv(d, m, 1)) == nil)
		return;
	memset(data, 0x22, sizeof data);
	mkobj(srvstore(ctx), "alpha", data, sizeof data, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach)
		fail("attach: %s", r.type == Rerror ? r.ename : "?");
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", r.type == Rerror ? r.ename : "?");
		clstop(&cl);
		srvfree(ctx);
		devclose(d);
		free(m);
		return;
	}
	srvhook(ctx, "objhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Fctl;
	t.data = "verify alpha";
	t.count = strlen(t.data);
	clput(&cl, &t);
	sleep(200);			/* running, and held */

	clhangup(&cl);			/* the loop ends with it in flight */
	sleep(200);
	istrue("the store is not closed while a request is in flight",
		freedseen == 0);
	srvhook(ctx, "objhold", 0);
	istrue("the service loop ends", clwaitend(&cl, 10000));
	clget(&cl, &r);
	checks++;
	if(r.type != Rwrite || r.tag != ta)
		fail("the drained request answered: type %d tag %ud", r.type,
			r.tag);
	eqv("the store was closed once", freedseen, 1);
	eqv("nothing was in flight when the store was freed",
		freedqdepth, 0);
	close(cl.rfd);
	close(cl.sin);
	close(cl.sout);
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
	clwatchon();

	tstartup();
	tattach();
	tmsize();
	tmatrix();
	tmodes();
	tobjects();
	tstatus();
	tctl();
	tverify();
	tdevintr();
	tobjgate();
	tdown("in", "no");
	tdown("out", "yes");
	tfidstate();
	tflush();
	tanyq();
	tflushrace();
	terrors();
	tshutdown();

	clwatchoff();
	if(fails > 0){
		fprint(2, "srvtest: %d of %d checks failed\n", fails, checks);
		threadexitsall("fail");
	}
	print("srvtest: %d checks ok\n", checks);
	threadexitsall(nil);
}
