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

/*
 * What a role=client operation on this id is answered with.  §5.1 and
 * §5.4 step 1 serve a client only from the object's serving primary,
 * and this map places with two instances on one node, so which of them
 * an id lands on is HRW's answer (§4.3) and not this case's: an id
 * placed elsewhere is layer-a §2.6's `not primary' with that iid in
 * the detail, and one placed here is served.
 */
static char*
clientwant(Srvctx *ctx, char *oid, char *buf, int nbuf)
{
	Cinst *p;

	p = mapprimary(srvmap(ctx), oid);
	if(p != nil && strcmp(p->iid, srviid(ctx)) == 0)
		return "ok";
	snprint(buf, nbuf, "not primary: %s", p != nil ? p->iid : "");
	return buf;
}

/*
 * Two ids the map places differently: one this instance is the serving
 * primary for and one it is not.  A case that runs a role=client row
 * against both takes both of clientwant's answers, which is what makes
 * the row's `ok' half a driven path rather than an assumption about
 * where blake2s happens to send one name.  `pfx' is so that a case
 * wanting ids for a create and ids for an existing object gets two
 * disjoint pairs.  Both nil on failure, which is a failed check.
 */
static void
placeids(Srvctx *ctx, char *pfx, char **mine, char **theirs)
{
	char name[32];
	Cinst *p;
	int i;

	*mine = *theirs = nil;
	for(i = 0; i < 64 && (*mine == nil || *theirs == nil); i++){
		snprint(name, sizeof name, "%s%d", pfx, i);
		if((p = mapprimary(srvmap(ctx), name)) == nil)
			continue;
		if(strcmp(p->iid, srviid(ctx)) == 0){
			if(*mine == nil)
				*mine = strdup(name);
		}else if(*theirs == nil)
			*theirs = strdup(name);
	}
	checks++;
	if(*mine == nil || *theirs == nil){
		fail("the map places every `%s' id the same way: no case to"
			" drive", pfx);
		free(*mine);
		free(*theirs);
		*mine = *theirs = nil;
	}
}

/* how many directory entries a read answered */
static int
dirents(char *p, long n)
{
	Dir dir;
	char *ep;
	int m, k;

	k = 0;
	ep = p + n;
	while(p < ep){
		m = convM2D((uchar*)p, ep-p, &dir, p+BIT16SZ);
		if(m <= BIT16SZ)
			break;
		p += m;
		k++;
	}
	return k;
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
		/* leading zeros are part of the number: this is epoch 7 */
		{"epoch=007",			nil},
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
			  {"permission denied", "permission denied", nil}},
		{"meta",  {nil, nil, nil},
			  {"permission denied", "permission denied", nil}},
		{"repl",  {"permission denied", nil, "permission denied"},
			  {nil, "shoalsrv: not built", nil}},
		{"rpc",	  {"permission denied", nil, nil},
			  {nil, "shoalsrv: not built", "shoalsrv: not built"}},
		{"advert",{"permission denied", nil, "permission denied"},
			  {nil, nil, nil}},
		{"dirty", {"permission denied", "permission denied", nil},
			  {nil, nil, nil}},
		{"stale", {"permission denied", "permission denied", nil},
			  {nil, nil, nil}},
		{"tombs", {"permission denied", "permission denied", nil},
			  {nil, nil, nil}},
		{"lost",  {"permission denied", "permission denied", nil},
			  {nil, nil, nil}},
		{"jobs",  {"permission denied", "permission denied", nil},
			  {nil, nil, nil}},
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
 * Each is gated on the row's write column; what a role=client
 * operation is then answered is §2.4's own and depends on where the
 * map places the id (clientwant), while the other two roles never
 * reach the content at all.  Each operation therefore runs on an id
 * this instance is the serving primary for and on one it is not, so
 * both of clientwant's answers are on the wire.  The roles run in
 * reverse order because the client's half may remove the object the
 * other two walk to.
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
		nil,			/* clientwant's, per id */
		"permission denied",
		"permission denied",
	};
	char what[96], buf[64], *m, *mine, *theirs, *newmine, *newtheirs;
	char *id, *newid;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	Dir dir;
	int role, k;

	clstage = "modes";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	/*
	 * Each operation is run on an id this instance is the serving
	 * primary for and on one it is not, so the client row takes both
	 * of clientwant's answers rather than whichever one the hash
	 * happens to give one name.
	 */
	placeids(ctx, "obj", &mine, &theirs);
	placeids(ctx, "new", &newmine, &newtheirs);
	if(mine == nil || newmine == nil)
		goto Out;
	mkobj(srvstore(ctx), mine, nil, 0, 1);
	mkobj(srvstore(ctx), theirs, nil, 0, 1);
	clstart(&cl, ctx, Clmsize);
	for(role = 2; role >= 0; role--){
		if(clattach(&cl, Froot, anames[role], &r) != Rattach){
			fail("attach %s: %s", anames[role],
				r.type == Rerror ? r.ename : "?");
			continue;
		}
		for(k = 0; k < 2; k++){
			id = k == 0 ? mine : theirs;
			newid = k == 0 ? newmine : newtheirs;
			/* Tcreate in /obj */
			if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk)
				fail("walk /obj: %s", clerr(&r));
			clcreate(&cl, Ffile, newid, 0666, OWRITE, &r);
			snprint(what, sizeof what, "create %s in /obj as %s",
				newid, anames[role]);
			clerris(what, &r, role == 0 ?
				clientwant(ctx, newid, buf, sizeof buf) :
				want[role]);
			clclunk(&cl, Ffile, &r);

			/*
			 * Twstat and Tremove on /obj/<oid>.  A Tremove clunks
			 * its fid whether or not it removes anything, so Ffile2
			 * is free again for the next walk.
			 */
			if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk
			|| clwalk1(&cl, Ffile, Ffile2, id, &r) != Rwalk)
				fail("walk /obj/%s: %s", id, clerr(&r));
			nulldir(&dir);
			dir.length = 4096;
			clwstat(&cl, Ffile2, &dir, &r);
			snprint(what, sizeof what, "wstat /obj/%s as %s", id,
				anames[role]);
			clerris(what, &r, role == 0 ?
				clientwant(ctx, id, buf, sizeof buf) :
				want[role]);
			clremove(&cl, Ffile2, &r);
			snprint(what, sizeof what, "remove /obj/%s as %s", id,
				anames[role]);
			clerris(what, &r, role == 0 ?
				clientwant(ctx, id, buf, sizeof buf) :
				want[role]);
			clclunk(&cl, Ffile, &r);
		}
		clclunk(&cl, Froot, &r);
	}
	clstop(&cl);
Out:
	free(mine);
	free(theirs);
	free(newmine);
	free(newtheirs);
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
	clerris("walk to an absent id", &r, "no such object");
	clwalk1(&cl, Ffile, Ffile2, "not!a!name", &r);
	clerris("walk to a bad id", &r, "bad object name");
	/*
	 * An over-long id names no object, so it is not ordered against
	 * one either: the pushed count does not move.  Cutting it to
	 * Oidmax would put the walk on the queue of whatever object its
	 * first 128 bytes name.
	 */
	srvcount(ctx, &np, &nd);
	clwalk1(&cl, Ffile, Ffile2, longname, &r);
	clerris("walk to an over-long id", &r, "bad object name");
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
	clerris("walk to a tombstone", &r, "object deleted");
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

/*
 * A row that renders at open AND takes its own reads.  dat.h gives
 * the read cell precedence: such a row gets every read, and may serve
 * the rendered bytes itself with textread; only a row with a render
 * cell and no read cell takes the automatic text path.  /ctl is both
 * here, through the cell point, and its own render is empty -- so the
 * bytes a read answers say which of the two served it.
 */
static void
treadcell(void)
{
	char buf[256], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	char *w[1];
	long n;

	clstage = "readcell";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	srvcellpoint(ctx, 1);
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OREAD, &r) != Ropen)
		fail("open /ctl for reading: %s", clerr(&r));
	else{
		n = clslurp(&cl, Fctl, buf, sizeof buf);
		istrue("a row with both cells serves its reads from the read one",
			n > 0);
		if(n > 0)
			eqs("... and not from the text its open rendered", buf,
				"cell\n");
		clclunk(&cl, Fctl, &r);
	}
	clclunk(&cl, Froot, &r);
Out:
	srvcellpoint(ctx, 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The cell point goes with the context that set it.  Its cells are the
 * file table's, and the table is the program's, so a point left on
 * when a context ends would hand its cells to the next server started
 * in the same program.  The shutdown is where they go, with the holds:
 * it is after the service loop and the drain, so nothing can still be
 * inside one.  A second server over the same disk is what sees the
 * difference — a read of its /ctl is served from the row's own
 * rendered text, which is empty, and not from a dead context's cell.
 */
static void
tcellclear(void)
{
	char buf[256], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	char *w[1];
	long n;

	clstage = "cellclear";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil){
		devclose(d);
		free(m);
		return;
	}
	srvcellpoint(ctx, 1);
	clstart(&cl, ctx, Clmsize);
	clstop(&cl);			/* the hangup is the shutdown's trigger */
	srvfree(ctx);

	if((ctx = startsrv(d, m, 4)) == nil){
		devclose(d);
		free(m);
		return;
	}
	clstart(&cl, ctx, Clmsize);
	w[0] = "ctl";
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach)
		fail("attach to the second server: %s", clerr(&r));
	else if(clopenpath(&cl, Froot, Fctl, 1, w, OREAD, &r) != Ropen)
		fail("open /ctl on the second server: %s", clerr(&r));
	else{
		n = clslurp(&cl, Fctl, buf, sizeof buf);
		eqv("a server started after a shutdown carries no cells of its"
			" own", n, 0);
		clclunk(&cl, Fctl, &r);
	}
	srvcellpoint(ctx, 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * Which refusal `drop <oid>' answers for an id this store does not
 * hold.  §7.4's guard runs before the engine's drop, so an id this
 * instance is in P(o) for is `still placed' and one it is not is `no
 * such object'.  Which of the two a given id gets is the map's to
 * say, so the expected string is computed from the same map the
 * server adopted rather than guessed.
 *
 * It is computed with mapplace, which is the function the server's
 * own guard calls, so this case does not check the placement: it
 * checks what the verb does on each side of it.  That is deliberate
 * and not an oversight.  `maptest' is where placement is checked, at
 * known-answer vectors computed outside this codebase (AGENTS.md), and
 * a case that recomputed it here would be asserting the server against
 * itself and would pass whatever mapplace answered.
 */
static char*
droperr(Srvctx *ctx, char *oid)
{
	Cinst *pl[Maxplace];
	int i, n;

	n = mapplace(srvmap(ctx), oid, pl, nelem(pl));
	if(n > nelem(pl))
		n = nelem(pl);
	for(i = 0; i < n; i++)
		if(strcmp(pl[i]->iid, srviid(ctx)) == 0)
			return "still placed";
	return "no such object";
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
		{"drop dropprobe", nil,			"fenced"},
		{"verify alpha", nil,			nil},
		{"scrub",	nil,			nil},
		{"forget n1.1",	nil,			"fenced"},
		{"newmonid 00112233445566778899aabbccddeeff",
				nil,			nil},
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
	char *m, oid[Oidmax+2], line[Oidmax+16], what[Oidmax+32];
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
	for(i = 0; i < nelem(verbs); i++)
		if(strncmp(verbs[i].line, "drop ", 5) == 0)
			verbs[i].unfenced = droperr(ctx, verbs[i].line+5);
	clstart(&cl, ctx, Clmsize);

	/* every verb is refused a non-admin fid (§2.5's role column) */
	if(clattach(&cl, Froot, "role=client,epoch=7", &r) != Rattach)
		fail("attach client: %s", r.type == Rerror ? r.ename : "?");
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen)
		fail("client open /ctl: %s", r.type == Rerror ? r.ename : "?");
	else for(i = 0; i < nelem(verbs); i++){
		clwrite(&cl, Fctl, 0, verbs[i].line, &r);
		snprint(what, sizeof what, "%#q as client", verbs[i].line);
		clerris(what, &r, "permission denied");
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
		snprint(what, sizeof what, "%#q", bad[i].line);
		clerris(what, &r, bad[i].err);
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
	clerris("verify of a 129-byte id", &r, "bad object name");

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
	clerris("open an object while fenced", &r, "fenced");
	clclunk(&cl, Ffile2, &r);
	clclunk(&cl, Ffile, &r);

	/* F4 is clearable; a lease fence would not be (§2.5, D25) */
	clwrite(&cl, Fctl, 0, "fence off", &r);
	checks++;
	if(r.type != Rwrite)
		fail("fence off under an operator fence: %s",
			r.type == Rerror ? r.ename : "?");
	clwrite(&cl, Fctl, 0, "pull alpha n1.1", &r);
	clerris("a fenced-set verb after fence off", &r,
		"shoalsrv: not built");
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
	clerris("verify an absent object", &r, "no such object");

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
	clerris("verify a damaged object", &r, "checksum mismatch");
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
		fail("attach: %s", clerr(&r));
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", clerr(&r));
		goto Out;
	}
	simfault(d, Sfintr, 1);
	clwrite(&cl, Fctl, 0, "verify alpha", &r);
	clerris("a device interrupt with no flush pending", &r,
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
		fail("verify after a device interrupt: %s", clerr(&r));
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
 * through it.  What the gate lets through is §2.4's content, which
 * answers it: this case is about which operations reach that far, and
 * srviotest is about what they do when they get there.
 */
static void
tobjgate(void)
{
	char cbuf[64], what[96], *m, *mine, *theirs, *newmine, *newtheirs;
	char *id, *newid;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	char *w[2];
	int k;

	clstage = "objgate";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0, 1);
	mkobj(srvstore(ctx), "shoal.map.7", nil, 0, 1);
	/* the client half below runs on one id placed here and one not */
	placeids(ctx, "obj", &mine, &theirs);
	placeids(ctx, "new", &newmine, &newtheirs);
	if(mine == nil || newmine == nil){
		srvfree(ctx);
		devclose(d);
		free(m);
		return;
	}
	mkobj(srvstore(ctx), mine, nil, 0, 1);
	mkobj(srvstore(ctx), theirs, nil, 0, 1);
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach admin: %s", clerr(&r));
		goto Out;
	}

	/* §2.1: role=admin's grant of /obj is read-only … */
	if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk)
		fail("walk /obj: %s", clerr(&r));
	clcreate(&cl, Ffile, "brandnew", 0666, OWRITE, &r);
	clerris("admin create of an id that is not reserved", &r,
		"permission denied");
	/* … except for §1.1's reserved ids, which it may create and write */
	clcreate(&cl, Ffile, "shoal.map.8", 0666, OWRITE, &r);
	clerris("admin create of a reserved id", &r, "ok");
	clclunk(&cl, Ffile, &r);

	w[0] = "obj";
	w[1] = "alpha";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
		fail("walk /obj/alpha: %s", clerr(&r));
	clopen(&cl, Ffile, OWRITE, &r);
	clerris("admin open of an unreserved object for writing", &r,
		"permission denied");
	/*
	 * ORCLOSE is the remove §2.1 refuses, one message earlier — asked
	 * on a fid of its own, because an open that succeeds leaves the
	 * fid open and 9P admits no second open of one.
	 */
	if(clwalk(&cl, Froot, Ffile2, 2, w, &r) != Rwalk)
		fail("walk /obj/alpha again: %s", clerr(&r));
	clopen(&cl, Ffile2, OREAD|ORCLOSE, &r);
	clerris("admin open of an unreserved object for reading with ORCLOSE",
		&r, "permission denied");
	clclunk(&cl, Ffile2, &r);
	clopen(&cl, Ffile, OREAD, &r);
	clerris("admin open of an unreserved object for reading", &r, "ok");
	clclunk(&cl, Ffile, &r);

	w[1] = "shoal.map.7";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
		fail("walk /obj/shoal.map.7: %s", clerr(&r));
	clopen(&cl, Ffile, OWRITE, &r);
	clerris("admin open of a reserved object for writing", &r, "ok");
	clclunk(&cl, Ffile, &r);

	/* the fence, and §2.1's sole exemption from it */
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", clerr(&r));
		goto Out;
	}
	if(clwrite(&cl, Fctl, 0, "fence on", &r) != Rwrite)
		fail("fence on: %s", clerr(&r));
	w[0] = "obj";
	w[1] = "shoal.map.7";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
		fail("walk /obj/shoal.map.7 while fenced: %s", clerr(&r));
	if(clwalk(&cl, Froot, Ffile2, 2, w, &r) != Rwalk)
		fail("walk /obj/shoal.map.7 again: %s", clerr(&r));
	clopen(&cl, Ffile2, OWRITE, &r);
	clerris("fenced admin write of a reserved id", &r, "fenced");
	clclunk(&cl, Ffile2, &r);
	clopen(&cl, Ffile, OREAD, &r);
	clerris("fenced admin read of a reserved id", &r, "ok");
	clclunk(&cl, Ffile, &r);
	w[0] = "meta";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
		fail("walk /meta/shoal.map.7 while fenced: %s", clerr(&r));
	clopen(&cl, Ffile, OREAD, &r);
	clerris("fenced admin read of a reserved id through /meta", &r, "ok");
	clclunk(&cl, Ffile, &r);
	w[0] = "obj";
	w[1] = "alpha";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
		fail("walk /obj/alpha while fenced: %s", clerr(&r));
	clopen(&cl, Ffile, OREAD, &r);
	clerris("fenced admin read of an unreserved id", &r, "fenced");
	clclunk(&cl, Ffile, &r);
	if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk)
		fail("walk /obj while fenced: %s", clerr(&r));
	/*
	 * A listing is not a read of an object, so F1 does not fence it.
	 * It gets a fid of its own because the open succeeds, and 9P has
	 * no Tcreate on a fid that is already open.
	 */
	if(clwalk1(&cl, Froot, Ffile2, "obj", &r) != Rwalk)
		fail("walk /obj for a listing while fenced: %s", clerr(&r));
	clopen(&cl, Ffile2, OREAD, &r);
	checks++;
	if(r.type != Ropen)
		fail("fenced admin open of the /obj directory: %s", clerr(&r));
	clclunk(&cl, Ffile2, &r);
	clcreate(&cl, Ffile, "shoal.map.9", 0666, OWRITE, &r);
	clerris("fenced admin create of a reserved id", &r, "fenced");
	/*
	 * The order of the gate's rules is on the wire.  §2.1's operator
	 * rule is asked first, so an operation it forbids is `permission
	 * denied' whatever the fence says: the fence is a state that
	 * moves, and the name and the role are not.
	 */
	clcreate(&cl, Ffile, "brandnew", 0666, OWRITE, &r);
	clerris("fenced admin create of an id that is not reserved", &r,
		"permission denied");
	clclunk(&cl, Ffile, &r);
	/* the /meta directory is the same listing under a second name */
	if(clwalk1(&cl, Froot, Ffile, "meta", &r) != Rwalk)
		fail("walk /meta while fenced: %s", clerr(&r));
	clopen(&cl, Ffile, OREAD, &r);
	checks++;
	if(r.type != Ropen)
		fail("fenced admin open of the /meta directory: %s", clerr(&r));
	clclunk(&cl, Ffile, &r);
	if(clwrite(&cl, Fctl, 0, "fence off", &r) != Rwrite)
		fail("fence off: %s", clerr(&r));
	clclunk(&cl, Fctl, &r);
	clclunk(&cl, Froot, &r);

	/* a client is not the operator: its writes are §2.4's, not §2.1's */
	if(clattach(&cl, Froot, "role=client,epoch=7", &r) != Rattach){
		fail("attach client: %s", clerr(&r));
		goto Out;
	}
	w[0] = "obj";
	for(k = 0; k < 2; k++){
		id = k == 0 ? mine : theirs;
		newid = k == 0 ? newmine : newtheirs;
		if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk)
			fail("walk /obj as client: %s", clerr(&r));
		clcreate(&cl, Ffile, newid, 0666, OWRITE, &r);
		snprint(what, sizeof what, "client create of %s", newid);
		clerris(what, &r, clientwant(ctx, newid, cbuf, sizeof cbuf));
		clclunk(&cl, Ffile, &r);
		w[1] = id;
		if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
			fail("walk /obj/%s as client: %s", id, clerr(&r));
		clopen(&cl, Ffile, OWRITE, &r);
		snprint(what, sizeof what, "client open of %s for writing", id);
		clerris(what, &r, clientwant(ctx, id, cbuf, sizeof cbuf));
		clclunk(&cl, Ffile, &r);
	}
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	free(mine);
	free(theirs);
	free(newmine);
	free(newtheirs);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §6.4 F1 fences OPERATIONS rather than opens — "every role=client
 * read and write, every role=repl and role=admin read of an object
 * through /obj or /meta, every /repl and /rpc operation" — and the
 * operator fence can go on while a fid is open.  So a Tread and a
 * Twrite are gated by the row exactly as an open is, and the two
 * channel rows are gated at all.  The write the fence refuses here is
 * one that succeeds without it, which is §2.4's own write cell.
 */
static void
tiogate(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	char *w[2];

	clstage = "iogate";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0, 1);
	mkobj(srvstore(ctx), "shoal.map.7", nil, 0, 1);
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach admin: %s", clerr(&r));
		goto Out;
	}
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", clerr(&r));
		goto Out;
	}

	/* a write fid on a reserved id and a read fid on an ordinary one */
	w[0] = "obj";
	w[1] = "shoal.map.7";
	if(clopenpath(&cl, Froot, Ffile, 2, w, OWRITE, &r) != Ropen){
		fail("open /obj/shoal.map.7 for writing: %s", clerr(&r));
		goto Out;
	}
	clwrite(&cl, Ffile, 0, "bytes", &r);
	checks++;
	if(r.type != Rwrite)
		fail("an admin write of a reserved id while unfenced: %s",
			clerr(&r));
	w[1] = "alpha";
	if(clopenpath(&cl, Froot, Ffile2, 2, w, OREAD, &r) != Ropen)
		fail("open /obj/alpha for reading: %s", clerr(&r));
	clread(&cl, Ffile2, 0, 16, &r);
	clerris("an admin read of an object while unfenced", &r, "ok");

	if(clwrite(&cl, Fctl, 0, "fence on", &r) != Rwrite)
		fail("fence on: %s", clerr(&r));
	clwrite(&cl, Ffile, 0, "bytes", &r);
	clerris("a write on a fid opened before the fence", &r, "fenced");
	clread(&cl, Ffile2, 0, 16, &r);
	clerris("a read on a fid opened before the fence", &r, "fenced");
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Ffile2, &r);
	clclunk(&cl, Fctl, &r);
	clclunk(&cl, Froot, &r);

	/* the channels are in F1's list; F3's `down' is not theirs */
	if(clattach(&cl, Froot2, "role=repl,peer=n1.1", &r) != Rattach){
		fail("attach repl: %s", clerr(&r));
		goto Out;
	}
	w[0] = "repl";
	clopenpath(&cl, Froot2, Ffile, 1, w, ORDWR, &r);
	clerris("a repl open of /repl while fenced", &r, "fenced");
	w[0] = "rpc";
	clopenpath(&cl, Froot2, Ffile2, 1, w, ORDWR, &r);
	clerris("a repl open of /rpc while fenced", &r, "fenced");
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Ffile2, &r);
	clclunk(&cl, Froot2, &r);
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
	char *w[2], *w2[1];

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
			clerr(&r));
		goto Out;
	}
	w[0] = "obj";
	w[1] = "alpha";
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
		fail("walk /obj/alpha: %s", clerr(&r));
	clopen(&cl, Ffile, OREAD, &r);
	clerris("a client read on an instance the map does not serve with",
		&r, "down");
	clopen(&cl, Ffile, OWRITE, &r);
	clerris("a client write on an instance the map does not serve with",
		&r, "down");
	clclunk(&cl, Ffile, &r);
	if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk)
		fail("walk /obj: %s", clerr(&r));
	clcreate(&cl, Ffile, "brandnew", 0666, OWRITE, &r);
	clerris("a client create on an instance the map does not serve with",
		&r, "down");
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot, &r);

	/* F3 is about serving clients: an operator still reaches the disk */
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach admin: %s", clerr(&r));
		goto Out;
	}
	if(clwalk(&cl, Froot, Ffile, 2, w, &r) != Rwalk)
		fail("walk /obj/alpha as admin: %s", clerr(&r));
	clopen(&cl, Ffile, OREAD, &r);
	clerris("an admin read on the same instance", &r, "ok");
	clclunk(&cl, Ffile, &r);

	/*
	 * F3 and the fence are two MUSTs over one operation once the
	 * instance is both: the gate asks F3 first, so a client read on a
	 * down AND fenced instance is `down' (store.md §14(24)).
	 */
	w2[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w2, OWRITE, &r) != Ropen)
		fail("open /ctl as admin: %s", clerr(&r));
	else if(clwrite(&cl, Fctl, 0, "fence on", &r) != Rwrite)
		fail("fence on: %s", clerr(&r));
	clclunk(&cl, Fctl, &r);
	clclunk(&cl, Froot, &r);
	if(clattach(&cl, Froot2, "role=client,epoch=7", &r) != Rattach){
		fail("attach client while fenced: %s", clerr(&r));
		goto Out;
	}
	if(clwalk(&cl, Froot2, Ffile, 2, w, &r) != Rwalk)
		fail("walk /obj/alpha while fenced: %s", clerr(&r));
	clopen(&cl, Ffile, OREAD, &r);
	clerris("a client read on an instance that is both down and fenced",
		&r, "down");
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot2, &r);
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
	Fcall t, r;
	uvlong nc, nf;

	clstage = "fidstate";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	freedseen = 0;
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	srvauxpoint(ctx, 1);
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
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
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalk1(&cl, Froot, Froot, "ctl", &r) != Rwalk)
		fail("walk /ctl onto the same fid: %s", clerr(&r));
	srvauxcount(ctx, nil, &nc, &nf);
	eqv("a walk that moves a fid closes the state it held", nc, 2);
	eqv("a walk that moves a fid frees the state it held", nf, 2);
	clclunk(&cl, Froot, &r);
	srvauxcount(ctx, nil, &nc, &nf);
	eqv("the moved fid had nothing left to close", nc, 2);
	eqv("the moved fid had nothing left to free", nf, 2);

	/*
	 * A fid still open when the connection drops.  Its state is given
	 * back in two halves and the shutdown is what separates them: the
	 * close hook runs in the shutdown's sweep, with the store still
	 * open — which is what a fid holding a stage needs, since §9
	 * allows nothing but the Objsnap calls after the close — and the
	 * free hook runs with lib9p's fid pool, after it.
	 */
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}

	/*
	 * A walk of a fid onto itself that names nothing does not move it
	 * — it is 9P's probe of the fid — so it gives nothing back.
	 */
	memset(&t, 0, sizeof t);
	t.type = Twalk;
	t.tag = cltag(&cl);
	t.fid = Froot;
	t.newfid = Froot;
	t.nwname = 0;
	clrpc(&cl, &t, &r);
	checks++;
	if(r.type != Rwalk || r.nwqid != 0)
		fail("a zero-name walk of a fid onto itself: %s", clerr(&r));
	srvauxcount(ctx, nil, &nc, &nf);
	eqv("a fid that did not move closed nothing", nc, 2);
	eqv("a fid that did not move freed nothing", nf, 2);
Out:
	clstop(&cl);
	srvauxcount(ctx, nil, &nc, &nf);
	eqv("a fid outliving the loop has its state closed as well", nc, 3);
	eqv("a fid outliving the loop still runs its free hook", nf, 3);
	eqv("all three close hooks found the engine still open",
		srvauxopen(ctx), 3);
	eqv("and the store was closed after them", freedseen, 1);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The give-back a create owes the directory fid it is issued on, and
 * what a create that FAILS owes it instead.  A Tcreate that succeeds
 * turns that fid into the created object's, and one fid cannot hold an
 * enumeration's snapshot and an object's state at once (dat.h), so a
 * create cell gives the old state back -- the close hook, then the
 * free hook -- as it retargets the fid.  A create that fails moves no
 * fid: 9P leaves it exactly where it was, so the cell gives nothing
 * back, and once /obj is enumerated the state a failed create dropped
 * would be that fid's own listing snapshot.
 *
 * The create cell here is /obj's own (§2.4's create): it refuses a
 * name §1.1 forbids and retargets the fid on any other, and the
 * fid-state point is what counts the hooks.
 *
 * Both of its refusals are driven, because they are on opposite sides
 * of the queue: a name §1.1 forbids is refused on the service loop,
 * before the request is pushed at all, while `object exists' is
 * refused by the unit that runs on the object's queue -- which is the
 * one that goes on to retarget the fid, so it is the one whose
 * give-back can be too early.
 */
static void
tcreategive(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	uvlong nc, nf;

	clstage = "creategive";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	mkobj(srvstore(ctx), "shoal.map.7", nil, 0, 1);
	srvauxpoint(ctx, 1);
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk){
		fail("walk /obj: %s", clerr(&r));
		goto Out;
	}
	srvauxcount(ctx, nil, &nc, &nf);
	eqv("the directory fid is still holding its state", nc + nf, 0);

	clcreate(&cl, Ffile, "shoal.bad name", 0666, OWRITE, &r);
	clerris("a create of a name §1.1 forbids", &r, "bad object name");
	srvauxcount(ctx, nil, &nc, &nf);
	eqv("a failed create closes nothing of the fid's", nc, 0);
	eqv("a failed create frees nothing of the fid's", nf, 0);

	clcreate(&cl, Ffile, "shoal.map.7", 0666, OWRITE, &r);
	clerris("a create of an id this store holds live", &r,
		"object exists");
	srvauxcount(ctx, nil, &nc, &nf);
	eqv("a create the queue refused closes nothing of the fid's", nc, 0);
	eqv("a create the queue refused frees nothing of the fid's", nf, 0);

	clcreate(&cl, Ffile, "shoal.map.9", 0666, OWRITE, &r);
	checks++;
	if(r.type != Rcreate)
		fail("the create cell answered: %s", clerr(&r));
	srvauxcount(ctx, nil, &nc, &nf);
	eqv("a create closes the directory fid's state", nc, 1);
	eqv("a create frees the directory fid's state", nf, 1);

	clclunk(&cl, Ffile, &r);
	srvauxcount(ctx, nil, &nc, &nf);
	eqv("the clunk behind it had nothing left to close", nc, 1);
	eqv("the clunk behind it had nothing left to free", nf, 1);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * A Tcreate on a fid that is mid-listing, which is where the /obj
 * row's two halves meet on one fid: the directory open installs the
 * listing's snapshot as the fid's state (enum.c), and the create cell
 * gives the fid's state back as it moves the fid onto the object it
 * created (obj.c).  One fid cannot hold both, and the state a create
 * would drop here is the listing the client is part-way through.
 *
 * For a fid whose open has ANSWERED, 9P settles it a message earlier:
 * lib9p refuses a Tcreate on an open fid from Fid.omode, with its own
 * string, before any cell of this row is reached.  That is what this
 * case pins, together with the listing carrying on from exactly where
 * it was.  The name it creates is a reserved one, which is the name
 * this role may create (§2.1): a name the gate would have refused
 * anyway would make the check pass for the wrong reason.
 *
 * lib9p's guard does not reach a create pipelined behind an open that
 * has not answered yet, because the /obj open is offloaded and omode
 * is set only when it answers; the two cells refuse the second
 * themselves there, and tpipeopen below is that case.
 *
 * A create on another fid of the same directory is the case that IS
 * allowed, and it moves neither the cursor nor the snapshot -- the
 * snapshot was taken at the open, so what a later create adds is not
 * in it (store.md §9).
 */
static void
tdircreate(void)
{
	static char *ids[3] = {"dira", "dirb", "dirc"};
	char buf[8192], *m;
	char *w[1];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	Dir dir;
	vlong off;
	long one;
	int i, nent;

	clstage = "dircreate";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	for(i = 0; i < nelem(ids); i++)
		mkobj(srvstore(ctx), ids[i], nil, 0, 1);
	w[0] = "obj";
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	/*
	 * What one entry takes on the wire, measured on a fid of its own
	 * rather than computed, so that the read below stops inside the
	 * listing whatever an entry carries.  The three ids are the same
	 * length, so any of them is the measure.
	 */
	one = 0;
	if(clopenpath(&cl, Froot, Ffile2, 1, w, OREAD, &r) != Ropen)
		fail("open /obj to measure an entry: %s", clerr(&r));
	else if(clread(&cl, Ffile2, 0, 4096, &r) == Rread && r.count > 0)
		one = convM2D((uchar*)r.data, r.count, &dir, buf);
	clclunk(&cl, Ffile2, &r);
	checks++;
	if(one <= BIT16SZ){
		fail("no /obj entry to measure");
		goto Out;
	}

	if(clopenpath(&cl, Froot, Ffile, 1, w, OREAD, &r) != Ropen){
		fail("open /obj: %s", clerr(&r));
		goto Out;
	}
	if(clread(&cl, Ffile, 0, one, &r) != Rread){
		fail("the first read of /obj: %s", clerr(&r));
		goto Out;
	}
	off = r.count;
	nent = dirents(r.data, r.count);
	eqv("the first read stops inside the listing", nent, 1);

	clcreate(&cl, Ffile, "shoal.map.9", 0666, OWRITE, &r);
	clerris("a create on a fid that is mid-listing", &r,
		"9P protocol botch");

	/* the snapshot and the cursor are where the create found them */
	for(;;){
		if(clread(&cl, Ffile, off, 4096, &r) != Rread){
			fail("the listing after the refused create: %s",
				clerr(&r));
			goto Out;
		}
		if(r.count == 0)
			break;
		nent += dirents(r.data, r.count);
		off += r.count;
	}
	eqv("the listing carries on over the refused create", nent,
		nelem(ids));
	clclunk(&cl, Ffile, &r);

	/* the create this row does answer is one on a fid of its own */
	if(clwalk(&cl, Froot, Ffile, 1, w, &r) != Rwalk)
		fail("walk /obj for the create: %s", clerr(&r));
	clcreate(&cl, Ffile, "shoal.map.9", 0666, OWRITE, &r);
	checks++;
	if(r.type != Rcreate)
		fail("a create on a fid that is not listing: %s", clerr(&r));
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * A Topen and a Tcreate pipelined on ONE /obj fid, which is the seam
 * 9P leaves open for this server.  lib9p refuses each of them on an
 * open fid from Fid.omode, and its `ropen' sets that field only once
 * the open has ANSWERED — while the /obj open is offloaded to a queue
 * (srv/enum.c), so a second message sent before that answer passes the
 * guard and both cells run, on two queue procs at once.
 *
 * One fid cannot hold a listing's snapshot and be the created object's
 * at the same time, so exactly one of the two may win and the other is
 * refused with lib9p's own `9P protocol botch' — the string lib9p
 * itself answers wherever it can see the conflict, so a client cannot
 * tell the two refusals apart.  WHICH of them wins is the two procs'
 * race and is not the server's to settle; what this case asserts is
 * that one answer is the operation and the other is the refusal, in
 * both wire orders.
 *
 * §13's `objhold' point is what pipelines them: it parks every queued
 * request at the head of its handler, so the first is held inside the
 * pool while the second is sent, and clearing it starts both.
 */
static void
pipeopen(char *what, int createfirst)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	char *w[1];
	ushort to, tc;
	int i, ok, botch;

	clstage = what;
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0, 1);
	w[0] = "obj";
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("%s: attach: %s", what, clerr(&r));
		goto Out;
	}
	if(clwalk(&cl, Froot, Ffile, 1, w, &r) != Rwalk){
		fail("%s: walk /obj: %s", what, clerr(&r));
		goto Out;
	}
	srvhook(ctx, "objhold", 1);
	to = cltag(&cl);
	tc = cltag(&cl);
	for(i = 0; i < 2; i++){
		memset(&t, 0, sizeof t);
		t.fid = Ffile;
		if((i == 0) == (createfirst != 0)){
			t.type = Tcreate;
			t.tag = tc;
			t.name = "shoal.map.9";
			t.perm = 0666;
			t.mode = OWRITE;
		}else{
			t.type = Topen;
			t.tag = to;
			t.mode = OREAD;
		}
		clput(&cl, &t);
		sleep(200);		/* it is parked at the point */
	}
	srvhook(ctx, "objhold", 0);

	ok = botch = 0;
	clgettag(&cl, to, &r);
	if(r.type == Ropen)
		ok++;
	else if(r.type == Rerror && strcmp(r.ename, "9P protocol botch") == 0)
		botch++;
	else
		fail("%s: the open answered: %s", what, clerr(&r));
	cltagfree(&cl, to);
	clgettag(&cl, tc, &r);
	if(r.type == Rcreate)
		ok++;
	else if(r.type == Rerror && strcmp(r.ename, "9P protocol botch") == 0)
		botch++;
	else
		fail("%s: the create answered: %s", what, clerr(&r));
	cltagfree(&cl, tc);
	eqv("exactly one of the two pipelined requests succeeded", ok, 1);
	eqv("and the other is lib9p's own botch", botch, 1);

	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot, &r);
Out:
	srvhook(ctx, "objhold", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

static void
tpipeopen(void)
{
	pipeopen("pipeopen: open then create", 0);
	pipeopen("pipeopen: create then open", 1);
}

/*
 * The fid registry under a walk that moves a fid.  A walk that names
 * an object runs on that object's queue while attaches and clones run
 * on the service loop, and both reach the same list: the walk gives
 * the fid's state back and writes the new one, the attach links a new
 * fid in at the head.  What the walk MUST NOT carry across that
 * window is the fid's registry links — putting back what it read
 * before the attach drops the attach's fid off the list, where
 * srvfidsclose can no longer reach it and where its own destroy
 * writes through a neighbour that has been freed.
 *
 * The window is forced rather than raced for: the point parks the
 * queued walk at its commit, the attach runs on the loop inside it,
 * and what the count says after the walked fid is clunked is whether
 * anything left the list with it.
 */
static void
tfidwalk(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	ushort ta;

	clstage = "fidwalk";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0, 1);
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk){
		fail("walk /obj: %s", clerr(&r));
		goto Out;
	}
	eqv("the registry holds the attach's fid and the walk's",
		srvfidcount(ctx), 2);

	/*
	 * A self-walk of that fid onto an object: it moves, and it runs on
	 * that object's queue.  The fid is the newest one, so it is the
	 * head of the list — which is the entry an attach writes.
	 */
	srvhook(ctx, "walkhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Twalk;
	t.tag = ta = cltag(&cl);
	t.fid = Ffile;
	t.newfid = Ffile;
	t.nwname = 1;
	t.wname[0] = "alpha";
	clput(&cl, &t);
	sleep(200);			/* it is held at its commit */

	if(clattach(&cl, Froot2, "role=admin", &r) != Rattach)
		fail("attach while a walk is held at its commit: %s", clerr(&r));
	eqv("the attach's fid is on the registry too", srvfidcount(ctx), 3);

	srvhook(ctx, "walkhold", 0);
	clget(&cl, &r);
	checks++;
	if(r.type != Rwalk || r.tag != ta || r.nwqid != 1)
		fail("the held self-walk: type %d tag %ud %s", r.type, r.tag,
			clerr(&r));
	eqv("and it is still there once the walk has committed",
		srvfidcount(ctx), 3);

	/* the moved fid leaves; nothing else may leave with it */
	clclunk(&cl, Ffile, &r);
	eqv("a fid the walk moved takes only itself off the registry",
		srvfidcount(ctx), 2);
	clclunk(&cl, Froot2, &r);
	eqv("the fid the attach made was still on it", srvfidcount(ctx), 1);
Out:
	srvhook(ctx, "walkhold", 0);
	clstop(&cl);
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
 * is the cell the object-I/O surface will discard its stage from.
 * §5.4.1 makes the whole of step 7 a MUST however far the flushed
 * request had got — a fid's stage spans several Twrites, so a Tflush
 * of the next queued write on a staging fid must still discard that
 * fid's stage — so it runs exactly once for each of the two halves,
 * and never after the reply.
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
		fail("verify before the hold: %s", clerr(&r));
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
		fail("open /status under the hold: %s", clerr(&r));
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
	eqv("a request flushed while queued runs step 7", n7, 1);
	eqv("and it ran before lib9p's own answer", srvauxlate(ctx), 0);

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
	eqv("a flushed running request runs step 7 once", n7, 2);
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
	eqv("a request flushed at its exit runs step 7", n7, 3);

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
	eqv("a request flushed at its error exit runs step 7", n7, 4);
	eqv("no step 7 ran after its reply", srvauxlate(ctx), 0);
	srvhook(ctx, "objexit", 0);

	/* a Tflush naming a request answered on the loop is still answered */
	checks++;
	if(clflush(&cl, 31337, &r) != Rflush)
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
	uvlong np, nd, n7;
	ushort ta, tb, tf;
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
		fail("attach: %s", clerr(&r));
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", clerr(&r));
		goto Out;
	}
	if(clwalk1(&cl, Froot, Ffile, "map", &r) != Rwalk){
		fail("walk /map: %s", clerr(&r));
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
			clerr(&r));
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
			"%s", clerr(&r));
	checks++;
	if(clflush(&cl, 31337, &r) != Rflush)
		fail("an unrelated Tflush while a request is held off the "
			"loop: type %d", r.type);

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
		fail("walk /map again: %s", clerr(&r));
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
		fail("walk /map a third time: %s", clerr(&r));
	clopen(&cl, Ffile, OREAD, &r);
	checks++;
	if(r.type != Ropen)
		fail("an open prepared for a queue and answered on the loop: "
			"%s", clerr(&r));
	clclunk(&cl, Ffile, &r);
	srvhook(ctx, "mapopen", 0);
	sleep(100);
	srvcount(ctx, &np, &nd);
	eqv("a preparation without a push leaves the pool empty", np - nd, 0);
	w[0] = "status";
	if(clopenpath(&cl, Froot, Ffile2, 1, w, OREAD, &r) != Ropen)
		fail("open /status: %s", clerr(&r));
	else{
		clslurp(&cl, Ffile2, buf, sizeof buf);
		eqs("/status qdepth= after it",
			clfield(buf, "qdepth", val, sizeof val), "0");
		clclunk(&cl, Ffile2, &r);
	}

	/*
	 * Two requests in flight and their replies out of order: the open
	 * waits on the reserved queue while the verb issued behind it runs
	 * to completion on an object's queue, so the second request is
	 * answered first.  A client that pipelines picks the reply it is
	 * waiting for out by tag, and the one it passed over is still
	 * there, in arrival order, for the read after it.
	 */
	srvhook(ctx, "mapopen", 1);
	if(clwalk1(&cl, Froot, Ffile, "map", &r) != Rwalk)
		fail("walk /map a fourth time: %s", clerr(&r));
	memset(&t, 0, sizeof t);
	t.type = Topen;
	t.tag = ta = cltag(&cl);
	t.fid = Ffile;
	t.mode = OREAD;
	clput(&cl, &t);
	sleep(200);			/* held on the reserved queue */
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = tb = cltag(&cl);
	t.fid = Fctl;
	t.offset = 0;
	t.data = "verify alpha";
	t.count = strlen(t.data);
	clput(&cl, &t);
	sleep(200);			/* answered while the open is still held */
	srvhook(ctx, "mapopen", 0);
	sleep(200);			/* and the open's answer is behind it now */
	checks++;
	if(clgettag(&cl, ta, &r) != Ropen)
		fail("the held open, collected by its tag: type %d tag %ud %s",
			r.type, r.tag, clerr(&r));
	checks++;
	if(clget(&cl, &r) != Rwrite || r.tag != tb)
		fail("the reply that came first, read after it: type %d tag "
			"%ud", r.type, r.tag);
	clclunk(&cl, Ffile, &r);

	/*
	 * The flush flag belongs to the QUEUE, not to a request: it is
	 * raised for the request the queue's proc is carrying and cleared
	 * when that proc takes the next one.  A request merely prepared for
	 * that queue and then answered on the service loop was never the
	 * queue's, so it must not read the flag — here the first open is
	 * flushed and held inside its handler, so the flag is still raised
	 * while the loop prepares and answers the second one.
	 */
	srvauxpoint(ctx, 1);
	srvhook(ctx, "mapopen", 1);
	srvhook(ctx, "anyexit", 1);
	if(clwalk1(&cl, Froot, Ffile, "map", &r) != Rwalk)
		fail("walk /map a fifth time: %s", clerr(&r));
	memset(&t, 0, sizeof t);
	t.type = Topen;
	t.tag = ta = cltag(&cl);
	t.fid = Ffile;
	t.mode = OREAD;
	clput(&cl, &t);
	sleep(200);			/* held on the reserved queue */
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = ta;
	clput(&cl, &t);
	sleep(200);			/* flushed, and held inside its handler */

	srvhook(ctx, "mapopen", 2);
	if(clwalk1(&cl, Froot, Ffile2, "map", &r) != Rwalk)
		fail("walk /map for a second open: %s", clerr(&r));
	clopen(&cl, Ffile2, OREAD, &r);
	checks++;
	if(r.type != Ropen)
		fail("an open prepared for a queue whose flush flag is up: %s",
			clerr(&r));
	srvauxcount(ctx, &n7, nil, nil);
	eqv("a request the queue never took runs no step 7", n7, 0);
	clclunk(&cl, Ffile2, &r);

	srvhook(ctx, "mapopen", 0);
	srvhook(ctx, "anyexit", 0);
	checks++;
	if(clgettag(&cl, ta, &r) != Rerror || strcmp(r.ename, "interrupted") != 0)
		fail("the held open, released and flushed: type %d %s", r.type,
			clerr(&r));
	cltagfree(&cl, ta);
	checks++;
	if(clgettag(&cl, tf, &r) != Rflush)
		fail("the Rflush after it: type %d", r.type);
	cltagfree(&cl, tf);
	srvauxcount(ctx, &n7, nil, nil);
	eqv("the request the queue did take runs step 7", n7, 1);
	clclunk(&cl, Ffile, &r);
	srvauxpoint(ctx, 0);
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
 *
 * That park has a deadline of its own (srv.h), which this case stays
 * well inside: what it does between setting the point and clearing it
 * is two 200 ms sleeps.  What a case that never clears the point gets
 * is tloophold's subject.
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
		fail("attach: %s", clerr(&r));
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", clerr(&r));
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
		fail("verify after the lost flush: %s", clerr(&r));
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
	static struct {
		char	*e;
		int	is;
	} intr[] = {
		{"interrupted",			1},
		{"shoalsrv: interrupted",	1},
		{"x: flush: interrupted",	1},
		{"x: interrupted by note",	1},
		{"no such object",		0},
		{"x: interrupted: y",		0},
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
	 * The other classifier: which strings are the interrupted class.
	 * The rule is lib/dev.c's — the last `: '-separated segment, with
	 * the word looked for inside it rather than matched whole — so a
	 * device that says what it was doing is still that class, and a
	 * segment that only mentions the word earlier is not.
	 */
	for(i = 0; i < nelem(intr); i++){
		checks++;
		if(srvintr(intr[i].e) != intr[i].is)
			fail("srvintr(%#q) is %d", intr[i].e,
				srvintr(intr[i].e));
	}
	checks++;
	if(srvintr(nil) != 0)
		fail("srvintr(nil) is %d", srvintr(nil));

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
 * A background job, which is what a pass proc started by a ctl verb
 * will be: inside the engine, and not a Req, so the drain cannot see
 * it.  store.md §9 forbids closing the store under one, so the
 * shutdown waits for it — and what the job records is whether the
 * store had been closed while it was still running.
 */
static Srvctx *jobctx;
static int jobstarted, jobended, jobrefused, jobfreed;

static void
jobproc(void*)
{
	if(srvjobstart(jobctx) < 0){
		jobrefused = 1;
		jobstarted = 1;
		return;
	}
	jobstarted = 1;
	sleep(600);			/* the shutdown must wait this out */
	jobfreed = freedseen;		/* ... so this is what it saw */
	jobended = 1;
	srvjobend(jobctx);
}

static void
tjobs(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	int i;

	clstage = "jobs";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	freedseen = 0;
	if((ctx = startsrv(d, m, 4)) == nil)
		return;
	jobctx = ctx;
	jobstarted = jobended = jobrefused = 0;
	jobfreed = -1;

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach)
		fail("attach: %s", clerr(&r));
	if(tspawn(jobproc, nil) < 0)
		fail("tspawn: %r");
	for(i = 0; i < 200 && !jobstarted; i++)
		sleep(20);
	istrue("a background job starts while the instance is serving",
		jobstarted && !jobrefused);

	clstop(&cl);			/* the loop ends; the shutdown runs */
	istrue("the job had ended when the shutdown went on", jobended);
	eqv("the store was closed once", freedseen, 1);
	eqv("the store was still open while the job ran", jobfreed, 0);
	istrue("a job is refused once the shutdown has begun",
		srvjobstart(ctx) < 0);
	istrue("and a job already running is told to stop",
		srvstopping(ctx) != 0);
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

	/*
	 * The loop ends with the request in flight, and nothing releases
	 * the hold: the shutdown clears the points itself, because the
	 * program that set one is not necessarily still watching when a
	 * connection drops and a held request would otherwise hold the
	 * drain -- and the store's close -- for as long as it lived.
	 * Whether anything was still in flight at the close is what the
	 * engine's freed callback recorded, below.
	 */
	clhangup(&cl);
	istrue("the service loop ends with a held request in flight",
		clwaitend(&cl, 10000));
	clget(&cl, &r);
	checks++;
	if(r.type != Rwrite || r.tag != ta)
		fail("the drained request answered: type %d tag %ud", r.type,
			r.tag);
	eqv("the store was closed once", freedseen, 1);
	eqv("nothing was in flight when the store was freed",
		freedqdepth, 0);
	clclose(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * A fid's state, between the queue proc discarding it and the service
 * loop giving it back.  Step 7 reads the flushed fid's flush cell and
 * calls through it on a queue proc; a clunk, and a walk that moves the
 * fid, clear those cells and free what they named on the service loop.
 * lib9p's own reference keeps the Fid alive across both, but not what
 * the fid is carrying, so the fid's state lock spans each of them
 * whole.
 *
 * The window is forced rather than raced for: the point parks step 7
 * between the read and the call, and the walk is issued into it.
 */
static void
tstep7fid(void)
{
	char *m;
	uchar data[1024];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	uvlong n7, nc, nf;
	ushort ta, tf, tw;

	clstage = "step7fid";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 1)) == nil)
		return;
	srvauxpoint(ctx, 1);
	memset(data, 0x66, sizeof data);
	mkobj(srvstore(ctx), "alpha", data, sizeof data, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach)
		fail("attach: %s", clerr(&r));
	if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk){
		fail("walk /obj: %s", clerr(&r));
		goto Out;
	}

	/*
	 * A self-walk of that directory fid onto an object runs on the
	 * object's queue, so it is a queued request whose fid the service
	 * loop can still reach: the fid is a directory and is not open,
	 * which is what 9P asks of a fid a walk may move.
	 */
	srvhook(ctx, "objhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Twalk;
	t.tag = ta = cltag(&cl);
	t.fid = Ffile;
	t.newfid = Ffile;
	t.nwname = 1;
	t.wname[0] = "alpha";
	clput(&cl, &t);
	sleep(200);			/* running, and held */

	srvhook(ctx, "step7", 1);
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = ta;
	clput(&cl, &t);
	sleep(300);			/* unwinding, and held inside step 7 */

	/* the same fid, moved off the file whose state step 7 is in */
	memset(&t, 0, sizeof t);
	t.type = Twalk;
	t.tag = tw = cltag(&cl);
	t.fid = Ffile;
	t.newfid = Ffile;
	t.nwname = 1;
	t.wname[0] = "..";
	clput(&cl, &t);
	sleep(300);
	srvauxcount(ctx, &n7, &nc, &nf);
	eqv("step 7 has not run yet", n7, 0);
	eqv("and the walk has not closed the state under it", nc, 0);
	eqv("nor freed it", nf, 0);

	srvhook(ctx, "step7", 0);
	sleep(300);
	srvauxcount(ctx, &n7, &nc, &nf);
	eqv("step 7 ran once", n7, 1);
	eqv("and only then did the walk close the state", nc, 1);
	eqv("and free it", nf, 1);
	eqv("step 7 ran before the reply", srvauxlate(ctx), 0);

	checks++;
	if(clgettag(&cl, ta, &r) != Rerror || strcmp(r.ename, "interrupted") != 0)
		fail("the flushed request: type %d %s", r.type, clerr(&r));
	cltagfree(&cl, ta);
	checks++;
	if(clgettag(&cl, tf, &r) != Rflush)
		fail("the Rflush after it: type %d", r.type);
	cltagfree(&cl, tf);
	checks++;
	if(clgettag(&cl, tw, &r) != Rwalk || r.nwqid != 1)
		fail("the walk that moved the fid: type %d %s", r.type,
			clerr(&r));
	cltagfree(&cl, tw);
	clclunk(&cl, Ffile, &r);
Out:
	srvhook(ctx, "step7", 0);
	srvhook(ctx, "objhold", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * Step 7 on a fid another request is still working through.  9P allows
 * two requests to be outstanding on one fid, and two requests naming
 * one object share a queue — serialised with each other, but not with
 * the service loop, which performs step 7 itself for a request flushed
 * while it was still queued (layer-a §5.4.1).  So the loop can reach
 * the flushed fid's state while a queue proc is part-way through a
 * step on it, which for object I/O is a stage discard landing in the
 * middle of the Twrite before it.
 *
 * The fid's state lock is what rules that out, and the fid-state point
 * is what shows it: its handler side marks the state mid-step across
 * the check point's hold, under that lock, and its flush hook counts
 * the times it ran on a state so marked.
 */
static void
tstep7busy(void)
{
	char *m;
	uchar data[1024];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	char *w[1];
	uvlong n7;
	ushort ta, tb, tf;

	clstage = "step7busy";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 1)) == nil)
		return;
	srvauxpoint(ctx, 1);
	memset(data, 0x77, sizeof data);
	mkobj(srvstore(ctx), "alpha", data, sizeof data, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach)
		fail("attach: %s", clerr(&r));
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", clerr(&r));
		goto Out;
	}

	/*
	 * Two writes on the ONE fid, naming the one object: the first runs
	 * and is held mid-step, the second waits on the same queue behind
	 * it.  Flushing the second is what sends step 7 to that fid from
	 * the service loop.
	 */
	srvhook(ctx, "objhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Fctl;
	t.offset = 0;
	t.data = "verify alpha";
	t.count = strlen(t.data);
	clput(&cl, &t);
	sleep(200);			/* running, and held mid-step */
	t.tag = tb = cltag(&cl);
	clput(&cl, &t);
	sleep(200);			/* ... and this one is behind it */

	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = tb;
	clput(&cl, &t);
	sleep(300);
	srvauxcount(ctx, &n7, nil, nil);
	eqv("step 7 waits for the handler working on the same fid", n7, 0);

	srvhook(ctx, "objhold", 0);
	sleep(300);
	srvauxcount(ctx, &n7, nil, nil);
	eqv("and runs once that handler is between steps", n7, 1);
	eqv("no step 7 ran inside a handler's step", srvauxbusy(ctx), 0);
	eqv("and none ran after its request had answered", srvauxlate(ctx), 0);

	checks++;
	if(clgettag(&cl, tb, &r) != Rerror
	|| strcmp(r.ename, "interrupted") != 0)
		fail("the flushed queued request: type %d %s", r.type,
			clerr(&r));
	cltagfree(&cl, tb);
	checks++;
	if(clgettag(&cl, tf, &r) != Rflush)
		fail("the Rflush after it: type %d", r.type);
	cltagfree(&cl, tf);
	checks++;
	if(clgettag(&cl, ta, &r) != Rwrite)
		fail("the request that was mid-step: type %d %s", r.type,
			clerr(&r));
	cltagfree(&cl, ta);
Out:
	srvhook(ctx, "objhold", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * Where the step 7 hold leaves the service loop.  A queue proc parked
 * inside step 7 holds the fid whose state it is discarding and nothing
 * else: every other fid goes on being attached, walked and clunked on
 * the loop, the connection dropping still ends the loop, and the
 * shutdown clears the point that is holding the proc.  A hold that sat
 * on the fid registry instead would stop the first clunk to come along
 * and, with the loop stopped there, the shutdown that would have
 * released it.
 */
static void
tstep7hold(void)
{
	char *m;
	uchar data[1024];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	uvlong n7;
	ushort ta, tf, tc;

	clstage = "step7hold";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 1)) == nil)
		return;
	srvauxpoint(ctx, 1);
	memset(data, 0x88, sizeof data);
	mkobj(srvstore(ctx), "alpha", data, sizeof data, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach)
		fail("attach: %s", clerr(&r));
	if(clwalk1(&cl, Froot, Ffile, "obj", &r) != Rwalk
	|| clwalk1(&cl, Froot, Fctl, "ctl", &r) != Rwalk){
		fail("the two fids: %s", clerr(&r));
		goto Out;
	}

	/* a queued walk, running and held, then flushed: step 7 unwinds it */
	srvhook(ctx, "objhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Twalk;
	t.tag = ta = cltag(&cl);
	t.fid = Ffile;
	t.newfid = Ffile;
	t.nwname = 1;
	t.wname[0] = "alpha";
	clput(&cl, &t);
	sleep(200);			/* running, and held */
	srvhook(ctx, "step7", 1);
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = ta;
	clput(&cl, &t);
	sleep(300);			/* unwinding, and held inside step 7 */

	/*
	 * A clunk of an unrelated fid, pipelined so that a loop that never
	 * answers it fails the case here rather than hanging it, and then
	 * the connection drops.  Both must go through with the point still
	 * set: the shutdown is what clears it.
	 */
	memset(&t, 0, sizeof t);
	t.type = Tclunk;
	t.tag = tc = cltag(&cl);
	t.fid = Fctl;
	clput(&cl, &t);
	clhangup(&cl);
	istrue("the service loop ends with a request held inside step 7",
		clwaitend(&cl, 8000));
	/* the shutdown cleared it; this is for a loop that never got there */
	srvhook(ctx, "step7", 0);

	checks++;
	if(clgettag(&cl, tc, &r) != Rclunk)
		fail("the clunk of an unrelated fid: type %d %s", r.type,
			clerr(&r));
	cltagfree(&cl, tc);
	checks++;
	if(clgettag(&cl, ta, &r) != Rerror
	|| strcmp(r.ename, "interrupted") != 0)
		fail("the flushed walk: type %d %s", r.type, clerr(&r));
	cltagfree(&cl, ta);
	checks++;
	if(clgettag(&cl, tf, &r) != Rflush)
		fail("the Rflush after it: type %d", r.type);
	cltagfree(&cl, tf);
	srvauxcount(ctx, &n7, nil, nil);
	eqv("step 7 ran once", n7, 1);
	clclose(&cl);
	srvhook(ctx, "step7", 0);
	srvhook(ctx, "objhold", 0);
	srvfree(ctx);
	devclose(d);
	free(m);
	return;
Out:
	srvhook(ctx, "step7", 0);
	srvhook(ctx, "objhold", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The two points that park the SERVICE LOOP, and the deadline that is
 * all either has.  srvholdclear cannot reach them — the shutdown runs
 * from Srv.end, which lib9p calls on the loop — so a program that sets
 * one and stops watching would wedge the server for as long as it
 * lived.  Neither arm below ever clears the point it sets.
 */
static void
tloophold(void)
{
	char *m;
	uchar data[1024];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	char *w[1];
	vlong t0, ms;
	ushort ta, tb, tf;

	clstage = "loophold";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 1)) == nil)
		return;
	memset(data, 0x99, sizeof data);
	mkobj(srvstore(ctx), "alpha", data, sizeof data, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach)
		fail("attach: %s", clerr(&r));
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", clerr(&r));
		goto Out;
	}

	/* the flush hold, over a Tflush of a request that is running */
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
	srvhook(ctx, "flushhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = ta;
	t0 = nsec();
	clput(&cl, &t);
	checks++;
	if(clgettag(&cl, ta, &r) != Rerror
	|| strcmp(r.ename, "interrupted") != 0)
		fail("the flushed request: type %d %s", r.type, clerr(&r));
	cltagfree(&cl, ta);
	checks++;
	if(clgettag(&cl, tf, &r) != Rflush)
		fail("the Rflush the flush hold let go of: type %d", r.type);
	cltagfree(&cl, tf);
	ms = (nsec() - t0) / 1000000;
	istrue("the flush hold parks the loop", ms > 1000);
	istrue("and lets go although nothing cleared it", ms < 15000);
	srvhook(ctx, "objhold", 0);

	/*
	 * The step 7 hold, on the loop: a request flushed while it was
	 * still queued has step 7 performed for it there, so the point
	 * parks the loop exactly as the flush hold does.  The first write
	 * is what keeps the second one queued, and it stays held until
	 * both replies are in, so that the second is still on the queue
	 * when the flush reaches it.
	 */
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
	t.tag = tb = cltag(&cl);
	clput(&cl, &t);
	sleep(200);			/* ... and this one is queued behind it */
	srvhook(ctx, "step7", 1);
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = tb;
	t0 = nsec();
	clput(&cl, &t);
	checks++;
	if(clgettag(&cl, tb, &r) != Rerror
	|| strcmp(r.ename, "interrupted") != 0)
		fail("the request flushed while queued: type %d %s", r.type,
			clerr(&r));
	cltagfree(&cl, tb);
	checks++;
	if(clgettag(&cl, tf, &r) != Rflush)
		fail("the Rflush the step 7 hold let go of: type %d", r.type);
	cltagfree(&cl, tf);
	ms = (nsec() - t0) / 1000000;
	istrue("the step 7 hold parks the loop", ms > 1000);
	istrue("and lets go although nothing cleared it either", ms < 15000);
	srvhook(ctx, "objhold", 0);
	checks++;
	if(clgettag(&cl, ta, &r) != Rwrite)
		fail("the request that was running: type %d %s", r.type,
			clerr(&r));
	cltagfree(&cl, ta);
Out:
	srvhook(ctx, "objhold", 0);
	clstop(&cl);
	srvhook(ctx, "flushhold", 0);
	srvhook(ctx, "step7", 0);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * srvrun can return while lib9p is still using the context.  A pushed
 * request is counted complete by srvdestroyreq, which lib9p runs from
 * closereq inside respond and therefore BEFORE respond releases the
 * service: the drain converges, the service loop ends and srvrun
 * returns with that reference still outstanding.  When it is finally
 * released, lib9p frees its fid pool, which runs this library's
 * destroy hook over the server's own fid registry — so a caller that
 * freed the context when srvrun returned would have it walk a registry
 * that is no longer there.  The point widens the gap; what the case
 * asks is that srvfree does not return inside it.
 */
static Srvctx *endctx;
static int endfreed;		/* srvfree has returned */

static void
endfreeproc(void*)
{
	srvfree(endctx);
	endfreed = 1;
}

static void
tendwait(void)
{
	char *m;
	uchar data[1024];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	char *w[1];
	vlong t0, ms;
	int i, held;

	clstage = "endwait";
	m = mkmap(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid);
	d = newdisk();
	if((ctx = startsrv(d, m, 1)) == nil)
		return;
	memset(data, 0x55, sizeof data);
	mkobj(srvstore(ctx), "alpha", data, sizeof data, 1);

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach)
		fail("attach: %s", clerr(&r));
	w[0] = "ctl";
	if(clopenpath(&cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", clerr(&r));
		clstop(&cl);
		srvfree(ctx);
		devclose(d);
		free(m);
		return;
	}

	srvendpoint(ctx, 10000);
	clwrite(&cl, Fctl, 0, "verify alpha", &r);
	checks++;
	if(r.type != Rwrite)
		fail("verify before the hangup: %s", clerr(&r));

	/*
	 * Its queue proc is now between the completion count and lib9p's
	 * release, so the drain converges and the loop ends under it.  It
	 * stays there until the point is cleared, which is what makes the
	 * moment lib9p is let go of this case's to choose rather than a
	 * count of milliseconds to race.
	 */
	clhangup(&cl);
	istrue("the service loop ends while a queue proc is still in lib9p",
		clwaitend(&cl, 10000));
	clclose(&cl);
	istrue("lib9p has not let go when the service loop returns",
		!srvreleased(ctx));

	endctx = ctx;
	endfreed = 0;
	if(tspawn(endfreeproc, nil) < 0)
		fail("tspawn: %r");
	sleep(300);
	held = !endfreed;
	istrue("srvfree does not return while lib9p holds the service", held);
	/*
	 * The context is there to ask only while srvfree has not returned;
	 * a srvfree that returned early has already freed it.
	 */
	istrue("and lib9p was indeed still holding it",
		held && !srvreleased(ctx));

	/*
	 * Let the queue proc out of lib9p.  What srvfree owes is to return
	 * after the release and not before: it has not returned yet, and
	 * from here it is a few milliseconds of its own polling away —
	 * which is what tells it apart from a wait long enough to have
	 * covered the window by the clock.
	 */
	t0 = nsec();
	if(held)
		srvendpoint(ctx, 0);
	for(i = 0; i < 300 && !endfreed; i++)
		sleep(10);
	ms = (nsec() - t0) / 1000000;
	istrue("and returns once lib9p has let go", endfreed);
	istrue("as soon as it has", held && endfreed && ms < 300);
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
	treadcell();
	tcellclear();
	tctl();
	tverify();
	tdevintr();
	tobjgate();
	tiogate();
	tdown("in", "no");
	tdown("out", "yes");
	tfidstate();
	tcreategive();
	tdircreate();
	tpipeopen();
	tfidwalk();
	tflush();
	tstep7fid();
	tstep7busy();
	tstep7hold();
	tanyq();
	tflushrace();
	tloophold();
	terrors();
	tjobs();
	tshutdown();
	tendwait();

	clwatchoff();
	if(fails > 0){
		fprint(2, "srvtest: %d of %d checks failed\n", fails, checks);
		threadexitsall("fail");
	}
	print("srvtest: %d checks ok\n", checks);
	threadexitsall(nil);
}
