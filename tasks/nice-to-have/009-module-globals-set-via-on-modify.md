# 009 — Module globals set through `on_modify` are still shared between requests

**Track:** nice-to-have. This task only applies to `pool.executor = fiber`,
which is moving behind a build flag that is **off by default**, so a stock
binary does not contain this code at all. The `Priority:` line below is the
priority *within* the fiber track — it is not a claim against the HTTP,
cron, scheduler or proxy work, which is where the project is focused.

**Priority:** medium. Known, documented, narrow but real.
**Status:** open.

## Context

Per-request isolation on the fiber executor now covers SG, OG,
`EG(symbol_table)`, superglobals, error handlers, `ext/session` globals, and the
**values** of ini entries (`sapi/fpmng/fpm/fpm_pool_coop_ini.c`).

The ini work isolates what `ini_get()` and `ini_set()` observe. It deliberately
does **not** call `on_modify` when switching fibers, and that leaves a gap: some
ini directives push their value into a *process* global through their
`on_modify` handler, and that global is not switched.

The worked example is `precision`. It lands in `core_globals` and is what
`var_dump()` and `serialize()` actually use. So two concurrent requests observe
their own `ini_get('precision')` correctly, while `var_dump()` in both uses
whichever value was written last.

This is recorded in `docs/frameworks.md` under "Known limitation of fix 4"
and was reported honestly by the implementer rather than discovered later.

## Problem

Decide how far this should go, then close the gap or document the boundary.

The general fix is to swap the affected module's globals at fiber switch, the
way `sapi/fpmng/fpm/fpm_pool_coop_session.c` does for `ext/session`. That file
is also the precedent for reaching a module's globals **without a hard linker
dependency** — it takes the base address from the `mh_arg2` field of an ini
entry belonging to that module, so it works whether the module is compiled
statically or as a shared object.

## Acceptance criteria

1. An enumeration of which ini directives actually matter — that is, whose
   `on_modify` writes request-relevant state into a module global that a script
   can change at runtime. This list, with its method, is the main deliverable;
   it may well be short.
2. For each entry on that list, either isolation or an explicit decision not to
   isolate, with the reason.
3. Demonstrated for at least `precision`: two concurrent requests, one setting
   `precision` and sleeping, the other calling `var_dump()` on a float — each
   observing its own value.
4. Requests that change no ini entries pay nothing extra, as today.
5. Any directive that cannot be isolated is documented as a known limitation in
   the same place as the current note, rather than left implicit.

## Explicitly out of scope

- `define()`. Constants are process-wide and there is no "restore" for them.
  This is already recorded as permanent in `docs/frameworks.md`.
- Extension globals unrelated to ini.

## Notes

- Resist the temptation to call `on_modify` on every fiber switch as a blanket
  solution. `OnUpdateTimeout` arms and disarms a **process** timer; firing it at
  every switch would be both expensive and wrong. The existing code avoids
  `on_modify` on the switch path for exactly this reason.
