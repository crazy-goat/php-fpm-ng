--TEST--
fpm-ng: a SIGUSR2 reload hands the gateway listener to the next generation, so no connection is refused (issue #661)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #661, the second phase of the graceful drain (#641 was the first).
 * Before it, fpm_http_cleanup() closed the gateway's listening socket before the
 * reload's execvp(), and the next master bound it again. A client that connected
 * in that gap got ECONNREFUSED. Now the old master keeps the listening socket
 * open across the exec and passes it on in FPMNG_HTTP_LISTENERS, and the new
 * generation takes it over. A connection made during the reload waits in the
 * kernel's accept queue until the new generation accepts it.
 *
 * A child process runs a client that connects in a loop, with Connection: close,
 * and counts every outcome. The master is reloaded while that client is still
 * running. The test expects no refused connection, no reset, and a log line that
 * proves the new generation took the listener over.
 *
 * A 502 is counted in 'other' and is not asserted here. It comes from a
 * persistent upstream connection that the app worker closed during the reload,
 * which the build without the handoff shows too. */

$root = sys_get_temp_dir() . '/fpmng-gw-listener-reload-' . getmypid();
@mkdir($root, 0700, true);
$log = "$root/error.log";
file_put_contents("$root/app.php", "<?php\nheader('Content-Length: 2');\necho 'ok';\n");
file_put_contents("$root/load.php", <<<'PHP'
<?php

[, $addr, $seconds, $out] = $argv;
$end = microtime(true) + (float) $seconds;
$counts = ['ok' => 0, 'refused' => 0, 'reset' => 0, 'other' => 0];
while (microtime(true) < $end) {
    $fp = @stream_socket_client("tcp://$addr", $errno, $error, 5);
    if ($fp === false) {
        $counts[$errno === 111 ? 'refused' : 'other']++; // 111 is ECONNREFUSED on Linux
        continue;
    }
    stream_set_timeout($fp, 10);
    fwrite($fp, "GET /app.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $body = (string) stream_get_contents($fp);
    $meta = stream_get_meta_data($fp);
    fclose($fp);
    if (str_starts_with($body, 'HTTP/1.1 200') && str_ends_with($body, 'ok')) {
        $counts['ok']++;
    } elseif ($body === '' && !$meta['timed_out']) {
        $counts['reset']++;
    } else {
        $counts['other']++;
    }
}
file_put_contents($out, json_encode($counts));
PHP);

$cfg = <<<EOT
[global]
error_log = $log
pid = {{FILE:PID}}
log_level = notice
process_control_timeout = 10
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 2
EOT;

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

$tester = new FPM\Tester($cfg, '<?php');
try {
    /* Without forceStderr the error_log of the config is kept, and the test reads it. */
    $tester->start([], false);
    $http = $tester->getAddr('ipv4', '[http]');

    $deadline = microtime(true) + 20;
    while (!gateway_ok($http)) {
        if (microtime(true) > $deadline) {
            throw new RuntimeException("the gateway never answered\n" . (string) @file_get_contents($log));
        }
        usleep(100000);
    }

    $result = "$root/counts.json";
    $load = proc_open([PHP_BINARY, "$root/load.php", $http, '8', $result],
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes);
    if (!is_resource($load)) {
        throw new RuntimeException('cannot start the load client');
    }
    usleep(1500000);
    $tester->signal('USR2');
    if (proc_close($load) !== 0) {
        throw new RuntimeException('the load client failed');
    }

    $counts = json_decode((string) @file_get_contents($result), true);
    if (!is_array($counts)) {
        throw new RuntimeException('the load client wrote no result');
    }
    if ($counts['refused'] !== 0 || $counts['reset'] !== 0 || $counts['ok'] === 0) {
        throw new RuntimeException('connections refused or reset across the reload: ' . json_encode($counts)
            . "\n" . (string) @file_get_contents($log));
    }
    echo "no refused or reset connection across the reload: ok\n";

    if (!str_contains((string) @file_get_contents($log), "took over the listener on $http")) {
        throw new RuntimeException("the new generation did not take the listener over\n" . (string) @file_get_contents($log));
    }
    echo "new generation took the listener over: ok\n";

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
no refused or reset connection across the reload: ok
new generation took the listener over: ok
Done
