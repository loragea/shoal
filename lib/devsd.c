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
 * rest.  A second opener of the raw file is undetectable from here:
 * the kernel's interlock is never set, so nothing this code does can
 * discover the violation, which is why the rule is the operator's.
 *
 * §3.2's single flusher proc is what owns this Dev.  It must be a
 * proc — proccreate, not threadcreate: the flush is three blocking
 * system calls, and under libthread a QLock parks one thread while a
 * blocking read parks the whole proc.
 */

typedef struct Sd Sd;
struct Sd
{
	int	fd;
	int	rawfd;
	char	*rawpath;
	int	broken;		/* the raw channel could not be recovered */
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

/*
 * Put the raw channel back after a failed command.  The cdb → data →
 * status exchange is per-unit kernel state, and a command that failed
 * inside devsd has already moved the unit on, but one whose system
 * call was aborted before it reached devsd — a note, which §0 says can
 * arrive at any device call — has not.  Guessing which is which leaves
 * the unit in a state the next flush trips over, so the channel is
 * closed and reopened instead: 462 µs on an error path
 * (docs/platform/9front-storage.md §6), and it puts the unit back at
 * Rawcmd whatever it was in the middle of.
 */
static void
rawrecover(Sd *s)
{
	close(s->rawfd);
	s->rawfd = open(s->rawpath, ORDWR);
	if(s->rawfd < 0)
		s->broken = 1;
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
	if(s->broken){
		werrstr("%s: the flush channel is gone", d->name);
		return -1;
	}
	if(s->rawfd < 0)
		return 0;		/* §3.2's -w: no channel, no flush */
	rv = 0;
	qlock(&s->raw);
	memset(cdb, 0, sizeof cdb);
	cdb[0] = 0x35;			/* SYNCHRONIZE CACHE (10) */
	if(write(s->rawfd, cdb, sizeof cdb) != sizeof cdb){
		werrstr("%s: flush cdb: %r", d->name);
		rawrecover(s);
		qunlock(&s->raw);
		return -1;
	}
	/*
	 * The data phase is where devsd actually issues the command;
	 * SYNCHRONIZE CACHE carries no data, so the count is zero.
	 */
	if(read(s->rawfd, buf, 0) < 0){
		werrstr("%s: flush: %r", d->name);
		rawrecover(s);
		qunlock(&s->raw);
		return -1;
	}
	buf[0] = '\0';
	n = read(s->rawfd, buf, sizeof buf - 1);
	if(n < 0){
		werrstr("%s: flush status: %r", d->name);
		rawrecover(s);
		rv = -1;
	}else{
		buf[n] = '\0';
		if(atoi(buf) != 0){
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
	free(s->rawpath);
	free(s);
}

/*
 * No point function: §13's named crash points are the simulated
 * disk's, and on a real partition devpoint is a no-op.  A crash at a
 * point on real hardware is the server killing itself there (§13's
 * -X), not the device doing anything.
 */
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
 * Is this path a partition of an sd(3) unit?  The question is about
 * the directory the path lies in, not about how the path is spelled:
 * a unit's directory holds the unit's own ctl and raw files beside its
 * partitions, and #S/sdF0/name in a cpu namespace is as ordinary a way
 * to name a partition as /dev/sdF0/name (§12).  Getting this wrong is
 * silent — a partition opened as a plain file takes the default sector
 * size and has no flush channel — so it is decided by what is there.
 */
int
sdpart(char *path)
{
	char *f;
	int ok;

	if((f = unitfile(path, "ctl")) == nil)
		return 0;
	ok = access(f, AREAD) == 0;
	free(f);
	if(!ok)
		return 0;
	if((f = unitfile(path, "raw")) == nil)
		return 0;
	ok = access(f, AEXIST) == 0;
	free(f);
	return ok;
}

/*
 * Open an sd(3) partition.  Dnoflush is §3.2's -w: the operator's
 * assertion that the unit is write-through, which is the only way to
 * run without the raw channel.  Drdonly opens the partition OREAD and
 * opens no raw channel at all, so the durability of the unit is not
 * observed and is reported as unknown rather than asserted.
 */
Dev*
sdopen(char *part, int flags)
{
	Dev *d;
	Sd *s;
	Dir *dir;
	long secsz;

	if((secsz = ctlsecsz(part)) < 0)
		return nil;
	if((s = mallocz(sizeof *s, 1)) == nil)
		return nil;
	s->rawfd = -1;
	if((s->fd = open(part, flags & Drdonly ? OREAD : ORDWR)) < 0){
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
	d->wunit = Wunitdflt;
	d->rdonly = (flags & Drdonly) != 0;
	d->flushmode = flags & Drdonly ? Funknown : Fasserted;
	d->aux = s;
	free(dir);
	if((flags & (Dnoflush|Drdonly)) == 0){
		if((s->rawpath = unitfile(part, "raw")) == nil){
			devclose(d);
			return nil;
		}
		s->rawfd = open(s->rawpath, ORDWR);
		if(s->rawfd < 0){
			werrstr("%s: no flush channel: %r", part);
			devclose(d);
			return nil;
		}
		d->flushmode = Fraw;
	}
	return d;
}
