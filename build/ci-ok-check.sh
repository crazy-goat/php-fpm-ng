#!/usr/bin/env bash
# The verdict of the `ci-ok` job in .github/workflows/build-matrix.yml (issue #781).
#
# usage: ci-ok-check.sh <code> <job>=<result>...
#
# <code> is needs.changes.outputs.code, each <job>=<result> pair is a needs.<job>.result.
# It fails closed: only a known-good value passes, so a value nobody planned for
# (empty, "cancelled", a new GitHub status) is red.
#
# Why: in run 37365615843 the reusable-workflow jobs `changes` and `docs` were cancelled
# after 15 minutes in the queue, every heavy job was skipped because it needs `changes`,
# `code` was empty, and the old blacklist loop (failure|cancelled) still ended green on a
# PR that nothing had built.
#
#   - `changes` and `docs` must be `success`: they are never skipped by design, and
#     `code` comes from `changes`.
#   - `code` must be exactly `true` or `false`.
#   - every other result must be `success` or `skipped`.
#   - `skipped` is allowed only when `code` is `false` (docs-only change).
set -uo pipefail

fail() { echo "ci-ok: FAIL: $*" >&2; exit 1; }

[ "$#" -ge 1 ] || fail "usage: $0 <code> <job>=<result>..."
code=$1
shift

echo "Code changes: '$code'"
echo "Results: $*"

case "$code" in
    true | false) ;;
    *) fail "needs.changes.outputs.code is '$code', expected true or false" ;;
esac

seen_changes=0
seen_docs=0
for pair in "$@"; do
    case "$pair" in
        *=*) ;;
        *) fail "argument '$pair' is not <job>=<result>" ;;
    esac
    job=${pair%%=*}
    result=${pair#*=}
    [ "$job" = changes ] && seen_changes=1
    [ "$job" = docs ] && seen_docs=1
    case "$result" in
        success) ;;
        skipped)
            [ "$code" = false ] || fail "$job was skipped but code is '$code': a code change needs every job green"
            case "$job" in
                changes | docs) fail "$job must run, it was skipped" ;;
            esac
            ;;
        *) fail "$job result is '$result'" ;;
    esac
done

[ "$seen_changes" -eq 1 ] || fail "no result for the changes job"
[ "$seen_docs" -eq 1 ] || fail "no result for the docs job"
echo "ci-ok: ok"
