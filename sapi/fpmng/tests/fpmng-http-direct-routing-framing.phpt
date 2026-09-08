--TEST--
fpm-ng: direct HTTP fixed routing ignores doc_root/user_dir and bodyless responses preserve framing
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
$root = __DIR__;
$script = '/fpmng-direct-routing-front-' . getmypid() . '.php';
$target = '/fpmng-direct-routing-secret-' . getmypid() . '.txt';
file_put_contents($root . $target, 'SECRET');
file_put_contents($root . $script, <<<'PHP'
<?php
if (isset($_GET['status'])) {
    http_response_code((int) $_GET['status']);
    echo 'must-not-be-sent';
} elseif (isset($_GET['flush'])) {
    flush(); echo 'must-not-be-sent';
} elseif (isset($_GET['overflow'])) {
    header('X-Large: ' . str_repeat('x', 65536));
} else { echo 'controller'; }
PHP);
$port = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054) + 4;
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = $script
php_admin_value[doc_root] = $root
php_admin_value[user_dir] = public_html
CFG;
function fetchDirect($fp, string $path, string $method = 'GET', int $expected = 200): string
{
    fwrite($fp, "$method $path HTTP/1.1\r\nHost: test\r\n\r\n");
    $line = fgets($fp);
    if (!$line || !str_starts_with($line, "HTTP/1.1 $expected ")) {
        throw new RuntimeException('bad response framing/status: ' . var_export($line, true));
    }
    $length = 0;
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        if (preg_match('/^Content-Length: (\d+)/i', $line, $m)) $length = (int) $m[1];
    }
    if ($method === 'HEAD' || in_array($expected, [204, 205, 304], true)) return '';
    $body = '';
    while (strlen($body) < $length) {
        $chunk = fread($fp, $length - strlen($body));
        if ($chunk === false || $chunk === '') throw new RuntimeException('short body');
        $body .= $chunk;
    }
    return $body;
}
$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    stream_set_timeout($fp, 5);
    foreach ([$target, '/../../etc/passwd', '/%2e%2e/%2e%2e/etc/passwd', '/~' . get_current_user() . $target] as $path) {
        if (fetchDirect($fp, $path) !== 'controller') throw new RuntimeException('client selected a script');
    }
    foreach ([204, 205, 304] as $status) {
        fetchDirect($fp, "/?status=$status", expected: $status);
        if (fetchDirect($fp, '/') !== 'controller') throw new RuntimeException('bodyless response corrupted next request');
    }
    foreach (['flush' => 200, 'overflow' => 500] as $query => $status) {
        fetchDirect($fp, "/?$query=1", 'HEAD', $status);
        if (fetchDirect($fp, '/') !== 'controller') throw new RuntimeException('HEAD corrupted next request');
    }
    fclose($fp);
    echo "fixed-routing/bodyless-framing: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    unlink($root . $script);
    unlink($root . $target);
}
?>
--EXPECT--
fixed-routing/bodyless-framing: ok
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
