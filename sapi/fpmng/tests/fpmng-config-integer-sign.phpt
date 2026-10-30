--TEST--
fpm-ng: integer directives refuse a leading '-' unless they take a signed value (issue #862)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
?>
--FILE--
<?php

require_once "tester.inc";

$base = <<<EOT
[global]
error_log = {{FILE:LOG}}
[pool]
listen = {{ADDR}}
pm = static
pm.max_children = 1
EOT;

/* Issue #862: fpm_conf_set_integer() skipped a leading '-' and atoi() took
 * it, so a negative value passed -t and reached the directive's use. The
 * refusal uses the setter's own message. listen.backlog = -1 stays valid
 * (the kernel maximum), so only -2 is refused for it. */
$refused = [
    'listen-backlog-below-minus-one' => "\nlisten.backlog = -2",
    'pm-max-requests' => "\npm.max_requests = -1",
    'rlimit-files' => "\nrlimit_files = -1",
];
foreach ($refused as $label => $extra) {
    $tester = new FPM\Tester($base . $extra, '<?php');
    $messages = $tester->testConfig(true);
    $text = $messages === null ? '' : implode("\n", $messages);
    if (!str_contains($text, 'is not a valid number (greater or equal than zero)')) {
        throw new RuntimeException("$label: " . var_export($messages, true));
    }
    echo "$label: refused\n";
}

/* The signed directives: process.priority has its own [-19,20] check later,
 * and rlimit_core keeps its own "unlimited" message for a negative value.
 * listen.backlog accepts -1 alone. */
$signed = [
    'listen-backlog-minus-one' => "\nlisten.backlog = -1",
    'process-priority' => "\nprocess.priority = -5",
    'rlimit-core-unlimited' => "\nrlimit_core = unlimited",
];
foreach ($signed as $label => $extra) {
    $tester = new FPM\Tester($base . $extra, '<?php');
    if ($tester->testConfig() !== null) {
        throw new RuntimeException("$label: refused");
    }
    echo "$label: accepted\n";
}

$tester = new FPM\Tester($base . "\nrlimit_core = -1", '<?php');
$messages = $tester->testConfig(true);
if ($messages === null || !str_contains(implode("\n", $messages), "must be greater than zero or 'unlimited'")) {
    throw new RuntimeException('rlimit-core-negative: ' . var_export($messages, true));
}
echo "rlimit-core-negative: refused\n";
?>
--EXPECT--
listen-backlog-below-minus-one: refused
pm-max-requests: refused
rlimit-files: refused
listen-backlog-minus-one: accepted
process-priority: accepted
rlimit-core-unlimited: accepted
rlimit-core-negative: refused
