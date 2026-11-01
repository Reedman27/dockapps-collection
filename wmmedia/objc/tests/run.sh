#!/bin/sh
# Integration test: runs wmmedia in a plain window inside Xvfb against fake
# playerctl/mpc scripts, drives it with synthetic mouse events (xdotool) and
# checks the exact commands it issues and what it draws.
# Needs: xvfb-run, xdotool, ImageMagick (import/compare; those checks are skipped without it).
set -u
cd "$(dirname "$0")/.."
if [ -z "${WMMEDIA_TEST_INNER:-}" ]; then
    WMMEDIA_TEST_INNER=1 exec xvfb-run -a -s "-screen 0 1280x800x24" sh "$0"
fi
T=$(mktemp -d); PID=
trap '[ -n "$PID" ] && kill $PID 2>/dev/null; rm -rf "$T"' EXIT
mkdir "$T/bin"; cp tests/fake-playerctl "$T/bin/playerctl"; cp tests/fake-mpc "$T/bin/mpc"
export FAKE_STATE="$T/state" FAKE_LOG="$T/log"
FULLPATH="$T/bin:$PATH"
fails=0
HAVE_IM=0; command -v import >/dev/null 2>&1 && command -v compare >/dev/null 2>&1 && HAVE_IM=1

PLAYING='Playing\t0.55\tDaft Punk\tAround the World\t83000000\t240000000\n'
PAUSED='Paused\t0.55\tBoards\tRoygbiv\t83000000\t240000000\n'   # short title: it fits, so it never scrolls

ok()   { echo "ok   $1"; }
fail() { echo "FAIL $1"; fails=$((fails+1)); }
check() { # name pattern  (polls up to 2s so a busy machine can't make it flaky)
    i=0
    while [ $i -lt 20 ]; do
        grep -Eq "$2" "$T/log" && { ok "$1"; return; }
        sleep 0.1; i=$((i+1))
    done
    fail "$1 (wanted /$2/)"
}
absent() { # name pattern: must NOT be in the log after a short wait
    sleep 0.5
    if grep -Eq "$2" "$T/log"; then fail "$1 (found /$2/)"; else ok "$1"; fi
}
mouse() { xdotool mousemove "$1" "$2"; shift 2; [ $# -gt 0 ] && xdotool "$@"; }
snap() { import -window root -crop 64x64+0+0 +repage "$T/$1.png"; }   # the tile, at 0,0
same() { cmp -s "$T/$1.png" "$T/$2.png"; }
start() { : > "$T/log"; PATH="$FULLPATH" ./wmmedia -w "$@" & PID=$!; sleep 1; }
stop()  { kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=; }

# ---------------- playerctl backend (window sits at 0,0 without a WM) ----------------
printf "$PLAYING" > "$FAKE_STATE"
start
check "polls the player on startup"        "metadata --format .*position.*mpris:length"
: > "$T/log"; mouse 12 41 click 1
check "prev key"                           "^previous$"
: > "$T/log"; mouse 50 41 click 1
check "next key"                           "^next$"
: > "$T/log"; mouse 31 41 click 1
check "play/pause key"                     "^play-pause$"
sleep 0.4
grep -q '^Paused' "$FAKE_STATE" && ok "fake player toggled" || fail "fake player not toggled"
: > "$T/log"; mouse 32 41 click 2
check "middle click stops"                 "^stop$"
printf "$PLAYING" > "$FAKE_STATE"; sleep 2.3
: > "$T/log"; mouse 32 10 click 4
check "wheel up raises volume"             "^volume 0\\.05\\+$"
: > "$T/log"; mouse 32 10 click 5
check "wheel down lowers volume"           "^volume 0\\.05-$"
: > "$T/log"; xdotool click --repeat 3 --delay 1 4
check "fast wheel is coalesced"            "^volume 0\\.15\\+$"
: > "$T/log"; xdotool keydown shift click 4 keyup shift
check "shift+wheel is a 1% step"           "^volume 0\\.01\\+$"
: > "$T/log"; mouse 12 41 mousedown 1; xdotool mousemove 60 62 mouseup 1
absent "press then drag off = cancelled"   "^previous$"
: > "$T/log"; mouse 32 10 click 1; mouse 12 10 click 1; mouse 32 26 click 1; mouse 32 31 click 1
absent "LCD / ticker clicks do nothing"    "^(previous|next|play-pause|stop|position)"

# ---------------- seeking: length is 240 s ----------------
: > "$T/log"; mouse 31 53 click 1
check "click mid-bar seeks to 120 s"       "^position 120$"
: > "$T/log"; mouse 7 53 click 1
check "click bar start seeks to 0"         "^position 0$"
: > "$T/log"; mouse 55 53 click 1
check "click bar end seeks to the end"     "^position 240$"
: > "$T/log"; mouse 19 53 mousedown 1; xdotool mousemove 25 53 mousemove 31 53 mouseup 1
check "drag scrubs and commits on release" "^position 120$"
sleep 0.3
[ "$(grep -c '^position' "$T/log")" -eq 1 ] && ok "drag sends one seek, not one per pixel" || fail "drag sent $(grep -c '^position' "$T/log") seeks"
: > "$T/log"; mouse 31 53 mousedown 1; xdotool mousemove 120 53 mouseup 1
check "dragging past the end clamps to 100%" "^position 240$"
: > "$T/log"; mouse 31 53 mousedown 1; xdotool mousemove 0 53 mouseup 1
check "dragging past the start clamps to 0" "^position 0$"

# ---------------- what it draws ----------------
if [ $HAVE_IM = 1 ]; then
    stop; printf "$PAUSED" > "$FAKE_STATE"; start
    sleep 0.5; snap p0
    mouse 40 10 click 1; sleep 0.3; snap p1
    same p0 p1 && fail "clicking the time did not toggle remaining" || ok "click on the time shows remaining"
    mouse 40 10 click 1; sleep 0.3; snap p2
    same p0 p2 && ok "second click goes back to elapsed" || fail "second click did not restore elapsed"
    mouse 32 10 click 4; sleep 0.3; snap pv
    same p0 pv && fail "wheel did not flash the volume" || ok "wheel flashes the volume"
    sleep 2.2; snap pa
    same p0 pa && ok "volume flash clears itself" || fail "volume flash stuck"
    stop; printf "$PLAYING" > "$FAKE_STATE"; start
    snap t0; sleep 1.5; snap t1
    same t0 t1 && fail "clock did not advance while playing" || ok "clock advances between polls while playing"
    mouse 31 41 mousedown 1; sleep 0.2; snap down; xdotool mouseup 1
    same t1 down && fail "pressed key looks the same as idle" || ok "pressed key lights up"
fi
kill -0 $PID 2>/dev/null && ok "still running" || fail "crashed"
kill $PID; i=0; while kill -0 $PID 2>/dev/null && [ $i -lt 20 ]; do sleep 0.1; i=$((i+1)); done
kill -0 $PID 2>/dev/null && { fail "ignores SIGTERM"; kill -9 $PID; } || ok "exits on SIGTERM"
PID=

# ---------------- no player: tile shows it, keys are dead ----------------
printf 'none\n' > "$FAKE_STATE"
start
: > "$T/log"; mouse 12 41 click 1; mouse 31 41 click 1; mouse 50 41 click 1; mouse 32 10 click 4; mouse 31 53 click 1
absent "no player: keys, wheel and bar do nothing" "^(previous|next|play-pause|stop|volume|position)"
stop

# ---------------- screenshot dump: all four states render and differ ----------------
shotp() { PATH="$FULLPATH" ./wmmedia -w -D "$T/$1" ; }
printf "$PLAYING" > "$FAKE_STATE";                                         shotp sPlay; rc=$?
[ $rc -eq 0 ] && head -c 11 "$T/sPlay-tile.ppm" | head -2 | tr '\n' ' ' | grep -q 'P6 64 64' \
    && ok "-D writes a 64x64 PPM" || fail "-D screenshot (rc=$rc)"
[ "$(wc -c < "$T/sPlay-tile.ppm")" -eq $((64*64*3 + 13)) ] && ok "PPM has the right size" || fail "PPM size"
printf "$PAUSED" > "$FAKE_STATE";                                          shotp sPause
printf 'Stopped\t0.5\t\t\t\t\n' > "$FAKE_STATE";                          shotp sStop
printf 'none\n' > "$FAKE_STATE";                                           shotp sNone
n=0
for a in sPlay sPause sStop sNone; do for b in sPlay sPause sStop sNone; do
    [ $a \< $b ] && cmp -s "$T/$a-tile.ppm" "$T/$b-tile.ppm" && { fail "$a and $b look identical"; n=1; }
done; done
[ $n = 0 ] && ok "playing / paused / stopped / no-player all look different"

# ---------------- backend missing: must show the tile, not crash ----------------
mkdir "$T/empty"
PATH="$T/empty:/usr/bin:/bin" ./wmmedia -w -D "$T/miss" ; rc=$?
[ $rc -eq 0 ] && [ -s "$T/miss-tile.ppm" ] && ok "missing playerctl handled" || fail "missing backend (rc=$rc)"

# ---------------- mpc backend ----------------
start -b mpc
check "mpc: polls status"                  "^-f .* status$"
: > "$T/log"; mouse 31 41 click 1
check "mpc: play/pause = toggle"           "^toggle$"
: > "$T/log"; mouse 12 41 click 1
check "mpc: prev"                          "^prev$"
: > "$T/log"; mouse 32 10 click 4
check "mpc: wheel up"                      "^volume \\+5$"
: > "$T/log"; mouse 31 53 click 1
check "mpc: bar click seeks by percent"    "^seek 50%$"
stop

# ---------------- player selection + step option ----------------
printf "$PLAYING" > "$FAKE_STATE"
start -p spotify -s 10
check "-p passes the player name"          "^-p spotify metadata"
: > "$T/log"; mouse 32 10 click 4
check "-s sets the wheel step"             "^-p spotify volume 0\\.10\\+$"
: > "$T/log"; mouse 31 53 click 1
check "-p applies to seeking too"          "^-p spotify position 120$"
stop

# ---------------- colours: wmvol teal by default, Window Maker theme only with -t ----------------
printf "$PLAYING" > "$FAKE_STATE"
printf '{ IconTitleBack = "#cc2222"; }\n' > "$T/th-red"
printf '{ FTitleBack = (hgradient, "rgb:20/40/dd", black); }\n' > "$T/th-blue"
shot() { PATH="$FULLPATH" "$@" ; }
shot ./wmmedia -w                                -D "$T/c0"
shot ./wmmedia -w -fg 20b2aa                     -D "$T/cF"
shot ./wmmedia -w -notheme                       -D "$T/cN"
shot ./wmmedia -w -c "$T/th-red"                 -D "$T/cIgn"     # file given but -t not: ignored
shot ./wmmedia -w -t -c "$T/th-red"              -D "$T/tR"
shot ./wmmedia -w -t -c "$T/th-blue"             -D "$T/tB"
shot ./wmmedia -w -t -c "$T/th-red" -fg 20b2aa   -D "$T/tRF"
shot ./wmmedia -w -fg 20b2aa -t -c "$T/th-red"   -D "$T/tFR"
shot ./wmmedia -w -t -c "$T/nonexistent"         -D "$T/tN"
shot ./wmmedia -w -fg ff0000                     -D "$T/cRed"
mkdir -p "$T/home/GNUstep/Defaults"; cp "$T/th-red" "$T/home/GNUstep/Defaults/WindowMaker"
HOME="$T/home" shot ./wmmedia -w -t -D "$T/tH"
HOME="$T/home" shot ./wmmedia -w    -D "$T/cH"
sameppm() { cmp -s "$T/$1-tile.ppm" "$T/$2-tile.ppm"; }
sameppm c0 cF  && ok "default look == -fg 20b2aa (wmvol teal)" || fail "default is not the wmvol teal"
sameppm c0 cN  && ok "-notheme is the default"                  || fail "-notheme differs"
sameppm c0 cIgn && ok "theme file ignored without -t"           || fail "theme applied without -t"
sameppm tR tB  && fail "different themes look the same"        || ok "-t: theme colours change the tile"
sameppm tR c0  && fail "-t ignored the theme"                  || ok "-t differs from classic"
sameppm tRF cF && ok "-fg overrides -t"                         || fail "-fg did not override -t"
sameppm tFR cF && ok "-fg overrides -t in either order"         || fail "-fg then -t not honoured"
sameppm tN c0  && ok "-t with a missing file = classic look"    || fail "missing file not classic"
sameppm tH tR  && ok "-t reads ~/GNUstep/Defaults/WindowMaker"  || fail "default path not read"
sameppm cH c0  && ok "...but only with -t"                      || fail "home theme applied without -t"
sameppm cRed c0 && fail "-fg ignored"                          || ok "-fg changes the colour"

# live reload with -t: rewrite the defaults file while running, the tile must change
if [ $HAVE_IM = 1 ]; then
    cp "$T/th-red" "$T/th-live"
    : > "$T/log"; printf "$PAUSED" > "$FAKE_STATE"
    PATH="$FULLPATH" ./wmmedia -w -t -c "$T/th-live" & PID=$!
    sleep 1.5; snap la
    printf '{ IconTitleBack = "#22cc66"; FTitleBack = (solid, "#22cc66"); }\n' > "$T/th-live"
    sleep 2.5; snap lb
    same la lb && fail "tile did not follow a theme change" || ok "-t follows a theme change while running"
    stop
else
    echo "skip image checks (no ImageMagick import/compare)"
fi

# docked mode without Window Maker must say why it will not dock; -w must stay quiet
printf "$PLAYING" > "$FAKE_STATE"
PATH="$FULLPATH" ./wmmedia -D "$T/nd" 2>"$T/err"
grep -q 'Window Maker not detected' "$T/err" && ok "warns when not under Window Maker" || fail "no warning outside Window Maker"
PATH="$FULLPATH" ./wmmedia -w -D "$T/nd2" 2>"$T/err"
[ -s "$T/err" ] && fail "-w printed warnings" || ok "-w is quiet"

# bad options fail loudly
./wmmedia -s abc 2>/dev/null && fail "bad -s accepted" || ok "bad -s rejected"
./wmmedia -b nope 2>/dev/null && fail "bad -b accepted" || ok "bad -b rejected"
./wmmedia -fg zzz 2>/dev/null && fail "bad -fg accepted" || ok "bad -fg rejected"

[ $fails -eq 0 ] && echo "ALL PASSED" || { echo "$fails FAILED"; exit 1; }
