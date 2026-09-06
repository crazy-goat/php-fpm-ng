<?php
/* Reference test for fiber.isolate_statics (fpm_pool_coop_statics.c).
 *
 * The spike (tasks/008-laravel-class-statics.md) explicitly did not test
 * `$x = &Class::$static;` across a suspension point. This script does.
 *
 * Run it as pool.executor = fiber, with
 *   fiber.isolate_statics = FpmNgStaticsTest::counter
 * and pm.max_children = 1, so two concurrent requests really do interleave
 * on one process. Call it twice concurrently with a different ?tag=, each
 * using its own cookie/connection, e.g.:
 *
 *   curl 'http://host:port/index.php/statics_reference.php?tag=A' &
 *   curl 'http://host:port/index.php/statics_reference.php?tag=B' &
 *   wait
 *
 * Each response is one JSON object. Correct behaviour, per response:
 *  - "ref_matches_static_before_sleep" and "...after_sleep": true -- the
 *    reference and the static property still see each other's writes
 *    across the fiber suspension caused by usleep() (sleep/usleep yield
 *    the fiber in this executor, see docs/fiber_async_io.md).
 *  - "isolated_from_other": true -- this request's own counter sequence
 *    (0 -> 1 -> 2) was not perturbed by the other tag's concurrent writes
 *    to the *same* class static, i.e. fiber.isolate_statics actually kept
 *    them apart, not just kept the reference internally consistent.
 */

class FpmNgStaticsTest {
	public static int $counter = 0;
}

header('Content-Type: application/json');

$tag = $_GET['tag'] ?? 'X';
$pid = getmypid();

// Establish a reference into the isolated static *before* any suspension --
// this is the case the spike never exercised.
$ref = &FpmNgStaticsTest::$counter;

$ref = 1;
$before_sleep_ref = $ref;
$before_sleep_static = FpmNgStaticsTest::$counter;
$before_sleep_match = ($before_sleep_ref === $before_sleep_static);

// Yields the fiber (docs/fiber_async_io.md: usleep is one of the hooked
// functions). While suspended, another concurrent request touching the
// *same* class static runs on this same worker process.
usleep(300000);

$after_sleep_static_seen = FpmNgStaticsTest::$counter;
$isolated_from_other = ($after_sleep_static_seen === 1);

$ref++;
$after_sleep_ref = $ref;
$after_sleep_static = FpmNgStaticsTest::$counter;
$after_sleep_match = ($after_sleep_ref === $after_sleep_static) && ($after_sleep_ref === 2);

echo json_encode([
	'tag' => $tag,
	'pid' => $pid,
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
