#!/bin/sh
# Print the license text a php-fpm-ng package ships (issue #581).
#
# MIT, the PHP License 3.01 and BSD-2-Clause each make the notice a condition of
# redistributing a binary, and the package is a binary redistribution. One file
# carries all three: the project's LICENSE (which also lists which files are not
# MIT), the vendored php-src LICENSE, and the FPM BSD-2-Clause text. Both
# package scripts install this output (deb as /usr/share/doc/<pkg>/copyright,
# apk under /usr/share/licenses/<pkg>/), and build/ci-package-gate.sh compares
# the installed file with a fresh run of this script, so a package that lost
# one of the texts fails the gate.
#
# Usage: build/package-licenses.sh > file
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)

emit() {
    [ -s "$2" ] || { echo "package-licenses.sh: FAIL: $2 is missing or empty" >&2; exit 1; }
    printf '%s\n\n' "=== $1 ==="
    cat "$2"
    printf '\n\n'
}

emit "MIT License (php-fpm-ng; the list at the end names the files that are not MIT)" "$REPO/LICENSE"
emit "The PHP License, version 3.01 (code taken from or derived from php-src)" "$REPO/third_party/php-src/LICENSE"
emit "BSD-2-Clause (the original FPM code, Andrei Nigmatulin)" "$REPO/third_party/php-src/sapi/fpm/LICENSE"
