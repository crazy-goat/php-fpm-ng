# 027 — An automated harness for the framework tests

**Track:** main line, but it needs re-scoping. This task was written to serve
tasks 024, 025 and 026, which have since moved to `tasks/nice-to-have/`
because they measure frameworks on `pool.executor = fiber`. The harness
itself is not fiber-specific and stays here: running a real framework over
the HTTP gateway on the default executor is worth at least as much. Whoever
picks this up decides the executor matrix first and says so in the task.

**Priority:** high. Without it, tasks 024, 025 and 026 have nowhere to put their
tests, and every framework result stays a one-off hand measurement.
**Status:** open.

## Why this is a separate task

Task 003 covers `.phpt` tests for our pool types and executors, and explicitly
puts frameworks out of scope: they belong to "a manual, documented procedure".
That was the right call for `.phpt` and the wrong end state overall — it leaves
nobody owning the most valuable tests this project has.

Everything we know about Symfony and Laravel on the fiber executor was measured
by hand, once, on one machine, by starting pools and running `curl` loops. Those
results are the project's headline claims. They are also the results most likely
to be silently undone by an unrelated commit, and nothing would notice.

Framework tests genuinely do not fit `.phpt`: they need MySQL, Redis, a
`composer install`, real application trees, several concurrent HTTP clients, and
assertions on JSON payloads rather than on output text. That is a harness, not a
test file.

## What it has to do

1. **Provision the applications reproducibly.** Today `~/rd/apps/symfony` and
   `~/rd/apps/laravel` exist only on one test box, hand-built, and would be
   irreproducible if that disk died. The probe endpoints
   (`src/Controller/ProbeController.php`, `routes/web.php`) are the actual test
   fixtures and the repository does not own them.
2. **Pin versions.** Results are meaningless without them: Symfony 8.1.6,
   Laravel 13.30.1 are what the current numbers refer to. A framework upgrade
   changing a result is information; a framework upgrade silently changing a
   result is not.
3. **Run the concurrency scenarios and assert on data.** Every framework failure
   this project has found returned HTTP 200. Status-code assertions would have
   passed on all of them. The assertions that matter:
   - each concurrent request sees its own session id and its own session data
   - each sees its own authenticated identity
   - object identities (container, request) are distinct per request
   - a second round with the same cookies sees its own persisted state
4. **Include the negative control.** The class-statics result was only credible
   because the same run with the feature disabled reproduced the original
   failure. A test that only ever passes proves less than one that is shown to
   fail when the mechanism is removed.
5. **Report per framework and per scenario**, in a form that can be pasted into
   `docs/frameworks.md`, so the document stops being hand-maintained.

## Acceptance criteria

1. A single documented command provisions everything and runs the suite against
   a built `php-fpm-ng`, from a clean machine.
2. The scenarios currently recorded in `docs/frameworks.md` for Symfony and
   Laravel run in it and reproduce the recorded results.
3. Each scenario has a negative control, or states why it cannot have one.
4. The suite distinguishes three outcomes, and never collapses the last two:
   pass / fail / **could not run** (a missing service, a failed `composer
   install`). A skipped scenario reported as a pass is the failure mode to
   design against.
5. It can run without the shared test box — the box is convenient, not a
   dependency. If it needs containers, they are part of this task.
6. Tasks 024, 025 and 026 can express their test requirements in it without
   further infrastructure work.

## Explicitly out of scope

- Running this on every commit. Deciding whether it belongs in CI (task 002),
  and at what frequency, comes after it exists and we know what it costs.
- Performance measurement. Correctness only; the throughput numbers stay a
  deliberate manual exercise on dedicated hardware.

## Notes

- `pm.max_children = 1` is what forces requests to actually overlap in one
  worker. Any harness that loses that setting stops testing the thing we care
  about while still passing.
- MySQL and Redis are shared on the test box. The harness must use its own
  database and its own Redis index and never issue `FLUSHALL` — this is a
  standing rule for the box and a correctness requirement for parallel runs.
