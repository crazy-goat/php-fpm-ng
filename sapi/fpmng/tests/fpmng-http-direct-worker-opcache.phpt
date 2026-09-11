--TEST--
fpm-ng: worker-mode HTTP-direct builtins and constants survive opcache folding (task 076, issue #149)
--SKIPIF--
<?php
include "skipif.inc";
if (!extension_loaded('Zend OPcache')) {
    die('skip requires the Zend OPcache extension to be compiled in');
}
/* Compiled in but switched off by ini is an environment this test cannot
 * exercise, not a regression: php_admin_value[opcache.enable] = 1 in the pool
 * config cannot turn it back on (OnEnable only ever reports success when it is
 * already on), so the in-test opcacheEnabled assertions would fail for a
 * reason that has nothing to do with the fix. Skip instead. Those assertions
 * stay, and still catch opcache being active in ini yet inactive in the
 * child. */
/* No filter_var()/FILTER_VALIDATE_BOOLEAN here: ext/filter is not built under
 * --disable-all, which is exactly the canonical build this suite runs on, and
 * calling it borked this test. Plain string comparison only. */
$opcacheIni = strtolower(trim((string) ini_get('opcache.enable')));
if ($opcacheIni === '' || $opcacheIni === '0' || $opcacheIni === 'off' || $opcacheIni === 'false') {
    die('skip requires opcache.enable=1 (compiled in but disabled by ini)');
}
?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$root = sys_get_temp_dir() . '/fpmng-direct-worker-opcache-' . getmypid();
@mkdir($root, 0700, true);

/* Included by BOTH worker.php and classic.php below, on purpose: opcache's
 * SHM entry for a compiled script is keyed on path alone and is shared
 * across every pool/executor in the process tree, not scoped per pool. If
 * fpm_worker_register_functions() ever claims MODULE_PERSISTENT again (see
 * fpm_http_direct_worker.c), the worker child -- which is hit first, below
 * -- would bake `hasWorkerLoop => true` into this file's cached op_array,
 * and the classic child would then incorrectly inherit that folded `true`
 * from the shared cache instead of evaluating function_exists() itself.
 * Using one shared file is what makes that leak observable; two separate
 * files would only prove worker builtins aren't *registered* in the classic
 * child, which is a weaker and different claim.
 *
 * The load-bearing part of probe() is that both function_exists() calls use
 * LITERAL string arguments. opcache's pass1 constant-folds exactly that
 * shape at compile time (Zend/Optimizer/zend_optimizer.c:113-118) and used
 * to dereference a NULL internal_function->module while doing it -- see
 * fpm_worker_register_functions() for the fix and the core-dump evidence
 * (task 076). A variable argument would defeat the folding and prove
 * nothing; this must stay literal.
 *
 * The same shape covers the constants (issue #149): defined() on a literal
 * name and a bare FPMNG_WORKER_READ fetch are the two opcodes pass1 folds
 * for a CONST_PERSISTENT constant, and these three are registered per fork
 * in worker children only. The fetch is wrapped in a try/catch because the
 * correct answer in the classic child is an Error -- a folded one would
 * quietly hand it the worker's value instead.
 *
 * Measured, not assumed: with CONST_PERSISTENT still on the three
 * REGISTER_MAIN_LONG_CONSTANT() calls, this test's classic child answered
 * `"definedRead":true,"readValue":1` -- the worker's value, in a process
 * where the constant is not registered at all -- while `hasWorkerLoop` was
 * correctly false, i.e. the function side was already safe and only the
 * constants leaked. CI run 34598875593. Note the first attempt measured
 * nothing at all because `cached` was false: opcache refuses a file whose
 * mtime is within opcache.file_update_protection, which is why both pools
 * below set it to 0 and why the probe reports `cached`. */
file_put_contents("$root/shared.php", <<<'PHP'
<?php
function fpmng_worker_probe(): array
{
    $status = opcache_get_status(false);
    return [
        'opcacheEnabled' => $status !== false && ($status['opcache_enabled'] ?? false) === true,
        'hasWorkerLoop' => function_exists('fpmng_worker_loop'),
        'hasBogus' => function_exists('fpmng_worker_this_does_not_exist'),
        'cached' => function_exists('opcache_is_script_cached')
            ? opcache_is_script_cached(__DIR__ . '/shared.php') : null,
        'definedRead' => defined('FPMNG_WORKER_READ'),
        'readValue' => (static function () {
            try {
                return FPMNG_WORKER_READ;
            } catch (\Throwable $e) {
                return null;
            }
        })(),
    ];
}
PHP);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
require __DIR__ . '/shared.php';

$notify = fpmng_worker_notify_stream();
$probe = fpmng_worker_probe();

function handle(int $id): void
{
    global $probe;
    fpmng_worker_respond($id, 200, ['Content-Type' => 'application/json'], json_encode($probe + [
        'pid' => getmypid(),
    ]));
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        handle($id);
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_stopping()) {
    fpmng_worker_loop(true);
}
PHP);

/* Also cover the classic executor of the same pool.type, in the same process
 * family, so the negative half of the acceptance criterion (worker builtins
 * must not leak into other executors, including via a shared opcache SHM
 * entry -- see the comment on shared.php above) is measured under opcache
 * too, not just assumed from the worker-only case. */
file_put_contents("$root/classic.php", <<<'PHP'
<?php
require __DIR__ . '/shared.php';
echo json_encode(fpmng_worker_probe());
PHP);

$workerPort = (int) (getenv('FPMNG_DIRECT_WORKER_OPCACHE_TEST_PORT') ?: 28076);
$classicPort = $workerPort + 1;
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[worker]
listen = 127.0.0.1:$workerPort
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
http.max_body = 1M
catch_workers_output = yes
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
php_admin_value[opcache.enable] = 1
; The test writes shared.php a moment before FPM starts, and opcache refuses
; to cache a file whose mtime is within opcache.file_update_protection (2s by
; default) of now -- which would leave the shared SHM entry this test is about
; empty. The probe's 'cached' field asserts that it is not.
php_admin_value[opcache.file_update_protection] = 0
[classic]
listen = 127.0.0.1:$classicPort
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /classic.php
http.read_timeout = 10000
http.max_body = 1M
php_admin_value[opcache.enable] = 1
; The test writes shared.php a moment before FPM starts, and opcache refuses
; to cache a file whose mtime is within opcache.file_update_protection (2s by
; default) of now -- which would leave the shared SHM entry this test is about
; empty. The probe's 'cached' field asserts that it is not.
php_admin_value[opcache.file_update_protection] = 0
CFG;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* Worker pool first: shared.php's SHM entry gets created here, so if it
     * were ever wrongly foldable this is where the wrong answer would be
     * baked in. */
    $body = file_get_contents("http://127.0.0.1:$workerPort/");
    check(is_string($body), 'worker child did not answer -- see the note below if it crashed');
    $row = json_decode($body, true);
    check(is_array($row), 'worker response not json: ' . var_export($body, true));
    check($row['opcacheEnabled'] === true, 'opcache is not actually enabled in the worker child -- this test proves nothing without it');
    check($row['cached'] === true, 'shared.php is not in the opcache SHM in the worker child -- nothing can be baked into an entry that does not exist');
    check($row['hasWorkerLoop'] === true, 'function_exists(fpmng_worker_loop) folded to false in the worker child');
    check($row['hasBogus'] === false, 'function_exists() on a nonexistent name folded to true');
    check($row['definedRead'] === true, 'defined(FPMNG_WORKER_READ) is false in the worker child');
    check(is_int($row['readValue']), 'FPMNG_WORKER_READ did not resolve in the worker child');
    echo "worker-opcache-folding: ok\n";

    /* Classic pool second, same shared.php file: must independently evaluate
     * function_exists() rather than inherit whatever the worker child's
     * compile baked into the shared opcache entry. */
    $classicBody = file_get_contents("http://127.0.0.1:$classicPort/");
    check(is_string($classicBody), 'classic child did not answer');
    $classicRow = json_decode($classicBody, true);
    check(is_array($classicRow), 'classic response not json: ' . var_export($classicBody, true));
    check($classicRow['opcacheEnabled'] === true, 'opcache is not actually enabled in the classic child -- this test proves nothing without it');
    check($classicRow['cached'] === true, 'shared.php is not in the opcache SHM in the classic child -- it did not read the entry the worker child created');
    check($classicRow['hasWorkerLoop'] === false, 'worker builtins leaked into the classic executor (directly, or via a shared opcache cache entry)');
    check($classicRow['definedRead'] === false, 'defined(FPMNG_WORKER_READ) folded to true in the classic executor (issue #149)');
    check($classicRow['readValue'] === null, 'FPMNG_WORKER_READ resolved in the classic executor, so its value was folded into the shared opcache entry (issue #149)');
    echo "classic-opcache-no-leak: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/shared.php");
    @unlink("$root/worker.php");
    @unlink("$root/classic.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
worker-opcache-folding: ok
classic-opcache-no-leak: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
