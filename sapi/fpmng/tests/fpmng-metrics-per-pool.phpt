--TEST--
fpm-ng: a pool's metrics endpoint carries that pool's application series and nobody else's (issue #276)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* operator.metrics_path exposes the series ext/fpmng_metrics collects from PHP, and
 * the endpoint is per pool. The extension stores them in per-worker slots keyed
 * by a global index, and rendering used to aggregate every slot in the region,
 * so the thing worth testing is that a per-pool scrape is a filter and not a
 * relabelled global endpoint: three pools write three differently named series
 * and each endpoint must show exactly one of them.
 *
 * http-direct because it is the one request-serving type both builds support,
 * so the per-pool filter is asked for on the libphp build too (issue #230).
 *
 * The third pool sets no metrics path at all. It is here for the other half of
 * "off": #273 decided there is no on/off directive, so off is an unset path --
 * and an unset path must take away the endpoint WITHOUT taking away
 * fpm_metric_*() from the scripts, which is a distinction only a test can hold
 * in place. */
$root = sys_get_temp_dir() . '/fpmng-metrics-per-pool-' . getmypid();
@mkdir($root, 0700, true);

foreach (['alpha' => 1.0, 'beta' => 5.0] as $pool => $step) {
    file_put_contents("$root/$pool.php", <<<PHP
    <?php
    fpm_metric_register('{$pool}_hits', 'counter', '{$pool} hits');
    fpm_metric_inc('{$pool}_hits', {$step});
    echo 'ok';
    PHP);
}

/* The pool with no endpoint: the functions must still work and still cost what
 * they cost, so the script asserts on their return values and on its own render
 * rather than on anything an operator could scrape. */
file_put_contents("$root/quiet.php", <<<'PHP'
<?php
$ok = fpm_metric_register('quiet_hits', 'counter', 'quiet hits')
    && fpm_metric_inc('quiet_hits', 3.0);
$text = fpm_metric_render();
echo $ok && str_contains($text, 'quiet_hits{pool="quiet"} 3') ? 'ok' : "bad:$text";
PHP);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}

[alpha]
listen = {{ADDR[alpha]}}
chdir = $root
pm = static
pm.max_children = 2
pool.type = http-direct
http.front_controller = /alpha.php
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /alpha-status
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /alpha-metrics

[beta]
listen = {{ADDR[beta]}}
chdir = $root
pm = static
pm.max_children = 2
pool.type = http-direct
http.front_controller = /beta.php
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /beta-metrics

[quiet]
listen = {{ADDR[quiet]}}
chdir = $root
pm = static
pm.max_children = 2
pool.type = http-direct
http.front_controller = /quiet.php
EOT;

function expect(string $what, $actual, $expected): void
{
    if ($actual !== $expected) {
        throw new RuntimeException("$what: expected " . var_export($expected, true) .
            ', got ' . var_export($actual, true));
    }
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
    $split = explode("\r\n\r\n", $raw, 2);
    return $split[1] ?? '';
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $operator = $tester->getListen('{{ADDR[operator]}}');

    /* Three requests to alpha and one to beta, so that the two counters differ
     * both by name and by value: a filter that leaked would show the wrong
     * number as well as the wrong name, and a filter that showed nothing would
     * be caught by the pool's own series being absent. */
    for ($i = 0; $i < 3; $i++) {
        expect('alpha request', hit($tester->getListen('{{ADDR[alpha]}}')), 'ok');
    }
    expect('beta request', hit($tester->getListen('{{ADDR[beta]}}')), 'ok');
    expect('quiet request', hit($tester->getListen('{{ADDR[quiet]}}')), 'ok');
    echo "pools served: ok\n";

    $alpha = fpmng_operator_body($operator, '/alpha-metrics');
    if (!str_contains($alpha, 'alpha_hits{pool="alpha"} 3')) {
        throw new RuntimeException("alpha metrics missing its own counter:\n$alpha");
    }
    /* The pool= label stays even though it is constant on a per-pool endpoint:
     * a scraper reading several of these endpoints needs it to tell the series
     * apart once they are in one time-series database. */
    if (!str_contains($alpha, '# TYPE alpha_hits counter')
        || !str_contains($alpha, '# HELP alpha_hits alpha hits')) {
        throw new RuntimeException("alpha metrics missing HELP/TYPE:\n$alpha");
    }
    foreach (['beta_hits', 'quiet_hits', 'pool="beta"', 'pool="quiet"'] as $foreign) {
        if (str_contains($alpha, $foreign)) {
            throw new RuntimeException("alpha metrics carries $foreign:\n$alpha");
        }
    }
    echo "alpha metrics: ok\n";

    $beta = fpmng_operator_body($operator, '/beta-metrics');
    if (!str_contains($beta, 'beta_hits{pool="beta"} 5')) {
        throw new RuntimeException("beta metrics missing its own counter:\n$beta");
    }
    foreach (['alpha_hits', 'quiet_hits'] as $foreign) {
        if (str_contains($beta, $foreign)) {
            throw new RuntimeException("beta metrics carries $foreign:\n$beta");
        }
    }
    echo "beta metrics: ok\n";

    /* Off means off: the quiet pool wrote series (its own script proved the
     * functions work by rendering them) and there is still no path anywhere on
     * this listener that hands them out. */
    [$status, , $body] = fpmng_operator_fetch($operator, '/quiet-metrics');
    expect('quiet metrics code', $status, 404);
    foreach (['/alpha-metrics', '/beta-metrics', '/alpha-status'] as $path) {
        if (!str_contains($body, $path)) {
            throw new RuntimeException("404 does not list $path:\n$body");
        }
    }
    echo "metrics off: ok\n";

    /* Upstream spells metrics as a flag on the status page. We do not, and this
     * pins it: ?openmetrics is the status page being asked for a variant it does
     * not have, exactly like ?json and ?xml, never an undocumented alias for
     * operator.metrics_path. On http-direct that page is the type's own text body
     * (issue #275), which is why the assertion is on its shape rather than on
     * JSON. */
    $status_page = fpmng_operator_body($operator, '/alpha-status?openmetrics');
    if (!str_contains($status_page, 'pool:') || !str_contains($status_page, 'alpha')) {
        throw new RuntimeException("?openmetrics did not return the status page:\n$status_page");
    }
    if (str_contains($status_page, 'alpha_hits')) {
        throw new RuntimeException("?openmetrics turned the status page into metrics:\n$status_page");
    }
    echo "openmetrics is not an alias: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    foreach (['alpha', 'beta', 'quiet'] as $pool) {
        @unlink("$root/$pool.php");
    }
    @rmdir($root);
}
?>
--EXPECT--
pools served: ok
alpha metrics: ok
beta metrics: ok
metrics off: ok
openmetrics is not an alias: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
