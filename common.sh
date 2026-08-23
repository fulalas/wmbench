# Shared by every rig in this directory. Source it, do not run it.
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

REPO=${REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
WM=${WM:-$REPO/src/xfwm4}

# The whole-package power sensor, which is the metric this project uses.
#
# Never hardcode an hwmon index: the numbering is not stable across boots, and
# reading the wrong one gives a plausible number for the wrong thing, or a
# silent zero. Find it by name.
#
# And never fall back to "whatever exposes power1_average". On this machine only
# the amdgpu hwmon does, but nvme and wifi devices expose that file on other
# machines, and picking one would report a disk's power draw as the package's:
# a confident number for entirely the wrong thing, with nothing in the output to
# say so. Only sensors known to cover the package are accepted.
#
# On an AMD APU the amdgpu hwmon's power1_average is labelled PPT and covers the
# processor cores as well as the graphics, which is what makes it usable here.
# On a discrete card the same file is board power and excludes the processor
# entirely, so it cannot answer the question this project asks of it.
find_power_sensor () {
    local h name label

    for h in /sys/class/hwmon/hwmon*; do
        [ -r "$h/power1_average" ] || continue
        [ -r "$h/name" ] || continue
        read -r name < "$h/name"
        case "$name" in
            amdgpu|k10temp)
                label=""
                [ -r "$h/power1_label" ] && read -r label < "$h/power1_label"
                # PPT is the package figure. Anything else from these drivers is
                # not necessarily, so say what was taken.
                if [ "$label" != "PPT" ]; then
                    echo "Using $h ($name, power1_label=${label:-none})." >&2
                    echo "That is only the package figure if this is an APU." >&2
                fi
                echo "$h/power1_average"

                return 0
                ;;
        esac
    done

    return 1
}

# No sensor is not fatal: everything else still runs, the power figures are
# simply not reported. Nothing is ever guessed at.
PWR=$(find_power_sensor) || {
    echo "No package power sensor found, and none will be guessed at;" >&2
    echo "power will not be reported. On an AMD APU the sensor is the" >&2
    echo "amdgpu hwmon's power1_average, labelled PPT." >&2
    PWR=""
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
      n=0; w=0; end=$((SECONDS + $1))
      while [ $SECONDS -lt $end ]; do
          w=$((w + $(cat "$PWR" 2>/dev/null || echo 0))); n=$((n + 1)); sleep 0.1
      done
      awk -v w=$w -v n=$n 'BEGIN{printf "%.2f", (n ? w/n/1e6 : 0)}' > "$2" ) &
    PW=$!
}

# Save the settings every rig touches and put them back when the script exits,
# however it exits - Ctrl-C included. The rigs replace the session's window
# manager per arm, and an interrupt kills the replacement too, so the restore
# also starts a clean WM, detached so nothing takes it down with the script.
# Normal completion goes through the same path: the last arm's WM was running
# with test environment variables, which the clean start clears.
save_xfwm_settings () {
    XFWM_SAVED_GL=$(xfconf-query -c xfwm4 -p /general/use_gl_compositing 2>/dev/null || echo true)
    XFWM_SAVED_COMP=$(xfconf-query -c xfwm4 -p /general/use_compositing 2>/dev/null || echo true)
    XFWM_SAVED_SUSP=$(xfconf-query -c xfwm4 -p /general/suspend_compositing_fullscreen 2>/dev/null || echo true)
    trap restore_xfwm_settings EXIT
    trap 'exit 130' INT TERM
}
restore_xfwm_settings () {
    xfconf-query -c xfwm4 -p /general/use_gl_compositing -s "$XFWM_SAVED_GL" 2>/dev/null
    xfconf-query -c xfwm4 -p /general/use_compositing -s "$XFWM_SAVED_COMP" 2>/dev/null
    xfconf-query -c xfwm4 -p /general/suspend_compositing_fullscreen -s "$XFWM_SAVED_SUSP" 2>/dev/null
    ( setsid "$WM" --replace >/dev/null 2>&1 & ) 2>/dev/null
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

# Which renderer actually took the screen, rather than which was asked for
backend () {
    xprop -root _XFWM4_RENDER_BACKEND 2>/dev/null |
        sed 's/.*= "//; s/ .*//; s/"//'
}

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
        [ -n "$dpi" ] || dpi=$(xfconf-query -c xsettings -p /Xft/DPI 2>/dev/null)
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
