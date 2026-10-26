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

See [tests](tests/README.md), [fuzzing](fuzz/README.md),
[OSS-Fuzz integration](oss-fuzz/README.md), and [TLA+ models](tla/README.md)
for the existing tools. A model result needs a correspondence review against
its implementation; a configured fuzz job needs execution evidence.

## Red-team engagement: October 2026

Alex led protocol red teaming, with independent review of the retained source
and execution evidence. The work exercised malformed traffic, handshake and
replay behavior, session lifetime, hostile peers and recovery after adverse
traffic. It included native socket transports and Linux guest AF_XDP runs.

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

## Two separate load questions

**Can a game server sustain the intended 1,000-player workload?** Run the server
on its own machine and generate representative player traffic remotely. Check
received payloads, connection progress, loss and recovery, memory use, and frame
latency under that workload. A load generator simulates the clients; it need not
run 1,000 complete game clients. The [split load guide](docs/LOAD_SPLIT.md)
explains the dedicated-server topology. The
[same-process load harness](docs/LOAD.md) is useful for correctness, but its
colocated timings do not qualify dedicated-server performance.

**Can a transport sustain a 10 Gb/s packet workload?** This is a separate
physical-link measurement. The [AF_XDP packet blaster](docs/XDP-PACKET-BLASTER.md)
is an external load-generation tool, not a public client API. Measure actual
transmitted and received traffic, directional rate and packet loss; a requested
send rate is not an achieved wire rate. Hostile-traffic tests must also check
the expected rejection counters and subsequent valid traffic, not just that
the server remains alive.

The planned blaster qualification covers the non-portable server transports:
`xdp`, `gso`, `io_uring`, `msgx`, `iocp` and `rio`. Portable remains part of
correctness and security testing, but is not expected to meet the 1,000-player
performance target. Profile support or a successful tool build is not a passed
load test. Physical 10 Gb/s and final integrated 1,000-player qualification
remain outstanding in this record.

Preserve the established 1,000-player XDP configuration, including 4096-byte
frames. Follow the [Linux deployment guide](docs/LINUX.md) and
[live XDP guide](docs/LIVE-XDP.md) for host setup. Copy versus zero-copy depends
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
an individual review approves a production release. Thanks to the
[people and models](CREDITS.md) doing the careful work behind those results.
