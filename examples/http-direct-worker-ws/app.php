<?php

/*
 * Native WebSocket on pool.executor = worker — the runnable example for
 * docs/http-direct.md's "WebSocket" section (issue #343).
 *
 * Dependency-free on purpose: the ONLY thing C contributes is
 * fpmng_worker_upgrade() — the 101 handshake and the hijacked connection as an
 * ordinary PHP stream. RFC 6455 framing (opcode, mask, fragmentation,
 * ping/pong, close codes) is the ~80 lines of userland codec below; in a real
 * application that job belongs to amphp/websocket-server or ratchet/rfc6455
 * running on this same stream.
 *
 * Routes:
 *   /ws  — upgrade; echoes every text frame back prefixed with "echo: ",
 *          answers pings with pongs, replies to close with close
 *   anything else — an ordinary one-shot response proving the same worker
 *          still serves plain requests while frames flow
 */

declare(strict_types=1);

/* ---------------------------- the userland codec (RFC 6455 section 5) ---- */

function wsEncode(string $payload, int $op = 0x1): string
{
    $len = strlen($payload);
    $head = chr($op | 0x80);                       /* FIN */
    if ($len < 126) {
        $head .= chr($len);
    } elseif ($len < 65536) {
        $head .= pack('n', 126) . pack('n', $len);
    } else {
        $head .= pack('n', 127) . pack('J', $len);
    }
    return $head . $payload;                        /* server frames are unmasked */
}

/** @return list<array{op: int, data: string}> */
function wsDecode(string $data): array
{
    $frames = [];
    $off = 0;
    while ($off + 2 <= strlen($data)) {
        $op = ord($data[$off]) & 0x0f;
        $masked = (ord($data[$off + 1]) & 0x80) !== 0;
        $len = ord($data[$off + 1]) & 0x7f;
        $off += 2;
        if ($len === 126) {
            $len = unpack('n', substr($data, $off, 2))[1];
            $off += 2;
        } elseif ($len === 127) {
            $len = unpack('J', substr($data, $off, 8))[1];
            $off += 8;
        }
        $mask = $masked ? substr($data, $off, 4) : '';
        $off += $masked ? 4 : 0;
        $payload = substr($data, $off, $len);
        $off += $len;
        if ($masked) {
            for ($i = 0; $i < strlen($payload); $i++) {
                $payload[$i] = $payload[$i] ^ $mask[$i % 4];
            }
        }
        $frames[] = ['op' => $op, 'data' => $payload];
    }
    return $frames;
}

/* ---------------------------- the application ---------------------------- */

$notify = fpmng_worker_notify_stream();
$ws = null;                 /* the upgraded stream */
$closed = false;

/* Retirement: the driver sees fpmng_worker_stopping() and sends the close
 * frame the protocol owes the client — C only guarantees the fd is closed. */
$timer = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () use (&$ws, &$timer, &$closed): void {
    if (fpmng_worker_stopping() && $ws !== null && !$closed) {
        $closed = true;
        fwrite($ws, wsEncode(pack('n', 1001), 0x8));   /* Going Away */
        /* The frame is only queued; the write watcher is what says it reached
         * the wire (issue #343's "the codec waits" contract). */
        $closeWatcher = fpmng_worker_event_create(FPMNG_WORKER_WRITE, $ws, function () use (&$ws, &$timer): void {
            fpmng_worker_event_free($timer);
            fclose($ws);
            $ws = null;
        });
        fpmng_worker_event_enable($closeWatcher);
        return;
    }
    fpmng_worker_event_enable($timer, 0.1);
});
fpmng_worker_event_enable($timer, 0.1);

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, &$ws): void {
    /* Level-triggered: drain or this watcher fires on every loop iteration. */
    fread($notify, 65536);

    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);

        if (!str_contains($env['REQUEST_URI'] ?? '/', '/ws')) {
            /* Ordinary requests keep being answered while frames flow — one
             * worker, one event loop. */
            fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'],
                'hello from pid ' . getmypid());
            continue;
        }

        /* The one builtin: 101 out, connection in, as a php_stream. Throws
         * ValueError for a non-upgrade request, which then stays answerable. */
        $ws = fpmng_worker_upgrade($id, []);

        $drain = function () use (&$ws): void {
            if ($ws === null) {
                return;
            }
            /* Read until short: fread() returns everything buffered, '' when
             * there is none — feof() only after the peer actually closed. */
            $data = '';
            while (true) {
                $chunk = fread($ws, 8192);
                if ($chunk === false || $chunk === '') {
                    break;
                }
                $data .= $chunk;
            }
            foreach (wsDecode($data) as $frame) {
                switch ($frame['op']) {
                    case 0x8:                       /* close */
                        fwrite($ws, wsEncode($frame['data'], 0x8));
                        fclose($ws);                /* tears the connection down */
                        $ws = null;
                        return;
                    case 0x9:                       /* ping -> pong */
                        fwrite($ws, wsEncode($frame['data'], 0xA));
                        break;
                    case 0x1:                       /* text */
                    case 0x2:                       /* binary */
                        fwrite($ws, wsEncode('echo: ' . $frame['data'], $frame['op']));
                        break;
                }
            }
        };

        $wsWatcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $ws, $drain);
        fpmng_worker_event_enable($wsWatcher);
        /* Frames may have arrived together with the handshake response, before
         * the watcher existed — drain once by hand; the watcher takes over. */
        $drain();
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit() || $ws !== null) {
    fpmng_worker_loop(true);
}
