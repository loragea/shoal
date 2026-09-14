#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"

/*
 * The monitor's map slot store, docs/design/store.md §10.
 *
 * A few MiB of raw partition through which the monitor makes a
 * published cluster map durable before it acknowledges the publish
 * (layer-a §8.2).  The map text is opaque here: §10 stores bytes and
 * a length, and never parses, compares or orders them.
 *
 *	sector 0	header, copy 0
 *	hdr.curoff	current-map slot 0
 *			current-map slot 1
 *	hdr.histoff	retain history slots, a ring
 *	last sector	header, copy 1
 *
 * The header is written only at format, so the two copies are always
 * identical and the start rule is "take either valid copy": there is
 * no generation to compare.  The current-map slots are §2.2's
 * two-slot rule again, keyed by seq instead of gen — a copy of thirty
 * lines rather than a coupling to superselect, because the two
 * structures share a rule and not a layout.
 *
 * Error strings.  Only an oversize map answers a layer-a §2.6 wire
 * error, `disk full', which is what §10 requires of it.  Every other
 * refusal here is local to the monitor — a damaged header, damaged
 * current slots, a geometry that does not fit — and carries no §2.6
 * prefix, per §3.7's rule that an internal-invariant error never
 * begins with one.
 */

static char monmagic[8] = { 's','h','o','a','l','m','o','n' };
static char mapmagic[8] = { 's','h','o','a','l','m','a','p' };

enum
{
	Hcsumoff	= 16,	/* both headers put the csum at 16 */
};

typedef struct Monslot Monslot;
struct Monslot
{
	int	valid;
	int	phantom;	/* seq above the current map's: never published */
	int	unread;		/* the device read failed: nothing was seen */
	ulong	len;
	uvlong	seq;
	uvlong	epoch;
	uchar	*text;		/* len bytes, nil when len is 0 */
	char	why[ERRMAX];	/* why it is invalid */
};

struct Mon
{
	Dev	*d;
	Monhdr	h;
	int	hdr;		/* header copy taken */
	int	hdrother;	/* the other copy was valid too */
	ulong	nphantom;
	Monslot	cur[2];
	int	curi;		/* the current-map slot in use */
	Monslot	*hist;
	uvlong	seqnext;
	uchar	*buf;		/* slotsz bytes: one slot image */
};

static uvlong
roundup(uvlong n, uvlong m)
{
	return (n + m - 1)/m * m;
}

/*
 * The geometry a header describes, given the device.  Every offset is
 * a sector and every slot is slotsz bytes, so the whole store is
 * 2 header sectors + (2 + retain) slots.
 */
static uvlong
monbytes(ulong secsz, ulong slotsz, ulong retain)
{
	return 2*(uvlong)secsz + (2 + (uvlong)retain)*slotsz;
}

static vlong
slotoff(Mon *m, uvlong sec, int i)
{
	return (vlong)sec*m->d->secsz + (vlong)i*m->h.slotsz;
}

/*
 * A header's own consistency, before anything is located from it.  A
 * slotsz that is not a sector multiple or a retain below 2 would make
 * every offset below meaningless, and §10 makes both MUSTs.
 */
static int
hdrsane(Monhdr *h, Dev *d, vlong size)
{
	if(h->slotsz == 0 || h->slotsz % d->secsz != 0){
		werrstr("slotsz %lud is not a multiple of the %lud-byte sector",
			h->slotsz, d->secsz);
		return -1;
	}
	if(h->slotsz / d->secsz < 2){
		werrstr("slotsz %lud is under two sectors", h->slotsz);
		return -1;
	}
	if(h->retain < Monretainmin){
		werrstr("retain %lud, under the %d layer-a §5.2 clause 2 needs",
			h->retain, Monretainmin);
		return -1;
	}
	if(h->curoff != 1 || h->histoff != 1 + 2*(uvlong)(h->slotsz/d->secsz)){
		werrstr("curoff %llud and histoff %llud do not describe "
			"two current slots after one header sector",
			h->curoff, h->histoff);
		return -1;
	}
	if(monbytes(d->secsz, h->slotsz, h->retain) > (uvlong)size){
		werrstr("a slotsz of %lud and retain of %lud need %llud bytes "
			"of a %lld-byte device", h->slotsz, h->retain,
			monbytes(d->secsz, h->slotsz, h->retain), size);
		return -1;
	}
	return 0;
}

static void
monhdrpack(uchar *p, ulong secsz, Monhdr *h)
{
	memset(p, 0, secsz);
	memmove(p + 0, monmagic, 8);
	PBIT32(p + 8, h->vers);
	PBIT32(p + 12, 0);			/* pad */
	/* 16..31 csum, filled below */
	PBIT32(p + 32, h->slotsz);
	PBIT32(p + 36, h->retain);
	PBIT64(p + 40, h->curoff);
	PBIT64(p + 48, h->histoff);
	reccsumset(p, secsz, Hcsumoff);
}

/*
 * §0's rule for every on-disk structure: the checksum covers the
 * whole header sector with the field zeroed.
 */
static int
monhdrunpack(Monhdr *h, uchar *p, Dev *d)
{
	memset(h, 0, sizeof *h);
	if(memcmp(p, monmagic, 8) != 0){
		werrstr("bad magic");
		return -1;
	}
	h->vers = GBIT32(p + 8);
	if(h->vers != Monvers){
		werrstr("format version %lud, this build implements %d",
			h->vers, Monvers);
		return -1;
	}
	if(!reccsumok(p, d->secsz, Hcsumoff)){
		/*
		 * Not the bare `checksum mismatch' of layer-a §2.6: this
		 * is a diagnostic about one header copy, captured into
		 * Monhsel.why by the only caller, and §3.7 keeps an
		 * internal-invariant error from beginning with a wire
		 * error's prefix even where it cannot escape today.
		 */
		werrstr("the header checksum does not match");
		return -1;
	}
	h->slotsz = GBIT32(p + 32);
	h->retain = GBIT32(p + 36);
	h->curoff = GBIT64(p + 40);
	h->histoff = GBIT64(p + 48);
	return hdrsane(h, d, d->size);
}

static vlong
hdr1off(Dev *d)
{
	return d->size - d->secsz;
}

/*
 * Read both header copies.  Nothing ever writes either after format,
 * so they are identical by construction and there is nothing to
 * choose between: take either valid one, refuse if neither.  Two
 * valid copies that DIFFER are not a choice either — a difference
 * means the operator has mixed two partitions' halves or the media is
 * lying — so it is a refusal that names the field.
 */
int
monhdrsel(Dev *d, Monhsel *sel)
{
	uchar *buf;
	Monhdr *a, *b;
	int i;

	memset(sel, 0, sizeof *sel);
	sel->use = -1;
	/*
	 * Copy 1 is the LAST sector, so a device with fewer than two of
	 * them has nowhere to read it from and hdr1off would name an
	 * offset before the start.  The guard is here rather than in
	 * the callers because every one of them — monfmt, and both
	 * commands asking whether an image is a store — may be handed
	 * an empty or truncated file.
	 */
	if(d->size < 2*(vlong)d->secsz){
		for(i = 0; i < 2; i++)
			snprint(sel->why[i], sizeof sel->why[i],
				"a %lld-byte device holds no header sector",
				d->size);
		werrstr("no valid monitor header: %s", sel->why[0]);
		return -1;
	}
	if((buf = malloc(d->secsz)) == nil)
		return -1;
	for(i = 0; i < 2; i++){
		if(devread(d, buf, d->secsz, i == 0 ? 0 : hdr1off(d)) < 0){
			snprint(sel->why[i], sizeof sel->why[i],
				"unreadable: %r");
			continue;
		}
		if(monhdrunpack(&sel->h[i], buf, d) < 0){
			snprint(sel->why[i], sizeof sel->why[i], "%r");
			continue;
		}
		sel->valid[i] = 1;
	}
	free(buf);

	if(sel->valid[0] && sel->valid[1]){
		a = &sel->h[0];
		b = &sel->h[1];
		if(a->slotsz != b->slotsz)
			werrstr("the header copies differ: slotsz %lud and %lud",
				a->slotsz, b->slotsz);
		else if(a->retain != b->retain)
			werrstr("the header copies differ: retain %lud and %lud",
				a->retain, b->retain);
		else if(a->curoff != b->curoff)
			werrstr("the header copies differ: curoff %llud and %llud",
				a->curoff, b->curoff);
		else if(a->histoff != b->histoff)
			werrstr("the header copies differ: histoff %llud "
				"and %llud", a->histoff, b->histoff);
		else{
			sel->use = 0;
			return 0;
		}
		return -1;
	}
	if(sel->valid[0] || sel->valid[1]){
		sel->use = sel->valid[0] ? 0 : 1;
		return 0;
	}
	werrstr("no valid monitor header: copy 0 %s; copy 1 %s",
		sel->why[0], sel->why[1]);
	return -1;
}

static void
slotclear(Monslot *sl)
{
	free(sl->text);
	memset(sl, 0, sizeof *sl);
}

/*
 * Read one slot.  len is bounds-checked BEFORE the len bytes it names
 * are read, so a torn length field cannot drive a read past the slot;
 * and the checksum covers exactly secsz+len bytes, so whatever a
 * previously longer map left past len is outside it (§10).
 */
static void
slotread(Mon *m, vlong off, Monslot *sl)
{
	Dev *d;
	ulong len;

	d = m->d;
	slotclear(sl);
	if(devread(d, m->buf, d->secsz, off) < 0){
		snprint(sl->why, sizeof sl->why, "unreadable: %r");
		sl->unread = 1;
		return;
	}
	if(memcmp(m->buf, mapmagic, 8) != 0){
		snprint(sl->why, sizeof sl->why, "bad magic");
		return;
	}
	if(GBIT32(m->buf + 8) != Monvers){
		snprint(sl->why, sizeof sl->why, "format version %lud, this "
			"build implements %d", (ulong)GBIT32(m->buf + 8),
			Monvers);
		return;
	}
	len = GBIT32(m->buf + 12);
	if(len > m->h.slotsz - d->secsz){
		snprint(sl->why, sizeof sl->why, "len %lud does not fit a "
			"%lud-byte slot", len, m->h.slotsz);
		return;
	}
	if(len > 0 && devread(d, m->buf + d->secsz,
		roundup(len, d->secsz), off + d->secsz) < 0){
		snprint(sl->why, sizeof sl->why, "unreadable: %r");
		sl->unread = 1;
		return;
	}
	if(!reccsumok(m->buf, d->secsz + len, Hcsumoff)){
		snprint(sl->why, sizeof sl->why, "checksum mismatch");
		return;
	}
	sl->len = len;
	sl->seq = GBIT64(m->buf + 32);
	sl->epoch = GBIT64(m->buf + 40);
	if(len > 0){
		if((sl->text = malloc(len)) == nil){
			snprint(sl->why, sizeof sl->why, "out of memory: %r");
			return;
		}
		memmove(sl->text, m->buf + d->secsz, len);
	}
	sl->valid = 1;
}

/*
 * Read a slot back after its flush and check that it is the slot that
 * was just written (§10).  A write that reports success, survives its
 * flush and lands nothing would otherwise be invisible until the next
 * open: the ring would hold no entry for the current map, so position
 * 0 would not be the current map and layer-a §8.2's E−1 entry would
 * be unanswerable.
 *
 * The read-back has two outcomes that are not the same fact, and §10
 * separates them.  A slot that reads back and is not the one written
 * says the map is NOT durable.  A read that FAILS says nothing about
 * the write at all: the slot may be on the platter.  The read is
 * retried once — a media read has its own transients, and the write
 * and the flush under it have already reported success — and if it
 * fails again the answer is Vunknown, which §10 calls an
 * indeterminate publish.
 */
enum
{
	Vok	= 0,	/* the platter holds the slot that was written */
	Vwrong,		/* it read back, and it is not that slot */
	Vunknown,	/* it could not be read back, twice */
};

static int
slotverify(Mon *m, vlong off, ulong len, uvlong seq, uvlong epoch)
{
	Monslot sl;
	char why[ERRMAX];

	memset(&sl, 0, sizeof sl);
	slotread(m, off, &sl);
	if(sl.unread)
		slotread(m, off, &sl);
	if(sl.unread){
		snprint(why, sizeof why, "%s", sl.why);
		slotclear(&sl);
		/* short enough that ERRMAX leaves room for why */
		werrstr("indeterminate publish: seq %llud epoch %llud was "
			"written and flushed but would not read back: %s",
			seq, epoch, why);
		return Vunknown;
	}
	if(!sl.valid)
		snprint(why, sizeof why, "%s", sl.why);
	else if(sl.len != len || sl.seq != seq || sl.epoch != epoch)
		snprint(why, sizeof why, "it holds len %lud seq %llud "
			"epoch %llud", sl.len, sl.seq, sl.epoch);
	else{
		slotclear(&sl);
		return Vok;
	}
	slotclear(&sl);
	werrstr("the slot did not read back after its flush (len %lud "
		"seq %llud epoch %llud): %s", len, seq, epoch, why);
	return Vwrong;
}

/*
 * Write one slot, flush it and read it back, with the crash point §13
 * names after the write returns and before the flush.  The write is
 * rounded up to roundup(secsz+len, secsz) and zero-padded, because
 * devsd turns a write whose byte count is not a sector multiple into
 * a read-modify-write; it is one devwrite, which splits at Wunit
 * itself.
 *
 * It answers 0, -1 for a slot the platter does not hold, and -2 when
 * the read-back could not say (§10's indeterminate publish): the
 * caller's bookkeeping differs, because -2 leaves bytes that may be
 * durable and -1 does not.
 */
static int
slotwrite(Mon *m, vlong off, ulong len, uvlong seq, uvlong epoch,
	void *text, char *point, char *after)
{
	Dev *d;
	ulong n;

	d = m->d;
	n = roundup(d->secsz + len, d->secsz);
	memset(m->buf, 0, n);
	memmove(m->buf + 0, mapmagic, 8);
	PBIT32(m->buf + 8, Monvers);
	PBIT32(m->buf + 12, len);
	PBIT64(m->buf + 32, seq);
	PBIT64(m->buf + 40, epoch);
	if(len > 0)
		memmove(m->buf + d->secsz, text, len);
	reccsumset(m->buf, d->secsz + len, Hcsumoff);
	if(devwrite(d, m->buf, n, off) < 0)
		return -1;
	devpoint(d, point, 0);
	if(devflush(d) < 0)
		return -1;
	switch(slotverify(m, off, len, seq, epoch)){
	case Vwrong:
		return -1;
	case Vunknown:
		return -2;
	}
	if(after != nil)
		devpoint(d, after, 0);
	return 0;
}

static void
slotset(Monslot *sl, ulong len, uvlong seq, uvlong epoch, void *text)
{
	slotclear(sl);
	sl->len = len;
	sl->seq = seq;
	sl->epoch = epoch;
	if(len > 0){
		if((sl->text = malloc(len)) == nil)
			sysfatal("monitor slot: %r");
		memmove(sl->text, text, len);
	}
	sl->valid = 1;
}

/*
 * Every geometry and size refusal a format makes, asked of a size
 * that need not be the device's own.  `shoalmonfmt -z' asks it about
 * the length the image would be given, BEFORE resizing it, because
 * §12 has a refused run leave the file byte-identical; monfmt below
 * asks it about the device it is about to write.  It fills in c's
 * defaults, so a caller may report what would be used.
 *
 * §10 sizes the partition at 4 MiB and refuses less than 1 MiB.  That
 * floor is about the deployment and not about the arithmetic — a
 * 1 MiB partition holds the default geometry with room to spare — so
 * it is checked before the geometry, whose own refusal would
 * otherwise answer for it.  It is asked of the size as given, not of
 * the sector-rounded device length, so that a 40-byte image is
 * refused as 40 bytes and not as 0.
 */
int
monfmtcheck(Dev *d, vlong size, Monfmtcfg *c)
{
	Monhdr h;
	ulong slotsecs;

	if(c->slotsz == 0)
		c->slotsz = Monslotszdflt;
	if(c->retain == 0)
		c->retain = Monretaindflt;
	memset(&h, 0, sizeof h);
	h.vers = Monvers;
	h.slotsz = c->slotsz;
	h.retain = c->retain;
	slotsecs = 0;
	if(c->slotsz % d->secsz == 0)
		slotsecs = c->slotsz / d->secsz;
	h.curoff = 1;
	h.histoff = 1 + 2*(uvlong)slotsecs;
	if(size < Monminbytes){
		werrstr("%lld bytes is under the %d-byte minimum §10 sets",
			size, Monminbytes);
		return -1;
	}
	return hdrsane(&h, d, size - size % (vlong)d->secsz);
}

/*
 * Format, §10 and §12.  Every refusal shoalmonfmt makes it makes
 * here, so that a T1 program drives the tool's decisions rather than
 * its argument parsing — with the two exceptions §12 names, both of
 * them about a file image the library never sees: which length to
 * open it at, and whether `-z' may shorten it.
 *
 * What a fresh store holds is §10's format-time contents and is not
 * arbitrary: both current slots are written as VALID empty maps at
 * len 0, seq 0, epoch 0, and the header sector of every history slot
 * is zeroed.  §10 copies §2.2's "if neither is valid, refuse" clause
 * into the commit rule, and without valid slots at format a fresh
 * store and a doubly damaged one would be indistinguishable at the
 * first commit.  Zeroing the history headers is the other half: no
 * slot an earlier format on the same bytes wrote survives as valid.
 * "The store holds no map" is then the chosen slot's len being 0, and
 * nothing else.
 *
 * The two header copies are written LAST, over headers zeroed first,
 * for the reason §12 gives shoalfmt: a format cut short must leave no
 * valid header rather than a valid one locating slots that were never
 * written — which, after a reformat at a different slotsz, would be
 * the previous store's header over this one's bytes.  §13's monfmthdr
 * point is exactly there — after the zeroing flush, before any other
 * write — so that a test can stage that durable state and watch the
 * open refuse it.
 */
int
monfmt(Dev *d, Monfmtcfg *c)
{
	Monhdr h;
	Sbsel sel;
	uchar *buf;
	uvlong need;
	int i;

	c->warnsuper = 0;
	/*
	 * Reformatting destroys every published map this partition
	 * holds, so it takes a flag, exactly as shoalfmt -r does.  It
	 * is asked BEFORE the geometry and the size, so that a run
	 * refused for either of those is refused over a store still
	 * standing: §12 has the tools resize an image only past this
	 * point, and a refusal here must be the one they report.
	 */
	if(!c->ream){
		Monhsel hs;

		if(monhdrsel(d, &hs) == 0){
			werrstr("already carries a valid monitor header "
				"(copy %d, slotsz %lud, retain %lud)",
				hs.use, hs.h[hs.use].slotsz,
				hs.h[hs.use].retain);
			return -1;
		}
	}
	/*
	 * §12: a unit that carries a valid object-store superblock is
	 * an object-store instance's, and §2.1's deployment rule says
	 * the monitor's partition MUST NOT be one.  It is a warning
	 * and not a refusal here — the operator may be reclaiming a
	 * decommissioned unit — so the caller is told and decides.
	 * `shoalmonfmt' is the caller that decides it needs -r.
	 */
	if(superselect(d, &sel) == 0)
		c->warnsuper = 1;

	if(monfmtcheck(d, d->size, c) < 0)
		return -1;
	memset(&h, 0, sizeof h);
	h.vers = Monvers;
	h.slotsz = c->slotsz;
	h.retain = c->retain;
	h.curoff = 1;
	h.histoff = 1 + 2*(uvlong)(c->slotsz/d->secsz);
	need = monbytes(d->secsz, c->slotsz, c->retain);

	if((buf = mallocz(d->secsz, 1)) == nil)
		return -1;
	/* no valid header while the slots under it are half written */
	if(devwrite(d, buf, d->secsz, 0) < 0
	|| devwrite(d, buf, d->secsz, hdr1off(d)) < 0
	|| devflush(d) < 0)
		goto bad;
	devpoint(d, "monfmthdr", 0);

	/* the header sector of every history slot: zero, so invalid */
	for(i = 0; i < (int)c->retain; i++)
		if(devwrite(d, buf, d->secsz,
			(vlong)h.histoff*d->secsz + (vlong)i*c->slotsz) < 0)
			goto bad;

	/* both current slots, as valid empty maps */
	memmove(buf + 0, mapmagic, 8);
	PBIT32(buf + 8, Monvers);
	PBIT32(buf + 12, 0);			/* len */
	PBIT64(buf + 32, 0);			/* seq */
	PBIT64(buf + 40, 0);			/* epoch */
	reccsumset(buf, d->secsz, Hcsumoff);
	for(i = 0; i < 2; i++)
		if(devwrite(d, buf, d->secsz,
			(vlong)h.curoff*d->secsz + (vlong)i*c->slotsz) < 0)
			goto bad;
	if(devflush(d) < 0)
		goto bad;

	monhdrpack(buf, d->secsz, &h);
	if(devwrite(d, buf, d->secsz, 0) < 0)
		goto bad;
	if(devwrite(d, buf, d->secsz, hdr1off(d)) < 0)
		goto bad;
	if(devflush(d) < 0)
		goto bad;
	free(buf);
	c->curoff = h.curoff;
	c->histoff = h.histoff;
	c->used = need;
	return 0;
bad:
	free(buf);
	return -1;
}

/*
 * Open.  Reads both header copies and takes either valid one; reads
 * both current slots and the whole ring; chooses the current map by
 * §2.2's rule keyed on seq; and marks every history slot whose seq
 * exceeds the chosen current slot's as a phantom — the entry of a
 * publish that wrote its ring slot and never published its map (§10).
 *
 * It WRITES NOTHING, so it works on a device opened read-only: a
 * phantom is marked reusable in memory and erased by the write that
 * reuses its slot, which is the next commit's.
 */
Mon*
monopen(Dev *d)
{
	Mon *m;
	Monhsel hs;
	uvlong max;
	int i;

	if(monhdrsel(d, &hs) < 0)
		return nil;
	if((m = mallocz(sizeof *m, 1)) == nil)
		return nil;
	m->d = d;
	m->h = hs.h[hs.use];
	m->hdr = hs.use;
	m->hdrother = hs.valid[hs.use == 0 ? 1 : 0];
	m->curi = -1;
	if((m->buf = malloc(m->h.slotsz)) == nil
	|| (m->hist = mallocz(m->h.retain*sizeof(Monslot), 1)) == nil){
		monclose(m);
		return nil;
	}
	for(i = 0; i < 2; i++)
		slotread(m, slotoff(m, m->h.curoff, i), &m->cur[i]);
	if(m->cur[0].valid && m->cur[1].valid)
		m->curi = m->cur[0].seq >= m->cur[1].seq ? 0 : 1;
	else if(m->cur[0].valid || m->cur[1].valid)
		m->curi = m->cur[0].valid ? 0 : 1;
	else{
		werrstr("no valid current-map slot: slot 0 %s; slot 1 %s",
			m->cur[0].why, m->cur[1].why);
		monclose(m);
		return nil;
	}
	max = m->cur[0].valid ? m->cur[0].seq : 0;
	if(m->cur[1].valid && m->cur[1].seq > max)
		max = m->cur[1].seq;
	for(i = 0; i < (int)m->h.retain; i++){
		slotread(m, slotoff(m, m->h.histoff, i), &m->hist[i]);
		if(!m->hist[i].valid)
			continue;
		if(m->hist[i].seq > max)
			max = m->hist[i].seq;
		if(m->hist[i].seq > m->cur[m->curi].seq){
			m->hist[i].phantom = 1;
			m->nphantom++;
		}
	}
	/*
	 * One seq space for the whole store, and the maximum is taken
	 * over the phantoms too: they are valid slots holding that seq,
	 * and a new commit that reused one of their numbers would put
	 * two entries with one seq in the ring.
	 */
	m->seqnext = max + 1;
	return m;
}

void
monclose(Mon *m)
{
	int i;

	if(m == nil)
		return;
	for(i = 0; i < 2; i++)
		slotclear(&m->cur[i]);
	if(m->hist != nil)
		for(i = 0; i < (int)m->h.retain; i++)
			slotclear(&m->hist[i]);
	free(m->hist);
	free(m->buf);
	free(m);
}

/*
 * The history victim.  A phantom first, an invalid slot next, and
 * failing both the valid slot with the lowest seq (§10).
 *
 * Phantoms come first, and that ordering is what bounds the ring to
 * one of them: a phantom's seq is above the current map's only until
 * the next map is published, after which the same bytes would read
 * back as an ordinary history entry for a map that never existed.
 * Consuming it at the next commit — the one whose publish either
 * completes, making the entry real, or does not, making it the
 * phantom again — is what keeps §10's "no committed history is
 * erased" and "no phantom survives" both true.
 */
static int
histvictim(Mon *m)
{
	int i, v;

	for(i = 0; i < (int)m->h.retain; i++)
		if(m->hist[i].phantom)
			return i;
	for(i = 0; i < (int)m->h.retain; i++)
		if(!m->hist[i].valid)
			return i;
	v = 0;
	for(i = 1; i < (int)m->h.retain; i++)
		if(m->hist[i].seq < m->hist[v].seq)
			v = i;
	return v;
}

/*
 * The books after a ring write that failed.  The slot is invalid in
 * memory, but the platter was not told: a write that landed nothing
 * leaves the victim's own bytes there, and a victim that was a
 * phantom is STILL a phantom on the disk.  Forgetting that would send
 * the next commit's victim search past it to some other slot, the
 * next published map would raise seq above the phantom's, and it
 * would read back at the following open as ordinary history for a map
 * that was never published.  So the slot stays first in line for
 * reuse — invalid and phantom both — and nphantom counts it once.
 *
 * Unless the platter says otherwise.  When the ring is full the
 * victim is an ordinary committed entry, and a write that landed
 * nothing left it exactly where it was: one read says so, and an
 * entry whose seq is not above the current map's is history this
 * store can still answer, so it goes back into memory as it is found
 * rather than vanishing from monhistory and monlookup — and counting
 * as a phantom the disk does not hold — until the next open.  The
 * read is not made after an INDETERMINATE write (§10): the read-back
 * has already failed twice on this slot, so whatever is there is not
 * to be trusted as the victim's own bytes, and the slot must be the
 * next one reused.
 */
static void
histfail(Mon *m, int v, char *err, int indet)
{
	Monslot sl;
	int wasphantom;

	wasphantom = m->hist[v].phantom;
	if(!indet){
		memset(&sl, 0, sizeof sl);
		slotread(m, slotoff(m, m->h.histoff, v), &sl);
		if(sl.valid && sl.seq <= m->cur[m->curi].seq){
			slotclear(&m->hist[v]);
			m->hist[v] = sl;
			if(wasphantom && m->nphantom > 0)
				m->nphantom--;
			return;
		}
		slotclear(&sl);
	}
	slotclear(&m->hist[v]);
	snprint(m->hist[v].why, sizeof m->hist[v].why,
		"the write that failed the commit: %s", err);
	m->hist[v].phantom = 1;
	if(!wasphantom)
		m->nphantom++;
}

/*
 * Commit, §10, in two steps with one flush each.
 *
 * Step 1 writes the ring entry for the new map, stamped with the seq
 * step 2 is about to carry, and flushes.  It is first because the
 * entry it writes is the one the NEXT publish must keep as E−1
 * (layer-a §5.2 clause 2): a torn ring write then damages only the
 * map being published, which fails the commit, rather than the
 * previous map, which nothing else can supply.  Step 2 writes the
 * current-map slot by §2.2's three clauses keyed on seq, and flushes.
 *
 * The three crash points are §13's, named through devpoint: monhist
 * after the ring write returns and before its flush, monhistflush
 * after that flush and before the current-slot write — the phantom
 * window — and moncur after the current-slot write and before its
 * flush.
 */
int
moncommit(Mon *m, void *text, ulong len, uvlong epoch)
{
	char err[ERRMAX];
	uvlong seq;
	int v, c, r;

	if(len > m->h.slotsz - m->d->secsz){
		/*
		 * layer-a §2.6's string, which §10 requires here: the
		 * text is valid and the partition is too small, so
		 * `bad map' would send an operator to the wrong place.
		 * It is the one wire error this store answers.
		 */
		werrstr("disk full: a %lud-byte map needs %llud bytes of a "
			"%lud-byte slot", len,
			(uvlong)m->d->secsz + len, m->h.slotsz);
		return -1;
	}
	seq = m->seqnext;
	v = histvictim(m);
	r = slotwrite(m, slotoff(m, m->h.histoff, v), len, seq, epoch, text,
		"monhist", "monhistflush");
	if(r < 0){
		rerrstr(err, sizeof err);
		/*
		 * An indeterminate write may be on the platter carrying
		 * this seq, so the seq is spent whether it is or not: the
		 * next commit's is above it, which makes a retry outrank
		 * anything that landed and keeps two entries from sharing
		 * one seq.  A determinate failure landed nothing and
		 * leaves the number to be used again.  histfail keeps the
		 * rest of the books; the sibling path below, for a failed
		 * current-slot write, keeps them for that case.
		 */
		if(r == -2)
			m->seqnext = seq + 1;
		histfail(m, v, err, r == -2);
		errstr(err, sizeof err);
		return -1;
	}
	/*
	 * The seq is spent whatever happens next: a slot on the disk
	 * carries it now.
	 */
	m->seqnext = seq + 1;
	if(m->nphantom > 0 && m->hist[v].phantom)
		m->nphantom--;
	slotset(&m->hist[v], len, seq, epoch, text);

	if(m->cur[0].valid && m->cur[1].valid)
		c = m->cur[0].seq < m->cur[1].seq ? 0 : 1;
	else if(m->cur[0].valid || m->cur[1].valid)
		c = m->cur[0].valid ? 1 : 0;
	else{
		werrstr("no valid current-map slot: slot 0 %s; slot 1 %s",
			m->cur[0].why, m->cur[1].why);
		m->hist[v].phantom = 1;
		m->nphantom++;
		return -1;
	}
	/*
	 * A failure here — the write, the flush or the read-back, and
	 * INDETERMINATE or not — leaves the slot unserved in memory and
	 * the ring entry a phantom.  The seq was spent above, so a
	 * retry in this session outranks an indeterminate slot that did
	 * land, and the retry writes over it: the selection above takes
	 * the invalid slot, which is this one.
	 */
	if(slotwrite(m, slotoff(m, m->h.curoff, c), len, seq, epoch, text,
		"moncur", nil) < 0){
		rerrstr(err, sizeof err);
		slotclear(&m->cur[c]);
		snprint(m->cur[c].why, sizeof m->cur[c].why,
			"the write that failed the commit: %s", err);
		/* the ring entry is a publish that did not complete */
		m->hist[v].phantom = 1;
		m->nphantom++;
		errstr(err, sizeof err);
		return -1;
	}
	slotset(&m->cur[c], len, seq, epoch, text);
	m->curi = c;
	return 0;
}

static void
fillmap(Monslot *sl, Monmap *mm)
{
	mm->text = sl->text;
	mm->len = sl->len;
	mm->seq = sl->seq;
	mm->epoch = sl->epoch;
}

/*
 * The current map, or 0 for "this store holds no map" — which is the
 * chosen slot carrying len 0, the state a fresh format leaves.  An
 * empty map text is not a map.
 *
 * The text is the store's own and is valid until the next commit or
 * monclose; a caller that wants it longer copies it.
 */
int
moncurrent(Mon *m, Monmap *mm)
{
	memset(mm, 0, sizeof *mm);
	if(m->cur[m->curi].len == 0)
		return 0;
	fillmap(&m->cur[m->curi], mm);
	return 1;
}

/*
 * The ring, newest first.  Position 0 is the current map itself,
 * because §10's step 1 writes the ring entry for every published map;
 * past the end the answer is 0.  Phantoms are not in it: they are the
 * maps that were never published.
 *
 * Every valid entry carries a distinct seq — one seq space for the
 * whole store — so an entry's position is the number of valid
 * entries above it, and no sort is needed for a ring of eight.
 */
int
monhistory(Mon *m, ulong i, Monmap *mm)
{
	int j, k;
	ulong rank;

	memset(mm, 0, sizeof *mm);
	for(j = 0; j < (int)m->h.retain; j++){
		if(!m->hist[j].valid || m->hist[j].phantom)
			continue;
		rank = 0;
		for(k = 0; k < (int)m->h.retain; k++)
			if(m->hist[k].valid && !m->hist[k].phantom
			&& m->hist[k].seq > m->hist[j].seq)
				rank++;
		if(rank == i){
			fillmap(&m->hist[j], mm);
			return 1;
		}
	}
	return 0;
}

/*
 * The by-epoch lookup, layer-a §8.2's /maps/<epoch>.  The store never
 * compares epochs — §10 records what it is given, "for the operator's
 * benefit", and layer-a §8.3's forceepoch and §8.6's rebuild path can
 * legitimately publish an epoch that is not above the last — so two
 * ring entries may carry one epoch.  The newer publish is the answer,
 * which is the one with the greater seq.
 */
int
monlookup(Mon *m, uvlong epoch, Monmap *mm)
{
	int j, best;

	memset(mm, 0, sizeof *mm);
	best = -1;
	for(j = 0; j < (int)m->h.retain; j++){
		if(!m->hist[j].valid || m->hist[j].phantom)
			continue;
		if(m->hist[j].epoch != epoch)
			continue;
		if(best < 0 || m->hist[j].seq > m->hist[best].seq)
			best = j;
	}
	if(best < 0)
		return 0;
	fillmap(&m->hist[best], mm);
	return 1;
}

void
monstat(Mon *m, Monstat *st)
{
	int j;

	memset(st, 0, sizeof *st);
	st->slotsz = m->h.slotsz;
	st->retain = m->h.retain;
	st->curoff = m->h.curoff;
	st->histoff = m->h.histoff;
	st->nphantom = m->nphantom;
	st->seq = m->cur[m->curi].seq;
	st->epoch = m->cur[m->curi].epoch;
	st->len = m->cur[m->curi].len;
	st->hasmap = m->cur[m->curi].len > 0;
	st->cur = m->curi;
	st->hdr = m->hdr;
	st->hdrother = m->hdrother;
	for(j = 0; j < (int)m->h.retain; j++)
		if(m->hist[j].valid && !m->hist[j].phantom)
			st->nhist++;
}
