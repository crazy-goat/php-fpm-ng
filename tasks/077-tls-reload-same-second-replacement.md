# 077 — TLS hot reload misses a certificate replaced in the same second

Status: todo
Type: bug
Depends on: —
Related: 040 (added the reload timer), 048 (measured this from the test side)

## Why

The master-side reload tick decides whether anything changed by comparing
whole-second modification times:

```
if (cert_st.st_mtime == r->cert_mtime && key_st.st_mtime == r->key_mtime) {
	return;
}
```

`sapi/fpmng/fpm/fpm_http_tls_reload.c:140-142`. `st_mtime` is a `time_t` —
second resolution. A certificate/key pair replaced inside the same wall-clock
second as the previously seen pair is therefore indistinguishable from "nothing
changed", and the tick returns without validating, without loading, and without
bumping the generation. Nothing is logged, so no gateway has anything to adopt
(`fpm_http_tls_reload.c:327` is the only per-gateway adoption notice) and the
old certificate keeps serving indefinitely — until some *later* write happens to
land in a different second.

This is not only a test artefact. It was found from the test side, where task
048 measured it: `build/test-http-tls-reload.sh` copied a new pair immediately
after startup and the master saw no change in 2 of 5 runs on the test box, the
same symptom as CI build-matrix runs 34139815139, 34140641442, 34121726613,
34120910345 and 34120774897. Task 048 worked around it in the test by retrying
the copy until the mtime actually moves (`swap_in()` in that script). But an
operator hits the same window in ordinary use:

- a deploy hook that writes the cert and the key and then, on failure, rolls
  back to the previous pair — a `cp cert; cp key` rollback landing in the same
  second as the renewal it reverts is silently ignored, and the pool goes on
  serving the certificate the operator just rolled *back*;
- any renewal script fast enough that two consecutive writes share a second;
- a filesystem whose timestamp granularity is coarser than a second, where the
  window is wider still.

The rejected-pair path makes it worse in one specific way. When validation
fails, the tick records the mtimes of the pair it just *rejected*
(`fpm_http_tls_reload.c:153-160`) so a persistently broken pair does not re-log
every tick. That is the right call on its own, but it means a corrected pair
written in the same second as the broken one it fixes is also invisible: the
operator sees the rejection in the log, fixes the file immediately, and gets no
second message either way.

Note what shape a fix can take. `st_mtim.tv_nsec` is not portable enough to
lean on — several filesystems and NFS mounts leave it zero or coarse, so
sub-second mtime turns the bug from "always" into "sometimes" rather than
fixing it. Content-derived identity (a digest of both files) or a
`(mtime, size, inode)` tuple do not depend on clock granularity. Whoever picks
this up chooses; the criteria below are written so they do not presuppose one.

## What

Make a certificate/key replacement visible to the reload tick regardless of how
close in time it is to the previous one.

## Acceptance criteria

1. A pool serving cert A adopts cert B when B is copied over the live paths
   with `touch -r` (or an equivalent) forcing B's mtime to equal A's: the
   `adopted reloaded TLS certificate` notice appears once per gateway process
   within 30 x `http.tls_reload_check`, and every gateway then serves B's
   serial. A test covers this and fails on today's code.
2. The same holds for the second half of the pair on its own: an unchanged cert
   file with a replaced key at an identical mtime is detected.
3. A pair that fails validation, followed by a corrected pair written at the
   same mtime, ends with the corrected pair adopted — one rejection line in the
   error log, then the adoption notice. This is the `:153-160` case.
4. A tick where genuinely nothing changed still does no work: no validation, no
   load, no generation bump, nothing logged. Demonstrate it by leaving a pool
   idle for at least 10 ticks and showing the generation is unchanged and the
   error log has no new lines.
5. `build/test-http-tls-reload.sh` still passes 10 consecutive runs. If the fix
   makes its `swap_in()` retry loop unnecessary, delete the loop and the part
   of its comment that only exists to explain the retry, keeping the run-ID
   evidence; if it is still needed, say why in the Outcome.
