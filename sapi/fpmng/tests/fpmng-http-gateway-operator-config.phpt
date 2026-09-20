--TEST--
fpm-ng: http.operator configurations that cannot mean anything are refused at startup (issue #389)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #389, acceptance criterion 4. Three refusals, each naming what is
 * wrong, so `php-fpm-ng -t` reports them without anything starting:
 *
 *  - http.operator = yes with no http.operator_allowed_clients;
 *  - both base paths empty, so there is no base to forward <base>/<pool> under;
 *  - an http.route[] prefix claiming a base path, which the operator namespace
 *    would shadow (operator paths are matched before routing). */

function expectConfigFailure(string $label, string $cfg, array $needles): void
{
    $tester = new FPM\Tester($cfg, '<?php echo "ok";');
    $messages = $tester->testConfig(true);
    if ($messages === null) {
        echo "FAIL: $label unexpectedly passed validation\n";
        exit(1);
    }
    $text = implode("\n", $messages);
    foreach ($needles as $needle) {
        if (!str_contains($text, $needle)) {
            echo "FAIL: $label missing needle: $needle\n";
            echo "got:\n$text\n";
            exit(1);
        }
    }
    echo "$label: rejected\n";
}

function gatewayConfig(string $routes, string $extra = ''): string
{
    return "[global]\nerror_log = {{FILE:LOG}}\n"
        . "[gw]\npool.type = gateway\nlisten = {{ADDR[http]}}\nhttp.gateways = 1\n"
        . $routes
        . $extra
        . "[app]\nlisten = {{ADDR}}\npm = static\npm.max_children = 1\n";
}

expectConfigFailure(
    'operator-without-acl',
    gatewayConfig("http.route[app] = /\n", "http.operator = yes\n"),
    ['http.operator = yes requires http.operator_allowed_clients']
);

expectConfigFailure(
    'operator-both-bases-empty',
    gatewayConfig("http.route[app] = /\n", "http.operator = yes\nhttp.operator_allowed_clients = 127.0.0.1\noperator.metrics_path =\noperator.status_path =\n"),
    ['both operator.metrics_path and operator.status_path are empty']
);

expectConfigFailure(
    'operator-both-bases-off',
    gatewayConfig("http.route[app] = /\n", "http.operator = yes\nhttp.operator_allowed_clients = 127.0.0.1\noperator.metrics = off\noperator.status = off\n"),
    ['both operator.metrics_path and operator.status_path are empty']
);

expectConfigFailure(
    'operator-route-claims-metrics-base',
    gatewayConfig("http.route[app] = /metrics\n", "http.operator = yes\nhttp.operator_allowed_clients = 127.0.0.1\n"),
    ['http.route[app]', "'/metrics'", 'collides with the operator base path']
);

expectConfigFailure(
    'operator-route-claims-status-base',
    gatewayConfig("http.route[app] = /status\n", "http.operator = yes\nhttp.operator_allowed_clients = 127.0.0.1\n"),
    ['http.route[app]', "'/status'", 'collides with the operator base path']
);

expectConfigFailure(
    'operator-acl-bad-address',
    gatewayConfig("http.route[app] = /\n", "http.operator = yes\nhttp.operator_allowed_clients = 10.0.0.0/8\n"),
    ['is not a valid IP address']
);

?>
Done
--EXPECT--
operator-without-acl: rejected
operator-both-bases-empty: rejected
operator-both-bases-off: rejected
operator-route-claims-metrics-base: rejected
operator-route-claims-status-base: rejected
operator-acl-bad-address: rejected
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
