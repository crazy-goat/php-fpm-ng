/* fpm-ng: a failure-injection seam for the test suite (issue #832). The
 * rationale -- why this is a compile flag and the syntax of FPMNG_FAULT -- is
 * in fpm_fault_inject.h. Read that first. */

#include "fpm_config.h"
#include "fpm_fault_inject.h"

#ifdef HAVE_FPMNG_FAULT_INJECT

#include <stdlib.h>
#include <string.h>

#include "zlog.h"

#define FPM_FAULT_INJECT_ENV "FPMNG_FAULT"
#define FPM_FAULT_INJECT_IFENV ":ifenv="

/* One entry of the list: `point` or `point:ifenv=VAR`. Both pointers are into
 * the caller's copy of the variable. */
static int fpm_fault_inject_entry_matches(char *entry, const char *point) /* {{{ */
{
	char *cond = strchr(entry, ':');

	if (cond) {
		*cond = '\0';
	}

	if (strcmp(entry, point) != 0) {
		return 0;
	}

	if (!cond) {
		return 1;
	}

	*cond = ':';
	if (strncmp(cond, FPM_FAULT_INJECT_IFENV, sizeof(FPM_FAULT_INJECT_IFENV) - 1) != 0) {
		/* An unknown condition must not arm the point: a typo would turn a
		 * test of the new master into a failure of the first start. */
		return 0;
	}

	cond += sizeof(FPM_FAULT_INJECT_IFENV) - 1;
	return *cond && getenv(cond) != NULL;
}
/* }}} */

int fpm_fault_inject_hit(const char *point) /* {{{ */
{
	const char *raw = getenv(FPM_FAULT_INJECT_ENV);
	char *copy, *save = NULL, *entry;
	int hit = 0;

	if (!raw || !*raw) {
		return 0;
	}

	copy = strdup(raw);
	if (!copy) {
		return 0;
	}

	for (entry = strtok_r(copy, ",", &save); entry; entry = strtok_r(NULL, ",", &save)) {
		if (fpm_fault_inject_entry_matches(entry, point)) {
			hit = 1;
			break;
		}
	}
	free(copy);

	if (hit) {
		zlog(ZLOG_WARNING, "fault injection: '%s' fails now (" FPM_FAULT_INJECT_ENV "); "
						   "this is a test build, not a real failure",
				point);
	}

	return hit;
}
/* }}} */

#endif
