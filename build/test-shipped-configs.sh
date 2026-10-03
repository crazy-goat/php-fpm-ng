#!/usr/bin/env bash
# Every configuration and Dockerfile this repository ships (examples/, docker/)
# still works with the current build contract (issue #639).
#
#   build/test-shipped-configs.sh static
#       Hermetic, a grep over the tree: no retired pool type, retired artefact
#       or pre-26.04 base image remains in examples/ or docker/.
#   build/test-shipped-configs.sh images /path/to/php-fpm-ng
#       Needs docker. Builds the image of every shipped configuration and runs
#       `php-fpm-ng -t` on that configuration inside it, because -t also checks
#       the filesystem (chdir, worker script, certificates) the container has.
#       Images that take the binary from the build context (the
#       http-direct-worker-* examples) are built with the binary under test; the
#       package images (released .deb) are checked twice: with the released
#       binary, and with the binary under test mounted over it.
#
# Why images: a bare `-t` on the host fails every shipped configuration on a
# missing /www, which says nothing about the example.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

fail() { echo "FAIL: $*" >&2; exit 1; }

static_check() {
    local bad=0 hits files
    # An empty list (not a git checkout, a git error) would make every grep pass vacuously.
    files=$(git ls-files examples docker) || fail "git ls-files failed"
    [ -n "$files" ] || fail "git ls-files lists nothing under examples/ docker/: static would check nothing"
    # Not *.md: the READMEs say what was removed, and where.
    hits=$(git ls-files -z examples docker | xargs -0 grep -nE \
        -e '^[[:space:]]*pool\.type[[:space:]]*=[[:space:]]*(status|http|fastcgi-ng)[[:space:]]*(;.*)?$' \
        -e 'php-fpm-ng-full' \
        -e '^FROM[[:space:]]+(scratch|ubuntu:(1|2[0-5])[.0-9]*|debian:)' \
        -e 'build/prepare\.sh|php-src' \
        --exclude='*.md' --exclude='*.lock' 2>/dev/null)
    if [ -n "$hits" ]; then
        echo "$hits" >&2
        bad=1
    fi
    # The docs must not point at what no longer exists.
    hits=$(git ls-files -z examples docker | xargs -0 grep -nE \
        -e 'Dockerfile\.scratch|Build the binary once|build/prepare\.sh|php-fpm-ng-full|fpm_pool_status' --include='*.md' 2>/dev/null)
    if [ -n "$hits" ]; then
        echo "$hits" >&2
        bad=1
    fi
    [ "$bad" -eq 0 ] || fail "retired names remain in examples/ or docker/ (lines above)"
    echo "static: no retired pool type, artefact or base image in examples/ or docker/"
}

# Staged binary of the current image (copied) and the user's own file moved aside (kept).
copied="" kept=""
unstage() {
    [ -z "$copied" ] || rm -f "$copied"
    [ -z "$kept" ] || mv "$kept" "$copied"
    copied="" kept=""
}
trap unstage EXIT

# One image per shipped configuration; echoes nothing, fails with the reason.
image_check() {
    local conf="$1" binary="$2" dir name dockerfile context="" tag args=() ep=() line context_binary="" user
    dir=$(dirname "$conf")
    name=$(basename "$dir")
    if [ -f "$dir/Dockerfile" ]; then
        dockerfile="$dir/Dockerfile"
        context="$dir"
    else
        # http-direct-worker-react has no Dockerfile of its own: it reuses the mysql one with APP_DIR.
        dockerfile="examples/http-direct-worker-mysql/Dockerfile"
        context="examples"
    fi
    # The pattern is a literal ${APP_DIR}, not an expansion.
    # shellcheck disable=SC2016
    if grep -q '^COPY \${APP_DIR}/php-fpm-ng' "$dockerfile"; then
        [ -n "$binary" ] || fail "$conf: its image takes the binary from the build context; pass /path/to/php-fpm-ng"
        # An example whose own Dockerfile is the shared one builds from examples/.
        context="examples"
        context_binary=1
        args=(--build-arg "APP_DIR=$name")
        copied="examples/$name/php-fpm-ng"
        # That path is where the README tells users to put their own binary: keep it.
        if [ -e "$copied" ]; then
            mv "$copied" "$copied.fpmng-user" || fail "$conf: cannot move $copied aside"
            kept="$copied.fpmng-user"
        fi
        cp "$binary" "$copied" || { unstage; fail "$conf: cannot stage the binary"; }
    fi
    if [ "$name" = http ] && [ ! -f examples/http/certs/fullchain.pem ]; then
        examples/http/generate-cert.sh >/dev/null || fail "$conf: generate-cert.sh failed"
    fi
    tag="fpmng-shipped-$name"
    echo "==> $conf ($dockerfile)"
    docker build -q -t "$tag" -f "$dockerfile" "${args[@]}" "$context" >/dev/null \
        || { unstage; fail "$conf: docker build failed"; }
    unstage
    # The entrypoint is [binary, -y, <conf in the image>, -F]: run the same with -t.
    while IFS= read -r line; do ep+=("$line"); done \
        < <(docker image inspect --format '{{range .Config.Entrypoint}}{{println .}}{{end}}' "$tag" | sed '/^$/d')
    [ "${#ep[@]}" -ge 3 ] || fail "$conf: unexpected entrypoint of $tag: ${ep[*]}"
    docker run --rm --entrypoint "${ep[0]}" "$tag" -t "${ep[@]:1}" \
        || fail "$conf: php-fpm-ng -t refused it inside $tag"
    # The package images carry the RELEASED binary, so the run above proves the
    # config against the last release only. Run -t again with the binary under test
    # mounted over it, so a PR that retires a directive fails here.
    if [ -z "$context_binary" ]; then
        # The CI binary is a TLS build: it links libevent_openssl, which the package
        # images (built for the non-TLS release) may not have.
        user=$(docker image inspect --format '{{.Config.User}}' "$tag")
        printf 'FROM %s\nUSER root\nRUN apt-get update \\\n && apt-get install -y --no-install-recommends libevent-openssl-2.1-7t64 \\\n && rm -rf /var/lib/apt/lists/*\nUSER %s\n' "$tag" "${user:-root}" \
            | docker build -q -t "$tag-tls" - >/dev/null || fail "$conf: cannot add libevent-openssl to $tag"
        docker run --rm -v "$binary:${ep[0]}:ro" --entrypoint "${ep[0]}" "$tag-tls" -t "${ep[@]:1}" \
            || fail "$conf: the binary under test refused it inside $tag (released binary accepted it)"
    fi
}

case "${1:-}" in
    static) static_check ;;
    images)
        command -v docker >/dev/null 2>&1 || fail "docker not found in PATH"
        binary="${2:-}"
        [ -n "$binary" ] || fail "images needs the path of the php-fpm-ng binary under test"
        [ -x "$binary" ] || fail "$binary is not an executable file"
        binary=$(cd "$(dirname "$binary")" && pwd)/$(basename "$binary")
        # php-fpm-ng's own files are fpm.conf / fpm-ng.conf; examples/*/origin/nginx.conf is nginx's.
        confs=$(git ls-files 'examples/*/fpm.conf' 'examples/*/fpm-ng.conf' 'docker/fpm.conf')
        [ -n "$confs" ] || fail "no shipped configuration found"
        n=0
        for conf in $confs; do
            image_check "$conf" "$binary"
            n=$((n + 1))
        done
        echo "images: php-fpm-ng -t passed for $n shipped configurations"
        ;;
    *) fail "usage: $0 static | images /path/to/php-fpm-ng" ;;
esac
