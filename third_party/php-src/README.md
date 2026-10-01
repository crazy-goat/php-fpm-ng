# third_party/php-src

This directory holds the php-src files that this SAPI compiles and that no
PHP development package ships (issue #421, part of #418). With these files,
this repository and a distribution PHP SDK (`php8.5-dev` and
`libphp8.5-embed`, or `php85-dev` and `php85-embed`) are enough to compile and
link `php-fpm-ng`. No php-src checkout is needed.

The SDK supplies the engine headers (`Zend/`, `main/`, `TSRM/`, `ext/`) and
`libphp`. The files here supply the rest: upstream FPM sources that
`sapi/fpmng/fpm/` does not override, and the FastCGI layer. The build keeps
the two sets apart. `build/standalone-tree.sh` puts `main/fastcgi.h` in a
directory of its own, and that directory comes first on the include path. No
other php-src `main/` header can reach the compiler this way, so every
`main/*.h` comes from the SDK the binary links against.
`build/audit-compile-deps.sh` checks this from the compiler's `-MD` output.

## Provenance

| | |
|---|---|
| Upstream | https://github.com/php/php-src |
| Tag | `php-8.5.9` |
| Commit | `dd6e76cce27aaa0ed9f7520648ed1081dfb6af36` |
| Files | 57 (see `MANIFEST`) |

`MANIFEST` lists every file, one per line, with four tab-separated columns:

- the path in this directory;
- the upstream path;
- the SHA-256 of the pristine upstream file at the tag;
- the SHA-256 of the copy here.

The two hashes are equal for every file except two:

- `main/fastcgi.c` and `main/fastcgi.h` carry `patches/0001` to `0005`. The
  manifest's `patches` line fingerprints that stack.

One file is stored under a different name, with its content unchanged:

- `sapi/fpm/fpm/zlog_upstream.h` is upstream's `sapi/fpm/fpm/zlog.h`.
  `sapi/fpmng/fpm/zlog.h` extends it (issue #130) and includes it by this
  name. That way the struct layouts still come from the pinned upstream
  (`build/prepare.sh` explains why that matters).

The pin is independent of the SDK version, and that is deliberate. The
vendored FPM files compile against the SDK's headers, and PHP keeps the
engine API stable within a minor. Measured on 2026-10-01: these 8.5.9 files
built against the Ubuntu 26.04 SDK (8.5.4) and the Alpine edge SDK (8.5.10).
The pin moves when a patch release changes one of these files, not with every
release.

## Licenses

The files keep their upstream notices unchanged. Nothing in this directory is
edited by hand.

- `LICENSE`: the PHP License, version 3.01. It covers `main/fastcgi.c`,
  `main/fastcgi.h` and the PHP Group headers in `sapi/fpm/fpm/`.
- `sapi/fpm/LICENSE`: the BSD-style license of Andrei Nigmatulin's original
  FPM. It covers the `sapi/fpm/fpm/` files marked "(c) Andrei Nigmatulin", and
  upstream ships it in the same place.

## What is included, and why

Each file is here for one reason only: a translation unit of the shipped build
reads it. `build/probe-standalone-compile.sh` checks this. It fails when a
vendored file is not read by any translation unit.

- **`sapi/fpm/fpm/*.c`, 21 base sources**: upstream's `PHP_FPM_FILES`, minus
  the files `sapi/fpmng/fpm/` replaces.
  - `events/kqueue.c` and `events/port.c` are BSD and Solaris backends.
    Without `HAVE_KQUEUE` and `HAVE_PORT` they compile to stubs. They are
    vendored anyway because `fpm_events.c` calls `fpm_event_kqueue_module()`
    and `fpm_event_port_module()` unconditionally.
- **`fpm_trace.c` and `fpm_trace_pread.c`**: the `/proc/<pid>/mem` slowlog
  backend. Upstream's `config.m4` adds one trace backend conditionally and
  prefers ptrace where it works. This build uses pread instead: it needs no
  ptrace permission, and it is what the libphp build has always compiled
  (`HAVE_PTRACE` is off in `build/libphp-build.sh`).
- **`sapi/fpm/fpm/*.h` and `events/*.h`, 30 headers**: the headers those
  sources and the overlay include.
  - `fpm_main_arginfo.h` is generated upstream by `gen_stub.php` from
    `fpm_main.stub.php`, and upstream commits it. It is vendored as
    generated; this repository never regenerates it.
- **`main/fastcgi.c` and `main/fastcgi.h`**: the FastCGI protocol layer with
  this repository's transport patches applied (`patches/README.md`).
  - The SDK ships its own `main/fastcgi.h`, but that copy is unpatched. A
    build that read it would compile against a different
    `fcgi_init_request()` signature than the one it links. In practice that
    is a compile error, and the audit refuses it as well.

There is one generated input that is not here: `config.h`. php-src's
configure writes it, and ext/fpmng_metrics includes it. On this path the build
writes a one-line shim instead, which includes the SDK's `php_config.h`.

## What is excluded, and why

From upstream `sapi/fpm/`:

| Files | Reason |
|---|---|
| `fpm.c`, `fpm_children.c`, `fpm_conf.c`, `fpm_conf.h`, `fpm_process_ctl.c`, `fpm_request.c`, `fpm_request.h`, `fpm_stdio.c`, `zlog.h` | `sapi/fpmng/fpm/` owns them. `vendor-php-src.sh check` refuses a vendored file that has the same name as an overlay file. |
| `fpm_systemd.c`, `fpm_systemd.h` | `HAVE_SYSTEMD` is off (it would add a libsystemd link, `build/libphp-build.sh`). |
| `fpm_trace_mach.c`, `fpm_trace_ptrace.c` | The macOS and ptrace trace backends; this build uses `fpm_trace_pread.c`. |
| `fpm_main.stub.php` | The input of the generated `fpm_main_arginfo.h`, which is vendored as generated. |
| `config.m4`, `Makefile.frag` | Build glue of the php-src build. This SAPI has its own in `sapi/fpmng/`. |
| `*.in` (`php-fpm.conf.in`, `www.conf.in`, `php-fpm.service.in`, `init.d.php-fpm.in`, `php-fpm.8.in`, `status.html.in`) | Installation templates. Packaging ships its own (`packaging/`). |
| `CREDITS` | Credits list for `php -i`; not a license notice. |
| `tests/` | The test harness is a separate dependency set (issue #423). |

Everything else in php-src (the engine, `main/`, `ext/`) is excluded because
the SDK provides it. Vendoring any of it would bring back the header
shadowing described above.

## Updating

**Patches.** A change to a vendored file belongs in `patches/`, never in the
file itself. After changing `patches/`, re-import (see the next step). Until
then, `vendor-php-src.sh check` fails, and so does the CI checks job.

**Refreshing or moving the pin.** Check out a release tag of php-src, with no
local changes, and run:

```sh
build/vendor-php-src.sh import /path/to/php-src
```

The import does the following:

1. Refuses to run if a vendored file no longer matches its manifest hash.
   Re-importing would overwrite a local edit without a trace.
2. Refuses a dirty checkout or an untagged commit.
3. Copies the listed files.
4. Applies `patches/` in the order and with the `php-<minor>/` overrides of
   `build/prepare.sh`.
5. Rewrites `MANIFEST` with the new hashes, then checks the result.

Review the diff as an upstream change. Then rebuild and run the suites. A
header that changed or a source that upstream added is exactly what the
import cannot judge.

**Adding or dropping a file.** Edit the file list in `MANIFEST`: add a line
with `-` in both hash columns, or delete a line. Then run the import.
`build/probe-standalone-compile.sh` shows whether the change was needed: it
fails on a vendored file that no translation unit reads, and on a missing one
the compile or the link fails.

## Checking

| Command | What it shows |
|---|---|
| `build/vendor-php-src.sh check` | The directory matches its manifest and the patch stack (CI checks job; needs no php-src and no network). |
| `build/probe-standalone-compile.sh <outdir>` | The set is sufficient: everything compiles and links against the SDK, with every command line in `<outdir>/commands.log`, and the dependency audit passes. |
