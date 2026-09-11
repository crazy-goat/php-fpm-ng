/* Declarations for the libphp build's stand-ins (issues #213, #216).
 *
 * The definitions live in fpm_libphp_compat.c, where each one carries the
 * reasoning for why it exists and why it is safe. Everything declared here is
 * defined in both builds; on the from-source path the body is empty.
 */

#ifndef FPM_LIBPHP_COMPAT_H
#define FPM_LIBPHP_COMPAT_H 1

/* Registers the PHP modules this binary carries but the loaded libphp does not
 * know about. Call once, from the master, after php_module_startup(). */
void fpmng_libphp_register_bundled_modules(void);

#endif
