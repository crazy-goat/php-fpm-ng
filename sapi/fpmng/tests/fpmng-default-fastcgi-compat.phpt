--TEST--
fpm-ng: a pool with no pool.type behaves as upstream FastCGI (docs/NOTES.md §3i, BC promise)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

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

$tester = new FPM\Tester($cfg, '<?php echo "fastcgi-ok";');
$tester->start();
$tester->expectLogStartNotices();
$tester->request()->expectBody('fastcgi-ok', skipHeadersCheck: true);
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
