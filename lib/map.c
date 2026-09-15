#include <u.h>
#include <libc.h>
#include <libsec.h>
#include "shoal.h"

/*
 * The cluster map: docs/design/layer-a.md §3's format, §4's
 * placement, §5.2's witness set, §6.3's adoption decision and §6.4's
 * fence state.  Pure functions over caller-owned memory; the map
 * arrives as bytes and this file answers questions about it.  Nothing
 * here dials, reads a clock or keeps state of its own.
 *
 * Error strings.  A text that fails validation answers §2.6's
 * `bad map' with a detail, which is the only §2.6 prefix produced
 * here (store.md §3.7).  The refusals of §6.3 are not wire errors —
 * they are /status flags — so they are answered as codes.
 */

enum
{
	Kunknown	= 0,	/* §3.1: a record kind to ignore whole */
	Kmap,
	Knode,
	Kinst,
	Kstale,
};

/* header attributes, for the seen-once bookkeeping */
enum
{
	Hepoch		= 1<<0,
	Hmonid		= 1<<1,
	Hobjmax		= 1<<2,
	Hblksz		= 1<<3,
	Hreplicas	= 1<<4,
	Hcsumalg	= 1<<5,
	Hplacehash	= 1<<6,
	Hpollms		= 1<<7,
	Hleasems	= 1<<8,
	Hreplms		= 1<<9,
	Hdeadms		= 1<<10,
	Houtmins	= 1<<11,
	Htombdays	= 1<<12,
	Hmincopies	= 1<<13,
	Hretain		= 1<<14,
	Hplacerule	= 1<<15,

	Hneed = Hepoch|Hmonid|Hobjmax|Hblksz|Hreplicas|Hcsumalg|
		Hplacehash|Hpollms|Hleasems|Hreplms|Hdeadms|Houtmins|
		Htombdays|Hmincopies|Hretain,
};

/* instance attributes */
enum
{
	Ionnode		= 1<<0,
	Iaddr		= 1<<1,
	Iuuid		= 1<<2,
	Iclass		= 1<<3,
	Izone		= 1<<4,
	Iweight		= 1<<5,
	Istatus		= 1<<6,
	Iup		= 1<<7,
	Isince		= 1<<8,
	Ifenced		= 1<<9,

	Ineed = Ionnode|Iaddr|Iuuid|Istatus|Iup,
};

/* stale attributes */
enum
{
	Treporter	= 1<<0,
	Tsince		= 1<<1,

	Tneed = Treporter,
};

typedef struct Parse Parse;
struct Parse
{
	Cmap	*m;
	char	*buf;		/* the mutable copy being tokenised */
	int	kind;		/* the record being filled */
	int	started;	/* a record has begun */
	int	seen;		/* its attributes, for duplicate detection */
	int	nmap;		/* `map' records seen */
	Cinst	*inst;		/* the instance record being filled */
	Cstale	*stale;
};

static int
badmap(char *fmt, ...)
{
	char buf[ERRMAX];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf + sizeof buf - 1, fmt, arg);
	va_end(arg);
	werrstr("bad map: %s", buf);
	return -1;
}

/*
 * §4.3's tie-break comparison: unsigned byte-wise, and if one id is a
 * prefix of the other the longer wins — which is what a comparison
 * against the shorter id's NUL gives, since no id byte is 0.
 */
static int
idcmp(char *a, char *b)
{
	uchar *p, *q;

	p = (uchar*)a;
	q = (uchar*)b;
	while(*p != 0 && *p == *q){
		p++;
		q++;
	}
	return (int)*p - (int)*q;
}

static int
isnodech(int c)
{
	return c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' ||
		c >= '0' && c <= '9' || c == '-' || c == '_';
}

/* §3.3: 1*63(ALPHA / DIGIT / "-" / "_"), and so no `.' */
static int
nodeok(char *s)
{
	int n;

	for(n = 0; s[n] != '\0'; n++)
		if(!isnodech(s[n]))
			return 0;
	return n >= 1 && n <= Nodelen;
}

/* 32 lower-case hex characters, §0's rendering of a hexadecimal field */
static int
hexok(char *s, int len)
{
	int i, c;

	for(i = 0; i < len; i++){
		c = s[i];
		if(!(c >= '0' && c <= '9' || c >= 'a' && c <= 'f'))
			return 0;
	}
	return s[len] == '\0';
}

/* unsigned decimal, whole token, no sign, no overflow */
static int
decode(char *s, uvlong *v)
{
	uvlong x;
	int c;

	if(*s == '\0')
		return -1;
	x = 0;
	for(; (c = *s) != '\0'; s++){
		if(c < '0' || c > '9')
			return -1;
		if(x > (~(uvlong)0 - (c - '0')) / 10)
			return -1;
		x = x*10 + (c - '0');
	}
	*v = x;
	return 0;
}

static int
ispow2(uvlong v)
{
	return v != 0 && (v & (v - 1)) == 0;
}

/*
 * An iid is <node>"."<index> (§3.3).  The index is decimal and
 * carries no leading zero, so that one instance has exactly one iid:
 * placement hashes the iid's bytes (§4.2), and `n2.01' and `n2.1'
 * would otherwise be one instance with two placement shares.
 */
static int
iidsplit(char *iid, char *node, ulong *idx)
{
	char *dot;
	uvlong v;
	long n;

	if((dot = strchr(iid, '.')) == nil)
		return -1;
	n = dot - iid;
	if(n < 1 || n > Nodelen)
		return -1;
	memmove(node, iid, n);
	node[n] = '\0';
	if(!nodeok(node))
		return -1;
	dot++;
	if(*dot == '\0' || strlen(dot) > Idxdigits)
		return -1;
	if(dot[0] == '0' && dot[1] != '\0')
		return -1;
	if(decode(dot, &v) < 0 || v > (uvlong)0xffffffff)
		return -1;
	*idx = (ulong)v;
	return 0;
}

static int
iidok(char *iid)
{
	char node[Nodelen+1];
	ulong idx;

	return iidsplit(iid, node, &idx) == 0;
}

static int
setstr(char *dst, int max, char *v, char *what)
{
	if(*v == '\0')
		return badmap("empty %s", what);
	if(strlen(v) > (uint)max)
		return badmap("%s over %d bytes", what, max);
	strcpy(dst, v);
	return 0;
}

static int
setu64(uvlong *dst, char *v, char *what)
{
	if(decode(v, dst) < 0)
		return badmap("%s %s is not a u64", what, v);
	return 0;
}

static int
setu32(ulong *dst, char *v, char *what)
{
	uvlong x;

	if(decode(v, &x) < 0 || x > (uvlong)0xffffffff)
		return badmap("%s %s is not a u32", what, v);
	*dst = (ulong)x;
	return 0;
}

static int
once(Parse *p, int bit, char *what)
{
	if(p->seen & bit)
		return badmap("%s given twice", what);
	p->seen |= bit;
	return 0;
}

char*
statusname(int s)
{
	switch(s){
	case Snew:	return "new";
	case Sin:	return "in";
	case Sout:	return "out";
	case Sdead:	return "dead";
	}
	return "?";
}

char*
upname(int u)
{
	switch(u){
	case Uyes:	return "yes";
	case Uheal:	return "heal";
	case Uno:	return "no";
	}
	return "?";
}

static int
statusval(char *v)
{
	if(strcmp(v, "new") == 0)	return Snew;
	if(strcmp(v, "in") == 0)	return Sin;
	if(strcmp(v, "out") == 0)	return Sout;
	if(strcmp(v, "dead") == 0)	return Sdead;
	return -1;
}

static int
upval(char *v)
{
	if(strcmp(v, "yes") == 0)	return Uyes;
	if(strcmp(v, "heal") == 0)	return Uheal;
	if(strcmp(v, "no") == 0)	return Uno;
	return -1;
}

static int
yesno(char *v)
{
	if(strcmp(v, "yes") == 0)	return 1;
	if(strcmp(v, "no") == 0)	return 0;
	return -1;
}

/* the header attributes of §3.2 */
static int
mapattr(Parse *p, char *a, char *v)
{
	Cmap *m;

	m = p->m;
	if(strcmp(a, "epoch") == 0){
		if(once(p, Hepoch, a) < 0)
			return -1;
		return setu64(&m->epoch, v, a);
	}
	if(strcmp(a, "monid") == 0){
		if(once(p, Hmonid, a) < 0)
			return -1;
		if(!hexok(v, Monidlen))
			return badmap("monid is not %d lower-case hex", Monidlen);
		strcpy(m->monid, v);
		return 0;
	}
	if(strcmp(a, "objmax") == 0){
		if(once(p, Hobjmax, a) < 0)
			return -1;
		if(setu64(&m->objmax, v, a) < 0)
			return -1;
		if(!ispow2(m->objmax) || m->objmax < (1<<20))
			return badmap("objmax %llud is not a power of two "
				"at least 2^20", m->objmax);
		return 0;
	}
	if(strcmp(a, "blksz") == 0){
		if(once(p, Hblksz, a) < 0)
			return -1;
		if(setu32(&m->blksz, v, a) < 0)
			return -1;
		if(!ispow2(m->blksz))
			return badmap("blksz %lud is not a power of two",
				m->blksz);
		return 0;
	}
	if(strcmp(a, "replicas") == 0){
		if(once(p, Hreplicas, a) < 0)
			return -1;
		if(setu32(&m->replicas, v, a) < 0)
			return -1;
		if(m->replicas < 1)
			return badmap("replicas 0, under the 1 §3.2 requires");
		if(m->replicas > Maxplace)
			return badmap("replicas %lud over the %d this build "
				"places", m->replicas, Maxplace);
		return 0;
	}
	if(strcmp(a, "csumalg") == 0){
		if(once(p, Hcsumalg, a) < 0)
			return -1;
		if(strcmp(v, "blake2s256") != 0)
			return badmap("csumalg %s: this build has blake2s256",
				v);
		strcpy(m->csumalg, v);
		return 0;
	}
	if(strcmp(a, "placehash") == 0){
		if(once(p, Hplacehash, a) < 0)
			return -1;
		if(strcmp(v, "blake2s256-64") != 0)
			return badmap("placehash %s: this build has "
				"blake2s256-64", v);
		strcpy(m->placehash, v);
		return 0;
	}
	if(strcmp(a, "pollms") == 0){
		if(once(p, Hpollms, a) < 0)
			return -1;
		if(setu32(&m->pollms, v, a) < 0)
			return -1;
		if(m->pollms < 1)
			return badmap("pollms 0");
		return 0;
	}
	if(strcmp(a, "leasems") == 0){
		if(once(p, Hleasems, a) < 0)
			return -1;
		return setu32(&m->leasems, v, a);
	}
	if(strcmp(a, "replms") == 0){
		if(once(p, Hreplms, a) < 0)
			return -1;
		return setu32(&m->replms, v, a);
	}
	if(strcmp(a, "deadms") == 0){
		if(once(p, Hdeadms, a) < 0)
			return -1;
		return setu32(&m->deadms, v, a);
	}
	if(strcmp(a, "outmins") == 0){
		if(once(p, Houtmins, a) < 0)
			return -1;
		return setu32(&m->outmins, v, a);
	}
	if(strcmp(a, "tombdays") == 0){
		if(once(p, Htombdays, a) < 0)
			return -1;
		return setu32(&m->tombdays, v, a);
	}
	if(strcmp(a, "mincopies") == 0){
		if(once(p, Hmincopies, a) < 0)
			return -1;
		if(setu32(&m->mincopies, v, a) < 0)
			return -1;
		if(m->mincopies < 1)
			return badmap("mincopies 0");
		return 0;
	}
	if(strcmp(a, "retain") == 0){
		if(once(p, Hretain, a) < 0)
			return -1;
		if(setu32(&m->retain, v, a) < 0)
			return -1;
		if(m->retain < Monretainmin)
			return badmap("retain %lud, under the %d §8.2 keeps "
				"for §5.2 clause 2", m->retain, Monretainmin);
		return 0;
	}
	if(strcmp(a, "placerule") == 0){
		if(once(p, Hplacerule, a) < 0)
			return -1;
		if(strcmp(v, "nodes") != 0)
			return badmap("placerule %s: v1 has nodes (§4.5)", v);
		strcpy(m->placerule, v);
		return 0;
	}
	return 0;		/* §3.1: ignore what we do not know */
}

static int
instattr(Parse *p, char *a, char *v)
{
	Cinst *i;
	uvlong x;
	int n;

	i = p->inst;
	if(strcmp(a, "onnode") == 0){
		if(once(p, Ionnode, a) < 0)
			return -1;
		if(strcmp(v, i->node) != 0)
			return badmap("%s: onnode=%s is not the iid's node",
				i->iid, v);
		return 0;
	}
	if(strcmp(a, "addr") == 0){
		if(once(p, Iaddr, a) < 0)
			return -1;
		return setstr(i->addr, Addrlen, v, "addr");
	}
	if(strcmp(a, "uuid") == 0){
		if(once(p, Iuuid, a) < 0)
			return -1;
		if(!hexok(v, Uuidlen))
			return badmap("%s: uuid is not %d lower-case hex",
				i->iid, Uuidlen);
		strcpy(i->uuid, v);
		return 0;
	}
	if(strcmp(a, "class") == 0){
		if(once(p, Iclass, a) < 0)
			return -1;
		return setstr(i->class, Classlen, v, "class");
	}
	if(strcmp(a, "zone") == 0){
		if(once(p, Izone, a) < 0)
			return -1;
		if(!nodeok(v))
			return badmap("%s: zone %s is not a node name",
				i->iid, v);
		strcpy(i->zone, v);
		return 0;
	}
	if(strcmp(a, "weight") == 0){
		if(once(p, Iweight, a) < 0)
			return -1;
		if(setu32(&i->weight, v, a) < 0)
			return -1;
		if(i->weight != 100)
			return badmap("%s: weight %lud, and v1 takes only 100 "
				"(§4.4)", i->iid, i->weight);
		return 0;
	}
	if(strcmp(a, "status") == 0){
		if(once(p, Istatus, a) < 0)
			return -1;
		if((n = statusval(v)) < 0)
			return badmap("%s: status %s", i->iid, v);
		i->status = n;
		return 0;
	}
	if(strcmp(a, "up") == 0){
		if(once(p, Iup, a) < 0)
			return -1;
		if((n = upval(v)) < 0)
			return badmap("%s: up %s", i->iid, v);
		i->up = n;
		return 0;
	}
	if(strcmp(a, "since") == 0){
		if(once(p, Isince, a) < 0)
			return -1;
		if(setu64(&x, v, a) < 0)
			return -1;
		i->since = x;
		return 0;
	}
	if(strcmp(a, "fenced") == 0){
		if(once(p, Ifenced, a) < 0)
			return -1;
		if((n = yesno(v)) < 0)
			return badmap("%s: fenced %s", i->iid, v);
		i->fenced = n;
		return 0;
	}
	return 0;
}

static int
staleattr(Parse *p, char *a, char *v)
{
	Cstale *s;

	s = p->stale;
	if(strcmp(a, "reporter") == 0){
		if(once(p, Treporter, a) < 0)
			return -1;
		if(!iidok(v))
			return badmap("stale=%s: reporter %s is not an iid",
				s->subject, v);
		strcpy(s->reporter, v);
		return 0;
	}
	if(strcmp(a, "since") == 0){
		if(once(p, Tsince, a) < 0)
			return -1;
		return setu64(&s->since, v, a);
	}
	return 0;
}

/* §3.3's legal status/up/fenced combinations, checked per record */
static int
instdone(Parse *p)
{
	Cinst *i;

	i = p->inst;
	if((p->seen & Ineed) != Ineed){
		if((p->seen & Ionnode) == 0)
			return badmap("%s: no onnode=", i->iid);
		if((p->seen & Iaddr) == 0)
			return badmap("%s: no addr=", i->iid);
		if((p->seen & Iuuid) == 0)
			return badmap("%s: no uuid=", i->iid);
		if((p->seen & Istatus) == 0)
			return badmap("%s: no status=", i->iid);
		return badmap("%s: no up=", i->iid);
	}
	if(i->status == Sdead && i->up != Uno)
		return badmap("%s: status=dead with up=%s", i->iid,
			upname(i->up));
	if(i->status == Sdead && i->fenced)
		return badmap("%s: status=dead with fenced=yes", i->iid);
	if(i->status == Snew && i->up == Uheal)
		return badmap("%s: status=new with up=heal", i->iid);
	if(i->fenced && i->up != Uno)
		return badmap("%s: fenced=yes with up=%s", i->iid,
			upname(i->up));
	return 0;
}

static int
staledone(Parse *p)
{
	Cstale *s, *t;
	int i;

	s = p->stale;
	if((p->seen & Tneed) != Tneed)
		return badmap("stale=%s: no reporter=", s->subject);
	/* §7.1: one mark per ordered pair, so at most one record */
	for(i = 0; i < p->m->nstale; i++){
		t = &p->m->stale[i];
		if(t != s && strcmp(t->subject, s->subject) == 0 &&
		   strcmp(t->reporter, s->reporter) == 0)
			return badmap("two stale records for %s from %s",
				s->subject, s->reporter);
	}
	return 0;
}

static int
mapdone(Parse *p)
{
	Cmap *m;

	m = p->m;
	if((p->seen & Hneed) != Hneed){
		static struct {
			int	bit;
			char	*name;
		} h[] = {
			{ Hepoch, "epoch" }, { Hmonid, "monid" },
			{ Hobjmax, "objmax" }, { Hblksz, "blksz" },
			{ Hreplicas, "replicas" }, { Hcsumalg, "csumalg" },
			{ Hplacehash, "placehash" }, { Hpollms, "pollms" },
			{ Hleasems, "leasems" }, { Hreplms, "replms" },
			{ Hdeadms, "deadms" }, { Houtmins, "outmins" },
			{ Htombdays, "tombdays" }, { Hmincopies, "mincopies" },
			{ Hretain, "retain" },
		};
		int i;

		for(i = 0; i < nelem(h); i++)
			if((p->seen & h[i].bit) == 0)
				return badmap("header has no %s=", h[i].name);
	}
	if(m->leasems <= m->pollms)
		return badmap("leasems %lud is not over pollms %lud",
			m->leasems, m->pollms);
	if(m->deadms <= m->leasems)
		return badmap("deadms %lud is not over leasems %lud",
			m->deadms, m->leasems);
	if(m->replms >= m->deadms)
		return badmap("replms %lud is not under deadms %lud",
			m->replms, m->deadms);
	return 0;
}

/* finish whatever record was open */
static int
endrec(Parse *p)
{
	switch(p->kind){
	case Kmap:
		return mapdone(p);
	case Kinst:
		return instdone(p);
	case Kstale:
		return staledone(p);
	}
	return 0;
}

static int
startrec(Parse *p, char *a, char *v)
{
	Cmap *m;
	Cinst *i;
	Cstale *s;
	int k;

	m = p->m;
	p->seen = 0;
	p->started = 1;
	p->inst = nil;
	p->stale = nil;
	if(strcmp(a, "map") == 0){
		if(++p->nmap > 1)
			return badmap("two map records");
		p->kind = Kmap;
		return setstr(m->name, Clnamelen, v, "cluster name");
	}
	if(strcmp(a, "node") == 0){
		if(!nodeok(v))
			return badmap("node %s is not a node name", v);
		for(k = 0; k < m->nnode; k++)
			if(strcmp(m->node[k], v) == 0)
				return badmap("two node records for %s", v);
		strcpy(m->node[m->nnode++], v);
		p->kind = Knode;
		return 0;
	}
	if(strcmp(a, "instance") == 0){
		if(strlen(v) > Iidlen)
			return badmap("instance id over %d bytes", Iidlen);
		i = &m->inst[m->ninst];
		memset(i, 0, sizeof *i);
		if(iidsplit(v, i->node, &i->idx) < 0)
			return badmap("instance %s is not <node>.<index>", v);
		strcpy(i->iid, v);
		for(k = 0; k < m->ninst; k++)
			if(strcmp(m->inst[k].iid, v) == 0)
				return badmap("two instance records for %s", v);
		strcpy(i->zone, "default");
		i->weight = 100;
		m->ninst++;
		p->kind = Kinst;
		p->inst = i;
		return 0;
	}
	if(strcmp(a, "stale") == 0){
		if(strlen(v) > Iidlen || !iidok(v))
			return badmap("stale=%s is not an iid", v);
		s = &m->stale[m->nstale];
		memset(s, 0, sizeof *s);
		strcpy(s->subject, v);
		m->nstale++;
		p->kind = Kstale;
		p->stale = s;
		return 0;
	}
	p->kind = Kunknown;		/* §3.1: ignore the whole record */
	return 0;
}

static int
attr(Parse *p, char *a, char *v)
{
	switch(p->kind){
	case Kmap:
		return mapattr(p, a, v);
	case Kinst:
		return instattr(p, a, v);
	case Kstale:
		return staleattr(p, a, v);
	}
	return 0;
}

/*
 * Tokenise one line into attr=value tuples and apply them.  §0:
 * tokens are separated by one or more spaces or tabs, and a value
 * contains no white space, so a token is a run of non-blanks split at
 * its first `='.  start says whether the line begins a record, in
 * which case its first token is the record's kind (§3.1).
 */
static int
line(Parse *p, char *s, int start)
{
	char *a, *v;
	int first;

	first = start;
	for(;;){
		while(*s == ' ' || *s == '\t')
			s++;
		if(*s == '\0')
			break;
		a = s;
		while(*s != '\0' && *s != ' ' && *s != '\t')
			s++;
		if(*s != '\0')
			*s++ = '\0';
		if((v = strchr(a, '=')) == nil || v == a)
			return badmap("%.24s is not attr=value", a);
		*v++ = '\0';
		if(first){
			if(startrec(p, a, v) < 0)
				return -1;
			first = 0;
			continue;
		}
		if(attr(p, a, v) < 0)
			return -1;
	}
	if(start && first)
		return badmap("empty record line");
	return 0;
}

/*
 * A record begins at a line whose first character is not white space
 * and continues through following indented lines (§3.1).  A blank
 * line and a comment line are ignored outright (§0) and so are
 * transparent: they neither begin nor end a record.  §0 puts a
 * comment's `#' AT THE START OF A LINE, and §3's grammar is
 * normative, so an indented line is a continuation whose tokens must
 * be attr=value and an indented `#' is bad grammar, not a comment.
 * A line of nothing but white space is blank either way.
 */
static int
blankline(char *s)
{
	if(*s == '#')
		return 1;
	while(*s == ' ' || *s == '\t')
		s++;
	return *s == '\0';
}

/* the node set V of §4.3 step 1: the onnode= of the status=in instances */
static void
placenodes(Cmap *m)
{
	int i, j;

	m->npnode = 0;
	for(i = 0; i < m->ninst; i++){
		if(m->inst[i].status != Sin)
			continue;
		for(j = 0; j < m->npnode; j++)
			if(strcmp(m->pnode[j], m->inst[i].node) == 0)
				break;
		if(j == m->npnode)
			strcpy(m->pnode[m->npnode++], m->inst[i].node);
	}
}

/*
 * §3.3: every instance of one node carries the same zone, absence
 * meaning `default'.  Across the whole map rather than per record,
 * so it runs once the records are all in.
 */
static int
zonesok(Cmap *m)
{
	int i, j;

	for(i = 0; i < m->ninst; i++)
		for(j = 0; j < i; j++)
			if(strcmp(m->inst[i].node, m->inst[j].node) == 0 &&
			   strcmp(m->inst[i].zone, m->inst[j].zone) != 0)
				return badmap("node %s has zones %s and %s",
					m->inst[i].node, m->inst[j].zone,
					m->inst[i].zone);
	return 0;
}

/*
 * A mark names two instances of this map (§3.1, §7.1): the ledger is
 * indexed on both by §5.2's clause 4 and its skip rule, and a mark
 * naming an instance the map does not carry cannot be evaluated by
 * either.
 */
static int
markrefsok(Cmap *m)
{
	int i;

	for(i = 0; i < m->nstale; i++){
		if(mapinst(m, m->stale[i].subject) == nil)
			return badmap("stale=%s names no instance",
				m->stale[i].subject);
		if(mapinst(m, m->stale[i].reporter) == nil)
			return badmap("stale=%s: reporter %s names no "
				"instance", m->stale[i].subject,
				m->stale[i].reporter);
	}
	return 0;
}

/*
 * n objects of sz bytes, zeroed, or nil with an errstr: the count
 * comes from the map text, and malloc takes a ulong, so the product
 * is computed in uvlong and refused rather than wrapped.
 */
static void*
allocn(uvlong n, uvlong sz)
{
	if(sz == 0 || n > 0xffffffffULL / sz){
		werrstr("out of memory");
		return nil;
	}
	return mallocz((ulong)(n * sz), 1);
}

void
mapfree(Cmap *m)
{
	if(m == nil)
		return;
	free(m->inst);
	free(m->stale);
	free(m->node);
	free(m->pnode);
	free(m);
}

Cmap*
mapparse(char *text, long n)
{
	Parse p;
	Cmap *m;
	char *s, *e, *nl;
	long i;
	long nrec;
	int start;

	for(i = 0; i < n; i++){
		if(text[i] == '\n' || text[i] == '\t')
			continue;
		if((uchar)text[i] < 0x20 || (uchar)text[i] > 0x7e){
			badmap("byte %ld is not 7-bit printable ascii", i);
			return nil;
		}
	}

	/*
	 * Every record array is sized by the lines that can START a
	 * record: unindented, not blank, not a comment (§3.1, §0).
	 * Sizing by newlines instead charged 720 bytes of heap for a
	 * blank line as for a record, so a map of blank lines cost
	 * about 720 times its own text.  What bounds nrec in practice
	 * is the monitor, which is where a map text comes from:
	 * store.md §10 refuses to commit a text longer than a slot
	 * less its header sector — lib/mon.c's moncommit, 64 KiB at
	 * the default slotsz — which is some thirty thousand record
	 * lines.  Nothing here rests on that bound, though: allocn
	 * computes every size in uvlong and refuses one a ulong
	 * cannot hold, ulong being 32 bits in this dialect.
	 */
	nrec = 0;
	start = 1;
	for(i = 0; i < n; i++){
		if(start && text[i] != '\n' && text[i] != ' ' &&
		   text[i] != '\t' && text[i] != '#')
			nrec++;
		start = text[i] == '\n';
	}
	if(nrec < 1)
		nrec = 1;		/* mallocz(0) is not a size */

	memset(&p, 0, sizeof p);
	if((m = mallocz(sizeof *m, 1)) == nil)
		return nil;
	p.m = m;
	m->inst = allocn(nrec, sizeof *m->inst);
	m->stale = allocn(nrec, sizeof *m->stale);
	m->node = allocn(nrec, sizeof *m->node);
	m->pnode = allocn(nrec, sizeof *m->pnode);
	p.buf = mallocz(n + 1, 1);
	if(m->inst == nil || m->stale == nil || m->node == nil ||
	   m->pnode == nil || p.buf == nil){
		free(p.buf);
		mapfree(m);
		return nil;
	}
	memmove(p.buf, text, n);
	p.buf[n] = '\0';
	strcpy(m->placerule, "nodes");	/* §3.2: v1's only value */

	s = p.buf;
	e = p.buf + n;
	while(s < e){
		if((nl = strchr(s, '\n')) == nil)
			nl = e;
		*nl = '\0';
		if(blankline(s)){
			s = nl + 1;
			continue;
		}
		start = s[0] != ' ' && s[0] != '\t';
		if(start){
			if(endrec(&p) < 0)
				goto bad;
		}else if(!p.started){
			badmap("an indented line begins the map");
			goto bad;
		}
		if(line(&p, s, start) < 0)
			goto bad;
		s = nl + 1;
	}
	if(endrec(&p) < 0)
		goto bad;
	if(p.nmap != 1){
		badmap("no map record");
		goto bad;
	}
	if(zonesok(m) < 0 || markrefsok(m) < 0)
		goto bad;
	placenodes(m);
	free(p.buf);
	return m;
bad:
	free(p.buf);
	mapfree(m);
	return nil;
}

Cinst*
mapinst(Cmap *m, char *iid)
{
	int i;

	for(i = 0; i < m->ninst; i++)
		if(strcmp(m->inst[i].iid, iid) == 0)
			return &m->inst[i];
	return nil;
}

/*
 * §8.1's commit-time validation, the half that needs the current map:
 * the epoch is exactly current+1 and §8.5's immutable attributes are
 * unchanged.  `force' is forceepoch's exemption (§8.6), which lifts
 * the monid check outright and replaces `exactly current+1' with
 * `above current' — §8.1 sets the next epoch to "an arbitrary higher
 * value", §6.1 makes the epoch strictly increasing and §8.6.2 forbids
 * publishing an epoch that cannot be proved the highest, so the
 * exemption is from the +1, not from increasing.
 */
int
mapnextok(Cmap *cur, Cmap *next, int force)
{
	if(force){
		if(next->epoch <= cur->epoch){
			badmap("epoch %llud is not above %llud", next->epoch,
				cur->epoch);
			return 0;
		}
	}else{
		if(next->epoch != cur->epoch + 1){
			badmap("epoch %llud is not %llud+1", next->epoch,
				cur->epoch);
			return 0;
		}
		if(strcmp(next->monid, cur->monid) != 0){
			badmap("monid changed, and it is immutable");
			return 0;
		}
	}
	if(next->objmax != cur->objmax){
		badmap("objmax changed, and §8.5 fixes it");
		return 0;
	}
	if(next->blksz != cur->blksz){
		badmap("blksz changed, and §8.5 fixes it");
		return 0;
	}
	if(strcmp(next->csumalg, cur->csumalg) != 0){
		badmap("csumalg changed, and §8.5 fixes it");
		return 0;
	}
	if(strcmp(next->placehash, cur->placehash) != 0){
		badmap("placehash changed, and §8.5 fixes it");
		return 0;
	}
	return 1;
}

/*
 * §4.2.  H(x) is the first 8 bytes of unkeyed BLAKE2s-256 over x,
 * big-endian; the score input is oid || 0x00 || dom || id, hashed in
 * three pieces rather than assembled, so no length bound of this
 * file's choosing sits in front of the placement function.
 *
 * The state is a zeroed one of our own on the stack, never nil:
 * libsec allocates a state for a nil one and answers nil if that
 * allocation fails, and maphash answers a uvlong with no error
 * channel, so a failure there would restart the hash over the second
 * piece and hand placement a plausible wrong score — and §4.2/§4.3
 * are normative in full, two implementations agreeing bit for bit.
 * A zeroed state is what libsec seeds (seeded == 0) and does not
 * free (malloced == 0), so it is also one malloc and free the
 * cheaper.
 */
uvlong
maphash(char *oid, int dom, char *id)
{
	DigestState s;
	uchar dig[Csumlen], sep[2];
	uvlong v;
	int i;

	memset(&s, 0, sizeof s);
	sep[0] = 0;
	sep[1] = (uchar)dom;
	blake2s_256((uchar*)oid, strlen(oid), nil, &s);
	blake2s_256(sep, sizeof sep, nil, &s);
	blake2s_256((uchar*)id, strlen(id), dig, &s);
	v = 0;
	for(i = 0; i < 8; i++)
		v = v<<8 | dig[i];
	return v;
}

/*
 * §4.3's order over scored candidates, the node round's and the
 * instance round's alike: descending score, ties broken in favour of
 * the byte-wise greater id.  Public because it is the normative half
 * a reimplementation must match and the half no known-answer vector
 * can reach: a tie needs two ids whose 64-bit scores collide.
 */
int
placecmp(uvlong sa, char *a, uvlong sb, char *b)
{
	if(sa != sb)
		return sa < sb ? -1 : 1;
	return idcmp(a, b);
}

/* a ranks below b in that order */
static int
below(uvlong sa, char *a, uvlong sb, char *b)
{
	return placecmp(sa, a, sb, b) < 0;
}

int
mapplace(Cmap *m, char *oid, Cinst **out, int nout)
{
	uvlong sbuf[32], *sc, sbest, si;
	Cinst *pick, *ip;
	int i, k, n, np, best, prev;

	if(m->npnode == 0)
		return 0;
	sc = sbuf;
	if(m->npnode > (int)nelem(sbuf) &&
	   (sc = malloc(m->npnode * sizeof *sc)) == nil){
		/*
		 * Not 0: |P| = 0 is §4.3's legal "nothing places here",
		 * which mapprimary turns into `object unavailable' on
		 * the wire, and an allocation failure must not be
		 * spelled as an answer about the map.
		 */
		werrstr("out of memory");
		return -1;
	}
	for(i = 0; i < m->npnode; i++)
		sc[i] = maphash(oid, 'N', m->pnode[i]);

	np = m->replicas;
	prev = -1;
	n = 0;
	for(k = 0; k < np; k++){
		/*
		 * The next node in descending order: the greatest that
		 * ranks below the one taken last.  The order is total
		 * because node ids are distinct, so this needs no
		 * scratch marks and no sort; running out of nodes
		 * before R of them is §4.3 step 4's |P| < R.
		 */
		best = -1;
		for(i = 0; i < m->npnode; i++){
			if(prev >= 0 &&
			   !below(sc[i], m->pnode[i], sc[prev], m->pnode[prev]))
				continue;
			if(best < 0 ||
			   below(sc[best], m->pnode[best], sc[i], m->pnode[i]))
				best = i;
		}
		if(best < 0)
			break;		/* §4.3 step 4: |P| < R is legal */
		prev = best;

		pick = nil;
		sbest = 0;
		for(i = 0; i < m->ninst; i++){
			ip = &m->inst[i];
			if(ip->status != Sin ||
			   strcmp(ip->node, m->pnode[best]) != 0)
				continue;
			si = maphash(oid, 'D', ip->iid);
			if(pick == nil || below(sbest, pick->iid, si, ip->iid)){
				pick = ip;
				sbest = si;
			}
		}
		/*
		 * A chosen node always has a status=in instance, since
		 * that is what put it in V; emitting nothing for one
		 * that does not keeps P free of holes rather than
		 * handing a caller a member to dereference.
		 */
		if(pick == nil)
			continue;
		if(n < nout)
			out[n] = pick;
		n++;
	}
	if(sc != sbuf)
		free(sc);
	return n;
}

Cinst*
mapprimary(Cmap *m, char *oid)
{
	Cinst *p[Maxplace];
	int i, np;

	if((np = mapplace(m, oid, p, nelem(p))) < 0)
		return nil;		/* mapplace's errstr stands */
	for(i = 0; i < np; i++)
		if(p[i]->up == Uyes)
			return p[i];
	return nil;
}

int
mapunderrep(Cmap *m, char *oid)
{
	Cinst *p[Maxplace];
	int np;

	if((np = mapplace(m, oid, p, nelem(p))) < 0)
		return -1;		/* mapplace's errstr stands */
	return np < (int)m->replicas;
}

/*
 * §6.4 F3: an instance whose own map says up=no or status=out for
 * itself may not serve role=client I/O, and answers `down'.  An
 * instance the map does not carry at all may not either — F3's
 * carve-out is for one the map still counts.
 */
int
mapdown(Cmap *m, char *iid)
{
	Cinst *i;

	if((i = mapinst(m, iid)) == nil)
		return 1;
	return i->up == Uno || i->status == Sout || i->status == Sdead;
}

/* §6.4's zombie rule: no record, or status=dead, is not a member */
int
mapmember(Cmap *m, char *iid)
{
	Cinst *i;

	if((i = mapinst(m, iid)) == nil)
		return 0;
	return i->status != Sdead;
}

static int
inplace(Cinst **p, int np, Cinst *i)
{
	int k;

	for(k = 0; k < np; k++)
		if(p[k] == i)
			return 1;
	return 0;
}

static int
addwit(Cwit *out, int nout, int n, Cinst *i, int why)
{
	int k;

	for(k = 0; k < n && k < nout; k++)
		if(out[k].inst == i){
			out[k].why |= why;
			return n;
		}
	if(n < nout){
		out[n].inst = i;
		out[n].why = why;
		out[n].how = Wquery;
	}
	return n + 1;
}

/*
 * §5.2's witness set.  §5.2 attaches its substitution — "substitute
 * every instance with status in {new,in,out}" — to clause 2 alone,
 * "for this clause", and clause 4 and the skip rule take no
 * substitute: they scope on P(o) at E, plus P(o) at E−1 when this
 * instance holds the E−1 map, and on P(o) at E alone when it does
 * not.  Substituting for them would make clause 4 the unscoped
 * reading §5.2 spends a paragraph ruling out — the substituted set
 * is every instance but a dead one, so "the reporter of any
 * unresolved mark, whatever the subject" — under which one down
 * reporter fails every currency check in the cluster.
 */

/* prev is the map at E−1 itself, which is what clause 2 asks for */
static int
prevmap(Witreq *w)
{
	return w->prev != nil && w->prev->epoch + 1 == w->m->epoch;
}

/*
 * Clause 2: P(o) at E−1 when this instance holds that map and its
 * reconcile pass is complete, and §5.2's substituted set otherwise.
 */
static int
clause2(Witreq *w, Cinst **pp, int npp, Cinst *i)
{
	if(prevmap(w) && !w->subst)
		return inplace(pp, npp, i);
	return i->status == Snew || i->status == Sin || i->status == Sout;
}

/*
 * The scope clause 4 and the skip rule share: the mark's subject is
 * in P(o) at E, or in P(o) at E−1 when that map is held.  subst does
 * not reach here; with no E−1 map the E−1 half is simply not
 * evaluated, and fetching /maps/<E−1> is the caller's obligation.
 */
static int
inscope(Witreq *w, Cinst **p, int np, Cinst **pp, int npp, Cinst *i)
{
	if(inplace(p, np, i))
		return 1;
	return prevmap(w) && inplace(pp, npp, i);
}

int
mapwitness(Witreq *w, Cwit *out, int nout)
{
	Cmap *m;
	Cinst *p[Maxplace], *pv[Maxplace], *pp[Maxplace], *ip, *sub;
	int i, k, n, np, npv, npp, why;

	m = w->m;
	if((np = mapplace(m, w->oid, p, nelem(p))) < 0)
		return -1;		/* mapplace's errstr stands */
	npp = 0;
	if(prevmap(w)){
		if((npv = mapplace(w->prev, w->oid, pv, nelem(pv))) < 0)
			return -1;
		/* the same instances, named in the map at E */
		for(i = 0; i < npv; i++)
			if((ip = mapinst(m, pv[i]->iid)) != nil)
				pp[npp++] = ip;
	}

	n = 0;
	for(i = 0; i < m->ninst; i++){
		ip = &m->inst[i];
		if(ip->status == Sdead)		/* §3.3, §5.2 */
			continue;
		why = 0;
		if(inplace(p, np, ip))
			why |= Wplace;
		if(clause2(w, pp, npp, ip))
			why |= Wprev;
		for(k = 0; k < w->nstray; k++)
			if(strcmp(ip->iid, w->stray[k]) == 0)
				why |= Wstray;
		/*
		 * Clause 4, scoped: the reporter of an unresolved mark
		 * whose SUBJECT is in P(o) at E or at E−1.
		 */
		for(k = 0; k < m->nstale; k++){
			if(strcmp(m->stale[k].reporter, ip->iid) != 0)
				continue;
			if((sub = mapinst(m, m->stale[k].subject)) == nil)
				continue;
			if(inscope(w, p, np, pp, npp, sub)){
				why |= Wreporter;
				break;
			}
		}
		if(why != 0)
			n = addwit(out, nout, n, ip, why);
	}

	/* §5.2's skip rule, which is clause 4's condition again */
	for(i = 0; i < n && i < nout; i++){
		if(out[i].inst->up != Uno)
			out[i].how = Wquery;
		else if(out[i].why & Wreporter)
			out[i].how = Wblock;
		else
			out[i].how = Wskip;
	}
	return n;
}

Cinst*
witblocker(Cwit *w, int n)
{
	int i;

	for(i = 0; i < n; i++)
		if(w[i].how == Wblock)
			return w[i].inst;
	return nil;
}

/*
 * §6.3.  The monid tripwire is answered before the epoch, because a
 * map from a different authority is not a map whose epoch means
 * anything next to ours: an instance that has never adopted takes
 * whatever monid its first map carries.
 */
int
mapadoptable(Adopt *a, Cmap *m)
{
	if(a->pinned && strcmp(a->monid, m->monid) != 0)
		return Mapmonid;
	if(a->pinned && m->epoch < a->epoch)
		return Mapregress;
	return Mapok;
}

void
mapadopted(Adopt *a, Cmap *m)
{
	if(!a->pinned){
		strcpy(a->monid, m->monid);
		a->pinned = 1;
	}
	a->epoch = m->epoch;
}

char*
adoptwhy(int r)
{
	switch(r){
	case Mapregress:	return "epochregress";
	case Mapmonid:		return "monidmismatch";
	}
	return nil;
}

/*
 * §6.4 F1 and F4.  F1 fences when leasems has elapsed since the last
 * successful refresh; an instance that has never refreshed has no map
 * and is fenced by the same rule.  A clock that has gone backwards
 * breaks §6.4's one assumption about clocks — that elapsed time can
 * be measured — and an interval that cannot be measured counts as
 * elapsed: fenced, kind lease, until the next successful refresh
 * moves `last' forward.  This function is the only thing between a
 * deposed primary and the D2 violation §6.4 exists to prevent, and
 * one poll interval of `not ready' is the cheaper mistake.
 */
int
fencekind(Fence *f, vlong now)
{
	int k;

	k = Fencenone;
	if(!f->refreshed || now < f->last || now - f->last >= (vlong)f->leasems)
		k |= Fencelease;
	if(f->oper)
		k |= Fenceoper;
	return k;
}

void
fencerefresh(Fence *f, vlong now)
{
	f->refreshed = 1;
	f->last = now;
}

void
fenceoperator(Fence *f, int on)
{
	f->oper = on != 0;
}

char*
fencename(int kind)
{
	switch(kind & Fenceboth){
	case Fencenone:		return "none";
	case Fencelease:	return "lease";
	case Fenceoper:		return "operator";
	}
	return "both";
}

/*
 * One refresh: §6.3's decision and §6.4's lease clock together.  A
 * map refused for regression or a monid mismatch is not a successful
 * refresh, so it neither adopts nor clears the lease fence.
 */
int
maprefresh(Adopt *a, Fence *f, Cmap *m, vlong now)
{
	int r;

	if((r = mapadoptable(a, m)) != Mapok)
		return r;
	mapadopted(a, m);
	fencerefresh(f, now);
	return Mapok;
}
