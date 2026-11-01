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
| [TLC records](../.github/workflows/tlc.yml) | Model inputs match the recorded required-case outcomes; see the [case inventory](../proofs/tla/CASES.tsv) and [run ledger](../proofs/tla/RUNS.tsv) |
| [CBMC](../.github/workflows/cbmc.yml) | The bounded arithmetic properties and assumptions in the [proof harnesses](../proofs/cbmc/) |
| [Documentation](../.github/workflows/docs.yml) | Links, compiled examples, tutorial excerpts and generated API reference |
| [Release checks](../.github/workflows/release-check.yml) | Version consistency and declaration of unreleased wire changes |
| [MemorySanitizer](../.github/workflows/msan.yml) | Instrumented portable unit-suite results; this workflow remains advisory under the release policy |

### Static-analysis evidence limit

The current GCC wrapper (`tools/run_static.py`) does not fail on every analyzer
invocation failure. It collects `warning` diagnostics with `-Wanalyzer-*`
options, while the default warnings-as-errors build can emit them as `error`
diagnostics with `-Werror=analyzer-*`; it also does not check the compiler's exit
status. A green GCC job alone therefore does not establish a completed, clean
analysis. Check the analyzer invocations, exit statuses and diagnostics before
crediting that result. The required workflow remains required; this limitation
is not a waiver or a claim that the wrapper has been repaired.

## Recorded results

The CI repairs in [PR #644](https://github.com/mas-bandwidth/rocketnet/pull/644)
passed all 48 applicable checks at `f6f396d9748762fadbec81e24e93515a5659a8e2`
before merging. The main [CI run](https://github.com/mas-bandwidth/rocketnet/actions/runs/40964376451)
includes the platform, sanitizer, functional and fuzz smoke results.

The current TLA ledger contains **639 required cases with PASS results** and
**16 non-required bench cases that exhausted their declared time budget**.
The model-record check verifies current inputs. A timeout is not a completed
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
