#!/bin/sh
# Regression test for task 014: the comment-content contract lives in
# CLAUDE.md and is linked from the contributor entry points.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
CLAUDE="$REPO/CLAUDE.md"
TASKS_README="$REPO/tasks/README.md"
FPMNG_README="$REPO/sapi/fpmng/README.md"

test -f "$CLAUDE"
test -f "$TASKS_README"
test -f "$FPMNG_README"

grep -q '## Comments: what earns one' "$CLAUDE"
grep -q '\*\*Keep\*\*' "$CLAUDE"
grep -q '\*\*Delete\*\*' "$CLAUDE"
grep -q '\*\*Never\*\*' "$CLAUDE"
grep -q 'no scheduled comment-cleanup pass' "$CLAUDE"
grep -q 'opportunistically' "$CLAUDE"

grep -q 'CLAUDE.md#comments-what-earns-one' "$TASKS_README"
grep -q 'CLAUDE.md#comments-what-earns-one' "$FPMNG_README"
