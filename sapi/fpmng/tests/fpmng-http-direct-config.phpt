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
    /* http.allowed_clients is the GATEWAY's directive and stays refused; the
     * pool-level listen.allowed_clients is supported since issue #59 and is
     * among the accepted cases below. */
    'gateway-acl' => [$base . "\nhttp.allowed_clients = 127.0.0.1", "'http.allowed_clients' is not supported"],
    /* issue #59. The classic executor answers ping, status and access.log from
     * the per-request scoreboard slot it maintains; the worker executor keeps
     * the same slot untouched for the whole life of the child, so there the
     * directives are refused rather than answered with placeholders. */
    'worker-ping' => [$base . "\npool.executor = worker\nping.path = /ping", "'ping.path' is not supported"],
    'worker-status' => [$base . "\npool.executor = worker\npm.status_path = /status", "'pm.status_path' is not supported"],
    'worker-access-log' => [$base . "\npool.executor = worker\naccess.log = /dev/null", "'access.log' is not supported"],
    /* Refused for both: it asks for a second listening socket served by a
     * second process, and a direct child owns exactly one listener. */
    'status-listen' => [$base . "\npm.status_listen = 127.0.0.1:1", "'pm.status_listen' is not supported"],
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
/* issue #59: the operator directives a fastcgi pool has always had. chroot is
 * validated here (the front controller is resolved inside it, as the child
 * will see it) even though chroot(2) itself needs root at run time, which is
 * why there is no chroot .phpt that actually starts such a pool. */
foreach (['', "\npool.executor = classic", "\nhttp.static = yes", "\nhttp.static = no",
          "\nlisten.allowed_clients = 127.0.0.1",
          "\nping.path = /ping\nping.response = alive",
          "\npm.status_path = /status",
          "\naccess.log = /dev/null",
          "\naccess.log = /dev/null\naccess.format = %R %m %r %s\naccess.suppress_path[] = /ping",
          "\nchroot = /"] as $extra) {
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
worker-ping: rejected
worker-status: rejected
worker-access-log: rejected
status-listen: rejected
traversal: rejected
unbounded-body: rejected
unbounded-timeout: rejected
stream-without-timeout: rejected
stream-with-tls: rejected
missing-script: rejected
classic: accepted
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
