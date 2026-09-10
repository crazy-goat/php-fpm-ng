# CI image for build-matrix.yml.
#
# Every job except static-musl runs inside this image, so the workflow no
# longer apt-get installs anything per job: the dependency set lives here and
# is reviewed as a diff like any other file. static-musl stays on the bare
# runner because it drives `docker run` with host bind mounts of its own.
#
# Pinned to 24.04 to match what ubuntu-latest gave the jobs before, so the
# clang-tidy caveat in build-matrix.yml keeps its original meaning: the
# checker version moves when this base image moves, not on GitHub's schedule.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential autoconf automake libtool bison re2c pkg-config \
      libevent-dev libssl-dev zlib1g-dev \
      ccache clang-tidy valgrind \
      git ca-certificates curl openssl \
      locales \
    && rm -rf /var/lib/apt/lists/*

# Issue #105: the locale leg of fpmng-http-direct-request-parity.phpt is only
# discriminating under a Turkish locale. Generated here rather than per job so
# no step needs root at runtime.
RUN locale-gen tr_TR.UTF-8

# The runner checks the workspace out as its own unprivileged user, but a
# container job runs as root, so git refuses every command with "detected
# dubious ownership" -- and build/check-owned-warnings.sh sends git's stderr
# to /dev/null, so it surfaces as an empty file list and a bare exit 2 rather
# than as a permissions error. Scoped to this CI image, where the only
# repository present is the one the job just checked out.
RUN git config --system --add safe.directory '*'

# ubuntu-latest ran jobs as an unprivileged user, and the suite depends on
# that: php-fpm refuses to start as root, so as root 47 of 48 fpmng tests
# report SKIP "Refusing to run as root" and the job goes green on PASS=0.
# fpmng-acme-state.phpt fails outright, because root ignores directory
# permissions (CAP_DAC_OVERRIDE) and its read-only-directory case never
# throws.
#
# uid 1001 matches both the gh-runner account on the self-hosted machine and
# the `runner` user on GitHub-hosted images, so the workspace bind mount is
# owned by the container user either way. Jobs opt in with
# `options: --user 1001:1001`; gateway-privileges deliberately does not,
# since it tests dropping root.
RUN useradd -m -u 1001 -s /bin/bash ci

# Shared with the host across runs; see the ccache volume in build-matrix.yml.
ENV CCACHE_DIR=/ccache \
    CCACHE_MAXSIZE=5G

# The volume is mounted from the host, but create it owned by the job user so
# a run with no volume (a GitHub-hosted fallback) still has a writable cache.
RUN install -d -o 1001 -g 1001 /ccache
