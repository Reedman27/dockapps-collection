# wmmedia, original Objective-C version (2.0.0)

This is the original implementation, kept for reference and for anyone who likes it.
The maintained version is the plain C one in the parent directory (`wmmedia.c`), which
behaves identically and renders a pixel-identical tile.

Needs an Objective-C compiler with its runtime (`libobjc`), libX11 and libXext; no Foundation or GNUstep.

```sh
cd objc
make            # builds ./wmmedia from wmmedia.m
make test       # unit + Xvfb integration tests (needs xvfb-run, xdotool)
```
