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
[ $# -ge 2 ] || { echo "usage: $0 <tool and its output flag...> <out.ppm>" >&2; exit 1; }
OUT=${!#}
PNG="${OUT%.ppm}.png"

# lib/capture.c hands every capture in a process the same path, so the PNG is
# the same file every time. Clear it first and on failure, or a tool that exits
# 0 without writing one leaves the previous screenshot to be converted as if it
# were this one.
rm -f "$PNG"
"${@:1:$#-1}" "$PNG" >/dev/null 2>&1 || { rm -f "$PNG"; exit 1; }

ffmpeg -loglevel error -y -i "$PNG" -frames:v 1 "$OUT"
rc=$?
rm -f "$PNG"
exit $rc
