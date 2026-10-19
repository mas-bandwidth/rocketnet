; RocketNet roadmap data. ROADMAP.md is rendered from this file by
; tools/roadmap/render.py (make roadmap); make roadmap-check fails when the
; committed page is not what this renders.
;
; A cell is y (done, with :evidence naming a commit or a test), n (not done)
; or - (does not apply). Evidence stays here; the page shows only the marks.
; Rows reconciled against main 5c51005 on 2026-10-17. Retired from the old
; card roadmap: the C# binding and its shim (binding removed 744bc1e), the
; duplicate second layer numbering (channels 18-23), and fixes cards 4 and 12
; (ruled: the standard is right).
(:title "RocketNet roadmap"
 :text ("Every feature RocketNet has, and every one still to come, by area. ✅ is done and tested on main; ❌ is not done yet; — does not apply. Items marked (later) are deliberately after the current work, by the owner's call. An area marked (next milestone) is the next milestone, by the owner's call.")
 :areas
 ((:id "handshake" :title "Connect handshake and security"
   :text "The secure connect from STANDARD.md: netcode 1.02 rebuilt on public keys, with every refusal counted."
   :columns ("done")
   :rows
   ((:feature "Public-key connect tokens: mint, verify and open in the standard's order" :cells (y) :evidence "rocketnet_test_connect_token_frozen, rocketnet_test_connect_token_mint")
    (:feature "Deterministic connect token vectors for minters in other languages" :cells (y) :evidence "e05088e rocketnet-connect-token-vectors.w1")
    (:feature "6-byte prefilter header with a per-title filter value and epoch" :cells (y) :evidence "52d9be7; rocketnet_test_prefilter")
    (:feature "Cookie gate: bounds, signature, expiry and address checked" :cells (y) :evidence "64ffe20; rocketnet_test_cookie, rocketnet_test_cookie_bounds_signature_verifies")
    (:feature "Cookie key rotates on its period" :cells (y) :evidence "6b60b32, 5e534d4; test_cookie_rotation_draws_a_fresh_key")
    (:feature "Per-connection keys with forward secrecy, secrets wiped on every exit" :cells (y) :evidence "rocketnet_test_connection_keys, rocketnet_test_client_terminal_states_wipe_secrets")
    (:feature "Sealed packets with every refusal proven" :cells (y) :evidence "rocketnet_test_sealed_packets, rocketnet_test_sealed_packet_refusals")
    (:feature "Used-token history and one token, one live connection" :cells (y) :evidence "rocketnet_test_used_token_history, server_admits_sequential_fresh_tokens_past_twice_max_clients, server_refuses_a_token_connected_now")
    (:feature "Admission hook drops a packet before any verify" :cells (y) :evidence "9eb49a9; rocketnet_test_admission_hook_drops_before_verify")
    (:feature "Cheap refusals before crypto: a full server refuses before keypair work" :cells (y) :evidence "PR #248, 4901aba; rocketnet_test_full_server_refuses_before_keypair, rocketnet_test_request_path_cheap_refusals_spend_no_crypto")
    (:feature "Bounded handshake work per update" :cells (y) :evidence "1745d64")
    (:feature "Configurable half-open handshake deadline" :cells (y) :evidence "916239a; rocketnet_test_handshake_deadline_is_configured")
    (:feature "Replay window, cleared whole by a long sequence skip" :cells (y) :evidence "a94f99c, 5ab9e5c; rocketnet_test_replay_protection, rocketnet_test_replay_long_skip_clears_whole_window")
    (:feature "Replay window advances only after the connect check" :cells (y) :evidence "8eab0e0 the slot gate before the replay mark")
    (:feature "Hostile fragment ids cannot poison a reassembly slot" :cells (y) :evidence "4f09049; rocketnet_test_fragment_hostile_id_does_not_poison_slot")
    (:feature "Oversize datagrams are dropped, never truncated" :cells (y) :evidence "611788d, 5321d20; rocketnet_test_receive_drops_oversize_datagram")
    (:feature "Release builds refuse development keys" :cells (y) :evidence "e5d3393; rocketnet_test_dev_keys_refused_in_release")
    (:feature "Release builds do not export development token minting" :cells (y) :evidence "5cc44f4, 1b81ce9; rocketnet_test_dev_token_minting_absent_in_release")
    (:feature "Public API is NULL-safe at the boundary" :cells (y) :evidence "3e51b9d; rocketnet_test_public_api_null_is_a_counted_no_op")
    (:feature "Refusal decisions agree with netcode 1.02" :cells (y) :evidence "e5acbbb")
    (:feature "The token is bound at response open" :cells (n) :note "rocketnet-fixes card 7, in progress")
    (:feature "Session layer: identity auth with a backend, rolling magic, keyed filter value (later)" :cells (n) :note "v2, optional, behind the first deliverable. The 6-byte header with its epoch byte is already in, so the keyed form is a configuration change, not a wire break. It needs a backend that tracks the connected set across servers live: one identity, one seat, so a shared token gives away the sharer's own slot, and the old player is kicked with rocketnet_server_transport_disconnect_client when the same user hash appears elsewhere. An extraction from the game's own backend, not a new design. Then the rolling magic feeds the keyed filter value (SipHash over type, length and the four bytes after the header, one MAC per packet), and a per-title generator emits the constant and the keyed reader in every language plus verifier-clean BPF behind a differential gate. Never: a default magic, or standalone IP pinning")))

  (:id "connection" :title "Connections and the transport"
   :text "The unreliable transport the message layer runs over: connection state, slots, keep-alives, fragments, acks and the server loop."
   :columns ("done")
   :rows
   ((:feature "Client connection state machine with every timeout pairing" :cells (y) :evidence "rocketnet_test_client_timeout_paths; tla/ClientConnection.tla 3afa363")
    (:feature "Server client slots from a free-slot stack" :cells (y) :evidence "c40f599; rocketnet_test_client_slots; tla/ServerConnection.tla eb57f2d")
    (:feature "Encryption mappings with handshake and idle deadlines" :cells (y) :evidence "rocketnet_test_encryption_map, rocketnet_test_server_update_sweep")
    (:feature "Keep-alives paced and suppressed by traffic" :cells (y) :evidence "rocketnet_test_client_keep_alive_suppression")
    (:feature "Payloads both ways from the connection response on" :cells (y) :evidence "rocketnet_test_client_payload_before_first_keep_alive")
    (:feature "Redundant disconnect burst, from the client or the server" :cells (y) :evidence "rocketnet_test_client_disconnect_reports_state_change; rocketnet_server_transport_disconnect_client")
    (:feature "Large payloads fragmented and reassembled" :cells (y) :evidence "7e959ab; rocketnet_test_fragment_vector; reassembly geometry under a 512 MiB component budget, oversized configurations refused before any allocation: test_lifecycle.cpp check_memory_geometry")
    (:feature "Fragments accepted only on a connected slot" :cells (y) :evidence "9e0d822")
    (:feature "Per-packet acks with delivery notification" :cells (y) :evidence "8af8b80; rocketnet_test_ack_block_validity, rocketnet_test_ack_block_build")
    (:feature "Ack history that survives a long lossy path" :cells (y) :evidence "187a875; rocketnet_test_send_history_lossy_long_path; ROCKETNET_HISTORY_PACKETS 4096 default, 4096 to 32768 (sdk/rocketnet_history.h), no frame, send or ack rate assumed; test_lifecycle.cpp check_ack_retention, rocketnet_test_send_history_silent_peer_overflows")
    (:feature "Frame sync: a fixed-rate server loop with priority and drop detection" :cells (y) :evidence "rocketnet_test_frame_sync_advance, rocketnet_test_frame_sync_guard")
    (:feature "Counters on every path, each classified, conserved and named by a test" :cells (y) :evidence "1750e83 rocketnet-counters-classified-and-conserved; rocketnet_num_counters and rocketnet_counter_value for FFI callers; packet-rate statistics in elapsed-time buckets: test_lifecycle.cpp check_stats_cadence")))

  (:id "reliability" :title "Reliability modes"
   :text "The eight RakNet reliabilities, per send, over the transport's per-packet acks."
   :columns ("done")
   :rows
   ((:feature "UNRELIABLE" :cells (y) :evidence "f6a42cc; unreliable_delivered_as_a_view, unreliable_never_takes_a_number")
    (:feature "UNRELIABLE_SEQUENCED" :cells (y) :evidence "160c5c0, 55cfe99; older_dropped_newest_delivered")
    (:feature "RELIABLE" :cells (y) :evidence "7b83d3c, 40941d1; reliable_exactly_once_any_order_under_loss_dup_reorder")
    (:feature "RELIABLE_ORDERED" :cells (y) :evidence "8cf8bc7, 16531dd; ordered_and_unreliable_end_to_end_in_memory")
    (:feature "RELIABLE_SEQUENCED" :cells (y) :evidence "999c7f5, 41fc05e; reliable_sequenced_block_delivers_whole")
    (:feature "UNRELIABLE_WITH_ACK_RECEIPT" :cells (y) :evidence "0b1f91c; unreliable_receipt_acked_or_loss_exactly_once")
    (:feature "RELIABLE_WITH_ACK_RECEIPT" :cells (y) :evidence "f391ded; reliable_receipt_acked_never_loss")
    (:feature "RELIABLE_ORDERED_WITH_ACK_RECEIPT" :cells (y) :evidence "f391ded, d94a228; ordered_receipt_after_all_chunks")))

  (:id "messages" :title "Channels, sending and memory"
   :text "The message layer of docs/CHANNELS.md: channels, priorities, budgets and one fixed region per connection."
   :columns ("done")
   :rows
   ((:feature "Zero-copy send: start, write in place, finish or abort" :cells (y) :evidence "2c1aa7b; reliable_sequenced_c_api_start_finish")
    (:feature "Receive and release messages as views" :cells (y) :evidence "2c1aa7b; unreliable_delivered_as_a_view; tla/RNSlots.tla")
    (:feature "N channels with per-record dispatch" :cells (y) :evidence "c79da45 rocketnet-channel-dispatch-and-packet-builder")
    (:feature "Large reliable messages split into blocks and delivered whole" :cells (y) :evidence "16531dd; reliable_sequenced_block_delivers_whole, ordered_receipt_after_all_chunks")
    (:feature "Four send priorities, round robin across channels" :cells (y) :evidence "273cc81, e0733e5; higher_priority_first_within_class")
    (:feature "Small messages aggregated into packets" :cells (y) :evidence "c79da45; builder_obeys_budget_and_class_order")
    (:feature "Send budget in kbps per connection" :cells (y) :evidence "a4ff908; budgets_are_per_connection")
    (:feature "A 1400-byte game message fits one packet" :cells (y) :evidence "ff4c7c6; message_of_1400_bytes_is_one_unfragmented_packet")
    (:feature "One fixed region per connection, no allocation after create" :cells (y) :evidence "8b99c72; no_allocation_after_create")
    (:feature "Caller-supplied memory block" :cells (y) :evidence "user_block_is_used_and_nothing_else; aligned allocation keeps its callback and context, late replacement refused: rocketnet_test_allocator_refusal_is_returned, test_lifecycle.cpp check_component_create_sweeps")
    (:feature "Memory exhaustion kicks only that connection" :cells (y) :evidence "c335529; server_kicks_exhausted_client_others_unaffected")
    (:feature "Hostile records and chunks refused" :cells (y) :evidence "d1ff1a2; hostile_chunk_count_or_index_kicks, hostile_small_at_held_ordering_is_refused")
    (:feature "Endpoint and slots as pure machines (CHANNELS.md layer 7)" :cells (n) :note "no rn_endpoint; the objects compose the layers directly")
    (:feature "CONFIG announcement: both ends prove the same configuration" :cells (n) :note "ROCKETNET_MESSAGE_ERROR_CONFIG_MISMATCH defined only")
    (:feature "Message layer soaked at full configured load" :cells (n) :note "bounded exhaustion soak c335529 only")
    (:feature "Broadcast send (later)" :cells (n) :note "owner's ideas roadmap; REQUIREMENTS.md step 3 with reliability (the Reliability modes table) and flows")
    (:feature "Flows: server-side subscription groups (later)" :cells (n) :note "REQUIREMENTS.md step 3")))

  (:id "drivers" :title "Drivers"
   :text "The server socket drivers below the transport, selected at runtime by the per-endpoint ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT; the old names ROCKETNET_DRIVER, ROCKETNET_TRANSPORT, ROCKETNET_SERVER_DRIVER and ROCKETNET_CLIENT_DRIVER are refused aloud and never applied (sdk/rocketnet_config.cpp). Loopback is pin-only."
   :columns ("portable" "gso" "msgx" "iocp" "xdp" "io_uring" "loopback")
   :rows
    ((:feature "Server driver" :cells (y y y y y y y) :evidence "portable: rocketnet_server_socket_linux.cpp; gso 0801c9a; msgx 8d54e30; iocp 45da97d; xdp c0b0928; io_uring f7cfe0f and 7be058d (test_server_uring_probe, test_server_uring_wire_ipv4, test_server_uring_wire_ipv6, test_server_uring_wire_dual_stack, test_server_uring_send_overflow, test_server_uring_receive_rearms_after_no_buffers); loopback bfcb888 (driver_loopback_binds_and_discovers_a_port, driver_loopback_exchanges_both_ways, driver_loopback_full_queue_drops_and_counts, driver_loopback_destroy_frees_the_port)")
     (:feature "Selectable by pin at runtime" :cells (y y y y y y y) :evidence "10308e4; rocketnet_test_server_driver_pin_portable, _gso, _msgx, _iocp, _xdp; io_uring 2d2c057 (test_server_driver_pin_io_uring); loopback 5d059d8 and 1c6f91a (test_driver_pin_only, test_server_driver_pin_loopback, test_driver_env_pins_loopback)")))

  (:id "planned-drivers" :title "Planned drivers and clients"
   :text "The driver and client work carded today, and the later milestones behind it."
   :columns ("done")
   :rows
   ((:feature "Linux UDP_GRO receive coalescing" :cells (y) :evidence "the gso driver enables UDP_GRO per socket and splits by the stride cmsg; GRO_BATCHES in the gso wire case")
    (:feature "Windows Registered I/O (RIO) server driver" :cells (n) :note "stream rocketnet-windows: rocketnet-tla-rio-driver, rocketnet-rio-driver-core, rocketnet-rio-driver-wired-and-pinned")
    (:feature "Windows USO and URO in the IOCP driver, probed with fallback" :cells (n) :note "rocketnet-iocp-uso-uro")
    (:feature "Client socket I/O off the main thread, gated by the measured main-thread cost per frame" :cells (n) :note "stream rocketnet-client-io")
    (:feature "Console client drivers (PS4, PS5, Switch, Xbox GDK)" :cells (n) :note "the source layers are under a different license; the owner's ruling is owed before any card")
    (:feature "XDP/AF_XDP on AWS GameLift fleets: research" :cells (n) :note "rocketnet-gamelift-xdp-spike, docs/GAMELIFT.md")))

  (:id "browser-clients" :title "Browser clients (later)"
   :text "Browsers cannot open raw UDP, so a browser client reaches a RocketNet server through a WebTransport or WebRTC data channel front-end, either a relay or a second front-end in the server."
   :columns ("done")
   :rows
   ((:feature "WebTransport (HTTP/3 datagrams) client and front-end" :cells (n))
    (:feature "WebRTC data channel client and front-end" :cells (n))
    (:feature "Relay or second front-end that bridges either into the UDP transport" :cells (n))))

  (:id "dedicated-nic" :title "Dedicated-NIC drivers (later)"
   :text "These drivers are only for a deployment where a second NIC is exclusively the game server's and the main NIC carries admin traffic; RocketNet never takes over a shared NIC, which is why AF_XDP (the kernel stack keeps the NIC, the server steers only its own packets) is the driver for shared NICs."
   :columns ("done")
   :rows
   ((:feature "netmap driver on FreeBSD (netmap is in the FreeBSD kernel)" :cells (n))
    (:feature "netmap driver on Linux, only if netmap enters the mainline kernel (today it needs an out-of-tree module and patched NIC drivers)" :cells (n))
    (:feature "DPDK driver" :cells (n))))

  (:id "xdp" :title "XDP (later)"
   :text "The XDP server transport. Further XDP work comes later, separately, by the owner's call; RocketNet already runs with XDP in the game, tested to 10 Gbps."
   :columns ("done")
   :rows
   ((:feature "Kernel prefilter with AF_XDP rings and counted verdicts" :cells (y) :evidence "c0b0928; xdp_host_every_counter_has_a_case; libxdp all-or-zero reservation, send pressure retains the staged packet, no worker exits: test_lifecycle.cpp check_frame_conservation, check_send_handoff")
    (:feature "Native attach on a production NIC with internet handshakes" :cells (y) :evidence "8d54514")
    (:feature "Kernel filter decisions run as host C in the unit suite" :cells (y) :evidence "bf2aecd")
    (:feature "IPv6 in the kernel program and the userspace path" :cells (y) :evidence "66d8282, 037c71c; ipv6_frame_matches_golden")
    (:feature "Kernel filter counters through the transport counter surface" :cells (y) :evidence "394dc9f the prefilter counter capability; 5d736db the xdp driver adapter; prefilter_counters_map_each_kernel_counter, prefilter_counters_are_assigned_not_added, prefilter_counters_xdp_driver_reports_them" :note "the live per-CPU read through a committed XDP socket is exercised only on the dedicated XDP machine")
    (:feature "Kernel replay protection with a read-only high-water map" :cells (y) :evidence "bace052 tla/RNXdpReplay.tla; 7013a56 kernel rule and xdp_host_replay_never_writes_the_map, xdp_host_replay_drops_behind_the_window; 26f2fac socket writer; 22f87a9 transport walk; 4f1d2a9 xdp_host_replay_table_set_get_reset; 3e1fd05 replay_drops_behind_the_window, replay_never_writes_the_map, replay_key_layout_matches_header; 306bd2b STANDARD.md sizing")
    (:feature "STANDARD.md states the kernel replay map is sized apart from the userspace replay window" :cells (n) :note "the standard states one rule today and is not to be quietly narrowed")
    (:feature "Packet blaster and simulated-client harnesses at line rate" :cells (n) :note "its own rung: a packet blaster load to prove line rate, and simulated clients against the XDP server")
    (:feature "Ownership and retry paths regression-validated against the 10 Gbps baseline" :cells (n) :note "the changed XDP ownership and retry paths, against the game deployment; CI runs no privileged XDP traffic and test executables forbid privileged commit")
    (:feature "Privileged syscall-failure coverage in its own lab" :cells (n) :note "targeted syscall-failure and frame-conservation tests cover different conditions from throughput tests")))

  (:id "scaling" :title "Scaling and load" :milestone "next"
   :text "Many channels and many connections, measured."
   :columns ("done")
   :rows
   ((:feature "1000-client in-process swarm with churn and garbage traffic" :cells (y) :evidence "a2f59cf; rocketnet_test_swarm")
    (:feature "Channel count a build bound from 1 to 65536" :cells (y) :evidence "ROCKETNET_MAX_CHANNELS with #error bound in sdk/rocketnet.h")
    (:feature "Nightly bounded soak" :cells (y) :evidence "846ae3d; .github/workflows/nightly-soak.yml")
    (:feature "Message layer stressed at 1024 channels" :cells (n))
    (:feature "Message layer stressed at 10k channels" :cells (n))
    (:feature "About 1000 real connections with channels on, at game-like load" :cells (n))
    (:feature "Socket-level swarm with Go orchestration against real transports" :cells (n) :note "the in-process swarm, at the socket level")
    (:feature "Multi-day soaks past 2^32 packets on one connection" :cells (n) :note "soak_sim and soak run unbounded on real hardware for days of parallel clients before the connect layer is believed; polite and garbage traffic, counters asserted while running, past 2^32 packets on one connection so sequence wrap is routine. The nightly CI soak is the bounded sentinel")))

  (:id "verification" :title "Verification"
   :text "How RocketNet is checked: unit tests without sockets or real time, functional tests, fuzzing, proofs and models."
   :columns ("done")
   :rows
   ((:feature "Unit suite with no sockets or real time" :cells (y) :evidence "test_unit_registry.h; socket tests in functional/")
    (:feature "Functional suite over simulated lossy links" :cells (y) :evidence "afa7ed8, 3d4f5b3; rocketnet_test_client_server_lossy_wire")
    (:feature "Seeded network simulator" :cells (y) :evidence "a2f59cf; rocketnet_test_network_simulator")
    (:feature "Fuzz targets for packets, tokens and messages" :cells (y) :evidence "fuzz/rocketnet_fuzz*.cpp; .github/workflows/fuzz-nightly.yml")
    (:feature "CBMC proofs of bounded readers and fragment bounds" :cells (y) :evidence "fa39b10, 7cb3cab")
    (:feature "TLA+ models: connection, security, replay, channels, window, scheduler, slots, receipts" :cells (y) :evidence "tla/*.tla; .github/workflows/tlc.yml")
    (:feature "Sanitizers: ASan, UBSan, TSan, MSan, LSan" :cells (y) :evidence "ci.yml, msan.yml, nightly-leaks.yml 079c3fa")
    (:feature "Big-endian (s390x) runs" :cells (y) :evidence "9c82ef8; nightly-bigendian-system.yml")
    (:feature "TLA+ model of transport fragments and acks" :cells (n) :note "TLA-INVENTORY M8, M9")
    (:feature "TLA+ models of blocks and the endpoint" :cells (n) :note "CHANNELS.md names RNBlocks and RNEndpoint")
    (:feature "Every modelled transition cites its module and action" :cells (n) :note "tla/COVERAGE.tsv started")
    (:feature "Connection and security machines as pure modules" :cells (n))))

  (:id "platforms" :title "Platforms"
   :text "Where RocketNet builds and passes."
   :columns ("linux" "macos" "windows")
   :rows
   ((:feature "Per-commit CI, Debug and Release" :cells (y y y) :evidence "ci.yml: native Debug and Release, portable and auto driver selection, serialization, s390x, sanitizers, bounded simulated soak, corpus replay, standard-value conformance")
    (:feature "Shared export list compared" :cells (y y y) :evidence "tools/check_exports.py, tools/exports.txt; 095f374 production sources separate from tests, strict endpoint port parsing")
    (:feature "Nightly soak" :cells (y y n) :evidence "nightly-soak.yml; 63a25be macOS")))

  (:id "api" :title "Public API"
   :text "The C surface in sdk/rocketnet.h and what a consumer needs to use it."
   :columns ("done")
   :rows
   ((:feature "Transport C surface: rocketnet_client_transport_* and rocketnet_server_transport_*" :cells (y) :evidence "ruled #77; sdk/rocketnet.h; packet contexts checked against single-consumption leases: test_lifecycle.cpp check_packet_leases")
    (:feature "Root header pure C: the two transport types with functions, the address type, nothing else" :cells (n) :note "ruled: netcode-style, real non-mangled symbols for C and FFI; a C++ inside is an implementation privilege. The public C API surface (#22), designed outside in, is its usage spec and an input to the top-layer client and server objects")
    (:feature "Message objects in C: rocketnet_message_client_* and rocketnet_message_server_*" :cells (y) :evidence "2c1aa7b")
    (:feature "ABI revision and layout queries" :cells (y) :evidence "sdk/rocketnet_abi.cpp; tools/abi_mismatch.cpp")
    (:feature "Installable CMake package with C and C++ consumers" :cells (y) :evidence "tools/check_package.py, tools/package_consumer")
    (:feature "Vendored sodium, no system libsodium" :cells (y) :evidence "23d4c40; sodium/")
    (:feature "Structured create errors and FFI-safe logging" :cells (n) :note "specified bfbdc94; for C ABI consumers")
    (:feature "Sized APIs for compatible struct evolution" :cells (n) :note "wire version 1.0.0 does not freeze the native ABI")
    (:feature "Whole-transport memory estimates and operator budgets (#155)" :cells (n) :note "all components, kernel buffers, thread stacks, allocator overhead and application memory; the component limits refuse the documented oversized geometry but do not prove a host affords every configuration below them")
    (:feature "C++ ClientTransport and ServerTransport interfaces" :cells (n) :note "light C++ like the game: the free-function transport surfaces map one to one onto methods, the portable implementations become the first concrete transports, the protocol core stays. The original motive, runtime selection, landed without them (#106), so what is left is the C++ ergonomics on their own merits")))

  (:id "release" :title "Release and process"
   :columns ("done")
   :rows
   ((:feature "Release hardening flags" :cells (y) :evidence "fe7966f; tools/check_hardening.py")
    (:feature "Reproducible vendored sources" :cells (y) :evidence "23d4c40 sodium; f6457b5 XDP archive" :note "reproducible release dependencies as a whole are still owed")
    (:feature "First integrating game vendors the SDK and proves it (release gate)" :cells (n) :note "held. The ultimate soak: vendored byte for byte, the game server soaks it under real conditions. The game loop pegs to real time with drop-frame detection, so update stays non-blocking with time passed in and bounded walks, and nothing in the transports blocks or takes unbounded time in update")
    (:feature "Supported-platform policy" :cells (n))
    (:feature "Security disclosure and release process" :cells (n))
    (:feature "Independent review of the transport repair diff, received" :cells (n) :note "the repair of findings #143 to #159; a review request sent is not a review received or an approval")
    (:feature "The old to-do and readiness files retired into this roadmap" :cells (y) :evidence "rocketnet-retire-todo-and-readiness" :note "TODO.md and TRANSPORT_READINESS.md deleted; every open item is a row here")))))
