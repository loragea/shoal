#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "shoal.h"

/*
 * A plain file as a device.  Not a deployment target — a partition is
 * (docs/decisions.md D13) — but the store's whole engine sits behind
 * the §0 vtable, so a file-backed device is what lets shoalfmt and
 * shoalck work on an image and what lets a T1 test drive them without
 * a disk.  It has no flush channel: canflush is 0 and flush is a
 * no-op, which is §3.2's -w case.
 */

typedef struct File File;
struct File
{
	int	fd;
};

static long
filerd(Dev *d, void *a, long n, vlong off)
{
	File *f;

	f = d->aux;
	return pread(f->fd, a, n, off);
}

static long
filewr(Dev *d, void *a, long n, vlong off)
{
	File *f;

	f = d->aux;
	return pwrite(f->fd, a, n, off);
}

static int
fileflush(Dev *)
{
	return 0;
}

static void
fileclose(Dev *d)
{
	File *f;

	f = d->aux;
	if(f->fd >= 0)
		close(f->fd);
	free(f);
}

static Devops fileops =
{
	filerd,
	filewr,
	fileflush,
	nil,
	fileclose,
};

/*
 * Open path as a device of secsz-byte sectors.  A size of 0 takes the
 * file's current length; any other size sets it, creating the file if
 * it is not there.
 */
Dev*
fileopen(char *path, ulong secsz, vlong size)
{
	Dev *d;
	File *f;
	Dir *dir, nd;

	if(secsz == 0 || (secsz & (secsz - 1)) != 0){
		werrstr("fileopen: secsz %lud is not a power of two", secsz);
		return nil;
	}
	if((f = mallocz(sizeof *f, 1)) == nil)
		return nil;
	if((f->fd = open(path, ORDWR)) < 0 && size > 0)
		f->fd = create(path, ORDWR, 0666);
	if(f->fd < 0){
		free(f);
		return nil;
	}
	if(size > 0){
		nulldir(&nd);
		nd.length = size;
		if(dirfwstat(f->fd, &nd) < 0){
			close(f->fd);
			free(f);
			return nil;
		}
	}else{
		if((dir = dirfstat(f->fd)) == nil){
			close(f->fd);
			free(f);
			return nil;
		}
		size = dir->length;
		free(dir);
	}
	if((d = mallocz(sizeof *d, 1)) == nil){
		close(f->fd);
		free(f);
		return nil;
	}
	d->ops = &fileops;
	d->name = strdup(path);
	d->secsz = secsz;
	d->size = size - size % secsz;
	d->canflush = 0;
	d->aux = f;
	return d;
}
