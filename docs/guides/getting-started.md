# Getting started

From an empty Ubuntu 26.04 machine to one process that serves a PHP
application over HTTP, runs a scheduled script and keeps a worker alive. No
nginx, no cron daemon, no supervisord.

The configurations on this page are not illustrations. CI runs
`build/test-doc-configs.sh`, which feeds every `ini verify` block of this page
to `php-fpm-ng -t`, starts the master on the one marked `verify-run` and checks
the answers shown in [Verify](#5-verify). The package manager and systemd steps
are the only commands CI does not run (they need root and an init system).

## 1. Install

Download the `.deb` of the latest release together with `SHA256SUMS`, check it
and install it. The exact commands, the Alpine variant and the TLS package are
in [`install.md`](../install.md#getting-the-files).

```sh
curl -fLO https://github.com/crazy-goat/php-fpm-ng/releases/latest/download/SHA256SUMS
curl -fLO https://github.com/crazy-goat/php-fpm-ng/releases/latest/download/php-fpm-ng_v0.13.0_php8.5_amd64.deb
sha256sum -c --ignore-missing SHA256SUMS
sudo apt install -y ./php-fpm-ng_v0.13.0_php8.5_amd64.deb
```

The file name carries the release version: use the one of the release you
download. The package depends on the distribution's `libphp8.5-embed`, so PHP
arrives with it. It ships `/etc/php-fpm-ng/php-fpm-ng.conf` (which includes
`pool.d/*.conf`), one idle pool `pool.d/www.conf` and a systemd unit; the unit is
installed but **not started**.

## 2. The application

Two files are enough to see something. `public/` is the document root; the
front controller is the only PHP file reachable from the web.

```php file=/srv/app/public/index.php
<?php
echo "hello from php-fpm-ng\n";
```

```php file=/srv/app/bin/tick.php
<?php
echo "tick at " . date('c') . "\n";
```

```php file=/srv/app/bin/worker.php
<?php
// A consumer: runs until it is stopped. The master starts it again when it exits.
while (true) {
    sleep(1);
}
```

```sh
sudo mkdir -p /srv/app/public /srv/app/bin
# create the three files above, then:
sudo chown -R www-data:www-data /srv/app
```

## 3. The configuration

One file for everything. Put it in `/etc/php-fpm-ng/pool.d/app.conf` and move the
shipped idle pool out of the way (`sudo mv /etc/php-fpm-ng/pool.d/www.conf
/etc/php-fpm-ng/www.conf.disabled`).

```ini verify-run
; The public port: static files and PHP, no web server in front.
[gateway]
pool.type = gateway
user = www-data
group = www-data
listen = 0.0.0.0:8080
chdir = /srv/app/public
http.static = yes
http.front_controller = /index.php
http.route[app] = /
; status and metrics for the pools, on their own listener (this is the default)
operator.status_listen = 127.0.0.1:9253
operator.metrics_listen = 127.0.0.1:9253

; The PHP workers behind it: an ordinary FastCGI pool, as in upstream FPM.
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
pm.max_children = 5
pm.start_servers = 2
pm.min_spare_servers = 1
pm.max_spare_servers = 3

; A scheduled script, in place of a crontab line.
[tick]
pool.type = cron
user = www-data
group = www-data
cron.schedule = * * * * *
cron.script = /srv/app/bin/tick.php
cron.log = /var/log/php-fpm-ng/tick.log

; A long-running script, in place of a supervisord program.
[worker]
pool.type = supervisor
user = www-data
group = www-data
supervisor.script = /srv/app/bin/worker.php
supervisor.processes = 1
supervisor.restart = always
```

What each section is:

- **`gateway`** is the HTTP front. `listen` is the public port, `chdir` its
  document root, `http.route[app] = /` sends every path that is not a static
  file to the pool named `app`. A gateway with no route is refused; a request that
  matches none gets a local 404. See [`gateway.md`](../gateway.md) and
  [`http-route.md`](../http-route.md).
- **`app`** runs PHP. Everything in it is upstream FPM vocabulary (`pm.*`,
  `listen.*`).
- **`tick`** and **`worker`** are the two pool types that replace cron and
  supervisord: [`cron.md`](../cron.md), [`supervisor.md`](../supervisor.md).

## 4. Test and start

`-t` runs the same checks a start runs and exits without starting anything:

```sh
sudo php-fpm-ng -t -y /etc/php-fpm-ng/php-fpm-ng.conf
sudo systemctl start php-fpm-ng
sudo systemctl status php-fpm-ng
```

A mistake is refused by name before anything starts, for example a gateway
without `http.route[...]`. To follow the log while it starts:
`sudo journalctl -u php-fpm-ng -f` (the packaged `error_log` is
`/var/log/php-fpm-ng/error.log`).

## 5. Verify

```sh
curl -i http://127.0.0.1:8080/
curl -s http://127.0.0.1:9253/status
```

The first answers `200` with the body `hello from php-fpm-ng`. The second is the
JSON status page of the gateway, `{"pools":[...]}`; per-pool pages and the
Prometheus `/metrics` are on the same listener, see
[`operator-endpoint.md`](../operator-endpoint.md). The cron pool writes one line per
run to `/var/log/php-fpm-ng/tick.log` after the first full minute, and
`pgrep -af 'pool worker'` shows the supervised process (its process title is `php-fpm: pool worker`).

## Where next

- Coming from an existing setup: [php-fpm + nginx](migrate-php-fpm-nginx.md),
  [NGINX Unit](migrate-nginx-unit.md), [supervisord + cron](migrate-supervisord-cron.md).
- Symfony and Laravel: [`framework-recipes.md`](framework-recipes.md).
- TLS in front of PHP: [`tls.md`](../tls.md) (a beta feature, in the `php-fpm-ng-tls`
  package).
- What is supported and what is beta: "Support tiers" in the
  [README](../../README.md#support-tiers).
