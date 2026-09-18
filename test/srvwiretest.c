#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../lib/shoal.h"
#include "../srv/srv.h"

/*
 * T1: the peer channels over the 9P surface — layer-a §5.5's /repl and
 * §5.6's /rpc.  A whole instance runs inside this program, as in
 * srviotest: a simulated disk, the store engine over it,
 * srv/libshoalsrv.a over that, and a raw 9P client on the other end of
 * a pipe (srv9p.h).  What a case asserts is the exact bytes of a reply.
 *
 * The maps here put two instances in the map with `replicas=1' and
 * both up, which is what a `role=repl' attach needs — §6.4 F3 refuses
 * an attach whose `peer=' is not a non-dead instance of this
 * instance's own map — and what op=drop's `still placed' re-check is
 * asked against.  Placement matters nowhere else: neither channel
 * admits `role=client', so layer-a §5.4's admission and its
 * `degraded' (store.md §14(34)) are not on these paths.
 *
 * The checksums a case names are computed here from the content the
 * operation is meant to leave behind, through the same library the
 * server computes its own with but over the test's own copy of the
 * bytes: what is under test is that the server computes the object's
 * resulting csum from what it committed and refuses a push whose named
 * csum differs.
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

	Froot	= 1,
	Frepl	= 2,
	Frpc	= 3,
	Frepl2	= 4,
	Frpc2	= 5,

	Maxresp	= 16*1024,
};

static char Tuuid[] = "0000000000000000000000000000000a";
static char Tmonid[] = "00112233445566778899aabbccddeeff";
static char Nrepl[] = "role=repl,peer=n2.0";
static char Nadmin[] = "role=admin";
static char Nclient[] = "role=client,epoch=7";

static char*
mkmap(void)
{
	char *p;

	p = smprint(
		"map=t epoch=%d\n"
		"\tmonid=%s\n"
		"\tobjmax=%llud blksz=%lud replicas=1\n"
		"\tcsumalg=blake2s256 placehash=blake2s256-64\n"
		"\tpollms=1000 leasems=3000 replms=1000 deadms=10000\n"
		"\toutmins=60 tombdays=7 mincopies=1 retain=8\n"
		"\n"
		"instance=n1.0 onnode=n1 addr=tcp!10.0.0.1!17011\n"
		"\tuuid=%s\n"
		"\tclass=ssd weight=100 status=in up=yes since=1 fenced=no\n"
		"instance=n2.0 onnode=n2 addr=tcp!10.0.0.2!17011\n"
		"\tuuid=0000000000000000000000000000000c\n"
		"\tclass=ssd weight=100 status=in up=yes since=1 fenced=no\n",
		Tepoch, Tmonid, (uvlong)Tobjmax, (ulong)Tblksz, Tuuid);
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
newdisk(ulong nslots, Super *sp)
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
	if(sp != nil)
		*sp = s;
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

/* the engine side, which is not what is under test */
static void
mkobj(Store *s, char *name, void *data, long n, uvlong ver)
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
	if(n > 0 && objwrite(s, oid, len, data, n, 0, ver, Tepoch, nil, 0) < 0)
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

static char*
hexs(char *buf, uchar *p, int n)
{
	static char hex[] = "0123456789abcdef";
	int i;

	for(i = 0; i < n; i++){
		buf[2*i] = hex[p[i]>>4];
		buf[2*i+1] = hex[p[i]&15];
	}
	buf[2*n] = 0;
	return buf;
}

/* the csum an object of these bytes has (§1.4), as the header spells it */
static char*
ocsum(char *buf, void *p, uvlong len)
{
	uchar cs[Csumlen];

	objcsum(p, len, Tblksz, cs);
	return hexs(buf, cs, Csumlen);
}

/* the dcsum of a payload (§5.5): BLAKE2s-128 over the bytes alone */
static char*
dcs(char *buf, void *p, long n)
{
	uchar d[Blkdlen];

	blkdigest(p, n, d);
	return hexs(buf, d, Blkdlen);
}

/*
 * One /repl or /rpc operation: the header line, a newline, and the
 * payload where the operation defines one.
 */
static int
chanop(Cl *c, ulong fid, char *hdr, void *data, long n, Fcall *r)
{
	static uchar buf[32*1024];
	Fcall t;
	long hn;

	hn = strlen(hdr);
	memmove(buf, hdr, hn);
	buf[hn++] = '\n';
	if(n > 0)
		memmove(buf+hn, data, n);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = cltag(c);
	t.fid = fid;
	t.offset = 0;
	t.count = hn + n;
	t.data = (char*)buf;
	return clrpc(c, &t, r);
}

/* a raw Twrite, for the cases about the message rather than the header */
static int
rawop(Cl *c, ulong fid, void *a, long n, Fcall *r)
{
	Fcall t;

	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = cltag(c);
	t.fid = fid;
	t.offset = 0;
	t.count = n;
	t.data = a;
	return clrpc(c, &t, r);
}

/* the error a /repl operation was refused with, or "ok" */
static char*
repler(Cl *c, ulong fid, char *hdr, void *data, long n)
{
	static Fcall r;

	if(chanop(c, fid, hdr, data, n, &r) == Rwrite)
		return "ok";
	return clerr(&r);
}

/*
 * One /rpc exchange: the request, then the single Tread that delivers
 * the response.  Answers the response bytes, or the Rerror string with
 * *np set to -1.
 */
static char*
rpc(Cl *c, ulong fid, char *req, long *np)
{
	static char resp[Maxresp];
	Fcall r;

	if(chanop(c, fid, req, nil, 0, &r) != Rwrite){
		if(np != nil)
			*np = -1;
		return clerr(&r);
	}
	if(clread(c, fid, 0, 8192, &r) != Rread){
		if(np != nil)
			*np = -1;
		return clerr(&r);
	}
	if(r.count >= sizeof resp)
		sysfatal("response too long");
	memmove(resp, r.data, r.count);
	resp[r.count] = 0;
	if(np != nil)
		*np = r.count;
	return resp;
}

/*
 * The §2.6 condition a refusal carries, without the detail §2.6 lets a
 * store add after it (`not primary: n5.0' is the pattern): what a case
 * asserts is the condition, and the detail is the store's to choose.
 */
static char*
cond(char *s)
{
	static char buf[ERRMAX];
	char *p;

	strecpy(buf, buf + sizeof buf, s);
	if((p = strstr(buf, ": ")) != nil)
		*p = 0;
	return buf;
}

/* the first line of a response, for a case that asserts it whole */
static char*
line1(char *s)
{
	static char buf[1024];
	char *p;

	strecpy(buf, buf + sizeof buf, s);
	if((p = strchr(buf, '\n')) != nil)
		*p = 0;
	return buf;
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

/*
 * The two fids every case wants: /repl under role=repl and /rpc under
 * the same attach.  Answers 0 when either could not be had.
 */
static int
chopen(Cl *cl, Srvctx *ctx, char *aname, ulong root, ulong repl, ulong rpc)
{
	Fcall r;

	USED(ctx);
	if(clattach(cl, root, aname, &r) != Rattach){
		fail("attach %s: %s", aname, clerr(&r));
		return 0;
	}
	if(repl != ~0UL){
		if(clwalk1(cl, root, repl, "repl", &r) != Rwalk
		|| clopen(cl, repl, OWRITE, &r) != Ropen){
			fail("open /repl: %s", clerr(&r));
			return 0;
		}
	}
	if(rpc != ~0UL){
		if(clwalk1(cl, root, rpc, "rpc", &r) != Rwalk
		|| clopen(cl, rpc, ORDWR, &r) != Ropen){
			fail("open /rpc: %s", clerr(&r));
			return 0;
		}
	}
	return 1;
}

/*
 * §5.5's header grammar, and store.md §3.8's rule that a header
 * carrying ver=0 is refused before any engine call.  Everything here
 * is answered on the service loop, so nothing of it reaches the store.
 */
static void
tgrammar(void)
{
	char *m, cs[Csumhexlen], zero[Csumhexlen], *hdr;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	uvlong np, nd;

	clstage = "grammar";
	m = mkmap();
	d = newdisk(Tnslots, nil);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	ocsum(zero, nil, 0);
	clstart(&cl, ctx, Clmsize);
	if(!chopen(&cl, ctx, Nrepl, Froot, Frepl, ~0UL))
		goto Out;
	waitidle(ctx, &np, &nd);

	eqs("an unknown op", repler(&cl, Frepl,
		"op=frobnicate oid=a epoch=7 ver=1 wepoch=7 csum=x", nil, 0),
		"bad ctl");
	hdr = smprint("op=create oid=a epoch=7 ver=1 csum=%s", zero);
	eqs("a missing attribute", repler(&cl, Frepl, hdr, nil, 0), "bad ctl");
	free(hdr);
	hdr = smprint("op=create oid=a epoch=7 ver=1 wepoch=7 off=0 csum=%s",
		zero);
	eqs("an attribute the op does not define",
		repler(&cl, Frepl, hdr, nil, 0), "bad ctl");
	free(hdr);
	hdr = smprint("op=create oid=a oid=b epoch=7 ver=1 wepoch=7 csum=%s",
		zero);
	eqs("a repeated attribute", repler(&cl, Frepl, hdr, nil, 0), "bad ctl");
	free(hdr);
	hdr = smprint("op=create oid=a epoch=7 ver=1x wepoch=7 csum=%s", zero);
	eqs("a u64 that is not digits", repler(&cl, Frepl, hdr, nil, 0),
		"bad ctl");
	free(hdr);
	hdr = smprint("op=create oid=a epoch=7 ver=0 wepoch=7 csum=%s", zero);
	eqs("store.md §3.8's ver=0, refused before any engine call",
		repler(&cl, Frepl, hdr, nil, 0), "bad ctl");
	free(hdr);
	eqs("a csum that is not 64 hex characters", repler(&cl, Frepl,
		"op=create oid=a epoch=7 ver=1 wepoch=7 csum=beef", nil, 0),
		"bad ctl");
	hdr = smprint("op=create oid= epoch=7 ver=1 wepoch=7 csum=%s", zero);
	eqs("an oid outside §1.1", repler(&cl, Frepl, hdr, nil, 0),
		"bad object name");
	free(hdr);
	eqs("a field with no `='", repler(&cl, Frepl, "op=create oid", nil, 0),
		"bad ctl");
	eqs("an empty write", repler(&cl, Frepl, "", nil, 0), "bad ctl");

	/* §5.5: exactly n bytes behind the header, in the same Twrite */
	dcs(cs, "abcd", 4);
	hdr = smprint("op=write oid=a epoch=7 ver=2 wepoch=7 pver=1 pwepoch=7"
		" off=0 n=4 dcsum=%s csum=%s", cs, zero);
	eqs("a payload shorter than n", repler(&cl, Frepl, hdr, "ab", 2),
		"bad ctl");
	eqs("a payload longer than n", repler(&cl, Frepl, hdr, "abcdef", 6),
		"bad ctl");
	free(hdr);
	hdr = smprint("op=create oid=a epoch=7 ver=1 wepoch=7 csum=%s\nx",
		zero);
	eqs("bytes behind a header that defines no payload",
		repler(&cl, Frepl, hdr, nil, 0), "bad ctl");
	free(hdr);

	/* §5.5's epoch rule, which §5.6 shares */
	hdr = smprint("op=create oid=a epoch=6 ver=1 wepoch=7 csum=%s", zero);
	eqs("an epoch below ours", repler(&cl, Frepl, hdr, nil, 0),
		"stale epoch");
	free(hdr);
	hdr = smprint("op=create oid=a epoch=8 ver=1 wepoch=7 csum=%s", zero);
	eqs("an epoch above ours", repler(&cl, Frepl, hdr, nil, 0),
		"future epoch");
	free(hdr);

	srvcount(ctx, &np, &nd);
	eqv("not one of those reached a queue", np, nd);

	/* §5.5 defines no read of /repl: a Tread is end of data */
	if(clwalk1(&cl, Froot, Frepl2, "repl", &r) != Rwalk
	|| clopen(&cl, Frepl2, OREAD, &r) != Ropen)
		fail("open /repl for reading: %s", clerr(&r));
	else if(clread(&cl, Frepl2, 0, 4096, &r) != Rread)
		fail("read /repl: %s", clerr(&r));
	else
		eqv("a Tread of /repl is end of data", r.count, 0);
	clclunk(&cl, Frepl2, &r);
	clclunk(&cl, Frepl, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §5.5's role rule: the attach MUST be role=repl with peer=, and any
 * other role MUST fail `permission denied' — not `bad open mode',
 * which §2.6 defines as an open-mode error.  /rpc takes role=admin as
 * well (§2.1).
 */
static void
troles(void)
{
	char *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;

	clstage = "roles";
	m = mkmap();
	d = newdisk(Tnslots, nil);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, Nclient, &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	checks++;
	if(clwalk1(&cl, Froot, Frepl, "repl", &r) != Rerror
	|| strcmp(r.ename, "permission denied") != 0)
		fail("a client walk to /repl: %s", clerr(&r));
	checks++;
	if(clwalk1(&cl, Froot, Frpc, "rpc", &r) != Rerror
	|| strcmp(r.ename, "permission denied") != 0)
		fail("a client walk to /rpc: %s", clerr(&r));
	clclunk(&cl, Froot, &r);

	/* §5.6's mode rule: ORDWR, since every exchange uses both ways */
	if(clattach(&cl, Froot, Nadmin, &r) != Rattach){
		fail("admin attach: %s", clerr(&r));
		goto Out;
	}
	if(clwalk1(&cl, Froot, Frpc, "rpc", &r) != Rwalk)
		fail("walk /rpc: %s", clerr(&r));
	else{
		eqs("an OREAD open of /rpc", clopen(&cl, Frpc, OREAD, &r) ==
			Ropen ? "ok" : clerr(&r), "bad open mode");
		eqs("an OWRITE open of /rpc", clopen(&cl, Frpc, OWRITE, &r) ==
			Ropen ? "ok" : clerr(&r), "bad open mode");
		eqs("an ORDWR|ORCLOSE open of /rpc",
			clopen(&cl, Frpc, ORDWR|ORCLOSE, &r) == Ropen ? "ok"
			: clerr(&r), "bad open mode");
		eqs("an ORDWR open of /rpc",
			clopen(&cl, Frpc, ORDWR, &r) == Ropen ? "ok"
			: clerr(&r), "ok");
	}
	clclunk(&cl, Frpc, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §5.5's delta operations: op=write and op=trunc apply iff the local
 * committed key equals (pwepoch, pver) exactly, else `out of
 * sequence' (§5.3).  The resulting csum is checked before anything is
 * durable (store.md §3.8, D23), and a mismatch leaves the object as it
 * was — which a restart over the same disk is what proves, since a
 * replay is what would surface a record that had been written.
 */
static void
tdelta(void)
{
	char *m, cs[Csumhexlen], ds[2*Blkdlen+1], *hdr;
	uchar want[64];
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	Objinfo oi;

	clstage = "delta";
	m = mkmap();
	d = newdisk(Tnslots, nil);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	st = srvstore(ctx);
	memset(want, 0, sizeof want);
	memmove(want, "0123456789", 10);
	mkobj(st, "alpha", want, 10, 2);
	clstart(&cl, ctx, Clmsize);
	if(!chopen(&cl, ctx, Nrepl, Froot, Frepl, ~0UL))
		goto Out;

	/* the predecessor rule, in its three directions */
	memmove(want, "ABCDE", 5);
	dcs(ds, "ABCDE", 5);
	ocsum(cs, want, 10);
	hdr = smprint("op=write oid=alpha epoch=7 ver=3 wepoch=7 pver=1"
		" pwepoch=7 off=0 n=5 dcsum=%s csum=%s", ds, cs);
	eqs("a delta whose predecessor key is not ours",
		repler(&cl, Frepl, hdr, "ABCDE", 5), "out of sequence");
	free(hdr);
	hdr = smprint("op=write oid=absent epoch=7 ver=3 wepoch=7 pver=2"
		" pwepoch=7 off=0 n=5 dcsum=%s csum=%s", ds, cs);
	eqs("a delta for an id this receiver holds nothing of",
		repler(&cl, Frepl, hdr, "ABCDE", 5), "out of sequence");
	free(hdr);
	hdr = smprint("op=write oid=alpha epoch=7 ver=3 wepoch=7 pver=2"
		" pwepoch=7 off=0 n=5 dcsum=%s csum=%s", ds, cs);
	eqs("a delta at exactly our key",
		repler(&cl, Frepl, hdr, "ABCDE", 5), "ok");
	free(hdr);
	if(statof(st, "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	else{
		eqv("... adopts the sender's ver verbatim", oi.ver, 3);
		eqv("... and its wepoch", oi.wepoch, Tepoch);
	}

	/* §5.5: verify dcsum over the payload; mismatch is `checksum mismatch' */
	dcs(ds, "xxxxx", 5);
	ocsum(cs, want, 10);
	hdr = smprint("op=write oid=alpha epoch=7 ver=4 wepoch=7 pver=3"
		" pwepoch=7 off=0 n=5 dcsum=%s csum=%s", ds, cs);
	eqs("a payload that is not the dcsum's",
		repler(&cl, Frepl, hdr, "ABCDE", 5), "checksum mismatch");
	free(hdr);

	/* D23: the resulting csum, checked before the record is written */
	dcs(ds, "ZZZZZ", 5);
	hdr = smprint("op=write oid=alpha epoch=7 ver=4 wepoch=7 pver=3"
		" pwepoch=7 off=0 n=5 dcsum=%s csum=%s", ds, cs);
	eqs("a resulting csum that is not the one it names",
		cond(repler(&cl, Frepl, hdr, "ZZZZZ", 5)), "checksum mismatch");
	free(hdr);
	if(statof(st, "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	else
		eqv("... and the refused push changed no key", oi.ver, 3);

	/* store.md §3.8's n=0 write: it commits nothing and adopts no key */
	dcs(ds, "", 0);
	hdr = smprint("op=write oid=alpha epoch=7 ver=9 wepoch=7 pver=3"
		" pwepoch=7 off=0 n=0 dcsum=%s csum=%s", ds, cs);
	eqs("a zero-byte replicated write", repler(&cl, Frepl, hdr, nil, 0),
		"ok");
	free(hdr);
	if(statof(st, "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	else
		eqv("... stays at the receiver's own key", oi.ver, 3);

	/* §2.6's bound, at either end */
	dcs(ds, "ABCDE", 5);
	hdr = smprint("op=write oid=alpha epoch=7 ver=4 wepoch=7 pver=3"
		" pwepoch=7 off=%llud n=5 dcsum=%s csum=%s",
		(uvlong)Tobjmax, ds, cs);
	eqs("a replicated write past objmax", repler(&cl, Frepl, hdr, "ABCDE",
		5), "object too large");
	free(hdr);

	/* op=trunc, the other delta */
	ocsum(cs, want, 4);
	hdr = smprint("op=trunc oid=alpha epoch=7 ver=4 wepoch=7 pver=3"
		" pwepoch=7 len=4 csum=%s", cs);
	eqs("a truncate at exactly our key", repler(&cl, Frepl, hdr, nil, 0),
		"ok");
	free(hdr);
	if(statof(st, "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	else{
		eqv("... applies the length", oi.len, 4);
		eqv("... at the sender's key", oi.ver, 4);
	}
	hdr = smprint("op=trunc oid=alpha epoch=7 ver=5 wepoch=7 pver=3"
		" pwepoch=7 len=2 csum=%s", cs);
	eqs("a truncate behind our key", repler(&cl, Frepl, hdr, nil, 0),
		"out of sequence");
	free(hdr);
	clclunk(&cl, Frepl, &r);
	clclunk(&cl, Froot, &r);
	clstop(&cl);
	srvfree(ctx);

	/*
	 * The refused push left nothing durable, which a restart over the
	 * same disk is what shows: recovery replays the log, so a record
	 * the commit had written would come back here.
	 */
	if((ctx = startsrv(d, m, 0, 0)) == nil){
		devclose(d);
		free(m);
		return;
	}
	if(statof(srvstore(ctx), "alpha", &oi) < 0)
		fail("objstat alpha after a restart: %r");
	else{
		eqv("a replay finds the key the refusals left", oi.ver, 4);
		eqv("... and the length", oi.len, 4);
	}
	srvfree(ctx);
	devclose(d);
	free(m);
	return;
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §5.5's self-contained op=delete, whose arbitration is split three
 * ways (store.md §3.8): objadopt's over an absent id and over an
 * existing tombstone, and the caller's — here — over a live copy.
 */
static void
tdelete(void)
{
	char *m, cs[Csumhexlen], *hdr;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	Objinfo oi;

	clstage = "delete";
	m = mkmap();
	d = newdisk(Tnslots, nil);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	st = srvstore(ctx);
	mkobj(st, "alpha", "hello", 5, 2);
	mkobj(st, "beta", "hello", 5, 2);
	ocsum(cs, nil, 0);
	clstart(&cl, ctx, Clmsize);
	if(!chopen(&cl, ctx, Nrepl, Froot, Frepl, ~0UL))
		goto Out;

	hdr = smprint("op=delete oid=alpha epoch=7 ver=1 wepoch=7 csum=%s", cs);
	eqs("a delete below our live key", repler(&cl, Frepl, hdr, nil, 0),
		"stale version");
	free(hdr);
	hdr = smprint("op=delete oid=alpha epoch=7 ver=2 wepoch=7 csum=%s", cs);
	eqs("a delete at our live key", repler(&cl, Frepl, hdr, nil, 0),
		"stale version");
	free(hdr);
	hdr = smprint("op=delete oid=alpha epoch=7 ver=3 wepoch=7 csum=%s", cs);
	eqs("a delete above our live key", repler(&cl, Frepl, hdr, nil, 0),
		"ok");
	free(hdr);
	if(statof(st, "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	else{
		eqv("... leaves a tombstone", oi.state == Stomb, 1);
		eqv("... at the sender's key", oi.ver, 3);
		eqv("... with len=0", oi.len, 0);
	}

	/* over the tombstone: objadopt's comparison, strictly greater */
	hdr = smprint("op=delete oid=alpha epoch=7 ver=3 wepoch=7 csum=%s", cs);
	eqs("a delete at our tombstone's key",
		cond(repler(&cl, Frepl, hdr, nil, 0)), "stale version");
	free(hdr);
	hdr = smprint("op=delete oid=alpha epoch=7 ver=4 wepoch=7 csum=%s", cs);
	eqs("a delete above our tombstone's key",
		repler(&cl, Frepl, hdr, nil, 0), "ok");
	free(hdr);
	if(statof(st, "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	else
		eqv("... re-keys the tombstone", oi.ver, 4);

	/* an id this receiver holds nothing of: an adoption at any key */
	hdr = smprint("op=delete oid=ghost epoch=7 ver=11 wepoch=7 csum=%s", cs);
	eqs("a delete of an id we hold nothing of",
		repler(&cl, Frepl, hdr, nil, 0), "ok");
	free(hdr);
	if(statof(st, "ghost", &oi) < 0)
		fail("objstat ghost: %r");
	else{
		eqv("... adopts the tombstone", oi.state == Stomb, 1);
		eqv("... at the key it carried", oi.ver, 11);
	}

	/* D14: a live copy that contributes no key defends none */
	if(objcorrupt(st, (uchar*)"beta", 4, 1, nil, 0) < 0)
		fail("objcorrupt beta: %r");
	hdr = smprint("op=delete oid=beta epoch=7 ver=1 wepoch=7 csum=%s", cs);
	eqs("a delete below the key of a corrupt live copy",
		repler(&cl, Frepl, hdr, nil, 0), "ok");
	free(hdr);
	clclunk(&cl, Frepl, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §5.5's op=create, which store.md §3.6 makes this path's zero-length
 * stage: self-contained, arbitrated on (wepoch, ver), and never
 * `object exists' — a replicated create adopts the sender's version
 * verbatim where §5.4's client create chooses its own.
 */
static void
tcreate(void)
{
	char *m, cs[Csumhexlen], *hdr;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	Objinfo oi;

	clstage = "create";
	m = mkmap();
	d = newdisk(Tnslots, nil);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	st = srvstore(ctx);
	mkobj(st, "alpha", "hello", 5, 4);
	ocsum(cs, nil, 0);
	clstart(&cl, ctx, Clmsize);
	if(!chopen(&cl, ctx, Nrepl, Froot, Frepl, ~0UL))
		goto Out;

	hdr = smprint("op=create oid=fresh epoch=7 ver=6 wepoch=7 csum=%s", cs);
	eqs("a create of an id we hold nothing of",
		repler(&cl, Frepl, hdr, nil, 0), "ok");
	free(hdr);
	if(statof(st, "fresh", &oi) < 0)
		fail("objstat fresh: %r");
	else{
		eqv("... adopts the sender's version", oi.ver, 6);
		eqv("... at length 0", oi.len, 0);
	}
	hdr = smprint("op=create oid=alpha epoch=7 ver=4 wepoch=7 csum=%s", cs);
	eqs("a create at our live key, never `object exists'",
		repler(&cl, Frepl, hdr, nil, 0), "stale version");
	free(hdr);
	hdr = smprint("op=create oid=alpha epoch=7 ver=3 wepoch=7 csum=%s", cs);
	eqs("a create below our live key", repler(&cl, Frepl, hdr, nil, 0),
		"stale version");
	free(hdr);
	hdr = smprint("op=create oid=alpha epoch=7 ver=5 wepoch=7 csum=%s", cs);
	eqs("a create above our live key", repler(&cl, Frepl, hdr, nil, 0),
		"ok");
	free(hdr);
	if(statof(st, "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	else{
		eqv("... replaces the copy at the sender's key", oi.ver, 5);
		eqv("... with a zero-length object", oi.len, 0);
	}
	clclunk(&cl, Frepl, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §5.5's op=full: a whole-object resync staged across many Twrites and
 * committed only at final=1, where the comparison against the
 * receiver's then-current key is made (store.md §3.6).
 */
static void
tfull(void)
{
	char *m, cs[Csumhexlen], d0[2*Blkdlen+1], d1[2*Blkdlen+1], *h0, *h1;
	uchar want[2*Tblksz], buf[2*Tblksz];
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	Objinfo oi;
	uvlong live, done;
	int i;

	clstage = "full";
	m = mkmap();
	d = newdisk(Tnslots, nil);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	st = srvstore(ctx);
	for(i = 0; i < sizeof want; i++)
		want[i] = i*7 + 3;
	ocsum(cs, want, sizeof want);
	dcs(d0, want, Tblksz);
	dcs(d1, want+Tblksz, Tblksz);
	mkobj(st, "alpha", "hello", 5, 2);
	clstart(&cl, ctx, Clmsize);
	if(!chopen(&cl, ctx, Nrepl, Froot, Frepl, ~0UL))
		goto Out;

	h0 = smprint("op=full oid=alpha epoch=7 ver=3 wepoch=7 len=%d off=0"
		" n=%d dcsum=%s csum=%s final=0", 2*Tblksz, Tblksz, d0, cs);
	h1 = smprint("op=full oid=alpha epoch=7 ver=3 wepoch=7 len=%d off=%d"
		" n=%d dcsum=%s csum=%s final=1", 2*Tblksz, Tblksz, Tblksz,
		d1, cs);
	eqs("the first chunk of a resync", repler(&cl, Frepl, h0, want, Tblksz),
		"ok");
	srvstagecount(ctx, &live, &done, nil);
	eqv("... leaves a stage on the fid", live, 1);
	eqs("the final chunk", repler(&cl, Frepl, h1, want+Tblksz, Tblksz),
		"ok");
	srvstagecount(ctx, &live, &done, nil);
	eqv("... consumes it", live, 0);
	if(statof(st, "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	else{
		eqv("the resync commits at the sender's key", oi.ver, 3);
		eqv("... and its length", oi.len, 2*Tblksz);
	}
	if(objread(st, (uchar*)"alpha", 5, buf, sizeof buf, 0) != sizeof buf)
		fail("objread alpha: %r");
	else
		eqv("... and every staged byte", memcmp(buf, want, sizeof buf),
			0);

	/*
	 * A concurrent local update between the chunks is what the
	 * commit-time comparison exists to catch (§5.5).
	 */
	eqs("the first chunk of a second resync",
		repler(&cl, Frepl, h0, want, Tblksz), "ok");
	if(objwrite(st, (uchar*)"alpha", 5, "zz", 2, 0, 9, Tepoch, nil, 0) < 0)
		fail("a local write between chunks: %r");
	eqs("... whose final=1 arrives behind a local update",
		repler(&cl, Frepl, h1, want+Tblksz, Tblksz), "stale version");
	srvstagecount(ctx, &live, nil, nil);
	eqv("a refused final=1 consumes the stage all the same", live, 0);
	free(h0);
	free(h1);

	/* §5.5's force=1: the divergence repair, at an equal key */
	if(statof(st, "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	h0 = smprint("op=full oid=alpha epoch=7 ver=%llud wepoch=%llud"
		" len=%d off=0 n=%d dcsum=%s csum=%s final=1", oi.ver,
		oi.wepoch, 2*Tblksz, Tblksz, d0, cs);
	eqs("an op=full at an equal key", repler(&cl, Frepl, h0, want, Tblksz),
		"stale version");
	free(h0);
	h0 = smprint("op=full oid=alpha epoch=7 ver=%llud wepoch=%llud"
		" len=%d off=0 n=%d dcsum=%s csum=%s final=0 force=1", oi.ver,
		oi.wepoch, 2*Tblksz, Tblksz, d0, cs);
	h1 = smprint("op=full oid=alpha epoch=7 ver=%llud wepoch=%llud"
		" len=%d off=%d n=%d dcsum=%s csum=%s final=1 force=1", oi.ver,
		oi.wepoch, 2*Tblksz, Tblksz, Tblksz, d1, cs);
	eqs("... with force=1, first chunk",
		repler(&cl, Frepl, h0, want, Tblksz), "ok");
	eqs("... and its final=1 at the same key",
		repler(&cl, Frepl, h1, want+Tblksz, Tblksz), "ok");
	if(statof(st, "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	else
		eqv("the repair keeps the key it carried", oi.len, 2*Tblksz);

	/* a chunk outside the transfer's declared length (store.md §3.7) */
	free(h1);
	h1 = smprint("op=full oid=alpha epoch=7 ver=99 wepoch=7 len=%d off=%d"
		" n=%d dcsum=%s csum=%s final=1", Tblksz, Tblksz, Tblksz, d1,
		cs);
	eqs("a chunk outside its stage's declared length",
		repler(&cl, Frepl, h1, want+Tblksz, Tblksz), "bad ctl");
	free(h0);
	free(h1);

	/* §5.5: `len' and `force' MUST be identical on every chunk */
	h0 = smprint("op=full oid=gamma epoch=7 ver=3 wepoch=7 len=%d off=0"
		" n=%d dcsum=%s csum=%s final=0", 2*Tblksz, Tblksz, d0, cs);
	h1 = smprint("op=full oid=gamma epoch=7 ver=3 wepoch=7 len=%d off=%d"
		" n=%d dcsum=%s csum=%s final=1", 3*Tblksz, Tblksz, Tblksz, d1,
		cs);
	eqs("a transfer of a second object", repler(&cl, Frepl, h0, want,
		Tblksz), "ok");
	eqs("a chunk whose len is not the transfer's",
		repler(&cl, Frepl, h1, want+Tblksz, Tblksz), "bad ctl");
	free(h1);

	/* a second transfer on one fid: §3.6's per-fid bound, `disk full' */
	h1 = smprint("op=full oid=delta epoch=7 ver=3 wepoch=7 len=%d off=0"
		" n=%d dcsum=%s csum=%s final=0", 2*Tblksz, Tblksz, d0, cs);
	eqs("a second object staged on one fid",
		repler(&cl, Frepl, h1, want, Tblksz), "disk full");
	free(h0);
	free(h1);

	/* §5.5: the stage is discarded when the /repl fid is clunked */
	srvstagecount(ctx, &live, nil, nil);
	eqv("the abandoned transfer is still staged", live, 1);
	clclunk(&cl, Frepl, &r);
	srvstagecount(ctx, &live, &done, nil);
	eqv("the clunk discards it", live, 0);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * D14's corrupt-receiver exemption (§5.5, store.md §3.6): a receiver
 * whose own copy fails local verification treats it as absent for the
 * comparison and takes an op=full at any key — greater, equal or lower
 * — and the commit clears the flag.
 */
static void
tcorrupt(void)
{
	char *m, cs[Csumhexlen], ds[2*Blkdlen+1], *hdr, *resp;
	uchar want[Tblksz];
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	Objinfo oi;
	int i;

	clstage = "corrupt";
	m = mkmap();
	d = newdisk(Tnslots, nil);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	st = srvstore(ctx);
	for(i = 0; i < sizeof want; i++)
		want[i] = i + 1;
	ocsum(cs, want, sizeof want);
	dcs(ds, want, sizeof want);
	mkobj(st, "alpha", "hello", 5, 9);
	if(objcorrupt(st, (uchar*)"alpha", 5, 1, nil, 0) < 0)
		fail("objcorrupt: %r");
	clstart(&cl, ctx, Clmsize);
	if(!chopen(&cl, ctx, Nrepl, Froot, Frepl, Frpc))
		goto Out;

	resp = rpc(&cl, Frpc, "op=meta oid=alpha epoch=7", nil);
	istrue("§5.6's third meta form names the flag",
		strstr(resp, " corrupt=1") != nil);
	istrue("... and carries the key the instance holds",
		strstr(resp, " ver=9 ") != nil);

	hdr = smprint("op=full oid=alpha epoch=7 ver=2 wepoch=7 len=%d off=0"
		" n=%d dcsum=%s csum=%s final=1", Tblksz, Tblksz, ds, cs);
	eqs("a repair push at a LOWER key than the corrupt copy's",
		repler(&cl, Frepl, hdr, want, sizeof want), "ok");
	free(hdr);
	if(statof(st, "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	else{
		eqv("... is taken at the key it carried", oi.ver, 2);
		eqv("... and the commit clears the flag", oi.corrupt, 0);
	}
	/* the exemption ends with the push that used it */
	hdr = smprint("op=full oid=alpha epoch=7 ver=1 wepoch=7 len=%d off=0"
		" n=%d dcsum=%s csum=%s final=1", Tblksz, Tblksz, ds, cs);
	eqs("a second push at a lower key is refused like any other",
		repler(&cl, Frepl, hdr, want, sizeof want), "stale version");
	free(hdr);
	clclunk(&cl, Frpc, &r);
	clclunk(&cl, Frepl, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §3.6's per-fid bound: at most `stagemax' grains staged on one /repl
 * fid, and a chunk that would exceed it fails `disk full'.  A chunk's
 * offset is the sender's, so shortening it would publish a hole, which
 * is why this bound refuses where a client write is shortened
 * (store.md §14(37)).
 */
static void
tstagemax(void)
{
	char *m, cs[Csumhexlen], d0[2*Blkdlen+1], d1[2*Blkdlen+1], *h0, *h1;
	uchar want[2*Tblksz];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	int i;

	clstage = "stagemax";
	m = mkmap();
	d = newdisk(Tnslots, nil);
	if((ctx = startsrv(d, m, 1, 0)) == nil)	/* one grain to a fid */
		return;
	for(i = 0; i < sizeof want; i++)
		want[i] = i;
	ocsum(cs, want, sizeof want);
	dcs(d0, want, Tblksz);
	dcs(d1, want+Tblksz, Tblksz);
	clstart(&cl, ctx, Clmsize);
	if(!chopen(&cl, ctx, Nrepl, Froot, Frepl, ~0UL))
		goto Out;
	h0 = smprint("op=full oid=alpha epoch=7 ver=3 wepoch=7 len=%d off=0"
		" n=%d dcsum=%s csum=%s final=0", 2*Tblksz, Tblksz, d0, cs);
	h1 = smprint("op=full oid=alpha epoch=7 ver=3 wepoch=7 len=%d off=%d"
		" n=%d dcsum=%s csum=%s final=1", 2*Tblksz, Tblksz, Tblksz, d1,
		cs);
	eqs("a first chunk within the bound",
		repler(&cl, Frepl, h0, want, Tblksz), "ok");
	eqs("a chunk that would take the fid past stagemax",
		repler(&cl, Frepl, h1, want+Tblksz, Tblksz), "disk full");
	free(h0);
	free(h1);
	clclunk(&cl, Frepl, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §5.5's other two lifetimes: the idle sweep, and a Tflush of a chunk
 * — layer-a §5.4.1 step 7, which discards whatever the fid holds.  A
 * chunk or a final=1 on a stage either of them took is refused `stage
 * expired' (§3.6), which carries no §2.6 prefix.
 */
static void
tstagelife(void)
{
	char *m, cs[Csumhexlen], d0[2*Blkdlen+1], *h0, *h1;
	uchar want[2*Tblksz];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, tf, r;
	uvlong live, done, openat, np0, nd0, np, nd;
	ushort ta, ft;
	int i;

	clstage = "stagelife";
	m = mkmap();
	d = newdisk(Tnslots, nil);
	if((ctx = startsrv(d, m, 0, 50)) == nil)
		return;
	for(i = 0; i < sizeof want; i++)
		want[i] = i;
	ocsum(cs, want, sizeof want);
	dcs(d0, want, Tblksz);
	h0 = smprint("op=full oid=alpha epoch=7 ver=3 wepoch=7 len=%d off=0"
		" n=%d dcsum=%s csum=%s final=0", 2*Tblksz, Tblksz, d0, cs);
	h1 = smprint("op=full oid=alpha epoch=7 ver=3 wepoch=7 len=%d off=%d"
		" n=%d dcsum=%s csum=%s final=1", 2*Tblksz, Tblksz, Tblksz, d0,
		cs);
	clstart(&cl, ctx, Clmsize);
	if(!chopen(&cl, ctx, Nrepl, Froot, Frepl, ~0UL))
		goto Out;

	/* the idle sweep, after stagems of no arrivals */
	eqs("a chunk that will be left idle",
		repler(&cl, Frepl, h0, want, Tblksz), "ok");
	sleep(200);
	eqs("a chunk on a stage the sweep expired",
		repler(&cl, Frepl, h1, want+Tblksz, Tblksz),
		"shoalsrv: stage expired");
	srvstagecount(ctx, &live, &done, &openat);
	eqv("the sweep gave its reservations back", done, 1);
	eqv("... with the store open", openat, 1);
	eqv("and the refusal took the dead stage out of the slot", live, 0);
	eqs("so the transfer starts over on the same fid",
		repler(&cl, Frepl, h0, want, Tblksz), "ok");

	/*
	 * A Tflush of a chunk: step 7 discards whatever the fid holds,
	 * and the chunk behind it is told the transfer is over.
	 */
	waitidle(ctx, &np0, &nd0);
	srvhook(ctx, "objhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Frepl;
	t.offset = 0;
	t.count = strlen(h1) + 1 + Tblksz;
	t.data = malloc(t.count);
	if(t.data == nil)
		sysfatal("malloc: %r");
	memmove(t.data, h1, strlen(h1));
	t.data[strlen(h1)] = '\n';
	memmove(t.data + strlen(h1) + 1, want+Tblksz, Tblksz);
	clput(&cl, &t);
	waitpush(ctx, np0, 1, &np, &nd);
	eqv("a chunk is held in the pool", np - np0, 1);
	memset(&tf, 0, sizeof tf);
	tf.type = Tflush;
	tf.tag = ft = cltag(&cl);
	tf.oldtag = ta;
	clput(&cl, &tf);
	srvhook(ctx, "objhold", 0);
	if(clgettag(&cl, ta, &r) < 0)
		fail("no answer to the flushed chunk");
	else
		eqs("a flushed chunk", clerr(&r), "interrupted");
	cltagfree(&cl, ta);
	if(clgettag(&cl, ft, &r) != Rflush)
		fail("no Rflush: %s", clerr(&r));
	cltagfree(&cl, ft);
	free(t.data);
	srvstagecount(ctx, &live, nil, nil);
	eqv("step 7 discarded the fid's stage", live, 0);
	eqs("and the chunk behind it is told the transfer is over",
		repler(&cl, Frepl, h1, want+Tblksz, Tblksz),
		"shoalsrv: stage expired");
	free(h0);
	free(h1);
	clclunk(&cl, Frepl, &r);
	clclunk(&cl, Froot, &r);
Out:
	srvhook(ctx, "objhold", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §5.6's channel rules: one outstanding request per fid, a response
 * prepared at Twrite time and delivered by exactly one Tread, and a
 * read with nothing buffered answering count 0.
 */
static void
tchannel(void)
{
	char *m, *resp;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	uvlong np0, nd0, np, nd;
	ushort ta;
	long n;

	clstage = "channel";
	m = mkmap();
	d = newdisk(Tnslots, nil);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", "hello", 5, 2);
	clstart(&cl, ctx, Clmsize);
	if(!chopen(&cl, ctx, Nrepl, Froot, ~0UL, Frpc))
		goto Out;

	if(clread(&cl, Frpc, 0, 4096, &r) != Rread)
		fail("a read with nothing buffered: %s", clerr(&r));
	else
		eqv("a Tread with no response buffered", r.count, 0);

	resp = rpc(&cl, Frpc, "op=meta oid=alpha epoch=7", &n);
	istrue("a response is delivered by one Tread", n > 0);
	USED(resp);
	if(clread(&cl, Frpc, 0, 4096, &r) != Rread)
		fail("a second read: %s", clerr(&r));
	else
		eqv("a Tread after the response was delivered", r.count, 0);

	/* the offset is ignored in both directions */
	if(chanop(&cl, Frpc, "op=meta oid=alpha epoch=7", nil, 0, &r) != Rwrite)
		fail("a request at a non-zero offset: %s", clerr(&r));
	if(clread(&cl, Frpc, 4096, 4096, &r) != Rread)
		fail("a read at a non-zero offset: %s", clerr(&r));
	else
		istrue("a Tread's offset is ignored", r.count > 0
			&& strncmp(r.data, "meta oid=alpha", 14) == 0);

	/*
	 * A second Twrite before the response is read is `bad ctl'.  The
	 * first is held in the pool, so the second meets a request that
	 * is genuinely outstanding.
	 */
	waitidle(ctx, &np0, &nd0);
	srvhook(ctx, "objhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Frpc;
	t.data = "op=meta oid=alpha epoch=7\n";
	t.count = strlen(t.data);
	clput(&cl, &t);
	waitpush(ctx, np0, 1, &np, &nd);
	eqs("a second request while the first is outstanding",
		rpc(&cl, Frpc, "op=meta oid=alpha epoch=7", nil), "bad ctl");
	srvhook(ctx, "objhold", 0);
	if(clgettag(&cl, ta, &r) != Rwrite)
		fail("the first request: %s", clerr(&r));
	cltagfree(&cl, ta);
	if(clread(&cl, Frpc, 0, 4096, &r) != Rread)
		fail("draining the first response: %s", clerr(&r));
	if(chanop(&cl, Frpc, "op=meta oid=alpha epoch=7", nil, 0, &r) != Rwrite)
		fail("a request whose response is left unread: %s", clerr(&r));
	eqs("a second request while the response is unread",
		chanop(&cl, Frpc, "op=meta oid=alpha epoch=7", nil, 0, &r)
		== Rwrite ? "ok" : clerr(&r), "bad ctl");
	if(clread(&cl, Frpc, 0, 4096, &r) != Rread)
		fail("draining: %s", clerr(&r));
	else
		istrue("... and the buffered response survived the refusal",
			r.count > 0);
	clclunk(&cl, Frpc, &r);
	clclunk(&cl, Froot, &r);
Out:
	srvhook(ctx, "objhold", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §5.6's op=meta, op=get, op=verify and op=discard.
 */
static void
trpcops(void)
{
	char *m, *resp, *req, want[64];
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	Objinfo oi;
	long n;

	clstage = "rpcops";
	m = mkmap();
	d = newdisk(Tnslots, nil);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	st = srvstore(ctx);
	memset(want, 0, sizeof want);
	memmove(want, "0123456789", 10);
	mkobj(st, "alpha", want, 10, 2);
	mkobj(st, "gone", nil, 0, 1);
	if(objremove(st, (uchar*)"gone", 4, 5, Tepoch, nil, 0) < 0)
		fail("objremove gone: %r");
	clstart(&cl, ctx, Clmsize);
	if(!chopen(&cl, ctx, Nadmin, Froot, ~0UL, Frpc))
		goto Out;

	/* op=meta, in its two ordinary forms */
	if(statof(st, "alpha", &oi) < 0)
		fail("objstat alpha: %r");
	resp = rpc(&cl, Frpc, "op=meta oid=alpha epoch=7", nil);
	istrue("op=meta names the key", strstr(resp, "ver=2 wepoch=7") != nil);
	istrue("... the length", strstr(resp, "len=10") != nil);
	istrue("... the state", strstr(resp, "state=live") != nil);
	istrue("... and cur=0, no check having been made",
		strstr(resp, "cur=0") != nil);
	eqs("op=meta of an id this instance holds nothing of",
		line1(rpc(&cl, Frpc, "op=meta oid=nothing epoch=7", nil)),
		"meta oid=nothing absent=1");
	resp = rpc(&cl, Frpc, "op=meta oid=gone epoch=7", nil);
	istrue("op=meta of a tombstone", strstr(resp, "state=tomb") != nil);

	/* op=get, which carries the expected key */
	req = smprint("op=get oid=alpha epoch=7 ver=2 wepoch=7 off=0 n=10");
	resp = rpc(&cl, Frpc, req, &n);
	istrue("op=get answers the line and the bytes",
		n > 10 && memcmp(resp + n - 10, want, 10) == 0);
	istrue("... naming what it read", strstr(resp, "n=10") != nil);
	free(req);
	eqs("op=get at a key that is not ours",
		line1(rpc(&cl, Frpc,
			"op=get oid=alpha epoch=7 ver=3 wepoch=7 off=0 n=10",
			nil)), "stale version");
	eqs("op=get of an absent id",
		line1(rpc(&cl, Frpc,
			"op=get oid=nothing epoch=7 ver=1 wepoch=7 off=0 n=1",
			nil)), "no such object");
	eqs("op=get of a tombstone",
		line1(rpc(&cl, Frpc,
			"op=get oid=gone epoch=7 ver=5 wepoch=7 off=0 n=1",
			nil)), "object deleted");
	eqs("op=get of more than the negotiated msize holds",
		line1(rpc(&cl, Frpc,
			"op=get oid=alpha epoch=7 ver=2 wepoch=7 off=0 n=65536",
			nil)), "bad ctl");

	/* op=verify */
	eqs("op=verify of a whole copy",
		line1(rpc(&cl, Frpc, "op=verify oid=alpha epoch=7", nil)),
		"ok op=verify oid=alpha");
	eqs("op=verify of an id we hold nothing of",
		line1(rpc(&cl, Frpc, "op=verify oid=nothing epoch=7", nil)),
		"no such object");

	/* op=discard: §1.5's three receiver checks */
	eqs("op=discard of a live copy",
		cond(line1(rpc(&cl, Frpc,
			"op=discard oid=alpha epoch=7 ver=2 wepoch=7", nil))),
		"not discardable");
	eqs("op=discard at a key that is not the tombstone's",
		cond(line1(rpc(&cl, Frpc,
			"op=discard oid=gone epoch=7 ver=4 wepoch=7", nil))),
		"not discardable");
	eqs("op=discard whose epoch is not above the tombstone's wepoch",
		cond(line1(rpc(&cl, Frpc,
			"op=discard oid=gone epoch=7 ver=5 wepoch=7", nil))),
		"not discardable");
	eqs("op=discard of an id we hold nothing of",
		line1(rpc(&cl, Frpc,
			"op=discard oid=nothing epoch=7 ver=1 wepoch=1", nil)),
		"no such object");
	clclunk(&cl, Frpc, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §5.6's op=discard at an epoch above the tombstone's wepoch, which is
 * §1.5's third check met — and op=drop with §7.4's re-check against
 * this instance's own map.
 */
static void
tdropdiscard(void)
{
	char *m, *resp, name[32];
	Cinst *pl[Maxplace];
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	Objinfo oi;
	char *mine, *stray, *stray2;
	int i, n;

	clstage = "dropdiscard";
	m = mkmap();
	d = newdisk(Tnslots, nil);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	st = srvstore(ctx);
	/*
	 * §7.4's guard is against P(oid) under this instance's own map,
	 * so the case needs one id this instance places and one it does
	 * not; which ids those are is the placement hash's to say.
	 */
	mine = stray = stray2 = nil;
	for(i = 0; i < 64 && (mine == nil || stray == nil || stray2 == nil);
		i++){
		snprint(name, sizeof name, "o%d", i);
		if((n = mapplace(srvmap(ctx), name, pl, nelem(pl))) < 1)
			continue;
		if(strcmp(pl[0]->iid, srviid(ctx)) == 0){
			if(mine == nil)
				mine = strdup(name);
		}else if(stray == nil)
			stray = strdup(name);
		else if(stray2 == nil)
			stray2 = strdup(name);
	}
	if(mine == nil || stray == nil || stray2 == nil){
		fail("no placed and two unplaced ids in 64 tries");
		goto Out;
	}
	mkobj(st, mine, "hello", 5, 2);
	mkobj(st, stray, "hello", 5, 2);
	mkobj(st, stray2, nil, 0, 1);
	if(objremove(st, (uchar*)stray2, strlen(stray2), 5, 6, nil, 0) < 0)
		fail("objremove: %r");
	mkobj(st, "gone", nil, 0, 1);
	if(objremove(st, (uchar*)"gone", 4, 5, 6, nil, 0) < 0)
		fail("objremove gone: %r");
	clstart(&cl, ctx, Clmsize);
	if(!chopen(&cl, ctx, Nrepl, Froot, ~0UL, Frpc))
		goto Out;

	snprint(name, sizeof name, "op=drop oid=%s epoch=7", mine);
	eqs("op=drop of a copy this instance is placed for",
		line1(rpc(&cl, Frpc, name, nil)), "still placed");
	snprint(name, sizeof name, "op=drop oid=%s epoch=7", stray);
	resp = line1(rpc(&cl, Frpc, name, nil));
	snprint(name, sizeof name, "ok op=drop oid=%s", stray);
	eqs("op=drop of a stray copy", resp, name);
	checks++;
	if(statof(st, stray, &oi) >= 0)
		fail("the dropped copy is still here");
	snprint(name, sizeof name, "op=drop oid=%s epoch=7", stray2);
	eqs("op=drop of a tombstoned id, which is a record and not a copy",
		cond(line1(rpc(&cl, Frpc, name, nil))), "no such object");

	/* §1.5's discard, with all three checks met */
	eqs("op=discard of a tombstone at its key, below the epoch",
		line1(rpc(&cl, Frpc, "op=discard oid=gone epoch=7 ver=5"
			" wepoch=6", nil)), "ok op=discard oid=gone");
	checks++;
	if(statof(st, "gone", &oi) >= 0)
		fail("the discarded tombstone is still here");
	clclunk(&cl, Frpc, &r);
	clclunk(&cl, Froot, &r);
Out:
	free(mine);
	free(stray);
	free(stray2);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §5.6's op=list: the inventory paged in oid byte order, resuming
 * after `after=', with `lines=' counting advert lines and `more=1'
 * answering a clamp as well as inventory that follows (store.md §3.8).
 */
static void
tlist(void)
{
	char *m, *resp, req[256], name[80], last[80], *p, *e;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	long n;
	int i, lines, more, total;

	clstage = "list";
	m = mkmap();
	d = newdisk(Tnslots, nil);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	for(i = 0; i < 60; i++){
		snprint(name, sizeof name,
			"obj%02d-padded-to-make-the-line-long-enough-to-clamp"
			"-a-page", i);
		mkobj(srvstore(ctx), name, nil, 0, 1);
	}
	clstart(&cl, ctx, Clmsize);
	if(!chopen(&cl, ctx, Nrepl, Froot, ~0UL, Frpc))
		goto Out;

	/* a page of two, which leaves inventory behind it */
	resp = rpc(&cl, Frpc, "op=list epoch=7 n=2", &n);
	eqs("a page of two", line1(resp), "list lines=2 more=1");
	p = strchr(resp, '\n') + 1;
	istrue("... whose first line is the smallest oid",
		strncmp(p, "oid=obj00-", 10) == 0);

	/* paging with after= neither duplicates nor skips */
	last[0] = 0;
	total = 0;
	for(i = 0; i < 40; i++){
		if(last[0] == 0)
			snprint(req, sizeof req, "op=list epoch=7 n=7");
		else
			snprint(req, sizeof req, "op=list epoch=7 after=%s n=7",
				last);
		resp = rpc(&cl, Frpc, req, &n);
		if(n < 0){
			fail("op=list: %s", resp);
			break;
		}
		if(strncmp(resp, "list lines=", 11) != 0){
			fail("op=list answered %#q", line1(resp));
			break;
		}
		lines = atoi(resp + 11);
		more = strstr(resp, " more=1") != nil;
		total += lines;
		p = strchr(resp, '\n');
		while(lines-- > 0 && p != nil){
			p++;
			if((e = strchr(p, '\n')) == nil)
				break;
			if(strncmp(p, "oid=", 4) != 0){
				fail("a listing line: %#q", p);
				break;
			}
			*e = 0;
			if((strchr(p, ' ')) != nil)
				*strchr(p, ' ') = 0;
			strecpy(last, last + sizeof last, p + 4);
			p = e;
		}
		if(!more)
			break;
	}
	eqv("paging with after= covers the inventory once", total, 60);

	/* the msize clamp, which is `more=1' as well */
	resp = rpc(&cl, Frpc, "op=list epoch=7 n=256", &n);
	lines = atoi(resp + 11);
	istrue("a page is clamped to the negotiated msize",
		lines > 0 && lines < 60);
	istrue("... and the whole response fits one Tread",
		n <= 8192 && n > 0);
	istrue("... and says there is more", strstr(resp, " more=1") != nil);
	clclunk(&cl, Frpc, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §3.6's `staged=<grains>' in /status, which arrives with the surface
 * whose stages reserve (store.md §14(37)), and §7.5's op=verify over a
 * copy that really is damaged.
 */
static void
damagedigest(Dev *d, Super *sup, ulong emapslot, ulong blk)
{
	uchar *p, *dig;
	int i;

	if((p = malloc(sup->emapsz)) == nil)
		sysfatal("malloc: %r");
	simpeek(d, emapentoff(sup, emapslot), p, sup->emapsz);
	dig = emapdig(p, sup->nblkmax, blk);
	for(i = 0; i < Blkdlen; i++)
		dig[i] = ~dig[i];
	reccsumset(p, sup->emapsz, 0);
	simpoke(d, emapentoff(sup, emapslot), p, sup->emapsz);
	free(p);
}

static void
tstaged(void)
{
	char *m, cs[Csumhexlen], d0[2*Blkdlen+1], *hdr, buf[8192], val[64];
	uchar want[2*Tblksz];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	int i;

	clstage = "staged";
	m = mkmap();
	d = newdisk(Tnslots, nil);
	if((ctx = startsrv(d, m, 0, 0)) == nil)
		return;
	for(i = 0; i < sizeof want; i++)
		want[i] = i*3;
	ocsum(cs, want, sizeof want);
	dcs(d0, want, Tblksz);
	clstart(&cl, ctx, Clmsize);
	if(!chopen(&cl, ctx, Nrepl, Froot, Frepl, ~0UL))
		goto Out;
	if(clattach(&cl, Frepl2, Nadmin, &r) != Rattach){
		fail("admin attach: %s", clerr(&r));
		goto Out;
	}
	hdr = smprint("op=full oid=beta epoch=7 ver=3 wepoch=7 len=%d off=0"
		" n=%d dcsum=%s csum=%s final=0", 2*Tblksz, Tblksz, d0, cs);
	eqs("a chunk that reserves a grain",
		repler(&cl, Frepl, hdr, want, Tblksz), "ok");
	free(hdr);
	if(clwalk1(&cl, Frepl2, Frpc2, "status", &r) != Rwalk
	|| clopen(&cl, Frpc2, OREAD, &r) != Ropen)
		fail("open /status: %s", clerr(&r));
	else if(clslurp(&cl, Frpc2, buf, sizeof buf) < 0)
		fail("read /status: %r");
	else
		eqs("/status reports the grains staged",
			clfield(buf, "staged", val, sizeof val), "1");
	clclunk(&cl, Frpc2, &r);
	clclunk(&cl, Frepl, &r);
	clclunk(&cl, Frepl2, &r);
	clclunk(&cl, Froot, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

static void
tverifybad(void)
{
	char *m;
	uchar want[2*Tblksz];
	Storecfg scfg;
	Srvctx *ctx;
	Store *s;
	Dev *d;
	Super sb;
	Cl cl;
	Fcall r;
	Objinfo oi;
	ulong emapslot;
	int i;

	clstage = "verifybad";
	m = mkmap();
	d = newdisk(Tnslots, &sb);
	for(i = 0; i < sizeof want; i++)
		want[i] = i*5 + 1;
	/*
	 * The copy is damaged with the store shut, so nothing of it is
	 * cached when the server opens it: what op=verify re-hashes is
	 * what the device holds.
	 */
	memset(&scfg, 0, sizeof scfg);
	scfg.spawn = tspawn;
	scfg.nockptproc = 1;
	if((s = storeopen(d, &scfg)) == nil){
		fail("storeopen: %r");
		free(m);
		devclose(d);
		return;
	}
	if(objcreate(s, (uchar*)"alpha", 5, 1, Tepoch, nil, 0, &oi) < 0
	|| objwrite(s, (uchar*)"alpha", 5, want, sizeof want, 0, 2, Tepoch,
		nil, 0) < 0)
		fail("making the copy: %r");
	if(objstat(s, (uchar*)"alpha", 5, &oi) < 0)
		fail("objstat alpha: %r");
	emapslot = oi.emapslot;
	/*
	 * The entry has to be IN the extent-map region to be damaged
	 * there: until a checkpoint it lives only in the log (§2.8), and
	 * start-up would replay an undamaged one over the damage.
	 */
	if(storecheckpoint(s) < 0)
		fail("storecheckpoint: %r");
	storeclose(s);
	damagedigest(d, &sb, emapslot, 0);

	if((ctx = startsrv(d, m, 0, 0)) == nil){
		free(m);
		devclose(d);
		return;
	}
	clstart(&cl, ctx, Clmsize);
	if(!chopen(&cl, ctx, Nrepl, Froot, ~0UL, Frpc))
		goto Out;
	eqs("op=verify of a copy whose digests do not hash to its csum",
		line1(rpc(&cl, Frpc, "op=verify oid=alpha epoch=7", nil)),
		"checksum mismatch");
	clclunk(&cl, Frpc, &r);
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
	quotefmtinstall();
	clwatchms = 120*1000;
	clwatchon();

	tgrammar();
	troles();
	tdelta();
	tdelete();
	tcreate();
	tfull();
	tcorrupt();
	tstagemax();
	tstagelife();
	tchannel();
	trpcops();
	tdropdiscard();
	tlist();
	tstaged();
	tverifybad();

	clwatchoff();
	if(fails > 0){
		fprint(2, "srvwiretest: %d of %d checks failed\n", fails, checks);
		threadexitsall("fail");
	}
	print("srvwiretest: %d checks ok\n", checks);
	threadexitsall(nil);
}
