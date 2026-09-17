--TEST--
fpm-ng: a POST body over http.max_body gets libevent's own 413 before the worker script ever runs (issue #336)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* No existing worker test ever POSTs an actually-sized body through
 * fpmng_worker_request_body() (fpmng-http-direct-worker-request-body-idempotent.phpt,
 * issue #335, already covers a real, non-trivial body and its idempotency --
 * that part of this gap is not duplicated here). What is untested is
 * http.max_body: evhttp_set_max_body_size() makes libevent itself refuse an
 * oversized request with 413 before the front controller is ever invoked --
 * the worker script's handle() never runs, so fpmng_worker_request_body() is
 * never even reached for that request. This test proves that boundary from
 * the outside: a marker file the handler writes on every invocation is used
 * to prove the handler did NOT run for the oversized request. */
$root = sys_get_temp_dir() . '/fpmng-worker-max-body-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        /* Proof the handler actually ran for this request, independent of
         * whatever fpmng_worker_respond() manages to get onto the wire. */
        file_put_contents(__DIR__ . '/handler-ran.count', '1', FILE_APPEND);
        $body = fpmng_worker_request_body($id);
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], (string) strlen($body));
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_MAX_BODY_PORT') ?: 28107);
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
http.max_body = 8K
catch_workers_output = yes
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

function post(int $port, string $body): array
{
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    if (!$fp) throw new RuntimeException("connect :$port: $error");
    stream_set_timeout($fp, 10);
    $len = strlen($body);
    fwrite($fp, "POST /whatever HTTP/1.1\r\nHost: t\r\nConnection: close\r\nContent-Length: $len\r\n\r\n$body");
    $response = stream_get_contents($fp);
    fclose($fp);
    if (!preg_match('#^HTTP/1\.\d (\d+) #', $response, $m)) {
        throw new RuntimeException('bad status line: ' . var_export($response, true));
    }
    $split = strpos($response, "\r\n\r\n");
    $responseBody = $split === false ? '' : substr($response, $split + 4);
    return [(int) $m[1], $responseBody];
}

function handlerRunCount(string $root): int
{
    $contents = @file_get_contents("$root/handler-ran.count");
    return $contents === false ? 0 : strlen($contents);
}

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* (a) Under http.max_body (8K): received correctly and completely, and
     * the handler genuinely ran. */
    $underBody = str_repeat('a', 4096);
    [$status, $responseBody] = post($port, $underBody);
    check($status === 200, "under max_body: status $status");
    check($responseBody === (string) strlen($underBody), 'under max_body: length mismatch: ' . var_export($responseBody, true));
    check(handlerRunCount($root) === 1, 'the handler did not run for a request under http.max_body');
    echo "under-max-body-received-completely: ok\n";

    /* (b) Over http.max_body: libevent answers 413 on its own, and the
     * handler is never invoked at all -- the marker file's count must not
     * have grown past the one increment from case (a) above. */
    $overBody = str_repeat('b', 16 * 1024);
    [$status, ] = post($port, $overBody);
    check($status === 413, "over max_body: expected 413, got $status");
    check(handlerRunCount($root) === 1, 'the handler ran for a request over http.max_body -- libevent did not reject it before the front controller');
    echo "over-max-body-rejected-before-handler-runs: ok\n";

    /* The worker is still alive and serving afterwards: an oversized body
     * must not have wedged or crashed it. */
    [$status, $responseBody] = post($port, 'small');
    check($status === 200, "after oversized request: status $status");
    check($responseBody === '5', 'after oversized request: ' . var_export($responseBody, true));
    check(handlerRunCount($root) === 2, 'the handler did not run for the request after the oversized one');
    echo "worker-persists-after-rejection: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @unlink("$root/handler-ran.count");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
under-max-body-received-completely: ok
over-max-body-rejected-before-handler-runs: ok
worker-persists-after-rejection: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
