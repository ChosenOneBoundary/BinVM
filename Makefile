CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
PREFIX  ?= /usr/local

all: binvm

binvm: binvm.c
	$(CC) $(CFLAGS) -o $@ $<

test: binvm
	@sh tests/smoke.sh

install: binvm
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 binvm $(DESTDIR)$(PREFIX)/bin/binvm

clean:
	rm -f binvm examples/*.bin

.PHONY: all test install clean
