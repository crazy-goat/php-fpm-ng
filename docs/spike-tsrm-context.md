# SPIKE: can the `fiber` executor work EXCLUSIVELY in ZTS, with a separate TSRM block per request?

Status: SPIKE, not to be merged. Experiment code: `spike/ext-tsrm-spike/`
(extension `tsrm_spike`, `--enable-tsrm-spike`, static, called from CLI:
`tsrm_spike_run()`, `tsrm_spike_mem(n)`, `tsrm_spike_require_in_ctx(path, which)`).
Does not implement an executor on TSRM contexts — it only checks whether the
foundation works at all.

Repo: php-fpm-ng @ `89c5151`, branch `spike/a5-tsrm-ctx`, worktree
`/Users/piotr.halas/work/php-fpm-ng-spike-a5`. Test box: `~/rd/a5` on
192.168.8.103. Build: PHP 8.5.11-dev (the same php-src as `~/rd/phpsrc`, a copy
in `~/rd/a5/phpsrc`), `./configure --disable-all --enable-fpmng --enable-zts
--enable-tsrm-spike --enable-session=shared --enable-mbstring --enable-ctype
--enable-tokenizer --enable-phar --with-openssl --with-zlib
--prefix=/home/piotr/rd/a5/inst`.

Verification that I am measuring MY OWN binary: `strings sapi/cli/php | grep -c 'Q4a:
before creating'` -> `1` (our string is in the binary), `php -v` -> `(ZTS)`,
`php -m | grep tsrm_spike` -> `tsrm_spike` (module loaded).

## Preliminary findings (research, not measurement — but backed by configure.ac and readelf)

- `tsrm_new_interpreter_context()` / `tsrm_set_interpreter_context()` really do
  not exist in the tree (`bd73607b9e4`, "TSRM cleanup for PHP8"). Zero
  occurrences in `TSRM/`, `Zend/`, `main/`.
- `TSRMG` (used among others by `PS()` in session, when the static cache is
  not set) and `TSRMG_STATIC` (used by `EG`/`PG`/`SG` and by `PS()` WHEN it IS
  set) are two different paths (`TSRM/TSRM.h:178-192`):
  - `TSRMG_STATIC` reads `TSRMLS_CACHE` (symbol `_tsrm_ls_cache`, `__thread`,
    extern) from the CURRENT compilation unit — this is an unofficial, but
    still working, "setter" (you can assign to it).
  - `TSRMG` (without `_STATIC`) calls `tsrm_get_ls_cache()` — a TSRM function
    that reads the real, internal `pthread_getspecific` (see `TSRM/TSRM.c:847`
    and `tsrm_tls_get()`), NOT the `_tsrm_ls_cache` variable.
  - Which path a given module gets is decided by
    `ZEND_ENABLE_STATIC_TSRMLS_CACHE` (`Zend/zend.h:56-69`), a per-file flag set
    in `config.m4`. `ext/session/config.m4:20` sets it ALSO for a `shared`
    build (so session USES the fast/static path, not `tsrm_get_ls_cache()`).
  - For `COMPILE_DL_SESSION`, `ext/session/session.c:3319-3323` does
    `ZEND_TSRMLS_CACHE_DEFINE()` (a NEW definition of the symbol `_tsrm_ls_cache`
    IN THIS FILE, i.e. in `session.so`) + `ZEND_GET_MODULE`, and
    `session.c:2884-2885` does `ZEND_TSRMLS_CACHE_UPDATE()` in MINIT — i.e.
    ONCE, at process start / dlopen of that `.so`. This is exactly the
    hypothesis from the task.
  - **Verified at the binary level** (not just by reading the code):
    `readelf -sW` on `sapi/cli/php` and on `ext/session/.libs/session.so`
    shows `_tsrm_ls_cache` as `TLS LOCAL` in BOTH files — i.e. these are
    PHYSICALLY two separate thread-local variables, with no possibility of
    ELF symbol interposition. Reason: compilation is done with
    `-fvisibility=hidden` (visible in the `make` compile line), and
    `_tsrm_ls_cache` has no visibility attribute in its definition
    (`TSRM_TLS void *_tsrm_ls_cache = NULL;`), so it inherits `hidden` from the
    per-file compilation flag. `session.so` physically CANNOT see a change to
    this variable in the main binary, and vice versa.

## Q1: is it possible to create a second TSRM block without the removed API?

**YES.** `ts_resource_ex(0, &fake_tid)` with a substituted (fake) `th_id`
creates a new block — `TSRM/TSRM.c:527-530` (`allocate_new_resource`), when
`thread_id` is not yet in the hashtable. `id == 0` passed to
`ts_resource_ex` returns `&thread_resources->storage` (see
`TSRM_SAFE_RETURN_RSRC`, `offset==0` -> `return &array`), i.e. exactly what
`tsrm_get_ls_cache()` normally returns for a REAL thread.
`allocate_new_resource` calls the ctor of EVERY registered module
(`TSRM/TSRM.c:470-480`) — i.e. the new block gets a full, fresh set of
globals (like a new real thread in a threaded SAPI).

Raw output (`tsrm_spike_run()`):
```
Q1: real thread=130144153527872, tsrm_get_ls_cache() (ctxA)=0x617b00d055d0, TSRMLS_CACHE=0x617b00d055d0
Q1: ts_resource_ex(0, fake_tid=130144153551077) -> ctxB=0x617b00f17b30 (created=yes, different from A=yes)
Q1: after creating B, tsrm_get_ls_cache()=0x617b00f17b30, TSRMLS_CACHE=0x617b00f17b30 (expected: both == ctxB - side effect of allocate_new_resource switching the REAL thread)
```

**Caveat discovered ALONG THE WAY (more important than the YES answer itself):**
`allocate_new_resource()` does NOT just create a new block alongside the
existing one — as a side effect it immediately SWITCHES the real thread
(the real `pthread_setspecific` USED by `tsrm_get_ls_cache()`, plus
`TSRMLS_CACHE`) to the newly created block. There is no public function that
lets you go back to the previous block for the `tsrm_get_ls_cache()` path —
`ts_resource_ex()` for an ALREADY EXISTING block (the "found in
hashtable" branch) does NOT call `set_thread_local_storage_resource_to()`, so
it does not switch the real TLS back. The only way back I found is manually
assigning `TSRMLS_CACHE = address_of_old_block` — and that only works
for the fast/static path (see Q2), NOT for `tsrm_get_ls_cache()`.

## Q2: does manually setting `TSRMLS_CACHE = ...` switch EG/PG (statically compiled code)?

**YES**, for code compiled statically (main binary, without `COMPILE_DL_*`),
because that is one, shared variable (`TSRMLS_MAIN_CACHE_EXTERN()` in
`zend.h:73-76` for files without `ZEND_COMPILE_DL_EXT`).

Raw output:
```
Q2: I set EG(precision)=111 in A, =222 in B. In B it reads=222 (expected 222)
Q2: after returning TSRMLS_CACHE=ctxA, EG(precision)=111 (expected 111, NOT 222)
Q2: CONCLUSION: switching EG() (statically compiled code) via a manual TSRMLS_CACHE works = YES
```
(The order of the lines in the terminal was shuffled relative to the order in
which the code executed — see the "Side effect on the output layer" section
below; the values are correct, only the output buffer FLUSH is unpredictable.)

## Q3: does session (`--enable-session=shared`) follow the switch?

**NO — as the hypothesis predicted, and in two ways, one worse than the other.**

1. Build: `session.so` uses `ZEND_ENABLE_STATIC_TSRMLS_CACHE=1`
   (`ext/session/config.m4:20`), so `PS(v)` goes through the fast/static
   path — but its OWN copy of `_tsrm_ls_cache`, frozen once, in this `.so`'s
   MINIT. Manually switching `TSRMLS_CACHE` in our file does NOT touch that
   copy (confirmed by `readelf`, see above: two physically separate
   `TLS LOCAL` variables).
2. Runtime, A<->A path (test works): we set `session.save_path` in A,
   read it in A — correctly, isolation is fine as long as we stay in A:
```
Q3: in A I set save_path=/spike/A, session_save_path() now returns: /spike/A
Q3: after returning, TSRMLS_CACHE=ctxA (the static EG/PG already sees A), session_save_path() (dynamically loaded session.so) returns: /spike/A
```
3. Runtime, attempt to read/write IN context B: **`call_user_function()` for
   `session_save_path` FAILED** (twice — set and get):
```
SPIKE: call_user_function(session_save_path, /spike/B) FAILED
SPIKE: call_user_function(session_save_path) FAILED
```
   I did not manage to establish the exact cause within this spike (did not
   dig deeper, per the "don't fix along the way" rule) — the most likely
   explanation: `CG(function_table)` in context B is fresh from GINIT (because
   `allocate_new_resource` calls the ctor of every module, but that is NOT the
   same as the full function registration done by `php_module_startup()` for
   a given thread), so `session_save_path` may simply not exist there.
   This is MORE IMPORTANT than Q3 itself: **context B, even correctly created
   (Q1), is NOT a working interpreter** — see also Q4b, where an attempt to
   run REAL PHP code in context B ends in a SEGFAULT.

**Conclusion for Q3**: this part of the question (whether session SILENTLY
sees the old context instead of the new one) could NOT be conclusively
measured at runtime, because the probe is blocked by a deeper problem —
context B cannot execute any PHP function call. What IS measured and certain:
session.so has ITS OWN, physically separate copy of `_tsrm_ls_cache` (proven
by `readelf`), so no external "switch" (even if context B worked) can touch
it — session always sees the context it had at the moment of its own MINIT
(usually process start), regardless of anything that happens afterward.

## Side effect discovered along the way: the output layer is also per-context

Lines printed by `php_printf()` WHILE `TSRMLS_CACHE` pointed to B appeared in
the terminal in a DIFFERENT order than in the code (e.g. the line "Q2: after
returning... 111" appeared in the log BEFORE the line "Q1: ts_resource_ex(...) ->
ctxB", even though the code has it the other way around). `php_printf` goes
through SAPI output buffering (`OG()`/`output_globals`), which is also
per-context TSRM (fast/static) — i.e. switching `TSRMLS_CACHE` mid-request
also switches the output buffer to a new, "orphaned" buffer of context B,
which flushes at a different moment than the normal stdout stream of context
A. I did not investigate this further (out of scope for the spike), but it is
another, independent piece of evidence that "just switch TSRMLS_CACHE" moves
much more state than just the globals we deliberately touch.

## Q4: memory cost

### (a) the TSRM resource block itself, measured

Method: `VmRSS` from `/proc/self/status` before and after creating N blocks via
`ts_resource_ex(0, &fake_tid)` in a loop (different `fake_tid` each iteration),
WITHOUT any PHP code inside (no request, no `require`).

Raw output:
```
N=1: Q4a: before creating 1 additional block: VmRSS=16564 kB
     Q4a: after creating 1 additional block: VmRSS=16856 kB (delta=292 kB, ~292.0 kB/block)
N=8: Q4a: before creating 8 additional blocks: VmRSS=16664 kB
     Q4a: after creating 8 additional blocks: VmRSS=18944 kB (delta=2280 kB, ~285.0 kB/block)
```
~285-292 kB per additional, EMPTY TSRM block (just the module structures after
GINIT, zero user code). This IS a measurable baseline cost, but does not
answer the question of the real-world cost (see (b)).

### (b) with Symfony (`require vendor/autoload.php`) in each context

**NOT MEASURED.** An attempt (`tsrm_spike_require_in_ctx()`, using
`zend_eval_string()` because `require` is a language construct, not a
function) on a simple `tiny.php` file in the CURRENT context works correctly:
```
Q4b: zend_eval_string(require /home/piotr/rd/a5/tiny.php) in the current context -> rv=0 (SUCCESS), exception=no
Q4b: VmRSS before=16632 kB after=16640 kB (delta=8 kB)
PHP script survived to the end.
```
but the same operation IN CONTEXT B (created via `ts_resource_ex`, as in Q1)
**ends in a process SEGFAULT, immediately, before any print**:
```
$ php -n spike_require.php tiny.php 1
Segmentation fault (core dumped)
EXIT=139
```
Not digging further into the cause (out of scope for the spike) — but this
closes the matter of (b): if even a trivial file segfaults in context B,
`require vendor/autoload.php` with Symfony in the same context has no chance
of working, and trying it further would be wasting time on something already
known not to work. The reference point from `docs/frameworks.md` (8 parallel
Symfony requests = 42 MB RSS in one process, the current model) remains the
only measured data point on this side.

## Q5: does a ZTS build pass with `patches/` and `sapi/fpmng`?

**NO.** `./configure --enable-zts` + `build/prepare.sh` (patches + assembling
`sapi/fpmng`) pass without warnings, but `make` fails in the source file that
implemented the request<->request session isolation mechanism on the
fiber/coop executor (now on branch `async`) — "swap globals by value":

```
error: 'sapi_globals' undeclared (first use in this function); did you mean 'fpm_globals'?
        memcpy(&base_sg, &sapi_globals, sizeof(sapi_globals));
error: 'output_globals' undeclared (first use in this function); did you mean 'output_globals_id'?
        memcpy(&base_og, &output_globals, sizeof(output_globals));
... (the same pair of errors in six other functions of the same file —
     8 occurrences in total)
make: *** Error 1
```

Cause: `sapi_globals` and `output_globals` as bare global variables exist
ONLY in an NTS build. In ZTS these are macros (`SG(...)`, `OG(...)`) going
through TSRM, not symbols you can take the address of / `memcpy`. The "swap
globals by value" mechanism between requests in the same process assumed NTS
outright — consistent with the executor's own validation already REJECTING
ZTS at runtime today. This means: the code was never written for ZTS and
today does not even pass compilation, let alone validation.

The rest of the tree (CLI SAPI, Zend, our `tsrm_spike` extension, `session`
as `shared`) compiles and links cleanly under ZTS (`make sapi/cli/php`
passed with no errors, `php -v` shows `(ZTS)`).

## Summary

| Question | Answer | Confidence |
|---|---|---|
| Q1: can a 2nd TSRM block be created | YES, via `ts_resource_ex(0,&fake_tid)` | high — it works, but has a side effect (auto-switch) and creates a context that is NOT a fully functional interpreter |
| Q2: manual `TSRMLS_CACHE` switches EG/PG | YES | high, measured directly |
| Q3: session follows the switch | NO (its own, physically separate copy of `_tsrm_ls_cache`, confirmed by `readelf`) | build-level: high. Runtime "what exactly does B see": not measured, because B does not execute function calls |
| Q4a: cost of an empty block | ~285-292 kB/block | measured |
| Q4b: cost with Symfony | not measured (segfault already on a trivial file in context B) | — |
| Q5: does a ZTS build pass | NO — `fpm_pool_coop.c` uses bare `sapi_globals`/`output_globals`, which do not exist in ZTS | high, a concrete compilation error |

**Overall conclusion of this spike**: the idea "fiber executor exclusively in
ZTS, separate TSRM context per request" runs into AT LEAST three independent
obstacles, each on its own sufficient to reject the idea in its current form:
1. There is no public, bidirectional way to switch context for ALL access
   paths (fast-static works, `tsrm_get_ls_cache()` has no equivalent setter).
2. Dynamically loaded extensions (like `session`) have their own, physically
   separate copy of the cache, frozen once at process start — no spike-level
   hack changes this without modifying every such extension.
3. A block created by `ts_resource_ex` with a substituted `th_id` is NOT a
   standalone, working interpreter — an attempt to execute REAL PHP code in
   it (even trivial) ends in a segfault. Fixing this would require
   reconstructing a significant part of what `php_module_startup()`/
   `php_request_startup()` normally does per-thread in a threaded SAPI —
   which is exactly what was removed from the public API in PHP8 (Q1's
   preliminary finding) and what this spike was meant to deliberately skip.

On top of this there is (4): a ZTS build of the current `sapi/fpmng` does not
pass anyway (Q5) independently of the above, because `fpm_pool_coop.c` was
never written for ZTS.

## What was NOT done

- The fiber-on-TSRM-contexts executor was not implemented — out of scope.
- `fpm_pool_coop.c` was not fixed for ZTS (Q5) — noted, not fixed.
- The exact cause of the segfault in Q4b / the `call_user_function` failure
  in Q3 was not diagnosed (out of scope — "don't fix along the way").
- Q4(b) with real Symfony — not measured, see above why trying further did
  not make sense.
