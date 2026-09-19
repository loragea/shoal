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
 * answers, hold points included.  The cases about a BROKEN peer run
 * against a stub of a few lines instead: a hang-up, a reply on a tag
 * nobody sent, a reply longer than the negotiated msize, a peer that
 * never speaks and a peer that stops reading are things a conforming
 * server never does, and a stub is the only way to make it do them.
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

/* and how many times the client asked for the transport to be broken */
static int hangups;

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

/*
 * Open a connection over those pipes.  `hangup' is §12's third
 * callback and is what lets a close recall a proc this program parked
 * in read(2) or write(2); most cases pass none, because the deferred
 * behaviour a nil one leaves is itself part of the contract.  This
 * one does not fail on nil: a case whose subject is a nineopen that
 * must not succeed calls it directly.
 */
static Nine*
openfull(Pipes *p, ulong msize, int openms, void (*hangup)(void*), void *harg)
{
	Ninecfg cfg;

	memset(&cfg, 0, sizeof cfg);
	cfg.connect = pipeconnect;
	cfg.connectarg = p;
	cfg.spawn = tspawn;
	cfg.hangup = hangup;
	cfg.hanguparg = harg;
	cfg.msize = msize;
	cfg.tickms = 5;
	cfg.nreq = 8;
	cfg.openms = openms;
	cfg.freed = onfreed;
	cfg.freedarg = nil;
	freedseen = 0;
	hangups = 0;
	return nineopen(&cfg);
}

static Nine*
opencl(Pipes *p, ulong msize)
{
	Nine *c;

	if((c = openfull(p, msize, Twaitms, nil, nil)) == nil)
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
 * the INSTANCE's to refuse at (§14(57)), which is what the low
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
	char buf[8192], sbuf[512], *sw[2];
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

	/*
	 * A walk that fails partway: 9P answers an Rwalk with fewer qids
	 * than names and leaves newfid uncreated, so there is nothing to
	 * clunk and no full walk to take a qid from.  It is neither
	 * Nineok nor §2.6's Nineerr — nothing was refused in those words
	 * — and the qids that did come back are the caller's to read.
	 */
	sw[0] = "obj";
	sw[1] = "nosuchthing";
	memset(&r, 0, sizeof r);
	ninewalk(c, Froot, Fc, sw, 2, Tms, &r);
	eqv("a walk that got only some of its names is not ok", r.out,
		Ninelocal);
	eqs("... and says how far it got", r.err,
		"ninep: a walk of 2 names got 1");
	eqv("... with the partial result left for the caller", r.nwqid, 1);
	eqv("... and no qid, which is a full walk's alone", r.qid.type, 0);
	eqv("no tag is held by it", nineheld(c), 0);
	/* nothing was created under the newfid, so the fid is still free */
	if(walk1(c, Froot, Fc, "status", &r) == Nineok)
		eqv("clunk", nineclunk(c, Fc, Tms, &r), Nineok);
	else
		fail("%s: the newfid after a short walk: %s", stage, r.err);

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
	char *w[1];
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

	/*
	 * A walk holds BOTH of its fids, so a walk ONTO the busy fid is
	 * refused too: the newfid a walk targets is as much in the
	 * exchange as the fid it walks from (§14(60)).
	 */
	w[0] = "status";
	ninewalk(c, Froot, Fa, w, 1, Tms, &r);
	eqv("a walk onto a busy newfid is refused", r.out, Ninebusy);
	eqs("... naming that fid and not the one walked from", r.err,
		"ninep: fid 2 already has a request outstanding");
	eqv("and nothing of it reached the peer either", pooldepth(in.ctx), 1);

	/* the rule is per fid: another fid is served while that one waits */
	if(openpath(c, Froot, Fb, "status", OREAD, &r) == Nineok)
		eqv("clunk", nineclunk(c, Fb, Tms, &r), Nineok);
	else
		fail("%s: another fid while one is busy: %s", stage, r.err);
	if(walk1(c, Froot, Fc, "status", &r) == Nineok)
		eqv("... and so is a walk onto one", nineclunk(c, Fc, Tms, &r),
			Nineok);
	else
		fail("%s: a walk onto a free newfid: %s", stage, r.err);

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
 * all usable again once the Rflush lands (§14(58)).
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
	Stmute,			/* never answer anything, the Tversion included */
	Stdeaf,			/* answer the Tversion, then stop reading */
	Stover,			/* answer an Rread/Rwrite over the count asked for */
	Stlong,			/* answer an Rread over the negotiated msize */
	Stflushdead,		/* hold a request, botch another, hold the Tflush */

	Stubbuf	= 16384,	/* the stub's own message buffer */
	Sover	= 64,		/* Stover: what the case asks a read for */
};

typedef struct Stub Stub;
struct Stub
{
	Pipes	p;
	int	mode;
	ulong	offer;		/* the msize it answers, if it is lower */
	ulong	over;		/* Stlong: the count it answers a Tread with */
	int	deafafter;	/* Stdeaf: requests it reads before going deaf */
	int	deaf;		/* Stdeaf: the Tversion is answered, no more reads */
	int	drain;		/* ... until a hangup, which resumes them */
	ushort	ftag;		/* Stflushdead: the tag its Tflush arrived under */
	int	hasflush;	/* ... and that it has arrived at all */
	int	got;		/* requests read past the Tversion */
	int	ended;
};

/*
 * The transport owner's half of §12's `hangup' contract, for a pipe
 * pair.  The end the client READS is closed, which is what ends a
 * read it has parked in.  The end it WRITES is drained and not
 * closed: `pipewrite' posts `sys: write on closed pipe' to a proc
 * blocked writing a pipe whose reader has gone
 * (/sys/src/9/port/devpipe.c:310), and that note kills the proc it
 * lands on — on the write path the client's own caller — rather than
 * failing its write.  It kills it in both of this library's homes: a
 * plain-libc program with no handler dies by the kernel's default
 * action for the note, and a libthread program's handler answers a
 * `sys:' note with noted(NDFLT), which is that same action.  So
 * closing that end would take the caller down instead of breaking
 * its write.  It may be called more than once, so the end goes at
 * most once and stubstop finds nothing left.
 */
static void
stubhangup(void *v)
{
	Stub *s;
	int fd;

	s = v;
	hangups++;
	if((fd = s->p.sout) >= 0){
		s->p.sout = -1;
		close(fd);
	}
	s->drain = 1;
}

static void
stubproc(void *v)
{
	uchar *buf, *data;
	Fcall t, f;
	Stub *s;
	int n;

	s = v;
	if((buf = mallocz(Stubbuf, 1)) == nil || (data = mallocz(Stubbuf, 1)) == nil)
		sysfatal("malloc: %r");
	for(;;){
		/*
		 * layer-a §5.4's own scenario: a peer that took the
		 * connection and then stopped reading, so the client's next
		 * write fills the pipe and parks in write(2).
		 */
		if(s->deaf){
			if(s->p.sin < 0)
				break;
			if(s->drain){
				/* the hangup: read and discard, to the end */
				if(read(s->p.sin, buf, Stubbuf) <= 0)
					break;
				continue;
			}
			sleep(25);
			continue;
		}
		if((n = read9pmsg(s->p.sin, buf, Stubbuf)) <= 0)
			break;
		if(convM2S(buf, n, &t) != n)
			break;
		memset(&f, 0, sizeof f);
		if(t.type == Tversion){
			if(s->mode == Stmute){
				s->got++;
				continue;
			}
			f.type = Rversion;
			f.tag = t.tag;
			f.msize = t.msize;
			if(s->offer != 0 && s->offer < f.msize)
				f.msize = s->offer;
			f.version = "9P2000";
			if((n = convS2M(&f, buf, Stubbuf)) > 0)
				write(s->p.sout, buf, n);
			if(s->mode == Stdeaf && s->deafafter == 0)
				s->deaf = 1;
			continue;
		}
		switch(s->mode){
		case Stbadtag:
			f.type = Rattach;
			f.tag = t.tag + 100;
			f.qid.type = QTDIR;
			if((n = convS2M(&f, buf, Stubbuf)) > 0)
				write(s->p.sout, buf, n);
			break;
		case Stover:
			/* one byte more than the request asked for */
			if(t.type != Tread && t.type != Twrite)
				break;
			f.type = t.type+1;
			f.tag = t.tag;
			f.count = t.count + 1;
			f.data = (char*)data;
			if((n = convS2M(&f, buf, Stubbuf)) > 0)
				write(s->p.sout, buf, n);
			break;
		case Stlong:
			/* ... and a whole message over the negotiated msize */
			if(t.type != Tread)
				break;
			f.type = Rread;
			f.tag = t.tag;
			f.count = s->over;
			f.data = (char*)data;
			if((n = convS2M(&f, buf, Stubbuf)) > 0)
				write(s->p.sout, buf, n);
			break;
		case Stflushdead:
			/*
			 * Everything is held and never answered, so the first
			 * request times out and the client leaves a Tflush
			 * behind; the Tflush's own tag is recorded and it too
			 * is left unanswered, for cflushdead to answer by hand
			 * once the connection is dead.  A Tclunk is the
			 * exception: it is answered with a reply of the wrong
			 * type, which is a violation the WAITER judges, so the
			 * reader is still inside read(2) when the connection
			 * dies of it.
			 */
			if(t.type == Tflush){
				s->ftag = t.tag;
				s->hasflush = 1;
				break;
			}
			if(t.type != Tclunk)
				break;
			f.type = Rattach;
			f.tag = t.tag;
			f.qid.type = QTDIR;
			if((n = convS2M(&f, buf, Stubbuf)) > 0)
				write(s->p.sout, buf, n);
			break;
		}
		s->got++;
		/*
		 * A peer that goes deaf after reading a request, not at the
		 * Tversion: ctwowriters needs one write of its own through
		 * before the pipe stops being emptied.
		 */
		if(s->mode == Stdeaf && s->got >= s->deafafter)
			s->deaf = 1;
	}
	free(buf);
	free(data);
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
 * close is what shuts the write end of it and lets the stub out — or,
 * where the case gave the client a hangup callback, that callback
 * already took the stub's ends.
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

/*
 * An Rflush that arrives on a connection which died while the flush
 * was outstanding.  A request is held until it times out, so the
 * client leaves a Tflush behind and the reader goes back into read(2)
 * with two tags owed; a Tclunk is then answered with a reply of the
 * wrong type, which the WAITER judges rather than the reader, so the
 * connection dies with the reader still parked in that read.  The
 * Rflush is written onto the wire by hand after that death, which is
 * the only way to order the two.
 *
 * A dead connection takes nothing more off the wire: the reader
 * leaves where it finds the connection dead instead of running the
 * reply through the demultiplexer, whose Nflushing arm would settle
 * both slots and decrement a nexpect the death has already zeroed.
 * The tags staying held is what makes that observable — nothing else
 * of it is — and the close that follows still reclaiming everything
 * is what says the reader did leave.
 */
enum
{
	Cdeadms		= 100,		/* the held request's deadline */
	Cdeadwaitms	= 500,		/* ... and how long the Rflush is given */
};

static void
cflushdead(void)
{
	uchar wbuf[64];
	Ninerep r;
	Fcall f;
	Stub s;
	Nine *c;
	int i, n;

	stage = "an Rflush onto a dead connection";
	stubstart(&s, Stflushdead, 0);
	if((c = opencl(&s.p, Ninemsizefloor)) == nil){
		close(s.p.cin);
		close(s.p.cout);
		stubstop(&s);
		return;
	}
	nineopenfid(c, Fa, OREAD, Cdeadms, &r);
	eqv("an open the peer is holding times out", r.out, Ninetimeout);
	for(i = 0; i*10 < Twaitms && !s.hasflush; i++)
		sleep(10);
	istrue("... leaving a Tflush the peer has not answered", s.hasflush);
	eqv("the tag and the tag of its Tflush are held", nineheld(c), 2);

	nineclunk(c, Fb, Tms, &r);
	eqv("a reply of the wrong type kills the connection", r.out, Ninebotch);
	eqs("... and says which", r.err, "ninep: a reply of type 105 to a T120");
	eqv("the flush's two tags are held across the death", nineheld(c), 2);

	memset(&f, 0, sizeof f);
	f.type = Rflush;
	f.tag = s.ftag;
	if((n = convS2M(&f, wbuf, sizeof wbuf)) <= 0)
		fail("%s: an Rflush will not encode", stage);
	else if(write(s.p.sout, wbuf, n) != n)
		fail("%s: writing the Rflush: %r", stage);
	for(i = 0; i*10 < Cdeadwaitms && nineheld(c) == 2; i++)
		sleep(10);
	eqv("a dead connection takes the Rflush off nothing", nineheld(c), 2);
	closecl(c);
	stubstop(&s);
}

/*
 * A peer that accepts the connection and then says nothing at all —
 * the monitor poll loop's hung-peer case.  The Tversion times out, so
 * nineopen answers nil; what this is about is what is left behind.
 * The reader is inside read(2) and nothing in plain libc recalls it,
 * so `hangup' is the whole of the answer: without it the two procs,
 * both fds and two buffers of the proposed msize are lost for every
 * dial attempt.  There is no `freed' to watch here — a nineopen that
 * answers nil never calls it (§14(59)), since the caller never held
 * the handle — so what says the fds went is the peer: its own read
 * ends when the last of the client's references closes them.
 */
static void
cmutepeer(void)
{
	Stub s;
	Nine *c;
	int i;

	stage = "a peer that never speaks";
	stubstart(&s, Stmute, 0);
	c = openfull(&s.p, Ninemsizefloor, 300, stubhangup, &s);
	istrue("nineopen against a mute peer answers nil", c == nil);
	if(c != nil)
		nineclose(c);
	istrue("... having asked for the transport to be broken", hangups > 0);
	for(i = 0; i*25 < Twaitms && !s.ended; i++)
		sleep(25);
	istrue("... and the client's fds go with the reader it recalled",
		s.ended);
	eqv("no free callback for a handle the caller never held", freedseen, 0);
	stubstop(&s);
}

/*
 * The other half of the same fault, and this one needs no callback at
 * all: a request whose slot is armed and which is then refused HERE,
 * before anything reaches the wire.  The reader must not have left its
 * park for it, because nothing will ever arrive for that tag.  The
 * refusal is a Twalk the negotiated msize will not hold, which is why
 * the connection is opened at a msize a walk can overrun.
 */
enum
{
	Csmall	= 600,		/* a msize MAXWELEM long names overrun */
	Cwname	= 200,		/* ... each of them this long */
};

static char cwbuf[MAXWELEM][Cwname+1];

static void
clocalrefuse(void)
{
	char *w[MAXWELEM];
	Ninerep r;
	Stub s;
	Nine *c;
	int i;

	stage = "a request refused after its slot was armed";
	stubstart(&s, Sthold, 0);
	if((c = openfull(&s.p, Csmall, Twaitms, nil, nil)) == nil){
		fail("%s: nineopen: %r", stage);
		close(s.p.cin);
		close(s.p.cout);
		stubstop(&s);
		return;
	}
	for(i = 0; i < MAXWELEM; i++){
		memset(cwbuf[i], 'x', Cwname);
		cwbuf[i][Cwname] = 0;
		w[i] = cwbuf[i];
	}
	ninewalk(c, Froot, Fa, w, MAXWELEM, Tms, &r);
	eqv("a walk the negotiated msize will not hold is refused here",
		r.out, Ninelocal);
	eqv("and nothing of it reached the peer", s.got, 0);
	eqv("no tag is left held", nineheld(c), 0);
	/*
	 * Nothing is on the wire and nothing ever was, so this is an idle
	 * connection and its close reclaims everything at once — with no
	 * hangup callback, which is the point.
	 */
	closecl(c);
	stubstop(&s);
}

/*
 * A peer that took the connection and then stopped reading — layer-a
 * §5.4's dead peer, arriving on the sending side.  The pipe fills and
 * write(2) parks with the write lock held, so the deadline bounds
 * nothing at all unless the timer settles the write itself: the
 * connection dies Ninedead within the deadline and a tick, every call
 * after it says so, and the close reclaims everything (§14(58)).
 *
 * The message is a megabyte because that is well past any pipe's
 * buffer, so one write is enough to park; a 64 KiB one would have to
 * be repeated an unknown number of times first.
 */
enum
{
	Cbig	= 1024*1024,	/* a Twrite no pipe buffer will take */
	Cwrms	= 200,		/* ... under this deadline */
};

static void
cwritestall(void)
{
	char *big;
	Ninerep r;
	Stub s;
	Nine *c;
	vlong t0, ms;

	stage = "a peer that stops reading";
	if((big = mallocz(Cbig, 1)) == nil)
		sysfatal("malloc: %r");
	stubstart(&s, Stdeaf, 0);
	if((c = openfull(&s.p, Cbig+IOHDRSZ, Twaitms, stubhangup, &s)) == nil){
		fail("%s: nineopen: %r", stage);
		free(big);
		stubstop(&s);
		return;
	}
	t0 = nsec();
	ninewrite(c, Fa, 0, big, Cbig, Cwrms, &r);
	ms = (nsec() - t0)/1000000;
	eqv("a write the peer will not accept kills the connection", r.out,
		Ninedead);
	eqs("... naming the message it stalled on", r.err, "ninep: a T118 the"
		" peer would not accept before the deadline");
	istrue("... not before the deadline", ms >= Cwrms - 5);
	istrue("... and not long after it", ms < 2000);

	nineclunk(c, Fb, Tms, &r);
	eqv("and a call afterwards says so too", r.out, Ninedead);
	closecl(c);
	free(big);
	stubstop(&s);
}

/*
 * The same stall, with a second writer in front of it.  The mark that
 * bounds a write is one per connection and the writers hand it over
 * with the write lock, so a case has to put two of them through that
 * hand-over: a small Tclunk the pipe buffer takes at once, which
 * leaves the lock with its own mark still to settle, and then the
 * megabyte of cwritestall behind it.  A mark cleared by the writer
 * that had finished would leave the stalled write with nothing to
 * bound it — the timer would see no write outstanding — and the
 * deadline, the Tflush behind it and every other exchange on the
 * connection would be unbounded with it (§14(58)).
 *
 * The window between a write returning and its mark being settled is
 * microseconds wide, so this does not race for it: `writewiden'
 * parks the first writer in exactly that window (ninehook), and the
 * peer is told to read one request past the Tversion so that what
 * this waits on is the peer HAVING the first request rather than a
 * sleep.  Timing is not asserted here — the widening is in the path —
 * only that the stalled write comes back at all, and with the
 * connection's death rather than its own timeout.
 */
enum
{
	Cwidenms	= 500,		/* how wide the hand-over window is */
	/*
	 * The second writer's deadline, which must outlast the window:
	 * it is set when the CALL starts and the call spends the window
	 * waiting for the write lock, so a deadline inside it would be
	 * the slot's ordinary timeout rather than the write's mark.
	 */
	Cwidewrms	= 1000,
	Cwidewaitms	= 5000,		/* ... and how long the stall may take */
};

static Nine *widec;
static char *widebig;
static int wideclunked, widewrote, wideout;
static char wideerr[ERRMAX];

/* the writer that finishes and must not settle the other's mark */
static void
wideclunkproc(void*)
{
	Ninerep r;

	nineclunk(widec, Fb, Twaitms, &r);
	wideclunked = 1;
	threadexits(nil);
}

/* ... and the one that stalls owning it */
static void
widewriteproc(void*)
{
	Ninerep r;

	wideout = ninewrite(widec, Fa, 0, widebig, Cbig, Cwidewrms, &r);
	utfecpy(wideerr, wideerr + sizeof wideerr, r.err);
	widewrote = 1;
	threadexits(nil);
}

static void
ctwowriters(void)
{
	Stub s;
	Nine *c;
	int i;

	stage = "two writers, the clear window widened";
	if((widebig = mallocz(Cbig, 1)) == nil)
		sysfatal("malloc: %r");
	stubstart(&s, Stdeaf, 0);
	s.deafafter = 1;		/* the Tclunk is read; nothing after it */
	if((c = openfull(&s.p, Cbig+IOHDRSZ, Twaitms, stubhangup, &s)) == nil){
		fail("%s: nineopen: %r", stage);
		free(widebig);
		stubstop(&s);
		return;
	}
	widec = c;
	wideclunked = widewrote = 0;
	wideout = -1;
	wideerr[0] = 0;
	ninehook(c, "writewiden", Cwidenms);
	proccreate(wideclunkproc, nil, Srvstack);
	for(i = 0; i*5 < Twaitms && s.got == 0; i++)
		sleep(5);
	istrue("the peer has the first writer's request", s.got > 0);

	/* from here the first writer is inside the window for Cwidenms */
	proccreate(widewriteproc, nil, Srvstack);
	for(i = 0; i*25 < Cwidewaitms && !widewrote; i++)
		sleep(25);
	checks++;
	if(!widewrote)
		fail("%s: the stalled write never came back: it is unbounded",
			stage);
	else{
		eqv("a write stalled behind another writer is still bounded",
			wideout, Ninedead);
		eqs("... by its own mark, naming its own message", wideerr,
			"ninep: a T118 the peer would not accept before the"
			" deadline");
	}
	ninehook(c, "writewiden", 0);
	closecl(c);
	for(i = 0; i*25 < Twaitms && !wideclunked; i++)
		sleep(25);
	istrue("and the writer in front of it is answered too", wideclunked);
	free(widebig);
	stubstop(&s);
}

/*
 * A peer whose Rread or Rwrite claims more bytes than the request
 * asked for.  That is a 9P violation like any other and it kills the
 * connection: a client that answered the NEXT call Nineok would be
 * handing the caller replies from a stream it no longer understands.
 * Two connections, because the first does not survive its own case.
 */
static void
covercount(void)
{
	char buf[256];
	Ninerep r;
	Stub s;
	Nine *c;

	stage = "an Rread over its Tread";
	stubstart(&s, Stover, 0);
	if((c = opencl(&s.p, Ninemsizefloor)) == nil){
		close(s.p.cin);
		close(s.p.cout);
		stubstop(&s);
		return;
	}
	nineread(c, Fa, 0, buf, Sover, Tms, &r);
	eqv("an Rread over the count asked for is a protocol violation",
		r.out, Ninebotch);
	eqs("... and says which", r.err,
		"ninep: an Rread of 65 bytes for a Tread of 64");
	nineclunk(c, Fb, Tms, &r);
	eqv("the connection does not survive it", r.out, Ninebotch);
	closecl(c);
	stubstop(&s);

	stage = "an Rwrite over its Twrite";
	stubstart(&s, Stover, 0);
	if((c = opencl(&s.p, Ninemsizefloor)) == nil){
		close(s.p.cin);
		close(s.p.cout);
		stubstop(&s);
		return;
	}
	ninewrite(c, Fa, 0, buf, Sover, Tms, &r);
	eqv("an Rwrite of more than was written is one too", r.out, Ninebotch);
	eqs("... and says which", r.err,
		"ninep: an Rwrite of 65 bytes for a Twrite of 64");
	nineclunk(c, Fb, Tms, &r);
	eqv("the connection does not survive it either", r.out, Ninebotch);
	closecl(c);
	stubstop(&s);
}

/*
 * A reply longer than the negotiated msize, both ways round: against
 * a peer that took the msize as proposed, and against one that
 * lowered it.  The violation is the same and so is the outcome — the
 * length is judged here, off the header, and not left to read9pmsg,
 * which cannot tell an over-long message from a broken transport.
 * The proposals are small so that the stub's own buffer holds the
 * over-long reply it has to build.
 */
enum
{
	Cpropose	= 4096,		/* what these two connections propose */
	Clower		= 2048,		/* ... and what the second peer answers */
	Clong1		= 4200,		/* an Rread count over the first msize */
	Clong2		= 2200,		/* ... and over the second */
	Crdhdr		= BIT32SZ+BIT8SZ+BIT16SZ+BIT32SZ,
};

static void
clongreply(void)
{
	char buf[256], want[256];
	Ninerep r;
	Stub s;
	Nine *c;

	stage = "a reply over a msize the peer took as proposed";
	stubstart(&s, Stlong, 0);
	s.over = Clong1;
	if((c = opencl(&s.p, Cpropose)) == nil){
		close(s.p.cin);
		close(s.p.cout);
		stubstop(&s);
		return;
	}
	eqv("the msize is the one proposed", ninemsize(c), Cpropose);
	nineread(c, Fa, 0, buf, Sover, Tms, &r);
	eqv("a reply over the negotiated msize is a protocol violation",
		r.out, Ninebotch);
	snprint(want, sizeof want, "ninep: a reply of %d bytes over the"
		" negotiated msize %d", Clong1+Crdhdr, Cpropose);
	eqs("... and says which", r.err, want);
	closecl(c);
	stubstop(&s);

	stage = "a reply over a msize the peer lowered";
	stubstart(&s, Stlong, Clower);
	s.over = Clong2;
	if((c = opencl(&s.p, Cpropose)) == nil){
		close(s.p.cin);
		close(s.p.cout);
		stubstop(&s);
		return;
	}
	eqv("the msize is the one the peer answered", ninemsize(c), Clower);
	nineread(c, Fa, 0, buf, Sover, Tms, &r);
	eqv("the same violation is the same outcome", r.out, Ninebotch);
	snprint(want, sizeof want, "ninep: a reply of %d bytes over the"
		" negotiated msize %d", Clong2+Crdhdr, Clower);
	eqs("... and says which", r.err, want);
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
	cflushdead();
	cmutepeer();
	clocalrefuse();
	cwritestall();
	ctwowriters();
	covercount();
	clongreply();

	watchoff();
	if(fails > 0){
		fprint(2, "clienttest: %d of %d checks failed\n", fails, checks);
		threadexitsall("fail");
	}
	print("clienttest: %d checks ok\n", checks);
	threadexitsall(nil);
}
