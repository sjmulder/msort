/*
 * msort.c
 * Copyright (c) 2018, Sijmen J. Mulder <ik@sjmulder.nl>
 *
 * msort is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * msort is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with msort. If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>

#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <err.h>

#define BUFSZ (64*1024)

struct work {
	/*
	 * Buffer slices to work on for current step. As mentioned in the
	 * readme, both a main and scratch buffer are required for merge sort.
	 * Roles are swapped at every step of recursion.
	 *
	 * Both data and scratch must be filled with with the unsorted data
	 * initially.
	 */
	char *data;
	char *scratch;
	size_t datasz;

	/*
	 * 'mask' and 'depth' are used for debug output. The mask starts out
	 * as all 1s, e.g. 111111. When the msort function divides the left
	 * and right working sets, the masks become 11110000 and 00001111
	 * respectively, and so on. This is printed to show progress
	 * visually.
	 */
	uint32_t mask;
	short depth;

	size_t njobs; /* job allowance */
};

/* proper function documentation found at point of definition */

/* utilities */
static ssize_t getfilesz(FILE *f);
static size_t copyfile(FILE *src, FILE *dst);
static char *readfilesh(FILE *f, size_t *lenp);

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
static void sfork_start(pid_t *pid, struct work *work);
static void sfork_wait(pid_t pid);

int
main(int argc, char **argv)
{
	struct work work;

	(void)argc;
	(void)argv;

	work.data = readfilesh(stdin, &work.datasz);
	/* replace \0 with \n for use with line* functions below */
	work.data[work.datasz] = '\n';

	debugf("setting up scratch buffer\n");

	work.scratch = mmap(NULL, work.datasz+1, PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANON, -1, 0);
	if (!work.scratch)
		err(1, "cannot mmap %zu byte anonymous file", work.datasz+1);

	memcpy(work.scratch, work.data, work.datasz+1);

	work.mask = 0xFFFFFFFF;
	work.depth = 0;
	work.njobs = 2;

	msort(&work);

	debugf("writing output\n");
	fwrite(work.data, work.datasz, 1, stdout);

	return 0;
}

/*
 * Sort work->data, leaving results in work->data. The work->scratch buffer
 * is used internally but both data and scratch must contain the full
 * unsorted buffer initially. See the struct work definition for field info.
 *
 * The work slice must exactly match line boundaries.
 */
static void
msort(struct work *work)
{
	struct work left;
	struct work right;
	pid_t pid;
	char *mid, maskstr[33];

	assert(work->data[work->datasz-1] == '\n');
	assert(work->scratch[work->datasz-1] == '\n');

	mid = linesmid(work->scratch, work->datasz);
	if (mid == work->scratch)
		return; /* just one line, we're done */

	assert(mid >= work->scratch);
	assert(mid < work->scratch + work->datasz);
	assert(*(mid-1) == '\n');

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

	/* mask becomes 0 when the slice gets too small; don't bother then */
	if (work->mask)
		getmaskstr(maskstr, work->mask);

	if (work->njobs > 1) {
		if (work->mask)
			debugf("sort  %s [fork]\n", maskstr);

		/* consume one job for fork, divide the rest */
		left.njobs = (work->njobs - 1) / 2;
		right.njobs = (work->njobs - 1) - left.njobs;

		sfork_start(&pid, &left);
		msort(&right);
		sfork_wait(pid);

		if (work->mask)
			debugf("merge %s [from fork]\n", maskstr);
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

/*
 * Merge two sorted buffers into a third sorted buffer. Both input buffers
 * must exactly match line boundaries.
 */
static void
merge(char *out, char *in1, char *in2, size_t sz1, size_t sz2)
{
	size_t len;

	assert(in1[sz1-1] == '\n');
	assert(in2[sz2-1] == '\n');

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

/*
 * sfork_start() and sfork_wait() together behave as msort(), but
 * asynchronously by using a child process to do the work. The child process
 * modifies work->data and work->scratch because those are shared memory.
 */
static void
sfork_start(pid_t *pid, struct work *work)
{
	switch ((*pid = fork())) {
	case -1:
		err(1, "fork");
	case 0:
		msort(work);
		exit(0);
	}
}

/*
 * Wait for the child process to finish. No further processing needs to be
 * done since the process updates the data in place.
 */
static void
sfork_wait(pid_t pid)
{
	int status;

	if (waitpid(pid, &status, 0) == -1)
		err(1, "waitpid");
	if (status)
		errx(1, "child failed");
}

/*
 * Returns the length of the given file, or -1 if that's not possible. Used
 * to optimise input file reading.
 */
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

/*
 * Write the rest of src to dst.
 */
static size_t
copyfile(FILE *src, FILE *dst)
{
	char buf[BUFSZ];
	size_t sz=0, n;

	while ((n = fread(buf, 1, BUFSZ, src))) {
		if (fwrite(buf, 1, n, dst) != n)
			err(1, "fwrite");
		sz += n;
	}

	if (ferror(src))
		err(1, "fread");

	return sz;
}

/*
 * Reads the entire file as a \0 terminated string in shared read/write
 * memory. Data is first copied to a temporary file if the file size cannot
 * be determined.
 *
 * The file size (excluding the terminating \0) is stored in lenp if given.
 */
static char *
readfilesh(FILE *f, size_t *lenp)
{
	FILE *tmp;
	ssize_t len;
	char *data;

	if ((len = getfilesz(f)) == -1) {
		/* unknown file size, copy to temp file first */

		debugf("writing input stream to temporary file\n");

		if (!(tmp = tmpfile()))
			err(1, "tmpfile");

		len = copyfile(f, tmp);
		fputc('\0', tmp);	/* terminating \0 */
		fflush(tmp);		/* flush libc's buffers before mmap */

		data = mmap(NULL, len+1, PROT_READ | PROT_WRITE, MAP_SHARED,
		    fileno(tmp), 0);
		if (!data)
			err(1, "cannot mmap %zu byte temporary file", len+1);

		debugf("read %zd bytes\n", len);
	} else {
		/* known file size, copy into anonymous mmap */

		debugf("reading %zd byte input into shared memory\n", len);

		data = mmap(NULL, len+1, PROT_READ | PROT_WRITE, MAP_SHARED |
		    MAP_ANON, -1, 0);
		if (!data)
			err(1, "cannot mmap %zd byte anonymous file", len+1);

		if (!fread(data, len, 1, f))
			err(1, "fread");

		data[len] = '\0';
	}

	assert(data[len] == '\0');

	if (lenp)
		*lenp = (size_t)len;
	return data;
}

/*
 * Compare \n-terminated strings, otherwise following strcmp() semantics.
 */
static int
linecmp(char *s1, char *s2)
{
	for (; ; ++s1, ++s2) {
		if (*s1 == '\n')
			return -1;
		else if (*s2 == '\n')
			return 1;
		else if (*s1 != *s2)
			return (int)(unsigned char)*s1 - (unsigned char)*s2;
	}
}

/*
 * Copy a \n-terminated string from src into dst, returning the number of
 * characters written.
 */
static size_t
linecpy(char *dst, char *src)
{
	size_t i;

	for (i=0; src[i] != '\n'; i++)
		dst[i] = src[i];

	dst[i++] = '\n';
	return i;
}

/*
 * Returns a pointer to the beginning of a line near the middle of the given
 * string. Search direction for line breaks is forward from the middle first,
 * backward from the middle second. If 's' itself is returned, there's only
 * one line.
 *
 * This is much more efficient than counting the number of lines at startup
 * and scanning all the way from the beginning of 's' for the n/2th line.
 *
 * Slice mutch exactly match line boundaries.
 */
static char *
linesmid(char *s, size_t sz)
{
	char *lf;

	assert(sz > 0);
	assert(s[sz-1] == '\n');

	/* search forwards */
	for (lf = s + sz/2; *lf != '\n'; lf++)
		;
	if (lf+1 < s+sz)
		return lf+1;

	/* backwards */
	for (lf = s + sz/2-1; *lf != '\n'; lf--)
		if (lf <= s)
			return s; /* not found */
	return lf+1;
}

/*
 * Write a message to stderr. Exists so that it may be disabled at some
 * point.
 */
static void
debugf(const char *fmt, ...)
{
	va_list ap;
	char buf[512];

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	fprintf(stderr, "[%6u] %s", (unsigned)getpid(), buf);
}

/*
 * Returns a new mask based representing the LEFT working set of the given
 * mask at the given depth. See the comment in struct work.
 */
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

/*
 * Returns a new mask based representing the RIGHT working set of the given
 * mask at the given depth. See the comment in struct work.
 */
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

/*
 * Convert the given integer mask to a \0 terminated string.
 */
static void
getmaskstr(char buf[33], uint32_t mask)
{
	int i;

	for (i=0; i<32; i++)
		buf[31-i] = ((mask >> i) & 1) ? '#' : '.';

	buf[32] = '\0';
}
