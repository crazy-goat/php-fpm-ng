/* fpm-ng: see fpm_children_extra.h. Single-threaded master event loop only,
 * so a plain linked list needs no locking. */

#include "fpm_config.h"

#include <stdlib.h>

#include "fpm_children_extra.h"

struct fpm_children_extra_entry_s {
	struct fpm_children_extra_entry_s *next;
	pid_t pid;
	void (*on_exit)(void *arg, pid_t pid, int status);
	void *arg;
};

static struct fpm_children_extra_entry_s *entries = NULL;

void fpm_children_extra_watch(pid_t pid, void (*on_exit)(void *arg, pid_t pid, int status), void *arg) /* {{{ */
{
	struct fpm_children_extra_entry_s *e = malloc(sizeof(*e));

	if (!e) {
		return; /* best-effort: a dead gateway just will not be respawned */
	}
	e->pid = pid;
	e->on_exit = on_exit;
	e->arg = arg;
	e->next = entries;
	entries = e;
}
/* }}} */

void fpm_children_extra_forget(pid_t pid) /* {{{ */
{
	struct fpm_children_extra_entry_s **cur = &entries;

	while (*cur) {
		if ((*cur)->pid == pid) {
			struct fpm_children_extra_entry_s *dead = *cur;
			*cur = dead->next;
			free(dead);
			return;
		}
		cur = &(*cur)->next;
	}
}
/* }}} */

int fpm_children_extra_handle_exit(pid_t pid, int status) /* {{{ */
{
	struct fpm_children_extra_entry_s **cur = &entries;

	while (*cur) {
		if ((*cur)->pid == pid) {
			struct fpm_children_extra_entry_s *dead = *cur;
			void (*on_exit)(void *, pid_t, int) = dead->on_exit;
			void *arg = dead->arg;

			*cur = dead->next;
			free(dead);
			on_exit(arg, pid, status);
			return 1;
		}
		cur = &(*cur)->next;
	}
	return 0;
}
/* }}} */
