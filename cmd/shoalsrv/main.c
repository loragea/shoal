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
 * (docs/design/store.md §14(18), §14(19)).
 */

enum
{
	Stack	= Srvstack,

	/*
	 * The largest `-d scrubdays' this command will take: a hundred
	 * years, which is orders above any period an operator has a use
	 * for and far below where the period in milliseconds outgrows
	 * the uvlong srvscrubperiod computes it in.  It is here rather
	 * than in srv/ because it bounds the SPELLING and not the
	 * mechanism: what makes a bound necessary is that Plan 9's
	 * strtol clamps (see `-d' below), which is a parsing fact.
	 * store.md §12 states it.
	 */
	Scrubdaysmax	= 36500,
};

void
usage(void)
{
	fprint(2, "usage: %s [-w] [-X point[,n]] [-q queues] [-d scrubdays] "
		"[-s srvname] -m mapfile /dev/sdXX/name\n", argv0);
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
	char *mapfile, *srvname, *point, *path, *p, *e;
	Srvcfg cfg;
	Srvctx *c;
	Dev *d;
	vlong days;
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
	/*
	 * layer-a §7.5's `scrubdays': how long a full scrub pass should
	 * take, which is also the period between passes (srv/job.c).  A
	 * whole positive number of days or nothing — 0 and a negative are
	 * not a period, and the default is what the flag is absent for.
	 *
	 * strtoll and its end pointer rather than atoi, which stops at
	 * the first character it cannot use and answers what it read:
	 * `3junk' would be 3 and `1.9' would be 1, where store.md §12
	 * says anything but a whole number of days is refused with the
	 * usage.  A period is a thing an operator gets one chance a
	 * start-up to spell, so a typo is worth a usage rather than a
	 * silent 1.
	 *
	 * The end pointer alone does not see an overflow, because Plan
	 * 9's strtol CLAMPS to LONG_MAX and still leaves *e at the end
	 * of the digits: measured on 9front, where a long is four bytes,
	 * `99999999999' comes back 2147483647 with the whole string
	 * consumed, so `-d 99999999999' would have started an instance
	 * scrubbing on a period of some five million years with nothing
	 * in the arguments to see it by.  Hence a vlong, which no
	 * spelling of days can overflow, and an upper bound of
	 * Scrubdaysmax above.
	 *
	 * The first character must be a DIGIT.  strtoll otherwise takes
	 * a leading `+' and leading white space, so `+14' and ` 14' were
	 * 14 while `14 ' was refused by the end pointer; one spelling of
	 * a number, accepted or refused whole, is the rule that has no
	 * such asymmetry in it.
	 */
	case 'd':
		p = EARGF(usage());
		days = strtoll(p, &e, 10);
		if(*p < '0' || *p > '9' || *e != 0
		|| days <= 0 || days > Scrubdaysmax)
			usage();
		cfg.scrubdays = days;
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
