# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The history below is rebuilt from the GitHub releases. Most of them carry only the install
boilerplate; those entries say so, and the few that could be checked against the tagged commits are summarized. The tags
v0.5.0, v0.5.1 and v0.11.0 have no GitHub release.

## [Unreleased]

### Removed
- The php-src patch machinery, now that `main` carries no patch (#592): the patch-stack step of `build/prepare.sh` and of `build/vendor-php-src.sh` (and the `php-<minor>/` variant selection), the `patches` line of `third_party/php-src/MANIFEST`, and `build/test-prepare-patch-stack.sh`. `build/vendor-php-src.sh check` now also refuses a vendored file that differs from its pristine upstream hash. `patches/` keeps only history notes; patches live on branch `async`. A PHP upgrade on `main` is a plain re-import.
- php-src patch `0001` (GH-18956, idle/active counting on FastCGI keep-alive connections) and its `php-8.3/` variant; `main` now carries no php-src patch, and `third_party/php-src/` is re-imported unpatched. Known effect, documented in `docs/operator-endpoint.md`, `docs/gateway.md` and `patches/README.md`; it does not affect http-direct pools: on keep-alive connections `accepted conn` and the per-process `requests` count one extra request per kept connection the client closes, a worker waiting on a kept connection shows as `Reading headers` with the idle wait in `request duration`, `max active processes` stays too high, `idle`/`active` can be wrong for up to one heartbeat, and `request_terminate_timeout` can hit an idle worker only with `http.idle_timeout = 0`, a large value or `fastcgi_keep_conn on` behind a proxy; pm scaling is not affected. The owned `fpm_request.c` again offers the `void` `fpm_request_accepting()` / `fpm_request_reading_headers()` that upstream's `fpm_main.c` passes to `fcgi_init_request()`, plus `_ex(bool)` variants for http-direct (#591).
- The dead optimized-transport patches `0003` (buffered read, `accept4`), `0004` (`fcgi_set_optimized_transport()`) and `0005` (`writev`), the `HAVE_ACCEPT4` probe and its binary assert; `third_party/php-src/` is re-imported at php-8.5.9. `main` carries only patches 0001 and 0002 (#589; 0002 was removed in #590, 0001 in #591).

### Changed
- `pool.type = fastcgi`: TCP_NODELAY is now set on the listening socket by the SAPI (`.listening_socket_nodelay`) and so applies to every accepted TCP connection, not only FCGI_KEEP_CONN ones. It replaces php-src patch `0002` (`main/fastcgi.c`), which is removed; `main` now carries only patch `0001`. No latency change against 0.12.0, which already carried the patch (#590).

### Fixed
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

### Added
- `pool.type = fastcgi` can join the shared operator listener on an explicit `pm.metrics_listen` / `pm.status_listen` (#383).
- Decision records: the packed-app contract (#427), the v0.12 build support contract (#419) and an embedded-PHAR feasibility spike (#426).

### Changed
- Verified the v0.9.0 docs, handshake and hardening findings and decided the open contracts (#440).

### Fixed
- Worker WebSocket: the RFC 6455 handshake now rejects non-conformant clients (#457), both close paths send a TLS close_notify (#458), the 101 head write is checked (#468), and a userland throw after `fpmng_worker_upgrade()` no longer leaves a zero-byte close (#461).
- Worker: a backpressured live stream no longer blocks `fpmng_worker_may_exit()` forever (#459); a handler that never checks `fpmng_worker_stopping()` can no longer block graceful shutdown indefinitely (#365).
- `http.route[]`: a non-loopback http-direct target is no longer accepted in cleartext (#450).
- Gateway: connection counters no longer scan the client list on every request (#490).
- `cron.expect_within`: staleness no longer depends on a scrape (#357), and the duplicate stale-episode warning is gone (#358).
- `supervisor.max_memory` below the baseline RSS is now warned about or rejected (#350); `cron.log` failure lines carry a `[pool %s]` prefix (#355).
- Documentation and comment fixes: #446, #447, #448, #385.

## [0.10.0] - 2026-09-22

### Changed
- Release; the GitHub release notes contain only the install boilerplate, no itemized changes.

## [0.9.0] - 2026-09-19

### Changed
- Release; the GitHub release notes contain only the install boilerplate, no itemized changes.

## [0.8.0] - 2026-09-19

### Added
- `http.route[]` routes path prefixes to other pools on one gateway, with per-target accounting (#341).

### Fixed
- The gateway answers `ping.path` locally instead of forwarding it upstream.
- `examples/combined` uses per-pool operator endpoints instead of the removed `pool.type = status`.

## [0.7.0] - 2026-09-18

### Added
- `--enable-fpmng-debug-clock`, a virtual clock so the test suite stops waiting on the real one (#396). It is not part of the packages.

### Fixed
- The operator page derives ages on the same clock that wrote the stamps (#396).

## [0.6.0] - 2026-09-17

### Changed
- Release; the GitHub release notes contain only the install boilerplate, no itemized changes.

## [0.5.2] - 2026-09-14

### Changed
- Release; the GitHub release notes contain only the install boilerplate, no itemized changes.

## [0.4.0] - 2026-09-13

### Changed
- Release; the GitHub release notes contain only the install boilerplate, no itemized changes.

## [0.3.0] - 2026-09-13

### Changed
- Release; the GitHub release notes contain only the install boilerplate, no itemized changes.

## [0.2.0] - 2026-09-12

### Fixed
- Repaired a corrupted comment splice in `fpm_cron_schedule_next()`, restored the `http.*` configuration fields lost in a rebase, and routed `TEST_PHP_EXECUTABLE` through the test harness directory.

## [0.1.0] - 2026-09-11

### Added
- First release: `.deb` and `.apk` packages with `SHA256SUMS`, unsigned by decision (#223).

[Unreleased]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.12.0...HEAD
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
