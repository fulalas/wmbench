#!/bin/bash
# Benchmark the window manager this session is running, whichever it is:
# builds the tools, detects the compositor, measures, prints one table.
# Run it in each desktop session you want to compare, and put the tables
# side by side.
set -u
ALL="idle move resize dnd popups video argb windows scroll render fullscreen uncapped stress"

usage_help () {
    cat <<EOF
benchmark the window manager this session is running

  ./benchmark.sh [test ...]     run everything, or only the tests named

tests:
  idle      nothing happening
  move      a window moved 2400 steps at 120 a second
  resize    two full cycles of resizing by every handle, corner and edge
  dnd       drag and drop: one pass of icons over, then all back at once
  popups    400 menus appearing and disappearing at 20 a second
  video     1200 frames handed over through shared memory, the way a player
            does, at 60 a second
  argb      2400 redraws of a transparent window that declares an opaque
            region, which is what every GTK window is, at 120 a second
  windows   maximize, minimize, snap to the sides and corners, raise
            two windows over each other, fullscreen and back
  scroll    a text document scrolled by the chevrons, then by dragging
            the thumb the whole way and back
  render    1200 frames of a GL window held to 60 fps
  fullscreen  the same frames in a fullscreen window, twice: as it comes, and
            asking compositing to step aside with _NET_WM_BYPASS_COMPOSITOR
            the way a player does. Measured in power, not frames: leaving a
            window out of compositing saves the compositor's work, not the
            application's, so the frame rate cannot see it
  uncapped  the frame rate flat out, windowed. Runs last
  stress    everything at once: many windows, one moving, one resizing,
            popups, a translucent window and a GL render. Each does the same
            amount of work it does in its own test above, and it ends when
            the last of them has finished it - not on the clock

Every test does the same fixed amount of work in every session, however long
that takes, so power and CPU time compare directly; the time it took is
a result, not a setting. The one exception is uncapped, which is deliberately
flat out and reports only frames a second.

A test the desktop refuses - a compositor that will not move a window it
manages, for one - is left empty and named at the end. An empty line says
nothing; a small number for a window that never moved reads as the best
result in the table.

The result is printed and saved under results/.
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

# How much work each row does. Same numbers in every session: that is the
# whole point - the time it takes is the result, not the setting.
IDLE_WINDOW=${IDLE_WINDOW:-20}          # the baseline has no tasks to count
MOVE_STEPS=${MOVE_STEPS:-2400}
POP_CYCLES=${POP_CYCLES:-400}
VIDEO_FRAMES=${VIDEO_FRAMES:-1200}
ARGB_STEPS=${ARGB_STEPS:-2400}
RENDER_FRAMES=${RENDER_FRAMES:-1200}
RESIZE_CYCLES=${RESIZE_CYCLES:-2}
DND_PASSES=${DND_PASSES:-1}
WINDOWS_PASSES=${WINDOWS_PASSES:-1}
SCROLL_PASSES=${SCROLL_PASSES:-1}
UNCAP_SECONDS=${UNCAP_SECONDS:-16}      # the one row that is timed, by design


source "$(dirname "$0")/lib/common.sh"
cd "$(dirname "$0")" || exit 1

make -s all 2>/dev/null || make all || exit 1

# Ask for the CPU sensor before measuring, not half way through
power_unlock

detect_wm || { echo "cannot tell what window manager this is"; exit 1; }
ST=$(session_type)

# Everything below goes to the screen and to a result file
DE=$(echo "${XDG_CURRENT_DESKTOP:-unknown}" | tr '[:upper:]' '[:lower:]' | tr ':/ ' '-')
# The version is in the name too, so two builds of the same window manager
# can be told apart; dashes in it would break the fields, so they become dots
WMVER=$(wm_version | tr '-' '.')
OUT="results/benchmark-$(date +%Y%m%d-%H%M%S)-$(hostname)-$DE-$WM_NAME-${WMVER:-unknown}-$ST.txt"
mkdir -p results
tee_report "$OUT"

# Ctrl-C at any moment: take the running benchmark, the power watcher and
# the tee down with us. The whole process group when we lead it (the normal
# terminal case), only our own children when someone else does.
on_int () {
    trap - INT TERM
    if [ "$(ps -o pgid= -p $$ | tr -d ' ')" = "$$" ]; then
        kill -- -$$ 2>/dev/null
    else
        kill $(jobs -p) 2>/dev/null
    fi
    exit 130
}
trap on_int INT TERM

echo "system:     $(hostname) ($(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}"))"
echo "kernel:     $(uname -r)"
echo "display:    $(display_info)"
echo "session:    $ST (${XDG_CURRENT_DESKTOP:-unknown desktop})"
# Is anything actually compositing? A session without a compositor is a fair
# thing to measure, but it is not the same measurement, and saying so is the
# difference between a result and a silent mistake. On X11 the compositor
# announces itself by owning a selection, which has to be asked for by name
# and cannot be read off the root window; a Wayland compositor always
# composites.
if [ "$ST" != x11 ] || ./tools/cmcheck >/dev/null 2>&1; then
    COMPOSITING=on
else
    COMPOSITING=OFF
fi
echo "compositor: $WM_NAME $(wm_version) (compositing $COMPOSITING)"
[ -z "$WM_PID" ] &&
    echo "            its process cannot be seen, so the CPU column is" &&
    echo "            left empty rather than filled with zeros"
[ "$POWER_OK" = 1 ] && echo "power:      $POWER_DESC" \
                     || echo "power:      no sensor, not reported"
[ "$COMPOSITING" = OFF ] &&
    echo "            these numbers are the no-compositing floor"
echo
# Roughly how long the fixed work takes on a healthy machine
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

# What compositing costs this session: the window manager's own process, and
# nothing else. The X server is deliberately left out on X11. It does part of
# the compositing, but it also does every client's drawing - the loads here
# push their pixels through it - and on Wayland that same drawing happens
# inside each program and is not counted anywhere. Adding the server would not
# make the two session types comparable; it would tilt X11 the other way.
comp_cpu () {
    wm_cpu
}

# CPU seconds between two wm_cpu readings, or "-" when either reading is
# missing (no visible process) or the counter went backwards (the process was
# replaced while we watched)
cpu_delta () {                  # $1 before, $2 after
    awk -v a="$1" -v b="$2" 'BEGIN{
        print (a == "" || b == "" || b < a) ? "-" : sprintf ("%.2f", (b - a) / 1000)}'
}

# The idle baseline: no tasks to count, so a fixed window everywhere.
# Prints "watts cpu_seconds duration".
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

# A fixed amount of work, however long it takes. The tool announces when its
# warm-up is over and when the last task is done; everything in between is the
# measurement. Prints "watts cpu_seconds duration", or fails if the load did
# not run. Nothing is ever cut short on time.
measure () {                    # $1 row name, $2 task count, $3... the load
    local name=$1 tasks=$2 log pid t0 t1 c0 c1 w
    shift 2
    log="bm-$name.log"
    : > "$log"
    BENCH_TASKS=$tasks "$@" >> "$log" 2>&1 &
    pid=$!

    while ! grep -q MEASURE-START "$log" 2>/dev/null; do
        kill -0 "$pid" 2>/dev/null || { wait "$pid"; return 1; }
        sleep 0.1
    done
    t0=$(now_s); c0=$(comp_cpu); power_begin
    while ! grep -q MEASURE-END "$log" 2>/dev/null; do
        kill -0 "$pid" 2>/dev/null || break
        power_sample
        sleep 0.1
    done
    t1=$(now_s); c1=$(comp_cpu); w=$(power_end)
    wait "$pid"; rc=$?
    # 3 is the load saying the desktop refused to do the thing being measured.
    # It ran and it finished; there is simply nothing here worth a number.
    [ "$rc" = 3 ] && return 3
    [ "$rc" = 0 ] || return 1
    grep -q MEASURE-END "$log" || return 1

    awk -v w="$w" -v c="$(cpu_delta "$c0" "$c1")" -v t0="$t0" -v t1="$t1" \
        'BEGIN{printf "%s %s %.1f", w, c, t1 - t0}'
}

# A row the desktop would not perform. Named, so the table says why it is
# empty instead of leaving a gap, and left out of the totals.
REFUSED=()
ROW_OUT=""
run_row () {                    # $1 label, $2 row name, $3 tasks, $4... load
    local label=$1 name=$2 rc
    shift
    running "$label"
    ROW_OUT=$(measure "$@"); rc=$?
    case $rc in
        0) tally "$label" "$ROW_OUT";;
        3) done_running; ROW_OUT=""
           REFUSED+=("$label")
           # In the data block too, so the comparison keeps the row and shows
           # it empty instead of dropping it as if it had never been asked for
           DATA+=("- - - $label")
           printf '%-17s%14s%16s%14s\n' "$label" "-" "-" "not done";;
        *) done_running; ROW_OUT=""; LOADFAIL="$LOADFAIL $name";;
    esac
}

IDLE=""; WINDOWS=""; SCROLL=""; MOVE=""; RESIZE=""; DND=""; POP=""; RENDER=""; UNCAP=""
VIDEO=""; ARGB=""
FPS=""; FS=""; FS_ASK=""; STRESS=""; LOADFAIL=""

# The live line goes to the terminal itself, not through the file: the run can
# be watched, and what is saved is only the finished rows.
# The braces matter: a bare exec applies every redirection to the shell itself,
# so the silence meant for a failed open would swallow this run's own errors,
# which tee_report has just pointed at the result file.
{ exec 3>/dev/tty; } 2>/dev/null || exec 3>/dev/null
running () {                    # $1 the name of the test now under way
    printf '\033[2K%s (running...)\r' "$1" >&3
}
done_running () { printf '\033[2K\r' >&3; }

RULE=$(printf '\u2500%.0s' $(seq 61))
# The table is for people. Every row is also kept as one plain line, printed
# together at the end for compare_results.sh to read: the columns above move
# whenever the wording does, and a reader that has to work out where they are
# drops rows silently when it guesses wrong.
DATA=()
print_row () {                  # $1 label, $2 "watts cpu_s seconds"
    local w c t
    done_running
    [ -n "$2" ] || return 0
    read -r w c t <<< "$2"
    DATA+=("$w $c $t $1")
    [ "$w" = "-" ] || w="$w W"
    [ "$c" = "-" ] || c="$c s"
    printf '%-17s%14s%16s%14s\n' "$1" "$w" "$c" "$t s"
}

# Each row is printed the moment its test is over, so the run can be watched,
# and counted towards the total as it goes. Seconds add; power does not -
# watts are a rate, so they are averaged over the time they were drawn. Idle
# is the baseline and no part of the total.
TCPU=0; TTIME=0; TENERGY=0; TROWS=0; TPOWER=1; TCPUOK=1
tally () {                      # $1 label, $2 "watts cpu_s seconds"
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
printf '%-17s%14s%16s%14s\n' workload power "compositor CPU" "time elapsed"

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

# The pair that says what leaving a window out of compositing is worth. The
# cheap shader is used on purpose: with the heavy one the application costs
# some 14 ms a frame at 4K and compositing well under one, so the difference
# disappears into the noise. A fullscreen window is also created already
# covering the monitor, because that is when a window manager decides.
if want fullscreen; then
    run_row "fullscreen" fullscreen "$RENDER_FRAMES" \
        env BENCH_LIGHT=1 ./tools/fsbench2 0 fullscreen 60
    FS=$ROW_OUT
    run_row "fullscreen asked" fullscreen-asked "$RENDER_FRAMES" \
        env BENCH_LIGHT=1 BENCH_BYPASS=1 ./tools/fsbench2 0 fullscreen 60
    FS_ASK=$ROW_OUT
fi

# Last on purpose: everything at once heats the chip and would warm up
# whatever ran after it. Nothing here ends on the clock either: every
# component has its own fixed number of tasks, and the row is over when the
# last of them has finished the lot.
if want stress; then
    running "stress"
    # The window being resized is movebench and not the scripted person: a pass
    # of the latter is some ten seconds long whatever the count, so it could
    # only ever land near the others by luck. manywin only puts windows on the
    # screen: it is scenery, it counts nothing, and it is killed at the end
    # rather than ending anything.
    GO="$PWD/bm-stress.go"; rm -f "$GO"
    # The mix itself, the work each load does and the order the windows are
    # opened in all live in lib/common.sh, so validate.sh means the same load
    # by it. They go up one at a time, in that order, so the stack is the
    # order they were opened and nothing has to be rearranged afterwards.
    stress_start "$GO" bm-s
    MANY=$STRESS_SCENERY_PID
    SL=("${STRESS_LOGS[@]}"); SP=("${STRESS_PIDS[@]}"); SEND=()
    stress_wait_ready

    STACK_OK=1
    stress_settle || STACK_OK=0
    [ "${STRESS_LATE:-0}" = 1 ] && STACK_OK=0
    : > "$GO"

    for i in "${!SL[@]}"; do
        while ! grep -q MEASURE-START "${SL[$i]}" 2>/dev/null; do
            # A load that died before the gate would never write it, and the
            # loop below is what turns that into a failed row
            kill -0 "${SP[$i]}" 2>/dev/null || break
            sleep 0.1
        done
    done
    T0=$(now_s); C0=$(comp_cpu); power_begin
    OK=1; LEFT=${#SL[@]}
    while [ "$LEFT" -gt 0 ]; do
        power_sample
        sleep 0.1
        LEFT=0
        for i in "${!SL[@]}"; do
            [ -n "${SEND[$i]:-}" ] && continue
            if grep -q MEASURE-END "${SL[$i]}" 2>/dev/null; then
                SEND[$i]=$(now_s)
            elif ! kill -0 "${SP[$i]}" 2>/dev/null; then
                # Gone with its work unfinished: the row is not a result
                SEND[$i]=$(now_s); OK=0
            else
                LEFT=$((LEFT + 1))
            fi
        done
    done
    T1=$(now_s); C1=$(comp_cpu)
    for p in "${SP[@]}" $MANY; do kill "$p" 2>/dev/null; done
    for p in "${SP[@]}" $MANY; do wait "$p" 2>/dev/null; done
    rm -f "$GO"
    # A component that ran out well before the last one leaves the others
    # composited on a thinner screen for a while. The work is the same either
    # way, but it is worth saying when it happens.
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
        # The window was opened before the loop. Left open, the sensor's own
        # background sampler keeps running through the test that follows and
        # the next power_begin loses the handle on it.
        power_end > /dev/null
        LOADFAIL="$LOADFAIL stress"
    fi
    # A load in the mix whose window never moved leaves the whole scene
    # different from every other session's, and there is nothing here worth a
    # number. A stack order that would not take is only worth saying.
    MIX_OK=$STACK_OK
    for l in "${SL[@]}"; do
        grep -q MOVE-NEVER-HAPPENED "$l" 2>/dev/null && MIX_OK=0
    done
    if [ "$MIX_OK" = 0 ] && [ -n "$STRESS" ]; then
        done_running
        REFUSED+=("stress")
        DATA+=("- - - stress")
        printf '%-17s%14s%16s%14s\n' stress "-" "-" "not done"
        STRESS=""
    fi
    tally "stress" "$STRESS"
    [ -n "$STRESS" ] && [ -n "$STRESS_THIN" ] &&
        echo "stress: one of the six finished early, the last $STRESS_THIN% of it" \
             "ran with less on screen"
fi

# Last of all, the only timed test: how many frames the session can deliver
# flat out. Nothing else is read from it, so no power or CPU time.
# One run, windowed. What a fullscreen window is worth, and whether being
# asked with _NET_WM_BYPASS_COMPOSITOR is honoured, is the fullscreen pair
# above, which is measured in power rather than frames.
if want uncapped; then
    speed_run () {              # $1 log name, $2... fsbench2 arguments
        local log=$1
        shift
        # A window mapped while the compositor is still catching up with the
        # last test is not composited, and the frame rate then measures
        # nothing
        sleep 3
        ./tools/fsbench2 "$@" > "bm-$log.log" 2>&1 || true
        awk '/AVERAGE/{print $2}' "bm-$log.log" 2>/dev/null
    }
    running "speed"
    FPS=$(speed_run uncapped "$UNCAP_SECONDS" windowed)
    done_running
    [ -n "$FPS" ] || LOADFAIL="$LOADFAIL uncapped"
fi

# A total over fewer rows is a smaller total. Printing one for a session that
# would not perform some of the work would hand it the win in every column it
# skipped - the same mistake as printing a small number for a window that
# never moved, one level up.
if [ "$TROWS" -gt 0 ] && [ -z "$LOADFAIL" ] && [ "${#REFUSED[@]}" != 0 ]; then
    echo "$RULE"
    [ "${#REFUSED[@]}" = 1 ] && N=test || N=tests
    echo "no total: ${#REFUSED[@]} $N not done"
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
    printf '%-17s%14s%16s%14s\n' total "$TW" "$TC" "$TTIME s"
    # The one number for all of it: watts spent over the seconds they were
    # spent in. Idle is the baseline and no part of it.
    [ "$TPOWER" = 1 ] && awk -v e="$TENERGY" \
        'BEGIN{printf "\nenergy:  %.0f J to do all the work above\n", e}'
fi

if [ -n "$FPS" ]; then
    echo
    echo "speed:   $FPS fps windowed"
fi

# Only where there is a battery to spend
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

if [ "$TROWS" -gt 0 ] && [ -z "$LOADFAIL" ] && [ "$ST" = x11 ]; then
    echo
    echo "On X11 part of the compositing happens inside the X server, whose"
    echo "CPU time is not counted here: the server also does every program's"
    echo "drawing, which on Wayland happens inside the programs themselves."
    echo "Neither column holds all of it, so read X11 against X11."
fi

# A reader who sees "-" in the table needs to know it is the desktop refusing
# the work, not a broken sensor. Named once, in the words of the table above.
# Why each one was refused is in its bm-*.log, which is kept when this happens.
if [ "${#REFUSED[@]}" != 0 ]; then
    echo
    echo "this environment doesn't allow the following tests to run properly:"
    ( IFS=,; echo "${REFUSED[*]}" ) | sed 's/,/, /g' | fold -s -w 66
fi

if [ -n "$LOADFAIL" ]; then
    red "FAILED to run:$LOADFAIL - the tests are missing above; their logs"
    red "are kept as bm-*.log"
elif [ "${#REFUSED[@]}" = 0 ]; then
    rm -f bm-*.log
fi

# The rows again, for compare_results.sh: watts, compositor seconds, seconds
# elapsed, then the name. A "-" is a column this machine could not measure.
# It goes to the result file only - on screen the table above already said it,
# so this has to stay last, where tee_report cuts the screen off.
if [ "${#DATA[@]}" != 0 ]; then
    echo
    echo "== data"
    for r in "${DATA[@]}"; do
        echo "row: $r"
    done
fi
end_report
