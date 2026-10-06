#!/bin/sh
# Issue #520: regression test for build/async-sync-merge.sh, the merge step of
# .github/workflows/async-sync.yml. Needs git and sh only, no PHP build.
#
# Part 1 replays the real #373 transition: async at be5570a (the first async
# commit, #374) merging main at f982f37 (#373 merged, which deleted the 46
# fiber/async paths). It first shows that a plain `git merge` deletes all
# 46 cleanly, which is the hazard, and then that the script refuses: it exits
# 3, lists exactly those 46 paths, leaves all of them in the checkout, and
# changes neither async nor the scratch ref on the remote. It needs both
# commits in the object store, so the checkout needs full history
# (fetch-depth: 0). Without them the part is skipped, and the run fails
# instead if ASYNC_SYNC_REQUIRE_HISTORY=1 (CI sets it).
#
# Part 2 builds small synthetic repositories for the other outcomes: a clean
# merge is published, a conflict is not, a deletion of a shared file (the
# patches/0006 case from #420) is refused unless allow-listed, a merge that
# fast-forwards is still checked, and a refused scratch push exits 4 with a
# report naming the token it tried (#710).
# shellcheck disable=SC2015 # `cond && ok || bad`: ok only echoes, so bad
# cannot run after a passing check.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT="$REPO/build/async-sync-merge.sh"
ASYNC_BEFORE=be5570a5e8e91baedaa4507901f325e5dbf32010
MAIN_AFTER_373=f982f37866a15db39ef609d22f966682f49ae30d
SCRATCH=_async-sync-scratch

TMP=$(mktemp -d "${TMPDIR:-/tmp}/test-async-sync-guard.XXXXXX")
trap 'rm -rf "$TMP"' EXIT INT TERM

# Hermetic git: the developer's global config (hooks, merge drivers,
# merge.ff=false) must not change what is being measured.
GIT_CONFIG_GLOBAL=/dev/null
GIT_CONFIG_NOSYSTEM=1
GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.invalid
GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.invalid
export GIT_CONFIG_GLOBAL GIT_CONFIG_NOSYSTEM GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL \
	GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL

fail=0
ok() { echo "ok   - $1"; }
bad() { echo "FAIL - $1" >&2; fail=1; }

# The workflow reports the push through this variable name and the script
# reads it; a rename typo on either side would silently fall back to the
# unset default, so pin the shared name.
grep -Fq 'ASYNC_SYNC_PUSH_TOKEN_SOURCE' "$REPO/.github/workflows/async-sync.yml" && ok "workflow references ASYNC_SYNC_PUSH_TOKEN_SOURCE" || bad "workflow no longer references ASYNC_SYNC_PUSH_TOKEN_SOURCE"
grep -Fq 'ASYNC_SYNC_PUSH_TOKEN_SOURCE' "$SCRIPT" && ok "script reads ASYNC_SYNC_PUSH_TOKEN_SOURCE" || bad "script no longer reads ASYNC_SYNC_PUSH_TOKEN_SOURCE"

# run_script <workdir> <args...>: sets $rc
run_script() {
	_wd=$1; shift
	rc=0
	(cd "$_wd" && sh "$SCRIPT" "$@") > "$TMP/script.log" 2>&1 || rc=$?
}

remote_ref() { git -C "$1" rev-parse -q --verify "refs/heads/$2" || true; }

# ---------------------------------------------------------------- part 1
echo "# part 1: replay of the #373 transition"
if git -C "$REPO" cat-file -e "$ASYNC_BEFORE^{commit}" 2>/dev/null &&
   git -C "$REPO" cat-file -e "$MAIN_AFTER_373^{commit}" 2>/dev/null; then
	main_sha=$(git -C "$REPO" rev-parse "$MAIN_AFTER_373^{commit}")
	git -C "$REPO" diff --no-renames --name-only --diff-filter=D \
		"$main_sha^1" "$main_sha" > "$TMP/expected"
	n=$(wc -l < "$TMP/expected" | tr -d ' ')
	[ "$n" = 46 ] && ok "#373 deleted 46 paths on main" || bad "#373 deleted $n paths, expected 46"

	git clone -q --bare --shared "$REPO" "$TMP/origin.git"
	git -C "$TMP/origin.git" update-ref refs/heads/async "$ASYNC_BEFORE"
	git -C "$TMP/origin.git" update-ref refs/heads/main "$main_sha"
	git -C "$TMP/origin.git" update-ref -d "refs/heads/$SCRATCH" 2>/dev/null || true

	# Negative control: the unguarded merge the workflow used to run.
	git clone -q --shared --branch async "$TMP/origin.git" "$TMP/plain"
	git -C "$TMP/plain" fetch -q origin main
	if git -C "$TMP/plain" merge -q --no-edit origin/main >/dev/null 2>&1; then
		git -C "$TMP/plain" diff --no-renames --name-only --diff-filter=D \
			"$ASYNC_BEFORE" HEAD > "$TMP/plain-deleted"
		if cmp -s "$TMP/expected" "$TMP/plain-deleted"; then
			ok "control: plain git merge exits 0 and deletes all 46 paths"
		else
			bad "control: plain git merge deleted a different set than #373"
		fi
	else
		bad "control: plain git merge conflicted; the replay no longer shows the hazard"
	fi

	git clone -q --shared --branch async "$TMP/origin.git" "$TMP/guarded"
	run_script "$TMP/guarded" origin main "$SCRATCH" "$TMP/report"
	[ "$rc" = 3 ] && ok "script exits 3 on the #373 merge" || { bad "script exited $rc, expected 3"; cat "$TMP/script.log" >&2; }
	cmp -s "$TMP/expected" "$TMP/report" && ok "report lists exactly the 46 paths" || bad "report differs from the 46 #373 paths"
	missing=0
	while IFS= read -r p; do
		[ -e "$TMP/guarded/$p" ] || { missing=$((missing + 1)); echo "  missing: $p" >&2; }
	done < "$TMP/expected"
	[ "$missing" = 0 ] && ok "all 46 paths still present in the checkout" || bad "$missing of the 46 paths missing from the checkout"
	[ "$(git -C "$TMP/guarded" rev-parse HEAD)" = "$ASYNC_BEFORE" ] && ok "HEAD reset to async's pre-merge commit" || bad "HEAD is not async's pre-merge commit"
	[ -z "$(git -C "$TMP/guarded" status --porcelain)" ] && ok "checkout is clean" || bad "checkout left dirty"
	[ -z "$(remote_ref "$TMP/origin.git" "$SCRATCH")" ] && ok "scratch ref not created" || bad "scratch ref was pushed"
	[ "$(remote_ref "$TMP/origin.git" async)" = "$ASYNC_BEFORE" ] && ok "async unchanged on the remote" || bad "async changed on the remote"
elif [ "${ASYNC_SYNC_REQUIRE_HISTORY:-0}" = 1 ]; then
	bad "commits $ASYNC_BEFORE / $MAIN_AFTER_373 not in the object store (fetch-depth: 0?)"
else
	echo "skip - #373 commits not in the object store; part 2 still runs"
fi

# ---------------------------------------------------------------- part 2
echo "# part 2: synthetic cases"

# new_case <name>: a bare origin with main and async diverged from a common
# base holding shared.txt, patches/0006.patch and fiber.c. async adds
# async-only.c. Sets $O (origin) and $W (a checkout of async).
new_case() {
	O="$TMP/$1.git"; W="$TMP/$1"; S="$TMP/$1-seed"
	git init -q --bare -b main "$O"
	git init -q -b main "$S"
	mkdir -p "$S/patches"
	echo shared > "$S/shared.txt"
	echo 0006 > "$S/patches/0006.patch"
	echo fiber > "$S/fiber.c"
	git -C "$S" add -A && git -C "$S" commit -q -m base
	git -C "$S" branch async
	git -C "$S" push -q "$O" main async
	git clone -q --branch async "$O" "$W"
}
seed_commit() { git -C "$S" add -A && git -C "$S" commit -q -m "$1"; }

# a. clean merge, nothing deleted: published to the scratch ref only.
new_case clean
git -C "$S" checkout -q async; echo async > "$S/async-only.c"; seed_commit async-adds
git -C "$S" checkout -q main;  echo more >> "$S/shared.txt"; seed_commit main-edits
git -C "$S" push -q "$O" main async
git -C "$W" pull -q --ff-only
before=$(git -C "$W" rev-parse HEAD)
run_script "$W" origin main "$SCRATCH" "$TMP/report"
[ "$rc" = 0 ] && ok "clean merge exits 0" || { bad "clean merge exited $rc"; cat "$TMP/script.log" >&2; }
[ "$(remote_ref "$O" "$SCRATCH")" = "$(git -C "$W" rev-parse HEAD)" ] && ok "clean merge published to the scratch ref" || bad "scratch ref is not the merge commit"
[ "$(remote_ref "$O" async)" = "$before" ] && ok "clean merge leaves async itself alone" || bad "script pushed async"
[ "$(git -C "$W" rev-list --parents -n1 HEAD | wc -w | tr -d ' ')" = 3 ] && ok "clean merge is a real merge commit (two parents)" || bad "clean merge is not a two-parent merge commit"

# b. conflict: aborted, nothing published.
new_case conflict
git -C "$S" checkout -q async; echo A > "$S/shared.txt"; seed_commit async-edit
git -C "$S" checkout -q main;  echo M > "$S/shared.txt"; seed_commit main-edit
git -C "$S" push -q "$O" main async
git -C "$W" pull -q --ff-only
before=$(git -C "$W" rev-parse HEAD)
run_script "$W" origin main "$SCRATCH" "$TMP/report"
[ "$rc" = 2 ] && ok "conflict exits 2" || bad "conflict exited $rc"
[ "$(git -C "$W" rev-parse HEAD)" = "$before" ] && [ -z "$(git -C "$W" status --porcelain)" ] && ok "conflict leaves the checkout at async, clean" || bad "conflict left the checkout changed"
[ -z "$(remote_ref "$O" "$SCRATCH")" ] && ok "conflict publishes nothing" || bad "conflict pushed the scratch ref"
[ "$(cat "$TMP/report")" = shared.txt ] && ok "conflict report lists the conflicted file" || bad "conflict report: $(tr '\n' ' ' < "$TMP/report")"

# c. main deletes two shared files async never touched (0006 and fiber.c):
#    refused and both listed; allow-listing only one still refuses and lists
#    the other; allow-listing both publishes.
new_case shared
git -C "$S" checkout -q async; echo async > "$S/async-only.c"; seed_commit async-adds
git -C "$S" checkout -q main;  git -C "$S" rm -q patches/0006.patch fiber.c; seed_commit main-deletes
git -C "$S" push -q "$O" main async
git -C "$W" pull -q --ff-only
run_script "$W" origin main "$SCRATCH" "$TMP/report"
printf 'fiber.c\npatches/0006.patch\n' > "$TMP/want"
[ "$rc" = 3 ] && cmp -s "$TMP/want" "$TMP/report" && ok "shared-file deletion refused and both paths reported" || bad "shared-file deletion: rc=$rc, report: $(tr '\n' ' ' < "$TMP/report")"
[ -e "$W/fiber.c" ] && [ -e "$W/patches/0006.patch" ] && ok "refused merge keeps both files" || bad "refused merge lost files"
[ -z "$(remote_ref "$O" "$SCRATCH")" ] && ok "refused merge publishes nothing" || bad "refused merge pushed the scratch ref"
printf 'patches/0006.patch\n' > "$TMP/allow1"
run_script "$W" origin main "$SCRATCH" "$TMP/report" "$TMP/allow1"
[ "$rc" = 3 ] && [ "$(cat "$TMP/report")" = fiber.c ] && ok "partial allow-list still refuses, reports only fiber.c" || bad "partial allow-list: rc=$rc, report: $(tr '\n' ' ' < "$TMP/report")"
run_script "$W" origin main "$SCRATCH" "$TMP/report" "$TMP/want"
[ "$rc" = 0 ] && [ ! -e "$W/fiber.c" ] && [ -n "$(remote_ref "$O" "$SCRATCH")" ] && ok "full allow-list publishes the deletion" || bad "full allow-list: rc=$rc"

# d. async has no commits of its own, so the merge fast-forwards and HEAD^1
#    is a main commit: the deletion must still be caught.
new_case ff
git -C "$S" checkout -q main; git -C "$S" rm -q fiber.c; seed_commit main-deletes
git -C "$S" push -q "$O" main
run_script "$W" origin main "$SCRATCH" "$TMP/report"
[ "$rc" = 3 ] && [ "$(cat "$TMP/report")" = fiber.c ] && [ -e "$W/fiber.c" ] && ok "fast-forward deletion refused" || bad "fast-forward deletion: rc=$rc"

# e. a clean merge whose scratch push the remote refuses (the token-scope case):
#    its own exit code, and a report naming the token the run tried next to
#    git's message. The whole report is what the workflow puts in the issue it
#    opens, so an operator has to be able to fix it from the issue alone (#710).
#    Run once per state of the repository secret ASYNC_SYNC_PUSH_TOKEN: set
#    (the workflow passes its name) and absent (GITHUB_TOKEN), plus a hand run
#    that passes no name at all, because that is what a person running the
#    script gets.
#
# refuse_push <origin>: the remote refuses every push, the way GitHub refuses a
# push of workflow files from a token without the `workflow` scope.
refuse_push() {
	printf '#!/bin/sh\necho "refusing to update workflow files" >&2\nexit 1\n' > "$1/hooks/pre-receive"
	chmod +x "$1/hooks/pre-receive"
}

# refused_push <token-source> <what>: run the script on the current case with
# ASYNC_SYNC_PUSH_TOKEN_SOURCE set to <token-source> (empty: not set at all),
# and check the exit code and the report. One `ok` per call, so a `ok` is never
# printed next to a failure of the same run.
refused_push() {
	src=$1
	what=$2
	fail_before=$fail
	if [ -n "$src" ]; then
		ASYNC_SYNC_PUSH_TOKEN_SOURCE=$src
		export ASYNC_SYNC_PUSH_TOKEN_SOURCE
	else
		unset ASYNC_SYNC_PUSH_TOKEN_SOURCE
	fi
	run_script "$W" origin main "$SCRATCH" "$TMP/report"
	unset ASYNC_SYNC_PUSH_TOKEN_SOURCE
	if [ "$rc" != 4 ]; then
		bad "$what: exited $rc, expected 4"
		cat "$TMP/script.log" >&2
		return
	fi
	if [ -n "$src" ]; then
		want="pushing the scratch ref $SCRATCH was refused; the token it used is $src"
	else
		# The script's own default, not a guess at a token name.
		want="ASYNC_SYNC_PUSH_TOKEN_SOURCE unset"
	fi
	grep -Fq "refusing to update workflow files" "$TMP/report" &&
		grep -Fq "$want" "$TMP/report" ||
		bad "$what: report: $(tr '\n' '|' < "$TMP/report")"
	# An `if` whose condition is false returns 0, a bare `&& ok` does not, and
	# with `set -eu` a function must not end on a false test.
	if [ "$fail" = "$fail_before" ]; then
		ok "$what"
	fi
}

new_case scratch-refused-secret
git -C "$S" checkout -q main; echo more >> "$S/shared.txt"; seed_commit main-edits
git -C "$S" push -q "$O" main
refuse_push "$O"
refused_push ASYNC_SYNC_PUSH_TOKEN "refused scratch push exits 4 and names the token (secret set)"

new_case scratch-refused-no-secret
git -C "$S" checkout -q main; echo more >> "$S/shared.txt"; seed_commit main-edits
git -C "$S" push -q "$O" main
refuse_push "$O"
refused_push GITHUB_TOKEN "refused scratch push exits 4 and names the token (secret absent)"
# A hand run of the script passes no token name, so the report has to say that
# rather than guess. The push is refused again: HEAD is still the merge commit
# of the run above, and a second run only re-attempts the push.
refused_push "" "refused scratch push reports that the workflow passed no token name"

if [ "$fail" = 0 ]; then
	echo "test-async-sync-guard.sh: all checks passed"
else
	echo "test-async-sync-guard.sh: FAILED" >&2
fi
exit "$fail"
