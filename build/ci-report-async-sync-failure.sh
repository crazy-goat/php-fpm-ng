#!/bin/sh
# Issue #374: the weekly async-sync workflow (.github/workflows/async-sync.yml)
# calls this when the merge from main conflicts, or when the fiber+async
# build/test run fails, instead of leaving a red scheduled run that nobody
# happens to open. That issue is the workflow's real output -- more than the
# green run, per #374's own description.
#
# gh, not curl+API like build/ci-cancel-run.sh: this script only ever runs on
# the plain ubuntu-latest job (the merge and the push/report job), never
# inside the CI image container that build-matrix.yml's jobs use, and gh
# ships on GitHub-hosted runners by default -- unlike the CI image, which
# deliberately does not carry it (see ci-cancel-run.sh's own note on why IT
# uses curl).
#
# Usage: GH_TOKEN=... ci-report-async-sync-failure.sh <reason> <body-file>
#   <reason>     short phrase, folded into a FIXED issue title so repeated
#                weekly failures land as comments on one issue instead of a
#                fresh issue every run. Keep it short and stable, not the
#                full error text -- that goes in the body.
#   <body-file>  path to a file with the issue/comment body (conflict details
#                or test failure details, plus a link back to the run).
set -eu

: "${GH_TOKEN:?ci-report-async-sync-failure.sh: GH_TOKEN is not set}"
: "${GITHUB_REPOSITORY:?ci-report-async-sync-failure.sh: GITHUB_REPOSITORY is not set}"

REASON=${1:?usage: ci-report-async-sync-failure.sh <reason> <body-file>}
BODY_FILE=${2:?usage: ci-report-async-sync-failure.sh <reason> <body-file>}
export GH_TOKEN

TITLE="Branch async: weekly sync failed ($REASON)"

# Search open issues carrying both labels for one with this exact title
# rather than trusting gh's full-text --search to be exact (it isn't; it
# would also match a differently-reasoned failure). Listing and grepping the
# title client-side is the precise match.
existing=$(gh issue list --repo "$GITHUB_REPOSITORY" \
  --label area:fiber --label area:ci --state open \
  --json number,title --jq ".[] | select(.title == \"$TITLE\") | .number" \
  | head -n1)

if [ -n "$existing" ]; then
  echo "ci-report-async-sync-failure.sh: commenting on existing issue #$existing"
  gh issue comment "$existing" --repo "$GITHUB_REPOSITORY" --body-file "$BODY_FILE"
else
  echo "ci-report-async-sync-failure.sh: opening a new issue"
  gh issue create --repo "$GITHUB_REPOSITORY" --title "$TITLE" \
    --label area:fiber --label area:ci --body-file "$BODY_FILE"
fi
