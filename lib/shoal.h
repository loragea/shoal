/*
 * libshoal — code shared by the shoal servers, commands and tests.
 *
 * Include after <u.h>, <libc.h> and <libsec.h>.
 */

/*
 * Object checksums, docs/design/layer-a.md §1.4.
 *
 * Content is divided into blocks of blksz bytes; block i covers
 * [i*blksz, min((i+1)*blksz, len)), so the final block is hashed
 * over its actual length.  A block digest is unkeyed BLAKE2s with a
 * 16-byte digest.  The object checksum is unkeyed BLAKE2s with a
 * 32-byte digest over the concatenation of the block digests in
 * ascending block order; a zero-length object hashes the empty
 * concatenation.  Holes are the caller's problem: they hash as the
 * zero bytes they read as, so a caller presenting object content
 * presents those zeroes.
 */
enum
{
	Blkdlen		= BLAKE2S_128dlen,	/* per-block digest, 16 */
	Csumlen		= BLAKE2S_256dlen,	/* object checksum, 32 */
	Csumhexlen	= 2*Csumlen + 1,	/* rendered csum + NUL */
	Blkszdflt	= 16384,		/* map default blksz */
};

/*
 * Accumulator over block digests.  Feeding every block digest of an
 * object in ascending order and finishing yields that object's csum;
 * an instance that keeps its block digests durably (§1.4) re-hashes
 * one block and replays them, rather than re-reading the object.
 */
typedef struct Csum Csum;
struct Csum
{
	DigestState	*s;
	int		n;	/* digests absorbed */
};

uvlong	blkcount(uvlong len, ulong blksz);
void	blkdigest(uchar *p, ulong n, uchar dig[Blkdlen]);

void	csuminit(Csum *c);
void	csumadd(Csum *c, uchar dig[Blkdlen]);
void	csumfinal(Csum *c, uchar csum[Csumlen]);

void	csumdigests(uchar *digs, uvlong nblk, uchar csum[Csumlen]);
uvlong	objdigests(uchar *p, uvlong len, ulong blksz, uchar *digs);
void	objcsum(uchar *p, uvlong len, ulong blksz, uchar csum[Csumlen]);

char*	csumfmt(char *buf, uchar csum[Csumlen]);

/*
 * The rest of this header is the local object store,
 * docs/design/store.md.  Anything that packs or unpacks an on-disk
 * integer must also include <fcall.h> before this file: that is where
 * 9front exports the GBIT/PBIT macros store.md §0 requires, and
 * nothing here redefines them.
 */

/*
 * The device interface, store.md §0.  Every device access in the
 * store goes through this vtable; nothing else calls pread, pwrite or
 * opens /dev/sdXX/raw.  Three implementations: the real sd(3)
 * partition (sdopen), a plain file (fileopen), and the simulated disk
 * the T1 tests drive (simopen).
 */
typedef struct Dev Dev;
typedef struct Devops Devops;

enum
{
	/*
	 * Error classes.  store.md §0: `interrupted' means this
	 * request has been flushed, not that the media failed, and
	 * Echange means the unit's partitions were re-declared under
	 * an open fid — neither is corruption.
	 */
	Denone	= 0,
	Deio,
	Deintr,
	Dechange,
};

enum
{
	/* open flags */
	Dnoflush	= 1<<0,	/* §3.2's -w: run without the flush channel */
	Drdonly		= 1<<1,	/* read-only: no raw channel, no write */
};

/*
 * What the device can say about durability, which is what shoalck
 * prints and what /status reports (§3.2, §12).  The four states are
 * not two: a tool that never opened the raw channel has not observed
 * write-through, and must not claim the operator asserted it.
 */
enum
{
	Fnone	= 0,		/* no flush channel exists: a file image */
	Fraw,			/* the sd(3) raw channel is open */
	Fasserted,		/* -w: the operator asserted write-through */
	Funknown,		/* not opened, so not known: a read-only tool */
};

char*	flushname(int mode);

struct Devops
{
	long	(*read)(Dev*, void*, long, vlong);
	long	(*write)(Dev*, void*, long, vlong);
	int	(*flush)(Dev*);
	void	(*point)(Dev*, char*, int);
	void	(*close)(Dev*);
};

struct Dev
{
	Devops	*ops;
	char	*name;		/* for diagnostics */
	ulong	secsz;
	vlong	size;		/* usable bytes, a whole number of sectors */
	ulong	wunit;		/* §0's Wunit: the largest single write */
	int	flushmode;
	int	rdonly;
	int	changed;	/* Echange was seen on this fid (§0) */
	void	*aux;
};

/*
 * Loop-until-complete wrappers (store.md §0).  devsd truncates a
 * request rather than splitting or failing, so a short count is
 * normal; these loop, and fail only on a real error or on no
 * progress.  devwrite also splits: §0 forbids a single pwrite larger
 * than the device's wunit, so a longer write is issued as wunit
 * pieces rather than refused, which is what lets a blksz above the
 * device's write unit be formatted at all (§2.1).  They return 0 or
 * -1 with the error string set; deverr classifies that string.
 */
int	devread(Dev*, void*, long, vlong);
int	devwrite(Dev*, void*, long, vlong);
int	devflush(Dev*);
int	devzero(Dev*, vlong off, vlong n, ulong unit);
void	devclose(Dev*);
int	deverr(void);
int	devclass(Dev*);		/* deverr, condemning the fid on Echange (§0) */
int	devwriteretry(Dev*, void*, long, vlong);
int	devflushretry(Dev*);

/*
 * A fault-injection point (store.md §13).  Inert on every device but
 * the simulated one, and inert there until simarm names it.
 */
void	devpoint(Dev*, char*, int);

/*
 * A path names an sd(3) partition when the directory holding it is an
 * sd unit — it has the unit's own ctl and raw files — and a plain file
 * otherwise (§12).  The spelling of the path decides nothing: an sd
 * unit is bound at #S in a cpu namespace as often as at /dev.
 */
int	sdpart(char *path);

Dev*	sdopen(char *part, int flags);
Dev*	fileopen(char *path, ulong secsz, vlong size, int flags);
Dev*	simopen(ulong secsz, uvlong nsec, ulong seed);

/*
 * Simulated-disk controls, store.md §13.  The sim models a volatile
 * write cache: a written sector is visible to reads at once but is
 * durable only after a flush, and a crash decides sector by sector
 * which of the two a dirty sector keeps.  Faults are armed for the
 * next n operations (n <= 0 arms them until disarmed); at most eight
 * may be armed at once — a ninth is a sysfatal — and simfaultat aims
 * one at a byte range.  Arming Sfnone disarms every one of them, and
 * so does a crash.
 *
 * One device operation takes at most one fault: the first armed one
 * it can take, which is then consumed unless it is sticky.  A short
 * count and a tear therefore apply to one devwrite rather than to one
 * request — the wrapper loops, and the next iteration takes the next
 * fault.
 */
enum
{
	Sfnone	= 0,
	Sfshort,	/* truncate the request to a seeded partial count */
	Sftearsec,	/* the write lands in a seeded subset of its sectors */
	Sftearbyte,	/* each sector lands as a seeded mix of old and new */
	Sfdrop,		/* the write lands nowhere and reports success */
	Sfeio,
	Sfechange,
	Sfintr,		/* `interrupted': the request was flushed, §0 */
};

/*
 * What a crash does with the sectors written since the last flush.
 * Scdrop is the default and is what a device that lost its whole
 * cache does; the others are how §13's schedules are staged, since a
 * torn commit header and a header on the platter with its grain still
 * in the cache both need a chosen subset to survive.
 */
enum
{
	Scdrop	= 0,	/* every dirty sector reverts */
	Sckeep,		/* every dirty sector survives */
	Scsome,		/* a subset chosen from the seed survives */
	Scnamed,	/* the sectors simcrashkeep named survive */
};

/* recorded device operations, in issue order */
enum
{
	Sopread	= 0,
	Sopwrite,
	Sopflush,
	Sopcrash,
};

typedef struct Simop Simop;
struct Simop
{
	int	op;
	vlong	off;
	long	n;
};

void	simfault(Dev*, int kind, int n);
void	simfaultat(Dev*, int kind, int n, vlong off, vlong len);
void	simcrash(Dev*);
void	simcrashdead(Dev*, int on);	/* a crash stops the device */
void	simrevive(Dev*);		/* ... until the machine comes back */
void	simcrashmode(Dev*, int mode);
void	simcrashkeep(Dev*, vlong off, vlong len);
void	simarm(Dev*, char *point, int n);
void	simslow(Dev*, int on);		/* yield under the lock; see §13 */
void	simpoke(Dev*, vlong off, void *buf, long n);	/* to durable storage */
void	simpeek(Dev*, vlong off, void *buf, long n);	/* from durable storage */
uvlong	simdirty(Dev*);				/* sectors written but not flushed */
long	simtrace(Dev*, Simop**);	/* good until the next operation */
void	simtracereset(Dev*);

/*
 * On-disk structures, store.md §2.  Every one of them carries a
 * BLAKE2s-128 checksum over its whole byte range with the checksum
 * field itself zeroed (§0); reccsumset and reccsumok are that rule.
 */
enum
{
	Storevers	= 1,		/* format version of every header */
	Csumblake2s	= 1,		/* csumalg: layer-a's blake2s256 */

	Recsumlen	= Blkdlen,	/* 16, a record checksum */

	Secszdflt	= 512,
	Blkszstore	= 16384,	/* §2.1: the default blksz, layer-a's */
	Blkszmax	= 1024*1024,	/* §2.1: the format's blksz ceiling */
	Wunitdflt	= 16384,	/* §0's Wunit on the reference unit */
	Objmaxdflt	= 16*1024*1024,
	Ndirtydflt	= 65536,
	Logbytesdflt	= 64*1024*1024,
	Nslotsmax	= 1<<20,	/* §2.1's cap on the nslots default */

	Idxentsz	= 256,		/* §2.3 */
	Dirtentsz	= 256,		/* §2.6 */
	Emaphdrsz	= 24,		/* §2.4, before the grain array */
	Bmhdrsz		= 48,		/* §2.5, before the bits */
	Lrechdrsz	= 56,		/* §2.7, the record header */
	Lenthdrsz	= 8,		/* §2.7, {u8 kind, u8 flags, u16 pad, u32 len} */
	Objfixed	= 84,		/* §2.7, Eobj bytes before oid[oidlen] */

	Oidmax		= 128,		/* layer-a §1.2 */
	Peermax		= 72,		/* §2.6: node name 63 + '.' + 8 digits */
};

void	reccsumset(uchar *p, ulong n, ulong csumoff);
int	reccsumok(uchar *p, ulong n, ulong csumoff);

/* the superblock, §2.2 */
typedef struct Super Super;
struct Super
{
	ulong	vers;
	ulong	hdrlen;		/* bytes covered by csum; secsz */
	uvlong	gen;
	uchar	uuid[16];
	vlong	ctime;
	ulong	secsz;
	ulong	blksz;
	uvlong	objmax;
	ulong	nblkmax;
	ulong	emapsz;
	ulong	nslots;
	ulong	nemap;
	ulong	ndirty;
	uvlong	ngrains;
	uvlong	logoff, logsecs;
	uvlong	idxoff, idxsecs;
	uvlong	emapoff, emapsecs;
	uvlong	dirtoff, dirtsecs;
	uvlong	bmapoff, bmapsecs;
	uvlong	dataoff, datasecs;
	uvlong	ckseq;
	uvlong	cklogoff;
	uvlong	qidnext;
	uvlong	epochhigh;
	uchar	monid[16];
	ulong	monidset;
	ulong	csumalg;
};

void	superpack(uchar *p, Super *s);
int	superunpack(Super *s, uchar *p, ulong secsz);
uvlong	nbmpage(Super *s);
uvlong	bmbits(ulong pagesz);
vlong	grainoff(Super *s, ulong grain);
uvlong	emapentoff(Super *s, ulong slot);
uvlong	idxentoff(Super *s, ulong slot);
uvlong	dirtentoff(Super *s, ulong slot);

/*
 * The two-slot rule, §2.2, stated in three clauses: exactly one valid
 * copy means the update writes the invalid one; both valid means it
 * writes the lower gen; neither valid means it MUST NOT write and
 * MUST NOT serve.  superselect reads both copies and reports the
 * choice and the reason for it, which is what shoalck prints.
 */
typedef struct Sbsel Sbsel;
struct Sbsel
{
	Super	sb[2];
	int	valid[2];
	char	why[2][ERRMAX];	/* why a copy is invalid */
	int	start;		/* copy to start from, -1 if neither */
	int	victim;		/* copy the next update writes, -1 to refuse */
	int	clause;		/* which of §2.2's three clauses decided */
	uvlong	nextgen;
};

int	superselect(Dev*, Sbsel*);
vlong	super1off(Dev*);

/* the index entry, §2.3 */
enum
{
	Sfree	= 0,
	Slive	= 1,
	Stomb	= 2,

	Icorrupt = 1<<0,	/* index entry flags bit0, layer-a §7.5 */
};

typedef struct Idxent Idxent;
struct Idxent
{
	uchar	state;
	uchar	oidlen;
	uchar	flags;
	uchar	vers;
	ulong	emapslot;	/* 0 = none: the map is inline */
	uvlong	qidpath;
	uvlong	len;
	uvlong	ver;
	uvlong	wepoch;
	vlong	mtime;
	uchar	csum[Csumlen];
	uchar	oid[Oidmax];
	ulong	grain0;		/* inline map: 0 = hole */
	uchar	dig0[Blkdlen];
};

void	idxpack(uchar *p, Idxent *e);
int	idxunpack(Idxent *e, uchar *p, ulong nemap);

/* the extent-map entry, §2.4 */
typedef struct Emap Emap;
struct Emap
{
	ulong	nblk;
	ulong	vers;
};

void	emappack(uchar *p, ulong emapsz, Emap *m);
int	emapunpack(Emap *m, uchar *p, ulong emapsz, ulong nblkmax);
ulong	emapgrain(uchar *p, ulong i);
void	emapsetgrain(uchar *p, ulong i, ulong grain);
uchar*	emapdig(uchar *p, ulong nblkmax, ulong i);

/* a free-grain bitmap page, §2.5 */
typedef struct Bmpage Bmpage;
struct Bmpage
{
	ulong	vers;
	ulong	page;
	uvlong	ckseq;
};

void	bmpack(uchar *p, ulong pagesz, Bmpage *h);
int	bmunpack(Bmpage *h, uchar *p, ulong pagesz, ulong page);
int	bmget(uchar *p, uvlong bit);
void	bmset(uchar *p, uvlong bit);
void	bmclr(uchar *p, uvlong bit);

/* a dirty record, §2.6 */
typedef struct Dirtent Dirtent;
struct Dirtent
{
	uvlong	epoch;
	uchar	state;
	uchar	oidlen;
	uchar	peerlen;
	uchar	vers;
	uchar	oid[Oidmax];
	uchar	peer[Peermax];
};

void	dirtpack(uchar *p, Dirtent *e);
int	dirtunpack(Dirtent *e, uchar *p);

/* the log record header and the entry stream, §2.7 */
enum
{
	Fwrap	= 1<<0,		/* record flags bit0 */

	Kobj	= 1,		/* entry kinds */
	Kdirty	= 2,
	Kslot	= 3,

	Oslot	= 1<<0,		/* Eobj oflags bit0: this commit changes emapslot */
	Ocorrupt = 1<<1,	/* bit1: the corrupt flag this commit publishes */
};

typedef struct Lrec Lrec;
struct Lrec
{
	ulong	vers;
	ulong	nsec;
	uvlong	seq;
	vlong	time;
	ulong	nent;
	ushort	flags;
};

void	lrecpack(uchar *p, Lrec *r, ulong secsz);
int	lrecunpack(Lrec *r, uchar *p);
int	lrecvalid(uchar *p, ulong secsz, Lrec *r, uvlong off, uvlong logsecs,
		uvlong seq);

typedef struct Lent Lent;
struct Lent
{
	uchar	kind;
	uchar	flags;
	ulong	len;		/* whole entry, this 8-byte header included */
	uchar	*body;		/* len - Lenthdrsz bytes */
};

int	lentunpack(Lent *e, uchar *p, long n);
void	lentpack(uchar *p, int kind, int flags, ulong len);

typedef struct Mapent Mapent;
struct Mapent
{
	ulong	blk;
	ulong	grain;
	uchar	dig[Blkdlen];
};

typedef struct Objrec Objrec;
struct Objrec
{
	ulong	slot;
	ulong	emapslot;
	uvlong	qidpath;
	uchar	state;
	uchar	oidlen;
	uchar	oflags;
	uvlong	len;
	uvlong	ver;
	uvlong	wepoch;
	vlong	mtime;
	uchar	csum[Csumlen];
	uchar	oid[Oidmax];
	ulong	nmap;
	Mapent	*map;
	ulong	nfree;
	ulong	*freed;
};

typedef struct Dirtyrec Dirtyrec;
struct Dirtyrec
{
	uchar	op;		/* 0 remove, 1 add */
	uchar	peerlen;
	uchar	oidlen;
	uvlong	epoch;
	uchar	peer[Peermax];
	uchar	oid[Oidmax];
};

ulong	objreclen(Objrec *o);
long	objrecpack(uchar *p, long max, Objrec *o);
int	objrecunpack(Objrec *o, uchar *p, long n);
void	objrecfree(Objrec *o);

ulong	dirtyreclen(Dirtyrec *d);
long	dirtyrecpack(uchar *p, long max, Dirtyrec *d);
int	dirtyrecunpack(Dirtyrec *d, uchar *p, long n);

long	slotrecpack(uchar *p, long max, ulong slot);
int	slotrecunpack(ulong *slot, uchar *p, long n);

/*
 * Geometry and format, §2.1 and §12.
 */
typedef struct Fmtcfg Fmtcfg;
struct Fmtcfg
{
	ulong	secsz;
	ulong	blksz;
	uvlong	objmax;
	ulong	nslots;		/* 0: the §2.1 default */
	ulong	nemap;		/* 0: the §2.1 default */
	ulong	ndirty;		/* 0: the §2.1 default */
	uvlong	logbytes;	/* 0: the §2.1 default */
	ulong	csumalg;
	uchar	uuid[16];
	int	uuidset;
};

int	geometry(Super *s, Fmtcfg *c, vlong partbytes);
uvlong	maxrecbytes(Super *s);	/* the largest Eobj record, §2.7 */
int	fmtstore(Dev *d, Super *s);
char*	csumalgname(ulong alg);
ulong	csumalgno(char *name);

/*
 * Inspection and checking, §12.  shoalck is a front end over this.
 */
typedef struct Ckcfg Ckcfg;
struct Ckcfg
{
	int	verbose;	/* -l: dump log records */
	int	quiet;		/* report problems only */
	char	*oid;		/* -o: dump one object */
	int	verify;		/* -v: verify every object's content (§8) */
	int	rebuild;	/* -R: rebuild the bitmap, rewrite the checkpoint */
	int	noflush;	/* -w: §3.2's assertion, which -R's open needs */
	int	out;		/* fd for the report */
};

int	ckstore(Dev *d, Ckcfg *c);

/*
 * The store engine, store.md §3 to §7: start-up and replay, the log
 * commit path, group commit, the checkpointer, the allocator, the
 * superblock publisher and a library-level object API.  The 9P
 * surface is not here — it is layer-a's, and it is what cmd/shoalsrv
 * will be.
 *
 * Procs.  The engine needs procs of its own — a flusher and a
 * checkpointer — and its callers put several more on it, so it must
 * run both in a plain-libc T1 program and inside a libthread 9P
 * server.  It therefore takes a spawn callback at open and uses
 * QLock, Rendez and Lock and nothing else: a T1 program passes an
 * rfork(RFPROC|RFMEM) wrapper, and the server will pass proccreate.
 * The engine's procs call the Dev directly, which is proc-safe, so
 * there is no Ioproc anywhere in lib/ and none in the device vtable.
 *
 * Per-object ordering (layer-a §5.4.1, R14) is the caller's: in the
 * server it is store.md §7's Reqqueue pool, and a T1 program keeps
 * one proc per object.  The engine serialises the state every proc
 * shares (§7's four QLocks) and nothing else.
 */
typedef struct Store Store;
typedef struct Storecfg Storecfg;
typedef struct Storestat Storestat;
typedef struct Objinfo Objinfo;
typedef struct Stage Stage;
typedef struct Objsnap Objsnap;

/*
 * Store and Stage are opaque outside lib/: their definitions are in
 * lib/store.h.  2c(1)'s type signatures are computed from the C
 * signof operator, so a function taking a Store* signs differently in
 * a file that has the definition and one that has not; the pragma is
 * what that mechanism provides for exactly this case.
 */
#pragma incomplete Store
#pragma incomplete Stage
#pragma incomplete Objsnap

enum
{
	Logdepthdflt	= 4,		/* §7's semaphore */
	Logdepthmax	= 8,
	Ckmsdflt	= 30000,	/* §2.8 */
	Ckhighdflt	= 4,		/* checkpoint past logsecs/ckhigh used */
	Ckwaitmsdflt	= 5000,		/* §6's bounded wait */
	Ckbackmsdflt	= 100,		/* §2.8's retry floor after a failure,
					 * capped at Ckwaitmsdflt here */
	Logresvdiv	= 16,		/* §6's reserved tail */
	Stagemaxdflt	= 2048,		/* §3.6, grains per stage */
	Stagetotdflt	= 16384,	/* §3.6, grains per process */
	Stagemsdflt	= 30000,
	Emapcachedflt	= 4096,		/* §9's LRU */
	Objsnapmaxdflt	= 8,		/* §9's concurrent-snapshot bound */
	Qidbatch	= 1024,		/* §2.2's qidnext batch */
};

struct Storecfg
{
	int	(*spawn)(void (*)(void*), void*);
	int	noflush;		/* §3.2's -w */
	int	nockptproc;		/* no checkpointer proc: T1 drives it */
	int	forcerebuild;		/* §12's shoalck -R: rebuild the free
					 * map from the live maps whatever the
					 * bitmap's own checksums say */
	ulong	logdepth;
	ulong	ckms;
	ulong	ckhigh;
	ulong	ckwaitms;
	ulong	ckbackms;		/* §2.8's retry floor: doubling per
					 * consecutive failure, capped at
					 * max(ckbackms, min(ckms, ckwaitms)) */
	ulong	stagemax, stagetot, stagems;
	ulong	emapcache;
	ulong	objsnapmax;		/* §9's bound on open snapshots */
	/*
	 * §13's free-observation hook: called by the engine as its last
	 * act before the Store's own memory goes, whichever path
	 * released it — a storeopen that failed, storeclose, or the
	 * last objsnapclose of a snapshot that outlived one (§9).  Inert
	 * when nil, which is what every caller but a test leaves it.
	 * It exists because the deferred free has no other observable:
	 * a read through a snapshot whose Store was freed early answers
	 * correctly out of freed memory, so a test that watched answers
	 * alone would pass the use-after-free.
	 */
	void	(*freed)(void*);
	void	*freedarg;
};

struct Storestat
{
	int	flushmode;
	int	bmaprebuild;
	uvlong	ckseq, cklogoff;
	uvlong	watermark, seqnext;
	uvlong	qidnext, epochhigh;
	int	monidset;
	uvlong	grainfree, staged;
	uvlong	grainleak;		/* §6: marked, named by nothing */
	/*
	 * §8's online bitmap rebuild (D18), as a /status renderer wants
	 * it: whether a pass is running, how far it has got, and what
	 * the last one cost.  bmfolded and bmreread are the live pass's
	 * while one runs and the last pass's once it has ended, so a
	 * finished pass still says what it did.  bmfolded counts the
	 * folds that COMPLETED for a `live` slot, so it is progress
	 * against nlive and a free or tomb slot adds nothing to it;
	 * bmreread counts the folds a slot that moved under the walk
	 * sent round again, which is how much the pass is fighting
	 * write traffic.  bmfolding is
	 * the folds holding a map read right now, and it is the live
	 * pass's alone: an end refuses while any is outstanding.
	 * bmswapped is the bitmap pages the last completed swap
	 * installed — of Storestat's nothing else, so a caller that
	 * wants it as a fraction reads §2.5's page count from the
	 * superblock.
	 */
	int	bmpass;			/* a rebuild pass is live */
	uvlong	bmfolded;		/* live slots folded, completed */
	uvlong	bmfolding;		/* folds holding a map read */
	uvlong	bmreread;		/* folds sent round again by the stamp */
	uvlong	bmswapped;		/* bitmap pages the last swap installed */
	uvlong	slotfree, emapfree;
	uvlong	logfree;		/* sectors */
	uvlong	logwait;		/* commits in §6's wait for log space */
	int	broken;			/* a log write failed: §3.2 */
	uvlong	nslots;			/* the index's size, for §8's cursor */
	uvlong	nlive, ntomb, nlost;	/* nlost: /lost, §8 */
	uvlong	nobjsnap;		/* §9's open object snapshots */
	uvlong	ndirty, ndirtydrop;
	uvlong	nreplay, pmax;
	/*
	 * §2.8's checkpointer, whose failures no client operation
	 * reports.  A store whose checkpoints fail reclaims no log
	 * space, so ckstuck — the LAST checkpoint failed and none has
	 * succeeded since — is what tells a full log from a stuck one,
	 * and ckerr is what that one said (empty when not stuck).
	 * ckfailed counts failed ATTEMPTS over the store's life: a
	 * stuck store re-attempts no faster than §2.8's retry floor, so
	 * it is a rate of retrying rather than a count of distinct
	 * outages.
	 *
	 * ckdead is the stronger condition: the failing checkpoint's
	 * device fid is condemned (§0's Echange), so no later checkpoint
	 * can succeed and nothing short of closing and opening the store
	 * clears it.  A dead checkpointer stops attempting, so ckfailed
	 * stands still while ckdead is set.
	 */
	int	ckstuck;
	int	ckdead;
	uvlong	ckfailed;
	char	ckerr[ERRMAX];
};

struct Objinfo
{
	ulong	slot;
	ulong	emapslot;
	uvlong	qidpath, len, ver, wepoch;
	vlong	mtime;
	uchar	csum[Csumlen];
	int	state;
	int	corrupt;
};

Store*	storeopen(Dev*, Storecfg*);
/*
 * Stop the procs and give the store up; it writes nothing, and the
 * device stays the caller's to close.  The Store* is INVALID the
 * moment this returns — every call below takes one, and none of them
 * may be made afterwards.
 *
 * QUIESCE FIRST: a call taking a Store* must not be in flight when
 * this one runs, either.  objsnapopen before it reaches §9's bound,
 * and dirtysnap, lostsnap, fullsyncsnap, storestat and the object
 * API throughout, block on the store's state lock holding nothing
 * that keeps the Store alive, so one queued on that lock when the
 * last release frees the Store wakes inside freed memory.  The
 * engine cannot close that window — a waiter would have to be
 * counted under the lock it is waiting for — so the caller MUST have
 * stopped issuing such calls BEFORE it calls this, and MUST make
 * none after.
 *
 * The single exception is an object snapshot (objsnapopen, below): a
 * snapshot MAY outlive this call.  It still names a Store that is
 * still there, objsnapent through it then fails `store closed', and
 * the store's memory is released when the LAST such snapshot is
 * closed.  So a caller that closes the store first loses its listing
 * and nothing else: no fatal, and no snapshot answering "that entry
 * is gone" for every entry out of freed memory.
 */
void	storeclose(Store*);
int	storecheckpoint(Store*);
void	storestat(Store*, Storestat*);
void	storehook(Store*, char *name, uvlong n);	/* §13's -X hooks */
/*
 * /lost, layer-a §7.5: every copy this instance holds that fails
 * local verification — §5 step 10's condemned slots and §8's
 * corrupt-flagged entries alike.  storelost answers the i'th slot, or
 * ~0 past the end; Storestat.nlost is how many there are.  The two
 * are read together and the list moves under a concurrent scrub, so
 * this is a cursor over the live list rather than a picture of it: a
 * caller that needs a picture takes lostsnap's copy (below).
 */
ulong	storelost(Store*, ulong i);
int	storefullsync(Store*, char *peer);

/*
 * §8's online bitmap rebuild (D18): declarations are added here by
 * the bitmap-rebuild work and nowhere else in this header.
 */
/* --- bitmap-rebuild: begin --- */
/*
 * The pass is driven by these four calls.  bmpassbegin allocates the
 * shadow bitmap and arms the write barrier: from then until the pass
 * ends, every grainmark and grainclear mirrors into the shadow, so
 * the live bitmap and the shadow stay current together and the swap
 * can be chunked.  One pass runs at a time; a second begin refuses.
 *
 * bmpassfold folds one slot.  It reads that slot's extent map outside
 * the state lock under the map cache's pin, exactly as every other
 * map read does, validates the entry it re-reads by its per-slot
 * generation stamp — reading again if the slot moved under it — and
 * ORs the grains the map names into the shadow.  A free slot folds to
 * nothing, and so does a slot §5 step 10 condemned: its map is the
 * damage, and the grains it named are exactly what the pass reclaims.
 * A slot whose map fails its checksum is condemned here, as it is by
 * every other reader of a map.  Each call is one object's worth of
 * work, so a server may push it through that object's Reqqueue (§8);
 * the order slots are folded in is the caller's, and a slot folded
 * twice is folded twice to no ill effect.
 *
 * bmpassend swaps the shadow in page by page under the state lock,
 * installing and dirtying only the pages that differ — §5 step 11's
 * rebuild dirties every page, which on a serving store is §2.5's
 * whole bitmap — and moves §6's free count by what each installed
 * page changed.  *npage, when not nil, is how many pages it
 * installed.  bmpassabort drops the shadow and disarms the barrier,
 * leaving the live bitmap exactly as it was.  An abort while an end
 * is installing pages does nothing at all: the end is mid-swap, it
 * drops the pass itself a moment later, and a half-installed bitmap
 * is the one state this mechanism has no name for.
 *
 * **An end is refused unless the walk covered the store.**  The swap
 * frees every grain the shadow does not mark, so a shadow the walk
 * did not finish would free grains a live map still names.  The pass
 * therefore carries a mark per index slot, and bmpassend refuses
 * unless every Slive slot carries one and no fold is in flight.  A
 * fold sets the mark for the slot it completes; so does an apply
 * whose record rebuilds the slot's map whole, because those grains
 * reach the shadow through the barrier.  A free slot and a tombstone
 * need no mark: a tombstone's Eobj carries len=0, an empty nmap and
 * emapslot=0 (§6), so neither names a grain.  A refused end installs
 * nothing and leaves the pass live: the caller folds the slots the
 * error names and ends again.
 *
 * **grainleak is what the swap did not reclaim, not zero.**  A leak
 * recorded while the pass was live and after it had folded that slot
 * is a leak the swap installs rather than returns — the fold had
 * already put those grains in the shadow — so the end leaves that
 * much standing and discharges the rest.
 *
 * The three that answer do so with 0, or -1 with an error set: on a
 * condemned store (§3.2), on a slot out of range, on a fold or an end
 * with no pass running, on an end the walk did not cover, and on a
 * begin with one already running.  bmpassabort answers nothing: it
 * has nothing to refuse and nothing to fail at.  A fold that could
 * not read a map says so ("map read"), which is the caller's cue to
 * fold that slot again rather than to give the pass up.  None of them
 * is a §2.6 wire condition, so none carries a §2.6 prefix (§3.7).
 *
 * **A pass MUST be ended or aborted before storeclose** — every call
 * on a closed store is undefined (D16), so no pass can outlive one.
 * A pass still live when storeclose runs is aborted by it, because
 * the shadow is the store's memory and goes with the rest; that is
 * the engine tidying up after a caller, not a way to leave one open,
 * and a call in flight in another proc when the close runs is
 * undefined exactly as any other call in flight is — a bmpassend
 * among them, which leaves the bitmap holding however many of its
 * pages had landed.
 */
int	bmpassbegin(Store*);
int	bmpassfold(Store*, ulong slot);
int	bmpassend(Store*, uvlong *npage);
void	bmpassabort(Store*);
/* --- bitmap-rebuild: end --- */

/* §2.2's publisher: durable before the value is acted on */
uvlong	qidalloc(Store*);
int	epochadopt(Store*, uvlong epoch);
int	monidpin(Store*, uchar id[16]);

/*
 * The object API.  Digest and csum handling is §4's.
 *
 * **Per-object serialisation is the caller's** (§3.1, §4, §7): the
 * functions below serialise only the state every proc shares, so two
 * concurrent writers to one object read the same old map and both
 * free the same grains.  The server's Reqqueue pool (§7) is what
 * orders them; a T1 program uses one proc per object.
 *
 * Every mutating call but objdiscard takes the Edirty records layer-a
 * §5.4 step 5b asks for, because §14(2) puts them in the same log
 * record as the update they belong to: either both are durable or
 * neither.  A separate dirtyadd is a second record, and a crash
 * between the two leaves the update durable and the stale mark absent
 * — layer-a §5.4's `degraded' case, arrived at silently.  objcreate
 * is in that set because layer-a §2.4 replicates a create like any
 * other write; objdiscard is not, because a discard leaves no peer
 * behind to mark: layer-a §1.5 has the primary remove its own record
 * *last*, after every holder has answered ok, and retry the whole
 * discard otherwise.
 *
 * objdiscard names the tombstone's key (ver, wepoch) and the caller's
 * current map epoch, and refuses `not discardable' unless its record
 * is a tombstone at exactly that key with wepoch strictly below the
 * epoch — layer-a §1.5's receiver checks.  The three checks are
 * atomic among themselves (one hold of the state lock), so they judge
 * one record and a separate objstat could not; the window between
 * the checks and the commit is the caller's per-oid queue's to close,
 * exactly as for every other mutation above.
 */
int	objstat(Store*, uchar *oid, int oidlen, Objinfo*);
int	objcreate(Store*, uchar *oid, int oidlen, uvlong ver, uvlong wepoch,
		Dirtyrec *dr, int ndr, Objinfo*);
long	objread(Store*, uchar *oid, int oidlen, void *a, long n, uvlong off);
int	objwrite(Store*, uchar *oid, int oidlen, void *a, long n, uvlong off,
		uvlong ver, uvlong wepoch, Dirtyrec *dr, int ndr);
int	objtrunc(Store*, uchar *oid, int oidlen, uvlong len, uvlong ver,
		uvlong wepoch, Dirtyrec *dr, int ndr);
int	objremove(Store*, uchar *oid, int oidlen, uvlong ver, uvlong wepoch,
		Dirtyrec *dr, int ndr);
int	objdiscard(Store*, uchar *oid, int oidlen, uvlong ver, uvlong wepoch,
		uvlong epoch);
int	objcorrupt(Store*, uchar *oid, int oidlen, int set, Dirtyrec *dr,
		int ndr);

/*
 * Verify, §8.  It answers the set of mismatching block indices, and
 * separately whether the digest array itself is suspect — the two
 * need different repairs, so a boolean would be the wrong answer.
 */
typedef struct Vfy Vfy;
struct Vfy
{
	int	arraybad;	/* hash(dig[]) != csum */
	ulong	nbad;
	ulong	*bad;		/* nbad block indices */
};
int	objverify(Store*, uchar *oid, int oidlen, Vfy*);
/*
 * vfyfree is always safe after objverify or objscrub, whatever they
 * returned, and safe twice: a failure from either leaves the Vfy
 * zeroed, so the bad-block array a partial pass allocated is never
 * the caller's to lose.
 */
void	vfyfree(Vfy*);

/*
 * §8's scrub: objverify plus the one durable transition it licenses —
 * a mismatch on a copy the index calls whole sets the corrupt flag, a
 * copy the index calls corrupt whose every block matches clears it,
 * and anything else commits nothing.  The Vfy is answered either way,
 * because which repair to ask for is what it says, and the caller
 * frees it with vfyfree.  objverify stays pure: it is also what
 * layer-a §5.6's op=verify and shoalck -v need.
 *
 * The rate limit, the proc and the pass are the server's (§8): this
 * is one object, called from the caller's per-object queue like every
 * other call here.
 */
int	objscrub(Store*, uchar *oid, int oidlen, Vfy*);

/*
 * §8's slot cursor, so a scrubber can walk the index in order.  Given
 * a slot below Storestat.nslots it answers 1 for a live or tomb entry
 * — filling oid (up to Oidmax bytes), *oidlen and *oi — 0 for a free
 * slot, and -1 for a slot out of range or a condemned store.  It
 * holds the state lock for the copy alone, so a caller may verify
 * between two calls; what it answers is a snapshot of a slot and not
 * a lease on it, so every call the caller then makes names the oid
 * rather than the slot.
 */
int	objslot(Store*, ulong slot, uchar *oid, int *oidlen, Objinfo*);

/*
 * §9's snapshot-at-open enumeration: what a server's /obj, /tombs and
 * /advert fids read, and layer-a §2.2's SHOULD for the first two.
 *
 * objsnapopen takes the vector of {slot, qid.path} of every index
 * entry whose state is in kinds — Snaplive for /obj, Snaptomb for
 * /tombs, both for /advert — and holds no lock once it returns.  12
 * bytes an entry, §9's sizing.  The snapshot holds no reference the
 * engine must honour: it is a list of names, and an entry a later
 * discard removes simply becomes gone.
 *
 * It takes TWO holds of the state lock to do that, one per step —
 * the count, the vector allocated outside any hold, the fill — and
 * two more for every re-count the index forces by growing past the
 * vector's slack in between, which is why it can fail with `object
 * snapshot: the index moved under 8 counts'.  The bound below costs
 * no hold of its own: it is tested and taken inside the first count's.
 * A refused open is 2·Snaptries + 1 holds, the last of them giving
 * the bound's slot back.  That failure is pathological and not
 * ordinary: the vector is allocated with slack over the count, so a
 * create rate would have to outrun a malloc eight times running to
 * provoke it.  It is not a layer-a §2.6
 * condition — nothing is full and nothing is broken — so it carries
 * no §2.6 prefix (§3.7), and a server SHOULD retry the open once
 * before answering a client at all.
 *
 * Access is by POSITION, not by slot, which is what lets a server map
 * a Tread offset onto an entry and restart from 0 on a re-read, the
 * way a Plan 9 directory read works.  objsnapent answers entry i: 1
 * with the oid, *oidlen and the Objinfo rendered from the LIVE index
 * under a short hold of the state lock, exactly as objslot does; 0
 * when that entry is gone; -1 past the end or on a condemned store.
 * objsnapcount is the number of entries, and it does not change: an
 * object created after the open is not in the vector at all.
 *
 * **Gone is two conditions.**  The slot's qid.path no longer matches
 * the snapshot's — the object was discarded and the slot reused, or
 * the slot was freed — OR the slot's state is no longer in the
 * snapshot's kinds.  The second is not a refinement of the first:
 * §2.3 keeps an object's qid.path across delete, tombstone and
 * re-create, so a live object deleted after a /obj open still matches
 * on qid.path and is now a tombstone, which layer-a §2.2 says /obj
 * MUST NOT list; a tombstone re-created over after a /tombs open
 * matches too and is now live.
 *
 * The number of snapshots open at once is bounded by Storecfg's
 * objsnapmax (§9: policy, default Objsnapmaxdflt), because the cost
 * is per open fid; an open past it answers `disk full' (layer-a
 * §2.6).  The bound is tested and the count taken in one step under
 * one hold of the state lock — the hold the open's first count takes
 * anyway — so two opens racing cannot both find room; an open that
 * fails after that gives the count back, and Storestat counts an open
 * in flight.  objsnapclose releases the count.  Giving the count back
 * releases a claim like any other, so an open in flight when
 * storeclose runs holds the store's last one and its own failure
 * path is what frees the Store.
 *
 * **A snapshot MAY outlive storeclose**, and it is the ONLY thing
 * that may: the Store* itself is invalid the moment storeclose
 * returns, so objsnapopen, dirtysnap, lostsnap, fullsyncsnap and
 * storestat on a closed store are undefined as before — and, since
 * none of them holds a claim while it waits on the state lock, they
 * must have stopped being issued before the close as well as after
 * it (storeclose, above).  What may be called on a handle taken
 * before the close is objsnapent, objsnapcount and objsnapclose,
 * and nothing else.  A snapshot
 * that outlives one keeps the Store's memory alive, so objsnapcount
 * still answers, objsnapent answers -1 `store closed' — refusing
 * rather than lying "gone" for every entry — and the memory goes at
 * the LAST objsnapclose.  objsnapopen on a store some other snapshot
 * is holding alive that way refuses `store closed' too — on each of
 * its count passes, so a close that lands while it re-counts is seen
 * — but that is a courtesy inside an undefined call and not a
 * guarantee the pointer can keep.  Closing one twice is UNDEFINED, exactly as
 * freeing the same pointer twice is: the second call reads a handle
 * the first freed, whose first word the pool has already overwritten,
 * so there is nothing it can check and no guard that would help.
 */
enum
{
	Snaplive	= 1<<0,		/* /obj */
	Snaptomb	= 1<<1,		/* /tombs */
	Snapboth	= Snaplive|Snaptomb,	/* /advert */
};

Objsnap*	objsnapopen(Store*, int kinds);
ulong		objsnapcount(Objsnap*);
int		objsnapent(Objsnap*, ulong i, uchar *oid, int *oidlen,
			Objinfo*);
void		objsnapclose(Objsnap*);

/*
 * The other two enumerations layer-a §2.2 makes a snapshot MUST, as
 * copies taken at open rather than as cursors: /dirty and /lost are
 * bounded by the dirty region and by what fails local verification,
 * so a copy is the whole of what a renderer needs.
 *
 * dirtysnap answers a malloc'd array of every record in the dirty set
 * (layer-a §7.1), taken under one hold of the lock that guards it;
 * the Dirtyrec carries the record's oid, oidlen, peer, peerlen and
 * epoch, and its op is 1 (add) because a record that is in the set is
 * one that was added.  lostsnap answers the same for /lost (layer-a
 * §7.5): one entry per slot storelost would name, with the oid and
 * the Objinfo beside it so a renderer need not go back to the index.
 * storelost stays: it is what a walker that wants the live list uses.
 *
 * The /lost copy names every one of those slots, §5 step 10's
 * included — an index entry that would not unpack is itself the
 * damage, so that entry has no oid to give: its oidlen is 0 and its
 * Objinfo is the slot number, state Sfree and zeroes, and a renderer
 * emits the line with no `oid='.  Any other rule would make the copy
 * disagree with Storestat.nlost.
 *
 * fullsyncsnap answers the other half of /dirty: a malloc'd array of
 * the names of the peers carrying §7.1's coarse fullsync flag, which
 * no record in the dirty set names — the exhaustion drop sets it on
 * the peer whose records it has just dropped.  A renderer of /dirty
 * therefore takes two copies, one call each.  The names live in the
 * same allocation as the pointer array, so one free releases both.
 *
 * All three answer 0 with *np 0 and *p nil when there is nothing to
 * report, -1 on failure, and the array is the caller's to free.
 */
typedef struct Lostent Lostent;
struct Lostent
{
	int	oidlen;
	uchar	oid[Oidmax];
	Objinfo	oi;		/* oi.slot is the slot storelost names */
};

int	dirtysnap(Store*, Dirtyrec **dp, ulong *np);
int	lostsnap(Store*, Lostent **lp, ulong *np);
int	fullsyncsnap(Store*, char ***pp, ulong *np);

/*
 * §8's block repair.  a is block blk as fetched from a holder of a
 * copy at the same key (layer-a §5.6's op=get), n its covered length.
 * The bytes are accepted only against the *stored* dig[i], and only
 * when hash(dig[]) == csum: an object whose digest array fails is
 * §8's whole-object op=full case, and asking for a block repair there
 * is a caller bug, so that refusal carries no §2.6 prefix while the
 * bytes' own failure is `checksum mismatch'.  On acceptance one Eobj
 * publishes the block with the four-tuple unchanged.  The corrupt
 * flag is not cleared — objscrub clears it when every block matches.
 */
int	objrepair(Store*, uchar *oid, int oidlen, ulong blk, void *a, long n);

/*
 * The engine calls layer-a §5.5/§5.6's peer channels need and the
 * store did not have (tombstone adoption, drop, the resulting-csum
 * check, oid-ordered listing): declarations are added here by the
 * peer-engine-ops work and nowhere else in this header.
 */
/* --- peer-engine-ops: begin --- */
/* --- peer-engine-ops: end --- */

/* the dirty set, §2.6 and layer-a §7.1 */
int	dirtyadd(Store*, uchar *oid, int oidlen, char *peer, uvlong epoch);
int	dirtydel(Store*, uchar *oid, int oidlen, char *peer);
int	dirtyhas(Store*, uchar *oid, int oidlen, char *peer);
ulong	dirtycount(Store*);

/* multi-request op=full stages, §3.6 */
Stage*	stageopen(Store*, uchar *oid, int oidlen, uvlong len, int force);
int	stagewrite(Stage*, void *a, long n, uvlong off);
int	stagefinal(Stage*, uvlong ver, uvlong wepoch, Dirtyrec *dr, int ndr);
			/* consumes the stage, whether it succeeds or not */
void	stagediscard(Stage*);
/*
 * The idle sweep (§3.6).  It releases an expired stage's reservations
 * but never frees the handle, which is the fid's: a later chunk or
 * final=1 on one is refused `stage expired', and the fid's own clunk
 * still calls stagediscard, which then finds nothing left to release.
 */
void	stagesweep(Store*, vlong now);

/*
 * The monitor's map slot store, docs/design/store.md §10.
 *
 * A raw partition of a few MiB through which the monitor makes a
 * published cluster map durable before it acknowledges the publish
 * (layer-a §8.2).  The map text is opaque to it: this store keeps
 * bytes and a length and never parses, compares or orders them —
 * including their epochs, which layer-a §8.3's forceepoch and §8.6's
 * rebuild path may legitimately republish out of order.
 *
 *	sector 0	header, copy 0
 *	hdr.curoff	current-map slot 0
 *			current-map slot 1
 *	hdr.histoff	retain history slots, a ring
 *	last sector	header, copy 1
 *
 * Error strings: only an oversize map answers a layer-a §2.6 wire
 * error, `disk full', which §10 requires of it.  Every other refusal
 * here is local to the monitor and carries no §2.6 prefix (§3.7).
 */
enum
{
	Monvers		= 1,		/* format version of every header */
	Monslotszdflt	= 65536,	/* §10's default slotsz */
	Monretaindflt	= 8,		/* §10's default ring length */
	Monretainmin	= 2,		/* layer-a §5.2 clause 2 reads E−1 */
	Monminbytes	= 1024*1024,	/* §10: shoalmonfmt refuses less */
};

typedef struct Mon Mon;
typedef struct Monhdr Monhdr;
typedef struct Monhsel Monhsel;
typedef struct Monfmtcfg Monfmtcfg;
typedef struct Monmap Monmap;
typedef struct Monstat Monstat;

/*
 * Mon is opaque outside lib/, like Store: 2c(1) signs a function
 * taking a pointer to it from the type's definition, so a file that
 * has the definition and one that has not would disagree.
 */
#pragma incomplete Mon

/* the header, written only at format */
struct Monhdr
{
	ulong	vers;
	ulong	slotsz;
	ulong	retain;
	uvlong	curoff;		/* sector of current-map slot 0 */
	uvlong	histoff;	/* sector of history slot 0 */
};

/*
 * Both header copies.  Nothing writes either after format, so they
 * are identical by construction: the rule is "take either valid copy,
 * refuse if neither", and two valid copies that DIFFER are a refusal
 * naming the field rather than a choice — the operator has mixed two
 * partitions' halves, or the media is lying.
 */
struct Monhsel
{
	Monhdr	h[2];
	int	valid[2];
	char	why[2][ERRMAX];	/* why a copy is invalid */
	int	use;		/* copy to take, -1 if neither */
};

int	monhdrsel(Dev*, Monhsel*);

/*
 * Format.  Every refusal shoalmonfmt makes it makes here, so that a
 * T1 program drives the tool's decisions and not its argument
 * parsing: a slotsz that is not a multiple of secsz or is under two
 * sectors, a retain under Monretainmin, a device under Monminbytes or
 * too small for 2 header sectors and 2+retain slots, and a valid
 * monitor header without ream.  warnsuper reports §12's warning — the
 * target already carries a valid object-store superblock, so the unit
 * is an instance's — which is a warning and not a refusal.
 *
 * A fresh store holds both current slots as VALID empty maps at len 0,
 * seq 0, epoch 0, and the header sector of every history slot zeroed.
 * "The store holds no map" is then the chosen slot's len being 0, and
 * "neither current slot valid" always means damage.
 */
struct Monfmtcfg
{
	ulong	slotsz;		/* 0: Monslotszdflt; on return, what was used */
	ulong	retain;		/* 0: Monretaindflt; on return, what was used */
	int	ream;		/* format over a valid monitor header */
	int	warnsuper;	/* out: §12's object-store superblock warning */
	uvlong	curoff, histoff;/* out: sectors */
	uvlong	used;		/* out: bytes the format occupies */
};

int	monfmt(Dev*, Monfmtcfg*);

/*
 * The geometry and size half of those refusals, against a size that
 * need not be the device's own, filling in c's defaults.  It is what
 * lets `shoalmonfmt -z' refuse the size it was asked for before it
 * shortens the image to it (§12): a refused run leaves the file
 * byte-identical.  monfmt makes the same check of the device itself.
 */
int	monfmtcheck(Dev*, vlong size, Monfmtcfg*);

/*
 * One published map.  text is the store's own and is valid until the
 * next commit or monclose; a caller that wants it longer copies it.
 */
struct Monmap
{
	uchar	*text;
	ulong	len;
	uvlong	seq;
	uvlong	epoch;
};

struct Monstat
{
	ulong	slotsz, retain;
	uvlong	curoff, histoff;
	ulong	nhist;		/* valid history entries, phantoms apart */
	ulong	nphantom;	/* ring slots ignored at open (§10) */
	uvlong	seq, epoch;
	ulong	len;		/* of the current map */
	int	hasmap;		/* 0 on a fresh store: len is 0 */
	int	cur;		/* current-map slot in use */
	int	hdr;		/* header copy the open took */
	int	hdrother;	/* the other copy was valid too */
};

/*
 * Open reads both header copies, both current slots and the whole
 * ring, chooses the current map by §2.2's rule keyed on seq, and
 * marks every history slot whose seq exceeds the chosen current
 * slot's as a phantom — a publish that wrote its ring entry and never
 * published its map.  A phantom is ignored by every accessor and is
 * the first slot the next commit reuses.  Open WRITES NOTHING and
 * works on a Drdonly device.  It refuses a store with no valid header
 * and one with neither current slot valid.
 *
 * moncommit is §10's two steps with one flush each; it answers
 * `disk full' and leaves the store unchanged when secsz+len exceeds
 * slotsz, and a failed ring write fails the commit with the current
 * map untouched.  Each slot is READ BACK after its flush and checked,
 * so a write that reports success and does not land fails the commit
 * exactly as a failed one does (§10); a device that loses the bytes
 * after acknowledging the flush is outside the model.  A read-back
 * whose READ fails, twice, is a third outcome: the commit fails
 * saying the publish is INDETERMINATE, because the slot may be on the
 * platter.  Such a slot is not served by this process and its seq is
 * spent, so a retry outranks it; a monitor that fails a publish has
 * not acknowledged it, and the map may still be there at the next
 * open (§10).  moncurrent answers 0 for "this store holds no map".
 * monhistory walks the ring newest-first, position 0 being the
 * current map itself, and answers 0 past the end.  monlookup answers
 * the entry for an epoch, taking the greater seq when two carry one
 * epoch.
 */
Mon*	monopen(Dev*);
void	monclose(Mon*);
int	moncommit(Mon*, void *text, ulong len, uvlong epoch);
int	moncurrent(Mon*, Monmap*);
int	monhistory(Mon*, ulong i, Monmap*);
int	monlookup(Mon*, uvlong epoch, Monmap*);
void	monstat(Mon*, Monstat*);

/*
 * The cluster map, docs/design/layer-a.md §3; placement, §4; the
 * currency witness set, §5.2; the epoch/monid adoption decision,
 * §6.3; and the fence state, §6.4.
 *
 * Everything below is pure: no globals, no I/O, no clock of its own.
 * The map arrives as bytes from wherever the caller got them and
 * this code parses it and answers questions about it.  mapparse is
 * the only allocator; the Cmap it returns, and everything reachable
 * from it, is freed by mapfree and by nothing else.  Every other
 * function writes only into arrays the caller supplies.
 *
 * Error strings: a map text that fails validation answers layer-a
 * §2.6's `bad map' with a detail after a colon, and that is the only
 * §2.6 prefix anything here produces (store.md §3.7's mapping rule).
 * An instance's refusal to adopt a map is not a wire error at all —
 * §6.3 reports it in /status — so mapadoptable answers flag bits,
 * not a string.
 */
enum
{
	Nodelen		= 63,	/* §3.3 node-name bound */
	Idxdigits	= 10,	/* an instance index is a u32 in decimal */
	Iidlen		= Nodelen + 1 + Idxdigits,
	Uuidlen		= 32,	/* §3.3, hex characters */
	Monidlen	= 32,	/* §3.2, hex characters */
	Clnamelen	= 63,	/* the cluster name in map=<name> */
	Addrlen		= 127,	/* a 9P dial string */
	Classlen	= 31,	/* a device class tag */

	/*
	 * The largest R this build places for.  layer-a bounds
	 * `replicas' only below (§3.2, ≥ 1); this bound is
	 * implementation policy, chosen so every placement array is a
	 * fixed size, and is two orders above the 3–12 node envelope
	 * §4.1 designs for.  mapparse refuses a larger `replicas'.
	 */
	Maxplace	= 64,
};

/* §3.3 status=, in the order the enum is compared nowhere */
enum
{
	Snew	= 0,
	Sin,
	Sout,
	Sdead,
};

/* §3.3 up= */
enum
{
	Uyes	= 0,
	Uheal,
	Uno,
};

typedef struct Cinst Cinst;
typedef struct Cstale Cstale;
typedef struct Cmap Cmap;

/*
 * One `instance' record.  class is the empty string when the record
 * carried none; zone is `default' then (§3.3), weight 100, fenced 0
 * and since 0.  node points at iid's node part, so it is iid's
 * prefix by construction, which is what §3.3 requires onnode= to be.
 */
struct Cinst
{
	char	iid[Iidlen+1];
	char	node[Nodelen+1];
	ulong	idx;			/* the iid's index part */
	char	addr[Addrlen+1];
	char	uuid[Uuidlen+1];
	char	class[Classlen+1];
	char	zone[Nodelen+1];	/* §4.5: parsed, never placed on */
	ulong	weight;			/* §4.4: parsed, must be 100 */
	int	status;			/* Snew … Sdead */
	int	up;			/* Uyes … Uno */
	int	fenced;			/* §3.3: parsed; §5.2's grace reads it */
	uvlong	since;
};

/* one `stale' record: the ledger of §7.1, travelling in the map */
struct Cstale
{
	char	subject[Iidlen+1];
	char	reporter[Iidlen+1];
	uvlong	since;
};

/*
 * A parsed map.  node[] is the `node' records, which §3.1 makes
 * optional and informational; placement uses pnode[], the node set
 * derived from the onnode= of status=in instances, which is what
 * §4.3 step 1 defines V to be.
 */
struct Cmap
{
	char	name[Clnamelen+1];
	uvlong	epoch;
	char	monid[Monidlen+1];
	uvlong	objmax;
	ulong	blksz;
	ulong	replicas;
	char	csumalg[32], placehash[32];
	ulong	pollms, leasems, replms, deadms;
	ulong	outmins, tombdays, mincopies, retain;
	char	placerule[32];

	Cinst	*inst;
	int	ninst;
	Cstale	*stale;
	int	nstale;
	char	(*node)[Nodelen+1];
	int	nnode;
	char	(*pnode)[Nodelen+1];	/* V, §4.3 step 1 */
	int	npnode;
};

/*
 * mapparse validates text[0:n] in full and answers nil with
 * `bad map: <why>' in the error string if it does not conform.  The
 * text need not be NUL-terminated and is not retained.
 *
 * nil is not always `bad map': an allocation failure answers nil
 * with whatever mallocz left in the error string, or with
 * `out of memory' for a size a ulong cannot hold.  A caller that
 * puts the errstr of a nil on the wire must therefore decide what a
 * non-`bad map' one means to it; store.md §3.7 forbids inventing a
 * §2.6 prefix for it.
 *
 * mapnextok is §8.1's commit-time half of the same validation, which
 * needs two maps: next's epoch MUST be exactly cur's plus one and
 * §8.5's immutable attributes MUST be unchanged.  force is the
 * `forceepoch' exemption (§8.6): it lifts the monid check and
 * replaces the epoch relation with cur's epoch < next's, which is
 * §8.1's "arbitrary higher value", §6.1's strictly increasing epoch
 * and §8.6.2's "MUST NOT publish an epoch it cannot prove is the
 * highest".  A regression under force is refused here and nowhere
 * else.  mapnextok answers 0 with `bad map: …' when next may not be
 * committed over cur.
 */
Cmap*	mapparse(char *text, long n);
void	mapfree(Cmap*);
int	mapnextok(Cmap *cur, Cmap *next, int force);

Cinst*	mapinst(Cmap*, char *iid);
char*	statusname(int status);
char*	upname(int up);

/*
 * Placement, §4.  maphash is §4.2's H over the score input for one
 * round: dom is 'N' for the node round and 'D' for the instance
 * round, and the bytes hashed are oid || 0x00 || dom || id.
 *
 * mapplace fills out[] with P(oid) in placement order and answers
 * |P|, which is min(replicas, |V|) and MAY be less than replicas —
 * §4.3 step 4's structural under-replication, which a caller reports
 * and still serves.  It answers the whole |P| even when out[] is
 * shorter, filling the first nout entries, so a caller may size
 * out[] by what it can use.  Above 32 nodes it allocates the score
 * array; that failing, it answers −1 with `out of memory' in the
 * error string rather than 0, which would be a statement about the
 * map.  mapprimary then answers nil with that errstr still set, and
 * mapunderrep and mapwitness answer −1: an allocation failure is
 * never a placement answer.
 *
 * placecmp is the order both HRW rounds sort by — descending score,
 * ties to the byte-wise greater id, the longer id winning when one is
 * the other's prefix — answering <0, 0 or >0 as the left candidate
 * ranks below, with or above the right.  It is the tie-break no
 * known-answer vector can exercise, since a tie needs a 64-bit
 * collision.
 *
 * mapprimary is §4.3's serving primary: the first member of P(oid)
 * with up=yes, or nil when there is none, which is the object's
 * `object unavailable' at this epoch — or nil with an error string,
 * which is not.  mapunderrep answers 1, 0, or −1 for the same
 * failure.  Being the serving primary is necessary and not
 * sufficient to serve: §5.2's grace and currency check are the rest,
 * and neither is computed here.  The grace is the caller's:
 * mapprimary(prev, oid) says whether it was already serving primary
 * for the object at E−1, and §5.2's exemption is that instance being
 * up=no with fenced set in the map at E — `fenced=' is parsed here
 * (Cinst.fenced) and consumed there.
 */
uvlong	maphash(char *oid, int dom, char *id);
int	placecmp(uvlong sa, char *a, uvlong sb, char *b);
int	mapplace(Cmap*, char *oid, Cinst **out, int nout);
Cinst*	mapprimary(Cmap*, char *oid);
int	mapunderrep(Cmap*, char *oid);

/*
 * §6.4 F3, and the membership rule its carve-out does not cover.
 *
 * mapdown answers whether the map bars this instance from serving
 * role=client I/O — up=no, status=out, status=dead, or no record at
 * all — which a caller refuses with `down'.  Keeping F3 to that is
 * the caller's job, not this answer's: F3 MUST NOT stop the instance
 * answering op=meta, op=get, op=list or op=verify, accepting an
 * incoming op=full or op=delete push, or completing a pull or push
 * it is the source of, and mis-wording exactly that is what made
 * `disable' self-defeating in the previous revision.  mapmember is
 * the zombie rule the carve-out does not cover: an instance with no
 * record, or status=dead, must not advertise, push, answer /rpc or
 * attach role=repl anywhere.
 */
int	mapdown(Cmap*, char *iid);
int	mapmember(Cmap*, char *iid);

/*
 * The currency witness set, §5.2.  A witness is an instance that is
 * not status=dead and satisfies one of the four clauses, recorded in
 * `why'; `how' is what the check must do with it:
 *
 *	Wquery	up is yes or heal: an op=meta response is required
 *	Wskip	up=no, and the reporter of no in-scope mark: skipped
 *	Wblock	up=no and an in-scope reporter: the check cannot
 *		complete, and the instance answers `not ready'
 */
enum
{
	Wquery	= 0,
	Wskip,
	Wblock,
};

enum
{
	Wplace		= 1<<0,	/* clause 1: in P(o) at E */
	Wprev		= 1<<1,	/* clause 2: in P(o) at E−1 */
	Wstray		= 1<<2,	/* clause 3: a known stray holder */
	Wreporter	= 1<<3,	/* clause 4: an in-scope mark's reporter */
};

typedef struct Cwit Cwit;
struct Cwit
{
	Cinst	*inst;
	int	how;
	int	why;	/* the clauses that put it here */
};

/*
 * m is the map at E and prev the map the instance last adopted, or
 * nil.  Clause 2 reads prev only when it is the map at E−1
 * specifically; when it is not, or when subst is set because this
 * instance's reconcile pass for the last placement change has not
 * completed, §5.2 substitutes every instance with status in
 * {new,in,out} — for that clause and for no other.
 *
 * Clause 4 and the skip rule are scoped on the mark's subject being
 * in P(o) at E or at E−1, and take no substitute: with no E−1 map
 * here (prev nil, or prev at some other epoch) the E−1 half of that
 * scope is NOT evaluated, whatever subst says.  §5.2 offers the
 * caller the other way out — fetch /maps/<E−1> from the monitor,
 * which §8.2 requires it to keep, and pass it as prev — and that
 * fetch is the caller's obligation, not this code's.  Substituting
 * for clause 4 instead would scope it on "every instance that is not
 * dead", which is the unscoped reading §5.2 rules out: one down
 * reporter of one unresolved mark would then block every currency
 * check in the cluster.
 *
 * stray[] is the locally known stray holders of clause 3, as iids;
 * one that names no instance of m is ignored.  mapwitness answers
 * |W|, or −1 with an error string when placement could not be
 * computed, and fills the first nout entries, so an out[] of
 * m->ninst entries always holds the whole set.
 *
 * witblocker answers the first Wblock witness, which is the one
 * whose name belongs in the `not ready' this check produces, or nil
 * when the check may complete once its Wquery responses are in.  The
 * n it is passed MUST be min(|W|, nout) — mapwitness answers the
 * whole |W| but fills and classifies only the first nout entries, so
 * handing it mapwitness's answer unclamped walks off a shorter
 * out[].  An out[] of m->ninst entries needs no clamp, since |W| can
 * never exceed it.
 */
typedef struct Witreq Witreq;
struct Witreq
{
	Cmap	*m;
	Cmap	*prev;
	char	*oid;
	char	**stray;
	int	nstray;
	int	subst;
};

int	mapwitness(Witreq*, Cwit *out, int nout);
Cinst*	witblocker(Cwit*, int n);

/*
 * §6.3's adoption decision.  An Adopt is the instance's own durable
 * pair (highest adopted epoch, pinned monid) — store.md §2.2 says
 * where it lives — plus whether it has ever adopted a map.
 * mapadoptable decides and changes nothing; mapadopted records an
 * adoption that went ahead.
 *
 * §6.3 makes two independent MUSTs, one per flag, so the codes below
 * are BITS and mapadoptable answers the OR of every refusal that
 * holds: the map that trips both — another authority's, at an epoch
 * below ours — must report both flags.  adoptwhy names the /status
 * flag of ONE bit, and answers nil for anything that is not a single
 * known bit; a caller renders each bit it finds set, rendering
 * /status itself being the server's job and not this library's.
 */
enum
{
	Mapok		= 0,
	Mapregress	= 1<<0,	/* epoch below the one held: epochregress=yes */
	Mapmonid	= 1<<1,	/* monid differs from the pin: monidmismatch=yes */
};

typedef struct Adopt Adopt;
struct Adopt
{
	int	pinned;			/* has adopted a map before */
	char	monid[Monidlen+1];
	uvlong	epoch;
};

int	mapadoptable(Adopt*, Cmap*);	/* Mapok, or the OR of the refusals */
void	mapadopted(Adopt*, Cmap*);
char*	adoptwhy(int bit);		/* one bit's /status flag, or nil */

/*
 * §6.4's fence state.  Times are milliseconds on the caller's own
 * monotonic clock, which is all §6.4 assumes of a clock — elapsed
 * time, not synchronisation.  `last' is the time of the last
 * SUCCESSFUL refresh, which §6.3 makes a narrower thing than a read
 * that returned bytes: a map refused for epoch regression or a monid
 * mismatch is not one.  maprefresh is that rule in one call — it
 * adopts and clears the lease fence together, or does neither.
 *
 * A `now' below `last' is a clock that has gone backwards, which is
 * §6.4's assumption broken rather than an interval: it counts as the
 * lease having elapsed, so the instance is fenced until a refresh
 * succeeds.
 *
 * F4's operator fence is a separate flag with the same effect, and
 * clearing it MUST NOT clear a lease-derived fence; since fencekind
 * derives both from state rather than latching a bit, it cannot.
 */
enum
{
	Fencenone	= 0,
	Fencelease	= 1<<0,
	Fenceoper	= 1<<1,
	Fenceboth	= Fencelease|Fenceoper,
};

typedef struct Fence Fence;
struct Fence
{
	int	refreshed;	/* a refresh has ever succeeded */
	vlong	last;		/* ms, the last successful refresh */
	ulong	leasems;
	int	oper;		/* F4 */
};

int	fencekind(Fence*, vlong now);
void	fencerefresh(Fence*, vlong now);
void	fenceoperator(Fence*, int on);
char*	fencename(int kind);
int	maprefresh(Adopt*, Fence*, Cmap*, vlong now);
