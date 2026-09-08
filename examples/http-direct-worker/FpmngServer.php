<?php

/*
 * Bridges the worker's request queue to an amphp application: one fiber per
 * request, so a handler that suspends on I/O does not stop the others.
 *
 * The SAPI hands over exactly two things: a notification stream that becomes
 * readable when something happened, and a queue of request ids. That single
 * readable watcher is also what keeps Revolt's run() from returning — without a
 * referenced callback the loop would consider itself finished and the worker
 * would exit (revolt/event-loop, AbstractDriver::isEmpty()).
 */

declare(strict_types=1);

namespace Fpmng\Poc;

use Revolt\EventLoop;

use function Amp\async;

final class FpmngServer
{
    private int $inFlight = 0;

    private ?string $watcher = null;

    /**
     * @param \Closure(array<string, string>, string): array{int, array<string, string|list<string>>, string} $handler
     *        Receives the CGI-shaped request environment and the request body,
     *        returns [status, headers, body]. There is no per-request output
     *        buffer in this mode: `echo` belongs to the worker and goes to
     *        stderr, so a handler returns its body instead of printing it.
     */
    public function __construct(private readonly \Closure $handler)
    {
    }

    public function run(): void
    {
        EventLoop::setDriver(new FpmngDriver());

        $notify = \fpmng_worker_notify_stream();

        $this->watcher = EventLoop::onReadable($notify, function () use ($notify): void {
            // Level-triggered: the pipe must be drained or this watcher fires
            // on every single loop iteration for ever.
            \fread($notify, 65536);

            while (($id = \fpmng_worker_next_request()) !== null) {
                $this->serve($id);
            }

            $this->stopWhenDrained();
        });

        EventLoop::run();
    }

    private function serve(int $id): void
    {
        $this->inFlight++;

        async(function () use ($id): void {
            try {
                [$status, $headers, $body] = ($this->handler)(
                    \fpmng_worker_request_env($id),
                    \fpmng_worker_request_body($id)
                );
                \fpmng_worker_respond($id, $status, $headers, $body);
            } catch (\Throwable $e) {
                // stderr, i.e. the FPM error log with catch_workers_output = yes.
                \fwrite(\STDERR, 'http-direct worker: handler failed: ' . $e . \PHP_EOL);
            } finally {
                // Idempotent safety net, and it belongs in finally rather than
                // in the catch above: respond() returns false for an id that
                // was already answered, while a request that is never answered
                // holds its pending slot for the life of the worker (see
                // README). Reachable when the handler returns a status or a
                // body respond() itself rejects.
                \fpmng_worker_respond($id, 500, [], "Internal Server Error\n");
                $this->inFlight--;
                $this->stopWhenDrained();
            }
        });
    }

    /**
     * SIGQUIT and pm.max_requests both surface as fpmng_worker_stopping(); the
     * transport has already stopped accepting by then, so draining is finite.
     */
    private function stopWhenDrained(): void
    {
        if (!\fpmng_worker_stopping() || $this->inFlight > 0) {
            return;
        }

        if ($this->watcher !== null) {
            EventLoop::cancel($this->watcher);
            $this->watcher = null;
        }

        EventLoop::getDriver()->stop();
    }
}
