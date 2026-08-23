#!/bin/bash
# Compare two checkpoint folders made by validate.sh (usagebench with
# BENCH_CHECKPOINT_DIR), one from each session under comparison.
#
#   ./compare_runs.sh <folder A> <folder B>
#
# Two answers per checkpoint:
#   geometry   did both window managers put the window where it was asked,
#              at the size that was asked? (position gets a 100 px allowance
#              for decorations; size must match exactly)
#   pixels     are the two screenshots identical? They photograph only the
#              window's own content, so the desktops' looks are not in them.
#
# Exit status 0 when every checkpoint agrees.
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
A=${1:?usage: compare_runs.sh <folder A> <folder B>}
B=${2:?usage: compare_runs.sh <folder A> <folder B>}
[ -r "$A/manifest.txt" ] || { echo "no manifest in $A"; exit 2; }
[ -r "$B/manifest.txt" ] || { echo "no manifest in $B"; exit 2; }
FAILED=0

echo "== geometry: what was asked against what each session did"
# Lines starting with -- are notes the run left about itself, not checkpoints
join -j 2 <(grep -v '^--' "$A/manifest.txt" | sort -k2) \
          <(grep -v '^--' "$B/manifest.txt" | sort -k2) 2>/dev/null |
awk '
{
    # join output: the name, then the fields of A, then the fields of B
    name = $1;
    split ($4,  aa, ","); split ($5,  as, "x");   # A asked
    split ($7,  ag, ","); split ($8,  gs, "x");   # A got
    split ($14, bg, ","); split ($15, hs, "x");   # B got
    bad = "";
    if (gs[1] != as[1] || gs[2] != as[2]) bad = bad " A-size";
    if (hs[1] != as[1] || hs[2] != as[2]) bad = bad " B-size";
    if (ag[1] > aa[1] + 100 || ag[1] < aa[1] - 100 ||
        ag[2] > aa[2] + 100 || ag[2] < aa[2] - 100) bad = bad " A-position";
    if (bg[1] > aa[1] + 100 || bg[1] < aa[1] - 100 ||
        bg[2] > aa[2] + 100 || bg[2] < aa[2] - 100) bad = bad " B-position";
    if (bad == "")
        printf "  %-14s ok\n", name;
    else
    {
        printf "  %-14s WRONG:%s (asked %s %s, A got %s %s, B got %s %s)\n",
               name, bad, $4, $5, $7, $8, $14, $15;
        exit_code = 1;
    }
}
END { exit exit_code }' || FAILED=1
echo

echo "== pixels: the window content, photographed"
for f in "$A"/ck-*.ppm; do
    [ -e "$f" ] || { echo "  no screenshots in $A (no capture tool there?)"; break; }
    n=$(basename "$f")
    if [ ! -r "$B/$n" ]; then
        echo "  $n: only in $A"
        FAILED=1
        continue
    fi
    if cmp -s "$f" "$B/$n"; then
        echo "  $n: identical"
    else
        # How different: count differing bytes over the smaller payload
        bytes=$(cmp -l "$f" "$B/$n" 2>/dev/null | wc -l)
        size=$(stat -c %s "$f")
        awk -v n="$n" -v b="$bytes" -v s="$size" \
            'BEGIN{printf "  %s: DIFFERS, %.1f%% of the bytes\n", n, 100*b/s}'
        FAILED=1
    fi
done
echo

[ "$FAILED" = 0 ] && echo "the two sessions did the same thing, pixel for pixel" \
                  || echo "the sessions DIVERGED, see above"
exit $FAILED
