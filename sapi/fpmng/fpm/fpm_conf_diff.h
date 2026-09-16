/* fpm-ng: issue #330 -- selective reload's config diff.
 *
 * Decides, pool by pool, whether the config file(s) that fpm_conf.c would
 * parse on a reload actually differ from what THIS generation is currently
 * running with, WITHOUT touching fpm_worker_all_pools or any other live
 * parser state: fpm_conf.c's parser has no notion of "parse into a scratch
 * list, then throw it away" -- it always builds directly into the global pool
 * list, which is exactly the list a running master is actively using to
 * serve requests. Re-entering it mid-flight to build a second, comparison-only
 * copy would be a much larger and riskier change to an existing file than
 * this issue's payoff justifies.
 *
 * Instead this reads the same file(s) fpm_conf.c would read, as plain text,
 * split into per-[section] bodies, and compares those bodies byte for byte.
 * That sidesteps the field-by-field struct comparison the issue's own
 * write-up suggested (fpm_worker_pool_config_s is large, and a naive memcmp()
 * of it is unsafe -- it holds zend_string* and other pointers whose bytes
 * differ across two independently parsed copies even when the values they
 * point to are identical) by never constructing that second parsed copy in
 * the first place: two config sections that read the same text always
 * produce the same struct, so comparing the text is exactly equivalent to
 * comparing the struct it would parse into, and cannot be fooled by pointer
 * identity the way memcmp() could.
 *
 * Safety bias: whenever this cannot confidently answer (I/O error, a missing
 * file, an include= this reader cannot resolve exactly like fpm_conf.c's own
 * parser would), the pool is reported CHANGED, never UNCHANGED. A false
 * "changed" costs one extra, otherwise-unnecessary restart of that pool --
 * exactly what reload.selective = no already does for it today. A false
 * "unchanged" would silently keep stale workers running past a config change
 * an operator asked for, which is the one outcome this feature must never
 * produce.
 */

#ifndef FPM_CONF_DIFF_H
#define FPM_CONF_DIFF_H 1

/* Reads `config_file` (and any include= it references, one level, the same
 * way fpm_conf.c's own include handling does) and remembers it as "what this
 * generation is running", for a later fpm_conf_diff_pool_unchanged() call to
 * diff the NEXT reload's file against. Called once, from fpm_conf_init_main()
 * right after a successful parse -- see fpm_conf.c. Best-effort: a read
 * failure here just means the next reload cannot prove any pool unchanged
 * (fpm_conf_diff_pool_unchanged() then reports everything CHANGED, the safe
 * default), not a startup failure. */
void fpm_conf_diff_snapshot_current(const char *config_file);

/* Called once per reload, before the per-pool loop that would otherwise ask
 * fpm_conf_diff_pool_unchanged() once per pool -- re-reading and re-splitting
 * the file(s) on every one of those calls would be wasted work. Re-reads
 * `config_file` and caches the result for the calls below, replacing whatever
 * a previous reload pass cached. Returns 1 if the new snapshot was taken
 * successfully (comparisons below may report UNCHANGED), 0 if not (every
 * pool reports CHANGED for this pass -- see the safety bias above). */
int fpm_conf_diff_begin_reload_pass(const char *config_file);

/* True only if `pool_name` names a [section] present in BOTH the snapshot
 * fpm_conf_diff_snapshot_current() took and the one the current
 * fpm_conf_diff_begin_reload_pass() took, with byte-identical body text.
 * False for a new pool, a removed one, a changed one, or any of the safety
 * cases fpm_conf_diff_begin_reload_pass() above already covers -- including
 * when it was never called (fpm_signal_sent uninitialized -> pass not begun)
 * or returned 0. */
int fpm_conf_diff_pool_unchanged(const char *pool_name);

/* Releases both cached snapshots. Not required before process exit (the
 * execvp() a selective reload ends in discards this heap exactly like
 * everything else non-shared) -- provided for the reload path that does NOT
 * exec (fpm_pctl_exec() failing execvp() itself, or a future caller that
 * wants to free eagerly) and for tests. */
void fpm_conf_diff_shutdown(void);

#endif
