#!/usr/bin/env bash
# Create an isolated worktree for one issue and prepare its environment.
#
# Shared script: the source of truth is standard/worktree.sh in crazy-goat/.github,
# compared here at blob 529a8fc7 (no --base). This copy adds --base, and on the
# default path two effects that come with it: one ls-remote round trip before the
# fetch, and a "Base:" line in the output. No other divergence from that blob is
# intended, so a sync from the standard has to carry --base with it. Nothing runs
# build/test-worktree-base.sh on main, only on async, so on main a sync that drops
# --base is not caught by anything.
#
# Usage: bin/worktree.sh [--dir <path>] [--base <branch>] <issue-number> [type]
#   type:  feat|fix|docs|refactor|test|chore (default: derived from the type:* label)
#   --dir: exact path of the new worktree (see "Worktree location" below)
#   --base: branch to create the worktree from (default: the repository's default branch)
#
# Creates a worktree (location: see below) on branch <type>/issue-<N>-<slug>, empty
# findings.md and review.md (both gitignored), a unique COMPOSE_PROJECT_NAME and
# free host ports in .env.worktree, then runs the optional bin/worktree-setup.sh.
set -euo pipefail

usage="usage: bin/worktree.sh [--dir <path>] [--base <branch>] <issue-number> [type]"
dir=""
base=""
args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dir) dir="${2:?$usage}"; shift 2 ;;
    --dir=*) dir="${1#--dir=}"; [[ -n "$dir" ]] || { echo "$usage" >&2; exit 1; }; shift ;;
    --base) base="${2:?$usage}"; shift 2 ;;
    --base=*) base="${1#--base=}"; [[ -n "$base" ]] || { echo "$usage" >&2; exit 1; }; shift ;;
    -h|--help) echo "$usage"; exit 0 ;;
    -*) echo "unknown option: $1" >&2; echo "$usage" >&2; exit 1 ;;
    *) args+=("$1"); shift ;;
  esac
done
# An option where a branch name belongs (`--base --dir /tmp/x 1`) has to be refused here:
# further down it is read as the issue number, and the operator is told the issue is not a
# number instead of being told that --base swallowed an option.
[[ "$base" != -* ]] || { echo "--base needs a branch name: $base" >&2; exit 1; }
# Git has to accept the name as a branch name before anything else looks at it. The ls-remote
# below is a glob, so a base like "as*" matches refs/heads/async, passes the existence check
# and only then dies in the fetch (measured); a name with a space or a `..` in it fails the
# same way. Every branch name this repository uses is accepted (main, async,
# release/v0.8.0, <type>/issue-<N>-<slug>), and git would refuse to create the ones this
# rejects anyway.
if [[ -n "$base" ]]; then
  git check-ref-format --branch "$base" >/dev/null 2>&1 \
    || { echo "not a branch name: $base" >&2; exit 1; }
fi
if [[ ${#args[@]} -lt 1 || ${#args[@]} -gt 2 ]]; then echo "$usage" >&2; exit 1; fi
issue="${args[0]}"
type="${args[1]:-}"
[[ $issue =~ ^[0-9]+$ ]] || { echo "issue must be a number: $issue" >&2; exit 1; }
case "$type" in
  ""|feat|fix|docs|refactor|test|chore) ;;
  *) echo "unknown type: $type (feat|fix|docs|refactor|test|chore)" >&2; exit 1 ;;
esac

# The main checkout, also when this runs inside one of its worktrees.
root="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")"
repo="$(basename "$root")"
# The base is the default branch unless --base says otherwise. A repository can carry a
# long-lived line the default branch has no trace of (work that is not to merge into the
# default branch, or an experiment that runs beside it), and a worktree cut from the default
# branch would hold none of the code such an issue is about.
if [[ -z "$base" ]]; then
  base="$(gh repo view --json defaultBranchRef --jq .defaultBranchRef.name)"
fi

info="$(gh issue view "$issue" --json title,labels)"
title="$(jq -r .title <<<"$info")"

if [[ -z "$type" ]]; then
  label="$(jq -r '[.labels[].name | select(startswith("type:"))][0] // ""' <<<"$info")"
  case "${label#type:}" in
    bug|security) type=fix ;;
    feature|performance) type=feat ;;
    docs) type=docs ;;
    tests) type="test" ;;
    refactor) type=refactor ;;
    *) type=chore ;;
  esac
fi

slug="$(tr '[:upper:]' '[:lower:]' <<<"$title" | tr -cs 'a-z0-9' '-' | sed 's/^-//; s/-$//' | cut -c1-40 | sed 's/-$//')"
branch="$type/issue-$issue-$slug"
# Worktree location, first match wins:
#   1. --dir <path>                      exactly <path>
#   2. WORKTREES_DIR=<dir>               <dir>/<repo>/issue-<N>
#   3. a .worktrees/ next to the clone   ../.worktrees/<repo>/issue-<N>
#   4. otherwise                         ../<repo>-worktrees/issue-<N>
parent="$(dirname "$root")"
if [[ -z "$dir" ]]; then
  if [[ -n "${WORKTREES_DIR:-}" ]]; then
    dir="$WORKTREES_DIR/$repo/issue-$issue"
  elif [[ -d "$parent/.worktrees" ]]; then
    dir="$parent/.worktrees/$repo/issue-$issue"
  else
    dir="$parent/$repo-worktrees/issue-$issue"
  fi
fi
# A relative path is relative to the current directory, not to the main checkout.
case "$dir" in /*) ;; *) dir="$PWD/$dir" ;; esac

# The base is checked before the fetch and before the worktree exists: `git worktree add`
# would otherwise fail with "invalid reference: origin/<base>", which reads like a git
# problem rather than a mistyped branch. refs/heads/ is spelled out because git matches an
# unqualified ls-remote pattern against the *tail* of a ref name: "v0.8.0" is answered by
# refs/heads/release/v0.8.0, so an unqualified pattern would accept a branch that is not
# there.
rc=0
git -C "$root" ls-remote --exit-code --heads origin "refs/heads/$base" >/dev/null || rc=$?
case "$rc" in
  0) ;;
  # --exit-code answers "no ref matched" with 2 (measured). It answers an unreachable remote
  # or refused credentials with 128 (measured), and calling that a mistyped branch sends the
  # operator looking for a typo that is not there: git's own stderr reached the terminal, but
  # the message under it said the branch does not exist.
  2) echo "base branch does not exist on origin: $base" >&2; exit 1 ;;
  *) echo "cannot reach origin to check base branch $base (git ls-remote exited $rc)" >&2; exit "$rc" ;;
esac
# The destination is spelled out because a plain `git fetch origin "$base"` is not enough:
# git updates refs/remotes/origin/$base opportunistically only while remote.origin.fetch is
# the wildcard refspec, and a `git clone --single-branch` clone (or `git remote set-branches`)
# narrows it to one branch. There the plain fetch writes FETCH_HEAD alone, origin/$base stays
# unresolvable and `git worktree add` dies with "invalid reference" (measured). The leading +
# forces the update, so a base that was rewritten (rebased) on the remote is taken instead of
# refused; without it git rejects the non-fast-forward and the fetch exits 1, which under
# `set -e` aborts the run before the worktree exists (measured). The wildcard refspec this
# replaces carries the same +.
git -C "$root" fetch origin "+refs/heads/$base:refs/remotes/origin/$base"
git -C "$root" worktree add -b "$branch" "$dir" "origin/$base"

cd "$dir"
for f in findings.md review.md; do
  printf '# %s for #%s\n\n' "${f%.md}" "$issue" >"$f"
done

project="$(tr '[:upper:]' '[:lower:]' <<<"$repo-issue-$issue" | tr -c 'a-z0-9\n' '-')"
{
  echo "COMPOSE_PROJECT_NAME=$project"
  # Every ${NAME_PORT:-1234} in a compose file gets a free host port here.
  # grep exits 1 when there is no compose file; that is fine.
  { grep -rhoE '\$\{[A-Z0-9_]*PORT[A-Z0-9_]*:-[0-9]+\}' --include='*compose*.y*ml' . 2>/dev/null || true; } \
    | sed -E 's/^\$\{([A-Z0-9_]+):-.*/\1/' | sort -u | while read -r name; do
      port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')"
      echo "$name=$port"
    done
} >.env.worktree

export COMPOSE_PROJECT_NAME="$project" COMPOSE_ENV_FILES=.env.worktree
if [[ -x bin/worktree-setup.sh ]]; then
  bin/worktree-setup.sh
fi

echo
echo "Worktree: $dir"
echo "Branch:   $branch"
# The base is printed because it is the one input that changes which tree the worktree holds,
# and nothing else in the output records it: the branch name says <type>/issue-<N>-<slug>,
# which names the issue, not the line it was cut from.
echo "Base:     origin/$base"
echo "Use:      cd $dir && set -a && . ./.env.worktree && set +a"
