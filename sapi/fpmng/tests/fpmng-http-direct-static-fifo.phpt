--TEST--
fpm-ng: direct HTTP static request for a FIFO in the docroot does not block the event loop (#1008)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
$base = sys_get_temp_dir() . '/fpmng-direct-static-fifo-' . getmypid();
@mkdir($base);
file_put_contents($base . '/app.css', 'body{color:red}');
/* The front controller answers every path PHP is asked for. A FIFO that the
 * static module hands over shows up here as "php:/pipe.css"; a FIFO that
 * blocks the event loop never shows up at all. */
file_put_contents($base . '/front.php', '<?php echo "php:" . $_SERVER["REQUEST_URI"];');
exec('mkfifo ' . escapeshellarg($base . '/pipe.css'), $out, $status);
if ($status !== 0) {
    throw new RuntimeException("mkfifo failed with status $status");
}
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
pool.type = http-direct
listen = {{ADDR}}
chdir = $base
pm = static
pm.max_children = 1
http.front_controller = /front.php
http.static = yes
EOT;

/* The timeout is short on purpose: with the event loop blocked, the first
 * request times out here, and the test fails in seconds, not minutes. */
function httpGet(string $url, int $timeout): string|false
{
    $ctx = stream_context_create(['http' => ['timeout' => $timeout, 'ignore_errors' => true]]);
    return @file_get_contents($url, false, $ctx);
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
    $public = $tester->getListen('{{ADDR}}');

    /* Not a regular file: the static module leaves it to PHP, as it does for
     * any other non-regular file. The answer must arrive within the timeout. */
    expect('fifo', httpGet("http://$public/pipe.css", 2), 'php:/pipe.css');
    echo "fifo answered: ok\n";

    /* The event loop is still running: a regular file answers too. */
    expect('regular file', httpGet("http://$public/app.css", 2), 'body{color:red}');
    echo "regular file after fifo: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($base . '/pipe.css');
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
