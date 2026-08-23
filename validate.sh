#!/bin/bash
# Validate the window manager this session is running, whichever it is:
# builds the checks, detects the compositor, runs the artifact checks and a
# stability pass, prints a verdict. Exit status 0 when everything passed.
#
#   ./validate.sh               full run
#   QUICK=1 ./validate.sh       fewer rounds
#
# On X11 the checks photograph the screen directly. On Wayland they need a
# screenshot tool (grim, or spectacle/gnome-screenshot plus ImageMagick or
# ffmpeg); captures are slow there, so the moving checks run shortened, and
# without any tool the pixel checks are skipped rather than pretended.
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
QUICK=${QUICK:-0}
BENCH_POWER_OPTIONAL=1
source "$(dirname "$0")/common.sh"
cd "$(dirname "$0")" || exit 1

make -s all 2>/dev/null || make all || exit 1

detect_wm || { echo "cannot tell what window manager this is"; exit 1; }
ST=$(session_type)

# Everything below goes to the screen and to a result file
DE=$(echo "${XDG_CURRENT_DESKTOP:-unknown}" | tr '[:upper:]' '[:lower:]' | tr ':/ ' '-')
OUT="results/validate-$(date +%Y%m%d-%H%M%S)-$(hostname)-$DE-$WM_NAME-$ST.txt"
mkdir -p results
exec > >(tee "$OUT") 2>&1

# Ctrl-C at any moment: take the running checks, the tools and
# the tee down with us. The whole process group when we lead it (the normal
# terminal case), only our own children when someone else does.
on_int () {
    trap - INT TERM
    # The GNOME screencast is not a child process; it must be told to stop
    stop_recorder 2>/dev/null
    if [ "$(ps -o pgid= -p $$ | tr -d ' ')" = "$$" ]; then
        kill -- -$$ 2>/dev/null
    else
        kill $(jobs -p) 2>/dev/null
    fi
    exit 130
}
trap on_int INT TERM

echo "session:    $ST (${XDG_CURRENT_DESKTOP:-unknown desktop})"
echo "compositor: $WM_NAME $(wm_version) (pid ${WM_PID:-unknown})"

PIXELS=1
if [ "$ST" = wayland ]; then
    if CAP=$(pick_capture_cmd); then
        export BENCH_CAPTURE_CMD=$CAP
        echo "captures:   $CAP (Wayland screenshots; slower than X11)"
    else
        PIXELS=0
        echo "captures:   no Wayland screenshot tool found; the compositor"
        echo "            must hand the screen out (grim on labwc/COSMIC/sway,"
        echo "            gnome-screenshot on GNOME, spectacle on KDE);"
        echo "            the pixel checks are skipped"
    fi
fi
echo

# The same counts in every session, or the results are not comparable
if [ "$QUICK" = 1 ]; then
    MOTION=6; RESIZE=20; CHK=2
else
    MOTION=25; RESIZE=120; CHK=3
fi

FAILED=0
# With no visible pid (containers, some setups) there is nothing to watch;
# that is not a death.
alive () {
    [ -n "$WM_PID" ] || return 0
    [ -r "/proc/$WM_PID/stat" ] && return 0
    echo "  THE COMPOSITOR DIED"; FAILED=1; return 1
}

# The same checks for every WM
CHECKS="motion stale pop suspend shape resize offscreen iconify"

if [ "$PIXELS" = 1 ]; then
    echo "== artifact checks"
    for c in $CHECKS; do
        case "$c" in motion) n=$MOTION;; resize) n=$RESIZE;; *) n=$CHK;; esac
        printf '  %-16s ' "${c}_check"
        ./checks/${c}_check "$n" > "va-$c.txt" 2>&1; rc=$?
        tail -1 "va-$c.txt"
        [ $rc -ne 0 ] && FAILED=1
        alive || break
    done
    echo
fi

echo "== usage checkpoints, for comparing sessions"
CK="results/frames-$(date +%Y%m%d-%H%M%S)-$(hostname)-$DE-$WM_NAME-$ST"
mkdir -p "$CK"
# Filmed as well: geometry can say a window did not move, the video shows
# what actually happened instead
if start_recorder "$CK/usage"; then
    RECORDED=1
else
    RECORDED=0
fi
BENCH_CHECKPOINT_DIR="$CK" ./tools/usagebench 0 3 > va-usage-ck.log 2>&1
[ "$RECORDED" = 1 ] && stop_recorder
if ls "$CK"/ck-*.ppm >/dev/null 2>&1; then
    echo "  $(ls "$CK"/ck-*.ppm | wc -l) checkpoints in $CK"
else
    echo "  geometry recorded in $CK; no screenshots (no capture tool)"
fi
if [ "$RECORDED" = 1 ] &&
   ffprobe -v error "$RECFILE" >/dev/null 2>&1; then
    echo "  video of the whole pass: $RECFILE"
elif [ "$RECORDED" = 1 ]; then
    rm -f "$RECFILE"
    echo "  the recorder produced nothing playable (on GNOME its screencast"
    echo "  needs PipeWire running)"
else
    echo "  no video: this session has no recorder (ffmpeg on X11,"
    echo "  wf-recorder on labwc/COSMIC/sway, the shell's screencast on GNOME)"
fi
echo "  compare two sessions with: ./compare_runs.sh <this> <other>"
echo

echo "== stability: every workload at once, 20 s"
./tools/manywin 12 30 > va-many.log 2>&1 &
M=$!
sleep 4
./tools/movebench 20 move 120 > va-move.log 2>&1 &
./tools/popbench 20 20 4 > va-pop.log 2>&1 &
./tools/transbench 20 0.75 60 > va-trans.log 2>&1 &
./tools/usagebench 20 3 > va-usage.log 2>&1 &
./tools/fsbench2 20 windowed > va-render.log 2>&1
wait
OKS=1
alive || OKS=0
if [ "$ST" = x11 ]; then
    # Still answering, not just still running
    xprop -root _NET_SUPPORTING_WM_CHECK >/dev/null 2>&1 || {
        echo "  the compositor stopped answering"; FAILED=1; OKS=0; }
fi
[ "$OKS" = 1 ] && echo "  survived"
kill $M 2>/dev/null; wait $M 2>/dev/null

echo
echo "== verdict"
if [ "$FAILED" = 0 ]; then
    if [ "$PIXELS" = 1 ]; then
        echo "  $WM_NAME passed: no artifacts found, stable under load"
    else
        echo "  $WM_NAME is stable under load; pixel checks were skipped"
    fi
else
    echo "  $WM_NAME FAILED, see above"
fi
rm -f va-*.txt va-*.log
echo
echo "saved to $OUT"
exit $FAILED
