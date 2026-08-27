#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../../lib/shoal.h"

/*
 * shoalfmt - format or ream an object-store partition,
 * docs/design/store.md §12.
 *
 * The path is an sd(3) partition when it names one of an sd unit's
 * partitions, and a plain file otherwise; a file is the image form the
 * T1 tests and an operator inspecting a copy both work against, and -z
 * gives it a size.
 */

static void
usage(void)
{
	fprint(2, "usage: %s [-rw] [-b blksz] [-o objmax] [-c csumalg] "
		"[-n nslots] [-e nemap] [-d ndirty] [-L logbytes] "
		"[-u uuid] [-z size] /dev/sdXX/name\n", argv0);
	exits("usage");
}

static uvlong
num(char *s)
{
	char *e;
	uvlong v;

	v = strtoull(s, &e, 0);
	switch(*e){
	case 'k':
	case 'K':
		v *= 1024;
		break;
	case 'm':
	case 'M':
		v *= 1024*1024;
		break;
	case 'g':
	case 'G':
		v *= 1024*1024*1024;
		break;
	case '\0':
		break;
	default:
		sysfatal("bad number %s", s);
	}
	return v;
}

/*
 * nslots, nemap and ndirty are u32 on disk (§2.2) and blksz is too,
 * so a value that does not fit one must be refused here rather than
 * truncated into a geometry the store would then believe.
 */
static ulong
num32(char *s)
{
	uvlong v;

	v = num(s);
	if(v == 0 || v >= (1ULL<<32))
		sysfatal("%s is out of range: 1 to 2^32-1", s);
	return v;
}

static void
gethex(uchar *p, int n, char *s)
{
	int i, c, v;

	if(strlen(s) != 2*n)
		sysfatal("uuid must be %d hex characters", 2*n);
	for(i = 0; i < 2*n; i++){
		c = s[i];
		if(c >= '0' && c <= '9')
			v = c - '0';
		else if(c >= 'a' && c <= 'f')
			v = c - 'a' + 10;
		else if(c >= 'A' && c <= 'F')
			v = c - 'A' + 10;
		else
			sysfatal("uuid: %c is not a hex digit", c);
		if(i & 1)
			p[i/2] |= v;
		else
			p[i/2] = v << 4;
	}
}

static char*
hexstr(char *buf, uchar *p, int n)
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

void
main(int argc, char **argv)
{
	Dev *d;
	Fmtcfg c;
	Super s;
	Sbsel sel;
	char *path, hb[33];
	vlong size;
	uvlong meta;
	int ream, noflush;

	memset(&c, 0, sizeof c);
	c.secsz = Secszdflt;
	c.blksz = Blkszstore;
	c.objmax = Objmaxdflt;
	c.csumalg = Csumblake2s;
	size = 0;
	ream = noflush = 0;

	ARGBEGIN{
	case 'r':
		ream = 1;
		break;
	case 'w':
		noflush = 1;
		break;
	case 'b':
		c.blksz = num32(EARGF(usage()));
		break;
	case 'o':
		c.objmax = num(EARGF(usage()));
		break;
	case 'c':
		if((c.csumalg = csumalgno(EARGF(usage()))) == 0)
			sysfatal("unknown csumalg; this build has blake2s256");
		break;
	case 'n':
		c.nslots = num32(EARGF(usage()));
		break;
	case 'e':
		c.nemap = num32(EARGF(usage()));
		break;
	case 'd':
		c.ndirty = num32(EARGF(usage()));
		break;
	case 'L':
		c.logbytes = num(EARGF(usage()));
		break;
	case 'u':
		gethex(c.uuid, 16, EARGF(usage()));
		c.uuidset = 1;
		break;
	case 'z':
		size = num(EARGF(usage()));
		break;
	default:
		usage();
	}ARGEND

	if(argc != 1)
		usage();
	path = argv[0];

	if(sdpart(path)){
		if(size != 0)
			sysfatal("-z sizes a file image, not a partition");
		if((d = sdopen(path, noflush ? Dnoflush : 0)) == nil)
			sysfatal("%s: %r", path);
	}else{
		if((d = fileopen(path, c.secsz, size, 0)) == nil)
			sysfatal("%s: %r", path);
	}
	c.secsz = d->secsz;

	/*
	 * Reaming a disk destroys an instance's identity, and layer-a
	 * §1.5 makes that a reformat-before-rejoin event, so it takes
	 * a flag.
	 */
	if(!ream){
		if(superselect(d, &sel) == 0)
			sysfatal("%s already carries a valid superblock "
				"(copy %d, gen %llud); -r to ream it", path,
				sel.start, sel.sb[sel.start].gen);
	}

	if(geometry(&s, &c, d->size) < 0)
		sysfatal("%s: %r", path);

	meta = (uvlong)(s.dataoff - 1)*s.secsz;
	print("%s: %lld bytes, %lud-byte sectors\n", path, d->size, s.secsz);
	print("blksz=%lud objmax=%llud csumalg=%s nblkmax=%lud emapsz=%lud\n",
		s.blksz, s.objmax, csumalgname(s.csumalg), s.nblkmax, s.emapsz);
	print("nslots=%lud nemap=%lud ndirty=%lud logsecs=%llud ngrains=%llud\n",
		s.nslots, s.nemap, s.ndirty, s.logsecs, s.ngrains);
	print("uuid=%s\n", hexstr(hb, s.uuid, 16));
	print("metadata %llud bytes, %llud.%.2llud%% of the partition\n",
		meta, meta*100/(uvlong)d->size,
		meta*10000/(uvlong)d->size % 100);
	/*
	 * nemap is the one sizing a workload can defeat, so say how
	 * many multi-block objects this geometry supports: slot 0 of
	 * the extent-map region is reserved and never allocated.
	 */
	print("supports %lud objects larger than %lud bytes\n",
		s.nemap - 1, s.blksz);
	if(meta*100 > (uvlong)d->size)
		print("warning: metadata is more than 1%% of the partition\n");

	if(fmtstore(d, &s) < 0)
		sysfatal("%s: %r", path);
	devclose(d);
	exits(nil);
}
