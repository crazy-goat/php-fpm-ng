# Payloads carried by the binary

`php-fpm-ng` can carry PHP code inside its own executable and run it from
there. Issue #171 builds the mechanism and uses it for one thing: the
project's own ACME client, which every build embeds so that
`cron.script = fpmng-dist://acme/renew.php` works on a machine where no
`.php` file was ever installed.

This is the operator-facing reference. The format's rationale — why a
backwards-linked chain instead of a directory, why a URI scheme instead of a
reserved directive value — is in the header comments of
[`sapi/fpmng/fpm/fpm_payload.h`](../sapi/fpmng/fpm/fpm_payload.h) and
[`sapi/fpmng/fpm/fpm_payload_dist.h`](../sapi/fpmng/fpm/fpm_payload_dist.h).

## Using an embedded script

Any directive that names a PHP script accepts an embedded path:

```ini
[acme]
pool.type = cron
cron.schedule = @daily
cron.script = fpmng-dist://acme/renew.php
```

The names under `fpmng-dist://acme/` are the files of
`sapi/fpmng/acme/` — `renew.php`, `client.php`, `jws.php`, `lock.php`,
`http.php`, `state.php`. `require`/`include` from inside an embedded script
resolves through the same scheme, so `require __DIR__ . '/client.php'` in
`renew.php` finds the embedded `client.php` and not a file on disk.

Embedded files are **read-only**. Opening one for writing or appending
fails; `stat()` reports mode `0100444` and the real size.

A name that is not embedded is refused **at startup**, with the pool named
in the message, rather than at the first run:

```
[pool acme] cron.script 'fpmng-dist://acme/nope.php': no such file in the embedded distribution payload
```

A binary built without a payload is refused the same way, with
`this build carries no embedded distribution payload` (see *strip* below).

## What is in a binary

`build/payload-pack.php list` walks the chain in a binary and prints one line
per entry, newest first:

```
$ php build/payload-pack.php list --binary=/usr/bin/php-fpm-ng
kind=1 offset=1852560 size=73887 sha256=575a46e1a10b1678…
```

`kind=1` is the distribution payload (our code), `kind=2` an application
payload. A binary with nothing appended prints nothing and exits 0 — "no
payload" is a normal state, not an error.

## Integrity

Each entry carries a SHA-256 of its own data, checked before anything is
handed to PHP. A truncated download or a partial write is therefore reported
as a named payload failure instead of surfacing as a PHP parse error
somewhere inside the ACME client.

It is **not** a signature. Anyone who can rewrite the binary can rewrite the
digest next to the data; protecting against that needs a key and is out of
scope here.

## strip(1) removes payloads

Appended data is not part of any ELF section, so `strip` drops it and the
binary silently goes back to having no payload. Embed *after* stripping, or
do not strip at all — which is what this repository's packaging does:
`build/package-apk.sh` builds with `!strip` and `build/package-deb.sh` never
strips.

`build/embed-payload.sh` is the one step every build path calls after linking
(`build/libphp-build.sh`, for the packages and for the binary the test suite
runs), and it reads
the entry back to prove the append landed. It is idempotent: a binary that
already carries the archive it would write is left alone, so a reused build
tree does not collect one archive per run.

## Packing an application

```
php-fpm-ng pack <app.phar> <php.ini> <fpm.conf> -o <output>
```

writes `<output>`: a copy of the running `php-fpm-ng` with the three files
appended as one `kind=2` application payload. All three are mandatory and are
stored byte for byte. Nothing is parsed or run: the PHAR and its stub are never
opened by PHP, so packing executes no application code, and it needs no PHP CLI.

```
$ php-fpm-ng pack app.phar php.ini fpm.conf -o myapp
php-fpm-ng pack: wrote myapp (PHAR 48213 bytes, php.ini 311 bytes, fpm.conf 702 bytes)
$ php build/payload-pack.php list --binary=myapp
kind=2 offset=1926447 size=49366 sha256=9c1f0e2a…
kind=1 offset=1852560 size=73887 sha256=575a46e1a10b1678…
```

The payload is an archive with the entries `fpm.conf`, `php.ini` and `app.phar`,
in that order. The distribution entry in front of it stays byte-identical.

`pack` refuses, with a message naming the file, and writes nothing:

- an input that is missing, unreadable, not a regular file or empty;
- a PHAR without a `__HALT_COMPILER` marker, and a `php.ini` or `fpm.conf` with a
  NUL byte;
- an `<output>` that already exists (remove it first);
- a running binary that already carries an application payload. Repack from an
  unpacked `php-fpm-ng`; to upgrade, pack the new inputs with the new binary.

The output is written under a temporary name next to it, read back with the same
reader the runtime uses (including the SHA-256 check) and only then moved into
place. The inputs are not validated beyond that: a PHAR that PHP cannot read, or
an `fpm.conf` that `php-fpm-ng -t` rejects, is packed as it is. Run `-t` on the
`fpm.conf` first.

This is the packing half of the single-file application (epic #425). Running a
packed executable is #430; until it lands, a packed `php-fpm-ng` behaves like the
binary it was copied from. The same external requirements apply as for any
`php-fpm-ng`: the output carries application code and configuration, and still
needs the supported PHP runtime libraries and extensions on the host. Writable
state (logs, cache, sessions, uploads) is not embedded.

## Appending your own

```
php build/payload-pack.php append --binary=PATH --kind=application \
    --dir=DIR [--prefix=P]
```

Appending never rewrites bytes an earlier append wrote: adding an application
payload leaves the distribution entry, digest included, byte-identical. Only
`--kind=distribution` is reachable from `fpmng-dist://`; the application kind
is what `pack` writes (see above); it has no configuration surface yet.
