#!/bin/bash
set -u
ALL="idle move resize dnd popups video argb windows scroll render fullscreen uncapped stress"

usage_help () {
    cat <<EOF
benchmark the window manager this session is running

  ./benchmark.sh [test ...]     run everything, or only the tests named

tests:
  idle        nothing happening
  move        a window moved 2400 steps at 120 a second
  resize      two full cycles of resizing by every handle, corner and edge
  dnd         drag and drop: one pass of icons over, then all back at once
  popups      400 menus appearing and disappearing at 20 a second
  video       1200 frames handed over through shared memory, the way a player
              does, at 60 a second
  argb        2400 redraws of a transparent window that declares an opaque
              region, which is what every GTK window is, at 120 a second
  windows     the states a window goes through: maximize and back, two windows
              raised over each other, fullscreen and back, and minimize last
  scroll      a text document scrolled by the chevrons, then by dragging the
              thumb the whole way and back
  render      1200 frames of a GL window held to 60 fps
  fullscreen  the same frames in a fullscreen window, twice: as it comes, and
              asking compositing to step aside the way a player does - with
              _NET_WM_BYPASS_COMPOSITOR on X11, and on Wayland by declaring
              the whole surface opaque
  uncapped    the frame rate flat out, windowed. Runs last
  stress      everything at once: many windows, one moving, one resizing,
              popups, a translucent window and a GL render

Every test does the same fixed amount of work in every session, however long
that takes, so power and CPU time compare directly. The one exception is
uncapped, which is deliberately flat out and reports only frames a second.

A test the window manager cannot run is flagged "not done", and a test that
tries and fails is flagged "failed".

The results appear as they are measured and are recorded in the results
subfolder.
EOF
}
case "${1:-}" in -h|--help) usage_help; exit 0;; esac

TESTS=${*:-$ALL}
for t in $TESTS; do
    case "$t" in idle|move|resize|dnd|popups|video|argb|windows|scroll|render|fullscreen|uncapped|stress) ;;
        *) echo "unknown test: $t"; echo; usage_help; exit 1;;
    esac
done
want () { case " $TESTS " in *" $1 "*) return 0;; *) return 1;; esac; }

IDLE_WINDOW=${IDLE_WINDOW:-20}
MOVE_STEPS=${MOVE_STEPS:-2400}
POP_CYCLES=${POP_CYCLES:-400}
VIDEO_FRAMES=${VIDEO_FRAMES:-1200}
ARGB_STEPS=${ARGB_STEPS:-2400}
RENDER_FRAMES=${RENDER_FRAMES:-1200}
RESIZE_CYCLES=${RESIZE_CYCLES:-2}
DND_PASSES=${DND_PASSES:-1}
WINDOWS_PASSES=${WINDOWS_PASSES:-1}
SCROLL_PASSES=${SCROLL_PASSES:-1}
UNCAP_SECONDS=${UNCAP_SECONDS:-16}


source "$(dirname "$0")/lib/common.sh"
cd "$(dirname "$0")" || exit 1

make -s all 2>/dev/null || make all || exit 1

# Before the first measurement: unlocking the sensor half way through a run
# leaves the rows before it without power
power_unlock

detect_wm || { echo "cannot tell what window manager this is"; exit 1; }
ST=$(session_type)

DE=$(echo "${XDG_CURRENT_DESKTOP:-unknown}" | tr '[:upper:]' '[:lower:]' | tr ':/ ' '-')
# A dash in the version would read as another field in the file name
WMVER=$(wm_version | tr '-' '.')
OUT="results/benchmark-$(date +%Y%m%d-%H%M%S)-$(hostname)-$DE-$WM_NAME-${WMVER:-unknown}-$ST.txt"
mkdir -p results
tee_report "$OUT"

# Killing the process group is only ours to do when we lead it: run from
# something that leads it instead, kill -- -$$ takes that down too
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

echo "system:     $(hostname) ($(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}"))"
echo "kernel:     $(uname -r)"
echo "display:    $(display_info)"
echo "session:    $ST (${XDG_CURRENT_DESKTOP:-unknown desktop})"
# On X11 the compositor announces itself by owning a selection, which has to
# be asked for by name and cannot be read off the root window; a Wayland
# compositor always composites
if [ "$ST" != x11 ] || ./tools/cmcheck >/dev/null 2>&1; then
    COMPOSITING=on
else
    COMPOSITING=OFF
fi
echo "compositor: $WM_NAME $(wm_version) (compositing $COMPOSITING)"
[ -z "$WM_PIDS" ] &&
    echo "            part of the desktop's processes cannot be seen, so the" &&
    echo "            CPU column is left empty rather than filled with a" &&
    echo "            figure that is missing a piece"
[ "$POWER_OK" = 1 ] && echo "power:      $POWER_DESC" \
                     || echo "power:      no sensor, not reported"
# The moving loads prove their windows really moved with one screenshot before
# and one after the measurement; without a tool they run on the protocol's
# word, and say so themselves in their logs
if [ "$ST" = wayland ]; then
    if CAP=$(pick_capture_cmd); then
        export BENCH_CAPTURE_CMD=$CAP
    else
        echo "captures:   no screenshot tool, so the window moves are unproven"
    fi
fi
echo
EST=0
for t in $TESTS; do
    case "$t" in
        idle)     EST=$((EST + IDLE_WINDOW + 4));;
        windows)  EST=$((EST + WINDOWS_PASSES * 15 + 10));;
        scroll)   EST=$((EST + SCROLL_PASSES * 12 + 10));;
        move)     EST=$((EST + MOVE_STEPS / 120 + 10));;
        resize)   EST=$((EST + RESIZE_CYCLES * 18 + 10));;
        dnd)      EST=$((EST + DND_PASSES * 20 + 10));;
        popups)   EST=$((EST + POP_CYCLES / 20 + 10));;
        video)    EST=$((EST + VIDEO_FRAMES / 60 + 10));;
        argb)     EST=$((EST + ARGB_STEPS / 120 + 10));;
        render)   EST=$((EST + RENDER_FRAMES / 60 + 10));;
        fullscreen) EST=$((EST + 2 * (RENDER_FRAMES / 60 + 10)));;
        uncapped) EST=$((EST + UNCAP_SECONDS + 6));;
        stress)   EST=$((EST + RENDER_FRAMES / 60 + 30));;
    esac
done
echo "running tests — do not use the computer (~$(( (EST + 59) / 60 )) min)"

now_s () { date +%s.%N; }

comp_cpu () {
    wm_cpu
}

# A counter that went backwards is a process replaced mid-test, not a small
# reading
cpu_delta () {
    awk -v a="$1" -v b="$2" 'BEGIN{
        print (a == "" || b == "" || b < a) ? "-" : sprintf ("%.2f", (b - a) / 1000)}'
}

idle_window () {
    local c0 c1 w end=$((SECONDS + $1))

    c0=$(comp_cpu); power_begin
    while [ $SECONDS -lt $end ]; do
        power_sample; sleep 0.1
    done
    c1=$(comp_cpu); w=$(power_end)
    awk -v w="$w" -v c="$(cpu_delta "$c0" "$c1")" -v t="$1" \
        'BEGIN{printf "%s %s %.1f", w, c, t}'
}

measure () {
    local name=$1 tasks=$2 log pid t0 t1 c0 c1 w deadline
    shift 2
    log="bm-$name.log"
    : > "$log"
    BENCH_TASKS=$tasks "$@" >> "$log" 2>&1 &
    pid=$!

    deadline=$((SECONDS + BENCH_START_TIMEOUT))
    while ! grep -q MEASURE-START "$log" 2>/dev/null; do
        # A load can refuse before the measurement even starts - a Wayland
        # session with nothing to ask for - and 3 means refused, not broken
        kill -0 "$pid" 2>/dev/null || { wait "$pid"
                                        [ "$?" = 3 ] && return 3
                                        return 1; }
        if [ "$SECONDS" -ge "$deadline" ]; then
            # Never the marker's own name in the message: the log is what is
            # searched for the markers, and a line saying one is missing would
            # be read back as the marker itself
            echo "killed after ${BENCH_START_TIMEOUT}s: never began measuring" \
                >> "$log"
            kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null

            return 1
        fi
        sleep 0.1
    done
    t0=$(now_s); c0=$(comp_cpu); power_begin
    deadline=$((SECONDS + BENCH_RUN_TIMEOUT))
    while ! grep -q MEASURE-END "$log" 2>/dev/null; do
        kill -0 "$pid" 2>/dev/null || break
        if [ "$SECONDS" -ge "$deadline" ]; then
            echo "killed after ${BENCH_RUN_TIMEOUT}s: never finished its work" \
                >> "$log"
            kill "$pid" 2>/dev/null
            break
        fi
        power_sample
        sleep 0.1
    done
    t1=$(now_s); c1=$(comp_cpu); w=$(power_end)
    wait "$pid"; rc=$?
    # 3 is the load saying the desktop refused the work, not a load that failed
    [ "$rc" = 3 ] && return 3
    [ "$rc" = 0 ] || return 1
    grep -q MEASURE-END "$log" || return 1

    awk -v w="$w" -v c="$(cpu_delta "$c0" "$c1")" -v t0="$t0" -v t1="$t1" \
        'BEGIN{printf "%s %s %.1f", w, c, t1 - t0}'
}

REFUSED=(); REFUSED_LOGS=()
ROW_OUT=""
run_row () {
    local label=$1 name=$2 rc
    shift
    running "$label"
    ROW_OUT=$(measure "$@"); rc=$?
    case $rc in
        0) tally "$label" "$ROW_OUT";;
        3) done_running; ROW_OUT=""
           REFUSED+=("$label"); REFUSED_LOGS+=("$name")
           row_state "$label" not_done;;
        *) done_running; ROW_OUT=""; LOADFAIL="$LOADFAIL $name"
           row_state "$label" failed;;
    esac
}

IDLE=""; WINDOWS=""; SCROLL=""; MOVE=""; RESIZE=""; DND=""; POP=""; RENDER=""; UNCAP=""
VIDEO=""; ARGB=""
FPS=""; FS=""; FS_ASK=""; STRESS=""; LOADFAIL=""

# The braces matter: a bare exec applies every redirection to the shell itself,
# so the silence meant for a failed open would swallow this run's own errors,
# which tee_report has just pointed at the result file
{ exec 3>/dev/tty; } 2>/dev/null || exec 3>/dev/null
running () {
    printf '\033[2K%s (running...)\r' "$1" >&3
}
done_running () { printf '\033[2K\r' >&3; }

RULE=$(printf '\u2500%.0s' $(seq 61))
DATA=()

# The colour goes outside the padded field: inside it the escapes count as
# characters and the column comes out short by their width
row_state () {
    local colour word
    case "$2" in
        failed)   colour=$RED;    word=failed;;
        not_done) colour=$YELLOW; word="not done";;
    esac
    done_running
    # Without a data line the comparison drops the row as if it had never been
    # asked for
    DATA+=("- - - $1")
    printf '%-18s%13s%16s%s%14s%s\n' "$1" "-" "-" "$colour" "$word" "$OFF"
}
print_row () {
    local w c t
    done_running
    [ -n "$2" ] || return 0
    read -r w c t <<< "$2"
    DATA+=("$w $c $t $1")
    [ "$w" = "-" ] || w="$w W"
    [ "$c" = "-" ] || c="$c s"
    printf '%-18s%13s%16s%14s\n' "$1" "$w" "$c" "$t s"
}

# Seconds add; watts are a rate and do not - they are kept as energy here and
# divided by the total time at the end
TCPU=0; TTIME=0; TENERGY=0; TROWS=0; TPOWER=1; TCPUOK=1
tally () {
    local rw rc rt
    [ -n "$2" ] || return 0
    print_row "$1" "$2"
    read -r rw rc rt <<< "$2"
    [ "$rw" = "-" ] && TPOWER=0
    [ "$rc" = "-" ] && TCPUOK=0
    TROWS=$((TROWS + 1))
    read -r TCPU TTIME TENERGY <<< "$(awk -v c="$TCPU" -v t="$TTIME" \
        -v e="$TENERGY" -v rc="$rc" -v rt="$rt" -v rw="$rw" 'BEGIN{
            printf "%.2f %.1f %.3f", c + ((rc == "-") ? 0 : rc), t + rt,
                   e + ((rw == "-") ? 0 : rw * rt)}')"
}

echo
printf '%-18s%13s%16s%14s\n' workload power "desktop CPU" "time elapsed"

# print_row and not tally: idle is the baseline and no part of the total
want idle && running "idle" && IDLE=$(idle_window "$IDLE_WINDOW") && print_row "idle" "$IDLE"

if want move; then
    run_row "move" move "$MOVE_STEPS" ./tools/movebench 0 move 120
    MOVE=$ROW_OUT
fi
if want resize; then
    run_row "resize" resize "$RESIZE_CYCLES" ./tools/usagebench 0 3 resize
    RESIZE=$ROW_OUT
fi
if want dnd; then
    run_row "drag and drop" dnd "$DND_PASSES" ./tools/usagebench 0 3 dnd
    DND=$ROW_OUT
fi
if want popups; then
    run_row "popups" popups "$POP_CYCLES" ./tools/popbench 0 20 6
    POP=$ROW_OUT
fi
if want video; then
    run_row "video" video "$VIDEO_FRAMES" ./tools/videobench 0 60
    VIDEO=$ROW_OUT
fi
if want argb; then
    run_row "transparent window" argb "$ARGB_STEPS" ./tools/argbbench 0 120
    ARGB=$ROW_OUT
fi
if want windows; then
    run_row "windows" windows "$WINDOWS_PASSES" ./tools/usagebench 0 3 windows
    WINDOWS=$ROW_OUT
fi
if want scroll; then
    run_row "scroll" scroll "$SCROLL_PASSES" ./tools/usagebench 0 3 scroll
    SCROLL=$ROW_OUT
fi
if want render; then
    run_row "render 60 fps" render "$RENDER_FRAMES" ./tools/fsbench2 0 windowed 60
    RENDER=$ROW_OUT
fi

# BENCH_LIGHT on purpose: with the heavy shader the application costs some
# 14 ms a frame at 4K and compositing well under one, and the difference this
# pair is here to show disappears into the noise
if want fullscreen; then
    run_row "fullscreen" fullscreen "$RENDER_FRAMES" \
        env BENCH_LIGHT=1 ./tools/fsbench2 0 fullscreen 60
    FS=$ROW_OUT
    run_row "fullscreen asked" fullscreen-asked "$RENDER_FRAMES" \
        env BENCH_LIGHT=1 BENCH_BYPASS=1 ./tools/fsbench2 0 fullscreen 60
    FS_ASK=$ROW_OUT
fi

# Second to last on purpose: everything at once heats the chip and would warm
# up whatever ran after it
if want stress; then
    running "stress"
    GO="$PWD/bm-stress.go"; rm -f "$GO"
    # The mix lives in lib/common.sh so validate.sh measures the same load
    stress_start "$GO" bm-s
    MANY=$STRESS_SCENERY_PID
    SL=("${STRESS_LOGS[@]}"); SP=("${STRESS_PIDS[@]}"); SEND=()
    stress_wait_ready

    STACK_OK=1
    stress_settle || STACK_OK=0
    [ "${STRESS_LATE:-0}" = 1 ] && STACK_OK=0
    : > "$GO"

    for i in "${!SL[@]}"; do
        SDL=$((SECONDS + BENCH_START_TIMEOUT))
        while ! grep -q MEASURE-START "${SL[$i]}" 2>/dev/null; do
            # A load that died before the gate never writes it: without this
            # break the wait never ends
            kill -0 "${SP[$i]}" 2>/dev/null || break
            if [ "$SECONDS" -ge "$SDL" ]; then
                echo "killed after ${BENCH_START_TIMEOUT}s: never began measuring" \
                    >> "${SL[$i]}"
                kill "${SP[$i]}" 2>/dev/null
                break
            fi
            sleep 0.1
        done
    done
    T0=$(now_s); C0=$(comp_cpu); power_begin
    OK=1; MIX_REFUSED=0; LEFT=${#SL[@]}
    SDL=$((SECONDS + BENCH_RUN_TIMEOUT))
    while [ "$LEFT" -gt 0 ]; do
        power_sample
        sleep 0.1
        # Killed, not merely given up on: the loop below counts a load as done
        # when its process goes, and one left running would go on drawing over
        # the test after this one
        if [ "$SECONDS" -ge "$SDL" ]; then
            for i in "${!SP[@]}"; do
                kill -0 "${SP[$i]}" 2>/dev/null || continue
                echo "killed after ${BENCH_RUN_TIMEOUT}s: never finished its work" \
                    >> "${SL[$i]}"
                kill "${SP[$i]}" 2>/dev/null
            done
        fi
        LEFT=0
        for i in "${!SL[@]}"; do
            [ -n "${SEND[$i]:-}" ] && continue
            if grep -q MEASURE-END "${SL[$i]}" 2>/dev/null; then
                SEND[$i]=$(now_s)
            elif ! kill -0 "${SP[$i]}" 2>/dev/null; then
                # Why it left, not just that it did: 3 is the desktop refusing
                # the work, which is a row not done, the same as on its own
                wait "${SP[$i]}" 2>/dev/null; rc=$?
                SEND[$i]=$(now_s)
                [ "$rc" = 3 ] && MIX_REFUSED=1 || OK=0
            else
                LEFT=$((LEFT + 1))
            fi
        done
    done
    T1=$(now_s); C1=$(comp_cpu)
    for p in "${SP[@]}" $MANY; do kill "$p" 2>/dev/null; done
    for p in "${SP[@]}" $MANY; do wait "$p" 2>/dev/null; done
    rm -f "$GO"
    STRESS_THIN=$(awk -v t0="$T0" -v t1="$T1" -v e="${SEND[*]}" 'BEGIN{
        n = split (e, a, " "); lo = t1;
        for (i = 1; i <= n; i++) if (a[i] < lo) lo = a[i];
        if (t1 - lo > 0.2 * (t1 - t0))
            printf "%.0f", 100 * (t1 - lo) / (t1 - t0)}')
    if [ "$OK" = 1 ]; then
        W=$(power_end)
        STRESS=$(awk -v w="$W" -v c="$(cpu_delta "$C0" "$C1")" \
                     -v t0="$T0" -v t1="$T1" \
                     'BEGIN{printf "%s %s %.1f", w, c, t1 - t0}')
    else
        # Every power_begin needs its power_end, failed row or not: left open,
        # the sensor's background sampler runs on through the next test and
        # that test's power_begin loses the handle on it
        power_end > /dev/null
    fi
    # A load whose window never moved leaves a scene no other session had, so
    # the row is not a result
    MIX_OK=$STACK_OK
    [ "$MIX_REFUSED" = 1 ] && MIX_OK=0
    for l in "${SL[@]}"; do
        grep -q MOVE-NEVER-HAPPENED "$l" 2>/dev/null && MIX_OK=0
    done
    # A load that died is asked about before the scene is: a mix that lost one
    # of its own is a failure with a log to read, not a desktop saying no
    if [ "$OK" != 1 ] || [ -n "$STRESS_THIN" ]; then
        # The six are given work that has them finish together, so one leaving
        # early leaves the rest running against a lighter screen than any other
        # session's
        [ -n "$STRESS_THIN" ] &&
            echo "one of the six loads finished early, $STRESS_THIN% of the run" \
                 "went with less on screen" >> "$STRESS_PRE-thin.log"
        LOADFAIL="$LOADFAIL stress"
        row_state stress failed
        STRESS=""
    elif [ "$MIX_OK" = 0 ]; then
        REFUSED+=("stress"); REFUSED_LOGS+=("stress")
        row_state stress not_done
        STRESS=""
    fi
    tally "stress" "$STRESS"
fi

if want uncapped; then
    speed_run () {
        local log=$1
        shift
        # A window mapped while the compositor is still catching up with the
        # last test is not composited, and the frame rate then measures nothing
        sleep 3
        ./tools/fsbench2 "$@" > "bm-$log.log" 2>&1 || true
        awk '/AVERAGE/{print $2}' "bm-$log.log" 2>/dev/null
    }
    running "speed"
    FPS=$(speed_run uncapped "$UNCAP_SECONDS" windowed)
    done_running
    [ -n "$FPS" ] || LOADFAIL="$LOADFAIL uncapped"
fi

# A total over fewer rows is a smaller total: printing one for a session that
# skipped work hands it the win in every column it did not do
NFAIL=$(set -- $LOADFAIL; echo $#)
if [ "$TROWS" -gt 0 ] && { [ "$NFAIL" != 0 ] || [ "${#REFUSED[@]}" != 0 ]; }; then
    echo "$RULE"
    MISSING=""
    [ "$NFAIL" != 0 ] && MISSING="$NFAIL failed"
    [ "${#REFUSED[@]}" != 0 ] &&
        MISSING="${MISSING:+$MISSING, }${#REFUSED[@]} not done"
    echo "no total: $MISSING"
fi
if [ "$TROWS" -gt 0 ] && [ -z "$LOADFAIL" ] && [ "${#REFUSED[@]}" = 0 ]; then
    echo "$RULE"
    if [ "$TPOWER" = 1 ]; then
        TW=$(awk -v e="$TENERGY" -v t="$TTIME" \
             'BEGIN{printf "%.2f", (t ? e / t : 0)}')
    else
        TW="-"
    fi
    [ "$TCPUOK" = 1 ] && TC=$TCPU || TC="-"
    DATA+=("$TW $TC $TTIME total")
    [ "$TW" = "-" ] || TW="$TW W (avg.)"
    [ "$TC" = "-" ] || TC="$TC s"
    printf '%-18s%13s%16s%14s\n' total "$TW" "$TC" "$TTIME s"
    [ "$TPOWER" = 1 ] && awk -v e="$TENERGY" \
        'BEGIN{printf "\nenergy:  %.0f J to do all the work above\n", e}'
fi

if [ -n "$FPS" ]; then
    echo
    echo "speed:   $FPS fps windowed"
fi

BAT=$(cat /sys/class/power_supply/BAT*/energy_full 2>/dev/null | head -1)
if [ "$POWER_OK" = 1 ] && [ -n "$BAT" ] && [ -n "$IDLE" ] && [ -n "$WINDOWS" ]; then
    read -r iw ic it <<< "$IDLE"
    read -r uw uc ut <<< "$WINDOWS"
    awk -v i="$iw" -v u="$uw" -v uwh="$BAT" 'BEGIN{
        printf "battery: %s W idle to %s W in constant use - about %.1f h to",
               i, u, uwh / 1e6 / i;
        printf " %.1f h\n         on this %.0f Wh battery\n",
               uwh / 1e6 / u, uwh / 1e6}'
fi

# Anything that is not a measurement goes here, so nothing interrupts the table
if [ -n "$LOADFAIL" ] || [ "${#REFUSED[@]}" != 0 ]; then
    echo
    echo "== notes"
    note () { fold -s -w 74 | sed '2,$s/^/  /;s/ *$//'; }
    if [ -n "$LOADFAIL" ]; then
        printf -- '- failed to run: %s. Log at the end of the report in results/\n' \
            "$(set -- $LOADFAIL; IFS=,; echo "$*" | sed 's/,/, /g')" |
            note | while IFS= read -r l; do red "$l"; done
    fi
    if [ "${#REFUSED[@]}" != 0 ]; then
        printf -- '- not done due to limitations of the current window manager: %s. Log at the end of the report in results/\n' \
            "$( IFS=,; echo "${REFUSED[*]}" | sed 's/,/, /g')" | note
    fi
fi

# compare_results.sh reads these lines and not the table, whose columns move
# whenever the wording does. It has to stay last: this is where tee_report
# cuts the screen off, so the block goes to the file only.
if [ "${#DATA[@]}" != 0 ]; then
    echo
    echo "== data"
    for r in "${DATA[@]}"; do
        echo "row: $r"
    done
fi

# Below "== data" so none of it reaches the screen. A refused row keeps its log
# too: the reason the desktop gave is in there and nowhere else.
if [ -n "$LOADFAIL" ] || [ "${#REFUSED_LOGS[@]}" != 0 ]; then
    echo
    echo "== logs"
    for t in $LOADFAIL ${REFUSED_LOGS[@]+"${REFUSED_LOGS[@]}"}; do
        [ "$t" = stress ] && set -- bm-s-*.log || set -- "bm-$t.log"
        for l in "$@"; do
            [ -s "$l" ] || continue
            echo
            echo "--- $l"
            cat "$l"
        done
    done
fi
rm -f bm-*.log
end_report
