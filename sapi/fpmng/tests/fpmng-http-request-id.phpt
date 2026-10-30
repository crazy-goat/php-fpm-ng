--TEST--
fpm-ng: http.request_id generates or propagates one id per request, to the target, the response and the access log (issue #642)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #642: http.request_id = generate makes a 128-bit id per request. The
 * same id is the X-Request-Id response header, HTTP_X_REQUEST_ID in the
 * target, and request_id= in the access log. propagate keeps an inbound
 * X-Request-Id only when the DIRECT peer is in http.trusted_proxies; a
 * client that is not trusted never gets its own id believed, and a value
 * that fails the charset check is replaced, not cut. A header with an
 * underscore (X_Request_Id) is dropped by the CGI key rule (#595), so it can
 * not spoof HTTP_X_REQUEST_ID either. The loopback test client is 127.0.0.1. */
$docroot = sys_get_temp_dir() . '/fpmng-reqid-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', <<<'PHP'
<?php
echo $_SERVER['HTTP_X_REQUEST_ID'] ?? 'none';
PHP);

function gatewayConfig(string $mode, string $trusted): string
{
    global $docroot;
    $trustedLine = $trusted === '' ? '' : "http.trusted_proxies = $trusted";
    return <<<EOT
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
http.request_id = $mode
$trustedLine
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 2
EOT;
}

/* One request. Returns the body, the X-Request-Id response header (or null). */
function fetch(string $url, string $extra = ''): array
{
    $ctx = stream_context_create(['http' => [
        'timeout' => 5,
        'ignore_errors' => true,
        'header' => $extra,
    ]]);
    $body = (string) @file_get_contents($url, false, $ctx);
    $id = null;
    foreach ($http_response_header ?? [] as $h) {
        if (preg_match('/^X-Request-Id:\s*(\S+)/i', $h, $m)) {
            $id = $m[1];
        }
    }
    return [$body, $id];
}

function accessLine(string $file, string $tag): string
{
    $deadline = microtime(true) + 10;
    do {
        $content = (string) @file_get_contents($file);
        foreach (explode("\n", $content) as $line) {
            if (str_contains($line, "r=$tag ")) {
                return $line;
            }
        }
        usleep(100000);
    } while (microtime(true) < $deadline);

    return '';
}

function check(bool $ok, string $what): void
{
    if (!$ok) {
        echo "FAIL: $what\n";
        exit(1);
    }
}

function run(string $label, string $mode, string $trusted, array $cases): void
{
    global $docroot;
    $tester = new FPM\Tester(gatewayConfig($mode, $trusted), '<?php echo "unused";');
    try {
        $tester->start();
        $tester->expectLogStartNotices();
        $http = $tester->getAddr('ipv4', '[http]');
        $accessLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);

        foreach ($cases as $tag => $case) {
            [$extra, $expect] = $case;
            [$body, $id] = fetch("http://$http/index.php?r=$tag", $extra);
            check($id !== null, "$label/$tag: no X-Request-Id response header");
            check($body === $id, "$label/$tag: target saw '$body', response has '$id'");
            $line = accessLine($accessLog, $tag);
            check(str_contains($line, " request_id=$id") && preg_match('/ request_id=\S+$/', $line) === 1,
                "$label/$tag: access log has no request_id=$id: $line");
            $expect($id);
            echo "$label/$tag: ok\n";
        }
    } finally {
        $tester->terminate();
        $tester->close();
    }
}

$isGenerated = function (string $id): void {
    check(preg_match('/^[0-9a-f]{32}$/', $id) === 1, "generated id is not 32 hex digits: $id");
};
$isKept = function (string $want): callable {
    return function (string $id) use ($want): void {
        check($id === $want, "inbound id '$want' was not kept, got '$id'");
    };
};
$isNotClient = function (string $client) use ($isGenerated): callable {
    return function (string $id) use ($client, $isGenerated): void {
        check($id !== $client, "client id '$client' was believed");
        $isGenerated($id);
    };
};

/* generate: the client's own id and an underscore spoof are both ignored. */
run('generate', 'generate', '', [
    'gen' => ["X-Request-Id: client-made-up\r\nX_Request_Id: spoofed-1\r\n", $isNotClient('client-made-up')],
]);

/* propagate, trusted direct peer: a valid id is kept; an invalid one is replaced. */
run('trusted', 'propagate', '127.0.0.1', [
    'keep' => ["X-Request-Id: edge.trace_42-a\r\n", $isKept('edge.trace_42-a')],
    'bad' => ["X-Request-Id: bad id!\r\n", function (string $id) use ($isGenerated): void {
        check($id !== 'bad id!', 'invalid inbound id was not replaced');
        $isGenerated($id);
    }],
]);

/* propagate, no http.trusted_proxies: the client's id is never believed. */
run('untrusted', 'propagate', '', [
    'nokeep' => ["X-Request-Id: edge.trace_42-a\r\n", $isNotClient('edge.trace_42-a')],
]);

@unlink("$docroot/index.php");
@rmdir($docroot);
echo "Done\n";
?>
--EXPECT--
generate/gen: ok
trusted/keep: ok
trusted/bad: ok
untrusted/nokeep: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
