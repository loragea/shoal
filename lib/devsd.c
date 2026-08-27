#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"

/*
 * The real sd(3) device, docs/design/store.md §0 and §3.2.
 *
 * Data goes through the partition file /dev/sdXX/<part>; the flush is
 * a SCSI SYNCHRONIZE CACHE (0x35) issued through /dev/sdXX/raw as a
 * write-cdb / read-data / read-status triple.  That triple is per-unit
 * kernel state and the raw file is not exclusive
 * (docs/platform/9front-storage.md §5), so this file serialises its
 * own commands under one QLock and store.md §2.1's deployment rule —
 * at most one process per sd unit issues raw commands — covers the
 * rest.  §3.2's single flusher proc is what owns this Dev.
 */

typedef struct Sd Sd;
struct Sd
{
	int	fd;
	int	rawfd;
	QLock	raw;
};

static long
sdrd(Dev *d, void *a, long n, vlong off)
{
	Sd *s;

	s = d->aux;
	return pread(s->fd, a, n, off);
}

static long
sdwr(Dev *d, void *a, long n, vlong off)
{
	Sd *s;

	s = d->aux;
	return pwrite(s->fd, a, n, off);
}

static int
sdflush(Dev *d)
{
	Sd *s;
	uchar cdb[10];
	char buf[32];
	long n;
	int rv;

	s = d->aux;
	if(s->rawfd < 0)
		return 0;		/* §3.2's -w: no channel, no flush */
	rv = 0;
	qlock(&s->raw);
	memset(cdb, 0, sizeof cdb);
	cdb[0] = 0x35;			/* SYNCHRONIZE CACHE (10) */
	if(write(s->rawfd, cdb, sizeof cdb) != sizeof cdb){
		werrstr("%s: flush cdb: %r", d->name);
		qunlock(&s->raw);
		return -1;
	}
	/*
	 * The data phase is where devsd actually issues the command;
	 * SYNCHRONIZE CACHE carries no data, so the count is zero.  A
	 * failure here still leaves the unit in its status state, so
	 * the status read below runs either way to return it to Rawcmd.
	 */
	werrstr("");
	if(read(s->rawfd, buf, 0) < 0){
		werrstr("%s: flush: %r", d->name);
		rv = -1;
	}
	buf[0] = '\0';
	n = read(s->rawfd, buf, sizeof buf - 1);
	if(n < 0){
		if(rv == 0)
			werrstr("%s: flush status: %r", d->name);
		rv = -1;
	}else{
		buf[n] = '\0';
		if(rv == 0 && atoi(buf) != 0){
			werrstr("%s: flush: scsi status %s", d->name, buf);
			rv = -1;
		}
	}
	qunlock(&s->raw);
	return rv;
}

static void
sdclose(Dev *d)
{
	Sd *s;

	s = d->aux;
	if(s->fd >= 0)
		close(s->fd);
	if(s->rawfd >= 0)
		close(s->rawfd);
	free(s);
}

static Devops sdops =
{
	sdrd,
	sdwr,
	sdflush,
	nil,
	sdclose,
};

/* the unit's own file named nm, given a partition path */
static char*
unitfile(char *part, char *nm)
{
	char *p, *q;
	int n;

	if((p = strrchr(part, '/')) == nil){
		werrstr("%s: not a /dev/sdXX/<part> path", part);
		return nil;
	}
	n = p - part;
	if((q = malloc(n + 1 + strlen(nm) + 1)) == nil)
		return nil;
	memmove(q, part, n);
	q[n] = '/';
	strcpy(q + n + 1, nm);
	return q;
}

/* sector size from the unit's ctl file: "geometry <sectors> <secsz>" */
static long
ctlsecsz(char *part)
{
	char *ctl, *buf, *ln[64], *fld[8];
	int fd, i, nl, nf;
	long n, secsz;

	if((ctl = unitfile(part, "ctl")) == nil)
		return -1;
	fd = open(ctl, OREAD);
	free(ctl);
	if(fd < 0)
		return -1;
	if((buf = malloc(8192)) == nil){
		close(fd);
		return -1;
	}
	n = readn(fd, buf, 8191);
	close(fd);
	if(n < 0){
		free(buf);
		return -1;
	}
	buf[n] = '\0';
	secsz = -1;
	nl = getfields(buf, ln, nelem(ln), 0, "\n");
	for(i = 0; i < nl; i++){
		nf = tokenize(ln[i], fld, nelem(fld));
		if(nf >= 3 && strcmp(fld[0], "geometry") == 0){
			secsz = strtol(fld[2], nil, 10);
			break;
		}
	}
	free(buf);
	if(secsz <= 0){
		werrstr("%s: no geometry line in the unit's ctl file", part);
		return -1;
	}
	return secsz;
}

/*
 * Open an sd(3) partition.  noflush is §3.2's -w: the operator's
 * assertion that the unit is write-through, which is the only way to
 * run without the raw channel.
 */
Dev*
sdopen(char *part, int noflush)
{
	Dev *d;
	Sd *s;
	Dir *dir;
	char *raw;
	long secsz;

	if((secsz = ctlsecsz(part)) < 0)
		return nil;
	if((s = mallocz(sizeof *s, 1)) == nil)
		return nil;
	s->rawfd = -1;
	if((s->fd = open(part, ORDWR)) < 0){
		free(s);
		return nil;
	}
	if((dir = dirfstat(s->fd)) == nil){
		close(s->fd);
		free(s);
		return nil;
	}
	if((d = mallocz(sizeof *d, 1)) == nil){
		free(dir);
		close(s->fd);
		free(s);
		return nil;
	}
	d->ops = &sdops;
	d->name = strdup(part);
	d->secsz = secsz;
	d->size = dir->length - dir->length % secsz;
	d->aux = s;
	free(dir);
	if(!noflush){
		if((raw = unitfile(part, "raw")) == nil){
			devclose(d);
			return nil;
		}
		s->rawfd = open(raw, ORDWR);
		free(raw);
		if(s->rawfd < 0){
			werrstr("%s: no flush channel: %r", part);
			devclose(d);
			return nil;
		}
		d->canflush = 1;
	}
	return d;
}
