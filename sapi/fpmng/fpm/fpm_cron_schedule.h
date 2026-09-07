/* fpm-ng: crontab-syntax schedule parser and "next run" calculator, used by
 * pool.type = cron. No dependencies beyond libc. See docs/NOTES.md for the
 * design writeup — in particular why this works entirely in UTC and why it
 * deliberately never "catches up" on missed runs.
 */

#ifndef FPM_CRON_SCHEDULE_H
#define FPM_CRON_SCHEDULE_H 1

#include <time.h>

/* Bitmaps for the five crontab fields. Indices are the natural field values
 * (minute 0-59, hour 0-23, day-of-month 1-31, month 1-12, day-of-week 0-6
 * with 0 = Sunday; day-of-week 7 is folded into 0 at parse time). */
struct fpm_cron_schedule_s {
	unsigned char minute[60];
	unsigned char hour[24];
	unsigned char mday[32];		/* index 0 unused, 1..31 valid */
	unsigned char month[13];	/* index 0 unused, 1..12 valid */
	unsigned char wday[7];		/* 0..6, 0 = Sunday */

	/* Whether the day-of-month / day-of-week field was the LITERAL
	 * character "*" (not e.g. "*\/1", which is a restriction that merely
	 * happens to match everything). This drives the classic cron OR rule
	 * for these two fields — see fpm_cron_schedule_matches() in the .c file. */
	unsigned char mday_is_star;
	unsigned char wday_is_star;
};

/* Parses a 5-field crontab expression, or one of the @hourly/@daily/@weekly/
 * @monthly/@yearly shorthands, into *out. On syntax error returns -1 and
 * writes a human-readable message into err (a buffer of err_len bytes owned
 * by the caller) — the caller is expected to reject the configuration and
 * show this message, not to guess at "close enough". Returns 0 on success. */
int fpm_cron_schedule_parse(const char *expr, struct fpm_cron_schedule_s *out,
	char *err, size_t err_len);

/* Computes the next time (epoch seconds, always exactly at the start of
 * a minute) that matches *sched and is strictly greater than `after`. Never
 * returns a time <= `after` — this is what makes "no catch-up" and "no
 * double-fire in the same minute after a fast run" free: the caller always
 * asks "what's next after right now", never "what did I miss". Returns
 * (time_t) -1 only if no match could be found within a bounded search
 * horizon (a schedule that can structurally never match, e.g. day 30 of
 * February with day-of-week left unrestricted so the OR rule never saves
 * it) — this should have been caught by validation already, so this return
 * value is a last-resort guard, not a normal code path.
 *
 * `tz` is an IANA zone name (e.g. "Europe/Warsaw") or NULL/"" for UTC (the
 * default, unchanged from before this parameter existed). When set, the
 * schedule's minute/hour/day/month/weekday fields are matched against that
 * zone's local time instead of UTC. Two local-time edge cases follow
 * directly from scanning real UTC instants and converting each one, with no
 * special-case code: a wall-clock time that a DST transition skips (spring
 * forward) simply never matches, because no UTC instant maps to it; a
 * wall-clock time that a transition repeats (fall back) can match twice in
 * the same day, because two distinct UTC instants map to it. This is the
 * same behaviour ordinary crontab(5) has when it runs in local time — an
 * accepted once-a-year quirk, not something this function tries to correct.
 * See fpm_pool_cron.c for how `tz` is applied (TZ environment variable,
 * saved and restored around the call — see that file for why). */
time_t fpm_cron_schedule_next(const struct fpm_cron_schedule_s *sched, time_t after, const char *tz);

#endif
