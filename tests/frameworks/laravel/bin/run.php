<?php

declare(strict_types=1);

$baseUrl = rtrim(getenv('LARAVEL_BASE_URL') ?: 'http://127.0.0.1:22726', '/');
$mode = getenv('LARAVEL_TEST_MODE') ?: 'configured';
$only = getenv('LARAVEL_ONLY') ?: '';
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
        }
        if (!empty($spec['headers'])) {
            $options[CURLOPT_HTTPHEADER] = $spec['headers'];
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
            'headers' => (string) curl_getinfo($handle, CURLINFO_HEADER_OUT),
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
    checkCondition(count($values) === count(array_unique($values)), "$field is shared: ".json_encode($values));
}

function checkOwnIdentity(array $rows): void
{
    foreach (['app_oid', 'request_oid', 'container_request_oid', 'container_instance_oid', 'facade_app_oid'] as $field) {
        checkUnique($rows, $field);
    }
    checkCondition(count(array_unique(array_map(static fn (array $row): mixed => $row['same_request'] ?? null, $rows))) === 1, 'request identity flag changed');
    foreach ($rows as $row) {
        checkCondition(($row['same_request'] ?? false) === true, 'route request was not the container request');
    }
}

function cookieFile(): string
{
    $file = tempnam(sys_get_temp_dir(), 'laravel025-cookie-');
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

function recordResult(string $name, callable $test): void
{
    global $results;

    try {
        $detail = $test();
        $results[$name] = 'PASS';
        echo '[PASS] '.$name.($detail === '' ? '' : " — $detail")."\n";
    } catch (Throwable $exception) {
        $results[$name] = 'ERROR';
        echo "[ERROR] $name — {$exception->getMessage()}\n";
    }
}

$parallelSpecs = static function (callable $urlFor, string $method = 'GET', ?callable $bodyFor = null, ?callable $cookieFor = null, ?callable $headersFor = null) use ($baseUrl, $parallel): array {
    $specs = [];
    for ($i = 1; $i <= $parallel; $i++) {
        $spec = [
            'url' => $baseUrl.$urlFor($i),
            'method' => $method,
        ];
        if ($bodyFor !== null) {
            $spec['body'] = $bodyFor($i);
        }
        if ($cookieFor !== null) {
            $spec['cookie'] = $cookieFor($i);
        }
        if ($headersFor !== null) {
            $spec['headers'] = $headersFor($i);
        }
        $specs[$i] = $spec;
    }

    return $specs;
};

$runSession = static function () use ($baseUrl, $parallel): string {
    $cookies = [];
    $firstSpecs = [];
    for ($i = 1; $i <= $parallel; $i++) {
        $cookies[$i] = cookieFile();
        $firstSpecs[$i] = [
            'url' => "$baseUrl/session?user=user-$i&sleep=0.4",
            'cookie' => $cookies[$i],
        ];
    }

    try {
        $first = requestBatch($firstSpecs);
        $firstRows = [];
        foreach ($first as $i => $response) {
            $row = getJson($response);
            checkCondition(($row['ok'] ?? false) === true, "first session request $i returned ok=false: ".json_encode($row));
            checkCondition(($row['user_param'] ?? null) === "user-$i", "first session request $i had wrong parameter");
            checkCondition(($row['sess_user'] ?? null) === "user-$i", "first session request $i saw another session user");
            checkCondition(($row['count'] ?? 0) === 1, "first session request $i had wrong count");
            checkCondition(is_string($row['sid'] ?? null) && $row['sid'] !== '', "first session request $i has no sid");
            $firstRows[$i] = $row;
        }
        checkOwnIdentity(array_values($firstRows));
        checkUnique(array_values($firstRows), 'sid');

        $secondSpecs = [];
        for ($i = 1; $i <= $parallel; $i++) {
            $secondSpecs[$i] = [
                'url' => "$baseUrl/session?user=user-$i&sleep=0.4",
                'cookie' => $cookies[$i],
            ];
        }
        $second = requestBatch($secondSpecs);
        foreach ($second as $i => $response) {
            $row = getJson($response);
            checkCondition(($row['ok'] ?? false) === true, "second session request $i returned ok=false: ".json_encode($row));
            checkCondition(($row['sess_user'] ?? null) === "user-$i", "second session request $i saw another user");
            checkCondition(($row['count'] ?? 0) === 2, "second session request $i did not increment its own counter");
            checkCondition(($row['sid'] ?? null) === ($firstRows[$i]['sid'] ?? null), "session id changed for user $i");
        }
    } finally {
        cleanupCookies($cookies);
    }

    return '8/8 own sid, session data, object identity and second-round count=2';
};

$runAuthenticated = static function () use ($baseUrl, $parallel): string {
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
            checkCondition(($row['auth_id'] ?? null) !== null, "login $i did not establish an auth id");
        }

        $meSpecs = [];
        for ($i = 1; $i <= $parallel; $i++) {
            $meSpecs[$i] = [
                'url' => "$baseUrl/me?sleep=0.4",
                'cookie' => $cookies[$i],
            ];
        }
        $meResponses = requestBatch($meSpecs);
        $rows = [];
        foreach ($meResponses as $i => $response) {
            $row = getJson($response);
            $expected = $i <= 4 ? 'alice' : 'bob';
            checkCondition(($row['user'] ?? null) === $expected, "request $i saw the wrong authenticated user: ".json_encode($row));
            checkCondition(($row['auth_id'] ?? null) !== null, "request $i lost its auth id");
            $rows[] = $row;
        }
        checkOwnIdentity($rows);
    } finally {
        cleanupCookies($cookies);
    }

    return '8/8 own authenticated identity after Auth::login';
};

$runMix = static function () use ($parallelSpecs): string {
    $responses = requestBatch($parallelSpecs(static fn (int $id): string => "/mix?id=$id&sleep=0.4"));
    $rows = [];
    foreach ($responses as $id => $response) {
        $row = getJson($response);
        checkCondition(($row['ok'] ?? false) === true, "mix request $id returned an inconsistent payload: ".json_encode($row));
        checkCondition(($row['id'] ?? null) === $id, "mix request $id returned the wrong id");
        checkCondition(($row['item'] ?? null) === "item-$id", "mix request $id returned another Eloquent item");
        checkCondition(($row['db_tag'] ?? '') !== '', "mix request $id has no DB tag");
        checkCondition(($row['redis'] ?? '') !== '' && ($row['cache'] ?? '') !== '', "mix request $id lost a Redis or cache value");
        $rows[] = $row;
    }
    checkOwnIdentity($rows);

    return '8/8 own MySQL, Eloquent, Redis and Cache data';
};

$runIdentity = static function () use ($parallelSpecs): string {
    $responses = requestBatch($parallelSpecs(static fn (int $id): string => "/identity?sleep=0.4&id=$id"));
    $rows = array_map(static fn (array $response): array => getJson($response), $responses);
    checkOwnIdentity($rows);
    foreach (['auth_root_oid', 'cache_root_oid', 'db_root_oid', 'redis_root_oid', 'middleware_oid'] as $field) {
        checkUnique($rows, $field);
    }

    return 'application, request, container, facade roots and middleware are distinct';
};

$runEloquent = static function () use ($parallelSpecs): string {
    $responses = requestBatch($parallelSpecs(static fn (int $id): string => "/eloquent?id=$id&sleep=0.4"));
    $rows = [];
    foreach ($responses as $id => $response) {
        $row = getJson($response);
        checkCondition(($row['ok'] ?? false) === true, "Eloquent request $id lost its resolver or item: ".json_encode($row));
        checkCondition(($row['item'] ?? null) === "item-$id", "Eloquent request $id returned another item");
        checkCondition(($row['resolver_stayed_same'] ?? false) === true, "Eloquent resolver changed inside request $id");
        $rows[] = $row;
    }
    checkUnique($rows, 'resolver_after_oid');
    checkUnique($rows, 'connection_after_oid');

    return '8/8 Eloquent resolver and connection identity stayed request-local';
};

$runLifecycle = static function () use ($baseUrl, $parallel): string {
    $markers = [];
    $specs = [];
    for ($i = 1; $i <= $parallel; $i++) {
        $markers[$i] = "lifecycle-$i-".bin2hex(random_bytes(3));
        $specs[$i] = [
            'url' => "$baseUrl/lifecycle?middleware_marker={$markers[$i]}&sleep=0.4",
        ];
    }

    $responses = requestBatch($specs);
    $rows = [];
    foreach ($responses as $i => $response) {
        $row = getJson($response);
        checkCondition(($row['marker'] ?? null) === $markers[$i], "lifecycle request $i lost its marker");
        checkCondition(($row['middleware_marker_from_request'] ?? null) === $markers[$i], "middleware request $i saw another marker");
        $rows[] = $row;
    }
    checkUnique($rows, 'middleware_oid');
    checkOwnIdentity($rows);

    foreach ($markers as $i => $marker) {
        $response = requestBatch([['url' => "$baseUrl/terminate-check?marker=$marker"]])[0];
        $row = getJson($response);
        $terminate = $row['terminate'] ?? null;
        checkCondition(is_array($terminate), "terminate record for request $i is missing");
        checkCondition(($terminate['marker'] ?? null) === $marker, "terminate record for request $i has another marker");
        checkCondition(($terminate['response_marker'] ?? null) === $marker, "terminate response attribution for request $i is wrong");
    }

    return '8/8 middleware and terminate records retained their own request marker';
};

$runCsrf = static function () use ($baseUrl, $parallel): string {
    $cookies = [];
    $formSpecs = [];
    for ($i = 1; $i <= $parallel; $i++) {
        $cookies[$i] = cookieFile();
        $formSpecs[$i] = ['url' => "$baseUrl/csrf-form", 'cookie' => $cookies[$i]];
    }

    try {
        $forms = requestBatch($formSpecs);
        $tokens = [];
        foreach ($forms as $i => $response) {
            $row = getJson($response);
            checkCondition(is_string($row['token'] ?? null) && $row['token'] !== '', "CSRF token $i is missing");
            $tokens[$i] = $row['token'];
        }
        checkUnique(array_map(static fn (string $token): array => ['token' => $token], $tokens), 'token');

        $submitSpecs = [];
        for ($i = 1; $i <= $parallel; $i++) {
            $submitSpecs[$i] = [
                'url' => "$baseUrl/csrf-submit",
                'method' => 'POST',
                'body' => json_encode(['_token' => $tokens[$i], 'marker' => "csrf-$i"], JSON_THROW_ON_ERROR),
                'headers' => ['Content-Type: application/json', 'Accept: application/json'],
                'cookie' => $cookies[$i],
            ];
        }
        $submitted = requestBatch($submitSpecs);
        foreach ($submitted as $i => $response) {
            $row = getJson($response);
            checkCondition(($row['marker'] ?? null) === "csrf-$i", "CSRF request $i used another session token");
        }

        $cross = requestBatch([[
            'url' => "$baseUrl/csrf-submit",
            'method' => 'POST',
            'body' => json_encode(['_token' => $tokens[2], 'marker' => 'cross-session'], JSON_THROW_ON_ERROR),
            'headers' => ['Content-Type: application/json', 'Accept: application/json'],
            'cookie' => $cookies[1],
        ]])[0];
        checkCondition($cross['status'] === 419, 'a CSRF token from session 2 was accepted by session 1');
        checkCondition(
            str_contains($cross['body'], 'CSRF token mismatch') || str_contains($cross['body'], 'Page Expired'),
            'cross-session CSRF rejection had no Laravel expiry or mismatch response',
        );
        checkCondition(!str_contains($cross['body'], 'cross-session'), 'cross-session CSRF rejection leaked the submitted marker');
    } finally {
        cleanupCookies($cookies);
    }

    return 'own CSRF tokens validate; a different session token is rejected';
};

$runValidation = static function () use ($baseUrl, $parallel): string {
    $cookies = [];
    $formSpecs = [];
    for ($i = 1; $i <= $parallel; $i++) {
        $cookies[$i] = cookieFile();
        $formSpecs[$i] = ['url' => "$baseUrl/csrf-form", 'cookie' => $cookies[$i]];
    }

    try {
        $forms = requestBatch($formSpecs);
        $tokens = [];
        foreach ($forms as $i => $response) {
            $tokens[$i] = getJson($response)['token'] ?? null;
            checkCondition(is_string($tokens[$i]) && $tokens[$i] !== '', "validation CSRF token $i is missing");
        }

        $failedSpecs = [];
        for ($i = 1; $i <= $parallel; $i++) {
            $failedSpecs[$i] = [
                'url' => "$baseUrl/validation-fail",
                'method' => 'POST',
                'body' => http_build_query(['_token' => $tokens[$i], 'marker' => "validation-$i"]),
                'headers' => ['Content-Type: application/x-www-form-urlencoded', 'Accept: application/json'],
                'cookie' => $cookies[$i],
            ];
        }
        $failed = requestBatch($failedSpecs);
        foreach ($failed as $i => $response) {
            checkCondition($response['status'] === 302, "validation request $i did not redirect: HTTP {$response['status']}");
        }

        $errorSpecs = [];
        for ($i = 1; $i <= $parallel; $i++) {
            $errorSpecs[$i] = ['url' => "$baseUrl/validation-errors", 'cookie' => $cookies[$i]];
        }
        $errors = requestBatch($errorSpecs);
        foreach ($errors as $i => $response) {
            $row = getJson($response);
            checkCondition(($row['message'] ?? null) === "validation-validation-$i", "validation error $i came from another session");
        }
    } finally {
        cleanupCookies($cookies);
    }

    return '8/8 flashed validation errors remained in their own sessions';
};

$runQueue = static function () use ($parallelSpecs): string {
    $responses = requestBatch($parallelSpecs(static fn (int $id): string => "/queue?marker=queue-$id&sleep=0.4"));
    $rows = [];
    foreach ($responses as $id => $response) {
        $row = getJson($response);
        checkCondition(($row['marker'] ?? null) === "queue-$id", "queue request $id returned another marker");
        checkCondition(($row['job']['marker'] ?? null) === "queue-$id", "sync job $id handled another marker");
        checkCondition(($row['job']['app_oid'] ?? null) === ($row['app_oid'] ?? null), "sync job $id used another application");
        checkCondition(($row['job']['request_oid'] ?? null) === ($row['request_oid'] ?? null), "sync job $id used another request");
        $rows[] = $row;
    }
    checkOwnIdentity($rows);

    return '8/8 sync jobs used the dispatching request context';
};

$runBroadcast = static function () use ($parallelSpecs): string {
    $responses = requestBatch($parallelSpecs(static fn (int $id): string => "/broadcast?marker=broadcast-$id&sleep=0.4"));
    $rows = [];
    foreach ($responses as $id => $response) {
        $row = getJson($response);
        checkCondition(($row['marker'] ?? null) === "broadcast-$id", "broadcast request $id returned another marker");
        checkCondition(($row['event_name'] ?? null) === 'laravel025.probe', "broadcast request $id returned another event");
        checkCondition(($row['channel'] ?? null) === "laravel025.probe.broadcast-$id", "broadcast request $id returned another channel");
        $rows[] = $row;
    }
    checkOwnIdentity($rows);

    return '8/8 synchronous broadcast events retained their own marker and channel';
};

$runRateLimit = static function () use ($parallelSpecs): string {
    $responses = requestBatch($parallelSpecs(static fn (int $id): string => "/rate-limit?id=$id&sleep=0.4"));
    $rows = [];
    foreach ($responses as $id => $response) {
        $row = getJson($response);
        checkCondition(($row['ok'] ?? false) === true, "rate-limit request $id counted the wrong key: attempts=".json_encode($row['attempts'] ?? null));
        checkCondition(($row['attempts'] ?? 0) === 2, "rate-limit request $id attempts is not 2");
        checkCondition(($row['remaining'] ?? -1) === 58, "rate-limit request $id remaining is not 58");
        $rows[] = $row;
    }
    checkUnique($rows, 'rate_limiter_root_oid');
    checkOwnIdentity($rows);

    return '8/8 rate-limiter hits were attributed to their own key';
};

$runMail = static function () use ($baseUrl, $parallel): string {
    $responses = requestBatch((static function () use ($baseUrl, $parallel): array {
        $specs = [];
        for ($i = 1; $i <= $parallel; $i++) {
            $specs[$i] = ['url' => "$baseUrl/mail?sleep=0.4"];
        }

        return $specs;
    })());
    $markers = [];
    $rows = [];
    foreach ($responses as $i => $response) {
        $row = getJson($response);
        checkCondition(is_string($row['marker'] ?? null) && $row['marker'] !== '', "mail request $i returned no marker");
        $markers[] = $row['marker'];
        $rows[] = $row;
    }
    checkUnique(array_map(static fn (string $marker): array => ['marker' => $marker], $markers), 'marker');
    checkUnique($rows, 'mailer_root_oid');
    checkUnique($rows, 'log_root_oid');
    checkOwnIdentity($rows);

    // The log transport writes each message as a multi-line block: a "To:"
    // line first, the body line a few lines later (full headers in between,
    // none of which carry the marker). Attribution is asserted on the file,
    // not on the HTTP response: each marker must appear exactly once as a
    // "To:" line and once in a body line, the body must belong to the same
    // block, and no line in that block may carry another request's marker
    // (a mid-block interleave between two Monolog handlers would produce
    // that).
    $logFile = dirname(__DIR__).'/storage/logs/laravel.log';
    checkCondition(is_file($logFile), "mail log file is missing: $logFile");
    $lines = is_file($logFile) ? file($logFile, FILE_IGNORE_NEW_LINES) : [];
    checkCondition($lines !== false, "could not read $logFile");
    foreach ($markers as $marker) {
        $toIndex = null;
        $bodyIndex = null;
        $other = [];
        foreach ($lines as $index => $line) {
            if (!str_contains($line, $marker)) {
                continue;
            }
            if (str_starts_with($line, "To: $marker@")) {
                $toIndex = $toIndex === null ? $index : 'duplicate';
            } elseif (str_contains($line, "laravel025 mail body $marker")) {
                $bodyIndex = $bodyIndex === null ? $index : 'duplicate';
            } else {
                $other[] = $line;
            }
        }
        checkCondition($toIndex === null || $toIndex !== 'duplicate', "mail marker $marker has more than one To: line (message sent more than once)");
        checkCondition($bodyIndex === null || $bodyIndex !== 'duplicate', "mail marker $marker has more than one body line (message logged more than once)");
        checkCondition($toIndex !== null, "mail marker $marker has no To: line in $logFile");
        checkCondition($bodyIndex !== null, "mail marker $marker has no body line in $logFile");
        checkCondition($other === [], "mail marker $marker appears on unexpected lines: ".json_encode($other));
        if (is_int($toIndex) && is_int($bodyIndex)) {
            checkCondition($bodyIndex > $toIndex, "mail body for $marker appears before its To: line");
            checkCondition($bodyIndex - $toIndex <= 20, "mail body for $marker is not in the same log block as its To: line");
            for ($index = $toIndex; $index <= $bodyIndex; $index++) {
                foreach ($markers as $otherMarker) {
                    if ($otherMarker !== $marker) {
                        checkCondition(!str_contains($lines[$index], $otherMarker), "mail log block for $marker also carries $otherMarker (interleaved write)");
                    }
                }
            }
        }
    }

    return '8/8 log-transport mails attributed to one intact log line each';
};

$runView = static function () use ($parallelSpecs): string {
    $responses = requestBatch($parallelSpecs(static fn (int $id): string => '/view?sleep=0.4'));
    $rows = [];
    foreach ($responses as $id => $response) {
        $row = getJson($response);
        checkCondition(($row['ok'] ?? false) === true, "view request $id rendered another request's data: ".json_encode($row['html'] ?? null));
        checkCondition(str_contains($row['html'] ?? '', 'composer-'), "view request $id has no composer data");
        $rows[] = $row;
    }
    checkUnique($rows, 'view_root_oid');
    checkOwnIdentity($rows);

    return '8/8 Blade renders and per-request view composers stayed request-local';
};

$runBinding = static function () use ($baseUrl, $parallel): string {
    // users alice/bob are seeded without guaranteed ids; discover the
    // id -> name mapping once before the concurrent window so the assertion
    // is on data, not on an assumed seed order.
    $names = [];
    for ($userId = 1; $userId <= 2; $userId++) {
        $response = requestBatch([['url' => "$baseUrl/binding/$userId?sleep=0"]])[0];
        $row = getJson($response);
        $boundId = $row['bound_id'] ?? null;
        checkCondition($boundId === $userId, "binding discovery for id $userId returned ".json_encode($boundId));
        $names[$userId] = (string) ($row['bound_name'] ?? '');
        checkCondition(in_array($names[$userId], ['alice', 'bob'], true), "binding discovery for id $userId returned an unknown user");
    }

    $specs = [];
    $expected = [];
    for ($i = 1; $i <= $parallel; $i++) {
        $userId = $i <= 4 ? 1 : 2;
        $specs[$i] = ['url' => "$baseUrl/binding/$userId?sleep=0.4"];
        $expected[$i] = $userId;
    }
    $responses = requestBatch($specs);
    $rows = [];
    foreach ($responses as $i => $response) {
        $row = getJson($response);
        checkCondition(($row['ok'] ?? false) === true, "binding request $i failed its own check: ".json_encode($row));
        checkCondition(($row['bound_id'] ?? null) === $expected[$i], "binding request $i resolved user ".($row['bound_id'] ?? null)." instead of {$expected[$i]}");
        checkCondition(($row['bound_name'] ?? null) === $names[$expected[$i]], "binding request $i resolved name ".($row['bound_name'] ?? null)." instead of {$names[$expected[$i]]}");
        $rows[] = $row;
    }
    checkOwnIdentity($rows);

    return '8/8 implicit route model bindings resolved the caller\'s model';
};

$runEloquentStatics = static function () use ($parallelSpecs): string {
    $responses = requestBatch($parallelSpecs(static fn (int $id): string => "/eloquent-statics?id=$id&sleep=0.4"));
    $rows = [];
    foreach ($responses as $id => $response) {
        $row = getJson($response);
        checkCondition(($row['ok'] ?? false) === true, "eloquent-statics request $id returned ok=false: ".json_encode($row));
        checkCondition(($row['item'] ?? null) === "item-$id", "eloquent-statics request $id returned another item");
        checkCondition(is_array($row['observer_record'] ?? null), "eloquent-statics request $id has no observer record");
        checkCondition(($row['observer_record']['item_id'] ?? null) === $id, "eloquent-statics request $id observer fired for another item");
        $rows[] = $row;
    }
    checkUnique($rows, 'dispatcher_oid');
    checkOwnIdentity($rows);

    return '8/8 model observers and global scopes stayed request-local';
};

$runStaticsAudit = static function (int $round): array {
    $specs = [];
    for ($i = 1; $i <= 8; $i++) {
        $specs[$i] = ['url' => getenv('LARAVEL_BASE_URL').'/statics-audit?sleep=0.4&marker=round'.$round.'-'.$i];
    }
    $responses = requestBatch($specs);
    $changed = [];
    $checked = 0;
    $failed = [];
    foreach ($responses as $i => $response) {
        // Under an empty isolate list the audit route itself can be killed by
        // cross-request damage (an HTTP 500 from a polluted subsystem is
        // evidence, not a reason to abort the audit); the other responses
        // still carry usable snapshots.
        if ($response['error'] !== '' || $response['status'] !== 200) {
            $failed[$i] = $response['status'] === 0 ? $response['error'] : 'HTTP '.$response['status'];
            continue;
        }
        try {
            $row = json_decode($response['body'], true, 512, JSON_THROW_ON_ERROR);
        } catch (Throwable) {
            $failed[$i] = 'invalid JSON';
            continue;
        }
        if (!is_array($row['changed'] ?? null)) {
            $failed[$i] = 'no changed map';
            continue;
        }
        $checked = max($checked, (int) ($row['checked'] ?? 0));
        foreach ($row['changed'] as $name => $detail) {
            $changed[$name] = $detail;
        }
    }

    return ['checked' => $checked, 'changed' => $changed, 'failed' => $failed];
};

// Statics that legitimately change across a suspension even with isolation
// active: process-shared caches of pure function objects, holding no request
// data. Measured on Laravel 13.30.1: opis/closure's Native serializer caches
// two static closures on first use and whichever request serializes a closure
// last leaves its instance there. Excluding them requires a justification on
// this list; anything not here that changes in a clean audit fails the run.
$auditExclusions = [
    'Laravel\SerializableClosure\Serializers\Native::transformUseVariables',
    'Laravel\SerializableClosure\Serializers\Native::resolveUseVariables',
];

$configuredTests = [
    'session-rounds' => $runSession,
    'authenticated-route' => $runAuthenticated,
    'mix-mysql-cache-redis-eloquent' => $runMix,
    'facade-and-object-identity' => $runIdentity,
    'eloquent-connection-resolver' => $runEloquent,
    'middleware-and-terminate' => $runLifecycle,
    'csrf-session-isolation' => $runCsrf,
    'validation-flash-session-isolation' => $runValidation,
    'queue-sync-context' => $runQueue,
    'broadcast-sync-context' => $runBroadcast,
    'rate-limiter' => $runRateLimit,
    'mail-attribution' => $runMail,
    'view-blade-composer' => $runView,
    'route-model-binding' => $runBinding,
    'eloquent-observers-and-global-scopes' => $runEloquentStatics,
];
$negativeTests = [
    'negative-session-empty-static-list' => $runSession,
    'negative-authenticated-empty-static-list' => $runAuthenticated,
    'negative-mix-empty-static-list' => $runMix,
    'negative-facade-and-object-identity-empty-static-list' => $runIdentity,
    'negative-eloquent-connection-resolver-empty-static-list' => $runEloquent,
    'negative-middleware-and-terminate-empty-static-list' => $runLifecycle,
    'negative-csrf-session-isolation-empty-static-list' => $runCsrf,
    'negative-validation-flash-session-isolation-empty-static-list' => $runValidation,
    'negative-queue-sync-context-empty-static-list' => $runQueue,
    'negative-broadcast-sync-context-empty-static-list' => $runBroadcast,
    'negative-rate-limiter-empty-static-list' => $runRateLimit,
    'negative-mail-attribution-empty-static-list' => $runMail,
    'negative-view-blade-composer-empty-static-list' => $runView,
    'negative-route-model-binding-empty-static-list' => $runBinding,
    'negative-eloquent-observers-and-global-scopes-empty-static-list' => $runEloquentStatics,
];

if ($mode === 'audit') {
    // Systematic statics audit (task 025). Two rounds against the same pool:
    // round 1 is cold (concurrent boot itself flips boot-once statics), so
    // the steady-state verdict comes from round 2 — statics that still change
    // across a suspension there hold per-request state.
    //
    // Default (and the only mode bin/run.sh uses): LARAVEL_AUDIT_EXPECT=clean
    // with the configured isolate list active. The probe touches every
    // state-keeping subsystem first, so a static another request overwrites
    // during our suspension shows up here even though the pool stays healthy
    // — safe enumeration. Anything in the steady-state change set that is
    // neither on the list nor a documented benign exclusion is reported as
    // UNCOVERED and fails the run.
    //
    // LARAVEL_AUDIT_EXPECT=leak (pool list empty) is a manual forensic mode:
    // every request-scoped static changes, and the probe itself tends to die
    // (cross-request damage: HTTP 500s, 502s, MySQL protocol corruption —
    // dangerous against a SHARED MySQL server). Failed requests count as
    // evidence, not as a reason to abort.
    $isolated = array_filter(array_map('trim', explode(',', (string) getenv('LARAVEL_ISOLATED_LIST'))));
    $expect = getenv('LARAVEL_AUDIT_EXPECT') ?: 'clean';
    $round1 = $runStaticsAudit(1);
    $round2 = $runStaticsAudit(2);

    echo "AUDIT expect=$expect checked={$round2['checked']} round1_changed=".count($round1['changed'])." round2_changed=".count($round2['changed'])."\n";
    foreach (['round1' => $round1, 'round2' => $round2] as $label => $round) {
        foreach ($round['failed'] as $i => $reason) {
            echo "  $label request $i FAILED: $reason\n";
        }
        foreach ($round['changed'] as $name => $detail) {
            echo "  $label $name {$detail['before']} -> {$detail['after']}\n";
        }
    }

    // A request that died mid-audit is evidence (cross-request damage under
    // an empty list; an unhealthy path the list does not cover in clean
    // mode) — but it is NOT a static name, and one transient infra error
    // (a single 500/curl glitch) must not be indistinguishable from an
    // uncovered static in the verdict. So: statics and failed requests are
    // reported separately, and a round with failed requests but zero static
    // changes gets exactly one extra round to adjudicate — a repeat failure
    // fails the run, a clean retry passes with the transient on record.
    $verdictRound = $round2;
    if ($round2['failed'] !== [] && $round2['changed'] === []) {
        $verdictRound = $runStaticsAudit(3);
        echo "AUDIT retry checked={$verdictRound['checked']} changed=".count($verdictRound['changed'])." failed=".count($verdictRound['failed'])."\n";
        foreach ($verdictRound['failed'] as $i => $reason) {
            echo "  retry request $i FAILED: $reason\n";
        }
        foreach ($verdictRound['changed'] as $name => $detail) {
            echo "  retry $name {$detail['before']} -> {$detail['after']}\n";
        }
    }

    $steady = array_keys($verdictRound['changed']);
    $uncovered = [];
    foreach ($steady as $name) {
        if (in_array($name, $auditExclusions, true)) {
            continue;
        }
        if ($expect !== 'clean' && in_array($name, $isolated, true)) {
            continue;
        }
        $uncovered[] = $name;
    }
    echo 'AUDIT_ISOLATED_LIST='.implode(',', $isolated)."\n";
    echo 'AUDIT_FAILED_REQUESTS='.(implode(',', array_keys($verdictRound['failed'])) ?: '(none)')."\n";
    if ($uncovered === [] && $verdictRound['failed'] === []) {
        echo 'AUDIT_RESULT=COVERED steady='.implode(',', $steady ?: ['(none)'])."\n";
        $results['statics-audit'] = 'PASS';
    } else {
        echo 'AUDIT_RESULT=UNCOVERED statics='.implode(',', $uncovered ?: ['(none)']).' failed_requests='.implode(',', array_keys($verdictRound['failed']))."\n";
        $results['statics-audit'] = 'ERROR';
    }
} else {
    $tests = $mode === 'negative' ? $negativeTests : $configuredTests;

    if ($only !== '') {
        checkCondition(isset($tests[$only]), "unknown Laravel test scenario: $only");
        recordResult($only, $tests[$only]);
    } else {
        foreach ($tests as $name => $test) {
            recordResult($name, $test);
        }
    }
}

$counts = array_count_values($results);
echo sprintf(
    "SUMMARY mode=%s pass=%d error=%d not_measured=%d\n",
    $mode,
    $counts['PASS'] ?? 0,
    $counts['ERROR'] ?? 0,
    $counts['NOT MEASURED'] ?? 0,
);

exit(($counts['ERROR'] ?? 0) === 0 ? 0 : 1);
