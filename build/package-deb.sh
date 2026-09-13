#!/bin/sh
# Build the php-fpm-ng .deb: the binary from build/libphp-build.sh, a
# configuration a user can start from, and a systemd unit (issue #221).
#
# The package deliberately does NOT contain a PHP. It depends on the
# distribution's embed package for the libphp it was linked against, so that
# `apt install libphp8.5-embed && apt install ./php-fpm-ng_*.deb` is the whole
# installation and no compiler is needed on the target.
#
# The dependency is on a SPECIFIC minor: libphp8.5-embed, not "any libphp".
# That is not caution, it is the same boundary the binary enforces at runtime
# (issue #220, fpmng_libphp_abi_check). The binary carries the struct layouts
# of the version it was built against, and the embed packages all export
# SONAME libphp.so, so nothing in the dynamic linker would stop 8.4's library
# from satisfying a binary built for 8.5. Expressing it to the package manager
# moves that failure from "starts and corrupts memory" to "apt refuses to
# install", which is where a user can do something about it.
#
# Usage: build/package-deb.sh <binary> [outdir]
#   binary   php-fpm-ng from build/libphp-build.sh
#   outdir   where the .deb lands, default ./out-deb
#   PHP_CONFIG  php-config of the same PHP the binary was built against
set -eu

fail() { echo "package-deb.sh: FAIL: $*" >&2; exit 1; }

REPO=$(cd "$(dirname "$0")/.." && pwd)
BIN=${1:?usage: build/package-deb.sh <binary> [outdir]}
OUT=${2:-$PWD/out-deb}
[ -x "$BIN" ] || fail "$BIN is not an executable"

command -v dpkg-deb >/dev/null || fail "dpkg-deb is not installed"

# The package exists to ship a DYNAMICALLY linked binary. A static one would
# mean the libphp dependency below is decoration and the user is running a PHP
# nobody in the distribution is patching. Checked before anything is built, so
# the wrong input never produces a .deb at all.
objdump -p "$BIN" | grep -q 'NEEDED *libphp\.so' \
    || fail "$BIN does not link libphp.so; this package is only for the build/libphp-build.sh output"

# The PHP minor this binary was built for. Not necessarily on the first line:
# issue #220's ABI guard prints its patch-level notice before the banner.
# Asked of the binary rather than of the machine building the package: a build host with two PHPs installed would
# otherwise produce a .deb whose Depends names a version its own binary was not
# linked against, and the mismatch would only surface on a user's machine.
PHP_VERSION=$("$BIN" -n -v 2>/dev/null | sed -n 's/^PHP \([0-9]*\.[0-9]*\)\..*/\1/p' | head -1)
[ -n "$PHP_VERSION" ] || fail "$BIN -v printed no version banner. Most often this host is
  missing a library the binary links -- it does not run, so it cannot be asked. Try:
$(ldd "$BIN" 2>&1 | grep -i 'not found' || echo '  (ldd found everything; run the binary by hand to see why)')"

# WHICH OF THE TWO PACKAGES THIS IS (issue #294). The repository builds the
# default one, which carries no TLS and no ACME (issues #280, #281), and
# php-fpm-ng-tls, built --enable-fpmng-tls --enable-fpmng-acme. They install the
# same paths and the same binary name, so they cannot be co-installed, and
# nothing in the file name would tell them apart unless this script puts it
# there.
#
# Asked of the BINARY, not of the environment. FPMNG_TLS is read two scripts
# earlier, by build/libphp-build.sh, and a package that named itself from the
# variable rather than from what the variable produced would be a claim nothing
# downstream ever checks. These are the same two symbols libphp-build.sh asserts
# on after the link, for the same reason.
command -v nm >/dev/null || fail "nm is not installed (it is in binutils, like objdump above)"
has_symbol() { nm --defined-only "$BIN" 2>/dev/null | grep -qw -- "$1"; }

HAS_TLS=0; has_symbol fpm_tls_http_validate && HAS_TLS=1
HAS_ACME=0; has_symbol fpm_acme_challenge_init_main && HAS_ACME=1

# Only two combinations have a name. A binary with TLS but no ACME is a
# perfectly good thing to build by hand and not a thing this repository
# publishes, so it is refused here rather than shipped under one of the two
# names, where its description would be wrong about it.
case "$HAS_TLS$HAS_ACME" in
00) PKGNAME=php-fpm-ng ;;
11) PKGNAME=php-fpm-ng-tls ;;
*)  fail "this binary has TLS=$HAS_TLS ACME=$HAS_ACME, which is neither of the two
  packages this repository publishes (issue #294): the default one is built with
  neither, php-fpm-ng-tls with both (FPMNG_TLS=1 FPMNG_ACME=1)." ;;
esac

# php-fpm-ng's own version. The repository has no VERSION file, so the release
# is the short commit and the upstream version is PHP's -- which is the honest
# description of an artefact that is a SAPI for exactly one PHP minor.
#
# FPMNG_RELEASE replaces the commit when there is a tag to name instead
# (issue #223: the release workflow sets it). Both halves stay in the version
# either way, because a user with two PHP minors installed has to be able to
# tell two of these files apart before installing either one.
COMMIT=$(cd "$REPO" && git rev-parse --short HEAD 2>/dev/null || echo unknown)
RELEASE=${FPMNG_RELEASE:-$COMMIT}
PHP_FULL=$("$BIN" -n -v 2>/dev/null | sed -n 's/^PHP \([0-9.]*\).*/\1/p' | head -1)
VERSION="${PHP_FULL}-1~$RELEASE"

ARCH=$(dpkg --print-architecture)

# The runtime libraries the binary actually needs, resolved to the packages
# that own them, so the dependency list cannot drift from the link line.
# libphp.so is excluded and named explicitly above: it is the one NEEDED entry
# whose SONAME does not identify the version we require.
DEPS=$(objdump -p "$BIN" | awk '/NEEDED/ {print $2}' | grep -v '^libphp\.so$' | while read -r soname; do
    path=$(ldconfig -p | awk -v s="$soname" '$1 == s {print $NF; exit}')
    # A soname this host cannot resolve would silently drop out of Depends and
    # produce a package that installs and then fails to start on the user's
    # machine. The build host must have every library the binary links.
    [ -n "$path" ] || { echo "unresolved:$soname"; continue; }
    owner=$(dpkg -S "$path" 2>/dev/null | cut -d: -f1)
    [ -n "$owner" ] || { echo "unowned:$path"; continue; }
    echo "$owner"
done | sort -u)

case $DEPS in
*unresolved:*|*unowned:*)
    fail "these libraries the binary links are not installed here, or belong to no package, so Depends would be incomplete:
$(echo "$DEPS" | grep -E '^(unresolved|unowned):')" ;;
esac
DEPS=$(echo "$DEPS" | tr '\n' ',' | sed 's/,$//;s/,/, /g')

ROOT=$OUT/root
rm -rf "$ROOT"
mkdir -p "$ROOT/DEBIAN" "$ROOT/usr/sbin" "$ROOT/etc/php-fpm-ng/pool.d" \
         "$ROOT/lib/systemd/system" "$ROOT/var/log/php-fpm-ng" \
         "$ROOT/usr/share/doc/$PKGNAME"

install -m 0755 "$BIN" "$ROOT/usr/sbin/php-fpm-ng"
install -m 0644 "$REPO/packaging/deb/php-fpm-ng.conf" "$ROOT/etc/php-fpm-ng/php-fpm-ng.conf"
install -m 0644 "$REPO/packaging/deb/www.conf" "$ROOT/etc/php-fpm-ng/pool.d/www.conf"
install -m 0644 "$REPO/packaging/deb/php-fpm-ng.service" "$ROOT/lib/systemd/system/php-fpm-ng.service"

# The relationship between the two packages (issue #294). Conflicts, because
# both own /usr/sbin/php-fpm-ng and /etc/php-fpm-ng; Replaces, so that dpkg
# accepts the file takeover when one is installed over the other; and both
# Provide the virtual php-fpm-ng-any, which is the name to depend on when
# either will do -- neither can Provide "php-fpm-ng", because that is the real
# name of one of them. Declared by both sides of the pair: dpkg enforces a
# Conflicts whichever package states it, and a reader of either control file
# should be able to see that the other one exists.
if [ "$PKGNAME" = php-fpm-ng-tls ]; then
    OTHER=php-fpm-ng
    SUMMARY="FPM process manager with HTTP-direct pools and TLS termination (beta)"
    EXTRA=" .
 This is the TLS build: --enable-fpmng-tls and --enable-fpmng-acme are compiled
 in, so http.tls_cert, http.tls_key and ACME certificate issuance work. Both are
 BETA and neither has been audited. They terminate TLS on a network-facing
 socket; run them where you would run other beta software, and read the support
 tiers table in /usr/share/doc/$PKGNAME/README.md before you do.
 .
 The default php-fpm-ng package is built without either and is the one to
 install if you terminate TLS somewhere else. The two cannot be co-installed:
 they are the same binary at the same path, built differently."
else
    OTHER=php-fpm-ng-tls
    SUMMARY="FPM process manager with HTTP-direct pools, on the distribution PHP"
    EXTRA=" .
 This build has no TLS termination and no ACME (issues #280, #281): http.tls_cert
 is refused at startup. The php-fpm-ng-tls package is the same release built with
 --enable-fpmng-tls --enable-fpmng-acme; the two cannot be co-installed."
fi

cat > "$ROOT/DEBIAN/control" <<EOT
Package: $PKGNAME
Version: $VERSION
Architecture: $ARCH
Maintainer: php-fpm-ng maintainers <https://github.com/crazy-goat/php-fpm-ng>
Depends: libphp$PHP_VERSION-embed, ${DEPS:-libc6}
Conflicts: $OTHER
Replaces: $OTHER
Provides: php-fpm-ng-any
Section: web
Priority: optional
Homepage: https://github.com/crazy-goat/php-fpm-ng
Description: $SUMMARY
 php-fpm-ng is a fork of PHP's FPM SAPI. This package links against the
 distribution's libphp$PHP_VERSION rather than embedding a PHP of its own, so
 installing it compiles nothing.
 .
 Two pool types are supported here: pool.type = fastcgi, which is classic FPM,
 and pool.type = http-direct, in which the worker speaks HTTP itself and needs
 no web server in front of it. pool.type = fastcgi-ng and pool.type = http are
 refused at startup: they need a patch inside Zend/ that a distribution libphp
 does not carry, and running them without it would change signal behaviour
 silently. Build from source for those.
$EXTRA
EOT

# Both configuration files are conffiles: dpkg then asks before replacing a
# file the administrator has edited, instead of overwriting it on upgrade.
cat > "$ROOT/DEBIAN/conffiles" <<'EOT'
/etc/php-fpm-ng/php-fpm-ng.conf
/etc/php-fpm-ng/pool.d/www.conf
EOT

cat > "$ROOT/DEBIAN/postinst" <<'EOT'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
    # The shipped pool runs as www-data. It comes from the base system on
    # Debian and Ubuntu, but the package does not depend on anything that
    # guarantees it, so create it rather than fail at first start.
    if ! getent passwd www-data >/dev/null; then
        adduser --system --group --no-create-home --home /nonexistent www-data || true
    fi
    chown root:adm /var/log/php-fpm-ng 2>/dev/null || true
    chmod 0750 /var/log/php-fpm-ng 2>/dev/null || true

    # Built without debhelper, so the unit is registered here rather than by a
    # dh_installsystemd snippet. Not started: an install that seizes port 80 or
    # a socket a running FPM already owns is a surprise, and the shipped pool
    # is a starting point to edit, not a deployment.
    if [ -d /run/systemd/system ]; then
        systemctl daemon-reload >/dev/null 2>&1 || true
        systemctl enable php-fpm-ng.service >/dev/null 2>&1 || true
    fi
fi
EOT

cat > "$ROOT/DEBIAN/prerm" <<'EOT'
#!/bin/sh
set -e
if [ "$1" = remove ] || [ "$1" = deconfigure ]; then
    if [ -d /run/systemd/system ]; then
        deb-systemd-invoke stop php-fpm-ng.service >/dev/null 2>&1 \
            || systemctl stop php-fpm-ng.service >/dev/null 2>&1 || true
    fi
fi
EOT

cat > "$ROOT/DEBIAN/postrm" <<'EOT'
#!/bin/sh
set -e
# The unit's RuntimeDirectory= is removed by systemd when the service stops,
# but a master started by hand leaves its socket behind. Acceptance criterion
# of issue #221: apt remove leaves no orphaned socket.
rm -rf /run/php-fpm-ng
if [ "$1" = purge ]; then
    rm -rf /etc/php-fpm-ng /var/log/php-fpm-ng
fi
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi
EOT

chmod 0755 "$ROOT/DEBIAN/postinst" "$ROOT/DEBIAN/prerm" "$ROOT/DEBIAN/postrm"

cp "$REPO/README.md" "$ROOT/usr/share/doc/$PKGNAME/README.md"

DEB=$OUT/${PKGNAME}_${VERSION}_${ARCH}.deb
dpkg-deb --build --root-owner-group "$ROOT" "$DEB" >/dev/null

echo "package-deb.sh: PASS ($DEB, depends on libphp$PHP_VERSION-embed)"
