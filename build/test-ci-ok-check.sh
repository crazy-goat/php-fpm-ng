#!/usr/bin/env bash
# build/ci-ok-check.sh, the verdict of the required `ci-ok` check, in every outcome that
# matters (issue #781), plus a drift guard between its call in build-matrix.yml and the
# `needs:` list of the same job. Hermetic: no network, no build, no YAML parser.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

check=build/ci-ok-check.sh
workflow=.github/workflows/build-matrix.yml
failed=0

# expect <ok|red> <description> <args to ci-ok-check.sh>...
expect() {
    local want=$1 what=$2 rc
    shift 2
    "$check" "$@" >/dev/null 2>&1
    rc=$?
    if { [ "$want" = ok ] && [ "$rc" -ne 0 ]; } || { [ "$want" = red ] && [ "$rc" -eq 0 ]; }; then
        echo "FAIL: $what: wanted $want, exit code $rc" >&2
        failed=1
    else
        echo "PASS: $what"
    fi
}

heavy_ok=(lint=success checks=success build=success)
heavy_skipped=(lint=skipped checks=skipped build=skipped)

expect ok  "code change, everything green" true changes=success docs=success "${heavy_ok[@]}"
expect ok  "docs-only change, heavy jobs skipped by design" false changes=success docs=success "${heavy_skipped[@]}"
expect ok  "code=false, heavy jobs green anyway" false changes=success docs=success "${heavy_ok[@]}"
expect red "code change, one job skipped" true changes=success docs=success lint=skipped checks=success build=success
expect red "code change, one job failed" true changes=success docs=success lint=success checks=failure build=success
expect red "docs-only change, a heavy job failed" false changes=success docs=success lint=failure checks=skipped build=skipped
expect red "a heavy job was cancelled" false changes=success docs=success lint=cancelled checks=skipped build=skipped
expect red "run 37365615843: changes and docs cancelled, code empty, rest skipped" "" changes=cancelled docs=cancelled "${heavy_skipped[@]}"
expect red "changes cancelled, code empty, docs green" "" changes=cancelled docs=success "${heavy_skipped[@]}"
expect red "changes cancelled but code=false was somehow set" false changes=cancelled docs=success "${heavy_skipped[@]}"
expect red "changes skipped, code=false" false changes=skipped docs=success "${heavy_skipped[@]}"
expect red "changes failed" false changes=failure docs=success "${heavy_skipped[@]}"
expect red "docs cancelled, code=false" false changes=success docs=cancelled "${heavy_skipped[@]}"
expect red "docs failed" false changes=success docs=failure "${heavy_skipped[@]}"
expect red "docs skipped" false changes=success docs=skipped "${heavy_skipped[@]}"
expect red "code empty, everything else green" "" changes=success docs=success "${heavy_ok[@]}"
expect red "code is a stray value" maybe changes=success docs=success "${heavy_ok[@]}"
expect red "a result is empty" false changes=success docs=success lint= checks=skipped build=skipped
expect red "a result is a status nobody planned for" false changes=success docs=success lint=neutral checks=skipped build=skipped
expect red "no changes result passed" false docs=success "${heavy_skipped[@]}"
expect red "no docs result passed" false changes=success "${heavy_skipped[@]}"
expect red "no arguments at all"
expect red "an argument that is not job=result" false changes=success docs=success lint

# Drift guard: the jobs the workflow gives the script are exactly the jobs ci-ok needs.
needs=$(awk '/^  ci-ok:/ { inj = 1 } inj && /^    needs:/ { gsub(/.*\[|\].*|,/, " "); print; exit }' "$workflow" | tr -s ' ' '\n' | sed '/^$/d' | sort)
passed=$(sed -n '/^  ci-ok:/,$p' "$workflow" | grep -o 'needs\.[a-z-]*\.result' | sed 's/needs\.//; s/\.result//' | sort)
# The label in front of each result is the job it names: a swapped pair would
# mislabel the verdict (issue #781 review, R2).
# the pattern is a literal GitHub expression, not a shell expansion
# shellcheck disable=SC2016
labels=$(sed -n '/^  ci-ok:/,$p' "$workflow" | grep -o '[a-z-]*=\${{ needs\.[a-z-]*\.result }}' |
    sed -E 's/^([a-z-]*)=.*needs\.([a-z-]*)\.result.*/\1 \2/' | awk '$1 == $2 { print $1 }' | sort)
if [ -z "$needs" ] || [ "$needs" != "$passed" ] || [ "$needs" != "$labels" ]; then
    echo "FAIL: ci-ok needs [${needs//$'\n'/ }], passes the result of [${passed//$'\n'/ }], correctly labelled [${labels//$'\n'/ }]" >&2
    failed=1
else
    echo "PASS: ci-ok passes the result of every job it needs, each under its own name"
fi

# The verdict is only reached when ci-ok runs after a cancelled or failed need. A
# skipped ci-ok reports success and does not block a merge (issue #781).
if sed -n '/^  ci-ok:/,$p' "$workflow" | awk 'NR > 1 && /^  [a-z]/ { exit } /^    if: always\(\)$/ { found = 1 } END { exit !found }'; then
    echo "PASS: ci-ok runs with if: always()"
else
    echo "FAIL: the ci-ok job lost 'if: always()': it would be skipped, and a skipped job reports success (issue #781)" >&2
    failed=1
fi

exit "$failed"
