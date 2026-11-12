# Client frame time

`client_frame_time` measures what a game thread spends inside the public
client calls on each frame, with the platform client transport the library
picks: `gso` on Linux, `msgx` on macOS, `iocp` on Windows. It is a bench, never
a unit test: it uses the wall clock and real sockets.

One process runs the server; another runs one client against it at the space
client profile:

- The server sends 15 unreliable 1200-byte messages a tick on channel 0, and
  one 64-byte reliable-ordered message on channel 1, at 60 Hz.
- The client writes 48 unreliable bytes a frame, and a 32-byte reliable
  message every 8th frame.

Both run the production library (`-DROCKETNET_DEV=OFF`). The server makes a
real backend keypair and a real connect token.

Each frame, the client times four windows with a monotonic clock read only
around the call:

- `update`: `rocketnet_client_update`;
- `receive`: draining and touching the received messages;
- `write`: the frame's sends;
- `send`: `rocketnet_client_send_packets`.

`frame` is their sum. Frames run at a fixed 60 Hz, and a late frame starts at
once (catch-up).

For each window it reports p50, p99, p99.9 and worst in microseconds, along
with these counts:

- Allocations, through the library's allocator hooks (`rocketnet_init`). Only
  the hooks are counted; malloc is not wrapped.
- Try-acquires of the hand-off mutex, and how many found it held.
- Blocking acquires, and the time spent waiting in them.
- The transport's wake: the eventfd `write` on Linux, the `EVFILT_USER`
  `kevent` on macOS, `PostQueuedCompletionStatus` on Windows.
- System calls, where the platform can count them:
  - Linux: per thread, from the `raw_syscalls:sys_enter` tracepoint.
  - macOS: whole-process task events, so an upper bound.

The counts come from `client_frame_time_hooks.c`. On POSIX the executable
interposes the calls (`dlsym(RTLD_NEXT)`); on Windows it patches its own import
table.

## Conditions

- Clean.
- Impaired: delay, jitter, loss and reorder applied by a relay inside the
  server process (`--relay-forward-ip`), or on Linux by netem on a veth
  between two network namespaces. On Windows the relay cannot bind beside the
  server's socket. There, the portable server impairs its own sends instead
  (`--transport portable --impair-sends 1`), server to client only.
- Hitch: `--hitch-every K --hitch-ms 100` sleeps 100 ms on some frames. The
  tool reports the frames after each hitch by offset, and counts any later
  frame over 1 ms as a second hitch.

## The plant and the assertions

`--plant busy|alloc|syscall --plant-call update|send` runs once per call,
inside the library, from the call's first try-acquire of the hand-off mutex:

- `busy`: a busy loop (`--plant-us`);
- `alloc`: one allocation through the hooks;
- `syscall`: one `getppid`.

A busy plant of 500 us must move that call's p50 by 500 us. This is the
proof that the meter sees inside the call.

The client exits non-zero when an assertion fails:

- `--assert-zero-alloc 1` exits 2 on any steady-state allocation inside a
  client call.
- `--assert-max-us X` exits 3 when the worst frame is over X.

`tools/check_client_frame_time.py` runs one nightly row. It runs the
assertions, then the plants as the red proof: each plant must fail its
assertion. `.github/workflows/nightly-client-frame-time.yml` runs that row on
each platform.

## Example

```
client_frame_time server --bind 127.0.0.1:40000 --token-file t.token --seconds 120
client_frame_time client --token-file t.token --frames 3600 --warmup 600 --assert-zero-alloc 1 --assert-max-us 400
```

## Limits

- Loopback or a veth: one client, one server. The client's I/O thread shares
  the machine, so the worst frame reflects host scheduling as well as library
  work. Compare repeated runs on a quiet machine.
- The crypto line names the implementation the library picks on the CPU it
  runs on. It differs by platform and compiler.
