--TEST--
fpm-ng: http.proxy_protocol on a TLS gateway reads the header before the handshake (issue #651)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
if (!function_exists('openssl_x509_parse')) {
    die('skip requires the openssl extension');
}
if (trim((string) shell_exec('command -v openssl 2>/dev/null')) === '') {
    die('skip requires the openssl CLI to generate a test certificate');
}
$probe = new FPM\Tester(<<<'EOT'
[global]
error_log = {{FILE:LOG}}
[gw]
pool.type = gateway
listen = {{ADDR[probe]}}
http.tls_cert = /nonexistent-cert.pem
http.tls_key = /nonexistent-key.pem
http.route[unconfined] = /
[unconfined]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
EOT, '<?php');
$messages = $probe->testConfig(true, null, false, false);
FPM\Tester::clean();
foreach ((array) $messages as $message) {
    if (str_contains($message, 'built with TLS support')) {
        die('skip php-fpm-ng built without TLS support (configure without --enable-fpmng-tls, issue #280)');
    }
}
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #651 on a TLS gateway. The PROXY header comes first on the raw socket,
 * then the TLS handshake and the HTTP request. Cases: v1 and v2, the header in
 * its own write and the header in the same write as the ClientHello, a trusted
 * and an untrusted source, and an untrusted source that sends the header and the
 * handshake in two writes. The TLS client is PHP's own; the relay that makes
 * the two write shapes is tlsExchange(). */

$certDir = sys_get_temp_dir() . '/' . basename(__FILE__, '.php') . '-certs';
@mkdir($certDir, 0700, true);
exec("openssl req -x509 -newkey rsa:2048 -nodes -keyout $certDir/gw.key -out $certDir/gw.crt " .
    "-days 2 -subj /CN=gw.test 2>&1", $out, $code);
if ($code !== 0) {
    echo "COMMAND FAILED: openssl req\n" . implode("\n", $out) . "\n";
    exit(1);
}

$docroot = sys_get_temp_dir() . '/fpmng-proxy-tls-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', <<<'PHP'
<?php
echo 'remote=' . ($_SERVER['REMOTE_ADDR'] ?? 'none') . "\n";
PHP);

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
http.plain_listen = {{ADDR[plain]}}
chdir = $docroot
http.gateways = 1
http.front_controller = /index.php
http.tls_cert = $certDir/gw.crt
http.tls_key = $certDir/gw.key
http.proxy_protocol = yes
http.proxy_protocol_trusted = 127.0.0.1
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 2
EOT;

const GET = "GET /index.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";

/* One TLS connection through a relay in this process. The TLS client connects
 * to the relay, and the relay connects to the gateway from $bindto. The relay
 * takes the first bytes the client sends (the ClientHello) and writes them to
 * the gateway together with the header (joined), or writes the header, waits
 * 200 ms, then writes the ClientHello (split). Both sides run non-blocking.
 * Returns the answer the gateway sent, or '' when it closed the connection. */
function tlsExchange(string $gwAddr, string $header, bool $joined, string $bindto = ''): string
{
    $server = stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr);
    if ($server === false) {
        return "RELAY FAILED: $errstr";
    }
    $name = stream_socket_get_name($server, false);
    $ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false, 'allow_self_signed' => true]]);
    $client = stream_socket_client("tcp://$name", $errno, $errstr, 5, STREAM_CLIENT_CONNECT, $ctx);
    $relayIn = $client === false ? false : stream_socket_accept($server, 5);
    fclose($server);
    $uctx = stream_context_create(['socket' => $bindto === '' ? [] : ['bindto' => $bindto]]);
    $relayOut = stream_socket_client("tcp://$gwAddr", $errno, $errstr, 5, STREAM_CLIENT_CONNECT, $uctx);
    if ($client === false || $relayIn === false || $relayOut === false) {
        return "CONNECT FAILED: $errstr";
    }
    stream_set_blocking($client, false);
    stream_set_blocking($relayIn, false);
    stream_set_blocking($relayOut, false);

    $handshake = null;
    $headerSent = false;
    $gatewayClosed = false;
    $resp = '';
    $deadline = microtime(true) + 10;
    while (microtime(true) < $deadline) {
        $busy = false;
        if ($handshake === null) {
            $r = @stream_socket_enable_crypto($client, true, STREAM_CRYPTO_METHOD_TLS_CLIENT);
            if ($r === false || ($r === 0 && $gatewayClosed)) {
                break;
            }
            if ($r === true) {
                $handshake = true;
                fwrite($client, GET);
                $busy = true;
            }
        }
        /* client -> gateway */
        $d = fread($relayIn, 8192);
        if ($d !== false && $d !== '') {
            if (!$headerSent) {
                $headerSent = true;
                if ($joined) {
                    @fwrite($relayOut, $header . $d);
                } else {
                    @fwrite($relayOut, $header);
                    usleep(200000);
                    @fwrite($relayOut, $d);
                }
            } else {
                @fwrite($relayOut, $d);
            }
            $busy = true;
        }
        /* gateway -> client */
        $d = fread($relayOut, 8192);
        if ($d !== false && $d !== '') {
            @fwrite($relayIn, $d);
            $busy = true;
        }
        if (!$gatewayClosed && feof($relayOut)) {
            $gatewayClosed = true;
            stream_socket_shutdown($relayIn, STREAM_SHUT_WR);
        }
        if ($handshake === true) {
            $d = fread($client, 8192);
            if ($d !== false && $d !== '') {
                $resp .= $d;
                $busy = true;
            }
            if (feof($client)) {
                break;
            }
        }
        if (!$busy) {
            usleep(2000);
        }
    }
    fclose($client);
    fclose($relayIn);
    fclose($relayOut);
    return $resp;
}

function remoteOf(string $resp): ?string
{
    return preg_match('/remote=(\S+)/', $resp, $m) === 1 ? $m[1] : null;
}

function check(bool $ok, string $what): void
{
    if (!$ok) {
        echo "FAIL: $what\n";
        exit(1);
    }
}

$v1 = "PROXY TCP4 192.0.2.10 198.51.100.1 56324 443\r\n";
$v2 = "\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A\x21\x11\x00\x0C"
    . pack('N', ip2long('192.0.2.20')) . pack('N', ip2long('198.51.100.1'))
    . pack('n', 40000) . pack('n', 443);

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $tlsAddr = $tester->getAddr('ipv4', '[http]');
    $plainAddr = $tester->getAddr('ipv4', '[plain]');

    $r = tlsExchange($tlsAddr, $v1, false);
    check(remoteOf($r) === '192.0.2.10', "v1, separate write: REMOTE_ADDR is not the header source: $r");
    echo "v1 header, then TLS: ok\n";

    $r = tlsExchange($tlsAddr, $v2, false);
    check(remoteOf($r) === '192.0.2.20', "v2, separate write: REMOTE_ADDR is not the header source: $r");
    echo "v2 header, then TLS: ok\n";

    $r = tlsExchange($tlsAddr, $v1, true);
    check(remoteOf($r) === '192.0.2.10', "v1, joined write: REMOTE_ADDR is not the header source: $r");
    echo "v1 header and ClientHello in one write: ok\n";

    $r = tlsExchange($tlsAddr, $v2, true);
    check(remoteOf($r) === '192.0.2.20', "v2, joined write: REMOTE_ADDR is not the header source: $r");
    echo "v2 header and ClientHello in one write: ok\n";

    /* Untrusted source with a valid header and a joined handshake: closed. */
    $r = tlsExchange($tlsAddr, $v1, true, '127.0.0.2:0');
    check(!str_contains($r, 'remote='), "untrusted source reached the TLS target: $r");
    echo "untrusted source over TLS is closed: ok\n";

    /* Untrusted source: the header first, then the handshake in a second write. */
    $r = tlsExchange($tlsAddr, $v1, false, '127.0.0.2:0');
    check(!str_contains($r, 'remote='), "untrusted second segment reached the TLS target: $r");
    echo "untrusted source with a second segment is closed: ok\n";

    /* Plain listener, measured: the directive is on for the TLS listener only.
     * A PROXY line sent to the plain port is read as a request line. */
    $fp = stream_socket_client("tcp://$plainAddr", $errno, $errstr, 5);
    stream_set_timeout($fp, 5);
    fwrite($fp, $v1 . GET);
    $plain = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 8192);
        if ($chunk === false || $chunk === '') {
            break;
        }
        $plain .= $chunk;
    }
    fclose($fp);
    /* Measured on the build box (issue #651): the plain listener has no PROXY
     * reader, so the line is an invalid request line and gets this answer. */
    echo 'plain listener answer to a PROXY line: ', strtok($plain, "\r\n") ?: 'none', "\n";
    check(!str_contains($plain, 'remote='), "plain listener ran the target for a PROXY line: $plain");
} finally {
    $tester->terminate();
    $tester->close();
}

@unlink("$docroot/index.php");
@rmdir($docroot);
echo "Done\n";
?>
--EXPECT--
v1 header, then TLS: ok
v2 header, then TLS: ok
v1 header and ClientHello in one write: ok
v2 header and ClientHello in one write: ok
untrusted source over TLS is closed: ok
untrusted source with a second segment is closed: ok
plain listener answer to a PROXY line: HTTP/1.1 400 Bad Request
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
