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
source "$(dirname "$0")/lib/common.sh"
cd "$(dirname "$0")" || exit 1

make -s all 2>/dev/null || make all || exit 1

# Ask for the CPU sensor before measuring, not half way through
power_unlock

detect_wm || { echo "cannot tell what window manager this is"; exit 1; }
ST=$(session_type)

DE=$(echo "${XDG_CURRENT_DESKTOP:-unknown}" | tr '[:upper:]' '[:lower:]' | tr ':/ ' '-')
OUT="results/validate-$(date +%Y%m%d-%H%M%S)-$(hostname)-$DE-$WM_NAME-$ST.txt"
mkdir -p results
tee_report "$OUT"

# Ctrl-C at any moment: take the running checks, the tools and
# the tee down with us. The whole process group when we lead it (the normal
# terminal case), only our own children when someone else does.
on_int () {
    trap - INT TERM
    let_sleep
    if [ "$(ps -o pgid= -p $$ | tr -d ' ')" = "$$" ]; then
        kill -- -$$ 2>/dev/null
    else
        kill $(jobs -p) 2>/dev/null
    fi
    exit 130
}
trap on_int INT TERM
# and put the screen's own settings back however this run ends
trap let_sleep EXIT
keep_awake

echo "session:    $ST (${XDG_CURRENT_DESKTOP:-unknown desktop})"
echo "compositor: $WM_NAME $(wm_version) (pid ${WM_PID:-unknown})"

PIXELS=1
if [ "$ST" = wayland ]; then
    if CAP=$(pick_capture_cmd); then
        export BENCH_CAPTURE_CMD=$CAP
        echo "captures:   $CAP (Wayland screenshots; slower than X11)"
    else
        PIXELS=0
        echo "captures:   no Wayland screenshot tool found; the window manager"
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

# Kept in the words the verdict at the end repeats back
FAILURES=()
UNPROVEN=()
BROKEN=()
fail () { FAILURES+=("$1"); }

#   0 passed  1 the window manager is at fault  2 could not run  3 not done
report () {                     # $1 label, $2 status, $3 what it said
    printf '  %-16s ' "$1"
    case $2 in
        0) green passed;;
        2) echo "could not run"; BROKEN+=("$1: $3");;
        3) echo "not done"; UNPROVEN+=("$1");;
        *) red failed; fail "$1: $3";;
    esac
}

# With no visible pid (containers, some setups) there is nothing to watch;
# that is not a death.
alive () {
    [ -n "$WM_PID" ] || return 0
    [ -r "/proc/$WM_PID/stat" ] && return 0
    red "  THE WINDOW MANAGER DIED"
    fail "the window manager died"
    return 1
}

# The same checks everywhere, or the verdicts do not compare
CHECKS="motion stale pop suspend shape resize offscreen iconify"

if [ "$PIXELS" = 1 ]; then
    echo "== artifact checks"
    for c in $CHECKS; do
        case "$c" in motion) n=$MOTION;; resize) n=$RESIZE;; *) n=$CHK;; esac
        # A check waiting on a compositor that never answers would hold the run
        # for ever, with nothing said and nothing on screen
        timeout "$BENCH_RUN_TIMEOUT" ./checks/${c}_check "$n" \
            > "va-$c.txt" 2>&1; rc=$?
        if [ "$rc" = 124 ]; then
            echo "stopped after ${BENCH_RUN_TIMEOUT}s without finishing" \
                >> "va-$c.txt"
            rc=2
        fi
        why=$(tail -1 "va-$c.txt")
        [ -n "$why" ] || why="said nothing at all, and left with status $rc"
        report "${c}_check" $rc "$why"
        alive || break
    done
fi

echo

# Geometry asked for against geometry got, and a photograph of the window's own
# content at each step, so a compositor that quietly ignores a move is caught
CK="results/frames-$(date +%Y%m%d-%H%M%S)-$(hostname)-$DE-$WM_NAME-$ST"
mkdir -p "$CK"
BENCH_CHECKPOINT_DIR="$CK" timeout "$BENCH_RUN_TIMEOUT" \
    ./tools/usagebench 0 3 > va-usage-ck.log 2>&1

# What the idle desktop looks like before any of this, to be compared with the
# same desktop once the load is over and gone. Windows are left out of it, so a
# clock or a terminal with output in it is not mistaken for a leftover.
SHOT=""
BASE="${TMPDIR:-/tmp}/wmbench-screen-$$.ppm"
if [ "$PIXELS" = 1 ]; then
    sleep 1                     # let the checkpoint pass finish disappearing
    timeout "$BENCH_START_TIMEOUT" ./checks/leftover_check save "$BASE" \
        > va-left.txt 2>&1 && SHOT=$BASE
fi

echo "== stability"
# The mix, the work each load does and the order they stack in all come from
# lib/common.sh, so this is the same load benchmark.sh measures. manywin is
# scenery: it counts nothing, and is held until killed rather than going away
# part way through.
GO="$PWD/va-stress.go"; rm -f "$GO"
stress_start "$GO" va
M=$STRESS_SCENERY_PID

# The same question motion_check asks on an idle screen, asked of a busy one.
# It waits at the gate with the rest, so it looks during the load and not around
# it, and its captures are spread over the load rather than taken flat out,
# which would be a load of its own.
LP=""
if [ "$PIXELS" = 1 ]; then
    : > va-load.txt
    BENCH_ABOVE=1 BENCH_SECONDS=20 BENCH_GO="$GO" \
        ./checks/motion_check 100 >> va-load.txt 2>&1 &
    LP=$!
    STRESS_LOGS+=(va-load.txt); STRESS_PIDS+=("$LP")
fi

stress_wait_ready
STACK_OK=1
stress_settle || STACK_OK=0
[ "${STRESS_LATE:-0}" = 1 ] && STACK_OK=0
# The pattern being watched goes last of all: covered, it would be
# photographing somebody else's window. On Wayland it holds itself in the
# overlay layer, above everything, so there is nothing to ask for.
if [ -n "$LP" ] && [ "$ST" = x11 ]; then
    ./tools/restack -w "motion_check" >> va-restack.log 2>&1
fi
: > "$GO"

# The work in there is 20 s long. A load still going long after that is the
# session stalling, which is the thing being tested here, so it is stopped and
# reported rather than waited on for ever.
# The sleep in there is a process of its own and outlives the subshell that
# holds it, so it is kept off the report's pipe and reaped with its parent.
( sleep 90; kill "${STRESS_PIDS[@]}" 2>/dev/null ) >/dev/null 2>&1 &
WD=$!
for p in "${STRESS_PIDS[@]}"; do
    wait "$p" 2>/dev/null; wrc=$?
    [ "$p" = "$LP" ] && LRC=$wrc
done
pkill -P "$WD" 2>/dev/null; kill $WD 2>/dev/null; wait $WD 2>/dev/null
rm -f "$GO"

kill $M 2>/dev/null; wait $M 2>/dev/null

# A load that left without finishing its work went down under the load. The
# pattern check is in the list too, and answers with its status instead.
WAS=${#FAILURES[@]}
for i in "${!STRESS_NAMES[@]}"; do
    grep -q MEASURE-END "${STRESS_LOGS[$i]}" 2>/dev/null && continue
    fail "under load, ${STRESS_NAMES[$i]} did not finish its work"
done
alive
if [ "$ST" = x11 ]; then
    # Still answering, not just still running
    xprop -root _NET_SUPPORTING_WM_CHECK >/dev/null 2>&1 ||
        fail "the window manager stopped answering while everything ran at once"
fi
printf '  %-16s ' survived
[ "${#FAILURES[@]}" = "$WAS" ] && green passed || red failed
# The scene the load was watched in has to be the same scene everywhere, or
# what the checks above saw is not what another session's checks saw.
[ "$STACK_OK" = 0 ] &&
    echo "  the windows did not stack in the named order, so this load was" &&
    echo "  a different scene from the one other sessions run"
# A stack nobody would take means the load ran, but not the load that was
# designed. Said here so the verdict is not read as covering it.
for l in "${STRESS_LOGS[@]}"; do
    grep -q MOVE-NEVER-HAPPENED "$l" 2>/dev/null &&
        echo "  the window manager would not move a window it manages, so the" &&
        echo "  load above was a different scene from other sessions'" && break
done

# The pattern that was scrolling in the middle of all that
if [ -n "$LP" ]; then
    why=$(tail -1 va-load.txt)
    [ -n "$why" ] || why="no answer: it was still going when the load overran"
    report "under load" "$LRC" "$why"
fi

# Everything is gone now: is the desktop the one photographed before it?
if [ -n "$SHOT" ]; then
    sleep 3                     # closing animations are not leftovers
    timeout "$BENCH_START_TIMEOUT" ./checks/leftover_check check "$SHOT" \
        >> va-left.txt 2>&1; rc=$?
    [ "$rc" = 124 ] && rc=2
    report leftovers $rc "$(tail -1 va-left.txt)"
elif [ "$PIXELS" = 1 ]; then
    report leftovers 3 ""
fi

echo
echo "== verdict"
if [ "${#FAILURES[@]}" = 0 ]; then
    if [ "$PIXELS" = 1 ]; then
        green "  $WM_NAME passed: no artifacts found, stable under load"
    else
        green "  $WM_NAME is stable under load; pixel checks were skipped"
    fi
else
    red "  $WM_NAME FAILED, and this is what did not pass:"
    for f in "${FAILURES[@]}"; do
        red "    $f"
    done
fi
if [ "${#UNPROVEN[@]}" != 0 ] || [ "${#BROKEN[@]}" != 0 ]; then
    echo
    echo "== notes"
    if [ "${#UNPROVEN[@]}" != 0 ]; then
        echo "  the following tests are not done by this window manager:"
        for u in "${UNPROVEN[@]}"; do
            echo "    $u"
        done
        if [ "$ST" = wayland ]; then
            echo "  on wayland, shape_check and leftovers can exist nowhere"
            echo "  (no shape concept, no way to read other windows' places);"
            echo "  anything else on the list is a protocol this window manager"
            echo "  does not speak, layer-shell or alpha-modifier usually"
        fi
    fi
    if [ "${#BROKEN[@]}" != 0 ]; then
        echo "  the following tests could not run here at all:"
        for b in "${BROKEN[@]}"; do
            echo "    $b"
        done
    fi
fi
rm -f va-*.txt va-*.log "$BASE"
echo
echo "saved report to $OUT"
# Only say it when there is something in there
if ls "$CK"/ck-*.ppm >/dev/null 2>&1 || [ -s "$CK/manifest.txt" ]; then
    echo "saved checkpoints to $CK"
else
    rmdir "$CK" 2>/dev/null
fi
end_report
[ "${#FAILURES[@]}" = 0 ] && exit 0
exit 1
