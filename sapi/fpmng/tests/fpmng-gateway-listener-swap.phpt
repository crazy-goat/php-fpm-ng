--TEST--
fpm-ng: a SIGUSR2 reload that gives a gateway's old address to a fastcgi pool does not fail the new generation (issue #661)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #661, the address swap. The old generation keeps the gateway's listening
 * socket open across the reload's execvp(). In the new configuration that address
 * belongs to a fastcgi pool, and the gateway moves to another address. The new
 * master must close the inherited socket before the pools bind; otherwise the
 * fastcgi pool fails with "Address already in use", the new master exits, and
 * the service is down. The fastcgi pool must then listen on the old address, and
 * the gateway must answer on the new one. */

$root = sys_get_temp_dir() . '/fpmng-gw-listener-swap-' . getmypid();
@mkdir($root, 0700, true);
$log = "$root/error.log";
file_put_contents("$root/app.php", "<?php\nheader('Content-Length: 2');\necho 'ok';\n");

function swap_config(string $gatewayAddress, string $fastcgiAddress, string $log, string $root): string
{
    return "[global]\n"
        . "error_log = $log\n"
        . "pid = {{FILE:PID}}\n"
        . "log_level = notice\n"
        . "process_control_timeout = 10\n"
        . "[gw]\n"
        . "pool.type = gateway\n"
        . "listen = $gatewayAddress\n"
        . "chdir = $root\n"
        . "http.gateways = 1\n"
        . "http.route[app] = /\n"
        . "[app]\n"
        . "pool.type = fastcgi\n"
        . "listen = $fastcgiAddress\n"
        . "chdir = $root\n"
        . "pm = static\n"
        . "pm.max_children = 2\n";
}

function gateway_ok(string $addr): bool
{
    $fp = @stream_socket_client("tcp://$addr", $errno, $error, 2);
    if (!$fp) {
        return false;
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET /app.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $body = (string) stream_get_contents($fp);
    fclose($fp);
    return str_starts_with($body, 'HTTP/1.1 200') && str_ends_with($body, 'ok');
}

function wait_answer(string $addr, string $log): void
{
    $deadline = microtime(true) + 20;
    while (!gateway_ok($addr)) {
        if (microtime(true) > $deadline) {
            throw new RuntimeException("$addr never answered\n" . (string) @file_get_contents($log));
        }
        usleep(100000);
    }
}

$tester = new FPM\Tester(swap_config('{{ADDR[http]}}', '{{ADDR[app]}}', $log, $root), '<?php');
try {
    /* Without forceStderr the error_log of the config is kept, and the test reads it. */
    $tester->start([], false);
    $old = $tester->getAddr('ipv4', '[http]');
    wait_answer($old, $log);

    $tester->reload(swap_config('{{ADDR[moved]}}', $old, $log, $root));
    $moved = $tester->getAddr('ipv4', '[moved]');
    wait_answer($moved, $log);
    echo "the gateway serves the new address: ok\n";

    $logText = (string) @file_get_contents($log);
    if (str_contains($logText, 'FPM initialization failed') || str_contains($logText, 'Address already in use')) {
        throw new RuntimeException("the reload failed to bind\n" . $logText);
    }
    echo "the reload did not fail to bind: ok\n";

    if (!str_contains($logText, "closing the listener on $old of the previous generation")) {
        throw new RuntimeException("the old gateway listener was not closed\n" . $logText);
    }
    echo "old gateway listener closed before the fastcgi pool bound: ok\n";

    $conn = @stream_socket_client("tcp://$old", $errno, $error, 2);
    if (!$conn) {
        throw new RuntimeException("nothing listens on the old address $old: $error");
    }
    fclose($conn);
    echo "fastcgi pool listens on the old gateway address: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    foreach (glob("$root/*") ?: [] as $f) {
        @unlink($f);
    }
    @rmdir($root);
}
?>
--EXPECT--
the gateway serves the new address: ok
the reload did not fail to bind: ok
old gateway listener closed before the fastcgi pool bound: ok
fastcgi pool listens on the old gateway address: ok
Done
