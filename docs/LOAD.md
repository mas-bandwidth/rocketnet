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
  - `0`: `ROCKETNET_RELIABILITY_UNRELIABLE` (channel `0`)
  - `1`: `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED` (channel `1`)
  - `2`: `ROCKETNET_RELIABILITY_RELIABLE` (channel `2`)
  - `3`: `ROCKETNET_RELIABILITY_RELIABLE_ORDERED` (channel `3`)
  - `4`: `ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED` (channel `4`)
  - `5`: `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT` (sent with channel `5`, delivered on code-0 channel `0`)
  - `6`: `ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT` (channel `6`)
  - `7`: `ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT` (channel `7`)
- Server-to-client traffic drives `ROCKETNET_RELIABILITY_RELIABLE_ORDERED` messages across connected clients so both directions of reliable delivery and acknowledgment are verified.
- `LOAD_PROFILE=light` replaces both batches with the light profile (`load.c:752-763`, `load.c:871-895`, `load.c:1003-1013`): the traffic loop updates a slice of 100 clients every 10 ms, so each client updates at 10 Hz, and on each update the client sends one `ROCKETNET_RELIABILITY_RELIABLE_ORDERED` message on channel `3` and the server queues one to that client; the server updates once per slice, on an absolute 10 ms schedule that catches a slow slice up. `LOAD_PROFILE` unset or `default` is the profile above; any other value exits `2`.
- Every payload carries a 32-byte deterministic frame (`magic`, `client_idx`, `combo_idx`, `seq`, and 16 hash-derived pattern bytes) verified byte-for-byte on receipt.

## Five Pass Criteria

1. **`connect_within_10s`**: Every client (`connected == <clients>`) completes the connection handshake and reaches `ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED` within `10.0` seconds of start.
2. **`no_unexpected_disconnect`**: No client transitions out of the connected state and no server disconnect or timeout counter increments during the run.
3. **`reliable_delivery`**: Every reliable and reliable-ordered message sent is delivered once, in order on ordered channels, and pattern-verified; sequenced reliable messages are strictly increasing in sequence number, never duplicated, and pattern-verified.
4. **`unreliable_integrity`**: Every unreliable message delivered (`ROCKETNET_RELIABILITY_UNRELIABLE`, `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED`, `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT`) is pattern-verified (never corrupted) and never duplicated (each sequence number per client and reliability mode is observed at most once, and strictly increasing for `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED`).
5. **`counters_agree`**: The total messages sent by all clients equals the total messages received by the server plus counted drops (`sent == received + drops`).

When `counters_agree` fails, `load` prints to stderr every nonzero counter of the server transport and the message server, the sums of every nonzero counter over the client transports and the message clients (`load: counters_agree ... <COUNTER>=<n>`), and each combo's client-to-server sent and received counts (`load: counters_agree combo=<n> sent=<n> received=<n>`), so the run names where the uncounted messages went (`load.c:1081`).

## Diagnostic Telemetry (Recorded Without Gating)

At the end of every run, the single summary line records:
- `connect_s`: wall time from the start of the connect phase until every client and the server report the full client count connected.
- `connect_p50_s` and `connect_max_s`: 50th percentile and maximum per-client connect time (in seconds), each read from the clock when `load` first sees that client in `ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED` (`load.c:660`), not from the connect loop's per-pass time, which every client of one pass shares. With three or more clients connected, a `connect_p50_s` equal to `connect_max_s` means the stamps share one value: `load` then prints `load: connect_stamps ...` to stderr and fails the run with `failed_criterion=connect_stamps` (`load.c:1152`).
- `server_update_p50_us` and `server_update_p99_us`: 50th and 99th percentile durations (in microseconds) of `rocketnet_message_server_update`.
- `rss_mb`: process resident set size (in MiB) at the end of the run.
- `server_driver` and `client_driver`: the transport the ladder granted to the server and to client `0`.
- `fd_limit_soft` and `fd_limit_hard`: the `RLIMIT_NOFILE` soft and hard values the run used (`infinity` when unlimited).
- `fds_start`, `fds_peak` and `fds_end`: open file-descriptor count at start, peak during the run, and end after every client, the server and `rocketnet_term` are gone.
- `threads_start`, `threads_peak` and `threads_end`: thread count at the same three points.

Start is sampled after `rocketnet_init`, which opens the process-wide random source once and keeps it for the life of the process, so start, peak and end measure what servers and clients add. A run whose end counts differ from its start counts leaks a descriptor or a thread. Each client holds two descriptors and one receive thread while it runs, so a 1000-client run peaks near 2000 descriptors and 1000 threads.

## macOS

### File-descriptor limit

A default macOS login session starts with a soft `RLIMIT_NOFILE` of `256` (`launchctl limit maxfiles` prints the session default; `ulimit -n` prints the shell's value). A 1000-client run needs two descriptors per client, so `bin/load` raises its own soft limit toward `65536` (bounded by the hard limit) and refuses to start when the soft limit it ends with is below two descriptors per client plus a 64-descriptor margin. The refusal exits `1` and names the limit, both values, the needed soft limit and the command that raises it:

```
load: RLIMIT_NOFILE soft=256 hard=256 cannot cover clients=1000 (2 fds per client) plus margin=64 (need soft >= 2064)
load: raise it with: ulimit -n 2064
load: if the hard limit blocks that, raise the hard limit in a new shell (macOS: sudo launchctl limit maxfiles 65536 65536)
```

When only the soft limit is low (the macOS default: soft `256`, hard `unlimited`), `bin/load` raises it itself and the run starts. The refusal fires when the hard limit is low too. In bash, `ulimit -n 256` sets both the soft and the hard limit, so `bash -c 'ulimit -n 256; ./bin/load 1000 60'` shows the refusal; in zsh, `ulimit -n 256` sets only the soft limit and the run raises it again. To run in a shell whose hard limit is low, raise the hard limit for the login session with `sudo launchctl limit maxfiles 65536 65536`, open a new shell, and run there:

```
ulimit -n 65536
./bin/load 1000 60
```

### Connect-deadline diagnostics

When the connect phase reaches its 10-second deadline with a client short of `CONNECTED`, `bin/load` names up to eight such clients on stderr before the summary line (`load.c:688-722`): each one's transport state, its driver, the wire datagrams it has sent and received, and its nonzero transport counters, then the server's connected count and every nonzero server transport counter:

```
load: client 481 not connected at the deadline: state=1 requesting cookie
load: client 481 driver=msgx packets_sent=36 packets_received=0
load: server reports 999 of 1000 clients connected at the deadline
load: server counter CONNECTION_REQUEST_NO_COOKIE=1035
```

### Traffic-phase disconnect diagnostics

For each of the first eight clients that leave `CONNECTED` during traffic, `bin/load` prints, after the line naming the client and its disconnect reason, when that client was last updated, the longest gap between two of its updates, and the longest single server update with its time into traffic (`load.c:839-843`, `load.c:905-912`, `load.c:921-925`):

```
load: client 432 left CONNECTED at t=6.496: state=0 disconnected reason=server disconnected
load: client 432 last updated 0.000 s ago, longest update gap 2.127 s; longest server update 0.291 s at t=4.003
```

One loop thread updates every client in slices of 100 with a server update after each slice, so an update gap near the 3-second reliable timeout means the loop starved the client of updates; a short gap means the client timed out while it was updated on time.

### Dual-stack client ports

Darwin lets a dual-stack socket (`[::]:0`) take an ephemeral port an IPv4 socket on the host already holds, and delivers IPv4 datagrams for that port to the IPv4 socket, so a dual-stack client of an IPv4 server sends but never hears the server and sits in `requesting cookie` until its deadline. On macOS the client of an IPv4 server binds an IPv4 socket (`sdk/rocketnet_client_transport.cpp:790-799`), and an IPv4 socket's ephemeral port is never one another IPv4 socket holds. `load` connects its clients to `127.0.0.1` unless `LOAD_SERVER_ADDRESS` names another host (`load.c:524-545`), so every load client takes that IPv4 bind.

### Server cells

macOS has two server transports, and the darwin batch pump has a degraded path; the load gate runs each:

```
ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable ./bin/load 1000 60   # portable
ROCKETNET_SERVER_TRANSPORT=msgx ROCKETNET_CLIENT_TRANSPORT=msgx ./bin/load 1000 60           # msgx
ROCKETNET_PUMP=portable ./bin/load 1000 60                                                   # msgx-degraded
```

The msgx-degraded cell runs the auto ladder (`msgx,portable`) under the `ROCKETNET_PUMP=portable` override: the msgx rung refuses and the ladder falls to portable, so the summary names `server_driver=portable`. A pinned `msgx` under that override refuses at create by design, so `load` exits `1` with `failed to create message server`.

### Gate runs

The macOS gate runs the light profile (`LOAD_PROFILE=light`): 1000 clients, each updating at 10 Hz and sending one reliable-ordered message per update, and each sent one by the server. It is three consecutive runs of each server cell, nine runs in all, with `LOAD_PROFILE=light` in the environment of every command below. Each summary line must say `verdict=PASS`, `fds_end` must equal `fds_start`, `threads_end` must equal `threads_start`, and `connect_p50_s` and `connect_max_s` are recorded for the run table:

```
for cell in portable msgx msgx-degraded; do
  for run in 1 2 3; do
    case "$cell" in
      portable) env ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable ./bin/load 1000 60 ;;
      msgx) env ROCKETNET_SERVER_TRANSPORT=msgx ROCKETNET_CLIENT_TRANSPORT=msgx ./bin/load 1000 60 ;;
      msgx-degraded) env ROCKETNET_PUMP=portable ./bin/load 1000 60 ;;
    esac
  done
done
```

A refused run is a finding, not a partial run: `bin/load` names the limit and the command that raises it before it opens a socket, and exits `1`.

A host that denies the loopback bind (a sandbox or firewall) makes every server transport refuse to start; `bin/load` then exits `1` with the transport code it got, before any client is created:

```
load: failed to create message server (status=18 TRANSPORT_LADDER_EXHAUSTED)
```
