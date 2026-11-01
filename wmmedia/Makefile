# POSIX make. Override anything on the command line, e.g.
#   make CC=clang OBJCFLAGS='-fobjc-runtime=gnustep-2.0' OBJCLIBS=-lobjc2
#   make X11CFLAGS='-I/usr/X11R6/include' X11LIBS='-L/usr/X11R6/lib -lX11 -lXext'
CC         = cc
CFLAGS     = -O2 -g
STD        = -std=c99
WARN       = -Wall -Wextra
OBJCFLAGS  =
OBJCLIBS   = -lobjc
X11CFLAGS  =
X11LIBS    = -lXext -lX11
SANITIZE   =
PREFIX     = /usr/local
DESTDIR    =

ALLFLAGS = $(CFLAGS) $(STD) $(WARN) $(OBJCFLAGS) $(X11CFLAGS) $(SANITIZE)

all: wmmedia

wmmedia: wmmedia.m
	$(CC) $(ALLFLAGS) -o wmmedia wmmedia.m $(X11LIBS) $(OBJCLIBS)

install: wmmedia
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/man/man1
	cp wmmedia $(DESTDIR)$(PREFIX)/bin/wmmedia
	chmod 755 $(DESTDIR)$(PREFIX)/bin/wmmedia
	cp wmmedia.1 $(DESTDIR)$(PREFIX)/share/man/man1/wmmedia.1
	chmod 644 $(DESTDIR)$(PREFIX)/share/man/man1/wmmedia.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/wmmedia $(DESTDIR)$(PREFIX)/share/man/man1/wmmedia.1

# parser/helper unit tests (they #include wmmedia.m, so sanitizers apply too)
unit: tests/parse_test.m wmmedia.m
	$(CC) $(ALLFLAGS) -o tests/parse_test tests/parse_test.m $(X11LIBS) $(OBJCLIBS)
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
