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
an `fpm.conf` that `php-fpm-ng -t` rejects, is packed as it is. Check the result
with `./myapp -t` (an unpacked `php-fpm-ng -t` rejects `fpmng-app://`, below).

The same external requirements apply as for any `php-fpm-ng`: the output carries
application code and configuration, and still needs the supported PHP runtime
libraries and extensions on the host. Writable state (logs, cache, sessions,
uploads) is not embedded.

## Running a packed executable

A `php-fpm-ng` that carries an application payload behaves as that application
(issue #430). Nothing else has to exist on the host: the original PHAR, php.ini,
fpm.conf and code directory may be gone.

**Configuration (decision D1).** The embedded `php.ini` is the only php.ini: the
host's php.ini, its scan directory and `PHPRC` are ignored, as with `-n`. The
embedded `fpm.conf` is the only configuration. `main()` achieves this by putting
`-c <php.ini> -y <fpm.conf>` in front of the command line and setting an empty
`PHP_INI_SCAN_DIR` (`-n` would drop the file too). Arguments the operator adds
(`-t`, `-d`, `-c`, `-y`) come after and win, as on any binary; nothing is merged.
Inside `fpm.conf` and `php.ini`, `include=` and `extension=` mean what they mean
anywhere: a file named there is a host file.

**Naming an entry.** Each script path of the configuration selects an archive
entry with `fpmng-app://<entry>`:

```
[web]
pool.type = http-direct
pm = static
chdir = /
http.front_controller = fpmng-app://public/index.php
[jobs]
pool.type = supervisor
supervisor.script = fpmng-app://bin/worker.php
```

The same spelling works for `cron.script`. The entry must be listed in the
PHAR's manifest (checked at startup); an empty, absolute, `..`, `//` or
backslash entry is refused. `fpmng-app://` is replaced everywhere in `fpm.conf`
and `php.ini` by the real `phar://` path before the text is read. A plain
`php-fpm-ng` has no application: it refuses `fpmng-app://` with a message. With
`http-direct` the front controller's directory is the PHAR, so `chdir` is only
the working directory (use an absolute directory such as `/`); every request,
whatever its path, goes to the front controller, so nothing outside the entry
point, and no archive or host file, can be reached by URL. `http.static = yes` is refused for such a pool
(public files inside a PHAR are not served). Not supported: `pool.type = gateway`/`fastcgi`
scripts and `.user.ini` for an archive front controller; the PHAR stub is never
run.

**State directory.** PHP cannot open a PHAR appended to an executable, so the
three inputs are written to `<base>/php-fpm-ng-app-<euid>/<sha256 of the
payload>/`, where `<base>` is `$FPMNG_APP_DIR`, else `$TMPDIR`, else `/tmp`.
`php-fpm-ng-app-<euid>` and the digest directory are owned by the effective user,
mode 0700 (0755 when started as root, so that workers that drop privileges can
read); a symlink in the way is refused; files are written under a temporary name,
renamed into place, and are read-only. A file already there is reused only if it
is byte-for-byte what the run would write. Because the name is the digest, two
different applications never share a path, so OPcache cannot hand one the
other's scripts. Directories are never removed by the runtime (a master started
before an upgrade, or a worker finishing a request, may still read the old one);
delete the whole `php-fpm-ng-app-<euid>` tree while the service is stopped.
Writable application state (logs, cache, sessions, uploads) must live outside it.

The embedded `php.ini` must say `extension=phar` (PHP is not built with Phar
loaded by default here); without it startup fails with that advice.

**Startup failures** exit with status 78 (before the engine) or fail
configuration post-processing (before "ready to handle connections"), and name the
cause: a payload that does not verify (flipped byte, truncation, also a binary
cut short so that the payload record is gone), an unusable PHAR (no
`__HALT_COMPILER();`, truncated manifest), a missing PHAR extension or an
`extension=` of the embedded php.ini that PHP did not load, an entry that is not
in the PHAR, an invalid `fpm.conf`. There is no fallback to a host php.ini or
fpm.conf. The running executable is found through `/proc/self/exe` (also when
it was deleted) or argv[0]; a re-exec of a packed master that can find neither
fails with exit 78. A first start of a binary that finds neither cannot know
whether it carries a payload and runs as a plain `php-fpm-ng` (needs no `/proc`
and an argv[0] without a slash, i.e. started through `PATH` on a system without
`/proc`). The ABI guard is an ELF constructor and runs before any of this.

**Lifecycle.** SIGTERM/SIGQUIT stop, SIGUSR1 reopens logs and SIGUSR2 re-execs the
master exactly as for any `php-fpm-ng`. To upgrade, pack the new application
with an unpacked `php-fpm-ng`, rename the result over the executable (not
write into it), and send SIGUSR2: the master re-execs, reads the payload the file
holds now and serves it from its new state directory. The arguments `main()`
injected (`-c <state>/php.ini -y <state>/fpm.conf`) are recognised by that exact
shape and stripped on the re-exec, which rebuilds them from the new payload; no
environment variable is involved, and any other arguments are never touched; replacing the executable with an unpacked `php-fpm-ng` makes the
re-exec run as a plain binary. `fpmng-pack-run.phpt` covers this.

OPcache note: OPcache caches the entries of the PHAR read through `phar://`
(`fpmng-pack-run.phpt` checks `num_cached_scripts > 0`), keyed by the digest
path, so a repack plus SIGUSR2 never serves the old payload's scripts: the new
state directory gives new keys, and timestamp validation covers the rest. One
condition: OPcache silently skips a script whose modification time is 0
(`ext/opcache/ZendAccelerator.c`, the timestamp check in PHP 8.5), and a PHAR
entry stored with mtime 0 reports exactly that. Such a PHAR still runs, but its
scripts are not cached. Setting `opcache.validate_timestamps=0` alone does not
help, because the default `opcache.file_update_protection=2` triggers the same
skip. Build the PHAR so that its entries carry a real mtime. Measured: the
hand-written test fixture wrote mtime 0; other tools that normalize timestamps
may do the same (not measured).

## Appending your own

```
php build/payload-pack.php append --binary=PATH --kind=application \
    --dir=DIR [--prefix=P]
```

Appending never rewrites bytes an earlier append wrote: adding an application
payload leaves the distribution entry, digest included, byte-identical. Only
`--kind=distribution` is reachable from `fpmng-dist://`; the application kind
is what `pack` writes (see above) and `main()` runs (see "Running a packed
executable").
