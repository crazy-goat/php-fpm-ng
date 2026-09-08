<?php

/*
 * A Revolt event-loop driver over the libevent base owned by an FPM worker
 * running pool.type = http-direct with pool.executor = worker (task 073, POC).
 *
 * This file is the whole integration with amphp, and it lives in userland on
 * purpose: the SAPI (sapi/fpmng/fpm/fpm_http_direct_worker.c) exposes libevent
 * primitives and knows nothing about Revolt, so the library version stays
 * editable without rebuilding PHP. It is a direct transposition of Revolt's own
 * ext-event driver (revolt/event-loop, src/EventLoop/Driver/EventDriver.php) —
 * compare the two when Revolt changes.
 */

declare(strict_types=1);

namespace Fpmng\Poc;

use Revolt\EventLoop\Internal\AbstractDriver;
use Revolt\EventLoop\Internal\DriverCallback;
use Revolt\EventLoop\Internal\StreamReadableCallback;
use Revolt\EventLoop\Internal\StreamWritableCallback;
use Revolt\EventLoop\Internal\TimerCallback;
use Revolt\EventLoop\UnsupportedFeatureException;

final class FpmngDriver extends AbstractDriver
{
    /** @var array<string, int> Revolt callback id => SAPI watcher id */
    private array $events = [];

    public static function isSupported(): bool
    {
        return \function_exists('fpmng_worker_loop');
    }

    public function __construct()
    {
        if (!self::isSupported()) {
            throw new \Error('FpmngDriver requires a pool.executor = worker child of pool.type = http-direct');
        }

        parent::__construct();
    }

    /**
     * Signal watchers are deliberately absent from the SAPI: the FPM master
     * owns SIGQUIT/SIGUSR2 and the worker lifecycle, and a userland watcher
     * competing for them would break graceful reload. Shutdown reaches the
     * application through the notification stream instead (FpmngServer).
     */
    public function onSignal(int $signal, \Closure $closure): string
    {
        throw new UnsupportedFeatureException(
            'Signal handling is owned by the FPM master under pool.executor = worker'
        );
    }

    public function cancel(string $callbackId): void
    {
        parent::cancel($callbackId);

        if (isset($this->events[$callbackId])) {
            \fpmng_worker_event_free($this->events[$callbackId]);
            unset($this->events[$callbackId]);
        }
    }

    public function stop(): void
    {
        \fpmng_worker_loop_break();
        parent::stop();
    }

    public function getHandle(): mixed
    {
        // The event base belongs to the worker, not to PHP; there is no handle
        // userland could legitimately do anything with.
        return null;
    }

    protected function now(): float
    {
        return (float) \hrtime(true) / 1_000_000_000;
    }

    protected function dispatch(bool $blocking): void
    {
        // The only place the base is ever driven. A watcher callback must not
        // reach here again: libevent refuses a reentrant event_base_loop().
        \fpmng_worker_loop($blocking);
    }

    protected function activate(array $callbacks): void
    {
        $now = $this->now();

        foreach ($callbacks as $callback) {
            $id = $callback->id;

            if (!isset($this->events[$id])) {
                $fire = fn() => $this->enqueueCallback($callback);

                $this->events[$id] = match (true) {
                    $callback instanceof StreamReadableCallback => \fpmng_worker_event_create(
                        \FPMNG_WORKER_READ,
                        $callback->stream,
                        $fire
                    ),
                    $callback instanceof StreamWritableCallback => \fpmng_worker_event_create(
                        \FPMNG_WORKER_WRITE,
                        $callback->stream,
                        $fire
                    ),
                    $callback instanceof TimerCallback => \fpmng_worker_event_create(
                        \FPMNG_WORKER_TIMER,
                        null,
                        $fire
                    ),
                    default => throw new \Error('Unsupported callback type: ' . \get_class($callback)),
                };
            }

            if ($callback instanceof TimerCallback) {
                // Timers are one-shot in the SAPI; AbstractDriver re-activates
                // a repeating callback after every firing, exactly as it does
                // for the ext-event driver.
                \fpmng_worker_event_enable($this->events[$id], \max(0, $callback->expiration - $now));
            } else {
                \fpmng_worker_event_enable($this->events[$id]);
            }
        }
    }

    protected function deactivate(DriverCallback $callback): void
    {
        if (isset($this->events[$callback->id])) {
            \fpmng_worker_event_disable($this->events[$callback->id]);
        }
    }
}
