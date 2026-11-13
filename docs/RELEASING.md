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
`"v1.1.21"` and the integers are `1.1.21`. The development branch uses
`"dev"` and the integer sentinel `255.255.255`.

The header label does not identify the current checkout as a release. Check
the exact tag and the repository's published release when choosing an artifact.
Advancing a label alone neither tags nor publishes a release.

The numeric `ROCKETNET_VERSION`, CMake project `VERSION` and installed
`RocketNetConfigVersion.cmake` remain numeric. CMake reads the full label
from the header and checks that its numeric portion matches the project
version. It exports `RocketNet_VERSION_STRING` as `v1.1.21` and
`RocketNet_VERSION_PRERELEASE` as an empty string. There is no separate public
prerelease macro.

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
pull request, where it judges the merge result, and on a push to `main`; it does
not run on a tag push. It compares the checked-out commit with the newest `v*`
tag. The job classifies a commit as wire-affecting when it touches one of these paths:

- `standard/TRANSPORT.md`
- `standard/MESSAGES.md`
- `sdk/rocketnet_wire.h`
- `sdk/rocketnet_protocol.h`
- `sdk/rocketnet_protocol.cpp`
- `sdk/rocketnet_connect_token.h`
- `sdk/rocketnet_connect_token.cpp`
- `sdk/rocketnet_message.cpp`
- `sdk/rocketnet_message.h`
- `sdk/rocketnet_ack.cpp`
- `sdk/rocketnet_ack.h`
- `sdk/rocketnet_client_protocol.cpp`
- `sdk/rocketnet_client_protocol.h`
- `sdk/rocketnet_server_protocol.cpp`
- `sdk/rocketnet_server_protocol.h`
- `sdk/rocketnet_channel.cpp`
- `sdk/rocketnet_channel.h`
- `sdk/rocketnet_channel_reliable.cpp`
- `sdk/rocketnet_channel_sequenced.cpp`
- `sdk/rocketnet_channel_receipts.cpp`
- `sdk/rocketnet_keys.cpp`
- `sdk/rocketnet_keys.h`
- `sdk/rocketnet_read_write.h`
- `sdk/rocketnet_layers.h`
- `sdk/rocketnet.h`

This is a path-based check: comments and explanatory edits in those files also
match, even when no wire behavior changes. The job does not inspect semantics.

When there are wire-affecting commits past the newest tag and the shipping
version still equals the tag version, the job fails: nothing announces that a
release is owed. Bump the shipping version to the next label (for a preview,
`v1.0.0-preview-N` becomes `v1.0.0-preview-N+1`, integers unchanged), or revert
the wire change. When the shipping version has already been bumped, the job
warns and the change waits for the tag. When the checkout shows no `v*` tag at
all, the job fails, because it has nothing to compare against.

A pull request that touches a wire-affecting path, while the shipping version
still names the newest tag, is red until it carries the bump. The tag may follow
in the release, but the version in `sdk/rocketnet.h` must no longer name the
newest release once a wire change is on `main`.

## The release gates

A release is cut by one command, `tools/release.py`, and it refuses unless
every release gate is green for exactly the release commit. The gates are the
lines of one file, [tools/release_gates.txt](../tools/release_gates.txt): each
names a workflow, a job (or the whole run), a kind, the first version it is in
force for, the events it reads and its reason. The same list is read by
`tools/release.py` before the tag, by `tools/check_release_tsan.py` on the tag
push (Release Check's second check), by `tools/land.py` before a pull request
merges, and by `tools/check_release_gates.py` in the lint job, which refuses a
workflow or job the list names that does not exist and a per-commit workflow
the list does not judge. This document does not repeat the list; read the file.

The rule for every gate is the same: not-green is red. A run or job that
concluded `cancelled` (a per-commit job past its two-minute `timeout-minutes`
concludes `cancelled`, not `failure`), `timed_out`, `skipped` without a `skip`
row, that is missing, queued or in progress, or that failed, refuses the
release, by name. A job skipped by its own `if:` (the nightly-tier jobs of
ci.yml, the tag-only jobs of release-check.yml) passes only when a `skip` row
names it. When a workflow ran more than once on the commit, the newest
completed run (its newest attempt) decides, and the output says so; a newer
run that has not finished refuses, because its verdict is not in.

In outline, from v1.1.8:

- the per-commit tier: the push run on `main` of CI (ci.yml), static analysis,
  docs and Release Check (roadmap when it ran), every job `success`, and
  every name in [.github/rulesets/main.json](../.github/rulesets/main.json)
  present;
- the nightly tier, runs whose head is the release commit: Nightly CI
  (nightly-ci.yml) with every job `success`; CBMC; TLC records with
  `Model records are current`; the TLC re-run evidence (a `TLC re-run full`
  job on the exact commit, or every `TLC re-run slice K/N` of the last N days'
  scheduled runs on ancestors of the commit); nightly.yml's
  `tsan (threaded transport) (<ref>)` and its three `Release functional
  nightly tier` jobs (linux, macos, windows); msan.yml's `MemorySanitizer`;
  and every job of nightly-soak.yml, nightly-leaks.yml,
  nightly-macos-sanitizers.yml and nightly-channels-stress.yml;
- on the tag push: Release Check's `threaded tsan run on the release commit`
  (the second check) and `release gates passed on the tag` (the tag run's
  final job, red unless the second check and the version check succeeded).

Which tiers gate which release (the owner, 2026-11-13: "I want each point
release to have fast tests only"): from v1.1.8 a point release (vX.Y.Z with
Z not 0 and no pre-release suffix) is gated by the per-commit tier and the
tag push only. A minor or major release (Z is 0), and every pre-release, is
gated by every tier. The rows whose events are `nightly` are the minor-only
rows: both `tools/release.py` and the second
check on the tag skip them for a point release and print each as `MINOR ONLY`.
The nightly tier still runs on `main`'s schedule; none of it is deleted or
muted, and a red in it is fixed in the next point release.

Inside the per-commit tier, from v1.1.10 a point release and every landing
(`tools/land.py` without `--full`) are light: the jobs a `full` row names
(functional suites, transport matrix, happy path, sanitizer legs, fuzz smoke)
still run on every commit and are printed with their result, but do not gate;
the light jobs (builds and unit suites on every platform, lint, checks,
instruction count, static analysis, docs, Release Check) do. A red FULL job is
fixed in the next point release. A minor or major release and a pre-release
hold every job.

A version is judged by the gates in force for it: the gates that were the
release requirement from v1.0.0-preview-6 (the per-commit runs green, Nightly
CI, CBMC, TLC records and the threaded TSan leg green) and the gates added on
2026-11-12, in force from v1.1.8 (every job `success`, Nightly CI every job,
the TLC re-run evidence, MemorySanitizer, the nightly functional tiers, soak,
leaks, macOS sanitizers and channel stress, the tag run's final job). The tool
prints each gate not in force for the version it judges; `--dry-run
--gates-as-of VERSION` judges a commit by another version's gates. v1.0.0
(9904027045) is refused by the gates in force for it: its CI push run failed
and its Nightly CI, CBMC and TLC records runs are not green. v1.1.7
(76edea48e) passes the gates in force for it and is refused by v1.1.8's: no
TLC re-run, MemorySanitizer, soak, leaks, macOS sanitizers or channel stress
run exists on that commit (it was tagged 2.5 hours after soak per transport
went red).

MSan, called advisory until v1.1.7, is a release gate from v1.1.8; it stays
out of the per-PR set.

The [GCC analyzer evidence limit](TESTING.md#static-analysis-evidence-limit)
also applies to release qualification: the gate covers its selected translation
units and compares findings with the baseline. Required green jobs do not
override a known failure in the checks they are intended to run.

The other `nightly*.yml` workflows, the fuzz-nightly workflow and
[tlc-debt.yml](../.github/workflows/tlc-debt.yml) (weekly, each timeout-debt
case run to its declared result as its own job, with its own 96-hour deadline
and its own evidence artifact) are watches, not release gates.

## Landing on main

Every per-commit job carries a two-minute `timeout-minutes`
(`tools/check_ci_budget.py`, in the lint job, refuses one without it); a job
past it concludes `cancelled`, which stops nothing by itself.
`python3 tools/land.py <pr>` merges a pull request only when its head is up
to date with `main`'s current head and every per-commit job on exactly that
head is `success` (the `pull_request` rows of the same gates file, and every
required name of `.github/rulesets/main.json`). A job cancelled at the
two-minute cap on a run's first attempt is re-run once, by the tool, with a
printed line and a pull request comment; a second cancel refuses. A job
cancelled before the cap (superseded or by hand), a failure, a missing job or
a stale head refuses by name. `--dry-run` prints the verdict and touches
nothing. [branch-protection.md](branch-protection.md) holds the ruleset that
makes the same checks required on `main` itself.

## Cutting the release

1. Set the shipping version in `sdk/rocketnet.h`: `ROCKETNET_VERSION_FULL`,
   including any prerelease suffix, and the three integer macros. Change the
   `project(... VERSION ...)` in `CMakeLists.txt` when the integers change;
   configure refuses a mismatch. Leave the development branch at `dev` and
   `255.255.255`. Check the export list and consumers for public API changes.
   Write the release notes as `docs/RELEASE-NOTES-<version>.md` (below).
   Freeze the counter ids the release ships: add the line
   `python3 tools/check_counter_contract.py --released v<version>` prints to
   `tests/counter_contract.inc`, beside the earlier releases' lines. The lint
   check refuses a shipping version without its line, and any later change to
   a frozen row.
2. Run `python3 tools/check_release_doc.py`. It exits 0 when this document
   names every version site that `release-check.yml`'s `version-consistency`
   job reads, and exits 1 naming any site this document omits.
3. Land the release pull request with `python3 tools/land.py <pr>`.
4. For a minor or major release, dispatch the nightly tier on the merged
   commit once it is `main`'s head (a point release skips this step):
   `gh workflow run nightly-ci.yml --ref main`, and the same for `nightly.yml`,
   `cbmc.yml`, `tlc.yml` (a dispatched run is never replaced by a scheduled
   one) and `msan.yml`. Resolve the analyzer evidence limitation above before
   treating the GCC result as a clean analyzer pass.
5. `python3 tools/release.py <version> <commit> --dry-run` prints every gate
   with its run id and conclusion, and the verdict. Wait until it passes.
6. `python3 tools/release.py <version> <commit>`. What the tool does:
   - checks the version format, that the commit is on `main` (an ancestor of
     `origin/main`), that the shipping `ROCKETNET_VERSION_FULL` at the commit
     equals the version, that the notes file exists at the commit and that the
     tag does not exist;
   - judges every gate in force for the version (above) and refuses on any red;
   - creates the annotated tag `vMAJOR.MINOR.PATCH[-PRERELEASE]` on the commit
     and pushes it;
   - waits for Release Check on the tag and requires its tag jobs to succeed
     (`threaded tsan run on the release commit` judges every gate again,
     `release gates passed on the tag` is red unless it and the version check
     succeeded); on red it does not publish;
   - publishes the GitHub release with the notes file, as latest (a
     prerelease as a prerelease).

   A pushed tag is never moved or reused. When the tag run is red, make the
   gates green and re-run that run, then `python3 tools/release.py <version>
   <commit> --resume` waits for it and publishes; or cut the next version.

The token is `GITHUB_TOKEN` or `GH_TOKEN`, else `gh auth token` (which honours
`GH_CONFIG_DIR`). Exit 0: released (with `--dry-run`, would release); 1:
refused; 2: usage or API error.

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
