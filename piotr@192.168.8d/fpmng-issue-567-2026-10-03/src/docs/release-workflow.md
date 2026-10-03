# Release workflow

One milestone = one release. Versions follow
[Semantic Versioning](https://semver.org/) and the changelog follows
[Keep a Changelog](https://keepachangelog.com/). Tags are `vX.Y.Z`.

Everything is written in **English**, including release notes.

## 1. Release gate

A release is ready when the milestone has **no open issues**:

```bash
gh api repos/{owner}/{repo}/milestones --jq '.[] | select(.title=="vX.Y.Z") | {title, open_issues, closed_issues}'
```

- Open issues that will not make it: move them to the next milestone
  (`gh issue edit <N> --milestone vX.Y.(Z+1)`).
- The default branch must have a green `ci-ok`.

## 2. Choose the version

| Change | Bump |
|---|---|
| Bug fixes only | patch (`1.2.3` → `1.2.4`) |
| New, backward compatible features | minor (`1.2.3` → `1.3.0`) |
| Breaking changes | major (`1.2.3` → `2.0.0`) |

Before `1.0.0`, breaking changes bump the minor version. The milestone title
already holds the planned version. Change the milestone title if the plan changed.

## 3. Prepare the CHANGELOG (pull request)

```bash
git switch -c chore/release-vX.Y.Z
```

In `CHANGELOG.md`:

- Rename `## [Unreleased]` to `## [X.Y.Z] - YYYY-MM-DD`.
- Add a fresh empty `## [Unreleased]` above it.
- Group entries under Added, Changed, Deprecated, Removed, Fixed, Security.
- Update the compare links at the bottom, if the file has them.
- No file carries the version. The packages take it from the tag (`FPMNG_RELEASE` in
  `.github/workflows/release.yml`). If the PR adds or removes `.phpt` tests, check the
  expected counts in `build/ci-package-gate.sh` (`EXPECT_TOTAL`), because the release
  workflow runs that gate.

Open a PR titled `chore: release vX.Y.Z`, wait for `ci-ok`, squash merge.

## 4. Tag

Tag the merge commit on the default branch with an **annotated** tag:

```bash
git switch <default-branch> && git pull --ff-only
git tag -a vX.Y.Z -m "Release vX.Y.Z"
git push origin vX.Y.Z
```

## 5. GitHub Release

Pushing the tag starts `.github/workflows/release.yml`. This repository builds packages, so the
release workflow is its own and not the shared one from `crazy-goat/.github`. It:

1. builds, installs into a clean container and tests the `.deb` and the `.apk`, with and
   without TLS and ACME, through `build/ci-package-gate.sh`;
2. collects the assets: `php-fpm-ng_<tag>_php<minor>_<arch>.deb`,
   `php-fpm-ng-<tag>-php<minor>-<arch>.apk`, the same two for `php-fpm-ng-tls`, and
   `SHA256SUMS`. The packages are unsigned on purpose (issue #223);
3. only for a tag, creates the GitHub Release with `gh release create --verify-tag`. The
   notes are the matching `CHANGELOG.md` section (truncated to 120000 characters), and the
   assets are attached. The job fails when the section is missing or empty.

`workflow_dispatch` rehearses steps 1 and 2 without a tag and uploads the assets as a
workflow artifact. Run it before tagging when packaging changed:

```bash
gh workflow run release.yml --ref main
gh run watch
```

After a tag:

```bash
gh run watch
gh release view vX.Y.Z
```

## 6. Close the milestone

```bash
gh api -X PATCH repos/{owner}/{repo}/milestones/<number> -f state=closed
```

Make sure the next milestone `vX.Y.(Z+1)` (or the next minor) exists.

## 7. After the release

- Check that install instructions work with the new version (Packagist, Go proxy, ...).
- If something is wrong, do not move the tag. Fix forward with a patch release.

## Checklist

- [ ] Milestone has no open issues, CI is green
- [ ] CHANGELOG section `[X.Y.Z] - date` written, `[Unreleased]` is empty
- [ ] Release PR merged
- [ ] Annotated tag `vX.Y.Z` pushed
- [ ] GitHub Release exists with the CHANGELOG notes, the `.deb` and `.apk` assets and `SHA256SUMS`
- [ ] Milestone closed, next milestone exists
