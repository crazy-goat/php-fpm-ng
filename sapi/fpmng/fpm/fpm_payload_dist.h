/* fpm-ng: the distribution payload -- the project's own PHP code, carried
 * inside the binary and reachable from configuration (issue #171).
 *
 * fpm_payload.h gives one typed blob per kind; this file is what the
 * distribution kind's blob contains and how a pool names a file in it.
 *
 * THE SPELLING: cron.script = fpmng-dist://acme/renew.php
 *
 * A URI scheme rather than a reserved directive value (`cron.script = @acme`)
 * or a second directive (`cron.embedded_script`), because the script is not the
 * only path involved: renew.php requires client.php next to it, that requires
 * jws.php, and PHP resolves those through the stream layer against the
 * including file's own path. A scheme gives every one of those includes a
 * working path for free; a reserved word would work for the directive and then
 * break at the first require. The scheme is also what makes
 * `include` of an embedded file readable in the script: the path says where the
 * code comes from.
 *
 * The archive inside the blob is deliberately minimal -- no compression, no
 * permissions, no timestamps, no directories:
 *
 *     "FPMNGAR1"  8 bytes
 *     count       u32
 *     count x   [ name length u32 ][ data size u64 ][ data offset u64 ][ name ]
 *     data...
 *
 * All offsets are from the start of the archive, all integers little-endian and
 * parsed byte by byte, like the payload records themselves. tar and phar were
 * both considered and both rejected: tar brings 512-byte blocks and metadata
 * nothing here uses, phar brings a dependency on ext/phar being built and on
 * php.ini's phar settings, for a reader that has to work before any of the
 * configuration is loaded.
 */

#ifndef FPM_PAYLOAD_DIST_H
#define FPM_PAYLOAD_DIST_H 1

#include "fpm_config.h"

#include <stdbool.h>

/* Every embedded path starts with this. */
#define FPM_PAYLOAD_DIST_SCHEME "fpmng-dist://"

/* Whether `path` names an embedded file rather than a file on disk. Answers
 * from the spelling alone, so it can be used in the master during
 * configuration validation. */
bool fpm_payload_dist_is_path(const char *path);

/* Loads the distribution payload out of the running binary and checks that
 * `path` (a fpmng-dist:// path) names a file in it. Returns 0, or -1 with `why`
 * set -- a build with no distribution payload at all, a corrupt one, or a name
 * that is not in the archive. Called in the master while validating a pool, so
 * that acceptance criterion 3 holds: a build that embedded nothing says so at
 * startup instead of failing at the first ACME order. */
int fpm_payload_dist_validate(const char *path, const char **why);

/* Registers the fpmng-dist:// stream wrapper in this process. Called once per
 * request-capable process before any script runs; a second call is a no-op.
 * Returns 0 or -1 (logged by the caller through `why`). */
int fpm_payload_dist_register(const char **why);

#endif
