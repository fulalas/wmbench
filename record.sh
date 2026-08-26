#!/bin/bash
# Record every test on video, about 5 seconds each, so what each desktop
# actually does can be watched and compared side by side. Nothing here is
# measured; the recordings are the result.
#
# Recording is gpu-screen-recorder: its KMS capture reads the scanout buffer,
# so it works the same on X11 and Wayland, and it is cheap. It needs
# cap_sys_admin on its helper once:
#
#   sudo setcap cap_sys_admin+ep /usr/bin/gsr-kms-server
#
# or every recording would ask for authentication. Another recorder can be
# used with BENCH_RECORD_CMD; the output file is appended to it.
#
# First a calibration: the render test with and without the recorder running,
# to see whether recording costs frames. If it does, the recordings cannot be
# trusted to show the desktop as it is, and the run says so.
set -u
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac

SECONDS_EACH=5
FPS=${BENCH_RECORD_FPS:-60}
# The stress mix does 5 seconds of work instead of benchmark.sh's 20
RENDER_FRAMES=$((SECONDS_EACH * 60))

source "$(dirname "$0")/lib/common.sh"
cd "$(dirname "$0")" || exit 1

make -s all 2>/dev/null || make all || exit 1

detect_wm || { echo "cannot tell what window manager this is"; exit 1; }
ST=$(session_type)
DE=$(echo "${XDG_CURRENT_DESKTOP:-unknown}" | tr '[:upper:]' '[:lower:]' | tr ':/ ' '-')
WMVER=$(wm_version | tr '-' '.')
if [ -z "${BENCH_RECORD_CMD:-}" ]; then
    command -v gpu-screen-recorder >/dev/null ||
        { echo "gpu-screen-recorder not found; set BENCH_RECORD_CMD"; exit 1; }
    GETCAP=$(command -v getcap || ls /sbin/getcap /usr/sbin/getcap 2>/dev/null | head -1)
    GSR_KMS=$(command -v gsr-kms-server || echo /usr/bin/gsr-kms-server)
    if [ -n "$GETCAP" ] &&
       ! "$GETCAP" "$GSR_KMS" 2>/dev/null | grep -q cap_sys_admin; then
        echo "gsr-kms-server has no cap_sys_admin, so every recording would"
        echo "ask for authentication. Give it once, then run this again:"
        echo
        echo "  sudo setcap cap_sys_admin+ep $GSR_KMS"
        exit 1
    fi
fi

# Only now that nothing can refuse to start does the folder exist
DIR="results/recordings-$(date +%Y%m%d-%H%M%S)-$(hostname)-$DE-$WM_NAME-${WMVER:-unknown}-$ST"
mkdir -p "$DIR"
MAN="$DIR/manifest.txt"

REC_PID=""
rec_start () {                  # $1 output file
    if [ -n "${BENCH_RECORD_CMD:-}" ]; then
        $BENCH_RECORD_CMD "$1" >> "$DIR/recorder.log" 2>&1 &
    else
        gpu-screen-recorder -w screen -f "$FPS" -o "$1" \
            >> "$DIR/recorder.log" 2>&1 &
    fi
    REC_PID=$!
    sleep 1                     # let it take its first frame
    # $! is set even for a command that died on the spot, and from then on
    # every test would be reported as recorded with no video behind it
    kill -0 "$REC_PID" 2>/dev/null ||
        { echo "the recorder did not start, see $DIR/recorder.log"
          # The stress loads wait at a gate this exit would never open, so
          # left alone they would wait for ever
          [ -n "${STRESS_PIDS+x}" ] && kill "${STRESS_PIDS[@]}" 2>/dev/null
          [ -n "${STRESS_SCENERY_PID:-}" ] &&
              kill "$STRESS_SCENERY_PID" 2>/dev/null
          exit 1; }
}
rec_stop () {                   # SIGINT is how the recorder finalizes the file
    kill -INT "$REC_PID" 2>/dev/null
    wait "$REC_PID" 2>/dev/null
    REC_PID=""
}

on_int () {
    trap - INT TERM
    [ -n "$REC_PID" ] && kill -INT "$REC_PID" 2>/dev/null
    let_sleep
    if [ "$(ps -o pgid= -p $$ | tr -d ' ')" = "$$" ]; then
        kill -- -$$ 2>/dev/null
    else
        kill $(jobs -p) 2>/dev/null
    fi
    exit 130
}
trap on_int INT TERM
# Three minutes with nobody touching the machine, and a screen that blanks
# part way through is recorded blanking: here the video is the whole result
trap let_sleep EXIT
keep_awake

{
    echo "session:    $ST (${XDG_CURRENT_DESKTOP:-unknown desktop})"
    echo "compositor: $WM_NAME $(wm_version)"
    echo "display:    $(display_info)"
    echo "recorder:   ${BENCH_RECORD_CMD:-gpu-screen-recorder} at $FPS fps"
} | tee "$MAN"
echo
echo "recording tests — do not use the computer (~3 min)"
echo

fps_of () { awk '/AVERAGE/{print $2}' "$1" 2>/dev/null; }

# Whether recording costs frames: the same uncapped render, alone and
# recorded. A drop means the recordings themselves change what they show.
echo "== calibration"
./tools/fsbench2 "$SECONDS_EACH" windowed > "$DIR/cal-alone.log" 2>&1
ALONE=$(fps_of "$DIR/cal-alone.log")
rec_start "$DIR/00-calibration.mp4"
./tools/fsbench2 "$SECONDS_EACH" windowed > "$DIR/cal-recorded.log" 2>&1
rec_stop
RECED=$(fps_of "$DIR/cal-recorded.log")
{
    echo "calibration: $ALONE fps alone, $RECED fps while recorded"
    awk -v a="$ALONE" -v r="$RECED" 'BEGIN{
        if (a == "" || r == "") { print "calibration: DID NOT RUN"; exit }
        d = 100 * (a - r) / a;
        # The tests are paced at 60 fps; what matters is headroom above that,
        # not the drop flat out
        if (r < 90) printf "recording leaves only %.0f fps of a needed 60 -\n%s\n",
                           r, "the recordings do not show the desktop as it is";
        else printf "recording costs %.0f%% flat out, %.0f fps of headroom left\n",
                    d, r - 60}'
} | tee -a "$MAN"
echo

# One test, one file. The cap is the tool's own clock plus its setup time;
# a phase whose single pass is longer than that is cut mid-action, at the
# same paced step in every session, and the manifest says so.
NUM=0
record () {                     # $1 name, $2 timeout, $3... the load
    local name=$1 to=$2 file rc
    shift 2
    NUM=$((NUM + 1))
    file=$(printf '%s/%02d-%s.mp4' "$DIR" "$NUM" "$name")
    printf '  %-12s' "$name"
    rec_start "$file"
    timeout -k 2 "$to" "$@" > "$DIR/$(printf '%02d' $NUM)-$name.log" 2>&1
    rc=$?
    rec_stop
    if [ "$rc" = 3 ]; then
        # 3 is the suite's refusal channel, not a failure: on a compositor
        # without layer-shell most of these have nothing to ask for, and a
        # video of a test that never ran is not a test that broke
        echo "nothing to record, this session refused"
        echo "$(basename "$file"): not done, this session refused" >> "$MAN"
    elif [ ! -s "$file" ]; then
        # Ahead of the status, not after it: rec_start only sees the recorder's
        # first second, and a test cut at its timeout is the one whose recorder
        # ran longest and had the most chance to die. Checked last, that test
        # went into the manifest as recorded with nothing behind it
        red "FAILED (no video, see $DIR/recorder.log)"
        echo "$(basename "$file"): FAILED, no video" >> "$MAN"
    elif [ "$rc" = 124 ]; then
        echo "recorded (cut at ${to}s, its pass is longer)"
        echo "$(basename "$file"): cut at ${to}s" >> "$MAN"
    elif [ "$rc" != 0 ]; then
        red "FAILED (status $rc, see the log)"
        echo "$(basename "$file"): FAILED status $rc" >> "$MAN"
    else
        echo "recorded"
        echo "$(basename "$file"): ok" >> "$MAN"
    fi
    sleep 2                     # let the desktop settle before the next one
}

echo "== tests"
record move       10 ./tools/movebench "$SECONDS_EACH" move 120
record resize     10 ./tools/usagebench "$SECONDS_EACH" 3 resize
record dnd        10 ./tools/usagebench "$SECONDS_EACH" 3 dnd
record windows    10 ./tools/usagebench "$SECONDS_EACH" 3 windows
record scroll     10 ./tools/usagebench "$SECONDS_EACH" 3 scroll
record popups     10 ./tools/popbench "$SECONDS_EACH" 20 6
record video      10 ./tools/videobench "$SECONDS_EACH" 60
record argb       10 ./tools/argbbench "$SECONDS_EACH" 120
record render     10 ./tools/fsbench2 "$SECONDS_EACH" windowed 60
record fullscreen 10 env BENCH_LIGHT=1 ./tools/fsbench2 "$SECONDS_EACH" fullscreen 60

# The stress mix, the same one benchmark.sh and validate.sh run, with 5
# seconds of work per load. Held at the gate until everything is on screen,
# stacked in the named order, and recorded from the moment the gate opens.
NUM=$((NUM + 1))
printf '  %-12s' "stress"
GO="$PWD/rec-stress.go"; rm -f "$GO"
stress_start "$GO" "$DIR/rec-s"
MANY=$STRESS_SCENERY_PID        # scenery, counts nothing, killed at the end
stress_wait_ready
STACK_OK=1
stress_settle || STACK_OK=0
[ "${STRESS_LATE:-0}" = 1 ] && STACK_OK=0
SFILE=$(printf '%s/%02d-stress.mp4' "$DIR" "$NUM")
rec_start "$SFILE"
: > "$GO"
# The sleep in there is a process of its own and outlives the subshell that
# holds it, so it is kept off this script's output and reaped with its parent
( sleep 30; kill "${STRESS_PIDS[@]}" 2>/dev/null ) >/dev/null 2>&1 &
WD=$!
OK=1
for i in "${!STRESS_PIDS[@]}"; do
    wait "${STRESS_PIDS[$i]}" 2>/dev/null
    grep -q MEASURE-END "${STRESS_LOGS[$i]}" 2>/dev/null || OK=0
done
pkill -P "$WD" 2>/dev/null; kill $WD 2>/dev/null; wait $WD 2>/dev/null
rec_stop
kill $MANY 2>/dev/null; wait $MANY 2>/dev/null
rm -f "$GO"
if [ ! -s "$SFILE" ]; then
    red "FAILED (no video, see $DIR/recorder.log)"
    echo "$(basename "$SFILE"): FAILED, no video" >> "$MAN"
elif [ "$OK" = 1 ] && [ "$STACK_OK" = 1 ]; then
    echo "recorded"
    echo "$(printf '%02d' $NUM)-stress.mp4: ok" >> "$MAN"
elif [ "$OK" = 1 ]; then
    # A desktop that stacked them some other way composited a different scene,
    # so the recording is not the one every other session shows
    red "recorded, but the stacking order did not take"
    echo "$(printf '%02d' $NUM)-stress.mp4: the stacking order did not take" >> "$MAN"
else
    red "recorded, but a load did not finish (see the logs)"
    echo "$(printf '%02d' $NUM)-stress.mp4: a load did not finish" >> "$MAN"
fi

echo
echo "saved recordings to $DIR"
