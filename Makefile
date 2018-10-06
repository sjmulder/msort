CFLAGS += -O2 -g
CFLAGS += -Wall -Wextra -Werror
# uncomment to disable assertions and debug output
#CFLAGS += -DNDEBUG

DICT = /usr/share/dict/words

all: msort

test: msort test.in test.expect
	time ./msort <test.in >test.out
	diff -q test.expect test.out

bench: msort large.in
	time ./msort <large.in >/dev/null

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

.PHONY: all test bench clean distclean
