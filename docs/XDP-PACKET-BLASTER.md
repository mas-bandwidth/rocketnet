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

XDP owns the first five filter/type stages before the shared server protocol.
The other physical server transports publish the generic named counters.
Their native receive paths may reject a short or oversize datagram before the
shared protocol; `server-ingress-size-filter` is therefore the published
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
and application payload 1–1200 bytes. The plan prints the deterministic offer
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
Submitted, backpressure, not-ready, receive, and refusal counts remain separate.
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
