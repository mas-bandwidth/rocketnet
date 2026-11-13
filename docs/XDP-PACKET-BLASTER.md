# XDP packet blaster

`xdp_packet_blaster` is a benchmark-only Linux tool for a reserved RocketNet
server bench. It is built only with `ROCKETNET_BUILD_TOOLS=ON` and
`ROCKETNET_XDP=ON`; it is not installed and adds no SDK API or production
client feature. Its UMEM frame size is the existing 4096-byte RocketNet XDP
geometry.

The tool has two modes:

- `throughput` connects protocol-valid synthetic clients using distinct real
  connect tokens. It uses `rocketnet_client_protocol` for handshake, session,
  sequence, encryption, replay, and transport ACK state, and one `rn_endpoint`
  region per client for CONFIG, reliable ACK/resend state, and application
  records.
- `robustness` derives one named mutation at a time from a successfully sealed
  authenticated payload. `tools/xdp_packet_blaster_cases.json` records the
  existing schema/corpus source, expected server filter/parser/refusal counter,
  exact expected counter delta, whether its transport sequence must already be accepted or remain fresh,
  and the required successful fresh-payload recovery. The bad-tag case uses a
  newly sealed sequence that was never sent valid, so replay refusal cannot
  mask the AEAD-open counter. The replay case deliberately uses an ACKed
  sequence. Schema version, mutation argument, and sequence state are a closed
  canonical contract; the runtime refuses a manifest that changes any of them.
  Process survival alone is not a passing oracle.

Robustness mode also requires
`--server-transport-profile xdp|gso|io_uring|msgx|iocp|rio`. This is the
operator's declaration of the controlled server build, not runtime transport
autodetection. `portable` is excluded from this physical campaign and
`loopback` is an in-process, nonphysical transport. The profile selects one
named server counter from the manifest for each canonical operation; it does
not create a client/server transport matrix and it does not prove that the
server observed the packet or changed that counter.

The manifest's seven-by-six oracle mapping is:

| Operation | `xdp` | `gso` | `io_uring` | `msgx` | `iocp` | `rio` |
| --- | --- | --- | --- | --- | --- | --- |
| `truncate` | `XDP_KERNEL_DROPPED_TOO_SMALL` | `PREFILTER_TOO_SHORT` | `PREFILTER_TOO_SHORT` | `PREFILTER_TOO_SHORT` | `PREFILTER_TOO_SHORT` | `PREFILTER_TOO_SHORT` |
| `oversize` | `XDP_KERNEL_DROPPED_TOO_LARGE` | `PREFILTER_TOO_LARGE` | `PREFILTER_TOO_LARGE` | `PREFILTER_TOO_LARGE` | `PREFILTER_TOO_LARGE` | `PREFILTER_TOO_LARGE` |
| `wrong_epoch` | `XDP_KERNEL_DROPPED_WRONG_EPOCH` | `PREFILTER_WRONG_EPOCH` | `PREFILTER_WRONG_EPOCH` | `PREFILTER_WRONG_EPOCH` | `PREFILTER_WRONG_EPOCH` | `PREFILTER_WRONG_EPOCH` |
| `wrong_filter` | `XDP_KERNEL_DROPPED_WRONG_FILTER` | `PREFILTER_WRONG_VALUE` | `PREFILTER_WRONG_VALUE` | `PREFILTER_WRONG_VALUE` | `PREFILTER_WRONG_VALUE` | `PREFILTER_WRONG_VALUE` |
| `reserved_type` | `XDP_KERNEL_DROPPED_INVALID_TYPE` | `RESERVED_TYPE` | `RESERVED_TYPE` | `RESERVED_TYPE` | `RESERVED_TYPE` | `RESERVED_TYPE` |
| `replay` | `PAYLOAD_ALREADY_RECEIVED` | `PAYLOAD_ALREADY_RECEIVED` | `PAYLOAD_ALREADY_RECEIVED` | `PAYLOAD_ALREADY_RECEIVED` | `PAYLOAD_ALREADY_RECEIVED` | `PAYLOAD_ALREADY_RECEIVED` |
| `bad_tag` | `PAYLOAD_DID_NOT_OPEN` | `PAYLOAD_DID_NOT_OPEN` | `PAYLOAD_DID_NOT_OPEN` | `PAYLOAD_DID_NOT_OPEN` | `PAYLOAD_DID_NOT_OPEN` | `PAYLOAD_DID_NOT_OPEN` |

XDP owns the first five filter/type stages before the shared server protocol,
and its kernel drops count only in its own kernel counters. The other physical
server transports publish the generic named counters. As
[RN-1 t10](../standard/TRANSPORT.md#t10) rule 2 permits, their native receive
paths may drop a datagram shorter than the smallest packet a server receives
or longer than the receive ceiling before the shared checks, counted in
`PREFILTER_TOO_SHORT` or `PREFILTER_TOO_LARGE` and not in `PACKETS_RECEIVED`;
`server-ingress-size-filter` is therefore the published
counter-family stage, not a claim that one shared function executed. Wrong
epoch/value and reserved-type packets reach the shared server checks.
Replay and authenticated-open refusals remain shared payload-stage oracles for
all six profiles. The case order, mutation bytes, accepted/fresh preconditions,
expected deltas, and recovery traffic are unchanged by profile selection.

Robustness mode is IPv4-only. Its exact 1453-byte RocketNet mutation is one
byte over the production receive ceiling but still forms a 1495-byte Ethernet
frame inside an IPv4 MTU of 1500. A narrow tool-local framing path exists only
for that canonical case; all valid traffic uses the production send-copy
helper. Throughput mode continues to support both private IPv4 and IPv6 ULA.
Robustness is ACK-driven rather than rate-paced: dry-run reports the case and
actual datagram-copy counts, one accepted baseline, and one ACKed recovery per
case, and explicitly marks `--rate-pps` ignored for that mode.

## Controlled server contract

This is not a general packet generator. The operator owns both endpoints on a
private, isolated L2 bench. The server mints at least one distinct token per
synthetic client; every decoded token must select the exact `--server`
address/port. Both ends use `rocketnet_message_default_config`; dry-run prints
the exact bounded arena requirement for the selected client count, and the
remote CONFIG must match the tool's production endpoint configuration.

`--direction c2s` schedules application sends from the tool. `s2c` and `both`
are server workload contracts: the controlled server must send application
records of exactly the selected payload size and, when the size is at least
four bytes, place the `RNPB` magic in the first four bytes. The tool consumes
those records through the real endpoint
and reports invalid bodies. Selecting `s2c` does not make the generator create
downstream load by itself.

The interface must have no XDP program attached, its explicit `--source`
address and `--source-mac` must belong to it, and `--next-hop-mac` must be the
distinct nonzero unicast address of the same-L2 server (or explicitly
provisioned next hop). RSS/queue routing is an
external prerequisite: the tool accepts only queue 0 on an already configured
one-queue interface (one combined queue and no split queues, or one RX plus one
TX split queue and no combined queue) and never changes RSS, NIC, link, route,
hugepage, SMT, or system settings.

## Safe planning

Dry-run is the default and is mandatory before a later qualification run. It
parses and validates the complete plan but opens no socket, starts no thread,
attaches no program, and transmits nothing:

```sh
./bin/xdp_packet_blaster --dry-run \
  --interface BENCH_IFACE --queue 0 --xdp-mode native \
  --source 10.40.0.2:42000 --server 10.40.0.1:40000 \
  --source-mac 02:00:00:00:00:02 --next-hop-mac 02:00:00:00:00:01 \
  --packet-filter b7590100 --token-file /secure/path/tokens.txt \
  --manifest tools/xdp_packet_blaster_cases.json \
  --clients 1 --duration 10 --rate-pps 1000 --payload-bytes 64 \
  --source-port 42000 --seed 0x1234 --mode throughput \
  --direction c2s --bind auto --owned-bench-confirm I-OWN-THIS-BENCH
```

Targets are limited to RFC 1918 IPv4 or IPv6 ULA source and server addresses.
Duration is 1–600 seconds, clients 1–1000, offered rate 1–20,000,000 packets/s,
and application payload 1 to `RN_BLASTER_MAX_PAYLOAD_BYTES` (1411) bytes. The plan prints the deterministic offer
count, fixed memory geometry, and all non-secret inputs before `--execute` can
attach. Tokens, keys, and packet bodies are never logged; decoded tokens are
wiped when planning or execution ends. Execution allows at most 30 seconds for
every client to connect and exchange matching CONFIG records; only then does
the requested workload duration begin. Offer ordinal zero is due at workload
time zero and the last ordinal is strictly capped by the planned count.

No implementation test for this tool should pass `--execute`. Physical NIC
traffic belongs only in the separately admitted qualification card.

For a robustness dry-run, change the mode to `robustness` and declare the
controlled server build, for example `--server-transport-profile gso`. The
plan and each `CASE` row repeat the declared profile and selected oracle, while
also reporting `runtime_autodetect=false` and `counter_observed=false`.

## Ownership and accounting

The 4096 UMEM frames are split permanently: frames `[0,2048)` are TX-owned and
return only through the completion ring; `[2048,4096)` are RX-owned and return
only through the fill ring. Backpressure never transfers ownership between
those sets. The 4096-entry fill ring is twice the RX-frame count, preserving a
return slot beyond the kernel's cached consumer batch; TX, RX, and completion
rings remain 2048 entries.

Output distinguishes:

- application bytes admitted to the client endpoint and application bytes
  delivered by the receive endpoint;
- RocketNet protocol datagram bytes submitted to AF_XDP;
- Ethernet frame bytes visible to AF_XDP (Ethernet header through payload,
  excluding FCS); and
- physical-wire occupancy: 8-byte preamble/SFD, the Ethernet frame plus
  4-byte FCS padded to the 64-byte minimum, and 12-byte inter-frame gap.

The offer counter is a workload schedule, not a transmit-success counter.
Submitted, backpressure, not-ready, unsent, receive, and refusal counts remain
separate. Every packet the tool writes is its own flush: it opens the
endpoint's write pass first, as the SDK's send path does. `unsent` counts
offers the endpoint queued but wrote no packet for (the update's
`packets_per_update` spent); they leave with a later packet, or never. The
run fails (exit 1, naming the counter) when `payload_errors`,
`ownership_errors`, `descriptor_refused` or `rx_wrong_thread` is not 0;
`rx_refused` is reported only, since it also counts other hosts' frames on the
shared queue.
Robustness expectations must be joined to the controlled server's named
counter receipt; the local case list is not evidence that the server observed
the mutation.

## Attach and cleanup contract

At execution time the tool records kernel, driver, requested and actual
copy/zero-copy mode, the empty pre-attach XDP IDs, and active program IDs. The
generic libxdp ingress uses a tool-local XSK configuration with
`libbpf_flags=0`; it does not reuse the server filter or its program-load
inhibition flag. `auto` tries zero-copy first and only then copy mode.

Cleanup deletes the XSK before UMEM, restores `RLIMIT_MEMLOCK`, and queries all
XDP attach modes again. It never performs a blind interface detach, because a
replacement may belong to another process. Any residual program makes the run
fail and is reported. Before deleting the XSK, every initialized production
client protocol takes its normal disconnect path: connected sessions emit the
bounded disconnect burst and every attempt resets its token, session keys, and
ephemeral secret. A valid qualification receipt therefore includes empty
pre-IDs, active IDs, empty post-IDs, selected bind mode, zero skipped cases,
server counter/recovery receipts, and an exit status of zero.

## Running wide

`--threads N` (1 to 32, default 1) sends from N threads, each with its own
AF_XDP socket and UMEM on its own NIC queue, created after the thread pins
itself. Each thread owns a slice of the clients and the whole send path for
them: no lock, batched TX (one wakeup per 64 descriptors at most), counters
summed at the end. `--threads 1` is the old tool: queue 0, the one-queue RSS
rule, the same plan, result fields and exit status. `--cpus LIST` (for
example `1-6` or `1-3,7`) names exactly one distinct CPU per thread.

Replies must come back to the thread that owns the client. With N above 1 the
tool reads the port's Toeplitz key and indirection table (ETHTOOL_GRSSH), and
refuses unless the UDP hash of the source's family covers both addresses and both
ports (`ethtool -N <if> rx-flow-hash udp4 sdfn`, or `udp6` for an IPv6 ULA
source). It then gives each client a source
port whose reply (server address and port to client address and port) hashes
to its owner's queue. The plan prints only the first source port
(`source_ports=P-rss_assigned`): the ports used run past the client count.
`RESULT` carries `rx_wrong_thread`, which must be 0: any reply steered to the
wrong thread fails the run.
Every `RESULT_THREAD` line shows one thread; `RESULT_RATE` shows the submitted
rate and the Gb/s on the wire.

Placement rules, measured on ixgbe (X540):

- Keep the sender threads, their NIC queues and the NIC on one NUMA node.
- The blaster's own XDP attach resets the port and rebuilds its vectors on
  CPU 0. Pin `<if>-TxRx-k` after the attach to a CPU that is not a sender
  thread's CPU (the SMT sibling of thread k's core works). With the queue
  interrupts on the sender CPUs, softirq takes 85 to 95% of those CPUs.
- On the server side, run `rocketnet-provision <if> N repin` after the
  server's attach (DEPLOYING.md, step 4a).
- 802.3x pause is on by default on both ends. A receiver that cannot keep up
  pauses the link; it does not drop. Read `tx_flow_control_xoff` on both
  ends to see which side is the slow one.

### Measured (2026-11-12)

The setup:

- Sender: vision (64 CPUs, SMT on, NIC on node 0). Six threads on CPUs 1 to 6,
  with `TxRx-0..5` on CPUs 33 to 38.
- Server: hulk (32 CPUs, `isolcpus=1-15`). `tests/client_server/server`
  echo, `--num-xdp-queues 6`, XDP workers on CPUs 1 to 6, main thread on
  CPU 7, `--update-hz 2000`, queue vectors repinned.
- Link: 10G direct cable, 256 clients, zero-copy on both ends.

Wire Gb/s includes preamble and IFG and is read from the NIC counters. Line
rate at 64 B payload is about 7.49 Mpps.

| payload | to server | back to sender | XOFF sent by | limit |
| --- | --- | --- | --- | --- |
| 64 B | 152 kpps, 0.27 Gb/s (2% of line pps) | 28 kpps | both | server |
| 1200 B | 165 kpps, 1.69 Gb/s (17%) | 157 kpps, 1.64 Gb/s | hulk only | server |

The sender is not the limit; the server's main thread is. It is the only
thread that opens and seals packets, and at 1.7 Gb/s it is busy on CPU 7 back
to back in loops of about 85 ms, two thirds of that in chacha20 and poly1305.
Nothing else runs on that CPU. While it is busy, the XDP workers' handoff
fills, the workers stop draining the NIC, and the server's NIC sends pause
frames. Turning off the fair server on the worker CPUs (0.19 Gb/s) or setting
`kernel.sched_rt_runtime_us=-1` (0.94 Gb/s) made it worse: on kernel 7.0
the NIC's softirq work on a worker CPU runs in `ksoftirqd`, which gets that
CPU only through the fair server's 50 ms a second. The library's server
transport has not been changed for throughput.

With the echo loop at its default 60 Hz, the same placement gave 0.94 Gb/s
at 1200 B. At 1412 B an unreliable message no longer fits one record and is
promoted to a numbered message (`MESSAGE_PROMOTED_UNRELIABLE`). The test
server echoes the client's packet payload back raw, so the client receives
its own send numbers. Once those run a window ahead of its receive window,
the client refuses them as `NUMBER_OUTSIDE_WINDOW` and ends with disconnect
reason -2 (`PROTOCOL_VIOLATION`). It happened only under load, and it came
from the echo, not from the library. The test server's echo now returns
only what the server could have sent byte for byte (unreliable records,
control records, raw transport bytes) and refuses a numbered message by
name (`server echo refused ...`, tests/echo_payload.h), and the blaster's
largest payload is 1411 B, the largest message sent as one unreliable
record. An echo cell measures the transport echo of one unreliable record
per message; it never measures numbered messages.
A failed endpoint now prints its nonzero `MESSAGE_*` counters.
