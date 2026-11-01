# wmmedia

**A media-control dockapp for [Window Maker](https://www.windowmaker.org/), written in plain C, in the same LCD style as wmvol.**

![wmmedia docked in Window Maker (4x)](docs/screenshot.png)

<sub>Docked in a real Window Maker session (Xvfb), 4x. Pixel-exact close-up: [docs/tile-8x.png](docs/tile-8x.png) (8x) or [docs/tile.png](docs/tile.png) (1x).</sub>

A 64×64 tile with **previous / play-pause / next** keys, **title and artist** tickers, the **elapsed (or remaining) time** and a **progress bar you can click or drag to seek**. It uses wmvol's three black bevelled LCD boxes and teal digits, cut out with a shape mask so Window Maker draws its own dock-tile texture around them and the tile matches the rest of your dock.

```
 ┌──────┐ ┌────────┐
 │  ▶   │ │  1:23  │   state icon · elapsed / remaining time
 └──────┘ └────────┘
 ┌──────────────────┐
 │ AROUND THE WORLD │   title    (scrolls when it does not fit)
 │ DAFT PUNK        │   artist
 │ [|◀◀] [❚❚] [▶▶|] │   previous / play-pause / next
 │ ▮▮▮▮▯▯▯▯▯▯       │   progress bar (click or drag = seek)
 └──────────────────┘
```

![The four states: playing, paused, stopped, no player](docs/states.png)

<sub>Playing (teal) · paused (amber) · stopped · no player (amber, keys disabled).</sub>

## Controls

| Input | Action |
|---|---|
| Left click a key | previous / play-pause / next (fires on release; drag off the key to cancel) |
| Left click or drag on the bar | seek (a drag shows the target time live and sends one seek on release) |
| Left click on the time | toggle elapsed ⇄ remaining |
| Mouse wheel | volume ± step (default 5 %); the time box and the bar show the volume for 1.5 s |
| Shift + wheel | volume ± 1 % |
| Middle click | stop |

Right click is left alone: Window Maker uses it for the dock menu. To move the tile in the dock, hold your WM modifier (Alt) while dragging, because the tile handles its own clicks.

## Backends

Players are controlled by running a command, so anything those tools support works:

* **`playerctl`** (default): any MPRIS player: Spotify, mpv, VLC, Firefox, Chromium, … Pick one with `-p NAME`; otherwise playerctl uses the first one it finds.
* **`mpc`** (`-b mpc`): MPD. Seeking goes in whole percent steps (that is what `mpc seek` offers).

## Building

Needs a C compiler, make, and the libX11 and libXext headers (pkg-config is used if present). Nothing else.

```sh
# Debian / Ubuntu
sudo apt install build-essential pkg-config libx11-dev libxext-dev playerctl
# Fedora
sudo dnf install gcc make pkgconf-pkg-config libX11-devel libXext-devel playerctl
# Void
doas xbps-install -S base-devel pkg-config libX11-devel libXext-devel playerctl
# Arch
sudo pacman -S --needed base-devel pkgconf libx11 libxext playerctl
# Gentoo
sudo emerge -aq --noreplace virtual/pkgconfig x11-libs/libX11 x11-libs/libXext media-sound/playerctl

make
./wmmedia &           # a loose dock icon appears; Alt-drag it into the dock
sudo make install     # PREFIX=/usr/local by default

./wmmedia -w          # preview as an ordinary window (NOT docked)
```

The Makefile honours `CC`, `CFLAGS`, `LDFLAGS`, `PREFIX` and `DESTDIR` from the environment or the command line:

```sh
make X11CFLAGS='-I/usr/X11R6/include' X11LIBS='-L/usr/X11R6/lib -lXext -lX11'
make CC=clang
make install DESTDIR=/tmp/stage PREFIX=/usr   # for packagers
```

## Window Maker setup

```sh
wmmedia &        # no -w: -w makes a plain window, which is not a dockapp
```

The tile first shows up as a loose icon near a screen corner, like every dockapp. Alt-drag it into the dock, then enable *Start when Window Maker is started* from its menu (or add `wmmedia &` to `~/GNUstep/Library/WindowMaker/autostart`: one method, not both).

## Colours

The default is **wmvol's look**: teal digits (`20b2aa`), amber for paused / no player, wmvol's grey bevels. Two options change it:

* `-fg RRGGBB` pins a different LCD colour.
* `-t` takes the colour from your Window Maker theme instead. wmmedia reads `~/GNUstep/Defaults/WindowMaker`, picks the theme's most vivid colour (from `FTitleBack`, `IconTitleBack`, `MenuTitleBack`, `MenuTextBack`, `UTitleBack`) as the accent and derives the LCD digits, glass and dim shades from it. Gray themes give a gray LCD, and on amber/gold themes "paused" switches to blue so it still stands out. The file is re-checked about once a second, so applying a theme recolours the tile live. `-c FILE` reads a different defaults file. If the file is missing or has no usable colour you get the teal. `-fg` always wins over `-t`.

All 36 themes shipped with Window Maker are exercised by the unit tests.

## Options

```
-display DPY   X display
-w             run in a normal window instead of docking (preview only)
-b BACKEND     playerctl (default) or mpc
-p PLAYER      playerctl player name (e.g. spotify, mpv)
-s N           wheel volume step, 1-50 percent (default 5)
-r             show remaining time instead of elapsed (click the time to toggle)
-fg RRGGBB     LCD colour (default 20b2aa)
-t             take the LCD colour from the Window Maker theme
-c FILE        Window Maker defaults file for -t
-D PREFIX      debug: write PREFIX-tile.ppm after the first poll and exit
```

`-notheme` from wmmedia 1.x is still accepted (it is now the default).

## How it works

* One single-threaded `poll()` loop. The player is queried by a short-lived `playerctl metadata` / `mpc status` subprocess every ~2 s (3 s when no player is running) and 150 ms after every action. Subprocesses are non-blocking and killed after 4 s, so the UI never waits on a player.
* Between polls the clock is **extrapolated locally** and redrawn once a second, so the time and bar move smoothly without polling every second.
* Wheel steps are coalesced into one relative volume command. The UI updates optimistically, and a poll that started before your last click is discarded, so the display does not flicker back.
* Pure POSIX: `fork/execvp/pipe/poll/waitpid/sigaction/clock_gettime`. No `prctl`, `pipe2` or other Linux-only calls. Helpers are short-lived, so nothing is left behind if the dockapp dies.
* One C file, split into small structs with `thing_verb()` functions: `Backend` (playerctl / mpc command lines and parsers), `Job` (non-blocking subprocess), `Tile` (renderer) and `App` (events and scheduling). Parsers and hit-testing are plain functions so they can be unit tested.
* Titles are folded to what the 3×5 pixel font can show: upper-case ASCII, every non-ASCII character becomes `?`.

## Tests

```sh
make test     # unit + fuzz tests, then the Xvfb integration test (needs xvfb-run, xdotool)
make asan     # the same, under AddressSanitizer + UBSan
```

The unit tests cover the parsers (including malformed input and a mutation fuzzer), clocks, hit areas, backend command lines and the theme code. The integration test drives the real binary with synthetic mouse events against fake `playerctl`/`mpc` scripts, asserts the exact commands issued (including seeks and drags) and checks what is drawn (ImageMagick `import`, otherwise those checks are skipped).

## Status

Checked in this form (v2.1.0):

* Builds without warnings with GCC 13 (`-std=c99 -Wall -Wextra`); the whole test suite also passes under ASan + UBSan. The C port renders pixel-identical tiles to the 2.0.0 Objective-C version (checked with `-D` dumps for playing, paused, stopped, remaining-time, custom-colour, stream and non-ASCII states).
* **Real MPD** (`mpd` + `mpc`, null output): metadata, next, play/pause, wheel volume, seek and stop through real mouse events.
* **Real `playerctl` 2.4.1 over D-Bus** against a minimal MPRIS player: confirmed that position and length are microseconds, and that play/pause, absolute seek (`SetPosition`) and volume arrive as expected.
* **Real Window Maker** (Ubuntu's `wmaker`, under Xvfb): the tile docks with Alt-drag and the keys work while loose in the corner.

**Not yet tried:** a real music player (Spotify, mpv, VLC, …) on a physical desktop, multi-monitor setups, and the BSDs. Reports welcome.

Known limits: updates are polled, not event-driven (a `playerctl --follow` child would need Linux's `prctl` to be cleaned up reliably, which is not POSIX); players that report no length (streams) show the time but an empty bar; some players do not support seeking, and then a bar click does nothing.

## License

MIT. Portions (tile shaping, pixel font, LCD style) derived from wmvol, also MIT; see `LICENSE`.

The original Objective-C implementation (v2.0.0) is kept in [`objc/`](objc/) for reference.
