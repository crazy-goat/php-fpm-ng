#!/usr/bin/env bash
# A red package cell must leave the failing .phpt tests' evidence downloadable
# under its own name (issue #669). Hermetic: reads a workflow and the gate
# script, needs no network, no build, no binary and no YAML parser.
#
# THE INVARIANT, not the literal: every job that runs build/ci-package-gate.sh
# has an upload step whose `if` includes failure(), whose `path` covers the four
# extensions run-tests.php leaves next to a failed test, whose if-no-files-found
# is not `error`, and whose retention is a decided and short number of days --
# and whose directory is still the one the gate stages the tree in.
#
# Why each half is load-bearing:
#
#   * failure(), not always() and no `if:` at all. A step with no `if:` gets the
#     default success() (GitHub, "Evaluate expressions in workflows and
#     actions", Status check functions), which is exactly the reason issue #527
#     had no log to read: the one artifact that carried the evidence was
#     skipped by the very failure it was needed for.
#   * not `error`. The gate can fail before a single test ran -- a package that
#     did not build -- and that must not become a second failure.
#   * retention-days. Absent means the action's default, which is the repository
#     setting: an undecided window, which is what this check exists to refuse
#     (build/test-release-retention.sh refuses the same shape for the artifacts
#     the release job downloads). Over MAX_DAYS means the argument for issue
#     #261 -- bytes kept per run of a workflow that runs on a tag -- no longer
#     holds, since nothing downloads this artifact at all.
#   * the directory. build/ci-package-gate.sh hands the runner a tree it staged
#     itself, so the runner's own <results>/failed-artifacts/ is empty for it
#     (build/run-fpmng-phpt.sh:126) and the evidence is only in the tree. A
#     rename on either side of that fact has to change both, and this is what
#     notices when only one of them did.
#
# Neither argument is optional in practice, and the defaults are the repository
# files: the two arguments exist so the check can be run against a mutated copy
# of them, which is how it is known to fail.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

MAX_DAYS=7
workflow=${1:-.github/workflows/release.yml}
gate=${2:-build/ci-package-gate.sh}

fail() { echo "FAIL: $*" >&2; exit 1; }

# One TAB-separated record per step, in file order:
#   step <job> <gate|upload|-> <if> <path> <if-no-files-found> <retention-days>
# `path` is the whole value, block scalar continuation lines included: the four
# globs are one input, and reading only the `path: |` line would see no glob at
# all. `gate` is a step whose body mentions build/ci-package-gate.sh, wherever
# the command sits in it.
records() {
    awk '
        function value(line) { sub(/^ *[^:]*:[ \t]*/, "", line); return line }
        function strip(line) { sub(/^ +/, "", line); return line }
        function flush() {
            if (kind != "" || body ~ /build\/ci-package-gate\.sh/) {
                printf "step\t%s\t%s\t%s\t%s\t%s\t%s\n", job,
                       (body ~ /build\/ci-package-gate\.sh/ ? "gate" : (kind == "upload" ? "upload" : "-")),
                       ifcond, path, nofiles, retention
            }
            kind = ""; ifcond = ""; path = ""; nofiles = ""; retention = ""; body = ""; inpath = 0
        }
        # A step starts at six spaces and a dash; the first key of a step may
        # itself be `uses:`, which is why there is no next in these two rules.
        # The rule that accumulates the body matches everything, so it has to
        # come after the two that cut a step or a job in half.
        /^      - / { flush(); inpath = 0; body = $0; next }
        /^  [A-Za-z0-9_-]+:[ \t]*$/ { flush(); job = $0; sub(/^ +/, "", job); sub(/:.*$/, "", job); inpath = 0; body = ""; next }
        { if (job != "") body = body "\n" $0 }
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

jobs=$(awk -F '\t' '$3 == "gate" { print $2 }' <<< "$map" | sort -u)
[ -n "$jobs" ] || fail "$workflow no longer runs build/ci-package-gate.sh"

for job in $jobs; do
    # The upload that answers this job's failure: `if: failure()`, nothing that
    # leaves it to the default success(), and no `always()` -- that one uploads
    # on every green run, which the definition of done for issue #669 forbids.
    found=
    while IFS=$'\t' read -r _step _job kind ifcond path nofiles days; do
        [ "$_job" = "$job" ] || continue
        [ "$kind" = upload ] || continue
        case "$ifcond" in
            *failure*) ;;
            *) continue ;;
        esac
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
        # any expression for retention-days and an absent one falls back to the
        # repository setting, so both are an undecided window.
        days=${days%% #*}
        case "$days" in
            '') fail "the failure upload of job '$job' sets no retention-days; decide the window (issue #669)" ;;
            *[!0-9]*) fail "the failure upload of job '$job' sets retention-days to '$days'; this check reads a plain number of days (issue #669)" ;;
        esac
        if [ "$days" -lt 1 ] || [ "$days" -gt "$MAX_DAYS" ]; then
            fail "the failure upload of job '$job' keeps $days days; nothing downloads it, so keep it between 1 and $MAX_DAYS (issue #261, #669)"
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
        echo "  $job: failure() upload of '$staged' tests/*.{diff,out,exp,log}, ${days} days"
    done <<< "$map"
    [ -n "$found" ] \
        || fail "job '$job' runs build/ci-package-gate.sh but has no actions/upload-artifact step with if: failure(); a failing .phpt in it leaves no evidence (issue #669)"
done

echo "a red package cell uploads the failing .phpt diffs and output"