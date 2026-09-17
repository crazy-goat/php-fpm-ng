--TEST--
fpm-ng: fpmng_worker_request_body() is idempotent — a second call for the same id repeats the same body instead of "" (issue #335)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$root = sys_get_temp_dir() . '/fpmng-worker-body-idempotent-' . getmypid();
@mkdir($root, 0700, true);

/* Before issue #335's fix, fpmng_worker_request_body() drained the request's
 * evbuffer on the first call and returned "" on any later call for the same
 * id — indistinguishable from "this request genuinely had no body". This
 * worker script calls it twice for every request and reports both results,
 * so the test can tell a repeated read from a genuinely empty body. */
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $first = fpmng_worker_request_body($id);
        $second = fpmng_worker_request_body($id);
        $third = fpmng_worker_request_body($id);
        fpmng_worker_respond($id, 200, ['Content-Type' => 'application/json'], json_encode([
            'first' => $first,
            'second' => $second,
            'third' => $third,
        ]));
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_BODY_IDEMPOTENT_PORT') ?: 28100);
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

function post(int $port, string $body): array
{
    $fp = connect($port);
    $len = strlen($body);
    fwrite($fp, "POST /whatever HTTP/1.1\r\nHost: t\r\nConnection: close\r\nContent-Length: $len\r\n\r\n$body");
    $response = stream_get_contents($fp);
    fclose($fp);
    $split = strpos($response, "\r\n\r\n");
    if ($split === false) throw new RuntimeException("no header/body split: $response");
    return json_decode(substr($response, $split + 4), true, flags: JSON_THROW_ON_ERROR);
}

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* 1. A request with a real body: all three calls return the same bytes,
     * not just the first. */
    $result = post($port, 'hello world, this is the request body');
    check($result['first'] === 'hello world, this is the request body', 'first call: ' . var_export($result['first'], true));
    check($result['second'] === $result['first'], 'second call did not repeat the body: ' . var_export($result['second'], true));
    check($result['third'] === $result['first'], 'third call did not repeat the body: ' . var_export($result['third'], true));
    echo "body-idempotent: ok\n";

    /* 2. A request with no body at all: every call consistently answers "",
     * the same "" a body-having request's *first* call would never produce
     * once drained -- an empty body is not distinguishable from a bug that
     * silently dropped it, so this needs its own case, not an inference from
     * case 1. */
    $result = post($port, '');
    check($result['first'] === '', 'empty body first call: ' . var_export($result['first'], true));
    check($result['second'] === '', 'empty body second call: ' . var_export($result['second'], true));
    check($result['third'] === '', 'empty body third call: ' . var_export($result['third'], true));
    echo "empty-body-idempotent: ok\n";

    $tester->expectNoLogPattern('/ERROR:/', true);
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
body-idempotent: ok
empty-body-idempotent: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
