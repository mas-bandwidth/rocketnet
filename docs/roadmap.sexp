; RocketNet roadmap data. ROADMAP.md is rendered from this file by
; tools/roadmap/render.py (make roadmap); make roadmap-check fails when the
; committed page is not what this renders.
;
; A cell is y (done, with :evidence naming a commit or a test), n (not done)
; or - (does not apply). Evidence stays here; the page shows only the marks.
; Rows reconciled against main 5c51005 on 2026-10-17, and against the
; landed sprint cards on sprint/rocketnet-proof 384da4c on 2026-10-18. Retired from the old
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
    (:feature "The token is bound at response open" :cells (y) :evidence "b515ec6 rocketnet-token-bound-at-response-open; rocketnet_test_token_bound_at_response_open; tla/RNSecurityTables.tla")
    (:feature "Development keys are off by default and warn on every start" :cells (y) :evidence "6a580ef; dev_keys_warn_on_every_start")
    (:feature "An unauthenticated reply is never larger than its request" :cells (y) :evidence "38caeb2; reply_never_larger_than_request_every_unauthenticated_type, reply_never_larger_than_request_hostile_inputs")
    (:feature "Session layer: identity auth with a backend, rolling magic, keyed filter value (later)" :cells (n) :note "in part: the design is docs/SESSION.md (eb22aab), the keyed filter value and its two-generation epoch window are in (8e99124 keyed_filter_value_known_answers; ce1b1a0 filter_epoch_window_accepts_two_generations; tla/RNFilterEpochs.tla 4ea64ce), and one token holds one live connection (eef77ac); the backend identity auth and the rolling magic are not built. v2, optional, behind the first deliverable. The 6-byte header with its epoch byte is already in, so the keyed form is a configuration change, not a wire break. It needs a backend that tracks the connected set across servers live: one identity, one seat, so a shared token gives away the sharer's own slot, and the old player is kicked with rocketnet_server_transport_disconnect_client when the same user hash appears elsewhere. An extraction from the game's own backend, not a new design. Then the rolling magic feeds the keyed filter value (SipHash over type, length and the four bytes after the header, one MAC per packet), and a per-title generator emits the constant and the keyed reader in every language plus verifier-clean BPF behind a differential gate. Never: a default magic, or standalone IP pinning")))

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
    (:feature "Server transport entries carry a connection id" :cells (y) :evidence "65bab41; rocketnet_test_server_transport_entries_carry_connection_id")
    (:feature "Per-packet acks with delivery notification" :cells (y) :evidence "8af8b80; rocketnet_test_ack_block_validity, rocketnet_test_ack_block_build")
    (:feature "Ack history that survives a long lossy path" :cells (y) :evidence "187a875; rocketnet_test_send_history_lossy_long_path; ROCKETNET_HISTORY_PACKETS 4096 default, 4096 to 32768 (sdk/rocketnet_history.h), no frame, send or ack rate assumed; test_lifecycle.cpp check_ack_retention, rocketnet_test_send_history_silent_peer_overflows")
    (:feature "Frame sync: a fixed-rate server loop with priority and drop detection" :cells (y) :evidence "rocketnet_test_frame_sync_advance, rocketnet_test_frame_sync_guard")
    (:feature "Counters on every path, each classified, conserved and named by a test" :cells (y) :evidence "1750e83 rocketnet-counters-classified-and-conserved; cumulative per-connection wire totals af8c0b0 and race-free 800119c (wire_totals_count_every_packet_and_byte, rocketnet_test_transport_wire_totals_concurrent_send_exact); rocketnet_num_counters and rocketnet_counter_value for FFI callers; packet-rate statistics in elapsed-time buckets: test_lifecycle.cpp check_stats_cadence")))

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
    (:feature "Send budget in kbps per connection" :cells (y) :evidence "a4ff908; budgets_are_per_connection; a configured default 8d9b806 (send_budget_defaults_from_config, send_budget_env_overrides_config)")
    (:feature "Resend delay from the measured round trip, block chunks paced by it, no backoff" :cells (y) :evidence "f6a3068, 2322d7c; resend_delay_is_min_of_interval_and_rtt, no_backoff_between_resends, chunk_resend_paced_by_rtt_not_interval; tla/RNResendTiming.tla")
    (:feature "A 1400-byte game message fits one packet" :cells (y) :evidence "ff4c7c6; message_of_1400_bytes_is_one_unfragmented_packet")
    (:feature "One fixed region per connection, no allocation after create" :cells (y) :evidence "8b99c72; no_allocation_after_create")
    (:feature "Caller-supplied memory block" :cells (y) :evidence "user_block_is_used_and_nothing_else; aligned allocation keeps its callback and context, late replacement refused: rocketnet_test_allocator_refusal_is_returned, test_lifecycle.cpp check_component_create_sweeps")
    (:feature "Memory exhaustion kicks only that connection" :cells (y) :evidence "c335529; server_kicks_exhausted_client_others_unaffected")
    (:feature "Hostile records and chunks refused" :cells (y) :evidence "d1ff1a2; hostile_chunk_count_or_index_kicks, hostile_small_at_held_ordering_is_refused")
    (:feature "Endpoint and slots as pure machines (CHANNELS.md layer 7)" :cells (y) :evidence "bbb62e8 endpoint_back_to_back_over_lossy_array, endpoint_never_allocates_or_reads_a_clock; 9bca948 slots_observe_and_admit; tla/RNEndpoint.tla 3c3c048")
    (:feature "CONFIG announcement: both ends prove the same configuration" :cells (y) :evidence "1026843 message_config_equal_connects_and_verifies, message_config_mismatch_ends_both_connections; functional 4906e5d")
    (:feature "Message layer soaked at full configured load" :cells (n) :note "bounded exhaustion soak c335529; the many-client load harness bin/load (docs/LOAD.md, f49cc28) is built, a recorded full-load run is owed")
    (:feature "Broadcast send" :cells (y) :evidence "7e2106e; message_server_broadcast_reaches_every_connected, message_server_broadcast_skips_free_and_exhausted")
    (:feature "Flows: server-side subscription groups (later)" :cells (n) :note "REQUIREMENTS.md step 3; designed and modelled, not built: docs/FLOWS.md, tla/RNFlows.tla (2c7236b)")))

  (:id "drivers" :title "Drivers"
   :text "The server socket drivers below the transport, selected at runtime by the per-endpoint ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT; the old names ROCKETNET_DRIVER, ROCKETNET_TRANSPORT, ROCKETNET_SERVER_DRIVER and ROCKETNET_CLIENT_DRIVER are refused aloud and never applied (sdk/rocketnet_config.cpp). Loopback is pin-only."
   :columns ("portable" "gso" "msgx" "iocp" "xdp" "io_uring" "loopback" "rio")
   :rows
    ((:feature "Server driver" :cells (y y y y y y y y) :evidence "rio 367a33f, 1dcb6b8 (rocketnet_test_server_rio_probe, rocketnet_test_server_rio_wire_ipv4, rocketnet_test_server_rio_wire_ipv6, rocketnet_test_server_rio_wire_dual_stack, rocketnet_test_server_rio_send_full; tla/RNRioDriver.tla 5255a0d); portable: rocketnet_server_socket_linux.cpp; gso 0801c9a; msgx 8d54e30; iocp 45da97d; xdp c0b0928; io_uring f7cfe0f and 7be058d (test_server_uring_probe, test_server_uring_wire_ipv4, test_server_uring_wire_ipv6, test_server_uring_wire_dual_stack, test_server_uring_send_overflow, test_server_uring_receive_rearms_after_no_buffers); loopback bfcb888 (driver_loopback_binds_and_discovers_a_port, driver_loopback_exchanges_both_ways, driver_loopback_full_queue_drops_and_counts, driver_loopback_destroy_frees_the_port)")
     (:feature "Selectable by pin at runtime" :cells (y y y y y y y y) :evidence "rio 0c31bf7 (rocketnet_test_server_driver_pin_rio); 10308e4; rocketnet_test_server_driver_pin_portable, _gso, _msgx, _iocp, _xdp; io_uring 2d2c057 (test_server_driver_pin_io_uring); loopback 5d059d8 and 1c6f91a (test_driver_pin_only, test_server_driver_pin_loopback, test_driver_env_pins_loopback)")))

  (:id "planned-drivers" :title "Planned drivers and clients"
   :text "The driver and client work carded today, and the later milestones behind it."
   :columns ("done")
   :rows
   ((:feature "Linux UDP_GRO receive coalescing" :cells (y) :evidence "the gso driver enables UDP_GRO per socket and splits by the stride cmsg; GRO_BATCHES in the gso wire case")
    (:feature "Windows Registered I/O (RIO) server driver" :cells (y) :evidence "367a33f, 1dcb6b8, 0c31bf7; rocketnet_test_server_rio_wire_ipv4, rocketnet_test_server_driver_pin_rio; tla/RNRioDriver.tla 5255a0d; the rio column of the Drivers table")
    (:feature "Windows USO and URO in the IOCP driver, probed with fallback" :cells (y) :evidence "a8ffbac; rocketnet_test_server_iocp_uso_uro_probe, rocketnet_test_server_iocp_uso_wire; the coalesce codec 7df4c3c (coalesce_split_by_stride_round_trips)")
    (:feature "Client socket I/O off the main thread, gated by the measured main-thread cost per frame" :cells (y) :evidence "docs/CLIENT-IO.md b0b4527, tla/RNClientIo.tla; 4f8a8e9 client_io_backend_with_wait_uses_io_thread; Linux a60ed16, Apple 151508f, Windows 8ddc93e (rocketnet_test_client_io_linux_handoff_round_trip, rocketnet_test_client_io_apple_handoff_round_trip, rocketnet_test_client_io_windows_handoff_round_trip); decrypt off the main thread c236806; the 100 us gate 20d8b35, bench/client_main_thread_cost.md, .github/workflows/nightly-frame-budget.yml f12db2b")
    (:feature "Console client drivers (PS4, PS5, Switch, Xbox GDK)" :cells (n) :note "the source layers are under a different license; the owner's ruling is owed before any card")
    (:feature "XDP/AF_XDP on AWS GameLift fleets: research" :cells (y) :evidence "6930277 docs/GAMELIFT.md")))

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
    (:feature "STANDARD.md states the kernel replay map is sized apart from the userspace replay window" :cells (y) :evidence "c1a6ff2 rocketnet-standard-sizes-kernel-replay-separately; STANDARD.md Replay window sizing (ROCKETNET_XDP_REPLAY_WINDOW)")
    (:feature "AF_XDP bind requests zero-copy" :cells (y) :evidence "6bb1361; xdp_socket_config_requests_zero_copy_at_bind")
    (:feature "Packet blaster and simulated-client harnesses at line rate" :cells (n) :note "its own rung: a packet blaster load to prove line rate, and simulated clients against the XDP server. The live XDP fleet run is designed (docs/LIVE-XDP.md, tla/RNLiveFleetRun.tla, bb9b36e) and its runner scripts are in (51f0fae); the run itself is owed")
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
    (:feature "About 1000 real connections with channels on, at game-like load" :cells (n) :note "the harness is built: bin/load, up to ROCKETNET_MAX_CLIENTS (1000, the default since 962e3a4) clients over loopback on every reliability, five pass criteria (docs/LOAD.md, f49cc28); a recorded 1000-client run is owed")
    (:feature "Socket-level swarm with Go orchestration against real transports" :cells (n) :note "the in-process swarm, at the socket level")
    (:feature "Multi-day soaks past 2^32 packets on one connection" :cells (n) :note "soak_sim and soak run unbounded on real hardware for days of parallel clients before the connect layer is believed; polite and garbage traffic, counters asserted while running, past 2^32 packets on one connection so sequence wrap is routine. The nightly CI soak is the bounded sentinel")))

  (:id "verification" :title "Verification"
   :text "How RocketNet is checked: unit tests without sockets or real time, functional tests, fuzzing, proofs and models."
   :columns ("done")
   :rows
   ((:feature "Unit suite with no sockets or real time" :cells (y) :evidence "test_unit_registry.h; socket tests in functional/")
    (:feature "Functional suite over simulated lossy links" :cells (y) :evidence "afa7ed8, 3d4f5b3; rocketnet_test_client_server_lossy_wire")
    (:feature "Seeded network simulator" :cells (y) :evidence "a2f59cf; rocketnet_test_network_simulator")
    (:feature "Fuzz targets for packets, tokens and messages" :cells (y) :evidence "fuzz/rocketnet_fuzz*.cpp; .github/workflows/fuzz-nightly.yml; every target smoked per commit ee35a0b; corpus replay on macOS and Windows af9c366")
    (:feature "Captured pcapng corpus replayed per commit and seeding the fuzzers" :cells (y) :evidence "b4be701 pcapng_corpus_replay, pcapng_corpus_witness_unread_packet_goes_red; 0416321 pcapng_corpus_covers_control_packet_types; seeds 8cceab2; fuzz findings become pcapng regressions fcfe160 (tools/check_fuzz_regressions.py)")
    (:feature "Wycheproof vectors for ChaCha20-Poly1305, Ed25519 and X25519" :cells (y) :evidence "f6fa899 test/wycheproof; wycheproof_chacha20_poly1305_vectors 3fbe4dc, wycheproof_ed25519_vectors 410a517, wycheproof_x25519_vectors 2798d97, each with a witness that goes red")
    (:feature "Happy-path end to end: the real server and client binaries as separate processes" :cells (y) :evidence "1b95d8a tools/happy_path; the happy-path job in ci.yml")
    (:feature "CBMC proofs of bounded readers and fragment bounds" :cells (y) :evidence "fa39b10, 7cb3cab; proofs/cbmc: connect token read 4255608, packet prefilter open 03ea6b9, message read max 5dc66fd, fragment reassembly 2d1d4ae, replay advance 4d58844; .github/workflows/nightly-proofs.yml")
    (:feature "TLA+ models: connection, security, replay, channels, window, scheduler, slots, receipts" :cells (y) :evidence "tla/*.tla; .github/workflows/tlc.yml")
    (:feature "Sanitizers: ASan, UBSan, TSan, MSan, LSan" :cells (y) :evidence "ci.yml, msan.yml, nightly-leaks.yml 079c3fa")
    (:feature "Big-endian (s390x) runs" :cells (y) :evidence "9c82ef8; nightly-bigendian-system.yml")
    (:feature "TLA+ model of transport fragments and acks" :cells (y) :evidence "e65b838 tla/FragmentsAndAcks.tla; the ack walk bitmaps 91383ff")
    (:feature "TLA+ models of blocks and the endpoint" :cells (y) :evidence "e21fb78 tla/RNBlocks.tla; 3c3c048 tla/RNEndpoint.tla")
    (:feature "Every modelled transition cites its module and action" :cells (n) :note "in part: every TLA+ module has a tla/COVERAGE.tsv row whose code file cites it, checked by tools/check_tla_coverage.py --all in tlc.yml (617d498, fb828d0, and one card per module); the check is per module, so a citation of each action is not yet checked")
    (:feature "Connection and security machines as pure modules" :cells (n) :note "audited, not extracted: docs/PURITY.md (6432754) gives each machine a verdict (client slots, token tables and the replay window pure; the client connection, the per-address handshake, keep-alives and the cookie gate impure) and lists the extraction cards")))

  (:id "platforms" :title "Platforms"
   :text "Where RocketNet builds and passes."
   :columns ("linux" "macos" "windows")
   :rows
   ((:feature "Per-commit CI, Debug and Release" :cells (y y y) :evidence "ci.yml: native Debug and Release, portable and auto driver selection, serialization, s390x, sanitizers, bounded simulated soak, corpus replay, standard-value conformance")
    (:feature "Shared export list compared" :cells (y y y) :evidence "tools/check_exports.py, tools/exports.txt; 095f374 production sources separate from tests, strict endpoint port parsing")
    (:feature "Nightly soak" :cells (y y y) :evidence "nightly-soak.yml; 63a25be macOS; 9ee42a2 Windows")))

  (:id "api" :title "Public API"
   :text "The C surface in sdk/rocketnet.h and what a consumer needs to use it."
   :columns ("done")
   :rows
   ((:feature "Transport C surface: rocketnet_client_transport_* and rocketnet_server_transport_*" :cells (y) :evidence "ruled #77; sdk/rocketnet.h; packet contexts checked against single-consumption leases: test_lifecycle.cpp check_packet_leases")
    (:feature "Root header pure C: the client and server with their functions, the address type, nothing else" :cells (y) :evidence "5c3027f, dcd941f rocketnet-header-is-clean-c (6b73068); sdk/rocketnet.h plain C99 with one extern C block; rocketnet_test_public_client_server_queries, rocketnet_test_config_refuses_rocketnet_driver; clean at /W4 and -Wextra 9ead512" :note "ruled: netcode-style, real non-mangled symbols for C and FFI; a C++ inside is an implementation privilege")
    (:feature "Message objects in C: rocketnet_message_client_* and rocketnet_message_server_*" :cells (y) :evidence "2c1aa7b")
    (:feature "ABI revision and layout queries" :cells (y) :evidence "sdk/rocketnet_abi.cpp; tools/abi_mismatch.cpp")
    (:feature "Installable CMake package with C and C++ consumers" :cells (y) :evidence "tools/check_package.py, tools/package_consumer")
    (:feature "Vendored sodium, no system libsodium" :cells (y) :evidence "23d4c40; sodium/")
    (:feature "Structured create errors and FFI-safe logging" :cells (y) :evidence "status codes 4e2d6f1 (status_strings_cover_every_code); create_ex 3e82436, b2bc4ea, 33f84d3 (server_transport_create_ex_reports_every_status, client_transport_create_ex_reports_every_status, lifecycle_ex_reports_every_status); preformatted log callback 194ecc5 (log_callback_preformatted_contract)")
    (:feature "Sized APIs for compatible struct evolution" :cells (y) :evidence "a5941cf; sized_config_accepts_every_known_size, sized_config_refuses_unknown_size; docs/ABI-EVOLUTION.md")
    (:feature "Whole-transport memory estimates and operator budgets (#155)" :cells (y) :evidence "rocketnet_server_required_memory and rocketnet_client_required_memory 1d9f830; the host memory estimate cad7a0c (rocketnet_test_server_host_memory_estimate_bounds_the_plan); operator budgets 227d895 USAGE.md; BUILDING.md Pre-allocation server resource plans" :note "an estimate of what is requested, with its exclusions named (kernel accounting beyond the requests, libbpf and libxdp, guard pages, application memory); not an available-RAM check")
    (:feature "C++ ClientTransport and ServerTransport interfaces" :cells (y) :evidence "925cbd7 cpp_transport_interfaces_map_one_to_one, cpp_transport_fake_implements_the_interface; 8985b00 rocketnet_test_cpp_transports_connect_over_loopback")
    (:feature "Shared library build, checked exports, callable from C and C++" :cells (y) :evidence "73733ad tools/check_header_exports.py; a26fd6c export count derived; a6e3991 tools/check_dll_consumer.py, tools/dll_consumer")
    (:feature "API reference generated by MrDocs" :cells (y) :evidence "baf841d mrdocs.yml, docs/mrdocs, .github/workflows/docs.yml")
    (:feature "USAGE.md: the client and server API, with worked usage" :cells (y) :evidence "b7f92c4, d0a5339")))

  (:id "release" :title "Release and process"
   :columns ("done")
   :rows
   ((:feature "Release hardening flags" :cells (y) :evidence "fe7966f; tools/check_hardening.py")
    (:feature "Reproducible vendored sources" :cells (y) :evidence "23d4c40 sodium; f6457b5 XDP archive" :note "reproducible release dependencies as a whole are still owed")
    (:feature "First integrating game vendors the SDK and proves it (release gate)" :cells (n) :note "held. The ultimate soak: vendored byte for byte, the game server soaks it under real conditions. The game loop pegs to real time with drop-frame detection, so update stays non-blocking with time passed in and bounded walks, and nothing in the transports blocks or takes unbounded time in update")
    (:feature "Supported-platform policy" :cells (y) :evidence "34406ce docs/PLATFORMS.md; tools/check_platform_policy.py")
    (:feature "Security disclosure and release process" :cells (y) :evidence "6795ac1 SECURITY.md, docs/RELEASING.md; tools/check_release_doc.py")
    (:feature "The example server runs out of the box" :cells (y) :evidence "14557ab")
    (:feature "The normative standard in standard/, a conformance vector per section" :cells (y) :evidence "61c0e67 skeleton and tools/conformance/check_rn_standard.py; one card per section, each with its vectors (standard_wire_primitives_vectors 062e2cb through standard_channels_receipts_security_vectors 263a088); standard/RATIONALE.md 3ba4ff2")
    (:feature "Independent review of the transport repair diff, received" :cells (n) :note "the repair of findings #143 to #159; a review request sent is not a review received or an approval")
    (:feature "The old to-do and readiness files retired into this roadmap" :cells (y) :evidence "rocketnet-retire-todo-and-readiness" :note "TODO.md and TRANSPORT_READINESS.md deleted; every open item is a row here")))
  (:id "landed-2026-10-19" :title "Landed in the sprint since the 2026-10-18 roadmap record"
   :text "Cards landed on sprint/rocketnet-proof after the 2026-10-18 roadmap record (782b0ba), one row per card, its id the card id, with the landing merge commit as evidence. They reach main when v1.0.0 promotes the sprint branch."
   :columns ("done")
   :rows
   (
    (:id "rocketnet-salt-mismatch-resends-challenge" :feature "T1: a request with another salt resends the stored challenge" :cells (y) :evidence "43626fc land rocketnet-salt-mismatch-resends-challenge on sprint/rocketnet-proof")
    (:id "rocketnet-standard-vector-index-on-demand" :feature "Print the standard's vector index on demand instead of committing it" :cells (y) :evidence "a11d924 land rocketnet-standard-vector-index-on-demand on sprint/rocketnet-proof")
    (:id "rocketnet-example-hello" :feature "hello: the least rocketnet program that works, and the home of the example series" :cells (y) :evidence "1581cc4 land rocketnet-example-hello on sprint/rocketnet-proof")
    (:id "rocketnet-example-big-messages" :feature "big_messages: a 200 KB reliable message, and an oversize unreliable send promoted" :cells (y) :evidence "a3722bb land rocketnet-example-big-messages on sprint/rocketnet-proof")
    (:id "rocketnet-token-expired-reasons-split" :feature "T2: split the merged TOKEN_EXPIRED refusals" :cells (y) :evidence "9b0b2ee land rocketnet-token-expired-reasons-split on sprint/rocketnet-proof")
    (:id "rocketnet-standard-session-r2" :feature "The rocketnet standard, section t13: Session: keep-alive, timeout, disconnect, slot reset" :cells (y) :evidence "6595114 land rocketnet-standard-session-r2 on sprint/rocketnet-proof")
    (:id "rocketnet-example-client-server" :feature "client + server: two programs over real UDP, with keys, a matchmaker stub and connect tokens" :cells (y) :evidence "f2b109f land rocketnet-example-client-server on sprint/rocketnet-proof")
    (:id "rocketnet-example-stats" :feature "stats: state and counters through the public queries" :cells (y) :evidence "e580649 land rocketnet-example-stats on sprint/rocketnet-proof")
    (:id "rocketnet-example-budgets" :feature "budgets: send budgets and over-budget behaviour" :cells (y) :evidence "a068af7 land rocketnet-example-budgets on sprint/rocketnet-proof")
    (:id "rocketnet-example-config-env" :feature "config_env: default, struct, environment, and a refused bad value" :cells (y) :evidence "64da826 land rocketnet-example-config-env on sprint/rocketnet-proof")
    (:id "rocketnet-example-transports" :feature "transports: auto, a pin, and a list, chosen and said out loud" :cells (y) :evidence "e87104d land rocketnet-example-transports on sprint/rocketnet-proof")
    (:id "rocketnet-live-message-matrix-functional-r2" :feature "Every size, every type, every channel, both directions, every message verified: the loopback matrix" :cells (y) :evidence "b0e8c84 land rocketnet-live-message-matrix-functional-r2 on sprint/rocketnet-proof")
    (:id "rocketnet-live-xdp-fleet-runner-client" :feature "The live stress runner, piece 1: the client stress mode and the merge" :cells (y) :evidence "211833e land rocketnet-live-xdp-fleet-runner-client on sprint/rocketnet-proof")
    (:id "rocketnet-dll-callable-tests-macos" :feature "The library is callable from a DLL on macOS" :cells (y) :evidence "eb50adb land rocketnet-dll-callable-tests-macos on sprint/rocketnet-proof")
    (:id "rocketnet-dst-design" :feature "Deterministic simulation testing for rocketnet: the design" :cells (y) :evidence "1a2aab4 land rocketnet-dst-design on sprint/rocketnet-proof")
    (:id "rocketnet-dll-callable-tests-windows" :feature "The library is callable from a DLL on Windows" :cells (y) :evidence "ae06e12 land rocketnet-dll-callable-tests-windows on sprint/rocketnet-proof")
    (:id "rocketnet-xdp-on-link-reply-uses-peer-mac" :feature "XDP replies go to the peer's own MAC address on link, to the gateway otherwise" :cells (y) :evidence "f435279 land rocketnet-xdp-on-link-reply-uses-peer-mac on sprint/rocketnet-proof")
    (:id "rocketnet-dst-fault-model" :feature "The DST's fault injector, as a TLA+ model first" :cells (y) :evidence "b522f5a land rocketnet-dst-fault-model on sprint/rocketnet-proof")
    (:id "rocketnet-standard-config-record" :feature "The rocketnet standard, section m11: CONFIG control record" :cells (y) :evidence "77dd5c2 land rocketnet-standard-config-record on sprint/rocketnet-proof")
    (:id "rocketnet-run-msan-fails-on-crashed-test-b" :feature "The MSan gate fails on crashed tests" :cells (y) :evidence "cafa290 land rocketnet-run-msan-fails-on-crashed-test-b on sprint/rocketnet-proof")
    (:id "rocketnet-ci-fleet-runners-lint" :feature "rocketnet CI: the fleet-runner lint, and the per-push workflows on the fleet" :cells (y) :evidence "e529c71 land rocketnet-ci-fleet-runners-lint on sprint/rocketnet-proof")
    (:id "rocketnet-example-game-loop" :feature "game_loop: a 60 Hz server and N clients, against the 100 microsecond client budget" :cells (y) :evidence "ab83099 land rocketnet-example-game-loop on sprint/rocketnet-proof")
    (:id "rocketnet-base-wire-totals-staged-finish-green" :feature "The sprint base green again: the staged-finish wire totals case and the MemorySanitizer check fixed" :cells (y) :evidence "8760e60 land rocketnet-base-wire-totals-staged-finish-green on sprint/rocketnet-proof")
    (:id "rocketnet-public-backend-keypair-from-seed" :feature "A backend keypair can be derived from a seed through rocketnet.h" :cells (y) :evidence "b1c5e74 land rocketnet-public-backend-keypair-from-seed on sprint/rocketnet-proof")
    (:id "rocketnet-loopback-transport-carries-max-clients" :feature "The loopback transport holds max clients plus the server" :cells (y) :evidence "ad98d7b land rocketnet-loopback-transport-carries-max-clients on sprint/rocketnet-proof")
    (:id "rocketnet-nightly-on-fleet" :feature "rocketnet CI: the nightly workflows on the fleet" :cells (y) :evidence "4f24f4d land rocketnet-nightly-on-fleet on sprint/rocketnet-proof")
    (:id "rocketnet-dst-harness-core" :feature "DST layer 1: the seeded core, N clients and a server, one seed, exact replay" :cells (y) :evidence "135a41c land rocketnet-dst-harness-core on sprint/rocketnet-proof")
    (:id "rocketnet-public-not-released-counters" :feature "The unreleased-message refusal is a public counter on client and server" :cells (y) :evidence "5a6cc45 land rocketnet-public-not-released-counters on sprint/rocketnet-proof")
    (:id "rocketnet-live-xdp-fleet-runner-server" :feature "The live stress runner, piece 2: the server stress mode" :cells (y) :evidence "1d4af4c land rocketnet-live-xdp-fleet-runner-server on sprint/rocketnet-proof")
    (:id "rocketnet-connect-token-time-from-game-clock" :feature "Connect-token create and expire checks can read the game clock" :cells (y) :evidence "9efc7cb land rocketnet-connect-token-time-from-game-clock on sprint/rocketnet-proof")
    (:id "rocketnet-public-step-driven-loopback-client" :feature "A public client can run with no I/O thread, stepped by the game's update" :cells (y) :evidence "16a7c0a land rocketnet-public-step-driven-loopback-client on sprint/rocketnet-proof")
    (:id "rocketnet-dst-invariant-checks" :feature "DST layer 3: the TLA+ properties as runtime checks" :cells (y) :evidence "1685d93 land rocketnet-dst-invariant-checks on sprint/rocketnet-proof")
    (:id "rocketnet-docs-platform" :feature "API reference comments: platform" :cells (y) :evidence "2c2fac0 land rocketnet-docs-platform on sprint/rocketnet-proof")
    (:id "rocketnet-server-receive-drains-backlog" :feature "The server drains its receive queue up to a per-update budget" :cells (y) :evidence "c61badc land rocketnet-server-receive-drains-backlog on sprint/rocketnet-proof")
    (:id "rocketnet-docs-reliability" :feature "API reference comments: reliability" :cells (y) :evidence "90683f2 land rocketnet-docs-reliability on sprint/rocketnet-proof")
    (:id "rocketnet-public-link-simulator-t" :feature "The development library can simulate a bad link, through config alone" :cells (y) :evidence "5182763 land rocketnet-public-link-simulator-t on sprint/rocketnet-proof")
    (:id "rocketnet-unreliable-size-one-rule" :feature "Oversize unreliable sends are promoted to their reliable twin, as RakNet does, and said so" :cells (y) :evidence "9121067 land rocketnet-unreliable-size-one-rule on sprint/rocketnet-proof")
    (:id "rocketnet-standard-key-schedule" :feature "The rocketnet standard, section t7: Key schedule" :cells (y) :evidence "1701719 land rocketnet-standard-key-schedule on sprint/rocketnet-proof")
    (:id "rocketnet-standard-handshake-client" :feature "The rocketnet standard, section t11: Handshake: client" :cells (y) :evidence "3c314a9 land rocketnet-standard-handshake-client on sprint/rocketnet-proof")
    (:id "rocketnet-standard-packet-formats" :feature "The rocketnet standard, section t8: Packet formats" :cells (y) :evidence "94331b6 land rocketnet-standard-packet-formats on sprint/rocketnet-proof")
    (:id "rocketnet-standard-packet-protection" :feature "The rocketnet standard, section t9: Packet protection" :cells (y) :evidence "1f75abd land rocketnet-standard-packet-protection on sprint/rocketnet-proof")
    (:id "rocketnet-standard-connect-token" :feature "The rocketnet standard, section t6: Connect token" :cells (y) :evidence "192db19 land rocketnet-standard-connect-token on sprint/rocketnet-proof")
    (:id "rocketnet-standard-replay" :feature "The rocketnet standard, section t14: Replay protection" :cells (y) :evidence "37885ea land rocketnet-standard-replay on sprint/rocketnet-proof")
    (:id "rocketnet-channels-unreliable-oversize-counter-doc" :feature "rocketnet: CHANNELS.md names the counter an unreliable oversize send moves" :cells (y) :evidence "1ad9b49 land rocketnet-channels-unreliable-oversize-counter-doc on sprint/rocketnet-proof")
    (:id "rocketnet-standard-blocks" :feature "The rocketnet standard, section m8: Blocks" :cells (y) :evidence "96845d6 land rocketnet-standard-blocks on sprint/rocketnet-proof")
    (:id "rocketnet-standard-handshake-server" :feature "The rocketnet standard, section t12: Handshake: server" :cells (y) :evidence "17fb384 land rocketnet-standard-handshake-server on sprint/rocketnet-proof")
    (:id "rocketnet-standard-receiver-checks" :feature "The rocketnet standard, section m4: Receiver checks" :cells (y) :evidence "601c397 land rocketnet-standard-receiver-checks on sprint/rocketnet-proof")
    (:id "rocketnet-example-channels" :feature "channels: a stalled chat channel does not block positions" :cells (y) :evidence "96da6fb land rocketnet-example-channels on sprint/rocketnet-proof")
    (:id "rocketnet-example-ack-receipts" :feature "ack receipts: did my unreliable update arrive?" :cells (y) :evidence "bdef6ee land rocketnet-example-ack-receipts on sprint/rocketnet-proof")
    (:id "rocketnet-standard-retire-old" :feature "Retire the old STANDARD.md" :cells (y) :evidence "3ce29a1 land rocketnet-standard-retire-old on sprint/rocketnet-proof")
    (:id "rocketnet-cachegrind-instruction-gate" :feature "An instruction-count gate for the client frame and the server tick, on every push" :cells (y) :evidence "56a9840 land rocketnet-cachegrind-instruction-gate on sprint/rocketnet-proof")
    (:id "rocketnet-clang-format-from-netcode" :feature "A clang-format configuration derived from the owner's netcode style, with a format check" :cells (y) :evidence "612f270 land rocketnet-clang-format-from-netcode on sprint/rocketnet-proof" :note "the whole-tree reformat itself is rocketnet-clang-format-apply, in Sprint work moved to the roadmap")
    (:id "rocketnet-per-connection-reset-is-complete" :feature "Every piece of per-connection state is reset when a connection ends, in one function per side" :cells (y) :evidence "9043173 land rocketnet-per-connection-reset-is-complete on sprint/rocketnet-proof")))
  (:id "sprint-moved" :title "Sprint work moved to the roadmap (later)"
   :text "Cards moved out of the sprint on 2026-10-19, when Glenn moved everything not for the rocketnet v1.0.0 release to the roadmap. One row per card, its id the card id; each keeps the card's whole brief under :brief in the data, and a carried branch and head where an attempt had pushed one."
   :columns ("done")
   :rows
   (
    (:id "rocketnet-docs-complete-gate" :feature "The docs job fails on any undocumented public symbol" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-api"
     :brief "tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: feature
PRIORITY: normal
STREAM: rocketnet-api
DEPENDS-ON: rocketnet-mrdocs-build,rocketnet-docs-platform,rocketnet-docs-cpp,rocketnet-docs-counters,rocketnet-docs-reliability,rocketnet-docs-configuration,rocketnet-docs-connection,rocketnet-docs-packets,rocketnet-docs-messages,rocketnet-docs-lifecycle
SOURCE: Glenn, 2026-10-18: the API reference is fully documented once the nine rocketnet-docs-* cards land, and the MrDocs CI job keeps it so by failing on any undocumented public symbol. MrDocs 2026.9.29 src/mrdocs/ConfigOptions.json (read with gh api at tag 2026.9.29) has warn-if-undocumented (bool, default true: warn if a symbol that passes all filters is not documented), warn-if-undoc-enum-val (default true), warn-no-paramdoc (default true), warn-no-brief (default true), warn-if-doc-error (default true), warn-broken-ref (default true), warnings (default true) and warn-as-error (bool, default false: treat warnings as errors and stop the generation).
PATHS: Makefile
NEW: mrdocs.yml,tools/docs/mrdocs.sh
TEST: none because the change is MrDocs configuration; DONE-WHEN plants an undocumented public function in a scratch edit and shows the docs job's command fail on it, then reverts the plant
DONE-WHEN: (1) red at BASE: with a planted undocumented public function `int rocketnet_docs_gate_planted( int value );` added to sdk/rocketnet.h (a scratch edit, never committed), `make docs` exits 0 (keep the line); revert the plant and `git status --porcelain sdk` prints nothing; (2) at head, without the plant, `make docs` exits 0 and `tools/docs/undocumented.sh | wc -l` prints 0; (3) at head, with the same plant, `make docs` exits nonzero and its output names `rocketnet_docs_gate_planted` (keep the lines); `make docs-undocumented` still prints a line naming `rocketnet_docs_gate_planted`; revert the plant and `git status --porcelain sdk` prints nothing; (4) `grep -E '^ *warn-as-error: *true' mrdocs.yml` and `grep -E '^ *warn-if-undocumented: *true' mrdocs.yml` each print a line; (5) `git diff --stat <BASE>..HEAD` names only PATHS files and no file under sdk/; (6) `make && ./bin/test` is green
TOOLCHAIN: cmake and a C++17 compiler on the Linux member JOB.md names; MrDocs 2026.9.29 as `make docs` finds or fetches it
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 60 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 60 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; MrDocs 2026.9.29 as `make docs` runs it, and its own configuration options.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected server drivers (DRIVERS.md (read)). `make` builds into bin/; `./bin/test` is the unit suite (`./bin/test <name>` runs one case and prints `unknown unit test <name>`, exit 2, for a name it does not hold); `./bin/functional` is the loopback and simulated tier; `./bin/test_xdp_filter` is the kernel-tier XDP suite, run inside a virtme-ng VM. Root test_unit_<topic>.cpp files register with REGISTER_UNIT_TEST (test_unit_registry.h (read)). CMakeLists.txt keeps -ffp-contract=off on every target.

# The docs job fails on any undocumented public symbol

THE TASK. The nine rocketnet-docs-* cards have documented every public symbol, so `tools/docs/undocumented.sh` prints nothing at BASE. This card makes that permanent: the docs CI job (.github/workflows/docs.yml, which runs `make docs` and fails when it exits nonzero) must fail when a public symbol is undocumented. No doc text changes, no code changes, and no file outside PATHS and NEW changes. mrdocs.yml and tools/docs/mrdocs.sh are made by rocketnet-mrdocs-build, a need of this card; they are on NEW: only because they are not yet at the BASE tip when this card is added, and at the deal they exist and this card edits them. There is no state and no transition added, so no TLA+ module is owed.
1 CONFIG. In mrdocs.yml set `warn-if-undocumented: true`, `warn-if-undoc-enum-val: true`, `warn-no-paramdoc: true` and `warn-as-error: true`, keeping `warnings` on; read the option names and their meaning in MrDocs 2026.9.29's own option list (src/mrdocs/ConfigOptions.json at that tag, or `mrdocs --help` from the pinned binary) before writing them, and use those exact keys. If warn-as-error alone does not make `make docs` exit nonzero on the planted symbol (read the exit status, never assume it), find the setting or the Makefile/tools/docs/mrdocs.sh check that does, inside PATHS, and say which in the report.
2 THE REPORT STILL WORKS. `make docs-undocumented` and `tools/docs/undocumented.sh` still list every undocumented symbol, not just the first: if warn-as-error stops generation at the first warning, the undocumented report runs MrDocs with warn-as-error off (a command-line override or a job-local config copy, inside PATHS) so it still prints the whole list.
3 NO DOC TEXT. Never edit a comment or a header; the plant in DONE-WHEN is a scratch edit to sdk/rocketnet.h (read), reverted before the commit. If a warning other than an undocumented symbol (a doc error, a broken reference, a missing brief) makes `make docs` fail at head without the plant, do not fix the comment and do not turn that warning off: report it as not-done with the lines, the file and the line number, because a doc card owes it.

STEP 1. Confirm `uname -s` is Linux; cd into the staged checkout JOB.md names; git log --oneline -1. Run DONE-WHEN (1) and keep its lines.
STEP 2. Read MrDocs 2026.9.29's option list; set the options in mrdocs.yml; change the Makefile or tools/docs/mrdocs.sh only if step 1 or 2 of THE TASK needs it.
STEP 3. Run the gate: every DONE-WHEN command, the plant added and reverted for (3). Read the last lines. Report the measured time of `make docs` at head (the docs job runs in parallel with CI and must finish within a minute).
STEP 4. Commit on your own branch as `rocketnet-docs-complete-gate: <what>` with the trailer; end as JOB.md says.

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel program or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.

BUILD BOUND (the bench sandbox caps a job at 256 processes and 8 GiB, and the Makefile's build target runs an unbounded make -j). Wherever this card says `make` for the build, run instead `(mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. >/dev/null && nice make -j4 --no-print-directory)`, the same build bounded to four jobs. Run builds and suites one at a time, never in the background. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any CI-equivalent gate that reads git status.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the Linux member JOB.md names, never the coordinator's machine. Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-profile-pass-1-measure" :feature "Profile pass 1, measurement: run soak across the matrix and commit the raw numbers" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-api"
     :brief "RESULT: rocketnet-profile-pass-1-measure sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: report
PRIORITY: critical
STREAM: rocketnet-api
DEPENDS-ON: rocketnet-example-soak
SOURCE: Glenn, 2026-10-18, through the coordinator: \"we should profile our library and look for optimization opportunities\", and of the soak example: it \"will also be our first approach to real profiling!!!\". The milestone promises no load or line-rate numbers; this pass learns where the time goes.
SPLIT: the measurement half of rocketnet-profile-pass-1 (Glenn 10-18 split request: pro pieces, heavy only for the judgment). rocketnet-profile-pass-1-report (heavy) reads what this card commits and writes docs/PROFILE-1.md with the ranked opportunities; this card ranks nothing and writes no opportunities.
START: examples/soak.c (read), docs/examples/soak.md (read), DRIVERS.md (read), sdk/rocketnet.h (read)
NEW: docs/profile-1/README.md,docs/profile-1/runs.md
TEST: none because this card is measurement with no code change; DONE-WHEN names every command and artifact
DONE-WHEN: (1) red at BASE: docs/profile-1/runs.md does not exist; (2) at head `git diff --stat BASE..HEAD` touches only files under docs/profile-1/ (no library, example or build change, and no docs/PROFILE-1.md); (3) docs/profile-1/runs.md holds one row per run: machine label, CPU model, core count, OS and kernel, build type and flags, commit, transport, clients (1, 100, 1000), seconds, seed, the load average and top three other processes by CPU at the start of that run (`uptime` and `ps` output), noisy or clean, and the measured numbers: server tick p50/p99/max, client per-frame cost p50/p99/max, CPU per client (server CPU time / clients / seconds), bytes per packet, syscalls per packet (Linux `perf stat -e 'syscalls:sys_enter_*'` or `strace -c -f`, else not-measured with the reason), allocations per tick from soak's own counter; a row not run (xdp without the XDP machine staged, macOS without mini-m5-1 staged) is a line saying not-run and why; (4) for each 1000-client row: the flame graph SVG (FlameGraph scripts or `perf script | inferno`, whichever the member has; none fetched from the network: if absent, the folded stacks committed and the gap named), the folded stacks, and the top 10 of `perf report --no-children` and of `--children` as text, plus every row's raw soak results file, all under docs/profile-1/ named <machine>-<transport>-<clients>.svg/.folded/.top.txt/.tsv, and docs/profile-1/README.md lists every file; (5) each soak run's own PASS line is quoted in runs.md; a run that FAILs integrity is reported first, never a measured row
TOOLCHAIN: cmake (RelWithDebInfo build), perf and a flame-graph tool on Linux, xctrace on macOS, Python 3
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report with Verdict: LAND or HOLD, with not-done named
DEADLINE: finish within 240 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 300 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; perf, a flame-graph tool and xctrace are the instruments, the soak example the workload.

# Profile pass 1, measurement: run soak across the matrix and commit the raw numbers

THE TASK. Measure only: no library, example or build change, and no ranking or opportunities (rocketnet-profile-pass-1-report does that from your files). Build RelWithDebInfo (symbols kept), run examples/soak at 1, 100 and 1000 clients, each for the same seconds and seed, on every transport the machine supports, profile each 1000-client run, and commit the numbers and artifacts under docs/profile-1/ as DONE-WHEN (3) and (4) lay out. A row not run is a welcome line; a number not measured is never estimated.
MACHINES (the profiling fleet plan: clean numbers come from a dedicated, un-busy bench, never a loaded shared one). The Linux rows run on the member JOB.md names, which the coordinator stages on an otherwise idle bench; record its load at the start of every run, and if its load average is above 0.5 per core or another heavy process is running, wait up to ten minutes for it to settle, then run anyway and mark the row noisy with the load quoted. Linux transports: portable and gso (and loopback as the library-only baseline). xdp runs only on the XDP machine (space) and only if JOB.md names it as staged and free for this card; never flip XDP on a shared bench; otherwise the xdp rows say not-run, machine not staged. macOS rows (msgx and portable) run on mini-m5-1 only if JOB.md names it as staged for this card, with xctrace Time Profiler; otherwise not-run with the reason.
METHOD. For each row: `uptime` and the top CPU processes; the soak run with `--out`; for the 1000-client rows, the soak again under `perf record -g --call-graph dwarf` (Linux, `--pause` so perf attaches to the server's process if the run splits processes) or `xctrace record --template 'Time Profiler'` (macOS); the flame graph and folded stacks; the top 10 from `perf report --no-children` and `--children`. Syscalls per packet: syscall count over the run divided by packets sent plus received, from soak's own packet totals.

STEP 1. cd into the staged checkout JOB.md names; git log --oneline -1; confirm examples/soak.c is at BASE (else stop HOLD: soak lands first); `uname -a`, `nproc`, `lscpu | head -20`, `uptime`.
STEP 2. Build RelWithDebInfo: `(mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo .. >/dev/null && nice make -j4 --no-print-directory)`.
STEP 3. Run the matrix rows one at a time, never two at once, never in the background; save results, folded stacks, top-10 text and SVGs under docs/profile-1/.
STEP 4. Write docs/profile-1/runs.md and docs/profile-1/README.md.
STEP 5. Commit on your own branch as `rocketnet-profile-pass-1-measure: <what>` with the trailer; end as JOB.md says with Verdict on line 1 and Head: <sha> on line 2.
STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel program or the wall clock is a functional test, never ./bin/test.
- This card changes no code at all.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.

BUILD BOUND (the bench sandbox caps a job at 256 processes and 8 GiB, and the Makefile's build target runs an unbounded make -j). Wherever this card says `make` for the build, run instead `(mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. >/dev/null && nice make -j4 --no-print-directory)`, the same build bounded to four jobs. Run builds and suites one at a time, never in the background. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any CI-equivalent gate that reads git status.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and run only on the members JOB.md names, never the coordinator's machine. Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-profile-pass-1-report" :feature "Profile pass 1, report: where rocketnet's time goes, and what to cut first" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-api"
     :brief "RESULT: rocketnet-profile-pass-1-report sha=000000000000 tier: heavy
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: report
PRIORITY: critical
STREAM: rocketnet-api
DEPENDS-ON: rocketnet-profile-pass-1-measure
SOURCE: Glenn, 2026-10-18, through the coordinator: \"we should profile our library and look for optimization opportunities\", and of the soak example: it \"will also be our first approach to real profiling!!!\". The milestone promises no load or line-rate numbers; this pass learns where the time goes.
SPLIT: the judgment half of rocketnet-profile-pass-1 (Glenn 10-18 split request); it replaces that card. rocketnet-profile-pass-1-measure (pro) ran the soak matrix and committed docs/profile-1/; this card reads those files and writes the ranked report. It runs nothing.
START: docs/profile-1/README.md (read), docs/profile-1/runs.md (read), examples/soak.c (read), sdk/rocketnet.h (read), DRIVERS.md (read)
NEW: docs/PROFILE-1.md
TEST: none because this card is a report with no code change; DONE-WHEN names every artifact
DONE-WHEN: (1) red at BASE: docs/PROFILE-1.md does not exist; (2) at head `git diff --stat BASE..HEAD` touches only docs/PROFILE-1.md (no library, example, build or docs/profile-1/ change); (3) docs/PROFILE-1.md holds: the run table summarised from docs/profile-1/runs.md (every row, noisy and not-run rows included, each citing its file); per transport the server tick p50/p99/max and the client per-frame cost p50/p99/max against the 100 µs budget, CPU per client, bytes per packet, syscalls per packet and allocations per tick, each number copied from runs.md, never re-estimated; the top 10 hotspots by inclusive and self % of time per transport at 1000 clients, from the committed top-10 files and folded stacks; and a ranked list of optimization opportunities, each with the measured cost it would remove (the hotspot % and µs per tick), the evidence (flame graph file and symbol, and the sdk file:line of the symbol at BASE), a size estimate (S under a day, M a few days, L more), and a one-line idea of the change; (4) a number docs/profile-1/ does not hold is named as not measured, never estimated; a row that FAILed integrity in runs.md is reported first
TOOLCHAIN: Python 3 (reading the committed files), git
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report with Verdict: HOLD (the coordinator reads the report and cards the opportunities), with not-done named
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 300 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the committed measurements are the input.

# Profile pass 1, report: where rocketnet's time goes, and what to cut first

THE TASK. Read the measurements rocketnet-profile-pass-1-measure committed under docs/profile-1/ and write docs/PROFILE-1.md as DONE-WHEN (3) lays out. Change no code and run no soak: the input is the committed files. The opportunities list is the product: ranked by measured cost removed, honest about size, one line each, each hotspot tied to the sdk code that spends it (read the symbol's source at BASE). The coordinator turns each opportunity into its own pro card with a before/after soak measurement; this card writes no cards.

STEP 1. cd into the staged checkout JOB.md names; git log --oneline -1; confirm docs/profile-1/runs.md is at BASE (else stop HOLD: the measurement lands first).
STEP 2. Read docs/profile-1/README.md, runs.md, each top-10 file and the folded stacks; open each hot symbol's source in sdk/.
STEP 3. Write docs/PROFILE-1.md.
STEP 4. Commit on your own branch as `rocketnet-profile-pass-1-report: <what>` with the trailer; end as JOB.md says with Verdict: HOLD on line 1 and Head: <sha> on line 2, then the opportunities list.
STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel program or the wall clock is a functional test, never ./bin/test.
- This card changes no code at all and runs no soak.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.

BUILD BOUND (the bench sandbox caps a job at 256 processes and 8 GiB, and the Makefile's build target runs an unbounded make -j). Wherever this card says `make` for the build, run instead `(mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. >/dev/null && nice make -j4 --no-print-directory)`, the same build bounded to four jobs. Run builds and suites one at a time, never in the background. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any CI-equivalent gate that reads git status.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and run only on the members JOB.md names, never the coordinator's machine. Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-dst-ci-wiring" :feature "DST layer 4: per-push shards, the nightly campaign, and the planted-bug proof" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-tests"
     :brief "RESULT: rocketnet-dst-ci-wiring sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: ci
PRIORITY: high
STREAM: rocketnet-tests
DEPENDS-ON: rocketnet-dst-fault-injectors,rocketnet-dst-invariant-checks
SOURCE: docs/DST.md (read) sections 5 and 6: thousands of seeds per CI shard inside the one-minute budget, a nightly campaign of at least a million seeds, and the proof that a planted ordering bug and a planted ack bug are each found within N seeds.
PATHS: .github/workflows/ci.yml
NEW: .github/workflows/nightly-dst.yml,scripts/dst-selftest,scripts/dst-sweep,docs/DST.md
START: .github/workflows/nightly-channel-seeds.yml (read), scripts/channel-seed-sweep (read), scripts/soak-sim-selftest (read), .github/workflows/ci.yml (read)
TEST: none because the work is CI wiring and shell selftests run as commands; DONE-WHEN runs each, including the runs that must fail
DONE-WHEN: (1) red at BASE: `test -e scripts/dst-selftest || echo missing` prints `missing`; (2) scripts/dst-selftest, in soak-sim-selftest's style (one line per leg, nonzero on any misbehaving leg): leg determinism (one seed run twice, the same trace); leg clean (the per-push seed range PASS on the healthy build); leg plant-ordering and leg plant-ack, each copying the one SDK file docs/DST.md section 5 names to build/, applying its exact-once replacement (the leg fails loudly if the anchor does not match exactly once), building a planted DST binary against the copy, and requiring a FAIL within the design's N seeds, then replaying the printed seed and requiring the same FAIL line; no product file and no #ifdef in sdk/ carries a plant; (3) ci.yml gains a dst job (the fleet runner label the other Linux jobs use) with the design's shard matrix, each shard building only the DST target and running its seed range, each shard's measured wall time in the pull request and at most 60 s, plus the selftest; (4) .github/workflows/nightly-dst.yml runs the nightly campaign (schedule and workflow_dispatch, refs as nightly-channel-seeds.yml takes them), prints one `FAIL seed=<n> ...` line per failure with its replay command, uploads the log on failure; (5) docs/DST.md section 6 records the measured shard times and the seeds each plant needed; (6) both workflows parse as YAML; `bash scripts/dst-selftest`, `make && ./bin/test` green
TOOLCHAIN: cmake, gcc or clang, bash, Python 3 (for the YAML parse) on the Linux member JOB.md names
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 150 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 150 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none new; bash, the tree's build, and GitHub Actions as the other workflows use it.

# DST layer 4: per-push shards, the nightly campaign, and the planted-bug proof

THE TASK. Wire the finished DST into CI as docs/DST.md (read) sections 5 and 6 say, and prove it finds bugs. The proof is the selftest's two plant legs: each breaks one rule in a copy of an SDK file (the exact-once replacement idea is the schema repository's sabotage tool: a replacement that does not match exactly once fails the leg instead of silently testing an unbroken copy), and each must go red inside N seeds and replay to the same line. The per-push shards stay inside the owner's one-minute rule; anything longer is nightly. The report states the hosted minutes, if any, and that the nightly takes effect once it reaches the default branch. Where docs/DST.md names other files, list them as PATHS-PROPOSED.

STEP 1. Confirm `uname -s` is Linux; cd into the staged checkout JOB.md names; git log --oneline -1; build (the bounded make) and record DONE-WHEN (1).
STEP 2. Write scripts/dst-selftest and scripts/dst-sweep; run the plant legs red first against the healthy build to show they need the plant.
STEP 3. Write the ci.yml job and nightly-dst.yml; time each shard on the member.
STEP 4. Run the gate: every DONE-WHEN command. Read the last lines.
STEP 5. Commit on your own branch as `rocketnet-dst-ci-wiring: <what>` with the trailer; end as JOB.md says.
THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with a message layer above it. `make` builds into bin/; `./bin/test` is the unit suite (no sockets, no real time; `./bin/test <name>` runs one test and prints `unknown unit test <name>` with exit 2 for a name it does not hold), `./bin/functional` the loopback tier. Root test_unit_<topic>.cpp files register with REGISTER_UNIT_TEST (test_unit_registry.h (read)) and build with no CMakeLists.txt edit (CMakeLists.txt globs test_unit_*.cpp). sdk/rocketnet.h (read) is the public C surface; every new export also goes in tools/exports.txt, which tools/check_exports.py (read) compares on every platform. docs/CHANNELS.md (read) is the message-layer design; STANDARD.md (read) the wire and connect standard; docs/TLA-INVENTORY.md (read) the state-machine inventory. tla/ holds the models (an MC wrapper, a cfg per case, a README per module with its reversed witnesses); tla/CASES.tsv (read) names each case once, tla/RUNS.tsv (read) holds the measured records written by `tlacheck run` and joined by `tlacheck merge --keep tla/RUNS.tsv`, never edited by hand (tla/README.md (read), \"Refreshing records after a model edit\"), and the lander refuses a change whose cases have no current record. tla/COVERAGE.tsv (read) records which code each model covers. CMakeLists.txt keeps -ffp-contract=off on every target.


STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel program or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.

BUILD BOUND (the bench sandbox caps a job at 256 processes and 8 GiB, and the Makefile's build target runs an unbounded make -j). Wherever this card says `make` for the build, run instead `(mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. >/dev/null && nice make -j4 --no-print-directory)`, the same build bounded to four jobs. Run builds and suites one at a time, never in the background. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any CI-equivalent gate that reads git status.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-frama-clang-installed" :feature "Sentinel: the proofs and Frama-Clang prerequisites are in place before the Frama-Clang trial" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-tests. A sentinel with no brief: it held rocketnet-frama-clang-trial, rocketnet-proofs-check-sdk-asserts and rocketnet-sim-and-reset-tests-bite behind the CBMC proof cards, rocketnet-happy-path, rocketnet-frama-clang-install (mas-bandwidth/rowan-tools) and the DST cards")
    (:id "rocketnet-frama-clang-trial" :feature "Frama-Clang trial: can WP prove rocketnet_ack_block_read from the C++ source?" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-tests"
     :brief "RESULT: rocketnet-frama-clang-trial sha=000000000000 tier: heavy
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: research
PRIORITY: normal
STREAM: rocketnet-tests
DEPENDS-ON: rocketnet-frama-clang-installed
SOURCE: Glenn, 2026-10-18: \"Let's apply it [Frama-C] where it makes sense.\" The coordinator, the same day: rocketnet's SDK is C++, so plain Frama-C does not parse it; Frama-Clang (the Frama-C front end for C++, opam package frama-clang 0.0.20 for Frama-C 33, built on libclang 22) is the only route to WP on it. This card is the trial: one reader, and the output is whether Frama-Clang is usable here. Its CBMC twin is rocketnet-cbmc-ack-block-read. Read at BASE 1fcaea324.
PATHS: none
START: sdk/rocketnet_ack.cpp (read),sdk/rocketnet_ack.h (read),sdk/rocketnet_read_write.h (read),proofs/cbmc/Makefile (read)
NEW: proofs/framaclang/README.md,proofs/framaclang/ack_block_acsl.cpp,proofs/framaclang/Makefile,proofs/framaclang/results.md
TEST: none because the work is a Frama-Clang trial, not a Go package or a unit-suite case; DONE-WHEN names the command and the verdict
TOOLCHAIN: a Linux member with frama-c 33.x, the frama-clang plugin (`frama-c -plugins` lists it; framaCIRGen on PATH), alt-ergo and z3, after export PATH=\"$HOME/sdk/bin:$PATH\"; a missing frama-clang is a HOLD at once naming the bench
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 180 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 180 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: Frama-Clang (frama-clang, the Frama-C C++ front end) with WP and -wp-rte; CBMC already proves this reader's bounds (rocketnet-cbmc-ack-block-read), so the question here is only whether the Frama-C route works on this code base at all.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with a message layer above it. `make` builds into bin/; `./bin/test` is the unit suite, `./bin/functional` the loopback tier. proofs/cbmc/ holds the CBMC proofs: proofs/cbmc/Makefile (read) runs `make -C proofs/cbmc` (the `all` target, which .github/workflows/cbmc.yml runs on pull requests touching sdk/: that target is per-commit CI and this card never adds to it) with CBMC_FLAGS = --bounds-check --pointer-check --signed-overflow-check --unsigned-overflow-check --div-by-zero-check --unwinding-assertions. A proof is a C file that #includes the sdk .cpp it proves (proof_message_read.c includes rocketnet_message.cpp: the worked example, with its nondet_ and __CPROVER_ fallbacks for a plain compiler) and a harness that drives it on symbolic input. Never mirror sdk code into a proof (proof_fragment_bounds.c copies the reassembly function rather than including it: that copy can drift from the code, which is why rocketnet-cbmc-fragment-reassembly proves the real one).

# Frama-Clang trial: can WP prove rocketnet_ack_block_read from the C++ source?

THE TASK. Take one reader, rocketnet_ack_block_read (sdk/rocketnet_ack.cpp (read)), and try to prove it with Frama-Clang and WP: no runtime error (-wp-rte) on any data of data_bytes bytes, with an ACSL contract stating only what the code documents (requires \\valid_read(data + (0 .. data_bytes-1)) and \\valid(block); assigns *block; ensures a 0 result below ROCKETNET_ACK_BLOCK_MIN_BYTES). The contract lives on a wrapper in NEW proofs/framaclang/ack_block_acsl.cpp that #includes the .cpp; no sdk edit.
Record, in proofs/framaclang/results.md, each step and where it stopped: (1) does framaCIRGen parse sdk/rocketnet_ack.cpp (read) and the headers it includes (paste the first error if not, and whether a -D or an include path fixes it); (2) does frama-c load the result and does -wp-rte generate goals for the wrapper; (3) how many goals WP proves with alt-ergo and z3 at -wp-timeout 60, and every unproved goal with its line and reason; (4) the wall time of each step.
THE VERDICT, the first line of results.md and of the report: `Frama-Clang: usable` (the reader parses, goals generate, and every RTE goal is proved or named as a prover limit), `Frama-Clang: usable with limits` (it runs but needs workarounds you name, e.g. templates, the C++ standard library, inline asm, a -D), or `Frama-Clang: not usable here` (with the blocking error). Then two to five lines on what it would take to apply it to the other readers the CBMC cards cover.
NEW proofs/framaclang/Makefile (target trial, running the steps above; not in any CI, not in the nightly) and NEW proofs/framaclang/README.md (the versions measured: frama-c -version, the frama-clang version from `frama-c -plugins`, clang's version, alt-ergo and z3; how to run the trial).

DONE-WHEN: `make -C proofs/framaclang trial` runs on the Linux member to its end or to the step where Frama-Clang stops (paste its last lines); proofs/framaclang/results.md starts with one of the three verdict lines and records each step; `git diff --stat` against BASE shows only the NEW files. Red at BASE: proofs/framaclang does not exist.

STEP 1. Confirm `uname -s` is Linux; cd into the staged checkout JOB.md names and run git log --oneline -1; export PATH=\"$HOME/sdk/bin:$PATH\"; run `frama-c -version`, `frama-c -plugins | grep -i clang` and `command -v framaCIRGen`; a missing plugin is a HOLD at once naming the bench.
STEP 2. Red first: show proofs/framaclang is absent at BASE; quote the line.
STEP 3. Walk THE TASK's four steps in order, timing each.
STEP 4. Gate: the DONE-WHEN lines, each with its last line pasted.
STEP 5. Commit on your own branch as `rocketnet-frama-clang-trial: <verdict>` with the trailer. The member makes the push and pull request from outside the wall; the pull request body states the verdict, each step's result and time, and what was not done.
STEP 6. End as JOB.md says; where it asks for RESULT.md write it in JOB.md's shape (head, branch, verdict, gate, output, report).

FRAMA-C PROOF RULES (as every Frama-C proof card): contracts state only what the code documents; no sdk edit; an unproved goal is a confirmed undefined behaviour (reproduced under -fsanitize=undefined,address and reported HOLD) or a named prover or front-end limit.

STANDING RULES.
Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0.
Make the smallest change, with no refactor.
Never touch the space game's baseline or delta code.
Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
Never read outside the job's working directory.
Schema-table bounds: a bound is declared only if every writer and reader path enforces it. Otherwise the field stays open (*string, []T).

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the Linux member JOB.md names, never the coordinator's machine.
Anything that uses real time, sockets or the network is a functional test and belongs in ./bin/functional, never ./bin/test.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.")
    (:id "rocketnet-proofs-check-sdk-asserts" :feature "ESBMC proofs: every sdk rocketnet_assert on the proved paths is a checked property" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-tests"
     :brief "RESULT: rocketnet-proofs-check-sdk-asserts sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: critical
STREAM: rocketnet-tests
DEPENDS-ON: rocketnet-cbmc-connect-token-read
SOURCE: Coordinator, 2026-10-18, proof gap: the ESBMC proofs message_read_max and connect_token build with -DNDEBUG, which turns rocketnet_assert into a no-op, so the sdk's own internal asserts are never proved on those paths. fragment_reassembly already proves them. LAND-AFTER rocketnet-cbmc-connect-token-read (open PR #410), which creates proofs/cbmc/connect_token/ and proofs/cbmc/esbmc_shim.h on the same base.
PATHS: proofs/cbmc/message_read_max/proof.mk,proofs/cbmc/message_read_max/results.md
CARRY-NOTE: the connect_token files and esbmc_shim.h on the NEW line are created by rocketnet-cbmc-connect-token-read (PR #410) and exist at BASE once it lands; this card edits them, it does not create them. Only planted_bug_asserts.sh is created here.
START: proofs/cbmc/message_read_max/proof.mk,proofs/cbmc/message_read_max/proof_message_read_max.c (read),proofs/cbmc/message_read_max/results.md,proofs/cbmc/connect_token/proof.mk (read),proofs/cbmc/connect_token/proof_connect_token.cpp (read),proofs/cbmc/connect_token/results.md (read),proofs/cbmc/esbmc_shim.h (read),proofs/cbmc/fragment_reassembly/proof.mk (read),proofs/cbmc/fragment_reassembly/esbmc_cprover.h (read)
NEW: proofs/cbmc/planted_bug_asserts.sh,proofs/cbmc/connect_token/proof.mk,proofs/cbmc/connect_token/results.md,proofs/cbmc/esbmc_shim.h
TEST: none because the proof is ESBMC runs, not a unit case; DONE-WHEN names the make targets and planted_bug_asserts.sh, and any sdk fix carries its own unit test
TOOLCHAIN: a Linux member with esbmc 8.5 (run `command -v esbmc` and `esbmc --version` first), make, a C++17 compiler; a bench without esbmc is a HOLD at once naming the bench
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 150 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 150 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: ESBMC 8.5 and the proof.mk layout already in proofs/cbmc/; the fragment_reassembly proof already checks sdk asserts, so its mechanism is reused; nothing new in the build.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library. `make` builds into bin/; `./bin/test` is the unit suite. proofs/cbmc/<name>/proof.mk holds each ESBMC proof (a harness .cpp that #includes the real sdk .cpp, run per harness by a make target appended to NIGHTLY).

# ESBMC proofs: every sdk rocketnet_assert on the proved paths is a checked property

THE TASK. proofs/cbmc/message_read_max/proof.mk and proofs/cbmc/connect_token/proof.mk pass -DNDEBUG to ESBMC, so rocketnet_assert (find its definition under sdk/ with grep) compiles to nothing and the sdk's internal asserts on those paths are never proved. fragment_reassembly does not pass -DNDEBUG and proves them. Do, in this order:
1. Red first: show with grep that both proof.mk files carry -DNDEBUG, and plant one false assert in a copy to show nothing reports it (record the line you saw).
2. Remove -DNDEBUG from both proofs, or, if the sdk's assert expands to abort/assert.h machinery ESBMC cannot model, map rocketnet_assert to __ESBMC_assert under the proof build (in proofs/cbmc/esbmc_shim.h, guarded by ROCKETNET_ESBMC, or the fragment_reassembly shim's way). Either way every rocketnet_assert on a proved path becomes a checked property. Follow what fragment_reassembly does so the three proofs agree.
3. Run both proofs (make -C proofs/cbmc proofs-message_read_max proofs-connect_token). If an assert fails on a proved path, decide whether it is a genuine sdk bug or a harness limit. A genuine sdk bug is fixed in the sdk as the smallest change, with a unit test in the existing unit suite that fails before the fix and passes after, and the proof re-run green. A harness limit (an input the sdk's callers never pass) is fixed with an __ESBMC_assume stating the caller's real precondition, named in results.md. Never weaken an assert to make it pass.
4. Planted-bug check: add proofs/cbmc/planted_bug_asserts.sh. For each of the two proofs, on a temporary copy of the sdk file (never committed in modified form), negate or break the condition of one internal rocketnet_assert that sits on a path the harness reaches, run that harness, and require ESBMC to report VERIFICATION FAILED naming that assert; then require the unmodified tree to report VERIFICATION SUCCESSFUL. The script exits 0 only if every planted bug is caught and prints one line per proof.
5. Keep each harness ONE translation unit: ESBMC 8.5 is unsound across several translation units because it merges header statics by name. Do not split a harness over several .cpp files and do not add a second .cpp to any ESBMC command line.
6. Update both results.md files: the asserts now checked, the count passed, and any harness assumption added. Do not change bounds or unwind depths, and do not touch the fragment_reassembly proof except to share the shim if that is the smallest change.

DONE-WHEN: grep -n NDEBUG proofs/cbmc/message_read_max/proof.mk proofs/cbmc/connect_token/proof.mk prints nothing (or the rocketnet_assert mapping is shown in force in the proof build, with a planted false assert reported FAILED); both make targets end with every harness VERIFICATION SUCCESSFUL, each last line pasted; proofs/cbmc/planted_bug_asserts.sh exits 0 with its two lines pasted; ./bin/test is green when an sdk fix was made, with the new test named; git diff --stat against BASE shows only the proof.mk files, results.md files, the shim if touched, the script, and any sdk fix with its test.

STEP 1. Confirm uname -s is Linux; command -v esbmc and esbmc --version. If proofs/cbmc/connect_token/ is absent at BASE, finish HOLD at once: this card needs rocketnet-cbmc-connect-token-read to land first.
STEP 2-6. As THE TASK numbers them. Commit on your own branch as `rocketnet-proofs-check-sdk-asserts: <what>` with the trailer. Nothing reaches the forge from inside the wall: the member makes the push and pull request from outside. The pull request body states the diff stat, the red line, each harness verdict, the planted-bug lines, and what was not done.

STANDING RULES.
Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0.
Make the smallest change, with no refactor.
Never touch the space game's baseline or delta code.
Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
Never read outside the job's working directory.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the Linux member JOB.md names, never the coordinator's machine.
Anything that uses real time, sockets or the network is a functional test and belongs in ./bin/functional, never ./bin/test.")
    (:id "rocketnet-sim-and-reset-tests-bite" :feature "The simulator and reset tests catch the bugs they exist to catch" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-tests"
     :brief "RESULT: rocketnet-sim-and-reset-tests-bite sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: code
PRIORITY: critical
STREAM: rocketnet-tests
DEPENDS-ON: rocketnet-public-link-simulator-t,rocketnet-per-connection-reset-is-complete
SOURCE: the coordinator, 2026-10-19: two cold readers each of rocketnet-public-link-simulator-t and rocketnet-per-connection-reset-is-complete found tests that do not bite: planted bugs that stay green.
PATHS: test_unit_public_link_simulator.cpp,test_unit_connection_reset.cpp,CMakeLists.txt
START: test_unit_public_link_simulator.cpp (read), test_unit_connection_reset.cpp (read), sdk/rocketnet_network_simulator.h (read), sdk/rocketnet_connection_reset.cpp (read)
TEST: none because the cases are C++ checks registered with REGISTER_UNIT_TEST in the two test files; DONE-WHEN names the commands that run them
DONE-WHEN: (1) red at BASE, each shown with its exit code: with the client transport's send path made to skip the simulator, ./bin/test public_link_simulator_jitter and public_link_simulator_same_seed_same_schedule both still pass at BASE (record this; it is the defect); with the server transport's send path made to skip the simulator, the full ./bin/test and ./bin/functional still pass at BASE (record this); (2) at head the jitter and same-seed cases drive datagrams through a loopback transport created with the simulator fields set (not a bare rocketnet_network_simulator), and each goes red when the client send path skips the simulator, shown once and restored; (3) at head a new case public_link_simulator_server_side applies loss and latency on the server transport config over loopback and goes red when the server send path skips the simulator, shown once and restored; (4) test_unit_connection_reset.cpp adds no -Winvalid-offsetof warning (compare the build's warning count at BASE and head for that file); (5) `make && ./bin/test` and `./bin/functional` green; time is an argument starting at 1000.0, loopback only, no socket
TOOLCHAIN: cmake, gcc or clang on the Linux member JOB.md names
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 120 minutes.

# The simulator and reset tests catch the bugs they exist to catch

THE TASK. Cold readers planted bugs that these tests should catch and they stayed green: the jitter and same-seed cases build a bare network simulator, so a transport that bypasses the simulator still passes; nothing tests the server-side simulator at all; and the reset test added six -Winvalid-offsetof warnings. Make each test bite: drive the cases through the transport and add the server-side case. Fix the six warnings (use the existing layout-field tables rather than offsetof on non-standard-layout types). Test files only. (Naming every struct and member in the reset walk, and the wire-totals reset, are owed by rocketnet-per-connection-reset-is-complete itself.)

STEP 1. Confirm `uname -s` is Linux; cd into the staged checkout JOB.md names; git log --oneline -1; build (the bounded make) and record DONE-WHEN (1) with the two bypass mutations in a scratch worktree.
STEP 2. Rewrite the jitter and same-seed cases over a loopback transport; add the server-side case.
STEP 3. Remove the offsetof warnings.
STEP 4. Run the gate: every DONE-WHEN command, each mutation shown red once then restored. Read the last lines.
STEP 5. Commit on your own branch as `rocketnet-sim-and-reset-tests-bite: <what>` with the trailer; end as JOB.md says.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected server drivers (DRIVERS.md (read)). `make` builds into bin/; `./bin/test` is the unit suite (`./bin/test <name>` runs one case and prints `unknown unit test <name>`, exit 2, for a name it does not hold); `./bin/functional` is the loopback and simulated tier; `./bin/test_xdp_filter` is the kernel-tier XDP suite, run inside a virtme-ng VM. Root test_unit_<topic>.cpp files register with REGISTER_UNIT_TEST (test_unit_registry.h (read)). CMakeLists.txt keeps -ffp-contract=off on every target.


STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel program or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.

BUILD BOUND (the bench sandbox caps a job at 256 processes and 8 GiB, and the Makefile's build target runs an unbounded make -j). Wherever this card says `make` for the build, run instead `(mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. >/dev/null && nice make -j4 --no-print-directory)`, the same build bounded to four jobs. Run builds and suites one at a time, never in the background. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any CI-equivalent gate that reads git status.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.

CARRY: head=3d762ec2cec5870b085ac602346af7029f9be550")
    (:id "rocketnet-lan-1000-clients-linux-gso" :feature "LAN 1000 clients: the Linux gso server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-1000-clients-linux-gso sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner
SOURCE: the owner, 2026-10-18: 1000 concurrent clients against one server, one card per server transport, on the LAN only, today. This card is the Linux `gso` server driver (linux-gso). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory, never the test server; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/1000-clients-linux-gso.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/1000-clients-linux-gso.md` fails at BASE and at head that file, titled `## 1000 clients: linux-gso`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the bench x direction x mode x channel x size-bucket matrix, per-bench connect success and times, cycles by kind, reconnect successes and times, failed connects, denials and unexpected disconnects, counters and resident memory each minute, the before/after restore check of the server host, and one verdict line: PASS needs every message verified, nothing missing or reordered on reliable modes, zero failed connects, zero denials and zero unexpected disconnects on every bench for the whole run, reconnects at 2 x the configured timeout, no crash and memory flat; anything else is FAIL with each failure named (bench label, reason, denial code or timeout side, counters, log excerpt)
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN 1000 clients: the Linux gso server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase clients1000 --transport linux-gso` once, end to end, as `## Running it` says, then record it in docs/lan-bench/1000-clients-linux-gso.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on that LAN Linux bench (never the test server, whose traffic crosses the studio's 20 to 50 Mbit/s uplink), its driver pinned to `gso` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/1000-clients-linux-gso.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/1000-clients-linux-gso.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-1000-clients-linux-gso: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-1000-clients-linux-io-uring" :feature "LAN 1000 clients: the Linux io_uring server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-1000-clients-linux-io-uring sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner
SOURCE: the owner, 2026-10-18: 1000 concurrent clients against one server, one card per server transport, on the LAN only, today. This card is the Linux `io_uring` server driver (linux-io-uring). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory, never the test server; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/1000-clients-linux-io-uring.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/1000-clients-linux-io-uring.md` fails at BASE and at head that file, titled `## 1000 clients: linux-io-uring`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the bench x direction x mode x channel x size-bucket matrix, per-bench connect success and times, cycles by kind, reconnect successes and times, failed connects, denials and unexpected disconnects, counters and resident memory each minute, the before/after restore check of the server host, and one verdict line: PASS needs every message verified, nothing missing or reordered on reliable modes, zero failed connects, zero denials and zero unexpected disconnects on every bench for the whole run, reconnects at 2 x the configured timeout, no crash and memory flat; anything else is FAIL with each failure named (bench label, reason, denial code or timeout side, counters, log excerpt)
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN 1000 clients: the Linux io_uring server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase clients1000 --transport linux-io-uring` once, end to end, as `## Running it` says, then record it in docs/lan-bench/1000-clients-linux-io-uring.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on that LAN Linux bench (never the test server, whose traffic crosses the studio's 20 to 50 Mbit/s uplink), its driver pinned to `io_uring` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/1000-clients-linux-io-uring.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/1000-clients-linux-io-uring.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-1000-clients-linux-io-uring: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-1000-clients-linux-portable" :feature "LAN 1000 clients: the Linux portable server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-1000-clients-linux-portable sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner
SOURCE: the owner, 2026-10-18: 1000 concurrent clients against one server, one card per server transport, on the LAN only, today. This card is the Linux `portable` server driver (linux-portable). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory, never the test server; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/1000-clients-linux-portable.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/1000-clients-linux-portable.md` fails at BASE and at head that file, titled `## 1000 clients: linux-portable`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the bench x direction x mode x channel x size-bucket matrix, per-bench connect success and times, cycles by kind, reconnect successes and times, failed connects, denials and unexpected disconnects, counters and resident memory each minute, the before/after restore check of the server host, and one verdict line: PASS needs every message verified, nothing missing or reordered on reliable modes, zero failed connects, zero denials and zero unexpected disconnects on every bench for the whole run, reconnects at 2 x the configured timeout, no crash and memory flat; anything else is FAIL with each failure named (bench label, reason, denial code or timeout side, counters, log excerpt)
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN 1000 clients: the Linux portable server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase clients1000 --transport linux-portable` once, end to end, as `## Running it` says, then record it in docs/lan-bench/1000-clients-linux-portable.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on that LAN Linux bench (never the test server, whose traffic crosses the studio's 20 to 50 Mbit/s uplink), its driver pinned to `portable` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/1000-clients-linux-portable.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/1000-clients-linux-portable.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-1000-clients-linux-portable: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-1000-clients-linux-xdp" :feature "LAN 1000 clients: the Linux xdp server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-1000-clients-linux-xdp sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner,rocketnet-xdp-on-link-reply-uses-peer-mac
SOURCE: the owner, 2026-10-18: 1000 concurrent clients against one server, one card per server transport, on the LAN only, today. This card is the Linux `xdp` server driver (linux-xdp). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here one LAN Linux bench with a 10G NIC chosen from docs/LAN-BENCH.md's inventory, held for XDP for the run and bound on that NIC under XDP SAFETY, never the test server; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/1000-clients-linux-xdp.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/1000-clients-linux-xdp.md` fails at BASE and at head that file, titled `## 1000 clients: linux-xdp`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the bench x direction x mode x channel x size-bucket matrix, per-bench connect success and times, cycles by kind, reconnect successes and times, failed connects, denials and unexpected disconnects, counters and resident memory each minute, the before/after restore check of the server host, and one verdict line: PASS needs every message verified, nothing missing or reordered on reliable modes, zero failed connects, zero denials and zero unexpected disconnects on every bench for the whole run, reconnects at 2 x the configured timeout, no crash and memory flat; anything else is FAIL with each failure named (bench label, reason, denial code or timeout side, counters, log excerpt)
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: xdp
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host. Stop the run, FAIL, if any XDP SAFETY check fails. Stop with not-done `XDP bench not held` if the inventory's XDP server row is not marked `xdp-held`.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN 1000 clients: the Linux xdp server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase clients1000 --transport linux-xdp` once, end to end, as `## Running it` says, then record it in docs/lan-bench/1000-clients-linux-xdp.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on that held LAN XDP bench, bound on its 10G NIC under XDP SAFETY (never the test server), its driver pinned to `xdp` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

XDP SAFETY (OWNER RULING, 2026-10-18, applied to the LAN bench held for XDP; \"AF_XDP steers only our packets; we never take over the NIC\"). 1. rocketnet's XDP program redirects only packets to the server's UDP port and returns XDP_PASS for everything else (server_xdp/server_xdp.c:126-128 at 8ddc93ec1771: `udp->dest != config->public_port || !address_match` returns XDP_PASS), so ssh and all other traffic pass; server_xdp/server_xdp.c is never edited. 2. Before the program loads, a dead-man's switch is armed on the XDP bench: a systemd-run timer (15 minutes) that runs `ip link set dev <nic> xdp off` and kills the server unless the orchestrator cancels it after proving ssh still works; re-armed for each phase. 3. Within 10 s of the program loading the orchestrator proves ssh to the XDP bench works; if not, it waits for the switch to fire and stops the run, FAIL. 4. The 10G NIC's driver and version (`ethtool -i <nic>`) are read first, read-only, and recorded; the driver is used as it is, never rebuilt or reloaded by the run. 5. After the run XDP is unloaded and `ip link show dev <nic>` shows no program attached; that line is recorded. 6. Root on the XDP bench is only for loading and unloading XDP, the dead-man timer, and starting and stopping the server, by non-interactive `sudo -n` on the landed script files; if `sudo -n true` fails or asks for a password, stop with not-done and report it; never type a password. The XDP SAFETY lines go into the results file.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/1000-clients-linux-xdp.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/1000-clients-linux-xdp.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-1000-clients-linux-xdp: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-1000-clients-macos-msgx" :feature "LAN 1000 clients: the macOS msgx server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-1000-clients-macos-msgx sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner
SOURCE: the owner, 2026-10-18: 1000 concurrent clients against one server, one card per server transport, on the LAN only, today. This card is the macOS `msgx` server driver (macos-msgx). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here the macOS bench; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/1000-clients-macos-msgx.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/1000-clients-macos-msgx.md` fails at BASE and at head that file, titled `## 1000 clients: macos-msgx`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the bench x direction x mode x channel x size-bucket matrix, per-bench connect success and times, cycles by kind, reconnect successes and times, failed connects, denials and unexpected disconnects, counters and resident memory each minute, the before/after restore check of the server host, and one verdict line: PASS needs every message verified, nothing missing or reordered on reliable modes, zero failed connects, zero denials and zero unexpected disconnects on every bench for the whole run, reconnects at 2 x the configured timeout, no crash and memory flat; anything else is FAIL with each failure named (bench label, reason, denial code or timeout side, counters, log excerpt)
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: darwin
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN 1000 clients: the macOS msgx server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase clients1000 --transport macos-msgx` once, end to end, as `## Running it` says, then record it in docs/lan-bench/1000-clients-macos-msgx.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on the macOS bench, its driver pinned to `msgx` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/1000-clients-macos-msgx.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/1000-clients-macos-msgx.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-1000-clients-macos-msgx: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-1000-clients-macos-portable" :feature "LAN 1000 clients: the macOS portable server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-1000-clients-macos-portable sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner
SOURCE: the owner, 2026-10-18: 1000 concurrent clients against one server, one card per server transport, on the LAN only, today. This card is the macOS `portable` server driver (macos-portable). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here the macOS bench; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/1000-clients-macos-portable.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/1000-clients-macos-portable.md` fails at BASE and at head that file, titled `## 1000 clients: macos-portable`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the bench x direction x mode x channel x size-bucket matrix, per-bench connect success and times, cycles by kind, reconnect successes and times, failed connects, denials and unexpected disconnects, counters and resident memory each minute, the before/after restore check of the server host, and one verdict line: PASS needs every message verified, nothing missing or reordered on reliable modes, zero failed connects, zero denials and zero unexpected disconnects on every bench for the whole run, reconnects at 2 x the configured timeout, no crash and memory flat; anything else is FAIL with each failure named (bench label, reason, denial code or timeout side, counters, log excerpt)
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: darwin
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN 1000 clients: the macOS portable server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase clients1000 --transport macos-portable` once, end to end, as `## Running it` says, then record it in docs/lan-bench/1000-clients-macos-portable.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on the macOS bench, its driver pinned to `portable` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/1000-clients-macos-portable.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/1000-clients-macos-portable.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-1000-clients-macos-portable: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-1000-clients-windows-iocp" :feature "LAN 1000 clients: the Windows iocp server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-1000-clients-windows-iocp sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner
SOURCE: the owner, 2026-10-18: 1000 concurrent clients against one server, one card per server transport, on the LAN only, today. This card is the Windows `iocp` server driver (windows-iocp). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here the native Windows side of the Linux-on-Windows bench; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/1000-clients-windows-iocp.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/1000-clients-windows-iocp.md` fails at BASE and at head that file, titled `## 1000 clients: windows-iocp`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the bench x direction x mode x channel x size-bucket matrix, per-bench connect success and times, cycles by kind, reconnect successes and times, failed connects, denials and unexpected disconnects, counters and resident memory each minute, the before/after restore check of the server host, and one verdict line: PASS needs every message verified, nothing missing or reordered on reliable modes, zero failed connects, zero denials and zero unexpected disconnects on every bench for the whole run, reconnects at 2 x the configured timeout, no crash and memory flat; anything else is FAIL with each failure named (bench label, reason, denial code or timeout side, counters, log excerpt)
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: windows
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN 1000 clients: the Windows iocp server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase clients1000 --transport windows-iocp` once, end to end, as `## Running it` says, then record it in docs/lan-bench/1000-clients-windows-iocp.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on the native Windows side of the Linux-on-Windows bench, its driver pinned to `iocp` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/1000-clients-windows-iocp.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/1000-clients-windows-iocp.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-1000-clients-windows-iocp: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-1000-clients-windows-portable" :feature "LAN 1000 clients: the Windows portable server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-1000-clients-windows-portable sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner
SOURCE: the owner, 2026-10-18: 1000 concurrent clients against one server, one card per server transport, on the LAN only, today. This card is the Windows `portable` server driver (windows-portable). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here the native Windows side of the Linux-on-Windows bench; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/1000-clients-windows-portable.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/1000-clients-windows-portable.md` fails at BASE and at head that file, titled `## 1000 clients: windows-portable`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the bench x direction x mode x channel x size-bucket matrix, per-bench connect success and times, cycles by kind, reconnect successes and times, failed connects, denials and unexpected disconnects, counters and resident memory each minute, the before/after restore check of the server host, and one verdict line: PASS needs every message verified, nothing missing or reordered on reliable modes, zero failed connects, zero denials and zero unexpected disconnects on every bench for the whole run, reconnects at 2 x the configured timeout, no crash and memory flat; anything else is FAIL with each failure named (bench label, reason, denial code or timeout side, counters, log excerpt)
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: windows
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN 1000 clients: the Windows portable server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase clients1000 --transport windows-portable` once, end to end, as `## Running it` says, then record it in docs/lan-bench/1000-clients-windows-portable.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on the native Windows side of the Linux-on-Windows bench, its driver pinned to `portable` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/1000-clients-windows-portable.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/1000-clients-windows-portable.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-1000-clients-windows-portable: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-1000-clients-windows-rio" :feature "LAN 1000 clients: the Windows rio server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-1000-clients-windows-rio sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner,rocketnet-rio-driver-core
SOURCE: the owner, 2026-10-18: 1000 concurrent clients against one server, one card per server transport, on the LAN only, today. This card is the Windows `rio` server driver (windows-rio). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here the native Windows side of the Linux-on-Windows bench; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it. The rio driver is added by rocketnet-rio-driver-core (landed by the need); if `rio` is not in sdk/rocketnet_drivers.inc at BASE, stop with not-done.
NEW: docs/lan-bench/1000-clients-windows-rio.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/1000-clients-windows-rio.md` fails at BASE and at head that file, titled `## 1000 clients: windows-rio`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the bench x direction x mode x channel x size-bucket matrix, per-bench connect success and times, cycles by kind, reconnect successes and times, failed connects, denials and unexpected disconnects, counters and resident memory each minute, the before/after restore check of the server host, and one verdict line: PASS needs every message verified, nothing missing or reordered on reliable modes, zero failed connects, zero denials and zero unexpected disconnects on every bench for the whole run, reconnects at 2 x the configured timeout, no crash and memory flat; anything else is FAIL with each failure named (bench label, reason, denial code or timeout side, counters, log excerpt)
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: windows
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN 1000 clients: the Windows rio server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase clients1000 --transport windows-rio` once, end to end, as `## Running it` says, then record it in docs/lan-bench/1000-clients-windows-rio.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on the native Windows side of the Linux-on-Windows bench, its driver pinned to `rio` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/1000-clients-windows-rio.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/1000-clients-windows-rio.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-1000-clients-windows-rio: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-bench-design" :feature "LAN transport throughput and 1000 clients, one server per transport" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-bench-design sha=000000000000 tier: heavy
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: design
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-live-xdp-fleet-run,rocketnet-lan-inventory
SOURCE: the owner, 2026-10-18, the phases after the internal-protocol stress run (rocketnet-live-xdp-fleet-run, whose docs/LIVE-XDP.md, scripts/live-xdp-fleet* and test/live_verify.h this builds on): phase 2, transport throughput toward 10 Gbit/s on the LAN, one card per server transport, reporting Gbit/s, pps, CPU per core, loss and latency; phase 3, 1000 concurrent clients (the max_clients ceiling, ROCKETNET_MAX_CLIENTS in sdk/rocketnet.h:44) against one server per transport, with the stress test's verification and hard-fail rules. Both on the LAN only, never the cloud benches: OWNER RULING (2026-10-18), flat out only on the LAN; any client that would cross the internet is capped at 100 Mbit/s and the phase spends at most 200 GB; the design names both numbers. OWNER RULING (2026-10-18): for these transport tests the server may run on the LAN machine that hosts that transport (one LAN Linux bench per Linux transport, chosen by NIC speed from section 1, and for xdp a LAN bench with a 10G NIC held for XDP for its run, never the test server; the native Windows side of the Linux-on-Windows bench for iocp and rio, the macOS bench for msgx and the macOS portable driver); clients stay on the other benches; each machine is restored afterwards.
PATHS: tla/CASES.tsv,tla/RUNS.tsv,tla/COVERAGE.tsv,tla/README.md
NEW: docs/LAN-BENCH.md,tla/RNLanBenchRun.tla,tla/MCRNLanBenchRun.tla,tla/MCRNLanBenchRun.cfg,tla/README-RNLanBenchRun.md,tla/MCRNLanBenchRunBrokenHostNotRestored.cfg,tla/MCRNLanBenchRunBrokenTwoRunsAtOnce.cfg
TEST: none because the output is a design document and a TLA+ model of the run's lifecycle
DONE-WHEN: `ls docs/LAN-BENCH.md` fails at BASE and at head the file holds every section THE DESIGN names, each decision citing the file:line or the command output it rests on, and a results index naming, per transport and phase, the file its run card writes (docs/lan-bench/throughput-<transport>.md and docs/lan-bench/1000-clients-<transport>.md, each marked `not yet run`); `tlacheck run` records MCRNLanBenchRun.cfg PASS and each reversed witness violating exactly its declared invariant (exit 12) on a Linux bench with the pinned jar, then `tlacheck merge --keep tla/RUNS.tsv`; tla/CASES.tsv names the three cases in a new group rnlanbenchrun; tla/COVERAGE.tsv has a row lan-bench-run with status owed; `tlacheck groups --root . --stale` prints []; `make && ./bin/test` stays green
TOOLCHAIN: TLC from the pinned JAR, java and tlacheck from the pinned nova-tools commit (tla/README.md), cmake and a C++17 compiler, on the Linux member JOB.md names
GATE-NEEDS: linux, tla
SHARED: tla/CASES.tsv,tla/RUNS.tsv,tla/COVERAGE.tsv,tla/README.md
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 180 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 180 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the stress run's programs, verify header and scripts, extended; ethtool and ip for the inventory.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers. The registry is sdk/rocketnet_drivers.inc (read): server drivers portable (every platform), gso, xdp and io_uring (Linux), msgx (macOS), iocp (Windows), and loopback (pin-only, in-process, not a network transport); rio is being added by rocketnet-rio-driver-core. DRIVERS.md (read) explains the buffered and staged shapes. docs/LIVE-XDP.md (read) is the stress design and its results; scripts/live-xdp-fleet* the orchestration; test/live_verify.h the verify scheme. `make` builds into bin/; `./bin/test` is the unit suite.

# LAN transport throughput and 1000 clients, one server per transport

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Design, do not build or load anything. Write docs/LAN-BENCH.md from the landed read-only LAN inventory docs/lan-bench/inventory.md (taken by rocketnet-lan-inventory; read, never edited, never re-taken), model the run lifecycle in TLA+. Cut from it: rocketnet-lan-bench-runner (the load modes and orchestration) and one run card per transport for each phase, rocketnet-lan-throughput-<transport> and rocketnet-lan-1000-clients-<transport>, for linux-portable, linux-gso, linux-io-uring, linux-xdp, windows-portable, windows-iocp, windows-rio, macos-portable and macos-msgx.

THE DESIGN (each a section of docs/LAN-BENCH.md):
1. The LAN inventory. Restate from docs/lan-bench/inventory.md, citing its rows: which machines have 10G NICs, which interface is each one's uplink, and the switch path between them (which hops are 10G, which 1G). Labels and shapes only in the document (host names stay in the coordinator's inventory file, outside the repository). Where the inventory marks a machine `unreachable` or a field missing, say so and say what the design assumes; take no inventory of your own.
2. Hosts per transport. Which machine serves each transport (per the OWNER RULING), which benches load it, and the expected ceiling from section 1 (a 1G hop caps the run; say so per transport). No LAN-phase server is on the test server (RULINGS (e)). The XDP server binds the held LAN XDP bench's 10G NIC under the XDP SAFETY rules below, which the document restates per phase.

XDP SAFETY (OWNER RULING, 2026-10-18, applied to the LAN bench held for XDP; \"AF_XDP steers only our packets; we never take over the NIC\"). 1. rocketnet's XDP program redirects only packets to the server's UDP port and returns XDP_PASS for everything else (server_xdp/server_xdp.c:126-128 at 8ddc93ec1771: `udp->dest != config->public_port || !address_match` returns XDP_PASS), so ssh and all other traffic pass; server_xdp/server_xdp.c is never edited. 2. Before the program loads, a dead-man's switch is armed on the XDP bench: a systemd-run timer (15 minutes) that runs `ip link set dev <nic> xdp off` and kills the server unless the orchestrator cancels it after proving ssh still works; re-armed for each phase. 3. Within 10 s of the program loading the orchestrator proves ssh to the XDP bench works; if not, it waits for the switch to fire and stops the run, FAIL. 4. The 10G NIC's driver and version (`ethtool -i <nic>`) are read first, read-only, and recorded; the driver is used as it is, never rebuilt or reloaded by the run. 5. After the run XDP is unloaded and `ip link show dev <nic>` shows no program attached; that line is recorded. 6. Root on the XDP bench is only for loading and unloading XDP, the dead-man timer, and starting and stopping the server, by non-interactive `sudo -n` on the landed script files; if `sudo -n true` fails or asks for a password, stop with not-done and report it; never type a password.

3. Phase 2, throughput. The load: flat-out sends from several LAN benches at once toward the server, the message mix (sizes and modes from test/live_verify.h, weighted to the large single-packet sizes for bytes and the small ones for pps), the duration, warm-up and steady window, the bench count and clients per bench. Metrics per transport: Gbit/s and pps each way at the server, CPU per core on the server (per core, not total), loss, RTT p50/p99, and the verify counts (every message still verified). Name the target (toward 10 Gbit/s where the path is 10G) and how the run records the ceiling it hit and why (NIC, CPU core, syscall rate, switch).
4. Phase 3, 1000 clients. 1000 concurrent clients across the LAN benches (for example about 125 per bench), as processes or as multiplexed clients per process if the client program supports it (say which, from client.c and the runner of the stress test). Same matrix, verification and hard-fail rules as docs/LIVE-XDP.md: every message verified; zero failed connects, denials and unexpected disconnects; the disconnect cycle with reconnect at 2 x the configured timeout; memory flat. OWNER RULING (2026-10-18): the run config sets max_clients 1000 explicitly; the default (64 at sdk/rocketnet.h:923) moves to 1000 in rocketnet-max-clients-defaults-to-1000, not here.
5. One run at a time. The LAN is one measurement: a run takes a lock file on every host it uses and refuses to start while another run holds one; the lock and the teardown are in the model.
6. Restore. Per server host, the state before and after (processes, XDP attachment, firewall rules, sysctl or registry changes, power plan, CPU governor); the run records both and fails if they differ.
7. Tokens, addresses, ports: as docs/LIVE-XDP.md sections 10 and 11, on LAN addresses.
8. Model. tla/RNLanBenchRun.tla: the lock per host, each server host's state (clean, serving, dirty), XDP attached, load benches running; invariants: at most one run holds the LAN, a server only on its transport's host and never on the test server, XDP only on a host held for XDP, XDP loaded only while the dead-man's switch is armed, no load before listen; liveness: every run ends with every host clean and the lock free. Witnesses BrokenHostNotRestored and BrokenTwoRunsAtOnce, each violating exactly its invariant.
9. File plan. For rocketnet-lan-bench-runner and each run card, the exact files and the gate command.
10. Findings for the owner: section 1's ceilings and the NIC line.

STEP 1. Confirm `uname -s` is Linux; cd into the staged checkout JOB.md names; git log --oneline -1. Run `ls docs/LAN-BENCH.md` and keep its line; if it exists, stop with not-done.
STEP 2. Read sdk/rocketnet_drivers.inc, DRIVERS.md, docs/LIVE-XDP.md, client.c, server.c, server_xdp/server_xdp.c, scripts/live-xdp-fleet*, test/live_verify.h and scripts/rocketnet-provision. Read docs/lan-bench/inventory.md; run nothing on a remote machine.
STEP 3. Write the document, the model and its configs; run the new group with `tlacheck run` on a Linux bench, then `tlacheck merge --keep tla/RUNS.tsv` as tla/README.md says; check every counterexample against the design by hand.
STEP 4. Run the gate: the DONE-WHEN commands. Read the last lines. When a test fails, name its file and say whether your work changed it (run it at BASE to say so), and report that line first.
STEP 5. Commit on your own branch as `rocketnet-lan-bench-design: <what>` with the trailer. The pull request body states the diff stat, the TLC lines, the findings, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services; a test clock starts at 1000.0.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

BUILD BOUND. For `make` run `(mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. >/dev/null && nice make -j4 --no-print-directory)`, one build or suite at a time, never in the background.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
This card designs: it runs nothing on a remote machine; it starts no server, client, load or XDP program anywhere and changes no setting.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both. Never merge into BASE.
Never search /, ~ or /Volumes.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-bench-runner-client" :feature "LAN bench runner, piece 1: the client load modes and the merge" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-bench-runner-client sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: feature
PRIORITY: critical
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-design
SPLIT: piece of rocketnet-lan-bench-runner (Glenn 10-18 split request); siblings rocketnet-lan-bench-runner-server,rocketnet-lan-bench-runner-scripts
SOURCE: the owner, 2026-10-18: phase 2 (transport throughput toward 10 Gbit/s on the LAN) and phase 3 (1000 concurrent clients) against one server per transport, on the LAN only. docs/LAN-BENCH.md (rocketnet-lan-bench-design, landed by the need) is the design; it extends the stress run's programs and scripts (docs/LIVE-XDP.md, scripts/live-xdp-fleet*, test/live_verify.h). This card builds the load modes and the LAN orchestration; the per-transport run cards run them.
PATHS: client.c,CMakeLists.txt
NEW: scripts/lan-bench-merge
TEST: none because the client load modes are exercised by the run cards; the shared logic is tested by test_unit_live_verify.cpp and the loopback matrix; the gate is the build, the existing suites, the client self-test result and the merge over it
DONE-WHEN: the client program builds on Linux at head and its new flags show in its usage; `./bin/test` and `./bin/functional` are green; `scripts/lan-bench-merge` on sample result files from the client's `--self-test-result` (one for the throughput phase, one for the 1000-client phase) prints the throughput row and the 1000-client matrix docs/LAN-BENCH.md names; the merge script passes its interpreter's syntax check (`bash -n`, or `python3 -m py_compile` for a python file)
TOOLCHAIN: cmake and a C++17 compiler, bash and python3, on the Linux member JOB.md names; libsodium is vendored under sodium/
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 90 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 90 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the stress run's programs, test/live_verify.h, the counters API, ssh, bash and python3's standard library.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)); a server driver is pinned with ROCKETNET_SERVER_DRIVER. `make` builds into bin/; `./bin/test` is the unit suite; `./bin/functional` the loopback tier. docs/LAN-BENCH.md (read) governs every choice below; where this card and the document differ, the document wins and the report names the difference.

# LAN bench runner, piece 1: the client load modes and the merge

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Build what docs/LAN-BENCH.md specifies for the client, in the files its File plan names for this role (client.c, and CMakeLists.txt only if the build needs it): the client's throughput load mode and its many-clients mode (processes or clients multiplexed per process, as the document decides) with the stress test's verification, hard-fail rules and disconnect cycle, writing a result file with the counts the document names; a `--self-test-result <file>` output for each phase written without any socket; and scripts/lan-bench-merge for the tables. This piece does not touch server.c, scripts/lan-bench, scripts/lan-bench-remote, the example inventory or docs/LAN-BENCH.md.

SPLIT NOTE. This card is one of three pieces of rocketnet-lan-bench-runner, run in parallel: rocketnet-lan-bench-runner-client (the client load modes and scripts/lan-bench-merge), rocketnet-lan-bench-runner-server (the server pinned-transport mode) and rocketnet-lan-bench-runner-scripts (scripts/lan-bench, scripts/lan-bench-remote, the example inventory and docs/LAN-BENCH.md `## Running it`). Where docs/LAN-BENCH.md's File plan names files for rocketnet-lan-bench-runner, this piece takes only its own files from that list; another piece's file named there is not a difference to stop on, but a file the File plan names for rocketnet-lan-bench-runner that no piece holds is. FLAG NAMES. The client and server flags are the ones docs/LAN-BENCH.md names; where it names none, the piece adding the flag picks it, prints it in its usage, and lists it in its report. The orchestration piece (rocketnet-lan-bench-runner-scripts) lands after both program pieces; it passes the names docs/LAN-BENCH.md gives and lists every flag it passes in its report, so a mismatch is caught at its landing.

STEP 1. Confirm `uname -s` is Linux; cd into the staged checkout JOB.md names; git log --oneline -1. Confirm docs/LAN-BENCH.md exists at BASE; if not, stop with not-done.
STEP 2. Read docs/LAN-BENCH.md whole, docs/LIVE-XDP.md, client.c, scripts/live-xdp-fleet-merge and test/live_verify.h.
STEP 3. Build it in the files PATHS and NEW name, and only those. Cite the docs/LAN-BENCH.md section from each function that implements a rule.
STEP 4. Run the gate: DONE-WHEN, then the suites. Read the last lines. When a test fails, name its file and say whether your work changed it (run it at BASE to say so), and report that line first.
STEP 5. Commit on your own branch as `rocketnet-lan-bench-runner-client: <what>` with the trailer. The pull request body states the diff stat, the client flags, the merge sample, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

BUILD BOUND. Wherever this card says `make` for the build, run instead `(mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. >/dev/null && nice make -j4 --no-print-directory)`. Run builds and suites one at a time, never in the background. Commit the work before any CI-equivalent gate that reads git status.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
This card builds only: it runs the programs only for their usage and `--self-test-result`, ssh-es nowhere and loads nothing.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-bench-runner-server" :feature "LAN bench runner, piece 2: the server pinned-transport mode" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-bench-runner-server sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: feature
PRIORITY: critical
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-design
LAND-AFTER: rocketnet-lan-bench-runner-client
SPLIT: piece of rocketnet-lan-bench-runner (Glenn 10-18 split request); siblings rocketnet-lan-bench-runner-client,rocketnet-lan-bench-runner-scripts
SOURCE: the owner, 2026-10-18: phase 2 (transport throughput toward 10 Gbit/s on the LAN) and phase 3 (1000 concurrent clients) against one server per transport, on the LAN only. docs/LAN-BENCH.md (rocketnet-lan-bench-design, landed by the need) is the design; it extends the stress run's programs and scripts (docs/LIVE-XDP.md, scripts/live-xdp-fleet*, test/live_verify.h). This card builds the load modes and the LAN orchestration; the per-transport run cards run them.
PATHS: server.c,CMakeLists.txt
TEST: none because the server mode is exercised by the run cards; the gate is the build on Linux and the existing suites
DONE-WHEN: the server program builds on Linux at head and its new flags (the pinned-transport mode, max_clients 1000, the per-core CPU, pps, Gbit/s, counters and resident-memory recording) show in its usage; `./bin/test` and `./bin/functional` are green
TOOLCHAIN: cmake and a C++17 compiler, bash and python3, on the Linux member JOB.md names; libsodium is vendored under sodium/
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 90 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 90 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the stress run's programs, test/live_verify.h, the counters API, ssh, bash and python3's standard library.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)); a server driver is pinned with ROCKETNET_SERVER_DRIVER. `make` builds into bin/; `./bin/test` is the unit suite; `./bin/functional` the loopback tier. docs/LAN-BENCH.md (read) governs every choice below; where this card and the document differ, the document wins and the report names the difference.

# LAN bench runner, piece 2: the server pinned-transport mode

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Build what docs/LAN-BENCH.md specifies for the server, in server.c (and CMakeLists.txt only if the build needs it): the server's pinned-transport mode on each server host (Linux, the native Windows side, macOS; the driver pinned through ROCKETNET_SERVER_DRIVER and confirmed by the server's own driver line in its log) at max_clients 1000, with per-core CPU, pps, Gbit/s, counters and resident memory recorded at the interval the document names. Code for the Windows and macOS hosts stays behind the platform guards the program already uses; the gate here is the Linux build. This piece does not touch client.c, any script or docs/LAN-BENCH.md.

SPLIT NOTE. This card is one of three pieces of rocketnet-lan-bench-runner, run in parallel: rocketnet-lan-bench-runner-client (the client load modes and scripts/lan-bench-merge), rocketnet-lan-bench-runner-server (the server pinned-transport mode) and rocketnet-lan-bench-runner-scripts (scripts/lan-bench, scripts/lan-bench-remote, the example inventory and docs/LAN-BENCH.md `## Running it`). Where docs/LAN-BENCH.md's File plan names files for rocketnet-lan-bench-runner, this piece takes only its own files from that list; another piece's file named there is not a difference to stop on, but a file the File plan names for rocketnet-lan-bench-runner that no piece holds is. FLAG NAMES. The client and server flags are the ones docs/LAN-BENCH.md names; where it names none, the piece adding the flag picks it, prints it in its usage, and lists it in its report. The orchestration piece (rocketnet-lan-bench-runner-scripts) lands after both program pieces; it passes the names docs/LAN-BENCH.md gives and lists every flag it passes in its report, so a mismatch is caught at its landing.

STEP 1. Confirm `uname -s` is Linux; cd into the staged checkout JOB.md names; git log --oneline -1. Confirm docs/LAN-BENCH.md exists at BASE; if not, stop with not-done.
STEP 2. Read docs/LAN-BENCH.md whole, docs/LIVE-XDP.md, server.c and test/live_verify.h.
STEP 3. Build it in the files PATHS and NEW name, and only those. Cite the docs/LAN-BENCH.md section from each function that implements a rule.
STEP 4. Run the gate: DONE-WHEN, then the suites. Read the last lines. When a test fails, name its file and say whether your work changed it (run it at BASE to say so), and report that line first.
STEP 5. Commit on your own branch as `rocketnet-lan-bench-runner-server: <what>` with the trailer. The pull request body states the diff stat, the server flags, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

BUILD BOUND. Wherever this card says `make` for the build, run instead `(mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. >/dev/null && nice make -j4 --no-print-directory)`. Run builds and suites one at a time, never in the background. Commit the work before any CI-equivalent gate that reads git status.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
This card builds only: it runs the programs only for their usage and `--self-test-result`, ssh-es nowhere and loads nothing.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-bench-runner-scripts" :feature "LAN bench runner, piece 3: the orchestration (twin)" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-bench-runner-scripts sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: feature
PRIORITY: critical
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-design
SPLIT: piece of rocketnet-lan-bench-runner (Glenn 10-18 split request); siblings rocketnet-lan-bench-runner-client,rocketnet-lan-bench-runner-server
LAND-AFTER: rocketnet-lan-bench-runner-client,rocketnet-lan-bench-runner-server
SOURCE: the owner, 2026-10-18: phase 2 (transport throughput toward 10 Gbit/s on the LAN) and phase 3 (1000 concurrent clients) against one server per transport, on the LAN only. docs/LAN-BENCH.md (rocketnet-lan-bench-design, landed by the need) is the design; it extends the stress run's programs and scripts (docs/LIVE-XDP.md, scripts/live-xdp-fleet*, test/live_verify.h). This card builds the load modes and the LAN orchestration; the per-transport run cards run them.
PATHS: docs/LAN-BENCH.md
NEW: docs/LAN-BENCH.md,scripts/lan-bench,scripts/lan-bench-remote,scripts/lan-bench.inventory.example
TEST: none because the scripts are exercised by the run cards; the gate is the orchestration's dry run and its refusals
DONE-WHEN: `scripts/lan-bench --dry-run --phase throughput --transport linux-portable --inventory scripts/lan-bench.inventory.example` and the same with `--phase clients1000` exit 0 and print every step per host (lock, build, start, load, collect, restore, unlock) with the client and server command lines in full and `dry-run: no host contacted`; the same for every transport docs/LAN-BENCH.md lists; it refuses (nonzero, naming the row) an inventory copy whose server row is the test server, and an xdp server row not marked `xdp-held`; `bash -n` passes on every new script; docs/LAN-BENCH.md gains only a short `## Running it` section; `./bin/test` stays green
TOOLCHAIN: cmake and a C++17 compiler, bash and python3, on the Linux member JOB.md names; libsodium is vendored under sodium/
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 90 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 90 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the stress run's programs, test/live_verify.h, the counters API, ssh, bash and python3's standard library.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)); a server driver is pinned with ROCKETNET_SERVER_DRIVER. `make` builds into bin/; `./bin/test` is the unit suite; `./bin/functional` the loopback tier. docs/LAN-BENCH.md (read) governs every choice below; where this card and the document differ, the document wins and the report names the difference.

# LAN bench runner, piece 3: the orchestration (twin)

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Build what docs/LAN-BENCH.md specifies for the orchestration: scripts/lan-bench (it refuses an inventory whose server row is the test server, and an xdp server row not marked `xdp-held`; `--phase throughput|clients1000`, `--transport <name>`, `--inventory <file>`, `--dry-run`) with the per-host lock, the XDP SAFETY steps for the xdp transport, the before/after restore check and teardown on every exit path; scripts/lan-bench-remote, the per-host step script (never inline quoted strings over ssh, each step under a timeout), whose collect step gathers the client result files for scripts/lan-bench-merge (piece rocketnet-lan-bench-runner-client); scripts/lan-bench.inventory.example with placeholder hosts only. docs/LAN-BENCH.md gains only a short `## Running it` section. This piece does not touch client.c, server.c or CMakeLists.txt. If the program pieces have landed at your BASE, check every flag the scripts pass against the programs' usage; if not, list every flag the scripts pass in the report.

SPLIT NOTE. This card is one of three pieces of rocketnet-lan-bench-runner, run in parallel: rocketnet-lan-bench-runner-client (the client load modes and scripts/lan-bench-merge), rocketnet-lan-bench-runner-server (the server pinned-transport mode) and rocketnet-lan-bench-runner-scripts (scripts/lan-bench, scripts/lan-bench-remote, the example inventory and docs/LAN-BENCH.md `## Running it`). Where docs/LAN-BENCH.md's File plan names files for rocketnet-lan-bench-runner, this piece takes only its own files from that list; another piece's file named there is not a difference to stop on, but a file the File plan names for rocketnet-lan-bench-runner that no piece holds is. FLAG NAMES. The client and server flags are the ones docs/LAN-BENCH.md names; where it names none, the piece adding the flag picks it, prints it in its usage, and lists it in its report. The orchestration piece (rocketnet-lan-bench-runner-scripts) lands after both program pieces; it passes the names docs/LAN-BENCH.md gives and lists every flag it passes in its report, so a mismatch is caught at its landing.

STEP 1. Confirm `uname -s` is Linux; cd into the staged checkout JOB.md names; git log --oneline -1. Confirm docs/LAN-BENCH.md exists at BASE; if not, stop with not-done.
STEP 2. Read docs/LAN-BENCH.md whole, docs/LIVE-XDP.md, scripts/live-xdp-fleet* and client.c and server.c (read only, for their flags).
STEP 3. Build it in the files PATHS and NEW name, and only those. Cite the docs/LAN-BENCH.md section from each function that implements a rule.
STEP 4. Run the gate: DONE-WHEN, then the suites. Read the last lines. When a test fails, name its file and say whether your work changed it (run it at BASE to say so), and report that line first.
STEP 5. Commit on your own branch as `rocketnet-lan-bench-runner-scripts: <what>` with the trailer. The pull request body states the diff stat, the dry-run output, every flag the scripts pass, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

BUILD BOUND. Wherever this card says `make` for the build, run instead `(mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. >/dev/null && nice make -j4 --no-print-directory)`. Run builds and suites one at a time, never in the background. Commit the work before any CI-equivalent gate that reads git status.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
This card builds only: it runs the programs only for their usage and `--self-test-result`, ssh-es nowhere and loads nothing.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-throughput-linux-gso" :feature "LAN throughput: the Linux gso server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-throughput-linux-gso sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner
SOURCE: the owner, 2026-10-18: transport throughput toward 10 Gbit/s on the LAN, one card per server transport, on the LAN only, today. This card is the Linux `gso` server driver (linux-gso). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory, never the test server; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/throughput-linux-gso.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/throughput-linux-gso.md` fails at BASE and at head that file, titled `## Throughput: linux-gso`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the server-side Gbit/s and pps each way, CPU per core on the server, loss, RTT p50/p99, the verify counts (every message still verified) and the ceiling hit with its cause (NIC, core, syscall rate, switch hop), the before/after restore check of the server host, and one verdict line: PASS when every message verifies and the throughput row is recorded with its ceiling and cause; a verify failure, crash or host left unrestored is FAIL
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN throughput: the Linux gso server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase throughput --transport linux-gso` once, end to end, as `## Running it` says, then record it in docs/lan-bench/throughput-linux-gso.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on that LAN Linux bench (never the test server, whose traffic crosses the studio's 20 to 50 Mbit/s uplink), its driver pinned to `gso` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/throughput-linux-gso.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/throughput-linux-gso.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-throughput-linux-gso: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-throughput-linux-io-uring" :feature "LAN throughput: the Linux io_uring server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-throughput-linux-io-uring sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner
SOURCE: the owner, 2026-10-18: transport throughput toward 10 Gbit/s on the LAN, one card per server transport, on the LAN only, today. This card is the Linux `io_uring` server driver (linux-io-uring). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory, never the test server; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/throughput-linux-io-uring.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/throughput-linux-io-uring.md` fails at BASE and at head that file, titled `## Throughput: linux-io-uring`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the server-side Gbit/s and pps each way, CPU per core on the server, loss, RTT p50/p99, the verify counts (every message still verified) and the ceiling hit with its cause (NIC, core, syscall rate, switch hop), the before/after restore check of the server host, and one verdict line: PASS when every message verifies and the throughput row is recorded with its ceiling and cause; a verify failure, crash or host left unrestored is FAIL
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN throughput: the Linux io_uring server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase throughput --transport linux-io-uring` once, end to end, as `## Running it` says, then record it in docs/lan-bench/throughput-linux-io-uring.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on that LAN Linux bench (never the test server, whose traffic crosses the studio's 20 to 50 Mbit/s uplink), its driver pinned to `io_uring` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/throughput-linux-io-uring.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/throughput-linux-io-uring.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-throughput-linux-io-uring: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-throughput-linux-portable" :feature "LAN throughput: the Linux portable server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-throughput-linux-portable sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner
SOURCE: the owner, 2026-10-18: transport throughput toward 10 Gbit/s on the LAN, one card per server transport, on the LAN only, today. This card is the Linux `portable` server driver (linux-portable). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory, never the test server; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/throughput-linux-portable.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/throughput-linux-portable.md` fails at BASE and at head that file, titled `## Throughput: linux-portable`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the server-side Gbit/s and pps each way, CPU per core on the server, loss, RTT p50/p99, the verify counts (every message still verified) and the ceiling hit with its cause (NIC, core, syscall rate, switch hop), the before/after restore check of the server host, and one verdict line: PASS when every message verifies and the throughput row is recorded with its ceiling and cause; a verify failure, crash or host left unrestored is FAIL
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN throughput: the Linux portable server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase throughput --transport linux-portable` once, end to end, as `## Running it` says, then record it in docs/lan-bench/throughput-linux-portable.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on that LAN Linux bench (never the test server, whose traffic crosses the studio's 20 to 50 Mbit/s uplink), its driver pinned to `portable` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/throughput-linux-portable.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/throughput-linux-portable.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-throughput-linux-portable: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-throughput-linux-xdp" :feature "LAN throughput: the Linux xdp server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-throughput-linux-xdp sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner,rocketnet-xdp-on-link-reply-uses-peer-mac
SOURCE: the owner, 2026-10-18: transport throughput toward 10 Gbit/s on the LAN, one card per server transport, on the LAN only, today. This card is the Linux `xdp` server driver (linux-xdp). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here one LAN Linux bench with a 10G NIC chosen from docs/LAN-BENCH.md's inventory, held for XDP for the run and bound on that NIC under XDP SAFETY, never the test server; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/throughput-linux-xdp.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/throughput-linux-xdp.md` fails at BASE and at head that file, titled `## Throughput: linux-xdp`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the server-side Gbit/s and pps each way, CPU per core on the server, loss, RTT p50/p99, the verify counts (every message still verified) and the ceiling hit with its cause (NIC, core, syscall rate, switch hop), the before/after restore check of the server host, and one verdict line: PASS when every message verifies and the throughput row is recorded with its ceiling and cause; a verify failure, crash or host left unrestored is FAIL
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: xdp
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host. Stop the run, FAIL, if any XDP SAFETY check fails. Stop with not-done `XDP bench not held` if the inventory's XDP server row is not marked `xdp-held`.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN throughput: the Linux xdp server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase throughput --transport linux-xdp` once, end to end, as `## Running it` says, then record it in docs/lan-bench/throughput-linux-xdp.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on that held LAN XDP bench, bound on its 10G NIC under XDP SAFETY (never the test server), its driver pinned to `xdp` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

XDP SAFETY (OWNER RULING, 2026-10-18, applied to the LAN bench held for XDP; \"AF_XDP steers only our packets; we never take over the NIC\"). 1. rocketnet's XDP program redirects only packets to the server's UDP port and returns XDP_PASS for everything else (server_xdp/server_xdp.c:126-128 at 8ddc93ec1771: `udp->dest != config->public_port || !address_match` returns XDP_PASS), so ssh and all other traffic pass; server_xdp/server_xdp.c is never edited. 2. Before the program loads, a dead-man's switch is armed on the XDP bench: a systemd-run timer (15 minutes) that runs `ip link set dev <nic> xdp off` and kills the server unless the orchestrator cancels it after proving ssh still works; re-armed for each phase. 3. Within 10 s of the program loading the orchestrator proves ssh to the XDP bench works; if not, it waits for the switch to fire and stops the run, FAIL. 4. The 10G NIC's driver and version (`ethtool -i <nic>`) are read first, read-only, and recorded; the driver is used as it is, never rebuilt or reloaded by the run. 5. After the run XDP is unloaded and `ip link show dev <nic>` shows no program attached; that line is recorded. 6. Root on the XDP bench is only for loading and unloading XDP, the dead-man timer, and starting and stopping the server, by non-interactive `sudo -n` on the landed script files; if `sudo -n true` fails or asks for a password, stop with not-done and report it; never type a password. The XDP SAFETY lines go into the results file.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/throughput-linux-xdp.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/throughput-linux-xdp.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-throughput-linux-xdp: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-throughput-macos-msgx" :feature "LAN throughput: the macOS msgx server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-throughput-macos-msgx sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner
SOURCE: the owner, 2026-10-18: transport throughput toward 10 Gbit/s on the LAN, one card per server transport, on the LAN only, today. This card is the macOS `msgx` server driver (macos-msgx). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here the macOS bench; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/throughput-macos-msgx.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/throughput-macos-msgx.md` fails at BASE and at head that file, titled `## Throughput: macos-msgx`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the server-side Gbit/s and pps each way, CPU per core on the server, loss, RTT p50/p99, the verify counts (every message still verified) and the ceiling hit with its cause (NIC, core, syscall rate, switch hop), the before/after restore check of the server host, and one verdict line: PASS when every message verifies and the throughput row is recorded with its ceiling and cause; a verify failure, crash or host left unrestored is FAIL
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: darwin
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN throughput: the macOS msgx server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase throughput --transport macos-msgx` once, end to end, as `## Running it` says, then record it in docs/lan-bench/throughput-macos-msgx.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on the macOS bench, its driver pinned to `msgx` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/throughput-macos-msgx.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/throughput-macos-msgx.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-throughput-macos-msgx: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-throughput-macos-portable" :feature "LAN throughput: the macOS portable server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-throughput-macos-portable sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner
SOURCE: the owner, 2026-10-18: transport throughput toward 10 Gbit/s on the LAN, one card per server transport, on the LAN only, today. This card is the macOS `portable` server driver (macos-portable). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here the macOS bench; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/throughput-macos-portable.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/throughput-macos-portable.md` fails at BASE and at head that file, titled `## Throughput: macos-portable`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the server-side Gbit/s and pps each way, CPU per core on the server, loss, RTT p50/p99, the verify counts (every message still verified) and the ceiling hit with its cause (NIC, core, syscall rate, switch hop), the before/after restore check of the server host, and one verdict line: PASS when every message verifies and the throughput row is recorded with its ceiling and cause; a verify failure, crash or host left unrestored is FAIL
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: darwin
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN throughput: the macOS portable server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase throughput --transport macos-portable` once, end to end, as `## Running it` says, then record it in docs/lan-bench/throughput-macos-portable.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on the macOS bench, its driver pinned to `portable` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/throughput-macos-portable.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/throughput-macos-portable.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-throughput-macos-portable: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-throughput-windows-iocp" :feature "LAN throughput: the Windows iocp server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-throughput-windows-iocp sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner
SOURCE: the owner, 2026-10-18: transport throughput toward 10 Gbit/s on the LAN, one card per server transport, on the LAN only, today. This card is the Windows `iocp` server driver (windows-iocp). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here the native Windows side of the Linux-on-Windows bench; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/throughput-windows-iocp.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/throughput-windows-iocp.md` fails at BASE and at head that file, titled `## Throughput: windows-iocp`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the server-side Gbit/s and pps each way, CPU per core on the server, loss, RTT p50/p99, the verify counts (every message still verified) and the ceiling hit with its cause (NIC, core, syscall rate, switch hop), the before/after restore check of the server host, and one verdict line: PASS when every message verifies and the throughput row is recorded with its ceiling and cause; a verify failure, crash or host left unrestored is FAIL
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: windows
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN throughput: the Windows iocp server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase throughput --transport windows-iocp` once, end to end, as `## Running it` says, then record it in docs/lan-bench/throughput-windows-iocp.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on the native Windows side of the Linux-on-Windows bench, its driver pinned to `iocp` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/throughput-windows-iocp.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/throughput-windows-iocp.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-throughput-windows-iocp: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-throughput-windows-portable" :feature "LAN throughput: the Windows portable server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-throughput-windows-portable sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner
SOURCE: the owner, 2026-10-18: transport throughput toward 10 Gbit/s on the LAN, one card per server transport, on the LAN only, today. This card is the Windows `portable` server driver (windows-portable). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here the native Windows side of the Linux-on-Windows bench; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it.
NEW: docs/lan-bench/throughput-windows-portable.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/throughput-windows-portable.md` fails at BASE and at head that file, titled `## Throughput: windows-portable`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the server-side Gbit/s and pps each way, CPU per core on the server, loss, RTT p50/p99, the verify counts (every message still verified) and the ceiling hit with its cause (NIC, core, syscall rate, switch hop), the before/after restore check of the server host, and one verdict line: PASS when every message verifies and the throughput row is recorded with its ceiling and cause; a verify failure, crash or host left unrestored is FAIL
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: windows
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN throughput: the Windows portable server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase throughput --transport windows-portable` once, end to end, as `## Running it` says, then record it in docs/lan-bench/throughput-windows-portable.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on the native Windows side of the Linux-on-Windows bench, its driver pinned to `portable` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/throughput-windows-portable.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/throughput-windows-portable.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-throughput-windows-portable: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-lan-throughput-windows-rio" :feature "LAN throughput: the Windows rio server" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
     :brief "RESULT: rocketnet-lan-throughput-windows-rio sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-lan-bench-runner,rocketnet-rio-driver-core
SOURCE: the owner, 2026-10-18: transport throughput toward 10 Gbit/s on the LAN, one card per server transport, on the LAN only, today. This card is the Windows `rio` server driver (windows-rio). OWNER RULING (2026-10-18): for these transport tests the server runs on the LAN machine that hosts the transport, here the native Windows side of the Linux-on-Windows bench; clients run on the other LAN benches; the machine is restored afterwards. docs/LAN-BENCH.md (rocketnet-lan-bench-design) is the design; scripts/lan-bench (rocketnet-lan-bench-runner, landed by the need) runs it. The rio driver is added by rocketnet-rio-driver-core (landed by the need); if `rio` is not in sdk/rocketnet_drivers.inc at BASE, stop with not-done.
NEW: docs/lan-bench/throughput-windows-rio.md
TEST: none because this card runs a live LAN measurement and records it; the gate is the run's own pass rule
DONE-WHEN: `ls docs/lan-bench/throughput-windows-rio.md` fails at BASE and at head that file, titled `## Throughput: windows-rio`, holds the run's date, the commit every machine built, the server host's shape and NIC (label only), the load benches' shapes, the server-side Gbit/s and pps each way, CPU per core on the server, loss, RTT p50/p99, the verify counts (every message still verified) and the ceiling hit with its cause (NIC, core, syscall rate, switch hop), the before/after restore check of the server host, and one verdict line: PASS when every message verifies and the throughput row is recorded with its ceiling and cause; a verify failure, crash or host left unrestored is FAIL
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/lan-bench-remote does
GATE-NEEDS: windows
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop with not-done `LAN busy` if the dry run finds another run's lock on any host.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (sdk/rocketnet_drivers.inc (read), DRIVERS.md (read)). docs/LAN-BENCH.md (read) is the design, the pass rule and `## Running it`; scripts/lan-bench is the orchestration, scripts/lan-bench-remote the per-host steps, scripts/lan-bench-merge the tables.

# LAN throughput: the Windows rio server

RULINGS (owner, 2026-10-18; these win over docs/LAN-BENCH.md and over any line of this card where they differ): (e) THE LAN PHASES NEVER USE THE TEST SERVER. Every LAN-site bench reaches the test server over the studio's one shared internet uplink, 20 to 50 Mbit/s, so no server for a LAN phase (throughput or 1000 clients) ever runs on the test server and no LAN-phase client ever sends to it. Each Linux transport's server runs on one LAN Linux bench chosen by NIC speed from docs/LAN-BENCH.md's inventory; iocp, rio and the Windows portable driver on the native Windows side of the Linux-on-Windows bench; msgx and the macOS portable driver on the macOS bench. The xdp transport's server runs on a LAN Linux bench with a 10G NIC, held for XDP for the run (the per-machine XDP mode: the coordinator holds it and marks its inventory row `xdp-held`, and the orchestrator refuses an XDP server row without that mark), under the same XDP SAFETY rules, and restored and released afterwards. Every client of these phases runs on the LAN; none crosses the studio uplink. (f) Any client that does cross the studio uplink is capped at 1 Mbit/s total, both directions combined; anything heavier stays on the LAN.

THE TASK. Run `scripts/lan-bench --phase throughput --transport windows-rio` once, end to end, as `## Running it` says, then record it in docs/lan-bench/throughput-windows-rio.md, a new file and the only file this card writes (docs/LAN-BENCH.md links it already). The server runs only on the native Windows side of the Linux-on-Windows bench, its driver pinned to `rio` (ROCKETNET_SERVER_DRIVER) and confirmed by the server's own driver line in its log; the clients run on the other LAN benches the inventory names, never on a cloud bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach a better number. A bench that cannot build or reach the server is named with its log, not removed.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LAN-BENCH.md; apply STOP. Confirm `ls docs/lan-bench/throughput-windows-rio.md` fails; if the file exists, stop with not-done.
STEP 2. Run the dry run with the same flags and `--inventory <inventory>`; keep its output. Then the real run, in the foreground, within the duration docs/LAN-BENCH.md names plus its build and restore bound.
STEP 3. Run `scripts/lan-bench-merge` over the collected results; write docs/lan-bench/throughput-windows-rio.md. Record the server host's before/after restore lines.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench or the server fails, name its label, reason and log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-lan-throughput-windows-rio: <what>` with the trailer. The pull request body states the verdict line, the table, every failure with its log, the restore lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a real thread or the wall clock is a functional test or a live run, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on a machine held for XDP (the per-machine XDP mode), never on an unheld shared bench; for the LAN phases that is the LAN bench held for the xdp run, under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after). No LAN phase runs anything on the test server.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on its transport host, through the scripts, and that host is restored after; every other bench runs only clients. Remote steps are the landed script files, never inline quoted strings over ssh, each under a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-live-xdp-fleet-run" :feature "The live stress run: the XDP server on the test server, one client from every fleet bench" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release. Carried: branch sprint/rocketnet-live-xdp-fleet-run.w4.g1.e21 head 5f3574873b2c0d3189e2bf5f03aebe6413456669 (attempt 4, waiting)"
     :brief "RESULT: rocketnet-live-xdp-fleet-run sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-release
DEPENDS-ON: rocketnet-live-xdp-fleet-runner-scripts,rocketnet-live-xdp-fleet-runner-client,rocketnet-live-xdp-fleet-runner-server
SOURCE: OWNER RULING (2026-10-18): the server runs on the test server for this test, which is restored after. The owner, 2026-10-18: \"once we have rocketnet done, what I would like us to do is actual tests where we run rocketnet with XDP on space, and then we connect a client to it from each of our fleet machines (sans space)\"; \"This is effectively a stress test, one client per-fleet machine, of the *internal* protocol\". docs/LIVE-XDP.md (rocketnet-live-xdp-fleet-design) is the design and its pass rule; scripts/live-xdp-fleet (rocketnet-live-xdp-fleet-runner, landed by the need) runs it. This card runs it once, end to end, and records the results. It is a live test on real time and real sockets by design, never a unit gate.
PATHS: docs/LIVE-XDP.md
NEW: docs/LIVE-XDP.md
TEST: none because this card runs the live test and records it; the gate is the run's own pass rule
DONE-WHEN: `grep -c '## Results' docs/LIVE-XDP.md` prints 0 at BASE and 1 at head; at head that section holds the run's date, the commit every machine built, the platform shape of each bench (labels only, no host names), the per bench x direction x mode x channel x size-bucket matrix, the per-bench rows (connect success and time, RTT p50/p99, loss, cycles by kind, reconnect successes and times, failed connects, denials, unexpected disconnects, final clean disconnect), the server and client counter and memory summaries, the bytes each client sent and the run total against the caps of RULINGS (d) (1 Mbit/s total per studio-site client, 100 Mbit/s total per datacenter client, both directions combined; 20 GB per run, at most 1 GB over the studio uplink), the studio-uplink total on its own line, and one verdict line: every bench connects, every message verifies, and zero failed connects, denials and unexpected disconnects on every bench (PASS), or each failure named with its bench label, reason and log excerpt (FAIL); the XDP SAFETY lines are recorded (driver version, switch armed and cancelled per phase, ssh proved within 10 s, `ip link show dev <nic>` with no program attached after); the test server is left as found (server stopped, timers gone, any firewall rule the run opened closed)
TOOLCHAIN: bash, ssh and python3 on the coordinator's machine; each remote machine builds with its own toolchain from a GitHub clone, as scripts/live-xdp-fleet-remote does
GATE-NEEDS: xdp
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named. Stop at once, before any remote step, with not-done, if the dry run does not show every XDP SAFETY step; stop the run, FAIL, if any XDP SAFETY check fails.
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the scripts the runner card landed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected drivers (DRIVERS.md (read)). docs/LIVE-XDP.md (read) is the design, the pass rule and `## Running it`; scripts/live-xdp-fleet is the orchestration, scripts/live-xdp-fleet-remote the per-host steps and scripts/live-xdp-fleet-merge the table.

# The live stress run: the XDP server on the test server, one client from every fleet bench

RULINGS (owner, 2026-10-18; these win over docs/LIVE-XDP.md and over any line of this card where they differ): (a) THE UPLINK. The ruling that lets the XDP server bind the test server's only NIC, its public uplink, under the dead-man timer, the watchdog, the 10-second ssh check and the verify-only pre-flight, covers every live XDP test on the test server under the XDP SAFETY rules, not only this run; worst case the owner can force-reboot the test server from its provider's console. (b) THE CAP IS A TOTAL. Every per-client internet cap is the client's total, both directions combined (a 100 Mbit/s cap is 50 Mbit/s each way). (c) ROOT. Root on the test server is allowed for exactly these steps and no other: loading and unloading the XDP program, arming and cancelling the systemd-run dead-man timer, and starting and stopping the server. Use the sudo path the earlier XDP and test-server cards used there: non-interactive `sudo -n <script>` on the landed script files (the host's sudo is sudo-rs: pass environment with `--preserve-env=<list>`, never -E). Before the first root step run `sudo -n true`; if it fails or asks for a password, stop with not-done and report it; never type or supply a password. (d) THE STUDIO UPLINK (replaces any LAN flat-out path to the test server). The studio site reaches the test server over one shared internet uplink of 20 to 50 Mbit/s, so every client that crosses it (every bench on the studio site, including the Linux-on-Windows bench's two sides and the macOS bench) is capped at 1 Mbit/s total, both directions combined (500 kbps each way, client send and the server's per-slot send); the datacenter benches do not cross it and keep 100 Mbit/s total each (50,000 kbps each way). No client runs the LAN flat-out rate against the test server; anything heavier belongs in the LAN phases (docs/LAN-BENCH.md). The server creates with the lowest cap (500 kbps) and raises a slot on HELLO only to that slot's inventory cap. THE BUDGET is lowered from 200 GB to 20 GB per run in total, of which at most 1 GB crosses the studio uplink (the bound at 7 studio clients x 1 Mbit/s x 621 s / 8 is about 0.55 GB, and 2 datacenter clients x 100 Mbit/s x 621 s / 8 about 15.5 GB); the orchestrator computes both bounds from the inventory and refuses a plan over either, and the run records the studio-uplink total separately.

THE TASK. Run the live test once, end to end, as `## Running it` says, then record it. The server runs only on the test server, bound on its public NIC under XDP SAFETY; one client runs on each bench the inventory names: every Linux bench, the Linux and the native Windows side of the Linux-on-Windows bench, and the macOS bench; never a server on a bench. The inventory file (host names) is the coordinator's, in the job directory, never committed. Every message verifies or the run is a FAIL; any failed connect, denial or unexpected disconnect is a FAIL with its reason. A FAIL is a real result: record it, commit the section, finish HOLD with --failed and the logs; never rerun selectively, retune or drop a bench to reach PASS. A bench that cannot build or reach the server is named with its log, not removed.

XDP SAFETY (OWNER RULING, 2026-10-18: the test server has one NIC in use, the public one, and the XDP server binds it; \"AF_XDP steers only our packets; we never take over the NIC\"). 1. rocketnet's XDP program redirects only packets to the server's UDP port and returns XDP_PASS for everything else (server_xdp/server_xdp.c:126-128 at 8ddc93ec1771: `udp->dest != config->public_port || !address_match` returns XDP_PASS), so ssh and all other traffic pass; server_xdp/server_xdp.c is never edited. 2. Before the program loads, a dead-man's switch is armed on the test server: a systemd-run timer (15 minutes) that runs `ip link set dev <nic> xdp off` and kills the server unless the orchestrator cancels it after proving ssh still works; re-armed for each phase. 3. Within 10 s of the program loading the orchestrator proves ssh to the test server works; if not, it waits for the switch to fire and stops the run, FAIL. 4. The out-of-tree i40e driver already built from source on the test server is used as it is: its version (`ethtool -i <nic>`) is read first, read-only, and recorded; the in-tree driver refuses the run. 5. After the run XDP is unloaded and `ip link show dev <nic>` shows no program attached; that line is recorded.

STEP 1. On the coordinator's machine, cd into the staged checkout JOB.md names; git log --oneline -1. Read docs/LIVE-XDP.md. Run `grep -c '## Results' docs/LIVE-XDP.md` and keep its line; if it prints 1, stop with not-done.
STEP 2. Run `scripts/live-xdp-fleet --dry-run --inventory <inventory>` and keep its output; then the real run with the same inventory, in the foreground, under the duration docs/LIVE-XDP.md names plus its build and teardown bound. Before the real run, confirm from the dry run's plan that every studio-site client is capped at 1,000 kbps total and the plan's bounds are within 20 GB and 1 GB (RULINGS (d)); if not, stop with not-done naming the line.
STEP 3. Run `scripts/live-xdp-fleet-merge` over the collected results; write `## Results` into docs/LIVE-XDP.md, and only that section. Confirm the test server's teardown (XDP detached, server stopped, firewall as before) and record the check lines in the report.
STEP 4. Run the gate: the DONE-WHEN checks. Read the last lines. When a bench fails, name its label, its reason and its log excerpt first.
STEP 5. Commit on your own branch as `rocketnet-live-xdp-fleet-run: <what>` with the trailer. The pull request body states the verdict line, the per-bench rows, every failure with its log, the teardown check lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. This card is the live tier, on real time and sockets by design.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`. Each remote machine clones from GitHub; nothing is built on the studio Mac.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- XDP runs only on the dedicated test server, never on a shared bench; there it binds the public NIC only under the XDP SAFETY rules (a dead-man's switch armed first, ssh proved within 10 s, unloaded after).

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
The server runs only on the test server, through the scripts; a bench runs only its one client. Remote steps are the landed script files, never inline quoted command strings over ssh; each is bounded by a timeout.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")
    (:id "rocketnet-nightly-load-tier" :feature "The load harness runs nightly on Linux, macOS and Windows" :cells (n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was ready in stream rocketnet-release. Carried: branch sprint/rocketnet-nightly-load-tier.w3.g7.e21 head f851097a382202ae94eeae03321f0cf9a311de4f (attempt 5, ready)"
     :brief "RESULT: rocketnet-nightly-load-tier sha=000000000000 tier: pro
CARRY: rocketnet-nightly-load-tier attempt 1 head=af67068675cbee3495ba28a3e9b455355e9ec1cb
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: ci
PRIORITY: normal
STREAM: rocketnet-release
SOURCE: docs/LOAD.md and bin/load come from rocketnet-load-many-clients-harness. nightly-soak.yml (read) is the worked example of one job with Linux fleet, macos-latest and windows-2025 legs, including the Windows build tree paths.
PATHS: .github/workflows/nightly-load.yml
NEW: .github/workflows/nightly-load.yml
DEPENDS-ON: rocketnet-load-many-clients-harness
TEST: none because the change is CI configuration; DONE-WHEN names the checks
DONE-WHEN: nightly-load.yml parses as YAML; it has schedule and workflow_dispatch triggers and one job with three legs, the Linux fleet runner label nightly-soak.yml uses, macos-latest and windows-2025, each building in Release as nightly-soak.yml does and running `load 1000 300` (bin/load, or the Windows build tree's path) with a timeout of three times that; any leg's exit other than 0 fails the job; each leg prints the summary line and uploads it as an artifact; if rocketnet-nightlies-run-on-the-sprint-branch has landed at BASE, the job uses its ROCKETNET_NIGHTLY_REFS ref dimension the same way; on the Linux member, the Linux leg's commands run by hand exit 0; the pull request states the hosted-runner minutes per night
TOOLCHAIN: a YAML parser on the member (python3 with PyYAML, or ruby); cmake and a C compiler for the Linux leg's commands
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 60 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 60 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none new; the standard library and the vendored sodium already in the tree are all this card uses.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with a message layer above it. `make` builds into bin/; `./bin/test` is the unit suite (no sockets, no real time; `./bin/test <name>` runs one test and prints `unknown unit test <name>` with exit 2 for a name it does not hold), `./bin/functional` the loopback tier. Root test_unit_<topic>.cpp files register with REGISTER_UNIT_TEST (test_unit_registry.h (read)) and build with no CMakeLists.txt edit (CMakeLists.txt globs test_unit_*.cpp). sdk/rocketnet.h (read) is the public C surface; every new export also goes in tools/exports.txt, which tools/check_exports.py (read) compares on every platform. docs/CHANNELS.md (read) is the message-layer design; STANDARD.md (read) the wire and connect standard; docs/TLA-INVENTORY.md (read) the state-machine inventory. tla/ holds the models (an MC wrapper, a cfg per case, a README per module with its reversed witnesses); tla/CASES.tsv (read) names each case once, tla/RUNS.tsv (read) holds the measured records written by `tlacheck run` and joined by `tlacheck merge --keep tla/RUNS.tsv`, never edited by hand (tla/README.md (read), \"Refreshing records after a model edit\"), and the lander refuses a change whose cases have no current record. tla/COVERAGE.tsv (read) records which code each model covers. CMakeLists.txt keeps -ffp-contract=off on every target.

# The load harness runs nightly on Linux, macOS and Windows

THE TASK. Write .github/workflows/nightly-load.yml as specified, copying nightly-soak.yml's runner selection, dependency and build steps for each OS exactly. Change no other workflow. The macOS and Windows legs are verified by the coordinator with a workflow_dispatch run after landing; name that as not done.

RED AT BASE (by listing the tree at 1fcaea324a): `test -e .github/workflows/nightly-load.yml || echo missing` prints `missing`.

STEP 1. Confirm `uname -s` is Linux; cd into the staged checkout JOB.md names and run git log --oneline -1; it is a NEW worktree on the branch this card names. Re-run the RED AT BASE command and keep its line; if the thing this card adds or fixes already holds at your BASE, stop with not-done and the line.
STEP 2. Red first: the RED AT BASE line is the failing line this card turns green; keep it for the pull request.
STEP 3. Make it pass in the files PATHS and NEW name, and only those. Cite the design section, the STANDARD.md section or the CI job from each change that implements a rule.
STEP 4. Run the gate: DONE-WHEN, then the YAML parse and the Linux leg's commands and read the last lines. When a test fails, name its file and say whether that file was changed by your work (yours) or is unchanged (already red at BASE: run the same test on the unchanged base to say so), and report that line first.
STEP 5. Commit on your own branch as `rocketnet-nightly-load-tier: <what>` with the trailer. Nothing reaches the forge from inside the wall: in the job the git shim records a push, the pull request is the finish JOB.md names, and the member makes both, against the BASE: line's branch, from outside the wall when the card finishes. The pull request body states the diff stat, the red line, the tests with what each pins, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end and there is nothing else to write, the gate's lines in the pull request body; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES.
Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0.
Make the smallest change, with no refactor.
Never touch the space game's baseline or delta code.
Never run `nova-sprint friend stage`. Clone on the bench from GitHub, never on the studio Mac.
The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
Never read outside the job's working directory.
Schema-table bounds: a bound is declared only if every writer and reader path enforces it. Otherwise the field stays open (*string, []T).

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine; TLC runs on a Linux bench, never on the coordinator's machine. Never search /, ~ or /Volumes; stay inside the checkout and the job directory.
Anything that uses real time, sockets or the network is a functional test and belongs in ./bin/functional, never ./bin/test.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.
ROUTE NOTE (coordinator, 2026-10-19; why attempts 2 and 3 held). The workflow YAML is on the carried head af6706867 and parsed; the attempts held waiting for an uncontended bench and for macOS and Windows evidence. Neither is owed by this card. (1) The Linux leg's commands by hand: build in Release as the leg does, then run `./bin/load 1000 300` once on the bench JOB.md names, as soon as its load guard admits a job; other work on the bench is not a reason to defer. bin/load and its five criteria are already on BASE (rocketnet-load-many-clients-harness), so a FAIL is not this card's: report its summary line first, rerun it once to tell an environment fault from a harness defect, and finish with both lines (LAND when the YAML holds and the run passed; HOLD naming the failed criterion otherwise, which the coordinator turns into its own card). (2) The macOS and Windows legs are verified by the coordinator's workflow_dispatch run after landing, as THE TASK says: name them as not done, never hold for them. Keep nightly-soak.yml's runner selection exactly.")))))
