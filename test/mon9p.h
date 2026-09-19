/*
 * An in-process 9P client, for the T1 programs that drive
 * mon/libshoalmon.a.
 *
 * This is test/srv9p.h, adapted: the server it starts is the
 * monitor's rather than the storage instance's, so `Cl' carries a
 * Monctx and the loop proc calls monsrvrun.  Everything else — the
 * tag accounting, the replies held aside, the watchdog and the small
 * request helpers — is the same file.  It is a copy and not a
 * parameter on the original because srv9p.h is tied to Srvctx in its
 * type, its start call and its shutdown report, and because a program
 * that included both headers would have to include both libraries'
 * public headers, which define two different snapshot types.  Folding
 * the two clients into one that takes a start callback is a later
 * change; this note is the record that there are two.
 *
 * The server runs on a pipe inside the test program — Srv.infd and
 * Srv.outfd are one end, this client holds the other — over a
 * simulated disk, so a whole monitor is one T1 program with no mount,
 * no file server and no exec.  The client speaks raw 9P through
 * convS2M/convM2S rather than through a mount, which is what lets a
 * test send a Tattach with any aname it likes and read the exact
 * Rerror string back off the wire unmangled.  Exact strings are what
 * most of the cases assert.
 *
 * Every program built on this arms a watchdog proc before it starts:
 * a wedged server must fail the test, not hang `mk test'.  The
 * watchdog prints a FAIL line and ends the whole program through
 * threadexitsall (thread(2)), which is what takes the server proc
 * down with it: a note to one proc leaves it running for as long as
 * the machine is up, and a note to the note group would reach the
 * shell that ran mk.
 *
 * Include after <u.h>, <libc.h>, <libsec.h>, <fcall.h>, <thread.h>,
 * <9p.h>, "../lib/shoal.h" and "../mon/mon.h", and after the
 * program's own fail(char *fmt, ...) and eqs(char *what, char *got,
 * char *want) helpers, which are what everything here reports
 * through.  A program that includes it installs quotefmtinstall, sets
 * mainstacksize to Monstack and arms the watchdog (clwatchon) before
 * its first case.
 *
 * Tags are the client's, and a tag is free for reuse once the reply
 * carrying it has been read: clrpc gives its own back, and a program
 * that pipelines gives each back with cltagfree when it is done with
 * it.  Replies arrive in request order here — the monitor answers
 * every request on its service loop and has no queues (mon/mon.c) —
 * but the machinery that survives them arriving out of order is kept
 * as it stands, so that a case that pipelines is asserting that order
 * rather than assuming it.
 *
 * A tag is OUTSTANDING from the write of the request carrying it
 * until its reply comes off the wire, and a reply held aside is one
 * nobody has claimed yet.  A tag in either state is not free, and
 * this client refuses to hand it out again.  So a reply for a tag
 * with nothing outstanding fails at once, and a case that leaves a
 * reply held aside fails when the client is closed.
 */
enum
{
	Clmsize		= 8192+IOHDRSZ,	/* what a test proposes by default */
	Clbuf		= 64*1024,
	Clwatchms	= 60*1000,	/* the whole program's budget */
	Clmaxpend	= 16,		/* replies held aside for later */
	Clmaxfree	= 64,		/* tags given back and not yet reused */
	Clmaxtag	= 512,		/* tags run 1..Clmaxtag-1: out[] is by tag */
};

/*
 * Two pipes rather than one, because Srv keeps infd and outfd apart
 * and the shutdown sequence needs them apart: a test ends the service
 * loop by closing the request pipe, and the reply pipe stays open so
 * that a request still in flight can be answered as the shutdown
 * drains it.  With one pipe the server would write into a closed one,
 * which on Plan 9 is a note and not an error return.
 */
typedef struct Cl Cl;
typedef struct Clpend Clpend;

/* a reply for another tag, kept as it came off the wire */
struct Clpend
{
	uchar	*m;
	int	n;
	ushort	tag;
};

struct Cl
{
	Monctx	*ctx;
	int	wfd;		/* the client writes requests here */
	int	rfd;		/* ... and reads replies here */
	int	sin;		/* the server's infd */
	int	sout;		/* ... and outfd */
	uint	msize;
	uchar	*wbuf;
	uchar	*rbuf;
	uchar	*rraw;		/* ... and the same message, undecoded */
	int	rlen;		/* the message the two are holding */
	ushort	tag;		/* the next tag never yet handed out */
	ushort	freetag[Clmaxfree];
	int	nfree;
	uchar	out[Clmaxtag];	/* the reply for this tag has not arrived */
	Clpend	pend[Clmaxpend];
	int	npend;
	int	botch;		/* a reply arrived that nothing was waiting for */
	int	ended;		/* the service loop has returned */
};

static int clwdstop;
static char *clstage = "starting";

/*
 * The whole program's budget.  Clwatchms is what a program gets if it
 * says nothing; one whose run is longer sets this before clwatchon,
 * since a watchdog that fires on a working test is a failure of the
 * test and not of the server.
 */
static int clwatchms = Clwatchms;

static void
clwatch(void*)
{
	int i;

	for(i = 0; i < clwatchms/50; i++){
		if(clwdstop)
			threadexits(nil);
		sleep(50);
	}
	fprint(2, "FAIL: wedged at stage %s: ending the test\n", clstage);
	threadexitsall("wedged");
}

/*
 * Arm the watchdog.  It is a proc of the program's own, so it dies
 * with the program; clwatchoff lets a finished run retire it rather
 * than end the program itself.
 */
static void
clwatchon(void)
{
	clwdstop = 0;
	proccreate(clwatch, nil, 8192);
}

static void
clwatchoff(void)
{
	clwdstop = 1;
}

static void
clsrvproc(void *v)
{
	Cl *c;

	c = v;
	monsrvrun(c->ctx, c->sin, c->sout);
	c->ended = 1;
	threadexits(nil);
}

/* what a reply says went wrong, for a case that compares the string */
static char*
clerr(Fcall *r)
{
	if(r->type == Rerror)
		return r->ename;
	return "ok";
}

/* is a reply for this tag still to come, or still held aside? */
static int
clbusy(Cl *c, ushort tag)
{
	int i;

	if(tag < Clmaxtag && c->out[tag])
		return 1;
	for(i = 0; i < c->npend; i++)
		if(c->pend[i].tag == tag)
			return 1;
	return 0;
}

/*
 * A tag nothing is waiting for.  Tags are handed out from the free
 * list first, so a program that gives them back keeps reusing a
 * handful of them and the server's tag handling is exercised rather
 * than merely counted up past — but never one whose reply has not
 * been accounted for, whatever the free list holds.
 */
static ushort
cltag(Cl *c)
{
	ushort t;

	while(c->nfree > 0){
		t = c->freetag[--c->nfree];
		if(!clbusy(c, t))
			return t;
	}
	for(;;){
		if(c->tag == NOTAG || c->tag >= Clmaxtag)
			c->tag = 1;
		t = c->tag++;
		if(!clbusy(c, t))
			return t;
	}
}

/*
 * The reply carrying this tag has been read: it may be handed out
 * again.  A tag given back while its reply is still outstanding or
 * still held aside is the error, not something to paper over — the
 * next request would take it and collect the old reply as its own.
 */
static void
cltagfree(Cl *c, ushort tag)
{
	if(tag == NOTAG)
		return;
	if(clbusy(c, tag)){
		fail("%s: tag %ud given back with its reply uncollected",
			clstage, tag);
		return;
	}
	if(c->nfree < nelem(c->freetag))
		c->freetag[c->nfree++] = tag;
}

static void
clput(Cl *c, Fcall *t)
{
	int n;

	if(t->tag != NOTAG){
		if(t->tag >= Clmaxtag)
			fail("%s: tag %ud is above what this client tracks",
				clstage, t->tag);
		else
			c->out[t->tag] = 1;
	}
	n = convS2M(t, c->wbuf, c->msize);
	if(n <= 0)
		sysfatal("convS2M: %r");
	if(write(c->wfd, c->wbuf, n) != n)
		sysfatal("9P write: %r");
}

/*
 * The next message off the wire, into rbuf.  What r points into stays
 * valid until the next read.
 *
 * rraw keeps the bytes as they arrived, because convM2S DECODES IN
 * PLACE: its gstring shifts each string down over the 2-byte count
 * that precedes it and NUL-terminates it there, so the buffer it
 * decoded is no longer a 9P message and a second convM2S over it
 * reads a count out of the string's own text.  A reply held aside is
 * held aside to be decoded again later (clgettag), so it is copied
 * from rraw and not from the buffer that was decoded.
 *
 * The tag settles here: a reply carrying one this client has nothing
 * outstanding for is a second answer to a request, or an answer to
 * none, and it fails where it arrives rather than being absorbed by
 * whatever asks next.  The Rversion carries NOTAG, which is nobody's.
 */
static int
clrecv(Cl *c, Fcall *r)
{
	int n;

	if((n = read9pmsg(c->rfd, c->rbuf, Clbuf)) <= 0){
		memset(r, 0, sizeof *r);
		c->rlen = 0;
		return -1;
	}
	c->rlen = n;
	memmove(c->rraw, c->rbuf, n);
	if(convM2S(c->rbuf, n, r) != n)
		sysfatal("convM2S: short message");
	if(r->tag != NOTAG){
		if(r->tag >= Clmaxtag || !c->out[r->tag]){
			c->botch = 1;
			fail("%s: a reply nothing is waiting for: type %d tag %ud",
				clstage, r->type, r->tag);
		}else
			c->out[r->tag] = 0;
	}
	return r->type;
}

/* the i'th reply held aside, taken off the list and decoded */
static int
clpop(Cl *c, int i, Fcall *r)
{
	int n;

	n = c->pend[i].n;
	memmove(c->rbuf, c->pend[i].m, n);
	memmove(c->rraw, c->pend[i].m, n);
	free(c->pend[i].m);
	c->npend--;
	memmove(c->pend+i, c->pend+i+1, (c->npend-i)*sizeof c->pend[0]);
	c->rlen = n;
	if(convM2S(c->rbuf, n, r) != n)
		sysfatal("convM2S: short message");
	return r->type;
}

/*
 * The next reply, in arrival order: the oldest one clgettag held aside
 * for another tag, if there is one, and otherwise the next off the
 * wire.  A test that pipelines asserts that order, so nothing here
 * reorders anything on its own.
 */
static int
clget(Cl *c, Fcall *r)
{
	if(c->npend > 0)
		return clpop(c, 0, r);
	return clrecv(c, r);
}

/*
 * The reply this tag is waiting for: one already held aside, or the
 * next off the wire that carries the tag.  Replies for other tags are
 * held aside in arrival order for a later clget or clgettag — which is
 * how a program reads the answers to pipelined requests in whatever
 * order the server chose to send them.
 */
static int
clgettag(Cl *c, ushort tag, Fcall *r)
{
	uchar *m;
	int i;

	for(i = 0; i < c->npend; i++)
		if(c->pend[i].tag == tag)
			return clpop(c, i, r);
	for(;;){
		if(clrecv(c, r) < 0)
			return -1;
		if(r->tag == tag)
			return r->type;
		if(c->botch)		/* it is nobody's: do not wait on more */
			return -1;
		if(c->npend >= nelem(c->pend)){
			fail("%s: %d replies held aside, none for tag %ud",
				clstage, c->npend, tag);
			return -1;
		}
		if((m = malloc(c->rlen)) == nil)
			sysfatal("malloc: %r");
		memmove(m, c->rraw, c->rlen);	/* as it arrived, not as decoded */
		c->pend[c->npend].m = m;
		c->pend[c->npend].n = c->rlen;
		c->pend[c->npend].tag = r->tag;
		c->npend++;
	}
}

/*
 * One exchange: the reply to this request, whatever else arrives
 * first.  The tag goes back on the free list, since the exchange it
 * named is over.
 */
static int
clrpc(Cl *c, Fcall *t, Fcall *r)
{
	clput(c, t);
	if(clgettag(c, t->tag, r) < 0)
		return -1;
	cltagfree(c, t->tag);
	return r->type;
}

/* the reply is this exact Rerror, and nothing else: one check */
static void
clerris(char *what, Fcall *r, char *want)
{
	eqs(what, clerr(r), want);
}

/*
 * Start a server on a pipe and negotiate a version.  msize is what the
 * client proposes: a test that wants §2.1's floor refused proposes
 * below it.
 */
static void
clstart(Cl *c, Monctx *ctx, ulong msize)
{
	Fcall t, r;
	int a[2], b[2];

	memset(c, 0, sizeof *c);
	c->ctx = ctx;
	c->tag = 1;
	if(pipe(a) < 0 || pipe(b) < 0)
		sysfatal("pipe: %r");
	c->wfd = a[1];
	c->sin = a[0];
	c->sout = b[0];
	c->rfd = b[1];
	c->msize = Clbuf;
	if((c->wbuf = malloc(Clbuf)) == nil || (c->rbuf = malloc(Clbuf)) == nil
		|| (c->rraw = malloc(Clbuf)) == nil)
		sysfatal("malloc: %r");
	proccreate(clsrvproc, c, Monstack);

	memset(&t, 0, sizeof t);
	t.type = Tversion;
	t.tag = NOTAG;
	t.msize = msize;
	t.version = "9P2000";
	if(clrpc(c, &t, &r) != Rversion)
		sysfatal("Tversion: no Rversion");
	c->msize = r.msize;
}

/*
 * Close the request pipe.  That is the EOF the service loop ends on,
 * which is D16's shutdown trigger; the reply pipe stays open, so a
 * request the shutdown is still draining can be answered and read.
 */
static void
clhangup(Cl *c)
{
	if(c->wfd >= 0){
		close(c->wfd);
		c->wfd = -1;
	}
}

static int
clwaitend(Cl *c, int ms)
{
	int i;

	for(i = 0; i*25 < ms && !c->ended; i++)
		sleep(25);
	return c->ended;
}

/*
 * Give back everything the client holds: both ends of both pipes, the
 * two message buffers and any reply still held aside.  The service
 * loop must already have ended — clstop is the one that waits for it,
 * and a case that drives the shutdown itself waits with clwaitend and
 * then comes here.
 *
 * A reply still held aside is a reply the case never claimed: either
 * the server sent one nobody asked for, or the case stopped reading
 * half way through what it pipelined.  Freeing it in silence is how
 * both go unnoticed, so it is a failure of the case.
 */
static void
clclose(Cl *c)
{
	int i;

	clhangup(c);
	if(c->rfd >= 0)
		close(c->rfd);
	if(c->sin >= 0)
		close(c->sin);
	if(c->sout >= 0)
		close(c->sout);
	c->rfd = c->sin = c->sout = -1;
	free(c->wbuf);
	free(c->rbuf);
	free(c->rraw);
	c->wbuf = c->rbuf = c->rraw = nil;
	if(c->npend > 0)
		fail("%s: %d replies nobody claimed", clstage, c->npend);
	for(i = 0; i < c->npend; i++)
		free(c->pend[i].m);
	c->npend = 0;
}

/* hang up, wait the service loop out, and let go */
static void
clstop(Cl *c)
{
	clhangup(c);
	if(!clwaitend(c, 10000))
		fail("%s: the service loop did not end within 10s", clstage);
	clclose(c);
}

/* the small conveniences every case uses */

static int
clattach(Cl *c, ulong fid, char *aname, Fcall *r)
{
	Fcall t;

	memset(&t, 0, sizeof t);
	t.type = Tattach;
	t.tag = cltag(c);
	t.fid = fid;
	t.afid = NOFID;
	t.uname = "glenda";
	t.aname = aname;
	return clrpc(c, &t, r);
}

static int
clwalk(Cl *c, ulong fid, ulong newfid, int n, char **names, Fcall *r)
{
	Fcall t;
	int i;

	memset(&t, 0, sizeof t);
	t.type = Twalk;
	t.tag = cltag(c);
	t.fid = fid;
	t.newfid = newfid;
	t.nwname = n;
	for(i = 0; i < n; i++)
		t.wname[i] = names[i];
	return clrpc(c, &t, r);
}

static int
clwalk1(Cl *c, ulong fid, ulong newfid, char *name, Fcall *r)
{
	char *v[1];

	v[0] = name;
	return clwalk(c, fid, newfid, 1, v, r);
}

static int
clopen(Cl *c, ulong fid, int mode, Fcall *r)
{
	Fcall t;

	memset(&t, 0, sizeof t);
	t.type = Topen;
	t.tag = cltag(c);
	t.fid = fid;
	t.mode = mode;
	return clrpc(c, &t, r);
}

/* §2.4's Tcreate, on a directory fid, which it turns into the new file's */
static int
clcreate(Cl *c, ulong fid, char *name, ulong perm, int mode, Fcall *r)
{
	Fcall t;

	memset(&t, 0, sizeof t);
	t.type = Tcreate;
	t.tag = cltag(c);
	t.fid = fid;
	t.name = name;
	t.perm = perm;
	t.mode = mode;
	return clrpc(c, &t, r);
}

static int
clremove(Cl *c, ulong fid, Fcall *r)
{
	Fcall t;

	memset(&t, 0, sizeof t);
	t.type = Tremove;
	t.tag = cltag(c);
	t.fid = fid;
	return clrpc(c, &t, r);
}

/* Twstat of a Dir the caller has filled in, nulldir'd or not */
static int
clwstat(Cl *c, ulong fid, Dir *d, Fcall *r)
{
	uchar stat[STATMAX];
	Fcall t;
	int n;

	if((n = convD2M(d, stat, sizeof stat)) <= BIT16SZ)
		sysfatal("convD2M: %r");
	memset(&t, 0, sizeof t);
	t.type = Twstat;
	t.tag = cltag(c);
	t.fid = fid;
	t.stat = stat;
	t.nstat = n;
	return clrpc(c, &t, r);
}

/*
 * §5.4.1's Tflush, for a case with nothing else in flight: the Rflush
 * is the next reply to arrive, and this asserts exactly that.  Waiting
 * for it by tag instead would hold a wrong-tagged Rflush aside and go
 * on reading for one that is never coming — the watchdog's whole
 * budget spent where a failure on the spot belongs.  A case that has a
 * flushed request still to be answered puts its own Tflush and reads
 * the two replies in the order §5.4.1 sends them.
 */
static int
clflush(Cl *c, ushort oldtag, Fcall *r)
{
	Fcall t;
	ushort tag;

	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = tag = cltag(c);
	t.oldtag = oldtag;
	clput(c, &t);
	if(clget(c, r) < 0)
		return -1;
	if(r->tag != tag){
		fail("%s: the Rflush for tag %ud came back on tag %ud",
			clstage, tag, r->tag);
		return -1;
	}
	cltagfree(c, tag);
	return r->type;
}

static int
clread(Cl *c, ulong fid, vlong off, long count, Fcall *r)
{
	Fcall t;

	memset(&t, 0, sizeof t);
	t.type = Tread;
	t.tag = cltag(c);
	t.fid = fid;
	t.offset = off;
	t.count = count;
	return clrpc(c, &t, r);
}

static int
clwrite(Cl *c, ulong fid, vlong off, char *s, Fcall *r)
{
	Fcall t;

	memset(&t, 0, sizeof t);
	t.type = Twrite;
	t.tag = cltag(c);
	t.fid = fid;
	t.offset = off;
	t.count = strlen(s);
	t.data = s;
	return clrpc(c, &t, r);
}

static int
clclunk(Cl *c, ulong fid, Fcall *r)
{
	Fcall t;

	memset(&t, 0, sizeof t);
	t.type = Tclunk;
	t.tag = cltag(c);
	t.fid = fid;
	return clrpc(c, &t, r);
}

static int
clstat(Cl *c, ulong fid, Fcall *r)
{
	Fcall t;

	memset(&t, 0, sizeof t);
	t.type = Tstat;
	t.tag = cltag(c);
	t.fid = fid;
	return clrpc(c, &t, r);
}

/* walk a fresh fid to a path from the attached root, and open it */
static int
clopenpath(Cl *c, ulong root, ulong fid, int n, char **names, int mode,
	Fcall *r)
{
	if(clwalk(c, root, fid, n, names, r) != Rwalk)
		return r->type;
	if(r->nwqid != n){
		fail("short walk: %d of %d", r->nwqid, n);
		return -1;
	}
	return clopen(c, fid, mode, r);
}

/* the whole of a small file, into buf; answers the byte count or -1 */
static long
clslurp(Cl *c, ulong fid, char *buf, long max)
{
	Fcall r;
	long off;

	off = 0;
	for(;;){
		if(clread(c, fid, off, 4096, &r) != Rread)
			return -1;
		if(r.count == 0)
			break;
		if(off + r.count > max-1)
			return -1;
		memmove(buf+off, r.data, r.count);
		off += r.count;
	}
	buf[off] = 0;
	return off;
}

/* one attr=value line's value out of a rendered status text */
static char*
clfield(char *text, char *attr, char *buf, int nbuf)
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
