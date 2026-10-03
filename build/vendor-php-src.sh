#!/bin/sh
# Maintain the bounded php-src subset in third_party/php-src/ (issue #421).
#
#   build/vendor-php-src.sh check
#   build/vendor-php-src.sh import <php-src-checkout>
#
# WHY THIS EXISTS. Main builds against a distribution's prebuilt libphp and its
# development headers (issue #418, contract in #419). What the SAPI still needs
# from php-src is not the engine but a few dozen FPM/FastCGI source files that
# no php*-dev package ships: upstream's sapi/fpm/fpm/ files this repo does not
# override, and main/fastcgi.c/.h.
# They are kept in third_party/php-src/, at their upstream paths, so that a
# build needs this repository and the SDK and nothing else.
#
# The same mechanism holds the test fixtures (issue #423): upstream's
# run-tests.php, the FPM test harness and the retained upstream FPM .phpt
# suite, so that the .phpt runners need no php-src checkout
# either. They are listed in the same manifest, under the same rules; the build
# never reads them (README.md has the list and the reason for each).
#
# THE MANIFEST IS THE INVENTORY. third_party/php-src/MANIFEST lists every
# vendored file with its upstream path, the SHA-256 of the pristine upstream
# file at the pinned tag, and the SHA-256 of the copy in this tree. Main carries
# no php-src patch (issue #592), so the two hashes are equal for every file;
# zlog_upstream.h is renamed, not changed. A file whose hashes differ is
# refused.
# Why each file is in, and why the rest of upstream's sapi/fpm/ is out, is in
# third_party/php-src/README.md.
#
# check   needs no php-src and no network. It fails when a vendored file no
#         longer has the hash the manifest records (someone edited it in
#         place, which the next import would silently undo), when a file
#         appears under third_party/php-src/ that the manifest does not list,
#         when a vendored copy differs from the pristine upstream file the
#         manifest names, or when a vendored FPM
#         file has the same name as one sapi/fpmng/fpm/ owns (ours would shadow
#         it in every build, so the vendored copy would be dead weight that
#         looks alive).
#
# import  refreshes the vendored files from a php-src checkout: copy the
#         pristine files, hash, rewrite the manifest. It runs `check` first and
#         refuses to overwrite local edits: main carries no php-src patch, so
#         a vendored copy is never edited. To bump the
#         pin, check out the new tag and run import; then rebuild and run the
#         suites, because a new upstream file or a changed header is exactly
#         what import cannot judge.
#
# The file list itself is edited by hand in the manifest: add a line with "-"
# in both hash columns and run import.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
TP=$REPO/third_party/php-src
MANIFEST=$TP/MANIFEST

fail() {
  echo "vendor-php-src.sh: FAIL: $*" >&2
  exit 1
}

usage() {
  echo "usage: build/vendor-php-src.sh check" >&2
  echo "       build/vendor-php-src.sh import <php-src-checkout>" >&2
  exit 2
}

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    fail "neither sha256sum nor shasum is available"
  fi
}

manifest_value() {
  awk -F '\t' -v k="$1" '$1 == k && NF == 2 { print $2 }' "$MANIFEST"
}

# Vendored file lines: four tab-separated columns, comments and the two-column
# key/value lines excluded.
manifest_files() {
  awk -F '\t' '!/^#/ && NF == 4 { print }' "$MANIFEST"
}

# do_check [preimport]
#
# preimport is the guard import runs before it overwrites anything. It looks
# only for what an import would destroy -- an in-place edit, an unlisted file,
# a shadowed one -- and skips the two things an import exists to fix: lines
# not imported yet ("-" hashes, a file just added to the list) and files no
# longer listed (import deletes them). It returns instead of exiting, so the
# caller can say why it refuses.
do_check() {
  mode=${1:-}
  [ -f "$MANIFEST" ] || fail "no manifest at ${MANIFEST#"$REPO"/}"
  tag=$(manifest_value tag)
  [ -n "$tag" ] || fail "the manifest has no 'tag' line"
  errors=0
  listed=$(mktemp)
  manifest_files > "$listed.lines"
  [ -s "$listed.lines" ] || fail "the manifest lists no files"
  while IFS="$(printf '\t')" read -r path upstream up_sha local_sha; do
    echo "$path" >> "$listed"
    f=$TP/$path
    if [ "$mode" = preimport ] && [ "$local_sha" = - ]; then
      continue
    fi
    if [ ! -f "$f" ]; then
      echo "  missing: third_party/php-src/$path" >&2
      errors=$((errors + 1))
      continue
    fi
    [ "$local_sha" != - ] || { echo "  never imported: $path (run import)" >&2; errors=$((errors + 1)); continue; }
    # Main carries no php-src patch: the copy is the pristine file.
    if [ "$up_sha" != "$local_sha" ]; then
      echo "  not pristine: third_party/php-src/$path (upstream sha256 $up_sha, manifest $local_sha)" >&2
      errors=$((errors + 1))
    fi
    got=$(sha256 "$f")
    if [ "$got" != "$local_sha" ]; then
      echo "  edited in place: third_party/php-src/$path" >&2
      echo "    manifest $local_sha" >&2
      echo "    file     $got" >&2
      errors=$((errors + 1))
    fi
    # A vendored FPM file the overlay also owns: ours wins on every build, so
    # this copy would never be compiled while looking like it is.
    case "$path" in
      sapi/fpm/fpm/*)
        rel=${path#sapi/fpm/}
        if [ -e "$REPO/sapi/fpmng/$rel" ]; then
          echo "  shadowed: $path is also owned as sapi/fpmng/$rel; drop it from the manifest" >&2
          errors=$((errors + 1))
        fi
        ;;
      # Same for a test fixture: build/phpt-tree.sh lays this repo's
      # sapi/fpmng/tests/ over upstream's, so a same-named file of ours would
      # replace the vendored one without anyone noticing.
      sapi/fpm/tests/*)
        rel=${path#sapi/fpm/}
        if [ -e "$REPO/sapi/fpmng/$rel" ]; then
          echo "  shadowed: $path is also owned as sapi/fpmng/$rel; drop it from the manifest" >&2
          errors=$((errors + 1))
        fi
        ;;
    esac
    : "$upstream"
  done < "$listed.lines"
  # Anything on disk the manifest does not account for. The manifest and the
  # README are the two files that describe the directory rather than belong to
  # upstream.
  (cd "$TP" && find . -type f | sed 's|^\./||' | grep -vx 'MANIFEST' | grep -vx 'README.md' | LC_ALL=C sort) > "$listed.disk"
  LC_ALL=C sort -o "$listed" "$listed"
  extra=$(LC_ALL=C comm -13 "$listed" "$listed.disk")
  # An import removes these itself (a line dropped from the list), so the
  # guard before it lets them through.
  if [ -n "$extra" ] && [ "$mode" != preimport ]; then
    echo "$extra" | sed 's|^|  not in the manifest: third_party/php-src/|' >&2
    errors=$((errors + 1))
  fi
  rm -f "$listed" "$listed.lines" "$listed.disk"
  if [ "$errors" != 0 ]; then
    [ "$mode" = preimport ] && return 1
    fail "$errors problem(s) in third_party/php-src (see above)"
  fi
  [ "$mode" = preimport ] && return 0
  echo "vendor-php-src.sh: third_party/php-src matches its manifest ($tag, $(manifest_files | wc -l | tr -d ' ') files, pristine)"
}

do_import() {
  SRC=${1:-}
  [ -n "$SRC" ] || usage
  SRC=$(cd "$SRC" 2>/dev/null && pwd) || fail "no such directory: $1"
  [ -f "$SRC/main/php_version.h" ] && [ -d "$SRC/sapi/fpm/fpm" ] || fail "$SRC does not look like php-src"

  # Refuse to overwrite local edits. A tree that was never imported has "-"
  # hashes and passes this part; a missing manifest is an error either way.
  if [ -n "$(manifest_value tag)" ]; then
    do_check preimport ||
      fail "refusing to import over the problems above: restore the vendored file (git checkout) and import again"
  fi

  # Pristine input only. A modified checkout would record local changes as
  # upstream's and make the next diff meaningless.
  if [ -d "$SRC/.git" ] || git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1; then
    dirty=$(git -C "$SRC" status --porcelain --untracked-files=no 2>/dev/null || true)
    [ -z "$dirty" ] || fail "$SRC has local modifications (git status); import needs a pristine checkout:
$dirty"
    commit=$(git -C "$SRC" rev-parse HEAD)
    tag=$(git -C "$SRC" describe --tags --exact-match 2>/dev/null || true)
  else
    fail "$SRC is not a git checkout; import records the upstream commit and needs git to know it"
  fi
  ver=$(awk -F'"' '/PHP_VERSION /{print $2}' "$SRC/main/php_version.h")
  [ -n "$tag" ] || fail "$SRC HEAD ($commit) is not a tagged release; the pin must be a tag (PHP $ver)"

  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT
  manifest_files | while IFS="$(printf '\t')" read -r path upstream up_sha local_sha; do
    : "$up_sha" "$local_sha"
    mkdir -p "$work/$(dirname "$upstream")"
    [ -f "$SRC/$upstream" ] || fail "upstream $tag has no $upstream (listed for $path)"
    cp "$SRC/$upstream" "$work/$upstream"
  done
  manifest_files | while IFS="$(printf '\t')" read -r path upstream up_sha local_sha; do
    : "$up_sha" "$local_sha"
    printf '%s\t%s\n' "$path" "$(sha256 "$work/$upstream")"
  done > "$work/.pristine"
  new=$work/.manifest
  {
    echo "# third_party/php-src/MANIFEST -- written by build/vendor-php-src.sh import."
    echo "# Hand-edit only the file list (path and upstream path, '-' for both hashes),"
    echo "# then run import. check compares the files against the last column."
    printf 'tag\t%s\n' "$tag"
    printf 'commit\t%s\n' "$commit"
    echo "# vendored path	upstream path	upstream sha256 (pristine $tag)	vendored sha256"
  } > "$new"
  manifest_files | while IFS="$(printf '\t')" read -r path upstream up_sha local_sha; do
    : "$up_sha" "$local_sha"
    mkdir -p "$TP/$(dirname "$path")"
    cp "$work/$upstream" "$TP/$path"
    pristine=$(awk -F '\t' -v p="$path" '$1 == p { print $2 }' "$work/.pristine")
    printf '%s\t%s\t%s\t%s\n' "$path" "$upstream" "$pristine" "$(sha256 "$TP/$path")" >> "$new"
  done
  mv "$new" "$MANIFEST"
  # Files dropped from the list go with the import, so the directory and the
  # manifest cannot drift apart. They are tracked in git, so this is visible
  # in the diff and recoverable.
  manifest_files | cut -f 1 | LC_ALL=C sort > "$work/.listed"
  (cd "$TP" && find . -type f | sed 's|^\./||' | grep -vx 'MANIFEST' | grep -vx 'README.md' | LC_ALL=C sort) |
    LC_ALL=C comm -13 "$work/.listed" - | while read -r f; do
      rm -f "$TP/$f"
      echo "  removed third_party/php-src/$f (no longer in the manifest)"
    done
  find "$TP" -type d -empty -delete 2>/dev/null || true
  trap - EXIT
  rm -rf "$work"
  echo "vendor-php-src.sh: imported $(manifest_files | wc -l | tr -d ' ') files from $tag ($commit)"
  do_check
}

case "${1:-}" in
  check) do_check ;;
  import) shift; do_import "$@" ;;
  *) usage ;;
esac
