--TEST--
fpm-ng: fiber executor isolates superglobals, sessions, and ini_set under concurrency (docs/frameworks.md, docs/NOTES.md §3t)
--SKIPIF--
<?php
include "skipif.inc";

if (!extension_loaded('session')) {
    die('skip requires the session extension');
}

$binary = getenv('TEST_PHP_FPM_EXECUTABLE') ?: FPM\Tester::findExecutable();
exec(escapeshellarg($binary) . ' -i 2>&1', $output, $status);
if ($status !== 0 || !str_contains(implode("\n", $output), '--enable-fpmng-fiber')) {
    die('skip php-fpm-ng was not built with --enable-fpmng-fiber');
}
?>
--FILE--
<?php

require_once "tester.inc";

function concurrentHttpGet(array $urls): array
{
    $descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $processes = [];
    $pipes = [];
    foreach ($urls as $i => $url) {
        $code = 'echo file_get_contents(' . var_export($url, true) . ');';
        $processes[$i] = proc_open(PHP_BINARY . ' -n -r ' . escapeshellarg($code), $descriptors, $pipes[$i]);
        fclose($pipes[$i][0]);
    }

    $bodies = [];
    foreach ($processes as $i => $proc) {
        $bodies[$i] = stream_get_contents($pipes[$i][1]);
        fclose($pipes[$i][1]);
        fclose($pipes[$i][2]);
        proc_close($proc);
    }

    return $bodies;
}

$docRoot = sys_get_temp_dir() . '/fpmng-fiber-iso-' . getmypid();
@mkdir($docRoot, 0700, true);
$probe = <<<'PHP'
<?php
$id = $_GET['id'] ?? 'missing';
ini_set('display_errors', $id === 'A' ? '1' : '0');
session_start();
if (!isset($_SESSION['owner'])) {
    $_SESSION['owner'] = $id;
}
usleep(300000);
echo json_encode([
    'id' => $id,
    'get' => $_GET['id'] ?? null,
    'session' => $_SESSION['owner'] ?? null,
    'ini' => ini_get('display_errors'),
], JSON_UNESCAPED_SLASHES);
PHP;
file_put_contents("$docRoot/probe.php", $probe);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
chdir = $docRoot
pm = static
pm.max_children = 1
pool.type = http
pool.executor = fiber
http.listen = {{ADDR[http]}}
php_admin_value[opcache.enable] = 0
php_admin_value[session.save_path] = $docRoot/sessions
EOT;

@mkdir("$docRoot/sessions", 0700, true);
$tester = new FPM\Tester($cfg, $probe);
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

$bodies = concurrentHttpGet([
    "http://$http/probe.php?id=A",
    "http://$http/probe.php?id=B",
]);

$decoded = array_map(fn ($body) => json_decode($body, true), $bodies);
foreach ($decoded as $i => $row) {
    if (!is_array($row)) {
        echo "FAIL: response $i not json: " . var_export($bodies[$i], true) . "\n";
        exit(1);
    }
    $expectedId = $i === 0 ? 'A' : 'B';
    if ($row['id'] !== $expectedId || $row['get'] !== $expectedId || $row['session'] !== $expectedId) {
        echo 'FAIL: isolation row ' . $i . '=' . json_encode($row) . "\n";
        exit(1);
    }
    $expectedIni = $expectedId === 'A' ? '1' : '0';
    if ($row['ini'] !== $expectedIni) {
        echo 'FAIL: ini isolation row ' . $i . '=' . json_encode($row) . "\n";
        exit(1);
    }
}
echo "concurrency-isolation: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

function rrmdir(string $dir): void
{
    foreach (scandir($dir) as $entry) {
        if ($entry === '.' || $entry === '..') {
            continue;
        }
        $path = "$dir/$entry";
        is_dir($path) ? rrmdir($path) : unlink($path);
    }
    rmdir($dir);
}
rrmdir($docRoot);

?>
Done
--EXPECT--
concurrency-isolation: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
