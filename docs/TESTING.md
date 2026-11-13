# Testing and qualification

RocketNet uses executable tests and recorded results to qualify changes.
The former roadmap and testing plans were retired on 2026-10-29. Their
historical checklists are preserved in Git history, not carried forward as
new feature or test commitments.

## Required checks

The [release process](RELEASING.md#the-release-gates) owns the required
checks. Use the results for the exact commit being delivered; an older green
run does not qualify a later commit. [Contributor build checks](BUILD-REFERENCE.md)
provides the local commands and prerequisites.

| Evidence | What it establishes |
| --- | --- |
| [CI](../.github/workflows/ci.yml) | Linux, macOS and Windows builds; unit and functional suites; live transport cases; integration examples; package and ABI checks; sanitizer and fuzz smoke runs; conformance and platform checks |
| [Threaded TSan](../.github/workflows/nightly.yml) | Unit and functional suites with ThreadSanitizer; required on the release commit before tagging |
| [Mutation campaign](../.github/workflows/nightly.yml) | Nightly, `linux / mutation`: one slice of 90 of the whole mutant list of `tools/mutation/run_mutants.py` (the six core table and ring files and every SDK source changed in the last 91 days; 53,350 mutants on 2026-11-12), the slice being the UTC day number modulo 90. Test code under `sdk/` is left out of every run and printed (the `EXCLUDED` list of `cmake/RocketnetSources.cmake`, today `rocketnet_tests.cpp`, and the `sdk/rocketnet_test_*.h` headers): mutating it measures nothing. The Linux runner leaves out the files the Linux build does not compile (the Windows and Apple lists of `cmake/RocketnetSources.cmake`, today the iocp and rio sources, and headers only they include) and prints them; it does not leave out io_uring or GSO. A slice holds 357 to 494 mutants (mean 418 over every SDK source but test code and the Windows ones at 45eb05364), 52 to 72 minutes at the measured 6.9 mutants a minute with 4 workers. The rule is a reviewed baseline, [`tools/mutation/survivors.tsv`](../tools/mutation/survivors.tsv): every known survivor, keyed by file, the line's text and the mutated text (never a line number), labelled `equivalent` with the reason no test can tell it from the library or `gap` with the test owed. The night fails on a survivor the baseline does not hold, on an `equivalent` entry that no longer survives, on a `gap` entry that no longer survives (its entry goes and the ratchet drops), on an entry the generator no longer emits, and when the number of `gap` entries differs from the file's `gap-ratchet`; that number is printed as `MUTATION-GAPS` and only goes down. AF_XDP survivors are judged by the baseline like the rest. The baseline was seeded from the 168 survivors of slice 69 (run 43219892575); a slice not yet reviewed into it fails until it is. The per-commit lint checks the baseline without a build (`run_mutants.py --check-baseline`), and `--rejudge` re-judges a night's `results.json` against an edited baseline. The 90 slices cover the whole list every 90 nights. workflow_dispatch `mutation_full` runs the whole list (every file but test code and the Windows sources) in one run under the same rule, about 99 hours with 4 workers, inside the job's 120-hour limit. Each night's slice and rule are printed (`MUTATION-SLICE`, `MUTATION-RULE`) and kept with the results and the summary as the `mutation-slice-*` artifact, 90 days, one full cycle |
| [Static analysis](../.github/workflows/static.yml) | Reported GCC analyzer, clang-tidy and cppcheck findings; see the [GCC evidence limit](#static-analysis-evidence-limit) below |
| [TLC records](../.github/workflows/tlc.yml) | Nightly. A green run proves: the coverage rows are true; the coverage checker's tests, the case planner's controls, the records checker's tests and the TLC adapter's controls pass; every committed record is valid and current (no stale group); every non-PASS record is a declared timeout-debt case with a bounded FAIL/124 record; the ledgers merge clean. The job `Model records are current` runs no model when no record is stale: it checks the [run ledger](../proofs/tla/RUNS.tsv) records as committed, and a stale record makes the run red. The job `TLC re-run slice K/N` re-runs the models: each night one slice of every [case](../proofs/tla/CASES.tsv) that is not timeout debt (slice key SHA-256 of group and config modulo 3, K the UTC day number modulo 3, so every case every three nights; `full` on a dispatch runs them all as `TLC re-run full`), and holds each fresh result to its record: result, exit, violated property, input fingerprint, JAR, workers, generated and distinct counts; any difference, or an empty slice, is red by case name. A PASS record over 60% of its 110 s budget is refused on every commit unless `proofs/tla/BUDGETS.tsv` declares a budget of at least twice its seconds (the re-run then runs it under that budget) or it becomes timeout debt. The timeout-debt cases (the rows of [CASES.tsv](../proofs/tla/CASES.tsv) whose `debt` column is not `-`) are checked only to their bounded record; their full results are not part of a release, and their evidence is [tlc-debt.yml](../.github/workflows/tlc-debt.yml), a weekly watch |
| [CBMC](../.github/workflows/cbmc.yml) | The bounded arithmetic properties and assumptions in the [proof harnesses](../proofs/cbmc/) |
| [Documentation](../.github/workflows/docs.yml) | Links, compiled examples, tutorial excerpts and generated API reference |
| [Release checks](../.github/workflows/release-check.yml) | Version consistency and declaration of unreleased wire changes |
| [MemorySanitizer](../.github/workflows/msan.yml) | Instrumented portable unit-suite results; a release gate from v1.1.8 (tools/release_gates.txt), advisory before; not a per-commit check |

### CI hosts

CI runs on self-hosted runners only: Linux jobs on `self-hosted, Linux, X64,
fleet` (Ubuntu 26.04, libxdp 1.6), macOS jobs on `self-hosted, macOS, ARM64,
fleet`, Windows jobs on `rocketnet-windows`. Ubuntu 22.04 LTS and 24.04 LTS
are built and unit-tested nightly in a podman container of each release on a
Linux runner (`ci.yml`, job `distro-container`); [LINUX.md, section 1](LINUX.md#1-build)
lists the tested distributions and libxdp versions. A Linux runner provides,
besides the build dependencies each job checks by name:

- the VM tier for the xdp cell: `vng` (virtme-ng), `qemu-system-x86_64`, a
  `/dev/kvm` the runner user can write, `~/vm-kernels/vmlinuz-$(uname -r)`
  readable by the runner user, and the running kernel's modules;
  `scripts/vm-tier-missing` prints what is absent, and the cell fails with
  that line (`VM-TIER-MISSING`); every guest booted for tests (the transport
  matrix and its nightly repeat, the XDP filter guest gate, the soak, load
  and outage cells) boots under `scripts/vm-boot-guard`: the guest writes a
  started marker as its first act, a guest with none after 120 s is killed
  and booted again, three attempts, and then fails by name (`VM-BOOT-GUARD`,
  `XDP-GUEST-DID-NOT-BOOT`). That deadline is on the boot only;
  `ROCKETNET_NIGHTLY_NO_TIMEOUT=1` lifts the limits on test cases and never
  the boot deadline, and nothing bounds a guest's cases once it has started.
  The nightly s390x full-system guest has the same rule with a 1200 s
  deadline (its boot takes about 4 minutes under TCG). `vng` carries its
  guest command on the guest kernel's command line, which x86 cuts at 2048
  bytes (a longer one panics the kernel before its console is up and the
  guest halts silently), so every caller writes the guest's commands to a
  script file and hands `vng` only its path, and `scripts/vm-boot-guard`
  measures the line with `vng --dry-run`, prints it, and refuses one over
  1900 bytes by name. The nightly cells job runs one xdp repetition from a
  work directory over 100 characters long to hold that;
- a locked-memory limit that covers the io_uring cases ([LINUX.md, section 8](LINUX.md#8-limits-for-1000-clients));
- for `big endian s390x`: the s390x cross compilers, the s390x multiarch
  libsodium, libbpf, libxdp and libstdc++, and `qemu-s390x` or
  `qemu-s390x-static`;
- for `distro-container`: rootless podman.

The nightly soak's real-socket outage cell, `scripts/outage-pair-real --vm` (its header says what it proves), runs in that VM tier: T - 1 outages each way and both ways must end no connection and lose no reliable message, a T + 2 outage must end both sides by timeout.

### Big-endian checks

Every big-endian run is emulated s390x. [PLATFORMS.md, Big-endian
coverage](PLATFORMS.md#big-endian-coverage) is the claim; these are the checks
behind it.

Every commit, the `big endian s390x` leg in `ci.yml`, cross-compiled and run
under qemu-user, with no socket:

- the library and the whole unit suite compile for s390x, XDP included, and
  the embedded XDP object must be big-endian (`cmake/CheckXdpObjectByteOrder.cmake`
  reads its ELF data encoding and fails the build otherwise);
- the XDP program compiles for `bpfeb` against the s390x kernel headers, its
  object is checked by the same script, and a negative control shows the
  script refuses the build machine's byte order;
- `tools/bigendian/wire_check.cpp` and `test_sodium`;
- `tools/bigendian/run_byte_cases.sh bin/test`: every case of the standard's
  byte vectors (`tests/test_unit_standard_*.cpp`,
  `tests/test_unit_connect_token_vectors.cpp`) and every case of the XDP
  filter compiled as host C (`tests/test_unit_xdp_filter_host.cpp`), one
  process each. A case named in those files but not built fails the step. The
  host-C shim (`tests/xdp_host/`) takes the target's byte order: the kernel's
  `<asm/byteorder.h>` for `__constant_htons` and the header bitfields, and
  `bpf_ntohs` defined from `__BYTE_ORDER__` as libbpf defines it. About 18
  seconds on a fleet bench.

Every commit, the `lint` job runs `tools/check_byte_order.py`, which refuses
in sdk/ (the XDP program included) a byte swap outside a byte-order
preprocessor branch, a host-order read of wire bytes outside its reasoned
allow list, and a network-order conversion of netlink data. `--self-test`
holds its red and green controls.

Nightly, `nightly-bigendian-system.yml` in an s390x guest under
qemu-system-s390x: `./bin/test` (the XDP filter cases included, compiled
natively), `./bin/functional` on the automatic transport order and with portable pinned, then
`tools/bigendian/run_interop.sh`: `bigendian_interop`
(`tools/bigendian/interop.c`) over real sockets, a big-endian server with
three little-endian clients on the x86-64 runner, then a little-endian server
with three big-endian clients. Each client sends unreliable and
reliable-ordered messages and one 48 KiB message (fragmented); the server
checks every byte, the sender's index and the token's client id and echoes
each message; the client checks the echoes and disconnects. A wire field
written in host order that no byte vector covers passes between two
big-endian ends and fails here. The default config with three clients (about
104 MB) fits the guest; the light profile caps a message at 1415 bytes, too
small for the fragmented message.

Not covered on big-endian: the XDP data plane (no big-endian host attaches the
program; the guest has no clang, so XDP is compiled out there), the io_uring
rung, and real hardware.

### Cross-version checks

Nightly, `nightly-xver.yml` on a Linux fleet runner: the release notes
promise that every v1.x release interoperates with every other, both ways.
`tools/xver/xver.c` is one end of a real connection; `tools/xver/xver.go`
builds it against three source trees, each with that tree's own library (the
checked-out ref, the previous release tag and v1.0.0), then runs each case
with each older version as the client of the newest server and as the server
of the newest client, and the newest against itself, on the portable server
transport; then every client version against every server version on the
gso and io_uring server transports (io_uring is reported as SKIP with its
reason when the kernel gates it off). The client is portable throughout.

The cases: basic (every reliability mode both ways, lengths up to the
largest message, content, reported reliability and channel, order and
receipts checked, then a clean disconnect), weather (the development link
simulator on both ends), outage-c2s, outage-s2c and outage-both (every
packet one way or both dropped for T - 1 seconds while traffic flows; the
connection must survive), die (the client is killed; a new client takes the
freed slot) and lifecycle (client disconnect, server disconnect, client
silence, server silence, reconnect). A run passes only when both ends exit
0, neither reports a failure, the server verified every session and its end
reasons are exactly the case's list. Each run that does not pass prints
`XVER FAIL case=... client=... server=... transport=...: <reason>` and the job
fails. `go test xver.go xver_test.go` in `tools/xver` checks the grading (the
nightly compiles it with `go test -c` under a deadline, then runs it with none).

One difference between versions is expected, and stated in
`silentClientEnd` in `xver.go`: a v1.0.x server may end a client that went
silent with `RELIABLE_TIMED_OUT` as well as `TIMED_OUT`, because its default
reliable-message timeout equals the connect token's timeout and the two
clocks race (documented in the v1.1.0 release notes; on 2026-11-11, 21 of 24
such ends named `RELIABLE_TIMED_OUT`, the other 3 on the io_uring server).
Only v1.0.x servers get that expectation; every other server must say
`TIMED_OUT`.

Per commit, the main build compiles `xver.c` against the current tree, so a
public API change that breaks it fails at once.

Not covered: the xdp server transport (ci.yml's transport matrix covers it on
the current tree), client transports other than portable, releases between
v1.0.0 and the previous one (v1.0.0, v1.1.0, v1.1.1, v1.1.2 and v1.1.3 were
each run against each other on 2026-11-11, all passing), and links beyond
loopback.

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
the inventory is partial. Its `tier` column names the kind of evidence each
row has: an executable unit test (`unit`), a model check (`model`), a source
review (`review`), or a protocol mode the library does not implement
(`deferred`). A source review is not an executed timing test, and a deferred
mode is not an implemented feature.

## Qualification limits

A unit or XDP host-shim test does not establish live NIC behavior. A functional
delivery pass does not establish latency or throughput. Model checks establish
the named invariants for their modeled inputs and bounds.

The 100-microsecond main-call target at 1000 clients is not established.
[PERFORMANCE.md](PERFORMANCE.md) records 1,000-player rows for the `xdp`,
`gso`, `io_uring`, `iocp` and `rio` server transports; each row qualifies
only the machines, workload, development build and commit it names. Its Linux rows ran over a 10 GbE link between two machines, and its
Windows IOCP and RIO rows on cloud machines. The available XDP run records do
not establish zero-copy line-rate performance, and the Windows cells of the
[LAN proof record](runs/lan-1000-proof.md) remain unmade. These are limits on
performance claims, not additional implementation commitments.
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
