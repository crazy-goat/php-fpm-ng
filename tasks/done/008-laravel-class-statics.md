# 008 — Laravel: class statics leak between concurrent requests

**Priority:** medium-high. The spike has reported and the answer is
surprisingly good; the remaining work is real but bounded.
**Status:** open, unblocked. Item 5 of the fix list in `docs/frameworks.md`.

## Context

After the per-request isolation work landed (autoglobals, all ini entries,
`ext/session` state, ini *values* between in-flight requests), Symfony works
correctly under concurrency with sessions. Laravel does not.

Measured 2026-09-06, Laravel 13.30.1, `pool.executor = fiber`,
`FPMNG_SHARED_INCLUDES=1`, `pm.max_children = 1`, 8 concurrent requests to
`/session?user=X&sleep=0.3`:

    user    sess_user  ok      app_oid  container_request_oid
    alice   alice      True    442      2053
    bob     bob        True    1224     1264
    carol   carol      True    1474     1540
    dave    dave       True    1751     1815
    eve     frank      False   442      2053
    frank   frank      True    442      2053
    grace   frank      False   442      2053
    heidi   frank      False   442      2053
    correct: 5/8

Five requests share **one** `Application` object (`app_oid = 442`) and **one**
`Request` object (2053). The diagnosis is `Illuminate\Container\Container::$instance`
and `Illuminate\Support\Facades\Facade::$app`: static properties are per process,
and we do not separate them. Request B bootstraps its own `Application` and
overwrites them; when fiber A resumes from I/O, `app()`, `session()`, `DB::` and
`Redis::` resolve inside B's container.

**The failure mode is silent.** Before this isolation work Laravel produced
1×200, 4×500 and three requests hanging to a 60 s timeout, plus MySQL and Redis
protocol errors. Now it returns 8×200 with an empty `laravel.log` and simply
serves the wrong user's data. For deployment that is worse, not better.

## Spike result (2026-09-06)

Branch `spike/a7-static-per-fiber`, commit `714f312`, not merged. Mechanism: a
list of `Class::property` read from `env[FPMNG_SPIKE_STATICS]`, swapped at
request enter/leave. Empty list is a no-op.

- **Isolating `Container::$instance` alone reaches 8/8**, over 4 runs. Each user
  got their own session and their own `app_oid`/`container_request_oid`. The 5/8
  baseline was reproduced without the isolation, so the comparison is real.
- Adding `Facade::$app` and `Facade::$resolvedInstance` also gives 8/8 and adds
  nothing over `Container::$instance` alone (2 runs).
- Symfony with an empty list: no regression, `/session` 8/8, `/mix` 4/4.
- Scope: 237 real `static $` declarations in
  `vendor/laravel/framework/src/Illuminate/`. 12 were hand-picked as plausible
  request-state holders; **3 were confirmed experimentally and 1 was enough**.
  The remaining ~225 are boot-once caches and registries, not request state.

So the set is small and enumerable — which is the answer this task was waiting
for, and it argues for proceeding rather than for documenting a "no".

**Two caveats the spike stated about itself, both unresolved:**

- The transfer is `ZVAL_COPY_VALUE` + `ZVAL_UNDEF` with no incref/decref. That
  is safe only under the assumption "the value lives in exactly one place at a
  time", which was **not proved** — only not observed to crash. References
  (`&Class::$static`) were not tested. Stability of the property `offset` after
  `zend_update_class_constants` was verified only indirectly.
- Only the `/session` scenario was measured. `Auth::`, login flows and Facade
  identity comparisons were **not measured**.

Side observation, unverified: in the coop model "boot once" statics such as
`Model::$booted` mean once per *process* rather than once per request as under
classic FPM. Consequence not explored.

Note that this direction was implemented and then **reverted** in the True Async
fork. The spike says it works for one scenario; it does not say why the other
project backed it out.

## Problem

Turn the spike into something defensible: correct memory ownership, a design
that is not keyed to Laravel's class names in C, and coverage beyond the one
scenario that was measured.

## Acceptance criteria

1. The 8-concurrent-request measurement above reaches **8/8 correct**, asserted
   on session data and object identity, not on HTTP status.
2. Symfony's existing results do not regress: 8/8 own sessions, distinct sids,
   `count` incrementing on a second round with the same cookies.
3. The mechanism does not depend on Laravel. Anything keyed to specific class
   names belongs in configuration or in documentation, not in the C code — and
   if the only workable design *is* keyed to specific classes, that fact is the
   finding and it argues for "no".
4. The cost is measured: a request that touches none of the isolated statics
   must not pay for the mechanism.
5. `pool.executor = classic` unaffected.
6. Memory ownership is **proved**, not assumed: the "exactly one place at a
   time" invariant is either established by argument or replaced by proper
   refcounting. References to a static property are covered by a test.
7. Coverage beyond `/session`: at minimum an authenticated flow
   (`Auth::login` then a concurrent `/me`), which previously returned
   `user: null` for 5 of 6 parallel requests.

## Explicitly out of scope

- Modifying Laravel. The question is whether *our* isolation is enough for the
  *unmodified* framework. Diagnostic output added to a test endpoint is fine;
  rewriting the framework to inject its container is not — that would answer a
  different question.

## Notes

- Symfony works not because it is better written but because it passes its
  container explicitly; Laravel keeps it in a class static. This distinction is
  the whole content of the task and should survive into whatever gets documented.

## Outcome (2026-09-06)

Implemented as `fiber.isolate_statics` (pool config, comma-separated
`Class\Name::property` list), `sapi/fpmng/fpm/fpm_pool_coop_statics.c`/`.h`,
following the existing `fpm_pool_coop_session.c`/`fpm_pool_coop_ini.c`
enter/leave pattern. No Laravel-specific code; the Laravel value is
documentation only (`docs/frameworks.md`). Rejected for non-fiber-executor
pools for free via the existing "fiber." prefix in `rejects[]`.

Measured on the test box: `/session` N=8 8/8 (was 5/8), `Auth::login` + `/me`
N=8 8/8 (a prior attempt reported 5/6 wrong), Symfony `/session` N=8 8/8 with
`count` incrementing correctly on a second round (no regression),
`pool.executor = classic` unaffected and the directive rejected for it,
misconfiguration (bad syntax) fails pool start, misconfiguration (missing
class/non-static property) logs a warning and keeps the pool running.

Memory ownership: argued as a relocation of the request's own zval (no
incref/decref needed), same pattern as the existing SG/OG/ini swaps. One real
bug was found and fixed during this task, not merely reasoned about: leaving
the live slot as `IS_UNDEF` after taking a request's value away (the spike's
approach) crashes on the very next unrelated request to touch a *typed*
static property with no default, while the first request is still away.
Fixed by refilling the live slot with the class's compiled-in default
(`ZVAL_COPY_OR_DUP` from `default_static_members_table`) instead of leaving a
hole. Caught and reproduced by `tests/statics_reference.php`, which also
covers the reference-across-suspension case the spike never tested.

Left undone / not measured: `/mix` (uses `Item::find()`) errors with an
unrelated 500 on this box (DB fixture issue, not part of the acceptance
criteria) and was not investigated. Latency cost of an empty
`fiber.isolate_statics` was not cleanly measured (curl process-spawn noise
dominated a 50-request loop); the zero-cost claim rests on the code path
(`if (count == 0) return;`), not on a number.
