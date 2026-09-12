--TEST--
fpm-ng: pool types count their own invocations without any fpm_metric_* call (issue #277)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* ext/fpmng_metrics is fed entirely from PHP, so until now the two questions an
 * operator asks first -- is this pool doing anything, is this pool flapping --
 * had no answer on a pool whose script registers nothing, and on supervisor
 * could not have one: a supervised script is not the shape that calls
 * fpm_metric_inc().
 *
 * The baseline counter answers them from the SAPI side. Nothing below registers
 * a series, and that absence is the assertion: the scripts are a front
 * controller that echoes and a supervised script that exits.
 *
 * cron's counter lives in fpmng-baseline-counters-cron.phpt, because the
 * shortest schedule that exists fires once a minute and a minute of waiting
 * does not belong in front of these two. */
$root = sys_get_temp_dir() . '/fpmng-baseline-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents($root . '/front.php', '<?php echo "ok";');
/* Issue #122's configuration in shape: restart = always with a script that
 * exits 0, which respawns without limit and, before this, without a number
 * anywhere. The usleep is the one departure -- #122 measured 12086 restarts a
 * second and this test does not need to reproduce that on a CI runner to prove
 * the counter moves. */
file_put_contents($root . '/flap.php', '<?php usleep(50000); exit(0);');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}

[web]
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 2
pm.max_requests = 2
pool.type = http-direct
http.front_controller = /front.php
pm.metrics_listen = {{ADDR[operator]}}
pm.metrics_path = /web-metrics

[flap]
pool.type = supervisor
supervisor.script = $root/flap.php
supervisor.processes = 1
supervisor.restart = always
pm.metrics_listen = {{ADDR[operator]}}
pm.metrics_path = /flap-metrics
pm.status_listen = {{ADDR[operator]}}
pm.status_path = /flap-status
EOT;

function expect(string $what, $actual, $expected): void
{
    if ($actual !== $expected) {
        throw new RuntimeException("$what: expected " . var_export($expected, true) .
            ', got ' . var_export($actual, true));
    }
}

/* One series' value out of a Prometheus body. Matched on the whole line
 * including the pool label, so that a leak from another pool cannot be read as
 * this pool's number. */
function series(string $body, string $name, string $pool): float
{
    $pattern = '/^' . preg_quote($name, '/') . '\{pool="' . preg_quote($pool, '/') . '"\} (\S+)$/m';
    if (!preg_match($pattern, $body, $m)) {
        throw new RuntimeException("no $name for pool $pool in:\n$body");
    }
    return (float) $m[1];
}

function hit(string $addr): string
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect $addr: $error");
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $raw = (string) stream_get_contents($fp);
    fclose($fp);
    return explode("\r\n\r\n", $raw, 2)[1] ?? '';
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $operator = $tester->getListen('{{ADDR[operator]}}');
    $public = $tester->getListen('{{ADDR}}');

    /* Ten requests through a pool of two workers that recycle every two of
     * them: the counter has to read 10, not 2. The issue asks for the
     * monotonicity across a recycle to be asserted rather than assumed, and
     * this is where it would break if the count lived in the worker. */
    for ($i = 0; $i < 10; $i++) {
        expect('request ' . $i, hit($public), 'ok');
    }
    expect('requests after recycling',
        series(fpmng_operator_body($operator, '/web-metrics'), 'fpmng_pool_requests_total', 'web'), 10.0);
    echo "http-direct requests: ok\n";

    /* The supervisor counter counts restarts, not failures: every one of these
     * exited 0, so consecutive_failures stays at zero while the restart count
     * climbs. A counter that only moved on failure would read zero here, which
     * is exactly the blind spot #122 fell into. */
    $first = series(fpmng_operator_body($operator, '/flap-metrics'), 'fpmng_pool_restarts_total', 'flap');
    $deadline = microtime(true) + 10.0;
    do {
        usleep(100000);
        $second = series(fpmng_operator_body($operator, '/flap-metrics'), 'fpmng_pool_restarts_total', 'flap');
    } while ($second <= $first && microtime(true) < $deadline);
    if ($second <= $first) {
        throw new RuntimeException("supervisor restarts stalled at $first (then $second)");
    }

    /* The same counter on the status page, under the short name the type chose:
     * one counter, two spellings of it, and they agree. */
    $json = json_decode(fpmng_operator_body($operator, '/flap-status'), true, flags: JSON_THROW_ON_ERROR);
    $pool = $json['pools'][0];
    expect('the status page reports the flapping pool', $pool['name'], 'flap');
    expect('flapping is not failing', $pool['consecutive_failures'], 0);
    if (!isset($pool['restarts']) || $pool['restarts'] < $second) {
        throw new RuntimeException('status page restarts behind the metrics page: ' . var_export($pool, true));
    }
    echo "supervisor restarts: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/front.php');
    @unlink($root . '/flap.php');
    @rmdir($root);
}
?>
--EXPECT--
http-direct requests: ok
supervisor restarts: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
