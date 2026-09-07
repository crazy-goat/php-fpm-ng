PHP_ARG_ENABLE([fpmng-metrics],
  [whether to enable fpm-ng application metrics],
  [AS_HELP_STRING([--enable-fpmng-metrics],
    [Enable fpmng_metrics: application metrics from PHP (fpm_metric_*,
     NOTES 3k). Under fpm-ng the store lives in shared memory and is
     exposed by pool.type = status on /metrics; elsewhere (CLI) it is
     process-local and fpm_metric_render() returns the text])],
  [yes])

dnl --disable-all forces "no" through PHP_ENABLE_ALL (see PHP_REAL_ARG_ENABLE
dnl in build/php.m4), and we piggyback on --enable-fpmng — that is why the
dnl forcing happens HERE, after PHP_ARG_ENABLE, not in the SAPI's config.m4.
dnl SAPI stubs expand before ext ones (configure.ac: config-stubs sapi at line
dnl 289, ext at 1097), so $PHP_FPMNG is already settled. The SAPI code
dnl (fpm_metrics.c, fpm_pool_status.c) calls this extension's symbols, so under
dnl --enable-fpmng there is no choice — the extension is always built (short of
dnl disabling the whole fpm-ng).
if test "$PHP_FPMNG" != "no" && test "$PHP_FPMNG_METRICS" = "no"; then
  AC_MSG_NOTICE([fpmng_metrics forced on: required by --enable-fpmng (NOTES 3k)])
  PHP_FPMNG_METRICS=yes
fi

if test "$PHP_FPMNG_METRICS" != "no"; then
  dnl The ext/ directory is discovered by the same glob as sapi/ (NOTES 3k):
  dnl build/prepare.sh copies this directory into the php-src tree.
  PHP_NEW_EXTENSION([fpmng_metrics],
    [fpmng_metrics.c],
    [$ext_shared])
fi
