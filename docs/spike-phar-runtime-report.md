# Spike #426: PHAR runtime in an embedded application payload

Measured 2026-09-24 on the poligon (`192.168.8.50`). This is an experiment,
not a runtime implementation or a performance benchmark. Raw artifacts and
probe scripts are retained outside the repository at
`~/rd/issue-426-phar-20260924/`; no binary/PHAR fixture is committed.

## Verdict

**PHP can execute the test PHAR on both measured libc families, but not directly
from the current FPMNG ELF payload.** The PHAR must first exist as a normal
filesystem archive (or a future implementation must supply an equivalent
seekable PHAR-aware wrapper/engine integration). A `phar://<executable>/...`
URL does not identify the kind=application payload or an offset within the ELF.

The current custom payload format (`FPMNGPY1` record + `FPMNGAR1` archive) is
read by project C code as an indexed byte buffer. The existing `fpmng-dist://`
wrapper is read-only, used by cron/supervisor scripts, and opens a copy of the
whole selected member in a memory stream. It does not register or expose a PHAR
archive. `http.front_controller` validates a real regular file under its
configured root/chdir; the normal FastCGI entry point also receives a filesystem
`SCRIPT_FILENAME`. Neither path accepts an application PHAR inside the ELF.

Loading the PHAR extension is a separate bootstrap requirement for
libphp/package builds. Ubuntu's package and Alpine's split PHP packages load
`phar.so` through INI scan configuration. `-n` omits it and the fixture fails
with `Class Phar not found`; the static source build below has Phar compiled in
and therefore works with `-n`. A generated minimal INI with `extension=phar.so`
works on Alpine when `PHP_INI_SCAN_DIR` is redirected to an empty directory, so
this need not imply reading the host's entire INI configuration.

## What the PHAR contained and proved

A signed 813-byte PHAR included:

- an `app.php` entry;
- `autoload.php`, registering a minimal class autoloader;
- `src/Greeting.php`, loaded by that autoloader;
- `resources/message.txt`, read-only data loaded with `file_get_contents()`.

Successful output was `included-class-ok|resource-ok` (the OPcache fixture also
returned `app=alpha` or `app=beta` and the internal `phar://` script identity).

The archives were generated with `Phar::setSignatureAlgorithm(Phar::SHA256)`.
They executed with `phar.readonly=On` and `phar.require_hash=On`; the first is a
write restriction, not an execution restriction. The PHAR stub maps its archive
and requires the app entry.

## Direct embedded-ELF probe

The source-built `php-fpm-ng` binary was copied and the fixture added to a kind
2 application payload using the existing packer:

```sh
cp php-fpm-ng appended-fpm-ng
mkdir phar-dir
cp probe.phar phar-dir/app.phar
php build/payload-pack.php append \
  --binary=appended-fpm-ng --kind=application --dir=phar-dir --prefix=app
php build/payload-pack.php list --binary=appended-fpm-ng
```

Observed record: kind 2, offset `49,766,288`, size 857, SHA-256
`ea3f71e75fb5dcb70728ae1b081af0fbf479283d0c119c311dae394e1a2429bf`. Its
`app/app.phar` member is 813 bytes and matches the standalone archive SHA-256
`d1108eb6a5a97e5a09e486c4a997ce57760bd26bbe7bab49c56838bdf35de086`.

This attempt failed:

```sh
php -r '$p="phar://".$argv[1]."/app.phar/app.php";
        var_dump(file_exists($p)); include $p;' ./appended-fpm-ng
```

Output: `bool(false)`, then `phar error: invalid url or non-existent phar`.
The FPMNG payload record is after an ELF image and names a typed payload blob;
the PHAR engine has no offset argument for that format. Pointing PHAR at the
executable's beginning also fails because the executable starts with ELF, not a
PHAR stub. This disproves direct nested/offset access for this probe; it does
not disprove a future custom stream wrapper or temporary extraction design.

A throwaway extractor parsed the tested FPMNGAR1 entry and wrote the unchanged
813-byte PHAR to a normal file. The extracted digest matched the standalone
fixture. This demonstrates an extraction alternative, but that PHP extractor is
only an experiment, not a proposed production bootstrap/parser.

## Runtime/package matrix

| Environment | Runtime/build | Phar mechanism | Result |
| --- | --- | --- | --- |
| Ubuntu 26.04.1 LTS (Resolute), glibc | `php8.5-cli`/`php8.5-common` `8.5.4-0ubuntu1.3`; package system PHP CLI | Shared `/usr/lib/php/20250925/phar.so`, loaded via `/etc/php/8.5/cli/conf.d/20-phar.ini` | Fixture runs with package INI; `php -n` fails with `Class Phar not found` |
| Ubuntu 26.04.1, glibc | Source `php-8.5.9` tag, commit `dd6e76cce27aaa0ed9f7520648ed1081dfb6af36`; FPMNG CI configure plus `--enable-phar --enable-opcache --with-zlib` | Phar statically compiled into the PHP binary | PHAR runs through `php` and HTTP-direct/FPM; `-n` also has Phar |
| Alpine 3.24.2, musl | `php85-cli` + `php85-phar` `8.5.10-r0`; `php85-embed` for FPMNG | Split `/usr/lib/php85/modules/phar.so`; package config `/etc/php85/conf.d/01_phar.ini` | CLI and package-linked FPMNG run with normal INI; `-n` omits Phar and HTTP request fails in the script with `Class Phar not found` |
| Official `php:8.5-cli-alpine` image, Alpine 3.24.2 | PHP `8.5.10` image digest `sha256:4992c6fda82eadfb3b22dca3929188dc5831e9f44a3d42b3c8d36a460b4d5a80` | Phar present in this image's PHP build; no `php.ini` loaded by default | Fixture runs with the image's CLI configuration; this is not the package-linked FPMNG cell |

For the Alpine package FPMNG case, the repository's existing
`build/libphp-build.sh` succeeded using `php-config85`, `php85-embed`,
`php85-dev`, plus build-only `libevent-dev` and `acl-dev`. The result was PHP
8.5.10 (fpm-fcgi), dynamically linked to `/usr/lib/php85/libphp.so`. With the
package INI it served the extracted PHAR on HTTP-direct successfully in 5/5
requests (each body contained the included class and resource output). Under
`-n`, a request returned the expected `Class Phar not found` fatal (the scratch
direct-HTTP handler still emitted HTTP status 200; do not interpret that status
as application success).

A minimal Alpine configuration also worked:

```ini
extension=/usr/lib/php85/modules/phar.so
phar.readonly=1
phar.require_hash=1
```

Run with `PHP_INI_SCAN_DIR` pointing to an empty directory to avoid loading
`01_phar.ini` as well and double-loading the module. That combination served
the fixture with HTTP 200 and no module warning. With the stock scan directory,
the same `extension=` line warned that Phar was already loaded because the
package's `01_phar.ini` also loads it.

No Alpine PHP-FPM-ng package was installed or tested; the Alpine FPM cell is
our `libphp-build.sh` product linked against the Alpine distribution embed
library, not an official php-fpm-ng package.

## FPM/PHAR and OPcache observations

With the source build and a minimal INI enabling OPcache, two HTTP-direct pools
served two archives (`app-a.phar` and `app-b.phar`) from distinct extracted
paths. The status endpoint's `opcache_get_status(true)['scripts']` contained
both archive paths independently, e.g.:

```text
/home/piotr/rd/issue-426-phar-20260924/extracted/app-a.phar
/home/piotr/rd/issue-426-phar-20260924/extracted/app-b.phar
```

The PHAR app's `__FILE__` was
`phar:///.../app-a.phar/app.php`, but `opcache_is_script_cached(__FILE__)`
returned false and that inner URI did not appear in the table. The archive
itself is the OPcache script entry. Therefore distinct stable extracted paths
(or a deliberate invalidation/key strategy for reused paths) are needed to
avoid different app archives sharing one identity. The unpacked control table
instead had separate `.php` entries under `unpacked/alpha` and `unpacked/beta`.
The captured key lists are in `phar-opcache-script-keys.txt` and
`plain-opcache-script-keys.txt` in the experiment scratch directory.

Small bounded timing smoke (same fixture, separate fresh FPM master per mode,
one first request plus 30 repeated `curl` requests; wall time includes local
HTTP overhead):

| Mode | First request | 30 warm requests: median | p95 |
| --- | ---: | ---: | ---: |
| Extracted PHAR | 1.185 ms | 0.582 ms | 0.630 ms |
| Unpacked equivalent | 1.179 ms | 0.447 ms | 0.504 ms |

The normalized 30-sample raw files are `phar-bench.warm.normalized.txt` and
`plain-bench.warm.normalized.txt` in the experiment scratch directory. This
single local run is only a bounded sanity comparison, not evidence for a
production latency claim or a broad performance conclusion.

## Bootstrap ordering and present support boundary

The FPM SAPI startup path calls `php_module_startup()` before entering
`fpm_init()` and before parsing the FPM pool configuration. CLI flags set
`php_ini_path_override` for `-c` or `php_ini_ignore` for `-n` before that
startup. Thus a package build that dynamically loads Phar must load its module
from the process INI/scan path before application execution; a later
`php_admin_value[...]` cannot load the module. A source build that statically
compiled Phar needs no runtime `extension=phar.so` line.

Today, the project exposes only kind 1 (`distribution`) and kind 2
(`application`) in the packer; kind 2 is a payload type, **not** an application
runtime. Current runtime registration and script execution are limited to
`fpmng-dist://` in cron/supervisor script handling. That wrapper is registered
from the child script path and uses a read-only memory stream. No PHP FPM pool
currently maps kind 2 into a PHAR URL or an extracted front-controller path.

The HTTP-direct front controller is validated by `realpath`/`stat` as a regular
file under its configured root; a virtual `phar://` URL will not meet that
contract. The probe set `http.front_controller` to the extracted `.phar` as the
real regular file, and its stub/application returned the expected output. It did
not implement extraction in master bootstrap or exercise framework routing. The
spike does not decide whether production
extraction is temporary, per-binary content-addressed, into a writable state
directory, or another design. It does establish that this path must account
for realpath validation, correct PHP/Phar startup, and distinct archive paths
for OPcache.

## Reproduction artifacts

All raw files and scripts remain on the poligon in
`~/rd/issue-426-phar-20260924/`, including:

- `probe-pack-phar.php`, `probe-make-app.php`, and `probe-make-opcache-phar.php`;
- the source FPMNG build at `php-src/` and the Alpine libphp-linked build under
  the experiment output tree/container;
- appended ELF `appended-fpm-ng`, standalone and extracted PHARs, unpacked
  fixtures, custom payload-list output;
- FPM configs, response outputs/logs, OPcache script tables, per-request time
  files (`phar-bench.*`, `plain-bench.*`), and scratch extraction scripts.

The build command for the glibc source cell was:

```sh
./build/prepare.sh /path/to/php-8.5.9
cd /path/to/php-8.5.9
./buildconf --force
./configure --disable-all --enable-fpmng --enable-fpmng-tls \
  --enable-fpmng-acme --enable-session --with-openssl \
  --enable-fpmng-debug-clock --enable-phar --enable-opcache --with-zlib
make -j8 fpmng cli
```

The Alpine FPMNG cell used, in an ephemeral `alpine:3.24` container:

```sh
apk add php85-embed php85-phar php85-cli php85-dev build-base autoconf \
  bison re2c openssl-dev zlib-dev libevent-dev acl-dev curl
./build/libphp-build.sh /work/php-src /work/out-alpine
```

## Unresolved for runtime design (#427)

This spike does not decide the product contract for entrypoint selection,
`php.ini` precedence, FPM config inputs/includes, external writable state,
filesystem permissions, or whether extraction is permitted/required. It also
does not prove behavior for a real framework or a full static Alpine package.
Those remain explicit runtime/pack contract questions. The measured evidence
supports proceeding to design with **extraction to a real path and explicit
Phar module/bootstrap policy** as the concrete baseline, while keeping a custom
seekable wrapper as an unproven alternative rather than assuming direct
`phar://` access works.
