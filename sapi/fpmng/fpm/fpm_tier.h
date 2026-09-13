/* fpm-ng: support tiers.
 *
 * Issue #269 decided three tiers, each defined by the promise it WITHHOLDS
 * rather than by how finished the code feels, and decided that a tier is
 * announced in three places or it does not count: as data on the thing itself,
 * as one line at startup, and as a table in README.md. This file is the second
 * of the three, and it is one file rather than a sentence repeated per feature
 * so that the wording and the level cannot drift apart between them.
 *
 * What each tier withholds is in README.md, under "Support tiers"; the startup
 * line points there instead of repeating the table into the log.
 */

#ifndef FPM_TIER_H
#define FPM_TIER_H 1

/* EXPERIMENTAL is 0 on purpose: a new pool type or feature that says nothing
 * about itself announces itself as the least-promised of the three. The honest
 * default for code nobody has classified is the one that withholds everything,
 * and it makes forgetting the field loud rather than silent. */
enum fpm_tier {
	FPM_TIER_EXPERIMENTAL = 0,
	FPM_TIER_BETA,
	FPM_TIER_SUPPORTED
};

/* One line, in the master, at startup. NOTICE for beta, WARNING for
 * experimental, and NOTHING AT ALL for supported -- a line an operator sees
 * for every pool of every tier is a line they learn to filter, and then the
 * two that matter go with it.
 *
 * pool is the pool section's name, or NULL for a feature that is not a pool
 * (TLS termination, ACME). subject is what is being classified, spelled the
 * way the operator wrote it where there is a way: "pool.type = http-direct
 * with pool.executor = worker", not an internal symbol.
 *
 * Per pool at startup, never per child and never per request: see the call in
 * fpm_run() (fpm.c). */
void fpm_tier_announce(enum fpm_tier tier, const char *pool, const char *subject);

#endif
