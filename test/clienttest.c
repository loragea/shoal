#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../lib/shoal.h"
#include "../srv/srv.h"

/*
 * T1: the 9P client in lib/ninep.c — msize negotiation and what is
 * reported of it, the ordinary exchanges of layer-a §2, the bounded
 * wait every call takes, §5.6's one-outstanding-per-fid rule, a
 * Tflush, a peer that hangs up and a peer that breaks 9P, and a close
 * that reclaims everything with work still in flight.
 *
 * Two peers are driven.  Most of it runs against a whole storage
 * instance inside this program — a simulated disk, the store engine
 * over it and srv/libshoalsrv.a over that, on the far end of a pair of
 * pipes — because what a client must get right is what a real server
 * answers, hold points included.  The two cases about a BROKEN peer
 * run against a stub of a few lines instead: a hang-up and a reply on
 * a tag nobody sent are things a conforming server never does, and a
 * stub is the only way to make it do them.
 *
 * This program does not include test/srv9p.h.  That file is the raw
 * in-process 9P client the §2 cases drive, and this one exists to
 * drive lib/ninep.c in its place; the watchdog below is its own for
 * the same reason.
 *
 * The geometry is store.md §13's small one with objmax raised to 2^20,
 * as srvtest's is: layer-a §1.2 makes 2^20 the floor for a legal map
 * and §14(8) makes the server refuse a map whose objmax is not the
 * disk's.
 */

int mainstacksize = Srvstack;

static int fails;
static int checks;
static char *stage = "starting";

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

/*
 * The whole program's budget.  A client that never answers is exactly
 * what this program is about, so a wedge here must fail the test
 * rather than hang `mk test': the watchdog ends the program through
 * threadexitsall, which takes the server's procs, the client's two and
 * this program's own down with it.
 */
enum
{
	Watchms	= 120*1000,
};

static int wdstop;

static void
watch(void*)
{
	int i;

	for(i = 0; i < Watchms/50; i++){
		if(wdstop)
			threadexits(nil);
		sleep(50);
	}
	fprint(2, "FAIL: wedged at stage %s: ending the test\n", stage);
	threadexitsall("wedged");
}

static void
watchon(void)
{
	wdstop = 0;
	proccreate(watch, nil, 8192);
}

static void
watchoff(void)
{
	wdstop = 1;
}

enum
{
	Tsecsz	= 512,
	Tnsec	= 16384,		/* an 8 MiB image */
	Tseed	= 0x5ea1,

	Tblksz	= 4096,
	Tobjmax	= 1<<20,
	Tepoch	= 7,

	Tms	= 5000,			/* an exchange nothing is holding */
	Twaitms	= 10000,		/* the longest this program waits */

	/* fids the cases use; each case takes a fresh set */
	Froot	= 1,
	Fa	= 2,
	Fb	= 3,
	Fc	= 4,
};

static char Tuuid[] = "0000000000000000000000000000000a";
static char Tmonid[] = "00112233445566778899aabbccddeeff";

static char*
mkmap(void)
{
	char *p;

	p = smprint(
		"map=t epoch=%d\n"
		"\tmonid=%s\n"
		"\tobjmax=%d blksz=%d replicas=2\n"
		"\tcsumalg=blake2s256 placehash=blake2s256-64\n"
		"\tpollms=1000 leasems=3000 replms=1000 deadms=10000\n"
		"\toutmins=60 tombdays=7 mincopies=1 retain=8\n"
		"\n"
		"instance=n1.0 onnode=n1 addr=tcp!10.0.0.1!17011\n"
		"\tuuid=%s\n"
		"\tclass=ssd weight=100 status=in up=yes since=1 fenced=no\n"
		"instance=n1.1 onnode=n1 addr=tcp!10.0.0.1!17012\n"
		"\tuuid=0000000000000000000000000000000b\n"
		"\tclass=ssd weight=100 status=in up=yes since=1 fenced=no\n",
		Tepoch, Tmonid, Tobjmax, Tblksz, Tuuid);
	if(p == nil)
		sysfatal("smprint: %r");
	return p;
}

static Dev*
newdisk(void)
{
	Dev *d;
	Super s;
	Fmtcfg c;

	if((d = simopen(Tsecsz, Tnsec, Tseed)) == nil)
		sysfatal("simopen: %r");
	memset(&c, 0, sizeof c);
	c.secsz = Tsecsz;
	c.blksz = Tblksz;
	c.objmax = Tobjmax;
	c.nslots = 128;
	c.nemap = 32;
	c.ndirty = 64;
	c.logbytes = 64*1024;
	c.csumalg = Csumblake2s;
	c.uuid[15] = 0x0a;		/* Tuuid */
	c.uuidset = 1;
	if(geometry(&s, &c, d->size) < 0)
		sysfatal("geometry: %r");
	if(fmtstore(d, &s) < 0)
		sysfatal("fmtstore: %r");
	return d;
}

/*
 * store.md §7's spawn callback, which the client takes for the same
 * reason the engine does: this is a libthread program, so it is
 * proccreate here and an rfork wrapper in a plain-libc one.
 */
static int
tspawn(void (*fn)(void*), void *a)
{
	if(proccreate(fn, a, Srvstack) < 0)
		return -1;
	return 0;
}

/* the client's last act before its memory goes: §13's observation */
static int freedseen;

static void
onfreed(void*)
{
	freedseen++;
}

/*
 * The transport.  Two pipes rather than one, for srv9p.h's reason: the
 * server keeps infd and outfd apart, and the request pipe closing is
 * what ends the service loop while the reply pipe stays open for a
 * request the shutdown is still draining.  The client is handed the
 * other two ends and closes them itself, so the connect callback is
 * the whole of what this program tells it about the network.
 */
typedef struct Pipes Pipes;
struct Pipes
{
	int	cin;		/* the client reads replies here */
	int	cout;		/* ... and writes requests here */
	int	sin;		/* the peer's infd */
	int	sout;		/* ... and outfd */
};

static void
mkpipes(Pipes *p)
{
	int a[2], b[2];

	if(pipe(a) < 0 || pipe(b) < 0)
		sysfatal("pipe: %r");
	p->cout = a[1];
	p->sin = a[0];
	p->sout = b[0];
	p->cin = b[1];
}

static int
pipeconnect(void *v, int *infd, int *outfd)
{
	Pipes *p;

	p = v;
	*infd = p->cin;
	*outfd = p->cout;
	return 0;
}

static Nine*
opencl(Pipes *p, ulong msize)
{
	Ninecfg cfg;
	Nine *c;

	memset(&cfg, 0, sizeof cfg);
	cfg.connect = pipeconnect;
	cfg.connectarg = p;
	cfg.spawn = tspawn;
	cfg.msize = msize;
	cfg.tickms = 5;
	cfg.nreq = 8;
	cfg.openms = Twaitms;
	cfg.freed = onfreed;
	cfg.freedarg = nil;
	freedseen = 0;
	if((c = nineopen(&cfg)) == nil)
		fail("%s: nineopen: %r", stage);
	return c;
}

/*
 * Close, and see the memory go.  A reader parked inside read(2) cannot
 * be recalled, so the release is the last reference's and not the
 * caller's; the callback is the only observation of it there is, and a
 * close that never reaches it is the leak this asserts against.
 */
static void
closecl(Nine *c)
{
	int i;

	nineclose(c);
	for(i = 0; i*25 < Twaitms && freedseen == 0; i++)
		sleep(25);
	checks++;
	if(freedseen != 1)
		fail("%s: the client was freed %d times by its close, want 1",
			stage, freedseen);
}

/* a whole storage instance on the far end of the pipes */
typedef struct Inst Inst;
struct Inst
{
	Dev	*d;
	char	*m;
	Srvctx	*ctx;
	Pipes	p;
	int	ended;
};

static void
srvproc(void *v)
{
	Inst *in;

	in = v;
	srvrun(in->ctx, in->p.sin, in->p.sout);
	in->ended = 1;
	threadexits(nil);
}

static int
instart(Inst *in)
{
	Srvcfg cfg;

	memset(in, 0, sizeof *in);
	in->m = mkmap();
	in->d = newdisk();
	memset(&cfg, 0, sizeof cfg);
	cfg.dev = in->d;
	cfg.maptext = in->m;
	cfg.maplen = strlen(in->m);
	cfg.nqueue = 4;
	cfg.store.spawn = tspawn;
	cfg.store.nockptproc = 1;
	cfg.store.ckwaitms = 200;
	cfg.store.emapcache = 16;
	cfg.store.stagemax = 8;
	cfg.store.stagetot = 12;
	cfg.store.stagems = 50;
	if((in->ctx = srvnew(&cfg)) == nil){
		fail("%s: srvnew: %r", stage);
		devclose(in->d);
		free(in->m);
		return -1;
	}
	mkpipes(&in->p);
	proccreate(srvproc, in, Srvstack);
	return 0;
}

/*
 * The client's close hung the request pipe up, which is D16's shutdown
 * trigger — but the fds go with the client's LAST reference, so this
 * runs after closecl and not beside it.
 */
static void
instop(Inst *in)
{
	int i;

	for(i = 0; i*25 < Twaitms && !in->ended; i++)
		sleep(25);
	checks++;
	if(!in->ended)
		fail("%s: the service loop did not end", stage);
	close(in->p.sin);
	close(in->p.sout);
	srvfree(in->ctx);
	devclose(in->d);
	free(in->m);
}

/* how many requests the queue pool is carrying */
static int
pooldepth(Srvctx *ctx)
{
	uvlong np, nd;

	np = nd = 0;
	srvcount(ctx, &np, &nd);
	return np - nd;
}

/* wait for a request to reach a queue, rather than sleeping a guess */
static void
waitpool(Srvctx *ctx, int n)
{
	int i;

	for(i = 0; i*10 < Twaitms && pooldepth(ctx) < n; i++)
		sleep(10);
	checks++;
	if(pooldepth(ctx) < n)
		fail("%s: %d requests reached the pool, want %d",
			stage, pooldepth(ctx), n);
}

static int
attachadmin(Nine *c, ulong fid, Ninerep *r)
{
	if(nineattach(c, fid, NOFID, "glenda", "role=admin", Tms, r) != Nineok)
		fail("%s: attach role=admin: %s", stage, r->err);
	return r->out;
}

/* walk a fresh fid to one name under the root */
static int
walk1(Nine *c, ulong root, ulong fid, char *name, Ninerep *r)
{
	char *w[1];

	w[0] = name;
	if(ninewalk(c, root, fid, w, 1, Tms, r) != Nineok)
		return r->out;
	if(r->nwqid != 1){
		fail("%s: a short walk to %s", stage, name);
		return -1;
	}
	return Nineok;
}

/* ... and open it */
static int
openpath(Nine *c, ulong root, ulong fid, char *name, int mode, Ninerep *r)
{
	if(walk1(c, root, fid, name, r) != Nineok)
		return r->out;
	return nineopenfid(c, fid, mode, Tms, r);
}

/* the whole of a small file, through the client */
static long
slurp(Nine *c, ulong fid, char *buf, long max)
{
	Ninerep r;
	long off, n;

	off = 0;
	for(;;){
		n = max-1 - off;
		if(n > 2048)
			n = 2048;
		if(n <= 0)
			return -1;
		if(nineread(c, fid, off, buf+off, n, Tms, &r) != Nineok)
			return -1;
		if(r.count == 0)
			break;
		off += r.count;
	}
	buf[off] = 0;
	return off;
}

/* one attr=value line's value out of a rendered status text */
static char*
field(char *text, char *attr, char *buf, int nbuf)
{
	char *p, *e;
	int n;

	n = strlen(attr);
	for(p = text; p != nil && *p != 0; p = (e == nil ? nil : e+1)){
		e = strchr(p, '\n');
		if(strncmp(p, attr, n) == 0 && p[n] == '='){
			p += n+1;
			if(e == nil)
				e = p + strlen(p);
			if(e - p >= nbuf)
				return nil;
			memmove(buf, p, e-p);
			buf[e-p] = 0;
			return buf;
		}
	}
	return nil;
}

/*
 * §5.5's msize, both halves.  What this client owes is the negotiated
 * number — a caller sizes a forwarded write off it — and the floor is
 * the INSTANCE's to refuse at (§14(48)), which is what the low
 * connection sees as an ordinary §2.6-marked Rerror from the attach.
 */
static void
cmsize(void)
{
	char buf[8192], val[64], want[128];
	Ninerep r;
	Inst in;
	Nine *c;
	long n;

	stage = "msize";
	if(instart(&in) < 0)
		return;
	if((c = opencl(&in.p, 4096)) == nil){
		instop(&in);
		return;
	}
	eqv("a low msize is negotiated as proposed", ninemsize(c), 4096);
	snprint(want, sizeof want, "shoalsrv: msize 4096 below the %d-byte"
		" floor", Msizemin);
	nineattach(c, Froot, NOFID, "glenda", "role=admin", Tms, &r);
	eqv("an attach under the floor is refused", r.out, Nineerr);
	eqs("... with the peer's own string, verbatim", r.err, want);
	istrue("... which carries no §2.6 prefix", srv26(r.err) == nil);
	closecl(c);
	instop(&in);

	/* a Srvctx serves one loop, so the second connection is a second one */
	stage = "msize floor";
	if(instart(&in) < 0)
		return;
	if((c = opencl(&in.p, Ninemsizefloor)) == nil){
		instop(&in);
		return;
	}
	eqv("the floor msize is negotiated as proposed", ninemsize(c),
		Ninemsizefloor);
	if(attachadmin(c, Froot, &r) == Nineok
	&& openpath(c, Froot, Fa, "status", OREAD, &r) == Nineok){
		n = slurp(c, Fa, buf, sizeof buf);
		istrue("/status reads through the client", n > 0);
		snprint(want, sizeof want, "%d", Ninemsizefloor);
		eqs("what the peer reports is what ninemsize answers",
			field(buf, "msize", val, sizeof val), want);
		eqv("clunk", nineclunk(c, Fa, Tms, &r), Nineok);
	}else
		fail("%s: open /status: %s", stage, r.err);
	eqv("no tag is left held", nineheld(c), 0);
	closecl(c);
	instop(&in);

	/*
	 * And the same at a proposal the peer is free to lower: what
	 * ninemsize answers is the NEGOTIATED size, whatever was asked
	 * for, which is the whole of this client's half of §5.5.
	 */
	stage = "msize negotiated";
	if(instart(&in) < 0)
		return;
	if((c = opencl(&in.p, Ninemsizedflt)) == nil){
		instop(&in);
		return;
	}
	istrue("a high proposal is not raised", ninemsize(c) <= Ninemsizedflt);
	istrue("... and is at layer-a §5.5's floor",
		ninemsize(c) >= Ninemsizefloor);
	if(attachadmin(c, Froot, &r) == Nineok
	&& openpath(c, Froot, Fa, "status", OREAD, &r) == Nineok){
		n = slurp(c, Fa, buf, sizeof buf);
		istrue("/status reads", n > 0);
		snprint(want, sizeof want, "%lud", ninemsize(c));
		eqs("the negotiated size is the peer's own",
			field(buf, "msize", val, sizeof val), want);
		nineclunk(c, Fa, Tms, &r);
	}else
		fail("%s: open /status: %s", stage, r.err);
	closecl(c);
	instop(&in);
}

/*
 * The ordinary exchanges: walk, open, read, stat, clunk, a write whose
 * answer is an exact §2.6 string, a create and a remove the role
 * matrix refuses, and a Tflush.
 */
static void
cexchanges(void)
{
	char buf[8192], sbuf[512];
	uchar st[512];
	Ninerep r;
	Inst in;
	Nine *c;
	Dir d;
	long n;

	stage = "exchanges";
	if(instart(&in) < 0)
		return;
	if((c = opencl(&in.p, Ninemsizefloor)) == nil){
		instop(&in);
		return;
	}
	if(attachadmin(c, Froot, &r) != Nineok){
		closecl(c);
		instop(&in);
		return;
	}
	eqv("the attached root is a directory", r.qid.type, QTDIR);

	if(openpath(c, Froot, Fa, "status", OREAD, &r) == Nineok){
		n = slurp(c, Fa, buf, sizeof buf);
		istrue("/status reads", n > 0);
		istrue("... and is attr=value lines", strstr(buf, "msize=") != nil);
		eqv("stat", ninestat(c, Fa, st, sizeof st, Tms, &r), Nineok);
		istrue("the stat message decodes",
			convM2D(st, r.count, &d, sbuf) > BIT16SZ);
		eqs("... and names the file", d.name, "status");
		eqv("clunk", nineclunk(c, Fa, Tms, &r), Nineok);
	}else
		fail("%s: open /status: %s", stage, r.err);

	/* §2.5's unknown verb, which is the exact §2.6 string a caller matches */
	if(openpath(c, Froot, Fb, "ctl", OWRITE, &r) == Nineok){
		ninewrite(c, Fb, 0, "frobnicate", 10, Tms, &r);
		eqv("an unknown ctl verb is refused", r.out, Nineerr);
		eqs("... with §2.6's string, verbatim", r.err, "unknown ctl");
		eqv("clunk", nineclunk(c, Fb, Tms, &r), Nineok);
	}else
		fail("%s: open /ctl: %s", stage, r.err);

	/* §2.1's role matrix: an admin create of an id that is not reserved */
	if(walk1(c, Froot, Fc, "obj", &r) == Nineok){
		ninecreate(c, Fc, "beta", 0666, OWRITE, Tms, &r);
		eqv("an admin create under /obj is refused", r.out, Nineerr);
		eqs("... with §2.6's string", r.err, "permission denied");
		nineremove(c, Fc, Tms, &r);
		eqv("and a remove of the directory itself is refused", r.out,
			Nineerr);
		eqs("... by the same cell of §2.1's matrix", r.err,
			"permission denied");
	}else
		fail("%s: walk /obj: %s", stage, r.err);

	/*
	 * §5.4.1's Tflush, named by the caller.  9P has the Rflush
	 * answered whether or not anything was outstanding under the tag,
	 * so this is an exchange like any other; the Tflush a TIMEOUT
	 * sends is the client's own and is ctimeout's subject.
	 */
	nineflush(c, 31337, Tms, &r);
	eqv("a Tflush naming a tag nobody sent is answered", r.out, Nineok);

	eqv("no tag is left held", nineheld(c), 0);
	eqv("and nothing arrived late", ninelate(c), 0);
	closecl(c);
	instop(&in);
}

/*
 * layer-a §5.6's one-outstanding-per-fid rule, enforced here: the
 * second request is refused before anything is written, so the peer
 * never sees it.  The first is held inside the server at the `mapopen'
 * point, which is what makes the window a case can write into.
 */
static Nine *busyc;
static int busyout;
static int busydone;

static void
busyproc(void*)
{
	Ninerep r;

	busyout = nineopenfid(busyc, Fa, OREAD, Twaitms, &r);
	busydone = 1;
	threadexits(nil);
}

static void
cfidbusy(void)
{
	Ninerep r;
	Inst in;
	Nine *c;
	int i;

	stage = "one per fid";
	if(instart(&in) < 0)
		return;
	if((c = opencl(&in.p, Ninemsizefloor)) == nil){
		instop(&in);
		return;
	}
	if(attachadmin(c, Froot, &r) != Nineok
	|| walk1(c, Froot, Fa, "map", &r) != Nineok){
		fail("%s: walk /map: %s", stage, r.err);
		closecl(c);
		instop(&in);
		return;
	}
	srvhook(in.ctx, "mapopen", 1);
	busyc = c;
	busydone = 0;
	busyout = -1;
	proccreate(busyproc, nil, Srvstack);
	waitpool(in.ctx, 1);

	nineclunk(c, Fa, Tms, &r);
	eqv("a second request on a busy fid is refused", r.out, Ninebusy);
	eqs("... locally, naming the fid", r.err,
		"ninep: fid 2 already has a request outstanding");
	eqv("and nothing of it reached the peer", pooldepth(in.ctx), 1);

	/* the rule is per fid: another fid is served while that one waits */
	if(openpath(c, Froot, Fb, "status", OREAD, &r) == Nineok)
		eqv("clunk", nineclunk(c, Fb, Tms, &r), Nineok);
	else
		fail("%s: another fid while one is busy: %s", stage, r.err);

	srvhook(in.ctx, "mapopen", 0);
	for(i = 0; i*25 < Twaitms && !busydone; i++)
		sleep(25);
	eqv("the held open is answered once the point is cleared", busyout,
		Nineok);
	eqv("and the fid is free again", nineclunk(c, Fa, Tms, &r), Nineok);
	eqv("no tag is left held", nineheld(c), 0);
	closecl(c);
	instop(&in);
}

/*
 * The bounded wait, against a peer that is holding the request: the
 * call comes back within its deadline and one timer tick, the client
 * flushes the tag itself, and the tag, the fid and the connection are
 * all usable again once the Rflush lands (§14(49)).
 */
static void
ctimeout(void)
{
	Ninerep r;
	Inst in;
	Nine *c;
	vlong t0, ms;
	int i;

	stage = "timeout";
	if(instart(&in) < 0)
		return;
	if((c = opencl(&in.p, Ninemsizefloor)) == nil){
		instop(&in);
		return;
	}
	if(attachadmin(c, Froot, &r) != Nineok
	|| walk1(c, Froot, Fa, "map", &r) != Nineok){
		fail("%s: walk /map: %s", stage, r.err);
		closecl(c);
		instop(&in);
		return;
	}
	srvhook(in.ctx, "mapopen", 1);
	t0 = nsec();
	nineopenfid(c, Fa, OREAD, 100, &r);
	ms = (nsec() - t0)/1000000;
	eqv("an open the peer is holding times out", r.out, Ninetimeout);
	eqs("... locally", r.err, "ninep: no reply before the deadline");
	istrue("... not before its deadline", ms >= 95);
	istrue("... and not long after it", ms < 2000);
	eqv("the tag and the tag of its Tflush are held", nineheld(c), 2);

	/*
	 * The Tflush is what releases the held request — a queue's flush
	 * flag lets a held one go, the point still being set — so both
	 * tags come back without this case clearing anything.
	 */
	for(i = 0; i*10 < Twaitms && nineheld(c) != 0; i++)
		sleep(10);
	eqv("the Rflush gives both tags back", nineheld(c), 0);
	eqv("and the flushed request's own reply was discarded",
		ninelate(c), 1);

	/* the connection is usable, and so is the fid the timeout was on */
	srvhook(in.ctx, "mapopen", 0);
	eqv("the same fid serves again", nineopenfid(c, Fa, OREAD, Tms, &r),
		Nineok);
	eqv("clunk", nineclunk(c, Fa, Tms, &r), Nineok);
	if(openpath(c, Froot, Fb, "status", OREAD, &r) == Nineok)
		eqv("clunk", nineclunk(c, Fb, Tms, &r), Nineok);
	else
		fail("%s: the connection after a timeout: %s", stage, r.err);
	closecl(c);
	instop(&in);
}

/*
 * A close with work in flight.  The waiter is answered Ninedead where
 * it stands; the reader is inside read(2) and cannot be recalled, so
 * the release waits for the peer's reply and the callback is what says
 * it happened.
 */
static void
cclosebusy(void)
{
	Ninerep r;
	Inst in;
	Nine *c;
	int i;

	stage = "close with work in flight";
	if(instart(&in) < 0)
		return;
	if((c = opencl(&in.p, Ninemsizefloor)) == nil){
		instop(&in);
		return;
	}
	if(attachadmin(c, Froot, &r) != Nineok
	|| walk1(c, Froot, Fa, "map", &r) != Nineok){
		fail("%s: walk /map: %s", stage, r.err);
		closecl(c);
		instop(&in);
		return;
	}
	srvhook(in.ctx, "mapopen", 1);
	busyc = c;
	busydone = 0;
	busyout = -1;
	proccreate(busyproc, nil, Srvstack);
	waitpool(in.ctx, 1);

	nineclose(c);
	for(i = 0; i*25 < Twaitms && !busydone; i++)
		sleep(25);
	eqv("a waiter is released by the close", busyout, Ninedead);

	/* the held reply is what lets the reader out of its read */
	srvhook(in.ctx, "mapopen", 0);
	for(i = 0; i*25 < Twaitms && freedseen == 0; i++)
		sleep(25);
	eqv("and everything is reclaimed once it arrives", freedseen, 1);
	instop(&in);
}

/*
 * A peer that is not a server.  These two cases are things no
 * conforming instance does, so there is no instance behind them: the
 * stub answers the Tversion and then hangs up, or answers on a tag
 * nobody sent.
 */
enum
{
	Sthold	= 0,		/* read the request and never answer it */
	Stbadtag,		/* answer on a tag nobody sent */
};

typedef struct Stub Stub;
struct Stub
{
	Pipes	p;
	int	mode;
	ulong	offer;		/* the msize it answers, if it is lower */
	int	got;		/* requests read past the Tversion */
	int	ended;
};

static void
stubproc(void *v)
{
	uchar buf[8192];
	Fcall t, f;
	Stub *s;
	int n;

	s = v;
	for(;;){
		if((n = read9pmsg(s->p.sin, buf, sizeof buf)) <= 0)
			break;
		if(convM2S(buf, n, &t) != n)
			break;
		memset(&f, 0, sizeof f);
		if(t.type == Tversion){
			f.type = Rversion;
			f.tag = t.tag;
			f.msize = t.msize;
			if(s->offer != 0 && s->offer < f.msize)
				f.msize = s->offer;
			f.version = "9P2000";
			if((n = convS2M(&f, buf, sizeof buf)) > 0)
				write(s->p.sout, buf, n);
			continue;
		}
		if(s->mode == Stbadtag){
			f.type = Rattach;
			f.tag = t.tag + 100;
			f.qid.type = QTDIR;
			if((n = convS2M(&f, buf, sizeof buf)) > 0)
				write(s->p.sout, buf, n);
		}
		s->got++;
	}
	s->ended = 1;
	threadexits(nil);
}

static void
stubstart(Stub *s, int mode, ulong offer)
{
	memset(s, 0, sizeof *s);
	s->mode = mode;
	s->offer = offer;
	mkpipes(&s->p);
	proccreate(stubproc, s, Srvstack);
}

/*
 * The stub is parked in a read of the request pipe; the client's own
 * close is what shuts the write end of it and lets the stub out.
 */
static void
stubstop(Stub *s)
{
	int i;

	for(i = 0; i*25 < Twaitms && !s->ended; i++)
		sleep(25);
	checks++;
	if(!s->ended)
		fail("%s: the stub peer did not end", stage);
	if(s->p.sin >= 0)
		close(s->p.sin);
	if(s->p.sout >= 0)
		close(s->p.sout);
}

static Nine *deadc;
static int deadout;
static int deaddone;

static void
deadproc(void*)
{
	Ninerep r;

	deadout = nineattach(deadc, Froot, NOFID, "glenda", "role=admin",
		Twaitms, &r);
	deaddone = 1;
	threadexits(nil);
}

static void
cdeath(void)
{
	Ninerep r;
	Stub s;
	Nine *c;
	int i;

	stage = "the peer hangs up";
	stubstart(&s, Sthold, Ninemsizefloor);
	if((c = opencl(&s.p, Ninemsizedflt)) == nil){
		close(s.p.cin);
		close(s.p.cout);
		stubstop(&s);
		return;
	}
	/*
	 * §5.5's other half: the peer answered below what was proposed,
	 * and what ninemsize reports is the peer's number and not ours.
	 */
	eqv("a lowered msize is what ninemsize reports", ninemsize(c),
		Ninemsizefloor);
	deadc = c;
	deaddone = 0;
	deadout = -1;
	proccreate(deadproc, nil, Srvstack);
	for(i = 0; i*10 < Twaitms && s.got == 0; i++)
		sleep(10);
	istrue("the stub has the request", s.got > 0);

	close(s.p.sout);		/* the peer hangs up */
	s.p.sout = -1;
	for(i = 0; i*25 < Twaitms && !deaddone; i++)
		sleep(25);
	eqv("a waiter is released when the peer hangs up", deadout, Ninedead);

	nineclunk(c, Froot, Tms, &r);
	eqv("and a call afterwards says so too", r.out, Ninedead);
	eqs("... naming the hangup", r.err, "ninep: the peer hung up");
	closecl(c);
	stubstop(&s);
}

static void
cbotch(void)
{
	Ninerep r;
	Stub s;
	Nine *c;

	stage = "the peer breaks 9P";
	stubstart(&s, Stbadtag, 0);
	if((c = opencl(&s.p, Ninemsizefloor)) == nil){
		close(s.p.cin);
		close(s.p.cout);
		stubstop(&s);
		return;
	}
	nineattach(c, Froot, NOFID, "glenda", "role=admin", Twaitms, &r);
	eqv("a reply on a tag nobody sent is a protocol violation", r.out,
		Ninebotch);
	eqs("... and says which", r.err, "ninep: a reply nothing is waiting"
		" for");
	nineclunk(c, Froot, Tms, &r);
	eqv("the connection does not survive it", r.out, Ninebotch);
	closecl(c);
	stubstop(&s);
}

void
threadmain(int argc, char **argv)
{
	USED(argc);
	USED(argv);
	quotefmtinstall();		/* the FAIL lines quote what they got */
	watchon();

	cmsize();
	cexchanges();
	cfidbusy();
	ctimeout();
	cclosebusy();
	cdeath();
	cbotch();

	watchoff();
	if(fails > 0){
		fprint(2, "clienttest: %d of %d checks failed\n", fails, checks);
		threadexitsall("fail");
	}
	print("clienttest: %d checks ok\n", checks);
	threadexitsall(nil);
}
