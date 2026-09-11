#!/bin/sh
# Issue #209: cancel the whole workflow run, called from an `if: failure()`
# step at the end of every job in build-matrix.yml.
#
# build-matrix.yml is 15 separate jobs, not a matrix, so GitHub's
# `strategy.fail-fast` does not apply and there is no workflow-level
# equivalent. Without this, one red job leaves the other fourteen compiling on
# four runner instances that share one 8-thread box, for a result nobody will
# read: the PR is already unmergeable.
#
# The cost, measured rather than assumed (issue #209, run 34600498146): the
# run's conclusion becomes "cancelled", and so does the conclusion of the job
# that failed -- the cancel arrives while this very step is running, so the
# job never gets to report "failure". Every job in the run therefore looks
# identical from the outside, and "which one broke" is only visible one level
# down, in the red step inside it.
#
# That is why this script writes the failing job's name into the run summary
# before cancelling: the summary is the first thing the run page shows, so the
# answer is there without opening fifteen jobs. A cancelled run is still not a
# passing check, so nothing can be merged by accident.
#
# A job killed mid-compile leaves a half-written build tree behind. That is
# already handled and is not a new hazard: build/ci-build-tree.sh and
# build/static-full.sh each write a dirty marker before compiling and clear it
# only on success, so the next run discards the tree instead of linking objects
# from two configurations.
#
# Limit, by design: this runs from the job's checkout, so a job that dies
# before actions/checkout -- a registry 403 on the container image, a checkout
# failure -- cannot cancel anything and the run proceeds as it did before.
# Making it independent of the checkout would mean inlining the API call into
# fifteen copies of a shell one-liner, which is the thing this script exists
# to avoid.
#
# curl, not gh: the CI image has curl and no gh, and static-musl runs
# uncontainerised on the host where gh is likewise not guaranteed.
set -eu

: "${GITHUB_API_URL:=https://api.github.com}"
: "${GH_TOKEN:?ci-cancel-run.sh: GH_TOKEN is not set (pass secrets.GITHUB_TOKEN)}"
: "${GITHUB_REPOSITORY:?ci-cancel-run.sh: GITHUB_REPOSITORY is not set}"
: "${GITHUB_RUN_ID:?ci-cancel-run.sh: GITHUB_RUN_ID is not set}"

url="$GITHUB_API_URL/repos/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID/cancel"
echo "ci-cancel-run.sh: this job failed; cancelling run $GITHUB_RUN_ID"
# The run page shows this above the job list, which is the only place the
# culprit is legible once every job reports "cancelled".
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  {
    echo "### fail-fast: \`${GITHUB_JOB:-this job}\` failed"
    echo
    echo "The rest of run \`$GITHUB_RUN_ID\` was cancelled by build/ci-cancel-run.sh (issue #209)."
    echo "Every other job reports **cancelled**; the failure is in this one."
  } >> "$GITHUB_STEP_SUMMARY"
fi
# 202 Accepted is the success case; 409 means the run is already finishing or
# already cancelled, which is the outcome we wanted anyway. Never fail the
# step: the job is red for its own reason and must stay red for that reason.
code=$(curl -sS -o /tmp/ci-cancel-run.out -w '%{http_code}' -X POST \
  -H "Accept: application/vnd.github+json" \
  -H "Authorization: Bearer $GH_TOKEN" \
  -H "X-GitHub-Api-Version: 2022-11-28" \
  "$url" || echo 000)
echo "ci-cancel-run.sh: POST .../cancel -> $code"
[ "$code" = "202" ] || { echo "ci-cancel-run.sh: response body:"; cat /tmp/ci-cancel-run.out 2>/dev/null; }
exit 0
