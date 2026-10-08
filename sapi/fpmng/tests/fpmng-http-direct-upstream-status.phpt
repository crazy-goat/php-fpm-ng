--TEST--
fpm-ng: buffered http-direct rejects an invalid CGI Status with 502, a WARNING and one rejected response (issue #604)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #604: the buffered path must use the gateway's strict CGI parser
 * (#594), not atoi(). A rejected status must discard the script's headers
 * and body without corrupting the next response on this connection (#451).
 * The counter and access log must describe the response the client receives. */
function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

/* The PHP HTTP wrapper can hide an invalid status line. Read one framed
 * response at a time so the same connection also detects a stray body. */
function fetch_raw($fp, string $path): array
{
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: status.test\r\n\r\n");
    $line = fgets($fp);
    check($line !== false && preg_match('#^HTTP/1\.1 (\d{3}) #', $line, $m) === 1,
        'bad status line: ' . var_export($line, true));
    $status = (int) $m[1];
    $headers = [];
    while (($header = fgets($fp)) !== false && $header !== "\r\n") {
        [$name, $value] = explode(':', $header, 2);
        $headers[strtolower($name)] = trim($value);
    }
    check($header === "\r\n" && isset($headers['content-length']), 'incomplete response headers');
    $length = (int) $headers['content-length'];
    $body = '';
    while (strlen($body) < $length) {
        $chunk = fread($fp, $length - strlen($body));
        check($chunk !== false && $chunk !== '', 'short response body');
        $body .= $chunk;
    }
    return [$status, rtrim($line, "\r\n"), $headers, $body];
}

function file_wait(string $file, string $pattern, int $count = 1): string
{
    $deadline = microtime(true) + 10;
    do {
        $content = (string) @file_get_contents($file);
        if (preg_match_all($pattern, $content) >= $count) {
            return $content;
        }
        usleep(20000);
    } while (microtime(true) < $deadline);
    throw new RuntimeException("missing log pattern $pattern\n$content");
}

function rejected(string $ops): int
{
    $body = fpmng_operator_body($ops, '/status');
    check(preg_match('/^rejected responses: *(\d+)$/m', $body, $m) === 1,
        "missing rejected counter\n$body");
    return (int) $m[1];
}

$root = sys_get_temp_dir() . '/fpmng-direct-upstream-status-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/front.php", <<<'PHP'
<?php
header('X-Script: present');
header('Content-Type: application/x-script');
if (isset($_GET['code'])) {
    http_response_code(103);
} elseif (isset($_GET['badname'])) {
    header('X Bad: invalid');
} elseif (isset($_GET['duplicate'])) {
    header('Status: abc');
    header('Status: 404', false);
} elseif (isset($_GET['mixed'])) {
    if ($_GET['mixed'] === 'name-first') {
        header('X Bad: invalid');
        header('Status: abc');
    } else {
        header('Status: abc');
        header('X Bad: invalid');
    }
} else {
    header('Status: ' . $_GET['v']);
}
echo 'script body';
if (isset($_GET['respond'])) {
    fpmng_respond();
    echo 'discarded after respond';
}
PHP);

$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[status]
listen = {{ADDR[http]}}
pool.type = http-direct
pool.executor = classic
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
access.log = {{FILE:LOG:ACC}}
access.format = "%m %r%Q%q %s"
operator.status_path = /status
operator.status_listen = {{ADDR[ops]}}
CFG;

$invalid = [
    'abc' => 'abc', 'big' => '99999', 'neg' => '-5', 'huge' => '99999999999',
    'info' => '103', 'cont' => '100', 'low' => '199', 'high' => '600',
    'empty' => '', 'short' => '40', 'suffix' => '404Oops', 'tab' => "404\tGone",
    'long' => str_repeat('9', 80),
];
$expectedAccess = [];
$tester = new FPM\Tester($cfg, '<?php');
try {
    /* The worker's WARNING must reach error_log without catch_workers_output
     * in the configuration, as required since issue #260. */
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');
    $ops = $tester->getAddr('ipv4', '[ops]');
    $fp = stream_socket_client("tcp://$http", $errno, $error, 5);
    check($fp !== false, "connect $http: $error");
    stream_set_timeout($fp, 10);

    foreach (['bare' => '404', 'reason' => '404 Gone', 'min' => '200',
              'custom' => '299', 'max' => '599'] as $name => $value) {
        [$status, $line, $headers, $body] = fetch_raw($fp, '/?v=' . rawurlencode($value) . "&n=$name");
        $code = (int) substr($value, 0, 3);
        check($status === $code, "control '$value': $line");
        check($body === 'script body' && ($headers['x-script'] ?? '') === 'present',
            "control '$value': script response was discarded");
        check(!isset($headers['status']), 'CGI Status reached the wire as a header');
        $expectedAccess[$name] = $code;
        echo "control-$name: $status\n";
    }
    check(rejected($ops) === 0, 'valid statuses counted as rejected');

    foreach ($invalid as $name => $value) {
        [$status, $line, $headers, $body] = fetch_raw($fp, '/?v=' . rawurlencode($value) . "&n=$name");
        echo "$name: $line\n";
        check($status === 502, "Status '$value' must get 502, got $line");
        check($body === "http-direct: invalid response Status\n", "unexpected 502 body: $body");
        check(!isset($headers['x-script']) && ($headers['content-type'] ?? '') !== 'application/x-script',
            'script headers survived an invalid Status');
        $expectedAccess[$name] = 502;
    }

    /* fpmng_respond() uses the same buffered finalizer before PHP shutdown.
     * Its later shutdown must neither count again nor replace the 502 in the log. */
    [$status, $line, , $body] = fetch_raw($fp, '/?v=abc&respond=1&n=respond');
    check($status === 502 && $body === "http-direct: invalid response Status\n", "respond: $line\n$body");
    $expectedAccess['respond'] = 502;
    echo "respond: 502\n";

    /* A later valid Status cannot undo the earlier rejection. */
    [$status, $line, , $body] = fetch_raw($fp, '/?duplicate=1&n=duplicate');
    check($status === 502 && $body === "http-direct: invalid response Status\n", "duplicate: $line\n$body");
    $expectedAccess['duplicate'] = 502;
    echo "duplicate: 502\n";

    /* The new 502 must not change the 500 contract for other rejection causes. */
    foreach (['code' => 'response status is not a final status',
              'badname' => 'malformed response header name'] as $name => $why) {
        [$status, $line, , $body] = fetch_raw($fp, "/?$name=1&n=$name");
        check($status === 500 && $body === "http-direct: $why\n", "$name: $line\n$body");
        $expectedAccess[$name] = 500;
        echo "$name: 500\n";
    }
    /* One response can contain both faults. The first cause must determine
     * the error body, status and WARNING instead of mixing a 502 with a 500 log. */
    foreach (['status-first' => [502, 'invalid response Status'],
              'name-first' => [500, 'malformed response header name']] as $name => [$code, $why]) {
        [$status, $line, , $body] = fetch_raw($fp, "/?mixed=$name&n=$name");
        check($status === $code && $body === "http-direct: $why\n", "$name: $line\n$body");
        $expectedAccess[$name] = $code;
        echo "$name: $status\n";
    }
    [$status, $line, , $body] = fetch_raw($fp, '/?v=404&n=post');
    check($status === 404 && $body === 'script body', "connection after rejection: $line\n$body");
    $expectedAccess['post'] = 404;
    echo "connection-after-rejection: 404\n";
    fclose($fp);

    $errors = file_wait($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR),
        "/response header name is not an HTTP token, answering 500: 'X\\\\x20Bad'/", 2);
    foreach ($invalid as $value) {
        $pattern = "/WARNING: \[pool status\] http-direct: upstream sent invalid Status '"
            . preg_quote(substr($value, 0, 64), '/') . "', answering 502/";
        check(preg_match($pattern, $errors) === 1, 'missing WARNING for ' . var_export($value, true));
    }
    check(preg_match_all('/upstream sent invalid Status /', $errors) === count($invalid) + 3,
        "unexpected Status WARNING count\n$errors");
    check(preg_match_all('/response header name is not an HTTP token, answering 500/', $errors) === 2,
        "unexpected header WARNING count\n$errors");
    echo "warnings-and-truncation: ok\n";

    $acc = file_wait($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC), '/n=post 404/');
    foreach ($expectedAccess as $name => $code) {
        check(preg_match('/n=' . $name . ' ' . $code . '$/m', $acc) === 1,
            "access log: missing $code for $name\n$acc");
    }
    echo "access-log: ok\n";
    $count = rejected($ops);
    check($count === count($invalid) + 6, "rejected responses: expected 19, got $count");
    echo "rejected-responses: $count\n";
} finally {
    if (isset($fp) && is_resource($fp)) {
        fclose($fp);
    }
    $tester->terminate();
    $tester->close();
    @unlink("$root/front.php");
    @rmdir($root);
}
?>
--EXPECT--
control-bare: 404
control-reason: 404
control-min: 200
control-custom: 299
control-max: 599
abc: HTTP/1.1 502 Bad Gateway
big: HTTP/1.1 502 Bad Gateway
neg: HTTP/1.1 502 Bad Gateway
huge: HTTP/1.1 502 Bad Gateway
info: HTTP/1.1 502 Bad Gateway
cont: HTTP/1.1 502 Bad Gateway
low: HTTP/1.1 502 Bad Gateway
high: HTTP/1.1 502 Bad Gateway
empty: HTTP/1.1 502 Bad Gateway
short: HTTP/1.1 502 Bad Gateway
suffix: HTTP/1.1 502 Bad Gateway
tab: HTTP/1.1 502 Bad Gateway
long: HTTP/1.1 502 Bad Gateway
respond: 502
duplicate: 502
code: 500
badname: 500
status-first: 502
name-first: 500
connection-after-rejection: 404
warnings-and-truncation: ok
access-log: ok
rejected-responses: 19
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
