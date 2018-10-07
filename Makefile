CFLAGS += -O2 -g
CFLAGS += -Wall -Wextra -Werror
# uncomment to disable assertions and debug output
#CFLAGS += -DNDEBUG

DICT = /usr/share/dict/words

all: msort

test: msort tests/words.in tests/words.expect
	./msort <tests/1-line.in         | diff -u tests/1-line.expect         -
	./msort <tests/2-lines.in        | diff -u tests/2-lines.expect        -
	./msort <tests/3-lines.in        | diff -u tests/3-lines.expect        -
	./msort <tests/empty.in          | diff -u tests/empty.expect          -
	./msort <tests/duplicates.in     | diff -u tests/duplicates.expect     -
	./msort <tests/casing.in         | diff -u tests/casing.expect         -
	./msort <tests/trailing-blank.in | diff -u tests/trailing-blank.expect -
	./msort <tests/unterminated.in   | diff -u tests/unterminated.expect   -
	./msort <tests/umlaut.in         | diff -u tests/umlaut.expect         -
	cat tests/3-lines.in | ./msort   | diff -u tests/3-lines.expect        -
	./msort <tests/words.in          | diff -u tests/words.expect          -

bench: msort tests/bench.in
	time ./msort <tests/bench.in >/dev/null

tests/words.in:
	sort -R <$(DICT) >tests/words.in

tests/words.expect: tests/words.in
	LC_ALL=C sort <tests/words.in >tests/words.expect

tests/bench.in: tests/words.in
	rm -f tests/bench.in
	for n in `seq 200`; do cat tests/words.in >>tests/bench.in; done

clean:
	rm -f msort

distclean: clean
	rm -f tests/words.in tests/words.expect tests/bench.in

.PHONY: all test bench clean distclean
