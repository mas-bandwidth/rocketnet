# RocketNet v1.1.53

Changes compared with the `v1.1.52` tag. A point release: no packet layout or
protocol version change, and v1.1.53 endpoints interoperate with every v1.x
endpoint. No counter row is added: the contract holds 290 counters and no
released id moved.

- Sequenced channels: a newer unreliable record that would be refused (no free
  hold slot ahead of its turn, the update's unreliable delivery allowance
  spent in turn, or stale at the half range) no longer releases the channel's
  held reliable message first. Before, the refused record was dropped and the
  held message with it, so a message the sender had seen acked was never
  delivered, and an older one could be delivered in its place. The refused
  record now leaves the channel as it was (standard MESSAGES.md m6.3).
- Ordered channels: a duplicate at the read index, or a block fragment there
  that cannot complete its block, now reserves one delivery. Before, it
  looked up every complete held ordering after it on the main thread: a sender
  holding a full window could make each 13-byte record cost 153 us at a
  window of 1024 and 38 ms at 65536. It is now about 2 us.
- io_uring transport:
  - Once the server is stopping, each wait for a full socket counts once
    towards the retry bound, so a burst of wakes no longer uses the bound up
    in microseconds; past the bound the worker gives up what still waits, so
    a stop finishes however full the socket stays.
  - A full answer (-EAGAIN, -EINTR, -ENOBUFS or -ENOMEM) now waits on a 1 ms
    timer, as the gso worker does, never a poll for room: a socket can report
    room while the device queue behind it still refuses. On a socket that
    stays full this retries more often than before (measured at 1 Gbit:
    `URING_SEND_DEFERRED` 192k/s against 42k/s, 0.29 worker cores against
    0.16, for the same packets sent). The remedy, sending one run first after
    the timer, is on the roadmap (`uring-retry-probe-first`).
- macOS msgx transport:
  - A socket slot the sweep frees no longer keeps a stale dead report. Before,
    when the slot reopened and the poller refused the new socket, that socket
    stayed open and was never polled, and its client's datagrams were lost
    uncounted.
  - The receive worker reads only into room. The receive bank holds one
    update's whole receive budget (16000 datagrams at 1000 clients, was
    8192), so the server's planned memory grows by about 35 MB at 1000
    clients; when the bank is full the kernel's socket buffers keep the rest.
  - Every msgx socket sets its buffers through the platform's halving walk;
    the shared socket's granted receive buffer is read back, logged and kept
    in stats.
  - On a connected client socket, EHOSTUNREACH, ENETUNREACH, EHOSTDOWN and
    ENETDOWN are taken as ICMP reports, like ECONNREFUSED, and EINTR keeps the
    bank on send and is read past on receive.
  - The memory plan's socket buffer estimate counts what each kind of socket
    asks: 157 MB at 1000 clients, not 26 GB.
- Development builds: the crypto, ack-visit and sequenced-hold tallies are
  reached through functions in their own files, not `extern thread_local`,
  which GCC 15 with AddressSanitizer and UBSan aborted on.
- Tests and models: the fixes above carry unit tests that were red before,
  model changes with reversed witnesses (RNChannelTypes, RNOrderingWrap,
  RNMsgxSocketTable, RNMsgxReceiveFill, RNUringQueueWorker) and their trace
  walks.

The branches merged, at these commits:

- `fix/v1.1.x/uring-stop-and-full-socket`: `9e85710798dedc5cf703bef897389933461c1dbb`
- `fix/v1.1.x/msgx-receive-and-tls-review`: `4119919c7cb09f9d99a31081ae2d2e0643ee24a8`
- `fix/v1.1.x/ordered-duplicate-scan`: `2dd95eb38c64f2a828b6b70ae036fcee33b9396d`
- `fix/v1.1.x/sequenced-acked-holds`: `8c6c753420ac5b0ca13b52edc36453603a7514dc`
- `roadmap/flaky-macos-reply-later-update`: `9d4dfd86395700e020e09ec2ee6a94865e5aa5e7`

- Tag: `v1.1.53`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.53).
