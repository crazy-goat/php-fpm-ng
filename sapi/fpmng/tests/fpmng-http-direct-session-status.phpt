--TEST--
fpm-ng: HTTP-direct runs session RINIT/RSHUTDOWN per request and updates the FPM scoreboard
--SKIPIF--
<?php
include "skipif.inc";
if (!extension_loaded('session')) die('skip requires session');
?>
--FILE--
<?php
require_once "tester.inc";
$root = __DIR__;
$script = '/fpmng-http-direct-session-front-' . getmypid() . '.php';
$sessionDir = sys_get_temp_dir() . '/fpmng-direct-session-' . getmypid();
mkdir($sessionDir, 0700);
file_put_contents($root . $script, '<?php $_SESSION["count"] = ($_SESSION["count"] ?? 0) + 1; echo session_id(), ":", $_SESSION["count"];');
$port = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054) + 2;
$statusPort = $port + 1;
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = $script
php_admin_value[session.auto_start] = 1
php_admin_value[session.save_path] = $sessionDir
[monitor]
pool.type = status
listen = 127.0.0.1:$statusPort
CFG;
$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    foreach (['alpha' => [1, 2], 'beta' => [1, 2]] as $session => $counts) {
        foreach ($counts as $count) {
            $context = stream_context_create(['http' => ['header' => "Cookie: PHPSESSID=$session\r\n", 'timeout' => 5]]);
            $body = file_get_contents("http://127.0.0.1:$port/", false, $context);
            if ($body !== "$session:$count") throw new RuntimeException("session: $body");
        }
    }
    $data = json_decode(file_get_contents("http://127.0.0.1:$statusPort/status"), true, flags: JSON_THROW_ON_ERROR);
    $direct = array_values(array_filter($data['pools'], fn($pool) => $pool['name'] === 'direct'))[0];
    if ($direct['requests'] !== 4 || $direct['active'] !== 0 || $direct['idle'] !== 1) {
        throw new RuntimeException('scoreboard: ' . json_encode($direct));
    }
    echo "session-lifecycle/scoreboard: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    unlink($root . $script);
    foreach (glob($sessionDir . '/sess_*') as $path) unlink($path);
    rmdir($sessionDir);
}
?>
--EXPECT--
session-lifecycle/scoreboard: ok
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
