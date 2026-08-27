#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../../lib/shoal.h"

/*
 * shoalck - inspect and check an object-store partition,
 * docs/design/store.md §12.  It reads and never writes.  The path is
 * an sd(3) partition when it lies under /dev and a plain file
 * otherwise.
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
	if(strncmp(path, "/dev/", 5) == 0)
		d = sdopen(path, 1);
	else
		d = fileopen(path, Secszdflt, 0);
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
