# Framework smoke test

`main` ships the `classic` and `worker` executors, so this directory holds one small framework
test for them: Slim 4, in [`slim4/`](slim4/README.md). It runs in CI (job `frameworks` of
`.github/workflows/build-matrix.yml`) and by hand:

```sh
FPMNG=/path/to/php-fpm-ng [PHP=/path/to/php] tests/frameworks/slim4/bin/run.sh
```

Needs a built `php-fpm-ng`, a PHP CLI with cURL and either Composer or network access for the
checksum-pinned Composer phar. No MySQL, Redis or Docker.

The Symfony, Laravel and Slim 4 probes for `pool.executor = fiber` (with MySQL/Redis through
Docker Compose, `run-all.sh`, the Laravel statics audit) are not here: they live on branch
`async` in `async/tests/frameworks/`, documented in `async/docs/frameworks-fiber.md`. `main` is
merged into `async`, so keeping them in this directory would have deleted them there at the next
sync.
