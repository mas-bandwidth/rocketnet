# Preview-2 qualification record

Preview-2 is published with recorded manual results and explicit known issues.
The release owner authorized the cut on 2026-10-22 with unresolved findings and
unfinished experiments carried to preview-3. CI restructuring is deferred;
GitHub CI is not a deliverable or publication gate for this preview.

## Source provenance

The original publication named main commit
`c1a63ac978c0389f97e2d37e9ab61c8139191e34`, whose complete source tree
matches candidate `a76d641ad64768e239528cd1c6bfdf37a0caaa41`.
The first recut named `2f88961a0b42713b832478df257d30e15c0944e6`, adding
the qualification documents and moving root-level test sources into `tests/`.
The corrected recut also includes the Windows resource-path fix from
`0c45746304893b384cb85a664bd05093e70683da` and the narrow functional and
unit fixture repairs described below, plus the GSO/io_uring send-boundary
retry repair from `d7f26728`. The final commit and native suite
results are recorded in the [published release notes](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.0.0-preview-2).

The owner explicitly authorized replacing this preview tag and requires native
unit and functional suites to pass on Linux, macOS and Windows at the final
commit. The final tagged commit is
`83d04c4d9df1b7f54fd6886455372b237f85740b` (v1.0.0-preview-2). One
preview-2 functional gate status is recorded here, in
[TESTING.md](../TESTING.md) and in the README: the functional gate is not
green at that commit. The macOS Debug socket receive-short condition and
the s390x big-endian full-system run are known issues deferred to
preview-3; the recorded manual runs and receipts below retain their
original tested heads and scope and do not turn that gate green. CI
infrastructure is not a deliverable.

The runtime candidate was `ba09993ba30e1c93c55d8d2f0bdf42baedd9ee0a`.
Commit `a76d641ad64768e239528cd1c6bfdf37a0caaa41` restores the earlier CI
configuration and records the manual qualification policy; it changes no SDK
or test implementation. The original publication documentation did not change
those inputs; subsequent recut changes are recorded separately here. Results
below retain their original tested commits. They are not blanket passes for every platform or configuration.

The SDK runtime is unchanged from `c9db35ff19dc17f0bc3801a1527fd5175260d022`
through the runtime candidate. Later repairs affect existing test checks,
GCC sanitizer declarations and Windows package-consumer probe paths. The TLA
input tree is unchanged from `b4c0a1096e4962d7e60cff6e1f598268525eb349`.

## Completed evidence

| Scope | Recorded result | Tested inputs |
| --- | --- | --- |
| Channel seed sweep | 60,000 runs, zero failures | `ba09993b` |
| Windows hard-kill and closed-port checks | All eight variants of each passed; causal live-peer progress, dead-slot timeout/reuse and closed-port delivery retained | `63a386c8`, integrated as `94e68995` |
| Darwin dual-stack port repair | 50/50 pressure repetitions and 256/256 namespace checks passed | Repair integrated before `c9db35ff` |
| Quiet client receive latency | Normal server-to-client delivery returned to one frame in the recorded comparisons; this is not a bound under load | Repair integrated in `c9db35ff` |
| Threaded TSan, sanitized soak, Linux Clang and nightly proofs | Scoped jobs passed | `c9db35ff` |
| Leak checks | Full recorded job passed | `415e796d` |
| macOS Release, happy path, platform selftests, frame-budget report, soak and examples | Scoped jobs passed; examples included universal builds and Rosetta execution | `415e796d` |
| Linux fuzz | All four full-duration targets passed | `415e796d` |
| Linux examples and frame budget | Examples passed; frame-budget p99 median 16.49 microseconds passed | `a76d641a` |
| Required TLA cases | 287/287 passed across 44 groups; input hashes and pinned tools audited | `b4c0a109`, identical model inputs in candidate |
| TLA benchmarks | Three passed; eighteen bounded-timeout results remain explicit debt | Same model inputs |
| Linux servers with Linux portable clients | All four server transports passed non-timing correctness at 1, 10, 100, 500 and 1,000 clients, 60 seconds per rung | `415e796d`, identical SDK and load-harness inputs |
| Linux portable server with Windows portable clients | All five rungs passed non-timing correctness, including 60,000 messages in each direction at 1,000 clients | `ba09993b` |
| Linux XDP server with Windows portable clients | 1, 10, 100 and 500 passed non-timing correctness; 1,000 failed | `ba09993b` |
| Linux GSO server with Windows portable clients | 1 and 10 passed; the 100-client run was interrupted for the release cut | `ba09993b` |
| io_uring lifecycle availability repair | Supported native lifecycle check passed; no-liburing full-system run no longer registered unavailable io_uring lifecycle cases and crossed the former mismatch point | `8d7e7732`, integrated as `ba09993b` |
| Documentation, version/wire checks, static analysis, CBMC and TLA ledger checks | Scoped recorded checks passed; manual and GitHub receipts remain distinguished in the evidence ledger | Candidate inputs as recorded |

Server-update timing is separate from correctness. At 1,000 Linux clients,
upper-bound p99 values were 2,282 microseconds for XDP copy mode, 935 for
portable, 1,146 for GSO and 1,846 for io_uring. These do not meet the
100-microsecond target. XDP was detached after the recorded privileged runs.
The evidence establishes copy mode, not zero-copy or line-rate throughput.

## Recut repairs and remaining qualification

- **XDP with 1,000 Windows clients:** eight connections reached the reliable
  message timeout during startup. The exact cause is unresolved. The passing
  Linux-client ladder and portable-server control do not qualify this failing
  combination.
- **GSO/io_uring send retry repaired for this recut:** ordinary server send
  calls retry retained native work even without a new message. This keeps
  the native flush nonblocking. Functional error-injection checks settle
  earlier sends and observe completion through the ordinary send boundary,
  rather than bypassing it with direct native flushes.
- **macOS flood isolation repaired for this recut (the separate macOS Debug socket receive-short condition remains a known issue, see the functional gate status above):** exact-delivery functional
  cases lost packets while the eight-thread destroy-under-traffic flood ran
  concurrently. The repair gives that existing flood its own Darwin suite
  group without changing workload, assertions or deadlines. Full Debug auto
  and portable-pinned runs passed at `afdc49b5` in 269 and 270 seconds while
  kernel drop counters still rose; that patch is included in this recut.
- **s390x fixture repaired; timing findings and the full-system guest run remain known issues:** the io_uring codec unit
  fixture now writes native UAPI fields in native byte order. Native Linux
  Release passed; verified big-endian S/390 Debug and Release each failed the
  original assertion on the base and passed with `1b91edbf`. The SDK parser
  is unchanged. Separate full-system connection and handoff completion
  failures during late external host overlap remain unclassified; no
  completion deadline was changed. Existing qemu-user jobs build the unit
  binary but run only the wire and sodium checks.
- **Full Linux soak:** completed non-timing cells preceded a process kill in
  the XDP cell. The full run is incomplete; no kernel receipt establishes the
  cause of that kill. Separately, the known 1,000-client shutdown memory spike
  and secrets-only destroy work remain for preview-3.
- **Remaining platform coverage:** unfinished macOS fuzz and LAN rows, Windows
  final-input Debug/ASan/toolchain/example/soak/frame-budget rows, remaining
  Windows LAN rows, and the instruction gate (tool-version prerequisite mismatch) are
  not represented as passes. Their recorded earlier receipts retain their
  narrower source and configuration scope.
- **Other limits:** Windows server LAN qualification awaits suitable hardware;
  server frame-time targets and XDP zero-copy/throughput remain unestablished.

The [release notes](RELEASE-NOTES-v1.0.0-preview-2.md) describe migration,
including the requirement to request shutdown and wait for completion before
destroying endpoints. The [testing tables](../TESTING.md) keep historical
preview-1 evidence separate from this preview's recorded scope.
