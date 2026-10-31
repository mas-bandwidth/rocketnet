# Releasing rocketnet

A release is cut from `main`. Nothing is tagged from a feature branch, and a tag
is never moved: if a tag is wrong, cut the next version.

## Version sites

The version lives in `sdk/rocketnet.h`, in two branches selected by
`ROCKETNET_DEV` (1 by default; release artifacts are built with
`-DROCKETNET_DEV=OFF`, BUILDING.md "Production builds").

The shipping branch carries `ROCKETNET_VERSION_FULL` and the integer macros
`ROCKETNET_VERSION_MAJOR`, `ROCKETNET_VERSION_MINOR` and
`ROCKETNET_VERSION_PATCH`. At this revision, the full label is
`"v1.0.0-preview-5"` and the integers are `1.0.0`. The development branch uses
`"dev"` and the integer sentinel `255.255.255`.

The header label does not identify the current checkout as a release. The
`v1.0.0-preview-5` tag identifies commit
`77cd38b0a4f879f3ae04ee6681ac7d94bfd63151`; later commits can still carry that
label. Check the exact tag and the repository's published release when choosing
an artifact. Advancing a label alone neither tags nor publishes a release.

The numeric `ROCKETNET_VERSION`, CMake project `VERSION` and installed
`RocketNetConfigVersion.cmake` remain numeric. CMake reads the full label
from the header and checks that its numeric portion matches the project
version. It exports `RocketNet_VERSION_STRING` as `v1.0.0-preview-5` and
derives `RocketNet_VERSION_PRERELEASE` as `preview-5`. A final release has
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
`v*` tag. The job classifies a commit as wire-affecting when it touches one of these paths:

- `standard/TRANSPORT.md`
- `standard/MESSAGES.md`
- `sdk/rocketnet_wire.h`
- `sdk/rocketnet_protocol.h`
- `sdk/rocketnet_protocol.cpp`
- `sdk/rocketnet_connect_token.h`
- `sdk/rocketnet_connect_token.cpp`

This is a path-based check: comments and explanatory edits in those files also
match, even when no wire behavior changes. The job does not inspect semantics.

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

## Default CI requirements

The release commit's required CI results are its qualification evidence. Tag only a `main`
commit whose required runs are green; the qualifying run is the green CI run on
the tagged commit, found from the commit itself, and the release notes do not
mention it. There is no separate per-release qualification record. The
required runs are:

- **CI** ([.github/workflows/ci.yml](../.github/workflows/ci.yml)), every job:
  - `build-and-test`, named `<os> / <build_type> / <leg>`: Ubuntu 24.04 and
    macOS 15, Debug and Release, four legs per row (`unit`, `functional`,
    `checks`, `builds`);
  - `windows / Release (<leg>)` (legs a, b and c);
  - `XDP / libxdp 1.4 / Debug`, `happy path / <os>` and
    `backend example / <backend>`;
  - `asan + ubsan (<leg>)` (`unit suite`, `unit suite portable`,
    `functional suite`) and `tsan (loopback soak)`;
  - `fuzz smoke (libFuzzer, corpus replay)` and
    `fuzz smoke NDEBUG (every fuzzer, corpus replay)`;
  - `big endian s390x / <build_type>`, `standard conformance`,
    `pinned libsodium (built from source)` and
    `platform selftests / <os>`;
  - `instruction count gate`, `validation tool selftests`,
    `format check (clang-format 18)` and `runs-on-lint`.
- **nightly** ([.github/workflows/nightly.yml](../.github/workflows/nightly.yml)):
  `tsan (threaded transport)` on the release commit. Run it with
  workflow_dispatch on `main` before the tag.
- **static analysis**
  ([.github/workflows/static.yml](../.github/workflows/static.yml)):
  `static analysis (gcc -fanalyzer)` and
  `static analysis (clang-tidy, cppcheck)`.
- **TLC records** ([.github/workflows/tlc.yml](../.github/workflows/tlc.yml)):
  `Model records are current`.
- **CBMC** ([.github/workflows/cbmc.yml](../.github/workflows/cbmc.yml)):
  `cbmc (arithmetic leaf proofs)`, when the change touches `sdk/**` or
  `proofs/cbmc/**`.
- **docs** ([.github/workflows/docs.yml](../.github/workflows/docs.yml)):
  `docs (MrDocs HTML reference build)`.
- **Release Check** ([.github/workflows/release-check.yml](../.github/workflows/release-check.yml)):
  `version consistency` on the pull request that carries the release version
  and on the tag push itself, and `unreleased wire changes`.

The [GCC analyzer evidence limit](TESTING.md#static-analysis-evidence-limit)
also applies to release qualification: the current wrapper can return green
without a completed, clean analyzer invocation. Review the actual invocations,
exit statuses and diagnostics before crediting that job. Required green jobs
do not override a known failure in the checks they are intended to run.

The other `nightly*.yml` workflows and the fuzz-nightly workflow are watches,
not release gates. The `msan` workflow (`MemorySanitizer`) is advisory: its
job is `continue-on-error`. The release commit must also be the commit the tag
points at, not a later merge.

A pushed tag is never moved or reused.

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
4. Build and test with `make && ./bin/test`, then `./bin/functional` where the
   platform permits it.
5. Open the release pull request against `main` with the version change.
   Merge it, then wait for every required run on that merged commit to be green,
   including the dispatched threaded TSan run. Resolve the evidence limitation
   above before treating the GCC result as a clean analyzer pass.
6. On the merged `main` commit, create an annotated tag `vMAJOR.MINOR.PATCH`,
   or `vMAJOR.MINOR.PATCH-PRERELEASE` for a prerelease, for example
   `v1.0.0-preview-N`, whose complete label equals the shipping
   `ROCKETNET_VERSION_FULL`. Do not tag a different commit, and do not move or
   reuse a tag after pushing it. Verify its consistency with the header before
   pushing it. Release Check also performs this check on the tag push.
7. Publish the GitHub release for that tag.

## Release notes

The GitHub release notes are the public record of the release. They state the
version, summarize the user-visible changes since the previous tag, and give
upgrade instructions where behavior changed. They do not mention CI,
workflows or the qualification run. They call out any wire-affecting
change explicitly and link the relevant section of
[standard/TRANSPORT.md](../standard/TRANSPORT.md) and the compatibility impact. A security release names the advisory it fixes and
the first fixed version, and follows the disclosure terms in
[SECURITY.md](SECURITY.md). Release notes do not contain credentials, private
contacts, or embargoed exploit details.
