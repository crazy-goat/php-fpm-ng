<?php

declare(strict_types=1);

$baseUrl = rtrim(getenv('SLIM_BASE_URL') ?: 'http://127.0.0.1:22626', '/');
$containerMode = getenv('SLIM_CONTAINER') ?: 'none';
$routeCacheEnabled = getenv('SLIM_ROUTE_CACHE') === '1';
$routeCacheFile = getenv('SLIM_ROUTE_CACHE_FILE') ?: '';
$parallel = 8;
$results = [];

function requestBatch(array $specs): array
{
    $multi = curl_multi_init();
    $handles = [];

    foreach ($specs as $key => $spec) {
        $handle = curl_init($spec['url']);
        $options = [
            CURLOPT_RETURNTRANSFER => true,
            CURLOPT_CONNECTTIMEOUT_MS => 2000,
            CURLOPT_TIMEOUT_MS => 15000,
            CURLOPT_FOLLOWLOCATION => false,
            CURLOPT_HEADER => false,
        ];
        if (($spec['method'] ?? 'GET') === 'POST') {
            $options[CURLOPT_POST] = true;
            $options[CURLOPT_POSTFIELDS] = $spec['body'] ?? '';
            $options[CURLOPT_HTTPHEADER] = ['Content-Type: application/json'];
        }
        if (isset($spec['cookie'])) {
            $options[CURLOPT_COOKIEFILE] = $spec['cookie'];
            $options[CURLOPT_COOKIEJAR] = $spec['cookie'];
        }
        curl_setopt_array($handle, $options);
        curl_multi_add_handle($multi, $handle);
        $handles[$key] = $handle;
    }

    do {
        do {
            $status = curl_multi_exec($multi, $running);
        } while ($status === CURLM_CALL_MULTI_PERFORM);

        if ($running > 0 && $status === CURLM_OK) {
            if (curl_multi_select($multi, 1.0) === -1) {
                usleep(10000);
            }
        }
    } while ($running > 0 && $status === CURLM_OK);

    $responses = [];
    foreach ($handles as $key => $handle) {
        $body = curl_multi_getcontent($handle);
        $responses[$key] = [
            'status' => (int) curl_getinfo($handle, CURLINFO_RESPONSE_CODE),
            'body' => is_string($body) ? $body : '',
            'error' => curl_error($handle),
        ];
        curl_multi_remove_handle($multi, $handle);
    }
    curl_multi_close($multi);

    return $responses;
}

function getJson(array $response, int $expectedStatus = 200): array
{
    if ($response['error'] !== '') {
        throw new RuntimeException("curl: {$response['error']}");
    }
    if ($response['status'] !== $expectedStatus) {
        throw new RuntimeException("HTTP {$response['status']} (expected $expectedStatus): {$response['body']}");
    }

    try {
        $json = json_decode($response['body'], true, 512, JSON_THROW_ON_ERROR);
    } catch (Throwable $exception) {
        throw new RuntimeException("invalid JSON: {$exception->getMessage()}; body={$response['body']}");
    }
    if (!is_array($json)) {
        throw new RuntimeException('JSON response is not an object');
    }

    return $json;
}

function checkCondition(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function checkUnique(array $rows, string $field): void
{
    $values = array_map(static fn (array $row): mixed => $row[$field] ?? null, $rows);
    checkCondition(!in_array(null, $values, true), "$field contains null");
    checkCondition(count($values) === count(array_unique($values)), "$field is shared: " . json_encode($values));
}

function checkContainerIdentity(array $rows): void
{
    $modes = array_map(static fn (array $row): mixed => $row['container_mode'] ?? null, $rows);
    checkCondition(count(array_unique($modes)) === 1, 'container mode changed between requests');
    $mode = array_values($modes)[0] ?? null;
    checkCondition($mode === 'none' || $mode === 'psr-container', "unexpected container mode: $mode");

    if ($mode === 'none') {
        foreach ($rows as $row) {
            checkCondition(($row['container_oid'] ?? null) === null, 'container_oid was set without a container');
            checkCondition(($row['container_service_oid'] ?? null) === null, 'container service was set without a container');
        }

        return;
    }

    checkUnique($rows, 'container_oid');
    checkUnique($rows, 'container_service_oid');
}

function cacheFingerprint(string $file): array
{
    clearstatcache(true, $file);
    $stat = stat($file);
    checkCondition(is_array($stat), "route cache is missing: $file");
    $contents = file_get_contents($file);
    checkCondition(is_string($contents) && str_starts_with($contents, '<?php return '), 'route cache is not a FastRoute PHP cache file');
    $dispatchData = require $file;
    checkCondition(is_array($dispatchData), 'route cache did not return dispatch data');

    return [
        'device' => $stat['dev'],
        'inode' => $stat['ino'],
        'size' => $stat['size'],
        'mtime' => $stat['mtime'],
        'ctime' => $stat['ctime'],
        'hash' => hash('sha256', $contents),
    ];
}

function recordResult(string $name, callable $test): void
{
    global $results;

    try {
        $detail = $test();
        $results[$name] = 'PASS';
        echo "[PASS] $name" . ($detail === '' ? '' : " — $detail") . "\n";
    } catch (Throwable $exception) {
        $results[$name] = 'ERROR';
        echo "[ERROR] $name — {$exception->getMessage()}\n";
    }
}

function cookieFile(): string
{
    $file = tempnam(sys_get_temp_dir(), 'slim4-cookie-');
    if ($file === false) {
        throw new RuntimeException('could not create a cookie file');
    }

    return $file;
}

function cleanupCookies(array $files): void
{
    foreach ($files as $file) {
        @unlink($file);
    }
}

$parallelSpecs = static function (callable $urlFor, string $method = 'GET', ?callable $bodyFor = null) use ($baseUrl, $parallel): array {
    $specs = [];
    for ($i = 1; $i <= $parallel; $i++) {
        $spec = [
            'url' => $baseUrl . $urlFor($i),
            'method' => $method,
        ];
        if ($bodyFor !== null) {
            $spec['body'] = $bodyFor($i);
        }
        $specs[$i] = $spec;
    }

    return $specs;
};

recordResult('entry-script-shared-includes', static function () use ($baseUrl): string {
    $specs = [];
    for ($i = 0; $i < 8; $i++) {
        $specs[$i] = ['url' => "$baseUrl/health?round=$i"];
    }
    $responses = requestBatch($specs);
    foreach ($responses as $response) {
        $json = getJson($response);
        checkCondition(($json['ok'] ?? false) === true, 'health endpoint returned ok=false');
        checkCondition(($json['framework'] ?? '') === 'slim4', 'unexpected framework marker');
    }

    return 'stock public/index.php survived repeated requests';
});

recordResult('mix-mysql-and-redis', static function () use ($parallelSpecs): string {
    $responses = requestBatch($parallelSpecs(
        static fn (int $id): string => "/mix?id=$id&sleep=1",
    ));
    $rows = [];
    foreach ($responses as $id => $response) {
        $row = getJson($response);
        checkCondition(($row['ok'] ?? false) === true, "request $id returned ok=false");
        checkCondition((int) ($row['id'] ?? 0) === $id, "request $id returned the wrong id");
        $rows[] = $row;
    }
    checkUnique($rows, 'request_oid');
    checkUnique($rows, 'app_oid');

    return '8/8 own MySQL row and Redis value';
});

recordResult('session-rounds', static function () use ($baseUrl, $parallel): string {
    $cookies = [];
    $firstSpecs = [];
    for ($i = 1; $i <= $parallel; $i++) {
        $cookies[$i] = cookieFile();
        $firstSpecs[$i] = [
            'url' => "$baseUrl/session?user=user-$i&sleep=1",
            'cookie' => $cookies[$i],
        ];
    }

    try {
        $first = requestBatch($firstSpecs);
        $firstRows = [];
        foreach ($first as $i => $response) {
            $row = getJson($response);
            checkCondition(($row['ok'] ?? false) === true, "first round request $i returned ok=false");
            checkCondition(($row['session_user'] ?? null) === "user-$i", "first round request $i saw another user");
            $firstRows[$i] = $row;
        }
        checkUnique(array_values($firstRows), 'sid');

        $secondSpecs = [];
        for ($i = 1; $i <= $parallel; $i++) {
            $secondSpecs[$i] = [
                'url' => "$baseUrl/session?user=user-$i&sleep=1",
                'cookie' => $cookies[$i],
            ];
        }
        $second = requestBatch($secondSpecs);
        foreach ($second as $i => $response) {
            $row = getJson($response);
            checkCondition(($row['ok'] ?? false) === true, "second round request $i returned ok=false");
            checkCondition(($row['session_user'] ?? null) === "user-$i", "second round request $i saw another user");
            checkCondition((int) ($row['count'] ?? 0) === 2, "second round request $i did not increment its counter");
            checkCondition(($row['sid'] ?? null) === ($firstRows[$i]['sid'] ?? null), "session id changed for user $i");
        }
    } finally {
        cleanupCookies($cookies);
    }

    return '8/8 own session id and data; second round count=2';
});

recordResult('authenticated-route', static function () use ($baseUrl, $parallel): string {
    $cookies = [];
    $loginSpecs = [];
    for ($i = 1; $i <= $parallel; $i++) {
        $user = $i <= 4 ? 'alice' : 'bob';
        $cookies[$i] = cookieFile();
        $loginSpecs[$i] = [
            'url' => "$baseUrl/login?user=$user",
            'cookie' => $cookies[$i],
        ];
    }

    try {
        $loginResponses = requestBatch($loginSpecs);
        foreach ($loginResponses as $i => $response) {
            $row = getJson($response);
            $expected = $i <= 4 ? 'alice' : 'bob';
            checkCondition(($row['logged_in'] ?? null) === $expected, "login $i returned the wrong identity");
        }

        $meSpecs = [];
        for ($i = 1; $i <= $parallel; $i++) {
            $meSpecs[$i] = [
                'url' => "$baseUrl/me?sleep=1",
                'cookie' => $cookies[$i],
            ];
        }
        $meResponses = requestBatch($meSpecs);
        foreach ($meResponses as $i => $response) {
            $row = getJson($response);
            $expected = $i <= 4 ? 'alice' : 'bob';
            checkCondition(($row['user'] ?? null) === $expected, "request $i saw the wrong authenticated user");
        }
    } finally {
        cleanupCookies($cookies);
    }

    return '8/8 own authenticated identity';
});

recordResult('object-identity', static function () use ($parallelSpecs): string {
    $responses = requestBatch($parallelSpecs(
        static fn (int $id): string => "/identity?sleep=1&id=$id",
    ));
    $rows = array_map(static fn (array $response): array => getJson($response), $responses);
    checkUnique($rows, 'app_oid');
    checkUnique($rows, 'request_oid');
    checkUnique($rows, 'route_collector_oid');
    checkContainerIdentity($rows);

    return 'app, request, route collector and container identity are isolated per concurrent request';
});

recordResult('psr7-request-body', static function () use ($parallelSpecs): string {
    $payloads = [];
    $responses = requestBatch($parallelSpecs(
        static fn (int $id): string => "/body?sleep=1&id=$id",
        'POST',
        static function (int $id) use (&$payloads): string {
            $payloads[$id] = json_encode([
                'marker' => "body-$id",
                'blob' => str_repeat(chr(64 + $id), 65536),
            ], JSON_THROW_ON_ERROR);

            return $payloads[$id];
        },
    ));
    foreach ($responses as $id => $response) {
        $row = getJson($response);
        checkCondition(($row['marker'] ?? null) === "body-$id", "request $id saw another POST body");
        checkCondition(($row['body_complete'] ?? false) === true, "request $id did not read its full body");
        checkCondition(($row['sha256'] ?? null) === hash('sha256', $payloads[$id]), "request $id body hash differs");
    }

    return '8/8 request bodies stayed separate; 64 KiB bodies were complete';
});

recordResult('psr7-response-body', static function () use ($baseUrl, $parallel): string {
    $specs = [];
    for ($i = 1; $i <= $parallel; $i++) {
        $specs[$i] = ['url' => "$baseUrl/response-stream?id=$i&sleep=1"];
    }
    $responses = requestBatch($specs);
    foreach ($responses as $id => $response) {
        if ($response['error'] !== '') {
            throw new RuntimeException("curl: {$response['error']}");
        }
        checkCondition($response['status'] === 200, "response $id returned HTTP {$response['status']}");
        checkCondition($response['body'] === "start-$id\nend-$id\n", "response body for $id was interleaved");
    }

    return '8/8 response streams stayed separate';
});

recordResult('middleware-state', static function () use ($parallelSpecs): string {
    $responses = requestBatch($parallelSpecs(
        static fn (int $id): string => "/middleware?sleep=1&id=$id",
    ));
    $rows = [];
    foreach ($responses as $id => $response) {
        $row = getJson($response);
        checkCondition(($row['middleware_hits'] ?? 0) === 1, "middleware hit count for $id was not one");
        checkCondition(is_string($row['middleware_id'] ?? null), "middleware id missing for $id");
        $rows[] = $row;
    }
    checkUnique($rows, 'middleware_id');

    return 'middleware state did not carry between requests';
});

recordResult('error-middleware', static function () use ($baseUrl, $parallel): string {
    $tags = [];
    $specs = [];
    for ($i = 1; $i <= $parallel; $i++) {
        $tags[$i] = "error-$i";
        $specs[$i] = ['url' => "$baseUrl/error?tag={$tags[$i]}&sleep=1"];
    }
    $responses = requestBatch($specs);
    foreach ($responses as $id => $response) {
        if ($response['error'] !== '') {
            throw new RuntimeException("curl: {$response['error']}");
        }
        checkCondition($response['status'] === 500, "error request $id returned HTTP {$response['status']}");
        checkCondition(str_contains($response['body'], $tags[$id]), "error response $id lost its own exception");
        foreach ($tags as $otherId => $otherTag) {
            if ($otherId !== $id) {
                checkCondition(!str_contains($response['body'], $otherTag), "error response $id contained $otherTag");
            }
        }
    }

    return '8/8 error responses kept their own exception';
});

if ($containerMode === 'php-di') {
    recordResult('container-php-di', static function () use ($parallelSpecs): string {
        $responses = requestBatch($parallelSpecs(
            static fn (int $id): string => "/identity?sleep=1&id=$id",
        ));
        $rows = array_map(static fn (array $response): array => getJson($response), $responses);
        foreach ($rows as $row) {
            checkCondition(($row['container_mode'] ?? null) === 'psr-container', 'PHP-DI container was not attached to Slim');
            checkCondition(is_int($row['container_service_oid'] ?? null), 'PHP-DI service identity is missing');
        }
        checkContainerIdentity($rows);

        return '8/8 concurrent requests used distinct PHP-DI containers and services';
    });
} else {
    $results['container-php-di'] = 'NOT MEASURED';
    echo "[NOT MEASURED] container-php-di — run with SLIM_CONTAINER=php-di to measure PHP-DI\n";
}

if ($routeCacheEnabled) {
    recordResult('route-cache', static function () use ($baseUrl, $parallel, $parallelSpecs, $routeCacheFile): string {
        checkCondition($routeCacheFile !== '', 'route cache file path is missing');
        $before = cacheFingerprint($routeCacheFile);

        $identityResponses = requestBatch($parallelSpecs(
            static fn (int $id): string => "/identity?sleep=1&id=$id",
        ));
        $identityRows = [];
        foreach ($identityResponses as $id => $response) {
            $row = getJson($response);
            checkCondition(($row['uri'] ?? null) === "/identity?sleep=1&id=$id", "identity route $id was not selected");
            $identityRows[] = $row;
        }
        checkUnique($identityRows, 'request_oid');
        checkUnique($identityRows, 'route_collector_oid');

        $mixSpecs = [];
        for ($id = 1; $id <= $parallel; $id++) {
            $mixSpecs[$id] = ['url' => "$baseUrl/mix?id=$id&sleep=1"];
        }
        foreach (requestBatch($mixSpecs) as $id => $response) {
            $row = getJson($response);
            checkCondition(($row['ok'] ?? false) === true, "cached route $id returned incorrect data");
            checkCondition((int) ($row['id'] ?? 0) === $id, "cached route $id returned the wrong id");
        }

        $after = cacheFingerprint($routeCacheFile);
        checkCondition($before === $after, 'route cache changed during concurrent requests');

        return 'route data stayed correct and the cache fingerprint was unchanged';
    });
} else {
    $results['route-cache'] = 'NOT MEASURED';
    echo "[NOT MEASURED] route-cache — run with SLIM_ROUTE_CACHE=1 to measure Slim route caching\n";
}

$counts = array_count_values($results);
echo sprintf(
    "SUMMARY pass=%d error=%d not_measured=%d\n",
    $counts['PASS'] ?? 0,
    $counts['ERROR'] ?? 0,
    $counts['NOT MEASURED'] ?? 0,
);

exit(($counts['ERROR'] ?? 0) === 0 ? 0 : 1);
