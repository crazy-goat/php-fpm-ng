--TEST--
fpm-ng: a SIGUSR2 reload that moves the gateway to another address does not take the old listener over (issue #661)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #661: a listener is handed over only to a gateway that binds the same
 * address with the same TLS setup. Here the gateway moves to another address in
 * the reload. The old socket must not be kept: the new master closes it before
 * the new gateway binds, and the old address is free afterwards. */

$root = sys_get_temp_dir() . '/fpmng-gw-listener-rebind-' . getmypid();
@mkdir($root, 0700, true);
$log = "$root/error.log";
file_put_contents("$root/app.php", "<?php\nheader('Content-Length: 2');\necho 'ok';\n");

function rebind_config(string $name, string $log, string $root): string
{
    return "[global]\n"
        . "error_log = $log\n"
        . "pid = {{FILE:PID}}\n"
        . "log_level = notice\n"
        . "process_control_timeout = 10\n"
        . "[gw]\n"
        . "pool.type = gateway\n"
        . "listen = {{ADDR[" . $name . "]}}\n"
        . "chdir = $root\n"
        . "http.gateways = 1\n"
        . "http.route[app] = /\n"
        . "[app]\n"
        . "pool.type = fastcgi\n"
        . "listen = {{ADDR}}\n"
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

$tester = new FPM\Tester(rebind_config('http', $log, $root), '<?php');
try {
    /* Without forceStderr the error_log of the config is kept, and the test reads it. */
    $tester->start([], false);
    $old = $tester->getAddr('ipv4', '[http]');
    wait_answer($old, $log);

    $tester->reload(rebind_config('moved', $log, $root));
    $new = $tester->getAddr('ipv4', '[moved]');
    if ($new === $old) {
        throw new RuntimeException('the reload did not move the gateway');
    }
    wait_answer($new, $log);
    echo "the gateway serves the new address: ok\n";

    $probe = @stream_socket_server("tcp://$old", $errno, $error);
    if (!$probe) {
        throw new RuntimeException("the old address $old is still held: $error");
    }
    fclose($probe);
    echo "the old address is released: ok\n";

    if (!str_contains((string) @file_get_contents($log), "closing the listener on $old")) {
        throw new RuntimeException("the old listener was not closed\n" . (string) @file_get_contents($log));
    }
    echo "old listener closed, not taken over: ok\n";

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
the old address is released: ok
old listener closed, not taken over: ok
Done
