# Does decrypt have to leave the main thread? Measured

The gate for the client transport is total main-thread time per frame:
**total_us_p99 at most 100 us at P = 32** (docs/CLIENT-IO.md, The gate). The
I/O thread design in docs/CLIENT-IO.md takes the send walk and the receive
mutex off the main thread and leaves in `rocketnet_client_transport_update` the
per-reply work: the checks, the decrypt and the protocol. This note measures
where that work goes at P = 1, 8 and 32, at `sprint/rocketnet-proof` `b0b4527`
(the landing of rocketnet-client-io-design), with scratch timers that are not
committed, and decides what has to leave the main thread.

**The answer.** Decrypt has to leave the main thread, but it is not the
largest part and it is not first. At BASE the largest part of update at P = 32
is the ack block walk: each reply's ack block restates the whole received run
(up to the 4096-sequence window) and `rocketnet_ack_apply_run` visits every
sequence it names, acknowledged or not, 4057 visits per reply. That walk is
397 us of update's 548 us median at P = 32; decrypt is 91 us. With the walk
removed (a scratch build), decrypt is 85 of 137 us at the median and its own
p99 is 261 us; one open of a 200 to 240 byte body costs 1.8 to 2.4 us on this machine, so
32 of them are 59 to 75 us before anything else runs. The order is: the walk, then
the per-frame ack statistics (17 to 24 us at every P), then the I/O thread cards,
then decrypt on the I/O thread before publish.

## Measured

### The builds

Four builds of the same tree, run interleaved in one session (base, lock,
outside, floor; then the next P; three rounds):

- **base**: BASE as it is, with the meter `bench/client_main_thread_cost.cpp`
  unchanged.
- **lock**: BASE plus the scratch timers below.
- **outside**: lock plus the receive recipe of rocketnet-client-receive-outside-lock
  in `rocketnet_client_receive_packets` (the driver's receive runs with the
  mutex released; the mutex is held only to check for room and to append one
  datagram), so the receive thread's hold does not show up as update's wait.
- **floor**: outside plus a scratch acknowledged floor in
  `rocketnet_ack_endpoint_process_block`: every run is clamped above the
  highest sequence below which every held sent record is acknowledged, and the
  floor advances over acknowledged records after each block. It is a bench
  approximation of a cheap block walk on a lossless path, not the design (see
  If it does not).

### The timers

`std::chrono::steady_clock` around each part, summed per frame, nearest-rank
p50 and p99 over the 2000 measured frames of each run:

| part | what it times |
|---|---|
| swap | `rocketnet_client_update_swap_receive_buffers`: the mutex acquire and the index flip, so the lock wait |
| checks | from the top of each datagram's iteration in update's loop to the AEAD call: the stats counters, the address compare, the prefilter, the type and direction checks, the per-type wire floor, the state check, sequence reconstruction and the replay check |
| decrypt | `rocketnet_open_sealed_packet` in `rocketnet_client_open_session_packet` (`crypto_aead_chacha20poly1305_ietf_decrypt`) |
| protocol | the rest of the per-datagram loop: the replay advance, the ack block read, `rocketnet_ack_endpoint_record_received`, `rocketnet_ack_endpoint_process_block`, the payload copy, the loss and jitter trackers |
| block walk | `rocketnet_ack_endpoint_process_block` alone (inside protocol) |
| protocol update, protocol send | `rocketnet_client_protocol_update`, and the driver send of its packet inside update |
| stats | `rocketnet_client_update_stats` |
| ack stats | `rocketnet_ack_endpoint_update_stats` alone (inside stats) |
| rest | update minus all of the above |
| update CPU | `CLOCK_THREAD_CPUTIME_ID` around update: the main thread's CPU time, without time it spent descheduled |
| block-walk visits | the change of `g_ack_visit_count` (`sdk/rocketnet_ack.cpp`) across update |

The gate columns are computed per frame before the percentile: "no send, no
lock" is update minus protocol send minus swap (what the I/O thread design
leaves in update), then minus decrypt, then minus the block walk. The timers
cost little: base against lock at P = 32 is 551 against 556 us at the median,
at P = 1 20.6 against 19.1 us.

### The machine

x86-64, 32 cores with 2-way SMT (64 hardware threads), 3.0 GHz maximum, 64 MiB
L3, 125 GiB, Linux 6.8.0-142-generic, g++ 13.3.0, Release build
(`cmake -DCMAKE_BUILD_TYPE=Release`, `cmake --build build -j8`), portable
driver, 2000 measured frames per run after 200 discarded.

The machine was not quiet, and no quiet Linux member was available in this
attempt: the one-minute load average was 155 to 197 during set 1 and 96 to 138
during set 2, and each
run's `uptime` is in its raw line. The medians are robust to that; the wall
p99 and max carry preemption, which is why update's CPU-time p99 is printed
beside it. For scale, docs/CLIENT-IO.md's quiet bench measured BASE update p50
at P = 32 as 366 to 418 us; here the base build reads 551 us (set 2) and
568 us (set 1), so this machine under
this load reads about 1.3 to 1.6 times slower at the median.

### Set 2: all four builds, three interleaved rounds

Medians of the three runs; microseconds unless named.

The meter's own columns:

| build | P | load (1 min) | send p50 | send p99 | update p50 | update p99 | total p50 | total p99 | update CPU p50 | update CPU p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| base | 1 | 109 | 12.894 | 32.832 | 20.599 | 174.561 | 33.322 | 195.050 | - | - |
| lock | 1 | 109 | 11.311 | 29.856 | 19.126 | 174.590 | 30.508 | 192.516 | 20.709 | 135.404 |
| outside | 1 | 109 | 12.624 | 25.539 | 20.559 | 143.872 | 33.785 | 158.119 | 22.252 | 144.440 |
| floor | 1 | 109 | 13.556 | 26.991 | 20.740 | 83.358 | 34.466 | 102.255 | 22.412 | 82.635 |
| base | 8 | 109 | 100.259 | 1198.827 | 152.380 | 3276.273 | 250.292 | 3433.691 | - | - |
| lock | 8 | 106 | 100.792 | 238.503 | 152.378 | 1398.697 | 254.160 | 1731.789 | 154.309 | 831.690 |
| outside | 8 | 106 | 102.615 | 154.163 | 142.279 | 837.164 | 237.119 | 981.907 | 144.131 | 778.680 |
| floor | 8 | 106 | 103.076 | 190.841 | 28.815 | 265.903 | 141.970 | 406.449 | 31.148 | 249.909 |
| base | 32 | 103 | 397.332 | 3390.129 | 551.491 | 3853.661 | 965.893 | 4331.925 | - | - |
| lock | 32 | 104 | 390.328 | 3483.878 | 555.831 | 4537.809 | 966.487 | 6038.920 | 551.695 | 1594.290 |
| outside | 32 | 105 | 391.942 | 1567.145 | 547.922 | 3164.777 | 948.834 | 3916.189 | 549.010 | 1320.847 |
| floor | 32 | 102 | 390.248 | 479.267 | 137.680 | 407.652 | 531.295 | 830.460 | 139.592 | 402.304 |

The parts of update:

| build | P | replies p50 | replies p99 | swap p50 | swap p99 | checks p50 | checks p99 | decrypt p50 | decrypt p99 | protocol p50 | protocol p99 | of which block walk p50 | block walk p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| lock | 1 | 0.000 | 16.000 | 0.100 | 23.434 | 0.000 | 2.656 | 0.000 | 41.087 | 0.060 | 72.279 | 0.000 | 65.783 |
| outside | 1 | 0.000 | 15.000 | 0.110 | 0.721 | 0.000 | 5.711 | 0.000 | 42.578 | 0.070 | 76.124 | 0.000 | 69.050 |
| floor | 1 | 0.000 | 16.000 | 0.120 | 0.922 | 0.000 | 5.891 | 0.000 | 43.993 | 0.070 | 11.371 | 0.000 | 1.512 |
| lock | 8 | 8.000 | 51.000 | 0.230 | 127.521 | 1.885 | 20.228 | 22.801 | 151.526 | 98.056 | 739.876 | 92.433 | 687.911 |
| outside | 8 | 8.000 | 48.000 | 0.300 | 1.393 | 2.315 | 19.039 | 21.361 | 141.704 | 89.159 | 618.451 | 84.621 | 588.862 |
| floor | 8 | 0.000 | 57.000 | 0.200 | 0.961 | 0.000 | 16.268 | 0.000 | 162.891 | 0.220 | 49.053 | 0.000 | 6.950 |
| lock | 32 | 32.000 | 96.000 | 0.371 | 3119.589 | 9.875 | 37.330 | 89.702 | 367.549 | 420.636 | 3451.898 | 400.528 | 3424.876 |
| outside | 32 | 32.000 | 76.000 | 0.320 | 2.434 | 9.926 | 26.982 | 90.973 | 252.085 | 416.168 | 2044.869 | 397.450 | 1783.922 |
| floor | 32 | 32.000 | 96.000 | 0.331 | 2.966 | 6.990 | 21.525 | 85.420 | 260.693 | 18.795 | 68.883 | 2.747 | 8.877 |

| build | P | protocol update p50 | protocol update p99 | protocol send p50 | protocol send p99 | stats p50 | stats p99 | of which ack stats p50 | ack stats p99 | rest p50 | rest p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| lock | 1 | 0.080 | 0.221 | 0.000 | 0.000 | 18.235 | 72.878 | 17.243 | 71.135 | 0.391 | 0.943 |
| outside | 1 | 0.090 | 0.280 | 0.000 | 0.000 | 19.146 | 32.632 | 18.094 | 30.969 | 0.401 | 0.903 |
| floor | 1 | 0.080 | 0.241 | 0.000 | 0.000 | 19.457 | 32.251 | 18.385 | 29.606 | 0.440 | 1.112 |
| lock | 8 | 0.140 | 0.621 | 0.000 | 0.000 | 24.115 | 53.902 | 22.413 | 50.094 | 0.781 | 1.893 |
| outside | 8 | 0.130 | 0.501 | 0.000 | 0.000 | 24.076 | 40.998 | 22.563 | 38.784 | 0.791 | 1.452 |
| floor | 8 | 0.130 | 0.480 | 0.000 | 0.000 | 24.416 | 47.210 | 22.713 | 40.557 | 0.750 | 1.632 |
| lock | 32 | 0.271 | 13.195 | 0.000 | 36.851 | 25.879 | 48.852 | 23.895 | 45.857 | 0.891 | 2.385 |
| outside | 32 | 0.151 | 12.383 | 0.000 | 21.441 | 25.088 | 41.679 | 23.454 | 38.954 | 0.882 | 1.794 |
| floor | 32 | 0.140 | 11.922 | 0.000 | 16.952 | 24.346 | 37.231 | 22.913 | 35.588 | 0.851 | 1.292 |

What the I/O thread design leaves in update, then without decrypt, then
without the block walk:

| build | P | no send, no lock p50 | p99 | and no decrypt p50 | p99 | and no block walk p50 | p99 |
|---|---:|---:|---:|---:|---:|---:|---:|
| lock | 1 | 19.036 | 171.124 | 19.036 | 137.151 | 19.036 | 81.784 |
| outside | 1 | 20.439 | 143.351 | 20.419 | 104.519 | 20.359 | 39.423 |
| floor | 1 | 20.589 | 81.595 | 20.558 | 41.278 | 20.539 | 41.278 |
| lock | 8 | 151.838 | 1027.631 | 127.362 | 837.771 | 32.391 | 104.230 |
| outside | 8 | 141.838 | 836.783 | 117.913 | 654.337 | 32.052 | 87.857 |
| floor | 8 | 28.554 | 249.733 | 28.234 | 105.884 | 28.173 | 104.198 |
| lock | 32 | 550.831 | 3666.392 | 459.290 | 3490.066 | 55.403 | 165.537 |
| outside | 32 | 547.592 | 3132.305 | 455.458 | 2083.881 | 54.537 | 127.348 |
| floor | 32 | 137.219 | 387.654 | 51.325 | 129.194 | 48.516 | 121.040 |

Per reply:

| build | P | replies per frame | decrypt ns per reply | receive loop ns per reply | block-walk visits per reply |
|---|---:|---:|---:|---:|---:|
| lock | 1 | 1.000 | 2634.2 | 7585.7 | 1193.0 |
| outside | 1 | 1.000 | 2831.8 | 7706.8 | 1191.5 |
| floor | 1 | 1.000 | 2914.1 | 4146.4 | 1.0 |
| lock | 8 | 8.000 | 3135.7 | 17738.4 | 3885.6 |
| outside | 8 | 8.000 | 3159.7 | 16303.1 | 3882.3 |
| floor | 8 | 8.000 | 3058.2 | 4141.9 | 1.0 |
| lock | 32 | 32.000 | 3779.2 | 20911.4 | 4057.5 |
| outside | 32 | 32.000 | 3025.9 | 17362.8 | 4057.3 |
| floor | 32 | 32.000 | 2670.0 | 3498.8 | 1.0 |

### Set 1: base, lock and outside, three interleaved rounds, earlier

| build | P | load (1 min) | send p50 | send p99 | update p50 | update p99 | total p50 | total p99 | update CPU p50 | update CPU p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| base | 1 | 176 | 12.764 | 36.009 | 19.077 | 250.707 | 33.654 | 305.511 | - | - |
| lock | 1 | 176 | 12.243 | 65.805 | 19.868 | 233.784 | 32.281 | 247.250 | 21.470 | 140.032 |
| outside | 1 | 176 | 12.794 | 27.603 | 20.970 | 156.898 | 33.533 | 172.748 | 22.793 | 156.252 |
| base | 8 | 176 | 106.012 | 1235.999 | 155.686 | 3336.436 | 258.231 | 3662.145 | - | - |
| lock | 8 | 171 | 108.276 | 273.550 | 152.480 | 1750.799 | 256.959 | 2506.588 | 155.422 | 952.877 |
| outside | 8 | 171 | 104.058 | 294.530 | 142.741 | 954.414 | 248.162 | 1354.053 | 144.581 | 911.019 |
| base | 32 | 170 | 414.518 | 6454.136 | 567.820 | 6603.801 | 991.985 | 8364.417 | - | - |
| lock | 32 | 171 | 436.069 | 10462.272 | 597.466 | 9657.984 | 1075.414 | 12072.291 | 593.483 | 1352.236 |
| outside | 32 | 171 | 417.975 | 6457.084 | 577.027 | 6713.516 | 1009.038 | 9372.239 | 577.654 | 1600.382 |

| build | P | no send, no lock p50 | p99 | and no decrypt p50 | p99 | and no block walk p50 | p99 |
|---|---:|---:|---:|---:|---:|---:|---:|
| lock | 1 | 19.738 | 176.755 | 19.738 | 141.511 | 19.718 | 84.934 |
| outside | 1 | 20.860 | 156.577 | 20.840 | 109.972 | 20.749 | 42.771 |
| lock | 8 | 152.039 | 1198.607 | 125.679 | 970.503 | 33.466 | 120.841 |
| outside | 8 | 142.481 | 947.821 | 118.295 | 784.291 | 32.192 | 109.504 |
| lock | 32 | 594.820 | 9629.949 | 492.826 | 9305.035 | 71.324 | 1181.780 |
| outside | 32 | 576.215 | 6710.981 | 479.530 | 6552.461 | 61.236 | 182.858 |

### The open alone

A scratch program (`crypto_aead_chacha20poly1305_ietf_decrypt` from the
vendored `sodium/sodium.c`, `cc -O2`, 20000 opens per repetition, the best of
21 repetitions) puts one open at 1.8 to 2.4 us for a 200 to 240 byte body and
3.7 to 4.5 us for 1200 bytes on this machine; the in-situ figure (decrypt ns
per reply above, 2.6 to 3.8 us) adds the sealed-packet wrapper and the load.

```
 16:39:44 up 1 day, 15:20,  4 users,  load average: 157.49, 132.37, 121.99
aead_open bytes=200 best_ns_per_call=1939.5
aead_open bytes=240 best_ns_per_call=2357.4
aead_open bytes=1200 best_ns_per_call=3705.2
 16:39:48 up 1 day, 15:20,  4 users,  load average: 159.86, 133.28, 122.34
aead_open bytes=200 best_ns_per_call=1845.9
aead_open bytes=240 best_ns_per_call=1958.0
aead_open bytes=1200 best_ns_per_call=4454.6
```

### Raw lines

Set 2, in run order:

```
round=1 build=base P=1  16:32:02 up 1 day, 15:12,  4 users,  load average: 137.88, 162.68, 124.28
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=11.852 send_us_p99=65.254 send_us_max=2854.180 update_us_p50=20.700 update_us_p99=174.561 update_us_max=3631.847 total_us_p50=32.602 total_us_p99=195.050 total_us_max=3643.930 received=2176
round=1 build=lock P=1  16:32:02 up 1 day, 15:12,  4 users,  load average: 137.88, 162.68, 124.28
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=12.113 send_us_p99=29.856 send_us_max=1147.490 update_us_p50=20.479 update_us_p99=174.792 update_us_max=1207.683 total_us_p50=32.793 total_us_p99=192.516 total_us_max=1237.952 received=2190
client_main_thread_parts packets_per_frame=1 n_p50=0.000 n_p99=15.000 swap_p50=0.130 swap_p99=0.400 checks_p50=0.000 checks_p99=2.404 decrypt_p50=0.000 decrypt_p99=40.476 protocol_p50=0.060 protocol_p99=72.279 proto_update_p50=0.080 proto_update_p99=0.221 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=19.467 stats_p99=78.699 rest_p50=0.440 rest_p99=0.943 update_nosend_p50=20.479 update_nosend_p99=174.792 update_nosend_nolock_p50=20.349 update_nosend_nolock_p99=174.702 update_nosend_nolock_nodecrypt_p50=20.339 update_nosend_nolock_nodecrypt_p99=143.043 decrypt_ns_per_reply=2838.3 loop_ns_per_reply=7593.8 replies_per_frame=1.00
client_main_thread_split packets_per_frame=1 ack_read_p50=0.000 ack_read_p99=1.182 record_received_p50=0.000 record_received_p99=0.871 process_block_p50=0.000 process_block_p99=65.783 trackers_p50=0.000 trackers_p99=0.981 loss_tracker_update_p50=0.060 loss_tracker_update_p99=0.211 ack_update_stats_p50=18.324 ack_update_stats_p99=77.337 ack_visits_p50=0.000 ack_visits_p99=20702.000 update_nosend_nolock_nodecrypt_noblock_p50=20.339 update_nosend_nolock_nodecrypt_noblock_p99=88.560 update_cpu_us_p50=22.201 update_cpu_us_p99=135.404 ack_visits_per_reply=1193.0
round=1 build=outside P=1  16:32:03 up 1 day, 15:12,  4 users,  load average: 137.88, 162.68, 124.28
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=13.105 send_us_p99=25.539 send_us_max=34.445 update_us_p50=20.559 update_us_p99=138.452 update_us_max=294.390 total_us_p50=33.785 total_us_p99=152.971 total_us_max=308.837 received=2192
client_main_thread_parts packets_per_frame=1 n_p50=0.000 n_p99=15.000 swap_p50=0.110 swap_p99=1.032 checks_p50=0.000 checks_p99=5.271 decrypt_p50=0.000 decrypt_p99=42.578 protocol_p50=0.060 protocol_p99=76.124 proto_update_p50=0.100 proto_update_p99=0.211 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=19.146 stats_p99=32.632 rest_p50=0.401 rest_p99=0.903 update_nosend_p50=20.559 update_nosend_p99=138.452 update_nosend_nolock_p50=20.439 update_nosend_nolock_p99=138.093 update_nosend_nolock_nodecrypt_p50=20.419 update_nosend_nolock_nodecrypt_p99=104.519 decrypt_ns_per_reply=2831.8 loop_ns_per_reply=7706.8 replies_per_frame=1.00
client_main_thread_split packets_per_frame=1 ack_read_p50=0.000 ack_read_p99=1.210 record_received_p50=0.000 record_received_p99=0.851 process_block_p50=0.000 process_block_p99=69.122 trackers_p50=0.000 trackers_p99=0.961 loss_tracker_update_p50=0.060 loss_tracker_update_p99=0.190 ack_update_stats_p50=18.094 ack_update_stats_p99=30.969 ack_visits_p50=0.000 ack_visits_p99=21510.000 update_nosend_nolock_nodecrypt_noblock_p50=20.359 update_nosend_nolock_nodecrypt_noblock_p99=36.813 update_cpu_us_p50=22.252 update_cpu_us_p99=140.404 ack_visits_per_reply=1195.5
round=1 build=floor P=1  16:32:04 up 1 day, 15:12,  4 users,  load average: 137.88, 162.68, 124.28
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=13.777 send_us_p99=29.566 send_us_max=480.252 update_us_p50=21.531 update_us_p99=83.358 update_us_max=246.959 total_us_p50=35.657 total_us_p99=102.255 total_us_max=507.974 received=2189
client_main_thread_parts packets_per_frame=1 n_p50=0.000 n_p99=14.000 swap_p50=0.120 swap_p99=0.921 checks_p50=0.000 checks_p99=5.891 decrypt_p50=0.000 decrypt_p99=43.091 protocol_p50=0.070 protocol_p99=11.371 proto_update_p50=0.100 proto_update_p99=0.301 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=20.098 stats_p99=37.321 rest_p50=0.470 rest_p99=1.181 update_nosend_p50=21.531 update_nosend_p99=83.358 update_nosend_nolock_p50=21.390 update_nosend_nolock_p99=81.595 update_nosend_nolock_nodecrypt_p50=21.322 update_nosend_nolock_nodecrypt_p99=41.278 decrypt_ns_per_reply=3016.5 loop_ns_per_reply=4252.7 replies_per_frame=1.00
client_main_thread_split packets_per_frame=1 ack_read_p50=0.000 ack_read_p99=1.274 record_received_p50=0.000 record_received_p99=0.931 process_block_p50=0.000 process_block_p99=1.512 trackers_p50=0.000 trackers_p99=1.093 loss_tracker_update_p50=0.060 loss_tracker_update_p99=0.321 ack_update_stats_p50=18.956 ack_update_stats_p99=36.299 ack_visits_p50=0.000 ack_visits_p99=14.000 update_nosend_nolock_nodecrypt_noblock_p50=21.291 update_nosend_nolock_nodecrypt_noblock_p99=41.278 update_cpu_us_p50=23.223 update_cpu_us_p99=82.635 ack_visits_per_reply=1.0
round=1 build=base P=8  16:32:04 up 1 day, 15:12,  4 users,  load average: 137.88, 162.68, 124.28
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=97.425 send_us_p99=1239.364 send_us_max=14739.242 update_us_p50=152.380 update_us_p99=3276.273 update_us_max=6905.905 total_us_p50=249.484 total_us_p99=3433.691 total_us_max=15096.801 received=17584
round=1 build=lock P=8  16:32:07 up 1 day, 15:12,  4 users,  load average: 134.92, 161.66, 124.15
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=100.792 send_us_p99=238.503 send_us_max=3858.647 update_us_p50=154.413 update_us_p99=1398.697 update_us_max=7308.720 total_us_p50=254.963 total_us_p99=1731.789 total_us_max=7404.922 received=17592
client_main_thread_parts packets_per_frame=8 n_p50=8.000 n_p99=51.000 swap_p50=0.230 swap_p99=105.250 checks_p50=1.885 checks_p99=20.228 decrypt_p50=22.993 decrypt_p99=151.526 protocol_p50=102.493 protocol_p99=765.706 proto_update_p50=0.160 proto_update_p99=0.691 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=24.607 stats_p99=73.830 rest_p50=0.871 rest_p99=1.893 update_nosend_p50=154.413 update_nosend_p99=1376.666 update_nosend_nolock_p50=154.202 update_nosend_nolock_p99=1027.631 update_nosend_nolock_nodecrypt_p50=130.107 update_nosend_nolock_nodecrypt_p99=837.771 decrypt_ns_per_reply=3135.7 loop_ns_per_reply=17738.4 replies_per_frame=8.00
client_main_thread_split packets_per_frame=8 ack_read_p50=0.640 ack_read_p99=4.878 record_received_p50=0.481 record_received_p99=3.729 process_block_p50=97.128 process_block_p99=736.299 trackers_p50=0.550 trackers_p99=4.791 loss_tracker_update_p50=0.170 loss_tracker_update_p99=0.561 ack_update_stats_p50=22.873 ack_update_stats_p99=72.007 ack_visits_p50=30816.000 ack_visits_p99=207144.000 update_nosend_nolock_nodecrypt_noblock_p50=32.863 update_nosend_nolock_nodecrypt_noblock_p99=113.835 update_cpu_us_p50=156.394 update_cpu_us_p99=894.828 ack_visits_per_reply=3885.6
round=1 build=outside P=8  16:32:09 up 1 day, 15:12,  4 users,  load average: 134.92, 161.66, 124.15
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=102.615 send_us_p99=154.163 send_us_max=8227.314 update_us_p50=156.046 update_us_p99=738.282 update_us_max=4070.911 total_us_p50=258.321 total_us_p99=874.491 total_us_max=8554.255 received=17584
client_main_thread_parts packets_per_frame=8 n_p50=8.000 n_p99=42.000 swap_p50=0.350 swap_p99=3.356 checks_p50=3.478 checks_p99=19.039 decrypt_p50=22.843 decrypt_p99=123.223 protocol_p50=103.246 protocol_p99=572.223 proto_update_p50=0.130 proto_update_p99=0.501 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=24.076 stats_p99=40.998 rest_p50=0.882 rest_p99=1.452 update_nosend_p50=156.046 update_nosend_p99=738.282 update_nosend_nolock_p50=155.746 update_nosend_nolock_p99=737.780 update_nosend_nolock_nodecrypt_p50=131.750 update_nosend_nolock_nodecrypt_p99=611.553 decrypt_ns_per_reply=2943.1 loop_ns_per_reply=16299.3 replies_per_frame=8.02
client_main_thread_split packets_per_frame=8 ack_read_p50=0.632 ack_read_p99=3.425 record_received_p50=0.470 record_received_p99=2.592 process_block_p50=98.315 process_block_p99=541.809 trackers_p50=0.550 trackers_p99=2.944 loss_tracker_update_p50=0.111 loss_tracker_update_p99=0.481 ack_update_stats_p50=22.563 ack_update_stats_p99=38.784 ack_visits_p50=32640.000 ack_visits_p99=166648.000 update_nosend_nolock_nodecrypt_noblock_p50=33.193 update_nosend_nolock_nodecrypt_noblock_p99=87.857 update_cpu_us_p50=158.087 update_cpu_us_p99=726.422 ack_visits_per_reply=3879.6
round=1 build=floor P=8  16:32:11 up 1 day, 15:12,  4 users,  load average: 131.81, 160.57, 124.00
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=103.076 send_us_p99=204.308 send_us_max=1910.078 update_us_p50=28.815 update_us_p99=247.210 update_us_max=1402.183 total_us_p50=141.970 total_us_p99=399.610 total_us_max=1979.971 received=17576
client_main_thread_parts packets_per_frame=8 n_p50=0.000 n_p99=57.000 swap_p50=0.200 swap_p99=0.812 checks_p50=0.000 checks_p99=13.781 decrypt_p50=0.000 decrypt_p99=162.891 protocol_p50=0.220 protocol_p99=49.053 proto_update_p50=0.130 proto_update_p99=0.480 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=24.897 stats_p99=47.210 rest_p50=0.750 rest_p99=1.632 update_nosend_p50=28.815 update_nosend_p99=247.210 update_nosend_nolock_p50=28.554 update_nosend_nolock_p99=247.000 update_nosend_nolock_nodecrypt_p50=28.234 update_nosend_nolock_nodecrypt_p99=105.884 decrypt_ns_per_reply=3075.4 loop_ns_per_reply=4141.9 replies_per_frame=7.99
client_main_thread_split packets_per_frame=8 ack_read_p50=0.000 ack_read_p99=4.938 record_received_p50=0.000 record_received_p99=3.939 process_block_p50=0.000 process_block_p99=7.146 trackers_p50=0.000 trackers_p99=4.377 loss_tracker_update_p50=0.110 loss_tracker_update_p99=0.511 ack_update_stats_p50=23.314 ack_update_stats_p99=40.557 ack_visits_p50=0.000 ack_visits_p99=56.000 update_nosend_nolock_nodecrypt_noblock_p50=28.173 update_nosend_nolock_nodecrypt_noblock_p99=104.198 update_cpu_us_p50=31.148 update_cpu_us_p99=249.909 ack_visits_per_reply=1.0
round=1 build=base P=32  16:32:13 up 1 day, 15:12,  4 users,  load average: 131.81, 160.57, 124.00
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=390.798 send_us_p99=3390.129 send_us_max=5465.104 update_us_p50=551.491 update_us_p99=3547.135 update_us_max=11015.921 total_us_p50=965.893 total_us_p99=4000.270 total_us_max=11401.248 received=70368
round=1 build=lock P=32  16:32:21 up 1 day, 15:12,  4 users,  load average: 123.67, 157.88, 123.52
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=388.799 send_us_p99=3408.460 send_us_max=7507.715 update_us_p50=545.367 update_us_p99=4537.809 update_us_max=9696.618 total_us_p50=952.241 total_us_p99=5708.915 total_us_max=10142.826 received=70368
client_main_thread_parts packets_per_frame=32 n_p50=32.000 n_p99=96.000 swap_p50=0.371 swap_p99=3406.397 checks_p50=9.875 checks_p99=37.330 decrypt_p50=88.614 decrypt_p99=900.232 protocol_p50=411.061 protocol_p99=3443.936 proto_update_p50=0.271 proto_update_p99=12.755 proto_send_p50=0.000 proto_send_p99=36.851 stats_p50=25.879 stats_p99=43.562 rest_p50=0.891 rest_p99=2.385 update_nosend_p50=545.367 update_nosend_p99=4497.923 update_nosend_nolock_p50=540.568 update_nosend_nolock_p99=3581.460 update_nosend_nolock_nodecrypt_p50=449.665 update_nosend_nolock_nodecrypt_p99=3479.426 decrypt_ns_per_reply=3779.2 loop_ns_per_reply=20911.4 replies_per_frame=32.00
client_main_thread_split packets_per_frame=32 ack_read_p50=2.563 ack_read_p99=8.098 record_received_p50=1.878 record_received_p99=6.740 process_block_p50=392.586 process_block_p99=3424.876 trackers_p50=2.272 trackers_p99=10.094 loss_tracker_update_p50=0.260 loss_tracker_update_p99=0.852 ack_update_stats_p50=23.775 ack_update_stats_p99=40.588 ack_visits_p50=130048.000 ack_visits_p99=387072.000 update_nosend_nolock_nodecrypt_noblock_p50=55.253 update_nosend_nolock_nodecrypt_noblock_p99=162.746 update_cpu_us_p50=542.608 update_cpu_us_p99=1594.290 ack_visits_per_reply=4057.5
round=1 build=outside P=32  16:32:29 up 1 day, 15:12,  4 users,  load average: 120.58, 156.67, 123.31
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=391.942 send_us_p99=1567.145 send_us_max=11697.960 update_us_p50=547.922 update_us_p99=1825.734 update_us_max=3860.363 total_us_p50=948.834 total_us_p99=3908.624 total_us_max=12269.572 received=70368
client_main_thread_parts packets_per_frame=32 n_p50=32.000 n_p99=76.000 swap_p50=0.320 swap_p99=2.966 checks_p50=9.926 checks_p99=26.663 decrypt_p50=90.512 decrypt_p99=252.085 protocol_p50=419.617 protocol_p99=1329.154 proto_update_p50=0.151 proto_update_p99=12.263 proto_send_p50=0.000 proto_send_p99=19.677 stats_p50=25.088 stats_p99=41.679 rest_p50=0.881 rest_p99=1.604 update_nosend_p50=547.922 update_nosend_p99=1811.396 update_nosend_nolock_p50=547.592 update_nosend_nolock_p99=1811.105 update_nosend_nolock_nodecrypt_p50=456.218 update_nosend_nolock_nodecrypt_p99=1366.984 decrypt_ns_per_reply=3025.9 loop_ns_per_reply=16992.6 replies_per_frame=32.00
client_main_thread_split packets_per_frame=32 ack_read_p50=2.572 ack_read_p99=7.146 record_received_p50=1.887 record_received_p99=4.912 process_block_p50=401.680 process_block_p99=1170.693 trackers_p50=2.401 trackers_p99=6.871 loss_tracker_update_p50=0.240 loss_tracker_update_p99=0.631 ack_update_stats_p50=23.454 ack_update_stats_p99=38.954 ack_visits_p50=130048.000 ack_visits_p99=307072.000 update_nosend_nolock_nodecrypt_noblock_p50=54.537 update_nosend_nolock_nodecrypt_noblock_p99=127.348 update_cpu_us_p50=549.010 update_cpu_us_p99=1320.847 ack_visits_per_reply=4057.4
round=1 build=floor P=32  16:32:36 up 1 day, 15:13,  4 users,  load average: 112.66, 153.79, 122.73
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=390.248 send_us_p99=479.267 send_us_max=4620.561 update_us_p50=137.680 update_us_p99=407.652 update_us_max=3145.901 total_us_p50=531.295 total_us_p99=830.460 total_us_max=4802.565 received=70368
client_main_thread_parts packets_per_frame=32 n_p50=32.000 n_p99=96.000 swap_p50=0.331 swap_p99=3.056 checks_p50=6.990 checks_p99=21.525 decrypt_p50=85.420 decrypt_p99=260.693 protocol_p50=18.795 protocol_p99=68.883 proto_update_p50=0.130 proto_update_p99=11.922 proto_send_p50=0.000 proto_send_p99=16.952 stats_p50=24.346 stats_p99=37.231 rest_p50=0.851 rest_p99=1.292 update_nosend_p50=137.680 update_nosend_p99=391.131 update_nosend_nolock_p50=137.219 update_nosend_nolock_p99=387.654 update_nosend_nolock_nodecrypt_p50=51.325 update_nosend_nolock_nodecrypt_p99=129.194 decrypt_ns_per_reply=2654.0 loop_ns_per_reply=3498.8 replies_per_frame=32.00
client_main_thread_split packets_per_frame=32 ack_read_p50=2.451 ack_read_p99=7.704 record_received_p50=1.824 record_received_p99=5.662 process_block_p50=2.747 process_block_p99=8.877 trackers_p50=1.914 trackers_p99=6.962 loss_tracker_update_p50=0.211 loss_tracker_update_p99=0.531 ack_update_stats_p50=22.913 ack_update_stats_p99=35.588 ack_visits_p50=32.000 ack_visits_p99=96.000 update_nosend_nolock_nodecrypt_noblock_p50=48.516 update_nosend_nolock_nodecrypt_noblock_p99=121.040 update_cpu_us_p50=139.592 update_cpu_us_p99=402.304 ack_visits_per_reply=1.0
round=2 build=base P=1  16:32:40 up 1 day, 15:13,  4 users,  load average: 108.84, 152.31, 122.42
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=12.894 send_us_p99=32.832 send_us_max=1143.744 update_us_p50=20.599 update_us_p99=189.388 update_us_max=6206.942 total_us_p50=33.914 total_us_p99=246.937 total_us_max=6221.510 received=2199
round=2 build=lock P=1  16:32:41 up 1 day, 15:13,  4 users,  load average: 108.84, 152.31, 122.42
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=11.311 send_us_p99=52.519 send_us_max=407.541 update_us_p50=19.126 update_us_p99=174.590 update_us_max=3201.958 total_us_p50=30.508 total_us_p99=193.817 total_us_max=3215.524 received=2195
client_main_thread_parts packets_per_frame=1 n_p50=0.000 n_p99=16.000 swap_p50=0.100 swap_p99=23.434 checks_p50=0.000 checks_p99=2.656 decrypt_p50=0.000 decrypt_p99=41.087 protocol_p50=0.061 protocol_p99=73.728 proto_update_p50=0.100 proto_update_p99=0.160 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=18.235 stats_p99=72.878 rest_p50=0.391 rest_p99=0.792 update_nosend_p50=19.126 update_nosend_p99=174.590 update_nosend_nolock_p50=19.036 update_nosend_nolock_p99=171.124 update_nosend_nolock_nodecrypt_p50=19.036 update_nosend_nolock_nodecrypt_p99=137.151 decrypt_ns_per_reply=2634.2 loop_ns_per_reply=7585.7 replies_per_frame=1.00
client_main_thread_split packets_per_frame=1 ack_read_p50=0.000 ack_read_p99=1.204 record_received_p50=0.000 record_received_p99=0.863 process_block_p50=0.000 process_block_p99=66.334 trackers_p50=0.000 trackers_p99=0.971 loss_tracker_update_p50=0.060 loss_tracker_update_p99=0.270 ack_update_stats_p50=17.243 ack_update_stats_p99=71.135 ack_visits_p50=0.000 ack_visits_p99=21795.000 update_nosend_nolock_nodecrypt_noblock_p50=19.036 update_nosend_nolock_nodecrypt_noblock_p99=81.784 update_cpu_us_p50=20.709 update_cpu_us_p99=135.894 ack_visits_per_reply=1195.0
round=2 build=outside P=1  16:32:42 up 1 day, 15:13,  4 users,  load average: 108.84, 152.31, 122.42
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=12.624 send_us_p99=27.051 send_us_max=217.502 update_us_p50=19.817 update_us_p99=143.872 update_us_max=1349.583 total_us_p50=32.973 total_us_p99=158.390 total_us_max=1363.189 received=2190
client_main_thread_parts packets_per_frame=1 n_p50=0.000 n_p99=14.000 swap_p50=0.170 swap_p99=0.721 checks_p50=0.000 checks_p99=5.711 decrypt_p50=0.000 decrypt_p99=42.120 protocol_p50=0.070 protocol_p99=81.956 proto_update_p50=0.070 proto_update_p99=0.350 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=17.923 stats_p99=35.076 rest_p50=0.440 rest_p99=1.383 update_nosend_p50=19.817 update_nosend_p99=143.872 update_nosend_nolock_p50=19.606 update_nosend_nolock_p99=143.552 update_nosend_nolock_nodecrypt_p50=19.586 update_nosend_nolock_nodecrypt_p99=109.467 decrypt_ns_per_reply=2951.9 loop_ns_per_reply=8496.8 replies_per_frame=1.00
client_main_thread_split packets_per_frame=1 ack_read_p50=0.000 ack_read_p99=1.602 record_received_p50=0.000 record_received_p99=1.344 process_block_p50=0.000 process_block_p99=69.050 trackers_p50=0.000 trackers_p99=1.622 loss_tracker_update_p50=0.060 loss_tracker_update_p99=0.351 ack_update_stats_p50=16.872 ack_update_stats_p99=33.283 ack_visits_p50=0.000 ack_visits_p99=21882.000 update_nosend_nolock_nodecrypt_noblock_p50=19.356 update_nosend_nolock_nodecrypt_noblock_p99=44.655 update_cpu_us_p50=21.631 update_cpu_us_p99=144.440 ack_visits_per_reply=1191.0
round=2 build=floor P=1  16:32:42 up 1 day, 15:13,  4 users,  load average: 108.84, 152.31, 122.42
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=12.273 send_us_p99=20.649 send_us_max=33.273 update_us_p50=16.571 update_us_p99=75.363 update_us_max=202.804 total_us_p50=30.167 total_us_p99=88.238 total_us_max=214.085 received=2197
client_main_thread_parts packets_per_frame=1 n_p50=0.000 n_p99=16.000 swap_p50=0.110 swap_p99=1.653 checks_p50=0.000 checks_p99=5.550 decrypt_p50=0.000 decrypt_p99=43.993 protocol_p50=0.060 protocol_p99=10.160 proto_update_p50=0.061 proto_update_p99=0.241 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=13.736 stats_p99=29.496 rest_p50=0.381 rest_p99=0.831 update_nosend_p50=16.571 update_nosend_p99=75.363 update_nosend_nolock_p50=16.382 update_nosend_nolock_p99=74.872 update_nosend_nolock_nodecrypt_p50=16.241 update_nosend_nolock_nodecrypt_p99=35.329 decrypt_ns_per_reply=2568.4 loop_ns_per_reply=3623.3 replies_per_frame=1.00
client_main_thread_split packets_per_frame=1 ack_read_p50=0.000 ack_read_p99=1.173 record_received_p50=0.000 record_received_p99=0.911 process_block_p50=0.000 process_block_p99=1.361 trackers_p50=0.000 trackers_p99=0.950 loss_tracker_update_p50=0.060 loss_tracker_update_p99=0.181 ack_update_stats_p50=12.794 ack_update_stats_p99=26.160 ack_visits_p50=0.000 ack_visits_p99=16.000 update_nosend_nolock_nodecrypt_noblock_p50=16.230 update_nosend_nolock_nodecrypt_noblock_p99=34.475 update_cpu_us_p50=18.365 update_cpu_us_p99=76.223 ack_visits_per_reply=1.0
round=2 build=base P=8  16:32:43 up 1 day, 15:13,  4 users,  load average: 108.84, 152.31, 122.42
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=102.093 send_us_p99=173.187 send_us_max=3584.180 update_us_p50=152.759 update_us_p99=941.922 update_us_max=8309.039 total_us_p50=254.061 total_us_p99=1439.373 total_us_max=8411.934 received=17584
round=2 build=lock P=8  16:32:45 up 1 day, 15:13,  4 users,  load average: 105.65, 150.93, 122.13
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=94.889 send_us_p99=149.383 send_us_max=5162.667 update_us_p50=141.057 update_us_p99=1093.709 update_us_max=4250.231 total_us_p50=233.612 total_us_p99=1291.844 total_us_max=5752.022 received=17584
client_main_thread_parts packets_per_frame=8 n_p50=8.000 n_p99=48.000 swap_p50=0.211 swap_p99=127.521 checks_p50=1.453 checks_p99=19.068 decrypt_p50=21.079 decrypt_p99=128.688 protocol_p50=93.457 protocol_p99=628.616 proto_update_p50=0.110 proto_update_p99=0.360 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=22.322 stats_p99=35.527 rest_p50=0.641 rest_p99=1.183 update_nosend_p50=141.057 update_nosend_p99=1074.553 update_nosend_nolock_p50=140.716 update_nosend_nolock_p99=796.207 update_nosend_nolock_nodecrypt_p50=119.345 update_nosend_nolock_nodecrypt_p99=667.861 decrypt_ns_per_reply=2675.3 loop_ns_per_reply=15450.9 replies_per_frame=8.00
client_main_thread_split packets_per_frame=8 ack_read_p50=0.610 ack_read_p99=3.847 record_received_p50=0.440 record_received_p99=2.915 process_block_p50=88.907 process_block_p99=592.811 trackers_p50=0.532 trackers_p99=3.426 loss_tracker_update_p50=0.141 loss_tracker_update_p99=0.351 ack_update_stats_p50=21.049 ack_update_stats_p99=33.814 ack_visits_p50=32640.000 ack_visits_p99=195264.000 update_nosend_nolock_nodecrypt_noblock_p50=29.727 update_nosend_nolock_nodecrypt_noblock_p99=79.901 update_cpu_us_p50=142.968 update_cpu_us_p99=798.838 ack_visits_per_reply=3885.6
round=2 build=outside P=8  16:32:47 up 1 day, 15:13,  4 users,  load average: 105.65, 150.93, 122.13
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=100.420 send_us_p99=129.305 send_us_max=3191.317 update_us_p50=142.279 update_us_p99=837.164 update_us_max=3282.630 total_us_p50=237.119 total_us_p99=981.907 total_us_max=3802.684 received=17584
client_main_thread_parts packets_per_frame=8 n_p50=8.000 n_p99=48.000 swap_p50=0.230 swap_p99=1.072 checks_p50=2.315 checks_p99=18.143 decrypt_p50=21.361 decrypt_p99=141.704 protocol_p50=89.159 protocol_p99=618.451 proto_update_p50=0.111 proto_update_p99=0.370 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=23.635 stats_p99=36.299 rest_p50=0.770 rest_p99=1.182 update_nosend_p50=142.279 update_nosend_p99=837.164 update_nosend_nolock_p50=141.838 update_nosend_nolock_p99=836.783 update_nosend_nolock_nodecrypt_p50=117.913 update_nosend_nolock_nodecrypt_p99=654.337 decrypt_ns_per_reply=3159.7 loop_ns_per_reply=16303.1 replies_per_frame=8.00
client_main_thread_split packets_per_frame=8 ack_read_p50=0.622 ack_read_p99=3.875 record_received_p50=0.431 record_received_p99=2.913 process_block_p50=84.621 process_block_p99=588.862 trackers_p50=0.501 trackers_p99=3.458 loss_tracker_update_p50=0.101 loss_tracker_update_p99=0.371 ack_update_stats_p50=22.272 ack_update_stats_p99=34.906 ack_visits_p50=28560.000 ack_visits_p99=195264.000 update_nosend_nolock_nodecrypt_noblock_p50=31.276 update_nosend_nolock_nodecrypt_noblock_p99=72.894 update_cpu_us_p50=144.131 update_cpu_us_p99=778.680 ack_visits_per_reply=3882.3
round=2 build=floor P=8  16:32:49 up 1 day, 15:13,  4 users,  load average: 105.65, 150.93, 122.13
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=99.548 send_us_p99=135.236 send_us_max=381.301 update_us_p50=25.098 update_us_p99=265.903 update_us_max=1188.869 total_us_p50=128.122 total_us_p99=406.449 total_us_max=1556.745 received=17584
client_main_thread_parts packets_per_frame=8 n_p50=0.000 n_p99=56.000 swap_p50=0.160 swap_p99=0.961 checks_p50=0.000 checks_p99=16.268 decrypt_p50=0.000 decrypt_p99=158.081 protocol_p50=0.121 protocol_p99=40.136 proto_update_p50=0.121 proto_update_p99=0.371 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=23.013 stats_p99=37.832 rest_p50=0.601 rest_p99=1.271 update_nosend_p50=25.098 update_nosend_p99=250.063 update_nosend_nolock_p50=24.936 update_nosend_nolock_p99=249.733 update_nosend_nolock_nodecrypt_p50=24.918 update_nosend_nolock_nodecrypt_p99=89.240 decrypt_ns_per_reply=2762.7 loop_ns_per_reply=3757.2 replies_per_frame=8.01
client_main_thread_split packets_per_frame=8 ack_read_p50=0.000 ack_read_p99=4.792 record_received_p50=0.000 record_received_p99=3.441 process_block_p50=0.000 process_block_p99=5.711 trackers_p50=0.000 trackers_p99=3.708 loss_tracker_update_p50=0.080 loss_tracker_update_p99=0.341 ack_update_stats_p50=21.781 ack_update_stats_p99=36.319 ack_visits_p50=0.000 ack_visits_p99=56.000 update_nosend_nolock_nodecrypt_noblock_p50=24.918 update_nosend_nolock_nodecrypt_noblock_p99=86.634 update_cpu_us_p50=26.920 update_cpu_us_p99=232.747 ack_visits_per_reply=1.0
round=2 build=base P=32  16:32:51 up 1 day, 15:13,  4 users,  load average: 102.48, 149.52, 121.83
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=397.332 send_us_p99=3368.953 send_us_max=4026.387 update_us_p50=531.145 update_us_p99=3853.661 update_us_max=8616.230 total_us_p50=951.621 total_us_p99=4331.925 total_us_max=8988.745 received=70368
round=2 build=lock P=32  16:32:59 up 1 day, 15:13,  4 users,  load average: 103.00, 148.85, 121.76
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=412.511 send_us_p99=6466.674 send_us_max=16243.889 update_us_p50=559.278 update_us_p99=5547.615 update_us_max=13424.175 total_us_p50=988.100 total_us_p99=8058.095 total_us_max=16767.750 received=70355
client_main_thread_parts packets_per_frame=32 n_p50=32.000 n_p99=96.000 swap_p50=0.491 swap_p99=1352.899 checks_p50=12.324 checks_p99=41.950 decrypt_p50=91.848 decrypt_p99=292.375 protocol_p50=420.636 protocol_p99=4424.160 proto_update_p50=0.290 proto_update_p99=13.886 proto_send_p50=0.000 proto_send_p99=40.897 stats_p50=26.711 stats_p99=63.400 rest_p50=1.032 rest_p99=2.395 update_nosend_p50=559.278 update_nosend_p99=5547.615 update_nosend_nolock_p50=555.051 update_nosend_nolock_p99=5318.441 update_nosend_nolock_nodecrypt_p50=461.354 update_nosend_nolock_nodecrypt_p99=4830.709 decrypt_ns_per_reply=3376.6 loop_ns_per_reply=20377.1 replies_per_frame=31.99
client_main_thread_split packets_per_frame=32 ack_read_p50=2.626 ack_read_p99=8.058 record_received_p50=1.952 record_received_p99=6.452 process_block_p50=400.528 process_block_p99=4325.672 trackers_p50=2.393 trackers_p99=10.466 loss_tracker_update_p50=0.271 loss_tracker_update_p99=0.971 ack_update_stats_p50=24.596 ack_update_stats_p99=53.080 ack_visits_p50=130048.000 ack_visits_p99=387072.000 update_nosend_nolock_nodecrypt_noblock_p50=59.933 update_nosend_nolock_nodecrypt_noblock_p99=191.531 update_cpu_us_p50=556.844 update_cpu_us_p99=1596.023 ack_visits_per_reply=4057.5
round=2 build=outside P=32  16:33:07 up 1 day, 15:13,  4 users,  load average: 104.53, 147.62, 121.65
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=390.809 send_us_p99=808.069 send_us_max=21876.305 update_us_p50=544.530 update_us_p99=3164.777 update_us_max=9940.155 total_us_p50=942.313 total_us_p99=3916.189 total_us_max=22492.501 received=70368
client_main_thread_parts packets_per_frame=32 n_p50=32.000 n_p99=66.000 swap_p50=0.301 swap_p99=2.434 checks_p50=9.539 checks_p99=26.982 decrypt_p50=90.973 decrypt_p99=219.648 protocol_p50=415.116 protocol_p99=2044.869 proto_update_p50=0.130 proto_update_p99=12.383 proto_send_p50=0.000 proto_send_p99=21.441 stats_p50=24.927 stats_p99=39.765 rest_p50=0.882 rest_p99=1.794 update_nosend_p50=544.530 update_nosend_p99=3132.616 update_nosend_nolock_p50=544.139 update_nosend_nolock_p99=3132.305 update_nosend_nolock_nodecrypt_p50=451.935 update_nosend_nolock_nodecrypt_p99=2083.881 decrypt_ns_per_reply=2934.6 loop_ns_per_reply=17362.8 replies_per_frame=32.03
client_main_thread_split packets_per_frame=32 ack_read_p50=2.543 ack_read_p99=7.345 record_received_p50=1.901 record_received_p99=5.289 process_block_p50=397.450 process_block_p99=1783.922 trackers_p50=2.195 trackers_p99=6.569 loss_tracker_update_p50=0.230 loss_tracker_update_p99=0.602 ack_update_stats_p50=23.294 ack_update_stats_p99=37.451 ack_visits_p50=130048.000 ack_visits_p99=267072.000 update_nosend_nolock_nodecrypt_noblock_p50=53.948 update_nosend_nolock_nodecrypt_noblock_p99=124.051 update_cpu_us_p50=545.282 update_cpu_us_p99=1216.552 ack_visits_per_reply=4057.3
round=2 build=floor P=32  16:33:14 up 1 day, 15:13,  4 users,  load average: 101.84, 146.35, 121.38
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=380.160 send_us_p99=425.845 send_us_max=3420.670 update_us_p50=135.607 update_us_p99=392.843 update_us_max=538.589 total_us_p50=519.092 total_us_p99=777.741 total_us_max=3581.185 received=70368
client_main_thread_parts packets_per_frame=32 n_p50=32.000 n_p99=96.000 swap_p50=0.331 swap_p99=2.966 checks_p50=6.527 checks_p99=20.583 decrypt_p50=84.736 decrypt_p99=256.967 protocol_p50=18.383 protocol_p99=66.571 proto_update_p50=0.140 proto_update_p99=11.552 proto_send_p50=0.000 proto_send_p99=16.652 stats_p50=23.915 stats_p99=37.050 rest_p50=0.801 rest_p99=1.201 update_nosend_p50=135.607 update_nosend_p99=376.302 update_nosend_nolock_p50=135.175 update_nosend_nolock_p99=375.540 update_nosend_nolock_nodecrypt_p50=49.890 update_nosend_nolock_nodecrypt_p99=121.509 decrypt_ns_per_reply=2670.0 loop_ns_per_reply=3489.0 replies_per_frame=32.00
client_main_thread_split packets_per_frame=32 ack_read_p50=2.407 ack_read_p99=7.323 record_received_p50=1.763 record_received_p99=5.320 process_block_p50=2.626 process_block_p99=8.373 trackers_p50=1.903 trackers_p99=5.769 loss_tracker_update_p50=0.240 loss_tracker_update_p99=0.521 ack_update_stats_p50=22.392 ack_update_stats_p99=35.247 ack_visits_p50=32.000 ack_visits_p99=96.000 update_nosend_nolock_nodecrypt_noblock_p50=47.234 update_nosend_nolock_nodecrypt_noblock_p99=113.432 update_cpu_us_p50=137.507 update_cpu_us_p99=393.789 ack_visits_per_reply=1.0
round=3 build=base P=1  16:33:19 up 1 day, 15:13,  4 users,  load average: 98.97, 145.02, 121.08
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=12.954 send_us_p99=15.819 send_us_max=54.252 update_us_p50=20.289 update_us_p99=174.450 update_us_max=2573.767 total_us_p50=33.322 total_us_p99=187.755 total_us_max=2586.341 received=2191
round=3 build=lock P=1  16:33:19 up 1 day, 15:13,  4 users,  load average: 98.97, 145.02, 121.08
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=10.660 send_us_p99=20.889 send_us_max=1131.159 update_us_p50=16.962 update_us_p99=171.955 update_us_max=3793.343 total_us_p50=27.732 total_us_p99=187.284 total_us_max=3805.756 received=2191
client_main_thread_parts packets_per_frame=1 n_p50=0.000 n_p99=16.000 swap_p50=0.100 swap_p99=48.803 checks_p50=0.000 checks_p99=6.443 decrypt_p50=0.000 decrypt_p99=42.462 protocol_p50=0.050 protocol_p99=66.919 proto_update_p50=0.071 proto_update_p99=0.411 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=16.170 stats_p99=26.800 rest_p50=0.381 rest_p99=1.353 update_nosend_p50=16.962 update_nosend_p99=171.955 update_nosend_nolock_p50=16.853 update_nosend_nolock_p99=127.981 update_nosend_nolock_nodecrypt_p50=16.852 update_nosend_nolock_nodecrypt_p99=90.393 decrypt_ns_per_reply=2517.7 loop_ns_per_reply=6518.0 replies_per_frame=1.01
client_main_thread_split packets_per_frame=1 ack_read_p50=0.000 ack_read_p99=1.424 record_received_p50=0.000 record_received_p99=1.112 process_block_p50=0.000 process_block_p99=59.352 trackers_p50=0.000 trackers_p99=1.357 loss_tracker_update_p50=0.050 loss_tracker_update_p99=0.350 ack_update_stats_p50=15.318 ack_update_stats_p99=25.238 ack_visits_p50=0.000 ack_visits_p99=26118.000 update_nosend_nolock_nodecrypt_noblock_p50=16.852 update_nosend_nolock_nodecrypt_noblock_p99=35.086 update_cpu_us_p50=18.365 update_cpu_us_p99=134.442 ack_visits_per_reply=1187.3
round=3 build=outside P=1  16:33:20 up 1 day, 15:13,  4 users,  load average: 98.97, 145.02, 121.08
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=12.594 send_us_p99=24.706 send_us_max=62.929 update_us_p50=20.719 update_us_p99=143.882 update_us_max=436.616 total_us_p50=34.094 total_us_p99=158.119 total_us_max=448.298 received=2190
client_main_thread_parts packets_per_frame=1 n_p50=0.000 n_p99=19.000 swap_p50=0.101 swap_p99=0.561 checks_p50=0.000 checks_p99=5.911 decrypt_p50=0.000 decrypt_p99=52.120 protocol_p50=0.070 protocol_p99=75.984 proto_update_p50=0.090 proto_update_p99=0.280 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=19.327 stats_p99=29.736 rest_p50=0.401 rest_p99=0.881 update_nosend_p50=20.719 update_nosend_p99=143.882 update_nosend_nolock_p50=20.599 update_nosend_nolock_p99=143.351 update_nosend_nolock_nodecrypt_p50=20.569 update_nosend_nolock_nodecrypt_p99=101.491 decrypt_ns_per_reply=2816.7 loop_ns_per_reply=7519.1 replies_per_frame=1.00
client_main_thread_split packets_per_frame=1 ack_read_p50=0.000 ack_read_p99=1.444 record_received_p50=0.000 record_received_p99=1.152 process_block_p50=0.000 process_block_p99=68.457 trackers_p50=0.000 trackers_p99=1.173 loss_tracker_update_p50=0.060 loss_tracker_update_p99=0.211 ack_update_stats_p50=18.235 ack_update_stats_p99=26.450 ack_visits_p50=0.000 ack_visits_p99=22098.000 update_nosend_nolock_nodecrypt_noblock_p50=20.479 update_nosend_nolock_nodecrypt_noblock_p99=39.423 update_cpu_us_p50=22.472 update_cpu_us_p99=149.601 ack_visits_per_reply=1191.5
round=3 build=floor P=1  16:33:20 up 1 day, 15:13,  4 users,  load average: 97.53, 143.96, 120.87
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=13.556 send_us_p99=26.991 send_us_max=63.169 update_us_p50=20.740 update_us_p99=89.610 update_us_max=208.204 total_us_p50=34.466 total_us_p99=104.989 total_us_max=221.349 received=2190
client_main_thread_parts packets_per_frame=1 n_p50=0.000 n_p99=18.000 swap_p50=0.140 swap_p99=0.922 checks_p50=0.000 checks_p99=6.383 decrypt_p50=0.000 decrypt_p99=50.008 protocol_p50=0.070 protocol_p99=12.513 proto_update_p50=0.080 proto_update_p99=0.180 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=19.457 stats_p99=32.251 rest_p50=0.440 rest_p99=1.112 update_nosend_p50=20.740 update_nosend_p99=89.610 update_nosend_nolock_p50=20.589 update_nosend_nolock_p99=89.019 update_nosend_nolock_nodecrypt_p50=20.558 update_nosend_nolock_nodecrypt_p99=44.253 decrypt_ns_per_reply=2914.1 loop_ns_per_reply=4146.4 replies_per_frame=1.00
client_main_thread_split packets_per_frame=1 ack_read_p50=0.000 ack_read_p99=1.432 record_received_p50=0.000 record_received_p99=1.072 process_block_p50=0.000 process_block_p99=1.625 trackers_p50=0.000 trackers_p99=1.142 loss_tracker_update_p50=0.070 loss_tracker_update_p99=0.261 ack_update_stats_p50=18.385 ack_update_stats_p99=29.606 ack_visits_p50=0.000 ack_visits_p99=18.000 update_nosend_nolock_nodecrypt_noblock_p50=20.539 update_nosend_nolock_nodecrypt_noblock_p99=43.360 update_cpu_us_p50=22.412 update_cpu_us_p99=91.301 ack_visits_per_reply=1.0
round=3 build=base P=8  16:33:21 up 1 day, 15:13,  4 users,  load average: 97.53, 143.96, 120.87
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=100.259 send_us_p99=1198.827 send_us_max=5803.484 update_us_p50=152.368 update_us_p99=3643.740 update_us_max=12547.455 total_us_p50=250.292 total_us_p99=3867.983 total_us_max=12651.652 received=17592
round=3 build=lock P=8  16:33:24 up 1 day, 15:13,  4 users,  load average: 97.53, 143.96, 120.87
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=104.137 send_us_p99=3089.943 send_us_max=15200.312 update_us_p50=152.378 update_us_p99=3503.444 update_us_max=9291.900 total_us_p50=254.160 total_us_p99=4055.307 total_us_max=15399.729 received=17584
client_main_thread_parts packets_per_frame=8 n_p50=8.000 n_p99=51.000 swap_p50=0.301 swap_p99=2052.531 checks_p50=2.073 checks_p99=21.445 decrypt_p50=22.801 decrypt_p99=164.267 protocol_p50=98.056 protocol_p99=739.876 proto_update_p50=0.140 proto_update_p99=0.621 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=24.115 stats_p99=53.902 rest_p50=0.781 rest_p99=2.023 update_nosend_p50=152.378 update_nosend_p99=3503.444 update_nosend_nolock_p50=151.838 update_nosend_nolock_p99=1555.782 update_nosend_nolock_nodecrypt_p50=127.362 update_nosend_nolock_nodecrypt_p99=941.001 decrypt_ns_per_reply=4222.6 loop_ns_per_reply=19123.6 replies_per_frame=8.00
client_main_thread_split packets_per_frame=8 ack_read_p50=0.650 ack_read_p99=4.187 record_received_p50=0.471 record_received_p99=3.155 process_block_p50=92.433 process_block_p99=687.911 trackers_p50=0.571 trackers_p99=3.951 loss_tracker_update_p50=0.170 loss_tracker_update_p99=0.591 ack_update_stats_p50=22.413 ack_update_stats_p99=50.094 ack_visits_p50=32640.000 ack_visits_p99=195264.000 update_nosend_nolock_nodecrypt_noblock_p50=32.391 update_nosend_nolock_nodecrypt_noblock_p99=104.230 update_cpu_us_p50=154.309 update_cpu_us_p99=831.690 ack_visits_per_reply=3885.3
round=3 build=outside P=8  16:33:27 up 1 day, 15:13,  4 users,  load average: 96.37, 142.94, 120.66
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=106.451 send_us_p99=250.504 send_us_max=5165.567 update_us_p50=110.619 update_us_p99=1269.891 update_us_max=22140.816 total_us_p50=215.867 total_us_p99=3362.036 total_us_max=22248.249 received=17584
client_main_thread_parts packets_per_frame=8 n_p50=5.000 n_p99=56.000 swap_p50=0.300 swap_p99=1.393 checks_p50=1.935 checks_p99=25.021 decrypt_p50=15.168 decrypt_p99=171.181 protocol_p50=60.484 protocol_p99=859.425 proto_update_p50=0.141 proto_update_p99=0.631 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=24.787 stats_p99=43.151 rest_p50=0.791 rest_p99=1.824 update_nosend_p50=110.619 update_nosend_p99=1249.933 update_nosend_nolock_p50=110.128 update_nosend_nolock_p99=1249.583 update_nosend_nolock_nodecrypt_p50=90.642 update_nosend_nolock_nodecrypt_p99=1023.187 decrypt_ns_per_reply=3663.7 loop_ns_per_reply=21203.7 replies_per_frame=8.00
client_main_thread_split packets_per_frame=8 ack_read_p50=0.442 ack_read_p99=5.139 record_received_p50=0.320 record_received_p99=3.965 process_block_p50=56.787 process_block_p99=783.371 trackers_p50=0.391 trackers_p99=5.299 loss_tracker_update_p50=0.141 loss_tracker_update_p99=0.621 ack_update_stats_p50=23.274 ack_update_stats_p99=41.208 ack_visits_p50=20368.000 ack_visits_p99=223440.000 update_nosend_nolock_nodecrypt_noblock_p50=32.052 update_nosend_nolock_nodecrypt_noblock_p99=105.826 update_cpu_us_p50=105.197 update_cpu_us_p99=958.708 ack_visits_per_reply=3883.5
round=3 build=floor P=8  16:33:29 up 1 day, 15:13,  4 users,  load average: 96.37, 142.94, 120.66
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=109.366 send_us_p99=190.841 send_us_max=7254.267 update_us_p50=29.426 update_us_p99=333.490 update_us_max=3124.658 total_us_p50=146.929 total_us_p99=511.397 total_us_max=7416.814 received=17582
client_main_thread_parts packets_per_frame=8 n_p50=0.000 n_p99=64.000 swap_p50=0.341 swap_p99=1.393 checks_p50=0.000 checks_p99=22.262 decrypt_p50=0.000 decrypt_p99=183.861 protocol_p50=0.261 protocol_p99=58.972 proto_update_p50=0.160 proto_update_p99=10.550 proto_send_p50=0.000 proto_send_p99=16.992 stats_p50=24.416 stats_p99=49.774 rest_p50=0.782 rest_p99=1.993 update_nosend_p50=29.426 update_nosend_p99=318.592 update_nosend_nolock_p50=29.075 update_nosend_nolock_p99=313.824 update_nosend_nolock_nodecrypt_p50=28.544 update_nosend_nolock_nodecrypt_p99=120.317 decrypt_ns_per_reply=3058.2 loop_ns_per_reply=4289.2 replies_per_frame=8.00
client_main_thread_split packets_per_frame=8 ack_read_p50=0.000 ack_read_p99=5.866 record_received_p50=0.000 record_received_p99=4.626 process_block_p50=0.000 process_block_p99=6.950 trackers_p50=0.000 trackers_p99=5.524 loss_tracker_update_p50=0.101 loss_tracker_update_p99=0.641 ack_update_stats_p50=22.713 ack_update_stats_p99=45.656 ack_visits_p50=0.000 ack_visits_p99=64.000 update_nosend_nolock_nodecrypt_noblock_p50=28.434 update_nosend_nolock_nodecrypt_noblock_p99=114.256 update_cpu_us_p50=31.850 update_cpu_us_p99=314.840 ack_visits_per_reply=1.0
round=3 build=base P=32  16:33:31 up 1 day, 15:13,  4 users,  load average: 102.91, 143.53, 120.97
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=409.314 send_us_p99=4153.584 send_us_max=11529.491 update_us_p50=557.615 update_us_p99=4297.466 update_us_max=14715.295 total_us_p50=995.212 total_us_p99=6978.525 total_us_max=15081.858 received=70368
round=3 build=lock P=32  16:33:40 up 1 day, 15:14,  4 users,  load average: 104.04, 142.42, 120.85
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=390.328 send_us_p99=3483.878 send_us_max=9398.661 update_us_p50=555.831 update_us_p99=4300.122 update_us_max=10962.699 total_us_p50=966.487 total_us_p99=6038.920 total_us_max=11341.065 received=70368
client_main_thread_parts packets_per_frame=32 n_p50=32.000 n_p99=96.000 swap_p50=0.370 swap_p99=3119.589 checks_p50=9.184 checks_p99=36.548 decrypt_p50=89.702 decrypt_p99=367.549 protocol_p50=421.890 protocol_p99=3451.898 proto_update_p50=0.170 proto_update_p99=13.195 proto_send_p50=0.000 proto_send_p99=28.915 stats_p50=25.829 stats_p99=48.852 rest_p50=0.862 rest_p99=2.193 update_nosend_p50=555.831 update_nosend_p99=4300.122 update_nosend_nolock_p50=550.831 update_nosend_nolock_p99=3666.392 update_nosend_nolock_nodecrypt_p50=459.290 update_nosend_nolock_nodecrypt_p99=3490.066 decrypt_ns_per_reply=3903.8 loop_ns_per_reply=21237.8 replies_per_frame=32.00
client_main_thread_split packets_per_frame=32 ack_read_p50=2.578 ack_read_p99=8.006 record_received_p50=1.903 record_received_p99=5.881 process_block_p50=402.681 process_block_p99=3423.269 trackers_p50=2.231 trackers_p99=7.862 loss_tracker_update_p50=0.250 loss_tracker_update_p99=0.821 ack_update_stats_p50=23.895 ack_update_stats_p99=45.857 ack_visits_p50=130048.000 ack_visits_p99=387072.000 update_nosend_nolock_nodecrypt_noblock_p50=55.403 update_nosend_nolock_nodecrypt_noblock_p99=165.537 update_cpu_us_p50=551.695 update_cpu_us_p99=1568.672 ack_visits_per_reply=4059.1
round=3 build=outside P=32  16:33:49 up 1 day, 15:14,  4 users,  load average: 101.87, 141.34, 120.62
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=403.343 send_us_p99=3544.403 send_us_max=10513.399 update_us_p50=549.449 update_us_p99=4790.909 update_us_max=10746.932 total_us_p50=965.526 total_us_p99=7033.800 total_us_max=11256.063 received=70336
client_main_thread_parts packets_per_frame=32 n_p50=32.000 n_p99=96.000 swap_p50=0.521 swap_p99=1.893 checks_p50=10.672 checks_p99=32.874 decrypt_p50=92.785 decrypt_p99=309.263 protocol_p50=416.168 protocol_p99=3944.572 proto_update_p50=0.210 proto_update_p99=13.265 proto_send_p50=0.000 proto_send_p99=34.014 stats_p50=26.600 stats_p99=49.444 rest_p50=1.022 rest_p99=2.485 update_nosend_p50=549.449 update_nosend_p99=4720.797 update_nosend_nolock_p50=548.818 update_nosend_nolock_p99=4719.595 update_nosend_nolock_nodecrypt_p50=455.458 update_nosend_nolock_nodecrypt_p99=4115.269 decrypt_ns_per_reply=3452.8 loop_ns_per_reply=19882.2 replies_per_frame=31.99
client_main_thread_split packets_per_frame=32 ack_read_p50=2.594 ack_read_p99=8.024 record_received_p50=1.911 record_received_p99=6.892 process_block_p50=396.928 process_block_p99=3684.417 trackers_p50=2.391 trackers_p99=8.765 loss_tracker_update_p50=0.301 loss_tracker_update_p99=0.962 ack_update_stats_p50=24.336 ack_update_stats_p99=44.674 ack_visits_p50=130048.000 ack_visits_p99=387072.000 update_nosend_nolock_nodecrypt_noblock_p50=58.436 update_nosend_nolock_nodecrypt_noblock_p99=189.978 update_cpu_us_p50=550.853 update_cpu_us_p99=1553.864 ack_visits_per_reply=4056.5
round=3 build=floor P=32  16:33:57 up 1 day, 15:14,  4 users,  load average: 101.93, 140.02, 120.41
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=401.870 send_us_p99=822.736 send_us_max=10428.580 update_us_p50=147.018 update_us_p99=555.139 update_us_max=36277.547 total_us_p50=551.222 total_us_p99=3262.107 total_us_max=36698.143 received=70368
client_main_thread_parts packets_per_frame=32 n_p50=32.000 n_p99=96.000 swap_p50=0.361 swap_p99=1.963 checks_p50=7.127 checks_p99=38.564 decrypt_p50=90.314 decrypt_p99=304.781 protocol_p50=20.416 protocol_p99=103.414 proto_update_p50=0.161 proto_update_p99=13.826 proto_send_p50=0.000 proto_send_p99=25.739 stats_p50=26.069 stats_p99=50.636 rest_p50=0.951 rest_p99=2.565 update_nosend_p50=147.018 update_nosend_p99=537.406 update_nosend_nolock_p50=146.608 update_nosend_nolock_p99=536.724 update_nosend_nolock_nodecrypt_p50=55.712 update_nosend_nolock_nodecrypt_p99=198.296 decrypt_ns_per_reply=2865.1 loop_ns_per_reply=4030.4 replies_per_frame=32.00
client_main_thread_split packets_per_frame=32 ack_read_p50=2.616 ack_read_p99=10.406 record_received_p50=1.901 record_received_p99=8.744 process_block_p50=3.010 process_block_p99=13.451 trackers_p50=2.052 trackers_p99=11.653 loss_tracker_update_p50=0.250 loss_tracker_update_p99=1.132 ack_update_stats_p50=24.235 ack_update_stats_p99=46.398 ack_visits_p50=32.000 ack_visits_p99=97.000 update_nosend_nolock_nodecrypt_noblock_p50=52.479 update_nosend_nolock_nodecrypt_noblock_p99=184.845 update_cpu_us_p50=149.170 update_cpu_us_p99=510.257 ack_visits_per_reply=1.0
DONE
```

Set 1, in run order:

```
round=1 build=base P=1  16:29:17 up 1 day, 15:09,  4 users,  load average: 176.44, 170.06, 118.24
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=13.345 send_us_p99=36.009 send_us_max=3482.313 update_us_p50=19.077 update_us_p99=250.707 update_us_max=8963.459 total_us_p50=33.654 total_us_p99=305.511 total_us_max=8976.173 received=2191
round=1 build=lock P=1  16:29:18 up 1 day, 15:09,  4 users,  load average: 176.44, 170.06, 118.24
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=11.972 send_us_p99=75.654 send_us_max=6041.080 update_us_p50=19.578 update_us_p99=912.746 update_us_max=8061.774 total_us_p50=31.661 total_us_p99=1263.002 total_us_max=8073.667 received=2193
client_main_thread_parts packets_per_frame=1 n_p50=0.000 n_p99=14.000 swap_p50=0.130 swap_p99=0.581 checks_p50=0.000 checks_p99=4.189 decrypt_p50=0.000 decrypt_p99=40.215 protocol_p50=0.070 protocol_p99=75.165 proto_update_p50=0.120 proto_update_p99=0.781 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=18.475 stats_p99=93.307 rest_p50=0.421 rest_p99=1.794 update_nosend_p50=19.578 update_nosend_p99=912.746 update_nosend_nolock_p50=19.407 update_nosend_nolock_p99=912.296 update_nosend_nolock_nodecrypt_p50=19.407 update_nosend_nolock_nodecrypt_p99=882.600 decrypt_ns_per_reply=2950.3 loop_ns_per_reply=8839.3 replies_per_frame=1.00
client_main_thread_split packets_per_frame=1 ack_read_p50=0.000 ack_read_p99=1.343 record_received_p50=0.000 record_received_p99=0.952 process_block_p50=0.000 process_block_p99=67.026 trackers_p50=0.000 trackers_p99=1.294 loss_tracker_update_p50=0.060 loss_tracker_update_p99=0.421 ack_update_stats_p50=17.333 ack_update_stats_p99=91.875 ack_visits_p50=0.000 ack_visits_p99=22022.000 update_nosend_nolock_nodecrypt_noblock_p50=19.407 update_nosend_nolock_nodecrypt_noblock_p99=273.060 update_cpu_us_p50=21.240 update_cpu_us_p99=137.688 ack_visits_per_reply=1194.5
round=1 build=outside P=1  16:29:19 up 1 day, 15:09,  4 users,  load average: 176.44, 170.06, 118.24
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=13.927 send_us_p99=32.472 send_us_max=11870.968 update_us_p50=22.123 update_us_p99=163.762 update_us_max=8998.135 total_us_p50=36.661 total_us_p99=208.677 total_us_max=11994.773 received=2193
client_main_thread_parts packets_per_frame=1 n_p50=0.000 n_p99=14.000 swap_p50=0.160 swap_p99=1.623 checks_p50=0.000 checks_p99=6.219 decrypt_p50=0.000 decrypt_p99=44.574 protocol_p50=0.080 protocol_p99=77.659 proto_update_p50=0.090 proto_update_p99=0.471 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=20.499 stats_p99=40.537 rest_p50=0.480 rest_p99=1.454 update_nosend_p50=22.123 update_nosend_p99=163.762 update_nosend_nolock_p50=21.932 update_nosend_nolock_p99=163.180 update_nosend_nolock_nodecrypt_p50=21.861 update_nosend_nolock_nodecrypt_p99=115.930 decrypt_ns_per_reply=3273.2 loop_ns_per_reply=8557.9 replies_per_frame=1.00
client_main_thread_split packets_per_frame=1 ack_read_p50=0.000 ack_read_p99=1.462 record_received_p50=0.000 record_received_p99=1.122 process_block_p50=0.000 process_block_p99=70.022 trackers_p50=0.000 trackers_p99=1.381 loss_tracker_update_p50=0.070 loss_tracker_update_p99=0.421 ack_update_stats_p50=19.167 ack_update_stats_p99=38.673 ack_visits_p50=0.000 ack_visits_p99=21575.000 update_nosend_nolock_nodecrypt_noblock_p50=21.761 update_nosend_nolock_nodecrypt_noblock_p99=50.927 update_cpu_us_p50=24.225 update_cpu_us_p99=156.252 ack_visits_per_reply=1196.0
round=1 build=base P=8  16:29:20 up 1 day, 15:09,  4 users,  load average: 176.44, 170.06, 118.24
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=106.012 send_us_p99=1253.545 send_us_max=12184.474 update_us_p50=156.488 update_us_p99=3336.436 update_us_max=18273.495 total_us_p50=258.441 total_us_p99=3662.145 total_us_max=18408.592 received=17592
round=1 build=lock P=8  16:29:22 up 1 day, 15:09,  4 users,  load average: 171.04, 169.05, 118.19
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=113.506 send_us_p99=236.159 send_us_max=5854.374 update_us_p50=152.480 update_us_p99=1750.799 update_us_max=4481.714 total_us_p50=256.959 total_us_p99=2506.588 total_us_max=6201.084 received=17592
client_main_thread_parts packets_per_frame=8 n_p50=8.000 n_p99=56.000 swap_p50=0.380 swap_p99=180.132 checks_p50=1.693 checks_p99=27.249 decrypt_p50=23.184 decrypt_p99=177.099 protocol_p50=95.272 protocol_p99=891.498 proto_update_p50=0.170 proto_update_p99=0.681 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=25.569 stats_p99=87.126 rest_p50=0.884 rest_p99=1.943 update_nosend_p50=152.480 update_nosend_p99=1733.787 update_nosend_nolock_p50=152.039 update_nosend_nolock_p99=1335.070 update_nosend_nolock_nodecrypt_p50=125.679 update_nosend_nolock_nodecrypt_p99=1182.269 decrypt_ns_per_reply=3259.0 loop_ns_per_reply=18030.9 replies_per_frame=8.00
client_main_thread_split packets_per_frame=8 ack_read_p50=0.655 ack_read_p99=5.775 record_received_p50=0.462 record_received_p99=4.181 process_block_p50=89.521 process_block_p99=830.136 trackers_p50=0.552 trackers_p99=5.148 loss_tracker_update_p50=0.160 loss_tracker_update_p99=0.672 ack_update_stats_p50=23.725 ack_update_stats_p99=85.022 ack_visits_p50=29824.000 ack_visits_p99=195264.000 update_nosend_nolock_nodecrypt_noblock_p50=33.466 update_nosend_nolock_nodecrypt_noblock_p99=147.994 update_cpu_us_p50=155.422 update_cpu_us_p99=952.877 ack_visits_per_reply=3885.0
round=1 build=outside P=8  16:29:25 up 1 day, 15:09,  4 users,  load average: 171.04, 169.05, 118.19
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=104.058 send_us_p99=355.376 send_us_max=22518.005 update_us_p50=32.111 update_us_p99=1779.855 update_us_max=19390.177 total_us_p50=151.688 total_us_p99=3452.517 total_us_max=22829.558 received=17584
client_main_thread_parts packets_per_frame=8 n_p50=0.000 n_p99=64.000 swap_p50=0.371 swap_p99=2.595 checks_p50=0.000 checks_p99=27.852 decrypt_p50=0.000 decrypt_p99=202.634 protocol_p50=0.371 protocol_p99=1170.600 proto_update_p50=0.160 proto_update_p99=5.651 proto_send_p50=0.000 proto_send_p99=18.445 stats_p50=24.436 stats_p99=52.500 rest_p50=0.941 rest_p99=2.735 update_nosend_p50=32.111 update_nosend_p99=1779.855 update_nosend_nolock_p50=31.451 update_nosend_nolock_p99=1540.509 update_nosend_nolock_nodecrypt_p50=31.299 update_nosend_nolock_nodecrypt_p99=1252.661 decrypt_ns_per_reply=3961.6 loop_ns_per_reply=22417.5 replies_per_frame=8.00
client_main_thread_split packets_per_frame=8 ack_read_p50=0.000 ack_read_p99=6.824 record_received_p50=0.000 record_received_p99=5.381 process_block_p50=0.000 process_block_p99=1092.589 trackers_p50=0.000 trackers_p99=7.201 loss_tracker_update_p50=0.130 loss_tracker_update_p99=0.932 ack_update_stats_p50=22.462 ack_update_stats_p99=48.202 ack_visits_p50=0.000 ack_visits_p99=228096.000 update_nosend_nolock_nodecrypt_noblock_p50=29.346 update_nosend_nolock_nodecrypt_noblock_p99=138.249 update_cpu_us_p50=34.645 update_cpu_us_p99=1103.209 ack_visits_per_reply=3878.9
round=1 build=base P=32  16:29:28 up 1 day, 15:09,  4 users,  load average: 169.91, 168.85, 118.40
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=441.069 send_us_p99=18633.363 send_us_max=38571.088 update_us_p50=605.311 update_us_p99=21323.259 update_us_max=45270.271 total_us_p50=1094.361 total_us_p99=24247.654 total_us_max=45654.291 received=70368
round=1 build=lock P=32  16:29:52 up 1 day, 15:10,  4 users,  load average: 178.47, 171.18, 120.53
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=436.069 send_us_p99=16582.108 send_us_max=27322.928 update_us_p50=597.466 update_us_p99=11537.255 update_us_max=22180.409 total_us_p50=1075.414 total_us_p99=19094.150 total_us_max=29782.554 received=70368
client_main_thread_parts packets_per_frame=32 n_p50=32.000 n_p99=96.000 swap_p50=0.571 swap_p99=3211.165 checks_p50=11.550 checks_p99=44.907 decrypt_p50=99.288 decrypt_p99=3122.755 protocol_p50=447.130 protocol_p99=9526.296 proto_update_p50=0.481 proto_update_p99=15.019 proto_send_p50=0.000 proto_send_p99=75.072 stats_p50=30.488 stats_p99=76.796 rest_p50=1.524 rest_p99=3.808 update_nosend_p50=597.466 update_nosend_p99=11405.263 update_nosend_nolock_p50=594.820 update_nosend_nolock_p99=10715.904 update_nosend_nolock_nodecrypt_p50=492.826 update_nosend_nolock_nodecrypt_p99=9967.827 decrypt_ns_per_reply=5532.8 loop_ns_per_reply=33658.4 replies_per_frame=32.00
client_main_thread_split packets_per_frame=32 ack_read_p50=2.862 ack_read_p99=9.040 record_received_p50=2.124 record_received_p99=7.334 process_block_p50=424.176 process_block_p99=9453.646 trackers_p50=2.657 trackers_p99=11.683 loss_tracker_update_p50=0.481 loss_tracker_update_p99=1.272 ack_update_stats_p50=27.362 ack_update_stats_p99=58.531 ack_visits_p50=130048.000 ack_visits_p99=387072.000 update_nosend_nolock_nodecrypt_noblock_p50=67.749 update_nosend_nolock_nodecrypt_noblock_p99=1181.780 update_cpu_us_p50=593.483 update_cpu_us_p99=1731.778 ack_visits_per_reply=4057.6
round=1 build=outside P=32  16:30:09 up 1 day, 15:10,  4 users,  load average: 176.57, 171.17, 121.34
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=421.872 send_us_p99=6457.084 send_us_max=12545.488 update_us_p50=577.027 update_us_p99=6894.505 update_us_max=22378.369 total_us_p50=1009.038 total_us_p99=9970.632 total_us_max=22810.139 received=70368
client_main_thread_parts packets_per_frame=32 n_p50=32.000 n_p99=96.000 swap_p50=0.571 swap_p99=1.954 checks_p50=11.313 checks_p99=37.037 decrypt_p50=95.292 decrypt_p99=376.638 protocol_p50=437.233 protocol_p99=6502.693 proto_update_p50=0.280 proto_update_p99=13.917 proto_send_p50=0.000 proto_send_p99=33.524 stats_p50=28.143 stats_p99=50.967 rest_p50=1.081 rest_p99=2.574 update_nosend_p50=577.027 update_nosend_p99=6894.505 update_nosend_nolock_p50=576.215 update_nosend_nolock_p99=6892.832 update_nosend_nolock_nodecrypt_p50=479.530 update_nosend_nolock_nodecrypt_p99=6552.461 decrypt_ns_per_reply=4658.9 loop_ns_per_reply=24483.0 replies_per_frame=32.00
client_main_thread_split packets_per_frame=32 ack_read_p50=2.680 ack_read_p99=8.306 record_received_p50=1.992 record_received_p99=6.522 process_block_p50=416.581 process_block_p99=6447.679 trackers_p50=2.475 trackers_p99=9.243 loss_tracker_update_p50=0.311 loss_tracker_update_p99=0.932 ack_update_stats_p50=25.749 ack_update_stats_p99=46.187 ack_visits_p50=130048.000 ack_visits_p99=387072.000 update_nosend_nolock_nodecrypt_noblock_p50=61.236 update_nosend_nolock_nodecrypt_noblock_p99=182.858 update_cpu_us_p50=577.654 update_cpu_us_p99=1683.236 ack_visits_per_reply=4057.7
round=2 build=base P=1  16:30:18 up 1 day, 15:10,  4 users,  load average: 169.85, 169.96, 121.48
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=12.493 send_us_p99=60.926 send_us_max=1259.845 update_us_p50=21.872 update_us_p99=182.437 update_us_max=3624.981 total_us_p50=34.606 total_us_p99=355.817 total_us_max=3637.585 received=2198
round=2 build=lock P=1  16:30:19 up 1 day, 15:10,  4 users,  load average: 169.85, 169.96, 121.48
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=12.243 send_us_p99=65.805 send_us_max=2053.964 update_us_p50=20.479 update_us_p99=184.792 update_us_max=5101.827 total_us_p50=32.903 total_us_p99=202.103 total_us_max=5115.303 received=2191
client_main_thread_parts packets_per_frame=1 n_p50=0.000 n_p99=14.000 swap_p50=0.130 swap_p99=26.731 checks_p50=0.000 checks_p99=4.258 decrypt_p50=0.000 decrypt_p99=42.021 protocol_p50=0.070 protocol_p99=71.646 proto_update_p50=0.080 proto_update_p99=0.331 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=19.437 stats_p99=75.373 rest_p50=0.450 rest_p99=1.271 update_nosend_p50=20.479 update_nosend_p99=184.792 update_nosend_nolock_p50=20.339 update_nosend_nolock_p99=176.755 update_nosend_nolock_nodecrypt_p50=20.339 update_nosend_nolock_nodecrypt_p99=141.511 decrypt_ns_per_reply=2898.4 loop_ns_per_reply=7632.7 replies_per_frame=1.00
client_main_thread_split packets_per_frame=1 ack_read_p50=0.000 ack_read_p99=1.312 record_received_p50=0.000 record_received_p99=0.931 process_block_p50=0.000 process_block_p99=65.163 trackers_p50=0.000 trackers_p99=1.242 loss_tracker_update_p50=0.060 loss_tracker_update_p99=0.340 ack_update_stats_p50=18.295 ack_update_stats_p99=74.362 ack_visits_p50=0.000 ack_visits_p99=21045.000 update_nosend_nolock_nodecrypt_noblock_p50=20.329 update_nosend_nolock_nodecrypt_noblock_p99=84.934 update_cpu_us_p50=22.181 update_cpu_us_p99=140.032 ack_visits_per_reply=1195.2
round=2 build=outside P=1  16:30:20 up 1 day, 15:10,  4 users,  load average: 169.85, 169.96, 121.48
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=12.694 send_us_p99=27.603 send_us_max=6046.734 update_us_p50=19.717 update_us_p99=156.898 update_us_max=3085.094 total_us_p50=32.873 total_us_p99=172.748 total_us_max=6200.867 received=2196
client_main_thread_parts packets_per_frame=1 n_p50=0.000 n_p99=17.000 swap_p50=0.140 swap_p99=0.962 checks_p50=0.000 checks_p99=6.784 decrypt_p50=0.000 decrypt_p99=46.390 protocol_p50=0.060 protocol_p99=81.725 proto_update_p50=0.071 proto_update_p99=0.421 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=18.355 stats_p99=35.117 rest_p50=0.402 rest_p99=1.283 update_nosend_p50=19.717 update_nosend_p99=156.898 update_nosend_nolock_p50=19.557 update_nosend_nolock_p99=156.577 update_nosend_nolock_nodecrypt_p50=19.556 update_nosend_nolock_nodecrypt_p99=109.972 decrypt_ns_per_reply=2831.2 loop_ns_per_reply=7751.9 replies_per_frame=1.00
client_main_thread_split packets_per_frame=1 ack_read_p50=0.000 ack_read_p99=1.542 record_received_p50=0.000 record_received_p99=1.182 process_block_p50=0.000 process_block_p99=70.784 trackers_p50=0.000 trackers_p99=1.352 loss_tracker_update_p50=0.060 loss_tracker_update_p99=0.311 ack_update_stats_p50=17.243 ack_update_stats_p99=33.984 ack_visits_p50=0.000 ack_visits_p99=23199.000 update_nosend_nolock_nodecrypt_noblock_p50=19.518 update_nosend_nolock_nodecrypt_noblock_p99=42.771 update_cpu_us_p50=21.390 update_cpu_us_p99=158.567 ack_visits_per_reply=1197.0
round=2 build=base P=8  16:30:20 up 1 day, 15:10,  4 users,  load average: 165.62, 169.08, 121.46
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=106.332 send_us_p99=1235.999 send_us_max=12244.103 update_us_p50=155.686 update_us_p99=3898.210 update_us_max=15945.469 total_us_p50=258.231 total_us_p99=5496.558 total_us_max=16069.776 received=17592
round=2 build=lock P=8  16:30:24 up 1 day, 15:10,  4 users,  load average: 165.62, 169.08, 121.46
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=108.276 send_us_p99=2171.488 send_us_max=9569.619 update_us_p50=161.527 update_us_p99=3420.762 update_us_max=12805.129 total_us_p50=268.450 total_us_p99=4356.290 total_us_max=12916.731 received=17592
client_main_thread_parts packets_per_frame=8 n_p50=8.000 n_p99=52.000 swap_p50=0.320 swap_p99=193.247 checks_p50=2.135 checks_p99=22.623 decrypt_p50=24.285 decrypt_p99=172.429 protocol_p50=104.307 protocol_p99=856.228 proto_update_p50=0.170 proto_update_p99=0.812 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=26.360 stats_p99=78.258 rest_p50=0.891 rest_p99=2.444 update_nosend_p50=161.527 update_nosend_p99=3420.762 update_nosend_nolock_p50=160.705 update_nosend_nolock_p99=1198.607 update_nosend_nolock_nodecrypt_p50=134.897 update_nosend_nolock_nodecrypt_p99=970.503 decrypt_ns_per_reply=3839.0 loop_ns_per_reply=20570.9 replies_per_frame=8.00
client_main_thread_split packets_per_frame=8 ack_read_p50=0.691 ack_read_p99=4.996 record_received_p50=0.480 record_received_p99=3.807 process_block_p50=98.409 process_block_p99=800.542 trackers_p50=0.641 trackers_p99=4.976 loss_tracker_update_p50=0.180 loss_tracker_update_p99=0.682 ack_update_stats_p50=24.416 ack_update_stats_p99=69.603 ack_visits_p50=32640.000 ack_visits_p99=203344.000 update_nosend_nolock_nodecrypt_noblock_p50=35.568 update_nosend_nolock_nodecrypt_noblock_p99=120.841 update_cpu_us_p50=163.427 update_cpu_us_p99=958.407 ack_visits_per_reply=3883.0
round=2 build=outside P=8  16:30:27 up 1 day, 15:10,  4 users,  load average: 162.68, 168.41, 121.50
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=113.997 send_us_p99=294.530 send_us_max=15145.176 update_us_p50=155.276 update_us_p99=935.769 update_us_max=4213.760 total_us_p50=267.519 total_us_p99=1354.053 total_us_max=15814.328 received=17568
client_main_thread_parts packets_per_frame=8 n_p50=8.000 n_p99=48.000 swap_p50=0.361 swap_p99=2.234 checks_p50=3.487 checks_p99=23.245 decrypt_p50=23.977 decrypt_p99=173.563 protocol_p50=98.498 protocol_p99=709.320 proto_update_p50=0.131 proto_update_p99=0.551 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=26.200 stats_p99=44.454 rest_p50=0.881 rest_p99=1.774 update_nosend_p50=155.276 update_nosend_p99=935.769 update_nosend_nolock_p50=154.784 update_nosend_nolock_p99=927.172 update_nosend_nolock_nodecrypt_p50=129.167 update_nosend_nolock_nodecrypt_p99=756.807 decrypt_ns_per_reply=3713.9 loop_ns_per_reply=18294.1 replies_per_frame=7.99
client_main_thread_split packets_per_frame=8 ack_read_p50=0.660 ack_read_p99=4.786 record_received_p50=0.481 record_received_p99=3.582 process_block_p50=92.485 process_block_p99=657.724 trackers_p50=0.571 trackers_p99=4.450 loss_tracker_update_p50=0.141 loss_tracker_update_p99=0.551 ack_update_stats_p50=24.397 ack_update_stats_p99=41.929 ack_visits_p50=28666.000 ack_visits_p99=195264.000 update_nosend_nolock_nodecrypt_noblock_p50=35.217 update_nosend_nolock_nodecrypt_noblock_p99=109.504 update_cpu_us_p50=157.416 update_cpu_us_p99=911.019 ack_visits_per_reply=3884.2
round=2 build=base P=32  16:30:29 up 1 day, 15:10,  4 users,  load average: 162.68, 168.41, 121.50
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=414.518 send_us_p99=6454.136 send_us_max=24601.377 update_us_p50=567.820 update_us_p99=6603.801 update_us_max=17612.583 total_us_p50=991.985 total_us_p99=8364.417 total_us_max=25175.899 received=70368
round=2 build=lock P=32  16:30:39 up 1 day, 15:11,  4 users,  load average: 155.33, 166.65, 121.43
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=439.665 send_us_p99=10462.272 send_us_max=15503.192 update_us_p50=606.062 update_us_p99=9657.984 update_us_max=14464.378 total_us_p50=1098.006 total_us_p99=12072.291 total_us_max=16149.530 received=70368
client_main_thread_parts packets_per_frame=32 n_p50=32.000 n_p99=64.000 swap_p50=0.882 swap_p99=2988.449 checks_p50=12.322 checks_p99=30.667 decrypt_p50=100.099 decrypt_p99=3128.526 protocol_p50=453.825 protocol_p99=8442.942 proto_update_p50=0.491 proto_update_p99=14.217 proto_send_p50=0.000 proto_send_p99=62.689 stats_p50=32.182 stats_p99=75.113 rest_p50=1.643 rest_p99=3.717 update_nosend_p50=606.062 update_nosend_p99=9657.984 update_nosend_nolock_p50=603.909 update_nosend_nolock_p99=9629.949 update_nosend_nolock_nodecrypt_p50=502.073 update_nosend_nolock_nodecrypt_p99=9305.035 decrypt_ns_per_reply=5465.8 loop_ns_per_reply=32123.9 replies_per_frame=32.00
client_main_thread_split packets_per_frame=32 ack_read_p50=2.900 ack_read_p99=6.928 record_received_p50=2.144 record_received_p99=5.632 process_block_p50=429.929 process_block_p99=7366.752 trackers_p50=2.694 trackers_p99=7.212 loss_tracker_update_p50=0.511 loss_tracker_update_p99=1.232 ack_update_stats_p50=28.685 ack_update_stats_p99=68.340 ack_visits_p50=130048.000 ack_visits_p99=259072.000 update_nosend_nolock_nodecrypt_noblock_p50=71.324 update_nosend_nolock_nodecrypt_noblock_p99=1150.993 update_cpu_us_p50=607.369 update_cpu_us_p99=1311.670 ack_visits_per_reply=4061.2
round=2 build=outside P=32  16:30:57 up 1 day, 15:11,  4 users,  load average: 171.11, 169.63, 123.38
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=417.975 send_us_p99=6615.811 send_us_max=12470.845 update_us_p50=588.909 update_us_p99=6713.516 update_us_max=19234.035 total_us_p50=1031.320 total_us_p99=9372.239 total_us_max=19588.959 received=70346
client_main_thread_parts packets_per_frame=32 n_p50=32.000 n_p99=72.000 swap_p50=0.441 swap_p99=2.014 checks_p50=9.130 checks_p99=35.633 decrypt_p50=96.785 decrypt_p99=1314.134 protocol_p50=447.452 protocol_p99=6483.818 proto_update_p50=0.381 proto_update_p99=13.476 proto_send_p50=0.000 proto_send_p99=32.702 stats_p50=28.955 stats_p99=57.699 rest_p50=1.092 rest_p99=2.686 update_nosend_p50=588.909 update_nosend_p99=6713.516 update_nosend_nolock_p50=588.388 update_nosend_nolock_p99=6710.981 update_nosend_nolock_nodecrypt_p50=489.570 update_nosend_nolock_nodecrypt_p99=6590.983 decrypt_ns_per_reply=4260.2 loop_ns_per_reply=25504.2 replies_per_frame=31.99
client_main_thread_split packets_per_frame=32 ack_read_p50=2.714 ack_read_p99=7.956 record_received_p50=2.015 record_received_p99=6.927 process_block_p50=426.298 process_block_p99=6448.886 trackers_p50=2.576 trackers_p99=7.895 loss_tracker_update_p50=0.411 loss_tracker_update_p99=1.232 ack_update_stats_p50=26.220 ack_update_stats_p99=52.439 ack_visits_p50=130048.000 ack_visits_p99=289536.000 update_nosend_nolock_nodecrypt_noblock_p50=62.367 update_nosend_nolock_nodecrypt_noblock_p99=411.115 update_cpu_us_p50=589.756 update_cpu_us_p99=1407.400 ack_visits_per_reply=4057.9
round=3 build=base P=1  16:31:09 up 1 day, 15:11,  4 users,  load average: 196.69, 175.40, 125.76
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=12.764 send_us_p99=31.399 send_us_max=97.315 update_us_p50=18.505 update_us_p99=262.258 update_us_max=3653.252 total_us_p50=32.011 total_us_p99=276.435 total_us_max=3669.944 received=2197
round=3 build=lock P=1  16:31:10 up 1 day, 15:11,  4 users,  load average: 196.69, 175.40, 125.76
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=12.845 send_us_p99=29.666 send_us_max=670.894 update_us_p50=19.868 update_us_p99=233.784 update_us_max=3459.643 total_us_p50=32.281 total_us_p99=247.250 total_us_max=3473.940 received=2189
client_main_thread_parts packets_per_frame=1 n_p50=0.000 n_p99=18.000 swap_p50=0.120 swap_p99=65.575 checks_p50=0.000 checks_p99=6.843 decrypt_p50=0.000 decrypt_p99=49.815 protocol_p50=0.080 protocol_p99=75.834 proto_update_p50=0.120 proto_update_p99=0.200 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=18.786 stats_p99=35.588 rest_p50=0.443 rest_p99=1.132 update_nosend_p50=19.868 update_nosend_p99=233.784 update_nosend_nolock_p50=19.738 update_nosend_nolock_p99=172.578 update_nosend_nolock_nodecrypt_p50=19.738 update_nosend_nolock_nodecrypt_p99=128.974 decrypt_ns_per_reply=2805.3 loop_ns_per_reply=7581.4 replies_per_frame=1.00
client_main_thread_split packets_per_frame=1 ack_read_p50=0.000 ack_read_p99=1.394 record_received_p50=0.000 record_received_p99=1.073 process_block_p50=0.000 process_block_p99=67.899 trackers_p50=0.000 trackers_p99=1.133 loss_tracker_update_p50=0.060 loss_tracker_update_p99=0.260 ack_update_stats_p50=17.744 ack_update_stats_p99=34.055 ack_visits_p50=0.000 ack_visits_p99=21295.000 update_nosend_nolock_nodecrypt_noblock_p50=19.718 update_nosend_nolock_nodecrypt_noblock_p99=52.869 update_cpu_us_p50=21.470 update_cpu_us_p99=163.236 ack_visits_per_reply=1187.5
round=3 build=outside P=1  16:31:10 up 1 day, 15:11,  4 users,  load average: 191.19, 174.62, 125.78
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=12.794 send_us_p99=27.151 send_us_max=63.240 update_us_p50=20.970 update_us_p99=138.463 update_us_max=763.951 total_us_p50=33.533 total_us_p99=151.739 total_us_max=776.174 received=2192
client_main_thread_parts packets_per_frame=1 n_p50=0.000 n_p99=14.000 swap_p50=0.110 swap_p99=0.611 checks_p50=0.000 checks_p99=4.888 decrypt_p50=0.000 decrypt_p99=44.112 protocol_p50=0.090 protocol_p99=75.594 proto_update_p50=0.120 proto_update_p99=0.200 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=19.607 stats_p99=36.169 rest_p50=0.421 rest_p99=0.941 update_nosend_p50=20.970 update_nosend_p99=138.463 update_nosend_nolock_p50=20.860 update_nosend_nolock_p99=138.282 update_nosend_nolock_nodecrypt_p50=20.840 update_nosend_nolock_nodecrypt_p99=102.776 decrypt_ns_per_reply=3352.8 loop_ns_per_reply=8272.9 replies_per_frame=1.00
client_main_thread_split packets_per_frame=1 ack_read_p50=0.000 ack_read_p99=1.181 record_received_p50=0.000 record_received_p99=0.862 process_block_p50=0.000 process_block_p99=68.770 trackers_p50=0.000 trackers_p99=1.000 loss_tracker_update_p50=0.061 loss_tracker_update_p99=0.220 ack_update_stats_p50=18.565 ack_update_stats_p99=35.146 ack_visits_p50=0.000 ack_visits_p99=20545.000 update_nosend_nolock_nodecrypt_noblock_p50=20.749 update_nosend_nolock_nodecrypt_noblock_p99=40.846 update_cpu_us_p50=22.793 update_cpu_us_p99=136.496 ack_visits_per_reply=1194.0
round=3 build=base P=8  16:31:11 up 1 day, 15:11,  4 users,  load average: 191.19, 174.62, 125.78
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=103.557 send_us_p99=207.153 send_us_max=2082.627 update_us_p50=141.379 update_us_p99=1425.388 update_us_max=3769.051 total_us_p50=239.595 total_us_p99=1542.210 total_us_max=3879.160 received=17584
round=3 build=lock P=8  16:31:13 up 1 day, 15:11,  4 users,  load average: 191.19, 174.62, 125.78
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=102.755 send_us_p99=273.550 send_us_max=2359.332 update_us_p50=140.396 update_us_p99=1073.760 update_us_max=4540.145 total_us_p50=250.446 total_us_p99=1363.761 total_us_max=4648.901 received=17576
client_main_thread_parts packets_per_frame=8 n_p50=8.000 n_p99=56.000 swap_p50=0.231 swap_p99=153.561 checks_p50=1.494 checks_p99=20.529 decrypt_p50=22.371 decrypt_p99=162.160 protocol_p50=87.334 protocol_p99=763.338 proto_update_p50=0.140 proto_update_p99=0.371 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=24.647 stats_p99=64.583 rest_p50=0.851 rest_p99=1.373 update_nosend_p50=140.396 update_nosend_p99=1073.760 update_nosend_nolock_p50=140.005 update_nosend_nolock_p99=969.433 update_nosend_nolock_nodecrypt_p50=114.826 update_nosend_nolock_nodecrypt_p99=814.186 decrypt_ns_per_reply=3085.6 loop_ns_per_reply=17158.4 replies_per_frame=8.00
client_main_thread_split packets_per_frame=8 ack_read_p50=0.642 ack_read_p99=4.478 record_received_p50=0.460 record_received_p99=3.357 process_block_p50=80.394 process_block_p99=715.577 trackers_p50=0.531 trackers_p99=3.773 loss_tracker_update_p50=0.160 loss_tracker_update_p99=0.400 ack_update_stats_p50=23.114 ack_update_stats_p99=58.912 ack_visits_p50=26592.000 ack_visits_p99=217392.000 update_nosend_nolock_nodecrypt_noblock_p50=32.562 update_nosend_nolock_nodecrypt_noblock_p99=110.369 update_cpu_us_p50=141.756 update_cpu_us_p99=918.352 ack_visits_per_reply=3882.6
round=3 build=outside P=8  16:31:15 up 1 day, 15:11,  4 users,  load average: 184.45, 173.49, 125.68
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=103.607 send_us_p99=276.685 send_us_max=4683.899 update_us_p50=142.741 update_us_p99=954.414 update_us_max=5904.198 total_us_p50=248.162 total_us_p99=1081.155 total_us_max=6018.816 received=17584
client_main_thread_parts packets_per_frame=8 n_p50=8.000 n_p99=52.000 swap_p50=0.301 swap_p99=1.893 checks_p50=2.403 checks_p99=18.347 decrypt_p50=21.961 decrypt_p99=160.093 protocol_p50=90.592 protocol_p99=722.836 proto_update_p50=0.141 proto_update_p99=0.491 proto_send_p50=0.000 proto_send_p99=0.000 stats_p50=24.267 stats_p99=44.294 rest_p50=0.851 rest_p99=1.433 update_nosend_p50=142.741 update_nosend_p99=951.578 update_nosend_nolock_p50=142.481 update_nosend_nolock_p99=947.821 update_nosend_nolock_nodecrypt_p50=118.295 update_nosend_nolock_nodecrypt_p99=784.291 decrypt_ns_per_reply=3075.7 loop_ns_per_reply=17380.8 replies_per_frame=8.00
client_main_thread_split packets_per_frame=8 ack_read_p50=0.602 ack_read_p99=4.207 record_received_p50=0.442 record_received_p99=3.245 process_block_p50=84.092 process_block_p99=684.967 trackers_p50=0.541 trackers_p99=3.896 loss_tracker_update_p50=0.160 loss_tracker_update_p99=0.421 ack_update_stats_p50=22.753 ack_update_stats_p99=42.340 ack_visits_p50=28616.000 ack_visits_p99=199995.000 update_nosend_nolock_nodecrypt_noblock_p50=32.192 update_nosend_nolock_nodecrypt_noblock_p99=106.993 update_cpu_us_p50=144.581 update_cpu_us_p99=875.012 ack_visits_per_reply=3883.8
round=3 build=base P=32  16:31:17 up 1 day, 15:11,  4 users,  load average: 184.45, 173.49, 125.68
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=398.467 send_us_p99=3473.979 send_us_max=12485.861 update_us_p50=553.301 update_us_p99=6531.709 update_us_max=17538.338 total_us_p50=968.901 total_us_p99=6938.601 total_us_max=17917.488 received=70358
round=3 build=lock P=32  16:31:27 up 1 day, 15:11,  4 users,  load average: 171.19, 171.01, 125.38
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=415.690 send_us_p99=3697.634 send_us_max=9486.942 update_us_p50=586.123 update_us_p99=6568.307 update_us_max=11587.512 total_us_p50=1034.155 total_us_p99=6998.234 total_us_max=12005.375 received=70368
client_main_thread_parts packets_per_frame=32 n_p50=32.000 n_p99=71.000 swap_p50=0.380 swap_p99=503.376 checks_p50=8.817 checks_p99=32.735 decrypt_p50=93.594 decrypt_p99=1208.269 protocol_p50=424.575 protocol_p99=4762.920 proto_update_p50=0.201 proto_update_p99=13.115 proto_send_p50=0.000 proto_send_p99=29.846 stats_p50=48.312 stats_p99=164.202 rest_p50=0.963 rest_p99=1.952 update_nosend_p50=586.123 update_nosend_p99=6568.307 update_nosend_nolock_p50=581.464 update_nosend_nolock_p99=5708.915 update_nosend_nolock_nodecrypt_p50=484.592 update_nosend_nolock_nodecrypt_p99=4826.420 decrypt_ns_per_reply=4318.4 loop_ns_per_reply=23156.7 replies_per_frame=32.00
client_main_thread_split packets_per_frame=32 ack_read_p50=2.694 ack_read_p99=7.596 record_received_p50=1.994 record_received_p99=5.371 process_block_p50=404.539 process_block_p99=4704.863 trackers_p50=2.364 trackers_p99=6.692 loss_tracker_update_p50=0.281 loss_tracker_update_p99=0.791 ack_update_stats_p50=46.108 ack_update_stats_p99=161.396 ack_visits_p50=130048.000 ack_visits_p99=287072.000 update_nosend_nolock_nodecrypt_noblock_p50=79.254 update_nosend_nolock_nodecrypt_noblock_p99=1218.107 update_cpu_us_p50=577.944 update_cpu_us_p99=1352.236 ack_visits_per_reply=4060.1
round=3 build=outside P=32  16:31:36 up 1 day, 15:12,  4 users,  load average: 160.07, 168.64, 125.10
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=397.896 send_us_p99=3407.663 send_us_max=15225.911 update_us_p50=551.838 update_us_p99=2579.170 update_us_max=15577.900 total_us_p50=954.994 total_us_p99=4048.029 total_us_max=15995.814 received=70336
client_main_thread_parts packets_per_frame=32 n_p50=32.000 n_p99=96.000 swap_p50=0.371 swap_p99=3.106 checks_p50=9.547 checks_p99=30.428 decrypt_p50=90.807 decrypt_p99=263.986 protocol_p50=421.790 protocol_p99=1741.469 proto_update_p50=0.150 proto_update_p99=12.223 proto_send_p50=0.000 proto_send_p99=25.709 stats_p50=26.090 stats_p99=42.601 rest_p50=0.872 rest_p99=1.554 update_nosend_p50=551.838 update_nosend_p99=2579.170 update_nosend_nolock_p50=551.056 update_nosend_nolock_p99=2578.749 update_nosend_nolock_nodecrypt_p50=459.311 update_nosend_nolock_nodecrypt_p99=2078.377 decrypt_ns_per_reply=2959.9 loop_ns_per_reply=17377.3 replies_per_frame=31.98
client_main_thread_split packets_per_frame=32 ack_read_p50=2.544 ack_read_p99=7.607 record_received_p50=1.906 record_received_p99=5.831 process_block_p50=403.686 process_block_p99=1522.031 trackers_p50=2.237 trackers_p99=6.874 loss_tracker_update_p50=0.251 loss_tracker_update_p99=0.731 ack_update_stats_p50=24.096 ack_update_stats_p99=39.976 ack_visits_p50=130048.000 ack_visits_p99=387072.000 update_nosend_nolock_nodecrypt_noblock_p50=55.082 update_nosend_nolock_nodecrypt_noblock_p99=142.113 update_cpu_us_p50=552.907 update_cpu_us_p99=1600.382 ack_visits_per_reply=4055.7
DONE
```

## What the gate needs

With the I/O thread design the send window holds one release store and at
most one wake (docs/CLIENT-IO.md, The gate: send p99 at most 10 us), and the
lock wait is gone (the handoff of The receive path has no mutex). What remains
is update without its send and without the swap: the "no send, no lock"
column.

- **At P = 32 that is 548 us at the median and 3132 us at p99** (outside, set
  2; 576 and 6711 us in set 1), against 100 us. The median alone is 5.5 times
  the gate.
- **At BASE decrypt is not the part that decides it.** The block walk is:
  397 us of 548 at the median (73 percent), 1784 us at p99, 4057 visits per
  reply. Decrypt is 91 us at the median (17 percent). Taking decrypt away alone
  leaves 455 us at the median and 2084 us at p99.
- **Once the walk is cheap, decrypt is.** In the floor build "no send, no lock"
  is 137 us at the median and 388 us at p99; decrypt is 85 and 261 of that, 62
  percent of the median, and its p99 alone is 2.6 times the gate. Without
  decrypt the rest is 51 us at the median and 129 us at p99, of which the ack
  statistics are 23 and 36.
- **Decrypt is the gate by itself on this machine.** 32 opens at 1.8 to 2.4 us
  are 59 to 75 us; the in-situ 2.7 to 3.0 us per reply makes 85 to 97 us.
  Nothing else in update would fit beside it.
- **Bursts set the p99.** Replies per frame are 32 at the median but 76 to 96
  at p99 at P = 32, because the receive thread drains on a 1 ms sleep and one
  update can see two or three frames of replies. Every per-reply microsecond
  left on the main thread is paid two or three times over at p99. The I/O
  thread's wait on readable (docs/CLIENT-IO.md, The wake) shrinks the bursts;
  it does not remove the per-reply cost.
- **The lock wait is real and the recipe removes it.** Swap p99 at P = 32 is
  3120 us in the lock build and 2.4 us in the outside build (set 2), the
  receive-outside-lock card's effect measured at P = 32.

So the gate needs, in this order: a block walk that does not visit
acknowledged sequences, the ack statistics off the per-frame path, the I/O
thread cards (send and lock), and decrypt off the main thread.

## If decrypt moves

### Which thread: the I/O thread, before it publishes

The I/O thread fills a receive buffer it alone owns (Free to Fill, the
`IoReceive` action of `tla/RNClientIo.tla`) and hands it to update with one
release store (`IoPublish`). Opening each datagram in that buffer before
`IoPublish` adds no hand-off, no lock and no wake: the plaintext rides the
publish that already exists. The cost moves to a thread that is otherwise idle
most of the frame (docs/CLIENT-IO.md, rocketnet-client-io-linux): 32 opens are
59 to 97 us of I/O-thread time per frame on this machine.

### What it shares with update, and the rule for each

| state | today | after |
|---|---|---|
| the session open key, `protocol_id`, `connection_salt`, `filter_value` | written by update at the challenge (`rocketnet_client_process_challenge`, the derivation in `sdk/rocketnet_client_protocol.cpp`), zeroed by update at the integrity limit (`rocketnet_client_integrity_failure`) and at disconnect | update copies the open key and the immutable inputs into an I/O-owned open block, then stores an open epoch with release; the I/O thread loads the epoch with acquire at the top of each drain and takes the copy when it changes. To retire, update stores the epoch as retired (seq_cst) before it zeroes its own keys; the I/O thread zeroes its copy when it reads retired and opens nothing after. The I/O thread never reads `client->keys` |
| the replay window `replay_session` | update only; read by fill threads through `rocketnet_ack_block_build` (`rocketnet_client_protocol_seal_payload_frame`) under the phase contract | unchanged: update is its only writer. The I/O thread keeps its own window of the same struct, advanced by the same `rocketnet_replay_already_received` and `rocketnet_replay_advance` on its own opens, so a duplicate or too-old sequence is refused before the AEAD |
| sequence reconstruction | from `replay_session.highest_opened` | the I/O thread reconstructs from its own window's mark and writes the 64-bit sequence into the datagram's record; update reconstructs again from its own and refuses a disagreement |
| the ack endpoint, `send_sequence_session` for the block read, the acked batch, `ack_debt`, `last_receive_time`, `received_session_sequence`, the trackers, the reassembly | update only | unchanged: the ack block read and everything after the open stay in update, on the opened bytes |
| counters and the integrity limit | update | unchanged: the I/O thread writes no counter. Each datagram's record gains a verdict (opened, or the rung that refused it); update counts each verdict with today's counter, in arrival position, and runs `rocketnet_client_integrity_failure` for a did-not-open verdict in that position, so the limit and its state change stay on the main thread and in order |
| handshake packets (cookie, challenge, denied) and anything before the epoch is live | update | passed through unopened with a raw verdict; update processes them as at BASE |

**The replay window and the receive order.** `tla/ReceiveOrder.tla`'s
`CheapRefusalsFirst` requires every cheap refusal, the replay check included,
to come before a crypto rung; an I/O thread that opened first and left the
replay check to update would spend an AEAD on every replayed datagram. The
I/O thread's own window keeps the order. It agrees with update's: update
consumes the buffer in arrival order and advances `replay_session` on exactly
the sequences the I/O thread opened (`rocketnet_client_open_session_packet`
advances right after the open, before the ack block checks), so the two
windows are the same function of the same sequence stream, update's lagging by
the buffers not yet exposed. The one update refusal between the I/O thread's
open and update's advance that the I/O thread does not mirror is the state
check, which fails only once the session has ended. `tla/ReplayWindow.tla`'s
`WindowIsTruth`, `ReceivedIsRefused` and `BelowFloorIsRefused` hold for each
window on its own; update keeps its own replay check, so a disagreement could
only cost work or drop a datagram, never admit a replay. That the windows agree
while connected is the new model's invariant (`MirrorAgrees`, below).

**The protocol's single-threaded assumptions.** The protocol struct keeps one
writer, update. The I/O thread reads only its own open block and its own
window and writes only the buffer it holds in Fill. `tla/RNClientIo.tla`'s
`ExposedStable` already guarantees update reads a buffer the I/O thread no
longer writes; the plaintext, the sequences and the verdicts are published by
the same release store as the datagrams are today.

**The cost of each hand-off.** The keys: one release store per change (once
per connection, and the retire), one acquire load per drain on the I/O thread.
Per datagram: no new hand-off; the plaintext replaces the ciphertext inside the
datagram's existing `ROCKETNET_FRAME_SIZE` slot, plus a verdict byte and a
64-bit sequence in its record. Per batch: nothing beyond `IoPublish` and
`Return`.

**The expected main-thread cost.** Per reply, the checks run again on the main
thread (7.0 us for 32 replies in the floor build, about 0.2 us each) and the
protocol after the open (18.8 us for 32, about 0.6 us each, the cheap block walk
included); there is no AEAD. From the floor build, update without send, lock
and decrypt is 51 us at the median and 129 us at p99 at P = 32 under a load
of 102; without the ack statistics (23 and 36) that is about 28 us at the
median and, subtracting p99s (which is only approximate), about 94 us at p99.
That is at the gate under this load, and below it on a quiet machine (1.3 to
1.6 times faster at the median) with the I/O thread's smaller bursts. The
implementation card measures it with the same meter.

### The other options

- **A worker thread for decrypt.** The same sharing, plus a second buffer
  handshake (I/O thread to worker to update): another Ready and Exposed pair,
  a wake per batch (an eventfd write and a context switch, about the cost of
  one open), and a scheduling hop of latency per batch. It pays only when the
  I/O thread cannot keep up, and at 59 to 97 us per frame it can. Not
  recommended.
- **Batching the opens on the main thread.** The vendored sodium already
  carries the optimized ChaCha20 and Poly1305 paths (`sodium/README.md`) and
  has no multi-buffer AEAD; the open's cost is per byte (1.8 to 2.4 us at 200
  to 240 bytes, 3.7 to 4.5 us at 1200) with nothing per call to amortize.
  Batched or not, 32 opens stay 59 to 75 us on the main thread. Not enough.
- **Decrypt stays, everything else is fixed.** The floor build's "no send, no
  lock" at P = 32 less the ack statistics is about 114 us at the median and
  352 us at p99. Above the gate.

### The model the next card writes first

`tla/RNClientIoDecrypt.tla` (wrapper `tla/MCRNClientIoDecrypt.tla`, document
`tla/README-RNClientIoDecrypt.md`, a `tla/COVERAGE.tsv` row), extending the
receive half of `tla/RNClientIo.tla` (`buf`, `IoReceive`, `IoPublish`,
`IoOwe`, `Expose`, `Return`) and abstracting the windows as
`tla/ReplayWindow.tla` does.

- **Variables:** `socket` (datagrams in flight, each genuine, duplicate,
  too old or forged, with a sequence); `rec` (the records of each buffer, each
  a sequence and a verdict); `ioWin` (the I/O thread's window, a mark and a set);
  `mainWin` (`replay_session`); `epoch` (`"none"`, `"live"`, `"retired"`);
  `ioKeys` (the I/O thread holds a live copy); `session` (`"handshake"`,
  `"connected"`, `"ended"`); the ghosts `crypto` (datagrams that reached the
  AEAD) and `accepted` (sequences update accepted).
- **Actions:** `Arrive`; `PublishKeys` (update at the challenge);
  `RetireKeys` (update at the integrity limit, at disconnect and at destroy);
  `IoTakeKeys` and `IoDropKeys` (the I/O thread reads the epoch); `IoCheck`
  (the cheap rungs, the `ioWin` replay check last); `IoOpen` (the AEAD: a
  genuine datagram opens and advances `ioWin`, a forged one is refused);
  `IoPublish`, `Expose` and `Return` from RNClientIo; `UpdateConsume` (one
  record in arrival order: for an opened record update's replay check and
  `mainWin` advance, otherwise the verdict's counter; a did-not-open record
  counts toward the integrity limit and may end the session); `FillAckBuild`
  (a fill thread reads `mainWin`).
- **Invariants:** `CheapRefusalsFirst` (as in `tla/ReceiveOrder.tla`: no
  datagram refused by a cheap rung, the replay check included, is in
  `crypto`); `NoOpenWithoutKeys` (no `IoOpen` while `ioKeys` is false, and
  `ioKeys` never true once the I/O thread has read `"retired"`);
  `MirrorAgrees` (while `session` is not `"ended"`, `ioWin` and `mainWin`
  agree on every sequence update has consumed); `AcceptedOnce` (update
  accepts a sequence at most once: `ReceivedIsRefused` across both windows);
  `SingleWriter` (`mainWin`, the counters and `session` change only in update's
  actions, `ioWin` only in the I/O thread's); `ExposedStable` and
  `NoReceiveStall` carried from RNClientIo. Liveness, under RNClientIo's
  fairness: `GenuineEventuallyAccepted` (a genuine datagram that arrives while
  connected is accepted) and `RetiredEventuallyZeroed` (after `RetireKeys`
  the I/O thread's copy is eventually dropped).
- **Reversed witnesses,** each violating exactly its rule:
  `MCRNClientIoDecryptBrokenOpenBeforeReplay.cfg` (the I/O thread opens
  before its replay check: `CheapRefusalsFirst`),
  `MCRNClientIoDecryptBrokenSharedWindow.cfg` (the I/O thread advances
  `mainWin`: `SingleWriter`), `MCRNClientIoDecryptBrokenNoRetire.cfg` (the I/O
  thread keeps its copy after retire: `NoOpenWithoutKeys`),
  `MCRNClientIoDecryptBrokenCountOnIo.cfg` (the I/O thread counts a refusal
  itself: `SingleWriter`).

## If it does not

Decrypt does move, but decrypt alone does not reach the gate: with the walk as
it is, update without send, lock and decrypt is 455 us at the median at P = 32.
What else update needs, measured:

- **The block walk (first, the largest).** 397 us at the median and 1784 us
  at p99 at P = 32, 85 us at P = 8, 4057 visits per reply at P = 32. Each
  reply's block restates the received run back to the window floor
  (`rocketnet_ack_block_build`), and `rocketnet_ack_apply_run` visits every
  sequence it names, newest to oldest, although almost all are already
  acknowledged. The floor build shows the prize: the walk falls to 2.7 us at
  the median and update's median at P = 32 from 548 to 138 us. The floor is
  not the design, because one lost packet holds the floor in place until it
  leaves the 4096-sequence history and the walk is long again. The design is
  a bitmap of held-and-unacknowledged sent records beside the sent ring (one
  bit per record, `ROCKETNET_SENT_PACKET_BUFFER_SIZE / 64` words, as the
  replay window holds its bits): set when a record is written (under
  `sent_lock`, in the fill phase), cleared when the record is acknowledged,
  left set when the record is deferred because the batch is full. A run then
  visits only the set bits in its range, at most one word per 64 sequences
  plus one visit per newly acknowledged record. `unknown_sequence_counter`
  keeps its meaning from a second bitmap of held records (named minus held,
  by popcount). `tla/RNSentHistory.tla`'s `ReceiveAck` already marks exactly
  the named records that are held and unacknowledged; the card extends
  `tla/FragmentsAndAcks.tla`, whose `RecvAck` carries the batch deferral,
  with the bitmaps first: variables `unacked` and `held`, invariant
  `BitsMatchRing` (the bitmaps equal what the ring holds), and the existing
  `SurfacedOnce`, `AckedMeansSurfaced`, `BurnedNeverNamed` and
  `OpenedSurfacesOrLaps` kept; reversed witnesses
  `MCFragmentsAndAcksWitClearOnDefer.cfg` (the bit cleared on a deferral:
  `OpenedSurfacesOrLaps`) and `MCFragmentsAndAcksWitNoSetOnAlias.cfg` (a
  write over an old record that leaves the bit clear: `BitsMatchRing`).
- **The ack statistics (every frame, every P).** `rocketnet_ack_endpoint_update_stats`
  is 17 to 24 us at the median at every P: it walks the 2048-entry rtt
  history twice and half the sent ring every update, and on the client only
  `rocketnet_client_transport_stats` reads what it computes (`ack.rtt`). It
  moves to that getter.
- **The send walk and the lock.** The send window is 390 us at the median at
  P = 32 and the lock wait 3120 us at p99; the I/O thread cards
  (docs/CLIENT-IO.md, Per-platform cards) and rocketnet-client-receive-outside-lock
  take them, as measured above.
- **What is left.** Checks 7 to 10 us and the protocol after the open about
  19 us at P = 32 with a cheap walk; protocol update under 1 us at the median;
  rest about 1 us.

## Cards

| id | tier | one line | red at BASE |
|---|---|---|---|
| rocketnet-ack-walk-unacked-only | pro | extend `tla/FragmentsAndAcks.tla` with the held and unacknowledged bitmaps and their two witnesses first, then give `sdk/rocketnet_ack.cpp` the bitmaps so a block's runs visit only held unacknowledged records; unit test `ack_block_walk_visits_only_unacknowledged` (a block restating 4096 acknowledged sequences makes zero record visits, a deferred record still surfaces from the next block); the meter at P = 32, BASE and head interleaved | `./bin/test ack_block_walk_visits_only_unacknowledged` prints `unknown unit test ack_block_walk_visits_only_unacknowledged`, exit 2 |
| rocketnet-client-ack-stats-on-read | flash | call `rocketnet_ack_endpoint_update_stats` from `rocketnet_client_transport_stats` instead of every update; unit test `client_ack_stats_computed_on_read` (the stats a getter returns are unchanged, and update does not recompute them); the meter at P = 1 | `./bin/test client_ack_stats_computed_on_read` prints `unknown unit test client_ack_stats_computed_on_read`, exit 2 |
| rocketnet-client-io-decrypt-model | heavy | write `tla/RNClientIoDecrypt.tla` as If decrypt moves names it, with the four reversed witnesses, checked with TLC on a bench, a `tla/COVERAGE.tsv` row and its README | `ls tla/RNClientIoDecrypt.tla` prints `ls: cannot access 'tla/RNClientIoDecrypt.tla': No such file or directory`, exit 2 |
| rocketnet-client-io-decrypt | pro | after rocketnet-client-io-transport, rocketnet-client-io-linux and the model: the I/O thread opens session datagrams before `IoPublish` with its own key copy and window, verdicts in the records, update consumes them; unit tests (step-driven, fake driver, clock from 1000.0) `client_io_decrypt_opens_before_publish`, `client_io_decrypt_replay_refused_before_open` (the crypto tally's `aead_open` does not move for a duplicate), `client_io_decrypt_no_open_after_retire`, `client_io_decrypt_verdicts_counted_in_order`; the gate row of docs/CLIENT-IO.md at P = 32 | `./bin/test client_io_decrypt_opens_before_publish` prints `unknown unit test client_io_decrypt_opens_before_publish`, exit 2 |

Each red was run at BASE `b0b4527` on the Linux bench of this note.
rocketnet-client-receive-outside-lock and the client I/O cards of
docs/CLIENT-IO.md are already cut and are not repeated here.
