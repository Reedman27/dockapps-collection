# Changelog

## 1.0.1
- Fix: helper processes (`wpctl`, `pactl subscribe`) inherited an ignored `SIGPIPE`, and nothing
  stopped them when wmvol died, so a restart of Window Maker could leave an orphaned `pactl subscribe`
  running forever. Helpers now get default signals, die with wmvol (`PR_SET_PDEATHSIG`), and an X
  connection loss cleans them up.
- Fix: wmvol exported `LC_ALL=C` globally, so the mixer it launched (e.g. pavucontrol) lost the user's
  locale. `LC_ALL=C` is now applied only to the `wpctl`/`pactl` helpers whose output is parsed.
- Fix: if a `wpctl status` call failed after the user had already changed something, the refresh was
  never rescheduled and polling stopped until the next `pactl` event.
- Fix: a `wpctl status` dump larger than the buffer limit is now rejected instead of parsed truncated;
  a failed `realloc` no longer leaks the old buffer.
- Fix: popup now closes itself if it cannot grab the pointer/keyboard (another client held the grab),
  instead of getting stuck on screen; stale hover highlight after a device-list relayout.
- Options are validated: `-s abc`, `-fg zzz` or a missing argument now print an error instead of
  silently becoming 0. `-l` outside 10-200 and `-s` outside 1-100 are rejected (were silently clamped).
- Tests: parser unit tests (+ mutation fuzzing), regression tests for every fix above, and `make asan`
  (ASan + UBSan over unit and integration tests); CI runs it.
- Docs: real in-dock screenshot, crisp integer upscale of the tile (`docs/tile-8x.png`), man page updated.
- Added SPDX license identifier to the source.
- Docs: Void dependency list no longer names `pipewire-pulse` (not a separate package on Void).

## 1.0.0
- Initial release.
- 64x64 shaped dock tile (black LCD boxes, wmclock/wmcpu palette) that inherits the Window Maker theme.
- Popup mixer: output/input volume sliders, mute buttons, default device pickers, mixer launcher.
- wpctl backend, event-driven refresh via `pactl subscribe`, optimistic debounced volume writes.
- Options: `-w`, `-s`, `-l`, `-m`, `-f`, `-fg`, `-D`.
- Integration test suite (Xvfb + XTest + fake wpctl) and GitHub Actions CI.
