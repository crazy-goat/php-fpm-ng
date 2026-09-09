--TEST--
fpm-ng: worker-mode HTTP-direct — a read watcher fires for TLS bytes sitting in the stream's userland buffer (task 079)
--SKIPIF--
<?php
include "skipif.inc";

if (!extension_loaded('openssl')) {
    die('skip requires the openssl extension (the test TLS origin runs in this CLI)');
}
// The origin runs as PHP_BINARY -n (no ini): a SHARED openssl is loaded via
// ini here but missing there, and the origin dies with "transport not found".
exec(PHP_BINARY . ' -n -r ' . escapeshellarg('exit(extension_loaded("openssl") ? 0 : 1);'), $o, $st);
if ($st !== 0) {
    die('skip the -n CLI has no openssl (shared ext loaded via ini here)');
}
?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Task 075 measured this over keep-alive TLS: with one read per readable
 * event, 769 of 8192 body bytes stranded at a 1 KiB read chunk, 3841 at 4 KiB,
 * 7937 at 8 KiB. The cause is that OpenSSL hands PHP a whole TLS record while
 * the descriptor libevent watches goes empty, so no readability event can ever
 * be produced for the remainder. This test reproduces the precondition
 * directly — bytes in the stream's buffer, nothing on the descriptor — and
 * then asserts the loop drains them anyway. */
const PAYLOAD = 8192;
const READ_CHUNK = 1024;

$root = sys_get_temp_dir() . '/fpmng-direct-worker-buffered-' . getmypid();
@mkdir($root, 0700, true);

$descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
$origin = null;
$originPipes = [];
$tester = null;

try {

$certFile = "$root/server.crt";
$keyFile = "$root/server.key";
$privkey = openssl_pkey_new(['private_key_bits' => 2048, 'private_key_type' => OPENSSL_KEYTYPE_RSA]);
$csr = openssl_csr_new(['commonName' => '127.0.0.1'], $privkey);
$x509 = openssl_csr_sign($csr, null, $privkey, 2);
openssl_x509_export_to_file($x509, $certFile);
openssl_pkey_export_to_file($privkey, $keyFile);

/* Binds on port 0 and prints the address it got, so a shared box cannot make
 * this test flaky. Every accepted connection gets the whole payload in one
 * write and is then held open: closing it would put an EOF on the client's
 * descriptor and hide exactly the stranding under test. */
file_put_contents("$root/origin.php", <<<'PHP'
<?php
[$cert, $key, $size] = [$argv[1], $argv[2], (int) $argv[3]];
$ctx = stream_context_create(['ssl' => ['local_cert' => $cert, 'local_pk' => $key]]);
$server = @stream_socket_server('tls://127.0.0.1:0', $errno, $errstr,
    STREAM_SERVER_BIND | STREAM_SERVER_LISTEN, $ctx);
if (!$server) {
    fwrite(STDERR, "origin: bind failed: $errstr\n");
    exit(1);
}
echo stream_socket_get_name($server, false), "\n";
/* STDIN is the pipe from the harness and nothing is ever written to it, so it
 * becomes readable only at EOF — that is, once the harness is gone. The
 * harness kills this process in its teardown, but a run that is interrupted
 * (Ctrl-C, CI timeout, SIGKILL) never reaches that teardown, and this process
 * would then hold a TLS listener on a shared box for ever (issue #89). Hence
 * the short accept timeout: it is the poll interval for that check, nothing
 * more. */
$stdin = fopen('php://stdin', 'r');
$open = [];
while (true) {
    $conn = @stream_socket_accept($server, 1);
    if ($conn === false) {
        $readable = [$stdin];
        $writable = $except = [];
        if (@stream_select($readable, $writable, $except, 0) > 0 && fgets($stdin) === false) {
            exit(0);
        }
        continue;
    }
    fgets($conn);
    fwrite($conn, str_repeat('a', $size));
    $open[] = $conn;
}
PHP);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

const PAYLOAD = 8192;
const READ_CHUNK = 1024;

/* The origin's address cannot come from the environment: FPM clears the
 * worker environment by default. The harness drops it next to this file. */
function connectToOrigin(): mixed
{
    $addr = trim((string) @file_get_contents(__DIR__ . '/tls.addr'));
    $ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
    $stream = @stream_socket_client("tls://$addr", $errno, $errstr, 5,
        STREAM_CLIENT_CONNECT, $ctx);
    if (!$stream) {
        return null;
    }
    fwrite($stream, "GET\n");
    return $stream;
}

/* Blocks until the origin's payload has actually arrived, then takes one short
 * read. After this the descriptor is empty and the rest of the record is in
 * the stream's userland buffer: the exact state that used to be permanent. */
function primeStrandedBuffer($stream): array
{
    stream_set_blocking($stream, true);
    $first = (string) fread($stream, READ_CHUNK);
    stream_set_blocking($stream, false);
    return [strlen($first), fpmng_worker_stream_has_buffered($stream)];
}

function handle(int $id): void
{
    $uri = fpmng_worker_request_env($id)['REQUEST_URI'] ?? '/';

    if ($uri === '/strand') {
        $stream = connectToOrigin();
        if ($stream === null) {
            fpmng_worker_respond($id, 500, [], json_encode(['error' => 'no origin']));
            return;
        }
        /* Nothing read yet, so the bytes are on the descriptor and not in the
         * buffer. The builtin has to tell those two apart. */
        $beforeRead = fpmng_worker_stream_has_buffered($stream);
        [$firstRead, $buffered] = primeStrandedBuffer($stream);

        $got = $firstRead;
        $reads = 0;
        $watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $stream, function () use ($stream, &$got, &$reads): void {
            $reads++;
            $chunk = fread($stream, READ_CHUNK);
            if (is_string($chunk)) {
                $got += strlen($chunk);
            }
        });
        fpmng_worker_event_enable($watcher);
        /* Non-blocking on purpose: before this task the loop had nothing to
         * wake for and a blocking pump would hang here instead of failing. */
        for ($i = 0; $i < 500 && $got < PAYLOAD; $i++) {
            fpmng_worker_loop(false);
        }
        fpmng_worker_event_free($watcher);
        fclose($stream);
        fpmng_worker_respond($id, 200, [], json_encode([
            'before_read' => $beforeRead,
            'first_read' => $firstRead,
            'buffered_after_short_read' => $buffered,
            'bytes' => $got,
            'reads' => $reads,
        ]));
        return;
    }

    if ($uri === '/spin') {
        $stream = connectToOrigin();
        if ($stream === null) {
            fpmng_worker_respond($id, 500, [], json_encode(['error' => 'no origin']));
            return;
        }
        primeStrandedBuffer($stream);
        $spins = 0;
        /* Reads nothing, ever. Level-triggered semantics trade a silent hang
         * for a busy spin, and the trade is only defensible if the spin is
         * named in the log rather than burning CPU quietly. */
        $watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $stream, function () use (&$spins): void {
            $spins++;
        });
        fpmng_worker_event_enable($watcher);
        for ($i = 0; $i < 200; $i++) {
            fpmng_worker_loop(false);
        }
        fpmng_worker_event_free($watcher);
        fclose($stream);
        fpmng_worker_respond($id, 200, [], json_encode(['spins' => $spins]));
        return;
    }

    if ($uri === '/closed') {
        /* The order userland is free to use and the SAPI must survive: close
         * the stream first, free the watcher afterwards. Between the two the
         * watcher holds a resource whose type zend_resource_dtor() has already
         * invalidated, and the per-iteration refill has to skip it silently.
         * Fetching it with the throwing accessor instead would leave an
         * exception pending before event_base_loop() ran, and since the
         * watcher stays registered every later fpmng_worker_loop() would throw
         * the same error: the worker's loop dead for good. */
        $stream = connectToOrigin();
        if ($stream === null) {
            fpmng_worker_respond($id, 500, [], json_encode(['error' => 'no origin']));
            return;
        }
        primeStrandedBuffer($stream);
        $fired = 0;
        $watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $stream, function () use (&$fired): void {
            $fired++;
        });
        fpmng_worker_event_enable($watcher);
        fclose($stream);
        $loops = 0;
        $error = null;
        try {
            for ($i = 0; $i < 5; $i++) {
                fpmng_worker_loop(false);
                $loops++;
            }
        } catch (\Throwable $e) {
            $error = $e->getMessage();
        }
        fpmng_worker_event_free($watcher);
        fpmng_worker_respond($id, 200, [], json_encode(['loops' => $loops, 'error' => $error]));
        return;
    }

    fpmng_worker_respond($id, 200, [], 'hello from pid ' . getmypid());
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
    while (($id = fpmng_worker_next_request()) !== null) {
        handle($id);
    }
}
PHP);

/* The array form runs the binary directly. With a command string PHP goes
 * through `/bin/sh -c`, and on a box where /bin/sh is dash the shell does not
 * exec its argument: proc_terminate() then signals the shell and leaves the
 * origin orphaned. Measured 2026-09-09 on 192.168.8.50 (dash 0.5.12): the
 * string form left the CLI running with ppid 1 after proc_terminate() plus
 * proc_close(), the array form left nothing — which is one leaked TLS
 * listener per suite run (issue #89). */
$origin = proc_open(
    [PHP_BINARY, '-n', "$root/origin.php", $certFile, $keyFile, (string) PAYLOAD],
    $descriptors, $originPipes);
check(is_resource($origin), 'could not start the TLS origin');
$addr = trim((string) fgets($originPipes[1]));
if (!preg_match('/^127\.0\.0\.1:\d+$/', $addr)) {
    /* Read the diagnostics only on failure, and only after making the pipe
     * non-blocking: the origin never exits on its own, so a blocking
     * stream_get_contents() here hangs the whole test instead of reporting
     * why the address was missing. */
    stream_set_blocking($originPipes[2], false);
    throw new RuntimeException('origin did not report an address: ' . var_export($addr, true) .
        ' / ' . stream_get_contents($originPipes[2]));
}
file_put_contents("$root/tls.addr", $addr);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_BUFFERED_PORT') ?: 28079);
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[buffered]
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
    $tester->start();
    $tester->expectLogStartNotices();

    $hello = file_get_contents("http://127.0.0.1:$port/");
    check(is_string($hello) && str_starts_with($hello, 'hello from pid '), 'hello: ' . var_export($hello, true));
    echo "hello-world: ok\n";

    $strand = json_decode((string) @file_get_contents("http://127.0.0.1:$port/strand"), true);
    check(is_array($strand), 'strand: not json');
    check($strand['before_read'] === false,
        'has_buffered: true before any read, so it is reporting the descriptor rather than the buffer');
    check($strand['buffered_after_short_read'] === true,
        'has_buffered: false after a short read, so the TLS record was not buffered and this test proves nothing');
    echo "has-buffered: ok\n";

    /* Before this task the answer was the first read and nothing more, for
     * ever: the descriptor was empty and no event could be produced. */
    check($strand['bytes'] === PAYLOAD,
        "strand: got {$strand['bytes']} of " . PAYLOAD . " bytes in {$strand['reads']} watcher call(s)");
    check($strand['reads'] > 0, 'strand: the watcher was never invoked');
    echo "buffered-stream-drains: ok\n";

    $spin = json_decode((string) @file_get_contents("http://127.0.0.1:$port/spin"), true);
    check(is_array($spin) && $spin['spins'] >= 100, 'spin: ' . var_export($spin, true));
    $tester->expectLogPattern(
        '/\[pool buffered\] http-direct worker: a read watcher was invoked 100 times in a row/', true);
    echo "busy-spin-is-logged: ok\n";

    $closed = json_decode((string) @file_get_contents("http://127.0.0.1:$port/closed"), true);
    check(is_array($closed), 'closed: not json');
    check($closed['error'] === null, 'closed: ' . var_export($closed['error'], true));
    check($closed['loops'] === 5, "closed: only {$closed['loops']} of 5 loop iterations ran");
    echo "closed-stream-does-not-wedge-the-loop: ok\n";

    $again = file_get_contents("http://127.0.0.1:$port/");
    check($again === $hello, "worker was replaced: $again vs $hello");
    echo "worker-persists: ok\n";
} finally {
    /* The whole body runs under this one finally, origin included: a failure
     * while starting the origin or reading its address used to happen outside
     * any teardown and left both the process and $root behind (issue #89).
     * Everything here has to tolerate a partially built fixture. */
    if ($tester !== null) {
        $tester->terminate();
        $tester->close();
    }
    if (is_resource($origin)) {
        proc_terminate($origin);
        foreach ($originPipes as $pipe) {
            @fclose($pipe);
        }
        proc_close($origin);
    }
    /* Not a fixed file list: rmdir() fails on anything the fixture happened to
     * leave behind, and then the directory survives the run too. */
    foreach (glob("$root/*") ?: [] as $file) {
        @unlink($file);
    }
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
hello-world: ok
has-buffered: ok
buffered-stream-drains: ok
busy-spin-is-logged: ok
closed-stream-does-not-wedge-the-loop: ok
worker-persists: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
