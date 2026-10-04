# `php-fpm-ng serve`: run an application with one command

For the first ten minutes and for local development: no configuration file, no
pool to write. `serve` writes the configuration to a temporary file, starts the usual master
on it and stays in the foreground.

```sh
cd my-app            # has public/index.php, or index.php in the directory itself
php-fpm-ng serve     # http://127.0.0.1:8080
```

Logs and the access log go to standard error, and Ctrl-C stops everything. It is
the same master, the same pools and the same configuration parser as in a
production setup, so what works here behaves the same there.

`serve` is part of the Linux packages. It is not built by the from-source flow
(`build/prepare.sh`).

## Modes

| Command | What runs | When |
|---|---|---|
| `php-fpm-ng serve` | `pool.type = gateway` in front of one `pool.type = fastcgi` pool (`pm = dynamic`) on a private unix socket | The default: the same model as nginx + php-fpm. Several `.php` entry points work (`/admin.php` runs `admin.php`), `fastcgi_finish_request()` works. |
| `php-fpm-ng serve --direct` | one `pool.type = http-direct` pool, classic executor, `pm = static` | Fewer processes. Every path that is not a static file runs the front controller; see [`http-direct.md`](../http-direct.md). |
| `php-fpm-ng serve --worker index.php` | one `pool.type = http-direct` pool, `pool.executor = worker` (implies `--direct`) | A long-running worker script (beta). It serves no static files and writes no access log. |

## Options

| Option | Default |
|---|---|
| `--root <dir>` | `public/` if it exists, else the current directory |
| `--listen <addr>` | `127.0.0.1:8080`; a bare port (`--listen 3000`) means `127.0.0.1:3000` |
| `--front-controller <file>` | `index.php`, relative to the root |
| `--workers <N>` | the number of CPUs (`pm.max_children`, or the size of the `--direct` pool) |
| `--worker <file>` | none; the worker script, relative to the root |
| `--print-config` | print the generated configuration and exit |
| `-n`, `-c <file>`, `-d <x=y>`, `-R` | passed on to the master: PHP ini handling, and running as root |

Options are written after `serve`. `serve` refuses a configuration file (`-y`):
it either builds the configuration or reads one, never both.

The default listener is the loopback interface. Write an address
(`--listen 0.0.0.0:8080`) to expose it; the dev server does no TLS and is not
meant to face a network.

## Leaving the dev server behind

```sh
php-fpm-ng serve --print-config > php-fpm-ng.conf
php-fpm-ng -t -y php-fpm-ng.conf
php-fpm-ng -y php-fpm-ng.conf
```

The printed file is a complete configuration that passes `-t`. It uses a
stand-in socket and pid paths (`/tmp/php-fpm-ng-serve.sock`, `/tmp/php-fpm-ng-serve.pid`); give the `[app]` pool a
`listen` of your own before running it for real, and add `user`/`group` and a
log file as in [Getting started](getting-started.md).

## Reloading after a code change

The master does not watch files, on purpose. PHP code is read again by every
request on the classic executors, so editing a file needs no reload. Only the
`--worker` mode keeps code in memory; reload it with `SIGUSR2` from any file
watcher:

```sh
# serve prints the master pid and the path of its pid file when it starts:
#   php-fpm-ng serve: master pid 12345, pid file /tmp/php-fpm-ng-serve-AbC123/serve.pid
watchexec -w src -- kill -USR2 12345
ls src/*.php | entr -s 'kill -USR2 12345'
```

The pid stays the same across reloads. The pid file is removed when the server
exits.

A reload starts the same generated configuration again.

## What it does not do

- No file watching in the master, no HTTPS (see [`tls.md`](../tls.md), a beta
  feature, for a real certificate setup).
- The operator pages (`/status`, `/metrics`) are switched off, so a second
  `serve` does not collide on `127.0.0.1:9253`; see
  [`operator-endpoint.md`](../operator-endpoint.md) to turn them on in a real
  configuration.
- The environment is cleared for the PHP workers in the default mode, as in a
  production FastCGI pool (`clear_env`).
- A temporary directory under `$TMPDIR` (default `/tmp`) holds the generated
  file, the pid file and the private socket. It is removed when the server exits.
- The listen address is tried first: when it is already in use, `serve` exits
  with status 1 and says so, instead of running without a listener.
- A document root, listen address or front controller that contains `"`, `\`,
  a line break, `${` or `$pool` is refused, because it cannot be written into the file.
