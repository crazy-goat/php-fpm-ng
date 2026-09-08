# Symfony, Laravel, and Slim 4 on `pool.executor = fiber`

Measurement from 2026-09-06 on the test box (Ubuntu 26.04, epoll, MySQL 8.4, Redis,
phpredis 6.3.0RC1), binary from commit `a5800e4`. **Symfony 8.1.6** (skeleton +
orm-pack + security-bundle, Doctrine ORM 3.6, sessions and cache on Redis),
**Laravel 13.30.1** (`SESSION_DRIVER=redis`, `CACHE_STORE=redis`,
`REDIS_CLIENT=phpredis`, Eloquent on MySQL). Each in three pools: `classic`,
`fiber` without the flag, `fiber` with `env[FPMNG_SHARED_INCLUDES] = 1`. `pm = static`,
`pm.max_children = 1`.

## Verdict

**Both verdicts below are initial-measurement snapshots, superseded by later
fixes in this document** — Symfony by "UPDATE 2026-09-06: after fixes 1, 2, 3
and ini value isolation" below, Laravel by "Laravel: fix 5 landed — class
statics per request (`fiber.isolate_statics`)". Read to the end before citing
either line.

**Symfony: YES, with conditions** — `FPMNG_SHARED_INCLUDES=1`, own
`public/index.php` without `symfony/runtime`, no PHP sessions and no stateful
firewall under concurrency (i.e.: a stateless API, or one request in flight at
a time).

**Laravel: NO.** Works sequentially after three changes to `index.php`, but with
more than one request in flight it mixes up user sessions and database
connections. Fiber makes no sense without concurrency, so this is a "no".

## Where it works, it works very well

Symfony, an endpoint reading MySQL (DBAL + ORM) and Redis (store + cache pool):

    N=8 /mix?sleep=0.3    fiber 0,351 s    classic 2,950 s    8/8 correct data
    N=4 SELECT SLEEP(1)   fiber 1,028 s    classic 4,221 s
    N=8 SELECT SLEEP(1)   fiber 1,046 s    classic 8,436 s
    600 requests          fiber 172 req/s  classic 15,5 req/s
                          RSS 40,2 -> 42,4 MB, same worker, 0 errors

"8/8 correct data" means: each of the eight parallel requests got ITS OWN id,
item, tag, value from Redis and from the cache. That is the claim we were
testing, and it holds up.

## What exactly has to change in the application

**Symfony: one file.** `public/index.php` rewritten in the pre-`symfony/runtime`
style (seven lines: `require vendor/autoload.php`, `bootEnv()`,
`new Kernel`, `handle()`, `send()`, `terminate()`). It cannot be done any
cheaper than that — see below.

**Laravel: one file, three changes** in `public/index.php`:
1. `require_once` -> `require` for `bootstrap/app.php` (fixes
   `Call to a member function handleRequest() on true`); `bootstrap/app.php`
   declares nothing, so `require` is more correct there anyway;
2. `define('LARAVEL_START', ...)` -> `defined(...) || define(...)`, because
   constants are process-wide and keep the value from the first request;
3. `$_SERVER; $_ENV; $_REQUEST;` — **workaround for OUR bug**, see "Autoglobals".

Additional requirement on the application: **no class or function declarations
in files pulled in with `require` per request**. Laravel does `require
routes/web.php` on every boot, so a class declared in that file gives
"Cannot redeclare".

## Baseline: without `FPMNG_SHARED_INCLUDES`

Both frameworks, req1 OK, req2 fatal:

    Symfony: Cannot redeclare class ComposerAutoloaderInite8d3a95... (autoload_real.php:5)
             stack: vendor/autoload.php(20) -> vendor/autoload_runtime.php(5) -> public/index.php(5)
    Laravel: Cannot redeclare class ComposerAutoloaderInitcda2add... (autoload_real.php:5)
             + Warning: Constant LARAVEL_START already defined

## Why caching the `require_once` return value will NOT help

It was on the TODO list as a fix for the `$app = require_once bootstrap/app.php`
pattern. For Laravel it does in fact fix that one error. **For Symfony it does
nothing**: `public/index.php` does `require_once vendor/autoload_runtime.php`,
and the entire runtime (`$runtime->getRunner($app)->run()`) is a **side effect
of that include**. When the include becomes a no-op, we get HTTP 200 with an
empty body and zero log entries — the application does not execute. The
problem is not in the return value, it is in the fact that the code has to
run.

Also measured: workaround A (`require` instead of `require_once` in
index.php): does not help, because `autoload_runtime.php:3` has
`if (true === (require_once __DIR__.'/autoload.php') || ...) return;`.

Conclusion: caching the `require_once` return value fixes one pattern in one
framework, not the include model.

## Autoglobals — OUR bug, cheap and critical

`$_SERVER`, `$_ENV` and `$_REQUEST` are created by `auto_globals_jit` **at
COMPILE time** of the file that uses them. With shared `included_files`,
vendor is not recompiled, and Laravel's `index.php` itself never touches
them — so:

    Warning: Undefined global variable $_SERVER in vendor/vlucas/phpdotenv/.../ServerConstAdapter.php:42
    Warning: Undefined global variable $_ENV   in .../EnvConstAdapter.php:42
    Fatal: Uncaught TypeError: Request::createRequestFromFactory(): Argument #6 ($server)
           must be of type array, null given (symfony/http-foundation/Request.php:2094)

**`php_admin_flag[auto_globals_jit] = off` does NOT help** — measured. The
executor does not create autoglobals per request on any path other than
compile-time JIT.

Symfony survived only by accident: the hand-written `index.php` refers to
`$_SERVER['APP_ENV']`, so JIT creates them.

**This breaks in every application whose entry script does not touch
autoglobals itself.** The fix is cheap: call the autoglobal callbacks in
`fpm_coop_req_enter`.

## Concurrency: where it breaks

### Symfony — PHP sessions

    20/20 rounds: HTTP 500, "Failed to start the session."
                  "PHP Request Startup: Cannot call session save handler in a recursive manner"

Sequentially (user A, then B) it works: different sid, count increases. It
only breaks under concurrency, because `ext/session` keeps state in the
PROCESS (`PS(session_status)`, `in_save_handler`, `$_SESSION`): fiber A hangs
in Redis I/O inside the `read()` handler, fiber B calls `session_start()`.

The stateful firewall (`http_basic` + session) fails for the same reason: 2/6
parallel requests OK, 4/6 fail.

The DI container, `Request` and `RequestStack` (depth 1) are clean per
request — `/who` without auth returns `user: null`. Only a class static leaks
(`leaked_static_user: "alice"`), i.e. process-wide state that we do not
separate.

### Laravel — framework statics, and this is a DATA leak

    N=8 /mix?sleep=0.3:  1 x 200, 4 x 500, 3 requests hung until the 60 s timeout
    N=4 SELECT SLEEP(1): 4/4 timeout
    gateway: "upstream: Connection reset by peer" / "no answer from upstream"

    laravel.log:
      QueryException 2014 Cannot execute queries while other unbuffered queries are active
      RedisException: read error on connection to 127.0.0.1:6379
      SQLSTATE[08S01] [1159] Got timeout reading communication packets
      PDOException: Error at offset 0 of 3 bytes   (unserialize of SOMEONE ELSE'S response)

    sessions: 19/20 rounds wrong, including  user=B  sess_user=A   <- session A's data in request B
    auth:  after Auth::login alice and bob, 5/6 parallel /me returned user: null

Cause, consistent with all the symptoms: `Container::$instance` and
`Facade::$app` are process statics. Request B, bootstrapping a new
`Application`, replaces them; when fiber A returns from I/O, `app()`, `DB::`,
`Redis::` and `session()` resolve in container B. Two requests share one PDO
connection and one Redis connection, and one session Store.

This is the same trap that Octane guards against — with the difference that
Octane NEVER has two requests in flight in the same worker.

Additionally: `HandleExceptions` in req1 sets `display_errors = Off` and
`error_reporting = -1`, which stays for the process (ini is not restored per
request), so fatals on the fiber give a 500 with no body.

## List of fixes on our side

In order: cheap and blocking everything first.

1. **Autoglobals per request** regardless of `auto_globals_jit` — call the
   callbacks in `fpm_coop_req_enter`. Without this, every application whose
   entry script does not touch them itself breaks. The only cheap item on
   this list.
2. **`ext/session` state per request** — swap `ps_globals` on enter/leave, the
   same as we do with SG and OG. Without this, Symfony sessions and security
   don't work concurrently.
3. **Restoring ALL ini entries per request** (the equivalent of
   `zend_ini_deactivate`), not just `max_execution_time`. `define()` stays
   process-wide and that will not change.
4. **The include model** — caching the `require_once` return value does not
   save Symfony. Either per-request `included_files` that skips redeclaring
   classes and functions that already exist on recompilation, or a documented
   requirement for your own `index.php`.
5. **Laravel: class statics per fiber** (`Container::$instance`,
   `Facade::$app`). This is the direction the True Async fork took and then
   **reverted**. Without this, Laravel on a fiber is at best one request in
   flight. **Done** — see "Laravel: fix 5 landed" below.
6. **HTTP gateway: fallback to the front controller** — `/mix` currently gives
   "File not found", you have to use `/index.php/mix`. A separate gap, also
   documented in NOTES ("index.php hardcoded / no try_files").

## What this measurement did NOT cover

- Laravel `classic` as a baseline for the parallel and session tests (single
  requests OK).
- Laravel: stability over 300 requests (interrupted by hanging requests). RSS
  after ~55 requests was 73 MB versus 40 MB for Symfony.
- Symfony with `APP_ENV=prod`, `pm.max_children > 1`, `fiber.revalidate_freq`.

## Methodological note

The HTTP gateway has no fallback to the front controller, so all requests
went through `/index.php/mix?...` (PATH_INFO). The old build on the test box
was `--disable-all` and had to be rebuilt with mbstring, session, ctype,
tokenizer, dom, iconv, fileinfo, phar and curl — without them composer and
both frameworks won't install.

---

# UPDATE 2026-09-06: after fixes 1, 2, 3 and ini value isolation

The verdicts above are now OUTDATED for Symfony. Re-measured on the same test
box, `pool.executor = fiber`, `env[FPMNG_SHARED_INCLUDES] = 1`,
`pm.max_children = 1`, sessions through a user handler, N=8 parallel.

## What was added

1. **Autoglobals per request** — `$_SERVER`/`$_ENV`/`$_REQUEST` forced in
   `fpm_coop_req_run`, regardless of `auto_globals_jit`. The
   `$_SERVER; $_ENV; $_REQUEST;` workaround in Laravel's `index.php` is no
   longer needed.
2. **Restoring ALL ini entries** at the end of the request
   (`zend_ini_deactivate()` instead of just one, `max_execution_time`).
3. **`ext/session` state isolation per request** — swap the module's globals
   on enter/leave plus RINIT/RSHUTDOWN per request (`fpm_pool_coop_session.c`).
   The address of `ps_globals` is taken from `mh_arg2` of the `session.save_path`
   ini entry, with no linker dependency: a build with `--enable-session=shared`
   and one with session disabled both link and work.
4. **Isolating the VALUES of ini entries between requests in flight**
   (`fpm_pool_coop_ini.c`) — without this `ini_get()` saw someone else's
   `ini_set()`.

## Symfony: YES, sessions included

    N=8, /session?user=X&sleep=0.3
    8/8 http=200, disjoint sid, ok=true, handler=user for all eight
    second round with the same cookies: 8/8 count=2, same sid

Previously: **20/20 rounds HTTP 500**, `Failed to start the session`,
`Cannot call session save handler in a recursive manner`. Logs clean — 421
lines of "WARNING" in the pool log are purely Symfony dev-mode debug passed
through `catch_workers_output`, zero real errors.

The test script uses a handler with REAL blocking I/O inside `read()`
(`blPop` on Redis), i.e. exactly what triggered the original failure.

## Laravel: still NO — and the failure mode changed from LOUD to SILENT

    user    sess_user  ok      count  app_oid  container_request_oid
    alice   alice      True    1      1261     1329
    bob     bob        True    1      225      1842
    carol   carol      True    1      1540     1604
    dave    bob        False   3      225      1842
    eve     bob        False   2      225      1842
    frank   frank      True    1      1011     1051
    grace   bob        False   4      225      1842
    heidi   heidi      True    1      225      1842

5/8 correct, 3/8 got SOMEONE ELSE'S session. Five requests share one
`Application` object (`app_oid = 225`) and one `Request` object (1842) — that
is `Container::$instance` and `Facade::$app`, i.e. item 5 on the fix list,
which we did not touch.

Previously: 1 x 200, 4 x 500, three requests hanging until the 60 s timeout,
plus `Cannot execute queries while other unbuffered queries are active` and
`unserialize` of someone else's Redis response. Now: **8 x 200, zero
exceptions, zero fatals, empty `laravel.log`**.

This is NOT an improvement from a deployment point of view. Before, Laravel
on a fiber crashed and it was visible. Now it returns 200 with someone else's
data and reports nothing. The verdict "Laravel: NO" is STRONGER after this
change, not weaker.

The difference between the frameworks is not in their quality: Symfony keeps
state in a container passed explicitly, Laravel in a class static — and class
statics are process-wide and we do not separate them.

## Laravel: fix 5 landed — class statics per request (`fiber.isolate_statics`)

**Previously ("Laravel: still NO" above):** 5/8 correct, three requests got
someone else's session, sharing one `Application` and one `Request` object.

**Now, with a configured, per-request-isolated list of class static
properties** (task 008, `sapi/fpmng/fpm/fpm_pool_coop_statics.c`):

    fiber.isolate_statics = Illuminate\Container\Container::instance,Illuminate\Support\Facades\Facade::app,Illuminate\Support\Facades\Facade::resolvedInstance,Illuminate\Database\Eloquent\Model::resolver

(Superseded — this was the four-entry list as of task 008. The current,
versioned snippet for Laravel 13.30.1 is
["Laravel: the versioned configuration snippet and how it is verified"](#laravel-the-versioned-configuration-snippet-and-how-it-is-verified)
below.)
Measured on the same `/session?user=X&sleep=0.3`, N=8, `pm.max_children = 1`:
8/8 correct, distinct `sess_user`, distinct `app_oid` and
`container_request_oid` per request. A negative control (same run, empty
`fiber.isolate_statics`) reproduced the original failure (4-5/8 wrong,
several requests sharing one `app_oid`), confirming the isolation is what
fixes it, not something else about the measurement.

Also measured: an authenticated flow not covered by the spike --
`Auth::login()` for two users (`alice`, `bob`) followed by 4 concurrent
`/me` requests per user. Without `Facade::app`/`Facade::resolvedInstance` in
the list (isolating `Container::instance` alone), this came back mostly
wrong (`user: null` for most requests) — the AuthManager instance resolved
through the Facade cache belonged to whichever *other* request bootstrapped
last. With the four-item list below: 8/8 correct.

### Laravel 13.30.1 repository-owned runner result

The Task 025 runner lives in `tests/frameworks/laravel/` and uses Laravel
13.30.1, Predis 3.6.0 locked in Composer, phpredis 6.3.0RC1, MySQL 8.4.11 and
Redis 8.0.5. On 2026-09-06 it used PHP-FPM-NG 8.5.11-dev, source worktree
HEAD `67d1476d4d8015c7a7ddf3221eb062c423869818` with the uncommitted fpm-ng
source overlay, binary SHA-256
`90a592e2f027fb50bbe94292dcc451b64d8a16b0490c6ed53abe085af95eeca6`,
`pool.executor = fiber`, `FPMNG_SHARED_INCLUDES=1`, `pm.max_children = 1`,
FastCGI port 22725, HTTP port 22726, database `laravel025` and Redis DB 3.
The binary was verified with `strings` before the run; it contained both
`fiber.isolate_statics` and `FPMNG_SHARED_INCLUDES`.

With this four-entry configuration, all ten implemented concurrent scenarios
passed: session rounds, authenticated `/me`, DB + Cache + Redis + Eloquent,
facade/object identity, Eloquent resolver identity, middleware/`terminate()`,
CSRF/session isolation, validation flash data, sync queue context and sync
broadcast context. The suite keeps rate limiting, mail attribution, Blade
view composers and request-dependent observers/global scopes as `NOT MEASURED`.

The fourth entry was found by the Eloquent test. With only the three entries
above, both the mixed DB/Cache/Redis/Eloquent test and the dedicated Eloquent
test returned HTTP 500 with `Cannot execute queries while other unbuffered
queries are active`; adding `Illuminate\Database\Eloquent\Model::resolver`
made both pass. The empty-list negative control retained errors: `/session`
returned HTTP 200 responses sharing one `app_oid`, while the authenticated and
mixed scenarios failed (including transport/connection failures). This is the
important failure mode: an incomplete list can return a correct-looking HTTP
200 response with another request's data.

This does not close the Laravel task. The list is still empirical, the
remaining matrix is not measured, and the four-entry configuration must be
re-verified for every Laravel minor-version upgrade.

**The mechanism does not know about Laravel.** The directive is a
comma-separated list of `Class\Name::property` read from pool configuration;
`fpm_pool_coop_statics.c` has no Laravel-specific code, no hardcoded class
name, and resolves any class/property pair the same way. The Laravel value
above is documentation, not code.

**Memory ownership is not merely assumed to be safe — it is argued, and the
one hazard that turned up experimentally has since been fixed.** The
transfer of a request's own value is a relocation (`ZVAL_COPY_VALUE`, no
incref/decref), exactly like the existing `SG`/`OG`/`ini_entry->value` swaps;
seeing this through required also fixing what the *live slot* holds while no
request owns it. Leaving it `IS_UNDEF` (the spike's approach) is a genuine
bug for any typed property with no default: a second, unrelated request
touching the same property for the first time while the first is suspended
elsewhere hit "Cannot access uninitialized non-nullable property ... by
reference" — reproduced with `tests/statics_reference.php` and fixed by
refilling the slot with the class's own compiled-in default
(`ZVAL_COPY_OR_DUP` from `default_static_members_table`) instead. References
(`$x = &Class::$static;`) across a real suspension point (a blocking MySQL
query, not `usleep()` — see the caveat below) are covered by that same test
and pass. See the commit and `fpm_pool_coop_statics.c`'s own comments for
the complete argument, hazard by hazard.

**Caveat worth stating plainly: `sleep()`/`usleep()` do not suspend the fiber
in this build** (`docs/fiber_async_io.md`) — they block the whole process.
The `?sleep=0.3` parameter above does not itself cause interleaving; the
interleaving that makes the `/session` and `/me` measurements meaningful
comes from the real Redis/MySQL I/O Laravel's own bootstrap and session
handling already do. This was checked with a negative control before relying
on it (see above), not assumed.

**Cost when unconfigured:** `fpm_coop_statics_req_enter()`/`_req_leave()`
both start with `if (fpm_coop_statics_count == 0) { return; }` — no
allocation, no lookup, when the directive is empty (the default). Latency
measurement attempted (50 sequential requests via curl, empty vs. 4 isolated
items) was within run-to-run noise (curl process spawn dominates at this
scale) and is not reported as a number for that reason; the zero-cost claim
rests on the code path, not on that measurement.

## Known limitation of fix 4

The VALUES of ini entries are isolated (what `ini_get`/`ini_set` see), but not
the process-wide globals that some entries set through `on_modify` — e.g.
`precision` lands in `core_globals` and is actually used by `var_dump` and
`serialize`, so it is still shared between requests in flight there. A full
fix would require swapping the globals of every such module, the way
`fpm_pool_coop_session.c` does for sessions.

## Symfony: stateful firewall, measured after the fixes

The earlier note above records a stateful firewall (`http_basic` plus a session)
failing under concurrency: 2 of 6 parallel requests correct, 4 of 6 wrong. That
was a consequence of the shared `ext/session` state, and it was left unmeasured
when that was fixed. Measured now, on a build carrying all of the per-request
isolation (autoglobals, all ini entries, `ext/session`, ini values, class
statics), 8 concurrent requests, 4 as `alice` and 4 as `bob`, `pm.max_children = 1`,
`/me?sleep=0.3` (the route is behind `access_control: { path: ^/me, roles: ROLE_USER }`):

    round 1, Authorization: Basic sent    8/8 correct, all HTTP 200
    round 2, NO Authorization header      8/8 correct, all HTTP 200, auth_header=no

Round 2 is the one that matters: with no credentials on the request, the token
came back from the session through `ContextListener`, which is exactly the
stateful path that used to mix users up. Each request saw its own identity.

Note that Symfony needs no `fiber.isolate_statics` entries for this — its
security state lives in the container and in the session, both of which are
already per request. The class-static isolation is a Laravel requirement, not a
general one.

---

# UPDATE 2026-09-06: automated Symfony probe — remaining task-024 scenarios measured

`tests/frameworks/symfony/run.sh` now covers the whole task-024 matrix
end-to-end (it provisions its own Symfony copy, private MySQL database and
Redis namespace per run, and asserts on response data, never on HTTP status).
Measured on macOS (arm64, kqueue) against a locally built php-fpm-ng
`PHP 8.6.0-dev (fpm-fcgi) (built: Sep  6 2026 15:00:30) (NTS)`, SHA-256
`862c180037fee77b38c64ee50c359a8d257b336c4fbea13266a9c9a0d6e3a881`, all four
feature markers present (`FPMNG_SHARED_INCLUDES`, `http.front_controller`,
`fiber.revalidate_freq`, `fiber.isolate_statics`), pool.executor = fiber,
`FPMNG_SHARED_INCLUDES=1`, opcache off, Symfony 8.1.6, predis.

Result of the full run: **PASS=19 ERROR=2 NOT MEASURED=0** (21 scenarios; the
runner stops with NOT MEASURED only when the environment itself is unusable).

| Area | Scenario | Result |
|---|---|---|
| concurrency (baseline) | mix / session / stateful-auth / object-identity, `APP_ENV=dev`, 1 worker | PASS |
| `APP_ENV=prod` | same four scenarios on a dedicated prod pool (`APP_DEBUG=0`) | PASS |
| `pm.max_children = 2` | mix / object-identity / session | PASS |
| `pm.max_children = 2` | stateful-auth | **ERROR, see below** |
| Twig | `renderView` + custom Twig extension that performs real blocking Redis I/O inside the template, `app.user` rendered per user | PASS |
| Forms + validation | form submit with `NotBlank`, valid/invalid split asserted on the validator's errors | PASS |
| Messenger (sync) | bus dispatch, handler result asserted through `HandledStamp` | PASS |
| RSS stability | 200 sequential requests in one worker, growth limit 6144 KiB | PASS (measured growth 1.7 MB) |
| `fiber.revalidate_freq` | controlled deploy: `DeployMarker::VALUE` changed on disk with `fiber.revalidate_freq = 1`; updated code served without a manual restart | PASS |

## Historical failure: `pm.max_children = 2` with stateful traffic

Before the accept fix below, the `pm2-*` suite ran each scenario on a fresh
2-worker pool. `pm2-mix`, `pm2-session`, `pm2-stateful-auth` and
`pm2-object-identity` each PASSED in at least one run, but **at least one of
them failed in every full run** with the same signature: in the cookie-replay
round (no `Authorization` header), some requests never reached the scenario's
Redis gate — the runner observed `llen` 2-7 of the expected 8 after a 90 s wait,
the stuck requests completed exactly at the gate's 90 s BLPOP timeout, and a
stalled scenario's blocked fibers degraded everything that followed on the same
pool. The runner retained the failing scenario as ERROR.

This was a pre-fix result, not the current support verdict. The root cause and
fix are recorded below. After the fix, the same four scenarios pass with
`pm.max_children = 2`.

## Root cause of the pm2 stall: blocking accept() (fixed)

Root-caused on 2026-09-06 (worktree `symfony-pm2-stall`, binary rebuilt from
this source). The stall has nothing to do with sessions — the same freeze was
reproduced in round 1 of the auth scenario (before any session round-trip
mattered) and it does not need session state at all. It is an accept race that
only exists with more than one child.

With `pm.max_children > 1` the children share one listening socket. For one
pending connection, kqueue wakes the accept callback in **all** of them, and
`fcgi_accept_request()` does a **blocking** `accept()` — the child that loses
the race blocks in the kernel and freezes its whole scheduler: every fiber in
flight stops being serviced, including its timers, until the NEXT connection
arrives. Evidence from the live repro (2 children, 8 requests, a Redis gate in
the handler):

- `sample` of the stalled child: ~1740 samples in `accept()` under
  `fpm_fiber_accept_cb` (`fpm_pool_coop.c`), while the sibling served
  everything;
- the frozen child's fibers stayed registered in libevent (per-second watchdog
  in the scheduler) and sat in `kevent` idle, while the Redis replies (gate
  releases) were already in the sockets' receive queues (`netstat`, Recv-Q up
  to 163 bytes, never read);
- downstream effects visible in the scenario data: requests whose round-1
  fiber never woke never wrote their security token to the session, so the
  cookie-replay round answered HTTP 401 (empty session) and never reached the
  gate — that is why the failing rounds showed `llen` 2-7 of 8.

Gate-free scenarios (`pm2-mix`) self-heal: the next connection arrives quickly
and unblocks the frozen child, which is why only gated scenarios failed
reproducibly.

**Fix:** `fpm_coop_accept()` (fiber pool acceptor) makes the listening socket
non-blocking for the duration of the accept — the same treatment
`fpm_coop_accept_kept()` already applies for the keep-alive path. With nothing
pending it gets -1/EAGAIN and returns; the accept callback already handles
that. With the fix, 30/30 loop rounds of the targeted pm2 repro pass (a stall
previously appeared every 1-6 rounds), and the full suite result is
**PASS=21 ERROR=0 NOT MEASURED=0** over the same 21 scenarios, measured
2026-09-06 on macOS (arm64, kqueue) with the binary rebuilt from this source:
`PHP 8.6.0-dev (fpm-fcgi) (built: Sep  6 2026 15:00:30) (NTS)`, SHA-256
`1d002fbff74530876245e2d055662de7c13acfc5ecd77569ee1c9c4c5ab1729d`, source
commit `d617976` — all four `pm2-*` scenarios pass their data assertions in
the same run, including `pm2-stateful-auth` round 2 (cookie replay).
`pool.executor = async` has the same latent pattern in
`fpm_pool_async.c` and is currently a rejected configuration; it needs the
same treatment before it can be enabled.

---

# Slim 4 on `pool.executor = fiber`

Measurement from 2026-09-06 on the shared test box, using the current-main fiber
build rather than the older `a5800e4` binary. The probe uses Slim **4.15.3**,
**slim/psr7 1.8.0**, and **predis/predis 3.6.0**. The pool has
`pool.executor = fiber`, `pm.max_children = 1`, and
`env[FPMNG_SHARED_INCLUDES] = 1`; it uses MySQL database `slim4` and Redis
database `2`.

## Verdict

**Slim 4: YES for the measured surface, with conditions** — the normal
`public/index.php` survived repeated requests with shared includes, and the
core plus listed PSR-7/middleware scenarios passed concurrently in one fiber
worker. The probe configured no `fiber.isolate_statics` entries and did not need
any: all tested request, application, route-collector, and default-container
observations stayed isolated. No Slim-specific C support was added.

The two remaining scenario rows have since been measured with the same binary
and settings. With `SLIM_CONTAINER=php-di` (`php-di/php-di` `7.1.1`), 8/8
concurrent requests used distinct PHP-DI containers and distinct container
services. With `SLIM_ROUTE_CACHE=1` (Slim's route collector cache file), the
cached routes returned correct data under concurrency and the cache file's
fingerprint (device, inode, size, mtime, content hash) was unchanged before and
after the run. The final combined run reported **11 PASS, 0 ERROR, 0 NOT
MEASURED**. Neither mode needed `fiber.isolate_statics` entries.

## Binary and command

The exact executable was:

    /home/piotr/rd/tasks/026-slim4/build/sapi/fpmng/php-fpm-ng

It reported:

    PHP 8.5.11-dev (fpm-fcgi) (built: Sep  6 2026 11:01:33) (NTS)

The binary SHA-256 was
`90a592e2f027fb50bbe94292dcc451b64d8a16b0490c6ed53abe085af95eeca6`.
`strings` confirmed the `FPMNG_SHARED_INCLUDES` and fiber-child literals. The
source tree's base commit was
`67d1476d4d8015c7a7ddf3221eb062c423869818`; the build also included the
uncommitted current-main worktree changes present in that build directory.

The final run was:

    cd /home/piotr/rd/tasks/026-slim4/slim4-tests
    RUN_DIR=/home/piotr/rd/tasks/026-slim4/slim4-tests/.run \
    PHP=/home/piotr/rd/tasks/026-slim4/build/sapi/cli/php \
    FPMNG=/home/piotr/rd/tasks/026-slim4/build/sapi/fpmng/php-fpm-ng \
    HTTP_PORT=22626 FCGI_PORT=22625 ./bin/run.sh

The runner uses bare paths such as `/health`. The current gateway's default
`http.front_controller = /index.php` invokes the normal entry script while
leaving `/health`, `/mix`, and the other route paths intact for Slim. An initial
harness attempt using `/index.php/<route>` reached Slim but produced Slim's own
404 because `/index.php` remained part of the route path; that setup defect was
fixed before counting results and was not treated as framework evidence.

## Results

All measured rows ran with N=8 concurrent requests unless stated otherwise.
Assertions were on response data, not only HTTP status.

| Scenario | Assertion | Result |
|---|---|---|
| repeated stock entry script | 8 health requests through shared includes | **PASS** |
| `/mix?sleep=1` | 8/8 own MySQL item/tag and Redis value | **PASS** |
| `/session?sleep=1` | two rounds: 8/8 own sid and user; round 2 count=2 | **PASS** |
| authenticated route | 8 concurrent login flows and 8/8 own `/me` identity | **PASS** |
| object identity | distinct app, request, and route collector; default container mode consistently `none` | **PASS** |
| PSR-7 request body | 8/8 own JSON marker/hash and complete 64 KiB body | **PASS** |
| PSR-7 response body | 8/8 exact `start-N`/`end-N` stream output | **PASS** |
| middleware stack | 8/8 own middleware id and one hit | **PASS** |
| container: PHP-DI (`SLIM_CONTAINER=php-di`) | 8/8 distinct PHP-DI containers and distinct container services per request | **PASS** |
| route cache (`SLIM_ROUTE_CACHE=1`) | cached routes returned correct data under concurrency; cache fingerprint unchanged during the run | **PASS** |
| error middleware | 8/8 HTTP 500 responses contained only their own error tag | **PASS** |

Final runner summary: **PASS=11, ERROR=0, NOT MEASURED=0** (the PHP-DI and
route-cache scenarios report NOT MEASURED when their mode is not selected; the
final combined run selected both). An `ERROR` remains a failure in `bin/run.php`
and makes the command exit non-zero; no failing scenario was deleted or
weakened.

## What remains unmeasured

- a classic/non-fiber baseline and a separate negative-control comparison;
- other Slim 4 minors and other PSR-7 implementations;
- `pm.max_children > 1` and `fiber.revalidate_freq`;
- longer-duration stability, production-style configuration, and Slim features
  outside this probe.

The empty/default `fiber.isolate_statics` configuration is the measured Slim
configuration. No separate non-empty static-isolation comparison was run because
this probe has no request-scoped Slim static candidate to configure; that is not
evidence for PHP-DI or other integrations.

# UPDATE 2026-09-08 (task 025): Laravel — the statics list made systematic

The four-entry list above was empirical: one entry per scenario that failed,
found by hand. Task 025 replaced that with a measurement and, in the process,
found two more entries the scenarios had never exercised.

## Laravel: the versioned configuration snippet and how it is verified

**Laravel 13.30.1** (phpredis 6.3.0RC1 as the Redis client, MySQL 8.4 via the
test box, `SESSION_DRIVER=redis`, `CACHE_STORE=redis`, `pm.max_children = 1`,
`pool.executor = fiber`, `FPMNG_SHARED_INCLUDES = 1`):

```ini
fiber.isolate_statics = Illuminate\Container\Container::instance,\
Illuminate\Support\Facades\Facade::app,\
Illuminate\Support\Facades\Facade::resolvedInstance,\
Illuminate\Database\Eloquent\Model::resolver,\
Illuminate\Database\Eloquent\Model::dispatcher,\
Illuminate\Database\Eloquent\Model::globalScopes
```

(The line is comma-separated with no whitespace in the real directive; broken
here only for readability. Copy it from `tests/frameworks/laravel/bin/run.sh`,
which is the file the runner itself uses.)

Where each entry came from:

| Entry | Found by | Failure without it |
|---|---|---|
| `Container::instance` | task 008 `/session` | 5/8 requests served another user's session data |
| `Facade::app`, `Facade::resolvedInstance` | task 008 authenticated `/me` | most requests got `user: null` (AuthManager resolved through another request's container) |
| `Model::resolver` | task 025 Eloquent probe | HTTP 500 `Cannot execute queries while other unbuffered queries are active` (shared PDO connection) |
| `Model::dispatcher` | task 025 observers scenario | model events dispatch through whichever request's event dispatcher was cached last — a listener registered by request A does not fire for A's own models |
| `Model::globalScopes` | task 025 global-scopes scenario | **silent**: request A's `addGlobalScope` filters request B's query — HTTP 200 with `item: null` |

**Warning — what an incomplete list does.** It does not crash. Every failure
mode in the table above except the Eloquent-resolver one presented as a
**correct-looking HTTP 200 with another request's data (or with missing
data) and an empty `laravel.log`**. Anyone deploying this configuration must
know that before, not after, an incident. The list is verified for the code
paths the repository probe exercises (below); a code path nobody has
exercised may still need an entry, and Laravel must be re-verified per minor
version — a framework upgrade can add or move a static.

## The audit: how the list is verified instead of hand-picked

`tests/frameworks/laravel/bin/run.sh` ends with a statics audit. The
`/statics-audit` route touches every state-keeping subsystem first (DB, cache,
Redis, log, mail, view, events, rate limiter, auth, Eloquent model events and
global scopes — so each one initializes whatever statics it owns in *this*
request), snapshots **every static property of every declared class**, blocks
on a real MySQL suspension, and snapshots again. A static whose value changed
across the suspension was overwritten by another concurrent request: it holds
per-request state and must be on the list. Round 1 is cold (concurrent boot
itself flips boot-once statics), so the verdict comes from round 2.

Measured 2026-09-08, PHP-FPM-NG 8.5.11-dev (built Sep 8 2026 05:30:28),
SHA-256 `71fe2574aa2d0df316fab05c9f51fdc1c7e9f96ff8b151e4ae9c69595c1eb0c9`,
feature markers verified via `strings` before the run, worktree branch
`task/025-laravel-statics`:

- audit against the six-entry pool: **235 static properties checked, round-2
  steady-state changes: zero** (`AUDIT_RESULT=COVERED`). The only round-1
  changes were `Laravel\SerializableClosure\Serializers\Native::transformUseVariables`
  and `::resolveUseVariables` — a process-shared cache of pure closure
  objects holding no request data; they are documented benign exclusions in
  `bin/run.php`, each exclusion needing a written justification.
- an audit against a pool with an **empty** list was tried and removed from
  the default run: without isolation the probe itself destabilizes the pool
  (17 of 64 audit requests returned 502, and the MySQL client logged
  `RSET_HEADER packet additional data length is past 3 bytes` — protocol
  corruption aimed at the *shared* MySQL server). The per-scenario negative
  controls already reproduce empty-list damage on isolated routes;
  `LARAVEL_AUDIT_EXPECT=leak` remains in `bin/run.php` as a manual forensic
  mode for a private MySQL.

`bin/statics-scan.sh` records the search space: **215 static property
declarations** in `vendor/laravel/framework` 13.30.1 (the spike's "237"
counted `static $` inside function bodies too — that is local state, not
class state). The audit is what discriminates request-scoped statics out of
that space; the hand-picking is gone.

## Full suite result (2026-09-08, same binary)

`configured_pass=15 configured_error=0 negative_pass=1 negative_error=14
not_measured=0 audit_status=0` — all fifteen configured scenarios pass,
including the five new since task 027: rate limiter (per-request keys, own
hit counts), mail attribution through the log transport (asserted on
`laravel.log` blocks — each marker in exactly one intact message block, no
interleaving), Blade view composer (per-request registration, own render),
implicit route model binding (own user), and Eloquent observers + global
scopes (own listener fires, own scope applied, 8/8). The audit runs after
them and passes.

The negative suite (same scenarios, `fiber.isolate_statics` empty) fails 14
of 15 as designed — the one pass (CSRF) does not exercise any of the
isolated statics, which the runner documents rather than hiding.

`Model::$booted` stays **off** the list deliberately: boot-once per process
is a side effect of the coop model, and none of the measured scenarios
depends on boot side effects being request-dependent. It is watched by the
audit — if it ever flips in steady state, the run fails.

## Laravel gotcha: `Model::observe()` cannot carry per-request state

Found while building the observers scenario (task 025): `Model::observe()`
ignores the instance you pass for anything except its class name — on each
model event, Laravel container-resolves the **observer class**, so an
observer with a constructor parameter is unresolvable
(`Unresolvable dependency resolving [Parameter #0 [ <required> string
$marker ]]`), and an observer with a default constructor gets a fresh
container-built instance per dispatch. Either way, per-request state cannot
travel through `observe()` at all. This is upstream Laravel behaviour, not a
fiber-executor bug — but anyone writing per-request model-event listeners on
this SAPI should register closures directly (`Model::retrieved(fn ($model)
=> ...)`, or `Model::$dispatcher`'s equivalent), which is what the fixture
does. The dispatch still goes through the isolated `Model::$dispatcher`
static, so isolation applies exactly as it does for any other listener.

## What is still not measured

- `pm.max_children > 1`, `APP_ENV=prod`, `fiber.revalidate_freq` and
  long-run RSS for the Laravel fixture (the Symfony probe covers these
  patterns; the Laravel one does not yet).
- Laravel versions other than 13.30.1. The snippet above is **versioned**:
  re-run the suite and audit against a new minor before extending the claim.
