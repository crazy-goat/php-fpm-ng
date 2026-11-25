--TEST--
fpm-ng: a worker that sleeps after X-Accel-Redirect and flush() does not crash the gateway when the file ends first (#649)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php
require_once "tester.inc";

/* The worker sends the redirect and flushes, then sleeps. The file is sent
 * and ends the reply during the sleep, while the upstream is still attached to
 * the request. When the worker wakes up and writes again, the upstream reads
 * into the freed request unless it was detached at the end of the file reply
 * (fpm_http_sendfile_end()). */
const ROUNDS = 3;
const SMALL = "hello sendfile\n";

$base = sys_get_temp_dir() . '/fpmng-sendfile-sleep-' . getmypid();
$root = $base . '/protected';
@mkdir($root, 0777, true);
file_put_contents($root . '/small.txt', SMALL);

file_put_contents($base . '/send.php', '<?php
header("X-Accel-Redirect: " . $_GET["r"]);
flush();
sleep(2);
echo "worker body";
');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $base
http.gateways = 1
http.front_controller = /send.php
http.sendfile_root = $root
http.route[web] = /
; The worker stays busy after the file ends, so a request waits for it
; instead of getting 503 from a full pool.
http.pool_full_policy = wait
http.pool_full_queue_max = 32
http.pool_full_wait_ms = 10000
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $base
pm = static
pm.max_children = 2
EOT;

/* The gateway process whose parent is $master, via the setproctitle line
 * fpm_http_gateway_run() installs. 0 when none exists. */
function gateway_pid(int $master): int
{
    foreach (glob('/proc/[0-9]*/stat') as $stat) {
        $raw = @file_get_contents($stat);
        if ($raw === false || !preg_match('/^(\d+) \((.*)\) \S (\d+) /s', $raw, $m)) {
            continue;
        }
        if ((int) $m[3] !== $master) {
            continue;
        }
        $cmd = @file_get_contents(dirname($stat) . '/cmdline');
        if ($cmd !== false && str_contains($cmd, 'http gateway gw')) {
            return (int) $m[1];
        }
    }
    return 0;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $master = $tester->getPid();
    $http = $tester->getAddr('ipv4', '[http]');

    $gw = 0;
    for ($i = 0; $i < 100 && $gw === 0; $i++) {
        $gw = gateway_pid($master);
        usleep(20000);
    }
    if ($gw === 0) {
        throw new RuntimeException('gateway process not found');
    }

    $ctx = stream_context_create(['http' => ['timeout' => 10, 'ignore_errors' => true]]);
    /* Every round runs, even after a bad answer: the crash that a bad answer
     * points to shows in the log only after the rounds, and the pid check
     * needs the rounds too. */
    $problems = [];
    for ($i = 0; $i < ROUNDS; $i++) {
        $body = @file_get_contents("http://$http/?r=%2Fsmall.txt", false, $ctx);
        $headers = $http_response_header ?? [];
        if (!str_starts_with($headers[0] ?? '', 'HTTP/1.1 200 ') || $body !== SMALL) {
            $problems[] = sprintf('round %d: %s, body %d bytes', $i, $headers[0] ?? 'none', strlen((string) $body));
        }
    }
    /* The last worker wakes up after 2 s and writes its body; wait for that. */
    sleep(3);
    /* The log check comes first: a crash shows there with its cause. */
    $tester->expectNoLogPattern('/killed by signal|exited with code/');
    if (gateway_pid($master) !== $gw) {
        throw new RuntimeException('the gateway process changed during the rounds: was ' . $gw . ', now ' . gateway_pid($master) . '.');
    }
    if ($problems !== []) {
        throw new RuntimeException('no answer from the gateway: ' . implode('; ', $problems) . '; gateway pid ' . $gw . ' now ' . gateway_pid($master));
    }
    echo "worker sleep: every round answered by the gateway: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/small.txt');
    @unlink($base . '/send.php');
    @rmdir($root);
    @rmdir($base);
}
?>
--EXPECT--
worker sleep: every round answered by the gateway: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
