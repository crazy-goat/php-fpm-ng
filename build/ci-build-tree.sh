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

# FPMNG_TREE_SKIP_CONFIGURE=yes stops after the checkout, the overlay and the
# patch stack. The static musl build (build/static-full.sh, issue #197) needs
# exactly that much: it configures out of tree, inside Alpine, with a musl
# toolchain this host does not have, and keeps its own key next to its object
# directory. The tree is then complete when `configure.ac` is there rather
# than when a Makefile is.
SKIP_CONFIGURE="${FPMNG_TREE_SKIP_CONFIGURE:-no}"
if [ "$SKIP_CONFIGURE" = yes ]; then
	SENTINEL="configure.ac"
else
	SENTINEL="Makefile"
fi

# --- the key ----------------------------------------------------------------
#
# php-src ref and configure flags are obvious. The rest are the ways a reused
# tree could go stale without either of them changing:
#
#   sapi/fpmng/    a new or deleted .c file changes the object list that
#                  configure freezes into the Makefile. build/prepare.sh warns
#                  about this on stdout, but it exits 0 and nothing reads the
#                  warning, so the warning cannot be the gate here.
#   config.m4,     these are configure inputs, not compile inputs: an added
#   Makefile.frag  AC_DEFINE lands in main/php_config.h and a new PHP_ARG_ENABLE
#                  or -l changes the link line, and a reused tree never runs
#                  configure again. Their contents are keyed, unlike .c/.h.
#   patches/       prepare.sh detects an already-patched tree and skips
#                  re-applying. If a patch has changed since, the tree keeps
#                  the old version of it and prepare.sh reports success.
#   prepare.sh     it generates config.m4's source lists; a change to how it
#                  splits them changes what gets compiled.
#   the compiler   configure records feature detection in main/php_config.h. A
#                  rebuilt CI image with a different gcc must not inherit the
#                  previous one's answers.
#
# Only file names matter for .c and .h, not contents: a changed .c file must
# recompile, which it does anyway (prepare.sh re-copies the directory), and
# keying on contents would throw the whole tree away on every push -- the exact
# thing this script exists to avoid.
key_input() {
	printf '%s\n%s\n%s\n' "$SRC_REPO" "$SRC_REF" "$FLAGS"
	(cd "$REPO/sapi/fpmng" && find . -type f | sort)
	if [ -d "$REPO/ext/fpmng_metrics" ]; then
		(cd "$REPO/ext/fpmng_metrics" && find . -type f | sort)
	fi
	# Everything in the overlay that is not a translation unit: config.m4,
	# Makefile.frag, .m4 includes, and anything of that kind added later.
	find "$REPO/sapi/fpmng" "$REPO/ext/fpmng_metrics" -type f \
		! -name '*.c' ! -name '*.h' -print0 2>/dev/null |
		sort -z | xargs -0 cat 2>/dev/null
	cat "$REPO"/patches/*.patch "$REPO"/patches/php-*/*.patch 2>/dev/null
	cat "$REPO/build/prepare.sh"
	# This script decides what the tree contains and how it is reused, so a
	# change to it must invalidate the trees it produced.
	cat "$REPO/build/ci-build-tree.sh"
	${CC:-cc} --version 2>&1 | head -1
	${CC:-cc} -dumpmachine 2>&1
}
KEY=$(key_input | sha256sum | cut -d' ' -f1)
KEYFILE="$TREE/.fpmng-ci-key"
# Written while the tree is in a state nobody may reuse -- mid-patch, mid-make.
# `concurrency: cancel-in-progress` kills a job outright on every superseded
# push, which is the normal case here, not an edge case: a .o truncated by that
# kill keeps a fresh mtime, so make considers it current for good and the
# resulting link error would reproduce on every later run of the same key. The
# build steps in the workflow write this marker around `make` for the same
# reason; see .github/workflows/build-matrix.yml.
DIRTY="$TREE/.fpmng-ci-dirty"

# --- reuse or start over ----------------------------------------------------
REUSED=no
if [ -f "$KEYFILE" ] && [ "$(cat "$KEYFILE")" = "$KEY" ] && [ -f "$TREE/$SENTINEL" ] &&
   [ ! -f "$DIRTY" ]; then
	REUSED=yes
else
	if [ -f "$DIRTY" ]; then
		echo "ci-build-tree: previous run left $TREE unfinished; starting over"
	fi
	# Stamp removal first: if anything below dies halfway, the next run must
	# see an unkeyed tree and start over rather than trust a partial one.
	rm -f "$KEYFILE"
	rm -rf "$TREE"
	mkdir -p "$TREE"
	git init -q "$TREE"
	git -C "$TREE" fetch -q --depth 1 "$SRC_REPO" "$SRC_REF"
	git -C "$TREE" checkout -q FETCH_HEAD
	# Identity of THIS checkout, for anything that caches build output keyed on
	# the tree (build/static-full.sh keeps an out-of-tree /build). The content
	# key above cannot serve: it is equal for two different checkouts of the
	# same inputs, and a /build full of objects whose generated sources (php
	# writes zend_ini_scanner_defs.h and friends into srcdir) were deleted with
	# the previous tree fails with "No rule to make target" -- observed on
	# issue #206.
	date +%s.%N > "$TREE/.fpmng-ci-epoch"
fi

# Always, reused or not: prepare.sh deletes and rebuilds sapi/fpmng from
# upstream plus our overlay, which is what puts this push's code in the tree.
# It is idempotent on an already-patched tree -- it detects the patch stack and
# reports "patch was already applied" rather than failing.
touch "$DIRTY"
"$REPO/build/prepare.sh" "$TREE"

# The configure log is kept inside the tree, not in the workspace, because a
# reused tree does not run configure again and the jobs assert on this log --
# fpmng-phpt-fiber checks that --enable-fpmng-fiber was actually honoured
# (issue #97). An assertion that silently has nothing to read on the fast path
# is worse than no assertion.
CONFIGURE_LOG="$TREE/.fpmng-ci-configure.log"

if [ "$REUSED" = no ] && [ "$SKIP_CONFIGURE" = no ]; then
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
if [ "$SKIP_CONFIGURE" = yes ]; then
	echo "$KEY" > "$KEYFILE"
fi
rm -f "$DIRTY"

# Touch it so the pruning below can tell a tree in use from an abandoned one.
touch "$KEYFILE"

# --- prune ------------------------------------------------------------------
# A tree is ~435 MB for the dynamic configure and more for the static one, and
# a new key (a new php-src pin, a new configure flag) leaves the old one behind
# for good. 14 days is long enough that a branch someone comes back to still
# finds its tree, and short enough that a retired configuration does not sit on
# the disk forever.
#
# Prune from the root of the cache, not from this runner's own directory: a
# runner that is renamed, deregistered or simply stops picking up this workflow
# never runs this script again, so only its neighbours can ever reclaim it.
# Depth 3 is exactly <root>/<runner>/<config>/.fpmng-ci-key, and nothing but
# this script writes that file, so nothing outside our own trees can match.
ROOT=$(dirname "$(dirname "$TREE")")
find "$ROOT" -mindepth 3 -maxdepth 3 -name .fpmng-ci-key -mtime +14 2>/dev/null |
	while read -r stale; do
		echo "ci-build-tree: pruning $(dirname "$stale") (unused for 14 days)"
		rm -rf "$(dirname "$stale")"
	done

echo "ci-build-tree: tree $TREE reused=$REUSED key=$KEY"
if [ "$SKIP_CONFIGURE" = yes ]; then
	echo "ci-build-tree: not configured here (FPMNG_TREE_SKIP_CONFIGURE=yes)"
else
	echo "ci-build-tree: configure log $CONFIGURE_LOG"
fi
