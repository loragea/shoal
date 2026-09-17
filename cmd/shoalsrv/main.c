#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../../lib/shoal.h"
#include "../../srv/srv.h"

/*
 * shoalsrv - the storage instance's 9P service, docs/design/layer-a.md
 * §2.  Everything the design has anything to say about is in
 * srv/libshoalsrv.a, where a T1 program can drive it; what is left
 * here is argument parsing, opening the device, reading the map file,
 * posting the service, and the trigger of the shutdown sequence — the
 * service loop ending, which srvshutdown hangs off.  Nothing in this
 * file can be T1-tested, because a T1 program execs nothing, so
 * nothing in it decides anything.
 *
 * The map is a file rather than a monitor connection: this build has
 * no network client, so the epoch, the placement and the stale ledger
 * are whatever -m carries, and F1's lease fence is inert
 * (docs/design/store.md §14(15), §14(16)).
 */

enum
{
	Stack	= Srvstack,
};

void
usage(void)
{
	fprint(2, "usage: %s [-w] [-X point[,n]] [-q queues] [-s srvname] "
		"-m mapfile /dev/sdXX/name\n", argv0);
	threadexitsall("usage");
}

static char*
readmap(char *path, long *np)
{
	char *p;
	Dir *d;
	int fd;
	long n;

	if((fd = open(path, OREAD)) < 0)
		sysfatal("%s: %r", path);
	if((d = dirfstat(fd)) == nil)
		sysfatal("%s: %r", path);
	n = d->length;
	free(d);
	if((p = malloc(n+1)) == nil)
		sysfatal("malloc: %r");
	if(n > 0 && readn(fd, p, n) != n)
		sysfatal("%s: %r", path);
	close(fd);
	p[n] = 0;
	*np = n;
	return p;
}

void
threadmain(int argc, char **argv)
{
	char *mapfile, *srvname, *point, *path, *p;
	Srvcfg cfg;
	Srvctx *c;
	Dev *d;
	int pointn;

	memset(&cfg, 0, sizeof cfg);
	mapfile = nil;
	srvname = "shoal";
	point = nil;
	pointn = 1;
	mainstacksize = Stack;
	ARGBEGIN{
	case 'w':
		cfg.noflush = 1;
		break;
	case 'q':
		cfg.nqueue = atoi(EARGF(usage()));
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'm':
		mapfile = EARGF(usage());
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

	if(argc != 1 || mapfile == nil)
		usage();
	path = argv[0];

	if(sdpart(path))
		d = sdopen(path, cfg.noflush ? Dnoflush : 0);
	else
		d = fileopen(path, Secszdflt, 0, cfg.noflush ? Dnoflush : 0);
	if(d == nil)
		sysfatal("%s: %r", path);
	if(point != nil)
		devpoint(d, point, pointn);

	cfg.dev = d;
	cfg.maptext = readmap(mapfile, &cfg.maplen);
	if((c = srvnew(&cfg)) == nil)
		sysfatal("%s: %r", path);
	free(cfg.maptext);

	srvpost(c, srvname);
	threadexits(nil);
}
