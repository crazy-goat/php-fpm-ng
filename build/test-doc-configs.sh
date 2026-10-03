#!/usr/bin/env bash
# Every configuration the user-facing guides print is accepted by the binary
# under test (issue #645).
#
#   build/test-doc-configs.sh /path/to/php-fpm-ng
#
# The pages listed in DOCS mark what they want checked in the info string of a
# fenced block:
#
#   ```ini verify          a complete configuration: `php-fpm-ng -t` must accept it
#   ```ini verify-run      the same, and the master is started on it; the block
#                          is the getting-started pool and must answer GET / with
#                          the body of the `file=/srv/app/public/index.php` block
#   ```php file=/srv/app/<x>   a file the configurations refer to, created verbatim
#
# Paths in a block are the ones a reader types (/srv/app, /run/php-fpm-ng,
# /var/log/php-fpm-ng) and are rewritten into a temporary directory, so no root
# and no installed package is needed. The listen port 8080 is rewritten to a
# free one and the default operator listener 9253 to the port above it. The
# run block also loses its listen.owner/group lines: a non-root master cannot
# chown the socket. A block without a [global] section gets
# `error_log = /dev/stderr` prepended, like a pool.d/ file under the packaged
# master configuration.
# A referenced script that no block creates is created empty: `-t` checks that
# it exists, not what it does.
#
# Run it as an unprivileged user to also start the master: php-fpm refuses to
# start as root without --allow-to-run-as-root. As root only the `-t` checks
# run (no master is started), which is how the pages tell a reader to run it:
# a pool without `user`/`group` is refused there and not for a non-root user.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

DOCS=(docs/guides/getting-started.md docs/guides/migrate-php-fpm-nginx.md docs/guides/migrate-nginx-unit.md
      docs/guides/migrate-supervisord-cron.md docs/guides/framework-recipes.md)

fail() { echo "FAIL: $*" >&2; exit 1; }

binary="${1:-}"
[ -n "$binary" ] || fail "usage: $0 /path/to/php-fpm-ng"
[ -x "$binary" ] || fail "$binary is not an executable file"
binary=$(cd "$(dirname "$binary")" && pwd)/$(basename "$binary")
as_root=0
[ "$(id -u)" -ne 0 ] || as_root=1

tmp=$(mktemp -d)
pid=""
cleanup() {
    if [ -n "$pid" ]; then kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; fi
    rm -rf "$tmp"
}
trap cleanup EXIT

port=${FPMNG_DOC_PORT:-18080}
if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
    fail "port $port is in use; set FPMNG_DOC_PORT"
fi
mkdir -p "$tmp/app/public" "$tmp/run" "$tmp/log" "$tmp/blocks"

# index.tsv: kind <TAB> doc <TAB> file
: >"$tmp/index.tsv"
for doc in "${DOCS[@]}"; do
    [ -f "$doc" ] || fail "$doc is listed in DOCS and does not exist"
    awk -v dir="$tmp/blocks" -v doc="$(basename "$doc")" -v idx="$tmp/index.tsv" '
        /^```/ && !inb {
            info = substr($0, 4); inb = 1; n++
            kind = ""; path = ""
            if (info ~ /(^| )verify-run( |$)/) kind = "run"
            else if (info ~ /(^| )verify( |$)/) kind = "conf"
            else if (match(info, /file=[^ ]+/)) { kind = "file"; path = substr(info, RSTART + 5, RLENGTH - 5) }
            out = dir "/" doc "." n
            if (kind != "") printf "" > out
            next
        }
        /^```/ && inb {
            inb = 0
            if (kind != "") { close(out); printf "%s\t%s\t%s\t%s\n", kind, doc, out, path >> idx }
            next
        }
        inb && kind != "" { print >> out }
    ' "$doc"
done

rewrite() {
    sed -e "s#/srv/app#$tmp/app#g" -e "s#/run/php-fpm-ng#$tmp/run#g" \
        -e "s#/var/log/php-fpm-ng#$tmp/log#g" -e "s#:8080#:$port#g" -e "s#:9253#:$((port + 1))#g"
}

# Files first, so a configuration can point at them.
while IFS=$'\t' read -r kind doc block path; do
    [ "$kind" = file ] || continue
    case "$path" in /srv/app/*) ;; *) fail "$doc: file=$path is not under /srv/app" ;; esac
    dest="$tmp/app/${path#/srv/app/}"
    mkdir -p "$(dirname "$dest")"
    cp "$block" "$dest"
done <"$tmp/index.tsv"

checked=0 ran=0
while IFS=$'\t' read -r kind doc block path; do
    [ "$kind" = file ] && continue
    conf="$block.conf"
    if grep -q '^\[global\]' "$block"; then
        rewrite <"$block" >"$conf"
    else
        { printf '[global]\nerror_log = /dev/stderr\n\n'; rewrite <"$block"; } >"$conf"
    fi
    # Create the directories and scripts the configuration names.
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        case "$p" in
            "$tmp"/*) ;;
            *) fail "$doc: $(basename "$block"): $p is outside the paths the test rewrites" ;;
        esac
        [ -e "$p" ] || { mkdir -p "$(dirname "$p")"; : >"$p"; }
    done < <(sed -nE 's/^[[:space:]]*(cron|supervisor)\.script[[:space:]]*=[[:space:]]*([^ ;]+).*/\2/p' "$conf")
    while IFS= read -r p; do
        [ -d "$p" ] || mkdir -p "$p"
    done < <(sed -nE 's/^[[:space:]]*chdir[[:space:]]*=[[:space:]]*([^ ;]+).*/\1/p' "$conf")
    echo "==> $doc $(basename "$block") ($kind)"
    out=$("$binary" -t -y "$conf" 2>&1) || { echo "$out" >&2; cat -n "$conf" >&2; fail "$doc: php-fpm-ng -t refused the block above"; }
    case "$out" in *"test is successful"*) ;; *) echo "$out" >&2; fail "$doc: -t did not report success" ;; esac
    checked=$((checked + 1))

    if [ "$kind" = run ] && [ "$as_root" -eq 0 ]; then
        # A non-root master cannot chown the socket to the documented owner.
        grep -vE '^[[:space:]]*listen\.(owner|group)[[:space:]]*=' "$conf" >"$conf.run"
        expect="$tmp/app/public/index.php"
        [ -f "$expect" ] || fail "$doc: the verify-run block needs a file=/srv/app/public/index.php block"
        "$binary" -y "$conf.run" -F >"$tmp/master.log" 2>&1 &
        pid=$!
        body=""
        for _ in $(seq 1 50); do
            body=$(curl -fsS --max-time 2 "http://127.0.0.1:$port/" 2>/dev/null) && break
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.2
        done
        status=$(curl -fsS --max-time 2 "http://127.0.0.1:$((port + 1))/status" 2>/dev/null)
        kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
        [ -n "$body" ] || { cat "$tmp/master.log" >&2; fail "$doc: the started master did not answer GET /"; }
        want=$("$(command -v php)" "$expect") || fail "php cannot run the index.php of the page"
        [ "$body" = "$want" ] || fail "$doc: GET / answered '$body', the page promises '$want'"
        case "$status" in *'"pools"'*) ;; *) fail "$doc: the operator endpoint on port $((port + 1)) did not answer /status: '$status'" ;; esac
        ran=$((ran + 1))
    fi
done <"$tmp/index.tsv"

[ "$checked" -gt 0 ] || fail "no verified block found in any page: the test would check nothing"
[ "$as_root" -eq 1 ] || [ "$ran" -gt 0 ] || fail "no verify-run block found: the getting-started page is not exercised"
[ "$as_root" -eq 0 ] || echo "(run as root: -t only, no master started)"
echo "doc configs: -t accepted $checked blocks, $ran started and answered"
