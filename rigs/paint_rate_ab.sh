#!/bin/bash
# How many screens each renderer actually delivers when the input is faster than
# either can composite. Unlike every watt figure here this needs no power
# sensor, so it reproduces anywhere; and it is what a person feels as smoothness.
#
# movebench left unpaced spams XMoveWindow far faster than anything can
# composite, so the paint rate is the compositor's capacity rather than the
# client's.
#
#   ./paint_rate_ab.sh [rounds]     default 3; env: RUN, WORK, RATE
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
RUN=${RUN:-14}
WORK=${WORK:-move}
RATE=${RATE:-200}
ROUNDS=${1:-3}
source "$(dirname "$0")/../common.sh"
cd "$(dirname "$0")/.." || exit 1

variant () {                    # $1 label, $2 use_gl
    xfconf-query -c xfwm4 -p /general/use_gl_compositing -s "$2" >/dev/null
    XFWM4_PAINT_STATS=1 "$WM" --replace > "pr-$1.log" 2>&1 &
    local wm=$!
    sleep 7
    xfconf-query -c xfwm4 -p /general/use_compositing -s true >/dev/null
    sleep 3
    [ -r "/proc/$wm/stat" ] || { echo "  $1: did not start"; return; }

    : > "pr-$1.mark"
    ./tools/movebench "$RUN" "$WORK" "$RATE" > pr-run.log 2>&1
    # the rates reported while the benchmark was running
    local r
    # Only the last reported interval, and only the counter that names the
    # renderer. Averaging in an earlier interval mixes in the ramp-up before the
    # benchmark got going, which is what produced a bogus 103/s once.
    r=$(grep -E '(opengl|xrender) paints' "pr-$1.log" | tail -1 |
        awk '{gsub("/s","",$NF); printf "%.1f", $NF}')
    printf '  %-8s %8s screens/s   (%s steps/s asked)\n' "$1" "$r" \
        "$(awk '/AVERAGE/{printf "%.0f", $2}' pr-run.log)"
    echo "$r" >> "pr-$1"
    sleep 3
}

/bin/rm -f pr-xrender pr-opengl
save_xfwm_settings
xfconf-query -c xfwm4 -p /general/suspend_compositing_fullscreen -s false >/dev/null
for i in $(seq 1 "$ROUNDS"); do
    echo "round $i ($WORK at $RATE steps/s)"
    if [ $((i % 2)) -eq 1 ]; then
        variant xrender false; variant opengl true
    else
        variant opengl true; variant xrender false
    fi
done
echo
for v in xrender opengl; do
    [ -r "pr-$v" ] && awk -v v="$v" '{s+=$1;n++} END{
        printf "%-8s %8.1f screens/s\n", v, s/n}' "pr-$v"
done
