--TEST--
fpm-ng: supervisor heartbeat is independent per child and reported with PID in status JSON (issue #356)
--SKIPIF--
<?php include "skipif.inc"; ?>
--ENV--
TEST_TIMEOUT=90
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

$work = sys_get_temp_dir() . '/fpmng-supervisor-heartbeat-child-' . getmypid();
@mkdir($work, 0700, true);
$claim = "$work/first-child";
$beatingClaim = "$work/beating-child";
$silentPidFile = "$work/silent.pid";
$beatingPidFile = "$work/beating.pid";
$latePidFile = "$work/late.pid";

$script = <<<'PHP'
<?php
$claim = fopen('CLAIM_PATH', 'x');
if ($claim !== false) {
    fclose($claim);
    file_put_contents('SILENT_PID_PATH', getmypid());
    fpmng_supervisor_heartbeat();
    sleep(30);
}
$claim = fopen('BEATING_CLAIM_PATH', 'x');
if ($claim !== false) {
    fclose($claim);
    file_put_contents('BEATING_PID_PATH', getmypid());
    while (true) {
        fpmng_supervisor_heartbeat();
        usleep(200000);
    }
}
file_put_contents('LATE_PID_PATH', getmypid());
sleep(5);
while (true) {
    fpmng_supervisor_heartbeat();
    usleep(200000);
}
PHP;
$script = str_replace(
    ['BEATING_CLAIM_PATH', 'SILENT_PID_PATH', 'BEATING_PID_PATH', 'LATE_PID_PATH', 'CLAIM_PATH'],
    [$beatingClaim, $silentPidFile, $beatingPidFile, $latePidFile, $claim],
    $script
);
file_put_contents("$work/job.php", $script);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no
[job]
pool.type = supervisor
supervisor.script = $work/job.php
supervisor.processes = 3
supervisor.restart = never
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /job-status
EOT;

$tester = new FPM\Tester($cfg, '<?php');
try {
    /* The operator port is deterministic in Tester; wait for a prior test's
     * master to release it before asking this master to bind. */
    [$operatorHost, $operatorPort] = explode(':', $tester->getListen('{{ADDR[operator]}}'));
    $waitUntil = time() + 30;
    do {
        $probe = @stream_socket_server("tcp://$operatorHost:$operatorPort", $errno, $errstr);
        if ($probe !== false) {
            fclose($probe);
            break;
        }
        usleep(200000);
    } while (time() < $waitUntil);

    $tester->start(extraArgs: ['-R'], forceStderr: true, daemonize: false);
    $tester->expectLogStartNotices();
    $operator = $tester->getListen('{{ADDR[operator]}}');

    $deadline = time() + 20;
    do {
        usleep(100000);
        $row = json_decode(fpmng_operator_body($operator, '/job-status'), true, flags: JSON_THROW_ON_ERROR)['pools'][0];
    } while ((!is_file($silentPidFile) || !is_file($beatingPidFile) || !is_file($latePidFile)
            || !isset($row['heartbeat_children']) || count($row['heartbeat_children']) !== 3)
        && time() < $deadline);

    if (!is_file($silentPidFile) || !is_file($beatingPidFile) || !is_file($latePidFile)
        || !isset($row['heartbeat_children']) || count($row['heartbeat_children']) !== 3) {
        throw new RuntimeException('two running heartbeat children not reported: ' . var_export($row, true));
    }

    $silentPid = (int) file_get_contents($silentPidFile);
    $beatingPid = (int) file_get_contents($beatingPidFile);
    $latePid = (int) file_get_contents($latePidFile);
    $byPid = [];
    foreach ($row['heartbeat_children'] as $child) {
        $byPid[$child['pid']] = $child;
    }
    if (!isset($byPid[$silentPid], $byPid[$beatingPid], $byPid[$latePid])) {
        throw new RuntimeException('status child PIDs do not match the running scripts: '
            . var_export($row['heartbeat_children'], true));
    }
    echo "status reports each child's PID: ok\n";
    if ($byPid[$latePid]['heartbeat_age'] !== null) {
        throw new RuntimeException('child without a heartbeat must report null: '
            . var_export($byPid[$latePid], true));
    }
    echo "child with no heartbeat is explicit: ok\n";

    $silentAgeBefore = $byPid[$silentPid]['heartbeat_age'];
    $maxBeatingAge = 0;
    $sampleUntil = time() + 6;
    while (time() < $sampleUntil) {
        usleep(250000);
        $row = json_decode(fpmng_operator_body($operator, '/job-status'), true, flags: JSON_THROW_ON_ERROR)['pools'][0];
        $byPid = [];
        foreach ($row['heartbeat_children'] as $child) {
            $byPid[$child['pid']] = $child;
        }
        if (!isset($byPid[$silentPid], $byPid[$beatingPid])) {
            throw new RuntimeException('child PID disappeared from status: ' . var_export($row, true));
        }
        $maxBeatingAge = max($maxBeatingAge, $byPid[$beatingPid]['heartbeat_age']);
    }

    $silentAgeAfter = $byPid[$silentPid]['heartbeat_age'];
    if ($maxBeatingAge > 5) {
        throw new RuntimeException('active child heartbeat grew stale: ' . $maxBeatingAge);
    }
    if ($silentAgeAfter < $silentAgeBefore + 4) {
        throw new RuntimeException("silent child heartbeat did not age independently: before=$silentAgeBefore after=$silentAgeAfter");
    }
    if (!isset($row['heartbeat_age']) || $row['heartbeat_age'] > 5) {
        throw new RuntimeException('backward-compatible pool heartbeat_age is not the freshest child: '
            . var_export($row['heartbeat_age'] ?? null, true));
    }
    if (!is_int($byPid[$latePid]['heartbeat_age']) || $byPid[$latePid]['heartbeat_age'] > 5) {
        throw new RuntimeException('child heartbeat did not become available after its first call: '
            . var_export($byPid[$latePid], true));
    }
    echo "sibling heartbeat does not refresh the silent child: ok\n";
    echo "child heartbeat appears after its first call: ok\n";
    echo "pool heartbeat remains the freshest child: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$work/job.php");
    @unlink($claim);
    @unlink($beatingClaim);
    @unlink($silentPidFile);
    @unlink($beatingPidFile);
    @unlink($latePidFile);
    @rmdir($work);
}
?>
--EXPECT--
status reports each child's PID: ok
child with no heartbeat is explicit: ok
sibling heartbeat does not refresh the silent child: ok
child heartbeat appears after its first call: ok
pool heartbeat remains the freshest child: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-supervisor-heartbeat-child-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>
