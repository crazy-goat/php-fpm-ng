--TEST--
fpm-ng: gateway static files follow a `current -> releases/N` symlink swap without a reload (#638)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php
require_once "tester.inc";
$base = sys_get_temp_dir() . '/fpmng-symswap-' . getmypid();
$outside = $base . '-outside';
@mkdir($base);
@mkdir($base . '/releases');
@mkdir($base . '/releases/1');
@mkdir($base . '/releases/2');
@mkdir($outside);
file_put_contents($base . '/releases/1/app.css', 'release-1');
file_put_contents($base . '/releases/2/app.css', 'release-2');
file_put_contents($base . '/releases/1/only1.css', 'only-in-1');
file_put_contents($base . '/releases/2/front.php', '<?php echo "php";');
file_put_contents($base . '/releases/1/front.php', '<?php echo "php";');
file_put_contents($outside . '/secret.css', 'OUTSIDE');
/* Release 2 carries a symlink out of the document root: containment must hold
 * against the root as re-resolved for each request. */
symlink($outside . '/secret.css', $base . '/releases/2/escape.css');
symlink('releases/1', $base . '/current');
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $base/current
http.gateways = 1
http.static = 1
http.front_controller = /front.php
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $base/current
pm = static
pm.max_children = 1
EOT;

function httpGet(string $url): string|false
{
    $ctx = stream_context_create(['http' => ['timeout' => 5, 'ignore_errors' => true]]);
    return @file_get_contents($url, false, $ctx);
}

function expect(string $what, $actual, $expected): void
{
    if ($actual !== $expected) {
        throw new RuntimeException("$what: expected " . var_export($expected, true) .
            ', got ' . var_export($actual, true));
    }
}

/* The atomic swap of Deployer/Envoyer/Capistrano: ln -sfn into a temporary
 * name, then rename over `current`. */
function swap(string $base, int $release): void
{
    @unlink($base . '/current.tmp');
    symlink('releases/' . $release, $base . '/current.tmp');
    rename($base . '/current.tmp', $base . '/current');
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');
    $get = fn(string $path) => httpGet("http://$http$path");

    expect('before swap', $get('/app.css'), 'release-1');
    echo "release 1: ok\n";

    swap($base, 2);
    expect('after swap', $get('/app.css'), 'release-2');
    echo "release 2 after swap: ok\n";

    /* A file only the old release had is gone: nothing is served from N-1. */
    expect('old release file', $get('/only1.css') === 'only-in-1', false);
    echo "old release gone: ok\n";

    /* Containment still holds under the re-resolved root. */
    expect('escape refused', $get('/escape.css') === 'OUTSIDE', false);
    echo "containment: ok\n";

    swap($base, 1);
    expect('rollback', $get('/app.css'), 'release-1');
    echo "rollback: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    foreach (['/current', '/current.tmp', '/releases/1/app.css', '/releases/1/only1.css', '/releases/1/front.php',
              '/releases/2/app.css', '/releases/2/front.php', '/releases/2/escape.css'] as $f) {
        @unlink($base . $f);
    }
    @unlink($outside . '/secret.css');
    @rmdir($base . '/releases/1');
    @rmdir($base . '/releases/2');
    @rmdir($base . '/releases');
    @rmdir($base);
    @rmdir($outside);
}
?>
--EXPECT--
release 1: ok
release 2 after swap: ok
old release gone: ok
containment: ok
rollback: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
