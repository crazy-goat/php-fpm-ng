--TEST--
FPM http gateway: a silent or trickling client on http.plain_listen is cut off at http.read_timeout (issue #593)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_no_acme();
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
listen = {{ADDR[http]}}
chdir = /tmp
http.tls_cert = /nonexistent-cert.pem
http.tls_key = /nonexistent-key.pem
http.route[unconfined] = /
[unconfined]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = /tmp
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

/* The plain companion of a TLS pool is an unauthenticated listener. Before #593
 * a client could connect to it and send nothing, forever. Both cases must now end
 * about http.read_timeout (1500 ms) after accept, and a normal request must
 * still be answered. */

function run(string $cmd): void
{
    exec($cmd . ' 2>&1', $output, $code);
    if ($code !== 0) {
        echo "FAIL: $cmd\n" . implode("\n", $output) . "\n";
        exit(1);
    }
}

function waitClosed($fp, float $max, ?callable $trickle = null): ?float
{
    $start = microtime(true);
    stream_set_blocking($fp, false);
    while (microtime(true) - $start < $max) {
        $r = [$fp];
        $w = $e = null;
        if (stream_select($r, $w, $e, 0, 100000) > 0) {
            $data = @fread($fp, 8192);
            if ($data === '' || $data === false) {
                return microtime(true) - $start;
            }
        }
        if ($trickle) {
            $trickle();
        }
    }
    return null;
}

$root = sys_get_temp_dir() . '/fpmng-plain-limits-' . getmypid();
@mkdir($root, 0700, true);
run("openssl req -x509 -newkey rsa:2048 -nodes -days 2 -sha256 "
    . "-subj /CN=plain.test -keyout $root/tls.key -out $root/tls.crt");

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[https]}}
chdir = $root
http.plain_listen = {{ADDR[plain]}}
http.gateways = 1
http.read_timeout = 1500
http.tls_cert = $root/tls.crt
http.tls_key = $root/tls.key
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 1
EOT;

$tester = new FPM\Tester($cfg, '<?php echo "x";');
$tester->start();
$tester->expectLogStartNotices();
$plain = $tester->getAddr('ipv4', '[plain]');

/* A normal request is still answered (a redirect). */
$fp = stream_socket_client("tcp://$plain", $errno, $error, 5);
fwrite($fp, "GET /x HTTP/1.1\r\nHost: plain.test\r\nConnection: close\r\n\r\n");
stream_set_timeout($fp, 5);
$line = fgets($fp);
fclose($fp);
if (!is_string($line) || !str_contains($line, ' 308 ')) {
    echo "FAIL: normal plain request: " . var_export($line, true) . "\n";
    exit(1);
}
echo "normal: ok\n";

/* [silent] */
$fp = stream_socket_client("tcp://$plain", $errno, $error, 5);
$t = waitClosed($fp, 12);
fclose($fp);
if ($t === null || $t < 1.0 || $t > 3.0) {
    echo "FAIL: [silent] closed after " . var_export($t, true) . " s, want about 1.5\n";
    exit(1);
}
echo "silent: closed\n";

/* [trickle] */
$fp = stream_socket_client("tcp://$plain", $errno, $error, 5);
$req = "GET /x HTTP/1.1\r\nHost: plain.test\r\nX-Slow: 1\r\n\r\n";
$sent = 0;
$trickle = function () use (&$sent, $fp, $req): void {
    static $last = 0.0;
    if (microtime(true) - $last >= 0.2 && $sent < strlen($req) - 4) {
        if (@fwrite($fp, $req[$sent]) === 1) {
            $sent++;
        }
        $last = microtime(true);
    }
};
$t = waitClosed($fp, 12, $trickle);
fclose($fp);
if ($t === null || $t < 1.0 || $t > 3.0) {
    echo "FAIL: [trickle] closed after " . var_export($t, true) . " s, want about 1.5\n";
    exit(1);
}
echo "trickle: closed\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
normal: ok
silent: closed
trickle: closed
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
@array_map('unlink', glob(sys_get_temp_dir() . '/fpmng-plain-limits-*/*'));
foreach (glob(sys_get_temp_dir() . '/fpmng-plain-limits-*') as $d) { @rmdir($d); }
?>
