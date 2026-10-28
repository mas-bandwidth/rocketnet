# Assert-only guard sweep, 2026-10-24

Sweep of `mas-bandwidth/rocketnet` at `9775266a705148969cc00fcadc1463b06879cfbd` for one bug class: an array index, a divisor, a size or length, or a pointer whose only check before use is `rocketnet_assert`, which compiles to nothing when `NDEBUG` is defined (`sdk/rocketnet_internal.h:285`–315, `ROCKETNET_ASSERTS` follows `NDEBUG`), so a release build reads, writes or divides unchecked. Scope read: every `.cpp` and `.h` in `sdk/` (`sdk/rocketnet_tests.cpp` only where it defines helpers production code calls), `functional/*.cpp`, and the `tests/*.h` helpers the functional and DST tiers share. Method: five patterns were run and every hit was judged by reading its function and following the value to its use, back through every caller reachable from the public API (`sdk/rocketnet.h`), the wire, or a config value. Pattern 1, `rocketnet_assert` over `sdk/*.cpp sdk/*.h functional/*.cpp tests/*.h`: 1059 hits. Pattern 2, a division by a lower-case name, `[^/*]/ [a-z_]` over the same files with comment lines dropped: 132 hits (the unfiltered `[^/]/ [a-z_]` gives 148; the 16 more are comment lines). Pattern 3, a modulo by a name or expression, `[a-zA-Z0-9_)\]] ?%=? ?[a-zA-Z_(]` over the same files with comment lines and format-string lines dropped: 225 hits. Reading showed divisions the second pattern misses (a cast or parenthesised divisor, such as `sdk/rocketnet_ack.cpp:752`), so it was widened twice over `sdk/` (test bodies and comment lines dropped): pattern 4, a parenthesised or cast divisor, `[^/*]/ ?\(` without `sizeof`: 15 hits; pattern 5, an upper-case divisor, `[^/*]/ ?[A-Z]`: 26 code hits, every one a compile-time constant. Of the 1457 hits, 13 are the seven findings below, 9 are already filed, 903 are in class and guarded by a real check, and 532 are out of class. Line numbers are at `9775266a`. Findings follow the format of `docs/audits/2026-10-24-code-quality-a.md`: severity (`bug` / `risk` / `quality`), `file:line`, a one-sentence defect, a one-sentence concrete failure or cost, and the smallest fix, most severe first. No source was changed. No hit reached a `bug`: every in-class use reachable from the public API, the wire or a supported config is guarded by a real check, a validated config bound applied at create, or a compile-time constant; the five risks are uses that a state the code permits, or a build option, would reach.

## Findings

### 1. risk — `sdk/rocketnet_encryption_map.cpp:259`

- Defect: `rocketnet_encryption_map_address` (and `rocketnet_encryption_map_entry` at line 243) bound `index` only with `rocketnet_assert( index >= 0 && index < map->capacity )`, and three callers pass a connected slot's mapping index with no `< 0` check: `rocketnet_server_protocol_accept_payload` (`sdk/rocketnet_server_protocol.cpp:2263`), the staged send path (`sdk/rocketnet_server_transport.cpp:2974`) and `rocketnet_server_transport_disconnect_client_impl` (`sdk/rocketnet_server_transport.cpp:3265`).
- Failure or cost: `rocketnet_client_slots_assign` stores any mapping index it is given, -1 included (`sdk/rocketnet_client_slots.cpp:204`), a state the code documents as permitted and checks for elsewhere (`sdk/rocketnet_server_protocol.cpp:1629`), so a connected slot holding -1 makes a release build read `address[-1]` on disconnect and staged send, and read then write `mapping[-1].accepted_session` on payload accept; the only production assignment (`sdk/rocketnet_server_protocol.cpp:873`) passes a live index today, so nothing reaches it yet.
- Smallest fix: Refuse a negative mapping index in `rocketnet_client_slots_assign` (return -1), which closes every caller at once; or add the `mapping_index < 0` check the other readers make at the three call sites.

### 2. risk — `sdk/rocketnet_config.cpp:875`

- Defect: The two auto-expansion loops of `rocketnet_transport_ladder_parse` (lines 902 and 1007) append every non-pin-only available transport to `candidates[ROCKETNET_MAX_TRANSPORT_CANDIDATES]` (8, `sdk/rocketnet_config.h:226`), and the only bound on that count is `rocketnet_assert( num_available <= ROCKETNET_MAX_TRANSPORT_CANDIDATES )`; the pinned path has a real check (line 953), the auto paths do not, and no `static_assert` ties `ROCKETNET_NUM_TRANSPORTS` to the array size.
- Failure or cost: A build that adds transports through `ROCKETNET_TRANSPORT_REGISTRY_HEADER` until one platform has nine or more auto-eligible transports writes past `candidates` in a release build whenever an endpoint is created with the transport unset or `auto`; the built-in registry has at most four auto-eligible transports per platform, so nothing reaches it today.
- Smallest fix: `static_assert( ROCKETNET_NUM_TRANSPORTS <= ROCKETNET_MAX_TRANSPORT_CANDIDATES, ... )` beside the array, or add `ladder->num_candidates < ROCKETNET_MAX_TRANSPORT_CANDIDATES` to both auto loops.

### 3. risk — `sdk/rocketnet_server_iocp_windows.cpp:800`

- Defect: `rocketnet_server_iocp_finish_packet` (and `rocketnet_server_iocp_abort_packet`, line 809) index `iocp->sends[packet_id]` with `packet_id` bounded only by `rocketnet_assert( packet_id < ROCKETNET_IOCP_SEND_QUEUE_SIZE )`, while the other staged transports derive the slot from the frame pointer and range-check it for real.
- Failure or cost: A finish or abort carrying an id the backend did not issue writes `packet_bytes` outside `sends` in a release build; every current caller passes an id from `start_packet`, and the public finish swaps the caller's context for a trusted lease first (`sdk/rocketnet_server_transport.cpp:3009`), so nothing reaches it today.
- Smallest fix: Begin both functions with `if ( packet_id >= ROCKETNET_IOCP_SEND_QUEUE_SIZE || frame != iocp->sends[packet_id].frame ) return;`.

### 4. risk — `sdk/rocketnet_send_handoff.h:36`

- Defect: `publish()` checks `filling >= 0` only with `rocketnet_assert`; the real check on line 37 covers the queue count, not `filling`.
- Failure or cost: A `publish()` without a prior successful `acquire()` queues -1, and the next `retire()` writes `free[-1] = true` outside the array in a release build; the header has no production includer (only unit and lifecycle tests use it), so nothing reaches it today.
- Smallest fix: `if ( filling < 0 ) return false;` before the queue write.

### 5. risk — `sdk/rocketnet_send_handoff.h:62`

- Defect: `retire()` guards an empty queue only with `rocketnet_assert( count > 0 )` before reading `queued[head]` and decrementing `count`.
- Failure or cost: A `retire()` on an empty queue in a release build frees whatever stale generation `queued[head]` names (possibly one still filling), advances `head` and leaves `count` at -1, after which `publish()` indexes the queue from a negative count; nothing reaches it today, for the reason given in finding 4.
- Smallest fix: `if ( count == 0 ) return;` at the top of `retire()`.

### 6. quality — `sdk/rocketnet_server_transport.cpp:1071`

- Defect: The host-memory estimate appends up to eight strings to `excluded[ROCKETNET_HOST_MEMORY_MAX_EXCLUSIONS]` (8, `sdk/rocketnet_layers.h:133`) and checks the count with `rocketnet_assert( count <= ROCKETNET_HOST_MEMORY_MAX_EXCLUSIONS )` only after every write.
- Failure or cost: The array is exactly full when the XDP, io_uring and worker-thread branches all apply, so the next exclusion line a maintainer adds overflows the stack array in every build, and the assert fires only after the write it exists to prevent.
- Smallest fix: Check the count before each append (or `static_assert` the number of append lines against the array size) instead of asserting after the writes.

### 7. quality — `sdk/rocketnet_platform.cpp:1964`

- Defect: The file-local template `rocketnet_vector_t` has `remove` (line 1964), `remove_ordered` (line 1975) and `insert` (line 1982), whose index is bounded only by `rocketnet_assert`, and none of the three is ever instantiated (the only instances, from line 2135, use `add` and loop-bounded `operator[]`).
- Failure or cost: Unchecked index paths that nothing calls are maintained and read as working container code, and the first caller to use one inherits an out-of-bounds write in release.
- Smallest fix: Delete the three unused members.

## Counts

- bug: 0
- risk: 5 (findings 1–5)
- quality: 2 (findings 6–7)
- total: 7

## Judged not a defect

Every hit of the five patterns that is not a finding above or already filed below, one row each. `guarded`: in class, and a real check (named, with its line) guards the use on every path. `out of class`: the assert or division is not about an index, divisor, size or pointer used afterwards (a state flag, a postcondition, a compile-time constant, a comment, or code only a test reaches). Cites inside a reason without a directory are in `sdk/`; a bare `:N` is in the row's own file.

### Pattern 1: `rocketnet_assert`

| file:line | judged | reason |
|---|---|---|
| `sdk/rocketnet_platform.cpp:277` | guarded | mutex_init reached only via rocketnet_platform_mutex_create, whose every caller passes the address of an embedded member (&server->mutex driver_builtin.cpp:395, &client_transport->mutex client_transport.cpp:1771, &gso->send_mutex server_gso_linux.cpp:1017); never NULL |
| `sdk/rocketnet_platform.cpp:320` | guarded | thread comes from rocketnet_platform_thread_create and every caller null-checks it before the priority call (client_transport.cpp:1810, server_gso_linux.cpp:1006/:1029, iocp_windows.cpp:726/:739, rio_windows.cpp:680) |
| `sdk/rocketnet_platform.cpp:348` | guarded | socket set at transport create, which fails on NULL (driver_builtin.cpp:390, :851; client_gso_linux.cpp:410); driver.cpp:162/:207 refuse a NULL instance |
| `sdk/rocketnet_platform.cpp:592` | guarded | every caller passes a literal/config name and a fixed array with sizeof (rocketnet.cpp:730, api.cpp:192-199/:1536, config.cpp:504/:651, server_socket_linux.cpp:421); length >= size refused at platform.cpp:598 |
| `sdk/rocketnet_platform.cpp:689` | out of class | assert(0) on an unknown address family; followed by return false, nothing indexed |
| `sdk/rocketnet_platform.cpp:763` | guarded | every caller passes &attempt, a local copy (driver.cpp:160/:205), after driver.cpp:152/:196 refuse a NULL bind |
| `sdk/rocketnet_platform.cpp:764` | out of class | address type is not an index/size/pointer; a NONE type takes the ipv4 bind path (no out-of-bounds access) |
| `sdk/rocketnet_platform.cpp:972` | guarded | socket from create, null-checked (driver_builtin.cpp:390/:851, client_gso_linux.cpp:410); server cleanup guards with if ( server->socket ) (driver_builtin.cpp:352) |
| `sdk/rocketnet_platform.cpp:1002` | guarded | socket from create, null-checked (driver_builtin.cpp:851, client_gso_linux.cpp:410; driver.cpp:207) |
| `sdk/rocketnet_platform.cpp:1003` | guarded | only callers pass &client_transport->protocol.server_address (client_transport.cpp:1503) or &race_server local (client_transport.cpp:1587) |
| `sdk/rocketnet_platform.cpp:1045` | guarded | socket from create, null-checked (driver_builtin.cpp:851, client_gso_linux.cpp:410; driver.cpp:207) |
| `sdk/rocketnet_platform.cpp:1046` | guarded | callers pass &io->to[ring] (client_io.h:482), &to locals (client_transport.cpp:734/:754), &race->server_address[1] (client_transport.cpp:595) or the protocol's member address; never NULL |
| `sdk/rocketnet_platform.cpp:1047` | out of class | address type is not an index/size/pointer; a non-v4/v6 type hits the explicit else "invalid address" refusal (platform.cpp:1107) or a v6 union read, no out-of-bounds access |
| `sdk/rocketnet_platform.cpp:1048` | guarded | callers pass a ring slot (client_io.h:481) or a stack array (client_transport.cpp:732/:750); never NULL |
| `sdk/rocketnet_platform.cpp:1049` | guarded | zero byte slots skipped (client_io.h:479), drains send only > 0 (client_transport.cpp:738/:752), seal results of 0 are dropped before the ring (client_transport.cpp:2719/:2777/:2814); the length reaches only the kernel's sendto, which refuses a bad length, no userspace read |
| `sdk/rocketnet_platform.cpp:1128` | guarded | socket from create, null-checked (driver_builtin.cpp:851, client_gso_linux.cpp:410; driver.cpp:207) |
| `sdk/rocketnet_platform.cpp:1129` | guarded | callers pass &from local (client_io.h:628, client_transport.cpp:3266) or &from[total] of a caller array inside a bounded loop (platform.cpp:5308) |
| `sdk/rocketnet_platform.cpp:1130` | guarded | callers pass a buffer slot (client_io.h:627) or scratch array (client_transport.cpp:3266) or packet_data+stride slot (platform.cpp:5308) |
| `sdk/rocketnet_platform.cpp:1131` | guarded | callers pass compile-time constants: ROCKETNET_RECEIVE_PACKET_BYTES (client_io.h:629, client_transport.cpp:3266) or packet_stride = ROCKETNET_FRAME_SIZE / ROCKETNET_RECEIVE_CEILING (client_io.h:578, driver_builtin.cpp:295) |
| `sdk/rocketnet_platform.cpp:1219` | out of class | assert(0) on an unknown family; followed by return 0 |
| `sdk/rocketnet_platform.cpp:1223` | guarded | the loop breaks only with result > 0: result < 0 returns (platform.cpp:1168-1179), 0 continues (:1182), hook path the same (:1158/:1160) |
| `sdk/rocketnet_platform.cpp:1264` | guarded | wake set at create which fails on NULL (driver_builtin.cpp:408/:857, client_gso_linux.cpp:418); destroys guarded where it can be NULL (driver_builtin.cpp:348/:350) |
| `sdk/rocketnet_platform.cpp:1272` | guarded | wake set at create which fails on NULL (driver_builtin.cpp:408/:857, client_gso_linux.cpp:418, server_gso_linux.cpp:1023) |
| `sdk/rocketnet_platform.cpp:1309` | guarded | wake set at create which fails on NULL (driver_builtin.cpp:408/:857, client_gso_linux.cpp:418, server_gso_linux.cpp:1023); NULL-possible sites guard it (driver_builtin.cpp:336/:337/:518, server_gso_linux.cpp:314/:1046) |
| `sdk/rocketnet_platform.cpp:1368` | guarded | BSD wake: set at create which fails on NULL (driver_builtin.cpp:408); destroys guarded (driver_builtin.cpp:348/:350) |
| `sdk/rocketnet_platform.cpp:1376` | guarded | BSD wake: set at create which fails on NULL (driver_builtin.cpp:408) |
| `sdk/rocketnet_platform.cpp:1389` | guarded | BSD wake: set at create which fails on NULL (driver_builtin.cpp:408); NULL-possible sites guard it (driver_builtin.cpp:336/:337) |
| `sdk/rocketnet_platform.cpp:1487` | guarded | socket from create, null-checked (driver_builtin.cpp:851, client_gso_linux.cpp:410; driver.cpp:207) |
| `sdk/rocketnet_platform.cpp:1488` | guarded | callers pass the stack array sends[ROCKETNET_CLIENT_IO_BATCH] (client_io.h:448) or forward it (client_transport.cpp:611/:619); every index bounded by num_packets (platform.cpp:1505) |
| `sdk/rocketnet_platform.cpp:1508` | guarded | entries built with to = &io->to[ring] (client_io.h:459); never NULL |
| `sdk/rocketnet_platform.cpp:1509` | guarded | entries built with packet_data = ring slot pointer (client_io.h:460); never NULL |
| `sdk/rocketnet_platform.cpp:1510` | guarded | zero byte slots skipped before the entry is built (client_io.h:458); seal failures (0) never stored (client_transport.cpp:2719/:2777/:2814); iov_len goes only to the kernel, which refuses a bad length |
| `sdk/rocketnet_platform.cpp:1578` | guarded | socket from create, null-checked (driver_builtin.cpp:851, client_gso_linux.cpp:410; driver.cpp:207) |
| `sdk/rocketnet_platform.cpp:1579` | guarded | caller passes stack array from[ROCKETNET_CLIENT_IO_BATCH] (client_io.h:569) or from+received inside it (client_transport.cpp:647) |
| `sdk/rocketnet_platform.cpp:1580` | guarded | caller passes buffer->data slot (client_io.h:572) |
| `sdk/rocketnet_platform.cpp:1581` | guarded | stride is the compile-time constant ROCKETNET_FRAME_SIZE (client_io.h:578), forwarded unchanged by the race (client_transport.cpp:643-649) |
| `sdk/rocketnet_platform.cpp:1582` | guarded | caller passes stack array packet_bytes[ROCKETNET_CLIENT_IO_BATCH] (client_io.h:570) |
| `sdk/rocketnet_platform.cpp:1583` | guarded | max_packets = min(room, BATCH) with room > 0 by the loop condition (client_io.h:564-567); race passes max - received only when received < max (client_transport.cpp:645); loop bound total < max_packets (platform.cpp:1587) also covers <= 0 |
| `sdk/rocketnet_platform.cpp:1685` | guarded | shim_data null-checked at create before pthread_create (platform.cpp:1730) |
| `sdk/rocketnet_platform.cpp:1762` | guarded | thread null-checked after create at every site (driver_builtin.cpp:421/:661, client_transport.cpp:1810, server_gso_linux.cpp:1006/:1029); joins guarded where NULL-possible (driver_builtin.cpp:340/:345) |
| `sdk/rocketnet_platform.cpp:1768` | guarded | thread null-checked after create at every site (driver_builtin.cpp:421/:661, client_transport.cpp:1810, server_gso_linux.cpp:1006/:1029) |
| `sdk/rocketnet_platform.cpp:1811` | guarded | rocketnet_platform_thread_wake has no caller in sdk/ (grep: definitions only); any thread would come from a null-checked create |
| `sdk/rocketnet_platform.cpp:1836` | guarded | every caller passes the address of an embedded member (&server->mutex driver_builtin.cpp:395, &gso->send_mutex server_gso_linux.cpp:1017, &worker->mutex iocp_windows.cpp:736) |
| `sdk/rocketnet_platform.cpp:1842` | guarded | every caller passes &member or io->mutex = &client_transport->mutex (client_transport.cpp:1771); never NULL |
| `sdk/rocketnet_platform.cpp:1843` | out of class | ok is a state flag, not an index/size/pointer |
| `sdk/rocketnet_platform.cpp:1849` | guarded | pointer part: callers pass io->mutex = &client_transport->mutex (client_io.h:384/:404, client_transport.cpp:1771); ok is a state flag |
| `sdk/rocketnet_platform.cpp:1855` | guarded | every caller passes &member or io->mutex = &client_transport->mutex (client_transport.cpp:1771); never NULL |
| `sdk/rocketnet_platform.cpp:1856` | out of class | ok is a state flag, not an index/size/pointer |
| `sdk/rocketnet_platform.cpp:1862` | guarded | every caller passes &member (e.g. driver_builtin.cpp, server_gso_linux.cpp:1004); never NULL |
| `sdk/rocketnet_platform.cpp:1882` | guarded | the only instances are default-constructed (platform.cpp:2135/:2136); reserve refuses size < 0 at platform.cpp:1924 |
| `sdk/rocketnet_platform.cpp:1910` | guarded | data non-NULL whenever length > 0 (add() allocates at platform.cpp:1999); every [] use is inside a loop bounded by length (platform.cpp:2028, :2041, :2174) |
| `sdk/rocketnet_platform.cpp:1911` | guarded | every [] use is inside a loop bounded by length (platform.cpp:2028, :2041, :2174) |
| `sdk/rocketnet_platform.cpp:1917` | guarded | data non-NULL whenever length > 0 (platform.cpp:1999); every [] use is inside a loop bounded by length (platform.cpp:2028, :2041, :2174) |
| `sdk/rocketnet_platform.cpp:1918` | guarded | every [] use is inside a loop bounded by length (platform.cpp:2028, :2041, :2174) |
| `sdk/rocketnet_platform.cpp:2464` | guarded | every caller passes a fixed array with sizeof (rocketnet.cpp:730, api.cpp:192-199/:1536, config.cpp:504/:651); length >= size refused at platform.cpp:2470 |
| `sdk/rocketnet_platform.cpp:2553` | out of class | assert(0) on an unknown address family; followed by return false |
| `sdk/rocketnet_platform.cpp:2626` | guarded | every caller passes &attempt, a local copy (driver.cpp:160/:205), after driver.cpp:152/:196 refuse a NULL bind |
| `sdk/rocketnet_platform.cpp:2627` | out of class | address type is not an index/size/pointer; NONE takes the ipv4 bind path, no out-of-bounds access |
| `sdk/rocketnet_platform.cpp:2898` | guarded | socket from create, null-checked (driver_builtin.cpp:390/:851); cleanup guarded by if ( server->socket ) (driver_builtin.cpp:352) |
| `sdk/rocketnet_platform.cpp:2931` | guarded | socket from create, null-checked (driver_builtin.cpp:851; driver.cpp:207) |
| `sdk/rocketnet_platform.cpp:2932` | guarded | only callers pass &client_transport->protocol.server_address (client_transport.cpp:1503) or &race_server (client_transport.cpp:1587) |
| `sdk/rocketnet_platform.cpp:2974` | guarded | socket from create, null-checked (driver_builtin.cpp:390/:851; driver.cpp:162/:207) |
| `sdk/rocketnet_platform.cpp:2975` | guarded | callers pass &io->to[ring] (client_io.h:482), &to locals, &bank->to[i] (driver_builtin.cpp:194 via send_packets) or member addresses; never NULL |
| `sdk/rocketnet_platform.cpp:2976` | out of class | address type is not an index/size/pointer; other types hit the explicit refusal at platform.cpp:3040 or a v6 union read |
| `sdk/rocketnet_platform.cpp:2977` | guarded | callers pass ring slots, stack arrays or bank->data[i] (driver_builtin.cpp:194); server entries validated non-NULL at driver_builtin.cpp:458 |
| `sdk/rocketnet_platform.cpp:2978` | guarded | server path refuses <= 0 and > ROCKETNET_RECEIVE_CEILING at driver_builtin.cpp:458; client path skips 0 (client_io.h:479) and drains send only > 0 (client_transport.cpp:738/:752); length goes only to the kernel |
| `sdk/rocketnet_platform.cpp:3100` | guarded | socket from create, null-checked (driver_builtin.cpp:390/:851; driver.cpp:162/:207) |
| `sdk/rocketnet_platform.cpp:3101` | guarded | callers pass &from local (client_io.h:628, client_transport.cpp:3266) or &from[total] inside a loop bounded by max_packets (platform.cpp:3643) |
| `sdk/rocketnet_platform.cpp:3102` | guarded | callers pass a buffer slot / scratch array (client_io.h:627, client_transport.cpp:3266) or packet_data + total*stride inside the bounded loop (platform.cpp:3643) |
| `sdk/rocketnet_platform.cpp:3103` | guarded | callers pass compile-time constants ROCKETNET_RECEIVE_PACKET_BYTES (client_io.h:629, client_transport.cpp:3266) or stride ROCKETNET_RECEIVE_CEILING / ROCKETNET_FRAME_SIZE (driver_builtin.cpp:295, client_io.h:578) |
| `sdk/rocketnet_platform.cpp:3194` | out of class | assert(0) on an unknown family; followed by return 0 |
| `sdk/rocketnet_platform.cpp:3198` | guarded | loop breaks only with result > 0: < 0 returns (platform.cpp:3142-3157), 0 continues (:3159), hook path the same (:3132/:3134) |
| `sdk/rocketnet_platform.cpp:3243` | guarded | rocketnet_platform_socket_pump is called only from rocketnet_tests.cpp, on sockets from a checked create |
| `sdk/rocketnet_platform.cpp:3390` | guarded | socket from create, null-checked (driver_builtin.cpp:390/:851; driver.cpp:162/:207) |
| `sdk/rocketnet_platform.cpp:3391` | guarded | callers pass bank->sends (driver_builtin.cpp:195) or the client's stack sends[] (client_io.h:448); indexes bounded by num_packets (platform.cpp:3395/:3418/:3519) |
| `sdk/rocketnet_platform.cpp:3421` | guarded | server entries: to = &bank->to[i] (driver_builtin.cpp:194) after driver_builtin.cpp:458 refuses !packet->to; client: &io->to[ring] (client_io.h:459) |
| `sdk/rocketnet_platform.cpp:3422` | guarded | server entries refused when !packet_data (driver_builtin.cpp:458) and rebuilt as bank->data[i] (:194); client entries are ring slots (client_io.h:460) |
| `sdk/rocketnet_platform.cpp:3423` | guarded | server entries refused when <= 0 or > ROCKETNET_RECEIVE_CEILING (driver_builtin.cpp:458); client zero slots skipped (client_io.h:458); iov_len reaches only the kernel |
| `sdk/rocketnet_platform.cpp:3530` | guarded | socket from create, null-checked (driver_builtin.cpp:390/:851; driver.cpp:162/:207) |
| `sdk/rocketnet_platform.cpp:3531` | guarded | callers pass bank->from sized rx_capacity (driver_builtin.cpp:294) or the client's stack from[] (client_io.h:569); writes bounded by total < max_packets (platform.cpp:3545/:3641) |
| `sdk/rocketnet_platform.cpp:3532` | guarded | callers pass &bank->data[0][0] (driver_builtin.cpp:295) or a buffer slot (client_io.h:572) |
| `sdk/rocketnet_platform.cpp:3533` | guarded | stride is a compile-time constant: ROCKETNET_RECEIVE_CEILING (driver_builtin.cpp:295) or ROCKETNET_FRAME_SIZE (client_io.h:578) |
| `sdk/rocketnet_platform.cpp:3534` | guarded | callers pass bank->bytes (driver_builtin.cpp:295) or stack packet_bytes[] (client_io.h:570) |
| `sdk/rocketnet_platform.cpp:3535` | guarded | server: rx_capacity = PER_CLIENT * max_clients with max_clients > 0 or default (driver_builtin.cpp:31-32); client: room > 0 by loop condition (client_io.h:564-567); loops bounded by total < max_packets (platform.cpp:3545/:3641) |
| `sdk/rocketnet_platform.cpp:3668` | guarded | shim_data null-checked at create before pthread_create (mac thread_create, platform.cpp:3698-3702) |
| `sdk/rocketnet_platform.cpp:3730` | guarded | thread null-checked after create at every site (driver_builtin.cpp:421/:661, client_transport.cpp:1810); joins guarded where NULL-possible (driver_builtin.cpp:340/:345) |
| `sdk/rocketnet_platform.cpp:3736` | guarded | thread null-checked after create at every site (driver_builtin.cpp:421/:661, client_transport.cpp:1810) |
| `sdk/rocketnet_platform.cpp:3768` | guarded | rocketnet_platform_thread_wake has no caller in sdk/ (grep: definitions only) |
| `sdk/rocketnet_platform.cpp:3791` | guarded | every caller passes the address of an embedded member (&server->mutex driver_builtin.cpp:395, &client_transport->mutex); never NULL |
| `sdk/rocketnet_platform.cpp:3797` | guarded | every caller passes &member or io->mutex = &client_transport->mutex (client_transport.cpp:1771) |
| `sdk/rocketnet_platform.cpp:3798` | out of class | ok is a state flag, not an index/size/pointer |
| `sdk/rocketnet_platform.cpp:3804` | guarded | pointer part: io->mutex = &client_transport->mutex (client_io.h:384/:404, client_transport.cpp:1771); ok is a state flag |
| `sdk/rocketnet_platform.cpp:3810` | guarded | every caller passes &member or io->mutex = &client_transport->mutex (client_transport.cpp:1771) |
| `sdk/rocketnet_platform.cpp:3811` | out of class | ok is a state flag, not an index/size/pointer |
| `sdk/rocketnet_platform.cpp:3817` | guarded | every caller passes &member; never NULL |
| `sdk/rocketnet_platform.cpp:3884` | guarded | wake set at create which fails on NULL (driver_builtin.cpp:408/:857); destroys guarded where NULL-possible (driver_builtin.cpp:348/:350) |
| `sdk/rocketnet_platform.cpp:3898` | guarded | wake set at create which fails on NULL (driver_builtin.cpp:408/:857) |
| `sdk/rocketnet_platform.cpp:3945` | guarded | wake set at create which fails on NULL (driver_builtin.cpp:408/:857); NULL-possible sites guard (driver_builtin.cpp:336/:337/:518) |
| `sdk/rocketnet_platform.cpp:4034` | guarded | reached only via mutex_create, whose callers pass &member (driver_builtin.cpp:395, iocp_windows.cpp:736) |
| `sdk/rocketnet_platform.cpp:4072` | guarded | thread null-checked before the priority call (iocp_windows.cpp:726/:739, rio_windows.cpp:680, client_transport.cpp:1810) |
| `sdk/rocketnet_platform.cpp:4148` | guarded | every caller passes a fixed array with sizeof (rocketnet.cpp:730, api.cpp:192-199/:1536, config.cpp:504/:651); required > size refused at platform.cpp:4155 |
| `sdk/rocketnet_platform.cpp:4176` | guarded | shim_data null-checked at create before CreateThread (platform.cpp:4196) |
| `sdk/rocketnet_platform.cpp:4219` | guarded | thread null-checked after create at every site (iocp_windows.cpp:726/:739, rio_windows.cpp:680, driver_builtin.cpp:421, client_transport.cpp:1810); teardown guarded (rio_windows.cpp:518-520, iocp_windows.cpp:571-584) |
| `sdk/rocketnet_platform.cpp:4225` | guarded | thread null-checked after create at every site (iocp_windows.cpp:726/:739, rio_windows.cpp:680, driver_builtin.cpp:421, client_transport.cpp:1810) |
| `sdk/rocketnet_platform.cpp:4234` | out of class | the Windows thread_wake never dereferences thread ((void) thread); and it has no caller in sdk/ |
| `sdk/rocketnet_platform.cpp:4254` | guarded | every caller passes the address of an embedded member (&worker->mutex iocp_windows.cpp:736, &server->mutex driver_builtin.cpp:395) |
| `sdk/rocketnet_platform.cpp:4260` | guarded | every caller passes &member or io->mutex = &client_transport->mutex (client_transport.cpp:1771) |
| `sdk/rocketnet_platform.cpp:4261` | out of class | ok is a state flag, not an index/size/pointer |
| `sdk/rocketnet_platform.cpp:4267` | guarded | pointer part: io->mutex = &client_transport->mutex (client_io.h:384/:404, client_transport.cpp:1771); ok is a state flag |
| `sdk/rocketnet_platform.cpp:4273` | guarded | every caller passes &member or io->mutex = &client_transport->mutex (client_transport.cpp:1771) |
| `sdk/rocketnet_platform.cpp:4274` | out of class | ok is a state flag, not an index/size/pointer |
| `sdk/rocketnet_platform.cpp:4280` | guarded | every caller passes &member; never NULL |
| `sdk/rocketnet_platform.cpp:4459` | out of class | assert(0) on an unknown address family; followed by return false |
| `sdk/rocketnet_platform.cpp:4566` | guarded | every caller passes &attempt, a local copy (driver.cpp:160/:205), after driver.cpp:152/:196 refuse a NULL bind |
| `sdk/rocketnet_platform.cpp:4567` | out of class | address type is not an index/size/pointer; NONE takes the ipv4 bind path, no out-of-bounds access |
| `sdk/rocketnet_platform.cpp:4744` | guarded | socket from create, null-checked (driver_builtin.cpp:390/:851); cleanup guarded by if ( server->socket ) (driver_builtin.cpp:352) |
| `sdk/rocketnet_platform.cpp:4787` | guarded | socket from create, null-checked (driver_builtin.cpp:851; driver.cpp:207) |
| `sdk/rocketnet_platform.cpp:4788` | guarded | only callers pass &client_transport->protocol.server_address (client_transport.cpp:1503) or &race_server (client_transport.cpp:1587) |
| `sdk/rocketnet_platform.cpp:4830` | guarded | socket from create, null-checked (driver_builtin.cpp:390/:851; driver.cpp:162/:207) |
| `sdk/rocketnet_platform.cpp:4831` | guarded | callers pass &io->to[ring] (client_io.h:482), &to locals, &bank->to[i] (driver_builtin.cpp:194 via send_packets) or member addresses; never NULL |
| `sdk/rocketnet_platform.cpp:4832` | out of class | address type is not an index/size/pointer; other types hit the explicit refusal at platform.cpp:4871 or a v6 union read |
| `sdk/rocketnet_platform.cpp:4833` | guarded | callers pass ring slots, stack arrays or bank->data[i]; server entries validated non-NULL at driver_builtin.cpp:458 |
| `sdk/rocketnet_platform.cpp:4834` | guarded | server path refuses <= 0 and > ROCKETNET_RECEIVE_CEILING at driver_builtin.cpp:458; client path skips 0 (client_io.h:479) and drains send only > 0 (client_transport.cpp:738/:752); length goes only to Winsock sendto |
| `sdk/rocketnet_platform.cpp:4884` | guarded | socket from create, null-checked (driver_builtin.cpp:390/:851; driver.cpp:162/:207); static fn reached only from receive_packet / receive_packets |
| `sdk/rocketnet_platform.cpp:4885` | guarded | callers pass &from local (client_io.h:628, client_transport.cpp:3266) or &from[num_packets] inside a loop bounded by max_packets (platform.cpp:5298/:5304) |
| `sdk/rocketnet_platform.cpp:4886` | guarded | callers pass a buffer slot / scratch array or packet_data + n*stride inside the bounded loop (platform.cpp:5304) |
| `sdk/rocketnet_platform.cpp:4887` | guarded | callers pass compile-time constants ROCKETNET_RECEIVE_PACKET_BYTES (client_io.h:629, client_transport.cpp:3266) or stride ROCKETNET_RECEIVE_CEILING / ROCKETNET_FRAME_SIZE (driver_builtin.cpp:295, client_io.h:578) |
| `sdk/rocketnet_platform.cpp:4988` | out of class | assert(0) on an unknown family; followed by return -1 |
| `sdk/rocketnet_platform.cpp:4992` | guarded | loop breaks only with result = max_packet_size + 1 (platform.cpp:4931) or bytes_received >= 0 (:4951); error paths return -1 (:4948) |
| `sdk/rocketnet_platform.cpp:5118` | guarded | wake set at create which fails on NULL (iocp_windows.cpp:723, driver_builtin.cpp:408/:857); destroys guarded where NULL-possible (iocp_windows.cpp:575, driver_builtin.cpp:348/:350) |
| `sdk/rocketnet_platform.cpp:5136` | guarded | wake set at create which fails on NULL (iocp_windows.cpp:723, driver_builtin.cpp:408/:857) |
| `sdk/rocketnet_platform.cpp:5172` | guarded | wake set at create which fails on NULL (iocp_windows.cpp:723, driver_builtin.cpp:408/:857); NULL-possible sites guard (iocp_windows.cpp:548/:1126, driver_builtin.cpp:336/:337/:518) |
| `sdk/rocketnet_platform.cpp:5241` | guarded | every caller passes &to / &storage / &socket_address locals or a checked address pointer (platform.cpp:861/:1027/:1097/:1449/:2956/:3028/:4812/:4864) |
| `sdk/rocketnet_platform.cpp:5242` | out of class | address type is not an index/size/pointer; the copy is a fixed 4-byte union read |
| `sdk/rocketnet_platform.cpp:5243` | guarded | every caller passes the address of a local sockaddr_in or sockaddr_storage (platform.cpp:860-861/:1027/:1096-1097/:1448-1449) |
| `sdk/rocketnet_platform.cpp:5252` | guarded | callers pass &sockaddr_from locals (platform.cpp:1215/:3190/:4984), a storage pointer, or result->ai_addr from a successful getaddrinfo (platform.cpp:683) |
| `sdk/rocketnet_platform.cpp:5253` | guarded | callers pass from (itself SAFE above), &parsed locals, or the caller's address after a successful getaddrinfo |
| `sdk/rocketnet_platform.cpp:5272` | out of class | the portable socket_pump never dereferences socket ((void) socket); test-only caller |
| `sdk/rocketnet_platform.cpp:5279` | guarded | socket from create, null-checked (driver_builtin.cpp:390/:851; driver.cpp:162/:207) |
| `sdk/rocketnet_platform.cpp:5280` | guarded | callers pass bank->sends (driver_builtin.cpp:195) or the client's stack sends[] (client_io.h:448); loop bounded by num_packets (platform.cpp:5282) |
| `sdk/rocketnet_platform.cpp:5290` | guarded | socket from create, null-checked (driver_builtin.cpp:390/:851; driver.cpp:162/:207) |
| `sdk/rocketnet_platform.cpp:5291` | guarded | callers pass bank->from sized rx_capacity (driver_builtin.cpp:294) or stack from[] (client_io.h:569); writes bounded by num_packets < max_packets (platform.cpp:5298) |
| `sdk/rocketnet_platform.cpp:5292` | guarded | callers pass &bank->data[0][0] (driver_builtin.cpp:295) or a buffer slot (client_io.h:572) |
| `sdk/rocketnet_platform.cpp:5293` | guarded | stride is a compile-time constant: ROCKETNET_RECEIVE_CEILING (driver_builtin.cpp:295) or ROCKETNET_FRAME_SIZE (client_io.h:578) |
| `sdk/rocketnet_platform.cpp:5294` | guarded | callers pass bank->bytes (driver_builtin.cpp:295) or stack packet_bytes[] (client_io.h:570) |
| `sdk/rocketnet_platform.cpp:5295` | guarded | server rx_capacity > 0 (driver_builtin.cpp:31-32); client room > 0 (client_io.h:564-567); loop bound num_packets < max_packets (platform.cpp:5298) covers <= 0 |
| `sdk/rocketnet_platform.cpp:5340` | out of class | asserts the pointer is NULL; nothing is dereferenced |
| `sdk/rocketnet_allocator.cpp:96` | guarded | layout is always the address of a caller's local (rocketnet_message_connection_bytes and rn_arena_plan pass &layout); never NULL |
| `sdk/rocketnet_allocator.cpp:198` | guarded | every sdk caller passes an in-range class: loop c < RN_STORE_NUM_CLASSES (allocator.cpp:123, :229) or the constant RN_STORE_LARGE (config.cpp:268) |
| `sdk/rocketnet_allocator.cpp:223` | out of class | rn_send_store_init has no sdk caller (only tests/test_channels.cpp, tests/test_unit_message_objects.cpp); the region path uses rn_send_store_init_counts |
| `sdk/rocketnet_allocator.cpp:224` | out of class | test-only entry (rn_send_store_init, no sdk caller); region path passes base + layout offset (allocator.cpp:463) |
| `sdk/rocketnet_allocator.cpp:232` | out of class | test-only entry (rn_send_store_init, no sdk caller); the region path's counts are bounded by the real check count >= 0x3FFFFFFF refusal at allocator.cpp:126 |
| `sdk/rocketnet_allocator.cpp:264` | guarded | store is &region->store, an embedded member (callers allocator.cpp:278, :633); never NULL |
| `sdk/rocketnet_allocator.cpp:274` | guarded | store is &region->store, an embedded member (caller allocator.cpp:681); never NULL |
| `sdk/rocketnet_allocator.cpp:296` | guarded | free_head only ever holds an index pushed by free_checked after the real i >= cls->fresh refusal (allocator.cpp:334) or by free_fragment on an alloc-issued chain; never wire data |
| `sdk/rocketnet_allocator.cpp:297` | out of class | checks an in-use flag (free-list invariant), not an index/size |
| `sdk/rocketnet_allocator.cpp:303` | guarded | fresh++ cannot pass num_chunks: rn_send_store_fits (allocator.cpp:283, free_count >= needed at :268) refuses first, and free_count counts untaken chunks plus the free list |
| `sdk/rocketnet_allocator.cpp:326` | guarded | store is &region->store (callers rn_send_store_free from allocator.cpp:948, :1017); never NULL |
| `sdk/rocketnet_allocator.cpp:344` | guarded | head index refused at allocator.cpp:334 (i >= fresh) and :339 (not an in-use head); later links were written only by rn_send_store_alloc with indices < fresh |
| `sdk/rocketnet_allocator.cpp:360` | out of class | asserts a boolean result (freed), not an index/size |
| `sdk/rocketnet_allocator.cpp:366` | guarded | store is &region->store (rn_region_send_data/write_pointer); and the index is refused by real checks at allocator.cpp:373 (class) and :378 (i >= fresh) |
| `sdk/rocketnet_allocator.cpp:385` | guarded | store pointer is an embedded member; index refused by real checks at allocator.cpp:391 and :396 |
| `sdk/rocketnet_allocator.cpp:404` | out of class | accessor with no sdk caller (only tests/test_channels.cpp, test_unit_message_objects.cpp pass RN_STORE_* constants) |
| `sdk/rocketnet_allocator.cpp:410` | out of class | accessor with no sdk caller (only tests pass RN_STORE_* constants) |
| `sdk/rocketnet_allocator.cpp:470` | guarded | region is &arena->regions[i] for i < num_regions (rn_arena_region range check allocator.cpp:1348; sole caller message_server.cpp:307 after the arena NULL check :289); base set by rn_region_point (:421) from the checked block |
| `sdk/rocketnet_allocator.cpp:563` | guarded | region is &arena->regions[i] (rn_arena_region range check allocator.cpp:1348; arena checked message_server.cpp:289) |
| `sdk/rocketnet_allocator.cpp:573` | guarded | region is &arena->regions[i] (allocator.cpp:1348; message_server.cpp:289) |
| `sdk/rocketnet_allocator.cpp:579` | guarded | region is &arena->regions[i] (allocator.cpp:1348; message_server.cpp:289) |
| `sdk/rocketnet_allocator.cpp:588` | guarded | region is &arena->regions[i] (allocator.cpp:1348; message_server.cpp:289) |
| `sdk/rocketnet_allocator.cpp:597` | guarded | both callers (:684, :869) run after the real descriptors_free == 0 refusal in rn_region_send_fit_store (allocator.cpp:630) / rn_region_send_fit_span (:828) |
| `sdk/rocketnet_allocator.cpp:602` | guarded | descriptors_free_head only holds d pushed by send_free/free_fragment after the real d >= descriptors_fresh refusals (allocator.cpp:939, :961) |
| `sdk/rocketnet_allocator.cpp:603` | out of class | checks an in-use flag, not an index/size |
| `sdk/rocketnet_allocator.cpp:609` | guarded | descriptors_free (queue - fresh + free list) > 0 is refused by real checks at allocator.cpp:630/:828, so fresh++ stays < queue |
| `sdk/rocketnet_allocator.cpp:619` | guarded | region is &arena->regions[i] (allocator.cpp:1348; message_server.cpp:289) |
| `sdk/rocketnet_allocator.cpp:672` | guarded | region is &arena->regions[i] (allocator.cpp:1348; message_server.cpp:289) |
| `sdk/rocketnet_allocator.cpp:682` | guarded | rn_region_send_fit_store (allocator.cpp:674) already required rn_send_store_fits (:633), so the alloc cannot return RN_CHUNK_NONE |
| `sdk/rocketnet_allocator.cpp:782` | out of class | checks a flag; and a real if at allocator.cpp:783 returns when it is clear |
| `sdk/rocketnet_allocator.cpp:816` | guarded | region is &arena->regions[i] (allocator.cpp:1348; message_server.cpp:289) |
| `sdk/rocketnet_allocator.cpp:858` | guarded | region is &arena->regions[i] (allocator.cpp:1348; message_server.cpp:289) |
| `sdk/rocketnet_allocator.cpp:884` | guarded | region is &arena->regions[i] (allocator.cpp:1348); d refused by real check at allocator.cpp:887 |
| `sdk/rocketnet_allocator.cpp:920` | guarded | region is &arena->regions[i] (allocator.cpp:1348); d refused by real check at allocator.cpp:921 |
| `sdk/rocketnet_allocator.cpp:937` | guarded | region is &arena->regions[i] (allocator.cpp:1348; message_server.cpp:289) |
| `sdk/rocketnet_allocator.cpp:938` | guarded | real refusal right after: if ( d >= region->descriptors_fresh ) return (allocator.cpp:939) |
| `sdk/rocketnet_allocator.cpp:942` | out of class | checks an in-use flag; real if at allocator.cpp:943 returns anyway |
| `sdk/rocketnet_allocator.cpp:959` | guarded | region is &arena->regions[i] (allocator.cpp:1348; message_server.cpp:289) |
| `sdk/rocketnet_allocator.cpp:960` | guarded | real refusal right after: if ( d >= region->descriptors_fresh ) return (allocator.cpp:961) |
| `sdk/rocketnet_allocator.cpp:964` | out of class | checks an in-use flag; real if at allocator.cpp:965 returns anyway |
| `sdk/rocketnet_allocator.cpp:971` | guarded | chunk != RN_CHUNK_NONE checked at allocator.cpp:968; descriptor->chunk is written only from rn_send_store_alloc (class c < 3, :309) or (c << 30) \| next with the same c (:997) |
| `sdk/rocketnet_allocator.cpp:974` | guarded | head comes from a handle rn_send_store_alloc issued (index < fresh <= num_chunks, :296/:303) or relinked at :997 from an in-chain link |
| `sdk/rocketnet_allocator.cpp:975` | out of class | checks link flags (head/in-use), not an index/size |
| `sdk/rocketnet_allocator.cpp:986` | guarded | walk bounded by cur != RN_LINK_END (allocator.cpp:981, :984) over links written only by rn_send_store_alloc |
| `sdk/rocketnet_allocator.cpp:1048` | guarded | region from the arena (allocator.cpp:1348); room is always &room, a caller's local (allocator.cpp:1107, :1137) |
| `sdk/rocketnet_allocator.cpp:1077` | guarded | region is &arena->regions[i] (allocator.cpp:1348); store class from rn_send_store_class_for after the real bytes range check at :1080 |
| `sdk/rocketnet_allocator.cpp:1105` | guarded | region is &arena->regions[i] (allocator.cpp:1348; message_server.cpp:289) |
| `sdk/rocketnet_allocator.cpp:1171` | guarded | region is &arena->regions[i] (allocator.cpp:1348); bounds are real checks at :1174-1176 |
| `sdk/rocketnet_allocator.cpp:1184` | guarded | region is &arena->regions[i] (allocator.cpp:1348); offset/bytes refused by real checks at :1185-1187 |
| `sdk/rocketnet_allocator.cpp:1341` | guarded | arena NULL-checked at create (message_server.cpp:289) before any accessor |
| `sdk/rocketnet_allocator.cpp:1347` | guarded | arena NULL-checked at create (message_server.cpp:289); index refused by the real check at allocator.cpp:1348 |
| `sdk/rocketnet_allocator.cpp:1355` | guarded | arena NULL-checked at create (message_server.cpp:289) |
| `sdk/rocketnet_client_gso_linux.cpp:83` | guarded | gso is &client->gso, an embedded member (client_gso_linux.cpp:431) |
| `sdk/rocketnet_client_gso_linux.cpp:84` | guarded | syscalls is &rocketnet_client_gso_kernel_syscalls, a static table (client_gso_linux.cpp:431) |
| `sdk/rocketnet_client_gso_linux.cpp:106` | guarded | gso is &client->gso (client_gso_linux.cpp:438) |
| `sdk/rocketnet_client_gso_linux.cpp:123` | guarded | rocketnet_coalesce_send_run returns >= 1 when count > 0 and packet_bytes[0] > 0 (coalesce.h:68); count = num_packets - packet > 0 by the loop (:117) and every packet_bytes is > 0: client_io.h:458 skips bytes == 0 and the ring's nonzero byte counts are sealed sizes written after a wire_bytes == 0 refusal (client_transport.cpp:2719/2725, :2777/2784, :2817/2823) |
| `sdk/rocketnet_client_gso_linux.cpp:162` | guarded | to is always &io->to[ring] (client_io.h:460) |
| `sdk/rocketnet_client_gso_linux.cpp:163` | guarded | packet_data is view->data + slot offset into the ring's frame array (client_io.h:461; views set client_transport.cpp:1762) |
| `sdk/rocketnet_client_gso_linux.cpp:164` | guarded | bytes == 0 skipped at client_io.h:458; nonzero values are sealed sizes from the frame (client_transport.cpp:2725, :2784, :2823), far below UINT16_MAX |
| `sdk/rocketnet_client_gso_linux.cpp:234` | guarded | gso is &client->gso (client_gso_linux.cpp:481) |
| `sdk/rocketnet_client_gso_linux.cpp:235` | guarded | sends is the caller's local array (client_io.h:452) |
| `sdk/rocketnet_client_gso_linux.cpp:252` | guarded | header is &gso->receive_message.msg_hdr (client_gso_linux.cpp:308, :346) |
| `sdk/rocketnet_client_gso_linux.cpp:253` | guarded | faults is &gso->gro_cmsg_faults (client_gso_linux.cpp:346) |
| `sdk/rocketnet_client_gso_linux.cpp:294` | guarded | gso is &client->gso (client_gso_linux.cpp:494) |
| `sdk/rocketnet_client_gso_linux.cpp:295` | guarded | from is the caller's local array (client_io.h:573) |
| `sdk/rocketnet_client_gso_linux.cpp:296` | guarded | packet_data is buffer->data + fill_slots offset (client_io.h:576) |
| `sdk/rocketnet_client_gso_linux.cpp:297` | guarded | packet_stride is the constant ROCKETNET_FRAME_SIZE (client_io.h:578) |
| `sdk/rocketnet_client_gso_linux.cpp:298` | guarded | packet_bytes is the caller's local array (client_io.h:574) |
| `sdk/rocketnet_client_gso_linux.cpp:299` | guarded | loop guard fill_slots < ROCKETNET_NUM_CLIENT_PACKETS (client_io.h:567) makes room >= 1, so max_packets >= 1 (:571-572) |
| `sdk/rocketnet_frame_sync.cpp:307` | out of class | time value (ewma), not an index/size/divisor |
| `sdk/rocketnet_frame_sync.cpp:318` | guarded | always &frame_sync->guard_ewma_ns (frame_sync.cpp:487) |
| `sdk/rocketnet_frame_sync.cpp:329` | guarded | always &frame_sync->deadline_ns (frame_sync.cpp:498) |
| `sdk/rocketnet_frame_sync.cpp:330` | guarded | always &frame_sync->wakeup_error_ns (frame_sync.cpp:498) |
| `sdk/rocketnet_frame_sync.cpp:331` | guarded | divisor period_ns = 1e9 / frame_rate with frame_rate refused outside (0, 1000] at create (frame_sync.cpp:370), so period_ns >= 1e6 |
| `sdk/rocketnet_frame_sync.cpp:442` | guarded | public-API null guard rocketnet_api_null_guard( frame_sync ) at frame_sync.cpp:440 |
| `sdk/rocketnet_frame_sync.cpp:460` | guarded | public-API null guard at frame_sync.cpp:458 |
| `sdk/rocketnet_frame_sync.cpp:514` | guarded | public-API null guard at frame_sync.cpp:510 |
| `sdk/rocketnet_frame_sync.cpp:515` | guarded | public-API null guard at frame_sync.cpp:512 |
| `sdk/rocketnet_gateway_netlink_linux.cpp:87` | guarded | data is the dump's local buffer (gateway_netlink_linux.cpp:403, :439) |
| `sdk/rocketnet_gateway_netlink_linux.cpp:88` | guarded | error is &error / the caller's *error, always an address (gateway_netlink_linux.cpp:439; server_socket_linux.cpp:371, :387) |
| `sdk/rocketnet_gateway_netlink_linux.cpp:201` | guarded | body_bytes is the constant sizeof(struct rtmsg) passed at gateway_netlink_linux.cpp:248; payload length checked against it by the real check at :143 |
| `sdk/rocketnet_gateway_netlink_linux.cpp:243` | guarded | sole caller derives family from a ternary AF_INET6 : AF_INET (server_socket_linux.cpp:365) |
| `sdk/rocketnet_gateway_netlink_linux.cpp:244` | out of class | interface_index is only compared with kernel-reported values, never used as an index/size |
| `sdk/rocketnet_gateway_netlink_linux.cpp:245` | guarded | gateway is the caller's local uint8_t[16] (server_socket_linux.cpp:368) |
| `sdk/rocketnet_gateway_netlink_linux.cpp:271` | guarded | body_bytes is the constant sizeof(struct ndmsg) passed at gateway_netlink_linux.cpp:303; payload length checked at :143 |
| `sdk/rocketnet_gateway_netlink_linux.cpp:298` | guarded | family from a ternary AF_INET6 : AF_INET (server_socket_linux.cpp:365) |
| `sdk/rocketnet_gateway_netlink_linux.cpp:299` | out of class | interface_index only compared with ndm_ifindex, never an index/size |
| `sdk/rocketnet_gateway_netlink_linux.cpp:300` | guarded | gateway is the caller's local uint8_t[16] (server_socket_linux.cpp:368) |
| `sdk/rocketnet_gateway_netlink_linux.cpp:301` | guarded | mac is the caller's mac_address out buffer, the same one the env branch writes (server_socket_linux.cpp:451-456, :461) |
| `sdk/rocketnet_gateway_netlink_linux.cpp:468` | guarded | error is &error, a caller local (server_socket_linux.cpp:371) |
| `sdk/rocketnet_gateway_netlink_linux.cpp:489` | guarded | error is &error, a caller local (server_socket_linux.cpp:387) |
| `sdk/rocketnet_server_gso_linux.cpp:251` | out of class | test hook (inside #if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)) |
| `sdk/rocketnet_server_gso_linux.cpp:258` | out of class | test hook (ROCKETNET_SEND_ERROR_TEST_HOOKS) |
| `sdk/rocketnet_server_gso_linux.cpp:270` | out of class | test hook (ROCKETNET_SEND_ERROR_TEST_HOOKS) |
| `sdk/rocketnet_server_gso_linux.cpp:281` | out of class | test hook (ROCKETNET_SEND_ERROR_TEST_HOOKS) |
| `sdk/rocketnet_server_gso_linux.cpp:282` | out of class | test hook (ROCKETNET_SEND_ERROR_TEST_HOOKS) |
| `sdk/rocketnet_server_gso_linux.cpp:313` | out of class | invariant over a packet count (bank reset), not an index/size used afterwards |
| `sdk/rocketnet_server_gso_linux.cpp:660` | guarded | thread argument is &gso->queues[i] set at create |
| `sdk/rocketnet_server_gso_linux.cpp:866` | guarded | bind is &attempt, a local in rocketnet_server_transport_io_open, which also refuses a NULL bind (driver.cpp:152, :160) |
| `sdk/rocketnet_server_gso_linux.cpp:867` | out of class | address type selects the socket family; not an index/size/divisor |
| `sdk/rocketnet_server_gso_linux.cpp:1040` | guarded | gso is the transport instance; rocketnet_server_transport_io_open refuses a NULL create result (driver.cpp:161-166) |
| `sdk/rocketnet_server_gso_linux.cpp:1063` | guarded | transport instance non-NULL (driver.cpp:161-166); io_close also tests instance (driver.cpp:174) |
| `sdk/rocketnet_server_gso_linux.cpp:1065` | out of class | asserts a boolean (stopped); real if at :1066 returns |
| `sdk/rocketnet_server_gso_linux.cpp:1112` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_gso_linux.cpp:1118` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_gso_linux.cpp:1124` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_gso_linux.cpp:1125` | guarded | counters is the caller's local array (driver_builtin.cpp:995-996) |
| `sdk/rocketnet_server_gso_linux.cpp:1148` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_gso_linux.cpp:1149` | guarded | too_short is &too_short, a caller local (server_transport.cpp:3378) |
| `sdk/rocketnet_server_gso_linux.cpp:1150` | guarded | too_large is &too_large, a caller local (server_transport.cpp:3378) |
| `sdk/rocketnet_server_gso_linux.cpp:1161` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_gso_linux.cpp:1162` | guarded | every caller passes &to of a local (server_transport.cpp:750, :2602) or a to it has already dereferenced (frame->to = *to at server_transport.cpp:812, :2735) |
| `sdk/rocketnet_server_gso_linux.cpp:1163` | guarded | every caller passes &id / &frame->packet_id / &frame->socket_packet_id (server_transport.cpp:750, :816, :2602, :2739) |
| `sdk/rocketnet_server_gso_linux.cpp:1192` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_gso_linux.cpp:1193` | guarded | packet_data is only compared by address; the real range check at server_gso_linux.cpp:1205 and slot check :1211 refuse any non-slot pointer |
| `sdk/rocketnet_server_gso_linux.cpp:1194` | guarded | callers pass sealed wire bytes after the payload check payload_bytes < 1 \|\| > payload_capacity (server_transport.cpp:2945) and seal-failure refusal (:2966), or a simulator datagram > 0 (:745) |
| `sdk/rocketnet_server_gso_linux.cpp:1195` | guarded | payload bounded by payload_capacity (server_transport.cpp:2945) before the seal adds fixed overhead; the simulator path copies at most ROCKETNET_RECEIVE_CEILING (server_transport.cpp:740) |
| `sdk/rocketnet_server_gso_linux.cpp:1219` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_gso_linux.cpp:1447` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_gso_linux.cpp:1508` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_gso_linux.cpp:1522` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_rio_windows.cpp:514` | out of class | asserts a boolean (stopped), not an index/size |
| `sdk/rocketnet_server_rio_windows.cpp:727` | guarded | real refusal right after: index < 0 \|\| packet_bytes <= 0 \|\| packet_bytes > ROCKETNET_RECEIVE_CEILING returns (server_rio_windows.cpp:730) |
| `sdk/rocketnet_server_rio_windows.cpp:882` | guarded | structural bound: slice is range-checked 0..ROCKETNET_RIO_RECEIVES-1 (server_rio_windows.cpp:836), and a slice enters retry_slices only after its own completion with its re-post refused, so it is not posted (cannot complete again) until rocketnet_rio_retry_receives removes it; at most ROCKETNET_RIO_RECEIVES distinct entries (retry_slices sized :145) |
| `sdk/rocketnet_server_rio_windows.cpp:1418` | out of class | test hook (inside #if defined(ROCKETNET_RIO_TESTS)) |
| `sdk/rocketnet_server_uring_linux.cpp:979` | out of class | test hook (ROCKETNET_SEND_ERROR_TEST_HOOKS) |
| `sdk/rocketnet_server_uring_linux.cpp:980` | out of class | test hook (ROCKETNET_SEND_ERROR_TEST_HOOKS) |
| `sdk/rocketnet_server_uring_linux.cpp:981` | out of class | test hook (ROCKETNET_SEND_ERROR_TEST_HOOKS) |
| `sdk/rocketnet_server_uring_linux.cpp:994` | out of class | test hook (ROCKETNET_SEND_ERROR_TEST_HOOKS) |
| `sdk/rocketnet_server_uring_linux.cpp:1002` | out of class | test hook (ROCKETNET_SEND_ERROR_TEST_HOOKS) |
| `sdk/rocketnet_server_uring_linux.cpp:1012` | out of class | test seam (ROCKETNET_TEST_SEAMS) |
| `sdk/rocketnet_server_uring_linux.cpp:1018` | out of class | test seam (ROCKETNET_TEST_SEAMS) |
| `sdk/rocketnet_server_uring_linux.cpp:1085` | out of class | test-only fake transport (ROCKETNET_URING_REAP_TEST_HOOKS) |
| `sdk/rocketnet_server_uring_linux.cpp:1177` | guarded | bind is &attempt, a local in rocketnet_server_transport_io_open, which refuses a NULL bind (driver.cpp:152, :160) |
| `sdk/rocketnet_server_uring_linux.cpp:1178` | out of class | address type selects the socket family; not an index/size/divisor |
| `sdk/rocketnet_server_uring_linux.cpp:1360` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_uring_linux.cpp:1398` | guarded | transport instance non-NULL (driver.cpp:161-166); io_close tests instance (driver.cpp:174) |
| `sdk/rocketnet_server_uring_linux.cpp:1400` | out of class | asserts a boolean (stopped); real if at :1401 returns |
| `sdk/rocketnet_server_uring_linux.cpp:1444` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_uring_linux.cpp:1450` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_uring_linux.cpp:1456` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_uring_linux.cpp:1457` | guarded | counters is the caller's local array (driver_builtin.cpp:1031-1032) |
| `sdk/rocketnet_server_uring_linux.cpp:1467` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_uring_linux.cpp:1468` | guarded | &too_short, a caller local (server_transport.cpp:3378) |
| `sdk/rocketnet_server_uring_linux.cpp:1469` | guarded | &too_large, a caller local (server_transport.cpp:3378) |
| `sdk/rocketnet_server_uring_linux.cpp:1482` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_uring_linux.cpp:1483` | guarded | callers pass &to of a local (server_transport.cpp:750, :2602) or an already-dereferenced to (:812, :2735) |
| `sdk/rocketnet_server_uring_linux.cpp:1484` | guarded | callers pass &id / &frame->packet_id / &frame->socket_packet_id (server_transport.cpp:750, :816, :2602, :2739) |
| `sdk/rocketnet_server_uring_linux.cpp:1522` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_uring_linux.cpp:1523` | guarded | packet_data only compared by address; real slot check in rocketnet_server_uring_slot (server_uring_linux.cpp:1513) refuses at :1530 |
| `sdk/rocketnet_server_uring_linux.cpp:1524` | guarded | sealed wire bytes after payload check (server_transport.cpp:2945) and seal-failure refusal (:2966), or simulator datagram > 0 (:745); the sender also skips packet_bytes <= 0 (server_uring_linux.cpp:1613) |
| `sdk/rocketnet_server_uring_linux.cpp:1525` | guarded | payload bounded by payload_capacity (server_transport.cpp:2945) before sealing; simulator path bounded by its ROCKETNET_RECEIVE_CEILING buffer (server_transport.cpp:740) |
| `sdk/rocketnet_server_uring_linux.cpp:1540` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_uring_linux.cpp:1568` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_uring_linux.cpp:1655` | guarded | run >= 1 because ahead >= 1 (slot < num_slots at :1609) and the first packet's bytes > 0 (real skip at server_uring_linux.cpp:1613); rocketnet_coalesce_send_run then returns >= 1 (coalesce.h:68, uring_codec.h:236-238) |
| `sdk/rocketnet_server_uring_linux.cpp:1667` | guarded | SQE availability checked: space == 0 handled at server_uring_linux.cpp:1591 and prepared < space bounds the loop (:1609) |
| `sdk/rocketnet_server_uring_linux.cpp:1749` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_uring_linux.cpp:1789` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_iocp_windows.cpp:405` | guarded | real check free_count > 0 (server_iocp_windows.cpp:401) with fill + ready + free + exposed = ROCKETNET_IOCP_RECEIVE_BANKS, so ready_count <= BANKS - 2 here |
| `sdk/rocketnet_server_iocp_windows.cpp:568` | out of class | asserts a boolean (stopped), not an index/size |
| `sdk/rocketnet_server_iocp_windows.cpp:589` | out of class | invariant on an in-flight count at destroy, not an index/size |
| `sdk/rocketnet_server_iocp_windows.cpp:801` | out of class | frame is compared only (cast to void), never dereferenced or used as index |
| `sdk/rocketnet_server_iocp_windows.cpp:802` | guarded | callers pass sealed bytes after payload check (server_transport.cpp:2945) or a simulator datagram bounded by its ROCKETNET_RECEIVE_CEILING buffer (:740, :745) |
| `sdk/rocketnet_server_iocp_windows.cpp:810` | out of class | frame is compared only (cast to void), never dereferenced or used as index |
| `sdk/rocketnet_server_iocp_windows.cpp:1065` | guarded | structural: fill(1) + ready + free + exposed = ROCKETNET_IOCP_RECEIVE_BANKS under the worker mutex, so free_count < BANKS while exposed banks are returned |
| `sdk/rocketnet_server_iocp_windows.cpp:1176` | out of class | test hook (rocketnet_server_iocp_test_ready_banks) |
| `sdk/rocketnet_server_iocp_windows.cpp:1211` | out of class | test hook (rocketnet_server_iocp_test_fail_next) |
| `sdk/rocketnet_server_socket_linux.cpp:20` | guarded | sole caller (server_socket_linux.cpp:3003) passes num_consumed counted inside the loop over the num_packets peeked descriptors, so num_consumed <= num_packets by construction |
| `sdk/rocketnet_server_socket_linux.cpp:170` | guarded | recycle callers pass the index claim() returned (server_socket_linux.cpp:2819 after held_index >= 0 at :2813; :3153 after on_index >= 0 at :3061); ring of N with N-1 free slots plus one producer and the claimed one, so free_count < N |
| `sdk/rocketnet_server_socket_linux.cpp:714` | guarded | line is the caller's fgets buffer (server_socket_linux.cpp:983 loop) |
| `sdk/rocketnet_server_socket_linux.cpp:715` | guarded | interface_name is the caller's resolved name (server_socket_linux.cpp:983) |
| `sdk/rocketnet_server_socket_linux.cpp:817` | guarded | public_address_string null-guarded at the public API (server_transport.cpp:1134) and parsed (:1274) before the transport create |
| `sdk/rocketnet_server_socket_linux.cpp:818` | guarded | rocketnet_server_socket_memory_required returns 0 for num_queues < 1 or > ROCKETNET_XDP_MAX_QUEUES (server_socket_linux.cpp:785) and preflight refuses 0 (:822); xdp_create also defaults <= 0 to ROCKETNET_XDP_DEFAULT_QUEUES (driver_builtin.cpp:1049) |
| `sdk/rocketnet_server_socket_linux.cpp:1189` | guarded | xdp_create defaults num_queues <= 0 (driver_builtin.cpp:1049) and preflight refuses out-of-range counts (server_socket_linux.cpp:1276 via :785) |
| `sdk/rocketnet_server_socket_linux.cpp:1190` | guarded | public-API null guard on public_address_string (server_transport.cpp:1134) and parse (:1274) |
| `sdk/rocketnet_server_socket_linux.cpp:1191` | guarded | packet_filter is config->transport->packet_filter, an embedded array (rocketnet.h:1080; driver_builtin.cpp:1056) |
| `sdk/rocketnet_server_socket_linux.cpp:1806` | guarded | compile-time constants (ROCKETNET_XDP_* defines) |
| `sdk/rocketnet_server_socket_linux.cpp:1807` | guarded | compile-time constants (ROCKETNET_XDP_* defines) |
| `sdk/rocketnet_server_socket_linux.cpp:1923` | guarded | transport instance non-NULL (driver.cpp:161-166); io_close tests instance (driver.cpp:174) |
| `sdk/rocketnet_server_socket_linux.cpp:1925` | out of class | asserts a boolean (stopped); real if at :1926 returns |
| `sdk/rocketnet_server_socket_linux.cpp:2018` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_socket_linux.cpp:2025` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_socket_linux.cpp:2043` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_socket_linux.cpp:2071` | guarded | sole caller passes the start_packet's to, already dereferenced by rocketnet_xdp_reply_mac (server_socket_linux.cpp:2142) before :2152 and by the transport callers (server_transport.cpp:812, :2735) |
| `sdk/rocketnet_server_socket_linux.cpp:2072` | guarded | divisor num_queues is server_socket->num_queues (server_socket_linux.cpp:2152), set at :1346 only after preflight refused counts outside 1..ROCKETNET_XDP_MAX_QUEUES (:1276 via :785) |
| `sdk/rocketnet_server_socket_linux.cpp:2123` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_socket_linux.cpp:2124` | guarded | callers pass &to of a local (server_transport.cpp:750, :2602) or an already-dereferenced to (:812, :2735) |
| `sdk/rocketnet_server_socket_linux.cpp:2125` | guarded | callers pass &id / &frame->packet_id / &frame->socket_packet_id (server_transport.cpp:750, :816, :2602, :2739) |
| `sdk/rocketnet_server_socket_linux.cpp:2164` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_socket_linux.cpp:2165` | guarded | packet_data only used in pointer arithmetic; real slot refusal at server_socket_linux.cpp:2187 (packet_index < 0 \|\| >= ROCKETNET_XDP_SEND_QUEUE_SIZE) |
| `sdk/rocketnet_server_socket_linux.cpp:2166` | guarded | sealed bytes after payload check (server_transport.cpp:2945) and seal refusal (:2966), or a simulator datagram > 0 (:745) |
| `sdk/rocketnet_server_socket_linux.cpp:2167` | guarded | payload bounded by payload_capacity (server_transport.cpp:2945) before sealing; simulator path bounded by its ROCKETNET_RECEIVE_CEILING buffer (:740) |
| `sdk/rocketnet_server_socket_linux.cpp:2220` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_socket_linux.cpp:2281` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_socket_linux.cpp:2282` | guarded | sink passes &slot->marked_address, an embedded member (server_replay_marks.cpp:114) |
| `sdk/rocketnet_server_socket_linux.cpp:2283` | guarded | replay_marks NULL-checked at create (server_socket_linux.cpp:1291), create fails |
| `sdk/rocketnet_server_socket_linux.cpp:2290` | guarded | transport instance non-NULL (driver.cpp:161-166); view_index refused by the real check at :2291 |
| `sdk/rocketnet_server_socket_linux.cpp:2298` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_socket_linux.cpp:2299` | guarded | sink passes rocketnet_encryption_map_address for a mapping_index refused < 0 (server_replay_marks.cpp:128), and the same address is dereferenced at :145 |
| `sdk/rocketnet_server_socket_linux.cpp:2300` | guarded | replay_marks NULL-checked at create (server_socket_linux.cpp:1291) |
| `sdk/rocketnet_server_socket_linux.cpp:2312` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_socket_linux.cpp:2313` | guarded | replay_marks NULL-checked at create (server_socket_linux.cpp:1291) |
| `sdk/rocketnet_server_socket_linux.cpp:2375` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_socket_linux.cpp:2689` | guarded | the CPU_SET index is bounded by the real check rocketnet_xdp_cpu_in_set( cpu, CPU_SETSIZE ) at server_socket_linux.cpp:2682; a cpu past the online count only makes pthread_setaffinity_np fail, which is handled (:2698-2703) |
| `sdk/rocketnet_server_socket_linux.cpp:2709` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_socket_linux.cpp:2715` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_socket_linux.cpp:2726` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_socket_linux.cpp:2767` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_socket_linux.cpp:2768` | guarded | counters is the caller's local array (driver_builtin.cpp:1073) |
| `sdk/rocketnet_server_socket_linux.cpp:2802` | guarded | transport instance non-NULL (driver.cpp:161-166) |
| `sdk/rocketnet_server_socket_linux.cpp:2844` | guarded | only rocketnet_xdp_frame_receive fills packet_bytes, and it refuses payload_bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES (xdp_frame.h:389) |
| `sdk/rocketnet_server_socket_linux.cpp:2855` | guarded | thread argument is &server_socket->socket[i] set at create |
| `sdk/rocketnet_server_socket_linux.cpp:3011` | out of class | asserts a boolean (retained); real if at :3012 handles failure |
| `sdk/rocketnet_server_socket_linux.cpp:3073` | guarded | num_staged_packets is only a loop bound: a negative value makes the loop at :3083 empty (no index taken); the upper clamp is the real check at :3069-3071 |
| `sdk/rocketnet_server_socket_linux.cpp:3074` | guarded | real clamp to ROCKETNET_XDP_SEND_QUEUE_SIZE at server_socket_linux.cpp:3068-3071 |
| `sdk/rocketnet_server_socket_linux.cpp:3080` | guarded | packet_start_index is only reset to 0 (:3151) or incremented inside the loop bounded by i < num_staged_packets <= SIZE (:3083), so it is never negative |
| `sdk/rocketnet_server_socket_linux.cpp:3081` | guarded | packet_start_index advances only inside the loop bounded by num_staged_packets, which is clamped to ROCKETNET_XDP_SEND_QUEUE_SIZE (:3069-3072) |
| `sdk/rocketnet_client_io.h:328` | guarded | opened_bytes = packet_bytes - header - tag by construction, and rocketnet_open_sealed_packet refuses packet_bytes below the header+tag and plaintext above out_capacity (protocol.cpp:316, :321); the memcpy target is inside the same packet's bytes |
| `sdk/rocketnet_client_io.h:374` | out of class | asserts a state flag (quitting); real if at client_io.h:375 returns |
| `sdk/rocketnet_client_io.h:505` | guarded | every caller passes &client_transport->client_io (client_transport.cpp:2373, :2420, :2492, :2867) or the I/O thread's argument (client_io.h:705) |
| `sdk/rocketnet_server_protocol.cpp:272` | guarded | server: only production caller is process_received (server_protocol.cpp:1441) forwarding &server_transport->protocol from server_transport.cpp:2130/2287 (address of embedded field, never null) |
| `sdk/rocketnet_server_protocol.cpp:273` | guarded | from: forwarded from server_transport.cpp:2130 (&received->from[i]) / :2287 (&from, a local); never null |
| `sdk/rocketnet_server_protocol.cpp:274` | guarded | packet_data: forwarded from the receive batch at server_transport.cpp:2130/2287; packet_bytes also checked exactly at server_protocol.cpp:290 |
| `sdk/rocketnet_server_protocol.cpp:275` | guarded | response_packet: callers pass `response ? response : response_overflow` (stack buffer) at server_transport.cpp:2133/2290; never null |
| `sdk/rocketnet_server_protocol.cpp:276` | guarded | response_bytes: &response_bytes local at server_transport.cpp:2133/2290 |
| `sdk/rocketnet_server_protocol.cpp:277` | guarded | encryption_map: create fails when NULL (server_transport.cpp:1801) |
| `sdk/rocketnet_server_protocol.cpp:278` | guarded | used_token_history: create fails when NULL (server_transport.cpp:1801) |
| `sdk/rocketnet_server_protocol.cpp:279` | guarded | client_slots: create fails when NULL (server_transport.cpp:1801) |
| `sdk/rocketnet_server_protocol.cpp:280` | guarded | counters = &server_transport->counters (server_transport.cpp:1790), address of embedded struct |
| `sdk/rocketnet_server_protocol.cpp:497` | out of class | assert(0) in a failure branch that really refuses (counts INTERNAL_ERROR, returns IGNORED); guards no index/size/pointer |
| `sdk/rocketnet_server_protocol.cpp:594` | out of class | assert(0) in a failure branch that really refuses (returns IGNORED) |
| `sdk/rocketnet_server_protocol.cpp:696` | out of class | assert(0) in a failure branch that really refuses (returns IGNORED) |
| `sdk/rocketnet_server_protocol.cpp:772` | out of class | assert(0) in a failure branch that removes the mapping and returns IGNORED |
| `sdk/rocketnet_server_protocol.cpp:861` | out of class | assert(0) in a failure branch that counts and returns IGNORED |
| `sdk/rocketnet_server_protocol.cpp:874` | guarded | client_index used as index after assign; real check `if ( rocketnet_client_slots_num_free(...) <= 0 )` returns SEND_DENIED at server_protocol.cpp:849, and assign returns -1 only when free_stack_count==0 (client_slots.cpp:197) |
| `sdk/rocketnet_server_protocol.cpp:892` | out of class | assert(0) in a failure branch with real handling (returns ACCEPTED, no use of bytes) |
| `sdk/rocketnet_server_protocol.cpp:972` | guarded | no production caller of rocketnet_server_payload_copy (tests only, stack buffers); copy size bounded by real check `type_data_bytes > out_payload_capacity` at server_protocol.cpp:976 |
| `sdk/rocketnet_server_protocol.cpp:973` | guarded | same: test-only callers; size bounded at server_protocol.cpp:976 |
| `sdk/rocketnet_server_protocol.cpp:974` | guarded | same: test-only callers pass a local |
| `sdk/rocketnet_server_protocol.cpp:1336` | out of class | assert(0) in the !server->reassembly branch (server_protocol.cpp:1331) which really returns; reassembly also required non-NULL at create (server_transport.cpp:1801) |
| `sdk/rocketnet_server_protocol.cpp:1396` | guarded | server = &server_transport->protocol at server_transport.cpp:2130/2287 (and test callers) |
| `sdk/rocketnet_server_protocol.cpp:1397` | guarded | from = &received->from[i] / &from local (server_transport.cpp:2130/2287) |
| `sdk/rocketnet_server_protocol.cpp:1398` | guarded | packet_data from the transport receive batch / receive_frame (server_transport.cpp:2131/2288); length checked by prefilter (server_protocol.cpp:1422) |
| `sdk/rocketnet_server_protocol.cpp:1399` | guarded | response_packet is `response ? response : response_overflow` (server_transport.cpp:2133/2290) |
| `sdk/rocketnet_server_protocol.cpp:1400` | guarded | &response_bytes local (server_transport.cpp:2133/2290) |
| `sdk/rocketnet_server_protocol.cpp:1401` | guarded | opened is payload slot or stack payload_overflow (server_transport.cpp:2115 / 2272) |
| `sdk/rocketnet_server_protocol.cpp:1402` | guarded | &payload local (server_transport.cpp:2135/2292) |
| `sdk/rocketnet_server_protocol.cpp:1403` | guarded | &payload_bytes local (server_transport.cpp:2135/2292) |
| `sdk/rocketnet_server_protocol.cpp:1404` | guarded | &payload_client_index local (server_transport.cpp:2135/2292) |
| `sdk/rocketnet_server_protocol.cpp:1480` | guarded | process_packet_capacity has no production caller (tests and tools/differential only, stack buffers); memcpy guarded by `if ( payload && *out_payload_bytes > 0 )` at server_protocol.cpp:1488 with size bounded by out_payload_capacity check (server_protocol.cpp:1304) |
| `sdk/rocketnet_server_protocol.cpp:1523` | guarded | callers pass a server already in use: server_protocol.cpp:429 and :1561 (both from &server_transport->protocol) |
| `sdk/rocketnet_server_protocol.cpp:1556` | guarded | only production caller server_transport.cpp:2359 passes &server_transport->protocol |
| `sdk/rocketnet_server_protocol.cpp:1557` | guarded | encryption_map non-NULL enforced at create (server_transport.cpp:1801) |
| `sdk/rocketnet_server_protocol.cpp:1558` | guarded | client_slots non-NULL enforced at create (server_transport.cpp:1801) |
| `sdk/rocketnet_server_protocol.cpp:1559` | guarded | counters = &server_transport->counters (server_transport.cpp:1790) |
| `sdk/rocketnet_server_protocol.cpp:1666` | guarded | type_data_bytes sizes the memcpy into plaintext[ACK_BLOCK_MAX+8]; static function, all three call sites pass compile-time constants 8 (server_protocol.cpp:1733-1735) or 0 (:1786, :2235) |
| `sdk/rocketnet_server_protocol.cpp:1689` | guarded | bytes bounded by the block_capacity clamp (budget - overhead - type_data_bytes) at server_protocol.cpp:1672-1674 |
| `sdk/rocketnet_server_protocol.cpp:1720` | guarded | callers pass server from &server_transport->protocol (server_protocol.cpp:889 via process_received, :1875 via server_transport.cpp:2390) |
| `sdk/rocketnet_server_protocol.cpp:1721` | guarded | out_packet is response_packet (non-null, see :275) at :889, or frame_data+offset after `if ( !frame_data ) break` at server_transport.cpp:2387 |
| `sdk/rocketnet_server_protocol.cpp:1746` | out of class | lower-bound sanity on a returned size; nothing indexes or copies with it here (the bytes==0 case really returns at :1743) |
| `sdk/rocketnet_server_protocol.cpp:1773` | guarded | only production caller server_transport.cpp:2392 passes &server_transport->protocol |
| `sdk/rocketnet_server_protocol.cpp:1774` | guarded | out_packet = frame_data+offset after real `if ( !frame_data ) break` at server_transport.cpp:2387 |
| `sdk/rocketnet_server_protocol.cpp:1812` | guarded | only production caller server_transport.cpp:2390 passes &server_transport->protocol |
| `sdk/rocketnet_server_protocol.cpp:1813` | guarded | out_packet = frame_data+offset after `if ( !frame_data ) break` at server_transport.cpp:2387 |
| `sdk/rocketnet_server_protocol.cpp:1881` | guarded | only production caller server_transport.cpp:2371 passes &server_transport->protocol |
| `sdk/rocketnet_server_protocol.cpp:1908` | guarded | callers server_transport.cpp:2840/2955 pass &server_transport->protocol |
| `sdk/rocketnet_server_protocol.cpp:1909` | guarded | to = &to local at server_transport.cpp:2840/2955 |
| `sdk/rocketnet_server_protocol.cpp:1910` | guarded | sequence = &context->sequence where context is &trusted (server_transport.cpp:3014) or already deref'd |
| `sdk/rocketnet_server_protocol.cpp:1938` | guarded | callers server_transport.cpp:2847/2961 pass &server_transport->protocol |
| `sdk/rocketnet_server_protocol.cpp:1939` | guarded | wire_start = frame.data+offset after reserve_fill_frame success (server_transport.cpp:2832) or context->frame+offset after `if ( !context->frame ) return 0` (server_transport.cpp:2942) |
| `sdk/rocketnet_server_protocol.cpp:1987` | guarded | rocketnet_server_protocol_write_payload has no production caller (tests only); payload_bytes really checked at server_protocol.cpp:1991 |
| `sdk/rocketnet_server_protocol.cpp:1988` | guarded | test-only callers pass stack arrays |
| `sdk/rocketnet_server_protocol.cpp:1989` | guarded | test-only callers pass stack arrays |
| `sdk/rocketnet_server_protocol.cpp:2037` | guarded | callers server_transport.cpp:2561/2823 pass &server_transport->protocol |
| `sdk/rocketnet_server_protocol.cpp:2038` | guarded | out_address = &to local (server_transport.cpp:2561/2823) |
| `sdk/rocketnet_server_protocol.cpp:2061` | guarded | callers server_transport.cpp:2900 and server_protocol.cpp:2174 pass a live protocol |
| `sdk/rocketnet_server_protocol.cpp:2076` | guarded | callers server_transport.cpp:2890 and server_protocol.cpp:2179 pass a live protocol |
| `sdk/rocketnet_server_protocol.cpp:2077` | guarded | base_sequence = &base_sequence local at both callers |
| `sdk/rocketnet_server_protocol.cpp:2078` | guarded | fragment_count (sequence-block length) comes only from rocketnet_fragment_plan, which forces count >= 2 (fragment.cpp:63-64) and returns 0 otherwise (checked at server_transport.cpp:2864 / server_protocol.cpp:2168) |
| `sdk/rocketnet_server_protocol.cpp:2101` | guarded | callers server_transport.cpp:2909 / server_protocol.cpp:2185 pass a live protocol |
| `sdk/rocketnet_server_protocol.cpp:2102` | guarded | fragment_data = context->frame + i*stride, context->frame checked at server_transport.cpp:2942 before finish_arena_packet |
| `sdk/rocketnet_server_protocol.cpp:2103` | guarded | out_packet = frames[i].data+offset after every reserve_fill_frame succeeded (server_transport.cpp:2873) |
| `sdk/rocketnet_server_protocol.cpp:2137` | guarded | bytes bounded by block_capacity clamp plus real fragment_bytes <= MAX_FRAGMENT_BYTES refusal at server_protocol.cpp:2109-2112 |
| `sdk/rocketnet_server_protocol.cpp:2162` | guarded | write_fragments has no production caller (tests only) |
| `sdk/rocketnet_server_protocol.cpp:2163` | guarded | write_fragments test-only callers pass stack arrays |
| `sdk/rocketnet_server_protocol.cpp:2164` | guarded | write_fragments test-only; fragment_count <= max_packets really checked at server_protocol.cpp:2171 |
| `sdk/rocketnet_server_protocol.cpp:2165` | guarded | write_fragments test-only callers pass stack arrays |
| `sdk/rocketnet_server_protocol.cpp:2190` | guarded | test-only function; seal_fragment returns 0 only for disconnected/out-of-range args, excluded by plan (:2168) and connected checks (:2174/:2179); a 0 would be a zero-length record, not an overrun |
| `sdk/rocketnet_server_protocol.cpp:2210` | guarded | only production caller server_transport.cpp:3282 passes &server_transport->protocol |
| `sdk/rocketnet_server_protocol.cpp:2211` | guarded | out_packets = burst stack array (server_transport.cpp:3279) |
| `sdk/rocketnet_server_protocol.cpp:2212` | guarded | &burst_count local (server_transport.cpp:3282) |
| `sdk/rocketnet_server_protocol.cpp:2213` | guarded | &burst_bytes local (server_transport.cpp:3282) |
| `sdk/rocketnet_server_protocol.cpp:2239` | guarded | bytes upper bound by block_capacity clamp (server_protocol.cpp:1672-1674) so each copy fits its SEND_BUDGET stride; seal returns 0 only on fixed-argument mistakes or sequence 0 (protocol.cpp:243-256), and 0 gives a zero-length memcpy, no overrun |
| `sdk/rocketnet_server_protocol.cpp:2240` | guarded | all copies sealed from unchanged mapping->accepted_session with a fixed-width header, so equal size; even if not, each is <= SEND_BUDGET by the clamp at server_protocol.cpp:1672-1674, so the transport memcpy of burst_bytes (server_transport.cpp:3293) stays inside the ROCKETNET_SEND_BUDGET stride |
| `sdk/rocketnet_server_transport.cpp:374` | out of class | test-only code (ROCKETNET_TEST_SEAMS poison_slot_for_test) |
| `sdk/rocketnet_server_transport.cpp:375` | out of class | test-only code (ROCKETNET_TEST_SEAMS poison_slot_for_test) |
| `sdk/rocketnet_server_transport.cpp:393` | out of class | test-only code (ROCKETNET_TEST_SEAMS slot_clean_for_test) |
| `sdk/rocketnet_server_transport.cpp:396` | out of class | test-only code (ROCKETNET_TEST_SEAMS slot_clean_for_test) |
| `sdk/rocketnet_server_transport.cpp:534` | guarded | only caller wire_client_after: real `if ( wire_client_after >= 0 )` at server_transport.cpp:513; value is a mapping's client_index set only from client_slots_assign (< max_clients, client_slots.cpp:197-199) and capacity == max_clients (server_transport.cpp:1761) |
| `sdk/rocketnet_server_transport.cpp:686` | out of class | test-only code (ROCKETNET_TEST_SEAMS network_simulator) |
| `sdk/rocketnet_server_transport.cpp:772` | out of class | asserts a transport mode flag; callers reach it only on the non-staged path (staged returns first at server_transport.cpp:815) |
| `sdk/rocketnet_server_transport.cpp:773` | guarded | io_open refuses a non-staged transport without send_batch (driver.cpp:154-155); operations set only on success (driver.cpp:167) |
| `sdk/rocketnet_server_transport.cpp:843` | guarded | every commit follows a non-NULL reserve: `if ( response )` server_transport.cpp:2148/2298, `if ( !frame_data )` :2387/:3207/:3291; portable reserve never returns NULL |
| `sdk/rocketnet_server_transport.cpp:844` | guarded | callers really check `response_bytes > 0` (:2150/:2300), `send_bytes > 0` (:2393), `bytes <= 0` (:3216); the burst path's bytes are > 0 per protocol :2239 reasoning |
| `sdk/rocketnet_server_transport.cpp:845` | guarded | RECEIVE_CEILING == SEND_BUDGET (wire.h:83); every producer is a fixed wire size or bounded by the block_capacity clamp (server_protocol.cpp:1672-1674) |
| `sdk/rocketnet_server_transport.cpp:898` | out of class | test-only code (dev_config under ROCKETNET_TEST_SEAMS); also guarded by rocketnet_api_null_guard at server_transport.cpp:896 |
| `sdk/rocketnet_server_transport.cpp:1121` | guarded | create_internal handles NULL config: `if ( config ) ... else default_config_current` at server_transport.cpp:1165-1168, then config = &effective_config (:1200) |
| `sdk/rocketnet_server_transport.cpp:1153` | guarded | rocketnet_api_null_guard( public_address_string ) at server_transport.cpp:1134 |
| `sdk/rocketnet_server_transport.cpp:1840` | guarded | rocketnet_api_null_guard( server_transport ) at server_transport.cpp:1838 |
| `sdk/rocketnet_server_transport.cpp:1842` | out of class | state check, and the real `if ( !stopped ) return` follows at :1843 |
| `sdk/rocketnet_server_transport.cpp:1922` | guarded | rocketnet_api_null_guard at server_transport.cpp:1920 |
| `sdk/rocketnet_server_transport.cpp:1946` | guarded | rocketnet_api_null_guard at server_transport.cpp:1944 |
| `sdk/rocketnet_server_transport.cpp:1957` | guarded | rocketnet_api_null_guard at server_transport.cpp:1955 |
| `sdk/rocketnet_server_transport.cpp:1972` | out of class | test-only seam (platform_socket_for_testing, ROCKETNET_DEV development seam for functional cases) |
| `sdk/rocketnet_server_transport.cpp:2026` | out of class | test-only code (ROCKETNET_TEST_SEAMS set_test_timestamp) |
| `sdk/rocketnet_server_transport.cpp:2027` | out of class | time value, and test-only code |
| `sdk/rocketnet_server_transport.cpp:2034` | out of class | test-only code (ROCKETNET_TEST_SEAMS test_receive_work) |
| `sdk/rocketnet_server_transport.cpp:2035` | out of class | test-only code |
| `sdk/rocketnet_server_transport.cpp:2036` | out of class | test-only code |
| `sdk/rocketnet_server_transport.cpp:2047` | guarded | rocketnet_api_null_guard at server_transport.cpp:2045 |
| `sdk/rocketnet_server_transport.cpp:2144` | out of class | consistency invariant between shape check and protocol outcome; an invalid shape gets response = NULL (writes go to stack response_overflow[RECEIVE_CEILING]) and nothing is committed (`if ( response )` :2148) |
| `sdk/rocketnet_server_transport.cpp:2294` | out of class | same invariant as :2144; `if ( response )` at :2298 |
| `sdk/rocketnet_server_transport.cpp:2455` | guarded | rocketnet_api_null_guard at server_transport.cpp:2453 |
| `sdk/rocketnet_server_transport.cpp:2469` | guarded | rocketnet_api_null_guard at server_transport.cpp:2461 |
| `sdk/rocketnet_server_transport.cpp:2470` | guarded | rocketnet_api_null_guard( client_index ) at server_transport.cpp:2463 |
| `sdk/rocketnet_server_transport.cpp:2471` | guarded | rocketnet_api_null_guard( payload_data ) at server_transport.cpp:2465 |
| `sdk/rocketnet_server_transport.cpp:2472` | guarded | rocketnet_api_null_guard( payload_bytes ) at server_transport.cpp:2467 |
| `sdk/rocketnet_server_transport.cpp:2490` | guarded | rocketnet_api_null_guard at server_transport.cpp:2486 |
| `sdk/rocketnet_server_transport.cpp:2491` | guarded | rocketnet_api_null_guard( connection_id ) at server_transport.cpp:2488 |
| `sdk/rocketnet_server_transport.cpp:2506` | guarded | rocketnet_api_null_guard at server_transport.cpp:2500 |
| `sdk/rocketnet_server_transport.cpp:2507` | guarded | rocketnet_api_null_guard( client_index ) at server_transport.cpp:2502 |
| `sdk/rocketnet_server_transport.cpp:2508` | guarded | rocketnet_api_null_guard( payload_bytes ) at server_transport.cpp:2504 |
| `sdk/rocketnet_server_transport.cpp:2541` | guarded | rocketnet_api_null_guard at server_transport.cpp:2537 |
| `sdk/rocketnet_server_transport.cpp:2542` | guarded | rocketnet_api_null_guard( context ) at server_transport.cpp:2539 |
| `sdk/rocketnet_server_transport.cpp:2581` | out of class | alignment invariant of an already NULL-checked frame (`if ( !frame )` :2574); not an index/size/null guard |
| `sdk/rocketnet_server_transport.cpp:2637` | out of class | alignment invariant of a frame NULL-checked on both branches (:2603, and the ring-full refusal above :2626) |
| `sdk/rocketnet_server_transport.cpp:2656` | guarded | static; callers are start_packet (guard :2537), abort_packet (guard :2701), finish_packet_unchecked (reached via guard :3001) |
| `sdk/rocketnet_server_transport.cpp:2657` | guarded | context is the guarded public context (:2539) or &trusted local (:2710, :3014) |
| `sdk/rocketnet_server_transport.cpp:2939` | guarded | only caller finish_packet: null guards at server_transport.cpp:3001 and `if ( !server_transport \|\| !context )` :3005 |
| `sdk/rocketnet_server_transport.cpp:2940` | guarded | context = &trusted local (server_transport.cpp:3014) |
| `sdk/rocketnet_server_transport.cpp:3032` | guarded | rocketnet_api_null_guard at server_transport.cpp:3030 |
| `sdk/rocketnet_server_transport.cpp:3139` | guarded | rocketnet_api_null_guard at server_transport.cpp:3135 |
| `sdk/rocketnet_server_transport.cpp:3140` | guarded | rocketnet_api_null_guard( payload_data ) at server_transport.cpp:3137 |
| `sdk/rocketnet_server_transport.cpp:3164` | guarded | ring count before append; the only caller checks `server_transport->disconnect_burst_count < ROCKETNET_MAX_CLIENTS` at server_transport.cpp:3273 |
| `sdk/rocketnet_server_transport.cpp:3215` | guarded | real check `if ( bytes <= 0 )` aborts at server_transport.cpp:3216 before the commit uses bytes |
| `sdk/rocketnet_server_transport.cpp:3246` | out of class | test-only code (ROCKETNET_TEST_SEAMS disconnect_seals_for_test) |
| `sdk/rocketnet_server_transport.cpp:3256` | guarded | rocketnet_api_null_guard at server_transport.cpp:3254 |
| `sdk/rocketnet_server_transport.cpp:3319` | guarded | rocketnet_api_null_guard at server_transport.cpp:3317 |
| `sdk/rocketnet_server_transport.cpp:3327` | guarded | rocketnet_api_null_guard at server_transport.cpp:3325 |
| `sdk/rocketnet_server_transport.cpp:3335` | guarded | rocketnet_api_null_guard at server_transport.cpp:3333 |
| `sdk/rocketnet_server_transport.cpp:3345` | guarded | rocketnet_api_null_guard at server_transport.cpp:3343 |
| `sdk/rocketnet_server_transport.cpp:3354` | out of class | test-only code (io_name_for_testing under ROCKETNET_SEND_ERROR_TEST_HOOKS) |
| `sdk/rocketnet_server_transport.cpp:3363` | guarded | rocketnet_api_null_guard at server_transport.cpp:3361 |
| `sdk/rocketnet_server_transport.cpp:3394` | guarded | rocketnet_api_null_guard at server_transport.cpp:3390 |
| `sdk/rocketnet_server_transport.cpp:3395` | guarded | rocketnet_api_null_guard( stats ) at server_transport.cpp:3392 |
| `sdk/rocketnet_server_transport.cpp:3411` | guarded | rocketnet_api_null_guard at server_transport.cpp:3409 |
| `sdk/rocketnet_server_transport.cpp:3423` | guarded | rocketnet_api_null_guard at server_transport.cpp:3417 |
| `sdk/rocketnet_server_transport.cpp:3424` | guarded | rocketnet_api_null_guard( client_index ) at server_transport.cpp:3419 |
| `sdk/rocketnet_server_transport.cpp:3425` | guarded | rocketnet_api_null_guard( packet_ids ) at server_transport.cpp:3421 |
| `sdk/rocketnet_server_transport.cpp:3444` | guarded | rocketnet_api_null_guard at server_transport.cpp:3440 |
| `sdk/rocketnet_server_transport.cpp:3445` | guarded | rocketnet_api_null_guard( connection_id ) at server_transport.cpp:3442 |
| `sdk/rocketnet_server_transport.cpp:3458` | guarded | rocketnet_api_null_guard at server_transport.cpp:3454 |
| `sdk/rocketnet_server_transport.cpp:3459` | guarded | rocketnet_api_null_guard( stats ) at server_transport.cpp:3456 |
| `sdk/rocketnet_client_slots.cpp:63` | guarded | max_clients refused before use by if ( max_clients < 1 \|\| max_clients > INT_MAX / 2 ) return NULL at sdk/rocketnet_client_slots.cpp:69 |
| `sdk/rocketnet_client_slots.cpp:122` | guarded | destroy callers never pass NULL: create's partial cleanup passes the just-allocated slots (sdk/rocketnet_client_slots.cpp:73,93); server transport destroy checks if ( protocol.client_slots ) at sdk/rocketnet_server_transport.cpp:1858 |
| `sdk/rocketnet_client_slots.cpp:143` | guarded | slots is server->client_slots, set at create and create fails when NULL (sdk/rocketnet_server_transport.cpp:1801); connection_reset also checks !server->client_slots at sdk/rocketnet_connection_reset.cpp:75 |
| `sdk/rocketnet_client_slots.cpp:149` | guarded | slots is server->client_slots, create fails when NULL (sdk/rocketnet_server_transport.cpp:1801) |
| `sdk/rocketnet_client_slots.cpp:155` | guarded | slots is server->client_slots, create fails when NULL (sdk/rocketnet_server_transport.cpp:1801) |
| `sdk/rocketnet_client_slots.cpp:170` | guarded | slots is server->client_slots, create fails when NULL (sdk/rocketnet_server_transport.cpp:1801) |
| `sdk/rocketnet_client_slots.cpp:186` | guarded | slots is server->client_slots, create fails when NULL (sdk/rocketnet_server_transport.cpp:1801) |
| `sdk/rocketnet_client_slots.cpp:187` | guarded | only production caller passes mapping->user_data, an array member uint8_t user_data[ROCKETNET_USER_DATA_BYTES] (sdk/rocketnet_server_protocol.cpp:873, sdk/rocketnet_encryption_map.h:77); never NULL |
| `sdk/rocketnet_client_slots.cpp:188` | guarded | duplicate client id refused before assign by if ( rocketnet_client_slots_find(...) != -1 ) return IGNORED at sdk/rocketnet_server_protocol.cpp:839; the assert guards no index anyway |
| `sdk/rocketnet_client_slots.cpp:220` | guarded | slots is server->client_slots, create fails when NULL (sdk/rocketnet_server_transport.cpp:1801); index itself range-checked at sdk/rocketnet_client_slots.cpp:221 |
| `sdk/rocketnet_client_slots.cpp:228` | guarded | slots is server->client_slots, create fails when NULL (sdk/rocketnet_server_transport.cpp:1801); index range-checked at sdk/rocketnet_client_slots.cpp:229 |
| `sdk/rocketnet_client_slots.cpp:238` | guarded | slots is server->client_slots, create fails when NULL (sdk/rocketnet_server_transport.cpp:1801) |
| `sdk/rocketnet_client_slots.cpp:239` | guarded | every production caller gates on rocketnet_client_slots_connected (which range-checks, sdk/rocketnet_client_slots.cpp:221) first: server_protocol.cpp:1618,1882,1912,2040,2259; server_transport.cpp:2369,3261,3506; connection_reset.cpp:75-80; server_replay_marks.cpp:93,124; server_transport.cpp:2973 runs only after start_payload succeeded via the connected check at server_protocol.cpp:1912 |
| `sdk/rocketnet_client_slots.cpp:240` | out of class | connected flag (state); the index is already in range on every path (see :239) and a freed slot returns mapping_index -1, which callers check |
| `sdk/rocketnet_client_slots.cpp:246` | guarded | slots is server->client_slots / test-created; never NULL (sdk/rocketnet_server_transport.cpp:1801) |
| `sdk/rocketnet_client_slots.cpp:247` | guarded | no production caller; the only caller (sdk/rocketnet_tests.cpp:3901) passes an index just returned by assign |
| `sdk/rocketnet_client_slots.cpp:248` | out of class | connected flag (state), not an index/size check |
| `sdk/rocketnet_client_slots.cpp:254` | guarded | slots is server->client_slots, create fails when NULL (sdk/rocketnet_server_transport.cpp:1801) |
| `sdk/rocketnet_client_slots.cpp:255` | guarded | production caller sdk/rocketnet_server_protocol.cpp:1258 uses mapping->control->client_index, which only assign sets (server_protocol.cpp:873-877, in range) and session packets with client_index == -1 are refused at server_protocol.cpp:1143 |
| `sdk/rocketnet_client_slots.cpp:256` | out of class | connected flag (state), not an index/size check |
| `sdk/rocketnet_client_slots.cpp:262` | guarded | slots is server->client_slots, create fails when NULL (sdk/rocketnet_server_transport.cpp:1801) |
| `sdk/rocketnet_client_slots.cpp:263` | guarded | production caller sdk/rocketnet_server_protocol.cpp:1259 uses control->client_index set only by assign (server_protocol.cpp:873-877); -1 refused at server_protocol.cpp:1143 |
| `sdk/rocketnet_client_slots.cpp:264` | out of class | connected flag (state); confirm writes confirmed[i] with i already in range |
| `sdk/rocketnet_client_slots.cpp:272` | guarded | slots is server->client_slots; connection_reset checks !server->client_slots at sdk/rocketnet_connection_reset.cpp:75 |
| `sdk/rocketnet_client_slots.cpp:273` | guarded | only production caller sdk/rocketnet_connection_reset.cpp:92 is behind the range check at connection_reset.cpp:75-77 |
| `sdk/rocketnet_client_slots.cpp:274` | guarded | in class (a double release would push past free_stack[max_clients-1]); the only production caller is gated by if ( rocketnet_client_slots_connected(...) ) at sdk/rocketnet_connection_reset.cpp:91 |
| `sdk/rocketnet_connection_reset.cpp:21` | guarded | callers pass client already dereferenced (connection_reset.cpp:48 after reading client fields at :32-41); client is &client_transport->protocol |
| `sdk/rocketnet_connection_reset.cpp:30` | guarded | callers pass the protocol struct by address (client_protocol.cpp:65,189,1710 operate on client already dereferenced); never NULL |
| `sdk/rocketnet_connection_reset.cpp:73` | guarded | callers pass &server_transport->protocol or a server already dereferenced (server_protocol.cpp:949,1376,1600,2254; server_transport.cpp:3185); client_index range-checked at connection_reset.cpp:75-77 |
| `sdk/rocketnet_connection_reset.cpp:108` | guarded | object is the message object already dereferenced by every caller (message_server.cpp:303-312, 640-674) |
| `sdk/rocketnet_connection_reset.cpp:109` | guarded | server callers pass loop indices bounded by num_connections (sdk/rocketnet_message_server.cpp:303 and :656); client caller passes constant 0 (connection_reset.cpp:137) on the one-connection client object |
| `sdk/rocketnet_connection_reset.cpp:110` | guarded | same: loop bound i < num_connections at sdk/rocketnet_message_server.cpp:303 and :656; client passes 0 |
| `sdk/rocketnet_connection_reset.cpp:135` | guarded | callers pass object already dereferenced (sdk/rocketnet_message_server.cpp:312-314, 640-649) |
| `sdk/rocketnet_connection_reset.cpp:136` | out of class | is_server role flag, not an index/size/pointer |
| `sdk/rocketnet_connection_reset.cpp:142` | guarded | callers pass object already dereferenced (sdk/rocketnet_message_server.cpp:311-312, 654-674) |
| `sdk/rocketnet_connection_reset.cpp:143` | out of class | is_server role flag, not an index/size/pointer |
| `sdk/rocketnet_connection_reset.cpp:158` | out of class | inventory checker called only from ROCKETNET_TEST_SEAMS blocks (client_slots.cpp:315-329, client_transport.cpp:281, server_replay_marks.cpp:165); test-only |
| `sdk/rocketnet_client_protocol.cpp:41` | out of class | switch default in a state-name function; returns "???" after the assert |
| `sdk/rocketnet_client_protocol.cpp:103` | guarded | sole callers pass &client_transport->protocol (sdk/rocketnet_client_transport.cpp:1280, 3207); never NULL |
| `sdk/rocketnet_client_protocol.cpp:104` | guarded | sole callers pass &client_transport->counters (sdk/rocketnet_client_transport.cpp:1280, 3207); never NULL |
| `sdk/rocketnet_client_protocol.cpp:147` | guarded | callers pass addresses of struct members/locals (&client->signed_fields client_protocol.cpp:222; client_transport.cpp:1348; &ipv4_only client_transport.cpp:1573); never NULL |
| `sdk/rocketnet_client_protocol.cpp:148` | guarded | callers pass &client->server_address / &race_server (client_protocol.cpp:222; client_transport.cpp:1348,1573); never NULL |
| `sdk/rocketnet_client_protocol.cpp:182` | guarded | sole caller passes &client_transport->protocol (sdk/rocketnet_client_transport.cpp:1300) |
| `sdk/rocketnet_client_protocol.cpp:183` | guarded | public create refuses NULL token with rocketnet_api_null_guard( connect_token ) at sdk/rocketnet_client_transport.cpp:1076 before the connect at :1300 |
| `sdk/rocketnet_client_protocol.cpp:184` | guarded | counters set by rocketnet_client_protocol_init (client_protocol.cpp:109) at client_transport.cpp:1280, before connect at :1300, and kept across resets (connection_reset.cpp:37,54) |
| `sdk/rocketnet_client_protocol.cpp:290` | guarded | static function; all three callers pass constant sizes 0 or 8 (client_protocol.cpp:351-352, 398-399, 1695-1696) into plaintext[ACK_BLOCK_MAX + 8] |
| `sdk/rocketnet_client_protocol.cpp:313` | guarded | post-write size; bounded by the block_capacity clamp to SEND_BUDGET - OVERHEAD - type_data_bytes at client_protocol.cpp:296-298 and type_data_bytes <= 8 (constant callers) |
| `sdk/rocketnet_client_protocol.cpp:331` | guarded | sole caller passes &client_transport->protocol after rocketnet_api_null_guard at sdk/rocketnet_client_transport.cpp:2364 (call at :2473) |
| `sdk/rocketnet_client_protocol.cpp:332` | guarded | sole caller passes stack buffer packet[ROCKETNET_RECEIVE_CEILING] (sdk/rocketnet_client_transport.cpp:2471-2473) |
| `sdk/rocketnet_client_protocol.cpp:333` | guarded | sole caller passes &packet_bytes local (sdk/rocketnet_client_transport.cpp:2472-2473) |
| `sdk/rocketnet_client_protocol.cpp:400` | out of class | result check on a sealed keep-alive; a 0 (seal failure) just sends nothing (caller tests packet_bytes > 0 at client_transport.cpp:2485,2493); no index/size overrun |
| `sdk/rocketnet_client_protocol.cpp:589` | out of class | response_bytes is (void)-discarded and never used; the stored response is sent with the constant WIRE_BYTES into a fixed-size stored_response buffer |
| `sdk/rocketnet_client_protocol.cpp:1001` | out of class | switch default over the I/O verdict; returns -1 after the assert |
| `sdk/rocketnet_client_protocol.cpp:1236` | guarded | inside if ( !client->reassembly ) { ... return 1; } at sdk/rocketnet_client_protocol.cpp:1231-1239; the real check is the if |
| `sdk/rocketnet_client_protocol.cpp:1297` | guarded | sole caller path passes &client_transport->protocol (sdk/rocketnet_client_transport.cpp:2036 via process_record) |
| `sdk/rocketnet_client_protocol.cpp:1298` | guarded | packet_data is a receive-buffer frame pointer receive_buffer->packet_data[i] set at sdk/rocketnet_client_transport.cpp:3290 (or the I/O core's frames); never NULL |
| `sdk/rocketnet_client_protocol.cpp:1299` | guarded | caller passes payload ? payload : overflow (stack array) at sdk/rocketnet_client_transport.cpp:2038; never NULL; capacity checked at client_protocol.cpp:1311 |
| `sdk/rocketnet_client_protocol.cpp:1300` | guarded | caller passes &payload_bytes local (sdk/rocketnet_client_transport.cpp:2029,2040) |
| `sdk/rocketnet_client_protocol.cpp:1409` | guarded | sole caller passes &client_transport->protocol (sdk/rocketnet_client_transport.cpp:2036) |
| `sdk/rocketnet_client_protocol.cpp:1426` | guarded | sole caller passes &client_transport->protocol after api null guards (sdk/rocketnet_client_transport.cpp:2550-2553, call at :2569) |
| `sdk/rocketnet_client_protocol.cpp:1427` | guarded | caller passes &sequence local (sdk/rocketnet_client_transport.cpp:2568-2569) |
| `sdk/rocketnet_client_protocol.cpp:1446` | guarded | callers pass &client_transport->protocol (sdk/rocketnet_client_transport.cpp:2715, 2810) |
| `sdk/rocketnet_client_protocol.cpp:1447` | guarded | wire_start = context->frame + offset; frame checked non-NULL by if ( !context->frame ) return 0 at sdk/rocketnet_client_transport.cpp:2798 (and frame from ring data at :2715) |
| `sdk/rocketnet_client_protocol.cpp:1489` | guarded | no SDK runtime caller; used by tests/soak/fuzz harnesses that pass &protocol (e.g. tests/test_unit_max_payload.cpp); payload_bytes range-checked at client_protocol.cpp:1496 |
| `sdk/rocketnet_client_protocol.cpp:1490` | guarded | no SDK runtime caller; harness callers pass buffers; size range-checked at client_protocol.cpp:1496 before the memcpy at :1504 |
| `sdk/rocketnet_client_protocol.cpp:1491` | guarded | no SDK runtime caller; harness callers pass buffers |
| `sdk/rocketnet_client_protocol.cpp:1537` | guarded | callers pass &client_transport->protocol (sdk/rocketnet_client_transport.cpp:2764) or &protocol in write_fragments |
| `sdk/rocketnet_client_protocol.cpp:1549` | guarded | callers pass &client_transport->protocol (sdk/rocketnet_client_transport.cpp:2758) or the already-dereferenced client (client_protocol.cpp:1648) |
| `sdk/rocketnet_client_protocol.cpp:1550` | guarded | callers pass &base_sequence local (sdk/rocketnet_client_transport.cpp:2757-2758; client_protocol.cpp:1645) |
| `sdk/rocketnet_client_protocol.cpp:1551` | out of class | fragment_count is only an atomic-add amount on the sequence counter (client_protocol.cpp:1556); not an index/size into memory; rocketnet_fragment_plan yields >= 2 anyway (client_transport.cpp:2731) |
| `sdk/rocketnet_client_protocol.cpp:1572` | guarded | callers pass &client_transport->protocol (sdk/rocketnet_client_transport.cpp:2772) |
| `sdk/rocketnet_client_protocol.cpp:1573` | guarded | fragment_data = context->frame + i*stride, frame non-NULL (finish_arena reached only after the frame check at sdk/rocketnet_client_transport.cpp:2798); fragment_bytes bounded at client_protocol.cpp:1585-1588 |
| `sdk/rocketnet_client_protocol.cpp:1574` | guarded | out_packet = ring->data + slots[i]*STRIDE with slots[i] < RING_SIZE checked at sdk/rocketnet_client_transport.cpp:2741 |
| `sdk/rocketnet_client_protocol.cpp:1607` | guarded | post-write size; bounded by the block_capacity clamp (client_protocol.cpp:1593-1595) and fragment_bytes <= ROCKETNET_MAX_FRAGMENT_BYTES (client_protocol.cpp:1585-1588) |
| `sdk/rocketnet_client_protocol.cpp:1627` | guarded | no SDK runtime caller; harness callers pass &protocol |
| `sdk/rocketnet_client_protocol.cpp:1628` | guarded | no SDK runtime caller; harness callers pass a buffer; payload_bytes validated by rocketnet_fragment_plan at client_protocol.cpp:1636 |
| `sdk/rocketnet_client_protocol.cpp:1629` | guarded | no SDK runtime caller; fragment_count <= max_packets checked at client_protocol.cpp:1639 before writing out_packets + i * SEND_BUDGET |
| `sdk/rocketnet_client_protocol.cpp:1630` | guarded | no SDK runtime caller; out_packet_bytes[i] writes bounded by the same fragment_count <= max_packets check at client_protocol.cpp:1639 |
| `sdk/rocketnet_client_protocol.cpp:1656` | out of class | seal result; a 0 would store 0 into out_packet_bytes[i] (in range by :1639) and copy nothing; args in range by rocketnet_fragment_plan (:1636) |
| `sdk/rocketnet_client_protocol.cpp:1673` | guarded | sole caller passes &client_transport->protocol after the api null guard (sdk/rocketnet_client_transport.cpp:3040, call at :3061) |
| `sdk/rocketnet_client_protocol.cpp:1674` | guarded | sole caller passes stack burst[NUM_DISCONNECT_PACKETS * SEND_BUDGET] (sdk/rocketnet_client_transport.cpp:3058) |
| `sdk/rocketnet_client_protocol.cpp:1675` | guarded | sole caller passes &burst_count local (sdk/rocketnet_client_transport.cpp:3058) |
| `sdk/rocketnet_client_protocol.cpp:1676` | guarded | sole caller passes &burst_bytes local (sdk/rocketnet_client_transport.cpp:3059) |
| `sdk/rocketnet_client_protocol.cpp:1698` | out of class | minimum-size result check; a 0 just makes burst_bytes 0 and the commit copies nothing; no overrun |
| `sdk/rocketnet_client_protocol.cpp:1699` | guarded | every bytes is <= SEND_BUDGET by the block_capacity clamp at client_protocol.cpp:296-298, and the burst buffer strides SEND_BUDGET (client_transport.cpp:3058, 3077), so using burst_bytes for each copy stays in bounds; equality is a wire-shape invariant |
| `sdk/rocketnet_client_transport.cpp:62` | out of class | time usability (finite, monotone); handled by the if returning last_good |
| `sdk/rocketnet_client_transport.cpp:337` | out of class | ROCKETNET_TEST_SEAMS test-only function (client_transport.cpp:334-340) |
| `sdk/rocketnet_client_transport.cpp:347` | out of class | ROCKETNET_TEST_SEAMS test-only function (client_transport.cpp:343-377) |
| `sdk/rocketnet_client_transport.cpp:383` | out of class | thread-not-started state flag in abort_create |
| `sdk/rocketnet_client_transport.cpp:399` | guarded | callers pass a transport already through rocketnet_api_null_guard (update client_transport.cpp:2364 -> :2476; disconnect :3040 -> :3062) or the test seam at :308 |
| `sdk/rocketnet_client_transport.cpp:1022` | guarded | a NULL config is handled by create_internal: if ( config ) memcpy at client_transport.cpp:1109, then config = &effective_config at :1142 |
| `sdk/rocketnet_client_transport.cpp:1094` | guarded | if ( !rocketnet_api_null_guard( connect_token ) ) return NULL at sdk/rocketnet_client_transport.cpp:1076 |
| `sdk/rocketnet_client_transport.cpp:1956` | out of class | stopped lifecycle state; followed by a real if ( !stopped ) return |
| `sdk/rocketnet_client_transport.cpp:2183` | guarded | callers: packet_bytes > 0 checked at client_transport.cpp:2485 for a packet from a RECEIVE_CEILING buffer bounded <= SEND_BUDGET by client_protocol.cpp:296-298; burst copies bounded the same way (burst_bytes <= SEND_BUDGET, 0 copies nothing) |
| `sdk/rocketnet_client_transport.cpp:2263` | guarded | returned_count is always 0 when num_exposed > 0: exposure only happens after publish_returns zeroes it under the same lock (client_transport.cpp:2236-2239, 2224), and a failed expose returns 0 so return_legacy exits at :2260 |
| `sdk/rocketnet_client_transport.cpp:2366` | guarded | if ( !rocketnet_api_null_guard( client_transport ) ) return at sdk/rocketnet_client_transport.cpp:2364 |
| `sdk/rocketnet_client_transport.cpp:2518` | guarded | rocketnet_api_null_guard( client_transport ) at sdk/rocketnet_client_transport.cpp:2514 |
| `sdk/rocketnet_client_transport.cpp:2519` | guarded | rocketnet_api_null_guard( packet_data ) at sdk/rocketnet_client_transport.cpp:2516 |
| `sdk/rocketnet_client_transport.cpp:2520` | guarded | if ( packet_bytes < 1 \|\| packet_bytes > max_payload_bytes ) return at sdk/rocketnet_client_transport.cpp:2522 |
| `sdk/rocketnet_client_transport.cpp:2554` | guarded | rocketnet_api_null_guard( client_transport ) at sdk/rocketnet_client_transport.cpp:2550 |
| `sdk/rocketnet_client_transport.cpp:2555` | guarded | rocketnet_api_null_guard( context ) at sdk/rocketnet_client_transport.cpp:2552 |
| `sdk/rocketnet_client_transport.cpp:2615` | out of class | 16-byte alignment invariant of the frame, not a bound |
| `sdk/rocketnet_client_transport.cpp:2633` | guarded | static; callers already null-guarded the transport (client_transport.cpp:2550 -> :2625; :2669 -> :2678; :2831 -> :2844 -> :2803/:2816) |
| `sdk/rocketnet_client_transport.cpp:2634` | guarded | static; callers pass a guarded context (:2552 -> :2625) or &trusted locals (:2678, :2844) |
| `sdk/rocketnet_client_transport.cpp:2795` | guarded | static; sole caller null-guards the transport at sdk/rocketnet_client_transport.cpp:2831 (call at :2844) |
| `sdk/rocketnet_client_transport.cpp:2796` | guarded | static; sole caller passes &trusted local (sdk/rocketnet_client_transport.cpp:2844) |
| `sdk/rocketnet_client_transport.cpp:2856` | guarded | rocketnet_api_null_guard( client_transport ) at sdk/rocketnet_client_transport.cpp:2852 |
| `sdk/rocketnet_client_transport.cpp:2910` | guarded | rocketnet_api_null_guard( client_transport ) at sdk/rocketnet_client_transport.cpp:2906 |
| `sdk/rocketnet_client_transport.cpp:2911` | guarded | rocketnet_api_null_guard( packet_data ) at sdk/rocketnet_client_transport.cpp:2908 |
| `sdk/rocketnet_client_transport.cpp:2952` | guarded | rocketnet_api_null_guard( client_transport ) at sdk/rocketnet_client_transport.cpp:2950 |
| `sdk/rocketnet_client_transport.cpp:2964` | guarded | rocketnet_api_null_guard( client_transport ) at sdk/rocketnet_client_transport.cpp:2962 |
| `sdk/rocketnet_client_transport.cpp:2974` | guarded | rocketnet_api_null_guard( client_transport ) at sdk/rocketnet_client_transport.cpp:2970 |
| `sdk/rocketnet_client_transport.cpp:2975` | guarded | rocketnet_api_null_guard( payload_bytes ) at sdk/rocketnet_client_transport.cpp:2972; payload_index range-checked at :2981 |
| `sdk/rocketnet_client_transport.cpp:2997` | guarded | rocketnet_api_null_guard( client_transport ) at sdk/rocketnet_client_transport.cpp:2991 |
| `sdk/rocketnet_client_transport.cpp:2998` | guarded | rocketnet_api_null_guard( payload_data ) at sdk/rocketnet_client_transport.cpp:2993 |
| `sdk/rocketnet_client_transport.cpp:2999` | guarded | rocketnet_api_null_guard( payload_bytes ) at sdk/rocketnet_client_transport.cpp:2995 |
| `sdk/rocketnet_client_transport.cpp:3011` | guarded | rocketnet_api_null_guard( client_transport ) at sdk/rocketnet_client_transport.cpp:3009 |
| `sdk/rocketnet_client_transport.cpp:3022` | guarded | rocketnet_api_null_guard( client_transport ) at sdk/rocketnet_client_transport.cpp:3020 |
| `sdk/rocketnet_client_transport.cpp:3030` | guarded | rocketnet_api_null_guard( client_transport ) at sdk/rocketnet_client_transport.cpp:3028 |
| `sdk/rocketnet_client_transport.cpp:3042` | guarded | rocketnet_api_null_guard( client_transport ) at sdk/rocketnet_client_transport.cpp:3040 |
| `sdk/rocketnet_client_transport.cpp:3121` | guarded | rocketnet_api_null_guard( client_transport ) at sdk/rocketnet_client_transport.cpp:3119 |
| `sdk/rocketnet_client_transport.cpp:3155` | guarded | rocketnet_api_null_guard( client_transport ) at sdk/rocketnet_client_transport.cpp:3151 |
| `sdk/rocketnet_client_transport.cpp:3156` | guarded | rocketnet_api_null_guard( packet_ids ) at sdk/rocketnet_client_transport.cpp:3153 |
| `sdk/rocketnet_client_transport.cpp:3169` | guarded | rocketnet_api_null_guard( client_transport ) at sdk/rocketnet_client_transport.cpp:3165 |
| `sdk/rocketnet_client_transport.cpp:3170` | guarded | rocketnet_api_null_guard( stats ) at sdk/rocketnet_client_transport.cpp:3167 |
| `sdk/rocketnet_client_transport.cpp:3205` | out of class | malloc asserted then used, but inside #if defined(ROCKETNET_ACK_STATS_TEST_HOOKS) test-only hook (client_transport.cpp:3199-3216) |
| `sdk/rocketnet_client_transport.cpp:3249` | guarded | static; callers pass a guarded/created transport: update after null guard (client_transport.cpp:2364 -> :2428), the worker thread started by create, and the test seam (:3314) |
| `sdk/rocketnet_client_transport.cpp:3303` | out of class | ROCKETNET_TEST_SEAMS test-only function, and followed by a real if returning (client_transport.cpp:3305-3307) |
| `sdk/rocketnet_client_transport.cpp:3320` | out of class | ROCKETNET_TEST_SEAMS test-only function |
| `sdk/rocketnet_client_transport.cpp:3330` | out of class | ROCKETNET_TEST_SEAMS test-only function |
| `sdk/rocketnet_client_transport.cpp:3371` | out of class | ROCKETNET_TEST_SEAMS test-only function (network_simulator seam, client_transport.cpp:3368) |
| `sdk/rocketnet_connect_token.cpp:73` | guarded | sole caller passes &client_transport->protocol.signed_fields (sdk/rocketnet_client_transport.cpp:1332); never NULL |
| `sdk/rocketnet_connect_token.cpp:531` | guarded | if ( length == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 ) guards the memcpy at sdk/rocketnet_connect_token.cpp:532 and the return at :535 |
| `sdk/rocketnet_endpoint.cpp:374` | out of class | reliability-kind invariant of a pending reservation (enum value), not an index/size/pointer |
| `sdk/rocketnet_endpoint.cpp:501` | out of class | pending_type == -1 state flag (no in-place build open) |
| `sdk/rocketnet_endpoint.cpp:556` | guarded | if ( length <= 0 ) { rn_endpoint_in_place_failed; return serial; } at sdk/rocketnet_endpoint.cpp:568 |
| `sdk/rocketnet_keys.cpp:32` | guarded | static; callers pass constants 16 and 112 (keys.cpp:55-56, 87-88), both + 8 <= 128 |
| `sdk/rocketnet_keys.cpp:45` | guarded | callers pass &keys / &temporary_keys / &client->keys (server_protocol.cpp:681,712; client_protocol.cpp:521,547,659); never NULL |
| `sdk/rocketnet_keys.cpp:46` | guarded | callers pass array members private_data->client_to_server_key / client->token.client_to_server_key; never NULL |
| `sdk/rocketnet_keys.cpp:47` | guarded | callers pass array members ...server_to_client_key; never NULL |
| `sdk/rocketnet_keys.cpp:48` | guarded | callers pass connection_salt locals/array members (server_protocol.cpp:684; client->connection_salt client_protocol.cpp:524,550,662) |
| `sdk/rocketnet_keys.cpp:49` | guarded | callers pass session_nonce/carried_nonce locals or arrays (client_protocol.cpp:524,550,662; server_protocol.cpp:681-686) |
| `sdk/rocketnet_keys.cpp:69` | guarded | callers pass &keys / &client->keys (server_protocol.cpp:712; client_protocol.cpp:547) |
| `sdk/rocketnet_keys.cpp:70` | guarded | callers pass ephemeral key arrays (server_protocol.cpp:712-; client_protocol.cpp:547-); never NULL |
| `sdk/rocketnet_keys.cpp:71` | guarded | callers pass ephemeral key arrays/packet field pointers; never NULL |
| `sdk/rocketnet_keys.cpp:72` | guarded | callers pass ephemeral key arrays; never NULL |
| `sdk/rocketnet_keys.cpp:73` | guarded | callers pass ephemeral key arrays; never NULL |
| `sdk/rocketnet_message_server.cpp:392` | out of class | boolean stopped result, followed by a real if ( !stopped ) return at message_server.cpp:393 |
| `sdk/rocketnet_message_server.cpp:479` | out of class | is_server role flag; the index comes from the loop i < num_connections (message_server.cpp:656, 670) |
| `sdk/rocketnet_message_server.cpp:560` | out of class | misuse count (held messages) handled by a real if: counted, logged, result set |
| `sdk/rocketnet_message_server.cpp:584` | out of class | misuse count (waiting deliveries) handled by real code: counted, logged, connection kicked |
| `sdk/rocketnet_replay.cpp:32` | guarded | callers pass the address of a struct member (&mapping->accepted_session, &client->accepted_session, replay = &mapping->...); never NULL |
| `sdk/rocketnet_replay.cpp:33` | out of class | sequence != 0 is a protocol value rule; the bit index is masked by sequence % ROCKETNET_REPLAY_WINDOW_BITS (replay.cpp:13), always in range |
| `sdk/rocketnet_replay.cpp:49` | guarded | callers pass the address of a struct member (e.g. server_protocol.cpp:1253,1209; client_protocol.cpp:1716); never NULL |
| `sdk/rocketnet_replay.cpp:50` | out of class | value rule; bit index masked by % ROCKETNET_REPLAY_WINDOW_BITS (replay.cpp:19,25) |
| `sdk/rocketnet_replay.cpp:51` | out of class | replay-state invariant (not already received), not an index/size |
| `sdk/rocketnet_server_replay_marks.cpp:65` | guarded | sole production caller checks if ( server_transport->replay_marks ) at sdk/rocketnet_server_transport.cpp:1872 |
| `sdk/rocketnet_server_replay_marks.cpp:75` | guarded | marks created whenever replay_mark op exists and create fails when it is NULL (sdk/rocketnet_server_transport.cpp:1798-1801); walk called only under the same if ( replay_mark ) at :2416 |
| `sdk/rocketnet_server_replay_marks.cpp:76` | guarded | protocol.client_slots; server create fails when NULL (sdk/rocketnet_server_transport.cpp:1801) |
| `sdk/rocketnet_server_replay_marks.cpp:77` | guarded | protocol.encryption_map; server create fails when NULL (sdk/rocketnet_server_transport.cpp:1801) |
| `sdk/rocketnet_server_replay_marks.cpp:78` | guarded | sink is &replay_sink local (sdk/rocketnet_server_transport.cpp:2418-2423) |
| `sdk/rocketnet_server_replay_marks.cpp:79` | guarded | driver registration refuses a partial replay op set: replay_forget/mark/commit all-or-none at sdk/rocketnet_driver.cpp:156-157; caller checks replay_mark at sdk/rocketnet_server_transport.cpp:2416 |
| `sdk/rocketnet_server_replay_marks.cpp:80` | guarded | caller enters only if ( io.operations->replay_mark ) at sdk/rocketnet_server_transport.cpp:2416 |
| `sdk/rocketnet_token_tables.cpp:75` | guarded | if ( capacity < 1 \|\| capacity > INT_MAX / 2 ) return NULL at sdk/rocketnet_token_tables.cpp:81 (and at history create :289) |
| `sdk/rocketnet_token_tables.cpp:133` | guarded | static; every caller checks the table first (token_tables.cpp:306-309, 318-321) |
| `sdk/rocketnet_token_tables.cpp:211` | out of class | chain-membership assert after a failed unlink walk; no index/size/pointer is used from it (unlink just returns; the entry is not linked anywhere it could corrupt) |
| `sdk/rocketnet_token_tables.cpp:361` | guarded | select_slot with evict_oldest=1, protect_claims=0 cannot return -1: either free_head (token_tables.cpp:230-234) or claim_free is always true so oldest is set at i=0 (token_tables.cpp:239-247), and capacity >= 1 is enforced at create (:81, :289) |
| `sdk/rocketnet_ack.cpp:30` | guarded | output is a caller stack/array buffer at every caller (ack.cpp:147 via build; build callers server_protocol.cpp:1675/1956/2003/2124, client_protocol.cpp:299/1463/1504/1594, server_transport.cpp:3173 all pass local or struct arrays); never null |
| `sdk/rocketnet_ack.cpp:31` | guarded | only non-test caller is ack.cpp:147 passing &block (a local) |
| `sdk/rocketnet_ack.cpp:32` | guarded | num_ranges is clamped by max_ranges <= ROCKETNET_ACK_BLOCK_MAX_RANGES (ack.cpp:84-86) and the loop bound block.num_ranges < max_ranges (ack.cpp:109); other writers are tests only |
| `sdk/rocketnet_ack.cpp:33` | out of class | value bound on a wire field written as uint16, not an index/size; build also caps first_run < 0xFFFF (ack.cpp:94) |
| `sdk/rocketnet_ack.cpp:66` | guarded | output is a local/struct array at every caller (server_protocol.cpp:1665/1952/1998/2119, client_protocol.cpp:289/1459/1499/1589, server_transport.cpp:3173 burst->plaintext) |
| `sdk/rocketnet_ack.cpp:67` | guarded | replay is &mapping->accepted_session / &client->accepted_session (address of a member) at every caller |
| `sdk/rocketnet_ack.cpp:68` | guarded | capacity >= 7 at every caller by constant arithmetic after real payload checks: SEND_BUDGET 1452 - overhead 26 - payload (<=1416 checked server_protocol.cpp:1945/1991, client_protocol.cpp:1452/1496) = >=10; fragment path 1452-26-6-fragment_bytes(<=1413 checked server_protocol.cpp:2112, client_protocol.cpp:1582) = >=7; type_data <=8 path >=1418; transport path 1426 |
| `sdk/rocketnet_ack.cpp:104` | out of class | invariant on a run length used in arithmetic (next_high), not an index/size/divisor/pointer |
| `sdk/rocketnet_ack.cpp:160` | guarded | block is &block, a local, at both non-test callers (server_protocol.cpp:1221, client_protocol.cpp:854) |
| `sdk/rocketnet_ack.cpp:161` | guarded | data is the opened local buffer at both callers (server_protocol.cpp:1221, client_protocol.cpp:854); data_bytes checked against MIN at ack.cpp:164 and geometry at ack.cpp:174 |
| `sdk/rocketnet_ack.cpp:548` | guarded | sequence is only used as sent[sequence % ring] (ack.cpp:550), always in range |
| `sdk/rocketnet_ack.cpp:642` | out of class | a stats counter, not index/size; slot indexes are sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE (ack.cpp:640); caller ack.cpp:690 checks count > 0 |
| `sdk/rocketnet_ack.cpp:785` | guarded | endpoint is a member address at every caller (&client->ack client_protocol.cpp:317/1474/1519/1610; &server->acks->endpoints[client_index] server_protocol.cpp:1704/1969/2020/2142) |
| `sdk/rocketnet_ack.cpp:786` | guarded | real check: if ( !sequence \|\| !isfinite(time) ) return REFUSED (ack.cpp:789) |
| `sdk/rocketnet_ack.cpp:787` | out of class | flag consistency invariant, not index/size/pointer |
| `sdk/rocketnet_ack.cpp:866` | out of class | no non-test caller in sdk (header-declared helper); pointer counters only in tests |
| `sdk/rocketnet_ack.cpp:867` | out of class | no non-test caller in sdk (header-declared helper); pointer counters only in tests |
| `sdk/rocketnet_ack.cpp:885` | guarded | only caller passes &server->acks->endpoints[client_index] (server_protocol.cpp:1760) |
| `sdk/rocketnet_ack.cpp:886` | guarded | sequence only used as sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE] (ack.cpp:890), always in range |
| `sdk/rocketnet_ack.cpp:900` | guarded | callers pass member addresses (&...acks->endpoints[context->client_index] server_transport.cpp:2901, &client_transport->protocol.ack client_transport.cpp:2765) |
| `sdk/rocketnet_ack.cpp:901` | out of class | count stored as aggregate->remaining, not an index/size; index is id % ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE (ack.cpp:908); callers pass fragment_plan counts (>=2 by fragment.cpp:63-64) |
| `sdk/rocketnet_ack.cpp:923` | guarded | endpoint is &client->ack (client_protocol.cpp:905) or &server->acks->endpoints[client_index] (server_protocol.cpp:1263-1265) |
| `sdk/rocketnet_ack.cpp:924` | guarded | sequence only used modulo ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE (ack.cpp:934-935), always in range |
| `sdk/rocketnet_ack.cpp:1124` | guarded | the if at ack.cpp:1122 returns on mismatch; the assert is only the debug voice of that real check |
| `sdk/rocketnet_ack.cpp:1225` | guarded | endpoint is &client->ack (client_protocol.cpp:907) or &server->acks->endpoints[client_index] (server_protocol.cpp:1263/1268) |
| `sdk/rocketnet_ack.cpp:1226` | guarded | block is &block, a local, at both callers (client_protocol.cpp:907, server_protocol.cpp:1268) |
| `sdk/rocketnet_ack.cpp:1227` | guarded | client->acked_packet_ids (struct array) / server->acks->acked_packet_ids (malloc checked ack.cpp:1537, create fails) |
| `sdk/rocketnet_ack.cpp:1228` | guarded | &client->num_acked / &server->acks->num_acked member addresses (client_protocol.cpp:908, server_protocol.cpp:1269) |
| `sdk/rocketnet_ack.cpp:1229` | guarded | &counters[...] member addresses at both callers (client_protocol.cpp:909, server_protocol.cpp:1270) |
| `sdk/rocketnet_ack.cpp:1230` | guarded | &counters[...] member addresses at both callers (client_protocol.cpp:910, server_protocol.cpp:1271) |
| `sdk/rocketnet_ack.cpp:1480` | out of class | rocketnet_ack_stats_history_compute has no non-test caller (reference recompute, ack.h:134); test-only |
| `sdk/rocketnet_ack.cpp:1487` | guarded | only non-test caller passes &client_transport->protocol.ack (client_transport.cpp:3176) |
| `sdk/rocketnet_ack.cpp:1515` | guarded | real check: max_clients < 1 \|\| > INT_MAX/... returns NULL (ack.cpp:1517-1518) |
| `sdk/rocketnet_ack.cpp:1551` | guarded | callers guard: if ( server_transport->protocol.acks ) (server_transport.cpp:1860); ack.cpp:1539 passes a non-null acks |
| `sdk/rocketnet_ack.cpp:1563` | guarded | only caller guards if ( server->acks ) (connection_reset.cpp:98) |
| `sdk/rocketnet_ack.cpp:1564` | guarded | caller refuses client_index outside [0, client_slots max_clients) (connection_reset.cpp:75-77); slots and acks created with the same max_clients (server_transport.cpp:1758-1759) |
| `sdk/rocketnet_fragment.cpp:23` | guarded | output is plaintext + block_bytes, a local array (server_protocol.cpp:2119/2126, client_protocol.cpp:1589/1596) |
| `sdk/rocketnet_fragment.cpp:24` | out of class | payload id value, not index/size; callers also refuse 0 (client_protocol.cpp:1579, server_protocol.cpp:2109-2110) |
| `sdk/rocketnet_fragment.cpp:25` | guarded | both callers refuse fragment_count < 2 \|\| > ROCKETNET_MAX_FRAGMENTS (server_protocol.cpp:2110, client_protocol.cpp:1580) |
| `sdk/rocketnet_fragment.cpp:26` | guarded | both callers refuse fragment_index < 0 \|\| >= fragment_count (server_protocol.cpp:2111, client_protocol.cpp:1581) |
| `sdk/rocketnet_fragment.cpp:36` | guarded | only non-test caller passes type_data (fragment.cpp:331), already asserted and from caller's opened buffer; length checked fragment.cpp:320 |
| `sdk/rocketnet_fragment.cpp:37` | guarded | &payload_id local (fragment.cpp:331) |
| `sdk/rocketnet_fragment.cpp:38` | guarded | &fragment_index local (fragment.cpp:331) |
| `sdk/rocketnet_fragment.cpp:39` | guarded | &fragment_count local (fragment.cpp:331) |
| `sdk/rocketnet_fragment.cpp:49` | guarded | callers pass &fragment_count locals (server_protocol.cpp:2168, client_protocol.cpp:1636, server_transport.cpp:2864, client_transport.cpp:2731, endpoint.cpp:849) |
| `sdk/rocketnet_fragment.cpp:50` | guarded | callers pass &fragment_stride/&stride locals (same call sites as fragment.cpp:49) |
| `sdk/rocketnet_fragment.cpp:68` | guarded | arithmetic consequence of the real bound payload_bytes <= MAX_FRAGMENTS*MAX_FRAGMENT_BYTES (fragment.cpp:59): count = ceil(n/1413) <= 256 |
| `sdk/rocketnet_fragment.cpp:69` | guarded | arithmetic consequence of fragment.cpp:59: n > 1416 and count >= 2 give 1 <= ceil(n/count) <= 1413 |
| `sdk/rocketnet_fragment.cpp:70` | guarded | arithmetic consequence of fragment.cpp:59 with stride = ceil(n/count): final remainder >= 1 |
| `sdk/rocketnet_fragment.cpp:71` | guarded | arithmetic consequence of stride = ceil(n/count) (fragment.cpp:66): remainder <= stride |
| `sdk/rocketnet_fragment.cpp:194` | guarded | real check: rocketnet_reassembly_memory_required_delivered refuses num_connections < 1 (fragment.cpp:94) and create returns NULL (fragment.cpp:199-200) |
| `sdk/rocketnet_fragment.cpp:195` | guarded | real check: num_slots < 1 \|\| > ROCKETNET_MAX_FRAGMENTS refused (fragment.cpp:94), create returns NULL (fragment.cpp:199-200) |
| `sdk/rocketnet_fragment.cpp:196` | guarded | real check: max_payload_bytes < MAX_PACKET_PAYLOAD or > MAX_FRAGMENTED refused (fragment.cpp:95), create returns NULL (fragment.cpp:199-200) |
| `sdk/rocketnet_fragment.cpp:197` | guarded | real check: delivered_capacity < 1 refused (fragment.cpp:95), create returns NULL (fragment.cpp:199-200) |
| `sdk/rocketnet_fragment.cpp:252` | guarded | callers guard: if ( server_transport->protocol.reassembly ) (server_transport.cpp:1868), if ( client_transport->reassembly ) (client_transport.cpp:1981); fragment.cpp:239 passes non-null set |
| `sdk/rocketnet_fragment.cpp:264` | guarded | both callers guard null: if ( reassembly ) (connection_reset.cpp:65), if ( server->reassembly ) (connection_reset.cpp:101) |
| `sdk/rocketnet_fragment.cpp:273` | guarded | set always set by create, which fails when null (server_transport.cpp:1801, client_transport.cpp:1262) |
| `sdk/rocketnet_fragment.cpp:311` | guarded | set always set by create, which fails when null (server_transport.cpp:1801, client_transport.cpp:1262) |
| `sdk/rocketnet_fragment.cpp:313` | guarded | type_data is a pointer into the caller's opened buffer (server_protocol.cpp:1355-1356, client_protocol.cpp:1255-1256 opened + offset); length refused at fragment.cpp:320 |
| `sdk/rocketnet_fragment.cpp:314` | guarded | counters is server counters array / client->counters->values (server_protocol.cpp:1357, client_protocol.cpp:1257), struct arrays |
| `sdk/rocketnet_fragment.cpp:315` | guarded | table is &fragment_counters, a static local (server_protocol.cpp:1341/1357, client_protocol.cpp:1257) |
| `sdk/rocketnet_fragment.cpp:574` | guarded | count >= 2 (wire count 1 refused fragment.cpp:405) so fragment 0 is non-final and fixes the stride (fragment.cpp:533) before num_received can reach fragment_count |
| `sdk/rocketnet_fragment.cpp:575` | guarded | a staged final is placed and final_staged cleared when the stride is fixed (fragment.cpp:533-537), which must happen before completion |
| `sdk/rocketnet_fragment.cpp:576` | guarded | every fragment has >= 1 byte (type_data_bytes < HEADER+1 refused fragment.cpp:320) |
| `sdk/rocketnet_fragment.cpp:579` | guarded | real check: stride*(count-1)+final > max_payload_bytes refused before mutation (fragment.cpp:487); delivery copy also bounded by delivered_bytes (fragment.cpp:586) |
| `sdk/rocketnet_fragment.cpp:629` | guarded | callers pass compile-time ROCKETNET_SEND_ARENA_SLOTS (server_transport.cpp:1795, client_transport.cpp:1253) or test hook constant 1 |
| `sdk/rocketnet_fragment.cpp:630` | guarded | callers pass ( ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES + 15 ) & ~15 (client_transport.cpp:1247/1254) and same on server, a positive constant |
| `sdk/rocketnet_fragment.cpp:653` | guarded | callers guard: if ( server_transport->send_arena ) (server_transport.cpp:1870), if ( client_transport->send_arena ) (client_transport.cpp:1979), test hook :3626 |
| `sdk/rocketnet_fragment.cpp:661` | guarded | send_arena always set by create, which fails when null (server_transport.cpp:1801, client_transport.cpp:1262) |
| `sdk/rocketnet_fragment.cpp:672` | guarded | send_arena always set by create, which fails when null (server_transport.cpp:1801, client_transport.cpp:1262) |
| `sdk/rocketnet_fragment.cpp:678` | guarded | send_arena always set by create, which fails when null (server_transport.cpp:1801, client_transport.cpp:1262) |
| `sdk/rocketnet_network_simulator.cpp:82` | out of class | comment |
| `sdk/rocketnet_network_simulator.cpp:241` | out of class | comment |
| `sdk/rocketnet_network_simulator.cpp:260` | guarded | real check: if ( capacity <= 0 ) return NULL (network_simulator.cpp:262) |
| `sdk/rocketnet_network_simulator.cpp:338` | guarded | callers guard: server_transport.cpp:1874, client_transport.cpp:1972, server_transport.cpp:1677 after create null check :1666, test hook :3624 |
| `sdk/rocketnet_network_simulator.cpp:360` | guarded | called only right after create's null check (server_transport.cpp:1666-1682, client_transport.cpp:1662-1669) |
| `sdk/rocketnet_network_simulator.cpp:427` | guarded | callers guard !link_simulator (server_transport.cpp:690, client_transport.cpp:3372) |
| `sdk/rocketnet_network_simulator.cpp:428` | guarded | mutex is a member address (&server_transport->link_simulator_mutex, &client_transport->mutex) |
| `sdk/rocketnet_network_simulator.cpp:448` | guarded | callers guard !link_simulator (server_transport.cpp:647, client_transport.cpp:467) |
| `sdk/rocketnet_network_simulator.cpp:449` | guarded | mutex is a member address (server_transport.cpp:649, client_transport.cpp:470) |
| `sdk/rocketnet_network_simulator.cpp:484` | guarded | called only after create's null check (server_transport.cpp:1666/1687, client_transport.cpp:1662/1674), test hook after create |
| `sdk/rocketnet_network_simulator.cpp:485` | guarded | config walk refuses drop_every_nth outside 0 or 2..1000000 (rocketnet_internal.h:465, :543); use is also guarded by >= 2 (network_simulator.cpp:959), not an index |
| `sdk/rocketnet_network_simulator.cpp:486` | guarded | config walk refuses reverse_window outside 0 or 2..64 (rocketnet_internal.h:466, :544); window is malloc'd with reverse_window entries (network_simulator.cpp:495) and flushed at window_count >= reverse_window (:916) |
| `sdk/rocketnet_network_simulator.cpp:510` | guarded | caller guards !transport->link_simulator (server_transport.cpp:735) |
| `sdk/rocketnet_network_simulator.cpp:516` | guarded | callers are inside link_active's null guard (server_transport.cpp:647-650, :720) or created_active's null guard (network_simulator.cpp created_active returns false on NULL) |
| `sdk/rocketnet_network_simulator.cpp:528` | guarded | callers guard null (server_transport.cpp:735, client_transport.cpp:727/746) |
| `sdk/rocketnet_network_simulator.cpp:529` | guarded | out_from is &from, a local (server_transport.cpp:742, client_transport.cpp:732/752) |
| `sdk/rocketnet_network_simulator.cpp:530` | guarded | out_to is &to, a local (same call sites) |
| `sdk/rocketnet_network_simulator.cpp:531` | guarded | out_packet is uint8_t packet[ROCKETNET_RECEIVE_CEILING] local (server_transport.cpp:740, client_transport.cpp:730/750); stored bytes <= RECEIVE_CEILING |
| `sdk/rocketnet_network_simulator.cpp:559` | out of class | rocketnet_network_simulator_audit is called only by test/soak harnesses (soak_sim.cpp, tests/), not the library or public API |
| `sdk/rocketnet_network_simulator.cpp:843` | out of class | inject_index_fault is called only by test/soak harnesses (soak_sim.cpp, tests/), negative control |
| `sdk/rocketnet_network_simulator.cpp:908` | guarded | window_count reset to 0 by patterns (network_simulator.cpp:489) and flush (:942), and flushed whenever it reaches reverse_window (:916); window has reverse_window entries (:495), reverse_window <= 64 by config walk (rocketnet_internal.h:466/544) |
| `sdk/rocketnet_network_simulator.cpp:924` | guarded | callers guard: server link_active null check (server_transport.cpp:720 via :647), client link_patterns null check (client_transport.cpp:453 for :494), client_transport.cpp:746 |
| `sdk/rocketnet_network_simulator.cpp:952` | guarded | callers guard: server link_active (server_transport.cpp:647/659), client :707/:711, client hold under link_active (client_transport.cpp:2153) |
| `sdk/rocketnet_network_simulator.cpp:953` | guarded | from is &transport->link_simulator_from member (server_transport.cpp:673, client_transport.cpp:717/795) |
| `sdk/rocketnet_network_simulator.cpp:954` | guarded | to is the send's destination address (non-null address struct at every transport send site) |
| `sdk/rocketnet_network_simulator.cpp:955` | guarded | packet_data is the transport's own frame/packet buffer at every site (server_transport.cpp:673, client_transport.cpp:717/796) |
| `sdk/rocketnet_network_simulator.cpp:956` | guarded | every packet handed in is self-built: sealed packets bounded by real payload checks (protocol.cpp:192-194, server_protocol.cpp:1945/1991/2110-2112, client_protocol.cpp:1452/1496/1580-1582) to <= ROCKETNET_SEND_BUDGET == ROCKETNET_RECEIVE_CEILING (wire.h:83); handshake packets static-asserted <= 1452 (wire.h:134-137); client hold skips wire_bytes 0 (client_transport.cpp:794) |
| `sdk/rocketnet_network_simulator.cpp:983` | out of class | deliver_packets is called only by test/soak harnesses (tests/, soak_sim.cpp); it also re-checks each packet (network_simulator.cpp:993) |
| `sdk/rocketnet_network_simulator.cpp:984` | out of class | deliver_packets test/soak-only caller |
| `sdk/rocketnet_network_simulator.cpp:985` | out of class | deliver_packets test/soak-only caller |
| `sdk/rocketnet_network_simulator.cpp:986` | out of class | deliver_packets test/soak-only caller; per-entry null check at network_simulator.cpp:993 |
| `sdk/rocketnet_network_simulator.cpp:987` | out of class | deliver_packets test/soak-only caller |
| `sdk/rocketnet_network_simulator.cpp:988` | out of class | deliver_packets test/soak-only caller; negative count just skips the loop |
| `sdk/rocketnet_network_simulator.cpp:1007` | out of class | rocketnet_network_simulator_receive is called only by test/soak harnesses (soak_sim.cpp, tests/, functional/channel_harness.h, fuzz/) |
| `sdk/rocketnet_network_simulator.cpp:1008` | out of class | receive test/soak-only caller |
| `sdk/rocketnet_network_simulator.cpp:1009` | out of class | receive test/soak-only caller |
| `sdk/rocketnet_network_simulator.cpp:1010` | out of class | receive test/soak-only caller |
| `sdk/rocketnet_protocol.cpp:50` | out of class | rocketnet_keyed_filter_value has no non-test caller in sdk |
| `sdk/rocketnet_protocol.cpp:51` | out of class | rocketnet_keyed_filter_value has no non-test caller in sdk |
| `sdk/rocketnet_protocol.cpp:52` | out of class | rocketnet_keyed_filter_value has no non-test caller in sdk |
| `sdk/rocketnet_protocol.cpp:85` | guarded | the default path still returns a valid named counter (protocol.cpp:86); callers pass only negative prefilter_check results (client_protocol.cpp:1329-1332, server_protocol.cpp:1426) |
| `sdk/rocketnet_protocol.cpp:137` | guarded | only callers (protocol.cpp:210/266/328) pass the local associated_data[ROCKETNET_AD_WITH_EPHEMERAL_BYTES] array |
| `sdk/rocketnet_protocol.cpp:138` | guarded | connection_salt is a struct array member at every caller (client->connection_salt, mapping->connection_salt, burst->connection_salt, local connection_salt pointer into the request packet server_protocol.cpp:334 area) |
| `sdk/rocketnet_protocol.cpp:180` | guarded | wire_start is a frame inside the transport's ring/arena at both callers (server_protocol.cpp:1958, client_protocol.cpp:1465) |
| `sdk/rocketnet_protocol.cpp:181` | guarded | packet_filter is a struct array member (server->packet_filter, client->packet_filter) |
| `sdk/rocketnet_protocol.cpp:182` | guarded | ack_block is a local array (server_protocol.cpp:1952, client_protocol.cpp:1459) |
| `sdk/rocketnet_protocol.cpp:183` | guarded | connection_salt is mapping->/client-> array member (server_protocol.cpp:1961, client_protocol.cpp:1468) |
| `sdk/rocketnet_protocol.cpp:184` | guarded | key is mapping->keys.session_server_to_client / client->keys.session_client_to_server, array members |
| `sdk/rocketnet_protocol.cpp:238` | guarded | packet_data is out_packet / frame / stored_response / response_packet buffers owned by the caller at every site (server_transport.cpp:3210, client_protocol.cpp:305/583/1509/1599, server_protocol.cpp:689/763/854/1681/2008/2129) |
| `sdk/rocketnet_protocol.cpp:239` | guarded | packet_filter is server->/client->packet_filter array member at every site |
| `sdk/rocketnet_protocol.cpp:240` | guarded | connection_salt is an array member or local at every site (burst->, client->, mapping->, server_protocol.cpp:691 connection_salt) |
| `sdk/rocketnet_protocol.cpp:241` | guarded | key is an array member at every site (keys.*, disconnect_burst_keys[index], local keys struct server_protocol.cpp:692) |
| `sdk/rocketnet_protocol.cpp:295` | guarded | out_data is a local/array at every caller (client_protocol.cpp:528/648/663/803, server_protocol.cpp:1155) with out_capacity checked (protocol.cpp:316-318) |
| `sdk/rocketnet_protocol.cpp:296` | guarded | packet_data is the received packet buffer at every caller; length checked protocol.cpp:316 |
| `sdk/rocketnet_protocol.cpp:297` | guarded | connection_salt is client->/mapping-> array member at every caller |
| `sdk/rocketnet_protocol.cpp:298` | guarded | key is an array member at every caller (client->keys.*, temporary_keys.*, server key from mapping->keys.* server_protocol.cpp:1108) |
| `sdk/rocketnet_protocol.cpp:353` | guarded | only caller passes out_packet buffer (client_protocol.cpp:372) |
| `sdk/rocketnet_protocol.cpp:354` | guarded | client->packet_filter array member (client_protocol.cpp:372) |
| `sdk/rocketnet_protocol.cpp:355` | guarded | client->connection_salt array member (client_protocol.cpp:374) |
| `sdk/rocketnet_protocol.cpp:356` | guarded | client->client_ephemeral_public_key array member (client_protocol.cpp:375) |
| `sdk/rocketnet_protocol.cpp:357` | guarded | client->token.signed_connect_data array member (client_protocol.cpp:376) |
| `sdk/rocketnet_protocol.cpp:358` | guarded | token signature array member (client_protocol.cpp:377) |
| `sdk/rocketnet_protocol.cpp:370` | guarded | every write is a fixed compile-time size; total is the constant ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES, static-asserted to fit (wire.h:134) |
| `sdk/rocketnet_protocol.cpp:379` | guarded | only caller passes response_packet buffer (server_protocol.cpp:502) |
| `sdk/rocketnet_protocol.cpp:380` | guarded | server->packet_filter array member (server_protocol.cpp:502) |
| `sdk/rocketnet_protocol.cpp:381` | guarded | connection_salt points into the received request after its length was checked (server_protocol.cpp:502) |
| `sdk/rocketnet_protocol.cpp:382` | guarded | fresh_mac local array (server_protocol.cpp:495/502) |
| `sdk/rocketnet_protocol.cpp:389` | guarded | every write is a fixed compile-time size; total is the constant ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES |
| `sdk/rocketnet_protocol.cpp:435` | guarded | output is a local array at both callers (protocol.cpp:469 input[], encryption_map.cpp:165 address_bytes[]) |
| `sdk/rocketnet_protocol.cpp:436` | guarded | address is the received packet's from address or &map->address[index] (encryption_map.cpp:184/231/271, server_protocol.cpp:461/468/495 via cookie mac) |
| `sdk/rocketnet_protocol.cpp:466` | guarded | only caller passes local input[ROCKETNET_COOKIE_MAC_INPUT_IPV6_BYTES] (protocol.cpp:489-490) |
| `sdk/rocketnet_protocol.cpp:467` | guarded | connection_salt points into the received request (server_protocol.cpp:461/468/495) |
| `sdk/rocketnet_protocol.cpp:486` | guarded | mac is a local array (expected_mac / fresh_mac, server_protocol.cpp:461/468/495) |
| `sdk/rocketnet_protocol.cpp:487` | guarded | cookie_key is server->cookie_key / server->previous_cookie_key array members (server_protocol.cpp:461/468/495) |
| `sdk/rocketnet_network_simulator.h:126` | out of class | comment |
| `sdk/rocketnet_channel.cpp:38` | guarded | both callers check room first: unreliable path channel.cpp:416-417 (deliveries >= delivery_capacity returns), reliable path rn_channels_reliable_delivery channel.cpp:52 (exhausts and returns NULL) |
| `sdk/rocketnet_channel.cpp:1348` | guarded | ready-list invariant: a channel is pushed only when its queue becomes non-empty (channel.cpp:1001-1009, 1075-1083) and re-pushed after a dequeue only if queue_head != NONE (channel.cpp:1375-1377); loop guard ready_head != NONE channel.cpp:1343 |
| `sdk/rocketnet_config.cpp:71` | guarded | rocketnet_api_null_guard( config ) returns first at config.cpp:69 |
| `sdk/rocketnet_config.cpp:178` | guarded | internal (not in rocketnet.h); every caller passes &local: config.cpp:205, api.cpp:148 |
| `sdk/rocketnet_config.cpp:312` | guarded | internal; every caller passes &local: api.cpp:261, api.cpp:613, client_transport.cpp:1112 |
| `sdk/rocketnet_config.cpp:319` | guarded | internal; every caller passes &local: api.cpp:347, api.cpp:1466, server_transport.cpp:1171 |
| `sdk/rocketnet_config.cpp:337` | guarded | no production caller (declared rocketnet_config.h:51, no sdk call site); buffer length checked at config.cpp:346 |
| `sdk/rocketnet_config.cpp:338` | guarded | no production caller (rocketnet_config.h:51 only); buffer_size bound checked config.cpp:346 before write |
| `sdk/rocketnet_config.cpp:363` | guarded | callers pass string literals or static field-table names: config.cpp:497 (from apply_env_uint64 literals), config.cpp:644 (field->name from static tables) |
| `sdk/rocketnet_config.cpp:364` | guarded | field_name from static field tables or literals (config.cpp:497, config.cpp:644) |
| `sdk/rocketnet_config.cpp:365` | guarded | buffer is a local env_name[128] at config.cpp:496 and config.cpp:643; bound checked config.cpp:376 |
| `sdk/rocketnet_config.cpp:492` | guarded | callers pass literals "server"/"client"/side: server_transport.cpp:1340, client_transport.cpp:1188, api.cpp:221 |
| `sdk/rocketnet_config.cpp:493` | guarded | field_name is the literal "memory_budget_bytes" at all three callers (server_transport.cpp:1340, client_transport.cpp:1188, api.cpp:221) |
| `sdk/rocketnet_config.cpp:494` | guarded | out is &local at all callers (server_transport.cpp:1340, client_transport.cpp:1188, api.cpp:221) |
| `sdk/rocketnet_config.cpp:633` | guarded | transport_name literal at callers server_transport.cpp:1173, client_transport.cpp:1114, api.cpp:153/156/220 |
| `sdk/rocketnet_config.cpp:634` | guarded | config is &effective_config or a local struct at server_transport.cpp:1173, client_transport.cpp:1114, api.cpp:153/156/220 |
| `sdk/rocketnet_config.cpp:635` | guarded | fields are static tables from *_config_fields() (config.cpp:310-321, 176-181) or static budget rows at every caller |
| `sdk/rocketnet_config.cpp:678` | guarded | transport_name literal at callers server_transport.cpp:1180, client_transport.cpp:1121, api.cpp:158/284/360/614/1467, config.cpp:206 |
| `sdk/rocketnet_config.cpp:679` | guarded | config is &effective_config/&local or a null-replaced config (config.cpp:197-201) at every caller |
| `sdk/rocketnet_config.cpp:680` | guarded | fields are static field tables at every caller (config.cpp:205, server_transport.cpp:1171, client_transport.cpp:1112, api.cpp) |
| `sdk/rocketnet_config.cpp:799` | guarded | side is a literal at client_transport.cpp:1135, server_transport.cpp:1194, api.cpp:618, api.cpp:1471 |
| `sdk/rocketnet_config.cpp:800` | guarded | filter is the embedded array member packet_filter of a config struct at the same four callers |
| `sdk/rocketnet_config.cpp:810` | guarded | transport_name literal at server_transport.cpp:1187, client_transport.cpp:1128, api.cpp:616, api.cpp:1469 |
| `sdk/rocketnet_config.cpp:811` | guarded | config is &effective_config or validated transport struct at the same callers |
| `sdk/rocketnet_config.cpp:812` | guarded | fields are static field tables at the same callers |
| `sdk/rocketnet_config.cpp:873` | guarded | available is the static lists array returned by rocketnet_*_transport_io_availability (driver.cpp:129-138), never NULL |
| `sdk/rocketnet_config.cpp:874` | guarded | num_available is only a loop bound (config.cpp:900, 942, 1005); zero yields an empty ladder, no index or divisor |
| `sdk/rocketnet_config.cpp:876` | guarded | ladder is &local at every caller (api.cpp:627, api.cpp:1485, client_transport.cpp:1174, server_transport.cpp:1264) |
| `sdk/rocketnet_message.cpp:106` | guarded | only caller passes &local sequenced_shape (channel.cpp:981-986) |
| `sdk/rocketnet_message.cpp:167` | guarded | writer is &local or embedded build->writer at every caller (packet_builder.cpp:352, endpoint.cpp:552/1083, message_server.cpp:843) |
| `sdk/rocketnet_message.cpp:168` | guarded | payload null-checked on the in-place path (endpoint.cpp:513); other callers pass transport frame/payload buffers (packet_builder.cpp:352, message_server.cpp:843, endpoint.cpp:1083) |
| `sdk/rocketnet_message.cpp:169` | guarded | a negative capacity clamps to zero space in rocketnet_message_writer_space (message.cpp:180), so put refuses at message.cpp:206 and finish returns 0 at message.cpp:261 |
| `sdk/rocketnet_message.cpp:178` | guarded | writer is &local/embedded at every caller (see message.cpp:167 callers, message_server.cpp:829/1019) |
| `sdk/rocketnet_message.cpp:199` | guarded | static; writer is &local/embedded at every caller via writer_add/add_in_place |
| `sdk/rocketnet_message.cpp:200` | guarded | real check: if ( !rocketnet_message_record_valid( record ) ) return 0 at message.cpp:201-202 (record_valid also NULL-checks at :112) |
| `sdk/rocketnet_message.cpp:234` | out of class | postcondition that the header written equals the computed header; the space check at message.cpp:206 precedes every write |
| `sdk/rocketnet_message.cpp:260` | guarded | writer &local/embedded at every caller (packet_builder.cpp:428, endpoint.cpp:554/1084, message_server.cpp:349) |
| `sdk/rocketnet_message.cpp:263` | guarded | put admits only when header + bytes <= capacity - 1 - writer->bytes (message.cpp:179, 206) and writer fields are never mutated outside put; records == 0 returns at message.cpp:261 |
| `sdk/rocketnet_message.cpp:278` | guarded | only caller rn_endpoint_in_place_finish returns on !payload at endpoint.cpp:513 |
| `sdk/rocketnet_message.cpp:438` | guarded | limits is &ordered->limits (channel.cpp:1479), an embedded member, via reader_begin (message.cpp:480) |
| `sdk/rocketnet_message.cpp:468` | out of class | postcondition; decode_record refuses length > remaining - header (message.cpp:358) so offset never overshoots, and offset is not used after |
| `sdk/rocketnet_message.cpp:474` | guarded | reader is &local at the only caller (channel.cpp:1479) |
| `sdk/rocketnet_message.cpp:475` | guarded | limits is &ordered->limits at channel.cpp:1479 |
| `sdk/rocketnet_message.cpp:487` | guarded | reader &local at channel.cpp:1487 |
| `sdk/rocketnet_message.cpp:488` | guarded | record &local at channel.cpp:1487 |
| `sdk/rocketnet_message.cpp:512` | guarded | real check: if ( violation != NONE ) { reader->violation = violation; return 0; } at message.cpp:513-517 |
| `sdk/rocketnet_message.cpp:531` | guarded | output = rn_region_send_write_pointer of a descriptor just allocated after d != RN_DESCRIPTOR_NONE check (channel.cpp:1059-1062); write_pointer returns NULL only for a not-in-use or chunkless descriptor (allocator.cpp:921-927) |
| `sdk/rocketnet_message.cpp:532` | guarded | body is &ordered->config (channel.cpp:1047) |
| `sdk/rocketnet_message.cpp:544` | out of class | postcondition over a fixed-size layout, nothing indexed by it |
| `sdk/rocketnet_message.cpp:549` | guarded | body is &remote local at the only caller (channel.cpp:1100); data/bytes checked at message.cpp:550-553 |
| `sdk/rocketnet_encryption_map.cpp:78` | guarded | real check: capacity < 1 \|\| capacity > INT_MAX / 2 returns NULL at encryption_map.cpp:84 |
| `sdk/rocketnet_encryption_map.cpp:148` | guarded | callers null-check: server_transport.cpp:1854, server_replay_marks.cpp:182 (and :209 after the :178 null exit) |
| `sdk/rocketnet_encryption_map.cpp:167` | guarded | length is 0 (non-IP family, protocol.cpp:451-453) or within the local 19-byte buffer; a zero-length hash reads nothing; the divisor num_buckets >= 2 by encryption_map.cpp:84/93 |
| `sdk/rocketnet_encryption_map.cpp:181` | guarded | map always set: server create fails when encryption_map is NULL (server_transport.cpp:1801) |
| `sdk/rocketnet_encryption_map.cpp:182` | guarded | address is a received from-address or a stored map/slot address at every caller, never NULL |
| `sdk/rocketnet_encryption_map.cpp:195` | guarded | map set at create, create fails on NULL (server_transport.cpp:1801) |
| `sdk/rocketnet_encryption_map.cpp:204` | guarded | map set at create (server_transport.cpp:1801); test caller checks at server_replay_marks.cpp:178 |
| `sdk/rocketnet_encryption_map.cpp:205` | guarded | address is the packet from-address (server_protocol.cpp:731) or &local (server_replay_marks.cpp:188) |
| `sdk/rocketnet_encryption_map.cpp:206` | out of class | uniqueness invariant over addresses, not an index/size/pointer |
| `sdk/rocketnet_encryption_map.cpp:242` | guarded | map set at create (server_transport.cpp:1801) |
| `sdk/rocketnet_encryption_map.cpp:244` | out of class | liveness flag (state), the range question is the line above |
| `sdk/rocketnet_encryption_map.cpp:250` | guarded | map set at create (server_transport.cpp:1801) |
| `sdk/rocketnet_encryption_map.cpp:251` | guarded | control callers take the index from rocketnet_encryption_map_next (server_protocol.cpp:1568-1571, server_transport.cpp:402) or check < 0 first (server_transport.cpp:493); all indices come from add/find/next, which return only [0, capacity) |
| `sdk/rocketnet_encryption_map.cpp:252` | out of class | liveness flag (state) |
| `sdk/rocketnet_encryption_map.cpp:258` | guarded | map set at create (server_transport.cpp:1801) |
| `sdk/rocketnet_encryption_map.cpp:260` | out of class | liveness flag (state) |
| `sdk/rocketnet_encryption_map.cpp:267` | guarded | map set at create (server_transport.cpp:1801) |
| `sdk/rocketnet_encryption_map.cpp:268` | guarded | every remove caller has a real check: add result checked == -1 (server_protocol.cpp:732), lookup checked == -1 (server_protocol.cpp:1094) before integrity_failure (:957), iterator from map_next (:1568-1603), mapping_index >= 0 (connection_reset.cpp:88), mapping >= 0 (server_replay_marks.cpp:188-189) |
| `sdk/rocketnet_encryption_map.cpp:269` | out of class | liveness flag (state) |
| `sdk/rocketnet_encryption_map.cpp:288` | out of class | bucket-chain consistency invariant; every index walked is already in [0, capacity) |
| `sdk/rocketnet_encryption_map.cpp:305` | guarded | map set at create (server_transport.cpp:1801) |
| `sdk/rocketnet_encryption_map.cpp:306` | guarded | iterator is &local at every caller (server_protocol.cpp:1568-1569 and the transport walks) |
| `sdk/rocketnet_encryption_map.cpp:321` | guarded | map set at create (server_transport.cpp:1801) |
| `sdk/rocketnet_encryption_map.cpp:328` | guarded | test seam only (ROCKETNET_TEST_SEAMS); map set at create |
| `sdk/rocketnet_window_ack.cpp:83` | out of class | counter invariant, not used as index/size/divisor |
| `sdk/rocketnet_window_ack.cpp:133` | guarded | ack is the embedded &ordered->ack, initialised in rn_ordered_init after the region null check (channel.cpp:1147, 1158) |
| `sdk/rocketnet_window_ack.cpp:140` | out of class | FIFO class (chunk vs small) invariant, not an index bound |
| `sdk/rocketnet_window_ack.cpp:193` | guarded | the only caller rn_window_ack_commit returns on packet_id == 0 (window_ack.cpp:463) |
| `sdk/rocketnet_window_ack.cpp:208` | out of class | probe loop bounded by cap (window_ack.cpp:197); exhaustion loses an index entry, no out-of-range write |
| `sdk/rocketnet_window_ack.cpp:319` | guarded | only caller passes &ordered->ack (channel.cpp:1158) |
| `sdk/rocketnet_window_ack.cpp:320` | guarded | only caller passes &ordered->send (channel.cpp:1158) |
| `sdk/rocketnet_window_ack.cpp:321` | guarded | region null-checked before init at channel.cpp:1147 |
| `sdk/rocketnet_window_ack.cpp:322` | out of class | pointer identity relation, both set by the same caller (channel.cpp:1157-1158) |
| `sdk/rocketnet_window_ack.cpp:380` | guarded | ROCKETNET_DEV only; ack embedded (channel.cpp:1158) |
| `sdk/rocketnet_window_ack.cpp:425` | out of class | status enum value, used only to pick a counter and stored, not an index |
| `sdk/rocketnet_window_ack.cpp:428` | guarded | receipt_entries = max_message_receipts (allocator.cpp:435), table minimum 16 (rocketnet_internal.h:587), enforced by rocketnet_message_validate_config at region layout (allocator.cpp:105) |
| `sdk/rocketnet_window_ack.cpp:442` | guarded | ack embedded (&connection->ordered.ack / &endpoint->ordered.ack) |
| `sdk/rocketnet_window_ack.cpp:462` | guarded | static; callers commit_packet/commit_receipt_packet pass embedded ack |
| `sdk/rocketnet_window_ack.cpp:465` | guarded | numbers is an array at every count>0 caller (packet_builder.cpp:505 builder->packet[].numbers, window_ack.cpp:823 &number) |
| `sdk/rocketnet_window_ack.cpp:466` | guarded | serials is an array at every receipt caller (message_server.cpp:369-370, endpoint.cpp:607, endpoint.cpp:1166) |
| `sdk/rocketnet_window_ack.cpp:472` | guarded | count capped at ROCKETNET_PACKET_BUILDER_MAX_NUMBERS=16 (packet_builder.cpp:415), resend commits 1 (window_ack.cpp:823), receipt commits capped at 16 (message_server.cpp:830 flush; endpoint receipt_serials[ROCKETNET_PACKET_BUILDER_MAX_RECORDS], endpoint.h:102); no caller combines numbers and serials |
| `sdk/rocketnet_window_ack.cpp:482` | out of class | identity check; index is masked n & (W - 1) at window_ack.cpp:480 |
| `sdk/rocketnet_window_ack.cpp:505` | guarded | loop exits once the ring empties: total <= 16 (see :472) and max_refs = 16 * P (allocator.cpp:432) with P >= 64 (rocketnet_internal.h:583, validated allocator.cpp:105), so pkt_tail - pkt_head = 0 < P and 0 + total <= max_refs |
| `sdk/rocketnet_window_ack.cpp:564` | guarded | ack embedded (on_acks passes its own ack) |
| `sdk/rocketnet_window_ack.cpp:573` | guarded | sent_index entry is written only by rn_sent_index_insert with entry_index = pkt_tail % P (window_ack.cpp:524, 542) |
| `sdk/rocketnet_window_ack.cpp:575` | out of class | identity check on packet id |
| `sdk/rocketnet_window_ack.cpp:666` | guarded | ack embedded |
| `sdk/rocketnet_window_ack.cpp:678` | guarded | ack embedded |
| `sdk/rocketnet_window_ack.cpp:685` | guarded | ack embedded |
| `sdk/rocketnet_window_ack.cpp:727` | guarded | ack embedded; packet_ids null-checked at window_ack.cpp:728 |
| `sdk/rocketnet_window_ack.cpp:744` | guarded | ack embedded; numbers null-checked at window_ack.cpp:745 |
| `sdk/rocketnet_window_ack.cpp:759` | out of class | identity check; index is masked n & (W - 1) at window_ack.cpp:757 |
| `sdk/rocketnet_window_ack.cpp:772` | guarded | ack embedded |
| `sdk/rocketnet_window_ack.cpp:836` | guarded | rn_window_ack_resend has no production caller (tests only) |
| `sdk/rocketnet_window_ack.cpp:837` | guarded | rn_window_ack_resend has no production caller (tests only) |
| `sdk/rocketnet_window_ack.cpp:853` | guarded | ack embedded |
| `sdk/rocketnet_window_ack.cpp:867` | guarded | ack embedded |
| `sdk/rocketnet_window_receive.cpp:24` | guarded | region null-checked at channel.cpp:1147 before init (channel.cpp:1159); received_bits/receive_descriptors are carved at base + offset in rn_region_point (allocator.cpp:442-443) |
| `sdk/rocketnet_window_receive.cpp:25` | guarded | reliable_messages_in_flight min 64 (rocketnet_internal.h:578) and power of two (config.cpp:218), validated at region layout allocator.cpp:105 |
| `sdk/rocketnet_window_receive.cpp:36` | guarded | receive is embedded &ordered->receive with region set at init; record/slot are caller locals |
| `sdk/rocketnet_window_receive.cpp:109` | guarded | receive embedded, region set at init (channel.cpp:1159) |
| `sdk/rocketnet_window_send.cpp:40` | guarded | real check: fits requires p >= floor && p - floor <= B - span at window_send.cpp:41 |
| `sdk/rocketnet_window_send.cpp:65` | guarded | bytes16 is round16 of a single record (<= small_limit <= payload_capacity <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES 1416, channel.cpp:207 and 985-988, so <= 89 units) or a block fragment <= F=1152 (72 units) |
| `sdk/rocketnet_window_send.cpp:83` | guarded | region null-checked at channel.cpp:1147; send_numbers carved at allocator.cpp:440 |
| `sdk/rocketnet_window_send.cpp:84` | guarded | power-of-two window, config.cpp:218 validated at allocator.cpp:105 |
| `sdk/rocketnet_window_send.cpp:100` | guarded | send embedded &ordered->send; number/ring_start are &locals (channel.cpp:1361) |
| `sdk/rocketnet_window_send.cpp:103` | out of class | code enum; only compared (holdable), not an index |
| `sdk/rocketnet_window_send.cpp:104` | guarded | fragments == 1 only when bytes <= small_limit (channel.cpp:985-988) and small_limit <= payload_capacity - 7 <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES (channel.cpp:207); bytes >= 1 at channel.cpp:961 |
| `sdk/rocketnet_window_send.cpp:105` | guarded | bytes <= region->max_message_bytes refused at send start (channel.cpp:932) and pending_bytes bound at finish (channel.cpp:961); ring fit also checked at window_send.cpp:41 |
| `sdk/rocketnet_window_send.cpp:130` | guarded | send embedded; first_number/ring_start are &locals (channel.cpp:1360) |
| `sdk/rocketnet_window_send.cpp:131` | out of class | code enum, read only by the assert ((void) code) |
| `sdk/rocketnet_window_send.cpp:134` | guarded | block path only when bytes > small_limit > F (payload_capacity >= F + max header + 1, channel.cpp:206-207) and bytes <= max_message_bytes (channel.cpp:932) |
| `sdk/rocketnet_window_send.cpp:136` | guarded | count = ceil(bytes/F) <= ceil(max_message_bytes/F) <= W by config.cpp:245, validated at allocator.cpp:105 |
| `sdk/rocketnet_window_send.cpp:140` | guarded | max_message_bytes <= receive_message_buffer_bytes (config.cpp:236) with B % 16 == 0 (config.cpp:227), and admit refuses span > B at window_send.cpp:41 |
| `sdk/rocketnet_window_send.cpp:162` | guarded | send embedded, region set at init (channel.cpp:1157) |
| `sdk/rocketnet_window_send.cpp:182` | out of class | invariant over number/fragment values; U_prime is only used modulo W (window_send.cpp:38) and in subtractions guarded at :33 |
| `sdk/rocketnet_window_send.cpp:195` | guarded | send embedded, region set at init (channel.cpp:1157) |
| `functional/rocketnet_functional_tests.cpp:1906` | out of class | installs an assert hook (test code); not an assert |
| `functional/rocketnet_functional_tests.cpp:1910` | out of class | restores the assert hook (test code); not an assert |
| `sdk/rocketnet.cpp:56` | out of class | assert callback variable definition, not an assert |
| `sdk/rocketnet.cpp:58` | out of class | rocketnet_assert_function setter definition, not an assert |
| `sdk/rocketnet.h:913` | out of class | typedef of the assert callback |
| `sdk/rocketnet.h:920` | out of class | public declaration of the assert-hook setter |
| `sdk/rocketnet_address.cpp:103` | guarded | internal helper (rocketnet_address.h, not exported from rocketnet.h); every caller passes a local char array (address_string[...]) e.g. api.cpp:1932, client_transport.cpp:1352, server_transport.cpp:1640 |
| `sdk/rocketnet_address.cpp:15` | guarded | internal helper not exported in rocketnet.h; no sdk caller other than its definition (grep finds none), so no path passes NULL |
| `sdk/rocketnet_address.cpp:161` | guarded | internal helper not exported; no sdk caller outside tests, every test passes a local buffer |
| `sdk/rocketnet_address.cpp:193` | guarded | internal helper; every caller passes &member/&local (server_protocol.cpp:74,166,169; token_tables.cpp:336; encryption_map.cpp:187; client_io.h:296 from = receive path address of a datagram slot) |
| `sdk/rocketnet_address.cpp:194` | guarded | same as :193, second operand is always &member/&local |
| `sdk/rocketnet_address.cpp:230` | guarded | internal helper; only callers are unit tests passing &local (rocketnet_tests.cpp:7045,7071) |
| `sdk/rocketnet_address.cpp:254` | guarded | internal helper; callers pass &local/&member (server_protocol.cpp:161,212,227,239, address.h:117 after its own deref of from) |
| `sdk/rocketnet_address.cpp:274` | guarded | internal helper; no sdk caller outside address.cpp/tests, tests pass &local |
| `sdk/rocketnet_address.cpp:293` | guarded | internal helper; callers pass &local (address.h:117 path guards with rocketnet_address_is_ipv4_in_ipv6 before converting) |
| `sdk/rocketnet_address.cpp:294` | guarded | precondition is a real if at the only sdk call path: address.h:117 `if ( !rocketnet_address_is_ipv4_in_ipv6( from ) )` returns before convert; server_protocol.cpp:212/227/239 test it in an if first |
| `sdk/rocketnet_address.cpp:308` | guarded | internal helper not exported; no sdk caller outside tests |
| `sdk/rocketnet_address.cpp:41` | guarded | real check follows: address.cpp:43 `if ( !address ) return false;` |
| `sdk/rocketnet_address.cpp:42` | guarded | real check follows: address.cpp:46 `if ( !address_string_in ) return false;` |
| `sdk/rocketnet_api.cpp:1058` | guarded | index is returned, not used here, and it was validated off the wire at client_protocol.cpp:1073 (`client_index < 0 \|\| client_index >= max_clients \|\| max_clients > ROCKETNET_MAX_CLIENTS` refuses the keep-alive) before client_protocol.cpp:1115 stores it; otherwise -1 from init (client_protocol.cpp:111) and api.cpp:1055 returns -1 unless CONNECTED |
| `sdk/rocketnet_api.cpp:1818` | guarded | destroy of the retiring connection: both callers check first with a real if, api.cpp:1727 (`if ( !rocketnet_server_stopped( server ) ) return SERVER_NOT_STOPPED`) and api.cpp:1856/1871 (stop loop then `if ( !rocketnet_server_stopped ) return`) |
| `sdk/rocketnet_api.cpp:1855` | guarded | real check on the next line, api.cpp:1856 `if ( !rocketnet_server_stopped( server ) )` drives the stop and api.cpp:1871 returns without freeing if the worker has not exited |
| `sdk/rocketnet_api.cpp:779` | guarded | both callers check first: api.cpp:855 (`if ( !rocketnet_client_disconnected ) return CLIENT_NOT_DISCONNECTED`) and api.cpp:888/902 (drive stop, return without freeing if still not disconnected) |
| `sdk/rocketnet_api.cpp:887` | guarded | real check on the next line, api.cpp:888 `if ( !rocketnet_client_disconnected( client ) )`, and api.cpp:902 returns without freeing |
| `sdk/rocketnet_base64.cpp:13` | guarded | internal (not exported); only sdk callers pass arrays: connect_token.cpp:530 (token), config.cpp:471 (base of a config field) |
| `sdk/rocketnet_base64.cpp:14` | guarded | internal; callers pass a local/char buffer (connect_token.cpp:530 encoded, config.cpp:471 buffer) |
| `sdk/rocketnet_base64.cpp:151` | guarded | post-condition only; the writes it describes are bounded by the real refusal base64.cpp:123 `if ( output_length > output_size ) return 0;` and j counts exactly output_length bytes |
| `sdk/rocketnet_base64.cpp:158` | guarded | internal; no sdk caller of encode_string outside tests (tests pass literals) |
| `sdk/rocketnet_base64.cpp:159` | guarded | internal; no sdk caller of encode_string outside tests (tests pass arrays) |
| `sdk/rocketnet_base64.cpp:15` | guarded | real check: base64.cpp:28 `if ( output_length >= output_size ) return -1;` refuses output_size 0 before any write |
| `sdk/rocketnet_base64.cpp:160` | guarded | forwards to encode_data whose base64.cpp:28 refuses output_size 0 |
| `sdk/rocketnet_base64.cpp:167` | guarded | internal; no sdk caller of decode_string outside tests |
| `sdk/rocketnet_base64.cpp:168` | guarded | internal; no sdk caller of decode_string outside tests |
| `sdk/rocketnet_base64.cpp:169` | guarded | real check: base64.cpp:171 `if ( output_size < 2 ) return 0;` before output_size - 1 and output[...] |
| `sdk/rocketnet_base64.cpp:91` | guarded | callers pass a checked string: api.cpp:801 bounded memchr NUL scan of connect_token (null-guarded at the public entry), config.cpp:588 value from the parsed key/value |
| `sdk/rocketnet_base64.cpp:92` | guarded | callers pass arrays: api.cpp:807 token[], config.cpp:588 decoded[256] |
| `sdk/rocketnet_base64.cpp:93` | guarded | real check base64.cpp:123 `if ( output_length > output_size ) return 0;` refuses any non-empty decode into 0; callers pass ROCKETNET_CONNECT_TOKEN_BYTES or field->size (bounded at config.cpp:580) |
| `sdk/rocketnet_buffer_handoff.h:111` | guarded | every caller releases only an index it holds after a real `>= 0` test: client_io.h:406 (`if ( io->receive_held >= 0 )`), client_io.h:526 (`if ( io->send_ring >= 0 )`), client_transport.cpp:2223 returned[] filled from exposed take results (:2267); indices originate in the handoff's own free/ready arrays |
| `sdk/rocketnet_buffer_handoff.h:112` | guarded | structural conservation of N buffers (producer holds 1, released one held by consumer), tla/RNBufferHandoff.tla; free_count < N follows because only a held index (>=0 guarded, see :111) is released; all callers internal |
| `sdk/rocketnet_buffer_handoff.h:54` | guarded | callers pass constant 0 (client_io.h:169,173; client_transport.cpp:1685), NumBuffers >= 3 by static_assert at :53 |
| `sdk/rocketnet_buffer_handoff.h:76` | guarded | producer is always the last index returned by init (0) or a successful publish (next >= 0 tested at client_io.h:388, :666, client_transport.cpp:3235 before storing) |
| `sdk/rocketnet_buffer_handoff.h:77` | guarded | max_ready is a compile-time constant at every caller (NUM_RINGS-1 client_io.h:386; 1 client_io.h:664; NUM_BUFFERS-1 client_transport.cpp:3233) |
| `sdk/rocketnet_buffer_handoff.h:83` | guarded | real check at buffer_handoff.h:78 `handoff->ready_count >= max_ready` returns -1, and max_ready < NumBuffers (constants, see :77) |
| `sdk/rocketnet_counters.cpp:51` | guarded | assert(0) inside a real range check: counters.cpp:49 `if ( counter < 0 \|\| counter >= ROCKETNET_NUM_COUNTERS )` returns "???" |
| `sdk/rocketnet_driver.cpp:178` | guarded | real check on the next line, driver.cpp:179 `if ( !stopped ) return;` before destroy |
| `sdk/rocketnet_internal.h:293` | out of class | comment |
| `sdk/rocketnet_internal.h:294` | out of class | comment |
| `sdk/rocketnet_internal.h:302` | out of class | extern declaration of the assert callback |
| `sdk/rocketnet_internal.h:305` | out of class | macro definition |
| `sdk/rocketnet_internal.h:314` | out of class | macro definition (release no-op) |
| `sdk/rocketnet_internal.h:330` | out of class | comment |
| `sdk/rocketnet_internal.h:338` | guarded | inside rocketnet_api_null_guard after the real `if ( p != NULL ) return true;` at internal.h:335; the guard returns false |
| `sdk/rocketnet_internal.h:350` | out of class | comment |
| `sdk/rocketnet_jitter_tracker.h:30` | guarded | callers pass &member/&local (client_transport.cpp:315,355,413,1273,2064) |
| `sdk/rocketnet_jitter_tracker.h:38` | guarded | same callers, &member/&local |
| `sdk/rocketnet_jitter_tracker.h:45` | guarded | same callers, &member/&local |
| `sdk/rocketnet_memory_checks.h:19` | out of class | sentinel value check in a debug memory-check macro, not an index/size/pointer guard |
| `sdk/rocketnet_out_of_order_tracker.h:24` | guarded | no non-test caller in sdk/ (only rocketnet.cpp include and rocketnet_tests.cpp, which pass &local) |
| `sdk/rocketnet_out_of_order_tracker.h:32` | guarded | as :24 |
| `sdk/rocketnet_out_of_order_tracker.h:39` | guarded | as :24 |
| `sdk/rocketnet_packet_loss_tracker.h:31` | guarded | callers pass &member/&local (client_transport.cpp:314,354,412,1272,2063,2348) |
| `sdk/rocketnet_packet_loss_tracker.h:40` | guarded | as :31 |
| `sdk/rocketnet_packet_loss_tracker.h:48` | guarded | as :31 |
| `sdk/rocketnet_platform.h:946` | guarded | every mutex_guard caller passes &member (driver_loopback.cpp:220,302,400,426,464,471 `&state.mutex`) |
| `sdk/rocketnet_platform.h:960` | guarded | as :946, the helper stores the non-NULL pointer from its constructor |
| `sdk/rocketnet_tests.cpp:13051` | out of class | test code installing an assert hook |
| `sdk/rocketnet_tests.cpp:13055` | out of class | test code restoring an assert hook |
| `sdk/rocketnet_tests.cpp:15172` | out of class | test code installing an assert hook |
| `sdk/rocketnet_tests.cpp:15176` | out of class | test code restoring an assert hook |
| `sdk/rocketnet_tests.cpp:254` | out of class | test code installing an assert hook |
| `sdk/rocketnet_tests.cpp:298` | out of class | test code restoring an assert hook |
| `sdk/rocketnet_tests.cpp:7482` | out of class | comment in test code |
| `sdk/rocketnet_tests.cpp:7500` | out of class | string literal in a test export table |
| `sdk/rocketnet_tests.cpp:7625` | out of class | string literal in a test export table |
| `sdk/rocketnet_tests.cpp:7656` | out of class | string literal in a test export table |
| `sdk/rocketnet_tests.cpp:7938` | out of class | string literal in a test export table |
| `sdk/rocketnet_tests.cpp:7992` | out of class | test code installing an assert hook |
| `sdk/rocketnet_tests.cpp:8143` | out of class | test code restoring an assert hook |
| `sdk/rocketnet_tests.cpp:8169` | out of class | test code installing an assert hook |
| `sdk/rocketnet_tests.cpp:8623` | out of class | comment in test code |
| `sdk/rocketnet_tests.cpp:8634` | out of class | test code restoring an assert hook |
| `tests/rocketnet_value_tracker.h:107` | out of class | test-only harness (tests/); also backed by the real `if ( tracker->num_samples == 0 ) return` at value_tracker.h:91 |
| `tests/rocketnet_value_tracker.h:35` | out of class | test-only harness (tests/) |
| `tests/rocketnet_value_tracker.h:44` | out of class | test-only harness (tests/) |
| `tests/rocketnet_value_tracker.h:52` | out of class | test-only harness (tests/) |
| `tests/rocketnet_value_tracker.h:87` | out of class | test-only harness (tests/) |
| `tests/rocketnet_value_tracker.h:88` | out of class | test-only harness (tests/) |
| `tests/rocketnet_value_tracker.h:89` | out of class | test-only harness (tests/) |

### Pattern 2: division by a lower-case name

| file:line | judged | reason |
|---|---|---|
| `sdk/rocketnet_abi.cpp:26` | out of class | sizeof/sizeof array count in a macro |
| `sdk/rocketnet_ack.cpp:762` | guarded | ternary guard ack.cpp:761 `stats_sent_count > 0 && sent_span > 0.0` (times are clamped finite by rocketnet_clamp_update_time, so no NaN span) |
| `sdk/rocketnet_ack.cpp:766` | guarded | ternary guard ack.cpp:765 `stats_acked_count > 0 && acked_span > 0.0` |
| `sdk/rocketnet_ack.cpp:773` | guarded | ternary guard ack.cpp:772 `stats_received_count > 0 && received_span > 0.0` |
| `sdk/rocketnet_channel.cpp:1119` | out of class | sizeof/sizeof loop bound |
| `sdk/rocketnet_allocator.cpp:125` | guarded | divisor payload_bytes = typical_payload[c], a constant table {128,512,1024} at allocator.cpp:117 |
| `sdk/rocketnet_allocator.cpp:428` | out of class | division by sizeof(rn_send_descriptor_t) |
| `sdk/rocketnet_allocator.cpp:1032` | guarded | ternary guard allocator.cpp:1032 `total > 0 ?` |
| `sdk/rocketnet_client_protocol.cpp:98` | out of class | sizeof/sizeof array count |
| `sdk/rocketnet_client_protocol.cpp:346` | guarded | client->send_rate is never config-derived: set to the constant 10.0 at client_protocol.cpp:108 and preserved across reset (connection_reset.cpp:35/52); no other writer |
| `sdk/rocketnet_client_slots.cpp:324` | out of class | sizeof/sizeof array count |
| `sdk/rocketnet_client_slots.cpp:325` | out of class | sizeof/sizeof array count |
| `sdk/rocketnet_config.cpp:834` | out of class | sizeof/sizeof loop bound |
| `sdk/rocketnet_driver.cpp:32` | out of class | static_assert over sizeof/sizeof |
| `sdk/rocketnet_client_transport.cpp:281` | out of class | sizeof/sizeof array count |
| `sdk/rocketnet_client_transport.cpp:282` | out of class | sizeof/sizeof array count |
| `sdk/rocketnet_client_transport.cpp:2282` | out of class | compile-time constant divisor (ROCKETNET_NUM_CLIENT_STATS_FRAMES - 1) |
| `sdk/rocketnet_counters.cpp:100` | out of class | static_assert over sizeof/sizeof |
| `sdk/rocketnet_fragment.cpp:66` | guarded | count clamped to >= 2 by the real `if ( count < 2 ) count = 2;` at fragment.cpp:62-63 |
| `sdk/rocketnet_frame_sync.cpp:101` | guarded | no assert involved; denom from mach_timebase_info (OS contract: never 0), read once at frame_sync.cpp:73-81 |
| `sdk/rocketnet_frame_sync.cpp:108` | guarded | no assert involved; numer from mach_timebase_info (OS contract: never 0) |
| `sdk/rocketnet_frame_sync.cpp:142` | guarded | no assert involved; QueryPerformanceFrequency is documented non-zero on XP and later (frame_sync.cpp:123-131) |
| `sdk/rocketnet_frame_sync.cpp:219` | guarded | no assert involved; numer from mach_timebase_info (OS contract: never 0) |
| `sdk/rocketnet_frame_sync.cpp:337` | guarded | inside `if ( error > period_ns )` and period_ns = 1e9/frame_rate with frame_rate refused outside (0, 1000] at frame_sync.cpp:372, so period_ns >= 1,000,000; the only caller is frame_sync.cpp:498 passing frame_sync->period_ns |
| `sdk/rocketnet_frame_sync.cpp:397` | guarded | real check frame_sync.cpp:372 `if ( !( frame_rate > 0.0 ) \|\| frame_rate > ROCKETNET_FRAME_SYNC_MAX_FRAME_RATE )` refuses 0, negatives and NaN before the divide |
| `sdk/rocketnet_server_replay_marks.cpp:165` | out of class | sizeof/sizeof array count |
| `sdk/rocketnet_server_replay_marks.cpp:166` | out of class | sizeof/sizeof array count |
| `sdk/rocketnet_server_rio_windows.cpp:334` | guarded | stride = round_up64(layout.frame_bytes, 64); layout planned from the constant ROCKETNET_FRAME_STRIDE (rio_windows.cpp:648/1129) and rio_layout.h:58 refuses frame_bytes <= 0, so stride >= 64; rio_windows.cpp:327 returns when region is unset |
| `sdk/rocketnet_server_rio_windows.cpp:338` | guarded | same stride as :334, and :334 already returned on any offset not a multiple of it |
| `sdk/rocketnet_platform.cpp:1939` | out of class | SIZE_MAX / sizeof(T) |
| `sdk/rocketnet_server_protocol.cpp:1803` | guarded | ternary clamp server_protocol.cpp:1799 `server->send_rate > 0.0 ? server->send_rate : 10.0` (and send_rate is the constant ROCKETNET_DEFAULT_SEND_RATE, server_transport.cpp:1703) |
| `sdk/rocketnet_server_socket_linux.cpp:2139` | out of class | pointer difference / sizeof |
| `sdk/rocketnet_coalesce.h:43` | guarded | real clamp coalesce.h:40-41 `if ( segment_bytes <= 0 ) segment_bytes = message_bytes;` after :37 returned for message_bytes <= 0 |
| `sdk/rocketnet_server_transport.cpp:363` | out of class | sizeof/sizeof array count |
| `sdk/rocketnet_frame_sync.h:28` | out of class | #define and comment, not a division |
| `sdk/rocketnet_memory.h:28` | guarded | guarded in the same expression: `if (stride && ...)` at memory.h:28 |
| `sdk/rocketnet_tests.cpp:690` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_tests.cpp:700` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_tests.cpp:968` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_tests.cpp:1041` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_tests.cpp:1546` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_tests.cpp:5079` | out of class | test code |
| `sdk/rocketnet_tests.cpp:6106` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_tests.cpp:8132` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_tests.cpp:8133` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_tests.cpp:8134` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_tests.cpp:8591` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_tests.cpp:8592` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_tests.cpp:8593` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_tests.cpp:8653` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_tests.cpp:12662` | out of class | test code, sizeof divisor |
| `sdk/rocketnet_tests.cpp:12837` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_tests.cpp:12841` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_tests.cpp:15007` | out of class | test code, sizeof/sizeof |
| `sdk/rocketnet_replay.h:29` | out of class | sizeof/sizeof in a constant expression |
| `functional/channel_resend_timing.cpp:517` | out of class | functional test harness, not library code |
| `functional/main.cpp:165` | out of class | functional test harness, sizeof/sizeof |
| `sdk/rocketnet_rio_layout.h:73` | guarded | frame_stride = round_up64(frame_bytes, 64) with frame_bytes <= 0 refused at rio_layout.h:58, so >= 64 |
| `sdk/rocketnet_rio_layout.h:76` | guarded | addr_stride >= 64: address_bytes <= 0 refused at rio_layout.h:58 |
| `sdk/rocketnet_rio_layout.h:79` | guarded | slice_stride >= 64: receive_slice_bytes <= 0 refused at rio_layout.h:58 |
| `sdk/rocketnet_rio_layout.h:82` | guarded | addr_stride >= 64: address_bytes <= 0 refused at rio_layout.h:58 |
| `functional/posix_lifecycle_stress.cpp:193` | out of class | functional test harness |
| `functional/posix_lifecycle_stress.cpp:346` | out of class | functional test harness, sizeof/sizeof |
| `functional/posix_lifecycle_stress.cpp:958` | out of class | functional test harness |
| `functional/posix_socket_errors.cpp:2143` | out of class | functional test harness |
| `functional/posix_socket_errors.cpp:2144` | out of class | functional test harness |
| `functional/channel_ordered.cpp:109` | out of class | functional test harness (and ternary-guarded) |
| `functional/windows_socket_errors.cpp:100` | out of class | functional test harness, sizeof/sizeof |
| `sdk/rocketnet_stats_window.h:14` | out of class | compile-time constant divisor N-1 with static_assert( N > 1 ) at stats_window.h:12 |
| `tests/dst_checks.h:6` | out of class | comment |
| `tests/dst_harness.h:1354` | out of class | test harness, sizeof/sizeof |
| `functional/live_matrix.cpp:919` | out of class | functional test harness, sizeof/sizeof |
| `functional/live_matrix.cpp:920` | out of class | functional test harness, sizeof/sizeof |
| `functional/live_matrix.cpp:1032` | out of class | functional test harness, sizeof/sizeof |
| `functional/posix_hard_killed_client.cpp:1230` | out of class | functional test harness, sizeof/sizeof |
| `functional/posix_hard_killed_client.cpp:1244` | out of class | functional test harness, sizeof/sizeof |
| `functional/posix_hard_killed_client.cpp:1245` | out of class | functional test harness, sizeof/sizeof |
| `functional/channel_unreliable_packing.cpp:44` | out of class | functional test harness |
| `tests/live_verify.h:11` | out of class | comment |
| `functional/rocketnet_functional_tests.cpp:9949` | out of class | functional test harness, sizeof/sizeof |
| `tests/suite_runner.h:177` | out of class | test runner harness |
| `tests/suite_runner.h:263` | out of class | test runner harness, sizeof/sizeof |
| `tests/suite_runner.h:420` | out of class | test runner harness |
| `tests/serialize.h:499` | out of class | test-tree serializer (tests/), not the sdk library |
| `tests/serialize.h:544` | out of class | test-tree serializer operator |
| `tests/serialize.h:761` | out of class | test-tree serializer operator |
| `tests/serialize.h:2515` | out of class | comment |
| `tests/serialize.h:2519` | out of class | comment |
| `tests/serialize.h:2530` | out of class | test-tree serializer (tests/), not the sdk library |
| `tests/serialize.h:2585` | out of class | test-tree serializer (tests/), not the sdk library |
| `tests/serialize.h:2639` | out of class | test-tree serializer (tests/), not the sdk library |
| `tests/serialize.h:4605` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:4690` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:4733` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:5090` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:5124` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:5145` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:5639` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:6058` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:6079` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:6101` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:6451` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:6537` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:6569` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:6591` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:6617` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:6819` | out of class | test code, constant operands |
| `tests/serialize.h:6822` | out of class | test code, constant operands |
| `tests/serialize.h:7085` | out of class | test code (zero divisors excluded by the test itself) |
| `tests/serialize.h:7159` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:7256` | out of class | test code |
| `tests/serialize.h:7308` | out of class | test code |
| `tests/serialize.h:7466` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:7471` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:7477` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:7482` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:7537` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:7543` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:7549` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:7570` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:7620` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:8029` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:8061` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:8668` | out of class | test-tree serializer (tests/), not the sdk library |
| `tests/serialize.h:8692` | out of class | test-tree serializer (tests/), not the sdk library |
| `tests/serialize.h:8722` | out of class | test-tree serializer (tests/), not the sdk library |
| `tests/serialize.h:8805` | out of class | test-tree serializer (tests/), not the sdk library |
| `tests/serialize.h:8822` | out of class | test-tree serializer (tests/), not the sdk library |
| `tests/serialize.h:9069` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:9097` | out of class | test code |
| `tests/serialize.h:9149` | out of class | test code, sizeof/sizeof |
| `tests/serialize.h:9161` | out of class | test code, sizeof/sizeof |

### Pattern 3: modulo

| file:line | judged | reason |
|---|---|---|
| `sdk/rocketnet_ack.cpp:54` | out of class | compile-time constant modulus ROCKETNET_REPLAY_WINDOW_BITS |
| `sdk/rocketnet_ack.cpp:354` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:360` | out of class | compile-time constant modulus ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:398` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:408` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:436` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:505` | out of class | ring is the constant ROCKETNET_SENT_PACKET_BUFFER_SIZE (ack.cpp:482) |
| `sdk/rocketnet_ack.cpp:536` | out of class | ring is the constant ROCKETNET_SENT_PACKET_BUFFER_SIZE (ack.cpp:482) |
| `sdk/rocketnet_ack.cpp:550` | out of class | ring is the constant ROCKETNET_SENT_PACKET_BUFFER_SIZE (ack.cpp:482) |
| `sdk/rocketnet_ack.cpp:563` | out of class | ring is the constant ROCKETNET_SENT_PACKET_BUFFER_SIZE (ack.cpp:482) |
| `sdk/rocketnet_ack.cpp:576` | out of class | ring is the constant ROCKETNET_SENT_PACKET_BUFFER_SIZE (ack.cpp:482) |
| `sdk/rocketnet_ack.cpp:577` | out of class | ring is the constant ROCKETNET_SENT_PACKET_BUFFER_SIZE (ack.cpp:482) |
| `sdk/rocketnet_ack.cpp:596` | out of class | compile-time constant modulus ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:641` | out of class | compile-time constant modulus ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:701` | out of class | ring is the constant ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE (ack.cpp:674) |
| `sdk/rocketnet_ack.cpp:702` | out of class | ring is the constant ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE (ack.cpp:674) |
| `sdk/rocketnet_ack.cpp:792` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:794` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:835` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:890` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:891` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:908` | out of class | compile-time constant modulus ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:909` | out of class | compile-time constant modulus ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:934` | out of class | compile-time constant modulus ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:935` | out of class | compile-time constant modulus ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:938` | out of class | compile-time constant modulus ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:1098` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:1099` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:1116` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:1151` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:1175` | out of class | compile-time constant modulus ROCKETNET_RTT_HISTORY_SIZE |
| `sdk/rocketnet_ack.cpp:1190` | out of class | compile-time constant modulus ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:1191` | out of class | compile-time constant modulus ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:1253` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:1254` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:1256` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:1370` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:1371` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:1372` | out of class | compile-time constant modulus ROCKETNET_SENT_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:1447` | out of class | compile-time constant modulus ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:1448` | out of class | compile-time constant modulus ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_ack.cpp:1449` | out of class | compile-time constant modulus ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE |
| `sdk/rocketnet_channel.cpp:1211` | guarded | region->ring_bytes = config->receive_message_buffer_bytes (allocator.cpp:427), whose table minimum is 4096 (internal.h:579) enforced by rocketnet_config_validate (config.cpp:206) inside rn_region_layout (allocator.cpp:105), which both arena creates run before carving (allocator.cpp:1216 via :1264 and :1308); same divisor as the filed audit A #3 |
| `sdk/rocketnet_api.cpp:702` | out of class | compile-time constant modulus ROCKETNET_ALLOCATION_ALIGNMENT |
| `sdk/rocketnet_api.cpp:1642` | out of class | compile-time constant modulus ROCKETNET_ALLOCATION_ALIGNMENT |
| `sdk/rocketnet_client_slots.cpp:165` | guarded | num_buckets = max_clients * 2 (client_slots.cpp:79) after the real refusal `if ( max_clients < 1 \|\| max_clients > INT_MAX / 2 ) return NULL;` at client_slots.cpp:69 |
| `sdk/rocketnet_client_transport.cpp:2284` | out of class | compile-time constant modulus ROCKETNET_NUM_CLIENT_STATS_FRAMES |
| `sdk/rocketnet_client_transport.cpp:2314` | out of class | compile-time constant modulus ROCKETNET_NUM_CLIENT_STATS_FRAMES |
| `sdk/rocketnet_endpoint.cpp:1311` | guarded | region->packets = config->packets_in_flight (allocator.cpp:429), table minimum 64 (internal.h:583) enforced by rn_region_layout -> rocketnet_message_validate_config (allocator.cpp:105, config.cpp:206) before any region is carved |
| `sdk/rocketnet_frame_sync.cpp:56` | out of class | compile-time constant modulus ROCKETNET_NANOS_PER_SECOND |
| `sdk/rocketnet_frame_sync.cpp:60` | out of class | compile-time constant modulus ROCKETNET_NANOS_PER_SECOND |
| `sdk/rocketnet_encryption_map.cpp:176` | guarded | num_buckets = capacity * 2 (encryption_map.cpp:94) after the real refusal `if ( capacity < 1 \|\| capacity > INT_MAX / 2 ) return NULL;` at encryption_map.cpp:84 |
| `sdk/rocketnet_packet_builder.cpp:190` | out of class | compile-time constant modulus RN_BUILDER_LINK_SLOTS |
| `sdk/rocketnet_packet_builder.cpp:275` | guarded | clamp packet_builder.cpp:260 `channels = builder->num_channels > 0 ? builder->num_channels : 1` |
| `sdk/rocketnet_driver_builtin.cpp:147` | out of class | compile-time constant modulus PORTABLE_TX_BANKS |
| `sdk/rocketnet_driver_builtin.cpp:149` | out of class | compile-time constant modulus PORTABLE_TX_BANKS |
| `sdk/rocketnet_driver_builtin.cpp:169` | out of class | compile-time constant modulus PORTABLE_TX_BANKS |
| `sdk/rocketnet_driver_builtin.cpp:199` | out of class | compile-time constant modulus PORTABLE_TX_BANKS |
| `sdk/rocketnet_driver_builtin.cpp:230` | out of class | compile-time constant modulus PORTABLE_RX_BANKS |
| `sdk/rocketnet_driver_builtin.cpp:232` | out of class | compile-time constant modulus PORTABLE_RX_BANKS |
| `sdk/rocketnet_driver_builtin.cpp:255` | out of class | compile-time constant modulus PORTABLE_RX_BANKS |
| `sdk/rocketnet_driver_builtin.cpp:262` | out of class | compile-time constant modulus PORTABLE_RX_BANKS |
| `sdk/rocketnet_gateway_netlink_linux.cpp:186` | out of class | static_assert over sizeof/constant |
| `sdk/rocketnet_message_server.cpp:231` | out of class | compile-time constant modulus ROCKETNET_ALLOCATION_ALIGNMENT |
| `sdk/rocketnet_message_server.cpp:1142` | guarded | ternary guard message_server.cpp:1142 `num_connections > 0 ? ... : 0` |
| `sdk/rocketnet_message_server.cpp:1149` | guarded | inside `for ( offset = 0; offset < num_connections; ... )` (message_server.cpp:1147), so num_connections >= 1 in the body |
| `sdk/rocketnet_message_server.cpp:1161` | guarded | inside the same loop (message_server.cpp:1147), num_connections >= 1 |
| `sdk/rocketnet_server_gso_linux.cpp:307` | out of class | compile-time constant modulus ROCKETNET_GSO_TX_BANKS |
| `sdk/rocketnet_server_gso_linux.cpp:309` | out of class | compile-time constant modulus ROCKETNET_GSO_TX_BANKS |
| `sdk/rocketnet_server_gso_linux.cpp:330` | out of class | compile-time constant modulus ROCKETNET_GSO_TX_BANKS |
| `sdk/rocketnet_server_gso_linux.cpp:347` | out of class | compile-time constant modulus ROCKETNET_GSO_TX_BANKS |
| `sdk/rocketnet_fragment.cpp:350` | guarded | set->num_slots set at fragment.cpp:210 only after fragment.cpp:199 rocketnet_reassembly_memory_allowed_delivered -> memory_required_delivered refuses num_slots < 1 (fragment.cpp:95) |
| `sdk/rocketnet_fragment.cpp:351` | guarded | same divisor as :350, bounded at fragment.cpp:95/199 |
| `sdk/rocketnet_server_rio_windows.cpp:334` | guarded | stride = round_up64(layout.frame_bytes, 64) >= 64: layout from constant ROCKETNET_FRAME_STRIDE (rio_windows.cpp:648/1129), frame_bytes <= 0 refused at rio_layout.h:58 |
| `sdk/rocketnet_server_rio_windows.cpp:764` | out of class | compile-time constant modulus ROCKETNET_RIO_SEND_PHASES |
| `sdk/rocketnet_server_rio_windows.cpp:770` | out of class | compile-time constant modulus ROCKETNET_RIO_SEND_PHASES |
| `sdk/rocketnet_server_rio_windows.cpp:950` | out of class | compile-time constant modulus ROCKETNET_RIO_SEND_PHASES |
| `sdk/rocketnet_server_rio_windows.cpp:981` | out of class | compile-time constant modulus ROCKETNET_RIO_RECEIVE_BANKS |
| `sdk/rocketnet_server_rio_windows.cpp:1057` | out of class | compile-time constant modulus ROCKETNET_RIO_RECEIVE_BANKS |
| `sdk/rocketnet_server_rio_windows.cpp:1107` | out of class | compile-time constant modulus ROCKETNET_RIO_RECEIVES |
| `sdk/rocketnet_server_rio_windows.cpp:1250` | out of class | compile-time constant modulus ROCKETNET_RIO_RECEIVES |
| `sdk/rocketnet_network_simulator.cpp:41` | out of class | comment (percent sign) |
| `sdk/rocketnet_network_simulator.cpp:959` | guarded | guarded in the same condition: `simulator->drop_every_nth >= 2 &&` at network_simulator.cpp:959 (config table min 2, internal.h:465 and :543) |
| `sdk/rocketnet_replay.cpp:13` | out of class | compile-time constant modulus ROCKETNET_REPLAY_WINDOW_BITS |
| `sdk/rocketnet_replay.cpp:19` | out of class | compile-time constant modulus ROCKETNET_REPLAY_WINDOW_BITS |
| `sdk/rocketnet_replay.cpp:25` | out of class | compile-time constant modulus ROCKETNET_REPLAY_WINDOW_BITS |
| `sdk/rocketnet_buffer_handoff.h:84` | out of class | template constant modulus NumBuffers (static_assert >= 3, buffer_handoff.h:53) |
| `sdk/rocketnet_buffer_handoff.h:87` | out of class | template constant modulus NumBuffers |
| `sdk/rocketnet_buffer_handoff.h:101` | out of class | template constant modulus NumBuffers |
| `sdk/rocketnet_buffer_handoff.h:113` | out of class | template constant modulus NumBuffers |
| `sdk/rocketnet_server_iocp_windows.cpp:56` | out of class | static_assert over offsetof/constant |
| `sdk/rocketnet_server_iocp_windows.cpp:57` | out of class | static_assert over alignof/constant |
| `sdk/rocketnet_server_iocp_windows.cpp:404` | out of class | compile-time constant modulus ROCKETNET_IOCP_RECEIVE_BANKS |
| `sdk/rocketnet_server_iocp_windows.cpp:989` | out of class | compile-time constant modulus ROCKETNET_IOCP_SEND_BANKS |
| `sdk/rocketnet_server_iocp_windows.cpp:1021` | out of class | compile-time constant modulus ROCKETNET_IOCP_SEND_BANKS |
| `sdk/rocketnet_server_iocp_windows.cpp:1076` | out of class | compile-time constant modulus ROCKETNET_IOCP_RECEIVE_BANKS |
| `sdk/rocketnet_window_ack.cpp:166` | guarded | every caller passes cap = region->sent_index_entries = 2 * packets_in_flight (allocator.cpp:431), packets_in_flight >= 64 by the table minimum (internal.h:583) validated at allocator.cpp:105 before carving |
| `sdk/rocketnet_window_ack.cpp:186` | guarded | cap = region->sent_index_entries >= 128 (allocator.cpp:431, config min internal.h:583, validated allocator.cpp:105) |
| `sdk/rocketnet_window_ack.cpp:206` | guarded | cap = region->sent_index_entries >= 128 (as :186) |
| `sdk/rocketnet_window_ack.cpp:215` | guarded | cap = region->sent_index_entries >= 128 (as :186) |
| `sdk/rocketnet_window_ack.cpp:225` | guarded | cap = region->sent_index_entries >= 128 (as :186) |
| `sdk/rocketnet_window_ack.cpp:251` | guarded | P = region->packets >= 64 (allocator.cpp:429, config min internal.h:583, validated allocator.cpp:105) |
| `sdk/rocketnet_window_ack.cpp:434` | guarded | capacity = region->receipt_entries = max_message_receipts >= 16 (allocator.cpp:435, config min internal.h:587, validated allocator.cpp:105) |
| `sdk/rocketnet_window_ack.cpp:451` | guarded | max_refs = region->sent_ref_entries = 16 * packets_in_flight >= 1024 (allocator.cpp:432, validated allocator.cpp:105) |
| `sdk/rocketnet_window_ack.cpp:506` | guarded | P = region->packets >= 64 (as :251) |
| `sdk/rocketnet_window_ack.cpp:524` | guarded | P = region->packets >= 64 (as :251) |
| `sdk/rocketnet_window_ack.cpp:525` | guarded | max_refs >= 1024 (as :451) |
| `sdk/rocketnet_window_ack.cpp:527` | guarded | max_refs >= 1024 (as :451) |
| `sdk/rocketnet_window_ack.cpp:529` | guarded | max_refs >= 1024 (as :451) |
| `sdk/rocketnet_window_ack.cpp:538` | guarded | max_refs >= 1024 (as :451) |
| `sdk/rocketnet_window_ack.cpp:607` | guarded | max_refs >= 1024 (as :451) |
| `sdk/rocketnet_window_ack.cpp:697` | guarded | region->packets >= 64 (as :251) |
| `sdk/rocketnet_window_ack.cpp:718` | guarded | region->receipt_entries >= 16 (as :434) |
| `sdk/rocketnet_window_ack.cpp:859` | guarded | P = region->packets >= 64 (as :251) |
| `sdk/rocketnet_server_transport.cpp:2367` | guarded | inside `for ( offset = 0; offset < max_clients; ... )` (server_transport.cpp:2365), so max_clients >= 1 in the body (and client_slots_create refuses max_clients < 1, client_slots.cpp:69) |
| `sdk/rocketnet_server_transport.cpp:2373` | guarded | inside the same loop (server_transport.cpp:2365) |
| `sdk/rocketnet_server_transport.cpp:2381` | guarded | inside the same loop (server_transport.cpp:2365) |
| `sdk/rocketnet_server_transport.cpp:2397` | guarded | inside the same loop (server_transport.cpp:2365) |
| `sdk/rocketnet_server_transport.cpp:3167` | out of class | compile-time constant modulus ROCKETNET_MAX_CLIENTS |
| `sdk/rocketnet_server_transport.cpp:3202` | out of class | compile-time constant modulus ROCKETNET_MAX_CLIENTS |
| `sdk/rocketnet_server_transport.cpp:3233` | out of class | compile-time constant modulus ROCKETNET_MAX_CLIENTS |
| `sdk/rocketnet_server_socket_linux.cpp:152` | out of class | template constant modulus N |
| `sdk/rocketnet_server_socket_linux.cpp:154` | out of class | template constant modulus N |
| `sdk/rocketnet_server_socket_linux.cpp:163` | out of class | template constant modulus N |
| `sdk/rocketnet_server_socket_linux.cpp:171` | out of class | template constant modulus N |
| `sdk/rocketnet_server_socket_linux.cpp:2056` | out of class | compile-time constant modulus ROCKETNET_XDP_FRAME_SIZE |
| `sdk/rocketnet_server_socket_linux.cpp:2085` | guarded | sole sdk caller passes server_socket->num_queues (socket_linux.cpp:2152), set at :1346 only after create's preflight (:1276) refused num_queues < 1 via memory_required (:785, :822) |
| `sdk/rocketnet_server_socket_linux.cpp:2169` | guarded | server_socket->num_queues >= 1: set at socket_linux.cpp:1346 after preflight :1276 -> memory_required :785 refuses num_queues < 1 (preflight returns false at :822) |
| `sdk/rocketnet_server_socket_linux.cpp:2222` | guarded | as :2169 |
| `sdk/rocketnet_token_tables.cpp:150` | guarded | num_buckets = capacity * 2 (token_tables.cpp:91) after the real refusal `if ( capacity < 1 \|\| capacity > INT_MAX / 2 ) return NULL;` at token_tables.cpp:81 |
| `sdk/rocketnet_coalesce.h:43` | guarded | real clamp coalesce.h:40-41 `if ( segment_bytes <= 0 ) segment_bytes = message_bytes;` after :37 returns for message_bytes <= 0 |
| `sdk/rocketnet_packet_loss_tracker.h:69` | out of class | compile-time constant modulus ROCKETNET_PACKET_LOSS_TRACKER_HISTORY |
| `sdk/rocketnet_packet_loss_tracker.h:99` | out of class | compile-time constant modulus ROCKETNET_PACKET_LOSS_TRACKER_HISTORY |
| `sdk/rocketnet_xdp_frame.h:443` | out of class | compile-time constant modulus ROCKETNET_XDP_FRAME_SIZE |
| `sdk/rocketnet_stats_window.h:16` | out of class | template constant modulus N (static_assert N > 1, stats_window.h:12) |
| `sdk/rocketnet_window_ack.h:92` | out of class | comment (percent sign) |
| `sdk/rocketnet_tests.cpp:173` | out of class | test code, constant modulus |
| `sdk/rocketnet_tests.cpp:9194` | out of class | test code |
| `sdk/rocketnet_tests.cpp:9196` | out of class | test code |
| `sdk/rocketnet_tests.cpp:9201` | out of class | test code |
| `sdk/rocketnet_tests.cpp:9218` | out of class | test code |
| `sdk/rocketnet_tests.cpp:9644` | out of class | test code |
| `sdk/rocketnet_tests.cpp:9661` | out of class | test code |
| `sdk/rocketnet_tests.cpp:9674` | out of class | test code |
| `sdk/rocketnet_tests.cpp:9951` | out of class | test code |
| `sdk/rocketnet_tests.cpp:9966` | out of class | test code |
| `sdk/rocketnet_tests.cpp:10188` | out of class | test code |
| `sdk/rocketnet_tests.cpp:10234` | out of class | test code |
| `sdk/rocketnet_tests.cpp:10405` | out of class | test code |
| `sdk/rocketnet_tests.cpp:10406` | out of class | test code |
| `sdk/rocketnet_tests.cpp:12024` | out of class | test code |
| `sdk/rocketnet_tests.cpp:12025` | out of class | test code |
| `functional/channel_resend_timing.cpp:8` | out of class | comment (percent sign) |
| `functional/channel_resend_timing.cpp:341` | out of class | comment (percent sign) |
| `sdk/rocketnet_window_send.cpp:39` | guarded | W = region->window = reliable_messages_in_flight (allocator.cpp:426), table minimum 64 (internal.h:578) validated at allocator.cpp:105 before carving |
| `sdk/rocketnet_window_send.cpp:66` | guarded | region->window >= 64 (as :39) |
| `sdk/rocketnet_window_send.cpp:165` | guarded | region->window >= 64 (as :39) |
| `sdk/rocketnet_window_send.cpp:171` | guarded | region->window >= 64 (as :39) |
| `sdk/rocketnet_window_send.cpp:181` | guarded | region->window >= 64 (as :39) |
| `functional/channel_reliable_sequenced.cpp:115` | out of class | functional test harness |
| `sdk/rocketnet_allocator.h:72` | out of class | comment |
| `sdk/rocketnet_allocator.h:96` | out of class | comment |
| `sdk/rocketnet_allocator.h:202` | out of class | comment |
| `sdk/rocketnet_allocator.h:357` | out of class | comment |
| `functional/live_matrix.cpp:497` | out of class | functional test harness |
| `functional/live_matrix.cpp:502` | out of class | functional test harness |
| `functional/live_matrix.cpp:515` | out of class | functional test harness |
| `functional/live_matrix.cpp:555` | out of class | functional test harness |
| `functional/lifecycle_driver.cpp:119` | out of class | functional test harness, constant modulus |
| `functional/lifecycle_driver.cpp:141` | out of class | functional test harness, constant modulus |
| `functional/posix_lifecycle_stress.cpp:959` | out of class | functional test harness |
| `functional/posix_lifecycle_stress.cpp:961` | out of class | functional test harness |
| `functional/posix_lifecycle_stress.cpp:965` | out of class | functional test harness |
| `functional/posix_lifecycle_stress.cpp:986` | out of class | functional test harness |
| `functional/posix_hard_killed_client.cpp:1274` | out of class | functional test harness |
| `functional/posix_hard_killed_client.cpp:1276` | out of class | functional test harness |
| `tests/dst_checks.h:226` | out of class | test harness, constant modulus |
| `tests/dst_checks.h:237` | out of class | test harness, constant modulus |
| `tests/dst_checks.h:247` | out of class | test harness, constant modulus |
| `tests/dst_checks.h:296` | out of class | test harness, constant modulus |
| `tests/dst_checks.h:297` | out of class | test harness, constant modulus |
| `tests/dst_checks.h:524` | out of class | test harness, constant modulus |
| `tests/dst_checks.h:1027` | out of class | test harness, constant modulus |
| `tests/dst_checks.h:1036` | out of class | test harness, constant modulus |
| `tests/live_verify.h:222` | out of class | test harness |
| `tests/live_verify.h:236` | out of class | test harness, constant modulus |
| `tests/live_verify.h:243` | out of class | test harness |
| `tests/rocketnet_value_tracker.h:79` | out of class | test harness, constant modulus |
| `sdk/rocketnet_send_handoff.h:38` | out of class | template constant modulus NumBuffers (static_assert >= 2) |
| `sdk/rocketnet_send_handoff.h:55` | out of class | template constant modulus NumBuffers |
| `sdk/rocketnet_send_handoff.h:64` | out of class | template constant modulus NumBuffers |
| `sdk/rocketnet_xdp_free_list.h:19` | out of class | compile-time constant modulus ROCKETNET_XDP_FRAME_SIZE |
| `functional/channel_config.cpp:9` | out of class | comment (percent sign) |
| `functional/windows_socket_errors.cpp:362` | out of class | functional test harness, constant modulus |
| `functional/windows_socket_errors.cpp:518` | out of class | functional test harness, constant modulus |
| `tests/dst_harness.h:106` | out of class | test harness |
| `functional/posix_socket_errors.cpp:581` | out of class | functional test harness |
| `functional/posix_socket_errors.cpp:2212` | out of class | functional test harness |
| `functional/posix_socket_errors.cpp:2217` | out of class | functional test harness |
| `tests/fake_client_driver.h:93` | out of class | test harness, constant modulus |
| `tests/fake_client_driver.h:126` | out of class | test harness, constant modulus |
| `tests/fake_client_driver.h:206` | out of class | test harness, constant modulus |
| `functional/rocketnet_functional_tests.cpp:4585` | out of class | functional test harness, constant modulus |
| `functional/rocketnet_functional_tests.cpp:5462` | out of class | functional test harness, constant modulus |
| `functional/rocketnet_functional_tests.cpp:5481` | out of class | functional test harness, constant modulus |
| `functional/rocketnet_functional_tests.cpp:5510` | out of class | functional test harness, constant modulus |
| `functional/rocketnet_functional_tests.cpp:5538` | out of class | functional test harness, constant modulus |
| `functional/rocketnet_functional_tests.cpp:6209` | out of class | functional test harness, constant modulus |
| `functional/rocketnet_functional_tests.cpp:6228` | out of class | functional test harness, constant modulus |
| `functional/rocketnet_functional_tests.cpp:6254` | out of class | functional test harness, constant modulus |
| `functional/rocketnet_functional_tests.cpp:6303` | out of class | functional test harness, constant modulus |
| `tests/serialize.h:59` | out of class | comment (percent sign) |
| `tests/serialize.h:82` | out of class | comment (percent sign) |
| `tests/serialize.h:500` | out of class | test-tree serializer (tests/), not the sdk library |
| `tests/serialize.h:523` | out of class | test-tree serializer operator definition |
| `tests/serialize.h:545` | out of class | test-tree serializer operator |
| `tests/serialize.h:750` | out of class | test-tree serializer operator definition |
| `tests/serialize.h:762` | out of class | test-tree serializer operator |
| `tests/serialize.h:1152` | out of class | comment (percent sign) |
| `tests/serialize.h:6823` | out of class | test code, constant operands |
| `tests/serialize.h:7086` | out of class | test code (zero divisors excluded by the test) |
| `tests/serialize.h:7128` | out of class | test code |
| `tests/serialize.h:7257` | out of class | test code |
| `tests/serialize.h:7310` | out of class | test code |

### Pattern 4: division by a parenthesised or cast divisor

| file:line | judged | reason |
|---|---|---|
| `sdk/rocketnet_ack.cpp:752` | guarded | ternary `stats_rtt_count > 0` on ack.cpp:751 |
| `sdk/rocketnet_ack.cpp:758` | guarded | ternary `stats_eligible_count > 0` on ack.cpp:757 |
| `sdk/rocketnet_ack.cpp:1345` | guarded | ternary `count > 0` on the same line |
| `sdk/rocketnet_ack.cpp:1422` | guarded | ternary `num_sent > 0` on the same line |
| `sdk/rocketnet_ack.cpp:1425` | guarded | ternary `sent_finish > sent_start` on ack.cpp:1424 |
| `sdk/rocketnet_ack.cpp:1428` | guarded | ternary `acked_finish > acked_start` on ack.cpp:1427 |
| `sdk/rocketnet_ack.cpp:1471` | guarded | ternary `finish > start` on ack.cpp:1470 |
| `sdk/rocketnet_allocator.cpp:1220` | guarded | `num_regions < 1` refused at allocator.cpp:1210 |
| `sdk/rocketnet_client_transport.cpp:2336` | guarded | ternary `loss_denominator > 0` on client_transport.cpp:2335 |
| `sdk/rocketnet_network_simulator.cpp:195` | out of class | constant divisor 2^53 |
| `sdk/rocketnet_platform.cpp:2588` | guarded | `timebase_info.denom` from `mach_timebase_info` (platform.cpp:2398), non-zero by the OS contract |
| `sdk/rocketnet_platform.cpp:4355` | guarded | QueryPerformanceFrequency, non-zero by the OS contract on every supported Windows |
| `sdk/rocketnet_platform.cpp:4381` | guarded | `timer_frequency` from QueryPerformanceFrequency (platform.cpp:4120), non-zero by the OS contract |
| `sdk/rocketnet_layers.h:25` | out of class | macro whose `align` argument is a compile-time constant at every use |
| `sdk/rocketnet_window_ack.h:157` | out of class | constant divisor `ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS` |

### Pattern 5: division by an upper-case divisor

| file:line | judged | reason |
|---|---|---|
| `sdk/rocketnet_ack.cpp:1506` | out of class | divisor `ROCKETNET_SENT_PACKET_BUFFER_SIZE` is a compile-time constant |
| `sdk/rocketnet_ack.cpp:1517` | out of class | divisor `ROCKETNET_SENT_PACKET_BUFFER_SIZE` is a compile-time constant |
| `sdk/rocketnet_channel.cpp:713` | out of class | divisor `F` is the constant `ROCKETNET_MESSAGE_FRAGMENT_BYTES` |
| `sdk/rocketnet_channel.cpp:988` | out of class | divisor `F` is the constant `ROCKETNET_MESSAGE_FRAGMENT_BYTES` |
| `sdk/rocketnet_allocator.cpp:193` | out of class | divisor `RN_STORE_NUM_CLASSES` is a compile-time constant |
| `sdk/rocketnet_allocator.cpp:218` | out of class | divisor `RN_STORE_LARGE_BYTES` is a compile-time constant |
| `sdk/rocketnet_channel_sequenced.cpp:122` | out of class | divisor `F` is the constant `ROCKETNET_MESSAGE_FRAGMENT_BYTES` |
| `sdk/rocketnet_api.cpp:467` | out of class | divisor `ROCKETNET_ALLOCATION_ALIGNMENT` is a compile-time constant |
| `sdk/rocketnet_config.cpp:216` | out of class | divisor `F` is the constant `ROCKETNET_MESSAGE_FRAGMENT_BYTES` |
| `sdk/rocketnet_channel_reliable.cpp:37` | out of class | divisor `F` is the constant `ROCKETNET_MESSAGE_FRAGMENT_BYTES` |
| `sdk/rocketnet_client_transport.cpp:2642` | out of class | divisor `ROCKETNET_FRAME_STRIDE` is a compile-time constant |
| `sdk/rocketnet_client_transport.cpp:2822` | out of class | divisor `ROCKETNET_FRAME_STRIDE` is a compile-time constant |
| `sdk/rocketnet_frame_sync.cpp:55` | out of class | divisor `ROCKETNET_NANOS_PER_SECOND` is a compile-time constant |
| `sdk/rocketnet_frame_sync.cpp:59` | out of class | divisor `ROCKETNET_NANOS_PER_SECOND` is a compile-time constant |
| `sdk/rocketnet_frame_sync.cpp:398` | out of class | divisor `ROCKETNET_FRAME_SYNC_GUARD_CAP_DIVISOR` is a compile-time constant |
| `sdk/rocketnet_fragment.cpp:62` | out of class | divisor `ROCKETNET_MAX_FRAGMENT_BYTES` is a compile-time constant |
| `sdk/rocketnet_server_gso_linux.cpp:1209` | out of class | divisor `ROCKETNET_FRAME_STRIDE` is a compile-time constant |
| `sdk/rocketnet_server_gso_linux.cpp:1231` | out of class | divisor `ROCKETNET_FRAME_STRIDE` is a compile-time constant |
| `sdk/rocketnet_server_rio_windows.cpp:1180` | out of class | divisor `ROCKETNET_RIO_DEQUEUE_BATCH` is a compile-time constant |
| `sdk/rocketnet_server_rio_windows.cpp:1329` | out of class | divisor `ROCKETNET_RIO_RECEIVES` is a compile-time constant |
| `sdk/rocketnet_server_rio_windows.cpp:1330` | out of class | divisor `ROCKETNET_RIO_RECEIVES` is a compile-time constant |
| `sdk/rocketnet_server_transport.cpp:2675` | out of class | divisor `ROCKETNET_FRAME_STRIDE` is a compile-time constant |
| `sdk/rocketnet_server_transport.cpp:2990` | out of class | divisor `ROCKETNET_FRAME_STRIDE` is a compile-time constant |
| `sdk/rocketnet_server_uring_linux.cpp:1512` | out of class | divisor `ROCKETNET_FRAME_STRIDE` is a compile-time constant |
| `sdk/rocketnet_server_socket_linux.cpp:2058` | out of class | divisor `ROCKETNET_XDP_FRAME_SIZE` is a compile-time constant |
| `sdk/rocketnet_window_send.cpp:135` | out of class | divisor `F` is the constant `ROCKETNET_MESSAGE_FRAGMENT_BYTES` |

## Already filed

Hits of this sweep that earlier audits already report; listed once here, never as new findings.

| file:line | filed in | finding | what |
|---|---|---|---|
| `sdk/rocketnet_window_send.cpp:85` | `docs/audits/2026-10-24-code-quality-a.md` | 3 | `ring_bytes` non-zero and a multiple of 16, asserted only; the divisor of line 19 |
| `sdk/rocketnet_window_send.cpp:19` | `docs/audits/2026-10-24-code-quality-a.md` | 3 | `ring_next % ring_bytes`, the use finding 3 names (pattern 3 hit) |
| `sdk/rocketnet_fragment.cpp:265` | `docs/audits/2026-10-24-code-quality-a.md` | 4 | `connection_index` range asserted only in the reset entry point |
| `sdk/rocketnet_fragment.cpp:312` | `docs/audits/2026-10-24-code-quality-a.md` | 4 | `connection_index` range asserted only in `rocketnet_reassembly_process_fragment` |
| `sdk/rocketnet_server_transport.cpp:409` | `docs/audits/2026-10-24-code-quality-b.md` | 1 | `malloc` result asserted non-null, then reset through |
| `sdk/rocketnet_ack.cpp:279` | `docs/audits/2026-10-24-code-quality-b.md` | 1 | the `fresh` pointer that filing names reaches this assert-only null check |
| `sdk/rocketnet_ack.cpp:303` | `docs/audits/2026-10-24-code-quality-b.md` | 1 | the `fresh` pointer that filing names reaches this `memset` behind an assert-only null check |
| `sdk/rocketnet_window_send.cpp:32` | `docs/audits/2026-10-24-code-quality-b.md` | 2 | `count <= W` asserted only before `W - count` |
| `sdk/rocketnet_client_slots.cpp:294` | `docs/audits/2026-10-24-code-quality-b.md` | 3 | chain search result asserted only, then the slot is freed |
| `sdk/rocketnet_server_socket_linux.cpp:2689` | `docs/audits/2026-10-24-area-transports-linux-af-xdp.md` | 4 | the XDP pin target is checked against the online CPU count by an assert only; filed after the swept sha as a pre-flight defect, and the Pattern 1 table judges the `CPU_SET` index itself guarded |
