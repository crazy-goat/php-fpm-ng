<?php

/*
 * A ReactPHP event loop over the libevent base owned by an FPM worker running
 * pool.type = http-direct with pool.executor = worker (task 073).
 *
 * This is the second consumer of those primitives and the reason task 075
 * exists: everything before it went through one Revolt driver, which cannot
 * distinguish "libevent primitives" from "the four methods Revolt's
 * AbstractDriver needs". Nothing here is shared with
 * ../http-direct-worker/FpmngDriver.php, because LoopInterface and Revolt's
 * Driver share no interface — the only thing the two files have in common is
 * the five SAPI builtins they call.
 *
 * It is a direct transposition of ReactPHP's own ext-event loop
 * (react/event-loop v1.6.0, src/ExtEventLoop.php), which drives libevent 2
 * through the `event` PECL extension. Compare the two when ReactPHP changes;
 * where this file departs from it, there is a comment saying why.
 *
 * Method -> builtin, for the README's table:
 *   addReadStream    FPMNG_WORKER_READ   watcher (EV_READ|EV_PERSIST)
 *   addWriteStream   FPMNG_WORKER_WRITE  watcher (EV_WRITE|EV_PERSIST)
 *   addTimer         FPMNG_WORKER_TIMER  watcher, one-shot
 *   addPeriodicTimer FPMNG_WORKER_TIMER  watcher, re-armed by this file
 *   futureTick       no builtin at all — see run()
 *   run              fpmng_worker_loop()
 *   stop             fpmng_worker_loop_break()
 *   addSignal        unsupported on purpose
 */

declare(strict_types=1);

namespace Fpmng\React;

use React\EventLoop\LoopInterface;
use React\EventLoop\Tick\FutureTickQueue;
use React\EventLoop\Timer\Timer;
use React\EventLoop\TimerInterface;

final class FpmngLoop implements LoopInterface
{
    private FutureTickQueue $futureTickQueue;

    /** @var array<int, int> (int) $stream => SAPI watcher id */
    private array $readEvents = [];

    /** @var array<int, int> (int) $stream => SAPI watcher id */
    private array $writeEvents = [];

    /** @var array<int, callable> */
    private array $readListeners = [];

    /** @var array<int, callable> */
    private array $writeListeners = [];

    /** @var \SplObjectStorage<TimerInterface, int> timer => SAPI watcher id */
    private \SplObjectStorage $timerEvents;

    private bool $running = false;

    public static function isSupported(): bool
    {
        return \function_exists('fpmng_worker_loop');
    }

    public function __construct()
    {
        if (!self::isSupported()) {
            throw new \BadMethodCallException(
                'FpmngLoop requires a pool.executor = worker child of pool.type = http-direct'
            );
        }

        $this->futureTickQueue = new FutureTickQueue();
        $this->timerEvents = new \SplObjectStorage();
    }

    public function addReadStream($stream, $listener): void
    {
        $key = (int) $stream;
        // Second listener for the same stream is ignored, as in every upstream
        // loop (ExtEventLoop::addReadStream()).
        if (isset($this->readListeners[$key])) {
            return;
        }

        // The watcher is created before the listener is recorded, which is the
        // opposite order from ExtEventLoop. event_create() throws for a stream
        // with no usable descriptor (fpm_http_direct_worker.c:816-830); doing
        // it first means an application that catches that exception is left
        // with neither half, instead of a listener with no watcher that
        // removeReadStream() would not clear and addReadStream() would refuse
        // to replace.
        //
        // No readRefs array here, unlike ExtEventLoop: the SAPI takes its own
        // ZVAL_COPY of the stream (fpm_http_direct_worker.c:836-838), so the
        // resource cannot be collected while the watcher lives.
        $id = \fpmng_worker_event_create(
            \FPMNG_WORKER_READ,
            $stream,
            function () use ($key, $stream): void {
                // Re-read from the array: a listener that removed and re-added
                // this stream must not keep being called through a stale
                // closure. The watcher is EV_PERSIST, so it stays armed.
                if (isset($this->readListeners[$key])) {
                    ($this->readListeners[$key])($stream);
                }
            }
        );

        $this->readListeners[$key] = $listener;
        $this->readEvents[$key] = $id;
        \fpmng_worker_event_enable($id);
    }

    public function addWriteStream($stream, $listener): void
    {
        $key = (int) $stream;
        if (isset($this->writeListeners[$key])) {
            return;
        }

        // Watcher first, then the listener — see addReadStream().
        $id = \fpmng_worker_event_create(
            \FPMNG_WORKER_WRITE,
            $stream,
            function () use ($key, $stream): void {
                if (isset($this->writeListeners[$key])) {
                    ($this->writeListeners[$key])($stream);
                }
            }
        );

        $this->writeListeners[$key] = $listener;
        $this->writeEvents[$key] = $id;
        \fpmng_worker_event_enable($id);
    }

    public function removeReadStream($stream): void
    {
        $key = (int) $stream;

        if (isset($this->readEvents[$key])) {
            \fpmng_worker_event_free($this->readEvents[$key]);
            unset($this->readEvents[$key], $this->readListeners[$key]);
        }
    }

    public function removeWriteStream($stream): void
    {
        $key = (int) $stream;

        if (isset($this->writeEvents[$key])) {
            \fpmng_worker_event_free($this->writeEvents[$key]);
            unset($this->writeEvents[$key], $this->writeListeners[$key]);
        }
    }

    public function addTimer($interval, $callback): TimerInterface
    {
        $timer = new Timer($interval, $callback, false);

        $id = \fpmng_worker_event_create(\FPMNG_WORKER_TIMER, null, function () use ($timer): void {
            // Free before calling: this is the advertised cancellation idiom
            // and it is safe from inside the watcher's own callback
            // (fpm_worker_watcher_dtor, fpm_http_direct_worker.c:268-281).
            // Doing it first also means a callback that calls cancelTimer() on
            // itself finds nothing left to free, rather than double-freeing.
            $this->cancelTimer($timer);
            ($timer->getCallback())($timer);
        });
        $this->timerEvents[$timer] = $id;
        \fpmng_worker_event_enable($id, (float) $timer->getInterval());

        return $timer;
    }

    public function addPeriodicTimer($interval, $callback): TimerInterface
    {
        $timer = new Timer($interval, $callback, true);

        $id = \fpmng_worker_event_create(\FPMNG_WORKER_TIMER, null, function () use ($timer): void {
            // Re-armed here because FPMNG_WORKER_TIMER is one-shot by design
            // (fpm_http_direct_worker.c:808-810) — ExtEventLoop gets
            // Event::PERSIST from the extension instead. Re-arming *before*
            // the callback rather than after keeps the period from drifting by
            // the callback's own duration; a callback that cancels itself is
            // still correct, because cancelTimer() frees the watcher and
            // event_free() cancels a pending event.
            if (isset($this->timerEvents[$timer])) {
                \fpmng_worker_event_enable($this->timerEvents[$timer], (float) $timer->getInterval());
            }
            ($timer->getCallback())($timer);
        });
        $this->timerEvents[$timer] = $id;
        \fpmng_worker_event_enable($id, (float) $timer->getInterval());

        return $timer;
    }

    public function cancelTimer(TimerInterface $timer): void
    {
        if (isset($this->timerEvents[$timer])) {
            \fpmng_worker_event_free($this->timerEvents[$timer]);
            unset($this->timerEvents[$timer]);
        }
    }

    public function futureTick($listener): void
    {
        $this->futureTickQueue->add($listener);
    }

    /**
     * Signal watchers are deliberately absent from the SAPI: the FPM master
     * owns SIGQUIT/SIGUSR2 and the worker lifecycle, and a userland watcher
     * competing for them would break graceful reload. Shutdown reaches the
     * application through the notification stream instead (FpmngReactServer).
     */
    public function addSignal($signal, $listener): void
    {
        throw new \BadMethodCallException(
            'Signal handling is owned by the FPM master under pool.executor = worker'
        );
    }

    public function removeSignal($signal, $listener): void
    {
        throw new \BadMethodCallException(
            'Signal handling is owned by the FPM master under pool.executor = worker'
        );
    }

    public function run(): void
    {
        $this->running = true;

        while ($this->running) {
            $this->futureTickQueue->tick();

            // The whole of futureTick() lives in this decision, and it is why
            // fpmng_worker_loop() takes a $blocking argument at all: a pending
            // tick must run on the next iteration, so the loop may not go to
            // sleep in libevent waiting for a descriptor that nothing is
            // waiting on. Revolt has no equivalent, so no earlier test ever
            // passed false here for this reason.
            $blocking = $this->running && $this->futureTickQueue->isEmpty();

            // Tested on the watcher arrays rather than the listener arrays,
            // as ExtEventLoop does: a registered watcher is what can actually
            // wake fpmng_worker_loop(), and a loop that stays here with no
            // events registered would spin — event_base_loop() returns
            // immediately in that state rather than sleeping.
            if ($blocking
                && !$this->readEvents
                && !$this->writeEvents
                && $this->timerEvents->count() === 0
            ) {
                // Nothing can ever wake us: LoopInterface::run() must return.
                break;
            }

            \fpmng_worker_loop($blocking);
        }
    }

    public function stop(): void
    {
        $this->running = false;
        // The call above is not enough on its own: stop() is normally called
        // from inside a watcher callback, i.e. from inside the
        // event_base_loop() that fpmng_worker_loop() is running, and without
        // loopbreak that iteration would keep dispatching.
        \fpmng_worker_loop_break();
    }
}
