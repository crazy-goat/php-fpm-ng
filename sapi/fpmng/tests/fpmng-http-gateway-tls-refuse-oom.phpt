--TEST--
fpm-ng: a gateway whose TLS connection cannot be set up and cannot be refused exits, and serves no cleartext (issue #1001)
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

/* Issue #1001. SSL_new() fails, so fpm_tls_http_bevcb() tries to refuse the
 * connection with a bufferevent_pair. tls_refuse_pair makes that allocation
 * fail too. The child cannot refuse the connection without serving it in
 * cleartext, so it exits with FPM_EXIT_SOFTWARE (70). The plaintext client
 * must get no bytes at all, and the master must log the exit. */

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

/* Plaintext to the TLS port. `timed_out` tells a closed connection from one
 * that stayed open. */
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

$root = sys_get_temp_dir() . '/fpmng-gateway-tls-refuse-oom-' . getmypid();
@mkdir($root, 0700, true);
run("openssl req -x509 -newkey rsa:2048 -nodes -days 2 -sha256 "
    . "-subj /CN=refuse-oom.test -keyout $root/serving.key -out $root/serving.crt");
file_put_contents("$root/index.php", "<?php echo 'tls-ok';");

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
    'FPMNG_FAULT' => 'tls_ssl_new,tls_refuse_pair',
]));
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();
$addr = $tester->getAddr('ipv4', '[tls]');

$timed_out = false;
$plain = plainGet($addr, $timed_out);
check(!$timed_out, "the connection stayed open until the read timed out");
check($plain === '', "the plaintext request got bytes: " . var_export($plain, true));
echo "plaintext got no bytes\n";

$errors = file_wait($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR), '/exited with code 70/');
check(str_contains($errors, 'tls: cannot refuse a TLS connection (out of memory)'),
    "expected the refusal ERROR line:\n$errors");
check(preg_match('/http gateway \d+ \(pid \d+\) exited with code 70/', $errors) === 1,
    "expected the gateway to exit with code 70:\n$errors");
echo "exit 70 logged\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink("$root/serving.key");
@unlink("$root/serving.crt");
@unlink("$root/index.php");
@rmdir($root);
?>
Done
--EXPECT--
plaintext got no bytes
exit 70 logged
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
