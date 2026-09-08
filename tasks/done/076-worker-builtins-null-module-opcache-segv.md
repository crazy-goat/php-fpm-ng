# 076 — Worker-mode builtins crash opcache's optimizer

Status: done
Type: bug
Depends on: —
Related: 073 (introduced the executor), 074 (blocked by this)

## Why

Every `pool.type = http-direct` + `pool.executor = worker` child segfaults at
startup, in a respawn loop, as soon as two things are true at once: opcache is
enabled, and the worker script's compiled code contains a literal
`function_exists('fpmng_worker_…')`. Both are the normal case — opcache is on by
default in any production build, and the driver shipped with the POC does
exactly that check (`examples/http-direct-worker/FpmngDriver.php:33`,
`return \function_exists('fpmng_worker_loop');`). Worker mode as merged in task
073 is therefore unusable outside the test suite.

The mechanism, measured rather than assumed:

`sapi/fpmng/fpm/fpm_http_direct_worker.c:1130` registers the worker builtins
with `zend_register_functions(NULL, fpm_worker_functions, CG(function_table),
MODULE_PERSISTENT)`. That sets `internal_function->module = EG(current_module)`,
and `EG(current_module)` is NULL outside a module's own startup — which is where
this call happens, long after startup, in the forked child. opcache's `pass1`
constant-folds `function_exists()` at compile time and dereferences
`func->module->type` without a NULL check
(`Zend/Optimizer/zend_optimizer.c:114`, PHP 8.5).

Evidence, x86_64 static-full binary on the test box, from a core dump:

```
#0  zend_optimizer_eval_special_func_call ... Zend/Optimizer/zend_optimizer.c:114
#1  zend_optimizer_pass1 ... Zend/Optimizer/pass1.c:254
#4  cache_script_in_shared_memory ... ext/opcache/ZendAccelerator.c:1581
#14 fpm_http_direct_worker_child_main ... sapi/fpmng/fpm/fpm_http_direct_worker.c:1152
```

Faulting instruction `cmpb $0x1,0x8c(%rax)` with `rax = 0`: offset 0x8c is
`zend_module_entry.type`, the compared value 1 is `MODULE_PERSISTENT`, and
`%rax` is `func->module`. The op_array being optimised is
`Fpmng\Poc\FpmngDriver::isSupported`.

Four independent confirmations that it is this and not something else:

- the same binary running a worker script with no `function_exists()` call
  serves normally;
- `pool.executor = classic` compiles the same `FpmngDriver.php` under the same
  opcache settings without crashing — the worker builtins are not registered in
  that executor, so the lookup finds nothing and folding bails out;
- `php_admin_value[opcache.enable] = 0` makes it go away;
  `php_admin_value[opcache.jit] = disable` does **not** — it is the optimizer,
  not the JIT;
- rewriting the call to pass a variable instead of a literal, which is what
  defeats `pass1` folding, makes it go away and the worker then serves
  (`hello world from pid 1359853`).

The existing phpt suite does not catch this because the test build is
`--disable-all` and has no opcache at all.

## Scope

Make the worker builtins safe for opcache's compile-time evaluation of
`function_exists()` / `is_callable()`, and cover the combination that is
currently untested: the worker executor with opcache enabled.

Whether the builtins gain a real `zend_module_entry`, or the registration is
made to happen with a valid `EG(current_module)`, or some third arrangement, is
the implementer's decision. The constraint is that
`function_exists('fpmng_worker_loop')` must be foldable without crashing and
must fold to the correct answer — `true` in a worker child, and unchanged
behaviour (not registered) in every other executor.

Fixing php-src's missing NULL check is **not** in scope: `patches/` exists for
core changes, but a SAPI that registers functions without a module is the party
in the wrong here, and a core patch would leave the same crash in place for
anyone building against an unpatched php-src.

## Acceptance criteria

- A worker child whose script contains a literal
  `function_exists('fpmng_worker_loop')` starts and serves a request with
  opcache **enabled**, on a build that actually has opcache compiled in.
- `var_dump(function_exists('fpmng_worker_loop'))` in a worker script prints
  `true` with opcache both enabled and disabled, and the value is identical
  whether or not the call site uses a literal.
- The same call in a `pool.executor = classic` child of `pool.type = http-direct`
  still prints `false`, with opcache enabled and disabled. Worker builtins must
  not become globally visible.
- `examples/http-direct-worker/` runs with opcache enabled and no
  `opcache.enable = 0` workaround anywhere in the example or its README.
- A regression test in `sapi/fpmng/tests/` covers the worker executor with
  opcache enabled and a literal `function_exists()` in the script, and it is
  skipped (not failed) on a build without opcache.
- The test is reachable from CI on at least one job that has opcache, or, if no
  such job exists, `.github/workflows/build-matrix.yml` gains one. Say which was
  done.
- No change to the observable behaviour of any other pool type or executor.

## Out of scope

- The duplicated request/path handling between `fpm_http_direct.c` and
  `fpm_http_direct_worker.c` (separate finding).
- `fpm_pool_type_resolve()` still comparing type names for `fiber`/`async`
  (separate finding).
- Anything in `examples/http-direct-worker-mysql/` — that is task 074, which is
  waiting on this fix.
- Making the amphp harness run in CI (separate finding).

## Outcome

Fixed in `sapi/fpmng/fpm/fpm_http_direct_worker.c`: the worker builtins are now
registered through a new `fpm_worker_register_functions()` helper that
temporarily points `EG(current_module)` at a small static
`fpm_worker_module_entry` (never added to `module_registry`, no
MINIT/MSHUTDOWN) for the duration of the `zend_register_functions()` call,
then restores whatever `EG(current_module)` was before. `internal_function->module`
is therefore always a valid pointer instead of NULL, which fixes the
dereference in opcache's `pass1`
(`Zend/Optimizer/zend_optimizer.c:113-118`, the NULL dereference on line
114). The anchor module deliberately
claims `MODULE_TEMPORARY`, not `MODULE_PERSISTENT`: a first version of this
fix used `MODULE_PERSISTENT`, which fixed the crash but made
`function_exists('fpmng_worker_...')` foldable again — and opcache's SHM
entry for a compiled script is shared across every pool/executor in the
process tree, keyed on path alone, so a worker child that compiled a shared
file first could bake `true` into that cache entry and leak it into a later
classic/fiber/fastcgi child hitting the same cached script (a real review
finding, not a hypothetical — see the regression test below for how it's
now measured). `MODULE_TEMPORARY` keeps `func->module` non-NULL (fixing the
crash) while making the optimizer's fold condition
(`func->module->type == MODULE_PERSISTENT`) always false, so
`function_exists()`/`is_callable()` always fall through to correct per-child
runtime evaluation instead of being folded at compile time at all. No
php-src patch; the fix stays entirely in `sapi/fpmng/`.

Reproduced pre-fix, on the test box, with a real opcache-enabled build
(`--disable-all --enable-fpmng --enable-session --enable-opcache
--with-openssl`): a worker pool with `function_exists('fpmng_worker_loop')`
literal in the script respawn-crashed continuously —
`WARNING: [pool worker] child NNNN exited on signal 11 (SIGSEGV - core
dumped) after ~0.3-1.0s from start`. Post-fix, the identical repro and config
served normally (`child NNNN said into stderr: "bool(true)"`, clean exit,
no crash).

Also measured, and worth recording since it corrects an assumption this task
file's own Evidence section made: `ext/opcache/config.m4` calls
`PHP_NEW_EXTENSION([opcache], ..., [yes])`, so on this php-src (pinned
`php-8.5.9`) opcache builds in and `opcache.enable` defaults to `On`
regardless of whether `--enable-opcache` is passed at all — the flag is
accepted-but-inert (`configure: WARNING: unrecognized options:
--enable-opcache` is printed, harmlessly). Concretely: a from-scratch tree
configured with the *canonical* CI `build`/`fpmng-phpt` flags
(`--disable-all --enable-fpmng --enable-session --with-openssl`, no opcache
flag at all) still reports `Zend OPcache` loaded and `opcache.enable => On`
via `php -m` / `php -i`, and reproduces the pre-fix SIGSEGV respawn loop
identically to a build with `--enable-opcache` added. In other words, this
task file's claim that "the test build is `--disable-all` and has no
opcache at all" was not correct for this php-src version — the *existing*,
already-merged canonical `build`/`fpmng-phpt` CI jobs, and any consumer
following that exact configure line, already have opcache compiled in and
enabled by default, so worker mode as merged in task 073 was already broken
there too, not just in explicitly `--enable-opcache` configurations. This
does not change the fix (it is unconditional and correct regardless of how
opcache got enabled).

CI coverage: **no new CI job was added.** Precisely because of the finding
above, the existing `build` + `fpmng-phpt` cell already builds with opcache
compiled in and active, and `fpmng-phpt` already runs every
`fpmng-*.phpt` file including the new one — a separate `--enable-opcache`
cell would have built and run the identical thing a second time for no
added signal. `.github/workflows/build-matrix.yml` gained a comment next to
the `fpmng-phpt` job recording this reasoning and the measurement behind it,
so the "say which was done" criterion is answered: reused the existing job,
did not add a new one. `static-musl` also has opcache in
(`build/static-full.sh`) but only smoke-tests a scratch container, never the
`.phpt` suite, so it doesn't cover this either way.

New regression test: `sapi/fpmng/tests/fpmng-http-direct-worker-opcache.phpt`.
Skips (does not fail) when the `Zend OPcache` extension isn't compiled in at
all; when it is compiled in but disabled by ini, the test itself asserts
`opcache_get_status(false)['opcache_enabled'] === true` in both the worker
and classic responses and fails loudly rather than silently passing with
folding never having been exercised (a review finding: `php_admin_value
[opcache.enable] = 1` in the pool config is a no-op if opcache is already
off — `OnEnable` can only report success when it's already on, never
actually turn it on). The worker and classic front controllers both
`require` the *same* `shared.php`, which does the literal
`function_exists('fpmng_worker_loop')` / `function_exists('fpmng_worker_this_does_not_exist')`
checks; the worker pool is hit first, so if the anchor module ever regresses
back to `MODULE_PERSISTENT` this reproduces the SHM-cache leak described
above (classic pool inheriting `hasWorkerLoop => true` from the worker
child's compile), not just "is the function registered" — two separate
files would only have proven the weaker claim.

Full fpmng-owned `.phpt` suite (`build/run-fpmng-phpt.sh`), run on the test
box against the post-fix opcache-enabled build, re-run again after the
post-review fixes below (`MODULE_TEMPORARY`, the shared-file leak test, the
`opcache_enabled` assertions) against a rebuilt binary: 24 tests discovered,
PASS=16, SKIP=8 (all fiber tests, skipped because that build didn't pass
`--enable-fpmng-fiber`, unrelated to this change), FAIL=0, ERROR=0 both
times. Both `fpmng-http-direct-worker-opcache.phpt` (final version) and the
existing `fpmng-http-direct-worker.phpt` (task 073) show PASS. Also re-ran
the plain repro (`function_exists('fpmng_worker_loop')` in a worker script,
opcache enabled) against the rebuilt post-review binary directly:
`child NNNN said into stderr: "bool(true)"`, no crash. Confirmed separately
that every existing `*fiber*`/`pool.executor = fiber` test already sets
`php_admin_value[opcache.enable] = 0` in its pool config, so
`fpm_pool_coop.c`'s existing opcache-rejection guard is unaffected by any of
this and those tests won't start failing now that opcache is known to
default to on.

`examples/http-direct-worker/`: confirmed no `opcache.enable = 0` workaround
exists anywhere in the example directory or its README (nothing to change to
satisfy that acceptance criterion).

An independent review pass (before this Outcome section was written) found
three major issues in the first version of this change, all addressed above:
the `MODULE_PERSISTENT` cross-pool opcache-leak risk (fixed by switching to
`MODULE_TEMPORARY`), the test's `opcache.enable = 1` no-op giving a false
sense of coverage (fixed by asserting `opcache_enabled` at runtime), and the
now-redundant `build-opcache`/`fpmng-phpt-opcache` CI job pair (removed, per
the CI coverage note above). The review also flagged that the workflow
comments and this file's own Evidence section contradicted each other about
whether the canonical build has opcache — resolved by rewriting both to
state the measured fact plainly.

Not measured: behaviour on Windows (`fpm_worker_module_entry` and this whole
executor are POSIX-only already); behaviour on GitHub Actions'
`ubuntu-latest` runner specifically was not simulated locally beyond the
test-box poligon build — that is what the PR's own CI run (`build`/
`fpmng-phpt`, which per the CI-coverage note above does have opcache active)
is for.
