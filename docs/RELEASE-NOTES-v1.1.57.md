# RocketNet v1.1.57

Changes compared with the `v1.1.56` tag. A point release: no packet layout or
protocol version change, and v1.1.57 endpoints interoperate with every v1.x
endpoint.

Public surface added in this release:

- Three server config fields: `gso_reserve_cpu_0`, `io_uring_reserve_cpu_0`
  and `portable_reserve_cpu_0` (environment
  `ROCKETNET_SERVER_GSO_RESERVE_CPU_0`, `ROCKETNET_SERVER_IO_URING_RESERVE_CPU_0`,
  `ROCKETNET_SERVER_PORTABLE_RESERVE_CPU_0`), with the values
  `ROCKETNET_CPU_0_RESERVED` (1; zero, the default, reads as it) and
  `ROCKETNET_CPU_0_NOT_RESERVED` (2). Any other value is refused at create.
- One counter, `PORTABLE_PIN_REFUSALS`, appended at id 290: the contract holds
  291 counters and no released id moved.

- Server receive, xdp, rio and iocp: a datagram the transport's worker has
  committed is delivered at the server's next receive call, as gso's has been
  since v1.1.54. Before, it could wait one more server update.
- Client I/O: the I/O thread opens no payload before the client is
  connected. Before, a payload that arrived while the client was still
  sending its connection response was opened and marked received, counted
  `WRONG_STATE` by update, and a later copy of it after connecting was dropped
  as already received. The client's receive handoff on the I/O thread is
  modelled and trace-checked.
- Receive: a datagram whose source port is 0 is refused on every transport
  that judges sources; the msgx and rio native IPv4 sources go through the
  same rule.
- io_uring:
  - The worker begins its shutdown explicitly; from then on only its retry
    timer ends a wait, and its bounded waits count the waits begun.
  - A refused receive waits for its own retry timer from the second refusal
    in a row.
  - Main only tries the send handoff's mutex and never waits on the worker.
  - The availability probe arms a multishot receive, and a kernel that takes
    it and answers `-ENOBUFS` is accepted.
  - A segmented send accepted on any queue keeps every queue segmenting.
    Before, a queue whose few destinations all refused stopped segmenting
    while other queues' segmented sends were accepted.
  - Source and no-buffer receive drops move `RECEIVE_FAILED`; a receive end
    that delivered nothing counts as refused.
- xdp receive: the per-call budget ends on the queue that used its last room,
  so a queue that fills the budget exactly no longer starves the next queue.
- Send capacity is judged per destination queue in the send pass, the
  maintenance sweep and the stop.
- Server update: `rocketnet_server_update` or
  `rocketnet_server_update_receive` called while the three-call update is
  still open counts as API misuse once, finishes the open update and begins
  none. Before, it then ran the next update's not-received check over the
  messages just finished, and disconnected every client that had sent one
  with `MESSAGE_NOT_RECEIVED`; one update finish refused because a game
  thread joined late was enough. A plain finish or abort is a main-thread
  call, checked before the scan.
- Linux thread placement:
  - New server config settings `gso_reserve_cpu_0`, `io_uring_reserve_cpu_0`
    and `portable_reserve_cpu_0` (`ROCKETNET_CPU_0_RESERVED`, the default, or
    `ROCKETNET_CPU_0_NOT_RESERVED`) say whether a transport's workers keep off
    cpu 0. `num_os_cpus` now applies to the gso, io_uring and portable
    transports as well as xdp.
  - The portable transport's send and receive workers on Linux (also the
    server behind the msgx name there) pin themselves as gso's do, counting a
    refused pin in the new `PORTABLE_PIN_REFUSALS`. On xdp, gso and io_uring
    as well, a worker whose pin is refused is still counted and warned about,
    and asks for no real-time priority.
- Documentation: the transport internals, receive handoff and sequence buffer
  documents say what the code does for the per-queue io_uring, lanes, bind
  addresses, receive order, stop retry bound and publish wait, and that gso
  and io_uring pin their workers whatever the priority switch says.
- Tests: receive latency rows for clients (portable everywhere, gso on Linux,
  msgx on macOS, iocp on Windows) report milliseconds first and fail when
  another transport is granted; placement tests expect what the host's cpuset
  allows; the stop's hold-back is tested; a red test for each receive and
  budget fix. In development builds each client latency row also fails unless
  every client armed its check by the end of the run, so a run where every
  payload was opened on the main thread can no longer pass. The portable
  client's latency row reports its delay without failing on it: a portable
  client's receive delay under load is not a requirement.
- The release pull request's first CI run found a crash the merged branches
  carried, repaired forward: the development-build seam that swaps a test's
  server transport operations recounted the send queues by reading the
  operations table before checking it for NULL, and the mass-disconnect unit
  case crashed on every platform. The check now comes first. A game cannot
  reach this: only the `ROCKETNET_DEV` test seam sets a NULL table.
- Models: `RNUringQueueWorker`, `RNUringQueueSend`, `RNSegmentRefusal`,
  `RNGsoRecvWalk`, `RNClientIoDecrypt`, `RNServerLinkStop` and the fill-phase
  model, with their records and trace walks where the layer has one.
- Static analysis: the selection floors are today's counts, and the generated
  xdp header is built before the analyzer runs.
- The Linux, Windows and macOS light memory pins and the loopback goldens are
  measured for this tree.

Merged branches:

- `fix/v1.1.x/linux-thread-placement`
- `fix/v1.1.x/xdp-budget-fairness`
- `fix/v1.1.x/windows-receive-handoff-latency`
- `fix/v1.1.x/send-queue-capacity`
- `fix/v1.1.x/xdp-static-repair`
- `fix/v1.1.x/uring-receive-rearm`
- `fix/v1.1.x/split-update-pass2`
- `docs/v1.1.x/transport-internals`
- `fix/v1.1.x/client-io-payload-state`
- `fix/v1.1.x/gso-send-retry`
- `fix/v1.1.x/v1157-hot-repairs`
- `fix/v1.1.x/v1157-disconnect-together-crash`

- Tag: `v1.1.57`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.57).
