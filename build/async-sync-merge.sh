#!/bin/sh
# Issue #520: the merge step of the weekly async-sync workflow
# (.github/workflows/async-sync.yml), which runs it in the merge job; this
# script also pushes the scratch ref. It lives in a script, not inline in
# the workflow, so build/test-async-sync-guard.sh exercises the exact code
# the workflow runs.
#
# Why a clean merge is not enough: Git resolves "deleted on main, unchanged
# on async" by deleting the path, with no conflict. Main's #373 cut
# (f982f37) deleted 46 fiber/async paths that both branches had inherited
# from b99db87 and that async had not touched since, so a plain
# `git merge origin/main` on async deleted every one of them and exited 0.
#
# The rule: a merge that removes ANY path present in async's pre-merge tree is
# not published. A narrower rule ("only paths async added since the merge
# base") would never fire. A clean merge can only drop a path when async left
# it unchanged since the base and main deleted it, so the path existed at the
# base on both sides. That is exactly the #373 case, and it is shaped exactly
# like a legitimate deletion (main dropping patches/0006 in #420). Git history
# cannot tell the two apart; only a person can. A legitimate deletion
# is let through by naming it in <allow-file> (workflow_dispatch input
# allow_deletions), or by merging by hand as was done for #538.
#
# Usage: async-sync-merge.sh <remote> <upstream-branch> <scratch-ref> <report-file> [<allow-file>]
#   Run from a checkout of async. Fetches <remote>/<upstream-branch>, merges it
#   into HEAD and, only if the merge is clean AND deletes nothing that is not
#   listed in <allow-file> (one path per line), force-pushes HEAD to
#   refs/heads/<scratch-ref> on <remote>. It never pushes async itself.
#
# Exit status:
#   0  clean, published to the scratch ref; HEAD is the merge commit
#   2  merge conflict; merge aborted, HEAD unchanged, nothing pushed
#   3  the merge would delete async paths; they are listed one per line in
#      <report-file>, HEAD reset to its pre-merge commit, nothing pushed
#   1  anything else (bad usage, fetch/push failure)
set -eu

usage() {
	echo "usage: async-sync-merge.sh <remote> <upstream-branch> <scratch-ref> <report-file> [<allow-file>]" >&2
	exit 1
}
[ $# -ge 4 ] && [ $# -le 5 ] || usage
REMOTE=$1
UPSTREAM=$2
SCRATCH=$3
REPORT=$4
ALLOW=${5:-}

if [ -n "$ALLOW" ] && [ ! -f "$ALLOW" ]; then
	echo "async-sync-merge.sh: allow-file $ALLOW does not exist" >&2
	exit 1
fi

git fetch "$REMOTE" "$UPSTREAM"
# Recorded before the merge, and compared against instead of HEAD^1: when
# async has no commits of its own the merge fast-forwards, and HEAD^1 is then
# a main commit, not the async tree being protected.
before=$(git rev-parse HEAD)
: > "$REPORT"

if ! git merge --no-edit "$REMOTE/$UPSTREAM"; then
	git merge --abort
	echo "async-sync-merge.sh: merge of $REMOTE/$UPSTREAM conflicted; aborted" >&2
	exit 2
fi

# --no-renames: a path that main renamed is still gone from async's tree, so
# it is reported under its old name rather than hidden as a rename.
deleted=$(git diff --no-renames --name-only --diff-filter=D "$before" HEAD)
if [ -n "$deleted" ]; then
	if [ -n "$ALLOW" ]; then
		printf '%s\n' "$deleted" | grep -vxF -f "$ALLOW" > "$REPORT" || true
	else
		printf '%s\n' "$deleted" > "$REPORT"
	fi
	if [ -s "$REPORT" ]; then
		git reset -q --hard "$before"
		echo "async-sync-merge.sh: merge would delete $(wc -l < "$REPORT" | tr -d ' ') path(s) present on async; not published:" >&2
		cat "$REPORT" >&2
		exit 3
	fi
	echo "async-sync-merge.sh: deletions all listed in $ALLOW; publishing"
fi

git push --force "$REMOTE" "HEAD:refs/heads/$SCRATCH"
