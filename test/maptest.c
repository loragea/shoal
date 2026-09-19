#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"

/*
 * T1: the cluster map, docs/design/layer-a.md §3's format and
 * validation, §4's placement, §5.2's witness set, §6.3's adoption
 * decision and §6.4's fence state.  No 9P, no disk, no procs: every
 * function under test is pure and the only input is text.
 *
 * The placement known-answer vectors below were computed outside this
 * codebase from §4.2 and §4.3, with Python's
 * hashlib.blake2s(digest_size=32); the derivation is written out at
 * tvectors().
 */

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

/*
 * The vector cluster.  Three nodes; n1 and n3 carry two instances
 * each, so both HRW rounds do real work.  Header values are §3.2's
 * defaults, which satisfy leasems > pollms, deadms > leasems and
 * replms < deadms.
 */
static char hdr[] =
	"# the vector cluster\n"
	"map=vec epoch=7\n"
	"\tmonid=00112233445566778899aabbccddeeff\n"
	"\tobjmax=16777216 blksz=16384 replicas=2\n"
	"\tcsumalg=blake2s256 placehash=blake2s256-64\n"
	"\tpollms=1000 leasems=3000 replms=1000 deadms=10000\n"
	"\toutmins=60 tombdays=7 mincopies=1 retain=8\n"
	"\n";

static char allin[] =
	"instance=n1.0 onnode=n1 addr=tcp!10.0.0.1!17011\n"
	"\tuuid=0000000000000000000000000000000a\n"
	"\tclass=ssd weight=100 status=in up=yes since=7 fenced=no\n"
	"instance=n1.1 onnode=n1 addr=tcp!10.0.0.1!17012\n"
	"\tuuid=0000000000000000000000000000000b\n"
	"\tclass=ssd weight=100 status=in up=yes since=7 fenced=no\n"
	"instance=n2.0 onnode=n2 addr=tcp!10.0.0.2!17011\n"
	"\tuuid=0000000000000000000000000000000c\n"
	"\tclass=ssd weight=100 status=in up=yes since=7 fenced=no\n"
	"instance=n3.0 onnode=n3 addr=tcp!10.0.0.3!17011\n"
	"\tuuid=0000000000000000000000000000000d\n"
	"\tclass=ssd weight=100 status=in up=yes since=7 fenced=no\n"
	"instance=n3.2 onnode=n3 addr=tcp!10.0.0.3!17013\n"
	"\tuuid=0000000000000000000000000000000e\n"
	"\tclass=ssd weight=100 status=in up=yes since=7 fenced=no\n";

/* build a map text from the header, a replicas override and records */
static Cmap*
build(char *rep, char *epoch, char *body)
{
	char *t;
	Cmap *m;
	int n;

	n = strlen(hdr) + strlen(body) + 256;
	if((t = malloc(n)) == nil)
		sysfatal("malloc: %r");
	snprint(t, n, "%s%s", hdr, body);
	if(rep != nil){
		char *p;
		/* replace "replicas=2" with the wanted count */
		if((p = strstr(t, "replicas=2")) == nil)
			sysfatal("no replicas in header");
		memmove(p + strlen("replicas="), rep, 1);
	}
	if(epoch != nil){
		char *p;
		if((p = strstr(t, "epoch=7")) == nil)
			sysfatal("no epoch in header");
		memmove(p + strlen("epoch="), epoch, 1);
	}
	if((m = mapparse(t, strlen(t))) == nil)
		sysfatal("mapparse of the vector map: %r");
	free(t);
	return m;
}

static Cmap*
vecmap(void)
{
	return build(nil, nil, allin);
}

/* ------------------------------------------------------------------ */
/* §3.1–§3.3: what parses and what answers `bad map'                   */

typedef struct Case Case;
struct Case
{
	char	*name;
	char	*text;
	char	*want;	/* nil: must parse; else a detail it must contain */
};

/*
 * layer-a §3.1's example map, VERBATIM: this text must equal the one
 * printed there byte for byte with the code block's four-space indent
 * stripped, comment line and all.  The spec's one complete example is
 * an input a conforming parser must accept, so the test reads it from
 * the doc rather than paraphrasing it; change one and change both.
 */
static char good[] =
	"# shoal cluster map\n"
	"map=cluster0 epoch=41\n"
	"    monid=8c1d0f5a9b2e47c3a6d180fe37b45219\n"
	"    objmax=16777216 blksz=16384 replicas=2\n"
	"    csumalg=blake2s256 placehash=blake2s256-64\n"
	"    pollms=1000 leasems=3000 replms=1000 deadms=10000\n"
	"    outmins=60 tombdays=7 mincopies=1 retain=8\n"
	"\n"
	"node=n2\n"
	"\n"
	"instance=n2.1 onnode=n2\n"
	"    addr=tcp!10.0.0.2!17011\n"
	"    uuid=3f1c9a20b47e4d18a0c6e5721b93df04\n"
	"    class=ssd weight=100\n"
	"    status=in up=yes since=41 fenced=no\n"
	"\n"
	"node=n5\n"
	"\n"
	"instance=n5.0 onnode=n5\n"
	"    addr=tcp!10.0.0.5!17011\n"
	"    uuid=5b9e13c74a0d482fb6318ce2d05a7f16\n"
	"    class=hdd weight=100\n"
	"    status=out up=no since=39 fenced=no\n"
	"\n"
	"stale=n5.0 reporter=n2.1 since=39\n";

/*
 * Each rejection names one rule.  The expected detail is a substring,
 * so the test pins the rule that fired and not the wording.
 */
static Case cases[] =
{
	{ "example",		good,			nil },
	{ "nomap",
	  "node=n2\ninstance=n2.1 onnode=n2 addr=a uuid=3f1c9a20b47e4d18a0c6e5721b93df04 status=in up=yes\n",
	  "no map record" },
	{ "twomaps",		nil,			"two map records" },
	{ "notattrvalue",	nil,			"is not attr=value" },
	{ "indentfirst",	"\tepoch=1\n",		"indented line begins" },
	{ "badbyte",		nil,			"7-bit printable" },
	{ "noepoch",		nil,			"has no epoch=" },
	{ "dupepoch",		nil,			"epoch given twice" },
	{ "shortmonid",		nil,			"monid is not 32" },
	{ "uppermonid",		nil,			"monid is not 32" },
	{ "objmaxsmall",	nil,			"objmax" },
	{ "objmaxodd",		nil,			"objmax" },
	{ "blksznotpow2",	nil,			"blksz" },
	{ "replicas0",		nil,			"replicas 0" },
	{ "replicasbig",	nil,			"replicas 65" },
	{ "csumalg",		nil,			"csumalg" },
	{ "placehash",		nil,			"placehash" },
	{ "placerulezones",	nil,			"placerule zones" },
	{ "leasenotoverpoll",	nil,			"leasems" },
	{ "deadnotoverlease",	nil,			"deadms" },
	{ "replnotunderdead",	nil,			"replms" },
	{ "retainlow",		nil,			"retain 1" },
	{ "iidnodot",		nil,			"not <node>.<index>" },
	{ "iidleadzero",	nil,			"not <node>.<index>" },
	{ "onnodewrong",	nil,			"is not the iid's node" },
	{ "nouuid",		nil,			"no uuid=" },
	{ "noaddr",		nil,			"no addr=" },
	{ "nostatus",		nil,			"no status=" },
	{ "noup",		nil,			"no up=" },
	{ "baduuid",		nil,			"uuid is not 32" },
	{ "weight50",		nil,			"weight 50" },
	{ "badstatus",		nil,			"status gone" },
	{ "badup",		nil,			"up maybe" },
	{ "deadup",		nil,			"status=dead with up=yes" },
	{ "deadfenced",		nil,			"status=dead with fenced" },
	{ "newheal",		nil,			"status=new with up=heal" },
	{ "fencedup",		nil,			"fenced=yes with up=yes" },
	{ "dupinst",		nil,			"two instance records" },
	{ "dupnode",		nil,			"two node records" },
	{ "zonesplit",		nil,			"zones" },
	{ "stalenoreporter",	nil,			"no reporter=" },
	{ "staledangling",	nil,			"names no instance" },
	{ "stalebadreporter",	nil,			"names no instance" },
	{ "dupstale",		nil,			"two stale records" },
	{ "indentedcomment",	nil,			"is not attr=value" },
	{ "selfmark",		nil,		"reporter is the subject" },
};

/* one instance record, spelled from parts, for the rejection cases */
static char*
inst(char *iid, char *rest)
{
	static char buf[512];

	snprint(buf, sizeof buf,
		"instance=%s onnode=n2 addr=tcp!a!1 "
		"uuid=3f1c9a20b47e4d18a0c6e5721b93df04 %s\n", iid, rest);
	return buf;
}

static char*
withinst(char *rest)
{
	static char buf[4096];

	snprint(buf, sizeof buf, "%s%s", hdr, inst("n2.1", rest));
	return buf;
}

static char*
withhdr(char *body)
{
	static char buf[4096];

	snprint(buf, sizeof buf, "%s%s", hdr, body);
	return buf;
}

/* a header with one attribute line replaced */
static char*
hdrwith(char *old, char *new)
{
	static char buf[4096];
	char *p;
	int n;

	if((p = strstr(hdr, old)) == nil)
		sysfatal("hdrwith: no %s", old);
	n = p - hdr;
	memmove(buf, hdr, n);
	snprint(buf + n, sizeof buf - n, "%s%s", new, p + strlen(old));
	return buf;
}

/*
 * The rejection texts share static buffers, so each is built as it is
 * about to be parsed rather than all of them up front.
 */
static char*
casetext(Case *c)
{
	static char twomaps[4096], badbyte[4096];

	if(c->text != nil)
		return c->text;
	if(strcmp(c->name, "notattrvalue") == 0)
		return withhdr("instance\n");
	if(strcmp(c->name, "noepoch") == 0)
		return hdrwith("map=vec epoch=7\n", "map=vec\n");
	if(strcmp(c->name, "dupepoch") == 0)
		return hdrwith("map=vec epoch=7\n",
				"map=vec epoch=7 epoch=8\n");
	if(strcmp(c->name, "shortmonid") == 0)
		return hdrwith(
				"monid=00112233445566778899aabbccddeeff",
				"monid=00112233");
	if(strcmp(c->name, "uppermonid") == 0)
		return hdrwith(
				"monid=00112233445566778899aabbccddeeff",
				"monid=00112233445566778899AABBCCDDEEFF");
	if(strcmp(c->name, "objmaxsmall") == 0)
		return hdrwith("objmax=16777216", "objmax=65536");
	if(strcmp(c->name, "objmaxodd") == 0)
		return hdrwith("objmax=16777216", "objmax=16777217");
	if(strcmp(c->name, "blksznotpow2") == 0)
		return hdrwith("blksz=16384", "blksz=16000");
	if(strcmp(c->name, "replicas0") == 0)
		return hdrwith("replicas=2", "replicas=0");
	if(strcmp(c->name, "replicasbig") == 0)
		return hdrwith("replicas=2", "replicas=65");
	if(strcmp(c->name, "csumalg") == 0)
		return hdrwith("csumalg=blake2s256",
				"csumalg=sha256");
	if(strcmp(c->name, "placehash") == 0)
		return hdrwith("placehash=blake2s256-64",
				"placehash=murmur3");
	if(strcmp(c->name, "placerulezones") == 0)
		return hdrwith("retain=8",
				"retain=8 placerule=zones");
	if(strcmp(c->name, "leasenotoverpoll") == 0)
		return hdrwith("leasems=3000", "leasems=1000");
	if(strcmp(c->name, "deadnotoverlease") == 0)
		return hdrwith("deadms=10000", "deadms=3000");
	if(strcmp(c->name, "replnotunderdead") == 0)
		return hdrwith("replms=1000", "replms=10000");
	if(strcmp(c->name, "retainlow") == 0)
		return hdrwith("retain=8", "retain=1");
	if(strcmp(c->name, "iidnodot") == 0)
		return withhdr(inst("n2", "status=in up=yes"));
	if(strcmp(c->name, "iidleadzero") == 0)
		return withhdr(inst("n2.01", "status=in up=yes"));
	if(strcmp(c->name, "onnodewrong") == 0)
		return withhdr(
				"instance=n5.1 onnode=n2 addr=a "
				"uuid=3f1c9a20b47e4d18a0c6e5721b93df04 "
				"status=in up=yes\n");
	if(strcmp(c->name, "nouuid") == 0)
		return withhdr("instance=n2.1 onnode=n2 "
				"addr=a status=in up=yes\n");
	if(strcmp(c->name, "noaddr") == 0)
		return withhdr("instance=n2.1 onnode=n2 "
				"uuid=3f1c9a20b47e4d18a0c6e5721b93df04 "
				"status=in up=yes\n");
	if(strcmp(c->name, "nostatus") == 0)
		return withinst("up=yes");
	if(strcmp(c->name, "noup") == 0)
		return withinst("status=in");
	if(strcmp(c->name, "baduuid") == 0)
		return withhdr("instance=n2.1 onnode=n2 addr=a "
				"uuid=zz1c9a20b47e4d18a0c6e5721b93df04 "
				"status=in up=yes\n");
	if(strcmp(c->name, "weight50") == 0)
		return withinst("status=in up=yes weight=50");
	if(strcmp(c->name, "badstatus") == 0)
		return withinst("status=gone up=yes");
	if(strcmp(c->name, "badup") == 0)
		return withinst("status=in up=maybe");
	if(strcmp(c->name, "deadup") == 0)
		return withinst("status=dead up=yes");
	if(strcmp(c->name, "deadfenced") == 0)
		return withinst("status=dead up=no fenced=yes");
	if(strcmp(c->name, "newheal") == 0)
		return withinst("status=new up=heal");
	if(strcmp(c->name, "fencedup") == 0)
		return withinst("status=in up=yes fenced=yes");
	if(strcmp(c->name, "dupinst") == 0)
		return withhdr(
				"instance=n2.1 onnode=n2 addr=a "
				"uuid=3f1c9a20b47e4d18a0c6e5721b93df04 "
				"status=in up=yes\n"
				"instance=n2.1 onnode=n2 addr=b "
				"uuid=3f1c9a20b47e4d18a0c6e5721b93df05 "
				"status=in up=yes\n");
	if(strcmp(c->name, "dupnode") == 0)
		return withhdr("node=n2\nnode=n2\n");
	if(strcmp(c->name, "zonesplit") == 0)
		return withhdr(
				"instance=n2.1 onnode=n2 addr=a "
				"uuid=3f1c9a20b47e4d18a0c6e5721b93df04 "
				"status=in up=yes zone=east\n"
				"instance=n2.2 onnode=n2 addr=b "
				"uuid=3f1c9a20b47e4d18a0c6e5721b93df05 "
				"status=in up=yes zone=west\n");
	if(strcmp(c->name, "stalenoreporter") == 0)
		return withhdr(
				"instance=n2.1 onnode=n2 addr=a "
				"uuid=3f1c9a20b47e4d18a0c6e5721b93df04 "
				"status=in up=yes\nstale=n2.1 since=3\n");
	if(strcmp(c->name, "staledangling") == 0)
		return withhdr(
				"instance=n2.1 onnode=n2 addr=a "
				"uuid=3f1c9a20b47e4d18a0c6e5721b93df04 "
				"status=in up=yes\n"
				"stale=n9.1 reporter=n2.1 since=3\n");
	if(strcmp(c->name, "stalebadreporter") == 0)
		return withhdr(
				"instance=n2.1 onnode=n2 addr=a "
				"uuid=3f1c9a20b47e4d18a0c6e5721b93df04 "
				"status=in up=yes\n"
				"stale=n2.1 reporter=n9.9 since=3\n");
	if(strcmp(c->name, "dupstale") == 0)
		return withhdr(
				"instance=n2.1 onnode=n2 addr=a "
				"uuid=3f1c9a20b47e4d18a0c6e5721b93df04 "
				"status=in up=yes\n"
				"instance=n2.2 onnode=n2 addr=b "
				"uuid=3f1c9a20b47e4d18a0c6e5721b93df05 "
				"status=in up=yes\n"
				"stale=n2.1 reporter=n2.2 since=3\n"
				"stale=n2.1 reporter=n2.2 since=4\n");
	if(strcmp(c->name, "selfmark") == 0)
		return withhdr(
				"instance=n2.1 onnode=n2 addr=a "
				"uuid=3f1c9a20b47e4d18a0c6e5721b93df04 "
				"status=in up=yes\n"
				"stale=n2.1 reporter=n2.1 since=3\n");
	if(strcmp(c->name, "indentedcomment") == 0)
		return withhdr("\t# an indented # is a continuation, not a "
				"comment\n");
	if(strcmp(c->name, "twomaps") == 0){
		snprint(twomaps, sizeof twomaps, "%smap=other epoch=8\n", hdr);
		return twomaps;
	}
	if(strcmp(c->name, "badbyte") != 0)
		sysfatal("case %s has no text", c->name);
	snprint(badbyte, sizeof badbyte, "%s", hdr);
	badbyte[10] = 0x07;
	return badbyte;
}

/*
 * An error string reaches a caller's Rerror body, which §0 makes
 * 7-bit ASCII and ERRMAX-bounded, and which this parser would itself
 * reject a byte of in a map text.  So every detail is ASCII: no
 * section mark, no UTF-8 of any kind.
 */
static void
tascii(char *name, char *err)
{
	int i;

	for(i = 0; err[i] != '\0'; i++)
		if((uchar)err[i] < 0x20 || (uchar)err[i] > 0x7e){
			fail("%s: error byte %#.2ux in `%s' is not 7-bit "
				"printable ascii", name, (uchar)err[i], err);
			return;
		}
}

static void
tmatrix(void)
{
	char err[ERRMAX];
	Cmap *m;
	int i;

	for(i = 0; i < nelem(cases); i++){
		char *t;

		werrstr("");
		t = casetext(&cases[i]);
		m = mapparse(t, strlen(t));
		if(cases[i].want == nil){
			if(m == nil)
				fail("%s: rejected: %r", cases[i].name);
			mapfree(m);
			checks++;
			continue;
		}
		if(m != nil){
			fail("%s: accepted, wanted `%s'", cases[i].name,
				cases[i].want);
			mapfree(m);
			checks++;
			continue;
		}
		rerrstr(err, sizeof err);
		if(strncmp(err, "bad map: ", 9) != 0)
			fail("%s: error `%s' is not a bad map (§2.6)",
				cases[i].name, err);
		else if(strstr(err, cases[i].want) == nil)
			fail("%s: error `%s' does not name `%s'",
				cases[i].name, err, cases[i].want);
		tascii(cases[i].name, err);
		checks += 2;
	}
}

/* the header of the ratified example, read back field by field (§3.2) */
static void
theader(void)
{
	Cmap *m;
	Cinst *i;

	if((m = mapparse(good, strlen(good))) == nil){
		fail("example: %r");
		return;
	}
	if(strcmp(m->name, "cluster0") != 0)
		fail("name %s", m->name);
	if(m->epoch != 41 || m->objmax != 16777216 || m->blksz != 16384 ||
	   m->replicas != 2)
		fail("epoch/objmax/blksz/replicas");
	if(strcmp(m->monid, "8c1d0f5a9b2e47c3a6d180fe37b45219") != 0)
		fail("monid %s", m->monid);
	if(m->pollms != 1000 || m->leasems != 3000 || m->replms != 1000 ||
	   m->deadms != 10000)
		fail("timers");
	if(m->outmins != 60 || m->tombdays != 7 || m->mincopies != 1 ||
	   m->retain != 8)
		fail("outmins/tombdays/mincopies/retain");
	if(strcmp(m->placerule, "nodes") != 0)
		fail("placerule defaults to %s, not nodes", m->placerule);
	if(m->nnode != 2 || m->ninst != 2 || m->nstale != 1)
		fail("records: %d node %d instance %d stale", m->nnode,
			m->ninst, m->nstale);
	checks += 8;

	if((i = mapinst(m, "n2.1")) == nil){
		fail("no n2.1");
		mapfree(m);
		return;
	}
	if(strcmp(i->node, "n2") != 0 || i->idx != 1)
		fail("iid split %s %lud", i->node, i->idx);
	if(strcmp(i->addr, "tcp!10.0.0.2!17011") != 0)
		fail("addr %s", i->addr);
	if(strcmp(i->class, "ssd") != 0 || i->weight != 100)
		fail("class/weight");
	if(strcmp(i->zone, "default") != 0)
		fail("absent zone is %s, not default (§3.3)", i->zone);
	if(i->status != Sin || i->up != Uyes || i->fenced != 0 ||
	   i->since != 41)
		fail("status/up/fenced/since");
	if(m->nstale != 1 || strcmp(m->stale[0].subject, "n5.0") != 0 ||
	   strcmp(m->stale[0].reporter, "n2.1") != 0 ||
	   m->stale[0].since != 39)
		fail("stale record");
	checks += 6;

	/*
	 * §7.1's mark names an instance the map carries, which is what
	 * makes the example a map this parser accepts: the subject is
	 * the second instance record, out and down since epoch 39.
	 */
	if((i = mapinst(m, "n5.0")) == nil){
		fail("no n5.0, the subject of the example's stale mark");
		mapfree(m);
		return;
	}
	if(strcmp(i->node, "n5") != 0 || i->idx != 0)
		fail("n5.0 iid split %s %lud", i->node, i->idx);
	if(strcmp(i->addr, "tcp!10.0.0.5!17011") != 0)
		fail("n5.0 addr %s", i->addr);
	if(strcmp(i->uuid, "5b9e13c74a0d482fb6318ce2d05a7f16") != 0)
		fail("n5.0 uuid %s", i->uuid);
	if(i->status != Sout || i->up != Uno || i->fenced != 0 ||
	   i->since != 39)
		fail("n5.0 status/up/fenced/since");
	if(mapinst(m, m->stale[0].subject) != i)
		fail("the mark's subject is not the instance record");
	checks += 5;
	mapfree(m);
}

/*
 * §3.1: a reader MUST ignore attributes it does not know and records
 * whose kind it does not know — including their continuation lines,
 * which is what keeps §8.7's additions additive.
 */
static void
tunknown(void)
{
	static char t[] =
		"map=vec epoch=7 futurething=42\n"
		"\tmonid=00112233445566778899aabbccddeeff\n"
		"\tobjmax=16777216 blksz=16384 replicas=2\n"
		"\tcsumalg=blake2s256 placehash=blake2s256-64\n"
		"\tpollms=1000 leasems=3000 replms=1000 deadms=10000\n"
		"\toutmins=60 tombdays=7 mincopies=1 retain=8\n"
		"quorum=q1 members=3\n"
		"\tweight=7 status=nonsense up=nonsense\n"
		"instance=n2.1 onnode=n2 addr=tcp!a!1 later=yes\n"
		"\tuuid=3f1c9a20b47e4d18a0c6e5721b93df04\n"
		"\tstatus=in up=yes rack=b12\n";
	Cmap *m;

	if((m = mapparse(t, strlen(t))) == nil){
		fail("unknown kinds and attributes: %r");
		return;
	}
	if(m->ninst != 1 || mapinst(m, "n2.1") == nil)
		fail("unknown: instance lost");
	if(m->epoch != 7)
		fail("unknown: header lost");
	checks += 2;
	mapfree(m);
}

/*
 * §0: a comment begins with a `#' AT THE START OF A LINE, and
 * comments and blank lines are ignored and do not end a record.  A
 * line of nothing but white space is blank; an indented `#' is a
 * continuation line whose first token is not attr=value, which §3's
 * normative grammar makes bad grammar and §8.1 makes `bad map'.
 */
static void
tcomments(void)
{
	static char t[] =
		"# leading comment\n"
		"\n"
		"map=vec epoch=7\n"
		"# a comment inside the record\n"
		"\tmonid=00112233445566778899aabbccddeeff\n"
		"\t\t\n"
		"\tobjmax=16777216 blksz=16384 replicas=2\n"
		"\tcsumalg=blake2s256 placehash=blake2s256-64\n"
		"\tpollms=1000 leasems=3000 replms=1000 deadms=10000\n"
		"\toutmins=60 tombdays=7 mincopies=1 retain=8\n";
	Cmap *m;

	if((m = mapparse(t, strlen(t))) == nil){
		fail("comments: %r");
		return;
	}
	if(m->blksz != 16384 || m->retain != 8)
		fail("comments: a comment or a blank line ended the record");
	checks++;
	mapfree(m);
}

/*
 * What a record may leave out, and what it is then.  §3.2 gives the
 * header no defaults except the two layer-a prints itself: `retain',
 * which §8.2 writes as "(default 8)", and `placerule', whose only v1
 * value is `nodes'.  §3.3 defaults `zone' to `default' and the rest
 * of an instance's optional attributes are conservative: weight 100
 * (the only value §4.4 accepts), fenced no ("no otherwise"), class
 * empty, since 0.
 */
static void
tdefaults(void)
{
	static char t[] =
		"map=vec epoch=7\n"
		"\tmonid=00112233445566778899aabbccddeeff\n"
		"\tobjmax=16777216 blksz=16384 replicas=2\n"
		"\tcsumalg=blake2s256 placehash=blake2s256-64\n"
		"\tpollms=1000 leasems=3000 replms=1000 deadms=10000\n"
		"\toutmins=60 tombdays=7 mincopies=1\n"
		"instance=n2.1 onnode=n2 addr=tcp!a!1\n"
		"\tuuid=3f1c9a20b47e4d18a0c6e5721b93df04\n"
		"\tstatus=in up=yes\n";
	Cmap *m;
	Cinst *i;

	if((m = mapparse(t, strlen(t))) == nil){
		fail("defaults: a map with no retain= was rejected: %r");
		return;
	}
	if(m->retain != 8)
		fail("defaults: an absent retain is %lud, not 8", m->retain);
	if(strcmp(m->placerule, "nodes") != 0)
		fail("defaults: an absent placerule is %s", m->placerule);
	checks += 2;
	if((i = mapinst(m, "n2.1")) == nil)
		fail("defaults: no n2.1");
	else{
		if(strcmp(i->zone, "default") != 0)
			fail("defaults: an absent zone is %s", i->zone);
		if(i->weight != 100 || i->fenced != 0 || i->since != 0 ||
		   i->class[0] != '\0')
			fail("defaults: weight %lud fenced %d since %llud "
				"class `%s'", i->weight, i->fenced, i->since,
				i->class);
		checks += 2;
	}
	mapfree(m);
}

/*
 * A thousand blank lines in front of §3.1's example, which the record
 * arrays are sized against: only a line that can start a record is
 * counted, so the map still parses whole and every record is there.
 * Nothing here asserts a heap size; what it asserts is that the
 * arrays are big enough for the records the text really holds.
 */
static void
tblank(void)
{
	char *t;
	Cmap *m;
	int i, n;

	n = 1000 + strlen(good) + 1;
	if((t = malloc(n)) == nil)
		sysfatal("malloc: %r");
	for(i = 0; i < 1000; i++)
		t[i] = '\n';
	strcpy(t + 1000, good);
	if((m = mapparse(t, strlen(t))) == nil)
		fail("1000 blank lines: %r");
	else{
		if(m->epoch != 41 || m->nnode != 2 || m->ninst != 2 ||
		   m->nstale != 1)
			fail("1000 blank lines: %d node %d instance %d "
				"stale", m->nnode, m->ninst, m->nstale);
		if(mapinst(m, "n2.1") == nil || mapinst(m, "n5.0") == nil)
			fail("1000 blank lines: an instance is missing");
		mapfree(m);
	}
	checks++;
	free(t);
}

/*
 * §3.3's spellings, which render an instance's state wherever a
 * caller shows one.  Reached indirectly through two of instdone's
 * error details, which is not a test of them.
 */
static void
tnames(void)
{
	if(strcmp(statusname(Snew), "new") != 0 ||
	   strcmp(statusname(Sin), "in") != 0 ||
	   strcmp(statusname(Sout), "out") != 0 ||
	   strcmp(statusname(Sdead), "dead") != 0)
		fail("statusname: %s %s %s %s", statusname(Snew),
			statusname(Sin), statusname(Sout),
			statusname(Sdead));
	if(strcmp(upname(Uyes), "yes") != 0 ||
	   strcmp(upname(Uheal), "heal") != 0 ||
	   strcmp(upname(Uno), "no") != 0)
		fail("upname: %s %s %s", upname(Uyes), upname(Uheal),
			upname(Uno));
	checks += 2;
	/* neither invents a spelling for a value §3.3 does not define */
	if(strcmp(statusname(-1), "?") != 0 || strcmp(upname(99), "?") != 0)
		fail("names: an undefined value renders as `%s'/`%s'",
			statusname(-1), upname(99));
	checks++;
}

/* ------------------------------------------------------------------ */
/* §4: placement                                                       */

/*
 * Known answers, computed outside this codebase.  H(x) is the first 8
 * bytes of BLAKE2s-256(x) big-endian (§4.2); the node score is
 * H(oid||0x00||'N'||nid) and the instance score H(oid||0x00||'D'||iid)
 * (§4.2); §4.3 takes the min(R,|V|) highest-scoring nodes in
 * descending order and then the highest-scoring status=in instance on
 * each.  For the vector cluster (nodes n1 n2 n3; n1 carries n1.0 and
 * n1.1, n3 carries n3.0 and n3.2), python3:
 *
 *	import hashlib
 *	H=lambda x: int.from_bytes(
 *		hashlib.blake2s(x,digest_size=32).digest()[:8],'big')
 *	H(b'alpha\0Nn2') -> f140ceae475fdd5c
 *
 * gives, descending:
 *
 *  alpha   n2 f140ceae475fdd5c  n1 54897958874f8a0d  n3 2bf464f2630e1c70
 *          n1: n1.1 60358eee65937274 > n1.0 4626a03ab636d76f
 *          n3: n3.0 93c101f8ba533202 > n3.2 05b7d5be168915a9
 *  beta    n2 aecc88689b95729a  n3 7f1402a759f6673d  n1 33b260ca7de7ac28
 *          n3: n3.0 efdbbb1fe6a96c58 > n3.2 a043970ab154e271
 *          n1: n1.0 fc4f9fa0eed2b746 > n1.1 0168c392f95a98fc
 *  gamma   n2 f5908195fb5bc19c  n1 98a10b6f1be1e1a2  n3 43a915b2f4e32da9
 *          n1: n1.1 d6a6dedeb1325944 > n1.0 ab992eb064d0c096
 *          n3: n3.2 b54263633a503c01 > n3.0 1417491ccc0a0bf5
 *  delta   n3 4e28a030418963b9  n1 42d1c59219a6c30d  n2 3682b583a3c6da93
 *          n3: n3.0 ba4dfbbe91cc7f6d > n3.2 a0810f1e0d13df0d
 *          n1: n1.1 7f09844f59ee43c9 > n1.0 5a7500574db5f804
 *  epsilon n2 de2ee72436c0a329  n1 d773dc300dcb982c  n3 21699ca1612535e2
 *          n1: n1.0 d791e341ed705d29 > n1.1 0ca0a65180f13488
 *          n3: n3.2 b029343d34481d72 > n3.0 35dc09db7c3735a8
 *  zeta    n1 a2e5b8798be7b928  n3 80fe3027dc216c47  n2 7c8a754f32037efa
 *          n1: n1.1 8c949e84ae03992d > n1.0 073488d41f060038
 *          n3: n3.0 ec960684f4687831 > n3.2 3c789498bdf9d71c
 */
typedef struct Pvec Pvec;
struct Pvec
{
	char	*oid;
	uvlong	sn2;		/* H(oid||0||'N'||"n2"), spot-checked */
	char	*p2[2];		/* P at replicas=2 */
	char	*p3[3];		/* P at replicas=3 */
};

static Pvec pvec[] =
{
	{ "alpha",	0xf140ceae475fdd5cULL,
	  { "n2.0", "n1.1" }, { "n2.0", "n1.1", "n3.0" } },
	{ "beta",	0xaecc88689b95729aULL,
	  { "n2.0", "n3.0" }, { "n2.0", "n3.0", "n1.0" } },
	{ "gamma",	0xf5908195fb5bc19cULL,
	  { "n2.0", "n1.1" }, { "n2.0", "n1.1", "n3.2" } },
	{ "delta",	0x3682b583a3c6da93ULL,
	  { "n3.0", "n1.1" }, { "n3.0", "n1.1", "n2.0" } },
	{ "epsilon",	0xde2ee72436c0a329ULL,
	  { "n2.0", "n1.0" }, { "n2.0", "n1.0", "n3.2" } },
	{ "zeta",	0x7c8a754f32037efaULL,
	  { "n1.1", "n3.0" }, { "n1.1", "n3.0", "n2.0" } },
};

static void
tvectors(void)
{
	Cmap *m2, *m3;
	Cinst *p[Maxplace];
	int i, k, np;

	m2 = vecmap();
	m3 = build("3", nil, allin);
	for(i = 0; i < nelem(pvec); i++){
		if(maphash(pvec[i].oid, 'N', "n2") != pvec[i].sn2)
			fail("%s: H(oid||0||N||n2) is %llux, want %llux",
				pvec[i].oid,
				maphash(pvec[i].oid, 'N', "n2"), pvec[i].sn2);
		checks++;

		np = mapplace(m2, pvec[i].oid, p, nelem(p));
		if(np != 2){
			fail("%s: |P| %d at R=2", pvec[i].oid, np);
			continue;
		}
		for(k = 0; k < 2; k++)
			if(strcmp(p[k]->iid, pvec[i].p2[k]) != 0)
				fail("%s: P[%d] is %s, want %s", pvec[i].oid,
					k, p[k]->iid, pvec[i].p2[k]);
		checks++;

		np = mapplace(m3, pvec[i].oid, p, nelem(p));
		if(np != 3){
			fail("%s: |P| %d at R=3", pvec[i].oid, np);
			continue;
		}
		for(k = 0; k < 3; k++)
			if(strcmp(p[k]->iid, pvec[i].p3[k]) != 0)
				fail("%s: R=3 P[%d] is %s, want %s",
					pvec[i].oid, k, p[k]->iid,
					pvec[i].p3[k]);
		checks++;
	}
	mapfree(m2);
	mapfree(m3);
}

/*
 * §4.3's tie-break, which no known-answer vector can reach: it needs
 * two ids whose 64-bit scores collide.  placecmp is the comparison
 * both rounds use, so this exercises the rule where it lives.
 */
static void
ttiebreak(void)
{
	if(placecmp(5, "n1", 4, "n9") <= 0)
		fail("tie-break: the higher score must win");
	if(placecmp(4, "n9", 5, "n1") >= 0)
		fail("tie-break: the lower score must lose");
	checks++;
	if(placecmp(5, "n2", 5, "n1") <= 0)
		fail("tie-break: n2 must beat n1 at an equal score");
	if(placecmp(5, "n1", 5, "n2") >= 0)
		fail("tie-break: n1 must lose to n2 at an equal score");
	checks++;
	/* a prefix loses: the longer id wins (§4.3 step 2) */
	if(placecmp(5, "n1", 5, "n10") >= 0)
		fail("tie-break: n1 must lose to n10, its extension");
	if(placecmp(5, "n10", 5, "n1") <= 0)
		fail("tie-break: n10 must beat n1, its prefix");
	checks++;
	/* the comparison is over unsigned bytes */
	if(placecmp(5, "n~", 5, "nA") <= 0)
		fail("tie-break: bytes compare unsigned");
	if(placecmp(5, "n1", 5, "n1") != 0)
		fail("tie-break: an id ties with itself");
	checks += 2;
}

/*
 * §4.3 step 4: |P| < R is legal and means the object is structurally
 * under-replicated.  Only status=in places (§3.3), so taking a node's
 * only in instance out of placement takes the node out of V.
 */
static void
tunderrep(void)
{
	Cmap *m;
	Cinst *p[Maxplace];
	int i, np;

	/* R=4 over three nodes */
	m = build("4", nil, allin);
	np = mapplace(m, "alpha", p, nelem(p));
	if(np != 3)
		fail("underrep: |P| %d at R=4 over 3 nodes", np);
	if(!mapunderrep(m, "alpha"))
		fail("underrep: R=4 over 3 nodes is not reported");
	checks += 2;
	mapfree(m);

	/*
	 * n2 is the only node with a single instance, so making it
	 * status=out empties n2 from V: two nodes at R=2.
	 */
	m = build(nil, nil,
		"instance=n1.0 onnode=n1 addr=a "
		"uuid=0000000000000000000000000000000a status=in up=yes\n"
		"instance=n1.1 onnode=n1 addr=b "
		"uuid=0000000000000000000000000000000b status=in up=yes\n"
		"instance=n2.0 onnode=n2 addr=c "
		"uuid=0000000000000000000000000000000c status=out up=yes\n"
		"instance=n3.0 onnode=n3 addr=d "
		"uuid=0000000000000000000000000000000d status=in up=yes\n"
		"instance=n3.2 onnode=n3 addr=e "
		"uuid=0000000000000000000000000000000e status=in up=yes\n");
	np = mapplace(m, "alpha", p, nelem(p));
	if(np != 2)
		fail("out: |P| %d with n2 out", np);
	for(i = 0; i < np; i++)
		if(strcmp(p[i]->iid, "n2.0") == 0)
			fail("out: status=out placed");
	/* alpha ranked n1 above n3, and n1's pick is n1.1 */
	if(np == 2 && (strcmp(p[0]->iid, "n1.1") != 0 ||
	   strcmp(p[1]->iid, "n3.0") != 0))
		fail("out: P is %s,%s, want n1.1,n3.0", p[0]->iid, p[1]->iid);
	if(mapunderrep(m, "alpha"))
		fail("out: |P|=2 at R=2 is not under-replicated");
	checks += 4;
	mapfree(m);

	/* nothing in placement at all */
	m = build(nil, nil,
		"instance=n1.0 onnode=n1 addr=a "
		"uuid=0000000000000000000000000000000a status=new up=yes\n");
	if(mapplace(m, "alpha", p, nelem(p)) != 0)
		fail("empty: status=new placed");
	if(mapprimary(m, "alpha") != nil)
		fail("empty: a primary with no placement");
	checks += 2;
	mapfree(m);
}

/*
 * Above 32 nodes mapplace scores over an allocated array instead of
 * its stack one, which the five-instance vector cluster never
 * reaches.  Forty nodes of one instance each; the answers were
 * computed outside this codebase, python3:
 *
 *	import hashlib
 *	H=lambda x: int.from_bytes(
 *		hashlib.blake2s(x,digest_size=32).digest()[:8],'big')
 *	nodes = ["nn%d" % i for i in range(40)]
 *
 * gives, descending by H(oid||0x00||'N'||nid):
 *
 *  alpha  nn9  fd2e952d5c055b95   nn6  f1a208c16492f97f
 *         nn21 f1611eef853ec9be   nn35 eeaee7cb8ac34f20
 *  beta   nn39 fc54bdb6a445fe22   nn24 fb563b3ecfe5986a
 *         nn30 f68f8d053bbc2933   nn5  e3e0340e2442a26c
 *
 * and each node carries exactly one status=in instance, so the
 * instance round has one candidate and takes it.
 */
static Cmap*
bigmap(char *rep)
{
	static char body[40*128];
	char *s, *e;
	int i;

	s = body;
	e = body + sizeof body;
	for(i = 0; i < 40; i++)
		s = seprint(s, e, "instance=nn%d.0 onnode=nn%d addr=tcp!a!%d "
			"uuid=%.32d status=in up=yes\n", i, i, i + 1, i);
	return build(rep, nil, body);
}

static void
tbignodes(void)
{
	static char *alpha3[] = { "nn9.0", "nn6.0", "nn21.0" };
	static char *beta3[] = { "nn39.0", "nn24.0", "nn30.0" };
	Cmap *m;
	Cinst *p[Maxplace], *pr;
	int i, np;

	m = bigmap(nil);
	if(m->npnode != 40)
		fail("40 nodes: |V| is %d", m->npnode);
	np = mapplace(m, "alpha", p, nelem(p));
	if(np != 2 || strcmp(p[0]->iid, alpha3[0]) != 0 ||
	   strcmp(p[1]->iid, alpha3[1]) != 0)
		fail("40 nodes: P(alpha) at R=2 is %s,%s", np > 0 ?
			p[0]->iid : "-", np > 1 ? p[1]->iid : "-");
	np = mapplace(m, "beta", p, nelem(p));
	if(np != 2 || strcmp(p[0]->iid, beta3[0]) != 0 ||
	   strcmp(p[1]->iid, beta3[1]) != 0)
		fail("40 nodes: P(beta) at R=2 is %s,%s", np > 0 ?
			p[0]->iid : "-", np > 1 ? p[1]->iid : "-");
	if((pr = mapprimary(m, "alpha")) == nil ||
	   strcmp(pr->iid, alpha3[0]) != 0)
		fail("40 nodes: the primary for alpha is %s",
			pr == nil ? "none" : pr->iid);
	if(mapunderrep(m, "alpha") != 0)
		fail("40 nodes: |P|=2 at R=2 is under-replicated");
	checks += 5;
	mapfree(m);

	m = bigmap("3");
	np = mapplace(m, "alpha", p, nelem(p));
	if(np != 3)
		fail("40 nodes: |P| %d at R=3", np);
	else
		for(i = 0; i < 3; i++)
			if(strcmp(p[i]->iid, alpha3[i]) != 0)
				fail("40 nodes: R=3 P[%d] is %s, want %s",
					i, p[i]->iid, alpha3[i]);
	np = mapplace(m, "beta", p, nelem(p));
	if(np != 3)
		fail("40 nodes: |P| %d at R=3 for beta", np);
	else
		for(i = 0; i < 3; i++)
			if(strcmp(p[i]->iid, beta3[i]) != 0)
				fail("40 nodes: R=3 P[%d] for beta is %s, "
					"want %s", i, p[i]->iid, beta3[i]);
	/* |P| is the whole count even when out[] is shorter */
	if(mapplace(m, "alpha", p, 1) != 3)
		fail("40 nodes: |P| with nout=1");
	checks += 3;
	mapfree(m);
}

/*
 * §4.3: the serving primary is the first member of P(oid) with
 * up=yes, and P is stable across an up change — a flap promotes the
 * next member without moving a byte.
 */
static void
tprimary(void)
{
	static struct {
		char	*name;
		char	*n20up;		/* n2.0's up, alpha's P[0] */
		char	*n11up;		/* n1.1's up, alpha's P[1] */
		char	*want;		/* nil: object unavailable */
	} t[] = {
		{ "both yes",	"yes",	"yes",	"n2.0" },
		{ "head no",	"no",	"yes",	"n1.1" },
		{ "head heal",	"heal",	"yes",	"n1.1" },
		{ "both no",	"no",	"no",	nil },
		{ "both heal",	"heal",	"heal",	nil },
		{ "tail only",	"no",	"heal",	nil },
		{ "back up",	"yes",	"no",	"n2.0" },
	};
	char body[2048];
	Cmap *m;
	Cinst *p[Maxplace], *pr;
	int i, np;

	for(i = 0; i < nelem(t); i++){
		snprint(body, sizeof body,
			"instance=n1.0 onnode=n1 addr=a "
			"uuid=0000000000000000000000000000000a "
			"status=in up=yes\n"
			"instance=n1.1 onnode=n1 addr=b "
			"uuid=0000000000000000000000000000000b "
			"status=in up=%s\n"
			"instance=n2.0 onnode=n2 addr=c "
			"uuid=0000000000000000000000000000000c "
			"status=in up=%s\n"
			"instance=n3.0 onnode=n3 addr=d "
			"uuid=0000000000000000000000000000000d "
			"status=in up=yes\n"
			"instance=n3.2 onnode=n3 addr=e "
			"uuid=0000000000000000000000000000000e "
			"status=in up=yes\n", t[i].n11up, t[i].n20up);
		m = build(nil, nil, body);

		/* placement does not consult liveness (§4.3) */
		np = mapplace(m, "alpha", p, nelem(p));
		if(np != 2 || strcmp(p[0]->iid, "n2.0") != 0 ||
		   strcmp(p[1]->iid, "n1.1") != 0)
			fail("%s: P moved with up", t[i].name);
		checks++;

		pr = mapprimary(m, "alpha");
		if(t[i].want == nil){
			if(pr != nil)
				fail("%s: primary %s, want none", t[i].name,
					pr->iid);
		}else if(pr == nil)
			fail("%s: no primary, want %s", t[i].name, t[i].want);
		else if(strcmp(pr->iid, t[i].want) != 0)
			fail("%s: primary %s, want %s", t[i].name, pr->iid,
				t[i].want);
		checks++;
		mapfree(m);
	}
}

/* §6.4 F3, and the membership rule for a map that no longer counts us */
static void
tdown(void)
{
	Cmap *m;

	m = build(nil, nil,
		"instance=n1.0 onnode=n1 addr=a "
		"uuid=0000000000000000000000000000000a status=in up=yes\n"
		"instance=n1.1 onnode=n1 addr=b "
		"uuid=0000000000000000000000000000000b status=in up=no\n"
		"instance=n2.0 onnode=n2 addr=c "
		"uuid=0000000000000000000000000000000c status=out up=yes\n"
		"instance=n3.0 onnode=n3 addr=d "
		"uuid=0000000000000000000000000000000d status=dead up=no\n"
		"instance=n3.2 onnode=n3 addr=e "
		"uuid=0000000000000000000000000000000e status=in up=heal\n");
	if(mapdown(m, "n1.0"))
		fail("F3: in/yes is down");
	if(!mapdown(m, "n1.1"))
		fail("F3: up=no is not down");
	if(!mapdown(m, "n2.0"))
		fail("F3: status=out is not down");
	if(mapdown(m, "n3.2"))
		fail("F3: up=heal is down, and F3's list omits it");
	if(!mapdown(m, "n9.9"))
		fail("F3: an instance the map does not carry is not down");
	checks += 5;
	if(!mapmember(m, "n1.1"))
		fail("member: up=no is not a member");
	if(mapmember(m, "n3.0"))
		fail("member: status=dead is a member");
	if(mapmember(m, "n9.9"))
		fail("member: an absent instance is a member");
	checks += 3;
	mapfree(m);
}

/* ------------------------------------------------------------------ */
/* §5.2: the witness set and the up=no skip rule                       */

static int
haswit(Cwit *w, int n, char *iid, int why, int how)
{
	int i;

	for(i = 0; i < n; i++)
		if(strcmp(w[i].inst->iid, iid) == 0)
			return (why == 0 || (w[i].why & why) == why) &&
				(how < 0 || w[i].how == how);
	return 0;
}

/*
 * witblocker walks the entries mapwitness filled, so it takes
 * min(|W|, nout) and not |W| (shoal.h).  Every caller here passes
 * nelem(ws), which is over m->ninst in every map below, but the
 * clamp is where a caller's is.
 */
static Cinst*
blocker(Cwit *w, int n, int nout)
{
	return witblocker(w, n < nout ? n : nout);
}

static int
inwit(Cwit *w, int n, char *iid)
{
	int i;

	for(i = 0; i < n; i++)
		if(strcmp(w[i].inst->iid, iid) == 0)
			return 1;
	return 0;
}

/*
 * The map at E−1 for alpha: n1.1 is status=out there, so n1's only
 * placing instance is n1.0 and P(alpha) at E−1 is n2.0,n1.0 — a
 * placement change that hands n1.1 an object it did not hold.
 */
static char prevbody[] =
	"instance=n1.0 onnode=n1 addr=a "
	"uuid=0000000000000000000000000000000a status=in up=yes\n"
	"instance=n1.1 onnode=n1 addr=b "
	"uuid=0000000000000000000000000000000b status=out up=yes\n"
	"instance=n2.0 onnode=n2 addr=c "
	"uuid=0000000000000000000000000000000c status=in up=yes\n"
	"instance=n3.0 onnode=n3 addr=d "
	"uuid=0000000000000000000000000000000d status=in up=yes\n"
	"instance=n3.2 onnode=n3 addr=e "
	"uuid=0000000000000000000000000000000e status=in up=yes\n";

static void
twitness(void)
{
	Witreq w;
	Cmap *m, *prev, *old;
	Cwit ws[64];
	Cinst *p[Maxplace];
	char *stray[2];
	int n, np;

	prev = build(nil, "6", prevbody);
	if(prev->epoch != 6)
		fail("witness: the E−1 map is at epoch %llud", prev->epoch);
	np = mapplace(prev, "alpha", p, nelem(p));
	if(np != 2 || strcmp(p[1]->iid, "n1.0") != 0)
		fail("witness: P(alpha) at E−1 is not n2.0,n1.0");
	checks++;

	/* clauses 1 and 2, with no strays and no marks */
	m = vecmap();
	memset(&w, 0, sizeof w);
	w.m = m;
	w.prev = prev;
	w.oid = "alpha";
	n = mapwitness(&w, ws, nelem(ws));
	if(n != 3)
		fail("witness: |W| %d, want 3", n);
	if(!haswit(ws, n, "n2.0", Wplace|Wprev, Wquery))
		fail("witness: n2.0 is in P at E and E−1");
	if(!haswit(ws, n, "n1.1", Wplace, Wquery))
		fail("witness: n1.1 is clause 1");
	if(!haswit(ws, n, "n1.0", Wprev, Wquery))
		fail("witness: n1.0 is clause 2");
	if(inwit(ws, n, "n3.0") || inwit(ws, n, "n3.2"))
		fail("witness: an instance in neither placement");
	checks += 4;

	/* clause 3: a locally known stray holder */
	stray[0] = "n3.2";
	stray[1] = "n9.9";		/* not in the map: ignored */
	w.stray = stray;
	w.nstray = 2;
	n = mapwitness(&w, ws, nelem(ws));
	if(n != 4 || !haswit(ws, n, "n3.2", Wstray, Wquery))
		fail("witness: clause 3 stray holder, |W| %d", n);
	checks++;
	w.nstray = 0;

	/*
	 * Clause 2's substitution: when the cached map is not E−1, or
	 * the reconcile pass has not completed, every instance with
	 * status in {new,in,out} stands in for P at E−1.
	 */
	old = build(nil, "5", prevbody);
	w.prev = old;
	n = mapwitness(&w, ws, nelem(ws));
	if(n != 5)
		fail("witness: substitution over a map at E−2 gives %d", n);
	checks++;
	w.prev = prev;
	w.subst = 1;
	n = mapwitness(&w, ws, nelem(ws));
	if(n != 5)
		fail("witness: substitution while reconcile is open gives %d",
			n);
	checks++;
	w.subst = 0;
	w.prev = nil;
	n = mapwitness(&w, ws, nelem(ws));
	if(n != 5)
		fail("witness: substitution with no cached map gives %d", n);
	checks++;
	mapfree(old);
	mapfree(m);
	mapfree(prev);
}

/* clause 4 and the skip rule, with the map's own stale ledger */
static void
tskip(void)
{
	static struct {
		char	*name;
		char	*mark;		/* the stale record, or "" */
		char	*n32up;		/* n3.2's up: it is the reporter */
		int	nwit;
		int	inwit;		/* n3.2 is a witness */
		int	blocked;
	} t[] = {
		{ "no marks",		"",				"no",	3, 0, 0 },
		{ "subject in P",	"stale=n1.1 reporter=n3.2 since=5\n", "no",	4, 1, 1 },
		{ "reporter up",	"stale=n1.1 reporter=n3.2 since=5\n", "yes",	4, 1, 0 },
		{ "reporter heal",	"stale=n1.1 reporter=n3.2 since=5\n", "heal",	4, 1, 0 },
		{ "subject in P at E−1","stale=n1.0 reporter=n3.2 since=5\n", "no",	4, 1, 1 },
		{ "subject out of scope","stale=n3.0 reporter=n3.2 since=5\n", "no",	3, 0, 0 },
	};
	char body[3072];
	Witreq w;
	Cmap *m, *prev;
	Cwit ws[64];
	Cinst *b;
	int i, n;

	prev = build(nil, "6", prevbody);
	for(i = 0; i < nelem(t); i++){
		snprint(body, sizeof body,
			"instance=n1.0 onnode=n1 addr=a "
			"uuid=0000000000000000000000000000000a "
			"status=in up=yes\n"
			"instance=n1.1 onnode=n1 addr=b "
			"uuid=0000000000000000000000000000000b "
			"status=in up=yes\n"
			"instance=n2.0 onnode=n2 addr=c "
			"uuid=0000000000000000000000000000000c "
			"status=in up=yes\n"
			"instance=n3.0 onnode=n3 addr=d "
			"uuid=0000000000000000000000000000000d "
			"status=in up=yes\n"
			"instance=n3.2 onnode=n3 addr=e "
			"uuid=0000000000000000000000000000000e "
			"status=in up=%s\n%s", t[i].n32up, t[i].mark);
		m = build(nil, nil, body);
		memset(&w, 0, sizeof w);
		w.m = m;
		w.prev = prev;
		w.oid = "alpha";
		n = mapwitness(&w, ws, nelem(ws));
		if(n != t[i].nwit)
			fail("skip %s: |W| %d, want %d", t[i].name, n,
				t[i].nwit);
		if(inwit(ws, n, "n3.2") != t[i].inwit)
			fail("skip %s: clause 4 membership", t[i].name);
		b = blocker(ws, n, nelem(ws));
		if(t[i].blocked && (b == nil || strcmp(b->iid, "n3.2") != 0))
			fail("skip %s: an up=no in-scope reporter must block",
				t[i].name);
		if(!t[i].blocked && b != nil)
			fail("skip %s: blocked by %s", t[i].name, b->iid);
		checks += 3;
		mapfree(m);
	}
	mapfree(prev);
}

/*
 * §5.2 attaches its substitution to clause 2 alone ("for this
 * clause").  Clause 4 and the skip rule scope on the mark's subject
 * being in P(o) at E or at E−1, and the E−1 half is evaluated only
 * against a real E−1 map.  The subject here, n5.0, is in no
 * placement set at E, and its reporter n4.0 is up=no: with no E−1
 * map n4.0 is a witness by clause 2's substitution alone and must be
 * skipped, and with an E−1 map that placed n5.0 it must block.
 */
static char scopebody[] =
	"instance=n1.0 onnode=n1 addr=a "
	"uuid=0000000000000000000000000000000a status=in up=yes\n"
	"instance=n1.1 onnode=n1 addr=b "
	"uuid=0000000000000000000000000000000b status=in up=yes\n"
	"instance=n2.0 onnode=n2 addr=c "
	"uuid=0000000000000000000000000000000c status=in up=yes\n"
	"instance=n3.0 onnode=n3 addr=d "
	"uuid=0000000000000000000000000000000d status=in up=yes\n"
	"instance=n3.2 onnode=n3 addr=e "
	"uuid=0000000000000000000000000000000e status=in up=yes\n"
	"instance=n4.0 onnode=n4 addr=f "
	"uuid=0000000000000000000000000000000f status=out up=no\n"
	"instance=n5.0 onnode=n5 addr=g "
	"uuid=00000000000000000000000000000010 status=out up=yes\n"
	"stale=n5.0 reporter=n4.0 since=5\n";

/* at E−1 only n2 and n5 place, so n5.0 is in P(alpha) at E−1 */
static char scopeprev[] =
	"instance=n1.0 onnode=n1 addr=a "
	"uuid=0000000000000000000000000000000a status=out up=yes\n"
	"instance=n1.1 onnode=n1 addr=b "
	"uuid=0000000000000000000000000000000b status=out up=yes\n"
	"instance=n2.0 onnode=n2 addr=c "
	"uuid=0000000000000000000000000000000c status=in up=yes\n"
	"instance=n3.0 onnode=n3 addr=d "
	"uuid=0000000000000000000000000000000d status=out up=yes\n"
	"instance=n3.2 onnode=n3 addr=e "
	"uuid=0000000000000000000000000000000e status=out up=yes\n"
	"instance=n4.0 onnode=n4 addr=f "
	"uuid=0000000000000000000000000000000f status=out up=no\n"
	"instance=n5.0 onnode=n5 addr=g "
	"uuid=00000000000000000000000000000010 status=in up=yes\n";

static void
tscope(void)
{
	Witreq w;
	Cmap *m, *prev;
	Cwit ws[64];
	Cinst *p[Maxplace], *b;
	int n, np;

	m = build(nil, nil, scopebody);
	prev = build(nil, "6", scopeprev);
	np = mapplace(prev, "alpha", p, nelem(p));
	if(np != 2 || (strcmp(p[0]->iid, "n5.0") != 0 &&
	   strcmp(p[1]->iid, "n5.0") != 0))
		fail("scope: n5.0 is not in P(alpha) at E−1");
	np = mapplace(m, "alpha", p, nelem(p));
	if(np != 2 || strcmp(p[0]->iid, "n2.0") != 0 ||
	   strcmp(p[1]->iid, "n1.1") != 0)
		fail("scope: P(alpha) at E is not n2.0,n1.1");
	checks += 2;

	/* no E−1 map, reconcile open: clause 2 substitutes, 4 does not */
	memset(&w, 0, sizeof w);
	w.m = m;
	w.prev = nil;
	w.oid = "alpha";
	w.subst = 1;
	n = mapwitness(&w, ws, nelem(ws));
	if(!haswit(ws, n, "n4.0", Wprev, Wskip))
		fail("scope: under substitution an out-of-scope up=no "
			"reporter is not Wskip");
	if(haswit(ws, n, "n4.0", Wreporter, -1))
		fail("scope: the substitution reached clause 4");
	if((b = blocker(ws, n, nelem(ws))) != nil)
		fail("scope: blocked by %s under substitution", b->iid);
	checks += 3;

	/* no E−1 map and no substitution: the same */
	w.subst = 0;
	n = mapwitness(&w, ws, nelem(ws));
	if(!haswit(ws, n, "n4.0", Wprev, Wskip))
		fail("scope: with no E−1 map an out-of-scope up=no "
			"reporter is not Wskip");
	if((b = blocker(ws, n, nelem(ws))) != nil)
		fail("scope: blocked by %s with no E−1 map", b->iid);
	checks += 2;

	/* the E−1 map placed the subject: clause 4 holds and blocks */
	w.prev = prev;
	n = mapwitness(&w, ws, nelem(ws));
	if(!haswit(ws, n, "n4.0", Wreporter, Wblock))
		fail("scope: an in-scope up=no reporter at E−1 is not "
			"Wblock");
	b = blocker(ws, n, nelem(ws));
	if(b == nil || strcmp(b->iid, "n4.0") != 0)
		fail("scope: the blocker is %s, want n4.0",
			b == nil ? "none" : b->iid);
	checks += 2;

	/* subst is clause 2's alone: it does not unscope clause 4 */
	w.subst = 1;
	n = mapwitness(&w, ws, nelem(ws));
	if(!haswit(ws, n, "n4.0", Wreporter, Wblock))
		fail("scope: subst moved clause 4 off the E−1 map");
	checks++;

	mapfree(prev);
	mapfree(m);
}

/* §5.2: status=dead is excluded from the witness set outright */
static void
twitdead(void)
{
	static char body[] =
		"instance=n1.0 onnode=n1 addr=a "
		"uuid=0000000000000000000000000000000a status=in up=yes\n"
		"instance=n1.1 onnode=n1 addr=b "
		"uuid=0000000000000000000000000000000b status=in up=yes\n"
		"instance=n2.0 onnode=n2 addr=c "
		"uuid=0000000000000000000000000000000c status=in up=yes\n"
		"instance=n3.0 onnode=n3 addr=d "
		"uuid=0000000000000000000000000000000d status=in up=yes\n"
		"instance=n3.2 onnode=n3 addr=e "
		"uuid=0000000000000000000000000000000e status=dead up=no\n"
		"stale=n1.1 reporter=n3.2 since=5\n";
	Witreq w;
	Cmap *m;
	Cwit ws[64];
	char *stray[1];
	int n;

	m = build(nil, nil, body);
	memset(&w, 0, sizeof w);
	w.m = m;
	w.oid = "alpha";
	w.subst = 1;
	stray[0] = "n3.2";
	w.stray = stray;
	w.nstray = 1;
	n = mapwitness(&w, ws, nelem(ws));
	if(inwit(ws, n, "n3.2"))
		fail("witness: status=dead is a witness");
	if(blocker(ws, n, nelem(ws)) != nil)
		fail("witness: a dead reporter blocks");
	checks += 2;
	mapfree(m);
}

/* ------------------------------------------------------------------ */
/* §6.3 and §6.4                                                       */

static char *monid0 = "00112233445566778899aabbccddeeff";
static char *monid1 = "ffeeddccbbaa99887766554433221100";

static Cmap*
epochmap(uvlong epoch, char *monid)
{
	char t[2048];
	Cmap *m;

	snprint(t, sizeof t,
		"map=vec epoch=%llud\n"
		"\tmonid=%s\n"
		"\tobjmax=16777216 blksz=16384 replicas=2\n"
		"\tcsumalg=blake2s256 placehash=blake2s256-64\n"
		"\tpollms=1000 leasems=3000 replms=1000 deadms=10000\n"
		"\toutmins=60 tombdays=7 mincopies=1 retain=8\n",
		epoch, monid);
	if((m = mapparse(t, strlen(t))) == nil)
		sysfatal("epochmap: %r");
	return m;
}

static void
tadopt(void)
{
	Adopt a;
	Cmap *m;

	/* an instance that has never adopted takes what it is given */
	memset(&a, 0, sizeof a);
	m = epochmap(41, monid0);
	if(mapadoptable(&a, m) != Mapok)
		fail("adopt: a first map was refused");
	mapadopted(&a, m);
	if(!a.pinned || strcmp(a.monid, monid0) != 0 || a.epoch != 41)
		fail("adopt: the first map did not pin monid and epoch");
	checks += 2;
	mapfree(m);

	/* the same epoch again is a successful refresh (§6.3) */
	m = epochmap(41, monid0);
	if(mapadoptable(&a, m) != Mapok)
		fail("adopt: re-reading the held epoch was refused");
	checks++;
	mapfree(m);

	/* forward is fine */
	m = epochmap(42, monid0);
	if(mapadoptable(&a, m) != Mapok)
		fail("adopt: a higher epoch was refused");
	mapadopted(&a, m);
	if(a.epoch != 42)
		fail("adopt: the higher epoch was not recorded");
	checks += 2;
	mapfree(m);

	/* backward is not: epochregress=yes */
	m = epochmap(41, monid0);
	if(mapadoptable(&a, m) != Mapregress)
		fail("adopt: a lower epoch was accepted");
	if(strcmp(adoptwhy(Mapregress), "epochregress") != 0)
		fail("adopt: the regression flag is %s",
			adoptwhy(Mapregress));
	checks += 2;
	mapfree(m);

	/* a different monid is refused whatever its epoch */
	m = epochmap(99, monid1);
	if(mapadoptable(&a, m) != Mapmonid)
		fail("adopt: a foreign monid at a higher epoch was accepted");
	if(strcmp(adoptwhy(Mapmonid), "monidmismatch") != 0)
		fail("adopt: the monid flag is %s", adoptwhy(Mapmonid));
	checks += 2;
	mapfree(m);

	/*
	 * §6.3 makes two MUSTs, each with its own /status flag, and the
	 * accident it names — a freshly created monitor pointed at a
	 * live cluster — trips both: a foreign monid BELOW the held
	 * epoch must answer both flags, not whichever was tested first.
	 */
	m = epochmap(1, monid1);
	if(mapadoptable(&a, m) != (Mapmonid|Mapregress))
		fail("adopt: a foreign monid below the held epoch answers "
			"%d, not both flags", mapadoptable(&a, m));
	if(adoptwhy(Mapok) != nil)
		fail("adopt: Mapok names a flag");
	if(adoptwhy(Mapmonid|Mapregress) != nil)
		fail("adopt: two bits at once name one flag");
	checks += 3;
	mapfree(m);

	/* and each condition alone still answers its own bit alone */
	m = epochmap(1, monid0);
	if(mapadoptable(&a, m) != Mapregress)
		fail("adopt: a regression alone answers %d",
			mapadoptable(&a, m));
	mapfree(m);
	m = epochmap(99, monid1);
	if(mapadoptable(&a, m) != Mapmonid)
		fail("adopt: a foreign monid alone answers %d",
			mapadoptable(&a, m));
	checks += 2;
	mapfree(m);
}

/*
 * §6.4 F1 and F4 on a synthetic clock: milliseconds, monotonic, and
 * nothing here reads a real one.
 */
static void
tfence(void)
{
	Fence f;
	Adopt a;
	Cmap *m;

	memset(&f, 0, sizeof f);
	f.leasems = 3000;
	if(fencekind(&f, 0) != Fencelease)
		fail("F1: an instance that has never refreshed is not fenced");
	if(strcmp(fencename(Fencelease), "lease") != 0)
		fail("F4: /status names the lease fence %s",
			fencename(Fencelease));
	checks += 2;

	fencerefresh(&f, 10000);
	if(fencekind(&f, 10000) != Fencenone)
		fail("F1: fenced at the moment of a refresh");
	if(fencekind(&f, 12999) != Fencenone)
		fail("F1: fenced 1 ms inside the lease");
	if(fencekind(&f, 13000) != Fencelease)
		fail("F1: not fenced when leasems has elapsed");
	if(fencekind(&f, 99000) != Fencelease)
		fail("F1: not fenced long after the lease");
	checks += 4;

	/*
	 * A clock that has gone backwards cannot measure the interval
	 * §6.4 F1 is about, so the lease counts as elapsed rather
	 * than as nothing having passed.
	 */
	if(fencekind(&f, 9999) != Fencelease)
		fail("F1: a clock 1 ms backwards leaves the lease running");
	if(fencekind(&f, -990000) != Fencelease)
		fail("F1: a clock far backwards leaves the lease running");
	checks += 2;

	/* a refresh clears it (§6.4 F1: "until a refresh succeeds") */
	fencerefresh(&f, 99000);
	if(fencekind(&f, 99500) != Fencenone)
		fail("F1: a refresh did not clear the lease fence");
	checks++;

	/* F4 is a separate flag with the same effect */
	fenceoperator(&f, 1);
	if(fencekind(&f, 99500) != Fenceoper)
		fail("F4: the operator fence did not take");
	if(strcmp(fencename(Fenceoper), "operator") != 0)
		fail("F4: /status names the operator fence %s",
			fencename(Fenceoper));
	if(fencekind(&f, 103000) != Fenceboth)
		fail("F4: both fences in force are not reported as both");
	if(strcmp(fencename(Fenceboth), "both") != 0)
		fail("F4: /status names both fences %s",
			fencename(Fenceboth));
	checks += 4;

	/* fence off MUST NOT clear a lease-derived fence */
	fenceoperator(&f, 0);
	if(fencekind(&f, 103000) != Fencelease)
		fail("F4: fence off cleared the lease fence");
	if(fencekind(&f, 99500) != Fencenone)
		fail("F4: fence off did not clear the operator fence");
	if(strcmp(fencename(Fencenone), "none") != 0)
		fail("F4: /status names no fence %s", fencename(Fencenone));
	checks += 3;

	/*
	 * §6.3: a map refused for regression or a foreign monid is not
	 * a successful refresh, so it must not clear the lease.
	 */
	memset(&a, 0, sizeof a);
	memset(&f, 0, sizeof f);
	f.leasems = 3000;
	m = epochmap(41, monid0);
	if(maprefresh(&a, &f, m, 1000) != Mapok)
		fail("refresh: the first map was refused");
	if(fencekind(&f, 1000) != Fencenone)
		fail("refresh: a good map did not clear the fence");
	checks += 2;
	mapfree(m);

	m = epochmap(40, monid0);
	if(maprefresh(&a, &f, m, 2000) != Mapregress)
		fail("refresh: a regressed map was adopted");
	if(a.epoch != 41)
		fail("refresh: a regressed map moved the held epoch");
	if(fencekind(&f, 4100) != Fencelease)
		fail("refresh: a regressed map counted as a refresh");
	checks += 3;
	mapfree(m);

	m = epochmap(50, monid1);
	if(maprefresh(&a, &f, m, 4200) != Mapmonid)
		fail("refresh: a foreign monid was adopted");
	if(a.epoch != 41 || strcmp(a.monid, monid0) != 0)
		fail("refresh: a foreign monid moved the pin");
	if(fencekind(&f, 4200) != Fencelease)
		fail("refresh: a foreign monid counted as a refresh");
	checks += 3;
	mapfree(m);
}

/* §8.1's commit validation over two maps, and §8.6's exemption */
static void
tnext(void)
{
	char err[ERRMAX];
	Cmap *cur, *next;

	cur = epochmap(41, monid0);
	next = epochmap(42, monid0);
	if(!mapnextok(cur, next, 0))
		fail("commit: epoch+1 was refused: %r");
	checks++;
	mapfree(next);

	next = epochmap(43, monid0);
	if(mapnextok(cur, next, 0))
		fail("commit: epoch+2 was accepted");
	if(!mapnextok(cur, next, 1))
		fail("commit: forceepoch did not lift the epoch check");
	checks += 2;
	mapfree(next);

	next = epochmap(42, monid1);
	werrstr("");
	if(mapnextok(cur, next, 0))
		fail("commit: a changed monid was accepted");
	rerrstr(err, sizeof err);
	tascii("commit monid", err);
	if(!mapnextok(cur, next, 1))
		fail("commit: forceepoch did not lift the monid check");
	checks += 2;
	mapfree(next);

	/*
	 * §8.6's exemption is from `exactly current+1', not from
	 * increasing: §8.1 sets the next epoch to an arbitrary
	 * HIGHER value, §6.1 makes the epoch strictly increasing and
	 * §8.6.2 forbids publishing one that cannot be proved the
	 * highest.  Equal and lower are refused under force too.
	 */
	next = epochmap(41, monid0);
	if(mapnextok(cur, next, 1))
		fail("commit: forceepoch accepted an equal epoch");
	mapfree(next);
	next = epochmap(5, monid0);
	werrstr("");
	if(mapnextok(cur, next, 1))
		fail("commit: forceepoch accepted a lower epoch");
	rerrstr(err, sizeof err);
	if(strstr(err, "epoch 5 is not above 41") == nil)
		fail("commit: a forced regression answers `%s'", err);
	tascii("commit", err);
	mapfree(next);
	next = epochmap(99, monid1);
	if(!mapnextok(cur, next, 1))
		fail("commit: forceepoch refused a higher epoch with a "
			"new monid: %r");
	mapfree(next);
	checks += 4;
	mapfree(cur);

	/*
	 * §8.5's other immutables, which forceepoch does not lift:
	 * §8.1 exempts the `exactly current+1' and `monid immutable'
	 * checks from the commit it governs and then says
	 * "Everything else in the validation still applies to it", so
	 * each of these is refused at force = 1 as well as at 0.
	 */
	cur = mapparse(good, strlen(good));
	next = mapparse(good, strlen(good));
	if(cur == nil || next == nil)
		sysfatal("commit: the example map: %r");
	next->epoch = cur->epoch + 1;
	next->objmax = cur->objmax * 2;
	if(mapnextok(cur, next, 0) || mapnextok(cur, next, 1))
		fail("commit: a changed objmax was accepted");
	next->objmax = cur->objmax;
	next->blksz = cur->blksz * 2;
	if(mapnextok(cur, next, 0) || mapnextok(cur, next, 1))
		fail("commit: a changed blksz was accepted");
	next->blksz = cur->blksz;
	strcpy(next->csumalg, "other");
	if(mapnextok(cur, next, 0) || mapnextok(cur, next, 1))
		fail("commit: a changed csumalg was accepted");
	strcpy(next->csumalg, cur->csumalg);
	strcpy(next->placehash, "other");
	if(mapnextok(cur, next, 0) || mapnextok(cur, next, 1))
		fail("commit: a changed placehash was accepted");
	strcpy(next->placehash, cur->placehash);
	if(!mapnextok(cur, next, 0) || !mapnextok(cur, next, 1))
		fail("commit: an otherwise equal map at epoch+1 was refused");
	checks += 5;
	mapfree(next);
	mapfree(cur);
}

void
main(int, char**)
{
	tmatrix();
	theader();
	tunknown();
	tcomments();
	tdefaults();
	tblank();
	tnames();
	tvectors();
	ttiebreak();
	tunderrep();
	tbignodes();
	tprimary();
	tdown();
	twitness();
	tskip();
	tscope();
	twitdead();
	tadopt();
	tfence();
	tnext();
	if(fails > 0)
		exits("failed");
	print("maptest: %d checks ok\n", checks);
	exits(nil);
}
