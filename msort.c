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

struct dataset {
	char *data;	/* input and output (start) */
	char *scratch;	/* for the first step, a copy of data */
	size_t nlines;	/* number of lines to sort */
	size_t datasz;	/* of this slice */

	uint32_t mask;	/* rough bitmap of the slice pos, e.g. 00110000 */
	short depth;	/* 0 for top level, 1 for first recur, etc */
};

struct knobs {
	size_t njobs;	/* job allowance */
	size_t nthreads;/* thread allowance */
};

struct threadctx {
	struct dataset *dataset;
	struct knobs *knobs;
};

struct sfork {
	pid_t pid;
	int readfd;	/* fd to read output from */
	char *dst;	/* where the output goes */
	size_t dstsz;	/* amount of output to read */
};

struct sthread {
	pthread_t tid;
	struct threadctx ctx;
};

/* utilities */
static void debugf(const char *fmt, ...);
static ssize_t getfilesz(FILE *f);
static char *readfile(FILE *f, size_t *lenp);

static void ds_maskstr(char buf[33], uint32_t mask);
static void ds_split(struct dataset *ds, struct dataset *left,
    struct dataset *right);

/* the real deal */
static void msort(struct dataset *ds, struct knobs *knobs);
static void merge(char *out, char *in1, char *in2, size_t n1, size_t n2);

static void sfork_start(struct sfork *sf, struct dataset *ds,
    struct knobs *knobs);
static void sfork_wait(struct sfork *sf);

static void sthread_start(struct sthread *st, struct dataset *ds,
    struct knobs *knobs);
static void *sthread_entry(void *arg);
static void sthread_wait(struct sthread *st);

int
main(int argc, char **argv)
{
	struct dataset ds;
	struct knobs knobs;
	size_t i;
	char *s;

	(void)argc;
	(void)argv;

	debugf("reading input\n");
	ds.data = readfile(stdin, &ds.datasz);

	debugf("processing lines\n");
	/* datasz-1 to ignore \n at end */
	for (ds.nlines=1, i=0; i < ds.datasz-1; i++) {
		if (ds.data[i] == '\n') {
			ds.nlines++;
			ds.data[i] = '\0';
		}
	}
	if (ds.data[ds.datasz-1] == '\n')
		ds.data[ds.datasz-1] = '\0';

	debugf("countred %zu lines\n", ds.nlines);
	debugf("creating scratch buffer\n");

	if (!(ds.scratch = malloc(ds.datasz)))
		err(1, "failed to alloc %zu bytes", ds.datasz);
	memcpy(ds.scratch, ds.data, ds.datasz);

	ds.mask = 0xFFFFFFFF;
	ds.depth = 0;
	knobs.njobs = 4;
	knobs.nthreads = 4;

	msort(&ds, &knobs);

	debugf("writing output\n");
	for (i=0, s=ds.data; i < ds.nlines; i++, s += strlen(s)+1) 
		puts(s);

	return 0;
}

static void
msort(struct dataset *ds, struct knobs *knobs)
{
	struct dataset left;
	struct dataset right;
	struct knobs lknobs;
	struct knobs rknobs;
	struct sfork sfork;
	struct sthread sthread;
	char maskstr[33];

	if (ds->nlines <= 1)
		return;

#if 0
	debugf("  first line: '%s'\n", ds->data);
#endif

	/* split the dataset into two, swapping data and scratch */
	ds_split(ds, &left, &right);

	lknobs = *knobs;
	rknobs = *knobs;

	if (ds->mask)
		ds_maskstr(maskstr, ds->mask);

	if (knobs->njobs > 1) {
		if (ds->mask)
			debugf("sort  %s [fork]\n", maskstr);

		/* consume one job for fork, divide the rest */
		lknobs.njobs = (knobs->njobs - 1) / 2;
		rknobs.njobs = (knobs->njobs - 1) - lknobs.njobs;

		sfork_start(&sfork, &left, &lknobs);
		msort(&right, &rknobs);
		sfork_wait(&sfork);

		if (ds->mask)
			debugf("merge %s [from fork]\n", maskstr);
	} else if (knobs->nthreads > 1) {
		if (ds->mask)
			debugf("sort  %s [thread]\n", maskstr);

		/* consume one thread for fork, divide the rest */
		lknobs.nthreads = (knobs->nthreads - 1) / 2;
		rknobs.nthreads = (knobs->nthreads - 1) - lknobs.nthreads;

		sthread_start(&sthread, &left, &lknobs);
		msort(&right, &rknobs);
		sthread_wait(&sthread);

		if (ds->mask)
			debugf("merge %s [from thread]\n", maskstr);
	} else {
		if (ds->mask)
			debugf("sort  %s\n", maskstr);

		msort(&left, &lknobs);
		msort(&right, &rknobs);

		if (ds->mask)
			debugf("merge %s\n", maskstr);
	}

	/* remember: left and right's .data are our scratch */
	merge(ds->data, left.data, right.data, left.nlines, right.nlines); 
}

static void
merge(char *out, char *in1, char *in2, size_t n1, size_t n2)
{
	size_t len;

	while (n1 || n2) {
		if (n1 && (!n2 || strcmp(in1, in2) <= 0)) {
#if 0
			debugf("  - %s (%zu, %zu)\n", in1, n1-1, n2);
#endif
			len = strlen(in1);
			memcpy(out, in1, len+1);
			out += len+1;
			in1 += len+1;
			n1--;
		} else {
#if 0
			debugf("  - %s (%zu, %zu)\n", in2, n1, n2-1);
#endif
			len = strlen(in2);
			memcpy(out, in2, len+1);
			out += len+1;
			in2 += len+1;
			n2--;
		}
	}
}


static void
sfork_start(struct sfork *sf, struct dataset *ds, struct knobs *knobs)
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

		msort(ds, knobs);

		debugf("  sending  %zu bytes\n", ds->datasz);
		nwritten = fwrite(ds->data, 1, ds->datasz, f);
		assert(nwritten == ds->datasz);

		fclose(f);
		exit(0);

	default:
		close(fds[1]);

		sf->pid = pid;
		sf->readfd = fds[0];
		sf->dst = ds->data;
		sf->dstsz = ds->datasz;
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
sthread_start(struct sthread *st, struct dataset *ds, struct knobs *knobs)
{
	st->ctx.dataset = ds;
	st->ctx.knobs = knobs;

	if (pthread_create(&st->tid, NULL, sthread_entry, &st->ctx) == -1)
		err(1, "pthread_create");
}

static void *
sthread_entry(void *arg)
{
	struct threadctx *ctx = arg;	

	msort(ctx->dataset, ctx->knobs);
	return NULL;
}

static void
sthread_wait(struct sthread *st)
{
	if (pthread_join(st->tid, NULL) == -1)
		err(1, "pthread_join");
}

static void
ds_maskstr(char buf[33], uint32_t mask)
{
	int i;

	for (i=0; i<32; i++)
		buf[31-i] = ((mask >> i) & 1) ? '#' : '.';

	buf[32] = '\0';
}

static void
ds_split(struct dataset *ds, struct dataset *left, struct dataset *right)
{
	size_t offset, line;

	left->data = ds->scratch;
	left->scratch = ds->data;
	left->nlines = ds->nlines / 2;
	left->depth = ds->depth + 1;

	right->data = ds->scratch;
	right->scratch = ds->data;
	right->nlines = ds->nlines - left->nlines;
	right->depth = ds->depth + 1;

	for (offset=0, line=0; line < left->nlines; line++)
		offset += strlen(&left->data[offset]) + 1;

	left->datasz = offset;
	right->data += offset;
	right->scratch += offset;
	right->datasz = ds->datasz - offset;

	switch (left->depth) {
		case 1: left->mask = ds->mask & 0xFFFF0000; break;
		case 2: left->mask = ds->mask & 0xFF00FF00; break;
		case 3: left->mask = ds->mask & 0xF0F0F0F0; break;
		case 4: left->mask = ds->mask & 0xCCCCCCCC; break;
		case 5: left->mask = ds->mask & 0xAAAAAAAA; break;
		default: left->mask = 0; break;
	}

	switch (right->depth) {
		case 1: right->mask = ds->mask & 0x0000FFFF; break;
		case 2: right->mask = ds->mask & 0x00FF00FF; break;
		case 3: right->mask = ds->mask & 0x0F0F0F0F; break;
		case 4: right->mask = ds->mask & 0x33333333; break;
		case 5: right->mask = ds->mask & 0x55555555; break;
		default: right->mask = 0; break;
	}

#if 0
	debugf("  split %zu+%zu(%zu) to %zu+%zu(%zu) and %zu+%zu(%zu)\n",
	    ds->scratch, ds->datasz, ds->nlines,
	    left->scratch, left->datasz, left->nlines,
	    right->scratch, right->datasz, right->nlines);
#endif
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
