#!/bin/bash
# Glue for screenshot tools that cannot write PPM themselves.
#
#   capture_ppm.sh <tool and its output flag...> <out.ppm>
#
# Runs the tool into a temporary PNG and converts it with ffmpeg. Used by the checks
# through BENCH_CAPTURE_CMD on Wayland; grim writes PPM itself and skips this.
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
OUT=${!#}
PNG="${OUT%.ppm}.png"

"${@:1:$#-1}" "$PNG" >/dev/null 2>&1 || exit 1

ffmpeg -loglevel error -y -i "$PNG" -frames:v 1 "$OUT"
rc=$?
rm -f "$PNG"
exit $rc
