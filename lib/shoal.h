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
 * prints and what /status reports (§3.2, §12).  The three states are
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
	void	*aux;
};

/*
 * Loop-until-complete wrappers (store.md §0).  devsd truncates a
 * request rather than splitting or failing, so a short count is
 * normal; these loop, and fail only on a real error or on no
 * progress.  They return 0 or -1 with the error string set; deverr
 * classifies that string.
 */
int	devread(Dev*, void*, long, vlong);
int	devwrite(Dev*, void*, long, vlong);
int	devflush(Dev*);
int	devzero(Dev*, vlong off, vlong n, ulong unit);
void	devclose(Dev*);
int	deverr(void);

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
 * next n operations (n <= 0 arms them until disarmed); several may be
 * armed at once, and simfaultat aims one at a byte range.  Arming
 * Sfnone disarms every one of them, and so does a crash.
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
void	simcrashmode(Dev*, int mode);
void	simcrashkeep(Dev*, vlong off, vlong len);
void	simarm(Dev*, char *point, int n);
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
	Blkszstore	= 16384,	/* §0: blksz = Wunit = the grain */
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
	int	out;		/* fd for the report */
};

int	ckstore(Dev *d, Ckcfg *c);
