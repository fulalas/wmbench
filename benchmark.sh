#!/bin/bash
# Benchmark the window manager this session is running, whichever it is:
# builds the tools, detects the compositor, measures, prints one table.
# Run it in each desktop session you want to compare, and put the tables
# side by side.
set -u
ALL="idle usage move resize dnd popups render fullscreen uncapped stress"

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
  usage     a scripted person: maximize, minimize, snap to sides and
            corners, scroll, resize, drag and drop, raise, fullscreen
  render    1200 frames of a GL window held to 60 fps
  fullscreen  the same frames in a fullscreen window, twice: as it comes, and
            asking compositing to step aside with _NET_WM_BYPASS_COMPOSITOR
            the way a player does. Measured in power, not frames: leaving a
            window out of compositing saves the compositor's work, not the
            application's, so the frame rate cannot see it
  uncapped  the frame rate flat out, windowed. Runs last
  stress    everything at once: many windows, one moving, one resizing,
            popups, a translucent window and a GL render. Each does the same
            amount of work it does in its own row above, and the row ends when
            the last of them has finished it - not on the clock

Every test does the same fixed amount of work in every session, however long
that takes, so power and processor time compare directly; the time it took is
a result, not a setting. The one exception is uncapped, which is deliberately
flat out and reports only frames a second.

The result is printed and saved under results/.
EOF
}
case "${1:-}" in -h|--help) usage_help; exit 0;; esac

TESTS=${*:-$ALL}
for t in $TESTS; do
    case "$t" in idle|usage|move|resize|dnd|popups|render|fullscreen|uncapped|stress) ;;
        *) echo "unknown test: $t"; echo; usage_help; exit 1;;
    esac
done
want () { case " $TESTS " in *" $1 "*) return 0;; *) return 1;; esac; }

# How much work each row does. Same numbers in every session: that is the
# whole point - the time it takes is the result, not the setting.
IDLE_WINDOW=${IDLE_WINDOW:-20}          # the baseline has no tasks to count
MOVE_STEPS=${MOVE_STEPS:-2400}
POP_CYCLES=${POP_CYCLES:-400}
RENDER_FRAMES=${RENDER_FRAMES:-1200}
RESIZE_CYCLES=${RESIZE_CYCLES:-2}
DND_PASSES=${DND_PASSES:-1}
USAGE_PASSES=${USAGE_PASSES:-1}
UNCAP_SECONDS=${UNCAP_SECONDS:-16}      # the one row that is timed, by design


source "$(dirname "$0")/common.sh"
cd "$(dirname "$0")" || exit 1

make -s all 2>/dev/null || make all || exit 1

detect_wm || { echo "cannot tell what window manager this is"; exit 1; }
ST=$(session_type)

# Everything below goes to the screen and to a result file
DE=$(echo "${XDG_CURRENT_DESKTOP:-unknown}" | tr '[:upper:]' '[:lower:]' | tr ':/ ' '-')
# The version is in the name too, so two builds of the same window manager
# can be told apart; dashes in it would break the fields, so they become dots
WMVER=$(wm_version | tr '-' '.')
OUT="results/benchmark-$(date +%Y%m%d-%H%M%S)-$(hostname)-$DE-$WM_NAME-${WMVER:-unknown}-$ST.txt"
mkdir -p results
exec > >(tee "$OUT") 2>&1

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
[ -n "$PWR" ] && echo "power:      $PWR" || echo "power:      no sensor, not reported"
[ "$COMPOSITING" = OFF ] &&
    echo "            these numbers are the no-compositing floor"
echo
# Roughly how long the fixed work takes on a healthy machine
EST=0
for t in $TESTS; do
    case "$t" in
        idle)     EST=$((EST + IDLE_WINDOW + 4));;
        usage)    EST=$((EST + 70));;
        move)     EST=$((EST + MOVE_STEPS / 120 + 10));;
        resize)   EST=$((EST + RESIZE_CYCLES * 18 + 10));;
        dnd)      EST=$((EST + DND_PASSES * 20 + 10));;
        popups)   EST=$((EST + POP_CYCLES / 20 + 10));;
        render)   EST=$((EST + RENDER_FRAMES / 60 + 10));;
        fullscreen) EST=$((EST + 2 * (RENDER_FRAMES / 60 + 10)));;
        uncapped) EST=$((EST + UNCAP_SECONDS + 6));;
        stress)   EST=$((EST + RENDER_FRAMES / 60 + 30));;
    esac
done
echo "running tests — do not use the computer (~$(( (EST + 59) / 60 )) min)"

wm_cpu () { [ -n "$WM_PID" ] && cpu_of "$WM_PID" 2>/dev/null || echo 0; }
now_s () { date +%s.%N; }

# The idle baseline: no tasks to count, so a fixed window everywhere.
# Prints "watts cpu_seconds duration".
idle_window () {
    local c0 c1 n=0 sum=0 w end=$((SECONDS + $1))

    c0=$(wm_cpu)
    while [ $SECONDS -lt $end ]; do
        [ -n "$PWR" ] && { sum=$((sum + $(cat "$PWR" 2>/dev/null || echo 0)))
                           n=$((n + 1)); }
        sleep 0.1
    done
    c1=$(wm_cpu)
    if [ -n "$PWR" ]; then
        w=$(awk -v s=$sum -v n=$n 'BEGIN{printf "%.2f", (n ? s/n/1e6 : 0)}')
    else
        w="-"
    fi
    awk -v a=$c0 -v b=$c1 -v w="$w" -v t="$1" \
        'BEGIN{printf "%s %.2f %.1f", w, (b-a)/1000, t}'
}

# A fixed amount of work, however long it takes. The tool announces when its
# warm-up is over and when the last task is done; everything in between is the
# measurement. Prints "watts cpu_seconds duration", or fails if the load did
# not run. Nothing is ever cut short on time.
measure () {                    # $1 row name, $2 task count, $3... the load
    local name=$1 tasks=$2 log pid t0 t1 c0 c1 n=0 sum=0 w
    shift 2
    log="bm-$name.log"
    : > "$log"
    BENCH_TASKS=$tasks "$@" >> "$log" 2>&1 &
    pid=$!

    while ! grep -q MEASURE-START "$log" 2>/dev/null; do
        kill -0 "$pid" 2>/dev/null || { wait "$pid"; return 1; }
        sleep 0.1
    done
    t0=$(now_s); c0=$(wm_cpu)
    while ! grep -q MEASURE-END "$log" 2>/dev/null; do
        kill -0 "$pid" 2>/dev/null || break
        [ -n "$PWR" ] && { sum=$((sum + $(cat "$PWR" 2>/dev/null || echo 0)))
                           n=$((n + 1)); }
        sleep 0.1
    done
    t1=$(now_s); c1=$(wm_cpu)
    wait "$pid" || return 1
    grep -q MEASURE-END "$log" || return 1

    if [ -n "$PWR" ]; then
        w=$(awk -v s=$sum -v n=$n 'BEGIN{printf "%.2f", (n ? s/n/1e6 : 0)}')
    else
        w="-"
    fi
    awk -v a=$c0 -v b=$c1 -v w="$w" -v t0="$t0" -v t1="$t1" \
        'BEGIN{printf "%s %.2f %.1f", w, (b-a)/1000, t1-t0}'
}

IDLE=""; USAGE=""; MOVE=""; RESIZE=""; DND=""; POP=""; RENDER=""; UNCAP=""
FPS=""; FS=""; FS_ASK=""; STRESS=""; LOADFAIL=""

# The live line goes to the terminal itself, not through the file: the run can
# be watched, and what is saved is only the finished rows.
exec 3>/dev/tty 2>/dev/null || exec 3>/dev/null
running () {                    # $1 the name of the test now under way
    printf '\033[2K%s (running...)\r' "$1" >&3
}
done_running () { printf '\033[2K\r' >&3; }

RULE=$(printf '\u2500%.0s' $(seq 61))
print_row () {                  # $1 label, $2 "watts cpu_s seconds"
    local w c t
    done_running
    [ -n "$2" ] || return 0
    read -r w c t <<< "$2"
    if [ "$w" = "-" ]; then
        printf '%-17s%14s%16s%14s\n' "$1" "-" "$c s" "$t s"
    else
        printf '%-17s%14s%16s%14s\n' "$1" "$w W" "$c s" "$t s"
    fi
}

# Each row is printed the moment its test is over, so the run can be watched,
# and counted towards the total as it goes. Seconds add; power does not -
# watts are a rate, so they are averaged over the time they were drawn. Idle
# is the baseline and no part of the total.
TCPU=0; TTIME=0; TENERGY=0; TROWS=0; TPOWER=1
tally () {                      # $1 label, $2 "watts cpu_s seconds"
    local rw rc rt
    [ -n "$2" ] || return 0
    print_row "$1" "$2"
    read -r rw rc rt <<< "$2"
    [ "$rw" = "-" ] && TPOWER=0
    TROWS=$((TROWS + 1))
    read -r TCPU TTIME TENERGY <<< "$(awk -v c="$TCPU" -v t="$TTIME" \
        -v e="$TENERGY" -v rc="$rc" -v rt="$rt" -v rw="$rw" 'BEGIN{
            printf "%.2f %.1f %.3f", c + rc, t + rt,
                   e + ((rw == "-") ? 0 : rw * rt)}')"
}

echo
printf '%-17s%14s%16s%14s\n' workload power "compositor CPU" "time elapsed"

want idle && running "idle" && IDLE=$(idle_window "$IDLE_WINDOW") && print_row "idle" "$IDLE"

if want move; then
    running "move"
    MOVE=$(measure move "$MOVE_STEPS" ./tools/movebench 0 move 120) ||
        LOADFAIL="$LOADFAIL move"
    tally "move" "$MOVE"
fi
if want resize; then
    running "resize"
    RESIZE=$(measure resize "$RESIZE_CYCLES" ./tools/usagebench 0 3 resize) ||
        LOADFAIL="$LOADFAIL resize"
    tally "resize" "$RESIZE"
fi
if want dnd; then
    running "drag and drop"
    DND=$(measure dnd "$DND_PASSES" ./tools/usagebench 0 3 dnd) ||
        LOADFAIL="$LOADFAIL dnd"
    tally "drag and drop" "$DND"
fi
if want popups; then
    running "popups"
    POP=$(measure popups "$POP_CYCLES" ./tools/popbench 0 20 6) ||
        LOADFAIL="$LOADFAIL popups"
    tally "popups" "$POP"
fi
if want usage; then
    running "usage"
    USAGE=$(measure usage "$USAGE_PASSES" ./tools/usagebench 0 3) ||
        LOADFAIL="$LOADFAIL usage"
    tally "usage" "$USAGE"
fi
if want render; then
    running "render 60 fps"
    RENDER=$(measure render "$RENDER_FRAMES" ./tools/fsbench2 0 windowed 60) ||
        LOADFAIL="$LOADFAIL render"
    tally "render 60 fps" "$RENDER"
fi

# The pair that says what leaving a window out of compositing is worth. The
# cheap shader is used on purpose: with the heavy one the application costs
# some 14 ms a frame at 4K and compositing well under one, so the difference
# disappears into the noise. A fullscreen window is also created already
# covering the monitor, because that is when a window manager decides.
if want fullscreen; then
    running "fullscreen"
    FS=$(measure fullscreen "$RENDER_FRAMES" \
         env BENCH_LIGHT=1 ./tools/fsbench2 0 fullscreen 60) ||
        LOADFAIL="$LOADFAIL fullscreen"
    tally "fullscreen" "$FS"
    running "fullscreen asked"
    FS_ASK=$(measure fullscreen-asked "$RENDER_FRAMES" \
             env BENCH_LIGHT=1 BENCH_BYPASS=1 ./tools/fsbench2 0 fullscreen 60) ||
        LOADFAIL="$LOADFAIL fullscreen-asked"
    tally "fullscreen asked" "$FS_ASK"
fi

# Last on purpose: everything at once heats the chip and would warm up
# whatever ran after it. Nothing here ends on the clock either: every
# component has its own fixed number of tasks, and the row is over when the
# last of them has finished the lot.
if want stress; then
    running "stress"
    # The counts are set so that at their fixed rates all five have the same
    # amount of work to get through as the render does, and all five pace
    # themselves the same way, so on a slow machine they fall behind together
    # and still finish together. That is why the window being resized here is
    # movebench and not the scripted person: a pass of the latter is some ten
    # seconds long whatever the count, so it could only ever land near the
    # others by luck. manywin only puts windows on the screen: it is scenery,
    # it counts nothing, and it is killed at the end rather than ending
    # anything.
    #
    # Everything is started first and then held at the gate, so the stack can
    # be arranged and no task at all is done before the whole load is up. See
    # gate.c.
    GO="$PWD/bm-stress.go"; rm -f "$GO"
    S_MOVE=$((2 * RENDER_FRAMES))                 # 120 a second
    S_POP=$(( (RENDER_FRAMES + 2) / 3 ))          # 20 a second
    S_TRANS=$RENDER_FRAMES                        # 60 a second
    S_RESIZE=$((2 * RENDER_FRAMES))               # 120 a second
    ./tools/manywin 12 > bm-many.log 2>&1 &
    MANY=$!
    SL=(); SP=(); SEND=()
    load () {                   # $1 log, $2 tasks, $3... the program
        local log=$1 tasks=$2
        shift 2
        : > "$log"
        BENCH_GO="$GO" BENCH_TASKS=$tasks "$@" >> "$log" 2>&1 &
        SL+=("$log"); SP+=("$!")
    }
    load bm-smove.log   "$S_MOVE"   ./tools/movebench 0 move 120
    load bm-spop.log    "$S_POP"    ./tools/popbench 0 20 4
    load bm-strans.log  "$S_TRANS"  ./tools/transbench 0 0.75 60
    load bm-sresize.log "$S_RESIZE" ./tools/movebench 0 resize 120
    load bm-srender.log "$RENDER_FRAMES" ./tools/fsbench2 0 windowed 60

    # Wait until every one of them is set up and waiting at the gate. A
    # program that is still drawing its content when the gate opens is not
    # held by it, so it would start as late as its setup took and the whole
    # mix would be staggered by that much. See gate.c.
    for i in "${!SL[@]}"; do
        while ! grep -q MEASURE-READY "${SL[$i]}" 2>/dev/null; do
            kill -0 "${SP[$i]}" 2>/dev/null || break
            sleep 0.1
        done
    done

    # Everything is on screen now, so the stack can be put in a named order
    ./tools/restack -w "fsbench" "popbench background" "transbench background" \
              "manywin" "movebench resize" "movebench" \
              "transbench translucent" >/dev/null
    : > "$GO"

    for l in "${SL[@]}"; do
        while ! grep -q MEASURE-START "$l" 2>/dev/null; do sleep 0.1; done
    done
    T0=$(now_s); C0=$(wm_cpu); N=0; SUM=0
    OK=1; LEFT=${#SL[@]}
    while [ "$LEFT" -gt 0 ]; do
        [ -n "$PWR" ] && { SUM=$((SUM + $(cat "$PWR" 2>/dev/null || echo 0)))
                           N=$((N + 1)); }
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
    T1=$(now_s); C1=$(wm_cpu)
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
        if [ -n "$PWR" ]; then
            W=$(awk -v s=$SUM -v n=$N 'BEGIN{printf "%.2f", (n ? s/n/1e6 : 0)}')
        else
            W="-"
        fi
        STRESS=$(awk -v a=$C0 -v b=$C1 -v w="$W" -v t0="$T0" -v t1="$T1" \
                 'BEGIN{printf "%s %.2f %.1f", w, (b-a)/1000, t1-t0}')
    else
        LOADFAIL="$LOADFAIL stress"
    fi
    tally "stress" "$STRESS"
    [ -n "$STRESS" ] && [ -n "$STRESS_THIN" ] &&
        echo "stress: one of the six finished early, the last $STRESS_THIN% of it" \
             "ran with less on screen"
fi

# Last of all, the only timed test: how many frames the session can deliver
# flat out. Nothing else is read from it, so no power or processor time.
# Three runs, because a fullscreen window is where a compositor can step out
# of the way: windowed, then fullscreen, then fullscreen asking to be left
# alone with _NET_WM_BYPASS_COMPOSITOR, which is what a player sets. The
# three together say whether this desktop stops compositing for a fullscreen
# window at all, and whether it honours being asked.
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

if [ "$TROWS" -gt 0 ] && [ -z "$LOADFAIL" ]; then
    echo "$RULE"
    if [ "$TPOWER" = 1 ]; then
        printf '%-17s%14s%16s%14s\n' total \
               "$(awk -v e="$TENERGY" -v t="$TTIME" \
                  'BEGIN{printf "%.2f W (avg.)", (t ? e / t : 0)}')" \
               "$TCPU s" "$TTIME s"
    else
        printf '%-17s%14s%16s%14s\n' total "-" "$TCPU s" "$TTIME s"
    fi
    # The one number for all of it: watts spent over the seconds they were
    # spent in. Idle is the baseline and no part of it.
    [ "$TPOWER" = 1 ] && awk -v e="$TENERGY" \
        'BEGIN{printf "\nenergy:  %.0f J to do all the work above\n", e}'
fi

if [ -n "$FPS" ]; then
    echo
    echo "speed:   $FPS fps windowed, flat out"
fi

# Only where there is a battery to spend
BAT=$(cat /sys/class/power_supply/BAT*/energy_full 2>/dev/null | head -1)
if [ -n "$PWR" ] && [ -n "$BAT" ] && [ -n "$IDLE" ] && [ -n "$USAGE" ]; then
    read -r iw ic it <<< "$IDLE"
    read -r uw uc ut <<< "$USAGE"
    awk -v i="$iw" -v u="$uw" -v uwh="$BAT" 'BEGIN{
        printf "battery: %s W idle to %s W in constant use - about %.1f h to",
               i, u, uwh / 1e6 / i;
        printf " %.1f h\n         on this %.0f Wh battery\n",
               uwh / 1e6 / u, uwh / 1e6}'
fi

if [ "$TROWS" -gt 0 ] && [ -z "$LOADFAIL" ] && [ "$ST" = x11 ]; then
    echo
    echo "On X11 part of the compositing happens inside the X server, whose"
    echo "processor time is not measured here."
fi
if [ -n "$LOADFAIL" ]; then
    echo "FAILED to run:$LOADFAIL - the rows are missing above; their logs"
    echo "are kept as bm-*.log"
else
    rm -f bm-*.log
fi
