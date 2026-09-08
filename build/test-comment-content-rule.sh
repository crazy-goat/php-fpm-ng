#!/bin/sh
# Regression test for task 014: the comment-content contract lives in
# workflow.md and is linked from the contributor entry points.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORKFLOW="$REPO/workflow.md"
ROOT_README="$REPO/README.md"
FPMNG_README="$REPO/sapi/fpmng/README.md"

test -f "$WORKFLOW"
test -f "$ROOT_README"
test -f "$FPMNG_README"

grep -q '## Comments: what earns one' "$WORKFLOW"
grep -q '\*\*Keep\*\*' "$WORKFLOW"
grep -q '\*\*Delete\*\*' "$WORKFLOW"
grep -q '\*\*Never\*\*' "$WORKFLOW"
grep -q 'no scheduled comment-cleanup pass' "$WORKFLOW"
grep -q 'opportunistically' "$WORKFLOW"

grep -q 'workflow.md#comments-what-earns-one' "$ROOT_README"
grep -q 'workflow.md#comments-what-earns-one' "$FPMNG_README"
