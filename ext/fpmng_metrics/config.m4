PHP_ARG_ENABLE([fpmng-metrics],
  [whether to enable fpm-ng application metrics],
  [AS_HELP_STRING([--enable-fpmng-metrics],
    [Enable fpmng_metrics: application metrics from PHP (fpm_metric_*,
     NOTES 3k). Under fpm-ng the store lives in shared memory and is
     exposed by pool.type = status on /metrics; elsewhere (CLI) it is
     process-local and fpm_metric_render() returns the text])],
  [yes])

dnl --disable-all wymusza "no" przez PHP_ENABLE_ALL (patrz PHP_REAL_ARG_ENABLE
dnl w build/php.m4), a my keyjemy sie na --enable-fpmng — dlatego wymuszenie
dnl idzie TUTAJ, po PHP_ARG_ENABLE, a nie w config.m4 SAPI. Stuby sapi rozwijaja
dnl sie przed ext (configure.ac: config-stubs sapi w wierszu 289, ext w 1097),
dnl wiec $PHP_FPMNG jest juz ustalone. Kod SAPI (fpm_metrics.c,
dnl fpm_pool_status.c) wolac symbole tego rozszerzenia, wiec pod --enable-fpmng
dnl nie ma wyboru — rozszerzenie idzie zawsze (poza wylaczeniem calego fpm-ng).
if test "$PHP_FPMNG" != "no" && test "$PHP_FPMNG_METRICS" = "no"; then
  AC_MSG_NOTICE([fpmng_metrics forced on: required by --enable-fpmng (NOTES 3k)])
  PHP_FPMNG_METRICS=yes
fi

if test "$PHP_FPMNG_METRICS" != "no"; then
  dnl Katalog ext/ jest wykrywany tym samym globem co sapi/ (NOTES 3k):
  dnl build/prepare.sh kopiuje ten katalog do drzewa php-src.
  PHP_NEW_EXTENSION([fpmng_metrics],
    [fpmng_metrics.c],
    [$ext_shared])
fi
