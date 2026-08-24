#!/bin/bash
# Put every benchmark result side by side: one column per desktop, one row per
# workload, and notes about what the numbers can and cannot say. Runs of the
# same desktop are averaged.
#
#   ./compare_results.sh [folder]        default results/
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
DIR=${1:-$(dirname "$0")/results}
[ -d "$DIR" ] || { echo "no such folder: $DIR"; exit 1; }
# Only reports that carry the data block at the end can be read; a report from
# before it existed is skipped whole rather than averaged in with no rows of its
# own, which would mix one run's numbers with another's energy and frame rate.
FILES=()
for f in "$DIR"/benchmark-*.txt; do
    grep -q '^row: ' "$f" 2>/dev/null && FILES+=("$f")
done
if [ "${#FILES[@]}" = 0 ]; then
    echo "no readable benchmark results in $DIR"
    echo "(a report needs the '== data' block that benchmark.sh writes)"
    exit 1
fi

awk '
function key_of(path) {
    # benchmark-<date>-<time>-<host>-<de>-<wm>-<session>.txt
    n = split (path, p, "/");
    f = p[n];
    sub (/\.txt$/, "", f);
    n = split (f, q, "-");
    # ...-<de>-<wm>-<version>-<session>, and older files without a version
    if (q[n - 1] ~ /^[0-9]/)
    {
        de = q[n - 3]; wm = q[n - 2]; ver = q[n - 1];
    }
    else
    {
        de = q[n - 2]; wm = q[n - 1]; ver = "";
    }
    sess = (q[n] == "wayland") ? "Wayland" : "X11";
    kk = de "/" wm " " ver " " sess;
    # the desktop and its window manager on one line, the build below
    hdr1[kk] = de "/" wm;
    hdr2[kk] = (ver == "") ? sess : ver " " sess;

    return kk;
}
# A column that could not be measured is left out of its average rather than
# counted as a zero, which is why each one carries its own count
function add(kk, label, w, c) {
    if (!(label in rowseen)) { rowseen[label] = 1; rows[++nrows] = label }
    if (w != "-") { pw[kk, label] += w; pwn[kk, label]++ }
    if (c != "-") { cpu[kk, label] += c; cpun[kk, label]++ }
}
function val(kk, label, a, n) {
    return n[kk, label] ? a[kk, label] / n[kk, label] : ""
}
function table(what, a, n, unit,    i, j, k2, line, v) {
    printf "%-18s", what;
    for (j = 1; j <= nkeys; j++) printf "%16s", hdr1[order[j]];
    printf "\n%-18s", "";
    for (j = 1; j <= nkeys; j++) printf "%16s", hdr2[order[j]];
    printf "\n";
    for (i = 1; i <= nrows; i++)
    {
        if (rows[i] == "total") continue;
        printf "%-18s", rows[i];
        for (j = 1; j <= nkeys; j++)
        {
            v = val(order[j], rows[i], a, n);
            if (v == "") printf "%16s", "-";
            else printf "%16.2f", v;
        }
        printf "\n";
    }
    # A rule as wide as the table, so the total stands apart
    line = "";
    for (j = 0; j < 18 + 16 * nkeys; j++) line = line "\u2500";
    print line;
    printf "%-18s", "total";
    for (j = 1; j <= nkeys; j++)
    {
        v = val(order[j], "total", a, n);
        if (v == "") printf "%16s", "-"; else printf "%16.2f", v;
    }
    printf "   %s\n\n", unit;
}


FNR == 1 {
    k = key_of(FILENAME);
    if (!(k in seen)) { seen[k] = 1; order[++nkeys] = k; }
    runs[k]++;
}

/^display:/  {
    sub (/^display: */, "");
    line = $0;
    # scale 1 and scale 1.00 are the same thing
    if (match (line, /scale [0-9.]+/))
    {
        v = substr (line, RSTART + 6, RLENGTH - 6) + 0;
        line = substr (line, 1, RSTART - 1) "scale " v;
    }
    disp[k] = line;
}
/^session:/  { if ($2 == "x11") isx11[k] = 1 }
/^compositor:/ { comp[k] = $2 " " $3; if (/compositing OFF/) nocomp[k] = 1 }
/^work:/     { sub (/^work: */, ""); work[k] = $0 }
/^energy:/   { en[k] += $2; enn[k]++ }
/^speed:/                       { fps[k] += $2;   fpsn[k]++ }
/fps fullscreen$/               { fsf[k] += $1;   fsn[k]++ }
/fps fullscreen, asking/        { byf[k] += $1;   byn[k]++ }

# row: <watts> <cpu seconds> <seconds elapsed> <name>, as benchmark.sh writes
# it at the end of its report, with a "-" for a column it could not measure.
# The pretty table above it is for people: its columns move whenever the
# wording does, so it is not what is read here.
/^row: / {
    label = $5;
    for (i = 6; i <= NF; i++) label = label " " $i;
    add(k, label, $2, $3);
}




END {
    if (nkeys == 0) { print "nothing to compare"; exit 1 }

    print "== power, watts (lower is better)";
    table("workload", pw, pwn, "average while working");
    print "== compositor CPU, seconds (lower is better)";
    table("workload", cpu, cpun, "for the whole job");

    print "== energy for all the work, joules (lower is better)";
    printf "%-18s", "energy";
    for (j = 1; j <= nkeys; j++)
        if (enn[order[j]]) printf "%16.0f", en[order[j]] / enn[order[j]];
        else printf "%16s", "-";
    printf "\n\n";

    print "== frames a second flat out, windowed (higher is better)";
    printf "%-18s", "fps";
    for (j = 1; j <= nkeys; j++)
        printf "%16.1f", (fpsn[order[j]] ? fps[order[j]] / fpsn[order[j]] : 0);
    printf "\n\n";

    print "== notes";

    # Who won what, first: it is what the tables are read for
    bw = 1e9; bwk = ""; bc = 1e9; bck = ""; bf = 0; bfk = "";
    for (j = 1; j <= nkeys; j++)
    {
        k2 = order[j];
        v = val(k2, "total", pw, pwn);   if (v != "" && v < bw) { bw = v; bwk = k2 }
        v = val(k2, "total", cpu, cpun);  if (v != "" && v < bc) { bc = v; bck = k2 }
        v = (fpsn[k2] ? fps[k2] / fpsn[k2] : 0);
        if (v > bf) { bf = v; bfk = k2 }
    }
    # Nothing wins a column that nobody could measure
    if (bfk != "") printf "* fastest: %s, %.1f fps\n", bfk, bf;
    if (bwk != "") printf "* more efficient: %s, %.2f W\n", bwk, bw;
    if (bck != "") printf "* least CPU time: %s, %.2f s\n", bck, bc;

    # Only what changes a reading of the tables. Long notes do not get read.
    for (j = 1; j <= nkeys; j++)
        if (runs[order[j]] == 1)
            printf "* %s: 1 run\n", order[j];
        else
            printf "* %s: average of %d runs\n", order[j], runs[order[j]];

    # Were the conditions the same?
    same = 1; first = "";
    for (j = 1; j <= nkeys; j++)
    {
        if (first == "") first = disp[order[j]];
        else if (disp[order[j]] != first) same = 0;
    }
    if (same) printf "* same display everywhere: %s\n", first;
    else
    {
        print "* CAREFUL, not the same display:";
        for (j = 1; j <= nkeys; j++)
            printf "    %-14s %s\n", order[j], disp[order[j]];
    }

    # A window manager that does not composite is the floor, not a rival. It
    # said so itself when it ran; a low CPU total is not the same thing,
    # since a cheap compositor has one too.
    for (j = 1; j <= nkeys; j++)
        if (nocomp[order[j]])
            printf "* %s is not compositing: read it as the floor\n", order[j];

    printf "* fullscreen asked test if the compositor is disabled when asked\n";

    x = "";
    for (j = 1; j <= nkeys; j++)
        if (isx11[order[j]]) x = x (x == "" ? "" : ", ") order[j];
    if (x != "")
        printf "* on X11 the CPU column doesn'\''t include what the X server composites\n";
}
' "${FILES[@]}"
