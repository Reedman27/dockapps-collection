# Override anything on the command line or in the environment, e.g.
#   make CC=clang
#   make X11CFLAGS='-I/usr/X11R6/include' X11LIBS='-L/usr/X11R6/lib -lXext -lX11'
CC         ?= cc
CFLAGS     ?= -O2 -g
STD         = -std=c99
WARN        = -Wall -Wextra
PKG_CONFIG ?= pkg-config
X11CFLAGS  ?= $(shell $(PKG_CONFIG) --cflags x11 xext 2>/dev/null)
X11LIBS    ?= $(shell $(PKG_CONFIG) --libs x11 xext 2>/dev/null || echo "-lXext -lX11")
SANITIZE   ?=
PREFIX     ?= /usr/local
DESTDIR    ?=

ALLCFLAGS = $(CFLAGS) $(STD) $(WARN) $(X11CFLAGS) $(SANITIZE)
ALLLDFLAGS = $(LDFLAGS) $(SANITIZE)

all: wmmedia

wmmedia: wmmedia.c
	$(CC) $(ALLCFLAGS) $(ALLLDFLAGS) -o wmmedia wmmedia.c $(X11LIBS)

install: wmmedia
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/man/man1
	cp wmmedia $(DESTDIR)$(PREFIX)/bin/wmmedia
	chmod 755 $(DESTDIR)$(PREFIX)/bin/wmmedia
	cp wmmedia.1 $(DESTDIR)$(PREFIX)/share/man/man1/wmmedia.1
	chmod 644 $(DESTDIR)$(PREFIX)/share/man/man1/wmmedia.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/wmmedia $(DESTDIR)$(PREFIX)/share/man/man1/wmmedia.1

# parser/helper unit tests (they #include wmmedia.c, so sanitizers apply too)
unit: tests/parse_test.c wmmedia.c
	$(CC) $(ALLCFLAGS) $(ALLLDFLAGS) -o tests/parse_test tests/parse_test.c $(X11LIBS)
	./tests/parse_test

test: wmmedia unit
	sh tests/run.sh

# everything under AddressSanitizer + UBSan
asan:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=0 $(MAKE) SANITIZE="-fsanitize=address,undefined -fno-omit-frame-pointer" test
	$(MAKE) clean

clean:
	rm -f wmmedia tests/parse_test

.PHONY: all install uninstall unit test asan clean
