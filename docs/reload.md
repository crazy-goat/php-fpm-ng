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

### When the check cannot be run, the reload goes ahead

Only a check that **ran and refused** cancels the reload. A `fork()` that
failed, an `execvp()` that failed (the binary this master was started from was
removed — a package upgrade in progress, say), or a check killed by a signal
logs a **WARNING** and lets the reload proceed exactly as it did before #640.
Refusing there would mean that a transient fork/exec failure leaves an operator
with a service that can never reload again, and it would make the failed-`execvp()`
path of `fpm_pctl_exec()` unreachable, which is the path issue #690's
discard-the-spared-workers logic exists for
(`fpmng-reload-selective-failed-exec.phpt`).

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
   discard path is for.
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
   every child and re-execs, and an OOM at that moment, a `SIGTERM` between the
   check and the exec, or an `execvp()` that fails still ends the generation.

So the gate's failure mode is "the service keeps running the configuration it
already has", never "the service is down because of the check" — except through
the last two items above, which are the reload's own pre-existing risks and are
unchanged by it.

### Reloading from a supervisor

Both packaged units run the test themselves, so an operator gets the reason on
the spot instead of a silent no-op:

- systemd — `/lib/systemd/system/php-fpm-ng.service`
  (`packaging/deb/php-fpm-ng.service`), two `ExecReload=` lines, which
  `systemd.service(5)` says run in order and stop at the first failure:

  ```
  ExecReload=/usr/sbin/php-fpm-ng --nodaemonize --fpm-config /etc/php-fpm-ng/php-fpm-ng.conf -t
  ExecReload=/bin/kill -USR2 $MAINPID
  ```

  Measured on the container CI uses, with that exact command and `stderr` on a
  file instead of a tty (which is what a unit gives it): exit status **78** and
  the diagnostics on `stderr`, i.e. on the journal — plus the same lines in the
  `error_log` the configuration names, because a non-tty `stderr` makes `-t`
  use the file too. *Not measured:* an actual `systemctl reload`, this
  environment has no systemd; the ordering is `systemd.service(5)`, not a test.

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
  its old slot; the new pool's slots are cleared by punching a hole in the
  memfd, which frees the memory instead of faulting it in.
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
`fpm_pctl_exec()`, and once after the initial fork loop in `fpm_run()`. The last
call removes entries that no pool of the new generation adopted (a pool that
disappeared from the config), so a stale pid never stays in the environment.
It sends `SIGTERM` to each pid still listed in
`FPMNG_SELECTIVE_RELOAD_SURVIVORS` and logs a WARNING (`issue #690`). It uses
`SIGTERM`, not the graceful `SIGQUIT`, even on a graceful stop: the pool type is
unknown there, and a request in flight on a spared worker is cut. Adoption
removes a pool's entry as it takes it, so after a successful start nothing is
left to discard.

Not covered: a new master that is killed outright (`SIGKILL`, a crash) cannot
run this code, and its spared workers stay orphaned. Workers do not poll their
parent pid. Covered by `fpmng-reload-selective-failed-init.phpt` (a new master
that cannot bind — a failure `-t` cannot see) and
`fpmng-reload-selective-failed-exec.phpt` (failed `execvp()`).

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
