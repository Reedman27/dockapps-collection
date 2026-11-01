# dockapps-collection

A collection of my [Window Maker](https://www.windowmaker.org/) dockapps.

Dockapps are small tiles that easily fit into a dock in your window manager, but each one could be considered a complete configuration application, amusement, monitoring tool, or a combination of features.

Each tile is generally larger than a tray icon but smaller than an application, so they offer a way to do detailed operations while staying out of the way.

The instructions below complement the README file in each app's subdirectory.

Generally, you should consult the individual README before running the commands in this file.

## Contents

| Dockapp | Language | What it does |
|---|---|---|
| [wmvol](wmvol/) | C (Xlib, Xft, XShape) | PipeWire volume tile with a KDE-style popup mixer: output/input volume, mute, device selection. |
| [wmmedia](wmmedia/) | Objective-C (no Foundation/GNUstep) | Media-control tile: previous / play-pause / next, title and artist tickers, time and a seekable progress bar. Works with `playerctl` (MPRIS) or `mpc` (MPD). |

Both share the same LCD look, so they match each other in the dock.

## Layout

Like the upstream [dockapps](https://github.com/window-maker/dockapps) repository, every dockapp lives in its own subdirectory and carries its own README, license, changelog, man page, Makefile, docs and tests. Nothing at the top level is shared, so each app builds on its own.

```
dockapps-collection/
├── readme.md
├── LICENSE
├── .gitignore
├── wmvol/
│   ├── README.md  LICENSE  CHANGELOG.md
│   ├── Makefile   wmvol.c  wmvol.1
│   ├── docs/      tests/
└── wmmedia/
    ├── README.md  LICENSE  CHANGELOG.md
    ├── Makefile   wmmedia.m  wmmedia.1
    ├── docs/      tests/
```

## Support
(issue reporting, pull request instructions, official links, etc.)

The issue tracker is at <https://github.com/Reedman27/dockapps-collection/issues>.

## Compile

Enter the dockapp's directory and build it there. Both apps use a plain `Makefile`, there is no `configure` step.

```bash
git clone https://github.com/Reedman27/dockapps-collection.git
cd dockapps-collection/wmvol      # or wmmedia
make
```

Preview it in an ordinary window before docking (`-w` is for testing only, it is not a dockapp):

```bash
./wmvol -w
./wmmedia -w
```

### Dependencies

**wmvol**: C compiler, make, pkg-config, libX11 / libXext / libXft headers. At runtime: PipeWire and WirePlumber (`wpctl`); optionally `pactl` (event-driven updates) and `pavucontrol` (full mixer).

**wmmedia**: Objective-C compiler with runtime (`libobjc`), make, libX11 / libXext headers. At runtime: `playerctl` (default) or `mpc`.

| Distribution | Install |
|---|---|
| Void | `doas xbps-install -S base-devel pkg-config libX11-devel libXext-devel libXft-devel gcc-objc pipewire wireplumber pulseaudio-utils pavucontrol playerctl` |
| Debian / Ubuntu | `sudo apt install build-essential gobjc pkg-config libx11-dev libxext-dev libxft-dev wireplumber pipewire-pulse pulseaudio-utils pavucontrol playerctl` |
| Fedora | `sudo dnf install gcc gcc-objc make pkgconf-pkg-config libX11-devel libXext-devel libXft-devel wireplumber pipewire-pulseaudio pulseaudio-utils pavucontrol playerctl` |
| Arch | `sudo pacman -S --needed base-devel pkgconf libx11 libxext libxft pipewire wireplumber pipewire-pulse libpulse pavucontrol playerctl` |
| Gentoo | `sudo emerge -aq --noreplace dev-build/make virtual/pkgconfig x11-libs/libX11 x11-libs/libXext x11-libs/libXft media-video/pipewire media-video/wireplumber media-libs/libpulse media-sound/pavucontrol media-sound/playerctl` (PipeWire needs the `sound-server` USE flag, and GCC needs the `objc` USE flag) |

For other distributions, or NixOS, see each app's README.

## Install

Generally, at this point you can run, inside the dockapp's directory:

```bash
sudo make install        # PREFIX=/usr/local by default
```

unless `make` had errors, or the app's README has special install instructions. `make uninstall` removes it again.

## Window Maker setup

Run the dockapp without `-w`:

```bash
wmvol &
wmmedia &
```

Each tile first shows up as a loose icon near a screen corner, like every dockapp. Alt-drag it into the dock, then enable *Start when Window Maker is started* from its menu (or add the command to `~/GNUstep/Library/WindowMaker/autostart`: one method, not both).

## Tests

```bash
cd wmvol && make test
cd wmmedia && make test
```

Some tests need Xvfb. See each app's README for the details.

## License

The whole repository, including every dockapp in it, is released under the [MIT License](LICENSE). Each dockapp also keeps its own `LICENSE` file in its directory (also MIT), so it stays properly licensed if copied out on its own.
