#!/bin/sh
# The build output directory stays out of git (issue #847).
#
# build/libphp-build.sh writes its objects, dependency files, the compat header
# and the binary into the directory given on the command line, and AGENTS.md
# calls that directory `out/`. It is regenerated on every build, so tracking it
# makes every worktree dirty after every build, lets a `git add -A` commit
# object files and the binary into a pull request, and inflated the repository
# by ~78k lines when 372 files under out/ landed on main (squash 140ed13, #843).
#
# Hermetic: git only, no build, no binary.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"

fail() { echo "FAIL: $*" >&2; exit 1; }

tracked=$(git ls-files -- 'out/*' 'out/**')
if [ -n "$tracked" ]; then
    echo "$tracked" | sed 's/^/  tracked build output: /' >&2
    fail "$(printf '%s\n' "$tracked" | wc -l | tr -d ' ') file(s) under out/ are tracked; run 'git rm -r --cached out' and keep them ignored"
fi
echo "ok: no tracked file under out/"

# The directory must be ignored by name, not only absent from the index: without
# the rule the next `git add -A` in a worktree puts it straight back. The probe
# is a path only the out/ rule can match -- `php-fpm-*` already ignores the
# binary by name, so it would pass even with no out/ entry.
if ! git check-ignore -q -- 'out/obj/some_object.o'; then
    fail ".gitignore does not ignore out/ (a build would show up as untracked)"
fi
echo "ok: out/ is ignored"

echo "all tracked-build-output checks passed"
