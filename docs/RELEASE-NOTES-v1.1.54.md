# RocketNet v1.1.54

Changes compared with the `v1.1.53` tag. A point release: no packet layout or
protocol version change, and v1.1.54 endpoints interoperate with every v1.x
endpoint. No counter row is added: the contract holds 290 counters and no
released id moved.

- gso transport, receive: a datagram the worker has received is delivered at
  the server's next receive call. Before, a filled buffer waited in a single
  ready slot until main claimed it, so every datagram waited one more server
  update than on io_uring (a steady mean of 1.49 updates against 0.52; now
  0.52). Main reads the worker's buffer in place up to the count the worker
  committed; main never blocks and nothing is copied twice. The kernel's
  drops at a full queue socket are now counted in
  `GSO_RECEIVE_DROPPED_OVERFLOW`.
- Client:
  - A client racing both address families now closes every family's socket
    when its I/O thread exits. Before, the kept family stayed open until
    destroy, and datagrams that arrived after the exit were freed unread.
  - A send refused because the payload ring is full no longer uses up a
    session sequence number.
  - The client's rate stats are fractional kbps, as the server's are.
- Server lifecycle:
  - Polling `rocketnet_server_stopped` while a fill call runs is refused and
    counted, not raced.
  - A destroy whose stop is refused tries the stop again every tenth of a
    second inside its bounded wait, and logs the true cause if it keeps the
    server.
  - An IPv4-mapped address is refused as the IPv6 address of server create
    and of a connect token (`INVALID_ADDRESS`).
  - `required_memory` answers 0 when no transport can plan, and negative
    thread counts are refused, never read as the default.
- Ordered channels: the duplicate fix of v1.1.53 now says what its held
  lookups count, and a test checks that its two duplicate tests agree. No
  behaviour change.
- macOS msgx transport: a socket's reads in one round no longer go past its
  budget for the round (before, up to the budget plus one batch less one).
- Windows IOCP transport: a receive post now counts itself pending before it
  tests whether the endpoint is stopping. Before, a post could land after its
  owner had drained and exited, and the stop never completed.
- `rocketnet_time` counts suspend: `CLOCK_BOOTTIME` on Linux and
  `CLOCK_MONOTONIC` on macOS. Before, after a suspend longer than the
  30-second token creation skew, a server on token epoch 0 refused every
  fresh connect token until restart. Windows is unchanged.
- CI: nightly shard summary jobs find their shards under the caller's prefix.
- Tests: the functional disconnect delivery case no longer counts a late
  duplicate connection response, arriving after the disconnect, as a fault
  (it reddened a macOS run of v1.1.53's main).
- Tests and models: the fixes above carry tests that were red before, and
  model changes with reversed witnesses (RNReceiveHandoff, RNGsoHandoff,
  RNClientRace, RNMsgxReceiveFill, RNIocpRetryStop, RNIocpEndpoints), with
  their trace walks where the layer has one. Three model-coverage checks
  that `fix/v1.1.x/msgx-residue` failed on its own are repaired in the
  merge: the coverage table carried the `fill-phase` row twice (one row is
  kept, main's); a citation in `README-RNMsgxReceiveFill.md` named the test
  in a form the citation check reads as a missing symbol (reworded); and
  the strict action check, which the first red had kept from running, found
  RNMsgxReceiveFill's MainStop and Exit uncited in its header and Done not
  named as an environment action (cited and named).

Eight fix branches and the flaky-test repair, at these commits:

- `fix/v1.1.x/receive-handoff-latency`: `84f80fc1e91d58f69998aee1e340596f83bb5aee`
- `fix/v1.1.x/client-stop-and-race`: `4e5719ef7799572ef8e9af501aaba38d0dfdf884`
- `fix/v1.1.x/nightly-shard-summaries`: `1e672b490ed76860356a2a47ee56356023ab5c13`
- `fix/v1.1.x/msgx-residue`: `d13767a837db915b1bbfa3f14dae591d0df34b47`
- `fix/v1.1.x/server-lifecycle-and-plan`: `6085f04727396681fed7c014001b3e9de835584c`
- `fix/v1.1.x/ordered-duplicate-scan`, two follow-up commits cherry-picked:
  `fdf1541d6a0ec8a3a6ac52a4f29b5c974fedeb50` and
  `9b8d460d9085e66ec3733e80ccdf6a7b224c739d`
- `fix/v1.1.x/iocp-stop-race`: `ae3af09f610706c8959613d691b36eeb63626f62`
- `fix/v1.1.x/token-clock`: `a71da8e748e2efccf633422f88d1e9b36472c750`
- `fix/v1.1.x/flaky-disconnect-delivery`: `0ceee9e847f783e71fc198f9e89adfadeef8402e` (tests only)

- Tag: `v1.1.54`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.54).
