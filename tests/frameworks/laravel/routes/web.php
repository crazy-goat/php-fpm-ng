<?php

use App\Events\ProbeBroadcast;
use App\Jobs\ProbeJob;
use App\Models\Item;
use App\Models\User;
use Illuminate\Container\Container;
use Illuminate\Foundation\Application;
use Illuminate\Http\Request;
use Illuminate\Support\Facades\Auth;
use Illuminate\Support\Facades\Bus;
use Illuminate\Support\Facades\Cache;
use Illuminate\Support\Facades\DB;
use Illuminate\Support\Facades\Event;
use Illuminate\Support\Facades\Facade;
use Illuminate\Support\Facades\Log;
use Illuminate\Support\Facades\Mail;
use Illuminate\Support\Facades\RateLimiter;
use Illuminate\Support\Facades\Redis;
use Illuminate\Support\Facades\Route;
use Illuminate\Support\Facades\Validator;
use Illuminate\Support\Facades\View;
use App\Observers\ProbeItemObserver;

$objectId = static function (mixed $value): ?int {
    return is_object($value) ? spl_object_id($value) : null;
};

$common = static function (Request $request) use ($objectId): array {
    $application = app();
    $container = Container::getInstance();
    $facadeApplication = Facade::getFacadeApplication();

    return [
        'pid' => getmypid(),
        'uri' => $request->getRequestUri(),
        'app_oid' => spl_object_id($application),
        'request_oid' => spl_object_id($request),
        'container_request_oid' => $objectId($application->make('request')),
        'container_instance_oid' => $objectId($container),
        'facade_app_oid' => $objectId($facadeApplication),
        'same_request' => $request === $application->make('request'),
        'auth_root_oid' => $objectId(Auth::getFacadeRoot()),
        'cache_root_oid' => $objectId(Cache::getFacadeRoot()),
        'db_root_oid' => $objectId(DB::getFacadeRoot()),
        'redis_root_oid' => $objectId(Redis::getFacadeRoot()),
        'middleware_oid' => $request->attributes->get('probe.middleware_oid'),
        'middleware_marker' => $request->attributes->get('probe.middleware_marker'),
        'included' => count(get_included_files()),
        'ob_level' => ob_get_level(),
        'memory_mb' => round(memory_get_usage(true) / 1048576, 1),
        'laravel_start_defined' => defined('LARAVEL_START'),
        // Constants are process-wide in this SAPI; recording the value per
        // response documents that the first request's LARAVEL_START survives
        // into every later request, instead of assuming it.
        'laravel_start' => defined('LARAVEL_START') ? (string) constant('LARAVEL_START') : null,
    ];
};

$suspend = static function (Request $request): void {
    $seconds = max(0.0, min(2.0, (float) $request->query('sleep', 0.3)));
    if ($seconds > 0) {
        DB::selectOne('SELECT SLEEP(?) AS slept', [$seconds]);
    }
};

$json = static fn (array $payload, int $status = 200) => response()->json($payload, $status);

Route::get('/', static fn () => response()->json([
    'ok' => true,
    'framework' => 'laravel',
    'version' => Application::VERSION,
]));

Route::get('/up', static fn () => response()->json(['ok' => true, 'framework' => 'laravel']));

Route::get('/session', static function (Request $request) use ($common, $suspend, $json): mixed {
    $user = (string) $request->query('user', 'anonymous');
    $session = $request->session();

    if (!$session->has('user')) {
        $session->put('user', $user);
    }
    $count = (int) $session->get('count', 0) + 1;
    $session->put('count', $count);

    $sidBefore = $session->getId();
    $userBefore = $session->get('user');
    $suspend($request);
    $sidAfter = $request->session()->getId();
    $userAfter = $request->session()->get('user');

    return $json([
        'user_param' => $user,
        'sess_user' => $userAfter,
        'sess_user_before_suspend' => $userBefore,
        'sid' => $sidAfter,
        'sid_before_suspend' => $sidBefore,
        'count' => (int) $request->session()->get('count', 0),
        'ok' => $userAfter === $user && $sidAfter === $sidBefore,
        'driver' => config('session.driver'),
    ] + $common($request));
});

Route::get('/login', static function (Request $request) use ($common, $json): mixed {
    $name = (string) $request->query('user', 'anonymous');
    $user = User::query()->where('name', $name)->firstOrFail();

    Auth::login($user);
    $request->session()->regenerate();

    return $json([
        'logged_in' => $user->name,
        'sid' => $request->session()->getId(),
        'auth_id' => Auth::id(),
    ] + $common($request));
});

Route::get('/me', static function (Request $request) use ($common, $suspend, $json): mixed {
    $before = Auth::user()?->name;
    $beforeId = Auth::id();
    $suspend($request);
    $after = Auth::user()?->name;
    $afterId = Auth::id();

    return $json([
        'user' => $after,
        'user_before_suspend' => $before,
        'auth_id' => $afterId,
        'auth_id_before_suspend' => $beforeId,
        'sid' => $request->session()->getId(),
        'auth_check' => Auth::check(),
    ] + $common($request));
});

Route::get('/mix', static function (Request $request) use ($common, $suspend, $json): mixed {
    $id = max(1, min(8, (int) $request->query('id', 1)));
    $token = bin2hex(random_bytes(8));
    $redisKey = 'laravel025:mix:'.$id.':'.$token;
    $cacheKey = 'laravel025.mix.'.$id.'.'.$token;
    $tag = 'db-'.$id.'-'.$token;
    $redisValue = 'redis-'.$id.'-'.$token;
    $cacheValue = 'cache-'.$id.'-'.$token;

    Redis::setex($redisKey, 60, $redisValue);
    Cache::put($cacheKey, $cacheValue, 60);
    $row = DB::selectOne(
        'SELECT SLEEP(?) AS slept, CONNECTION_ID() AS connection_id, ? AS tag',
        [max(0.0, min(2.0, (float) $request->query('sleep', 0.3))), $tag],
    );
    $item = Item::query()->find($id);
    $redisBack = Redis::get($redisKey);
    $cacheBack = Cache::get($cacheKey);

    return $json([
        'id' => $id,
        'item' => $item?->label,
        'db_tag' => $row->tag ?? null,
        'db_connection_id' => (int) ($row->connection_id ?? 0),
        'redis' => $redisBack,
        'cache' => $cacheBack,
        'ok' => ($item?->label === 'item-'.$id)
            && ($row->tag ?? null) === $tag
            && $redisBack === $redisValue
            && $cacheBack === $cacheValue,
    ] + $common($request));
});

Route::get('/identity', static function (Request $request) use ($common, $suspend, $json): mixed {
    $suspend($request);

    return $json($common($request));
});

Route::get('/eloquent', static function (Request $request) use ($common, $suspend, $json, $objectId): mixed {
    $id = max(1, min(8, (int) $request->query('id', 1)));
    $resolverBefore = Item::getConnectionResolver();
    $connectionBefore = Item::resolveConnection()->getPdo();
    $suspend($request);
    $resolverAfter = Item::getConnectionResolver();
    $connectionAfter = Item::resolveConnection()->getPdo();
    $item = Item::query()->find($id);

    return $json([
        'id' => $id,
        'item' => $item?->label,
        'resolver_before_oid' => $objectId($resolverBefore),
        'resolver_after_oid' => $objectId($resolverAfter),
        'connection_before_oid' => $objectId($connectionBefore),
        'connection_after_oid' => $objectId($connectionAfter),
        'resolver_stayed_same' => $resolverBefore === $resolverAfter,
        'connection_stayed_same' => $connectionBefore === $connectionAfter,
        'ok' => $item?->label === 'item-'.$id && $resolverBefore === $resolverAfter,
    ] + $common($request));
});

Route::get('/lifecycle', static function (Request $request) use ($common, $suspend, $json): mixed {
    $suspend($request);

    return $json([
        'marker' => (string) $request->query('middleware_marker', ''),
        'middleware_marker_from_request' => $request->attributes->get('probe.middleware_marker'),
    ] + $common($request));
});

Route::get('/terminate-check', static function (Request $request) use ($json): mixed {
    $marker = (string) $request->query('marker', '');
    $value = $marker === '' ? null : Redis::get('laravel025:terminate:'.$marker);

    return $json([
        'marker' => $marker,
        'terminate' => $value === null ? null : json_decode($value, true, 512, JSON_THROW_ON_ERROR),
    ]);
});

Route::get('/csrf-form', static function (Request $request) use ($common, $json): mixed {
    return $json([
        'token' => $request->session()->token(),
        'sid' => $request->session()->getId(),
    ] + $common($request));
});

Route::post('/csrf-submit', static function (Request $request) use ($common, $json): mixed {
    return $json([
        'marker' => (string) $request->input('marker', ''),
        'csrf_token_present' => is_string($request->input('_token')),
    ] + $common($request));
});

Route::post('/validation-fail', static function (Request $request) {
    $marker = (string) $request->input('marker', '');
    $validator = Validator::make(
        [],
        ['probe' => ['required']],
        ['probe.required' => 'validation-'.$marker],
    );

    return redirect('/validation-errors')->withErrors($validator)->withInput();
});

Route::get('/validation-errors', static function (Request $request) use ($common, $json): mixed {
    $messages = $request->session()->get('errors')?->get('probe') ?? [];

    return $json([
        'messages' => array_values($messages),
        'message' => $messages[0] ?? null,
    ] + $common($request));
});

Route::get('/queue', static function (Request $request) use ($common, $suspend, $json): mixed {
    $marker = (string) $request->query('marker', '');
    $sleep = max(0.0, min(2.0, (float) $request->query('sleep', 0.3)));
    Bus::dispatchSync(new ProbeJob($marker, $sleep));
    $job = Redis::get('laravel025:job:'.$marker);

    return $json([
        'marker' => $marker,
        'job' => $job === null ? null : json_decode($job, true, 512, JSON_THROW_ON_ERROR),
    ] + $common($request));
});

Route::get('/broadcast', static function (Request $request) use ($common, $suspend, $json): mixed {
    $marker = (string) $request->query('marker', '');
    Event::dispatch(new ProbeBroadcast($marker));
    $suspend($request);

    return $json([
        'marker' => $marker,
        'event_name' => 'laravel025.probe',
        'channel' => 'laravel025.probe.'.$marker,
    ] + $common($request));
});

// Task 025: runtime statics audit. The request boots the app, touches every
// subsystem that keeps state (so each one initializes whatever class statics
// it owns in THIS request), snapshots every static property of every declared
// class, blocks on a real MySQL suspension, then snapshots again. Any static
// whose value changed across the suspension was overwritten by another
// concurrent request: that static holds per-request state and needs to be on
// the fiber.isolate_statics list. This is the systematic replacement for the
// hand-picked empirical list ("the spike counted 237 static $ declarations
// and hand-picked 12; only 3 were ever confirmed").
$staticsAudit = static function (Request $request) use ($json): mixed {
    $seconds = max(0.0, min(2.0, (float) $request->query('sleep', 0.4)));
    $marker = (string) $request->query('marker', 'audit');

    // Touch each subsystem before snapshotting so its statics are initialized
    // by this request rather than appearing as "changed" only because another
    // request initialized them first.
    Cache::get("laravel025:audit:$marker");
    Redis::get("laravel025:audit:$marker");
    DB::selectOne('SELECT 1');
    Log::info("laravel025 audit warm $marker");
    view('probe', ['marker' => $marker])->render();
    Event::dispatch(new ProbeBroadcast($marker));
    RateLimiter::hit("laravel025:audit:$marker");
    Mail::raw("laravel025 audit warm $marker", static fn ($message) => $message->to('audit025@example.test'));
    Auth::user();

    $signature = static function (mixed $value): string {
        if (is_object($value)) {
            return 'obj:'.spl_object_id($value);
        }
        try {
            return 'val:'.md5(serialize($value));
        } catch (Throwable) {
            return 'unserializable';
        }
    };

    $snapshot = static function () use ($signature): array {
        $out = [];
        foreach (get_declared_classes() as $class) {
            foreach ((new ReflectionClass($class))->getProperties(ReflectionProperty::IS_STATIC) as $property) {
                // Only the declaring class reports the property, so inherited
                // statics are counted once under their canonical owner.
                if ($property->getDeclaringClass()->getName() !== $class) {
                    continue;
                }
                $name = $class.'::'.$property->getName();
                try {
                    if (!$property->isInitialized()) {
                        $out[$name] = 'uninitialized';
                        continue;
                    }
                    $out[$name] = $signature($property->getValue());
                } catch (Throwable) {
                    $out[$name] = 'error';
                }
            }
        }

        return $out;
    };

    $before = $snapshot();
    DB::selectOne('SELECT SLEEP(?) AS slept', [$seconds]);
    $after = $snapshot();

    $changed = [];
    foreach ($after as $name => $signatureAfter) {
        $signatureBefore = $before[$name] ?? 'absent';
        if ($signatureBefore !== $signatureAfter) {
            $changed[$name] = ['before' => $signatureBefore, 'after' => $signatureAfter];
        }
    }
    // A static that appeared initialized only after the suspension was
    // initialized by another request (ours touched every subsystem already).
    foreach ($before as $name => $signatureBefore) {
        if (!array_key_exists($name, $after)) {
            $changed[$name] = ['before' => $signatureBefore, 'after' => 'absent'];
        }
    }

    return $json([
        'marker' => $marker,
        'checked' => count($after),
        'changed' => $changed,
        'pid' => getmypid(),
    ]);
};

Route::get('/statics-audit', static fn (Request $request): mixed => $staticsAudit($request));

Route::get('/rate-limit', static function (Request $request) use ($common, $suspend, $json, $objectId): mixed {
    $id = max(1, min(8, (int) $request->query('id', 1)));
    $key = "laravel025:rate:$id:".bin2hex(random_bytes(4));
    RateLimiter::hit($key, 60);
    $suspend($request);
    RateLimiter::hit($key, 60);
    $attempts = RateLimiter::attempts($key);
    $remaining = RateLimiter::remaining($key, 60);

    return $json([
        'id' => $id,
        'attempts' => $attempts,
        'remaining' => $remaining,
        'rate_limiter_root_oid' => $objectId(RateLimiter::getFacadeRoot()),
        // Two hits from this request on a unique key: anything other than 2
        // means the limiter resolved through another request's cache root and
        // counted someone else's key.
        'ok' => $attempts === 2,
    ] + $common($request));
});

Route::get('/mail', static function (Request $request) use ($common, $suspend, $json, $objectId): mixed {
    $marker = 'mail-'.bin2hex(random_bytes(4));
    $suspend($request);
    Mail::raw("laravel025 mail body $marker", static fn ($message) => $message->to("$marker@example.test"));

    return $json([
        'marker' => $marker,
        'mailer_root_oid' => $objectId(Mail::getFacadeRoot()),
        'log_root_oid' => $objectId(Log::getFacadeRoot()),
    ] + $common($request));
});

Route::get('/view', static function (Request $request) use ($common, $suspend, $json, $objectId): mixed {
    $marker = 'view-'.bin2hex(random_bytes(4));
    // Composer registered per request, the way an app's boot code would do
    // it: under classic FPM it only ever applies to the registering request.
    View::composer('probe', static function ($view) use ($marker): void {
        $view->with('composer_marker', 'composer-'.$marker);
    });
    $suspend($request);
    $html = view('probe', ['marker' => $marker])->render();

    return $json([
        'marker' => $marker,
        'html' => $html,
        'view_root_oid' => $objectId(View::getFacadeRoot()),
        'ok' => str_contains($html, $marker) && str_contains($html, 'composer-'.$marker),
    ] + $common($request));
});

Route::get('/binding/{user}', static function (Request $request, User $user) use ($common, $suspend, $json): mixed {
    $id = $user->id;
    $suspend($request);

    return $json([
        'bound_id' => $id,
        'bound_name' => $user->name,
        'ok' => in_array($user->name, ['alice', 'bob'], true),
    ] + $common($request));
});

// Model events, global scopes and the boot-once cache are the Eloquent
// statics the four-entry list never covered. Each request registers its own
// observer and scope, exactly as per-request boot code would, then suspends
// before using the model, so a static rooted in another request's container
// or accumulated from earlier requests is what the assertions catch.
Route::get('/eloquent-statics', static function (Request $request) use ($common, $suspend, $json, $objectId): mixed {
    $id = max(1, min(8, (int) $request->query('id', 1)));
    $marker = 'eloquent-'.bin2hex(random_bytes(4));

    Item::observe(new ProbeItemObserver($marker));
    Item::addGlobalScope('probe-'.$marker, static function ($query) use ($id): void {
        $query->where('label', 'item-'.$id);
    });

    $suspend($request);

    $item = Item::query()->find($id);
    $observerRecord = Redis::get('laravel025:observer:'.$marker);
    $dispatcher = Item::getEventDispatcher();

    return $json([
        'id' => $id,
        'marker' => $marker,
        'item' => $item?->label,
        'observer_record' => $observerRecord === null ? null : json_decode($observerRecord, true, 512, JSON_THROW_ON_ERROR),
        'dispatcher_oid' => $objectId($dispatcher),
        'ok' => ($item?->label === 'item-'.$id)
            && is_array($observerRecord)
            && (($observerRecord['marker'] ?? null) === $marker),
    ] + $common($request));
});
