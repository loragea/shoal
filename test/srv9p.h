/*
 * An in-process 9P client, for the T1 programs that drive
 * srv/libshoalsrv.a.
 *
 * The server runs on a pipe inside the test program — Srv.infd and
 * Srv.outfd are one end, this client holds the other — over a
 * simulated disk, so a whole storage instance is one T1 program with
 * no mount, no file server and no exec.  The client speaks raw 9P
 * through convS2M/convM2S rather than through a mount, which is what
 * lets a test do the three things a mounted client cannot: pipeline
 * requests and read the replies in arrival order, send a Tflush naming
 * any tag it likes, and propose an msize below the floor layer-a §2.1
 * enforces.  Exact Rerror strings are what most of the cases assert,
 * and those come off the wire here unmangled.
 *
 * Every program built on this arms a watchdog proc before it starts:
 * a wedged server must fail the test, not hang `mk test'.  The
 * watchdog prints a FAIL line and ends the whole program through
 * threadexitsall (thread(2)), which is what takes the server proc and
 * the pool's queue procs down with it: a note to one proc leaves those
 * running for as long as the machine is up, and a note to the note
 * group would reach the shell that ran mk.
 *
 * Include after <u.h>, <libc.h>, <libsec.h>, <fcall.h>, <thread.h>,
 * <9p.h>, "../lib/shoal.h" and "../srv/srv.h", and after the program's
 * own fail(char *fmt, ...) and eqs(char *what, char *got, char *want)
 * helpers, which are what everything here reports through.  A program
 * that includes it installs quotefmtinstall, sets mainstacksize to
 * Srvstack and arms the watchdog (clwatchon) before its first case.
 *
 * Tags are the client's, and a tag is free for reuse once the reply
 * carrying it has been read: clrpc gives its own back, and a program
 * that pipelines gives each back with cltagfree when it is done with
 * it.  Replies need not arrive in request order once more than one
 * queue is in play, so clgettag collects the one reply a tag is
 * waiting for and holds the others aside; clget then answers those in
 * arrival order, which is the order layer-a §5.4.1 is about.
 */

enum
{
	Clmsize		= 8192+IOHDRSZ,	/* what a test proposes by default */
	Clbuf		= 64*1024,
	Clwatchms	= 60*1000,	/* the whole program's budget */
	Clmaxpend	= 16,		/* replies held aside for later */
	Clmaxfree	= 64,		/* tags given back and not yet reused */
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
	Srvctx	*ctx;
	int	wfd;		/* the client writes requests here */
	int	rfd;		/* ... and reads replies here */
	int	sin;		/* the server's infd */
	int	sout;		/* ... and outfd */
	uint	msize;
	uchar	*wbuf;
	uchar	*rbuf;
	int	rlen;		/* the message rbuf is holding */
	ushort	tag;		/* the next tag never yet handed out */
	ushort	freetag[Clmaxfree];
	int	nfree;
	Clpend	pend[Clmaxpend];
	int	npend;
	int	ended;		/* the service loop has returned */
};

static int clwdstop;
static char *clstage = "starting";

static void
clwatch(void*)
{
	int i;

	for(i = 0; i < Clwatchms/50; i++){
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
	srvrun(c->ctx, c->sin, c->sout);
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

/*
 * A tag nothing is waiting for.  Tags are handed out from the free
 * list first, so a program that gives them back keeps reusing a
 * handful of them and the server's tag handling is exercised rather
 * than merely counted up past.
 */
static ushort
cltag(Cl *c)
{
	if(c->nfree > 0)
		return c->freetag[--c->nfree];
	if(c->tag == NOTAG)
		c->tag = 1;
	return c->tag++;
}

/* the reply carrying this tag has been read: it may be handed out again */
static void
cltagfree(Cl *c, ushort tag)
{
	if(tag != NOTAG && c->nfree < nelem(c->freetag))
		c->freetag[c->nfree++] = tag;
}

static void
clput(Cl *c, Fcall *t)
{
	int n;

	n = convS2M(t, c->wbuf, c->msize);
	if(n <= 0)
		sysfatal("convS2M: %r");
	if(write(c->wfd, c->wbuf, n) != n)
		sysfatal("9P write: %r");
}

/*
 * The next message off the wire, into rbuf.  What r points into stays
 * valid until the next read.
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
	if(convM2S(c->rbuf, n, r) != n)
		sysfatal("convM2S: short message");
	return r->type;
}

/* the i'th reply held aside, taken off the list and decoded */
static int
clpop(Cl *c, int i, Fcall *r)
{
	int n;

	n = c->pend[i].n;
	memmove(c->rbuf, c->pend[i].m, n);
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
		if(c->npend >= nelem(c->pend)){
			fail("%s: %d replies held aside, none for tag %ud",
				clstage, c->npend, tag);
			return -1;
		}
		if((m = malloc(c->rlen)) == nil)
			sysfatal("malloc: %r");
		memmove(m, c->rbuf, c->rlen);
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
clstart(Cl *c, Srvctx *ctx, ulong msize)
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
	if((c->wbuf = malloc(Clbuf)) == nil || (c->rbuf = malloc(Clbuf)) == nil)
		sysfatal("malloc: %r");
	proccreate(clsrvproc, c, Srvstack);

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

static void
clstop(Cl *c)
{
	uvlong np, nd;

	clhangup(c);
	if(!clwaitend(c, 10000)){
		np = nd = 0;
		srvcount(c->ctx, &np, &nd);
		fail("%s: the service loop did not end within 10s "
			"(pushed %llud, done %llud)", clstage, np, nd);
	}
	close(c->rfd);
	close(c->sin);
	close(c->sout);
	c->rfd = c->sin = c->sout = -1;
	free(c->wbuf);
	free(c->rbuf);
	c->wbuf = c->rbuf = nil;
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
 * §5.4.1's Tflush.  The reply this waits for is the Rflush; the
 * flushed request's own answer, which the server sends first, is held
 * aside for the clget or clgettag that asserts it.
 */
static int
clflush(Cl *c, ushort oldtag, Fcall *r)
{
	Fcall t;

	memset(&t, 0, sizeof t);
	t.type = Tflush;
	t.tag = cltag(c);
	t.oldtag = oldtag;
	return clrpc(c, &t, r);
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
