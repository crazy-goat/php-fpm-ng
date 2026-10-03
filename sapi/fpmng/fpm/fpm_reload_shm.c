/* fpm-ng: issue #537 -- see fpm_reload_shm.h for the design. */

#include "fpm_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fpm_reload_shm.h"
#include "fpm_children.h"
#include "fpm_conf.h"
#include "fpm_scoreboard.h"
#include "fpm_shm.h"
#include "fpm_worker_pool.h"
#include "zlog.h"

/* Records separated by ';', fields by ':' (pool names containing ':' or ';'
 * are never spared, see fpm_reload_selective_name_ok()):
 *   S:<pool>:<fd>:<size>                    scoreboard of a spared pool
 *   M:<fd>:<size>:<slots>:<limit>           the metrics region
 *   B:<pool>:<base>:<count>                 a spared pool's metrics slot range
 *   X:<pool>:<base>:<count>                 any pool's metrics slot range in the
 *                                           old generation (spared or not): the
 *                                           ones without a B record are reserved,
 *                                           a #329 survivor may still write there */
#define FPM_RELOAD_SHM_ENV "FPMNG_SELECTIVE_RELOAD_SHM"

struct fpm_reload_shm_inh_s {
	struct fpm_reload_shm_inh_s *next;
	char kind;
	char *name;
	int fd;
	size_t size;
	uint32_t a, b;
	int claimed;
};

static struct fpm_reload_shm_inh_s *inherited = NULL;
static int inherited_loaded = 0;

/* This generation's own memfd-backed scoreboards, by pool name. */
struct fpm_reload_shm_sb_s {
	struct fpm_reload_shm_sb_s *next;
	char *name;
	int fd;
	size_t size;
};

static struct fpm_reload_shm_sb_s *own_sb = NULL;
static int own_mx_fd = -1;
static size_t own_mx_size = 0;
static uint32_t own_mx_slots = 0, own_mx_limit = 0;
static int mx_recorded = 0;

void *fpm_reload_shm_alloc(size_t size, int *fd_out) /* {{{ */
{
#if defined(__linux__) && defined(MFD_CLOEXEC)
	void *mem;
	int fd = memfd_create("fpmng-shm", MFD_CLOEXEC);

	if (fd < 0) {
		zlog(ZLOG_WARNING, "issue #537: memfd_create() failed (%s); shared memory stays "
						   "anonymous and a pool spared by a selective reload will not keep its "
						   "status and metrics",
				strerror(errno));
		return NULL;
	}
	if (ftruncate(fd, (off_t) size) != 0) {
		zlog(ZLOG_WARNING, "issue #537: ftruncate(%zu) of a shared-memory fd failed (%s)", size, strerror(errno));
		close(fd);
		return NULL;
	}
	mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) {
		zlog(ZLOG_WARNING, "issue #537: mmap of a shared-memory fd failed (%s)", strerror(errno));
		close(fd);
		return NULL;
	}
	*fd_out = fd;
	return mem;
#else
	(void) size;
	(void) fd_out;
	return NULL;
#endif
}
/* }}} */

static void fpm_reload_shm_set_cloexec(int fd, int on) /* {{{ */
{
	int flags = fcntl(fd, F_GETFD);

	if (flags < 0) {
		return;
	}
	fcntl(fd, F_SETFD, on ? (flags | FD_CLOEXEC) : (flags & ~FD_CLOEXEC));
}
/* }}} */

static void fpm_reload_shm_add_inherited(char kind, const char *name, int fd, size_t size, uint32_t a, uint32_t b) /* {{{ */
{
	struct fpm_reload_shm_inh_s *r = calloc(1, sizeof(*r));

	if (!r) {
		return;
	}
	r->kind = kind;
	r->name = name ? strdup(name) : NULL;
	r->fd = fd;
	r->size = size;
	r->a = a;
	r->b = b;
	r->next = inherited;
	inherited = r;
}
/* }}} */

/* Parses and then removes FPM_RELOAD_SHM_ENV: the next exec must only see what
 * the next reload records. */
static void fpm_reload_shm_load(void) /* {{{ */
{
	const char *env;
	char *copy, *rec, *save = NULL;

	if (inherited_loaded) {
		return;
	}
	inherited_loaded = 1;

	env = getenv(FPM_RELOAD_SHM_ENV);
	if (!env || !*env) {
		return;
	}
	copy = strdup(env);
	unsetenv(FPM_RELOAD_SHM_ENV);
	if (!copy) {
		return;
	}

	for (rec = strtok_r(copy, ";", &save); rec; rec = strtok_r(NULL, ";", &save)) {
		char name[256];
		unsigned long long size;
		unsigned a, b;
		int fd;

		if (sscanf(rec, "S:%255[^:]:%d:%llu", name, &fd, &size) == 3) {
			fpm_reload_shm_add_inherited('S', name, fd, (size_t) size, 0, 0);
		} else if (sscanf(rec, "M:%d:%llu:%u:%u", &fd, &size, &a, &b) == 4) {
			fpm_reload_shm_add_inherited('M', NULL, fd, (size_t) size, a, b);
		} else if (sscanf(rec, "B:%255[^:]:%u:%u", name, &a, &b) == 3) {
			fpm_reload_shm_add_inherited('B', name, -1, 0, a, b);
		} else if (sscanf(rec, "X:%255[^:]:%u:%u", name, &a, &b) == 3) {
			fpm_reload_shm_add_inherited('X', name, -1, 0, a, b);
		}
	}
	free(copy);
}
/* }}} */

static struct fpm_reload_shm_inh_s *fpm_reload_shm_find(char kind, const char *name) /* {{{ */
{
	struct fpm_reload_shm_inh_s *r;

	for (r = inherited; r; r = r->next) {
		if (r->kind == kind && (!name || (r->name && strcmp(r->name, name) == 0))) {
			return r;
		}
	}
	return NULL;
}
/* }}} */

static int fpm_reload_shm_fd_size_ok(int fd, size_t size) /* {{{ */
{
	struct stat st;

	return fd >= 0 && fstat(fd, &st) == 0 && (size_t) st.st_size >= size;
}
/* }}} */

static void fpm_reload_shm_register_sb(const char *name, int fd, size_t size) /* {{{ */
{
	struct fpm_reload_shm_sb_s *s = calloc(1, sizeof(*s));

	if (!s) {
		return;
	}
	s->name = strdup(name);
	s->fd = fd;
	s->size = size;
	s->next = own_sb;
	own_sb = s;
}
/* }}} */

int fpm_reload_shm_scoreboards(void) /* {{{ */
{
	struct fpm_worker_pool_s *wp;
	struct fpm_reload_shm_inh_s *r;

	fpm_reload_shm_load();

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		const char *name = wp->config->name;
		size_t size = sizeof(struct fpm_scoreboard_s) + sizeof(struct fpm_scoreboard_proc_s) * (size_t) wp->config->pm_max_children;
		struct fpm_scoreboard_s *fresh = wp->scoreboard;
		struct fpm_reload_shm_inh_s *rec = fpm_reload_shm_find('S', name);
		void *mem = NULL;
		int fd = -1;

		if (rec && rec->size == size && fpm_reload_shm_fd_size_ok(rec->fd, size)) {
			mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, rec->fd, 0);
			if (mem != MAP_FAILED && ((struct fpm_scoreboard_s *) mem)->nprocs == (unsigned) wp->config->pm_max_children && strncmp(((struct fpm_scoreboard_s *) mem)->pool, name, sizeof(((struct fpm_scoreboard_s *) mem)->pool) - 1) == 0) {
				fd = rec->fd;
				rec->claimed = 1;
				fpm_reload_shm_set_cloexec(fd, 1);
				fpm_shm_free(fresh, size);
				wp->scoreboard = mem;
				fpm_reload_shm_register_sb(name, fd, size);
				continue;
			}
			if (mem != MAP_FAILED) {
				munmap(mem, size);
			}
			zlog(ZLOG_WARNING, "[pool %s] issue #537: the scoreboard carried over by a selective "
							   "reload does not match this pool; starting with an empty one",
					name);
		}

		if (!fpm_global_config.reload_selective) {
			continue;
		}

		mem = fpm_reload_shm_alloc(size, &fd);
		if (!mem) {
			continue; /* stays anonymous, warned above */
		}
		memcpy(mem, fresh, size);
		fpm_shm_free(fresh, size);
		wp->scoreboard = mem;
		fpm_reload_shm_register_sb(name, fd, size);
	}

	/* upstream's scoreboard init links a pool to the shared pool's scoreboard
	 * by pointer; both may have moved */
	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		if (wp->shared && wp->scoreboard && wp->shared->scoreboard) {
			wp->scoreboard->shared = wp->shared->scoreboard;
		}
	}

	for (r = inherited; r; r = r->next) {
		if (r->kind == 'S' && !r->claimed && r->fd >= 0) {
			close(r->fd);
			r->fd = -1;
		}
	}
	return 0;
}
/* }}} */

int fpm_reload_shm_inherited_metrics(int *fd, size_t *size, uint32_t *slots, uint32_t *limit) /* {{{ */
{
	struct fpm_reload_shm_inh_s *r;

	fpm_reload_shm_load();
	r = fpm_reload_shm_find('M', NULL);
	if (!r || !fpm_reload_shm_fd_size_ok(r->fd, r->size)) {
		return 0;
	}
	*fd = r->fd;
	*size = r->size;
	*slots = r->a;
	*limit = r->b;
	return 1;
}
/* }}} */

int fpm_reload_shm_inherited_range(const char *name, uint32_t *base, uint32_t *count) /* {{{ */
{
	struct fpm_reload_shm_inh_s *r;

	fpm_reload_shm_load();
	r = fpm_reload_shm_find('B', name);
	if (!r) {
		return 0;
	}
	*base = r->a;
	*count = r->b;
	return 1;
}
/* }}} */

void fpm_reload_shm_foreach_unspared_range(void (*cb)(uint32_t base, uint32_t count)) /* {{{ */
{
	struct fpm_reload_shm_inh_s *r;

	fpm_reload_shm_load();
	for (r = inherited; r; r = r->next) {
		if (r->kind == 'X' && r->name && !fpm_reload_shm_find('B', r->name)) {
			cb(r->a, r->b);
		}
	}
}
/* }}} */

void fpm_reload_shm_metrics_done(int kept) /* {{{ */
{
	struct fpm_reload_shm_inh_s *r = fpm_reload_shm_find('M', NULL);

	if (!r || r->fd < 0) {
		return;
	}
	if (kept) {
		fpm_reload_shm_set_cloexec(r->fd, 1);
	} else {
		close(r->fd);
	}
	r->fd = -1;
}
/* }}} */

void fpm_reload_shm_register_metrics(int fd, size_t size, uint32_t slots, uint32_t limit) /* {{{ */
{
	own_mx_fd = fd;
	own_mx_size = size;
	own_mx_slots = slots;
	own_mx_limit = limit;
}
/* }}} */

static void fpm_reload_shm_append_env(const char *record) /* {{{ */
{
	const char *existing = getenv(FPM_RELOAD_SHM_ENV);
	size_t len = (existing ? strlen(existing) : 0) + strlen(record) + 2;
	char *next = malloc(len);

	if (!next) {
		return;
	}
	if (existing && *existing) {
		snprintf(next, len, "%s;%s", existing, record);
	} else {
		snprintf(next, len, "%s", record);
	}
	setenv(FPM_RELOAD_SHM_ENV, next, 1);
	free(next);
}
/* }}} */

/* Defined in fpm_metrics.c. */
int fpm_metrics_pool_range(const struct fpm_worker_pool_s *wp, uint32_t *base, uint32_t *count);

void fpm_reload_shm_spare_pool(struct fpm_worker_pool_s *wp) /* {{{ */
{
	const char *name = wp->config->name;
	struct fpm_reload_shm_sb_s *s;
	struct fpm_worker_pool_s *o;
	uint32_t base, count;
	char rec[512];

	for (s = own_sb; s; s = s->next) {
		if (strcmp(s->name, name) == 0) {
			fpm_reload_shm_set_cloexec(s->fd, 0);
			snprintf(rec, sizeof(rec), "S:%s:%d:%zu", name, s->fd, s->size);
			fpm_reload_shm_append_env(rec);
			break;
		}
	}

	if (own_mx_fd >= 0 && fpm_metrics_pool_range(wp, &base, &count)) {
		if (!mx_recorded) {
			mx_recorded = 1;
			fpm_reload_shm_set_cloexec(own_mx_fd, 0);
			snprintf(rec, sizeof(rec), "M:%d:%zu:%u:%u", own_mx_fd, own_mx_size, own_mx_slots, own_mx_limit);
			fpm_reload_shm_append_env(rec);
			/* Every pool's old range, so the next master can keep new ranges
			 * off the slots of pools that are about to be replaced. */
			for (o = fpm_worker_all_pools; o; o = o->next) {
				uint32_t ob, oc;

				if (!strpbrk(o->config->name, ":;") && fpm_metrics_pool_range(o, &ob, &oc)) {
					snprintf(rec, sizeof(rec), "X:%s:%u:%u", o->config->name, ob, oc);
					fpm_reload_shm_append_env(rec);
				}
			}
		}
		snprintf(rec, sizeof(rec), "B:%s:%u:%u", name, base, count);
		fpm_reload_shm_append_env(rec);
	}
}
/* }}} */

static int fpm_reload_shm_slot_of(struct fpm_worker_pool_s *wp, pid_t pid) /* {{{ */
{
	struct fpm_scoreboard_s *sb = wp->scoreboard;
	int i;

	if (!sb) {
		return -1;
	}
	for (i = 0; i < wp->config->pm_max_children; i++) {
		if (sb->procs[i].used && sb->procs[i].pid == pid) {
			return i;
		}
	}
	return -1;
}
/* }}} */

void fpm_reload_shm_prepare_adopt(struct fpm_worker_pool_s *wp, pid_t pid) /* {{{ */
{
	int own = fpm_reload_shm_slot_of(wp, pid);

	if (own < 0) {
		return;
	}
	/* fpm_scoreboard_proc_alloc() tries scoreboard->free_proc first and takes
	 * it if it is not marked used; the statistics in the slot stay. */
	wp->scoreboard->procs[own].used = 0;
	wp->scoreboard->free_proc = own;
}
/* }}} */

void fpm_reload_shm_drop_slot(struct fpm_worker_pool_s *wp, pid_t pid) /* {{{ */
{
	int i = fpm_reload_shm_slot_of(wp, pid);

	if (i >= 0) {
		memset(&wp->scoreboard->procs[i], 0, sizeof(struct fpm_scoreboard_proc_s));
		wp->scoreboard->free_proc = i;
	}
}
/* }}} */
