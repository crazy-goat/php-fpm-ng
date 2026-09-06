<?php

namespace App\Http\Middleware;

use Closure;
use Illuminate\Http\Request;
use Illuminate\Support\Facades\Redis;
use Symfony\Component\HttpFoundation\Response;

final class ProbeLifecycleMiddleware
{
    public function handle(Request $request, Closure $next): Response
    {
        $marker = (string) $request->query('middleware_marker', '');
        $request->attributes->set('probe.middleware_marker', $marker);
        $request->attributes->set('probe.middleware_oid', spl_object_id($this));

        $response = $next($request);

        if ($marker !== '') {
            $response->headers->set('X-Probe-Middleware-Marker', $marker);
            $response->headers->set('X-Probe-Middleware-Oid', (string) spl_object_id($this));
        }

        return $response;
    }

    public function terminate(Request $request, Response $response): void
    {
        $marker = (string) $request->attributes->get('probe.middleware_marker', '');
        if ($marker === '') {
            return;
        }

        Redis::setex(
            'laravel025:terminate:'.$marker,
            60,
            json_encode([
                'marker' => $marker,
                'middleware_oid' => $request->attributes->get('probe.middleware_oid'),
                'request_oid' => spl_object_id($request),
                'response_marker' => $response->headers->get('X-Probe-Middleware-Marker'),
            ], JSON_THROW_ON_ERROR),
        );
    }
}
