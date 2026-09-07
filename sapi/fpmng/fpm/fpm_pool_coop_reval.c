/* fpm-ng: detect changes to loaded files — see fpm_pool_coop_reval.h. */

#include "fpm_config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "php.h"
#include "zend_compile.h"
#include "zend_stream.h"

#include "fpm_pool_coop.h"
#include "fpm_pool_coop_reval.h"
#include "zlog.h"

/* Nanosecond mtime: macOS has st_mtimespec, glibc/musl st_mtim (and define
 * st_mtime as a macro for its tv_sec). Fall back to seconds without either. */
#if defined(__APPLE__)
# define FPM_REVAL_MTIM(st) ((st)->st_mtimespec)
#elif defined(st_mtime)
# define FPM_REVAL_MTIM(st) ((st)->st_mtim)
#endif

struct fpm_coop_reval_rec_s {
	time_t mtime_sec;
	long mtime_nsec;
	off_t size;
	dev_t dev;
	ino_t ino;
};

/* Key: opened_path (realpath, the same key as EG(included_files)); value:
 * fpm_coop_reval_rec_s. Persistent table — lives for the process lifetime,
 * outside the request arena. */
static HashTable fpm_coop_reval_files;
static bool fpm_coop_reval_on = false;
static unsigned fpm_coop_reval_sweeps = 0;
static unsigned fpm_coop_reval_stat_calls = 0;

static zend_op_array *(*fpm_coop_reval_orig_compile_file)(zend_file_handle *file_handle, int type);

static void fpm_coop_reval_rec_free(zval *zv) /* {{{ */
{
	pefree(Z_PTR_P(zv), 1);
}
/* }}} */

static void fpm_coop_reval_rec_fill(struct fpm_coop_reval_rec_s *rec, const struct stat *st) /* {{{ */
{
#ifdef FPM_REVAL_MTIM
	rec->mtime_sec = FPM_REVAL_MTIM(st).tv_sec;
	rec->mtime_nsec = FPM_REVAL_MTIM(st).tv_nsec;
#else
	rec->mtime_sec = st->st_mtime;
	rec->mtime_nsec = 0;
#endif
	rec->size = st->st_size;
	rec->dev = st->st_dev;
	rec->ino = st->st_ino;
}
/* }}} */

static int fpm_coop_reval_stat(const char *path, struct stat *st) /* {{{ */
{
	fpm_coop_reval_stat_calls++;
	return stat(path, st);
}
/* }}} */

/* First sight of a file in the process: remember what it looked like. Later
 * compilations of the same file (require without _once, mode without shared
 * included_files) only look it up in the table — the first state wins, because
 * it is the source of the functions and classes in the process tables. */
static void fpm_coop_reval_remember(zend_string *path) /* {{{ */
{
	struct stat st;
	struct fpm_coop_reval_rec_s *rec;
	zend_string *key;

	if (zend_hash_exists(&fpm_coop_reval_files, path)) {
		return;
	}
	/* Stat() AFTER compilation, by path: if the file was replaced between
	 * open() and this stat(), we would remember the new state with old code.
	 * The window is only the first-compilation microseconds; deliberately simple. */
	if (fpm_coop_reval_stat(ZSTR_VAL(path), &st) < 0) {
		zlog(ZLOG_DEBUG, "[pool %s] revalidate: cannot stat %s (%s), not tracked",
			fpm_coop_pool_name(), ZSTR_VAL(path), strerror(errno));
		return;
	}
	rec = pemalloc(sizeof(*rec), 1);
	fpm_coop_reval_rec_fill(rec, &st);
	key = zend_string_init(ZSTR_VAL(path), ZSTR_LEN(path), 1);
	zend_hash_add_new_ptr(&fpm_coop_reval_files, key, rec);
	zend_string_release(key);	/* the table keeps its own reference */
	zlog(ZLOG_DEBUG, "[pool %s] revalidate: tracking %s (%u files)",
		fpm_coop_pool_name(), ZSTR_VAL(path), zend_hash_num_elements(&fpm_coop_reval_files));
}
/* }}} */

/* zend_compile_file hook: chain behind the original (OPcache keeps its own even
 * when disabled), do not replace it. Skip the entry script — see .h.
 * Read primary_script BEFORE compilation: php_stream_open_for_zend_ex
 * (main/main.c) clears the entire zend_file_handle while opening the stream and
 * the flag is lost — for the same reason OPcache checks it before compilation. */
static zend_op_array *fpm_coop_reval_compile_file(zend_file_handle *file_handle, int type) /* {{{ */
{
	bool primary = file_handle->primary_script;
	zend_op_array *op_array = fpm_coop_reval_orig_compile_file(file_handle, type);

	if (op_array && !primary && file_handle->handle.stream.handle) {
		/* The same value compile_filename() puts in EG(included_files):
		 * opened_path, or filename when it is unavailable. */
		fpm_coop_reval_remember(file_handle->opened_path ? file_handle->opened_path : file_handle->filename);
	}
	return op_array;
}
/* }}} */

void fpm_coop_reval_start(int freq_seconds) /* {{{ */
{
	if (freq_seconds <= 0 || fpm_coop_reval_on) {
		return;
	}
	zend_hash_init(&fpm_coop_reval_files, 64, NULL, fpm_coop_reval_rec_free, 1);
	fpm_coop_reval_orig_compile_file = zend_compile_file;
	zend_compile_file = fpm_coop_reval_compile_file;
	fpm_coop_reval_on = true;
}
/* }}} */

bool fpm_coop_reval_enabled(void) /* {{{ */
{
	return fpm_coop_reval_on;
}
/* }}} */

int fpm_coop_reval_sweep(const char **path, char *why, size_t why_len) /* {{{ */
{
	zend_string *key;
	struct fpm_coop_reval_rec_s *rec;

	if (!fpm_coop_reval_on) {
		return 0;
	}
	fpm_coop_reval_sweeps++;

	ZEND_HASH_FOREACH_STR_KEY_PTR(&fpm_coop_reval_files, key, rec) {
		struct stat st;
		struct fpm_coop_reval_rec_s now;

		if (fpm_coop_reval_stat(ZSTR_VAL(key), &st) < 0) {
			snprintf(why, why_len, "stat() failed: %s", strerror(errno));
			*path = ZSTR_VAL(key);
			return 1;
		}
		fpm_coop_reval_rec_fill(&now, &st);
		if (now.mtime_sec != rec->mtime_sec || now.mtime_nsec != rec->mtime_nsec) {
			snprintf(why, why_len, "mtime %ld.%09ld -> %ld.%09ld",
				(long) rec->mtime_sec, rec->mtime_nsec, (long) now.mtime_sec, now.mtime_nsec);
		} else if (now.size != rec->size) {
			snprintf(why, why_len, "size %lld -> %lld", (long long) rec->size, (long long) now.size);
		} else if (now.ino != rec->ino || now.dev != rec->dev) {
			/* rename() in place with preserved mtime and size (rsync -t, cp -p) */
			snprintf(why, why_len, "replaced, inode %llu -> %llu", (unsigned long long) rec->ino, (unsigned long long) now.ino);
		} else {
			continue;
		}
		*path = ZSTR_VAL(key);
		return 1;
	} ZEND_HASH_FOREACH_END();

	return 0;
}
/* }}} */

void fpm_coop_reval_stats(unsigned *files, unsigned *sweeps, unsigned *stat_calls) /* {{{ */
{
	*files = fpm_coop_reval_on ? zend_hash_num_elements(&fpm_coop_reval_files) : 0;
	*sweeps = fpm_coop_reval_sweeps;
	*stat_calls = fpm_coop_reval_stat_calls;
}
/* }}} */
