# The four images build/ci-package-gate.sh runs its two stages in (issue #240).
#
# WHY THEY EXIST. Timed on the poligon box, one `deb` flavour of the gate cost
# 328 seconds, of which 232 were `apt-get` installing the distribution's PHP --
# 124 in the build stage and 108 in the install stage. Our own build and package
# step was 16 seconds and the 61-test suite was 72. It is not the network:
# measured separately in the same image, `apt-get update` took 3s, downloading
# the packages 6s, and `dpkg` unpacking them 66s. So a caching proxy would have
# bought back six seconds. Doing the unpacking once, here, is what buys back the
# six minutes the gate spends on it across two flavours.
#
# HOW THE GATE USES THEM. It does not branch on whether they exist: the stages
# still run the same `apt-get install` / `apk add` lines they always did. Against
# these images those lines find everything already present and return in a couple
# of seconds; against the bare base image they install, exactly as before. That
# is deliberate -- the package list stays written down in one place
# (build/ci-package-gate.sh), these images are only a warm cache of it, and a run
# on a machine that never pulled them is slow rather than wrong.
#
# WHAT MUST NOT BE BAKED IN. The install stage proves two things that a
# pre-installed dependency would make vacuous: that the package's own declared
# dependencies resolve on a machine that does not have them, and that a package
# built for another PHP minor is refused before anything is unpacked. So the
# -test images carry only what the TEST RIG needs -- the CLI that runs
# run-tests.php, strings(1), and openssl's config file -- and never
# libphp8.5-embed / php85-embed, which is what the package itself depends on.
# The build images have no such property to protect: a build stage is allowed a
# compiler and the headers, and nothing about the gate's meaning depends on
# fetching them again each time.
#
# DISTRIBUTION DRIFT. This cell is the only one in the matrix that links against
# a PHP nobody in this repository pinned -- 8.5.4 from Ubuntu, 8.5.10 from Alpine
# against our php-8.5.9 -- and that is what it is for. Freezing those into an
# image would throw it away, so ci-image.yml rebuilds these nightly, and the
# version line stage 1 prints ("libphp-build.sh: php-config8.5 -> PHP x.y.z") is
# where a drift becomes visible in the job log.

# --- Debian/Ubuntu ---------------------------------------------------------

# ubuntu:26.04, not the 24.04 of ci.Dockerfile: this cell deliberately tracks
# the distribution that ships PHP 8.5, which is the whole reason it can link
# against a libphp nobody here built.
FROM ubuntu:26.04 AS deb-build
ENV DEBIAN_FRONTEND=noninteractive
# The same set build/ci-package-gate.sh names in BUILD_SETUP, for the same
# reasons -- binutils for objdump, php8.5-dev for the headers, the embed package
# for the library, libevent/libacl for what the SAPI needs. dpkg-dev stays out:
# it pulls gcc in, and the two package sets are kept honest about who needs one.
RUN apt-get update -qq \
    && apt-get install -y -qq binutils php8.5-dev libphp8.5-embed \
         libevent-dev libevent-openssl-2.1-7 libacl1-dev \
    && rm -rf /var/lib/apt/lists/*

FROM ubuntu:26.04 AS deb-test
ENV DEBIAN_FRONTEND=noninteractive
# Test rig only. binutils is strings(1); php8.5-cli runs run-tests.php; openssl
# is here for /usr/lib/ssl/openssl.cnf, without which openssl_pkey_new() fails
# and the three ACME tests fail for a reason that has nothing to do with the
# package. None of the three is a dependency of what we ship -- see the header.
RUN apt-get update -qq \
    && apt-get install -y -qq binutils php8.5-cli openssl \
    && rm -rf /var/lib/apt/lists/*
# The gate asserts this image has no compiler before it installs anything. The
# assertion is in build/ci-package-gate.sh, where it runs against whatever image
# is actually used; this is only where the image stops carrying one.

# --- Alpine ----------------------------------------------------------------

FROM alpine:edge AS apk-build
# openssl-dev is named explicitly: Ubuntu's php8.5-dev drags libssl-dev in,
# Alpine's php85-dev does not, and without it fpm_http_tls.h stops the build at
# openssl/ssl.h rather than quietly producing a smaller binary.
RUN apk add --no-cache alpine-sdk php85-dev php85-embed \
      libevent-dev acl-dev openssl-dev

FROM alpine:edge AS apk-test
# Test rig only; php85-embed, the package's own dependency, is deliberately absent.
RUN apk add --no-cache binutils php85 php85-openssl openssl
