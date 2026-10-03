<?php

declare(strict_types=1);

use DI\ContainerBuilder;
use Psr\Container\ContainerInterface;
use Psr\Http\Message\ResponseInterface;
use Psr\Http\Message\ServerRequestInterface;
use Psr\Http\Server\RequestHandlerInterface;
use Slim\Factory\AppFactory;
use Slim\Psr7\Response;

require __DIR__ . '/../vendor/autoload.php';

$containerMode = getenv('SLIM_CONTAINER') ?: 'none';
$container = null;
$containerService = null;
if ($containerMode === 'php-di') {
    $containerBuilder = new ContainerBuilder();
    $containerBuilder->addDefinitions([
        'slim4.probe.identity' => static fn (): object => new stdClass(),
    ]);
    $container = $containerBuilder->build();
    $containerService = $container->get('slim4.probe.identity');
    $app = AppFactory::createFromContainer($container);
} elseif ($containerMode === 'none') {
    $app = AppFactory::create();
} else {
    throw new RuntimeException("unsupported Slim container mode: $containerMode");
}

if (getenv('SLIM_ROUTE_CACHE') === '1') {
    $routeCacheFile = getenv('SLIM_ROUTE_CACHE_FILE');
    if ($routeCacheFile === false || $routeCacheFile === '') {
        throw new RuntimeException('SLIM_ROUTE_CACHE_FILE is required when route cache is enabled');
    }
    $app->getRouteCollector()->setCacheFile($routeCacheFile);
}

$app->addRoutingMiddleware();
$app->addErrorMiddleware(true, true, true);

$middlewareId = bin2hex(random_bytes(4));

$app->add(function (
    ServerRequestInterface $request,
    RequestHandlerInterface $handler
) use ($middlewareId): ResponseInterface {
    $request = $request
        ->withAttribute('slim_probe_middleware_id', $middlewareId)
        ->withAttribute('slim_probe_middleware_hits', 1);

    return $handler->handle($request);
});

$app->add(function (
    ServerRequestInterface $request,
    RequestHandlerInterface $handler
): ResponseInterface {
    if (session_status() !== PHP_SESSION_ACTIVE) {
        session_name('slim4_probe');
        session_set_cookie_params([
            'path' => '/',
            'httponly' => true,
            'samesite' => 'Lax',
        ]);
        session_start();
    }

    try {
        return $handler->handle($request->withAttribute('slim_probe_session_user', $_SESSION['user'] ?? null));
    } finally {
        if (session_status() === PHP_SESSION_ACTIVE) {
            session_write_close();
        }
    }
});

$json = static function (
    ResponseInterface $response,
    array $data,
    int $status = 200
): ResponseInterface {
    $response = $response
        ->withStatus($status)
        ->withHeader('Content-Type', 'application/json');
    $response->getBody()->write(json_encode($data, JSON_THROW_ON_ERROR));

    return $response;
};

$queryValue = static function (ServerRequestInterface $request, string $name, mixed $default = null): mixed {
    $query = $request->getQueryParams();

    return $query[$name] ?? $default;
};

$common = static function (ServerRequestInterface $request) use ($app, $containerService): array {
    $container = $app->getContainer();
    // A global survives only if the worker keeps PHP state between requests, which
    // `classic` must not: every response has to report 1.
    $GLOBALS['slim4_probe_hits'] = ($GLOBALS['slim4_probe_hits'] ?? 0) + 1;

    return [
        'pid' => getmypid(),
        'uri' => $request->getUri()->getPath() . ($request->getUri()->getQuery() === '' ? '' : '?' . $request->getUri()->getQuery()),
        'app_oid' => spl_object_id($app),
        'request_oid' => spl_object_id($request),
        'route_collector_oid' => spl_object_id($app->getRouteCollector()),
        'container_oid' => $container instanceof ContainerInterface ? spl_object_id($container) : null,
        'container_mode' => $container instanceof ContainerInterface ? 'psr-container' : 'none',
        'container_service_oid' => is_object($containerService) ? spl_object_id($containerService) : null,
        'middleware_id' => $request->getAttribute('slim_probe_middleware_id'),
        'middleware_hits' => $request->getAttribute('slim_probe_middleware_hits'),
        'global_hits' => $GLOBALS['slim4_probe_hits'],
        'session_user' => $_SESSION['user'] ?? null,
        'included' => count(get_included_files()),
        'ob_level' => ob_get_level(),
        'memory_mb' => round(memory_get_usage(true) / 1048576, 1),
    ];
};

$app->get('/health', function (
    ServerRequestInterface $request,
    ResponseInterface $response
) use ($json, $common): ResponseInterface {
    return $json($response, [
        'ok' => true,
        'framework' => 'slim4',
    ] + $common($request));
});

$app->get('/identity', function (
    ServerRequestInterface $request,
    ResponseInterface $response
) use ($json, $common): ResponseInterface {
    return $json($response, $common($request));
});

$app->get('/session', function (
    ServerRequestInterface $request,
    ResponseInterface $response
) use ($json, $common, $queryValue): ResponseInterface {
    $user = (string) $queryValue($request, 'user', 'anonymous');

    if (!array_key_exists('user', $_SESSION)) {
        $_SESSION['user'] = $user;
    }
    $_SESSION['count'] = ((int) ($_SESSION['count'] ?? 0)) + 1;
    $sessionUser = $_SESSION['user'];
    $count = $_SESSION['count'];
    $sessionId = session_id();

    return $json($response, [
        'user_param' => $user,
        'session_user' => $sessionUser,
        'count' => $count,
        'sid' => $sessionId,
        'ok' => $sessionUser === $user,
    ] + $common($request));
});

$app->get('/login', function (
    ServerRequestInterface $request,
    ResponseInterface $response
) use ($json, $common, $queryValue): ResponseInterface {
    $user = (string) $queryValue($request, 'user', 'anonymous');
    $_SESSION['user'] = $user;

    return $json($response, [
        'logged_in' => $user,
        'sid' => session_id(),
    ] + $common($request));
});

$app->get('/me', function (
    ServerRequestInterface $request,
    ResponseInterface $response
) use ($json, $common): ResponseInterface {
    return $json($response, [
        'user' => $_SESSION['user'] ?? null,
    ] + $common($request));
});

$app->post('/body', function (
    ServerRequestInterface $request,
    ResponseInterface $response
) use ($json, $common): ResponseInterface {
    $body = $request->getBody();
    if ($body->isSeekable()) {
        $body->rewind();
    }
    $contents = $body->getContents();
    $payload = json_decode($contents, true, 512, JSON_THROW_ON_ERROR);

    return $json($response, [
        'marker' => $payload['marker'] ?? null,
        'length' => strlen($contents),
        'sha256' => hash('sha256', $contents),
        'body_complete' => isset($payload['blob']) && strlen((string) $payload['blob']) >= 65536,
    ] + $common($request));
});

$app->get('/response-stream', function (
    ServerRequestInterface $request,
    ResponseInterface $response
) use ($queryValue): ResponseInterface {
    $id = (int) $queryValue($request, 'id', 0);
    $response->getBody()->write("start-$id\n");
    $response->getBody()->write("end-$id\n");

    return $response->withHeader('Content-Type', 'text/plain');
});

$app->get('/error', function (
    ServerRequestInterface $request
) use ($queryValue): never {
    $tag = (string) $queryValue($request, 'tag', 'unknown');

    throw new RuntimeException("slim4 probe error: $tag");
});

$app->run();
