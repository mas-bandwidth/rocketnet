# Packet-lease cost and portable allocator footprint

This measures the valid public packet API with the current ownership checks and
a diagnostic control that bypasses only `insert` and `resolve`. Both variants
retain the same table storage, allocation/lifetime code, history, transport,
protocol, crypto, calls and workload. The control does not issue process-unique
leases or enforce ownership. It is never a supported build configuration and
must not be used by an application. Its sole use is isolating the cost of the
current implementation; it is not an equivalent replacement.

## Reproduce

On macOS or Linux with the ordinary SDK build dependencies, CMake, Python 3.12+
and a C++11 compiler, from a committed checkout:

```sh
python3 benchmarks/lease_cost.py --work build/lease-cost-run
```

The destination must be new. The controller archives the exact Git HEAD into
two isolated source trees, edits only the control's two method bodies, builds
Release static libraries, and links the same harness against each. It clears
`ROCKETNET_*` overrides and records the commit, compiler, platform, sodium
version and both header hashes. All timing runs are sequential, with pair order
alternated over five samples. It retains partial observations on failure; a
successful run also requires an obvious response to the planted slowdown.

The harness establishes a real portable loopback connection, then measures
start/write/finish, start/write/abort, or start/eight owned-payload accesses/write/
abort. A batch has 64 payloads at 64 or 1192 bytes. It warms up 100 batches and
measures 300. The 4-thread case uses persistent workers; its timing includes the
wake/join barrier. The 1-thread case calls directly. Every start and finish must
succeed, every owned view must match the acquired payload, every returned context
must be consumed, and allocator accounting must return to zero at destruction.
Received application payloads are checked byte for byte. The finish cases require
some delivery, but UDP receipt is diagnostic, never the timing denominator.

Update, flush, receive validation, setup and teardown are outside the timer.
These are aggregate fill costs per logical payload, not latency percentiles,
wire packet throughput, 1000 independent connections, or a saturation test.
Four workers targeting one connection also contend on its existing sequence and
ACK bookkeeping; this is not a model of four unrelated clients.

## Observation — 2026-09-08

Apple silicon on a shared machine, unpinned, with other tasks active.
Source `834bccf9e09392802f0f2a7e116bb694bee7459c`, Apple clang version 21.0.0 (clang-2100.1.1.101), libsodium 1.0.22.
Release `-O3 -DNDEBUG -ffp-contract=off`, no LTO. Raw observations and source/header
hashes are in [lease_cost_2026-09-08.json](lease_cost_2026-09-08.json).
That retained file supplements the measured controller output with an independently
computed `harness_sha256`, separately run `memory_profiles`, and a completed-run
`status`; the measured 834bccf controller did not emit those fields. Later
controller revisions write status themselves. `subsequent_setup_check` is a
separately added record of a later diagnostic, not output a fresh run reproduces.
Each cell is the median [minimum, maximum] ns per logical payload over five
process runs. Overlapping ranges do not establish a stable difference here.

| Endpoint | Operation | Bytes | Workers | Checks | Diagnostic control |
| --- | --- | ---: | ---: | ---: | ---: |
| server | finish | 64 | 1 | 432.17 [402.10, 495.46] | 422.41 [401.46, 438.92] |
| server | finish | 64 | 4 | 467.76 [411.25, 957.10] | 429.48 [360.04, 1112.32] |
| server | finish | 1192 | 1 | 2432.40 [2299.48, 2504.19] | 2402.32 [2260.62, 2464.34] |
| server | finish | 1192 | 4 | 1097.28 [951.99, 2041.91] | 1024.45 [953.75, 1784.54] |
| server | abort | 64 | 1 | 15.51 [14.79, 33.90] | 11.27 [10.00, 32.14] |
| server | abort | 64 | 4 | 303.34 [236.39, 762.30] | 229.61 [144.30, 338.04] |
| server | abort | 1192 | 1 | 31.97 [27.74, 42.87] | 25.35 [24.54, 52.23] |
| server | abort | 1192 | 4 | 345.06 [216.03, 480.05] | 300.77 [190.11, 411.23] |
| server | access | 64 | 1 | 61.55 [36.39, 129.67] | 20.00 [17.78, 55.82] |
| server | access | 64 | 4 | 271.04 [219.14, 379.03] | 331.80 [233.77, 841.39] |
| server | access | 1192 | 1 | 54.39 [49.53, 115.35] | 34.45 [30.15, 59.60] |
| server | access | 1192 | 4 | 266.62 [225.96, 830.91] | 342.92 [202.02, 374.68] |
| client | finish | 64 | 1 | 587.18 [548.64, 614.14] | 536.45 [523.14, 608.97] |
| client | finish | 64 | 4 | 662.86 [423.15, 996.68] | 501.84 [383.16, 955.55] |
| client | finish | 1192 | 1 | 2504.43 [2382.59, 2549.27] | 2475.87 [2274.41, 2538.52] |
| client | finish | 1192 | 4 | 1498.53 [966.85, 1972.69] | 996.79 [958.54, 2083.83] |
| client | abort | 64 | 1 | 13.98 [11.91, 42.79] | 7.40 [6.68, 7.62] |
| client | abort | 64 | 4 | 275.48 [196.83, 716.14] | 246.20 [192.75, 624.64] |
| client | abort | 1192 | 1 | 27.08 [26.08, 32.01] | 21.72 [20.33, 22.89] |
| client | abort | 1192 | 4 | 376.77 [212.62, 897.76] | 274.69 [174.79, 843.40] |
| client | access | 64 | 1 | 36.51 [32.13, 50.30] | 15.32 [14.85, 34.70] |
| client | access | 64 | 4 | 369.42 [258.81, 645.26] | 357.56 [280.79, 690.88] |
| client | access | 1192 | 1 | 55.51 [50.53, 129.72] | 31.15 [28.02, 71.85] |
| client | access | 1192 | 4 | 380.37 [231.31, 647.37] | 368.18 [302.76, 568.94] |

The server's single-thread 64-byte finish median was 432.17 ns with checks
and 422.41 ns in the control: 9.76 ns (2.31% relative to the control).
The finish ranges overlap in this repeat, so the shared-host measurement does
not establish a stable net finish advantage. In this recorded repeat, the
single-thread client abort ranges separate: at 64 bytes, medians are 13.98 vs
7.40 ns, a 6.58 ns increase (89% relative to the control). An independent
same-host reviewer rerun found overlapping 64/1192-byte abort ranges, so that
separation is not a stable cross-run result. Neither overlapping ranges nor
these local medians establish zero cost or a NIC throughput result.
The access case deliberately repeats eight views and reports their combined
cost with start/write/abort; do not read its ratio as ordinary packet throughput.

The planted 4096-step dependent caller workload measured 6704.89 ns
with checks and 6688.22 ns in the control, well above either
ordinary range. This checks timing sensitivity, not a library performance target.

## Storage and current disposition

`sizeof` the unchanged table is 8,388,672 bytes per server and 65,600 per client
on this target. Both benchmark builds retain those bytes. The timed connection
uses two configured server clients and 1416-byte maximum payloads; its observed
allocator totals are TODO-NUMBER bytes for the server and TODO-NUMBER for the client.

`transport_memory.cpp` supplies a separate constructor/destructor observation:

```sh
c++ -O3 -DNDEBUG -std=c++11 -ffp-contract=off -I sdk \
  benchmarks/transport_memory.cpp build/librocketnet.a \
  -framework CoreFoundation -framework SystemConfiguration -o build/transport-memory
./build/transport-memory 1000 1192 portable
```

Use `-lbpf -lxdp -pthread` instead of the frameworks on Linux.

| Configured clients | Max payload bytes | Live allocator bytes after create |
| ---: | ---: | ---: |
| 1 | 1192 | TODO-NUMBER |
| 2 | 1192 | TODO-NUMBER |
| 1000 | 1192 | TODO-NUMBER |

Each profile used the portable server, 40 live allocations, and the same peak
as post-create storage. Every profile returned to zero on destroy. These totals
include RocketNet's alignment/ownership overhead, and exclude kernel socket
buffers, OS thread stacks, libc bookkeeping, observer overhead and application
memory. They are observed combined SDK heap storage, not a pre-allocation API or
an estimate that certifies a host can afford a configuration. XDP and IOCP
geometry are different and remain platform measurements.

The current release checks stay enabled. The public C API already promises
process-unique leases and refusal of stale, modified or wrong-owner contexts
before frame access. The measured diagnostic does not preserve those guarantees.
A future optimization must retain the promised behavior or have an explicit
compatibility decision; these numbers do not justify silently weakening it.
No history, cadence, resource-budget default, ABI or C# API changes accompany
these measurements. Hardware line-rate validation remains #167/#168; broader
memory estimation remains #155.

## Subsequent setup diagnostic

A later one-sample controller check at `876548d` stopped after 67 successful
rows when one portable client/server handshake missed its 10-second setup
deadline. It retained `status: running` and exited nonzero; it did not certify
an incomplete timing matrix. The same case passed immediately afterward, and
100 alternating checked/control setup-and-access probes with added state/counter
diagnostics all passed. The cause remains unresolved; this observation is
recorded with the existing unreproduced portable-run evidence in #122, without
claiming the same root cause. The completed five-pair data above are retained.
The harness now prints client state and nonzero endpoint counters on setup
failure; it does not lengthen the deadline or retry failed samples silently.
