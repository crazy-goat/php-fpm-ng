PHP_ARG_ENABLE([fpmng],
  [for fpm-ng build],
  [AS_HELP_STRING([--enable-fpmng],
    [Enable building of the fpm-ng SAPI executable])],
  [no],
  [no])

dnl Configure checks.
AC_DEFUN([PHP_FPMNG_CLOCK], [
AC_CHECK_FUNCS([clock_gettime],, [
  LIBS_save=$LIBS
  AC_SEARCH_LIBS([clock_gettime], [rt], [
    ac_cv_func_clock_gettime=yes
    AC_DEFINE([HAVE_CLOCK_GETTIME], [1])
    AS_VAR_IF([ac_cv_search_clock_gettime], ["none required"],,
      [AS_VAR_APPEND([FPMNG_EXTRA_LIBS], [" $ac_cv_search_clock_gettime"])])
  ])
  LIBS=$LIBS_save
])

AS_VAR_IF([ac_cv_func_clock_gettime], [no],
  [AC_CACHE_CHECK([for clock_get_time], [php_cv_func_clock_get_time],
    [AC_RUN_IFELSE([AC_LANG_SOURCE([
      #include <mach/mach.h>
      #include <mach/clock.h>
      #include <mach/mach_error.h>

      int main(void)
      {
        kern_return_t ret; clock_serv_t aClock; mach_timespec_t aTime;
        ret = host_get_clock_service(mach_host_self(), REALTIME_CLOCK, &aClock);

        if (ret != KERN_SUCCESS) {
          return 1;
        }

        ret = clock_get_time(aClock, &aTime);
        if (ret != KERN_SUCCESS) {
          return 2;
        }

        return 0;
      }
    ])],
    [php_cv_func_clock_get_time=yes],
    [php_cv_func_clock_get_time=no],
    [php_cv_func_clock_get_time=no])])
  AS_VAR_IF([php_cv_func_clock_get_time], [yes],
    [AC_DEFINE([HAVE_CLOCK_GET_TIME], [1],
      [Define to 1 if you have the 'clock_get_time' function.])])
])])

AC_DEFUN([PHP_FPMNG_TRACE],
[AC_CACHE_CHECK([for ptrace], [php_cv_func_ptrace],
  [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([
    #include <sys/types.h>
    #include <sys/ptrace.h>
  ],
  [ptrace(0, 0, (void *) 0, 0);])],
  [AC_RUN_IFELSE([AC_LANG_SOURCE([
      #include <unistd.h>
      #include <signal.h>
      #include <sys/wait.h>
      #include <sys/types.h>
      #include <sys/ptrace.h>
      #include <errno.h>

      #if !defined(PTRACE_ATTACH) && defined(PT_ATTACH)
      #define PTRACE_ATTACH PT_ATTACH
      #endif

      #if !defined(PTRACE_DETACH) && defined(PT_DETACH)
      #define PTRACE_DETACH PT_DETACH
      #endif

      #if !defined(PTRACE_PEEKDATA) && defined(PT_READ_D)
      #define PTRACE_PEEKDATA PT_READ_D
      #endif

      int main(void)
      {
        /* copy will fail if sizeof(long) == 8 and we've got "int ptrace()" */
        long v1 = (unsigned int) -1;
        long v2;
        pid_t child;
        int status;

        if ( (child = fork()) ) { /* parent */
          int ret = 0;

          if (0 > ptrace(PTRACE_ATTACH, child, 0, 0)) {
            return 2;
          }

          waitpid(child, &status, 0);

      #ifdef PT_IO
          struct ptrace_io_desc ptio = {
            .piod_op = PIOD_READ_D,
            .piod_offs = &v1,
            .piod_addr = &v2,
            .piod_len = sizeof(v1)
          };

          if (0 > ptrace(PT_IO, child, (void *) &ptio, 0)) {
            ret = 3;
          }
      #else
          errno = 0;

          v2 = ptrace(PTRACE_PEEKDATA, child, (void *) &v1, 0);

          if (errno) {
            ret = 4;
          }
      #endif
          ptrace(PTRACE_DETACH, child, (void *) 1, 0);

          kill(child, SIGKILL);

          return ret ? ret : (v1 != v2);
        }
        else { /* child */
          sleep(10);
          return 0;
        }
      }
    ])],
    [php_cv_func_ptrace=yes],
    [php_cv_func_ptrace=no],
    [php_cv_func_ptrace=yes])],
  [php_cv_func_ptrace=no])])

AS_VAR_IF([php_cv_func_ptrace], [yes],
  [AC_DEFINE([HAVE_PTRACE], [1],
    [Define to 1 if you have the 'ptrace' function.])],
  [AC_CACHE_CHECK([for mach_vm_read], [php_cv_func_mach_vm_read],
    [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <mach/mach.h>
      #include <mach/mach_vm.h>
    ], [
      mach_vm_read(
        (vm_map_t)0,
        (mach_vm_address_t)0,
        (mach_vm_size_t)0,
        (vm_offset_t *)0,
        (mach_msg_type_number_t*)0);
    ])],
    [php_cv_func_mach_vm_read=yes],
    [php_cv_func_mach_vm_read=no])])
])

AS_VAR_IF([php_cv_func_mach_vm_read], [yes],
  [AC_DEFINE([HAVE_MACH_VM_READ], [1],
    [Define to 1 if you have the 'mach_vm_read' function.])])

AC_CACHE_CHECK([for proc mem file], [php_cv_file_proc_mem],
[AS_IF([test -r /proc/$$/mem], [proc_mem_file=mem],
  [test -r /proc/$$/as], [proc_mem_file=as],
  [proc_mem_file=])

AS_VAR_IF([proc_mem_file],,,
  [AC_RUN_IFELSE([AC_LANG_SOURCE([[
      #ifndef _GNU_SOURCE
      #define _GNU_SOURCE
      #endif
      #define _FILE_OFFSET_BITS 64
      #include <stdint.h>
      #include <unistd.h>
      #include <sys/types.h>
      #include <sys/stat.h>
      #include <fcntl.h>
      #include <stdio.h>
      int main(void)
      {
        long v1 = (unsigned int) -1, v2 = 0;
        char buf[128];
        int fd;
        sprintf(buf, "/proc/%d/$proc_mem_file", getpid());
        fd = open(buf, O_RDONLY);
        if (0 > fd) {
          return 1;
        }
        if (sizeof(long) != pread(fd, &v2, sizeof(long), (uintptr_t) &v1)) {
          close(fd);
          return 1;
        }
        close(fd);
        return v1 != v2;
      }
    ]])],
    [php_cv_file_proc_mem=$proc_mem_file],
    [php_cv_file_proc_mem=],
    [php_cv_file_proc_mem=$proc_mem_file])
  ])
])

AS_VAR_IF([php_cv_file_proc_mem],,,
  [AC_DEFINE_UNQUOTED([PROC_MEM_FILE], ["$php_cv_file_proc_mem"],
    [Define to the /proc/pid/mem interface filename value.])])

AS_IF([test "x$php_cv_func_ptrace" = xyes], [fpmng_trace_type=ptrace],
  [test -n "$php_cv_file_proc_mem"], [fpmng_trace_type=pread],
  [test "x$php_cv_func_mach_vm_read" = xyes], [fpmng_trace_type=mach],
  [fpmng_trace_type=])

AS_VAR_IF([fpmng_trace_type],,
  [AC_MSG_WARN([FPM Trace - ptrace, pread, or mach: could not be found])])
])

AC_DEFUN([PHP_FPMNG_BUILTIN_ATOMIC],
[AC_CACHE_CHECK([if compiler supports __sync_bool_compare_and_swap],
  [php_cv_have___sync_bool_compare_and_swap],
  [AC_LINK_IFELSE([AC_LANG_PROGRAM([], [
    int variable = 1;
    return (__sync_bool_compare_and_swap(&variable, 1, 2)
           && __sync_add_and_fetch(&variable, 1)) ? 1 : 0;
  ])],
  [php_cv_have___sync_bool_compare_and_swap=yes],
  [php_cv_have___sync_bool_compare_and_swap=no])])
AS_VAR_IF([php_cv_have___sync_bool_compare_and_swap], [yes],
  [AC_DEFINE([HAVE_BUILTIN_ATOMIC], [1],
    [Define to 1 if compiler supports __sync_bool_compare_and_swap() a.o.])])
])

AC_DEFUN([PHP_FPMNG_LQ],
[AC_CACHE_CHECK([for TCP_INFO], [php_cv_have_TCP_INFO],
  [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <netinet/tcp.h>], [
    struct tcp_info ti;
    int x = TCP_INFO;
    (void)ti;
    (void)x;
  ])],
  [php_cv_have_TCP_INFO=yes],
  [php_cv_have_TCP_INFO=no])])
AS_VAR_IF([php_cv_have_TCP_INFO], [yes],
  [AC_DEFINE([HAVE_LQ_TCP_INFO], [1], [Define to 1 if you have 'TCP_INFO'.])])

AC_CACHE_CHECK([for TCP_CONNECTION_INFO], [php_cv_have_TCP_CONNECTION_INFO],
  [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <netinet/tcp.h>], [
    struct tcp_connection_info ti;
    int x = TCP_CONNECTION_INFO;
    (void)ti;
    (void)x;
  ])],
  [php_cv_have_TCP_CONNECTION_INFO=yes],
  [php_cv_have_TCP_CONNECTION_INFO=no])])
AS_VAR_IF([php_cv_have_TCP_CONNECTION_INFO], [yes],
  [AC_DEFINE([HAVE_LQ_TCP_CONNECTION_INFO], [1],
    [Define to 1 if you have 'TCP_CONNECTION_INFO'.])])

AC_CACHE_CHECK([for SO_LISTENQLEN], [php_cv_have_SO_LISTENQLEN],
  [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <sys/socket.h>], [
    int x = SO_LISTENQLIMIT;
    int y = SO_LISTENQLEN;
    (void)x;
    (void)y;
  ])],
  [php_cv_have_SO_LISTENQLEN=yes],
  [php_cv_have_SO_LISTENQLEN=no])])
AS_VAR_IF([php_cv_have_SO_LISTENQLEN], [yes],
  [AC_DEFINE([HAVE_LQ_SO_LISTENQ], [1],
    [Define to 1 if you have 'SO_LISTENQ*'.])])
])

AC_DEFUN([PHP_FPMNG_KQUEUE],
[AC_CACHE_CHECK([for kqueue],
  [php_cv_have_kqueue],
  [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([dnl
    #include <sys/types.h>
    #include <sys/event.h>
    #include <sys/time.h>
  ], [dnl
    int kfd;
    struct kevent k;
    kfd = kqueue();
    EV_SET(&k, 0, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    (void)kfd;
  ])],
  [php_cv_have_kqueue=yes],
  [php_cv_have_kqueue=no])])
AS_VAR_IF([php_cv_have_kqueue], [yes],
  [AC_DEFINE([HAVE_KQUEUE], [1],
    [Define to 1 if system has a working 'kqueue' function.])])
])

AC_DEFUN([PHP_FPMNG_EPOLL],
[AC_CACHE_CHECK([for epoll],
  [php_cv_have_epoll],
  [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <sys/epoll.h>], [dnl
    int epollfd;
    struct epoll_event e;

    epollfd = epoll_create(1);
    if (epollfd < 0) {
      return 1;
    }

    e.events = EPOLLIN | EPOLLET;
    e.data.fd = 0;

    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, 0, &e) == -1) {
      return 1;
    }

    e.events = 0;
    if (epoll_wait(epollfd, &e, 1, 1) < 0) {
      return 1;
    }
  ])],
  [php_cv_have_epoll=yes],
  [php_cv_have_epoll=no])])
AS_VAR_IF([php_cv_have_epoll], [yes],
  [AC_DEFINE([HAVE_EPOLL], [1], [Define to 1 if system has a working epoll.])])
])

if test "$PHP_FPMNG" != "no"; then
  PHP_ADD_INCLUDE([$abs_srcdir/ext/fpmng_metrics])

  PHP_FPMNG_CLOCK
  PHP_FPMNG_KQUEUE
  PHP_FPMNG_EPOLL
  PHP_FPMNG_TRACE
  PHP_FPMNG_BUILTIN_ATOMIC
  PHP_FPMNG_LQ

  AC_CHECK_FUNCS([clearenv setproctitle setproctitle_fast])
  dnl fpm-ng: accept4(SOCK_CLOEXEC) used in main/fastcgi.c (patches/0003).
  dnl Upstream checks for it only in ext/sockets/config.m4; without
  dnl HAVE_ACCEPT4 the old path compiles.
  AC_CHECK_FUNCS([accept4])

  AC_CHECK_HEADER([priv.h], [AC_CHECK_FUNCS([setpflags])])
  AC_CHECK_HEADER([sys/times.h], [AC_CHECK_FUNCS([times])])

  PHP_ARG_WITH([fpmng-user],,
    [AS_HELP_STRING([[--with-fpmng-user[=USER]]],
      [Set the user for php-fpm to run as. (default: nobody)])],
    [nobody],
    [no])

  PHP_ARG_WITH([fpmng-group],,
    [AS_HELP_STRING([[--with-fpmng-group[=GRP]]],
      [Set the group for php-fpm to run as. For a system user, this should
      usually be set to match the fpm username (default: nobody)])],
    [nobody],
    [no])

  PHP_ARG_WITH([fpmng-systemd],
    [whether to enable systemd integration in PHP-FPM],
    [AS_HELP_STRING([--with-fpmng-systemd],
      [Activate systemd integration])],
    [no],
    [no])

  PHP_ARG_WITH([fpmng-acl],
    [whether to use Access Control Lists (ACL) in PHP-FPM],
    [AS_HELP_STRING([--with-fpmng-acl],
      [Use POSIX Access Control Lists])],
    [no],
    [no])

  PHP_ARG_WITH([fpmng-apparmor],
    [whether to enable AppArmor confinement in PHP-FPM],
    [AS_HELP_STRING([--with-fpmng-apparmor],
      [Support AppArmor confinement through libapparmor])],
    [no],
    [no])

  PHP_ARG_WITH([fpmng-selinux],
    [whether to enable SELinux support in PHP-FPM],
    [AS_HELP_STRING([--with-fpmng-selinux],
      [Support SELinux policy library])],
    [no],
    [no])

  AS_VAR_IF([PHP_FPMNG_SYSTEMD], [no], [php_fpm_systemd=simple], [
    PKG_CHECK_MODULES([SYSTEMD], [libsystemd >= 209])

    AC_DEFINE([HAVE_SYSTEMD], [1],
      [Define to 1 if FPM has systemd integration.])
    PHP_FPMNG_SD_FILES="fpm/fpm_systemd.c"
    PHP_EVAL_LIBLINE([$SYSTEMD_LIBS], [FPMNG_EXTRA_LIBS], [yes])
    PHP_EVAL_INCLINE([$SYSTEMD_CFLAGS])

    php_fpm_systemd=notify

    dnl Sanity check.
    CFLAGS_save=$CFLAGS
    CFLAGS="$INCLUDES $CFLAGS"
    AC_CHECK_HEADER([systemd/sd-daemon.h],,
      [AC_MSG_FAILURE([Required <systemd/sd-daemon.h> header file not found.])])
    CFLAGS=$CFLAGS_save
  ])

  AC_SUBST([php_fpm_systemd])

  dnl This SAPI is built against a php-src that build/prepare.sh has patched,
  dnl so patches/0006 (persistent Zend signal handlers) is present by
  dnl construction -- the `patches` CI job fails if it is not. The define
  dnl exists so that the ONE other way this SAPI can be built,
  dnl build/libphp-build.sh (a distribution libphp, which carries no patch of
  dnl ours inside Zend/), has something to leave unset. It gates the call in
  dnl fpm.c and the pool types that depend on it
  dnl (fpm_pool_type_check_build_support). See issue #214.
  AC_DEFINE([HAVE_FPMNG_PERSISTENT_SIGNALS], [1],
    [Define to 1 if Zend carries patches/0006 (persistent signal handlers).])

  dnl The HTTP gateway is not optional in fpm-ng, it is the point of it.
  PKG_CHECK_MODULES([LIBEVENT], [libevent >= 2.1])
  AC_DEFINE([HAVE_FPM_HTTP], [1],
    [Define to 1 if fpm-ng has the plain HTTP gateway.])
  PHP_EVAL_LIBLINE([$LIBEVENT_LIBS], [FPMNG_EXTRA_LIBS], [yes])
  PHP_EVAL_INCLINE([$LIBEVENT_CFLAGS])

  dnl TLS termination (http.tls_cert/http.tls_key), for both the `http`
  dnl gateway and pool.type = http-direct, is opt-in and OFF by default
  dnl (issue #280, part of #279): this code is beta, unaudited and
  dnl network-facing, and until this flag existed the answer to "does this
  dnl binary terminate TLS?" was "whatever was installed on the build host",
  dnl which is not a default anybody chose.
  dnl
  dnl Without the flag nothing below runs, nothing links against OpenSSL, the
  dnl fpm_tls_*.c sources are not compiled at all (build/prepare.sh puts them
  dnl in their own list), and http.tls_cert is refused at config-validation
  dnl time with a message naming this flag -- see fpm_http_validate_pool() in
  dnl fpm_http.c and the stubs in fpm_http_direct_tls.c.
  dnl
  dnl With the flag the library probe is a hard error rather than the old
  dnl AC_MSG_WARN: the operator asked for TLS, so a missing libevent_openssl
  dnl must stop the build instead of quietly producing the binary they were
  dnl trying not to build.
  PHP_ARG_ENABLE([fpmng-tls],
    [whether to build TLS termination in fpm-ng],
    [AS_HELP_STRING([--enable-fpmng-tls],
      [Build fpm-ng with TLS termination for the HTTP gateway and http-direct (BETA, unaudited)])],
    [no],
    [no])

  PHP_FPMNG_TLS_FILES=""
  AS_VAR_IF([PHP_FPMNG_TLS], [no],, [
    fpmng_http_tls=no
    PKG_CHECK_MODULES([LIBEVENT_OPENSSL], [libevent_openssl >= 2.1], [
      PKG_CHECK_MODULES([FPMNG_OPENSSL], [openssl >= 1.1.1], [
        fpmng_http_tls=yes
      ], [
        LIBS_save=$LIBS
        AC_CHECK_LIB([ssl], [SSL_CTX_new], [
          AC_CHECK_HEADER([openssl/ssl.h], [
            FPMNG_OPENSSL_LIBS="-lssl -lcrypto"
            fpmng_http_tls=yes
          ])
        ])
        LIBS=$LIBS_save
      ])
    ], [
      AC_MSG_ERROR([--enable-fpmng-tls needs libevent_openssl >= 2.1 (Debian/Ubuntu: libevent-openssl-2.1-7 + libevent-dev, Alpine: libevent-dev; both also need OpenSSL's development package). Build without --enable-fpmng-tls for a binary with no TLS support.])
    ])
    AS_VAR_IF([fpmng_http_tls], [yes],, [
      AC_MSG_ERROR([--enable-fpmng-tls needs OpenSSL >= 1.1.1 development files (Debian/Ubuntu: libssl-dev, Alpine: openssl-dev). libevent_openssl was found, OpenSSL itself was not.])
    ])
    AC_DEFINE([HAVE_FPM_HTTP_TLS], [1],
      [Define to 1 if fpm-ng terminates TLS (--enable-fpmng-tls).])
    PHP_FPMNG_TLS_FILES="@FPMNG_TLS_SOURCES@"
    PHP_EVAL_LIBLINE([$LIBEVENT_OPENSSL_LIBS], [FPMNG_EXTRA_LIBS], [yes])
    PHP_EVAL_LIBLINE([$FPMNG_OPENSSL_LIBS], [FPMNG_EXTRA_LIBS], [yes])
    PHP_EVAL_INCLINE([$LIBEVENT_OPENSSL_CFLAGS])
    PHP_EVAL_INCLINE([$FPMNG_OPENSSL_CFLAGS])
  ])

  dnl ACME certificate issuance (sapi/fpmng/acme/*.php plus the shared
  dnl challenge state in fpm_acme_challenge.c) is opt-in and OFF by default
  dnl for the same reasons as TLS above, with one of its own: this is the
  dnl only facility in the SAPI that reaches out to a third party -- a
  dnl certificate authority -- on the operator's behalf, from a process
  dnl inside their server. Issue #281, part of #279.
  dnl
  dnl It requires --enable-fpmng-tls, and that is an error rather than a
  dnl warning: ACME obtains a certificate, and a binary that cannot terminate
  dnl TLS has nothing to serve it with, so the combination would build
  dnl cleanly and do nothing.
  PHP_ARG_ENABLE([fpmng-acme],
    [whether to build ACME certificate issuance in fpm-ng],
    [AS_HELP_STRING([--enable-fpmng-acme],
      [Build fpm-ng with ACME certificate issuance (BETA, unaudited; requires --enable-fpmng-tls)])],
    [no],
    [no])

  PHP_FPMNG_ACME_FILES=""
  AS_VAR_IF([PHP_FPMNG_ACME], [no],, [
    AS_VAR_IF([PHP_FPMNG_TLS], [no], [
      AC_MSG_ERROR([--enable-fpmng-acme requires --enable-fpmng-tls: ACME obtains a certificate, and a binary built without TLS termination has nothing to serve it with. Add --enable-fpmng-tls, or drop --enable-fpmng-acme.])
    ])
    AC_DEFINE([HAVE_FPMNG_ACME], [1],
      [Define to 1 if fpm-ng carries ACME certificate issuance (--enable-fpmng-acme).])
    PHP_FPMNG_ACME_FILES="@FPMNG_ACME_SOURCES@"
  ])

  AS_VAR_IF([PHP_FPMNG_ACL], [no],, [
    AC_CHECK_HEADERS([sys/acl.h])

    dnl *BSD has acl_* built into libc, macOS doesn't have user/group support.
    LIBS_save=$LIBS
    AC_SEARCH_LIBS([acl_free], [acl],
    [AC_CACHE_CHECK([for ACL user/group permissions support],
      [php_cv_lib_acl_user_group],
      [AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <sys/acl.h>], [
        acl_t acl;
        acl_entry_t user, group;
        acl = acl_init(1);
        acl_create_entry(&acl, &user);
        acl_set_tag_type(user, ACL_USER);
        acl_create_entry(&acl, &group);
        acl_set_tag_type(user, ACL_GROUP);
        acl_free(acl);
      ])],
      [php_cv_lib_acl_user_group=yes],
      [php_cv_lib_acl_user_group=no])])
      AS_VAR_IF([php_cv_lib_acl_user_group], [yes], [
        AC_DEFINE([HAVE_FPM_ACL], [1],
          [Define to 1 if PHP-FPM has ACL support.])
        AS_VAR_IF([ac_cv_search_acl_free], ["none required"],,
          [AS_VAR_APPEND([FPMNG_EXTRA_LIBS], [" $ac_cv_search_acl_free"])])
      ])
    ])
    LIBS=$LIBS_save
  ])

  AS_VAR_IF([PHP_FPMNG_APPARMOR], [no],, [
    PKG_CHECK_MODULES([APPARMOR], [libapparmor],
      [PHP_EVAL_INCLINE([$APPARMOR_CFLAGS])],
      [AC_CHECK_LIB([apparmor], [aa_change_profile],
        [APPARMOR_LIBS=-lapparmor],
        [AC_MSG_FAILURE([Required libapparmor library not found.])])])
    PHP_EVAL_LIBLINE([$APPARMOR_LIBS], [FPMNG_EXTRA_LIBS], [yes])

    dnl Sanity check.
    CFLAGS_save=$CFLAGS
    CFLAGS="$INCLUDES $CFLAGS"
    AC_CHECK_HEADER([sys/apparmor.h],
      [AC_DEFINE([HAVE_APPARMOR], [1],
        [Define to 1 if AppArmor confinement is available for PHP-FPM.])],
      [AC_MSG_FAILURE([Required <sys/apparmor.h> header file not found.])])
    CFLAGS=$CFLAGS_save
  ])

  AS_VAR_IF([PHP_FPMNG_SELINUX], [no],, [
    PKG_CHECK_MODULES([SELINUX], [libselinux],
      [PHP_EVAL_INCLINE([$SELINUX_CFLAGS])],
      [AC_CHECK_LIB([selinux], [security_setenforce],
        [SELINUX_LIBS=-lselinux],
        [AC_MSG_FAILURE([Required SELinux library not found.])])])
    PHP_EVAL_LIBLINE([$SELINUX_LIBS], [FPMNG_EXTRA_LIBS], [yes])

    dnl Sanity check.
    CFLAGS_save=$CFLAGS
    CFLAGS="$INCLUDES $CFLAGS"
    AC_CHECK_HEADER([selinux/selinux.h],
      [AC_DEFINE([HAVE_SELINUX], [1],
        [Define to 1 if SELinux is available in PHP-FPM.])],
      [AC_MSG_FAILURE([Required <selinux/selinux.h> header file not found.])])
    CFLAGS=$CFLAGS_save
  ])

  if test -z "$PHP_FPMNG_USER" || test "$PHP_FPMNG_USER" = "yes" || test "$PHP_FPMNG_USER" = "no"; then
    php_fpm_user=nobody
  else
    php_fpm_user=$PHP_FPMNG_USER
  fi

  if test -z "$PHP_FPMNG_GROUP" || test "$PHP_FPMNG_GROUP" = "yes" || test "$PHP_FPMNG_GROUP" = "no"; then
    php_fpm_group=nobody
  else
    php_fpm_group=$PHP_FPMNG_GROUP
  fi

  AC_SUBST([php_fpm_user])
  AC_SUBST([php_fpm_group])
  php_fpm_sysconfdir=$(eval echo $sysconfdir)
  AC_SUBST([php_fpm_sysconfdir])
  php_fpm_localstatedir=$(eval echo $localstatedir)
  AC_SUBST([php_fpm_localstatedir])
  php_fpm_prefix=$(eval echo $prefix)
  AC_SUBST([php_fpm_prefix])

  PHP_ADD_BUILD_DIR([
    sapi/fpmng/fpm
    sapi/fpmng/fpm/events
  ])
  AC_CONFIG_FILES([
    sapi/fpmng/php-fpm.conf
    sapi/fpmng/www.conf
  ])
  PHP_ADD_MAKEFILE_FRAGMENT([$abs_srcdir/sapi/fpmng/Makefile.frag])

  SAPI_FPMNG_PATH=sapi/fpmng/php-fpm-ng

  AS_VAR_IF([fpmng_trace_type],,,
    [AS_IF([test -f "$abs_srcdir/sapi/fpmng/fpm/fpm_trace_$fpmng_trace_type.c"],
      [PHP_FPMNG_TRACE_FILES="fpm/fpm_trace.c fpm/fpm_trace_$fpmng_trace_type.c"])])

  dnl The source list is injected by build/prepare.sh based on this php-src's
  dnl sapi/fpm/config.m4 — so it does not drift when upstream adds or removes
  dnl a file (e.g. events/devpoll.c).
  PHP_FPMNG_FILES="@FPMNG_SOURCES@"

  dnl Multi-request executors (pool.executor = fiber / async) are opt-in and
  dnl OFF by default, so a default build carries none of their code. Each
  dnl flag pulls in its own source list, substituted by build/prepare.sh from
  dnl the same file the base list comes from (see NOTES: the source split).
  PHP_ARG_ENABLE([fpmng-fiber],
    [whether to build the fiber-based multi-request executor in fpm-ng],
    [AS_HELP_STRING([--enable-fpmng-fiber],
      [Build fpm-ng with pool.executor = fiber support])],
    [no],
    [no])

  PHP_ARG_ENABLE([fpmng-async],
    [whether to build the async multi-request executor in fpm-ng],
    [AS_HELP_STRING([--enable-fpmng-async],
      [Build fpm-ng with pool.executor = async support])],
    [no],
    [no])

  PHP_FPMNG_FIBER_FILES=""
  AS_VAR_IF([PHP_FPMNG_FIBER], [no],, [
    AC_DEFINE([HAVE_FPMNG_FIBER], [1],
      [Define to 1 if fpm-ng has the fiber-based multi-request executor.])
    PHP_FPMNG_FIBER_FILES="@FPMNG_FIBER_SOURCES@"
  ])

  dnl Fiber non-blocking TLS (patch 0007, HAVE_FPMNG_FIBER_TLS): needs
  dnl ext/openssl compiled in AND linked into the fpmng binary. A shared
  dnl openssl.so loads at runtime, after our check, so there is nothing to
  dnl detect at build time — in that configuration the patch's ssl/tls
  dnl transports stay upstream's (blocking). Caveat: sapi/fpmng/config.m4
  dnl runs BEFORE ext/openssl/config0.m4 in configure, so
  dnl PHP_OPENSSL/ext_shared do not exist yet; the raw autoconf option
  dnl variable with_openssl is set during initial option parsing and IS
  dnl available here.
  AS_VAR_IF([PHP_FPMNG_FIBER], [no],, [
    AS_CASE([$with_openssl], [no|""], [
      dnl no ext/openssl at all: TLS transports do not exist; nothing to do
    ], [shared], [
      AC_MSG_WARN([pool.executor = fiber: ext/openssl is shared, the fiber TLS interception (patch 0007) is off; https:// will block the process])
    ], [
      AC_DEFINE([HAVE_FPMNG_FIBER_TLS], [1],
        [Define to 1 if fpm-ng intercepts ssl/tls transports for the fiber executor (patch 0007).])
    ])
  ])

  PHP_FPMNG_ASYNC_FILES=""
  AS_VAR_IF([PHP_FPMNG_ASYNC], [no],, [
    AC_DEFINE([HAVE_FPMNG_ASYNC], [1],
      [Define to 1 if fpm-ng has the async multi-request executor.])
    PHP_FPMNG_ASYNC_FILES="@FPMNG_ASYNC_SOURCES@"
  ])

  PHP_SELECT_SAPI([fpmng],
    [program],
    [$PHP_FPMNG_FILES $PHP_FPMNG_TRACE_FILES $PHP_FPMNG_SD_FILES $PHP_FPMNG_FIBER_FILES $PHP_FPMNG_ASYNC_FILES $PHP_FPMNG_TLS_FILES $PHP_FPMNG_ACME_FILES],
    [-I$abs_srcdir/sapi/fpm -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1])

  AS_CASE([$host_alias],
    [*aix*], [
      BUILD_FPMNG="echo '\#! .' > php.sym && echo >>php.sym && nm -BCpg \`echo \$(PHP_GLOBAL_OBJS) \$(PHP_BINARY_OBJS) \$(PHP_FPMNG_OBJS) | sed 's/\([A-Za-z0-9_]*\)\.lo/\1.o/g'\` | \$(AWK) '{ if (((\$\$2 == \"T\") || (\$\$2 == \"D\") || (\$\$2 == \"B\")) && (substr(\$\$3,1,1) != \".\")) { print \$\$3 } }' | sort -u >> php.sym && \$(LIBTOOL) --tag=CC --mode=link \$(CC) -export-dynamic \$(CFLAGS_CLEAN) \$(EXTRA_CFLAGS) \$(EXTRA_LDFLAGS_PROGRAM) \$(LDFLAGS) -Wl,-brtl -Wl,-bE:php.sym \$(PHP_RPATHS) \$(PHP_GLOBAL_OBJS) \$(PHP_BINARY_OBJS) \$(PHP_FASTCGI_OBJS) \$(PHP_FPMNG_OBJS) \$(EXTRA_LIBS) \$(FPMNG_EXTRA_LIBS) \$(ZEND_EXTRA_LIBS) -o \$(SAPI_FPMNG_PATH)"
    ],
    [*darwin*], [
      BUILD_FPMNG="\$(CC) \$(CFLAGS_CLEAN) \$(EXTRA_CFLAGS) \$(EXTRA_LDFLAGS_PROGRAM) \$(LDFLAGS) \$(NATIVE_RPATHS) \$(PHP_GLOBAL_OBJS:.lo=.o) \$(PHP_BINARY_OBJS:.lo=.o) \$(PHP_FASTCGI_OBJS:.lo=.o) \$(PHP_FPMNG_OBJS:.lo=.o) \$(PHP_FRAMEWORKS) \$(EXTRA_LIBS) \$(FPMNG_EXTRA_LIBS) \$(ZEND_EXTRA_LIBS) -o \$(SAPI_FPMNG_PATH)"
    ], [
      BUILD_FPMNG="\$(LIBTOOL) --tag=CC --mode=link \$(CC) -export-dynamic \$(CFLAGS_CLEAN) \$(EXTRA_CFLAGS) \$(EXTRA_LDFLAGS_PROGRAM) \$(LDFLAGS) \$(PHP_RPATHS) \$(PHP_GLOBAL_OBJS:.lo=.o) \$(PHP_BINARY_OBJS:.lo=.o) \$(PHP_FASTCGI_OBJS:.lo=.o) \$(PHP_FPMNG_OBJS:.lo=.o) \$(EXTRA_LIBS) \$(FPMNG_EXTRA_LIBS) \$(ZEND_EXTRA_LIBS) -o \$(SAPI_FPMNG_PATH)"
    ])

  PHP_SUBST([SAPI_FPMNG_PATH])
  PHP_SUBST([BUILD_FPMNG])
  PHP_SUBST([FPMNG_EXTRA_LIBS])
fi
