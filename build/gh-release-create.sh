#!/bin/sh
# Create a GitHub Release for a tag and attach every file in a directory.
#
# This exists instead of `gh release create` because the runner that builds the
# packages has no gh CLI, and a release that only works on the one machine
# somebody installed a tool on is not a release process. curl and python3 are
# on every runner here; the API is stable and does not need either of them to
# be a particular version.
#
# Usage: build/gh-release-create.sh <tag> <assets-dir>
# Requires GH_TOKEN with contents: write, and GITHUB_REPOSITORY.
set -eu

TAG=${1:?usage: build/gh-release-create.sh <tag> <assets-dir>}
DIR=${2:?usage: build/gh-release-create.sh <tag> <assets-dir>}
: "${GH_TOKEN:?GH_TOKEN is not set}"
: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is not set}"

API=${GITHUB_API_URL:-https://api.github.com}
# The upload host is a separate one from the API host, and the runner exports
# no env var for it the way it does GITHUB_API_URL, so it is spelled out here.
UPLOADS=${GITHUB_UPLOAD_URL:-https://uploads.github.com}

NOTES=$(cat <<NOTE
php-fpm-ng $TAG.

The packages carry no PHP. They depend on the embed package of the distribution for
the libphp they were linked against, which is named both in the file name and
in the dependency, so two builds for two PHP minors can be told apart before
either is installed.

Install them by path:

    sudo apt install ./php-fpm-ng_*.deb
    sudo apk add --allow-untrusted ./php-fpm-ng-*.apk

They are unsigned by the decision recorded on issue #223: check them against
SHA256SUMS first, which is what authenticates them. See docs/install.md for
what the packaged build supports and what it does not.
NOTE
)

body=$(TAG="$TAG" NOTES="$NOTES" python3 -c '
import json, os
print(json.dumps({
    "tag_name": os.environ["TAG"],
    "name": os.environ["TAG"],
    "body": os.environ["NOTES"],
}))')

# --fail-with-body so an API refusal is visible in the log rather than being a
# silent empty response that the id parse below blames itself for.
response=$(printf '%s' "$body" | curl -sS --fail-with-body \
    -H "Authorization: Bearer $GH_TOKEN" \
    -H "Accept: application/vnd.github+json" \
    -H "Content-Type: application/json" \
    --data-binary @- \
    "$API/repos/$GITHUB_REPOSITORY/releases")

release_id=$(printf '%s' "$response" | python3 -c '
import json, sys
print(json.load(sys.stdin)["id"])')

echo "created release $TAG (id $release_id)"

for asset in "$DIR"/*; do
    [ -f "$asset" ] || continue
    name=$(basename "$asset")
    curl -sS --fail-with-body -o /dev/null \
        -H "Authorization: Bearer $GH_TOKEN" \
        -H "Accept: application/vnd.github+json" \
        -H "Content-Type: application/octet-stream" \
        --data-binary "@$asset" \
        "$UPLOADS/repos/$GITHUB_REPOSITORY/releases/$release_id/assets?name=$name"
    echo "uploaded $name"
done
