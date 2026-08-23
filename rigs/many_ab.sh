#!/bin/bash
# A desktop with many shadowed windows sitting idle while one small window
# animates. Two questions at once: what a many-window desktop costs each
# renderer, and whether skipping the blended pass for windows whose shadow is
# nowhere near the damage pays for its four rectangle tests.
#
# XRender against OpenGL on a many-window desktop. (A MODE=ab arm existed
# for a shadow experiment whose switch is gone from the compositor; removed,
# it compared two identical arms.)
#
#   ./many_ab.sh [rounds]           default 3; env: RUN, NWIN
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
RUN=${RUN:-18}
NWIN=${NWIN:-20}
ROUNDS=${1:-3}
source "$(dirname "$0")/../common.sh"
cd "$(dirname "$0")/.." || exit 1

cpu_of () { awk '{print ($14 + $15) * 1000 / 100}' /proc/$1/stat; }

variant () {                    # $1 label, $2 gl, $3 env
    xfconf-query -c xfwm4 -p /general/use_gl_compositing -s "$2" >/dev/null
    env $3 "$WM" --replace > "ma-$1.log" 2>&1 &
    local wm=$!
    sleep 7
    xfconf-query -c xfwm4 -p /general/use_compositing -s true >/dev/null
    sleep 3
    [ -r /proc/$wm/stat ] || { echo "  $1: did not start"; return; }

    ./tools/manywin "$NWIN" $((RUN + 12)) > ma-win.log 2>&1 &
    local mw=$!
    sleep 7

    local c0 c1
    pwr_watch $((RUN - 3)) ma-pw
    c0=$(cpu_of $wm)
    ./tools/transbench "$RUN" 1.0 60 > /dev/null 2>&1
    c1=$(cpu_of $wm)
    wait $PW
    kill $mw 2>/dev/null; wait $mw 2>/dev/null

    awk -v l="$1" -v w="$(cat ma-pw)" -v c0=$c0 -v c1=$c1 -v r="$RUN" '
        BEGIN { printf "  %-8s %6s W   wm %5.1f ms/s\n", l, w, (c1-c0)/r;
                printf "%s %.1f\n", w, (c1-c0)/r >> ("ma-" l) }'
    sleep 3
}

/bin/rm -f ma-xrender ma-opengl
save_xfwm_settings
xfconf-query -c xfwm4 -p /general/suspend_compositing_fullscreen -s false >/dev/null
for i in $(seq 1 "$ROUNDS"); do
    echo "round $i ($NWIN windows)"
    if [ $((i % 2)) -eq 1 ]; then
        variant xrender false ""
        variant opengl  true  ""
    else
        variant opengl  true  ""
        variant xrender false ""
    fi
done
echo
for v in xrender opengl; do
    [ -r "ma-$v" ] && awk -v v="$v" '{w+=$1;c+=$2;n++} END{
        printf "%-8s %6.2f W   wm %5.1f ms/s\n", v, w/n, c/n}' "ma-$v"
done
