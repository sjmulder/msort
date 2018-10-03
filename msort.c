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
static void debugf(const char *fmt, ...);
static ssize_t getfilesz(FILE *f);
static char *readfile(FILE *f, size_t *lenp);
static char *strsmid(char *strs, size_t sz);

static void getmaskstr(char buf[33], uint32_t mask);
static void splitwork(struct work *ds, struct work *left, struct work *right);

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
	size_t nlines, i;
	char *s;

	(void)argc;
	(void)argv;

	debugf("reading input\n");
	work.data = readfile(stdin, &work.datasz);

	debugf("processing lines\n");
	/* datasz-1 to ignore \n at end */
	for (nlines=1, i=0; i < work.datasz-1; i++) {
		if (work.data[i] == '\n') {
			nlines++;
			work.data[i] = '\0';
		}
	}
	if (work.data[work.datasz-1] == '\n')
		work.data[work.datasz-1] = '\0';

	debugf("countred %zu lines\n", nlines);
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
	for (i=0, s=work.data; i < nlines; i++, s += strlen(s)+1)
		puts(s);

	return 0;
}

static void
msort(struct work *work)
{
	struct work left;
	struct work right;
	struct sfork sfork;
	struct sthread sthread;
	char maskstr[33];

	/* split the dataset into two, swapping data and scratch */
	splitwork(work, &left, &right);

	/*
	 * If no more data left or right after split it means there's only one
	 * line and we are done.
	 */
	if (!left.datasz || !right.datasz)
		return;

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
		if (sz1 && (!sz2 || strcmp(in1, in2) <= 0)) {
			len = strlen(in1);
			memcpy(out, in1, len+1);
			out += len+1;
			in1 += len+1;
			sz1 -= len+1;
		} else {
			len = strlen(in2);
			memcpy(out, in2, len+1);
			out += len+1;
			in2 += len+1;
			sz2 -= len+1;
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

static void
getmaskstr(char buf[33], uint32_t mask)
{
	int i;

	for (i=0; i<32; i++)
		buf[31-i] = ((mask >> i) & 1) ? '#' : '.';

	buf[32] = '\0';
}

static void
splitwork(struct work *work, struct work *left, struct work *right)
{
	char *mid;

	mid = strsmid(work->scratch, work->datasz);

	left->data = work->scratch;
	left->scratch = work->data;
	left->datasz = mid - work->scratch;
	left->depth = work->depth + 1;
	left->njobs = 0;
	left->nthreads = 0;

	right->data = mid;
	right->scratch = work->data + (mid - work->scratch);
	right->datasz = work->datasz - (mid - work->scratch);
	right->depth = work->depth + 1;
	right->njobs = 0;
	right->nthreads = 0;

	switch (left->depth) {
		case 1: left->mask = work->mask & 0xFFFF0000; break;
		case 2: left->mask = work->mask & 0xFF00FF00; break;
		case 3: left->mask = work->mask & 0xF0F0F0F0; break;
		case 4: left->mask = work->mask & 0xCCCCCCCC; break;
		case 5: left->mask = work->mask & 0xAAAAAAAA; break;
		default: left->mask = 0; break;
	}

	switch (right->depth) {
		case 1: right->mask = work->mask & 0x0000FFFF; break;
		case 2: right->mask = work->mask & 0x00FF00FF; break;
		case 3: right->mask = work->mask & 0x0F0F0F0F; break;
		case 4: right->mask = work->mask & 0x33333333; break;
		case 5: right->mask = work->mask & 0x55555555; break;
		default: right->mask = 0; break;
	}
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

static char *
strsmid(char *strs, size_t sz)
{
	char *p;

	if (sz<2)
		return strs;

	/* try to find a string boundary from the middle forward */
	if ((p = memchr(strs + sz/2, '\0', sz-(sz/2)-1)))
		return p+1;

	/* from the middle backward */
	if ((p = memrchr(strs, '\0', sz/2)))
		return p+1;

	/* nothing, so it's just a single string */
	return strs;
}
