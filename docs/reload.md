# `reload.selective` — restart only the pools that changed (issue #330)

## The problem this solves

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

## The directive

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

## What "changed" means

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

## Why text, not structs

The natural-sounding design is "parse both configs, compare the resulting
`fpm_worker_pool_config_s` field by field." Two things rule that out here:

1. `fpm_conf.c`'s parser mutates the single live `fpm_worker_all_pools`
   list; it has no second, scratch pool list to parse a candidate config
   into without disturbing the pools that are actively serving traffic.
   Giving it one would be a large, risky change to an existing, heavily
   loaded file for this issue's time budget.
2. `fpm_worker_pool_config_s` is large (100+ fields across the base config
   plus the cron/supervisor/http/http-direct/tls/acme/fiber extensions) and
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

## How an unchanged pool survives

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

## Relationship to issue #329's rolling restart

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

## Accepted degradations of an adopted child

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
