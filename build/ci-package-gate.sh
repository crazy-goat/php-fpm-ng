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
# WHAT IT ASSERTS, AND WHY IT IS A NUMBER. The standing trap in this repository
# is that a misconfigured run skips every test and still reports success: as
# root php-fpm refuses to start, so the suite reports PASS=0 with everything
# skipped and exits 0. So the gate is the COUNT, and it is exact in both
# directions -- a run that gains skips fails here just as a run that loses
# passes does. The expected numbers are the ones build/libphp-build.sh records
# for this binary (issue #230 is what makes the 32 skips legitimate: those
# tests ask the binary whether it supports their pool type and skip when it
# says no, instead of failing).
#
# Usage: build/ci-package-gate.sh deb|apk <prepared-php-src> <outdir>
#   prepared-php-src  a tree with our overlay applied (build/prepare.sh)
#   outdir            package, results and logs land here
#
# Runs on the host, not in a container: it drives `docker run` with bind
# mounts, and -v paths are resolved by the host daemon.
set -eu

fail() { echo "ci-package-gate.sh: FAIL: $*" >&2; exit 1; }

FLAVOUR=${1:?usage: build/ci-package-gate.sh deb|apk <prepared-php-src> <outdir>}
SRC=${2:?usage: build/ci-package-gate.sh deb|apk <prepared-php-src> <outdir>}
OUT=${3:?usage: build/ci-package-gate.sh deb|apk <prepared-php-src> <outdir>}
REPO=$(cd "$(dirname "$0")/.." && pwd)
SRC=$(cd "$SRC" && pwd) || fail "no prepared php-src tree at $2"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

[ -f "$SRC/sapi/fpmng/config.m4" ] || fail "$SRC has no sapi/fpmng: run build/prepare.sh first"

# What the packages call themselves. Resolved here, on the host, because the
# containers below mount the repository read-only and have no git in them -- so
# the package scripts' own fallback would land on "unknown" every single run.
# The release workflow (issue #223) passes a tag in; otherwise it is the commit.
RELEASE=${FPMNG_RELEASE:-$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)}
command -v docker >/dev/null || fail "docker is not available; this script drives containers"

# The expected score, per distribution. 32 of the 77 owned tests skip on either
# of them for the same reason: their pool type needs patches/0006 inside Zend/,
# which a distribution libphp does not carry, so they ask the binary and skip
# (issue #230) instead of failing. That number is a property of this build path
# and is the same everywhere.
#
# It moved from 31 of 72 with the per-pool operator endpoint (issue #274), which
# added four tests: three exercise cron, supervisor and http-direct pools and run
# here, and fpmng-operator-endpoint-http.phpt needs pool.type = http and joins
# the skips.
#
# It moved to 77 with the per-pool metrics path (issue #276), which added
# fpmng-metrics-per-pool.phpt. That one uses http-direct pools on purpose, so it
# runs on both distributions rather than joining the skips: the filter it tests
# is the same filter on either build path.
#
# The two distributions differ by two more tests, and the difference is in how
# they package PHP, not in what we ship. Ubuntu compiles session into its CLI
# and its libphp; Alpine ships every extension as a shared module loaded from
# /etc/php85/conf.d, and both the CLI and the FPM binary are started with -n by
# the suite, which reads no ini file at all. So on Alpine the two tests that
# need session inside the pool -- fpmng-http-direct-session-status.phpt and
# fpmng-http-direct-worker-buffered-streams.phpt -- skip.
#
# It moved to 79 with the baseline counters (issue #277), which added
# fpmng-baseline-counters.phpt and fpmng-baseline-counters-cron.phpt. Both run
# everywhere: one uses an http-direct pool and a supervisor pool, the other a
# cron pool, and none of those depends on how the distribution packages PHP.
#
# It moved to 80 with the supervisor fast-restart warning (issue #122), which
# added fpmng-supervisor-fast-restart.phpt. A supervisor pool running a script
# that does nothing needs no extension from either distribution, so it runs on
# both.
#
# Written out here rather than read from anywhere, so that a change in the
# suite has to be a change in this file too, made by someone who looked at why
# the number moved.
# Issue #280 moved TLS termination behind --enable-fpmng-tls (FPMNG_TLS=1 for
# build/libphp-build.sh), and the packages are built WITHOUT it. The two
# http-direct TLS tests that used to run here now skip on both flavours, which
# is the regression against v0.2.0 that #279 decided to take; the numbers below
# moved by exactly that.
# Issue #281 did the same for ACME (FPMNG_ACME=1), and the packages are built
# without that too -- which moved the numbers by exactly ONE test, measured on
# a real gate run rather than reasoned about. The ACME tests that need a pool
# (the challenge pair, issuance, renew-failure, handover) were already skipping
# here, and for an older reason: they use pool.type = http, which a binary
# linked against a distribution libphp refuses, and that check comes first in
# their SKIPIF. The four that drive sapi/fpmng/acme/*.php through the CLI
# (jose, state, renew-policy, single-renewer) keep running: they read the
# scripts out of the source tree and never ask the binary anything, so a
# package built without ACME is not evidence about them.
# The one that moved is fpmng-payload-distribution: the payload IS the ACME
# client, so build/embed-payload.sh embeds nothing when FPMNG_ACME=0 and the
# test skips instead of asserting on a payload this package deliberately has
# no reason to carry.
EXPECT_FAIL=0
EXPECT_TOTAL=80

case "$FLAVOUR" in
deb)
    IMAGE=ubuntu:26.04
    EXPECT_PASS=45
    EXPECT_SKIP=35
    # binutils for objdump (package-deb.sh resolves NEEDED sonames with it),
    # php8.5-dev for the headers libphp-build.sh compiles against, the embed
    # package for the library it links, and libevent/libacl for what the SAPI
    # itself needs -- no libevent_openssl, because the package is built without
    # TLS (issue #280) and this stage should not be able to link it by accident.
    # dpkg-deb is in dpkg, which is already there -- dpkg-dev is
    # deliberately NOT installed: it pulls in gcc, and a build stage is allowed
    # a compiler but this keeps the two package sets honest about who needs one.
    BUILD_SETUP='export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq binutils php8.5-dev libphp8.5-embed \
            libevent-dev libacl1-dev >/dev/null'
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
    EXPECT_PASS=43
    EXPECT_SKIP=37
    # No openssl-dev: the package is built without TLS (issue #280), so the
    # build stage does not get the headers that would let it link OpenSSL even
    # by accident. libphp-build.sh asserts the produced binary's dynamic
    # section, which is the evidence; this is the belt.
    BUILD_SETUP='apk add --no-cache alpine-sdk php85-dev php85-embed \
            libevent-dev acl-dev >/dev/null'
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
echo "ci-package-gate.sh: build stage in $BUILD_IMAGE, install stage in $TEST_IMAGE"

# ---------------------------------------------------------------------------
# Stage 1: the build host. It has the compiler and the headers; nothing it
# leaves behind is used by stage 2 except the package itself and the test
# fixtures staged below.
# ---------------------------------------------------------------------------
echo "ci-package-gate.sh: stage 1 -- build the binary and the $FLAVOUR package"
cat > "$OUT/stage1-payload.sh" <<EOF
set -eu
FPMNG_RELEASE=$RELEASE
export FPMNG_RELEASE
/repo/build/libphp-build.sh /src /out
$PACKAGE_CMD
$NEGATIVE_CONTROL

# The fixtures stage 2 needs to run the suite, staged the same way the build
# job stages them for fpmng-phpt: run-tests.php next to the tests, plus the two
# source-tree files individual tests reach for by relative path. Copied out of
# the prepared tree here because stage 2 has no php-src and must not have one
# -- it is standing in for a user's machine.
rm -rf /out/prepared
mkdir -p /out/prepared/sapi/fpmng /out/prepared/ext/standard/tests/misc
cp -r /src/sapi/fpmng/tests /out/prepared/sapi/fpmng/tests
cp -r /src/sapi/fpmng/acme /out/prepared/sapi/fpmng/acme
cp /src/run-tests.php /out/prepared/
cp /src/ext/standard/tests/misc/browscap.ini /out/prepared/ext/standard/tests/misc/
EOF
cat > "$OUT/stage1.sh" <<EOF
set -eu
$BUILD_SETUP
$RUN_PAYLOAD
EOF
docker run --rm \
    -v "$SRC:/src:ro" -v "$REPO:/repo:ro" -v "$OUT:/out" \
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
cat > "$OUT/stage2.sh" <<'EOF'
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
# binary under test; php8.5-cli is the harness that runs run-tests.php; openssl
# is not the library (that comes with the CLI) but its configuration file --
# without /usr/lib/ssl/openssl.cnf openssl_pkey_new() fails with a bare
# "No such file or directory" and the three ACME tests fail for a reason that
# has nothing to do with the package. None of the three is a dependency of what
# we ship: they belong to the test rig, and naming them here keeps that visible.
apt-get install -y -qq binutils php8.5-cli openssl >/dev/null
apt-get install -y -qq /out/php-fpm-ng_*.deb
dpkg -s php-fpm-ng | grep -E '^(Package|Version|Depends):'
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
# See the Debian branch: binutils and the CLI are the test rig, not package
# dependencies.
apk add --no-cache binutils php85 php85-openssl openssl >/dev/null
apk add --no-cache --allow-untrusted "$(find /out/repo -name 'php-fpm-ng-*.apk' | head -1)"
apk info -d php-fpm-ng
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

# php-fpm refuses to run as root, and a suite that cannot start a pool reports
# every test as SKIP and still exits 0. So the suite runs as an unprivileged
# user, and the count assertion on the host is what notices if that ever stops
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

if [ "$GOT_PASS" != "$EXPECT_PASS" ] || [ "$GOT_FAIL" != "$EXPECT_FAIL" ] \
   || [ "$GOT_SKIP" != "$EXPECT_SKIP" ] || [ "$GOT_TOTAL" != "$EXPECT_TOTAL" ]; then
    fail "the packaged binary scored PASS=$GOT_PASS FAIL=$GOT_FAIL SKIP=$GOT_SKIP of $GOT_TOTAL,
  expected PASS=$EXPECT_PASS FAIL=$EXPECT_FAIL SKIP=$EXPECT_SKIP of $EXPECT_TOTAL.
  A number that moved in either direction is a finding: more skips usually means
  the pool stopped starting, more passes means these expectations are stale.
  Per-test results are in $OUT/results/results.tsv."
fi

echo "ci-package-gate.sh: PASS ($FLAVOUR package installed on a machine with no compiler, suite PASS=$GOT_PASS FAIL=$GOT_FAIL SKIP=$GOT_SKIP of $GOT_TOTAL)"
