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
 * it; -z sizes a file image.  The geometry and reformat refusals are
 * monfmt's and monfmtcheck's, so that a T1 program drives them
 * without exec'ing anything; what is left here is argument parsing,
 * a report, and the decisions about the file image itself, which the
 * library is never handed: the length to open it at, whether -z may
 * shorten or create it, the two reformat guards whose refusals this
 * command is the one to print, and the removal of an image this run
 * itself created.
 *
 * Those are the order below.  -z is destructive on its own: it
 * truncates whatever the image already holds before a byte of it has
 * been looked at.  So a refused run must refuse BEFORE the resize,
 * whatever the refusal — and the refusals are not all in one place.
 * The order is: open the image at its own length; run both reformat
 * guards over it, since an image carrying a valid monitor header or
 * a valid object-store superblock is a store and neither is
 * overwritten or shortened without -r; ask monfmtcheck about the
 * length -z would give it; and only then resize and format.  A run
 * refused at any of those leaves the file byte-identical, length
 * included — and leaves no file at all where the run's own -z had
 * just created one.
 *
 * A first open that fails is not by itself "no image there yet": a
 * path that exists and will not open READ-WRITE would be CREATED by
 * the second open, truncating it with no guard run at all.  So the
 * path is stat'd, and only a path that is not there falls through to
 * -z's create.
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

/* the image's own length, which is not the device's rounded size */
static vlong
ownlen(char *path, Dev *d)
{
	Dir *dir;
	vlong n;

	if((dir = dirstat(path)) == nil)
		return d->size;
	n = dir->length;
	free(dir);
	return n;
}

/*
 * A run that CREATED the image and then refused destroys nothing by
 * removing it again: there was no file on that path before the run,
 * so §12's "a refused run leaves the file byte-identical, its length
 * included" is kept by the path holding no file once more.  Without
 * this, a geometry refusal after -z's create leaves an image at the
 * refused length, and an operator who corrects the flag and re-runs
 * without -z formats that leftover at the wrong one.
 *
 * It is an atexit rather than a remove beside each refusal because
 * every exit between the create and the end of the run is a sysfatal,
 * which ends in exits(), which runs this — including the ones the
 * library raises and any added later.  The successful path disarms
 * it.  Armed before the create, so that a create which fails partway
 * leaves nothing either.
 */
static char *created;

static void
rmcreated(void)
{
	if(created != nil)
		remove(created);
}

void
main(int argc, char **argv)
{
	Dev *d;
	Monfmtcfg c;
	Monhsel hs;
	Sbsel sb;
	Dir *dir;
	char *path, err[ERRMAX];
	vlong size, have;

	memset(&c, 0, sizeof c);
	size = 0;

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
		if(size != 0)
			sysfatal("-z sizes a file image, not a partition");
		if((d = sdopen(path, 0)) == nil)
			sysfatal("%s: %r", path);
		have = d->size;
	}else if((d = fileopen(path, Secszdflt, 0, 0)) == nil){
		rerrstr(err, sizeof err);
		if((dir = dirstat(path)) != nil){
			/*
			 * It is there and will not open read-write: a
			 * directory, a permission, a file server saying no.
			 * -z cannot help, and creating over it would be
			 * the truncation this order exists to prevent.
			 */
			free(dir);
			sysfatal("%s: %s", path, err);
		}
		/*
		 * No image there yet.  A new one has nothing to destroy,
		 * so -z creates it at its size; without -z there is no
		 * size to create it at, and a partition's own length is
		 * what a partition would have supplied.
		 */
		if(size == 0)
			sysfatal("%s: %s; -z sizes a new file image", path,
				err);
		if(atexit(rmcreated) == 0)
			sysfatal("atexit: %r");
		created = path;
		if((d = fileopen(path, Secszdflt, size, 0)) == nil)
			sysfatal("%s: %r", path);
		size = 0;
		have = d->size;
	}else
		have = ownlen(path, d);

	/*
	 * Both reformat guards, over the image as it stands.  Either
	 * kind of valid header means this image is a store, and -r is
	 * what says to destroy one — by formatting over it or by
	 * shortening it.  monfmt refuses the monitor header again for
	 * a library caller; the object-store superblock it only warns
	 * about, because §2.1's co-location rule is about the unit and
	 * not about these bytes, and the refusal that guards the bytes
	 * is this one.
	 */
	if(!c.ream){
		if(monhdrsel(d, &hs) == 0)
			sysfatal("%s already carries a valid monitor header "
				"(copy %d, slotsz %lud, retain %lud); -r to "
				"reformat it", path, hs.use,
				hs.h[hs.use].slotsz, hs.h[hs.use].retain);
		if(superselect(d, &sb) == 0)
			sysfatal("%s already carries a valid shoal "
				"object-store superblock (copy %d, gen %llud); "
				"-r to format over it", path, sb.start,
				sb.sb[sb.start].gen);
	}

	/* the geometry and the size, against the size asked for */
	if(monfmtcheck(d, size != 0 ? size : have, &c) < 0)
		sysfatal("%s: %r", path);

	/* every refusal is past: now the image may be resized */
	if(size != 0){
		devclose(d);
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
	created = nil;		/* the run finished; the image stays */
	devclose(d);
	exits(nil);
}
