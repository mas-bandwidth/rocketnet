# Releasing rocketnet

A release is cut from `main`. Nothing is tagged from a feature branch, and a tag
is never moved: if a tag is wrong, cut the next version.

## Version sites

The version lives in `sdk/rocketnet.h`, in two branches selected by
`ROCKETNET_DEVELOPMENT`.

The shipping branch carries `ROCKETNET_VERSION_FULL` and the integer macros
`ROCKETNET_VERSION_MAJOR`, `ROCKETNET_VERSION_MINOR` and
`ROCKETNET_VERSION_PATCH`. For this preview, the full label is
`"v1.0.0-preview-1"` and the integers are `1.0.0`. The development branch uses
`"dev"` and the integer sentinel `255.255.255`.

The numeric `ROCKETNET_VERSION`, CMake project `VERSION` and installed
`RocketNetConfigVersion.cmake` remain numeric. CMake reads the full label
from the header and checks that its numeric portion matches the project
version. It exports `RocketNet_VERSION_STRING` as `v1.0.0-preview-1` and
derives `RocketNet_VERSION_PRERELEASE` as `preview-1`. A final release has
an empty prerelease string. There is no separate public prerelease macro.

The `version-consistency` job in
[release-check.yml](../.github/workflows/release-check.yml) checks both
header branches. A shipping label starts with `vMAJOR.MINOR.PATCH` and may
have a `-PRERELEASE` suffix. The development label must be `dev`, with the
sentinel integers. On a `v*` tag push, the complete tag must equal the
shipping `ROCKETNET_VERSION_FULL`.

Review public layout and calling-contract changes separately from the release
label. Keep the installed libraries, export list and consumers consistent.

## The wire-change rule

The `unreleased-wire-changes` job in the same workflow enforces that `main` does
not drift past the newest release with unannounced wire changes. It runs on a
push to `main`, not on a pull request or a tag push, and compares `main` with the newest
`v*` tag. A commit is wire-affecting when it touches one of these paths:

- `standard/TRANSPORT.md`
- `standard/MESSAGES.md`
- `sdk/rocketnet_wire.h`
- `sdk/rocketnet_protocol.h`
- `sdk/rocketnet_protocol.cpp`
- `sdk/rocketnet_connect_token.h`
- `sdk/rocketnet_connect_token.cpp`

When there are wire-affecting commits past the newest tag and the shipping
version still equals the tag version, the job fails: nothing announces that a
release is owed. The release must either bump the shipping version and tag it,
or revert the wire change. When the shipping version has already been bumped,
the job warns and the change waits for the tag. Until the first `v*` tag exists
the job reports itself dormant and exits green.

A pull request that touches a wire-affecting path therefore has to arrange the
version bump that will make `main` truthful after merge. The tag may follow in
the release, but the version in `sdk/rocketnet.h` must no longer name the newest
release once a wire change is on `main`.

## CI that must be green

Tag only a `main` commit whose required runs are green. Those runs are:

- **CI** ([.github/workflows/ci.yml](../.github/workflows/ci.yml)): the full
  `build-and-test` matrix on Ubuntu and macOS in Debug and Release, the Windows
  Release matrix, `asan + ubsan`, `fuzz smoke` and its NDEBUG variant, big-endian
  s390x, `standard conformance`, `tsan`, `pinned libsodium`, and the platform
  selftests.
- **static analysis**
  ([.github/workflows/static.yml](../.github/workflows/static.yml)): `gcc -fanalyzer`.
- **TLC records** ([.github/workflows/tlc.yml](../.github/workflows/tlc.yml)): the
  model records are current.
- **CBMC** ([.github/workflows/cbmc.yml](../.github/workflows/cbmc.yml)) when the
  change touches `sdk/**` or `proofs/cbmc/**`.
- **Release Check**'s `version-consistency` job on the pull request that carries
  the release version, and on the tag push itself.

The nightly and soak workflows are watches, not release gates; the MSan leg is
advisory. The release commit must also be the commit the tag points at, not a
later merge.

## Cutting the release

1. Check out `main` and confirm the worktree is clean and up to date. Do not
   release from a branch or a detached commit.
2. Set the shipping version in `sdk/rocketnet.h`: `ROCKETNET_VERSION_FULL`,
   including any prerelease suffix, and the three integer macros. Change the `project(... VERSION ...)` in `CMakeLists.txt`
   when the integers change; configure refuses a mismatch. Leave the development branch at `dev` and
   `255.255.255`. Check the export list and consumers for public API changes.
3. Run `python3 tools/check_release_doc.py`. It exits 0 when this document names
   every version site that `release-check.yml`'s `version-consistency` job reads,
   and exits 1 naming any site this document omits.
4. Build and test locally with `make && ./bin/test`, then `./bin/functional`
   where the platform permits it. Local runs are a convenience; CI is the gate.
5. Open the release pull request against `main` with the version change, wait for
   the CI above to be green on its head, and merge it without changing the
   commit.
6. On the merged `main` commit, create an annotated tag `vMAJOR.MINOR.PATCH`,
   or `vMAJOR.MINOR.PATCH-PRERELEASE` for a prerelease, for example
   `v1.0.0-preview-1`, whose complete label equals the shipping
   `ROCKETNET_VERSION_FULL`. Do not tag a different commit, and do not move or
   reuse a tag after pushing it. Push the tag and let Release Check's
   `version-consistency` job confirm it against the header.
7. Publish the GitHub release for that tag.

## Release notes

The GitHub release notes are the public record of the release. They state the
version, summarize the user-visible changes since the previous tag, and give
upgrade instructions where behavior changed. They call out any wire-affecting
change explicitly and link the relevant section of
[standard/TRANSPORT.md](../standard/TRANSPORT.md) and the compatibility impact. A security release names the advisory it fixes and
the first fixed version, and follows the disclosure terms in
[SECURITY.md](../SECURITY.md). Release notes do not contain credentials, private
contacts, or embargoed exploit details.
