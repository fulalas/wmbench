#!/bin/bash
# A video player: a window whose pixels arrive as XShmPutImage from the client,
# not from OpenGL or from X primitives. The pixmap the compositor samples has
# been filled by the processor, and if sampling that is slower than sampling a
# GPU-rendered one, this is the workload where the OpenGL renderer would lose.
#
#   ./video_power.sh [rounds]       default 3; env: RUN, FPS, VW, VH
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
RUN=${RUN:-20}
FPS=${FPS:-60}
VW=${VW:-1920}
VH=${VH:-1080}
ROUNDS=${1:-3}
source "$(dirname "$0")/../common.sh"
cd "$(dirname "$0")/.." || exit 1

variant () {                    # $1 label, $2 use_gl
    xfconf-query -c xfwm4 -p /general/use_gl_compositing -s "$2" >/dev/null
    "$WM" --replace > "vb-$1.log" 2>&1 &
    local wm=$!
    sleep 7
    xfconf-query -c xfwm4 -p /general/use_compositing -s true >/dev/null
    sleep 3
    [ -r "/proc/$wm/stat" ] || { echo "  $1: did not start"; return; }
    ./tools/videobench $((RUN + 6)) "$FPS" "$VW" "$VH" > vb-run.log 2>&1 &
    local vb=$!
    sleep 5
    local c0 c1
    pwr_watch $((RUN - 2)) vb-pw
    c0=$(cpu_of $wm)
    wait $PW
    c1=$(cpu_of $wm)
    wait $vb
    awk -v l="$1" -v w="$(cat vb-pw)" -v c0=$c0 -v c1=$c1 -v r="$((RUN-2))" \
        -v f="$(awk '/AVERAGE/{print $2}' vb-run.log)" '
        BEGIN { printf "  %-8s %6s W   wm %5.1f ms/s   %s fps\n", l, w, (c1-c0)/r, f;
                printf "%s %.1f\n", w, (c1-c0)/r >> ("vb-" l) }'
    sleep 3
}

/bin/rm -f vb-xrender vb-opengl
save_xfwm_settings
xfconf-query -c xfwm4 -p /general/suspend_compositing_fullscreen -s false >/dev/null
for i in $(seq 1 "$ROUNDS"); do
    echo "round $i (${VW}x${VH} at ${FPS} fps)"
    if [ $((i % 2)) -eq 1 ]; then
        variant xrender false; variant opengl true
    else
        variant opengl true; variant xrender false
    fi
done
echo
for v in xrender opengl; do
    [ -r "vb-$v" ] && awk -v v="$v" '{w+=$1;c+=$2;n++} END{
        printf "%-8s %6.2f W   wm %5.1f ms/s\n", v, w/n, c/n}' "vb-$v"
done
