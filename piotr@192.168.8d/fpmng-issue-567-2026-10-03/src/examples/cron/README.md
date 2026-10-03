# Example: `pool.type = cron`

One pool, one schedule, one script. `cron.schedule = * * * * *` (every
minute -- `fpm_cron_schedule.c:20-26,160-236`), not `@daily`, so this is
verifiable in under two minutes.

## Run it

```sh
# from examples/README.md "Build the binary once", then:
cp <build>/sapi/fpmng/php-fpm-ng examples/cron/php-fpm-ng
cd examples/cron
docker build -t fpmng-cron-example .
docker run --rm --name fpmng-cron-example fpmng-cron-example
```

## Verify

Wait up to 60s for the next minute boundary, then check the app's own
marker file and fpm's own cron.log both moved:

```sh
docker exec fpmng-cron-example cat /www/data/tick.txt
docker exec fpmng-cron-example cat /www/data/cron.log
```

`tick.txt` gets one line per run from `tick.php` itself (proves the PHP
script ran); `cron.log` gets one line per run from fpm-ng.conf's `cron.log`
directive with fpm's own start time, exit code and duration
(`fpm_pool_cron.c:306-357`). Both should show a fresh timestamp within the
last minute if you wait long enough between the two commands.
