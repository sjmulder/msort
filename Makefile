CFLAGS += -g
CFLAGS += -O2
CFLAGS += -Wall -Wextra -Werror

LDFLAGS += -pthread

DICT = /usr/share/dict/words

all: msort

test: msort test.in test.expect
	time ./msort <test.in >test.out
	diff -q test.expect test.out

bench: msort large.in
	time ./msort <large.in >/dev/null

watch-test:
	ls Makefile msort.c | entr -c make clean test

watch-bench:
	ls Makefile msort.c | entr -c make clean bench

test.in: $(DICT)
	sort -R <$(DICT) >small.in
	cat small.in small.in small.in small.in small.in small.in \
	    small.in small.in small.in small.in small.in small.in \
	    small.in small.in small.in small.in small.in small.in \
	    small.in small.in small.in small.in small.in small.in \
	    small.in small.in small.in small.in small.in small.in \
	  >test.in
	rm small.in

test.expect: test.in
	sort <test.in >test.expect

large.in: test.in
	cat test.in test.in test.in test.in test.in test.in test.in test.in \
	  >large.in

clean:
	rm -f msort *.out

distclean: clean
	rm -f *.in *.expect

.PHONY: all test bench watch-test watch-bench clean distclean
