# Reloads (`SIGUSR2`)

Two independent things are called "a reload" here, and this page is about
both:

- **safe reload** — the new configuration is checked before anything is
  stopped, and a configuration that does not load leaves the running pools
  serving what they already have (issue #640). Always on: there is no directive
  that turns it off;
- **`reload.selective`** — an unchanged pool is not restarted at all across
  that reload (issue #330).

## Safe reload: the configuration is checked before anything is stopped (issue #640)

### What happens on `SIGUSR2`

A reload is a full `execvp()` of the master (`fpm_process_ctl.c`,
`fpm_pctl_exec()`): every pool's children are signalled, waited for and
replaced by newly forked ones, and the next generation parses the configuration
from scratch. Until #640 that sequence started with the signals: a
configuration the next generation could not parse took the whole service down
with it — which is exactly what an automated config management (Ansible, a
ConfigMap sync, a `kubectl rollout restart`) does with a half-written file.

Now, before any of that, the master forks a child that asks the question it is
about to be asked:

```
SIGUSR2
  -> the master forks a child
  -> the child execs itself with the same argv plus -t
     (fpm_reload_config_check.c, fpm_reload_config_check.h)
  -> exit status 0: the reload sequence starts exactly as before
  -> anything else: one ERROR line, nothing signalled, the master keeps
     serving the configuration it was started with
```

The check is not a cheaper approximation of the parse. It is the binary's own
configuration test, with the arguments this master was started with — the same
argv `fpm_pctl_exec()` is about to re-exec, so the same `php.ini`, the same
`-c`/`-d`/`-O`/`-F`, the same `-y`. A fresh process is used rather than a fork
that parses in place, so the check cannot touch the running generation: with
`-t`, `fpm_init()` returns from inside `fpm_conf_init_main()` (`fpm.c:81-100`)
and the scoreboard, the metrics region, the signal handlers, the pid file and
the first fork loop are never reached (`fpm.c:102-202`).

Two consequences worth knowing:

- The gate runs in the **event loop**, so the master is blocked for the length
  of one configuration test. Measured on the container the CI test jobs use
  (`ubuntu:26.04`, PHP 8.5.4 SDK, 60 runs of
  `php-fpm-ng -n -t -y fpm.conf -F` on a one-pool `http-direct`
  configuration): **min 7 ms, median 8 ms, p90 9 ms, max 12 ms** wall clock.
  That is against a full master start on the same configuration at ~600 ms,
  so the gate is noise next to the reload it precedes. Not measured: a
  configuration with many pools and TLS certificates, which reads and parses
  every certificate.
- The **emergency restart** is gated too: enough children dying at once makes
  the master re-read the configuration (`fpm_children.c`), and that reload now
  asks the same question. A configuration that does not load is not a better
  answer than the one that is already loaded, so the crash loop keeps running
  on the current configuration and logs the same ERROR.
- A configuration the test refuses **cannot be loaded by a reload at all**. The
  way to load it is to stop the master and start it again, which is what an
  operator does with a configuration that does not parse anyway. There is no
  directive that turns the gate off, deliberately: a "reload anyway" switch is
  a footgun with a name an operator reaches for under pressure.

### The refusal

Measured output of a real master refusing a reload. The broken line is
`pm.max_children = 0` in the single `http-direct` pool of
`/tmp/refusal/fpm.conf`, whose `error_log` is a file:

```
[...] NOTICE: Reloading in progress ...
[...] ALERT: [pool www] pm.max_children must be a positive value
[...] ALERT: [pool www] pm.max_children must be a positive value
[...] ERROR: failed to post process the configuration
[...] ERROR: failed to post process the configuration
[...] ERROR: FPM initialization failed
[...] ERROR: FPM initialization failed
[...] ERROR: reload refused: the configuration test says /tmp/refusal/fpm.conf is not valid
    (exit status 78), so the pools that are running keep serving the configuration they were
    started with; fix the configuration and send SIGUSR2 again
```

The duplicated lines are the child's own — the directive and the pool that is
wrong — and the last line is the master's summary. The **doubling is real and
expected**: until `zlog_set_launched()` runs, `zlog` writes each line to both
its descriptor and `stderr` (`zlog.c:249` and `:254`), and a master's `stderr`
is the `error_log` file (`fpm_stdio_init_final()` →
`fpm_stdio_redirect_stderr_to_error_log()`). A `-t` run by hand prints each line
once, because its `stderr` is a terminal.

The exit status is `EX_CONFIG` (78) for a configuration the parser or a
validator refused; the message quotes it so the two can be correlated.

Nothing else changes: no state was entered, no child was signalled, the pid
file is untouched, and the next `SIGUSR2` tries again from the top — measured
by `fpmng-reload-broken-config.phpt`, which also asserts the worker pid that
answers after the refusal is the one that answered before it.

### When the check cannot run, the reload goes ahead or is refused

Two kinds of failure stop the check from running. Each has its own outcome.

- **The `execvp()` of the check fails.** The binary this master was started
  from is missing, for example during a package upgrade. The reload is
  **refused**, and one ERROR line says so. The pools that run keep serving,
  and the next `SIGUSR2` tries again. Before issue #661 this case logged a
  WARNING and the reload went ahead. The old generation had already drained
  its gateways, so the service went down.
- **A `fork()` or a `pipe()` fails, or the check is killed by a signal.** The
  master logs a **WARNING** and the reload goes ahead, as it did before #640. A
  transient fork failure must not leave the service unable to reload.

The failed `execvp()` in `fpm_pctl_exec()` (issue #690) is now reached only
when the binary is removed between the check and the exec.
`fpmng-reload-selective-failed-exec.phpt` tests the refusal. It checks that
the master, its worker and its listener keep running.

When the exec fails in that window, the master does not exit (issue #925). All
children are gone and the pools are freed by then, so the old master cannot
serve again. It tries `execv("/proc/self/exe")` instead
(`fpm_reload_exec_fallback.c`). The next generation runs the binary of the old
master, with the same arguments and the same configuration. It takes over the
listeners as in a normal reload. The master keeps its pid.
`fpmng-reload-selective-exec-after-check.phpt` tests it: the master keeps its
pid, and the next generation adopts the spared worker.
`fpmng-reload-gateway-exec-after-check.phpt` tests the gateway listener: the
next generation takes it over, and the port keeps answering. If the retry fails too,
or the platform has no `/proc/self/exe`, the master exits as before. The next
master keeps the missing path as `argv[0]`, so the next `SIGUSR2` is refused by
the check until the binary is back.

### What `-t` does catch

Everything `fpm_conf_init_main()` does with `test_conf` set, which is a lot
more than "does it parse". The notable ones, with where they live:

- the file loads: syntax, an unknown directive or entry, `include=` and glob
  expansion, each with the file and line it was found in
  (`fpm_conf.c:2728-2741` for the load, `fpm_conf.c:2199` and `:2290` for the
  two refusals);
- `[global]`: `log_limit`, `process_max`, `process.priority`, and that the
  `error_log` can be opened (`fpm_conf.c:1962-2010`);
- `pool.type` / `pool.executor` resolution, including the retired names and the
  types this binary does not have (`fpm_conf.c:1344-1376`), each pool type's own
  `validate()`, and the directives a type does not support
  (`fpm_conf.c:1398-1429`);
- `pm`, `pm.max_children` and the `dynamic`/`ondemand` sub-checks
  (`fpm_conf.c:1447-1523`), the operator endpoint's own directives
  (`fpm_conf.c:1527-1597`), `ping.path` and `access.format`
  (`fpm_conf.c:1600-1647`);
- `chroot` and `chdir` exist, and inside a `chroot` the `chdir` exists too
  (`fpm_conf.c:1728-1777`);
- two pools in one file may not share a listening address
  (`fpm_conf.c:1862`, `fpm_conf_check_unique_listen()`), one operator listener
  is one process with one identity
  (`fpm_operator_endpoint.c:149`, `fpm_operator_listener_identity_ok()`) and
  one answer per URL
  (`fpm_operator_endpoint.c:256`, `fpm_operator_endpoint_add_route()`);
- a gateway's whole route table: an unknown target pool, a target type the
  gateway cannot speak to, a prefix without a leading `/`, a prefix claimed
  twice (`fpm_http_route.c:269`, `fpm_http_validate_routes()`);
- TLS: `http.tls_cert` and `http.tls_key` are read, parsed and must match,
  `http.tls_client_ca` must parse, and `http.tls_min_version` /
  `http.tls_verify_client` must be known values (`fpm_tls_http.c:351-419`),
  reached for `http-direct` at `fpm_http_direct_request.c:294` and for the
  gateway at `fpm_http_route.c:1727-1734`;
- `user`, `group`, `listen.owner` and `listen.group` name accounts that exist,
  and a Unix listen path is not longer than `sun_path`
  (`third_party/php-src/sapi/fpm/fpm/fpm_unix.c:115` and `:67`).

Side effects to know about, because they are real and useful: a `-t` run
**opens or creates** the `error_log` (`fpm_conf.c:2008`), every pool's
`access.log` (`third_party/php-src/sapi/fpm/fpm/fpm_log.c:41`) and any
`slowlog` (`fpm_conf.c:1683`). Running it is therefore a way to find out that a
log directory is not writable — and a way to leave empty files behind.

### What `-t` does not catch

Verified against this tree, not assumed:

1. **Anything that needs a `bind()` or a `fork()`.** `-t` returns from inside
   `fpm_conf_init_main()`, so `fpm_sockets_init_main()`
   (`third_party/php-src/sapi/fpm/fpm/fpm_sockets.c:438`), every pool type's
   `init_main` (`fpm.c:162-185`) and `fpm_children_create_initial()`
   (`fpm.c:188-202`) never run. A `listen` address that is already taken is
   therefore invisible — by another process, by a second master on the same
   host, or by a gateway's default operator listener `127.0.0.1:9253`
   (issue #561). This is not an oversight that could be fixed by binding in the
   check: at reload time **the running generation still holds every one of its
   addresses**, so a check that bound anything would fail every reload on the
   host. A reload can still fail on a bind collision, which is what the #690
   discard path is for. A gateway listener that the next generation takes over
   is not bound again, so a bind collision cannot happen there (see "Gateway
   listeners across the reload" below).
2. **A `pid` path that cannot be written.** `fpm_conf_write_pid()` is called
   after the point `-t` returns from (`fpm.c:102`).
3. **A script or document root that is not there yet.** `chroot`/`chdir` must
   exist, but `http.front_controller`, `supervisor.script` and `cron.script` on
   disk are deliberately not required — a path may legitimately appear before
   the first run (`fpm_pool_cron.c:331-334` says so). A gateway with neither
   `chdir` nor `http.front_controller` passes `-t` **silently**: the WARNING
   that names the problem (`fpm_http_route.c:1034`) is written by
   `fpm_http_init_pool_ex()`, which `-t` never reaches.
4. **Anything PHP resolves later**: `extension=`, `extension_dir`,
   `open_basedir`, the contents of a `.user.ini`, an opcache file. `-t` copies
   these strings into the ini; it does not load them.
5. **That the file is still the same when the master execs.** The check and the
   `execvp()` are two separate reads of the configuration: a second edit (or a
   ConfigMap sync) between them is loaded without being checked. Closing that
   window would mean reading the configuration once and handing the result to
   the next master instead of letting it re-read the file — a different design,
   and out of scope here. The window is **documented here, not closed**.
6. **Whether the generation that is being replaced can be replaced at all.**
   The gate says the configuration loads. After that the old master still stops
   every child and re-execs. An OOM at that moment, a `SIGTERM` between the
   check and the exec still ends the generation. An `execvp()` that fails is
   retried through `/proc/self/exe` (issue #925). Only when that fails too, the
   master exits, and the gateway listeners it handed on close with it.

So the gate's failure mode is "the service keeps running the configuration it
already has", never "the service is down because of the check" — except through
the last two items above, which are the reload's own pre-existing risks. The
listener handoff of issue #661 does not change them.

### Gateway listeners across the reload (issue #661)

Before issue #661, a reload closed each gateway listener before the
`execvp()`, and the new master bound it again. A client that connected in
that gap got `ECONNREFUSED`. Now the old master keeps the TCP listening socket
open across the `execvp()`. The socket stays in the `LISTEN` state, so a
connection made during the reload waits in the kernel accept queue. The new
master accepts it.

The handoff runs in these steps:

1. The old master drains its gateways, as before.
2. The old master clears `FD_CLOEXEC` on the listening socket of each TCP
   gateway listener that does not use `http.reuseport` and is in the `LISTEN`
   state. A gateway listener in the NO_CERT state of `http.tls_wait_for_cert`
   is bound but not listening. It is not handed over, and it closes on the
   `execvp()`. The old master writes one record per handed socket to
   `FPMNG_HTTP_LISTENERS`. A record is `<fd>:<tls fingerprint>:<bind text>`,
   and the records are comma-separated.
3. The new master reads `FPMNG_HTTP_LISTENERS` and removes it, after the
   configuration is read and before any pool binds its socket. A socket that no
   gateway of the new configuration uses is closed at this point, so it cannot
   block the bind of a fastcgi pool on the same address.
4. A gateway of the new generation takes a socket over when its bind text and
   its TLS fingerprint match. It takes a socket over only when it listens at
   once. A gateway in the NO_CERT state of `http.tls_wait_for_cert` does not,
   because it does not listen at once.
5. The new master closes each inherited socket that no gateway takes over.
   The gateway binds its address as before. The new master does not close two
   kinds of socket. It leaves them open:
   - A record whose descriptor is not a listening TCP socket. The record is
     ignored.
   - A record whose descriptor is a listening socket that is not bound to the
     address in the record. The record is ignored. The new master logs a
     WARNING (see the list below).

The bind text must match as written. `0.0.0.0:8080` and `*:8080` are different
texts, so the gateway binds a new socket. The TLS fingerprint is a hash of the
values of `http.tls_cert`, `http.tls_key`, `http.tls_min_version`,
`http.tls_sni_cert`, `http.tls_verify_client`, `http.tls_client_ca` and
`http.tls_wait_for_cert`. It uses the configuration values, not the file
contents. A gateway that moves to another address does not take the old socket
over. The old socket closes, and the old address is free after the reload.

The new master logs these lines:

- NOTICE `[pool NAME] http: took over the listener on ADDR from the previous generation`, when a gateway takes a socket over.
- NOTICE `http: closing the listener on ADDR of the previous generation: no gateway uses that address now`, when the new configuration has no gateway on that address.
- WARNING `http: closing the listener on ADDR of the previous generation: no gateway took it over`, when a socket is still not taken over after start-up.
- WARNING `http: not taking over descriptor FD for ADDR of the previous generation: it is not bound to that address`, when a record names a listening socket that is not bound to the address in the record. The new master leaves that socket open. `FD` and `ADDR` are placeholders for the descriptor number and the bind text.

Not covered by the handoff:

- `http.reuseport = on`. Each gateway child binds its own socket, and the
  master socket closes before the exec, as before. A client can still get
  `ECONNREFUSED` during a reload with reuseport.
- A unix socket as the public listener. It is not passed on.
- An `execvp()` that fails after the check passed, when `/proc/self/exe` fails
  too. See item 6 in "What `-t` does not catch". The first failure is retried
  and the listeners are handed on (issue #925).
- Overlapping generations, where two masters hold the same socket at once. This
  case is out of scope.

Measured on the test box, with four clients that connect in a loop with
`Connection: close`, and a reload at 10 s of a 20 s run: the reload refused
140 connections in five runs without this change, and none in five runs with
it. Resets and `502` responses are not zero yet. The numbers and the cause are
in `docs/NOTES.md`, section 3ak.

Under load, a reload still resets some in-flight requests, and some requests
get `502`. The pool workers stop when the reload starts. The gateways still
have requests in flight, and they wait `process_control_timeout` before they
close those connections. Issue #661 does not change this order. A change of
the order is a separate decision, and it is open.

### Reloading from a supervisor

Both packaged units run the test themselves, so an operator gets the reason on
the spot instead of a silent no-op:

- systemd — `/lib/systemd/system/php-fpm-ng.service`
  (`packaging/deb/php-fpm-ng.service`), one `ExecReload=` line, the
  configuration test, and `ReloadSignal=SIGUSR2`. systemd sends the signal only
  after the `ExecReload=` line exits with status 0:

  ```
  ExecReload=/usr/sbin/php-fpm-ng --nodaemonize --fpm-config /etc/php-fpm-ng/php-fpm-ng.conf -t
  ReloadSignal=SIGUSR2
  ```

  Measured on the container CI uses, with that exact command and `stderr` on a
  file instead of a tty (which is what a unit gives it): exit status **78** and
  the diagnostics on `stderr`, i.e. on the journal — plus the same lines in the
  `error_log` the configuration names, because a non-tty `stderr` makes `-t`
  use the file too. The `systemctl reload` behaviour is measured in
  `docs/systemd.md` (Reload).

- OpenRC — `/etc/init.d/php-fpm-ng` (`packaging/apk/php-fpm-ng.initd`), the
  same command before `supervise-daemon --signal USR2`, and `reload` returns
  non-zero when the test fails.

- Docker, and anything else without a unit file: the master's own gate already
  refuses the reload, so `docker kill --signal USR2 <container>` is safe. Run
  the test first if you want the reason where your deploy log can see it:

  ```
  docker exec <container> php-fpm-ng -t --fpm-config /etc/php-fpm-ng/php-fpm-ng.conf
  docker kill --signal USR2 <container>
  ```

With the packaged systemd unit, the master also reports the reload to systemd.
It sends `RELOADING=1` when a reload passes the configuration check. It sends
`READY=1` when the pools of the new generation listen again. A refused reload
sends `RELOADING=1` and then `READY=1`, because the master keeps its current
generation. The `systemctl reload` command returns when the `READY=1` arrives.
A script that runs `systemctl reload` then has the new generation listening, or
the old one still serving after a refusal. See `docs/systemd.md`.

## `reload.selective` — restart only the pools that changed (issue #330)

### The problem this solves

A `SIGUSR2` reload in php-fpm (upstream and fpmng alike) is a full
`execvp()` of the master: **every** child in **every** pool is signalled to
stop, and the new generation starts from zero (`fpm_process_ctl.c`,
`fpm_pctl_kill_all()` / `fpm_pctl_exec()`). That is true even if the config
edit touched exactly one pool out of fifty — a one-line change to `[pool-b]`
still stops `[pool-a]` through `[pool-z]` and restarts them, with whatever
gap `process_control_timeout` and each pool type's own stop grace allow. For
`supervisor`/`cron` pools running long scripts, or request-serving pools
mid-request, that is a blackout across the whole deployment for a change
that concerns one part of it.

`reload.selective` narrows the blast radius: pools whose config did not
change are left running, untouched, across the reload.

### The directive

```
[global]
reload.selective = yes
```

- **`[global]`-only**, boolean, **default `no`**.
- Default `no` preserves today's behaviour exactly: every pool restarts on
  every reload, changed or not. This is intentionally the zero-risk default
  — turning the feature off must be indistinguishable from a build that
  never had it.
- Registered in `ini_fpm_global_options[]`, `sapi/fpmng/fpm/fpm_conf.c`;
  backing field `fpm_global_config.reload_selective`,
  `sapi/fpmng/fpm/fpm_conf.h`.

### What "changed" means

A pool is **unchanged** only if its entire `[section]` body — every line
between its header and the next section header (or end of file), across
however many files `include=` pulled it from — is byte-for-byte identical
between the previous reload's config and the new one. Anything else (a
directive value edited, a directive added or removed, even a comment or
blank line touched) counts as **changed**. A pool present before and absent
now is **removed**; a pool absent before and present now is **new**.

This is a whole-section, text-level comparison, not a parsed-struct
comparison — see "Why text, not structs" below. The practical corollary:
formatting-only edits (reordering directives, adding a comment, fixing
indentation) count as a change and still trigger a restart for that pool.
That is a deliberate conservative bias, not a currently-planned refinement
(see `findings.md`).

**Changed, new, and removed pools need no special-case code**: the
existing reload path already does exactly the right thing for them (full
stop-and-restart, or cold start, or graceful stop). The entire feature is
additive code that recognizes and skips **unchanged** pools; everything
else was already correct.

### Why text, not structs

The natural-sounding design is "parse both configs, compare the resulting
`fpm_worker_pool_config_s` field by field." Two things rule that out here:

1. `fpm_conf.c`'s parser mutates the single live `fpm_worker_all_pools`
   list; it has no second, scratch pool list to parse a candidate config
   into without disturbing the pools that are actively serving traffic.
   Giving it one would be a large, risky change to an existing, heavily
   loaded file for this issue's time budget.
2. `fpm_worker_pool_config_s` is large (100+ fields across the base config
   plus the cron/supervisor/http/http-direct/tls/acme extensions) and
   several fields are pointers (`zend_string *`, `char *`) where a naive
   `memcmp()` of the struct compares addresses, not content — silently
   wrong in exactly the cases that matter.

Instead, `fpm_conf_diff.c` (`sapi/fpmng/fpm/fpm_conf_diff.c`) reads the
config file(s) directly — independent of the live parser, following the
same `include=`/glob expansion `fpm_conf.c` uses — and extracts each
`[section]`'s raw text body. Two byte-identical text bodies are guaranteed
to parse into identical structs (the parser is a pure function of that
text), so text equality is a safe, provably-sufficient proxy for struct
equality, without touching a single pointer field. The trade-off is the
formatting-sensitivity above.

The comparison snapshots "what was loaded at the START of this generation"
(`fpm_conf_diff_snapshot_current()`, called at the end of
`fpm_conf_init_main()`) and diffs it against a fresh read of the config
file(s) at the moment a reload begins
(`fpm_conf_diff_begin_reload_pass()`, called from `fpm_pctl_kill_all()`).
Every comparison uses `memcmp()` too — but only ever on the raw section
text buffers themselves, never on a struct.

### How an unchanged pool survives

`fpm_pctl_kill_all()` (`sapi/fpmng/fpm/fpm_process_ctl.c`) runs once per
reload's first signal pass. When `reload.selective = yes` and a pool is
found unchanged, it is skipped entirely instead of being sent a stop
signal: `fpm_reload_selective_spare_pool()`
(`sapi/fpmng/fpm/fpm_reload_selective.c`) detaches every one of that pool's
running children from the master's bookkeeping (without signalling them —
`fpm_children_detach_oldest()`, already used by issue #329) and records
their pids in an environment variable
(`FPMNG_SELECTIVE_RELOAD_SURVIVORS`) that, unlike shared memory, survives
the master's `execvp()`.

The new generation's `fpm_children_create_initial()` calls
`fpm_reload_selective_adopt()` before its normal fork loop runs. For each
surviving pid still alive (`kill(pid, 0)`), it builds a real
`fpm_child_s`, gives it a scoreboard slot, and links it into
`wp->children`/`wp->running_children` exactly as if it had just been
forked (`fpm_children_adopt()`, `sapi/fpmng/fpm/fpm_children.c`). Because
the fork loop tops up `running_children` to `pm.max_children`/
`supervisor.processes`/etc., a pool that adopts its full previous
complement forks zero new children — the same worker process keeps running,
same pid, across the reload.

### Shared memory of a spared pool (issue #537)

The scoreboard (the status page) and the application-metrics region are
shared memory, and a spared worker keeps the mapping it inherited from the
old master. Anonymous memory does not survive `execvp()`, so with
`reload.selective = yes` the master backs both with a close-on-exec `memfd`
(`sapi/fpmng/fpm/fpm_reload_shm.c`, Linux only; without the directive, or
without `memfd_create()`, they stay anonymous and nothing changes). Sparing a
pool clears close-on-exec on its scoreboard fd and on the metrics fd and
records the fd numbers, the sizes and the pool's metrics slot range in
`FPMNG_SELECTIVE_RELOAD_SHM`; the new master maps the same pages again. So
the spared workers and the new master's operator endpoint see one region: a
counter continues from its pre-reload value and the status page keeps
counting the worker.

Details worth knowing:

- Adoption gives the worker the scoreboard slot it already writes to (the one
  whose `pid` matches), not the first free one.
- A spared pool keeps its metrics slot range even if an earlier pool's
  `pm.max_children` changed; pools that were not spared, or are new, are
  placed in the remaining gaps and start from zero. The slot ranges are kept
  in a per-generation table in `fpm_metrics.c`, not recomputed from pool
  order. The slots a replaced pool used in the previous generation are kept
  free of new ranges, because a #329 survivor of that pool may still write to
  its old slot. The reservation lasts until the survivor can no longer write.
  The expiry is set at the exec of the reload that spares the survivor. It adds
  30 seconds, the `supervisor.stop_timeout` of the pool, and 3 seconds of slack.
  A later reload carries that expiry as a `Y` record and does not extend it (see
  "Back-to-back selective reloads" below). The new pool's slots are cleared by
  punching a hole in the memfd, which frees the memory instead of faulting it in.
- If `fpmng_metrics.series_limit` changed, the slot tables differ in size,
  the old region cannot be reused, and the spared pools' application series
  restart from zero (a warning is logged). The scoreboard is not affected.
- A forked worker keeps only the mappings: it closes the memfd descriptors
  right after fork (close-on-exec is not close-on-fork), so a script running
  in the worker cannot reach them through `/proc/self/fd` (issue #691).

### A new master that fails before adopting (issue #690)

The spared workers are in no master's bookkeeping between the old master's
`execvp()` and `fpm_reload_selective_adopt()`. If the master gives up in that
window, nobody would ever stop them: they would keep the spared pool's
listening socket open. The causes are an invalid directive in a changed pool,
a listening address it cannot bind, a failure while starting another pool, a
`SIGTERM` or `SIGQUIT` that ends a reload between sparing and `execvp()`, and
an `execvp()` that fails (for example the binary was removed). Since #640 the
first of those no longer reaches a new master — the reload is refused before
any pool is spared — and it is listed here because the check that refuses it
is itself only one `-t` away from failing on something it cannot see. What is
left is exactly the second and third: the failures that need a `bind()` or a
fork, which `-t` never does.

So `fpm_reload_selective_discard_unadopted()`
(`sapi/fpmng/fpm/fpm_reload_selective.c`) runs on these paths:
`fpm_init()` failing, `fpm_pctl_exit()`, a failed `execvp()` in
`fpm_pctl_exec()` whose retry through `/proc/self/exe` fails too (issue #925;
`fpmng-reload-selective-exec-fallback-fails.phpt` forces that with a binary
that has lost its execute bit), and once after the initial fork loop in
`fpm_run()`. The last
call removes entries that no pool of the new generation adopted (a pool that
disappeared from the config), so a stale pid never stays in the environment.
It sends `SIGTERM` to each pid still listed in
`FPMNG_SELECTIVE_RELOAD_SURVIVORS` and logs a WARNING (`issue #690`). It uses
`SIGTERM`, not the graceful `SIGQUIT`, even on a graceful stop: the pool type is
unknown there, and a request in flight on a spared worker is cut. Adoption
removes a pool's entry as it takes it, so after a successful start nothing is
left to discard.

The call in `fpm_pctl_exit()` also runs when `type->init_main()` of a pool
fails after `fpm_init()` returned OK. No configuration reaches that failure,
because every cause is a resource failure. `fpmng-reload-selective-failed-init-main.phpt`
makes it fail with the failure-injection seam (`FPMNG_FAULT_INJECT=1`, see
[`fpmng-phpt.md`](fpmng-phpt.md#failure-injection)). It checks the WARNING, the
spared worker and the port. The test skips on a binary built without the seam.

These tests also cover the #690 discard:

- `fpmng-reload-selective-failed-init.phpt`: a new master that cannot bind (a
  failure `-t` cannot see).
- `fpmng-reload-selective-exec-fallback-fails.phpt`: a failed `execvp()` whose
  retry through `/proc/self/exe` fails too.

The second test also checks that the guard of issue #829 logs nothing. When both
exec attempts fail, the old master releases the guard before it exits.

A new master that is killed outright (`SIGKILL`, a crash) cannot run this code.
The guard of issue #829 covers its selective survivors. Its description follows
below. Workers do not poll their parent pid.

**A new master that is killed before it adopts the spared workers (issue #829).**
A guard process covers this case for the selective survivors
(`FPMNG_SELECTIVE_RELOAD_SURVIVORS`). The old master starts the guard before its
`execvp()`. The guard holds the read end of a pipe. The write end crosses the
`execvp()` and the daemonize fork.

Each child that the new master forks closes its copy of the write end. This
includes a gateway process. A gateway that kept the copy would keep the guard
waiting after its master died.

The new master writes one byte and closes the write end after its initial fork
loop. This is the release. On a normal reload, the guard reads the byte and
exits. When both exec attempts fail, the old master writes the same byte before
it exits.

If every holder of the write end dies first, the guard reads end of file. The
guard has its own copy of the carried list. The copy is made before the
`execvp()`. The guard sends `SIGTERM` to every carried worker that still runs,
adopted or not. Before each signal, it checks the start time of the pid. It logs
one WARNING that starts with `issue #829`.

The guard does not cover these cases:

- A new master that hangs and does not exit.
- A master that is killed after the release. Its adopted workers stay orphaned,
  as before.
- The #329 survivors (see the next section).

The design, with the rejected alternatives, is in
`sapi/fpmng/fpm/fpm_reload_selective_guard.h`. The tests
`fpmng-reload-selective-guard-sigkill.phpt` (`-F`) and
`fpmng-reload-selective-guard-sigkill-daemonize.phpt` (`daemonize = yes`) cover
this case. They need the failure-injection seam and skip without it.

### Relationship to issue #329's rolling restart

Issue #329 (`fpm_pool_supervisor.c`, `FPMNG_RELOAD_SURVIVORS`) is a
different mechanism for a different case: a **supervisor pool that IS
restarting** spares exactly one child as a temporary overlap so the
supervised script is never fully down, and kills that spare once the new
generation's own fresh fork has started. It only ever applies to
`supervisor` pools and only ever hands over one pid.

Issue #330's mechanism only applies to pools that are **not restarting at
all**, for every pool type (not supervisor-specific), and hands over the
pool's entire running complement as a permanent adoption, not a temporary
bridge. The two use distinct environment variables with distinct
delimiter schemes specifically so that a config with both an unchanged
pool and a separately-changed `supervisor` pool in the same reload cannot
have one mechanism misparse the other's env var contents.

**A new master that fails before a pool takes a #329 survivor (issue #831).**
Until a pool takes the survivor, `FPMNG_RELOAD_SURVIVORS` lists it, and no
master tracks it. If the new master gives up before that, the survivor has no
bound. `fpm_pool_supervisor_reload_survivors_discard_unadopted()`
(`sapi/fpmng/fpm/fpm_pool_supervisor.c`) sends `SIGTERM` to each listed
survivor. It runs on these paths: `fpm_init()` failing, `fpm_pctl_exit()`, and a
failed `execvp()` whose retry through `/proc/self/exe` fails too (issue #925).
The survivor's own watchdog then ends it after the pool's
`supervisor.stop_timeout`.

The call does not run on the success path. There, `fpm_pool_supervisor_reload_orphans_arm()`
arms the entries that no pool takes, at their deadline (issue #889). The `-t`
child of the configuration check inherits the variable. Its `fpm_init()` skips
the call (`!test_conf`), so the check does not signal the survivors of the
running master.

A normal stop has a side effect. `fpm_pctl_exit()` also sends `SIGTERM` to a
survivor of a removed pool (issue #889) that waits for its deadline. Before
issue #831, that survivor ran until the deadline.

The window of issue #690 remains open for a #329 survivor. A `SIGTERM` or
`SIGQUIT` to the old master between the spare and the `execvp()` leaves the
spared copy without a master, because the carry hook writes the variable only
at the `execvp()`. A new master that is killed outright (`SIGKILL` or a crash)
cannot run this code. The guard of issue #829 covers only the selective
survivors (`FPMNG_SELECTIVE_RELOAD_SURVIVORS`), not these.

### Back-to-back selective reloads (issue #692)

This section covers a #329 survivor that still runs when a second selective
reload starts. It states why no two live writers can share a metrics slot
across back-to-back reloads. It also lists the limits that remain. The proof
covers the survivors of pools that the new configuration keeps. The survivors
of pools that the new configuration removes are covered by a separate paragraph
(issue #889). Limit 2 below lists what remains open for them.

**The deadline starts at the exec.** A master writes the deadline of a spared
child at its `execvp()`, not at the spare. The entry is `pool-name:pid:deadline`,
written to `FPMNG_RELOAD_SURVIVORS`. The deadline is the exec time plus
`FPM_SUPERVISOR_RELOAD_SURVIVOR_TIMEOUT_S`, which is 30 seconds. The old master
waits for its other children after the spare, and this wait can take longer
than 30 seconds. A deadline set at the spare could expire before the new
generation starts. The new master tracks the survivor. It sends the stop signal
at least once: at the deadline, or earlier when a replacement copy starts.

**The survivor exits within a bounded time.** When the survivor gets its first
stop signal, it starts a watchdog process. A later stop signal does not start a
second watchdog. The watchdog sends `SIGKILL` after the
`supervisor.stop_timeout` of the pool, if the survivor still runs. The survivor
therefore exits no later than the deadline, plus one poll of 200 milliseconds,
plus `supervisor.stop_timeout`.

If `fork()` fails, no watchdog starts. The master then sends `SIGKILL` itself,
`supervisor.stop_timeout` plus one second after the stop signal, if the survivor
still runs. This backstop logs one WARNING (issue #890). In this case the
survivor exits no later than the deadline, plus one poll of 200 milliseconds,
plus `supervisor.stop_timeout`, plus one second.

**A survivor of a removed pool gets SIGTERM at its deadline (issue #889).** The
new master tracks each survivor that no pool of the new configuration claims. It
sends `SIGTERM` at the deadline, not the `supervisor.stop_signal` of the pool,
because the pool is gone from the configuration. If the survivor's own watchdog
starts, the survivor exits no later than the deadline, plus one poll of 200
milliseconds, plus `supervisor.stop_timeout`. There is no master backstop for
this case. Limit 2 describes the cases that remain open.

**A master carries its survivor across a reload.** A master tracks at most one
survivor for each pool. It does not spare another child of that pool while it
tracks one. At its `execvp()`, the master writes each survivor that it tracks
back to `FPMNG_RELOAD_SURVIVORS`, with the same deadline. It also writes each
child that it spares in this reload, with the deadline set at this exec. The
`FPM_CLEANUP_PARENT_EXEC` cleanup `fpm_pool_supervisor_reload_survivor_carry()`
does this. The next master tracks the survivor again and retires it at the same
deadline. Before issue #692, the second reload neither signalled the survivor
nor wrote it back, so no master tracked the survivor.

**A carried pid is checked before it is used (issue #891).** A pid in
`FPMNG_RELOAD_SURVIVORS` or `FPMNG_SELECTIVE_RELOAD_SURVIVORS` can be stale. The
old master can reap a spared child. The kernel can then give the same pid to an
unrelated process. `kill(pid, 0)` cannot tell the two apart.

Each carried entry also holds the start time of its process. On Linux, the
master reads this time from field 22 of `/proc/<pid>/stat`
(`fpm_pid_start_time()` in `fpm_pid_child.c`). The master uses the pid only when
the start time still matches (`fpm_pid_is_same()`).

The check does not use parentage. Under `daemonize = yes`, the exec'd image
forks again (`third_party/php-src/sapi/fpm/fpm/fpm_unix.c:618`), so the new
master is not the parent of the carried children.

The check covers the supervisor survivor at the exec and in `init_main()`, its
retirement signal, the selective survivor list when it is adopted, and that list
when it is signalled after a failed handoff. A refused pid is not signalled.
Refusals at the exec and at adoption log one NOTICE each, with the pool name and
the pid. The failed-handoff list logs no line per pid.

Without `/proc`, no start time is recorded, and the start time is 0. The check
then falls back to `kill(pid, 0)`, as before #891. An entry that an older binary
wrote has no start time and takes the same fallback. Not measured on macOS.

Two gaps remain. The kernel can reuse the pid between the check and the signal.
Under `daemonize = yes`, no master sees the exit of a survivor that is not its
child, so the check narrows the window but does not close it. Not measured.

A worker that a selective reload carried over is in the same position under
`daemonize = yes`: the new master is not its parent and gets no `SIGCHLD`.
The master checks each adopted worker that is not its own child once per second
(`fpm_adopted_sweep.c`, issue #792). A worker that is gone, is a zombie that
its new parent has not reaped, or has another start time, is retired as after a
normal exit: its slot is freed and the pool
forks a replacement. In a stop, this lets `running_children` reach 0, so the
master exits. A stop can take up to one second longer than with a worker that
the master forked. The carried supervisor survivor (#329) is not covered; it
has its own path (issue #981).

**A slot reservation has an expiry.** At its `execvp()`, the old master writes
one `X:<pool>:<base>:<count>:<until>` record for the slot range of each pool it
runs. This includes a changed pool that lost a child to a #329 survivor. The
`<until>` is the exec time plus 30 seconds, plus the `supervisor.stop_timeout`
of that pool, plus 2 seconds of slack (`fpm_pool_supervisor_slot_reserve_until()`).
The new master reserves the range of each pool that the reload does not spare
(`fpm_metrics_reserve()`). A reserved range stays reserved for the whole
generation. At the next reload, the old master writes each reserved range as a
`Y:<base>:<count>:<until>` record, if its expiry is later than the current time.
A `Y` record keeps its `<until>`. The new master does not extend it. An `X`
record without `<until>` comes from an older binary. The new master then uses
the load time plus 30 seconds.

**Why no two live writers share a slot.** Consider a survivor `S` of pool `p`,
which writes to the slot range `R`. The master that spares `S` execs at time
`E`, and the new generation replaces `p`. Before `E`, the old master still owns
`R`, and no new pool uses it. So the wait before `E` does not change the bound.

1. `S` has the deadline `D = E + 30`, where `E` is the whole-second exec time.
   It gets the stop signal no later than `D + 0.2`. Its watchdog sends `SIGKILL`
   no later than `D + 0.2 + stop_timeout`. Without a watchdog, the master
   backstop sends it no later than `D + 0.2 + stop_timeout + 1`. So `S` exits no
   later than `E + 31.2 + stop_timeout`.
2. The same exec writes the expiry `U = E' + 30 + stop_timeout + 3` for `R`,
   where `E'` is the whole-second time of the expiry read. The two reads at the
   exec differ by at most one second, so `E' >= E - 1` and
   `U >= E + 32 + stop_timeout`. `U` is therefore at least 0.8 seconds later
   than the exit time of `S`. The 3 seconds of slack cover the one-second
   difference between the two reads. The 0.8 seconds cover the signal delivery
   and the exit.
3. The new master reserves `R` for its generation. `R` belongs to no pool in this
   generation. At the exec of a later reload, the old master writes `R` as a `Y`
   record with the same `<until>`, if `<until>` is later than the current time.
   This applies whether the later reload keeps `p` or replaces it. If the later
   reload keeps `p`, `p` keeps the range it has in the running generation. So `R`
   stays reserved at least until `U`, and no later master extends `U`.
4. No other pool takes a slot of `R` before `U`. At `U`, `S` has already exited.

Without `reload.selective = yes`, the metrics region is not inherited. A
survivor then writes only to the old region, and no new pool can share it.

**Remaining limits.**

1. The master backstop (issue #890) ends a survivor whose watchdog did not
   start. Two cases remain open. First, a reload that starts between the stop
   signal and the backstop drops the timer. The next master retires the survivor
   again and arms a new backstop, so the kill can come later than the bound.
   Second, a master that exits before the backstop fires leaves the survivor
   with no bound. In both cases the slot can be taken after the expiry while the
   survivor still writes there.
2. A survivor of a removed pool has no master backstop. If its watchdog cannot
   start, for example because `fork()` fails, no process sends `SIGKILL`, and the
   survivor has no bound. A master that is killed (`SIGKILL` or a crash) before the
   deadline also leaves the survivor with no bound. A master that stops or fails
   through its own exit path sends `SIGTERM` instead (issue #831). A reload before
   the deadline does not lose the
   survivor: the next master arms it again with the same deadline. The new master
   cannot add a backstop with the right delay, because it does not know the
   `supervisor.stop_timeout` of the removed pool. The slot range stays reserved
   until the expiry, as for a kept pool. The expiry comes after the exit of the
   survivor only when its watchdog runs. See `docs/supervisor.md`.
3. The deadline and the expiry use the wall clock (`time(NULL)`) in whole
   seconds. A forward clock step after the stop signal can move the expiry before
   the survivor exits. A step of more than 1.8 seconds breaks the bound with a
   watchdog. A step of more than 0.8 seconds breaks it when the backstop ends the
   survivor. A step before the signal does not break the bound, because the
   expiry is at least 2 seconds plus `supervisor.stop_timeout` after the deadline
   on the same clock.
4. In a test build with `FPMNG_DEBUG_CLOCK_RATE` above 1, each process uses its
   own debug clock. The carried times are then approximate. The packages do not
   allow this clock.
5. The bound assumes one `supervisor.stop_timeout` value for the survivor and
   for its expiry. A change of this value between two reloads is not tested.

The test `fpmng-reload-survivor-back-to-back.phpt` runs two selective reloads
with a survivor that writes into the slot of its pool. A pool added by the
second reload must not show the survivor's counter. The carry check depends on
the start jitter. If a copy of the new generation starts first, the survivor is
retired before reload B. Then the test checks only the slot.

The test `fpmng-reload-survivor-watchdog-fork-fails.phpt` makes the survivor's
watchdog `fork()` fail with `FPMNG_FAULT=pool_watchdog_fork`. It checks that the
master backstop SIGKILLs the survivor and logs one WARNING. The log stamps have
whole seconds, so the test checks that the WARNING comes 3 or 4 seconds after
the retirement line. The test sets `supervisor.stop_timeout` to 2, so the gap is
`stop_timeout + 1` or `stop_timeout + 2`.

The test `fpmng-reload-survivor-removed-pool.phpt` removes pool `[s]` with a
reload. It checks that the new master logs the survivor, that the survivor still
runs 5 seconds after the reload, and that the master sends `SIGTERM` about 30
seconds after the reload. The log stamps have whole seconds, so the test checks
that the gap between the two log lines is 29 to 31 seconds. The test then checks
that the survivor ends after its watchdog.

The test `fpmng-reload-survivor-init-fails.phpt` runs a supervisor pool with two
copies. The reload moves an http-direct pool to an address that the test holds,
so the new master fails in `fpm_sockets_init_main()`, before any pool takes the
survivor. The test checks that the spared copy ends after the master exits, and
that the master logs a WARNING that names issue #831.

### Accepted degradations of an adopted child

An adopted child is the same OS process as before, but the new master's
bookkeeping for it is necessarily approximate in two ways:

- **No log forwarding**: `fd_stdout`/`fd_stderr` are set to `-1` (the same
  convention issue #329 already established for its one spared child) —
  the master never had these fds for a process it did not fork itself.
- **Approximate start time**: `started` is set to "now" (the moment of
  adoption), not the child's true original fork time. This only ever makes
  the child look *younger* than it really is to
  `pm.process_idle_timeout`/slowlog/`supervisor.max_runtime`-style age
  checks — never older — which is the safe direction (it under-counts
  time-to-live pressure rather than over-counting it).

Both are documented, deliberate scope boundaries, not open bugs — see
`findings.md` for what is explicitly left for later.
