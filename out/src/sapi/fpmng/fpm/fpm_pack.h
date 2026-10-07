/* fpm-ng: `php-fpm-ng pack`, a ready executable from a PHAR, a php.ini and an
 * fpm.conf (issue #429). Implementation and rationale in fpm_pack.c. */

#ifndef FPM_PACK_H
#define FPM_PACK_H 1

/* Entry names inside the application payload archive (kind
 * FPM_PAYLOAD_KIND_APPLICATION). Part of the on-disk contract: the runtime
 * issues (#428, #430) look the three inputs up by these names. */
#define FPM_PACK_ENTRY_PHAR "app.phar"
#define FPM_PACK_ENTRY_INI "php.ini"
#define FPM_PACK_ENTRY_CONF "fpm.conf"

/* argv[0] is the program, argv[1] is "pack". Returns the process exit status
 * (0 on success, 64 for a usage error, 1 for any other failure). */
int fpm_pack_main(int argc, char **argv);

#endif
