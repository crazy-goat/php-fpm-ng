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
 * Across minors the skew is not fine, and the failure is not a clean error.
 * This binary carries 8.5's struct layouts and macro-expanded offsets baked
 * into every object file; handing it 8.4's structures is memory corruption,
 * which surfaces as an unrelated crash somewhere else entirely, long after the
 * cause. Within one minor, the version numbers alone are not enough either: a
 * distribution can backport a security fix that changes a struct and keep the
 * version (issue #994). The layout check below compares the sizes as well.
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
 * It only happens when a symbol was added between the two versions. The
 * dangerous case -- same symbols, changed struct layout -- relocates cleanly
 * and would run. The layout check below refuses it when the size of
 * executor_globals or compiler_globals changes; a layout change that keeps both
 * sizes is not seen (see WHY SIZES below).
 */
#include "php.h"
#include "php_main.h"
#include "zend_globals.h"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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
 * side is always the real php_version_id() of the libphp that got loaded, and
 * the real symbol sizes of that libphp.
 */
#ifndef FPMNG_BUILT_PHP_VERSION_ID
# define FPMNG_BUILT_PHP_VERSION_ID PHP_VERSION_ID
#endif
#ifndef FPMNG_BUILT_PHP_VERSION
# define FPMNG_BUILT_PHP_VERSION PHP_VERSION
#endif
#ifndef FPMNG_BUILT_EXECUTOR_GLOBALS_SIZE
# define FPMNG_BUILT_EXECUTOR_GLOBALS_SIZE sizeof(zend_executor_globals)
#endif
#ifndef FPMNG_BUILT_COMPILER_GLOBALS_SIZE
# define FPMNG_BUILT_COMPILER_GLOBALS_SIZE sizeof(zend_compiler_globals)
#endif

/* WHY SIZES. The Ubuntu backport of the CVE-2026-109618 fix keeps the version
 * 8.5.4 and adds 24 bytes to executor_globals: 1984 bytes in the base package,
 * 2008 in 8.5.4-0ubuntu1.5 (measured with the headers and libphp of both on
 * 2026-10-24). A binary built against the 1984-byte headers and run on the
 * 2008-byte libphp crashes on a request that ends in a fatal error (see
 * docs/install.md#engine-security-fixes). The dynamic loader already compares
 * this number: it prints "has different size in shared object" when the binary's
 * copy of the symbol and the libphp disagree. This check makes that a refusal.
 *
 * A size is necessary for a matching layout, not sufficient. The same backport
 * adds a bool to compiler_globals that fits a padding hole, so the size stays
 * 616 bytes on both sides and this check does not see it.
 *
 * WHY THE FILE. The size is the st_size of the libphp's dynamic symbol. There is
 * no portable way to read it from memory: dladdr1(RTLD_DL_SYMENT) is glibc only,
 * and glibc and musl relocate the dynamic section differently. The section
 * headers are not mapped at run time, but the file keeps them, and Alpine is a
 * shipped target, so the check reads the ELF file of the loaded libphp. It is
 * the file the loader mapped a moment before; a replacement during the start-up
 * itself is not guarded against.
 *
 * WHY php_version_id AND NOT executor_globals. The binary exports its own copy of
 * executor_globals (a copy relocation, linked with -Wl,-E), so dlsym() for that
 * name returns the copy at the built size, and the comparison would always pass.
 * php_version_id is defined by libphp only, so dladdr() on it names the libphp.
 */
#if UINTPTR_MAX == UINT64_MAX
# define FPMNG_ELFW(t) Elf64_##t
# define FPMNG_ELF_CLASS ELFCLASS64
#else
# define FPMNG_ELFW(t) Elf32_##t
# define FPMNG_ELF_CLASS ELFCLASS32
#endif

/* True when [off, off + size) lies inside a file of len bytes. */
static int fpmng_in_file(size_t len, uint64_t off, uint64_t size)
{
	return off <= len && size <= len - off;
}

/* st_size of the defined dynamic symbol `name` in the ELF file at `path`, or 0
 * when the file cannot be read or does not define it. Every offset read from the
 * file is checked against its length first, so a damaged file gives 0 and not a
 * fault in a constructor. */
static size_t fpmng_libphp_symbol_size(const char *path, const char *name)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	const unsigned char *map;
	const FPMNG_ELFW(Ehdr) *eh;
	const FPMNG_ELFW(Shdr) *sh;
	size_t len, name_len = strlen(name) + 1, size = 0;
	unsigned int i;

	if (fd < 0)
		return 0;
	if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(FPMNG_ELFW(Ehdr))) {
		close(fd);
		return 0;
	}
	len = (size_t)st.st_size;
	map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return 0;

	eh = (const FPMNG_ELFW(Ehdr) *)map;
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
	    eh->e_ident[EI_CLASS] != FPMNG_ELF_CLASS ||
	    eh->e_shentsize != sizeof(FPMNG_ELFW(Shdr)) ||
	    !fpmng_in_file(len, eh->e_shoff, (uint64_t)eh->e_shnum * sizeof(FPMNG_ELFW(Shdr))))
		goto out;

	sh = (const FPMNG_ELFW(Shdr) *)(map + eh->e_shoff);
	for (i = 0; i < eh->e_shnum; i++) {
		const FPMNG_ELFW(Shdr) *dynsym = &sh[i];
		const FPMNG_ELFW(Shdr) *strtab;
		const FPMNG_ELFW(Sym) *sym;
		size_t n, j;

		if (dynsym->sh_type != SHT_DYNSYM)
			continue;
		if (dynsym->sh_link >= eh->e_shnum || dynsym->sh_entsize != sizeof(FPMNG_ELFW(Sym)))
			break;
		strtab = &sh[dynsym->sh_link];
		if (!fpmng_in_file(len, dynsym->sh_offset, dynsym->sh_size) ||
		    !fpmng_in_file(len, strtab->sh_offset, strtab->sh_size))
			break;
		sym = (const FPMNG_ELFW(Sym) *)(map + dynsym->sh_offset);
		n = dynsym->sh_size / sizeof(*sym);
		for (j = 0; j < n; j++) {
			if (sym[j].st_shndx == SHN_UNDEF || sym[j].st_name >= strtab->sh_size ||
			    strtab->sh_size - sym[j].st_name < name_len)
				continue;
			if (memcmp(map + strtab->sh_offset + sym[j].st_name, name, name_len) == 0) {
				size = (size_t)sym[j].st_size;
				goto out;
			}
		}
		break;
	}

out:
	munmap((void *)map, len);
	return size;
}

/* The file of the libphp this process loaded, or NULL. */
static const char *fpmng_libphp_path(void)
{
	Dl_info info;
	void *fn = dlsym(RTLD_DEFAULT, "php_version_id");

	if (fn == NULL || dladdr(fn, &info) == 0 || info.dli_fname == NULL)
		return NULL;
	return info.dli_fname;
}

/* Refuse when the struct sizes differ, as the dynamic loader would have
 * warned. A libphp whose sizes cannot be read is refused too: the layouts
 * cannot then be compared, and a guess would let a mismatch through. */
static void fpmng_check_layout(void)
{
	const char *path = fpmng_libphp_path();
	size_t built_exec = FPMNG_BUILT_EXECUTOR_GLOBALS_SIZE;
	size_t built_comp = FPMNG_BUILT_COMPILER_GLOBALS_SIZE;
	size_t loaded_exec = path ? fpmng_libphp_symbol_size(path, "executor_globals") : 0;
	size_t loaded_comp = path ? fpmng_libphp_symbol_size(path, "compiler_globals") : 0;

	if (loaded_exec == 0 || loaded_comp == 0) {
		fprintf(stderr,
			"php-fpm-ng: FATAL: cannot read the struct sizes of the libphp it loaded (%s).\n"
			"php-fpm-ng: without them the struct layouts cannot be compared, so this binary refuses to start.\n"
			"php-fpm-ng: check that the libphp file is readable and defines executor_globals and compiler_globals.\n",
			path ? path : "unknown file");
		exit(1);
	}

	if (built_exec != loaded_exec || built_comp != loaded_comp) {
		fprintf(stderr,
			"php-fpm-ng: FATAL: this binary was built against PHP %s headers, with executor_globals "
			"of %zu bytes and compiler_globals of %zu bytes, but the libphp it loaded (PHP %s) has %zu and %zu bytes.\n"
			"php-fpm-ng: the struct layouts differ, and continuing would corrupt memory rather than fail cleanly.\n"
			"php-fpm-ng: rebuild php-fpm-ng against the headers of the installed libphp, or install the libphp "
			"this binary was built against.\n",
			FPMNG_BUILT_PHP_VERSION, built_exec, built_comp, php_version(), loaded_exec, loaded_comp);
		exit(1);
	}
}

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

	/* Same major and minor: the version numbers agree, so the sizes decide. */
	fpmng_check_layout();

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
