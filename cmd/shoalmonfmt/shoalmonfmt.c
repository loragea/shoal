#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../../lib/shoal.h"

/*
 * shoalmonfmt - format a monitor map partition,
 * docs/design/store.md §10 and §12.
 *
 * The path is an sd(3) partition when it names one of an sd unit's
 * partitions, and a plain file otherwise, exactly as shoalfmt decides
 * it; -z sizes a file image.  Every refusal below is monfmt's — the
 * tool is argument parsing and a report, so that a T1 program drives
 * the decisions without exec'ing anything.
 */

static void
usage(void)
{
	fprint(2, "usage: %s [-r] [-s slotsz] [-R retain] [-z size] "
		"/dev/sdXX/name\n", argv0);
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

/* slotsz and retain are u32 in the header (§10) */
static ulong
num32(char *s)
{
	uvlong v;

	v = num(s);
	if(v == 0 || v >= (1ULL<<32))
		sysfatal("%s is out of range: 1 to 2^32-1", s);
	return v;
}

void
main(int argc, char **argv)
{
	Dev *d;
	Monfmtcfg c;
	char *path;
	vlong size;

	memset(&c, 0, sizeof c);
	size = Monsizedflt;

	ARGBEGIN{
	case 'r':
		c.ream = 1;
		break;
	case 's':
		c.slotsz = num32(EARGF(usage()));
		break;
	case 'R':
		c.retain = num32(EARGF(usage()));
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
		if((d = sdopen(path, 0)) == nil)
			sysfatal("%s: %r", path);
	}else{
		if((d = fileopen(path, Secszdflt, size, 0)) == nil)
			sysfatal("%s: %r", path);
	}

	if(monfmt(d, &c) < 0)
		sysfatal("%s: %r", path);

	/*
	 * §12: a valid object-store superblock on the target means the
	 * unit is an object-store instance's, and §2.1's deployment
	 * rule says the monitor's partition MUST NOT be one.  It is
	 * reported and not refused — the operator may be reclaiming a
	 * decommissioned unit — but it is reported loudly.
	 */
	if(c.warnsuper)
		fprint(2, "warning: %s carried a valid shoal object-store "
			"superblock; §2.1 forbids sharing an sd unit with an "
			"object-store instance\n", path);

	print("%s: %lld bytes, %lud-byte sectors\n", path, d->size, d->secsz);
	print("slotsz=%lud retain=%lud\n", c.slotsz, c.retain);
	print("header sectors 0 and %llud, current slots at sector %llud, "
		"%lud history slots at sector %llud\n",
		(uvlong)(d->size/d->secsz - 1), c.curoff, c.retain, c.histoff);
	print("uses %llud bytes of the partition\n", c.used);
	devclose(d);
	exits(nil);
}
