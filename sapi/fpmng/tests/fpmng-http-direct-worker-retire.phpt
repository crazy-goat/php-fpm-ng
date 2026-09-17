--TEST--
fpm-ng: SIGUSR1 retires a pool.executor = worker child without dropping the in-flight request (issue #336)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* fpmng-http-direct-retire.phpt (issue #65) covers the classic executor only.
 * This executor maps SIGUSR1 to the very same handler as SIGQUIT
 * (fpm_http_direct_worker.c, "issue #65 retires one child with SIGUSR1" --
 * both set fpm_worker_stopping and notify the loop) rather than a distinct
 * drain-the-connections behaviour: this executor tracks no per-connection
 * scoreboard to drain in the first place. What SIGUSR1 must still guarantee
 * here is the same contract the classic retire has -- a request already
 * in flight finishes with 200 rather than being abandoned, and a NEW request
 * arriving afterwards (even on a connection that was already open and idle)
 * is refused with 503 and Connection: close while the child winds down and
 * exits so the master can replace it. */
$root = sys_get_temp_dir() . '/fpmng-worker-retire-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

function waitFor(float $seconds): void
{
    $fiber = Fiber::getCurrent();
    $box = new stdClass();
    $box->id = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () use ($fiber, $box) {
        fpmng_worker_event_free($box->id);
        $fiber->resume();
    });
    fpmng_worker_event_enable($box->id, $seconds);
    Fiber::suspend();
}

function handle(int $id): void
{
    $env = fpmng_worker_request_env($id);
    $uri = $env['REQUEST_URI'] ?? '/';

    if (str_starts_with($uri, '/sleep')) {
        /* A marker file so the harness can wait for this handler to actually
         * be suspended -- i.e. genuinely in flight -- before it sends the
         * signal, rather than racing an arbitrary delay against the accept. */
        file_put_contents(__DIR__ . '/in-flight.marker', '1');
        waitFor(0.6);
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'slept ' . getmypid());
        return;
    }
    fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'hello from pid ' . getmypid());
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        (new Fiber(function () use ($id) {
            try {
                handle($id);
            } catch (Throwable $e) {
                fpmng_worker_respond($id, 500, [], 'handler failed');
            }
        }))->start();
    }
});
fpmng_worker_event_enable($watcher);

/* fpmng_worker_may_exit(), not "stopping and nothing in flight": the
 * in-flight /sleep fiber above must be allowed to finish once SIGUSR1 sets
 * fpm_worker_stopping -- the SAME loop condition
 * fpmng-http-direct-worker.phpt uses for its own concurrent-wait coverage. */
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_RETIRE_PORT') ?: 28104);
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
http.read_timeout = 10000
http.max_body = 1M
catch_workers_output = yes
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

function connect(int $port)
{
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    if (!$fp) throw new RuntimeException("connect :$port: $error");
    stream_set_timeout($fp, 10);
    return $fp;
}

function fetch($fp, string $path, bool $close): array
{
    $extra = $close ? "Connection: close\r\n" : '';
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: t\r\n$extra\r\n");
    $line = fgets($fp);
    if (!$line || !preg_match('#^HTTP/1\.1 (\d+) #', $line, $m)) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $status = (int) $m[1];
    $length = 0;
    $closing = false;
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        if (stripos($line, 'Content-Length:') === 0) {
            $length = (int) trim(substr($line, 15));
        }
        if (stripos($line, 'Connection:') === 0 && stripos($line, 'close') !== false) {
            $closing = true;
        }
    }
    $body = '';
    while (strlen($body) < $length) {
        $chunk = fread($fp, $length - strlen($body));
        if ($chunk === false || $chunk === '') {
            break;
        }
        $body .= $chunk;
    }
    return [$status, $body, $closing];
}

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    @unlink("$root/in-flight.marker");

    /* An idle keep-alive connection, opened and answered before the signal --
     * it must be refused only AFTER the retire, on its NEXT request. */
    $idleConn = connect($port);
    [$status] = fetch($idleConn, '/', false);
    check($status === 200, "idle connection's first request: $status");
    $pid = null;
    $body = trim((string) file_get_contents("http://127.0.0.1:$port/"));
    check(str_starts_with($body, 'hello from pid '), 'pid probe: ' . var_export($body, true));
    $pid = (int) substr($body, strlen('hello from pid '));
    check($pid > 1, "could not read the worker's pid: $body");

    /* The in-flight request, on a connection of its own. */
    $slow = connect($port);
    fwrite($slow, "GET /sleep HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");

    /* Wait for the handler to actually be suspended inside waitFor() before
     * signalling, so this genuinely retires a child with a request in
     * flight rather than merely racing the accept. */
    $deadline = microtime(true) + 10;
    while (!file_exists("$root/in-flight.marker") && microtime(true) < $deadline) {
        usleep(20000);
    }
    check(file_exists("$root/in-flight.marker"), 'the /sleep handler never reported itself in flight');

    $tester->signal('USR1', $pid);

    /* The idle keep-alive connection's NEXT request must be sent WHILE the
     * /sleep handler is still in flight (fw.pending still non-empty), not
     * after: docs/http-direct.md's "pool.executor = worker" interaction note
     * says this executor "tracks no connections", so fpmng_worker_may_exit()
     * only waits on fw.pending -- the instant the /sleep fiber answers and
     * pending drops to zero, the child tears itself down without lingering
     * for any idle connection that might still be open. So the only window in
     * which fpm_worker_accept()'s fpm_worker_stopping disjunct can be
     * observed refusing an idle connection is before that last pending entry
     * clears, exactly like fpmng-http-direct-worker-saturation-refuses-new.phpt
     * does with its own held request. */
    [$statusIdle, , $closeIdle] = fetch($idleConn, '/', true);
    check($statusIdle === 503, "idle connection's request after SIGUSR1: $statusIdle");
    check($closeIdle, 'the refusal after SIGUSR1 did not add Connection: close');
    fclose($idleConn);
    echo "idle-connection-refused-with-close: ok\n";

    $line = fgets($slow);
    check(is_string($line) && str_starts_with($line, 'HTTP/1.1 200'), 'in-flight request: ' . var_export($line, true));
    while (($l = fgets($slow)) !== false && $l !== "\r\n") {
    }
    $slowBody = stream_get_contents($slow);
    check(str_starts_with($slowBody, 'slept '), 'in-flight body: ' . var_export($slowBody, true));
    check((int) substr($slowBody, strlen('slept ')) === $pid, 'in-flight request answered by a different pid: ' . $slowBody);
    fclose($slow);
    echo "in-flight-request-completes: ok\n";

    /* And the child actually exits: the master replaces it, so a probe
     * afterwards gets a different pid. */
    $deadline = microtime(true) + 15;
    $newPid = null;
    while (microtime(true) < $deadline) {
        $probe = @file_get_contents("http://127.0.0.1:$port/");
        if (is_string($probe) && str_starts_with($probe, 'hello from pid ')) {
            $candidate = (int) substr(trim($probe), strlen('hello from pid '));
            if ($candidate !== $pid) {
                $newPid = $candidate;
                break;
            }
        }
        usleep(50000);
    }
    check($newPid !== null, 'the retired child was never replaced by a differently-pid\'d one');
    echo "child-exits-and-is-replaced: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @unlink("$root/in-flight.marker");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
idle-connection-refused-with-close: ok
in-flight-request-completes: ok
child-exits-and-is-replaced: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
