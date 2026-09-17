# C style and static analysis

Task 013. How this tree stays comparable to upstream `sapi/fpm/`.

## EditorConfig

`.editorconfig` matches php-src's (tabs for C/headers, LF, final newline,
`tab_width = 4`). Editors that honour EditorConfig need no project-specific
setup beyond that file.

## clang-format: not adopted

php-src ships **no** `.clang-format`. `build/prepare.sh` copies `sapi/fpm/`
and overlays our files so both live in one directory and get diffed against
each other. A format style that diverges from upstream would make that
comparison harder for no gain.

If a `.clang-format` is ever added, it must run only on paths that exist in
**this** repository (`sapi/fpmng/`, `ext/fpmng_metrics/`), never on a prepared
php-src tree (that tree mixes untouched upstream copies with our overlays).
`build/lint-c.sh` already enforces that file-list boundary for clang-tidy;
the same rule would apply to format.

Bulk reformat of the existing tree is out of scope here (destroys `git blame`
on comments that are the primary documentation). If it happens, it is its own
commit plus an entry in `.git-blame-ignore-revs`.

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

(It was its own `lint` job until the CI restructure of 2026-09-17, which
folded it in with the two hermetic doc/coverage checks -- three jobs that each
paid a runner allocation to run a script finishing in seconds, none of them on
the critical path.)
