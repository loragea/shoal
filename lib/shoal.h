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
 * docs/design/store.md.  Includers of it must also include <fcall.h>
 * before this file: every on-disk integer is packed and unpacked with
 * the GBIT/PBIT macros 9front exports from there (store.md §0), and
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
	int	canflush;	/* 0: flush is a no-op (§3.2's -w) */
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

Dev*	sdopen(char *part, int noflush);
Dev*	fileopen(char *path, ulong secsz, vlong size);
Dev*	simopen(ulong secsz, uvlong nsec, ulong seed);

/*
 * Simulated-disk controls, store.md §13.  The sim models a volatile
 * write cache: a written sector is visible to reads at once but is
 * durable only after a flush, and simcrash drops everything written
 * since the last one.  Faults are armed for the next n operations
 * (n <= 0 arms them until disarmed with Sfnone).
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
void	simcrash(Dev*);
void	simarm(Dev*, char *point, int n);
void	simpoke(Dev*, vlong off, void *buf, long n);	/* straight to durable storage */
void	simpeek(Dev*, vlong off, void *buf, long n);	/* straight from durable storage */
uvlong	simdirty(Dev*);					/* sectors written but not flushed */
long	simtrace(Dev*, Simop**);
void	simtracereset(Dev*);
