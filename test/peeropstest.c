#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"
#include "t1.h"

/*
 * T1 for the engine calls layer-a §5.5/§5.6's peer channels need and
 * the store did not have: tombstone adoption (§1.5), op=drop (§5.6,
 * §7.4), the resulting-csum check §5.5 makes the receiver's duty, and
 * the oid-ordered op=list enumeration.  docs/design/store.md §13's
 * T1.30 to T1.33.
 */

enum
{
	Blk	= 4096,
};

static uchar emptycsum[Csumlen];

/* ------------------------------------------------------------------ */

static void
errsays(char *what, char *pfx)
{
	char e[ERRMAX];

	rerrstr(e, sizeof e);
	checks++;
	if(strncmp(e, pfx, strlen(pfx)) != 0)
		fail("%s: error `%s', wanted `%s'", what, e, pfx);
}

/*
 * §3.7's normative carve-out: an internal-invariant error MUST NOT
 * begin with one of layer-a §2.6's prefixes, because that set is
 * prefix-free and a client parsing one out of a bug would read it as
 * an ordinary refusal.
 */
static void
errnotwire(char *what)
{
	static char *w[] = {
		"no such object", "object deleted", "object exists",
		"bad object name", "object too large", "stale version",
		"bad ctl", "checksum mismatch", "not discardable",
		"disk full", "not ready", "permission denied",
		"still placed", "bad map", "stale epoch", "future epoch",
		"fenced", nil,
	};
	char e[ERRMAX];
	int i;

	rerrstr(e, sizeof e);
	checks++;
	for(i = 0; w[i] != nil; i++)
		if(strncmp(e, w[i], strlen(w[i])) == 0){
			fail("%s: `%s' begins with a layer-a 2.6 prefix, `%s'",
				what, e, w[i]);
			return;
		}
}

/* layer-a §1.1's byte order, computed here rather than borrowed */
static int
oidcmptest(uchar *a, int na, uchar *b, int nb)
{
	int n, c;

	n = na < nb ? na : nb;
	if(n > 0 && (c = memcmp(a, b, n)) != 0)
		return c;
	if(na == nb)
		return 0;
	return na < nb ? -1 : 1;
}

static int
ostat(Store *s, char *name, Objinfo *oi)
{
	uchar o[Oidmax];

	oidof(o, name);
	return objstat(s, o, strlen(name), oi);
}

static void
mk(Store *s, char *name)
{
	uchar o[Oidmax];

	oidof(o, name);
	if(objcreate(s, o, strlen(name), 1, 1, nil, 0, nil) < 0)
		fail("objcreate %s: %r", name);
}

static int
wr(Store *s, char *name, void *a, long n, uvlong off, uvlong ver)
{
	uchar o[Oidmax];

	oidof(o, name);
	return objwrite(s, o, strlen(name), a, n, off, ver, 1, nil, 0);
}

static int
rmv(Store *s, char *name, uvlong ver)
{
	uchar o[Oidmax];

	oidof(o, name);
	return objremove(s, o, strlen(name), ver, 1, nil, 0);
}

static Store*
restart(Store *s, Dev *d, char *what)
{
	storeclose(s);
	return mustopen(d, what);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */

/*
 * T1.32, the resulting-csum check.  Each case is run twice over two
 * identical stores: the first learns the csum the operation produces,
 * the second is offered a wrong one — which must publish nothing at
 * all, shown by the log's own watermark and by a restart — and then
 * the right one, which must commit.
 */
enum
{
	Cwrite	= 0,
	Ctrunc,
	Cdelete,
	Ccreate,
	Cfull,
	Cadopt,
};

static char *cname[] = {
	"objwrite", "objtrunc", "objremove", "objcreate", "op=full",
	"objadopt",
};

/* everything the case needs in place before the operation under test */
static void
csumsetup(Store *s, int kind, uchar *buf)
{
	switch(kind){
	case Cwrite:
	case Ctrunc:
	case Cdelete:
		mk(s, "sub");
		if(wr(s, "sub", buf, 2*Blk, 0, 2) < 0)
			fail("%s: setup objwrite: %r", cname[kind]);
		break;
	case Ccreate:
	case Cfull:
	case Cadopt:
		break;
	}
}

/* the operation under test, with the expected csum (nil = no check) */
static int
csumop(Store *s, int kind, uchar *buf, uchar *csum)
{
	Stage *g;
	uchar o[Oidmax];

	switch(kind){
	case Cwrite:
		oidof(o, "sub");
		return objwritecsum(s, o, 3, buf, Blk, 2*Blk, 3, 1, csum,
			nil, 0);
	case Ctrunc:
		oidof(o, "sub");
		return objtrunccsum(s, o, 3, Blk/2, 3, 1, csum, nil, 0);
	case Cdelete:
		oidof(o, "sub");
		return objremovecsum(s, o, 3, 3, 1, csum, nil, 0);
	case Ccreate:
		oidof(o, "new");
		return objcreatecsum(s, o, 3, 1, 1, csum, nil, 0, nil);
	case Cfull:
		oidof(o, "new");
		if((g = stageopen(s, o, 3, 2*Blk, 0)) == nil){
			fail("op=full: stageopen: %r");
			return -1;
		}
		if(stagewrite(g, buf, 2*Blk, 0) < 0){
			fail("op=full: stagewrite: %r");
			stagediscard(g);
			return -1;
		}
		/* stagefinalcsum consumes the handle on every outcome */
		return stagefinalcsum(g, 7, 2, csum, nil, 0);
	}
	return -1;
}

/* the object the case ends on */
static char*
csumobj(int kind)
{
	return kind == Cwrite || kind == Ctrunc || kind == Cdelete
		? "sub" : "new";
}

static void
tcsum(int kind)
{
	Dev *d;
	Store *s;
	Objinfo want, oi;
	Storestat st0, st1;
	uchar *buf, bad[Csumlen];
	char *what;

	what = cname[kind];

	/* pass one: what does the operation produce? */
	spawnforget();
	d = newdisk();
	if((s = mustopen(d, what)) == nil){
		devclose(d);
		return;
	}
	buf = mkbuf(2*Blk, 41 + kind);
	csumsetup(s, kind, buf);
	if(csumop(s, kind, buf, nil) < 0){
		fail("%s: the unchecked operation: %r", what);
		storeclose(s);
		devclose(d);
		free(buf);
		return;
	}
	if(ostat(s, csumobj(kind), &want) < 0)
		fail("%s: objstat after the unchecked operation: %r", what);
	storeclose(s);
	devclose(d);

	/* pass two: the same store, offered a wrong csum and then a right one */
	spawnforget();
	d = newdisk();
	if((s = mustopen(d, what)) == nil){
		devclose(d);
		free(buf);
		return;
	}
	csumsetup(s, kind, buf);
	if(ostat(s, csumobj(kind), &oi) < 0)
		memset(&oi, 0, sizeof oi);
	storestat(s, &st0);
	memset(bad, 0xa5, sizeof bad);
	checks++;
	if(csumop(s, kind, buf, bad) >= 0)
		fail("%s: a wrong expected csum was taken", what);
	else
		errsays(what, "checksum mismatch");
	/*
	 * Nothing durable: the log's next sequence number has not moved,
	 * so no record was written — the check that a later objstat
	 * alone could not make, since an in-memory refusal and a durable
	 * record followed by an in-memory refusal look the same there.
	 */
	storestat(s, &st1);
	eqv("a failed csum check writes no log record", st1.seqnext,
		st0.seqnext);
	eqv("... and moves no watermark", st1.watermark, st0.watermark);
	if((s = restart(s, d, what)) == nil){
		devclose(d);
		free(buf);
		return;
	}
	if(kind == Ccreate || kind == Cadopt || kind == Cfull){
		checks++;
		if(ostat(s, "new", &oi) >= 0)
			fail("%s: the refused operation is durable", what);
	}else if(ostat(s, "sub", &oi) < 0)
		fail("%s: objstat after the replay: %r", what);
	else{
		eqv("a refused operation leaves the len alone", oi.len, 2*Blk);
		eqv("... and the ver", oi.ver, 2);
		eqv("... and the state", oi.state, Slive);
	}

	/* and the right csum commits */
	if(csumop(s, kind, buf, want.csum) < 0)
		fail("%s: the right expected csum was refused: %r", what);
	else if(ostat(s, csumobj(kind), &oi) < 0)
		fail("%s: objstat after the checked operation: %r", what);
	else{
		eqv("a checked operation commits the same len", oi.len,
			want.len);
		eqv("... the same ver", oi.ver, want.ver);
		eqv("... the same state", oi.state, want.state);
		checks++;
		if(memcmp(oi.csum, want.csum, Csumlen) != 0)
			fail("%s: the checked operation published another csum",
				what);
	}

	free(buf);
	storeclose(s);
	devclose(d);
	killspawned();
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */

void
main(int argc, char **argv)
{
	int i;

	USED(argc); USED(argv);
	csumdigests(nil, 0, emptycsum);

	for(i = Cwrite; i <= Cfull; i++)
		tcsum(i);

	killspawned();
	if(fails > 0){
		print("peeropstest: %d of %d checks FAILED\n", fails, checks);
		exits("failed");
	}
	print("peeropstest: %d checks ok\n", checks);
	exits(nil);
}
