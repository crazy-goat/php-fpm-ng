# Migrating from php-fpm + nginx

Two steps, and you can stop after the first.

1. **Swap the binary, keep nginx.** `pool.type = fastcgi` is classic FPM and
   stays compatible with upstream: your `www.conf` pools (`pm.*`, `listen.*`,
   `php_admin_value[...]`, `request_terminate_timeout`, `slowlog`, `pm.status_path`)
   work unchanged. Install the package ([`getting-started.md`](getting-started.md#1-install)),
   copy your pool files to `/etc/php-fpm-ng/pool.d/`, point nginx's `fastcgi_pass`
   at the same socket, run `php-fpm-ng -t`, start it. The benefit is small on
   its own; it is the precondition for step 2 and for moving cron and workers
   in ([supervisord + cron](migrate-supervisord-cron.md)).
2. **Replace nginx with the gateway** (`pool.type = gateway`), when what nginx
   does for you is: listen, serve static files, send everything else to
   `index.php`, cap request bodies, log accesses, perhaps terminate TLS. Anything
   beyond that list needs a closer look at the table below first.

## Before

```nginx
server {
    listen 80;
    root /srv/app/public;

    client_max_body_size 16m;
    access_log /var/log/nginx/access.log;

    location / {
        try_files $uri /index.php$is_args$args;
    }

    location ~ ^/index\.php(/|$) {
        fastcgi_pass unix:/run/php-fpm-ng/app.sock;
        include fastcgi_params;
        fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;
        internal;
    }
}
```

with the usual FPM pool (`listen = /run/php-fpm-ng/app.sock`, `pm = dynamic`, ...).

## After

```ini verify
; The nginx server block becomes the gateway; the FPM pool stays as it was.
[gateway]
pool.type = gateway
user = www-data
group = www-data
listen = 0.0.0.0:8080
chdir = /srv/app/public           ; nginx: root
http.static = yes                 ; nginx: try_files $uri ...
http.front_controller = /index.php ; nginx: ... /index.php$is_args$args
http.route[app] = /               ; nginx: fastcgi_pass, to the pool named below
http.max_body = 16M               ; nginx: client_max_body_size
http.access_log = /var/log/php-fpm-ng/access.log
ping.path = /ping                 ; answered by the gateway itself
ping.response = pong

[app]
pool.type = fastcgi
user = www-data
group = www-data
listen = /run/php-fpm-ng/app.sock
listen.owner = www-data
listen.group = www-data
listen.mode = 0660
chdir = /srv/app/public
pm = dynamic
pm.max_children = 20
pm.start_servers = 4
pm.min_spare_servers = 2
pm.max_spare_servers = 6
```

`php-fpm-ng -t -y /etc/php-fpm-ng/php-fpm-ng.conf` must say `test is
successful` before you stop nginx. Then stop nginx, start php-fpm-ng, and run the
same requests against the new port. Keep the old configuration until you have.

## Directive map

| nginx | php-fpm-ng | notes |
|---|---|---|
| `listen 80;` | `listen = 0.0.0.0:80` on the gateway | |
| `listen 443 ssl;`, `ssl_certificate`, `ssl_certificate_key` | `listen` + `http.tls_cert`, `http.tls_key` | **beta**, and only in the `php-fpm-ng-tls` package ([`tls.md`](../tls.md)). Certificates are re-read without a restart. If you want a supported TLS setup today, keep nginx for TLS |
| `ssl_protocols` | `http.tls_min_version` | `TLSv1.2` (default) or `TLSv1.3` |
| redirect 80 to 443 | `http.plain_listen` | the plain-HTTP companion port of a TLS gateway, redirect-only ([`acme-challenge.md`](../acme-challenge.md)) |
| `root` | `chdir` | one document root per gateway, shared by every route |
| `try_files $uri /index.php...` | `http.static = yes` + `http.front_controller` | static files are served by the gateway for `GET`/`HEAD`, the rest goes to the front controller |
| `fastcgi_pass` | `http.route[<pool>] = <prefix>[,...]` | longest prefix wins; a request matching no route is a local 404 ([`http-route.md`](../http-route.md)) |
| `client_max_body_size` | `http.max_body` | default `32M` |
| `access_log` | `http.access_log` | |
| `client_header_timeout`, `client_body_timeout`, `keepalive_timeout`, `send_timeout` | `http.read_timeout`, `http.keepalive_timeout`, `http.write_timeout` | milliseconds, not seconds ([`gateway.md`](../gateway.md)) |
| `fastcgi_read_timeout` | `request_terminate_timeout` on the PHP pool | |
| `allow` / `deny` | `http.allowed_clients` | |
| `set_real_ip_from` | `http.trusted_proxies` | when something still sits in front of the gateway |
| `stub_status` | `operator.status_path`, `operator.metrics_path` | JSON and Prometheus, on their own listener ([`operator-endpoint.md`](../operator-endpoint.md)) |
| `location /ping` health check | `ping.path`, `ping.response` | answered in the gateway before routing, so it proves the gateway, not PHP |

## What has no equivalent

Checked against the directive list of this tree; if one of these is something
your nginx does, keep nginx in front of php-fpm-ng (step 1) rather than working
around it:

- virtual hosts: one gateway has one document root, `server_name` does not exist
  (run one gateway per listen address if the sites can have their own address);
- `rewrite`, `return`, `add_header`, response compression, response caching,
  rate limiting (`limit_req`), `proxy_pass` to a non-PHP upstream;
- HTTP/2 and HTTP/3: not in this tree ([`install.md`](../install.md#tls-acme-and-the-debug-clock));

## Verify

`php-fpm-ng -t`, then the same request set against both servers: status codes,
`Content-Type` of a static file, a request body larger than `http.max_body`
(expect `413`), and the static file of a path that does not exist (expect the
application's own 404, because it falls through to the front controller).
