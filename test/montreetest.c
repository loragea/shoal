#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../lib/shoal.h"
#include "../mon/mon.h"

/*
 * T1: the monitor's 9P surface, docs/design/layer-a.md §8 — §8.1's
 * attach grammar, its file tree and role matrix, the render-at-open
 * status files, §8.2's retention as /maps, §8.3's ctl framework and
 * §8.4's liveness evidence — and the shutdown.
 *
 * A whole monitor runs inside this program: a simulated disk, the map
 * slot store of docs/design/store.md §10 formatted on it,
 * mon/libshoalmon.a over that, and a raw 9P client on the other end of
 * a pipe (test/mon9p.h).  Nothing is mounted and nothing is exec'd, so
 * what a case asserts is the exact bytes of a reply — which is what
 * most of §8's rules are about.
 *
 * The maps are committed with moncommit directly, before the service
 * is started, so that the ring has history to serve; the snapshot case
 * commits through the RUNNING service's own Mon.  That is allowed and
 * is the reason mon.h exports the lock: Mon has none of its own
 * (lib/shoal.h), so a caller that publishes to a Mon a service is
 * serving MUST hold monsrvlock across the commit and call monsrvremap
 * before letting go.  A test that committed behind the lock would be
 * racing the renders it is about to assert, which is a bug in the test
 * and not a discovery about the server.
 */

int mainstacksize = Monstack;

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

#include "mon9p.h"

enum
{
	Tsecsz	= 512,
	Tnsec	= 4096,			/* a 2 MiB image: §10's floor is 1 */
	Tseed	= 0x71c3,
	Tslotsz	= 4096,
	Tretain	= 4,			/* short, so a ring wrap is cheap */

	/* fids the cases use */
	Froot	= 1,
	Ff	= 2,
	Ff2	= 3,
	Ff3	= 4,
	Froot2	= 5,
	Froot3	= 6,
};

static char Tmonid[] = "00112233445566778899aabbccddeeff";
static char Tuuid0[] = "3f1c9a20b47e4d18a0c6e5721b93df04";
static char Tuuid1[] = "5b9e13c74a0d482fb6318ce2d05a7f16";

/* the three anames the role matrix is driven through */
static char Areadera[]	= "";
static char Ainsta[]	= "role=instance,peer=n1.0";
static char Aadmina[]	= "role=admin";

/*
 * One cluster map, at the epoch asked for.  Two instances on two
 * nodes, so /instances has two lines to be field-exact about, and one
 * stale record, so /stale has one (§3.1's ledger travels in the map).
 * `since=' on the mark is the epoch, which makes each epoch's text
 * distinct from every other's — what the /maps cases compare.
 */
static char*
mkmap(uvlong epoch)
{
	char *p;

	p = smprint(
		"map=t epoch=%llud\n"
		"\tmonid=%s\n"
		"\tobjmax=1048576 blksz=4096 replicas=2\n"
		"\tcsumalg=blake2s256 placehash=blake2s256-64\n"
		"\tpollms=1000 leasems=3000 replms=1000 deadms=10000\n"
		"\toutmins=60 tombdays=7 mincopies=1 retain=8\n"
		"\n"
		"node=n1\n"
		"\n"
		"instance=n1.0 onnode=n1 addr=tcp!10.0.0.1!17011\n"
		"\tuuid=%s\n"
		"\tclass=ssd weight=100 status=in up=yes since=1 fenced=no\n"
		"instance=n2.0 onnode=n2 addr=tcp!10.0.0.2!17011\n"
		"\tuuid=%s\n"
		"\tclass=hdd weight=100 status=out up=no since=2 fenced=no\n"
		"\n"
		"stale=n2.0 reporter=n1.0 since=%llud\n",
		epoch, Tmonid, Tuuid0, Tuuid1, epoch);
	if(p == nil)
		sysfatal("smprint: %r");
	return p;
}

static Dev*
freshdisk(void)
{
	Dev *d;
	Monfmtcfg c;

	if((d = simopen(Tsecsz, Tnsec, Tseed)) == nil)
		sysfatal("simopen: %r");
	memset(&c, 0, sizeof c);
	c.slotsz = Tslotsz;
	c.retain = Tretain;
	if(monfmt(d, &c) < 0)
		sysfatal("monfmt: %r");
	return d;
}

/* publish epochs first..last through a Mon of this program's own */
static void
seedmaps(Dev *d, uvlong first, uvlong last)
{
	Mon *m;
	char *t;
	uvlong e;

	if((m = monopen(d)) == nil)
		sysfatal("monopen: %r");
	for(e = first; e <= last; e++){
		t = mkmap(e);
		if(moncommit(m, t, strlen(t), e) < 0)
			sysfatal("moncommit %llud: %r", e);
		free(t);
	}
	monclose(m);
}

static Monctx*
start(Dev *d)
{
	Moncfg cfg;
	Monctx *c;

	memset(&cfg, 0, sizeof cfg);
	cfg.dev = d;
	checks++;
	if((c = monsrvnew(&cfg)) == nil){
		fail("monsrvnew: %r");
		return nil;
	}
	return c;
}

/*
 * Walk a fresh fid from the root to one name and open it.  The answer
 * is "ok" or the exact Rerror, so a role-matrix cell is one string
 * compare whether it was the walk or the open that refused.  A fid the
 * walk established is clunked either way, so the cells can be run in a
 * loop over one fid number.
 */
static char*
opencell(Cl *c, ulong root, ulong fid, char *name, int mode, char *buf,
	int nbuf)
{
	Fcall r;

	if(clwalk1(c, root, fid, name, &r) != Rwalk){
		snprint(buf, nbuf, "%s", clerr(&r));
		return buf;
	}
	if(r.nwqid != 1){
		snprint(buf, nbuf, "short walk");
		return buf;
	}
	if(clopen(c, fid, mode, &r) != Ropen){
		snprint(buf, nbuf, "%s", clerr(&r));
		clclunk(c, fid, &r);
		return buf;
	}
	clclunk(c, fid, &r);
	return "ok";
}

/* the whole of one file, opened by name from the root; -1 on a refusal */
static long
slurpname(Cl *c, ulong root, ulong fid, char *name, char *buf, long max,
	char *err, int nerr)
{
	Fcall r;
	long n;

	err[0] = 0;
	if(clwalk1(c, root, fid, name, &r) != Rwalk){
		snprint(err, nerr, "%s", clerr(&r));
		return -1;
	}
	if(clopen(c, fid, OREAD, &r) != Ropen){
		snprint(err, nerr, "%s", clerr(&r));
		clclunk(c, fid, &r);
		return -1;
	}
	n = clslurp(c, fid, buf, max);
	clclunk(c, fid, &r);
	if(n < 0)
		snprint(err, nerr, "read failed");
	return n;
}

/* one ctl write's answer, on a fid already open for writing */
static char*
ctlsay(Cl *c, ulong fid, char *line, char *buf, int nbuf)
{
	Fcall t, r;

	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = cltag(c);
	t.fid = fid;
	t.offset = 0;
	t.count = strlen(line);
	t.data = line;
	if(clrpc(c, &t, &r) != Rwrite){
		snprint(buf, nbuf, "%s", clerr(&r));
		return buf;
	}
	if(r.count != (ulong)strlen(line))
		snprint(buf, nbuf, "short write %lud", r.count);
	else
		snprint(buf, nbuf, "ok");
	return buf;
}

/* how many directory entries a read answered, and the i'th name */
static int
dirents(char *p, long n, char **names, int maxn)
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
		if(k < maxn)
			names[k] = strdup(dir.name);
		p += m;
		k++;
	}
	return k;
}

static int
hasname(char **v, int n, char *want)
{
	int i;

	for(i = 0; i < n; i++)
		if(v[i] != nil && strcmp(v[i], want) == 0)
			return 1;
	return 0;
}

static void
freenames(char **v, int n)
{
	int i;

	for(i = 0; i < n; i++){
		free(v[i]);
		v[i] = nil;
	}
}

/*
 * A monitor that has never published.  store.md §14(54): the start is
 * not a refusal, /map is refused with this server's own string rather
 * than answered with zero bytes, the record files render nothing and
 * /status says which case a reader is in.
 */
static void
tempty(void)
{
	char buf[8192], err[ERRMAX], f[256], *v[8];
	Monctx *ctx;
	Dev *d;
	Fcall r;
	Cl cl;
	long n;
	int k;

	clstage = "empty store";
	d = freshdisk();
	if((ctx = start(d)) == nil){
		devclose(d);
		return;
	}
	clstart(&cl, ctx, Clmsize);
	istrue("an empty store attaches",
		clattach(&cl, Froot, Aadmina, &r) == Rattach);

	n = slurpname(&cl, Froot, Ff, "map", buf, sizeof buf, err, sizeof err);
	eqv("/map on an empty store answers no bytes", n == -1, 1);
	eqs("/map on an empty store", err, "shoalmon: no map");

	n = slurpname(&cl, Froot, Ff, "status", buf, sizeof buf, err,
		sizeof err);
	istrue("/status renders on an empty store", n > 0);
	eqs("hasmap", clfield(buf, "hasmap", f, sizeof f), "no");
	istrue("no epoch= on an empty store",
		clfield(buf, "epoch", f, sizeof f) == nil);
	istrue("no monid= on an empty store",
		clfield(buf, "monid", f, sizeof f) == nil);
	istrue("no deadms= on an empty store",
		clfield(buf, "deadms", f, sizeof f) == nil);
	istrue("no retain= on an empty store",
		clfield(buf, "retain", f, sizeof f) == nil);
	eqs("ledger", clfield(buf, "ledger", f, sizeof f), "ok");
	eqs("pending", clfield(buf, "pending", f, sizeof f), "0");
	eqs("slots", clfield(buf, "slots", f, sizeof f), "4");
	eqs("maps", clfield(buf, "maps", f, sizeof f), "0");
	eqs("seq", clfield(buf, "seq", f, sizeof f), "0");

	n = slurpname(&cl, Froot, Ff, "instances", buf, sizeof buf, err,
		sizeof err);
	eqv("/instances on an empty store", n, 0);
	n = slurpname(&cl, Froot, Ff, "stale", buf, sizeof buf, err,
		sizeof err);
	eqv("/stale on an empty store", n, 0);
	n = slurpname(&cl, Froot, Ff, "health", buf, sizeof buf, err,
		sizeof err);
	eqv("/health on an empty store", n, 0);
	n = slurpname(&cl, Froot, Ff, "maps", buf, sizeof buf, err,
		sizeof err);
	istrue("/maps on an empty store reads", n >= 0);
	k = dirents(buf, n, v, nelem(v));
	eqv("/maps on an empty store lists nothing", k, 0);
	freenames(v, k);

	clstop(&cl);
	monsrvfree(ctx);
	devclose(d);
}

/*
 * store.md §14(54)'s third clause: a current map that IS there and
 * does not parse refuses the start, because every file but /map is
 * rendered out of the parse.
 */
static void
tbadmap(void)
{
	Monctx *ctx;
	Moncfg cfg;
	Dev *d;
	Mon *m;
	char *t;

	clstage = "unparseable map";
	d = freshdisk();
	t = "this is not a cluster map\n";
	if((m = monopen(d)) == nil)
		sysfatal("monopen: %r");
	if(moncommit(m, t, strlen(t), 3) < 0)
		sysfatal("moncommit: %r");
	monclose(m);

	memset(&cfg, 0, sizeof cfg);
	cfg.dev = d;
	ctx = monsrvnew(&cfg);
	istrue("a current map that does not parse refuses the start",
		ctx == nil);
	if(ctx != nil)
		monsrvfree(ctx);
	devclose(d);
}

/* §8.1's attach grammar, and store.md §14(58)'s fills */
static void
tattach(void)
{
	static struct {
		char	*aname;
		char	*want;
	} v[] = {
		{"",				"ok"},
		{"role=reader",			"ok"},
		{"role=admin",			"ok"},
		{"role=instance,peer=n1.0",	"ok"},
		{"peer=n1.0,role=instance",	"ok"},
		{"peer=n9.9",			"ok"},
		{"role=admin,peer=n1.0",	"ok"},
		{"role=instance",		"bad aname"},
		{"role=instance,peer=",		"bad aname"},
		{"role=bogus",			"bad aname"},
		{"role=client",			"bad aname"},
		{"epoch=3",			"bad aname"},
		{"epoch=3,role=instance,peer=n1.0", "bad aname"},
		{"role=reader,role=admin",	"bad aname"},
		{"peer=n1.0,peer=n2.0",		"bad aname"},
		{"rolereader",			"bad aname"},
		{"role=admin,",			"bad aname"},
		{",role=admin",			"bad aname"},
	};
	char what[128];
	Monctx *ctx;
	Dev *d;
	Fcall r;
	Cl cl;
	int i;

	clstage = "attach";
	d = freshdisk();
	seedmaps(d, 11, 11);
	if((ctx = start(d)) == nil){
		devclose(d);
		return;
	}
	clstart(&cl, ctx, Clmsize);
	for(i = 0; i < nelem(v); i++){
		snprint(what, sizeof what, "attach %#q", v[i].aname);
		clattach(&cl, Froot, v[i].aname, &r);
		eqs(what, clerr(&r), v[i].want);
		if(r.type == Rattach){
			istrue("the attach qid is the root directory",
				r.qid.type == QTDIR);
			clclunk(&cl, Froot, &r);
		}
	}
	clstop(&cl);
	monsrvfree(ctx);
	devclose(d);
}

/*
 * §8.1's tree: all eight names present, with the qid type each row
 * carries, and the root listing filtered by the walk column — which is
 * store.md §14(58)'s /map.next cell said the other way round.
 */
static void
ttree(void)
{
	static struct {
		char	*name;
		int	type;
	} v[] = {
		{"ctl",		QTFILE},
		{"map",		QTFILE},
		{"map.next",	QTFILE},
		{"maps",	QTDIR},
		{"instances",	QTFILE},
		{"stale",	QTFILE},
		{"health",	QTFILE},
		{"status",	QTFILE},
	};
	char buf[8192], err[ERRMAX], what[128], sbuf[512], *nm[16];
	Monctx *ctx;
	Dev *d;
	Fcall r;
	Cl cl;
	Dir dir;
	long n;
	int i, k;

	USED(err);

	clstage = "tree";
	d = freshdisk();
	seedmaps(d, 11, 12);
	if((ctx = start(d)) == nil){
		devclose(d);
		return;
	}
	clstart(&cl, ctx, Clmsize);
	clattach(&cl, Froot, Aadmina, &r);
	for(i = 0; i < nelem(v); i++){
		snprint(what, sizeof what, "walk to /%s", v[i].name);
		if(clwalk1(&cl, Froot, Ff, v[i].name, &r) != Rwalk){
			checks++;
			fail("%s: %s", what, clerr(&r));
			continue;
		}
		istrue(what, r.nwqid == 1);
		snprint(what, sizeof what, "/%s qid type", v[i].name);
		eqv(what, r.wqid[0].type, v[i].type);
		snprint(what, sizeof what, "/%s qid vers", v[i].name);
		eqv(what, r.wqid[0].vers, 0);
		if(clstat(&cl, Ff, &r) == Rstat
		&& convM2D(r.stat, r.nstat, &dir, sbuf) > BIT16SZ){
			snprint(what, sizeof what, "/%s stat name", v[i].name);
			eqs(what, dir.name, v[i].name);
			snprint(what, sizeof what, "/%s stat length",
				v[i].name);
			eqv(what, dir.length, 0);
			snprint(what, sizeof what, "/%s stat mode", v[i].name);
			eqv(what, dir.mode & DMDIR,
				v[i].type == QTDIR ? DMDIR : 0);
		}
		clclunk(&cl, Ff, &r);
	}
	/* a name this tree does not have */
	clwalk1(&cl, Froot, Ff, "nosuch", &r);
	eqs("a name the tree does not have", clerr(&r),
		"shoalmon: no such file");

	/* the root itself: clone the attached fid and read it */
	clwalk(&cl, Froot, Ff, 0, nil, &r);
	clopen(&cl, Ff, OREAD, &r);
	n = clslurp(&cl, Ff, buf, sizeof buf);
	k = dirents(buf, n, nm, nelem(nm));
	eqv("an admin sees every row of the tree", k, nelem(v));
	for(i = 0; i < nelem(v); i++){
		snprint(what, sizeof what, "/%s is in an admin's listing",
			v[i].name);
		istrue(what, hasname(nm, k, v[i].name));
	}
	freenames(nm, k);
	clclunk(&cl, Ff, &r);

	clattach(&cl, Froot2, Areadera, &r);
	clwalk(&cl, Froot2, Ff2, 0, nil, &r);
	clopen(&cl, Ff2, OREAD, &r);
	n = clslurp(&cl, Ff2, buf, sizeof buf);
	k = dirents(buf, n, nm, nelem(nm));
	eqv("a reader sees every row but /map.next", k, nelem(v)-1);
	istrue("/map.next is not in a reader's listing",
		!hasname(nm, k, "map.next"));
	istrue("/map is in a reader's listing", hasname(nm, k, "map"));
	freenames(nm, k);
	clclunk(&cl, Ff2, &r);

	clstop(&cl);
	monsrvfree(ctx);
	devclose(d);
}

/*
 * §8.1's role matrix, every cell, exact strings.  store.md §14(58) is
 * the table this drives; a cell reached through a walk the role may
 * not make answers the same `permission denied' the open would.
 */
static void
tmatrix(void)
{
	static struct {
		char	*file;
		int	mode;
		char	*want[3];	/* reader, instance, admin */
	} v[] = {
		{"ctl",		OREAD,	{"ok", "ok", "ok"}},
		{"ctl",		OWRITE,	{"permission denied", "ok", "ok"}},
		{"ctl",		ORDWR,	{"permission denied", "ok", "ok"}},
		{"map",		OREAD,	{"ok", "ok", "ok"}},
		{"map",		OWRITE,	{"permission denied",
					 "permission denied",
					 "permission denied"}},
		{"map.next",	OREAD,	{"permission denied",
					 "permission denied", "ok"}},
		{"map.next",	OWRITE,	{"permission denied",
					 "permission denied", "ok"}},
		{"maps",	OREAD,	{"ok", "ok", "ok"}},
		/*
		 * The write column of a directory row is lib9p's to
		 * refuse: sopen answers a non-read open of a QTDIR fid
		 * with `is a directory' before this server is called at
		 * all, whatever the row's matrix says (store.md
		 * §14(58)).
		 */
		{"maps",	OWRITE,	{"is a directory", "is a directory",
					 "is a directory"}},
		{"instances",	OREAD,	{"ok", "ok", "ok"}},
		{"instances",	OWRITE,	{"permission denied",
					 "permission denied",
					 "permission denied"}},
		{"stale",	OREAD,	{"ok", "ok", "ok"}},
		{"stale",	OWRITE,	{"permission denied",
					 "permission denied",
					 "permission denied"}},
		{"health",	OREAD,	{"ok", "ok", "ok"}},
		{"health",	OWRITE,	{"permission denied",
					 "permission denied",
					 "permission denied"}},
		{"status",	OREAD,	{"ok", "ok", "ok"}},
		{"status",	OWRITE,	{"permission denied",
					 "permission denied",
					 "permission denied"}},
	};
	static char *aname[3] = {Areadera, Ainsta, Aadmina};
	static char *rname[3] = {"reader", "instance", "admin"};
	static ulong root[3] = {Froot, Froot2, Froot3};
	char buf[ERRMAX], what[160];
	Monctx *ctx;
	Dev *d;
	Fcall r;
	Cl cl;
	int i, j;

	clstage = "role matrix";
	d = freshdisk();
	seedmaps(d, 11, 12);
	if((ctx = start(d)) == nil){
		devclose(d);
		return;
	}
	clstart(&cl, ctx, Clmsize);
	for(j = 0; j < 3; j++)
		clattach(&cl, root[j], aname[j], &r);
	for(i = 0; i < nelem(v); i++)
		for(j = 0; j < 3; j++){
			snprint(what, sizeof what, "%s opens /%s for %s",
				rname[j], v[i].file,
				v[i].mode == OREAD ? "reading" :
				v[i].mode == OWRITE ? "writing" : "both");
			eqs(what, opencell(&cl, root[j], Ff, v[i].file,
				v[i].mode, buf, sizeof buf), v[i].want[j]);
		}
	/* a create, a remove and a wstat: nothing in this tree takes one */
	clwalk1(&cl, Froot3, Ff, "status", &r);
	clremove(&cl, Ff, &r);
	eqs("an admin removes /status", clerr(&r), "permission denied");
	clwalk1(&cl, Froot3, Ff, "ctl", &r);
	clremove(&cl, Ff, &r);
	eqs("an admin removes /ctl", clerr(&r), "shoalmon: not built");

	clstop(&cl);
	monsrvfree(ctx);
	devclose(d);
}

/* /map is the text committed, byte for byte */
static void
tmap(void)
{
	char buf[8192], err[ERRMAX];
	Monctx *ctx;
	Dev *d;
	Fcall r;
	Cl cl;
	char *want;
	long n;

	clstage = "map";
	d = freshdisk();
	seedmaps(d, 11, 13);
	if((ctx = start(d)) == nil){
		devclose(d);
		return;
	}
	clstart(&cl, ctx, Clmsize);
	clattach(&cl, Froot, Aadmina, &r);
	n = slurpname(&cl, Froot, Ff, "map", buf, sizeof buf, err, sizeof err);
	want = mkmap(13);
	eqv("/map length", n, strlen(want));
	eqs("/map is the map that was committed", buf, want);
	free(want);
	clstop(&cl);
	monsrvfree(ctx);
	devclose(d);
}

/*
 * §8.2's retention, across a ring wrap at `retain'.  Six maps are
 * published into a four-slot ring, so the two oldest are gone and
 * /maps/<E−1> — which §5.2 clause 2 depends on — is still there.
 */
static void
tmaps(void)
{
	char buf[8192], err[ERRMAX], what[128], sbuf[512], *nm[16], *want;
	Monctx *ctx;
	Dev *d;
	Fcall r;
	Cl cl;
	Dir dir;
	uvlong q15, q16;
	long n;
	int k, i;

	q15 = q16 = 0;

	clstage = "maps";
	d = freshdisk();
	seedmaps(d, 11, 16);
	if((ctx = start(d)) == nil){
		devclose(d);
		return;
	}
	clstart(&cl, ctx, Clmsize);
	clattach(&cl, Froot, Ainsta, &r);

	n = slurpname(&cl, Froot, Ff, "maps", buf, sizeof buf, err,
		sizeof err);
	k = dirents(buf, n, nm, nelem(nm));
	eqv("/maps lists retain entries", k, Tretain);
	for(i = 13; i <= 16; i++){
		snprint(what, sizeof what, "/maps lists %d", i);
		snprint(err, sizeof err, "%d", i);
		istrue(what, hasname(nm, k, err));
	}
	istrue("/maps does not list the map that wrapped out",
		!hasname(nm, k, "12"));
	istrue("/maps does not list the first map", !hasname(nm, k, "11"));
	freenames(nm, k);

	/* every retained epoch answers its own text */
	for(i = 13; i <= 16; i++){
		snprint(err, sizeof err, "%d", i);
		clwalk1(&cl, Froot, Ff, "maps", &r);
		if(clwalk1(&cl, Ff, Ff2, err, &r) != Rwalk){
			checks++;
			fail("walk to /maps/%d: %s", i, clerr(&r));
			clclunk(&cl, Ff, &r);
			continue;
		}
		if(i == 15)
			q15 = r.wqid[0].path;
		if(i == 16)
			q16 = r.wqid[0].path;
		snprint(what, sizeof what, "/maps/%d qid type", i);
		eqv(what, r.wqid[0].type, QTFILE);
		clopen(&cl, Ff2, OREAD, &r);
		n = clslurp(&cl, Ff2, buf, sizeof buf);
		want = mkmap(i);
		snprint(what, sizeof what, "/maps/%d is that epoch's map", i);
		eqs(what, buf, want);
		free(want);
		clclunk(&cl, Ff2, &r);
		clclunk(&cl, Ff, &r);
	}
	istrue("two retained maps have different qid paths", q15 != q16);

	/* leading zeros are part of the number (store.md §14(55)) */
	clwalk1(&cl, Froot, Ff, "maps", &r);
	if(clwalk1(&cl, Ff, Ff2, "015", &r) == Rwalk){
		checks++;
		if(r.wqid[0].path != q15)
			fail("/maps/015 is not /maps/15");
		clopen(&cl, Ff2, OREAD, &r);
		n = clslurp(&cl, Ff2, buf, sizeof buf);
		want = mkmap(15);
		eqs("/maps/015 is epoch 15's map", buf, want);
		free(want);
		if(clstat(&cl, Ff2, &r) == Rstat
		&& convM2D(r.stat, r.nstat, &dir, sbuf) > BIT16SZ)
			eqs("/maps/015 stats under its canonical name",
				dir.name, "15");
		clclunk(&cl, Ff2, &r);
	}else{
		checks++;
		fail("/maps/015: %s", clerr(&r));
	}
	clclunk(&cl, Ff, &r);

	/* the two refusals of store.md §14(55) */
	clwalk1(&cl, Froot, Ff, "maps", &r);
	clwalk1(&cl, Ff, Ff2, "12", &r);
	eqs("an epoch the ring no longer holds", clerr(&r),
		"shoalmon: no such epoch");
	clwalk1(&cl, Ff, Ff2, "99", &r);
	eqs("an epoch never published", clerr(&r), "shoalmon: no such epoch");
	clwalk1(&cl, Ff, Ff2, "0", &r);
	eqs("epoch zero", clerr(&r), "shoalmon: no such epoch");
	clwalk1(&cl, Ff, Ff2, "abc", &r);
	eqs("a /maps element that is not a u64", clerr(&r),
		"shoalmon: no such file");
	clwalk1(&cl, Ff, Ff2, "15x", &r);
	eqs("a /maps element with a digit and a letter", clerr(&r),
		"shoalmon: no such file");
	clclunk(&cl, Ff, &r);

	clstop(&cl);
	monsrvfree(ctx);
	devclose(d);
}

/* /instances, /stale, /health and /status against the committed map */
static void
tfiles(void)
{
	char buf[8192], err[ERRMAX], f[256], *p;
	Monctx *ctx;
	Dev *d;
	Fcall r;
	Cl cl;
	long n;

	clstage = "status files";
	d = freshdisk();
	seedmaps(d, 11, 14);
	if((ctx = start(d)) == nil){
		devclose(d);
		return;
	}
	clstart(&cl, ctx, Clmsize);
	clattach(&cl, Froot, Aadmina, &r);

	n = slurpname(&cl, Froot, Ff, "instances", buf, sizeof buf, err,
		sizeof err);
	USED(n);
	p = smprint(
		"uuid=%s iid=n1.0 node=n1 addr=tcp!10.0.0.1!17011 class=ssd "
			"registered=0 lastseen=0\n"
		"uuid=%s iid=n2.0 node=n2 addr=tcp!10.0.0.2!17011 class=hdd "
			"registered=0 lastseen=0\n", Tuuid0, Tuuid1);
	eqs("/instances", buf, p);
	free(p);

	n = slurpname(&cl, Froot, Ff, "stale", buf, sizeof buf, err,
		sizeof err);
	USED(n);
	eqs("/stale is the map's ledger in §3.1's grammar", buf,
		"stale=n2.0 reporter=n1.0 since=14\n");

	n = slurpname(&cl, Froot, Ff, "health", buf, sizeof buf, err,
		sizeof err);
	USED(n);
	eqs("/health before anything has refreshed", buf,
		"iid=n1.0 lastrefresh=0 silent=0 reports=\n"
		"iid=n2.0 lastrefresh=0 silent=0 reports=\n");

	n = slurpname(&cl, Froot, Ff, "status", buf, sizeof buf, err,
		sizeof err);
	USED(n);
	eqs("hasmap", clfield(buf, "hasmap", f, sizeof f), "yes");
	eqs("epoch", clfield(buf, "epoch", f, sizeof f), "14");
	eqs("monid", clfield(buf, "monid", f, sizeof f), Tmonid);
	eqs("pollms", clfield(buf, "pollms", f, sizeof f), "1000");
	eqs("leasems", clfield(buf, "leasems", f, sizeof f), "3000");
	eqs("replms", clfield(buf, "replms", f, sizeof f), "1000");
	eqs("deadms", clfield(buf, "deadms", f, sizeof f), "10000");
	eqs("outmins", clfield(buf, "outmins", f, sizeof f), "60");
	eqs("tombdays", clfield(buf, "tombdays", f, sizeof f), "7");
	eqs("mincopies", clfield(buf, "mincopies", f, sizeof f), "1");
	eqs("replicas", clfield(buf, "replicas", f, sizeof f), "2");
	eqs("retain is the map's attribute",
		clfield(buf, "retain", f, sizeof f), "8");
	eqs("slots is the partition's ring",
		clfield(buf, "slots", f, sizeof f), "4");
	eqs("maps", clfield(buf, "maps", f, sizeof f), "4");
	eqs("seq", clfield(buf, "seq", f, sizeof f), "4");
	eqs("ledger", clfield(buf, "ledger", f, sizeof f), "ok");
	eqs("pending", clfield(buf, "pending", f, sizeof f), "0");
	istrue("uptime is present", clfield(buf, "uptime", f, sizeof f) != nil);

	clstop(&cl);
	monsrvfree(ctx);
	devclose(d);
}

/*
 * §8.1's last paragraph: every status file is snapshot-at-open, so a
 * publish that lands under an open fid cannot tear the read.  The
 * commit goes through the service's own Mon under monsrvlock, which is
 * the only way a caller outside this library may publish to a Mon a
 * service is serving (mon.h).
 */
static void
tsnapshot(void)
{
	char buf[8192], err[ERRMAX], *old, *new, *nm[16];
	Monctx *ctx;
	Dev *d;
	Fcall r;
	Cl cl;
	long n;
	int k;

	clstage = "snapshot at open";
	d = freshdisk();
	seedmaps(d, 11, 13);
	if((ctx = start(d)) == nil){
		devclose(d);
		return;
	}
	clstart(&cl, ctx, Clmsize);
	clattach(&cl, Froot, Aadmina, &r);

	/* /map, open before the publish */
	clwalk1(&cl, Froot, Ff, "map", &r);
	istrue("the snapshot fid opens", clopen(&cl, Ff, OREAD, &r) == Ropen);
	/* /maps, its listing snapshotted at the same moment */
	clwalk1(&cl, Froot, Ff2, "maps", &r);
	clopen(&cl, Ff2, OREAD, &r);

	new = mkmap(14);
	monsrvlock(ctx);
	if(moncommit(monsrvmon(ctx), new, strlen(new), 14) < 0)
		fail("moncommit under the service: %r");
	if(monsrvremap(ctx) < 0)
		fail("monsrvremap: %r");
	monsrvunlock(ctx);

	n = clslurp(&cl, Ff, buf, sizeof buf);
	old = mkmap(13);
	eqv("the snapshot's length is the old map's", n, strlen(old));
	eqs("a fid opened before the publish reads the old map whole",
		buf, old);
	free(old);
	clclunk(&cl, Ff, &r);

	n = clslurp(&cl, Ff2, buf, sizeof buf);
	k = dirents(buf, n, nm, nelem(nm));
	eqv("the /maps listing is the one taken at its open", k, 3);
	istrue("the map published since is not in it", !hasname(nm, k, "14"));
	freenames(nm, k);
	clclunk(&cl, Ff2, &r);

	/* and a fid opened after it reads the new one */
	n = slurpname(&cl, Froot, Ff, "map", buf, sizeof buf, err, sizeof err);
	eqv("a fid opened after the publish reads the new map", n,
		strlen(new));
	eqs("... and it is the new map", buf, new);
	free(new);

	/* the renders that come out of the parse followed it */
	n = slurpname(&cl, Froot, Ff, "stale", buf, sizeof buf, err,
		sizeof err);
	USED(n);
	eqs("/stale after the publish", buf,
		"stale=n2.0 reporter=n1.0 since=14\n");

	clstop(&cl);
	monsrvfree(ctx);
	devclose(d);
}

/*
 * §8.4's evidence, as store.md §14(56) defines it: a Tread of /map on
 * a role=instance fid answered with a count greater than zero, and
 * nothing else.
 */
static void
tseen(void)
{
	char buf[8192], err[ERRMAX], f[256];
	Monctx *ctx;
	Dev *d;
	Fcall r;
	Cl cl;
	long n;

	clstage = "liveness evidence";
	d = freshdisk();
	seedmaps(d, 11, 12);
	if((ctx = start(d)) == nil){
		devclose(d);
		return;
	}
	clstart(&cl, ctx, Clmsize);

	/* a reader's read of /map moves nothing */
	clattach(&cl, Froot, "peer=n1.0", &r);
	n = slurpname(&cl, Froot, Ff, "map", buf, sizeof buf, err, sizeof err);
	istrue("a reader reads /map", n > 0);
	eqv("a reader's read is not evidence", monsrvlastseen(ctx, "n1.0"), 0);
	clclunk(&cl, Froot, &r);

	/* an admin's read of /map moves nothing either */
	clattach(&cl, Froot, "role=admin,peer=n1.0", &r);
	n = slurpname(&cl, Froot, Ff, "map", buf, sizeof buf, err, sizeof err);
	istrue("an admin reads /map", n > 0);
	eqv("an admin's read is not evidence", monsrvlastseen(ctx, "n1.0"), 0);
	clclunk(&cl, Froot, &r);

	/* an instance's read of a file that is not /map moves nothing */
	clattach(&cl, Froot, "role=instance,peer=n7.7", &r);
	n = slurpname(&cl, Froot, Ff, "status", buf, sizeof buf, err,
		sizeof err);
	istrue("an instance reads /status", n > 0);
	eqv("a read of another file is not evidence",
		monsrvlastseen(ctx, "n7.7"), 0);

	/* ... and a /map read that transfers no bytes moves nothing */
	clwalk1(&cl, Froot, Ff, "map", &r);
	clopen(&cl, Ff, OREAD, &r);
	eqv("an open alone is not evidence", monsrvlastseen(ctx, "n7.7"), 0);
	clread(&cl, Ff, 1000000, 4096, &r);
	eqv("the read past the end answered no bytes", r.count, 0);
	eqv("a read that transfers nothing is not evidence",
		monsrvlastseen(ctx, "n7.7"), 0);
	clread(&cl, Ff, 0, 64, &r);
	istrue("the read at the head answered bytes", r.count > 0);
	istrue("a read that transfers bytes IS evidence",
		monsrvlastseen(ctx, "n7.7") != 0);
	clclunk(&cl, Ff, &r);
	clclunk(&cl, Froot, &r);

	/* the evidence the map can carry reaches /health and /instances */
	clattach(&cl, Froot, Ainsta, &r);
	n = slurpname(&cl, Froot, Ff, "map", buf, sizeof buf, err, sizeof err);
	istrue("an instance reads /map", n > 0);
	istrue("its read is evidence", monsrvlastseen(ctx, "n1.0") != 0);
	eqv("and it is evidence for that iid alone",
		monsrvlastseen(ctx, "n2.0"), 0);

	n = slurpname(&cl, Froot, Ff, "health", buf, sizeof buf, err,
		sizeof err);
	USED(n);
	istrue("/health carries the refresh", strstr(buf,
		"iid=n1.0 lastrefresh=") != nil);
	istrue("... at a time and not zero",
		strstr(buf, "iid=n1.0 lastrefresh=0 ") == nil);
	istrue("/health still shows the other instance silent",
		strstr(buf, "iid=n2.0 lastrefresh=0 silent=0 reports=\n")
			!= nil);

	n = slurpname(&cl, Froot, Ff, "instances", buf, sizeof buf, err,
		sizeof err);
	USED(n);
	istrue("/instances carries lastseen for the instance that read",
		clfield(buf, "uuid", f, sizeof f) != nil);
	istrue("... and it is not zero",
		strstr(buf, "iid=n1.0 ") != nil
		&& strstr(buf, "iid=n1.0 node=n1 addr=tcp!10.0.0.1!17011 "
			"class=ssd registered=0 lastseen=0\n") == nil);
	istrue("... while the instance that did not read is still 0",
		strstr(buf, "iid=n2.0 node=n2 addr=tcp!10.0.0.2!17011 "
			"class=hdd registered=0 lastseen=0\n") != nil);

	clstop(&cl);
	monsrvfree(ctx);
	devclose(d);
}

/*
 * §8.3's ctl framework.  Every verb of both tables is present and
 * answers this server's local `not built' to the role its row names;
 * the other role gets `permission denied', an unknown spelling gets
 * `unknown ctl' and bad arguments get `bad ctl'.
 */
static void
tctl(void)
{
	static struct {
		char	*line;
		int	role;		/* 0 instance, 1 admin */
	} v[] = {
		{"register uuid=a node=b addr=c class=d",	0},
		{"unreachable n2.0",				0},
		{"reachable n2.0",				0},
		{"stale n2.0",					0},
		{"synced n2.0",					0},
		{"healed epoch=3",				0},
		{"rebalanced epoch=3",				0},
		{"propose",					1},
		{"enable n1.0",					1},
		{"disable n1.0",				1},
		{"retire n1.0",					1},
		{"rehome 3f1c node=n2",				1},
		{"setclass n1.0 ssd",				1},
		{"set replicas 3",				1},
		{"commit",					1},
		{"commit force",				1},
		{"abort",					1},
		{"bump",					1},
		{"promote n1.0 force",				1},
		{"forcesync n1.0 from=n2.0",			1},
		{"forceepoch 99",				1},
		{"forceepoch 99 monid=deadbeef",		1},
	};
	char buf[8192], err[ERRMAX], what[160];
	ulong fid[2];
	Monctx *ctx;
	Dev *d;
	Fcall r;
	Cl cl;
	long n;
	int i;

	clstage = "ctl";
	d = freshdisk();
	seedmaps(d, 11, 12);
	if((ctx = start(d)) == nil){
		devclose(d);
		return;
	}
	clstart(&cl, ctx, Clmsize);
	clattach(&cl, Froot, Ainsta, &r);
	clattach(&cl, Froot2, Aadmina, &r);
	fid[0] = Ff;
	fid[1] = Ff2;
	clwalk1(&cl, Froot, fid[0], "ctl", &r);
	clopen(&cl, fid[0], OWRITE, &r);
	clwalk1(&cl, Froot2, fid[1], "ctl", &r);
	clopen(&cl, fid[1], OWRITE, &r);

	for(i = 0; i < nelem(v); i++){
		snprint(what, sizeof what, "%#q on its own role's fid",
			v[i].line);
		eqs(what, ctlsay(&cl, fid[v[i].role], v[i].line, err,
			sizeof err), "shoalmon: not built");
		snprint(what, sizeof what, "%#q on the other role's fid",
			v[i].line);
		eqs(what, ctlsay(&cl, fid[1 - v[i].role], v[i].line, err,
			sizeof err), "permission denied");
	}

	eqs("an unknown verb", ctlsay(&cl, fid[1], "frobnicate", err,
		sizeof err), "unknown ctl");
	eqs("an unknown verb on an instance fid",
		ctlsay(&cl, fid[0], "frobnicate", err, sizeof err),
		"unknown ctl");
	eqs("a verb with too few arguments",
		ctlsay(&cl, fid[1], "enable", err, sizeof err), "bad ctl");
	eqs("a verb with too many arguments",
		ctlsay(&cl, fid[1], "enable n1.0 n2.0", err, sizeof err),
		"bad ctl");
	eqs("an instance verb with too few arguments",
		ctlsay(&cl, fid[0], "healed", err, sizeof err), "bad ctl");
	eqs("a verb with no arguments where none are taken",
		ctlsay(&cl, fid[1], "abort extra", err, sizeof err),
		"bad ctl");
	eqs("a trailing newline is accepted",
		ctlsay(&cl, fid[1], "propose\n", err, sizeof err),
		"shoalmon: not built");
	eqs("two lines in one write",
		ctlsay(&cl, fid[1], "propose\nabort", err, sizeof err),
		"bad ctl");
	eqs("a line of whitespace",
		ctlsay(&cl, fid[1], "   ", err, sizeof err), "bad ctl");
	eqs("an empty write",
		ctlsay(&cl, fid[1], "", err, sizeof err), "bad ctl");
	clclunk(&cl, fid[0], &r);
	clclunk(&cl, fid[1], &r);

	/* the reads of the two write surfaces */
	n = slurpname(&cl, Froot2, Ff, "ctl", buf, sizeof buf, err,
		sizeof err);
	eqv("/ctl reads as no bytes", n, 0);
	n = slurpname(&cl, Froot2, Ff, "map.next", buf, sizeof buf, err,
		sizeof err);
	eqv("/map.next reads as no bytes", n, 0);

	/* and /map.next's write, which the next unit builds */
	clwalk1(&cl, Froot2, Ff, "map.next", &r);
	clopen(&cl, Ff, OWRITE, &r);
	eqs("a write to /map.next", ctlsay(&cl, Ff, "map=t epoch=13\n", err,
		sizeof err), "shoalmon: not built");
	clclunk(&cl, Ff, &r);

	clstop(&cl);
	monsrvfree(ctx);
	devclose(d);
}

/*
 * The shutdown, with fids open: the service loop ends when the request
 * pipe closes, Srv.end closes the slot store, and lib9p then frees the
 * fid pool over fids that are still holding their snapshots.  Nothing
 * a fid holds reaches the store (mon/dat.h), so the order is safe —
 * which is what this case is here to hold to.
 */
static void
tshutdown(void)
{
	char buf[8192];
	Monctx *ctx;
	Dev *d;
	Fcall r;
	Cl cl;
	long n;
	int i;

	clstage = "shutdown";
	d = freshdisk();
	seedmaps(d, 11, 13);
	if((ctx = start(d)) == nil){
		devclose(d);
		return;
	}
	clstart(&cl, ctx, Clmsize);
	clattach(&cl, Froot, Aadmina, &r);
	clwalk1(&cl, Froot, Ff, "map", &r);
	clopen(&cl, Ff, OREAD, &r);
	n = clslurp(&cl, Ff, buf, sizeof buf);
	istrue("the fid left open has read its snapshot", n > 0);
	clwalk1(&cl, Froot, Ff2, "maps", &r);
	clopen(&cl, Ff2, OREAD, &r);
	clwalk1(&cl, Froot, Ff3, "status", &r);
	clopen(&cl, Ff3, OREAD, &r);

	clhangup(&cl);
	istrue("the service loop ends with fids open", clwaitend(&cl, 10000));
	clclose(&cl);
	for(i = 0; i < 400 && !monsrvreleased(ctx); i++)
		sleep(5);
	istrue("lib9p lets go of the service", monsrvreleased(ctx));
	monsrvfree(ctx);
	devclose(d);
}

void
threadmain(int argc, char **argv)
{
	USED(argc);
	USED(argv);
	quotefmtinstall();		/* the FAIL lines quote what they got */
	clwatchms = 120*1000;		/* this program's own budget */
	clwatchon();

	tempty();
	tbadmap();
	tattach();
	ttree();
	tmatrix();
	tmap();
	tmaps();
	tfiles();
	tsnapshot();
	tseen();
	tctl();
	tshutdown();

	clwatchoff();
	if(fails > 0){
		fprint(2, "montreetest: %d of %d checks failed\n", fails,
			checks);
		threadexitsall("fail");
	}
	print("montreetest: %d checks ok\n", checks);
	threadexitsall(nil);
}
