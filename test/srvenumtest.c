#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../lib/shoal.h"
#include "../srv/srv.h"

/*
 * T1: the parts of the storage instance's 9P surface that report and
 * walk this instance's own index — layer-a §2.2's status files and
 * the /obj and /meta enumerations, §2.5's ctl verbs that are not
 * object I/O, §7.5's scrub pass and store.md §9's tombstone reclaim.
 * store.md §13's T1.27 is here too: that a scrub read of one object
 * is held inside that object's queue.
 *
 * It is a second program beside srvtest rather than more cases in it.
 * srvtest is the framework's — attach, the tree, the role matrix, the
 * queue pool, Tflush and the shutdown — and is already 3.5k lines;
 * these cases drive content over that framework and share none of its
 * fixtures but the geometry.  `mk test' also stops at the first
 * failing program, so two programs say which half broke.
 *
 * A whole instance runs inside this one, as it does in srvtest: a
 * simulated disk, the store engine over it, srv/libshoalsrv.a over
 * that, and a raw 9P client on the other end of a pipe (srv9p.h).
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
	Fctl	= 2,
	Ffile	= 3,
	Ffile2	= 4,
	Fdir	= 5,
	Fdir2	= 6,
	Fdir3	= 7,
	Froot2	= 8,
};

static char Tuuid[] = "0000000000000000000000000000000a";
static char Tmonid[] = "00112233445566778899aabbccddeeff";

/*
 * The map this instance serves.  tombdays is a parameter because
 * store.md §9's reclaim walk tests a tombstone's mtime against it,
 * and the two marks are what /stale renders: one names this instance
 * as the subject, one as the reporter, and one names neither.
 */
static char *
mkmapd(uvlong epoch, ulong blksz, uvlong objmax, char *csumalg, char *uuid,
	ulong tombdays)
{
	char *p;

	p = smprint(
		"map=t epoch=%llud\n"
		"\tmonid=%s\n"
		"\tobjmax=%llud blksz=%lud replicas=2\n"
		"\tcsumalg=%s placehash=blake2s256-64\n"
		"\tpollms=1000 leasems=3000 replms=1000 deadms=10000\n"
		"\toutmins=60 tombdays=%lud mincopies=1 retain=8\n"
		"\n"
		"instance=n1.0 onnode=n1 addr=tcp!10.0.0.1!17011\n"
		"\tuuid=%s\n"
		"\tclass=ssd weight=100 status=in up=yes since=1 fenced=no\n"
		"instance=n1.1 onnode=n1 addr=tcp!10.0.0.1!17012\n"
		"\tuuid=0000000000000000000000000000000b\n"
		"\tclass=ssd weight=100 status=in up=yes since=1 fenced=no\n"
		"instance=n2.0 onnode=n2 addr=tcp!10.0.0.2!17011\n"
		"\tuuid=0000000000000000000000000000000c\n"
		"\tclass=ssd weight=100 status=in up=yes since=1 fenced=no\n"
		"\n"
		"stale=n1.0 reporter=n1.1 since=3\n"
		"stale=n2.0 reporter=n1.0 since=4\n"
		"stale=n1.1 reporter=n2.0 since=5\n",
		epoch, Tmonid, objmax, blksz, csumalg, tombdays, uuid);
	if(p == nil)
		sysfatal("smprint: %r");
	return p;
}

static char *
mkmap(void)
{
	return mkmapd(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid, 7);
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
static Srvctx *freedctx;
static int freedjobs;

static void
onfreed(void*)
{
	freedseen++;
	freedjobs = freedctx != nil && srvstopping(freedctx);
}

static Srvctx*
startsrv(Dev *d, char *maptext, int nq, ulong snapmax)
{
	Srvcfg cfg;
	Srvctx *c;

	memset(&cfg, 0, sizeof cfg);
	cfg.dev = d;
	cfg.maptext = maptext;
	cfg.maplen = strlen(maptext);
	cfg.nqueue = nq;
	cfg.store.spawn = tspawn;
	cfg.store.nockptproc = 1;
	cfg.store.ckwaitms = 200;
	cfg.store.emapcache = 16;
	cfg.store.stagemax = 8;
	cfg.store.stagetot = 12;
	cfg.store.stagems = 50;
	cfg.store.objsnapmax = snapmax;
	cfg.store.freed = onfreed;
	if((c = srvnew(&cfg)) == nil){
		fail("srvnew: %r");
		return nil;
	}
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

/* §1.5's delete: the object becomes a tombstone at this key */
static void
rmobj(Store *s, char *name, uvlong ver, uvlong wepoch)
{
	uchar oid[Oidmax];
	int len;

	len = strlen(name);
	memmove(oid, name, len);
	if(objremove(s, oid, len, ver, wepoch, nil, 0) < 0)
		fail("objremove %s: %r", name);
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

/* attach as role=admin and open /ctl for writing; 0 on failure */
static int
adminctl(Cl *cl, char *aname)
{
	Fcall r;
	char *w[1];

	if(clattach(cl, Froot, aname, &r) != Rattach){
		fail("attach %s: %s", aname, clerr(&r));
		return 0;
	}
	w[0] = "ctl";
	if(clopenpath(cl, Froot, Fctl, 1, w, OWRITE, &r) != Ropen){
		fail("open /ctl: %s", clerr(&r));
		return 0;
	}
	return 1;
}

/* the whole of a status file, walked and opened on a fresh fid */
static long
slurpfile(Cl *cl, ulong fid, char *name, char *buf, long max)
{
	Fcall r;
	char *w[1];
	long n;

	w[0] = name;
	if(clopenpath(cl, Froot, fid, 1, w, OREAD, &r) != Ropen){
		fail("open /%s: %s", name, clerr(&r));
		return -1;
	}
	n = clslurp(cl, fid, buf, max);
	if(n < 0)
		fail("read /%s: short", name);
	clclunk(cl, fid, &r);
	return n;
}

/* how many lines of buf begin with pfx */
static int
nlines(char *buf, char *pfx)
{
	char *p, *e;
	int n, k;

	n = 0;
	k = strlen(pfx);
	for(p = buf; p != nil && *p != 0; p = (e == nil ? nil : e+1)){
		e = strchr(p, '\n');
		if(strncmp(p, pfx, k) == 0)
			n++;
	}
	return n;
}

static int
haspfx(char *buf, char *pfx)
{
	char *p, *e;
	int k;

	k = strlen(pfx);
	for(p = buf; p != nil && *p != 0; p = (e == nil ? nil : e+1)){
		e = strchr(p, '\n');
		if(strncmp(p, pfx, k) == 0)
			return 1;
	}
	return 0;
}

static int
hasline(char *buf, char *line)
{
	char *p, *e;
	int k;

	k = strlen(line);
	for(p = buf; p != nil && *p != 0; p = (e == nil ? nil : e+1)){
		e = strchr(p, '\n');
		if(strncmp(p, line, k) == 0 && (p[k] == '\n' || p[k] == 0))
			return 1;
	}
	return 0;
}

/*
 * §2.5's grammar for the verbs this unit builds: what parses, what is
 * `bad ctl', and the role and fence gates over both.  The effects are
 * the cases below; this is the line the server accepts at all.
 */
static void
tparse(void)
{
	static struct {
		char	*line;
		char	*err;		/* nil: an Rwrite */
	} lines[] = {
		{"scrub",			nil},
		{"scrub stop",			nil},
		{"scrub rate=1",		nil},
		{"scrub rate=4096",		nil},
		{"scrub stop rate=77",		nil},
		{"scrub start",			nil},
		{"scrub start rate=100000",	nil},
		{"scrub start",			nil},	/* a second is accepted */
		{"scrub stop",			nil},
		{"scrub rate=0",		"bad ctl"},
		{"scrub rate=",			"bad ctl"},
		{"scrub rate=12x",		"bad ctl"},
		{"scrub rate=-1",		"bad ctl"},
		{"scrub go",			"bad ctl"},
		{"scrub rate=5 start",		"bad ctl"},
		{"scrub start stop",		"bad ctl"},
		{"scrub start start",		"bad ctl"},
		{"scrub start rate=5 more",	"bad ctl"},
		{"newmonid 00112233445566778899aabbccddeeff",	nil},
		{"newmonid 00112233445566778899aabbccddeef",	"bad ctl"},
		{"newmonid 00112233445566778899aabbccddeeff0",	"bad ctl"},
		{"newmonid 00112233445566778899aabbccddeegg",	"bad ctl"},
		{"newmonid",			"bad ctl"},
		{"forget n1.1",			nil},
		{"forget",			"bad ctl"},
		{"forget n1.1 n2.0",		"bad ctl"},
		{"drop not!a!name",		"bad object name"},
		{"drop",			"bad ctl"},
	};
	static char *fenced[] = {
		"forget n1.1",
		"drop whatever",
		"pull alpha n1.1",
		"push alpha n1.1",
		"reconcile",
		"advert",
	};
	static char *unfenced[] = {
		"scrub start",
		"scrub stop",
		"newmonid 00112233445566778899aabbccddeeff",
	};
	static char *notbuilt[] = {
		"refresh",
		"register",
		"pull alpha n1.1",
		"push alpha n1.1",
		"reconcile",
		"advert",
	};
	char what[128], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	int i;

	clstage = "parse";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	clstart(&cl, ctx, Clmsize);

	/* §2.5's role column: every one of these rows is admin */
	if(!adminctl(&cl, "role=client,epoch=7"))
		goto Out;
	for(i = 0; i < nelem(lines); i++){
		clwrite(&cl, Fctl, 0, lines[i].line, &r);
		snprint(what, sizeof what, "%#q as client", lines[i].line);
		clerris(what, &r, "permission denied");
	}
	clclunk(&cl, Fctl, &r);
	clclunk(&cl, Froot, &r);

	if(!adminctl(&cl, "role=admin"))
		goto Out;
	for(i = 0; i < nelem(lines); i++){
		clwrite(&cl, Fctl, 0, lines[i].line, &r);
		checks++;
		if(lines[i].err == nil){
			if(r.type != Rwrite)
				fail("%#q: %s", lines[i].line, clerr(&r));
			else
				eqv("an Rwrite counts the bytes written",
					r.count, strlen(lines[i].line));
		}else if(r.type != Rerror || strcmp(r.ename, lines[i].err) != 0)
			fail("%#q: %s, want %s", lines[i].line,
				r.type == Rerror ? r.ename : "ok",
				lines[i].err);
	}
	/* the verbs that gate and then answer the local `not built' */
	for(i = 0; i < nelem(notbuilt); i++){
		clwrite(&cl, Fctl, 0, notbuilt[i], &r);
		snprint(what, sizeof what, "%#q unfenced", notbuilt[i]);
		clerris(what, &r, "shoalsrv: not built");
	}

	/* §2.5's fenced set, under §6.4 F4's operator fence */
	if(clwrite(&cl, Fctl, 0, "fence on", &r) != Rwrite)
		fail("fence on: %s", clerr(&r));
	for(i = 0; i < nelem(fenced); i++){
		clwrite(&cl, Fctl, 0, fenced[i], &r);
		snprint(what, sizeof what, "%#q fenced", fenced[i]);
		clerris(what, &r, "fenced");
	}
	for(i = 0; i < nelem(unfenced); i++){
		clwrite(&cl, Fctl, 0, unfenced[i], &r);
		checks++;
		if(r.type != Rwrite)
			fail("%#q fenced: %s", unfenced[i], clerr(&r));
	}
	clwrite(&cl, Fctl, 0, "scrub stop", &r);
	if(clwrite(&cl, Fctl, 0, "fence off", &r) != Rwrite)
		fail("fence off: %s", clerr(&r));
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §2.5's `newmonid <hex32>' as far as the engine lets it go.
 *
 * §2.5 has the verb REPLACE this instance's pinned monid (§6.3), and
 * the engine's publisher for that value is monidpin, which pins a
 * value on a store that has none and refuses a DIFFERENT one on a
 * store that has: "monitor identity already pinned".  Every serving
 * instance has one, since start-up pins the map's (srvnew), so the
 * replacement §2.5 asks for is not reachable through the call this
 * verb has.  What is built and driven here is the rest of the verb —
 * the spelling, the gates, the durable publish and /status's report —
 * and the refusal the engine answers; store.md §14(32) records the
 * gap and what lib/ owes it.
 */
static void
tmonid(void)
{
	static char newid[] = "ffeeddccbbaa99887766554433221100";
	char buf[8192], val[64], line[64], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;

	clstage = "monid";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	if(slurpfile(&cl, Ffile, "status", buf, sizeof buf) > 0)
		eqs("/status monid= is the one start-up pinned",
			clfield(buf, "monid", val, sizeof val), Tmonid);

	/* the value already pinned: the publish is a no-op and succeeds */
	snprint(line, sizeof line, "newmonid %s", Tmonid);
	if(clwrite(&cl, Fctl, 0, line, &r) != Rwrite)
		fail("newmonid of the pinned value: %s", clerr(&r));
	if(slurpfile(&cl, Ffile, "status", buf, sizeof buf) > 0)
		eqs("/status monid= is unchanged by it",
			clfield(buf, "monid", val, sizeof val), Tmonid);

	/* a different one is what the engine has no call for */
	snprint(line, sizeof line, "newmonid %s", newid);
	clwrite(&cl, Fctl, 0, line, &r);
	clerris("newmonid of a different value", &r,
		"shoalsrv: monitor identity already pinned");
	if(slurpfile(&cl, Ffile, "status", buf, sizeof buf) > 0)
		eqs("/status monid= is unchanged by the refusal",
			clfield(buf, "monid", val, sizeof val), Tmonid);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * layer-a §2.2's status files, each rendered at open: /dirty's two
 * kinds of line (§7.1), /stale's marks out of the map's ledger,
 * /tombs and /advert in §7.2's grammar, /lost's two kinds of line
 * (store.md §9), and the offsets a read of any of them is served at.
 */
static void
tfiles(void)
{
	char buf[16*1024], *m;
	uchar data[2048];
	Objinfo oi;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	char line[512], csum[2*Csumlen+1], *w[1];
	long n;
	int i;

	clstage = "files";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	memset(data, 0x5a, sizeof data);
	mkobj(st, "alpha", data, sizeof data, 1);
	mkobj(st, "beta", data, 512, 1);
	mkobj(st, "gamma", nil, 0, 1);
	rmobj(st, "gamma", 9, 3);		/* a tombstone at (9, 3) */
	if(dirtyadd(st, (uchar*)"alpha", 5, "n1.1", 11) < 0)
		fail("dirtyadd: %r");
	if(dirtyadd(st, (uchar*)"beta", 4, "n2.0", 12) < 0)
		fail("dirtyadd: %r");

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}

	/* /dirty: one line per record, then §7.1's coarse flag per peer */
	n = slurpfile(&cl, Ffile, "dirty", buf, sizeof buf);
	istrue("/dirty renders", n > 0);
	istrue("/dirty carries alpha's record",
		hasline(buf, "oid=alpha peer=n1.1 epoch=11"));
	istrue("/dirty carries beta's record",
		hasline(buf, "oid=beta peer=n2.0 epoch=12"));
	eqv("/dirty has one record line per record", nlines(buf, "oid="), 2);
	istrue("/dirty carries a fullsync line for n1.1",
		hasline(buf, "fullsync peer=n1.1"));
	istrue("/dirty carries a fullsync line for n2.0",
		hasline(buf, "fullsync peer=n2.0"));

	/* /stale: the marks this instance is party to, and no others */
	n = slurpfile(&cl, Ffile, "stale", buf, sizeof buf);
	istrue("/stale renders", n > 0);
	istrue("/stale carries the mark naming this instance the subject",
		hasline(buf, "stale=n1.0 reporter=n1.1 since=3"));
	istrue("/stale carries the mark naming it the reporter",
		hasline(buf, "stale=n2.0 reporter=n1.0 since=4"));
	eqv("/stale carries no mark this instance is not party to",
		nlines(buf, "stale="), 2);

	/* /tombs: §7.2's line, over the tombstones alone */
	if(statof(st, "gamma", &oi) < 0)
		fail("objstat gamma: %r");
	else{
		for(i = 0; i < Csumlen; i++)
			snprint(csum+2*i, 3, "%.2ux", oi.csum[i]);
		snprint(line, sizeof line,
			"oid=gamma ver=9 wepoch=3 csum=%s len=0 state=tomb",
			csum);
	}
	n = slurpfile(&cl, Ffile, "tombs", buf, sizeof buf);
	istrue("/tombs renders", n > 0);
	eqv("/tombs lists one entry", nlines(buf, "oid="), 1);
	istrue("a tombstone's line carries len=0 state=tomb", hasline(buf, line));

	/* /advert: the same grammar over both states (§7.2) */
	if(clattach(&cl, Froot2, "role=repl,peer=n1.1", &r) != Rattach)
		fail("attach repl: %s", clerr(&r));
	w[0] = "advert";
	if(clopenpath(&cl, Froot2, Ffile2, 1, w, OREAD, &r) != Ropen)
		fail("open /advert: %s", clerr(&r));
	else{
		n = clslurp(&cl, Ffile2, buf, sizeof buf);
		istrue("/advert renders", n > 0);
		eqv("/advert lists every object, live and tomb",
			nlines(buf, "oid="), 3);
		istrue("/advert carries the tombstone", hasline(buf, line));
		istrue("/advert carries a live object",
			nlines(buf, "oid=alpha ") == 1);
		clclunk(&cl, Ffile2, &r);
	}
	clclunk(&cl, Froot2, &r);

	/* /lost: nothing has failed verification yet */
	eqv("/lost is empty while nothing fails verification",
		slurpfile(&cl, Ffile, "lost", buf, sizeof buf), 0);

	/* /jobs: nothing is running */
	eqv("/jobs is empty while no pass runs",
		slurpfile(&cl, Ffile, "jobs", buf, sizeof buf), 0);

	/*
	 * §2.2's render-at-open, and the offsets a read is served at.
	 * The fid is opened before the record is added, so its bytes do
	 * not move under it.
	 */
	w[0] = "dirty";
	if(clopenpath(&cl, Froot, Ffile, 1, w, OREAD, &r) != Ropen)
		fail("re-open /dirty: %s", clerr(&r));
	else{
		if(dirtyadd(st, (uchar*)"alpha", 5, "n2.0", 13) < 0)
			fail("dirtyadd: %r");
		n = clslurp(&cl, Ffile, buf, sizeof buf);
		eqv("a snapshot taken at open does not grow under a write",
			nlines(buf, "oid="), 2);
		if(clread(&cl, Ffile, 4, 3, &r) == Rread){
			eqv("a short read answers what was asked", r.count, 3);
			istrue("a read at an offset answers those bytes",
				memcmp(r.data, buf+4, 3) == 0);
		}else
			fail("short read of /dirty: %s", clerr(&r));
		if(clread(&cl, Ffile, n+100, 16, &r) == Rread)
			eqv("a read past the end answers count 0", r.count, 0);
		else
			fail("read past the end: %s", clerr(&r));
		if(clread(&cl, Ffile, n-2, 16, &r) == Rread)
			eqv("a read crossing the end is clamped", r.count, 2);
		else
			fail("read crossing the end: %s", clerr(&r));
		clclunk(&cl, Ffile, &r);
	}
	/* a fid opened after it sees the third record */
	if(slurpfile(&cl, Ffile, "dirty", buf, sizeof buf) <= 0)
		fail("re-slurp /dirty");
	eqv("a later open sees the record the first one missed",
		nlines(buf, "oid="), 3);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/* every name a directory listing answered, one per line into names[] */
static int
dirnames(char *buf, long n, char (*names)[Oidmax+1], int max)
{
	uchar *p, *ep;
	Dir dir;
	int k, m;

	k = 0;
	p = (uchar*)buf;
	ep = p + n;
	while(p < ep){
		m = convM2D(p, ep-p, &dir, (char*)p+BIT16SZ);
		if(m <= BIT16SZ)
			break;
		if(k < max)
			strecpy(names[k], names[k]+Oidmax+1, dir.name);
		k++;
		p += m;
	}
	return k;
}

static int
hasname(char (*names)[Oidmax+1], int n, char *want)
{
	int i;

	for(i = 0; i < n; i++)
		if(strcmp(names[i], want) == 0)
			return 1;
	return 0;
}

/*
 * The /obj and /meta directory reads, over the snapshot the open
 * took.  layer-a §2.2: every live object MUST be enumerated, no
 * tombstone MAY be, and no entry may be skipped or duplicated
 * part-way through a sequential read under concurrent mutation.
 */
static void
tdir(void)
{
	char names[64][Oidmax+1];
	char buf[16*1024], name[32], *m;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	char *w[1];
	long n, off;
	int i, k, nent;

	clstage = "dir";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	for(i = 0; i < 20; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		mkobj(st, name, nil, 0, 1);
	}
	rmobj(st, "obj19", 9, 3);		/* a tombstone, never listed */

	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}

	/* the whole listing, in one client-side loop */
	n = slurpfile(&cl, Fdir, "obj", buf, sizeof buf);
	istrue("/obj renders", n > 0);
	nent = dirnames(buf, n, names, nelem(names));
	eqv("/obj lists every live object", nent, 19);
	istrue("/obj lists the first", hasname(names, nent, "obj00"));
	istrue("/obj lists the last live one", hasname(names, nent, "obj18"));
	istrue("/obj does not list a tombstone",
		!hasname(names, nent, "obj19"));

	/* /meta is the same listing under a second name */
	n = slurpfile(&cl, Fdir, "meta", buf, sizeof buf);
	nent = dirnames(buf, n, names, nelem(names));
	eqv("/meta lists the same objects", nent, 19);

	/*
	 * A read at offset 0 restarts the cursor (store.md §9), a read at
	 * an offset the fid did not leave is a seek in a directory, and
	 * the offset the last read started at rewinds.
	 */
	w[0] = "obj";
	if(clopenpath(&cl, Froot, Fdir, 1, w, OREAD, &r) != Ropen){
		fail("open /obj: %s", clerr(&r));
		goto Out;
	}
	if(clread(&cl, Fdir, 0, 200, &r) != Rread){
		fail("first read: %s", clerr(&r));
		goto Out;
	}
	n = r.count;
	memmove(buf, r.data, n);
	k = dirnames(buf, n, names, nelem(names));
	istrue("a bounded read answers whole entries and stops", k > 0 && k < 19);
	/*
	 * lib9p refuses a directory read at an offset that is neither 0
	 * nor where the fid left off, against its own Fid.diroffset, and
	 * it does so before this row's read cell is reached (srv.c's
	 * Ebadoffset).  The cursor's own refusal is therefore behind that
	 * guard on the wire; what the wire shows is the guard.
	 */
	off = n;
	clread(&cl, Fdir, off+1, 200, &r);
	clerris("a read at an offset the fid did not leave", &r, "bad offset");
	if(clread(&cl, Fdir, off, 200, &r) != Rread)
		fail("a read at the offset the fid left: %s", clerr(&r));
	if(clread(&cl, Fdir, 0, sizeof buf, &r) != Rread)
		fail("a read at offset 0: %s", clerr(&r));
	else{
		memmove(buf, r.data, r.count);
		k = dirnames(buf, r.count, names, nelem(names));
		eqv("a read at offset 0 restarts the listing", k, 19);
	}
	clclunk(&cl, Fdir, &r);

	/*
	 * An entry going live -> tomb under an open fid.  §9's snapshot
	 * names it, and objsnapent answers `gone' for it because its
	 * state is no longer one the open asked for — so it is skipped,
	 * and the entries after it are neither skipped nor repeated.
	 * An object created after the open is not in the vector at all.
	 */
	if(clopenpath(&cl, Froot, Fdir, 1, w, OREAD, &r) != Ropen){
		fail("re-open /obj: %s", clerr(&r));
		goto Out;
	}
	if(clread(&cl, Fdir, 0, 200, &r) != Rread){
		fail("partial read: %s", clerr(&r));
		goto Out;
	}
	n = r.count;
	memmove(buf, r.data, n);
	k = dirnames(buf, n, names, nelem(names));
	/* delete everything this read has not yet reached, and add one */
	for(i = 0; i < 19; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		if(!hasname(names, k, name))
			rmobj(st, name, 9, 3);
	}
	mkobj(st, "latecomer", nil, 0, 1);
	off = n;
	for(;;){
		if(clread(&cl, Fdir, off, 4096, &r) != Rread){
			fail("read on: %s", clerr(&r));
			goto Out;
		}
		if(r.count == 0)
			break;
		if(n + r.count > sizeof buf)
			break;
		memmove(buf+n, r.data, r.count);
		n += r.count;
		off += r.count;
	}
	nent = dirnames(buf, n, names, nelem(names));
	eqv("an entry that went to tomb mid-read is not listed", nent, k);
	istrue("an object created after the open is not listed",
		!hasname(names, nent, "latecomer"));
	for(i = 0; i < nent; i++)
		for(k = i+1; k < nent; k++)
			if(strcmp(names[i], names[k]) == 0){
				fail("the listing repeats %s", names[i]);
				i = nent;
				break;
			}
	clclunk(&cl, Fdir, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * store.md §9's two refusals at the open, as the wire carries them.
 *
 * The bound's is layer-a §2.6's `disk full' with D20's detail, and it
 * goes out verbatim — err.c passes a §2.6 string through, and a
 * server that wrapped it would have made a new prefix out of a
 * condition §2.6 already names.  The index-moved one is not a §2.6
 * condition at all, and §9 asks a server to retry the open once
 * before answering: §13's snapstale point arms exactly as many failed
 * fills as a test wants, so eight are survived by the retry and
 * sixteen are not.
 */
static void
tsnaprefuse(void)
{
	char buf[4096], val[64], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	char *w[1];

	clstage = "snaprefuse";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 2, 0)) == nil)	/* objsnapmax stays the default */
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0, 1);
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	w[0] = "obj";

	/* eight failed fills: the retry is what makes this open succeed */
	storehook(srvstore(ctx), "snapshort", 4096);
	storehook(srvstore(ctx), "snapstale", 8);
	if(clopenpath(&cl, Froot, Fdir, 1, w, OREAD, &r) != Ropen)
		fail("an open retried once after `the index moved': %s",
			clerr(&r));
	else
		clclunk(&cl, Fdir, &r);

	/* sixteen: both the open and its retry give up, and the client is told */
	storehook(srvstore(ctx), "snapstale", 16);
	clopenpath(&cl, Froot, Fdir2, 1, w, OREAD, &r);
	clerris("an open whose retry also failed", &r,
		"shoalsrv: object snapshot: the index moved under 8 counts");
	clclunk(&cl, Fdir2, &r);
	storehook(srvstore(ctx), "snapstale", 0);
	storehook(srvstore(ctx), "snapshort", 0);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);

	/* the bound, with room for two snapshots and three fids wanting one */
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 2, 2)) == nil)
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0, 1);
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out2;
	}
	w[0] = "obj";
	if(clopenpath(&cl, Froot, Fdir, 1, w, OREAD, &r) != Ropen)
		fail("the first snapshot: %s", clerr(&r));
	w[0] = "meta";
	if(clopenpath(&cl, Froot, Fdir2, 1, w, OREAD, &r) != Ropen)
		fail("the second snapshot: %s", clerr(&r));
	if(slurpfile(&cl, Ffile, "status", buf, sizeof buf) > 0)
		eqs("/status objsnapopen= counts the open snapshots",
			clfield(buf, "objsnapopen", val, sizeof val), "2");
	w[0] = "obj";
	clopenpath(&cl, Froot, Fdir3, 1, w, OREAD, &r);
	clerris("an open past objsnapmax", &r,
		"disk full: 2 object snapshots open, objsnapmax 2");
	clclunk(&cl, Fdir3, &r);
	/* a close releases the count, and the next open finds room */
	clclunk(&cl, Fdir, &r);
	if(clopenpath(&cl, Froot, Fdir, 1, w, OREAD, &r) != Ropen)
		fail("an open after a close released the count: %s", clerr(&r));
	clclunk(&cl, Fdir, &r);
	clclunk(&cl, Fdir2, &r);
Out2:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * One field out of the first /jobs line, or nil.  clfield is for the
 * attr=value-per-line files; a /jobs line is one record with several
 * attributes on it (layer-a §2.2: one line per job), so this reads
 * the attribute out of the line instead.
 */
static char*
jobfield(Cl *cl, char *attr, char *val, int nval)
{
	char buf[4096], want[32], *p, *e;

	if(slurpfile(cl, Ffile2, "jobs", buf, sizeof buf) <= 0)
		return nil;
	snprint(want, sizeof want, "%s=", attr);
	if((p = strstr(buf, want)) == nil)
		return nil;
	p += strlen(want);
	for(e = p; *e != 0 && *e != ' ' && *e != '\n' && *e != '/'; e++)
		;
	if(e - p >= nval)
		return nil;
	memmove(val, p, e-p);
	val[e-p] = 0;
	return val;
}

/* is any pass listed at /jobs? */
static int
jobrunning(Cl *cl)
{
	char buf[4096];

	return slurpfile(cl, Ffile2, "jobs", buf, sizeof buf) > 0;
}

/*
 * layer-a §7.5's scrub pass: it walks the index, flags the copy whose
 * bytes no longer hash to their digest, and /lost and /status's
 * `lost=' are where that shows.  §2.5's verb returns once the pass is
 * accepted, so /jobs is what the case waits on.
 */
static void
tscrub(void)
{
	char buf[16*1024], val[64], *m;
	uchar data[4096], probe[4096];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	Super sb;
	Sbsel sel;
	vlong off, end;
	int i, found;

	clstage = "scrub";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	for(i = 0; i < sizeof data; i++)
		data[i] = (uchar)(0x31 + (i & 0x3f));
	mkobj(srvstore(ctx), "alpha", data, sizeof data, 1);
	mkobj(srvstore(ctx), "beta", nil, 0, 1);

	/* damage alpha's bytes on the platter, as srvtest's verify does */
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
			probe[23] ^= 0xff;
			simpoke(d, off, probe, Tblksz);
			found++;
			break;
		}
	}
	istrue("an object's bytes were found in the data region", found > 0);

	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	if(slurpfile(&cl, Ffile, "lost", buf, sizeof buf) != 0)
		fail("/lost is not empty before the pass");
	if(clwrite(&cl, Fctl, 0, "scrub start rate=1000000", &r) != Rwrite){
		fail("scrub start: %s", clerr(&r));
		goto Out;
	}
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	istrue("the pass ended and left /jobs empty", !jobrunning(&cl));

	if(slurpfile(&cl, Ffile, "lost", buf, sizeof buf) > 0){
		istrue("/lost names the damaged copy",
			haspfx(buf, "oid=alpha kind=corrupt slot="));
		eqv("/lost names the damaged copy alone",
			nlines(buf, "oid="), 1);
	}else
		fail("/lost is empty after a pass that found damage");
	if(slurpfile(&cl, Ffile, "status", buf, sizeof buf) > 0)
		eqs("/status lost= counts it",
			clfield(buf, "lost", val, sizeof val), "1");

	/*
	 * store.md §8: every block matching clears the flag, so a pass
	 * over the repaired bytes takes the object back out of /lost.
	 */
	simpoke(d, off, data, Tblksz);
	if(clwrite(&cl, Fctl, 0, "scrub start", &r) != Rwrite)
		fail("second scrub start: %s", clerr(&r));
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	if(slurpfile(&cl, Ffile, "lost", buf, sizeof buf) >= 0)
		eqv("a pass over repaired bytes clears the flag",
			nlines(buf, "oid="), 0);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * store.md §13's T1.27, as far as a server can be driven to show it:
 * the scrub's read of one object is held inside THAT object's queue.
 *
 * §8's argument is that a scrubber reading beside the queue hits
 * grains a commit on the same object freed and another object staged
 * into, and durably flags a live, correct object `corrupt'.  What a
 * test can hold the server to is the property that argument rests on
 * — that the unit of scrub work is pushed to the oid's Reqqueue — and
 * the way to see it is to occupy that queue: with §13's `objhold'
 * point set, a client's `verify <oid>' parks inside the queue, and
 * the pass cannot get past that object until it is let go.  A pass
 * that called the engine beside the queue would walk straight past.
 *
 * The object the client holds is chosen by its index slot, so that
 * the pass reaches it with slots still ahead of it: `done=' standing
 * still at that slot is the pass waiting, and not the pass finished.
 */
static void
tqueued(void)
{
	char val[64], line[64], name[32], *m;
	Objinfo oi;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall t, r;
	char *w[1];
	ushort ta;
	uvlong done;
	int i;

	clstage = "queued";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 8, 0)) == nil)
		return;
	st = srvstore(ctx);
	for(i = 0; i < 12; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		mkobj(st, name, nil, 0, 1);
	}
	if(statof(st, "obj04", &oi) < 0){
		fail("objstat obj04: %r");
		goto Out;
	}

	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;

	/* park a client request inside obj04's queue */
	srvhook(ctx, "objhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Fctl;
	t.offset = 0;
	t.data = "verify obj04";
	t.count = strlen(t.data);
	clput(&cl, &t);
	sleep(100);

	/* and start the pass over the whole index */
	if(clattach(&cl, Froot2, "role=admin", &r) != Rattach)
		fail("second attach: %s", clerr(&r));
	snprint(line, sizeof line, "scrub start rate=1000000");
	w[0] = "ctl";
	if(clopenpath(&cl, Froot2, Ffile, 1, w, OWRITE, &r) != Ropen)
		fail("open the second /ctl: %s", clerr(&r));
	else if(clwrite(&cl, Ffile, 0, line, &r) != Rwrite)
		fail("scrub start: %s", clerr(&r));
	sleep(60);
	istrue("the pass is listed at /jobs", jobrunning(&cl));

	/* the pass reaches obj04's slot and stops there */
	done = ~0ULL;
	for(i = 0; i < 100; i++){
		sleep(20);
		if(jobfield(&cl, "done", val, sizeof val) == nil)
			break;
		done = strtoull(val, nil, 10);
		if(done >= oi.slot+1)
			break;
	}
	eqv("the pass stops at the slot whose queue is held",
		done, (uvlong)oi.slot+1);
	sleep(300);
	if(jobfield(&cl, "done", val, sizeof val) == nil)
		fail("the pass ended while the object's queue was held");
	else
		eqv("and does not advance past it", strtoull(val, nil, 10),
			(uvlong)oi.slot+1);
	istrue("slots remain ahead of it", oi.slot+1 < 12);

	/* let the held request go: the pass finishes the index */
	srvhook(ctx, "objhold", 0);
	if(clgettag(&cl, ta, &r) != Rwrite)
		fail("the held verify: %s", clerr(&r));
	cltagfree(&cl, ta);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	istrue("the pass runs to the end once the queue is free",
		!jobrunning(&cl));
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot2, &r);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The pass's units are the pool's: srvqjob counts a push on the way in
 * and a completion on the way out, exactly as a client's request is
 * counted, so the shutdown's drain (D16) sees a pass's work in flight
 * and the store outlives it.  What a test can hold the server to is
 * the arithmetic: a pass over an index holding `nlive' live objects
 * and some tombstones pushes one unit per LIVE object and none for a
 * tombstone (layer-a §7.5 re-verifies live copies), and the two counts
 * balance once it has ended.
 *
 * The counts are read with no 9P traffic in between, because a walk or
 * an open of some rows pushes to the pool too; the pass is given a
 * fixed wait rather than polled at /jobs for the same reason.
 */
static void
tqjobcount(void)
{
	char buf[4096], name[32], *m;
	uvlong p0, d0, p1, d1;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	int i;

	clstage = "qjobcount";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	for(i = 0; i < 8; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		mkobj(st, name, nil, 0, 1);
	}
	for(i = 5; i < 8; i++){			/* three of them go to tomb */
		snprint(name, sizeof name, "obj%.2d", i);
		rmobj(st, name, 9, 3);
	}
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	srvcount(ctx, &p0, &d0);
	if(clwrite(&cl, Fctl, 0, "scrub start rate=1000000", &r) != Rwrite){
		fail("scrub start: %s", clerr(&r));
		goto Out;
	}
	sleep(1500);				/* 128 slots at that rate */
	srvcount(ctx, &p1, &d1);
	eqv("the pass pushed one unit per live object",
		p1 - p0, 5);
	eqv("and the pool counted every one of them off again",
		d1 - d0, p1 - p0);
	istrue("the pass had ended", !jobrunning(&cl));
	if(slurpfile(&cl, Ffile, "tombs", buf, sizeof buf) >= 0)
		eqv("the tombstones it did not push are still there",
			nlines(buf, "oid="), 3);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * Wait for a pass to reach the end of its walk and park at §13's
 * jobhold point, then answer one field of its /jobs line.  That point
 * is what makes a finished pass readable at all: /jobs lists a pass
 * only while it is running or queued (layer-a §2.2), so its counters
 * and the error it gave up with are gone the moment it unlinks.
 *
 * The walk being over is `done=' having reached `total=', which
 * jobfield reads as the numerator of the done=<n>/<n> pair.  Answers
 * nil if no pass parked.
 */
static char*
jobparked(Cl *cl, char *attr, char *val, int nval)
{
	char buf[4096], *p;
	uvlong done, total;
	int i;

	for(i = 0; i < 500; i++){
		if(slurpfile(cl, Ffile2, "jobs", buf, sizeof buf) > 0
		&& (p = strstr(buf, "done=")) != nil){
			done = strtoull(p+5, &p, 10);
			total = *p == '/' ? strtoull(p+1, nil, 10) : 0;
			if(total != 0 && done == total)
				return jobfield(cl, attr, val, nval);
		}
		sleep(20);
	}
	return nil;
}

/*
 * store.md §9's tombstone reclaim walk, which rides on the scrub pass
 * and discards NOTHING.
 *
 * layer-a §1.5 licenses a discard only when all three of its
 * conditions hold, and the two the walk can test are local: the
 * entry's mtime against the map header's `tombdays', and its wepoch
 * strictly below the map epoch.  The third — confirmation from every
 * non-dead instance in the map — has nothing to answer it while there
 * is no peer client, so what the walk produces is the count at /jobs
 * and every record stays where it is.  Both sides of both local tests
 * are driven here, and the count is read while the pass is parked at
 * §13's jobhold point, since /jobs lists a pass only while it runs.
 */
static void
treclaim(void)
{
	char buf[8192], val[128], *m;
	Objinfo oi;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	int i;

	clstage = "reclaim";
	/* tombdays=7: nothing written today is old enough */
	m = mkmapd(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid, 7);
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	mkobj(st, "old", nil, 0, 1);
	mkobj(st, "young", nil, 0, 1);
	rmobj(st, "old", 9, 3);			/* wepoch 3 < the map's 7 */
	rmobj(st, "young", 9, 7);		/* wepoch 7, not below it */
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	srvhook(ctx, "jobhold", 1);
	if(clwrite(&cl, Fctl, 0, "scrub start rate=1000000", &r) != Rwrite)
		fail("scrub start: %s", clerr(&r));
	if(jobparked(&cl, "reclaimable", val, sizeof val) == nil)
		fail("the pass never reached its hold");
	else
		eqs("a tombstone younger than tombdays is not reclaimable",
			val, "0");
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	if(slurpfile(&cl, Ffile, "tombs", buf, sizeof buf) >= 0)
		eqv("and both tombstones are still there",
			nlines(buf, "oid="), 2);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);

	/*
	 * tombdays=0: the cutoff is the present, so a tombstone written
	 * before this second is past it.  The wepoch test is what still
	 * separates the two — `young' carries the map's own epoch, and
	 * §1.5's condition 3 wants the epoch strictly above it.  Nothing
	 * re-checks that condition downstream any more, since there is no
	 * discard to re-check it: the count is the whole of the verdict.
	 */
	m = mkmapd(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid, 0);
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	mkobj(st, "old", nil, 0, 1);
	mkobj(st, "young", nil, 0, 1);
	rmobj(st, "old", 9, 3);
	rmobj(st, "young", 9, 7);
	sleep(1100);				/* past the cutoff's second */
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out2;
	srvhook(ctx, "jobhold", 1);
	if(clwrite(&cl, Fctl, 0, "scrub start rate=1000000", &r) != Rwrite)
		fail("scrub start: %s", clerr(&r));
	if(jobparked(&cl, "reclaimable", val, sizeof val) == nil)
		fail("the pass never reached its hold");
	else
		eqs("the tombstone past both cutoffs is counted, and it "
			"alone: the other's wepoch is not below the map "
			"epoch", val, "1");
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	if(slurpfile(&cl, Ffile, "tombs", buf, sizeof buf) >= 0){
		eqv("a counted tombstone is kept: §1.5's condition 1 has "
			"nothing to answer it", nlines(buf, "oid="), 2);
		istrue("including the one past both cutoffs",
			nlines(buf, "oid=old ") == 1);
	}
	istrue("and its record is still in the index",
		statof(st, "old", &oi) == 0);
Out2:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §2.5's `forget <iid>' (§7.1): the fine-grained records for that
 * peer go and no others do.  The coarse `fullsync' flag has no setter
 * and is already set for every peer the store knows of (store.md §9),
 * so what the verb achieves today is the discard, and /dirty's
 * fullsync half is unchanged by it.
 */
static void
tforget(void)
{
	char buf[8192], val[64], *m;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	int i;

	clstage = "forget";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	mkobj(st, "alpha", nil, 0, 1);
	mkobj(st, "beta", nil, 0, 1);
	if(dirtyadd(st, (uchar*)"alpha", 5, "n1.1", 11) < 0
	|| dirtyadd(st, (uchar*)"beta", 4, "n1.1", 11) < 0
	|| dirtyadd(st, (uchar*)"alpha", 5, "n2.0", 12) < 0)
		fail("dirtyadd: %r");

	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	if(slurpfile(&cl, Ffile, "dirty", buf, sizeof buf) > 0)
		eqv("/dirty carries three records before the verb",
			nlines(buf, "oid="), 3);
	if(clwrite(&cl, Fctl, 0, "forget n1.1", &r) != Rwrite){
		fail("forget: %s", clerr(&r));
		goto Out;
	}
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	istrue("the forget pass ended", !jobrunning(&cl));
	if(slurpfile(&cl, Ffile, "dirty", buf, sizeof buf) > 0){
		eqv("the records for that peer are gone",
			nlines(buf, "oid="), 1);
		istrue("and the other peer's record is not",
			hasline(buf, "oid=alpha peer=n2.0 epoch=12"));
		istrue("the coarse flag for that peer is untouched",
			hasline(buf, "fullsync peer=n1.1"));
	}
	if(slurpfile(&cl, Ffile, "status", buf, sizeof buf) > 0)
		eqs("/status dirty= falls with them",
			clfield(buf, "dirty", val, sizeof val), "1");
	/* a peer with nothing recorded is not an error */
	if(clwrite(&cl, Fctl, 0, "forget n2.0", &r) != Rwrite)
		fail("forget a peer with one record: %s", clerr(&r));
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	if(slurpfile(&cl, Ffile, "dirty", buf, sizeof buf) >= 0)
		eqv("the last record goes too", nlines(buf, "oid="), 0);
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * /jobs lists every pass, and the passes are bounded.
 *
 * layer-a §2.2 wants "one line per running or queued background job",
 * which is every one of them; and §2.5 bounds neither verb, so
 * `forget <iid>' — one proc and one dirty snapshot per write — needed
 * a bound of this server's (store.md §14(30)).  Twelve passes may run
 * at once and a thirteenth is refused, which is the one thing §2.5's
 * "return success once the job is accepted" leaves room to say.
 *
 * §13's jobhold point is what holds the passes still: each parks at
 * the end of its walk with its record still listed, so the case can
 * stack them up and count the lines.
 */
static void
tjobs(void)
{
	char buf[16*1024], line[64], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	int i;

	clstage = "jobs";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	srvhook(ctx, "jobhold", 1);
	for(i = 0; i < 9; i++){
		snprint(line, sizeof line, "forget peer%.2d", i);
		if(clwrite(&cl, Fctl, 0, line, &r) != Rwrite){
			fail("%#q: %s", line, clerr(&r));
			goto Out;
		}
	}
	if(slurpfile(&cl, Ffile, "jobs", buf, sizeof buf) > 0)
		eqv("/jobs renders a line for every pass, not the first few",
			nlines(buf, "job="), 9);
	else
		fail("/jobs is empty with nine passes accepted");

	/* the cap: three more are taken, and the one past it is refused */
	for(i = 9; i < 12; i++){
		snprint(line, sizeof line, "forget peer%.2d", i);
		if(clwrite(&cl, Fctl, 0, line, &r) != Rwrite)
			fail("%#q: %s", line, clerr(&r));
	}
	if(slurpfile(&cl, Ffile, "jobs", buf, sizeof buf) > 0)
		eqv("/jobs renders all twelve", nlines(buf, "job="), 12);
	clwrite(&cl, Fctl, 0, "forget peer12", &r);
	clerris("a pass past the cap", &r, "shoalsrv: too many jobs");
	/* a second `forget' of a peer already running is one of the twelve */
	clwrite(&cl, Fctl, 0, "forget peer00", &r);
	clerris("and so is a repeat of a peer already running", &r,
		"shoalsrv: too many jobs");

	/* let them all go: the list empties and the cap is free again */
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	istrue("every pass ended once the hold was cleared", !jobrunning(&cl));
	if(clwrite(&cl, Fctl, 0, "forget peer12", &r) != Rwrite)
		fail("a pass after the list emptied: %s", clerr(&r));
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
Out:
	srvhook(ctx, "jobhold", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * §2.5's `drop <oid>' and §7.4's guard.  The engine holds no map, so
 * the check that this instance is not in P(oid) is the server's; an
 * id it IS placed for is `still placed' whether or not a copy is
 * here, because the guard runs before the engine's drop.
 */
static void
tdrop(void)
{
	char line[64], name[32], *m;
	Cinst *pl[Maxplace];
	Objinfo oi;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	char *placed, *stray;
	int i, j, n;

	clstage = "drop";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);

	/* one id this instance is placed for and one it is not */
	placed = stray = nil;
	for(i = 0; i < 40 && (placed == nil || stray == nil); i++){
		snprint(name, sizeof name, "obj%.2d", i);
		n = mapplace(srvmap(ctx), name, pl, nelem(pl));
		if(n > nelem(pl))
			n = nelem(pl);
		for(j = 0; j < n; j++)
			if(strcmp(pl[j]->iid, srviid(ctx)) == 0)
				break;
		if(j < n){
			if(placed == nil)
				placed = strdup(name);
		}else if(stray == nil)
			stray = strdup(name);
	}
	if(placed == nil || stray == nil){
		fail("no pair of ids straddling this instance's placement");
		goto Out;
	}
	mkobj(st, placed, nil, 0, 1);
	mkobj(st, stray, nil, 0, 1);

	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	snprint(line, sizeof line, "drop %s", placed);
	clwrite(&cl, Fctl, 0, line, &r);
	clerris("a drop of an id this instance is placed for", &r,
		"still placed");
	istrue("and the copy is still there", statof(st, placed, &oi) == 0);

	snprint(line, sizeof line, "drop %s", stray);
	if(clwrite(&cl, Fctl, 0, line, &r) != Rwrite)
		fail("a drop of a stray: %s", clerr(&r));
	istrue("the stray copy is gone", statof(st, stray, &oi) < 0);
	istrue("and it left no tombstone behind", statof(st, stray, &oi) < 0);

	/* a second drop of the same id has nothing to remove */
	clwrite(&cl, Fctl, 0, line, &r);
	clerris("a second drop of the same id", &r, "no such object");
Out:
	free(placed);
	free(stray);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * D16's order with a pass running: srv.h makes the shutdown wait for
 * the background jobs after it has drained the requests, because
 * store.md §9 forbids closing the store while anything is still
 * inside the engine and the drain cannot see a proc that is not a
 * request.  So the store closes only after the pass has ended — and a
 * pass tests srvstopping between objects, so it ends rather than
 * running the shutdown out.
 *
 * The /obj fids the case leaves open are the other half of D16: a
 * snapshot MAY outlive storeclose, and it is what a directory fid
 * hands back after the store has gone.
 */
static void
tshutdown(void)
{
	char name[32], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	char *w[1];
	int i;

	clstage = "shutdown";
	m = mkmap();
	d = newdisk();
	freedseen = 0;
	freedjobs = -1;
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	for(i = 0; i < 40; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		mkobj(srvstore(ctx), name, nil, 0, 1);
	}
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	/* a rate low enough that the pass is certainly still walking */
	if(clwrite(&cl, Fctl, 0, "scrub start rate=1", &r) != Rwrite){
		fail("scrub start: %s", clerr(&r));
		goto Out;
	}
	istrue("the pass is listed at /jobs", jobrunning(&cl));
	w[0] = "obj";
	if(clopenpath(&cl, Froot, Fdir, 1, w, OREAD, &r) != Ropen)
		fail("open /obj: %s", clerr(&r));
Out:
	clstop(&cl);			/* the loop ends; the shutdown runs */
	eqv("the store was closed once", freedseen, 1);
	istrue("the pass had ended before the store closed", freedjobs == 1);
	istrue("no job is left running", srvjobstart(ctx) < 0);
	/* the fid, and the snapshot it holds, are given back after that */
	srvfree(ctx);
	eqv("the store was not closed twice", freedseen, 1);
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

	tparse();
	tmonid();
	tfiles();
	tdir();
	tsnaprefuse();
	tscrub();
	tqueued();
	tqjobcount();
	treclaim();
	tforget();
	tjobs();
	tdrop();
	tshutdown();

	clwatchoff();
	if(fails > 0){
		fprint(2, "srvenumtest: %d of %d checks failed\n", fails, checks);
		threadexitsall("fail");
	}
	print("srvenumtest: %d checks ok\n", checks);
	threadexitsall(nil);
}
