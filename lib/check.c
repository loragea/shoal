#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"
#include "store.h"		/* objrecok, dirtyrecok: replay's own checks */

/*
 * The inspect-and-check engine behind shoalck, docs/design/store.md
 * §12.  It reads and never writes: it prints both superblocks and
 * which one §2.2's three clauses select and why, the geometry and the
 * region table, then validates every index entry's checksum, every
 * extent map a live entry claims, every bitmap page and every dirty
 * record, cross-checks the bitmap against the grains the live maps
 * reference, and scans the log from the checkpoint mark.  It exits
 * non-zero on any inconsistency.
 *
 * Bulk reads go in 64 KiB requests: the driver's 32-sector split
 * happens inside one syscall, so that is ~24% faster per byte than
 * 16 KiB ones (docs/platform/9front-storage.md §6).  The Wunit rule
 * of §0 governs writes only.
 */

enum
{
	Rdunit	= 65536,	/* §0's bulk-read request size */
};

/*
 * One bit per grain, not one byte: the cross-check below wants two
 * arrays over ngrains, which is 2.7e8 at the envelope of §2.1's
 * worked example.
 */
static int
getbit(uchar *p, uvlong i)
{
	return (p[i/8] >> (i%8)) & 1;
}

static void
setbit(uchar *p, uvlong i)
{
	p[i/8] |= 1 << (i%8);
}

typedef struct Ck Ck;
struct Ck
{
	Dev	*d;
	Ckcfg	*c;
	Super	*s;
	int	bad;		/* problems found */
	uchar	*used;		/* bit per grain: referenced by a live map */
	uchar	*alloc;		/* bit per grain: set in the bitmap */
	uvlong	nlive, ntomb, nfree, nbadent, ncorrupt;
	uvlong	nemapused, nbademap;
	uvlong	ndirtyused, nbaddirty;
	uvlong	nbmbad;
	uvlong	nbmfree;	/* grains clear in the on-disk bitmap */
	uvlong	pmax;
	int	havepmax;
};

static void
problem(Ck *k, char *fmt, ...)
{
	char buf[512];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf + sizeof buf, fmt, arg);
	va_end(arg);
	k->bad++;
	fprint(k->c->out, "problem: %s\n", buf);
}

static void
say(Ck *k, char *fmt, ...)
{
	char buf[512];
	va_list arg;

	if(k->c->quiet)
		return;
	va_start(arg, fmt);
	vseprint(buf, buf + sizeof buf, fmt, arg);
	va_end(arg);
	fprint(k->c->out, "%s\n", buf);
}

static char*
hex(char *buf, uchar *p, int n)
{
	static char h[] = "0123456789abcdef";
	int i;

	for(i = 0; i < n; i++){
		buf[2*i] = h[p[i] >> 4];
		buf[2*i+1] = h[p[i] & 0xf];
	}
	buf[2*n] = '\0';
	return buf;
}

/* an oid is layer-a §1.2 text; anything else in the field is a fault */
static char*
oidstr(char *buf, uchar *oid, int n)
{
	int i;

	for(i = 0; i < n; i++)
		buf[i] = oid[i] >= 0x20 && oid[i] < 0x7f ? oid[i] : '?';
	buf[n] = '\0';
	return buf;
}

static void
region(Ck *k, char *nm, uvlong off, uvlong secs)
{
	say(k, "	%-8s %12llud %12llud %14llud", nm, off, secs,
		secs*(uvlong)k->s->secsz);
}

/* §5 step 3: regions inside the partition, no overlaps, sane marks */
static void
ckgeom(Ck *k)
{
	Super *s;
	uvlong nsec, o[7], n[7], i, pagesecs;
	char *nm[7];

	s = k->s;
	if(s->secsz == 0 || s->blksz == 0 || s->blksz % s->secsz != 0){
		/*
		 * Everything below divides by these two.  A checker is
		 * the one tool that is run against hostile bytes, so it
		 * says so and stops rather than trapping.
		 */
		problem(k, "secsz %lud and blksz %lud are not a geometry",
			s->secsz, s->blksz);
		return;
	}
	nsec = k->d->size / k->d->secsz;
	nm[0] = "log";		o[0] = s->logoff;	n[0] = s->logsecs;
	nm[1] = "index";	o[1] = s->idxoff;	n[1] = s->idxsecs;
	nm[2] = "emap";		o[2] = s->emapoff;	n[2] = s->emapsecs;
	nm[3] = "dirty";	o[3] = s->dirtoff;	n[3] = s->dirtsecs;
	nm[4] = "bitmap";	o[4] = s->bmapoff;	n[4] = s->bmapsecs;
	nm[5] = "data";		o[5] = s->dataoff;	n[5] = s->datasecs;
	nm[6] = nil;

	pagesecs = s->blksz / s->secsz;
	for(i = 0; nm[i] != nil; i++){
		if(o[i] < 1 || o[i] + n[i] > nsec - 1)
			problem(k, "%s region %llud+%llud outside the "
				"partition's %llud sectors", nm[i], o[i], n[i],
				nsec);
		if(i > 0 && o[i] < o[i-1] + n[i-1])
			problem(k, "%s region %llud overlaps %s %llud+%llud",
				nm[i], o[i], nm[i-1], o[i-1], n[i-1]);
		/* §2.1: every region start is a blksz boundary */
		if(o[i] % pagesecs != 0)
			problem(k, "%s region starts at sector %llud, which is "
				"not a %lud-byte boundary", nm[i], o[i],
				s->blksz);
	}
	/*
	 * §14(8) makes refusing a csumalg mismatch a MUST, and a digest
	 * is meaningless until the algorithm behind it is known.
	 */
	if(s->csumalg != Csumblake2s)
		problem(k, "csumalg %lud is not one this build implements",
			s->csumalg);
	if(s->logsecs*(uvlong)s->secsz >= (1ULL<<32))
		problem(k, "log region of %llud bytes exceeds the u32 a "
			"record length is computed in",
			s->logsecs*(uvlong)s->secsz);
	if(s->objmax % s->blksz != 0 || s->nblkmax != s->objmax/s->blksz)
		problem(k, "nblkmax %lud does not match objmax %llud / blksz %lud",
			s->nblkmax, s->objmax, s->blksz);
	/* §2.6: a store refuses to open over ndirty == 0, and so must this */
	if(s->ndirty == 0)
		problem(k, "the geometry has no dirty region");
	/*
	 * In uvlong: nblkmax is a u32 the superblock supplies, so
	 * 20*nblkmax reaches 2^36 and the ulong sum wraps.  A
	 * checksum-valid superblock naming 2^30 blocks with a
	 * 512-byte emapsz would otherwise pass this and send ckemap's
	 * walk 4 GiB past the buffer ckindex allocates for it.
	 */
	if((uvlong)s->emapsz < Emaphdrsz + 20*(uvlong)s->nblkmax)
		problem(k, "emapsz %lud is too small for %lud blocks",
			s->emapsz, s->nblkmax);
	if(s->ngrains >= (1ULL<<32))
		problem(k, "ngrains %llud reaches 2^32", s->ngrains);
	if(s->ngrains > s->datasecs/(s->blksz/s->secsz))
		problem(k, "ngrains %llud exceeds the data region",
			s->ngrains);
	if(s->cklogoff < s->logoff || s->cklogoff >= s->logoff + s->logsecs)
		problem(k, "cklogoff %llud outside the log region "
			"%llud+%llud", s->cklogoff, s->logoff, s->logsecs);
	if(nbmpage(k->s)*bmbits(s->blksz) < s->ngrains)
		problem(k, "%llud bitmap pages do not cover %llud grains",
			nbmpage(k->s), s->ngrains);
}

/* mark grain g referenced; slot is for the diagnostic */
static void
refgrain(Ck *k, ulong g, ulong slot, ulong blk)
{
	if(g == 0)
		return;			/* a hole */
	if(g >= k->s->ngrains){
		problem(k, "slot %lud block %lud names grain %lud, ngrains "
			"is %llud", slot, blk, g, k->s->ngrains);
		return;
	}
	if(getbit(k->used, g))
		problem(k, "grain %lud is referenced twice (slot %lud "
			"block %lud)", g, slot, blk);
	setbit(k->used, g);
}

static void
ckemap(Ck *k, Idxent *e, ulong slot, uchar *emap)
{
	Super *s;
	Emap m;
	uvlong nblk, i;
	ulong g;

	s = k->s;
	if(devread(k->d, emap, s->emapsz, emapentoff(s, e->emapslot)) < 0){
		problem(k, "slot %lud: extent map %lud: %r", slot, e->emapslot);
		k->nbademap++;
		return;
	}
	k->nemapused++;
	if(emapunpack(&m, emap, s->emapsz, s->nblkmax) < 0){
		problem(k, "slot %lud: extent map %lud: %r", slot, e->emapslot);
		k->nbademap++;
		return;
	}
	/*
	 * §2.4: nblk is blkcount(len) and nothing else - a reader that
	 * has both MUST recompute it rather than trust the header
	 * sector, which may be the torn one.
	 */
	nblk = blkcount(e->len, s->blksz);
	if(m.nblk != nblk)
		problem(k, "slot %lud: extent map %lud says nblk %lud, len "
			"%llud gives %llud", slot, e->emapslot, m.nblk, e->len,
			nblk);
	for(i = 0; i < nblk; i++)
		refgrain(k, emapgrain(emap, i), slot, i);
	/* §2.4: array entries beyond nblk MUST be zero */
	for(i = nblk; i < s->nblkmax; i++){
		g = emapgrain(emap, i);
		if(g != 0){
			problem(k, "slot %lud: extent map %lud block %llud is "
				"at or beyond nblk %llud and names grain %lud",
				slot, e->emapslot, i, nblk, g);
			break;
		}
	}
}

static void
ckindex(Ck *k)
{
	Super *s;
	uchar *buf, *emap;
	uvlong off, per, slot, i, nblk;
	long n;

	s = k->s;
	per = Rdunit / Idxentsz;
	if((buf = malloc(per*Idxentsz)) == nil)
		sysfatal("malloc: %r");
	if((emap = malloc(s->emapsz)) == nil)
		sysfatal("malloc: %r");
	for(slot = 0; slot < s->nslots; slot += per){
		n = per;
		if(slot + n > s->nslots)
			n = s->nslots - slot;
		off = idxentoff(s, slot);
		if(devread(k->d, buf, n*Idxentsz, off) < 0){
			problem(k, "index slots %llud..%llud: %r", slot,
				slot + n - 1);
			k->nbadent += n;
			continue;
		}
		for(i = 0; i < (uvlong)n; i++){
			Idxent e;

			if(idxunpack(&e, buf + i*Idxentsz, s->nemap) < 0){
				problem(k, "index slot %llud: %r", slot + i);
				k->nbadent++;
				continue;
			}
			if(e.state == Sfree){
				k->nfree++;
				continue;
			}
			if(e.state == Slive)
				k->nlive++;
			else
				k->ntomb++;
			if(e.flags & Icorrupt)
				k->ncorrupt++;
			if(e.len > s->objmax){
				problem(k, "index slot %llud: len %llud exceeds "
					"objmax %llud", slot + i, e.len,
					s->objmax);
				continue;
			}
			nblk = blkcount(e.len, s->blksz);
			if(nblk <= 1){
				if(e.emapslot != 0)
					problem(k, "index slot %llud: %llud "
						"blocks but emapslot %lud",
						slot + i, nblk, e.emapslot);
				refgrain(k, e.grain0, slot + i, 0);
			}else{
				if(e.emapslot == 0){
					problem(k, "index slot %llud: %llud "
						"blocks and no extent-map slot",
						slot + i, nblk);
					continue;
				}
				ckemap(k, &e, slot + i, emap);
			}
		}
	}
	free(emap);
	free(buf);
}

static void
ckbitmap(Ck *k)
{
	Super *s;
	Bmpage h;
	uchar *buf;
	uvlong npage, i, bpp, g, base;

	s = k->s;
	npage = nbmpage(s);
	bpp = bmbits(s->blksz);
	if((buf = malloc(s->blksz)) == nil)
		sysfatal("malloc: %r");
	for(i = 0; i < npage; i++){
		if(devread(k->d, buf, s->blksz,
			(vlong)s->bmapoff*s->secsz + (vlong)i*s->blksz) < 0){
			problem(k, "bitmap page %llud: %r", i);
			k->nbmbad++;
			continue;
		}
		if(bmunpack(&h, buf, s->blksz, i) < 0){
			problem(k, "bitmap page %llud: %r", i);
			k->nbmbad++;
			continue;
		}
		/*
		 * §2.5: Pmax is the greatest ckseq over the pages that
		 * pass their own checksum; a page that fails one has an
		 * arbitrary ckseq and contributes nothing.
		 */
		if(!k->havepmax || h.ckseq > k->pmax){
			k->pmax = h.ckseq;
			k->havepmax = 1;
		}
		base = i*bpp;
		for(g = 0; g < bpp && base + g < s->ngrains; g++)
			if(bmget(buf, g))
				setbit(k->alloc, base + g);
	}
	free(buf);
}

static void
ckdirty(Ck *k)
{
	Super *s;
	Dirtent e;
	uchar *buf;
	uvlong per, slot, i;
	long n;

	s = k->s;
	per = Rdunit / Dirtentsz;
	if((buf = malloc(per*Dirtentsz)) == nil)
		sysfatal("malloc: %r");
	for(slot = 0; slot < s->ndirty; slot += per){
		n = per;
		if(slot + n > s->ndirty)
			n = s->ndirty - slot;
		if(devread(k->d, buf, n*Dirtentsz, dirtentoff(s, slot)) < 0){
			problem(k, "dirty records %llud..%llud: %r", slot,
				slot + n - 1);
			k->nbaddirty += n;
			continue;
		}
		for(i = 0; i < (uvlong)n; i++){
			if(dirtunpack(&e, buf + i*Dirtentsz) < 0){
				problem(k, "dirty record %llud: %r", slot + i);
				k->nbaddirty++;
				continue;
			}
			if(e.state != 0)
				k->ndirtyused++;
		}
	}
	free(buf);
}

/*
 * Decode and judge one log entry, exactly as §5 step 7's apply would:
 * a checksummed, in-sequence record whose entries cannot be decoded
 * or whose fields the range checks refuse is one replay refuses to
 * start on — and the refusal's own message names shoalck as the way
 * out, so a checker that printed the entry verbatim and counted no
 * problem would leave the operator with a store that will not start
 * and a report that finds nothing wrong.  Run for every entry, quiet
 * or verbose; the dump lines are the verbose extra.
 */
static void
ckent(Ck *k, uvlong seq, ulong i, Lent *e)
{
	Objrec o;
	Dirtyrec dr;
	char buf[2*Csumlen + 1], ob[Oidmax + 1];
	ulong slot, j;

	switch(e->kind){
	case Kobj:
		if(objrecunpack(&o, e->body, e->len - Lenthdrsz) < 0){
			problem(k, "log seq %llud entry %lud: %r", seq, i);
			return;
		}
		if(objrecok(k->s, &o) < 0)
			problem(k, "log seq %llud entry %lud: %r", seq, i);
		if(k->c->verbose){
			say(k, "		Eobj slot=%lud emapslot=%lud oflags=%#ux "
				"oid=%s", o.slot, o.emapslot, o.oflags,
				oidstr(ob, o.oid, o.oidlen));
			say(k, "		     len=%llud ver=%llud wepoch=%llud "
				"state=%d csum=%s", o.len, o.ver, o.wepoch, o.state,
				hex(buf, o.csum, Csumlen));
			say(k, "		     nmap=%lud nfree=%lud", o.nmap, o.nfree);
			for(j = 0; j < o.nmap && k->c->verbose > 1; j++)
				say(k, "		     blk %lud grain %lud",
					o.map[j].blk, o.map[j].grain);
		}
		objrecfree(&o);
		break;
	case Kdirty:
		if(dirtyrecunpack(&dr, e->body, e->len - Lenthdrsz) < 0
		|| dirtyrecok(k->s, &dr) < 0){
			problem(k, "log seq %llud entry %lud: %r", seq, i);
			return;
		}
		if(k->c->verbose)
			say(k, "		Edirty op=%s epoch=%llud peer=%.*s oid=%s",
				dr.op ? "add" : "remove", dr.epoch, dr.peerlen,
				(char*)dr.peer, oidstr(ob, dr.oid, dr.oidlen));
		break;
	case Kslot:
		if(slotrecunpack(&slot, e->body, e->len - Lenthdrsz) < 0){
			problem(k, "log seq %llud entry %lud: %r", seq, i);
			return;
		}
		if(slot >= k->s->nslots)
			problem(k, "log seq %llud entry %lud: Eslot: slot %lud, "
				"nslots %lud", seq, i, slot, k->s->nslots);
		if(k->c->verbose)
			say(k, "		Eslot slot=%lud", slot);
		break;
	default:
		problem(k, "log seq %llud entry %lud: unknown kind %d", seq, i,
			e->kind);
	}
}

/*
 * Scan the log from the checkpoint mark, exactly as §5 step 7 replays
 * it but without applying anything: bounds-check nsec before using
 * it, verify the checksum over the range it names, check seq against
 * the expectation seeded at ckseq+1, judge every entry by the decode
 * and range checks the apply itself makes (ckent), and continue at
 * the region start when Fwrap is set or when +nsec reaches the
 * region end.
 */
static void
cklog(Ck *k)
{
	Super *s;
	Lrec r;
	Lent e;
	uchar *buf;
	uvlong rel, seq, first, nrec, lim, off;
	ulong left, n;
	uchar *p;

	s = k->s;
	lim = (uvlong)s->logsecs*s->secsz;
	if(lim > 1024*1024)
		lim = 1024*1024;
	if((buf = malloc(lim)) == nil)
		sysfatal("malloc: %r");
	rel = s->cklogoff - s->logoff;
	seq = s->ckseq + 1;
	first = seq;
	nrec = 0;
	while(nrec < s->logsecs){
		off = (uvlong)(s->logoff + rel)*s->secsz;
		if(devread(k->d, buf, s->secsz, off) < 0){
			problem(k, "log sector %llud: %r", rel);
			break;
		}
		if(lrecunpack(&r, buf) < 0)
			break;
		if(r.nsec < 1 || rel + r.nsec > s->logsecs)
			break;
		if((uvlong)r.nsec*s->secsz > lim){
			problem(k, "log record at sector %llud claims %lud "
				"sectors", rel, r.nsec);
			break;
		}
		if(r.nsec > 1 && devread(k->d, buf, r.nsec*s->secsz, off) < 0){
			problem(k, "log record at sector %llud: %r", rel);
			break;
		}
		if(lrecvalid(buf, s->secsz, &r, rel, s->logsecs, seq) < 0)
			break;
		nrec++;
		if(k->c->verbose)
			say(k, "	seq %llud at sector %llud: %lud sectors, "
				"%lud entries%s", r.seq, rel, r.nsec, r.nent,
				r.flags & Fwrap ? ", Fwrap" : "");
		p = buf + Lrechdrsz;
		left = r.nsec*s->secsz - Lrechdrsz;
		for(n = 0; n < r.nent; n++){
			if(lentunpack(&e, p, left) < 0){
				problem(k, "log seq %llud entry %lud: %r",
					r.seq, n);
				break;
			}
			ckent(k, r.seq, n, &e);
			p += e.len;
			left -= e.len;
		}
		seq++;
		if(r.flags & Fwrap)
			rel = 0;
		else{
			rel += r.nsec;
			if(rel >= s->logsecs)
				rel = 0;
		}
	}
	say(k, "log: %llud valid records from sector %llud, seq %llud..%llud, "
		"next seq %llud", nrec, s->cklogoff - s->logoff, first,
		seq > first ? seq - 1 : first, seq);
	free(buf);
}

static void
ckcross(Ck *k)
{
	uvlong g, nref, nmark, phantom, leaked, nfree;
	int u, a;

	nref = nmark = phantom = leaked = nfree = 0;
	for(g = 0; g < k->s->ngrains; g++){
		u = getbit(k->used, g);
		a = getbit(k->alloc, g);
		if(u)
			nref++;
		if(a)
			nmark++;
		else
			nfree++;
		if(u && !a){
			if(phantom++ == 0)
				problem(k, "grain %llud is referenced by a "
					"live map and clear in the bitmap", g);
		}else if(!u && a && g != 0){
			if(leaked++ == 0)
				problem(k, "grain %llud is set in the bitmap "
					"and referenced by nothing", g);
		}
	}
	/*
	 * §2.1: grain 0 is reserved to mean `no grain', and shoalfmt
	 * MUST mark it allocated.  A store whose bit 0 was cleared
	 * would check clean and then hand grain 0 out as a real grain,
	 * aliasing dataoff under every hole.
	 */
	if(k->s->ngrains > 0 && !getbit(k->alloc, 0))
		problem(k, "grain 0 is not marked allocated, and §2.1 "
			"reserves it");
	if(phantom > 1)
		problem(k, "%llud grains in all are referenced and unmarked",
			phantom);
	if(leaked > 1)
		problem(k, "%llud grains in all are marked and unreferenced",
			leaked);
	k->nbmfree = nfree;
	say(k, "grains: %llud total, %llud referenced, %llud marked "
		"allocated, %llud free (grain 0 is reserved)",
		k->s->ngrains, nref, nmark, nfree);
}

static void
dumpobj(Ck *k, char *oid)
{
	Super *s;
	Idxent e;
	Emap m;
	uchar *buf, *emap;
	char ob[Oidmax + 1], hb[2*Csumlen + 1];
	uvlong slot, nblk, i;
	int n;

	s = k->s;
	n = strlen(oid);
	if((buf = malloc(Idxentsz)) == nil)
		sysfatal("malloc: %r");
	if((emap = malloc(s->emapsz)) == nil)
		sysfatal("malloc: %r");
	for(slot = 0; slot < s->nslots; slot++){
		if(devread(k->d, buf, Idxentsz, idxentoff(s, slot)) < 0)
			break;
		if(idxunpack(&e, buf, s->nemap) < 0 || e.state == Sfree)
			continue;
		if(e.oidlen != n || memcmp(e.oid, oid, n) != 0)
			continue;
		say(k, "slot %llud: oid=%s state=%d flags=%#ux", slot,
			oidstr(ob, e.oid, e.oidlen), e.state, e.flags);
		say(k, "	qidpath=%llud len=%llud ver=%llud wepoch=%llud "
			"mtime=%lld", e.qidpath, e.len, e.ver, e.wepoch,
			e.mtime);
		say(k, "	csum=%s", hex(hb, e.csum, Csumlen));
		nblk = blkcount(e.len, s->blksz);
		say(k, "	nblk=%llud emapslot=%lud", nblk, e.emapslot);
		if(nblk <= 1){
			say(k, "	block 0: grain %lud dig %s", e.grain0,
				hex(hb, e.dig0, Blkdlen));
		}else if(devread(k->d, emap, s->emapsz,
			emapentoff(s, e.emapslot)) == 0
		&& emapunpack(&m, emap, s->emapsz, s->nblkmax) == 0){
			for(i = 0; i < nblk; i++)
				say(k, "	block %llud: grain %lud dig %s", i,
					emapgrain(emap, i),
					hex(hb, emapdig(emap, s->nblkmax, i),
						Blkdlen));
		}else
			problem(k, "slot %llud: extent map %lud: %r", slot,
				e.emapslot);
		free(emap);
		free(buf);
		return;
	}
	problem(k, "no live object with oid %s", oid);
	free(emap);
	free(buf);
}

/*
 * -v and -R work on the REPLAYED state, and nothing above works on
 * anything but the checkpoint.  The difference is not a refinement:
 * §2.8 makes the log the durable authority for everything since
 * ckseq, and §3.5 defers a released grain's reuse only until the
 * freeing commit's flush has returned — so a grain freed by a
 * committed-but-not-checkpointed record may already hold another
 * object's bytes.  Verifying an object against the checkpointed index
 * would read those bytes and report a mismatch on an object that is
 * perfectly well, and a bitmap rebuilt from the checkpointed index
 * would clear grains the log has since handed out.
 *
 * storeopen(spawn=nil, nockptproc=1) is what replays the log into
 * memory: with no spawn callback the engine makes no proc, commits
 * are synchronous in the caller, and the start writes nothing —
 * §5 step 11's rebuild only marks pages dirty, and replay leaves the
 * maps it applied dirty in the cache for the checkpoint rather than
 * writing them.  storecheckpoint is therefore the only write either
 * flag makes, and only -R makes it.
 */
static void
ckverify(Ck *k, Store *st)
{
	Vfy v;
	Ient *e;
	char ob[Oidmax + 1], blks[128], *p, *ep;
	uvlong nver, ntomb, nbadobj, nclean;
	ulong slot, j;

	nver = ntomb = nbadobj = nclean = 0;
	for(slot = 0; slot < st->sb.nslots; slot++){
		e = &st->idx[slot];
		if(e->state == Sfree)
			continue;
		if(e->state == Stomb){
			/*
			 * A tombstone holds no content: layer-a §1.5 keeps
			 * the key and nothing else, so it verifies
			 * vacuously and is counted rather than read.
			 */
			ntomb++;
			continue;
		}
		nver++;
		if(objverify(st, e->oid, e->oidlen, &v) < 0){
			problem(k, "slot %lud oid %s: verify: %r", slot,
				oidstr(ob, e->oid, e->oidlen));
			nbadobj++;
			vfyfree(&v);	/* always safe, whatever it returned */
			continue;
		}
		if(v.nbad > 0 || v.arraybad){
			p = blks;
			ep = blks + sizeof blks;
			*p = '\0';
			for(j = 0; j < v.nbad && j < 8; j++)
				p = seprint(p, ep, "%s%lud", j > 0 ? "," : "",
					v.bad[j]);
			if(v.nbad > 8)
				seprint(p, ep, ",...");
			problem(k, "slot %lud oid %s: %lud of %llud blocks "
				"mismatch (blocks %s), arraybad=%d, "
				"corrupt-flagged=%d", slot,
				oidstr(ob, e->oid, e->oidlen), v.nbad,
				blkcount(e->len, st->sb.blksz), blks,
				v.arraybad, (e->flags & Icorrupt) != 0);
			nbadobj++;
		}else if(e->flags & Icorrupt){
			/*
			 * Information and not a problem.  The flag is
			 * durable and §8's online scrub is what clears it,
			 * with a key-preserving Eobj this tool does not
			 * write; an operator wants to know the copy is out
			 * of arbitration and that its content is sound.
			 */
			nclean++;
			say(k, "slot %lud oid %s: flagged corrupt and verifies "
				"clean; §8's scrub is what clears the flag",
				slot, oidstr(ob, e->oid, e->oidlen));
		}
		vfyfree(&v);
	}
	say(k, "verify: %llud objects verified, %llud tombstones skipped, "
		"%llud failed, %llud flagged corrupt but clean", nver, ntomb,
		nbadobj, nclean);
}

static void
ckengine(Ck *k, Dev *d)
{
	Storecfg cfg;
	Storestat ss;
	Store *st;

	if(k->c->rebuild && d->rdonly){
		problem(k, "-R rebuilds the bitmap and rewrites the "
			"checkpoint, and %s is open read-only", d->name);
		return;
	}
	memset(&cfg, 0, sizeof cfg);
	cfg.spawn = nil;		/* no procs: a pure in-memory replay */
	cfg.nockptproc = 1;
	cfg.noflush = k->c->noflush;
	cfg.forcerebuild = k->c->rebuild;
	if((st = storeopen(d, &cfg)) == nil){
		/*
		 * The engine's own refusal is the report: every one of
		 * §5's names the structure, the offset and the tool, and
		 * restating it here would be a second, worse diagnosis.
		 */
		problem(k, "the store does not open: %r");
		return;
	}
	storestat(st, &ss);
	if(ss.nlost > 0)
		say(k, "replay: %llud slot(s) condemned by §5 step 10 and not "
			"served", ss.nlost);
	if(k->c->rebuild){
		say(k, "rebuild: bmaprebuild=%s", ss.bmaprebuild ? "yes" : "no");
		/*
		 * The `as found' count comes from the checker's own bitmap
		 * pass, so it means what it says only when every page of
		 * that bitmap was read and passed its checksum.  A store
		 * with an unreadable page is exactly a store -R serves, and
		 * printing a number there would tell the operator that the
		 * rebuild changed a count it never had.
		 */
		if(k->nbmbad == 0)
			say(k, "rebuild: the on-disk bitmap leaves %llud "
				"grains free, the rebuild leaves %llud",
				k->nbmfree, ss.grainfree);
		else
			say(k, "rebuild: %llud bitmap page(s) did not read, so "
				"what the on-disk bitmap left free is not "
				"known; the rebuild leaves %llud grains free",
				k->nbmbad, ss.grainfree);
		if(storecheckpoint(st) < 0)
			problem(k, "rewriting the checkpoint: %r");
		else{
			storestat(st, &ss);
			say(k, "rebuild: checkpoint rewritten at ckseq %llud, "
				"cklogoff %llud", ss.ckseq, ss.cklogoff);
		}
	}
	if(k->c->verify)
		ckverify(k, st);
	storeclose(st);
}

int
ckstore(Dev *d, Ckcfg *c)
{
	Ck k;
	Sbsel sel;
	Super *s;
	char hb[33];
	uvlong nbit;
	int i;

	memset(&k, 0, sizeof k);
	k.d = d;
	k.c = c;
	say(&k, "device: %s, %lud-byte sectors, %lld bytes, flush=%s",
		d->name, d->secsz, d->size, flushname(d->flushmode));

	if(superselect(d, &sel) < 0 && sel.clause != 3){
		/*
		 * Not clause 3: superselect could not get far enough to
		 * apply the rule at all, and saying "neither copy is
		 * valid" would be a different diagnosis from the truth.
		 */
		problem(&k, "reading the superblocks: %r");
		return k.bad;
	}
	for(i = 0; i < 2; i++){
		if(sel.valid[i])
			say(&k, "superblock %d: valid, gen %llud, ckseq %llud, "
				"cklogoff %llud", i, sel.sb[i].gen,
				sel.sb[i].ckseq, sel.sb[i].cklogoff);
		else
			say(&k, "superblock %d: INVALID (%s)", i, sel.why[i]);
	}
	switch(sel.clause){
	case 1:
		say(&k, "selected: copy %d (gen %llud) - clause 1, exactly one "
			"copy is valid; the next update writes copy %d",
			sel.start, sel.sb[sel.start].gen, sel.victim);
		break;
	case 2:
		say(&k, "selected: copy %d (gen %llud) - clause 2, both copies "
			"are valid, so the next update writes copy %d, the "
			"lower gen", sel.start, sel.sb[sel.start].gen,
			sel.victim);
		break;
	default:
		problem(&k, "clause 3, neither copy is valid: the store MUST "
			"NOT write and MUST NOT serve");
		return k.bad;
	}
	say(&k, "next gen would be %llud", sel.nextgen);

	s = &sel.sb[sel.start];
	k.s = s;
	say(&k, "uuid=%s csumalg=%s vers=%lud ctime=%lld",
		hex(hb, s->uuid, 16), csumalgname(s->csumalg), s->vers,
		s->ctime);
	say(&k, "blksz=%lud objmax=%llud nblkmax=%lud emapsz=%lud",
		s->blksz, s->objmax, s->nblkmax, s->emapsz);
	say(&k, "nslots=%lud nemap=%lud ndirty=%lud ngrains=%llud",
		s->nslots, s->nemap, s->ndirty, s->ngrains);
	say(&k, "qidnext=%llud epochhigh=%llud monidset=%lud", s->qidnext,
		s->epochhigh, s->monidset);
	say(&k, "	%-8s %12s %12s %14s", "region", "sector", "sectors",
		"bytes");
	region(&k, "log", s->logoff, s->logsecs);
	region(&k, "index", s->idxoff, s->idxsecs);
	region(&k, "emap", s->emapoff, s->emapsecs);
	region(&k, "dirty", s->dirtoff, s->dirtsecs);
	region(&k, "bitmap", s->bmapoff, s->bmapsecs);
	region(&k, "data", s->dataoff, s->datasecs);

	ckgeom(&k);
	if(k.bad > 0)
		return k.bad;

	if(c->oid != nil){
		/*
		 * -o dumps one object's entry and map; -v and -R are
		 * whole-store passes over the replayed state, and a
		 * rebuild driven from one object's map would clear every
		 * grain the rest of the store holds.
		 */
		if(c->rebuild || c->verify)
			problem(&k, "-o dumps one object; -v and -R work over "
				"the whole store");
		else
			dumpobj(&k, c->oid);
		return k.bad;
	}

	nbit = (s->ngrains + 7)/8;
	if((k.used = mallocz(nbit, 1)) == nil)
		sysfatal("malloc %llud: %r", nbit);
	if((k.alloc = mallocz(nbit, 1)) == nil)
		sysfatal("malloc %llud: %r", nbit);
	ckindex(&k);
	ckbitmap(&k);
	ckdirty(&k);
	cklog(&k);
	ckcross(&k);

	say(&k, "index: %llud live, %llud tomb, %llud free, %llud bad, %llud "
		"corrupt-flagged", k.nlive, k.ntomb, k.nfree, k.nbadent,
		k.ncorrupt);
	say(&k, "extent maps: %llud claimed, %llud bad; %lud slots, slot 0 "
		"reserved", k.nemapused, k.nbademap, s->nemap);
	say(&k, "dirty records: %llud used, %llud bad of %lud", k.ndirtyused,
		k.nbaddirty, s->ndirty);
	say(&k, "bitmap: %llud pages, %llud bad, Pmax %llud", nbmpage(s),
		k.nbmbad, k.pmax);
	if(k.havepmax && k.pmax > s->ckseq)
		say(&k, "bitmap pages are stamped ahead of the superblock "
			"(Pmax %llud > ckseq %llud): the ordinary "
			"mid-checkpoint crash", k.pmax, s->ckseq);

	if(c->verify || c->rebuild)
		ckengine(&k, d);

	free(k.used);
	free(k.alloc);
	return k.bad;
}
