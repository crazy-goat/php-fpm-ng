# Known problems in the Fiber executor

State as of 2026-09-06, after hardening from the `fiber-hardening` branch. `pool.executor = fiber` is experimental and is not meant for production use.

## Scope

The problems concern the configuration:

```ini
pool.type = fastcgi-ng | http
pool.executor = fiber
```

They do not concern the production paths `fastcgi`, `fastcgi-ng/classic` or `http/classic`. All the code described below lives in `sapi/fpmng/fpm/fpm_pool_coop.c` (the shared "many requests in one process" core), in the pool type's `validate` callback, and in container startup — `fpm_conf.c`, `fpm_children.c` and `fpm_pool_type.h` were not touched, per the extensibility contract.

Fiber requires OPcache disabled and `max_execution_time` set to zero:

```ini
php_admin_value[opcache.enable] = 0
php_admin_value[max_execution_time] = 0
```

## Summary: what is closed and by what

None of the three problems has been **fixed** — fiber still has no per-request timeout and no signal isolation. Instead of silently not working, the configuration **refuses to start**, and the API **is blocked**:

| Problem | Mechanism | What it is | What remains open |
|---|---|---|---|
| `max_execution_time` != 0 does not interrupt the request | `fpm_coop_validate()`: effective value (pool -> php.ini) != 0 => `ALERT` + startup refusal | refusal | there is no per-fiber timeout |
| `set_time_limit(N)` arms the process-wide timer | `fpm_coop_req_run()`: `zend_restore_ini_entry("max_execution_time", DEACTIVATE)` after the script; `php_admin_value` additionally blocks `ini_set` | mitigation | during the request, SIGPROF can hit someone else's fiber |
| `pcntl_signal()` leaks between requests | `fpm_coop_container_start()`: `zend_disable_functions()` on the process-wide pcntl API | block | signal handlers are process-wide; no isolation |
| `pcntl_fork()` duplicates the multi-request process | the same block (`pcntl_fork`, `pcntl_rfork`, `pcntl_forkx`, `pcntl_exec`) | block | — |

## `max_execution_time` does not interrupt the request

A request running an infinite loop is not interrupted once the configured limit is exceeded:

```php
<?php while (true) {}
```

### Cause

The Fiber executor does not run a full `php_request_startup()` and `php_request_shutdown()` separately for every request — it does one `php_request_startup()` for the life of the process (a "request container", `fpm_coop_container_start`). The Zend timeout is process-wide: one `setitimer()` (`ITIMER_PROF`/`SIGPROF`; on aarch64 macOS `ITIMER_REAL`/`SIGALRM`) in `zend_set_timeout_ex()`. One timer does not represent the independent deadlines of many requests. The container had always done `zend_unset_timeout()`, so a non-zero value was accepted and silently not enforced.

### State after the change: refused at validation

`fpm_coop_validate()` computes the pool's effective value — `php_admin_value`/`php_value[max_execution_time]` from the pool (admin before value, same as `fpm_php_apply_defines` in `fpm_php.c`), and when the pool does not set it, the value from php.ini via `zend_ini_long()` (default `30`, `main/main.c`). A value != 0 ends in a refusal to start, the same as with OPcache enabled:

```text
ALERT: [pool fiber] pool.executor = fiber: max_execution_time = 30 is not enforced (the Zend timeout is one setitimer()/SIGPROF timer per process, and this process runs many requests at once; see docs/fiber_errors.md); set php_admin_value[max_execution_time] = 0 in this pool or max_execution_time = 0 in php.ini
ERROR: failed to post process the configuration
ERROR: FPM initialization failed
```

Consequence: **every** fiber pool without an explicit `php_admin_value[max_execution_time] = 0` stops starting, because the default ini value is 30. That is intentional — the same way the OPcache-disabled requirement works. Measured: `php_admin_value = 1` -> refusal; no directive (default 30) -> refusal; `php_value = 5` + `php_admin_value = 0` -> starts (admin wins); `= 0` -> starts and serves requests.

### `set_time_limit()` — the hole that remained, and its mitigation

Validation only rejects the configuration. `set_time_limit(N)` (`main/main.c`, `PHP_FUNCTION(set_time_limit)`) goes through `zend_alter_ini_entry_ex(..., PHP_INI_STAGE_RUNTIME)` to `OnUpdateTimeout`, which calls `zend_set_timeout()` — the process timer gets armed. When it fires, `zend_timeout_handler` sets `EG(timed_out)` and the request that executes the next opcode — not necessarily the one that called `set_time_limit` — gets "Maximum execution time exceeded". This is not a crash: `zend_try` in `fpm_coop_execute` catches the bailout.

Mitigation in `fpm_coop_req_run()`, after the script and after sending the headers: `zend_restore_ini_entry("max_execution_time", ZEND_INI_STAGE_DEACTIVATE)` — exactly what `zend_ini_deactivate()` does for all entries in classic `php_request_shutdown()`. `OnUpdateTimeout` in stage DEACTIVATE disarms the timer and does **not** re-arm it, and the ini value goes back to its initial one. Neither the timer nor `ini_get('max_execution_time')` survive the request that changed them.

Two observations from measurements:

- With `php_admin_value[max_execution_time] = 0` (the recommended configuration), `set_time_limit(1)` returns `false` — `php_admin_value` blocks `ini_set` (`fpm_php_zend_ini_alter_master` with `ZEND_INI_SYSTEM`). The timer never arms; the mitigation has nothing to do.
- With `php_value[max_execution_time] = 0`, `set_time_limit(1)` returns `true`; the request that calls it ends, the next request with a 2 s CPU loop survives, and `ini_get` in the following one shows `0` again.

**Open:** during the request itself that called `set_time_limit(N)`, the timer is armed and can hit another request's fiber. Closing this requires a per-fiber timeout (below), not a cheap patch. We do not block `set_time_limit` — frameworks call `set_time_limit(0)` routinely, and zero is harmless.

### Full fix (separate project)

1. deadline stored separately for each fiber;
2. timer queue integrated with the scheduler;
3. switching the active timer on suspend/resume;
4. assigning `SIGPROF` to the fiber currently executing;
5. safely interrupting only one request;
6. handling CPU-bound code that never returns to the event loop;
7. confirming that the bailout does not corrupt shared process state.

## `pcntl_signal()` handlers leak between requests

A handler set in one request remained visible in the next request handled by the same process:

```php
// Request 1
pcntl_signal(SIGUSR1, static function (): void {});

// Request 2
var_dump(pcntl_signal_get_handler(SIGUSR1) === SIG_DFL); // false
```

### Cause

Two states leak, not one:

- the logical pcntl table `PCNTL_G(php_signal_table)` (`ext/pcntl/php_pcntl.h`) — this is what `pcntl_signal_get_handler()` reads;
- the kernel disposition and the Zend table: `php_signal4()` (`ext/pcntl/php_signal.c`) -> `zend_sigaction()`, which writes `SIGG(handlers)[signo-1]` **and** installs `zend_signal_handler_defer` in the kernel (`Zend/zend_signal.c`).

Classic resets both in `PHP_RSHUTDOWN(pcntl)` (`ext/pcntl/pcntl.c`: `php_signal(signo, SIG_DFL)` for every entry, `zend_hash_destroy` of the table), then `zend_signal_deactivate()` in `php_request_shutdown()`, and on the next request `zend_signal_activate()` does `memcpy(&SIGG(handlers), &global_orig_handlers, ...)`. Fiber does RINIT once per process, so the table lives until the process ends. `zend_signal_activate()` alone between requests achieves nothing: it does not touch `PCNTL_G(php_signal_table)`.

### State after the change: process-wide pcntl API blocked

At the start of `fpm_coop_container_start()`, if `zend_get_module_started("pcntl") == SUCCESS`, `zend_disable_functions()` is called on the `fpm_coop_disabled_functions` list (next to `fpm_coop_rejects`):

```text
pcntl_signal, pcntl_signal_get_handler, pcntl_signal_dispatch, pcntl_async_signals,
pcntl_sigprocmask, pcntl_sigwaitinfo, pcntl_sigtimedwait, pcntl_alarm,
pcntl_fork, pcntl_rfork, pcntl_forkx, pcntl_exec
```

This is the same mechanism `fpm_php.c` (`fpm_php_apply_defines_ex`) uses to apply `php_admin_value[disable_functions]` in the child — removing the entry from `CG(function_table)` before the first request, after `fpm_php_init_child` (MINIT and `php_admin_value[extension]` already behind us). The function disappears: calling it gives `Error: Call to undefined function pcntl_signal()`, and `function_exists()` returns `false`, so libraries with a fallback keep working. `extension_loaded('pcntl')` still returns `true`. `pcntl_alarm` is on the list because `SIGALRM` is process-wide (and on aarch64 macOS it is the Zend timeout signal). `pcntl_wait*`, `pcntl_wifexited` etc. stay — without fork they are harmless, and they're useful with `proc_open`. `proc_open`/`popen`/`exec` stay: they do fork+exec, and the child does not return to the scheduler.

The container logs one `NOTICE` with the list of disabled functions. Note: this is a **worker** log; with `error_log = /dev/stderr` and without `catch_workers_output = yes` you won't see it, same as any other child log.

Measured via `http/fiber`:

```text
pcntl_loaded=true
function_exists(pcntl_signal)=false
function_exists(pcntl_fork)=false
function_exists(pcntl_waitpid)=true
pcntl_signal=Error: Call to undefined function pcntl_signal()
pcntl_fork=Error: Call to undefined function pcntl_fork()
```

The same script via `http/classic` and `fastcgi` (no config changes, `max_execution_time` default 30): `function_exists(pcntl_signal)=true`, `pcntl_signal=OK handler=SET`, `pcntl_fork=OK`, the worker keeps serving requests, no alerts in the log.

### Rejected: resetting handlers via pcntl RSHUTDOWN/RINIT between requests

Considered "cheap reset": after every request, in `fpm_coop_req_run()`, call the `pcntl` module's `request_shutdown_func` + `request_startup_func` from `module_registry` (`zend_hash_str_find_ptr(&module_registry, "pcntl", 5)`, fields of `zend_module_entry` in `Zend/zend_modules.h`). Technically ~15 lines. **Rejected** for three reasons, each disqualifying on its own:

1. **Tick functions grow per request.** `PHP_RINIT(pcntl)` calls `php_add_tick_function(pcntl_signal_dispatch_tick_function, NULL)` on every invocation (`ext/pcntl/pcntl.c`). `PG(tick_functions)` grows by one entry per request, and `php_run_ticks()` iterates the whole list (`main/php_ticks.c`) — tick cost is O(number of requests), without end. Selective removal is not possible: the tick function is `static` in `pcntl.c`, and `php_deactivate_ticks()` clears the entire list, including the `run_user_tick_functions` tick from `ext/standard/basic_functions.c`. Bypassing RINIT and zeroing the table directly would require `PCNTL_G()`, i.e. the `pcntl_globals` symbol, which is linkable when pcntl is static but is not portably reachable when pcntl is loaded as `.so`.
2. **No isolation during the request itself.** Handlers are process-wide *during* the request: request B calling `pcntl_signal(SIGUSR1, ...)` overwrites A's handler, and the signal gets delivered to whichever fiber happens to be executing a tick or handling `vm_interrupt`. A reset after the request ends only fixes the "SET -> LEAK" test, not isolation.
3. **`SIG_DFL` breaks a parallel fiber.** `PHP_RSHUTDOWN(pcntl)` after request A does `php_signal(signo, SIG_DFL)` also for signals that a still-running parallel request B depends on. The next such signal kills the entire multi-request process (default disposition of `SIGUSR1`/`SIGTERM`).

Conclusion: the reset would be cosmetic and a trap — it removes the symptom from the test, and leaves and worsens the real behavior. The block is honest: it says outright that this API does not exist in this executor.

### Full fix (separate branch)

Handler state has to be attached to the Fiber request context:

1. save it on fiber suspend;
2. restore it before resume;
3. reset it after the request ends;
4. preserve internal handlers required by Zend and FPM;
5. test several concurrent requests setting different handlers.

Until this exists, the block stays.

## `pcntl_fork()` inside a request

`pcntl_fork()` duplicates the entire multi-request process along with the libevent scheduler, the descriptors of all connections, and requests in flight. `exit()` alone in the child does not end the FPM process — it ends the script, after which the child goes through the rest of `fpm_coop_req_run()` (sending a **duplicate response** on the shared descriptor) and returns to the worker loop as a clone. A parent waiting via `pcntl_waitpid()` can block forever. `pcntl_exec()` has the same problem from the other side: it replaces the entire process image.

### State after the change: blocked

`pcntl_fork`, `pcntl_rfork`, `pcntl_forkx` and `pcntl_exec` are on the `fpm_coop_disabled_functions` list (see above). Measured: `pcntl_fork=Error: Call to undefined function pcntl_fork()`, `function_exists('pcntl_fork') === false`.

### Rejected alternatives

- `pthread_atfork()` with `_exit()` in the child — breaks `proc_open`/`popen`, which fork internally. Ruled out.
- A PID guard in the scheduler (`getpid()` compared in `fpm_fiber_after_switch()` against the PID remembered at startup, `_exit()` on mismatch) — closes the parent's hang, but not correctness: the child reaches the guard only after sending the duplicate response. Redundant once `pcntl_fork` is blocked.

## Confirmed working elements

For `fastcgi-ng/fiber` and `http/fiber` on a clean release build of PHP 8.5, previously confirmed:

- small and large responses;
- binary POST;
- FastCGI/HTTP keep-alive and `Connection: close`;
- surviving a client disconnect;
- another request after a client error;
- reload via `SIGUSR2`;
- shutdown via `SIGTERM`;
- the OPcache-disabled requirement.

The hardening from this document was measured on 2026-09-06 on a debug build of `PHP 8.6.0-dev` (master `4e55e35ead7` + 6 patches from `patches/`, `--disable-all --enable-fpmng --enable-pcntl`), macOS aarch64:

- `http/fiber`: refusal to start for `max_execution_time` = 1 and for the default 30; starts for `= 0`; `php_admin_value` wins over `php_value`; requests served; pcntl blocked; `set_time_limit(1)` (with `php_value`) does not survive the request.
- `http/classic` and `fastcgi` in the same binary: `max_execution_time = 30` does not block startup, `pcntl_signal()` and `pcntl_fork()` work, no alerts — production paths untouched.

The same series repeated on the test box (Ubuntu 26.04, x86_64, gcc 15, same commit and same patches) gave identical results. This matters for `set_time_limit`: on Linux the timer is `ITIMER_PROF` (CPU time), so `set_time_limit(1)` in one request, followed by 2 s of CPU loop in the next, would end in "Maximum execution time of 1 second exceeded" without disarming — the log shows 0 such entries, the request survived.

## Recommendation

Do not extend the current fix into a rebuild of the Fiber lifecycle. Order of further work:

1. ~~explicitly reject non-zero `max_execution_time` for Fiber~~ — done (refused at validation);
2. ~~document or block the process-wide `pcntl` API~~ — done (blocked in the container);
3. on a separate branch implement signal handler isolation — only then lift the block;
4. treat per-fiber timeout as a separate project requiring concurrency, bailout and CPU-bound-code tests — only then lift the refusal and the `set_time_limit` mitigation;
5. keep Fiber marked experimental until these problems are resolved.

## EXPERIMENT: `included_files` shared for the process

`FPMNG_SHARED_INCLUDES=1` (via `env[]` in the pool — `clear_env = 1` is the
default, so without this the variable does not reach the child). Disabled by
default.

**What for.** Without this, no application using Composer survives a second
request. The list of included files is per request, the function and class
tables are per process — so the second request thinks `vendor/autoload.php`
was not loaded, loads it again, and redeclares a class that is still alive in
the process. A shared list makes bootstrap a one-time thing WITHOUT
worker-mode and without changes to the application.

**Measured** (bootstrap pattern: `require vendor/autoload.php`,
`require_once` helpers with a function and a class, `$app = require_once
bootstrap/app.php`):

    BEFORE:  req1 ok, req2 and req3 Fatal: Cannot redeclare class ComposerAutoloaderInit...
    AFTER:   req1 ok, req2 ok, req3 ok — 30/30 requests, zero errors in the log

I/O concurrency untouched: 4 parallel `fsockopen` at 500 ms each still take
0,525 s total.

### Two side effects — one to fix, one not

**Loud, to fix:** `require_once` on a second call returns `true`, not the
value the file returned. The pattern
`$app = require_once 'bootstrap/app.php'` gives `app=true` from request 2
onward (measured). To do: cache the value returned by the file and give it
back on subsequent calls instead of `true`.

**Silent, NOT to fix:** code executed at the top of a file pulled in via
`require_once` stops executing from the second request onward. No error, no
warning:

    BEFORE:  req1 init_runs=1   req2 init_runs=1     req3 init_runs=1
    AFTER:   req1 init_runs=1   req2 init_runs=NONE  req3 init_runs=NONE

This is not a bug to remove — it is exactly the semantics intended
("execute once"). This is a REQUIREMENT on the application: nothing
significant may happen as a side effect of loading a file. Frameworks are in
good shape here (bootstrap builds objects and returns them), but this has to
be confirmed on real code, not on a pattern.

### What does NOT change

- **OPcache is still rejected** by validation — checked. Incidentally, shared
  `included_files` largely removes the reason you'd want OPcache in the first
  place: bootstrap compiles once per process, per request only `index.php`
  gets recompiled.
- **Class statics still leak** between requests (`Foo::$hits` grew
  2 -> 3 -> 4). The same risk as in Octane and Swoole.
- `EG(symbol_table)` stays per request — separating it is forced by
  `zend_attach_symbol_table` (SIGABRT measured in NOTES 3t).

### What this experiment did NOT check

Real Symfony or Laravel — only the bootstrap pattern. It's not known how much
of real frameworks relies on the `require_once` return value or on side
effects of loading a file. That's the next step and it's doable right away:
the binary exists.

### Swapping files on disk: deploy REQUIRES a reload

Measured with `FPMNG_SHARED_INCLUDES=1`, swapping both files between requests:

    req1                     entry=ENTRY-1  lib=VERSION-1
    req2 (after the swap)    entry=ENTRY-2  lib=VERSION-1   <- MIXED state
    req3                     entry=ENTRY-2  lib=VERSION-1
    req4 (after SIGUSR2)     entry=ENTRY-2  lib=VERSION-2

The entry script is read from disk on EVERY request, because we execute it
directly, not via `require_once`. Everything it pulls in stays frozen in the
process. So after `git pull` without a reload, the new `index.php` runs on
the old bootstrap — nonsensical errors with no trail.

This is a BEHAVIOR CHANGE, not just a new limitation: previously, swapping a
file with a function gave a loud "Cannot redeclare" fatal on the second
request; now you get silent stale code. For classes loaded via the
autoloader, staleness already existed before (the class sits in the process
table, the autoloader is not called) — this change extends it to the whole
bootstrap.

Rule for the user documentation: **deploy ends with `SIGUSR2`**, the same as
in Octane, Swoole and RoadRunner. Reload is graceful and works (req4). Dev
alternative: `fiber.revalidate_freq` below.

### `fiber.revalidate_freq` — worker replaces itself after a file change

A pool directive, seconds, **default `0` = disabled**. With `N > 0` the
worker remembers the mtime/size/inode of every file at the moment the engine
compiles it (hook `zend_compile_file` in `fpm_pool_coop_reval.c` — the same
moment `opened_path` lands in `EG(included_files)`; the entry script is
skipped, since it's read from disk per request anyway), and every N seconds
runs one `stat()` pass over that table from the event loop. No `stat()` per
request — the cost is (number of files / N) per second, regardless of
traffic. A change to any file (mtime, size, inode after a `rename`, or a
failed `stat()` — file removed during deploy) produces a NOTICE with the file
name and **drains**: the worker stops accepting connections, closes idle
keep-alive, finishes requests in flight and exits with code 0; the master
replaces it the same way it recycles a classic worker after
`pm.max_requests` (`fpm_children_bury`, `restart_child = 1`). This is NOT the
SIGQUIT path (`fcgi_in_shutdown`), which drops requests in flight.

Measured (2026-09-06, `pm.max_children = 1`, with `FPMNG_SHARED_INCLUDES=1`
and without):

    freq = 1:  req1 lib=VERSION-1 pid=2137 | swap | req2 (right away) lib=VERSION-1 pid=2137
               req3 (after 1.5 s) lib=VERSION-2 pid=2166 — NOTICE "lib.php changed on disk (mtime ...)"
    in flight: slow.php (3 s sleeping) during the swap: "worker will exit after
               1 request(s) in flight finish", "draining, 1 request(s) still in flight",
               the request finished after 3.09 s with a correct response; "abandoned" in the log: 0
    freq = 0:  swap -> lib=VERSION-1 same pid after 2.2 s (behavior as before)
    freq = 2:  sweeps every 2.000 s; swap right after a sweep: +0.3 s stale, +1.3 s stale, +2.5 s fresh;
               200 requests in the window -> stat() counter grew by 1 per sweep, not by 200
    fastcgi / http classic: unchanged; directive rejected:
               ALERT: [pool fc] 'fiber.revalidate_freq' is not supported by pool.type = fastcgi
    4 x fsockopen 500 ms parallel: 504 ms total (classic worker: 2017 ms)

Why `0` by default: (1) the process killing itself is new behavior and has
to be opt-in — existing configs keep working as before; (2) in production
there are files loaded via `require` that the application LEGITIMATELY
overwrites while running (compiled Twig/Blade templates,
`bootstrap/cache/*.php`, the DI container) — with the automatic version, every
such write would replace the worker; (3) deploy via `rsync` is not atomic —
a worker could start on half a copied tree and replace itself again in the
next window. In dev, `fiber.revalidate_freq = 1` gives "you copied the files,
the new code runs" without having to remember `SIGUSR2`.

Limitations:

- **Symlink-deploy (`current -> release-N`) is not detected.**
  `EG(included_files)` keeps realpaths, so the files of the old release don't
  change — flipping the symlink doesn't touch their mtime. `SIGUSR2` is still
  needed there.
- During drain, new connections wait in the socket backlog until the
  successor starts (with `pm.max_children = 1`, as long as the longest
  request in flight takes; with more children, the rest pick up the slack).
  Idle gateway keep-alive connections are closed immediately — the gateway
  treats this like EOF after `pm.max_requests` and reconnects; a request the
  gateway sent exactly in that window is lost the same way it is today with
  `pm.max_requests`.
- A request suspended outside the scheduler (`Fiber::suspend()` in the main
  fiber, already logged today as "dropping it") never comes off the
  in-flight counter, so drain never finishes; the tick then logs every
  second "draining, N request(s) still in flight".
- First file state wins: we `stat()` the path right AFTER the first
  compilation; a swap within that microsecond window records the new state
  against the old code.
- Only files going through `zend_compile_file` are tracked: `eval()`, data
  read via `file_get_contents` (YAML/JSON configuration) and non-PHP
  templates are not seen.

## Persistent connections: BLOCKED (measured on the test box)

`EG(persistent_list)` is PROCESS-WIDE, and the coop core does not swap it when
switching requests — so two requests in flight can get the same socket.
Database protocols are request-response, so this is not a slowdown, it's a
protocol gone out of sync.

**Measured** (Ubuntu 26.04, epoll, one worker, 4 parallel requests, same
DSN, MySQL 8):

    PDO with ATTR_PERSISTENT, run 1:  3 requests HANG until the client timeout (60 s),
                                       1 finishes correctly
    PDO with ATTR_PERSISTENT, run 2:  3 x HTTP 502, the fourth
                                       "PDOException: Trying to access array offset on false",
                                       the worker died and was replaced
    PDO without persistent, same test: 1,019 s, clean

    mysqli with "p:" prefix:          did NOT break — four DIFFERENT session
                                       identifiers (215-218), tag_ok=true, 1,017 s

So safety depends on whether a given client tracks connection ownership: PDO
does not, mysqli does. You can't see this from the transport layer and we
don't control it, so **we block both**. Better to refuse loudly when opening
the connection than to desync the protocol in a random request.

### Where, and why there

In the transport factory (`fpm_pool_fiber_xport.c`), not in `validate()` —
persistent is a connection attribute given in application code, not a
configuration directive, so there is nothing to check at pool-validation
time. The refusal applies only to the fiber executor
(`fpm_pool_fiber_can_wait()`); `fastcgi` and `http/classic` are untouched —
measured, `classic` with persistent still works as before.

### The message: two recipients

The developer gets an `E_WARNING` with an explanation. **But PDO catches the
connection error and throws its own `PDOException: SQLSTATE[HY000] [2002]
Unknown error while connecting`, so our warning does NOT reach the code's
author** — measured. That's why the operator gets a separate `ZLOG_NOTICE` in
the worker log, once per process.

**`persistent_id` is NOT logged**: PDO builds this key from the DSN together
with the username and password, so it would end up in the error log.

### Regressions checked

    pdo non-persistent N=4    1,018 s
    mysqli non-persistent N=4 1,018 s
    phpredis N=4 (BLPOP)      1,116 s
    classic with persistent   2,019 s (unchanged)

## TLS configurations that cannot be non-blocking: REFUSED (task 005)

With patch 0007 (`HAVE_FPMNG_FIBER_TLS`) the `ssl`/`tls` transports suspend
the fiber like plain `tcp`. Two configurations cannot be made safe and are
refused with an `E_WARNING` plus one `ZLOG_NOTICE` per process — the same
shape as the persistent-connection refusal above, and for the same reason:
silently blocking would stop every request in the process, which is worse
than a loud error.

- **`ssl.allow_blocking => true` in the stream context.** That option exists
  exactly to keep the stream blocking, which under this executor means
  "stop the world on every I/O". Drop the option; without it the stream is
  fiber-aware.
- **Server-side `ssl://`/`tls://` from a request fiber**
  (`stream_socket_server("tls://...")`). An accepted client socket is
  allocated with the LISTENER's ops table (`php_openssl_tcp_sockop_accept`),
  so wrapping the listener would hand accepted sockets a wrapper whose
  `close()` frees a structure it does not own. Refused instead of corrupted.
  This does not affect serving HTTPS: the fpm-ng HTTP gateway terminates TLS
  in libevent (`fpm_tls_http.c`) and never touches these transports.

Not refused, still blocking (as before): a build with a **shared**
`openssl.so` — configure prints a warning that the fiber TLS interception is
off, and TLS simply stays upstream-blocking.

## REJECTED: fiber executor only in ZTS, per-request TSRM context

Idea: since in ZTS all globals (`EG`, `SG`, `PG` and the globals of EVERY
module, including `ps_globals`) are offsets in a thread's resource block, if
every request in flight had its own block, sessions per request, ini
entries, framework class statics (`Container::$instance`, `Facade::$app`) and
the whole "Cannot redeclare" / `require_once` wall would disappear in one
move — because every request would be a fresh interpreter.

**Checked experimentally and rejected.** Full report with raw results:
`docs/spike-tsrm-context.md`. Summary:

- Switching via manual `TSRMLS_CACHE` DOES WORK for code compiled statically
  (measured on `EG(precision)`), but there is no setter for the
  `tsrm_get_ls_cache()` path — so switching is one-directional and
  incomplete.
- A DYNAMICALLY loaded extension has a **physically separate copy** of
  `_tsrm_ls_cache`, frozen at process start. `readelf -sW` shows this symbol
  as `TLS LOCAL` separately in `sapi/cli/php` and in
  `ext/session/.libs/session.so`. No swap in the main binary can possibly be
  seen by it — with no compile error and no warning.
- A block created by `ts_resource_ex(0, &fake_tid)` gets created, but it is
  **not a working interpreter**: executing even a trivial PHP file in it ends
  in a segfault. The API that did this correctly
  (`tsrm_set_interpreter_context()`) was removed in PHP 8 (`bd73607b9e4`).
- Regardless of the above, a ZTS build of `sapi/fpmng` does not even build
  today: `fpm_pool_coop.c` uses bare `sapi_globals`/`output_globals`, which in
  ZTS are macros, not symbols (8 occurrences) — consistent with
  `fpm_coop_validate()` already rejecting ZTS at runtime.

The memory cost of an empty block was measured at ~285-292 kB; the cost with
a loaded framework was NOT measured, because it never got that far.
