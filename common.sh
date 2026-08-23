# Shared by the scripts in this directory. Source it, do not run it.
#
# Two rules are baked in here because getting either wrong has produced a
# confident wrong answer in this project before.
#
#   Alternate the arms. Whichever variant runs first in a round comes out
#   cooler and wins, whatever it is. Every rig here alternates, and a
#   comparison that does not is worthless however tight its samples look.
#
#   Compare within one condition. A per-paint processor or GPU figure is not
#   comparable between an idle desktop and a loaded one, or between a capped
#   and an uncapped run: the clocks differ. Only put numbers side by side when
#   the state they were taken in is the same.

# The power sensors, and what they can honestly answer.
#
# Two kinds of source, read differently:
#   a hwmon power1_average   microwatts, taken as it reads
#   a RAPL energy_uj         a rising counter, so power is what it gained
#                            divided by how long that took
#
# Never hardcode an hwmon index: the numbering is not stable across boots. And
# never fall back to "whatever exposes power1_average", because nvme drives and
# wireless cards expose that file too, and a disk's power draw looks just as
# plausible in the output.
#
# What a machine can give:
#   AMD integrated    the amdgpu hwmon labelled PPT already covers the
#                     processor cores and the graphics together
#   AMD discrete      that same file is the board alone, so the processor is
#                     added from RAPL
#   Intel integrated  no GPU hwmon, and the RAPL package covers the processor
#                     and the graphics together
#
# Anything else is refused rather than reported. A board figure with no
# processor figure would read like a whole-machine one and be wrong by the
# entire processor, and there is nothing in a bare number to say so.

# The graphics sensor, if a driver we trust exposes one. Echoes "path label".
find_gpu_power () {
    local h name label

    for h in /sys/class/hwmon/hwmon*; do
        [ -r "$h/power1_average" ] || continue
        [ -r "$h/name" ] || continue
        read -r name < "$h/name"
        case "$name" in
            amdgpu|k10temp)
                label=none
                [ -r "$h/power1_label" ] && read -r label < "$h/power1_label"
                echo "$h/power1_average $label"

                return 0
                ;;
        esac
    done

    return 1
}

# The processor's energy counter. Most kernels keep it readable by root only,
# so being unable to read it is the common case and worth saying out loud.
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

# Intel's discrete cards report energy rather than power, the same way the CPU
# does, so watts come from how fast the counter rises.
find_gpu_energy () {
    local h name

    for h in /sys/class/hwmon/hwmon*; do
        [ -r "$h/energy1_input" ] || continue
        [ -r "$h/name" ] || continue
        read -r name < "$h/name"
        case "$name" in
            i915|xe)
                echo "$h/energy1_input"

                return 0
                ;;
        esac
    done

    return 1
}

# NVIDIA boards keep their power behind the proprietary driver's NVML, so the
# only way to it is nvidia-smi. Streamed for a whole window rather than forked
# per sample: a fork every tenth of a second would show up in the very
# processor figures being measured.
find_nvidia_power () {
    command -v nvidia-smi >/dev/null 2>&1 || return 1
    nvidia-smi --query-gpu=power.draw --format=csv,noheader,nounits \
        >/dev/null 2>&1 || return 1
    echo nvidia-smi
}

# Decide what the sensors found can answer. A function so that it can be
# re-run, which is how the machines this one is not get tested.
power_choose () {
    POWER_HWMON=""; POWER_LABEL=""; POWER_ENERGY=""; POWER_GPU_ENERGY=""
    POWER_NVIDIA=""; POWER_DESC=""; POWER_OK=0; POWER_CPU_WANTED=1
    local gpu="" cpu=""

    read -r POWER_HWMON POWER_LABEL <<< "$(find_gpu_power)"
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

    [ -n "$POWER_HWMON" ] && gpu="$POWER_HWMON (GPU)"
    if [ -z "$gpu" ]; then
        POWER_GPU_ENERGY=$(find_gpu_energy) && gpu="$POWER_GPU_ENERGY (GPU)"
    fi
    if [ -z "$gpu" ]; then
        POWER_NVIDIA=$(find_nvidia_power) && gpu="nvidia-smi (GPU)"
    fi
    [ -n "$POWER_ENERGY" ] && cpu="$POWER_ENERGY (CPU)"

    # Report whatever can be read and say exactly what that covers. A figure
    # missing a part is still worth having between two runs on this machine,
    # which is what the rows are for; what it must never do is read like a
    # whole-machine figure when it is not one.
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
        echo "  No graphics sensor: amdgpu and k10temp expose one in sysfs," >&2
        echo "  an NVIDIA board needs nvidia-smi, nouveau has none at all." >&2
        if [ -n "$POWER_ENERGY_LOCKED" ]; then
            echo "  $POWER_ENERGY_LOCKED exists but is root-only. Unlock it with" >&2
            echo "  sudo chmod a+r $POWER_ENERGY_LOCKED" >&2
        else
            echo "  No RAPL package counter for the processor either." >&2
        fi
    fi
}

power_choose

# The CPU counter is root-only on most kernels, which is what stops a machine
# with a separate card from being measured at all. Offer to open it rather than
# leaving the run half measured, and only when it would add something: on a
# chip that reports CPU and GPU as one figure there is nothing to gain.
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

    # Ask before root is involved at all, so nobody meets a password prompt
    # they did not expect. Saying no is a complete answer: the run goes ahead
    # without the CPU in its power figures.
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

# One measurement window: power_begin, power_sample as often as you like, then
# power_end, which echoes the average watts over the window, or "-".
#
# The hwmon side is averaged over the samples. The counter side needs no
# sampling at all: its rise across the whole window, over the window's length,
# is the average by definition - and it costs two reads instead of hundreds.
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

power_sample () {
    [ -n "$POWER_HWMON" ] || return 0
    POWER_SUM=$((POWER_SUM + $(cat "$POWER_HWMON" 2>/dev/null || echo 0)))
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
                  END { printf "%.3f", (n ? s / n : 0) }' "$POWER_NV_FILE")
        rm -f "$POWER_NV_FILE"
    fi
    awk -v s="$POWER_SUM" -v n="$POWER_N" -v nv="$nv" \
        -v c0="$POWER_CPU_E0" -v c1="$cpu_e1" \
        -v g0="$POWER_GPU_E0" -v g1="$gpu_e1" \
        -v t0="$POWER_T0" -v t1="$t1" 'BEGIN{
            w = (n ? s / n / 1e6 : 0) + nv;
            if (t1 > t0) {
                # A counter that wrapped, which these do, is not a rise
                if (c1 != "" && c1 >= c0) w += (c1 - c0) / (t1 - t0) / 1e6;
                if (g1 != "" && g1 >= g0) w += (g1 - g0) / (t1 - t0) / 1e6;
            }
            printf "%.2f", w }'
}

# Average package power over $1 seconds into file $2, in the background, after
# skipping $3 seconds (default none).
#
# Skip whenever the watcher is started alongside the benchmark rather than after
# it. A benchmark's power ramps over about four seconds while it creates its
# window, builds a GL context and compiles shaders, measured here as 9.1 W in the
# first second, 13.1 in the second, 18.5 over the next two and 20.2 settled.
# Averaging that in drags a 14 s window from 20.2 W down to 18.6. Both arms of a
# comparison ramp the same way, so it does not reverse a result, but it dilutes
# the difference between them by roughly the fraction of the window the ramp
# occupies, which makes every margin measured that way an understatement.
#
# Wait for $PW afterwards, never a bare wait: that would also wait for the
# window manager these scripts start, which never exits.
pwr_watch () {
    ( if [ "${3:-0}" -gt 0 ]; then sleep "$3"; fi
      power_begin
      end=$((SECONDS + $1))
      while [ $SECONDS -lt $end ]; do
          power_sample; sleep 0.1
      done
      power_end > "$2" ) &
    PW=$!
}

# The AMD card's sysfs device directory, found by driver name, never by index:
# card numbering is not stable across boots or machines.
amdgpu_device () {
    local c
    for c in /sys/class/drm/card*/device; do
        [ "$(basename "$(readlink -f "$c/driver" 2>/dev/null)" 2>/dev/null)" = amdgpu ] &&
            { echo "$c"; return 0; }
    done
    return 1
}

# Processor time a process has used, in milliseconds. Quantised to one clock
# tick over the run, so at 16 seconds it cannot resolve better than 0.6 ms/s.
cpu_of () { awk '{print ($14 + $15) * 1000 / 100}' "/proc/$1/stat"; }

all_cpu () { awk '/^cpu /{print ($2+$3+$4+$6+$7+$8) * 1000 / 100}' /proc/stat; }

# The version of the running window manager, asked of the running binary
# itself (/proc/pid/exe, so a locally built one answers for itself), or of
# the binary by name when the process is not visible. Empty when nothing says.
wm_version () {
    local exe
    exe=$(readlink -f "/proc/$WM_PID/exe" 2>/dev/null)
    [ -x "$exe" ] || exe=$(command -v "$WM_NAME") || return 0
    { "$exe" --version 2>/dev/null || "$exe" -v 2>/dev/null; } | head -2 |
        grep -oE '[0-9]+\.[0-9]+[0-9A-Za-z.+~-]*' | head -1
}

# Resolution, refresh and scale, e.g. "3840x2160 @ 120 Hz, scale 1.33".
# On Wayland the compositor is asked (wlr-randr, cosmic-randr on COSMIC,
# kscreen-doctor on KDE), trying each until one answers: XWayland's xrandr
# only shows the scaled-down logical view, not the panel. On X11 the mode
# comes from xrandr and the scale from the font DPI (96 = 1.0).
display_info () {
    local res="" scale="" dpi out tool

    if [ "$(session_type)" = wayland ]; then
        for tool in "wlr-randr" "cosmic-randr list" "kscreen-doctor -o"; do
            command -v "${tool%% *}" >/dev/null || continue
            # kscreen-doctor colours its output; strip the escapes first
            out=$($tool 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g') || continue
            # KDE marks the current mode with a star instead of a word
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
    # GNOME: mutter answers over D-Bus; the current mode is the one marked
    # is-current, the scale sits on the logical monitor
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

# Record the screen into $1.<ext> until stop_recorder. Sets RECFILE, or
# fails when this session has no way to record. X11: ffmpeg. Wayland:
# wf-recorder where the compositor offers screencopy (labwc, COSMIC, sway),
# GNOME through its shell's own screencast service. No root anywhere.
start_recorder () {
    local res
    RECPID=""; RECMODE=""; RECFILE=""

    if [ "$(session_type)" = x11 ]; then
        res=$(xrandr --current 2>/dev/null |
              awk '/ current /{gsub(",","",$10); print $8 "x" $10; exit}')
        RECFILE="$1.mkv"
        ffmpeg -loglevel quiet -f x11grab -framerate 30                -video_size "${res:-1920x1080}" -i "$DISPLAY"                -c:v libx264 -preset ultrafast -crf 30 -y "$RECFILE" &
        RECPID=$!; RECMODE=ffmpeg
    elif command -v wf-recorder >/dev/null; then
        RECFILE="$1.mkv"
        wf-recorder -f "$RECFILE" >/dev/null 2>&1 &
        RECPID=$!; RECMODE=wf
    elif gdbus introspect --session --dest org.gnome.Shell.Screencast                --object-path /org/gnome/Shell/Screencast >/dev/null 2>&1; then
        # The shell answers with the file it actually writes (it picks the
        # container), so the real name comes from the reply. max-length 0
        # lifts the 30 s default cap.
        case "$1" in /*) RECFILE="$1.webm";; *) RECFILE="$(pwd)/$1.webm";; esac
        RECFILE=$(gdbus call --session --dest org.gnome.Shell.Screencast --object-path /org/gnome/Shell/Screencast --method org.gnome.Shell.Screencast.Screencast "$RECFILE" "{'max-length': <uint32 0>}" 2>/dev/null | sed -n "s/^(true, '\(.*\)')$/\1/p")
        [ -n "$RECFILE" ] || return 1
        RECMODE=gnome
    else
        return 1
    fi
    sleep 1
}

stop_recorder () {
    case "${RECMODE:-}" in
        ffmpeg|wf) kill -TERM "$RECPID" 2>/dev/null; wait "$RECPID" 2>/dev/null;;
        gnome) gdbus call --session --dest org.gnome.Shell.Screencast                      --object-path /org/gnome/Shell/Screencast                      --method org.gnome.Shell.Screencast.StopScreencast                      >/dev/null 2>&1;;
    esac
    RECMODE=""
}

# x11 or wayland. XDG_SESSION_TYPE lies less than it used to, but a live
# Wayland socket is the fact of the matter.
session_type () {
    if [ -n "${WAYLAND_DISPLAY:-}" ]; then echo wayland; else echo x11; fi
}

# The window manager of this session. Sets WM_NAME (mutter, kwin, xfwm4, ...)
# and WM_PID, the process whose CPU is the compositor's. The desktop decides
# first; a process scan and the X11 WM name are fallbacks.
detect_wm () {
    local desktop=${XDG_CURRENT_DESKTOP:-} st p
    st=$(session_type)
    WM_NAME=""; WM_PID=""

    case "$desktop" in
        *GNOME*)     WM_NAME=mutter
                     WM_PID=$(pgrep -x mutter | head -1)
                     [ -n "$WM_PID" ] || WM_PID=$(pgrep -x gnome-shell | head -1);;
        *KDE*)       WM_NAME=kwin
                     [ "$st" = wayland ] && WM_PID=$(pgrep -x kwin_wayland | head -1) \
                                         || WM_PID=$(pgrep -x kwin_x11 | head -1);;
        *X-Cinnamon*|*Cinnamon*)
                     WM_NAME=muffin
                     WM_PID=$(pgrep -x muffin | head -1)
                     [ -n "$WM_PID" ] || WM_PID=$(pgrep -x cinnamon | head -1);;
        *COSMIC*)    WM_NAME=cosmic-comp; WM_PID=$(pgrep -x cosmic-comp | head -1);;
        *MATE*)      WM_NAME=marco;   WM_PID=$(pgrep -x marco | head -1);;
        *LXQt*|*LXDE*)
                     [ "$st" = wayland ] && { WM_NAME=labwc; WM_PID=$(pgrep -x labwc | head -1); } \
                                         || { WM_NAME=openbox; WM_PID=$(pgrep -x openbox | head -1); };;
        *XFCE*)      [ "$st" = wayland ] && { WM_NAME=labwc; WM_PID=$(pgrep -x labwc | head -1); } \
                                         || { WM_NAME=xfwm4; WM_PID=$(pgrep -x xfwm4 | head -1); };;
    esac

    # No desktop said so: ask around
    if [ -z "$WM_PID" ]; then
        for p in xfwm4 kwin_x11 kwin_wayland labwc openbox marco cosmic-comp \
                 mutter muffin gnome-shell cinnamon sway; do
            WM_PID=$(pgrep -x "$p" | head -1)
            [ -n "$WM_PID" ] && { WM_NAME=$p; break; }
        done
        case "$WM_NAME" in gnome-shell) WM_NAME=mutter;;
                           cinnamon) WM_NAME=muffin;; esac
    fi
    if [ -z "$WM_NAME" ] && [ "$st" = x11 ]; then
        WM_NAME=$(xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null |
                  awk '{print $NF}' | xargs -r -I{} xprop -id {} _NET_WM_NAME 2>/dev/null |
                  sed 's/.*= "//; s/".*//')
    fi
    [ -n "$WM_NAME" ]
}

# A command that writes a full-screen PPM to the path it is given, for the
# checks to photograph a Wayland screen with. Prints it, or fails.
pick_capture_cmd () {
    local helper="$(dirname "${BASH_SOURCE[0]}")/capture_ppm.sh"

    if command -v grim >/dev/null; then
        echo "grim -t ppm"
        return 0
    fi
    # The others produce PNG; the helper converts it with ffmpeg
    if command -v ffmpeg >/dev/null; then
        for t in "gnome-screenshot -f" "spectacle -b -n -o"; do
            command -v "${t%% *}" >/dev/null && { echo "$helper $t"; return 0; }
        done
    fi

    return 1
}
