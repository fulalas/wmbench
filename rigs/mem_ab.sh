#!/bin/bash
# What a window costs the compositor in memory, XRender against OpenGL.
#
# The GL renderer keeps a GLX pixmap and a texture per window where XRender
# keeps a picture, and for small windows a shadow image as well. A compositor
# that grows faster per window than the one it replaces would
# be a regression whatever its frame rate.
#
# Reported: the window manager's resident set with no extra windows and with
# NWIN of them, the difference per window, and the change in video memory the
# whole system reports.
#
#   ./mem_ab.sh [rounds]            default 2; env: NWIN
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
NWIN=${NWIN:-40}
ROUNDS=${1:-2}
source "$(dirname "$0")/../common.sh"
cd "$(dirname "$0")/.." || exit 1

VRAM=$(a=$(amdgpu_device)) && VRAM="$a/mem_info_vram_used" || {
    VRAM=""
    echo "no amdgpu card here: the vram column will read -" >&2
}

rss_of () { awk '/^VmRSS:/{print $2}' "/proc/$1/status"; }
vram () { [ -n "$VRAM" ] && [ -r "$VRAM" ] && cat "$VRAM" || echo -; }

variant () {                    # $1 label, $2 use_gl
    xfconf-query -c xfwm4 -p /general/use_gl_compositing -s "$2" >/dev/null
    "$WM" --replace > "me-$1.log" 2>&1 &
    local wm=$!
    sleep 7
    xfconf-query -c xfwm4 -p /general/use_compositing -s true >/dev/null
    sleep 4
    [ -r "/proc/$wm/status" ] || { echo "  $1: did not start"; return; }

    local r0 r1 v0 v1
    r0=$(rss_of $wm); v0=$(vram)

    ./tools/manywin "$NWIN" 26 > me-win.log 2>&1 &
    local mw=$!
    # Let every window be mapped, drawn and composited at least once
    sleep 14
    r1=$(rss_of $wm); v1=$(vram)
    kill $mw 2>/dev/null; wait $mw 2>/dev/null
    sleep 3

    awk -v l="$1" -v r0="$r0" -v r1="$r1" -v v0="$v0" -v v1="$v1" -v n="$NWIN" '
        BEGIN {
            vd = (v0 == "-" || v1 == "-") ? "-" : sprintf ("%+.1f", (v1 - v0) / 1048576);
            printf "  %-8s rss %6.1f -> %6.1f MB  (%5.0f kB a window)   vram %5s MB\n",
                   l, r0/1024, r1/1024, (r1 - r0) / n, vd;
            printf "%.1f %.1f %.0f %s\n", r0/1024, r1/1024, (r1-r0)/n,
                   vd >> ("me-" l);
        }'
    sleep 2
}

/bin/rm -f me-xrender me-opengl
save_xfwm_settings
xfconf-query -c xfwm4 -p /general/suspend_compositing_fullscreen -s false >/dev/null
for i in $(seq 1 "$ROUNDS"); do
    echo "round $i ($NWIN windows)"
    if [ $((i % 2)) -eq 1 ]; then
        variant xrender false; variant opengl true
    else
        variant opengl true; variant xrender false
    fi
done
echo
for v in xrender opengl; do
    [ -r "me-$v" ] && awk -v v="$v" '{a+=$1;b+=$2;c+=$3;
        if ($4 == "-") nod = 1; else d+=$4; n++} END{
        vd = nod ? "-" : sprintf ("%+.1f", d/n);
        printf "%-8s rss %6.1f -> %6.1f MB  (%5.0f kB a window)   vram %5s MB\n",
               v, a/n, b/n, c/n, vd}' "me-$v"
done
