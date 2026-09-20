--TEST--
fpm-ng: worker executor answers ping.path after the saturation check and before the userland queue (issue #387)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Issue #387: the worker executor refused ping.path together with the status
 * page, but ping needs none of the per-request scoreboard accounting that
 * refusal was about -- it is a literal path match. This pins that it is
 * answered (rule 1 of docs/gateway.md), that the match is exact and the query
 * is cut off, and that a matching request never reaches the application. */

$root = sys_get_temp_dir() . '/fpmng-worker-ping-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$w = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'app:' . ($env['REQUEST_URI'] ?? ''));
    }
});
fpmng_worker_event_enable($w);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_PING_PORT') ?: 28148);
$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[worker]
listen = 127.0.0.1:$port
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
ping.path = /ping
ping.response = pong
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

function get(int $port, string $path): string
{
    $body = @file_get_contents("http://127.0.0.1:$port$path");
    if ($body === false) {
        throw new RuntimeException("GET $path failed");
    }
    return $body;
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    check(get($port, '/ping') === 'pong', '/ping did not answer pong');
    echo "ping-answered: ok\n";

    /* The query is cut off before the match, as in the classic executor. */
    check(get($port, '/ping?x=1') === 'pong', '/ping?x=1 did not answer pong');
    echo "ping-query-ignored: ok\n";

    /* Exact match, not a prefix: /pings is the application's. */
    check(get($port, '/pings') === 'app:/pings', '/pings reached the ping handler');
    echo "prefix-not-ping: ok\n";
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
ping-answered: ok
ping-query-ignored: ok
prefix-not-ping: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
