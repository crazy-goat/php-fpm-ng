/* fpm-ng: the upstream zlog.h, plus one hook — issue #130.
 *
 * Everything upstream declares still comes from upstream: build/prepare.sh
 * keeps a copy of THIS php-src's zlog.h next to us as zlog_upstream.h and this
 * file includes it. Nothing is duplicated here, so struct zlog_stream (which
 * gained two bit-fields in 8.5) and every prototype stay whatever the tree we
 * are compiled into says they are.
 *
 * The hook: zlog_buf_prefix() (zlog.c) omits the timestamp whenever
 * fpm_globals.is_child is set, because an upstream FPM child logs through a
 * pipe that the master decorates. An fpmng HTTP gateway process keeps the
 * master's error_log fd and writes to it directly, so its lines landed in the
 * shared file undecorated, next to the master's decorated ones — and the pair
 * of lines issue #118 asks an operator to compare could not be correlated by
 * time at all. Routing zlog() through fpmng_zlog_ex() restores the decoration
 * for such a process; see fpm_child_error_log.h for how it does it and for
 * which processes opt in. Every other process gets one predictable branch and
 * an unchanged line.
 *
 * Only the zlog() macro is rerouted. zlog_msg() and the zlog_stream API are
 * the master's paths for captured child output and for the access log, both of
 * which already decorate (or deliberately do not) on the master side.
 */

#ifndef FPMNG_ZLOG_H
#define FPMNG_ZLOG_H 1

#include "zlog_upstream.h"

void fpmng_zlog_ex(const char *function, int line, int flags, const char *fmt, ...)
		__attribute__ ((format(printf,4,5)));

#undef zlog
#define zlog(flags,...) fpmng_zlog_ex(__func__, __LINE__, flags, __VA_ARGS__)

#endif
