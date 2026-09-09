#!/bin/sh
# Fails when a build log contains a compiler diagnostic in a file THIS
# repository owns (issue #111).
#
#   build/check-owned-warnings.sh /path/to/make.log
#
# The build job cannot use -Werror: it compiles php-src, and upstream's own
# warnings are not ours to fix. Nor is the path a statement of ownership --
# build/prepare.sh copies ALL of upstream's sapi/fpm/ into sapi/fpmng/ and then
# overlays this repo's files on top, so of the 57 .c files under
# sapi/fpmng/fpm/ in a prepared tree only 37 are ours; sapi/fpmng/fpm/
# fpm_signals.c, fpm_sockets.c, fpm_main.c and 17 others are upstream's files
# sitting under our directory (measured 2026-09-09 against php-8.5.9). The file
# list therefore comes from git, from this repo, exactly like build/lint-c.sh
# does it: a warning counts only if the file it names is one we ship.
#
# Exit status: 0 when no owned file produced a diagnostic, 1 when one did (the
# offending lines are printed), 2 on a usage or environment error -- including
# a log that contains no compile of an owned file at all, which would otherwise
# report success for having checked nothing.
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LOG="${1:-}"

[ -n "$LOG" ] || { echo "usage: $0 /path/to/make.log" >&2; exit 2; }
[ -f "$LOG" ] || { echo "$0: no such build log: $LOG" >&2; exit 2; }

OWNED_LIST=$(mktemp)
MATCHED=$(mktemp)
trap 'rm -f "$OWNED_LIST" "$MATCHED"' EXIT

# Compiled sources and their headers only. A path is matched as a suffix,
# because the log names files inside the prepared php-src tree.
( cd "$REPO" && git ls-files 'sapi/fpmng' 'ext/fpmng_metrics' 2>/dev/null ) \
  | grep -E '\.(c|h)$' > "$OWNED_LIST" || true

if [ ! -s "$OWNED_LIST" ]; then
    echo "$0: could not list owned sources (not a git checkout?)" >&2
    exit 2
fi

# A green result must mean "checked and clean", not "checked nothing". If the
# log does not even mention an owned source file, the build did not compile
# them (wrong target, truncated log, or a `tee` destination that moved) and
# saying "0 diagnostics" would be a lie that hides the broken gate.
if ! grep -F -q -f "$OWNED_LIST" "$LOG"; then
    echo "$0: $LOG mentions no file this repo owns -- nothing was checked." >&2
    echo "$0: the build step's log path and this script's argument have to" >&2
    echo "$0: name the same file. Refusing to report success." >&2
    exit 2
fi

# "warning:" and "error:" both, so a diagnostic that only appears with a
# different compiler cannot slip through as "not a warning". Two location
# forms: gcc/clang's usual `file:line:col:` and the file-scope `file: warning:`
# gcc emits for diagnostics it cannot pin to a line (-Wunused-but-set at link
# time, for instance) -- the second was missing and would have passed silently.
grep -E ':[0-9]+:[0-9]+: (warning|error):|\.(c|h): (warning|error):' "$LOG" \
  | grep -F -f "$OWNED_LIST" > "$MATCHED" || true

if [ -s "$MATCHED" ]; then
    COUNT=$(wc -l < "$MATCHED" | tr -d ' ')
    echo "check-owned-warnings.sh: $COUNT diagnostic(s) in files this repo owns:"
    sed 's/^/    /' "$MATCHED"
    echo "check-owned-warnings.sh: fix them, or state in the PR why the"
    echo "warning is wrong and silence it at the site (issue #111)."
    exit 1
fi

echo "check-owned-warnings.sh: 0 diagnostics in $(wc -l < "$OWNED_LIST" | tr -d ' ') owned source files"
