# async/: what branch async adds to main's build

Branch `async` is `main` plus the multi-request executors (`pool.executor =
fiber`). Everything async-only that is not a new `sapi/fpmng/fpm/`
file lives here, so main's build contract stays intact and a merge from main
never conflicts with it:

- main carries **no php-src patch** and its vendored `third_party/php-src/` is
  pristine (`build/vendor-php-src.sh check`). Nothing here changes that.
- main's SDK build (`build/libphp-build.sh`) compiles **no** fiber/coop
  source and asserts the produced binary has none of their symbols
  (`async/check-no-fiber-symbols.sh`). The fiber executor needs changes
  inside php-src, which a distribution `libphp` cannot carry.
- the fiber build is therefore the from-source flow only, and CI builds it in
  `.github/workflows/async-fiber.yml`.

| Path | What |
|---|---|
| `patches/0007-fiber-tls-nonblocking-transports.patch` | `ext/openssl`: TLS handshake and IO suspend the request fiber; gated on `HAVE_FPMNG_FIBER_TLS`, upstream behavior otherwise |
| `patches/0008-fiber-stream-select.patch` | `ext/standard/streamsfuncs.c`: `stream_select()` waits through the fiber scheduler; gated on `HAVE_FPMNG_FIBER` |
| `apply-patches.sh <php-src>` | applies the two patches (idempotent); used only by `prepare.sh` |
| `prepare.sh <php-src>` | main's `build/prepare.sh`, then the fiber source split in `config.m4`, then the patches |
| `build-tree.sh <tree> <repo> <ref> <flags...>` | fresh php-src checkout, `prepare.sh`, `buildconf`, `configure`; keeps the configure log in the tree |
| `check-no-fiber-symbols.sh <binary>` | fails if the binary carries fiber/coop symbols; run by `build/libphp-build.sh` |

Docs that moved here from main (#601), kept for the fiber executor:

- `async/docs/frameworks-fiber.md`: Symfony, Laravel and Slim 4 on `pool.executor = fiber`
  (the former README section and `docs/frameworks.md` of main; the harness is `async/tests/frameworks/`)
- `async/tests/frameworks/`: the fiber probe harness itself (Symfony, Laravel, Slim 4), moved here
  from main by #602; main keeps only a Slim 4 smoke test for `classic` and `worker`
- `async/docs/FASTCGI_NG_OPTIMIZATION.md`: the retired `fastcgi-ng` plan and measurements (historical)

0001-0005 were dropped on async together with main (#589-#592): nothing on
this branch calls what they added, and 0007/0008 touch only `ext/openssl/*`
and `ext/standard/streamsfuncs.c`.

## Why 0007 (fiber TLS transports)

`pool.executor = fiber` suspends the request fiber instead of blocking on
`tcp`/`unix` sockets (`sapi/fpmng/fpm/fpm_pool_fiber_xport.c`), but TLS lives
inside ext/openssl's own loops, which poll internally: `https://`, Guzzle's
stream handler and TLS-wrapped MySQL/Redis would block the whole process. The
interception must sit inside `php_openssl_enable_crypto` and
`php_openssl_sockop_io` (a handshake is a sequence of reads and writes, and
suspension has to be possible inside it), which cannot be done from `sapi/`
alone. Fully gated on `HAVE_FPMNG_FIBER_TLS` (off unless `--enable-fpmng-fiber`
with a static ext/openssl). Valid for php-8.5.9 and master.

## Why 0008 (stream_select)

`stream_select()` called `php_select()` directly, so a fiber request waiting in
it blocked the process. Gated on `HAVE_FPMNG_FIBER`; outside a fiber request
the fallback is the real `select()`. Valid for the PHP-8.5 branch
(67d1476d4d80, 8.5.11-dev) and php-8.5.9.

Both patches are fpm-ng behavior with nothing upstream; they expire by
redesign (the executor deletion decisions are in the Async-Plan wiki page).

## Building the fiber binary

```sh
async/build-tree.sh "$PWD/php-src-fiber" https://github.com/php/php-src.git php-8.5.9 \
  --disable-all --enable-fpmng --enable-fpmng-tls --enable-fpmng-acme \
  --enable-session --with-openssl --enable-fpmng-debug-clock --enable-fpmng-fiber
make -C php-src-fiber -j2 fpmng cli
```

Do not run `build/prepare.sh` on its own for this: `config.m4` refuses a tree
whose always-built list names fiber sources.
