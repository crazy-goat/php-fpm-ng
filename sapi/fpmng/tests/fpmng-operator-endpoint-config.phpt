--TEST--
fpm-ng: operator endpoint configuration -- collisions, the internal type, the operator.* namespace and its old pm. spellings (issues #274, #283, #386)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* A binary linked against a distribution libphp refuses a type whose children
 * need patches/0006, before any directive of that pool is read
 * (fpm_pool_type_check_build_support()). Issue #388 retired pool.type = http,
 * the last type that needed it, so the cases below -- cron, supervisor and
 * http-direct pools -- all run on both builds. */

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
    $head . $cron('a', "operator.status_listen = {{ADDR[op]}}\noperator.status_path = /status")
          . $cron('b', "operator.status_listen = {{ADDR[op]}}\noperator.status_path = /status"),
    ['collides with pool', "operator.status_path = /status"]
);

/* The same path on two different listeners is not a collision -- the triple
 * differs in the address. Each endpoint reports on its own pool. */
expectAccepted(
    'same path on two listeners',
    $head . $cron('a', "operator.status_listen = {{ADDR[op1]}}\noperator.status_path = /status")
          . $cron('b', "operator.status_listen = {{ADDR[op2]}}\noperator.status_path = /status")
);

/* One pool may take both formats from one address: the paths differ, so the
 * two routes are two URLs. */
expectAccepted(
    'both formats on one listener',
    $head . $cron('a', "operator.status_listen = {{ADDR[op]}}\noperator.status_path = /status\n"
                     . "operator.metrics_listen = {{ADDR[op]}}\noperator.metrics_path = /metrics")
);

/* ...and one pool may not take both formats from ONE path, which is the same
 * rule seen from inside a single pool: the triple is (address, port, path), and
 * nothing about it cares whether the two claimants are two pools or one. The
 * message has to name both directives -- "collides with pool 'a'" told an
 * operator their pool collides with itself (issue #276). */
expectRejected(
    'both formats on one path',
    $head . $cron('a', "operator.status_listen = {{ADDR[op]}}\noperator.status_path = /both\n"
                     . "operator.metrics_listen = {{ADDR[op]}}\noperator.metrics_path = /both"),
    ['operator.status_path and operator.metrics_path are both /both',
     'the status page and the metrics page need different paths']
);

/* The same path for both formats on two different addresses is fine: two
 * triples, two endpoints, and an operator who wants /metrics to be spelled
 * /metrics on every port gets to have that. */
expectAccepted(
    'both formats on one path, two listeners',
    $head . $cron('a', "operator.status_listen = {{ADDR[op1]}}\noperator.status_path = /page\n"
                     . "operator.metrics_listen = {{ADDR[op2]}}\noperator.metrics_path = /page")
);

/* The listener pool is created by fpm-ng and is not a type anyone can name.
 * The message is the one an unknown pool.type gets, and the list it prints must
 * not offer the internal type either. */
expectRejected(
    'the internal type is not configurable',
    $head . "\n[mine]\nlisten = {{ADDR}}\npool.type = operator-endpoint\n",
    ["unknown pool.type 'operator-endpoint'", 'known types: ']
);

/* Issue #386: the operator directives left the "pm." namespace, so cron and
 * supervisor need no carve-out any more -- "pm." is refused whole, and the
 * operator.* names are accepted because nothing rejects them. */
expectAccepted(
    'operator directive on cron',
    $head . $cron('a', "operator.status_listen = {{ADDR[op]}}\noperator.status_path = /status")
);
expectRejected(
    'other pm. directive on cron',
    $head . $cron('a', "pm.max_children = 4"),
    ["'pm.max_children' is not supported by pool.type = cron"]
);
expectAccepted(
    'operator metrics flag on supervisor',
    $head . "\n[sup]\npool.type = supervisor\nsupervisor.script = /dev/null\n"
          . "supervisor.restart = never\noperator.metrics = on\n"
);

/* The old names are refused by name with the replacement, on any type -- not
 * aliased. cron is the interesting one because it also rejects "pm.": the
 * rename message must win over the generic reject. */
expectRejected(
    'old pm.metrics_path on cron',
    $head . $cron('a', "pm.metrics_listen = {{ADDR[op]}}\npm.metrics_path = /metrics"),
    ["'pm.metrics_path' was renamed to 'operator.metrics_path' (issue #386)"]
);
expectRejected(
    'old pm.status_path on cron',
    $head . $cron('a', "pm.status_path = /status"),
    ["'pm.status_path' was renamed to 'operator.status_path' (issue #386)"]
);
expectRejected(
    'old pm.status_listen on cron',
    $head . $cron('a', "pm.status_listen = {{ADDR[op]}}\noperator.status_path = /status"),
    ["'pm.status_listen' was renamed to 'operator.status_listen' (issue #386)"]
);

/* Both spellings of one page at once is refused naming both directives. */
expectRejected(
    'operator.metrics flag and path together',
    $head . $cron('a', "operator.metrics = on\noperator.metrics_path = /x"),
    ["'operator.metrics = on' and 'operator.metrics_path = /x'", 'set one or the other']
);
expectRejected(
    'operator.status flag and path together',
    $head . $cron('a', "operator.status = on\noperator.status_path = /x"),
    ["'operator.status = on' and 'operator.status_path = /x'", 'set one or the other']
);

/* A pool that exposes a page carries its name into a URL, so its name has to be
 * path-safe; a pool that exposes nothing keeps whatever name it had. */
expectRejected(
    'operator flag on a pool whose name is not path-safe',
    $head . "\n[bad name]\npool.type = cron\ncron.schedule = @hourly\ncron.script = /dev/null\n"
          . "operator.metrics = on\n",
    ['[pool bad name]', "must contain only the characters '[alphanum]/_-.~'", 'URL path segment']
);
expectAccepted(
    'the same pool with no operator directive',
    $head . "\n[bad name]\npool.type = cron\ncron.schedule = @hourly\ncron.script = /dev/null\n"
);

/* The derived paths are just paths: operator.metrics = on alone is accepted and
 * answers /metrics/<pool>. */
expectAccepted(
    'operator.metrics = on alone',
    $head . $cron('tick', "operator.metrics = on")
);

/* Both of http-direct's operator pages go to the operator listener, and both
 * directives that name where are accepted on it. */
expectAccepted(
    'both operator paths on http-direct',
    $head . $direct('web', "operator.metrics_listen = {{ADDR[op]}}\noperator.metrics_path = /metrics\n"
                         . "operator.status_listen = {{ADDR[op]}}\noperator.status_path = /status")
);

/* And the status route is a real route, so it collides like one: since #275
 * http-direct's status page IS on the operator listener, and two pools naming
 * the same path on the same address is two answers for one URL. This case was
 * accepted while the move was still pending, which is exactly why it is here --
 * a regression that put the page back on the pool's own listener would make it
 * pass again. */
expectRejected(
    'status path claimed by two http-direct pools on one listener',
    $head . $direct('one', "operator.status_listen = {{ADDR[op]}}\noperator.status_path = /status")
          . $direct('two', "operator.status_listen = {{ADDR[op]}}\noperator.status_path = /status"),
    ['collides with pool', 'an address, a port and a path identify one endpoint']
);

/* One socket is one process and one identity. Pools sharing an operator address
 * must agree on it, or the endpoint runs as, and its socket belongs to,
 * whichever pool the configuration happened to list first. */
expectRejected(
    'pools sharing a listener disagree on identity',
    $head . $direct('one', "listen.mode = 0660
operator.metrics_listen = {{ADDR[op]}}
operator.metrics_path = /one")
          . $direct('two', "listen.mode = 0600
operator.metrics_listen = {{ADDR[op]}}
operator.metrics_path = /two"),
    ['disagree on listen.mode', 'one listener is one process and one socket']
);

/* On fastcgi, pm.status_path keeps upstream's meaning and there is no operator
 * listener to name, so pm.status_path is fine and every operator.* directive is
 * an error rather than one that quietly does nothing. The old metrics spelling
 * is refused by its own name first, naming the replacement (issue #386). */
expectAccepted(
    'upstream pm.status_path on fastcgi',
    $head . "\n[app]\nlisten = {{ADDR}}\npm = static\npm.max_children = 1\npm.status_path = /status\n"
);
expectRejected(
    'operator.metrics_path on a fastcgi pool',
    $head . "\n[app]\nlisten = {{ADDR}}\npm = static\npm.max_children = 1\noperator.metrics_path = /metrics\n",
    ["'operator.metrics_path' is not supported by pool.type = fastcgi"]
);
expectRejected(
    'old pm.metrics_path on http-direct',
    $head . $direct('web', "pm.metrics_listen = {{ADDR[op]}}\npm.metrics_path = /metrics"),
    ["'pm.metrics_path' was renamed to 'operator.metrics_path' (issue #386)"]
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
operator directive on cron: accepted
other pm. directive on cron: rejected
operator metrics flag on supervisor: accepted
old pm.metrics_path on cron: rejected
old pm.status_path on cron: rejected
old pm.status_listen on cron: rejected
operator.metrics flag and path together: rejected
operator.status flag and path together: rejected
operator flag on a pool whose name is not path-safe: rejected
the same pool with no operator directive: accepted
operator.metrics = on alone: accepted
both operator paths on http-direct: accepted
status path claimed by two http-direct pools on one listener: rejected
pools sharing a listener disagree on identity: rejected
upstream pm.status_path on fastcgi: accepted
operator.metrics_path on a fastcgi pool: rejected
old pm.metrics_path on http-direct: rejected
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
