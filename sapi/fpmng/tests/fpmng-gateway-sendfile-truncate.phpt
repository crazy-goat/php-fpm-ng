--TEST--
fpm-ng: a file truncated during an X-Accel-Redirect transfer ends the reply cut, and the gateway keeps answering, over plain HTTP and TLS (#649)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
fpmng_skip_if_pool_type_unsupported('gateway');
if (trim((string) shell_exec('command -v openssl')) === '') die('skip needs the openssl command');
$probe = new FPM\Tester(<<<'EOT'
[global]
error_log = {{FILE:LOG}}
[gw]
pool.type = gateway
listen = {{ADDR[probe]}}
http.tls_cert = /nonexistent-cert.pem
http.tls_key = /nonexistent-key.pem
http.tls_wait_for_cert = yes
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

/* The gateway reads a served file with pread(), one window at a time, and never
 * maps it. The file is truncated after the client has read CUT_AFTER bytes.
 * The next pread() gets EOF before the length in Content-Length, so the reply
 * is cut (the client gets a short body and the connection is shut down), the
 * gateway logs a warning and keeps running.
 *
 * The case that guards the no-mapping rule is the TLS one. A mapped file would
 * raise SIGBUS on the first page past the new end, and the 97a5d0c build was
 * killed by signal 7 on TLS only. The plain HTTP case runs too, as the base
 * line: the cut and the log line are the same on both transports. */
const FILE_SIZE = 64 * 1024 * 1024;
const CUT_AFTER = 4 * 1024 * 1024;

$base = sys_get_temp_dir() . '/fpmng-sendfile-trunc-' . getmypid();
$root = $base . '/protected';
@mkdir($root, 0777, true);
$big = $root . '/big.bin';

file_put_contents($root . '/small.txt', "hello sendfile\n");
file_put_contents($base . '/send.php', '<?php
header("X-Accel-Redirect: " . $_GET["r"]);
echo "worker body";
');

/* Self-signed: the point is the cut, not the chain. The client below does not
 * verify the peer. */
exec("openssl req -x509 -newkey rsa:2048 -nodes -days 2 -sha256 -subj /CN=trunc.test "
    . "-keyout $base/tls.key -out $base/tls.crt 2>/dev/null", $out, $code);
if ($code !== 0) {
    throw new RuntimeException('openssl could not make the test certificate');
}

/* The gateway process whose parent is $master, via the setproctitle line
 * fpm_http_gateway_run() installs. 0 when none exists. */
function gateway_pid(int $master): int
{
    foreach (glob('/proc/[0-9]*/stat') as $stat) {
        $raw = @file_get_contents($stat);
        if ($raw === false || !preg_match('/^(\d+) \((.*)\) \S (\d+) /s', $raw, $m)) {
            continue;
        }
        if ((int) $m[3] !== $master) {
            continue;
        }
        $cmd = @file_get_contents(dirname($stat) . '/cmdline');
        if ($cmd !== false && str_contains($cmd, 'http gateway gw')) {
            return (int) $m[1];
        }
    }
    return 0;
}

function truncationCase(string $label, bool $tls, string $base, string $root): void
{
    $big = $root . '/big.bin';
    $fh = fopen($big, 'wb');
    $block = str_repeat('t', 1024 * 1024);
    for ($i = 0; $i < FILE_SIZE / (1024 * 1024); $i++) {
        fwrite($fh, $block);
    }
    fclose($fh);

    $tlsLines = $tls ? "http.tls_cert = $base/tls.crt\nhttp.tls_key = $base/tls.key\n" : '';
    $cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $base
http.gateways = 1
http.front_controller = /send.php
http.sendfile_root = $root
{$tlsLines}http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $base
pm = static
pm.max_children = 1
EOT;

    $tester = new FPM\Tester($cfg, '<?php');
    try {
        $tester->start();
        $tester->expectLogStartNotices();
        $master = $tester->getPid();
        $http = $tester->getAddr('ipv4', '[http]');
        $host = preg_replace('/:\d+$/', '', $http);
        $port = (int) substr($http, strrpos($http, ':') + 1);

        $gw = 0;
        for ($i = 0; $i < 100 && $gw === 0; $i++) {
            $gw = gateway_pid($master);
            usleep(20000);
        }
        if ($gw === 0) {
            throw new RuntimeException("$label: gateway process not found");
        }

        $sock = fsockopen($host, $port, $errno, $errstr, 5);
        if ($sock === false) {
            throw new RuntimeException("$label: connect failed: $errstr ($errno)");
        }
        stream_set_timeout($sock, 10);
        if ($tls) {
            stream_context_set_options($sock, ['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
            if (stream_socket_enable_crypto($sock, true, STREAM_CRYPTO_METHOD_TLS_CLIENT) !== true) {
                throw new RuntimeException("$label: TLS handshake failed");
            }
        }
        fwrite($sock, "GET /?r=%2Fbig.bin HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");

        $head = '';
        while (($line = fgets($sock)) !== false && $line !== "\r\n") {
            $head .= $line;
        }
        if (!str_starts_with($head, 'HTTP/1.1 200 ')) {
            throw new RuntimeException("$label: unexpected status line: " . strtok($head, "\r\n"));
        }
        if (!preg_match('/^Content-Length: ' . FILE_SIZE . '\r$/mi', $head)) {
            throw new RuntimeException("$label: Content-Length is not the file size");
        }

        $got = 0;
        while ($got < CUT_AFTER) {
            $chunk = fread($sock, 64 * 1024);
            if ($chunk === false || $chunk === '') {
                throw new RuntimeException("$label: transfer ended at $got bytes before the truncation");
            }
            $got += strlen($chunk);
        }
        /* Truncate the file under the transfer. Most of it is still to be read. */
        $fh = fopen($big, 'r+');
        ftruncate($fh, 1024);
        fclose($fh);

        while (($chunk = fread($sock, 64 * 1024)) !== false && $chunk !== '') {
            $got += strlen($chunk);
        }
        fclose($sock);

        if ($got >= FILE_SIZE) {
            throw new RuntimeException("$label: the body was not cut: $got bytes");
        }
        if (gateway_pid($master) !== $gw) {
            throw new RuntimeException("$label: the gateway process did not survive the truncation");
        }

        $scheme = $tls ? 'https' : 'http';
        $ctx = stream_context_create(['http' => ['timeout' => 5, 'ignore_errors' => true],
            'ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
        $body = @file_get_contents("$scheme://$http/?r=%2Fsmall.txt", false, $ctx);
        $headers = $http_response_header ?? [];
        if (!str_starts_with($headers[0] ?? '', 'HTTP/1.1 200 ') || $body !== "hello sendfile\n") {
            throw new RuntimeException("$label: no answer after the truncation: " . ($headers[0] ?? 'none'));
        }

        $tester->expectLogPattern('/cannot read the rest of .* the reply is cut short/');
    } finally {
        $tester->terminate();
        $tester->close();
        @unlink($big);
    }
}

try {
    truncationCase('plain HTTP', false, $base, $root);
    echo "truncated transfer over plain HTTP: cut, gateway alive, next request answered: ok\n";
    truncationCase('TLS', true, $base, $root);
    echo "truncated transfer over TLS: cut, gateway alive, next request answered: ok\n";
    echo "Done\n";
} finally {
    @unlink($root . '/small.txt');
    @unlink($base . '/send.php');
    @unlink($base . '/tls.key');
    @unlink($base . '/tls.crt');
    @rmdir($root);
    @rmdir($base);
}
?>
--EXPECT--
truncated transfer over plain HTTP: cut, gateway alive, next request answered: ok
truncated transfer over TLS: cut, gateway alive, next request answered: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
