# RocketNet v1.2.0: Load Tested

Changes compared with the `v1.1.60` tag. No packet layout or protocol version
change, no public API change and no new counter (the contract still holds 292
counters); v1.2.0 endpoints interoperate with every v1.x endpoint. v1.1.61 and
v1.1.62 were labelled on `main` but never tagged or published, so their
changes ship for the first time here and are listed at the end.

## Load tested

Five server transports were measured with 1000 connected players at 60 Hz. At
peak each player is sent 15 messages of 1200 bytes every update: 900.3k
packets/s, about 9.2 Gb/s, against a 10 GbE line rate of 958.6k. Each
transport ran at peak and at average load (480.3k packets/s), with the game
threads idle and half busy, three runs of each, with nothing configured
beyond the address to bind. A run passes when it holds 60 frames per second
with every player connected, nothing lost and no send refused.

| Transport | Machine | Network threads (default) | Result | Main thread p99 |
|---|---|---|---|---|
| xdp (Linux, root) | Bare metal, AMD EPYC 9254, Intel X710 | 1 | 24 of 24 runs pass, at 1 and at 2 threads | 0.43 to 0.59 ms |
| io_uring (Linux) | AWS c8gn.8xlarge | 2 | 12 of 12 pass | 0.29 to 0.44 ms |
| gso (Linux) | AWS c8gn.8xlarge | 2 | 11 of 12 pass | 0.29 to 0.39 ms |
| iocp (Windows) | AWS m7i.16xlarge | 2 completion workers, 2 send threads | 12 of 12 pass | 0.60 to 0.84 ms |
| rio (Windows) | AWS m7i.16xlarge | 6 lanes | 12 of 12 pass | 0.58 to 0.85 ms |

"Main thread" is RocketNet's own work per frame on the thread that calls
update. The one gso run that did not pass refused 701 sends in a single
second at average load, when a network thread lost its processor for about
100 ms on the cloud VM; the same gaps appear on io_uring without refused
sends. The xdp rows were measured at v1.1.58, and one run of one workload was
repeated on this release's xdp code; the others were measured on the branches
merged into this release.

The defaults changed so that each of these transports passes with nothing
configured:

- Linux: `auto` now tries `xdp`, `io_uring`, `gso`, then `portable` (was
  `xdp`, `gso`, `io_uring`, `portable`). Without root, a server runs
  `io_uring` where the build and kernel have it and `gso` where they do not.
- `gso`: 2 workers by default (was 1). With 1 worker it fell to 842k to 872k
  packets/s at peak with the game threads half busy.
- `rio`: 6 lanes by default (was 5). At 1000 players its memory plan grows
  from 904 MB to 1008 MB.
- `iocp`: 64 send operations per client (was 32). With 32 it refused 6,631 to
  16,794 sends per run at peak. At 1000 players its memory plan grows from
  534 MB to 603 MB.
- Windows: `auto` is `iocp`, `rio`, then `portable` in development and
  production builds alike (development builds tried `portable` first). The
  portable transport is last on every platform.

The macOS `msgx` transport is not a load-tested transport in this release: on
the machines measured it does not yet hold 1000 players at these loads. The
portable transport is not a performance transport and was not load tested.

Machines, commands and every row: [PERFORMANCE.md](PERFORMANCE.md).

## Other changes

Library:

- macOS `msgx` server: the main thread's receive now reads the worker's bank
  up to its committed count, so a datagram banked just before the receive
  call is returned by that call. Before, it waited for the next update, which
  added 16.7 ms at 60 Hz; on loopback the mean delivery time fell from
  27.2 ms to 10.5 ms.
- An `io_uring` refusal at create (an old kernel, a seccomp filter, a build
  without liburing) logs a warning naming the step and the errno, and `auto`
  goes on to `gso`. Two `iocp` create refusals that returned silently now
  log. The `rio` warning for more lanes than cores now counts physical cores.
- `io_uring` server create: when the kernel refuses ring memory with ENOMEM,
  create tries again for up to 100 ms before failing (or, under `auto`, going
  on to `gso`). A release test on a saturated host saw that refusal on the
  second of 1000 create and destroy cycles; the cause was not reproduced, and
  the retry count is chosen, not measured.
- Windows ASan builds: the red-zone walk asks the OS once per committed run
  instead of once per 32 KiB, which took the 1000-client unit cases from
  about two hours each to 13 seconds.

Build:

- MSVC v141 (Visual Studio 2017) and v142 (Visual Studio 2019) build clean
  under `/WX`, and the tests build under clang-cl. No library behaviour
  changed on newer toolsets.

Tests, tools and proofs (no library behaviour changed):

- Functional tests that assumed every datagram arrives over a real socket now
  resend within a hard bound when the kernel is shown to have dropped one,
  and the reply ordering property has a deterministic in-process proof.
- Windows: the lifecycle stress test judges the process handle count; the
  soak allows the handles Winsock keeps per completion worker once; the
  machine-wide test lock names its holder while a run waits; `auto` walking
  past a refusing `iocp` is tested.
- macOS: the test loss gate counts this process's own datagrams, backed by
  the loopback interface's drops; the frame budget check can fail under its
  own plant.
- Tools: the memory check reads each transport's thread counts and defaults
  from the library; the fuzz transport seeds are regenerated; the nightly
  dispositions file lists every known nightly red with its roadmap item.
- Documentation: `TRANSPORTS.md`, `LINUX.md`, `WINDOWS.md`, `USAGE.md`,
  `DEPLOYING.md`, `TRANSPORT-INTERNALS.md` and `MEMORY.md` state the new
  orders and defaults. `LINUX.md` says what a cloud VM can do to a network
  thread, and `WINDOWS.md` says how to place the adapter's interrupts for
  `rio` and its receive work for `iocp`.

## From v1.1.61 and v1.1.62

- Server memory: every transport sizes its buffers from the configured
  `max_clients`, with no 512-player floor. An 8-player server on the portable
  transport needs 21.6 MB, down from 47.1 MB. Sizes at 512 players and above
  are unchanged. On macOS the msgx receive banks, socket table and worker
  sort buckets follow `max_clients` too.
- Sends after an ICMP error: a connected UDP socket hands back the ICMP error
  an earlier datagram drew on its next send call, sending nothing. The Linux
  gso client took that as a refusal and dropped the whole coalesced message;
  the portable sends and the macOS msgx batch send dropped one datagram or
  chunk. Each now sends the same message again once; a second error in a row
  drops that one message, counted as before.
- Tests and tools: a functional case whose server fails to create or start is
  red, and a transport the host cannot run is a named skip; the simulation,
  load, soak, fuzz and source-audit harnesses fail where they used to pass
  silently. No library behaviour changed.
- Proofs: the TLA+ models and their READMEs describe the code as it is, each
  check that could not fail has a broken variant that fails it, and the trace
  checker's planted violations are held to the records they change. Code
  comments only in the library.
