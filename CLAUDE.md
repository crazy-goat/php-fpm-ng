# php-fpm-ng — working rules

A separate SAPI in `sapi/fpmng/`, built alongside upstream `sapi/fpm/`.
**Not a fork of php-src.** Target: small projects on a single VPS — one binary
plus application code in a container image, no nginx, no supervisord, no system
cron, no shell. One configuration file describes the application together with
its workers and crons.

## Language: English, everywhere

Decided 2026-09-06. English is the only language in this project:

- code comments in our own files
- `docs/`, `README.md`, `sapi/fpmng/README.md`, `tasks/`
- commit messages
- log messages, error messages and configuration documentation

Two exceptions, both about files we do not own:

- Comments inherited from upstream in files that `build/prepare.sh` copies from
  `sapi/fpm/` are left alone. That script re-copies them on every run, so any
  edit there is lost anyway.
- Quoted material stays verbatim: error strings, log lines, command output,
  measurements. Never translate something that appears in the output of a
  program.

Existing Polish text is translated incrementally, file by file, one commit per
file — see `tasks/012`. Do not bulk-translate.

## Comments: what earns one

Comments in this codebase are the primary record of **why**, and most of what
they record cannot be recovered from the code.

- **Keep** a comment that justifies a decision, cites a measurement, names a
  rejected alternative and the reason it was rejected, or warns about a trap the
  code cannot express.
- **Delete** a comment that restates the line below it.
- **Never** delete a comment containing a number, an error message, or a
  `file:line` reference without independently re-establishing the fact first.

There is no scheduled comment-cleanup pass. Redundant comments go away
opportunistically, in the same commit as a real change to that code.

## Architecture contract

- **Never open a PR against upstream php/php-src.**
- New behaviour goes into **new files** under `sapi/fpmng/fpm/`. Existing
  php-src files get minimal hooks only.
- Per-pool-type behaviour is a **field, a callback, or data** in
  `fpm_pool_type_s` (`sapi/fpmng/fpm/fpm_pool_type.h`). Never
  `if (type == ...)`, never `strcmp(type->name, ...)`.
- `sapi/fpmng/config.m4` is not hand-edited in its source-list part:
  `build/prepare.sh` substitutes `@FPMNG_SOURCES@` from `find fpm -name '*.c'`.
  An existing build directory has a frozen object list and will not see a new
  `.c` file — `buildconf --force` plus `config.nice` is required. The script
  warns about this at the end of its output; read that warning.
- Patches against php-src live in `patches/` and are applied by `prepare.sh`.
  Anything that must change in core belongs there, gated so that upstream
  behaviour is the default.

## Evidence

- Claims about behaviour are backed by a `file:line`, a measurement, or a named
  document. A guess is written as a guess.
- Report what was actually measured. "Not measured" is a complete answer and is
  preferred over an estimate presented as a result.
- Before measuring, confirm you are measuring the binary you think you are
  (`strings` on a distinctive literal). This project has already drawn a false
  conclusion from measuring someone else's build.

## Test box

`192.168.8.50`, user `piotr`, passwordless sudo. It is **shared** — other work
runs there concurrently.

- Work in your own directory and your own port range.
- Never `pkill php-fpm` or anything matching by binary name. Kill by port
  (`ss -lntp`) or by the pid file of your own pool.
- MySQL (3306) and Redis (6379) are shared: use your own database and Redis
  index, never `FLUSHALL`.
- `pgrep -f "<pattern>"` matches its own command line. Do not use it to wait for
  a job to finish.

## Tasks

Open work is one file per task in `tasks/`, finished work moves to
`tasks/done/`. See `tasks/README.md`.
