--TEST--
fpm-ng: an experimental pool announces its tier as a WARNING at startup (issue #295)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');

$binary = getenv('TEST_PHP_FPM_EXECUTABLE') ?: FPM\Tester::findExecutable();
exec(escapeshellarg($binary) . ' -i 2>&1', $output, $status);
if ($status !== 0 || !str_contains(implode("\n", $output), '--enable-fpmng-fiber')) {
    die('skip php-fpm-ng was not built with --enable-fpmng-fiber');
}
?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #295. The third tier needs a build flag to reach, so this is a second
 * file rather than a third pool in fpmng-tier-announce.phpt: that one runs
 * everywhere and this one runs in the fiber cell (build-matrix.yml,
 * fpmng-phpt-fiber).
 *
 * The level is the assertion. #269 chose WARNING for experimental against
 * NOTICE for beta on purpose -- "may be removed in any release" is not the
 * same news as "a directive may move in a minor release", and a log an
 * operator greps by level has to be able to tell them apart. */
$root = sys_get_temp_dir() . '/fpmng-tier-fiber-' . getmypid();
@mkdir($root, 0700, true);
$probe = '<?php echo "fiber";';
file_put_contents("$root/index.php", $probe);

/* The gateway's own address comes from {{ADDR[http]}}, like every other fiber
 * test here: the pool still has a FastCGI listen of its own. Nothing is
 * requested through either -- the line under test is emitted before the first
 * child is forked, and a request would only prove the pool works, which other
 * tests already do. */
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[experimental]
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 1
pool.type = http
pool.executor = fiber
http.listen = {{ADDR[http]}}
php_admin_value[opcache.enable] = 0
php_admin_value[max_execution_time] = 0
CFG;

$tester = new FPM\Tester($cfg, $probe);
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* Through the tester's log API and not file_get_contents(): the suite runs
     * FPM in the foreground, and tester.inc only switches the log source to the
     * error_log file when it daemonizes -- so the file named by {{FILE:LOG}}
     * above stays empty and every assertion made against it would pass on
     * nothing. checkAllLogs because the line is emitted before the first child,
     * which puts it behind the reader once expectLogStartNotices() has walked
     * past "ready to handle connections".
     *
     * expectLogWarning, not a pattern match on the text: the LEVEL is what is
     * under test here. */
    $tester->expectLogWarning(
        'pool\.type = http with pool\.executor = fiber is EXPERIMENTAL: .*README\.md',
        'experimental',
        checkAllLogs: true
    );
    echo "experimental announced as a WARNING: ok\n";

    /* The other half of the level assertion: nothing announced this pool at
     * NOTICE as well, which a second call site or a copied line would show up
     * as. */
    $tester->expectNoLogPattern('/NOTICE:.*\[pool experimental\].*is EXPERIMENTAL/', true);
    echo "not also announced at NOTICE: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/index.php");
    @rmdir($root);
}
?>
--EXPECT--
experimental announced as a WARNING: ok
not also announced at NOTICE: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
