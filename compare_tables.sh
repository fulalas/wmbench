#!/bin/bash
# Put every benchmark result in two boxed tables, one for power and one for
# compositor CPU: one column per desktop, one line per test, the best of each
# line in bold.
#
#   ./compare_tables.sh [folder]        default results/
#
# compare_results.sh answers the same question in plain columns and says more
# about what the numbers can and cannot mean. This one is for reading at a
# glance and for pasting somewhere.
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
DIR=${1:-$(dirname "$0")/results}
[ -d "$DIR" ] || { echo "no such folder: $DIR"; exit 1; }

# Only reports carrying the data block at the end can be read; the table above
# it in a report moves whenever the wording does.
FILES=()
for f in "$DIR"/benchmark-*.txt; do
    grep -q '^row: ' "$f" 2>/dev/null && FILES+=("$f")
done
if [ "${#FILES[@]}" = 0 ]; then
    echo "no readable benchmark results in $DIR"
    echo "(a report needs the '== data' block that benchmark.sh writes)"
    exit 1
fi

# Bold only where somebody is watching; a file has no use for escape codes
if [ -t 1 ]; then
    B=$'\033[1m'; O=$'\033[0m'
else
    B=""; O=""
fi

awk -v B="$B" -v O="$O" '
# benchmark-<date>-<time>-<host>-<de>-<wm>-<version>-<session>.txt, and older
# files without a version
function key_of(path,    n, p, f, q, de, wm) {
    n = split (path, p, "/");
    f = p[n];
    sub (/\.txt$/, "", f);
    n = split (f, q, "-");
    if (q[n - 1] ~ /^[0-9]/)
    {
        de = q[n - 3]; wm = q[n - 2];
    }
    else
    {
        de = q[n - 2]; wm = q[n - 1];
    }
    kk = de "/" wm "/" q[n];
    # The three parts go one above the other: on one line the header alone is
    # wider than the numbers under it, and the table wraps
    h1[kk] = de; h2[kk] = wm; h3[kk] = q[n];

    return kk;
}

function pad(s, w,    l, left) {         # centred, the way the box wants it
    l = length (s);
    if (l >= w) return s;
    left = int ((w - l) / 2);
    return sprintf ("%*s%s%*s", left, "", s, w - l - left, "");
}

function rule(l, m, r,    i, j, s) {
    s = l;
    for (j = 0; j < w0; j++) s = s "\342\224\200";
    for (i = 1; i <= nkeys; i++)
    {
        s = s m;
        for (j = 0; j < wid[i]; j++) s = s "\342\224\200";
    }
    print s r;
}

function line(first, arr,    i, s) {     # one line of the box
    s = "\342\224\202" pad(first, w0);
    for (i = 1; i <= nkeys; i++) s = s "\342\224\202" pad(arr[i], wid[i]);
    print s "\342\224\202";
}

# One table: which of the two numbers, and what to call it
function table(idx, title,    i, j, k2, v, best, cells, hdr, blank) {
    printf "%s - lower is better, best in bold\n\n", title;
    rule("\342\224\214", "\342\224\254", "\342\224\220");
    for (i = 1; i <= nkeys; i++) hdr[i] = h1[order[i]];
    line("workload", hdr);
    for (i = 1; i <= nkeys; i++) hdr[i] = h2[order[i]];
    line("", hdr);
    for (i = 1; i <= nkeys; i++) hdr[i] = h3[order[i]];
    line("", hdr);
    rule("\342\224\234", "\342\224\274", "\342\224\244");
    for (j = 1; j <= nrows; j++)
    {
        if (rows[j] == "total")
        {
            rule("\342\224\234", "\342\224\274", "\342\224\244");
        }
        best = "";
        for (i = 1; i <= nkeys; i++)
        {
            v = val[order[i], rows[j], idx];
            if (v != "" && v != "-" && (best == "" || v + 0 < best))
            {
                best = v + 0;
            }
        }
        for (i = 1; i <= nkeys; i++)
        {
            v = val[order[i], rows[j], idx];
            if (v == "") v = "-";
            # The bold codes are not printed characters, so the cell is padded
            # first and dressed afterwards, or every column would come out short
            if (v != "-" && best != "" && v + 0 == best)
            {
                cells[i] = B pad(v, wid[i]) O;
            }
            else
            {
                cells[i] = pad(v, wid[i]);
            }
        }
        # already padded above
        printf "\342\224\202%s", pad(rows[j], w0);
        for (i = 1; i <= nkeys; i++) printf "\342\224\202%s", cells[i];
        print "\342\224\202";
    }
    rule("\342\224\224", "\342\224\264", "\342\224\230");
    print "";
}

FNR == 1 {
    k = key_of(FILENAME);
    if (!(k in seen)) { seen[k] = 1; order[++nkeys] = k }
}
/compositing OFF/ { nocomp[k] = 1 }

# row: <watts> <cpu seconds> <seconds elapsed> <name>, as benchmark.sh writes
# it. Runs of the same desktop are averaged, so each column carries its count.
/^row: / {
    label = $5;
    for (i = 6; i <= NF; i++) label = label " " $i;
    if (!(label in rowseen)) { rowseen[label] = 1; rows[++nrows] = label }
    if ($2 != "-") { sum[k, label, 1] += $2; cnt[k, label, 1]++ }
    if ($3 != "-") { sum[k, label, 2] += $3; cnt[k, label, 2]++ }
}

END {
    if (nkeys == 0) { print "nothing to compare"; exit 1 }

    # The total goes last, under a rule of its own
    for (j = 1; j <= nrows; j++) if (rows[j] == "total") tj = j;
    if (tj)
    {
        for (j = tj; j < nrows; j++) rows[j] = rows[j + 1];
        rows[nrows] = "total";
    }

    for (i = 1; i <= nkeys; i++)
    {
        k2 = order[i];
        for (j = 1; j <= nrows; j++)
        {
            for (n = 1; n <= 2; n++)
            {
                val[k2, rows[j], n] = cnt[k2, rows[j], n] ?
                    sprintf ("%.2f", sum[k2, rows[j], n] / cnt[k2, rows[j], n]) : "-";
            }
            if (rows[j] != "total" && val[k2, rows[j], 1] == "-") short[k2] = 1;
        }
    }

    # Wide enough for the longest thing in each column
    w0 = length ("workload");
    for (j = 1; j <= nrows; j++) if (length (rows[j]) > w0) w0 = length (rows[j]);
    w0 += 2;
    for (i = 1; i <= nkeys; i++)
    {
        k2 = order[i];
        wid[i] = length (h1[k2]);
        if (length (h2[k2]) > wid[i]) wid[i] = length (h2[k2]);
        if (length (h3[k2]) > wid[i]) wid[i] = length (h3[k2]);
        if (wid[i] < 6) wid[i] = 6;
        wid[i] += 2;
    }

    table(1, "Power (W)");
    table(2, "Compositor CPU (s)");

    s = "";
    for (i = 1; i <= nkeys; i++)
        if (short[order[i]]) s = s (s == "" ? "" : ", ") order[i];
    if (s != "") printf "* some tests didn'\''t run on: %s\n", s;

    s = "";
    for (i = 1; i <= nkeys; i++)
        if (nocomp[order[i]]) s = s (s == "" ? "" : ", ") order[i];
    if (s != "") printf "* no compositing on: %s\n", s;

    for (i = 1; i <= nkeys; i++)
        if (h3[order[i]] == "x11")
        {
            print "* on X11 some of the CPU is used by the X server, which we don'\''t measure";
            break;
        }
}
' "${FILES[@]}"
