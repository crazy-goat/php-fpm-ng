--TEST--
fpm-ng: cron and supervisor scripts get the CLI argv, argc, PHP_SELF and SCRIPT_FILENAME (issue #738)
--SKIPIF--
<?php include "skipif.inc"; ?>
--ENV--
FPMNG_DEBUG_CLOCK_RATE=10
--FILE--
<?php
/* FPMNG_DEBUG_CLOCK_RATE: the cron pool below fires on a "* * * * *" tick, see
 * fpmng-baseline-counters-cron.phpt. The 75-second deadline stays in real
 * seconds, so a binary without the debug clock still passes. */

require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-cli-vars-' . getmypid();
@mkdir($work, 0700, true);

$cleanup = function () use ($work) {
    foreach (glob("$work/*") as $file) {
        @unlink($file);
    }
    @rmdir($work);
};

/* One script for both pools. It writes what it saw to a file named after the
 * pool (its own PHP_SELF cannot name it, that is the thing under test), and
 * fails soft: before the fix PHP_SELF is not defined and an unguarded read
 * would die with the very warning from the issue. */
file_put_contents("$work/dump.php", <<<'PHP'
<?php
$out = [
    'argv' => $_SERVER['argv'] ?? null,
    'argc' => $_SERVER['argc'] ?? null,
    'global_argv' => $GLOBALS['argv'] ?? null,
    'global_argc' => $GLOBALS['argc'] ?? null,
    'PHP_SELF' => $_SERVER['PHP_SELF'] ?? null,
    'SCRIPT_FILENAME' => $_SERVER['SCRIPT_FILENAME'] ?? null,
    'SCRIPT_NAME' => $_SERVER['SCRIPT_NAME'] ?? null,
];
file_put_contents(__DIR__ . '/' . getenv('FPMNG_TEST_POOL') . '.json', json_encode($out));
PHP);

$script = "$work/dump.php";
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no
[sup]
pool.type = supervisor
supervisor.script = $script
supervisor.processes = 1
supervisor.restart = never
env[FPMNG_TEST_POOL] = sup
[job]
pool.type = cron
cron.schedule = * * * * *
cron.script = $script
env[FPMNG_TEST_POOL] = job
EOT;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start(extraArgs: ['-R'], forceStderr: true, daemonize: false);
    $tester->expectLogStartNotices();

    $deadline = time() + 75;
    foreach (['sup', 'job'] as $pool) {
        $file = "$work/$pool.json";
        while (!is_file($file) && time() < $deadline) {
            usleep(200000);
        }
        if (!is_file($file)) {
            echo "$pool: script did not run\n";
            continue;
        }
        /* The script may still be writing; the file is a few hundred bytes. */
        usleep(200000);
        $seen = json_decode(file_get_contents($file), true);
        $ok = $seen === [
            'argv' => [$script],
            'argc' => 1,
            'global_argv' => [$script],
            'global_argc' => 1,
            'PHP_SELF' => $script,
            'SCRIPT_FILENAME' => $script,
            'SCRIPT_NAME' => $script,
        ];
        echo "$pool: ", $ok ? 'ok' : 'WRONG ' . var_export($seen, true), "\n";
    }
} finally {
    $tester->terminate();
    $tester->close();
    $cleanup();
}
?>
Done
--EXPECT--
sup: ok
job: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-cli-vars-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>
