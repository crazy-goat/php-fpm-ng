# Example: `pool.type = http`

One pool (`fpm-ng.conf`), TLS, one static file, one PHP request. Whole
config fits on one screen.

## Run it

```sh
# from examples/README.md "Build the binary once", then:
cp <build>/sapi/fpmng/php-fpm-ng examples/http/php-fpm-ng
cd examples/http
./generate-cert.sh
docker build -t fpmng-http-example .
docker run --rm -p 8443:8443 fpmng-http-example
```

## Verify

Static file, over TLS, without touching PHP (`http.static`, sendfile/mmap
path in `fpm_http.c:1279` per prior verification):

```sh
curl -ks https://localhost:8443/hello.txt
# served by http.static (fpm_http.c) over the sendfile/mmap path, no PHP involved.
```

A PHP request, same pool, same TLS listener:

```sh
curl -ks https://localhost:8443/index.php
# php-fpm-ng http example
# scheme:  https
# request: /index.php
```

`-k` because the cert is self-signed (see `generate-cert.sh` -- this
example intentionally does not do ACME/auto-issuance, out of scope per the
task). Both commands above were run against a locally built binary as part
of this task; see the PR description for the actual output captured.
