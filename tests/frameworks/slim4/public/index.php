<?php

declare(strict_types=1);

use Predis\Client as RedisClient;
use Psr\Container\ContainerInterface;
use Psr\Http\Message\ResponseInterface;
use Psr\Http\Message\ServerRequestInterface;
use Psr\Http\Server\RequestHandlerInterface;
use Slim\Factory\AppFactory;
use Slim\Psr7\Response;

require __DIR__ . '/../vendor/autoload.php';

$app = AppFactory::create();
$app->addRoutingMiddleware();
$app->addErrorMiddleware(true, true, true);

$middlewareId = bin2hex(random_bytes(4));

$app->add(static function (
    ServerRequestInterface $request,
    RequestHandlerInterface $handler
) use ($middlewareId): ResponseInterface {
    $request = $request
        ->withAttribute('slim_probe_middleware_id', $middlewareId)
        ->withAttribute('slim_probe_middleware_hits', 1);

    return $handler->handle($request);
});

$app->add(static function (
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

$newRedis = static function (): RedisClient {
    $database = getenv('SLIM_REDIS_DB');

    return new RedisClient([
        'scheme' => 'tcp',
        'host' => getenv('SLIM_REDIS_HOST') ?: '127.0.0.1',
        'port' => (int) (getenv('SLIM_REDIS_PORT') ?: 6379),
        'database' => $database === false ? 2 : (int) $database,
        'timeout' => 5,
        'read_write_timeout' => 5,
    ]);
};

$suspend = static function (float $seconds) use ($newRedis): void {
    if ($seconds <= 0) {
        return;
    }

    $redis = $newRedis();
    $key = 'slim4:wait:' . bin2hex(random_bytes(8));
    $redis->executeRaw(['BRPOP', $key, (string) max(1, (int) ceil($seconds))]);
    $redis->disconnect();
};

$newPdo = static function (): PDO {
    $host = getenv('SLIM_DB_HOST') ?: '127.0.0.1';
    $port = (int) (getenv('SLIM_DB_PORT') ?: 3306);
    $database = getenv('SLIM_DB_NAME') ?: 'slim4';
    $user = getenv('SLIM_DB_USER') ?: 'bench';
    $password = getenv('SLIM_DB_PASSWORD') ?: 'bench';

    return new PDO(
        "mysql:host=$host;port=$port;dbname=$database;charset=utf8mb4",
        $user,
        $password,
        [
            PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
            PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
            PDO::ATTR_EMULATE_PREPARES => false,
        ]
    );
};

$common = static function (ServerRequestInterface $request) use ($app): array {
    $container = $app->getContainer();

    return [
        'pid' => getmypid(),
        'uri' => $request->getUri()->getPath() . ($request->getUri()->getQuery() === '' ? '' : '?' . $request->getUri()->getQuery()),
        'app_oid' => spl_object_id($app),
        'request_oid' => spl_object_id($request),
        'route_collector_oid' => spl_object_id($app->getRouteCollector()),
        'container_oid' => $container instanceof ContainerInterface ? spl_object_id($container) : null,
        'container_mode' => $container instanceof ContainerInterface ? 'psr-container' : 'none',
        'middleware_id' => $request->getAttribute('slim_probe_middleware_id'),
        'middleware_hits' => $request->getAttribute('slim_probe_middleware_hits'),
        'session_user' => $_SESSION['user'] ?? null,
        'included' => count(get_included_files()),
        'ob_level' => ob_get_level(),
        'memory_mb' => round(memory_get_usage(true) / 1048576, 1),
    ];
};

$app->get('/health', static function (
    ServerRequestInterface $request,
    ResponseInterface $response
) use ($json, $common): ResponseInterface {
    return $json($response, [
        'ok' => true,
        'framework' => 'slim4',
    ] + $common($request));
});

$app->get('/identity', static function (
    ServerRequestInterface $request,
    ResponseInterface $response
) use ($json, $common, $queryValue, $suspend): ResponseInterface {
    $suspend((float) $queryValue($request, 'sleep', 0));

    return $json($response, $common($request));
});

$app->get('/mix', static function (
    ServerRequestInterface $request,
    ResponseInterface $response
) use ($json, $common, $queryValue, $newPdo, $newRedis): ResponseInterface {
    $id = max(1, min(8, (int) $queryValue($request, 'id', 1)));
    $sleep = max(0, min(2, (float) $queryValue($request, 'sleep', 0)));
    $token = bin2hex(random_bytes(4));
    $tag = "t-$id-$token";
    $value = "v-$id-$token";
    $key = "slim4:probe:$id:$token";
    $redis = $newRedis();
    $pdo = $newPdo();

    $redis->executeRaw(['SETEX', $key, '60', $value]);

    $statement = $pdo->prepare(
        'SELECT SLEEP(:delay) AS waited, CONNECTION_ID() AS connection_id, :tag AS tag'
    );
    $statement->execute(['delay' => $sleep, 'tag' => $tag]);
    $databaseRow = $statement->fetch();

    $statement = $pdo->prepare('SELECT label FROM slim4_probe_items WHERE id = :id');
    $statement->execute(['id' => $id]);
    $item = $statement->fetch();
    $redisValue = $redis->get($key);
    $redis->disconnect();

    return $json($response, [
        'id' => $id,
        'token' => $token,
        'item' => $item['label'] ?? null,
        'db_tag' => $databaseRow['tag'] ?? null,
        'db_connection_id' => (int) ($databaseRow['connection_id'] ?? 0),
        'redis' => $redisValue,
        'ok' => ($item['label'] ?? null) === "item-$id"
            && ($databaseRow['tag'] ?? null) === $tag
            && $redisValue === $value,
    ] + $common($request));
});

$app->get('/session', static function (
    ServerRequestInterface $request,
    ResponseInterface $response
) use ($json, $common, $queryValue, $suspend): ResponseInterface {
    $user = (string) $queryValue($request, 'user', 'anonymous');
    $sleep = max(0, min(2, (float) $queryValue($request, 'sleep', 0)));

    if (!array_key_exists('user', $_SESSION)) {
        $_SESSION['user'] = $user;
    }
    $_SESSION['count'] = ((int) ($_SESSION['count'] ?? 0)) + 1;
    $sessionUser = $_SESSION['user'];
    $count = $_SESSION['count'];
    $sessionId = session_id();
    $suspend($sleep);

    return $json($response, [
        'user_param' => $user,
        'session_user' => $sessionUser,
        'count' => $count,
        'sid' => $sessionId,
        'ok' => $sessionUser === $user,
    ] + $common($request));
});

$app->get('/login', static function (
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

$app->get('/me', static function (
    ServerRequestInterface $request,
    ResponseInterface $response
) use ($json, $common, $queryValue, $suspend): ResponseInterface {
    $suspend((float) $queryValue($request, 'sleep', 0));

    return $json($response, [
        'user' => $_SESSION['user'] ?? null,
    ] + $common($request));
});

$app->post('/body', static function (
    ServerRequestInterface $request,
    ResponseInterface $response
) use ($json, $common, $suspend, $queryValue): ResponseInterface {
    $body = $request->getBody();
    if ($body->isSeekable()) {
        $body->rewind();
    }
    $contents = $body->getContents();
    $payload = json_decode($contents, true, 512, JSON_THROW_ON_ERROR);
    $suspend((float) $queryValue($request, 'sleep', 0));

    return $json($response, [
        'marker' => $payload['marker'] ?? null,
        'length' => strlen($contents),
        'sha256' => hash('sha256', $contents),
        'body_complete' => isset($payload['blob']) && strlen((string) $payload['blob']) >= 65536,
    ] + $common($request));
});

$app->get('/response-stream', static function (
    ServerRequestInterface $request,
    ResponseInterface $response
) use ($queryValue, $suspend): ResponseInterface {
    $id = (int) $queryValue($request, 'id', 0);
    $sleep = (float) $queryValue($request, 'sleep', 0);
    $response->getBody()->write("start-$id\n");
    $suspend($sleep);
    $response->getBody()->write("end-$id\n");

    return $response->withHeader('Content-Type', 'text/plain');
});

$app->get('/middleware', static function (
    ServerRequestInterface $request,
    ResponseInterface $response
) use ($json, $common, $queryValue, $suspend): ResponseInterface {
    $suspend((float) $queryValue($request, 'sleep', 0));

    return $json($response, $common($request));
});

$app->get('/error', static function (
    ServerRequestInterface $request
) use ($queryValue, $suspend): never {
    $tag = (string) $queryValue($request, 'tag', 'unknown');
    $suspend((float) $queryValue($request, 'sleep', 0));

    throw new RuntimeException("slim4 probe error: $tag");
});

$app->run();
