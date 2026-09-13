/* fpm-ng: support tiers. See fpm_tier.h for the why. */

#include "fpm_config.h"

#include "fpm_tier.h"
#include "zlog.h"

void fpm_tier_announce(enum fpm_tier tier, const char *pool, const char *subject)
{
	int level;
	const char *label;
	const char *withholds;

	switch (tier) {
		case FPM_TIER_BETA:
			level = ZLOG_NOTICE;
			label = "BETA";
			withholds = "its directives may change in a minor release, "
				"and fixes carry no response-time commitment";
			break;
		case FPM_TIER_EXPERIMENTAL:
			level = ZLOG_WARNING;
			label = "EXPERIMENTAL";
			withholds = "its directives may change in any release, "
				"support is best effort, and it may be removed";
			break;
		default:
			/* Supported says nothing. The absence IS the announcement: an
			 * operator who reads their log once should be able to conclude
			 * that what they did not see is what carries the full promise. */
			return;
	}

	if (pool) {
		zlog(level, "[pool %s] %s is %s: %s -- see \"Support tiers\" in README.md",
			pool, subject, label, withholds);
	} else {
		zlog(level, "%s is %s: %s -- see \"Support tiers\" in README.md",
			subject, label, withholds);
	}
}
