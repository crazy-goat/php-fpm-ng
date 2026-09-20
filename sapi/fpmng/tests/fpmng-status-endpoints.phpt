--TEST--
fpm-ng: the status pool is gone, its two pages are per-pool paths (issue #278)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* This test used to start a pool.type = status and read /status and /metrics
 * off its listener. That pool aggregated every pool in the master from a
 * listener of its own, which is the one thing the operator endpoint (#274)
 * also does, so #278 removed it. The two pages did not go with it: the same
 * two formats are now configured per pool, with operator.status_path and
 * operator.metrics_path (the names since issue #386; this test predates them).
 *
 * So this asserts what replaced it and, before that, that the old spelling
 * fails loudly. A config that used to start and now silently reports nothing
 * is the failure mode worth a test of its own. */

function expectRejected(string $label, string $cfg, array $needles): void
{
    $tester = new FPM\Tester($cfg, '<?php');
    $messages = $tester->testConfig(true);
    if ($messages === null) {
        echo "FAIL: $label was accepted\n";
        exit(1);
    }
    $text = implode("\n", $messages);
    foreach ($needles as $needle) {
        if (!str_contains($text, $needle)) {
            echo "FAIL: $label missing needle: $needle\ngot:\n$text\n";
            exit(1);
        }
    }
    echo "$label: rejected\n";
}

/* A retired type name is not a typo, and the message is the whole difference:
 * whoever reads it is holding a config file that used to start. */
expectRejected('pool.type = status', <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}

[monitor]
listen = {{ADDR}}
pool.type = status
EOT, [
    "pool.type 'status' no longer exists",
    "set 'operator.status_path' and 'operator.metrics_path' on the pool you want to watch",
]);

/* fastcgi has a web server in front of it, which is where a path is
 * restricted, so it gets no operator listener and the address directives have
 * nothing to name. The old spelling is refused by name with its replacement
 * (issue #386); the operator.* spelling is refused by the type (see
 * fpmng-operator-endpoint-config.phpt). */
expectRejected('pm.status_listen renamed', <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}

[www]
listen = {{ADDR}}
pm = static
pm.max_children = 1
pm.status_path = /status
pm.status_listen = {{ADDR[operator]}}
EOT, [
    "'pm.status_listen' was renamed to 'operator.status_listen' (issue #386)",
]);

/* And the replacement, on a type whose whole state comes through
 * fpm_pool_type_s.status() rather than a scoreboard -- so the JSON is the
 * generic page this file used to read off the aggregate pool, for one pool. */
$root = sys_get_temp_dir() . '/fpmng-status-endpoints-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents($root . '/loop.php', '<?php sleep(30);');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}

[monitored]
pool.type = supervisor
supervisor.script = $root/loop.php
supervisor.processes = 1
operator.status_listen = {{ADDR}}
operator.status_path = /status
operator.metrics_listen = {{ADDR}}
operator.metrics_path = /metrics
EOT;

$tester = new FPM\Tester($cfg, '<?php');
$tester->start();
$tester->expectLogStartNotices();
$operator = $tester->getListen('{{ADDR}}');

[$status, $headers, $body] = fpmng_operator_fetch($operator, '/status');
if ($status !== 200 || !str_contains($headers['content-type'] ?? '', 'application/json')) {
    echo "FAIL: /status answered $status " . var_export($headers, true) . "\n";
    exit(1);
}
$pools = json_decode($body, true)['pools'] ?? null;
/* One pool, not every pool: that is what #278 changed. The array it is wrapped
 * in is the shape the aggregate page had, kept so that a client written
 * against it still parses. */
if (!is_array($pools) || count($pools) !== 1 || $pools[0]['name'] !== 'monitored'
    || $pools[0]['type'] !== 'supervisor') {
    echo "FAIL: /status body: $body\n";
    exit(1);
}
echo "/status: ok\n";

$metrics = fpmng_operator_body($operator, '/metrics');
foreach (['# HELP fpmng_pool_info', 'fpmng_pool_info{pool="monitored",type="supervisor"} 1'] as $needle) {
    if (!str_contains($metrics, $needle)) {
        echo "FAIL: /metrics missing: $needle\ngot:\n$metrics\n";
        exit(1);
    }
}
echo "/metrics: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink($root . '/loop.php');
@rmdir($root);

?>
Done
--EXPECT--
pool.type = status: rejected
pm.status_listen renamed: rejected
/status: ok
/metrics: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
