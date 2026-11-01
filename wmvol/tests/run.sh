#!/bin/sh
# Integration test: runs wmvol against a fake wpctl inside Xvfb and drives it
# with synthetic mouse/keyboard input (XTest). Needs: xvfb-run, python3, libXtst-dev.
set -u
cd "$(dirname "$0")/.."
ROOT=$(pwd)
if [ -z "${WMVOL_TEST_INNER:-}" ]; then
    WMVOL_TEST_INNER=1 exec xvfb-run -a -s "-screen 0 1280x800x24" sh "$0"
fi
T=$(mktemp -d); trap 'kill $PID 2>/dev/null; rm -rf "$T"' EXIT
mkdir "$T/bin"; cp tests/fake-wpctl "$T/bin/wpctl"; cp tests/fake-pactl "$T/bin/pactl"
export PATH="$T/bin:$PATH" FAKE_WPCTL_STATE="$T/state.json" FAKE_WPCTL_LOG="$T/log" FAKE_DIR="$T"
cc -O2 -o "$T/xinput" tests/xinput.c -lX11 -lXtst || exit 1
X="$T/xinput"; fails=0

# odd LC_ALL on purpose: wmvol must not leak its own LC_ALL=C into the mixer it launches
LC_ALL=en_US.UTF-8 ./wmvol -w -m "echo \"LC_ALL=[\$LC_ALL]\" > $T/mixer-env" & PID=$!
sleep 1.2
: > "$T/log"

check() { # name pattern  (polls up to 2s so a busy machine can't make it flaky)
    i=0
    while [ $i -lt 20 ]; do
        grep -Eq "$2" "$T/log" && { echo "ok   $1"; return; }
        sleep 0.1; i=$((i+1))
    done
    echo "FAIL $1 (wanted /$2/)"; fails=$((fails+1))
}

# --- tile (window sits at 0,0 without a WM) ---
$X move 32 32 click 4 click 4 sleep 300
check "wheel up raises output volume"      "set-volume -l 1.00 54 0.85"
: > "$T/log"; $X click 5 sleep 300
check "wheel down lowers output volume"    "set-volume -l 1.00 54 0.80"
: > "$T/log"; $X click 2 sleep 300
check "middle click mutes output"          "set-mute 54 1"
: > "$T/log"; $X click 2 sleep 300
check "middle click unmutes output"        "set-mute 54 0"
: > "$T/log"; $X ctrl down click 4 ctrl up sleep 300
check "ctrl+wheel raises input volume"     "set-volume -l 1.00 56 1.00"
: > "$T/log"; $X click 3 sleep 300
check "right click cycles output device"   "set-default 55"

: > "$T/mixer-env"; $X ctrl down click 3 ctrl up sleep 400
check_file() { # name file pattern
    i=0
    while [ $i -lt 20 ]; do
        grep -Eq "$3" "$2" 2>/dev/null && { echo "ok   $1"; return; }
        sleep 0.1; i=$((i+1))
    done
    echo "FAIL $1 (wanted /$3/ in $2)"; fails=$((fails+1))
}
check_file "mixer keeps the user's locale (no LC_ALL=C leak)" "$T/mixer-env" 'LC_ALL=\[en_US.UTF-8\]'
check_file "helper starts with SIGPIPE default" "$T/pactl-sigpipe" 'SIGPIPE=default'

# --- popup (opens to the right of the tile at x=70) ---
: > "$T/log"; $X click 1 sleep 400
: > "$T/log"
$X move 150 150 click 1 sleep 300
check "popup: pick third output device"    "set-default 61"
: > "$T/log"; $X move 150 281 click 1 sleep 300
check "popup: pick second input device"    "set-default 62"
: > "$T/log"; $X move 98 54 click 1 sleep 300
check "popup: output mute button"          "set-mute 61 1"
: > "$T/log"; $X move 150 54 press 1 move 200 54 move 250 54 release 1 sleep 400
check "popup: slider drag sets volume"     "set-volume -l 1.00 61 0\\.[0-9]+"
: > "$T/log"; $X move 150 54 press 1 move 400 54 release 1 sleep 400
check "popup: slider clamps to limit"      "set-volume -l 1.00 61 1.00"
: > "$T/log"; $X move 150 190 click 4 sleep 300
check "popup: wheel on input section"      "set-volume -l 1.00 62"
$X esc sleep 300
: > "$T/log"; $X move 32 32 click 4 sleep 300
check "escape closed popup (tile responds)" "set-volume"

kill -0 $PID 2>/dev/null && echo "ok   still running" || { echo "FAIL crashed"; fails=$((fails+1)); }

# helpers must die with wmvol (even on SIGKILL): no orphaned `pactl subscribe`
HP=$(cat "$T/pactl-pid" 2>/dev/null)
kill -9 $PID 2>/dev/null; sleep 0.5
# (a killed child can linger as a zombie if PID 1 doesn't reap, so check the state, not kill -0)
HS=$(awk '/^State/ { print $2 }' "/proc/$HP/status" 2>/dev/null)
if [ -n "$HP" ] && { [ -z "$HS" ] || [ "$HS" = Z ]; }; then echo "ok   helper dies with wmvol"
else echo "FAIL helper $HP orphaned after wmvol died"; fails=$((fails+1)); kill "$HP" 2>/dev/null; fi
PID=
[ $fails -eq 0 ] && echo "ALL PASSED" || { echo "$fails FAILED"; exit 1; }
