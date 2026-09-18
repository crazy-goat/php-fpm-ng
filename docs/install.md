# Installing php-fpm-ng

php-fpm-ng ships as a package that contains **no PHP**. It is one binary, a
configuration file and a service file; the interpreter it runs on is the
distribution's own `libphp`, which arrives as a dependency. There is no
compiler on the target and no php-src to build.

That is possible for two of the pool types and not for the others. The table
under [What the packaged build supports](#what-the-packaged-build-supports)
says which, and names the mechanism in each case rather than saying "not
supported" and leaving the next reader to guess.

Every command below was run on 2026-09-12 against v0.2.0, in the image it
names, in the order printed, as `root` (so no `sudo`; add it if you are not
root). The exceptions
are called out where they appear: `systemctl` needs an init system a container
does not have, and says so on the spot.

## Getting the files

The repository is public and the release assets download anonymously -- no
GitHub account, no token:

```sh
curl -fLO https://github.com/crazy-goat/php-fpm-ng/releases/latest/download/SHA256SUMS
curl -fLO https://github.com/crazy-goat/php-fpm-ng/releases/latest/download/php-fpm-ng_v0.2.0_php8.5_amd64.deb
```

`latest/download/` redirects to whatever the newest release is, so the file
name in the second line has to match that release -- it is the version, not a
placeholder. Checked on 2026-09-12 against v0.2.0 with the credentials removed
from the environment: HTTP 200, and the SHA256SUMS body lists the .deb and the
.apk of that release.

With a GitHub account this is the shorter equivalent:

```sh
gh release download v0.2.0 --repo crazy-goat/php-fpm-ng
```

Either way you end up with `SHA256SUMS` and the package next to it, which is
what the rest of this page assumes.

## Debian / Ubuntu

Image: `ubuntu:26.04`.

```sh
sha256sum -c --ignore-missing SHA256SUMS
apt install -y ./php-fpm-ng_v0.2.0_php8.5_amd64.deb
```

`apt` pulls `libphp8.5-embed` in as a dependency; that is the PHP the binary
runs on. The packages are unsigned on purpose -- they are files you fetched and
install by path, not a repository your machine trusts for every future upgrade
-- so the checksum step is part of the install, not an optional extra.

```console
# dpkg -l php-fpm-ng | tail -1
ii  php-fpm-ng  8.5.4-1~v0.2.0  amd64  FPM process manager with HTTP-direct pools, on the distribution PHP
# php-fpm-ng -t -y /etc/php-fpm-ng/php-fpm-ng.conf
NOTICE: configuration file /etc/php-fpm-ng/php-fpm-ng.conf test is successful
```

The pool lives in `/etc/php-fpm-ng/pool.d/www.conf` and listens on
`/run/php-fpm-ng/www.sock` as `www-data`. The service is installed but **not
started** by the install: an install that seizes a port is a surprise. Edit the
pool, then:

```sh
systemctl start php-fpm-ng
systemctl status php-fpm-ng
```

Those two are the only commands on this page that were not run in the image
above -- a container has no init system. What was checked there instead is that
`systemd-analyze verify` accepts the unit, that it is `Type=exec` (the packaged
binary is built without systemd notification support, so `Type=notify` would
hang), and that its `ExecStart` line starts a master which creates the socket
and answers a FastCGI request.

## Alpine

Image: `alpine:edge`.

```sh
grep php-fpm-ng-v0.2.0-php8.5-x86_64.apk SHA256SUMS | sha256sum -c -
apk add --allow-untrusted ./php-fpm-ng-v0.2.0-php8.5-x86_64.apk
```

The checksum line is spelled differently here because BusyBox `sha256sum` has
no `--ignore-missing`: it would try to verify the `.deb` too and fail on a file
you did not download. Piping the one line you care about is the same check.

`--allow-untrusted` is not a workaround being tolerated: the package is
unsigned by decision, the checksum above is what authenticates it, and apk is
being told the truth about what it is being handed.

```console
# apk info -v php-fpm-ng
php-fpm-ng: FPM process manager with HTTP-direct pools, on the distribution PHP (v0.2.0)
```

apk keeps `pkgver` at the PHP version and `pkgrel` an integer, so our release
name lives in the description rather than in the version string.

The pool is at `/etc/php-fpm-ng/conf.d/www.conf` -- Alpine's layout, not
Debian's `pool.d`. Then:

```sh
rc-update add php-fpm-ng default
rc-service php-fpm-ng start
rc-service php-fpm-ng status
```

On Alpine the library lives at `/usr/lib/php85/libphp.so`, off the default
library path; the binary finds it through an `RPATH` written at build time,
which pins the PHP minor a second time, by path.

## A pool that answers HTTP

The shipped pool is `pool.type = fastcgi` behind a web server, which is what
most installs want. `http-direct` needs no web server in front, and has two
requirements the classic pool does not:

```ini
[www]
pool.type = http-direct
user = www-data
group = www-data
listen = 127.0.0.1:8080
pm = static                       ; http-direct requires static
pm.max_children = 2
chdir = /srv/www                  ; must be absolute
http.front_controller = /index.php ; relative to chdir
```

`php-fpm-ng -t` refuses each of those by name if it is missing, before anything
starts. See [`docs/http-direct.md`](http-direct.md) for the rest of the
`http.*` directives.

## What the packaged build supports

| `pool.type` | packaged | from source | why |
|---|---|---|---|
| `fastcgi` | yes | yes | upstream FPM's protocol handling; needs nothing from the engine that a distribution `libphp` does not export. |
| `http-direct` | yes | yes | including `pool.executor = worker`. The HTTP listener lives entirely in this SAPI. |
| `http` | **no** | yes | `http` keeps a request runtime alive across requests, which is what `zend_signal_use_persistent_handlers()` -- added by `patches/0006` **inside `Zend/`** -- exists for. That is the distribution's file, not ours, so a distribution `libphp` does not export it. |
| fibers (`pool.executor = fiber`) | **no** | yes | `patches/0007` applies inside `libphp`. |
| async | **no** | yes | `patches/0008`, likewise inside `libphp`. |
| TLS termination (`http.tls_*`) | **no** in `php-fpm-ng`, yes in `php-fpm-ng-tls` | yes | opt-in since v0.4.0 (issue #280): the code is beta, unaudited and network-facing, so the *default* package is the one without it. The second package below is built with it, and from source it is `./configure --enable-fpmng --enable-fpmng-tls`. **This is a change against v0.2.0**, where the single package terminated TLS. |
| the ACME client (`fpmng-dist://acme/...`) | **no** in `php-fpm-ng`, yes in `php-fpm-ng-tls` | yes | opt-in since v0.4.0 (issue #281), and it requires the TLS flag: `./configure --enable-fpmng --enable-fpmng-tls --enable-fpmng-acme`. The default package carries neither the challenge state nor the client scripts, and refuses `cron.script = fpmng-dist://acme/renew.php` at startup. Also a change against v0.2.0. |

The default package's binary does not silently degrade: a pool it cannot honour is
refused before the master forks anything, by name and with the reason.

```console
# php-fpm-ng -t -y /etc/php-fpm-ng/php-fpm-ng.conf
ALERT: [pool www] 'pool.type = http' is not supported by this binary: it was linked
against a distribution libphp, which does not carry patches/0006 (persistent Zend
signal handlers) -- a pool of this type would run with upstream signal behaviour
without saying so
ALERT: [pool www] use 'pool.type = fastcgi' or 'pool.type = http-direct', which this
binary supports in full, or a build from patched source (build/static-full.sh)
ERROR: failed to post process the configuration
```

`-t` runs the same check a start runs, so a configuration can be tested before
a restart rather than after one.

## The second package: `php-fpm-ng-tls`

From v0.4.0 on every release carries **two** packages per distribution, built
from the same commit and differing in one thing: the TLS build has
`--enable-fpmng-tls --enable-fpmng-acme` compiled in, so `http.tls_cert`,
`http.tls_key` and `cron.script = fpmng-dist://acme/renew.php` work in it and
are refused by the default one.

```sh
# instead of php-fpm-ng_v0.4.0_php8.5_amd64.deb
apt install -y ./php-fpm-ng-tls_v0.4.0_php8.5_amd64.deb
# instead of php-fpm-ng-v0.4.0-php8.5-x86_64.apk
apk add --allow-untrusted ./php-fpm-ng-tls-v0.4.0-php8.5-x86_64.apk
```

Everything else on this page applies unchanged: same paths, same
`/etc/php-fpm-ng`, same service file, same dependency on the distribution
`libphp`, same refusal of `pool.type = http`.

**The two cannot be co-installed**, and they say so to the package manager
rather than fighting over `/usr/sbin/php-fpm-ng`: each declares `Conflicts` and
`Replaces` against the other (`replaces=` on Alpine), and both provide the
virtual `php-fpm-ng-any` for anything that depends on "either of them".
Installing one over the other is therefore the normal package-manager
operation, not a remove-then-install dance -- your `/etc/php-fpm-ng` is
conffile-protected across it.

**It is beta, and unaudited.** That is not a formality: TLS termination is the
part of this project that faces the network with the least scrutiny behind it,
and a beta tier means its directives may change in a minor release and fixes
carry no response-time commitment. The binary says so itself, once per start,
in the master, before it forks anything (issue #295):

```
NOTICE: TLS termination, unaudited and network-facing (this binary was built with
--enable-fpmng-tls) is BETA: its directives may change in a minor release, and
fixes carry no response-time commitment -- see "Support tiers" in README.md
NOTICE: ACME certificate issuance, unaudited (this binary was built with
--enable-fpmng-acme) is BETA: its directives may change in a minor release, and
fixes carry no response-time commitment -- see "Support tiers" in README.md
```

(One line each, wrapped here to fit the page; the log writes each on one line.)

The support tiers table in [`README.md`](../README.md#support-tiers) is what
those words mean. If you terminate TLS in front of PHP -- in nginx, in a load
balancer, at a CDN -- the default package is the one to install, and it is the
default because that is the more common arrangement, not because the TLS build
is unfinished.

Both packages go through the same gate before they are published: built,
installed into a container with no compiler in it, and measured against an
exact PASS/SKIP count (`build/ci-package-gate.sh`, issue #224). The TLS one
scores more passes, which is the point -- the tests that skip on the default
package for want of TLS run there.

## Version skew

The package depends on a specific PHP minor -- `libphp8.5-embed`, not "any
`libphp`". Every embed package exports the same `SONAME`, `libphp.so`, so
nothing in the dynamic linker would stop 8.4's library from satisfying a binary
built against 8.5's headers: it would load, and then misbehave. Naming the
minor moves that failure to the package manager, where it is a refusal to
install.

At runtime the binary checks again, and the two cases differ:

* **A patch-level difference is fine and is logged once**, in the master,
  before any child exists:

  ```
  php-fpm-ng: notice: built against PHP 8.5.4, running on libphp 8.5.10
  (patch-level difference, supported)
  ```

  PHP keeps the ABI stable across patch releases of one minor. This exists so
  that the version pair is already in the log when a bug report arrives.

* **A minor or major difference is fatal**, before the first request:

  ```
  php-fpm-ng: FATAL: this binary was built against PHP 8.5.4 headers, but the
  libphp it loaded is PHP 8.4.12.
  ```

  The binary carries the struct layouts of the version it was built against, so
  continuing would corrupt memory rather than fail cleanly.

## If you need what the package cannot give

`http`, fibers and async need patches that apply inside `libphp`,
so they need a build from source. TLS termination needs only a flag, but the
packaged binary is built without it, so it is the same answer:

```sh
./build/prepare.sh /path/to/php-src   # applies the overlay and the patch stack
./build/static-full.sh                # or the dynamic build, see build/dynamic.sh
```

For TLS from a `configure` of your own, add `--enable-fpmng-tls`; it needs
`libevent_openssl >= 2.1` and OpenSSL >= 1.1.1 development files, and
`configure` fails naming the missing package rather than producing a binary
without TLS. A pool with `http.tls_cert` on a binary built without the flag is
refused at startup, naming the flag to rebuild with -- it never falls back to
plain HTTP on a port configured as HTTPS.

For the ACME client on top of that, add `--enable-fpmng-acme`. It requires
`--enable-fpmng-tls` and `configure` errors out if it is missing; see
[`acme-renewal.md`](acme-renewal.md#the-build-flag) for what a build without
it does with an ACME configuration.

`--enable-fpmng-debug-clock` is **for running the test suite, not for a
server**. It makes the master honour `FPMNG_DEBUG_CLOCK_RATE` and run its clocks
faster than real time, so that tests waiting on a one-minute cron schedule or on
a supervisor timeout do not have to wait in real seconds. No shipped package is
built with it, and `configure` prints a warning when it is used. See
[`fpmng-phpt.md`](fpmng-phpt.md#the-virtual-clock).

`--enable-fpmng-http2` and `--enable-fpmng-quic` are **reserved names, not
features**. Neither protocol exists in this tree, and `configure` refuses both
flags with a message naming the issue that is deciding them (#186/#187 for
HTTP/2, #188 for QUIC) rather than accepting a flag that switches nothing on.
The names are settled early so they are settled once; that is not a commitment
that either feature will arrive. #188 in particular may return "no": QUIC has
no `accept()`, connection IDs have to be routed in userland, and this project
hands each child a listening socket the kernel demultiplexes for it. Both
would require `--enable-fpmng-tls` if they existed -- HTTP/2 is negotiated over
ALPN, and QUIC carries TLS 1.3 inside the transport; there is no plaintext
QUIC.

For the two pool types the package does support, the package is the normal
case -- building from source to get `fastcgi` or `http-direct` buys nothing.

## See also

* [`docs/http-direct.md`](http-direct.md) -- what `http-direct` is, and how to
  configure the worker executor.
* [`docs/tls.md`](tls.md) and [`docs/acme-client.md`](acme-client.md) --
  terminating TLS in the pool, and obtaining the certificate.
