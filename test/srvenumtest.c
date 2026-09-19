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
 * object I/O, §7.5's scrub pass and store.md §9's tombstone reclaim —
 * the last both as `reclaim [start|stop]' and as the timer that runs
 * it with no verb written — as the scrub's own timer runs the pass
 * §7.5 calls continuous.  store.md §13's T1.27 is here too: that a
 * scrub read of one object is held inside that object's queue.
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

	/*
	 * How many checks a whole run makes, asserted at the end: see
	 * threadmain.  Every check this file makes is unconditional once
	 * its case is entered, so the number is fixed.
	 */
	Nchecks	= 291,

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

/*
 * D16's observable: the engine's last act before the Store's memory
 * goes.  What the shutdown's waits are worth is read here and nowhere
 * else — the hook runs inside storeclose, so a job still held or a
 * timer still reading the context at this point is one that outlived
 * the store it walks, and the context itself a moment later.
 */
static int freedseen;
static Srvctx *freedctx;
static int freedjobs;
static int freedheld;
static int freedlive;
static int freedscrub;

static void
onfreed(void*)
{
	freedseen++;
	freedjobs = freedctx != nil && srvstopping(freedctx);
	freedheld = freedctx != nil ? srvjobcount(freedctx) : -1;
	freedlive = freedctx != nil ? srvreclaimlive(freedctx) : -1;
	freedscrub = freedctx != nil ? srvscrublive(freedctx) : -1;
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

/*
 * One numeric field of /status, read on a fresh fid.  ~0 says the file
 * would not render or carries no such field, which no caller here can
 * mistake for a value: every field this is asked for counts something
 * far short of it.
 */
static uvlong
statusnum(Cl *cl, char *attr)
{
	char buf[8192], val[64];

	if(slurpfile(cl, Ffile, "status", buf, sizeof buf) <= 0)
		return ~0ULL;
	if(clfield(buf, attr, val, sizeof val) == nil)
		return ~0ULL;
	return strtoull(val, nil, 10);
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
		{"reclaim",			nil},
		{"reclaim start",		nil},
		{"reclaim stop",		nil},
		{"reclaim go",			"bad ctl"},
		{"reclaim start stop",		"bad ctl"},
		{"reclaim rate=1",		"bad ctl"},	/* not its grammar */
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
		"reclaim start",	/* it will discard, so it is fenced */
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
		"reclaim stop",		/* it mutates nothing: §14(39) */
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
	char what[128], idline[16+Iidlen], *m;
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
	/*
	 * `forget's argument is an instance id (§3.3: a node name, a dot
	 * and the index), and the longest one is well formed: a peer this
	 * store has no record for is not an error, so the bound is the
	 * id's own width and not that of the `peer' field a dirty record
	 * carries, which is two digits narrower.
	 */
	strcpy(idline, "forget ");
	memset(idline+7, 'n', Iidlen);
	idline[7+Nodelen] = '.';
	idline[7+Iidlen] = 0;
	clwrite(&cl, Fctl, 0, idline, &r);
	checks++;
	if(r.type != Rwrite)
		fail("forget of an id of the full instance-id width: %s",
			clerr(&r));
	idline[7+Iidlen] = '0';
	idline[7+Iidlen+1] = 0;
	clwrite(&cl, Fctl, 0, idline, &r);
	clerris("forget of an id one character past it", &r, "bad ctl");

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
	else
		fail("/status is empty before the verb");

	/* the value already pinned: the publish is a no-op and succeeds */
	snprint(line, sizeof line, "newmonid %s", Tmonid);
	if(clwrite(&cl, Fctl, 0, line, &r) != Rwrite)
		fail("newmonid of the pinned value: %s", clerr(&r));
	if(slurpfile(&cl, Ffile, "status", buf, sizeof buf) > 0)
		eqs("/status monid= is unchanged by it",
			clfield(buf, "monid", val, sizeof val), Tmonid);
	else
		fail("/status is empty after the no-op publish");

	/* a different one is what the engine has no call for */
	snprint(line, sizeof line, "newmonid %s", newid);
	clwrite(&cl, Fctl, 0, line, &r);
	clerris("newmonid of a different value", &r,
		"shoalsrv: monitor identity already pinned");
	if(slurpfile(&cl, Ffile, "status", buf, sizeof buf) > 0)
		eqs("/status monid= is unchanged by the refusal",
			clfield(buf, "monid", val, sizeof val), Tmonid);
	else
		fail("/status is empty after the refusal");
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

/* every live object was listed exactly once across the reads */
static void
wholedir(char *what, char *buf, long n, int want)
{
	char names[64][Oidmax+1];
	int i, k, nent;

	nent = dirnames(buf, n, names, nelem(names));
	eqv(what, nent, want);
	for(i = 0; i < nent; i++)
		for(k = i+1; k < nent; k++)
			if(strcmp(names[i], names[k]) == 0){
				fail("%s: the listing repeats %s", what,
					names[i]);
				return;
			}
}

/*
 * Two ways a directory read can stop short of the snapshot's end, and
 * what the cursor owes the read after it.
 *
 * The client's count is the first: an entry that will not fit in what
 * was asked for is not listed, and layer-a §2.2 has it listed next
 * time rather than skipped — so the cursor stands on it and not past
 * it, and a whole listing read in counts that end mid-entry is the
 * same listing as one read in a single count.
 *
 * An `objsnapent' that refuses is the second (store.md §9: a store
 * that has stopped serving answers nothing).  The read answers an
 * error, lib9p leaves `Fid.diroffset' where it was, and the client
 * asks again at the offset the failed read started at — so the
 * position must be back there too.  §13's `slotfail' point is what
 * refuses one such read with the store under it healthy; nothing else
 * can, since the engine's own way of refusing is to stop serving,
 * which refuses the retry as well.
 */
static void
tdircursor(void)
{
	char names[64][Oidmax+1];
	char buf[16*1024], name[32], *m;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	char *w[1];
	vlong off;
	long n;
	int i, k;

	clstage = "dircursor";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	for(i = 0; i < 20; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		mkobj(st, name, nil, 0, 1);
	}
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	w[0] = "obj";

	/* the whole listing in counts small enough to end mid-entry */
	if(clopenpath(&cl, Froot, Fdir, 1, w, OREAD, &r) != Ropen){
		fail("open /obj: %s", clerr(&r));
		goto Out;
	}
	n = 0;
	off = 0;
	for(i = 0; i < 64; i++){
		if(clread(&cl, Fdir, off, 200, &r) != Rread){
			fail("a bounded read: %s", clerr(&r));
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
	wholedir("an entry that did not fit is listed by the next read",
		buf, n, 20);
	clclunk(&cl, Fdir, &r);

	/* a read refused part-way, and the client's retry at that offset */
	if(clopenpath(&cl, Froot, Fdir, 1, w, OREAD, &r) != Ropen){
		fail("re-open /obj: %s", clerr(&r));
		goto Out;
	}
	if(clread(&cl, Fdir, 0, 200, &r) != Rread){
		fail("first read: %s", clerr(&r));
		goto Out;
	}
	n = r.count;
	memmove(buf, r.data, n);
	k = dirnames(buf, n, names, nelem(names));
	istrue("a bounded read stops part-way through the snapshot",
		k > 0 && k+2 < 20);
	off = n;
	srvhook(ctx, "slotfail", k+3);	/* the read of position k+2 refuses */
	clread(&cl, Fdir, off, 4096, &r);
	checks++;
	if(r.type != Rerror)
		fail("a read whose entry read was refused: type %d", r.type);
	srvhook(ctx, "slotfail", 0);
	for(i = 0; i < 64; i++){
		if(clread(&cl, Fdir, off, 4096, &r) != Rread){
			fail("the retry at the same offset: %s", clerr(&r));
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
	wholedir("a refused read drops none of the entries it consumed",
		buf, n, 20);
	clclunk(&cl, Fdir, &r);
Out:
	srvhook(ctx, "slotfail", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * store.md §14(33)'s rewind branch, which nothing else reaches.
 *
 * lib9p refuses a directory read at an offset that is neither 0 nor
 * where the fid left off, against its own `Fid.diroffset', before this
 * row's read cell is reached — so the cursor's own refusal is behind
 * that guard, and the one place the two can disagree is a read this
 * server answered `interrupted' after it had advanced the cursor.  9P
 * has the client discard that reply and ask again from where it was,
 * and the cursor keeps the offset the previous read started at for
 * exactly this.
 *
 * §13's `objexit' point is what puts a read there: it holds the read
 * at the far end of its handler, with the cursor already committed,
 * so the Tflush lands after the advance rather than racing it.
 */
static void
tdirflush(void)
{
	char names[64][Oidmax+1];
	char buf[16*1024], name[32], *m;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall t, r;
	char *w[1];
	ushort ta, tf;
	vlong off;
	long n;
	int i, k;

	clstage = "dirflush";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	for(i = 0; i < 20; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		mkobj(st, name, nil, 0, 1);
	}
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
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
	istrue("a bounded read stops part-way", k > 0 && k < 20);
	off = n;

	/* a read held at its exit, with the cursor advanced, then flushed */
	srvhook(ctx, "objexit", 1);
	memset(&t, 0, sizeof t);
	t.type = Tread;
	t.tag = ta = cltag(&cl);
	t.fid = Fdir;
	t.offset = off;
	t.count = 4096;
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
		fail("a directory read flushed at its exit: type %d %s",
			r.type, r.type == Rerror ? r.ename : "");
	clget(&cl, &r);
	checks++;
	if(r.type != Rflush || r.tag != tf)
		fail("the Rflush after it: type %d tag %ud", r.type, r.tag);
	cltagfree(&cl, ta);
	cltagfree(&cl, tf);
	srvhook(ctx, "objexit", 0);

	/* the client asks again where it was: the cursor rewinds to it */
	for(i = 0; i < 64; i++){
		if(clread(&cl, Fdir, off, 4096, &r) != Rread){
			fail("the read after the flushed one: %s", clerr(&r));
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
	wholedir("a flushed read leaves the listing whole and unrepeated",
		buf, n, 20);
	clclunk(&cl, Fdir, &r);
Out:
	srvhook(ctx, "objexit", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The service loop does not wait for a directory read.
 *
 * A Tflush of a request that is still QUEUED has step 7 performed on
 * the service loop (srv/queue.c), and step 7 takes the flushed fid's
 * state lock.  The directory read works over that same fid's state —
 * a whole listing's worth of objsnapent calls, each taking the
 * engine's lock — and if it held the fid's lock across that walk the
 * loop would block behind it for the length of the walk, which dat.h
 * forbids: nothing a queue proc holds may stop the loop.  So the read
 * lifts the snapshot and the cursor under the lock, walks unlocked,
 * and retakes the lock to commit.
 *
 * §13's `dirhold' point parks the read inside that walk, with no lock
 * of the fid's held.  A second read on the same fid then waits on the
 * reserved queue — one proc — and flushing THAT one is the loop-side
 * step 7 this case is about.  The reply order is the answer: the
 * flushed sibling and its Rflush first, the held read last.
 *
 * The rescue proc is what keeps a failure a failure rather than a
 * wedged program: a loop stalled behind the walk clears nothing, so
 * the point is cleared from a proc of its own and the three replies
 * still arrive — in the other order.
 */
static Srvctx *dirrctx;

static void
dirrescue(void*)
{
	sleep(1500);
	srvhook(dirrctx, "dirhold", 0);
	threadexits(nil);
}

static void
tdirstall(void)
{
	char name[32], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	char *w[1];
	ushort ta, tb, tf, tg[3];
	int i, ty[3];

	clstage = "dirstall";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	for(i = 0; i < 20; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		mkobj(srvstore(ctx), name, nil, 0, 1);
	}
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	w[0] = "obj";
	if(clopenpath(&cl, Froot, Fdir, 1, w, OREAD, &r) != Ropen){
		fail("open /obj: %s", clerr(&r));
		goto Out;
	}

	srvhook(ctx, "dirhold", 3);	/* held before the third entry */
	memset(&t, 0, sizeof t);
	t.type = Tread;
	t.tag = ta = cltag(&cl);
	t.fid = Fdir;
	t.offset = 0;
	t.count = 4096;
	clput(&cl, &t);
	sleep(200);			/* it is now held inside the walk */
	t.tag = tb = cltag(&cl);
	clput(&cl, &t);
	sleep(200);			/* ... and this one is queued behind it */

	dirrctx = ctx;
	if(proccreate(dirrescue, nil, 8192) < 0)
		sysfatal("proccreate: %r");
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = tb;
	clput(&cl, &t);
	for(i = 0; i < 3; i++){
		if(clget(&cl, &r) < 0){
			fail("only %d of the three replies arrived", i);
			goto Out;
		}
		tg[i] = r.tag;
		ty[i] = r.type;
		cltagfree(&cl, r.tag);
	}
	istrue("the flushed sibling is answered while the read is held",
		tg[0] == tb && ty[0] == Rerror);
	istrue("and its Rflush comes after it",
		tg[1] == tf && ty[1] == Rflush);
	istrue("the held read is the last of the three",
		tg[2] == ta && ty[2] == Rread);
	clclunk(&cl, Fdir, &r);
Out:
	srvhook(ctx, "dirhold", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * A flushed directory OPEN holds nothing back.
 *
 * The open takes one of store.md §9's `objsnapmax' snapshots and
 * installs it on the fid, and then leaves through srvqdone, which may
 * answer `interrupted' (layer-a §5.4.1 step 7).  lib9p does not run
 * its `ropen' on an error, so the fid never opens — and a snapshot
 * left on it would be a slot no clunk this client makes gives back,
 * which a client that flushes opens could spend the bound with.  Step
 * 7 is where it goes back; `/status's `objsnapopen=' is where that
 * shows, and the bound itself is the other half: with room for two,
 * two opens must still find it after a flushed one.
 */
static void
tdiropenflush(void)
{
	char buf[8192], val[64], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	char *w[1];
	ushort ta, tf;

	clstage = "diropenflush";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 2)) == nil)	/* objsnapmax 2 */
		return;
	mkobj(srvstore(ctx), "alpha", nil, 0, 1);
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	w[0] = "obj";
	if(clwalk(&cl, Froot, Fdir, 1, w, &r) != Rwalk){
		fail("walk to /obj: %s", clerr(&r));
		goto Out;
	}
	srvhook(ctx, "objexit", 1);
	memset(&t, 0, sizeof t);
	t.type = Topen;
	t.tag = ta = cltag(&cl);
	t.fid = Fdir;
	t.mode = OREAD;
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
		fail("a directory open flushed at its exit: type %d %s",
			r.type, r.type == Rerror ? r.ename : "");
	clget(&cl, &r);
	checks++;
	if(r.type != Rflush || r.tag != tf)
		fail("the Rflush after it: type %d tag %ud", r.type, r.tag);
	cltagfree(&cl, ta);
	cltagfree(&cl, tf);
	srvhook(ctx, "objexit", 0);

	if(slurpfile(&cl, Ffile, "status", buf, sizeof buf) > 0)
		eqs("a flushed open leaves no snapshot on the fid",
			clfield(buf, "objsnapopen", val, sizeof val), "0");
	else
		fail("/status did not render after a flushed open");
	/* and the bound is whole: two opens still find room */
	if(clopenpath(&cl, Froot, Fdir2, 1, w, OREAD, &r) != Ropen)
		fail("the first open after a flushed one: %s", clerr(&r));
	w[0] = "meta";
	if(clopenpath(&cl, Froot, Fdir3, 1, w, OREAD, &r) != Ropen)
		fail("the second open after a flushed one: %s", clerr(&r));
	clclunk(&cl, Fdir2, &r);
	clclunk(&cl, Fdir3, &r);
	clclunk(&cl, Fdir, &r);
Out:
	srvhook(ctx, "objexit", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * A flushed SECOND Topen leaves the FIRST open's snapshot alone.
 *
 * Two Topens can be outstanding on one /obj fid: lib9p refuses the
 * second from Fid.omode, which its `ropen' sets only once the first
 * has answered, and this row's open is offloaded to a queue.  The
 * second waits on that queue — one proc — so flushing it is the
 * loop-side step 7, which calls this fid's flush hook with a request
 * that installed nothing.  A hook keyed to the message type alone
 * would give the FIRST open's snapshot back there, and the fid would
 * open with nothing on it: every Tread answering `not built'.
 *
 * The listing read afterwards is what says the snapshot survived.
 */
static void
tdiropen2(void)
{
	char buf[16*1024], name[32], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	char *w[1];
	vlong off;
	long n;
	ushort ta, tb, tf;
	int i;

	clstage = "diropen2";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	for(i = 0; i < 10; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		mkobj(srvstore(ctx), name, nil, 0, 1);
	}
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	w[0] = "obj";
	if(clwalk(&cl, Froot, Fdir, 1, w, &r) != Rwalk){
		fail("walk to /obj: %s", clerr(&r));
		goto Out;
	}

	/* the first open, held at its exit with its snapshot installed */
	srvhook(ctx, "objexit", 1);
	memset(&t, 0, sizeof t);
	t.type = Topen;
	t.tag = ta = cltag(&cl);
	t.fid = Fdir;
	t.mode = OREAD;
	clput(&cl, &t);
	sleep(200);			/* it is now held at its exit */
	t.tag = tb = cltag(&cl);
	clput(&cl, &t);
	sleep(200);			/* ... and the second is queued behind it */

	/* the second is flushed while it is still queued: step 7 on the loop */
	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tf = cltag(&cl);
	t.oldtag = tb;
	clput(&cl, &t);
	checks++;
	if(clgettag(&cl, tb, &r) != Rerror
	|| strcmp(r.ename, "interrupted") != 0)
		fail("the second open, flushed while queued: type %d %s",
			r.type, clerr(&r));
	cltagfree(&cl, tb);
	checks++;
	if(clget(&cl, &r) != Rflush || r.tag != tf)
		fail("the Rflush after it: type %d tag %ud", r.type, r.tag);
	cltagfree(&cl, tf);
	srvhook(ctx, "objexit", 0);
	checks++;
	if(clgettag(&cl, ta, &r) != Ropen)
		fail("the first open: %s", clerr(&r));
	cltagfree(&cl, ta);

	/* the first open's snapshot is still there, and still lists */
	n = 0;
	off = 0;
	for(i = 0; i < 64; i++){
		if(clread(&cl, Fdir, off, 4096, &r) != Rread){
			fail("the listing after the flushed second open: %s",
				clerr(&r));
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
	wholedir("the first open's listing is whole", buf, n, 10);
	clclunk(&cl, Fdir, &r);
Out:
	srvhook(ctx, "objexit", 0);
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
	else
		fail("/status is empty with two snapshots open");
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
 *
 * `err=' is the one field whose value is not a word: the string a
 * pass gave up with may hold spaces and slashes, which is why
 * store.md §14(30) puts it last on the line.  So it runs to the end
 * of the line, and every other field stops at the first space — or at
 * the `/' that separates `done='s pair, whose numerator is what a
 * caller asking for `done' wants.
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
	if(strcmp(attr, "err") == 0)
		for(e = p; *e != 0 && *e != '\n'; e++)
			;
	else
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

/* poll /jobs until a parked pass shows an err=, or give up */
static int
joberred(Cl *cl, char *val, int nval)
{
	char buf[4096];
	int i;

	for(i = 0; i < 400; i++){
		if(slurpfile(cl, Ffile2, "jobs", buf, sizeof buf) > 0
		&& strstr(buf, "err=") != nil)
			return jobfield(cl, "err", val, nval) != nil;
		sleep(20);
	}
	return 0;
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
	else
		fail("/status is empty after a pass that found damage");

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
 * /lost's other kind of line: store.md §5 step 10's condemned slot,
 * which layer-a §2.2's `oid=' cannot name because the index entry
 * that would have carried an oid IS the damage (store.md §14(15)).
 *
 * The damage is made where the engine will find it — an index entry
 * on the freshly formatted platter, whose checksum then fails to
 * unpack — and it is found at start-up, since §5 step 4 reads the
 * index region tolerantly and step 10 condemns what replay did not
 * restore.  The slot damaged is one no object is in: a slot an
 * object's record names is restored by the replay of that record,
 * which is the whole of step 10's "did not restore".
 */
static void
tlostslot(void)
{
	char buf[8192], val[64], line[64], *m;
	uchar sec[Idxentsz];
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	Super sb;
	Sbsel sel;
	vlong off;
	ulong slot;

	clstage = "lostslot";
	m = mkmap();
	d = newdisk();
	slot = 100;			/* within the 128 slots fmtcfg asks for */
	if(superselect(d, &sel) < 0 || sel.start < 0){
		fail("superselect: %r");
		devclose(d);
		free(m);
		return;
	}
	sb = sel.sb[sel.start];
	off = (vlong)sb.idxoff * sb.secsz + (vlong)slot * Idxentsz;
	simpeek(d, off, sec, Idxentsz);
	sec[0] ^= 0xff;			/* the entry no longer unpacks */
	simpoke(d, off, sec, Idxentsz);

	if((ctx = startsrv(d, m, 4, 0)) == nil){
		devclose(d);
		free(m);
		return;
	}
	mkobj(srvstore(ctx), "alpha", nil, 0, 1);
	clstart(&cl, ctx, Clmsize);
	if(clattach(&cl, Froot, "role=admin", &r) != Rattach){
		fail("attach: %s", clerr(&r));
		goto Out;
	}
	snprint(line, sizeof line, "slot=%lud kind=lost", slot);
	if(slurpfile(&cl, Ffile, "lost", buf, sizeof buf) > 0){
		istrue("/lost names the condemned slot and gives it no oid",
			hasline(buf, line));
		eqv("and the line carries no oid=", nlines(buf, "oid="), 0);
		eqv("/lost has that one line", nlines(buf, "slot="), 1);
	}else
		fail("/lost is empty with a condemned slot");
	if(slurpfile(&cl, Ffile, "status", buf, sizeof buf) > 0)
		eqs("/status lost= counts the condemned slot",
			clfield(buf, "lost", val, sizeof val), "1");
	else
		fail("/status did not render");
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
	uvlong done, np0, np, nd;
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
	srvcount(ctx, &np0, &nd);
	srvhook(ctx, "objhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Fctl;
	t.offset = 0;
	t.data = "verify obj04";
	t.count = strlen(t.data);
	clput(&cl, &t);
	/*
	 * The pass must not start until that queue is occupied, and what
	 * says so is the pool's own push count rather than a wait long
	 * enough to be probably true: srvqpush counts a request as it
	 * hands it to the queue, so one more push than there were is this
	 * write and nothing else.  Nothing else is in flight here.
	 */
	np = np0;
	for(i = 0; i < 400 && np == np0; i++){
		sleep(5);
		srvcount(ctx, &np, &nd);
	}
	eqv("the held verify reached its queue", np - np0, 1);

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
 * store.md §9's tombstone reclaim walk, which §2.5's `reclaim start'
 * drives and which discards NOTHING.
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
	if(clwrite(&cl, Fctl, 0, "reclaim start", &r) != Rwrite)
		fail("reclaim start: %s", clerr(&r));
	if(jobparked(&cl, "reclaimable", val, sizeof val) == nil)
		fail("the pass never reached its hold");
	else
		eqs("a tombstone younger than tombdays is not reclaimable",
			val, "0");
	if(slurpfile(&cl, Ffile2, "jobs", buf, sizeof buf) > 0)
		istrue("the walk is a job of its own at /jobs",
			haspfx(buf, "job=reclaim "));
	else
		fail("the parked reclaim pass left no /jobs line");
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
	if(clwrite(&cl, Fctl, 0, "reclaim start", &r) != Rwrite)
		fail("reclaim start: %s", clerr(&r));
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

	/*
	 * The same walk cut short: a prefix of the snapshot counted, with
	 * an `err=' beside it saying so.  §13's `reclaimhold' is what
	 * stops the walk part-way — it is paced by nothing and asks no
	 * queue, so nothing else can hold it still long enough for the
	 * case to write the verb.
	 */
	srvhook(ctx, "reclaimhold", 2);		/* held before the 2nd entry */
	srvhook(ctx, "jobhold", 1);
	if(clwrite(&cl, Fctl, 0, "reclaim start", &r) != Rwrite)
		fail("a third reclaim start: %s", clerr(&r));
	for(i = 0; i < 500; i++){
		if(jobfield(&cl, "reclaimable", val, sizeof val) != nil
		&& strcmp(val, "1") == 0)
			break;
		sleep(20);
	}
	istrue("the reclaim walk counted an entry and parked", i < 500);
	if(clwrite(&cl, Fctl, 0, "reclaim stop", &r) != Rwrite)
		fail("reclaim stop over the walk: %s", clerr(&r));
	srvhook(ctx, "reclaimhold", 0);
	if(!joberred(&cl, val, sizeof val))
		fail("a reclaim walk stopped part-way reported nothing");
	else
		eqs("a reclaim walk that did not finish says so", val,
			"shoalsrv: stopped");
	if(jobfield(&cl, "reclaimable", val, sizeof val) == nil)
		fail("the stopped pass left no /jobs line");
	else
		eqs("and the count it did reach stands beside it", val, "1");
	/*
	 * `done=' and `total=' are this walk's own entries, so the prefix
	 * shows there as well: one of the snapshot's two entries counted
	 * (store.md §14(31)).  It is the mark and not this pair that says
	 * the walk is over, since a walk still running reads the same.
	 */
	if(slurpfile(&cl, Ffile2, "jobs", buf, sizeof buf) > 0)
		istrue("and `done=' is a prefix of the snapshot too",
			strstr(buf, "done=1/2 ") != nil);
	else
		fail("the stopped pass left no line to read done= from");
Out2:
	srvhook(ctx, "reclaimhold", 0);
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The walk's own timer (store.md §14(39)), which is what runs it on an
 * instance nobody writes a verb to.
 *
 * The period is `tombdays'/2 and the shortest a map can ask for is
 * half a day, so srv.h's `srvreclaimms' knob is what a T1 can drive:
 * set to a few tens of milliseconds, the next tick starts the same
 * pass the verb starts.  No `reclaim' is written in this case at all —
 * the /jobs line it reads is the timer's.  §13's `jobhold' keeps that
 * pass listed once its walk is over, as it does for a verb's, and the
 * knob goes back before the hold is cleared so that a second tick does
 * not start a pass behind the case.
 */
static void
treclaimtimer(void)
{
	char buf[8192], val[64], *m;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	int i;

	clstage = "reclaimtimer";
	m = mkmapd(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid, 0);
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	mkobj(st, "old", nil, 0, 1);
	rmobj(st, "old", 9, 3);			/* past both local cutoffs */
	sleep(1100);				/* past the cutoff's second */
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	istrue("no pass runs before the timer fires", !jobrunning(&cl));
	/*
	 * The period this map gets is the FLOOR (store.md §14(39)):
	 * `tombdays=0' halves to no period at all, and a timer running on
	 * that would walk the snapshot for as long as the instance
	 * served.  Twelve hours is longer than a test can wait for, so it
	 * is read rather than waited out.
	 */
	eqv("a map that retains nothing gets the floor, not no period",
		srvreclaimperiod(ctx), 12*3600*1000);
	srvhook(ctx, "jobhold", 1);
	srvreclaimms(ctx, 50);
	eqv("and the knob is what overrides it", srvreclaimperiod(ctx), 50);
	if(jobparked(&cl, "reclaimable", val, sizeof val) == nil)
		fail("no pass reached the hold: the timer never fired");
	else
		eqs("the pass the timer started counts what the verb's "
			"would", val, "1");
	if(slurpfile(&cl, Ffile2, "jobs", buf, sizeof buf) > 0){
		istrue("and it is a reclaim job", haspfx(buf, "job=reclaim "));
		eqv("the ticks behind it started no second walk",
			nlines(buf, "job="), 1);
	}else
		fail("the parked pass left no /jobs line");
	srvreclaimms(ctx, 0);
Out:
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * `reclaim [start|stop]' as a grammar with effects: what a second
 * `start' does to a running walk, what `stop' does to one, and what a
 * `start' over a walk that is stopping or over a full job list is
 * answered.  They are `scrub's three answers (store.md §14(31)) over
 * this verb's own flag.
 *
 * §13's `reclaimhold' is what holds a walk still while the case writes
 * the next verb: the walk asks no queue and is paced by nothing, so
 * without it a walk over any index a test can build is over before the
 * second write lands.  It parks before the walk's first entry here,
 * which needs an index that has one.
 */
static void
treclaimctl(void)
{
	char buf[16*1024], name[32], line[64], *m;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	int i;

	clstage = "reclaimctl";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	for(i = 0; i < 8; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		mkobj(st, name, nil, 0, 1);
		rmobj(st, name, 9, 3);
	}
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;

	eqv("a map's own period is half its tombdays",
		srvreclaimperiod(ctx), (uvlong)7*86400000/2);

	/* the form with neither word starts nothing and succeeds */
	if(clwrite(&cl, Fctl, 0, "reclaim", &r) != Rwrite)
		fail("bare reclaim: %s", clerr(&r));
	istrue("a bare reclaim starts no walk", !jobrunning(&cl));

	srvhook(ctx, "reclaimhold", 1);		/* held before the 1st entry */
	if(clwrite(&cl, Fctl, 0, "reclaim start", &r) != Rwrite)
		fail("reclaim start: %s", clerr(&r));
	for(i = 0; i < 400 && !jobrunning(&cl); i++)
		sleep(5);
	if(slurpfile(&cl, Ffile, "jobs", buf, sizeof buf) > 0)
		eqv("a walk is listed at /jobs", nlines(buf, "job=reclaim"), 1);
	else
		fail("/jobs is empty with a walk started");
	if(clwrite(&cl, Fctl, 0, "reclaim start", &r) != Rwrite)
		fail("a second reclaim start: %s", clerr(&r));
	if(slurpfile(&cl, Ffile, "jobs", buf, sizeof buf) > 0)
		eqv("a second start puts no second walk over one index",
			nlines(buf, "job="), 1);
	else
		fail("/jobs is empty after a second reclaim start");

	/* a `start' over a walk that has been told to stop is refused */
	if(clwrite(&cl, Fctl, 0, "reclaim stop", &r) != Rwrite)
		fail("reclaim stop over a held walk: %s", clerr(&r));
	clwrite(&cl, Fctl, 0, "reclaim start", &r);
	clerris("a reclaim start while a walk is stopping", &r,
		"shoalsrv: reclaim stopping");
	srvhook(ctx, "reclaimhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	istrue("a walk stops when it is told to", !jobrunning(&cl));

	/* and the refusal latched nothing: this one really starts */
	srvhook(ctx, "reclaimhold", 1);
	if(clwrite(&cl, Fctl, 0, "reclaim start", &r) != Rwrite)
		fail("a reclaim start after a stopping one: %s", clerr(&r));
	for(i = 0; i < 400 && !jobrunning(&cl); i++)
		sleep(5);
	istrue("a reclaim start after a stopping one starts a walk",
		jobrunning(&cl));
	if(clwrite(&cl, Fctl, 0, "reclaim stop", &r) != Rwrite)
		fail("reclaim stop after it: %s", clerr(&r));
	srvhook(ctx, "reclaimhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);

	/*
	 * The job cap refuses a walk like any other pass, and the flag it
	 * raised before asking for the job has to go back with the
	 * refusal: a flag left raised makes every later start answer
	 * success and start nothing, and stops the timer with it.
	 */
	srvhook(ctx, "jobhold", 1);
	for(i = 0; i < 12; i++){
		snprint(line, sizeof line, "forget peer%.2d", i);
		if(clwrite(&cl, Fctl, 0, line, &r) != Rwrite)
			fail("%#q: %s", line, clerr(&r));
	}
	clwrite(&cl, Fctl, 0, "reclaim start", &r);
	clerris("a reclaim start with no job to be had", &r,
		"shoalsrv: too many jobs");
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	srvhook(ctx, "reclaimhold", 1);
	if(clwrite(&cl, Fctl, 0, "reclaim start", &r) != Rwrite)
		fail("a reclaim start after a refused one: %s", clerr(&r));
	for(i = 0; i < 400 && !jobrunning(&cl); i++)
		sleep(5);
	if(slurpfile(&cl, Ffile, "jobs", buf, sizeof buf) > 0)
		eqv("a reclaim start after a refused one starts a walk",
			nlines(buf, "job=reclaim"), 1);
	else
		fail("a reclaim start after a refused one started nothing");
	if(clwrite(&cl, Fctl, 0, "reclaim stop", &r) != Rwrite)
		fail("final reclaim stop: %s", clerr(&r));
Out:
	srvhook(ctx, "reclaimhold", 0);
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * A `reclaim start' written while a tick is in flight that can start
 * nothing.  The verb owes the answer the pass itself would be given:
 * the job cap (store.md §14(30)) is what refuses both, and a tick
 * refused it raises no flag, so the write meets the cap rather than a
 * flag standing for a pass that never started.  Answering success
 * there loses the pass outright — nothing runs, no line at /jobs, and
 * the next chance is a period away.
 *
 * §13's `tickhold' is set here for what it would do if the flag were
 * raised ahead of the admission: it parks the timer's own call between
 * the decision and the proc, widening the window the verb's write has
 * to land in.  With the flag raised inside the admission instead, a
 * refused tick never reaches the point — the twelve `forget' passes
 * parked at `jobhold' leave no job to be had, so the tick is turned
 * back before the hold — and the case is what keeps it that way.
 * `srvreclaimms' is what makes the timer tick inside a test at all
 * (srv.h).
 */
static void
treclaimrace(void)
{
	char buf[16*1024], line[64], *m;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	int i;

	clstage = "reclaimrace";
	m = mkmapd(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid, 0);
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	mkobj(st, "old", nil, 0, 1);
	rmobj(st, "old", 9, 3);			/* past both local cutoffs */
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	srvhook(ctx, "jobhold", 1);
	for(i = 0; i < 12; i++){
		snprint(line, sizeof line, "forget peer%.2d", i);
		if(clwrite(&cl, Fctl, 0, line, &r) != Rwrite)
			fail("%#q: %s", line, clerr(&r));
	}
	clwrite(&cl, Fctl, 0, "reclaim start", &r);
	clerris("a reclaim start with the job list full", &r,
		"shoalsrv: too many jobs");

	/* the same write, with a tick of the timer's inside the window */
	srvhook(ctx, "tickhold", 1);
	srvreclaimms(ctx, 30);
	sleep(1200);
	clwrite(&cl, Fctl, 0, "reclaim start", &r);
	clerris("a reclaim start against a tick that can start nothing",
		&r, "shoalsrv: too many jobs");
	if(slurpfile(&cl, Ffile, "jobs", buf, sizeof buf) > 0)
		eqv("and neither of them left a walk at /jobs",
			nlines(buf, "job=reclaim"), 0);
	else
		fail("/jobs is empty with twelve passes parked");
	srvreclaimms(ctx, 0);
	srvhook(ctx, "tickhold", 0);
Out:
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * D16's shutdown over a reclaim walk parked at §13's `reclaimhold'.
 *
 * That point holds a pass proc, so it carries `jobhold's hazard
 * (srv.h): the pass holds one of the jobs the shutdown waits for, and
 * the wait is unbounded because store.md §9 forbids closing the store
 * while a pass is inside the engine.  What keeps it from wedging is
 * srvholdclear, which the shutdown runs before it drains — the walk
 * wakes, reads srvstopping between its entries, and gives the job
 * back.  The case also leaves the timer's proc to that same shutdown,
 * which waits for it separately.
 */
static void
treclaimdown(void)
{
	char name[32], *m;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	int i;

	clstage = "reclaimdown";
	m = mkmapd(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid, 0);
	d = newdisk();
	freedseen = 0;
	freedheld = -1;
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	for(i = 0; i < 8; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		mkobj(st, name, nil, 0, 1);
		rmobj(st, name, 9, 3);
	}
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	srvhook(ctx, "reclaimhold", 1);
	if(clwrite(&cl, Fctl, 0, "reclaim start", &r) != Rwrite){
		fail("reclaim start: %s", clerr(&r));
		goto Out;
	}
	for(i = 0; i < 400 && !jobrunning(&cl); i++)
		sleep(5);
	istrue("the walk is listed at /jobs", jobrunning(&cl));
	eqv("the parked walk holds a job", srvjobcount(ctx), 1);
Out:
	clstop(&cl);			/* the loop ends; the shutdown runs */
	eqv("the store was closed once", freedseen, 1);
	eqv("the parked walk had given its job back by then", freedheld, 0);
	eqv("no job is left held", srvjobcount(ctx), 0);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * The same shutdown against the TIMER, which is the one proc of this
 * unit that holds no job: srvshutdown waits for it apart from the jobs
 * and before them (srv.h), and what that wait is worth is read at the
 * moment the store closes.
 *
 * The timer is made demonstrably alive across the shutdown rather than
 * assumed to be: the knob puts its period in tens of milliseconds, the
 * case waits until a pass no verb asked for has come and gone, and it
 * asserts the proc is still up with the client about to go.  The
 * period knob then goes back and the SLICE knob goes to seconds, so
 * the shutdown begins with the proc inside a sleep longer than
 * everything srvshutdown does after the wait.  That is what makes `the
 * timer had ended when the store closed' a check about the wait: at
 * the server's own half-second slice the proc would be gone by then
 * whether the shutdown waited for it or not (srv.h).
 */
static void
treclaimwait(void)
{
	char val[64], *m;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	int i;

	clstage = "reclaimwait";
	m = mkmapd(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid, 0);
	d = newdisk();
	freedseen = 0;
	freedlive = -1;
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	mkobj(st, "old", nil, 0, 1);
	rmobj(st, "old", 9, 3);			/* past both local cutoffs */
	sleep(1100);				/* past the cutoff's second */
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	srvhook(ctx, "jobhold", 1);
	srvreclaimms(ctx, 30);
	if(jobparked(&cl, "reclaimable", val, sizeof val) == nil)
		fail("no pass reached the hold: the timer never fired");
	else
		eqs("a pass the timer started counted the tombstone",
			val, "1");
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	istrue("the timer is still up with the pass over",
		srvreclaimlive(ctx) != 0);
	srvreclaimms(ctx, 0);		/* no tick inside the long slice */
	srvreclaimslicems(ctx, 3000);
	/*
	 * Out of the old half-second slice and into a new long one.
	 * Three of the server's own slices and not one, for tscrubwait's
	 * reason: the proc is at most one old slice from waking, but
	 * sleep(2) guarantees only a lower bound, so a 700 ms sleep was
	 * a 200 ms margin against a scheduler that promises none.
	 */
	sleep(1500);
Out:
	clstop(&cl);			/* the loop ends; the shutdown runs */
	eqv("the store was closed once", freedseen, 1);
	eqv("the timer had ended when the store closed", freedlive, 0);
	istrue("and it is not reading the context now either",
		srvreclaimlive(ctx) == 0);
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
	else
		fail("/dirty is empty with three records added");
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
	}else
		fail("/dirty is empty after forgetting one of two peers");
	if(slurpfile(&cl, Ffile, "status", buf, sizeof buf) > 0)
		eqs("/status dirty= falls with them",
			clfield(buf, "dirty", val, sizeof val), "1");
	else
		fail("/status is empty after the forget pass");
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
 * A pass that fails says so — and a scrub counts no tombstones,
 * whether it failed or not.
 *
 * Every error inside a pass used to be swallowed: the walk broke off
 * and the verb had already answered success, so the wire said the
 * index had been scrubbed.
 *
 * Three points drive it.  `slotfail' fails one index read with the
 * store under it healthy, which is what tells a broken-off walk from
 * a walk whose store has gone.  `fatal' condemns the engine outright,
 * which is how a `dirtydel' is made to fail.  The simulated disk's own
 * read fault over the data region is how ONE object is made unreadable
 * with the index and the rest of the store healthy — that pass walks
 * to the end of the index and carries an `err=' all the same.
 * `jobhold' keeps the pass listed long enough to read what it gave up
 * with in every case.
 *
 * The tombstone here is past both of §1.5's local cutoffs, so the
 * whole store's `reclaimable=' is 1: the scrub's line carries 0 all
 * the same, at the end of the case, and the `reclaim' beside it
 * carries 1 over the same index.  That pair is what says the walk no
 * longer rides on the scrub.
 */
static void
tpassfail(void)
{
	char buf[8192], val[ERRMAX], name[32], *m;
	uchar data[4096];
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	Super sb;
	Sbsel sel;
	vlong off, len;
	int i;

	clstage = "passfail";
	m = mkmapd(Tepoch, Tblksz, Tobjmax, "blake2s256", Tuuid, 0);
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	for(i = 0; i < sizeof data; i++)
		data[i] = (uchar)(0x31 + (i & 0x3f));
	for(i = 0; i < 20; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		mkobj(st, name, data, sizeof data, 1);
	}
	rmobj(st, "obj19", 9, 3);	/* past both of §1.5's local cutoffs */
	sleep(1100);			/* past the cutoff's second */
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	srvhook(ctx, "jobhold", 1);

	/* a scrub that breaks off part-way through the index */
	srvhook(ctx, "slotfail", 4);		/* the read of slot 3 fails */
	if(clwrite(&cl, Fctl, 0, "scrub start rate=1000000", &r) != Rwrite){
		fail("scrub start: %s", clerr(&r));
		goto Out;
	}
	if(!joberred(&cl, val, sizeof val))
		fail("a scrub that broke off reported nothing at /jobs");
	else
		eqs("/jobs says what the pass gave up with", val,
			"shoalsrv: index read refused at the point");
	srvhook(ctx, "slotfail", 0);
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);

	/* the same for a forget whose dirtydel fails */
	if(dirtyadd(st, (uchar*)"obj00", 5, "n1.1", 11) < 0)
		fail("dirtyadd: %r");
	srvhook(ctx, "jobhold", 1);
	storehook(st, "fatal", 1);
	if(clwrite(&cl, Fctl, 0, "forget n1.1", &r) != Rwrite)
		fail("forget: %s", clerr(&r));
	if(!joberred(&cl, val, sizeof val))
		fail("a forget that could not discard reported nothing");
	else
		eqs("/jobs says what the forget pass gave up with", val,
			"store condemned: in-memory state no longer matches "
			"the log; open it again");
	storehook(st, "fatal", 0);
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	istrue("the record the failed forget could not discard is still there",
		dirtyhas(st, (uchar*)"obj00", 5, "n1.1"));

	/*
	 * one object the pass cannot read: it says so, and it walks the
	 * rest of the index all the same
	 */
	if(superselect(d, &sel) < 0 || sel.start < 0)
		fail("superselect: %r");
	else{
		sb = sel.sb[sel.start];
		off = (vlong)sb.dataoff * sb.secsz;
		len = (vlong)sb.datasecs * sb.secsz;
		simfaultat(d, Sfeio, 1, off, len);	/* one grain read */
		srvhook(ctx, "jobhold", 1);
		if(clwrite(&cl, Fctl, 0, "scrub start rate=1000000", &r)
			!= Rwrite)
			fail("scrub start over an unreadable object: %s",
				clerr(&r));
		if(jobparked(&cl, "err", val, sizeof val) == nil)
			fail("the pass walked the whole index with no err= "
				"over an object it could not read");
		else
			eqs("/jobs says which object read failed", val,
				"i/o error");
		simfault(d, Sfnone, 0);
		srvhook(ctx, "jobhold", 0);
		for(i = 0; i < 400 && jobrunning(&cl); i++)
			sleep(20);
	}

	/*
	 * A whole scrub over the same index counts no tombstone, and the
	 * reclaim walk over that same index counts the one there is.
	 */
	srvhook(ctx, "jobhold", 1);
	if(clwrite(&cl, Fctl, 0, "scrub start rate=1000000", &r) != Rwrite)
		fail("second scrub start: %s", clerr(&r));
	if(jobparked(&cl, "reclaimable", val, sizeof val) == nil)
		fail("the whole pass never reached its hold");
	else
		eqs("a scrub that completed counts no tombstones", val, "0");
	if(slurpfile(&cl, Ffile2, "jobs", buf, sizeof buf) > 0){
		istrue("and carries no err=", strstr(buf, "err=") == nil);
		eqv("and started no walk of its own",
			nlines(buf, "job=reclaim"), 0);
	}else
		fail("the parked whole pass left no /jobs line");
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	srvhook(ctx, "jobhold", 1);
	if(clwrite(&cl, Fctl, 0, "reclaim start", &r) != Rwrite)
		fail("reclaim start over the same index: %s", clerr(&r));
	if(jobparked(&cl, "reclaimable", val, sizeof val) == nil)
		fail("the reclaim walk never reached its hold");
	else
		eqs("the walk over that same index counts the tombstone",
			val, "1");
Out:
	storehook(srvstore(ctx), "fatal", 0);
	srvhook(ctx, "slotfail", 0);
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * An object that goes away between the index read and the queue is
 * not the pass's failure.
 *
 * The walk reads the index outside every queue, and the unit it
 * pushes is ordered behind whatever that object's queue was already
 * holding — so a drop or a delete landing in that window is the
 * ordering working, and the engine answers the push `no such object'.
 * A pass that recorded that as a failure would block its own reclaim
 * on a client doing nothing wrong.
 *
 * §13's `objhold' point is what opens the window: a client's `verify'
 * parked inside the one object's queue keeps the pass's own unit
 * queued behind it, and the pool's push count says the unit is there
 * — one push for the client's request and one for the pass's, with
 * nothing else in flight.  The object is then dropped through the
 * engine, and the unit runs against an id the store no longer holds.
 */
static void
tobjgone(void)
{
	char buf[4096], val[ERRMAX], *m;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall t, r;
	char *w[1];
	ushort ta;
	uvlong np0, np, nd;
	int i;

	clstage = "objgone";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	mkobj(st, "alpha", nil, 0, 1);		/* the index's only live object */
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	/* a second /ctl, since the first carries the request that parks */
	if(clattach(&cl, Froot2, "role=admin", &r) != Rattach){
		fail("second attach: %s", clerr(&r));
		goto Out;
	}
	w[0] = "ctl";
	if(clopenpath(&cl, Froot2, Ffile, 1, w, OWRITE, &r) != Ropen){
		fail("open the second /ctl: %s", clerr(&r));
		goto Out;
	}

	/* park a client request inside alpha's queue */
	srvcount(ctx, &np0, &nd);
	srvhook(ctx, "objhold", 1);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Fctl;
	t.offset = 0;
	t.data = "verify alpha";
	t.count = strlen(t.data);
	clput(&cl, &t);
	np = np0;
	for(i = 0; i < 400 && np - np0 < 1; i++){
		sleep(5);
		srvcount(ctx, &np, &nd);
	}
	eqv("the held verify reached its queue", np - np0, 1);

	/* the pass queues its unit for that object behind it */
	srvhook(ctx, "jobhold", 1);
	if(clwrite(&cl, Ffile, 0, "scrub start rate=1000000", &r) != Rwrite){
		fail("scrub start: %s", clerr(&r));
		goto Out;
	}
	for(i = 0; i < 400 && np - np0 < 2; i++){
		sleep(5);
		srvcount(ctx, &np, &nd);
	}
	eqv("the pass's unit is queued behind it", np - np0, 2);

	/* and the object goes while the unit waits */
	if(objdrop(st, (uchar*)"alpha", 5) < 0)
		fail("objdrop alpha: %r");
	srvhook(ctx, "objhold", 0);
	clgettag(&cl, ta, &r);			/* the client's own answer */
	cltagfree(&cl, ta);
	if(jobparked(&cl, "skipped", val, sizeof val) == nil)
		fail("the pass never reached its hold");
	else
		eqs("an object gone between the index and the queue is "
			"counted apart", val, "1");
	if(slurpfile(&cl, Ffile2, "jobs", buf, sizeof buf) > 0)
		istrue("and is not the pass's failure",
			strstr(buf, "err=") == nil);
	else
		fail("the parked pass left no /jobs line");
Out:
	srvhook(ctx, "objhold", 0);
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * What `scrub start' and `scrub stop' do to a pass that is running,
 * and what a REFUSED `scrub start' leaves behind.
 *
 * `scrubbing' is raised before jobstart is called, because raising it
 * afterwards would race the pass proc's own clearing of it — so every
 * way jobstart can refuse has to put it back.  It cannot be driven
 * through the shutdown (the loop has ended by then and no ctl write
 * can reach the verb), so the cap of store.md §14(30) is what refuses
 * here: twelve passes parked at §13's jobhold point, and the `scrub
 * start' behind them.  A flag left raised makes every later `scrub
 * start' answer success and start nothing.
 *
 * A `start' over a pass that has been told to stop is the third
 * answer and is refused: the job it asks for is not running and is
 * not going to be.  Driving it needs a pass that cannot wind down
 * while the case writes the verb, which is §13's `objhold' — a
 * client's `verify' parked inside one object's queue, with the pass's
 * own unit for that object queued behind it.  The object is the one
 * at index slot 0, so that it is the first unit the pass pushes and
 * the pool's push count says when the pass is there.
 */
static void
tscrubctl(void)
{
	char buf[16*1024], name[32], line[64], val[64], *m;
	uchar oid[Oidmax];
	Objinfo oi;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall t, r;
	char *w[1];
	ushort ta;
	uvlong np0, np, nd;
	int i, oidlen;

	clstage = "scrubctl";
	m = mkmap();
	d = newdisk();
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
	if(slurpfile(&cl, Ffile, "jobs", buf, sizeof buf) > 0)
		eqv("a pass is listed at /jobs", nlines(buf, "job="), 1);
	else
		fail("/jobs is empty with a pass started");
	if(clwrite(&cl, Fctl, 0, "scrub start", &r) != Rwrite)
		fail("a second scrub start: %s", clerr(&r));
	if(slurpfile(&cl, Ffile, "jobs", buf, sizeof buf) > 0)
		eqv("a second scrub start puts no second pass over one index",
			nlines(buf, "job="), 1);
	else
		fail("/jobs is empty after a second scrub start");

	/* and `scrub stop' is read by the pass between objects */
	if(clwrite(&cl, Fctl, 0, "scrub stop", &r) != Rwrite)
		fail("scrub stop: %s", clerr(&r));
	for(i = 0; i < 150 && jobrunning(&cl); i++)
		sleep(20);
	istrue("a running pass stops when it is told to", !jobrunning(&cl));

	/* a `scrub start' over a pass that has been told to stop */
	if(objslot(srvstore(ctx), 0, oid, &oidlen, &oi) <= 0)
		fail("no object at index slot 0");
	if(clattach(&cl, Froot2, "role=admin", &r) != Rattach)
		fail("second attach: %s", clerr(&r));
	w[0] = "ctl";
	if(clopenpath(&cl, Froot2, Ffile, 1, w, OWRITE, &r) != Ropen)
		fail("open the second /ctl: %s", clerr(&r));
	srvcount(ctx, &np0, &nd);
	srvhook(ctx, "objhold", 1);
	snprint(line, sizeof line, "verify %.*s", oidlen, (char*)oid);
	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = ta = cltag(&cl);
	t.fid = Fctl;
	t.offset = 0;
	t.data = line;
	t.count = strlen(t.data);
	clput(&cl, &t);
	np = np0;
	for(i = 0; i < 400 && np - np0 < 1; i++){
		sleep(5);
		srvcount(ctx, &np, &nd);
	}
	eqv("the held verify reached its queue", np - np0, 1);
	if(clwrite(&cl, Ffile, 0, "scrub start rate=1000000", &r) != Rwrite)
		fail("a scrub start over a held queue: %s", clerr(&r));
	for(i = 0; i < 400 && np - np0 < 2; i++){
		sleep(5);
		srvcount(ctx, &np, &nd);
	}
	eqv("the pass is held inside that object's queue", np - np0, 2);
	if(clwrite(&cl, Ffile, 0, "scrub stop", &r) != Rwrite)
		fail("scrub stop over a held pass: %s", clerr(&r));
	clwrite(&cl, Ffile, 0, "scrub start", &r);
	clerris("a scrub start while a pass is stopping", &r,
		"shoalsrv: scrub stopping");
	srvhook(ctx, "objhold", 0);
	if(clgettag(&cl, ta, &r) != Rwrite)
		fail("the held verify: %s", clerr(&r));
	cltagfree(&cl, ta);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	istrue("the stopping pass ended", !jobrunning(&cl));
	/* and the refusal latched nothing: this one really starts */
	if(clwrite(&cl, Ffile, 0, "scrub start rate=1", &r) != Rwrite)
		fail("a scrub start after a stopping one: %s", clerr(&r));
	istrue("a scrub start after a stopping one starts a pass",
		jobrunning(&cl));
	if(clwrite(&cl, Ffile, 0, "scrub stop", &r) != Rwrite)
		fail("scrub stop after it: %s", clerr(&r));
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	clclunk(&cl, Ffile, &r);
	clclunk(&cl, Froot2, &r);

	/*
	 * A pass parked at the END of its run, with `scrub stop' raised
	 * over it.  The pass has walked its index and is held at §13's
	 * jobhold point with its record still listed, so the job the next
	 * `scrub start' asks for is neither running nor stopping: it is
	 * over.  jobproc gives `scrubbing' back before it parks for
	 * exactly that, and a `start' refused `scrub stopping' here would
	 * be naming a pass that had already finished.
	 */
	srvhook(ctx, "jobhold", 1);
	if(clwrite(&cl, Fctl, 0, "scrub start rate=1000000", &r) != Rwrite)
		fail("a scrub whose walk ends at the hold: %s", clerr(&r));
	if(jobparked(&cl, "done", val, sizeof val) == nil)
		fail("the pass never reached its hold");
	if(clwrite(&cl, Fctl, 0, "scrub stop", &r) != Rwrite)
		fail("scrub stop over a parked pass: %s", clerr(&r));
	checks++;
	if(clwrite(&cl, Fctl, 0, "scrub start", &r) != Rwrite)
		fail("a scrub start over a pass that has finished: %s",
			clerr(&r));
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	istrue("the passes over that index ended", !jobrunning(&cl));

	/* fill the job cap, so that the next `scrub start' is refused */
	srvhook(ctx, "jobhold", 1);
	for(i = 0; i < 12; i++){
		snprint(line, sizeof line, "forget peer%.2d", i);
		if(clwrite(&cl, Fctl, 0, line, &r) != Rwrite)
			fail("%#q: %s", line, clerr(&r));
	}
	clwrite(&cl, Fctl, 0, "scrub start", &r);
	clerris("a scrub start with no job to be had", &r,
		"shoalsrv: too many jobs");
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	istrue("the passes holding the cap ended", !jobrunning(&cl));

	/* the refusal left nothing latched: this one really starts */
	if(clwrite(&cl, Fctl, 0, "scrub start rate=1", &r) != Rwrite)
		fail("scrub start after a refused one: %s", clerr(&r));
	if(slurpfile(&cl, Ffile, "jobs", buf, sizeof buf) > 0)
		eqv("a scrub start after a refused one starts a pass",
			nlines(buf, "job=scrub"), 1);
	else
		fail("a scrub start after a refused one started nothing");
	if(clwrite(&cl, Fctl, 0, "scrub stop", &r) != Rwrite)
		fail("final scrub stop: %s", clerr(&r));
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
Out:
	srvhook(ctx, "objhold", 0);
	srvhook(ctx, "jobhold", 0);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * `scrub rate=' written to a pass that is running paces what is left
 * of it, and does not re-bill what it has already read.
 *
 * store.md §14(31) makes the pace cumulative: the pass charges itself
 * each object's bytes plus a floor and waits until the whole charge
 * would have taken that long at the rate.  Read fresh each object,
 * that arithmetic applies a new rate to the bytes already charged as
 * well — so a rate raised mid-pass would put the deadline for
 * everything read so far in the past and the pass would run flat out
 * until it caught up, which is the opposite of what an operator
 * raising a rate asks for.
 *
 * Driven by the clock, since the pace is the thing under test: a pass
 * at 8 objects a second is left to walk for three seconds, the rate
 * is raised fourfold, and it must still be walking more than a second
 * later.  With the charge carried over it would have run the rest of
 * the index off in a few tens of milliseconds.
 */
static void
tscrubrate(void)
{
	char val[64], name[32], *m;
	Srvctx *ctx;
	Dev *d;
	Cl cl;
	Fcall r;
	uvlong d0, d1;
	int i;

	clstage = "scrubrate";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	for(i = 0; i < 100; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		mkobj(srvstore(ctx), name, nil, 0, 1);
	}
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	/* eight objects a second: an empty object is charged the floor */
	if(clwrite(&cl, Fctl, 0, "scrub start rate=8", &r) != Rwrite){
		fail("scrub start: %s", clerr(&r));
		goto Out;
	}
	sleep(3000);
	if(jobfield(&cl, "done", val, sizeof val) == nil){
		fail("the pass ended before the rate was raised");
		goto Out;
	}
	d0 = strtoull(val, nil, 10);
	istrue("the pass is still walking at the rate it started with", d0 > 0);
	if(clwrite(&cl, Fctl, 0, "scrub rate=32", &r) != Rwrite)
		fail("scrub rate=32: %s", clerr(&r));
	sleep(1200);
	istrue("a raised rate paces what is left, not what is done",
		jobrunning(&cl));
	if(jobfield(&cl, "done", val, sizeof val) == nil)
		fail("the pass is listed but has no done=");
	else{
		d1 = strtoull(val, nil, 10);
		istrue("and it does move on at the rate it was given", d1 > d0);
	}
	if(clwrite(&cl, Fctl, 0, "scrub stop", &r) != Rwrite)
		fail("scrub stop: %s", clerr(&r));
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	istrue("and stops when it is told to", !jobrunning(&cl));
Out:
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * layer-a §7.5's "continuously": the scrub's own timer (store.md §8),
 * which is what runs a pass on an instance nobody writes a verb to.
 *
 * The period is `scrubdays' and the default is 14 days, so srv.h's
 * `srvscrubms' is what a T1 can drive and the default itself is read
 * rather than waited out.  /status's `scrubnext=' is that same
 * schedule seen from the wire: it counts down while the timer waits,
 * and a TICK re-arms it at whatever period is then in force — which
 * is what the knob's second value shows, since a wait armed once at
 * start-up would still be counting the first one down.
 *
 * §13's `jobhold' keeps the pass the timer started listed once its
 * walk is over, as it does for a verb's, and the knob goes to a period
 * no test outlasts before the /jobs line is counted, so the ticks
 * behind the first cannot park a second pass beside it.
 */
static void
tscrubtimer(void)
{
	char buf[8192], val[64], name[32], *m;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	uvlong dflt, slow, n0, n1, n2;
	int i;

	clstage = "scrubtimer";
	dflt = (uvlong)14*86400000;
	slow = (uvlong)4*3600000;
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	for(i = 0; i < 4; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		mkobj(st, name, nil, 0, 1);
	}
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	istrue("no pass runs before the timer fires", !jobrunning(&cl));
	eqv("the default period is layer-a §7.5's own 14 days",
		srvscrubperiod(ctx), dflt);

	/*
	 * The schedule at /status, before any tick: a whole period less
	 * the moment this instance has been up, and falling.
	 */
	n0 = statusnum(&cl, "scrubnext");
	istrue("/status's scrubnext= is inside that period",
		n0 > dflt - 60000 && n0 <= dflt);
	sleep(1200);
	n1 = statusnum(&cl, "scrubnext");
	istrue("and it counts down as the timer waits", n1 + 1000 <= n0);

	/* a pass with no verb written at all */
	srvhook(ctx, "jobhold", 1);
	srvscrubms(ctx, 30);
	eqv("the knob is what overrides the period",
		srvscrubperiod(ctx), 30);
	if(jobparked(&cl, "job", val, sizeof val) == nil)
		fail("no pass reached the hold: the timer never fired");
	else
		eqs("the pass the timer started is a scrub", val, "scrub");
	srvscrubms(ctx, slow);		/* no further tick in this case */
	if(slurpfile(&cl, Ffile2, "jobs", buf, sizeof buf) > 0)
		eqv("and the ticks behind it started no second pass",
			nlines(buf, "job="), 1);
	else
		fail("the parked pass left no /jobs line");

	/* the tick re-armed the wait, at the period then in force */
	n2 = 0;
	for(i = 0; i < 100; i++){
		n2 = statusnum(&cl, "scrubnext");
		if(n2 > slow - 5000 && n2 <= slow)
			break;
		sleep(50);
	}
	istrue("scrubnext= is re-armed after a tick",
		n2 > slow - 5000 && n2 <= slow);
	srvscrubms(ctx, 0);
Out:
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * What a tick does to a pass that is already running, and what the
 * verbs do to the schedule (store.md §8, §14(39)).
 *
 * A tick that lands on a running pass starts nothing: the walk still
 * going IS layer-a §7.5's continuity, and a second pass over one index
 * is what `scrubbing' keeps off.  `scrub stop' ends that pass and
 * leaves the schedule where it was, so the NEXT tick starts another —
 * the reading §2.5's `reclaim' row already states for the reclaim
 * walk, which is how the two verbs come to agree by design.
 *
 * The pass is made slow enough to still be walking several ticks later
 * by `rate=1': one KiB/s over objects charged the 1 KiB floor each is
 * a second an object, and the index holds forty of them.  The rate
 * outlives the pass it was set on, so the pass the next tick starts is
 * as slow, which is what makes that one visible at /jobs too.
 */
static void
tscrubticks(void)
{
	char buf[8192], name[32], *m;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	Fcall r;
	int i;

	clstage = "scrubticks";
	m = mkmap();
	d = newdisk();
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	for(i = 0; i < 40; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		mkobj(st, name, nil, 0, 1);
	}
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	if(clwrite(&cl, Fctl, 0, "scrub start rate=1", &r) != Rwrite){
		fail("scrub start: %s", clerr(&r));
		goto Out;
	}
	istrue("the pass a verb started is listed at /jobs", jobrunning(&cl));

	/* several ticks land on it, and each starts nothing */
	srvscrubms(ctx, 30);
	sleep(2000);
	if(slurpfile(&cl, Ffile, "jobs", buf, sizeof buf) > 0){
		eqv("a tick on a running pass starts no second one",
			nlines(buf, "job="), 1);
		istrue("and the pass it landed on is still running",
			haspfx(buf, "job=scrub state=running "));
	}else
		fail("/jobs is empty with a slow pass running");

	/*
	 * `stop' ends that pass and turns no schedule off.  The period
	 * goes back to `scrubdays' across the stop, because what says the
	 * pass ended is jobrunning(), and that reads "/jobs is empty" and
	 * not "this pass is over": a tick landing in the window where the
	 * stopped pass has unlinked would fill /jobs with a pass of its
	 * own, and the stop would read as never having taken.  The window
	 * is a fraction of a tick and the poll below samples it, so the
	 * tick is kept out of it rather than raced with.  The half after
	 * shortens the period again, which is what that half is about.
	 */
	srvscrubms(ctx, 0);
	if(clwrite(&cl, Fctl, 0, "scrub stop", &r) != Rwrite)
		fail("scrub stop: %s", clerr(&r));
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	istrue("a pass stops when it is told to", !jobrunning(&cl));
	srvscrubms(ctx, 30);
	for(i = 0; i < 200 && !jobrunning(&cl); i++)
		sleep(20);
	if(slurpfile(&cl, Ffile, "jobs", buf, sizeof buf) > 0)
		eqv("and a later tick starts another: the stop held no "
			"schedule", nlines(buf, "job=scrub"), 1);
	else
		fail("no tick started a pass after a scrub stop");
	srvscrubms(ctx, 0);
	if(clwrite(&cl, Fctl, 0, "scrub stop", &r) != Rwrite)
		fail("the final scrub stop: %s", clerr(&r));
Out:
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	clstop(&cl);
	srvfree(ctx);
	devclose(d);
	free(m);
}

/*
 * D16's shutdown against the SCRUB timer, which like the reclaim
 * walk's holds no job: srvshutdown waits for it apart from the jobs
 * and before them (srv.h), and what that wait is worth is read at the
 * moment the store closes.
 *
 * The timer is made demonstrably alive across the shutdown rather than
 * assumed to be: the knob puts its period in tens of milliseconds, the
 * case waits until a pass no verb asked for has come and gone, and it
 * asserts the proc is still up with the client about to go.  The
 * period knob then goes back and the SLICE knob goes to seconds, so
 * the shutdown begins with the proc inside a sleep longer than
 * everything srvshutdown does after the wait — which is what makes
 * `the scrub timer had ended when the store closed' a check about the
 * wait rather than about how long the rest of the shutdown takes
 * (srv.h).  The slice is also what bounds that wait, which is the
 * other thing read here.
 */
static void
tscrubwait(void)
{
	char val[64], name[32], *m;
	Srvctx *ctx;
	Store *st;
	Dev *d;
	Cl cl;
	vlong t0;
	uvlong tick;
	int i;

	clstage = "scrubwait";
	tick = 3000;
	m = mkmap();
	d = newdisk();
	freedseen = 0;
	freedscrub = -1;
	if((ctx = startsrv(d, m, 4, 0)) == nil)
		return;
	st = srvstore(ctx);
	for(i = 0; i < 4; i++){
		snprint(name, sizeof name, "obj%.2d", i);
		mkobj(st, name, nil, 0, 1);
	}
	clstart(&cl, ctx, Clmsize);
	if(!adminctl(&cl, "role=admin"))
		goto Out;
	srvhook(ctx, "jobhold", 1);
	srvscrubms(ctx, 30);
	if(jobparked(&cl, "job", val, sizeof val) == nil)
		fail("no pass reached the hold: the timer never fired");
	else
		eqs("a pass the timer started is a scrub", val, "scrub");
	srvscrubms(ctx, 0);
	srvhook(ctx, "jobhold", 0);
	for(i = 0; i < 400 && jobrunning(&cl); i++)
		sleep(20);
	istrue("the timer is still up with the pass over",
		srvscrublive(ctx) != 0);
	/*
	 * And it is made to stay up ACROSS the wait, which is what gives
	 * the check at the store's close anything to catch: the server's
	 * own tick is half a second and everything srvshutdown does
	 * after the wait takes longer than that, so a shutdown that never
	 * waited would still reach storeclose with the proc gone most
	 * times over, and the check would pass either way (srv.h).  The
	 * tick goes to seconds with the period already back at
	 * `scrubdays', and the sleep is what carries the proc out of the
	 * old half-second tick and into a new long one before the loop
	 * ends below.
	 *
	 * 1500 is three of the server's own ticks, not one: the proc is
	 * at most one old tick from waking, but Plan 9's sleep(2) is a
	 * lower bound on the wait and nothing bounds it from above, so a
	 * sleep of 700 was a 200 ms margin against a scheduler that owes
	 * none.  Three ticks is a margin worth having and costs a second
	 * of one case's run.
	 */
	srvscrubtickms(ctx, tick);
	sleep(1500);
Out:
	t0 = nsec()/1000000;
	clstop(&cl);			/* the loop ends; the shutdown runs */
	istrue("the shutdown waited no longer than a tick of the timer's",
		nsec()/1000000 - t0 < (vlong)tick + 1000);
	eqv("the store was closed once", freedseen, 1);
	eqv("the scrub timer had ended when the store closed", freedscrub, 0);
	istrue("and it is not reading the context now either",
		srvscrublive(ctx) == 0);
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
	else
		fail("/jobs is empty with twelve passes accepted");
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
 *
 * The pair of ids the case needs — one this instance is placed for
 * and one it is not — is chosen with mapplace, which is the function
 * the server's guard calls.  So this case does not check the
 * placement: it checks what the verb does on each side of it, which
 * is what it is for.  Placement is `maptest's, at known-answer
 * vectors computed outside this codebase (AGENTS.md); choosing the
 * ids here any other way would only be a second guess at the same
 * function.
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
	istrue("the stray copy is gone and left no tombstone",
		statof(st, stray, &oi) < 0);

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
	/*
	 * The job the shutdown is about to wait for, read while the pass
	 * is still holding it.  The same count after clstop is taken once
	 * jobwait has returned and so cannot be anything but 0; this is
	 * the reading that can.
	 */
	eqv("the running pass holds a job", srvjobcount(ctx), 1);
Out:
	clstop(&cl);			/* the loop ends; the shutdown runs */
	eqv("the store was closed once", freedseen, 1);
	istrue("the pass had ended before the store closed", freedjobs == 1);
	eqv("no job is left held", srvjobcount(ctx), 0);
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
	clwatchms = 120*1000;		/* this program's own budget */
	clwatchon();

	tparse();
	tmonid();
	tfiles();
	tdir();
	tdircursor();
	tdirflush();
	tdirstall();
	tdiropenflush();
	tdiropen2();
	tsnaprefuse();
	tscrub();
	tlostslot();
	tqueued();
	tqjobcount();
	treclaim();
	treclaimtimer();
	treclaimctl();
	treclaimrace();
	treclaimdown();
	treclaimwait();
	tforget();
	tscrubctl();
	tscrubrate();
	tscrubtimer();
	tscrubticks();
	tscrubwait();
	tpassfail();
	tobjgone();
	tjobs();
	tdrop();
	tshutdown();

	clwatchoff();
	/*
	 * Every case ran and every check in it was reached.  A case that
	 * skips a block — a file that rendered empty, a fixture that
	 * could not be built — reports one fewer check and no failure, so
	 * the total is what says the suite is whole.  Update it when a
	 * check is added or removed; it is not a target to reach.
	 */
	eqv("every case ran", checks, Nchecks);
	if(fails > 0){
		fprint(2, "srvenumtest: %d of %d checks failed\n", fails, checks);
		threadexitsall("fail");
	}
	print("srvenumtest: %d checks ok\n", checks);
	threadexitsall(nil);
}
