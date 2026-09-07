# 027 — An automated harness for the framework tests

**Track:** main line, but it needs re-scoping. This task was written to serve
tasks 024, 025 and 026, which have since moved to `tasks/nice-to-have/`
because they measure frameworks on `pool.executor = fiber`. The harness
itself is not fiber-specific and stays here: running a real framework over
the HTTP gateway on the default executor is worth at least as much. Whoever
picks this up decides the executor matrix first and says so in the task.

**Priority:** high. Without it, tasks 024, 025 and 026 have nowhere to put their
tests, and every framework result stays a one-off hand measurement.
**Status:** done.

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

## Outcome

Substantial harness work already existed for all three frameworks (Symfony,
Laravel, Slim 4) — a repository-owned, data-asserting runner per framework —
by the time this task was picked up. The gap this pass closed was
acceptance criterion 5: Laravel's and Slim4's runners required an
already-running external MySQL/Redis, unlike Symfony's, so they could not
run on a clean machine.

**Executor matrix: fiber-only.** All three runners exercise
`pool.executor = fiber` only. Default/classic-executor coverage is not
part of this harness — it would need its own probe scenarios and pool
configs per framework, which is a separate, sizeable piece of work, not a
one-line addition to what exists. The task's own opening paragraph left this
decision to whoever picked it up; fiber-only was chosen because that is what
tasks 024/025/026 and `docs/frameworks.md` actually measure today, and
extending to the default executor without an immediate consumer would be
speculative scope.

**What was added:**

- `tests/frameworks/laravel/compose.yaml` and
  `tests/frameworks/slim4/compose.yaml`: private MySQL (`mysql:8.4.6`) and
  Redis (`redis:7.4.2-alpine`), pinned to the same versions as
  `tests/frameworks/symfony/compose.yaml` so a framework-to-framework
  comparison is not confounded by a service-version difference.
- `SERVICE_MODE` (default `docker`) added to `laravel/bin/run.sh` and
  `slim4/bin/run.sh`, mirroring the Symfony runner: private free host ports
  via a `choose_free_port` helper copied from `symfony/run.sh`, a private
  per-run database name, and full teardown (`docker compose down --volumes
  --remove-orphans`) on exit, trap-based, never issuing `FLUSHALL`/`FLUSHDB`.
  `SERVICE_MODE=external` preserves each runner's previous env-var-driven
  behaviour unchanged, for pointing at already-running services (e.g. the
  shared test box).
- `tests/frameworks/run-all.sh`: one documented command that runs all three
  runners in sequence (sequential because they share default ports and could
  collide if raced — parallelizing them is future work, not this task) with
  `SERVICE_MODE=docker` by default, forwarding `FPMNG_BIN`/`PHP_BIN` under
  the env var name each runner itself expects (`FPMNG_BIN`/`PHP_BIN` for
  Symfony, `FPMNG`/`PHP` for Laravel and Slim 4). Captures each runner's
  output under `tests/frameworks/.runs/<run-id>/`, classifies each framework
  as PASS/ERROR/NOT MEASURED from that runner's own exit status and reuses
  its own summary line (`Summary: PASS=... ERROR=... NOT MEASURED=...` for
  Symfony, `SUMMARY configured_pass=...` for Laravel, `SUMMARY pass=...` for
  Slim 4) rather than inventing new vocabulary, and exits non-zero unless
  every framework fully passed — a NOT MEASURED framework is never counted
  as a pass.
- `tests/frameworks/README.md`: the one documented top-level command, links
  to each framework's own README, and the scope decisions above.

**What was measured:** end-to-end runs against the pre-built
`fpmng-symfony024-build` binary (confirmed via `strings` to carry
`FPMNG_SHARED_INCLUDES`, `http.front_controller`, `fiber.revalidate_freq`,
`fiber.isolate_statics`), on a machine with no external MySQL/Redis
pre-provisioned, no `~/rd/apps/*`, and no `ext-redis` (so `REDIS_CLIENT=predis`
was used for Laravel — see `findings.md`):

- `laravel/bin/run.sh` with `SERVICE_MODE=docker`: configured suite
  `pass=10 error=0`; negative controls `pass=1 error=9` (a negative control
  is expected to fail without `fiber.isolate_statics`; this matches the
  behaviour documented in `laravel/README.md`, unchanged from before this
  task). Docker Compose project torn down cleanly on exit (`docker ps -a`
  showed no leftover containers).
- `slim4/bin/run.sh` with `SERVICE_MODE=docker`: `pass=9 error=0
  not_measured=2` in the default mode, `pass=11 error=0 not_measured=0` with
  `SLIM_CONTAINER=php-di SLIM_ROUTE_CACHE=1`. Compose project torn down
  cleanly on exit.
- `tests/frameworks/run-all.sh` end to end (post-review, see below): Symfony
  `PASS` (`PASS=21 ERROR=0 NOT MEASURED=0`), Laravel `PASS`
  (`configured_pass=10 configured_error=0 negative_pass=1 negative_error=9`
  — a healthy negative-control result, verdicted PASS by `run-all.sh`'s own
  `classify_laravel`, not by Laravel's raw exit status; see "Post-review
  fixes" below), Slim 4 `PASS` (`pass=9 error=0 not_measured=2`). All three
  Docker Compose projects torn down cleanly; overall exit **0**.

**Post-review fixes.** An independent review of the branch found six major
issues, all fixed in follow-up commits (not amended):

1. `setup_services` in `laravel/bin/run.sh` and `slim4/bin/run.sh` could
   set `DOCKER_STARTED=1` and then `exit 2` on a later precondition (e.g.
   Laravel's default-path phpredis-extension check, which fires on any
   machine without a built `redis.so` — exactly the machine used for
   verification) before the `cleanup()` trap was installed, leaking the
   compose project and its volume forever. Fixed by trapping a
   `cleanup_services()` function immediately after `mkdir -p "$RUN_DIR"`,
   before any code that can exit; confirmed no leaked containers/volumes
   after reproducing the exact early-exit path directly.
2. `run-all.sh` mapped Laravel's raw exit status straight to ERROR, but
   Laravel's own `bin/run.sh` exits non-zero whenever a negative control
   correctly demonstrates the expected failure — the designed, healthy
   outcome — so the combined summary could never report Laravel as PASS.
   Fixed by parsing Laravel's own `SUMMARY configured_pass=...
   configured_error=...` line instead of its exit status: PASS when
   `configured_error=0`. Laravel's own script and negative controls are
   unchanged.
3. `FCGI_PORT` and `HTTP_PORT` were chosen by two independent
   `choose_free_port` searches only 1 apart by default; if the FCGI default
   was occupied, both searches could land on the same bumped port,
   producing a broken pool config. Fixed by starting the `HTTP_PORT` search
   strictly after the `FCGI_PORT` actually chosen; reproduced the collision
   and confirmed the fix picks distinct ports.
4. Slim 4 had no dependency provisioner (`bin/run.sh` just told the user to
   run `composer install` manually), unlike Symfony and Laravel. Added
   `slim4/bin/provision.sh` (same pattern as Laravel's) and wired it in
   automatically. Also, `run-all.sh` forwarded no `REDIS_CLIENT` to
   Laravel, so Laravel defaulted to `phpredis` and aborted without
   `REDIS_EXTENSION`; `run-all.sh` now defaults `REDIS_CLIENT=predis` for
   the Laravel run unless the caller has set `REDIS_CLIENT` or
   `REDIS_EXTENSION`.
5. `port_is_free` shells out to `nc -z`; if `nc` is missing, it silently
   treats every port as free, turning `choose_free_port` into a no-op.
   Added an `nc` precondition check (alongside `curl`) to both runners.
6. `RUN_ID` (caller-controllable via `FPMNG_RUN_ID`) was not sanitized
   before use as a MySQL database name or a Docker Compose project name.
   Fixed to match `tests/frameworks/symfony/run.sh`'s originals:
   `${RUN_ID//[^A-Za-z0-9]/_}` for the database name, `tr 'A-Z:' 'a-z--'`
   for the Compose project name.

Re-verified end to end after all six fixes: `laravel/bin/run.sh` and
`slim4/bin/run.sh` individually in `SERVICE_MODE=docker` (same pass/error
counts as above), the simulated early-exit leak path confirmed clean
(`docker ps -a` / `docker volume ls` empty), the FCGI/HTTP port collision
confirmed fixed under a forced port conflict, and `tests/frameworks/run-all.sh`
end to end with no `REDIS_CLIENT`/`REDIS_EXTENSION` set: all three
frameworks PASS, overall exit 0, no leaked containers or volumes.

**Explicitly left out** (per the task's own "Explicitly out of scope"
section, plus decisions made along the way):

- Running this in CI, or deciding the frequency (task 002's territory).
- Performance measurement (unchanged: correctness only).
- Default/classic-executor coverage for any of the three frameworks.
- Parallelizing the three runners inside `run-all.sh`.
- Reproducing the exact recorded phpredis numbers for Laravel (this
  machine has no `ext-redis`; `REDIS_CLIENT=predis` was used instead, which
  `laravel/README.md` already documents as a different measurement).
- Changing Laravel's own `bin/run.sh` exit-status convention itself (a
  fully-successful run there is still a non-zero process exit by design;
  only how `run-all.sh` *interprets* that result was changed, per the
  review). Giving Laravel's own script a distinct "healthy" exit code is a
  possible follow-up, not done here to avoid touching existing
  scenario/assertion logic beyond what the review required.
