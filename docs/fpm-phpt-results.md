# Upstream FPM PHPT result

**Result: NOT MEASURED.** No upstream test process was started from this retry
worktree. The inventory below records the 150-test upstream reference set and
marks every row `NOT MEASURED`; it is not a fabricated pass, failure or skip
count.

| Category | Count |
|---|---:|
| PASS | 0 |
| FAIL/ERROR | 0 |
| SKIP | 0 |
| NOT MEASURED | 150 |
| **TOTAL** | **150** |

## Exact blocker

The retry worktree contains neither a prepared PHP source tree nor a built
`php-fpm-ng` binary. The inspected 150-test reference checkout is PHP source
commit `d452441142446a1e2b83a58c556e7e8fd7f96b80`, which already contains the
GH-18956 change targeted by repository patch `0001-gh18956-fastcgi-keepalive-counting.patch`.
It is therefore not an eligible input to `build/prepare.sh`: the patch stack
cannot be applied to it. The validated pre-GH-18956 source pin
`de5a5820efb` enumerates 141 tests, not the 150-test inventory required here.
No binary from the retry branch was available to fingerprint or measure.

The committed runner records the exact source commit, binary paths, `-v`
output, SHA-256 hashes and distinctive `strings` markers when a compatible
source and binary are supplied. It emits `NOT MEASURED` rows if a prerequisite
or status is missing. See [`fpm-phpt.md`](fpm-phpt.md).

## Per-test inventory

The paths are the locations after `build/prepare.sh` copies upstream
`sapi/fpm/tests` to `sapi/fpmng/tests`.

| Test | Result |
|---|---|
| `sapi/fpmng/tests/bug64539-status-json-encoding.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug68207-fastcgi-error-header-sent.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug68381-log-level-warning.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug68391-conf-include-order.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug68420-ipv4-all-addresses.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug68421-ipv6-access-log.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug68423-multi-pool-all-pms.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug68428-ipv6-allowed-clients.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug68442-signal-reload.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug68458-pm-no-start-server.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug68591-conf-test-group.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug68591-conf-test-listen-group.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug68591-conf-test-listen-owner.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug68591-conf-test-user.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug69625-no-script-filename.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug72185-fcgi-empty-frame.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug72573-http-proxy.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug73342-nonblocking-stdio.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug74083-concurrent-reload.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug75212-php-value-in-user-ini.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug75712-getenv-server-vars_001.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug75712-getenv-server-vars_002.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug76601-reload-child-signals.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug76922-fcgi-get-value-conn.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug77023-pm-dynamic-blocking-sigquit.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug77106-fcgi-missing-nl.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug77780-header-sent-error.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug77934-reload-process-control.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug78323.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug78599-path-info-underflow.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug80024-socket-reduced-inherit.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug80669-uid-user-groups.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/bug80849-fpm.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/config-array-validation-php-value-key.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/config-array-validation-suppress-path-key-2.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/config-array-validation-suppress-path-key.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/config-array-validation-suppress-path-starts-slash.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/config-array.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/dynamic-keep-alive-spare.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fastcgi_finish_request_basic.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-nopif-apache-handler-with-pi-with-pt-pd.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-nopif-custom-with-pi-with-pt-pd.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-nopif-custom-with-pi-with-pt.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-nopif-custom-with-pi-without-pt.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-nopif-custom-without-pi-with-pt.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-nopif-custom-without-pi-without-pt.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-nopif-custom-without-sf-with-pt.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-pif-apache-balancer-legacy.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-pif-apache-balancer-real.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-pif-apache-handler-uds.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-pif-apache-handler-with-pi.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-pif-apache-handler-with-query.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-pif-apache-handler-without-docroot.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-pif-apache-pp-sfp-decoding.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-pif-apache-pp-sfp-encoded.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-pif-apache-pp-sn-strip-basic.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-pif-apache-pp-sn-strip-encoded-plus.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-pif-apache-pp-sn-strip-encoded.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fcgi-env-pif-apache-pp-sn-strip-invalid.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/fpm_get_status_basic.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/getallheaders.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh-11086-daemonized-logs-duplicated.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh12232-php-value-extension.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh12385.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh12621.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh13563-conf-bool-env.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh14212-status-accept-timeout.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh14212-status-empty-connections.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh14212-status-keep-alive.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh15395-php-auth-shutdown.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh16432-status-high-nprocs.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh16628.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh16932-scoreboard-reset.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh19989-access-log-fcgi-stderr.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh23122-status-requests-keep-alive.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh8157-user-ini-post.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh8646.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh8885-stderr-fd-reload-usr1.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh8885-stderr-fd-reload-usr2.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh9754-daemonized-stderr-close.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh9921-php-value-ext-mod-handlers.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/gh9981-fastcgi-error-header-reset.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/ghsa-54hq-v5wp-fqgv-max-body-parts-custom.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/ghsa-54hq-v5wp-fqgv-max-body-parts-default.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/ghsa-54hq-v5wp-fqgv-max-file-uploads.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/ghsa-7qg2-v9fj-4mwv-status-xss.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/http-basic.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-access-extended-limit.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bm-in-shutdown-fn.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bm-limit-1024-msg-80.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bm-limit-2048-msg-4000.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bwd-limit-1050-msg-2048.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bwd-limit-1050-msg-2900.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bwd-limit-64-too-low-error.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bwd-limit-8000-msg-4096.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bwd-msg-with-nl.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bwd-multiple-msgs-stdout-stderr.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bwd-multiple-msgs.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bwp-limit-1024-msg-120.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bwp-limit-1500-msg-3300.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bwp-msg-flush-split-fallback.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bwp-msg-flush-split-real.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bwp-msg-flush-split-sep-pos-end.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bwp-msg-flush-split-sep-pos-start.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-bwp-realloc-buffer.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-dwd-limit-1050-msg-2048.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-dwd-limit-1050-msg-2900.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-dwd-limit-8000-msg-4096.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-dwp-limit-1000-msg-2000.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-invalid-port.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-suppress-output-request-body.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/log-suppress-output.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/main-global-prefix.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/main-version.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/ondemand-keep-alive-accept.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/opcache_enable_admin_value.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/php-admin-doc-root.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/php_admin_value-failure.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/pm-max-requests-keep-alive.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/pm-max-spawn-rate-config.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/pm-max-spawn-rate-run.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/pool-apparmor-basic.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/pool-prefix.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/proc-idle-timeout.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/proc-no-start-server.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/proc-user-ignored.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/proc-user-not-set-when-root.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/reload-uses-sigkill-as-last-measure.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/request-terminate-timeout-keep-alive.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/request_parse_body_multipart.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/request_parse_body_urlencoded.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/setsofib.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/socket-close-on-exec.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/socket-invalid-allowed-clients.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/socket-ipv4-allowed-clients.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/socket-ipv4-basic.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/socket-ipv4-fallback.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/socket-ipv6-any.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/socket-ipv6-basic.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/socket-uds-acl.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/socket-uds-basic.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/socket-uds-numeric-ugid-nonroot.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/socket-uds-numeric-ugid.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/socket-uds-too-long-filename-start.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/socket-uds-too-long-filename-test.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/status-basic.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/status-listen-expose-php-off.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/status-listen-expose-php-on.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/status-listen.phpt` | NOT MEASURED |
| `sapi/fpmng/tests/status-ping.phpt` | NOT MEASURED |
