--TEST--
fpm-ng: HTTP-direct accepts only classic/static and supported HTTP directives
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
$root = __DIR__;
$script = '/fpmng-direct-config-front-' . getmypid() . '.php';
file_put_contents($root . $script, '<?php');
register_shutdown_function(static function () use ($root, $script) { @unlink($root . $script); });
$base = <<<CFG
[global]
error_log = {{FILE:LOG}}
[direct]
listen = {{ADDR}}
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = $script
CFG;
$cases = [
    'dynamic' => [str_replace('pm = static', 'pm = dynamic', $base), 'requires pm = static'],
    'ondemand' => [str_replace('pm = static', 'pm = ondemand', $base), 'requires pm = static'],
    'fiber' => [$base . "\npool.executor = fiber", 'supports only pool.executor = classic'],
    'async' => [$base . "\npool.executor = async", 'supports only pool.executor = classic'],
    'gateway-listen' => [$base . "\nhttp.listen = 127.0.0.1:1", "'http.listen' is not supported"],
    /* http.tls_cert IS supported since issue #55 -- what stays rejected is a
     * half-configured pair, and the second listener a direct pool has nowhere
     * to put. A cert alone must not silently start a pool serving plain HTTP
     * on a port its configuration says is HTTPS. */
    'tls-cert-without-key' => [$base . "\nhttp.tls_cert = /missing.pem", 'http.tls_cert requires http.tls_key'],
    'tls-key-without-cert' => [$base . "\nhttp.tls_key = /missing.pem", 'nothing to attach the key to'],
    'tls-tuning-without-cert' => [$base . "\nhttp.tls_min_version = TLSv1.2", 'require http.tls_cert'],
    'gateway-plain-listen' => [$base . "\nhttp.plain_listen = 127.0.0.1:1", "'http.plain_listen' is not supported"],
    /* issue #58. http.static IS supported on the classic executor, which serves
     * the file in its request callback before any PHP runs. The worker executor
     * never passes through there, so the directive is refused rather than
     * accepted into doing nothing. */
    'worker-static' => [$base . "\npool.executor = worker\nhttp.static = yes", "'http.static' is not supported"],
    'gateway-acl' => [$base . "\nhttp.allowed_clients = 127.0.0.1", "'http.allowed_clients' is not supported"],
    'fastcgi-acl' => [$base . "\nlisten.allowed_clients = 127.0.0.1", "'listen.allowed_clients' is not supported"],
    'chroot' => [$base . "\nchroot = /", "'chroot' is not supported"],
    'traversal' => [$base . "\nhttp.front_controller = /../secret.php", 'requires an absolute chdir'],
    'unbounded-body' => [$base . "\nhttp.max_body = 0", 'http.max_body between 1 and 32M'],
    'unbounded-timeout' => [$base . "\nhttp.read_timeout = 0", 'http.read_timeout > 0'],
    /* issue #56. A streamed response blocks the child on one client, and this
     * executor has exactly one request in flight, so an unbounded wait would
     * take the whole pool down with a single stalled reader. */
    'stream-without-timeout' => [$base . "\nhttp.stream = yes\nhttp.stream_write_timeout = 0", 'http.stream requires http.stream_write_timeout > 0'],
    /* issue #56. The streaming writer reaches past the bufferevent to the
     * descriptor, which on a TLS connection would send plaintext. */
    'stream-with-tls' => [$base . "\nhttp.stream = yes\nhttp.tls_cert = /missing.pem\nhttp.tls_key = /missing.pem", 'http.stream cannot be combined with http.tls_cert'],
    'missing-script' => [$base . "\nhttp.front_controller = /missing-direct-script.php", 'front controller must be a regular file inside chdir'],
];
foreach ($cases as $label => [$config, $needle]) {
    $tester = new FPM\Tester($config, '<?php');
    $messages = $tester->testConfig(true);
    if ($messages === null || !str_contains(implode("\n", $messages), $needle)) {
        throw new RuntimeException("$label: " . var_export($messages, true));
    }
    echo "$label: rejected\n";
}
foreach (['', "\npool.executor = classic", "\nhttp.static = yes", "\nhttp.static = no"] as $extra) {
    $tester = new FPM\Tester($base . $extra, '<?php');
    if ($tester->testConfig() !== null) throw new RuntimeException('classic config failed');
}
echo "classic: accepted\n";
?>
--EXPECT--
dynamic: rejected
ondemand: rejected
fiber: rejected
async: rejected
gateway-listen: rejected
tls-cert-without-key: rejected
tls-key-without-cert: rejected
tls-tuning-without-cert: rejected
gateway-plain-listen: rejected
worker-static: rejected
gateway-acl: rejected
fastcgi-acl: rejected
chroot: rejected
traversal: rejected
unbounded-body: rejected
unbounded-timeout: rejected
stream-without-timeout: rejected
stream-with-tls: rejected
missing-script: rejected
classic: accepted
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
