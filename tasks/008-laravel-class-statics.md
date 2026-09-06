# 008 — Laravel: class statics leak between concurrent requests

**Priority:** blocked on a spike; do not start implementing before it reports.
**Status:** open. Item 5 of the fix list in `docs/frameworks.md`.

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

## Blocked on

A spike is running that answers, before anyone writes an engine mechanism:

- does isolating `Container::$instance` alone reach 8/8?
- does adding the `Facade` statics reach 8/8?
- if not, what else leaks — named, not guessed
- how many static properties in `vendor/laravel/framework` hold request-scoped
  state at all

That last number decides whether a general mechanism is worth building. This
direction was implemented and then **reverted** in the True Async fork, which is
reason enough not to start from optimism.

## Problem (conditional on the spike)

If the spike shows a small, enumerable set of statics is enough: decide whether
to isolate class statics per fiber, and if so, do it.

If the spike shows the set is large or unbounded: record that Laravel is not
supportable on the fiber executor, with the evidence, and close this task by
moving it to `done/` with that outcome. A documented "no" is a completed task.

## Acceptance criteria (if it proceeds)

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

## Explicitly out of scope

- Modifying Laravel. The question is whether *our* isolation is enough for the
  *unmodified* framework. Diagnostic output added to a test endpoint is fine;
  rewriting the framework to inject its container is not — that would answer a
  different question.

## Notes

- Symfony works not because it is better written but because it passes its
  container explicitly; Laravel keeps it in a class static. This distinction is
  the whole content of the task and should survive into whatever gets documented.
