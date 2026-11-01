CC       ?= cc
CFLAGS   ?= -O2 -g
CFLAGS   += -Wall -Wextra -std=gnu11 $(SANITIZE)
LDFLAGS  += $(SANITIZE)
PKGS      = x11 xft xext
CFLAGS   += $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS)) -lm
PREFIX   ?= /usr/local
DESTDIR  ?=

all: wmvol

wmvol: wmvol.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: wmvol
	install -Dm755 wmvol $(DESTDIR)$(PREFIX)/bin/wmvol
	install -Dm644 wmvol.1 $(DESTDIR)$(PREFIX)/share/man/man1/wmvol.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/wmvol $(DESTDIR)$(PREFIX)/share/man/man1/wmvol.1

test: wmvol unit
	sh tests/run.sh

# parser + scheduler unit tests (they #include wmvol.c, so they get sanitizers too)
unit: tests/parse_test.c wmvol.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o tests/parse_test tests/parse_test.c $(LDLIBS)
	./tests/parse_test

# build + run every test under AddressSanitizer + UBSan
asan:
	$(MAKE) clean
	# fontconfig/Xft leave one-time allocations at exit; that is not ours to chase
	ASAN_OPTIONS=detect_leaks=0 $(MAKE) SANITIZE="-fsanitize=address,undefined -fno-omit-frame-pointer" test
	$(MAKE) clean

clean:
	rm -f wmvol tests/parse_test

.PHONY: all install uninstall test unit asan clean
