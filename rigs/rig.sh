#!/bin/bash
# The measurement rig that replaces the whole-machine figure.
#
# For each variant, in one window manager session:
#   idle window     - what the session costs with nothing happening
#   throttled run   - identical work for every variant, 60 fps
#   unthrottled run - what the application reaches, and how busy the GPU is
#
# Reported: the compositor's own CPU with the idle window subtracted, the GPU
# busy percentage over each window, and the application's frame rate.
#
#   ./rig.sh [rounds]               default 2; env: IDLE, RUN, FPS, VARIANTS
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
IDLE=${IDLE:-15}
RUN=${RUN:-20}
FPS=${FPS:-60}
ROUNDS=${1:-2}
VARIANTS=${VARIANTS:-"xrender gl-swap gl-fbo"}
source "$(dirname "$0")/../common.sh"
BUSY=$(a=$(amdgpu_device)) && BUSY="$a/gpu_busy_percent" || {
    BUSY=""
    echo "no amdgpu card here: the gpu column will read -" >&2
}
cd "$(dirname "$0")/.." || exit 1

cpu_of ()  { awk '{print ($14 + $15) * 1000 / 100}' /proc/$1/stat; }
all_cpu () { awk '/^cpu /{print ($2+$3+$4+$6+$7+$8) * 1000 / 100}' /proc/stat; }

# Average GPU busy and package power over $1 seconds, result to file $2
# $3 seconds are skipped first, for the reason given on pwr_watch in common.sh
gpu_watch () {
    ( if [ "${3:-0}" -gt 0 ]; then sleep "$3"; fi
      local n=0 s=0 w=0 v end=$((SECONDS + $1))
      while [ $SECONDS -lt $end ]; do
          v=$(cat "$BUSY" 2>/dev/null); s=$((s + ${v:-0}))
          v=$(cat "$PWR" 2>/dev/null); w=$((w + ${v:-0}))
          n=$((n + 1)); sleep 0.1
      done
      # A missing sensor reads -, never a plausible-looking 0
      { [ -n "$BUSY" ] && awk -v s=$s -v n=$n 'BEGIN{printf "%.1f ", (n ? s/n : 0)}' || printf '%s ' -
        [ -n "$PWR" ] && awk -v w=$w -v n=$n 'BEGIN{printf "%.2f", (n ? w/n/1e6 : 0)}' || printf '%s' -
      } > "$2" ) &
    GPUPID=$!
}

env_for () {
    case "$1" in
        xrender)  echo "GL=false ENV=";;
        gl-swap)  echo "GL=true ENV=XFWM4_GL_PRESENT=swap";;
        gl-copy)  echo "GL=true ENV=XFWM4_GL_PRESENT=copy";;
        gl-fbo)   echo "GL=true ENV=XFWM4_GL_PRESENT=fbo";;
        gl-egl)   echo "GL=true ENV=XFWM4_GL_BACKEND=egl";;
        gl-noext) echo "GL=true ENV=XFWM4_GL_NO_EXT=1";;
        none)     echo "GL=false ENV=";;
    esac
}

variant () {
    local v=$1 gl env
    eval "$(env_for $v)"
    gl=$GL; env=$ENV

    xfconf-query -c xfwm4 -p /general/use_gl_compositing -s "$gl" >/dev/null
    env $env "$WM" --replace > "rig-$v.log" 2>&1 &
    local wm=$!
    sleep 7
    if [ "$v" = none ]; then
        xfconf-query -c xfwm4 -p /general/use_compositing -s false >/dev/null
    else
        xfconf-query -c xfwm4 -p /general/use_compositing -s true >/dev/null
    fi
    sleep 3
    [ -r /proc/$wm/stat ] || { echo "  $v: did not start"; return; }

    local iw0 iw1 ia0 ia1 bw0 bw1 ba0 ba1 fps
    gpu_watch "$IDLE" rig-gi
    iw0=$(cpu_of $wm); ia0=$(all_cpu)
    sleep "$IDLE"
    iw1=$(cpu_of $wm); ia1=$(all_cpu)
    wait $GPUPID

    gpu_watch $((RUN - 4)) rig-gb 4
    bw0=$(cpu_of $wm); ba0=$(all_cpu)
    ./tools/fsbench2 "$RUN" windowed "$FPS" > rig-app.log 2>&1
    bw1=$(cpu_of $wm); ba1=$(all_cpu)
    wait $GPUPID

    gpu_watch 8 rig-gu 4
    ./tools/fsbench2 12 windowed > rig-app2.log 2>&1
    fps=$(grep AVERAGE rig-app2.log | awk '{print $2}')
    [ -n "$fps" ] || fps=-
    wait $GPUPID

    awk -v v="$v" -v iw0=$iw0 -v iw1=$iw1 -v ia0=$ia0 -v ia1=$ia1 \
        -v bw0=$bw0 -v bw1=$bw1 -v ba0=$ba0 -v ba1=$ba1 -v fps="$fps" \
        -v gi="$(cat rig-gi)" -v gb="$(cat rig-gb)" -v gu="$(cat rig-gu)" \
        -v idle="$IDLE" -v run="$RUN" '
        BEGIN {
            iwr = (iw1 - iw0) / idle; bwr = (bw1 - bw0) / run;
            iar = (ia1 - ia0) / idle; bar = (ba1 - ba0) / run;
            split (gi, a, " "); split (gb, b, " "); split (gu, c, " ");
            printf "  %-9s wm %5.1f-%.1f=%5.1f ms/s   watts idle %5s run %5s uncap %5s   gpu %4s   fps %7s\n",
                   v, bwr, iwr, bwr - iwr, a[2], b[2], c[2], b[1], fps;
            printf "%.2f %.2f %.2f %.2f %.2f %s %.2f %.2f\n",
                   bwr - iwr, bwr, b[2] - a[2], c[2], b[1], fps, a[2], b[2] >> ("rig-" v);
        }'
    sleep 3
}

save_xfwm_settings
for v in $VARIANTS; do rm -f "rig-$v"; done
xfconf-query -c xfwm4 -p /general/suspend_compositing_fullscreen -s false >/dev/null
for i in $(seq 1 "$ROUNDS"); do
    echo "round $i"
    for v in $VARIANTS; do variant "$v"; done
done
echo
printf '%-9s %9s %9s %9s %9s %9s %9s\n' variant "wm ms/s" "wm raw" "W idle" "W run" "W uncap" "fps"
for v in $VARIANTS; do
    [ -r "rig-$v" ] || continue
    awk -v v="$v" '{a+=$1;b+=$2;d+=$4;f+=$6;g+=$7;h+=$8;n++} END{
        printf "%-9s %9.1f %9.1f %9.2f %9.2f %9.2f %9.2f\n", v, a/n, b/n, g/n, h/n, d/n, f/n}' "rig-$v"
done
