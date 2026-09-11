/* Prototype for the patches/0006 symbol a distribution libphp does not have,
 * so the compat unit and its callers agree on the signature. See
 * libphp_compat.c for why a no-op is the right content and what it costs.
 */
#ifndef FPMNG_LIBPHP_COMPAT_H
#define FPMNG_LIBPHP_COMPAT_H
/* _Bool, not bool: this header is force-included with -include ahead of
 * every other header, so <stdbool.h> has not been seen yet. */
void zend_signal_use_persistent_handlers(_Bool enable);
#endif
