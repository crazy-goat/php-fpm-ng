#!/usr/bin/env bash
# Create an isolated worktree for one issue and prepare its environment.
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
# long-lived line the default branch has no trace of: php-fpm-ng has the fiber and coop
# executors (sapi/fpmng/fpm/fpm_pool_fiber*.c, fpm_pool_coop*.c), 10 fiber/coop .phpt tests
# and .github/workflows/async-fiber.yml on `async` and none of them on `main` (verified with
# `git ls-tree` against origin/async and origin/main), so a worktree cut from `main` for a
# fiber issue would hold none of the code the issue is about and every check run in it
# would describe another tree.
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
# there. --exit-code is what turns "no ref matched" into a non-zero status (measured: 2).
git -C "$root" ls-remote --exit-code --heads origin "refs/heads/$base" >/dev/null \
  || { echo "base branch does not exist on origin: $base" >&2; exit 1; }
# A plain branch name is enough for the fetch, no refs/heads/ qualification: git updates the
# matching remote-tracking ref opportunistically under the default refspec
# (+refs/heads/*:refs/remotes/origin/*), for every branch, not only the default one. What
# makes origin/$base resolvable below is that update, not FETCH_HEAD.
git -C "$root" fetch origin "$base"
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
