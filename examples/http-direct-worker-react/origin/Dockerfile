# A TLS origin for the /tls route, and nothing else. It has one job that the
# route depends on: deliver a body far above ReactPHP's 64 KiB read chunk,
# slowly enough that it arrives as many TLS records over about a second. If it
# arrived in one burst, a watcher that only ever fires once could still finish
# the download and the route would prove nothing.
FROM alpine:3.22 AS cert
RUN apk add --no-cache openssl \
	&& mkdir -p /tls \
	&& openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
		-keyout /tls/origin.key -out /tls/origin.crt \
		-subj "/CN=origin" -addext "subjectAltName=DNS:origin,DNS:localhost,IP:127.0.0.1"

FROM nginx:1.27-alpine
COPY --from=cert /tls /etc/nginx/tls
COPY origin/nginx.conf /etc/nginx/conf.d/default.conf
# Incompressible, so no proxy or filter in between can turn the measured body
# size into something else.
RUN head -c 1048576 /dev/urandom > /usr/share/nginx/html/slow.bin \
	&& head -c 8192 /dev/urandom > /usr/share/nginx/html/small.bin \
	&& echo ok > /usr/share/nginx/html/health
