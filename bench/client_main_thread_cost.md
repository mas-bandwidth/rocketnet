# Client main-thread cost per frame

`client_main_thread_cost.cpp` is a public-API bench for the work a game's main
thread spends inside the client transport on one frame. It does not measure
throughput, packet delivery rate, or end-to-end latency.

It establishes a real encrypted loopback connection, then per frame writes P
payloads of 200 bytes with `rocketnet_client_transport_start_packet` and
`rocketnet_client_transport_finish_packet`, and times two calls with
`std::chrono::steady_clock`:

- `rocketnet_client_transport_send_packets`, the transport send walk;
- `rocketnet_client_transport_update`, the per-frame receive and protocol work.

The payload writes are outside both timers. After the timed calls, each frame
runs in lockstep: the server loops, each pass updating, draining what arrived,
sending one 200-byte reply per received payload and flushing, until the frame's
P payloads have arrived or a 0.5 s deadline passes, sleeping 0.1 ms between
passes. The pacing exists because on a transport with the client I/O thread,
`send_packets` is a hand-off and the I/O thread sends asynchronously; an
unpaced loop refills the Filling ring faster than one ring can be sent, and
`start_packet` then refuses on a full ring, as designed. The client then drains
its receive cursor to count `received`. All of that server, pacing and drain
work is outside both timed windows. The first 200 frames are
discarded. Each row below reports the median of three process runs of 2000
measured frames, with nearest-rank percentiles from the sorted per-frame
samples. `total` is the per-frame send plus update time.

The `plant` argument injects 4096 dependent volatile arithmetic steps inside
the timed send window, after `send_packets` returns. It is caller work, never a
production SDK branch. It adds about 3.3 us to the send window at every P; the
effect is clear at P = 1 and P = 8 and sits inside the run-to-run spread at
P = 32, where the send walk itself is near 100 us.

## Limits

- Loopback only, one process, one client and one server. The client's receive
  thread shares the machine, so a run can be preempted and its p99 and max
  reflect host scheduling as well as transport work.
- The wall clock is used; this is a bench, never a unit test. Compare repeated
  runs, and treat overlapping ranges as tied.
- Received count is diagnostic. UDP may drop in the OS or receiver, so it is
  not the denominator.
- On a transport with the I/O thread, the meter waits for each frame's payloads
  to reach the server, so it measures per-frame cost, not a producer running
  faster than the I/O thread drains.
- The transport argument selects the server and client transport. The BASE numbers
  use `portable`.

## Commands

```sh
make
./bin/client_main_thread_cost portable 1 2000
./bin/client_main_thread_cost portable 8 2000
./bin/client_main_thread_cost portable 32 2000
./bin/client_main_thread_cost portable 1 2000 plant
./bin/client_main_thread_cost portable 8 2000 plant
./bin/client_main_thread_cost portable 32 2000 plant
```

One line is printed:

```
client_main_thread_cost transport=<d> packets_per_frame=<P> frames=<F> send_us_p50=<> send_us_p99=<> send_us_max=<> update_us_p50=<> update_us_p99=<> update_us_max=<> total_us_p50=<> total_us_p99=<> total_us_max=<> received=<n>
```

Any `CHECK` failure exits 1 and names the failed line.

## BASE numbers

Measured on Linux kernel `7.0.0-38-generic` with g++ 15.2.0, Release build,
from the tree at commit `8985b00fcf7d2062ec32dde8a533a03fc823a124` (BASE) with
this bench added; the card does not change the SDK. Medians of three runs,
2000 frames each, portable transport.

| P | plant | send p50 | send p99 | send max | update p50 | update p99 | update max | total p50 | total p99 | total max |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | no | 3.226 | 7.253 | 12.693 | 4.288 | 152.614 | 224.847 | 7.985 | 157.552 | 231.971 |
| 1 | yes | 6.532 | 16.130 | 20.268 | 4.318 | 131.785 | 259.713 | 10.900 | 138.467 | 266.425 |
| 8 | no | 25.368 | 63.097 | 120.694 | 5.540 | 266.375 | 543.270 | 30.938 | 292.664 | 574.157 |
| 8 | yes | 28.733 | 42.810 | 127.908 | 5.740 | 248.121 | 475.504 | 34.394 | 280.762 | 562.214 |
| 32 | no | 102.481 | 121.515 | 283.006 | 124.301 | 304.937 | 666.428 | 226.071 | 406.926 | 768.228 |
| 32 | yes | 104.344 | 115.394 | 299.086 | 119.532 | 306.450 | 635.511 | 224.858 | 412.366 | 763.569 |

All values are microseconds. The large update maxima come from occasional
preemption and from the receive thread's 1 ms poll delivering a burst of
datagrams to one update; the p99 is not a transport-only tail.
