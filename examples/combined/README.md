# Example: combined (Tier 1)

One `fpm-ng.conf`, one container, one `docker run`: `http` (`[app]`),
`cron` (`[tick]`, every minute), `supervisor` (`[worker]`) and `status`
(`[metrics]`) running at the same time, in the same process.

## Run it

```sh
# from examples/README.md "Build the binary once", then:
cp <build>/sapi/fpmng/php-fpm-ng examples/combined/php-fpm-ng
cd examples/combined
docker build -t fpmng-combined-example .
docker run --rm --name fpmng-combined-example -p 8080:8080 -p 8081:8081 fpmng-combined-example
```

## Verify all four at once

The HTTP gateway answers a real request:

```sh
curl -s http://localhost:8080/index.php
```

The cron pool's scheduled script has run (wait for a minute boundary):

```sh
docker exec fpmng-combined-example cat /www/data/cron-tick.txt
docker exec fpmng-combined-example cat /www/data/cron.log
```

The supervisor's worker is alive:

```sh
docker exec fpmng-combined-example cat /www/data/worker-heartbeat.txt
```

`pool.type = status` reflects all three **at once**, in the same request:

```sh
curl -s http://localhost:8081/status
```

The JSON body includes, together: `app`'s scoreboard (idle/active workers,
requests, from `fpm_scoreboard_copy()`, `fpm_pool_status.c:196-205`),
`tick`'s `state`/`last_start`/`next_run` (`fpm_pool_cron_status()`,
`fpm_pool_cron.c:444-472`), and `worker`'s `state`/`last_start`
(`fpm_pool_supervisor_status()`, `fpm_pool_supervisor.c:438-467`, `state`
should read `running`) -- one process, one config, one running instance,
all four pool types answering simultaneously.
