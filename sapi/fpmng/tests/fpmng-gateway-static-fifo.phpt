--TEST--
fpm-ng: gateway static request for a FIFO in the docroot does not block the event loop (#1008)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php
require_once "tester.inc";
$base = sys_get_temp_dir() . '/fpmng-static-fifo-' . getmypid();
@mkdir($base);
file_put_contents($base . '/app.css', 'body{color:red}');
/* The front controller answers every path PHP is asked for. It must never see
 * the FIFO: the gateway hands the FIFO to the FastCGI worker, which refuses a
 * script that does not end in .php (fpm_main.c, "Access denied."), and a FIFO
 * that blocks the gateway never answers at all. */
file_put_contents($base . '/front.php', '<?php echo "php:" . $_SERVER["REQUEST_URI"];');
exec('mkfifo ' . escapeshellarg($base . '/pipe.txt'), $out, $status);
if ($status !== 0) {
    throw new RuntimeException("mkfifo failed with status $status");
}
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $base
http.gateways = 1
http.static = 1
http.front_controller = /front.php
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $base
pm = static
pm.max_children = 1
EOT;

/* The timeout is short on purpose: with the event loop blocked, the first
 * request times out here, and the test fails in seconds, not minutes. Returns
 * the status and the body; a timeout gives status 0 and body false. */
function httpGet(string $url, int $timeout): array
{
    $ctx = stream_context_create(['http' => ['timeout' => $timeout, 'ignore_errors' => true]]);
    $body = @file_get_contents($url, false, $ctx);
    $status = preg_match('#^HTTP/1\.[01] (\d{3}) #', $http_response_header[0] ?? '', $m) ? (int) $m[1] : 0;
    return [$status, $body];
}

function expect(string $what, $actual, $expected): void
{
    if ($actual !== $expected) {
        throw new RuntimeException("$what: expected " . var_export($expected, true) .
            ', got ' . var_export($actual, true));
    }
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');

    /* Not a regular file: the static module leaves it to the worker, and the worker
     * refuses it with 403. The answer must arrive within the timeout. */
    expect('fifo', httpGet("http://$http/pipe.txt", 2), [403, "Access denied.\n"]);
    echo "fifo answered: ok\n";

    /* The gateway is still serving: a regular file answers too. */
    expect('regular file', httpGet("http://$http/app.css", 2), [200, 'body{color:red}']);
    echo "regular file after fifo: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($base . '/pipe.txt');
    @unlink($base . '/app.css');
    @unlink($base . '/front.php');
    @rmdir($base);
}
?>
--EXPECT--
fifo answered: ok
regular file after fifo: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
