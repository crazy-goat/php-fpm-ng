# Spike: modular build -- core modes vs. experimental modules

Measured 2026-09-12 on the test box against a canonical build
(`--disable-all --enable-fpmng --enable-session --with-openssl`, php-8.5.9).
Investigation only: no production code was changed. The task breakdown in §8 is
a proposal; nothing in it is filed, and §8's T0 is the decision that gates the
rest.

Question asked: the product decision is that `pool.type = fastcgi` and
`pool.type = http-direct` are the CORE, always compiled; everything else
(`fastcgi-ng`, fiber, async, the `http` gateway, `supervisor`, `cron`,
`status`, ACME, metrics) becomes EXPERIMENTAL, not compiled unless a configure
flag is passed, flags defaulting to "no". This spike maps the real coupling,
says where the `#ifdef`s would have to live, works out the configuration and
test consequences, and measures what the gating buys -- in binary size and,
because it is a stated motivation, in compile time.

---

## 0. Verdict in one page

1. **The seam is real and unusually clean.** Outside `fpm_pool_type.c`, no core
   file calls any module symbol except for ACME and metrics (two call sites
   each in `fpm.c`). Every other cross-reference found by grep is a *comment*
   citing a `file:line`. See §1.

2. **The measured saving is ~nothing.** Removing the gateway, supervisor, cron,
   script, watchdog, status, cron_schedule, ACME and metrics from a canonical
   build removes **42,592 bytes of `.text`** (0.77% of the binary's `.text`,
   0.28% of the sum of its loadable sections) and **192 bytes of stripped file
   size** -- 0.0012% -- because the link line uses
   `-Wl,-zmax-page-size=2097152` and the saving disappears into segment
   alignment padding. Our entire SAPI is 165,379 bytes of `.text` in a
   5,522,585-byte `.text`. See §5. **"Smaller production binary" must be dropped
   as a motivation.**

3. **Build time IS a stated motivation -- and build flags are the wrong lever
   for it.** The goal is not to recompile all of PHP on every change, in CI
   above all. But a build flag can only ever remove files from the 50 objects
   that are ours, out of 619 in the build: **no build flag reduces the PHP part
   of the compile at all**, and the PHP part is essentially all of it. The
   largest gate available (the gateway, 5 files) is 5 of 619 objects. Worse, a
   default-off flag *adds* CI compile time, because covering it needs another
   job that compiles PHP from scratch -- `fpmng-phpt-fiber` (232 s of build)
   exists for exactly that reason and for no other. The real levers are
   elsewhere: three jobs compile PHP independently per push, and ccache is
   running at a 91.7% hit rate with 51.9% of those hits in the slow
   preprocessed mode on a cache that uses 50 MB of its 5 GB. See §5b -- that
   analysis is the main new argument this spike produces, and it points at
   T10, not at T2/T3.

4. **`fastcgi-ng` cannot be gated the way the other modules can.** It owns no
   `.c` file. Its behaviour is `patches/0001`–`0006` against php-src
   (`main/fastcgi.c`, `Zend/zend_signal.c`), applied unconditionally by
   `build/prepare.sh`, switched on at runtime by
   `sapi/fpmng/fpm/fpm.c:184`. Gating it removes a registry entry and
   **zero bytes of code**. See §1.1 and §2.4.

5. **The "everything else is experimental" list is not coherent as stated.**
   Three of its members are load-bearing for a core mode:
   `status` is the only monitoring `http-direct` has (§6.1); ACME needs the
   `http` gateway *and* `cron` to work at all, so gating both strands TLS users
   of `http-direct`, which is a core mode with TLS (§6.2); and
   `fpm_http_tls.c` / `fpm_http_tls_reload.c` are shared, not gateway-owned
   (§6.3).

6. **Recommendation: a three-tier split** (§7), and -- separately -- that the
   supported-surface message is carried by **data in the type registry plus
   documentation**, not by a compile flag, because the compile flag buys 192
   bytes and costs build-matrix cells.

7. **The test matrix is where the real cost is.** 27 of the 61 owned `.phpt`
   files (44%) configure `pool.type = http`, including **every** ACME test and
   **every** fiber test. A default build without the gateway loses 44% of the
   owned suite from the default cell, and the existing fiber cell stops
   working unless it also passes the gateway flag. See §4.

---

## 1. The coupling map

Method: `grep` for `#include "fpm_*.h"` in every one of our `.c` files, plus
`nm --defined-only` / `nm -u` over the built objects to find the symbols that
actually cross module boundaries at link time (this is stronger than a header
grep: it catches symbols reached without including the header). Done on the
test box against a canonical build,
`--disable-all --enable-fpmng --enable-session --with-openssl`, php-8.5.9.

### 1.1 Per-module table

`.text` / `.rodata` are from `size -A` on the object in the canonical build;
they are the real code contribution, not the on-disk `.o` size (which is
dominated by `-g` debug info: `fpm_http.o` is 279 KiB on disk and 23,689 bytes
of `.text`).

| Module | Files | Symbols referenced from outside the module | `.text` | `.rodata` | What breaks if the sources go |
|---|---|---|---|---|---|
| `http` gateway | `fpm_http.c`, `fpm_http_access_log.c`, `fpm_http_acl.c`, `fpm_http_auth.c`, `fpm_http_forwarded.c` | `fpm_http_init_pool`, `fpm_http_validate_pool` (both from `fpm_pool_type.c` only); `fpm_http_init_pool_with_capacity` only under `HAVE_FPMNG_FIBER`/`HAVE_FPMNG_ASYNC` | 27,884 | 6,168 | Nothing outside `fpm_pool_type.c`. **But**: ACME HTTP-01 answering lives here (`fpm_http.c:1822`), and 27 tests use this type. |
| `supervisor` | `fpm_pool_supervisor.c` | `fpm_pool_supervisor_{rejects,validate,init_main,child_main,status}` -- `fpm_pool_type.c` only | 2,209 | 1,445 | Nothing outside the registry |
| `cron` | `fpm_pool_cron.c`, `fpm_cron_schedule.c` | `fpm_pool_cron_{rejects,validate,init_main,child_main,status}` -- registry only; `fpm_cron_schedule.*` used by `fpm_pool_cron.c` only | 4,523 | 2,067 | Nothing outside the registry. **But**: ACME's client is documented to run here (`docs/acme-client.md:22,30`). |
| `script` | `fpm_pool_script.c` | used by `fpm_pool_cron.c` and `fpm_pool_supervisor.c` only | 996 | 214 | Follows cron+supervisor; not independently gateable and should not be its own flag |
| `watchdog` | `fpm_pool_watchdog.c` | used by `fpm_pool_cron.c` and `fpm_pool_supervisor.c` only | 278 | 0 | Same as script |
| `status` pool | `fpm_pool_status.c` | `fpm_pool_status_{rejects,validate,child_main}` -- registry only. Calls **into** `ext/fpmng_metrics` (`fpmng_metrics_render_text`, `fpm_pool_status.c:49,297`) | 2,833 | 2,596 | Nothing at link time. **But** it is the only `/status` and the only `/metrics` endpoint in the tree (`fpm_pool_status.c:464-472`). |
| ACME challenge store | `fpm_acme_challenge.c` | `fpm_acme_challenge_init_main` (**`fpm.c:112`**), `fpm_acme_challenge_lookup` (`fpm_http.c:1822`), `fpm_acme_challenge_register_functions` (`fpm_pool_script.c`) | 3,313 | 982 | **`fpm.c` breaks.** A core file calls it unconditionally. |
| metrics (SAPI side) | `fpm_metrics.c` | `fpm_metrics_init_main` (**`fpm.c:92`**), `fpm_metrics_child_init` (**`fpm.c:183`**) | 398 | 193 | **`fpm.c` breaks.** Also `ext/fpmng_metrics` (11,511 bytes `.text`) is a separate object with its own `--enable-fpmng-metrics` flag that `ext/fpmng_metrics/config.m4` **force-enables** whenever `--enable-fpmng` is on, precisely because `fpm_metrics.c` and `fpm_pool_status.c` call its symbols. |
| `fastcgi-ng` | **none** | one `strcmp` at `fpm.c:184` | 0 | 0 | Nothing. Gating it removes a registry entry and no code (§2.4). |
| fiber | `fpm_pool_fiber*.c`, `fpm_pool_coop*.c` | already gated | -- | -- | already working, the model for the rest |
| async | `fpm_pool_async.c` | already gated | -- | -- | already working |

### 1.2 Files that look like modules but are NOT

These came up as candidates and the evidence says they are core infrastructure:

- **`fpm_child_error_log.c`** -- its header is included only by `fpm_http.c`, which
  makes it *look* gateway-owned. `nm` says it defines **`fpmng_zlog_ex`**, the
  symbol every `zlog()` call in the SAPI resolves to. It is the child→master
  log channel behind `fpm_pool_type_s.child_logs_via_master`. Removing it
  fails the link of the whole binary. A header grep alone would have got this
  wrong.
- **`fpm_http_tls.c` (6,044 `.text`) and `fpm_http_tls_reload.c` (3,681)** --
  included by both `fpm_http.c` and `fpm_http_direct_tls.c`. `http-direct` TLS
  is built on them (`fpm_http_direct_tls.c:` includes `fpm_http_tls.h`,
  `fpm_http_tls_reload.h`). They are **core**, not gateway. See §6.3.
- **`fpm_error_log_follow.c`** -- included by `fpm_stdio.c` (core) as well as
  `fpm_http.c`.
- **`fpm_children_extra.c`** -- included by `fpm_children.c` (core) as well as
  `fpm_http.c`.
- **`fpm_std_streams.c`** -- used by `fpm_http_direct_worker.c` (core mode) and
  `fpm_pool_script.c`.
- **`fpm_status.c` / `fpm_log.c`** in the object list are **upstream FPM's**
  files (the FastCGI `pm.status_path` page and the access log), copied by
  `build/prepare.sh` from `sapi/fpm/`. They are not ours and not candidates.

### 1.3 What does NOT couple

Every other match of a module name in a core file is a **comment**, verified
line by line:

- `fpm_conf.c:732,734,735,736` -- defaults with a `/* fpm-ng: … in fpm_http.c */`
  note; `fpm_conf.c:1039,1045-1046` -- comments naming
  `fpm_http_validate_pool()`, `fpm_pool_status_validate()`,
  `fpm_pool_supervisor_validate()` to explain ordering.
- `fpm_children.c:410` -- comment citing `fpm_pool_status.c`.
- `fpm_process_ctl.c:172,176` -- comments citing `fpm_pool_status.c:435-446`.
- `fpm_error_log_follow.c:54,116`, `fpm_pool_coop_statics.c:210` -- comments.

These are exactly the comments `workflow.md` forbids deleting without
re-establishing the fact. They cost nothing at build time but they mean a
`grep -l` over the tree over-reports coupling by a factor of about three.

### 1.4 The hard cases, named

1. **`fpm.c` calls ACME and metrics unconditionally** (`fpm.c:92`, `:112`,
   `:183`). These are the only module calls in a core file. Gating either
   requires either an `#ifdef` in `fpm.c` (logic, not data -- a rule problem) or
   no-op stubs.
2. **`fpm.c:184` is already a `strcmp` on a type name** -- an existing violation
   of the "data, not code" rule, already filed as **issue #153**. It is also
   exactly the line a `fastcgi-ng` gate would have to touch.
3. **`ext/fpmng_metrics` has a flag that is a lie.** `--enable-fpmng-metrics`
   defaults to yes and is force-enabled under `--enable-fpmng`
   (`ext/fpmng_metrics/config.m4`, `AC_MSG_NOTICE([fpmng_metrics forced on…])`)
   because the SAPI calls into it. Any metrics gating must fix this too, or the
   flag stays decorative.
4. **`status` → `ext/fpmng_metrics`**: gating metrics without gating status
   breaks `fpm_pool_status.c:297`. They gate together or not at all.
5. **ACME's two halves live in two different modules**: the challenge *answer*
   is served only by the gateway (`fpm_http.c:1822`); the *client* is
   documented to run in a `cron` pool. `http-direct` never answers a challenge.
6. **`fastcgi-ng` has no sources.** §2.4.

---

## 2. Where each `#ifdef` would have to live

The sanctioned pattern is: sources excluded by `config.m4`/`prepare.sh`, and
the `#ifdef` confined to the type-struct definitions and the executor-list
entries in `fpm_pool_type.c` (today `fpm_pool_type.c:54,67,83,106,108,129,175,
182,194,201` -- that plus `config.m4` is the **complete** footprint of
`HAVE_FPMNG_FIBER`/`HAVE_FPMNG_ASYNC` in the tree; verified by grep).

### 2.1 Clean -- the pattern extends with no new rule problem

`http` gateway, `supervisor`, `cron` (+`script`, `watchdog`, `cron_schedule`),
`status`. For each: three `#ifdef`s in `fpm_pool_type.c` (the `#include`, any
local wrapper such as `fpm_pool_type_http_init()`, and the `fpm_pool_types[]`
entry) plus one source group in `config.m4`/`prepare.sh`. No `#ifdef` anywhere
else. No `if (type == …)` is introduced.

One wrinkle worth deciding rather than discovering: `fpm_pool_types[]` is an
array, not a list of registrations, so an `#ifdef`-ed-out entry silently
shortens it and `fpm_pool_type_list()` stops naming the type. **Better:** give
`fpm_pool_type_s` a `.build_flag` field, exactly mirroring
`fpm_pool_executor_s.build_flag` (`fpm_pool_type.h:56-58`), keep the entry with
all callbacks NULL, and let `fpm_conf.c` report *"pool.type = http: this binary
was built without --enable-fpmng-http-gateway; rebuild with that flag"* instead
of *"unknown pool.type 'http'; known types: …"* (`fpm_conf.c:1010`). That is
data, it is already the project's own precedent one level down, and it is the
best operator message available. It costs about 40 bytes of `.rodata` per gated
type.

### 2.2 Needs a decision, not an `#ifdef`: ACME and metrics

`fpm.c:92/112/183` are unconditional calls from a core file. Three options:

- **(a) `#ifdef` in `fpm.c`.** Rejected: `fpm.c` is core logic; this is exactly
  the "`#ifdef` in logic" the rule exists to prevent, and it would set the
  precedent that any future module may put one there.
- **(b) No-op stubs in a small always-compiled file.** Works (this spike used
  stubs to link the measurement variants) but splits each module's API across
  two files and makes "is this facility present?" unanswerable at runtime.
- **(c) Do not gate them.** ACME's shared store is 3,313 bytes of `.text`, and
  its own header already argues for unconditional allocation: *"The region is
  allocated once in the master before the first fork, whether or not anything
  in the configuration uses ACME: it costs a couple of kilobytes, and
  allocating it conditionally would mean fpm.c or a pool type deciding on
  behalf of a facility that is global to the process tree."*
  (`fpm_acme_challenge.h`). `fpm_metrics.c` is 398 bytes.

**Finding, not a task:** ACME's challenge store and `fpm_metrics.c` cannot be
gated without either breaking the "data, not code" rule in `fpm.c` or adding a
stub layer, and they are together 3,711 bytes of `.text`. Recommend (c) for
both: leave them always compiled. What can honestly be gated is the *client
side* -- the builtins registered in `fpm_pool_script.c`, which already follow
cron/supervisor -- and `ext/fpmng_metrics` (11,511 bytes), but only together
with `status`.

### 2.3 `http-direct` TLS

`fpm_http_tls.c` and `fpm_http_tls_reload.c` must stay in the core source list.
Gating them with the gateway would break `pool.type = http-direct` with
`http.tls_cert`, which `docs/http-direct.md` documents as supported. See §6.3.

### 2.4 `fastcgi-ng` -- the case where gating is not possible as described

There is no `fpm_pool_fastcgi_ng.c`. The type's entry in `fpm_pool_types[]`
(`fpm_pool_type.c:229-236`) carries no `validate`, no `init_main`, no
`child_main` -- it is the ordinary FastCGI child path with a runtime switch:

```c
/* fpm.c:184 */
if (type && (!strcmp(type->name, "fastcgi-ng") || !strcmp(type->name, "http"))) {
        fcgi_set_optimized_transport(true);
        zend_signal_use_persistent_handlers(true);
}
```

`fcgi_set_optimized_transport()` is added by `patches/0004-fastcgi-ng-transport-switch.patch`
to php-src's `main/fastcgi.c`; the behaviour it switches on comes from patches
0001, 0002, 0003, 0005 and 0006. `build/prepare.sh` applies all of them
unconditionally.

Consequences, all of them findings rather than tasks:

- A `--enable-fpmng-fastcgi-ng` flag would remove a registry entry and
  **0 bytes** of code. The optimized transport stays in `main/fastcgi.c` in
  every build.
- The same switch is used by the `http` gateway's internal FastCGI leg, so the
  two cannot be separated at the patch level either.
- The line is already a rule violation and already has an issue (**#153**). Its
  fix -- a `.uses_optimized_fcgi_transport:1` flag on `fpm_pool_type_s` -- is a
  prerequisite for any `fastcgi-ng` gating, and is worth doing on its own
  merits whether or not this spike leads anywhere.
- If `fastcgi-ng` must be marked experimental, the honest mechanism is **data
  on the type** (`.experimental:1` → a startup `ZLOG_WARNING` naming the type)
  plus documentation, not a configure flag. That delivers the supported-surface
  message with no build-matrix cell and no misleading "not built" claim.

---

## 3. `fpm_conf.c` and directives belonging to an uncompiled module

### 3.1 What the fiber flag does today, and it is not consistent

`ini_fpm_pool_options[]` in `fpm_conf.c` contains, unconditionally:

```c
/* fpm_conf.c:197-198 */
{ "fiber.revalidate_freq",     &fpm_conf_set_time,        WPO(fiber_revalidate_freq) },
{ "fiber.isolate_statics",     &fpm_conf_set_string,      WPO(fiber_isolate_statics) },
```

so in a build without `--enable-fpmng-fiber` the directives still parse and
still occupy their fields in `struct fpm_worker_pool_config_s`; they are
refused per-pool by the type's `rejects` list
(`fpm_pool_fastcgi_rejects[] = { "http.", "fiber.", NULL }`,
`fpm_pool_type.c:28-32`). Two lines further down the same table does the
opposite:

```c
/* fpm_conf.c:199-201 */
#ifdef HAVE_APPARMOR
        { "apparmor_hat",              &fpm_conf_set_string,      WPO(apparmor_hat) },
#endif
```

So the table already carries **both** policies, with no note saying why they
differ. That inconsistency is itself worth an issue regardless of this spike.

### 3.2 Which gives the better operator message

Three messages are reachable today; only one is any good.

| Policy | Message the operator gets | Verdict |
|---|---|---|
| Shrink the table (`apparmor_hat` precedent) | `[/etc/fpm.conf:12] unknown entry 'http.tls_cert'` (`fpm_conf.c:1716`, `ZLOG_ERROR`) | **Worst.** Indistinguishable from a typo. Says nothing about the build. |
| Keep the directive, reject per type (fiber precedent) | `[pool www] 'http.tls_cert' is not supported by pool.type = fastcgi` | Correct but misleading here: it blames the type, not the missing build flag. |
| Keep the directive; let the **type** carry `.build_flag` | `[pool www] pool.type = http: this binary was built without --enable-fpmng-http-gateway; rebuild with that flag to use this type` | **Best**, and it is the project's existing wording for executors (`fpm_pool_type.c:` `fpm_pool_type_validate_executor()`). |

**Recommendation:** the directive table does **not** shrink. The *type* entry
stays in `fpm_pool_types[]` with `.build_flag` set and its callbacks NULL, and
the pool fails at validation naming the flag. A configuration that sets
`http.*` on a build without the gateway then fails on `pool.type = http` with a
message that tells the operator what to do, and a configuration that sets
`http.*` on a `fastcgi` pool keeps today's message unchanged. Zero BC risk:
the parse behaviour of every directive is unchanged in every build.

---

## 4. The test matrix

### 4.1 How a `.phpt` learns whether its feature is compiled in

There are two conventions in the tree and they are not equivalent.

`sapi/fpmng/tests/skipif.inc` is **upstream's** (copied by `build/prepare.sh`
from `sapi/fpm/tests/`). It knows nothing about features: Windows, root, and
"php-fpm binary not found", and nothing else. Every feature gate is written
after the `include "skipif.inc"` line, per test.

1. **Configure-line probe** (all 10 fiber-gated tests, e.g.
   `fpmng-fiber-flock.phpt:4-11`):

   ```php
   exec(escapeshellarg($binary) . ' -i 2>&1', $output, $status);
   if ($status !== 0 || !str_contains(implode("\n", $output), '--enable-fpmng-fiber')) {
       die('skip php-fpm-ng was not built with --enable-fpmng-fiber');
   }
   ```

   It greps PHP's `Configure Command` line. It generalises to any new
   `--enable-fpmng-*` flag for free. **Two weaknesses worth recording**:
   `str_contains` would also match `--enable-fpmng-fiber=no`, and it matches
   the *request* rather than the *result* -- a flag that configure accepted but
   a missing dependency disabled would still look present. Neither has bitten
   yet (CI defends the first half differently, §4.2).

2. **Behavioural probe** (`fpmng-http-direct-tls.phpt:6-35`): start a throwaway
   pool that uses the feature and skip on the specific startup message
   (`'built with TLS support'`). Slower, but it tests the binary's actual
   behaviour, not its command line. This is the better pattern, and it is
   already in the tree.

3. **The CI-side assertion that makes either of them safe**:
   `build/assert-fiber-tests-ran.sh` names the ten tests literally and fails
   the job unless each reports **PASS** -- not merely "not SKIP", because
   `run-fpmng-phpt.sh` can also bucket a test as `NOT MEASURED`/`PARTIAL` and
   still exit 0. This is the mechanism that turns "green" into "green with a
   PASS count", and it is the thing any new flag must also get.

### 4.2 Today's cost, and what the proposal costs

Today: two `.phpt` cells -- `fpmng-phpt` (canonical, reuses the `build` job's
artifact) and `fpmng-phpt-fiber` (builds its own binary because the flag
changes the SAPI; measured in the workflow comment as PASS=58, FAIL=0, SKIP=0
of 58). The fiber cell additionally asserts the configure line actually took
effect (`grep -q "fiber-based multi-request executor in fpm-ng\.\.\. yes"`) --
because "configure does not fail on an unknown `--enable` flag, it warns".
`--enable-fpmng-async` is deliberately not set anywhere (issue #87).

Measured distribution of the 61 owned `.phpt` files by the `pool.type` they
configure (`grep -lE '^pool\.type = X\s*$'`):

| `pool.type` | files |
|---|---|
| `http` (gateway) | **27** |
| `http-direct` | 15 |
| `supervisor` | 8 |
| `fastcgi-ng` | 2 |
| `status` | 2 |
| `cron` | 1 |
| `fastcgi` | 0 (it is the default; tests omit the directive) |

The 27 gateway tests include **all 5 ACME tests** and **all 7 fiber tests** --
the fiber executor is exercised through `pool.type = http`. Two tests use
`pool.type = status`, and one of them, **`fpmng-http-direct-session-status.phpt`,
is a core-mode test that depends on the status pool** (§6.1).

Consequence if the gateway, supervisor, cron and status become default-off:

- the default `fpmng-phpt` cell loses 27+8+2+1 = **38 of 61 tests (62%)** to
  SKIP;
- the existing `fpmng-phpt-fiber` cell **cannot run at all** without also
  passing the gateway flag;
- `fpmng-http-direct-session-status.phpt` -- a core-mode test -- starts skipping
  on the default build.

To keep today's coverage you need, at minimum, **one more full build+test
cell** that turns every gated module on ("everything" cell), plus the default
cell, plus the fiber cell now configured with `--enable-fpmng-fiber` *and* the
gateway flag: **3 build+test cells instead of 2**, each one a full php-src
build. That is not cheap: the existing flag-driven cell, `fpmng-phpt-fiber`,
spends **232 s of build** per push and exists for no other reason than that
fiber is behind a build flag; the `build` job itself is ~5 m (configure 21 s,
`make -j8 fpmng cli` 264 s) on run 34140645116. A third cell of that order is
roughly another 4–5 minutes of compiling per push, against a gating saving of
≤5 s (§5b). And it is **one more place where a silently skipping test can
hide**, which the project's standing rule -- a green job means nothing without
its PASS count -- exists to prevent. Each new flag therefore also needs its own
`assert-<flag>-tests-ran.sh` list, or one generalised version of it.

**Finding:** the test-matrix cost is real on both axes. In build minutes it is
the *opposite* of a saving: default-off flags add whole PHP compiles to CI
(§5b.2). In coverage it is that 38 tests would have to be re-proved as "ran
somewhere" by a named list, maintained by hand --
`build/assert-fiber-tests-ran.sh` explains at length why it must be literal and
not globbed.

---

## 5. Measurement: what gating actually buys

**Where and what.** Test box `192.168.8.50`, own directory `~/spike-modular-build/`
(removed afterwards; `~/issue-56/php-8.5.9` untouched and verified present; no
processes started, no ports in 28300-28399 used). php-8.5.9, our
`build/prepare.sh` overlay of this branch, configure line
`--disable-all --enable-fpmng --enable-session --with-openssl`, gcc, `-g -O2`.

**Method.** A module was "removed" by replacing its `.c` files with an empty
translation unit and, for the handful of symbols the registry still references,
compiling a stub file in place of one of them. The registry entries in
`fpm_pool_type.c` were left intact, so these numbers measure the **code**
removal only and slightly *understate* a real gating (which also drops the
`fpm_pool_type_s` initialisers and the `rejects` arrays -- tens of bytes). Each
variant was rebuilt from the same tree with all other sources restored and
`touch`ed; a control rebuild of the unmodified tree (`A2`) reproduced the
baseline `.text` byte for byte, and the individual deltas sum exactly to the
combined one, which is the check that the variants really were independent.

*(A first pass used `cp -p`, whose preserved mtimes made `make` skip the
restore, so the variants accumulated. Caught by `D == E`, and redone. Recorded
because the wrong numbers would have looked plausible.)*

**Sections** (`size -A`, bytes):

| variant | `.text` | Δ`.text` | `.rodata` | `.data.rel.ro` | `.bss` |
|---|---|---|---|---|---|
| baseline | 5,522,585 | -- | 2,179,160 | 747,720 | 154,416 |
| − `http` gateway | 5,494,649 | **−27,936** | 2,174,040 | 747,304 | 150,288 |
| − supervisor/cron/script/watchdog/status/cron_schedule | 5,511,801 | **−10,784** | 2,173,528 | 747,144 | 154,384 |
| − ACME store + `fpm_metrics.c` | 5,518,713 | **−3,872** | 2,178,040 | 747,368 | 154,416 |
| − all three | 5,479,993 | **−42,592** | 2,167,224 | 746,376 | 150,256 |

**Files** (bytes):

| variant | unstripped (`-g`) | stripped |
|---|---|---|
| baseline | 48,238,264 | 15,477,976 |
| − all three | 47,991,728 | **15,477,784** |
| delta | −246,536 (−0.51%) | **−192 (−0.0012%)** |

**The stripped-size result is the headline.** The link line contains
`-Wl,-zcommon-page-size=2097152 -Wl,-zmax-page-size=2097152`, so loadable
segments are padded to 2 MiB boundaries and a 60 KB reduction across all
loadable sections disappears into existing padding. Two of the five variants
produced a *byte-identical* stripped size to the baseline.

**Context.** All of our SAPI's objects together are **165,379 bytes of `.text`**
(3.0% of the binary's `.text`), plus 11,511 for `ext/fpmng_metrics`. The
maximum achievable saving from this whole exercise is 26% of our own code and
0.77% of the binary's code.

**No library dependency is removed.** `sapi/fpmng/config.m4:397-401` says
*"The HTTP gateway is not optional in fpm-ng, it is the point of it"* and makes
`PKG_CHECK_MODULES([LIBEVENT], [libevent >= 2.1])` a hard failure. `http-direct`
uses libevent's `evhttp` too, and `http-direct` TLS uses `libevent_openssl` +
OpenSSL. The core-only variant links exactly the same
`-lm -lssl -lcrypto -levent -levent_openssl` line as the baseline. So the
"fewer dependencies in the image" argument does not apply either.

**Not measured:** the static musl artifact (`build/static-full.sh`,
`LDFLAGS=-static-pie`). Its link flags differ and the alignment padding may
behave differently there, so the 192-byte result should not be assumed to
transfer. If the binary-size argument is going to be pressed at all, that is
the build to measure, and it is a separate piece of work.

**Attack surface** is the one motivation the measurement does *not* refute:
27,884 bytes of gateway `.text` includes an HTTP/1.1 parser, an access-log
writer, an ACL parser, HTTP Basic auth and `Forwarded`/`X-Forwarded-*` parsing,
all reachable from the network. That is a real reduction in reachable code even
though it is a negligible reduction in bytes. It should be stated that way and
not dressed up as a size argument.

---

## 5b. Measurement: what gating buys in COMPILE TIME

Build time is a stated motivation for this spike: the wish is not to rebuild
all of PHP for a change that only touches what we overlay -- mainly in CI, but
the local loop matters too. This section answers what a build flag can and
cannot do about that. The figures below are given facts from the project's own
measurements, not new measurements by this spike.

### 5b.1 The arithmetic that settles it

A canonical build (`--disable-all --enable-fpmng --enable-session
--with-openssl`) produces **619 object files, 50 of which are ours.** A build
flag can only ever remove files from those 50. It cannot touch Zend, `main/`,
or the extensions that `--disable-all` still forces in (lexbor, uri, pcre,
hash, json, random, date, spl, reflection, standard, opcache), because a PHP
SAPI is linked into a binary that contains them and there is no stable ABI to
build a SAPI against an installed PHP.

So the ceiling is:

| Gate | Files it removes | Share of the 619 objects |
|---|---|---|
| `http` gateway (T2) | 5 | 0.81% |
| script pools: supervisor, cron, script, watchdog, cron_schedule (T3) | 5 | 0.81% |
| ACME store + `fpm_metrics.c` (not recommended, §2.2) | 2 | 0.32% |
| `fastcgi-ng` | **0** | **0%** |
| everything above together | 12 | 1.9% |

**No build flag reduces the PHP part of the build, which is 92% of the object
count and, because our files are small and PHP's are not, more than that of the
time.** On the CI build job of run 34140645116, `make -j8 fpmng cli` took
**264 s**; even a generous linear reading of 12/619 puts the ceiling at ~5 s,
and that is for a build with *every* gate off, which is not a configuration
anybody would run in CI as the main cell.

Where it saves nothing at all, stated plainly:
- **`fastcgi-ng`**: zero. It has no `.c` file (§2.4); its code is php-src
  patches applied unconditionally by `build/prepare.sh`.
- **The configure step**: 21 s on that run, unchanged. Adding flags makes it
  marginally longer, never shorter.
- **The link**: unchanged in composition -- the same
  `-lm -lssl -lcrypto -levent -levent_openssl` (§5); dropping 12 small objects
  does not measurably change link time.
- **The local loop**: already 0–6 s. `prepare.sh` plus `make` after a source
  change recompiled 0 objects on the poligon in the established measurement,
  and 6 s rebuilds all 44 of our `.c` files plus the link. There is nothing
  here for a build flag to remove.

### 5b.2 Build flags make CI compile time WORSE, and by a larger amount

Three jobs compile PHP independently on every push:

| job | PHP compile |
|---|---|
| `build` | ~5 m (configure 21 s + `make -j8` 264 s) |
| `fpmng-phpt-fiber` | ~232 s |
| `static-musl` | ~330 s |

`fpmng-phpt-fiber` **exists only because fiber is behind a build flag.** That
is the actual price of a default-off flag, measured: **232 s per push, versus
the ≤5 s a gate could save.** Every new default-off flag that gets real
coverage adds another cell of the same order (§4.2 needs at least one, and a
two-tier default-off split needs two). A modular build justified by build time
would, on the current CI shape, multiply the thing it is meant to reduce by
roughly 50 to 1.

### 5b.3 Where the CI build time actually is, and what would reduce it

- **The same PHP is compiled three times per push.** `fpmng-phpt` already
  avoids this by reusing the `build` job's artifact; the other two do not,
  because their configure lines differ. `fpmng-phpt-fiber` differs by one flag
  that affects only our SAPI -- the ~569 PHP objects it rebuilds are identical
  to the ones `build` already produced. That is the single largest recoverable
  block: on the order of 232 s per push.
- **ccache is configured but under-exploited.** `CCACHE_DIR=/ccache`, 5 GB max,
  **currently 50 MB used** -- 1% of the allowance, on four self-hosted runners,
  so the cache is not being evicted; it is not being *populated* the way it
  could be. Cumulative hit rate is **91.7%**, but **51.9% of the hits are
  preprocessed-mode hits**, i.e. the slow path that still runs the
  preprocessor. Moving those to direct mode (`depend` mode, stable
  `-fdebug-prefix-map`/relative paths, `CCACHE_BASEDIR`, `CCACHE_SLOPPINESS`)
  is a compile-time win on all three jobs at once, and it is a configuration
  change, not an architecture change.
- **A prebuilt php-src base image** (upstream tree already configured and
  compiled, our overlay applied on top) would cut the cold path for all three.
  Not measured; listed as the third candidate, not a recommendation.

**Finding:** the build-time motivation is legitimate, but the modular build
does not serve it. The three levers above (artifact reuse across jobs, ccache
direct-mode hit rate, a prebuilt base) each target the 569 objects that no
build flag can reach. They are proposed as **T10**, and T10 does not depend on
any of the gating work. If build time is the main driver, T10 should be done
*instead of* T2/T3, not after them.

---

## 6. The open tensions

### 6.1 `status` -- resolve: `status` stays in the CORE, always compiled

Evidence: `docs/http-direct.md:77-78` -- *"FPM ping/status listeners and the
FastCGI access log are not implemented here; use a separate `pool.type = status`
for the FPM scoreboard."* Issue **#59** ("Operator parity for HTTP-direct:
ping/status, access log, allowed clients, chroot", open, `priority:high`) is
open about exactly this and says *"Status already works through a separate
pool… but a pool should be able to report on itself."*
`fpm_pool_status.c:464-472` serves `/metrics` and `/status`, and it is the only
place in the tree that serves either. `fpmng-http-direct-session-status.phpt`
-- a core-mode test -- is built on a status pool.

Putting the *only* monitoring a core mode has behind a default-off flag makes
the default build unobservable and silently disables a core-mode test. It costs
2,833 bytes of `.text` to keep.

**Coherent answer:** `status` is core. It is not an extra mode competing with
`fastcgi` and `http-direct`; it is the observability of those modes. If issue
#59 ever lands self-reporting for `http-direct`, revisit -- and say so in the
issue so the decision is not lost.

### 6.2 ACME -- resolve: do not gate the challenge store; gating gateway+cron by default breaks ACME entirely, and that must be an explicit, documented choice

Evidence: the HTTP-01 answer is served in exactly one place,
`fpm_http.c:1822` (`fpm_acme_challenge_lookup`), i.e. **only by the `http`
gateway**. `docs/acme-challenge.md:13` -- *"Both sockets of a `pool.type = http`
pool answer the challenge namespace"*. The client is documented to run in a
`cron` pool (`docs/acme-client.md:22,30`), and only `cron`/`supervisor` carry
`.publishes_acme_challenges` (`fpm_pool_type.c`). All 5 ACME `.phpt` tests
configure `pool.type = http`.

So: **`http-direct` cannot obtain a certificate today at all.** It can *use*
one (`http.tls_cert`, `docs/http-direct.md` "TLS"), and it can reload one, but
the HTTP-01 challenge can only be answered by a gateway pool. Gating gateway
and cron off by default therefore does not merely "strand TLS users of a core
mode" -- it removes the only working ACME path from the default build, and the
core mode's TLS goes back to operator-supplied certificates.

**Coherent answer, three parts:**
1. `fpm_acme_challenge.c` (the shared store, 3,313 bytes) stays always
   compiled -- its own header already argues this, and gating it would put an
   `#ifdef` in `fpm.c`.
2. Whether ACME is available in a default build is a decision *about the
   gateway and cron*, not about ACME. If they are default-off, the release
   notes must say "automatic certificates require `--enable-…-http-gateway`
   and `--enable-…-cron`" in the same breath as "http-direct supports TLS".
3. There is a real gap worth its own issue independent of this spike:
   `http-direct` should be able to answer the HTTP-01 challenge itself. That
   would make ACME work for a core-only build and would remove this tension
   entirely.

### 6.3 Shared TLS -- resolve: `fpm_http_tls.c` and `fpm_http_tls_reload.c` are CORE

`fpm_http_direct_tls.c` includes both headers; `fpm_http_direct_tls_init_main()`
is the `init_main` of the `http-direct` type *and* of its `worker` executor
variant (`fpm_pool_type.c:44-50` and the comment on `fpm_http_direct_worker`,
which records that omitting it made TLS worker pools respawn forever, issue
#55). 9,725 bytes of `.text` between the two files. They stay in the always-built
source list; only `fpm_http.c` and its five gateway-only companions move.

This is precisely the shared-dependency case the coupling map was asked to
catch, and a naive "everything named `fpm_http_*` belongs to the gateway" split
would have broken `http-direct` TLS. `fpm_child_error_log.c` (§1.2) is the
second one, and it would have broken the whole link.

### 6.4 Does "experimental" mean "not compiled" for all of them?

No. See §7.

---

## 7. Recommendation: three tiers, and separate the message from the mechanism

A two-tier split (core / not-compiled) is not honest here, because the evidence
says the members of the proposed "experimental" list are three different kinds
of thing:

- things that are genuinely research-grade and whose code an operator should
  not have in a production binary (**fiber**, **async**),
- things that are finished, tested, documented, load-bearing for somebody's
  deployment, and simply *not one of the two supported modes* (**gateway**,
  **supervisor**, **cron**, **status**, **metrics**, **ACME**),
- and one thing that has no code to gate at all (**`fastcgi-ng`**).

Calling the middle group "experimental" and refusing to compile it would mean
telling operators who run a supervisor pool today that the feature they use was
downgraded to experimental, in exchange for 10,784 bytes.

**Proposed tiers:**

| Tier | Members | Flag | Default |
|---|---|---|---|
| **1 -- core, always compiled** | `fastcgi`; `http-direct` (+ `fpm_http_direct*`, `fpm_http_tls.c`, `fpm_http_tls_reload.c`); `status`; ACME challenge store; `fpm_metrics.c`; `fpm_child_error_log.c` and the rest of §1.2 | none | -- |
| **2 -- supported, built by default, removable** | `http` gateway (+ access log, ACL, auth, forwarded); `supervisor` + `cron` (+ script, watchdog, cron_schedule); `ext/fpmng_metrics` | `--disable-fpmng-http-gateway`, `--disable-fpmng-script-pools` | **yes** |
| **3 -- experimental, off by default** | fiber, async (as today) | `--enable-fpmng-fiber`, `--enable-fpmng-async` | **no** |
| **not a build tier** | `fastcgi-ng` | -- | marked experimental by **data** on the type + docs (§2.4) |

Why this shape:

- It delivers the product decision. "Supported modes are `fastcgi` and
  `http-direct`" is a statement in `README.md`, in the docs, and -- checkably --
  in a startup warning driven by `.experimental:1` on the type. It does not
  need a compiler flag to be true.
- It delivers the two motivations that survived measurement. A hardened
  deployment gets `--disable-fpmng-http-gateway` and loses the network-reachable
  HTTP/1.1 parser, ACL, auth and forwarded-header code (attack surface, §5).
  A configuration naming a type that is not in the binary gets a message naming
  the flag (§3.2).
- It does not break a core mode's monitoring (§6.1) or its certificate story
  (§6.2).
- **Default-on means the default CI cell keeps running all 61 tests.** The
  matrix grows by one cell (a "minimal" build that turns the tier-2 flags off
  and asserts the *negative*: that the gated types are refused with the
  flag-naming message), not by the several cells a default-off split would
  need. That is a far better coverage-per-cell trade.

If the two-tier "everything else default-off" shape is kept anyway -- it is the
user's call -- then §6.1 and §6.2 must be answered explicitly in writing, §4.2's
38-test hole must be closed with a third cell and per-flag `assert-*-ran.sh`
lists, and the release notes must state that a default build has no monitoring
endpoint and no way to obtain a certificate.

---

## 8. Proposed breakdown into tasks

Not filed. Titles, acceptance criteria, labels, dependency order. Criteria are
written so someone who did not write them can check them, and each allows an
explicit negative result where the outcome is genuinely unknown.

### Order

```
T0  (decision; blocks T2..T7)
T1 ──┬── T2 ──┬── T4 ── T5 ── T7
     │        └── T6
     └── T3
T8  (independent, blocks nothing)
T9  (independent; informs T5)
T10 (independent; does NOT depend on any gating work -- if build time is the
     driver, this is the one that addresses it)
```

---

**T0. Decision: the tier split for pool types and modules, and what
"experimental" means in this project**

- **Labels:** `decision`, `area:pool-types`, `build`, `priority:high`
- **Why:** this spike ([`docs/spike-modular-build.md`](spike-modular-build.md)) measured the saving at
  42,592 bytes of `.text` and 192 bytes of stripped binary, found `status` and
  ACME load-bearing for a core mode, and found `fastcgi-ng` has no sources to
  gate. Nothing below should be built before the tier question is answered.
- **Acceptance criteria:**
  - The issue records one of: (a) three tiers as proposed in §7, (b) two tiers
    with everything default-off, or (c) no build gating at all -- and the reason,
    naming the measurements it accepts or rejects.
  - For (b), the issue states in writing what happens to `pool.type = status`
    as `http-direct`'s only monitoring, and what happens to ACME, or explicitly
    accepts both losses.
  - The issue states whether build time is still held to be a motivation for
    *gating*, given §5b (50 of 619 objects are ours; ceiling ≤5 s of a 264 s
    `make`; `fpmng-phpt-fiber` costs 232 s per push and exists only because of
    a build flag). If it is, T10 is scheduled ahead of T2/T3.
  - The meaning of the word "experimental" in this project is written down once:
    whether it implies "not compiled", "compiled but warns at startup", or only
    "documented as unsupported".
  - `README.md` names the supported modes after the decision lands.
- **Out of scope:** any code change.

---

**T1. `fpm_pool_type_s` gains `.build_flag`, so a type that is not in this build
says which flag would provide it**

- **Labels:** `enhancement`, `area:pool-types`, `build`, `priority:high`
- **Why:** `fpm_pool_executor_s` already carries `.build_flag` and
  `fpm_pool_type_validate_executor()` already produces *"this binary was built
  without …; rebuild with that flag"*. Types have no equivalent, so an
  `#ifdef`-ed-out type entry would collapse into
  `unknown pool.type 'http'; known types: …` (`fpm_conf.c:1010`), which reads
  like a typo.
- **Acceptance criteria:**
  - A pool naming a type present in the registry but absent from the build
    fails startup with a message that names both the type and the exact
    configure flag; a `.phpt` asserts the message text.
  - `fpm_pool_type_list()`'s output for a build missing a type is decided and
    asserted (either the type is listed or it is not -- state which and why).
  - No `if (type == …)` and no `strcmp(type->name, …)` is added; the only new
    `#ifdef`s are in `fpm_pool_type.c` and `sapi/fpmng/config.m4`. A grep in the
    PR body shows the complete `#ifdef` footprint.
  - Existing messages for unknown types and unknown executors are unchanged
    (existing suites green, with PASS counts).
- **Out of scope:** actually gating any module.

---

**T2. Build flag for the `http` gateway**

- **Labels:** `build`, `area:http-gateway`, `area:pool-types`, `priority:medium`
- **Depends on:** T0, T1
- **Why:** measured 27,884 bytes of `.text` / 6,168 `.rodata` across
  `fpm_http.c`, `fpm_http_access_log.c`, `fpm_http_acl.c`, `fpm_http_auth.c`,
  `fpm_http_forwarded.c`, and it is the largest network-reachable parser surface
  we own. The only symbols crossing out of it are `fpm_http_init_pool` and
  `fpm_http_validate_pool`, both referenced from `fpm_pool_type.c` alone.
- **Acceptance criteria:**
  - With the flag off, the five sources are not compiled (their objects are
    absent from the link line) and `pool.type = http` fails with T1's message.
  - **`fpm_http_tls.c` and `fpm_http_tls_reload.c` are still compiled**, and
    `fpmng-http-direct-tls.phpt` passes on the gated build -- this is the
    regression the spike identified (§6.3).
  - With the flag on, the full owned suite has the same PASS count as before
    the change (state the number).
  - The `#ifdef` footprint is confined to `fpm_pool_type.c` and `config.m4`;
    shown by grep in the PR body.
  - The measured `.text` delta of the gated build is reported -- or an explicit
    negative result if it differs from the 27,936 bytes this spike measured.
- **Out of scope:** ACME (T4), the CI cells (T5).

---

**T3. Build flag for the script-running pool types (`supervisor`, `cron`)**

- **Labels:** `build`, `area:pool-types`, `priority:low`
- **Depends on:** T0, T1
- **Why:** measured 10,007 bytes of `.text` across `fpm_pool_supervisor.c`,
  `fpm_pool_cron.c`, `fpm_cron_schedule.c`, `fpm_pool_script.c`,
  `fpm_pool_watchdog.c`. `fpm_pool_script.c` and `fpm_pool_watchdog.c` are used
  by these two types and nothing else, so they belong in the same group rather
  than in flags of their own.
- **Acceptance criteria:**
  - One flag covers all five files; with it off, `pool.type = supervisor` and
    `pool.type = cron` both fail with T1's message, and the nine `.phpt` files
    that use them SKIP with a reason naming the flag.
  - `pool.type = status` still works with the flag off (it is core; §6.1) and
    `fpmng-status-endpoints.phpt` passes.
  - `fpm_acme_challenge_register_functions()` is no longer reachable with the
    flag off, and the ACME `.phpt` files SKIP with a reason that says so rather
    than failing.
  - Measured `.text` delta reported, or an explicit negative result.
- **Out of scope:** `status` (stays core), metrics.

---

**T4. Close the ACME gap the spike found: `http-direct` cannot answer an HTTP-01
challenge**

- **Labels:** `enhancement`, `area:acme`, `area:http-direct`, `area:tls`,
  `priority:medium`
- **Depends on:** T2 (the gate makes this urgent; the gap exists today
  regardless)
- **Why:** `fpm_acme_challenge_lookup()` is called from exactly one place,
  `fpm_http.c:1822`. `docs/acme-challenge.md:13` documents the gateway as the
  only answerer. `http-direct` supports `http.tls_cert` (`docs/http-direct.md`,
  "TLS") but has no way to obtain that certificate. Gating the gateway makes a
  core mode's TLS certificate story disappear.
- **Acceptance criteria:**
  - A `pool.type = http-direct` pool answers `/.well-known/acme-challenge/<token>`
    from the shared store, on both the plain and the TLS socket, without the
    answer becoming a static file and without depending on `http.static` (the
    same constraints issue #48 criteria 4, 6, 7 put on the gateway).
  - An end-to-end `.phpt` against the fake CA (`sapi/fpmng/tests/acme-fake-ca.inc`)
    obtains a certificate for an `http-direct` pool with the gateway **not
    compiled in**.
  - The challenge namespace is never routed to the front controller, asserted
    by a test.
  - Or an explicit negative result: a written argument for why `http-direct`
    cannot answer the challenge, in which case T2's release note must say
    automatic certificates require the gateway.
- **Out of scope:** DNS-01; changing the ACME client.

---

**T5. CI: the build matrix for the gated build, with per-flag PASS assertions**

- **Labels:** `ci`, `test`, `build`, `area:ci`, `area:test-harness`,
  `priority:high`
- **Depends on:** T2, T3
- **Why:** 27 of the 61 owned `.phpt` files configure `pool.type = http`,
  including all 5 ACME and all 7 fiber tests; 8 use `supervisor`, 1 uses `cron`.
  The existing `fpmng-phpt-fiber` cell exercises the fiber executor *through the
  gateway*, so a gateway flag changes that cell too. The project's standing rule
  is that a green job means nothing without its PASS count
  (`build/assert-fiber-tests-ran.sh` and the `fpmng-phpt` job comment about
  PASS=0 under root).
- **Acceptance criteria:**
  - The number of build+test cells after the change is stated, with the measured
    wall-clock added per PR on `ubuntu-latest`. (Written when the matrix ran on
    the self-hosted box; those runners were unregistered on 2026-09-12, so the
    figure has to come from GitHub-hosted runs, where cell count costs
    concurrency rather than one box's cores.)
  - Every owned `.phpt` runs (status PASS) on at least one cell, proved by a
    script in `build/` that reads the `results.tsv` of all cells and fails if
    any owned test is PASS on none. Not a glob -- a check against the contents of
    `sapi/fpmng/tests/`, in the spirit of issue #95.
  - Each new flag's cell asserts the flag actually took effect by grepping the
    configure log for the `… yes` line, as `fpmng-phpt-fiber` does -- not by
    trusting the command line.
  - There is a cell that builds with the tier-2 flags **off** and asserts the
    *negative*: `pool.type = http` / `supervisor` / `cron` are refused with the
    flag-naming message from T1.
  - `build/assert-fiber-tests-ran.sh` is either generalised to take a flag name
    and a test list, or a sibling script per flag exists; state which.
- **Out of scope:** the static-musl cell (T9).

---

**T6. The `--SKIPIF--` feature probe: one convention, and fix the two known
weaknesses**

- **Labels:** `test`, `refactor`, `area:test-harness`, `priority:medium`
- **Depends on:** T2, T3
- **Why:** the tree has two conventions. Ten tests grep `php-fpm-ng -i` for the
  literal `--enable-fpmng-fiber` (e.g. `fpmng-fiber-flock.phpt:4-11`);
  `fpmng-http-direct-tls.phpt:6-35` instead probes the binary's behaviour. The
  configure-line probe matches `--enable-fpmng-fiber=no` as readily as `=yes`,
  and reports what was *asked for*, not what configure *decided* -- which is the
  exact failure mode `fpmng-phpt-fiber`'s `grep -q "… yes"` step exists to catch
  on the CI side but nothing catches on the test side.
- **Acceptance criteria:**
  - One documented helper (in `sapi/fpmng/tests/`, copied by
    `build/prepare.sh`) answers "is feature X in this binary?" and is used by
    every feature-gated test.
  - It does not report a feature present for a binary configured with
    `--enable-fpmng-<x>=no`; a test proves this (it can construct the `-i`
    output it parses).
  - Every currently gated test is converted, and the PASS count of both existing
    cells is unchanged (state the numbers before and after).
  - Or an explicit negative result: a written argument that a behavioural probe
    is not available for some feature, naming which and why.
- **Out of scope:** adding new tests.

---

**T7. Documentation: the supported surface, and what a default build contains**

- **Labels:** `documentation`, `build`, `priority:medium`
- **Depends on:** T0, T2, T3, T4
- **Acceptance criteria:**
  - `README.md` and `docs/NOTES.md` name the supported modes and the tier of
    every other pool type, consistent with T0.
  - Every configure flag added by T2/T3 is documented with its default and with
    what stops working when it is off -- including, if T4 came back negative,
    the sentence that automatic certificates need the gateway.
  - `docs/http-direct.md:77-78` is updated if T4 changed the answer.
  - `build/test-comment-content-rule.sh` and the doc-link checks stay green.

---

**T8. Fix `fpm.c:184`: pool-type behaviour selected by name comparison (issue
#153 already open)**

- **Labels:** `refactor`, `area:pool-types`, `priority:medium`
- **Independent**, but a prerequisite for ever gating `fastcgi-ng`.
- **Why:** `fpm.c:184` does
  `!strcmp(type->name, "fastcgi-ng") || !strcmp(type->name, "http")` to enable
  `fcgi_set_optimized_transport()` and `zend_signal_use_persistent_handlers()`.
  This is the one remaining violation of the architecture contract
  (`workflow.md`, "never `strcmp(type->name, …)`") and it is in core startup
  code. Already filed as issue #153; this spike adds the reason it matters now.
- **Acceptance criteria:**
  - The two behaviours are selected by a field on `fpm_pool_type_s`, set on the
    `fastcgi-ng` and `http` entries (and on their executor variants, which
    replace the whole struct -- the trap `fpm_http_direct_worker`'s comment
    records for `init_main`).
  - No `strcmp` on a type name remains anywhere in the tree; shown by grep in
    the PR body.
  - A test proves `pool.type = fastcgi` still does **not** get the optimized
    transport and `pool.type = fastcgi-ng` still does, in the same build.
  - The measured request-path behaviour of `fastcgi-ng` is unchanged, or the
    difference is reported.
- **Out of scope:** gating `fastcgi-ng` out of the build (it has no sources to
  gate -- see the spike, §2.4).

---

**T9. Measure the binary-size effect on the static musl artifact, or drop the
size argument for good**

- **Labels:** `measurement`, `build`, `priority:low`, `track:nice-to-have`
- **Independent**; informs T5 and T7.
- **Why:** on the dynamic build this spike measured the stripped-size saving of
  removing the gateway, the script pools, ACME and metrics at **192 bytes**
  (15,477,976 → 15,477,784), because the link line uses
  `-Wl,-zmax-page-size=2097152` and 60 KB of section shrinkage fits in existing
  alignment padding. The shipped artifact is the `-static-pie` musl build
  (`build/static-full.sh`), whose link flags differ and which was **not
  measured**.
- **Acceptance criteria:**
  - Stripped size of the static musl binary, baseline vs. a build with the
    tier-2 flags off, both produced by `build/static-full.sh`, with the binary
    under test confirmed by `strings` on a distinctive literal first.
  - The result is written into the size claim in the docs -- including, if the
    saving is again negligible, an explicit sentence that binary size is **not**
    a motivation for the modular build.
  - "Not measured" for the container image layer size is an acceptable answer if
    stated.

---

---

**T10. Cut CI compile time by attacking the 569 objects no build flag can
reach**

- **Labels:** `ci`, `build`, `measurement`, `area:ci`, `priority:high`
- **Independent.** Does not depend on T0–T9. If build time is the motivation
  for the modular build, this is the task that serves it (§5b).
- **Why:** a canonical build has 619 objects, 50 of them ours, so no build flag
  can reduce the PHP part of the compile (§5b.1) -- the ceiling for *all* the
  gating in this spike together is ~1.9% of the objects, ≤5 s of a 264 s
  `make -j8`. Meanwhile three jobs compile PHP independently per push
  (`build` ~5 m, `fpmng-phpt-fiber` ~232 s, `static-musl` ~330 s), and
  `fpmng-phpt-fiber` exists solely because fiber is behind a build flag.
  ccache (`CCACHE_DIR=/ccache`, 5 GB max) is using 50 MB and reports a 91.7%
  cumulative hit rate of which **51.9% are preprocessed-mode hits** -- the slow
  path.
- **Acceptance criteria:**
  - Wall-clock of the compile step of each of the three jobs is recorded before
    and after, from real workflow runs (run ids quoted), not from a local
    build.
  - `fpmng-phpt-fiber` either reuses the `build` job's php-src object tree and
    rebuilds only the SAPI, or the issue records the measured reason it cannot
    (e.g. the flag changes a header that PHP objects include -- name it).
  - ccache's preprocessed-mode share is reported before and after; direct-mode
    hits go up, or an explicit negative result explains what blocks direct mode
    (absolute paths in `-I`, `__FILE__`, `-g` prefixes -- name which).
  - Total PHP-compiling wall-clock per push is stated as one number before and
    after.
  - A prebuilt php-src base image is either adopted with its measured effect,
    or rejected with a reason. "Not measured" is acceptable for the container
    layer size.
- **Out of scope:** any build flag, any pool type, any change to what gets
  compiled into the SAPI.

## 9. What argues AGAINST doing this at all

Stated as plainly as the case for it.

1. **The measured benefit is 192 bytes of stripped binary**, and the measured
   compile-time benefit is at most ~5 s of a 264 s `make` (§5b). Two of the
   stated motivations -- binary size and build time -- are therefore not served
   by build flags; the build-time one is actively harmed, because each
   default-off flag needs a CI job that compiles PHP from scratch to cover it
   (§5b, §4.2). Of what is left, "config rejects a directive for a type that is
   not built" is achievable by data alone (T1 plus §3.2) without gating
   anything, and "supported-surface story" is a documentation and
   startup-warning problem, not a compiler problem.

2. **No dependency is removed.** libevent is a hard configure requirement
   because `http-direct` needs it; OpenSSL and `libevent_openssl` are needed for
   `http-direct` TLS. The core-only variant's link line was byte-identical.
   There is no smaller base image at the end of this.

3. **The cost lands on the thing this project is most careful about.** 38 of 61
   owned tests would need a second or third build cell to keep running, and the
   project's own experience (issue #97 -- ten tests that could only ever SKIP;
   the root-user job that reported PASS=0 and looked green) says that is exactly
   where coverage goes quietly missing. Every gated module permanently owes a
   hand-maintained "these tests must PASS here" list, because
   `build/assert-fiber-tests-ran.sh` explains why such a list cannot be a glob.

4. **`fastcgi-ng`, one of the named targets, cannot be gated.** Its behaviour is
   in php-src patches applied unconditionally. A flag for it would be a flag
   that removes no code -- arguably worse than no flag, because it tells
   operators something untrue about their binary. `ext/fpmng_metrics` already
   has exactly this kind of decorative flag today (force-enabled under
   `--enable-fpmng`), which is the precedent for how this goes wrong.

5. **Two of the proposed "experimental" members are load-bearing for a core
   mode.** `status` is `http-direct`'s only monitoring; ACME's only working path
   runs through the gateway plus cron. Shipping a default build with no
   monitoring endpoint and no way to get a certificate is a bigger operator
   regression than any attack-surface gain.

6. **`#ifdef` count goes up, in a codebase whose central design rule is that
   per-type behaviour is data.** Today the entire conditional-compilation
   footprint of the pool-type system is ten lines in one file plus `config.m4`.
   Each new flag adds three more, and two of the candidates (ACME, metrics)
   cannot be gated at all without either an `#ifdef` in `fpm.c` or a stub layer.
   The rule has held so far partly because there were only two flags.

7. **A cheaper alternative exists for the actual product goal.** `.experimental:1`
   on `fpm_pool_type_s`, a startup `ZLOG_WARNING` naming the type, and a section
   in `README.md` deliver "two supported modes, everything else is
   experimental" -- checkably, by a `.phpt` asserting the warning -- in roughly
   one small PR, with no new build cell, no new `#ifdef`, and no module removed
   from anybody's deployment.

8. **If build time is the driver, this is the wrong project.** 50 of 619
   objects are ours; the other 569 are PHP and no build flag touches them
   (§5b.1). The whole gating exercise has a ceiling of ~1.9% of the object
   count, while the CI shape currently compiles PHP three times per push and
   ccache serves 51.9% of its hits from the slow preprocessed path. T10
   addresses the actual cost; T2/T3 do not.

**The honest summary:** the seam is clean enough that this *can* be done well,
and the attack-surface argument for gating the gateway specifically is real.
But the size argument does not survive measurement, the build-time argument is
served by T10 and actively harmed by T2/T3, `fastcgi-ng` cannot be gated at
all, and the list as proposed breaks a core mode's monitoring and its
certificate story. If only one thing is done for the **product** goal, it
should be T1 + T8 + the `.experimental` marker: they deliver the
supported-surface decision, fix an existing rule violation, and cost nothing in
CI. If only one thing is done for the **build-time** goal, it is T10.
