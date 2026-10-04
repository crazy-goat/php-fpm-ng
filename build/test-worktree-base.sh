#!/usr/bin/env bash
# Issue #709: bin/worktree.sh --base, the branch a worktree is cut from.
#
# The option exists because a repository can carry a long-lived line the default
# branch has no trace of; here that line is `async`, and `main` has none of its 28
# fpm_pool_fiber*/fpm_pool_coop* sources, none of its 9 fiber tests and no
# async-fiber.yml (git ls-tree against origin/async and origin/main). The scenarios
# below drive the real bin/worktree.sh, never a reimplementation of it, against a
# throwaway bare remote: a stub `gh` answers the two calls the script makes (the
# default branch name and the issue), and jq reads the issue JSON.
#
# Hermetic: git, bash, jq and coreutils only. No network, no PHP, no build. The
# remote is a local bare repository in the temp dir, and nothing is pushed to it
# after the fixture is built.
#
# Usage: build/test-worktree-base.sh
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/fpmng-worktree-base.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM

# Hermetic git, as in build/test-async-sync-guard.sh: the developer's global config
# (init.defaultBranch, hooks, merge drivers) must not decide what is measured.
export GIT_CONFIG_GLOBAL=/dev/null
export GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.invalid
export GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.invalid

fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { echo "ok: $*"; }

# --- the throwaway remote ----------------------------------------------------
# main and async carry the same file with different content, plus a file that only
# async has. That makes "which branch did this worktree come from" answerable from
# the worktree itself and not only from git's own output.
REMOTE="$WORK/remote.git"
SEED="$WORK/seed"
git init -q --bare -b main "$REMOTE"
git init -q -b main "$SEED"
echo main >"$SEED/line.txt"
git -C "$SEED" add -A
git -C "$SEED" commit -q -m base
git -C "$SEED" checkout -q -b async
echo async >"$SEED/line.txt"
echo 'fpm_pool_fiber' >"$SEED/only-on-async.c"
git -C "$SEED" add -A
git -C "$SEED" commit -q -m async-only
git -C "$SEED" checkout -q main
git -C "$SEED" push -q "$REMOTE" main async
MAIN_SHA=$(git -C "$REMOTE" rev-parse refs/heads/main)
ASYNC_SHA=$(git -C "$REMOTE" rev-parse refs/heads/async)
[ "$MAIN_SHA" != "$ASYNC_SHA" ] || fail 'the fixture branches are the same commit'

# --- a stub gh ---------------------------------------------------------------
# bin/worktree.sh asks gh for the default branch and for the issue. Answering both
# keeps the scenario offline and keeps the real script in charge of everything else.
STUB="$WORK/stub-bin"
mkdir -p "$STUB"
cat >"$STUB/gh" <<'STUBEOF'
#!/usr/bin/env bash
case "$1 $2" in
  "repo view") echo main ;;
  "issue view") echo '{"title":"worktree base scenario","labels":[{"name":"type:chore"}]}' ;;
  *) echo "stub gh: unexpected call: $*" >&2; exit 1 ;;
esac
STUBEOF
chmod +x "$STUB/gh"

# run <clone> <issue> [worktree.sh args...]: runs the real script inside <clone>.
# Sets $rc and $LOG.
run() {
	_clone=$1
	_issue=$2
	shift 2
	LOG="$WORK/log-$_issue-$RANDOM.txt"
	rc=0
	(
		cd "$_clone"
		PATH="$STUB:$PATH" "$REPO/bin/worktree.sh" --dir "$WORK/wt-$_issue" "$@" "$_issue"
	) >"$LOG" 2>&1 || rc=$?
}

# --- 1. without --base the worktree comes from the default branch -------------
# The option must not change the old behaviour, so this is asserted on the tree the
# worktree holds and not only on the line the script prints.
git clone -q "$REMOTE" "$WORK/plain"
[ -z "$(git -C "$WORK/plain" config --get-all remote.origin.fetch)" ] ||
	[ "$(git -C "$WORK/plain" config --get-all remote.origin.fetch)" = '+refs/heads/*:refs/remotes/origin/*' ] ||
	fail 'the fixture clone is not a default-refspec clone, the test would prove nothing'
run "$WORK/plain" 709
[ "$rc" = 0 ] || { cat "$LOG" >&2; fail "no --base exited $rc, expected 0"; }
[ "$(git -C "$WORK/wt-709" rev-parse HEAD)" = "$MAIN_SHA" ] ||
	fail 'without --base the worktree is not at the default branch tip'
[ "$(cat "$WORK/wt-709/line.txt")" = main ] ||
	fail 'without --base the worktree does not hold the default branch content'
[ ! -e "$WORK/wt-709/only-on-async.c" ] ||
	fail 'without --base the worktree holds a file that is only on the other branch'
grep -qx "Base:     origin/main" "$LOG" || { cat "$LOG" >&2; fail 'the summary does not name the base'; }
ok 'without --base the worktree is cut from the default branch'

# --- 2. --base <branch> cuts from that branch --------------------------------
run "$WORK/plain" 710 --base async
[ "$rc" = 0 ] || { cat "$LOG" >&2; fail "--base async exited $rc, expected 0"; }
[ "$(git -C "$WORK/wt-710" rev-parse HEAD)" = "$ASYNC_SHA" ] ||
	fail '--base async did not cut from the async tip'
[ "$(cat "$WORK/wt-710/line.txt")" = async ] ||
	fail 'the --base async worktree does not hold the async content'
[ -e "$WORK/wt-710/only-on-async.c" ] ||
	fail 'the --base async worktree lacks the file that is only on async'
grep -qx "Base:     origin/async" "$LOG" || { cat "$LOG" >&2; fail 'the summary does not name async as the base'; }
ok '--base async cuts from async and the worktree holds what is on it'

# --- 3. a base that is not on the remote is refused, nothing is created -------
before=$(git -C "$WORK/plain" worktree list | wc -l | tr -d ' ')
run "$WORK/plain" 711 --base no-such-branch
[ "$rc" != 0 ] || fail 'a base that is not on the remote was accepted'
grep -q 'base branch does not exist on origin: no-such-branch' "$LOG" ||
	{ cat "$LOG" >&2; fail 'no "does not exist" diagnosis for a base that is not on the remote'; }
[ ! -e "$WORK/wt-711" ] || fail 'the refused run created the worktree directory'
[ "$(git -C "$WORK/plain" worktree list | wc -l | tr -d ' ')" = "$before" ] ||
	fail 'the refused run registered a worktree'
[ -z "$(git -C "$WORK/plain" branch --list '*issue-711*')" ] ||
	fail 'the refused run created a branch'
ok 'a base that is not on the remote is refused and creates nothing'

# --- 3b. a base git would not accept as a branch name is refused -------------
# The existence check is an ls-remote pattern, and git matches those as globs: "as*"
# matches refs/heads/async, so before check-ref-format the run got past the check,
# wrote refs/remotes/origin/async and only then died in the fetch. Nothing may happen
# here, not even a mutated ref, so the ref list is compared instead of a directory.
before=$(git -C "$WORK/plain" for-each-ref --format='%(refname) %(objectname)' | sort)
run "$WORK/plain" 714 --base 'as*'
[ "$rc" != 0 ] || fail 'a glob base was accepted'
grep -q 'not a branch name: as\*' "$LOG" ||
	{ cat "$LOG" >&2; fail 'no "not a branch name" diagnosis for a glob base'; }
[ ! -e "$WORK/wt-714" ] || fail 'the refused run created the worktree directory'
[ "$(git -C "$WORK/plain" for-each-ref --format='%(refname) %(objectname)' | sort)" = "$before" ] ||
	fail 'the refused run changed a ref in the clone'
[ -z "$(git -C "$WORK/plain" branch --list '*issue-714*')" ] ||
	fail 'the refused run created a branch'
ok 'a glob base is refused before anything is created'

# --- 4. --base in a clone whose refspec is narrowed (issue #709, point 1) -----
# `git clone --single-branch` narrows remote.origin.fetch to the one branch, and then
# a plain `git fetch origin async` writes FETCH_HEAD alone: origin/async stays
# unresolvable and the round-1 script died with "fatal: invalid reference:
# origin/async", the very message the existence check exists to prevent. Asserted
# end to end here so a narrowed clone cannot rot unnoticed.
git clone -q --single-branch --branch main "$REMOTE" "$WORK/single"
[ "$(git -C "$WORK/single" config --get-all remote.origin.fetch)" = '+refs/heads/main:refs/remotes/origin/main' ] ||
	fail 'the fixture is not a single-branch clone, scenario 4 would prove nothing'
git -C "$WORK/single" rev-parse --verify -q origin/async >/dev/null &&
	fail 'origin/async exists before the run, scenario 4 would prove nothing'
run "$WORK/single" 712 --base async
[ "$rc" = 0 ] || { cat "$LOG" >&2; fail "--base async in a single-branch clone exited $rc, expected 0"; }
[ "$(git -C "$WORK/wt-712" rev-parse HEAD)" = "$ASYNC_SHA" ] ||
	fail 'the single-branch clone did not end up at the async tip'
[ -e "$WORK/wt-712/only-on-async.c" ] ||
	fail 'the single-branch worktree lacks the file that is only on async'
ok '--base async works in a clone whose refspec is narrowed to one branch'

# --- 5. an unreachable remote is not reported as a mistyped branch (point 2) --
# git ls-remote --exit-code answers 2 for "no ref matched" and 128 for a remote it
# cannot reach. Both are non-zero, and mapping both to "the branch does not exist"
# sends the operator looking for a typo in a branch that is spelled correctly.
git clone -q "$REMOTE" "$WORK/gone"
mv "$REMOTE" "$WORK/remote-moved"
run "$WORK/gone" 713 --base async
[ "$rc" != 0 ] || fail 'an unreachable remote was accepted'
[ "$rc" != 2 ] || fail 'an unreachable remote is reported with the status of "no ref matched"'
grep -q 'base branch does not exist on origin' "$LOG" &&
	fail 'an unreachable remote is reported as a branch that does not exist'
grep -q 'cannot reach origin to check base branch async' "$LOG" ||
	{ cat "$LOG" >&2; fail 'no "cannot reach origin" diagnosis for an unreachable remote'; }
[ ! -e "$WORK/wt-713" ] || fail 'the run against an unreachable remote created the worktree directory'
ok 'an unreachable remote is reported as such, not as a mistyped branch'

echo 'test-worktree-base.sh: PASS'
