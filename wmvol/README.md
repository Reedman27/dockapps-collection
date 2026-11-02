# wmvol

A lightweight PipeWire volume dockapp for [Window Maker](https://www.windowmaker.org/), with a popup mixer for output and input devices.

> [!IMPORTANT]
> **AI-written software.** wmvol was written entirely by Claude (Anthropic) from human requirements and feedback. The C source, tests, man page, and this README were AI-generated. A human has built and run it on Void Linux with Window Maker, but the source has not received a complete line-by-line human review. Independent code review and testing are welcome; see [Project status](#project-status).

![wmvol tile in a Window Maker dock (6×) with its popup mixer](docs/screenshot.png)

<sub>The wmvol tile docked in Window Maker (6×), with the popup mixer open. Pixel-exact tile images: [8×](docs/tile-8x.png) or [original size](docs/tile.png).</sub>

## Overview

wmvol puts everyday audio controls in a 64×64 Window Maker dock tile. The tile displays the default output volume. Open its popup mixer to adjust output and input levels, toggle mute, and choose default audio devices.

The tile uses Xlib drawing and XShape masking, allowing the surrounding Window Maker dock texture to remain visible. It shares its LCD-inspired appearance with [wmmedia](../wmmedia/README.md).

## Features

- **Output and input controls:** adjust volume and mute the default devices.
- **Device selection:** choose default output and input devices from the popup.
- **Quick mouse controls:** use the wheel for volume, middle-click for mute, and right-click to cycle output devices.
- **Full mixer shortcut:** launch `pavucontrol` or another configured mixer with Ctrl + right-click.
- **Window Maker integration:** a shaped tile designed to sit naturally in the dock.
- **Low overhead:** plain C with Xlib, Xft, and XShape; approximately 4 MB of RAM and negligible idle CPU, as measured by the project.
- **Responsive updates:** use `pactl subscribe` for event-driven refreshes when available, with polling as a fallback.
- **Simple audio backend:** all audio discovery and control is handled through WirePlumber's `wpctl`.

## Requirements

wmvol targets Linux.

| Purpose | Requirements |
|---|---|
| Build | C compiler, `make`, `pkg-config`, X11, Xext, and Xft development headers |
| Runtime | PipeWire, WirePlumber, and `wpctl` |
| Optional | `pactl` for event-driven updates; `pavucontrol` for the full mixer |

### Install dependencies

<details>
<summary><strong>Void Linux</strong></summary>

```sh
doas xbps-install -S base-devel pkg-config libX11-devel libXext-devel libXft-devel
doas xbps-install -S pipewire wireplumber pulseaudio-utils pavucontrol
```
</details>

<details>
<summary><strong>Debian / Ubuntu</strong></summary>

```sh
sudo apt install build-essential pkg-config libx11-dev libxext-dev libxft-dev \
    wireplumber pipewire-pulse pulseaudio-utils pavucontrol
```
</details>

<details>
<summary><strong>Fedora</strong></summary>

```sh
sudo dnf install gcc make pkgconf-pkg-config libX11-devel libXext-devel libXft-devel \
    wireplumber pipewire-pulseaudio pulseaudio-utils pavucontrol
```
</details>

<details>
<summary><strong>Arch Linux</strong></summary>

```sh
sudo pacman -S --needed base-devel pkgconf libx11 libxext libxft
sudo pacman -S --needed pipewire wireplumber pipewire-pulse libpulse pavucontrol
```
</details>

<details>
<summary><strong>Gentoo</strong></summary>

The compiler is normally provided by `@system`. Install the build dependencies:

```sh
sudo emerge --ask --noreplace dev-build/make virtual/pkgconfig \
    x11-libs/libX11 x11-libs/libXext x11-libs/libXft
```

Enable PipeWire's `sound-server` USE flag and install the audio components:

```sh
echo "media-video/pipewire sound-server" | sudo tee -a /etc/portage/package.use/pipewire
sudo emerge --ask media-video/pipewire media-video/wireplumber \
    media-libs/libpulse media-sound/pavucontrol
```

If your Portage tree uses `sys-devel/make` rather than `dev-build/make`, use the package name available in your tree. You can also install wmvol through this repository's Gentoo overlay; see the [collection README](../README.md#gentoo-overlay).
</details>

<details>
<summary><strong>NixOS</strong></summary>

Provide the build and runtime dependencies through your system configuration or a development shell. wmvol is not currently packaged in nixpkgs.
</details>

## Build and install

Clone the repository and build wmvol:

```sh
git clone https://github.com/Reedman27/dockapps-collection.git
cd dockapps-collection/wmvol
make
```

You can preview the tile in a normal window before adding it to the dock:

```sh
./wmvol -w
```

Install using the default `/usr/local` prefix:

```sh
sudo make install
```

Use `doas` instead of `sudo` if that is your system's privilege tool.

### Installation options

```sh
sudo make install PREFIX=/usr
make install DESTDIR=/tmp/stage PREFIX=/usr
sudo make uninstall
```

For uninstalling, use the same `PREFIX` that was used during installation. `DESTDIR` is intended for staged packaging installs.

### Gentoo

The repository includes a live ebuild:

```sh
sudo eselect repository add dockapps git https://github.com/Reedman27/dockapps-collection.git
sudo emaint sync -r dockapps
echo "x11-plugins/wmvol **" | sudo tee -a /etc/portage/package.accept_keywords/dockapps
sudo emerge --ask x11-plugins/wmvol
```

For the complete overlay setup, including wmmedia, see the [collection README](../README.md#gentoo-overlay).

## Window Maker setup

1. Start wmvol without `-w`:

   ```sh
   wmvol &
   ```

2. Hold **Alt** (the default Window Maker modifier) and drag the tile into the dock.
3. Open the docked tile's menu and enable **Start when Window Maker is started**.

Alternatively, add `wmvol &` to:

```text
~/GNUstep/Library/WindowMaker/autostart
```

Choose one startup method. Enabling both can launch two instances.

## Controls

### Dock tile

| Input | Action |
|---|---|
| Left-click | Open or close the popup mixer |
| Mouse wheel | Adjust output volume |
| Shift + mouse wheel | Adjust output volume in 1% steps |
| Middle-click | Toggle output mute |
| Right-click | Switch to the next output device |
| Ctrl + mouse wheel | Adjust input volume |
| Ctrl + middle-click | Toggle input mute |
| Ctrl + right-click | Open the full mixer |

When the output device name is too long for the display, it scrolls across the LCD.

The tile handles its own mouse input. To move it in Window Maker, hold the configured modifier (Alt by default) while dragging.

### Popup mixer

| Input | Action |
|---|---|
| Click speaker or microphone icon | Toggle mute |
| Drag a volume slider | Set volume |
| Click a device | Make it the default |
| Mouse wheel | Adjust volume |
| Esc or click outside | Close the popup |

Increasing the volume of a muted device automatically unmutes it, matching the behaviour of KDE's audio controls.

## Command-line options

```text
-display DPY   X display to use (-d also works)
-w             Run in a normal window instead of dock mode (preview only)
-s N           Mouse-wheel volume step, 1–100 (default: 5)
-l N           Maximum volume, 10–200% (default: 100)
-m CMD         Full mixer command (default: pavucontrol)
-f FONT        Popup font as an Xft pattern (default: sans-10)
-fg RRGGBB     LCD accent colour (default: 20b2aa)
-D PREFIX      Write PREFIX-tile.ppm and PREFIX-popup.ppm, then exit
-v             Print version and exit
-h             Print usage and exit
```

The `-l` option sets the highest volume wmvol will request. Values above 100% can amplify beyond the normal software volume range.

## Troubleshooting

**The tile will not drag into the dock.** Hold Alt (or your configured Window Maker modifier) while dragging.

**The tile appears as a normal window.** You probably started it with `-w`. Run `wmvol` without that option for dock mode.

**Changes made in other audio applications appear late.** Without `pactl`, wmvol checks for changes by polling. Install `pulseaudio-utils` (or the appropriate package providing `pactl`) to enable event-driven updates.

**The full mixer does not open.** wmvol launches `pavucontrol` by default. Install it or choose another command with `-m`.

**The device list looks incorrect.** wmvol parses the output of `wpctl status`. When reporting this issue, include the raw command output and your WirePlumber version.

## How it works

wmvol is a single-threaded C application built around `poll()` and non-blocking subprocesses.

- **Device discovery:** `wpctl status` supplies the output and input device lists and their current state.
- **Volume changes:** updates are debounced and sent as absolute values, avoiding one subprocess per wheel notch.
- **Change notifications:** `pactl subscribe` triggers refreshes when available; otherwise wmvol polls.
- **Rendering:** Xlib draws the tile, Xft renders popup text, and XShape makes the area around the LCD panels transparent.
- **Responsive interaction:** the interface updates optimistically and discards stale refresh results so sliders do not jump backwards while being dragged.

## Testing

Run the test suite from this directory:

```sh
make test
make asan
```

`make test` runs parser unit tests, including mutation fuzzing, followed by the Xvfb integration test. `make asan` runs the same tests with AddressSanitizer and UndefinedBehaviorSanitizer.

The integration test runs wmvol under Xvfb with a fake `wpctl`, sends synthetic mouse and keyboard input through XTest, and checks the exact commands wmvol issues.

The integration test requires `xvfb-run`, Python 3, and the XTest development headers. Package examples:

```sh
sudo pacman -S --needed xorg-server-xvfb python python-pytest libxtst
sudo apt install xvfb python3 libxtst-dev
sudo dnf install xorg-x11-server-Xvfb python3 libXtst-devel
```

Passing the tests does not guarantee compatibility with every PipeWire/WirePlumber configuration, Window Maker theme, or multi-monitor setup.

## Project status

wmvol was written entirely by Claude (Anthropic) from human requirements and feedback. The C source, tests, man page, and README were AI-generated.

A human has built and run wmvol on Void Linux with Window Maker. The source has not received a complete line-by-line human review. Automated checks include warning-free builds, cppcheck, unit tests, Xvfb integration tests, and ASan/UBSan runs against a simulated `wpctl` backend and synthetic input.

Independent code review and testing are welcome.

## Packaging and contributing

wmvol currently builds from source, and Gentoo users can install it through the repository overlay. Packages for other distributions are welcome, including AUR, XBPS, NixOS, Debian, and Fedora RPMs.

Please report bugs, compatibility issues, and packaging work through the [GitHub issue tracker](https://github.com/Reedman27/dockapps-collection/issues). Include your distribution, Window Maker version, PipeWire and WirePlumber versions, reproduction steps, and relevant terminal output.

## License

[MIT](LICENSE).
