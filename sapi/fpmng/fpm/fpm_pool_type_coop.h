/* fpm-ng: the fiber/async executor variants, kept out of fpm_pool_type.c.
 *
 * Issue #371 (part of the #370 epic): a long-lived branch (issue #373's cut)
 * needs its diff against main to be additive -- a file main does not have,
 * plus appends to the tail of things main owns. fpm_pool_type.c is the one
 * file every pool-type change touches, so the fiber and async executor
 * structs cannot live inside it without conflicting on every merge as the
 * base type table grows. They live here instead, reached through the one
 * lookup below.
 *
 * This file is ALWAYS compiled (see build/prepare.sh: its name matches
 * neither the fiber/coop nor the async source pattern), regardless of
 * --enable-fpmng-fiber / --enable-fpmng-async. What is conditional is only
 * the BODY of fpm_pool_type_coop_variant() -- see fpm_pool_type_coop.c --
 * exactly the way the four struct literals it used to hand back were
 * conditional before this refactor. Adding or removing a variant is a change
 * to that one function; fpm_pool_type.c never needs to know.
 */

#ifndef FPM_POOL_TYPE_COOP_H
#define FPM_POOL_TYPE_COOP_H 1

struct fpm_pool_type_s;

/* The executor variant of base pool.type = type_name ("http"; "fastcgi-ng",
 * which this function also served before issue #376, is retired in 0.9.0)
 * for pool.executor = executor_name ("fiber" or "async"), or NULL when this
 * binary was not built with the configure flag that provides it. Called once
 * per (type, executor) pair, lazily, the first time fpm_pool_type.c needs to
 * fill in an executor-table entry -- see fpm_pool_type_get() in
 * fpm_pool_type.c. */
const struct fpm_pool_type_s *fpm_pool_type_coop_variant(const char *type_name, const char *executor_name);

#endif
