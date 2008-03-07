/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2008 Linpro AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id$
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <sys/mman.h>

#include "shmlog.h"
#include "cache.h"

#define LOCKSHM(foo)					\
	do {						\
		if (pthread_mutex_trylock(foo)) {	\
			AZ(pthread_mutex_lock(foo));	\
			VSL_stats->shm_cont++;		\
		}					\
	} while (0);

#define UNLOCKSHM(foo)	AZ(pthread_mutex_unlock(foo))

#ifndef MAP_HASSEMAPHORE
#define MAP_HASSEMAPHORE 0 /* XXX Linux */
#endif

#ifndef MAP_NOSYNC
#define MAP_NOSYNC 0 /* XXX Linux */
#endif

struct varnish_stats *VSL_stats;
static struct shmloghead *loghead;
static unsigned char *logstart;
static MTX vsl_mtx;


static void
vsl_wrap(void)
{

	*logstart = SLT_ENDMARKER;
	logstart[loghead->ptr] = SLT_WRAPMARKER;
	loghead->ptr = 0;
}

static void
vsl_hdr(enum shmlogtag tag, unsigned char *p, unsigned len, unsigned id)
{

	p[__SHMLOG_LEN_HIGH] = (len >> 8) & 0xff;
	p[__SHMLOG_LEN_LOW] = len & 0xff;
	p[__SHMLOG_ID_HIGH] = (id >> 8) & 0xff;
	p[__SHMLOG_ID_LOW] = id & 0xff;
	p[SHMLOG_DATA + len] = '\0';
	p[SHMLOG_NEXTTAG + len] = SLT_ENDMARKER;	
	/* XXX: Write barrier here */
	p[SHMLOG_TAG] = tag;
}

/*--------------------------------------------------------------------
 * This variant copies a byte-range directly to the log, without
 * taking the detour over sprintf()
 */

void
VSLR(enum shmlogtag tag, int id, txt t)
{
	unsigned char *p;
	unsigned l;

	Tcheck(t);

	/* Truncate */
	l = Tlen(t);
	if (l > 255) {
		l = 255;
		t.e = t.b + l;
	}

	/* Only hold the lock while we find our space */
	LOCKSHM(&vsl_mtx);
	VSL_stats->shm_writes++;
	VSL_stats->shm_records++;
	assert(loghead->ptr < loghead->size);

	/* Wrap if necessary */
	if (loghead->ptr + SHMLOG_NEXTTAG + l + 1 >= loghead->size)
		vsl_wrap();
	p = logstart + loghead->ptr;
	loghead->ptr += SHMLOG_NEXTTAG + l;
	assert(loghead->ptr < loghead->size);
	UNLOCKSHM(&vsl_mtx);

	memcpy(p + SHMLOG_DATA, t.b, l);
	vsl_hdr(tag, p, l, id);
}

/*--------------------------------------------------------------------*/

void
VSL(enum shmlogtag tag, int id, const char *fmt, ...)
{
	va_list ap;
	unsigned char *p;
	unsigned n;
	txt t;

	AN(fmt);
	va_start(ap, fmt);

	if (strchr(fmt, '%') == NULL) {
		t.b = TRUST_ME(fmt);
		t.e = strchr(t.b, '\0');
		VSLR(tag, id, t);
	} else {
		LOCKSHM(&vsl_mtx);
		VSL_stats->shm_writes++;
		VSL_stats->shm_records++;
		assert(loghead->ptr < loghead->size);

		/* Wrap if we cannot fit a full size record */
		if (loghead->ptr + SHMLOG_NEXTTAG + 255 + 1 >= loghead->size)
			vsl_wrap();

		p = logstart + loghead->ptr;
		n = vsnprintf((char *)(p + SHMLOG_DATA), 256, fmt, ap);
		if (n > 255)
			n = 255; 	/* we truncate long fields */
		vsl_hdr(tag, p, n, id);
		loghead->ptr += SHMLOG_NEXTTAG + n;
		assert(loghead->ptr < loghead->size);
		UNLOCKSHM(&vsl_mtx);
	}
	va_end(ap);
}

/*--------------------------------------------------------------------*/

void
WSL_Flush(struct worker *w, int overflow)
{
	unsigned char *p;
	unsigned l;

	l = pdiff(w->wlb, w->wlp);
	if (l == 0)
		return;
	LOCKSHM(&vsl_mtx);
	VSL_stats->shm_flushes += overflow;
	VSL_stats->shm_writes++;
	VSL_stats->shm_records += w->wlr;
	if (loghead->ptr + l + 1 >= loghead->size)
		vsl_wrap();
	p = logstart + loghead->ptr;
	memcpy(p + 1, w->wlb + 1, l - 1);
	p[l] = SLT_ENDMARKER;
	loghead->ptr += l;
	assert(loghead->ptr < loghead->size);
	/* XXX: memory barrier here */
	p[0] = w->wlb[0];
	UNLOCKSHM(&vsl_mtx);
	w->wlp = w->wlb;
	w->wlr = 0;
}

/*--------------------------------------------------------------------*/

void
WSLR(struct worker *w, enum shmlogtag tag, int id, txt t)
{
	unsigned char *p;
	unsigned l;

	Tcheck(t);

	/* Truncate */
	l = Tlen(t);
	if (l > 255) {
		l = 255;
		t.e = t.b + l;
	}

	assert(w->wlp < w->wle);

	/* Wrap if necessary */
	if (w->wlp + SHMLOG_NEXTTAG + l + 1 >= w->wle)
		WSL_Flush(w, 1);
	p = w->wlp;
	w->wlp += SHMLOG_NEXTTAG + l;
	assert(w->wlp < w->wle);
	memcpy(p + SHMLOG_DATA, t.b, l);
	vsl_hdr(tag, p, l, id);
	w->wlr++;
}

/*--------------------------------------------------------------------*/

void
WSL(struct worker *w, enum shmlogtag tag, int id, const char *fmt, ...)
{
	va_list ap;
	unsigned char *p;
	unsigned n;
	txt t;

	AN(fmt);
	va_start(ap, fmt);

	if (strchr(fmt, '%') == NULL) {
		t.b = TRUST_ME(fmt);
		t.e = strchr(t.b, '\0');
		WSLR(w, tag, id, t);
	} else {
		assert(w->wlp < w->wle);

		/* Wrap if we cannot fit a full size record */
		if (w->wlp + SHMLOG_NEXTTAG + 255 + 1 >= w->wle)
			WSL_Flush(w, 1);

		p = w->wlp;
		n = vsnprintf((char *)(p + SHMLOG_DATA), 256, fmt, ap);
		if (n > 255)
			n = 255; 	/* we truncate long fields */
		vsl_hdr(tag, p, n, id);
		w->wlp += SHMLOG_NEXTTAG + n;
		assert(w->wlp < w->wle);
		w->wlr++;
	}
	va_end(ap);
}

/*--------------------------------------------------------------------*/

void
VSL_Init(void)
{

	assert(loghead->magic == SHMLOGHEAD_MAGIC);
	assert(loghead->hdrsize == sizeof *loghead);
	/* XXX more check sanity of loghead  ? */
	logstart = (unsigned char *)loghead + loghead->start;
	MTX_INIT(&vsl_mtx);
	loghead->starttime = TIM_real();
	memset(VSL_stats, 0, sizeof *VSL_stats);
}

/*--------------------------------------------------------------------*/

static int
vsl_goodold(int fd)
{
	struct shmloghead slh;
	int i;

	memset(&slh, 0, sizeof slh);	/* XXX: for flexelint */
	i = read(fd, &slh, sizeof slh);
	if (i != sizeof slh)
		return (0);
	if (slh.magic != SHMLOGHEAD_MAGIC)
		return (0);
	if (slh.hdrsize != sizeof slh)
		return (0);
	if (slh.start != sizeof slh + sizeof *params)
		return (0);
	/* XXX more checks */
	heritage.vsl_size = slh.size + slh.start;
	return (1);
}

static void
vsl_buildnew(const char *fn, unsigned size)
{
	struct shmloghead slh;
	int i;

	(void)unlink(fn);
	heritage.vsl_fd = open(fn, O_RDWR | O_CREAT, 0644);
	if (heritage.vsl_fd < 0) {
		fprintf(stderr, "Could not open %s: %s\n",
		    fn, strerror(errno));
		exit (1);
	}

	memset(&slh, 0, sizeof slh);
	slh.magic = SHMLOGHEAD_MAGIC;
	slh.hdrsize = sizeof slh;
	slh.size = size;
	slh.ptr = 0;
	slh.start = sizeof slh + sizeof *params;
	i = write(heritage.vsl_fd, &slh, sizeof slh);
	xxxassert(i == sizeof slh);
	heritage.vsl_size = slh.start + size;
	AZ(ftruncate(heritage.vsl_fd, (off_t)heritage.vsl_size));
}

void
VSL_MgtInit(const char *fn, unsigned size)
{
	int i;
	struct params *pp;

	i = open(fn, O_RDWR, 0644);
	if (i >= 0 && vsl_goodold(i)) {
		fprintf(stderr, "Using old SHMFILE\n");
		heritage.vsl_fd = i;
	} else {
		fprintf(stderr, "Creating new SHMFILE\n");
		(void)close(i);
		vsl_buildnew(fn, size);
	}

	loghead = mmap(NULL, heritage.vsl_size,
	    PROT_READ|PROT_WRITE,
	    MAP_HASSEMAPHORE | MAP_NOSYNC | MAP_SHARED,
	    heritage.vsl_fd, 0);
	xxxassert(loghead != MAP_FAILED);
	VSL_stats = &loghead->stats;
	pp = (void *)(loghead + 1);
	*pp = *params;
	params = pp;
}
