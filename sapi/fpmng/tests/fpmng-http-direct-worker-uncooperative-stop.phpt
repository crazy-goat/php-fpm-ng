--TEST--
fpm-ng: a worker script that ignores fpmng_worker_stopping is bounded by the master's reload escalation (issue #365)
--SKIPIF--
<?php include "skipif.inc"; ?>
--ENV--
TEST_TIMEOUT=30
--FILE--
<?php
require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-worker-uncooperative-stop-' . getmypid();
@mkdir($work, 0700, true);
$marker = "$work/worker.pid";
$script = <<<'PHP'
<?php
file_put_contents('PID_MARKER', (string) getmypid());
/* Deliberately never calls fpmng_worker_stopping()/may_exit() and never pumps
 * the notification pipe. SIGQUIT sets the cooperative flag, but this script
 * keeps running until the master escalates according to process_control_timeout. */
while (true) {
    usleep(100000);
}
PHP;
$script = str_replace('PID_MARKER', $marker, $script);
file_put_contents("$work/worker.php", $script);

$port = (int) (getenv('FPMNG_WORKER_UNCOOPERATIVE_PORT') ?: 28165);
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no
process_control_timeout = 2s
[worker]
listen = 127.0.0.1:$port
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $work
http.front_controller = /worker.php
php_admin_value[max_execution_time] = 0
EOT;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $deadline = microtime(true) + 10;
    do {
        $oldPid = is_file($marker) ? (int) file_get_contents($marker) : 0;
        if ($oldPid > 0) {
            break;
        }
        usleep(10000);
    } while (microtime(true) < $deadline);
    if ($oldPid <= 0) {
        throw new RuntimeException('uncooperative worker did not start');
    }

    $started = microtime(true);
    $tester->reload();
    do {
        usleep(10000);
        $newPid = is_file($marker) ? (int) file_get_contents($marker) : 0;
        if ($newPid > 0 && $newPid !== $oldPid) {
            break;
        }
    } while (microtime(true) < $started + 8);
    $elapsed = microtime(true) - $started;

    if ($newPid <= 0 || $newPid === $oldPid) {
        throw new RuntimeException('reload did not replace the uncooperative worker within 8 seconds');
    }
    echo "uncooperative worker replaced: ok\n";
    if ($elapsed < 1.5 || $elapsed > 6.0) {
        throw new RuntimeException("reload elapsed $elapsed seconds; expected the 2s master grace plus bounded escalation");
    }
    echo "bounded by process_control_timeout: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($marker);
    @unlink("$work/worker.php");
    @rmdir($work);
}
?>
--EXPECT--
uncooperative worker replaced: ok
bounded by process_control_timeout: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-worker-uncooperative-stop-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>
