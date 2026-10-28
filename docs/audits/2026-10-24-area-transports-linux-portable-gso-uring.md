# Linux portable, GSO/GRO and io_uring transports audit, 2026-10-24

Cold reading audit of `mas-bandwidth/rocketnet` at `9775266a705148969cc00fcadc1463b06879cfbd`
(`main`). Scope read: the Linux and BSD section of `sdk/rocketnet_platform.cpp` (lines 522-2309)
and its shared tail (from line 5176), `sdk/rocketnet_platform.h`, `sdk/rocketnet_server_gso.h`,
`sdk/rocketnet_server_gso_linux.cpp`, `sdk/rocketnet_client_gso.h`,
`sdk/rocketnet_client_gso_linux.cpp`, `sdk/rocketnet_server_uring.h`,
`sdk/rocketnet_server_uring_linux.cpp`, `sdk/rocketnet_uring_codec.h`, `sdk/rocketnet_coalesce.h`,
the portable, gso and io_uring adapters in `sdk/rocketnet_driver_builtin.cpp`, the transport
ladders and receive loops of `sdk/rocketnet_server_transport.cpp` and
`sdk/rocketnet_client_transport.cpp`, the client I/O step in `sdk/rocketnet_client_io.h`,
`docs/LINUX.md` and `TRANSPORTS.md`, with the unit and functional tests that pin them. AF_XDP is
out of scope. Method: each candidate was traced to the input or call sequence that reaches it;
one with no reachable path today is graded risk or quality, never bug. No code was changed and no
candidate needed a scratch unit test to settle. Findings are ordered most severe first; each
gives severity (`bug` / `risk` / `quality`), `file:line` at 9775266a, a one-sentence defect, a
one-sentence concrete failure or cost, and the smallest fix.

## Findings

### 1. bug — `sdk/rocketnet_server_gso_linux.cpp:1431`

- Defect: When `sendmmsg` answers -1 for anything but a live GSO refusal, the flush counts every remaining message of the batch as dropped and abandons them, although -1 means only that the first unsent message (one destination) was refused.
- Failure or cost: One client whose sends fail on their own (a path MTU below the packet size giving `EMSGSIZE`, a netfilter `EPERM`, `EHOSTUNREACH` for its route) loses up to 127 later messages, to other clients, queued behind it in the same 128-message batch, on every flush that reaches it; the platform batch (`sdk/rocketnet_platform.cpp:1560`) and the gso client (`sdk/rocketnet_client_gso_linux.cpp:222`) drop only the refused message and send the rest.
- Smallest fix: Keep the drop-the-tail branch for `EAGAIN`, `EWOULDBLOCK` and `ENOBUFS`; for every other errno count `message_num_packets[sent]` as dropped, advance `sent` by one and continue the loop.

### 2. bug — `sdk/rocketnet_server_gso_linux.cpp:481`

- Defect: A queue binding a configured (non-zero) port sets `SO_REUSEPORT` before `bind`, so the first socket of the group joins any existing reuseport group of the same user on that port instead of being refused; `sdk/rocketnet_server_uring_linux.cpp:405` copies the rule.
- Failure or cost: A second gso or io_uring server started on a port a running gso or io_uring server already holds starts without error and the kernel splits the port's datagrams between the two processes, so clients hashed to the wrong process never connect, where the portable transport fails the bind with `EADDRINUSE`; the comment at lines 469-476 names exactly this sharing as the failure to avoid, but only for ephemeral ports.
- Smallest fix: Bind queue 0 without `SO_REUSEPORT` whatever the port and set it after the bind (the `discovers_port` path), so only this server's later queues can join, in both copies of `open_socket`.

### 3. bug — `sdk/rocketnet_client_gso_linux.cpp:366`

- Defect: With `UDP_GRO` on, `rocketnet_client_gso_receive_batch` always reads the first message even when the caller has fewer free slots than its segments, and drops the excess into `gro_packets_dropped`, a field no API, statistic, counter or log reads outside the unit tests.
- Failure or cost: When the client I/O buffer has fewer than 64 free slots (`sdk/rocketnet_client_io.h:572` passes the room as `max_packets`), a server burst coalesced into one GRO message loses every segment past the room on the client host, and the application sees it only as network packet loss.
- Smallest fix: For a GRO transport, have the I/O step treat a room below `ROCKETNET_CLIENT_GSO_RECEIVE_MAX_SEGMENTS` as a full buffer (publish, then wait on the wake alone) instead of reading into it, and fold `gro_packets_dropped` into the transport's published receive drops.

### 4. risk — `sdk/rocketnet_server_gso_linux.cpp:1415`

- Defect: A live `EINVAL`, `EIO` or `EOPNOTSUPP` on one segmented message turns `UDP_SEGMENT` off for the whole server for good, but the kernel returns `EINVAL` for a segment larger than one destination's cached path MTU and `EIO` for a destination routed through IPsec; `sdk/rocketnet_server_uring_linux.cpp:835` has the same rule.
- Failure or cost: One client behind a reduced path MTU (or an IPsec route) permanently degrades every client of the server to plain sends, and on io_uring each such refusal also drops the run.
- Smallest fix: Treat the refusal as per-destination (resend that run plain and keep the feature), and turn the feature off only when the same errno repeats across distinct destinations or on the first send after create.

### 5. risk — `sdk/rocketnet_server_uring_linux.cpp:1759`

- Defect: `receive_packets` (and the reap inside `send_packets`) budgets in completions, `ROCKETNET_URING_RECEIVE_BUDGET` of them, while with `UDP_GRO` one completion carries up to 64 datagrams into a fill bank of `ROCKETNET_URING_BANK_SIZE` packets, so the reap keeps consuming completions after the bank is full.
- Failure or cost: Under coalesced load the packets past the bank are dropped (counted `RECEIVE_DROPPED_OVERFLOW` at line 770) although their completions could have stayed queued, contradicting the header's "one receive bank's worth" budget (`sdk/rocketnet_server_uring.h:68`) and TRANSPORTS.md's "completions past a budget stay queued for the next call".
- Smallest fix: Stop the reap when the fill bank has fewer than `ROCKETNET_URING_MAX_SEGMENTS` free entries, counting the call `REAP_DEFERRED`, so the remaining completions wait for the next call.

### 6. risk — `sdk/rocketnet_platform.cpp:2275`

- Defect: The connection-type probe ignores the result of `sendmsg` and reads the reply on a blocking netlink socket with no timeout until an `NLMSG_DONE`, skipping an `NLMSG_ERROR` reply (the loop at line 2104 tests only the bare numbers 24 and 3).
- Failure or cost: In a sandbox or kernel that refuses the request (a seccomp or LSM refusal of the send, or an error reply to the dump), `rocketnet_platform_init` blocks forever inside `recvmsg` instead of falling back to `ROCKETNET_CONNECTION_TYPE_UNKNOWN`.
- Smallest fix: Return UNKNOWN when `sendmsg` fails, treat `NLMSG_ERROR` as the end of a failed dump, set `SO_RCVTIMEO` on the probe socket, and use `RTM_NEWROUTE` and `NLMSG_DONE` by name.

### 7. risk — `sdk/rocketnet_server_gso_linux.cpp:670`

- Defect: A receive worker whose buffer is full while a ready buffer still waits for the main thread loops on `poll`, which returns at once because the unread socket stays readable, and takes `receive_mutex` on every pass.
- Failure or cost: While the main thread is late claiming a queue (a slow update under load), each gso worker spins at a full core and contends the lock main try-acquires, the spin the portable worker avoids by waiting on its wake alone when full (`sdk/rocketnet_driver_builtin.cpp:316`).
- Smallest fix: When the buffer is full and the publish fails, wait on an event-only wake that `rocketnet_server_gso_receive_from_queue` sets when it returns a free buffer, instead of polling the socket.

### 8. risk — `sdk/rocketnet_platform.cpp:936`

- Defect: An IPv6 socket sets only `IPV6_MTU_DISCOVER`, which governs IPv6 output; IPv4 datagrams it sends to v4-mapped destinations keep the kernel's IPv4 default (`IP_PMTUDISC_WANT`), and the gso and io_uring sockets do the same with `IPV6_DONTFRAG` (`sdk/rocketnet_server_gso_linux.cpp:598`, `sdk/rocketnet_server_uring_linux.cpp:513`).
- Failure or cost: A dual-stack server (a "[::]" bind or a second public address) or a dual-stack client fragments an IPv4 send larger than the known path MTU locally, where an IPv4-bound socket refuses it with `EMSGSIZE`, so the comment "set don't fragment bit" holds for one family only and the two binds behave differently on the same path.
- Smallest fix: Also set `IP_MTU_DISCOVER` to `IP_PMTUDISC_DO` (level `IPPROTO_IP`) on IPv6 dual-stack sockets in all three socket opens.

### 9. quality — `docs/LINUX.md:186`

- Defect: The io_uring section describes the old geometry and send path: 4096 buffers of 2048 bytes (the code has 66 KiB buffers), one completion per datagram, one `SENDMSG` per staged slot, and a bounded wait of `ROCKETNET_URING_SEND_WAIT_SECONDS` (line 199), a constant that exists nowhere; TRANSPORTS.md:438 likewise says `send_packets` "waits at most once" while it never waits.
- Failure or cost: An operator sizing memory or latency from the deployment guide reads a receive footprint 33 times too small and a send wait that does not happen, and several of the section's line citations (`:383-421` for the probe, `:679-683` for its refusal) point at unrelated code.
- Smallest fix: Rewrite the receive and send paragraphs from `sdk/rocketnet_server_uring.h` (66 KiB buffers, 264 MiB per queue, segmented runs, no wait) and drop the wait sentence from TRANSPORTS.md.

### 10. quality — `sdk/rocketnet_server_gso_linux.cpp:778`

- Defect: TRANSPORTS.md:137 says the coalescing rules "live once" in `sdk/rocketnet_coalesce.h`, but the gso server re-implements the receive split here and the send run at line 1291, and the three receivers disagree on an untrustworthy stride: the gso server splits at any stride, io_uring drops the buffer (`sdk/rocketnet_server_uring_linux.cpp:741`), the client keeps it as one datagram (`sdk/rocketnet_client_gso_linux.cpp:349`).
- Failure or cost: A fix to the split or run rule lands in `rocketnet_coalesce.h` and silently misses the transport the rule was written from, and the same malformed coalesced receive is delivered, dropped or cut depending on the transport.
- Smallest fix: Call `rocketnet_coalesce_split` and `rocketnet_coalesce_send_run` from the gso server and pick one fault rule for a stride that cuts more than the segment maximum.

### 11. quality — `sdk/rocketnet_platform.cpp:1540`

- Defect: The `sendmmsg` error rule is written three times and disagrees: the platform batch retries `EINTR` without bound, the gso server drops the whole tail on it (`sdk/rocketnet_server_gso_linux.cpp:1413`), and the gso client drops one message (`sdk/rocketnet_client_gso_linux.cpp:226`).
- Failure or cost: A reader cannot tell which behaviour is the transport contract, and finding 1 is the cost of the copies drifting.
- Smallest fix: One shared helper that classifies a `sendmmsg` errno as retry, drop-one or drop-tail, called by all three.

### 12. quality — `sdk/rocketnet_platform.cpp:5268`

- Defect: Linux does not define `ROCKETNET_PLATFORM_HAS_BATCH_PUMP`, so the batch pump seam falls to the per-packet loop on Linux although `sendmmsg`/`recvmmsg` implementations sit in the same file, contradicting the seam comment that a platform with batch syscalls overrides it (`sdk/rocketnet_platform.h:386`).
- Failure or cost: The portable server's TX and RX workers (`sdk/rocketnet_driver_builtin.cpp:195`, `:294`) make one syscall per datagram on Linux while the portable client on the same host batches, and nothing names the difference.
- Smallest fix: Route the Linux `rocketnet_platform_socket_send_packets` and `rocketnet_platform_socket_receive_packets` to the `_mmsg` functions, or correct both comments to say Linux batches only through the explicit `_mmsg` calls.

### 13. quality — `sdk/rocketnet_server_gso_linux.cpp:1143`

- Defect: Code comments cite line numbers that no longer hold: this one names the size pass at `:532-586` and the fold at `:1209-1210` (now lines 783-793 and 1478-1480), `sdk/rocketnet_server_uring_linux.cpp:1462` names `:636-638, :653-655` (now `get_sqe` and `arm`), and `sdk/rocketnet_driver_builtin.cpp:972` and `:1012` place the gso and io_uring `receive_drops` at `:869` and `:1223` (now 1146 and 1465).
- Failure or cost: A maintainer following the cites lands on unrelated code when checking which drops feed `PREFILTER_TOO_SHORT` and `PREFILTER_TOO_LARGE`.
- Smallest fix: Replace the line numbers with function names (`rocketnet_server_gso_worker`, `rocketnet_server_uring_receive_cqe`, `rocketnet_server_gso_receive_drops`).

### 14. quality — `sdk/rocketnet_platform.cpp:689`

- Defect: `rocketnet_platform_hostname_resolve` reads only the first `getaddrinfo` result and asserts on any family but IPv4 or IPv6, while its header (`sdk/rocketnet_platform.h:210`) promises the first supported result and a false return for an unsupported family.
- Failure or cost: The documented contract and the code differ: a resolver answer whose first entry is another family trips the assert in a debug build and returns false in release even when a later entry is usable.
- Smallest fix: Walk `ai_next` to the first IPv4 or IPv6 entry and return false without asserting when none exists.

## Counts

- bug: 3 (findings 1–3)
- risk: 5 (findings 4–8)
- quality: 6 (findings 9–14)
- total: 14

## Already filed

None. The findings of the two code-quality audits already in `docs/audits/` at this sha lie
outside this scope, and none of them is repeated here.
