#include <u.h>
#include <libc.h>
#include <libsec.h>
#include "../../lib/shoal.h"

/*
 * shoalcsum — print the layer-A object checksum (§1.4) of each file,
 * treating the file's bytes as the object's content.
 */

static ulong blksz = Blkszdflt;

static void
usage(void)
{
	fprint(2, "usage: %s [-b blksz] file...\n", argv0);
	exits("usage");
}

static int
csumfile(char *name)
{
	Csum c;
	uchar *buf, dig[Blkdlen], csum[Csumlen];
	char hex[Csumhexlen];
	int fd;
	long n;

	if((fd = open(name, OREAD)) < 0){
		fprint(2, "%s: %s: %r\n", argv0, name);
		return -1;
	}
	if((buf = malloc(blksz)) == nil)
		sysfatal("malloc: %r");
	csuminit(&c);
	while((n = readn(fd, buf, blksz)) > 0){
		blkdigest(buf, n, dig);
		csumadd(&c, dig);
		if((ulong)n < blksz)
			break;
	}
	free(buf);
	close(fd);
	if(n < 0){
		fprint(2, "%s: %s: %r\n", argv0, name);
		return -1;
	}
	csumfinal(&c, csum);
	print("%s\t%s\n", csumfmt(hex, csum), name);
	return 0;
}

void
main(int argc, char **argv)
{
	int i, rv;

	ARGBEGIN{
	case 'b':
		blksz = strtoul(EARGF(usage()), nil, 0);
		if(blksz == 0 || (blksz & (blksz-1)) != 0)
			sysfatal("blksz must be a power of two");
		break;
	default:
		usage();
	}ARGEND

	if(argc == 0)
		usage();
	rv = 0;
	for(i = 0; i < argc; i++)
		if(csumfile(argv[i]) < 0)
			rv = -1;
	exits(rv < 0 ? "errors" : nil);
}
