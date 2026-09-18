#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../lib/shoal.h"
#include "srv.h"
#include "dat.h"
#include "fns.h"

/*
 * layer-a §2.2's render-at-open: "A read of any of the status files
 * above MUST reflect a snapshot taken when the fid was opened, in the
 * Plan 9 convention, so that a concurrent mutation cannot tear a
 * read."  A Text is that snapshot.  It is composed once, into a buffer
 * that grows, and then it is immutable: every Tread on the fid is a
 * memmove out of it at the offset the client asked for.
 *
 * Growing rather than rendering into a fixed buffer matters for /map,
 * whose bytes are the whole cluster map, and for the enumerations that
 * will be rendered this way later.  An allocation that fails sets err
 * and the text stays as long as it was: the open answers the failure
 * rather than serving a text with a hole in it.
 */

enum
{
	Textchunk	= 1024,
};

Text*
textnew(void)
{
	Text *t;

	if((t = mallocz(sizeof *t, 1)) == nil)
		return nil;
	return t;
}

void
textfree(Text *t)
{
	if(t == nil)
		return;
	free(t->p);
	free(t);
}

static int
textroom(Text *t, long n)
{
	char *p;
	long max;

	if(t->err)
		return -1;
	if(t->n + n <= t->max)
		return 0;
	max = t->max + Textchunk;
	while(max < t->n + n)
		max += Textchunk;
	if((p = realloc(t->p, max)) == nil){
		t->err = 1;
		return -1;
	}
	t->p = p;
	t->max = max;
	return 0;
}

int
textwrite(Text *t, void *a, long n)
{
	if(n < 0 || textroom(t, n) < 0)
		return -1;
	memmove(t->p + t->n, a, n);
	t->n += n;
	return 0;
}

/*
 * One attr=value line at a time is what §0's record grammar wants, so
 * the formatted form is what every renderer uses.  A line longer than
 * the chunk is composed into a buffer of its own rather than being
 * truncated, because /status carries an error string from the engine
 * and /lost will carry a checksum.
 */
int
textprint(Text *t, char *fmt, ...)
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
		n = textwrite(t, p, strlen(p));
		free(p);
		return n;
	}
	return textwrite(t, buf, n);
}

/*
 * Serve one Tread out of the snapshot.  lib9p's readbuf is the whole
 * of the offset and short-read rule — a read at or past the end
 * answers count 0, and one that crosses the end answers the bytes
 * below it — and sread has already clamped count to the iounit.
 */
void
textread(Req *r, Text *t)
{
	if(t == nil){
		respond(r, Enotbuilt);
		return;
	}
	readbuf(r, t->p, t->n);
	respond(r, nil);
}
