#!/bin/sh
# Issue #196: hand back a php-src tree that is already checked out, overlaid
# with sapi/fpmng, patched and configured -- reusing the one from the previous
# run whenever reusing it is safe.
#
#   build/ci-build-tree.sh <tree-dir> <php-src-repo> <php-src-ref> <configure flags...>
#
# On return, `make` in <tree-dir> builds only what actually changed. Measured on
# the poligon (issue #196): a run that only changes files under sapi/fpmng/
# costs 3 s here instead of the 96 s a fresh tree costs, because the other 654
# objects are still on disk and up to date. ccache alone does not get close --
# it was already at a 99.7% direct hit rate and a fresh tree still cost 20 s of
# make on an idle box, since every object has to be relinked and every compile
# re-run through ccache.
#
# Reuse is decided by a key, not by inspection. Anything that would make the
# existing objects wrong goes into the key, and a mismatch wipes the tree:
# there is no partial invalidation, because a tree half-built from two
# configurations links without complaint and produces a binary nobody can
# reason about.
set -e

TREE="${1:?usage: ci-build-tree.sh <tree-dir> <php-src-repo> <php-src-ref> <configure flags...>}"
SRC_REPO="${2:?provide the php-src repository URL}"
SRC_REF="${3:?provide the php-src ref}"
shift 3
FLAGS="$*"

REPO="$(cd "$(dirname "$0")/.." && pwd)"

# --- the key ----------------------------------------------------------------
#
# php-src ref and configure flags are obvious. The other three are the ways a
# reused tree could go stale without either of them changing:
#
#   sapi/fpmng/    a new or deleted .c file changes the object list that
#                  configure freezes into the Makefile. build/prepare.sh warns
#                  about this on stdout, but it exits 0 and nothing reads the
#                  warning, so the warning cannot be the gate here.
#   patches/       prepare.sh detects an already-patched tree and skips
#                  re-applying. If a patch has changed since, the tree keeps
#                  the old version of it and prepare.sh reports success.
#   prepare.sh     it generates config.m4's source lists; a change to how it
#                  splits them changes what gets compiled.
#
# Only file names matter for sapi/fpmng, not contents: a changed .c file must
# recompile, which it does anyway (prepare.sh re-copies the directory), and
# keying on contents would throw the whole tree away on every push -- the exact
# thing this script exists to avoid.
key_input() {
	printf '%s\n%s\n' "$SRC_REF" "$FLAGS"
	(cd "$REPO/sapi/fpmng" && find . -type f | sort)
	[ -d "$REPO/ext/fpmng_metrics" ] && (cd "$REPO/ext/fpmng_metrics" && find . -type f | sort)
	cat "$REPO"/patches/*.patch "$REPO"/patches/php-*/*.patch 2>/dev/null
	cat "$REPO/build/prepare.sh"
}
KEY=$(key_input | sha256sum | cut -d' ' -f1)
KEYFILE="$TREE/.fpmng-ci-key"

# --- reuse or start over ----------------------------------------------------
REUSED=no
if [ -f "$KEYFILE" ] && [ "$(cat "$KEYFILE")" = "$KEY" ] && [ -f "$TREE/Makefile" ]; then
	REUSED=yes
else
	# Stamp removal first: if anything below dies halfway, the next run must
	# see an unkeyed tree and start over rather than trust a partial one.
	rm -f "$KEYFILE"
	rm -rf "$TREE"
	mkdir -p "$TREE"
	git init -q "$TREE"
	git -C "$TREE" fetch -q --depth 1 "$SRC_REPO" "$SRC_REF"
	git -C "$TREE" checkout -q FETCH_HEAD
fi

# Always, reused or not: prepare.sh deletes and rebuilds sapi/fpmng from
# upstream plus our overlay, which is what puts this push's code in the tree.
# It is idempotent on an already-patched tree -- it detects the patch stack and
# reports "patch was already applied" rather than failing.
"$REPO/build/prepare.sh" "$TREE"

# The configure log is kept inside the tree, not in the workspace, because a
# reused tree does not run configure again and the jobs assert on this log --
# fpmng-phpt-fiber checks that --enable-fpmng-fiber was actually honoured
# (issue #97). An assertion that silently has nothing to read on the fast path
# is worse than no assertion.
CONFIGURE_LOG="$TREE/.fpmng-ci-configure.log"

if [ "$REUSED" = no ]; then
	if ! ( cd "$TREE" && ./buildconf --force ) > "$TREE/.fpmng-ci-buildconf.log" 2>&1; then
		echo "=== BUILDCONF FAILED ==="
		tail -20 "$TREE/.fpmng-ci-buildconf.log"
		exit 1
	fi
	# The flag list is meant to word-split.
	# shellcheck disable=SC2086
	if ! ( cd "$TREE" && ./configure $FLAGS ) > "$CONFIGURE_LOG" 2>&1; then
		echo "=== CONFIGURE FAILED ==="
		tail -20 "$CONFIGURE_LOG"
		exit 1
	fi
	echo "$KEY" > "$KEYFILE"
fi

# Touch it so the pruning below can tell a tree in use from an abandoned one.
touch "$KEYFILE"

# --- prune ------------------------------------------------------------------
# A tree is ~435 MB for the dynamic configure and more for the static one, and
# a new key (a new php-src pin, a new configure flag) leaves the old one behind
# for good. 14 days is long enough that a branch someone comes back to still
# finds its tree, and short enough that a retired configuration does not sit on
# the disk forever. Only siblings that carry our own stamp file are touched.
PARENT=$(dirname "$TREE")
find "$PARENT" -mindepth 2 -maxdepth 2 -name .fpmng-ci-key -mtime +14 2>/dev/null |
	while read -r stale; do
		echo "ci-build-tree: pruning $(dirname "$stale") (unused for 14 days)"
		rm -rf "$(dirname "$stale")"
	done

echo "ci-build-tree: tree $TREE reused=$REUSED key=$KEY"
echo "ci-build-tree: configure log $CONFIGURE_LOG"
