# Many-Client Load Harness (`bin/load`)

`bin/load` stands up one `rocketnet_message_server_t` and `N` `rocketnet_message_client_t` instances (`1 <= N <= ROCKETNET_MAX_CLIENTS`, where `ROCKETNET_MAX_CLIENTS = 1000`) inside a single process on loopback (`127.0.0.1`), connects every client, drives traffic across all eight reliability and channel combinations (`0..7`) for a configured duration, and exits `0` only when all five pass criteria hold.

## Usage

```
./bin/load <clients> <seconds> [drop-one]
```

- `<clients>`: integer client count in `1..ROCKETNET_MAX_CLIENTS` (`1..1000`). Any value outside `1..ROCKETNET_MAX_CLIENTS` is rejected at CLI parse time with exit code `2` and an error message naming `ROCKETNET_MAX_CLIENTS`.
- `<seconds>`: positive integer traffic duration in seconds.
- `drop-one` (optional): fault-injection mode in which client `0` stops acknowledging and processing reliable messages partway through the run, proving that the harness exits `1` (`verdict=FAIL`) and names the broken criterion.

## Traffic Profile

- Client-to-server traffic cycles every client across all 8 reliability and channel combinations:
  - `0`: `ROCKETNET_UNRELIABLE` (channel `0`)
  - `1`: `ROCKETNET_UNRELIABLE_SEQUENCED` (channel `1`)
  - `2`: `ROCKETNET_RELIABLE` (channel `2`)
  - `3`: `ROCKETNET_RELIABLE_ORDERED` (channel `3`)
  - `4`: `ROCKETNET_RELIABLE_SEQUENCED` (channel `4`)
  - `5`: `ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT` (sent with channel `5`, delivered on code-0 channel `0`)
  - `6`: `ROCKETNET_RELIABLE_WITH_ACK_RECEIPT` (channel `6`)
  - `7`: `ROCKETNET_RELIABLE_ORDERED_WITH_ACK_RECEIPT` (channel `7`)
- Server-to-client traffic drives `ROCKETNET_RELIABLE_ORDERED` messages across connected clients so both directions of reliable delivery and acknowledgment are verified.
- Every payload carries a 32-byte deterministic frame (`magic`, `client_idx`, `combo_idx`, `seq`, and 16 hash-derived pattern bytes) verified byte-for-byte on receipt.

## Five Pass Criteria

1. **`connect_within_10s`**: Every client (`connected == <clients>`) completes the connection handshake and reaches `ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED` within `10.0` seconds of start.
2. **`no_unexpected_disconnect`**: No client transitions out of the connected state and no server disconnect or timeout counter increments during the run.
3. **`reliable_delivery`**: Every reliable and reliable-ordered message sent is delivered once, in order on ordered channels, and pattern-verified; sequenced reliable messages are strictly increasing in sequence number, never duplicated, and pattern-verified.
4. **`unreliable_integrity`**: Every unreliable message delivered (`ROCKETNET_UNRELIABLE`, `ROCKETNET_UNRELIABLE_SEQUENCED`, `ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT`) is pattern-verified (never corrupted) and never duplicated (each sequence number per client and reliability mode is observed at most once, and strictly increasing for `ROCKETNET_UNRELIABLE_SEQUENCED`).
5. **`counters_agree`**: The total messages sent by all clients equals the total messages received by the server plus counted drops (`sent == received + drops`).

## Diagnostic Telemetry (Recorded Without Gating)

At the end of every run, the single summary line records:
- `server_update_p50_us` and `server_update_p99_us`: 50th and 99th percentile durations (in microseconds) of `rocketnet_message_server_update`.
- `rss_mb`: process resident set size (in MiB) at the end of the run.
- `fd_limit_soft` and `fd_limit_hard`: the `RLIMIT_NOFILE` soft and hard values the run used (`infinity` when unlimited).
- `fds_start`, `fds_peak` and `fds_end`: open file-descriptor count at start, peak during the run, and end after teardown.
- `threads_start`, `threads_peak` and `threads_end`: thread count at start, peak during the run, and end after teardown.

## macOS file-descriptor limit

macOS ships a low per-process file-descriptor limit: on the fleet's Apple-silicon member both `ulimit -n` and the soft `launchctl limit maxfiles` value are `256`. A 1000-client run needs one socket per client plus the server socket, so `bin/load` raises its own soft `RLIMIT_NOFILE` and refuses to start when the resulting soft limit is below two file descriptors per client plus a 64-descriptor margin. The refusal names the limit, both values, the needed soft limit and the command that raises it:

```
load: RLIMIT_NOFILE soft=256 hard=256 cannot cover clients=1000 (2 fds per client) plus margin=64 (need soft >= 2064)
load: raise it with: ulimit -n 2064
load: if the hard limit blocks that, raise the hard limit in a new shell (macOS: sudo launchctl limit maxfiles 65536 65536)
```

Raise the limit for the shell that runs the harness, then start the run:

```
ulimit -n 65536
./bin/load 1000 60
```

When `ulimit -n` cannot raise the soft limit because the hard limit is low, raise the hard limit for the login session with `sudo launchctl limit maxfiles 65536 65536`, open a new shell, and run the harness there. The summary line records the soft and hard values the run used and the file-descriptor and thread counts at start, peak and end.
