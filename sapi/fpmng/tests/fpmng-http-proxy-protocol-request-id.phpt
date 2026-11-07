--TEST--
fpm-ng: http.request_id propagate trusts an inbound id by the PROXY source, not the socket peer (issue #651)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #651, review R2. With http.proxy_protocol, the X-Forwarded-For and the
 * http.trusted_proxies checks judge the PROXY source. The X-Request-Id trust
 * must judge the same address. The socket peer here is 127.0.0.1, which is
 * NOT in http.trusted_proxies; the PROXY source 192.0.2.10 IS. So a client
 * that connects through 192.0.2.10 keeps its id, and one behind 192.0.2.11
 * does not. Before the fix the socket peer decided, and the first case got a
 * generated id. */
$docroot = sys_get_temp_dir() . '/fpmng-proxy-reqid-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', <<<'PHP'
<?php
echo 'ok';
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
http.proxy_protocol = yes
http.proxy_protocol_trusted = 127.0.0.1
http.trusted_proxies = 192.0.2.10
http.request_id = propagate
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 2
EOT;

const INBOUND = '5b1e0c7a9d3f4e2b8a6c1f0e9d7b3a25';
const SPOOF = 'deadbeefdeadbeefdeadbeefdeadbeef';

function exchange(string $addr, string $bytes): string
{
    $fp = @stream_socket_client("tcp://$addr", $errno, $errstr, 5);
    if ($fp === false) {
        return "CONNECT FAILED: $errstr";
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, $bytes);
    $resp = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 8192);
        if ($chunk === false || $chunk === '') {
            break;
        }
        $resp .= $chunk;
    }
    fclose($fp);
    return $resp;
}

/* The X-Request-Id response header, or null when there is none. */
function responseId(string $resp): ?string
{
    return preg_match('/^X-Request-Id: ([^\r\n]+)/mi', $resp, $m) === 1 ? $m[1] : null;
}

function getWithId(string $id): string
{
    return "GET /index.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\nX-Request-Id: $id\r\n\r\n";
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $addr = $tester->getAddr('ipv4', '[http]');

    /* PROXY source 192.0.2.10 is a trusted proxy: the inbound id is kept. */
    $r = exchange($addr, "PROXY TCP4 192.0.2.10 198.51.100.1 56324 443\r\n" . getWithId(INBOUND));
    if (responseId($r) !== INBOUND) {
        echo "FAIL: trusted PROXY source did not keep its X-Request-Id: " . var_export(responseId($r), true) . "\n$r\n";
        exit(1);
    }
    echo "inbound id kept for a trusted PROXY source: ok\n";

    /* PROXY source 192.0.2.11 is not a trusted proxy: the inbound id is replaced. */
    $r = exchange($addr, "PROXY TCP4 192.0.2.11 198.51.100.1 56324 443\r\n" . getWithId(SPOOF));
    $id = responseId($r);
    if ($id === null || $id === SPOOF) {
        echo "FAIL: untrusted PROXY source kept its X-Request-Id: " . var_export($id, true) . "\n$r\n";
        exit(1);
    }
    echo "inbound id replaced for an untrusted PROXY source: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
}

@unlink("$docroot/index.php");
@rmdir($docroot);
echo "Done\n";
?>
--EXPECT--
inbound id kept for a trusted PROXY source: ok
inbound id replaced for an untrusted PROXY source: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
