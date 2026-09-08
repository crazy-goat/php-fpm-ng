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
    'gateway-tls' => [$base . "\nhttp.tls_cert = /missing.pem", "'http.tls_cert' is not supported"],
    'gateway-static' => [$base . "\nhttp.static = 0", "'http.static' is not supported"],
    'gateway-acl' => [$base . "\nhttp.allowed_clients = 127.0.0.1", "'http.allowed_clients' is not supported"],
    'fastcgi-acl' => [$base . "\nlisten.allowed_clients = 127.0.0.1", "'listen.allowed_clients' is not supported"],
    'chroot' => [$base . "\nchroot = /", "'chroot' is not supported"],
    'traversal' => [$base . "\nhttp.front_controller = /../secret.php", 'requires an absolute chdir'],
    'unbounded-body' => [$base . "\nhttp.max_body = 0", 'http.max_body between 1 and 32M'],
    'unbounded-timeout' => [$base . "\nhttp.read_timeout = 0", 'http.read_timeout > 0'],
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
foreach (['', "\npool.executor = classic"] as $executor) {
    $tester = new FPM\Tester($base . $executor, '<?php');
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
gateway-tls: rejected
gateway-static: rejected
gateway-acl: rejected
fastcgi-acl: rejected
chroot: rejected
traversal: rejected
unbounded-body: rejected
unbounded-timeout: rejected
missing-script: rejected
classic: accepted
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
