--TEST--
fpm-ng: http.pool_full_policy = wait requires both bounds set (issue #309)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #309. Unbounded queueing is the slowloris/hoarding hazard the 503
 * exists to avoid in the first place, so a pool that turns the wait policy on
 * must not be allowed to leave either bound unset (unset means "0" here,
 * since these are plain integer directives with no directive-was-set
 * fallback -- see docs/http-gateway-pool-full.md). fpm_http_validate_pool()
 * refuses this at startup rather than silently falling back to reject or to
 * an unbounded queue/wait. */

function expectConfigFailure(string $label, string $cfg, string $needle): void
{
    $tester = new FPM\Tester($cfg, '<?php echo "ok";');
    $messages = $tester->testConfig(true);
    if ($messages === null) {
        echo "FAIL: $label unexpectedly passed validation\n";
        exit(1);
    }
    $text = implode("\n", $messages);
    if (!str_contains($text, $needle)) {
        echo "FAIL: $label missing needle: $needle\ngot:\n$text\n";
        exit(1);
    }
    echo "$label: rejected\n";
}

function expectConfigOk(string $label, string $cfg): void
{
    $tester = new FPM\Tester($cfg, '<?php echo "ok";');
    $messages = $tester->testConfig(true);
    if ($messages !== null) {
        echo "FAIL: $label unexpectedly failed validation:\n" . implode("\n", $messages) . "\n";
        exit(1);
    }
    echo "$label: accepted\n";
}

/* The cases below append http.pool_full_* directives to this text, and an
 * appended directive lands in the LAST section -- so [gw] is written last
 * (issue #388: those directives belong on the gateway, not on the fastcgi
 * target). */
$base = <<<EOT
[global]
error_log = {{FILE:LOG}}
[pool]
pool.type = fastcgi
listen = {{ADDR[fastcgi]}}
pm = static
pm.max_children = 1
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
http.route[pool] = /
EOT;

expectConfigFailure(
    'wait-with-zero-queue-max',
    $base . "\nhttp.pool_full_policy = wait\nhttp.pool_full_queue_max = 0\nhttp.pool_full_wait_ms = 500",
    'http.pool_full_policy = wait requires http.pool_full_queue_max > 0'
);

expectConfigFailure(
    'wait-with-zero-wait-ms',
    $base . "\nhttp.pool_full_policy = wait\nhttp.pool_full_queue_max = 32\nhttp.pool_full_wait_ms = 0",
    'http.pool_full_policy = wait requires http.pool_full_wait_ms > 0'
);

expectConfigFailure(
    'wait-with-negative-wait-ms',
    $base . "\nhttp.pool_full_policy = wait\nhttp.pool_full_queue_max = 32\nhttp.pool_full_wait_ms = -1",
    'http.pool_full_policy = wait requires http.pool_full_wait_ms > 0'
);

expectConfigFailure(
    'invalid-policy-name',
    $base . "\nhttp.pool_full_policy = queue",
    'invalid http.pool_full_policy'
);

// The default (policy not set at all) is still reject, with no bounds
// required -- the two integer directives keep their shipped defaults
// (32 / 500ms, see docs/http-gateway-pool-full.md) but reject never reads
// them, so a pool that never mentions the wait policy is unaffected.
expectConfigOk('reject-default-no-bounds-needed', $base);

// A pool that explicitly asks for wait with both bounds set is accepted.
expectConfigOk(
    'wait-with-both-bounds',
    $base . "\nhttp.pool_full_policy = wait\nhttp.pool_full_queue_max = 32\nhttp.pool_full_wait_ms = 500"
);

?>
--EXPECT--
wait-with-zero-queue-max: rejected
wait-with-zero-wait-ms: rejected
wait-with-negative-wait-ms: rejected
invalid-policy-name: rejected
reject-default-no-bounds-needed: accepted
wait-with-both-bounds: accepted
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
