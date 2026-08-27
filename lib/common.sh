# Alternate the arms. Whichever variant runs first in a round comes out cooler
# and wins, whatever it is, so a comparison that does not alternate is worthless
# however tight its samples look.
#
# Compare within one condition. A per-paint CPU or GPU figure is not comparable
# between an idle desktop and a loaded one, or between a capped and an uncapped
# run: the clocks differ.

# A power1_average reads as watts; an energy_uj is a rising counter, so watts
# are its rise over the window divided by the window.
#
# Never hardcode an hwmon index: the numbering is not stable across boots. And
# never fall back to "whatever exposes power1_average", because nvme drives and
# wireless cards expose that file too, and a disk's power draw looks just as
# plausible in the output.
#
# Sensors that cannot cover both are refused rather than reported: a board
# figure with no CPU figure reads like a whole-machine one and is wrong by the
# entire CPU, and there is nothing in a bare number to say so.
find_gpu_power () {
    local h name label want

    # amdgpu first whatever the hwmon numbering says: k10temp is the CPU
    # package, and taken in glob order it can shadow the real graphics sensor
    for want in amdgpu k10temp; do
        for h in /sys/class/hwmon/hwmon*; do
            [ -r "$h/power1_average" ] || continue
            [ -r "$h/name" ] || continue
            read -r name < "$h/name"
            [ "$name" = "$want" ] || continue
            label=none
            [ -r "$h/power1_label" ] && read -r label < "$h/power1_label"
            echo "$h/power1_average $label $name"

            return 0
        done
    done

    return 1
}

# Most kernels keep this readable by root only, so failing to read it is the
# common case and worth saying out loud rather than passing over
POWER_ENERGY_LOCKED=""
find_cpu_energy () {
    local d name

    for d in /sys/class/powercap/*/; do
        [ -r "$d/name" ] || continue
        read -r name < "$d/name"
        case "$name" in
            package-*)
                if [ -r "${d}energy_uj" ]; then
                    echo "${d}energy_uj"

                    return 0
                fi
                POWER_ENERGY_LOCKED="${d}energy_uj"
                ;;
        esac
    done

    return 1
}

# Only a card with its own power supply belongs here. Integrated graphics sit
# inside the CPU package, and so inside the CPU's own figure already, and adding
# this counter to that counts them twice. Integrated always sits at 0000:00:02.0
# on the root bus while a card sits behind a bridge, which is the test below.
find_gpu_energy () {
    local h name dev

    for h in /sys/class/hwmon/hwmon*; do
        [ -r "$h/energy1_input" ] || continue
        [ -r "$h/name" ] || continue
        read -r name < "$h/name"
        case "$name" in i915|xe) ;; *) continue;; esac
        dev=$(basename "$(readlink -f "$h/device" 2>/dev/null)" 2>/dev/null)
        case "$dev" in 0000:00:*) continue;; esac
        echo "$h/energy1_input"

        return 0
    done

    return 1
}

# Streamed for a whole window rather than forked per sample: a fork every tenth
# of a second would show up in the very CPU figures being measured
find_nvidia_power () {
    command -v nvidia-smi >/dev/null 2>&1 || return 1
    nvidia-smi --query-gpu=power.draw --format=csv,noheader,nounits \
        >/dev/null 2>&1 || return 1
    echo nvidia-smi
}

power_choose () {
    POWER_HWMON=""; POWER_LABEL=""; POWER_NAME=""; POWER_ENERGY=""
    POWER_GPU_ENERGY=""
    POWER_NVIDIA=""; POWER_DESC=""; POWER_OK=0; POWER_CPU_WANTED=1
    local gpu="" cpu=""

    read -r POWER_HWMON POWER_LABEL POWER_NAME <<< "$(find_gpu_power)"
    POWER_ENERGY=$(find_cpu_energy)

    if [ "$POWER_LABEL" = PPT ]; then
        # Already covers both, so reading the CPU as well, which is possible
        # whenever RAPL happens to be unlocked, would count it twice
        POWER_ENERGY=""
        POWER_CPU_WANTED=0
        POWER_OK=1
        POWER_DESC="$POWER_HWMON (PPT: CPU and GPU)"

        return 0
    fi

    if [ "$POWER_NAME" = k10temp ]; then
        # k10temp reads the CPU package, not a GPU: reported as GPU it stood
        # in for a graphics figure it never was, and with RAPL on top the same
        # package was counted twice
        POWER_ENERGY=""
        POWER_CPU_WANTED=0
        cpu="$POWER_HWMON (CPU)"
    elif [ -n "$POWER_HWMON" ]; then
        gpu="$POWER_HWMON (GPU)"
    fi
    if [ -z "$gpu" ]; then
        POWER_GPU_ENERGY=$(find_gpu_energy) && gpu="$POWER_GPU_ENERGY (GPU)"
    fi
    if [ -z "$gpu" ]; then
        POWER_NVIDIA=$(find_nvidia_power) && gpu="nvidia-smi (GPU)"
    fi
    [ -n "$POWER_ENERGY" ] && cpu="$POWER_ENERGY (CPU)"

    # A figure missing a part is still worth having between two runs on this
    # machine; what it must never do is read like a whole-machine figure
    if [ -n "$gpu" ] && [ -n "$cpu" ]; then
        POWER_OK=1
        POWER_DESC="$gpu + $cpu"
    elif [ -n "$cpu" ]; then
        POWER_OK=1
        POWER_DESC="$cpu only, the GPU cannot be read"
    elif [ -n "$gpu" ]; then
        POWER_OK=1
        POWER_DESC="$gpu only, the CPU cannot be read"
    else
        echo "Power will not be reported, and nothing will be guessed at." >&2
        echo "  No power sensor: amdgpu exposes a graphics one in sysfs and" >&2
        echo "  k10temp a CPU one, an NVIDIA board needs nvidia-smi, nouveau" >&2
        echo "  has none at all." >&2
        if [ -n "$POWER_ENERGY_LOCKED" ]; then
            echo "  $POWER_ENERGY_LOCKED exists but is root-only. Unlock it with" >&2
            echo "  sudo chmod a+r $POWER_ENERGY_LOCKED" >&2
        else
            echo "  No RAPL package counter for the CPU either." >&2
        fi
    fi
}

power_choose

# Only offered where it would add something: where one figure already covers
# CPU and GPU there is nothing to gain by opening this
power_unlock () {
    local reply

    [ -n "$POWER_ENERGY" ] && return 0
    [ "$POWER_CPU_WANTED" = 1 ] || return 0
    [ -n "$POWER_ENERGY_LOCKED" ] || return 0

    if ! command -v sudo >/dev/null 2>&1; then
        echo "The CPU power sensor is root-only. Open it with:" >&2
        echo "  chmod a+r $POWER_ENERGY_LOCKED" >&2

        return 0
    fi

    if [ ! -t 0 ]; then
        echo "The CPU power sensor is root-only, and there is no terminal to" >&2
        echo "ask on, so the CPU is left out of the power figures. Open it with:" >&2
        echo "  sudo chmod a+r $POWER_ENERGY_LOCKED" >&2

        return 0
    fi

    # Asked before root is involved at all, so nobody meets a password prompt
    # they did not expect
    echo "The CPU power sensor is root-only, so the CPU is missing from the"
    echo "power figures. Opening it needs root, and it then stays open to"
    echo "everyone until you reboot."
    read -r -p "Open it? [y/N] " reply
    case "$reply" in
        [yY]*) ;;
        *) echo "Left closed; the CPU is not in the power figures."

           return 0
           ;;
    esac

    if sudo chmod a+r "$POWER_ENERGY_LOCKED"; then
        power_choose
    else
        echo "Left closed; the CPU is not in the power figures." >&2
    fi
}

# The counter side needs no sampling at all: its rise across the window, over
# the window's length, is the average by definition, at two reads not hundreds
power_begin () {
    POWER_N=0; POWER_SUM=0; POWER_NV_PID=""
    POWER_CPU_E0=""; POWER_GPU_E0=""; POWER_T0=$EPOCHREALTIME
    [ -n "$POWER_ENERGY" ] && read -r POWER_CPU_E0 < "$POWER_ENERGY"
    [ -n "$POWER_GPU_ENERGY" ] && read -r POWER_GPU_E0 < "$POWER_GPU_ENERGY"
    if [ -n "$POWER_NVIDIA" ]; then
        POWER_NV_FILE=$(mktemp)
        nvidia-smi --query-gpu=power.draw --format=csv,noheader,nounits \
            -lms 200 > "$POWER_NV_FILE" 2>/dev/null &
        POWER_NV_PID=$!
    fi
}

# Read with builtins alone: this runs ten times a second inside the window
# being measured, and a fork there would show up in the very figures it is
# taking. A read that fails is no sample, not a zero: counting it would drag
# the average down as if the machine had drawn nothing for a tenth of a second.
power_sample () {
    local v

    [ -n "$POWER_HWMON" ] || return 0
    # stderr is redirected first on purpose: the other way round the shell has
    # not silenced it yet when the open fails, and says so ten times a second
    read -r v 2>/dev/null < "$POWER_HWMON" || return 0
    [ -n "$v" ] || return 0
    POWER_SUM=$((POWER_SUM + v))
    POWER_N=$((POWER_N + 1))
}

power_end () {
    local cpu_e1="" gpu_e1="" t1="" nv=0

    if [ "$POWER_OK" != 1 ]; then
        echo "-"

        return 0
    fi
    t1=$EPOCHREALTIME
    [ -n "$POWER_ENERGY" ] && read -r cpu_e1 < "$POWER_ENERGY"
    [ -n "$POWER_GPU_ENERGY" ] && read -r gpu_e1 < "$POWER_GPU_ENERGY"
    if [ -n "$POWER_NV_PID" ]; then
        kill "$POWER_NV_PID" 2>/dev/null; wait "$POWER_NV_PID" 2>/dev/null
        nv=$(awk 'NF && $1 + 0 == $1 { s += $1; n++ }
                  END { if (n) printf "%.3f", s / n }' "$POWER_NV_FILE")
        rm -f "$POWER_NV_FILE"
        # A sampler that produced nothing is a failed read, not a zero: folding
        # in 0 W would report a GPU that was never measured
        if [ -z "$nv" ]; then
            echo "-"

            return 0
        fi
    fi
    awk -v s="$POWER_SUM" -v n="$POWER_N" -v nv="$nv" \
        -v c0="$POWER_CPU_E0" -v c1="$cpu_e1" \
        -v g0="$POWER_GPU_E0" -v g1="$gpu_e1" \
        -v t0="$POWER_T0" -v t1="$t1" 'BEGIN{
            w = (n ? s / n / 1e6 : 0) + nv;
            if (t1 > t0) {
                # A counter that wrapped, which these do, is not a rise, and
                # a window missing either end is not a window: without the
                # opening reading the whole counter, joules since boot, would
                # be charged to it
                if (c0 != "" && c1 != "" && c1 >= c0)
                    w += (c1 - c0) / (t1 - t0) / 1e6;
                if (g0 != "" && g1 != "" && g1 >= g0)
                    w += (g1 - g0) / (t1 - t0) / 1e6;
            }
            printf "%.2f", w }'
}

# By driver name, never by index: card numbering is not stable across boots
amdgpu_device () {
    local c
    for c in /sys/class/drm/card*/device; do
        [ "$(basename "$(readlink -f "$c/driver" 2>/dev/null)" 2>/dev/null)" = amdgpu ] &&
            { echo "$c"; return 0; }
    done
    return 1
}


# Prints nothing at all when there is no readable process, so a session where
# this cannot be measured says so instead of reporting zero. Quantised to one
# clock tick over the run, so at 16 seconds it cannot resolve better than
# 0.6 ms/s. Read with builtins alone: this sits between reading the counter and
# starting the clock at every measurement boundary, and an exec there would be
# measured along with what it is measuring.
wm_cpu () {
    local p f ticks=""
    for p in ${WM_PIDS:-}; do
        # The process can go between looking and reading, so let the read be
        # the test: no answer is no answer, not a zero
        read -r -a f < "/proc/$p/stat" 2>/dev/null || continue
        [ -n "${f[13]:-}" ] || continue
        ticks=$(( ${ticks:-0} + f[13] + f[14] ))
    done
    [ -n "$ticks" ] && echo $((ticks * 10))
}

# Asked of the running binary itself, so a locally built one answers for
# itself, and only of the binary by name when the process is not visible
wm_version () {
    local exe
    exe=$(readlink -f "/proc/$WM_PID/exe" 2>/dev/null)
    [ -x "$exe" ] || exe=$(command -v "$WM_NAME") || return 0
    { "$exe" --version 2>/dev/null || "$exe" -v 2>/dev/null; } | head -2 |
        grep -oE '[0-9]+\.[0-9]+[0-9A-Za-z.+~-]*' | head -1
}

# On Wayland the compositor is asked, one tool after another until one answers:
# XWayland's xrandr shows only the scaled-down logical view, not the panel
display_info () {
    local res="" scale="" dpi out tool cur hz

    if [ "$(session_type)" = wayland ]; then
        for tool in "wlr-randr" "cosmic-randr list" "kscreen-doctor -o"; do
            command -v "${tool%% *}" >/dev/null || continue
            # Some of these colour their output; strip the escapes first
            out=$($tool 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g') || continue
            # Some mark the current mode with a star instead of a word
            cur=$(echo "$out" | grep -i current | head -1)
            [ -n "$cur" ] || cur=$(echo "$out" |
                grep -oE '[0-9]{3,}x[0-9]{3,}@[0-9]+(\.[0-9]+)?\*' | head -1)
            [ -n "$cur" ] || continue
            res=$(echo "$cur" | grep -oE '[0-9]{3,}x[0-9]{3,}' | head -1)
            [ -n "$res" ] || continue
            hz=$(echo "$cur" |
                 grep -oE '[0-9]+(\.[0-9]+)?[[:space:]]*(Hz|\*)' |
                 grep -oE '^[0-9]+' | tail -1)
            [ -z "$hz" ] && hz=$(echo "$cur" | grep -oE '@[0-9]+' | tr -d @ | head -1)
            scale=$(echo "$out" | grep -iE '^[[:space:]]*Scale' | head -1 |
                    grep -oE '[0-9]+(\.[0-9]+)?' | head -1)
            [ -n "$scale" ] && scale=$(awk -v x="$scale" 'BEGIN{printf "%.2f", x}')
            [ -n "$hz" ] && res="$res @ $hz Hz"
            break
        done
    fi
    # Where none of them answered, the compositor may still answer over D-Bus:
    # the current mode is the one marked is-current, the scale sits on the
    # logical monitor
    if [ "$(session_type)" = wayland ] && [ -z "$res" ]; then
        out=$(gdbus call --session --dest org.gnome.Mutter.DisplayConfig \
                    --object-path /org/gnome/Mutter/DisplayConfig \
                    --method org.gnome.Mutter.DisplayConfig.GetCurrentState \
                    2>/dev/null)
        if [ -n "$out" ]; then
            cur=$(echo "$out" |
                  grep -oE "\('[0-9]+x[0-9]+@[0-9.]+'[^)]*'is-current': <true>" |
                  head -1)
            res=$(echo "$cur" | grep -oE '[0-9]+x[0-9]+' | head -1)
            hz=$(echo "$cur" | grep -oE '@[0-9]+' | head -1 | tr -d @)
            scale=$(echo "$out" |
                    grep -oE '\[\(-?[0-9]+, -?[0-9]+, [0-9]+(\.[0-9]+)?, uint32' |
                    head -1 | awk -F', ' '{printf "%.2f", $3}')
            [ -n "$res" ] && [ -n "$hz" ] && res="$res @ $hz Hz"
        fi
    fi

    if [ -z "$res" ]; then
        res=$(xrandr --current 2>/dev/null | awk '/\*/{
                  for (i = 2; i <= NF; i++)
                      if ($i ~ /\*/) { gsub (/[*+]/, "", $i);
                                       printf "%s @ %.0f Hz", $1, $i; exit }}')
        dpi=$(xrdb -query 2>/dev/null | awk '/Xft.dpi/{print $2; exit}')
        if [ -n "$dpi" ] && [ "$dpi" -gt 0 ] 2>/dev/null; then
            scale=$(awk -v d="$dpi" 'BEGIN{printf "%g", d / 96}')
        fi
        [ "$(session_type)" = wayland ] && [ -n "$res" ] && res="$res (XWayland view)"
    fi
    echo "${res:-unknown}, scale ${scale:-1}"
}

# Only when the output is a terminal: a result file has no use for escape codes
if [ -t 1 ]; then
    RED=$'\033[31m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'; OFF=$'\033[0m'
else
    RED=""; GREEN=""; YELLOW=""; OFF=""
fi
green () { printf '%s%s%s\n' "$GREEN" "$*" "$OFF"; }
red   () { printf '%s%s%s\n' "$RED" "$*" "$OFF"; }

# Line by line, so a run cut short still leaves what it printed, and with the
# colours taken back out on the way into the file. The screen stops at
# "== data": that block is for compare_results.sh to read, not for a person.
tee_report () {
    exec > >(tee >(sed -u 's/\x1b\[[0-9;]*m//g' > "$1") |
             sed -u '/^== data$/,$d') 2>&1
    REPORT_PID=$!
}

# The writers behind the stream need a moment to put the last lines in the file.
# Waiting outright can hang on a stray background job holding the pipe open, so
# the wait is bounded.
end_report () {
    exec 1>&- 2>&-
    [ -n "${REPORT_PID:-}" ] || return 0
    for _ in $(seq 50); do
        kill -0 "$REPORT_PID" 2>/dev/null || return 0
        sleep 0.1
    done
}

# A screen that blanks mid-run leaves the compositor drawing nothing, so the
# tests after it are not measurements of anything. The settings are put back
# exactly as they were afterwards, including when the run is interrupted. Only
# X11 needs this: on Wayland each window inhibits idling through the protocol.
AWAKE_SAVED=""
keep_awake () {
    local q

    command -v xset >/dev/null 2>&1 || return 0
    [ -n "${DISPLAY:-}" ] || return 0
    q=$(xset q 2>/dev/null) || return 0
    # timeout cycle | standby suspend off | whether DPMS was on at all
    AWAKE_SAVED=$(echo "$q" | awk '
        /timeout:/  { t = $2; c = $4 }
        /Standby:/  { sb = $2; su = $4; of = $6 }
        /DPMS is/   { on = ($3 == "Enabled") ? 1 : 0 }
        END { if (t != "") printf "%s %s %s %s %s %s", t, c, sb, su, of, on }')
    xset s off 2>/dev/null
    xset -dpms 2>/dev/null
}

let_sleep () {
    local t c sb su of on

    [ -n "$AWAKE_SAVED" ] || return 0
    read -r t c sb su of on <<< "$AWAKE_SAVED"
    AWAKE_SAVED=""
    xset s "$t" "$c" 2>/dev/null
    [ -n "$sb" ] && xset dpms "$sb" "$su" "$of" 2>/dev/null
    # Spelled out rather than "&& ... || ...": there the second arm also runs
    # when the first one fails, which under XWayland it does, and the screen is
    # left with DPMS off after having had it on
    if [ "$on" = 1 ]; then
        xset +dpms 2>/dev/null
    else
        xset -dpms 2>/dev/null
    fi
    return 0
}

# XDG_SESSION_TYPE lies less than it used to, but a live Wayland socket is the
# fact of the matter
session_type () {
    if [ -n "${WAYLAND_DISPLAY:-}" ]; then echo wayland; else echo x11; fi
}

# What the windows are, which is not what the session is: BENCH_BACKEND=x11 on
# a Wayland session opens them through XWayland. Anything that talks to the
# windows - markers they print, tools that read the stack - goes by this;
# anything about the session itself goes by session_type. Same rule as
# bw_open in lib/win.c, and it has to stay the same rule.
backend_type () {
    if [ "${BENCH_BACKEND:-}" = x11 ]; then echo x11; else session_type; fi
}

# The name is on the window named by _NET_SUPPORTING_WM_CHECK, the pid only
# sometimes: some leave _NET_WM_PID unset. Echoed on two lines, because a name
# has spaces in it often enough and a missing pid leaves nothing.
x11_wm_check () {
    local win name pid
    win=$(xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null |
          grep -oE '0x[0-9a-fA-F]+' | head -1)
    [ -n "$win" ] || return 1
    name=$(xprop -id "$win" _NET_WM_NAME 2>/dev/null |
           sed -n 's/.*= "\(.*\)"$/\1/p')
    [ -n "$name" ] || return 1
    pid=$(xprop -id "$win" _NET_WM_PID 2>/dev/null | grep -oE '[0-9]+$')
    printf '%s\n%s\n' "$pid" "$name"
}

# Nothing in the protocol says which process is the compositor, and a list of
# names only ever knows the ones somebody thought to add, so the owner of the
# display socket is asked instead. Echoes the pid and the name, on two lines.
#
# The listening socket is asked first and the lock file only after: libwayland
# opens the socket with CLOEXEC and the lock file without it, so every program
# the compositor started holds a copy of the lock and only the compositor holds
# the socket. Where the lock is all there is, the compositor is the oldest of
# them, having made it before starting anything.
wayland_socket_owner () {
    local sock ino p c pid name cname
    local -a targets
    [ -n "${WAYLAND_DISPLAY:-}" ] || return 1
    sock=$WAYLAND_DISPLAY
    case "$sock" in
        /*) ;;
        *)  sock="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock";;
    esac

    # State 01 is listening: the same path is on every client's end of it too
    ino=$(awk -v s="$sock" '$6 == "01" && $NF == s { print $7; exit }' \
          /proc/net/unix 2>/dev/null)
    targets=("$sock.lock")
    # The brackets are escaped because -lname takes a glob, and unescaped they
    # would be a one-character class matching any digit of the inode
    [ -n "$ino" ] && targets=("socket:\\[$ino\\]" "${targets[@]}")
    for p in "${targets[@]}"; do
        # One find rather than a readlink per descriptor: this runs before the
        # first measurement, but a thousand forks is still a thousand forks
        pid=$(find /proc/[0-9]*/fd -lname "$p" -print 2>/dev/null |
              awk -F/ '{print $3}' | sort -n | head -1)
        [ -n "$pid" ] || continue
        [ -r "/proc/$pid/comm" ] || continue
        read -r name < "/proc/$pid/comm"
        # Some desktops make the socket in a wrapper and hand the fd to the
        # compositor, and the wrapper then sits idle with the CPU column
        # reading 0.00. A wrapper is named for what it wraps with something on
        # the end, so only a child whose name starts the owner's is taken for
        # one - a compositor's other children must not be
        for c in $(pgrep -P "$pid" 2>/dev/null); do
            [ -r "/proc/$c/comm" ] || continue
            read -r cname < "/proc/$c/comm"
            case "$name" in
                "$cname"?*) pid=$c; name=$cname; break;;
            esac
        done
        printf '%s\n%s\n' "$pid" "$name"

        return 0
    done

    return 1
}

# By name, never by who owns the socket: the X server usually runs as root, and
# root's /proc/pid/fd cannot be read, while /proc/pid/stat can.
#
# More than one server is up more often than not - every Wayland compositor
# starts an Xwayland of its own - so it has to be the one serving our DISPLAY.
# Several that could be it are refused rather than guessed between: the wrong
# one charges a whole desktop's CPU to a session that never spent it.
x_server_pid () {
    local n p d args one="" many=0
    d=${DISPLAY:-}; d=${d#*:}; d=${d%%.*}

    for n in Xorg X Xwayland; do
        for p in $(pgrep -x "$n"); do
            [ -r "/proc/$p/stat" ] || continue
            args=$(tr '\0' '\n' < "/proc/$p/cmdline" 2>/dev/null)
            if [ -n "$d" ] && grep -qx ":$d" <<< "$args"; then
                echo "$p"

                return 0
            fi
            # One naming a display that is not ours is another session's. One
            # naming none stays a candidate: startx says nothing and means :0
            grep -qE '^:[0-9]+$' <<< "$args" && continue
            one=$p; many=$((many + 1))
        done
    done
    [ "$many" = 1 ] || return 1
    echo "$one"
}

# The short name used in the result file names, so no spaces and no capitals
wm_canon () {
    local n=${1,,}

    case "$n" in
        *xfwm*)                 echo xfwm4;;
        *compiz*)               echo compiz;;
        *kwin*)                 echo kwin;;
        *muffin*|*cinnamon*)    echo muffin;;
        *mutter*|*gnome*shell*) echo mutter;;
        *marco*)                echo marco;;
        *metacity*)             echo metacity;;
        *openbox*)              echo openbox;;
        *labwc*)                echo labwc;;
        # Anything else keeps its own name, minus what would break a file name
        *)                      echo "${n//[^a-z0-9.]/_}";;
    esac
}

# Sets WM_NAME, WM_PID and WM_PIDS, which is everything whose CPU time is the
# desktop's.
#
# The running window manager is asked first, because it is the only source that
# keeps up when one replaces another mid-session: the desktop still names the
# one it started with, and going by that reports a session as running something
# that is not in it. Only when nothing answers does the desktop name candidates,
# and then only a name with a live process behind it is taken.
detect_wm () {
    local desktop=${XDG_CURRENT_DESKTOP:-} st p cand="" dcand="" name="" pid=""
    local helpers=""
    st=$(session_type)
    WM_NAME=""; WM_PID=""; WM_PIDS=""

    if [ "$st" = x11 ]; then
        { read -r pid; read -r name; } <<< "$(x11_wm_check)"
    else
        { read -r pid; read -r name; } <<< "$(wayland_socket_owner)"
    fi
    [ -n "$name" ] && WM_NAME=$(wm_canon "$name")
    # A pid nothing can be read from is no pid: containers and some setups
    # hide the process, and it is then worth looking for it by name below
    [ -n "$pid" ] && [ -r "/proc/$pid/stat" ] && WM_PID=$pid

    case "$desktop" in
        *GNOME*)                 dcand="mutter gnome-shell";;
        *KDE*)                   dcand="kwin_wayland kwin_x11";;
        *X-Cinnamon*|*Cinnamon*) dcand="muffin cinnamon";;
        *COSMIC*)                dcand="cosmic-comp";;
        *MATE*)                  dcand="marco";;
        *LXQt*|*LXDE*)           dcand="labwc openbox";;
        *XFCE*)                  dcand="labwc xfwm4";;
        # A desktop that names only its own compositor needs no arm of its
        # own. The last field is the most specific one.
        *) dcand=$(echo "${desktop##*:}" | tr '[:upper:]' '[:lower:]' |
                   tr -cd 'a-z0-9._-');;
    esac
    # Whatever the desktop is, and whatever it failed to mention. The name from
    # the screen goes first: that one is known to be the right answer, and all
    # that is missing is its process.
    cand="$WM_NAME $dcand xfwm4 compiz kwin_x11 kwin_wayland labwc openbox marco
          metacity cosmic-comp mutter muffin gnome-shell cinnamon sway
          hyprland Hyprland niri jay river wayfire dwl weston
          i3 awesome fluxbox icewm"

    if [ -z "$WM_PID" ]; then
        for p in $cand; do
            # A name already settled is not up for changing, only for finding
            [ -z "$WM_NAME" ] || [ "$(wm_canon "$p")" = "$WM_NAME" ] || continue
            pid=$(pgrep -x "$p" | head -1)
            [ -n "$pid" ] || continue
            WM_NAME=$(wm_canon "$p"); WM_PID=$pid; break
        done
    fi

    # Nothing published and no process to be seen, which is the container case:
    # the desktop's own compositor is the most that can be said, and saying it
    # beats refusing to run at all
    [ -z "$WM_NAME" ] && [ -n "$dcand" ] && WM_NAME=$(wm_canon "${dcand%% *}")

    # Compositing can cost more than the window manager's own process: some
    # paint their frames in a separate program, and one that does not composite
    # at all is usually paired with something that does nothing else. Those
    # helpers are what compositing costs, so they count too. The names are what
    # /proc shows, cut to 15 characters.
    WM_PIDS=$WM_PID
    case "$WM_NAME" in
        compiz)   helpers='gtk-window-deco|emerald';;
        openbox|metacity|marco|xfwm4|i3|awesome|fluxbox|icewm)
                  helpers='picom|compton|xcompmgr';;
        *)        helpers="";;
    esac
    if [ -n "$helpers" ] && [ -n "$WM_PID" ]; then
        WM_PIDS="$WM_PID $(pgrep -x "$helpers" | tr '\n' ' ')"
    fi

    # The server counts too: a Wayland compositor is one process putting the
    # screen together, and on X11 that work is split with the server, so leaving
    # it out hands X11 whatever it did - on a session that does not composite,
    # nearly the lot. Both or neither, because a figure missing one of them
    # still reads like the whole desktop's.
    if [ "$(backend_type)" = x11 ]; then
        pid=$(x_server_pid) || pid=""
        if [ -n "$pid" ] && [ -n "$WM_PIDS" ]; then
            WM_PIDS="$WM_PIDS $pid"
        else
            WM_PIDS=""
        fi
    fi

    [ -n "$WM_NAME" ]
}

# Tried for real, not just found: a tool is installed on desktops whose
# compositor refuses to hand the screen out, and then every proof answers
# "could not look" while the report says a tool was found.
pick_capture_cmd () {
    local t cmd tmp helper="$(dirname "${BASH_SOURCE[0]}")/capture_ppm.sh"

    tmp=$(mktemp --suffix=.ppm) || return 1
    # The last two produce PNG; the helper converts it with ffmpeg
    for t in "grim -t ppm" "gnome-screenshot -f" "spectacle -b -n -o"; do
        command -v "${t%% *}" >/dev/null || continue
        case "$t" in
            grim*) cmd=$t;;
            *)     command -v ffmpeg >/dev/null || continue
                   cmd="$helper $t";;
        esac
        : > "$tmp"
        # Bounded: one of these asks a portal, which waits on a dialog nobody
        # is there to click, and the run would hang before its first line
        if timeout 10 $cmd "$tmp" >/dev/null 2>&1 && [ -s "$tmp" ]; then
            rm -f "$tmp"
            echo "$cmd"

            return 0
        fi
    done
    rm -f "$tmp"

    return 1
}

# Every wait for a load is bounded by the clock, not by whether the process
# still exists: a load stuck on a compositor that never answers stays alive
# saying nothing, and holds the whole run with a blank screen.
#
# The gap between the two is deliberate: setting a window up takes seconds, so a
# minute there is already far out, while the work itself legitimately takes as
# long as the machine needs, so only a stall can reach ten minutes.
BENCH_START_TIMEOUT=${BENCH_START_TIMEOUT:-60}
BENCH_RUN_TIMEOUT=${BENCH_RUN_TIMEOUT:-600}

# The mix lives here because both scripts have to mean the same load by it.
#
# Every load is given the same amount of work at its own fixed rate, so on a
# machine that keeps up they all finish together, and on one that does not they
# fall behind together. Each is held at the gate until the whole mix is on
# screen, so no task at all is done before the load is up. See gate.c.
STRESS_FRAMES=${RENDER_FRAMES:-1200}    # 20 s of frames at 60 a second

# Bottom first: the last one opened ends up on top. The order is fixed so that
# the same windows are covered, and so the same amount of work is composited,
# every run.
STRESS_STACK=("fsbench" "popbench background" "transbench background"
              "manywin" "movebench resize" "movebench" "transbench translucent")

# On Wayland nothing can ask about another program's windows, so a window is
# waited for through the load's own log instead: every tool prints a
# "WINDOW-UP <name>" marker after its first commit.
stress_wait_up () {             # $1 marker, $2 log, $3 pid
    local i
    for i in $(seq 300); do     # up to 30 seconds, like restack -wait
        grep -q "$1" "$2" 2>/dev/null && return 0
        # A load that has already gone will never write it. Waiting the full
        # thirty seconds for a marker from a dead process buries the reason it
        # gave - a refusal names the protocol the compositor is missing
        if [ -n "${3:-}" ] && ! kill -0 "$3" 2>/dev/null; then
            grep -q "$1" "$2" 2>/dev/null && return 0
            grep -q REFUSED "$2" 2>/dev/null ||
                echo "stopped before its window was on screen" >> "$2"
            return 1
        fi
        sleep 0.1
    done
    echo "never saw $1" >> "$2"
    return 1
}

# STRESS_NAMES, STRESS_LOGS and STRESS_PIDS are filled in step with each other,
# and STRESS_SCENERY_PID with the filler windows, which count nothing.
#
# One at a time, each window waited for before the next is opened. A window goes
# on top of the ones already there on every window manager, so opening them in
# order is the order, and nothing depends on a raise being granted. Starting all
# six at once left them mapping in a race, and the restack that tried to sort it
# out afterwards was honoured in part and differently each run.
stress_start () {
    STRESS_GATE=$1; STRESS_PRE=$2
    STRESS_NAMES=(); STRESS_LOGS=(); STRESS_PIDS=(); STRESS_LATE=0
    local r=$STRESS_FRAMES
    # On Wayland the translucent window maps itself only when the gate opens,
    # so it lands on top; what says the load is up there is its background
    local tw="transbench translucent"
    [ "$(backend_type)" = wayland ] && tw="transbench background"

    stress_load render "$r"             "fsbench" \
        ./tools/fsbench2 0 windowed 60
    stress_load popups $(((r + 2) / 3)) "popbench background" \
        ./tools/popbench 0 20 4
    # transbench opens a background window and a translucent one over it. One
    # program owns both, so opening cannot separate them: its background lands
    # here, and the translucent one is put back on top below.
    stress_load trans  "$r"             "$tw" \
        ./tools/transbench 0 0.75 60
    ./tools/manywin 12 > "$STRESS_PRE-many.log" 2>&1 &
    STRESS_SCENERY_PID=$!
    if [ "$(backend_type)" = wayland ]; then
        # READY comes after every one of its windows is up
        stress_wait_up "^READY " "$STRESS_PRE-many.log" \
            "$STRESS_SCENERY_PID" || STRESS_LATE=1
    else
        ./tools/restack -wait "manywin" || STRESS_LATE=1
    fi
    stress_load resize $((2 * r))       "movebench resize" \
        ./tools/movebench 0 resize 120
    stress_load move   $((2 * r))       "movebench" \
        ./tools/movebench 0 move 120
}

# A desktop that stacks them some other way composites a different scene, and
# its numbers are not the same measurement as anyone else's. The fallback asks
# for the whole order outright, for one that will not stack them as they open:
# better a scene put right by asking than no test at all.
stress_settle () {
    if [ "$(backend_type)" = wayland ]; then
        # The translucent window maps itself at the gate, so it lands on top;
        # nothing can read the order back to prove the rest
        return 0
    fi
    ./tools/restack -w "transbench translucent" > "$STRESS_PRE-restack.log" 2>&1
    ./tools/restack -c "${STRESS_STACK[@]}" >> "$STRESS_PRE-restack.log" 2>&1 &&
        return 0
    ./tools/restack -w "${STRESS_STACK[@]}" >> "$STRESS_PRE-restack.log" 2>&1
    ./tools/restack -c "${STRESS_STACK[@]}" >> "$STRESS_PRE-restack.log" 2>&1
}

stress_load () {                # $1 name, $2 tasks, $3 window, $4... program
    local name=$1 tasks=$2 win=$3 log="$STRESS_PRE-$1.log"
    shift 3
    : > "$log"
    BENCH_GO="$STRESS_GATE" BENCH_TASKS=$tasks "$@" >> "$log" 2>&1 &
    STRESS_NAMES+=("$name"); STRESS_LOGS+=("$log"); STRESS_PIDS+=("$!")
    # On screen before the next one is opened, or they race for the order. A
    # window that never appears is not a stacking question, it is a broken
    # load, and the caller is told rather than left to guess later.
    if [ "$(backend_type)" = wayland ]; then
        stress_wait_up "^WINDOW-UP $win\$" "$log" "${STRESS_PIDS[-1]}" ||
            STRESS_LATE=1
    else
        ./tools/restack -wait "$win" >> "$log" 2>&1 || STRESS_LATE=1
    fi
}

# A program still drawing its content when the gate opens is not held by it: it
# starts as late as its setup took and staggers the whole mix by that much.
#
# One that never reaches the gate is killed rather than waited on: the loops
# after this one wait on the same programs, and a mix missing a load is not the
# mix anyway, so the test has to fail whatever happens next.
stress_wait_ready () {
    local i left=1 deadline=$((SECONDS + BENCH_START_TIMEOUT))

    while [ "$left" = 1 ]; do
        left=0
        for i in "${!STRESS_LOGS[@]}"; do
            grep -q MEASURE-READY "${STRESS_LOGS[$i]}" 2>/dev/null && continue
            kill -0 "${STRESS_PIDS[$i]}" 2>/dev/null || continue
            if [ "$SECONDS" -ge "$deadline" ]; then
                echo "never reached the gate, killed after ${BENCH_START_TIMEOUT}s" \
                    >> "${STRESS_LOGS[$i]}"
                kill "${STRESS_PIDS[$i]}" 2>/dev/null
                STRESS_LATE=1
                continue
            fi
            left=1
        done
        [ "$left" = 1 ] && sleep 0.1
    done
}
