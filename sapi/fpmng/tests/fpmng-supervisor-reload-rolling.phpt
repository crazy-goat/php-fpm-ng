--TEST--
fpm-ng: supervisor keeps at least one copy of the script running across a reload (issue #329)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-sup-roll-' . getmypid();
@mkdir($work, 0700, true);
$aliveFile = "$work/alive.log";
@unlink($aliveFile);

$cleanup = function () use ($work, $aliveFile) {
    @unlink($aliveFile);
    @unlink("$work/loop.php");
    @rmdir($work);
};

/* One append per iteration, "pid microtime\n" — small enough to stay well
 * under PIPE_BUF so concurrent writers from different processes never
 * interleave mid-line (the same atomicity argument fpmng-supervisor-restart.phpt
 * relies on for its own append-only file, see its own comment). A short sleep
 * between writes rather than a tight loop: this only needs to prove the gap
 * between two live copies never exceeds a bound generous enough to catch a
 * real "zero copies" window, not to maximize write rate. */
$script = <<<PHP
<?php
error_reporting(0);
for (;;) {
    @file_put_contents('{$aliveFile}', getmypid() . ' ' . microtime(true) . "\\n", FILE_APPEND);
    usleep(20000);
}
PHP;
file_put_contents("$work/loop.php", $script);

$cfgBefore = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
[sup]
pool.type = supervisor
supervisor.script = $work/loop.php
supervisor.processes = 3
supervisor.restart = always
supervisor.restart_delay = 1
EOT;

/* issue #329 scope (a): supervisor.processes changes on a reload of an
 * otherwise-unchanged pool. 3 -> 2 exercises fpm_pool_supervisor_reload_spare_child()
 * (wp->running_children >= 2 at the time it is called) and, once the new
 * generation's first start is confirmed, retirement back down to steady
 * state of exactly 2. */
$cfgAfter = str_replace('supervisor.processes = 3', 'supervisor.processes = 2', $cfgBefore);

$tester = new FPM\Tester($cfgBefore, $script);
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));

$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

/* Read every "pid timestamp" line currently in the file. Returns [count of
 * distinct pids seen, latest timestamp seen] — used both to detect
 * "pool fully up" before reloading and, after the run, to find the largest
 * gap between two consecutive writes across the whole timeline. */
$readAll = function () use ($aliveFile) {
    $data = @file_get_contents($aliveFile);
    $rows = [];
    if (is_string($data)) {
        foreach (explode("\n", $data) as $line) {
            $line = trim($line);
            if ($line === '') {
                continue;
            }
            $parts = explode(' ', $line, 2);
            if (count($parts) !== 2) {
                continue; /* a torn read of the very last, in-progress line */
            }
            $rows[] = [(int) $parts[0], (float) $parts[1]];
        }
    }
    return $rows;
};

/* Wait for the pool to be fully up (3 distinct pids observed) before
 * reloading — reloading while the pool is still starting would not be
 * testing what this test is for. */
$deadline = time() + 15;
$pidsSeen = [];
while (time() < $deadline) {
    foreach ($readAll() as [$pid, $ts]) {
        $pidsSeen[$pid] = true;
    }
    if (count($pidsSeen) >= 3) {
        break;
    }
    usleep(100000);
}

if (count($pidsSeen) < 3) {
    echo "FAIL: only " . count($pidsSeen) . " distinct pids observed before reload (wanted 3)\n";
    $tester->close(true);
    $cleanup();
    exit(1);
}

/* Trigger the reload, then keep polling the alive file for a bounded window
 * that comfortably covers the whole reload (execvp() + new children forking
 * and reaching their first write) plus the survivor's retirement afterwards.
 * Poll-based, not sleep-and-hope: every iteration re-reads the file, so the
 * loop only runs as long as needed up to the deadline rather than assuming a
 * fixed wait is enough. */
$tester->reload($cfgAfter);

/* Consume the reload's own NOTICE lines (including the new "sparing"/"reload
 * survivor"/"retiring" lines this feature adds along the way -- the log tool
 * skips over lines that don't match the next expected one, so they don't need
 * to be listed explicitly) before anything else reads from the log, or the
 * later expectLogTerminatingNotices() call would be left trying to match
 * against "Reloading in progress ..." instead of the terminating lines.
 * socketCount=0: a supervisor pool has no listening socket, so there is no
 * "using inherited socket" line to expect. */
$tester->expectLogReloadingNotices(0);

/* Poll until the pool has settled back to exactly 2 distinct pids (the new
 * generation's steady state) for a full second, or give up at the deadline —
 * whichever comes first. This still collects every row along the way (via
 * $readAll() reading the whole file each time), so the gap analysis below
 * covers the entire reload regardless of which way this loop exits. */
$reloadDeadline = time() + 20;
$settledSince = null;
while (time() < $reloadDeadline) {
    $recent = array_slice($readAll(), -20);
    $recentPids = [];
    foreach ($recent as [$pid, $ts]) {
        $recentPids[$pid] = true;
    }
    if (count($recent) >= 20 && count($recentPids) === 2) {
        if ($settledSince === null) {
            $settledSince = microtime(true);
        } elseif (microtime(true) - $settledSince >= 1.0) {
            break;
        }
    } else {
        $settledSince = null;
    }
    usleep(100000);
}

$rows = $readAll();
if (count($rows) < 2) {
    echo "FAIL: only " . count($rows) . " alive-log rows collected in total\n";
    $tester->close(true);
    $cleanup();
    exit(1);
}

usort($rows, fn($a, $b) => $a[1] <=> $b[1]);

$maxGap = 0.0;
$maxGapAt = 0.0;
for ($i = 1; $i < count($rows); $i++) {
    $gap = $rows[$i][1] - $rows[$i - 1][1];
    if ($gap > $maxGap) {
        $maxGap = $gap;
        $maxGapAt = $rows[$i - 1][1];
    }
}

/* 20ms between writes per process, 3 processes overlapping during the
 * busiest part of the reload — a healthy timeline has gaps on the order of a
 * few tens of ms. 2.5s is generous headroom for a loaded CI box (measured:
 * one flake in ~25 local runs at a tighter 1.5s bound, on a box also running
 * a concurrent build) while still being far short of what an actual "zero
 * running copies for the whole reload" regression would produce (fork + php
 * startup + this script reaching its first write is not instant, and losing
 * issue #329's spared survivor would reintroduce exactly that whole-reload
 * gap). */
$maxAllowedGap = 2.5;

$finalPids = [];
foreach (array_slice($rows, -30) as [$pid, $ts]) {
    $finalPids[$pid] = true;
}

if ($maxGap > $maxAllowedGap) {
    printf("FAIL: max gap between consecutive alive-log writes was %.3fs (limit %.3fs), starting at t=%.3f\n",
        $maxGap, $maxAllowedGap, $maxGapAt);
} elseif (count($finalPids) > 2) {
    /* Not the primary thing this test is for, but a real signal: more than 2
     * distinct pids still writing at the very end means a reload survivor
     * (or an old-generation child) was never retired — a leak. */
    printf("FAIL: %d distinct pids still active at the end of the run (wanted 2, steady state after reload)\n",
        count($finalPids));
} else {
    echo "supervisor-reload-rolling: ok\n";
}

if ($maxGap > $maxAllowedGap || count($finalPids) > 2) {
    printf("(%d rows collected, %d distinct pids overall)\n", count($rows), count(array_unique(array_column($rows, 0))));
    $tester->close(true);
    $cleanup();
    exit(1);
}

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

$cleanup();

?>
Done
--EXPECT--
supervisor-reload-rolling: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-sup-roll-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>
