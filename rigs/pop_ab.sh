#!/bin/bash
# Menus, tooltips and notifications: windows appearing and disappearing at a
# fixed rate. The per-window setup cost dominates here, which is where the GL
# renderer is structurally worst off: a GLX pixmap, a texture, a binding and a
# blocking round trip for every window that appears, against one picture for
# XRender.
#
#   ./pop_ab.sh [rounds]            default 3; env: RUN, RATE, NPOP, PW_W, PW_H
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
RUN=${RUN:-20}
RATE=${RATE:-20}
NPOP=${NPOP:-6}
ROUNDS=${1:-3}
source "$(dirname "$0")/../common.sh"
cd "$(dirname "$0")/.." || exit 1

cpu_of () { awk '{print ($14 + $15) * 1000 / 100}' /proc/$1/stat; }

variant () {                    # $1 label, $2 use_gl
    xfconf-query -c xfwm4 -p /general/use_gl_compositing -s "$2" >/dev/null
    "$WM" --replace > "po-$1.log" 2>&1 &
    local wm=$!
    sleep 7
    xfconf-query -c xfwm4 -p /general/use_compositing -s true >/dev/null
    sleep 3
    [ -r /proc/$wm/stat ] || { echo "  $1: did not start"; return; }

    # popbench spends its first four seconds settling and drawing the
    # background; the power window has to start after that, not during it.
    ./tools/popbench $((RUN + 6)) "$RATE" "$NPOP" ${PW_W:-340} ${PW_H:-440} > po-run.log 2>&1 &
    local pb=$!
    sleep 6
    local c0 c1
    pwr_watch $((RUN - 2)) po-pw
    c0=$(cpu_of $wm)
    wait $PW
    c1=$(cpu_of $wm)
    wait $pb

    awk -v l="$1" -v w="$(cat po-pw)" -v c0=$c0 -v c1=$c1 -v r="$((RUN - 2))" \
        -v cy="$(awk '/AVERAGE/{print $2}' po-run.log)" '
        BEGIN { printf "  %-8s %6s W   wm %5.1f ms/s   %s cycles/s\n",
                       l, w, (c1 - c0) / r, cy;
                printf "%s %.1f\n", w, (c1 - c0) / r >> ("po-" l) }'
    sleep 3
}

/bin/rm -f po-xrender po-opengl
save_xfwm_settings
xfconf-query -c xfwm4 -p /general/suspend_compositing_fullscreen -s false >/dev/null
for i in $(seq 1 "$ROUNDS"); do
    echo "round $i ($RATE popups/s)"
    if [ $((i % 2)) -eq 1 ]; then
        variant xrender false; variant opengl true
    else
        variant opengl true; variant xrender false
    fi
done
echo
for v in xrender opengl; do
    [ -r "po-$v" ] && awk -v v="$v" '{w+=$1;c+=$2;n++} END{
        printf "%-8s %6.2f W   wm %5.1f ms/s\n", v, w/n, c/n}' "po-$v"
done
