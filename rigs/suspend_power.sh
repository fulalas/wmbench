#!/bin/bash
# What the "suspend compositing for fullscreen windows" option is worth now
# that the default presentation is a swap. The old +3.9% was measured with the
# scene buffer as the default, which cost the application far more, so the
# option had more to give back.
#
# The application asks the window manager for fullscreen and never sets
# _NET_WM_BYPASS_COMPOSITOR, which is the category the option exists for.
#
#   ./suspend_power.sh [rounds]     default 2; env: RUN
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
RUN=${RUN:-16}
ROUNDS=${1:-2}
source "$(dirname "$0")/../common.sh"
cd "$(dirname "$0")/.." || exit 1

one () {                        # $1 label, $2 suspend option
    xfconf-query -c xfwm4 -p /general/suspend_compositing_fullscreen -s "$2" >/dev/null
    sleep 3
    pwr_watch $((RUN - 4)) sp-pw
    local fps
    fps=$(./tools/fsbench2 "$RUN" fullscreen 2>&1 | awk '/AVERAGE/{print $2}')
    wait $PW
    printf '  %-12s %8s fps   %6s W\n' "$1" "$fps" "$(cat sp-pw)"
    echo "$fps $(cat sp-pw)" >> "sp-$1"
    sleep 3
}

/bin/rm -f sp-off sp-on
save_xfwm_settings
xfconf-query -c xfwm4 -p /general/use_gl_compositing -s true >/dev/null
"$WM" --replace > sp-wm.log 2>&1 &
WM=$!
sleep 8
xfconf-query -c xfwm4 -p /general/use_compositing -s true >/dev/null
sleep 3
[ -r /proc/$WM/stat ] || { echo "did not start"; exit 1; }

for i in $(seq 1 "$ROUNDS"); do
    echo "round $i"
    if [ $((i % 2)) -eq 1 ]; then one off false; one on true
    else one on true; one off false; fi
done
echo
for v in off on; do
    [ -r "sp-$v" ] && awk -v v="$v" '{f+=$1;w+=$2;n++} END{
        printf "suspend %-4s %8.2f fps   %6.2f W\n", v, f/n, w/n}' "sp-$v"
done
