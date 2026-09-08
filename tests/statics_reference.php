<?php
/* Reference test for fiber.isolate_statics (fpm_pool_coop_statics.c).
 *
 * The spike (task 008, done; see docs/task-archive.md) explicitly did not test
 * `$x = &Class::$static;` across a suspension point. This script does.
 *
 * Suspension point: a real blocking MySQL query (`SELECT SLEEP(?)`) through
 * mysqli, which goes through mysqlnd and is genuinely intercepted by this
 * executor (docs/fiber_async_io.md, "Intercepted today" -- mysqlnd). Plain
 * usleep()/sleep() do NOT suspend the fiber in this build -- per the same
 * document, they block the *whole process* instead, so they cannot be used
 * to force two requests to interleave; an earlier version of this script
 * used usleep() and silently exercised no real concurrency at all.
 *
 * Run it as pool.executor = fiber, with
 *   fiber.isolate_statics = ...,FpmNgStaticsTest::counter
 * and pm.max_children = 1, so two concurrent requests really do interleave
 * on one process. Call it twice concurrently with a different ?tag=, e.g.:
 *
 *   curl 'http://host:port/statics_reference.php?tag=A&sleep_ms=300' &
 *   curl 'http://host:port/statics_reference.php?tag=B&sleep_ms=50' &
 *   wait
 *
 * Each response is one JSON object. Correct behaviour, per response:
 *  - "ref_matches_static_before_sleep" and "...after_sleep": true -- the
 *    reference and the static property still see each other's writes
 *    across the fiber suspension.
 *  - "isolated_from_other": true -- this request's own counter sequence
 *    (0 -> 1 -> 2) was not perturbed by the other tag's concurrent writes
 *    to the *same* class static, i.e. fiber.isolate_statics actually kept
 *    them apart, not just kept the reference internally consistent.
 *  - "t_start"/"t_woke" let the caller confirm two tagged requests actually
 *    overlapped in wall-clock time (the point of the test), not just that
 *    both happened to run one after the other.
 */

// This whole file is re-executed on every request (it is hit directly, not
// through require_once), unlike Laravel's own bootstrap -- so the class
// declaration needs the same guard any repeatedly-included script needs
// under FPMNG_SHARED_INCLUDES=1 (fpm_pool_coop.c).
if (!class_exists('FpmNgStaticsTest', false)) {
	class FpmNgStaticsTest {
		public static int $counter = 0;
	}
}

header('Content-Type: application/json');

$tag = $_GET['tag'] ?? 'X';
$sleep_ms = (int) ($_GET['sleep_ms'] ?? 300);
$pid = getmypid();

// Establish a reference into the isolated static *before* any suspension --
// this is the case the spike never exercised.
$t_start = microtime(true);
$ref = &FpmNgStaticsTest::$counter;

$ref = 1;
$before_sleep_ref = $ref;
$before_sleep_static = FpmNgStaticsTest::$counter;
$before_sleep_match = ($before_sleep_ref === $before_sleep_static);

// Real blocking I/O through mysqlnd -- genuinely suspends this fiber and
// lets another connection's fiber run on this same worker process meanwhile.
$mysqli = new mysqli('127.0.0.1', 'bench', 'bench', 'laravel', 3306);
$mysqli->query('SELECT SLEEP(' . ((float) $sleep_ms / 1000) . ')');
$mysqli->close();
$t_woke = microtime(true);

$after_sleep_static_seen = FpmNgStaticsTest::$counter;
$isolated_from_other = ($after_sleep_static_seen === 1);

$ref++;
$after_sleep_ref = $ref;
$after_sleep_static = FpmNgStaticsTest::$counter;
$after_sleep_match = ($after_sleep_ref === $after_sleep_static) && ($after_sleep_ref === 2);

echo json_encode([
	'tag' => $tag,
	'pid' => $pid,
	't_start' => $t_start,
	't_woke' => $t_woke,
	'before_sleep_ref' => $before_sleep_ref,
	'before_sleep_static' => $before_sleep_static,
	'ref_matches_static_before_sleep' => $before_sleep_match,
	'value_seen_right_after_waking' => $after_sleep_static_seen,
	'isolated_from_other' => $isolated_from_other,
	'after_sleep_ref' => $after_sleep_ref,
	'after_sleep_static' => $after_sleep_static,
	'ref_matches_static_after_sleep' => $after_sleep_match,
	'pass' => $before_sleep_match && $isolated_from_other && $after_sleep_match,
]) . "\n";
