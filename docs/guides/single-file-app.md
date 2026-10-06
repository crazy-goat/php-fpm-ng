# One file: your PHAR, php.ini and fpm.conf in a single executable

`php-fpm-ng pack` writes one executable that carries your application and its
configuration. It is for the same small projects as the rest of this tree: one
machine, one binary, no nginx, no supervisord, no cron daemon.

What you supply is **finished**: an application PHAR, a `php.ini` and an
`fpm.conf`. All three are mandatory. `pack` does not build the PHAR, run
Composer, adapt a framework or fix your application, and it never opens the PHAR
(so a PHAR stub does not run while packing). The reference for the payload format
and the runtime mechanics is [`../payload.md`](../payload.md); this page is the
workflow and its limits.

## What the result is, and is not

* It carries code and configuration. It still needs the **supported PHP runtime on
  the host**: Ubuntu 26.04 with `libphp8.5-embed`, or Alpine edge with
  `php85-embed`, NTS only, the same as any `php-fpm-ng`
  ([`../install.md`](../install.md)). One executable per PHP minor.
* The PHAR extension is not loaded by default here, so the embedded `php.ini`
  must say `extension=phar`. On Alpine install `php85-phar` as well; on Ubuntu
  `phar.so` comes with `php8.5-common`. Any other extension your application
  needs must be installed on the host and named in the embedded `php.ini`; one
  that PHP did not load fails startup by name.
* It is not a static binary, not portable between distributions or PHP minors,
  and not a way to run an arbitrary framework: whether your application works
  from inside a PHAR is for you to test (see the smoke test below for the
  evidence this project has).
* The payload carries a SHA-256 that detects a truncated or damaged file. It is
  **not** a signature and says nothing about who made the file.
* Logs, cache, sessions and uploads are not embedded. They live outside the
  executable and outside the state directory (see below).

## 1. Prepare the three inputs

```sh
ls -l app.phar php.ini fpm.conf
```

`php.ini` is the only `php.ini` the packed executable will read: the host's
`php.ini`, its scan directory and `PHPRC` are ignored, and nothing is merged. A
complete minimal one:

```ini
extension=phar
memory_limit = 128M
error_log = /var/log/myapp/php-error.log
```

A complete minimal `fpm.conf`. The entry point is named with `fpmng-app://`
followed by an entry of the PHAR (here `public/index.php`); the entry must be in
the PHAR's manifest:

```ini
[global]
daemonize = no
pid = /run/myapp.pid
error_log = /var/log/myapp/fpm.log

[web]
listen = 127.0.0.1:8080
pool.type = http-direct
pm = static
pm.max_children = 4
chdir = /
http.front_controller = fpmng-app://public/index.php
```

Supported: `pool.type = http-direct` (front controller), `supervisor.script` and
`cron.script` with `fpmng-app://`. Refused or unsupported: `http.static = yes` on
an archive front controller, `pool.type = gateway`/`fastcgi` scripts, and
`.user.ini` for such a pool. Every request goes to the front controller; no
file inside the PHAR and no host file is reachable by URL.

`include=` and `extension=` inside `fpm.conf` and `php.ini` name host files. There
is no override or include mechanism for the embedded files: the operator can add
`-t`, `-d`, `-c` or `-y` to the command line and those win, as on any binary.

## 2. Pack

Pack with an **unpacked** `php-fpm-ng` (the package binary). No compiler, no PHP
source and no PHP CLI are needed.

```sh
php-fpm-ng pack app.phar php.ini fpm.conf -o myapp
./myapp -t
```

`pack` refuses a missing, unreadable or empty input, naming the file, an
existing output, and a binary that already carries an application. It writes
nothing in those cases. A PHAR that PHP cannot read is not detected by `pack`;
`./myapp -t` reports it.

Do not run `strip` on the result: appended data is not part of any ELF section,
so `strip` removes the payload and the executable silently becomes a plain
`php-fpm-ng`. Pack after stripping, or never strip (the project's packages do not).

## 3. Run

The original `app.phar`, `php.ini`, `fpm.conf` and the source directory may now be
deleted.

```sh
rm app.phar php.ini fpm.conf
mkdir -p /var/log/myapp
./myapp
```

Startup fails (status 78, or before "ready to handle connections") with a message
naming the cause, and never falls back to a host configuration, when: the payload
is damaged or cut short, the PHAR is unusable, `extension=phar` is missing, an
`extension=` of the embedded `php.ini` is not loaded, the entry is not in the
PHAR, or `fpm.conf` is invalid. The PHP ABI guard of the binary runs first, as for
any `php-fpm-ng`: a libphp of another minor is fatal
(`this binary was built against PHP 8.5.4 headers, but the libphp it loaded is
PHP 8.4.12`, see [`../install.md`](../install.md#version-skew)).

## 4. State directory and writable state

PHP cannot open a PHAR appended to an executable, so at startup the three inputs
are written, read-only, to `<base>/php-fpm-ng-app-<euid>/<sha256 of the payload>/`
(`<base>` is `$FPMNG_APP_DIR`, else `$TMPDIR`, else `/tmp`; mode 0700, 0755 when
started as root). This is the only temporary-file policy: the runtime never
deletes these directories, and `rm -rf <base>/php-fpm-ng-app-<euid>` while the
service is stopped removes the old ones. Code is immutable; anything the
application writes (logs, cache, sessions, uploads) must go to a directory you
choose, such as `/var/log/myapp` or `/var/lib/myapp`, never into the PHAR or the
state directory. Set `FPMNG_APP_DIR` to a private directory if `/tmp` is cleaned
while the service runs.

## 5. Reload and upgrade

`SIGUSR1` reopens logs, `SIGTERM`/`SIGQUIT` stop, `SIGUSR2` re-executes the master.
To upgrade, repack with an unpacked binary, rename the result **over** the
running executable (do not write into it), and send `SIGUSR2`:

```sh
php-fpm-ng pack app-v2.phar php.ini fpm.conf -o myapp.new
mv myapp.new myapp
kill -USR2 "$(cat /run/myapp.pid)"   # the pid file named in [global]
```

The master re-reads the payload the file holds now and serves it from a new
state directory; the old one stays until you remove it. To change only the
configuration, repack the same PHAR with the new `php.ini` or `fpm.conf`.

## Evidence: the smoke test and the gate

A contributor can reproduce the workflow with the checked-in fixture and no
other file: build or install `php-fpm-ng`, then

```sh
build/test-pack-smoke.sh /path/to/php-fpm-ng "$(php-config8.5 --php-binary)"
```

It writes a tiny PHAR (a hand-written fixture, not a PHAR builder), packs it with
a `php.ini` and an `fpm.conf`, deletes the three inputs, starts the result and
reads code, an included file and a resource over HTTP, shows that a host
`php.ini` is ignored, refuses a missing input and a truncated executable,
repacks over the running executable, reloads with `SIGUSR2` and stops. It needs
`curl`, a PHP 8.5 CLI (only to write the fixture) and a non-root user. CI runs it
in the `integration` job of `.github/workflows/build-matrix.yml`.

The detailed tests are `.phpt` files run by `build/run-fpmng-phpt.sh` in the
`fpmng-phpt` job (against the canonical build on Ubuntu 26.04, PHP 8.5) and again
by the release package gate against the installed `.deb` and `.apk`
(`build/ci-package-gate.sh`; a skip is allowed only if listed with a reason in
`build/package-gate-expected.txt`, and none is listed for these tests):

| Promise | Test |
|---|---|
| mandatory-input refusal, no stub execution while packing, output is a working binary without its inputs | `fpmng-pack.phpt` |
| configuration precedence (embedded ini wins, host ini and scan dir ignored), PHAR include and resource access, boundary of public files, state directory, OPcache script identity | `fpmng-pack-run.phpt` |
| corrupt or truncated payload, missing entry, bad `fpm.conf`, missing `extension=phar`, missing extension, `fpmng-app://` on an unpacked binary | `fpmng-pack-run.phpt` |
| lifecycle: `SIGUSR1`, `SIGUSR2`, `SIGTERM`, upgrade by repack and rename | `fpmng-pack-run.phpt` |
| class autoloading from the PHAR (`spl_autoload_register` loading `lib/AutoGreeter.php`), at start and after the upgrade | `build/test-pack-smoke.sh` (CI job `integration`); not in the `.phpt` tests |
| coexistence with the distribution payload (`fpmng-dist://`) | `fpmng-pack.phpt` (byte-identical prefix), `fpmng-payload-distribution.phpt` |
| ordinary unpacked execution, supplied-ini bootstrap | the rest of the `fpmng-*.phpt` suite, `fpmng-ini-bootstrap-policy.phpt`, `fpmng-ini-bootstrap-extension.phpt` |
| ABI mismatch | `build/test-libphp-abi-guard.sh` (CI job `checks`) |

OPcache: scripts read through `phar://` are cached (`fpmng-pack-run.phpt` checks
`num_cached_scripts > 0`). Two payloads never share cache keys because the digest
is in the script path; stale code after an upgrade is avoided by that path and by
timestamp validation. A PHAR entry with modification time 0 is silently not
cached (OPcache skips timestamp 0, `ext/opcache/ZendAccelerator.c`); build the
PHAR with real mtimes. `opcache.validate_timestamps=0` alone does not avoid this.
