--TEST--
fpm-ng: fpm_connection_info() on pool.executor = worker, keyed by request id (issue #335)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$root = sys_get_temp_dir() . '/fpmng-worker-conninfo-' . getmypid();
@mkdir($root, 0700, true);

/* Plain (non-TLS) worker pool: fpm_connection_info($id) reports peer_addr/
 * peer_port and transport=plain with no TLS-specific keys. Client-certificate
 * and TLS-protocol fields are already exercised for this same code path
 * (shared with the classic executor via fpm_http_direct_x509_*()) by
 * sapi/fpmng/tests/fpmng-http-direct-connection-info.phpt's TLS cases; this
 * test does not duplicate a TLS harness just to re-cover the same formatting
 * code under the worker executor. */
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        $uri = $env['REQUEST_URI'] ?? '/';

        if ($uri === '/withid') {
            $info = fpm_connection_info($id);
        } elseif ($uri === '/noid') {
            $info = fpm_connection_info();
        } elseif ($uri === '/badid') {
            /* An id nothing has ever handed out for this worker: next_id
             * starts at 1 and only counts up, so this is never valid. */
            $info = fpm_connection_info(999999999);
        } else {
            $info = 'unexpected-uri';
        }
        fpmng_worker_respond($id, 200, ['Content-Type' => 'application/json'], json_encode($info));
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_CONNINFO_PORT') ?: 28098);
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

function fetch(int $port, string $path): array
{
    $fp = connect($port);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    $response = stream_get_contents($fp);
    fclose($fp);
    $split = strpos($response, "\r\n\r\n");
    if ($split === false) throw new RuntimeException("no header/body split: $response");
    return [substr($response, 0, $split), json_decode(substr($response, $split + 4), true, flags: JSON_THROW_ON_ERROR)];
}

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* 1. A valid id: peer_addr/peer_port and transport=plain are there, no
     * TLS-only key leaks through on a pool without http.tls_cert. age/requests
     * are absent -- not null, absent -- because this executor never tracks a
     * connection once the first request off it has been dispatched (see the
     * fpm_connection_info() comment in fpm_http_direct_worker.c). */
    [, $info] = fetch($port, '/withid');
    check(is_array($info), 'fpm_connection_info($id) did not return an array: ' . var_export($info, true));
    check($info['transport'] === 'plain', 'transport: ' . var_export($info['transport'] ?? null, true));
    check($info['peer_addr'] === '127.0.0.1', 'peer_addr: ' . var_export($info['peer_addr'] ?? null, true));
    check(is_int($info['peer_port']) && $info['peer_port'] > 0, 'peer_port: ' . var_export($info['peer_port'] ?? null, true));
    check(!array_key_exists('age', $info), 'age key present when it cannot be known');
    check(!array_key_exists('requests', $info), 'requests key present when it cannot be known');
    check(!array_key_exists('tls_protocol', $info), 'tls_protocol leaked on a plain pool');
    echo "with-id-reports-peer: ok\n";

    /* 2. No id (the omitted-argument default, 0): false, unconditionally --
     * there is no ambient "current connection" to default to. */
    [, $info] = fetch($port, '/noid');
    check($info === false, 'fpm_connection_info() with no id should answer false, got: ' . var_export($info, true));
    echo "no-id-refused: ok\n";

    /* 3. An id nothing ever handed out: false, the same as "no id" -- an
     * unknown id is not distinguishable from "nothing to report". */
    [, $info] = fetch($port, '/badid');
    check($info === false, 'fpm_connection_info() with an unknown id should answer false, got: ' . var_export($info, true));
    echo "unknown-id-refused: ok\n";

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
with-id-reports-peer: ok
no-id-refused: ok
unknown-id-refused: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
