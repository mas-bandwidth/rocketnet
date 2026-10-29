# Transport interface cost checks

These are CPU microbenchmarks for a native interface change, separate from the
NIC campaign in `PROTOCOL.md`. They do not certify packet delivery rate,
send/receive throughput, latency tails, or line-rate operation.

`transport_fill.cpp` uses only `rocketnet.h`. It establishes a real encrypted
loopback connection, then times server acquire/write/commit in batches of 128.
Update, flush and client drain are outside the timer. Every reservation and
commit must succeed; received payloads are checked. UDP may drop in the OS or
receiver, so received count is diagnostic and is not the denominator. The first
500 batches are discarded. One result is an aggregate fill cost per packet,
not a latency percentile. Select the transport, payload bytes, and measured batch
count as arguments. The client uses portable I/O.

Build the same source against each candidate's Release static library, with the
same compiler, flags and dependencies. For example on macOS:

```sh
c++ -O3 -DNDEBUG -std=c++11 -ffp-contract=off -I sdk \
  benchmarks/transport_fill.cpp build/librocketnet.a \
  -framework CoreFoundation -framework SystemConfiguration -o fill
./fill portable 64 2000
./fill portable 1192 2000
./fill portable 64 2000 plant
./fill portable 64 2000 drain
./fill portable 1192 2000 drain
```

The `plant` argument injects 4096 dependent volatile arithmetic steps per
packet. It must move the timing substantially; it is only an instrument check.

The `drain` argument times the client's receive cursor
(`rocketnet_client_transport_receive_packet`) over the batches the server
filled, and prints a second line: `drain_ns` is the mean steady-clock cost per
drained payload. The fill line is unchanged in drain mode, so one process run
reports both the send (acquire/write/commit) and the receive cursor costs over
the same traffic.
Alternate candidate order across repeated process runs. Report median and
min/max, and treat overlapping ranges as tied. Clear `ROCKETNET_*` environment
overrides when comparing the same configuration.

`driver_dispatch.cpp` isolates a start/finish pair using the actual native
operation-record layout against a direct-call control. The callback bodies live
in `driver_dispatch_fixture.cpp`; build without LTO so the compiler cannot
replace the indirect calls. Both paths check every callback executed. This uses
minimal synthetic callbacks, with no socket, encryption or allocator, and does
not substitute for measuring a real staged backend on its platform.

```sh
c++ -O3 -DNDEBUG -std=c++11 -ffp-contract=off -I sdk \
  benchmarks/driver_dispatch.cpp benchmarks/driver_dispatch_fixture.cpp -o dispatch
./dispatch
```

## 2026-09-08 macOS observation

Apple silicon, Apple clang 21.0.0, Release `-O3 -DNDEBUG`, libsodium 1.0.22.
Unpinned and sharing the machine with other work. Baseline `77d89fd` (IOCP before
the interface); candidate `dc1f40e` (the interface). Later `c27976d` changes only
Windows test instrumentation, an explicit include and documentation/build setup;
the measured macOS production packet path is unchanged.

Seven alternating process runs per candidate and payload; 256000 measured
packets each, after 64000 warm-up packets:

| Payload | Baseline ns/packet median [min, max] | Interface ns/packet median [min, max] |
| --- | --- | --- |
| 64 bytes | 952.141 [872.755, 1073.942] | 1001.016 [877.438, 1287.391] |
| 1192 bytes | 3240.674 [3222.359, 3418.514] | 3243.902 [3197.271, 3323.108] |

Both comparisons are tied at the observed variance. The small-packet median is
5.1% higher, so this is not evidence of zero cost or of an improvement. A planted
run measured 8096.361 ns/packet, outside both unplanted ranges.

Nine interleaved dispatch samples of ten million start/finish pairs each:

- Direct: median 2.158 ns/pair, range 2.139–2.200.
- Indirect plus capability checks: median 2.430 ns/pair, range 2.330–2.499.
- Paired difference: median 0.270 ns/pair, range 0.136–0.342.

The dispatch overhead is measurable here. Linux GSO/XDP and Windows IOCP need
real backend/hardware measurements before making performance claims for them.
Raw observations are in `transport_interface_cost_2026-09-08.json`.
