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
	Blkszdflt	= 65536,		/* map default blksz */
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
