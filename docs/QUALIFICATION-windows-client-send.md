# Qualification: the Windows client's overlapped send

Roadmap row 223 (final performance milestone). The owner's rule for the row:
stable today, performant in the final milestone, so the overlapped send
lands only if a Windows client against a Linux server across the LAN beats
BASE on ring-full refusals and client update p99 with no fewer packets sent.
The design is docs/CLIENT-IO.md "The overlapped send".

## Verdict

**HOLD: not measured.** The six BASE and six head runs below were not taken.
The measurement and the Windows gate both need captain under
`winlock --exclusive`, and on 2026-10-23 at 23:12 EDT captain's exclusive
queue held 15 jobs ahead of this card, the oldest waiting 1 h 37 min, so
neither could start before the card's deadline. The code stays on the
branch; nothing here claims a Windows result.

| run | build | packets sent | ring-full refusals | client update p99 (ms) |
|---|---|---:|---:|---:|
| 1-6 | BASE 010c8167 | not run | not run | not run |
| 1-6 | head | not run | not run | not run |

## Why the prototype regressed (from its code, not from a new run)

The prototype (branch
`origin/sprint/rocketnet-windows-client-overlapped-send.w6.g1.e0`, head
b3a03d87) sent 69-77k packets with about 2,100 ring-full refusals against
614,400 at BASE, and client update p99 rose from 0.01 ms to about 3 ms. Read
against its code, three causes, each removed here:

1. **One ring in flight.** Its I/O step asserted that no ring was posted
   before it took the next one (`posted_ring < 0 && outstanding == 0`), so a
   ready ring waited until every completion of the ring ahead was reaped.
   With the game thread filling the third ring, the hand-off found no free
   ring and payloads were refused. Here the step keeps taking and posting
   ready rings while earlier ones are outstanding
   (`rocketnet_client_io_send_overlapped`), with posts preallocated for every
   slot of every ring (`ROCKETNET_CLIENT_TRANSPORT_MAX_POSTED_SENDS`, three
   rings of 256), so the I/O thread never waits on a completion to post.
2. **Every pending post cancelled.** It cancelled a post at once when
   `WSASendTo` returned `WSA_IO_PENDING`, so every datagram the kernel could
   not buffer at once was dropped as an error. Here a pending post stays
   posted; only stop cancels.
3. **A scan of every record per reap, maybe inside update.** Its reap read
   all 512 records with `WSAGetOverlappedResult` on every step, and its
   pressure case (`rocketnet_test_client_send_buffer_pressure_windows`)
   created the clients step-driven, so that step ran inside the game thread's
   update. If the LAN runs used clients made the same way, the 3 ms update
   p99 is the I/O step's cost moved onto the game thread; the row's note does
   not say how the LAN clients were made. Here the reap walks the records oldest first and stops at the
   first pending one (in post order), and the step-driven mode keeps the
   synchronous send, so the overlapped send never runs inside update.

These are read from the prototype's diff; none was measured on Windows by
this card.

## How to take the measurement

The harness is a scratch tool, not committed (it needs a CMake line outside
this card's paths): `ovl_pressure.cpp`, with `ovlmeas.sh` and
`ovlbuild.bat`, kept with the card's evidence. The server runs on amd-3
(192.168.1.38, its own machine), the client on captain (192.168.1.46), across
the LAN:

```sh
# amd-3, Release build of ovl_pressure against rocketnet_dev
./bin/ovl_pressure server 192.168.1.38:40123 <seconds>
# captain, under winlock --exclusive: builds ovl_pressure at BASE and head,
# then six alternating runs of each, threaded and step-driven
bash ovlmeas.sh 010c8167 <head> 192.168.1.38:40123 6
```

Each client run is eight client transports in the default threaded mode,
one game thread each, 300 frames. A frame fills the send ring with 1000-byte
payloads until `start_packet` refuses (at most 256), then calls
`send_packets` and `update` (timed), drains receives and sleeps 1 ms. The
`RESULT` line gives packets sent, ring-full refusals (`start_packet`
refusals), `CLIENT_SEND_DEFERRED`, `SEND_FAILED`, and update p50, p99 and
max over every frame of every client. A Linux smoke run of the harness on
loopback (two clients, 50 frames) sent 25,600 packets with no refusal and an
update p99 of 0.008 ms; it shows the harness works, not anything about
Windows.
