#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../lib/shoal.h"
#include "srv.h"
#include "dat.h"
#include "fns.h"

/*
 * The peer channels: layer-a §5.5's /repl push channel and §5.6's
 * /rpc request/response channel.  Both are single files rather than
 * directories — a sender opens one fid and streams operations for many
 * objects through it — and both take one operation per Twrite, as a
 * header line (§0's one physical line) followed, where the operation
 * defines a payload, by exactly `n' bytes in the same Twrite.
 *
 * Nothing here sends: store.md §14(18) leaves this build without a
 * peer client, so what is built is the RECEIVING half of both
 * channels.  That half is complete — every operation of §5.5 and §5.6
 * is answered here — and it is what a peer with a client would speak
 * to.
 *
 * Where the work runs.  Every operation but §5.6's op=list names an
 * object, so it runs on that object's queue (§5.4.1, store.md §7) and
 * leaves through srvqdone; op=list names none and goes to the reserved
 * queue srvqpushany owns.  What the service loop keeps is the parse
 * and the refusals that need no engine call — the malformed header,
 * the epoch and, for /rpc, the claim on the fid — because store.md
 * §3.8 requires a header carrying ver=0 to be refused `bad ctl' BEFORE
 * any engine call, and because a refusal answered on the loop costs no
 * queue.  The header is parsed a second time on the queue rather than
 * carried there: hdrparse is a pure function of the request's own
 * bytes, which lib9p holds until the handler responds, and a parse of
 * a header under 512 bytes is cheaper than an allocation whose
 * lifetime would have to be threaded through the pool.
 *
 * The order the refusals are made in, which is §5.5's receiver rules
 * read top to bottom:
 *
 *	the role		the row's matrix (tree.c), and §6.4 F1's
 *				fence in the rows' gate (chgate)
 *	the header		`bad ctl', and `bad object name' for an
 *				oid outside §1.1 — including a version of
 *				0, which §3.8 makes a malformed header
 *	the epoch		`stale epoch' below ours, `future epoch'
 *				above it (§5.5; §14(21) says the fetch it
 *				SHOULD trigger cannot be made here)
 *	the payload's dcsum	`checksum mismatch' (§5.5)
 *	the bounds		`object too large' (§2.6)
 *	the arbitration		`out of sequence' for a delta op whose
 *				predecessor key is not ours, `stale
 *				version' where the key we hold defends
 *				itself (§5.3, §5.5)
 *	the commit		which makes the resulting-csum check and
 *				answers `checksum mismatch' before
 *				anything is durable (store.md §3.8, D23)
 *
 * Arbitration is split by call, not by where the key came from
 * (store.md §3.8): objwritecsum, objtrunccsum and objremovecsum
 * compare nothing and apply the key they are handed, so the comparison
 * for those is made here under the object's queue; stagefinalcsum and
 * objadoptcsum make layer-a §5.5's comparison themselves and answer
 * `stale version' when it fails.
 */

enum
{
	/*
	 * §5.5: "The longest header defined above is under 512 bytes."
	 * A Twrite whose first line is longer than this carries no header
	 * this design defines, and one with no newline at all inside it
	 * is not a header line either (§0).
	 */
	Maxhdr		= 1024,
	Maxfield	= 20,

	/* §5.6's op=list paging */
	Listdflt	= 64,		/* `n=' absent */
	Listmax		= 256,		/* `n=' clamped before the msize does */
	Listhdr		= 64,		/* room kept for the `list lines=' line */

	/*
	 * Room kept for the `get' response line, which is the longest
	 * fixed part of any response here: `oid=' and §1.1's 128 bytes,
	 * three u64s, a u32 and 32 hex characters of dcsum.
	 */
	Gethdr		= 320,
};

/* the two channels, which share the grammar and not the operations */
enum
{
	Crepl	= 1<<0,
	Crpc	= 1<<1,
};

/* §5.5's five operations and §5.6's six */
enum
{
	Ocreate	= 0,
	Owrite,
	Otrunc,
	Odelete,
	Ofull,
	Ometa,
	Oget,
	Olist,
	Odrop,
	Overify,
	Odiscard,
	Nop,
};

/* every attribute either channel's grammar defines */
enum
{
	Fop	= 0,
	Foid,
	Fepoch,
	Fver,
	Fwepoch,
	Fpver,
	Fpwepoch,
	Foff,
	Fn,
	Flen,
	Fdcsum,
	Fcsum,
	Ffinal,
	Fforce,
	Fafter,
	Nfield,
};

typedef struct Fld Fld;
typedef struct Op Op;
typedef struct Hdr Hdr;
typedef struct Rpc Rpc;

struct Fld
{
	char	*name;
	int	kind;
};

enum
{
	Kid	= 0,		/* an oid (§1.1), 1*128 bytes */
	Ku64,
	Ku32,
	Kflag,			/* 0 or 1 */
	Khex16,			/* dcsum: BLAKE2s-128, 32 hex characters */
	Khex32,			/* csum: §1.4's hex64 */
	Kword,			/* op= */
};

static Fld srvflds[Nfield] =
{
[Fop]		= { "op",		Kword },
[Foid]		= { "oid",	Kid },
[Fepoch]	= { "epoch",	Ku64 },
[Fver]		= { "ver",	Ku64 },
[Fwepoch]	= { "wepoch",	Ku64 },
[Fpver]		= { "pver",	Ku64 },
[Fpwepoch]	= { "pwepoch",	Ku64 },
[Foff]		= { "off",	Ku64 },
[Fn]		= { "n",		Ku32 },
[Flen]		= { "len",	Ku64 },
[Fdcsum]	= { "dcsum",	Khex16 },
[Fcsum]		= { "csum",	Khex32 },
[Ffinal]	= { "final",	Kflag },
[Fforce]	= { "force",	Kflag },
[Fafter]	= { "after",	Kid },
};

#define B(f)	(1UL<<(f))

/*
 * One operation of either grammar: which channel carries it, which
 * attributes it requires, which it allows, and whether `n=' counts
 * payload bytes that must follow the header in the same Twrite.  The
 * masks are §5.5's and §5.6's request grammars verbatim, so an
 * attribute an operation does not define is `bad ctl' rather than
 * ignored: a sender that names one means something this receiver does
 * not do.
 */
struct Op
{
	char	*name;
	int	chan;
	ulong	req;
	ulong	opt;
	int	payload;
};

static Op srvops[Nop] =
{
[Ocreate] = {
	.name	= "create",
	.chan	= Crepl,
	.req	= B(Foid)|B(Fepoch)|B(Fver)|B(Fwepoch)|B(Fcsum),
},
[Owrite] = {
	.name	= "write",
	.chan	= Crepl,
	.req	= B(Foid)|B(Fepoch)|B(Fver)|B(Fwepoch)|B(Fpver)|B(Fpwepoch)
		 |B(Foff)|B(Fn)|B(Fdcsum)|B(Fcsum),
	.payload= 1,
},
[Otrunc] = {
	.name	= "trunc",
	.chan	= Crepl,
	.req	= B(Foid)|B(Fepoch)|B(Fver)|B(Fwepoch)|B(Fpver)|B(Fpwepoch)
		 |B(Flen)|B(Fcsum),
},
[Odelete] = {
	.name	= "delete",
	.chan	= Crepl,
	.req	= B(Foid)|B(Fepoch)|B(Fver)|B(Fwepoch)|B(Fcsum),
},
[Ofull] = {
	.name	= "full",
	.chan	= Crepl,
	.req	= B(Foid)|B(Fepoch)|B(Fver)|B(Fwepoch)|B(Flen)|B(Foff)|B(Fn)
		 |B(Fdcsum)|B(Fcsum)|B(Ffinal),
	.opt	= B(Fforce),
	.payload= 1,
},
[Ometa] = {
	.name	= "meta",
	.chan	= Crpc,
	.req	= B(Foid)|B(Fepoch),
},
[Oget] = {
	.name	= "get",
	.chan	= Crpc,
	.req	= B(Foid)|B(Fepoch)|B(Fver)|B(Fwepoch)|B(Foff)|B(Fn),
},
[Olist] = {
	.name	= "list",
	.chan	= Crpc,
	.req	= B(Fepoch),
	.opt	= B(Fafter)|B(Fn),
},
[Odrop] = {
	.name	= "drop",
	.chan	= Crpc,
	.req	= B(Foid)|B(Fepoch),
},
[Overify] = {
	.name	= "verify",
	.chan	= Crpc,
	.req	= B(Foid)|B(Fepoch),
},
[Odiscard] = {
	.name	= "discard",
	.chan	= Crpc,
	.req	= B(Foid)|B(Fepoch)|B(Fver)|B(Fwepoch),
},
};

/* one parsed header, plus the payload that followed it in the Twrite */
struct Hdr
{
	int	op;
	ulong	have;
	uchar	oid[Oidmax];
	int	oidlen;
	uchar	after[Oidmax];
	int	afterlen;
	uvlong	epoch, ver, wepoch, pver, pwepoch, off, len;
	ulong	n;
	int	final, force;
	uchar	dcsum[Blkdlen];
	uchar	csum[Csumlen];
	void	*data;			/* the payload, inside the Req's buffer */
};

/*
 * §5.6's per-fid state: the one request that may be outstanding and
 * the one response that may be buffered.  It is what `aux' names on an
 * open /rpc fid, under the fid's own state lock like every other
 * per-fid state (dat.h), and its three hooks are below.
 *
 * `busy' is a Twrite this fid has not answered — §5.6's "at most one
 * outstanding request per fid".  `t' is the prepared response and
 * `delivered' says a Tread has taken it: §5.6 destroys a buffered
 * response at the next Twrite and at the Tclunk and by nothing else,
 * and answers a Tread that follows the delivery with count 0, so the
 * bytes stay until one of those two moments and the flag is what makes
 * the second read an end of data rather than a repeat.
 */
struct Rpc
{
	Srvctx	*ctx;
	int	busy;
	int	delivered;
	Text	*t;
};

static char Eoom[] = "shoalsrv: out of memory";

static char*
hexof(char *out, uchar *p, int n)
{
	static char hex[] = "0123456789abcdef";
	int i;

	for(i = 0; i < n; i++){
		out[2*i] = hex[p[i]>>4];
		out[2*i+1] = hex[p[i]&15];
	}
	out[2*n] = 0;
	return out;
}

static int
hexval(int ch)
{
	if(ch >= '0' && ch <= '9')
		return ch - '0';
	if(ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if(ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

static int
unhex(uchar *out, char *s, int n)
{
	int i, hi, lo;

	if(strlen(s) != 2*n)
		return -1;
	for(i = 0; i < n; i++){
		if((hi = hexval(s[2*i])) < 0 || (lo = hexval(s[2*i+1])) < 0)
			return -1;
		out[i] = (hi<<4) | lo;
	}
	return 0;
}

/*
 * A u64 on the wire is decimal digits and nothing else: strtoull takes
 * a sign, a 0x prefix and leading space, none of which §0's grammar
 * defines, so the string is checked before it is converted rather than
 * after.
 */
static int
u64val(char *s, uvlong *vp)
{
	char *e;
	int i, n;

	n = strlen(s);
	if(n < 1 || n > 20)
		return -1;
	for(i = 0; i < n; i++)
		if(s[i] < '0' || s[i] > '9')
			return -1;
	*vp = strtoull(s, &e, 10);
	if(*e != 0)
		return -1;
	return 0;
}

static char*
oidval(uchar *oid, int *lenp, char *s)
{
	int n;

	n = strlen(s);
	if(!srvoidok((uchar*)s, n))
		return Ebadname;
	memmove(oid, s, n);
	*lenp = n;
	return nil;
}

/*
 * One header line and the payload behind it.  §5.5 and §5.6 make the
 * header one physical line, so it runs to the first newline and the
 * bytes after that newline are the payload — exactly `n' of them for
 * an operation that defines one, and none for an operation that does
 * not.  A header with no newline at all is accepted when nothing
 * follows it, which is what a sender writing a payload-less operation
 * with no terminator sends.
 *
 * Every refusal here is `bad ctl' but one: an oid outside §1.1's bound
 * is `bad object name', which §2.6 makes the answer to "any operation
 * naming an oid that violates §1.1" and which store.md §14(27) already
 * gives the more specific of the two on the ctl path.
 */
static char*
hdrparse(Hdr *h, int chan, void *a, long count)
{
	char line[Maxhdr+1], *f[Maxfield], *v, *e;
	uchar *p;
	Op *o;
	long hn;
	int nf, i, j;

	memset(h, 0, sizeof *h);
	h->op = -1;
	p = a;
	if(count <= 0)
		return Ebadctl;
	for(hn = 0; hn < count && hn <= Maxhdr; hn++)
		if(p[hn] == '\n')
			break;
	if(hn > Maxhdr)
		return Ebadctl;
	if(hn >= count){		/* no newline: a header and nothing else */
		hn = count;
		h->data = nil;
	}else
		h->data = p + hn + 1;
	memmove(line, p, hn);
	line[hn] = 0;
	if((nf = tokenize(line, f, nelem(f))) < 1)
		return Ebadctl;
	for(i = 0; i < nf; i++){
		if((v = strchr(f[i], '=')) == nil)
			return Ebadctl;
		*v++ = 0;
		for(j = 0; j < Nfield; j++)
			if(strcmp(srvflds[j].name, f[i]) == 0)
				break;
		if(j >= Nfield || (h->have & B(j)) != 0)
			return Ebadctl;
		h->have |= B(j);
		switch(srvflds[j].kind){
		case Kword:
			for(h->op = 0; h->op < Nop; h->op++)
				if(strcmp(srvops[h->op].name, v) == 0)
					break;
			if(h->op >= Nop || (srvops[h->op].chan & chan) == 0)
				return Ebadctl;
			break;
		case Kid:
			if((e = oidval(j == Fafter ? h->after : h->oid,
				j == Fafter ? &h->afterlen : &h->oidlen,
				v)) != nil)
				return e;
			break;
		case Ku64:
		case Ku32:
			switch(j){
			case Fepoch:	if(u64val(v, &h->epoch) < 0) return Ebadctl; break;
			case Fver:	if(u64val(v, &h->ver) < 0) return Ebadctl; break;
			case Fwepoch:	if(u64val(v, &h->wepoch) < 0) return Ebadctl; break;
			case Fpver:	if(u64val(v, &h->pver) < 0) return Ebadctl; break;
			case Fpwepoch:	if(u64val(v, &h->pwepoch) < 0) return Ebadctl; break;
			case Foff:	if(u64val(v, &h->off) < 0) return Ebadctl; break;
			case Flen:	if(u64val(v, &h->len) < 0) return Ebadctl; break;
			case Fn:
				{
					uvlong u;

					if(u64val(v, &u) < 0 || u > 0xffffffffULL)
						return Ebadctl;
					h->n = u;
				}
				break;
			}
			break;
		case Kflag:
			if(strcmp(v, "0") != 0 && strcmp(v, "1") != 0)
				return Ebadctl;
			if(j == Ffinal)
				h->final = v[0] - '0';
			else
				h->force = v[0] - '0';
			break;
		case Khex16:
			if(unhex(h->dcsum, v, Blkdlen) < 0)
				return Ebadctl;
			break;
		case Khex32:
			if(unhex(h->csum, v, Csumlen) < 0)
				return Ebadctl;
			break;
		}
	}
	if(h->op < 0 || h->op >= Nop)
		return Ebadctl;
	o = &srvops[h->op];
	if((h->have & o->req) != o->req)
		return Ebadctl;
	if((h->have & ~(o->req|o->opt|B(Fop))) != 0)
		return Ebadctl;
	/*
	 * §5.5: "a header line ... followed, where a payload is defined,
	 * by exactly `n' bytes in the same Twrite."  Anything else is a
	 * message this receiver cannot read as one operation.
	 */
	if(o->payload){
		if(h->data == nil || count - (hn+1) != (long)h->n)
			return Ebadctl;
	}else if(h->data != nil && count != hn+1)
		return Ebadctl;
	/*
	 * store.md §3.8: a wire header carrying ver=0 is refused `bad
	 * ctl' BEFORE any engine call.  The engine answers a 0 by the
	 * call reached rather than by where the version came from, so the
	 * delta calls would answer it with an internal-invariant error;
	 * refusing it here is what makes that refusal a caller bug by
	 * construction, and what puts §5.5's `bad ctl' on the wire for
	 * every operation a conforming sender cannot send.
	 */
	if((h->have & B(Fver)) != 0 && h->ver == 0)
		return Ebadctl;
	return nil;
}

/*
 * §5.5's epoch rule, which is §5.6's too: below ours is `stale epoch'
 * — what stops a fenced-era primary writing — and above ours is
 * `future epoch'.  §5.5 has the receiver fetch the map at once on the
 * second; there is no monitor client to fetch it from, so the refusal
 * is the whole of what happens here (store.md §14(21)).
 */
static char*
epochck(Srvctx *c, uvlong e)
{
	if(e < c->map->epoch)
		return Estaleepoch;
	if(e > c->map->epoch)
		return Efutureepoch;
	return nil;
}

/* §5.5's dcsum: BLAKE2s-128 over the payload bytes alone */
static int
dcsumok(Hdr *h)
{
	uchar d[Blkdlen];

	blkdigest(h->data, h->n, d);
	return memcmp(d, h->dcsum, Blkdlen) == 0;
}

static char*
oidstr(char *buf, uchar *oid, int oidlen)
{
	memmove(buf, oid, oidlen);
	buf[oidlen] = 0;
	return buf;
}

/*
 * layer-a §1.3's key order: (wepoch, ver), wepoch first.  Answers -1,
 * 0 or 1 for the first key against the second.
 */
static int
keycmp(uvlong we1, uvlong v1, uvlong we2, uvlong v2)
{
	if(we1 != we2)
		return we1 < we2 ? -1 : 1;
	if(v1 != v2)
		return v1 < v2 ? -1 : 1;
	return 0;
}

/*
 * The record this operation is arbitrated against.  Answers 1 for a
 * record, 0 for an id this store holds nothing for, and -1 with the
 * engine's own error for anything else.
 */
static int
recof(Srvctx *c, Hdr *h, Objinfo *oi, char *buf, int nbuf, char **err)
{
	char e[ERRMAX];

	*err = nil;
	if(objstat(c->store, h->oid, h->oidlen, oi) >= 0)
		return 1;
	rerrstr(e, sizeof e);
	if(srv26(e) == Enoobj)
		return 0;
	*err = srverrs(buf, nbuf, e);
	return -1;
}

/*
 * §5.3's predecessor rule, which §5.5 gives the two DELTA operations:
 * apply iff the local committed key equals (pwepoch, pver) exactly,
 * else `out of sequence'.  An id this receiver holds no record of has
 * no key at all, so it cannot equal the sender's predecessor and takes
 * the same refusal — the sender re-reads op=meta and pushes an op=full
 * instead, which is the operation for a receiver that holds nothing.
 */
static char*
seqok(Objinfo *oi, int have, Hdr *h)
{
	if(!have || oi->ver != h->pver || oi->wepoch != h->pwepoch)
		return Eoutofseq;
	return nil;
}

static void
replok(Req *r)
{
	srvqexit(r);
	r->ofcall.count = r->ifcall.count;
	srvqdone(r, nil);
}

/*
 * op=write, a delta op.  The bytes are the Req's throughout: objwrite
 * commits from them, and nothing of this operation is staged on the
 * fid, so there is no window for step 7 to discard (store.md §14(10)'s
 * shape, reached for the same reason a client create has no stage).
 *
 * A count of 0 is not an extend (§2.4) and commits no record — it
 * adopts no key, so a sender that bumped `ver' for one would leave
 * this receiver a key behind with nothing to catch up on (store.md
 * §3.8).  The check of the named csum still runs, against the csum the
 * object already carries, and objwritecsum is what makes it.
 */
static void
replwriteq(Req *r, Srvctx *c, Hdr *h)
{
	char buf[ERRMAX], *e;
	Objinfo oi;
	uvlong max;
	int have;

	max = c->sb.objmax;
	if(h->off > max || (uvlong)h->n > max - h->off){
		srvqdone(r, Etoobig);
		return;
	}
	if((have = recof(c, h, &oi, buf, sizeof buf, &e)) < 0){
		srvqdone(r, e);
		return;
	}
	if((e = seqok(&oi, have, h)) != nil){
		srvqdone(r, e);
		return;
	}
	if(objwritecsum(c->store, h->oid, h->oidlen, h->data, h->n, h->off,
		h->ver, h->wepoch, h->csum, nil, 0) < 0){
		srvqexit(r);
		srvrerror(r);
		return;
	}
	replok(r);
}

/* op=trunc, the other delta op */
static void
repltruncq(Req *r, Srvctx *c, Hdr *h)
{
	char buf[ERRMAX], *e;
	Objinfo oi;
	int have;

	if(h->len > c->sb.objmax){
		srvqdone(r, Etoobig);
		return;
	}
	if((have = recof(c, h, &oi, buf, sizeof buf, &e)) < 0){
		srvqdone(r, e);
		return;
	}
	if((e = seqok(&oi, have, h)) != nil){
		srvqdone(r, e);
		return;
	}
	if(objtrunccsum(c->store, h->oid, h->oidlen, h->len, h->ver, h->wepoch,
		h->csum, nil, 0) < 0){
		srvqexit(r);
		srvrerror(r);
		return;
	}
	replok(r);
}

/*
 * op=delete, self-contained, and the one operation whose arbitration
 * is split three ways (store.md §3.8):
 *
 *	over an absent id or an existing TOMBSTONE it is objadopt's —
 *		the call makes §5.5's comparison itself, under the hold
 *		that read the record, because the key arrived from
 *		elsewhere and a caller could make it only with a second
 *		read an op=delete can overtake;
 *	over a LIVE copy it is the caller's, because objremove applies
 *		the key it is given and compares nothing, and the
 *		adoption refuses a live copy outright.  It is made here,
 *		under the object's queue, like every delta op's.
 *
 * D14's third case rides on the second: a live copy whose `corrupt'
 * flag is set contributes no key (§1.3, §5.5), so there is nothing
 * here for it to defend and the delete applies at whatever key it
 * carries.
 */
static void
repldeleteq(Req *r, Srvctx *c, Hdr *h)
{
	char buf[ERRMAX], *e;
	Objinfo oi;
	int have;

	if((have = recof(c, h, &oi, buf, sizeof buf, &e)) < 0){
		srvqdone(r, e);
		return;
	}
	if(!have || oi.state != Slive){
		if(objadoptcsum(c->store, h->oid, h->oidlen, h->ver, h->wepoch,
			h->csum, nil, 0) < 0){
			srvqexit(r);
			srvrerror(r);
			return;
		}
		replok(r);
		return;
	}
	if(!oi.corrupt && keycmp(h->wepoch, h->ver, oi.wepoch, oi.ver) <= 0){
		srvqdone(r, Estalever);
		return;
	}
	if(objremovecsum(c->store, h->oid, h->oidlen, h->ver, h->wepoch,
		h->csum, nil, 0) < 0){
		srvqexit(r);
		srvrerror(r);
		return;
	}
	replok(r);
}

/*
 * op=create.  store.md §3.6 makes the replicated create this path's
 * zero-length stage: it is self-contained and arbitrated on
 * (wepoch, ver) exactly as op=full is, and a created object carries no
 * content, so the receiver's form of it is a stage of zero length
 * whose final=1 follows no chunk.  objcreate is NOT that path — it is
 * §5.4's CLIENT create, which answers `object exists' for a live id
 * and chooses the new version itself over a tombstone, where a
 * replicated create must adopt the sender's version verbatim and
 * defend an existing key with `stale version'.
 *
 * The stage is made and consumed inside this one request, so it never
 * reaches the fid's slot and there is no window for step 7 to discard.
 */
static void
replcreateq(Req *r, Srvctx *c, Hdr *h)
{
	Stage *g;

	if((g = stageopen(c->store, h->oid, h->oidlen, 0, 0)) == nil){
		srvqexit(r);
		srvrerror(r);
		return;
	}
	if(stagefinalcsum(g, h->ver, h->wepoch, h->csum, nil, 0) < 0){
		srvqexit(r);
		srvrerror(r);
		return;
	}
	replok(r);
}

/*
 * op=full: the whole-object resync, staged across as many Twrites as
 * it takes and committed only at final=1, so a resync is never half
 * applied (§5.5).  The stage is the FID's and outlives the request
 * that made it — the one stage in this design with that lifetime
 * (store.md §3.6) — and obj.c owns its slot, its list and its three
 * hooks; what is here is the channel's half.
 *
 * `busy' is set across the engine call and cleared at the look that
 * follows it, because a chunk still in flight is not an absence of
 * arrivals: a maximal chunk can take longer than `stagems' to land,
 * and the reservations it is filling are the ones the sweep would
 * otherwise return to the allocator while the write is still indexing
 * them (§3.6).
 *
 * A chunk whose stagewrite FAILED leaves the stage where it is.  §3.6
 * names the triggers for a stage whose final=1 has not been attempted
 * — clunk, flush, idle, restart — "and for no other", so a refused
 * chunk ends the transfer for the sender and the fid's own clunk is
 * what releases what it holds.
 */
static void
replfullq(Req *r, Srvctx *c, Sfid *f, Hdr *h)
{
	char buf[ERRMAX], *e;
	Sstage *s;
	Stage *g;
	uvlong max;
	int rc;

	max = c->sb.objmax;
	if(h->len > max || h->off > max || (uvlong)h->n > max - h->off){
		srvqdone(r, Etoobig);
		return;
	}
	/*
	 * A chunk outside its stage's declared length is `bad ctl'
	 * (store.md §3.7): `len' is the final object length and every
	 * chunk of the transfer carries it, so bytes above it are a
	 * header a conforming sender cannot send.
	 */
	if(h->off > h->len || (uvlong)h->n > h->len - h->off){
		srvqdone(r, Ebadctl);
		return;
	}
	if((g = srvstagemore(c, f, h->oid, h->oidlen, h->len, h->force,
		h->off, h->n, &s, &e)) == nil){
		if(e != nil){
			srvqdone(r, e);
			return;
		}
		if((g = srvstagefull(c, f, h->oid, h->oidlen, h->len, h->force,
			h->ver, h->wepoch, h->off, h->n, &s, buf, sizeof buf,
			&e)) == nil){
			srvqdone(r, e);
			return;
		}
	}
	rc = 0;
	buf[0] = 0;
	if(h->n > 0 && (rc = stagewrite(g, h->data, h->n, h->off)) < 0)
		rerrstr(buf, sizeof buf);
	if(!srvstagelive(c, f, s)){
		srvqdone(r, Estageexp);
		return;
	}
	if(rc < 0){
		srvqexit(r);
		srvqdone(r, buf);
		return;
	}
	if(!h->final){
		replok(r);
		return;
	}
	/*
	 * final=1 consumes the handle on every outcome (§3.6), so the fid
	 * forgets it here, before the comparison stagefinal makes is
	 * known: a Tclunk behind a refused final=1 would otherwise
	 * discard a stage that has already been discarded.
	 */
	if((g = srvstagefinal(c, f, s)) == nil){
		srvqdone(r, Estageexp);
		return;
	}
	if(stagefinalcsum(g, h->ver, h->wepoch, h->csum, nil, 0) < 0){
		srvqexit(r);
		srvrerror(r);
		return;
	}
	replok(r);
}

static void
replq(Req *r)
{
	char *e;
	Srvctx *c;
	Sfid *f;
	Hdr h;

	if(srvqcheck(r)){
		srvqdone(r, nil);
		return;
	}
	c = r->srv->aux;
	f = r->fid->aux;
	srvstagesweep(c);
	if((e = hdrparse(&h, Crepl, r->ifcall.data, r->ifcall.count)) != nil){
		srvqdone(r, e);
		return;
	}
	if((h.have & B(Fdcsum)) != 0 && !dcsumok(&h)){
		srvqdone(r, Ecsum);
		return;
	}
	switch(h.op){
	case Ocreate:
		replcreateq(r, c, &h);
		break;
	case Owrite:
		replwriteq(r, c, &h);
		break;
	case Otrunc:
		repltruncq(r, c, &h);
		break;
	case Odelete:
		repldeleteq(r, c, &h);
		break;
	case Ofull:
		replfullq(r, c, f, &h);
		break;
	default:
		srvqdone(r, Ebadctl);
		break;
	}
}

/*
 * /repl's write cell.  The parse, the epoch and §3.8's ver=0 are
 * answered on the service loop, before the object's queue is asked
 * for; everything that reaches the engine runs on that queue.
 *
 * §5.5's "at most one outstanding operation per object per /repl fid"
 * is the SENDER's rule and is not refused here: op=full chunks for one
 * object MAY be pipelined, so a receiver that refused a second
 * outstanding operation would refuse what §5.5 licenses in the same
 * breath.  What the receiver owes is that two operations naming one
 * object are ordered, and the pool is that — both hash to the same
 * queue and run one after the other (§5.4.1).
 */
void
srvreplwrite(Req *r)
{
	char *e;
	Srvctx *c;
	Hdr h;

	c = r->srv->aux;
	if((e = hdrparse(&h, Crepl, r->ifcall.data, r->ifcall.count)) != nil){
		srvqdone(r, e);
		return;
	}
	if((e = epochck(c, h.epoch)) != nil){
		srvqdone(r, e);
		return;
	}
	srvqpush(c, h.oid, h.oidlen, r, replq);
}

/*
 * /repl's read cell.  §5.5 defines no read: the channel is one-way,
 * and the reply to an operation is the Rwrite that carries it.  A
 * Tread is therefore end of data rather than `not built' — the file is
 * built, and a row with no read cell would answer the other (store.md
 * §14(41)).
 */
void
srvreplread(Req *r)
{
	r->ofcall.count = 0;
	respond(r, nil);
}

/*
 * §5.6's three hooks on the per-fid state.  None of them reaches the
 * engine: what a /rpc fid holds is a prepared response and a flag, so
 * the close hook has nothing an engine call could release and the free
 * hook is the only one that gives memory back.
 *
 * The flush hook is layer-a §5.4.1 step 7 on this fid: the request it
 * runs for is unwinding flushed, so the claim that request made on the
 * fid goes with it and so does anything it had prepared.  It runs
 * under the fid's state lock, which is the lock the handler installs
 * its response under, so the two cannot cross: a handler whose claim
 * is gone when it comes to install finds it gone and throws the
 * response away.
 */
static void
rpcflush(Sfid *f, Req *r)
{
	Rpc *p;

	USED(r);
	if((p = f->aux) == nil)
		return;
	p->busy = 0;
	textfree(p->t);
	p->t = nil;
	p->delivered = 0;
}

static void
rpcclose(void *a)
{
	Rpc *p;

	if((p = a) == nil)
		return;
	textfree(p->t);
	p->t = nil;
}

static void
rpcfree(void *a)
{
	Rpc *p;

	if((p = a) == nil)
		return;
	textfree(p->t);
	free(p);
}

/*
 * Claim the fid for one request.  §5.6 allows at most one outstanding
 * request per fid and makes a second Twrite before the response is
 * read `bad ctl'; it also destroys a buffered response at the next
 * Twrite.  Both are kept: a Twrite that arrives while a request is in
 * flight, or while a response nobody has read is buffered, is refused,
 * and a Twrite that arrives after the response was delivered destroys
 * it and takes the fid.  store.md §14(42) records the reading, and
 * why both of §5.6's sentences are live under it.
 */
static char*
rpcclaim(Srvctx *c, Sfid *f)
{
	char *e;
	Rpc *p;

	e = nil;
	qlock(&f->lk);
	if((p = f->aux) != nil && f->auxfree != rpcfree){
		qunlock(&f->lk);
		return Efidstate;	/* the T1 fid-state point (srv.h) */
	}
	if(p == nil){
		if((p = mallocz(sizeof *p, 1)) == nil){
			qunlock(&f->lk);
			return Eoom;
		}
		p->ctx = c;
		f->aux = p;
		f->auxflush = rpcflush;
		f->auxclose = rpcclose;
		f->auxfree = rpcfree;
		f->auxclosed = 0;
	}
	if(p->busy || (p->t != nil && !p->delivered))
		e = Ebadctl;
	else{
		textfree(p->t);
		p->t = nil;
		p->delivered = 0;
		p->busy = 1;
	}
	qunlock(&f->lk);
	return e;
}

/* the claim goes back on a refusal; step 7 gives it back on a flush */
static void
rpcdrop(Sfid *f)
{
	Rpc *p;

	qlock(&f->lk);
	if((p = f->aux) != nil && f->auxfree == rpcfree)
		p->busy = 0;
	qunlock(&f->lk);
}

/*
 * Install the prepared response, unless step 7 has taken the claim
 * while this handler was preparing it — in which case the request is
 * unwinding flushed and the response is nobody's.
 */
static void
rpcinstall(Sfid *f, Text *t)
{
	Rpc *p;

	qlock(&f->lk);
	p = f->aux;
	if(p == nil || f->auxfree != rpcfree || !p->busy){
		qunlock(&f->lk);
		textfree(t);
		return;
	}
	p->busy = 0;
	p->t = t;
	p->delivered = 0;
	qunlock(&f->lk);
}

/*
 * §5.6's op=meta, the primitive behind currency checks, tombstone
 * confirmation and arbitration.  Three forms: the key, `absent=1' for
 * an instance that holds no copy, and D14's `corrupt=1', which an
 * instance whose copy fails local verification MUST answer and which
 * carries the key while contributing none.
 *
 * `cur=' is the epoch of this instance's most recent COMPLETED
 * currency check and reads 0 for every object, because no check can be
 * made without a peer client (store.md §14(35)).
 */
static char*
rpcmeta(Srvctx *c, Hdr *h, Text *t, char *buf, int nbuf)
{
	char name[Oidmax+1], csum[Csumhexlen], *e;
	Objinfo oi;
	int have;

	if((have = recof(c, h, &oi, buf, nbuf, &e)) < 0)
		return e;
	oidstr(name, h->oid, h->oidlen);
	if(!have){
		textprint(t, "meta oid=%s absent=1\n", name);
		return nil;
	}
	hexof(csum, oi.csum, Csumlen);
	textprint(t, "meta oid=%s ver=%llud wepoch=%llud csum=%s len=%llud"
		" state=%s cur=0%s\n", name, oi.ver, oi.wepoch, csum, oi.len,
		oi.state == Stomb ? "tomb" : "live",
		oi.corrupt ? " corrupt=1" : "");
	return nil;
}

/*
 * §5.6's op=get.  It carries the EXPECTED key: if the object's
 * committed key is not exactly (wepoch, ver) the answer is `stale
 * version' and the puller re-reads op=meta, which is how a multi-chunk
 * pull stays consistent without holding a lock across the transfer.
 *
 * `n' MUST fit the negotiated msize.  The whole response — the line
 * and the bytes behind it — is delivered by one Tread of at most
 * msize−IOHDRSZ, so a request for more than that is one this receiver
 * cannot answer and is `bad ctl'.  The `n=' of the response is what
 * was read, which is short at the end of the object (§4's clamp).
 */
static char*
rpcget(Srvctx *c, Hdr *h, Text *t, char *buf, int nbuf)
{
	char name[Oidmax+1], dcsum[2*Blkdlen+1], *e;
	uchar d[Blkdlen], *a;
	Objinfo oi;
	long n;
	int have;

	oidstr(name, h->oid, h->oidlen);
	if((uvlong)h->n + Gethdr > (uvlong)c->srv.msize - IOHDRSZ)
		return Ebadctl;
	if((have = recof(c, h, &oi, buf, nbuf, &e)) < 0)
		return e;
	if(!have)
		return Enoobj;
	if(oi.state != Slive)
		return Edeleted;
	if(oi.ver != h->ver || oi.wepoch != h->wepoch)
		return Estalever;
	if((a = malloc(h->n + 1)) == nil)
		return Eoom;
	if((n = objread(c->store, h->oid, h->oidlen, a, h->n, h->off)) < 0){
		free(a);
		return srverr(buf, nbuf);
	}
	blkdigest(a, n, d);
	textprint(t, "get oid=%s ver=%llud wepoch=%llud off=%llud n=%lud"
		" dcsum=%s\n", name, oi.ver, oi.wepoch, h->off, n,
		hexof(dcsum, d, Blkdlen));
	if(textwrite(t, a, n) < 0){
		free(a);
		return Eoom;
	}
	free(a);
	return nil;
}

/*
 * §5.6's op=list: the instance's whole inventory, live and tomb, paged
 * in oid byte order and resuming after `after='.  The engine answers
 * the page (objlist) and the RENDERING is the server's, which is where
 * §5.6's two rules that the engine cannot hold are applied (store.md
 * §3.8): the requested `n=' is a maximum clamped so the whole response
 * fits the negotiated msize less IOHDRSZ, `lines=' counts advert lines
 * rather than bytes, and `more=1' answers a clamp as well as inventory
 * that follows.
 *
 * The lines are §7.2's advert grammar, which is what /advert and
 * /tombs render (enum.c) — §5.6 calls them advert lines and means that
 * one.
 */
static char*
rpclist(Srvctx *c, Hdr *h, Text *t, char *buf, int nbuf)
{
	char name[Oidmax+1], csum[Csumhexlen];
	Objent *ent;
	Text *lines;
	long budget, was;
	int i, k, n, more, clamped;

	k = Listdflt;
	if((h->have & B(Fn)) != 0)
		k = h->n < Listmax ? h->n : Listmax;
	budget = (long)(c->srv.msize - IOHDRSZ) - Listhdr;
	if(budget < 0)
		budget = 0;
	if(k <= 0){
		/*
		 * `n=0' is the caller clamping itself to nothing, and a page
		 * of no lines cannot say whether the inventory is over; §5.6
		 * makes `more=1' the answer to a clamp, and this is one.
		 */
		textprint(t, "list lines=0 more=1\n");
		return nil;
	}
	if((ent = malloc(k*sizeof *ent)) == nil)
		return Eoom;
	more = 0;
	n = objlist(c->store, h->afterlen > 0 ? h->after : nil, h->afterlen,
		ent, k, &more);
	if(n < 0){
		free(ent);
		return srverr(buf, nbuf);
	}
	if((lines = textnew()) == nil){
		free(ent);
		return Eoom;
	}
	clamped = 0;
	for(i = 0; i < n; i++){
		was = lines->n;
		oidstr(name, ent[i].oid, ent[i].oidlen);
		hexof(csum, ent[i].oi.csum, Csumlen);
		textprint(lines, "oid=%s ver=%llud wepoch=%llud csum=%s"
			" len=%llud state=%s\n", name, ent[i].oi.ver,
			ent[i].oi.wepoch, csum, ent[i].oi.len,
			ent[i].oi.state == Stomb ? "tomb" : "live");
		if(lines->err){
			textfree(lines);
			free(ent);
			return Eoom;
		}
		if(lines->n > budget){
			/*
			 * The whole response is delivered by one Tread, so
			 * the line that crossed the budget is not sent: it is
			 * dropped here and `more=1' says so, and the next page
			 * resumes after the last oid this one did send.
			 */
			lines->n = was;
			clamped = 1;
			break;
		}
	}
	free(ent);
	textprint(t, "list lines=%d more=%d\n", i, clamped || more ? 1 : 0);
	if(textwrite(t, lines->p, lines->n) < 0){
		textfree(lines);
		return Eoom;
	}
	textfree(lines);
	return nil;
}

/*
 * §5.6's op=drop, the wire form of §7.4's drop guard: the serving
 * primary tells a stray holder to delete its copy with no tombstone.
 * The receiver MUST re-check, against its OWN current map, that it is
 * not in P(oid), and MUST answer `still placed' if it is — the same
 * check §2.5's `drop' verb makes, against the same static map, and for
 * the same reason: the engine holds no map (store.md §3.8).
 */
static char*
rpcdropop(Srvctx *c, Hdr *h, Text *t, char *buf, int nbuf)
{
	char name[Oidmax+1];
	Cinst *pl[Maxplace];
	int i, n;

	oidstr(name, h->oid, h->oidlen);
	if((n = mapplace(c->map, name, pl, nelem(pl))) < 0)
		return srverr(buf, nbuf);
	if(n > nelem(pl))
		n = nelem(pl);
	for(i = 0; i < n; i++)
		if(pl[i] == c->self)
			return Estillplaced;
	if(objdrop(c->store, h->oid, h->oidlen) < 0)
		return srverr(buf, nbuf);
	textprint(t, "ok op=drop oid=%s\n", name);
	return nil;
}

/*
 * §5.6's op=verify: re-hash now and compare against the stored csum
 * (§7.5), answering `ok' or `checksum mismatch'.  objverify answers
 * the mismatching blocks and, separately, whether the digest array is
 * suspect; §5.6's row makes both of them the one wire error, as §2.5's
 * `verify' verb does.
 */
static char*
rpcverify(Srvctx *c, Hdr *h, Text *t, char *buf, int nbuf)
{
	char name[Oidmax+1];
	Vfy v;
	int bad;

	if(objverify(c->store, h->oid, h->oidlen, &v) < 0)
		return srverr(buf, nbuf);
	bad = v.arraybad || v.nbad > 0;
	vfyfree(&v);
	if(bad)
		return Ecsum;
	textprint(t, "ok op=verify oid=%s\n", oidstr(name, h->oid, h->oidlen));
	return nil;
}

/*
 * §5.6's op=discard, §1.5's tombstone discard.  The receiver applies
 * §1.5's three local checks — a tombstone, at exactly the named key,
 * whose wepoch is strictly below the given epoch — and objdiscard
 * makes all three under one hold of the engine's state lock, so they
 * judge one record where a separate objstat could not.  An id this
 * store holds no record for is `no such object' (D15).
 */
static char*
rpcdiscard(Srvctx *c, Hdr *h, Text *t, char *buf, int nbuf)
{
	char name[Oidmax+1];

	if(objdiscard(c->store, h->oid, h->oidlen, h->ver, h->wepoch,
		h->epoch) < 0)
		return srverr(buf, nbuf);
	textprint(t, "ok op=discard oid=%s\n", oidstr(name, h->oid, h->oidlen));
	return nil;
}

/*
 * The whole response is prepared HERE, at Twrite time, under the
 * object's queue — which is what makes it atomic with respect to
 * concurrent object writes (§5.6).  Request errors are Rerror to the
 * Twrite and leave nothing buffered.
 */
static void
rpcq(Req *r)
{
	char buf[ERRMAX], *e;
	Srvctx *c;
	Sfid *f;
	Text *t;
	Hdr h;

	if(srvqcheck(r)){
		srvqdone(r, nil);	/* step 7 gives the claim back */
		return;
	}
	c = r->srv->aux;
	f = r->fid->aux;
	if(srvqreq(r)->oidlen > 0)
		srvstagesweep(c);
	if((e = hdrparse(&h, Crpc, r->ifcall.data, r->ifcall.count)) != nil){
		rpcdrop(f);
		srvqdone(r, e);
		return;
	}
	if((t = textnew()) == nil){
		rpcdrop(f);
		srvqdone(r, Eoom);
		return;
	}
	switch(h.op){
	case Ometa:
		e = rpcmeta(c, &h, t, buf, sizeof buf);
		break;
	case Oget:
		e = rpcget(c, &h, t, buf, sizeof buf);
		break;
	case Olist:
		e = rpclist(c, &h, t, buf, sizeof buf);
		break;
	case Odrop:
		e = rpcdropop(c, &h, t, buf, sizeof buf);
		break;
	case Overify:
		e = rpcverify(c, &h, t, buf, sizeof buf);
		break;
	case Odiscard:
		e = rpcdiscard(c, &h, t, buf, sizeof buf);
		break;
	default:
		e = Ebadctl;
		break;
	}
	if(e == nil && t->err)
		e = Eoom;
	srvqexit(r);
	if(e != nil){
		textfree(t);
		rpcdrop(f);
		srvqdone(r, e);
		return;
	}
	rpcinstall(f, t);
	r->ofcall.count = r->ifcall.count;
	srvqdone(r, nil);
}

/*
 * /rpc's open cell.  §5.6: the fid MUST be opened ORDWR, since every
 * exchange uses both directions, and an OREAD or OWRITE open MUST fail
 * `bad open mode'.  ORCLOSE is refused as everywhere else, and so is
 * OTRUNC, which would ask this channel to be emptied; both are modes
 * outside the one this row admits and take the same string.
 */
void
srvrpcopen(Req *r)
{
	if(r->ifcall.mode != ORDWR){
		respond(r, Ebadopen);
		return;
	}
	respond(r, nil);
}

/*
 * /rpc's read cell.  §5.6 ignores the offset in both directions: a
 * read always returns the buffered response from its start, and a read
 * with nothing buffered — or one after the response has been delivered
 * — returns count 0, which is an end of data and not an error.  The
 * caller MUST offer a read of at least the negotiated msize−IOHDRSZ,
 * and the response was sized to fit one.
 *
 * It answers on the service loop: what it does is a copy out of the
 * fid's own state under that state's lock, with no engine call in it.
 */
void
srvrpcread(Req *r)
{
	Srvctx *c;
	Sfid *f;
	Rpc *p;
	long n;

	c = r->srv->aux;
	USED(c);
	f = r->fid->aux;
	n = 0;
	qlock(&f->lk);
	if((p = f->aux) != nil && f->auxfree == rpcfree
	&& p->t != nil && !p->delivered){
		n = p->t->n;
		if(n > r->ifcall.count)
			n = r->ifcall.count;
		memmove(r->ofcall.data, p->t->p, n);
		p->delivered = 1;
	}
	qunlock(&f->lk);
	r->ofcall.count = n;
	respond(r, nil);
}

/*
 * /rpc's write cell.  The parse, the epoch and the claim on the fid
 * are the service loop's; everything that reaches the engine runs on a
 * queue.  op=list names no object, so it goes to the reserved queue
 * (srvqpushany) — it takes no object's ordering point and must not sit
 * on one object's queue while it walks the whole index.
 */
void
srvrpcwrite(Req *r)
{
	char *e;
	Srvctx *c;
	Sfid *f;
	Hdr h;

	c = r->srv->aux;
	f = r->fid->aux;
	if((e = hdrparse(&h, Crpc, r->ifcall.data, r->ifcall.count)) != nil){
		srvqdone(r, e);
		return;
	}
	if((e = epochck(c, h.epoch)) != nil){
		srvqdone(r, e);
		return;
	}
	if((e = rpcclaim(c, f)) != nil){
		srvqdone(r, e);
		return;
	}
	if(h.op == Olist)
		srvqpushany(c, r, rpcq);
	else
		srvqpush(c, h.oid, h.oidlen, r, rpcq);
}
