# Symfony and Laravel on `pool.executor = fiber`

Measurement from 2026-09-06 on the test box (Ubuntu 26.04, epoll, MySQL 8.4, Redis,
phpredis 6.3.0RC1), binary from commit `a5800e4`. **Symfony 8.1.6** (skeleton +
orm-pack + security-bundle, Doctrine ORM 3.6, sessions and cache on Redis),
**Laravel 13.30.1** (`SESSION_DRIVER=redis`, `CACHE_STORE=redis`,
`REDIS_CLIENT=phpredis`, Eloquent on MySQL). Each in three pools: `classic`,
`fiber` without the flag, `fiber` with `env[FPMNG_SHARED_INCLUDES] = 1`. `pm = static`,
`pm.max_children = 1`.

## Verdict

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
