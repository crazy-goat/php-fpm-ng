#!/bin/sh
# Regression test for issue #813:
# The async executor was deleted (#623) and pool.executor = async is refused.
# Prevent docs and configs from re-introducing 'executor = async' mentions
# outside of historical records.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"

# Allowlisted paths that legitimately record history or test the refusal:
# - patches/ (historical patches)
# - docs/task-archive.md (completed task archive)
# - docs/NOTES.md (historical notes)
# - sapi/fpmng/config.m4 (reserved configure flag help string)
# - sapi/fpmng/tests/fpmng-http-direct-config.phpt (test asserting refusal)
# - build/test-executor-async-rule.sh (this test script)

if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    hits=$(git grep -nE 'executor[[:space:]]*=[[:space:]]*async' -- \
        ':(exclude)patches' \
        ':(exclude)docs/task-archive.md' \
        ':(exclude)docs/NOTES.md' \
        ':(exclude)sapi/fpmng/config.m4' \
        ':(exclude)sapi/fpmng/tests/fpmng-http-direct-config.phpt' \
        ':(exclude)build/test-executor-async-rule.sh' || true)
else
    hits=$(grep -rnE 'executor[[:space:]]*=[[:space:]]*async' . \
        --exclude-dir='.git' \
        --exclude-dir='patches' \
        --exclude='task-archive.md' \
        --exclude='NOTES.md' \
        --exclude='config.m4' \
        --exclude='fpmng-http-direct-config.phpt' \
        --exclude='test-executor-async-rule.sh' || true)
fi

if [ -n "$hits" ]; then
    printf '%s\n' "$hits" >&2
    echo "FAIL: 'executor = async' found outside allowlisted history paths (file and line above)" >&2
    exit 1
fi

echo "PASS: no 'executor = async' found outside allowlisted history notes"
