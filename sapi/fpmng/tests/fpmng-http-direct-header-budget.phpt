--TEST--
fpm-ng: both HTTP-direct executors spend the same response-header budget on the same bytes (issue #104)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

/* One cap, one set of bytes: `name: value\r\n` per header that reaches the
 * wire, charged by fpm_http_direct_header_charge() for whoever serves the
 * response. Before issue #104 the classic transport charged the whole raw
 * header() line *before* deciding whether the header would be dropped, and
 * the worker executor charged name + value with no punctuation — so the same
 * response could be served by one executor and answered 500 by the other.
 * This test sends the same header block to both pools and asserts the same
 * verdict, which is the only thing that notices a third answer appearing. */

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

function request(int $port, string $path): array
{
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    check($fp !== false, "connect to $port failed: $error");
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: budget.test\r\nConnection: close\r\n\r\n");
    $status = (string) fgets($fp);
    $headers = [];
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        [$key, $value] = explode(':', trim($line), 2);
        $headers[strtolower($key)][] = trim($value);
    }
    fclose($fp);
    return [$status, $headers];
}

$root = sys_get_temp_dir() . '/fpmng-direct-budget-' . getmypid();
@mkdir($root, 0700, true);

/* Both scripts fill the response up to the SAME number of charged bytes, and
 * each computes its own fixed part rather than assuming one: the classic
 * transport's Content-Type reaches send_headers() rewritten by PHP
 * ("Content-type: text/plain;charset=UTF-8", measured through headers_list()
 * on php-8.5.11-dev), while the worker executor emits the array it was given.
 * Hard-coding either would pin default_charset, not the budget. */
file_put_contents("$root/budget.inc", <<<'PHP'
<?php
/* What the transport writes for one header, and therefore what
 * fpm_http_direct_header_charge() charges: `name: value\r\n`. */
function budget_charge(string $name, string $value): int
{
    return strlen($name) + strlen($value) + 4;
}

/* Pads [name, value] pairs until the block charges exactly $target bytes,
 * counting the $fixed pairs the caller has already emitted. */
function budget_padding(int $target, array $fixed): array
{
    $rest = $target;
    foreach ($fixed as [$name, $value]) {
        $rest -= budget_charge($name, $value);
    }
    if ($rest < 0) {
        throw new RuntimeException("target $target is below the fixed part");
    }
    $pads = intdiv($rest, 1024);
    $tail = $rest % 1024;
    /* "X-Fill: \r\n" is 10 bytes on its own, so a remainder below 11 cannot be
     * expressed as one header; give the fill a whole pad's worth to spend. */
    if ($tail !== 0 && $tail < 11 && $pads > 0) {
        $pads--;
        $tail += 1024;
    }
    $out = [];
    for ($i = 0; $i < $pads; $i++) {
        /* 9 + 1011 + 4 = 1024. */
        $out[] = [sprintf('X-Pad-%03d', $i), str_repeat('p', 1011)];
    }
    if ($tail !== 0) {
        $out[] = ['X-Fill', str_repeat('f', $tail - 10)];
    }
    return $out;
}

/* Framing headers the transport owns: dropped by
 * fpm_http_direct_header_dropped(), never written, and therefore free. 64 KiB
 * of them — a whole second budget — is what the classic transport used to
 * charge before issue #104. */
function budget_dropped(): array
{
    $names = ['Content-Length', 'Transfer-Encoding', 'Connection', 'Keep-Alive', 'Upgrade', 'Trailer'];
    $out = [];
    for ($i = 0; $i < 64; $i++) {
        $out[] = [$names[$i % count($names)], str_repeat('d', 1000)];
    }
    return $out;
}
PHP);

/* header_remove() first: X-Powered-By is added at request startup and is
 * charged like any other header, so leaving it in would make the boundary
 * depend on expose_php. The Content-Type is set explicitly to keep PHP from
 * adding its default inside sapi_send_headers(), after headers_list() can no
 * longer see it. */
file_put_contents("$root/front.php", <<<'PHP'
<?php
require __DIR__ . '/budget.inc';
header_remove();
header('Content-Type: text/plain');
$fixed = [];
foreach (headers_list() as $line) {
    [$name, $value] = explode(':', $line, 2);
    $fixed[] = [$name, ltrim($value)];
}
$pairs = budget_padding((int) ($_GET['bytes'] ?? 0), $fixed);
if (isset($_GET['dropped'])) {
    $pairs = array_merge($pairs, budget_dropped());
}
foreach ($pairs as [$name, $value]) {
    /* replace = false: the framing block repeats names on purpose. */
    header("$name: $value", false);
}
echo 'served';
PHP);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
require __DIR__ . '/budget.inc';
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        $query = [];
        parse_str((string) ($env['QUERY_STRING'] ?? ''), $query);
        $fixed = [['Content-Type', 'text/plain']];
        $pairs = array_merge($fixed, budget_padding((int) ($query['bytes'] ?? 0), $fixed));
        if (isset($query['dropped'])) {
            $pairs = array_merge($pairs, budget_dropped());
        }
        /* respond() takes an array, so a repeated name becomes a list value —
         * the same header block, expressed the way this executor takes it. */
        $headers = [];
        foreach ($pairs as [$name, $value]) {
            $headers[$name] = isset($headers[$name])
                ? array_merge((array) $headers[$name], [$value])
                : $value;
        }
        fpmng_worker_respond($id, 200, $headers, 'served');
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$classicPort = (int) (getenv('FPMNG_DIRECT_BUDGET_CLASSIC_PORT') ?: 28104);
$workerPort = (int) (getenv('FPMNG_DIRECT_BUDGET_WORKER_PORT') ?: 28105);
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[classic]
listen = 127.0.0.1:$classicPort
pool.type = http-direct
pool.executor = classic
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
http.read_timeout = 10000
http.max_body = 1M
php_admin_value[display_errors] = 0
; The fixed part of the block is read with headers_list(), which cannot see a
; header PHP appends later inside sapi_send_headers(): with output compression
; on, Content-Encoding and Vary would be charged but not counted here and the
; at-the-cap case would flip to 500. Pinned rather than left to the host ini.
php_admin_value[zlib.output_compression] = 0
[worker]
listen = 127.0.0.1:$workerPort
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
http.max_body = 1M
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

$pools = ['classic' => $classicPort, 'worker' => $workerPort];
$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* Exactly FPM_HTTP_DIRECT_HEADERS_MAX (64 KiB) of header bytes is served,
     * and every header the application set is on the wire. */
    foreach ($pools as $name => $port) {
        [$status, $headers] = request($port, '/?bytes=65536');
        check(str_contains($status, ' 200 '), "$name refused a response at exactly 64 KiB: $status");
        check(count($headers['x-pad-000'] ?? []) === 1 && count($headers['x-fill'] ?? []) === 1,
            "$name lost a header at the cap: " . json_encode(array_keys($headers)));
    }
    echo "at-the-cap-served: ok\n";

    /* One byte more is refused by both, with a status rather than a response
     * missing a header the application asked for. */
    foreach ($pools as $name => $port) {
        [$status] = request($port, '/?bytes=65537');
        check(str_contains($status, ' 500 '), "$name served a response one byte over the cap: $status");
    }
    echo "one-byte-over-refused: ok\n";

    /* The regression #104 names: 64 KiB of framing headers the transport drops
     * are free, because none of them reaches the wire. The classic transport
     * charged them and answered 500 here while the worker executor served the
     * same response. */
    foreach ($pools as $name => $port) {
        [$status, $headers] = request($port, '/?bytes=65536&dropped=1');
        check(str_contains($status, ' 200 '), "$name charged dropped framing headers: $status");
        check(!in_array(str_repeat('d', 1000), $headers['content-length'] ?? [], true),
            "$name wrote an application Content-Length: " . json_encode($headers['content-length'] ?? []));
        check(count($headers['x-fill'] ?? []) === 1,
            "$name lost a header when framing headers were present: " . json_encode(array_keys($headers)));
    }
    echo "dropped-headers-are-free: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/front.php");
    @unlink("$root/worker.php");
    @unlink("$root/budget.inc");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
at-the-cap-served: ok
one-byte-over-refused: ok
dropped-headers-are-free: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
