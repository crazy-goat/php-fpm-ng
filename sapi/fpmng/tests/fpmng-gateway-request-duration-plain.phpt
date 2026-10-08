--TEST--
fpm-ng: a request answered by the plain listener is counted once, in the "-" row of the request-duration histogram (issue #652)
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
require_once "fpmng-operator.inc";

/* Issue #652. The plain listener answers every request itself. Two answers are
 * checked here: the redirect to https, which does not reach
 * fpm_http_log_response() and is timed by the plain wrapper, and the ACME
 * HTTP-01 answer, which does reach it. Each must be observed exactly once, in
 * the "-" row: the row its requests_total was counted in. The "web" target gets
 * no request, so its row stays at zero. */

function run(string $cmd): void
{
    exec($cmd . ' 2>&1', $output, $code);
    if ($code !== 0) {
        throw new RuntimeException("COMMAND FAILED: $cmd\n" . implode("\n", $output));
    }
}

function plainStatus(string $addr, string $target): string
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect: $error");
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET $target HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    $raw = stream_get_contents($fp);
    fclose($fp);
    return preg_match('#^HTTP/\S+ (\d+)#', (string) $raw, $m) ? $m[1] : 'none';
}

function sample(string $metrics, string $line): ?int
{
    return preg_match('/^' . preg_quote($line, '/') . ' (\d+)$/m', $metrics, $m) ? (int) $m[1] : null;
}

$root = sys_get_temp_dir() . '/fpmng-gw-duration-plain-' . getmypid();
@mkdir($root, 0700, true);
run("openssl req -x509 -newkey rsa:2048 -nodes -days 2 -sha256 "
    . "-subj /CN=t -keyout $root/tls.key -out $root/tls.crt");
file_put_contents("$root/index.php", '<?php echo "x";');

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
http.front_controller = /index.php
http.tls_cert = $root/tls.crt
http.tls_key = $root/tls.key
http.route[web] = /
http.operator = yes
http.operator_allowed_clients = 127.0.0.1
operator.metrics_listen = {{ADDR[operator]}}
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 1
EOT;

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $plain = $tester->getAddr('ipv4', '[plain]');
    $operator = $tester->getListen('{{ADDR[operator]}}');

    $status = plainStatus($plain, '/x?a=1');
    if ($status !== '308') {
        echo "FAIL: plain listener answered $status, want the 308 redirect\n";
        exit(1);
    }
    echo "plain: 308\n";

    $acme = plainStatus($plain, '/.well-known/acme-challenge/nope');
    if ($acme !== '404') {
        echo "FAIL: plain listener answered the ACME challenge with $acme, want 404 for an unknown token\n";
        exit(1);
    }
    echo "plain acme: 404\n";

    $metrics = fpmng_operator_body($operator, '/metrics');
    $local = sample($metrics, 'fpmng_gateway_requests_total{pool="gw",target="-"}');
    $localInf = sample($metrics, 'fpmng_gateway_request_duration_seconds_bucket{pool="gw",target="-",le="+Inf"}');
    $localCount = sample($metrics, 'fpmng_gateway_request_duration_seconds_count{pool="gw",target="-"}');
    $web = sample($metrics, 'fpmng_gateway_request_duration_seconds_count{pool="gw",target="web"}');
    if ($local !== 2 || $localCount !== 2 || $localInf !== 2) {
        echo "FAIL: '-' row requests_total=" . var_export($local, true)
            . " count=" . var_export($localCount, true)
            . " +Inf=" . var_export($localInf, true) . " (want 2 each)\n$metrics\n";
        exit(1);
    }
    echo "local: requests_total=2, count=2, +Inf=2\n";

    if ($web !== 0) {
        echo "FAIL: web row count=" . var_export($web, true) . " (want 0)\n$metrics\n";
        exit(1);
    }
    echo "web: count=0\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink("$root/index.php");
    @unlink("$root/tls.key");
    @unlink("$root/tls.crt");
    @rmdir($root);
}
?>
--EXPECT--
plain: 308
plain acme: 404
local: requests_total=2, count=2, +Inf=2
web: count=0
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
