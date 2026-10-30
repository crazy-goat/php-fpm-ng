--TEST--
fpm-ng: http.request_id = generate on a gateway replaces the client's X-Request-Id on an http.route[] http-direct target (issue #642)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #642: with http.request_id on, fpm_http_client.c drops the client's
 * X-Request-Id and sends the gateway's id to an http-direct route target as
 * an X-Request-Id header. The direct pool reads it as HTTP_X_REQUEST_ID. The
 * target must see the gateway's id and never the client's, and that same id
 * must be the X-Request-Id response header and request_id= in the access log.
 * The FastCGI route is covered by fpmng-http-request-id.phpt; this is the
 * HTTP/1.1 route transport. */
$docroot = sys_get_temp_dir() . '/fpmng-route-reqid-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', <<<'PHP'
<?php
echo $_SERVER['HTTP_X_REQUEST_ID'] ?? 'none';
PHP);

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.front_controller = /index.php
http.access_log = {{FILE:LOG:ACC}}
http.request_id = generate
http.route[direct] = /direct
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 1
[direct]
listen = {{ADDR[direct]}}
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $docroot
http.front_controller = /index.php
EOT;

function check(bool $ok, string $what): void
{
    if (!$ok) {
        echo "FAIL: $what\n";
        exit(1);
    }
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');
    $accessLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);

    $ctx = stream_context_create(['http' => [
        'timeout' => 5,
        'ignore_errors' => true,
        'header' => "X-Request-Id: client-made-up\r\n",
    ]]);
    $body = (string) @file_get_contents("http://$http/direct/index.php?r=rid", false, $ctx);
    $id = null;
    foreach ($http_response_header ?? [] as $h) {
        if (preg_match('/^X-Request-Id:\s*(\S+)/i', $h, $m)) {
            $id = $m[1];
        }
    }

    check($id !== null, "no X-Request-Id response header, target body: " . var_export($body, true));
    check($body !== 'client-made-up', 'the http-direct target saw the client X-Request-Id');
    check(preg_match('/^[0-9a-f]{32}$/', $body) === 1, "the http-direct target did not see a generated id: '$body'");
    check($body === $id, "the http-direct target saw '$body', the response has '$id'");
    echo "target-sees-gateway-id: ok\n";

    $line = '';
    $deadline = microtime(true) + 10;
    do {
        $content = (string) @file_get_contents($accessLog);
        foreach (explode("\n", $content) as $l) {
            if (str_contains($l, 'r=rid ')) {
                $line = $l;
            }
        }
        if ($line !== '') {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);

    check(str_contains($line, " request_id=$id") && preg_match('/ request_id=\S+$/', $line) === 1,
        "access log has no request_id=$id: $line");
    echo "access-log-id: ok\n";
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink("$docroot/index.php");
    @rmdir($docroot);
}
echo "Done\n";
?>
--EXPECT--
target-sees-gateway-id: ok
access-log-id: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
