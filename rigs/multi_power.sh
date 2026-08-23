#!/bin/bash
# The eight-window stress, measured by package power instead of the retired
# whole-machine CPU figure. Eight independent OpenGL windows, each drawing as
# fast as it can, so the compositor has eight textures to sample every frame.
#
# Reports the aggregate frames the eight windows manage, and what compositing
# costs in watts, which is the distance from the same run with no compositor.
#
#   ./multi_power.sh [rounds]       default 2; env: RUN, VARIANTS
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
RUN=${RUN:-20}
ROUNDS=${1:-2}
VARIANTS=${VARIANTS:-"none xrender gl-swap"}
source "$(dirname "$0")/../common.sh"
cd "$(dirname "$0")/.." || exit 1

env_for () {
    case "$1" in
        none)    echo "GL=false COMP=false ENV=";;
        xrender) echo "GL=false COMP=true  ENV=";;
        gl-swap) echo "GL=true  COMP=true  ENV=XFWM4_GL_PRESENT=swap";;
        gl-fbo)  echo "GL=true  COMP=true  ENV=XFWM4_GL_PRESENT=fbo";;
        gl-egl)  echo "GL=true  COMP=true  ENV=XFWM4_GL_BACKEND=egl";;
    esac
}

variant () {
    local v=$1 i
    eval "$(env_for $v)"
    xfconf-query -c xfwm4 -p /general/use_gl_compositing -s "$GL" >/dev/null
    env $ENV "$WM" --replace > "mp-$v.log" 2>&1 &
    local wm=$!
    sleep 7
    xfconf-query -c xfwm4 -p /general/use_compositing -s "$COMP" >/dev/null
    sleep 3
    [ -r /proc/$wm/stat ] || { echo "  $v: did not start"; return; }

    /bin/rm -f mp-w-*
    local pids=""
    for i in 0 1 2 3 4 5 6 7; do
        ./tools/multiscene $i "$RUN" > "mp-w-$i" 2>&1 &
        pids="$pids $!"
    done
    sleep 2
    pwr_watch $((RUN - 4)) mp-pw
    wait $PW
    wait $pids

    local total=0 f
    for i in 0 1 2 3 4 5 6 7; do
        f=$(awk '/AVERAGE/{print $2}' "mp-w-$i" 2>/dev/null)
        total=$((total + ${f:-0}))
    done
    awk -v v="$v" -v t="$total" -v r="$RUN" -v w="$(cat mp-pw)" 'BEGIN{
        printf "  %-8s %8.1f frames/s aggregate   %6.2f W\n", v, t/r, w;
        printf "%.2f %.2f\n", t/r, w >> ("mp-" v)}'
    sleep 3
}

for v in $VARIANTS; do /bin/rm -f "mp-$v"; done
save_xfwm_settings
xfconf-query -c xfwm4 -p /general/suspend_compositing_fullscreen -s false >/dev/null
for i in $(seq 1 "$ROUNDS"); do
    echo "round $i"
    if [ $((i % 2)) -eq 1 ]; then ORDER=$VARIANTS
    else ORDER=$(printf '%s\n' $VARIANTS | tac | tr '\n' ' '); fi
    for v in $ORDER; do variant "$v"; done
done
xfconf-query -c xfwm4 -p /general/use_compositing -s true >/dev/null
echo
for v in $VARIANTS; do
    [ -r "mp-$v" ] && awk -v v="$v" '{f+=$1;w+=$2;n++} END{
        printf "%-8s %8.1f frames/s aggregate   %6.2f W\n", v, f/n, w/n}' "mp-$v"
done
