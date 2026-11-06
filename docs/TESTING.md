# Testing and qualification

RocketNet uses executable tests and recorded results to qualify changes.
The former roadmap and testing plans were retired on 2026-10-29. Their
historical checklists are preserved in Git history, not carried forward as
new feature or test commitments.

## Required checks

The [release process](RELEASING.md#default-ci-requirements) owns the required
checks. Use the results for the exact commit being delivered; an older green
run does not qualify a later commit. [Contributor build checks](BUILD-REFERENCE.md)
provides the local commands and prerequisites.

| Evidence | What it establishes |
| --- | --- |
| [CI](../.github/workflows/ci.yml) | Linux, macOS and Windows builds; unit and functional suites; live transport cases; integration examples; package and ABI checks; sanitizer and fuzz smoke runs; conformance and platform checks |
| [Threaded TSan](../.github/workflows/nightly.yml) | Unit and functional suites with ThreadSanitizer; required on the release commit before tagging |
| [Static analysis](../.github/workflows/static.yml) | Reported GCC analyzer, clang-tidy and cppcheck findings; see the [GCC evidence limit](#static-analysis-evidence-limit) below |
| [TLC records](../.github/workflows/tlc.yml) | Nightly: every stale case is run with TLC and reaches its declared result, and the committed records match the required-case outcomes; see the [case inventory](../proofs/tla/CASES.tsv) and [run ledger](../proofs/tla/RUNS.tsv) |
| [CBMC](../.github/workflows/cbmc.yml) | The bounded arithmetic properties and assumptions in the [proof harnesses](../proofs/cbmc/) |
| [Documentation](../.github/workflows/docs.yml) | Links, compiled examples, tutorial excerpts and generated API reference |
| [Release checks](../.github/workflows/release-check.yml) | Version consistency and declaration of unreleased wire changes |
| [MemorySanitizer](../.github/workflows/msan.yml) | Instrumented portable unit-suite results; this workflow remains advisory under the release policy |

### Static-analysis evidence limit

The GCC wrapper (`tools/run_static.py`) analyzes its selected translation
units and compares findings against the checked-in baseline. It requires a
compile command for each selected unit, collects analyzer warnings and errors,
and fails incomplete compiler invocations. A green result covers those selected
units and the baseline comparison; it is not whole-program analysis.

## Recorded results

The CI repairs in [PR #644](https://github.com/mas-bandwidth/rocketnet/pull/644)
passed all 48 applicable checks at `f6f396d9748762fadbec81e24e93515a5659a8e2`
before merging. The main [CI run](https://github.com/mas-bandwidth/rocketnet/actions/runs/40964376451)
includes the platform, sanitizer, functional and fuzz smoke results.

`proofs/tla/CASES.tsv` and `proofs/tla/RUNS.tsv` are the authoritative TLA
ledgers for all required `PASS` cases (`gate=required`) and declared `bench`
timeout-debt rows (`gate=bench`, `debt != -`). `tools/check_tla_records.py`
validates the ledger schema, exact one-to-one `(config, module)` declaration and
record mapping, expected outcomes and exit codes, and the pinned TLC JAR digest
(`jar_sha256`), while `tlacheck groups --stale` and `tlacheck merge` verify
current-input fingerprint freshness (`input_sha256` against `.tla` and `.cfg`
sources) and `tools/check_tla_coverage.py` checks module, action, symbol and
citation coverage in `proofs/tla/COVERAGE.tsv`. A timeout is not a completed
proof; the [model guide](../proofs/tla/README.md) explains those boundaries.

[Standard witnesses](WITNESSES.tsv) map recorded requirements to their evidence;
the inventory is partial, and rows marked `none` remain evidence gaps.
The [witness review](audits/WITNESS-REVIEW-20261029.md) distinguishes executable
unit tests, model checks, source review and unsupported modes. A source review
is not an executed timing test, and an unsupported mode is not an implemented
feature.

## Qualification limits

A unit or XDP host-shim test does not establish live NIC behavior. A functional
delivery pass does not establish latency or throughput. Model checks establish
the named invariants for their modeled inputs and bounds.

The 100-microsecond main-call target at 1000 clients is not established.
The available XDP run records do not establish zero-copy line-rate performance,
and the historical Windows 10 GbE LAN qualification remains absent. These are
limits on performance claims, not additional implementation commitments.
Qualify a deployment against its own workload and hardware using the
[capacity guide](MEMORY.md), [LAN proof records](runs/lan-1000-proof.md) and
[XDP run record](runs/xdp-zero-copy.md).

The [preview-4](QUALIFICATION-v1.0.0-preview-4.md),
[preview-3](QUALIFICATION-v1.0.0-preview-3.md) and
[preview-2](QUALIFICATION-v1.0.0-preview-2.md) qualification documents retain
their original tested heads and results. Their old placeholders and pending
experiments are historical; they are not the current release checklist.

For an application integration, start with the [getting-started example](GETTING-STARTED.md).
Observe connection, delivery and shutdown within bounded deadlines; one update
or a fixed sleep does not establish asynchronous completion.
