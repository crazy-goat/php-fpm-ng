<?php

/*
 * Server-Sent Events on pool.executor = worker — the runnable example for
 * docs/http-direct.md's "Server-Sent Events" section (issue #342).
 *
 * Dependency-free on purpose: the neighbouring examples/http-direct-worker/
 * proves the primitives are sufficient for a full Revolt driver; this one
 * shows only what an SSE endpoint adds on top of them, so it uses the raw
 * fpmng_worker_* builtins and one Fiber per stream.
 *
 * Routes:
 *   GET /events?user=N   open a stream; replays events after ?last_event_id=
 *                        or the Last-Event-ID header, then ": ping" every 15 s
 *   GET /publish?msg=... append one event; fanned out to every open stream on
 *                        THIS worker (cross-worker fan-out is issue #182's
 *                        question and deliberately not answered here)
 */

declare(strict_types=1);

/* The per-worker event log every stream reads from its own cursor. Publishing
 * is appending to one array, so fan-out needs no wakeup primitive: each stream
 * polls on its heartbeat timer. That trades latency for simplicity — a real
 * application would use the cross-worker primitive #182 names instead. */
final class Broadcast
{
    /** @var list<array{id: int, data: string}> */
    private array $events = [];
    private int $nextId = 1;

    public function publish(string $data): int
    {
        $this->events[] = ['id' => $this->nextId, 'data' => $data];
        return $this->nextId++;
    }

    /** All events the stream with the given cursor has not seen yet. */
    public function since(int $lastId): array
    {
        return array_values(array_filter($this->events, fn (array $e): bool => $e['id'] > $lastId));
    }
}

$broadcast = new Broadcast();

/* One Fiber per open stream, keyed by the SAPI's request id. The SAPI caps
 * this at worker.max_pending slots — an SSE pool's size IS that directive. */
$streams = [];

function suspendFor(float $seconds): void
{
    $fiber = Fiber::getCurrent();
    $box = new stdClass();
    $box->id = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () use ($fiber, $box) {
        fpmng_worker_event_free($box->id);
        $fiber->resume();
    });
    fpmng_worker_event_enable($box->id, $seconds);
    Fiber::suspend();
}

/* A false chunk result is ambiguous: the SAPI also uses it when the live
 * connection's output buffer reached worker.send_buffer_limit. Retry the same
 * chunk after a short timer; an empty request_env means the client is gone or
 * reaped, while a live request is retried until it drains, the worker stops, or
 * the 10s retry ceiling expires. The caller then closes the stream cleanly. */
function respondChunkWhenWritable(int $id, string $data): bool
{
    $deadline = hrtime(true) + 10_000_000_000;
    while (true) {
        if (fpmng_worker_stopping()) {
            return false;
        }
        if (fpmng_worker_respond_chunk($id, $data)) {
            return true;
        }
        if (fpmng_worker_request_env($id) === [] || hrtime(true) >= $deadline) {
            return false;
        }
        suspendFor(0.05);
    }
}

function endSseStream(int $id): void
{
    if (fpmng_worker_request_env($id) !== []) {
        /* Best effort: if even this small chunk is refused, end still queues
         * the terminal chunk and releases the slot instead of orphaning it. */
        fpmng_worker_respond_chunk($id, "event: bye\ndata: reconnect\n\n");
        fpmng_worker_respond_end($id);
    }
}

$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, $broadcast, &$streams): void {
    /* Level-triggered: drain or the watcher fires on every loop iteration. */
    fread($notify, 65536);

    /* Semantics 3 of the docs section: clients that walked away are reported
     * here by id, oldest first, and each dead id's fiber is dropped — before
     * #342 the driver learned of a dead client only from the next _chunk()
     * returning false, up to one heartbeat late. */
    foreach (fpmng_worker_closed_requests() as $closedId) {
        unset($streams[$closedId]);
    }

    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        if (!str_starts_with($env['REQUEST_URI'] ?? '/', '/events')) {
            /* Everything that is not a stream is an ordinary one-shot
             * response. /publish appends to the log; every open stream picks
             * it up on its next heartbeat. */
            parse_str($env['QUERY_STRING'] ?? '', $query);
            $eventId = $broadcast->publish((string) ($query['msg'] ?? ''));
            fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], "published $eventId\n");
            continue;
        }
        $streams[$id] = new Fiber(function () use ($id, $env, $broadcast): void {
            if (!fpmng_worker_respond_start($id, 200, [
                'Content-Type' => 'text/event-stream',
                'Cache-Control' => 'no-cache',
            ])) {
                return;    // HTTP/1.0 client, gone, bodyless status — the docs list them all
            }
            /* Last-Event-ID: header on reconnect (HTTP_LAST_EVENT_ID — every
             * request header arrives CGI-style), or the ?last_event_id= query
             * for testing without an EventSource. */
            parse_str($env['QUERY_STRING'] ?? '', $query);
            $lastId = (int) ($env['HTTP_LAST_EVENT_ID'] ?? $query['last_event_id'] ?? 0);
            foreach ($broadcast->since($lastId) as $event) {
                if (!fpmng_worker_respond_chunk($id, "id: {$event['id']}\ndata: {$event['data']}\n\n")) {
                    return;
                }
            }
            while (!fpmng_worker_stopping()) {
                suspendFor(15.0);
                foreach ($broadcast->since($lastId) as $event) {
                    if (!respondChunkWhenWritable($id, "id: {$event['id']}\ndata: {$event['data']}\n\n")) {
                        endSseStream($id); // gone, worker stopping, or retry ceiling reached
                        return;
                    }
                }
                if (!respondChunkWhenWritable($id, ": ping\n\n")) {
                    endSseStream($id);
                    return;
                }
            }
            /* The worker is retiring. Ending cleanly here — not waiting for
             * the SAPI's shutdown path — is what lets the EventSource
             * reconnect with its Last-Event-ID (docs semantics 2). */
            endSseStream($id);
        });
        $streams[$id]->start();
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
