# 053 — CI `phpt` job fails on `packages.microsoft.com` 403 (infra flake)

**Track:** nice-to-have.
**Priority:** low, but it will cost someone 20 minutes every time it fires.
**Status:** open.

## Where it was seen

2026-09-08, PR #31, first CI run: the `phpt` job failed before building
anything:

    E: Failed to fetch https://packages.microsoft.com/ubuntu/24.04/prod/dists/noble/InRelease  403 Forbidden

A rerun with no changes passed. The job's Dockerfile installs runtime
dependencies through an apt setup that includes Microsoft's repo; when that
host 403s (rate limit, geo, transient), a mergeable PR shows a red required
check for a reason unrelated to the code.

## What this task must produce

1. Identify why the `phpt` job image pulls from `packages.microsoft.com`
   (probably MSSQL tooling or a copied base image step) and drop it if it is
   not actually needed for running the fpm `.phpt` suite.
2. If it is needed: pin/vendor it or move the job to a base image that
   already has it, so an upstream 403 cannot fail our merge gate.
3. Optionally: make the affected step retry once, since a rerun with zero
   changes passed.

## Explicitly out of scope

- The intermittent `phpt` test failure itself (task 029's territory) — this
  is only the dependency-install step.
