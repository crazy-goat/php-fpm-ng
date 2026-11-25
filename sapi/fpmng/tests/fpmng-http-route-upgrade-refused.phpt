--TEST--
fpm-ng: http.route_upgrade[] is refused at startup unless the target is a worker-executor http-direct pool, and a zero timeout is refused (issue #659)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #659: a WebSocket upgrade is only passed on to a target that answers
 * it itself. A fastcgi target and a classic http-direct target do not, so the
 * configuration is a startup refusal, not a 502 per request. */

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
        echo "FAIL: $label missing needle: $needle\n";
        echo "got:\n$text\n";
        exit(1);
    }
    echo "$label: rejected\n";
}

function expectConfigAccepted(string $label, string $cfg): void
{
    $tester = new FPM\Tester($cfg, '<?php echo "ok";');
    $messages = $tester->testConfig(true);
    if ($messages !== null) {
        echo "FAIL: $label unexpectedly failed validation\n";
        echo implode("\n", $messages) . "\n";
        exit(1);
    }
    echo "$label: accepted\n";
}

/* A worker target needs a real script in its chdir (the worker validation). */
$root = sys_get_temp_dir() . '/fpmng-route-upgrade-refused-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/worker.php", '<?php fpmng_worker_loop(true);');

function gateway(string $target, string $extra = '', string $upgrade = 'http.route_upgrade[/ws] = websocket'): string
{
    global $root;

    return <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
http.gateways = 1
http.route[ws] = /ws,/
$upgrade
$extra

[ws]
listen = $target
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
php_admin_value[max_execution_time] = 0
EOT;
}

expectConfigAccepted('worker-target', gateway('{{ADDR[ws]}}'));

expectConfigFailure('fastcgi-target', <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
http.gateways = 1
http.route[ws] = /ws
http.route_upgrade[/ws] = websocket

[ws]
listen = {{ADDR[ws]}}
pm = static
pm.max_children = 1
EOT, "pool 'ws' does not serve WebSocket upgrades");

expectConfigFailure('classic-http-direct-target', <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
http.gateways = 1
http.route[ws] = /ws
http.route_upgrade[/ws] = websocket

[ws]
listen = {{ADDR[ws]}}
pool.type = http-direct
pm = static
pm.max_children = 1
http.front_controller = /index.php
EOT, "pool 'ws' does not serve WebSocket upgrades");

expectConfigFailure('prefix-without-route', gateway('{{ADDR[ws]}}', '', 'http.route_upgrade[/chat] = websocket'),
    'no http.route[] entry has this path prefix');

expectConfigFailure('zero-idle-timeout', gateway('{{ADDR[ws]}}', 'http.upgrade_idle_timeout = 0'),
    'http.upgrade_idle_timeout must be greater than zero');

expectConfigFailure('zero-max-duration', gateway('{{ADDR[ws]}}', 'http.upgrade_max_duration = 0'),
    'http.upgrade_max_duration must be greater than zero');

@unlink("$root/worker.php");
@rmdir($root);
?>
--EXPECT--
worker-target: accepted
fastcgi-target: rejected
classic-http-direct-target: rejected
prefix-without-route: rejected
zero-idle-timeout: rejected
zero-max-duration: rejected
