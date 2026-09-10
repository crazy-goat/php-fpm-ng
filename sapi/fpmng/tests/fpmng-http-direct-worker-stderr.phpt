--TEST--
fpm-ng: worker-mode HTTP-direct — STDIN, STDOUT and STDERR exist, and a failing handler logs instead of killing the worker (issue #73)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* The master respawns the child after a generation ends; the first connection
 * afterwards can arrive before it is listening again. */
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

$root = sys_get_temp_dir() . '/fpmng-worker-stderr-' . getmypid();
@mkdir($root, 0700, true);

/* Deliberately dependency-free — core Fiber plus the raw primitives, like the
 * other worker tests — but shaped like the amphp bridge's failure path, which
 * is the one issue #73 is about: the handler throws, and the catch that is
 * supposed to swallow it reports the failure with fwrite(STDERR, ...). Before
 * the fix that write threw "Undefined constant" from inside the catch, the
 * Error escaped the fiber, and one failing request took the whole worker down
 * (examples/http-direct-worker/FpmngServer.php:69, in Amp\async()).
 *
 * The fiber is not decoration: an exception escaping Fiber::start() unwinds
 * into the libevent callback that started it, which is exactly how the bridge
 * loses its worker. */
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

function handle(int $id): void
{
    $env = fpmng_worker_request_env($id);
    $uri = $env['REQUEST_URI'] ?? '/';

    if (str_starts_with($uri, '/constants')) {
        fwrite(STDOUT, "worker wrote to stdout\n");
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], sprintf(
            "in=%d out=%d err=%d read=%s eof=%d pid=%d",
            (int) defined('STDIN'), (int) defined('STDOUT'), (int) defined('STDERR'),
            var_export(fread(STDIN, 8), true), (int) feof(STDIN), getmypid()));
        return;
    }
    if (str_starts_with($uri, '/throw')) {
        throw new RuntimeException('handler blew up');
    }
    fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'alive pid ' . getmypid());
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        (new Fiber(function () use ($id) {
            try {
                handle($id);
            } catch (Throwable $e) {
                fwrite(STDERR, 'http-direct worker: handler failed: ' . $e->getMessage() . PHP_EOL);
                fpmng_worker_respond($id, 500, [], "Internal Server Error\n");
            }
        }))->start();
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_STDERR_PORT') ?: 28084);

$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[work]
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
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* All three constants, and what STDIN actually is in such a child: a
     * handle on the /dev/null fpm_stdio_init_main() installs, so an immediate
     * EOF rather than a fatal or a blocking read. */
    $constants = httpGetRetry("http://127.0.0.1:$port/constants");
    check((bool) preg_match("/^in=1 out=1 err=1 read='' eof=1 pid=(\\d+)$/", $constants, $m),
        'constants: ' . var_export($constants, true));
    $pid = $m[1];
    echo "constants-exist: ok\n";

    $tester->expectLogPattern('/WARNING: .*\[pool work\] child \d+ said into stdout: "worker wrote to stdout"/', true, 10);
    echo "stdout-reaches-the-log: ok\n";

    /* The regression this issue names. The 500 comes from the catch, so
     * receiving it already proves the catch ran to completion. */
    $failed = @file_get_contents("http://127.0.0.1:$port/throw");
    $status = http_get_last_response_headers() ?? [];
    check((bool) preg_grep('{^HTTP/1\.[01] 500}', $status),
        'a failing handler did not get its 500, got ' . json_encode($status) . ' body ' . var_export($failed, true));
    echo "failing-handler-gets-500: ok\n";

    $tester->expectLogPattern('/WARNING: .*\[pool work\] child \d+ said into stderr: '
        . '"http-direct worker: handler failed: handler blew up"/', true, 10);
    echo "failure-reaches-the-log: ok\n";

    /* Before the fix the fwrite above threw and the Error unwound out of the
     * fiber into the libevent callback, so this is the assertion that would
     * have gone red: same pid, no respawn, the worker never noticed. */
    $alive = httpGetRetry("http://127.0.0.1:$port/");
    /* The same pid, not merely a well-formed reply: pm.max_children = 1 and
     * httpGetRetry() retries for 5 s, so a worker that died on the failing
     * handler and was respawned by the master would also answer here. */
    check($alive === "alive pid $pid",
        'the worker did not survive a failing handler: ' . var_export($alive, true)
        . ' (expected the pid ' . $pid . ' that served /constants)');
    echo "worker-survives: ok\n";

    $tester->expectNoLogPattern('/Undefined constant "STD(IN|OUT|ERR)"/', true);
    $tester->expectNoLogPattern('/the worker script returned without being asked to stop/', true);
    echo "no-undefined-constant-and-no-restart: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
constants-exist: ok
stdout-reaches-the-log: ok
failing-handler-gets-500: ok
failure-reaches-the-log: ok
worker-survives: ok
no-undefined-constant-and-no-restart: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
