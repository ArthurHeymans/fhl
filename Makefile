CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

all: fhl

fhl: fhl.c fhl.h
	$(CC) $(CFLAGS) -o $@ fhl.c -lm

fhl-asan: fhl.c fhl.h
	$(CC) -O1 -g -fsanitize=address,undefined -o $@ fhl.c -lm

test: fhl-asan
	./tests/run.sh ./fhl-asan

clean:
	rm -f fhl fhl-asan

.PHONY: all test clean
