--TEST--
fpm-ng: operator endpoint configuration -- collisions, the internal type, and the pm. carve-out (issues #274, #283)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* A binary linked against a distribution libphp refuses pool.type = http and
 * fastcgi-ng before any directive of that pool is read
 * (fpm_pool_type_check_build_support()). None of the cases below use those
 * types, so every one of them runs on both builds. */

function expectRejected(string $label, string $cfg, array $needles): void
{
    $tester = new FPM\Tester($cfg, '<?php echo "ok";');
    $messages = $tester->testConfig(true);
    if ($messages === null) {
        echo "FAIL: $label was accepted\n";
        exit(1);
    }
    $text = implode("\n", $messages);
    foreach ($needles as $needle) {
        if (!str_contains($text, $needle)) {
            echo "FAIL: $label missing needle: $needle\ngot:\n$text\n";
            exit(1);
        }
    }
    echo "$label: rejected\n";
}

function expectAccepted(string $label, string $cfg): void
{
    $tester = new FPM\Tester($cfg, '<?php echo "ok";');
    $messages = $tester->testConfig(true);
    if ($messages !== null) {
        echo "FAIL: $label was rejected\n" . implode("\n", $messages) . "\n";
        exit(1);
    }
    echo "$label: accepted\n";
}

$root = sys_get_temp_dir() . '/fpmng-operator-config-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents($root . '/front.php', '<?php');

$head = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
EOT;

$cron = function (string $name, string $extra): string {
    return <<<EOT

[$name]
pool.type = cron
cron.schedule = @hourly
cron.script = /dev/null
$extra
EOT;
};

$direct = function (string $name, string $extra) use ($root): string {
    return <<<EOT

[$name]
listen = {{ADDR[$name]}}
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
$extra
EOT;
};

/* #273, point 7: an address, a port and a path identify one endpoint. Two pools
 * asking for the same triple is refused, because a scrape of that URL has no
 * way to tell which pool answered. */
expectRejected(
    'same path on one listener',
    $head . $cron('a', "pm.status_listen = {{ADDR[op]}}\npm.status_path = /status")
          . $cron('b', "pm.status_listen = {{ADDR[op]}}\npm.status_path = /status"),
    ['collides with pool', "pm.status_path = /status"]
);

/* The same path on two different listeners is not a collision -- the triple
 * differs in the address. Each endpoint reports on its own pool. */
expectAccepted(
    'same path on two listeners',
    $head . $cron('a', "pm.status_listen = {{ADDR[op1]}}\npm.status_path = /status")
          . $cron('b', "pm.status_listen = {{ADDR[op2]}}\npm.status_path = /status")
);

/* One pool may take both formats from one address: the paths differ, so the
 * two routes are two URLs. */
expectAccepted(
    'both formats on one listener',
    $head . $cron('a', "pm.status_listen = {{ADDR[op]}}\npm.status_path = /status\n"
                     . "pm.metrics_listen = {{ADDR[op]}}\npm.metrics_path = /metrics")
);

/* ...and one pool may not take both formats from ONE path, which is the same
 * rule seen from inside a single pool: the triple is (address, port, path), and
 * nothing about it cares whether the two claimants are two pools or one. The
 * message has to name both directives -- "collides with pool 'a'" told an
 * operator their pool collides with itself (issue #276). */
expectRejected(
    'both formats on one path',
    $head . $cron('a', "pm.status_listen = {{ADDR[op]}}\npm.status_path = /both\n"
                     . "pm.metrics_listen = {{ADDR[op]}}\npm.metrics_path = /both"),
    ['pm.status_path and pm.metrics_path are both /both',
     'the status page and the metrics page need different paths']
);

/* The same path for both formats on two different addresses is fine: two
 * triples, two endpoints, and an operator who wants /metrics to be spelled
 * /metrics on every port gets to have that. */
expectAccepted(
    'both formats on one path, two listeners',
    $head . $cron('a', "pm.status_listen = {{ADDR[op1]}}\npm.status_path = /page\n"
                     . "pm.metrics_listen = {{ADDR[op2]}}\npm.metrics_path = /page")
);

/* The listener pool is created by fpm-ng and is not a type anyone can name.
 * The message is the one an unknown pool.type gets, and the list it prints must
 * not offer the internal type either. */
expectRejected(
    'the internal type is not configurable',
    $head . "\n[mine]\nlisten = {{ADDR}}\npool.type = operator-endpoint\n",
    ["unknown pool.type 'operator-endpoint'", 'known types: ']
);

/* Issue #283, both ways round on one type: cron rejects the whole "pm." prefix,
 * and the four operator directives are carved out of that reject. The carve-out
 * has to be exact -- a directive under the same prefix that was never carved out
 * is still refused, or the mechanism is just a hole in the reject list. */
expectAccepted(
    'carved-out pm. directive on cron',
    $head . $cron('a', "pm.status_listen = {{ADDR[op]}}\npm.status_path = /status")
);
expectRejected(
    'other pm. directive on cron',
    $head . $cron('a', "pm.max_children = 4"),
    ["'pm.max_children' is not supported by pool.type = cron"]
);

/* supervisor carries the same reject list and the same carve-out, and the
 * metrics pair goes through it as well as the status pair -- four directives
 * were carved out, so all four have to be asked for somewhere. */
expectAccepted(
    'carved-out metrics directives on supervisor',
    $head . "\n[sup]\npool.type = supervisor\nsupervisor.script = /dev/null\n"
          . "supervisor.restart = never\n"
          . "pm.metrics_listen = {{ADDR[op]}}\npm.metrics_path = /metrics\n"
);
expectRejected(
    'other pm. directive on supervisor',
    $head . "\n[sup]\npool.type = supervisor\nsupervisor.script = /dev/null\n"
          . "supervisor.restart = never\npm.max_children = 4\n",
    ["'pm.max_children' is not supported by pool.type = supervisor"]
);

/* Both of http-direct's operator pages go to the operator listener, and both
 * directives that name where are accepted on it. pm.status_listen used to be
 * refused by this type outright -- under its upstream meaning it asked for a
 * second FastCGI socket a direct child has nowhere to put -- and issue #275 is
 * what gave it something to name. */
expectAccepted(
    'both operator paths on http-direct',
    $head . $direct('web', "pm.metrics_listen = {{ADDR[op]}}\npm.metrics_path = /metrics\n"
                         . "pm.status_listen = {{ADDR[op]}}\npm.status_path = /status")
);

/* And the status route is a real route, so it collides like one: since #275
 * http-direct's status page IS on the operator listener, and two pools naming
 * the same path on the same address is two answers for one URL. This case was
 * accepted while the move was still pending, which is exactly why it is here --
 * a regression that put the page back on the pool's own listener would make it
 * pass again. */
expectRejected(
    'status path claimed by two http-direct pools on one listener',
    $head . $direct('one', "pm.status_listen = {{ADDR[op]}}\npm.status_path = /status")
          . $direct('two', "pm.status_listen = {{ADDR[op]}}\npm.status_path = /status"),
    ['collides with pool', 'an address, a port and a path identify one endpoint']
);

/* One socket is one process and one identity. Pools sharing an operator address
 * must agree on it, or the endpoint runs as, and its socket belongs to,
 * whichever pool the configuration happened to list first. */
expectRejected(
    'pools sharing a listener disagree on identity',
    $head . $direct('one', "listen.mode = 0660
pm.metrics_listen = {{ADDR[op]}}
pm.metrics_path = /one")
          . $direct('two', "listen.mode = 0600
pm.metrics_listen = {{ADDR[op]}}
pm.metrics_path = /two"),
    ['disagree on listen.mode', 'one listener is one process and one socket']
);

/* pm.metrics_path only means something on a type that serves an operator
 * endpoint. On fastcgi, pm.status_path keeps its upstream meaning and there is
 * nowhere to put a metrics page, so asking for one is an error rather than a
 * directive that quietly does nothing. */
expectRejected(
    'metrics path on a fastcgi pool',
    $head . "\n[app]\nlisten = {{ADDR}}\npm = static\npm.max_children = 1\npm.metrics_path = /metrics\n",
    ["'pm.metrics_path' is not supported by pool.type = fastcgi"]
);

@unlink($root . '/front.php');
@rmdir($root);

echo "Done\n";
?>
--EXPECT--
same path on one listener: rejected
same path on two listeners: accepted
both formats on one listener: accepted
both formats on one path: rejected
both formats on one path, two listeners: accepted
the internal type is not configurable: rejected
carved-out pm. directive on cron: accepted
other pm. directive on cron: rejected
carved-out metrics directives on supervisor: accepted
other pm. directive on supervisor: rejected
both operator paths on http-direct: accepted
status path claimed by two http-direct pools on one listener: rejected
pools sharing a listener disagree on identity: rejected
metrics path on a fastcgi pool: rejected
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
