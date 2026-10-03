# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The entries for v0.1.0 to v0.11.1 were rebuilt from the tagged history (`git log <previous tag>..<tag>`, merges included),
because the GitHub release notes of those releases carry only the install boilerplate. Issue numbers come from the
commit subjects and from the merge subjects (`Merge pull request #N from crazy-goat/issue/<issue>-...`); an entry
without one is a change whose commits and merge named none. The tags v0.5.0, v0.5.1 and v0.11.0 have no GitHub
release and no entry of their own: they are folded into the next entry (v0.5.2 and v0.11.1), which says so.

## [Unreleased]

## [0.14.0] - 2026-10-03

### Changed

- Docs: new `docs/guides/getting-started.md` (install the `.deb`, a combined gateway + FastCGI + cron + supervisor configuration, `-t`, start, verify), migration guides `docs/guides/migrate-php-fpm-nginx.md`, `docs/guides/migrate-nginx-unit.md` and `docs/guides/migrate-supervisord-cron.md` (each with a directive map and the list of what has no equivalent), and `docs/guides/framework-recipes.md` (Symfony and Laravel on the classic executors, tried by hand once, not in CI). New `build/test-doc-configs.sh` (new CI job `doc-configs`, which `ci-ok` needs; `docs/guides/` counts as code for the docs-only skip) runs `php-fpm-ng -t` on every `ini verify` block of those pages and starts the getting-started configuration. `README.md` no longer says "POC", drops the php-src branch status and the "Plan" section that said the SAPI would eventually exist, calls `http-direct` with the classic executor supported as the tier table does, and links the new pages; `docs/http-direct.md` loses the same "POC, not a production frontend" claim (#645).
- `fpmng-cron-jitter` and `fpmng-supervisor-jitter` take `FPMNG_DEBUG_CLOCK_RATE=10`: their assertions read the pool's master-measured `last_start` (and `next_run`) from the operator status page instead of a `gmdate()`/`microtime()` value the job script wrote, so a scaled clock can speed them up. They still fail on a libc `rand()` state shared across `fork()`. Test and `docs/fpmng-phpt.md` only; no operator surface added (#398).
- `tests/frameworks/` is now a Slim 4 smoke test for what `main` ships: the stock `public/index.php` answers through `pool.type = gateway` in front of `fastcgi` and through `http-direct` with `pool.executor = classic`, plain and with PHP-DI plus the Slim route cache, and CI runs it in the new `frameworks` job (`ci-ok` needs it). It needs no MySQL, Redis or Docker. The fiber-only Symfony, Laravel and Slim 4 probes, `run-all.sh` and the Laravel lock moved to `async/tests/frameworks/` on branch `async`; this also ends the failing Dependabot updates for `/tests/frameworks/laravel` (#602).
- Docs: `README.md` and `docs/frameworks.md` no longer describe framework support on `pool.executor = fiber`, and `docs/FASTCGI_NG_OPTIMIZATION.md` is gone. The fiber framework measurements and the `fastcgi-ng` plan live on branch `async` (`async/docs/frameworks-fiber.md`, `async/docs/FASTCGI_NG_OPTIMIZATION.md`); `main` describes only the executors it ships. History documents carry a one-line note (#601).
- `CHANGELOG.md`: the entries v0.1.0 to v0.11.1 are filled in from `git log <previous tag>..<tag>` instead of placeholders; the v0.11.1 entry was checked against its commits; the tags v0.5.0, v0.5.1 and v0.11.0 are folded into the next entry, and the compare links match (#586).
- `examples/` and `docker/` follow the v0.12 build contract: the Tier 1 and Tier 2 example images (`combined`, `cron`, `http`, `status`, `supervisor`) and the new `docker/Dockerfile` are `ubuntu:26.04` and install the released `.deb` (checksum-verified), instead of `ubuntu:24.04` plus a binary built from php-src. `examples/status` is an `operator.*` example on a real pool (it still used the removed `pool.type = status`), `docker/Dockerfile.scratch` (it copied the retired `php-fpm-ng-full`) is replaced, `examples/README.md` no longer describes a php-src build, and `examples/http-direct-worker-mysql/Dockerfile` installs `libevent-openssl-2.1-7t64`, without which the TLS-enabled binary does not start in it. New `build/test-shipped-configs.sh`: `static` (in the `checks` job) greps `examples/` and `docker/` for retired names, `images` (new `examples` job) builds every shipped configuration's image and runs `php-fpm-ng -t` inside it (#639).
- `third_party/php-src/MANIFEST` has three columns (path, upstream path, SHA-256) instead of four: the two hashes were always equal. `build/vendor-php-src.sh` and `build/audit-compile-deps.sh` read the new layout; the "not pristine" refusal is gone because one hash now says both (#676).
- `LICENSE` and `README.md` no longer list `patches/` under the PHP License 3.01: it holds only project-written history notes (MIT). `build/phpt-fixture-patches/` stays listed (#676).
- `patches/0002-upstream-report.md` is English throughout; `docs/NOTES.md` marks the patch 0002/0003 passages as historic and a `build/libphp-build.sh` comment no longer names the removed patches/0006 (#676).
- Owned `.phpt` suite: every test port (the Tester's blocks from 9008 and the http-direct tests from 28054) now also moves by a per-run `FPMNG_PHPT_PORT_SHIFT` that `build/run-fpmng-phpt.sh` picks as the first shift whose blocks nothing listens on, so another php-fpm holding 9201 or 9208 on a shared host no longer fails dozens of tests. Set the variable to pin a shift. The Tester also gives each gateway pool a run-own `operator.*_listen`, so a master holding `127.0.0.1:9253` no longer fails the gateway tests (#674).
- `bin/lint.sh` runs shellcheck at its default (style) severity instead of `warning`. The 22 info/style findings in `build/` and `tests/frameworks/symfony/run.sh` are fixed or silenced with a reasoned `# shellcheck disable` (#584).
- `tests/frameworks/laravel/composer.lock` pins `league/commonmark` 2.10.3 (was 2.10.0), closing Dependabot alerts 2 and 3. Test fixture only, not shipped in the packages (#695).

### Fixed

- `fpm.conf` diagnostics after an `include=` name the real line of the including file: the line counter is put back when the included file has been read, so an error on a later line, and the "Unable to include" message, no longer carry a number from the end of the included file (#545).
- The gateway listeners (the main one and `http.plain_listen`) pause accepting for 100 ms when `accept()` fails with `EMFILE`/`ENFILE` (or another non-retriable error) instead of spinning on one core until a descriptor is freed; the failure is logged at most once every 10 s (#687).
- `third_party/php-src/README.md` states the right file counts (207: 57 for the build, 150 test fixtures), and `build/vendor-php-src.sh check` now fails when the Provenance table disagrees with `MANIFEST` (#665).
- `ci-image.yml` passes the ghcr.io login token and actor through `env:` instead of expanding
  `${{ }}` inside the `run:` script, matching `release.yml` (#583).
- Gateway: the upstream response (FastCGI or an `http.route[]` HTTP target) was appended to the client's output buffer as fast as the worker produced it, so a client that stopped reading kept the whole response in the memory of the gateway process that serves every connection of the pool. Now, once more than `http.response_buffer` bytes (new, default 1 MiB, `0` = unlimited as before) of a client's output are unwritten, the gateway stops reading that request's upstream and the worker blocks in its own write; it reads again when the client has drained the buffer, when the request ends or when the client is gone. A client that never reads is still closed by `http.write_timeout`. `http.response_buffer` is refused on `http-direct`. Behaviour change to know about: the worker's blocked time now counts against `http.stream_write_timeout` (streaming `http-direct` targets, total budget, default 10 s) and `request_terminate_timeout` (FastCGI), so a client slower than that gets a cut response unless those limits are raised or `http.response_buffer = 0` is set; and a client that trickles bytes (never cut by `http.write_timeout`, a stall timer) now holds a worker for as long as it trickles (#596).

## [0.13.0] - 2026-10-03

### Changed
- `release.yml`: the four package-gate cells (`deb`, `apk`, `deb-tls`, `apk-tls`) run as a parallel matrix (`fail-fast: false`) and each uploads its output as an artifact; a `release` job that needs all of them collects the assets and publishes on a tag only. The release notes are checked in a separate first job. A rehearsal now takes about the slowest cell instead of the sum of the four (13.5 min measured on 2026-10-03 instead of about 49); asset names and publish-on-tag-only behaviour are unchanged (#575).
- `pool.type = fastcgi`: TCP_NODELAY is now set on the listening socket by the SAPI (`.listening_socket_nodelay`) and so applies to every accepted TCP connection, not only FCGI_KEEP_CONN ones. It replaces php-src patch `0002` (`main/fastcgi.c`), which is removed; `main` now carries only patch `0001`. No latency change against 0.12.0, which already carried the patch (#590).
- Docs: document that the default operator listener `127.0.0.1:9253` is global to the host: a second master with gateway (or any operator-page) pools on the default fails to bind, and how to avoid it with `operator.*_listen` or `operator.status|metrics = off` (#561).

### Removed
- The php-src patch machinery, now that `main` carries no patch (#592): the patch-stack step of `build/prepare.sh` and of `build/vendor-php-src.sh` (and the `php-<minor>/` variant selection), the `patches` line of `third_party/php-src/MANIFEST`, and `build/test-prepare-patch-stack.sh`. `build/vendor-php-src.sh check` now also refuses a vendored file that differs from its pristine upstream hash. `patches/` keeps only history notes; patches live on branch `async`. A PHP upgrade on `main` is a plain re-import.
- php-src patch `0001` (GH-18956, idle/active counting on FastCGI keep-alive connections) and its `php-8.3/` variant; `main` now carries no php-src patch, and `third_party/php-src/` is re-imported unpatched. Known effect, documented in `docs/operator-endpoint.md`, `docs/gateway.md` and `patches/README.md`; it does not affect http-direct pools: on keep-alive connections `accepted conn` and the per-process `requests` count one extra request per kept connection the client closes, a worker waiting on a kept connection shows as `Reading headers` with the idle wait in `request duration`, `max active processes` stays too high, `idle`/`active` can be wrong for up to one heartbeat, and `request_terminate_timeout` can hit an idle worker only with `http.idle_timeout = 0`, a large value or `fastcgi_keep_conn on` behind a proxy; pm scaling is not affected. The owned `fpm_request.c` again offers the `void` `fpm_request_accepting()` / `fpm_request_reading_headers()` that upstream's `fpm_main.c` passes to `fcgi_init_request()`, plus `_ex(bool)` variants for http-direct (#591).
- The dead optimized-transport patches `0003` (buffered read, `accept4`), `0004` (`fcgi_set_optimized_transport()`) and `0005` (`writev`), the `HAVE_ACCEPT4` probe and its binary assert; `third_party/php-src/` is re-imported at php-8.5.9. `main` carries only patches 0001 and 0002 (#589; 0002 was removed in #590, 0001 in #591).

### Fixed
- Package gate (`build/ci-package-gate.sh`): stopped pinning `EXPECT_TOTAL`/`EXPECT_PASS`/`EXPECT_SKIP` per flavour, which went stale on every new `.phpt` (#445, #441). The total is now the number of owned tests, and the only non-PASS results are the named lines of the new `build/package-gate-expected.txt` (cells, `SKIP`/`XFAIL`, test, reason). A new test that passes needs no gate edit; an unlisted skip fails as `unexpected SKIP: <name> on <cell>`, and a listed skip that passes fails as stale. The scoring is `build/package-gate-compare.sh`, tested hermetically by `build/test-package-gate-expected.sh` (#678).
- Selective reload (`reload.selective = yes`): a pool spared across the reload kept serving, but its application metrics series vanished and its status page showed 0 processes, because the new master allocated fresh anonymous shared memory the spared workers never wrote to. The scoreboards and the metrics region are now `memfd`-backed and inherited across the master's `execvp()` for spared pools (Linux); a spared pool's counters continue and the status page keeps counting its workers. `fpmng-reload-selective-metrics.phpt` no longer carries `--XFAIL--` (#537).
- Gateway: a client connection was time-limited only until its first request was read. Now `http.read_timeout` also bounds every later request on a keep-alive connection (from its first byte), the new `http.keepalive_timeout` (default 60000 ms, 0 = unlimited) closes an idle keep-alive connection, and the new `http.write_timeout` (default 30000 ms, 0 = unlimited) closes a client that stalls a pending response write. `http.plain_listen` now has a first-request read deadline and the same keep-alive limit. `http.max_connections` and `http.max_connections_per_client` are refused on a gateway (they were silently ignored); both new directives are refused on `http-direct`. Docs corrected: `http.read_timeout` is not libevent's `evhttp_set_timeout_tv()` and `http.idle_timeout` is not a client timeout (#593).
- The `.deb` and `.apk` packages now ship the license texts: one file with the MIT text (and the list of non-MIT files), the PHP License 3.01 text and the BSD-2-Clause FPM text, as `/usr/share/doc/<pkg>/copyright` on Debian and `/usr/share/licenses/<pkg>/LICENSE` on Alpine. It is assembled by the new `build/package-licenses.sh`, and the package gate fails if the installed file differs from it or lacks one of the three texts (#581).
- `pool.type = gateway`: an absolute-form request-target (`GET http://h/path HTTP/1.1`, RFC 9112 3.2.2) is normalized once: `ping.path`, the operator namespace and `http.operator_allowed_clients`, `access.suppress_path[]` and the plain-HTTP redirect now see the origin-form path (before, the request bypassed those checks; the matchers now use the same libevent parse as routing, so `http:/path` is covered too; a target starting with `/`, including `//x`, stays the origin-form path), and the authority replaces the `Host` header / `HTTP_HOST` on both transports; the forwarded target (`REQUEST_URI`, the request line to an `http.route[]` target, the redirect `Location`) comes from the same parse, so the path a request is matched on is the path the application sees; an authority longer than 261 bytes is answered 400 (#534).
- Test `fpmng-http-direct-respond.phpt`: the marker files written after `fpmng_respond()` are now polled for (5 s bound) instead of read once, so a loaded machine can no longer make the test read a marker the script has not written yet (#544).
- Test `fpmng-http-direct-worker-saturation-refuses-new.phpt`: the test now synchronizes explicitly (marker file when the first hold reaches the worker, the idle-connection request written only after the second hold got 503) and the worker script keeps the event loop running after the stop request until the test releases it (10 s bound), instead of relying on scheduler timing (#527).
- `pool.type = gateway` and `pool.type = http-direct`: a request header whose name contains `_` is no longer passed to the worker. `X_Real_IP` and `X-Real-IP` both became `HTTP_X_REAL_IP` and the last one won, so a client could override a header set by the reverse proxy in front (nginx drops such headers by default, `underscores_in_headers off`, and Apache 2.4 drops them too). The header is now dropped; there is no opt-in option (#595).
- `pool.type = gateway`: an upstream FastCGI `Status:` that is not three digits in 200..599 (`abc`, `-5`, `99999`, a 1xx) is no longer sent to the client as the status line. The gateway answers `502 Bad Gateway`, logs a WARNING and drops the rest of the upstream reply. The same applies to an `http.route[]` HTTP target whose status line is outside 200..599 (#594).
- `pool.type = gateway`: when an upstream (FastCGI or `http.route[]` HTTP target) fails after the response head was sent, the gateway now closes the client connection without the terminating chunk and logs a WARNING, so a truncated body is observable. It used to end the reply with `0\r\n\r\n`, which a client or a cache stored as a complete response. A close-delimited upstream body still ends normally (#533).
- The operator `?full` page no longer prints a `live` row with `pid` 0 for a slot whose child is not forked yet; a test (or an operator script) that took that 0 as an address ran `kill -USR1 0`, which signals the whole process group and killed the `fpmng-phpt` runner with exit 138 (#567).
- The test harness refuses to signal a pid below 2 (`build/phpt-fixture-patches/0004`), so such a mistake fails one test instead of the run (#567).

## [0.12.0] - 2026-10-02

### Added
- `third_party/php-src/`: the bounded php-src subset the SAPI compiles (pinned to php-8.5.9), with a `MANIFEST` and `build/vendor-php-src.sh check` in CI (#421, #536).
- `fpm.conf` can be parsed from memory, diagnostics name the configuration input, and the php.ini bootstrap policy is pinned by tests (#428).
- The fpmng `.phpt` suite runs in parallel with `run-tests.php -j` (#394).
- A selective-reload metrics test, landed as an expected failure for #537 (#384).
- `bin/lint.sh` runs clang-tidy, clang-format, shellcheck and hadolint; CI runs it as the `lint` job (#571).
- `CHANGELOG.md`, `AGENTS.md`, `docs/release-workflow.md`, `.github/dependabot.yml` and the organization's `bin/` worktree scripts.
- CI: `changes`, `docs` and `ci-ok` jobs; the build workflow now also runs on push to `main`.
- A registration stub `async-sync.yml` on `main`, so the weekly `async-sync-trigger` dispatch resolves (#528).

### Changed
- Main, CI and the packages build with `build/libphp-build.sh` against a distribution PHP 8.5 SDK, without a php-src tree or `build/prepare.sh` (#418, #422, #424). Other PHP minor versions are refused up front.
- The FPM regression tests run from a bounded fixture bundle and the packaged PHP, without a php-src checkout (#423).
- The package gate refuses a build/test PHP patch-level skew up front; the apk SDK is upgraded (#557). Its expected counts are re-measured (#555); it now runs 179 owned tests.
- `examples/http-direct-worker-mysql` runs on `ubuntu:26.04` with `libphp8.5-embed` (#564).
- License: own code is MIT (Crazy Goat Software); code taken from or patching php-src stays under the PHP License 3.01.
- The development process lives in `docs/workflow.md`; project specifics moved to `AGENTS.md`.
- Releases are published with `gh release create --verify-tag` and use the matching section of this file as notes.
- Own C sources are formatted with clang-format 23 (whitespace only); the remaining files are listed in `build/clang-format-exclude.txt`.

### Removed
- The static musl build and its CI job, the `patches` CI job, `build/static-full.sh`, `build/ci-build-tree.sh` and the `php-fpm-ng-ci` image (#424).
- `build/gh-release-create.sh`, replaced by `gh release create` in `release.yml`.
- The local issue templates, in favour of the organization's.
- The vendored upstream `browscap.ini`, replaced by a minimal self-written fixture (#559).

### Fixed
- `http.route[]` HTTP transport: hostile upstream framing is rejected with a log line and requests are forwarded in origin-form (#462); the true errno is logged for an oversize head, and a half head plus EOF is logged (#463); bytes after a completed response are logged and end the connection (#464); a forwarded header block over the target's bound is refused (#466).
- Gateway: an unreachable route target answers 502 with the connect error instead of a pool-full 503 (#465).
- http-direct on a UNIX listener no longer exits on the first connection on musl, and its failure-matrix test runs without ext/posix (#467).
- Worker: a closed upgraded stream releases its connection, so fds no longer accumulate (#472).
- `fpm_payload_dist.c` compiles against the php-src PHP-8.6/master stream error API (#435).
- libphp build: no SIGSEGV with `opcache.enable=0` on a libphp with OPcache compiled in (#558).
- phpt: a port collision now fails the test instead of passing as WARN on retry (#562).

## [0.11.1] - 2026-09-30

Also covers the tag v0.11.0 (tagged 2026-09-29, on the 2026-09-25 merge of #525; no GitHub release): v0.11.0 is that merge and v0.11.1 adds one commit on top, so both are listed here.

### Added
- `pool.type = fastcgi` can join the shared operator listener on an explicit `pm.metrics_listen` / `pm.status_listen`, so a FastCGI pool exposes its own per-pool metrics (#383).
- `pool.type = supervisor`: the operator page reports the heartbeat age per child (#356).
- Decision records and spike results: the approved PHAR runtime contract (#427), the embedded-PHAR runtime spike (#426) and the verdict on the event API (#190).
- Documentation: the bound for a worker that never checks `fpmng_worker_stopping()` (`docs/shutdown-timeouts.md`, `docs/http-direct.md`), with a test (#365); the gateway and operator endpoint guides brought up to date (#385); a README mention of the http-direct pool executors (#447).

### Fixed
- Worker WebSocket: the RFC 6455 handshake now rejects non-conformant clients (#457), both close paths send a TLS close_notify (#458), the 101 head write is checked (#468), and a userland throw after `fpmng_worker_upgrade()` no longer leaves a zero-byte close (#461).
- Worker: a backpressured live stream no longer blocks `fpmng_worker_may_exit()` forever, so retiring a worker no longer ends in the master's SIGKILL that lost the terminating chunk (#459).
- `http.route[]`: a cleartext route to a non-loopback http-direct target is refused (#450).
- Gateway: connection counters no longer scan the client list on every request and disconnect (#490).
- `cron.expect_within`: staleness is checked without an operator scrape (#357), and the duplicate stale-episode warning is gone (#358).
- `supervisor.max_memory` at or below the master's baseline RSS is warned about (#350); `cron.log` failure lines carry a `[pool %s]` prefix (#355).
- Documentation and comment fixes: #446, #448; example SSE clients retry under backpressure (#454, #455).

### Changed
- The cron stale and `cron.log` tests run at real speed and the package gate counts were refreshed (#526).

## [0.10.0] - 2026-09-22

### Added
- `pool.type = gateway`: the HTTP front of the pool, replacing `pool.type = http`. It has its own counters in one shared-memory segment rendered by the operator child, counts the plain listener, and `http.operator` forwards the operator pages of exposed pools (#388, #389, #390).
- `pool.executor = worker` answers `ping.path` after the saturation check (#387).

### Changed
- The operator directives moved out of the `pm.` namespace into `operator.*` (#386).
- `pool.type = http` is retired in favour of `pool.type = gateway` (#388).
- The reduced pool type/executor surface was audited against the tier bar (#380) and the persistent-signal build capability was dropped (#420).
- The gateway refuses `listen.allowed_clients` instead of ignoring it (#493); the docs state that gateway metrics `off` is explicit, not unset (#491).

### Fixed
- Worker WebSocket: the read watcher no longer spins after EOF and `has_buffered()` no longer doubles as an EOF signal (#460, #456).
- Supervisor: the one-shot restart decision is made per copy instead of pool-wide, and one-shot completion is derived from the per-slot bytes (#347, #492).
- Tests and CI: the in-pool ACME tests skip when the `-n` FPM pool has no openssl (#500); `reload-selective-on` writes the error log and asserts the sparing NOTICE (#405); the Laravel runner never runs its negative controls against shared services (#51); the package gate EXPECT rows are measured (#497).

## [0.9.0] - 2026-09-19

### Fixed
- `http.route[]`: a 1xx interim response from the target no longer completes the exchange (#451); `X-Forwarded-For` appends the direct peer, not the resolved client (#452); response header names are forwarded up to the maximum length (#453).
- Worker WebSocket: a dead client's pending entry is reaped in the close callback (#444), the orphaned flag guards every stream operation (#443), and the hijacked bufferevent is disarmed before the idle-close shutdown (#442).
- Test-harness helpers (`expectNoLogPattern`, `readHead`) and the package gate counts (136 tests, #445).

## [0.8.0] - 2026-09-19

### Added
- `http.route[]` routes path prefixes to other pools on one gateway (#340), with per-target accounting (#341) and an HTTP/1.1 client transport so a route can target `http-direct` pools (#344).
- Worker: native WebSocket, `fpmng_worker_upgrade()` hijacks the connection into a `php_stream` (#343); SSE semantics, a clean end on retire and closed clients reported by id (#342).

### Changed
- The fiber and async executors were cut out of `main` onto the branch `async` (#373, #374).

### Removed
- `pool.type = fastcgi-ng` and its documentation and packaging text (#376, #377, #378).

### Fixed
- The gateway answers `ping.path` locally instead of forwarding it upstream (#382).
- `examples/combined` uses per-pool operator endpoints instead of the removed `pool.type = status` (#278).
- clang-analyzer findings are fixed and now fail CI (#414); tests stopped waiting out fixed budgets (#399).

## [0.7.0] - 2026-09-18

### Added
- `pool.executor = worker`: configurable `max_pending` and `request_timeout` (#331), streaming responses with backpressure (`fpmng_worker_respond_start/chunk/end`, #332), `worker.max_memory` and `worker.max_lifetime` recycling (#334), worker metrics (#333), `fpm_connection_info()` and `fpm_send_early_hints()` by request id (#335), honest request metrics with pending and watcher gauges, an accept ceiling so one child cannot hoard the backlog (#338), and per-slot metrics for capacity, fairness and stall diagnosis (#339).
- `--enable-fpmng-debug-clock`, a virtual clock so the test suite stops waiting on the real one (#396). It is not part of the packages.

### Changed
- CI: the package gate left the pull-request path (17 jobs to 8) and the nightly image rebuild is the daily signal for distribution drift (#393).
- Design documents for `pool.type = gateway`, the `operator.*` namespace and the default operator port 9253 (#386, #387).

### Fixed
- The operator page derives ages on the same clock that wrote the stamps (#396).
- The test runner fails a full run that passes fewer than half its tests, and reports per-test durations (#393, #396).
- Missing `.phpt` coverage for `pool.executor = worker` (#336).

## [0.6.0] - 2026-09-17

### Added
- `cron.jitter` / `cron.jitter_mode` spread scheduled pools out (#322); `supervisor.restart_jitter` and `supervisor.start_jitter` break restart-storm lockstep (#323).
- `supervisor.max_memory` recycles a script on memory and `supervisor.stop_signal` is configurable (#324); `cron.stop_signal` for a graceful kill on master shutdown or reload (#325).
- `supervisor.max_runtime`, a runtime cap per script iteration (#326).
- Cron and supervisor expose staleness so a stuck or silently dead job is visible (#327); `cron.output_log` / `supervisor.output_log` give a dedicated per-pool output log (#328).
- Supervisor rolling restart across a reload (#329).

### Changed
- A reload restarts only the pools whose configuration changed (#330).

### Fixed
- Cron jitter PRNG state across fork; a bind race and an idle-timeout risk in the new cron and supervisor tests.

## [0.5.2] - 2026-09-14

Also covers the tags v0.5.0 and v0.5.1 (both 2026-09-14, no GitHub release): v0.5.1 and v0.5.2 are one commit each on top of v0.5.0, so all three are listed here.

### Added
- `pool.type = http-direct`: `fpm_connection_info()` and client-certificate verification (#62); early hints, status codes and method passthrough (#63); `.user.ini` is read from the front controller's directory (#60).
- Gateway: an opt-in `wait` pool-full policy (#309).
- Documentation: recommendations for the gateway pool-full policy (#160) and for the http-direct dynamic and ondemand process managers (#170).

### Fixed
- `pm.max_requests` rollover on http-direct drains held connections instead of severing them (#313); a scaled-down child gets its `http.read_timeout` before SIGKILL (#310) and a retiring child's deadline is extended on every completed response (#311).
- Test and CI: the TLS `.deb` and `.apk` are gated on every PR; the connection-info test's SKIPIF gap and supervisor poll were fixed (#320, #321).
- Benchmark harnesses for the pool-full and pm studies were extended (#154, #157, #158, #169).

## [0.4.0] - 2026-09-13

### Added
- TLS termination is behind `--enable-fpmng-tls`, off by default (#280), and ACME certificate issuance is a build flag that takes its payload with it (#281). A second package, `php-fpm-ng-tls`, is built and gated on a tag (#294).
- Tiers: `.tier` on the pool type, one line per pool at startup and a table in the README (#295).
- `--enable-fpmng-http2` and `--enable-fpmng-quic` are reserved and refused (#282).

### Changed
- Breaking for users of in-pool TLS and ACME: the default `php-fpm-ng` package and build no longer contain TLS termination (`http.tls_cert` and friends) or ACME certificate issuance, because the build flags default to off. This is a regression against v0.2.0 for anyone terminating TLS in the pool; install the new `php-fpm-ng-tls` package for them (#280, #281, #294).

### Fixed
- http-direct: `SA_RESTART` is restored after every request startup (#259) and the child's own log lines reach the master's error log (#260).
- The three ACME log-leak test assertions were passing on an empty string (#297); the package gate counts a test retried into a pass as a pass (#301).

## [0.3.0] - 2026-09-13

### Added
- Operator endpoint: one HTTP listener per address serving each pool's own stats and metrics (#274); http-direct serves `pm.status_path` from it (#275); a per-pool metrics path reports its own pool's series (#276) and each pool type has a baseline counter (#277).
- Supervisor warns once when a script restarts at PHP-startup speed (#122).
- Pool types: a reject list can carve out individual directives (#283).

### Removed
- `pool.type = status` and the shared status pool; use the operator endpoint (#278).

### Changed
- `pool.type = http`: `pm.status_path` is taken off the public listener (the operator listener serves it), and a shared operator listener with two identities is refused (#274).
- CI moved off the self-hosted box onto `ubuntu-latest`, with caches written only by `main`; the modular-build spike was documented.

## [0.2.0] - 2026-09-12

### Added
- http-direct: static files from the pool's own root (#58), operator parity (ping, status, access log, `allowed_clients`, chroot; #59), a bound on the first request and on connections per worker (#61), per-connection metrics and per-child rows on `pm.status` (#64), retiring one child with SIGUSR1 (#65), `fpmng_respond()` to finish a response early (#57), and TLS streaming with the pool's own `SSL_write` (#195).
- The ACME client is carried inside the binary (#171); the metrics extension is registered on the libphp path and tested from PHP (#216).
- A pm-sizing benchmark harness for http-direct (#163) and installation docs for the packages (#225).

### Fixed
- http-direct: one child no longer scoops a whole burst (#53), Nagle is off on the listening socket (#244), and the signal handlers are re-snapshotted after the child installs its own (#256).

## [0.1.0] - 2026-09-11

### Added
- First release: `.deb` and `.apk` packages with `SHA256SUMS`, unsigned by decision (#223). The tagged history up to this release is the project's initial development; there is no earlier tag to compare with, so no further items are listed.

[Unreleased]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.14.0...HEAD
[0.14.0]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.13.0...v0.14.0
[0.13.0]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.12.0...v0.13.0
[0.12.0]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.11.1...v0.12.0
[0.11.1]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.10.0...v0.11.1
[0.10.0]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.9.0...v0.10.0
[0.9.0]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.8.0...v0.9.0
[0.8.0]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.5.2...v0.6.0
[0.5.2]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.4.0...v0.5.2
[0.4.0]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/crazy-goat/php-fpm-ng/releases/tag/v0.1.0
