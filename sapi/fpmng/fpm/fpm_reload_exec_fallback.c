/* fpm-ng: issue #925 -- see fpm_reload_exec_fallback.h. */

#include "fpm_config.h"

#include <errno.h>
#include <unistd.h>

#include "fpm_reload_exec_fallback.h"

void fpm_reload_exec_fallback(char *const *argv) /* {{{ */
{
#ifdef __linux__
	execv("/proc/self/exe", argv);
#else
	(void) argv;
	errno = ENOSYS;
#endif
}
/* }}} */
