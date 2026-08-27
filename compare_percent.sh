#!/bin/bash
# Put the four totals of every benchmark result in one boxed table as
# percentages, the best of each line being 100%.
#
#   ./compare_percent.sh [folder]        default results/
case "${1:-}" in
    -h|--help) sed -n '2,/^[^#]/ { /^#/ s/^# \{0,1\}//p }' "$0"; exit 0;;
esac
set -u
DIR=${1:-$(dirname "$0")/results}
[ -d "$DIR" ] || { echo "no such folder: $DIR"; exit 1; }

# Only reports carrying the data block at the end can be read; the table above
# it in a report moves whenever the wording does.
FILES=()
for f in "$DIR"/*.txt; do
    grep -q '^test: ' "$f" 2>/dev/null && FILES+=("$f")
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
# What ran is read from the report itself, never from the name of the file:
# a report that was renamed, or whose header was corrected by hand, has to
# come out of here as what it says it is.
function key_from_report(path,    line, s, de, wm, st, ver, kk) {
    while ((getline line < path) > 0)
    {
        if (line ~ /^session:/)
        {
            s = line;
            sub (/^session: */, "", s);
            st = s;
            sub (/ .*$/, "", st);
            st = tolower (st);
            # Only the first group: a hand-written note after it would end up
            # in the header and take the whole table wider with it
            if (match (s, /\([^)]*\)/))
            {
                de = tolower (substr (s, RSTART + 1, RLENGTH - 2));
                gsub (/[:\/ ]/, "-", de);
            }
        }
        else if (line ~ /^compositor:/)
        {
            wm = line;
            sub (/^compositor: */, "", wm);
            ver = wm;
            sub (/^[^ ]* */, "", ver);
            sub (/ *\(.*$/, "", ver);
            sub (/ .*$/, "", wm);
            wm = tolower (wm);
        }
        else if (line ~ /^== data/)
        {
            break;
        }
    }
    close (path);
    if (st == "" || wm == "") return "";
    if (de == "" || de == "unknown-desktop") de = "unknown";

    kk = de "/" wm "/" st;
    h1[kk] = de; h2[kk] = wm; h3[kk] = st;
    if (!(kk in h4)) h4[kk] = ver;
    else if (h4[kk] != ver) vmixed[kk] = 1;

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

# Higher is better for frames, lower for the rest, and both end up as "100% is
# the best": the winner divides the others one way round or the other.
function measure_line(what, low,    i, v, best, cells) {
    best = "";
    for (i = 1; i <= nkeys; i++)
    {
        v = one[order[i], what];
        if (v == "" || v == "-" || v + 0 == 0) continue;
        if (best == "" || (low ? v + 0 < best : v + 0 > best)) best = v + 0;
    }
    for (i = 1; i <= nkeys; i++)
    {
        v = one[order[i], what];
        if (v == "" || v == "-" || v + 0 == 0 || best == "")
        {
            cells[i] = pad("-", wid[i]);
            continue;
        }
        # One decimal, or two figures a fraction apart both read 100% and the
        # bold is the only thing saying which one won
        cells[i] = sprintf ("%.1f%%", 100 * (low ? best / v : v / best));
        cells[i] = (v + 0 == best) ? B pad(cells[i], wid[i]) O :
                                     pad(cells[i], wid[i]);
    }
    printf "\342\224\202%s", pad(what, w0);
    for (i = 1; i <= nkeys; i++) printf "\342\224\202%s", cells[i];
    print "\342\224\202";
}

FNR == 1 {
    k = key_from_report(FILENAME);
    if (k == "")
    {
        ignored = ignored (ignored == "" ? "" : ", ") FILENAME;
        next;
    }
    if (!(k in seen)) { seen[k] = 1; order[++nkeys] = k }
    runs[k]++;
}
k == "" { next }
/compositing OFF/ { nocomp[k] = 1 }
/^display:/  {
    d = $0;
    sub (/^display: */, "", d);
    # scale 1 and scale 1.00 are the same thing
    if (match (d, /scale [0-9.]+/))
    {
        d = substr (d, 1, RSTART - 1) "scale " (substr (d, RSTART + 6, RLENGTH - 6) + 0);
    }
    if (disp[k] == "") disp[k] = d;
    else if (disp[k] != d) mixed[k] = 1;
}
/^energy:/   { sum[k, "energy"] += $2; cnt[k, "energy"]++ }
/^speed:/    { sum[k, "frames per second"] += $2; cnt[k, "frames per second"]++ }

# test: <watts> <cpu seconds> <seconds elapsed> <name>, as benchmark.sh writes
# it. Only the total line is wanted here, and runs of the same desktop are
# averaged, so each column carries its count.
/^test: / {
    label = $5;
    for (i = 6; i <= NF; i++) label = label " " $i;
    # A desktop that skipped tests has a smaller total than one that ran them
    # all, and would win the table for having done less
    if ($2 == "-" && $3 == "-" && label != "total") short[k] = 1;
    if (label != "total") next;
    if ($2 != "-") { sum[k, "power"] += $2; cnt[k, "power"]++ }
    if ($3 != "-") { sum[k, "desktop CPU"] += $3; cnt[k, "desktop CPU"]++ }
}

END {
    if (nkeys == 0)
    {
        print "nothing to compare";
        if (ignored != "") printf "no session or compositor line in: %s\n", ignored;
        exit 1;
    }

    nmeasures = split ("frames per second,energy,power,desktop CPU", measures, ",");
    for (i = 1; i <= nkeys; i++)
        for (j = 1; j <= nmeasures; j++)
        {
            one[order[i], measures[j]] = cnt[order[i], measures[j]] ?
                sum[order[i], measures[j]] / cnt[order[i], measures[j]] : "-";
        }

    # Wide enough for the longest thing in each column
    w0 = length ("measure");
    for (j = 1; j <= nmeasures; j++) if (length (measures[j]) > w0) w0 = length (measures[j]);
    w0 += 2;
    for (i = 1; i <= nkeys; i++)
    {
        k2 = order[i];
        if (vmixed[k2]) h4[k2] = h4[k2] "*";
        wid[i] = length (h1[k2]);
        if (length (h2[k2]) > wid[i]) wid[i] = length (h2[k2]);
        if (length (h3[k2]) > wid[i]) wid[i] = length (h3[k2]);
        if (length (h4[k2]) > wid[i]) wid[i] = length (h4[k2]);
        if (wid[i] < 6) wid[i] = 6;
        wid[i] += 2;
    }

    print "Totals as percentages - the best is 100%, in bold\n";
    rule("\342\224\214", "\342\224\254", "\342\224\220");
    for (i = 1; i <= nkeys; i++) hdr[i] = h1[order[i]];
    line("measure", hdr);
    for (i = 1; i <= nkeys; i++) hdr[i] = h2[order[i]];
    line("", hdr);
    for (i = 1; i <= nkeys; i++) hdr[i] = h3[order[i]];
    line("", hdr);
    for (i = 1; i <= nkeys; i++) hdr[i] = h4[order[i]];
    line("", hdr);
    rule("\342\224\234", "\342\224\274", "\342\224\244");
    for (j = 1; j <= nmeasures; j++) measure_line(measures[j], measures[j] != "frames per second");
    rule("\342\224\224", "\342\224\264", "\342\224\230");
    print "";

    print "* a line reads: 100% is the best figure, 50% is half as good";
    print "* frames per second is the uncapped test, the other three are the";
    print "  totals of every test that ran";

    s = "";
    for (i = 1; i <= nkeys; i++)
        if (short[order[i]]) s = s (s == "" ? "" : ", ") order[i];
    if (s != "") printf "* NOT COMPARABLE: some tests didn'\''t run on: %s\n", s;

    s = "";
    for (i = 1; i <= nkeys; i++)
        if (nocomp[order[i]]) s = s (s == "" ? "" : ", ") order[i];
    if (s != "") printf "* no compositing on: %s\n", s;

    s = "";
    for (i = 1; i <= nkeys; i++)
        if (vmixed[order[i]]) s = s (s == "" ? "" : ", ") order[i];
    if (s != "") printf "* marked *: the runs were not all the same version: %s\n", s;

    # Were the conditions the same? A different refresh rate or scale is not a
    # detail: at 120 Hz the compositor repaints twice as often as at 60.
    same = 1; first = "";
    for (i = 1; i <= nkeys; i++)
    {
        if (mixed[order[i]]) same = 0;
        if (first == "") first = disp[order[i]];
        else if (disp[order[i]] != first) same = 0;
    }
    if (same)
    {
        printf "* same display everywhere: %s\n", first;
    }
    else
    {
        print "* NOT COMPARABLE: the display was not the same in every run";
        for (i = 1; i <= nkeys; i++)
            printf "    %s: %s\n", order[i], disp[order[i]];
    }

    if (ignored != "")
        printf "* left out, no session or compositor line: %s\n", ignored;

    same = 1;
    for (i = 2; i <= nkeys; i++) if (runs[order[i]] != runs[order[1]]) same = 0;
    if (same)
    {
        printf "* %d run%s each\n", runs[order[1]], runs[order[1]] == 1 ? "" : "s";
    }
    else
    {
        for (i = 1; i <= nkeys; i++)
            printf "* %s: %d run%s\n", order[i], runs[order[i]],
                   runs[order[i]] == 1 ? "" : "s";
    }
}
' "${FILES[@]}"
