#!/usr/bin/env bash
# A red package cell must leave the failing .phpt tests' evidence downloadable
# under its own name (issue #669). Hermetic: reads a workflow and the gate
# script, needs no network, no build, no binary and no YAML parser.
#
# THE INVARIANT, not the literal: every job that runs build/ci-package-gate.sh
# has an upload step that comes AFTER that job's gate step, whose `if` includes
# failure(), whose `path` covers the four extensions run-tests.php leaves next
# to a failed test, whose if-no-files-found is not `error`, and whose
# retention-days is a decided number of days inside the window below -- and
# whose staging directory is still the tail of the tree the gate hands the
# runner.
#
# Why each half is load-bearing:
#
#   * order, then failure(). `if: failure()` is true only once a previous step
#     of the job has failed (GitHub, "Evaluate expressions in workflows and
#     actions", Status check functions), so an upload placed ABOVE the gate
#     step is dead in every run -- the one shape that looks right in the file
#     and uploads nothing.
#   * failure(), not always() and not an absent `if:`. A step with no `if:`
#     gets the default success(), which is why package-<cell> is not in the
#     run's artifact list on a red cell.
#   * not `error`. The gate can fail before a single test ran -- a package that
#     did not build -- and that must not become a second failure.
#   * retention-days, inside [MIN_DAYS, MAX_DAYS]. Absent, and `0`, both mean
#     "the repository setting" (upload-artifact v7's action.yml: "0 means using
#     default retention"), i.e. an undecided window -- the same shape
#     build/test-release-retention.sh refuses for the artifacts the release job
#     downloads. The floor is #527's own failure-to-fix interval, three days
#     (opened 2026-09-30, the fix #666 merged 2026-10-03): evidence that
#     expires before the investigation ends is the failure this issue exists
#     for. The ceiling is #261's storage argument, which was about a per-run
#     upload on a workflow that runs on every push; this one exists only on a
#     red cell of a workflow that runs on a tag and on rehearsals, and nothing
#     downloads it.
#   * the staging directory. build/ci-package-gate.sh hands the runner a tree
#     it staged itself, so the runner's own <results>/failed-artifacts/ is
#     empty for it (build/run-fpmng-phpt.sh:126) and the evidence is only in
#     the tree. A rename on either side of that has to change both, and this
#     is what notices when only one of them did.
#
# Neither argument is optional in practice and the defaults are the repository
# files: the two arguments exist so this can be run against a mutated copy of
# them, which is how it is known to fail. It writes nothing.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

MIN_DAYS=3
MAX_DAYS=7
workflow=${1:-.github/workflows/release.yml}
gate=${2:-build/ci-package-gate.sh}

fail() { echo "FAIL: $*" >&2; exit 1; }

# One record per step, in file order, fields separated by US (sprintf "%c" 31)
# and terminated by a newline:
#   step <job> <gate|upload|-> <if> <path> <if-no-files-found> <retention-days> <line>
# <line> is where the step starts, which is what the order assertion compares.
#
# US rather than a tab, because tab is IFS *whitespace*: `read` collapses
# consecutive tabs, so a record with an empty column arrives shifted and the
# next assertion blames the wrong field. US is not whitespace to read, so an
# empty field stays empty.
#
# Concatenation, not printf: with no literal in the format string and a
# parenthesized expression among the arguments, the One True Awk on macOS
# (/usr/bin/awk) prints the arguments and silently drops the conversion
# after the expression -- measured, `printf "a%s%s%s%s", S, "x",
# (1 ? "y" : "z"), "w"` gives `a\037xyw`: four values, one separator.
# `print "a" S "x" S (1 ? "y" : "z") S "w"` gives `a\037x\037y\037w`.
#
# `path` is the whole value, block scalar continuation lines included: the four
# globs are one input, and reading only the `path: |` line would see no glob at
# all. `gate` is a step whose body mentions build/ci-package-gate.sh wherever
# the command sits in it -- comment lines are left out of the body, or the
# comment block above a step would make the step before it a gate step.
records() {
    awk '
        function value(line) { sub(/^ *[^:]*:[ \t]*/, "", line); return line }
        function strip(line) { sub(/^ +/, "", line); return line }
        function flush() {
            if (kind != "" || body ~ /build\/ci-package-gate\.sh/) {
                print "step" SEP job SEP \
                    (body ~ /build\/ci-package-gate\.sh/ ? "gate" : (kind == "upload" ? "upload" : "-")) SEP \
                    ifcond SEP path SEP nofiles SEP retention SEP start
            }
            kind = ""; ifcond = ""; path = ""; nofiles = ""; retention = ""; body = ""; inpath = 0; start = 0
        }
        BEGIN { SEP = sprintf("%c", 31) }
        # Both keep their `next`: the first key of a step can be `uses:`, which is
        # the one line that classifies it, and neither that line nor the first
        # key of a job may fall into the body rule below, which matches all.
        # (No apostrophes anywhere in this awk program: it sits inside single
        # quotes, and one would end the program.)
        /^      - / { flush(); inpath = 0; body = $0; start = FNR; next }
        /^  [A-Za-z0-9_-]+:[ \t]*$/ { flush(); job = $0; sub(/^ +/, "", job); sub(/:.*$/, "", job); inpath = 0; body = ""; start = 0; next }
        {
            if (job == "") next
            line = $0; sub(/^[ \t]+/, "", line)
            if (line !~ /^#/) body = body "\n" $0
        }
        /^        if:[ \t]/ { ifcond = value($0); next }
        /^        uses: actions\/upload-artifact@/ { kind = "upload"; next }
        /^        with:[ \t]*$/ { inwith = 1; inpath = 0; next }
        /^        [A-Za-z]/ { inwith = 0; inpath = 0 }
        inwith && /^          path:[ \t]*\|[ \t]*$/ { inpath = 1; path = ""; next }
        inpath && /^            / { p = strip($0); path = path (path == "" ? "" : " ") p; next }
        inwith && /^          [A-Za-z0-9_.-]+:[ \t]/ {
            inpath = 0
            k = $0; sub(/^ +/, "", k); sub(/:.*$/, "", k)
            v = value($0)
            if (k == "path") path = v
            else if (k == "retention-days") retention = v
            else if (k == "if-no-files-found") nofiles = v
            next
        }
        END { flush() }
    ' "$1"
}

[ -f "$workflow" ] || fail "$workflow not found"
[ -f "$gate" ] || fail "$gate not found"
map=$(records "$workflow")
[ -n "$map" ] || fail "$workflow has no step that runs build/ci-package-gate.sh"

jobs=$(awk -F $'\x1f' '$3 == "gate" { print $2 }' <<< "$map" | sort -u)
[ -n "$jobs" ] || fail "$workflow no longer runs build/ci-package-gate.sh"

for job in $jobs; do
    # The last line of this job that runs the gate. An upload has to come after
    # it: `if: failure()` cannot see a failure that has not happened yet.
    last_gate=$(awk -F $'\x1f' -v j="$job" '$3 == "gate" && $2 == j { print $8 }' <<< "$map" | sort -n | tail -1)
    [ -n "$last_gate" ] || fail "job '$job' runs build/ci-package-gate.sh but records() saw no such step"

    found=
    while IFS=$'\x1f' read -r _step _job kind ifcond path nofiles days start; do
        [ "$_job" = "$job" ] || continue
        [ "$kind" = upload ] || continue
        case "$ifcond" in
            *failure*) ;;
            *) continue ;;
        esac
        if [ "$start" -le "$last_gate" ]; then
            fail "the failure() upload of job '$job' is at $workflow:$start, above the gate step at line $last_gate; failure() is false until a step has failed, so it would upload nothing on every run (issue #669)"
        fi
        found=1
        missing=
        for ext in diff out exp log; do
            case "$path" in
                *"sapi/fpmng/tests/*.$ext"*) ;;
                *) missing="$missing *.$ext" ;;
            esac
        done
        [ -z "$missing" ] \
            || fail "the failure upload of job '$job' does not cover$missing of the files run-tests.php leaves next to a failed test; its path is '$path'"
        [ "$nofiles" != error ] \
            || fail "the failure upload of job '$job' sets if-no-files-found: error; a gate that failed before any test ran would then fail a second time"
        # A plain YAML scalar ends at an unquoted ` #`; upload-artifact takes
        # any expression for retention-days, and both an absent one and `0`
        # mean the repository setting, so all three are an undecided window.
        days=${days%% #*}
        case "$days" in
            '') fail "the failure upload of job '$job' sets no retention-days; decide the window (issue #669)" ;;
            0) fail "the failure upload of job '$job' sets retention-days: 0, which upload-artifact reads as the repository default rather than a decided number of days (issue #669)" ;;
            *[!0-9]*) fail "the failure upload of job '$job' sets retention-days to '$days'; this check reads a plain number of days (issue #669)" ;;
        esac
        if [ "$days" -lt "$MIN_DAYS" ]; then
            fail "the failure upload of job '$job' keeps $days days; #527's failure-to-fix interval was three days (2026-09-30 to 2026-10-03), so a shorter window expires the evidence before the investigation ends (issue #669)"
        fi
        if [ "$days" -gt "$MAX_DAYS" ]; then
            fail "the failure upload of job '$job' keeps $days days; nothing downloads it and it exists only on a red cell, so keep it at most $MAX_DAYS (issue #261, #669)"
        fi
        # The staging directory the globs are built on, cross-checked against
        # the tree the gate hands the runner. The space in `${{ matrix.cell }}`
        # has to go before the path can be cut.
        staged=$(sed -n 's|^\(.*\)/sapi/fpmng/tests/\*\.diff.*|\1|p' <<< "$path" | head -1 | tr -d ' ')
        # ... and one component down, the cell's output directory, which only
        # the workflow knows. `work/prepared` is what is left.
        staged=${staged#*/}
        [ -n "$staged" ] || fail "the failure upload of job '$job' names no staging directory: '$path'"
        tree=$(sed -n 's|.*run-fpmng-phpt\.sh \(/[^ ]*\) \([^ ]*\).*|\1|p' "$gate" | head -1)
        [ -n "$tree" ] || fail "$gate hands build/run-fpmng-phpt.sh no absolute tree to read"
        case "$tree" in
            *"/$staged") ;;
            *) fail "the failure upload of job '$job' uploads from '$staged', which is not the tail of the tree '$tree' $gate stages; the runner's own results/failed-artifacts is empty for a tree it did not assemble (build/run-fpmng-phpt.sh:126), so move both sides together (issue #669)" ;;
        esac
        echo "  $job: line $start, failure() upload of '$staged' tests/*.{diff,out,exp,log}, ${days} days"
    done <<< "$map"
    [ -n "$found" ] \
        || fail "job '$job' runs build/ci-package-gate.sh but has no actions/upload-artifact step with if: failure() after it; a failing .phpt in it leaves no evidence (issue #669)"
done

echo "a red package cell uploads the failing .phpt diffs and output"
