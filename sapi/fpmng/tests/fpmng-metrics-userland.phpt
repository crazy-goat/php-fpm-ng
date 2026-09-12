--TEST--
fpm-ng: the fpm_metric_*() functions exist and work inside a worker (issue #216)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

/* The userland side of ext/fpmng_metrics, asked of a running worker.
 *
 * A plain `pool.type = fastcgi` pool on purpose: both builds serve it, so this
 * test runs on the from-source build, where configure links the extension into
 * the static module list, AND on the libphp build, where nothing does and
 * fpm_init() registers the module by hand (fpm_libphp_compat.c). Those are two
 * different ways of arriving at the same five functions, and until now neither
 * was covered from PHP -- the C side had tests through the operator endpoint,
 * the PHP side had none.
 *
 * Every metric type the extension offers is exercised, because the one that is
 * not asked for is the one a registration bug takes away. */

require_once "tester.inc";

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[www]
listen = {{ADDR}}
pm = static
pm.max_children = 1
EOT;

$code = <<<'PHP'
<?php
foreach (['fpm_metric_register', 'fpm_metric_inc', 'fpm_metric_set',
          'fpm_metric_observe', 'fpm_metric_render'] as $fn) {
    if (!function_exists($fn)) {
        echo "MISSING $fn\n";
    }
}

var_dump(fpm_metric_register('t_requests', 'counter', 'requests handled'));
var_dump(fpm_metric_register('t_queue', 'gauge'));
var_dump(fpm_metric_register('t_latency', 'histogram', 'seconds', [0.1, 1.0]));

fpm_metric_inc('t_requests');
fpm_metric_inc('t_requests', 2.0, ['route' => 'home']);
fpm_metric_set('t_queue', 7.0);
fpm_metric_observe('t_latency', 0.5);

$text = fpm_metric_render();
foreach (['# HELP t_requests requests handled',
          '# TYPE t_requests counter',
          't_requests{pool="www"} 1',
          't_requests{pool="www",route="home"} 2',
          't_queue{pool="www"} 7',
          't_latency_bucket{pool="www",le="1"} 1',
          't_latency_count{pool="www"} 1'] as $needle) {
    echo str_contains($text, $needle) ? "ok\n" : "NOT FOUND: $needle\n";
}
PHP;

$tester = new FPM\Tester($cfg, $code);
$tester->start();
$tester->expectLogStartNotices();
$tester->request()->expectBody([
    'bool(true)',
    'bool(true)',
    'bool(true)',
    'ok',
    'ok',
    'ok',
    'ok',
    'ok',
    'ok',
    'ok',
], skipHeadersCheck: true);
$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
