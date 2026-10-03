# Example: the operator endpoint (`operator.status_path`, `operator.metrics_path`)

One FastCGI pool (`[app]`) behind a gateway, with the operator endpoint
switched on for it, so `/status` and `/metrics` have real state to report
rather than an empty pool list. This replaces `pool.type = status`, which was
removed in #278 (see [`docs/operator-endpoint.md`](../../docs/operator-endpoint.md)).

## Run it

```sh
# no binary to build: the image installs the released package
cd examples/status
docker build -t fpmng-status-example .
docker run --rm -p 8080:8080 -p 8081:8081 -p 8082:8082 fpmng-status-example
```

## Verify

Generate some traffic on `[app]`, then read it back from the operator
listeners:

```sh
curl -s http://localhost:8080/index.php >/dev/null
curl -s http://localhost:8080/index.php >/dev/null
curl -s http://localhost:8081/status
curl -s http://localhost:8082/metrics
```

`/status` (JSON, port 8081) and `/metrics` (Prometheus text, port 8082) are
served by the master on listeners of their own, not through the gateway, PHP or
FastCGI. Both should show the `app` pool's request count having advanced past
zero.

Not re-run when this example was moved to the operator endpoint (#639): CI
builds the image and runs `php-fpm-ng -t` on this configuration inside it
(`build/test-shipped-configs.sh`), nothing more.
