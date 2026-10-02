#!/usr/bin/env bash
# Run all static analysis, linters and formatter checks. --fix applies fixes first.
#
# clang-tidy needs the compile flags of a real build: set FPMNG_LIBPHP_OUT to the
# output directory of build/libphp-build.sh (Linux, PHP 8.5 SDK). The build is a
# setup step, so this script does not run it.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

FIX=0
[ "${1:-}" = "--fix" ] && FIX=1
failed=()

step() {
    local name="$1"; shift
    echo "==> $name"
    "$@" || failed+=("$name")
}

# A missing tool is a failure, not a skip.
need() {
    command -v "$1" >/dev/null 2>&1 || { echo "$1 not found in PATH" >&2; return 1; }
}

# Our own C sources that clang-format checks: everything tracked under sapi/fpmng and
# ext/fpmng_metrics except the paths listed in build/clang-format-exclude.txt.
format_files() {
    git ls-files -z -- 'sapi/fpmng/*.c' 'sapi/fpmng/*.h' 'ext/fpmng_metrics/*.c' 'ext/fpmng_metrics/*.h' \
        | grep -zvxFf <(grep -v '^#' build/clang-format-exclude.txt | grep -v '^$')
}

# Tracked shell scripts: *.sh, plus extensionless files with a shell shebang.
# third_party/ is vendored.
shell_files() {
    local f
    git ls-files -z | while IFS= read -r -d '' f; do
        case "$f" in third_party/*) continue ;; esac
        [ -f "$f" ] && [ ! -L "$f" ] || continue
        case "$f" in
            *.sh) printf '%s\0' "$f" ;;
            *.*) ;;
            *) head -n 1 "$f" | grep -Eq '^#!.*\b(sh|bash|dash|ksh)\b' && printf '%s\0' "$f" ;;
        esac
    done
}

dockerfiles() {
    git ls-files -z | grep -zE '(^|/)(Dockerfile|Dockerfile\.[^/]+|[^/]+\.Dockerfile)$' | grep -zv '\.dockerignore$'
}

clang_tidy() {
    if [ -z "${FPMNG_LIBPHP_OUT:-}" ]; then
        echo "FPMNG_LIBPHP_OUT is not set: build with build/libphp-build.sh <dir> and pass <dir>" >&2
        return 1
    fi
    need clang-tidy || return 1
    build/lint-c.sh "$FPMNG_LIBPHP_OUT"
}

clang_format_check() {
    need clang-format || return 1
    format_files | xargs -0 -r clang-format --dry-run --Werror
}

check_shellcheck() {
    need shellcheck || return 1
    # Warnings and errors; the style and info notes in build/ and tests/ are a follow-up.
    shell_files | xargs -0 -r shellcheck -S warning
}

check_hadolint() {
    need hadolint || return 1
    dockerfiles | xargs -0 -r hadolint
}

if [ "$FIX" = 1 ]; then
    need clang-format && format_files | xargs -0 -r clang-format -i
fi

step "clang-tidy" clang_tidy
step "clang-format" clang_format_check
step "shellcheck" check_shellcheck
step "hadolint" check_hadolint

if [ "${#failed[@]}" -gt 0 ]; then
    echo "Failed: ${failed[*]}" >&2
    exit 1
fi
echo "All checks passed."
