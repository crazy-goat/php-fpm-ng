# Spike #198: linking php-fpm-ng against a distribution libphp

Measured 2026-09-11 on the poligon (192.168.8.50, Ubuntu 26.04.1 LTS) and in an
`alpine:edge` container on the same host. Artifacts: `~/spike198/` on that host.

**Amended by issue #420.** `patches/0006` and the
`zend_signal_use_persistent_handlers()` shim it required are gone, along with
the pool types that used them (`fastcgi-ng` retired by #376, `http` retired by
#388 into `pool.type = gateway`). Mentions of 0006 and of those pool types
below are historical measurements, not current requirements; the NTS
`zend_signal_init()` compatibility layer this report is about is unchanged and
still in scope.

## Result

**It links, it starts, and it serves requests on both modes the spike asked
about (`pool.type = fastcgi` and `pool.type = http-direct`).** On Ubuntu the
owned `.phpt` suite reports **PASS=48, FAIL=1, SKIP=10 of 59**, against
**49/0/10** for the normal build. The single failure is a test that reads
`Configure Command` out of `php-fpm-ng -i`, which a distribution PHP does not
print at all (`php8.5 -i | grep -c 'Configure Command'` is `0` on Ubuntu too).

| | libphp used | binary | `-v` reports |
|---|---|---|---|
| Ubuntu 26.04 | `/usr/lib/libphp8.5.so` (`libphp8.5-embed` 8.5.4-0ubuntu1.3) | 1.6 MB + the 10 MB `.so` | `PHP 8.5.4 (fpm-fcgi) (built: Sep 2 2026) (NTS)` |
| Alpine edge | `/usr/lib/php85/libphp.so` (`php85-embed` 8.5.10) | links, starts, serves `http-direct` | `PHP 8.5.10 (fpm-fcgi) (built: Aug 26 2026) (NTS)` |

58 translation units, 27 s single-threaded on the poligon (`time` over the
compile step), against 711 files for the full php-src build.

## The `zend_signal_init` gap: resolved, not a blocker

`Zend/zend_signal.h:94` declares `zend_signal_init()` without `ZEND_API`, so
`-fvisibility=hidden` keeps it out of the shipped `.so`. FPM calls it once per
child from `fpm_signals_init_child()` (`sapi/fpm/fpm/fpm_signals.c:252`), after
the child has installed its own handlers. What it does
(`Zend/zend_signal.c:390-407`) is snapshot the current handlers into the
file-static `global_orig_handlers`, which `zend_signal_activate()` then copies
into `SIGG(handlers)` at the start of every request. The file-static cannot be
written from outside the library.

`zend_signal_startup()` **is** exported, and its last statement is a call to
`zend_signal_init()` (`Zend/zend_signal.c:443`). Everything it adds on top is
idempotent at the point FPM needs the snapshot: `zend_signal_globals_ctor()`
(memset of the globals, `reset = 1`, rebuild of the pending-queue free list)
and recomputing `global_sigmask`. The child has not run a request yet, so the
globals still hold exactly the post-ctor state the master left them in. So the
shim is three lines:

```c
void zend_signal_init(void) { zend_signal_startup(); }
```

**This is not valid under ZTS** — there `zend_signal_startup()` calls
`ts_allocate_fast_id()`, and a second call would be a bug. Both distribution
builds and ours are NTS.

The snapshot is genuinely needed, and the owned suite catches its absence. A
counterfactual build with `void zend_signal_init(void) {}` produces:

```
PASS=47 FAIL=2 SKIP=10        (versus 48/1/10 with the shim)
fpmng-http-direct-lifecycle.phpt:
    005- graceful-stop: ok
    006+ Fatal error: Uncaught RuntimeException: graceful stop truncated request
```

Without the snapshot the child's `SIGQUIT` handler (`sig_soft_quit`) is not
what `zend_signal_activate()` restores, so a graceful stop truncates the
in-flight request instead of letting it finish.

The other missing symbol, `zend_signal_use_persistent_handlers()`
(`patches/0006`), is called only for `pool.type = fastcgi-ng` and
`pool.type = http` (`sapi/fpmng/fpm/fpm.c:184-187`). A no-op keeps the link
closed for the two supported modes; it means this build must not be used to
judge those two pool types.

## How it was built

No php-src core is compiled. Sources: the `PHP_FPMNG_FILES` list out of the
prepared `sapi/fpmng/config.m4`, plus `fpm_trace.c` + `fpm_trace_pread.c`,
plus our patched `main/fastcgi.c`, plus `ext/fpmng_metrics/fpmng_metrics.c`,
plus the shim above.

```sh
CORE_INC=$(php-config8.5 --includes)
INC="-I$OUT/fcgi-inc $CORE_INC -I$SRC/sapi/fpmng -I$SRC/sapi/fpmng/fpm \
     -I$SRC/ext/fpmng_metrics -I$OUT/compat -include $OUT/compat/spike_decls.h"
DEFS='-DHAVE_CONFIG_H -DHAVE_EPOLL=1 -DHAVE_SELECT=1 -DHAVE_BUILTIN_ATOMIC=1 \
      -DHAVE_LQ_TCP_INFO=1 -DHAVE_TIMES=1 -DHAVE_FPM_HTTP=1 \
      -DHAVE_FPM_HTTP_TLS=1 -DHAVE_CLEARENV=1 -DPROC_MEM_FILE="/proc/%d/mem"'
CFLAGS="-D_GNU_SOURCE -O2 -g -fno-strict-aliasing"
gcc ... -lphp8.5 -levent -levent_openssl -lssl -lcrypto -lm -ldl -lpthread -lrt -Wl,-E
```

Details that cost time and would cost it again:

- `$OUT/fcgi-inc` holds a copy of our **patched** `main/fastcgi.h`, first on the
  include path. `php8.5-dev` ships its own `main/fastcgi.h`, and it is the
  unpatched one; a plain `-I$SRC/main` would also shadow `php.h`.
- The `-D` list is what `sapi/fpm/config.m4` and `sapi/fpmng/config.m4` would
  have produced. There is no `configure` run on this path, so every
  `AC_DEFINE` has to be supplied by hand. Getting `HAVE_FPM_HTTP` and
  `HAVE_FPM_HTTP_TLS` wrong is expensive rather than fatal: the first attempt
  scored 26/22/11 purely because the gateway and TLS were compiled out.
- `-D_GNU_SOURCE` is required: `zend_operators.h:235` uses `memrchr()`.
- The distribution `php_config.h` describes the **distribution's** build
  machine. Alpine's carries `HAVE_FPM_ACL`, `HAVE_TIMES`, `HAVE_EPOLL`,
  `HAVE_CLEARENV`, `PROC_MEM_FILE "mem"`; Ubuntu's carries none of them. On
  Alpine that means the build needs `acl-dev` and `-lacl` whether we want FPM's
  ACL support or not.

## Caveats

- **Version.** Ubuntu ships 8.5.4, Alpine 8.5.10, we pin 8.5.9. A green run
  here is evidence that *our SAPI code is ABI- and API-compatible with the 8.5
  line as two distributions build it*. It is **not** evidence about php-8.5.9,
  and a failure on this path would have to be triaged against the version
  before it is believed.
- **Extension set.** The canonical build is
  `--disable-all --enable-fpmng --enable-session --with-openssl`. The Ubuntu
  libphp brings 40 extensions and OPcache loaded from the distribution's
  `.ini`. So a pass here does not say the canonical minimal build passes, and
  `fpmng-http-direct-worker-opcache.phpt` in particular passed against a
  *different* OPcache situation than CI's.
- **`ext/fpmng_metrics` is not registered as a PHP extension** on this path.
  Its C functions are linked in (so `/metrics` still renders and
  `fpmng-status-endpoints.phpt` passes), but the userland functions are absent
  — `php_module_startup()` takes one additional module and `cgi_module_entry`
  already occupies it. Nothing in the suite covers the userland side today.
- **Pool types this build cannot speak for:** `fastcgi-ng` and `http` (the
  `patches/0006` no-op), and the fiber/async executors (now on branch
  `async`, patches formerly numbered 0007/0008 lived inside libphp).
  `static-musl` cannot use this path at all — Alpine ships no static
  libphp.
- The one FAIL, `fpmng-config-rejected-directives.phpt`, is a harness
  assumption, not a defect: it inspects `Configure Command` to decide whether
  the async executor's build flag was set, and a distribution PHP prints no
  such line.
