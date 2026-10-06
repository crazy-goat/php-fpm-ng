#!/bin/sh
# Gate the artefact a user installs, not the tree it was built from (issue #224).
#
# The difference that matters is the INSTALL. Every other cell in the matrix
# runs a binary out of a build directory, which exercises none of what a
# package introduces: the file layout, the conffiles, the service file, the
# dependency on the distribution's embed package, and issue #220's refusal to
# run against a libphp from another minor. This script builds the package the
# way a release does and then throws the build host away -- stage 2 is a
# container with no compiler in it, which installs the package and runs the
# owned .phpt suite against the installed binary.
#
# WHAT IT ASSERTS. The standing trap in this repository is that a
# misconfigured run skips every test and still reports success: as root php-fpm
# refuses to start, so the suite reports PASS=0 with everything skipped and
# exits 0. So the gate names the exceptions (issue #678): every owned test must
# PASS except those listed, with a reason, in build/package-gate-expected.txt,
# and a listed one must still skip. A run that gains skips or loses them fails
# here by test name; the total is the number of owned tests, not a constant.
#
# Usage: build/ci-package-gate.sh deb|apk <outdir>
#   outdir            package, results and logs land here
#
# There is no php-src argument (issue #424): the build needs no php-src (issue
# #422) and the test fixtures come from third_party/php-src/ through
# build/phpt-tree.sh (issue #423).
#
# Runs on the host, not in a container: it drives `docker run` with bind
# mounts, and -v paths are resolved by the host daemon.
set -eu

fail() {
    echo "ci-package-gate.sh: FAIL: $*" >&2
    # Issue #669: a failing .phpt leaves the only record of why it failed --
    # the .diff/.out/.exp/.log run-tests.php writes next to it -- in the tree
    # the suite ran in, and this script leaves that tree behind. Say where, on
    # every fail(): issue #527 was a red cell that said nothing about them,
    # which is why the fix for it could only be by analysis.
    #
    # Every fail(), not every failure: set -eu above exits 1 at the docker run
    # of a stage that aborts, without reaching this function. Nothing is lost
    # there, because a stage that never ran the suite wrote no .phpt evidence.
    #
    # Not the runner's own <results>/failed-artifacts/, which is what
    # build/run-fpmng-phpt.sh:130 writes: that copy is guarded by a TREE_DIR
    # this script never sets (it hands the runner a directory, so the runner
    # assembled no tree of its own). release.yml uploads this directory as
    # phpt-failure-<cell> when the cell is red.
    #
    # The guard is on the files, not on the directory, so the line stays off a
    # failure that left no evidence -- a package that did not build has none,
    # and neither does a cell that failed for any reason before the suite ran.
    # Globs rather than a test for one file: a failing test does not
    # necessarily produce all four extensions, and one of them existing is
    # already something to point at. An unmatched glob is left as itself, so it
    # never matches.
    if [ -n "${OUT:-}" ]; then
        evidence=$OUT/work/prepared/sapi/fpmng/tests
        for f in "$evidence"/*.diff "$evidence"/*.out "$evidence"/*.exp "$evidence"/*.log; do
            if [ -e "$f" ]; then
                echo "ci-package-gate.sh: the failing tests' diffs and output are in $evidence/ (*.diff, *.out, *.exp, *.log)" >&2
                break
            fi
        done
    fi
    exit 1
}

FLAVOUR=${1:?usage: build/ci-package-gate.sh deb|apk <outdir>}
OUT=${2:?usage: build/ci-package-gate.sh deb|apk <outdir>}
[ $# -eq 2 ] || fail "usage: build/ci-package-gate.sh deb|apk <outdir> (the php-src argument was removed in issue #424)"
REPO=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

# WHICH PACKAGE THIS RUN GATES (issue #294). The repository publishes two: the
# default one, built without TLS and without ACME (issues #280, #281), and
# php-fpm-ng-tls, built with both and described in its own package description
# as beta and unaudited.
#
# FPMNG_PACKAGE_TLS=1 gates the second one. It is OFF by default and the pull
# request matrix does not set it: this script is the longest job in CI, and
# doubling it on every pull request to gate a package that only ships on a tag
# would buy a slower CI and no new information about the change under review.
# .github/workflows/release.yml sets it, on a tag, next to the default rows --
# so the TLS package is never published without having been installed into a
# container with no compiler in it and scored the counts below.
TLS_PACKAGE=${FPMNG_PACKAGE_TLS:-0}
case "$TLS_PACKAGE" in
0) PKGNAME=php-fpm-ng ; BUILD_FLAGS='FPMNG_TLS=0 FPMNG_ACME=0' ;;
1) PKGNAME=php-fpm-ng-tls ; BUILD_FLAGS='FPMNG_TLS=1 FPMNG_ACME=1' ;;
*) fail "FPMNG_PACKAGE_TLS must be 0 or 1, not '$TLS_PACKAGE'" ;;
esac

# What the packages call themselves. Resolved here, on the host, because the
# containers below mount the repository read-only and have no git in them -- so
# the package scripts' own fallback would land on "unknown" every single run.
# The release workflow (issue #223) passes a tag in; otherwise it is the commit.
RELEASE=${FPMNG_RELEASE:-$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)}
command -v docker >/dev/null || fail "docker is not available; this script drives containers"

# The expected score is not a set of numbers (issue #678). TOTAL is the number
# of owned tests in this tree, and the only results that are not a PASS are the
# named exceptions in build/package-gate-expected.txt, one line each with its
# reason. build/package-gate-compare.sh does the comparison. The history of how
# the old pinned counts moved is in git (build/ci-package-gate.sh before #678).
CELL=$FLAVOUR
[ "$TLS_PACKAGE" = 1 ] && CELL=$FLAVOUR-tls

case "$FLAVOUR" in
deb)
    IMAGE=ubuntu:26.04
    # binutils for objdump and nm (package-deb.sh resolves NEEDED sonames and
    # reads the binary's symbols with them),
    # php8.5-dev for the headers libphp-build.sh compiles against, the embed
    # package for the library it links, and libevent/libacl for what the SAPI
    # itself needs. The default package gets no OpenSSL headers at all, because
    # it is built without TLS (issue #280) and this stage should not be able to
    # link it by accident.
    # dpkg-deb is in dpkg, which is already there -- dpkg-dev is
    # deliberately NOT installed: it pulls in gcc, and a build stage is allowed
    # a compiler but this keeps the two package sets honest about who needs one.
    # libssl-dev only for the TLS package: libevent-dev ships
    # libevent_openssl, but linking it needs the OpenSSL headers, and leaving
    # them out of the default build is what makes "this stage cannot link TLS
    # by accident" true rather than merely intended.
    [ "$TLS_PACKAGE" = 1 ] && TLS_DEV=libssl-dev || TLS_DEV=
    BUILD_SETUP="export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq binutils php8.5-dev libphp8.5-embed \
            libevent-dev libacl1-dev $TLS_DEV >/dev/null"
    # The negative control, built here because only this stage has the tools:
    # the identical package, claiming a PHP minor that is not installed on the
    # target. package-deb.sh leaves the unpacked tree behind in /out/root.
    NEGATIVE_CONTROL='sed -i "s/libphp8.5-embed/libphp8.4-embed/" /out/root/DEBIAN/control
        dpkg-deb --build --root-owner-group /out/root /out/wrong-minor.deb >/dev/null
        sed -i "s/libphp8.4-embed/libphp8.5-embed/" /out/root/DEBIAN/control'
    PACKAGE_CMD='/repo/build/package-deb.sh /out/php-fpm-ng /out'
    # dpkg-deb --root-owner-group packs a tree it does not have to own, so the
    # Debian payload is content to run as root.
    RUN_PAYLOAD='sh /out/stage1-payload.sh'
    ;;
apk)
    IMAGE=alpine:edge
    # openssl-dev only for the TLS package (issue #294). Without it the build
    # stage does not get the headers that would let it link OpenSSL even by
    # accident, which is what a default build being TLS-free (issue #280) is
    # supposed to mean. libphp-build.sh asserts the produced binary's dynamic
    # section either way, which is the evidence; this is the belt.
    [ "$TLS_PACKAGE" = 1 ] && TLS_DEV=openssl-dev || TLS_DEV=
    # --upgrade: `apk add` leaves an already-installed package alone, so a warm
    # image (issue #240) from before the distribution's last PHP bump keeps its
    # old php85-dev while php85-embed arrives new in stage 2, and the binary
    # prints the patch-level notice on every start (issue #557, measured on the
    # poligon box against a 2026-09-14 image: SDK 8.5.10, libphp 8.5.11).
    # apt-get install already upgrades, so the deb branch needs nothing.
    BUILD_SETUP="apk add --no-cache --upgrade alpine-sdk php85-dev php85-embed \
            libevent-dev acl-dev $TLS_DEV >/dev/null"
    # Same negative control on the Alpine side: abuild is re-run over the
    # APKBUILD package-apk.sh generated, with the dependency moved to a minor
    # this image does not have, under a name of its own so the two packages
    # cannot be confused for each other.
    NEGATIVE_CONTROL='sed -i "s/^depends=.*/depends=\"php84-embed\"/;s/^pkgname=.*/pkgname=php-fpm-ng-wrong-minor/" /out/abuild/APKBUILD
        ( cd /out/abuild && REPODEST=/out/wrong-minor abuild -F -P /out/wrong-minor rootpkg ) >/dev/null'
    PACKAGE_CMD='/repo/build/package-apk.sh /out/php-fpm-ng /out'
    # abuild refuses to run as root and signs with the calling user's key, so
    # the whole Alpine payload runs as an unprivileged builder. abuild-keygen
    # makes a throwaway key for this container; the packages the project
    # publishes are signed elsewhere (issue #223), which is why stage 2 installs
    # with --allow-untrusted.
    RUN_PAYLOAD='adduser -D -u 1000 builder
        addgroup builder abuild 2>/dev/null || true
        chown builder /out
        su builder -c "abuild-keygen -a -n >/dev/null 2>&1; sh /out/stage1-payload.sh"'
    ;;
*)
    fail "unknown package flavour: $FLAVOUR (expected deb or apk)" ;;
esac

# The warm images (issue #240). BUILD_SETUP and the stage 2 install lines below
# are unchanged and still name every package they need: against these images
# they find it already unpacked and return in seconds, against the bare base
# image they install it, exactly as this script always did. So a machine that
# has never pulled them is slower, never different -- and the package list stays
# written down here rather than drifting into a Dockerfile.
#
# Measured on the poligon box: 232 of the deb flavour's 328 seconds were
# `apt-get` unpacking the distribution's PHP. See .github/docker/package-gate.Dockerfile.
#
# FPMNG_REQUIRE_WARM_IMAGES turns the fallback into a failure. In CI it is set,
# because there the fallback is never the situation it was written for -- it
# means the ghcr login did not happen or the token cannot read the package, and
# the job would otherwise take issue #240's 328 seconds again and still pass,
# with nothing in the log a reader would stop on. Off by default, so a working
# copy with no ghcr credentials still runs the gate.
GATE_IMAGE_PREFIX=${FPMNG_GATE_IMAGE_PREFIX:-ghcr.io/crazy-goat/php-fpm-ng-package-gate}
warm_image() {
    _want="$GATE_IMAGE_PREFIX:$1"
    if docker image inspect "$_want" >/dev/null 2>&1 || docker pull -q "$_want" >/dev/null 2>&1; then
        echo "$_want"
    else
        if [ -n "${FPMNG_REQUIRE_WARM_IMAGES:-}" ]; then
            fail "warm image $_want is unavailable and FPMNG_REQUIRE_WARM_IMAGES is set"
        fi
        echo "$IMAGE"
    fi
}
BUILD_IMAGE=$(warm_image "$FLAVOUR-build")
TEST_IMAGE=$(warm_image "$FLAVOUR-test")
echo "ci-package-gate.sh: gating $PKGNAME ($BUILD_FLAGS)"
echo "ci-package-gate.sh: build stage in $BUILD_IMAGE, install stage in $TEST_IMAGE"

# ---------------------------------------------------------------------------
# Stage 1: the build host. It has the compiler and the headers; nothing it
# leaves behind is used by stage 2 except the package itself and the test
# fixtures staged below.
# ---------------------------------------------------------------------------
echo "ci-package-gate.sh: stage 1 -- build the binary and the $FLAVOUR $PKGNAME package"
cat > "$OUT/stage1-payload.sh" <<EOF
set -eu
FPMNG_RELEASE=$RELEASE
export FPMNG_RELEASE
# The only difference between the two packages, and it is one line: what the
# binary is built with. Everything downstream -- the package name, its
# description, the expected counts -- follows from the artefact this produces.
export $BUILD_FLAGS
# The version guard the packaged binary carries (issue #220), in all three of
# its outcomes, against this SDK (issue #422). It needs nothing but the SDK, so
# it runs here, where the SDK is.
/repo/build/test-libphp-abi-guard.sh
/repo/build/libphp-build.sh /out
$PACKAGE_CMD
$NEGATIVE_CONTROL

# The fixtures stage 2 needs to run the suite: run-tests.php next to the tests,
# plus the source-tree files individual tests reach for by relative path.
# Assembled from this repository's bounded copy of them (issue #423), because
# stage 2 has no php-src and must not have one -- it is standing in for a user's
# machine.
rm -rf /out/prepared
/repo/build/phpt-tree.sh /out/prepared
EOF
cat > "$OUT/stage1.sh" <<EOF
set -eu
$BUILD_SETUP
$RUN_PAYLOAD
EOF
docker run --rm \
    -v "$REPO:/repo:ro" -v "$OUT:/out" \
    "$BUILD_IMAGE" sh /out/stage1.sh

# Stage 1 ran as root, so everything under $OUT is root-owned and the
# unprivileged CI user could not clean it up on the next run. Reclaim it from
# a container, since chowning a root-owned file is what an unprivileged user
# cannot do.
docker run --rm -v "$OUT:/out" "$BUILD_IMAGE" chown -R "$(id -u):$(id -g)" /out

# ---------------------------------------------------------------------------
# Stage 2: a machine that only installs. No compiler, no php-src, no build
# tree -- if the package needs any of those, it fails here, which is the whole
# point of the cell.
# ---------------------------------------------------------------------------
echo "ci-package-gate.sh: stage 2 -- install into a clean $TEST_IMAGE and run the owned suite"
# The name resolved above, handed to the payload: every heredoc below is
# quoted, so nothing in them expands on the host.
cat > "$OUT/stage2.sh" <<EOF
PKGNAME=$PKGNAME
EOF
cat >> "$OUT/stage2.sh" <<'EOF'
set -eu

# The claim this stage makes about itself. binutils is installed below for
# strings(1), which build/run-fpmng-phpt.sh uses to identify the binary under
# test; it carries an assembler but no compiler driver, and it is cc that
# would let a package get away with building something on the user's machine.
for c in gcc cc clang make; do
    command -v "$c" >/dev/null 2>&1 && { echo "FAIL: $c is present; this stage is not a clean machine" >&2; exit 1; }
done
echo "ok: no compiler in this image"
EOF

case "$FLAVOUR" in
deb)
    cat >> "$OUT/stage2.sh" <<'EOF'
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# binutils is strings(1), which build/run-fpmng-phpt.sh uses to identify the
# binary under test; patch(1) is what build/phpt-parallel.sh applies its
# fixture patches with (issue #394); php8.5-cli is the harness that runs run-tests.php; openssl
# is not the library (that comes with the CLI) but its configuration file --
# without /usr/lib/ssl/openssl.cnf openssl_pkey_new() fails with a bare
# "No such file or directory" and the three ACME tests fail for a reason that
# has nothing to do with the package. None of the four is a dependency of what
# we ship: they belong to the test rig, and naming them here keeps that visible.
apt-get install -y -qq binutils patch php8.5-cli openssl >/dev/null
apt-get install -y -qq "$(find /out -maxdepth 1 -name "${PKGNAME}_*.deb" ! -name 'wrong-minor*' | head -1)"
dpkg -s "$PKGNAME" | grep -E '^(Package|Version|Depends|Conflicts|Replaces|Provides):'
LICENSE_FILE=/usr/share/doc/$PKGNAME/copyright
ldd /usr/sbin/php-fpm-ng | grep libphp
PHP_CLI=/usr/bin/php8.5

# The negative control (issue #224's first acceptance criterion): the same
# package, claiming a PHP minor that is not installed. A gate that only ever
# sees the good case cannot say whether it would catch the bad one. The refusal
# has to come from the package manager, before anything is unpacked -- if this
# installs, Depends is not doing its job and the runtime guard of issue #220 is
# the only thing left between a user and a libphp from another minor.
if apt-get install -y -qq /out/wrong-minor.deb >/dev/null 2>&1; then
    echo "FAIL: a package depending on another PHP minor installed anyway" >&2
    exit 1
fi
echo "ok: a package built for another PHP minor is refused by apt"

useradd -m -u 1001 tester 2>/dev/null || true
EOF
    ;;
apk)
    cat >> "$OUT/stage2.sh" <<'EOF'
# See the Debian branch: binutils, patch and the CLI are the test rig, not package
# dependencies.
apk add --no-cache --upgrade binutils patch php85 php85-openssl php85-phar openssl >/dev/null
apk add --no-cache --allow-untrusted "$(find /out/repo -name "$PKGNAME-[0-9]*.apk" | head -1)"
apk info -d "$PKGNAME"
LICENSE_FILE=/usr/share/licenses/$PKGNAME/LICENSE
ldd /usr/sbin/php-fpm-ng | grep libphp
PHP_CLI=/usr/bin/php85
# Alpine ships every PHP extension as a shared module loaded from
# /etc/php85/conf.d, and build/run-fpmng-phpt.sh drives run-tests.php with -n,
# which reads no ini file at all. Ubuntu compiles openssl into its CLI, so four
# tests that only need it on the CLI side -- the three ACME ones and the TLS
# one -- would otherwise skip here with "requires the openssl extension", which
# is a statement about the test rig and not about the package. Loaded by hand
# through TEST_PHP_ARGS, which run-tests.php passes on to every test and every
# SKIPIF.
#
# session is deliberately NOT loaded the same way. It is needed INSIDE the pool,
# not on the CLI side, and the FPM binary is started by the tester with -n as
# well -- so loading it here would only make the SKIPIF say yes on behalf of a
# process that then does not have it, turning a legitimate skip into a failure
# about session_id() being undefined. The two tests that need it skip on Alpine
# instead, which is what the expected count above says.
export TEST_PHP_ARGS="-d extension=openssl"

if apk add --no-cache --allow-untrusted "$(find /out/wrong-minor -name '*.apk' | head -1)" >/dev/null 2>&1; then
    echo "FAIL: a package depending on another PHP minor installed anyway" >&2
    exit 1
fi
echo "ok: a package built for another PHP minor is refused by apk"

adduser -D -u 1001 tester 2>/dev/null || true
EOF
    ;;
esac

cat >> "$OUT/stage2.sh" <<'EOF'

# The license texts (issue #581): MIT, PHP License 3.01 and BSD-2-Clause are all
# notice-on-redistribution licenses, so the installed package must carry them.
# Compared with a fresh run of the script that assembled them, and the three
# texts are also looked for by a line that occurs only in that text's body (not
# in a section header and not in another license), so an empty or truncated
# source cannot make the comparison pass.
/repo/build/package-licenses.sh > /tmp/expected-license
cmp /tmp/expected-license "$LICENSE_FILE" || { echo "FAIL: $LICENSE_FILE differs from build/package-licenses.sh" >&2; exit 1; }
for t in 'Permission is hereby granted, free of charge' 'The PHP Group may publish revised' \
         'Copyright (c) 2007-2009, Andrei Nigmatulin' 'PROVIDED BY AUTHOR AND CONTRIBUTORS'; do
    grep -qF "$t" "$LICENSE_FILE" || { echo "FAIL: $LICENSE_FILE has no '$t'" >&2; exit 1; }
done
echo "ok: $LICENSE_FILE carries the MIT, PHP-3.01 and BSD-2-Clause texts"

# The build stage and this stage install PHP from the distribution on their
# own. If the SDK it built against and the libphp installed here are different
# patch releases, every start prints the guard's notice and a dozen tests fail
# on it; say that now, with both versions, rather than as a test diff (issue #557).
/repo/build/check-libphp-skew.sh /usr/sbin/php-fpm-ng

# php-fpm refuses to run as root, and a suite that cannot start a pool reports
# every test as SKIP and still exits 0. So the suite runs as an unprivileged
# user, and the name comparison on the host (build/package-gate-compare.sh) is what notices if that ever stops
# being true.
mkdir -p /out/results /out/work
rm -rf /out/work/prepared
cp -r /out/prepared /out/work/prepared
chown -R 1001:1001 /out/results /out/work

set +e
su tester -c "TEST_PHP_ARGS='${TEST_PHP_ARGS:-}' \
    TEST_PHP_EXECUTABLE=$PHP_CLI \
    TEST_PHP_FPM_EXECUTABLE=/usr/sbin/php-fpm-ng \
    TEST_FPM_TIMEOUT=120 \
    /repo/build/run-fpmng-phpt.sh /out/work/prepared /out/results"
echo "suite exit: $?"
set -e
EOF

docker run --rm \
    -v "$REPO:/repo:ro" -v "$OUT:/out" \
    "$TEST_IMAGE" sh /out/stage2.sh
docker run --rm -v "$OUT:/out" "$TEST_IMAGE" chown -R "$(id -u):$(id -g)" /out

# ---------------------------------------------------------------------------
# The assertion. Read on the host, out of the file the runner wrote, so that a
# stage 2 that died before writing it fails here rather than passing silently.
# ---------------------------------------------------------------------------
SUMMARY=$OUT/results/summary.txt
[ -f "$SUMMARY" ] || fail "the suite produced no $SUMMARY -- stage 2 did not get far enough to measure anything"
echo "--- $SUMMARY"
cat "$SUMMARY"

grep -q '^measurement_status=MEASURED$' "$SUMMARY" \
    || fail "the run did not measure every selected test; see $OUT/results/run.log"

get() { sed -n "s/^$1=\([0-9]*\)\$/\1/p" "$SUMMARY"; }
GOT_PASS=$(get PASS); GOT_FAIL=$(get 'FAIL\/ERROR'); GOT_SKIP=$(get SKIP); GOT_TOTAL=$(get TOTAL)
GOT_WARN=$(get WARN)
GOT_XFAIL=$(get XFAIL)

# Issue #301. run-tests.php has a third outcome between PASS and FAIL: a test
# that failed once and passed when it was retried is reported as WARNED. It is
# a pass here (the package did the job), and the retried tests are named, so a
# flake is visible on every run, green or red. One that keeps appearing here
# deserves an issue of its own.
if [ "${GOT_WARN:-0}" -gt 0 ]; then
    echo "ci-package-gate.sh: NOTICE: $GOT_WARN test(s) failed once and passed on retry."
    echo "  Counted as passes (issue #301):"
    awk -F '\t' 'NR > 1 && $2 == "WARN" {print "    " $1}' "$OUT/results/results.tsv" 2>/dev/null \
        || echo "    (no per-test rows; see $OUT/results/run.log)"
fi

# Issue #678: score by name, not by count. A new test that passes needs no edit
# here; one that skips, or a skip that starts passing, is named.
"$REPO/build/package-gate-compare.sh" "$CELL" "$REPO/build/package-gate-expected.txt" \
    "$OUT/results/results.tsv" "$REPO/sapi/fpmng/tests" \
    || fail "see $OUT/results/results.tsv for the per-test results"

echo "ci-package-gate.sh: PASS ($CELL package installed on a machine with no compiler, suite PASS=$GOT_PASS WARN=$GOT_WARN FAIL=$GOT_FAIL SKIP=$GOT_SKIP XFAIL=${GOT_XFAIL:-0} of $GOT_TOTAL)"
