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
   :text "The secure connect from standard/TRANSPORT.md: netcode 1.02 rebuilt on public keys, with every refusal counted."
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
   :text "The message layer of standard/MESSAGES.md: channels, priorities, budgets and one fixed region per connection."
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
    (:feature "Endpoint and slots as pure machines (standard/MESSAGES.md)" :cells (y) :evidence "bbb62e8 endpoint_back_to_back_over_lossy_array, endpoint_never_allocates_or_reads_a_clock; 9bca948 slots_observe_and_admit; tla/RNEndpoint.tla 3c3c048")
    (:feature "CONFIG announcement: both ends prove the same configuration" :cells (y) :evidence "1026843 message_config_equal_connects_and_verifies, message_config_mismatch_ends_both_connections; functional 4906e5d")
    (:feature "Message layer soaked at full configured load" :cells (n) :note "bounded exhaustion soak c335529; the many-client load harness bin/load (docs/LOAD.md, f49cc28) is built, a recorded full-load run is owed")
    (:feature "Broadcast send" :cells (y) :evidence "7e2106e; message_server_broadcast_reaches_every_connected, message_server_broadcast_skips_free_and_exhausted")
    (:feature "Flows: server-side subscription groups (later)" :cells (n) :note "REQUIREMENTS.md step 3; designed and modelled, not built: docs/FLOWS.md, tla/RNFlows.tla (2c7236b)")))

  (:id "transports" :title "Transports"
   :text "The server socket transports, selected at runtime by the per-endpoint ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT. Loopback is pin-only."
   :columns ("portable" "gso" "msgx" "iocp" "xdp" "io_uring" "loopback" "rio")
   :rows
    ((:feature "Server transport" :cells (y y y y y y y y) :evidence "rio 367a33f, 1dcb6b8 (rocketnet_test_server_rio_probe, rocketnet_test_server_rio_wire_ipv4, rocketnet_test_server_rio_wire_ipv6, rocketnet_test_server_rio_wire_dual_stack, rocketnet_test_server_rio_send_full; tla/RNRioDriver.tla 5255a0d); portable: rocketnet_server_socket_linux.cpp; gso 0801c9a; msgx 8d54e30; iocp 45da97d; xdp c0b0928; io_uring f7cfe0f and 7be058d (test_server_uring_probe, test_server_uring_wire_ipv4, test_server_uring_wire_ipv6, test_server_uring_wire_dual_stack, test_server_uring_send_overflow, test_server_uring_receive_rearms_after_no_buffers); loopback bfcb888 (transport_loopback_binds_and_discovers_a_port, transport_loopback_exchanges_both_ways, transport_loopback_full_queue_drops_and_counts, transport_loopback_destroy_frees_the_port)")
     (:feature "Selectable by pin at runtime" :cells (y y y y y y y y) :evidence "rio 0c31bf7 (rocketnet_test_server_transport_pin_rio); 10308e4; rocketnet_test_server_transport_pin_portable, _gso, _msgx, _iocp, _xdp; io_uring 2d2c057 (test_server_transport_pin_io_uring); loopback 5d059d8 and 1c6f91a (test_transport_pin_only, test_server_transport_pin_loopback, test_transport_env_pins_loopback)")))

  (:id "planned-transports" :title "Transports and clients"
   :text "The transport and client work shipped in v1.0.0, and the later milestones behind it."
   :columns ("done")
   :rows
   ((:feature "Linux UDP_GRO receive coalescing" :cells (y) :evidence "the gso transport enables UDP_GRO per socket and splits by the stride cmsg; GRO_BATCHES in the gso wire case")
    (:feature "Windows Registered I/O (RIO) server transport" :cells (y) :evidence "367a33f, 1dcb6b8, 0c31bf7; rocketnet_test_server_rio_wire_ipv4, rocketnet_test_server_transport_pin_rio; tla/RNRioDriver.tla 5255a0d; the rio column of the Transports table")
    (:feature "Windows USO and URO in the IOCP transport, probed with fallback" :cells (y) :evidence "a8ffbac; rocketnet_test_server_iocp_uso_uro_probe, rocketnet_test_server_iocp_uso_wire; the coalesce codec 7df4c3c (coalesce_split_by_stride_round_trips)")
    (:feature "Client socket I/O off the main thread, gated by the measured main-thread cost per frame" :cells (y) :evidence "docs/CLIENT-IO.md b0b4527, tla/RNClientIo.tla; 4f8a8e9 client_io_backend_with_wait_uses_io_thread; Linux a60ed16, Apple 151508f, Windows 8ddc93e (rocketnet_test_client_io_linux_handoff_round_trip, rocketnet_test_client_io_apple_handoff_round_trip, rocketnet_test_client_io_windows_handoff_round_trip); decrypt off the main thread c236806; the 100 us gate 20d8b35, bench/client_main_thread_cost.md, .github/workflows/nightly-frame-budget.yml f12db2b")
    (:feature "Console client transports (PS4, PS5, Switch, Xbox GDK)" :cells (n) :note "the source layers are under a different license; the owner's ruling is owed before any card")
    (:feature "XDP/AF_XDP on AWS GameLift fleets: research" :cells (y) :evidence "6930277 docs/GAMELIFT.md")))

  (:id "browser-clients" :title "Browser clients (later)"
   :text "Browsers cannot open raw UDP, so a browser client reaches a RocketNet server through a WebTransport or WebRTC data channel front-end, either a relay or a second front-end in the server."
   :columns ("done")
   :rows
   ((:feature "WebTransport (HTTP/3 datagrams) client and front-end" :cells (n))
    (:feature "WebRTC data channel client and front-end" :cells (n))
    (:feature "Relay or second front-end that bridges either into the UDP transport" :cells (n))))

  (:id "dedicated-nic" :title "Dedicated-NIC transports (later)"
   :text "These transports are only for a deployment where a second NIC is exclusively the game server's and the main NIC carries admin traffic; RocketNet never takes over a shared NIC, which is why AF_XDP (the kernel stack keeps the NIC, the server steers only its own packets) is the transport for shared NICs."
   :columns ("done")
   :rows
   ((:feature "netmap transport on FreeBSD (netmap is in the FreeBSD kernel)" :cells (n))
    (:feature "netmap transport on Linux, only if netmap enters the mainline kernel (today it needs an out-of-tree module and patched NIC modules)" :cells (n))
    (:feature "DPDK transport" :cells (n))))

  (:id "xdp" :title "XDP (later)"
   :text "The XDP server transport. Further XDP work comes later, separately, by the owner's call; RocketNet already runs with XDP in the game, tested to 10 Gbps."
   :columns ("done")
   :rows
   ((:feature "Kernel prefilter with AF_XDP rings and counted verdicts" :cells (y) :evidence "c0b0928; xdp_host_every_counter_has_a_case; libxdp all-or-zero reservation, send pressure retains the staged packet, no worker exits: test_lifecycle.cpp check_frame_conservation, check_send_handoff")
    (:feature "Native attach on a production NIC with internet handshakes" :cells (y) :evidence "8d54514")
    (:feature "Kernel filter decisions run as host C in the unit suite" :cells (y) :evidence "bf2aecd")
    (:feature "IPv6 in the kernel program and the userspace path" :cells (y) :evidence "66d8282, 037c71c; ipv6_frame_matches_golden")
    (:feature "Kernel filter counters through the transport counter surface" :cells (y) :evidence "394dc9f the prefilter counter capability; 5d736db the xdp transport adapter; prefilter_counters_map_each_kernel_counter, prefilter_counters_are_assigned_not_added, prefilter_counters_xdp_transport_reports_them" :note "the live per-CPU read through a committed XDP socket is exercised only on the dedicated XDP machine")
    (:feature "Kernel replay protection with a read-only high-water map" :cells (y) :evidence "bace052 tla/RNXdpReplay.tla; 7013a56 kernel rule and xdp_host_replay_never_writes_the_map, xdp_host_replay_drops_behind_the_window; 26f2fac socket writer; 22f87a9 transport walk; 4f1d2a9 xdp_host_replay_table_set_get_reset; 3e1fd05 replay_drops_behind_the_window, replay_never_writes_the_map, replay_key_layout_matches_header; 306bd2b standard/TRANSPORT.md sizing")
    (:feature "standard/TRANSPORT.md states the kernel replay map is sized apart from the userspace replay window" :cells (y) :evidence "c1a6ff2 rocketnet-standard-sizes-kernel-replay-separately; standard/TRANSPORT.md Replay window sizing (ROCKETNET_XDP_REPLAY_WINDOW)")
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
    (:feature "Multi-day soaks past 2^32 packets on one connection" :cells (n) :note "soak_sim and soak run unbounded on real hardware for days of parallel clients before the connect layer is believed; polite and garbage traffic, counters asserted while running, past 2^32 packets on one connection so sequence wrap is routine. The nightly CI soak is the bounded sentinel")
    (:feature "LAN throughput and 1000-client benchmarks across every server transport" :cells (n))
    (:feature "Full-matrix profiling and instruction-budget pass" :cells (n))))

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
    (:feature "Connection and security machines as pure modules" :cells (n) :note "client slots, token tables and the replay window are pure; the client connection, the per-address handshake, keep-alives and the cookie gate remain to be extracted")
    (:feature "Deterministic simulation testing across fault-injected seeds" :cells (n))
    (:feature "Extended formal verification of packet readers, ack blocks and sequence buffers" :cells (n))))

  (:id "platforms" :title "Platforms"
   :text "Where RocketNet builds and passes."
   :columns ("linux" "macos" "windows")
   :rows
   ((:feature "Per-commit CI, Debug and Release" :cells (y y y) :evidence "ci.yml: native Debug and Release, portable and auto transport selection, serialization, s390x, sanitizers, bounded simulated soak, corpus replay, standard-value conformance")
    (:feature "Shared export list compared" :cells (y y y) :evidence "tools/check_exports.py, tools/exports.txt; 095f374 production sources separate from tests, strict endpoint port parsing")
    (:feature "Nightly soak" :cells (y y y) :evidence "nightly-soak.yml; 63a25be macOS; 9ee42a2 Windows")))

  (:id "api" :title "Public API"
   :text "The C surface in sdk/rocketnet.h and what a consumer needs to use it."
   :columns ("done")
   :rows
   ((:feature "Transport C surface: rocketnet_client_transport_* and rocketnet_server_transport_*" :cells (y) :evidence "ruled #77; sdk/rocketnet.h; packet contexts checked against single-consumption leases: test_lifecycle.cpp check_packet_leases")
    (:feature "Root header pure C: the client and server with their functions, the address type, nothing else" :cells (y) :evidence "5c3027f, dcd941f rocketnet-header-is-clean-c (6b73068); sdk/rocketnet.h plain C99 with one extern C block; rocketnet_test_public_client_server_queries, rocketnet_test_config_refuses_rocketnet_transport; clean at /W4 and -Wextra 9ead512" :note "ruled: netcode-style, real non-mangled symbols for C and FFI; a C++ inside is an implementation privilege")
    (:feature "Message objects in C: rocketnet_message_client_* and rocketnet_message_server_*" :cells (y) :evidence "2c1aa7b")
    (:feature "ABI revision and layout queries" :cells (y) :evidence "sdk/rocketnet_abi.cpp; tools/abi_mismatch.cpp")
    (:feature "Installable CMake package with C and C++ consumers" :cells (y) :evidence "tools/check_package.py, tools/package_consumer")
    (:feature "Vendored sodium, no system libsodium" :cells (y) :evidence "23d4c40; sodium/")
    (:feature "Structured create errors and FFI-safe logging" :cells (y) :evidence "status codes 4e2d6f1 (status_strings_cover_every_code); create_ex 3e82436, b2bc4ea, 33f84d3 (server_transport_create_ex_reports_every_status, client_transport_create_ex_reports_every_status, lifecycle_ex_reports_every_status); preformatted log callback 194ecc5 (log_callback_preformatted_contract)")
    (:feature "Sized APIs for compatible struct evolution" :cells (y) :evidence "a5941cf; sized_config_accepts_every_known_size, sized_config_refuses_unknown_size; docs/ABI-EVOLUTION.md")
    (:feature "Whole-transport memory estimates and operator budgets" :cells (y) :evidence "rocketnet_server_required_memory and rocketnet_client_required_memory 1d9f830; the host memory estimate cad7a0c (rocketnet_test_server_host_memory_estimate_bounds_the_plan); operator budgets 227d895 USAGE.md; BUILDING.md Pre-allocation server resource plans" :note "an estimate of what is requested, with its exclusions named (kernel accounting beyond the requests, libbpf and libxdp, guard pages, application memory); not an available-RAM check")
    (:feature "C++ ClientTransport and ServerTransport interfaces" :cells (y) :evidence "925cbd7 cpp_transport_interfaces_map_one_to_one, cpp_transport_fake_implements_the_interface; 8985b00 rocketnet_test_cpp_transports_connect_over_loopback")
    (:feature "Shared library build, checked exports, callable from C and C++" :cells (y) :evidence "73733ad tools/check_header_exports.py; a26fd6c export count derived; a6e3991 tools/check_dll_consumer.py, tools/dll_consumer")
    (:feature "API reference generated by MrDocs" :cells (y) :evidence "baf841d mrdocs.yml, docs/mrdocs, .github/workflows/docs.yml")
    (:feature "USAGE.md: the client and server API, with worked usage" :cells (y) :evidence "b7f92c4, d0a5339")
    (:feature "CI gate requiring every public symbol to be documented" :cells (n))))

  (:id "release" :title "Release and process"
   :columns ("done")
   :rows
   ((:feature "Release hardening flags" :cells (y) :evidence "fe7966f; tools/check_hardening.py")
    (:feature "Reproducible vendored sources" :cells (y) :evidence "23d4c40 sodium; f6457b5 XDP archive" :note "reproducible release dependencies as a whole are still owed")
    (:feature "First integrating game vendors the SDK and proves it (release gate)" :cells (y) :evidence "sdk/rocketnet.h")
    (:feature "Supported-platform policy" :cells (y) :evidence "34406ce docs/PLATFORMS.md; tools/check_platform_policy.py")
    (:feature "Security disclosure and release process" :cells (y) :evidence "6795ac1 SECURITY.md, docs/RELEASING.md; tools/check_release_doc.py")
    (:feature "The example server runs out of the box" :cells (y) :evidence "14557ab")
    (:feature "The normative standard in standard/, a conformance vector per section" :cells (y) :evidence "61c0e67 skeleton and tools/conformance/check_rn_standard.py; one card per section, each with its vectors (standard_wire_primitives_vectors 062e2cb through standard_channels_receipts_security_vectors 263a088); standard/RATIONALE.md 3ba4ff2")
    (:feature "The old to-do and readiness files retired into this roadmap" :cells (y) :evidence "rocketnet-retire-todo-and-readiness" :note "TODO.md and TRANSPORT_READINESS.md deleted; every open item is a row here")))

  (:id "second-deliverable" :title "Second deliverable: keys and backends (later)"
   :text "What customers need to run their own connect-token backend: their own keypairs, the guide, and working example backends in Go and C#. After v1.0.0, by the owner's call."
   :columns ("done")
   :rows
   ((:feature "Key generator: a rocketnet_keygen tool, built with the SDK, writing a fresh Ed25519 signing keypair and the token encryption key for production" :cells (n) :note "the server takes the public key; the private key stays on the backend; it replaces the published development keys. References: netcode's keygen and yojimbo's (keygen.cpp at the repository root is the starting point)")
    (:feature "Keys documentation: a KEYS.md guide to generating keypairs, where each key lives, rotation, and rocketnet_write_production_connect_token" :cells (n) :note "covers where each key lives (backend, server, and never shipped in the client), rotation, and development keys versus production keys. References: netcode's and yojimbo's key documentation")
    (:feature "Example backends in Go and C#: a minimal HTTP backend in each that authenticates a player (stub), mints a connect token with the production keys for a chosen server address, and returns it for rocketnet_client_connect" :cells (n) :note "each comes with a connect token format test vector shared with the C SDK, so tokens minted in Go and C# are accepted by the server. References: the netcode connect token and the yojimbo matcher backend (Go)")))

  (:id "correctness-post-v1" :title "Correctness after v1.0.0 (later)"
   :text "Correctness work that follows the v1.0.0 milestone, by the owner's call."
   :columns ("done")
   :rows
   ((:feature "TLA+ invariants NoReceiveUnlessRunning and NoAdmitUnlessRunning on the server lifecycle, plus tests that a stopping or stopped server reads no packet and admits no client" :cells (n) :note "the query itself ships as rocketnet_server_running in v1.0.0")
    (:feature "Deterministic simulation fault injectors: reorder, delay, clock skew, and crash and restart of a client and of the server, drawn from the seed" :cells (n) :note "built from tla/RNSimFaults.tla; after v1.0.0, by the owner's call")))))
