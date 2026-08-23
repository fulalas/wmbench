#!/bin/bash
# Regression check against the committed code. Both arms are forced to the
# swap presentation, so the default change is held constant and what is left
# is everything else in the working tree: the profiler instrumentation in the
# paint path, the pass-2 gating, the fence option, and the skipped XRender
# picture.
#
# WORK=move | resize | render | pop
#
#   ./bin_ab.sh [rounds]            default 3; env: WORK, RUN, NEW, OLD
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
source "$(dirname "$0")/../common.sh"
RUN=${RUN:-16}
WORK=${WORK:-move}
ROUNDS=${1:-3}
NEW=${NEW:-$WM}
OLD=${OLD:-./xfwm4-committed}
cd "$(dirname "$0")/.." || exit 1

cpu_of () { awk '{print ($14 + $15) * 1000 / 100}' /proc/$1/stat; }

variant () {                    # $1 label, $2 binary
    xfconf-query -c xfwm4 -p /general/use_gl_compositing -s true >/dev/null
    XFWM4_GL_PRESENT=swap "$2" --replace > "ba-$1.log" 2>&1 &
    local wm=$!
    sleep 7
    xfconf-query -c xfwm4 -p /general/use_compositing -s true >/dev/null
    sleep 3
    [ -r /proc/$wm/stat ] || { echo "  $1: did not start"; return; }

    local c0 c1 l="$1"
    case "$WORK" in
        move)   set -- ./tools/movebench "$RUN" move 120;;
        resize) set -- ./tools/movebench "$RUN" resize 120;;
        render) set -- ./tools/fsbench2 "$RUN" windowed 60;;
        pop)    set -- ./tools/popbench "$RUN" 20 6;;
        *)      echo "unknown WORK=$WORK"; exit 1;;
    esac
    pwr_watch $((RUN - 3)) ba-pw
    c0=$(cpu_of $wm)
    "$@" > ba-run.log 2>&1
    c1=$(cpu_of $wm)
    wait $PW

    awk -v l="$l" -v w="$(cat ba-pw)" -v c0=$c0 -v c1=$c1 -v r="$RUN" '
        BEGIN { printf "  %-9s %6s W   wm %5.1f ms/s\n", l, w, (c1 - c0) / r;
                printf "%s %.1f\n", w, (c1 - c0) / r >> ("ba-" l) }'
    sleep 3
}

/bin/rm -f ba-committed ba-current
save_xfwm_settings
xfconf-query -c xfwm4 -p /general/suspend_compositing_fullscreen -s false >/dev/null
for i in $(seq 1 "$ROUNDS"); do
    echo "round $i ($WORK)"
    if [ $((i % 2)) -eq 1 ]; then
        variant committed "$OLD"; variant current "$NEW"
    else
        variant current "$NEW"; variant committed "$OLD"
    fi
done
echo
for v in committed current; do
    [ -r "ba-$v" ] && awk -v v="$v" '{w+=$1;c+=$2;n++} END{
        printf "%-9s %6.2f W   wm %5.1f ms/s\n", v, w/n, c/n}' "ba-$v"
done
