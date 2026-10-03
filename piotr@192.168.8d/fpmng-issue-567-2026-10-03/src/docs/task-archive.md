# Task archive

Until 2026-09-08 this project tracked work as one Markdown file per task under
`tasks/`. That tracker was replaced by GitHub Issues; the directory was deleted in
the commit that added this file. Nothing was rewritten in history, so every task
file is still readable from git.

This document exists for one reason: older commit messages, code comments,
documentation and issue bodies refer to work by its **task number** (`task 040`,
`tasks/done/034-scheduler-define-or-drop.md`). This is the index that turns such
a number back into something you can read.

## Reading a task file that no longer exists

```sh
# the last commit that still had the tracker
git show 7a663f8:tasks/done/040-tls-certificate-reload-without-restart.md

# or list everything that was there
git ls-tree -r --name-only 7a663f8 tasks/
```

The full history of a single file, including the commit that finished it:

```sh
git log --follow -p -- tasks/done/040-tls-certificate-reload-without-restart.md
```

## Open tasks — migrated to issues

These files were not deleted silently: each became a GitHub issue with the same
content. The task number is no longer used for new work.

| Task | Issue | Title |
| ---- | ----- | ----- |
| 007 | [#78](https://github.com/crazy-goat/php-fpm-ng/issues/78) | Decide and implement the include model for shared includes |
| 009 | [#79](https://github.com/crazy-goat/php-fpm-ng/issues/79) | Module globals set through `on_modify` are still shared between requests |
| 016 | [#80](https://github.com/crazy-goat/php-fpm-ng/issues/80) | Symlink deploys are not detected by `fiber.revalidate_freq` |
| 020 | [#46](https://github.com/crazy-goat/php-fpm-ng/issues/46) | ACME: obtain and renew TLS certificates (umbrella) |
| 021 | [#81](https://github.com/crazy-goat/php-fpm-ng/issues/81) | `curl` blocks the fiber executor |
| 022 | [#82](https://github.com/crazy-goat/php-fpm-ng/issues/82) | Process-wide functions still reachable from a request on the fiber executor |
| 026 | [#83](https://github.com/crazy-goat/php-fpm-ng/issues/83) | Slim 4 on the fiber executor: unknown, and the cheapest one to find out |
| 028 | [#84](https://github.com/crazy-goat/php-fpm-ng/issues/84) | Two known gaps in the fiber sleep interception |
| 030 | [#85](https://github.com/crazy-goat/php-fpm-ng/issues/85) | Session RINIT runs before the request's superglobals exist |
| 045 | [#47](https://github.com/crazy-goat/php-fpm-ng/issues/47) | ACME: exactly one process renews, and the others pick the result up |
| 046 | [#48](https://github.com/crazy-goat/php-fpm-ng/issues/48) | ACME: serve the HTTP-01 challenge from the local-answer hook |
| 047 | [#49](https://github.com/crazy-goat/php-fpm-ng/issues/49) | ACME: issuance and renewal end to end, staging by default |
| 050 | [#50](https://github.com/crazy-goat/php-fpm-ng/issues/50) | Transient Redis reply misattribution under the configured isolation list |
| 051 | [#51](https://github.com/crazy-goat/php-fpm-ng/issues/51) | Negative-control Laravel pools corrupt the protocol of the shared MySQL |
| 052 | [#52](https://github.com/crazy-goat/php-fpm-ng/issues/52) | Measure the per-request cost of a non-empty `fiber.isolate_statics` list |
| 053 | [#86](https://github.com/crazy-goat/php-fpm-ng/issues/86) | CI `phpt` job fails on `packages.microsoft.com` 403 (infra flake) |
| 055 | [#53](https://github.com/crazy-goat/php-fpm-ng/issues/53) | Measure and improve HTTP-direct connection fairness |
| 056 | [#54](https://github.com/crazy-goat/php-fpm-ng/issues/54) | Spike: pool-full policy for the HTTP gateway — measure before choosing |
| 057 | [#55](https://github.com/crazy-goat/php-fpm-ng/issues/55) | TLS for HTTP-direct pools |
| 058 | [#56](https://github.com/crazy-goat/php-fpm-ng/issues/56) | Streaming responses for HTTP-direct |
| 059 | [#57](https://github.com/crazy-goat/php-fpm-ng/issues/57) | `fpm_respond()`: send the response and keep computing |
| 060 | [#58](https://github.com/crazy-goat/php-fpm-ng/issues/58) | Static file serving for HTTP-direct pools |
| 061 | [#59](https://github.com/crazy-goat/php-fpm-ng/issues/59) | Operator parity for HTTP-direct: ping/status, access log, allowed clients, chroot |
| 062 | [#60](https://github.com/crazy-goat/php-fpm-ng/issues/60) | `.user.ini` per-directory INI for HTTP-direct pools |
| 063 | [#61](https://github.com/crazy-goat/php-fpm-ng/issues/61) | Connection limits and slowloris hardening for HTTP-direct |
| 064 | [#62](https://github.com/crazy-goat/php-fpm-ng/issues/62) | `fpm_connection_info()` and client certificate exposure in PHP |
| 065 | [#63](https://github.com/crazy-goat/php-fpm-ng/issues/63) | Full response control: early hints and custom methods in HTTP-direct |
| 066 | [#64](https://github.com/crazy-goat/php-fpm-ng/issues/64) | Per-connection metrics and scoreboard extension for HTTP-direct |
| 067 | [#65](https://github.com/crazy-goat/php-fpm-ng/issues/65) | Per-worker drain trigger for zero-downtime deploys |
| 068 | [#66](https://github.com/crazy-goat/php-fpm-ng/issues/66) | Spike: dynamic and ondemand process managers for HTTP-direct |
| 069 | [#67](https://github.com/crazy-goat/php-fpm-ng/issues/67) | Spike: singleflight coalescing and worker-side response cache |
| 070 | [#68](https://github.com/crazy-goat/php-fpm-ng/issues/68) | Spike: long-lived connections (SSE, long-polling, WebSocket) and `fpm_push()` |
| 071 | [#69](https://github.com/crazy-goat/php-fpm-ng/issues/69) | Spike: HTTP/2 (and the road to QUIC) for HTTP-direct |
| 072 | [#70](https://github.com/crazy-goat/php-fpm-ng/issues/70) | Spike: SAPI event API and a shared scheduler extension (ext/fpm) |
| 077 | [#71](https://github.com/crazy-goat/php-fpm-ng/issues/71) | TLS hot reload misses a certificate replaced in the same second |
| 078 | [#72](https://github.com/crazy-goat/php-fpm-ng/issues/72) | `fpmng-supervisor-restart.phpt` intermittently reports `restarts=0` |
| 081 | [#73](https://github.com/crazy-goat/php-fpm-ng/issues/73) | The worker executor has no way to log |
| 082 | [#74](https://github.com/crazy-goat/php-fpm-ng/issues/74) | Extract the shared HTTP-direct request/path helpers |
| 083 | [#75](https://github.com/crazy-goat/php-fpm-ng/issues/75) | Decide whether the amphp harness is gated in CI or declared optional |
| 084 | [#76](https://github.com/crazy-goat/php-fpm-ng/issues/76) | `fpm_pool_type_resolve()` still name-compares for `fiber` and `async` |
| 085 | [#77](https://github.com/crazy-goat/php-fpm-ng/issues/77) | Assert `build/static-full.sh`'s extension set against the artefact |

## Completed tasks — history only

Finished before the move, each with an `Outcome` section recording what was
actually done and measured. They were **not** migrated to issues; read them from
git with the commands above.

| Task | Title | File |
| ---- | ----- | ---- |
| 001 | Run upstream FPM's `.phpt` suite against `php-fpm-ng` | `tasks/done/001-restore-fpm-phpt-tests.md` |
| 002 | CI: a build matrix over the combinations that break silently | `tasks/done/002-ci-build-matrix.md` |
| 003 | Our own `.phpt` tests for pool types and executors | `tasks/done/003-own-phpt-pool-types-executors.md` |
| 004 | `build/static-full.sh` verifies the wrong SAPI | `tasks/done/004-static-build-verifies-wrong-sapi.md` |
| 005 | Non-blocking `ssl://` / `tls://` for the fiber executor | `tasks/done/005-fiber-tls-transport-interception.md` |
| 006 | Make `stream_select()` cooperate with the fiber executor | `tasks/done/006-fiber-stream-select.md` |
| 008 | Laravel: class statics leak between concurrent requests | `tasks/done/008-laravel-class-statics.md` |
| 010 | The HTTP gateway does not drop privileges, and now holds a TLS private key | `tasks/done/010-http-gateway-drop-privileges.md` |
| 011 | Enforce the English-only rule so the tree does not drift back | `tasks/done/011-comment-language-policy.md` |
| 012 | Translate documentation and our own comments to English | `tasks/done/012-translate-docs-and-comments-to-english.md` |
| 013 | C style rules and a linter, matching upstream rather than inventing | `tasks/done/013-c-style-and-lint.md` |
| 014 | Write down what a comment is for, instead of pruning comments | `tasks/done/014-comment-content-rule.md` |
| 015 | `http.listen` is effectively always required, contradicting its own error message | `tasks/done/015-http-listen-always-required.md` |
| 017 | `process_control_timeout` silently kills long-lived pools on shutdown | `tasks/done/017-process-control-timeout-guidance.md` |
| 018 | Two gaps left by the HTTP gateway's front-controller fallback | `tasks/done/018-front-controller-remaining-gaps.md` |
| 019 | `pool.executor = async` has the fiber executor's holes and none of its guards | `tasks/done/019-async-executor-parity.md` |
| 023 | Reload signalling: the comment does not cover the `status` pool, and the README's churn note is stale | `tasks/done/023-reload-signal-comment-and-readme-churn-note.md` |
| 024 | Symfony on the fiber executor: from "measured once" to "supported" | `tasks/done/024-framework-support-symfony.md` |
| 025 | Laravel on the fiber executor: the statics list is the open risk | `tasks/done/025-framework-support-laravel.md` |
| 027 | An automated harness for the framework tests | `tasks/done/027-framework-integration-test-harness.md` |
| 029 | Investigate intermittent GH-16432 FPM status failure | `tasks/done/029-fpm-phpt-gh16432-intermittent-failure.md` |
| 031 | Turn the "already in plan" half of the Node gap analysis into owned work | `tasks/done/031-node-gaps-already-in-plan-audit.md` |
| 032 | `pool.type = proxy`: decide what it means before anything is built | `tasks/done/032-proxy-decide-meaning.md` |
| 033 | Revisit three deliberate limits in `pool.type = cron` | `tasks/done/033-cron-revisit-three-deliberate-limits.md` |
| 034 | Define "scheduler" against cron and supervisor, or drop the word | `tasks/done/034-scheduler-define-or-drop.md` |
| 035 | Examples: there is nothing to copy from | `tasks/done/035-examples-nothing-to-copy-from.md` |
| 037 | `O_NONBLOCK` leaks onto the listening socket and survives a reload | `tasks/done/037-nonblock-leaks-onto-the-listening-socket-across-reload.md` |
| 038 | Patches 0004 and 0005 no longer apply to PHP 8.6.0-dev, blocking `prepare.sh` | `tasks/done/038-patches-0004-0005-no-longer-apply-to-php-8-6.md` |
| 039 | TLS: only the leaf certificate is sent, the chain is dropped | `tasks/done/039-tls-certificate-chain-not-sent.md` |
| 040 | TLS: replace the certificate without restarting the gateway | `tasks/done/040-tls-certificate-reload-without-restart.md` |
| 041 | TLS: ALPN and SNI — decide the scope before ACME needs them | `tasks/done/041-tls-alpn-and-sni.md` |
| 042 | A plain HTTP listener alongside the TLS one (redirect, and ACME HTTP-01) | `tasks/done/042-plain-http-listener-alongside-tls.md` |
| 043 | ACME: decide where the client lives, C in the gateway or PHP in a cron pool | `tasks/done/043-acme-decide-where-the-client-lives.md` |
| 044 | ACME: define what state lives where, and who owns it | `tasks/done/044-acme-state-on-a-writable-volume.md` |
| 048 | make the tls-reload CI job robust against a slow runner | `tasks/done/048-tls-reload-test-polling-deadline.md` |
| 054 | Direct HTTP classic/static proof of concept | `tasks/done/054-http-direct-classic-static-poc.md` |
| 073 | POC: worker-mode HTTP-direct with a userland Revolt driver | `tasks/done/073-http-direct-worker-revolt-poc.md` |
| 074 | Example: worker-mode HTTP-direct + amphp/mysql, runnable with Docker Compose | `tasks/done/074-http-direct-worker-mysql-compose-example.md` |
| 075 | Example: worker-mode HTTP-direct + ReactPHP, on the same primitives | `tasks/done/075-http-direct-worker-reactphp-example.md` |
| 076 | Worker-mode builtins crash opcache's optimizer | `tasks/done/076-worker-builtins-null-module-opcache-segv.md` |
| 079 | Worker event API: streams whose data is invisible to libevent | `tasks/done/079-worker-event-api-buffered-streams.md` |
| 080 | Worker bridges can drop a queued request while draining | `tasks/done/080-worker-bridge-drain-race.md` |

## Numbers that appear nowhere here

Some numbers were never a file of their own — they were assigned in a plan,
folded into another task, or dropped. If a number is missing from both tables,
check `git log --all --oneline --grep '<number>'` before assuming the work exists.
