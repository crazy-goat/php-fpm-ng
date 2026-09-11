# Installing php-fpm-ng

php-fpm-ng ships as a package that contains **no PHP**. It is one binary, a
configuration file and a service file; the interpreter it runs on is the
distribution's own `libphp`, which arrives as a dependency. There is no
compiler on the target and no php-src to build.

That is possible for two of the pool types and not for the others. The table
under [What the packaged build supports](#what-the-packaged-build-supports)
says which, and names the mechanism in each case rather than saying "not
supported" and leaving the next reader to guess.

Every command below was run on 2026-09-11 in the image it names, in the order
printed, as `root` (so no `sudo`; add it if you are not root). The exceptions
are called out where they appear: `systemctl` needs an init system a container
does not have, and the plain `curl`/`wget` download lines need the repository
to be public, which it is not yet -- both say so on the spot.

## Getting the files

The repository is private, so the release assets are not anonymously
downloadable yet. What works today, with a GitHub account that can see the
repository:

```sh
gh release download v0.1.0 --repo crazy-goat/php-fpm-ng
```

Once the repository is public the same three files are at stable URLs, and this
is the form to use:

```sh
curl -fLO https://github.com/crazy-goat/php-fpm-ng/releases/latest/download/SHA256SUMS
curl -fLO https://github.com/crazy-goat/php-fpm-ng/releases/latest/download/php-fpm-ng_v0.1.0_php8.5_amd64.deb
```

Either way you end up with `SHA256SUMS` and the package next to it, which is
what the rest of this page assumes.

## Debian / Ubuntu

Image: `ubuntu:26.04`.

```sh
sha256sum -c --ignore-missing SHA256SUMS
apt install -y ./php-fpm-ng_v0.1.0_php8.5_amd64.deb
```

`apt` pulls `libphp8.5-embed` in as a dependency; that is the PHP the binary
runs on. The packages are unsigned on purpose -- they are files you fetched and
install by path, not a repository your machine trusts for every future upgrade
-- so the checksum step is part of the install, not an optional extra.

```console
# dpkg -l php-fpm-ng | tail -1
ii  php-fpm-ng  8.5.4-1~v0.1.0  amd64  FPM process manager with HTTP-direct pools, on the distribution PHP
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
grep php-fpm-ng-v0.1.0-php8.5-x86_64.apk SHA256SUMS | sha256sum -c -
apk add --allow-untrusted ./php-fpm-ng-v0.1.0-php8.5-x86_64.apk
```

The checksum line is spelled differently here because BusyBox `sha256sum` has
no `--ignore-missing`: it would try to verify the `.deb` too and fail on a file
you did not download. Piping the one line you care about is the same check.

`--allow-untrusted` is not a workaround being tolerated: the package is
unsigned by decision, the checksum above is what authenticates it, and apk is
being told the truth about what it is being handed.

```console
# apk info -v php-fpm-ng
php-fpm-ng: FPM process manager with HTTP-direct pools, on the distribution PHP (v0.1.0)
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
| `http-direct` | yes | yes | including `pool.executor = worker`, TLS termination and the ACME client. The HTTP listener lives entirely in this SAPI. |
| `fastcgi-ng` | **no** | yes | needs `zend_signal_use_persistent_handlers()`, added by `patches/0006` **inside `Zend/`**. That is the distribution's file, not ours, so a distribution `libphp` does not export it. |
| `http` | **no** | yes | same mechanism as `fastcgi-ng`: both keep a request runtime alive across requests, which is what the persistent signal handlers exist for. |
| fibers (`pool.executor = fiber`) | **no** | yes | `patches/0007` applies inside `libphp`. |
| async | **no** | yes | `patches/0008`, likewise inside `libphp`. |

The packaged binary does not silently degrade: a pool it cannot honour is
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

`fastcgi-ng`, `http`, fibers and async need patches that apply inside `libphp`,
so they need a build from source:

```sh
./build/prepare.sh /path/to/php-src   # applies the overlay and the patch stack
./build/static-full.sh                # or the dynamic build, see build/dynamic.sh
```

For the two pool types the package does support, the package is the normal
case -- building from source to get `fastcgi` or `http-direct` buys nothing.

## See also

* [`docs/http-direct.md`](http-direct.md) -- what `http-direct` is, and how to
  configure the worker executor.
* [`docs/tls.md`](tls.md) and [`docs/acme-client.md`](acme-client.md) --
  terminating TLS in the pool, and obtaining the certificate.
