# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The history below is rebuilt from the GitHub releases. Most of them carry only the install
boilerplate; those entries say so, and the few that could be checked against the tagged commits are summarized. The tags
v0.5.0, v0.5.1 and v0.11.0 have no GitHub release.

## [Unreleased]

### Added
- `bin/lint.sh` runs clang-tidy, clang-format, shellcheck and hadolint; CI runs it as the `lint` job (#571).
- `CHANGELOG.md`, `AGENTS.md`, `docs/release-workflow.md`, `.github/dependabot.yml` and the organization's `bin/` worktree scripts.
- CI: `changes`, `docs` and `ci-ok` jobs; the build workflow now also runs on push to `main`.
- A registration stub `async-sync.yml` on `main`, so the weekly `async-sync-trigger` dispatch resolves (#528).

### Changed
- License: own code is MIT (Crazy Goat Software); code taken from or patching php-src stays under the PHP License 3.01.
- The development process lives in `docs/workflow.md`; project specifics moved to `AGENTS.md`.
- Releases are published with `gh release create --verify-tag` and use the matching section of this file as notes.
- Own C sources are formatted with clang-format 23 (whitespace only); the remaining files are listed in `build/clang-format-exclude.txt`.

### Removed
- `build/gh-release-create.sh`, replaced by `gh release create` in `release.yml`.
- The local issue templates, in favour of the organization's.

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

[Unreleased]: https://github.com/crazy-goat/php-fpm-ng/compare/v0.11.1...HEAD
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
