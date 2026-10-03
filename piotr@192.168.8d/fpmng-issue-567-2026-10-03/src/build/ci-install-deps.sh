#!/bin/sh
# Acquire the dependencies of one CI job, in the job (issue #424).
#
#   build/ci-install-deps.sh build|test|lint
#
# This is the whole of the "prebuilt PHP SDK contract" on the CI side: the jobs
# of .github/workflows/build-matrix.yml run in a stock ubuntu:26.04 container
# and install the distribution's PHP 8.5 SDK from the official Ubuntu archive
# with apt. Nothing fetches php-src and nothing compiles the engine. The one
# thing each job prints is what it got, so the log answers "which PHP was this
# run built against" without a separate artefact.
#
# WHY IN-JOB AND NOT A CI IMAGE. The image would be the same package list baked
# into a layer, but ci-image.yml does not push on a pull_request, so a change to
# the package list could only be tried after it merged -- and it would need a
# registry pull (and credentials) in every job. Installing costs about a minute
# of apt per job and keeps the dependency list reviewable in the diff that
# changes it. .github/docker/ci.Dockerfile was removed for the same reason.
#
# Runs as root: apt needs it. The job's other steps do not (see
# `defaults.run.shell` in build-matrix.yml): php-fpm refuses to start as root,
# so a root job reports every test SKIP and still goes green. This script
# therefore ends by creating the unprivileged account the suites run as and
# handing it the workspace.
#
# Roles:
#   build  headers, libphp, the PHP CLI, libevent/OpenSSL/libacl headers and a
#          compiler -- everything build/libphp-build.sh needs
#   test   the same set. The binary is dynamically linked against libphp,
#          libevent and OpenSSL, and the suites need the CLI, strings(1) and
#          openssl.cnf; the -dev packages carry the runtime libraries, so one
#          list serves, and the install-side claim ("a package pulls in only its
#          own dependencies") is the package gate's job, not this one's
#   lint   build, plus clang-tidy
#
# Extras that belong to a single job are named by that job, not here:
#   FPMNG_CI_EXTRA_PACKAGES   extra apt packages (valgrind, locales, ...)
set -eu

fail() { echo "ci-install-deps.sh: FAIL: $*" >&2; exit 1; }

ROLE=${1:?usage: build/ci-install-deps.sh build|test|lint}
[ "$(id -u)" = 0 ] || fail "must run as root (apt-get); the later steps drop to the ci account"

# php8.5-dev: headers and php-config8.5. libphp8.5-embed: the library the binary
# links. php8.5-cli: the payload packer and the test runner's interpreter.
# binutils: strings/nm/objdump. curl, openssl, ca-certificates: the cancel
# script, the TLS tests and the ACME tests. git: actions/checkout needs it to
# leave a .git, and check-owned-warnings.sh and the vendor check read it.
PKGS="build-essential binutils git ca-certificates curl openssl
  php8.5-dev php8.5-cli libphp8.5-embed
  libevent-dev libssl-dev libacl1-dev"
case "$ROLE" in
build|test) ;;
lint) PKGS="$PKGS clang-tidy" ;;
*) fail "unknown role: $ROLE (expected build, test or lint)" ;;
esac
PKGS="$PKGS ${FPMNG_CI_EXTRA_PACKAGES:-}"

export DEBIAN_FRONTEND=noninteractive
echo "ci-install-deps.sh: role=$ROLE on $(. /etc/os-release && echo "$PRETTY_NAME") $(uname -m)"
echo "ci-install-deps.sh: apt packages: $(echo $PKGS)"
set -x
apt-get update -qq
# shellcheck disable=SC2086
apt-get install -y -qq --no-install-recommends $PKGS >/dev/null
set +x

# What was actually installed, from dpkg and from the SDK itself.
echo "ci-install-deps.sh: resolved versions"
dpkg-query -W -f='  ${Package} ${Version}\n' php8.5-dev php8.5-cli libphp8.5-embed libevent-dev libssl-dev
echo "ci-install-deps.sh: php-config8.5 -> PHP $(php-config8.5 --version), $(php-config8.5 --include-dir)"
for t in gcc php8.5; do
  command -v "$t" >/dev/null || fail "$t is not on PATH after the install"
done
# The contract is NTS: build/libphp-build.sh refuses ZTS, so say which this is.
php8.5 -r 'echo "ci-install-deps.sh: ", PHP_VERSION, " ", PHP_ZTS ? "ZTS" : "NTS", "\n";'
# The account the suites run as. uid 1001 is the `runner` account that owns the
# workspace on a hosted runner, so files written there are readable by the host
# afterwards, as the old `--user 1001:1001` container option arranged.
if ! getent passwd 1001 >/dev/null; then
  useradd -m -u 1001 -s /bin/bash ci
fi
# actions/checkout ran as root and left root-owned files; the suites write
# into the workspace.
if [ -n "${GITHUB_WORKSPACE:-}" ] && [ -d "$GITHUB_WORKSPACE" ]; then
  chown -R 1001:1001 "$GITHUB_WORKSPACE"
fi
# The custom shell of the later steps runs the runner's generated script file
# as that account, so the directory holding it has to be traversable.
[ -z "${RUNNER_TEMP:-}" ] || chmod a+rx "$RUNNER_TEMP"
git config --system --add safe.directory '*'
echo "ci-install-deps.sh: done"
