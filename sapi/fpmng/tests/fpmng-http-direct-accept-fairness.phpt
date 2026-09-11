--TEST--
fpm-ng: direct HTTP spreads a burst over idle workers instead of one (issue #53)
--SKIPIF--
<?php
include "skipif.inc";
?>
--FILE--
<?php
require_once "tester.inc";

// Issue #53. Every child accepts on the one listening socket the master opened,
// and libevent's listener drains the whole accept queue in a single loop
// iteration. A child that wakes first therefore used to take an entire burst
// for itself and then serve it one request at a time, with its event loop
// blocked for the whole of each, while its sibling sat idle.
//
// The shape below is the one that made that visible and is cheap to assert: one
// slow request and several fast ones, all connected before any of them is
// written, so they arrive as one burst. The fast ones must be served by the
// other worker while the slow one sleeps. The threshold is an order of
// magnitude above what the fixed path measures (~1.5 ms) and an order of
// magnitude below the sleep, so it is not a timing-sensitive number.
//
// Three rounds, two of which must be clean. The gate is not a scheduler: it
// stops the accept drain after one connection, but a child whose request
// completes in microseconds re-opens accepting and can legitimately take a
// second connection out of the same burst -- including, rarely, the slow one.
// Measured on the poligon over three repeats: before the gate, zero rounds were
// clean at this shape (a fast request waited 1001, 1003 and 1005 ms); after it,
// three of three. Requiring two of three therefore still fails hard on the old
// behaviour while leaving room for that one legitimate interleaving.

$connections = 8;
$sleepMs = 1000;
$fastLimitMs = 400;
$rounds = 3;
$cleanRequired = 2;

$root = __DIR__;
$script = '/fpmng-http-direct-accept-fairness-front-' . getmypid() . '.php';
file_put_contents($root . $script, <<<'PHP'
<?php
if (isset($_GET['slow'])) {
    usleep((int) $_GET['slow'] * 1000);
}
echo getmypid();
PHP);

$port = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054) + 12;
$addr = "127.0.0.1:$port";
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = $addr
pool.type = http-direct
pm = static
pm.max_children = 2
chdir = $root
http.front_controller = $script
CFG;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    // One round of the burst. Returns the per-response elapsed times in ms,
    // keyed the same way as the connections, with index 0 the slow one.
    $round = function () use ($addr, $connections, $sleepMs): array {
        // Every connection is established before any request is written, so the
        // kernel has the whole burst queued when the first child wakes.
        $sockets = [];
        for ($i = 0; $i < $connections; $i++) {
            $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
            if (!$fp) {
                throw new RuntimeException("connect #$i: $error ($errno)");
            }
            stream_set_blocking($fp, false);
            $sockets[$i] = $fp;
        }
        $started = microtime(true);
        foreach ($sockets as $i => $fp) {
            $query = $i === 0 ? "slow=$sleepMs" : '';
            fwrite($fp, "GET /?$query HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
        }

        // Timestamped as each response completes, rather than read in order: a
        // sequential read would block on the slow request and destroy the very
        // measurement this test is making.
        $raw = array_fill(0, $connections, '');
        $elapsed = [];
        $deadline = microtime(true) + 20;
        while ($sockets && microtime(true) < $deadline) {
            $read = $sockets;
            $write = $except = null;
            if (!stream_select($read, $write, $except, 1)) {
                continue;
            }
            foreach ($read as $fp) {
                $i = array_search($fp, $sockets, true);
                $chunk = fread($fp, 8192);
                if ($chunk !== false && $chunk !== '') {
                    $raw[$i] .= $chunk;
                }
                if ($chunk === '' || feof($fp)) {
                    $elapsed[$i] = (microtime(true) - $started) * 1000;
                    fclose($fp);
                    unset($sockets[$i]);
                }
            }
        }
        foreach ($sockets as $i => $fp) {
            fclose($fp);
            throw new RuntimeException("response #$i never completed");
        }

        foreach ($raw as $i => $text) {
            [$head, $body] = array_pad(explode("\r\n\r\n", $text, 2), 2, '');
            if (!str_contains($head, ' 200 ')) {
                throw new RuntimeException("response #$i is not a 200: " . strtok($head, "\r\n"));
            }
            if ((int) $body <= 0) {
                throw new RuntimeException("response #$i carries no pid");
            }
        }
        if ($elapsed[0] < $sleepMs) {
            throw new RuntimeException('the slow request did not actually sleep');
        }
        ksort($elapsed);
        return $elapsed;
    };

    $clean = 0;
    $worst = [];
    for ($r = 0; $r < $rounds; $r++) {
        $elapsed = $round();
        $fast = $elapsed;
        unset($fast[0]);
        $worst[] = sprintf('%.0f', max($fast));
        if (max($fast) < $fastLimitMs) {
            $clean++;
        }
    }
    if ($clean < $cleanRequired) {
        throw new RuntimeException(sprintf(
            'only %d of %d rounds kept every fast request off the sleeper; slowest fast request per round: %s ms',
            $clean, $rounds, implode(', ', $worst)));
    }
    echo "burst-spread: ok\n";

    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
} finally {
    @unlink($root . $script);
}
echo "Done\n";
?>
--EXPECT--
burst-spread: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
