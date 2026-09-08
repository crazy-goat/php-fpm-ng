# 060 — Static file serving for HTTP-direct pools

Status: open
Depends on: —

## Why

HTTP-direct pools execute one fixed front controller for every path; files that
need no PHP still pay a full PHP request lifecycle. The `http` gateway serves
static files without involving PHP. With the worker owning the connection, the
direct transport can do better than either: route in PHP when needed and serve
bytes with zero-copy (sendfile) through the existing evbuffer, without a
second process or an internal proxy hop.

## Scope

Opt-in static file handling for direct pools: a documented, validated
configuration declaring a static root and allowed file classes (extension or
explicit list), served directly by the worker's event loop. The front
controller stays the fallback for everything else. Directory traversal,
symlink policy, and dotfiles must be resolved by explicit rules that fail
validation when underspecified, following the task 054 principle that
unsupported or ambiguous configuration is rejected rather than guessed.

## Acceptance criteria

- Static files are served without a PHP request (verified by scoreboard/request
  counters not advancing) with correct `Content-Type`, `Content-Length`,
  `Last-Modified`, and range or conditional request support as documented.
- Zero-copy or mmap delivery verified by measurement (CPU per byte compared
  with the front-controller path and with nginx serving the same file on the
  poligon); raw artifacts retained.
- Traversal attempts (`..`, encoded separators, absolute paths, symlink escapes)
  are refused with tested behavior; dotfiles and undersized configuration fail
  per the documented rules.
- Unsupported directives fail validation; the task 054 routing/framing tests
  keep passing unchanged.
- Documentation in `docs/http-direct.md` states what static handling covers and
  what it deliberately does not (e.g. no compression, no caching headers beyond
  the documented set).

## Out of scope

- Compression (gzip/brotli), cache-control policy, and CDNs; can be follow-ups.
- Any change to how the `http` gateway or nginx serve static content.
