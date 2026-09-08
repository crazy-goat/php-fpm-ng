#!/usr/bin/env bash
# Static enumeration half of the Laravel statics audit (task 025).
#
# The spike hand-picked 12 candidates out of ~237 `static $` declarations in
# vendor/laravel/framework. This script makes the enumeration reproducible:
# it lists every static *property* declaration (the ~215 real properties;
# `static $` inside function bodies is local state, not class state, and is
# excluded) so the search space is on record rather than remembered.
#
# The list itself cannot say which properties hold per-request state — that
# is what the runtime audit (bin/run.sh audit mode, /statics-audit) measures.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
VENDOR="$ROOT/vendor/laravel/framework/src/Illuminate"
OUT=${1:-/dev/stdout}

if [ ! -d "$VENDOR" ]; then
    echo "vendor/laravel/framework is missing; run bin/provision.sh first" >&2
    exit 2
fi

{
    printf 'Laravel framework: '
    grep -A1 '"name": "laravel/framework"' "$ROOT/composer.lock" | grep -m1 '"version"'
    echo
    grep -rnoE '(public|protected|private)[[:space:]]+(static|static readonly)[[:space:]]+\$[A-Za-z_]+' \
        "$VENDOR" --include='*.php' \
        | sed "s#$VENDOR/##" \
        | sort
} > "$OUT"

count=$(grep -cE 'static( readonly)?[[:space:]]+\$' "$OUT" || true)
echo "static property declarations: $count (full list in $OUT)" >&2
