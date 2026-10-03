# patches/

`main` carries no php-src patch (issues #589, #590, #591, #592). The directory
holds only history notes. `prepare.sh` creates `sapi/fpmng/` and touches nothing
else in the php-src tree, and `third_party/php-src/` holds byte-exact copies of
the pinned tag (`build/vendor-php-src.sh check` verifies the hashes). A PHP
upgrade on `main` is a plain re-import:
`build/vendor-php-src.sh import <php-src checkout at the new tag>`.

Patches against php-src (fibers, async) live only on branch `async`. Do not
add a patch here.

## What was dropped, and where it went

- 0001 (GH-18956, idle/active counting on FastCGI keep-alive connections,
  #591). The upstream bug is still real: php/php-src#18956, fix
  bukka/php-src#2. The patch is in git history
  (`git log --diff-filter=D -- patches/0001-gh18956-fastcgi-keepalive-counting.patch`).
  What the unpatched counters cost (static reading, not measured):
  - pm scaling is not affected: the maintenance loop recounts idle and active
    processes from `request_stage` on every heartbeat
    (`sapi/fpmng/fpm/fpm_process_ctl.c`).
  - Pristine `fastcgi.c` calls `on_read()` before the blocking read on a kept
    connection (`third_party/php-src/main/fastcgi.c`), so a worker that starts
    waiting on a kept connection counts as reading headers. Hence `accepted
    conn` and the per-process `requests` get one extra request for every kept
    connection the client closes (the gateway does this after
    `http.idle_timeout`, 500 ms by default), a waiting worker is shown as
    `Reading headers` with the idle wait in `request duration`, and `max active
    processes` stays too high.
  - `idle processes` and `active processes` in the status page can be wrong for
    up to one heartbeat (about 1 s).
  - `request_terminate_timeout` can hit an idle kept-alive worker only with
    `http.idle_timeout = 0`, a large value, or an external proxy using
    `fastcgi_keep_conn on`.
  - `sapi/fpmng/fpm/fpm_request.c` offers the `void` entry points
    `fpm_request_accepting()` and `fpm_request_reading_headers()` that the
    pristine `fpm_main.c` passes to `fcgi_init_request()`, plus the `_ex(bool)`
    variants the http-direct executors use.
- 0002 (`TCP_NODELAY` on FastCGI keep-alive connections, #590). The same effect
  now comes from `.listening_socket_nodelay = 1` on the `fastcgi` pool type in
  `sapi/fpmng/fpm/fpm_pool_type.c`, because accepted sockets inherit the option
  from the listener on Linux (`sapi/fpmng/tests/fpmng-fastcgi-tcp-nodelay.phpt`;
  macOS/BSD: not verified). The upstream bug is still real:
  `patches/0002-upstream-report.md` is the report to send to php/php-src. The
  patch and reproducer are in git history
  (`git show bd3f332:patches/0002-fastcgi-tcp-nodelay-never-set.patch`,
  `git show bd3f332:patches/0002-fcgi-nodelay-repro.py`).
- 0003 (buffered read, `accept4`), 0004 (`fcgi_set_optimized_transport()`) and
  0005 (`writev` for large responses), #589. Nothing has selected the optimized
  transport since #420 (#376 retired `fastcgi-ng`, #388 retired `pool.type =
  http`). Branch `async` may keep its own copies. The numbers they earned are
  in `docs/NOTES.md` (3t and the `writev` section).
