# Changelog

## 2.1.0

* Ported from Objective-C to plain C99 (`wmmedia.c`). Same behaviour, same options, pixel-identical tile; the unit, integration and ASan/UBSan tests all pass unchanged in substance.
* No `libobjc` or `gcc[objc]` any more: builds with any C compiler, which makes packaging for distributions much simpler.
* Makefile honours `CC`, `CFLAGS`, `LDFLAGS`, `PREFIX` and `DESTDIR` from the environment, and finds X11 through pkg-config (falls back to `-lXext -lX11`).
* The 2.0.0 Objective-C source is kept in `objc/`.

## 2.0.0

* New tile layout in wmvol's style: the same three LCD boxes, grey bevels and teal digits; state icon, elapsed/remaining time, title and artist tickers, LCD transport keys and a 10-segment progress bar.
* Progress bar with click and drag to seek (`playerctl position`, `mpc seek N%`).
* Elapsed time with local extrapolation between polls; click the time to show remaining (`-r` to start that way).
* The wheel flashes the volume in the time box and the bar for 1.5 s, like wmvol's flash line.
* Artist and title are separate (`playerctl` fields; for MPD the format is requested explicitly, with `Artist - Title` and filename fallbacks).
* Default colours are now wmvol's. Following the Window Maker theme is opt-in with `-t` (it was the default in 1.x); `-fg` always wins. `-notheme` still accepted.
* Keys, wheel and bar are disabled (and drawn disabled) while no player is running.
* README: Debian/Ubuntu install line no longer asks for the non-existent `libobjc-dev`.

## 1.0.0

* First version: three buttons, ticker, status line, wheel volume, theme following.
