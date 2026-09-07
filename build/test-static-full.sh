#!/bin/sh
set -eu

fail() {
  echo "test-static-full.sh: FAIL: $*" >&2
  exit 1
}

[ "$#" -eq 1 ] || fail "usage: $0 /path/to/php-fpm-ng-full"
artifact=$1
[ -x "$artifact" ] || fail "not executable: $artifact"
command -v file >/dev/null 2>&1 || fail "file is required"
command -v docker >/dev/null 2>&1 || fail "docker is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

file_output=$(file "$artifact")
echo "$file_output"
echo "$file_output" | grep -q 'static-pie linked' ||
  fail "artefact is not static-pie linked"

dir=$(mktemp -d)
container=
image=
cleanup() {
  set +e
  [ -n "$container" ] && docker rm -f "$container" >/dev/null 2>&1
  [ -n "$image" ] && docker image rm -f "$image" >/dev/null 2>&1
  rm -rf "$dir"
}
trap cleanup EXIT INT TERM

cp "$artifact" "$dir/php-fpm-ng-full"
cp docker/Dockerfile.scratch "$dir/Dockerfile"
cp docker/fpm.conf "$dir/"
mkdir "$dir/www"
cat > "$dir/www/index.php" <<'EOF'
<?php echo "php-fpm-ng static scratch"; ?>
EOF

suffix=$(basename "$dir")
image="php-fpm-ng-static-test-$suffix"
docker build -t "$image" "$dir" >/dev/null
container=$(docker run -d -p 127.0.0.1::9001 "$image")
port=$(docker port "$container" 9001/tcp | sed 's/.*://')
[ -n "$port" ] || fail "Docker did not allocate an HTTP port"

i=0
while :; do
  body=$(curl -fsS --connect-timeout 1 "http://127.0.0.1:$port/" 2>/dev/null || true)
  [ "$body" = "php-fpm-ng static scratch" ] && break
  docker inspect -f '{{.State.Running}}' "$container" | grep -q true ||
    fail "scratch container exited during startup"
  [ "$i" -lt 100 ] || fail "HTTP gateway did not become ready"
  i=$((i + 1))
  sleep 0.1
done

echo "test-static-full.sh: PASS"
