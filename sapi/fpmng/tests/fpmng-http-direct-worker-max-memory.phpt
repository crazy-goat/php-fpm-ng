--TEST--
fpm-ng: worker.max_memory recycles the worker gracefully once its peak RSS crosses the limit (issue #334)
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

$root = sys_get_temp_dir() . '/fpmng-worker-max-memory-' . getmypid();
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

$port = (int) (getenv('FPMNG_DIRECT_WORKER_MAX_MEMORY_PORT') ?: 28094);

/* worker.max_memory = 1 (one byte) always compares true against any real
 * getrusage() peak RSS, the same deterministic-and-frequent trigger
 * fpmng-supervisor-max-memory.phpt uses for supervisor.max_memory -- not a
 * realistic limit, just one guaranteed to trip on the health sweep's very
 * first tick (fw.health_sweep runs every FPM_WORKER_HEALTH_SWEEP_INTERVAL_SEC
 * = 1 second) without needing the worker to actually allocate anything. */
$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[mem]
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
worker.max_memory = 1
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* worker.max_memory = 1 recycles the worker on the health sweep's very
     * first tick (about a second in), so the first request racing that tick
     * can legitimately land on a worker already refusing new connections
     * (fpm_worker_accept() 503s once fpm_worker_stopping is set) -- retried
     * here the same way a post-recycle request is below, rather than assumed
     * to always win the race. */
    $hello = httpGetRetry("http://127.0.0.1:$port/");
    check($hello !== '' && str_starts_with($hello, 'hello from pid '), 'hello: ' . var_export($hello, true));

    $tester->expectLogPattern('/NOTICE: .*\[pool mem\] http-direct worker: memory usage \d+ bytes reached '
        . 'worker\.max_memory = 1 bytes, recycling the worker/', true);
    echo "max-memory-trip-is-logged: ok\n";

    /* And the pool keeps working: the master respawned the child. */
    $again = httpGetRetry("http://127.0.0.1:$port/");
    check(str_starts_with($again, 'hello from pid '), 'no respawn after recycle: ' . var_export($again, true));
    check($again !== $hello, 'the worker did not actually restart: ' . $again);
    echo "respawn-after-max-memory: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
max-memory-trip-is-logged: ok
respawn-after-max-memory: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
