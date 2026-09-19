#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../../lib/shoal.h"
#include "../../mon/mon.h"

/*
 * shoalmon - the monitor's 9P service, docs/design/layer-a.md §8.
 * Everything the design has anything to say about is in
 * mon/libshoalmon.a, where a T1 program can drive it; what is left
 * here is argument parsing, opening the device and posting the
 * service.  Nothing in this file can be T1-tested, because a T1
 * program execs nothing, so nothing in it decides anything.
 *
 * The argument is the monitor's raw partition, formatted by
 * shoalmonfmt (docs/design/store.md §10, §12).  There is no map file
 * and no -m: the monitor's map is the one in its own partition, which
 * is the difference from shoalsrv, where -m stands in for the monitor
 * client that build does not have.
 *
 * What this build serves is §8.1's READ surface and §8.3's framework:
 * the tree, the role matrix, the status files and the evidence of
 * §8.4.  No verb of §8.3 has an effect, nothing is staged at
 * /map.next, no epoch is published and no automatic transition or
 * timer runs, so this process never writes to the partition it opens.
 * store.md §12 says the same for a reader outside this file.
 */

enum
{
	Stack	= Monstack,
};

void
usage(void)
{
	fprint(2, "usage: %s [-X point[,n]] [-s srvname] /dev/sdXX/mon\n",
		argv0);
	threadexitsall("usage");
}

void
threadmain(int argc, char **argv)
{
	char *srvname, *point, *path, *p;
	Moncfg cfg;
	Monctx *c;
	Dev *d;
	int pointn;

	memset(&cfg, 0, sizeof cfg);
	srvname = "shoalmon";
	point = nil;
	pointn = 1;
	mainstacksize = Stack;
	ARGBEGIN{
	case 's':
		srvname = EARGF(usage());
		break;
	case 'X':
		point = EARGF(usage());
		if((p = strchr(point, ',')) != nil){
			*p++ = 0;
			pointn = atoi(p);
		}
		break;
	default:
		usage();
	}ARGEND

	if(argc != 1)
		usage();
	path = argv[0];

	if(sdpart(path))
		d = sdopen(path, 0);
	else
		d = fileopen(path, Secszdflt, 0, 0);
	if(d == nil)
		sysfatal("%s: %r", path);
	if(point != nil)
		devpoint(d, point, pointn);

	cfg.dev = d;
	if((c = monsrvnew(&cfg)) == nil)
		sysfatal("%s: %r", path);

	monsrvpost(c, srvname);
	threadexits(nil);
}
