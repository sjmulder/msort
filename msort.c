#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>

#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <getopt.h>
#include <err.h>

#define BUFSZ (64*1024)

struct work {
	char *data;	/* input and output (start) */
	char *scratch;	/* for the first step, a copy of data */
	size_t datasz;	/* of this slice */

	uint32_t mask;	/* rough bitmap of the slice pos, e.g. 00110000 */
	short depth;	/* 0 for top level, 1 for first recur, etc */

	size_t njobs;	/* job allowance */
	size_t nthreads;/* thread allowance */
};

struct sfork {
	pid_t pid;
	int readfd;	/* fd to read output from */
	char *dst;	/* where the output goes */
	size_t dstsz;	/* amount of output to read */
};

struct sthread {
	pthread_t tid;
};

/* utilities */
static ssize_t getfilesz(FILE *f);
static char *readfile(FILE *f, size_t *lenp);

static int linecmp(char *s1, char *s2);
static size_t linecpy(char *dst, char *src);
static char *linesmid(char *s, size_t sz);

/* debugging */
static void debugf(const char *fmt, ...);
static uint32_t maskleft(uint32_t mask, int depth);
static uint32_t maskright(uint32_t mask, int depth);
static void getmaskstr(char buf[33], uint32_t mask);

/* the real deal */
static void msort(struct work *work);
static void merge(char *out, char *in1, char *in2, size_t sz1, size_t sz2);

static void sfork_start(struct sfork *sf, struct work *work);
static void sfork_wait(struct sfork *sf);

static void sthread_start(struct sthread *st, struct work *work);
static void *sthread_entry(void *arg);
static void sthread_wait(struct sthread *st);

int
main(int argc, char **argv)
{
	struct work work;

	(void)argc;
	(void)argv;

	debugf("reading input\n");
	work.data = readfile(stdin, &work.datasz);
	if (work.data[work.datasz-1] == '\n')
		work.data[work.datasz-1] = '\0';

	debugf("creating scratch buffer\n");

	if (!(work.scratch = malloc(work.datasz)))
		err(1, "failed to alloc %zu bytes", work.datasz);
	memcpy(work.scratch, work.data, work.datasz);

	work.mask = 0xFFFFFFFF;
	work.depth = 0;
	work.njobs = 2;
	work.nthreads = 1;

	msort(&work);

	debugf("writing output\n");
	fwrite(work.data, work.datasz, 1, stdout);

	return 0;
}

static void
msort(struct work *work)
{
	struct work left;
	struct work right;
	struct sfork sfork;
	struct sthread sthread;
	char *mid, maskstr[33];

	mid = linesmid(work->scratch, work->datasz);
	if (mid == work->scratch)
		return; /* just one line, we're done */

	left = *work;
	left.data = work->scratch;
	left.datasz = mid - work->scratch;
	left.scratch = work->data;
	left.mask = maskleft(work->mask, ++left.depth);

	right = *work;
	right.data = mid;
	right.datasz = work->datasz - left.datasz;
	right.scratch = work->data + left.datasz;
	right.mask = maskright(work->mask, ++right.depth);

	if (work->mask)
		getmaskstr(maskstr, work->mask);

	if (work->njobs > 1) {
		if (work->mask)
			debugf("sort  %s [fork]\n", maskstr);

		/* consume one job for fork, divide the rest */
		left.njobs = (work->njobs - 1) / 2;
		right.njobs = (work->njobs - 1) - left.njobs;

		left.nthreads = work->nthreads;
		right.nthreads = work->nthreads;

		sfork_start(&sfork, &left);
		msort(&right);
		sfork_wait(&sfork);

		if (work->mask)
			debugf("merge %s [from fork]\n", maskstr);
	} else if (work->nthreads > 1) {
		if (work->mask)
			debugf("sort  %s [thread]\n", maskstr);

		/* consume one thread for fork, divide the rest */
		left.nthreads = (work->nthreads - 1) / 2;
		right.nthreads = (work->nthreads - 1) - left.nthreads;

		sthread_start(&sthread, &left);
		msort(&right);
		sthread_wait(&sthread);

		if (work->mask)
			debugf("merge %s [from thread]\n", maskstr);
	} else {
		if (work->mask)
			debugf("sort  %s\n", maskstr);

		msort(&left);
		msort(&right);

		if (work->mask)
			debugf("merge %s\n", maskstr);
	}

	/* remember: left and right's .data are our scratch */
	merge(work->data, left.data, right.data, left.datasz, right.datasz);
}

static void
merge(char *out, char *in1, char *in2, size_t sz1, size_t sz2)
{
	size_t len;

	while (sz1 || sz2) {
		if (sz1 && (!sz2 || linecmp(in1, in2) <= 0)) {
			len = linecpy(out, in1);
			out += len;
			in1 += len;
			sz1 -= len;
		} else {
			len = linecpy(out, in2);
			out += len;
			in2 += len;
			sz2 -= len;
		}
	}
}


static void
sfork_start(struct sfork *sf, struct work *work)
{
	pid_t pid;
	int fds[2];
	FILE *f;
	size_t nwritten;

	if (pipe(fds) == -1)
		err(1, "pipe");

	switch ((pid = fork())) {
	case -1:
		err(1, "fork");

	case 0:
		close(fds[0]);
		if (!(f = fdopen(fds[1], "w")))
			err(1, "fdopen");

		msort(work);

		debugf("  sending  %zu bytes\n", work->datasz);
		nwritten = fwrite(work->data, 1, work->datasz, f);
		assert(nwritten == work->datasz);

		fclose(f);
		exit(0);

	default:
		close(fds[1]);

		sf->pid = pid;
		sf->readfd = fds[0];
		sf->dst = work->data;
		sf->dstsz = work->datasz;
		return;
	}
}

static void
sfork_wait(struct sfork *sf)
{
	FILE *f;
	size_t nread;
	int status;

	if (!(f = fdopen(sf->readfd, "r")))
		err(1, "fdopen");

	nread = fread(sf->dst, 1, sf->dstsz, f);
	debugf("  received %zu bytes\n", nread);
	assert(nread = sf->dstsz);

	if (waitpid(sf->pid, &status, 0) == -1)
		err(1, "waitpid");
	if (status)
		errx(1, "child failed");
}

static void
sthread_start(struct sthread *st, struct work *work)
{
	int errn;

	if ((errn = pthread_create(&st->tid, NULL, sthread_entry, work)))
		errc(1, errn, "pthread_create");
}

static void *
sthread_entry(void *arg)
{
	struct work *work = arg;

	msort(work);
	return NULL;
}

static void
sthread_wait(struct sthread *st)
{
	int errn;

	if ((errn = pthread_join(st->tid, NULL)))
		errc(1, errn, "pthread_join");
}

static ssize_t
getfilesz(FILE *f)
{
	off_t pos, end;

	if ((pos = ftello(f)) == -1)
		return -1;

	if (fseeko(f, 0, SEEK_END) == -1)
		err(1, "seeko");
	if ((end = ftello(f)) == -1)
		err(1, "ftello");
	if (fseeko(f, pos, SEEK_SET) == -1)
		err(1, "fseeko");

	return (size_t)end;
}

static char *
readfile(FILE *f, size_t *lenp)
{
	ssize_t filesz;
	size_t len=0, bufsz=0, nread;
	char *buf=NULL;

	if ((filesz = getfilesz(f)) != -1) {
		debugf("file is %zd bytes\n", filesz);

		if (!(buf = malloc(filesz+1)))
			err(1, "cannot malloc %zd bytes", filesz+1);

		len = fread(buf, 1, filesz, f);
	} else {
		while (1) {
			bufsz += BUFSZ;
			if (!(buf = realloc(buf, bufsz)))
				err(1, "cannot realloc %zu bytes", bufsz);

			nread = fread(buf+len, 1, bufsz-len, f);
			debugf("read %zu bytes\n", nread);
			len += nread;
			if (nread < bufsz-len)
				break;
		}

		/* we always end up with room for trailing \0 */
	}

	debugf("read total of %zu bytes\n", len);

	buf[len] = '\0';
	if (lenp)
		*lenp = len;
	return buf;
}

static int
linecmp(char *s1, char *s2)
{
	for (; ; ++s1, ++s2) {
		if (!s1 || *s1 == '\n')
			return -1;
		else if (!s2 || *s2 == '\n')
			return 1;
		else if (*s1 != *s2)
			return *s1 - *s2;
	}
}

static size_t
linecpy(char *dst, char *src)
{
	size_t i;

	for (i=0; src[i] && src[i] != '\n'; i++)
		dst[i] = src[i];

	dst[i++] = '\n';
	return i;
}

static char *
linesmid(char *s, size_t sz)
{
	char *p;

	if (sz<2)
		return s;

	/* try to find a string boundary from the middle forward */
	if ((p = memchr(s + sz/2, '\n', sz-(sz/2)-1)))
		return p+1;

	/* from the middle backward */
	if ((p = memrchr(s, '\n', sz/2)))
		return p+1;

	/* nothing, so it's just a single string */
	return s;
}

static void
debugf(const char *fmt, ...)
{
	va_list ap;
	char buf[512];

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	fprintf(stderr, "[%6u:%11u] %s", (unsigned)getpid(),
	    (unsigned)pthread_self(), buf);
}

static uint32_t
maskleft(uint32_t mask, int depth)
{
	switch (depth) {
		case 1: return mask & 0xFFFF0000;
		case 2: return mask & 0xFF00FF00;
		case 3: return mask & 0xF0F0F0F0;
		case 4: return mask & 0xCCCCCCCC;
		case 5: return mask & 0xAAAAAAAA;
		default: return 0;
	}
}

static uint32_t
maskright(uint32_t mask, int depth)
{
	switch (depth) {
		case 1: return mask & 0x0000FFFF;
		case 2: return mask & 0x00FF00FF;
		case 3: return mask & 0x0F0F0F0F;
		case 4: return mask & 0x33333333;
		case 5: return mask & 0x55555555;
		default: return 0;
	}
}

static void
getmaskstr(char buf[33], uint32_t mask)
{
	int i;

	for (i=0; i<32; i++)
		buf[31-i] = ((mask >> i) & 1) ? '#' : '.';

	buf[32] = '\0';
}
