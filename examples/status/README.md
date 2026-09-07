# Example: `pool.type = status`

One `status` pool paired with one `http` pool (`[app]`), so `/status` and
`/metrics` have real state to report rather than an empty pool list.

## Run it

```sh
# from examples/README.md "Build the binary once", then:
cp <build>/sapi/fpmng/php-fpm-ng examples/status/php-fpm-ng
cd examples/status
docker build -t fpmng-status-example .
docker run --rm -p 8080:8080 -p 8081:8081 fpmng-status-example
```

## Verify

Generate some traffic on `[app]`, then read it back through `[metrics]`:

```sh
curl -s http://localhost:8080/index.php >/dev/null
curl -s http://localhost:8080/index.php >/dev/null
curl -s http://localhost:8081/status
curl -s http://localhost:8081/metrics
```

`/status` (JSON) and `/metrics` (Prometheus text) are served directly by
the status pool's own process, not through PHP or FastCGI
(`fpm_pool_status.c:1-29`). Both should show the `app` pool's request count
having advanced past zero, aggregated via `fpm_scoreboard_copy()`
(`fpm_pool_status.c:196-205`) -- the status pool reads `app`'s scoreboard
from a different process, which is why it's a copy rather than a direct
read.
