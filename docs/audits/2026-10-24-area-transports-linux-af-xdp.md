# Linux AF_XDP transport audit, 2026-10-24

Cold reading audit of the Linux AF_XDP server transport in `mas-bandwidth/rocketnet` at
`9775266a705148969cc00fcadc1463b06879cfbd` (`main`). Scope read completely: socket and UMEM setup,
the fill, completion, receive and transmit rings, frame ownership and the free lists, the in-host
replay marks, the gateway netlink lookup, and the BPF program that steers packets —
`sdk/rocketnet_server_socket_linux.cpp`, `sdk/rocketnet_server_socket.h`,
`sdk/rocketnet_xdp_compat.h`, `sdk/rocketnet_xdp_frame.h`, `sdk/rocketnet_xdp_free_list.h`,
`sdk/rocketnet_xdp_limits.h`, `sdk/rocketnet_xdp_replay_marks.h`,
`sdk/rocketnet_xdp_socket_config.h`, `sdk/rocketnet_gateway_netlink_linux.cpp`,
`sdk/rocketnet_gateway_netlink.h`, `server_xdp/server_xdp.c`, `tests/xdp_host/server_xdp_host.c`,
`docs/LIVE-XDP.md`, with the tests that pin them (`tests/test_unit_xdp_*.cpp`, `tests/test_xdp.cpp`,
`tests/test_xdp_filter.cpp`) and the callers and contracts they cite (`sdk/rocketnet_driver.h`,
`sdk/rocketnet_driver_builtin.cpp`, `sdk/rocketnet_server_replay_marks.cpp`, `sdk/rocketnet_wire.h`,
`sdk/rocketnet_constants.h`). Method: each candidate was traced to the input or call sequence that
reaches it; one with no reachable path today is graded risk or quality, never bug. No code was
changed and nothing was attached to an interface. Findings are ordered most severe first; each
gives severity (`bug` / `risk` / `quality`), `file:line` at 9775266a, a one-sentence defect, a
one-sentence concrete failure or cost, and the smallest fix.

## Findings

### 1. bug — `sdk/rocketnet_server_socket_linux.cpp:884`

- Defect: The R3 exclusivity check trusts `rocketnet_xdp_query_id` with flags 0, and libbpf's `bpf_xdp_query_id` (and the legacy `bpf_get_link_xdp_id`) reports program id 0 whenever programs are attached in more than one mode (`XDP_ATTACHED_MULTI`), so an interface carrying both a generic-mode and a native-mode program reads as free.
- Failure or cost: The pre-flight reports the machine provisioned while the XDP slot is taken, and create then either fails at attach after the pre-flight passed or, under libxdp's multi-program dispatcher, joins a chain behind another program that can drop or pass this server's packets first, which the header's R3 ("no program of anyone's attached") exists to refuse.
- Smallest fix: Query with `bpf_xdp_query` and refuse when any of `prog_id`, `drv_prog_id`, `skb_prog_id` or `hw_prog_id` is non-zero (the legacy path reads `XDP_FLAGS_DRV_MODE` and `XDP_FLAGS_SKB_MODE` separately).

### 2. risk — `sdk/rocketnet_server_socket_linux.cpp:2806`

- Defect: `rocketnet_server_socket_receive_packets` always drains queue 0 first and stops at the shared `ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS` budget (line 2837), and a partly read buffer keeps its cursor, so a queue that holds a full budget of packets on every call is served first on every call.
- Failure or cost: A sender whose 4-tuple hashes to queue 0 and who sustains 16000 filter-passing packets per update (handshake types carry no replay check) starves every later queue: their staging buffers fill, their workers stop peeking, and the NIC drops every other client's packets, with no counter naming the starvation.
- Smallest fix: Start each drain at a rotating queue (one `int` cursor advanced per call), or cap each queue's share of the budget at `budget / num_queues` before handing the remainder out.

### 3. risk — `sdk/rocketnet_server_socket_linux.cpp:2341`

- Defect: A forget the kernel refuses (`bpf_map_delete_batch` failing with anything but ENOENT/EINVAL/ENOTSUPP, or `bpf_map_delete_elem` failing at line 2336) is counted and never retried, because the walk already cleared the slot's mark record when it issued the forget (`sdk/rocketnet_server_replay_marks.cpp:115`).
- Failure or cost: The stale high-water stays in the kernel map for that source address and port, so a later connection from the same address and port (a reconnect through the same NAT mapping) has every session packet dropped in the kernel as replayed, and since none reaches userspace its `highest_opened` never moves and no mark ever overwrites the entry: the client connects and then times out.
- Smallest fix: Keep failed forgets in the pending buffer for the next commit (reset only the entries that succeeded), or re-issue a forget for every slot whose mark was cleared in an update whose commit failed.

### 4. risk — `sdk/rocketnet_server_socket_linux.cpp:2857`

- Defect: Each XDP thread is pinned to `num_os_cpus + queue`, but the pre-flight only checks that the isolated set has at least `num_queues` CPUs (line 949); nothing checks that those pin targets exist, are online or are isolated, and `pin_thread_to_cpu` asserts `cpu < num_cpus` against the online count (line 2689).
- Failure or cost: With in-range settings such as `num_os_cpus = 2`, 7 queues and `isolcpus=1-7` on an 8-CPU machine, the pre-flight passes, the last thread's target is CPU 8, an assert-enabled build aborts in the thread, and a Release build logs the refused affinity and runs that queue unpinned on the OS cores.
- Smallest fix: In the pre-flight, refuse unless every CPU in `num_os_cpus .. num_os_cpus + num_queues - 1` is in the isolated list, and replace the assert with the same refusal path the out-of-set case already takes.

### 5. risk — `sdk/rocketnet_gateway_netlink_linux.cpp:225`

- Defect: The route match reads `RTA_MULTIPATH` only for `AF_INET`, and for IPv4 only the first next hop, so an IPv6 equal-cost default route (which the kernel dumps as `RTA_MULTIPATH` with no `RTA_OIF`) or an IPv4 one whose first next hop is on another interface never matches.
- Failure or cost: The dump ends `DONE`, create treats the interface as having no default route and starts gatewayless with the warning at `sdk/rocketnet_server_socket_linux.cpp:1420`, so every reply to an address not yet learned and not answering a received frame is refused as `SEND_NO_LINK_ADDRESS` on a routed host.
- Smallest fix: Walk every `rtnexthop` record for both families and take the first whose interface is this one; refuse with a named error, rather than report no route, when a default route exists but none of its next hops is on the interface.

### 6. risk — `sdk/rocketnet_server_socket_linux.cpp:2958`

- Defect: The receive loop records `desc->addr` for the fill ring before `rocketnet_xdp_receive_descriptor` judges it, so a descriptor refused for lying outside the receive region is still handed back to the kernel as a fill frame.
- Failure or cost: The check prevents the out-of-region read but not the ownership error it implies: a descriptor naming a send-region frame (a driver or kernel defect, the only way one arrives) puts that frame on the fill ring while it is also on the send free list, and the next received packet overwrites a frame being transmitted.
- Smallest fix: Append the address to the pending return only when it lies in the receive region (count the others in `receive_drops.descriptor` and drop them), and keep the frame alignment rule for the ones returned.

### 7. risk — `sdk/rocketnet_server_socket_linux.cpp:328`

- Defect: `get_interface_mac_address` parses `/sys/class/net/<if>/address` at fixed offsets after one `fgets` without checking that 17 characters of `xx:xx:xx:xx:xx:xx` were read, unlike the environment override (lines 424–432), which is strict-parsed.
- Failure or cost: An interface whose address file is empty or short (an L3 device such as WireGuard or tun, which the skb-mode fallback can attach to) makes `strtol` read uninitialised stack bytes as the server's source MAC and create succeeds, sending every frame from an undefined address instead of refusing the interface.
- Smallest fix: Require `strlen` of the line (newline stripped) to be 17 and validate it with the same character loop the override uses; refuse create otherwise.

### 8. risk — `sdk/rocketnet_server_socket_linux.cpp:2285`

- Defect: A learned reply MAC is forgotten only inside `rocketnet_server_socket_replay_forget`, which the replay walk issues only for slots that held a kernel mark, and a slot holds no mark while its `highest_opened` is 0 (`sdk/rocketnet_server_replay_marks.cpp:132`), although `client_accepted` learns the MAC on the first opened packet.
- Failure or cost: A client accepted with only sequence 0 opened (or one that leaves before its first mark commits) leaves its MAC in the table indefinitely, and because a learned MAC outranks the gateway and the answered frame in `rocketnet_xdp_reply_mac`, the next client at that address and port behind a different link-layer path has even its handshake replies sent to the stale MAC.
- Smallest fix: Forget the reply MAC from the transport's slot-release path (or from the walk for every slot that stopped being connected), independent of whether a replay mark was held.

### 9. risk — `sdk/rocketnet_server_socket_linux.cpp:2107`

- Defect: `rocketnet_server_socket_start_packet_internal` logs `rocketnet_warn( "sent too many packets!" )` on every start past `ROCKETNET_XDP_SEND_QUEUE_SIZE`, on the fill threads' hot path, with no rate limit, counter or queue named.
- Failure or cost: An update that over-stages one queue by N packets writes N identical log lines from concurrent fill threads inside the real-time send path, and the line says neither which queue overflowed nor how many packets were refused.
- Smallest fix: Count the refusals per queue (an atomic beside `send_refused_length`) and log once per flush with the queue and the count.

### 10. quality — `sdk/rocketnet_server_socket_linux.cpp:1822`

- Defect: Each queue's receive free list is built with all `ROCKETNET_XDP_NUM_FRAMES / 2` (32768) receive frames, but only `ROCKETNET_XDP_FILL_FRAMES` (16384) are ever taken (line 1845, at create), there is no free path, and frames return straight to the fill ring.
- Failure or cost: 64 MiB of registered, locked UMEM per queue (448 MiB at the default 7 queues) is never used yet is counted by `rocketnet_server_socket_memory_required` against `ROCKETNET_XDP_MEMORY_MIB`, along with the 256 KiB `receive_frames` array that is dead after create.
- Smallest fix: Size the receive region to `ROCKETNET_XDP_FILL_FRAMES` frames, write the fill addresses directly at create, and delete `receive_frames`, `num_free_receive_frames` and `alloc_receive_frame`.

### 11. quality — `sdk/rocketnet_server_socket.h:118`

- Defect: The header labels the receive and send entry points "(zero copy)" (lines 108 and 118), but the worker copies every received payload out of its UMEM frame into the staging buffer (`sdk/rocketnet_server_socket_linux.cpp:2995`) and copies every staged packet into a UMEM frame before sending (`sdk/rocketnet_server_socket_linux.cpp:3120`).
- Failure or cost: A reader sizing the transport's CPU or memory-bandwidth cost from the header is told there is no per-packet copy when there are two.
- Smallest fix: Say "staged (one copy each way)" and reserve "zero copy" for the UMEM bind mode the socket reports.

### 12. quality — `server_xdp/server_xdp.c:132`

- Defect: The comment over `server_xdp_is_ipv6_extension` says the program "does not walk the chain behind any of them", while lines 577–608 walk up to `SERVER_XDP_IPV6_EXTENSION_WALK` headers and drop a frame whose chain reaches UDP to this server.
- Failure or cost: The comment states the filter's IPv6 extension rule backwards beside the function the walk dispatches on, so a reader trusting it misjudges which extension-header frames the server ever sees.
- Smallest fix: Say the function names the headers the walk below starts from, and that only ESP and non-first fragments stop it unopened.

### 13. quality — `server_xdp/server_xdp.c:299`

- Defect: A UDP length larger than the IP header's datagram or larger than the frame is counted in `DROPPED_TOO_SMALL`, the same counter as a length below the UDP header.
- Failure or cost: An operator reading the prefilter counters sees oversized or inconsistent length fields reported as "too small", and cannot tell truncated senders from forged lengths.
- Smallest fix: Count the `udp_len > ip_udp_bytes || udp_len > frame_udp_bytes` case separately (or under `DROPPED_TOO_LARGE`), and update the rows that pin it in `tests/test_unit_xdp_filter_host.cpp`.

### 14. quality — `sdk/rocketnet_xdp_frame.h:283`

- Defect: After both family branches have already refused `frame_bytes < header_bytes` (lines 243 and 266), `rocketnet_xdp_frame_parse` checks it a third time and re-reads `udp_len` (line 289) that each branch already read and range-checked.
- Failure or cost: The duplicated guard and the third read of the same field make the parse's length rules look as if they differ by path, costing a reader a check that they do not.
- Smallest fix: Delete the check at line 283 and carry each branch's `udp_len` to the shared tail.

### 15. quality — `sdk/rocketnet_server_socket_linux.cpp:392`

- Defect: When the gateway's neighbour entry is missing, `get_gateway_mac_address_netlink` logs the reason only for IPv6 and returns false silently for IPv4.
- Failure or cost: An IPv4 server whose gateway has no usable ARP entry refuses create with only the generic "server could not get gateway mac address", hiding the one errand (populate the neighbour entry) the IPv6 path names.
- Smallest fix: Drop the `if ( ipv6 )` so both families log the missing neighbour entry with the gateway address and interface.

### 16. quality — `sdk/rocketnet_server_socket_linux.cpp:1420`

- Defect: A server serving both families sets `gateway_known` only from the IPv4 discovery (lines 1383–1387), so with an IPv6 gateway and no IPv4 default route it warns "server found no gateway" although `reply_macs.gateway_ipv6` is set and used.
- Failure or cost: The start-up log tells the operator replies to unlearned addresses are refused when IPv6 replies in fact go through the discovered IPv6 gateway.
- Smallest fix: Log the gateway outcome per family when serving both.

### 17. quality — `sdk/rocketnet_server_socket_linux.cpp:237`

- Defect: The per-queue `gateway_ethernet_address` is written at create (line 1795) and never read, since each frame takes `destination_mac` from its staging slot, and `server_id` and `match_id` (lines 255–256) are set at create and only logged.
- Failure or cost: Dead copies of the gateway MAC beside the reply-MAC table invite a maintainer to "fix" a reply path through a field nothing sends with.
- Smallest fix: Delete the per-queue field and its copy, and delete `server_id` and `match_id` or expose them through a caller.

### 18. quality — `sdk/rocketnet_xdp_replay_marks.h:67`

- Defect: `rocketnet_xdp_replay_marks_reset` zeroes `refused` at every commit and no production code reads it, while `sdk/rocketnet_server_socket.h:76–77` documents that a full buffer "counts in the buffer's refused count".
- Failure or cost: The documented refusal count is unobservable outside the unit test, and `rocketnet_xdp_replay_marks_forgets` returns only a count (no keys) unlike its `marks` sibling, so the accessor pair reads as unfinished.
- Smallest fix: Accumulate refusals into the socket's `replay_commit_failures` (or a sibling counter) before the reset, or delete the field and the header sentence; return the forget keys from the accessor or delete it.

## Counts

- bug: 1 (finding 1)
- risk: 8 (findings 2–9)
- quality: 9 (findings 10–18)
- total: 18

## Already filed

None: no finding of the two code-quality audits already under `docs/audits/` at 9775266a
falls in this area.
