#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../../lib/shoal.h"

/*
 * shoalck - inspect and check an object-store partition,
 * docs/design/store.md §12.  It reads and never writes, and opens the
 * device read-only so that the kernel enforces that rather than this
 * code promising it.  The path is an sd(3) partition when it names one
 * of an sd unit's partitions and a plain file otherwise.
 */

static void
usage(void)
{
	fprint(2, "usage: %s [-lq] [-o oid] /dev/sdXX/name\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	Dev *d;
	Ckcfg c;
	char *path;
	int bad;

	memset(&c, 0, sizeof c);
	c.out = 1;
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
	default:
		usage();
	}ARGEND

	if(argc != 1)
		usage();
	path = argv[0];

	/* read-only: no flush channel is wanted or opened */
	if(sdpart(path))
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
