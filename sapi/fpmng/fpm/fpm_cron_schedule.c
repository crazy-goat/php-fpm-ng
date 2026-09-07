/* fpm-ng: crontab-syntax schedule parser and "next run" calculator. See
 * fpm_cron_schedule.h. No dependencies beyond libc, on purpose (docs/NOTES.md
 * estimated ~100 lines, no deps — this is that).
 */

#include "fpm_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fpm_cron_schedule.h"

struct fpm_cron_shorthand_s {
	const char *name;
	const char *expansion;
};

static const struct fpm_cron_shorthand_s fpm_cron_shorthands[] = {
	{ "@hourly",  "0 * * * *" },
	{ "@daily",   "0 0 * * *" },
	{ "@weekly",  "0 0 * * 0" },
	{ "@monthly", "0 0 1 * *" },
	{ "@yearly",  "0 0 1 1 *" },
};

static int fpm_cron_parse_uint(const char **p, int *out) /* {{{ */
{
	const char *s = *p;
	int v = 0;

	if (!isdigit((unsigned char) *s)) {
		return -1;
	}
	while (isdigit((unsigned char) *s)) {
		v = v * 10 + (*s - '0');
		s++;
	}
	*out = v;
	*p = s;
	return 0;
}
/* }}} */

/* Parses one comma-separated element of a crontab field: "*", "N", "N-M",
 * "*\/S", "N/S" or "N-M/S". "N/S" (a start value with a step but no explicit
 * range) means "from N to the end of the field's domain", same as real
 * crontab(5) — distinct from a plain "N" (a single value, no step at all). */
static int fpm_cron_parse_item(const char *item, int min, int max,
		unsigned char *bitmap, const char *field_name, char *err, size_t err_len) /* {{{ */
{
	const char *p = item;
	int low, high, step;
	int has_range = 0;
	int i;
	int is_star = (*p == '*');

	if (is_star) {
		low = min;
		high = max;
		p++;
	} else {
		if (0 > fpm_cron_parse_uint(&p, &low)) {
			snprintf(err, err_len, "%s: expected a number, '*' or a range, got '%s'", field_name, item);
			return -1;
		}
		if (*p == '-') {
			p++;
			if (0 > fpm_cron_parse_uint(&p, &high)) {
				snprintf(err, err_len, "%s: expected a number after '-' in '%s'", field_name, item);
				return -1;
			}
			has_range = 1;
		} else {
			high = low;
		}
	}

	step = 1;
	if (*p == '/') {
		p++;
		if (0 > fpm_cron_parse_uint(&p, &step)) {
			snprintf(err, err_len, "%s: expected a number after '/' in '%s'", field_name, item);
			return -1;
		}
		if (step < 1) {
			snprintf(err, err_len, "%s: step must be a positive number, got '%s'", field_name, item);
			return -1;
		}
		if (!has_range && !is_star) {
			high = max;
		}
	}

	if (*p != '\0') {
		snprintf(err, err_len, "%s: unexpected trailing characters in '%s'", field_name, item);
		return -1;
	}

	if (low < min || low > max || high < min || high > max) {
		snprintf(err, err_len, "%s: value out of range in '%s' (allowed %d-%d)", field_name, item, min, max);
		return -1;
	}
	if (low > high) {
		snprintf(err, err_len, "%s: invalid range in '%s' (start after end)", field_name, item);
		return -1;
	}

	for (i = low; i <= high; i += step) {
		bitmap[i] = 1;
	}

	return 0;
}
/* }}} */

/* Parses one whole field (a comma-separated list of items) into bitmap[0..].
 * bitmap_size is the size in bytes of the caller's array, only used to zero
 * it out first. is_star (may be NULL) receives whether the field was the
 * literal single character "*" — used for the mday/wday OR rule. */
static int fpm_cron_parse_field(const char *field, int min, int max,
		unsigned char *bitmap, size_t bitmap_size, const char *field_name,
		unsigned char *is_star, char *err, size_t err_len) /* {{{ */
{
	char buf[128];
	char *item, *saveptr;

	memset(bitmap, 0, bitmap_size);

	if (is_star) {
		*is_star = !strcmp(field, "*");
	}

	if ((size_t) snprintf(buf, sizeof(buf), "%s", field) >= sizeof(buf)) {
		snprintf(err, err_len, "%s: field too long ('%s')", field_name, field);
		return -1;
	}
	if (!*buf) {
		snprintf(err, err_len, "%s: empty field", field_name);
		return -1;
	}

	item = strtok_r(buf, ",", &saveptr);
	if (!item) {
		snprintf(err, err_len, "%s: empty field", field_name);
		return -1;
	}
	while (item) {
		if (0 > fpm_cron_parse_item(item, min, max, bitmap, field_name, err, err_len)) {
			return -1;
		}
		item = strtok_r(NULL, ",", &saveptr);
	}

	return 0;
}
/* }}} */

int fpm_cron_schedule_parse(const char *expr, struct fpm_cron_schedule_s *out, /* {{{ */
		char *err, size_t err_len)
{
	char buf[256];
	char *fields[5];
	char *p, *saveptr;
	int n = 0;
	size_t i;
	unsigned char wday_tmp[8];

	if (!expr || !*expr) {
		snprintf(err, err_len, "empty schedule");
		return -1;
	}

	if (expr[0] == '@') {
		for (i = 0; i < sizeof(fpm_cron_shorthands) / sizeof(fpm_cron_shorthands[0]); i++) {
			if (!strcmp(expr, fpm_cron_shorthands[i].name)) {
				return fpm_cron_schedule_parse(fpm_cron_shorthands[i].expansion, out, err, err_len);
			}
		}
		snprintf(err, err_len,
			"unknown schedule shorthand '%s' (known: @hourly, @daily, @weekly, @monthly, @yearly)", expr);
		return -1;
	}

	if ((size_t) snprintf(buf, sizeof(buf), "%s", expr) >= sizeof(buf)) {
		snprintf(err, err_len, "schedule expression too long: '%s'", expr);
		return -1;
	}

	memset(out, 0, sizeof(*out));

	p = strtok_r(buf, " \t", &saveptr);
	while (p) {
		if (n < 5) {
			fields[n] = p;
		}
		n++;
		p = strtok_r(NULL, " \t", &saveptr);
	}

	if (n != 5) {
		snprintf(err, err_len,
			"expected 5 fields (minute hour day-of-month month day-of-week), got %d: '%s'", n, expr);
		return -1;
	}

	if (0 > fpm_cron_parse_field(fields[0], 0, 59, out->minute, sizeof(out->minute), "minute", NULL, err, err_len)) {
		return -1;
	}
	if (0 > fpm_cron_parse_field(fields[1], 0, 23, out->hour, sizeof(out->hour), "hour", NULL, err, err_len)) {
		return -1;
	}
	if (0 > fpm_cron_parse_field(fields[2], 1, 31, out->mday, sizeof(out->mday), "day-of-month",
			&out->mday_is_star, err, err_len)) {
		return -1;
	}
	if (0 > fpm_cron_parse_field(fields[3], 1, 12, out->month, sizeof(out->month), "month", NULL, err, err_len)) {
		return -1;
	}

	/* Dzien tygodnia: 0-7, gdzie zarowno 0 jak i 7 znacza niedziele —
	 * parsujemy w tymczasowym bitmapie 0..7, potem 7 skladamy w 0. */
	if (0 > fpm_cron_parse_field(fields[4], 0, 7, wday_tmp, sizeof(wday_tmp), "day-of-week",
			&out->wday_is_star, err, err_len)) {
		return -1;
	}
	for (i = 0; i < 7; i++) {
		out->wday[i] = wday_tmp[i];
	}
	if (wday_tmp[7]) {
		out->wday[0] = 1;
	}

	return 0;
}
/* }}} */

static int fpm_cron_schedule_matches(const struct fpm_cron_schedule_s *sched, const struct tm *tmv) /* {{{ */
{
	int day_match;

	if (!sched->minute[tmv->tm_min]) {
		return 0;
	}
	if (!sched->hour[tmv->tm_hour]) {
		return 0;
	}
	if (!sched->month[tmv->tm_mon + 1]) {
		return 0;
	}

	/* Klasyczna, zaskakujaca semantyka crona: kiedy OBA pola dnia sa
	 * ograniczone (zadne nie jest literalnym "*"), dopasowanie to SUMA (OR),
	 * nie iloczyn (AND). Musi tak byc, zeby harmonogramy przeniesione
	 * z systemowego crona dzialaly tak samo (np. "13. kazdego miesiaca ORAZ
	 * kazdy piatek", nie "13. tylko jesli akurat wypada w piatek"). Kiedy
	 * ktores z pol jest "*", sprowadza sie to do zwyklego AND, bo "*" i tak
	 * nie ogranicza niczego. */
	if (sched->mday_is_star && sched->wday_is_star) {
		day_match = 1;
	} else if (sched->mday_is_star) {
		day_match = sched->wday[tmv->tm_wday];
	} else if (sched->wday_is_star) {
		day_match = sched->mday[tmv->tm_mday];
	} else {
		day_match = sched->mday[tmv->tm_mday] || sched->wday[tmv->tm_wday];
	}

	return day_match;
}
/* }}} */

/* Applies `tz` as the process-wide TZ environment variable for the duration
 * of the search below, and restores whatever was there before. Safe here
 * (unlike a general-purpose reentrant helper would need to be) because both
 * callers are single-threaded for this purpose: a cron pool's child process
 * only ever computes its own single schedule (fpm_pool_cron_child_main()),
 * and the master's status computation (fpm_pool_cron_status()) runs on its
 * single event-loop thread, one pool at a time, never concurrently with
 * itself. glibc/macOS have no reentrant tzalloc()/localtime_rz() available
 * on every platform this project targets, so TZ + tzset() + localtime_r()
 * is the portable choice. */
static char *fpm_cron_schedule_tz_push(const char *tz) /* {{{ */
{
	char *saved = NULL;
	const char *cur = getenv("TZ");

	if (cur) {
		saved = strdup(cur);
	}
	setenv("TZ", tz, 1);
	tzset();
	return saved;
}
/* }}} */

static void fpm_cron_schedule_tz_pop(char *saved) /* {{{ */
{
	if (saved) {
		setenv("TZ", saved, 1);
		free(saved);
	} else {
		unsetenv("TZ");
	}
	tzset();
}
/* }}} */

time_t fpm_cron_schedule_next(const struct fpm_cron_schedule_s *sched, time_t after, const char *tz) /* {{{ */
{
	/* Zawsze zaczynamy szukanie od NASTEPNEJ pelnej minuty po "after", nigdy
	 * od samego "after" ani od "ostatnio widzianej minuty" — to jest cale
	 * "brak nadrabiania zgubionych przebiegow" i "brak podwojnego odpalenia
	 * w tej samej minucie po szybkim powrocie" za darmo. Nie pytamy "co
	 * przegapilem", tylko "co jest najblizej w przyszlosci" — wiec proces
	 * odrodzony chwile po tym, jak poprzedni skonczyl przebieg w tej samej
	 * minucie, nigdy nie znajdzie ponownie minuty, ktora wlasnie minela,
	 * a master wylaczony na godzine po prostu leci od najblizszego
	 * przyszlego terminu, bez dwunastu zalegych przebiegow naraz. */
	time_t t = (after / 60 + 1) * 60;
	/* Limit poszukiwan: ok. 4 lata w minutach. Chroni WYLACZNIE przed
	 * harmonogramem, ktory strukturalnie nigdy nie moze zajsc (np. dzien 30
	 * lutego z dniem tygodnia rowniez ograniczonym do czegos, co nigdy nie
	 * wypada w lutym, wiec regula OR tez nie ratuje) — walidacja przy
	 * starcie nie lapie tego dzis, wiec to jest ostatnia siatka
	 * bezpieczenstwa przed nieskonczona petla, nie normalna sciezka. */
	time_t limit = t + (time_t) 4 * 366 * 24 * 60 * 60;
	char *saved_tz = NULL;
	time_t result = (time_t) -1;

	if (tz && *tz) {
		saved_tz = fpm_cron_schedule_tz_push(tz);
	}

	while (t < limit) {
		struct tm tmv;

		if (tz && *tz) {
			localtime_r(&t, &tmv);
		} else {
			gmtime_r(&t, &tmv);
		}
		if (fpm_cron_schedule_matches(sched, &tmv)) {
			result = t;
			break;
		}
		t += 60;
	}

	if (tz && *tz) {
		fpm_cron_schedule_tz_pop(saved_tz);
	}

	return result;
}
/* }}} */
