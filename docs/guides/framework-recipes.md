# Framework recipes: Symfony and Laravel

Recipes for the two big frameworks on the `classic` executor, which is what
`pool.type = fastcgi` behind a gateway and `pool.type = http-direct` with the
default executor run: one request at a time per worker, every request starts
from a clean request state, so the framework needs no php-fpm-ng specific
configuration. Other things are out of scope on purpose: the `fiber` and `async`
executors are not on `main` ([`frameworks.md`](../frameworks.md)), and the beta
`worker` executor is not a framework runtime (no Octane or worker-mode promise).

**What was verified, and what was not.** The two configurations below are run
through `php-fpm-ng -t` by CI (`build/test-doc-configs.sh`). In addition, on
2026-10-03 on Ubuntu 26.04 with PHP 8.5.4, a fresh `symfony/skeleton` 7.4.20 and a
fresh `laravel/laravel` (Laravel 13.34.0), each with a one-route hello
controller, served through a gateway and these pools: a 200 for the route, a
404 for a path that does not exist, a static file from `public/`, and one run
of the cron wrapper. That was done by hand, once, and is not in CI; CI
runs only the Slim 4 smoke test ([`frameworks.md`](../frameworks.md#what-is-tested)).
Databases, caches, queues and sessions beyond the file driver were not tried,
and neither was the long-running worker wrapper (see the end).

## Layout

Both frameworks keep the front controller in `public/`. The gateway's `chdir` is
that directory, the front controller is `/index.php`, and the static files of
`public/` are served by the gateway without starting PHP.

## Symfony

```ini verify
[gateway]
pool.type = gateway
user = www-data
group = www-data
listen = 0.0.0.0:8080
chdir = /srv/app/public
http.static = yes
http.front_controller = /index.php
http.route[app] = /

[app]
pool.type = fastcgi
user = www-data
group = www-data
listen = /run/php-fpm-ng/app.sock
listen.owner = www-data
listen.group = www-data
chdir = /srv/app/public
pm = dynamic
pm.max_children = 10
pm.start_servers = 2
pm.min_spare_servers = 2
pm.max_spare_servers = 4
env[APP_ENV] = prod
env[APP_DEBUG] = 0

; Symfony console commands on a schedule, in place of a crontab line.
[console-task]
pool.type = cron
user = www-data
group = www-data
cron.schedule = */5 * * * *
cron.script = /srv/app/bin/task.php
cron.log = /var/log/php-fpm-ng/console-task.log
cron.output_log = /var/log/php-fpm-ng/console-task.out
env[APP_ENV] = prod
env[APP_DEBUG] = 0
```

Before the first start, as the user that owns the workers:
`composer install --no-dev --optimize-autoloader`, then
`APP_ENV=prod php bin/console cache:clear`. `var/` must be writable by
`www-data`. `.env` is read by Symfony itself; the `env[...]` lines are only for
what you want to force.

`bin/task.php` is what `cron.script` points at. A cron or supervisor pool runs a
PHP file, not `bin/console`, so the file boots the kernel and runs one command
(replace `about` with yours):

```php
<?php
use App\Kernel;
use Symfony\Bundle\FrameworkBundle\Console\Application;
use Symfony\Component\Console\Input\ArrayInput;
use Symfony\Component\Dotenv\Dotenv;

require dirname(__DIR__).'/vendor/autoload.php';
(new Dotenv())->bootEnv(dirname(__DIR__).'/.env');
$app = new Application(new Kernel($_SERVER['APP_ENV'], (bool) $_SERVER['APP_DEBUG']));
$app->setAutoExit(false);
exit($app->run(new ArrayInput(['command' => 'about'])));
```

## Laravel

The gateway and the PHP pool are the same as for Symfony. The Laravel
differences are the environment and the scheduler.

```ini verify
[gateway]
pool.type = gateway
user = www-data
group = www-data
listen = 0.0.0.0:8080
chdir = /srv/app/public
http.static = yes
http.front_controller = /index.php
http.route[app] = /

[app]
pool.type = fastcgi
user = www-data
group = www-data
listen = /run/php-fpm-ng/app.sock
listen.owner = www-data
listen.group = www-data
chdir = /srv/app/public
pm = dynamic
pm.max_children = 10
pm.start_servers = 2
pm.min_spare_servers = 2
pm.max_spare_servers = 4
env[APP_ENV] = production
env[APP_DEBUG] = 0

; `php artisan schedule:run` every minute, in place of the documented crontab line.
[scheduler]
pool.type = cron
user = www-data
group = www-data
cron.schedule = * * * * *
cron.script = /srv/app/bin/schedule.php
cron.log = /var/log/php-fpm-ng/scheduler.log
cron.output_log = /var/log/php-fpm-ng/scheduler.out
env[APP_ENV] = production
env[APP_DEBUG] = 0
```

`storage/` and `bootstrap/cache/` must be writable by `www-data`. Keep `APP_KEY`
and the other secrets in Laravel's own `.env`: see the second trap below for why
they do not belong in `env[...]`. The default `.env` of a new project uses the
`database` session, cache and queue drivers; the test above used the `file`
session and cache drivers because the box had no database.

`bin/schedule.php`:

```php
<?php
// A cron pool runs this file without a CLI argv, and Laravel's console reads it.
$_SERVER['argv'] = ['artisan', 'schedule:run'];
$_SERVER['argc'] = 2;
$_SERVER['PHP_SELF'] = 'artisan';

require __DIR__.'/../vendor/autoload.php';
$app = require __DIR__.'/../bootstrap/app.php';
$kernel = $app->make(Illuminate\Contracts\Console\Kernel::class);
exit($kernel->call('schedule:run'));
```

Without the three `$_SERVER` lines the run fails with `Undefined array key
"PHP_SELF"` from Symfony's `DumpCompletionCommand` (observed, Laravel 13.34.0).

## Two traps in `env[...]`

Both were hit while writing these recipes and are upstream FPM behaviour:

- **`false`, `off`, `no`, `none` and `null` read as an empty value**, and an empty
  value is refused (`empty value`) at `-t`. Write `env[APP_DEBUG] = 0`, not
  `false`.
- **A value containing `=` must be quoted**, for example
  `env[APP_KEY] = "base64:..."`, or the INI parser reports a syntax error.

## Long-running workers

A Messenger consumer or `queue:work` runs under `pool.type = supervisor` with
the same pattern as `bin/task.php`: a PHP file that boots the application and runs
the command, with `supervisor.restart = always`. This is the part **not tried**
here (no queue backend on the test box), so test it before relying on it. Use
`--time-limit` / `--max-time` so the process is recycled, and mind that a
supervised script that returns at once is restarted at once
([`supervisor.md`](../supervisor.md)). For the shape of the pool see
[migrate-supervisord-cron.md](migrate-supervisord-cron.md).
