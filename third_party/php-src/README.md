# third_party/php-src

This directory holds the php-src files that this SAPI compiles and that no
PHP development package ships (issue #421, part of #418). With these files,
this repository and a distribution PHP SDK (`php8.5-dev` and
`libphp8.5-embed`, or `php85-dev` and `php85-embed`) are enough to compile and
link `php-fpm-ng`. No php-src checkout is needed.

It also holds the test fixtures that the `.phpt` runners take from php-src
(issue #423): upstream's `run-tests.php`, the FPM test harness and the retained
upstream FPM `.phpt` suite, and one data file. With these, a candidate binary
and a distribution CLI are enough to run both suites. Those are listed
separately under "Test fixtures" below; nothing in the build reads them.

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
| Files | 207 (see `MANIFEST`): 57 for the build, 150 test fixtures |

`MANIFEST` lists every file, one per line, with four tab-separated columns:

- the path in this directory;
- the upstream path;
- the SHA-256 of the pristine upstream file at the tag;
- the SHA-256 of the copy here.

The two hashes are equal for every file: `main` carries no php-src patch
(issues #591, #592), and `vendor-php-src.sh check` refuses a file where they
differ.

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

Each of the 57 build files is here for one reason only: a translation unit of
the shipped build reads it. `build/libphp-build.sh` checks this through
`build/audit-compile-deps.sh`. The build fails when a vendored file is not
read by any translation unit.

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
- **`main/fastcgi.c` and `main/fastcgi.h`**: the FastCGI protocol layer,
  pristine at the pinned tag.
  - The build reads these copies, not a header from the SDK, so the header
    and `fastcgi.c` always match.

There is one generated input that is not here: `config.h`. php-src's
configure writes it, and ext/fpmng_metrics includes it. On this path the build
writes a one-line shim instead, which includes the SDK's `php_config.h`.

## Test fixtures

Issue #423. `build/phpt-tree.sh` assembles the tree that `build/run-fpmng-phpt.sh`
and `build/run-fpm-phpt.sh` run in from the files below, this repository's own
`sapi/fpmng/tests/` and `sapi/fpmng/acme/`. It lays them out the way
`build/prepare.sh` does, without a php-src checkout. The runners take `-` in
place of the tree to do that themselves.

| Files | Why they are needed |
|---|---|
| `run-tests.php` | The test runner. Both runners drive the candidate through it. |
| `sapi/fpm/tests/tester.inc`, `fcgi.inc`, `logreader.inc`, `logtool.inc`, `response.inc`, `skipif.inc`, `status.inc` | The FPM test harness. Our own `fpmng-*.phpt` use it as well (`require_once "tester.inc"`, `include "skipif.inc"`), so it is part of the owned suite's dependency closure, not only upstream's. |
| `sapi/fpm/tests/*.phpt`, 141 files | The retained upstream FPM compatibility suite that `build/run-fpm-phpt.sh` runs. All of them are kept: `sapi/fpmng/tests/upstream-deviations.list` names the ones that are expected to fail, and none is dropped to make a run green. |
| `sapi/fpm/tests/CONFLICTS` | Read by `run-tests.php` when it runs in parallel (`-j`, issue #394); inert for a serial run, kept because it belongs to the directory. |
| `ext/standard/tests/misc/browscap.ini` (not vendored) | `gh12621.phpt` sets `browscap` to `__DIR__/../../../ext/standard/tests/misc/browscap.ini`, so the test needs a file at that relative path. `build/phpt-tree.sh` writes this repository's own `sapi/fpmng/fixtures/browscap.ini` there (issue #559); see below. |

The fixtures are stored at their upstream paths and are not edited, like every
other file here, with one exception: the browscap data file is not vendored. `vendor-php-src.sh check` and `import` treat them like the
build files. Only the harness side matters for the closure: the runners never
read a php-src header or source file.

The licenses of the fixtures, inventoried from the headers of each file:

- `run-tests.php`: PHP License 3.01 (covered by `LICENSE` above). It embeds
  `sebastian/diff`, which carries its own BSD 3-Clause notice in the file.
- `tester.inc`, `logreader.inc`, `logtool.inc`, `response.inc`, `skipif.inc`,
  `status.inc` and the `.phpt` files: no per-file notice. They are part of
  php-src, so `LICENSE` (PHP License 3.01) is the license that applies.
- `fcgi.inc`: MIT license, "This file is part of PHP-FastCGI-Client" by
  Pierrick Charron. The notice is in the file.
- `browscap.ini`: not vendored. php-src's copy (296 KB, from Gary Keith's
  browscap project, 2008) states no license terms, so it is replaced by a
  minimal file written for this repository, `sapi/fpmng/fixtures/browscap.ini`
  (issue #559). This is the one place where the test tree is not a byte-exact
  upstream subset. `gh12621.phpt` is unchanged and still passes the file through
  `php_admin_value[browscap]`; it only needs a section that matches
  `Konqueror/2.0`.

None of these files is compiled, linked, packaged or installed. The deb and apk
packages ship `php-fpm-ng`, its configuration and its service file only.

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
| `tests/` | Vendored, but as test fixtures, not as build input: see the next section. |

Everything else in php-src (the engine, `main/`, `ext/`, apart from the test
fixtures above) is excluded because
the SDK provides it. Vendoring any of it would bring back the header
shadowing described above.

## Updating

**No patches.** `main` carries no php-src patch, so a vendored file is never
edited: `vendor-php-src.sh check` fails (and so does the CI checks job) when a
file differs from the hash in the manifest. A fix to upstream behaviour goes
into `sapi/fpmng/fpm/` as an owned file, or upstream.

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
4. Rewrites `MANIFEST` with the new hashes, then checks the result.

Review the diff as an upstream change. Then rebuild and run the suites. A
header that changed or a source that upstream added is exactly what the
import cannot judge.

**Adding or dropping a file.** Edit the file list in `MANIFEST`: add a line
with `-` in both hash columns, or delete a line. Then run the import.
`build/libphp-build.sh` shows whether the change was needed: it
fails on a vendored file that no translation unit reads, and on a missing one
the compile or the link fails.

## Checking

| Command | What it shows |
|---|---|
| `build/vendor-php-src.sh check` | The directory matches its manifest and is pristine (CI checks job; needs no php-src and no network). |
| `build/test-phpt-tree.sh` | The test tree assembles from this directory alone and an edited fixture is refused. It also covers the runners' refusal to test a binary other than the one they were given (CI checks job; hermetic). |
| `build/libphp-build.sh <outdir>` | The set is sufficient: everything compiles and links against the SDK, with every command line in `<outdir>/commands.log`, and the dependency audit passes. |
