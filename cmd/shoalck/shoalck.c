#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../../lib/shoal.h"

/*
 * shoalck - inspect and check an object-store partition,
 * docs/design/store.md §12.  Every flag but -R reads and never
 * writes, and the device is then opened read-only so that the kernel
 * enforces that rather than this code promising it.  -R rewrites the
 * checkpoint, so it takes the read-write open shoalfmt takes, with
 * the flush channel and -w for the unit whose raw channel will not
 * open (§3.2).  The path is an sd(3) partition when it names one of
 * an sd unit's partitions and a plain file otherwise.
 */

static void
usage(void)
{
	fprint(2, "usage: %s [-lqvRw] [-o oid] /dev/sdXX/name\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	Dev *d;
	Ckcfg c;
	char *path;
	int bad, noflush;

	memset(&c, 0, sizeof c);
	c.out = 1;
	noflush = 0;
	ARGBEGIN{
	case 'l':
		c.verbose++;
		break;
	case 'q':
		c.quiet = 1;
		break;
	case 'o':
		c.oid = EARGF(usage());
		break;
	case 'v':
		c.verify = 1;
		break;
	case 'R':
		c.rebuild = 1;
		break;
	case 'w':
		noflush = 1;
		break;
	default:
		usage();
	}ARGEND

	if(argc != 1)
		usage();
	/*
	 * -o dumps one object; -v and -R are whole-store passes.  -w is
	 * the assertion the read-write open of -R needs, and asserting
	 * write-through of a device this run will not write is a claim
	 * with nothing behind it, so it is refused rather than ignored.
	 */
	if(c.oid != nil && (c.verify || c.rebuild))
		usage();
	if(noflush && !c.rebuild)
		usage();
	c.noflush = noflush;
	path = argv[0];

	if(c.rebuild){
		/* -R writes: the open shoalfmt takes, flush channel and all */
		if(sdpart(path))
			d = sdopen(path, noflush ? Dnoflush : 0);
		else
			d = fileopen(path, Secszdflt, 0, 0);
	}else if(sdpart(path))
		d = sdopen(path, Dnoflush|Drdonly);
	else
		d = fileopen(path, Secszdflt, 0, Drdonly);
	if(d == nil)
		sysfatal("%s: %r", path);

	bad = ckstore(d, &c);
	devclose(d);
	if(bad > 0){
		fprint(2, "%s: %d problem%s\n", path, bad, bad == 1 ? "" : "s");
		exits("bad");
	}
	exits(nil);
}
