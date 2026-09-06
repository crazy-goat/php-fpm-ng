# 016 — Symlink deploys are not detected by `fiber.revalidate_freq`

**Track:** nice-to-have. This task only applies to `pool.executor = fiber`,
which is moving behind a build flag that is **off by default**, so a stock
binary does not contain this code at all. The `Priority:` line below is the
priority *within* the fiber track — it is not a claim against the HTTP,
cron, scheduler or proxy work, which is where the project is focused.

**Priority:** medium. Affects the most common PHP deployment layout.
**Status:** open. Recorded as a known limitation in `docs/fiber_errors.md`
(section on the revalidation timer).

## Context

`sapi/fpmng/fpm/fpm_pool_coop_reval.c` registers compiled files through a
`zend_compile_file` hook (chained behind opcache), remembers each file's mtime,
size, device and inode, and a libevent timer periodically `stat()`s them. When
something changed, the worker drains — stops accepting, closes idle keep-alives,
lets in-flight requests finish, then exits so the master replaces it.

This is what makes deploying new code work without `SIGUSR2` under shared
includes, where files are otherwise read once per process.

**It does not see the standard symlink deploy**: `current -> release-N`, atomic
switch of the symlink. `EG(included_files)` holds realpaths, so the recorded
files belong to the *old* release directory. Those files do not change — the
symlink flip does not touch their mtime, inode or size — so the timer sees
nothing and the worker keeps serving the previous release indefinitely. There,
`SIGUSR2` is still required.

Since capistrano-style symlink releases are the dominant PHP deployment layout,
"deploys are picked up automatically" is currently true only for the less common
in-place layout.

## Problem

Detect a symlink deploy, or state clearly that it is not detected and that
`SIGUSR2` is required for it.

## Acceptance criteria

1. A decision, with reasoning, between:
   - detecting the change (for example by also watching the unresolved path, or
     the document root's own link target, rather than only realpaths), or
   - documenting the limitation prominently — in `README.md` where
     `fiber.revalidate_freq` is described, not only in `docs/fiber_errors.md`
2. If detection is implemented: flipping `current` to a new release directory
   causes the worker to drain and be replaced within roughly the configured
   interval, demonstrated on a realistic two-release layout.
3. No additional per-request cost. This runs on a timer, not on the request
   path, and must stay that way.
4. No false positives: an unchanged deployment must not cause workers to cycle.
   A worker restart loop is worse than a missed deploy.
5. The extra `stat()` load is bounded and stated — a large application has
   thousands of included files, and whatever is added is multiplied by that.

## Notes

- Watching the document root or the entry script's *unresolved* path is the
  cheap idea; check whether it actually catches the case, since the pool's
  `chdir` may already be the resolved path by the time we see it.
