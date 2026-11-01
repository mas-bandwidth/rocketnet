# Hardening rocketnet

We want you to spend your time making games. This page explains how we challenge
rocketnet before asking you to trust it, what the results establish, and what
still needs qualification. It is a record of engineering work, not a claim
that any network library is invulnerable.

[TESTING.md](TESTING.md) records release qualification. The
[security policy](SECURITY.md) explains private vulnerability reporting and
supported releases. The wire specifications in [STANDARD.md](STANDARD.md)
remain the authority for protocol guarantees.

## What we check

We review parsing, authentication, connection lifetime, replay protection,
acknowledgements, message delivery, memory ownership and transport behavior.
Independent reviewers also check the tests themselves: a green result is useful
only when breaking the promised behavior makes the test fail.

The layers have different jobs:

| Layer | What it can establish | What it cannot establish alone |
| --- | --- | --- |
| Deterministic unit tests | Boundary arithmetic, state transitions and small component contracts | Socket, scheduler or NIC behavior |
| Functional tests | Observable behavior through real integration paths, including payload correctness and recovery | Sustained load or rare failures over long runs |
| Fuzzing and regression corpora | Behavior on generated and previously troublesome inputs | Coverage of every possible input or state |
| Sanitizers and static analysis | Particular memory, concurrency and source defects | Absence of all defects |
| Formal models | Properties within the modeled state space and assumptions | Automatic correctness of the implementation |
| Soak and load tests | Behavior for the recorded duration, workload and deployment | Other hardware, workloads or configurations |

See [tests](../tests/README.md), [fuzzing](../tests/fuzz/README.md),
[OSS-Fuzz integration](../tests/oss-fuzz/README.md), and [TLA+ models](../proofs/tla/README.md)
for the existing tools. A model result needs a correspondence review against
its implementation; a configured fuzz job needs execution evidence.
The current [GCC analyzer wrapper limitation](TESTING.md#static-analysis-evidence-limit)
also means that a green static job alone is not evidence of a completed, clean
GCC analysis. Historical triage records below retain their original scope.

## Source audits and executed verification passes

Source inspections and executed campaigns serve different roles and are recorded
separately under `docs/audits/`:

- **Source-reading area and sweep audits** inspect code paths and record
  findings for follow-up repair, but do not by themselves constitute an executed
  pass gate:
  [config and API](audits/2026-10-24-area-config-api.md),
  [connect tokens and handshake](audits/2026-10-24-area-connect-tokens-handshake.md),
  [encryption, keys and replay](audits/2026-10-24-area-encryption-keys-replay.md),
  [acks and fragments](audits/2026-10-24-area-protocol-acks-fragments.md),
  [channels and messages](audits/2026-10-24-area-protocol-channels-messages.md),
  [Linux AF_XDP](audits/2026-10-24-area-transports-linux-af-xdp.md),
  [Linux portable, GSO and io_uring](audits/2026-10-24-area-transports-linux-portable-gso-uring.md),
  [macOS transports](audits/2026-10-24-area-transports-macos.md),
  [Windows transports](audits/2026-10-24-area-transports-windows.md),
  [code quality A](audits/2026-10-24-code-quality-a.md),
  [code quality B](audits/2026-10-24-code-quality-b.md),
  [assert-only guards](audits/2026-10-24-sweep-assert-only-guards.md),
  [monotonic clock](audits/2026-10-24-sweep-clock-monotonic.md),
  [count-before-success](audits/2026-10-24-sweep-count-before-success.md),
  [integer narrowing](audits/2026-10-24-sweep-int-narrowing.md), and
  [silent discard](audits/2026-10-24-sweep-silent-discard.md).
- **Executed verification and static-analysis records** document the exact
  tested commit, tool invocation, observed outcome, and remaining limits:
  - [Release-build (`-DNDEBUG`) fuzz campaign](audits/2026-10-24-fuzz-ndebug.md)
    at `6ac80bfb59eb3b9e3e6f748c1973c6b6c8f0b228`: ran all 11 `tests/fuzz/` harnesses
    on Linux with `-DNDEBUG`, AddressSanitizer, and UndefinedBehaviorSanitizer
    for 600 seconds each so bounds enforced only by debug assertions are tested
    under release compilation.
  - [Core protocol mutation testing](audits/2026-10-24-mutation-testing.md)
    at `44ab971773c8a05dd00f63e77b7159d6b489a76c`: ran `tools/mutation/run_mutants.py`
    with `./bin/test` as the oracle on Linux Release across ack, fragment,
    protocol, window, and quality-tracker units to identify surviving mutants
    and oracle gaps.
  - [Deterministic simulation fault schedules](audits/2026-10-24-dst-fault-schedules.md)
    at `b0e281c4126c43f268b5129ee29539b416b17a9c`: ran `./bin/dst --seeds 1-1000`
    across named fault schedules on Linux Release, recording passing schedules
    and replaying failing seeds (`clock-back`, `reorder-burst`, `dup-storm`, etc.)
    for targeted fixes.
  - [Production-path line coverage](audits/2026-10-24-coverage-production-paths.md)
    at `aa193eb3bab05b0f384ecdd1f0229c1ccbe41774`: measured `sdk/` line coverage
    via `tools/coverage_split.py`, separating production-exercised lines from
    unit-only and unreached guards (and recording an `example_game_loop` failure
    during the run).
  - [Static-analysis baseline triage](audits/2026-10-24-static-analysis.md)
    at `53cbafcf843fc9f08b7a5de5325c254cd47f9a5d`,
    [zero-unresolved static-analysis triage](audits/2026-10-26-static-analysis-triage.md),
    and [workflow and shell lint](audits/2026-10-26-workflow-shell-lint.md):
    dispositioned GCC `-fanalyzer`, `clang-tidy`, `cppcheck`, `actionlint`, and
    `shellcheck` diagnostics on Linux without claiming that source triage alone
    replaces runtime gates.

## Red-team engagement: October 2026

Alex led protocol red teaming, with independent review of the retained source
and execution evidence. The work exercised malformed traffic, handshake and
replay behavior, session lifetime, hostile clients or servers, and recovery
after adverse traffic. It included native socket transports and Linux guest
AF_XDP runs.

The transport campaign reviewed source revision
`acc4d77b1834a403f45f9ae7450b81c9c06808c2`; earlier core work used
`63fc51d1`. These are historical pins, not qualification of every later commit.
Retained results include Linux portable, GSO and io_uring runs, macOS msgx,
and Windows IOCP and RIO runs. Selected runs used address sanitizers;
that does not mean every transport, address family and configuration received
every sanitizer. Windows evidence was IPv4-specific.

Independent checking found limitations in the evidence as well as useful
results. Some harness checks established continued operation without proving
the intended rejection path. Those checks are not credited as proof of the
stronger property. We also do not publish an aggregate run count as a coverage
score: repeated cases and different levels of evidence are not interchangeable.

In particular, the AF_XDP evidence used a Linux guest and virtual Ethernet in
copy mode. One retained log contains fewer case results than its summary claims;
its missing cases remain unverified. The corresponding sanitizer log is empty,
so it establishes no sanitizer pass. These runs do not qualify a physical NIC,
zero-copy operation or line-rate performance. Incomplete binary and harness
manifests further limit historical reproducibility. Later release evidence must
identify the exact tested source and binaries.

We keep security-sensitive reports private under [SECURITY.md](SECURITY.md).
Public regression tests and release notes should describe corrected behavior
without exposing an unresolved vulnerability.

## Two separate load questions and recorded evidence

**Can a game server sustain the intended 1,000-player workload?** Run the server
on its own machine and generate representative player traffic remotely. Check
received payloads, connection progress, loss and recovery, memory use, and frame
latency under that workload. A load generator simulates the clients; it need not
run 1,000 complete game clients. The [split load guide](LOAD_SPLIT.md) and
[LAN proof orchestration guide](LAN-PROOF.md) explain the dedicated-server
topology. The [same-process load harness](LOAD.md) is useful for
correctness, but its colocated timings do not qualify dedicated-server
performance.

**Can a transport sustain a 10 Gb/s packet workload?** This is a separate
physical-link measurement. The [AF_XDP packet blaster](XDP-PACKET-BLASTER.md)
is an external load-generation tool, not a public client API. Measure actual
transmitted and received traffic, directional rate and packet loss; a requested
send rate is not an achieved wire rate. Hostile-traffic tests must also check
the expected rejection counters and subsequent valid traffic, not just that
the server remains alive.

The historical blaster qualification plan covered the non-portable server transports:
`xdp`, `gso`, `io_uring`, `msgx`, `iocp` and `rio`. Portable remains part of
correctness and security testing, but is not expected to meet the 1,000-player
performance target. Profile support or a successful tool build is not a passed
load test. This record does not establish physical 10 Gb/s or final integrated
1,000-player qualification; those limits do not create new deliverable scope.

Recorded load, soak, and dataplane runs establish the following specific results
and limits:

- **Dedicated-host 1,000-client LAN proof ([summary table](runs/lan-1000-proof.md)):**
  - [Linux `gso` and `io_uring` cells](runs/lan-1000-proof-linux.md) at
    `1ca477339e150220f90e96bbe7c61be5b175eb51`: 1,000 clients in four 250-client
    shards across two client hosts for 600s over LAN passed connection and
    delivery checks.
  - [macOS `msgx` cell](runs/lan-1000-proof-macos.md) at
    `813b4adbe3dec2eee496577e3da2c0d7d66b8063`: 1,000 clients across two Linux
    client hosts to the dedicated macOS server over LAN for 600s passed after
    superseding an earlier send-buffer failure.
  - [Linux `xdp` cell](runs/lan-1000-proof-xdp.md) at `534cce2e`: ran 1,000
    clients for 300s against a bare-metal server on its shared 10G uplink
    (`--xdp-uplink`); connection and payload delivery passed, while the merger's
    timing verdict failed. Windows `iocp` and `rio` LAN cells remain unmade in
    [docs/runs/lan-1000-proof.md](runs/lan-1000-proof.md).
- **AF_XDP dataplane and guest runs:**
  - [AF_XDP zero-copy qualification](runs/xdp-zero-copy.md) at
    `010c8167de062d24dafcdf9153577b2b8e078760`: **HOLD** — the available Intel
    X540-AT2 NIC refused `XDP_ZEROCOPY` bind, so zero-copy operation is not
    qualified on that hardware.
  - [XDP IPv4 and IPv6 max-size end-to-end run](runs/xdp-ipv6-end-to-end.md)
    at `3a04ed7a224b4567fc729b575b662fa275372dec`: passed inside a `virtme-ng`
    guest over a `veth` pair in copy mode (establishes guest/shim packet framing,
    not physical NIC throughput).
- **30-minute impaired loopback soaks (correctness under simulated latency,
  jitter, loss, and duplication; same-host loopback, not physical NIC proof):**
  - [Linux per-transport 30-minute soak](runs/linux-soak-30min-per-transport.md)
    at `adca4345bf963c7bcb5396ce5cd80a5111793533`: all five cells passed with
    link impairment applied (superseding earlier unimpaired runs).
  - [macOS per-transport 30-minute soak](runs/macos-soak-per-transport.md)
    at `31ad78624a5d46f873678ecaaeb884bdbe1af72f`: `portable` and `msgx` cells
    passed on loopback under applied impairment.
  - [Windows native (`iocp`, `rio`) soak](runs/windows-soak-native-server.md)
    at `3f900fa1254d938e50150bcb3386a5a0bb857e1a` and
    [Windows `portable` soak](runs/windows-soak-portable.md): 1,800s
    same-host loopback runs passed `--assert-flat`.
- **Preview-3 qualification:**
  - [Preview-3 qualification](QUALIFICATION-v1.0.0-preview-3.md): native
    Release unit and functional suites on Linux, macOS and Windows at the
    candidate head, the LAN proof and soak records it cites, and the gates
    still owed at tag time.
- **Historical and custom-capacity qualification records:**
  - [Preview-2 qualification](QUALIFICATION-v1.0.0-preview-2.md) at
    `c1a63ac978c0389f97e2d37e9ab61c8139191e34` /
    `2f88961a0b42713b832478df257d30e15c0944e6`: records the manual Linux, macOS,
    and Windows suite results for `v1.0.0-preview-2` and the items carried
    forward.
  - [10,000-client custom build qualification](QUALIFICATION-max-clients-10000.md)
    at `09395b079957938538af5e36c67479e938b1293b`: verifies that a custom
    `-DROCKETNET_MAX_CLIENTS=10000` build compiles warning-free and completes a
    bounded same-host loopback load test on Linux (`ROCKETNET_MAX_CLIENTS`
    remains 1,000 by default).

Preserve the established 1,000-player XDP configuration, including 4096-byte
frames. Follow the [Linux deployment guide](LINUX.md) and
[live XDP guide](LIVE-XDP.md) for host setup. Copy versus zero-copy depends
on the NIC, driver, kernel and deployment. Above 1,000 players, users must
research, stabilize and qualify their own configuration; no higher population
is guaranteed.

## What counts as qualification

Record the source revision, binary identity, build flags, platform, selected
transport, address family, topology, duration, workload and raw result. Preserve
failures and explain any rerun. A skipped or unavailable test is unrun, not a
pass. Synthetic output validates a result parser, not a network path.

Recheck the integrated release candidate after repairs land. Keep long stress,
fuzzing and performance campaigns in nightly or dedicated runs; ordinary unit
and functional feedback must remain under two minutes, ideally one minute.
Necessary test-only hooks belong behind `ROCKETNET_DEV` in private code, with
production builds checked separately.

This work continues until the required release gates pass. Neither this page nor
an individual review approves a production release. See [Credits](CREDITS.md)
for project authorship and sponsorship.
