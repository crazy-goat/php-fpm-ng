# C style and static analysis

Task 013. How this tree stays comparable to upstream `sapi/fpm/`.

## EditorConfig

`.editorconfig` matches php-src's (tabs for C/headers, LF, final newline,
`tab_width = 4`). Editors that honour EditorConfig need no project-specific
setup beyond that file.

## clang-format

Config: `.clang-format`, run by `bin/lint.sh` (check only; `--fix` rewrites).
CI pins clang-format 23.1.2. The style follows the existing hand-written code
(tabs, `BreakStringLiterals: false`, `SortIncludes: Never`, trailing comments
left unaligned) so a reformat is whitespace only.

Scope is a file list, never a prepared php-src tree: tracked `*.c`/`*.h` under
`sapi/fpmng/` and `ext/fpmng_metrics/`, minus `build/clang-format-exclude.txt`.
That file has two parts:

- the nine modified copies of upstream `sapi/fpm/` files, excluded for good so
  they stay diffable against upstream (`build/prepare.sh` diffs them against
  the originals);
- legacy files that would be reflowed in a large way. Shrink that part, never
  grow it; a new file must pass the check. Reformat a legacy file in a commit of
  its own, so the whitespace-only change is easy to review.

## clang-tidy: chosen subset

Config: `.clang-tidy`. Runner: `build/lint-c.sh` (see
`sapi/fpmng/README.md`).

Only checks listed under `Checks:` are considered. Everything else is off via
the leading `-*,`.

### Enabled, and why

| Check | Why |
|---|---|
| `bugprone-sizeof-expression` | Catches `sizeof(ptr)` / wrong-type sizeof that compile cleanly and corrupt lengths. |
| `bugprone-suspicious-memset-usage` | Wrong memset length/fill is a classic silent bug in C buffers. |
| `bugprone-macro-parentheses` | Macro args without parens change precedence; FPM has many macros. |
| `bugprone-integer-division` | Accidental truncating division in size/time arithmetic. |
| `bugprone-posix-return` | Posix APIs return `-1`/`errno`; treating them as boolean is a real footgun. |
| `bugprone-suspicious-string-compare` | `strcmp` used as boolean without comparing to 0. |
| `bugprone-unused-return-value` | Ignoring `malloc`/`read`/`write` return values. |
| `clang-analyzer-core.NullDereference` | Null deref that the compiler does not always warn about. |
| `clang-analyzer-core.uninitialized.Assign` | Use of uninitialized values. |
| `clang-analyzer-unix.Malloc` | Mismatched or leaked `malloc`/`free`. |
| `clang-analyzer-deadcode.DeadStores` | Assignments whose value is never read. |

### Deliberately not enabled, and why

| Family / check | Why off |
|---|---|
| `google-*`, `llvm-*`, `hicpp-*`, `cert-*` | Style/cert packs that fight php-src conventions. |
| `readability-*` (all) | Naming and bracing differ from Google/LLVM; EditorConfig + matching upstream is enough. |
| `modernize-*` | C++, not C. |
| `performance-*` | Noisy on FPM-style code; gain unclear without a baseline. |
| `misc-redundant-expression` | Flags the deliberate `errno == EAGAIN \|\| errno == EWOULDBLOCK` portability idiom (equal on Linux). Suppressed at the site for `-Wlogical-op` instead — see `fpm_pool_coop.c`. |
| `clang-analyzer-security.*` | Too many false positives on a setuid-adjacent SAPI without a dedicated pass. |
| `concurrency-*` | Assumes different locking models than FPM's process model. |

Known-benign findings are suppressed **at the site** with a comment (and a
diagnostic pragma when the compiler needs one), never by widening this config.

## CI

The `checks` job in `.github/workflows/build-matrix.yml` runs
`build/lint-c.sh` and is **blocking**. It landed non-blocking
(`continue-on-error: true`) on purpose -- a red build on day one for
pre-existing findings teaches people to ignore the job -- but that backlog was
emptied by issues #107 and #111, and `continue-on-error` was dropped on
2026-09-09. A finding here is fixed in the code, or silenced in `.clang-tidy`
with a reason; it is not waved through.

### Warnings are errors (issue #414)

`WarningsAsErrors: '*'` since the #414 fixes. The report was already scoped to
our TUs and headers only (`lint-c.sh` passes only our `.c` files and the
`HeaderFilterRegex` covers only our directories), so every diagnostic it
prints is ours to own — letting them through printed the six #414 findings
into the artifact while the job stayed green. Site suppressions
(`NOLINT` with a stated reason) are the release valve for false positives,
e.g. the two analyzer reports in `fpm_http_direct_conn.c` where `forget()`
unlinks before it frees and the loop legitimately re-reads the list head.

(It was its own `lint` job until the CI restructure of 2026-09-17, which
folded it in with the two hermetic doc/coverage checks -- three jobs that each
paid a runner allocation to run a script finishing in seconds, none of them on
the critical path.)
