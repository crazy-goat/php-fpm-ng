/* fpm-ng: pool.executor = fiber — surface of fpm_pool_fiber_select.c used by
 * the stream_select() patch (patches/0008-fiber-stream-select.patch, gated on
 * HAVE_FPMNG_FIBER).
 *
 * The patch changes one call site in ext/standard/streamsfuncs.c
 * (PHP_FUNCTION(stream_select)): php_select(...) becomes fpm_fiber_select(...).
 * Everything else in that function — parsing arguments, building the fd_sets
 * from the stream arrays, and reading them back afterwards — stays exactly as
 * upstream wrote it. See task 006 (tasks/nice-to-have/006-fiber-stream-select.md)
 * for why the interception happens here and not at the transport layer, like
 * the rest of the fiber executor's socket handling. */

#ifndef FPM_POOL_FIBER_SELECT_H
#define FPM_POOL_FIBER_SELECT_H 1

#include <sys/select.h>
#include <sys/time.h>

/* Drop-in replacement for php_select()/select(): same signature, same
 * postconditions (rfds/wfds/efds narrowed to the ready subset, return count of
 * ready descriptors, 0 on timeout, -1 on error with errno set). Outside a
 * fiber that can suspend, and whenever efds is non-empty, this calls the real
 * select() and blocks exactly as upstream does — see the .c file for why. */
int fpm_fiber_select(int max_fd, fd_set *rfds, fd_set *wfds, fd_set *efds, struct timeval *timeout);

#endif
