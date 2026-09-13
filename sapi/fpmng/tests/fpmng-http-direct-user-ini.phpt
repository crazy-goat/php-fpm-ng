--TEST--
fpm-ng: .user.ini for http-direct pools is the front controller's directory, never the client's path (issue #60)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Returns the decoded report the front controller prints, or 'FAILED'. */
function report(string $addr, string $path)
{
    $context = stream_context_create(['http' => [
        'method' => 'GET', 'ignore_errors' => true, 'timeout' => 5,
        // Keep the raw path: the traversal cases below are the point of the
        // test and PHP's own stream wrapper must not normalise them away.
        'header' => "Connection: close\r\n",
    ]]);
    $body = @file_get_contents("http://$addr$path", false, $context);
    if ($body === false) {
        return 'FAILED';
    }
    return json_decode($body, true) ?? ('BAD: ' . $body);
}

/* A request written by hand, so a path the URL parser would rewrite still
 * reaches the pool exactly as typed. */
function rawReport(string $addr, string $rawPath)
{
    $fp = @stream_socket_client("tcp://$addr", $errno, $error, 5);
    check($fp !== false, "connect: $error");
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET $rawPath HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    $raw = stream_get_contents($fp);
    fclose($fp);
    $body = substr($raw, strpos($raw, "\r\n\r\n") + 4);
    return json_decode($body, true) ?? ('BAD: ' . $raw);
}

$base = sys_get_temp_dir() . '/fpmng-user-ini';
@mkdir("$base/app/public", 0700, true);
@mkdir("$base/elsewhere", 0700, true);
@mkdir("$base/control", 0700, true);
@mkdir("$base/ttl/app", 0700, true);

/* The front controller reports the values a .user.ini is allowed to set, plus
 * its own pid: the TTL case below has to know that the re-read happened inside
 * the child that already had the file cached, not in a replacement child that
 * would have read it fresh whatever the TTL said. A per-request counter cannot
 * answer that -- pool.executor = classic ends the request, so a static resets
 * every time. */
$front = <<<'PHP'
<?php
if (isset($_GET['poison'])) {
    // Issue #60 leakage criterion: a request that changes the value itself
    // must not change what the next request sees.
    ini_set('precision', '3');
}
echo json_encode([
    'pid'        => getmypid(),
    'precision'  => ini_get('precision'),
    'charset'    => ini_get('default_charset'),
    'script'     => $_SERVER['SCRIPT_FILENAME'],
    'uri'        => $_SERVER['REQUEST_URI'],
]);
PHP;

/* root/.user.ini is above the front controller: it must be read, then
 * overridden by the one next to the front controller. */
file_put_contents("$base/app/.user.ini", "precision = 5\ndefault_charset = \"iso-8859-1\"\n");
file_put_contents("$base/app/public/.user.ini", "precision = 9\n");
file_put_contents("$base/app/public/front.php", $front);

/* Never read: it is not on the path from the document root to the front
 * controller, and only a client-supplied URI could ever name it. */
file_put_contents("$base/elsewhere/.user.ini", "precision = 1\ndefault_charset = \"windows-1250\"\n");

/* The control pool: same binary, same php.ini, no .user.ini anywhere. */
file_put_contents("$base/control/front.php", $front);

/* The TTL pool, whose .user.ini is rewritten while the pool is running. */
file_put_contents("$base/ttl/app/.user.ini", "precision = 7\n");
file_put_contents("$base/ttl/app/front.php", $front);

$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[ini]
listen = {{ADDR[ini]}}
pool.type = http-direct
pool.executor = classic
pm = static
pm.max_children = 1
chdir = $base/app
http.front_controller = /public/front.php
php_admin_value[user_ini.cache_ttl] = 300
php_admin_value[display_errors] = 0
[control]
listen = {{ADDR[control]}}
pool.type = http-direct
pool.executor = classic
pm = static
pm.max_children = 1
chdir = $base/control
http.front_controller = /front.php
php_admin_value[user_ini.cache_ttl] = 300
php_admin_value[display_errors] = 0
[ttl]
listen = {{ADDR[ttl]}}
pool.type = http-direct
pool.executor = classic
pm = static
pm.max_children = 1
chdir = $base/ttl
http.front_controller = /app/front.php
php_admin_value[user_ini.cache_ttl] = 0
php_admin_value[display_errors] = 0
CFG;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $ini     = $tester->getAddr('ipv4', '[ini]');
    $control = $tester->getAddr('ipv4', '[control]');
    $ttl     = $tester->getAddr('ipv4', '[ttl]');

    /* 1. The file applies, and the nearer directory wins over the document
     *    root -- i.e. the whole walk ran, not just one directory of it. */
    $r = report($ini, '/');
    echo "applied: precision=", $r['precision'], " charset=", $r['charset'], "\n";

    /* 2. A pool with no .user.ini is untouched. Whatever the built-in default
     *    is, the two pools must not agree here. */
    $c = report($control, '/');
    echo "control differs: ", var_export($c['precision'] !== $r['precision'], true), "\n";

    /* 3. Cached across requests: the file is replaced and, within the TTL,
     *    the pool keeps serving the values it already read. */
    file_put_contents("$base/app/public/.user.ini", "precision = 11\n");
    $r2 = report($ini, '/');
    echo "cached within ttl: precision=", $r2['precision'], "\n";

    /* 4. Client paths, including traversal shapes, never change the set. The
     *    elsewhere/.user.ini would be visible to any of these if the ini set
     *    were derived from the request the way the CGI SAPI derives it. */
    foreach (['/elsewhere/', '/../elsewhere/', '/%2e%2e/elsewhere/',
              '/public/../../elsewhere/', '/elsewhere/index.php'] as $path) {
        $t = rawReport($ini, $path);
        check(is_array($t), "traversal request failed for $path: " . var_export($t, true));
        printf("traversal %-26s precision=%s charset=%s script=%s\n", $path,
            $t['precision'], $t['charset'], basename($t['script']));
    }

    /* 5. No leakage: a request that calls ini_set() itself does not change
     *    what the next request is given. */
    $p = report($ini, '/?poison=1');
    $after = report($ini, '/');
    echo "poisoned request saw: ", $p['precision'], "\n";
    echo "next request saw: ", $after['precision'], "\n";

    /* 6. The re-read. With user_ini.cache_ttl = 0 the entry expires as soon as
     *    the request clock moves on, and the changed file is picked up. */
    $before = report($ttl, '/');
    file_put_contents("$base/ttl/app/.user.ini", "precision = 13\n");
    sleep(2);
    $afterTtl = report($ttl, '/');
    echo "ttl before=", $before['precision'], " after=", $afterTtl['precision'], "\n";
    echo "same child: ", var_export($afterTtl['pid'] === $before['pid'], true), "\n";

    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
} catch (Throwable $e) {
    echo "EXCEPTION: ", $e->getMessage(), "\n";
    $tester->close();
}
?>
--EXPECTF--
applied: precision=9 charset=iso-8859-1
control differs: true
cached within ttl: precision=9
traversal /elsewhere/                precision=9 charset=iso-8859-1 script=front.php
traversal /../elsewhere/             precision=9 charset=iso-8859-1 script=front.php
traversal /%2e%2e/elsewhere/         precision=9 charset=iso-8859-1 script=front.php
traversal /public/../../elsewhere/   precision=9 charset=iso-8859-1 script=front.php
traversal /elsewhere/index.php       precision=9 charset=iso-8859-1 script=front.php
poisoned request saw: 3
next request saw: 9
ttl before=7 after=13
same child: true
--CLEAN--
<?php
$base = sys_get_temp_dir() . '/fpmng-user-ini';
$walk = function (string $dir) use (&$walk): void {
    foreach (glob("$dir/{,.}*", GLOB_BRACE) ?: [] as $entry) {
        if (basename($entry) === '.' || basename($entry) === '..') continue;
        is_dir($entry) ? $walk($entry) : @unlink($entry);
    }
    @rmdir($dir);
};
if (is_dir($base)) $walk($base);
?>
