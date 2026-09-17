--TEST--
fpm-ng: worker.max_lifetime recycles the worker gracefully once its uptime crosses the limit (issue #334)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* The master respawns the child after a recycle; the first connection
 * afterwards can arrive before it is listening again -- same retry shape
 * fpmng-http-direct-worker-max-pending.phpt uses. */
function httpGetRetry(string $url, int $attempts = 50): string
{
    for ($i = 0; $i < $attempts; $i++) {
        $body = @file_get_contents($url);
        if (is_string($body) && $body !== '') {
            return $body;
        }
        usleep(100000);
    }
    return '';
}

$root = sys_get_temp_dir() . '/fpmng-worker-max-lifetime-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

function handle(int $id): void
{
    fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'hello from pid ' . getmypid());
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        handle($id);
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_MAX_LIFETIME_PORT') ?: 28095);

/* worker.max_lifetime = 1 (one second) is the shortest useful value: every
 * generation of this worker lives for about a second before the health sweep
 * (fw.health_sweep, armed every FPM_WORKER_HEALTH_SWEEP_INTERVAL_SEC = 1
 * second) recycles it -- deterministic and fast for a test window, the same
 * "not a realistic limit, just a reliable trigger" reasoning
 * fpmng-http-direct-worker-max-memory.phpt uses for worker.max_memory = 1. */
$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[life]
listen = 127.0.0.1:$port
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
http.max_body = 1M
catch_workers_output = yes
worker.max_lifetime = 1
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $hello = httpGetRetry("http://127.0.0.1:$port/");
    check($hello !== '' && str_starts_with($hello, 'hello from pid '), 'hello: ' . var_export($hello, true));

    $tester->expectLogPattern('/NOTICE: .*\[pool life\] http-direct worker: lifetime \d+ s reached '
        . 'worker\.max_lifetime = 1 s, recycling the worker/', true);
    echo "max-lifetime-trip-is-logged: ok\n";

    /* And the pool keeps working: the master respawned the child. */
    $again = httpGetRetry("http://127.0.0.1:$port/");
    check(str_starts_with($again, 'hello from pid '), 'no respawn after recycle: ' . var_export($again, true));
    check($again !== $hello, 'the worker did not actually restart: ' . $again);
    echo "respawn-after-max-lifetime: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
max-lifetime-trip-is-logged: ok
respawn-after-max-lifetime: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
