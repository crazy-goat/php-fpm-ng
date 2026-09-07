#!/bin/sh
# Self-signed cert+key for this example only -- not part of the ACME
# question (task 020, open), see the task file's "explicitly out of scope".
# http.tls_cert accepts a full chain (leaf + intermediates concatenated,
# task 039); a self-signed leaf is a one-certificate "chain" of length 1.
set -e
cd "$(dirname "$0")"
mkdir -p certs
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout certs/privkey.pem -out certs/fullchain.pem \
  -subj "/CN=php-fpm-ng.example"
echo "wrote certs/fullchain.pem and certs/privkey.pem"
