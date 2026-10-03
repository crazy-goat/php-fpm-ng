<?php

/*
 * Bridges the worker's request queue to a ReactPHP application: one promise
 * per request, so a handler waiting on I/O does not stop the others.
 *
 * The counterpart for amphp is ../http-direct-worker/FpmngServer.php, which
 * starts a fiber per request with Amp\async(). This one starts nothing: a
 * handler returns a promise and the response is sent from its callback. That
 * is not a stylistic difference — it is the point of task 075. Fibers were
 * what tasks 073 and 074 proved; if the worker's primitives only worked under
 * a fiber-based scheduler, plain promise callbacks driven straight off
 * libevent watchers would not complete a request here.
 *
 * The SAPI hands over exactly two things: a notification stream that becomes
 * readable when something happened, and a queue of request ids. That single
 * read listener is also what keeps run() from returning — with no listeners,
 * no writers and no timers, LoopInterface::run() must return (FpmngLoop::run(),
 * and ExtEventLoop::run() upstream), and the worker would exit.
 */

declare(strict_types=1);

namespace Fpmng\React;

use React\EventLoop\Loop;

use function React\Promise\resolve;

final class FpmngReactServer
{
    /** @var resource|null */
    private $notify = null;

    /**
     * @param \Closure(array<string, string>, string): mixed $handler
     *        Receives the CGI-shaped request environment and the request body,
     *        returns either [status, headers, body] or a promise for it. There
     *        is no per-request output buffer in this mode: `echo` belongs to
     *        the worker and goes to stderr, so a handler returns its body
     *        instead of printing it.
     */
    public function __construct(private readonly \Closure $handler)
    {
    }

    public function run(): void
    {
        // Before anything else touches Loop::get(), which would otherwise
        // build a StreamSelectLoop — a second event loop, blocking in
        // stream_select() on descriptors the worker's libevent base is also
        // watching. Every react/* library reaches the loop through this class,
        // so this one line is the whole installation.
        Loop::set(new FpmngLoop());

        $this->notify = \fpmng_worker_notify_stream();

        Loop::addReadStream($this->notify, function ($notify): void {
            // Level-triggered: the pipe must be drained or this listener fires
            // on every single loop iteration for ever.
            \fread($notify, 65536);

            while (($id = \fpmng_worker_next_request()) !== null) {
                $this->serve($id);
            }

            $this->stopWhenDrained();
        });

        Loop::run();
    }

    private function serve(int $id): void
    {
        $done = function () use ($id): void {
            // Idempotent safety net: respond() returns false for an id that was
            // already answered, while a request that is never answered holds
            // its pending slot for the life of the worker. Reachable when the
            // handler resolves with a status or body respond() itself rejects.
            \fpmng_worker_respond($id, 500, [], "Internal Server Error\n");
            $this->stopWhenDrained();
        };

        try {
            $promise = resolve(($this->handler)(
                \fpmng_worker_request_env($id),
                \fpmng_worker_request_body($id)
            ));
        } catch (\Throwable $e) {
            // A handler that throws synchronously never returns a promise, so
            // this path has to answer the request itself.
            $this->fail($e);
            $done();

            return;
        }

        $promise->then(
            function (array $response) use ($id): void {
                [$status, $headers, $body] = $response;
                \fpmng_worker_respond($id, $status, $headers, $body);
            },
            function (\Throwable $e): void {
                $this->fail($e);
            }
        )->then($done, function (\Throwable $e) use ($done): void {
            // Not ->then($done, $done): $done answers the request but says
            // nothing, and the failures that reach here are exactly the ones
            // worth a log line — a handler resolving with something that is not
            // [status, headers, body] (TypeError from the callback above), or
            // respond() rejecting a status outside 200..599 or a body over
            // 8 MiB (ValueError, fpm_http_direct_worker.c:714-721). Silently
            // returning a bare 500 for those would be a diagnosability
            // regression against ../http-direct-worker/FpmngServer.php.
            $this->fail($e);
            $done();
        });
    }

    private function fail(\Throwable $e): void
    {
        // stderr, i.e. the FPM error log with catch_workers_output = yes.
        \fwrite(\STDERR, 'http-direct worker: handler failed: ' . $e . \PHP_EOL);
    }

    /**
     * SIGQUIT and pm.max_requests both surface as a stop request, and the
     * transport refuses new requests from that moment on, so draining is
     * finite.
     *
     * This asks the SAPI rather than counting requests itself. An in-flight
     * counter sees only what fpmng_worker_next_request() already handed over,
     * and the SAPI has a queue behind it (fw.ready): a request can land there
     * during the very loop iteration in which the last in-flight response
     * trips pm.max_requests, and the read listener will not be polled again
     * before Loop::stop() takes effect. Task 075 worked around that by
     * draining the queue once more here; fpmng_worker_may_exit() removes the
     * need to know the queue exists at all — it is true only when the stop was
     * requested *and* nothing accepted is still unanswered (task 080).
     */
    private function stopWhenDrained(): void
    {
        if (!\fpmng_worker_may_exit()) {
            return;
        }

        if ($this->notify !== null) {
            Loop::removeReadStream($this->notify);
            $this->notify = null;
        }

        Loop::stop();
    }
}
