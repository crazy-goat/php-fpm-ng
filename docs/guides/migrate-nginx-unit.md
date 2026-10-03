# Migrating from NGINX Unit

NGINX Unit was archived on 2025-10-08. A Unit installation that runs PHP, serves
static files from the same tree and routes a few paths is what one php-fpm-ng
process does: the gateway takes the listeners and routes, FPM pools take the
applications.

Unit is configured through a JSON API at run time; php-fpm-ng reads a file and
reloads on `SIGUSR2` (`systemctl reload php-fpm-ng`, optionally with
`reload.selective`, [`reload.md`](../reload.md)). Keep the file in version control
where the PUT requests used to be.

## Before

```json
{
  "listeners": { "*:8080": { "pass": "routes" } },
  "routes": [
    { "match": { "uri": "/api/*" }, "action": { "pass": "applications/api" } },
    { "action": { "share": "/srv/app/public$uri",
                  "fallback": { "pass": "applications/web" } } }
  ],
  "applications": {
    "web": {
      "type": "php",
      "root": "/srv/app/public",
      "script": "index.php",
      "user": "www-data",
      "group": "www-data",
      "processes": { "max": 10, "spare": 2, "idle_timeout": 20 },
      "limits": { "requests": 1000, "timeout": 30 },
      "environment": { "APP_ENV": "prod" },
      "options": { "admin": { "memory_limit": "256M" } }
    },
    "api": {
      "type": "php",
      "root": "/srv/app/public",
      "script": "index.php",
      "user": "www-data",
      "processes": 4
    }
  }
}
```

## After

```ini verify
[gateway]
pool.type = gateway
user = www-data
group = www-data
listen = 0.0.0.0:8080                 ; Unit: listeners "*:8080"
chdir = /srv/app/public               ; Unit: share + root (one document root)
http.static = yes                     ; Unit: share with a fallback
http.front_controller = /index.php    ; Unit: script
http.route[api] = /api                ; Unit: match uri /api/*, pass applications/api
http.route[web] = /                   ; Unit: the fallback

[web]
pool.type = fastcgi
user = www-data
group = www-data
listen = /run/php-fpm-ng/web.sock
listen.owner = www-data
listen.group = www-data
chdir = /srv/app/public
pm = dynamic                          ; Unit: processes as an object
pm.max_children = 10                  ; max
pm.start_servers = 2
pm.min_spare_servers = 2              ; spare
pm.max_spare_servers = 4
pm.max_requests = 1000                ; limits.requests
request_terminate_timeout = 30s       ; limits.timeout
env[APP_ENV] = prod                   ; environment
php_admin_value[memory_limit] = 256M  ; options.admin

[api]
pool.type = fastcgi
user = www-data
group = www-data
listen = /run/php-fpm-ng/api.sock
listen.owner = www-data
listen.group = www-data
chdir = /srv/app/public
pm = static                           ; Unit: "processes": 4
pm.max_children = 4
```

The `[gateway]` section decides by path prefix, so `/api/...` goes to `[api]`
and everything else to `[web]`; the pool names are the keys of `http.route`.

## Directive map

| Unit | php-fpm-ng | notes |
|---|---|---|
| `listeners["*:8080"]` | `listen = 0.0.0.0:8080` on a gateway | one gateway per public address |
| `routes[].match.uri` (prefix with `*`) | `http.route[<pool>] = <prefix>` | prefixes only; longest prefix wins ([`http-route.md`](../http-route.md)) |
| `action.share` | `http.static = yes` + `chdir` | `GET`/`HEAD` of files under the document root |
| `fallback` / `pass` to a PHP app | falls through to `http.front_controller` and the matching route | |
| `applications.<n>.root` | `chdir` | **every route of a gateway uses the gateway's `chdir`**: two applications with different roots need two gateways on two ports |
| `script` | `http.front_controller` | one per gateway; Unit's `index` is not needed, the front controller is explicit |
| `processes: N` | `pm = static`, `pm.max_children = N` | |
| `processes.max` / `spare` | `pm = dynamic`, `pm.max_children`, `pm.min_spare_servers`, `pm.max_spare_servers`, `pm.start_servers` | |
| `processes.idle_timeout` | `pm = ondemand`, `pm.process_idle_timeout` | |
| `limits.requests` | `pm.max_requests` | |
| `limits.timeout` | `request_terminate_timeout` | |
| `user`, `group` | `user`, `group` | the master starts as root and drops privileges, as FPM does |
| `environment` | `env[NAME] = value` | |
| `options.admin`, `options.user` | `php_admin_value[...]`, `php_value[...]` | `options.file` is `php_ini` loading, not a pool directive |
| TLS `certificate` bundle | `http.tls_cert`, `http.tls_key` | **beta**, `php-fpm-ng-tls` package only ([`tls.md`](../tls.md)) |
| `PUT /config` | edit the file, `systemctl reload php-fpm-ng` | not atomic: run `php-fpm-ng -t` first |

## What has no equivalent

- non-PHP applications (Python, Node, Go, ...): php-fpm-ng runs PHP, and the
  gateway routes only to its own pools;
- the control API and run-time reconfiguration without a reload;
- `match` on host, headers, arguments, cookies or source address (a gateway
  matches path prefixes; `http.allowed_clients` is a per-gateway ACL);
- `rewrite`, `response_headers` and proxying to an arbitrary upstream;
- HTTP/2 ([`install.md`](../install.md#tls-acme-and-the-debug-clock)).

If your Unit configuration uses any of these, migrate the PHP part and keep
a small nginx or Caddy for the rest.

## Verify

`php-fpm-ng -t -y /etc/php-fpm-ng/php-fpm-ng.conf`, then replay the requests
your routes were written for: one under `/api`, one static file, one front
controller path, one missing file.
