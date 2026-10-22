/* fpm-ng: a failure-injection seam for the test suite (issue #832). The
 * rationale -- why this is a compile flag, the syntax of FPMNG_FAULT and which
 * functions a signal handler may call -- is in fpm_fault_inject.h. Read that
 * first. */

#include "fpm_config.h"
#include "fpm_fault_inject.h"

#ifdef HAVE_FPMNG_FAULT_INJECT

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FPM_FAULT_INJECT_ENV "FPMNG_FAULT"
#define FPM_FAULT_INJECT_IFENV ":ifenv="
#define FPM_FAULT_INJECT_MAX_POINTS 16
#define FPM_FAULT_INJECT_NAME_LEN 64

/* The table is written once, by the constructor below, before main(): every
 * later reader (a handler included) only reads it, and a forked child inherits
 * it. An entry whose `ifenv=` variable was not set at that moment is not
 * stored, so it never hits. */
static char fpm_fault_inject_points[FPM_FAULT_INJECT_MAX_POINTS][FPM_FAULT_INJECT_NAME_LEN];
static int fpm_fault_inject_count;

/* One entry of the list: `point` or `point:ifenv=VAR`. `entry` is a copy that
 * this function edits. */
static void fpm_fault_inject_add(char *entry) /* {{{ */
{
	char *cond = strchr(entry, ':');

	if (cond) {
		if (strncmp(cond, FPM_FAULT_INJECT_IFENV, sizeof(FPM_FAULT_INJECT_IFENV) - 1) != 0) {
			/* An unknown condition must not arm the point: a typo would turn a
			 * test of the new master into a failure of the first start. */
			return;
		}
		*cond = '\0';
		cond += sizeof(FPM_FAULT_INJECT_IFENV) - 1;
		if (!*cond || getenv(cond) == NULL) {
			return;
		}
	}

	if (!*entry || strlen(entry) >= FPM_FAULT_INJECT_NAME_LEN || fpm_fault_inject_count >= FPM_FAULT_INJECT_MAX_POINTS) {
		return;
	}

	strcpy(fpm_fault_inject_points[fpm_fault_inject_count++], entry);
}
/* }}} */

/* A constructor and not a call from fpm_run(): it needs no second hook in the
 * php-src files, and the table exists before any handler can run. getenv() and
 * malloc() are safe here: nothing else is running yet. */
__attribute__((constructor)) static void fpm_fault_inject_load(void) /* {{{ */
{
	const char *raw = getenv(FPM_FAULT_INJECT_ENV);
	char *copy, *save = NULL, *entry;

	if (!raw || !*raw) {
		return;
	}

	copy = strdup(raw);
	if (!copy) {
		return;
	}

	for (entry = strtok_r(copy, ",", &save); entry; entry = strtok_r(NULL, ",", &save)) {
		fpm_fault_inject_add(entry);
	}
	free(copy);
}
/* }}} */

int fpm_fault_inject_hit(const char *point) /* {{{ */
{
	static const char head[] = "fault injection: '";
	static const char tail[] = "' fails now (" FPM_FAULT_INJECT_ENV "); this is a test build, not a real failure\n";
	char line[sizeof(head) + FPM_FAULT_INJECT_NAME_LEN + sizeof(tail)];
	size_t len = 0, n;
	int i;

	for (i = 0; i < fpm_fault_inject_count; i++) {
		if (strcmp(fpm_fault_inject_points[i], point) == 0) {
			break;
		}
	}
	if (i == fpm_fault_inject_count) {
		return 0;
	}

	/* write(2), not zlog(): zlog() is not async-signal-safe. */
	memcpy(line, head, sizeof(head) - 1);
	len = sizeof(head) - 1;
	n = strlen(fpm_fault_inject_points[i]);
	memcpy(line + len, fpm_fault_inject_points[i], n);
	len += n;
	memcpy(line + len, tail, sizeof(tail) - 1);
	len += sizeof(tail) - 1;
	/* The result is ignored: a lost note must not change the injected failure. */
	n = (size_t) write(STDERR_FILENO, line, len);
	(void) n;

	return 1;
}
/* }}} */

#endif
