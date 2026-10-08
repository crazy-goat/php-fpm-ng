/* fpm-ng: running a packed executable (issue #430). Implementation and the
 * reasons for each choice in fpm_pack_run.c; the operator-facing description
 * is docs/payload.md. */

#ifndef FPM_PACK_RUN_H
#define FPM_PACK_RUN_H 1

#include "fpm_config.h"

#include <stdbool.h>

/* How an fpm.conf (or php.ini) of a packed executable names a file inside the
 * application PHAR: `cron.script = fpmng-app://bin/console`. The runtime
 * rewrites every occurrence into `phar://<extracted archive>/bin/console`
 * before the text is used, so the rest of the master only ever sees a phar://
 * path. */
#define FPM_PACK_APP_SCHEME "fpmng-app://"

/* Called from main() before the engine starts, with the argument vector the
 * process was started with. Returns 0 when this binary carries no application
 * (argv untouched), 1 when it does and the three embedded inputs were
 * materialised and `*argv` now starts with `-c <php.ini> -y <fpm.conf>`, and -1
 * when the payload is present but cannot be used. The reason has been printed
 * on stderr by then; the caller exits. */
int fpm_pack_activate(int *argc, char ***argv);

/* Whether `path` is a path into the active application PHAR, and whether it is
 * the unresolved fpmng-app:// spelling (only meaningful in a packed executable;
 * anywhere else it is a configuration error worth naming). */
bool fpm_pack_is_app_path(const char *path);
bool fpm_pack_is_unresolved_path(const char *path);

/* Checks that `path` (an app path) names a file listed in the PHAR's manifest.
 * Returns 0, or -1 with `why` pointing at a static description. */
int fpm_pack_validate_path(const char *path, const char **why);

/* The SCRIPT_NAME / PHP_SELF to report for a front controller: the URL-style
 * "/public/index.php" for an app path, the configured string otherwise. */
const char *fpm_pack_http_script_name(const char *front_controller);

/* Run by the master once the engine is up and before any pool is validated:
 * the PHAR extension and every `extension=` the embedded php.ini asks for must
 * be loaded. Returns 0 (also when this is not a packed executable) or -1 after
 * logging the reason. */
int fpm_pack_check_runtime(void);

#endif
