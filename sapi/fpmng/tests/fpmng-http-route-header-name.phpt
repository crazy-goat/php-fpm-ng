--TEST--
fpm-ng: http.route[] HTTP transport forwards response header names up to FPM_HTTP_HEADER_NAME_MAX and refuses longer ones audibly (issue #453)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Issue #453: the relay loop bounded header names with the size of its stack
 * keybuf (64), so a name of 64+ bytes was dropped with no log line while the
 * FastCGI transport and the inbound side (both bounded by
 * FPM_HTTP_HEADER_NAME_MAX = 1024) forwarded or refused it. Boundary cases:
 * 63 survives, 64 survives, 1024 survives, 1025 is dropped. */

$docroot = sys_get_temp_dir() . '/fpmng-http-route-hdrname-' . getmypid();
@mkdir($docroot, 0700, true);

function headerName(int $len): string
{
    /* Valid token characters only; a trailing digit makes the length exact. */
    $base = 'X-Test-';
    return $base . str_repeat('a', max(0, $len - strlen($base) - 3)) . sprintf('%03d', $len);
}

file_put_contents($docroot . '/index.php', <<<'PHP'
<?php
foreach (['63', '64', '1024', '1025'] as $len) {
    header('X-Test-' . str_repeat('a', max(0, $len - 10)) . sprintf('%03d', (int) $len) . ': ' . $len);
}
echo "body";
PHP);

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 2
pool.type = http
http.gateways = 1
http.listen = {{ADDR[http]}}
http.front_controller = /index.php
http.route[direct] = /direct

[direct]
listen = {{ADDR[direct]}}
pm = static
pm.max_children = 1
pool.type = http-direct
chdir = $docroot
http.front_controller = /index.php
EOT;

function readHead($fp): array
{
    $line = fgets($fp);
    if (!$line || !str_starts_with($line, 'HTTP/1.1 200 ')) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $headers = [];
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        [$k, $v] = explode(':', $line, 2);
        $headers[strtolower(trim($k))] = trim($v);
    }
    return $headers;
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');

    $fp = stream_socket_client("tcp://$http", $errno, $error, 5);
    if (!$fp) throw new RuntimeException("connect: $error");
    stream_set_timeout($fp, 10);
    fwrite($fp, "GET /direct/index.php HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    $headers = readHead($fp);
    $body = stream_get_contents($fp);
    fclose($fp);
    check($body === 'body', 'body intact: ' . var_export($body, true));

    foreach (['63' => true, '64' => true, '1024' => true, '1025' => false] as $len => $expect) {
        $name = 'x-test-' . str_repeat('a', max(0, (int) $len - 10)) . $len;
        $present = isset($headers[$name]) && $headers[$name] === $len;
        check($present === $expect,
            "header name of $len bytes: " . ($expect ? 'expected present' : 'expected dropped')
            . ', got ' . var_export($headers[$name] ?? null, true));
    }
    echo "header-name-boundaries: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$docroot/index.php");
    @rmdir($docroot);
}
echo "Done\n";
?>
--EXPECT--
header-name-boundaries: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
