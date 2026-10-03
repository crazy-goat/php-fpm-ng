#!/usr/bin/env bash
# Check the toolchain of a new worktree. Called by bin/worktree.sh.
#
# The project has no package dependencies to install: build/libphp-build.sh uses
# the distribution's PHP 8.5 SDK, and third_party/php-src is vendored. What a
# worktree needs is the right tools on PATH, so this reports what is missing and
# how to get it. It only warns; bin/lint.sh is the place where a missing tool
# fails.
set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 1

missing=0
need() {
  local tool="$1" hint="$2"
  if command -v "$tool" >/dev/null 2>&1; then
    echo "ok       $tool"
  else
    echo "MISSING  $tool -- $hint"
    missing=1
  fi
}

echo "Lint tools (bin/lint.sh):"
need clang-format "pipx install clang-format==23.1.2"
need clang-tidy "apt install clang-tidy (or brew install llvm)"
need shellcheck "apt install shellcheck (CI pins v0.11.0)"
need hadolint "https://github.com/hadolint/hadolint/releases (CI pins v2.12.0)"

echo
echo "Build tools (build/libphp-build.sh, Linux only):"
if [ "$(uname -s)" = Linux ]; then
  need php-config8.5 "sudo build/ci-install-deps.sh build (Ubuntu 26.04) installs the PHP 8.5 SDK"
  need php8.5 "sudo build/ci-install-deps.sh test"
else
  echo "skipped  $(uname -s) cannot run build/libphp-build.sh; build on Linux or in a container (see AGENTS.md)"
fi

if [ "$missing" = 1 ]; then
  echo
  echo "Some tools are missing; the checks that need them will fail until they are installed."
fi
exit 0
