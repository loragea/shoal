/*
 * Helpers shared by the T1 programs that drive the store engine.
 * Each program is still one translation unit; this only keeps three
 * copies of the same scaffolding out of the tree.
 *
 * Include after <u.h>, <libc.h>, <libsec.h>, <fcall.h> and
 * "../lib/shoal.h".
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

/*
 * store.md §7's procs.  The engine takes a spawn callback at open and
 * uses QLock, Rendez and Lock and nothing else, so a T1 program gives
 * it plain rfork procs sharing memory and the 9P server will give it
 * proccreate.  RFMEM shares the data and bss segments — which is
 * where malloc's arena and every Store structure live — and leaves
 * the stack split, which is what makes this a proc rather than a
 * thread.
 *
 * Every proc a T1 program makes is made here — its own workers and,
 * through Storecfg.spawn, the engine's — so this is the one place
 * that can note their pids down.  A test that has to abandon a store
 * (a worker asleep inside it can neither be woken nor freed) still
 * MUST NOT abandon the procs: they hold their end of mk test's pipe
 * open long after the program has reported, and every failing run
 * adds a fresh set.  killspawned is that reaping, by pid.  Not by
 * note group: the group is the one the program inherited from the
 * shell that ran it, so a group note takes mk and the shell down with
 * the workers — and the FAIL lines with them.
 */
enum
{
	Nspawnpid	= 64,
};

static int spawnpid[Nspawnpid];
static int nspawnpid;

static int
spawnproc(void (*fn)(void*), void *a)
{
	int pid;

	switch(pid = rfork(RFPROC|RFMEM|RFNOWAIT)){
	case -1:
		return -1;
	case 0:
		(*fn)(a);
		exits(nil);
	}
	/*
	 * Dropping the pid would silently restore the leak this registry
	 * exists to close, so a full registry stops the program instead.
	 * nspawnpid is a plain counter in RFMEM-shared memory and is not
	 * synchronised: every spawn a T1 program makes is made from its
	 * own main proc — the engine calls Storecfg.spawn only from
	 * storeopen, in the caller — so there is one writer today.
	 */
	if(nspawnpid >= Nspawnpid)
		sysfatal("spawnproc: more than %d procs in one test",
			Nspawnpid);
	spawnpid[nspawnpid++] = pid;
	return 0;
}

/*
 * Start the record afresh, so what killspawned reaps is this test's
 * own procs and not a pid some earlier test's finished worker left
 * behind.  Call before the store whose procs are to be reapable is
 * opened, so its checkpointer is recorded too.
 */
static void
spawnforget(void)
{
	nspawnpid = 0;
}

static void
killspawned(void)
{
	int i;

	for(i = 0; i < nspawnpid; i++)
		postnote(PNPROC, spawnpid[i], "kill");
	nspawnpid = 0;
}

enum
{
	Tsecsz	= 512,
	Tnsec	= 8192,			/* a 4 MiB image */
	Tseed	= 0x5ea1,
};

/*
 * §13's small geometry: a few MiB with nslots and nemap in the
 * hundreds, so that mk test stays within AGENTS.md's seconds.
 */
static void
smallcfg(Fmtcfg *c)
{
	memset(c, 0, sizeof *c);
	c->secsz = Tsecsz;
	c->blksz = 4096;
	c->objmax = 65536;
	c->nslots = 128;
	c->nemap = 32;
	c->ndirty = 64;
	c->logbytes = 64*1024;
	c->csumalg = Csumblake2s;
}

static Dev*
newdisk(void)
{
	Dev *d;
	Super s;
	Fmtcfg c;

	if((d = simopen(Tsecsz, Tnsec, Tseed)) == nil)
		sysfatal("simopen: %r");
	smallcfg(&c);
	if(geometry(&s, &c, d->size) < 0)
		sysfatal("geometry: %r");
	if(fmtstore(d, &s) < 0)
		sysfatal("fmtstore: %r");
	return d;
}

static void
tcfg(Storecfg *c)
{
	memset(c, 0, sizeof *c);
	c->spawn = spawnproc;
	c->nockptproc = 1;		/* T1 drives the checkpointer by hand */
	c->ckwaitms = 200;
	c->emapcache = 16;
	c->stagemax = 8;
	c->stagetot = 12;
	c->stagems = 50;
}

static Store*
openstore(Dev *d)
{
	Storecfg c;

	tcfg(&c);
	return storeopen(d, &c);
}

/*
 * With §2.8's checkpointer proc running, so that a test which drives
 * more commits than the log holds is testing what it means to and not
 * §6's exhaustion.  ckhigh is turned up because the T1 log is 64 KiB.
 */
static Store*
openstoreck(Dev *d)
{
	Storecfg c;

	tcfg(&c);
	c.nockptproc = 0;
	c.ckhigh = 8;
	c.ckms = 50;
	return storeopen(d, &c);
}

static Store*
mustopen(Dev *d, char *what)
{
	Store *s;

	if((s = openstore(d)) == nil){
		fail("%s: storeopen: %r", what);
		return nil;
	}
	return s;
}

static uchar*
mkbuf(long n, int seed)
{
	uchar *p;
	long i;

	if((p = malloc(n)) == nil)
		sysfatal("malloc: %r");
	for(i = 0; i < n; i++)
		p[i] = (uchar)(seed*7 + i*31 + (i>>8)*13);
	return p;
}

static void
oidof(uchar *oid, char *name)
{
	memset(oid, 0, Oidmax);
	memmove(oid, name, strlen(name));
}
