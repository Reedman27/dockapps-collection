dockapps-collection
Small, themeable dockapps for Window Maker, written in plain C.

The collection currently includes wmvol, a PipeWire volume controller, and wmmedia, a media player controller for MPRIS and MPD. Both use the same 64×64 LCD-inspired design, with shaped windows that let Window Maker's dock texture show through.
[!IMPORTANT]
AI-written software. Both applications were written by Claude (Anthropic) from human requirements and feedback. This includes their C source, tests, documentation, and man pages. Automated tests and selected real-world checks have been performed, but neither application has received a complete independent human code review. Each application README documents its test coverage and known gaps. Independent review, testing, and bug reports are welcome.

Applications
Application	Purpose	Runtime requirements	Platform
wmvol	PipeWire volume control with a popup mixer for output and input devices.	PipeWire, WirePlumber (wpctl)	Linux
wmmedia	Playback controls, track information, volume, and seeking.	playerctl for MPRIS, or mpc for MPD	POSIX C; tested on Linux


Shared design
- Made for Window Maker. Shaped 64×64 tiles leave the surrounding dock texture visible.
- Consistent appearance. Both applications use bevelled LCD panels and teal digits, with matching typography and visual states.
- Theme-aware options. Use the default accent or specify a fixed colour; wmmedia can also derive its accent from the Window Maker theme.
- Small and focused. Plain C and Xlib, without a widget toolkit or background daemon.
- Responsive event loops. Both applications use poll() and short-lived subprocesses rather than waiting indefinitely for external commands.
- Independently buildable. Each application has its own source, Makefile, README, man page, tests, and license.
wmvol
<details>
<summary><strong>PipeWire volume control</strong></summary>A 64×64 volume tile with a popup mixer for output and input devices. Adjust volume, toggle mute, and select default devices without launching a full desktop settings application.

Controls: scroll to adjust output volume, middle-click to mute, right-click to cycle output devices, and Ctrl + right-click to launch the full mixer. The popup provides sliders, mute controls, and device selection.

Runtime: PipeWire, WirePlumber, and wpctl. pactl is optional and enables event-driven updates; without it, wmvol polls for changes.

[Read the wmvol README](wmvol/README.md).
</details>wmmedia
<details>
<summary><strong>Media playback control</strong></summary>A 64×64 media tile with previous, play/pause, and next controls; scrolling title and artist text; elapsed or remaining time; and a progress bar that supports seeking.

It supports MPRIS players through playerctl (including Spotify, mpv, VLC, and browsers that expose MPRIS) or MPD through mpc.

[Read the wmmedia README](wmmedia/README.md).
</details>Build from source
Each application builds separately. Start with the application directory you want:
git clone https://github.com/Reedman27/dockapps-collection.git
cd dockapps-collection/wmvol
make
./wmvol -w
Use ./wmmedia -w when building wmmedia. The -w option opens a normal preview window; it is not the docked mode.

After installing the relevant dependencies, install with:
sudo make install
The default installation prefix is /usr/local. Both Makefiles support PREFIX and DESTDIR; see the individual README for details. Use doas instead of sudo where appropriate.
Gentoo overlay
This repository includes a Gentoo overlay with live (-9999) ebuilds for both applications.

Install the repository management tools and add the overlay:
sudo emerge --ask --noreplace app-eselect/eselect-repository dev-vcs/git
sudo eselect repository add dockapps git https://github.com/Reedman27/dockapps-collection.git
sudo emaint sync -r dockapps
The live ebuilds do not have stable keywords, so accept them locally:
echo "x11-plugins/wmvol **" | sudo tee -a /etc/portage/package.accept_keywords/dockapps
echo "x11-plugins/wmmedia **" | sudo tee -a /etc/portage/package.accept_keywords/dockapps
Then install either or both:
sudo emerge --ask x11-plugins/wmvol x11-plugins/wmmedia
wmmedia has an optional mpd USE flag that adds the mpc dependency:
echo "x11-plugins/wmmedia mpd" | sudo tee -a /etc/portage/package.use/dockapps
To update the overlay and rebuild installed live ebuilds after new commits:
sudo emaint sync -r dockapps
sudo emerge --ask @live-rebuild
See each application's README for its complete dependency and configuration information.
Window Maker setup
Start the application without -w, then move its tile into the dock using your Window Maker modifier (Alt by default) while dragging.

Once it is docked, use the tile's menu to enable Start when Window Maker is started. Alternatively, add the application command to:
~/GNUstep/Library/WindowMaker/autostart
Use one startup method per application. Enabling both can start duplicate instances.
Tests
Run tests from the relevant application directory:
cd wmvol
make test
make asan
cd wmmedia
make test
make asan
The test suites include unit tests and Xvfb-based integration tests. Their additional dependencies differ; consult each README for the exact requirements and coverage. Passing automated tests does not guarantee compatibility with every audio setup, player, Window Maker theme, display configuration, or operating system.
Repository layout
dockapps-collection/
├── README.md
├── LICENSE
├── metadata/                 Gentoo repository metadata
├── profiles/                 Gentoo repository profiles
├── x11-plugins/              Gentoo ebuilds
├── wmvol/
│   ├── README.md
│   ├── LICENSE
│   ├── CHANGELOG.md
│   ├── Makefile
│   ├── wmvol.c
│   ├── wmvol.1
│   ├── docs/
│   └── tests/
└── wmmedia/
    ├── README.md
    ├── LICENSE
    ├── CHANGELOG.md
    ├── Makefile
    ├── wmmedia.c
    ├── wmmedia.1
    ├── docs/
    ├── tests/
    └── objc/                 Original Objective-C implementation
Each application is self-contained and can be copied, built, and packaged independently.
Reporting issues and contributing
Bug reports, compatibility reports, code reviews, and pull requests are welcome through the issue tracker.

For a useful report, include:
- Distribution and version
- Window Maker version
- The application and its version
- Relevant runtime dependency versions (wpctl, PipeWire, WirePlumber, playerctl, or MPD/`mpc`)
- Steps to reproduce and terminal output
Reports from real desktops, different themes, multi-monitor systems, and non-Linux platforms are particularly useful.

Packages for other distributions are welcome, including XBPS, AUR, Nix, Debian, and RPM-based packaging.
License
The collection is released under the MIT License. Each application also includes its own license file so it can be distributed independently.
