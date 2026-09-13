# php-fpm-ng — project notes

Status as of 2026-09-05. This file is the project's memory: decisions, measured
numbers, open questions, and the list of known problems. Update it whenever the
direction changes, so the same conclusions do not have to be worked out again.

## 1. Why this exists

One binary plus application code in the container image, and nothing else. No
nginx, no supervisord, no system cron, no shell. One configuration file
describes the application together with its workers and cron jobs.

Target audience: small projects. One VPS, one instance, typically an
application plus one or two consumers and a few cron jobs. **Not** k8s and not
large companies.

The selling point is NOT performance (see section 4), but:
- one configuration file for the whole application together with its workers
- one binary to scan in the image
- no sidecars and no second system to learn

## 2. Architecture: a separate SAPI, not an FPM fork

`configure.ac:289` calls `esyscmd(./build/config-stubs sapi)`, and
`build/config-stubs` runs `for stubfile in $dir/*/config.m4`. Thus **every
directory placed under `sapi/` is detected automatically**. No patches to
existing php-src files.

This gives us the following structure:
- the separate repository contains only `sapi/fpmng/`
- the build clones php-src at a **pinned tag**, copies or symlinks the directory,
  then runs `./buildconf --force && ./configure --enable-fpmng`
- updating PHP means bumping the tag

### What we track by reference and what we own

Churn in `sapi/fpm/fpm` since 2024-01 (commits):

```
20  fpm_main.c        <- owned (the SAPI entry point must be different anyway)
16  fpm_conf.c        <- owned (directives, pool types)
11  fpm_status.c      <- owned (type-aware output)
 1  fpm.c             <- owned (hook at run_child:)
 1  fpm_process_ctl.c <- reference
 0  fpm_children.c    <- reference
 0  fpm_worker_pool.c <- reference
```

Process management is practically frozen — it is the foundation for the
supervisor and cron, and does not need to be taken over. The rest of the stable
files (`fpm_unix.c`, `fpm_stdio.c`, `fpm_scoreboard.c`, `fpm_events.c`,
`fpm_signals.c`, `fpm_sockets.c`, `fpm_shm.c`, `zlog.c`, ...) are also tracked
by reference — `PHP_ADD_SOURCES` accepts a directory, but **this must be
verified in a real build** before we rely on it.

Maintenance cost: about 14 upstream commits per year to review in the four
owned files. A few hours per year.

A useful trick: write changes to `fpm_conf.c` as one line appended to the
directive table plus new functions at the end of the file — then merge conflicts
are trivial.

### The pool-type seam

`fpm.c:88` — `fpm_run()` never returns in the parent; in the child it reaches the
`run_child:` label and returns a descriptor that `fpm_main.c:1793` receives
before entering the `fcgi_accept_request` loop. **This is the entire attachment
point.**

The pool type must be a real abstraction (a structure with `init`,
`spawn_child`, `child_main`, `status`, and `validate_config` operations), with
one file per type. Not a collection of `if` statements — with four modes, those
branches would spread through the code, exactly what we wanted to avoid.

`ini_fpm_pool_options` is one flat table — it needs information about which
directives make sense for which type, and must **reject** nonsensical
combinations (for example, `pm.max_children` in a supervisor pool) rather than
silently ignoring them.

## 3. Pool types

No `pool.type` → `fcgi`. Zero BC.

| type | what it does | status |
|---|---|---|
| `fcgi` | as today | ready, dispatch only |
| `http` | libevent HTTP gateway in front of the pool | POC works, see section 6 |
| `supervisor` | N long-lived processes, respawned | ready, see section 3o |
| `cron` | a script launched by a schedule | ready, see section 3r |

Metrics are cross-cutting, not a pool type.

### http

Under the pool-type model, `type = http` should mean that the pool speaks
FastCGI over a Unix socket in the runtime directory, with a gateway in front.
The FastCGI socket becomes internal. UDS is 11-17% cheaper than a TCP loopback
(measured). `FPM_HTTP_LISTEN` and the port-plus-one gymnastics disappear.

### supervisor

Smaller than it looks. `pm = static` with `pm.max_children = 1` already means
"one process, respawned after it dies". Logs go to the FPM log through the
existing pipe capture (`fpm_stdio_child_said`). User/group/chdir/rlimits come
from `fpm_unix.c`. SIGTERM comes from `fpm_signals.c`.

The child at `run_child:` does not return to the accept loop; it runs the
script. One call at the same location as today's `fpm_http_init_main()`.

An advantage that cannot be faked: the child is a fork of a master with **PHP
already initialized**, so the consumer starts without launching a new
interpreter and with OPcache ready. supervisord cannot do this because it only
starts `php consumer.php` again.

Still to add: `listen` must become optional (a supervisor pool does not listen),
a directive for the script path, backoff after a rapid death, the semantics of
`pm.max_requests` (the script exits after N jobs and FPM brings it back — the
same as `--max-jobs`, solving memory leaks), and the type in status output.

### cron

The same child path as supervisor, but a different launch policy. The master
already has timers (`fpm_events.c:86` uses `fpm_event_set_timer`). So this is a
timer plus a fork, with a few dozen lines on top of supervisor.

Crontab parser: 5 fields, about 100 lines, no dependencies. The alternative
`cron.interval = 300s` is much smaller, but people expect `*/5 * * * *`.

**Four things must be decided, or they will conflict:**
1. Overlapping runs — skip and log by default. This is the most common cron-implementation bug.
2. Time — calculate in UTC. Local time with DST produces a duplicate run or no run.
3. Missed ticks — **do not catch up**. Record this, because someone will eventually add catch-up and produce twelve runs at once.
4. A timeout per run plus exit-code logging.

Note: this is a singleton on the master, not a cluster feature. It is irrelevant
for one instance (our target audience), but must be documented.

### metrics

A structure in `fpm_shm_alloc`, PHP functions, and exposure in Prometheus text
format. Precedent: `pm.status_path` is matched inside a request in
`fpm_main.c:1832`.

**Cardinality trap.** Shared memory has a fixed size, while labels can come from
user data. Use a fixed number of slots and make an explicit decision for
exhaustion: reject and log; do not grow.

**A split to settle early.** FPM registers PHP functions through the `cgi-fcgi`
SAPI in `fpm_main.c`. The CLI does not have them. If metrics must also work from
the CLI, they must be an **extension** that uses the shared-memory backend under
fpm-ng and a local or empty backend outside it. Moving this later will hurt.

A supervisor pool does not handle requests, so it cannot expose its own metrics;
another pool must read them. The scoreboard is per pool in shared memory, so it
is possible, but it is a new access pattern.

## 3a. Self-runner: one binary containing the application

Goal: `fpm-ng pack app.phar php.ini fpm.conf -o myapp` produces one file that
contains everything. The cherry on top, done last.

**This is not compilation, only appending data.** A payload at the end of the
finished binary plus a footer at the very end: magic bytes, offset, and size.
At startup fpm-ng reads `/proc/self/exe`, checks the final bytes, and if it finds
the magic, takes `php.ini`, `fpm.conf`, and the phar from there. If it finds
nothing, it works normally from disk.

Consequence: **packaging requires no toolchain**. No compiler and no PHP source.
It takes a second. This is an advantage over FrankenPHP, which can also embed an
application, but rebuilds it with Go embed — meaning the user needs Go.

We are not inventing a format: phar works exactly this way (a stub is appended
to the file, and phar can find itself inside a larger file).

### Things to consider

1. **Writes — the biggest problem.** A phar cannot be written to. The
   application needs cache, sessions, logs, and uploads. Configuration must
   firmly separate "code, immutable, in the binary" from "state, on a volume".
   It is healthy discipline, but it will surprise anyone using Laravel or
   Symfony, because they write to the project directory by default. Solve and
   document this as a design matter, or the first encounter with a real
   application ends with "it does not work".
2. Static files from a phar go through a stream wrapper — slower than disk. It is
   irrelevant for our traffic, but loading them into memory at startup may be worth it.
3. With `validate_timestamps=0`, code in the binary does not change, so OPcache
   works perfectly and faster than from disk.
4. `/proc/self/exe` is Linux; in a container `/proc` is mounted, so scratch is
   OK. Keep a fallback through `argv[0]` for running outside a container.
5. Optionally sign the payload (phar supports this) and refuse to start if the
   application code has been replaced.

### PROJECT CONSTRAINT FOR PLAN ITEM 1

Configuration loading must accept data **from a stream or an in-memory buffer**,
not only from a path on disk. If we hard-code `open(path)`, changing it later
will hurt.


## 3b. Binary extensions from other vendors (New Relic, ionCube, ...)

**A static binary cannot load any `.so`.** On musl, static `dlopen` is a stub
that returns an error; static glibc is formally possible, but broken and
unsupported. These extensions cannot be compiled in because vendors provide
`.so` files, not source code.

Affected vendors: New Relic, Datadog, Blackfire, ionCube, SourceGuardian, Zend Guard.

**Solution: two build variants from one source tree, differing only in linker flags.**

1. *static* — scratch, without third-party extensions;
2. *dynamic* — a small base image (Alpine/distroless, ~8 MB), loading `.so`
   files. This is ordinary building; point `extension_dir` and `extension=` in
   `php.ini` at the files. Nothing special needs to be written.

This fits the self-runner (section 3a) nicely: `.so` files can travel **in the
payload** and be unpacked into tmpfs at startup, with `extension_dir` set to
that directory. It is still one file to ship, only a dynamic binary. Replacing
an extension means repacking, not rebuilding. **This should probably be the
default variant**, with full static as the variant for users who need nothing
else.

Priority: ionCube matters more than New Relic. Small projects rarely have APM
(expensive, enterprise), but many host commercial software encoded with ionCube
(WHMCS and similar products).

A static-independent catch: these extensions are built for a particular ABI
(PHP version, NTS/ZTS, libc). Most vendors now have musl variants for Alpine,
but not all — **check with specific vendors before promising anything**. The
dynamic variant may need to come in two forms, glibc and musl.

### DECISION TO MAKE EARLY: `php_sapi_name()`

Half the ecosystem checks the SAPI name. Frameworks test
`PHP_SAPI === 'fpm-fcgi'` to know whether `fastcgi_finish_request()` exists.
New Relic uses it to identify the application type. Monitoring tools do the
same.

If we use a different name, all of this silently stops working.
Proposal: report `fpm-fcgi`. This is not deception — we are FPM with added
modes. But the decision must be deliberate, not discovered six months later
when `fastcgi_finish_request()` does not work for someone.

## 3c. Build findings (2026-09-05)

- **musl does not have `sys/queue.h`** (it is a BSD header; glibc includes it
  as a courtesy). The gateway used its TAILQ macros. Fixed: `fpm_http.c`
  includes it through `__has_include` and supplies the missing macros itself,
  each under its own `#ifndef` — because libevent headers can pull in a partial
  `sys/queue.h`.
- **`LDFLAGS=-static` IS NOT ENOUGH.** The Alpine toolchain defaults to PIE,
  and `-pie` conflicts with `-static`, so the linker silently produces a
  dynamic binary while the build still succeeds. Check with `file`. The correct
  flag is **`-static-pie`** — it works and preserves ASLR.
- The gateway's default docroot is the pool's `chdir` or the current directory.
  In scratch, the current directory is `/`, so without `chdir` you get "File not found". Replace this with an explicit directive.
- `php_sapi_name()` currently returns `fpm-fcgi` — so the decision from section
  3b is simply "do not change it".

### PLAN ITEM 0 RESULT (2026-09-05): GREEN

A `static-pie` binary on musl running in a bare `FROM scratch` produced HTTP
200, executed PHP, ran FPM as PID 1, and used `user = 65534` without `/etc/passwd`.
**The entire image was 20 MB**, including PHP, the gateway, and the application.
The test configuration is in `~/ngbuild/scratch/` on the test host (Dockerfile,
fpm.conf, and www/hello.php).

The concept works. But see below — scratch was demoted to a secondary target.

- Test build: `~/ngbuild/build3.sh` (dynamic) and `build4.sh` (`-static-pie`)
  on the test host, out of tree in Alpine
  (`docker run -v php-src:/src -v ngbuild/build:/build -v ngbuild:/out alpine`),
  `--disable-all --enable-fpm --with-fpm-http`, `LDFLAGS=-static`.


## 3d. DECISION (2026-09-05): Alpine is the base, not scratch

Piotr: "scratch is a secondary target — Alpine for musl and GCP images are
enough, some kind of minimal image".

Consequences, all positive:
- **Plan item 0 is no longer a gateway blocker.** Dynamic builds simply work.
  The project's largest risk is removed.
- Third-party `.so` extensions work by default — the section 3b problem is
  solved, including ionCube and New Relic without workarounds.
- Three missing files (`resolv.conf`, certificates, `tzdata`) disappear — they
  come from the base image.
- Static remains an **optional variant** for users who need nothing else, not the
  foundation. It is verified to work (see 3c), so the option exists.

## 3e. SAPI skeleton — verified (2026-09-05)

`sapi/fpmng/` was built against **untouched** php-src. `git status` in the
upstream tree shows only `?? sapi/fpmng/` — no existing file was changed. The
`php-fpm-ng` binary starts, the HTTP gateway responds, and `php_sapi_name()`
returns `fpm-fcgi`.

Our files (the rest are copied from `sapi/fpm/` by `build/prepare.sh`):
`config.m4`, `Makefile.frag`, `fpm/fpm.c`, `fpm/fpm_http.c`, `fpm/fpm_http.h`.

### What had to be renamed for both SAPIs to coexist

- `AC_DEFUN` macros: `PHP_FPM_*` → `PHP_FPMNG_*` (otherwise "already defined");
- variables: `SAPI_FPM_PATH`, `BUILD_FPM`, `FPM_EXTRA_LIBS`, `PHP_FPM_OBJS`;
- **configure options too**: `--with-fpm-systemd` → `--with-fpmng-systemd`, etc.
  Renaming only the variable is not enough — `PHP_ARG_WITH` generates the
  variable from the option name, so renaming only the variable makes the test
  read an undefined value and run despite being disabled (systemd broke the
  build);
- `Makefile.frag` — the `fpm:` target must become `fpmng:`, otherwise
  `No rule to make target 'fpmng'`.

### GENERATED, NOT HARDCODED, SOURCE FILES

The first attempt hard-coded the file list in our `config.m4` and failed at
once: our copy came from a newer php-src without `events/devpoll.c`, while we
were building against an older one that had it → `undefined reference to
fpm_event_devpoll_module`.

Therefore `config.m4` has the `@FPMNG_SOURCES@` placeholder, and
`build/prepare.sh` extracts the list from `sapi/fpm/config.m4` in **that exact
php-src** and appends our files. This permanently solves this class of drift —
and is the pattern to repeat wherever hard-coding an upstream detail is tempting.

### Operational detail

The build directory is created in the container and belongs to root — cleaning
it from the host requires `sudo rm -rf`.


## 3f. Model boundary: files outside `sapi/` — the patch mechanism (2026-09-05)

The GH-18956 statistics-counting fix touches `main/fastcgi.c` and
`main/fastcgi.h`, the **php-src core outside `sapi/`**. A separate SAPI cannot
reach those files. This is the first real limitation of the model.

**And it is not a theoretical problem; it is our own problem.** The gateway
keeps persistent connections to the pool (`FCGI_KEEP_CONN`), so fpm-ng is exactly
the case broken by this bug: the idle-versus-active counter lies, and
`pm = dynamic` and `ondemand` scale the pool incorrectly. Without the patch,
only `pm = static` is trustworthy.

Decision: **carry the patch**, but make the deviation visible and measurable.
`patches/` + `prepare.sh` applies it and reports it loudly; without patches it
says explicitly "upstream untouched". After applying, `git status` in the
upstream tree shows exactly `M main/fastcgi.c`, `M main/fastcgi.h`,
`?? sapi/fpmng/` — the complete blast radius on one screen.

The rules in `patches/README.md`: each patch names the upstream PR and disappears
when that PR is merged; one patch per problem, not per version; more than two
version variants are a signal that the change must go upstream or into
`sapi/fpmng/`; CI builds every supported version, so a patch that no longer
applies fails the build.

**How many variants per version?** Measured: `0001` applies cleanly to PHP-8.3,
8.4, 8.5, and master. `main/fastcgi.c` has 2–9 commits per year, and they do
not touch our area. So far there are zero version variants.

The companion changes in `fpm_request.c` and `fpm_request.h` are carried as our
own files because they live under `sapi/`.

Operational detail: a dynamic Alpine binary needs `libgcc` in the container
alongside `libevent`; otherwise it reports `Error loading shared library libgcc_s.so.1`.


## 3g. Freedom on the HTTP path and the extensibility contract (2026-09-05)

Because this will not fit easily into php-src, the HTTP path is not bound by
compatibility with anything. But the distinction remains: **we may move fast in
our own files**; in the four shared files (`fpm_main.c`, `fpm_conf.c`,
`fpm_status.c`, `fpm.c`), every unusual change costs us at every PHP release.

None of the following work is motivated by performance — there is no meaningful
performance headroom there (section 4). We do it to remove moving parts or add a
missing capability.

| idea | assessment |
|---|---|
| **Static files in the gateway** (`sendfile`, without involving the worker) | DO IT first. The only item that decides whether nginx is still needed. Possible ONLY with a gateway — the in-process variant loses it. |
| **The HTTP pool does not open a FastCGI socket** (a Unix socket in the runtime directory) | DO IT alongside `pool.type` — the same work. Less attack surface and one fewer directive. |
| **Remove FastCGI framing from responses, `splice()`** | Later. The gateway remains in the path, so 502 and timeouts still work. A clean gain without losing control. |
| **Pass the descriptor through `SCM_RIGHTS`** | REJECTED. The largest gain for large responses, but if the worker dies halfway through, the client gets a truncated stream and the gateway cannot send 502 or enforce a timeout on a socket it no longer owns. With one instance and no cluster, this is a bad trade. |
| **`$_SERVER` without the CGI detour** | REJECTED for now. Applications rely on the exact CGI key set, so the saving would be internal only. |

### `fcgi-async` — experimental, old `fcgi` unchanged

Piotr's decision. The caveat to remember: **BC covers configuration and the
protocol, not internal behavior.** Existing `fpm.conf` must work and nginx must
be able to talk to it — but bug fixes and internal improvements belong in
`fcgi`. GH-18956 is a fix, not a contract change.

`fcgi-async` makes sense as a place for changes that break observable behavior
and for experiments (io_uring, per-worker `SO_REUSEPORT`, syscall batching).
One structural point still needs verification: the FastCGI path is built
differently from the gateway. The gateway is our process with a libevent loop;
the FastCGI worker is a blocking loop in `fpm_main.c`, while FPM's event loop
lives in the MASTER. Therefore what worked in the gateway does not necessarily
translate here. **A separate agent is investigating this — paste the result here.**

### `http-async` (in-process) — not now, but keep the path open

Correction to an earlier argument: I said that without a gateway, keep-alive
pins a worker, so eight workers mean eight connections. That is true only when
the worker returns to `accept` after the response. A worker with its own event
loop holds many connections and processes one at a time — then the objection
disappears, and the kernel backlog per `SO_REUSEPORT` socket does the queuing.
The architecture holds up.

The real cost is different and more serious: today the worker is DUMB (reads
FastCGI, runs the script, writes the result), while the gateway takes on HTTP
parsing, keep-alive, timeouts, body limits, and slow clients. In-process pushes
all of that into the process executing PHP code — every HTTP parser bug becomes a
bug in the process holding application memory, with nothing in front of it.
This is the FrankenPHP worker mode. It works, but it is a different product.

This is sequencing, not a ban: rewriting the serving core is good when there
are users, but a bad second step with no user base.

## 3h. CONTRACT: the pool type must be extensible

Requirement recorded BEFORE the code, at an explicit request.

Adding a new pool type must cost **one new file plus one line in the registry**.
Nothing else. In particular, it must not touch:

- validation logic in `fpm_conf.c` (the directive table may change, validation `if` statements may not);
- `fpm_children.c` — if a new type needs a change there, the spawning policy was not cleanly separated;
- `fpm_status.c`, apart from adding a label.

One operations structure per type:

```
validate_config()   what is required and forbidden for this type
init_main()         master-side preparation (sockets, gateways, timers)
spawn_policy()      how many children and when (static N / on socket / on tick)
child_main()        what the child does at run_child: — accept loop or script
status()            how it appears in status output
```

Known types that must fit this interface: `fcgi` (the default, no `pool.type`
means `fcgi`, zero BC), `http`, `fcgi-async`, `supervisor`, and `cron`, with
`http-async` in the future. If any of these does not fit cleanly, the interface
is wrong and it is better to find out now.

`listen` is no longer unconditionally required (verified: today a pool without
it produces `ALERT: no listen address have been defined!`) — the type's
`validate_config()` decides whether it is required.


## 3i. `pool.type` — implemented and verified (2026-09-05)

The registry is in `fpm_pool_type.c`. The type declares configuration
requirements as **data** (`requires_listen`, `requires_pm`, `serves_requests`),
not as code — so `fpm_conf.c` does not know any particular type and adding
another type requires no change there. The 3h contract is satisfied: **a new
type = one new file plus one line in `fpm_pool_types[]`**.

Verified on a built binary:
```
no pool.type    -> fcgi, no gateways, default port        (full BC)
pool.type = http -> gateway starts and responds
pool.type = xxx -> ALERT: unknown pool.type 'xxx'; known types: fcgi, http
```

**Side effect: one blocker disappeared.** The gateway no longer starts by
default for every TCP pool — it must be requested with `pool.type = http`.

### Restricting directives per type

Requirement: a block must have one type and a **limited** set of directives —
directives that make no sense for the type must be rejected, not silently ignored.

The problem: the configuration value alone **cannot distinguish "unset" from
"set to the default value"** (`pm_max_children = 0` could mean either). Therefore
`fpm_conf.c` records the directives actually set during parsing in
`config->set_directives` as `";name;name;"` — delimiters on both sides prevent a
prefix false positive (`pm` versus `pm.max_children`).

The type declares `rejects` — a NULL-terminated array of names. It is a **rejection
list, not an allow-list**: a new directive is allowed everywhere by default, so
missing an entry cannot break backward compatibility. A name ending in a dot
works as a prefix (`pm.` matches all of `pm.*`).

STATUS: the mechanism is implemented and compiled, but it **has no consumer yet**
— neither `fcgi` nor `http` rejects anything because both currently use the same
directives. The first real test will be `supervisor` (it will reject `listen`,
`pm.start_servers`, `pm.min_spare_servers`, `pm.max_spare_servers`,
`request_terminate_timeout`, `slowlog`, `ping.path`, and
`security.limit_extensions`). Until then, treat it as unverified.

### Two implementation details worth remembering

**The child finds its pool through the scoreboard.** The naive solution (using
the loop variable `wp` at the `run_child:` label) is WRONG: children respawned
from the event loop leave `fpm_event_loop()` through
`if (fpm_globals.is_child) break` and reach `run_child:` with `wp == NULL`,
because the loop over pools ended long ago. The scoreboard is per pool and the
child receives its own at `fpm_scoreboard_init_child()`, so matching is enough —
`fpm_children.c` remains untouched, as required by the contract.

**A reload bug was fixed** (found in section 6, previously unverified): gateway
cleanup is now also registered for `FPM_CLEANUP_PARENT_EXEC`. Reload uses
`execvp()`, so without this the gateways became orphans and held the port that
the new master wanted to bind.

### A `prepare.sh` trap that I introduced and fixed

Testing whether a patch is already applied with `patch -R --dry-run` **first** is
wrong: on an untouched tree it can also report success (BSD patch on macOS).
The result would be quiet and nasty — a binary without the patch and a message
saying that the patch is present. The order must be: try the forward patch first,
then test the reverse direction.


## 3j. Statistics for `supervisor` and `cron` — design (2026-09-05)

Requirement: statistics for pools that do not handle requests must be
available over HTTP on a separate port. In Docker/k8s, the orchestrator manages
restarts (see `supervisor.fatal`), so fpm-ng's role there is **state reporting only**.

### Why this is not obvious

A supervisor or cron pool **does not handle requests**, so it cannot expose its
own statistics — `pm.status_path` works inside a request (`fpm_main.c:1832`).
Something else must expose statistics for such a pool. The scoreboard is per pool
in shared memory, so technically every process can read it — but this is a new
access pattern; nothing does it today.

### The data shape is DIFFERENT from FastCGI

This is the core of the problem, not a detail. The FastCGI scoreboard measures
idle/active workers, request count, and queue length. For supervisor and cron,
completely different values make sense:

- state: running / sleeping in backoff / gave up / exited normally;
- time of the last start and lifetime of the current process;
- exit code of the last completion;
- consecutive failure count and current backoff delay;
- for cron, additionally: time of the last run, time of the next run, and how many runs were skipped due to overlap.

Therefore `fpm_status.c` must become type-aware. The `serves_requests` field in
the type descriptor **already exists and is currently unused** — this is where
the branch belongs.

### Where to expose it — undecided

1. **A separate `pool.type = status` pool** — a small HTTP listener that does
   not start PHP at all, but reads the scoreboards of all pools and serializes
   them. Advantage: a separate port, so metrics are not exposed on the public
   port; it is also an excellent test of the extensibility contract (a type
   without workers, `pm`, or a script). Disadvantage: another pool type.
2. **The HTTP gateway answers `/status` and `/metrics` itself**, without involving
   the worker — the same mechanism as the planned static files. Advantage:
   nothing new. Disadvantage: the same ports as public traffic, so access
   control is required.

**I lean toward (1)** because separating the metrics port from the public port
is worth more in production than saving one type, and Piotr explicitly said
"on some port".

Format: Prometheus text plus JSON on another path. Cardinality is limited here
(see the metrics section), because the label is the pool name and there are only
finitely many pools.

### Order

After supervisor and cron, together with metrics — only then is it clear what
there really is to show. Doing this earlier would mean guessing the data shape.


## 3k. PHP metrics — API and design decisions (2026-09-05)

### Where the code lives: an EXTENSION, not SAPI functions

`configure.ac:1097` calls `esyscmd(./build/config-stubs ext)` — `ext/` is
detected by the same glob as `sapi/`. The repository can therefore contain
`ext/fpmng_metrics/`, and `prepare.sh` will place it next to `sapi/fpmng/`, still
without patches to upstream.

This settles the split from the metrics section: functions registered in
`fpm_main.c` would exist only under fpm-ng, while CLI mode with metrics enabled
manually is one of the product goals. Under fpm-ng the extension uses shared
memory; under CLI it has process-local storage and a function returning rendered
text so the script can expose it itself. The same PHP code works in both places.

### API

```php
fpm_metric_register(string $name, string $type, string $help, array $buckets = []): bool
fpm_metric_inc(string $name, float $by = 1.0, array $labels = []): bool
fpm_metric_set(string $name, float $value, array $labels = []): bool
fpm_metric_observe(string $name, float $value, array $labels = []): bool
```

The type follows from the function used; `register` is optional (HELP and
buckets). All functions return `bool` — this detects pool exhaustion; it is not
decoration.

### Concurrency: SLOTS PER WORKER, not atomics on a shared counter

A dozen workers atomically hitting the same counter creates cache-line
contention on every `inc`. Instead, each worker writes to its own unsynchronized
fragment, and **aggregation happens at read time**. Zero locks.

The slot is keyed by the **process index from the scoreboard, not the pid** —
otherwise the counter would start at zero again after `pm.max_requests` recycling.

A gauge is not summed: sum by default, but registration must allow selecting an
aggregation (maximum makes sense for "memory usage"). Prometheus has the same
problem in multiprocess mode and solves it the same way.

### Cardinality — a hard rule

The directive sets a fixed series limit. When it is exhausted, **reject and
return `false`**; never grow. Log a warning **once**, not on every request, and
include the name of the metric that exhausted the pool — without that, the
operator cannot find the culprit.

### Histograms ENTER the first version

CORRECTION to my earlier, wrong assessment. For a queue consumer, the processing
time distribution is the one metric that makes sense — an average hides the
tail, and the tail is exactly what fills a queue. I also overestimated the cost:
a histogram with fixed buckets is N counters plus a sum and a count, with a
bucket lookup among a dozen values on increment. Exemplars, native histograms,
and quantile estimation are the expensive parts — we are not doing those.

Stricter rule: **histograms are not expensive by themselves; they are expensive
when combined with labels having an unbounded number of values.** A request-time
histogram with a `route` label, 100 routes, and 12 buckets is 1200 series times
the number of workers. The same consumer histogram with a `queue` label and 5
queues is 60 series.

**This decides `fpm_metric_observe`:** with a supervisor there is NO WAY to infer
where one job ends — the script is a long-lived loop, and only the application
knows the boundary. Therefore this is not a convenience feature, but the only
way to get useful consumer metrics.

Default buckets must extend further than typical HTTP buckets (jobs last longer
than requests): 0.005 to 60 s.

### How a consumer distinguishes what a metric describes

The pool name is a safe key — verified: at `fpm_conf.c:1439`, a repeated
`[name]` section returns to the existing pool instead of creating a second one.

Two mechanisms answer two different questions:

1. **What type is the pool?** An information metric, always present:
   `fpmng_pool_info{pool="queue",type="supervisor"} 1`.
2. **Which fields should be expected?** Use the metric NAME, not its label.
   `fpmng_fcgi_idle_processes` simply does **not exist** for a supervisor pool.
   A metric name must have one meaning and one unit; supervisor "idle" is not the
   same concept as an idle FastCGI pool.

**TRAP:** never emit a false zero where the concept makes no sense.
`fpmng_fcgi_idle_processes{pool="queue"} 0` would trigger an "idle == 0" alert
and wake someone up for a pool that has no such concept. **No series is
information; a false zero is a lie.**

Use an enum with a label for state (`state="running"|"backoff"|"gave_up"`,
each value 0/1), not one number encoding the state. JSON omits inapplicable
fields. The human-facing page gets a separate table per type.


## 3l. TLS and ACME — DECISION: build them, but last (2026-09-05)

Piotr: "TLS and ACME last — but we will build them". It is no longer an open question.

### Consequence 1: this closes the `http-async` question

With TLS, the worker cannot write directly to the client — the stream is
encrypted and session state lives in the gateway. The in-process variant would
have to give every worker its own TLS state and certificate handling. This
practically **closes** that path rather than merely postponing it. Do not return
to it without a new argument.

### Consequence 2: the ACME client is embedded PHP (decision, 2026-09-07)

The client will be a project-owned PHP script run by a dedicated `cron` pool,
not C in a gateway process. It is part of fpm-ng, not part of the user's
application.

The three checks behind the decision:

1. **The measured C baseline is 8,327 physical lines.** The source is
   `ndilieto/uacme` at revision
   `e9cfa6f052644864a28c7d9d04756900abfc653f`. At that revision
   `Makefile.am` names 13 production C/header files for `uacme` (excluding the
   optional `ualpn` helper and optional `read-file` backend); counting those
   files with `wc -l` gives 8,327. The count is reproducible with:

       git clone https://github.com/ndilieto/uacme.git
       cd uacme
       git checkout e9cfa6f052644864a28c7d9d04756900abfc653f
       wc -l uacme.c base64.c base64.h crypto.c crypto.h \
         curlwrap.c curlwrap.h json.c json.h jsmn.h msg.c msg.h

   `uacme` supports RFC 8555 and delegates challenge installation to a hook;
   its `uacme.sh` at the same revision demonstrates HTTP-01 by writing the key
   authorization under `/.well-known/acme-challenge/`. A single CA and
   HTTP-01 remove CLI and alternate-challenge branches, but do not remove the
   protocol, HTTP, JSON, JWS, key and CSR machinery represented by this real
   baseline. Maintaining that machinery in project-owned C is not justified.
2. **The PHP bootstrap is a small explicit state machine.** `NO_CERT` means
   port 80 serves only a known HTTP-01 token (otherwise redirect/service
   unavailable), port 443 is closed, and the dedicated ACME process is allowed
   to issue. `ISSUING` adds exactly one token to the shared challenge state and
   returns to `NO_CERT` with backoff on failure. An atomic certificate install
   moves to `READY`; every gateway loads it, opens port 443, and port 80 returns
   to challenge-or-redirect behavior. `READY` renewal failures retain the
   current certificate and retry with backoff; successful renewal uses task
   040's existing certificate-reload mechanism. These are three states and two
   transitions, not per-request special cases. The only bootstrap-specific
   behavior is withholding HTTPS until certificate installation.
3. **Yes, the script can be embedded without coupling it to the
   self-runner.** Use the same append-only payload plus footer mechanism
   described in section 3a, but give the distribution payload its own magic,
   offset and size. The build embeds the project-owned ACME script; the
   optional pack command appends the user's application as a separate payload.
   Runtime lookup selects the distribution entry by kind, so either payload
   can exist without the other and repacking an application does not replace
   ACME code.

The PHP client therefore requires the PHP OpenSSL extension and an HTTPS
client capability in supported builds. Task 047 must choose and document the
concrete HTTP mechanism; this decision does not silently assume that the
gateway's libevent HTTP client API is exposed to PHP.

TLS termination remains in C through `bufferevent_openssl` (libevent provides
it and OpenSSL is already linked statically; see section 3c). Certificate
replacement in every gateway uses the no-disconnect reload mechanism from task
040.

### Two constraints to preserve

1. The gateway request path has one point that answers without a worker.
   Static files, the ACME challenge and `/status` share that hook. Task 046
   must put the fixed challenge path before disk-backed static files.
2. Certificates and the ACME account are mutable state, not code. They live on
   a writable volume under the immutable-code/mutable-state split shared with
   the self-runner; task 044 defines that split once.

### Configuration consequence

HTTP-01 requires port 80 while application traffic uses port 443. Task 042
implemented the plain redirect companion in the same pool; task 046 extends
its local-answer hook with HTTP-01.

### DECISIONS (2026-09-06, project owner) — see tasks 039-042

- **ALPN: yes.** Advertise `http/1.1`; a client offering only an unsupported
  protocol is rejected at the TLS layer.
- **SNI: yes.** One pool may serve more than one certificate — a certificate
  selection callback based on the server name in `fpm_tls_http_ctx_new()`, as
  per-process state rather than a new `struct fpm_tls_http_s` field. Without
  SNI, or with an unknown name, use the default (first configured) certificate.
  Consequence: 020 has both ACME challenges open (HTTP-01 and TLS-ALPN-01).
- **"one pool, one port" is NOT sufficient** — solution: a redirect-only
  companion (task 042). Port 80 is in the same pool as TLS, never reaches the
  worker, and answers only with a 301/308 redirect and (after 046) an HTTP-01
  challenge through the existing local-answer hook (`fpm_http.c:1041-1048`).
  Cost: the gateway gains a second ordinary HTTP socket per process for pools
  that opt into it.

**Implementation note (English, task 041, done):** both decisions above are
implemented in `sapi/fpmng/fpm/fpm_tls_http.c`. ALPN: `SSL_CTX_set_alpn_select_cb()`
on every `SSL_CTX` this file builds, advertising `http/1.1` only; a client
offering ALPN without `http/1.1` gets `SSL_TLSEXT_ERR_ALERT_FATAL` (verified
with `openssl s_client -alpn`). SNI: a new pool directive, `http.tls_sni_cert`
(`servername:cert_path:key_path`, comma-separated, on top of the existing
`http.tls_cert`/`http.tls_key` default pair), parsed and validated the same
way as the primary pair; the per-servername `SSL_CTX*` switch table is built
in `fpm_tls_http_ctx_new()`, per gateway process, exactly as this decision
requires — never a new field of `struct fpm_tls_http_s` (that struct only
carries the raw PEM bytes, read once in the master, the same way it already
did for the primary pair). Test: `sapi/fpmng/tests/fpmng-http-tls-alpn-sni.phpt`.
Scope cut, not covered by this task: SNI certificates are validated once at
startup but are NOT part of task 040's hot-reload mtime check — only the
primary `http.tls_cert`/`http.tls_key` pair reloads without a restart; a

**Implementation note (English, task 042, done):** `http.plain_listen` adds a
redirect-only plain HTTP companion to the TLS listener in the same pool. It
returns 308 to the same host, path, and query over HTTPS. The
`/.well-known/acme-challenge/` namespace is answered locally with 404 until
task 046 provides challenge contents; it is never redirected or dispatched to
a worker. The complete compact configuration is:

```ini
[app]
pool.type = http
listen = 127.0.0.1:9000
pm = dynamic
pm.max_children = 8
http.listen = 0.0.0.0:443
http.plain_listen = 0.0.0.0:80
http.tls_cert = /state/acme/example.com/fullchain.pem
http.tls_key = /state/acme/example.com/privkey.pem
```
renewed SNI certificate needs one. See task 041 (done; [`task-archive.md`](task-archive.md)).


## 3m. `fcgi-async` — research results and the REAL GOAL: an experimental async build

### The goal that gives all numbers their meaning

`fcgi-async` is not primarily about squeezing microseconds out of today's FPM.
It is meant to be an **experimental build for real asynchronous PHP**. That
changes the weight of every measurement below.

**Async changes the denominator from wall-clock time to CPU time.** The refrain
"on a 20 ms request, transport is 0.15%" assumes that the worker is busy for
those 20 ms. But a typical PHP request is about 2 ms of CPU and 18 ms waiting for
the database — today that wait blocks the process. Under async, the worker
handles other requests during the wait, so CPU per request matters, not wall
clock: **30 µs out of 2 ms is 1.5%, not 0.15%.** For a lightweight cached API
endpoint (200 µs CPU), it is 15%.

### Research results (agent, 2026-09-05, test host, artifacts in `~/ng-research/`)

The FastCGI worker makes **26 syscalls per keep-alive request**, 33 for a new
connection. By comparison, our HTTP gateway makes about 9 — so in the gateway
plus worker pair, **the worker was the larger consumer**, while we optimized the
thinner end. Only **7 of the 26 are FastCGI**; the rest are PHP/Zend/FPM overhead:
8x `rt_sigaction` (`zend_signal_activate`), 2x `setitimer` + `rt_sigprocmask`
(`max_execution_time`), `getcwd` + 2x `chdir`, 2x `fcntl` (OPcache lock), 2x
`times` (request CPU for status), and `write(2,"\0fscf")`.

Measured with two experimental patches (spread < 3%):

| | syscalls | TCP µs/req | UDS µs/req |
|---|---|---|---|
| today | 26 | 65 | 54 |
| input buffer + `accept4`, without `poll` | 21 | 56 | 46 |
| + accounting removed | 8 | 34 | 26 |
| + `max_execution_time=0` | **4** | **30** | **23** |

Two items are **free through configuration alone**: `max_execution_time=0`
(−3 µs) and a Unix socket instead of a TCP loopback (−7…−11 µs).

The floor after removing everything is about 23 µs (UDS), of which about 9 µs
is PHP itself. There is no larger remaining item: `stat` on the script does not
occur at all (OPcache serves it from SHM), realpath hits the cache, and the
scoreboard costs about 0.25 µs.

**Hardware caveat:** the test host has PTI + IBRS, so a syscall costs about
0.9 µs there. On newer CPUs (Ice Lake+, Zen) it is 0.1–0.3 µs, so the
**absolute gain shrinks 3–5x**. Those 30 µs are an upper bound, not a typical value.

### UPSTREAM BUG: `TCP_NODELAY` is never enabled on Linux

Confirmed in our own tree. `main/fastcgi.c:891` assigns `req->tcp` **only under
`#ifdef _WIN32`**, while line 1085 uses it unconditionally to decide about
`TCP_NODELAY`. Outside Windows the field remains zero from `calloc`, so on TCP
keep-alive a response larger than 8 KB goes through several `write` calls and
the final small segment waits for ACK → Nagle + delayed ACK.

Measured: **40 ms** instead of 80 µs. It was NOT reproduced with real nginx on a
loopback (nginx ACKs quickly), so it does not hurt a typical deployment — but it
does with a non-loopback path or another FastCGI client.

The fix is one line. **Report it upstream independently of this project.**

### Two rejected directions — but one only CONDITIONALLY

**Per-worker `SO_REUSEPORT` — permanently rejected.** Thundering herd **does not
exist at all**: blocking `accept` uses an exclusive queue and the kernel wakes
one worker (measured: 1.78 context switches per `accept` with 8 workers versus
1.88 with one). And `SO_REUSEPORT` hashes a connection to a socket, not to a
free worker, so it would worsen the tail.

**`io_uring` — rejected ONLY FOR THE CURRENT MODEL.** The agent based the verdict
on the fact that after its patches the cycle is `read` → PHP → `write` → `read`,
so at most 1–2 syscalls can be combined. That is correct for a **blocking worker**.

Under async, the worker does not sit in a blocking `read`; it has an event loop
and N concurrent requests — exactly what io_uring was made for: many
file descriptors, many operations submitted through one `io_uring_enter`,
multishot accept/recv, and no `epoll_ctl` for every interest change.
**Async requires a new calculation. Do not quote only the conclusion.**

### CORRECTION: async reopens `http-async`

In section 3l I wrote that TLS practically closes the in-process variant because
the worker would need its own TLS state and event loop. Under async the worker
**already has an event loop** — that is the essence of async. HTTP and TLS in the
worker then stop being a parser pushed into a process with nowhere to hold it.

The argument was too strong. `http-async` returns to **open**, not closed.

### Agent recommendation — order for `fcgi-async`

1. Fix `TCP_NODELAY` (one line, a bug, report upstream).
2. Input buffer + `accept4` (~7 µs keep-alive, ~12 µs new connection, no wire change).
3. `write(2,"\0fscf")` only when `catch_workers_output` is enabled (~1 µs).
4. CWD cache in the SAPI + `SAPI_OPTION_NO_CHDIR` (~5 µs, preserves CWD semantics).
5. Opt in to request CPU accounting (`times()`, ~2.6 µs).
6. Remove `poll` ONLY together with `SO_RCVTIMEO` on the listening socket
   (`poll` protects against a silent client — the ACCEPTING stage is not covered
   by `request_terminate_timeout`).
7. Document `max_execution_time=0` + UDS for micro-endpoints.

Outside `fcgi-async`, send upstream: 7x `rt_sigaction` in
`zend_signal_activate` (~6.5 µs) and 2x OPcache `fcntl` (~1.7 µs) — 40% of the
remaining syscalls, but not on the SAPI side.


## 3n. Mode names: `fcgi-async` and `http-async`, not `-ng` (2026-09-05)

Piotr's decision. `-ng` only says that something is newer; `-async` says what it
is and how it differs. The previous names in this file were renamed:

- `fcgi-ng` -> **`fcgi-async`** — an experimental FastCGI pool type for real
  asynchronous PHP. The old `fcgi` is unchanged.
- `http-direct` -> **`http-async`** — the variant where HTTP lives in the worker
  with an event loop, without a gateway and without a FastCGI hop.

`http-async` is also more accurate than `http-direct`: the important property is
not "directness", but that the worker has an event loop. Without async, this
variant makes no sense (it loses queuing and pins keep-alive connections to
workers); with async, it does.

The product name `php-fpm-ng` is unchanged for now; that is a separate matter,
see section 9 (trademark).


## 3o. `pool.type = supervisor` — implemented and verified (2026-09-05)

**Amended 2026-09-13 (issue #122).** The restart contract below is unchanged —
an exit 0 under `restart = always` still starts the next run with no delay —
but a script that returns instead of looping now gets one WARNING saying so.
Nothing is throttled and no directive was added; see
[`supervisor.md`](supervisor.md).

New files `sapi/fpmng/fpm/fpm_pool_supervisor.c` + `.h`, one line in
`fpm_pool_types[]` (`fpm_pool_type.c`), and directives in `fpm_conf.c`/`fpm_conf.h`.
Five directives from the task (`supervisor.script`, `.processes`, `.restart`,
backoff/`.restart_max`, `.stop_timeout`) plus a sixth added during the work by
the coordinator: `supervisor.fatal`. All were verified on a built binary (10
scenarios, below).

### Decision: `supervisor.processes` → `pm = static` + `pm.max_children`

Resolved in favor of this design. `fpm_pool_type_s.validate()` for this type
sets `wp->config->pm = PM_STYLE_STATIC` and `pm_max_children = supervisor.processes`
**programmatically**, before `fpm_conf_process_all_pools()` reaches the
`requires_pm` checks — so those checks pass trivially, and the user never sets
`pm`/`pm.*` themselves (`rejects` rejects them, see below). The result is that
spawning N processes and respawning after `exit()`/a crash is **free** through
the existing `fpm_children.c` — no custom process pool.

The consequence is `requires_pm = 1`, not `0` as the task sketch suggested —
"requires a meaningful pm" is true; this pool simply generates that value
itself instead of reading it from configuration.

### `rejects`: reject all of `pm`/`pm.` and `listen`/`listen.`

Because `pm`/`pm.max_children` are generated from `supervisor.processes`,
letting the user set them too would create two sources of truth for the same
number — so reject them **entirely** (`"pm"` and the `"pm."` prefix), not just
the individual fields named in the task. Likewise reject all of `listen`/`listen.`
(this type does not listen), all of `ping.`/`access.` (without `listen` there is
nothing to ping or log as "access"), plus the individual task entries
(`request_terminate_timeout(_track_finished)`, `request_slowlog_timeout`,
`request_slowlog_trace_depth`, `slowlog`, `security.limit_extensions`).

**The `rejects` mechanism was verified — it worked immediately, without fixes.**
`fpm_pool_type_check_directives()` (in `fpm_pool_type.c`) was correct; the only
missing piece was a real consumer (until then it was tested with zero directives,
see 3i). Tests 8/8b below show a clear ALERT at startup when a rejected
directive is present, exactly as designed.

### DESIGN PROBLEM (a) versus (b): resolved in favor of (a), as preferred

Implemented (a): shared memory per pool (`fpm_shm_alloc`, the same pattern as
`fpm_http.c`), with an `fpm_supervisor_shared_s` structure (`failures`,
`next_allowed_start`, `terminal`, `gave_up`, `fatal_signaled`). `fpm_children.c`
is **untouched**. After starting, the child checks the state: if `terminal` is
already set (the previous process for this pool already decided "enough") it
parks itself (`pause()` in a loop until signalled) instead of doing anything.
Verified: test 4 (`restart = never`) demonstrates exactly this mechanism.

**Important correction to the task description:** a process respawned by
`fpm_children.c` after `exit()` **does not disappear** — it remains as a harmless
parked process (exactly as problem (a) predicted in the task description:
"processes exist and sleep instead of disappearing"). Test 4 shows this directly
in `ps`: after the script started with `restart = never` finishes, one
`pool sup` process remains, but it is a **new, parked** process (a different
PID), not the original. The script itself **is not run again** (the script log
has exactly one entry) — this is the guarantee (a) can provide without changing
`fpm_children.c`.

### Not "one PHP instance per script run" — a loop INSIDE the process

Clarification to the sketch in section 3 ("the child runs the script" in the
singular): supervisor does **not** exit the process after every script run.
`child_main()` calls `php_request_startup()` /
`php_fopen_primary_script()` / `php_execute_script()` /
`php_request_shutdown()` **in a C loop, in the same process**, as long as the
`restart` policy says to keep trying. The process exits (deliberately, through
`exit()`) only when the policy says "stop for good" (`never` after one run,
`on-failure` after success, `restart_max` exhausted), or when SIGTERM arrives
and the current script run has finished. This matches the task's wording ("give
it internal loop logic around script executions"), but was not obvious from the
section 3 description above — that description ("the child runs the script")
suggested "one process = one run", which turned out not to match the wording of
the task. The section 3 bonus (forking a master with PHP ready) therefore works
once for the entire process lifetime, not on every iteration — iterations in
the same process are even cheaper because they do not pay for `fork()`.

### Backoff: what counts as a "failure"

Decided: **only `exit_code != 0` counts as a failure** for backoff and
`restart_max`. The first version also counted successful exits (because it only
measured "did the process live briefly"), which under `restart = always` with a
short but completely healthy script (the typical consumer: receive one job and
return) would classify normal work as "flapping" and kill a healthy pool after
`restart_max` cycles. Fixed: `exit_code == 0` always resets the counter and starts
the next iteration **immediately** (no artificial throttling — the script itself,
for example through `sleep()`, controls the pace). Only `exit_code != 0` enters
backoff logic (`restart_delay`, doubling up to `restart_delay_max`) and counts
toward `restart_max`. The reset threshold after a long-lived failure is
`restart_delay_max` (if the process lived longer than the longest possible gap
between attempts, it was not a "rapid death" — one unlucky failure after weeks
of work should not count against the same limit as a real crash loop).
`supervisor.restart_max` defaults to `0` = no limit (never give up); the task had
no explicit default.

### `supervisor.fatal` (added by the coordinator during the work)

`supervisor.fatal = no` (default) | `yes`. When a pool gives up because of a
**real failure** (`restart_max` exhausted, `shared->gave_up = 1`) and `fatal = yes`:
log ALERT and `kill(fpm_globals.parent_pid, SIGTERM)` the master — the same as
ordinary signal shutdown of the master, so the remaining pools finish requests
and clean up through the EXISTING machinery (`fpm_signals.c`,
`fpm_process_ctl.c`, untouched). The master's exit code must be != 0 (otherwise
Docker/k8s will not restart the container) — one additional hook is needed:
`fpm_cleanup_add(FPM_CLEANUP_PARENT_EXIT_MAIN, ...)` (the same mechanism already
used for HTTP gateway cleanup), called just before `exit(FPM_EXIT_OK)` in
`fpm_pctl_exit()` (`fpm_process_ctl.c`, untouched). The callback checks
`shared->gave_up && supervisor.fatal` for all supervisor pools and, if true,
does `_exit(FPM_EXIT_SOFTWARE)` (70) **before** `fpm_pctl_exit()` can call its
`exit(FPM_EXIT_OK)`. The distinction between a "planned exit"
(`restart = never` / success under `on-failure`, `gave_up = 0`) and a "real
failure" (`gave_up = 1`) is explicit in the code (`shared->gave_up`, not merely
`shared->terminal`) — test 10 shows that `restart = never` + `fatal = yes` with
a script exiting zero **does not** kill the master.

### Trap #1 (serious): `child_main` had never been called before — `fpm_worker_all_pools` dies before `run_child:`

This was the biggest problem in the task, in **other people's code** that
already existed in `fpm.c`/`fpm_pool_type.c`. The
`fpm_pool_type_current_pool()` mechanism (matching through the scoreboard) was
described in 3i as "implemented and compiled", but **`http` does not use it**
(the gateway forks its own processes directly in `init_main()`, not through
`child_main`) — so `supervisor` was the first real `child_main` consumer, and
the mechanism proved broken.

The cause: `fpm_worker_pool_init_main()` (`fpm_worker_pool.c`, a reference
file) registers `fpm_worker_pool_cleanup()` for `FPM_CLEANUP_ALL`, including
`FPM_CLEANUP_CHILD`. That function **frees the entire `fpm_worker_all_pools`
list**, including `wp->config` (`free()`), and finally sets
`fpm_worker_all_pools = NULL`. `fpm.c` calls
`fpm_cleanups_run(FPM_CLEANUP_CHILD)` **at the very start** of the `run_child:`
label, **before** trying to find the pool type through
`fpm_pool_type_current_pool()`. By the time our code tried to read
`wp->config->supervisor_script`, `wp` no longer existed (use-after-free).
This is invisible for an ordinary FastCGI worker because after this point the
code never looks at `wp`/`config` again; it uses only `fpm_globals`.

Fixed in `fpm.c` (our file, so this is allowed): type resolution
(`fpm_pool_type_current_pool()` + `fpm_pool_type_of()`) was moved **before**
`fpm_cleanups_run(FPM_CLEANUP_CHILD)`, and for a type with `child_main` set,
**the entire `fpm_cleanups_run(FPM_CLEANUP_CHILD)` is skipped** — because
`child_main` does not return anyway, while `wp`/`config` are needed for the
whole process lifetime. This changes `fpm.c` beyond "`fpm_conf.c/.h` + a new
file"; it is recorded here because the task instructions require it. Without
it, supervisor (and every future type with `child_main`, such as `cron`) would
not start at all: the process exited immediately (0.001s), without running the
script, in a hot fork-exit-fork loop (visible in the log as dozens of "child
exited with code 0" messages per second), because it returned to the ordinary
FastCGI accept loop on descriptor 0 (a duplicate of stdin, since
`requires_listen = 0`), which immediately failed.

### Trap #2: PHP itself uses `SIGALRM`/`ITIMER_REAL` for `max_execution_time`

The first `stop_timeout` version used `alarm()` plus its own `SIGALRM` handler
as a safety net (a hard `SIGKILL` if the script did not finish within
`stop_timeout`). Live testing showed that this conflicts with
`zend_set_timeout_ex()` (`Zend/zend_execute_API.c`), which **also** uses
`SIGALRM`/`setitimer(ITIMER_REAL,...)` for `max_execution_time`, and under
`ZEND_SIGNALS` (this build has `-DZEND_SIGNALS`) reinstalls its handler on every
script execution. Our `sigaction(SIGALRM,...)` (called once at process startup)
was silently replaced. In the test, the process waited for PHP's
`max_execution_time` ("Maximum execution time of 30 seconds exceeded"), not for
our `stop_timeout`.

Fixed: `stop_timeout` **does not use any PHP signal or timer**. The `SIGTERM`
handler forks a small watchdog process (`fork()` is async-signal-safe), which
waits for `stop_timeout` in a **separate process**, completely independent of
Zend's signal state, and kills the supervisor if it is still alive. Verified
live (test 7c, a busy loop with no safe point): killed exactly after
`stop_timeout` by a signal, not by the script ending naturally.

**PID race — closed on Linux (2026-09-05, coordinator addition).** The first
version identified the process only by the PID saved at `fork()` time: the
watchdog slept for `stop_timeout` seconds, then ran `kill(pid, SIGKILL)`. The
theoretical race was that the supervisor could die and its PID be reused by
another process before the watchdog woke up; the watchdog would then kill the
wrong process — unacceptable in a product responsible for other people's
processes (Docker/k8s).

Fixed with `pidfd_open()`/`pidfd_send_signal()` (Linux, kernel ≥5.3/5.1 — exactly
the target platform: Alpine containers). The key is opening the pidfd on
itself **just before the watchdog `fork()`**, while "I" is still unambiguous
(the process has just received SIGTERM and is certainly still alive), so opening
itself has no race window. The watchdog inherits the descriptor through `fork()`
and waits on it through `poll()` instead of blindly sleeping: a pidfd refers to
a **specific process instance**, regardless of what later happens to that PID
number. Thus a timeout in `poll()` is unambiguous proof that "this is still the
same process, still alive", and only then does it send `SIGKILL` (through
`pidfd_send_signal`, so even the final shot does not use a bare PID).

Syscall numbers (`SYS_pidfd_open` = 434, `SYS_pidfd_send_signal` = 424) are hard-coded — not every libc (musl, older glibc) wraps them yet, and direct `syscall()` is stable enough (the numbers do not change between x86_64 and aarch64).

On non-Linux (this Mac — local builds and tests only, never the target platform),
`pidfd_open` does not exist: the old fallback `sleep()` + `kill(pid, ...)` remains,
with the same narrow, documented race window. Only this fallback branch was
verified (macOS cannot test the `pidfd` path because the kernel has no such API).
Test 7c was repeated after the change: behavior was identical to before (the
busy loop was killed with `SIGKILL` exactly after `stop_timeout`).

### Another detail: `catch_workers_output`

Without `catch_workers_output = yes`, supervisor **works correctly but produces
no logs** — `echo`/`error_log`/our own `zlog()` from the child process go to
`/dev/null` (FPM's default behavior for child stdout/stderr when the directive
is disabled, which is the default). This is not supervisor-specific, but it is
critical for this type (the only way to see anything from the script) — recommend
`catch_workers_output = yes` as practically "required" for
`pool.type = supervisor` in user documentation (not enforced in code, to avoid
adding another validation rule without a clear need).

**Superseded 2026-09-09 (issue #121).** The paragraph above stays because it
records what the type did for most of its life, but the recommendation no longer
holds: a pool type that sets `fpm_pool_type_s.child_logs_via_master` (supervisor
and cron) now gets one `AF_UNIX SOCK_DGRAM` socketpair per pool, and the child's
`zlog()` is relayed through it and re-emitted by the master at its own level
(`sapi/fpmng/fpm/fpm_child_log.c`, `fpm_child_log.h` for the why). Measured on
the test box with no `catch_workers_output` anywhere: a supervisor whose script
exits 3 logs `NOTICE: [pool sup] supervisor: script exited (code 3) after 0s,
restarting in 30s (failure 1/unlimited)`, and `restart_max` /
`supervisor.fatal` / `cannot open script` arrive as ALERT/ALERT/ERROR. What the
directive still does, and all it now does for these pools, is carry what the
**script itself** writes to stdout/stderr — including PHP's own error output,
which does not go through `zlog()` and therefore still needs it.

**Amended 2026-09-09 (issue #124).** The last clause above no longer holds
either: PHP's own error output now goes through `zlog()` as well for these two
types. A child with `child_logs_via_master` starts with `log_errors = 1`,
`display_errors = 0` and `html_errors = 0` — applied before
`fpm_php_init_child()`, so the pool's own `php_value` / `php_admin_value` still
override them, while `php.ini` does not: the call that applies them writes the
ini entry's master value and cannot tell a compiled-in default from something
`php.ini` set (`fpm_child_php_log.h` records why that trade is the right one for
a pool type with no response to display an error in) —
and `sapi_module.log_message` is replaced so that the message reaches `zlog()`
at a level derived from the error's severity instead of upstream's fixed
`ZLOG_NOTICE` (`sapi/fpmng/fpm/fpm_child_php_log.c`). Measured on the test box
with no `catch_workers_output` anywhere: a supervisor script calling an
undefined function, which used to log **nothing at all**, now produces one
entry, `ERROR: [pool fatal] PHP message: PHP Fatal error:  Uncaught Error: Call
to undefined function no_such_function_here() in /.../fatal.php:2` plus its
stack trace, and a `trigger_error(..., E_USER_WARNING)` arrives as `WARNING`.
`catch_workers_output` is now only about what the script writes to
stdout/stderr on purpose; `php_admin_value[error_log]` still wins over all of
this, because `php_log_err()` then never calls the SAPI.

**Amended 2026-09-09 (issue #126).** The other half of "the script writes on
purpose": `STDIN`, `STDOUT` and `STDERR` are registered for every run of a
script-running pool type, so the first thing an author reaches for —
`fwrite(STDERR, ...)` — is no longer `Uncaught Error: Undefined constant`. They
are the CLI SAPI's constants, and nothing else in an FPM process registers
them. Where they point is entirely decided by FPM and not by this change:
stdin is the `/dev/null` `fpm_stdio_init_main()` installs, so `STDIN` reads EOF
at once, and stdout/stderr are the master's pipes under
`catch_workers_output` or that same `/dev/null` without it.

Not a copy of CLI's `cli_register_file_handles()`: CLI keeps the handles past
request shutdown (`PHP_STREAM_FLAG_NO_RSCR_DTOR_CLOSE`) for extensions that
write to stderr during `MSHUTDOWN`, which it can afford because it runs one
request per process. A supervisor process runs one per iteration, and outside
CLI `php://std*` **dup()s** the descriptor
(`ext/standard/php_fopen_wrapper.c`), so preserving the handle would leak three
descriptors per run — at the 12086 runs/s of issue #122, the fd limit in under
a second. The streams are therefore ordinary request-scoped ones and the
constants are re-registered per run. Measured on the test box over 100
consecutive runs of one supervisor process: 16–17 open descriptors, no trend
(`sapi/fpmng/tests/fpmng-supervisor-std-streams.phpt` asserts the band).

**Amended 2026-09-10 (issue #73).** The same three constants now exist for
`pool.executor = worker`, and the registration itself moved to
`sapi/fpmng/fpm/fpm_std_streams.c` so there is one definition of "what CLI
gives a script" rather than a copy per process type. A worker is the second
place in this SAPI where a PHP script owns the process instead of a request,
and it had the same hole with a sharper edge: both example bridges report a
failed handler with `fwrite(STDERR, ...)` from *inside* their `catch`, so the
`Undefined constant` fired on the one path whose job is to swallow the
failure. In `examples/http-direct-worker/FpmngServer.php` that `Error` escaped
the `Amp\async()` fiber into Revolt's uncaught-throwable handler, which
rethrows out of `EventLoop::run()`: one failing request handler killed the
worker instead of logging a line.

The route was already there and only the constant was missing —
`fpm_worker_ub_write()` has always written worker output to `STDERR_FILENO`,
which is why `echo` in a worker already reached the log. A dedicated
`fpmng_worker_log()` builtin going straight to `zlog()` was the alternative
and was rejected: it would have needed edits to both bridges and left every
third-party bridge written against the documented `STDERR` idiom still broken,
while the two things it would have added — a severity, and independence from
`catch_workers_output` — are what `error_log()` already gives in this SAPI
(issue #124). The decision is recorded in full on issue #73.

Registered once per worker, not per request, and that is the *same* rule as
above rather than an exception to it: the fd-leak argument that forces
request-scoped streams exists because a script-running pool starts a request
per iteration, and a worker has exactly one request spanning its whole life,
so it dups three descriptors once.

### Monkey-patching `sapi_module` for the process lifetime — safe because the process does not return

`child_main` overwrites several global `sapi_module` fields (`ub_write`,
`getenv`, `read_post`, `read_cookies`, `register_server_variables`,
`pre_request_init = NULL`) — the original versions in `fpm_main.c` unconditionally
cast `SG(server_context)` to `fcgi_request*`, while supervisor never has a real
FastCGI request (`SG(server_context)` remains `NULL` for the whole process
lifetime), so without the overwrite the first `echo` in the script would
segfault. This is safe only because this process **never returns** to the
FastCGI accept loop — if it did, these overwrites would break ordinary request
handling. `ub_write` writes directly to `STDOUT_FILENO`, which reaches the FPM
log through the existing pipe capture (hence the `catch_workers_output`
requirement above).

### Tests (10 scenarios, all green)

1. No `pool.type` → ordinary `fcgi` pool, full BC (also verified with a real
   FastCGI request over UDS using a manual Python client — `X-Powered-By` and the
   script body).
2. `pool.type = supervisor` + `supervisor.script` — the script really executes
   (a log file with timestamp and PID), in a loop, in the same process.
3. `supervisor.processes = 3` → three real `php-fpm: pool sup` processes in `ps`.
4. `restart = never` — the script runs exactly once (one log entry), the process
   exits; a **new, parked** process takes its place (see the (a) section above),
   but the script does not repeat.
5. `restart = on-failure` — `exit(0)` does not restart (log: "not restarting"),
   `exit(1)` restarts with backoff, in the same process (same PID).
6. Rapid `exit(1)` in a loop — backoff grows geometrically (1s, 2s, 4s,
   capped at `restart_delay_max`), then `restart_max` produces an ALERT and
   permanently stops attempts (one parked process instead of a crash loop).
7. SIGTERM during script execution (a loop with `usleep`) — the script finishes
   the current run (all 10 "ticks"), the process exits cleanly (`exited with
   code 0`), and is NOT respawned for the same script in the same sense (a new
   process starts because this is a normal exit, not the "stop for good" policy).
   Separately (7c): a busy loop with no safe point + `stop_timeout = 2` is killed
   with `SIGKILL` after about 2s (it does not finish by itself after 15s).
8. `rejects` — `listen` and `pm.*` (`pm`, `pm.start_servers`,
   `pm.min_spare_servers`, `pm.max_spare_servers`) in a `supervisor` pool
   configuration produce a clear ALERT and `FPM initialization failed` (exit
   78); the mechanism worked without fixes.
9. `supervisor.fatal = yes` + a rapid crash loop → after `restart_max` is
   exhausted: ALERT, the master follows the normal path ("Terminating ...",
   "exiting, bye-bye!"), **`echo $? == 70`**.
10. `restart = never` + `fatal = yes` with a script that exits zero — the master
    **does not** die (the "sup" process parks, and the master stays alive).

### Not done / uncertain

- The `stop_timeout` watchdog has the theoretical PID race described above.
- `pm.max_requests` (the section 3 semantics: "the script exits after N jobs")
  is **not implemented** — it was not among the five required functions, and
  `pm.*` is now rejected entirely for this type, so it would need a custom
  directive such as `supervisor.max_iterations` if wanted.
- Supervisor status in `fpm_status.c` (the type marker mentioned in section 3)
  was not touched; it is outside this task.
- Coexistence of multiple `supervisor` pools next to `fcgi`/`http` in one master
  during reload/SIGHUP — **tested, see section 3p** (coordinator addition,
  2026-09-05).
- `security.limit_extensions` and other security directives have no dedicated
  test beyond rejection during configuration — for example, we did not check
  whether rejection breaks anything in `fpm_unix.c` (it should not, since it is
  only a config string, but this was not explicitly verified).

## 3p. `supervisor` across reload and mixed pools — verified (2026-09-05)

Coordinator addition after 3o: historically, HTTP gateways registered cleanup
only for `FPM_CLEANUP_PARENT`, while reload uses `FPM_CLEANUP_PARENT_EXEC`
(`fpm_pctl_exec()` calls `execvp()`) — so they became orphans and held the port
(fixed for `http`, see 3i). The same question had not previously been checked
for `supervisor`; it was checked now.

**Test configuration**: one `http` pool, one ordinary `fcgi` pool, one
`supervisor` with `restart = always` (script: 6 ticks at 0.4s, about 2.4s per
iteration, logging to a file), and one `supervisor` with `restart = never` (the
script calls `exit(0)` immediately, so the pool is already parked before the
signal test — no special wait is needed; it is the natural effect of
`restart = never`).

### Scenario 1: `SIGUSR2` (reload, `execvp()`)

```
BEFORE: pgrep -P 90726
90728 90729 90730 90732   # web, plainfcgi, sup_always, sup_parked

kill -USR2 90726
[...] NOTICE: Reloading in progress ...
[...] NOTICE: reloading: execvp("php-fpm-ng", {"-y", "reload_test.conf", "-F", "-O"})
[...] NOTICE: using inherited socket fd=8, ".../reload_web.sock"
[...] NOTICE: using inherited socket fd=9, ".../reload_fcgi.sock"
[...] NOTICE: fpm is running, pid 90726          # <- SAME PID (execvp does not change the PID)
[...] NOTICE: ready to handle connections

AFTER (t+3s): master pid=90726 (the same)
children of the new master: 90739 90740 90741 90743
all pool sup* processes:
90741 90726 php-fpm: pool sup_always
90743 90726 php-fpm: pool sup_parked
```

**No orphans** — both old supervisor instances disappeared, and both new ones
have the correct `PPID` (the new/same master). The `reload_always.log` shows that
the old process (pid 90730) finished ITS current iteration ("tick 0".."tick 5",
"iter end") **before** the actual `execvp()` at 17:44:33 — the signal arrived,
`child_main` reacted, and the new iteration (pid 90741) started only after reload:

```
2026-09-05T15:44:31+00:00 reload_always iter start pid=90730
2026-09-05T15:44:31+00:00 reload_always tick 0
...
2026-09-05T15:44:33+00:00 reload_always tick 5
2026-09-05T15:44:33+00:00 reload_always iter end
2026-09-05T15:44:33+00:00 reload_always iter start pid=90741   # new generation, after reload
```

**Shared memory (backoff) after `execvp()`**: `fpm_shm_alloc()` uses
`mmap(MAP_ANONYMOUS | MAP_SHARED)` — this mapping **does not survive `execve()`**
(POSIX: the entire address space except open descriptors disappears at exec).
This is not a matter of inheriting it correctly or incorrectly — it is
physically impossible to inherit, so "does the failure counter make sense after
a reload?" has a simple answer: **the new master instance allocates a new
`fpm_shm_alloc()` segment from scratch anyway** (in its own
`fpm_init()`/`fpm_pool_type_of()->init_main()`), so backoff counters naturally
reset on reload. This is consistent with the rest of FPM: reload is a new
process instance, not a continuation of the old one.

### Scenario 2: `SIGQUIT` (graceful stop, no exec)

```
kill -QUIT 90858
[...] NOTICE: Finishing ...
[...] NOTICE: exiting, bye-bye!
master exited after 0s (sampled every 0.3s — very fast)

reload_always.log (end):
...tick 4
...tick 5
...iter end        # <- finished the current iteration before exiting
```

Immediately after the master exited, `ps` briefly showed two processes with
`PPID=1` (`pool sup_parked`, `pool sup_always`) — these were our own
`stop_timeout` watchdog processes (see 3o). On this Mac (no `pidfd`) they wait
in a loop checking `kill(pid,0)` once per second instead of reacting to a
signal, and disappeared on their own within about 1–2 seconds. This is not a
real orphan in the sense of "remaining forever", but a transient artifact of
our own watchdog mechanism, documented and accepted in 3o.

### Scenario 3: `SIGTERM` (fast shutdown)

```
kill -TERM 91009
[...] NOTICE: Terminating ...
[...] NOTICE: exiting, bye-bye!
master exited after ~2s

reload_always.log (end):
...iter start pid=91013
...tick 0
...tick 1
...tick 2
...tick 3
...tick 4          # <- NO "tick 5"/"iter end" — the process did not finish in time
```

**The difference from `SIGQUIT`/`SIGUSR2`, worth recording**: in the
`TERMINATING` state, `fpm_pctl_action_next()` (`fpm_process_ctl.c`, a reference
file) sends `SIGTERM` as the **first** signal (not `SIGQUIT`), and with the
default `process_control_timeout = 0` escalates to `SIGKILL` after about one
second if the child is still alive. This is behavior of **the entire FPM master,
not something introduced by `supervisor`**, and applies to every pool type (an
ordinary FastCGI worker in a long request dies the same way). Our own
`supervisor.stop_timeout` (10s by default) is irrelevant here because the
**master**, not our watchdog, kills the process first. The script (2.4s per
iteration) did not finish its iteration with the default
`process_control_timeout`. No orphan remained (`ps` was clean immediately after
the master exited) — the process was collected correctly; only one `child exited`
line for that PID was missing from the log (probably because the master's event
loop ended before it logged the reap for that particular SIGCHLD — cosmetic log
ordering, verified with `ps` showing that the process really disappeared rather
than becoming an orphan).

**User conclusion**: anyone who wants `supervisor` (or any other pool type) to
have time to finish work during `SIGTERM`/`docker stop` must set
`process_control_timeout` globally to a sensible value — it is an existing FPM
directive, not something to add. This belongs in future user documentation, not
only here.

### Related fix: the `stop_timeout` fallback without `pidfd` shortened the orphan window

The first version of the test (before the fix later that day) showed orphans
remaining for the full `stop_timeout` (10s by default) after reload, because the
fallback watchdog (without `pidfd`, on this Mac) did one long
`sleep(stop_timeout)` instead of checking once per second whether the supervised
process had already finished. Fixed (see commit `2a4da50`/the following one):
the fallback watchdog calls `kill(pid, 0)` once per second and exits immediately
when the supervised process is gone, rather than blindly waiting for the whole
`stop_timeout`. After the fix, scenario 1 (USR2) shows zero orphans at `t+3s`
(previously two orphans were still visible at `t+3s` and disappeared only at
`t+12s`).

## 3q. Async: epoll versus io_uring measurements and traps (agent, 2026-09-05)

CAVEAT: the agent measured **with its own C prototype** simulating a FastCGI
server with an event loop. It did not test the actual True Async implementation;
semantic consequences were inferred by reading `fpm_scoreboard.c`,
`fpm_request.c`, `ZendAccelerator.c`, `zend_signal.c`, and `main.c`. The
epoll/io_uring numbers are measured; conclusions about what disappears under
async are conclusions from FPM/Zend code, not from async itself. A separate
agent is investigating True Async at the source — see the section that will be
created from its report.

### io_uring also loses under async, but for a different reason

In the blocking model there was nothing to batch. In the event-loop model,
batching works — syscalls fall from ~2.0/request to 0.08–0.2/request (10–25x) —
but **CPU falls by only 0.3 µs/request** (UDS, saturated), 0.2 µs (TCP), and
0.7–1.15 µs for rare events. io_uring removes transitions, not kernel work, and
its own overhead (task work, CQE, buffer ring) consumes most of the saving.

At 2 ms CPU/request, it takes **17–67 I/O operations per request** to reach 1%.
For PHP there is no level of concurrency N at which this rises above the noise.
io_uring becomes relevant only when `fcgi-async` stops being PHP and becomes a
proxy with dozens of I/O operations for a microscopic request.

**Practical nail in the coffin: Docker blocks `io_uring_*` in seccomp by default.**
It also needs liburing and kernel ≥ 6.0 for multishot recv, and may be disabled
by `io_uring_disabled`. Our target environment is a container.

### The real win is the EVENT LOOP, not the I/O API

After optimization, a blocking worker costs 22–23 µs/request (UDS). The async
epoll prototype costs **3.4 µs/request** (6.3 TCP). The roughly 10 µs difference
is the cost of the "process sleeps in `read` and wakes per request" model — a
context switch plus cold caches. It disappears in an event loop **regardless of
io_uring**. Therefore the libevent we already have is sufficient.

### THREE EARLIER RECOMMENDATIONS BECOME BUGS UNDER ASYNC

Not merely useless — **wrong**. Mark these before anyone implements them from
the list in section 3m:

1. **CWD cache in the SAPI** — CWD is per process. Two interleaved requests from
   different docroots mean one runs with the other's CWD (relative `include`,
   `fopen`). Under async **every per-request `chdir` is a bug**; a virtual CWD
   per context is needed, and NTS does not have one. This belongs in Zend/TSRM.
2. **`times()` opt-in** — it measures **process** CPU, so "request CPU" would be
   the sum of other requests. Per-coroutine CPU needs
   `CLOCK_THREAD_CPUTIME_ID` on every switch — **1.1 µs/syscall** (measured, not
   vDSO); at 10 switches that is 11 µs, more than all current savings.
3. **`SO_RCVTIMEO` instead of `poll`** — it does not work on nonblocking sockets
   (`recv` returns EAGAIN immediately). Timeouts must be event-loop timers.

### Three traps that were on no list

- **Scoreboard**: the `proc` slot is per process, while `request_uri`,
  `request_stage`, and `accepted` describe ONE request. Under async, status and
  slowlog lie, and `request_terminate_timeout` (the master checks `proc->tv` and
  `request_stage`) **kills a process with N requests because of one**.
- **OPcache lock**: `accel_activate_add` takes F_RDLCK per request;
  `accel_deactivate_sub` from the first request to finish **would release the
  lock while others were still running**. Hold it once per process with a counter.
- **The concept of an "idle worker"** disappears. `fpm_request_end`,
  `fpm_stdio_flush_child`, and `fpm_log_write` assume "request finished = process
  idle", and `pm = dynamic/ondemand` relies on that. Redefine it as in-flight
  requests below the limit.

### `max_execution_time` and `zend_signal_activate`

Per-request `setitimer(ITIMER_PROF)` is **impossible by definition** under async
— one timer per process. It disappears by itself, but a replacement is needed:
- **bad**: `CLOCK_THREAD_CPUTIME_ID` on every coroutine switch (1.1 µs syscall,
  10–50 switches = 11–55 µs — more expensive than all of today's 26 syscalls);
- **good**: a wall-clock deadline checked with `CLOCK_MONOTONIC_COARSE`
  (7 ns, vDSO), plus one event-loop timer for the nearest deadline.

Per-request `zend_signal_activate` (7x `rt_sigaction`): handlers are per process,
so registering once per process becomes **necessary, not an optimization**. The
deeper problem is that deferring Zend signals (`SIGG(depth)`,
`HANDLE_BLOCK_INTERRUPTIONS`) assumes one execution thread — switching a
coroutine in a critical section leaves the counter suspended. This is Zend, not
SAPI.

**Net: 12 of today's 26 syscalls disappear under async for structural reasons**,
not optimization reasons.

### What remains from the floor

Of about 21–23 µs (UDS), about 12 µs is specific to the "one request per process"
model and falls to about 3–4 µs in the event loop (measured by the prototype);
about 9 µs is user-space PHP and FPM that remains. Target transport + lifecycle
floor under async: **about 12–14 µs/request**, or 0.6% at 2 ms request CPU.

**Conclusion: the thing that will actually change CPU/request under async is the
cost of the per-coroutine PHP context (startup/shutdown, heap, superglobals) —
and that lives in Zend, not the SAPI.** The entire transport layer is below 1%.

Artifacts: `~/ng-research/async/` on the test host.


## 3r. `pool.type = cron` — implemented and verified (2026-09-05)

New files: `fpm_pool_cron.c`/`.h` (the type itself),
`fpm_cron_schedule.c`/`.h` (the crontab parser, with no dependency beyond
libc), plus two files extracted from the common supervisor code:
`fpm_pool_watchdog.c`/`.h` (the pidfd watchdog) and
`fpm_pool_script.c`/`.h` (running one PHP script outside a FastCGI request,
with `sapi_module` overrides). One line in `fpm_pool_types[]`
(`fpm_pool_type.c`), and three directives in `fpm_conf.c`/`fpm_conf.h`
(`cron.schedule`, `cron.script`, `cron.timeout`).
`fpm_pool_supervisor.c` was rewritten to use these two shared files instead of
keeping its own copies — no behavior change, only duplication removed (see below).

### Key simplifying decision: no master-side timers

As decided before the code (section 3), after starting the `cron` child,
it calculates the next due time, sleeps interruptibly until then, runs the script
ONCE, and exits. FPM respawns it through the existing machinery
(`fpm_children.c`, UNTOUCHED), and the new process calculates the next due time
from the current clock. `fpm_events.c` is also untouched — cron does not use
`fpm_event_set_timer()`, contrary to the earlier sketch in section 3 ("timer +
fork"). This is a different and simpler mechanism than that sketch, and simpler
than supervisor.

### Difference from supervisor: ZERO shared-memory state

This is the largest design difference from supervisor, and worth recording
explicitly because it is not obvious. Supervisor needs `fpm_shm_alloc()` because
its restart/backoff/`restart_max` policy must survive process death. Cron **has no
policy that must survive**: every new process calculates the due time ONLY from
the current clock and schedule, never from what the previous process did. Thus
there is no `init_main` for `cron` in `fpm_pool_types[]` (the field stays `NULL` —
"nothing to do on the master side", as required by the contract in
`fpm_pool_type.h`).

The same decision has another consequence: **"overlapping runs" is not a
policy that had to be written.** There is deliberately no `cron.processes`
kind of directive — cron always has `pm.max_children = 1`, set programmatically
in `validate()`, just as supervisor maps `supervisor.processes` to
`pm.max_children`. With `pm.max_children = 1`, a second process for this pool
physically does not exist until the first one exits (`exit()`) — `fpm_children.c`
starts the next one only AFTER the previous one dies. There is therefore no run
with which another run could overlap. The "skip overlaps" policy (planned in
section 3 as something to write) comes for free from `pm=static+1` and **was not
written at all**, exactly as the task predicted.

### Why "no catch-up" and "no double run" are the same line of code

`fpm_cron_schedule_next(sched, after)` always calculates the smallest time
strictly GREATER than `after`, starting at `((after / 60) + 1) * 60` (the start
of the next full minute). It never asks "what did I miss since the last run?" —
it always asks "what is the nearest future time from now?". The free side effects
are:
- master off for an hour → the new process calculates from the current clock
  and gets the nearest future time, not twelve overdue runs;
- a process respawned shortly after the previous one finished in the same minute
  (the script lasted a fraction of a second) never finds that minute again,
  because it searches strictly in the future.

Someone will eventually want to add catch-up for missed runs — that must be a
deliberate design change (change `after` to "the time of the last successful run",
stored in state that deliberately does not exist today), not a fix.

### Time: UTC only

`fpm_cron_schedule_next()` uses `gmtime_r()`, never `localtime_r()`. Local time
plus a DST change would produce a duplicate run (clock moves backward) or no run
(clock moves forward) on every transition. It is not worth taking that risk for
the convenience of writing schedules in local time — schedules copied from
system cron (which usually recommends UTC for servers anyway) behave identically.

### Crontab parser (`fpm_cron_schedule.c`) — scope and traps

Five fields (minute, hour, day of month, month, day of week), with no dependency
beyond libc. It supports `*`, `N`, `N-M`, `*/S`, `N-M/S`, comma-separated lists,
and the non-obvious but real crontab(5) form `N/S` WITHOUT a range (for example,
`10/15` means "from N to the end of the field's domain in steps of S", not one
value — otherwise schedules copied from system cron would change meaning). The
day of week is 0-7, with both 0 and 7 meaning Sunday (parsed in a temporary
0..7 bitmap, then 7 folded into 0). Shorthands: `@hourly`, `@daily`, `@weekly`,
`@monthly`, `@yearly` (only these five, as required — `@midnight`/`@annually`
are deliberately omitted as an easy addition later if needed).

**The OR rule for day of month and day of week** (classic, surprising cron):
when BOTH fields are restricted (neither is a literal `*`), matching is a UNION,
not an intersection. Literalness is checked on the whole field BEFORE splitting
on commas (`*/1` does NOT count as `*`, matching vixie-cron) — implemented and
verified by a separate program (see tests below): `0 0 13 * 5` matches every
Friday AND the 13th day of the month regardless of its weekday.

**A syntax error rejects the configuration at startup**, with a clear message
showing which field failed and why (`fpm_cron_schedule_parse()` fills the error
buffer passed by `validate()`, `validate()` logs `ZLOG_ALERT` and returns -1 —
the same mechanism as the rest of `fpm_conf.c`, with no special handling).
Verified: `* * * *` (too few fields), `99 * * * *` (out of range), and
`*/0 * * * *` (zero step) — all three are rejected at startup, exit 78, with no
attempt to interpret them "approximately".

`fpm_cron_schedule_next()` has a search limit (about 4 years in minutes) as a
last safety net against a schedule that can never structurally occur (for
example, `0 0 30 2 1` — February 30, with a restricted weekday, so the OR rule
does not help either). Startup validation checks syntax only, not "whether a
schedule can ever occur". There is no separate structural-feasibility
validation — it was judged not worth the complexity for a case that is already
an obvious operator error and will be noticed (the process refuses to start
with ALERT "schedule never matches").

### `cron.timeout` — exactly the same watchdog as `supervisor.stop_timeout`

Extracted into `fpm_pool_watchdog.c`: `fpm_pool_watchdog_arm(target_pid,
timeout_seconds)` forks a watchdog process that waits through `pidfd` (Linux)
or `kill(pid,0)` in a once-per-second loop (fallback, local builds/tests only)
until `target_pid` exits OR `timeout_seconds` elapses — if the timeout expires
while the process is still alive, send SIGKILL. For supervisor it is armed in the
SIGTERM handler (the `stop_timeout` safety net); for cron it is armed BEFORE
running the script (`cron.timeout`, 0 by default = no limit, in which case it
is not armed). The mechanism cancels itself: if the script finishes in time, the
process eventually calls `exit()`, `poll()` on the pidfd gets POLLIN (the process
exited), and the watchdog exits quietly without a signal — no separate "cancel
timeout" call is needed.

### Script execution — exactly the same code as supervisor, now shared

Extracted into `fpm_pool_script.c`: `fpm_pool_script_install_sapi_overrides()`
(overrides for `ub_write`/`getenv`/`read_post`/`read_cookies`/
`register_server_variables`, safe for the same reason as in supervisor — the
process never returns to the accept loop) and
`fpm_pool_script_run(pool_name, script_path)` (`php_request_startup` /
`php_fopen_primary_script` / `php_execute_script` / `php_request_shutdown`,
without FastCGI `SG(request_info)`). `fpm_pool_supervisor.c` was rewritten to use
these two functions instead of keeping its own copies — a clean refactor with
no behavior change (the same 10 scenarios from section 3o still pass; see
"verified after refactor" below).

### SIGTERM: simpler than supervisor because there is no "current iteration to watch while sleeping"

Unlike supervisor, cron's SIGTERM handler ONLY sets a flag (`cron_term_requested`);
it does not arm a watchdog. Nothing runs while sleeping — the flag wakes
`sleep()` (interrupted by every delivered signal for which a handler exists)
and the process exits immediately, before anything is forked. During script
EXECUTION, the time boundary is set by `cron.timeout` (a separate mechanism, see
above), not SIGTERM — the script finishes its current run naturally, like
supervisor (test 8 below). Ordinary master escalation (`process_control_timeout`,
see section 3p) applies to this type just as to every other type, with no
cron-specific code.

### `requires_pm = 0` (different from supervisor, where it is 1)

As described by the task. In practice this has no functional significance —
`validate()` sets `pm`/`pm_max_children` programmatically BEFORE the
`requires_pm` checks in `fpm_conf.c` (see section 3i, `fpm_conf.c:960-969`),
so those checks pass trivially regardless of the `requires_pm` value. It remains
as in the task because it is semantically more precise: cron does not "require"
`pm` in the sense that "the user must configure something", because the user
has nothing to configure here (`rejects` rejects it).

### `rejects`

`listen`, `listen.`, `pm`, `pm.`, `request_terminate_timeout(_track_finished)`,
`request_slowlog_timeout`, `request_slowlog_trace_depth`, `slowlog`, `ping.`,
`access.`, `security.limit_extensions`, `supervisor.` — a superset of the task
list, with the same reasoning as supervisor (section 3o): reject all of `pm`/
`pm.` (generated programmatically, always 1, otherwise two sources of truth),
all of `listen`/`listen.`/`ping.`/`access.` (no FastCGI requests), plus
`supervisor.` (directives for a SECOND pool type).

### Tests (9 task scenarios, all green, raw output in the report)

1. Configuration without `pool.type` (`pm = static`, ordinary `listen`) — `-t`
   is green, the process starts and exits without a trace (BC untouched).
2. `cron.schedule = * * * * *`, a script writing `gmdate()` + PID to a file —
   two consecutive runs at `16:07:00` and `16:08:00` UTC (exactly at the start
   of the minute), with different PIDs (the process exits and is respawned
   between runs, according to the "one process = one run" model).
3. A temporary test program (compiled separately, NOT part of production,
   removed after the test) for `*/15 * * * *`, `0 3 * * *`, `30 4 1,15 * *`,
   `0 0 * * 0`, and `@daily`, calculated from `2026-09-05 16:10:00 UTC` (Friday)
   — all due times were checked manually as correct (next quarter-hour, next
   03:00, next 1st or 15th day of the month at 04:30, next Sunday at midnight,
   and the same for `@daily`).
4. OR rule: `0 0 13 * 5` — the next 10 matches from 2026-09-01 alternate
   Fridays (any date) and the 13th day of the month (any weekday, including
   Sunday and Tuesday in successive months) — confirmed directly through
   `fpm_cron_schedule_next()`, not only through bitmaps.
5. Bad schedules `* * * *` / `99 * * * *` / `*/0 * * * *` — each rejected at
   startup (`ALERT` + `FPM initialization failed`, exit 78) with a clear message
   identifying the field and the reason.
6. `rejects` — a `cron` pool with `listen` or `pm.max_children` in its
   configuration produces a clear `ALERT` and `FPM initialization failed` (exit
   78).
7. `cron.timeout = 3`, a script with `sleep(20)` — the process is killed; the
   script log contains ONLY "starting sleep(20)", never "finished normally"; the
   next run (new PID) starts normally in the following minute.
8. SIGTERM while sleeping (sent directly to the child before the due time) —
   the process exits immediately, the script is NEVER run (empty log), and a new
   child respawned by `fpm_children.c` waits for the next due time. SIGTERM during
   a run (sent halfway through the 10-tick loop) — the script finishes ALL 10
   ticks and "run end" before the process exits (same behavior as supervisor,
   test 7 in 3o). Both scenarios leave zero orphans in `ps`.
9. Coexistence: one `http` pool (`pm = static`, 2 workers) + one `cron` pool in
   the same master — both start, cron runs normally while the HTTP pool remains
   ready. `SIGUSR2` (reload, `execvp()`) — same master PID, fresh children of
   both types (`web` x2 + `cronjob`), and zero processes with PPID=1 after reload.

**Verified after refactoring supervisor onto the shared files**: the tests above
ran on the same binary in which `fpm_pool_supervisor.c` already uses
`fpm_pool_watchdog.c`/`fpm_pool_script.c` instead of its own copies — clean build
(zero warnings about undefined symbols), while tests 7–9 (SIGTERM, timeout,
coexistence) indirectly exercise the same functions shared with supervisor. A
separate full run of the 10 scenarios from section 3o was NOT repeated in this
task (out of scope) — regression risk was judged low because the refactor is
mechanical (moving identical function bodies and changing only names and file
location), but this should be recorded explicitly under "not done".

### Not done / uncertain

- The full repeated run of the 10 supervisor tests from section 3o after the
  shared-file refactor (see above) — low risk, but not directly verified; only
  indirectly covered by cron tests using the same code.
- Cron status in `fpm_status.c` (the type marker, as for supervisor in section
  3o) was not touched; it is outside this task (see section 3j, metrics for
  requestless types are a separate task).
- Validation of "whether a schedule can ever structurally occur" (for example,
  February 30) — only a runtime search limit is used as a safety net, not a
  startup rejection. See above.
- `@midnight`/`@annually` as extra shorthand aliases — deliberately omitted; the
  task named only five shorthands.
- Catch-up for missed runs — deliberately NOT done, as required by the task (see
  the "no catch-up" section above). Someone may want it later; it must be a
  deliberate design change.


## 3s. Async in PHP — CLOSED, plus an `ext` option for C-parsed request data (2026-09-05)

The question was whether PHP could have a model where the worker yields to I/O
and the gateway feeds it another request in the meantime. Conclusion: **we are
not building this**, but the reasons are worth recording because someone (us)
will ask again in six months.

### Why not — three conditions must all hold at once

1. **Per-coroutine request state in the engine.** PHP has ONE complete request
   state per process, not per request. `php_request_startup()` /
   `php_request_shutdown()` set up and clean up globals: `$_SERVER`, headers,
   output buffers, the error handler, current ini, and the session. There is no
   "request object" on which this depends. Letting a second request in during
   the first always overwrites the first request's state, not "sometimes".
   After all the work so far, True Async has per-coroutine `ob_*` and hostent
   cache. That shows the scale of the work. STATUS: not available.

2. **I/O interception in drivers.** CORRECTION to the earlier note ("half
   the extensions will not work") — that is not true for the typical stack:
   - `mysqlnd` goes through `php_stream` → MySQL through PDO/mysqli is in the
     interceptable layer in the default build;
   - `phpredis` also uses streams. Predis (userland) even more so;
   - on the wrong side are `libpq` (`pdo_pgsql`), `libmysqlclient`, and `curl` —
     their own sockets, with no knowledge of the scheduler.
   So this is a short, concrete list, not an "ecosystem". STATUS: technically
   within reach, not done.
   CORRECTION (2026-09-05, after the question about a custom scheduler): this is
   within OUR OWN reach, without patching the engine. PHP has the public API
   `php_stream_xport_register()` — an extension can replace the `tcp` transport
   with its own (this is how ext/openssl works). Since mysqlnd and phpredis use
   streams, our `ext` could intercept their sockets and suspend the fiber instead
   of blocking. No patch and no php-src fork. It is feasible for us — and
   WORTHLESS by itself while condition 1 remains unresolved.

3. **Frameworks written to take advantage of it.** This is the condition that
   kills the idea because it does not depend on anyone we can influence.
   Fiber alone does nothing — it suspends execution only when a call AT THE
   BOTTOM yields control. `PDO::query()` enters `read()` and blocks the process
   regardless of whether it is called from a fiber.
   amphp works with amphp CLIENTS (`amphp/mysql`, `amphp/redis`), but Doctrine
   and Eloquent will not use them because their APIs are synchronous by design
   (`$user->posts` must return a collection, not a promise). This is not an
   adapter issue but an ORM-shape issue.
   The only real exception is `Symfony\HttpClient` (parallel requests through
   `curl_multi`) — but that is HTTP, not a database, and an island rather than
   a model. Laravel Octane is NOT async, only worker mode (the process lives
   between requests, one at a time). Symfony Runtime is the same.
   STATUS: they are not and are not expected to be.

   CORRECTION (2026-09-05, after the question "doesn't True Async handle this?"):
   the above is true for the amphp model (EXPLICIT: different clients,
   different APIs, so Eloquent/Doctrine do not fit), but NOT for the True Async
   model, which is TRANSPARENT — `PDO::query()` yields control by itself and
   the calling code knows nothing about it. If conditions 1 and 2 were delivered,
   Laravel and Symfony would work WITHOUT CHANGES. Condition 3 does not vanish
   completely; it weakens to "the framework must not leak state between requests"
   (a singleton remembering the logged-in user — class statics live in the
   process, not the coroutine). This is EXACTLY the discipline required by
   Octane, and Laravel has Octane while Symfony has Runtime — the ecosystem is
   therefore partially prepared for it. This makes True Async MORE IMPORTANT
   than the first version of this section suggested, where condition 3 was stated
   too strongly.

   What True Async has today: much of condition 2 (interception in the engine,
   broader than our `php_stream_xport_register`); condition 1 only for `ob_*`
   and hostent cache per coroutine, with the rest of EG/SG not separated;
   `max_execution_time` unchanged. Things that will never disappear: a fatal
   error kills every coroutine in the process, and `memory_limit` is per process.

   Practically for us: it is a php-src fork, the user-facing RFC is CANCELLED,
   and for 8.7 the proposal is only the scheduler ABI without I/O. Building on
   it today means tracking someone else's fork and abandoning the rule of a
   "pinned upstream tag, zero patches" on which this project stands. Our role is
   to WATCH one signal — whether request state becomes per coroutine.

### POC RESULT (2026-09-05): works, but only on the fork — `async-poc` branch

`pool.type = async` was built and RUN on the `true-async-stable` fork together
with our `sapi/fpmng`. One process, `pm.max_children = 1`, custom FastCGI client:

    4 x slow.php (usleep 500 ms)    503 ms   (fcgi: 2008 ms)
    4 x net.php  (fsockopen 500 ms) 537 ms   (fcgi: 2140 ms)
    RSS after 2000 requests         no growth

Each request got its own headers, `$_GET`, `$_SERVER`, `$GLOBALS`, and
`get_included_files()`. Full description in NOTES section 3t ON the `async-poc`
BRANCH.

**CORRECTION to this section:** the claim "fiber switches the stack, not globals,
so no scheduler can fix it" was TOO STRONG. The fork has public switch handlers
(`zend_async_API.h:269`) through which SG, `EG(symbol_table)`, and
`EG(included_files)` can be swapped WITHOUT VM changes. The fork uses them for
`ob_*`; the POC used them for the rest, and that was enough.

**The wall is elsewhere and remains:** function and class tables are PER PROCESS
(`EG(function_table)` = `CG(function_table)`, cleared only in
`shutdown_executor()`), so a second request declaring a function still gets
"Cannot redeclare". OPcache assumes one request per process
(`ZendAccelerator.c:1958,2481`), so the POC works with `opcache.enable = off`.
The fork's `global-isolation` branch made `symbol_table` per coroutine but NEVER
reached stable; per-coroutine class statics were implemented and reverted.

Thus the conclusion, also reached independently from the code, is that the real
shape of this type is not "many independent requests", but a WORKER MODEL — the
application is loaded ONCE, and a request is a call into it. Then function and
class tables are not a problem because nobody declares them twice.

### DECISION (2026-09-05): TWO pool types, not one switch

`pool.type = fiber` (custom scheduler, unmodified upstream) and
`pool.type = true-async` (the fork). The sole reason is to make WITHDRAWAL
cheap: our contract makes a type one file plus one registry line, so abandoning
one path means deleting a file and a line.
Rejected: one `async` type with an `async.engine = fiber|true-async` directive.
Then both paths would be intertwined in one file, and removing one would mean
operating on the other path's live code — exactly what this arrangement is meant
to avoid.

Shared code (acceptor, FastCGI request handling, state swapping) goes into a
THIRD shared file — the same pattern as `fpm_pool_watchdog.c` and
`fpm_pool_script.c`, extracted for cron. Removing one type then does not touch
the core because the other type uses it.

For the record: the `fiber` variant does NOT EXIST and is not verified (see
below) — the POC exists only on the fork. Agreeing to two types therefore means
agreeing to build something unverified, which is an argument FOR this layout,
not against it. Both variants hit the same wall (function/class tables, OPcache),
so both will end up as a worker model anyway.


### VARIANT "async without the fork" — an option, NOT verified

This is reasoning, not a measurement — nobody has tried it. It is recorded
because it changes the cost calculation for the experiment: if True Async never
enters PHP, this path still exists.

What currently comes from the fork and would have to be built here: (a) the
scheduler and reactor (`ext/async`, 33k lines on libuv) — it could be built on
libevent, which we already use in the gateway; (b) engine-level I/O interception
(`xp_socket.c`, `network.c`, `plain_wrapper.c`, `curl_async.c`, sleeps in
`basic_functions.c`) — partly replaceable through
`php_stream_xport_register()`; (c) switch handlers — NOT NEEDED, because in
this variant WE are the ones switching, so we swap state ourselves just before
resuming the fiber. `zend_fiber_suspend`/`zend_fiber_resume` are `ZEND_API` in
upstream (`zend_fibers.h:135-136`).

Scope of such a version: sockets and only sockets — mysqlnd, phpredis,
`fsockopen`. It will NOT catch `sleep()`/`usleep()`, curl, libpq, or ordinary
files because they do not go through the stream layer.

Cost: write from scratch what the fork has already written and take on its
maintenance. The function/class table and OPcache wall remains exactly the same
— neither path bypasses it.


### A custom scheduler for amphp — checked, nothing to build

The question was: since amphp runs on fibers, could we provide it with our
loop and "catch" the suspension? Answer: this piece ALREADY EXISTS. Revolt has
a driver abstraction and detects `ext-event` itself (literally libevent),
`ext-uv`, and `ext-ev`; without them it falls back to `stream_select`. Our driver
would be a fourth copy of `ext-event`, using the same libevent as the gateway.
Its only advantage would be ONE shared loop for gateway sockets in C and PHP I/O
— useful only in the ambitious version (our C accepts connections and calls PHP).
For an ordinary amphp application that owns its port, the gain is zero and the
cost is a driver that must follow Revolt changes.

The most important point if anyone wants to pursue this:
**Fiber switches the STACK, not GLOBALS.** No scheduler can fix that because it
is not a scheduling problem. To switch requests, `EG` and `SG` must be swapped
on every fiber switch — condition 1. The scheduler and I/O interception are
within our reach; neither is the missing element.

**What to watch if returning to the topic:** not "did True Async add drivers",
but **is request state per coroutine**? That one question decides condition 1.
Condition 3 remains either way.

### The calculation that still favors async

Worth recording because it shows that this is not a silly idea, only an
unachievable one. A typical framework request with a database and Redis: about
30 queries × about 1 ms waiting with about 5 ms CPU. The worker is idle for
about 85% of the request. Saturating 4 cores requires not 4 processes but about
28 — at 60 MB/worker, about 1.7 GB of RAM spent waiting. On a small VPS this is
a real limitation. **Async saves processes that WAIT, not processes that
CALCULATE** — concurrency is not parallelism; 4 cores still need 4 processes.

TO MEASURE on a real application: the ratio of request time to request CPU time
(FPM logs both). This packing factor determines the value of async through facts
rather than predictions. 1:7 → async saves 6 of 7 processes. 1:2 → the topic is
closed definitively.

### What this means FOR US — an amphp/ReactPHP application

Important discovery from this discussion: **an amphp application does not need
php-fpm-ng as its HTTP server.** It has its own server, owns its port, and is a
long-lived process. Our gateway, worker pool, and all of FastCGI have nothing to
connect to. We are not competitors — we pass each other by.

This is still a good result, because such an application fits us as
**`pool.type = supervisor`** — process supervision, restart policy, backoff,
statistics, and cron jobs in the same configuration file. We already have this
and it works.

Protocol caveat — amphp limitations, so we do not sell it as a free lunch:
(a) ONE blocking call anywhere in the dependency tree stops the loop for ALL
in-flight requests (in FPM it would slow one); (b) the Composer ecosystem is
blocking (payment SDKs, AWS → Guzzle/curl); (c) there is no boundary between
requests — the arena is not reset, `memory_limit`/`max_execution_time`/
`set_time_limit()` do not work per request, a fatal error kills all in-flight
requests, and a singleton remembers the previous user's data (the known
Octane/Swoole footgun).

### OPTION (not a decision): `pool.type = proxy`

The only missing piece between us and asynchronous applications. The gateway
holds :443, terminates TLS, handles ACME, serves static files itself, and passes
the rest over ordinary HTTP/1.1 on localhost to the application process
(amphp/ReactPHP/anything). The application knows nothing about it, and we do not
touch Revolt. This completes the "one binary, no nginx" story. Cost: `evhttp` as
a CLIENT, a few dozen lines; nothing that already works is changed. It fits well
with the TLS+ACME work in section 3l, which is at the end of the plan anyway.

### OPTION (not a decision): an `ext` exposing request data parsed in C

Piotr's observation: amphp parses HTTP in PHP, while we already have it in C —
perhaps we can use that. There are two very different versions:

**Small version — our C parses, PHP receives the result.** An extension exposing
method, URI, headers, and body already parsed by `evhttp` (working name:
`fpmng_request_*`). amphp would use it instead of its own parser. Feasible and
small, but **the gain is probably negligible**: we measured our gateway at about
25 µs/request — that is the ENTIRE HTTP path in C (accept, parsing, response).
Even if amphp needed 10x that, on a request spending 5 ms of CPU it is only a few
percent. Parsing 200 bytes of headers is not where the time goes.

**Ambitious version — our C drives the loop and calls PHP from it.** The real
obstacle is **TWO EVENT LOOPS** — ours (libevent) and Revolt. Both cannot drive
the process. When PHP suspends on a database query, someone must handle that
socket; if it is Revolt, Revolt must be on top while our loop stops. This can be
solved because Revolt has a driver abstraction — write a C driver on our libevent
and have ONE loop, ours. It is a real project, not a weekend, plus maintenance
alongside Revolt changes.

**Where C really wins — and it is NOT parsing:** TLS, HTTP/2 (amphp implements
the entire state machine, framing, and HPACK in PHP), and static files. The
difference is large there, not a few percent. None of the versions above is
needed for that — `pool.type = proxy` is enough.

### MEASUREMENT (2026-09-05) — the small version makes no sense

Test host 192.168.8.103, amphp/http-server 3.4.6, PHP 8.6.0-dev from the same
source tree as our binaries (`--disable-all --enable-filter`, so league/uri has
`filter_var()`), `zend.assertions=-1`, compression disabled. "Hello World"
response, `wrk -t1 -c2 -d12s`, CPU counted from `/proc/<pid>/stat`
(utime+stime) for the server process itself, 3 runs:

    amphp (TCP_NODELAY enabled) ~6 900 req/s    ~147 us/req CPU
    our gateway, hello.php      ~13 600 req/s    (k3d: 139 us/req)

So **the entire per-request cost of amphp in PHP is roughly equal to our entire
C path including `php_request_startup/shutdown` and executing `hello.php`** —
while our throughput is twice as high on the same machine. Since HTTP parsing is
only a fraction of those 147 us, exposing our parser as an `ext` cannot save
anything. SMALL VERSION: REJECTED based on the numbers. The ambitious version
(a Revolt driver in C) is unchanged — still a real project, not a weekend.

Protocol caveat: our 139 us/req comes from a k3d measurement, while 13,600 req/s
comes from the bare machine; req/s is comparable (same machine, same method), but
us/req is not fully comparable. To close this properly, measure the bare-metal
gateway with the same method.

### FINDING: amphp does NOT enable TCP_NODELAY by default

`Amp\Socket\BindContext::$tcpNoDelay` defaults to `false`, and
`SocketHttpServer::expose()` does not change it without an explicit context.
Measured effect: on a keep-alive connection, the first job takes 0.5 ms and the
**second takes 41 ms**; `wrk -t1 -c2` then gives 49 req/s instead of 6,900, with
latency fixed at 40.8 ms. Classic Nagle + delayed ACK, because the response uses
`transfer-encoding: chunked` over several writes. With `Connection: close`, the
problem disappears (2,686 req/s), confirming the mechanism.
Remedy: `$server->expose($addr, (new BindContext())->withTcpNoDelay())`.

This is EXACTLY the same class of bug as our 0002 patch to `main/fastcgi.c`
(there, `req->tcp` is set only under `#ifdef _WIN32`). Worth reporting to amphp.

Diagnostic trap worth remembering: with `NullLogger`, amphp SWALLOWS exceptions
from client handling. The symptom was `wrk` showing 80,000 "read errors" and no
responses, with nothing in the log. Only a custom stderr logger showed
`Call to undefined function filter_var()`.


## 3u. `pool.type = status` — implemented and verified (2026-09-05)

**Superseded 2026-09-13 (issue #278).** The pool type is gone. Everything below
stays because it records what it did and why, and because the two pages
survived it unchanged in shape — but they are now per-pool directives
(`pm.status_path`, `pm.metrics_path`) answered by the operator endpoint
(issues #274/#275) rather than by a pool of their own, and a configuration that still says
`pool.type = status` fails to start with a message naming the replacement.
`fpm_pool_status.c` is `fpm_operator_pages.c`; the two renderers kept their
bodies and lost the walk over `fpm_worker_all_pools` — a page describes the one
pool whose directive asked for it. The reason for the removal is that a pool
reporting on every other pool from a listener of its own is the thing the
operator endpoint already is, and keeping both meant two configuration
languages for one listener. See [`docs/operator-endpoint.md`](operator-endpoint.md).

New file `sapi/fpmng/fpm/fpm_pool_status.c`/`.h`, one line in
`fpm_pool_types[]` (`fpm_pool_type.c`). No new configuration directives —
`validate()` programmatically enforces `pm = static` + `pm.max_children = 1`
(one process is entirely sufficient for monitoring scrapes), just as supervisor
maps `supervisor.processes` to `pm.*`. `requires_listen = 1`, but unlike the
HTTP gateway the port is directly the configured `listen` (not fcgi+1) — this
type has no FastCGI behind it, so there is nothing to shift.

### Data shape — branch on `serves_requests`, exactly as in 3j

`fpm_pool_status.c` walks `fpm_worker_all_pools`, gets
`fpm_pool_type_of(wp)` for each pool, and:
- for `serves_requests = 1` (`fcgi`, `http`), reads `wp->scoreboard` (the same
  scoreboard used by today's `fpm_status.c`,
  `fpm_scoreboard_copy(wp->scoreboard, 0)` — copied under a lock because it is
  a FOREIGN pool read from ANOTHER process) — idle, active, requests;
- for `serves_requests = 0` (`supervisor`, `cron`), calls the NEW fifth operator
  in `fpm_pool_type_s`, `status(wp, out)`, which returns
  `struct fpm_pool_status_s` (`state`, `last_start`, optional `last_exit_code`,
  `consecutive_failures`, optional `next_run` or `backoff_until`). Each such
  type implements this in ITS OWN file, reading ITS OWN shared memory —
  `fpm_pool_status.c` does not know the internal structure of
  `fpm_supervisor_shared_s` or `fpm_cron_shared_s`, exactly as the 3h contract
  requires ("new type = new file + one registry line");
- skips a type without `.status` and with `serves_requests = 0` (today:
  `status` itself), without detecting it by name.

`enum fpm_pool_state_e` (running/backoff/gave_up/finished/idle) and
`struct fpm_pool_status_s` live in `fpm_pool_type.h` — the only shared location
for types that fill them and `fpm_pool_status.c`, which reads them. Prometheus
gets `fpmng_pool_info{pool,type} 1`, while state is a set of
`fpmng_pool_state{pool,state} 0|1` series, not a number requiring knowledge of
the enum. Type-specific fields (`next_run` for cron, `backoff_seconds` for
supervisor) are emitted only where meaningful; the same rule applies to JSON.
No labels with unbounded cardinality: labels come only from pool configuration
and a closed set of states.

### Minimal shared-memory state added — exactly what is shown, no extra fields

**Supervisor** (`fpm_supervisor_shared_s` in `fpm_pool_supervisor.c`) already
had `failures`/`terminal`/`gave_up` (backoff policy, 3o) — THREE new fields were
added only for status: `running` (bool, set immediately before
`fpm_pool_script_run()` and cleared immediately after), `last_start` (epoch),
and `last_exit_code`. None affects restart/backoff policy; this is purely
observational state.

**Cron** (3r) had ZERO shared-memory state — deliberately, because "every new
process calculates the due time only from the current clock". That decision
**remains in force**: status calculates `next_run` ON DEMAND in
`fpm_pool_cron_status()` through
`fpm_cron_schedule_next(wp->config->cron_parsed_schedule, time(NULL))` —
exactly the same function cron itself uses to calculate its next due time,
called on configuration read from the status process's memory (a common fork
from the master, the same configuration for all pools — see below). No shared
memory for `next_run`.

What cron CANNOT calculate from the clock alone is historical fact:
`last_run` and `last_exit_code`. For those two (plus `running` and
`consecutive_failures`, which are effectively free in the same allocation),
`fpm_cron_shared_s` was added — a NEW, minimal `init_main` for the `cron` type
(previously `NULL`, "nothing to do on the master side"; now it performs one
`fpm_shm_alloc()`, the same pattern as supervisor). **This is the only compromise
with "cron is stateless" from 3r** — and it is deliberate: without it,
`last_start`/`last_exit_code` cannot physically be shown (nobody else remembers
them), while `next_run` remains stateless. `consecutive_failures` is free to
calculate from the same `last_exit_code`, but affects NOTHING — cron has no
policy that consumes it (unlike supervisor); it is only a human/monitoring signal
("this cron has failed N times in a row"), accepted as a cheap consequence of
the same state rather than a separate design decision. "How many runs were
skipped due to overlap" (listed in 3j as a possible cron addition) **was NOT
implemented** — with `pm.max_children = 1`, overlap is physically impossible
(3r), and counting "how many due times were missed between last_run and now"
would be cosmetic without a policy behind it; it was judged unnecessary
complexity.

### TRAP — real discovery: `fpm_children.c` RELEASES FOREIGN pool scoreboards in every child by default

This is the **only place where `fpm_children.c` was touched**, with full
justification exactly as the task required. Symptom: a `status` pool reading the
scoreboard of an `fcgi`/`http` pool (exactly what variant 1 in 3j exists for)
segfaulted **less than 1 ms after fork**, before any of our code could do
anything — on the first access to `wp->scoreboard->idle` for the FOREIGN pool.

Diagnosis (hours wasted on false leads, recorded so nobody repeats them): it
was NOT Zend MM, NOT OPcache, NOT allocation-versus-fork ordering (verified:
both scoreboards were allocated in the master BEFORE any fork), NOT anonymous
versus named shared memory (tested separately: `shm_open()` instead of
`mmap(MAP_ANON)` in `fpm_shm_alloc()` — identical crash), NOT adjacency of memory
regions (a gap between allocations was tested — identical crash), and NOT a
sandbox limitation in this repository (a bare C program doing
`mmap(MAP_SHARED)` plus two more forks works correctly in the same bash sandbox).

The real cause, found by reading code rather than guessing:
`fpm_children.c:fpm_child_resources_use()` (called in EVERY child immediately
after `fork()`, before anything else) has this loop:

```c
for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
    if (wp == child->wp || wp == child->wp->shared) {
        continue;
    }
    fpm_scoreboard_free(wp);   /* munmap() the scoreboard of EVERY OTHER pool */
}
```

This is EXISTING, deliberate upstream memory hygiene: after fork, every worker
releases (unmaps) the scoreboards of ALL pools except its own, so its address
space does not retain unnecessary mappings. It has always been safe because
EVERY previous scoreboard consumer (only `fpm_status.c`) reads ONLY its OWN
pool's scoreboard — nobody had ever tried to read a FOREIGN pool's scoreboard
from ANOTHER process. `pool.type = status` is the **first** such consumer, and
this hygiene is destructive to it: the `status` process wants to show fcgi/http
data, but those pools' scoreboards have already been unmapped before it can read them.

This is EXACTLY the 3h contract limitation anticipated by the task:
_"Do not touch fpm_children.c without a very good reason — if you decide you
must, explain why"_. The reason: without this change, `pool.type = status` cannot
work FOR FCGI/HTTP AT ALL (it works immediately for supervisor/cron because
their own state does not pass through `fpm_scoreboard_free()` — it is a separate
allocation and their own registry in `fpm_pool_supervisor.c`/`fpm_pool_cron.c`).

The fix is minimal and data-driven, in the spirit of 3h: add
`reads_foreign_scoreboards:1` to `fpm_pool_type_s` (set only for `status` today).
In `fpm_child_resources_use()`, wrap the loop that releases foreign scoreboards
in `if (!fpm_pool_type_of(child->wp)->reads_foreign_scoreboards)` — the decision
is per CHILD, not per master: **only** a `status` child keeps foreign scoreboards;
EVERY other pool in the same configuration (including an ordinary fcgi/http next
to `status`) continues to release them exactly as today, regardless of whether
a `status` pool exists anywhere in the configuration. `fpm_children.c` asks for
the type of THIS SPECIFIC child through the existing generic
`fpm_pool_type_of()` function; it does not know any concrete type, it only reads
data. The first version disabled the hygiene for the entire master through a
function scanning the whole configuration
(`fpm_pool_type_any_reads_foreign_scoreboards()`) — **fixed after review**:
that was too broad (fcgi/http workers lost their protection against accidental
access to foreign memory even though it did not apply to them). The whole-config
scanning function was removed as unnecessary.

Without `pool.type = status` in the configuration, behavior is 1:1 identical to
upstream — verified directly, not only theoretically: two ordinary `fcgi` pools
(`web`, `web2`, with no `status` in the configuration) — `vmmap` on each child
shows exactly ONE 16K `VM_ALLOCATE...SM=S/A` region (its own scoreboard), NOT
two. Worker `web` therefore still has no physical mapping for `web2`, and vice
versa, exactly as before any change in this task. In contrast, the master before
fork has one COMBINED 32K region (both scoreboards together, adjacent in memory)
— showing exactly WHAT each child inherited and what it released itself.

After narrowing the fix, the cost is that ONLY the `status` process keeps mappings
for ALL other pool scoreboards in its address space (a few dozen KB per pool,
negligible); no other process pays this cost.

### Tests (verified on a built binary, macOS/arm64)

1. Configuration without `pool.type` (`fcgi`) — `-t` is green and it behaves as
   today (BC).
2. `pool.type = status` with directives related to "anything involving php/pm"
   (`pm.max_children`, `supervisor.script`) in the same block — `-t` gives a
   clear ALERT for both and `FPM initialization failed` (exit 78).
3. Real server: one `fcgi` pool (`web`), one `supervisor` (`sup`), one `cron`
   (`cronjob`, `* * * * *`), and one `status` (`metrics`) — all start. `curl
   /metrics` (Prometheus) and `curl /status` (JSON) on the status port show
   idle/active/requests for `web` AND `pool_info`, state/last_start/exit_code/
   failures for `sup` and `cronjob` (plus `backoff_seconds` only for `sup` and
   `next_run` only for `cronjob`) at the same time — fields inapplicable to the
   type are absent.
4. `SIGUSR2` (reload) — same master PID, new children of every type, and
   `/status` after reload shows a fresh `last_start` for `sup` (backoff counters
   reset, consistent with 3p — a new shared-memory segment after `execvp()`).
5. `kill -9` on the `sup` process — untouched `fpm_children.c` respawns it
   immediately; `/status` shows a new, later `last_start` after respawn — status
   correctly reflects the new process generation.
6. A supervisor script that always exits `exit(1)`, with `restart = on-failure`
   and `restart_max = 2` — after two failures `/status` shows
   `"state":"gave_up"`, `"last_exit_code":1`, and
   `"consecutive_failures":2`, exactly the state the supervisor is in (also
   confirmed by the concurrent ALERT "giving up" log).
7. After narrowing the `fpm_children.c` fix (see above) to a single child: repeat
   test 3 (status still reads all scoreboards without a segfault) — green.
   Separately, a configuration WITHOUT any `status` pool (two ordinary `fcgi`
   pools) — `vmmap` on each child shows ONLY its own scoreboard (16K), not both,
   bit for bit as before the entire task.
8. A client connecting to the status port and sending nothing (`socket.recv()`
   without a preceding `send()`) — the server closes the connection after
   exactly about 5s (`SO_RCVTIMEO`), measured directly rather than assumed; the
   process does NOT hang forever and still answers the next real request normally.
9. Two `pool.type = status` pools in one configuration (different ports) — both
   start independently and both correctly answer `/status` for the same `fcgi`
   pool next to them, with no conflict.

### Not done / uncertain

- The PHP application-metrics API (`fpm_metric_register/inc/set/observe`) from
  3k is a separate task; this type exposes only built-in pool state.
- Multiple `status` processes (such as `status.processes`) — deliberately absent;
  one process is enough for monitoring scrapes. If more are needed, it is a
  deliberate design decision (a new directive), not a default.
- Keep-alive/chunked/full HTTP parser — deliberately absent (see the comment in
  `fpm_pool_status.c`): this is a monitoring endpoint, not a web server, and one
  request per connection is sufficient.
- `SIGQUIT` graceful handling for `status` — no custom handler, with the same
  side effect as supervisor/cron without a `SIGQUIT` handler (delay until the
  master's `SIGTERM` escalation, see 3p scenario 2). Accepted because `status`
  has no in-flight work to finish. This delay no longer occurs on reload: 3x
  below has the master send `SIGTERM` to `status` directly instead of waiting
  for escalation. It still applies to an explicit graceful stop or log
  rotation, which keep the ordinary `SIGQUIT` fan-out.
- "How many cron runs were skipped due to overlap" — see above; deliberately
  omitted as unnecessary complexity with `pm.max_children = 1`.
- `SO_RCVTIMEO`/`SO_SNDTIMEO` (5s, `FPM_POOL_STATUS_IO_TIMEOUT_SEC`) on the
  client connection — added and measured directly (test 8 above): a client that
  keeps the connection open without sending data is disconnected after exactly
  about 5s, and the process handles later requests normally. This does NOT
  protect against a slow but non-silent client (for example, one byte every 4s,
  resetting the timeout each time) — a theoretical DoS gap for a single process
  (`pm.max_children` is always 1), accepted as sufficient for a monitoring
  endpoint with no public traffic.

## 3t. Syscalls of the blocking FastCGI worker — implemented and verified (2026-09-05)

The assumption changed relative to 3m/3q: **there is no `fcgi-async` type**. True
Async was checked at the source — the user-facing RFC was cancelled, blocking I/O
in the C SAPI is not intercepted, and `main/fastcgi.c` and `sapi/fpm` remain
untouched. The optimizations therefore went into the ordinary blocking worker,
split by whether they change observable behavior: no change → default; change →
opt-in.

### What landed

| what | where | default | behavior change |
|---|---|---|---|
| fix `TCP_NODELAY` (upstream bug) | `patches/0002` (`main/fastcgi.c`) | yes | fix only — Nagle disappears from TCP keep-alive |
| 16 KB input buffer in `safe_read()` | `patches/0003` | yes | no (one `read()` instead of six for the request header) |
| `accept4(SOCK_CLOEXEC)` | `patches/0003` + `AC_CHECK_FUNCS([accept4])` in our `config.m4` | yes when `HAVE_ACCEPT4` | no (falls back to `accept` + 2x `fcntl`) |
| `write(2, "\0fscf")` only with `catch_workers_output = yes` | `fpm_stdio.c` (owned, 4 commits/year) | yes | no — with `no`, fd 2 is `/dev/null` and the write went nowhere |
| `request_cpu_tracking = yes|no` (2x `times()`) | `fpm_conf.c/h`, `fpm_request.c/h`, hook in `fpm.c` | **yes** = upstream behavior | `no` zeros "last request cpu" in status and `%C` in `access.format` |

`fpm_stdio.c` is the fifth owned file (after `fpm_main.c`… — see section 2);
the change is a static flag set in `fpm_stdio_child_use_pipes()` and an early
`return` in `fpm_stdio_flush_child()`.

The child reads `request_cpu_tracking` in `fpm.c` **before**
`fpm_cleanups_run(FPM_CLEANUP_CHILD)`, because `wp->config` no longer exists
afterwards (the same trap as in 3o). The directive is per pool.

### Rejected and why

- **`poll` after `accept` → `SO_RCVTIMEO` on the listening socket.** Measured
  with a C program on the test host (Linux 7.0), not from documentation:
  - TCP: the option is inherited through `accept()` and interrupts `read()` —
    but **`accept()` without a client also gets EAGAIN after the timeout**, and
    `fcgi_accept_request` treats every error other than EINTR/ECONNABORTED as
    fatal → the worker exits after 5 seconds of idleness;
  - **UDS does not inherit it at all** (`SO_RCVTIMEO` on the connection = 0),
    meaning zero protection on the transport we recommend;
  - the timeout would carry over to reads on keep-alive connections (nginx holds
    them idle for a long time), so it would need to be removed with another
    `setsockopt` — the saving of one syscall on a NEW connection disappears.
  Keep `poll`. One syscall is not worth a suspended or disappearing worker.
- **CWD cache** — out of scope (`main/`, CWD semantics), as decided.
- **`zend_signal_activate` (7x `rt_sigaction`) and the OPcache lock (2x `fcntl`)**
  — Zend and ext/opcache, not the SAPI. Material for a separate upstream
  report: register handlers once per process instead of per request; hold the
  `accel_activate_add`/`deactivate_sub` lock per request.
- **`SO_REUSEPORT`** — untouched (3m: thundering herd does not exist).

### Traps found along the way

1. **`0001` broke `--enable-fpm` in the same tree — FIXED (path 1).** It changed
   the hook signatures in `main/fastcgi.h` (`void(*)(bool)`), while upstream
   `fpm_main.c` passed `void(*)(void)`; GCC 14+ treats incompatible pointers as
   an error. Cause: `0001` was a slice of bukka#2's PR limited to `main/`, while
   its `sapi/fpm/fpm/fpm_request.c/.h` part was carried only as our own files in
   `sapi/fpmng/`. Coordinator decision: `0001` now carries the full PR (without
   `.phpt` tests). Verified: `--enable-fpm --enable-fpmng` builds in one tree
   (Mac), and `sapi/fpm/tests` on the upstream `php-fpm` built with the full
   0001+0002+0003 stack: 141 tests, 0 failed. Side effect: `prepare.sh` could
   stop deleting `sapi/fpmng/tests`. Cost: PHP-8.3 needs a `0001` variant
   (`fpm_scoreboard_update_commit` has 7 arguments there; 8.4 added
   `memory_peak`) — together with the `0003` variant, that is two variants for
   8.3, the warning threshold from `patches/README.md`. Upstream tests can also
   run against `php-fpm-ng` through `TEST_PHP_FPM_EXECUTABLE` (it looks two
   levels above for `<dir>/fpm/php-fpm`, so a symlink is needed).
2. **The patch stack is ordered.** 0002 and 0003 assume 0001 in their context
   (the init hunk in `fcgi_init_request` and around `accept()`), although they
   are independent in substance. Upstream versions would need rebasing onto a
   clean tree.
3. **`prepare.sh` lied about the stack.** It checked "already applied" with a
   reverse dry run per patch — 0002 failed when 0003 was on top of it. The
   decision is now made once for the whole stack: forward on an untouched tree,
   or the entire stack in reverse from copies of the touched files. Checked on
   BSD patch (Mac) and GNU patch.
4. **PHP-8.3 `safe_read()` has `const void *buf`** (changed in 8.4 by #20887) —
   0003 needs `patches/php-8.3/`. It is the first version variant in the repo;
   two is the warning threshold from `patches/README.md`.
5. Copying the repository from a Mac with `tar` without `COPYFILE_DISABLE=1`
   adds `._*.c` files, which `prepare.sh` treats as sources and the build fails
   with `No rule to make target '._fpm_request.c'`.

### Correctness

The full `sapi/fpm/tests` suite (141 tests), 0 failed, in five variants — the
fifth was upstream `php-fpm` built together with `php-fpm-ng` in one tree with
the complete 0001+0002+0003 stack (Mac, 115 pass / 25 skip). The other four were
clean upstream and upstream+0002/0003 on Linux (121 pass / 19 skip, identical
before and after; `HAVE_ACCEPT4` enabled manually because upstream does not check
it), upstream+0002/0003 on Mac (path without `accept4`), and `php-fpm-ng` on Mac
(115 / 25, environmental skips). The only "warn" was an upstream XFAIL that
passes — the same without patches.

Real nginx 1.28 on the test host (`ngtest/nginx-check.sh`), baseline binary
(0001 + our SAPI) versus new, with `catch_workers_output = yes`, `no`, and
`no` + `request_cpu_tracking = no`. **Four runs, zero failures**, identical
results for baseline and new:

- `hello.php` x50 on every path: TCP keep-alive, new TCP connection, UDS
  keep-alive, new UDS connection, `fastcgi_buffering off` +
  `fastcgi_request_buffering off`;
- raw POSTs of 1 KB, 17 KB (just over the buffer), 100 KB, 1 MB, and 5 MB —
  body length and md5 match on every path; 300 KB multipart with a file —
  `$_POST` and the file md5 match;
- 20 KB, 200 KB, and 5 MB responses (md5 of the complete body) on every path;
- chunked: nginx answers with `Transfer-Encoding: chunked`, raw body longer
  than the decoded body, decoded md5 correct;
- connection dropped halfway through a response (`curl -m 0.3` during a script
  pause with 100 KB + `usleep` + 100 KB, twice, on keep-alive and the stream):
  worker survives, the next 20 requests are OK, worker count unchanged;
- `catch_workers_output = yes`: worker `php://stderr` reaches the master's
  `error_log`; with `no` it does not (as specified) — skipping
  `write(2, "\0fscf")` is safe when there is a receiver;
- default `request_cpu_tracking`: "last request cpu" in status and `%C` in the
  access log are non-zero after a script using about 30 ms CPU; `= no`: both are 0.

Test trap (not code): the FPM SAPI has no `STDERR` constant — a script using
`fwrite(STDERR, …)` fails fatally; use `fopen('php://stderr')`. The first test
for "STDERR absent from the log" was a false alarm, also on upstream.

### Performance — measured (i7-6700T test host, Linux 7.0, PTI+IBRS, k3d disabled, idle machine)

Baseline binary = 0001 + our `sapi/fpmng` before this work; new = the same plus
0002 + 0003 + `fpm_stdio.c` + `request_cpu_tracking` (default `yes`, so `times()`
in both). Measurement configurations did not use `catch_workers_output`, so
the new binary skips `write(2, "\0fscf")`. `hello.php` (`echo "hello\n"`), OPcache.

**Syscalls per request** (`strace -c` on one worker, fcgibench, TCP):

| | keep-alive | new connection |
|---|---|---|
| baseline | **26** (read 6, write 2, rt_sigaction 8, chdir 2, fcntl 2, times 2, setitimer 2, getcwd, rt_sigprocmask) | **33** (+ accept, fcntl 2, poll, shutdown, recvfrom 2, close; read 5) |
| new | **20** (read 1, write 1, rest unchanged) | **25** (accept4 1, fcntl 2 = OPcache only, read 1, recvfrom 1, poll, shutdown, close) |

Of the remaining 20: 8x `rt_sigaction` + 1x `rt_sigprocmask` (Zend signals), 2x
`setitimer` (`max_execution_time`), 2x `chdir` + `getcwd`, 2x `fcntl` (OPcache),
and 2x `times` (disabled by the directive) — only `read` + `write` are FastCGI.

**Worker CPU per request** (children's utime+stime from `/proc`, fcgibench, 2
connections, 5s, three runs; min–max values):

| path | baseline µs/req | new µs/req | difference |
|---|---|---|---|
| TCP keep-alive | 60,2–62,5 | 48,7–50,8 | **−12 (−19%)** |
| TCP new connection | 89,0–95,4 | 81,7–84,5 | −8 (−9%) |
| UDS keep-alive | 52,6–53,7 | 41,7–44,8 | **−9 (−18%)** |
| UDS new connection | 72,0–73,0 | 52,3–54,3 | **−19 (−27%)** |

The gain is larger than in 3m (7/12 µs), because skipping
`write(2, …)` was not included there — on this machine `write` costs 4–7 µs per
call according to `strace`, the most expensive individual syscall on the path.

**`wrk -t1 -c2 -d10s` through nginx 1.28** (nginx + wrk + 2 workers on the same
machine, three runs):

| path | baseline req/s | new req/s | worker CPU µs/req baseline → new |
|---|---|---|---|
| TCP keep-alive (`fastcgi_keep_conn on`) | 8441–8830 | 8543–8576 | 108–113 → 106–108 (−3…−5) |
| TCP new connection | 7622–7948 | 7737–7935 | 106–109 → 97–98 (**−11**) |
| UDS keep-alive | 14657–15012 | 15962–16191 (**+7%**) | 69–72 → 59–60 (**−10**) |

Throughput through nginx barely moves because it mostly measures nginx+wrk (as
in 3m: under higher load we see contention for hyperthreads, not code); worker
CPU per request is the right metric. The surprise: TCP keep-alive through nginx
gains the least (−3…−5 µs), although direct fcgibench gains −12 — not investigated,
recorded as open. A request through nginx costs about 45 µs more CPU than the
same fcgibench script — about 20 `fastcgi_params` variables and a larger
`$_SERVER`, not transport.

**Nagle (`TCP_NODELAY`, patch 0002)** — custom Python FastCGI client
(`patches/0002-fcgi-nodelay-repro.py`), one TCP connection with
`FCGI_KEEP_CONN`, 50 consecutive requests, `mid.php` = 20 KB response (> 8 KB
buffer), three runs:

| | median | min | max |
|---|---|---|---|
| baseline | **41,0 ms** | 0,27 ms | 41,8 ms |
| new | **0,08 ms** | 0,07 ms | 0,71 ms |
| baseline, `hello.php` (< 8 KB) | 0,09 ms | 0,07 ms | 0,34 ms |

Two separate things must not be mixed:

1. **Code defect — proven from the code alone**: `req->tcp` is assigned only
   under `_WIN32` but read unconditionally; the `TCP_NODELAY` branch outside
   Windows is dead. This is enough for an upstream report independently of the measurements.
2. **Practical effect — only partially demonstrated.** With the custom client:
   41 ms → 0.08 ms, the Linux delayed-ACK timer (40 ms) on every request with a
   response larger than one `write()`. **Not reproduced with nginx 1.28 on a
   loopback** — responses > 8 KB take microseconds with and without the patch.
   We found no real-nginx configuration that catches it, so we do not claim that
   a typical nginx + FPM deployment sees it. Hypothesis (not a finding): nginx
   reads the response immediately, its kernel ACKs after two full segments, so
   delayed ACK has no chance to fire; a slower-reading client or a real network
   path gets the delay — our Python client is such a client.

The report text keeps this distinction: `patches/0002-upstream-report.md`.

**Hardware caveat**, unchanged from 3m: PTI+IBRS, syscall about 0.9 µs; on a
newer CPU the absolute gain shrinks 3–5x.

### Recommended configuration for lightweight endpoints (zero code lines)

- `php_admin_value[max_execution_time] = 0` — removes 2x `setitimer` +
  `rt_sigprocmask` (−3 µs/request on the test host). FPM still has
  `request_terminate_timeout` as a wall-clock guard.
- `listen = /run/php/pool.sock` instead of `127.0.0.1:9000` — −7…−11 µs/request.
  The HTTP gateway and worker are in the same container, so loopback TCP adds nothing.
- `catch_workers_output = no` when logs go through `error_log()`/stderr to the
  application's own stack — saves one `write()` per request (now automatic).
- `request_cpu_tracking = no` if nobody reads "last request cpu" or `%C`.

## 4. Measured: performance is NOT the argument

Test host 192.168.8.103, k3d, i7-6700T. Full data was in the Claude project
memory (`reference_k3d_bench_poligon.md`), which is not a file in this
repository -- the table below is the whole of it that survives here.

CPU per request, clean measurement (`wrk -t1 -c2`, unsaturated node):

| script | nginx+fpm | gateway | saving |
|---|---|---|---|
| hello.php (~0) | 227 µs | 128 µs | 44% |
| work.php (~1 ms) | ~1590 µs | ~1400 µs | 12% |
| w5.php (7.7 ms) | 7917 µs | 7116 µs | 10% |
| w20.php (74 ms) | 74132 µs | 73548 µs | 0.8% |

A real application (20–100 ms CPU) lies between the last two rows — **a few
percent**. Throughput is analogous: 2.21x for an empty script, 1.06x at 1 ms,
1.01x at 5 ms.

The gateway itself costs **at most about 25 µs/request** (C total 128 µs versus
php-fpm alone in B at 104 µs). That is the entire budget for further
optimizations.

**Conclusions we will not revisit:**
- io_uring — rejected; it attacks part of those 25 µs and requires a second
  event-loop backend only on Linux;
- the in-process variant (HTTP in the worker) — rejected; roughly 50–60 µs saved
  by putting HTTP into the worker lifecycle (`max_requests`,
  `request_terminate_timeout`, and no room for queuing);
- **no further performance optimizations**, put all energy into features.

Methodology: the test host has 4 physical cores / 8 threads and wrk runs on the
same node. With `-t4 -c64`, the measurement captures hyperthread contention,
not hop cost (the same pair produced a 1670 µs versus 234 µs difference). Only
`wrk -t1 -c2` is authoritative.

## 5. Open questions

### TLS — DECIDED 2026-09-05: build it, last. See section 3l.

The following remains as a record of the reasoning; the chosen path is our own
HTTPS with ACME.

A small VPS project has no load balancer. Three options:
1. Put Caddy/Traefik in front of fpm-ng — but then "you need nothing else" stops
   being true;
2. fpm-ng handles HTTPS including ACME — a substantial amount of work;
3. keep TLS termination out of scope and say so explicitly (Cloudflare / reverse proxy).

Competitive context: FrankenPHP targets the same group and has one binary, HTTP,
static files, automatic HTTPS through Caddy, and worker mode. It **does not have**
declarative consumers and cron jobs. Our HTTP gateway is therefore the least
differentiating part, while supervisor and cron are what nobody else has. If we
need to cut ambition somewhere, cut HTTP first.

### Static binary — check FIRST

The entire systemless-image concept depends on statically linking PHP + FPM +
libevent + pcre + zlib. It is smooth with musl and a fight with NSS on glibc.
**If this turns out to be hell, the concept fails** — better to know before
writing four pool types.

Good news: `fpm_unix.c:51` has `fpm_unix_is_id` — if `user`/`group` contains a
number, FPM does not call `getpwnam`. Thus `user = 65534` works without
`/etc/passwd`. That is usually what breaks scratch.

### Hot-reload scope

A real hot reload (configuration diff, touching only the changed pool) is
possible, but fights the `exec`-based architecture — several weeks of work.
Not at startup. See section 7.

## 6. Known problems and gaps

### Blockers — CLOSED (2026-09-06, branch `http-config`)

- Configuration: directives exist for `http.listen`, `http.gateways` (2),
  `http.reuseport` (0), `http.static` (1), `http.idle_timeout` (500 ms),
  `http.read_timeout` (5000 ms, task 031), `http.max_body` (32m, task 031), and
  `http.allowed_clients`. Validation is in the pool-type `.validate` hook. The
  `FPM_HTTP_*` environment variables remain as a fallback, with the directive
  taking precedence. `rejects[]` on FastCGI types makes `http.*` outside
  `pool.type = http` a configuration error, not a silent ignore.
- "Enabled by default" was **out of date** — since `pool.type` was introduced,
  the gateway starts only under `pool.type = http`. This item remained here from
  the pre-pool-type version.
- Access control: `fpm_http_acl.c/.h`, the same logic as `listen.allowed_clients`
  (literal IPv4/IPv6, **no CIDR** — as upstream). A rejected client gets 403.
- Respawn: `fpm_children_extra.c/.h` — a generic registry for processes forked
  directly by a pool type, outside the `pm.*`-counted `fpm_child_s`. One hook in
  the "unknown child" branch of `fpm_children.c`, with no knowledge of a
  concrete type. Limit of 5 respawns in 10 seconds, then the gateway gives up
  until the next reload.

### Functional (required to replace nginx)

- No TLS (see section 5);
- `index.php` is hard-coded, with no `try_files` equivalent.

### CGI variables, X-Forwarded-*, access log — CLOSED (2026-09-06, branch `http-cgi-vars`)

Three points were closed together because they overlap
(`REMOTE_ADDR`/`SERVER_PORT` mean different things behind a trusted proxy, and
the access log wants the same final address).

- **Missing CGI variables**: `SERVER_PORT`, `SERVER_ADDR`, `HTTPS`,
  `REQUEST_SCHEME`, `AUTH_TYPE`, and `REMOTE_USER` were added in
  `fpm_http_build_request()` (`fpm_http.c`). `SERVER_ADDR`/`SERVER_PORT` come
  from `getsockname()` on the descriptor for the SPECIFIC connection
  (`evhttp_connection_get_bufferevent()` + `bufferevent_getfd()`), not from the
  pool's listening address — correct even with `SO_REUSEPORT` or a bind to
  `0.0.0.0`/`::`. The gateway never claims TLS itself, so `HTTPS`/
  `REQUEST_SCHEME` default to "absent"/`http`; ONLY a trusted proxy overrides
  them (below). `AUTH_TYPE`/`REMOTE_USER` are derived from the `Authorization`
  header in new `fpm_http_auth.c/.h` — a custom base64 decoder (Zend MM is not
  initialized in the gateway process, so `php_base64_decode_ex()` from
  `ext/standard` is not safe to call here). `Basic` decodes `user:pass`; every
  other scheme gets only `AUTH_TYPE`.
- **`X-Forwarded-For`/`-Proto`/`-Port`**: new `fpm_http_forwarded.c/.h` plus
  `http.trusted_proxies` (like `http.allowed_clients`: a list of literal
  IPv4/IPv6 addresses, reusing `fpm_http_acl_parse()`/`_check()` — which is why
  the module's first parameter was renamed to `directive`, so parse errors name
  the correct directive instead of always "http.allowed_clients"). Headers
  count ONLY when the direct TCP address is on the list — an empty directive
  means trust nobody, the safe default. From `X-Forwarded-For`, take the first
  address FROM THE RIGHT that is not itself trusted. **Not the first from the
  left** — the first version did that and it was a hole: nginx's default
  `$proxy_add_x_forwarded_for` APPENDS the client address to the header the
  client sent, so the left side of the list is directly controlled by the client
  and `REMOTE_ADDR` could be spoofed DESPITE the trusted proxy (measured:
  `"9.9.9.9, 8.8.8.8"` produced 9.9.9.9, now 8.8.8.8). Scanning from the
  right works for one proxy and for a chain. `X-Forwarded-Proto` maps to
  `HTTPS`/`REQUEST_SCHEME`; `X-Forwarded-Port` maps to `SERVER_PORT` (NOT
  `REMOTE_PORT` — that is always the real TCP connection port). The resolution
  runs ONCE per request in `fpm_http_request()`, and the result is shared by CGI
  variables and the access log.
- **Access log**: new `fpm_http_access_log.c/.h`, `http.access_log` directive
  (path, empty = disabled). Combined Log Format, NOT CONFIGURABLE — deliberately,
  to avoid complicating the code with a configurable formatter. Each of the
  `http.gateways` gateway processes opens ITS OWN descriptor for THE SAME file
  with `O_APPEND`; one `write()` per line (deliberately no retry after a short
  write — appending the rest with a second `write()` would break the guarantee
  against interleaving) relies on POSIX's guarantee that `write()` with
  `O_APPEND` on a regular file is atomic relative to other writers — **measured
  live**: 40 concurrent requests through 2 gateway processes, 40 clean lines,
  zero interleaved. `remote_user` and `remote_addr` use the same escaping as the
  URI and User-Agent — `remote_user` comes from base64 in `Authorization`, so it
  may contain arbitrary bytes; the first version did not escape it, allowing
  injection of lines into the access log (measured:
  `Basic base64("ad\nmin:x")`). Writes are synchronous (as in nginx/Apache) — a
  local disk/page cache makes this cheap. Logged points: worker response
  (success and 502), ACL 403 (`http.allowed_clients`, using the RAW address of
  the connecting client — this is a direct-peer decision and does not pass
  through X-Forwarded-For), 400 for a bad request, and every static-file
  response (304/200/404/502).

**Verified LIVE** (macOS/arm64, PHP 8.5.11-dev, local build with brew
libevent — not Docker/Alpine, see "how built" below): baseline `$_SERVER` over
HTTP; spoofed `X-Forwarded-*` from an address OUTSIDE `http.trusted_proxies`
was rejected (`REMOTE_ADDR`/`SERVER_PORT`/`HTTPS` unchanged); the same headers
from an address IN `http.trusted_proxies` were accepted, with
`REMOTE_ADDR`/`SERVER_PORT`/`HTTPS`/`REQUEST_SCHEME` correctly overridden and
`REMOTE_PORT` unchanged; `Authorization: Basic` -> `AUTH_TYPE=Basic` + correctly
decoded `REMOTE_USER`; `Authorization: Bearer ...` -> only `AUTH_TYPE=Bearer`,
without `REMOTE_USER`; base64 without a colon after decoding -> `AUTH_TYPE`
remains, `REMOTE_USER` does not; `http.trusted_proxies`/`http.access_log` were
rejected on
`pool.type = fcgi` (`rejects[]` works); an invalid `http.trusted_proxies` value
is rejected at startup with the correct message (see the bug below); an access
log with `http.gateways = 2` under 40 concurrent requests has 40 clean lines;
ACL 403 reaches the log; a static file (`http.static = 1`) reaches the log with
its real size.

**Not measured / deliberately omitted**: a live nginx+fpm comparison — no local
nginx was available, so the comparison relies on knowledge of standard nginx
`fastcgi_params`, not a running test. Slow loris/client timeouts in the access
log are a separate known gap (see "Hardening" below). Logging for the
`SCM_RIGHTS`/`fcgi-async` path is irrelevant — the HTTP gateway is the only type
with an access log.

**Found incidentally and FIXED as part of the same work**:
`fpm_http_acl_parse()` had the error message `"http.allowed_clients: ..."`
hard-coded — invisible while the function had one caller. Reusing it for
`http.trusted_proxies` exposed it immediately (an invalid configuration named
the wrong directive). Fixed by adding a `directive` parameter to
`fpm_http_acl_parse()` (the signature changed and all four call sites were updated).

**Found incidentally, NOT fixed here (task 015, fixed later)**: the order of
checks in `fpm_conf.c` (the "type-specific directives" section before the
`/* listen */` block) means `type->validate()` (that is,
`fpm_http_validate_pool()` sees `wp->listen_address_domain` still UNSET (zero,
neither `FPM_AF_UNIX` nor `FPM_AF_INET`). As a result, the
`fpm_http_validate_pool()` check "`listen_address_domain != FPM_AF_INET`
requires `http.listen`" is always true, REGARDLESS of whether `listen` is
`host:port` or a Unix socket — so `http.listen` is effectively ALWAYS REQUIRED
today, not only for UDS as the error message and documentation say. Harmless:
the requirement is only TOO BROAD, not too narrow, but the error message is
misleading. Tests in this work therefore always had to add `http.listen` — even
for `listen = 127.0.0.1:9001`. A separate small fix, not touched here to avoid
mixing tasks. Task 015 fixed this by moving the "listen" block in
`fpm_conf_process_all_pools()` before `type->validate()`.

**How it was built and tested**: a separate php-src clone (PHP-8.5,
`/private/tmp`, NOT the user's `~/work/php-src`) + `build/prepare.sh` +
`buildconf --force` + `./configure --disable-all --enable-fpmng` (brew
libevent found through `pkg-config`, brew `bison` before the system one in
`PATH`) + `make`. The server was run live (`php-fpm-ng -y fpm.conf -F`) and
requests were sent with `curl` (including `curl --interface 127.0.0.2` to
simulate a non-loopback peer — this did not work without
`sudo ifconfig lo0 alias`, so the trusted/untrusted test switched
`http.trusted_proxies` between the client address and an address that was not
the client, instead of using two client addresses).

### Hardening

- ~~hard-coded limits: 32 MB body, 64 KB CGI headers~~ — `http.max_body` (task 031) makes the body cap a directive; CGI headers remain a compile-time 64 KB. The gateway's *request* header block was a third case, unbounded (libevent's `EV_SIZE_MAX` default) until issue #117 gave it the same compile-time 64 KiB both HTTP-direct executors use; over it, libevent answers 400 and closes;
- ~~no client-side timeouts (slow loris)~~ — `http.read_timeout` (task 031) bounds one whole client read (headers + body);
- ~~a full pool returns 502; it should return 503 with `Retry-After`~~ — done (task 031);
- no backpressure while sending the body — a large upload lands in gateway memory. Decision (task 031): stays; bounded by `http.max_body`, consequence documented in `docs/node_server_gaps.md`.

### Supervisor and cron reliability

With one instance per VPS there is **no cluster to catch a failure**. Backoff,
an error threshold, and visible logging are features, not hygiene. A non-zero
exit code must be visible, not only at debug level.

### Graceful stopping — VERIFIED LIVE (2026-09-06), with one bug found and fixed

This entry was out of date: the mechanism (`supervisor.stop_timeout`,
`cron.timeout`, the shared watchdog in `fpm_pool_watchdog.c`) had been
implemented for a long time (see 3o, 3r), but this paragraph in section 6 was
not updated. It was re-verified from scratch on a live binary (PHP 8.5.11-dev,
macOS, `sapi/fpmng/php-fpm-ng`) and a **real, serious regression** was found,
described below and now fixed.

#### Stop semantics by pool type

| pool type | SIGTERM sent directly to the CHILD | SIGQUIT/SIGTERM sent to the MASTER (`docker stop`) | hard limit |
|---|---|---|---|
| `fastcgi`/`fastcgi-ng`/`http` (and `fiber`/`async` variants) | the in-flight request finishes; `request_terminate_timeout` is the hard limit | same, through master escalation (`process_control_timeout`) — standard untouched FPM behavior | `request_terminate_timeout` (existing FPM directive) |
| `supervisor` | the current script iteration gets `supervisor.stop_timeout` seconds to finish by ITSELF (nothing specially wakes the process; it simply gets time); after the limit, `SIGKILL` from a separate watchdog process, with `[pool NAME] ... exited on signal 9` in the log | **only if global `process_control_timeout >= supervisor.stop_timeout`** — otherwise the master kills the child through its own escalation FIRST, before our watchdog can act (see below) | `supervisor.stop_timeout`, 10s by default |
| `cron` | if sleeping (before the script starts), it exits immediately and cleanly WITHOUT running the script for that run; if the script is already running, SIGTERM is not caught in this phase, and the only limit is `cron.timeout` armed before the script starts | as above: requires global `process_control_timeout >= cron.timeout` | `cron.timeout`, 0 by default = no limit |
| `status` | no own SIGTERM/SIGQUIT handler — there is nothing to "finish" (it does not run PHP, only reads scoreboards), so the lack of graceful stopping is harmless by definition | same | none (not needed) |

Cron's rule is therefore "finish within `cron.timeout`, but ONLY if the run has
already started; if it is still sleeping, simply skip that run" — exactly the
argument from this section before the fix. Orphans: with `pm.max_children = 1`,
there is no way to leave a running script without a process supervising it
(either the script exits the process itself or the watchdog kills it) — verified
with `ps` after full shutdown, zero orphans in all (a)/(b)/(c) cases.

#### BUG FOUND: `stop_timeout`/`cron.timeout` worked only for the FIRST process iteration

`php_request_startup()` and `php_request_shutdown()` (PHP core, not our code)
replace the `SIGTERM` disposition with Zend's own handler — `zend_sigs[]` in
`Zend/zend_signal.c` includes `SIGTERM`, not only `SIGALRM`/`SIGPROF` used by
`max_execution_time` (a comment in `fpm_pool_supervisor.c` already covered the
latter, but the problem proved broader). This happens on EVERY call, not just
once per process.

`pool.type = supervisor` installs its own SIGTERM handler once in
`fpm_pool_supervisor_child_main()`, before entering the `for(;;)` loop. With
`supervisor.restart = always`, the process lives through many iterations
(`fpm_pool_script_run()` in a loop) — and the FIRST completed iteration silently
replaced the SIGTERM disposition. The result: SIGTERM sent to the child after
at least one completed iteration killed the process IMMEDIATELY through the
default action — no "finish the current job" and no `stop_timeout` watchdog.
Measured directly (temporary debug added at
`sigaction(SIGTERM, NULL, &check)`): immediately after installing the handler in
`child_main()` the disposition was correct, but immediately after the first
return from `fpm_pool_script_run()` it was not (a different function pointer,
not ours, not `SIG_DFL`).

The damage is smaller for `cron` (the process runs ONLY ONE script in its entire
lifetime), but the same mechanism would affect a second run if the model ever
changed to "more than one run per process".

**Fix** (`fpm_pool_script.c`, shared by `supervisor` and `cron`, with no knowledge
of a concrete type — as required by the 3h contract): `fpm_pool_script_run()`
saves the SIGTERM disposition from BEFORE `php_request_startup()` (the one the
caller actually wanted — it may also be `SIG_DFL` if the caller installed
nothing, in which case restoring it is a no-op) and restores it immediately:
after `php_request_startup()`, and a second time after
`php_request_shutdown()` (in case shutdown also replaces it — measured that it
does). This fixes `supervisor` and `cron` together, without any
`if (type == ...)`.

Verified live after the fix, repeatedly, AFTER at least several completed
iterations (not only the first):
```
[pool consumer] child 39020 exited with code 0 after 11.77s   # (a) SIGTERM during a short job -> exits by itself, exit 0
[pool consumer] child 39136 exited on signal 9 (SIGKILL) after 26.75s  # (b) job longer than stop_timeout=3s -> hard kill after ~3s
[pool job] child 39510 exited on signal 9 (SIGKILL) after 49.01s  # cron.timeout=3s counted from SCRIPT start (started at :07:00, killed at :07:03), not from process start
```

#### SECOND PROBLEM FOUND (not fixed in code because it cannot be — documented and warned about)

`docker stop`/systemd send SIGTERM (or the configured `STOPSIGNAL`) to PID 1,
the MASTER, not directly to the child. In `TERMINATING`, the master
(`fpm_process_ctl.c`, reference and untouched) sends SIGTERM to children and
escalates ITSELF to SIGKILL after `process_control_timeout` seconds (default
**0**, effectively immediate). Our `supervisor.stop_timeout`/`cron.timeout` never
get a chance to act when `process_control_timeout` is smaller — the child dies
from the MASTER's SIGKILL, not our watchdog.

Measured: with the default `process_control_timeout = 0`, SIGTERM to the master
kills the job immediately (without finishing the current iteration); with
`process_control_timeout = 5` (>= `supervisor.stop_timeout = 3`), the job gets
its three seconds, the watchdog kills it, the master waits, and there are no
orphans.

**This cannot be fixed inside the pool type** — `process_control_timeout` is
global (shared by all pools) and `fpm_process_ctl.c` is a reference file
(untouched, as required by the contract). Instead, `supervisor` and `cron`
(`fpm_pool_supervisor_init_main()`, `fpm_pool_cron_init_main()`) log a LOUD
warning at startup if `process_control_timeout < stop_timeout/cron.timeout`
for that pool, including the pool name and a concrete suggested value.
**Record this in future user documentation as a configuration requirement, not
only here**: anyone who wants SIGTERM/`docker stop` to give a job time to finish
must set `[global]` `process_control_timeout` to at least the largest
`stop_timeout`/`cron.timeout` in the configuration.

#### PHP gets a chance to clean up

`fpm_pool_script_run()` (`fpm_pool_script.c`) calls
`php_request_startup()` / `php_execute_script()` / `php_request_shutdown()` for
EVERY iteration/run — destructors, `register_shutdown_function()`, closing
streams/connections, and flushing output buffers all use the normal PHP path,
regardless of whether the iteration ends by itself or termination was requested
by SIGTERM (we do not interrupt the request specially; we give it time and do
not kill it externally until `stop_timeout` expires). The only exception is when
the watchdog actually kills the process (`SIGKILL`, because the job exceeded the
limit) — by definition SIGKILL gives NO code a chance to react, so
`php_request_shutdown()` does not run for THAT iteration. This is deliberate:
it is the boundary between graceful stopping and "there is no way out" — the
hard limit must be hard.

Separately, `child_main()` (for `supervisor`/`cron`) never calls
`php_module_shutdown()` when the process exits normally (`exit()`), unlike a
classic FastCGI worker, which calls it in `fpm_main.c` after returning from
`fpm_run()` (reference file). We checked the real cost:
`php_module_shutdown()` is extension-level shutdown (MSHUTDOWN), not request
shutdown — most meaningful cleanup (destructors, output buffers, closing
connections) already happened through `php_request_shutdown()` above, after
EVERY run. Omitting `php_module_shutdown()` at process exit matters only to
extensions doing work at complete-process shutdown (for example, saving OPcache
statistics) — for our case (the process is about to die and the system reclaims
resources anyway) it is acceptable, but **not verified with a concrete
extension that would suffer from it**. It is recorded as deliberate omission,
not an oversight.

### What we do NOT do

HTTP/2, compression, cache. These are things nginx exists for.

### Scratch: files the application will lack (not us)

- no `/etc/resolv.conf` → no DNS; a database connection by hostname fails;
- no certificate bundle → every `https://` request to an external API fails;
- no `tzdata` → `date()` is in UTC regardless of `date.timezone`.

None can be solved in code — these are data, not features. In practice this is
`FROM scratch` + binary + code + three data files. Say so honestly from the start.

### Configuration

`include=` **already works**, with a glob (`fpm_conf.c:1383`, using `php_glob`).
So `include=/etc/fpmng/conf.d/*.conf` is ready; nothing needs to be written.

Project requirement: a file for "application + two consumers + three crons" must
fit in about forty lines. If it takes two hundred, we lose to docker-compose and
supervisord despite better technology.

No shell in scratch means no entrypoint. Configuration must be fully declarative,
plus environment-variable substitution in the file. **To check** how much FPM
can do today — without this, one image cannot serve both dev and production.

## 7. Work order

0. **Static musl binary in scratch, serving `hello.php`.** One day's work;
decides whether the concept stands. The rest of the plan is sensible regardless,
but this is the only assumption that can take everything down with it.
0b. Decide TLS (section 5).
1. `pool.type` as the seam: an operations table, default `fcgi`, with `fcgi` and
   `http` only dispatching existing code. No new functionality — the point is to
   see whether the abstraction is good. If `http` does not fit cleanly, it is wrong.
2. `supervisor`. Test the abstraction with a case genuinely different from `fcgi`.
3. `cron`. A second launch policy on the same machinery. A good seam test: if it
   requires touching `fpm_children.c`, the spawning policy was not separated cleanly.
4. Metrics. Decide SAPI versus extension here.
5. Static files.
6. Self-runner (section 3a) — independent of the rest, but the 3a project
   constraint must already be satisfied at item 1.
7. A proper reload: sockets survive, requests finish, and the consumer gets
   SIGTERM and finishes its job. Optionally watch configuration files. **Not** a
   real hot reload — postpone it until it is clear anyone needs it.
8. TLS + ACME (section 3l).
9. `pool.type = proxy` — DECISION (2026-09-05): build it, but **last**, after TLS.
   The gateway holds :443, terminates TLS, handles ACME and static files itself,
   and passes the rest over ordinary HTTP/1.1 on localhost to a long-lived
   application process (amphp, ReactPHP, Octane, anything). Ordering reason:
   without TLS and ACME this type adds nothing — any tool can pass HTTP on
   localhost. Its value comes from combining "one binary holds the certificate
   and static files" with "the application is a separate process". Context and
   origin: section 3s.

### Status as of 2026-09-06

Done: 1 (`pool.type` with the extensibility contract), 2 (supervisor), 3 (cron),
4 (metrics: `pool.type = status` on 2026-09-05 plus application metrics from
PHP, see 3w), 5 (static files), 7 (graceful reload, see 3x), 0 (static musl
binary) — plus syscall optimizations for the blocking worker (3t) and
experimental Fiber/async executors (POCs).
Currently in progress: nothing open.
Remaining: 6 (self-runner), 8 (TLS+ACME), 9 (proxy), and the long tail of gateway
gaps from section 6.

## 8. Maintenance: a new PHP version means a rebuild

The SAPI is compiled **into** the PHP binary and has no stable ABI. Every release
(8.5.11 → 8.5.12) requires rebuilding the binary against that tag.

This is not a flaw in the architecture — distributions do exactly the same with
their `php-fpm` packages. And since we ship a container image anyway,
"rebuild" means "CI runs against the new tag and pushes the image".

Work scale:
- **patch** (8.5.11 → 8.5.12): mechanical, internal APIs do not change; CI simply passes;
- **minor** (8.5 → 8.6): may require fixes, mainly in `fpm_main.c`;
- **major** (9.0): certainly requires fixes.

Therefore php-src is pinned by **tag, not branch**, and bumped deliberately. CI
with a matrix of two or three PHP versions is the only way to learn early that
upstream changed something.

## 9. Formal matters

- **License**: the code comes from FPM → PHP License 3.01. There is no choice.
- **Name**: "PHP" is a trademark of the PHP Group, which has a usage policy.
  `php-fpm-ng` as a product name invites a letter asking us to change it. It can
  remain provisional; choose something of our own before publication.


## 3t. `pool.type = async` — POC RUNNING on the True Async fork (agent, 2026-09-05)

Continuation of 3s (there: "we are not building this"). This is a feasibility
study from code, not descriptions. Everything below comes from source reading
(file:line) or was measured locally on a Mac (arm64, NTS, `--enable-debug`). Code:
`sapi/fpmng/fpm/fpm_pool_async.c` (+ `.h`, one line in `fpm_pool_types[]`). Nothing
went upstream. Fork sources and builds:
`~/work/true-async/{php-src,php-async,build,php-upstream,build-upstream}`.

### What the fork actually exposes (from the code)

The `true-async/php-src` fork has many branches; the one on which the extension
builds is `true-async-stable` (2026-08-26, ABI "TrueAsync ABI v0.26.0",
`Zend/zend_async_API.h`, 3060 lines; the extension README points at
`true-async-api`, which is based on 2025-09 and is outdated). Versus upstream
master: 228 files, +21k lines. A separate `async-core-master` branch (2026-07-03)
has "AsyncCore ABI v0.1.0", a 573-line header with scheduler SLOTS
(`new_coroutine`, `enqueue`, `suspend`, `resume`, `cancel`, `launch`,
`intercept_fiber`, `defer`) and NOTHING else — no reactor, events, or poll. This
is what is intended for 8.7. For the SAPI this means: the ABI alone cannot wait
on a descriptor; the provider (extension) does that.

The scheduler and reactor are in the `true-async/php-async` extension (libuv,
33k lines of C; `scheduler.c`, `coroutine.c`, `libuv_reactor.c`). The engine
provides function slots (`zend_async_scheduler_register`,
`zend_async_API.c:402`) and `zend_async_is_enabled()` means the scheduler and
reactor are registered (`:305`). The scheduler starts LAZILY at the first
`spawn`/`suspend` (`scheduler.c:1153 async_scheduler_launch`), turning the
current run into the "main coroutine"; it requires `EG(active_fiber) == NULL` and
extension RINIT.

Running code from C: `ZEND_ASYNC_SPAWN()` (`zend_async_API.h:2726`) returns a
`zend_coroutine_t*`; set `internal_entry` (`void(*)(void)`) and `extended_data`,
and it starts at the next yield. Waiting for an fd:
`ZEND_ASYNC_NEW_SOCKET_EVENT(fd, ASYNC_READABLE)` + `ZEND_ASYNC_WAKER_NEW` +
`zend_async_resume_when(...)` + `ZEND_ASYNC_SUSPEND()` — the pattern in
`main/network_async.c:241 network_async_await_stream_socket`. It works from C,
verified below in the acceptor.

Where the engine suspends on I/O (fork, `git diff master..true-async-stable`):
`main/streams/xp_socket.c` (+346, every read/write/connect/accept through
`network_async_await_stream_socket`), `main/network.c` (`php_poll2` ->
`php_poll2_async` when `ZEND_ASYNC_IS_ACTIVE`),
`main/streams/plain_wrapper.c` (+743, files/pipes through
`ZEND_ASYNC_IO_CREATE`), `ext/standard/basic_functions.c` (`sleep_async` for
sleep/usleep/time_nanosleep), `ext/curl/curl_async.c` (+2278),
`ext/pdo_pgsql`/`ext/pgsql` (libpq through socket polling), `ext/sockets`, and
`ext/standard/dns.c`. Thus mysqlnd (streams) and libpq are handled IN THE ENGINE
— more than our `php_stream_xport_register` in 3s.

### What is per coroutine TODAY (derived from code, not description)

- Fiber switch (`Zend/zend_fibers.c:121-160 zend_fiber_vm_state`):
  `vm_stack*`, `current_execute_data`, `error_handling`, `exception_class`,
  `jit_trace_num`, `active_fiber`, `bailout`. That is all from EG.
- `main/output.c`: the `ob_*` stack through the coroutine's "internal context"
  key (`php_output_get_async_context`, main-coroutine start handler `:209`,
  `:1663`).
- `main/network_async.c:1586`: `hostent` cache.
- `EG(shutdown_context)` (destructors at coroutine shutdown).
- SG: ZERO changes (`main/SAPI.c`, `main/php_variables.c` — empty diff).
- `EG(symbol_table)`, `function_table`/`class_table`, `included_files`, ini,
  `error_reporting`, `user_error_handler`, `memory_limit`, and timeout: SHARED.

The beginning of the mechanism EXISTS and is public: **switch handlers** —
`zend_coroutine_switch_handler_fn(coroutine, is_enter, is_finishing)`
(`zend_async_API.h:269`), `ZEND_COROUTINE_ADD_SWITCH_HANDLER` (`:3043`),
called in `ext/async/scheduler.c:1697` (LEAVE before switching) and `:1719`
(ENTER after resuming), with FINISH in `coroutine.c` during finalization. The
fork itself uses this for `ob_*`. Our POC uses it for SG and the symbol table —
and that is SUFFICIENT without VM changes (see below).

The `global-isolation` branch (af6c53037, 2025-11-30) made
`EG(symbol_table)` per coroutine (`coroutine->symbol_table`, `zend_execute.c`,
`zend_vm_def.h` +116) and per-scope superglobals (`scope->superglobals`) — it
**never entered `true-async-stable`** (`git branch --contains` = that branch
only). Per-coroutine class statics were implemented and reverted (858ece3db).
The fork author therefore tried condition 1 from 3s and abandoned it; in stable,
request state is per process.

### POC: what works and how (MEASURED)

`child_main` (one process, `pm = static`, `pm.max_children = 1`):
1. ONE `php_request_startup()` — the "request container" (executor, extension
   RINIT, arena). `zend_unset_timeout()`, because `max_execution_time` would
   apply to the process.
2. `ZEND_ASYNC_SPAWN()` for the acceptor: waits for `listening_socket` through a
   poll event, `fcgi_init_request` + `fcgi_accept_request` (accept immediately;
   `poll` on the new fd is asynchronous in the fork through `php_poll2`; header
   `read` is blocking, but the data is already there), then
   `ZEND_ASYNC_SPAWN()` for the request coroutine.
3. Request coroutine: fresh SG copy (the equivalent of `sapi_activate` +
   `init_request_info`), fresh `EG(symbol_table)` and `EG(included_files)`
   (`zend_hash_init` like `init_executor`), re-arm auto-globals, and a switch
   handler that swaps all of `sapi_globals` and both HashTable headers with
   `memcpy` (the address of `&EG(symbol_table)` does not change; its contents do
   — the script frame keeps the pointer and IS_INDIRECT entries point into this
   coroutine's VM stack). `zend_execute_scripts` (NOT `php_execute_script`, see
   traps), `sapi_send_headers`/`sapi_flush`, `fcgi_finish_request` without
   keep-alive, and `zend_hash_graceful_reverse_destroy` for the symbol table.
4. The main coroutine wakes every second and checks `fcgi_in_shutdown()`.

Results (PHP FastCGI client, N parallel connections, one worker process):

    4 x slow.php (usleep 500 ms)       async: 503 ms total    fcgi: 2008 ms
    4 x net.php (fsockopen -> server
      responding after 500 ms)          async: 537 ms           fcgi: 2140 ms
    50 x slow.php (100 ms)              async: 105 ms
    200 x slow.php (0 ms)               async: 22 ms (~110 us/req, debug build)
    child RSS after 2000 requests       8624 KB -> 8624 KB (arena reclaimed)

Correctness: every request received ITS OWN `X-Req` header, `$_GET`,
`$_SERVER['REQUEST_URI']`, `$GLOBALS`, and `get_included_files()`; warnings and
"Uncaught RuntimeException" reach the correct client; SIGQUIT to the master ->
all processes disappear in < 2.5s.

### What does NOT work and which code causes it (measured, not inferred)

- **Function and class tables are per process.** `classdef.php` (which defines a
  class and functions) works ONCE; every later request in that process gets
  "Fatal error: Cannot redeclare function helper()" — permanently, because
  `EG(function_table)` is `CG(function_table)`, populated during compilation
  and cleared only in `shutdown_executor()`. Swapping the tables per coroutine
  would require copying thousands of internal entries per request or changing
  the engine. This is the real wall for frameworks today (every request loads the
  same classes). With OPcache the same problem takes another form:
  `zend_accel_load_script` binds classes to `EG(class_table)` per request.
- **OPcache assumes one request per process.** Initially, with OPcache enabled,
  requests 2..N lost `$_GET`/`$_SERVER`: a cache hit skipped the compilation on
  which the POC relied to run auto-global callbacks. The POC now explicitly
  calls `zend_is_auto_global_str()` for every request; test 3 x 4 parallel
  requests with OPcache active retained separate `$_GET`, `$_SERVER`, `$_COOKIE`,
  `$GLOBALS`, and `get_included_files()` (4 x 500 ms in 506–508 ms). It is still
  **recommended to use `opcache.enable = off`**: OPcache and its
  `ZCG(cwd)`, `ZCG(include_path)` state were not designed for many interleaved
  requests in one process, and the test does not prove isolation of every
  extension path.
- **No keep-alive on the pool side**: `fcgi_accept_request` reads blockingly on
  an open fd (`main/fastcgi.c:1445`, without poll). Our HTTP gateway keeps
  connections persistent — add an asynchronous poll on the fd before reading.
- **POST**: `sapi_cgi_read_post` uses a static `request_body_fd` (`fpm_main.c`),
  shared by all in-flight requests. The POC tested GET.
- No `X-Powered-By` and no per-request `php_request_startup/shutdown`, so
  extension RINIT/RSHUTDOWN do not run per request (sessions, mysqlnd stats,
  per-request `set_time_limit` and `memory_limit` are absent).
- A fatal error in any coroutine: our `zend_try` catches the bailout in the
  coroutine, but `CG(unclean_shutdown)` and engine state after E_ERROR are shared.
- Child logs do not reach `error_log` (FPM closes zlog in the child without
  `catch_workers_output`) — our `NOTICE`/`DEBUG` from `child_main` disappear.

### Traps encountered (each is a concrete code path)

1. `php_execute_script()` in the fork calls
   `ZEND_ASYNC_RUN_SCHEDULER_AFTER_MAIN` (`main/main.c php_execute_script_ex`),
   which means `suspend(from_main=true)` ->
   `async_scheduler_main_coroutine_suspend` (`scheduler.c:1315`), which FINALIZES
   the current coroutine as the main one. From inside a request coroutine, call
   `zend_execute_scripts()` directly.
2. A Fiber coroutine has an artificial internal-function frame at its bottom
   (`scheduler.c:1786-1811 fiber_entry`, `root_function`). With non-NULL
   `EG(current_execute_data)`, `zend_execute()` searches up the stack for the
   symbol table (`zend_rebuild_symbol_table`) and gets NULL -> SIGSEGV in
   `zend_attach_symbol_table` (`zend_execute_API.c:2029`). Set
   `EG(current_execute_data) = NULL` while executing the script.
3. A shared `EG(symbol_table)` is more than a `$GLOBALS` leak: in
   `zend_attach_symbol_table`, the second main script BITWISE takes over (without
   addref) the first script's CV values with the same names and frees them under
   it. Measured: SIGABRT in `gc_possible_root` (`zend_gc.c:800`) on `net.php`
   when `$fp` held a socket resource. Scalar scripts "worked" by accident. The
   POC solution is a per-coroutine table swapped by the switch handler.
4. `fiber_entry` sets `EG(error_reporting)` from ini instead of inheriting it
   (`scheduler.c:1766-1813`); without php.ini it becomes 0, and warnings and
   "Uncaught" disappear. Inherit the container value manually.
5. `fpm_main.c:1800-1801` replaces `php_import_environment_variables` with a
   variant that reads the FastCGI environment ONLY after `fpm_run()` returns.
   `child_main` does not return, so `$_SERVER` contained only the process environment.
6. Building the fork on macOS: `ext/async/thread.c:3222` declares a weak
   `OPENSSL_thread_stop` symbol — Apple's linker requires
   `-Wl,-U,_OPENSSL_thread_stop`. Apart from that, the fork + extension + our
   `sapi/fpmng` build together without any patch (`prepare.sh` works on the fork
   as on upstream; the fork's `main/fastcgi.c` is identical to upstream).

### Engine detection (point 4) — verified on both binaries

Compile time: `__has_include("zend_async_API.h")` (only the fork has the header;
`-IZend` is always present). Runtime in `validate()`: `zend_async_is_enabled()`
is possible because `fpm_init()` runs AFTER `php_module_startup()`
(`fpm_main.c:1749` versus `:1765`), so ext/async MINIT has already registered
the scheduler. `ZEND_ASYNC_API` also provides the ABI version for the message.
On the upstream binary (`~/work/true-async/build-upstream`, PHP 8.6.0-dev
without the API):

    ALERT: [pool async] pool.type = async requires a PHP engine with the True
    Async API (Zend/zend_async_API.h); this binary is PHP 8.6.0-dev without it

`-t` on the fork: `pm = dynamic` and `request_terminate_timeout` are rejected
clearly, and a correct configuration passes.

### Point 6: `php_stream_xport_register()` — confirmed in upstream

`PHPAPI` (`main/streams/php_stream_transport.h:32`), with the registry using
`zend_hash_update_ptr` (`transports.c:30`) — the last registration wins;
`_php_stream_xport_create` looks up by protocol name (`transports.c:109`).
Default `tcp/udp/unix/udg` are registered in `streams.c:1765-1769`; ext/openssl
OVERWRITES `tcp` in MINIT (`openssl.c:832`) and restores it in MSHUTDOWN (`:906`)
— our extension must therefore load AFTER openssl. mysqlnd:
`mysqlnd_vio.c:210` calls `php_stream_xport_create(scheme...)` with
`tcp://`/`unix://` (`:278-305`) — it is caught. phpredis does the same at
`library.c:3345`. Upstream provides `ZEND_API zend_fiber_suspend/resume` for
suspending a fiber from C (`zend_fibers.h:135-136`). The 3s thesis stands:
condition 2 is within our reach on upstream; condition 1 (request state) is not.

### Next step, if continuing

Not "more I/O" — the fork already has I/O. Three things are missing, all in the
engine, not the SAPI: (a) function/class tables per request or a mechanism for
"the script is already loaded in this process" (what FrankenPHP/Octane worker
mode does: application loaded ONCE, request = a function call — the realistic
model for `async`, not "every request from scratch"); (b) per-coroutine
`RINIT/RSHUTDOWN` or a list of safe extensions; (c) OPcache aware of many
requests per process. Our side (keep-alive, POST, scoreboard, child logs) is a
day of work and not the hard part.

## 3u. `pool.type = fiber` — POC on upstream PHP (2026-09-05)

Goal: test the variant without the True Async fork. It uses the public Fiber API
from upstream PHP (`zend_fiber_start/resume/suspend`) and a custom libevent
scheduler. One `pm = static` process handles many FastCGI requests; each request
runs in its own fiber.

### What was implemented

- `fpm_pool_fiber.c`: accept, libevent scheduler, Fiber lifecycle, and FastCGI
  keep-alive;
- `fpm_pool_fiber_xport.c`: interception of `tcp://` and `unix://` transports
  through `php_stream_xport_register()`, suspending the fiber on
  read/write/connect;
- `fpm_pool_coop.c`: separate SG, output stack, `$GLOBALS`, superglobals,
  `included_files`, and error handlers for each request;
- validation enforces NTS, `pm = static`, and disabled OPcache.

The code builds against clean upstream PHP 8.6.0-dev (`5be4de10`, NTS, debug)
without the True Async API or the `async` extension.

### Results on `192.168.8.103`

Four concurrent requests, each doing `fsockopen()` to a server responding after
500 ms, finished in **508 ms**. There were four in-flight requests in one
process. Each retained its own `$_GET`, `REQUEST_URI`, headers, cookie,
`$GLOBALS`, a variable from `require`, and `get_included_files()`.

The control test, 4 x `usleep(500 ms)`, took **2009 ms**. This is expected and
defines the boundary of this variant: upstream PHP does not pass `sleep`/`usleep`,
curl, libpq, or ordinary files to our scheduler. Concurrent operations are
socket-stream operations using intercepted transports (including `fsockopen`,
mysqlnd, and typically phpredis).

Active OPcache is rejected by `php-fpm-ng -t` with a message telling the user to
set `php_admin_flag[opcache.enable] = off`. In this variant this is not merely a
recommendation: OPcache keeps state assuming one request per process, including
the auto-global mask and script timestamps.

### Limitations — POC, not a replacement for ordinary FPM

- function and class tables are process-wide; an application loading definitions
  on every request hits redeclarations. A realistic production model would load
  the application once and call an explicit request handler;
- no per-request `php_request_startup()`/`php_request_shutdown()` means no full
  extension RINIT/RSHUTDOWN and no isolation of `ini_set`, memory limits,
  timeouts, class statics, or shutdown functions;
- only `tcp` and `unix` transports are intercepted; blocking APIs outside them
  block the whole process;
- POST reading is functionally correct but blocking; a large or slow body blocks
  every request;
- a fatal/bailout can leave shared engine state damaged;
- the FPM scoreboard does not represent multiple requests in one process, so
  timeouts/slowlog and `pm.max_requests` are rejected.

### `pool.type = http-fiber`

A thin composition of the existing HTTP gateway and the Fiber executor. It does
not copy the parser or scheduler: `init_main` comes from the gateway and
`child_main` from `fiber`. The classic gateway limits FastCGI connections to the
number of workers because each connection pins a blocking worker. For
`http-fiber`, the default limit is 128 per pool and can be changed through
`FPM_HTTP_MAX_UPSTREAMS`.

A direct HTTP test: four requests, each waiting 500 ms on a socket, finished in
**514 ms**. The log confirmed four in-flight requests in one Fiber worker and a
128-connection gateway budget. `async`, `fiber`, and `http-fiber` remain clearly
**experimental and are not intended for production**.

### k3d benchmark: nginx + FPM versus `http-fiber`

The same PHP 8.6.0-dev release build, same application code, and one pod on the
same k3d node. `wrk`: 2 threads, 32 connections, 8 seconds, three runs; median
requests/s below. The classic variant is nginx 1.29 + upstream FPM, while
`http-fiber` is one gateway and one Fiber worker. OPcache remains realistic:
available to classic FPM and disabled for Fiber because the latter rejects active
OPcache.

| test | nginx + FPM, 1 worker | nginx + FPM, 8 workers | `http-fiber`, 1 worker |
|---|---:|---:|---:|
| CPU: 1000 x SHA-256 | 1821 req/s | 4846 req/s | 2024 req/s |
| `usleep(50 ms)` | 16 req/s | 157 req/s | 16 req/s |
| socket I/O, response after 50 ms | 16 req/s | 145 req/s | **618 req/s** |

For socket I/O, median p50 was about 1.65 s, 207 ms, and **51.6 ms**;
p99 about 1.92 s, 547 ms, and **53.2 ms**. `http-fiber` reported no errors.
With `usleep`, the one-worker variants have the same throughput; percentiles are
distorted by `wrk` timeouts and do not change the conclusion that `usleep`
blocks the process.

Approximate RSS after the tests (summing processes overstates shared memory):
classic 1 worker + nginx about 29 MB, classic 8 workers + nginx about 90 MB,
`http-fiber` (master + gateway + worker) about 29 MB. The socket-delay backend
used separate resources and is not included.

#### Apples-to-apples comparison: 4 workers versus 4 workers

The test host has four physical cores. Both variants received a 4-CPU limit and
four PHP processes; remaining conditions as above. Median of three runs:

| test | nginx + FPM, 4 workers | `http-fiber`, 4 workers | difference |
|---|---:|---:|---:|
| CPU: 1000 x SHA-256 | 5255 req/s | **6210 req/s** | Fiber +18% |
| `usleep(50 ms)` | 76,3 req/s | 76,8 req/s | practically tied |

The first socket-I/O test used `ThreadingTCPServer`; at Fiber throughput the delay
generator itself began creating hundreds of threads and the result fell in later
runs. We do not treat that measurement as authoritative. The repeat used the
single-threaded `asyncio` server, five 10-second runs, and alternating variants:

| socket I/O 50 ms | nginx + FPM, 4 workers | `http-fiber`, 4 workers |
|---|---:|---:|
| requests/s (median) | 77,0 | **572,4** |
| p50 | 415,2 ms | **55,4 ms** |
| p99 | 416,6 ms | **59,7 ms** |

That is **7.4x throughput** and 7.5x lower p50 for Fiber. Results are close to
the model limits: four blocking workers at 50 ms theoretically give 80 req/s,
while 32 concurrent connections give 640 req/s. Fiber achieved about 95% and 89%
of those limits. There were no errors or timeouts in the corrected run.

With 128 clients (`wrk -t4 -c128`, 5s timeout, five interleaved 10-second runs,
`FPM_HTTP_MAX_UPSTREAMS=256`), classic FPM remained at a median of
**76,5 req/s**, p50 **1,66 s**, p99 **1,66 s**. `http-fiber` reached a median of
**1924 req/s**, p50 **64,5 ms**, p99 **87,1 ms** — about **25x throughput**.
The theoretical limit from 128 / 50 ms is 2560 req/s, so Fiber's median is about
75% of that value. Run-to-run spread was significant (1442–2022 req/s), so do
not present the best result as fixed performance. Both pools remained free of
restarts and log errors.

### Conclusion

The experiment confirms that a Fiber-based variant is feasible on upstream PHP
and removes the dependency on the True Async fork. It does not automatically
make PHP asynchronous: it provides concurrency only at points explicitly
integrated with the scheduler. The direction for further development is worker
mode: load the application once, use an isolated request/response object, and
expand the I/O adapter list instead of pretending to implement the full classic
FPM request lifecycle.

## 3v. Splitting frontend and executor (2026-09-05)

Request-pool configuration has two dimensions:

```ini
pool.type = fastcgi | fastcgi-ng | http
pool.executor = classic | fiber | async
```

No `pool.type` means `fastcgi`; the explicit historical `fcgi` remains a
compatibility alias. `fastcgi` retains the classic path and does not accept
`pool.executor`. `fastcgi-ng` means the optimized FastCGI frontend, while `http`
is the built-in HTTP gateway. For the latter two, an omitted `pool.executor`
means `classic`. The `fiber` and `async` executors remain experimental and are
not intended for production; Fiber requires OPcache disabled, and that remains
recommended for Async.

The former combined types become:

- `fiber` -> `pool.type = fastcgi-ng`, `pool.executor = fiber`;
- `async` -> `pool.type = fastcgi-ng`, `pool.executor = async`;
- `http-fiber` -> `pool.type = http`, `pool.executor = fiber`.

## 3w. Splitting transport and `writev()` for large responses (2026-09-05)

The process-wide `fcgi_set_optimized_transport()` switch is disabled by default
and set after the worker forks. `fastcgi` retains the upstream `accept()` and
read path, while `fastcgi-ng` and the internal `http` frontend transport enable
buffered reads, `accept4(SOCK_CLOEXEC)`, and optimized writes for large records.
A test with two pools in one binary confirmed that the setting does not leak
between processes.

For a lightweight request, four workers and five alternating
`wrk -t1 -c2 -d10s` runs produced:

- `fastcgi`: 8718,57 req/s and 114,244 us worker CPU/request;
- `fastcgi-ng`: 8866,06 req/s and 104,377 us worker CPU/request;
- result: **-8,64% CPU/request** for `fastcgi-ng`.

The syscall profile confirmed `accept()` only for `fastcgi`, `accept4()` only
for `fastcgi-ng`, and a drop in reads from about 9.1 to 4.1 per request.

### Large responses

`patches/0005-fastcgi-writev-large-response.patch` combines the record header and
large body with `writev()` only for the optimized Unix transport.
`safe_writev()` handles `EINTR` and partial writes across both vectors. Classic
`fastcgi` and Windows retain the existing path.

For a 262 144 B response, transport operations fell from 11 to 6. The final
`strace` on PHP 8.5 showed:

- `fastcgi`: 11 FastCGI writes, 0 `writev()`;
- `fastcgi-ng`: 5 `writev()` calls with header and payload plus a separate final record.

PHP 8.5 benchmark, five alternating runs:

| frontend | CPU/request baseline | CPU/request `writev` | CPU change | req/s baseline | req/s `writev` | req/s change |
|---|---:|---:|---:|---:|---:|---:|
| `fastcgi-ng` | 195,433 us | 178,824 us | **-8,5%** | 2018,63 | 2037,15 | **+0,9%** |
| `http`, `Connection: close` | 525,209 us | 490,612 us | **-6,6%** | 2682,67 | 2705,14 | **+0,8%** |

In both tests `writev()` had lower CPU/request in all five pairs. The first HTTP
keep-alive run reached about 50 req/s because of Nagle/delayed ACK, so its
throughput was not used as the optimization result.

### `TCP_NODELAY` on the HTTP listener

The gateway set `TCP_NODELAY` on the connection to the worker, but not on the
client connection. Setting the option once on the listening socket is inherited
by accepted sockets and adds no syscall to the request path.

For a 262 144 B response over keep-alive, five alternating
`wrk -t1 -c2 -d10s` runs:

- throughput median: 49,74 -> 2686,13 req/s (**+5300%**);
- combined gateway and worker CPU/request: 643,939 -> 494,200 us (**-23,25%**);
- every baseline run was between 49–53 req/s;
- every `TCP_NODELAY` run was between 2648–2696 req/s.

The small response showed no improvement (medians 12833,55 and 12668,64 req/s),
as expected: it fits in one write and does not trigger a delayed small final
fragment. Regression passed for small and large responses, binary 65 792 B POST,
keep-alive, `Connection: close`, and a disconnected client.

### PHP 8.5 syscall profile after the fix

`strace -f -c` was used only to count calls, not to compare req/s. The profile
covered three frontends, TCP/UDS to the worker, and keep-alive/close:

| frontend | upstream | client | syscalls/request |
|---|---|---|---:|
| `fastcgi` | TCP | keep-alive / close | 32,289 / 32,296 |
| `fastcgi` | UDS | keep-alive / close | 31,289 / 31,283 |
| `fastcgi-ng` | TCP | keep-alive / close | 25,228 / 25,237 |
| `fastcgi-ng` | UDS | keep-alive / close | 24,232 / 24,227 |
| `http` | TCP | keep-alive / close | 32,233 / 41,254 |
| `http` | UDS | keep-alive / close | 32,256 / 41,271 |

Compared with classic `fastcgi`, `fastcgi-ng` removes about 5 `read()` and 2
`fcntl()` calls per request. UDS saves about one syscall/request, mainly
`setsockopt(TCP_NODELAY)`.

The shared worker path still performs about 8 `rt_sigaction`, 2 `times`, 2
`setitimer`, 2 `chdir`, 1 `getcwd`, and 2 `fcntl` calls per request. For HTTP
keep-alive, the gateway mainly adds 4 `epoll_ctl`, 3 `epoll_wait`, one each of
`readv`, `writev`, and `ioctl`. `Connection: close` adds about 9 HTTP
syscalls/request: `epoll_ctl` grows from 4 to 8, with about 2 `accept4`, one
additional `epoll_wait`, and `shutdown`.

### Persistent signal handlers in optimized frontends

`fastcgi-ng` and `http` install the Zend handler set during the first request,
then leave it in place for the worker lifetime. Resetting the logical handler
table still happens per request, as does installing `SIGPROF` for
`max_execution_time`. Classic `fastcgi` retains upstream behavior.

For `fastcgi-ng`, syscalls fell from 25,228 to 18,173/request and
`rt_sigaction` from about 8 to about 1/request. Five alternating small-response
runs produced CPU/request 86,615 -> 80,569 us (**-6,98%**). For HTTP:
111,041 -> 103,964 us (**-6,37%**) and 12850,98 -> 13853,09 req/s (**+7,80%**).
The final comparison on one binary, `fastcgi` -> `fastcgi-ng`, produced 94,964
-> 80,499 us CPU/request (**-15,23%**); the -0.58% req/s difference is not
considered a gain.

The `classic` executor regression covered a 1s timeout and a subsequent request
on the same worker, `pcntl_signal()` reset between requests, graceful shutdown,
small and large responses, binary POST, and a disconnected client for `fastcgi`,
`fastcgi-ng`, and `http`. The case of an extension replacing a handler through
libc `sigaction()` rather than the Zend API deliberately changes: with the
default `zend.signal_check=0`, the handler is not automatically fixed on the
next request. `zend.signal_check=1` still checks at request end. Fiber does not
have full request startup/shutdown, so it provides no timeout or `pcntl`
isolation; details are in `docs/fiber_errors.md`.

### PHP 8.5 release validation after signal optimization

A clean `--enable-fpmng` build revealed that `sapi/fpmng/config.m4` did not run
the `epoll`/`kqueue` tests that upstream runs only inside the `--enable-fpm`
block. The standalone FPM-NG binary therefore rejected `pm = ondemand` on Linux.
Independent `PHP_FPMNG_EPOLL` and `PHP_FPMNG_KQUEUE` macros were added; after the
fix `HAVE_EPOLL=1`, and `ondemand` starts without building the classic FPM SAPI.

A PHP 8.5.11-dev release build from upstream commit `67d1476d4d`, `-O2 -DNDEBUG`,
passed for `fastcgi` (`static`, `dynamic`, `ondemand`), `fastcgi-ng` (`classic`,
`fiber`), and `http` (`classic`, `fiber`). Small and large responses, binary
POST, keep-alive/close, a disconnected client, recovery, reload, and stop were
checked. Classic also passed a timeout, `pcntl` reset, and controlled fork. Async
was correctly rejected because clean upstream PHP 8.5 has no True Async API.

The final five alternating release pairs `fastcgi` -> `fastcgi-ng`
(`wrk -t1 -c2`) produced median CPU/request 93,601 -> 79,615 us (**-14,94%**).
Median throughput 9155,85 -> 9028,11 req/s (**-1,40%**) is not an improvement.
Additional regression of the current HTTP gateway confirmed CGI variables,
trusted `X-Forwarded-*`, Basic Auth, two gateways, 40 non-interleaved access-log
entries, and no way to inject a new line through `REMOTE_USER`.

### Zero-copy / DMA — postponed

DMA is not a direct API for responses generated by PHP. `sendfile()` makes sense
only for static files; first check whether libevent already uses it.
`MSG_ZEROCOPY` may be a candidate for large TCP responses, but needs completion-
queue handling and measurement through a physical interface; loopback is not
authoritative. `splice()` is unattractive because the gateway must parse
FastCGI records and build HTTP. For dynamic responses, the simpler
benchmark-confirmed `writev()` remains the current choice.

An earlier master benchmark produced about **-9,3% CPU/request** and **+4,8% req/s**.
Memory measurement showed no cost: median RSS 7812 -> 7792 KB, PSS 3921 -> 3911 KB.

The final PHP 8.5 regression passed for `fastcgi`, `fastcgi-ng`, and `http`: small
response, 262 144 B response, binary 65 792 B POST with SHA-256, keep-alive/close,
and a disconnected client. The earlier byte-for-byte comparison covered responses
of 1, 8000, 8184, 8192, 65527, 65528, 65529, 131056, 262144, and 1048576 B.

### Rejected variants

- 8/16/32 KB input buffers produced 86,591 / 87,321 / 86,564 us per request.
  Differences below 1% do not justify a change; keep 16 KB.
- Small-response batching does not save a FastCGI write: the response already
  reaches one `write()`, while the second observed write belongs to another descriptor.
- Caching two `getpid()` calls per request was rejected because the calls belong
  to Zend timeouts and `fork()` detection.

## 3x. Executor `fiber`: asynchronous DNS through evdns (2026-09-06)

Gap in 3u: `getaddrinfo()` lives in `php_network_connect_socket_to_host`,
that is, INSIDE the delegated connect, and blocked the entire process with all
requests in flight. Every fresh MySQL/Redis connection by hostname stopped the
concurrency that is the point of this executor.

### What was done

- For `tcp`, `fpm_fiber_xop_connect` (`fpm_pool_fiber_xport.c`) first resolves
  the name through **libevent evdns** (`evdns_getaddrinfo` on the scheduler's
  `event_base`; libevent is already linked), suspends the fiber until the
  callback, and gives the original connect a literal IP. The evdns base is
  created once per process, lazily on the first named connect, from
  `/etc/resolv.conf` **and `/etc/hosts`
  (`EVDNS_BASE_INITIALIZE_NAMESERVERS` = `DNS_OPTIONS_ALL`; `evdns_getaddrinfo`
  checks hosts before the network). 0x20 randomization is disabled, as in
  getaddrinfo.
- A/AAAA addresses are tried in order with the remaining shared timeout, as in
  upstream. This **restored the fallback** previously lost by the asynchronous
  Fiber connect: `connect()` returning EINPROGRESS is success to the upstream
  loop, so it never reached the next address. Before the change,
  `fsockopen('localhost', ...)` to a server on 127.0.0.1 failed with
  "Connection refused" on `::1` (CLI PHP connected through the fallback).
- New primitives in `fpm_pool_fiber.{h,c}`:
  `fpm_pool_fiber_waiter()` / `wait_wake()` / `wake()` (wait for a callback
  instead of an fd), plus `fpm_pool_fiber_event_base()`. No new files and no
  `config.m4` change.
- Also fixed a scheduler bug: `event_add` calculated the deadline from time
  cached at the beginning of the loop turn, so 60 ms of blocking work in a
  fiber (getaddrinfo, `usleep`) consumed the 50 ms connect timeout — "Operation
  timed out" immediately. Call `event_base_update_cache_time()` before arming
  the timer.

### Measurements (macOS, one Fiber worker, `pool.type = http` + `pool.executor = fiber`)

Unique `*.localtest.me` names (each requires a real network query, ~60–100 ms),
two alternating BEFORE/AFTER runs:

| test | BEFORE | AFTER |
| --- | --- | --- |
| 4 x `fsockopen()` by name, target responds immediately | 367 / 343 ms | **163 / 115 ms** |
| 8 x same | 540 / 534 ms | **166 / 148 ms** |
| 4 x same, target responds after 500 ms | 874 / 843 ms | **661 / 669 ms** |
| 4 x mysqli by name, `SELECT SLEEP(0.5)` | 809 ms (connect 44/128/159/238 ms) | **628 ms** (connect 73/73/74/75 ms) |

Before, `connect_ms` grew stepwise — DNS queued requests. With an overloaded
resolver, BEFORE took 24s for 4 requests (all waited behind one blocking
getaddrinfo).

Regressions, all AFTER with real output:

- `localhost` (hosts: `::1` and `127.0.0.1`, server only on 127.0.0.1) and
  `/etc/hosts` entries (`api.subscription.local`, `sub-api.localhost`) connect,
  `connect_ms` 0; the debug log shows the evdns base being created but no
  "Resolve requested" — hosts were handled locally;
- literal `127.0.0.1`, `127.1`, `[::1]`: zero evdns log lines (at
  `log_level = debug`, evdns logs every query through `evdns_set_log_fn`);
- nonexistent name: `false`, `$errstr` = "php_network_getaddresses:
  getaddrinfo for X failed: nodename nor servname provided, or not known", the
  same E_WARNING as upstream, 36 ms; NODATA gives "non-recoverable failure in
  name resolution" (different wording from getaddrinfo, same prefix);
- 10 ms timeout with a name requiring the network: "getaddrinfo for X failed:
  timed out", errno 60 after 11 ms — no hang;
- nested user `Fiber` (`can_wait() == 0`): the old blocking path works;
- TLS: `tcp://` + `stream_socket_enable_crypto` to www.google.com — OK, cert CN
  `www.google.com`; `wrong.host.badssl.com` — REJECTED ("did not match expected
  CN"); `expired.badssl.com` — REJECTED ("certificate verify failed");
  `file_get_contents('https://...')` OK / rejected as above. `peer_name`/SNI
  use the name from the resource name in the factory (`xp_ssl.c`
  `php_openssl_get_url_name`), not the name passed to connect.

### Limitations

- `ssl://` and `tls://` transports (therefore `https://`) are not hooked at all
  — DNS and the handshake still block as before. Deliberately not extended:
  the handshake itself would also block (OpenSSL polls on its own).
- `STREAM_CLIENT_ASYNC_CONNECT` (the user `CONNECT_ASYNC` option) uses the old
  path with blocking getaddrinfo.
- evdns does not know macOS `/etc/resolver/*`, mDNS (`.local`), split DNS from a
  VPN, or `/etc/gai.conf`; it reads `/etc/hosts` once at child startup. Address
  order is evdns order, without RFC 6724 sorting.
- Without `/etc/resolv.conf` (a scratch container), `evdns_base_new` returns NULL
  — one log warning and a return to the blocking path.
- Redis by name was not tested (not available); mysqli was.

## 3w. Application metrics from PHP (`fpm_metric_*`) — IMPLEMENTED (2026-09-06)

Plan item 4, the part described in 3k. The `ext/fpmng_metrics/` extension
(copied by `prepare.sh` next to `sapi/fpmng/`) is enabled by default and forced
under `--enable-fpmng` (the SAPI code calls its symbols — the force is in the
extension's `config.m4` after `PHP_ARG_ENABLE`, because `--disable-all`
overrides the preset through `PHP_ENABLE_ALL`; see `PHP_REAL_ARG_ENABLE` in
`build/php.m4`).

API as in 3k: `fpm_metric_register/inc/set/observe` (bool = problem detection)
plus `fpm_metric_render()` — returns ready Prometheus text so the same code works
from CLI without an operator endpoint.

What happened exactly as planned in 3k:
- per-worker slots, zero atomics: each worker has its OWN series table in shared
  memory, with aggregation at read time. Slot = sum of `pm.max_children` for
  pools earlier in the configuration + scoreboard index (not pid —
  `pm.max_requests` recycling does not zero counters). The master allocates the
  region after `fpm_conf_init_main` (one `fpm_shm_alloc`, children inherit the map);
- allocation failure does NOT kill FPM — metrics are an add-on; functions return
  false (a deliberate departure from the error chain in `fpm_init`);
- cardinality: `fpmng_metrics.series_limit` (INI, 256 by default) series per
  worker; after exhaustion, false + E_WARNING ONCE per metric, WITH THE NAME.
  Verified: limit 3, the fourth series rejected, existing series remain writable,
  warning once per name;
- bucketed histograms (default 5 ms–60 s, as in 3k), without exemplars;
  `observe` is the only route to consumer metrics because only the application
  knows job boundaries;
- SAPI/extension split resolved as in 3k: shared-memory backend under fpm-ng,
  process-local storage + `render()` under CLI (and every other SAPI).

Additional implementation decisions (supplementing 3k):
1. **Automatic `pool="..."` label** on every series under fpm-ng. Without it,
   an application's `jobs_total` and a consumer's would merge into one series
   with no trace. A user-supplied `pool` label is REJECTED (false + warning).
   CLI does not add the label.
2. **`register` in shared-memory mode is METADATA, not a series.** A bare key
   (without labels) cannot collide with an inc/set/observe key (each gets
   `pool=...`), so register carries only HELP/TYPE/buckets and is NOT emitted as
   a series — otherwise every register would produce a false `name 0` series
   (exactly the trap caught on the test host). In CLI, a bare key IS a series
   (inc without labels produces the same key).
3. **Buckets are part of the identity of the label set, not of a series.** Two
   workers with different bucket sets (for example, a consumer without register
   -> defaults, a web worker with register -> custom) emit different label sets
   and do not clash; aggregation is over identical boundaries. Limitation:
   bucket definitions work per worker slot — register must be called in the
   process that observes (typically at the start of every request/job, as in PHP
   Prometheus clients).
4. **Gauge: two aggregations** — `gauge` (sum over slots) and `gauge_max` (maximum),
   selected in register; the auto-type from `set` is `gauge`.
5. Warnings use `php_error(E_WARNING)` (they reach the PHP/FPM log), not zlog —
   the extension does not depend on SAPI symbols and also builds for CLI. Once
   per topic, a per-process topic list, hard limit 64.

Test host (debug build, macOS/arm64, http 4 workers + supervisor + cron +
status): 41 requests through 12 concurrent clients — three independent counters
agree (`app_requests_total{pool="www"} 41` = `app_jobs_total 41` = the sum of
`app_worker_hits_total` over 41 labels = built-in `fpmng_pool_requests_total 41`),
histogram count 41, buckets sum to 41, unsynchronized concurrent writes do not
corrupt the sum. CLI: render works without fpm-ng, limit and rejection work.

Known limitations (deliberate, later):
- metric type comes from the first slot when workers conflict on type (different
  code in different workers) — values are still summed, but TYPE in HELP may be
  misleading; left until real users appear;
- `/status` (JSON) does not show application metrics — only `/metrics`; deliberate,
  JSON has a per-pool shape.

Merge order: branch `metrics-php`, files: `ext/fpmng_metrics/` (whole directory),
`sapi/fpmng/fpm/fpm_metrics.[ch]`, one line in `fpm.c` (two calls),
`fpm_pool_status.c` (append to `/metrics`), `config.m4` (include path), and
`build/prepare.sh` (copy the extension).

## 3x. Graceful reload — IMPLEMENTED AND VERIFIED (2026-09-06)

Plan item 7. Ordinary FPM reload through `SIGUSR2` still performs `execvp()` of
**the same master** and preserves sockets through `FPM_SOCKETS*` in the process
environment. Only the signal distribution in the first reload phase changed:

- request worker (`fastcgi`/`http`, including Fiber/Async) gets `SIGQUIT`: it
  stops accepting new connections and finishes the current request;
- `supervisor`, `cron`, and `status` pools get `SIGTERM`: supervisor uses its
  handler, sets the flag, arms the watchdog, and exits after the current
  iteration; cron wakes from sleep and exits without starting another run;
  status has no PHP work to finish;
- after collecting all children, the master calls `execvp()`; the new generation
  inherits the same sockets and starts under the same PID.

Implementation is in the overridden `sapi/fpmng/fpm/fpm_process_ctl.c`. The
condition is deliberately limited to `fpm_state == RELOADING` and `SIGQUIT`:
`SIGQUIT` during ordinary graceful stop and log rotation keeps its old meaning
and does not kill consumers through the new path.

Test host (macOS/arm64, debug, `http` + `supervisor` + `cron`):
- a request with `sleep(3)` received reload during execution and returned
  `request-done`, with `start` and `end` from the same worker in the log;
- the master log showed `web -> SIGQUIT`, `consumer -> SIGTERM`,
  `cron -> SIGTERM`;
- supervisor finished the current iteration (`iter-end`), then started a new
  process with a new PID;
- after reload, the log showed `using inherited socket` and the new master
  answered on the same port.

This is not a real hot-reload that diffs configuration and touches only the
changed pool — that scope remains postponed.

## 3y. ACME state on a writable volume — layout, ownership, permissions (task 044, 2026-09-07)

Task 043 decided the ACME client is project-owned PHP run by a dedicated
`pool.type = cron` process (3l above). This settles the other half named
there: certificates and the account key are **state, not code**, and the
project's premise (immutable image, mutable volume) requires the split to be
explicit and shared with the self-runner (3a).

**What is state:** the ACME account private key, the account URL returned by
registration, the certificate's own private key, the certificate chain
(`fullchain.pem`), and per-certificate renewal metadata. Replay nonces and
order state are not persisted — RFC 8555 nonces are single-use and fetched
fresh per request; an in-flight order that is lost (process killed
mid-renewal) is simply retried as a new order on the next scheduled run (see
045), not resumed.

**One base directory, several per-domain subdirectories**, not several base
directories — a pool serving more than one certificate over SNI (041) needs
one subdirectory per name, not a second configuration surface:

```
$ACME_STATE_DIR/
  account.key            account private key (EC P-256), 0600
  account.json           {"url": "..."} from registration, 0600
  <domain>/
    privkey.pem           certificate private key, 0600
    fullchain.pem          leaf + intermediate, 0644 (public)
    renewal.json           renewal metadata, 0600
```

**Configured by `env[ACME_STATE_DIR]`** on the ACME cron pool — the existing
FPM pool directive for setting an environment variable, not a new C
directive. This needed no change to `fpm_pool_cron.c` or `fpm_pool_type.h`:
the state directory is purely the concern of the PHP script `cron.script`
points at, which is why this task's implementation is entirely
`sapi/fpmng/acme/state.php`, a small library the script uses, plus a test.

**Ownership:** the ACME cron pool and every `http` pool serving a certificate
it manages must share the same pool `user`/`group`. The private key is 0600 —
task 044 acceptance criterion 2 — not group-readable, so a gateway running
under a different uid after task 010's privilege drop could not read it. This
is a documented operational requirement, not something the code enforces
(there is no cross-pool identity check anywhere else in this SAPI either).

**Startup checks, not renewal-time surprises:** the library's
`assertUsable()` fails loudly and specifically, before any key is touched, in
two cases the task calls out explicitly: the directory does not exist, and
the directory exists but is not writable (probed with a throwaway file, not
inferred from `is_writable()`, which does not account for a read-only bind
mount reporting normal permission bits). Both produce one message naming the
path; neither crashes and neither falls back to serving plain HTTP — the
bootstrap state machine (3l) already keeps 443 closed and 80 challenge-only
until a certificate exists, so a state-directory failure simply means no
certificate is ever produced, which is the existing `NO_CERT` state, not a
new failure mode.

**Restart reuses existing material:** the account key, the account record and
a certificate's key are created once, on first use, and loaded thereafter —
`loadOrRegisterAccount()` takes the actual registration call as a callback and
never invokes it a second time once `account.json` exists. Verified in
`sapi/fpmng/tests/fpmng-acme-state.phpt` by simulating two process "boots" against
the same directory and asserting the registration callback runs exactly once
and both the account key bytes and the account URL are identical across the
two.

**Sole writer:** the HTTP gateway has no code path that writes under this
directory today — it only reads `http.tls_cert`/`http.tls_key` — so criterion
7 holds by the absence of a write path, not by an added check.

**Atomic writes:** every file this library writes goes through a temp file in
the same directory, `chmod`ed to its final mode, then `rename()`d — a
concurrent reader (a gateway reloading `http.tls_cert`/`http.tls_key`, task
040) never observes a partial file, and a private file is never briefly
visible under its final name with the wrong permissions.

**Left to later tasks:** the actual RFC 8555 protocol calls (045's handover,
047's issuance and renewal) are not part of this task; `state.php` only
defines and manages the on-disk layout. `installCertificateChain()` exists as
the write path 047 will call after a successful order; it is exercised in the
test with a placeholder PEM string, not a real certificate.

## 3z. A gateway process's own log lines are decorated like the master's (issue #130, 2026-09-10)

`zlog_buf_prefix()` (`zlog.c`) writes the timestamp only when
`fpm_globals.is_child` is clear, because an upstream FPM child logs through a
pipe the master decorates. An fpmng HTTP gateway process is forked outside that
path (`fpm_http_gateway_run()`), keeps the inherited `error_log` fd and writes
into it directly — so its lines landed in the shared file with no time of their
own, next to the master's:

```
WARNING: [pool web] http: upstream '127.0.0.1:9008' closed 2.0 ms after the complete request was written to it, ...
[09-Sep-2026 21:00:49] WARNING: [pool web] child 2763991 exited on signal 9 (SIGKILL) after 0.042689 seconds from start
```

The pair of lines issue #118 asks an operator to compare was therefore
correlatable only by its order inside one file, which several gateway processes
and the master writing concurrently do not guarantee.

**The fix is a prefix decision, not a second formatter.** `fpmng_zlog_ex()`
(`sapi/fpmng/fpm/fpm_child_error_log.c`) clears `fpm_globals.is_child` for the
duration of the `vzlog()` call and restores it immediately, so upstream
produces exactly the line the master would have. Reformatting the message in
our own code instead would have meant reimplementing `vzlog()`'s level filter,
its truncation and its `": %s (%d)"` errno suffix — the last of which the child
log relay does lose, as `fpm_child_log.h` warns, because `zlog()` calls the
external logger before appending it and masks `ZLOG_HAVE_ERRNO` out of the
flags it passes. Here `ZLOG_SYSERROR` keeps its reason.

Coverage comes from the `zlog()` macro: `sapi/fpmng/fpm/zlog.h` is ours and
reroutes it, so an upstream file that logs inside a gateway process
(`fpm_unix.c` during the privilege drop, say) is decorated too, with no
per-file include to remember. That header does **not** copy upstream —
`build/prepare.sh` keeps this php-src's own `zlog.h` next to it as
`zlog_upstream.h` and ours includes it. Copying would have frozen the
declarations: `struct zlog_stream` gained two bit-fields in 8.5, so a header
taken from one branch and compiled against another describes a different
object without saying so. `zlog_msg()` and the `zlog_stream` API are left
alone; they are the master's paths for captured child output and the access
log.

Opting in is explicit — `fpm_child_error_log_use()`, called by
`fpm_http_gateway_run()` — and is a no-op unless the process actually holds an
`error_log` file (`fpm_globals.error_log_fd > 0`). An ordinary pool child, whose
log was closed by `fpm_stdio_init_child()`, writes to `STDERR_FILENO`, which the
master decorates itself when it captures it; decorating there would produce two
timestamps in one line. Under `error_log = syslog` there is nothing to restore,
because `zlog_buf_prefix()` never consults `is_child` on that path.

Measured on 192.168.8.50 (php-8.5.11, 2026-09-10), one gateway pool whose worker
`kill -9`s itself:

```
[10-Sep-2026 08:27:38] WARNING: [pool web] http: upstream '127.0.0.1:9130' closed 2.0 ms after the complete request was written to it, without one byte of a reply (EOF): the worker died, or it refused the request head and closed without answering -- if the master reports no child exit for this pool, the request head is the remaining suspect
[10-Sep-2026 08:27:38] WARNING: [pool web] child 2782431 exited on signal 9 (SIGKILL) after 1.014005 seconds from start
```

`sapi/fpmng/tests/fpmng-http-gateway-log-decoration.phpt` asserts both shapes.
Negative control: with the one call in `fpm_http_gateway_run()` commented out
and nothing else changed, that test fails on the gateway line and the other 45
still pass.

## 3aa. A gateway process follows `error_log` across a SIGUSR1 reopen (issue #134, 2026-09-10)

`fpm_stdio_open_error_log(1)` re-points the log **in place** with
`dup2(fd, fpm_globals.error_log_fd)` (`fpm_stdio.c`), and only the master runs
it — SIGUSR1 is handled in the master's event loop. `dup2()` touches one
process's descriptor table, so a process forked earlier keeps its own copy
pointing at the renamed file. Upstream never hit this because an ordinary child
has no `error_log` fd at all: `fpm_stdio_init_child()` closes it, with the
comment "child cannot use master error_log because not aware when being
reopen". Section 3z made an HTTP gateway process keep that descriptor **on
purpose** so its lines carry the master's decoration — which is exactly what
turns rotation into missing lines: after a logrotate the master writes into the
new file while every gateway keeps appending to the old one.

**Why a descriptor is handed over instead of a "reopen now" signal.** The
obvious fix — SIGUSR1 to every gateway plus `fpm_stdio_open_error_log(1)` there
— cannot work in the deployment that matters. A gateway drops privileges to the
pool user before it serves anything (`fpm_http_gateway_drop_privileges()`, task
010) and a distribution `error_log` is root-owned (Debian:
`/var/log/php8.3-fpm.log`, `root:adm 0640`). Measured on 192.168.8.50 with a
file of exactly those permissions:

```
$ sudo -u nobody cat /tmp/fpmng-134-rootlog
cat: /tmp/fpmng-134-rootlog: Permission denied
```

`open()` in the gateway would fail with `EACCES`, and the diagnostic about it
would go into the old file. So the master owns the file and children never open
it — the rule `fpm_tls_reload.c` already states for the TLS private key.

**The mechanism** (`sapi/fpmng/fpm/fpm_error_log_follow.{c,h}`, which carries
the full reasoning): one `AF_UNIX SOCK_DGRAM` socketpair per followed process,
created in the master immediately before `fork()`; after the reopen the master
sends its already re-pointed `fpm_globals.error_log_fd` over every channel as
`SCM_RIGHTS`, and the receiver `dup2()`s it onto its own
`fpm_globals.error_log_fd` — the number `zlog.c`'s static `zlog_fd` holds — from
its libevent loop. A pair per process, because a datagram reaches exactly one
reader. Both ends non-blocking: neither the master's nor the gateway's event
loop may block. The send end is `shutdown(SHUT_RD)` and the receive end
`shutdown(SHUT_WR)`, so a de-privileged gateway cannot push datagrams (or
descriptors) back into a buffer the master never drains.

Lines a follower writes between the master's reopen and its own adoption still
land in the old file. That window is one event-loop turn wide and is not worth
a synchronous handshake: the rotated file is still on disk, so nothing is lost,
it is only in the previous file.

`fpm_stdio_child_use_pipes()` calls `fpm_error_log_follow_child(NULL)` so an
ordinary worker drops every channel it inherited — it follows nothing, its
`error_log` is taken away entirely a moment later.

Measured on 192.168.8.50 (php-8.5.11, 2026-09-10), whole fpmng suite:
**PASS=37, FAIL=0, SKIP=10** of 47. Negative control — the one call to
`fpm_error_log_follow_publish()` in `fpm_stdio.c` commented out, nothing else
changed — is **PASS=36, FAIL=1**, and the failure is the bug itself: the
gateway's line is in the rotated file and absent from the one the operator
reads.

```
--- reopened ---
[10-Sep-2026 09:43:20] NOTICE: error log file re-opened
[10-Sep-2026 09:43:21] WARNING: [pool web] child 2387827 exited on signal 9 (SIGKILL) after 0.343640 seconds from start
--- rotated ---
[10-Sep-2026 09:43:21] WARNING: [pool web] http: upstream '127.0.0.1:9008' closed 1.0 ms after the complete request was written to it, without one byte of a reply (EOF): ...
```

`sapi/fpmng/tests/fpmng-http-gateway-log-reopen.phpt` asserts both directions:
the gateway line is in the reopened file and is not in the rotated one.

`http.access_log` had the same symptom and a different fix — section 3ab.

## 3ab. A gateway reopens `http.access_log` on the same notification (issue #137, 2026-09-10)

The symptom of 3aa, on the other log a gateway process owns: each gateway opens
`http.access_log` itself (`fpm_http_access_log_open()`, from
`fpm_http_gateway_run()`), nothing reopened it, and after a logrotate every
gateway kept appending to the renamed file while the file an operator reads
stayed empty.

**The ownership is the opposite of the error_log's, and that decides the fix.**
A gateway opens the access log *after* `fpm_http_gateway_drop_privileges()`, so
the file belongs to the dropped-to identity and this process can `open()` it
again by path — no descriptor has to be handed over, and 3aa's `SCM_RIGHTS`
machinery would be pure ceremony here. What the gateway cannot do is *notice*
the rotation: SIGUSR1 is handled in the master's event loop, and a gateway sets
`SIGUSR1` to `SIG_DFL` (`fpm_http_gateway_run()`), so signalling the process
group would kill the gateways instead of rotating anything. Installing a
libevent signal handler in the gateway instead was rejected: it leaves a window
between `fork()` and `event_add()` in which the default disposition is still
in force, i.e. a logrotate that lands while a gateway is respawning kills it.

**So the 3aa channel became the wakeup carrier.** A datagram on it now means
"the master reopened its logs", and the descriptor it carries is a payload
rather than the message. `fpm_error_log_follow_child_adopt()` returns how many
notifications it consumed, and `fpm_http_log_follow_readable()` turns a
positive count into `fpm_http_access_log_reopen()`. The two concerns meet in
the gateway, where the wakeup lands; neither `fpm_error_log_follow.c` nor
`fpm_http_access_log.c` learns about the other.

Two consequences of "the datagram is a wakeup", both in
`fpm_error_log_follow.c`:

- the channel is created even when there is no `error_log` descriptor to hand
  over (`error_log = syslog`), and `fpm_stdio_open_error_log(1)` publishes from
  the syslog branch too. Keeping 3aa's `fpm_globals.error_log_fd <= 0` gate
  would have left `http.access_log` unrotated in exactly the configuration
  where it is the only file an operator has;
- such a datagram carries no `SCM_RIGHTS`, and the receiver treats its absence
  as normal rather than warning about it.

`fpm_http_access_log_reopen()` opens the new file before letting go of the old
descriptor, so a failed reopen leaves the process appending to the rotated file
— lines in the previous file, not a log switched off. It also revives a handle
that was NULL because the open at startup had failed: a rotation is the one
moment retrying is worth it.

The module kept its `fpm_error_log_follow` name after this widened it. A rename
to something like `fpm_log_reopen.c` would describe today's contract better, at
the price of detaching every call site, comment and commit that speaks of the
error_log follow channel from its history.

Measured on 192.168.8.50 (php-8.5.11, 2026-09-10), whole fpmng suite:
**PASS=38, FAIL=0, SKIP=10** of 48. Negative control — the one gateway-side
call commented out (`fpm_http_access_log_reopen()` in
`fpm_http_log_follow_readable()`), nothing else changed — is a FAIL of the new
test, and the failure is the bug itself: the reopened `http.access_log` is
empty and both the pre- and the post-rotation line are in the rotated copy.

`sapi/fpmng/tests/fpmng-http-gateway-access-log-reopen.phpt` asserts both
directions: the post-rotation line is in the reopened `http.access_log` and is
not in the rotated copy, with a pre-rotation line first so that a failure
cannot be read as "the access log never worked".

## 3ac. TLS termination is a build flag, off by default (issue #280, 2026-09-13)

**The problem.** TLS had no flag. `HAVE_FPM_HTTP_TLS` was defined if
`configure` found `libevent_openssl` on the build host and not defined if it
did not, and the only trace of the difference was an `AC_MSG_WARN`. Two
machines, the same command line, two different binaries: one that terminates
TLS for the world, one that refuses `http.tls_cert` at startup. That is a
default nobody chose, for code that is beta, unaudited and network-facing.

**The decision** (from #279): `--enable-fpmng-tls`, default `no`, following
the `--enable-fpmng-fiber`/`--enable-fpmng-async` convention. The probe stays,
but with the flag given its failure is a `configure` **error** naming the
package, not a warning and a quiet downgrade. The shipped `.deb`/`.apk` are
built without TLS -- a deliberate regression against v0.2.0, recorded in
`docs/install.md`.

**Why the sources had to be renamed.** `build/prepare.sh` builds the source
list with `find`, and splits off an optional group by NAME PREFIX rather than
by enumerating files -- the comment there gives the reason: with enumeration a
new file silently lands in the base list and ships in the default binary,
which is the exact failure the flags exist to prevent. The TLS files were
`fpm_http_tls.c`, `fpm_http_tls_reload.c` and `fpm_http_direct_tls.c`, and no
"tls somewhere in the name" pattern could work, because
`fpm_http_direct_tls.c` **must stay in every build**: it holds
`fpm_http_direct_tls_enabled()` and the config-pairing validation, which
`fpm_http_direct.c`, `fpm_http_direct_worker.c`, `fpm_http_direct_request.c`
and `fpm_pool_type.c` call with no `#ifdef` of their own.

So the rule became the file name itself: **`fpm_tls_*.c` is code that needs
OpenSSL, everything else is code that does not.** `fpm_http_tls.c/.h` became
`fpm_tls_http.c/.h`, `fpm_http_tls_reload.c/.h` became `fpm_tls_reload.c/.h`
(symbols and macros with them), and the `#ifdef HAVE_FPM_HTTP_TLS` half of
`fpm_http_direct_tls.c` moved to a new `fpm_tls_http_direct.c`. What stayed
behind is the seam: the always-compiled config half, plus `#ifndef`-guarded
no-op stubs that stand in for the file that is not there. `HAVE_FPM_HTTP_TLS`
kept its name -- it is what the `#ifdef`s in `fpm_http.c` key off, and
renaming it would have been churn with no reader on the other end.

**Four places had to agree, and two of them are the point.**
`build/libphp-build.sh` runs no `configure`: it hard-coded
`-DHAVE_FPM_HTTP_TLS=1` and then asserted the symbol. It is also the path the
shipped packages are built from, so a flag that did not reach it would govern
nothing that ships. It now takes `FPMNG_TLS=0|1` (default `0`), compiles the
TLS group only when asked, links OpenSSL only when asked, and asserts the
result **on the binary in both directions**: with TLS, `fpm_tls_http_validate`
is present and the dynamic section mentions OpenSSL; without it, the symbol is
absent and the dynamic section mentions no OpenSSL at all. It also refuses to
build if a TLS source is in both lists, which is the leak the prefix rule
exists to prevent.

**Refusals name the flag.** The three "built without TLS" messages
(`fpm_http.c` twice, `fpm_http_direct_tls.c` once) used to say "libevent_openssl
and/or OpenSSL were not found at build time", which is now the wrong story:
the libraries may well be there and the operator still gets no TLS. They say
`rebuild with ./configure --enable-fpmng-tls` instead. The `.phpt` skip probes
key on the unchanged substring "built with TLS support", so they kept working.

## 3ad. ACME is a build flag too, and it takes the payload with it (issue #281, 2026-09-13)

**The decision** (from #279, after #280): `--enable-fpmng-acme`, default `no`,
requiring `--enable-fpmng-tls`. Asking for ACME without TLS is a `configure`
**error**, not a warning -- it would build cleanly and do nothing, because the
certificate it obtains would have nothing to serve it with. `build/prepare.sh`
gained an `ACME_PATTERN` group (`fpm_acme_*.c`), matched by name prefix like
the others, and `build/libphp-build.sh` gained `FPMNG_ACME=0|1` with the same
dependency check, so the two build paths cannot disagree about what is
buildable.

**The stubs, not a seam file.** Unlike TLS (§3ac), nothing outside the ACME
group is named `fpm_acme_*`, so no file had to be split. Three callers reach
into it with no `#ifdef` of their own -- `fpm.c` allocates the shared challenge
region before the first fork, `fpm_pool_script.c` registers the writer
functions, `fpm_http.c` answers `/.well-known/acme-challenge/` -- and they now
link against `static inline` no-ops in `fpm_acme_challenge.h`. The lookup
returning -1 makes the challenge URL a 404 like any other unrouted path, which
is what a build with no challenge state should say.

**The payload is ACME, so it is gated too.** This is the part that a
symbol-only reading of "contains no ACME code" would have missed: the client
is PHP (`sapi/fpmng/acme/*.php`), appended to the binary as the distribution
payload (issue #171), and the payload has never carried anything else. A
default build with the C half compiled out but the scripts still embedded
would still ship the facility, as data. `build/embed-payload.sh` therefore
takes `FPMNG_ACME` and embeds nothing without it, every caller passes it, and
`build/libphp-build.sh` asserts both directions on the finished binary -- the
symbol *and* whether a `kind=1` entry is there at all.

**Refusing an ACME configuration.** There are no `acme.*` directives to refuse:
the only thing a configuration says about ACME is
`cron.script = fpmng-dist://acme/renew.php`. So the refusal lives in
`fpm_payload_dist_validate()`, which answers that path by name in a build
without the flag -- "this build carries no ACME client: rebuild with
./configure --enable-fpmng-tls --enable-fpmng-acme" -- rather than leaving it
to the generic "no such file in the embedded distribution payload". The
difference between "you misspelled it" and "you need a different build" is the
whole message. `fpmng_skip_if_no_acme()` in `fpmng-skipif.inc` probes exactly
that, the same way every other skip here asks the binary rather than keeping a
list.

**Which tests skip.** The five that need the BINARY to carry ACME: the two
challenge tests, issuance, renew-failure, and `fpmng-payload-distribution`.
The four that drive `sapi/fpmng/acme/*.php` through the CLI (jose, state,
renew-policy, single-renewer) read the scripts out of the source tree and
never ask the binary anything, so they keep running everywhere -- a package
built without ACME is not evidence about them either way.

**The startup NOTICE.** A build made with the flag says once, in the master,
that ACME is BETA. It is not conditional on the configuration using ACME: what
it reports is a property of the binary, which `-v` does not show. The line
started life in `fpm_acme_challenge.c` with wording of its own; issue #295
moved it to `fpm_run()` and through `fpm_tier_announce()`, so that the log and
the tier table in `README.md` cannot end up saying different things about the
same feature. See section 3ah.

## 3ae. Two flag names reserved for features that do not exist (issue #282, 2026-09-13)

**The decision** (the last part of #279): `--enable-fpmng-http2` and
`--enable-fpmng-quic` exist in `configure` and both are **errors**. Neither
protocol is implemented -- #186 and #187 are still deciding whether an nghttp2
session layer is worth what it costs, and #188 is still deciding whether a UDP
listener can fit the fork-N-children model at all.

**Why reserve a name for something that may never be built.** Not to promise
it. The naming is the cheap part of the work and the part that gets argued at
the worst possible moment, when someone is in the middle of writing the code;
settling it now costs two `PHP_ARG_ENABLE` blocks. The help text carries the
dependency on `--enable-fpmng-tls` for the same reason: HTTP/2 is negotiated
over ALPN and QUIC carries TLS 1.3 inside the transport, so there is no
plaintext form of either, and that is better written down now than discovered
by whoever implements it.

**A reserved name is not a decision that the feature will exist.** #188 may
return "no", and the flag being spelled must not be read as evidence that it
will not. QUIC has no `accept()`: connection IDs have to be routed in
userland, and the whole model here is the kernel demultiplexing a listening
socket that each child holds.

**Refusal, not acceptance.** A flag that is accepted and switches nothing on
produces a binary the operator believes speaks HTTP/2. That is strictly worse
than having no flag: it converts a build-time question into a production
surprise. So `configure` stops, names the feature as not implemented, and
points at the issue deciding it.

**The test is an obstacle on purpose.**
`build/test-reserved-configure-flags.sh` asserts both refusals and both help
lines, and it runs in the matrix's build job because that job already has a
tree with `./configure` in it -- five seconds, because the refusal is reached
before any library check. Whoever implements HTTP/2 or QUIC has to delete a
failing assertion, which is a decision someone makes, rather than being able
to leave the refusal behind and wonder why their flag does nothing.

## 3af. `SA_RESTART` is lost on every request startup, and put back after it (issue #259, 2026-09-13)

An http-direct child installs its stop (SIGQUIT) and retire (SIGUSR1) handlers
with `SA_RESTART` on purpose: in both direct executors PHP runs with the event
loop stopped — inside evhttp's request callback in the classic one, inside the
booted script in the worker one — so a signal aimed at a busy child arrives
while the script is blocked in `read()` or `write()` on a database, cache or
HTTP socket. Without the flag that syscall returns `EINTR`, and the request the
retire was meant to protect is the one that fails. That is the failure mode
issue #65 was built to avoid.

**Every `php_request_startup()` takes the flag off again.** It calls
`zend_signal_activate()`, which reinstalls an action for every signal in
`zend_sigs[]` — SIGQUIT and SIGUSR1 among them — with

```c
sa.sa_flags = SA_SIGINFO; /* we'll use a siginfo handler */
```

(`Zend/zend_signal.c:305`). `SA_RESTART` is not carried over. Zend does save the
queried flags in `SIGG(handlers)[signo-1].flags`, but `zend_signal_handler()`
reads them only for `SA_SIGINFO` and `SA_RESETHAND`; the kernel action is what
decides whether an interrupted syscall restarts, and from the first request on
it says "do not restart". This is the same class of damage as the SIGTERM
disposition both executors already snapshot around request startup — the fix is
the same shape: `fpm_http_direct_restore_sa_restart()` re-reads the action and
ORs `SA_RESTART` back in, immediately after `php_request_startup()` returns, for
both signals, in both executors. It never installs a handler of its own, so
Zend's deferring handler stays exactly where Zend put it.

**MEASURED, on a child blocked in `fread()` on a FIFO** (the repro issue #259
said nobody had produced). What the measurement changed is the size of the
claim:

| signals while blocked | unpatched | patched |
| --- | --- | --- |
| one `SIGUSR1` | `fread()` returns `'hello'` | `'hello'` |
| two `SIGUSR1` | `fread()` returns `false` | `'hello'` |

One interruption is absorbed by PHP itself: `main/streams/plain_wrapper.c:459`
retries an `EINTR`'d `read()` exactly once. The second falls through to the
`TODO: Should this be treated as a proper error` branch at `:470` and the read
fails. And a *socket* read absorbs any number of them — `main/streams/
xp_socket.c:145-156` loops on `EINTR` around `poll()` by itself. So the real
exposure is narrower than the issue assumed: it needs repeated signals, a
non-socket stream, or an extension doing its own `read()`. Narrower, not
absent — a deploy script that sends SIGUSR1 twice is not unusual, and neither
is a PDO driver reading its own socket.

`sapi/fpmng/tests/fpmng-http-direct-sa-restart.phpt` is that repro as a test.
Two signals, not one, is the whole shape of it: with one signal the test passes
against an unpatched binary and proves nothing. A FIFO, not a socket, for the
`xp_socket.c` reason above. Verified in both directions on real hardware —
`read='hello'` with the fix, `read=false` against the binary built from the
commit before it.

## 3ag. A direct child narrates its own lifecycle, so it gets the log channel (issue #260, 2026-09-13)

Upstream FPM takes the error_log away from a child — `fpm_stdio_init_child()`
does `close(error_log_fd)` + `zlog_set_fd(-1, 0)`, commented "child cannot use
master error_log because not aware when being reopen" — on the premise that the
master narrates everything worth narrating about a child. `zlog()` in a child
then writes to `STDERR_FILENO`, which under the default
`catch_workers_output = no` is `/dev/null` (`fpm_stdio.c:375-393`).

**That premise is false for a direct pool.** The child owns the accept socket,
so the child is the only process that knows it has stopped accepting. Retiring —
the issue #65 feature a deploy is built on — is narrated by the child or by
nobody. MEASURED on 2026-09-12 while working on #256: the line

    [pool P] child N is retiring: no new connections, finishing the ones it holds, ...

appeared in no log file in any of the eight rows of that issue's before/after
matrix. The child had in fact retired; the behaviour was confirmed through the
socket. Only the record of it was missing. Reaching it required
`catch_workers_output = yes` — a setting whose documented purpose is capturing
*application* output, which is not what a daemon's own lifecycle is.

The fix is the channel that already exists for exactly this shape of problem:
`fpm_pool_type_s.child_logs_via_master` (issue #121, `fpm_child_log.h`), one
AF_UNIX SOCK_DGRAM socketpair per pool, with the master re-emitting each record
through its own `zlog()` at the child's level and `(child N)` appended. Both
http-direct executors now set it. Nothing was written for this beyond the flag
and the bit split below.

**The bit split, which is the part worth reading.** `child_logs_via_master` used
to do two things: carry the child's `zlog()` lines, *and* redirect PHP's own
diagnostics into the same channel while forcing `log_errors = 1`,
`display_errors = 0`, `html_errors = 0` on the child (issue #124,
`fpm_child_php_log.h`). Those two rest on different premises. The channel is for
"the policy runs in the child", true of any type with its own child loop. The
INI half is for "the child has nowhere else to put a PHP error" — true of
`supervisor` and `cron`, which serve no request and so have neither a response
nor a front end's FastCGI stderr, and **false** of http-direct, which has a
response. Taking `display_errors` away from a request-serving pool because it
wanted its lifecycle lines logged would be an unrelated behaviour change hidden
inside a logging fix. So the second half is now its own bit,
`child_php_log_via_master`, set by `supervisor` and `cron` only.

One consequence worth stating: PHP's `sapi_module.log_message` in a direct child
goes through `zlog()` (upstream's `sapi_cgi_log_message()`, fixed at NOTICE), so
with `log_errors = 1` a PHP error now reaches `error_log` from a direct pool
where it previously went to `/dev/null`. That is the hole closing, not a new
route: the INI defaults are untouched, so whether `log_errors` is on is still
the operator's `php.ini`.

Log volume was checked before flipping the flag, since the channel makes every
child `zlog()` visible: the two client-triggerable NOTICEs in
`fpm_http_direct_conn.c` (the first-request deadline and
`http.max_connections_per_client`) are both already "once per child" behind a
`said_*` flag, deliberately, "because a line per dropped client would be a log
amplifier for the very flood the deadline exists to survive". The rest are
per-pool or per-child events.

## 3ah. Tiers as data, announced once per pool at startup (issue #295, 2026-09-13)

Issue #269 decided the tier split and what each tier withholds; #295 is the
implementation, and the whole design question was *where the tier lives*. The
answer is: on the pool type, as data.

**Not a name comparison.** The tempting shape is a function somewhere that says
"if the type is called fiber or async, it is experimental". That reads the
classification off a string, which means the classification and the thing
classified can drift apart — a new type, or a renamed one, would be silently
supported. `struct fpm_pool_type_s` now carries `enum fpm_tier tier;` and every
one of the twelve entries in `fpm_pool_type.c` states it, next to the `.name`
it applies to. An executor variant is a whole separate struct here (issue #200:
a variant replaces the struct rather than inheriting fields), so the fiber and
async variants state their own tier rather than borrowing the classic one's —
which is exactly right, because `pool.type = http` is supported and
`pool.type = http` with `pool.executor = fiber` is not.

**`FPM_TIER_EXPERIMENTAL = 0`.** The zero value is the least-promised tier on
purpose. A type added later whose author forgets the field announces itself as
experimental and gets noticed; the opposite default would promise support for
something nobody classified.

**Three places or it does not count.** #269 asked for the tier as data, as a
line at startup, and as a table in `README.md`. The middle one is
`fpm_tier_announce()` — one function holding the level, the label and the
sentence of what the tier withholds, so the wording cannot fork. The ACME
`BETA and unaudited` NOTICE that `fpm_acme_challenge.c` used to emit on its own
was deleted and re-emitted through that function from `fpm_run()`: two
spellings of "this is beta" is how the table and the log stop agreeing.

**Where the line is emitted.** Once per pool, in `fpm_run()`, in the master,
before the first fork. Not per child (a `pm.max_children = 50` pool would
announce fifty times, and a respawn would announce again), not per request
(the fastest way to teach an operator to filter the whole family — which would
take the two lines that matter with it). The build-flag lines (TLS, ACME) are
emitted once per process with no pool prefix, because what they report is a
property of the binary rather than of a configuration.

**Silence is the third level.** Supported announces nothing at all, and
`fpm_tier_announce()` returns early for it. That is what makes the other two
readable: a startup log with no tier line in it is a configuration made
entirely of things this project will not move under you.

**The classification is evidence, not a declaration.** Each entry says why in a
comment next to it, and the three that are not supported cite something
checkable: fiber cites the open correctness issues (#79, #80, #82, #84, #85),
which fail #269's criterion 3; async cites the fact that no CI cell builds
`--enable-fpmng-async` at all (#87), which fails criterion 1; the http-direct
*worker* executor cites the unrun spikes #180–#183 and #191. The worker is the
one judgement here that had to be made rather than read off #269 — it is beta
and not experimental because it will not disappear: `examples/` ships against
it.

**A test trap worth recording.** `fpmng-tier-announce.phpt` was first written
to read the file named by `error_log = {{FILE:LOG}}` and assert on its
contents. It passed on nothing: the phpt tester runs FPM **non-daemonized**,
and `tester.inc` only switches its log source to that file when it daemonizes
(`if ($daemonize) { $this->switchLogSource('{{FILE:LOG}}'); }`) — otherwise it
reads the master's stdout pipe and the file stays empty. `file_get_contents()`
on it returns `''`, and every `str_contains($log, ...)` against `''` is false,
so an inverted assertion passes and a positive one fails for a reason that
looks like the feature. Both tier tests assert through the tester's own log API
(`expectLogNotice`, `expectLogWarning`, `expectNoLogPattern`) instead.
`checkAllLogs: true` is needed for the positive ones, because the tier lines
are written before `ready to handle connections` and the reader has already
walked past them by the time `expectLogStartNotices()` returns.

**And the trap had already been sprung three times (issue #297, 2026-09-13).**
The sweep the issue asked for found the same shape in
`fpmng-acme-challenge.phpt`, `fpmng-acme-challenge-plain.phpt` and
`fpmng-acme-handover.phpt` — each one reading
`file_get_contents($tester->getPrefixedFile('log'))` and asserting that the key
authorization, or a slice of the private key, was *not* in it. Two ways wrong
at once: `'log'` is not even the extension the tester uses (`err.log` is), and
the file the right name points at is empty anyway. All three assertions were
passing on `''` and would have gone on passing if the secret had leaked. They
now call `expectNoLogPattern('/' . preg_quote($secret, '/') . '/', true)`,
before `close()` rather than after it — `close()` reaps the master and with it
the pipe the log reader reads. Checked by mutation, which is the only way to
check a negative assertion: swapping the needle for `fpm is running`, a string
the log certainly contains, turns `fpmng-acme-challenge.phpt` red. The old
version stayed green under the same mutation.

The rest of the suite is clean. Every other test that reads a log *file*
(`fpmng-supervisor-fast-restart.phpt`, the four `fpmng-http-gateway-*log*`
ones) starts FPM with `start([], false)` — no `-O`, so `error_log` really is
written — and waits for a pattern it expects to appear, which a vacuous read
cannot fake.

## 3ai. Two packages, and the second one is built where it ships (issue #294, 2026-09-13)

Issues #280 and #281 moved TLS termination and ACME behind build flags that
default to `no`, which made the published `.deb` and `.apk` unable to do either
-- a regression against v0.2.0 for anyone terminating TLS in the pool. #279 left
open what the remedy is, and #294 is the answer: a second package,
`php-fpm-ng-tls`, the same commit built `--enable-fpmng-tls
--enable-fpmng-acme`, described in its own package description as beta and
unaudited.

The alternative was "build it from source". It was rejected on who the people
asking are: someone terminating TLS on a small box is the least likely person to
have a toolchain on it, and the answer would have sent them to an unpinned local
build that nothing in this repository tests.

**The package names itself off the binary, not off the environment.**
`build/package-deb.sh` and `build/package-apk.sh` ask the artefact whether
`fpm_tls_http_validate` and `fpm_acme_challenge_init_main` are defined in it --
the same two symbols `build/libphp-build.sh` asserts on after the link -- and
choose the name, the description and the conflict relationship from that.
Reading `FPMNG_TLS` instead would have been shorter and would have produced a
package that calls itself something its contents are not: the variable is read
two scripts earlier, by a different process, and nothing downstream would ever
compare the two. Only the two combinations that ship have a name; TLS without
ACME is refused rather than published under a description that is wrong about
it.

**They conflict, because they are the same path built differently.** Both own
`/usr/sbin/php-fpm-ng` and `/etc/php-fpm-ng`, so each declares `Conflicts` and
`Replaces` against the other on the Debian side and `replaces=` on the Alpine
one, and both `Provides: php-fpm-ng-any` -- a virtual name, because neither can
provide "php-fpm-ng" when that is the real name of one of them. The effect is
that installing one over the other is an ordinary package-manager operation and
co-installing them is refused before any file is touched.

**Built and gated on a tag, not on every pull request.** This is the cost
decision, and it is the one thing #294 asked that the code could have got wrong
quietly. `build/ci-package-gate.sh` is the longest job in CI; a second row in
`.github/workflows/build-matrix.yml` would have put a full second build,
install and suite run on every pull request in order to gate an artefact that
only exists on a tag. So `FPMNG_PACKAGE_TLS=1` is off by default and
`.github/workflows/release.yml` sets it, next to the rows that build the default
package -- the TLS package goes through the identical script, the identical
clean-container install, the identical negative control and an exact count
assertion of its own, and pull-request CI does not grow by a minute.

**The counts are the evidence that the flags reached the artefact.** The TLS
package scores 52 PASS / 33 SKIP on Debian and 50 / 35 on Alpine against the
default package's 48/37 and 46/39, and the difference is exactly the four tests
that skip on a binary with no TLS and no ACME in it. Measured on a real gate run
of each flavour rather than derived from the list of tests, for the same reason
every other number in that file was.

**And the first thing the second package found was a dead `#ifdef`.** Issue
#295 gave the two build flags a BETA announcement each, in `fpm_run()`. The
ACME one was guarded by `HAVE_FPMNG_ACME`, which `config.m4` defines; the TLS
one by `HAVE_FPMNG_TLS`, which nothing defines anywhere -- the macro is called
`HAVE_FPM_HTTP_TLS`. So a TLS build announced ACME and said nothing about TLS,
in every build, and the way it surfaced was mundane: this issue built the
package, started it, and read the log. Nothing else could have caught it. The
#295 tests cover the per-pool tier lines, which are a different call site, and
an `#ifdef` on a macro that does not exist compiles cleanly and silently to
nothing. `fpmng-tier-build-flags.phpt` now starts a TLS/ACME build and asserts
both lines with the flag names in them; it runs in the `fpmng-phpt` cell of the
matrix, which configures with both flags, and in the TLS package gate.
