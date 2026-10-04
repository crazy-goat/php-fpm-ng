#!/usr/bin/env bash
# The artifacts the release job downloads must outlive a re-run of that job alone
# (issue #679). Hermetic: reads .github/workflows/release.yml, needs no network,
# no build, no binary and no YAML parser.
#
# The invariant, not the literal: every actions/download-artifact step of the
# `release` job matches an upload of the same workflow whose retention is at
# least MIN_DAYS. `release` is the one job in the workflow a maintainer can
# re-run on its own days after the run that produced its inputs -- the other
# jobs' inputs are a handoff inside one run, which is why the one day of
# fpmng-canonical-build is right in build-matrix.yml and wrong here. So adding a
# download to `release` without deciding its retention fails this, and lowering
# one of the two below MIN_DAYS fails it too. Nothing here proves the workflow
# works: it keeps the window the comments at the two uploads and above the
# publish step name from being closed silently.
#
# MIN_DAYS is the re-run window written in those comments. Change it here and in
# .github/workflows/release.yml together.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

MIN_DAYS=7
workflow=.github/workflows/release.yml

fail() { echo "FAIL: $*" >&2; exit 1; }

# One TAB-separated record per artifact step of a workflow, in file order:
#   upload   <job> <name> <retention-days>
#   download <job> <name-or-pattern>
# A matrix expression in an upload name becomes `*`, which is how the release
# job's download pattern spells the same artifact.
records() {
    awk '
        function flush() {
            if (action == "upload" && name != "") printf "upload\t%s\t%s\t%s\n", job, name, retention
            else if (action == "download" && (name != "" || pattern != "")) printf "download\t%s\t%s\n", job, (name != "" ? name : pattern)
            action = ""; name = ""; pattern = ""; retention = ""; inwith = 0
        }
        BEGIN { stepuse = "(^      - |^        )uses: actions[/]" }
        # No `next`: a step whose first key is `uses:` has to reach the rule below.
        /^      - / { flush() }
        /^  [A-Za-z0-9_-]+:[ \t]*$/ { flush(); job = $0; sub(/^ +/, "", job); sub(/:.*$/, "", job) }
        $0 ~ stepuse {
            if (index($0, "actions/upload-artifact@")) action = "upload"
            else if (index($0, "actions/download-artifact@")) action = "download"
            next
        }
        /^        with:[ \t]*$/ { inwith = (action != ""); next }
        inwith && /^          [A-Za-z0-9_.-]+:/ {
            k = $0; sub(/^ +/, "", k); sub(/:.*$/, "", k)
            v = $0; sub(/^ +[^:]*:[ \t]*/, "", v)
            if (k == "name") name = v
            else if (k == "pattern") pattern = v
            else if (k == "retention-days") retention = v
            next
        }
        END { flush() }
    ' "$1" | sed 's/\${{ *matrix\.cell *}}/*/'
}

[ -f "$workflow" ] || fail "$workflow not found"
map=$(records "$workflow")
[ -n "$map" ] || fail "$workflow has no artifact step to check"

# The retention-days of the upload whose name is the one the download step
# asks for. Textual, not a glob: records() has already replaced a matrix
# expression in the upload name with `*`, so `package-${{ matrix.cell }}` and the
# `package-*` pattern of the download step meet here as the same string.
# Renaming one side without the other therefore fails, which is the point --
# renaming an artifact the release job downloads is a decision, not a detail.
retention_of() {
    local want="$1" kind job name days
    while IFS=$'\t' read -r kind job name days; do
        [ "$kind" = upload ] || continue
        if [ "$want" = "$name" ]; then printf '%s' "$days"; return 0; fi
    done <<< "$map"
    return 1
}

checked=0
while IFS=$'\t' read -r kind job name days; do
    [ "$kind" = download ] || continue
    [ "$job" = release ] || continue
    if ! days=$(retention_of "$name"); then
        fail "the release job downloads '$name', which no job of $workflow uploads"
    fi
    # A plain YAML scalar ends at an unquoted ` #`, so a value that carries a
    # comment is still that many days (`retention-days: 7 # a week` is 7) and
    # must not be called a non-literal.
    days=${days%% #*}
    # `upload-artifact` takes any expression for retention-days, and
    # `[ "$days" -lt 7 ]` answers 2 for one, which a `||` would read as "no
    # problem": a non-literal has to fail here, not sail through. No
    # retention-days at all is the same kind of undecided window, so it fails
    # too, loudly, instead of taking the action's own default.
    case "$days" in
        '') fail "the release job downloads '$name', whose upload sets no retention-days; decide the re-run window there (issue #679)" ;;
        *[!0-9]*) fail "the release job downloads '$name', whose upload sets retention-days to '$days'; this check reads a plain number of days (issue #679)" ;;
    esac
    if [ "$days" -lt "$MIN_DAYS" ]; then
        fail "the release job downloads '$name', whose upload keeps $days days; re-running only the release job must work for $MIN_DAYS days (issue #679)"
    fi
    echo "  $name: ${days} days"
    checked=$((checked + 1))
done <<< "$map"

[ "$checked" -gt 0 ] || fail "the release job downloads nothing: $workflow changed shape"
echo "release artifacts consumed by a re-runnable release job keep at least $MIN_DAYS days"
