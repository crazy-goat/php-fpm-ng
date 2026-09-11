/* Refuse to run on a libphp that does not match the headers this binary was
 * compiled against (issue #220, part of #219).
 *
 * WHY THIS EXISTS AT ALL. On the libphp path the binary is compiled once,
 * against one distribution's php8.5-dev headers, and then loads whatever
 * libphp the user's system resolves at run time. Those two are not tied
 * together by anything:
 *
 *     $ objdump -p /usr/lib/libphp8.5.so | grep SONAME
 *       SONAME               libphp.so
 *     $ ls -l /usr/lib/libphp8.so
 *     /usr/lib/libphp8.so -> /etc/alternatives/libphp8
 *
 * The recorded SONAME is unversioned, so linking with -lphp8.5 pins the file
 * at build time and pins nothing at run time; on Debian and Ubuntu the name
 * even resolves through update-alternatives. A user does not have to do
 * anything unusual to end up here -- installing another PHP minor is enough.
 *
 * Within one minor the skew is fine: PHP keeps the ABI stable across patch
 * releases, and a user who upgrades php8.5-embed must not have their pool
 * refuse to start. Across minors it is not fine, and the failure is not a
 * clean error. This binary carries 8.5's struct layouts and macro-expanded
 * offsets baked into every object file; handing it 8.4's structures is
 * memory corruption, which surfaces as an unrelated crash somewhere else
 * entirely, long after the cause.
 *
 * WHY A CONSTRUCTOR. The check has to happen before php_module_startup(), and
 * the obvious call site -- main() in fpm_main.c -- is upstream's file, which
 * this project deliberately does not own (build/prepare.sh copies it). An ELF
 * constructor runs before main() without touching it, and it cannot be
 * forgotten by someone editing the startup path later. It also costs the
 * from-source build exactly nothing, because this translation unit is only
 * compiled by build/libphp-build.sh.
 *
 * Output goes to stderr rather than through zlog(): at constructor time FPM
 * has not read its configuration, so there is no error log to write to, and
 * stderr is where FPM's own early failures go.
 *
 * WHAT THIS DOES NOT CATCH. If the older libphp is missing a symbol the binary
 * needs, the dynamic loader fails the relocation before any constructor runs,
 * and the user sees the loader's message instead of this one. Measured on
 * Alpine, an 8.5 binary pointed at php84's libphp:
 *
 *     Error relocating .../php-fpm-ng: php_glob: symbol not found
 *
 * That is not a substitute for this check and must not be mistaken for one.
 * It only happens when a symbol was added between the two versions; the
 * dangerous case -- same symbols, changed struct layout -- relocates cleanly
 * and would run, which is exactly what this refuses.
 */
#include "php.h"
#include "php_main.h"

#include <stdio.h>
#include <stdlib.h>

/* The compiled-against side of the comparison, overridable at build time.
 *
 * It has to be overridable because neither branch below can be reached with
 * packages that actually exist. A binary cannot be *built* against 8.4 at all
 * -- sapi/fpmng includes php_glob.h, which 8.5 introduced -- and no
 * distribution offers two patch releases of one minor side by side. The
 * alternative to a seam is an untested guard, which is worse than a documented
 * one: this is the code that decides whether the process lives.
 *
 * Overriding these does not change what the check compares at run time. That
 * side is always the real php_version_id() of the libphp that got loaded.
 */
#ifndef FPMNG_BUILT_PHP_VERSION_ID
# define FPMNG_BUILT_PHP_VERSION_ID PHP_VERSION_ID
#endif
#ifndef FPMNG_BUILT_PHP_VERSION
# define FPMNG_BUILT_PHP_VERSION PHP_VERSION
#endif

/* php_version() and php_version_id() are exported from a distribution build --
 * verified with nm -D on Ubuntu's libphp8.5.so -- and declared in
 * main/php_main.h, so this asks the loaded .so what it actually is rather than
 * trusting the macro the headers baked in. */
__attribute__((constructor))
static void fpmng_libphp_abi_check(void)
{
	unsigned int loaded = php_version_id();
	unsigned int built = FPMNG_BUILT_PHP_VERSION_ID;

	/* PHP_VERSION_ID is major*10000 + minor*100 + patch. */
	if (loaded / 100 != built / 100) {
		fprintf(stderr,
			"php-fpm-ng: FATAL: this binary was built against PHP %s headers, "
			"but the libphp it loaded is PHP %s.\n"
			"php-fpm-ng: those differ in major or minor version, and the binary "
			"carries the struct layouts of the version it was built against, so "
			"continuing would corrupt memory rather than fail cleanly.\n"
			"php-fpm-ng: install the embed package matching PHP %d.%d, or use a "
			"build from source.\n",
			FPMNG_BUILT_PHP_VERSION, php_version(),
			built / 10000, (built / 100) % 100);
		exit(1);
	}

	if (loaded != built) {
		/* Supported, and said out loud exactly once -- in the master, before
		 * any child exists, so it cannot repeat per request or per child. When
		 * a bug report arrives, the version pair is already in the log instead
		 * of being the first thing a maintainer has to ask for. */
		fprintf(stderr,
			"php-fpm-ng: notice: built against PHP %s, running on libphp %s "
			"(patch-level difference, supported)\n",
			FPMNG_BUILT_PHP_VERSION, php_version());
	}
}
