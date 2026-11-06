--TEST--
fpm-ng: a gateway whose TLS connection cannot be set up refuses it and never serves it in cleartext (issue #1001)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_no_fault_inject();
fpmng_skip_if_pool_type_unsupported('gateway');
if (!function_exists('openssl_x509_parse')) {
    die('skip requires the openssl extension');
}
if (trim((string) shell_exec('command -v openssl 2>/dev/null')) === '') {
    die('skip requires the openssl CLI to generate a test certificate');
}
/* Same probe as fpmng-http-tls-chain.phpt: a build without TLS refuses
 * http.tls_cert with "built with TLS support" wording. */
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

/* Issue #1001. fpm_tls_http_bevcb() used to return NULL, or a bufferevent
 * from a NULL SSL, when a TLS connection could not be set up. evhttp takes
 * NULL as "no TLS" and builds a plain socket bufferevent, so the TLS port
 * answered a plaintext request with HTTP. Each phase arms one failure point
 * for its first hit only (:times=1). The plaintext request uses that hit, and
 * the TLS request that follows must succeed in the same process: the child
 * keeps running and keeps serving TLS after it refused one connection. */

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

function run(string $cmd): void
{
    exec($cmd . ' 2>&1', $output, $code);
    if ($code !== 0) {
        throw new RuntimeException("COMMAND FAILED: $cmd\n" . implode("\n", $output));
    }
}

function file_wait(string $file, string $pattern, int $seconds = 10): string
{
    $deadline = microtime(true) + $seconds;
    do {
        $content = (string) @file_get_contents($file);
        if (preg_match($pattern, $content)) return $content;
        usleep(100000);
    } while (microtime(true) < $deadline);
    return $content;
}

/* Plaintext to the TLS port: the connection must end with no HTTP answer.
 * `timed_out` tells a closed connection from one that stayed open. */
function plainGet(string $addr, ?bool &$timed_out): string
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    check($fp !== false, "connect to $addr failed: $error");
    stream_set_timeout($fp, 5);
    @fwrite($fp, "GET /index.php HTTP/1.1\r\nHost: tls.test\r\nConnection: close\r\n\r\n");
    $reply = stream_get_contents($fp);
    $timed_out = stream_get_meta_data($fp)['timed_out'];
    fclose($fp);
    return (string) $reply;
}

/* A normal TLS request, verification off: the point is what the server
 * serves, not whether a CA chain validates. */
function tlsGet(string $addr): string
{
    $ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
    $client = @stream_socket_client("ssl://$addr", $errno, $error, 5, STREAM_CLIENT_CONNECT, $ctx);
    check($client !== false, "TLS connect to $addr failed: $error");
    stream_set_timeout($client, 5);
    fwrite($client, "GET /index.php HTTP/1.1\r\nHost: tls.test\r\nConnection: close\r\n\r\n");
    $reply = stream_get_contents($client);
    fclose($client);
    return (string) $reply;
}

$root = sys_get_temp_dir() . '/fpmng-gateway-tls-fail-closed-' . getmypid();
@mkdir($root, 0700, true);
run("openssl req -x509 -newkey rsa:2048 -nodes -days 2 -sha256 "
    . "-subj /CN=fail-closed.test -keyout $root/serving.key -out $root/serving.crt");
file_put_contents("$root/index.php", "<?php echo 'tls-ok';");

function phase(string $root, string $point, string $label): void
{
    $cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
[gw]
pool.type = gateway
listen = {{ADDR[tls]}}
http.gateways = 1
chdir = $root
http.tls_cert = $root/serving.crt
http.tls_key = $root/serving.key
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 1
EOT;

    $tester = new FPM\Tester($cfg);
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false, false, [], [], array_merge(getenv(), [
        'FPMNG_FAULT' => "$point:times=1",
    ]));
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();
    $addr = $tester->getAddr('ipv4', '[tls]');

    $timed_out = false;
    $plain = plainGet($addr, $timed_out);
    check(!$timed_out, "$label: the refused connection stayed open until the read timed out");
    check(!str_contains($plain, 'HTTP/'), "$label: the plaintext request got an HTTP answer: " . var_export($plain, true));
    echo "$label: plaintext refused\n";

    $body = tlsGet($addr);
    check(str_starts_with($body, 'HTTP/1.1 200 OK') && str_contains($body, 'tls-ok'),
        "$label: the next TLS request was not served: " . var_export($body, true));
    echo "$label: next TLS request served\n";

    $errors = file_wait($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR), '/cannot set up a TLS connection/');
    check(substr_count($errors, 'cannot set up a TLS connection') === 1,
        "$label: expected one WARNING for the refused connection:\n$errors");
    check(!preg_match('/killed by signal|respawning/', $errors), "$label: the gateway died:\n$errors");
    echo "$label: warning: ok\n";

    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
}

phase($root, 'tls_ssl_new', 'SSL_new fails');
phase($root, 'tls_bev_new', 'bufferevent_openssl_socket_new fails');

@unlink("$root/serving.key");
@unlink("$root/serving.crt");
@unlink("$root/index.php");
@rmdir($root);
?>
Done
--EXPECT--
SSL_new fails: plaintext refused
SSL_new fails: next TLS request served
SSL_new fails: warning: ok
bufferevent_openssl_socket_new fails: plaintext refused
bufferevent_openssl_socket_new fails: next TLS request served
bufferevent_openssl_socket_new fails: warning: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
