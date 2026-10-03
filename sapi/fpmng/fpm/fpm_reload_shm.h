/* fpm-ng: issue #537 -- shared memory that survives a selective reload.
 *
 * A selective reload (#330) spares an unchanged pool's workers across the
 * master's execvp(). Those workers keep the shared-memory mappings they
 * inherited by fork: the pool's scoreboard and the application-metrics region.
 * Both are MAP_ANONYMOUS (fpm_shm_alloc()), so the new master cannot reach
 * them: it allocates fresh, empty ones and every spared worker keeps writing
 * into a region nobody reads (status page: 0 processes, metrics series gone).
 *
 * The mechanism chosen here is "inherit the region across exec": when
 * reload.selective is on, the master backs the scoreboards and the metrics
 * region with a memfd (close-on-exec) instead of anonymous memory. Sparing a
 * pool clears close-on-exec on that pool's scoreboard fd and on the metrics
 * fd, and records fd numbers and sizes in FPMNG_SELECTIVE_RELOAD_SHM; the new
 * master maps the very same pages again, so the spared workers and the new
 * master see one region. The rejected alternative is telling spared workers to
 * re-map: they would need the fd passed over a socket and a safe point in the
 * request loop to switch at, while inheriting needs neither.
 *
 * Without reload.selective nothing changes: the regions stay anonymous. A
 * platform without memfd_create() (the macOS from-source flow) keeps anonymous
 * memory too, and a spared pool there keeps today's behaviour. */

#ifndef FPM_RELOAD_SHM_H
#define FPM_RELOAD_SHM_H 1

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct fpm_worker_pool_s;

/* Close-on-exec memfd of `size` bytes mapped MAP_SHARED, or NULL (nothing is
 * logged beyond a warning: callers fall back to fpm_shm_alloc()). *fd_out gets
 * the descriptor. */
void *fpm_reload_shm_alloc(size_t size, int *fd_out);

/* Master, in the init chain right after fpm_scoreboard_init_main(): moves
 * every pool's scoreboard onto a memfd (reload.selective = yes), or onto the
 * memfd a selective reload carried over for it. Returns 0, or -1 only when a
 * carried-over scoreboard is unusable and cannot be replaced. */
int fpm_reload_shm_scoreboards(void);

/* What the previous generation carried over for the metrics region, if
 * anything. Returns 1 and fills the out-parameters when a usable descriptor
 * is present (the caller validates `limit` and the size against its own
 * configuration and calls fpm_reload_shm_metrics_done()); 0 otherwise. */
int fpm_reload_shm_inherited_metrics(int *fd, size_t *size, uint32_t *slots, uint32_t *limit);

/* The slot range [base, base + count) the previous generation gave pool
 * `name`. Returns 1 if a range was carried over, 0 if not. */
int fpm_reload_shm_inherited_range(const char *name, uint32_t *base, uint32_t *count);

/* Calls `cb` for the slot range of every pool of the previous generation that
 * was NOT spared. A #329 survivor of such a pool may still write to its old
 * slot for a while, so the new generation must not hand those slots to
 * another writer. */
void fpm_reload_shm_foreach_unspared_range(void (*cb)(uint32_t base, uint32_t count));

/* Called by fpm_metrics_init_main() when it has finished with the inherited
 * descriptor, whether it kept it (`kept` != 0, the fd is then registered for
 * the next reload by fpm_reload_shm_register_metrics()) or not (closed). */
void fpm_reload_shm_metrics_done(int kept);

/* The metrics region of THIS generation, so that sparing a pool can hand it
 * on. */
void fpm_reload_shm_register_metrics(int fd, size_t size, uint32_t slots, uint32_t limit);

/* Called from fpm_reload_selective_spare_pool() once the pool's children are
 * detached: marks the pool's scoreboard and the metrics region for
 * inheritance and records the pool's metrics slot range. */
void fpm_reload_shm_spare_pool(struct fpm_worker_pool_s *wp);

/* Called from fpm_reload_selective_adopt() right BEFORE fpm_children_adopt():
 * an inherited scoreboard has the worker's own slot marked used and the worker
 * writes to that one, but fpm_scoreboard_proc_alloc() would take the first
 * free slot (and find none when pm.max_children workers are all spared). Finds
 * the slot whose pid is `pid`, marks it free (its statistics stay) and points
 * the allocator at it, so adoption claims exactly that slot. A no-op when
 * there is no such slot (a freshly allocated scoreboard). */
void fpm_reload_shm_prepare_adopt(struct fpm_worker_pool_s *wp, pid_t pid);

/* Called for a spared pid that died before it could be adopted: frees the
 * scoreboard slot it held, which nothing else would ever free. */
void fpm_reload_shm_drop_slot(struct fpm_worker_pool_s *wp, pid_t pid);

#endif
