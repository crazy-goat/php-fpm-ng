# Example: `pool.type = supervisor`

One long-lived worker, restarted if it exits.

## Run it

```sh
# from examples/README.md "Build the binary once", then:
cp <build>/sapi/fpmng/php-fpm-ng examples/supervisor/php-fpm-ng
cd examples/supervisor
docker build -t fpmng-supervisor-example .
docker run --rm --name fpmng-supervisor-example fpmng-supervisor-example
```

## Verify

The worker is alive (heartbeat file updates every ~2s):

```sh
docker exec fpmng-supervisor-example cat /www/data/heartbeat.txt
```

Force it to exit and watch `supervisor.restart = always`
(`fpm_pool_supervisor.c:137-149`) bring it back with a new pid:

```sh
PID=$(docker exec fpmng-supervisor-example cat /www/data/heartbeat.txt | awk '{print $1}')
docker exec fpmng-supervisor-example kill -9 "$PID"
sleep 3
docker exec fpmng-supervisor-example cat /www/data/heartbeat.txt
# pid differs from $PID, timestamp is fresh -- worker restarted per
# supervisor.restart_delay = 1 (fpm-ng.conf)
```
