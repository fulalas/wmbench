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
set -- "$DIR"/benchmark-*.txt
[ -r "$1" ] || { echo "no benchmark results in $DIR"; exit 1; }

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
function add(kk, label, w, c, t) {
    if (!(label in rowseen)) { rowseen[label] = 1; rows[++nrows] = label }
    pw[kk, label] += w; cpu[kk, label] += c; el[kk, label] += t;
    cnt[kk, label]++;
}
function val(kk, label, a) {
    return cnt[kk, label] ? a[kk, label] / cnt[kk, label] : ""
}
function table(what, a, unit,    i, j, k2, line, v) {
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
            v = val(order[j], rows[i], a);
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
        v = val(order[j], "total", a);
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
/^compositor:/ { comp[k] = $2 " " $3 }
/^work:/     { sub (/^work: */, ""); work[k] = $0 }
/^energy:/   { en[k] += $2; enn[k]++ }
/^speed:/                       { fps[k] += $2;   fpsn[k]++ }
/fps fullscreen$/               { fsf[k] += $1;   fsn[k]++ }
/fps fullscreen, asking/        { byf[k] += $1;   byn[k]++ }

# total          12.51 W (avg.)          2.50 s       156.7 s
/^total/ {
    add(k, "total", $2, $5, $7);
    next
}

# idle                   4.80 W          0.00 s        20.0 s
NF >= 7 && $NF == "s" && $(NF - 2) == "s" && $(NF - 4) == "W" {
    label = $1;
    for (i = 2; i <= NF - 6; i++) label = label " " $i;
    add(k, label, $(NF - 5), $(NF - 3), $(NF - 1));
}




END {
    if (nkeys == 0) { print "nothing to compare"; exit 1 }

    print "== power, watts (lower is better)";
    table("workload", pw, "average while working");
    print "== compositor CPU, seconds (lower is better)";
    table("workload", cpu, "for the whole job");

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

    # Only what changes a reading of the tables. Long notes do not get read.
    for (j = 1; j <= nkeys; j++)
        if (runs[order[j]] == 1)
            printf "* %s: one run, not an average\n", order[j];
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

    # A window manager that does not composite is the floor, not a rival
    for (j = 1; j <= nkeys; j++)
    {
        c = val(order[j], "total", cpu);
        if (c != "" && c < 1.0)
            printf "* %s is not compositing: read it as the floor\n", order[j];
    }

    printf "* the second fullscreen row asks compositing to step aside: less power there means the desktop did it\n";

    x = "";
    for (j = 1; j <= nkeys; j++)
        if (isx11[order[j]]) x = x (x == "" ? "" : ", ") order[j];
    if (x != "")
        printf "* on X11 some compositing happens inside the X server, so the CPU column understates it\n";

    # Who won what
    bw = 1e9; bwk = ""; bc = 1e9; bck = ""; bf = 0; bfk = "";
    for (j = 1; j <= nkeys; j++)
    {
        k2 = order[j];
        v = val(k2, "total", pw);   if (v != "" && v < bw) { bw = v; bwk = k2 }
        v = val(k2, "total", cpu);  if (v != "" && v < bc) { bc = v; bck = k2 }
        v = (fpsn[k2] ? fps[k2] / fpsn[k2] : 0);
        if (v > bf) { bf = v; bfk = k2 }
    }
    printf "* cheapest: %s, %.2f W\n", bwk, bw;
    printf "* least processor time: %s, %.2f s\n", bck, bc;
    printf "* fastest: %s, %.1f fps\n", bfk, bf;
}
' "$@"
