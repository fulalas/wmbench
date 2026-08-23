#!/bin/bash
# A translucent window redrawn over a busy one: steps a second and package
# power, XRender against OpenGL.
#
#   ./trans_power.sh [rounds]       default 3; env: RUN
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
RUN=${RUN:-14}
ROUNDS=${1:-3}
MODE=trans
source "$(dirname "$0")/../common.sh"
cd "$(dirname "$0")/.." || exit 1

variant () {                    # $1 label, $2 use_gl, $3 compositing
    xfconf-query -c xfwm4 -p /general/use_gl_compositing -s "$2" >/dev/null
    "$WM" --replace > "tb-$1.log" 2>&1 &
    local wm=$!
    sleep 7
    xfconf-query -c xfwm4 -p /general/use_compositing -s "$3" >/dev/null
    sleep 3
    [ -r /proc/$wm/stat ] || { echo "  $1: did not start"; return; }

    local c0 c1 steps
    pwr_watch $((RUN - 7)) tb-pw 4
    c0=$(cpu_of $wm)
    steps=$(./tools/transbench "$RUN" 0.75 120 2>&1 | awk '/AVERAGE/{print $2}')
    c1=$(cpu_of $wm)
    wait $PW

    awk -v l="$1" -v s="$steps" -v w="$(cat tb-pw)" -v c0=$c0 -v c1=$c1 -v r="$RUN" '
        BEGIN {
            printf "  %-9s %8s steps/s   %6s W   wm %5.1f ms/s\n",
                   l, s, w, (c1 - c0) / r;
            printf "%s %s %.1f\n", s, w, (c1 - c0) / r >> ("tb-" l);
        }'
    sleep 3
}

for v in none xrender opengl; do /bin/rm -f "tb-$v"; done
save_xfwm_settings
xfconf-query -c xfwm4 -p /general/suspend_compositing_fullscreen -s false >/dev/null
for i in $(seq 1 "$ROUNDS"); do
    echo "round $i ($MODE)"
    if [ $((i % 2)) -eq 1 ]; then
        variant none    false false
        variant xrender false true
        variant opengl  true  true
    else
        variant opengl  true  true
        variant xrender false true
        variant none    false false
    fi
done
xfconf-query -c xfwm4 -p /general/use_compositing -s true >/dev/null
echo
for v in none xrender opengl; do
    [ -r "tb-$v" ] && awk -v v="$v" '{s+=$1;w+=$2;c+=$3;n++} END{
        printf "%-9s %8.1f steps/s   %6.2f W   wm %5.1f ms/s\n", v, s/n, w/n, c/n}' "tb-$v"
done
