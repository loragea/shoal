#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../lib/shoal.h"
#include "mon.h"
#include "dat.h"
#include "fns.h"

/*
 * layer-a §8.1's render-at-open: "All status files are
 * snapshot-at-open (§2.2), so a concurrent mutation cannot produce a
 * torn read."  An Mtext is that snapshot.  It is composed once, into a
 * buffer that grows, and then it is immutable: every Tread on the fid
 * is a memmove out of it at the offset the client asked for.
 *
 * Here the copy is load-bearing twice over.  §8.1 asks for it against
 * a torn read, and lib/shoal.h asks for it against a freed one: a
 * Monmap's `text' is the slot store's own and is valid only until the
 * next commit or monclose, so a fid that kept the pointer would serve
 * another map's bytes — or freed memory — after a publish.
 *
 * This is srv/text.c's Text under a name of its own.  The two
 * libraries are independent by design (mon.h), so the forty lines are
 * duplicated rather than shared, and folding them into one helper both
 * can link is a later change.
 */

enum
{
	Mtextchunk	= 1024,
};

Mtext*
montextnew(void)
{
	Mtext *t;

	if((t = mallocz(sizeof *t, 1)) == nil)
		return nil;
	return t;
}

void
montextfree(Mtext *t)
{
	if(t == nil)
		return;
	free(t->p);
	free(t);
}

static int
montextroom(Mtext *t, long n)
{
	char *p;
	long max;

	if(t->err)
		return -1;
	if(t->n + n <= t->max)
		return 0;
	max = t->max + Mtextchunk;
	while(max < t->n + n)
		max += Mtextchunk;
	if((p = realloc(t->p, max)) == nil){
		t->err = 1;
		return -1;
	}
	t->p = p;
	t->max = max;
	return 0;
}

int
montextwrite(Mtext *t, void *a, long n)
{
	if(n < 0 || montextroom(t, n) < 0)
		return -1;
	memmove(t->p + t->n, a, n);
	t->n += n;
	return 0;
}

/*
 * One attr=value line at a time is what §0's record grammar wants, so
 * the formatted form is what every renderer uses.  A line longer than
 * the buffer is composed through smprint rather than truncated: an
 * /instances line carries a uuid, a dial string and a class tag, and
 * §8.1 fixes its field names.
 */
int
montextprint(Mtext *t, char *fmt, ...)
{
	char buf[512], *p;
	int n;
	va_list arg;

	va_start(arg, fmt);
	p = vseprint(buf, buf + sizeof buf, fmt, arg);
	va_end(arg);
	n = p - buf;
	/*
	 * vseprint truncates at the buffer, leaving no way to tell a
	 * line that just fitted from one that did not.  A line that
	 * reaches the end is re-rendered through smprint, which sizes
	 * itself.
	 */
	if(n == sizeof buf - 1){
		va_start(arg, fmt);
		p = vsmprint(fmt, arg);
		va_end(arg);
		if(p == nil){
			t->err = 1;
			return -1;
		}
		n = montextwrite(t, p, strlen(p));
		free(p);
		return n;
	}
	return montextwrite(t, buf, n);
}

/*
 * Serve one Tread out of the snapshot.  lib9p's readbuf is the whole
 * of the offset and short-read rule — a read at or past the end
 * answers count 0, and one that crosses the end answers the bytes
 * below it.
 */
void
montextread(Req *r, Mtext *t)
{
	if(t == nil){
		respond(r, Emonnotbuilt);
		return;
	}
	readbuf(r, t->p, t->n);
	respond(r, nil);
}
