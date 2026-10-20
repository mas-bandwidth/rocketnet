; RocketNet roadmap data. ROADMAP.md is rendered from this file by
; tools/roadmap/render.py (make roadmap); make roadmap-check fails when the
; committed page is not what this renders.
;
; An area declares :columns transports (one column per server transport, in
; the order the automatic ladder ranks them) or :columns platforms (Linux,
; macOS, Windows). A row has one cell per column: y (done and tested there),
; n (not done, or not proven at this base) or - (does not apply). Every y cell
; carries evidence: a CI job that runs there, a test named, or a landed
; commit. :evidence is either one string that names each column it covers,
; or a list of (column "evidence") pairs, one per y cell. Evidence stays here;
; the page shows only the marks.
;
; Every push runs the unit and functional suites (./bin/test, ./bin/functional,
; test_lifecycle) and the happy path on Linux, macOS and Windows (ci.yml), so
; a row those suites test is y on all three. A row only a Linux job checks (a
; sanitizer, the nightly soak, the examples, a dst run) is y on Linux and n on
; macOS and Windows, whose nightly jobs are disabled. A document, a model or a
; proof is checked on Linux and is - on macOS and Windows.
;
; Rows that carry :id came from the landed and the moved-out work lists, now
; dissolved into their subject areas; :brief keeps a moved item's whole brief
; and :merged keeps the items one row's cells stand for.
(:title "RocketNet roadmap"
 :text ("Every feature RocketNet has, and every one still to come, by area: a column per transport where a feature is per transport, and a column per platform where it is not. ✅ is done and tested there; ❌ is not done, or not yet proven there; — does not apply. Each table's complete row is the share of ✅ among the cells that apply. Items marked (later) are deliberately after the current work, by the owner's call. An area marked (next milestone) is the next milestone, by the owner's call.")
 :areas
 ((:id "handshake" :title "Connect handshake and security"
   :text "The secure connect of standard/TRANSPORT.md: netcode 1.02 rebuilt on public keys, with every refusal counted."
   :columns platforms
   :rows
   ((:feature "Public-key connect tokens: mint, verify and open in the standard's order" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; rocketnet_test_connect_token_frozen, rocketnet_test_connect_token_mint")
    (:feature "Deterministic connect token vectors for minters in other languages" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; e05088e rocketnet-connect-token-vectors.w1")
    (:feature "6-byte prefilter header with a per-title filter value and epoch" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 52d9be7; rocketnet_test_prefilter")
    (:feature "Cookie gate: bounds, signature, expiry and address checked" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 64ffe20; rocketnet_test_cookie, rocketnet_test_cookie_bounds_signature_verifies")
    (:feature "Cookie key rotates on its period" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 6b60b32, 5e534d4; test_cookie_rotation_draws_a_fresh_key")
    (:feature "Per-connection keys with forward secrecy, secrets wiped on every exit" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; rocketnet_test_connection_keys, rocketnet_test_client_terminal_states_wipe_secrets")
    (:feature "Sealed packets with every refusal proven" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; rocketnet_test_sealed_packets, rocketnet_test_sealed_packet_refusals")
    (:feature "Used-token history and one token, one live connection" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; rocketnet_test_used_token_history, server_admits_sequential_fresh_tokens_past_twice_max_clients, server_refuses_a_token_connected_now")
    (:feature "Admission hook drops a packet before any verify" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 9eb49a9; rocketnet_test_admission_hook_drops_before_verify")
    (:feature "Cheap refusals before crypto: a full server refuses before keypair work" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; PR #248, 4901aba; rocketnet_test_full_server_refuses_before_keypair, rocketnet_test_request_path_cheap_refusals_spend_no_crypto")
    (:feature "Bounded handshake work per update" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 1745d64")
    (:feature "Configurable half-open handshake deadline" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 916239a; rocketnet_test_handshake_deadline_is_configured")
    (:feature "Replay window, cleared whole by a long sequence skip" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; a94f99c, 5ab9e5c; rocketnet_test_replay_protection, rocketnet_test_replay_long_skip_clears_whole_window")
    (:feature "Replay window advances only after the connect check" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 8eab0e0 the slot gate before the replay mark")
    (:feature "Hostile fragment ids cannot poison a reassembly slot" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 4f09049; rocketnet_test_fragment_hostile_id_does_not_poison_slot")
    (:feature "Oversize datagrams are dropped, never truncated" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 611788d, 5321d20; rocketnet_test_receive_drops_oversize_datagram")
    (:feature "Release builds refuse development keys" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; e5d3393; rocketnet_test_dev_keys_refused_in_release")
    (:feature "Release builds do not export development token minting" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 5cc44f4, 1b81ce9; rocketnet_test_dev_token_minting_absent_in_release")
    (:feature "Public API is NULL-safe at the boundary" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 3e51b9d; rocketnet_test_public_api_null_is_a_counted_no_op")
    (:feature "Refusal decisions agree with netcode 1.02" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; e5acbbb")
    (:feature "The token is bound at response open" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; b515ec6 rocketnet-token-bound-at-response-open; rocketnet_test_token_bound_at_response_open; tla/RNSecurityTables.tla")
    (:feature "Development keys are off by default and warn on every start" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 6a580ef; dev_keys_warn_on_every_start")
    (:feature "An unauthenticated reply is never larger than its request" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 38caeb2; reply_never_larger_than_request_every_unauthenticated_type, reply_never_larger_than_request_hostile_inputs")
    (:id "rocketnet-salt-mismatch-resends-challenge" :feature "A request with another salt resends the stored challenge" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 43626fc; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-token-expired-reasons-split" :feature "Expired-token refusals counted apart, one counter per cause" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 9b0b2ee; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-connect-token-time-from-game-clock" :feature "Connect-token create and expire checks can read the game clock" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 9efc7cb; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-public-backend-keypair-from-seed" :feature "A backend keypair can be derived from a seed through rocketnet.h" :cells (y y y) :evidence "Linux, macOS, Windows: landed as b1c5e74; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:feature "Session layer: identity auth with a backend, rolling magic, keyed filter value (later)" :cells (n n n) :note "in part: the design is docs/SESSION.md (eb22aab), the keyed filter value and its two-generation epoch window are in (8e99124 keyed_filter_value_known_answers; ce1b1a0 filter_epoch_window_accepts_two_generations; tla/RNFilterEpochs.tla 4ea64ce), and one token holds one live connection (eef77ac); the backend identity auth and the rolling magic are not built. v2, optional, behind the first deliverable. The 6-byte header with its epoch byte is already in, so the keyed form is a configuration change, not a wire break. It needs a backend that tracks the connected set across servers live: one identity, one seat, so a shared token gives away the sharer's own slot, and the old player is kicked with rocketnet_server_transport_disconnect_client when the same user hash appears elsewhere. An extraction from the game's own backend, not a new design. Then the rolling magic feeds the keyed filter value (SipHash over type, length and the four bytes after the header, one MAC per packet), and a per-title generator emits the constant and the keyed reader in every language plus verifier-clean BPF behind a differential gate. Never: a default magic, or standalone IP pinning")))
  (:id "connection" :title "Connections and the connection layer"
   :text "The unreliable connection layer the message layer runs over: connection state, slots, keep-alives, fragments, acks and the server loop."
   :columns platforms
   :rows
   ((:feature "Client connection state machine with every timeout pairing" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; rocketnet_test_client_timeout_paths; tla/ClientConnection.tla 3afa363")
    (:feature "Server client slots from a free-slot stack" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; c40f599; rocketnet_test_client_slots; tla/ServerConnection.tla eb57f2d")
    (:feature "Encryption mappings with handshake and idle deadlines" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; rocketnet_test_encryption_map, rocketnet_test_server_update_sweep")
    (:feature "Keep-alives paced and suppressed by traffic" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; rocketnet_test_client_keep_alive_suppression")
    (:feature "Payloads both ways from the connection response on" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; rocketnet_test_client_payload_before_first_keep_alive")
    (:feature "Redundant disconnect burst, from the client or the server" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; rocketnet_test_client_disconnect_reports_state_change; rocketnet_server_transport_disconnect_client")
    (:feature "Large payloads fragmented and reassembled" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 7e959ab; rocketnet_test_fragment_vector; reassembly geometry under a 512 MiB component budget, oversized configurations refused before any allocation: test_lifecycle.cpp check_memory_geometry")
    (:feature "Fragments accepted only on a connected slot" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 9e0d822")
    (:feature "Server transport entries carry a connection id" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 65bab41; rocketnet_test_server_transport_entries_carry_connection_id")
    (:feature "Per-packet acks with delivery notification" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 8af8b80; rocketnet_test_ack_block_validity, rocketnet_test_ack_block_build")
    (:feature "Ack history that survives a long lossy path" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 187a875; rocketnet_test_send_history_lossy_long_path; ROCKETNET_HISTORY_PACKETS 4096 default, 4096 to 32768 (sdk/rocketnet_history.h), no frame, send or ack rate assumed; test_lifecycle.cpp check_ack_retention, rocketnet_test_send_history_silent_peer_overflows")
    (:feature "Frame sync: a fixed-rate server loop with priority and drop detection" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; rocketnet_test_frame_sync_advance, rocketnet_test_frame_sync_guard")
    (:feature "Counters on every path, each classified, conserved and named by a test" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 1750e83 rocketnet-counters-classified-and-conserved; cumulative per-connection wire totals af8c0b0 and race-free 800119c (wire_totals_count_every_packet_and_byte, rocketnet_test_transport_wire_totals_concurrent_send_exact); rocketnet_num_counters and rocketnet_counter_value for FFI callers; packet-rate statistics in elapsed-time buckets: test_lifecycle.cpp check_stats_cadence")
    (:id "rocketnet-server-receive-drains-backlog" :feature "The server drains its receive queue up to a per-update budget" :cells (y y y) :evidence "Linux, macOS, Windows: landed as c61badc; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-per-connection-reset-is-complete" :feature "Every piece of per-connection state is reset when a connection ends, in one function per side" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 9043173; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-public-step-driven-loopback-client" :feature "A public client can run with no I/O thread, stepped by the game's update" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 16a7c0a; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:feature "Client socket I/O off the main thread, gated by the measured main-thread cost per frame" :cells (y y y) :evidence "Linux: a60ed16 rocketnet_test_client_io_linux_handoff_round_trip and the Linux CI; macOS: 151508f rocketnet_test_client_io_apple_handoff_round_trip, landed from the macOS member; Windows: 8ddc93e rocketnet_test_client_io_windows_handoff_round_trip, landed from the Windows member; docs/CLIENT-IO.md b0b4527, tla/RNClientIo.tla; 4f8a8e9 client_io_backend_with_wait_uses_io_thread; Linux a60ed16, Apple 151508f, Windows 8ddc93e (rocketnet_test_client_io_linux_handoff_round_trip, rocketnet_test_client_io_apple_handoff_round_trip, rocketnet_test_client_io_windows_handoff_round_trip); decrypt off the main thread c236806; the 100 us gate 20d8b35, bench/client_main_thread_cost.md, .github/workflows/nightly-frame-budget.yml f12db2b")))
  (:id "reliability" :title "Reliability modes"
   :text "The eight RakNet reliabilities, per send, over the connection layer's per-packet acks."
   :columns platforms
   :rows
   ((:feature "UNRELIABLE" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; f6a42cc; unreliable_delivered_as_a_view, unreliable_never_takes_a_number")
    (:feature "UNRELIABLE_SEQUENCED" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 160c5c0, 55cfe99; older_dropped_newest_delivered")
    (:feature "RELIABLE" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 7b83d3c, 40941d1; reliable_exactly_once_any_order_under_loss_dup_reorder")
    (:feature "RELIABLE_ORDERED" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 8cf8bc7, 16531dd; ordered_and_unreliable_end_to_end_in_memory")
    (:feature "RELIABLE_SEQUENCED" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 999c7f5, 41fc05e; reliable_sequenced_block_delivers_whole")
    (:feature "UNRELIABLE_WITH_ACK_RECEIPT" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 0b1f91c; unreliable_receipt_acked_or_loss_exactly_once")
    (:feature "RELIABLE_WITH_ACK_RECEIPT" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; f391ded; reliable_receipt_acked_never_loss")
    (:feature "RELIABLE_ORDERED_WITH_ACK_RECEIPT" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; f391ded, d94a228; ordered_receipt_after_all_chunks")
    (:id "rocketnet-unreliable-size-one-rule" :feature "Oversize unreliable sends are promoted to their reliable twin, as RakNet does, and said so" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 9121067; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")))
  (:id "messages" :title "Channels, sending and memory"
   :text "The message layer of standard/MESSAGES.md: channels, priorities, budgets and one fixed region per connection."
   :columns platforms
   :rows
   ((:feature "Zero-copy send: start, write in place, finish or abort" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 2c1aa7b; reliable_sequenced_c_api_start_finish")
    (:feature "An unreliable message that fits one packet is written in place in that packet, with no copy" :cells (n n n) :note "the message-to-packet step copies the payload at this base (start returns a pointer into the connection's message memory); rocketnet-unreliable-message-is-the-packet removes the copy, and this row turns on its tests when it lands")
    (:feature "Receive and release messages as views" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 2c1aa7b; unreliable_delivered_as_a_view; tla/RNSlots.tla")
    (:feature "N channels with per-record dispatch" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; c79da45 rocketnet-channel-dispatch-and-packet-builder")
    (:feature "Large reliable messages split into blocks and delivered whole" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 16531dd; reliable_sequenced_block_delivers_whole, ordered_receipt_after_all_chunks")
    (:feature "Four send priorities, round robin across channels" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 273cc81, e0733e5; higher_priority_first_within_class")
    (:feature "Small messages aggregated into packets" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; c79da45; builder_obeys_budget_and_class_order")
    (:feature "Send budget in kbps per connection" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; a4ff908; budgets_are_per_connection; a configured default 8d9b806 (send_budget_defaults_from_config, send_budget_env_overrides_config)")
    (:feature "Resend delay from the measured round trip, block chunks paced by it, no backoff" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; f6a3068, 2322d7c; resend_delay_is_min_of_interval_and_rtt, no_backoff_between_resends, chunk_resend_paced_by_rtt_not_interval; tla/RNResendTiming.tla")
    (:feature "A 1400-byte game message fits one packet" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; ff4c7c6; message_of_1400_bytes_is_one_unfragmented_packet")
    (:feature "One fixed region per connection, no allocation after create" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 8b99c72; no_allocation_after_create")
    (:feature "Caller-supplied memory block" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; user_block_is_used_and_nothing_else; aligned allocation keeps its callback and context, late replacement refused: rocketnet_test_allocator_refusal_is_returned, test_lifecycle.cpp check_component_create_sweeps")
    (:feature "Memory exhaustion kicks only that connection" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; c335529; server_kicks_exhausted_client_others_unaffected")
    (:feature "Hostile records and chunks refused" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; d1ff1a2; hostile_chunk_count_or_index_kicks, hostile_small_at_held_ordering_is_refused")
    (:feature "Endpoint and slots as pure machines" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; bbb62e8 endpoint_back_to_back_over_lossy_array, endpoint_never_allocates_or_reads_a_clock; 9bca948 slots_observe_and_admit; tla/RNEndpoint.tla 3c3c048")
    (:feature "CONFIG announcement: both ends prove the same configuration" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 1026843 message_config_equal_connects_and_verifies, message_config_mismatch_ends_both_connections; functional 4906e5d")
    (:feature "Message layer soaked at full configured load" :cells (n n n) :note "bounded exhaustion soak c335529; the many-client load harness bin/load (docs/LOAD.md, f49cc28) is built, a recorded full-load run is owed")
    (:feature "Broadcast send" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 7e2106e; message_server_broadcast_reaches_every_connected, message_server_broadcast_skips_free_and_exhausted")
    (:id "rocketnet-public-not-released-counters" :feature "The unreleased-message refusal is a public counter on client and server" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 5a6cc45; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-channels-unreliable-oversize-counter-doc" :feature "The channel documentation names the counter an oversize unreliable send moves" :cells (y - -) :evidence "Linux: a document; landed as 1ad9b49")
    (:feature "Flows: server-side subscription groups (later)" :cells (n n n) :note "REQUIREMENTS.md step 3; designed and modelled, not built: docs/FLOWS.md, tla/RNFlows.tla (2c7236b)")))
  (:id "transports" :title "Transports"
   :text ("The server transports below the connection layer, one column each, in the order the automatic ladder ranks them: xdp, gso, io_uring and portable on Linux, msgx and portable on macOS, portable, iocp and rio on Windows. The per-endpoint ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT select one at runtime; loopback is pin-only." "AF_XDP serves IPv4 and IPv6 end to end (8a35a14): a server serves the family of its public address, and the ladder offers xdp for either. Its IPv6 cell is ❌ because no CI step runs AF_XDP end to end (it needs root and a VM guest); the msgx and iocp IPv6 cells are ❌ until the CI transport family matrix runs green on them." "— is a transport the feature does not apply to: xdp bypasses the kernel's UDP stack, loopback is in-process, and a row that names one transport's own mechanism is — for the others.")
   :columns transports
   :rows
   ((:feature "Server transport" :cells (y y y y y n y y) :evidence ((xdp "c0b0928; the xdp rows of the XDP table") (msgx "8d54e30") (gso "0801c9a; test_server_gso_wire_ipv6, test_server_gso_wire_dual_stack") (io_uring "f7cfe0f and 7be058d (test_server_uring_probe, test_server_uring_wire_ipv4, test_server_uring_wire_ipv6, test_server_uring_wire_dual_stack, test_server_uring_send_overflow, test_server_uring_receive_rearms_after_no_buffers)") (portable "ci.yml runs ./bin/functional with the portable rung pinned on the ubuntu-24.04 Debug and Release (b) legs, green in runs 39271096164 and 39286548764; rocketnet_server_socket_linux.cpp") (rio "367a33f, 1dcb6b8 (rocketnet_test_server_rio_probe, rocketnet_test_server_rio_wire_ipv4, rocketnet_test_server_rio_wire_ipv6, rocketnet_test_server_rio_wire_dual_stack, rocketnet_test_server_rio_send_full; tla/RNRioDriver.tla 5255a0d)") (loopback "bfcb888 (driver_loopback_binds_and_discovers_a_port, driver_loopback_exchanges_both_ways, driver_loopback_full_queue_drops_and_counts, driver_loopback_destroy_frees_the_port)")) :note "iocp: the transport is 45da97d, but ci.yml's Functional suite with IOCP selected step on windows / Release (a) is red in runs 39271096164 and 39286548764; the cell is n until that step is green")
    (:feature "Selectable by pin at runtime" :cells (y y y y y y y y) :evidence ((xdp "10308e4 rocketnet_test_server_driver_pin_xdp") (msgx "10308e4 rocketnet_test_server_driver_pin_msgx") (gso "10308e4 rocketnet_test_server_driver_pin_gso") (io_uring "2d2c057 test_server_driver_pin_io_uring") (portable "10308e4 rocketnet_test_server_driver_pin_portable") (iocp "10308e4 rocketnet_test_server_driver_pin_iocp") (rio "0c31bf7 rocketnet_test_server_driver_pin_rio") (loopback "5d059d8 and 1c6f91a (test_driver_pin_only, test_server_driver_pin_loopback, test_driver_env_pins_loopback)")))
    (:feature "IPv6 and dual-stack binds" :cells (n n y y y n y n) :evidence ((gso "test_server_gso_wire_ipv6, test_server_gso_wire_dual_stack in the Linux CI") (io_uring "7be058d test_server_uring_wire_ipv6, test_server_uring_wire_dual_stack in the Linux CI") (portable "ci.yml runs ./bin/functional with ROCKETNET_SERVER_TRANSPORT=portable on Linux, holding test_transport_ipv6_loopback") (rio "1dcb6b8 rocketnet_test_server_rio_wire_ipv6, rocketnet_test_server_rio_wire_dual_stack, landed from the Windows member")) :note "xdp: AF_XDP serves IPv4 and IPv6 end to end at 8a35a14 (DRIVERS.md); the unit cases xdp_frame_parse_ipv6_udp and xdp_ladder_keeps_xdp_for_ipv6 run in ./bin/test, and the end-to-end case xdp_max_size_both_ways_ipv6 needs root and a VM guest, which no ci.yml step runs (the transport family matrix has no xdp cell); the cell is n until a recorded run exercises it. msgx, iocp and loopback: ci.yml's Transport family matrix step (202ef32, 31f4050) runs test_transport_ipv6_loopback and test_transport_dual_stack_ipv4_client on msgx (macOS), iocp (Windows) and loopback, but no ci.yml run has run that step yet (the newest, 39290862797, predates it); each cell is n until that step is green")
    (:feature "Receive coalescing by the kernel (UDP_GRO on Linux, URO on Windows), split by stride" :cells (- n y n n y n -) :evidence ((gso "the gso driver enables UDP_GRO per socket and splits by the stride cmsg; GRO_BATCHES in the gso wire case") (iocp "a8ffbac rocketnet_test_server_iocp_uso_uro_probe; the coalesce codec 7df4c3c coalesce_split_by_stride_round_trips")) :note "msgx, io_uring, portable and rio receive one datagram per call today")
    (:feature "Send segmentation by the kernel (UDP GSO on Linux, USO on Windows), probed with fallback" :cells (- n y n n y n -) :evidence ((gso "0801c9a the gso transport; the gso wire case") (iocp "a8ffbac; rocketnet_test_server_iocp_uso_uro_probe, rocketnet_test_server_iocp_uso_wire; the coalesce codec 7df4c3c (coalesce_split_by_stride_round_trips)")) :note "msgx, io_uring, portable and rio send one datagram per call today")
    (:feature "Registered I/O (RIO) on Windows" :cells (- - - - - - y -) :evidence ((rio "367a33f, 1dcb6b8, 0c31bf7; rocketnet_test_server_rio_wire_ipv4, rocketnet_test_server_driver_pin_rio; tla/RNRioDriver.tla 5255a0d; the rio column of Server transport")))
    (:id "rocketnet-loopback-transport-carries-max-clients" :feature "The loopback transport holds max clients plus the server" :cells (- - - - - - - y) :evidence ((loopback "ad98d7b landed, gated on the Linux member")))))
  (:id "xdp" :title "XDP (later)"
   :text ("The AF_XDP server transport, the xdp column; every other column is —. Further XDP work comes later, separately, by the owner's call; RocketNet already runs with XDP in the game, tested to 10 Gbps.")
   :columns transports
   :rows
   ((:feature "Kernel prefilter with AF_XDP rings and counted verdicts" :cells (y - - - - - - -) :evidence ((xdp "c0b0928; xdp_host_every_counter_has_a_case; libxdp all-or-zero reservation, send pressure retains the staged packet, no worker exits: test_lifecycle.cpp check_frame_conservation, check_send_handoff")))
    (:feature "Native attach on a production NIC with internet handshakes" :cells (y - - - - - - -) :evidence ((xdp "8d54514")))
    (:feature "Kernel filter decisions run as host C in the unit suite" :cells (y - - - - - - -) :evidence ((xdp "bf2aecd")))
    (:feature "Kernel filter counters through the transport counter surface" :cells (y - - - - - - -) :evidence ((xdp "394dc9f the prefilter counter capability; 5d736db the xdp driver adapter; prefilter_counters_map_each_kernel_counter, prefilter_counters_are_assigned_not_added, prefilter_counters_xdp_driver_reports_them")) :note "the live per-CPU read through a committed XDP socket is exercised only on the dedicated XDP machine")
    (:feature "Kernel replay protection with a read-only high-water map" :cells (y - - - - - - -) :evidence ((xdp "bace052 tla/RNXdpReplay.tla; 7013a56 kernel rule and xdp_host_replay_never_writes_the_map, xdp_host_replay_drops_behind_the_window; 26f2fac socket writer; 22f87a9 transport walk; 4f1d2a9 xdp_host_replay_table_set_get_reset; 3e1fd05 replay_drops_behind_the_window, replay_never_writes_the_map, replay_key_layout_matches_header; 306bd2b standard/TRANSPORT.md or standard/MESSAGES.md sizing")))
    (:feature "The standard sizes the kernel replay map apart from the userspace replay window" :cells (y - - - - - - -) :evidence ((xdp "c1a6ff2 rocketnet-standard-sizes-kernel-replay-separately; standard/TRANSPORT.md or standard/MESSAGES.md Replay window sizing (ROCKETNET_XDP_REPLAY_WINDOW)")))
    (:feature "AF_XDP bind requests zero-copy" :cells (y - - - - - - -) :evidence ((xdp "6bb1361; xdp_socket_config_requests_zero_copy_at_bind")))
    (:id "rocketnet-xdp-on-link-reply-uses-peer-mac" :feature "Replies go to the peer's own MAC address on link, to the gateway otherwise" :cells (y - - - - - - -) :evidence ((xdp "f435279 landed, gated on the Linux member")))
    (:feature "AF_XDP on AWS GameLift fleets: the research" :cells (y - - - - - - -) :evidence ((xdp "6930277 docs/GAMELIFT.md")))
    (:id "rocketnet-live-xdp-fleet-runner-client" :feature "The live stress runner: the client stress mode and the merge" :cells (y - - - - - - -) :evidence ((xdp "211833e landed, gated on the Linux member")))
    (:id "rocketnet-live-xdp-fleet-runner-server" :feature "The live stress runner: the server stress mode" :cells (y - - - - - - -) :evidence ((xdp "1d4af4c landed, gated on the Linux member")))
    (:feature "Packet blaster and simulated-client harnesses at line rate" :cells (n - - - - - - -) :note "its own rung: a packet blaster load to prove line rate, and simulated clients against the XDP server. The live XDP fleet run is designed (docs/LIVE-XDP.md, tla/RNLiveFleetRun.tla, bb9b36e) and its runner scripts are in (51f0fae); the run itself is owed")
    (:feature "Ownership and retry paths regression-validated against the 10 Gbps baseline" :cells (n - - - - - - -) :note "the changed XDP ownership and retry paths, against the game deployment; CI runs no privileged XDP traffic and test executables forbid privileged commit")
    (:feature "Privileged syscall-failure coverage in its own lab" :cells (n - - - - - - -) :note "targeted syscall-failure and frame-conservation tests cover different conditions from throughput tests")
    (:id "rocketnet-live-xdp-fleet-run" :feature "The live stress run: the XDP server against one client from every fleet machine (later)" :cells (n - - - - - - -) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release. Carried: branch sprint/rocketnet-live-xdp-fleet-run.w4.g1.e21 head 5f3574873b2c0d3189e2bf5f03aebe6413456669 (attempt 4, waiting)"
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

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")))
  (:id "transport-load" :title "Throughput and load per transport (later)"
   :text ("One server per transport on a LAN, measured: a row per server platform, a cell per transport that platform runs; — is a transport the platform does not run, and loopback is in-process.")
   :columns transports
   :rows
   ((:feature "LAN throughput, a Linux server (later)" :cells (n - n n n - - -) :note "one cell per transport; each cell was its own item"
     :merged ((:id "rocketnet-lan-throughput-linux-xdp" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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
      (:id "rocketnet-lan-throughput-linux-gso" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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
      (:id "rocketnet-lan-throughput-linux-io-uring" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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
      (:id "rocketnet-lan-throughput-linux-portable" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")))
    (:feature "LAN throughput, a macOS server (later)" :cells (- n - - n - - -) :note "one cell per transport; each cell was its own item"
     :merged ((:id "rocketnet-lan-throughput-macos-msgx" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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
      (:id "rocketnet-lan-throughput-macos-portable" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")))
    (:feature "LAN throughput, a Windows server (later)" :cells (- - - - n n n -) :note "one cell per transport; each cell was its own item"
     :merged ((:id "rocketnet-lan-throughput-windows-portable" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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
      (:id "rocketnet-lan-throughput-windows-iocp" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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
      (:id "rocketnet-lan-throughput-windows-rio" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")))
    (:feature "1000 clients over a LAN, a Linux server (later)" :cells (n - n n n - - -) :note "one cell per transport; each cell was its own item"
     :merged ((:id "rocketnet-lan-1000-clients-linux-xdp" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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
      (:id "rocketnet-lan-1000-clients-linux-gso" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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
      (:id "rocketnet-lan-1000-clients-linux-io-uring" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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
      (:id "rocketnet-lan-1000-clients-linux-portable" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")))
    (:feature "1000 clients over a LAN, a macOS server (later)" :cells (- n - - n - - -) :note "one cell per transport; each cell was its own item"
     :merged ((:id "rocketnet-lan-1000-clients-macos-msgx" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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
      (:id "rocketnet-lan-1000-clients-macos-portable" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")))
    (:feature "1000 clients over a LAN, a Windows server (later)" :cells (- - - - n n n -) :note "one cell per transport; each cell was its own item"
     :merged ((:id "rocketnet-lan-1000-clients-windows-portable" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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
      (:id "rocketnet-lan-1000-clients-windows-iocp" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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
      (:id "rocketnet-lan-1000-clients-windows-rio" :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release" 
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

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")))))
  (:id "clients" :title "Further clients"
   :text "Clients beyond the desktop and server platforms; the client on Linux, macOS and Windows is in Connections and the connection layer."
   :columns platforms
   :rows
   ((:feature "Console clients (PS4, PS5, Switch, Xbox GDK)" :cells (n n n) :note "the source layers are under a different license; the owner's ruling is owed before the work starts")))
  (:id "browser-clients" :title "Browser clients (later)"
   :text "Browsers cannot open raw UDP, so a browser client reaches a RocketNet server through a WebTransport or WebRTC data channel front-end, either a relay or a second front-end in the server."
   :columns platforms
   :rows
   ((:feature "WebTransport (HTTP/3 datagrams) client and front-end" :cells (n n n))
    (:feature "WebRTC data channel client and front-end" :cells (n n n))
    (:feature "Relay or second front-end that bridges either into the UDP transport" :cells (n n n))))
  (:id "dedicated-nic" :title "Dedicated-NIC transports (later)"
   :text ("These transports are only for a deployment where a second NIC is exclusively the game server's and the main NIC carries admin traffic; RocketNet never takes over a shared NIC, which is why AF_XDP (the kernel stack keeps the NIC, the server steers only its own packets) is the transport for shared NICs." "FreeBSD has no column of its own; its row is counted under Linux, the other server platform netmap serves.")
   :columns platforms
   :rows
   ((:feature "netmap transport on FreeBSD (netmap is in the FreeBSD kernel)" :cells (n - -))
    (:feature "netmap transport on Linux, only if netmap enters the mainline kernel (today it needs an out-of-tree module and a patched NIC kernel module)" :cells (n - -))
    (:feature "DPDK transport" :cells (n - n))))
  (:id "scaling" :title "Scaling and load" :milestone "next"
   :text "Many channels and many connections, measured."
   :columns platforms
   :rows
   ((:feature "1000-client in-process swarm with churn and garbage traffic" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; a2f59cf; rocketnet_test_swarm")
    (:feature "Channel count a build bound from 1 to 65536" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; ROCKETNET_MAX_CHANNELS with #error bound in sdk/rocketnet.h")
    (:feature "Nightly bounded soak" :cells (y n n) :evidence "Linux: nightly-soak.yml on the Linux runners (the macOS and Windows soak jobs are disabled); 846ae3d; .github/workflows/nightly-soak.yml")
    (:feature "Message layer stressed at 1024 channels" :cells (n n n))
    (:feature "Message layer stressed at 10k channels" :cells (n n n))
    (:feature "About 1000 real connections with channels on, at game-like load" :cells (n n n) :note "the harness is built: bin/load, up to ROCKETNET_MAX_CLIENTS (1000, the default since 962e3a4) clients over loopback on every reliability, five pass criteria (docs/LOAD.md, f49cc28); a recorded 1000-client run is owed")
    (:feature "Socket-level swarm with Go orchestration against real transports" :cells (n n n) :note "the in-process swarm, at the socket level")
    (:feature "Multi-day soaks past 2^32 packets on one connection" :cells (n n n) :note "soak_sim and soak run unbounded on real hardware for days of parallel clients before the connect layer is believed; polite and garbage traffic, counters asserted while running, past 2^32 packets on one connection so sequence wrap is routine. The nightly CI soak is the bounded sentinel")
    (:id "rocketnet-nightly-load-tier" :feature "The load harness runs nightly on Linux, macOS and Windows (later)" :cells (n n n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was ready in stream rocketnet-release. Carried: branch sprint/rocketnet-nightly-load-tier.w3.g7.e21 head f851097a382202ae94eeae03321f0cf9a311de4f (attempt 5, ready)"
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

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with a message layer above it. `make` builds into bin/; `./bin/test` is the unit suite (no sockets, no real time; `./bin/test <name>` runs one test and prints `unknown unit test <name>` with exit 2 for a name it does not hold), `./bin/functional` the loopback tier. Root test_unit_<topic>.cpp files register with REGISTER_UNIT_TEST (test_unit_registry.h (read)) and build with no CMakeLists.txt edit (CMakeLists.txt globs test_unit_*.cpp). sdk/rocketnet.h (read) is the public C surface; every new export also goes in tools/exports.txt, which tools/check_exports.py (read) compares on every platform. docs/CHANNELS.md (read) is the message-layer design; standard/TRANSPORT.md or standard/MESSAGES.md (read) the wire and connect standard; docs/TLA-INVENTORY.md (read) the state-machine inventory. tla/ holds the models (an MC wrapper, a cfg per case, a README per module with its reversed witnesses); tla/CASES.tsv (read) names each case once, tla/RUNS.tsv (read) holds the measured records written by `tlacheck run` and joined by `tlacheck merge --keep tla/RUNS.tsv`, never edited by hand (tla/README.md (read), \"Refreshing records after a model edit\"), and the lander refuses a change whose cases have no current record. tla/COVERAGE.tsv (read) records which code each model covers. CMakeLists.txt keeps -ffp-contract=off on every target.

# The load harness runs nightly on Linux, macOS and Windows

THE TASK. Write .github/workflows/nightly-load.yml as specified, copying nightly-soak.yml's runner selection, dependency and build steps for each OS exactly. Change no other workflow. The macOS and Windows legs are verified by the coordinator with a workflow_dispatch run after landing; name that as not done.

RED AT BASE (by listing the tree at 1fcaea324a): `test -e .github/workflows/nightly-load.yml || echo missing` prints `missing`.

STEP 1. Confirm `uname -s` is Linux; cd into the staged checkout JOB.md names and run git log --oneline -1; it is a NEW worktree on the branch this card names. Re-run the RED AT BASE command and keep its line; if the thing this card adds or fixes already holds at your BASE, stop with not-done and the line.
STEP 2. Red first: the RED AT BASE line is the failing line this card turns green; keep it for the pull request.
STEP 3. Make it pass in the files PATHS and NEW name, and only those. Cite the design section, the standard/TRANSPORT.md or standard/MESSAGES.md section or the CI job from each change that implements a rule.
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
ROUTE NOTE (coordinator, 2026-10-19; why attempts 2 and 3 held). The workflow YAML is on the carried head af6706867 and parsed; the attempts held waiting for an uncontended bench and for macOS and Windows evidence. Neither is owed by this card. (1) The Linux leg's commands by hand: build in Release as the leg does, then run `./bin/load 1000 300` once on the bench JOB.md names, as soon as its load guard admits a job; other work on the bench is not a reason to defer. bin/load and its five criteria are already on BASE (rocketnet-load-many-clients-harness), so a FAIL is not this card's: report its summary line first, rerun it once to tell an environment fault from a harness defect, and finish with both lines (LAND when the YAML holds and the run passed; HOLD naming the failed criterion otherwise, which the coordinator turns into its own card). (2) The macOS and Windows legs are verified by the coordinator's workflow_dispatch run after landing, as THE TASK says: name them as not done, never hold for them. Keep nightly-soak.yml's runner selection exactly.")
    (:id "rocketnet-lan-bench-design" :feature "LAN throughput and 1000 clients, one server per transport: the design (later)" :cells (n - -) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
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
    (:id "rocketnet-lan-bench-runner-client" :feature "LAN bench runner: the client load modes and the merge (later)" :cells (n n n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
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
    (:id "rocketnet-lan-bench-runner-server" :feature "LAN bench runner: the server pinned-transport mode (later)" :cells (n n n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
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
    (:id "rocketnet-lan-bench-runner-scripts" :feature "LAN bench runner: the orchestration (later)" :cells (n n n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-release"
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
    (:id "rocketnet-profile-pass-1-measure" :feature "Profile pass 1, measurement: soak across the matrix, the raw numbers committed (later)" :cells (n n n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-api"
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
    (:id "rocketnet-profile-pass-1-report" :feature "Profile pass 1, report: where the time goes, and what to cut first (later)" :cells (n n n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-api"
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

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")))
  (:id "verification" :title "Verification"
   :text "How RocketNet is checked: unit tests without sockets or real time, functional tests, fuzzing, deterministic simulation, proofs and models. A proof or a model is platform-free: it is checked on Linux, where its job runs, and is — on macOS and Windows."
   :columns platforms
   :rows
   ((:feature "Unit suite with no sockets or real time" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; test_unit_registry.h; socket tests in functional/")
    (:feature "Functional suite over simulated lossy links" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; afa7ed8, 3d4f5b3; rocketnet_test_client_server_lossy_wire")
    (:feature "Seeded network simulator" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; a2f59cf; rocketnet_test_network_simulator")
    (:id "rocketnet-public-link-simulator-t" :feature "The development library can simulate a bad link, through config alone" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 5182763; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-live-message-matrix-functional-r2" :feature "Every size, every type, every channel, both directions, every message verified: the loopback matrix" :cells (y y y) :evidence "Linux, macOS, Windows: landed as b0e8c84; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:feature "Fuzz targets for packets, tokens and messages" :cells (y y y) :evidence "Linux: fuzz-smoke in ci.yml and fuzz-nightly.yml; macOS, Windows: ci.yml replays every target's corpus per push (af9c366); fuzz/rocketnet_fuzz*.cpp; .github/workflows/fuzz-nightly.yml; every target smoked per commit ee35a0b; corpus replay on macOS and Windows af9c366")
    (:feature "Captured pcapng corpus replayed per commit and seeding the fuzzers" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; b4be701 pcapng_corpus_replay, pcapng_corpus_witness_unread_packet_goes_red; 0416321 pcapng_corpus_covers_control_packet_types; seeds 8cceab2; fuzz findings become pcapng regressions fcfe160 (tools/check_fuzz_regressions.py)")
    (:feature "Wycheproof vectors for ChaCha20-Poly1305, Ed25519 and X25519" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; f6fa899 test/wycheproof; wycheproof_chacha20_poly1305_vectors 3fbe4dc, wycheproof_ed25519_vectors 410a517, wycheproof_x25519_vectors 2798d97, each with a witness that goes red")
    (:feature "Happy-path end to end: the real server and client binaries as separate processes" :cells (y y y) :evidence "Linux, macOS, Windows: the happy-path job in ci.yml, one row per platform (1237a47); 1b95d8a tools/happy_path; the happy-path job in ci.yml")
    (:feature "Sanitizers: ASan, UBSan, TSan, MSan, LSan" :cells (y n n) :evidence "Linux: the asan + ubsan and tsan jobs of ci.yml (green in run 39271096164), msan.yml (green in run 39291312697) and nightly-leaks.yml (green in run 39213196659) on the Linux runners; ci.yml, msan.yml, nightly-leaks.yml 079c3fa")
    (:id "rocketnet-run-msan-fails-on-crashed-test-b" :feature "The MSan gate fails on crashed tests" :cells (y - -) :evidence "Linux: landed as cafa290; MemorySanitizer runs on Linux only")
    (:id "rocketnet-base-wire-totals-staged-finish-green" :feature "The staged-finish wire totals case and the MemorySanitizer check pass" :cells (y n n) :evidence "Linux: landed as 8760e60, gated on the Linux member" :note "the wire totals case runs in the suites on all three, but the MemorySanitizer check runs on Linux only, so the row as a whole is proven on Linux")
    (:feature "Big-endian (s390x) runs" :cells (n - -) :note "9c82ef8 nightly-bigendian-system.yml runs the suite under s390x emulation; it has no green run (run 39244732570 fails booting the guest), and ci.yml's big endian s390x legs fail their toolchain check in runs 39271096164 and 39286548764. macOS and Windows have no big-endian target")
    (:id "rocketnet-cachegrind-instruction-gate" :feature "An instruction-count gate for the client frame and the server tick, on every push" :cells (y - -) :evidence "Linux: landed as 56a9840; the gate runs Cachegrind, which runs on Linux")
    (:id "rocketnet-clang-format-from-netcode" :feature "A clang-format configuration derived from the netcode style, with a format check" :cells (y - -) :evidence "Linux: landed as 612f270; the format is platform-free and checked on Linux" :note "the whole-tree reformat itself is a separate item")
    (:id "rocketnet-ci-fleet-runners-lint" :feature "The per-push workflows run on the self-hosted runners, held there by a lint" :cells (y y y) :evidence "Linux, macOS, Windows: e529c71 landed the lint and the Linux jobs; 1237a47 moved the macOS rows and build-and-test-windows onto the self-hosted macOS and Windows runners")
    (:id "rocketnet-nightly-on-fleet" :feature "The nightly workflows run on the self-hosted runners" :cells (y n n) :evidence "Linux: landed as 4f24f4d, gated on the Linux member" :note "the macOS and Windows nightly jobs are disabled until those runners take them")
    (:id "rocketnet-dst-design" :feature "Deterministic simulation testing: the design" :cells (y - -) :evidence "Linux: a document; landed as 1a2aab4")
    (:id "rocketnet-dst-fault-model" :feature "Deterministic simulation: the fault injector, as a TLA+ model first" :cells (y - -) :evidence "Linux: a model, checked by tlc.yml; landed as b522f5a")
    (:id "rocketnet-dst-harness-core" :feature "Deterministic simulation, layer 1: the seeded core, N clients and a server, one seed, exact replay" :cells (y n n) :evidence "Linux: landed as 135a41c, gated on the Linux member")
    (:id "rocketnet-dst-invariant-checks" :feature "Deterministic simulation, layer 3: the TLA+ properties as runtime checks" :cells (y n n) :evidence "Linux: landed as 1685d93, gated on the Linux member")
    (:id "rocketnet-dst-fault-injectors" :feature "Deterministic simulation, layer 2: the fault injectors (reorder, delay, clock skew, crash and restart), built from their model (later)" :cells (n n n) :note "not in v1.0.0; built from tla/RNSimFaults.tla, on top of the seeded core and the runtime checks, as docs/DST.md section 3 states")
    (:id "rocketnet-dst-ci-wiring" :feature "Deterministic simulation, layer 4: per-push shards, the nightly campaign, and the planted-bug proof (later)" :cells (n n n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-tests"
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
THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with a message layer above it. `make` builds into bin/; `./bin/test` is the unit suite (no sockets, no real time; `./bin/test <name>` runs one test and prints `unknown unit test <name>` with exit 2 for a name it does not hold), `./bin/functional` the loopback tier. Root test_unit_<topic>.cpp files register with REGISTER_UNIT_TEST (test_unit_registry.h (read)) and build with no CMakeLists.txt edit (CMakeLists.txt globs test_unit_*.cpp). sdk/rocketnet.h (read) is the public C surface; every new export also goes in tools/exports.txt, which tools/check_exports.py (read) compares on every platform. docs/CHANNELS.md (read) is the message-layer design; standard/TRANSPORT.md or standard/MESSAGES.md (read) the wire and connect standard; docs/TLA-INVENTORY.md (read) the state-machine inventory. tla/ holds the models (an MC wrapper, a cfg per case, a README per module with its reversed witnesses); tla/CASES.tsv (read) names each case once, tla/RUNS.tsv (read) holds the measured records written by `tlacheck run` and joined by `tlacheck merge --keep tla/RUNS.tsv`, never edited by hand (tla/README.md (read), \"Refreshing records after a model edit\"), and the lander refuses a change whose cases have no current record. tla/COVERAGE.tsv (read) records which code each model covers. CMakeLists.txt keeps -ffp-contract=off on every target.


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
    (:id "rocketnet-sim-and-reset-tests-bite" :feature "The simulator and reset tests catch the bugs they exist to catch (later)" :cells (n n n) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-tests"
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
    (:feature "CBMC proofs of bounded readers and fragment bounds" :cells (y - -) :evidence "Linux: a proof, checked by cbmc.yml on the Linux runners, green in run 39189679648; fa39b10, 7cb3cab; proofs/cbmc: connect token read 4255608, packet prefilter open 03ea6b9, message read max 5dc66fd, fragment reassembly 2d1d4ae, replay advance 4d58844; .github/workflows/nightly-proofs.yml")
    (:feature "TLA+ models: connection, security, replay, channels, window, scheduler, slots, receipts" :cells (y - -) :evidence "Linux: a model, checked by tlc.yml on the Linux runners, green in run 39271096109; tla/*.tla; .github/workflows/tlc.yml")
    (:feature "TLA+ model of transport fragments and acks" :cells (y - -) :evidence "Linux: a model, checked by tlc.yml on the Linux runners, green in run 39271096109; e65b838 tla/FragmentsAndAcks.tla; the ack walk bitmaps 91383ff")
    (:feature "TLA+ models of blocks and the endpoint" :cells (y - -) :evidence "Linux: a model, checked by tlc.yml on the Linux runners, green in run 39271096109; e21fb78 tla/RNBlocks.tla; 3c3c048 tla/RNEndpoint.tla")
    (:feature "Every modelled transition cites its module and action" :cells (n - -) :note "in part: every TLA+ module has a tla/COVERAGE.tsv row whose code file cites it, checked by tools/check_tla_coverage.py --all in tlc.yml (617d498, fb828d0, and one card per module); the check is per module, so a citation of each action is not yet checked")
    (:feature "Connection and security machines as pure modules" :cells (n n n) :note "audited, not extracted: docs/PURITY.md (6432754) gives each machine a verdict (client slots, token tables and the replay window pure; the client connection, the per-address handshake, keep-alives and the cookie gate impure) and lists the extraction cards")
    (:id "rocketnet-cbmc-ack-block-read" :feature "CBMC proves the ack-block reader free of out-of-bounds access up to the full packet size (later)" :cells (n - -) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was ready in stream rocketnet-tests. Carried: branch sprint/rocketnet-cbmc-ack-block-read.w1.g1.e21, no head pushed (attempt 5, ready)"
     :brief "RESULT: rocketnet-cbmc-ack-block-read sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-tests
DEPENDS-ON: rocketnet-cbmc-packet-prefilter-open
SOURCE: Glenn, 2026-10-18: \"Let's apply it [Frama-C] where it makes sense.\" The coordinator, the same day: rocketnet's SDK is C++ (sdk/*.cpp), so plain Frama-C does not apply; each untrusted-input reader gets a CBMC bounded check for no out-of-bounds access and no undefined behaviour on any input up to the maximum packet size. proofs/cbmc/ already proves small instances (8-byte fragments, 24-byte message payloads, read/write over 8 bytes); these cards take the readers to the full bound. CBMC 6.6.0 is installed in /usr/bin on amd-1, amd-2, amd-3, hetzner1, hetzner2 and vision (measured 2026-10-18; hulk has none). Read at BASE 1fcaea324.
PATHS: none
START: sdk/rocketnet_ack.cpp (read),sdk/rocketnet_ack.h (read),sdk/rocketnet_replay.h (read),proofs/cbmc/proof_ack_apply_run.c (read),proofs/cbmc/Makefile (read)
NEW: proofs/cbmc/ack_block/proof_ack_block.c,proofs/cbmc/ack_block/proof.mk,proofs/cbmc/ack_block/results.md
TEST: none because the work is CBMC proofs, not a Go package or a unit-suite case; DONE-WHEN names the make target and the lines it prints
TOOLCHAIN: a Linux member with cbmc 6.x and goto-cc on PATH, make, a C/C++ compiler and python3; run `command -v cbmc` and `cbmc --version` first; libsodium is vendored under sodium/ and is stubbed, not proved
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 150 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 150 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: CBMC with the flags proofs/cbmc/Makefile already uses, as the existing proofs do; nothing new in the build.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with a message layer above it. `make` builds into bin/; `./bin/test` is the unit suite, `./bin/functional` the loopback tier. proofs/cbmc/ holds the CBMC proofs: proofs/cbmc/Makefile (read) runs `make -C proofs/cbmc` (the `all` target, which .github/workflows/cbmc.yml runs on pull requests touching sdk/: that target is per-commit CI and this card never adds to it) with CBMC_FLAGS = --bounds-check --pointer-check --signed-overflow-check --unsigned-overflow-check --div-by-zero-check --unwinding-assertions. A proof is a C file that #includes the sdk .cpp it proves (proof_message_read.c includes rocketnet_message.cpp: the worked example, with its nondet_ and __CPROVER_ fallbacks for a plain compiler) and a harness that drives it on symbolic input. Never mirror sdk code into a proof (proof_fragment_bounds.c copies the reassembly function rather than including it: that copy can drift from the code, which is why rocketnet-cbmc-fragment-reassembly proves the real one).

# CBMC: the ack block reader and its apply path stay in bounds for every ack block up to the maximum packet size

THE TASK. An ack block is read from every session packet; its range count and range lengths are attacker-chosen and drive loops over the sent-packet window, so a missed bound is a read past the packet or a write past the window. Write proofs/cbmc/ack_block/proof_ack_block.c, one harness per reader below, each on symbolic input up to the full bound (CBMC PROOF RULES):
- rocketnet_ack_block_read (data of data_bytes symbolic bytes up to the maximum packet size, every highest_sent: ensures no read past data_bytes, a 0 refusal below ROCKETNET_ACK_BLOCK_MIN_BYTES, num_ranges inside the block's documented maximum)
- rocketnet_reconstruct_ack_largest
- rocketnet_ack_endpoint_process_block on a block the reader accepted (the static rocketnet_ack_apply_run at the largest run bound that finishes; the existing ack-run-bound-8 target is not proved and stays as it is)
- rocketnet_ack_block_build and rocketnet_ack_block_write (output of capacity bytes: no write past capacity, a result at most capacity)

Then proofs/cbmc/ack_block/proof.mk (target proofs-ack_block, appended to NIGHTLY) and proofs/cbmc/ack_block/results.md. Worktree: the job directory; branch: your own; base: sprint/rocketnet-proof; touch only the PATHS and NEW files.

THE NIGHTLY LAYOUT (the first CBMC card creates it; every other CBMC card follows it). proofs/cbmc/Makefile gains, below its existing targets and without changing them or `all`: `NIGHTLY :=`, `-include $(wildcard */proof.mk)`, and a `nightly` target that runs every entry of NIGHTLY. Each reader is its own directory proofs/cbmc/<reader>/: proof_<reader>.c (the harness), proof.mk (appends `proofs-<reader>` to NIGHTLY and defines that target: `$(CBMC) $(CBMC_FLAGS) --unwind <n> -I$(SDK) ...` from proofs/cbmc, SDK as the Makefile already defines it, one line of output per harness `<harness> VERIFICATION SUCCESSFUL|FAILED|unwinding assertion failed`), and results.md (per harness: the properties checked, how many passed, the bounds and unwinding used, and every failed or unproved property with its sdk file and line and its reason). .github/workflows/nightly-proofs.yml (schedule and workflow_dispatch only, never push or pull_request; runs-on [self-hosted, Linux, X64, fleet, long]; timeout-minutes 180) runs `make -C proofs/cbmc nightly`, uploading the output as an artifact. A reader joins the nightly by its proof.mk alone, so CBMC cards never edit a shared file after the first.

DONE-WHEN: on the Linux member, `make -C proofs/cbmc proofs-ack_block` runs every harness of this card to its end (paste the VERIFICATION line of each, and the bound each proved); proofs/cbmc/ack_block/results.md records per harness the properties passed out of total and every failed or unproved property with its sdk line and reason; `make -n -C proofs/cbmc nightly` lists proofs-ack_block; `make -C proofs/cbmc` (the per-commit `all`) prints the same VERIFICATION lines it prints at BASE; `git diff --stat` against BASE shows only the NEW files. Red at BASE: proofs/cbmc/ack_block does not exist at BASE and `make -C proofs/cbmc nightly` does not run proofs-ack_block.

STEP 1. Confirm `uname -s` is Linux; cd into the staged checkout JOB.md names and run git log --oneline -1; run `command -v cbmc` and `cbmc --version`. If `make -n -C proofs/cbmc nightly` fails at BASE, finish HOLD at once: this card needs the first CBMC card (the nightly scaffold).
STEP 2. Red first: show proofs/cbmc/ack_block does not exist at BASE and `make -C proofs/cbmc nightly` does not run proofs-ack_block; quote the line. Run `make -C proofs/cbmc` once and keep its VERIFICATION lines for the DONE-WHEN comparison.
STEP 3. Read the worked example (proofs/cbmc/proof_message_read.c) and each reader in START; write the harnesses and stubs, then proof.mk; run `make -C proofs/cbmc proofs-ack_block`, starting from a small bound and raising it to the full one.
STEP 4. Gate: the DONE-WHEN lines, each with its last line pasted. A failed property is named with its file, and whether it is a defect at BASE or the harness's.
STEP 5. Commit on your own branch as `rocketnet-cbmc-ack-block-read: <what>` with the trailer. Nothing reaches the forge from inside the wall: the member makes the push and pull request from outside the wall when the card finishes, against the BASE: line's branch. The pull request body states the diff stat, the red line, each harness with the bound it proved, and what was not done.
STEP 6. End as JOB.md says; where it asks for RESULT.md write it in JOB.md's shape (head, branch, verdict, gate, output, report).

CBMC PROOF RULES (every CBMC card in this repository).
The claim is no out-of-bounds access and no undefined behaviour on ANY input up to the maximum: packet_bytes symbolic in [0, ROCKETNET_MAX_PACKET_BYTES] (rocketnet.h: 1500 - 20 - 8 = 1472; also ROCKETNET_RECEIVE_CEILING, wire.h), over a buffer object of exactly packet_bytes symbolic bytes, so a read one past the end is a bounds failure and not slack. Where CBMC cannot finish the full bound inside 30 minutes on one bench core, prove the largest bound that does finish, record in results.md the bound proved and the full bound not proved, with the time the full bound ran before you stopped it; never shrink the claim silently.
Include the real sdk .cpp; never copy sdk code into the proof. Stub only what is outside the reader: every libsodium call (crypto_aead_chacha20poly1305_ietf_decrypt, crypto_box_seal_open, crypto_sign_verify_detached, sodium_memzero and kin) is a CBMC model in the harness that checks its own buffer arguments (__CPROVER_r_ok on what it reads, __CPROVER_w_ok on what it writes, with the lengths libsodium documents), writes nondet bytes, and returns a nondet success or failure; the platform, allocator and time calls a reader makes get the same treatment. libsodium itself is not proved.
A failed property is either a real defect (paste the CBMC trace's input, reproduce it with a small program in the job directory, built against the sdk under -fsanitize=address,undefined and never committed, finish HOLD naming the sdk line; never fix sdk code in this card) or a harness limit (name it). An unwinding-assertion failure means the unwind is too small: raise it or bound the loop's input as the code bounds it, and say which.
Never add a target to `all` or to .github/workflows/cbmc.yml: these proofs run nightly only. Use the installed cbmc (CBMC 6.x in /usr/bin on the Linux benches); a bench without `command -v cbmc` is a HOLD at once naming the bench.

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
\"Not done\" is a welcome report; a green claim you did not run is not.
ROUTE NOTE: coordinator 2026-10-18: THE CHECKER IS NOW ESBMC 8.5, NOT CBMC. This NOTE overrides every CBMC, cbmc, goto-cc, __CPROVER_ and \"CBMC 6.x\" statement above, including TOOLCHAIN, THE NIGHTLY LAYOUT, CBMC PROOF RULES and DONE-WHEN wording. Why: CBMC 6.6.0 cannot parse the sdk's C++ (range-for in rocketnet_memory.h, the || conditions in rocketnet_fragment.cpp, __builtin_offsetof static_asserts in rocketnet_wire.h). A trial of ESBMC 8.5.0 on amd-1 proved all three fragment harnesses from rocketnet PR #387 (head b2249e5c) with no sdk change: header_and_plan 59/59, process_fragment 240/240, memory_sizing 19/19. What to do:
ROUTE NOTE: Shim: commit one shim header in the proofs dir (proofs/cbmc/esbmc_shim.h, shared by every reader; if it is already on your base or on the carried head, reuse it, never a second one). It maps __CPROVER_ to __ESBMC_ (assume, assert, nondet and the r_ok/w_ok style checks to their ESBMC forms), and each harness includes it before the sdk .cpp. The sdk is still included, never copied.
ROUTE NOTE: Command per harness: `esbmc` built as C++17 (--std c++17), with --z3 (Bitwuzla ran out of memory on one property), --overflow-check, an explicit --unwind sized to that harness's loops (never a blanket large value) and an explicit memset bound (--memset-limit or the harness's own bound). Keep the harness's pointer, bounds and div-by-zero checks that ESBMC enables by default. Keep the proofs-<reader> target names, the proof.mk layout, results.md and the per-harness output line: `<harness> VERIFICATION SUCCESSFUL|FAILED|unwinding assertion failed`, now printed from ESBMC's result.
ROUTE NOTE: Report each property as proved, refuted (the counterexample checked by hand against the sdk code; a real defect finishes HOLD naming the sdk line, a harness limit is named) or timed out (with the bound and the time). results.md records proved/refuted/timed out per harness with the counts and the flags used.
ROUTE NOTE: Nightly only: the proofs never join `all` or .github/workflows/cbmc.yml. The nightly workflow runs `make -C proofs/cbmc nightly` as before; its job needs esbmc, which the benches now carry.
ROUTE NOTE: Per-commit `all` (the existing CBMC proofs) stays unchanged.
ROUTE NOTE (coordinator, 2026-10-19; LAND, after four held attempts): the scaffold and esbmc_shim.h are on BASE now (reuse them; STEP 1's scaffold HOLD is void). LAND when every harness named has a proof.mk target that ends in a verdict at the largest bound finishing under the per-harness cap (timeout 1800s, prlimit --as=8589934592, nice 19, one esbmc at a time, as connect_token/proof.mk), `make -C proofs/cbmc proofs-<reader>` exits 0, and results.md records per harness the bound proved, the full bound, proved/refuted/timed-out counts, and for any harness below the full bound how long (or how much memory) the full-bound run took before the cap. Split a harness by input range where that lets a larger bound finish; claim the union. Nightly runs only bounds that finish. A reduced bound recorded so is LAND; HOLD only for a real sdk defect (trace, ASan/UBSan repro, sdk line) or no esbmc. One translation unit: include exactly one sdk .cpp; a helper from another sdk .cpp is a harness model (r_ok/w_ok on its buffers at documented lengths, nondet result in its documented range), never a second .cpp (ESBMC merges header statics unsoundly). Benches: Linux amd-1, amd-2, amd-3, hetzner1, hetzner2, hulk, vision (esbmc 8.5 on ~/sdk/bin; run `command -v esbmc` first); never captain, space or the studio. A load-guard deferral is waited out, not a HOLD. Keep what the carried head proved.
ROUTE NOTE (carried head, attempts 3-4): ack_block_read proved at 512/1472 (79 props); writer 77/77, reconstruct 8/8, builder 91/91; apply OOM at run bound 1. Next: raise the partitioned reader from 512; bound apply's ranges as the reader bounds them (an assume citing the reader's check and sdk line).")
    (:id "rocketnet-cbmc-server-packet-readers" :feature "CBMC proves the server packet readers free of out-of-bounds access up to the full packet size (later)" :cells (n - -) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was ready in stream rocketnet-tests. Carried: branch sprint/rocketnet-cbmc-server-packet-readers.w3.g3.e21, no head pushed (attempt 5, ready)"
     :brief "RESULT: rocketnet-cbmc-server-packet-readers sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-tests
DEPENDS-ON: rocketnet-cbmc-packet-prefilter-open
SOURCE: Glenn, 2026-10-18: \"Let's apply it [Frama-C] where it makes sense.\" The coordinator, the same day: rocketnet's SDK is C++ (sdk/*.cpp), so plain Frama-C does not apply; each untrusted-input reader gets a CBMC bounded check for no out-of-bounds access and no undefined behaviour on any input up to the maximum packet size. proofs/cbmc/ already proves small instances (8-byte fragments, 24-byte message payloads, read/write over 8 bytes); these cards take the readers to the full bound. CBMC 6.6.0 is installed in /usr/bin on amd-1, amd-2, amd-3, hetzner1, hetzner2 and vision (measured 2026-10-18; hulk has none). Read at BASE 1fcaea324.
PATHS: none
START: sdk/rocketnet_server_protocol.cpp (read),sdk/rocketnet_server_protocol.h (read),sdk/rocketnet_protocol.h (read),sdk/rocketnet_read_write.h (read),proofs/cbmc/proof_message_read.c (read)
NEW: proofs/cbmc/server_packets/proof_server_packets.c,proofs/cbmc/server_packets/proof.mk,proofs/cbmc/server_packets/results.md
TEST: none because the work is CBMC proofs, not a Go package or a unit-suite case; DONE-WHEN names the make target and the lines it prints
TOOLCHAIN: a Linux member with cbmc 6.x and goto-cc on PATH, make, a C/C++ compiler and python3; run `command -v cbmc` and `cbmc --version` first; libsodium is vendored under sodium/ and is stubbed, not proved
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 150 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 150 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: CBMC with the flags proofs/cbmc/Makefile already uses, as the existing proofs do; nothing new in the build.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with a message layer above it. `make` builds into bin/; `./bin/test` is the unit suite, `./bin/functional` the loopback tier. proofs/cbmc/ holds the CBMC proofs: proofs/cbmc/Makefile (read) runs `make -C proofs/cbmc` (the `all` target, which .github/workflows/cbmc.yml runs on pull requests touching sdk/: that target is per-commit CI and this card never adds to it) with CBMC_FLAGS = --bounds-check --pointer-check --signed-overflow-check --unsigned-overflow-check --div-by-zero-check --unwinding-assertions. A proof is a C file that #includes the sdk .cpp it proves (proof_message_read.c includes rocketnet_message.cpp: the worked example, with its nondet_ and __CPROVER_ fallbacks for a plain compiler) and a harness that drives it on symbolic input. Never mirror sdk code into a proof (proof_fragment_bounds.c copies the reassembly function rather than including it: that copy can drift from the code, which is why rocketnet-cbmc-fragment-reassembly proves the real one).

# CBMC: the server's per-type packet readers read no byte out of bounds on any packet up to the maximum size

THE TASK. rocketnet_server_protocol_process_packet is the server's whole untrusted surface after the prefilter: connection requests, responses and sealed session packets from any address on the internet. Write proofs/cbmc/server_packets/proof_server_packets.c, one harness per reader below, each on symbolic input up to the full bound (CBMC PROOF RULES):
- rocketnet_server_protocol_process_connection_request (a request of any length up to the maximum, every connect-token byte symbolic)
- the static rocketnet_server_process_connection_response and rocketnet_server_process_sealed_packet (reached by including sdk/rocketnet_server_protocol.c (read)pp), each on every packet type byte
- rocketnet_server_protocol_process_packet, the dispatcher (from, packet_data of packet_bytes symbolic bytes, response_packet and out_payload sized as the header documents: ensures no write past them, *response_bytes and *out_payload_bytes inside their buffers)
The server struct is built by the code's own create path with the smallest config it accepts (one client slot); if CBMC cannot finish the dispatcher at the full bound, prove each per-type reader at the full bound and record the dispatcher's largest bound in results.md.
Then proofs/cbmc/server_packets/proof.mk (target proofs-server_packets, appended to NIGHTLY) and proofs/cbmc/server_packets/results.md. Worktree: the job directory; branch: your own; base: sprint/rocketnet-proof; touch only the PATHS and NEW files.

THE NIGHTLY LAYOUT (the first CBMC card creates it; every other CBMC card follows it). proofs/cbmc/Makefile gains, below its existing targets and without changing them or `all`: `NIGHTLY :=`, `-include $(wildcard */proof.mk)`, and a `nightly` target that runs every entry of NIGHTLY. Each reader is its own directory proofs/cbmc/<reader>/: proof_<reader>.c (the harness), proof.mk (appends `proofs-<reader>` to NIGHTLY and defines that target: `$(CBMC) $(CBMC_FLAGS) --unwind <n> -I$(SDK) ...` from proofs/cbmc, SDK as the Makefile already defines it, one line of output per harness `<harness> VERIFICATION SUCCESSFUL|FAILED|unwinding assertion failed`), and results.md (per harness: the properties checked, how many passed, the bounds and unwinding used, and every failed or unproved property with its sdk file and line and its reason). .github/workflows/nightly-proofs.yml (schedule and workflow_dispatch only, never push or pull_request; runs-on [self-hosted, Linux, X64, fleet, long]; timeout-minutes 180) runs `make -C proofs/cbmc nightly`, uploading the output as an artifact. A reader joins the nightly by its proof.mk alone, so CBMC cards never edit a shared file after the first.

DONE-WHEN: on the Linux member, `make -C proofs/cbmc proofs-server_packets` runs every harness of this card to its end (paste the VERIFICATION line of each, and the bound each proved); proofs/cbmc/server_packets/results.md records per harness the properties passed out of total and every failed or unproved property with its sdk line and reason; `make -n -C proofs/cbmc nightly` lists proofs-server_packets; `make -C proofs/cbmc` (the per-commit `all`) prints the same VERIFICATION lines it prints at BASE; `git diff --stat` against BASE shows only the NEW files. Red at BASE: proofs/cbmc/server_packets does not exist at BASE and `make -C proofs/cbmc nightly` does not run proofs-server_packets.

STEP 1. Confirm `uname -s` is Linux; cd into the staged checkout JOB.md names and run git log --oneline -1; run `command -v cbmc` and `cbmc --version`. If `make -n -C proofs/cbmc nightly` fails at BASE, finish HOLD at once: this card needs the first CBMC card (the nightly scaffold).
STEP 2. Red first: show proofs/cbmc/server_packets does not exist at BASE and `make -C proofs/cbmc nightly` does not run proofs-server_packets; quote the line. Run `make -C proofs/cbmc` once and keep its VERIFICATION lines for the DONE-WHEN comparison.
STEP 3. Read the worked example (proofs/cbmc/proof_message_read.c) and each reader in START; write the harnesses and stubs, then proof.mk; run `make -C proofs/cbmc proofs-server_packets`, starting from a small bound and raising it to the full one.
STEP 4. Gate: the DONE-WHEN lines, each with its last line pasted. A failed property is named with its file, and whether it is a defect at BASE or the harness's.
STEP 5. Commit on your own branch as `rocketnet-cbmc-server-packet-readers: <what>` with the trailer. Nothing reaches the forge from inside the wall: the member makes the push and pull request from outside the wall when the card finishes, against the BASE: line's branch. The pull request body states the diff stat, the red line, each harness with the bound it proved, and what was not done.
STEP 6. End as JOB.md says; where it asks for RESULT.md write it in JOB.md's shape (head, branch, verdict, gate, output, report).

CBMC PROOF RULES (every CBMC card in this repository).
The claim is no out-of-bounds access and no undefined behaviour on ANY input up to the maximum: packet_bytes symbolic in [0, ROCKETNET_MAX_PACKET_BYTES] (rocketnet.h: 1500 - 20 - 8 = 1472; also ROCKETNET_RECEIVE_CEILING, wire.h), over a buffer object of exactly packet_bytes symbolic bytes, so a read one past the end is a bounds failure and not slack. Where CBMC cannot finish the full bound inside 30 minutes on one bench core, prove the largest bound that does finish, record in results.md the bound proved and the full bound not proved, with the time the full bound ran before you stopped it; never shrink the claim silently.
Include the real sdk .cpp; never copy sdk code into the proof. Stub only what is outside the reader: every libsodium call (crypto_aead_chacha20poly1305_ietf_decrypt, crypto_box_seal_open, crypto_sign_verify_detached, sodium_memzero and kin) is a CBMC model in the harness that checks its own buffer arguments (__CPROVER_r_ok on what it reads, __CPROVER_w_ok on what it writes, with the lengths libsodium documents), writes nondet bytes, and returns a nondet success or failure; the platform, allocator and time calls a reader makes get the same treatment. libsodium itself is not proved.
A failed property is either a real defect (paste the CBMC trace's input, reproduce it with a small program in the job directory, built against the sdk under -fsanitize=address,undefined and never committed, finish HOLD naming the sdk line; never fix sdk code in this card) or a harness limit (name it). An unwinding-assertion failure means the unwind is too small: raise it or bound the loop's input as the code bounds it, and say which.
Never add a target to `all` or to .github/workflows/cbmc.yml: these proofs run nightly only. Use the installed cbmc (CBMC 6.x in /usr/bin on the Linux benches); a bench without `command -v cbmc` is a HOLD at once naming the bench.

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
\"Not done\" is a welcome report; a green claim you did not run is not.
ROUTE NOTE: coordinator 2026-10-18: THE CHECKER IS NOW ESBMC 8.5, NOT CBMC. This NOTE overrides every CBMC, cbmc, goto-cc, __CPROVER_ and \"CBMC 6.x\" statement above, including TOOLCHAIN, THE NIGHTLY LAYOUT, CBMC PROOF RULES and DONE-WHEN wording. Why: CBMC 6.6.0 cannot parse the sdk's C++ (range-for in rocketnet_memory.h, the || conditions in rocketnet_fragment.cpp, __builtin_offsetof static_asserts in rocketnet_wire.h). A trial of ESBMC 8.5.0 on amd-1 proved all three fragment harnesses from rocketnet PR #387 (head b2249e5c) with no sdk change: header_and_plan 59/59, process_fragment 240/240, memory_sizing 19/19. What to do:
ROUTE NOTE: Shim: commit one shim header in the proofs dir (proofs/cbmc/esbmc_shim.h, shared by every reader; if it is already on your base or on the carried head, reuse it, never a second one). It maps __CPROVER_ to __ESBMC_ (assume, assert, nondet and the r_ok/w_ok style checks to their ESBMC forms), and each harness includes it before the sdk .cpp. The sdk is still included, never copied.
ROUTE NOTE: Command per harness: `esbmc` built as C++17 (--std c++17), with --z3 (Bitwuzla ran out of memory on one property), --overflow-check, an explicit --unwind sized to that harness's loops (never a blanket large value) and an explicit memset bound (--memset-limit or the harness's own bound). Keep the harness's pointer, bounds and div-by-zero checks that ESBMC enables by default. Keep the proofs-<reader> target names, the proof.mk layout, results.md and the per-harness output line: `<harness> VERIFICATION SUCCESSFUL|FAILED|unwinding assertion failed`, now printed from ESBMC's result.
ROUTE NOTE: Report each property as proved, refuted (the counterexample checked by hand against the sdk code; a real defect finishes HOLD naming the sdk line, a harness limit is named) or timed out (with the bound and the time). results.md records proved/refuted/timed out per harness with the counts and the flags used.
ROUTE NOTE: Nightly only: the proofs never join `all` or .github/workflows/cbmc.yml. The nightly workflow runs `make -C proofs/cbmc nightly` as before; its job needs esbmc, which the benches now carry.
ROUTE NOTE: Per-commit `all` (the existing CBMC proofs) stays unchanged.
ROUTE NOTE (coordinator, 2026-10-19; LAND, after four held attempts): the scaffold and esbmc_shim.h are on BASE now (reuse them; STEP 1's scaffold HOLD is void). LAND when every harness named has a proof.mk target that ends in a verdict at the largest bound finishing under the per-harness cap (timeout 1800s, prlimit --as=8589934592, nice 19, one esbmc at a time, as connect_token/proof.mk), `make -C proofs/cbmc proofs-<reader>` exits 0, and results.md records per harness the bound proved, the full bound, proved/refuted/timed-out counts, and for any harness below the full bound how long (or how much memory) the full-bound run took before the cap. Split a harness by input range where that lets a larger bound finish; claim the union. Nightly runs only bounds that finish. A reduced bound recorded so is LAND; HOLD only for a real sdk defect (trace, ASan/UBSan repro, sdk line) or no esbmc. One translation unit: include exactly one sdk .cpp; a helper from another sdk .cpp is a harness model (r_ok/w_ok on its buffers at documented lengths, nondet result in its documented range), never a second .cpp (ESBMC merges header statics unsoundly). Benches: Linux amd-1, amd-2, amd-3, hetzner1, hetzner2, hulk, vision (esbmc 8.5 on ~/sdk/bin; run `command -v esbmc` first); never captain, space or the studio. A load-guard deferral is waited out, not a HOLD. Keep what the carried head proved.
ROUTE NOTE (carried head, attempts 3-4): connection request proved at 1472; response, sealed-session and dispatcher blocked on helpers in other TUs: model them per the one-TU rule, citing the proof covering each helper where one exists.")
    (:id "rocketnet-cbmc-client-packet-readers" :feature "CBMC proves the client packet readers free of out-of-bounds access up to the full packet size (later)" :cells (n - -) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was review in stream rocketnet-tests. Carried: branch sprint/rocketnet-cbmc-client-packet-readers.w6.g1.e21 head de2ec7ee82e58b1d3e13e4baffdf202675c6787b (attempt 7, review)"
     :brief "RESULT: rocketnet-cbmc-client-packet-readers sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-tests
DEPENDS-ON: rocketnet-cbmc-packet-prefilter-open
SOURCE: Glenn, 2026-10-18: \"Let's apply it [Frama-C] where it makes sense.\" The coordinator, the same day: rocketnet's SDK is C++ (sdk/*.cpp), so plain Frama-C does not apply; each untrusted-input reader gets a CBMC bounded check for no out-of-bounds access and no undefined behaviour on any input up to the maximum packet size. proofs/cbmc/ already proves small instances (8-byte fragments, 24-byte message payloads, read/write over 8 bytes); these cards take the readers to the full bound. CBMC 6.6.0 is installed in /usr/bin on amd-1, amd-2, amd-3, hetzner1, hetzner2 and vision (measured 2026-10-18; hulk has none). Read at BASE 1fcaea324.
PATHS: none
START: sdk/rocketnet_client_protocol.cpp (read),sdk/rocketnet_client_protocol.h (read),sdk/rocketnet_client_receive_handshake.h (read),sdk/rocketnet_read_write.h (read),proofs/cbmc/proof_message_read.c (read)
NEW: proofs/cbmc/client_packets/proof_client_packets.c,proofs/cbmc/client_packets/proof.mk,proofs/cbmc/client_packets/results.md
TEST: none because the work is CBMC proofs, not a Go package or a unit-suite case; DONE-WHEN names the make target and the lines it prints
TOOLCHAIN: a Linux member with cbmc 6.x and goto-cc on PATH, make, a C/C++ compiler and python3; run `command -v cbmc` and `cbmc --version` first; libsodium is vendored under sodium/ and is stubbed, not proved
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 150 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 150 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: CBMC with the flags proofs/cbmc/Makefile already uses, as the existing proofs do; nothing new in the build.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with a message layer above it. `make` builds into bin/; `./bin/test` is the unit suite, `./bin/functional` the loopback tier. proofs/cbmc/ holds the CBMC proofs: proofs/cbmc/Makefile (read) runs `make -C proofs/cbmc` (the `all` target, which .github/workflows/cbmc.yml runs on pull requests touching sdk/: that target is per-commit CI and this card never adds to it) with CBMC_FLAGS = --bounds-check --pointer-check --signed-overflow-check --unsigned-overflow-check --div-by-zero-check --unwinding-assertions. A proof is a C file that #includes the sdk .cpp it proves (proof_message_read.c includes rocketnet_message.cpp: the worked example, with its nondet_ and __CPROVER_ fallbacks for a plain compiler) and a harness that drives it on symbolic input. Never mirror sdk code into a proof (proof_fragment_bounds.c copies the reassembly function rather than including it: that copy can drift from the code, which is why rocketnet-cbmc-fragment-reassembly proves the real one).

# CBMC: the client's per-type packet readers read no byte out of bounds on any packet up to the maximum size

THE TASK. A client reads whatever arrives on its socket, including spoofed packets from anyone who knows its address; every per-type reader indexes the packet by its own length fields. Write proofs/cbmc/client_packets/proof_client_packets.c, one harness per reader below, each on symbolic input up to the full bound (CBMC PROOF RULES):
- rocketnet_client_protocol_process_packet, the dispatcher (packet_data of packet_bytes symbolic bytes, out_payload of out_payload_capacity bytes: ensures no read past packet_bytes, no write past out_payload_capacity, *out_payload_bytes in [0, out_payload_capacity])
- the static rocketnet_client_process_cookie, rocketnet_client_process_challenge, rocketnet_client_process_denied, rocketnet_client_open_session_packet, rocketnet_client_process_keep_alive, rocketnet_client_process_payload, rocketnet_client_process_ack, rocketnet_client_process_fragment, rocketnet_client_process_disconnect (reached by including sdk/rocketnet_client_protocol.c (read)pp), each on every client state the struct can hold
- rocketnet_client_protocol_process_record
As the server card: the client struct comes from the code's own create path; if the dispatcher does not finish at the full bound, prove each per-type reader at it and record the dispatcher's largest bound.
Then proofs/cbmc/client_packets/proof.mk (target proofs-client_packets, appended to NIGHTLY) and proofs/cbmc/client_packets/results.md. Worktree: the job directory; branch: your own; base: sprint/rocketnet-proof; touch only the PATHS and NEW files.

THE NIGHTLY LAYOUT (the first CBMC card creates it; every other CBMC card follows it). proofs/cbmc/Makefile gains, below its existing targets and without changing them or `all`: `NIGHTLY :=`, `-include $(wildcard */proof.mk)`, and a `nightly` target that runs every entry of NIGHTLY. Each reader is its own directory proofs/cbmc/<reader>/: proof_<reader>.c (the harness), proof.mk (appends `proofs-<reader>` to NIGHTLY and defines that target: `$(CBMC) $(CBMC_FLAGS) --unwind <n> -I$(SDK) ...` from proofs/cbmc, SDK as the Makefile already defines it, one line of output per harness `<harness> VERIFICATION SUCCESSFUL|FAILED|unwinding assertion failed`), and results.md (per harness: the properties checked, how many passed, the bounds and unwinding used, and every failed or unproved property with its sdk file and line and its reason). .github/workflows/nightly-proofs.yml (schedule and workflow_dispatch only, never push or pull_request; runs-on [self-hosted, Linux, X64, fleet, long]; timeout-minutes 180) runs `make -C proofs/cbmc nightly`, uploading the output as an artifact. A reader joins the nightly by its proof.mk alone, so CBMC cards never edit a shared file after the first.

DONE-WHEN: on the Linux member, `make -C proofs/cbmc proofs-client_packets` runs every harness of this card to its end (paste the VERIFICATION line of each, and the bound each proved); proofs/cbmc/client_packets/results.md records per harness the properties passed out of total and every failed or unproved property with its sdk line and reason; `make -n -C proofs/cbmc nightly` lists proofs-client_packets; `make -C proofs/cbmc` (the per-commit `all`) prints the same VERIFICATION lines it prints at BASE; `git diff --stat` against BASE shows only the NEW files. Red at BASE: proofs/cbmc/client_packets does not exist at BASE and `make -C proofs/cbmc nightly` does not run proofs-client_packets.

STEP 1. Confirm `uname -s` is Linux; cd into the staged checkout JOB.md names and run git log --oneline -1; run `command -v cbmc` and `cbmc --version`. If `make -n -C proofs/cbmc nightly` fails at BASE, finish HOLD at once: this card needs the first CBMC card (the nightly scaffold).
STEP 2. Red first: show proofs/cbmc/client_packets does not exist at BASE and `make -C proofs/cbmc nightly` does not run proofs-client_packets; quote the line. Run `make -C proofs/cbmc` once and keep its VERIFICATION lines for the DONE-WHEN comparison.
STEP 3. Read the worked example (proofs/cbmc/proof_message_read.c) and each reader in START; write the harnesses and stubs, then proof.mk; run `make -C proofs/cbmc proofs-client_packets`, starting from a small bound and raising it to the full one.
STEP 4. Gate: the DONE-WHEN lines, each with its last line pasted. A failed property is named with its file, and whether it is a defect at BASE or the harness's.
STEP 5. Commit on your own branch as `rocketnet-cbmc-client-packet-readers: <what>` with the trailer. Nothing reaches the forge from inside the wall: the member makes the push and pull request from outside the wall when the card finishes, against the BASE: line's branch. The pull request body states the diff stat, the red line, each harness with the bound it proved, and what was not done.
STEP 6. End as JOB.md says; where it asks for RESULT.md write it in JOB.md's shape (head, branch, verdict, gate, output, report).

CBMC PROOF RULES (every CBMC card in this repository).
The claim is no out-of-bounds access and no undefined behaviour on ANY input up to the maximum: packet_bytes symbolic in [0, ROCKETNET_MAX_PACKET_BYTES] (rocketnet.h: 1500 - 20 - 8 = 1472; also ROCKETNET_RECEIVE_CEILING, wire.h), over a buffer object of exactly packet_bytes symbolic bytes, so a read one past the end is a bounds failure and not slack. Where CBMC cannot finish the full bound inside 30 minutes on one bench core, prove the largest bound that does finish, record in results.md the bound proved and the full bound not proved, with the time the full bound ran before you stopped it; never shrink the claim silently.
Include the real sdk .cpp; never copy sdk code into the proof. Stub only what is outside the reader: every libsodium call (crypto_aead_chacha20poly1305_ietf_decrypt, crypto_box_seal_open, crypto_sign_verify_detached, sodium_memzero and kin) is a CBMC model in the harness that checks its own buffer arguments (__CPROVER_r_ok on what it reads, __CPROVER_w_ok on what it writes, with the lengths libsodium documents), writes nondet bytes, and returns a nondet success or failure; the platform, allocator and time calls a reader makes get the same treatment. libsodium itself is not proved.
A failed property is either a real defect (paste the CBMC trace's input, reproduce it with a small program in the job directory, built against the sdk under -fsanitize=address,undefined and never committed, finish HOLD naming the sdk line; never fix sdk code in this card) or a harness limit (name it). An unwinding-assertion failure means the unwind is too small: raise it or bound the loop's input as the code bounds it, and say which.
Never add a target to `all` or to .github/workflows/cbmc.yml: these proofs run nightly only. Use the installed cbmc (CBMC 6.x in /usr/bin on the Linux benches); a bench without `command -v cbmc` is a HOLD at once naming the bench.

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
\"Not done\" is a welcome report; a green claim you did not run is not.
ROUTE NOTE: coordinator 2026-10-18: THE CHECKER IS NOW ESBMC 8.5, NOT CBMC. This NOTE overrides every CBMC, cbmc, goto-cc, __CPROVER_ and \"CBMC 6.x\" statement above, including TOOLCHAIN, THE NIGHTLY LAYOUT, CBMC PROOF RULES and DONE-WHEN wording. Why: CBMC 6.6.0 cannot parse the sdk's C++ (range-for in rocketnet_memory.h, the || conditions in rocketnet_fragment.cpp, __builtin_offsetof static_asserts in rocketnet_wire.h). A trial of ESBMC 8.5.0 on amd-1 proved all three fragment harnesses from rocketnet PR #387 (head b2249e5c) with no sdk change: header_and_plan 59/59, process_fragment 240/240, memory_sizing 19/19. What to do:
ROUTE NOTE: Shim: commit one shim header in the proofs dir (proofs/cbmc/esbmc_shim.h, shared by every reader; if it is already on your base or on the carried head, reuse it, never a second one). It maps __CPROVER_ to __ESBMC_ (assume, assert, nondet and the r_ok/w_ok style checks to their ESBMC forms), and each harness includes it before the sdk .cpp. The sdk is still included, never copied.
ROUTE NOTE: Command per harness: `esbmc` built as C++17 (--std c++17), with --z3 (Bitwuzla ran out of memory on one property), --overflow-check, an explicit --unwind sized to that harness's loops (never a blanket large value) and an explicit memset bound (--memset-limit or the harness's own bound). Keep the harness's pointer, bounds and div-by-zero checks that ESBMC enables by default. Keep the proofs-<reader> target names, the proof.mk layout, results.md and the per-harness output line: `<harness> VERIFICATION SUCCESSFUL|FAILED|unwinding assertion failed`, now printed from ESBMC's result.
ROUTE NOTE: Where: ESBMC 8.5.0 is installed on every Linux bench by fleet ansible (amd-1, amd-2, amd-3, captain, hetzner1, hetzner2, hulk, vision), on the login PATH (~/sdk/bin). Run `command -v esbmc` and `esbmc --version` first; space and the studio are not benches for this.
ROUTE NOTE: Report each property as proved, refuted (the counterexample checked by hand against the sdk code; a real defect finishes HOLD naming the sdk line, a harness limit is named) or timed out (with the bound and the time). results.md records proved/refuted/timed out per harness with the counts and the flags used.
ROUTE NOTE: Nightly only: the proofs never join `all` or .github/workflows/cbmc.yml. The nightly workflow runs `make -C proofs/cbmc nightly` as before; its job needs esbmc, which the benches now carry.
ROUTE NOTE: Per-commit `all` (the existing CBMC proofs) stays unchanged.
ROUTE NOTE: coordinator 2026-10-19: LARGE-STRUCT EXCEPTION for client_packets. ESBMC was measured on this harness in attempts 4 and 5 (Z3 out of memory at 10-16 GB; Bitwuzla and Boolector crash; Yices not built) and cannot check the 307 KB client struct. CBMC parses this harness and proves it, so client_packets uses CBMC (with --external-sat-solver cadical where the harness needs it), overriding the ESBMC notes above for this harness only; every other harness stays on ESBMC. Head 3da641ff (11 harnesses, 4891/4891 properties, nothing refuted or timed out, counts in results.md) is the head to re-finish and read; a reader checks that a planted bug in a reader the harness covers is refuted.")
    (:id "rocketnet-cbmc-sequence-buffers" :feature "CBMC proves the sequence buffers and replay window free of out-of-bounds access (later)" :cells (n - -) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was review in stream rocketnet-tests. Carried: branch sprint/rocketnet-cbmc-sequence-buffers.w1.g1.e21 head 207b82605e720aaf0f1c12c403056f57826c5b17 (attempt 5, review)"
     :brief "RESULT: rocketnet-cbmc-sequence-buffers sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: normal
STREAM: rocketnet-tests
DEPENDS-ON: rocketnet-cbmc-packet-prefilter-open
SOURCE: Glenn, 2026-10-18: \"Let's apply it [Frama-C] where it makes sense.\" The coordinator, the same day: rocketnet's SDK is C++ (sdk/*.cpp), so plain Frama-C does not apply; each untrusted-input reader gets a CBMC bounded check for no out-of-bounds access and no undefined behaviour on any input up to the maximum packet size. proofs/cbmc/ already proves small instances (8-byte fragments, 24-byte message payloads, read/write over 8 bytes); these cards take the readers to the full bound. CBMC 6.6.0 is installed in /usr/bin on amd-1, amd-2, amd-3, hetzner1, hetzner2 and vision (measured 2026-10-18; hulk has none). Read at BASE 1fcaea324.
PATHS: none
START: sdk/rocketnet_replay.cpp (read),sdk/rocketnet_replay.h (read),sdk/rocketnet_ack.cpp (read),sdk/rocketnet_window_receive.cpp (read),sdk/rocketnet_window_receive.h (read),proofs/cbmc/proof_replay_advance.c (read),proofs/cbmc/Makefile (read)
NEW: proofs/cbmc/sequence_buffers/proof_sequence_buffers.c,proofs/cbmc/sequence_buffers/proof.mk,proofs/cbmc/sequence_buffers/results.md
TEST: none because the work is CBMC proofs, not a Go package or a unit-suite case; DONE-WHEN names the make target and the lines it prints
TOOLCHAIN: a Linux member with cbmc 6.x and goto-cc on PATH, make, a C/C++ compiler and python3; run `command -v cbmc` and `cbmc --version` first; libsodium is vendored under sodium/ and is stubbed, not proved
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 150 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 150 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: CBMC with the flags proofs/cbmc/Makefile already uses, as the existing proofs do; nothing new in the build.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with a message layer above it. `make` builds into bin/; `./bin/test` is the unit suite, `./bin/functional` the loopback tier. proofs/cbmc/ holds the CBMC proofs: proofs/cbmc/Makefile (read) runs `make -C proofs/cbmc` (the `all` target, which .github/workflows/cbmc.yml runs on pull requests touching sdk/: that target is per-commit CI and this card never adds to it) with CBMC_FLAGS = --bounds-check --pointer-check --signed-overflow-check --unsigned-overflow-check --div-by-zero-check --unwinding-assertions. A proof is a C file that #includes the sdk .cpp it proves (proof_message_read.c includes rocketnet_message.cpp: the worked example, with its nondet_ and __CPROVER_ fallbacks for a plain compiler) and a harness that drives it on symbolic input. Never mirror sdk code into a proof (proof_fragment_bounds.c copies the reassembly function rather than including it: that copy can drift from the code, which is why rocketnet-cbmc-fragment-reassembly proves the real one).

# CBMC: the replay window and the sequence buffers index in bounds for every sequence

THE TASK. Every received sequence number is attacker-chosen and is reduced to an index into a bit window or a ring; the existing replay proof covers deltas 1 to 3 only, and an index computed from a 64-bit sequence is exactly where a modulo or a shift goes wrong. Write proofs/cbmc/sequence_buffers/proof_sequence_buffers.c, one harness per reader below, each on symbolic input up to the full bound (CBMC PROOF RULES):
- rocketnet_replay_already_received and rocketnet_replay_advance (every sequence and highest_opened: ensures every window index in range; the far-advance branch is a known defect with its own fix card, rocketnet-replay-far-advance-clears-every-window-word: if it is still red at BASE, record it as that known defect, never as new)
- rocketnet_ack_endpoint_record_received, rocketnet_ack_endpoint_record_sent_classified and rocketnet_ack_endpoint_mark_keep_alive (the sent and received rings: every index inside its documented size for every sequence)
- rn_window_receive_init, rn_window_receive_accept and rn_window_receive_release (sdk/rocketnet_window_receive.c (read)pp: every message number, ensures *slot inside the window)
A 64-bit sequence space cannot be unrolled: state the claim per call (one arbitrary sequence against one arbitrary window state the code's invariants allow), as proof_replay_advance.c does with its one arbitrary word.
Then proofs/cbmc/sequence_buffers/proof.mk (target proofs-sequence_buffers, appended to NIGHTLY) and proofs/cbmc/sequence_buffers/results.md. Worktree: the job directory; branch: your own; base: sprint/rocketnet-proof; touch only the PATHS and NEW files.

THE NIGHTLY LAYOUT (the first CBMC card creates it; every other CBMC card follows it). proofs/cbmc/Makefile gains, below its existing targets and without changing them or `all`: `NIGHTLY :=`, `-include $(wildcard */proof.mk)`, and a `nightly` target that runs every entry of NIGHTLY. Each reader is its own directory proofs/cbmc/<reader>/: proof_<reader>.c (the harness), proof.mk (appends `proofs-<reader>` to NIGHTLY and defines that target: `$(CBMC) $(CBMC_FLAGS) --unwind <n> -I$(SDK) ...` from proofs/cbmc, SDK as the Makefile already defines it, one line of output per harness `<harness> VERIFICATION SUCCESSFUL|FAILED|unwinding assertion failed`), and results.md (per harness: the properties checked, how many passed, the bounds and unwinding used, and every failed or unproved property with its sdk file and line and its reason). .github/workflows/nightly-proofs.yml (schedule and workflow_dispatch only, never push or pull_request; runs-on [self-hosted, Linux, X64, fleet, long]; timeout-minutes 180) runs `make -C proofs/cbmc nightly`, uploading the output as an artifact. A reader joins the nightly by its proof.mk alone, so CBMC cards never edit a shared file after the first.

DONE-WHEN: on the Linux member, `make -C proofs/cbmc proofs-sequence_buffers` runs every harness of this card to its end (paste the VERIFICATION line of each, and the bound each proved); proofs/cbmc/sequence_buffers/results.md records per harness the properties passed out of total and every failed or unproved property with its sdk line and reason; `make -n -C proofs/cbmc nightly` lists proofs-sequence_buffers; `make -C proofs/cbmc` (the per-commit `all`) prints the same VERIFICATION lines it prints at BASE; `git diff --stat` against BASE shows only the NEW files. Red at BASE: proofs/cbmc/sequence_buffers does not exist at BASE and `make -C proofs/cbmc nightly` does not run proofs-sequence_buffers.

STEP 1. Confirm `uname -s` is Linux; cd into the staged checkout JOB.md names and run git log --oneline -1; run `command -v cbmc` and `cbmc --version`. If `make -n -C proofs/cbmc nightly` fails at BASE, finish HOLD at once: this card needs the first CBMC card (the nightly scaffold).
STEP 2. Red first: show proofs/cbmc/sequence_buffers does not exist at BASE and `make -C proofs/cbmc nightly` does not run proofs-sequence_buffers; quote the line. Run `make -C proofs/cbmc` once and keep its VERIFICATION lines for the DONE-WHEN comparison.
STEP 3. Read the worked example (proofs/cbmc/proof_message_read.c) and each reader in START; write the harnesses and stubs, then proof.mk; run `make -C proofs/cbmc proofs-sequence_buffers`, starting from a small bound and raising it to the full one.
STEP 4. Gate: the DONE-WHEN lines, each with its last line pasted. A failed property is named with its file, and whether it is a defect at BASE or the harness's.
STEP 5. Commit on your own branch as `rocketnet-cbmc-sequence-buffers: <what>` with the trailer. Nothing reaches the forge from inside the wall: the member makes the push and pull request from outside the wall when the card finishes, against the BASE: line's branch. The pull request body states the diff stat, the red line, each harness with the bound it proved, and what was not done.
STEP 6. End as JOB.md says; where it asks for RESULT.md write it in JOB.md's shape (head, branch, verdict, gate, output, report).

CBMC PROOF RULES (every CBMC card in this repository).
The claim is no out-of-bounds access and no undefined behaviour on ANY input up to the maximum: packet_bytes symbolic in [0, ROCKETNET_MAX_PACKET_BYTES] (rocketnet.h: 1500 - 20 - 8 = 1472; also ROCKETNET_RECEIVE_CEILING, wire.h), over a buffer object of exactly packet_bytes symbolic bytes, so a read one past the end is a bounds failure and not slack. Where CBMC cannot finish the full bound inside 30 minutes on one bench core, prove the largest bound that does finish, record in results.md the bound proved and the full bound not proved, with the time the full bound ran before you stopped it; never shrink the claim silently.
Include the real sdk .cpp; never copy sdk code into the proof. Stub only what is outside the reader: every libsodium call (crypto_aead_chacha20poly1305_ietf_decrypt, crypto_box_seal_open, crypto_sign_verify_detached, sodium_memzero and kin) is a CBMC model in the harness that checks its own buffer arguments (__CPROVER_r_ok on what it reads, __CPROVER_w_ok on what it writes, with the lengths libsodium documents), writes nondet bytes, and returns a nondet success or failure; the platform, allocator and time calls a reader makes get the same treatment. libsodium itself is not proved.
A failed property is either a real defect (paste the CBMC trace's input, reproduce it with a small program in the job directory, built against the sdk under -fsanitize=address,undefined and never committed, finish HOLD naming the sdk line; never fix sdk code in this card) or a harness limit (name it). An unwinding-assertion failure means the unwind is too small: raise it or bound the loop's input as the code bounds it, and say which.
Never add a target to `all` or to .github/workflows/cbmc.yml: these proofs run nightly only. Use the installed cbmc (CBMC 6.x in /usr/bin on the Linux benches); a bench without `command -v cbmc` is a HOLD at once naming the bench.

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
\"Not done\" is a welcome report; a green claim you did not run is not.
ROUTE NOTE: coordinator 2026-10-18: THE CHECKER IS NOW ESBMC 8.5, NOT CBMC. This NOTE overrides every CBMC, cbmc, goto-cc, __CPROVER_ and \"CBMC 6.x\" statement above, including TOOLCHAIN, THE NIGHTLY LAYOUT, CBMC PROOF RULES and DONE-WHEN wording. Why: CBMC 6.6.0 cannot parse the sdk's C++ (range-for in rocketnet_memory.h, the || conditions in rocketnet_fragment.cpp, __builtin_offsetof static_asserts in rocketnet_wire.h). A trial of ESBMC 8.5.0 on amd-1 proved all three fragment harnesses from rocketnet PR #387 (head b2249e5c) with no sdk change: header_and_plan 59/59, process_fragment 240/240, memory_sizing 19/19. What to do:
ROUTE NOTE: Shim: commit one shim header in the proofs dir (proofs/cbmc/esbmc_shim.h, shared by every reader; if it is already on your base or on the carried head, reuse it, never a second one). It maps __CPROVER_ to __ESBMC_ (assume, assert, nondet and the r_ok/w_ok style checks to their ESBMC forms), and each harness includes it before the sdk .cpp. The sdk is still included, never copied.
ROUTE NOTE: Command per harness: `esbmc` built as C++17 (--std c++17), with --z3 (Bitwuzla ran out of memory on one property), --overflow-check, an explicit --unwind sized to that harness's loops (never a blanket large value) and an explicit memset bound (--memset-limit or the harness's own bound). Keep the harness's pointer, bounds and div-by-zero checks that ESBMC enables by default. Keep the proofs-<reader> target names, the proof.mk layout, results.md and the per-harness output line: `<harness> VERIFICATION SUCCESSFUL|FAILED|unwinding assertion failed`, now printed from ESBMC's result.
ROUTE NOTE: Report each property as proved, refuted (the counterexample checked by hand against the sdk code; a real defect finishes HOLD naming the sdk line, a harness limit is named) or timed out (with the bound and the time). results.md records proved/refuted/timed out per harness with the counts and the flags used.
ROUTE NOTE: Nightly only: the proofs never join `all` or .github/workflows/cbmc.yml. The nightly workflow runs `make -C proofs/cbmc nightly` as before; its job needs esbmc, which the benches now carry.
ROUTE NOTE: Per-commit `all` (the existing CBMC proofs) stays unchanged.
ROUTE NOTE (coordinator, 2026-10-19; LAND, after four held attempts): the scaffold and esbmc_shim.h are on BASE now (reuse them; STEP 1's scaffold HOLD is void). LAND when every harness named has a proof.mk target that ends in a verdict at the largest bound finishing under the per-harness cap (timeout 1800s, prlimit --as=8589934592, nice 19, one esbmc at a time, as connect_token/proof.mk), `make -C proofs/cbmc proofs-<reader>` exits 0, and results.md records per harness the bound proved, the full bound, proved/refuted/timed-out counts, and for any harness below the full bound how long (or how much memory) the full-bound run took before the cap. Split a harness by input range where that lets a larger bound finish; claim the union. Nightly runs only bounds that finish. A reduced bound recorded so is LAND; HOLD only for a real sdk defect (trace, ASan/UBSan repro, sdk line) or no esbmc. One translation unit: include exactly one sdk .cpp; a helper from another sdk .cpp is a harness model (r_ok/w_ok on its buffers at documented lengths, nondet result in its documented range), never a second .cpp (ESBMC merges header statics unsoundly). Benches: Linux amd-1, amd-2, amd-3, hetzner1, hetzner2, hulk, vision (esbmc 8.5 on ~/sdk/bin; run `command -v esbmc` first); never captain, space or the studio. A load-guard deferral is waited out, not a HOLD. Keep what the carried head proved.
ROUTE NOTE (carried head, attempt 2): 4 harnesses, 130 props proved; ack received/sent stopped at 300 s (rerun at 1800 s); replay advance/window accept OOM at 8 GiB: split by sequence delta (in window, edge, beyond).")
    (:id "rocketnet-proofs-check-sdk-asserts" :feature "ESBMC proofs: every rocketnet_assert on the proved paths is a checked property (later)" :cells (n - -) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-tests"
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
    (:id "rocketnet-frama-clang-installed" :feature "The proof tools and Frama-Clang prerequisites in place before the Frama-Clang trial (later)" :cells (n - -) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-tests. A sentinel with no brief: it held rocketnet-frama-clang-trial, rocketnet-proofs-check-sdk-asserts and rocketnet-sim-and-reset-tests-bite behind the CBMC proof cards, rocketnet-happy-path, rocketnet-frama-clang-install (mas-bandwidth/rowan-tools) and the DST cards")
    (:id "rocketnet-frama-clang-trial" :feature "Frama-Clang trial: can WP prove rocketnet_ack_block_read from the C++ source? (later)" :cells (n - -) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-tests"
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
    (:id "rocketnet-msan-known-list-matches-every-fleet-toolchain-b" :feature "The MSan known-findings list matches every toolchain the runners use (later)" :cells (n - -) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was review in stream rocketnet-tests. Carried: branch sprint/rocketnet-msan-known-list-matches-every-fleet-toolchain-b.w5.g1.e21 head 338c15f8dc0fd179efe02993281f41660b37c002 (attempt 6, review)"
     :brief "RESULT: rocketnet-msan-known-list-matches-every-fleet-toolchain-b sha=<sha12> tier: pro
CARRY: rocketnet-msan-known-list-matches-every-fleet-toolchain-b attempt 5 head=60a84d7b5b66f7495393d7ecab61732ad50f4340
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: fix
PRIORITY: high
STREAM: rocketnet-tests
DEPENDS-ON: none
SOURCE: the coordinator, 2026-10-18. The MSan leg's verdict depends on which fleet runner takes it: on the recent msan.yml runs the \"Run the MSan gate\" step is green on the amd-3 runner and red on hetzner-org-1, hetzner-org-2 and hetzner2 (runs 39193202067 green; 39193511521, 39193140276, 39192529561, 39192509796, 39192336529, 39187758110 red), and in a bench read at BASE 64da826 the MSan gate script exited 0 on amd-1 and exited 1 on hulk at the same tree. Every red names top frames that are not in tools/msan_known.txt (listed below). This is toolchain drift between benches (clang and the libstdc++ it links), and some of it is line drift in test helpers since the list was written.
PATHS: tools/msan_known.txt,test/resource_file.h,test/pcapng.h,test_unit_counter_classes.cpp,test_unit_connect_token_vectors.cpp,sdk/rocketnet_platform.cpp,tools/run_msan.sh
TEST: none because the property is the MSan gate's verdict on the member's toolchain (the run_msan.sh command msan.yml runs, read only here); a test-helper fix, if any, carries its own red-at-BASE MSan line
TOOLCHAIN: clang with -fsanitize=memory, cmake, make, python3 on the Linux member JOB.md names; libsodium is vendored under sodium/
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none; the gate is tools/run_msan.sh (read) as it stands and the list is a text file.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library. tools/run_msan.sh (read) builds ./bin/test with clang -fsanitize=memory and fails on any MemorySanitizer warning whose top frame is not a line of tools/msan_known.txt (read it whole: each entry is a top frame, with `# origin:` lines and a comment saying why it is not an SDK defect). The suite is linked against the system libstdc++, which is not instrumented, so MSan reports reads of memory that libstdc++ initialised; which frames appear, and at which `include/c++/` line, depends on the clang and gcc versions on the bench.

THE UNLISTED FRAMES SEEN (short form; the run prints them in full):
on the hulk-class toolchain at BASE 64da826:
  rocketnet_test_fopen(...) test/resource_file.h:72:12
  std::replace<... wstring ...> include/c++/bits/stl_algo.h:4382:6
  std::_Rb_tree<string, pair<const string,string>, ...>::_S_left include/c++/bits/stl_tree.h:782:9 and ::_S_right :790:9
  std::_Rb_tree<string, pair<const string,int>, ...>::_S_left include/c++/bits/stl_tree.h:782:9
  rocketnet_pcapng_list(...) test/pcapng.h:457:17
  std::__cxx11::basic_string<char>::append(char const*) (/lib/x86_64-linux-gnu/libstdc++.so.6+0x16a0b8)
  counters_find(char const*) test_unit_counter_classes.cpp:313:14 (the list has :306:14)
  ctv_unhex(...) test_unit_connect_token_vectors.cpp:223:18 (the list has :219:18)
  vectors_regenerate_byte_identical() test_unit_connect_token_vectors.cpp:436:5 (the list has :430:19 and :432:5)
on the CI runners that go red: test/resource_file.h:72, std::replace in stl_algo.h, and _Rb_tree ... stl_tree.h:782.

ROUTE NOTE (coordinator, 2026-10-19; why attempts 2, 3 and 4 held). The brief contradicted itself: on the clang 18 / libstdc++ 13 toolchain the gate prints one unlisted SDK-origin finding, rocketnet_address_parse (bin/librocketnet_dev.so) with origin the stack allocation of `reply` in rocketnet_platform_read_route_table at sdk/rocketnet_platform.cpp:1321, so \"exit 0 at head\" could not hold while the brief also ruled every SDK finding out of scope (yet listed sdk/rocketnet_platform.cpp in PATHS). Ruling: that one SDK finding is in scope. The carried head a3d4dabfdedc fixes it by zeroing the 8 KiB reply buffer before each receive (sdk/rocketnet_platform.cpp:1322); keep it, add a one-line comment saying why (the netlink parse must never read reply bytes the receive did not write), and show the finding at BASE and gone at head. Measured by the coordinator on hetzner2 (Ubuntu clang 18.1.3) at a3d4dabfdedc: the gate prints no `MSan: findings not in tools/msan_known.txt` line (every finding listed, address_parse gone), and exits 1 only on four `MSan: test failed` lines: test_dev_token_minting_absent_in_release signal 4, backend_keypair_from_seed_null_refused signal 5, test_channels_allocator_trap_witness exit 1, dev_connect_token_timeout_is_the_named_constant exit 1. That exit comes from the sibling change already on BASE (6e31c3d, rocketnet-run-msan-fails-on-crashed-test-b: the gate fails on a crashed test even when every warning is known), so DONE-WHEN no longer depends on run_msan.sh's exit alone: run the gate at BASE on the same toolchain and show those same test-failed lines there (pre-existing, not this card's to fix; name each in the report). The head merges cleanly onto sprint/rocketnet-proof 4f24f4d58 (merge-tree clean); its diff is sdk/rocketnet_platform.cpp (+1) and tools/msan_known.txt (+110), both in PATHS. Attempt 4 ran no gate (its load guard deferred); run every gate this time on a Linux bench with clang 18 (hetzner2 or hulk reproduce it; clang 21 benches do not print the SDK frame).

THE TASK. Make the MSan gate's verdict the same on every fleet toolchain, with each listed frame justified by its origin and no SDK defect hidden.
(1) Record the member's toolchain: `clang --version | head -1`, and the libstdc++ clang selects (`clang++ -v -x c++ /dev/null -fsyntax-only 2>&1 | grep 'Selected GCC'`). Run the gate, bash tools/run_msan.sh (read) from the checkout root followed by `echo exit=$?`, at BASE and keep the output.
(2) For every finding the run prints that is not listed, read its origin trace and classify it: NOT A DEFECT when the origin is libstdc++ storage, the suite's own operator new (test_channels.cpp) or another uninstrumented allocation, with no SDK storage (sdk/) on the origin path; REAL when a test helper or the SDK reads memory it never wrote.
(3) NOT A DEFECT: add the frame to tools/msan_known.txt in the existing form (function and location, `include/c++/` for libstdc++ frames, `(bin/test)` for binary frames; a frame in a shared library is written as the run's shortened form prints it), with its `# origin:` lines and one comment naming the clang and libstdc++ major versions that produce it (versions only, never a machine name). A drifted test-helper line (counters_find, ctv_unhex, vectors_regenerate_byte_identical) is updated to the line the head's source has, and the stale line is removed only if no toolchain you ran still prints it.
(4) REAL: fix the read in the test helper (test/resource_file.h, test/pcapng.h, test_unit_counter_classes.cpp or test_unit_connect_token_vectors.cpp) by initialising what it reads, keep the MSan line red at BASE and gone at head. An SDK finding is fixed here only for the one already found (sdk/rocketnet_platform.cpp, the route-table reply buffer; see the ROUTE NOTE); any other SDK finding is reported with its trace as the first line of the report and that part stops as not-done.
(5) Also print, in the report, every entry of tools/msan_known.txt that your toolchain no longer produces, so the coordinator can compare with the other benches; do not delete those (another toolchain may produce them).

STEP 1. Confirm `uname -s` is Linux; cd into the staged checkout JOB.md names and run git log --oneline -1; it is a NEW worktree on the branch this card names. Do (1).
STEP 2. If the member's toolchain prints no unlisted finding at BASE, say so with its versions, still do (3) for the three drifted test-helper lines by reading the head's source (grep -n the named functions; the frame is the line the warning would name), and report Verdict: HOLD with \"not reproduced on this toolchain\" and the versions, so the card can be dealt to a bench that reproduces it.
STEP 3. Do (2), (3) and (4).
STEP 4. Run the gate, bash tools/run_msan.sh (read) then `echo exit=$?`, at head; it prints \"MSan: clean\" or \"(all known)\" and exit=0; keep its last 30 lines. Then `make && ./bin/test` stays green. When a test fails, name its file and say whether that file was changed by your work (yours) or is unchanged (already red at BASE: run the same test on the unchanged base to say so), and report that line first.
STEP 5. Commit on your own branch as `rocketnet-msan-known-list-matches-every-fleet-toolchain-b: <what>` with the trailer. Nothing reaches the forge from inside the wall: in the job the git shim records a push, the pull request is the finish JOB.md names, and the member makes both, against the BASE: line's branch, from outside the wall when the card finishes. The pull request body states the toolchain versions, the triage table (frame, origin, NOT A DEFECT or REAL, action), the gate's exit at BASE and at head, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end and there is nothing else to write, the gate's lines in the pull request body; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

DONE-WHEN: on the member's toolchain, bash tools/run_msan.sh (read) at BASE prints `MSan: findings not in tools/msan_known.txt:` naming the unlisted frames, and at head prints no such line (every finding listed, or `MSan: clean`); the gate's exit is 0 at head, OR it is 1 only because of `MSan: test failed: <name> signal|exit <n>` lines (tests that crash or fail under MSan for a reason other than a finding) and every one of those same lines is also printed at BASE on the same toolchain, all of them listed in the report as pre-existing and not fixed here; every added entry carries its origin and the toolchain versions that produce it; no SDK finding is listed in tools/msan_known.txt; the rocketnet_address_parse finding whose origin is the `reply` buffer of rocketnet_platform_read_route_table is shown at BASE and gone at head; any test-helper fix has its MSan line red at BASE; `make && ./bin/test` is green.

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
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.
ROUTE NOTE (coordinator, 2026-10-19; why attempt 5 held). The source work is done on the carried head 60a84d7b5b66 (it contains a3d4dabfdedc and a clean merge of the base): the route-table reply buffer is zeroed before each receive with the comment saying why, and tools/msan_known.txt carries the classified frames with no SDK finding listed. Attempt 5 ran no gate because it had no Linux job. What is left is measurement, and it is this card's DONE-WHEN unchanged: on a Linux bench with clang 18 and libstdc++ 13 (hetzner2 and hulk reproduce the SDK frame; a clang 21 bench does not), run STEP 1's gate at BASE and STEP 4's gate at the head, then the bounded build and ./bin/test, and report the lines. Change no file unless a gate shows a finding the head does not yet classify. A load-guard deferral is waited out inside the deadline (the gate is one process), not a HOLD; finishing without a gate run is not-done, and a bench whose toolchain is not clang 18 finishes STEP 2's HOLD at once, naming its versions.")))
  (:id "platforms" :title "Platforms"
   :text "Where RocketNet builds and passes. Every push runs the unit and functional suites on Linux, macOS and Windows; the nightly jobs run on Linux, and their macOS and Windows jobs are disabled."
   :columns platforms
   :rows
   ((:feature "Per-commit CI, Debug and Release" :cells (y n n) :evidence "Linux: ci.yml ubuntu-24.04 Debug and Release legs, green in runs 39271096164 and 39286548764; those legs run the unit suite, the functional suite on the auto ladder and with the portable rung pinned, and the lifecycle binary" :note "macOS: the macos-15 / Debug leg's Run functional suite (portable rung pinned by env) step is red in run 39286548764, so the cell is n until it is green. Windows runs Release per push (build-and-test-windows), and its Functional suite with IOCP selected step is red in runs 39271096164 and 39286548764; its Debug row is in nightly.yml and disabled")
    (:feature "Shared export list compared" :cells (y y y) :evidence "Linux, macOS, Windows: the production library boundary step runs tools/check_exports.py in build-and-test and build-and-test-windows; tools/check_exports.py, tools/exports.txt; 095f374 production sources separate from tests, strict endpoint port parsing")
    (:feature "Nightly soak" :cells (y n n) :evidence "Linux: nightly-soak.yml on the Linux runners; nightly-soak.yml; 63a25be macOS; 9ee42a2 Windows" :note "the macOS (63a25be) and Windows (9ee42a2) soak jobs are disabled")
    (:feature "The library is callable from a shared library and a static library, from C and C++" :cells (y y y) :evidence "Linux, macOS, Windows: tools/check_dll_consumer.py runs in build-and-test (Linux and macOS) and build-and-test-windows on every push; its macOS cases landed with eb50adb and its Windows cases with ae06e12" :note "one row for the macOS and Windows items"
     :merged ((:id "rocketnet-dll-callable-tests-macos")
      (:id "rocketnet-dll-callable-tests-windows")))))
  (:id "api" :title "Public API"
   :text "The C surface in sdk/rocketnet.h and what a consumer needs to use it."
   :columns platforms
   :rows
   ((:feature "Connection layer C surface: rocketnet_client_transport_* and rocketnet_server_transport_*" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; ruled #77; sdk/rocketnet.h; packet contexts checked against single-consumption leases: test_lifecycle.cpp check_packet_leases")
    (:feature "Root header pure C: the client and server with their functions, the address type, nothing else" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 5c3027f, dcd941f rocketnet-header-is-clean-c (6b73068); sdk/rocketnet.h plain C99 with one extern C block; rocketnet_test_public_client_server_queries, rocketnet_test_config_refuses_rocketnet_driver; clean at /W4 and -Wextra 9ead512" :note "ruled: netcode-style, real non-mangled symbols for C and FFI; a C++ inside is an implementation privilege")
    (:feature "Message objects in C: rocketnet_message_client_* and rocketnet_message_server_*" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 2c1aa7b")
    (:feature "ABI revision and layout queries" :cells (y y y) :evidence "Linux, macOS, Windows: abi_queries_match_the_compiled_layout and abi_queries_refuse_unknown_names in ./bin/test, which ci.yml runs on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; sdk/rocketnet_abi.cpp" :note "the cross-build mismatch library tools/abi_mismatch.cpp is EXCLUDE_FROM_ALL and no job builds or runs it")
    (:feature "Installable CMake package with C and C++ consumers" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; tools/check_package.py, tools/package_consumer")
    (:feature "Vendored sodium, no system libsodium" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 23d4c40; sodium/")
    (:feature "Structured create errors and FFI-safe logging" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; status codes 4e2d6f1 (status_strings_cover_every_code); create_ex 3e82436, b2bc4ea, 33f84d3 (server_transport_create_ex_reports_every_status, client_transport_create_ex_reports_every_status, lifecycle_ex_reports_every_status); preformatted log callback 194ecc5 (log_callback_preformatted_contract)")
    (:feature "Sized APIs for compatible struct evolution" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; a5941cf; sized_config_accepts_every_known_size, sized_config_refuses_unknown_size; docs/ABI-EVOLUTION.md")
    (:feature "Whole-transport memory estimates and operator budgets (#155)" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; rocketnet_server_required_memory and rocketnet_client_required_memory 1d9f830; the host memory estimate cad7a0c (rocketnet_test_server_host_memory_estimate_bounds_the_plan); operator budgets 227d895 USAGE.md; BUILDING.md Pre-allocation server resource plans" :note "an estimate of what is requested, with its exclusions named (kernel accounting beyond the requests, libbpf and libxdp, guard pages, application memory); not an available-RAM check")
    (:feature "C++ ClientTransport and ServerTransport interfaces" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 925cbd7 cpp_transport_interfaces_map_one_to_one, cpp_transport_fake_implements_the_interface; 8985b00 rocketnet_test_cpp_transports_connect_over_loopback")
    (:feature "Shared library build, checked exports, callable from C and C++" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 73733ad tools/check_header_exports.py; a26fd6c export count derived; a6e3991 tools/check_dll_consumer.py, tools/dll_consumer")
    (:feature "API reference generated by MrDocs" :cells (y - -) :evidence "Linux: a document, built by docs.yml on the Linux runners; baf841d mrdocs.yml, docs/mrdocs, .github/workflows/docs.yml")
    (:id "rocketnet-docs-platform" :feature "API reference comments: platform" :cells (y - -) :evidence "Linux: a document; landed as 2c2fac0")
    (:id "rocketnet-docs-reliability" :feature "API reference comments: reliability" :cells (y - -) :evidence "Linux: a document; landed as 90683f2")
    (:feature "USAGE.md: the client and server API, with worked usage" :cells (y - -) :evidence "Linux: a document; b7f92c4, d0a5339")
    (:id "rocketnet-docs-complete-gate" :feature "The docs job fails on any undocumented public symbol (later)" :cells (n - -) :note "moved out of the sprint 2026-10-19 (Glenn: not for the rocketnet v1.0.0 release); was waiting in stream rocketnet-api"
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

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.")))
  (:id "examples" :title "Examples"
   :text "The example series in examples/, each a small program that builds against the installed library; nightly-examples.yml runs them on Linux, and its macOS and Windows jobs are disabled."
   :columns platforms
   :rows
   ((:feature "The example server runs out of the box" :cells (y y y) :evidence "Linux, macOS, Windows: the happy-path job in ci.yml runs the example server and client on all three (1237a47); 14557ab")
    (:id "rocketnet-example-hello" :feature "hello: the least RocketNet program that works, and the home of the example series" :cells (y n n) :evidence "Linux: landed as 1581cc4, gated on the Linux member")
    (:id "rocketnet-example-client-server" :feature "client + server: two programs over real UDP, with keys, a matchmaker stub and connect tokens" :cells (y n n) :evidence "Linux: landed as f2b109f, gated on the Linux member")
    (:id "rocketnet-example-big-messages" :feature "big_messages: a 200 KB reliable message, and an oversize unreliable send promoted" :cells (y n n) :evidence "Linux: landed as a3722bb, gated on the Linux member")
    (:id "rocketnet-example-stats" :feature "stats: state and counters through the public queries" :cells (y n n) :evidence "Linux: landed as e580649, gated on the Linux member")
    (:id "rocketnet-example-budgets" :feature "budgets: send budgets and over-budget behaviour" :cells (y n n) :evidence "Linux: landed as a068af7, gated on the Linux member")
    (:id "rocketnet-example-config-env" :feature "config_env: default, struct, environment, and a refused bad value" :cells (y n n) :evidence "Linux: landed as 64da826, gated on the Linux member")
    (:id "rocketnet-example-transports" :feature "transports: auto, a pin, and a list, chosen and said out loud" :cells (y n n) :evidence "Linux: landed as e87104d, gated on the Linux member")
    (:id "rocketnet-example-channels" :feature "channels: a stalled chat channel does not block positions" :cells (y n n) :evidence "Linux: landed as 96da6fb, gated on the Linux member")
    (:id "rocketnet-example-ack-receipts" :feature "ack receipts: did my unreliable update arrive?" :cells (y n n) :evidence "Linux: landed as bdef6ee, gated on the Linux member")
    (:id "rocketnet-example-game-loop" :feature "game_loop: a 60 Hz server and N clients, against the 100 microsecond client budget" :cells (y n n) :evidence "Linux: landed as ab83099, gated on the Linux member")))
  (:id "standard" :title "The standard"
   :text "The normative standard in standard/: standard/TRANSPORT.md (the t sections) and standard/MESSAGES.md (the m sections), each section with its conformance vectors, checked by the unit suite and the conformance job."
   :columns platforms
   :rows
   ((:feature "The normative standard in standard/, a conformance vector per section" :cells (y y y) :evidence "Linux, macOS, Windows: ci.yml runs ./bin/test and ./bin/functional on the Linux, macOS and Windows legs on every push, green on all three in run 39271096164; 61c0e67 skeleton and tools/conformance/check_rn_standard.py; one card per section, each with its vectors (standard_wire_primitives_vectors 062e2cb through standard_channels_receipts_security_vectors 263a088); standard/RATIONALE.md 3ba4ff2")
    (:id "rocketnet-standard-connect-token" :feature "Section t6: connect token" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 192db19; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-standard-key-schedule" :feature "Section t7: key schedule" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 1701719; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-standard-packet-formats" :feature "Section t8: packet formats" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 94331b6; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-standard-packet-protection" :feature "Section t9: packet protection" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 1f75abd; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-standard-handshake-client" :feature "Section t11: handshake, the client" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 3c314a9; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-standard-handshake-server" :feature "Section t12: handshake, the server" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 17fb384; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-standard-session-r2" :feature "Section t13: session: keep-alive, timeout, disconnect, slot reset" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 6595114; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-standard-replay" :feature "Section t14: replay protection" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 37885ea; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-standard-receiver-checks" :feature "Section m4: receiver checks" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 601c397; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-standard-blocks" :feature "Section m8: blocks" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 96845d6; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-standard-config-record" :feature "Section m11: the CONFIG control record" :cells (y y y) :evidence "Linux, macOS, Windows: landed as 77dd5c2; its cases run in the unit and functional suites ci.yml runs on all three on every push, green on all three in run 39271096164")
    (:id "rocketnet-standard-vector-index-on-demand" :feature "The standard's vector index printed on demand, never committed" :cells (y - -) :evidence "Linux: a tool that prints a document; landed as a11d924")
    (:id "rocketnet-standard-retire-old" :feature "One standard: the old single-file standard retired into standard/" :cells (y - -) :evidence "Linux: a document; landed as 3ce29a1")))
  (:id "second-deliverable" :title "Second deliverable: keys and backends (later)"
   :text "What customers need to run their own connect-token backend: their own keypairs, the guide, and working example backends in Go and C#. After v1.0.0, by the owner's call."
   :columns platforms
   :rows
   ((:feature "Key generator: a rocketnet_keygen tool, built with the SDK, writing a fresh Ed25519 signing keypair and the token encryption key for production" :cells (n n n) :note "the server takes the public key; the private key stays on the backend; it replaces the published development keys. References: netcode's keygen and yojimbo's (keygen.cpp at the repository root is the starting point)")
    (:feature "Keys documentation: a KEYS.md guide to generating keypairs, where each key lives, rotation, and rocketnet_write_production_connect_token" :cells (n - -) :note "a document. Covers where each key lives (backend, server, and never shipped in the client), rotation, and development keys versus production keys. References: netcode's and yojimbo's key documentation")
    (:feature "Example backends in Go and C#: a minimal HTTP backend in each that authenticates a player (stub), mints a connect token with the production keys for a chosen server address, and returns it for rocketnet_client_connect" :cells (n n n) :note "each comes with a connect token format test vector shared with the C SDK, so tokens minted in Go and C# are accepted by the server. References: the netcode connect token, the yojimbo matcher backend (Go) and the space game backend")))
  (:id "correctness-post-v1" :title "Correctness after v1.0.0 (later)"
   :text "Correctness work that follows the v1.0.0 milestone, by the owner's call."
   :columns platforms
   :rows
   ((:id "server-running-invariants" :feature "TLA+ invariants NoReceiveUnlessRunning and NoAdmitUnlessRunning on the server lifecycle, plus tests that a stopping or stopped server reads no packet and admits no client" :cells (n n n) :note "the query itself ships as rocketnet_server_running in v1.0.0")))
  (:id "after-payment" :title "Hardening and long testing after the payment milestone (later)"
   :text "Soaks, load, stress, matrices, fuzzing, sanitizers, nightlies and the TLA+ ledger, moved off the v1.0.0 sprint on 2026-10-20 by the owner's call: the payment milestone is the public API complete with unit and functional tests green, and this work follows it. Each row keeps its whole brief and its latest head."
   :columns platforms
   :rows
   (
(:id "rocketnet-lan-1000-client-soak-matrix" :feature "A thousand clients for ten minutes on every transport of every platform, over the LAN, every guarantee checked" :cells (n n n) :note "after the payment milestone (owner 2026-10-20); latest work: no pushed branch" :brief "RESULT: rocketnet-lan-1000-client-soak-matrix sha=<sha12> tier: heavy
WHO: only friend rowan
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: code
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: rocketnet-linux-load-1000-clients,rocketnet-macos-load-1000-clients-t,rocketnet-win-load-1000-clients,rocketnet-linux-soak-per-transport,rocketnet-macos-soak-per-transport,rocketnet-win-soak-per-transport,rocketnet-windows-udp-connreset-never-stalls-receive,rocketnet-macos-hard-killed-peer-never-stalls-receive,rocketnet-linux-hard-killed-peer-never-stalls-receive,rocketnet-windows-suite-stable
LAND-AFTER: rocketnet-windows-client-overlapped-send,rocketnet-win-receive-backlog-connect-wait,rocketnet-destroy-sends-disconnect,rocketnet-client-send-drops-until-connected,rocketnet-network-thread-priority-as-space,rocketnet-xdp-drops-first-fragment,rocketnet-api-polish-v1-followups,rocketnet-linux-lifecycle-stress,rocketnet-linux-socket-error-paths,rocketnet-macos-client-timing-under-app-nap,rocketnet-macos-lifecycle-stress-t,rocketnet-macos-sanitizers-and-leaks,rocketnet-macos-socket-errors-and-enobufs-t,rocketnet-macos-universal-and-xcode-t,rocketnet-v1-hygiene-t,rocketnet-win-libfuzzer-clang-cl,rocketnet-win-lifecycle-stress,rocketnet-win-msvc-asan,rocketnet-win-socket-error-paths-t,rocketnet-win-toolchain-matrix
SOURCE: the owner, 2026-10-20: \"I also want to prove out we can actually connect 1000 clients and soak for 10 minutes per-transport and platform we have\", \"this means that we must do that work ON THIS LAN\", \"while we do it, we just hold all fleet machines on the LAN, and you distribute clients around\", \"in this test, we exercise all the different messages and reliability guarantees\", and the per-client load is 256 kbps. This is the v1.0.0 proof that every server transport on every platform holds 1000 real clients over a real network for 10 minutes with every reliability guarantee checked.
PATHS: load.c,tools/lan/**,docs/LOAD.md,CMakeLists.txt
NEW: tools/lan/soak_matrix.py,tools/lan/hosts.tsv,tools/lan/RESULTS.md
START: load.c (read), sdk/rocketnet.h (read), soak.c (read)
TEST: none because the property is real 1000-client runs over the LAN on held machines, a functional-tier run; DONE-WHEN names the runs and the table
DONE-WHEN: (1) load.c gains two modes beside its one-process mode: `load server <bind address> <clients> <seconds>` (one server, the transport from ROCKETNET_SERVER_TRANSPORT) and `load clients <server address> <first client id> <count> <seconds>` (count clients in one process), with tokens minted from the development keys for the server's LAN address; the one-process mode is unchanged; (2) every client sends and receives on all eight channel types (unreliable, unreliable sequenced, reliable, reliable ordered, reliable sequenced, and the receipt variants) for the whole run, small messages and oversize/fragmented messages on each, including oversize unreliable (which promotes), at a steady offered 256 kbps per client, payload plus headers, each direction; (3) every payload carries the sender's client id, channel, a per-channel sequence number and a checksum over the payload, and each end checks at the end of the run, per client and per channel: reliable: every message sent delivered exactly once; reliable ordered: in send order with no gap; both sequenced kinds: never older than one already delivered; unreliable: no duplicate, loss equal to the counters' loss; receipts: every acked receipt matches a delivered message and every lost receipt a message that was not; zero checksum failures; any violation fails the cell; (4) tools/lan/soak_matrix.py runs one cell: it starts the server on its host, waits for it to listen, starts the client processes spread over the client hosts (about 1000/number-of-client-hosts each, ids disjoint) over ssh by LAN address, waits for 1000 connected, holds 10 minutes, stops the clients, then the server, and collects every summary; a cell passes only when 1000 clients are connected from start to end with zero timeouts and zero disconnects, every method flowed for the whole run, and (3) holds; (5) the cell matrix, each run once, each passing: Linux server transports portable, gso (GSO/GRO with sendmmsg/recvmmsg) and io_uring on a LAN Linux host (hulk or vision), and AF_XDP: the dedicated XDP machine is not on this LAN, so the XDP cell runs its server inside a virtme-ng guest on a LAN Linux host with a bridged or tap interface the LAN clients can reach (if no guest can be reached from the LAN, the cell is reported not-run with the measured reason, and the owner decides); Windows server transports portable, iocp and rio on captain; macOS server transports portable and msgx on mini-m5-1 (or the studio if mini-m5-1 is down); the clients for every cell on the other LAN machines (amd-1, amd-2, amd-3, intel, vision, hulk, captain, mini-m5-1, never the cell's own server host), LAN addresses (192.168.1.x) only, never loopback and never a host off this LAN; (6) tools/lan/RESULTS.md holds the results table, one row per cell and method: platform, transport, server host and its NIC link speed, client hosts, clients connected at start and at end, duration, timeouts, disconnects, sent, delivered, lost (unreliable only), out of order (0 where ordered), duplicates (0), checksum failures (0), achieved kbps per client and aggregate, peak server CPU (percent of one core and of the host) and peak server memory, and the head the run built; (7) each host builds the card's head from GitHub itself (Release), never a binary copied from the studio; (8) ./bin/test and ./bin/functional green on Linux and macOS at the head, and load's one-process mode still passes `load 1000 60` on macOS.
TOOLCHAIN: on each held host its platform compiler (gcc or clang, Apple clang, MSVC) and cmake; on the coordinator's machine Python 3 (standard library only) and ssh to the LAN hosts
GATE-NEEDS: darwin, arm64
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 480 minutes.

# A thousand clients for ten minutes on every transport of every platform, over the LAN, every guarantee checked
The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 480 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none new; load.c and soak.c as the load and soak cards leave them, ssh and Python 3's subprocess for the runner.

HELD MACHINES. This card is run by the coordinator's own row, never dealt to the fleet. Before the first cell the coordinator holds every LAN fleet machine at once (`nova-sprint fleet hold amd-1 amd-2 amd-3 intel vision hulk captain mini-m5-1 --reason 'rocketnet LAN 1000-client soak matrix'`), waits until `nova-sprint where` shows each held with nothing running, runs the cells one after another, and unholds them all when the matrix ends or stops (`nova-sprint fleet unhold ...`). No other work runs on a held machine during a cell. Server and clients are always on different machines.

THE TASK. Build the two-mode load tool and the runner, then run the matrix and write the table. The guarantees are checked, not counted: each end keeps per-client, per-channel state for the checks in DONE-WHEN (3). The offered load is steady at 256 kbps per client each way (about 256 Mbps through the server and about 32 Mbps per client host), well inside every LAN link; record each server's NIC speed anyway. A cell that fails is a finding: keep its summaries, profile the server before changing code (a timed stack or perf/sample on the server host), and fix at the cause on this branch only if the fix is in PATHS; otherwise report it with verdict HOLD.

STEP 1. Confirm the platform is macOS and the coordinator's machine; cd into the staged checkout JOB.md names; git log --oneline -1; build; read the START files.
STEP 2. Write the two load modes and the checks; prove each check red once on a planted fault in a short local run (a duplicated reliable message, an out-of-order ordered message, a corrupted payload), kept out of the commit.
STEP 3. Write tools/lan/hosts.tsv (host, LAN address, platform, role) and tools/lan/soak_matrix.py; commit; push the branch so each host can build the head.
STEP 4. Hold the machines; run each cell of DONE-WHEN (5); write tools/lan/RESULTS.md; unhold.
STEP 5. Run DONE-WHEN (8); commit on your own branch as `rocketnet-lan-1000-client-soak-matrix: <what>` with the trailer; end as JOB.md says.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on the coordinator's machine; servers run only on the cell's server host.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy on this machine. Commit on its own branch; touch no other branch; the branch may be pushed so the LAN hosts can build its head; never merge into BASE.
Build and run the cells only on the held LAN hosts; on the coordinator's machine only build and run the unit and functional suites. Never search /, ~ or /Volumes; stay inside the checkout and the job directory (and one scratch directory per host).
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS names; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported. The per-case peak RSS bound in test/suite_runner.h is never changed by a card (the owner's ruling: Windows Debug runs above it are run by hand on a held machine).
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.

BUILD BOUND (the bench sandbox caps a job at 256 processes and 8 GiB). On Linux and macOS build with `(mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. >/dev/null && nice make -j4 --no-print-directory)` (Debug: -DCMAKE_BUILD_TYPE=Debug into build-debug); on Windows `cmake -B build -A x64` then `cmake --build build --config Release --parallel 4`. Run builds and suites one at a time, never in the background.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
CARRY: OWNER PROFILE (2026-10-20): \"I want 1000 players tested and working on all platforms and all transports for v1.0.0. But each client should be light. 10HZ ticks. 1 packet per-update.\" and \"I want the hardcore load testing done after v1.0.0.\" So every 1000-client run here is the light profile: 1000 clients, each updating at 10 Hz and sending 1 packet per update, on every server transport of the platform; pass = all connect, every client receives, none disconnected, reliable in order. No high-rate or bandwidth stress (that is the next milestone). This supersedes any heavier rate or the 256 kbps profile written earlier in this brief.
")
    (:id "rocketnet-linux-transport-matrix-repeat" :feature "Linux: every transport cell fifty times in a row with zero flakes, and the functional suite under ASan per transport" :cells (n - -) :note "after the payment milestone (owner 2026-10-20); latest work: no pushed branch" :brief "REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: code
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: rocketnet-linux-hard-killed-peer-never-stalls-receive
LAND-AFTER: rocketnet-windows-udp-connreset-never-stalls-receive,rocketnet-win-socket-error-paths-t,rocketnet-win-lifecycle-stress,rocketnet-win-msvc-asan,rocketnet-win-app-verifier,rocketnet-win-libfuzzer-clang-cl,rocketnet-win-toolchain-matrix,rocketnet-win-transport-matrix-repeat,rocketnet-win-soak-per-transport,rocketnet-win-load-1000-clients,rocketnet-win-nightlies-on-fleet-runner,rocketnet-linux-socket-error-paths,rocketnet-linux-lifecycle-stress,rocketnet-api-polish-v1-followups,rocketnet-destroy-sends-disconnect,rocketnet-functional-cookie-expiry-uses-test-clock,rocketnet-functional-free-ports,rocketnet-macos-client-timing-under-app-nap,rocketnet-macos-hard-killed-peer-never-stalls-receive,rocketnet-macos-socket-errors-and-enobufs-t,rocketnet-network-thread-priority-as-space,rocketnet-windows-client-overlapped-send,rocketnet-xdp-drops-first-fragment
SOURCE: the owner, 2026-10-19: \"I assume we are already linux hardened, but tell me if not and do that too.\" The audit found Linux CI strong on sanitizers, fuzzing and TSan but with no hard-killed-peer case, no per-transport repeat, soak, load or lifecycle-count proof for portable, gso, io_uring and xdp. ci.yml's transport family matrix runs each Linux cell (portable, gso, io_uring, xdp in a guest) once per commit, and the sanitizers and nightly-leaks jobs run the functional suite under ASan only on the auto ladder (gso), so portable and io_uring never run the whole suite under ASan. This card creates tools/posix/transport_repeat.py, the repeat runner shared with macOS, with the Linux cells; the macOS card rocketnet-macos-transport-matrix-repeat adds the macOS cells to it and lands after this card. This card depends on rocketnet-linux-hard-killed-peer-never-stalls-receive for the posix_hard_killed_client case on Linux.
PATHS: functional/rocketnet_functional_tests.cpp,sdk/rocketnet_platform.cpp,sdk/rocketnet_server_gso_linux.cpp,sdk/rocketnet_server_uring_linux.cpp,sdk/rocketnet_server_transport.cpp,.github/workflows/nightly-leaks.yml,.github/workflows/nightly-soak.yml
NEW: tools/posix/transport_repeat.py
START: .github/workflows/ci.yml (read), functional/posix_hard_killed_peer.cpp (read)
TEST: none because the checks are runs on the member that DONE-WHEN names, each with its count and its red proof
DONE-WHEN: (1) tools/posix/transport_repeat.py (macOS and Linux, Python 3 standard library only) runs each cell's cases by `./bin/functional --run <case>` with the cell's transport pins in the environment, N times (`--runs N`), takes a cell table that the macOS card rocketnet-macos-transport-matrix-repeat extends with its cells after this card lands (one entry per cell: name, env pins, cases, where it runs), prints one line per cell and case `<cell> <case> pass=<p>/<N> p50=<ms> max=<ms>`, and exits non-zero on any fail or skip, naming the run and its last five lines; it carries the Linux cells portable, gso, gso plain, io_uring (when offered) with the portable client, running test_transport_udp_loopback, test_transport_ipv6_loopback, test_transport_dual_stack_ipv4_client, driver_env_pin_is_honoured and posix_hard_killed_client, and the xdp cell (xdp_max_size_both_ways_ipv4 and _ipv6, inside one virtme-ng guest per batch of runs); (2) on this member `--runs 50` prints pass=50/50 on every line (the xdp lines 20/20), the table in the report; (3) the full functional suite runs under ASan+UBSan (the sanitizers job's flags) once pinned to each of portable, gso and io_uring, green, and nightly-leaks.yml runs the functional suite under LSan per pin; (4) each flake or report found is fixed at its cause with the run that showed it, or reported HOLD; (5) .github/workflows/nightly-soak.yml's Linux soak job gains a step running `--runs 20`, proven by hand
TOOLCHAIN: gcc and clang, cmake, make, Python 3 (standard library only), vng (virtme-ng) with the member's kernel image for the xdp cell, on the Linux member JOB.md names
GATE-NEEDS: linux, xdp
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 180 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 180 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: Python 3 standard library

# Linux: every transport cell fifty times in a row with zero flakes, and the functional suite under ASan per transport

THE TASK. Prove each Linux transport cell on IPv4, IPv6, a dual-stack IPv4 client and a hard-killed peer fifty times in a row with zero flakes through the shared repeat runner, run the whole functional suite under ASan and LSan per transport, and run both nightly. Fix each flake at its cause.

STEP 1. Confirm the platform is Linux (`uname -sr` and `nproc` recorded, `cat /proc/sys/kernel/io_uring_disabled` recorded); cd into the staged checkout JOB.md names; `git log --oneline -1`; build as THIS REPOSITORY says and keep the last build line.
STEP 2. Add the Linux cells (DONE-WHEN (1)); a --runs 3 pass.
STEP 3. Run the gate (DONE-WHEN (2), (3)); fix (DONE-WHEN (4)).
STEP 4. Nightly steps proven by hand (DONE-WHEN (3), (5)).
STEP 5. Commit on your own branch as `rocketnet-linux-transport-matrix-repeat: <what>` with the trailer; end as JOB.md says (the pull request body or RESULT.md carries the gate's lines and the run table).

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected transports (DRIVERS.md (read); sdk/rocketnet_drivers.inc (read) is the registry: on Linux the server builds portable, gso (UDP GSO/GRO over sendmmsg/recvmmsg, sdk/rocketnet_server_gso_linux.cpp), io_uring (multishot receive, sdk/rocketnet_server_uring_linux.cpp) and xdp (AF_XDP, sdk/rocketnet_server_socket_linux.cpp), the client portable with the mmsg batch pump; ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT pin one by name). The Linux build is `make` (Release) or `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel 4`; executables land in bin/ (test, functional, `./bin/functional --run <case>`, test_lifecycle, test_driver, soak, soak_sim, load). .github/workflows/ci.yml (read) job build-and-test (the ubuntu-24.04 rows) and its transport family matrix step (TRANSPORT_MATRIX_LINUX) are the worked example of every Linux step, including how the xdp cell runs: as root inside a virtme-ng guest of the member's own kernel (vng, ~/vm-kernels/vmlinuz-$(uname -r), /dev/kvm), never as root on the host. Config fields have an env overlay named ROCKETNET_<TRANSPORT>_<FIELD> (sdk/rocketnet_config.cpp (read)).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.

LINUX BOUND. This card runs on a Linux bench member, never on the coordinator's machine. Run builds and suites one at a time, never in the background, and never two suites at once. Build with `--parallel 4`. Root is used only inside a virtme-ng guest, exactly as ci.yml's xdp cell does; never sudo on the host, never touch the host's interfaces, qdiscs or sysctls. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any check that reads git status. Every run count in DONE-WHEN is a count of real runs on this member, each with its pass or fail and run time in the report; a run that was not made is reported as not made.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, the home directory or any volume root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
FIX (coordinator, 2026-10-20, owner deferral): under the owner's two-minute rule this card does NOT run its long soak, load, stress or repeat runs now. Scope it to what can be proven in short runs: the harness, workflow or code change the card adds, built and run in Debug at the exact head with each run under 2 minutes, a red-first proof where the card fixes a bug, and CHECKLIST.md. Then finish LAND, listing every long run as OWED (they become a separate card after v1.0.0-preview1). If the card has no change beyond the long run itself, finish LAND with only the docs or workflow wiring and the long run OWED. Never shorten a long run and call it a pass. Build on the latest pushed head.
")
    (:id "rocketnet-macos-transport-matrix-repeat" :feature "macOS: every transport cell fifty times in a row, zero flakes, nightly" :cells (- n -) :note "after the payment milestone (owner 2026-10-20); latest work: no pushed branch" :brief "WHO: only friend rowan
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: code
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: rocketnet-macos-hard-killed-peer-never-stalls-receive,rocketnet-linux-transport-matrix-repeat
LAND-AFTER: rocketnet-windows-udp-connreset-never-stalls-receive,rocketnet-win-socket-error-paths-t,rocketnet-win-lifecycle-stress,rocketnet-win-msvc-asan,rocketnet-win-app-verifier,rocketnet-win-libfuzzer-clang-cl,rocketnet-win-toolchain-matrix,rocketnet-win-transport-matrix-repeat,rocketnet-win-soak-per-transport,rocketnet-win-load-1000-clients,rocketnet-win-nightlies-on-fleet-runner,rocketnet-macos-socket-errors-and-enobufs-t,rocketnet-macos-lifecycle-stress-t,rocketnet-macos-sanitizers-and-leaks,rocketnet-macos-universal-and-xcode-t,rocketnet-linux-transport-matrix-repeat,rocketnet-api-polish-v1-followups,rocketnet-destroy-sends-disconnect,rocketnet-functional-cookie-expiry-uses-test-clock,rocketnet-functional-free-ports,rocketnet-linux-hard-killed-peer-never-stalls-receive,rocketnet-linux-lifecycle-stress,rocketnet-linux-socket-error-paths,rocketnet-macos-client-timing-under-app-nap,rocketnet-network-thread-priority-as-space,rocketnet-windows-client-overlapped-send,rocketnet-xdp-drops-first-fragment
SOURCE: the owner, 2026-10-19: \"Do we need to do any macos hardening, to match the windows hardening for rocketnet? Their main programmer on the engine is mac like us.\" The customer's engine programmer builds, runs and debugs on macOS, so the macOS client and server must be proven to the bar the Windows hardening set (stream rocketnet-api, the rocketnet-win-* cards) sets for Windows. The Windows card rocketnet-win-transport-matrix-repeat runs each cell 50 times. On macOS ci.yml's transport family matrix runs each of portable/portable, portable/msgx, msgx/portable and msgx/msgx once per commit; the degraded msgx path (ROCKETNET_PUMP=portable, or a probe refusal) has one case (test_platform_batch_pump_degraded).
PATHS: functional/rocketnet_functional_tests.cpp,sdk/rocketnet_platform.cpp,sdk/rocketnet_server_transport.cpp,sdk/rocketnet_client_transport.cpp,.github/workflows/nightly-soak.yml,.github/workflows/nightly-leaks.yml,sdk/rocketnet_server_gso_linux.cpp,sdk/rocketnet_server_uring_linux.cpp
NEW: tools/posix/transport_repeat.py
START: .github/workflows/ci.yml (read)
TEST: none because the checks are runs on the member that DONE-WHEN names, each with its count and its red proof
DONE-WHEN: (1) tools/posix/transport_repeat.py (created by rocketnet-linux-transport-matrix-repeat with the Linux cells, the CLI and the output line below; this card adds the macOS cells to its cell table and changes nothing else in it) runs, for each macOS cell portable/portable, portable/msgx, msgx/portable, msgx/msgx and msgx-degraded/msgx-degraded (ROCKETNET_PUMP=portable), the cases test_transport_udp_loopback, test_transport_ipv6_loopback, test_transport_dual_stack_ipv4_client, driver_env_pin_is_honoured and posix_hard_killed_client by `./bin/functional --run <case>`, N times (`--runs N`), prints one line per cell and case `<cell> <case> pass=<p>/<N> p50=<ms> max=<ms>`, and exits non-zero on any fail or skip, naming the run and its last five lines; (2) on this member `--runs 50` prints pass=50/50 on every line (25 lines, 1250 runs), the table in the report; (3) the full functional suite runs 10 times pinned msgx/msgx and 10 times degraded, 20 of 20 green, times recorded; (4) each flake found is fixed at its cause with the run that showed it, or reported HOLD; (5) .github/workflows/nightly-soak.yml's soak-macos job gains a step running `--runs 20`, proven by hand
TOOLCHAIN: Apple clang from the Command Line Tools, cmake, make, Python 3 (standard library only), on the fleet's macOS member JOB.md names
GATE-NEEDS: darwin, arm64
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 150 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 150 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: Python 3 standard library (subprocess, time, statistics)

# macOS: every transport cell fifty times in a row, zero flakes, nightly

THE TASK. Prove each macOS transport cell, including the degraded msgx path, on IPv4, IPv6, a dual-stack IPv4 client and a hard-killed peer fifty times in a row with zero flakes, through the repeat runner shared with Linux (created by rocketnet-linux-transport-matrix-repeat) that prints a pass table, and run it nightly. Fix each flake at its cause.

STEP 1. Confirm the platform is macOS on Apple silicon (`uname -s` prints Darwin, `uname -m` prints arm64, `sw_vers -productVersion` and `clang --version` recorded); cd into the staged checkout JOB.md names; `git log --oneline -1`; build as THIS REPOSITORY says and keep the last build line. Then, before any change: if tools/posix/transport_repeat.py is not at your BASE, take the Linux card rocketnet-linux-transport-matrix-repeat's pushed work, which creates it: `git ls-remote origin 'refs/heads/sprint/rocketnet-linux-transport-matrix-repeat.*'`, pick the branch with the highest w then g number, `git fetch origin <that branch>` and `git merge --no-edit FETCH_HEAD`, and name the merged sha in the report; those commits ride with yours and the lander lands the Linux card first (LAND-AFTER). If no such branch exists, finish HOLD naming the missing runner; never write a second runner.
STEP 2. Write the runner (DONE-WHEN (1)); a --runs 3 pass to see it work.
STEP 3. Run the gate (DONE-WHEN (2), (3)); fix flakes (DONE-WHEN (4)).
STEP 4. Add and prove the nightly step (DONE-WHEN (5)).
STEP 5. Commit on your own branch as `rocketnet-macos-transport-matrix-repeat: <what>` with the trailer; end as JOB.md says (the pull request body or RESULT.md carries the gate's lines and the run table).

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected transports (DRIVERS.md (read); sdk/rocketnet_drivers.inc (read) is the registry: on macOS the server builds portable and msgx, the client portable and msgx; msgx is the darwin batch pump over sendmsg_x/recvmsg_x, resolved by dlsym and probed per socket at create (sdk/rocketnet_platform.cpp, the block that opens 'The darwin batch pump'), degrading to the portable per-packet loop on refusal; ROCKETNET_PUMP=portable forces the degraded path; ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT pin a transport by name). The macOS build is `make` (Release) or `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel 4`; executables land in bin/ (test is the unit suite, functional the functional suite, `./bin/functional --run <case>` runs one case, test_msgx, test_lifecycle, test_driver, soak, soak_sim, load, transport_memory). .github/workflows/ci.yml (read) jobs build-and-test (the macos-15 rows on the fleet runner label rocketnet-macos) and its transport family matrix step (TRANSPORT_MATRIX_MACOS) are the worked example of every macOS step. Config fields have an env overlay named ROCKETNET_<TRANSPORT>_<FIELD> (sdk/rocketnet_config.cpp (read), rocketnet_config_env_var_name_for).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.

MACOS BOUND. This card runs on the fleet's macOS member (an Apple-silicon Mac mini; the CI runner label rocketnet-macos is on it), never on the coordinator's machine. Run builds and suites one at a time, never in the background, and never two suites at once. Build with `--parallel 4`. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any check that reads git status. Every run count in DONE-WHEN is a count of real runs on this member, each with its pass or fail and run time in the report; a run that was not made is reported as not made. A macOS firewall or permission prompt that a run raises is a finding to report, never something to click through.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, the home directory or any volume root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
FIX (coordinator, 2026-10-20, owner deferral): under the owner's two-minute rule this card does NOT run its long soak, load, stress or repeat runs now. Scope it to what can be proven in short runs: the harness, workflow or code change the card adds, built and run in Debug at the exact head with each run under 2 minutes, a red-first proof where the card fixes a bug, and CHECKLIST.md. Then finish LAND, listing every long run as OWED (they become a separate card after v1.0.0-preview1). If the card has no change beyond the long run itself, finish LAND with only the docs or workflow wiring and the long run OWED. Never shorten a long run and call it a pass. Build on the latest pushed head.
")
    (:id "rocketnet-win-load-1000-clients" :feature "A thousand clients against a Windows server, on every Windows transport" :cells (- - n) :note "after the payment milestone (owner 2026-10-20); latest work: no pushed branch" :brief "REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: code
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: rocketnet-windows-suite-stable
LAND-AFTER: rocketnet-windows-udp-connreset-never-stalls-receive,rocketnet-win-socket-error-paths-t,rocketnet-win-lifecycle-stress,rocketnet-win-msvc-asan,rocketnet-win-app-verifier,rocketnet-win-libfuzzer-clang-cl,rocketnet-win-toolchain-matrix,rocketnet-win-transport-matrix-repeat,rocketnet-win-soak-per-transport,rocketnet-api-polish-v1-followups,rocketnet-destroy-sends-disconnect,rocketnet-linux-lifecycle-stress,rocketnet-macos-client-timing-under-app-nap,rocketnet-macos-hard-killed-peer-never-stalls-receive,rocketnet-network-thread-priority-as-space,rocketnet-win-nightlies-on-fleet-runner,rocketnet-windows-client-overlapped-send
SOURCE: the owner, 2026-10-19: \"I think we need to do extra testing and checking on windows and windows transports before we deliver rocketnet. It will be their primary platform so we should make it perfect.\" The customer ships on Windows; v1.0.0 is not delivered until Windows is proven. load.c stands up one message server and up to 1000 clients in one process with five pass criteria, and builds on Windows (it already reads the working set through psapi), but no record shows it run at 1000 clients on Windows on any transport.
PATHS: load.c,sdk/rocketnet_platform.cpp,sdk/rocketnet_server_iocp_windows.cpp,sdk/rocketnet_server_rio_windows.cpp,sdk/rocketnet_server_transport.cpp,sdk/rocketnet_client_transport.cpp
START: load.c (read)
TEST: none because the checks are runs on native Windows that DONE-WHEN names, each with its count and its red proof
DONE-WHEN: (1) load.exe's summary line on Windows also prints `handles=<n>` (GetProcessHandleCount) and `threads=<n>` at the end and at the end of the connect phase, and the usage comment at the top of load.c says so; (2) on this member `load.exe 1000 300` passes (verdict=PASS, all five criteria) 3 times each with ROCKETNET_SERVER_TRANSPORT portable, iocp and rio (9 runs), every run's summary line in the report with server_update_p50_us, server_update_p99_us, rss_mb, handles and threads; (3) `load.exe 1000 60 drop-one` still fails naming its criterion on each transport (the harness can still fail); (4) handles and threads at the end equal those after connect within plus or minus 2 on every run; (5) a cause found (a connect that misses the 10 s deadline, a disconnect, a delivery or counter mismatch, a handle climb) is profiled before it is fixed: the report names the call and the evidence, and the fix is minimal in sdk/; a limit that is the machine's (cores, the loopback stack) and not the library's is shown with a smaller count that passes and named, verdict HOLD; (6) `bin/Release/test.exe` and `bin/Release/functional.exe` green
TOOLCHAIN: Visual Studio 18 / MSVC x64, cmake, Windows Performance Recorder (wpr) for profiling when a cause needs it, on the native Windows member JOB.md names
GATE-NEEDS: windows
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 180 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 180 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none new; load.c's own harness, psapi already linked by it.

# A thousand clients against a Windows server, on every Windows transport

THE TASK. Run the existing many-client load harness at its 1000-client maximum against each Windows server transport, three times each, and make every run pass all five criteria with no handle or thread growth. Add the handle and thread counts to its summary line. Profile any failure before fixing it.

AMENDMENT. Only the documentation destination changed: docs/LOAD.md is retired by the v1 docs cleanup (rocketnet-docs-v1-cleanup deletes it), so the text DONE-WHEN asks for goes to the usage comment at the top of load.c instead; write no .md file. Nothing else in this card changed.

STEP 1. Confirm the platform is native Windows (`cmd /c ver` prints a Windows version; under Git Bash `uname -s` prints MINGW or MSYS, never Linux); cd into the staged checkout JOB.md names; `git log --oneline -1`; build Release as THIS REPOSITORY says and keep the last build line.
STEP 2. Add the counts (DONE-WHEN (1)); run `load.exe 50 10` once per transport to see the line.
STEP 3. Run the gate: DONE-WHEN (2), (3), (4) and (6), one run at a time; profile and fix what fails (DONE-WHEN (5)) and rerun the whole count.
STEP 4. Commit on your own branch as `rocketnet-win-load-1000-clients: <what>` with the trailer; end as JOB.md says (the pull request body or RESULT.md carries the gate's lines and the run table).

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected server transports (TRANSPORTS.md, or DRIVERS.md where BASE predates the rename (read); sdk/rocketnet_drivers.inc (read) is the registry: on Windows the server builds portable, iocp and rio, the client portable; ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT pin one by name). The Windows build is `cmake -B build -A x64` then `cmake --build build --config Release --parallel 4`; executables land in bin/Release/ (test.exe is the unit suite, functional.exe the functional suite, `functional.exe --run <case>` runs one case, test_iocp.exe the IOCP driver tests, test_lifecycle.exe, soak.exe, load.exe). .github/workflows/ci.yml (read) job build-and-test-windows is the worked example of every Windows step, including the transport family matrix. Config fields have an env overlay named ROCKETNET_<TRANSPORT>_<FIELD> (sdk/rocketnet_config.cpp (read), rocketnet_config_env_var_name_for).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.

WINDOWS BOUND. This card runs on a native Windows member (captain today, a weak machine; titan, a Xeon workstation, soon). Run builds and suites one at a time, never in the background, and never two suites at once. Build with `--parallel 4`. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any check that reads git status. Every run count in DONE-WHEN is a count of real runs on this member, each with its pass or fail and run time in the report; a run that was not made is reported as not made.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the Windows member JOB.md names, never the coordinator's machine. Never search C:\\, the user profile or any drive root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
CARRY: OWNER PROFILE (2026-10-20): \"I want 1000 players tested and working on all platforms and all transports for v1.0.0. But each client should be light. 10HZ ticks. 1 packet per-update.\" and \"I want the hardcore load testing done after v1.0.0.\" So every 1000-client run here is the light profile: 1000 clients, each updating at 10 Hz and sending 1 packet per update, on every server transport of the platform; pass = all connect, every client receives, none disconnected, reliable in order. No high-rate or bandwidth stress (that is the next milestone). This supersedes any heavier rate or the 256 kbps profile written earlier in this brief.
FIX (coordinator, 2026-10-20, owner deferral): under the owner's two-minute rule this card does NOT run its long soak, load, stress or repeat runs now. Scope it to what can be proven in short runs: the harness, workflow or code change the card adds, built and run in Debug at the exact head with each run under 2 minutes, a red-first proof where the card fixes a bug, and CHECKLIST.md. Then finish LAND, listing every long run as OWED (they become a separate card after v1.0.0-preview1). If the card has no change beyond the long run itself, finish LAND with only the docs or workflow wiring and the long run OWED. Never shorten a long run and call it a pass. Build on the latest pushed head.
")
    (:id "rocketnet-win-soak-per-transport" :feature "Windows soaks per transport under loss, jitter and duplication, with flat handles and memory" :cells (- - n) :note "after the payment milestone (owner 2026-10-20); latest work: no pushed branch" :brief "REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: code
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: rocketnet-windows-suite-stable
LAND-AFTER: rocketnet-windows-udp-connreset-never-stalls-receive,rocketnet-win-socket-error-paths-t,rocketnet-win-lifecycle-stress,rocketnet-win-msvc-asan,rocketnet-win-app-verifier,rocketnet-win-libfuzzer-clang-cl,rocketnet-win-toolchain-matrix,rocketnet-win-transport-matrix-repeat,rocketnet-api-polish-v1-followups,rocketnet-destroy-sends-disconnect,rocketnet-functional-cookie-expiry-uses-test-clock,rocketnet-functional-free-ports,rocketnet-linux-lifecycle-stress,rocketnet-macos-client-timing-under-app-nap,rocketnet-macos-hard-killed-peer-never-stalls-receive,rocketnet-network-thread-priority-as-space,rocketnet-win-nightlies-on-fleet-runner,rocketnet-windows-client-overlapped-send,rocketnet-xdp-drops-first-fragment
SOURCE: the owner, 2026-10-19: \"I think we need to do extra testing and checking on windows and windows transports before we deliver rocketnet. It will be their primary platform so we should make it perfect.\" The customer ships on Windows; v1.0.0 is not delivered until Windows is proven. The Windows soak legs in .github/workflows/nightly-soak.yml are disabled (`if: false`, hosted windows-2025) and soak.c never runs with loss, latency or reordering on Windows. The public link simulator (simulated_loss_percent, simulated_latency_ms, simulated_jitter_ms, simulated_duplicate_percent in sdk/rocketnet.h, development library only, which soak.exe links) loses, delays, jitters (and so reorders) and duplicates outgoing datagrams on any transport. The card rocketnet-soak-seconds-flag (a dependency, landed at BASE before this card starts) gives soak.c `--seconds S`, the counter summary and the non-zero exit on an unplanned disconnect on every OS; this card uses them.
PATHS: soak.c,sdk/rocketnet_platform.cpp,sdk/rocketnet_server_iocp_windows.cpp,sdk/rocketnet_server_rio_windows.cpp,sdk/rocketnet_server_transport.cpp,sdk/rocketnet_client_transport.cpp,functional/rocketnet_functional_tests.cpp
NEW: tools/windows/resource_watch.py,tools/windows/soak_matrix.py
START: soak.c (read), sdk/rocketnet.h (read), sdk/rocketnet_config.cpp (read)
TEST: none because the checks are runs on native Windows that DONE-WHEN names, each with its count and its red proof
DONE-WHEN: (1) tools/windows/resource_watch.py samples a running process by pid every N seconds with ctypes only (GetProcessHandleCount, GetProcessMemoryInfo PrivateUsage and WorkingSetSize, the thread count from CreateToolhelp32Snapshot) into a CSV, and on exit fails when, after a warm-up of the first 10 percent of samples, the least-squares slope of handles, threads or private bytes over the rest exceeds a stated bound (handles and threads: zero growth beyond plus or minus 2; private bytes: under 1 MiB per hour); its own check on a scratch process that leaks one event handle per second turns it red, shown once in the report; (2) `soak.exe --seconds 10` (from rocketnet-soak-seconds-flag, at BASE) runs, prints its counter summary and exits 0 on this member, shown in the report; soak.c changes only to fix a cause a run finds; (3) tools/windows/soak_matrix.py runs soak.exe under the watcher for each of portable, iocp and rio, each pinned by env, with the link simulator set through the env overlay on both ends to loss 5 percent, latency 30 ms, jitter 20 ms (reordering), duplicate 2 percent, and once more per transport with the simulator off; (4) on this member each of the six runs lasts 30 minutes and passes: zero verify failures, zero unplanned disconnects, the conservation the counters allow (sent equals received plus counted drops and simulator losses, stated per run) and a flat watcher verdict; the table of the six runs, their counters and their slopes is in the report; (5) a leak found is fixed at its cause in sdk/ with the run that showed it, or reported with verdict HOLD; (6) the docstring at the top of tools/windows/soak_matrix.py names both scripts and how to read their verdicts; (7) `bin/Release/test.exe` and `bin/Release/functional.exe` green
TOOLCHAIN: Visual Studio 18 / MSVC x64, cmake, Python 3 (standard library and ctypes only) on the native Windows member JOB.md names
GATE-NEEDS: windows
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 240 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 240 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: ctypes over kernel32 and psapi for the samples (psutil is not on the member and is not added); the library's own link simulator for the impairment.

# Windows soaks per transport under loss, jitter and duplication, with flat handles and memory

THE TASK. Soak each Windows server transport for thirty minutes with the link simulator impairing both directions and again clean, and prove the process neither leaks handles, threads nor memory nor drops a connection it did not mean to. Use soak.c's duration and counter summary, and add a ctypes resource watcher with a slope verdict that is shown red on a planted leak.

AMENDMENT. Only the documentation destination changed: docs/LOAD.md is retired by the v1 docs cleanup (rocketnet-docs-v1-cleanup deletes it), so the text DONE-WHEN asks for goes to the docstring at the top of tools/windows/soak_matrix.py instead; write no .md file. Nothing else in this card changed.

STEP 1. Confirm the platform is native Windows (`cmd /c ver` prints a Windows version; under Git Bash `uname -s` prints MINGW or MSYS, never Linux); cd into the staged checkout JOB.md names; `git log --oneline -1`; build Release as THIS REPOSITORY says and keep the last build line.
STEP 2. Write the watcher and prove it red on the scratch leaker (DONE-WHEN (1)); confirm soak.c's duration and summary on Windows (DONE-WHEN (2)).
STEP 3. Write the matrix script (DONE-WHEN (3)); a 2-minute pass per cell first to see it work.
STEP 4. Run the gate: DONE-WHEN (4) and (7), one soak at a time; fix what fails (DONE-WHEN (5)); write DONE-WHEN (6).
STEP 5. Commit on your own branch as `rocketnet-win-soak-per-transport: <what>` with the trailer; end as JOB.md says (the pull request body or RESULT.md carries the gate's lines and the run table).

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected server transports (TRANSPORTS.md, or DRIVERS.md where BASE predates the rename (read); sdk/rocketnet_drivers.inc (read) is the registry: on Windows the server builds portable, iocp and rio, the client portable; ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT pin one by name). The Windows build is `cmake -B build -A x64` then `cmake --build build --config Release --parallel 4`; executables land in bin/Release/ (test.exe is the unit suite, functional.exe the functional suite, `functional.exe --run <case>` runs one case, test_iocp.exe the IOCP driver tests, test_lifecycle.exe, soak.exe, load.exe). .github/workflows/ci.yml (read) job build-and-test-windows is the worked example of every Windows step, including the transport family matrix. Config fields have an env overlay named ROCKETNET_<TRANSPORT>_<FIELD> (sdk/rocketnet_config.cpp (read), rocketnet_config_env_var_name_for).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.

WINDOWS BOUND. This card runs on a native Windows member (captain today, a weak machine; titan, a Xeon workstation, soon). Run builds and suites one at a time, never in the background, and never two suites at once. Build with `--parallel 4`. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any check that reads git status. Every run count in DONE-WHEN is a count of real runs on this member, each with its pass or fail and run time in the report; a run that was not made is reported as not made.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the Windows member JOB.md names, never the coordinator's machine. Never search C:\\, the user profile or any drive root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
FIX (coordinator, 2026-10-20, owner deferral): under the owner's two-minute rule this card does NOT run its long soak, load, stress or repeat runs now. Scope it to what can be proven in short runs: the harness, workflow or code change the card adds, built and run in Debug at the exact head with each run under 2 minutes, a red-first proof where the card fixes a bug, and CHECKLIST.md. Then finish LAND, listing every long run as OWED (they become a separate card after v1.0.0-preview1). If the card has no change beyond the long run itself, finish LAND with only the docs or workflow wiring and the long run OWED. Never shorten a long run and call it a pass. Build on the latest pushed head.
")
    (:id "rocketnet-win-transport-matrix-repeat" :feature "Every Windows transport, every address family, fifty times clean" :cells (- - n) :note "after the payment milestone (owner 2026-10-20); latest work: no pushed branch" :brief "REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: code
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: rocketnet-windows-suite-stable
LAND-AFTER: rocketnet-windows-udp-connreset-never-stalls-receive,rocketnet-win-socket-error-paths-t,rocketnet-win-lifecycle-stress,rocketnet-win-msvc-asan,rocketnet-win-app-verifier,rocketnet-win-libfuzzer-clang-cl,rocketnet-win-toolchain-matrix,rocketnet-api-polish-v1-followups,rocketnet-destroy-sends-disconnect,rocketnet-functional-cookie-expiry-uses-test-clock,rocketnet-functional-free-ports,rocketnet-linux-lifecycle-stress,rocketnet-macos-client-timing-under-app-nap,rocketnet-macos-hard-killed-peer-never-stalls-receive,rocketnet-network-thread-priority-as-space,rocketnet-win-nightlies-on-fleet-runner,rocketnet-windows-client-overlapped-send,rocketnet-xdp-drops-first-fragment
SOURCE: the owner, 2026-10-19: \"I think we need to do extra testing and checking on windows and windows transports before we deliver rocketnet. It will be their primary platform so we should make it perfect.\" The customer ships on Windows; v1.0.0 is not delivered until Windows is proven. rocketnet-transport-family-ci-matrix put each Windows transport (portable, iocp, rio) through IPv4, IPv6 and dual stack once per CI run; once is not proof against flakes, and the IOCP offloads (USO send, URO receive) are probed per host, so a host without them runs a different code path that only test_server_iocp_uso_uro_probe (functional/rocketnet_functional_tests.cpp) touches, by the internal ROCKETNET_IOCP_DISABLE_USO and ROCKETNET_IOCP_DISABLE_URO create flags (sdk/rocketnet_server_iocp.h).
PATHS: sdk/rocketnet_server_iocp_windows.cpp,sdk/rocketnet_server_iocp.h,sdk/rocketnet_server_rio_windows.cpp,sdk/rocketnet_server_rio.h,sdk/rocketnet_server_transport.cpp,sdk/rocketnet_client_transport.cpp,sdk/rocketnet_platform.cpp,functional/rocketnet_functional_tests.cpp,.github/workflows/nightly.yml
NEW: tools/windows/transport_repeat.py
START: sdk/rocketnet_server_iocp_windows.cpp (read 429-545), .github/workflows/ci.yml (read), functional/rocketnet_functional_tests.cpp (read)
TEST: none because the checks are runs on native Windows that DONE-WHEN names, each with its count and its red proof
DONE-WHEN: (1) at BASE, record that no transport-level run pins iocp with USO and URO off: `git grep -n IOCP_DISABLE_USO -- sdk functional` shows the one probe case only; keep the lines; (2) a development-only override forces the IOCP per-datagram path for a whole run: an env value read only when ROCKETNET_DEVELOPMENT is 1 (name it in the report, in the ROCKETNET_<TRANSPORT>_<FIELD> style, for example ROCKETNET_IOCP_OFFLOAD=off) that passes ROCKETNET_IOCP_DISABLE_USO | ROCKETNET_IOCP_DISABLE_URO at create; the production library ignores it (show with the production test that it changes nothing) and sdk/rocketnet.h and tools/exports.txt are unchanged; (3) tools/windows/transport_repeat.py runs, for each cell of portable, iocp (offloads as probed), iocp-offload-off and rio, each pinned by env with the portable client, the cases test_transport_udp_loopback, test_transport_ipv6_loopback, test_transport_dual_stack_ipv4_client and driver_env_pin_is_honoured by `functional.exe --run <case>`, N times (`--runs N`), and prints one line per cell and case `<cell> <case> pass=<p>/<N> p50=<ms> max=<ms>` and exits non-zero on any fail or skip, naming the run and the last five lines of its output; (4) on this member `python tools/windows/transport_repeat.py --runs 50` prints pass=50/50 on every line (16 lines, 800 runs), the table in the report; the per-datagram path is proven taken in the iocp-offload-off cell (the run logs USO 0, URO 0 from the transport counters); (5) the full functional suite runs 10 times with ROCKETNET_SERVER_TRANSPORT=rio and 10 times with iocp and the offload override off, 20 of 20 green, run times recorded; (6) each failure found on the way is fixed at its cause in the files PATHS names, with the run that showed it in the report, or reported with verdict HOLD; (7) .github/workflows/nightly.yml gains a step on [self-hosted, Windows, X64, rocketnet-windows] running `python tools/windows/transport_repeat.py --runs 10`; `python -c \"import yaml\"` permitting, the file parses; (8) `bin/Release/test.exe` and `bin/Release/test_iocp.exe` green
TOOLCHAIN: Visual Studio 18 / MSVC x64, cmake, Python 3 (standard library only) on the native Windows member JOB.md names
GATE-NEEDS: windows
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 150 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 150 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: Python's subprocess and statistics modules for the runner; nothing new in the library.

# Every Windows transport, every address family, fifty times clean

THE TASK. Prove each Windows server transport on IPv4, IPv6 and a dual-stack IPv4 client fifty times in a row with zero flakes, including the IOCP per-datagram path that a host without USO and URO takes. Add a development-only override that runs IOCP with its offloads off, a repeat runner that prints a pass table, and a nightly step that runs it. Fix each flake at its cause.

STEP 1. Confirm the platform is native Windows (`cmd /c ver` prints a Windows version; under Git Bash `uname -s` prints MINGW or MSYS, never Linux); cd into the staged checkout JOB.md names; `git log --oneline -1`; build Release as THIS REPOSITORY says and keep the last build line. Record DONE-WHEN (1).
STEP 2. Add the override (DONE-WHEN (2)) and the runner (DONE-WHEN (3)); run one pass of 5 to see the table.
STEP 3. Run the gate: DONE-WHEN (4), (5) and (8), one suite at a time; fix what fails at its cause (DONE-WHEN (6)) and rerun the whole count.
STEP 4. Add the nightly step (DONE-WHEN (7)).
STEP 5. Commit on your own branch as `rocketnet-win-transport-matrix-repeat: <what>` with the trailer; end as JOB.md says (the pull request body or RESULT.md carries the gate's lines and the run table).

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected server transports (DRIVERS.md (read); sdk/rocketnet_drivers.inc (read) is the registry: on Windows the server builds portable, iocp and rio, the client portable; ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT pin one by name). The Windows build is `cmake -B build -A x64` then `cmake --build build --config Release --parallel 4`; executables land in bin/Release/ (test.exe is the unit suite, functional.exe the functional suite, `functional.exe --run <case>` runs one case, test_iocp.exe the IOCP driver tests, test_lifecycle.exe, soak.exe, load.exe). .github/workflows/ci.yml (read) job build-and-test-windows is the worked example of every Windows step, including the transport family matrix. Config fields have an env overlay named ROCKETNET_<TRANSPORT>_<FIELD> (sdk/rocketnet_config.cpp (read), rocketnet_config_env_var_name_for).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.

WINDOWS BOUND. This card runs on a native Windows member (captain today, a weak machine; titan, a Xeon workstation, soon). Run builds and suites one at a time, never in the background, and never two suites at once. Build with `--parallel 4`. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any check that reads git status. Every run count in DONE-WHEN is a count of real runs on this member, each with its pass or fail and run time in the report; a run that was not made is reported as not made.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the Windows member JOB.md names, never the coordinator's machine. Never search C:\\, the user profile or any drive root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
FIX (coordinator, 2026-10-20, owner deferral): under the owner's two-minute rule this card does NOT run its long soak, load, stress or repeat runs now. Scope it to what can be proven in short runs: the harness, workflow or code change the card adds, built and run in Debug at the exact head with each run under 2 minutes, a red-first proof where the card fixes a bug, and CHECKLIST.md. Then finish LAND, listing every long run as OWED (they become a separate card after v1.0.0-preview1). If the card has no change beyond the long run itself, finish LAND with only the docs or workflow wiring and the long run OWED. Never shorten a long run and call it a pass. Build on the latest pushed head.
")
    (:id "rocketnet-linux-soak-per-transport" :feature "Linux: thirty-minute soaks per transport under impairment with flat fds, threads and memory" :cells (n - -) :note "after the payment milestone (owner 2026-10-20); latest work: sprint/rocketnet-linux-soak-per-transport.w9.g1.e0" :brief "CARRY: rocketnet-linux-soak-per-transport attempt 4 head=9395a2438f18acd9edc0e6486feaae072c0c270f
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: code
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: -
LAND-AFTER: rocketnet-windows-udp-connreset-never-stalls-receive,rocketnet-win-socket-error-paths-t,rocketnet-win-lifecycle-stress,rocketnet-win-msvc-asan,rocketnet-win-app-verifier,rocketnet-win-libfuzzer-clang-cl,rocketnet-win-toolchain-matrix,rocketnet-win-transport-matrix-repeat,rocketnet-win-load-1000-clients,rocketnet-win-nightlies-on-fleet-runner,rocketnet-macos-hard-killed-peer-never-stalls-receive,rocketnet-macos-socket-errors-and-enobufs-t,rocketnet-macos-lifecycle-stress-t,rocketnet-macos-sanitizers-and-leaks,rocketnet-macos-universal-and-xcode-t,rocketnet-macos-transport-matrix-repeat,rocketnet-macos-soak-per-transport,rocketnet-macos-load-1000-clients-t,rocketnet-macos-client-timing-under-app-nap,rocketnet-linux-hard-killed-peer-never-stalls-receive,rocketnet-linux-socket-error-paths,rocketnet-linux-lifecycle-stress,rocketnet-linux-transport-matrix-repeat,rocketnet-api-polish-v1-followups,rocketnet-destroy-sends-disconnect,rocketnet-network-thread-priority-as-space,rocketnet-win-soak-per-transport,rocketnet-windows-client-overlapped-send
SOURCE: the owner, 2026-10-19: \"I assume we are already linux hardened, but tell me if not and do that too.\" The audit found Linux CI strong on sanitizers, fuzzing and TSan but with no hard-killed-peer case, no per-transport repeat, soak, load or lifecycle-count proof for portable, gso, io_uring and xdp. nightly-soak.yml's Linux soak runs `./bin/soak 200000` once on the auto ladder (gso) with no impairment and no resource watch; sanitized-soak runs soak_sim (the in-memory simulator, no sockets) under LSan. No Linux transport is soaked for a fixed time under loss, jitter and duplication. This card depends on rocketnet-soak-seconds-flag for `soak --seconds S` and its counter summary; the Windows soak card is not a dependency.
PATHS: soak.c,sdk/rocketnet_platform.cpp,sdk/rocketnet_server_gso_linux.cpp,sdk/rocketnet_server_uring_linux.cpp,sdk/rocketnet_server_transport.cpp,sdk/rocketnet_client_transport.cpp,sdk/rocketnet_server_socket_linux.cpp,sdk/rocketnet_send_handoff.h,tools/linux/resource_watch.py,tools/linux/soak_matrix.py,sdk/rocketnet_send_handoff.h,test_lifecycle.cpp,tools/macos/soak_matrix.py
NEW: tools/linux/resource_watch.py,tools/linux/soak_matrix.py
START: soak.c (read)
TEST: none because the checks are runs on the member that DONE-WHEN names, each with its count and its red proof
DONE-WHEN: (1) tools/linux/resource_watch.py samples a pid from /proc (fd count, thread count, VmRSS, VmLck) every N seconds into a CSV with a slope verdict: after a warm-up of the first 10 percent of samples it fails when the least-squares slope of fds or threads over the rest exceeds plus or minus 2, or memory grows more than 1 MiB per hour; its check on a scratch process leaking one fd per second turns it red, shown once; (2) tools/linux/soak_matrix.py runs `soak --seconds S` under the watcher for the server cells portable, gso and io_uring with the portable client, the link simulator on both ends at loss 5 percent, latency 30 ms, jitter 20 ms, duplicate 2 percent, and again clean, and a 30-minute xdp soak inside a virtme-ng guest with the watcher inside the guest; (3) on this member each of the seven runs lasts 30 minutes and passes: zero verify failures, zero unplanned disconnects, the counter conservation stated per run and a flat verdict; one further run per cell of 10 minutes under ASan+LSan exits clean; the table in the report; (4) a leak found is fixed at its cause, or reported HOLD; (5) the docstring at the top of tools/linux/soak_matrix.py names both scripts and how to read their verdicts
TOOLCHAIN: gcc and clang, cmake, make, Python 3 (standard library only), vng (virtme-ng) with the member's kernel image for the xdp cell, on the Linux member JOB.md names
GATE-NEEDS: linux, xdp
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 240 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 240 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: /proc only; psutil is not added

# Linux: thirty-minute soaks per transport under impairment with flat fds, threads and memory

THE TASK. Soak each Linux server transport (portable, gso, io_uring, xdp in a guest) for thirty minutes under loss, jitter and duplication and again clean, prove no fd, thread or memory growth and no unplanned disconnect with a /proc resource watcher shown red on a planted leak, and a sanitized ten-minute run per transport.

AMENDMENT. Only the documentation destination changed: docs/LOAD.md is retired by the v1 docs cleanup (rocketnet-docs-v1-cleanup deletes it), so the text DONE-WHEN asks for goes to the docstring at the top of tools/linux/soak_matrix.py instead; write no .md file. Nothing else in this card changed.

STEP 1. Confirm the platform is Linux (`uname -sr` and `nproc` recorded, `cat /proc/sys/kernel/io_uring_disabled` recorded); cd into the staged checkout JOB.md names; `git log --oneline -1`; build as THIS REPOSITORY says and keep the last build line.
STEP 2. Write and prove the watcher (DONE-WHEN (1)); write the matrix script (DONE-WHEN (2)); a 2-minute pass per cell.
STEP 3. Run the soaks one at a time (DONE-WHEN (3)); fix what fails (DONE-WHEN (4)).
STEP 4. Write the docs section (DONE-WHEN (5)).
STEP 5. Commit on your own branch as `rocketnet-linux-soak-per-transport: <what>` with the trailer; end as JOB.md says (the pull request body or RESULT.md carries the gate's lines and the run table).

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected transports (TRANSPORTS.md, or DRIVERS.md where BASE predates the rename (read); sdk/rocketnet_drivers.inc (read) is the registry: on Linux the server builds portable, gso (UDP GSO/GRO over sendmmsg/recvmmsg, sdk/rocketnet_server_gso_linux.cpp), io_uring (multishot receive, sdk/rocketnet_server_uring_linux.cpp) and xdp (AF_XDP, sdk/rocketnet_server_socket_linux.cpp), the client portable with the mmsg batch pump; ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT pin one by name). The Linux build is `make` (Release) or `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel 4`; executables land in bin/ (test, functional, `./bin/functional --run <case>`, test_lifecycle, test_driver, soak, soak_sim, load). .github/workflows/ci.yml (read) job build-and-test (the ubuntu-24.04 rows) and its transport family matrix step (TRANSPORT_MATRIX_LINUX) are the worked example of every Linux step, including how the xdp cell runs: as root inside a virtme-ng guest of the member's own kernel (vng, ~/vm-kernels/vmlinuz-$(uname -r), /dev/kvm), never as root on the host. Config fields have an env overlay named ROCKETNET_<TRANSPORT>_<FIELD> (sdk/rocketnet_config.cpp (read)).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.

LINUX BOUND. This card runs on a Linux bench member, never on the coordinator's machine. Run builds and suites one at a time, never in the background, and never two suites at once. Build with `--parallel 4`. Root is used only inside a virtme-ng guest, exactly as ci.yml's xdp cell does; never sudo on the host, never touch the host's interfaces, qdiscs or sysctls. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any check that reads git status. Every run count in DONE-WHEN is a count of real runs on this member, each with its pass or fail and run time in the report; a run that was not made is reported as not made.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, the home directory or any volume root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
CARRY: head=2c26b9b2de2aa78d5feaea7c8fdf04e46c776571: portable/gso/io_uring soaks 30 min clean x6, sanitizer runs clean, watcher red on the planted leak; xdp guest boots at 3072M with the veth gateway set. One defect left (driver file now in PATHS): under the 30-min impaired soak the xdp server counts SEND_RING_FULL (51 on amd-1, 1 on vision) because both send buffers are handed off and neither has come back (rocketnet_server_socket_linux.cpp:2045-2054). Find why completions are not reaped in time under impairment (reap before claiming a buffer, or size the pool to the TX ring) and fix it so the impaired xdp soak shows zero SEND_RING_FULL; keep fds/threads/memory flat. Then re-run the xdp impaired and clean 30-min soaks.
NOTE: rebalanced to Stella (owner 2026-10-20, Rowan width 8): build on the latest pushed head of this card (the Rowan worker pushes it before stopping); run CHECKLIST.md; two-minute rule.
")
    (:id "rocketnet-load-light-profile-linux" :feature "Linux: 1000 light clients on every transport" :cells (n - -) :note "after the payment milestone (owner 2026-10-20); latest work: 6fdee017 on sprint/rocketnet-load-light-profile-linux.w5.g1.e0" :brief "CARRY: rocketnet-load-light-profile-linux attempt 1 head=e132c94da295fe4cbedc0edf24eb812db9b08681
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: -
SOURCE: TESTING.md (branch testing-md-v1, docs/testing.sexp) lists this cell as no card at sprint/rocketnet-proof 091204c9. Row: 1000 light clients / Linux. The owner, 2026-10-20: 1000 players tested and working on all platforms and all transports for v1.0.0, each client light, 10 Hz ticks, one packet per update; functional testing, not load testing. ./bin/load today runs its clients on a 1 ms loop with max_packets_per_update 4 (load.c ~515, ~729, ~963), which is not that profile.
PATHS: load.c,.github/workflows/nightly-soak.yml,sdk/rocketnet_server_transport.cpp
START: load.c (read the usage comment, the traffic loop and the five criteria)
TEST: none because the gate runs as DONE-WHEN says and has no go or ctest name
DONE-WHEN: (1) load.c gains a light mode (`./bin/load <clients> <seconds> light`): the server and every client update at 10 Hz on the wall clock, each client sends one message per update on a reliable ordered channel, max_packets_per_update is 1, and the server sends one reliable ordered message per client per update; the existing mode is unchanged; (2) its pass criteria are the owner's light profile, functional and not load: 1000 clients, each updating at 10 Hz and sending one packet per update; pass means all 1000 connect, every client receives, nobody is disconnected, and reliable messages arrive in order; build up from a few clients to 1000; no throughput, bandwidth or stress target; the summary line names the mode and the four criteria; (3) red first: `./bin/load 1000 60 light drop-one` (load's existing drop-one fault, client 0 stops processing reliable messages partway, docs/LOAD.md) fails naming its criterion; (4) on this member the light run passes at 10, 100 and 1000 clients, in that order, for each server transport portable, gso and io_uring (client portable), and once at 1000 for xdp inside a virtme-ng guest, the table in the report with connect time and the fd and thread counts at start and end, end equal to start; (5) nightly-soak.yml's Linux job gains one light run per cell, proven by running the step by hand; the usage comment at the top of load.c names the light mode and its criteria (docs/LOAD.md is left as it is)
TOOLCHAIN: gcc and clang, cmake, make, Python 3 (standard library only), on a Linux bench member JOB.md names; vng (virtme-ng) with the member's kernel image for the xdp cell, root only inside the guest
GATE-NEEDS: linux, kvm
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 150 minutes.

# Linux: 1000 light clients on every transport

THE TASK. Give the load harness the owner's light profile (10 Hz, one packet per update), prove it fails when a client stops, and pass 1000 light clients on portable, gso, io_uring and xdp.
STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.


RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, the home directory or any volume root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
NOTE: coordinator 2026-10-20: XDP checks run inside a booted vng guest on any kvm bench (VM KIT: /opt/vm-kernels/{6.8,7.0}/vmlinuz), with the guest uname -r quoted; GATE-NEEDS moved from xdp to kvm. A SKIPPED guest case is not a pass.
")
    (:id "rocketnet-linux-hard-killed-peer-never-stalls-receive" :feature "Linux: a hard-killed peer or an ICMP error never stalls portable, gso, io_uring or xdp" :cells (n - -) :note "after the payment milestone (owner 2026-10-20); latest work: 9bd3c64c on sprint/rocketnet-linux-hard-killed-peer-never-stalls-receive.w9.g1.e0" :brief "REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: code
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: -
LAND-AFTER: rocketnet-windows-udp-connreset-never-stalls-receive,rocketnet-win-socket-error-paths-t,rocketnet-win-lifecycle-stress,rocketnet-win-msvc-asan,rocketnet-win-app-verifier,rocketnet-win-libfuzzer-clang-cl,rocketnet-win-toolchain-matrix,rocketnet-win-transport-matrix-repeat,rocketnet-win-soak-per-transport,rocketnet-win-load-1000-clients,rocketnet-win-nightlies-on-fleet-runner,rocketnet-macos-hard-killed-peer-never-stalls-receive,rocketnet-api-polish-v1-followups,rocketnet-destroy-sends-disconnect,rocketnet-functional-cookie-expiry-uses-test-clock,rocketnet-functional-free-ports,rocketnet-linux-lifecycle-stress,rocketnet-macos-client-timing-under-app-nap,rocketnet-network-thread-priority-as-space,rocketnet-windows-client-overlapped-send,rocketnet-xdp-drops-first-fragment
SOURCE: the owner, 2026-10-19: \"I assume we are already linux hardened, but tell me if not and do that too.\" The audit found Linux CI strong on sanitizers, fuzzing and TSan but with no hard-killed-peer case, no per-transport repeat, soak, load or lifecycle-count proof for portable, gso, io_uring and xdp. On Linux an unconnected UDP socket without IP_RECVERR is not told of ICMP errors, and rocketnet sets no IP_RECVERR and connects no socket (sdk/rocketnet_platform.cpp), so the netcode #113 class should not arise; but no test proves it, and the arms differ: the portable recvfrom loop (sdk/rocketnet_platform.cpp near 580-595) returns 0, read as drained, on any errno but EAGAIN and EINTR; the client mmsg receive (near 935-950) stops its drain on any errno but EINTR, EAGAIN, EWOULDBLOCK; the gso worker (sdk/rocketnet_server_gso_linux.cpp near 450-460) breaks its drain on count <= 0 whatever errno; the io_uring multishot (sdk/rocketnet_server_uring_linux.cpp near 553-585) re-arms on a terminating CQE; the xdp path takes raw frames and passes ICMP to the kernel. The macOS card rocketnet-macos-hard-killed-peer-never-stalls-receive creates functional/posix_hard_killed_peer.cpp and its errno seam and has pushed them; this card is worked in parallel, starts from that pushed work (STEP 1), adds the Linux cells, and lands after it.
PATHS: sdk/rocketnet_platform.cpp,sdk/rocketnet_platform.h,sdk/rocketnet_server_gso_linux.cpp,sdk/rocketnet_server_uring_linux.cpp,sdk/rocketnet_server_socket_linux.cpp,sdk/rocketnet_client_transport.cpp,sdk/rocketnet_server_transport.cpp,functional/rocketnet_functional_tests.cpp,functional/main.cpp,CMakeLists.txt,DRIVERS.md,TRANSPORTS.md,sdk/rocketnet_server_uring.h,functional/posix_hard_killed_peer.cpp
NEW: functional/posix_hard_killed_peer.cpp,TRANSPORTS.md
START: sdk/rocketnet_platform.cpp (read 560-610 and 855-975), sdk/rocketnet_server_gso_linux.cpp (read 400-480), sdk/rocketnet_server_uring_linux.cpp (read 540-640), functional/posix_hard_killed_peer.cpp (read)
TEST: none because the checks are runs on the member that DONE-WHEN names, each with its count and its red proof
DONE-WHEN: (1) AUDIT FIRST, in the report: every socket rocketnet creates on Linux (portable server and client, gso queues, io_uring queues, the xdp transport's kernel sockets; IPv4 and IPv6) with file:line showing it is never connect()ed and never sets IP_RECVERR/IPV6_RECVERR, and every receive and send arm (recvfrom, recvmmsg in the client pump and the gso worker, the io_uring recv CQE, the xdp rx ring, sendto, sendmmsg, the gso flush, io_uring send CQEs) with file:line of how it handles ECONNREFUSED, EHOSTUNREACH, ENETUNREACH, ENOBUFS, EAGAIN, EINTR and a pending SO_ERROR; (2) at head every receive arm treats those as skip-and-continue, never the end of a drain while datagrams remain and never fatal; each fix cites its audit line; (3) posix_hard_killed_client, posix_send_to_closed_port and posix_client_server_gone run on Linux for the server cells portable, gso, gso with GSO and GRO off (the plain sendmmsg/recvmmsg tier, as test_server_gso_wire_plain selects it), io_uring (when /proc/sys/kernel/io_uring_disabled is 0, else reported not run with the value) and xdp (inside a virtme-ng guest, server xdp with the portable client, as ci.yml's xdp cell runs), IPv4 and IPv6, with the same bounds as the macOS cells; (4) the injected-errno seam of the macOS card is extended to recvfrom, recvmmsg and the io_uring recv CQE res, and cases prove each errno is skipped and the drain continues; (5) the red proof: a scratch commit (not kept) that sets IP_RECVERR on the portable server socket turns posix_hard_killed_client red (or, if the kernel queues the error without surfacing it to recvfrom, the report says so with the errno trace), and one that ends the gso worker on the first errno turns a seam case red; each shown once; (6) the cases pass 20 runs in a row on this member per cell (the xdp cell 5 runs, it boots a guest each time), the run table in the report; (7) `./bin/test`, `./bin/functional` and the ci.yml transport family matrix cases green at head; the transports document's 'ICMP errors' paragraph covers Linux
TOOLCHAIN: gcc and clang, cmake, make, Python 3 (standard library only), vng (virtme-ng) with the member's kernel image for the xdp cell, on the Linux member JOB.md names
GATE-NEEDS: linux, kvm
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 180 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 180 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: liburing as already linked; posix_spawn for the child client, ended by the test itself on its own pid; no new library

# Linux: a hard-killed peer or an ICMP error never stalls portable, gso, io_uring or xdp

THE TASK. Audit every Linux socket and receive arm for the ICMP error class and buffer errnos, make each skip-and-continue, and prove it per transport (portable, gso and its plain tier, io_uring, xdp in a guest) with a SIGKILLed client process, sends to a closed port and an injected-errno seam, red against planted regressions.

AMENDMENT. Only the documentation destination changed: DRIVERS.md is renamed TRANSPORTS.md by the v1 docs cleanup. Edit the transports document that exists at BASE (TRANSPORTS.md, or DRIVERS.md where BASE predates the rename), never both and never create the other. Nothing else in this card changed.

STEP 1. Confirm the platform is Linux (`uname -sr` and `nproc` recorded, `cat /proc/sys/kernel/io_uring_disabled` recorded); cd into the staged checkout JOB.md names; `git log --oneline -1`; build as THIS REPOSITORY says and keep the last build line. Then, before any change: if functional/posix_hard_killed_peer.cpp is not at your BASE, take the macOS card rocketnet-macos-hard-killed-peer-never-stalls-receive's pushed work, which creates it: `git ls-remote origin 'refs/heads/sprint/rocketnet-macos-hard-killed-peer-never-stalls-receive.*'`, pick the branch with the highest w then g number (at card time sprint/rocketnet-macos-hard-killed-peer-never-stalls-receive.w1.g1.e21 at 2ae31cbda5d0), `git fetch origin <that branch>` and `git merge --no-edit FETCH_HEAD`; name the merged sha in the report. Those commits ride with yours, the lander lands the macOS card first (LAND-AFTER), and you change its cases only to add the Linux cells; a defect you find in them is a finding for the report. If no such branch exists, finish HOLD naming the missing file.
STEP 2. Write the audit table (DONE-WHEN (1)) before changing code.
STEP 3. Add the Linux cells and the seam arms (DONE-WHEN (3), (4)); run at BASE; fix each gap (DONE-WHEN (2)).
STEP 4. Prove red (DONE-WHEN (5)) and drop the scratch commits.
STEP 5. Run the gate (DONE-WHEN (6), (7)), one suite at a time.
STEP 6. Commit on your own branch as `rocketnet-linux-hard-killed-peer-never-stalls-receive: <what>` with the trailer; end as JOB.md says (the pull request body or RESULT.md carries the gate's lines and the run table).

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected transports (TRANSPORTS.md, or DRIVERS.md where BASE predates the rename (read); sdk/rocketnet_drivers.inc (read) is the registry: on Linux the server builds portable, gso (UDP GSO/GRO over sendmmsg/recvmmsg, sdk/rocketnet_server_gso_linux.cpp), io_uring (multishot receive, sdk/rocketnet_server_uring_linux.cpp) and xdp (AF_XDP, sdk/rocketnet_server_socket_linux.cpp), the client portable with the mmsg batch pump; ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT pin one by name). The Linux build is `make` (Release) or `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel 4`; executables land in bin/ (test, functional, `./bin/functional --run <case>`, test_lifecycle, test_driver, soak, soak_sim, load). .github/workflows/ci.yml (read) job build-and-test (the ubuntu-24.04 rows) and its transport family matrix step (TRANSPORT_MATRIX_LINUX) are the worked example of every Linux step, including how the xdp cell runs: as root inside a virtme-ng guest of the member's own kernel (vng, ~/vm-kernels/vmlinuz-$(uname -r), /dev/kvm), never as root on the host. Config fields have an env overlay named ROCKETNET_<TRANSPORT>_<FIELD> (sdk/rocketnet_config.cpp (read)).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.

LINUX BOUND. This card runs on a Linux bench member, never on the coordinator's machine. Run builds and suites one at a time, never in the background, and never two suites at once. Build with `--parallel 4`. Root is used only inside a virtme-ng guest, exactly as ci.yml's xdp cell does; never sudo on the host, never touch the host's interfaces, qdiscs or sysctls. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any check that reads git status. Every run count in DONE-WHEN is a count of real runs on this member, each with its pass or fail and run time in the report; a run that was not made is reported as not made.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, the home directory or any volume root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
CARRY: Coordinator 2026-10-20 13:05Z: brief confirmed current for the v1.0 push; the newest CARRY above names exactly what is owed. Continue from the last pushed head merged with the current base.
CARRY: VM AUDIT 2026-10-20: the hetzner1 reader booted a vng guest at head 9bc92abd and found xdp_hard_killed_client_ipv4 RED: '0 of 2 live clients connected', contradicting the worker's '2/2 rc0' claim. Reproduce inside a booted guest (run vng from a short dir such as /tmp/rn-<short> so the virtiofs socket path is under 108 bytes; print the guest uname -r as boot proof), fix at the cause, and show the xdp cells green in a booted guest with that proof quoted. Any xdp claim without guest boot proof does not count.
NOTE: coordinator 2026-10-20: XDP checks run inside a booted vng guest on any kvm bench (VM KIT: /opt/vm-kernels/{6.8,7.0}/vmlinuz), with the guest uname -r quoted; GATE-NEEDS moved from xdp to kvm. A SKIPPED guest case is not a pass.
NOTE: fix routed to friends (owner 2026-10-20); carry the latest pushed head and FIX lines; run CHECKLIST.md; two-minute rule.
")
    (:id "rocketnet-macos-load-1000-clients-t" :feature "macOS: 1000 clients per transport, three runs each, with the fd limit and counts in the summary" :cells (- n -) :note "after the payment milestone (owner 2026-10-20); latest work: 6dd8067c on sprint/rocketnet-macos-load-1000-clients-t.w10.g1.e0" :brief "WHO: only friend rowan
CARRY: rocketnet-macos-load-1000-clients-t attempt 6 head=7c5994ed7d0fb1775b9942f58b4bd829205eac32
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: code
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: -
LAND-AFTER: rocketnet-windows-udp-connreset-never-stalls-receive,rocketnet-win-socket-error-paths-t,rocketnet-win-lifecycle-stress,rocketnet-win-msvc-asan,rocketnet-win-app-verifier,rocketnet-win-libfuzzer-clang-cl,rocketnet-win-toolchain-matrix,rocketnet-win-transport-matrix-repeat,rocketnet-win-soak-per-transport,rocketnet-win-load-1000-clients,rocketnet-win-nightlies-on-fleet-runner,rocketnet-macos-hard-killed-peer-never-stalls-receive,rocketnet-macos-socket-errors-and-enobufs-t,rocketnet-macos-lifecycle-stress-t,rocketnet-macos-sanitizers-and-leaks,rocketnet-macos-universal-and-xcode-t,rocketnet-macos-transport-matrix-repeat,rocketnet-macos-soak-per-transport,rocketnet-api-polish-v1-followups,rocketnet-destroy-sends-disconnect,rocketnet-linux-hard-killed-peer-never-stalls-receive,rocketnet-linux-lifecycle-stress,rocketnet-linux-socket-error-paths,rocketnet-linux-transport-matrix-repeat,rocketnet-macos-client-timing-under-app-nap,rocketnet-network-thread-priority-as-space,rocketnet-windows-client-overlapped-send
SOURCE: the owner, 2026-10-19: \"Do we need to do any macos hardening, to match the windows hardening for rocketnet? Their main programmer on the engine is mac like us.\" The customer's engine programmer builds, runs and debugs on macOS, so the macOS client and server must be proven to the bar the Windows hardening set (stream rocketnet-api, the rocketnet-win-* cards) sets for Windows. The Windows card rocketnet-win-load-1000-clients runs load.c at 1000 clients. load.c (rocketnet-load-many-clients-harness) raises RLIMIT_NOFILE itself but never runs in CI or a nightly on any platform; on the fleet Mac `ulimit -n` is 256 and `launchctl limit maxfiles` is 256 soft (checked 2026-10-19), so a game that opens 1000 sockets or a dev who runs a local 1000-client test meets the limit.
PATHS: load.c,docs/LOAD.md,sdk/rocketnet_platform.cpp,sdk/rocketnet_server_transport.cpp,sdk/rocketnet_client_transport.cpp,.github/workflows/nightly-soak.yml,sdk/rocketnet_client_io.h,docs/CLIENT-IO.md,tla/RNClientIo.tla,tla/MCRNClientIo.cfg,tla/RUNS.tsv,tla/CASES.tsv,functional/macos_client_timing.cpp
START: load.c (read), docs/LOAD.md (read)
TEST: none because the checks are runs on the member that DONE-WHEN names, each with its count and its red proof
DONE-WHEN: (1) load's summary line adds the fd count and thread count at start, peak and end, and the RLIMIT_NOFILE soft and hard values it ran with; when the limit cannot be raised to clients plus a margin it exits non-zero naming the limit and the command that raises it, never a silent partial run; (2) on this member `./bin/load 1000 60` passes all five criteria three times for each server cell portable, msgx and msgx-degraded, the table in the report with connect time p50 and max and the counts; fds and threads at end equal start; (3) the red proof: the run with `ulimit -n 256` in its shell fails with the named-limit message, shown once; (4) any failure is profiled (`sample <pid>` or a timed stack) before it is fixed, the profile cited; (5) nightly-soak.yml's soak-macos job gains `./bin/load 1000 60` per cell, proven by hand; (6) docs/LOAD.md gains a macOS section on the fd limit
TOOLCHAIN: Apple clang from the Command Line Tools, cmake, make, Python 3 (standard library only), on the fleet's macOS member JOB.md names; sample from macOS for profiling
GATE-NEEDS: darwin, arm64
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 180 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 180 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: none new

# macOS: 1000 clients per transport, three runs each, with the fd limit and counts in the summary

THE TASK. Run the many-client load harness at its 1000-client maximum against each macOS server transport three times each, make every run pass its five criteria with no fd or thread growth, make the fd limit loud, and run it nightly. Profile any failure before fixing it.

STEP 1. Confirm the platform is macOS on Apple silicon (`uname -s` prints Darwin, `uname -m` prints arm64, `sw_vers -productVersion` and `clang --version` recorded); cd into the staged checkout JOB.md names; `git log --oneline -1`; build as THIS REPOSITORY says and keep the last build line.
STEP 2. Extend the summary and the limit check (DONE-WHEN (1)); red proof (DONE-WHEN (3)).
STEP 3. Run the gate (DONE-WHEN (2)); profile and fix (DONE-WHEN (4)).
STEP 4. Nightly step and docs (DONE-WHEN (5), (6)).
STEP 5. Commit on your own branch as `rocketnet-macos-load-1000-clients-t: <what>` with the trailer; end as JOB.md says (the pull request body or RESULT.md carries the gate's lines and the run table).

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected transports (DRIVERS.md (read); sdk/rocketnet_drivers.inc (read) is the registry: on macOS the server builds portable and msgx, the client portable and msgx; msgx is the darwin batch pump over sendmsg_x/recvmsg_x, resolved by dlsym and probed per socket at create (sdk/rocketnet_platform.cpp, the block that opens 'The darwin batch pump'), degrading to the portable per-packet loop on refusal; ROCKETNET_PUMP=portable forces the degraded path; ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT pin a transport by name). The macOS build is `make` (Release) or `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel 4`; executables land in bin/ (test is the unit suite, functional the functional suite, `./bin/functional --run <case>` runs one case, test_msgx, test_lifecycle, test_driver, soak, soak_sim, load, transport_memory). .github/workflows/ci.yml (read) jobs build-and-test (the macos-15 rows on the fleet runner label rocketnet-macos) and its transport family matrix step (TRANSPORT_MATRIX_MACOS) are the worked example of every macOS step. Config fields have an env overlay named ROCKETNET_<TRANSPORT>_<FIELD> (sdk/rocketnet_config.cpp (read), rocketnet_config_env_var_name_for).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.

MACOS BOUND. This card runs on the fleet's macOS member (an Apple-silicon Mac mini; the CI runner label rocketnet-macos is on it), never on the coordinator's machine. Run builds and suites one at a time, never in the background, and never two suites at once. Build with `--parallel 4`. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any check that reads git status. Every run count in DONE-WHEN is a count of real runs on this member, each with its pass or fail and run time in the report; a run that was not made is reported as not made. A macOS firewall or permission prompt that a run raises is a finding to report, never something to click through.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, the home directory or any volume root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.

DEAL NOTE (coordinator 2026-10-19): earlier attempts of this card were dealt to Linux benches by a nova-sprint deal bug (the level, the rebalance and the next-attempt deal ignored GATE-NEEDS; fixed in nova-sprint v1.6.8), so their HOLDs were the machine's, not this brief's. The BENCH line keeps it on the fleet Mac; every DONE-WHEN run is on mini-m5-1 only. Carry the work already pushed and finish the macOS runs it still owes.

DECISION (coordinator audit 2026-10-20): attempts on the fleet member mini-m5-1 could not open a socket: the member's nova-sandbox wall on darwin grants no network-inbound (local ip) without --net-listen (measured with nova-sandbox policy), so every AF_INET/AF_INET6 bind returns EPERM, and the same red reproduces at BASE. That is the member's wall, not this brief and not the code. The BENCH line is removed and the card now carries a WHO line naming one friend, and that friend's child builds and runs every DONE-WHEN step directly on mini-m5-1 (Darwin arm64), outside the member wall, as an earlier macOS socket-errors attempt did. Carry the work already pushed (the carried head holds it); do not redo it. If bind still returns EPERM, report it with the errno and the command as evidence, verdict HOLD, blocked on environment; never weaken a test to pass.
CARRY: head=27aa95eab44a7d9ceea411b38649e116dcc0ac6c: fixed the macOS dual-stack port-0 collision (rocketnet_platform.cpp picks the port via an IPv4 probe first; 1000/1000 with 4000 ports held, 759 with the bug back); 9/9 Release gate, ulimit message, Debug suites green. One defect left: in 3 of 22 runs with the PORTABLE server ~97 clients timed out together on reliable messages ~3.4 s into traffic while the server's send ring was full (suspected 256-frame ring starving the last clients). Prove the cause (counters per client: queued vs sent vs acked), fix it at the cause (ring size, fair draining, or send budget) with a red-first proof, then 20 Release runs in a row of the 1000-client load with zero timeouts on portable and msgx. The Debug connect-deadline miss is base behaviour (connection-request crypto in Debug), not this card.
CARRY: Coordinator ruling 2026-10-20 09:35Z on the macOS dual-stack port-0 collision: rocketnet-macos-cookie-request-never-stalls (head cb809a1c, LAND, in review) fixes it at sdk/rocketnet_client_transport.cpp:796 (a macOS client of an IPv4 server binds an IPv4 socket; red proof holds 1024 IPv4 ports). Merge the base once that card lands (or merge its branch now and say so). Keep your rocketnet_platform.cpp IPv4-probe change ONLY if a red-first case shows a collision the client-side fix does not cover (e.g. a server binding [::]:0, or an IPv6 client); otherwise drop it so one fix lands. Then chase the remaining defect: ~97 clients timing out together while the PORTABLE server's send ring is full.
CARRY: head=149c1b864e38b1abaaa856a89407ff9df08d0f37. Coordinator ruling: the client I/O guard wake (ROCKETNET_CLIENT_IO_GUARD_TIMEOUT_SECONDS 0.001, rocketnet_client_io.h:40) goes to 0.100: it is a safety net behind the event wake, and 1 ms at top priority costs ~1.7% CPU per idle client (1681% for 1000 in one process). PATHS now include rocketnet_client_io.h, docs/CLIENT-IO.md and tla/RNClientIo + records. Show in tla/RNClientIo that no wake is lost with the longer guard (the guard only bounds the stall if an event wake is missed): run TLC through tlacheck merge --keep, never hand-written rows. Update docs/CLIENT-IO.md:356 with the measured reason. Then the 9-run gate, the 20-run streak and the msgx cells, on mini-m5-1 under the mkdir lock.
CARRY: OWNER PROFILE (2026-10-20): \"I want 1000 players tested and working on all platforms and all transports for v1.0.0. But each client should be light. 10HZ ticks. 1 packet per-update.\" and \"I want the hardcore load testing done after v1.0.0.\" So every 1000-client run here is the light profile: 1000 clients, each updating at 10 Hz and sending 1 packet per update, on every server transport of the platform; pass = all connect, every client receives, none disconnected, reliable in order. No high-rate or bandwidth stress (that is the next milestone). This supersedes any heavier rate or the 256 kbps profile written earlier in this brief.
NOTE: rebalanced to Stella (owner 2026-10-20, Rowan width 8): build on the latest pushed head of this card (the Rowan worker pushes it before stopping); run CHECKLIST.md; two-minute rule.
")
    (:id "rocketnet-macos-sanitizers-and-leaks" :feature "macOS: ASan, UBSan and TSan with Apple clang, and the leaks tool, on every suite and transport, nightly" :cells (- n -) :note "after the payment milestone (owner 2026-10-20); latest work: 0585538d on sprint/rocketnet-macos-sanitizers-and-leaks.w10.g1.e0" :brief "WHO: only friend rowan
CARRY: rocketnet-macos-sanitizers-and-leaks attempt 3 head=4b506cec7d12af242bd444a7c244af8626414805
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: code
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: -
LAND-AFTER: rocketnet-windows-udp-connreset-never-stalls-receive,rocketnet-win-socket-error-paths-t,rocketnet-win-lifecycle-stress,rocketnet-win-msvc-asan,rocketnet-win-app-verifier,rocketnet-win-libfuzzer-clang-cl,rocketnet-win-toolchain-matrix,rocketnet-win-transport-matrix-repeat,rocketnet-win-soak-per-transport,rocketnet-win-load-1000-clients,rocketnet-win-nightlies-on-fleet-runner,rocketnet-macos-hard-killed-peer-never-stalls-receive,rocketnet-macos-socket-errors-and-enobufs-t,rocketnet-macos-lifecycle-stress-t,rocketnet-api-polish-v1-followups,rocketnet-destroy-sends-disconnect,rocketnet-functional-cookie-expiry-uses-test-clock,rocketnet-functional-free-ports,rocketnet-linux-hard-killed-peer-never-stalls-receive,rocketnet-linux-lifecycle-stress,rocketnet-linux-socket-error-paths,rocketnet-linux-transport-matrix-repeat,rocketnet-macos-client-timing-under-app-nap,rocketnet-network-thread-priority-as-space,rocketnet-windows-client-overlapped-send,rocketnet-xdp-drops-first-fragment
SOURCE: the owner, 2026-10-19: \"Do we need to do any macos hardening, to match the windows hardening for rocketnet? Their main programmer on the engine is mac like us.\" The customer's engine programmer builds, runs and debugs on macOS, so the macOS client and server must be proven to the bar the Windows hardening set (stream rocketnet-api, the rocketnet-win-* cards) sets for Windows. The Windows cards rocketnet-win-msvc-asan and rocketnet-win-app-verifier run every suite under ASan and Application Verifier. On macOS no sanitizer runs at all: ci.yml's sanitizers, tsan and msan jobs and nightly-leaks.yml are Linux only. Apple clang 21 on the fleet Mac links -fsanitize=address,undefined and -fsanitize=thread (checked 2026-10-19); LeakSanitizer is not supported on Apple platforms, so leaks are checked with /usr/bin/leaks under MallocStackLogging on a non-ASan build.
PATHS: CMakeLists.txt,.github/workflows/nightly-leaks.yml,sdk/rocketnet_platform.cpp,sdk/rocketnet_server_transport.cpp,sdk/rocketnet_client_transport.cpp,functional/rocketnet_functional_tests.cpp,BUILDING.md,sdk/rocketnet.cpp,sdk/rocketnet_ack.cpp,sdk/rocketnet_tests.cpp,test_unit_ack_block_walk.cpp,functional/posix_socket_errors.cpp,functional/channel_harness.h,functional/thread_priority_timing.cpp,test_unit_sequence_burn.cpp
NEW: tools/macos/leaks_run.py
START: .github/workflows/ci.yml (read), .github/workflows/nightly-leaks.yml (read)
TEST: none because the checks are runs on the member that DONE-WHEN names, each with its count and its red proof
DONE-WHEN: (1) one CMake option (ROCKETNET_SANITIZE=address|thread, default off, Apple and Linux alike; the existing Linux jobs keep their flags) builds every suite with -fsanitize=address,undefined -fno-sanitize-recover=all or -fsanitize=thread; (2) the planted proofs, each a scratch commit not kept and shown once with the report's first lines: a one-byte heap overrun in a receive path is caught by ASan, a signed overflow by UBSan, an unsynchronised write between the client receive thread and update by TSan; (3) under ASan+UBSan on this member: ./bin/test, ./bin/test_lifecycle, ./bin/test_driver, ./bin/test_msgx and ./bin/functional with each pin (server portable, msgx, msgx forced portable; client portable, msgx) all green; under TSan ./bin/test and ./bin/functional with each pin green (setarch has no macOS form: say so and run without it); (4) tools/macos/leaks_run.py runs a named binary under `MallocStackLogging=1 leaks --atExit --` on a Release build without sanitizers, fails on any leak with its stack, and its check on a scratch binary that leaks one allocation turns it red; it runs test_lifecycle, functional and soak (100000 iterations) per pin, all green; (5) every report found is fixed at its cause in sdk/ with the run that showed it, or reported HOLD; (6) .github/workflows/nightly-leaks.yml gains a macOS job on [self-hosted, macOS, ARM64, rocketnet-macos] that runs (3) and (4) with a 60-minute timeout, each step proven green by hand on this member with its time in the report; (7) BUILDING.md names the option and the leaks script
TOOLCHAIN: Apple clang from the Command Line Tools, cmake, make, Python 3 (standard library only), on the fleet's macOS member JOB.md names; /usr/bin/leaks and MallocStackLogging from macOS
GATE-NEEDS: darwin, arm64
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 180 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 180 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: Apple clang's ASan, UBSan and TSan runtimes; the system leaks tool (LSan is unsupported on Apple)

# macOS: ASan, UBSan and TSan with Apple clang, and the leaks tool, on every suite and transport, nightly

THE TASK. Run every rocketnet suite on macOS under ASan with UBSan and under TSan, built with Apple clang behind one CMake option, on each macOS transport; check leaks with the system leaks tool on a plain build; prove each tool catches a planted defect; fix every report at its cause; add a nightly macOS job on the fleet runner.

STEP 1. Confirm the platform is macOS on Apple silicon (`uname -s` prints Darwin, `uname -m` prints arm64, `sw_vers -productVersion` and `clang --version` recorded); cd into the staged checkout JOB.md names; `git log --oneline -1`; build as THIS REPOSITORY says and keep the last build line.
STEP 2. Add the option (DONE-WHEN (1)) and make the planted proofs (DONE-WHEN (2)).
STEP 3. Run (3) one suite and one pin at a time; fix what they find (DONE-WHEN (5)).
STEP 4. Write and prove the leaks script (DONE-WHEN (4)); add the nightly job and prove its steps by hand (DONE-WHEN (6)); write (7).
STEP 5. Commit on your own branch as `rocketnet-macos-sanitizers-and-leaks: <what>` with the trailer; end as JOB.md says (the pull request body or RESULT.md carries the gate's lines and the run table).

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected transports (DRIVERS.md (read); sdk/rocketnet_drivers.inc (read) is the registry: on macOS the server builds portable and msgx, the client portable and msgx; msgx is the darwin batch pump over sendmsg_x/recvmsg_x, resolved by dlsym and probed per socket at create (sdk/rocketnet_platform.cpp, the block that opens 'The darwin batch pump'), degrading to the portable per-packet loop on refusal; ROCKETNET_PUMP=portable forces the degraded path; ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT pin a transport by name). The macOS build is `make` (Release) or `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel 4`; executables land in bin/ (test is the unit suite, functional the functional suite, `./bin/functional --run <case>` runs one case, test_msgx, test_lifecycle, test_driver, soak, soak_sim, load, transport_memory). .github/workflows/ci.yml (read) jobs build-and-test (the macos-15 rows on the fleet runner label rocketnet-macos) and its transport family matrix step (TRANSPORT_MATRIX_MACOS) are the worked example of every macOS step. Config fields have an env overlay named ROCKETNET_<TRANSPORT>_<FIELD> (sdk/rocketnet_config.cpp (read), rocketnet_config_env_var_name_for).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.

MACOS BOUND. This card runs on the fleet's macOS member (an Apple-silicon Mac mini; the CI runner label rocketnet-macos is on it), never on the coordinator's machine. Run builds and suites one at a time, never in the background, and never two suites at once. Build with `--parallel 4`. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any check that reads git status. Every run count in DONE-WHEN is a count of real runs on this member, each with its pass or fail and run time in the report; a run that was not made is reported as not made. A macOS firewall or permission prompt that a run raises is a finding to report, never something to click through.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, the home directory or any volume root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.

DEAL NOTE (coordinator 2026-10-19): earlier attempts of this card were dealt to Linux benches by a nova-sprint deal bug (the level, the rebalance and the next-attempt deal ignored GATE-NEEDS; fixed in nova-sprint v1.6.8), so their HOLDs were the machine's, not this brief's. The BENCH line keeps it on the fleet Mac; every DONE-WHEN run is on mini-m5-1 only. Carry the work already pushed and finish the macOS runs it still owes.


DECISION (coordinator 2026-10-19, after attempts 5 and 6): both attempts ended with no result because `/usr/bin/leaks --atExit` never returned on mini-m5-1. Root cause measured on the member: Developer Mode is disabled there (`DevToolsSecurity -status`), so leaks stops the target at exit and then waits for a task-port authorization that a non-GUI session never grants; the target stays stopped (state T) and leaks prints nothing, so the run went silent and was idle-reaped at 300 s. A bare run outside any sandbox hangs the same way. So: before any leaks run, record `DevToolsSecurity -status`; run every leaks invocation under a hard 120 s timeout that kills leaks and its stopped target; if Developer Mode is disabled or leaks times out, do not retry it: report DONE-WHEN (4) and the leaks step of (6) as not-done, blocked on environment (Developer Mode off on the member), with the status line as evidence, finish everything else (the sanitizer option, the planted proofs, the ASan/UBSan/TSan runs, BUILDING.md), and finish HOLD naming that one block. Never run `DevToolsSecurity` with sudo or change any system setting. This attempt is worked by the WHO friend's child directly on mini-m5-1.

DECISION (coordinator audit 2026-10-20): the BENCH line is removed. the BENCH line with the WHO line left the card dealable to no one (the bench names a fleet member, the WHO a friend row), so it sat ready untaken. the WHO friend's child works it directly on mini-m5-1 as the decision above says.
CARRY: head=4b506cec7d12af242bd444a7c244af8626414805. PATHS widened: apply the three fix diffs in /private/tmp/claude-501/-Volumes-nova-ai-rowan/91b9aacc-938b-4bf3-8fa6-975024be9624/scratchpad/rnreads/sanitizers-evidence/ (UBSan enum loads rocketnet.cpp:96,:223; TSan g_ack_visit_count rocketnet_ack.cpp:514; TSan volatile flags posix_socket_errors.cpp:968), each with its planted-red proof, then the full sanitizer set on the merged base. Leaks steps wait on Developer Mode on the studio (owner action); report them as pending, not failed. Base-only reds (env-pinned weather cases, TSan matrix 52.5s vs 50s, server_xdp.c:407 misalign, Release 100k soak refusal) are listed, not fixed here.
CARRY: w6 HOLD head 85b0f72b: three sanitizer fixes proven; leaks script fixed (per-case, signed binaries, self-test red); Developer Mode now on and the Xcode license now accepted on the studio (DEVELOPER_DIR workaround no longer needed). Remaining: (a) apply the channel_harness.h destroy fix (proposed-channel-harness.diff in ~/.local/state/nova-reads/rocketnet-macos-sanitizers-and-leaks.w6/) and show control_fragment_packets_each_end at 0 leaks on all pins; (b) fix the TSan race in functional/thread_priority_timing.cpp :130/:353; (c) merge current BASE and re-run; (d) functional reds that also fail at BASE under sanitizers are pre-existing: list them, do not fix them here.
NOTE: rebalanced to Stella (owner 2026-10-20): Rowan's row was over its width; build on the latest pushed head.
")
    (:id "rocketnet-linux-nightly-soak-green-at-base" :feature "Linux: the nightly soak jobs green at the branch" :cells (n - -) :note "after the payment milestone (owner 2026-10-20); latest work: 0e3143ab on sprint/rocketnet-linux-nightly-soak-green-at-base.w4.g1.e0" :brief "RESULT: rocketnet-linux-nightly-soak-green-at-base sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: high
STREAM: rocketnet-api
DEPENDS-ON: -
SOURCE: TESTING.md (branch testing-md-v1, docs/testing.sexp) lists this cell as no card at sprint/rocketnet-proof 091204c9. Row: Nightlies / Linux. The scheduled nightlies run only on main, which is far behind this branch; nightly-soak's Linux soak job (the soak run, the 1000-client load step per cell) and sanitized-soak (soak_sim under asan + ubsan + lsan) have not run on this branch's code as scheduled. rocketnet-linux-load-1000-clients proved only its load step by hand.
PATHS: .github/workflows/nightly-soak.yml
START: .github/workflows/nightly-soak.yml (read the soak job's ubuntu row and the sanitized-soak job)
TEST: none because the gate is the two Linux jobs' own steps, run by hand as DONE-WHEN says
DONE-WHEN: (1) at BASE, on this member, every step of nightly-soak.yml's soak job (the ubuntu row) and its sanitized-soak job is run by hand as the workflow spells it, one job at a time, each command's exit code, run time and last lines kept; the xdp load cell runs in a virtme-ng guest as the workflow does; (2) each red's cause is named in the report with file:line, fixed in this commit when the fix is in a file PATHS names and otherwise carried as a proposed diff for its own card; a bound is never loosened; a red already held by rocketnet-linux-soak-per-transport is listed with that id; (3) red first: each step fixed in this commit is shown red at BASE and green at head; (4) at head every step in (1) exits 0 on this member, or the report names the proposed diff it waits on, each job's run time recorded
TOOLCHAIN: gcc and clang, cmake, make, Python 3 (standard library only), on a Linux bench member JOB.md names; vng (virtme-ng) with the member's kernel image for the xdp cell, root only inside the guest
GATE-NEEDS: linux, xdp
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 180 minutes.

# Linux: the nightly soak jobs green at the branch

THE TASK. Run nightly-soak's Linux soak and sanitized-soak jobs by hand at BASE, fix each red at its cause, and show both green at head with their run times.
STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.


RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, the home directory or any volume root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
")
    (:id "rocketnet-win-libfuzzer-clang-cl" :feature "The fuzz targets build and run under libFuzzer on Windows" :cells (- - n) :note "after the payment milestone (owner 2026-10-20); latest work: sprint/rocketnet-win-libfuzzer-clang-cl.w9.g5.e0" :brief "CARRY: rocketnet-win-libfuzzer-clang-cl attempt 9 head=63b568add8b117fd6562188a88dd1fbbbba51bba
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: code
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: -
LAND-AFTER: rocketnet-windows-udp-connreset-never-stalls-receive,rocketnet-win-socket-error-paths-t,rocketnet-win-lifecycle-stress,rocketnet-win-msvc-asan,rocketnet-win-app-verifier,rocketnet-api-polish-v1-followups,rocketnet-destroy-sends-disconnect,rocketnet-functional-free-ports,rocketnet-linux-lifecycle-stress,rocketnet-macos-client-timing-under-app-nap,rocketnet-macos-hard-killed-peer-never-stalls-receive,rocketnet-network-thread-priority-as-space,rocketnet-win-nightlies-on-fleet-runner,rocketnet-windows-client-overlapped-send,rocketnet-xdp-drops-first-fragment
SOURCE: the owner, 2026-10-19: \"I think we need to do extra testing and checking on windows and windows transports before we deliver rocketnet. It will be their primary platform so we should make it perfect.\" The customer ships on Windows; v1.0.0 is not delivered until Windows is proven. The four fuzz harnesses (fuzz/rocketnet_fuzz.cpp, fuzz/rocketnet_fuzz_message.cpp, fuzz/rocketnet_fuzz_connect_token.cpp, fuzz/rocketnet_fuzz_receive_structured.cpp) fuzz only on Linux (fuzz/Makefile is Linux only and links bpf and xdp); Windows only replays the committed corpora through fuzz/replay_main.cpp. clang-cl supports -fsanitize=fuzzer,address on Windows, so the receive and parse paths as built for Windows can be fuzzed there.
PATHS: CMakeLists.txt,fuzz/*.cpp,fuzz/corpus/**,fuzz/corpus_message/**,fuzz/corpus_connect_token/**,fuzz/corpus_receive_structured/**,.github/workflows/fuzz-nightly.yml,sdk/rocketnet_platform.cpp,sdk/rocketnet_server_iocp_windows.cpp,sdk/rocketnet_server_rio_windows.cpp,sdk/rocketnet_server_transport.cpp,sdk/rocketnet_client_transport.cpp,BUILDING.md,build-fuzz/fuzz_rocketnet_fuzz.exe,build/test.exe,fuzz/rocketnet.dict,sdk/rocketnet_base64.cpp
START: fuzz/Makefile (read), CMakeLists.txt (read), .github/workflows/fuzz-nightly.yml (read)
TEST: none because the checks are runs on native Windows that DONE-WHEN names, each with its count and its red proof
DONE-WHEN: (1) a CMake option ROCKETNET_BUILD_FUZZERS (default OFF), honoured when the compiler is clang-cl, builds fuzz_rocketnet_fuzz, fuzz_rocketnet_fuzz_message, fuzz_rocketnet_fuzz_connect_token and fuzz_rocketnet_fuzz_receive_structured with -fsanitize=fuzzer,address against the development library built with -fsanitize=fuzzer-no-link,address; configured with `cmake -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DROCKETNET_BUILD_FUZZERS=ON` (or the Visual Studio generator with -T ClangCL), the four targets build on this member; any MSVC-only flag that clang-cl refuses is fixed in CMakeLists.txt; (2) each of the four runs for 15 minutes on its committed corpus (`-max_total_time=900 -rss_limit_mb=2048`, with fuzz/rocketnet.dict where the Linux runs use it) with no crash, leak or timeout; the exec/s and the final cov and corp lines of each are in the report; (3) a planted defect (scratch commit, not kept) in a parse path rocketnet_fuzz reaches is found by it within 15 minutes, the crash line in the report; (4) a real crash found is minimised, added to its corpus directory, fixed at its cause in sdk/, and the Release replay step (ci.yml's) passes over it; (5) .github/workflows/fuzz-nightly.yml gains a Windows job on [self-hosted, Windows, X64, rocketnet-windows] that builds with the option and runs each target 10 minutes; (6) the MSVC Release build and its test.exe stay green and the four fuzz_replay executables pass over their corpora
TOOLCHAIN: Visual Studio 18 with its LLVM toolset (clang-cl, the component Microsoft.VisualStudio.Component.VC.Llvm.Clang, which carries libFuzzer and the ASan runtime), cmake, Ninja from the Visual Studio install, on the native Windows member JOB.md names; a missing clang-cl is HOLD naming that component
GATE-NEEDS: windows
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 150 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 150 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: libFuzzer and AddressSanitizer as shipped with Visual Studio's clang-cl; the existing harnesses and dictionary unchanged unless a Windows build needs it.

# The fuzz targets build and run under libFuzzer on Windows

THE TASK. Build the four rocketnet fuzz harnesses with clang-cl and libFuzzer on Windows behind one CMake option, fuzz each for fifteen minutes from its corpus, prove a planted parse defect is found, fix and keep any real crash as a corpus file, and add a nightly Windows fuzz job.

STEP 1. Confirm the platform is native Windows (`cmd /c ver` prints a Windows version; under Git Bash `uname -s` prints MINGW or MSYS, never Linux); cd into the staged checkout JOB.md names; `git log --oneline -1`; build Release as THIS REPOSITORY says and keep the last build line. Confirm `clang-cl --version` answers from the Visual Studio LLVM toolset.
STEP 2. Add the option and build the four targets (DONE-WHEN (1)); a 1-minute run of each first.
STEP 3. Run the gate: DONE-WHEN (2), (3) and (6), one fuzzer at a time; handle crashes (DONE-WHEN (4)).
STEP 4. Add the nightly job (DONE-WHEN (5)).
STEP 5. Commit on your own branch as `rocketnet-win-libfuzzer-clang-cl: <what>` with the trailer; end as JOB.md says (the pull request body or RESULT.md carries the gate's lines and the run table).

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected server transports (DRIVERS.md (read); sdk/rocketnet_drivers.inc (read) is the registry: on Windows the server builds portable, iocp and rio, the client portable; ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT pin one by name). The Windows build is `cmake -B build -A x64` then `cmake --build build --config Release --parallel 4`; executables land in bin/Release/ (test.exe is the unit suite, functional.exe the functional suite, `functional.exe --run <case>` runs one case, test_iocp.exe the IOCP driver tests, test_lifecycle.exe, soak.exe, load.exe). .github/workflows/ci.yml (read) job build-and-test-windows is the worked example of every Windows step, including the transport family matrix. Config fields have an env overlay named ROCKETNET_<TRANSPORT>_<FIELD> (sdk/rocketnet_config.cpp (read), rocketnet_config_env_var_name_for).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.

WINDOWS BOUND. This card runs on a native Windows member (captain today, a weak machine; titan, a Xeon workstation, soon). Run builds and suites one at a time, never in the background, and never two suites at once. Build with `--parallel 4`. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any check that reads git status. Every run count in DONE-WHEN is a count of real runs on this member, each with its pass or fail and run time in the report; a run that was not made is reported as not made.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the Windows member JOB.md names, never the coordinator's machine. Never search C:\\, the user profile or any drive root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
CARRY: Coordinator 2026-10-20 13:05Z: brief confirmed current for the v1.0 push; the newest CARRY above names exactly what is owed. Continue from the last pushed head merged with the current base.
CARRY: w5 HOLD head 2d1bd5b4 (w4 8021603f + base 28038005, clean, pushed): only the gate is owed, never run because captain's winlock starved the shared take for 2h. Gate, in one SHARED winlock hold on captain: rebuild the 4 fuzz targets, 60 s each, fuzz_replay over each corpus, Release test.exe. Exclusive takers are paused so shared jobs run.
FIX (coordinator, 2026-10-20, owner's two-minute rule): DONE-WHEN (2)'s four 15-minute fuzz runs are OWED until features land. Do not run them, and do not count them as fails. At the exact head (581f28b merged with the current base), in one shared winlock hold on captain:
- rebuild the 4 targets;
- run each for 60 s (under 2 minutes);
- fuzz_replay over each corpus;
- the planted-defect proof: a planted bug in a fuzzed parser must be caught within 60 s at its own frame, then restored;
- Release test.exe.
Make sure the nightly step's paths match the Ninja output dir (build-fuzz/). Then finish LAND with the 15-minute runs listed as OWED.
")
    (:id "rocketnet-win-lifecycle-stress" :feature "Create, destroy, connect, restart: a thousand times on Windows with nothing left behind" :cells (- - n) :note "after the payment milestone (owner 2026-10-20); latest work: 745ffb1b on sprint/rocketnet-win-lifecycle-stress.w13.g3.e0" :brief "CARRY: rocketnet-win-lifecycle-stress attempt 2 head=0959fdc23f471c26d86b9ce30794d53088cfcb25 (the last pushed head, on sprint/rocketnet-win-lifecycle-stress.w2.g1.e21, verified on GitHub 2026-10-20; continue from it, merged with BASE, and redo nothing it already holds). This head supersedes any earlier carry the brief names below.
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: code
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: -
LAND-AFTER: rocketnet-windows-udp-connreset-never-stalls-receive,rocketnet-win-socket-error-paths-t,rocketnet-api-polish-v1-followups,rocketnet-destroy-sends-disconnect,rocketnet-functional-cookie-expiry-uses-test-clock,rocketnet-functional-free-ports,rocketnet-linux-lifecycle-stress,rocketnet-macos-client-timing-under-app-nap,rocketnet-macos-hard-killed-peer-never-stalls-receive,rocketnet-network-thread-priority-as-space,rocketnet-win-nightlies-on-fleet-runner,rocketnet-windows-client-overlapped-send,rocketnet-xdp-drops-first-fragment
SOURCE: the owner, 2026-10-19: \"I think we need to do extra testing and checking on windows and windows transports before we deliver rocketnet. It will be their primary platform so we should make it perfect.\" The customer ships on Windows; v1.0.0 is not delivered until Windows is proven. On Windows rocketnet_init calls WSAStartup and rocketnet_term calls WSACleanup (sdk/rocketnet_platform.cpp rocketnet_platform_init and rocketnet_platform_term), the IOCP and RIO transports bind with SO_EXCLUSIVEADDRUSE, and each transport starts and joins threads. A game creates and destroys clients and servers many times per session, restarts servers on the same port, and may use Winsock itself; no test repeats these cycles on Windows and counts what is left behind.
PATHS: functional/rocketnet_functional_tests.cpp,CMakeLists.txt,sdk/rocketnet_platform.cpp,sdk/rocketnet_server_iocp_windows.cpp,sdk/rocketnet_server_rio_windows.cpp,sdk/rocketnet_server_transport.cpp,sdk/rocketnet_client_transport.cpp
NEW: functional/windows_lifecycle_stress.cpp
START: functional/rocketnet_functional_tests.cpp (read), functional/lifecycle_driver.cpp (read), sdk/rocketnet.cpp (read)
TEST: none because the checks are runs on native Windows that DONE-WHEN names, each with its count and its red proof
DONE-WHEN: (1) functional/windows_lifecycle_stress.cpp, compiled on Windows only and listed in the functional target in CMakeLists.txt, adds cases that each record GetProcessHandleCount and the process thread count before and after and fail on any difference beyond plus or minus 2: windows_init_term_cycles (rocketnet_init and rocketnet_term 1000 times; then a socket the test opened with its own WSAStartup before the loop still sends and receives, and the test's own WSACleanup balances), windows_server_create_destroy_cycles (200 create, start, destroy of a message server on each of portable, iocp and rio), windows_server_restart_same_port (100 stop and recreate of each transport on one fixed port, each rebind succeeding within 1 s and a client connecting each time), windows_client_connect_disconnect_cycles (200 connect, exchange, disconnect, destroy of a client against one server, on each server transport), and windows_destroy_while_traffic (destroy a server and its client mid-stream 50 times; no crash, no hang beyond 2 s, nothing left); (2) each case is red when its object leaks: a scratch commit (not kept) that skips one CloseHandle or one thread join in a transport destroy turns the matching case red, shown once; (3) the five cases pass 10 runs in a row on this member (`functional.exe --run <case>`), run times recorded; (4) each leak, hang or rebind failure found is fixed at its cause in sdk/, or reported with verdict HOLD; (5) the full functional.exe and test.exe stay green
TOOLCHAIN: Visual Studio 18 / MSVC x64, cmake, on the native Windows member JOB.md names
GATE-NEEDS: windows
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 150 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 150 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: Win32 GetProcessHandleCount and CreateToolhelp32Snapshot for the counts; the functional harness already in the tree.

# Create, destroy, connect, restart: a thousand times on Windows with nothing left behind

THE TASK. Add Windows functional cases that cycle init and term, server create and destroy, same-port restart, client connect and disconnect, and destroy under traffic, on every Windows transport, and that fail on any handle or thread left behind or a Winsock state broken for the embedding game. Prove each is red on a planted leak and fix what they find.

STEP 1. Confirm the platform is native Windows (`cmd /c ver` prints a Windows version; under Git Bash `uname -s` prints MINGW or MSYS, never Linux); cd into the staged checkout JOB.md names; `git log --oneline -1`; build Release as THIS REPOSITORY says and keep the last build line.
STEP 2. Write the five cases (DONE-WHEN (1)); prove them red on the scratch leak (DONE-WHEN (2)) and drop the scratch commit.
STEP 3. Run the gate: DONE-WHEN (3) and (5), one suite at a time; fix what fails (DONE-WHEN (4)).
STEP 4. Commit on your own branch as `rocketnet-win-lifecycle-stress: <what>` with the trailer; end as JOB.md says (the pull request body or RESULT.md carries the gate's lines and the run table).

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with runtime-selected server transports (DRIVERS.md (read); sdk/rocketnet_drivers.inc (read) is the registry: on Windows the server builds portable, iocp and rio, the client portable; ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT pin one by name). The Windows build is `cmake -B build -A x64` then `cmake --build build --config Release --parallel 4`; executables land in bin/Release/ (test.exe is the unit suite, functional.exe the functional suite, `functional.exe --run <case>` runs one case, test_iocp.exe the IOCP driver tests, test_lifecycle.exe, soak.exe, load.exe). .github/workflows/ci.yml (read) job build-and-test-windows is the worked example of every Windows step, including the transport family matrix. Config fields have an env overlay named ROCKETNET_<TRANSPORT>_<FIELD> (sdk/rocketnet_config.cpp (read), rocketnet_config_env_var_name_for).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.

WINDOWS BOUND. This card runs on a native Windows member (captain today, a weak machine; titan, a Xeon workstation, soon). Run builds and suites one at a time, never in the background, and never two suites at once. Build with `--parallel 4`. A test red at head is rerun at the unchanged BASE only for that one test. Commit the work before any check that reads git status. Every run count in DONE-WHEN is a count of real runs on this member, each with its pass or fail and run time in the report; a run that was not made is reported as not made.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the Windows member JOB.md names, never the coordinator's machine. Never search C:\\, the user profile or any drive root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
CARRY: head=793576d9935bd9203a2a6ae27c6efdd29d0bd5b6 (functional/windows_lifecycle_stress.cpp ported to current names; all 5 cases passed 1 Release run on captain). Owed: merge current base, planted-leak red proof (skip CloseHandle(iocp->port) in rocketnet_server_iocp_destroy; skip WSACleanup in platform term), Debug build+gate, the 10-run gate under winlock --exclusive, full functional.exe and test.exe. Debug stack overflows and the >698 MB Debug bound are not this card's (nightlies card; owner's by-hand ruling).
CARRY: Coordinator ruling (2026-10-20): the Debug test.exe stack overflows and the over-698 MB Debug cases belong to rocketnet-win-nightlies-on-fleet-runner; run Debug with ROCKETNET_TEST_MAX_CASE_RSS_MB lifted for that run only (owner's ruling); neither blocks LAND here. rocketnet-functional-builds-on-windows has landed: merge the current base.
CARRY: head=f149e5f59879ae6c4ca0fbd2ac1f6c155f9cb598 (base dee6a29b merged, Release green on captain). Captain is congested: queue ONCE with winlock --exclusive and do EVERYTHING inside that single hold as one script: merge current base first (on captain, before queueing), Debug build, the two planted leaks (skip CloseHandle(iocp->port) at rocketnet_server_iocp_windows.cpp:441 -> windows_server_create_destroy_cycles red at its handle check; skip WSACleanup at rocketnet_platform.cpp:3270 -> windows_init_term_cycles red at probe == INVALID_SOCKET), restore, the 10-run gate, full functional.exe and test.exe (Debug RSS bound lifted per the owner). Never queue twice.
CARRY: w8 head a78dbc1d (sprint/rocketnet-win-lifecycle-stress.w8.g1.e0): the thread check fails only on growth (:113), handles unchanged (:112); captain Debug and Release build; test.exe green in both; planted leaks red at :112 and :355. ONLY OWED: merge BASE (4e1be4e8+), then one EXCLUSIVE winlock hold (allowed from 15:45Z): full functional.exe Release, Debug with the 4096 bound, and the 10-run gate of the five cases. Scripts are ready at /mnt/c/rowan-jobs/rocketnet-win-lifecycle-stress.w8/excl.sh. Push from captain over HTTPS via its nova-push-credential helper (benches can't reach captain).
FIX (coordinator, 2026-10-20, owner deferral): under the owner's two-minute rule this card does NOT run its long soak, load, stress or repeat runs now. Scope it to what can be proven in short runs: the harness, workflow or code change the card adds, built and run in Debug at the exact head with each run under 2 minutes, a red-first proof where the card fixes a bug, and CHECKLIST.md. Then finish LAND, listing every long run as OWED (they become a separate card after v1.0.0-preview1). If the card has no change beyond the long run itself, finish LAND with only the docs or workflow wiring and the long run OWED. Never shorten a long run and call it a pass. Build on the latest pushed head.
")
    (:id "rocketnet-r1-windows-nightlies-all-run" :feature "Windows nightlies: all jobs run, warnings gated, verifier script safe" :cells (- - n) :note "after the payment milestone (owner 2026-10-20); latest work: no pushed branch" :brief "REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: ci
PRIORITY: high
STREAM: rocketnet-api
DEPENDS-ON: -
SOURCE: holistic review round 1 at base 2803800 (six whole-system reviews and three re-reads, merged in v1-review/round1-merged.md). A re-read, verified at the tip: three Windows nightly jobs are still `if: false` (.github/workflows/nightly-examples.yml:116 examples-windows, the only Windows run of run_examples; nightly-frame-budget.yml:120 windows-frame-budget; nightly.yml:176 windows-driver-extension), though the fleet runner label [self-hosted, Windows, X64, rocketnet-windows] serves ci.yml. The nightly windows-toolchains job has no warning check, and CMakeLists.txt adds -Wno-deprecated-declarations to every clang-cl target. tools/windows/appverif_run.py runs -configure with check=False, loops export_logs `while True`, leaves -logtofile enabled globally and kills only the direct child on timeout.
PATHS: .github/workflows/nightly-examples.yml,.github/workflows/nightly-frame-budget.yml,.github/workflows/nightly.yml,docs/PLATFORMS.md,tools/windows/appverif_run.py
START: .github/workflows/nightly.yml (read the two jobs rocketnet-win-nightlies-on-fleet-runner moved), tools/windows/appverif_run.py (read 160-245)
TEST: none because the checks are runs on native Windows that DONE-WHEN names, each with its count and its red proof
DONE-WHEN: (1) the three jobs run on the fleet Windows label with no `if: false`, configured like the two jobs already moved; each job's steps are run by hand on captain under winlock and pass (run_examples.py, check_frame_budget.py, check_driver_extension.py), or a failure is fixed at its cause or reported with the line; (2) the windows-toolchains job fails on any clang-cl warning MSVC does not also give (a warning-diff step or /WX), red on a planted warning; (3) appverif_run.py checks the -configure exit status, bounds the log export loop, restores -logtofile, and kills the whole process tree on timeout; a dry run on captain shows each; (4) docs/PLATFORMS.md names these jobs as running; (5) the workflows parse (PyYAML) and Linux ./bin/test is untouched
TOOLCHAIN: cmake and MSVC on captain under winlock (one suite at a time); cmake, make on a Linux member JOB.md names
GATE-NEEDS: windows,linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 150 minutes.

# Windows nightlies: all jobs run, warnings gated, verifier script safe

THE TASK. Three Windows nightly jobs are still disabled, the toolchain nightly lets new warnings through, and the Application Verifier script can hang or leave global settings behind. Move the jobs to the fleet runner, gate warnings, and make the script safe.

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.


RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, the home directory or any volume root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
NOTE: fix routed to friends (owner 2026-10-20); build on the latest pushed head of this card, never discard it unless plainly garbage (say so); run CHECKLIST.md; two-minute rule.
")
    (:id "rocketnet-r1-tla-ledger-current" :feature "TLA ledger: every case finishes or says why; docs current" :cells (n n n) :note "after the payment milestone (owner 2026-10-20); latest work: d629f045 on sprint/rocketnet-r1-tla-ledger-current.w4.g1.e0" :brief "REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: fix
PRIORITY: high
STREAM: rocketnet-api
DEPENDS-ON: -
LAND-AFTER: rocketnet-r1-tlc-gate-runs-on-sprint-branch,rocketnet-r1-uring-update-bounded,rocketnet-r1-rio-update-bounded,rocketnet-r1-serversession-model-matches-stop
SOURCE: holistic review round 1 at base 2803800 (six whole-system reviews and three re-reads, merged in v1-review/round1-merged.md). Three reviewers: 17 TLC cases have never completed (exit 124 in tla/RUNS.tsv: the nine MCClientConnectionS*.cfg, LArbitrary, LCrash, MCFragmentsAndAcks.cfg and Live, PatNone/First/Burst, FindStaleSerialSmall, MCRNEndpointTwoWay/Partial), so ClientConnection's SafetyInvariants and the committed FragmentsAndAcks instance have no finished full-environment safety check; MCRNChannel.cfg passes at 109.4 s of a 110 s budget; tla/README.md says 47 cases against 217; docs/TLA-INVENTORY.md lists nine modules that do not exist and describes the fixed replay long-skip bug (F1) as open; docs/WITNESSES.tsv maps invariants to a retired draft's lines; tla/RUNS-ServerConnection.tsv is an unread duplicate; three COVERAGE rows (RNSimFaults, RNLiveFleetRun, RNFlows) promise code that does not exist; tla/ReplayWindow.tla's header describes fixed bugs, tla/RNUringDriver.tla's says it is not yet written, tla/RNDriverSendStaging.tla cites stale line numbers.
PATHS: tla/README.md,tla/RUNS-ServerConnection.tsv,tla/ReplayWindow.tla,tla/RNDriverSendStaging.tla,tla/CASES.tsv,tla/RUNS.tsv,tla/COVERAGE.tsv,docs/TLA-INVENTORY.md,docs/WITNESSES.tsv
START: tla/CASES.tsv and tla/RUNS.tsv (read), tla/README.md (read), docs/TLA-INVENTORY.md (read)
TEST: none because the cases are TLC rows and ledger checks; run as DONE-WHEN says
DONE-WHEN: (1) for each of the 17 never-completed cases: add one safety-only full-environment cfg per loss pattern that finishes inside its budget on the bench (no PROPERTIES), or record the case as unverified in CASES.tsv with the reason; no invariant is weakened; (2) MCRNChannel.cfg finishes with at least 25% headroom (a smaller instance or a split cfg, same invariants); (3) tla/README.md's case table is generated from CASES.tsv (a short script, or the existing ledger tool) and its counts match; docs/TLA-INVENTORY.md names only modules that exist and marks F1 fixed with the commit; docs/WITNESSES.tsv cites invariant names, not line numbers; tla/RUNS-ServerConnection.tsv is deleted; (4) the three design-only COVERAGE rows say `design, no code`; ReplayWindow.tla and RNDriverSendStaging.tla headers describe the current code by symbol; (5) `python3 tools/check_tla_coverage.py --all` exits 0 and `/opt/tla/tlacheck groups --root . --stale` prints [] (records only via /opt/tla/tlacheck merge --keep)
TOOLCHAIN: cmake, make, java and /opt/tla on a Linux member JOB.md names
GATE-NEEDS: tla,linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 180 minutes.

# TLA ledger: every case finishes or says why; docs current

THE TASK. Seventeen model cases have never finished, one runs at its time limit, and the TLA README, inventory and witness map describe a tree that no longer exists. Give each unfinished case a safety check that finishes or record it as unverified, and regenerate the ledger documents from the case table.

STEP 1. After any model edit, run TLC for its group and refresh the records with tlacheck merge --keep (tla/RUNS.tsv is written only by it).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.


RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, the home directory or any volume root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
CARRY: Owner 2026-10-20: fix cards go to friends, not the fleet. Carry the latest pushed head of this card and its FIX lines. Run CHECKLIST.md before finishing; two-minute rule.
CARRY: rocketnet-r1-tla-ledger-current attempt 3 head=9cbb38903fea51981731c0b6529f98c464482a90
FIX (coordinator, 2026-10-20, converge for v1.0): finish LAND at this head with the current base merged. Two DONE-WHEN items move after v1.0, so don't HOLD for them:
- item (2): the MCRNChannel.cfg headroom, since a smaller instance would cut coverage and that's an owner call later;
- item (3): rekeying docs/WITNESSES.tsv and its checker.
List both as OWED (post-v1.0). Approved as PATHS-PROPOSED: tla/README-ServerConnection.md and tla/README-ReplayWindow.md. The MCReplayWindowMid 445 s manual rerun is OWED.
")
    (:id "rocketnet-server-send-capacity-sized-from-config" :feature "Server send capacity is sized from the config, never a fixed 256" :cells (n n n) :note "after the payment milestone (owner 2026-10-20); latest work: no pushed branch" :brief "REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: fix
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: -
LAND-AFTER: rocketnet-r1-server-send-cap-fair-at-one,rocketnet-receive-budget-twice-send
SOURCE: owner, 2026-10-20: \"assume 100 packets per-client at 60HZ and this should be the budget for sends from the server ... It may not be able to sustain this.\" Also the owner, on the portable server's 256-packet send ring: \"obviously this is woefully inadequate.\" Found by rocketnet-load-light-profile-linux w1 (head e132c94d): sdk/rocketnet_server_transport.cpp:75 `#define ROCKETNET_SERVER_MAX_SEND_PACKETS 256` and :107 size the payload ring; every packet past the 256th in one send_packets call is dropped and counted SEND_RING_FULL. With the light profile (10 Hz, one reliable message per client per update, max_packets_per_update=1) 250 clients pass, 300 end with exactly 256 connected and SEND_RING_FULL=1364, and 1000 fail every_client_receives. v1.0 is 1000 clients on every platform and transport. rocketnet-r1-server-send-cap-fair-at-one makes sending fair under a cap; this card removes the cap as a capacity limit.
PATHS: CMakeLists.txt,sdk/rocketnet_constants.h,sdk/rocketnet_server_transport.cpp,sdk/rocketnet_message_server.cpp,sdk/rocketnet_server_socket_linux.cpp,sdk/rocketnet_server_gso.h,sdk/rocketnet_server_gso_linux.cpp,sdk/rocketnet_server_uring.h,sdk/rocketnet_server_uring_linux.cpp,sdk/rocketnet_server_iocp.h,sdk/rocketnet_server_iocp_windows.cpp,sdk/rocketnet_server_rio.h,sdk/rocketnet_server_rio_windows.cpp,sdk/rocketnet_driver_loopback.h,sdk/rocketnet_driver_loopback.cpp,sdk/rocketnet_platform.cpp,sdk/rocketnet_config.cpp,sdk/rocketnet_config.h,DRIVERS.md,test_unit_receive_budget_twice_send.cpp
NEW: test_unit_server_send_capacity.cpp
START: sdk/rocketnet_server_transport.cpp (read 60-130, 2190-2360), sdk/rocketnet_message_server.cpp (read 780-830), sdk/rocketnet_constants.h (read), DRIVERS.md (read)
TEST: none because the unit suite's REGISTER_UNIT_TEST cases have no go or ctest name; the cases are in the NEW file, run as DONE-WHEN says
DONE-WHEN: (1) a table in the report, one row per server driver (portable, msgx, gso, io_uring, xdp, iocp, rio, loopback): its send capacity per update at BASE, the constants and file:line it comes from, and the most one update can ask of it; (2) THE BUDGET (owner): the server's send capacity per update is max_packets_per_update packets per client (default 256, owner 2026-10-20: \"OK default it to 256. Let's aim high.\"), for an update rate of 60 Hz: max_clients x 256 packet slots by default (256,000 at 1000 clients, about 15M packets/s), plus room for control traffic (keep-alives, disconnects, challenge and connection replies to pending clients). CONFIGURABLE (owner, 2026-10-20: \"Remember that 100 packets per-update should be configurable in the config!\"): the per-client packets per update is a server config field, default 256 (owner revised the same day: \"OK default it to 256. Let's aim high.\"). The field is max_packets_per_update (owner, 2026-10-20), the existing per-client per-update cap: keep its default at 256 (unchanged) and size capacity from it; never add a second field for the same thing. Say so in the report, because it lands with the v1.0 config revision (rocketnet-r1-config-one-revision). Every driver computes capacity at create as max_clients x that field, never a fixed #define; it is allocated once at create, with no allocation per update. REFERENCE (owner, 2026-10-20): \"This is why the XDP transport takes a lot of memory. It's sized for space game doing exactly this.\" The XDP driver's sizing (about 3.9 GB server footprint, accepted) already serves this budget: read its constants first, state in the report how its per-update send and receive capacity meet max_clients x max_packets_per_update, and size the other drivers to the same per-update budget (their memory may be smaller where the kernel holds the buffers, never their capacity). Holding the budget is required; SUSTAINING that rate is NOT required for v1.0 (performance is the final milestone): the report states what each driver measured in a short run and names the bottleneck, and nothing is dropped for capacity below the budget; (3) RECEIVE IS AT LEAST 2X SEND (owner, 2026-10-20: \"Remember that this implies at least 2X receive side.\"): every driver's receive capacity per update (bank, ring, staging or completion budget, whichever the send path can fill) is at least 2 x max_clients x max_packets_per_update, which is 512,000 packet slots at 1000 clients with the default 256, sized at create from the config. The memory for it goes in the report (5); it is server-side, so more memory is fine. The receive side stays at least twice the new send capacity on every driver (the rule rocketnet-receive-budget-twice-send locks): update that rule's case for the new sizes, never weaken it; (4) red first: the NEW unit case drives a fake transport at max_clients=1000 with max_packets_per_update (256) packets queued per client in one update and asserts zero SEND_RING_FULL and every packet handed to the driver in that update; a second case does the light profile (one packet per client); both fail at BASE on the portable driver at the 257th packet, the failing line kept; it uses no real time, sockets or threads; (5) the server's memory at max_clients=1000 per driver, as counted by the allocator hooks goes in the report and in DRIVERS.md. More memory on the server is fine; the client allocates once. Memory is not a constraint (owner, 2026-10-20: \"Yes, it's just memory.\"): allocate the full send budget and the 2x receive side, and never shrink either to save memory; every new send and receive buffer is allocated through rocketnet_malloc / rocketnet_free (the caller's allocator from rocketnet_set_allocator, as in netcode), never raw malloc, calloc or new. NO EXCEPTIONS (owner, 2026-10-20): every allocation of regular memory goes through the user-provided alloc/free, including buffers the kernel later registers or needs page-aligned (XDP UMEM, the RIO region, io_uring buffer rings): over-allocate through rocketnet_malloc and align inside it. Only memory the kernel itself allocates and maps (io_uring SQ/CQ rings, xsk ring maps) is outside the hooks. List every allocation site that does not go through the hooks today with file:line, and fix the ones this card's drivers own; (6) a create whose config would need more than the platform can give fails create with a status, never a silent cap; (7) ./bin/test green in Debug on Linux, macOS and Windows (captain under winlock); planted bug: restore the 256 #define and show the NEW case red at its own check line; (8) TWO-MINUTE RULE: the 1000-client functional light run is OWED after landing, not run here.
TOOLCHAIN: cmake, make, liburing, on the members JOB.md names; Windows on captain under winlock; macOS on mini-m5-1
GATE-NEEDS: linux,macos,windows
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 180 minutes.

# Server send capacity is sized from the config, never a fixed 256

THE TASK. The portable server drops every packet past the 256th in an update, so a server with more than about 256 clients loses sends and drops clients. v1.0 serves 1000 clients on every platform and transport. Size every server driver's per-update send capacity at create to the owner's budget, max_packets_per_update (default 256) packets per client per update at 60 Hz (max_clients x max_packets_per_update), keep the receive side at least twice that, prove it red first with a unit case at 1000 fake clients, and record the memory cost.

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Match RakNet semantics; never invent API shape. No new public config field without saying so on a PATHS-PROPOSED line for the coordinator to rule on.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- Run CHECKLIST.md (the nova-sprint before-finish checklist) before every finish.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass.


RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
")
    (:id "rocketnet-load-light-profile-macos" :feature "macOS: 1000 light clients on msgx and portable" :cells (- n -) :note "after the payment milestone (owner 2026-10-20); latest work: no pushed branch" :brief "REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: rocketnet-load-light-profile-linux
SOURCE: TESTING.md (branch testing-md-v1, docs/testing.sexp) lists this cell as no card at sprint/rocketnet-proof 091204c9. Row: 1000 light clients / macOS. The light mode comes from rocketnet-load-light-profile-linux; this card runs it on the macOS transports. The owner, 2026-10-20: 1000 light players working on every platform and transport for v1.0.0.
PATHS: load.c,.github/workflows/nightly-soak.yml
START: load.c (read the usage comment, the light mode and the RLIMIT_NOFILE handling)
TEST: none because the gate runs as DONE-WHEN says and has no go or ctest name
DONE-WHEN: (1) on this member `./bin/load <n> 60 light` passes at 10, 100 and 1000 clients, in that order, for each server cell msgx and portable (ROCKETNET_SERVER_TRANSPORT pins it), with the criteria of the owner's light profile, functional and not load: 1000 clients, each updating at 10 Hz and sending one packet per update; pass means all 1000 connect, every client receives, nobody is disconnected, and reliable messages arrive in order; build up from a few clients to 1000; no throughput, bandwidth or stress target; the table in the report with connect time and the fd and thread counts at start and end, end equal to start; (2) red first: `./bin/load 1000 60 light drop-one` fails naming its criterion on msgx; (3) a failure is profiled (`sample <pid>`) before it is fixed, the fix minimal and in PATHS or reported with file:line; (4) nightly-soak.yml's soak-macos job gains one light run per cell, proven by running the step by hand
TOOLCHAIN: Apple clang from the Command Line Tools, cmake, make, Python 3, on the studio or mini-m5-1 (macOS)
GATE-NEEDS: darwin, arm64
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 120 minutes.

# macOS: 1000 light clients on msgx and portable

THE TASK. Run the load harness's light mode at 1000 clients on the macOS server transports msgx and portable, building up from 10, and run it nightly.
STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.


RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, the home directory or any volume root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
FIX (coordinator, 2026-10-20, owner deferral): under the owner's two-minute rule this card does NOT run its long soak, load, stress or repeat runs now. Scope it to what can be proven in short runs: the harness, workflow or code change the card adds, built and run in Debug at the exact head with each run under 2 minutes, a red-first proof where the card fixes a bug, and CHECKLIST.md. Then finish LAND, listing every long run as OWED (they become a separate card after v1.0.0-preview1). If the card has no change beyond the long run itself, finish LAND with only the docs or workflow wiring and the long run OWED. Never shorten a long run and call it a pass. Build on the latest pushed head.
")
    (:id "rocketnet-load-light-profile-windows" :feature "Windows: 1000 light clients on portable, iocp and rio" :cells (- - n) :note "after the payment milestone (owner 2026-10-20); latest work: no pushed branch" :brief "REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: rocketnet-load-light-profile-linux
SOURCE: TESTING.md (branch testing-md-v1, docs/testing.sexp) lists this cell as no card at sprint/rocketnet-proof 091204c9. Row: 1000 light clients / Windows. The light mode comes from rocketnet-load-light-profile-linux; this card runs it on the Windows transports. The owner, 2026-10-20: 1000 light players working on every platform and transport for v1.0.0.
PATHS: load.c,.github/workflows/nightly-soak.yml
START: load.c (read the usage comment and the light mode)
TEST: none because the gate runs as DONE-WHEN says and has no go or ctest name
DONE-WHEN: (1) on this member `bin/Release/load.exe <n> 60 light` passes at 10, 100 and 1000 clients, in that order, for each server transport portable, iocp and rio (ROCKETNET_SERVER_TRANSPORT pins it), with the criteria of the owner's light profile, functional and not load: 1000 clients, each updating at 10 Hz and sending one packet per update; pass means all 1000 connect, every client receives, nobody is disconnected, and reliable messages arrive in order; build up from a few clients to 1000; no throughput, bandwidth or stress target; each run's summary line in the report; handles and threads at the end within plus or minus 2 of those after connect; (2) red first: `load.exe 1000 60 light drop-one` fails naming its criterion on iocp; (3) a failure is profiled (wpr or a timed stack) before it is fixed, the fix minimal and in PATHS or reported with file:line; a red already held by rocketnet-windows-suite-stable is listed with that id; (4) nightly-soak.yml's soak-windows job gains one light run per transport, proven by running the step by hand
TOOLCHAIN: MSVC (Visual Studio), CMake, Python 3, on captain under winlock
GATE-NEEDS: windows
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 120 minutes.

# Windows: 1000 light clients on portable, iocp and rio

THE TASK. Run the load harness's light mode at 1000 clients on the Windows server transports portable, iocp and rio, building up from 10, and run it nightly.
STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.


RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, the home directory or any volume root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
FIX (coordinator, 2026-10-20, owner deferral): under the owner's two-minute rule this card does NOT run its long soak, load, stress or repeat runs now. Scope it to what can be proven in short runs: the harness, workflow or code change the card adds, built and run in Debug at the exact head with each run under 2 minutes, a red-first proof where the card fixes a bug, and CHECKLIST.md. Then finish LAND, listing every long run as OWED (they become a separate card after v1.0.0-preview1). If the card has no change beyond the long run itself, finish LAND with only the docs or workflow wiring and the long run OWED. Never shorten a long run and call it a pass. Build on the latest pushed head.
")
    (:id "rocketnet-macos-nightly-soak-green-at-base" :feature "macOS: the nightly soak job green at the branch" :cells (- n -) :note "after the payment milestone (owner 2026-10-20); latest work: 3bd59a0b on sprint/rocketnet-macos-nightly-soak-green-at-base.w4.g1.e0" :brief "RESULT: rocketnet-macos-nightly-soak-green-at-base sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: test
PRIORITY: high
STREAM: rocketnet-api
DEPENDS-ON: -
SOURCE: TESTING.md (branch testing-md-v1, docs/testing.sexp) lists this cell as no card at sprint/rocketnet-proof 091204c9. Row: Nightlies / macOS. The scheduled nightlies run only on main, which is far behind this branch; nightly-soak's soak-macos job has never run on this branch's code. rocketnet-macos-soak-per-transport (landed) ran its own soak matrix, not this job.
PATHS: .github/workflows/nightly-soak.yml
START: .github/workflows/nightly-soak.yml (read the soak-macos job)
TEST: none because the gate is the soak-macos job's own steps, run by hand as DONE-WHEN says
DONE-WHEN: (1) at BASE, on this member, every step of nightly-soak.yml's soak-macos job is run by hand as the workflow spells it, each command's exit code, run time and last lines kept; (2) each red's cause is named in the report with file:line, fixed in this commit when the fix is in a file PATHS names and otherwise carried as a proposed diff for its own card; a bound is never loosened; a red already held by rocketnet-macos-load-1000-clients-t is listed with that id; (3) red first: each step fixed in this commit is shown red at BASE and green at head; (4) at head every step exits 0 on this member, or the report names the proposed diff it waits on, the run time recorded
TOOLCHAIN: Apple clang from the Command Line Tools, cmake, make, Python 3, on the studio or mini-m5-1 (macOS)
GATE-NEEDS: darwin, arm64
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 150 minutes.

# macOS: the nightly soak job green at the branch

THE TASK. Run nightly-soak's soak-macos job by hand at BASE, fix each red at its cause, and show it green at head with its run time.
STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.


RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, the home directory or any volume root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
")
    (:id "rocketnet-example-soak-disconnect-regression" :feature "Example soak: disconnect under loss reaches the server again" :cells (n n n) :note "after the payment milestone (owner 2026-10-20); latest work: no pushed branch" :brief "RESULT: rocketnet-example-soak-disconnect-regression sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: fix
PRIORITY: critical
STREAM: rocketnet-api
DEPENDS-ON: -
SOURCE: worker report, 2026-10-20 19:25Z (rocketnet-example-ack-receipts-debug-assert w9): example_soak fails on Linux, macOS and Windows, in Debug and Release, at examples/soak.c check rocketnet_server_num_connected_clients( server ) == 0 (line 320). It fails the same way at BASE. Bisected to base commit 0500dc7a (rocketnet-r1-client-link-sim-and-memory-plan-e): its parent passes 3 of 3, and 0500dc7a fails 3 of 3. The soak example simulates 1% loss; since that commit, its two disconnect passes no longer leave the server empty.
PATHS: CMakeLists.txt,examples/soak.c,sdk/rocketnet_client_transport.cpp,sdk/rocketnet_client_io.h,sdk/rocketnet_client_protocol.cpp,sdk/rocketnet_server_protocol.cpp,sdk/rocketnet_api.cpp
NEW: test_unit_disconnect_under_loss.cpp
START: git show 0500dc7a; examples/soak.c (the disconnect passes); the client disconnect path under the link simulator
TEST: none because the unit suite's REGISTER_UNIT_TEST cases have no go or ctest name; run as DONE-WHEN says
DONE-WHEN: (1) The root cause in 0500dc7a is named: why a client disconnect under 1% simulated loss no longer reaches the server. Expected suspects: the link simulator now applies loss to the disconnect burst, or the burst size or resend behaviour changed. (2) Fix it in the SDK so that disconnect still reaches the server under loss, as before 0500dc7a: the disconnect burst (ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS) must survive 1% loss. Never weaken the soak example's check. (3) Red first: a unit case in the NEW file drives a client disconnect through a lossy fake link (no sockets, no real time, test clock from 1000.0) and fails at base at its own check line. (4) run_examples passes 11/11 plus example_soak, in Debug and Release on Linux, and on macOS (mini-m5-1 or studio) and Windows (captain under winlock); the known base build red (ROCKETNET_XDP_FRAME_SIZE) is owned elsewhere, so merge its fix locally only, to test. (5) ./bin/test is green. Two-minute rule.
TOOLCHAIN: cmake, make, on the members JOB.md names; Windows on captain under winlock; macOS on mini-m5-1
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 120 minutes.

# Example soak: disconnect under loss reaches the server again

THE TASK. Since 0500dc7a, example_soak leaves clients connected on the server after its disconnect passes under 1% loss, on every platform. Find what that commit changed in the disconnect path, fix it in the SDK without weakening the example, and prove it red first with a socket-free unit case.

STANDING RULES (owner, binding on every card).
- Make the smallest change, with no refactor. Unit tests use no real time, sockets or threads.
- Run CHECKLIST.md (the nova-sprint before-finish checklist) before every finish, with a cold read.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`.

RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
")
    (:id "rocketnet-r1-receipt-loss-after-its-own-deadline" :feature "Ack receipts: LOSS only after the receipt deadline" :cells (n n n) :note "after the payment milestone (owner 2026-10-20); latest work: sprint/rocketnet-r1-receipt-loss-after-its-own-deadline.w7.g1.e0" :brief "RESULT: rocketnet-r1-receipt-loss-after-its-own-deadline sha=000000000000 tier: pro
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: fix
PRIORITY: high
STREAM: rocketnet-api
DEPENDS-ON: rocketnet-r1-loopback-rtt-and-spurious-resends
LAND-AFTER: rocketnet-r1-tlc-gate-runs-on-sprint-branch,rocketnet-r1-uring-update-bounded,rocketnet-r1-rio-update-bounded,rocketnet-r1-serversession-model-matches-stop,rocketnet-r1-tla-ledger-current,rocketnet-r1-request-ladder-matches-spec,rocketnet-r1-production-token-minter-refuses-dead-tokens,rocketnet-r1-loopback-rtt-and-spurious-resends
SOURCE: holistic review round 1 at base 2803800 (six whole-system reviews and three re-reads, merged in v1-review/round1-merged.md). One reviewer, verified in code: UNRELIABLE_WITH_ACK_RECEIPT reports LOSS whenever the ack takes longer than the resend rto (sdk/rocketnet_window_ack.cpp ~708: now >= send_time + rto gives LOSS; the late ack is then suppressed, ~478-487), and rto is clamped to rto_max 250 ms (sdk/rocketnet_layers.h ~1468-1473). Any path with RTT >= 250 ms, or a LAN peer updating at 10 Hz, gets LOSS for delivered messages; sdk/rocketnet.h ~302-305 states no deadline. RakNet reports receipt loss on its own timeout, independent of the resend timer; owner ruling: match RakNet.
PATHS: sdk/rocketnet_window_ack.cpp,sdk/rocketnet_window_ack.h,sdk/rocketnet_layers.h,sdk/rocketnet.h,standard/MESSAGES.md,tla/RNReceipts.tla,tla/README-RNReceipts.md,tla/CASES.tsv,tla/RUNS.tsv,CMakeLists.txt,tla/MCRNReceipts.cfg,sdk/rocketnet_endpoint.cpp,docs/CHANNELS.md,tla/COVERAGE.tsv,tla/MCRNReceiptsBrokenLastIndexAck.cfg,tla/MCRNReceiptsBrokenTwoOutcomes.cfg,tla/MCRNReceiptsBrokenReliableLoss.cfg,tools/check_tla_ledger_merge.py,tools/check_test_environment.py,functional/posix_lifecycle_stress.cpp
NEW: test_unit_receipt_loss_deadline.cpp,tla/MCRNReceiptsBrokenLossAtRto.cfg,docs/SPEC-CARD-CONTRACT.md
START: sdk/rocketnet_window_ack.cpp (read 470-490, 690-720), tla/RNReceipts.tla (read)
TEST: none because the unit suite's REGISTER_UNIT_TEST cases have no go or ctest name; the cases are in the NEW file, run as DONE-WHEN says
DONE-WHEN: (1) red first at BASE: a unit case on two endpoints with the test clock and a 300 ms one-way delay, and another with a 1 ms path and a peer acking every 100 ms, each sends UNRELIABLE_WITH_ACK_RECEIPT and gets LOSS for a message the peer received; (2) receipt loss is decided by a receipt deadline independent of, and never below, the resend rto: a fixed constant stated in the header and in standard/MESSAGES.md, at least the reliable timeout's order of magnitude as RakNet's receipt timeout is; an ack arriving before the deadline gives ACKED; (3) exactly one outcome per receipt still holds (the existing receipt cases pass); (4) RNReceipts.tla states LOSS only after the deadline, with a reversed witness (LOSS at rto) failing it; records only via /opt/tla/tlacheck merge --keep; (5) the cases of (1) pass at head; ./bin/test and ./bin/functional green on Linux
FIX: Owner ruling wins over older lines in this brief: the receipt deadline is the shared RakNet RTO (first sample deviation = rtt, 5% smoothing, min(2*SRTT + 4*dev + 30 ms, max), 2 s unknown, no backoff) with the optional public config field rto_min_ms, default 0, range 0..60000; ignore the superseded 100 ms floor line and the fixed-constant wording in DONE-WHEN (2). Attempt 7 (head d9f3fdfd) has that shape. Unmet: (a) call rn_window_ack_compute_rto as rocketnet-r1-loopback-rtt-and-spurious-resends defines it, over the shared srtt/rttvar, with no second RTO function and no rsrtt/rdev fields, and keep the receipt deadline never below the resend rto; (b) ./bin/test red at mo_end_to_end_up: this card now needs the loopback card and starts after it lands; if mo_end_to_end_up is red at that BASE tip it is pre-existing and goes in the report as a finding for the loopback card, never loosened here; if it is green at BASE and red at your head, fix it at its cause. Runs over 2 minutes are OWED under the two-minute rule, never a HOLD cause.
TOOLCHAIN: cmake, make, java and /opt/tla on a Linux member JOB.md names
GATE-NEEDS: linux, tla
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 120 minutes.

# Ack receipts: LOSS only after the receipt deadline

THE TASK. An unreliable message with an ack receipt is reported lost whenever its ack takes longer than 250 ms, though it was delivered. Decide receipt loss on its own deadline, as RakNet does, documented and modelled, red first with a delayed-ack case.

STEP 1. After any model edit, run TLC for its group and refresh the records with tlacheck merge --keep (tla/RUNS.tsv is written only by it).

STANDING RULES (owner, binding on every card).
- Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0. Anything that needs a real socket, a kernel object, a child process, a thread timing or the wall clock is a functional test, never ./bin/test.
- Make the smallest change, with no refactor.
- Never touch the space game's baseline or delta code.
- Never run `nova-sprint friend stage`.
- Finish LAND with `--head <sha>`; finish HOLD with `--failed`. A red is pre-existing only if reproduced at BASE in the same environment.
- The report starts with `Verdict: LAND|HOLD` on line 1 and `Head: <sha>` on line 2.
- Never read outside the job's working directory.
- Never loosen a Release bound, a protocol timeout or a test assertion to make a run pass; a failure found is a finding, fixed at its cause or reported.


RULES.
Work only in the job directory this card names.
Never force-push or rebase a shared branch.
Never kill a process you did not start.
Never start a server on this machine.
No `rm -rf` outside the job directory.
Report what was not done.
Work only in the staged checkout JOB.md names; do not clone a second copy. Commit on its own branch; touch no other branch. No gh, no push: JOB.md's finish does both, from outside the wall. Never merge into BASE.
Build and test only on the member JOB.md names, never the coordinator's machine. Never search /, the home directory or any volume root; stay inside the checkout and the job directory.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install. A named tool that is missing on the member is reported as not-done with the missing component named, verdict HOLD.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.

ATTRIBUTION: each commit message ends with the trailer line Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, and a By: line naming the worker who does this attempt above it; a model name is never a By:.
CARRY: rocketnet-r1-receipt-loss-after-its-own-deadline attempt 7 head=d9f3fdfd1007aec4e652b00b44f2ca48a67657ef
CARRY: READER (vs RakNet CCRakNetSlidingWindow.cpp) on head d7355860: (A) first sample must set deviation = rtt (RakNet OnAck unset branch, CC.cpp:210-222), not rtt*0.5 (window_ack.cpp:614-615): first RTO is 6*rtt+30ms (300 ms -> 1.83 s); fix test_unit_receipt_loss_deadline.cpp:353 and :383-385 expected values (100 then 200 ms -> rsrtt 0.105, rdev 0.1, deadline 0.64 s). (B) drop the extra floor at window_ack.h:155: RakNet has no minimum (CC.cpp:294/298 commented out; RAKNET-RELIABILITY.md:690-692); update the test at :401-420. (C) ONE RTO function: rocketnet-r1-loopback-rtt-and-spurious-resends owns rn RakNet RTO (plain, no receipt-only floor); this card calls it, landing after that card. PATHS-PROPOSED tla/MCRNReceiptsBroken*.cfg are legitimate.
CARRY: OWNER 2026-10-20: match RakNet and be better where it helps. The RTO is RakNet's formula (first sample deviation = rtt; 5% smoothing; min(2*SRTT + 4*dev + 30 ms, max); 2 s unknown; no backoff) PLUS a minimum floor of 100 ms in the shared RTO function, documented as our deliberate improvement over RakNet. That replaces the earlier no-floor instruction. Receipt loss uses that same function with its 100 ms floor; no extra receipt-only floor.
CARRY: OWNER 2026-10-20 (supersedes the 100 ms floor line): the RTO floor is a config option, default 0.0 (no floor), which matches RakNet exactly; a caller may set it, for example to 0.1 s. One config field (one way to do it), validated >= 0, documented in the header; tests pin both the default (no floor, RakNet values) and a set floor.
CARRY: SHARED RTO HELPER (owned by rocketnet-r1-loopback-rtt-and-spurious-resends): rn_window_ack_compute_rto( srtt, rttvar, configured_min, configured_max ), using the public config field rto_min_ms (default 0, range 0..60000). Receipt loss calls that helper; don't write a second RTO function. Land after loopback-rtt.
")
    (:id "rocketnet-format-netcode-style-v1" :feature "rocketnet-format-netcode-style-v1" :cells (n n n) :note "after the payment milestone (owner 2026-10-20); latest work: no pushed branch" :brief "RESULT: rocketnet-format-netcode-style-v1 sha=000000000000 tier: heavy
REPO: mas-bandwidth/rocketnet
BASE: sprint/rocketnet-proof
KIND: chore
PRIORITY: blocker
STREAM: rocketnet-api
DEPENDS-ON: rocketnet-driver-renamed-transport
LAND-AFTER: rocketnet-destroy-sends-disconnect,rocketnet-api-polish-v1-followups,rocketnet-check-test-environment-green,rocketnet-client-send-drops-until-connected,rocketnet-example-ack-receipts-debug-assert,rocketnet-examples-free-ports,rocketnet-functional-cookie-expiry-uses-test-clock,rocketnet-functional-free-ports,rocketnet-functional-tokens-use-the-test-clock,rocketnet-lan-1000-client-soak-matrix,rocketnet-linux-hard-killed-peer-never-stalls-receive,rocketnet-linux-lifecycle-stress,rocketnet-linux-load-1000-clients,rocketnet-linux-soak-per-transport,rocketnet-linux-socket-error-paths,rocketnet-linux-transport-matrix-repeat,rocketnet-macos-client-timing-under-app-nap,rocketnet-macos-hard-killed-peer-never-stalls-receive,rocketnet-macos-lifecycle-stress-t,rocketnet-macos-load-1000-clients-t,rocketnet-macos-sanitizers-and-leaks,rocketnet-macos-soak-per-transport,rocketnet-macos-socket-errors-and-enobufs-t,rocketnet-macos-transport-matrix-repeat,rocketnet-macos-universal-and-xcode-t,rocketnet-macos-wildcard-bind-check,rocketnet-network-thread-priority-as-space,rocketnet-send-budget-env-refused-create-leaks-nothing,rocketnet-server-create-ex-test-installs-its-allocator,rocketnet-v1-hygiene-fallout,rocketnet-v1-hygiene-t,rocketnet-win-app-verifier,rocketnet-win-libfuzzer-clang-cl,rocketnet-win-lifecycle-stress,rocketnet-win-load-1000-clients,rocketnet-win-msvc-asan,rocketnet-win-nightlies-on-fleet-runner,rocketnet-win-receive-backlog-connect-wait,rocketnet-win-soak-per-transport,rocketnet-win-socket-error-paths-t,rocketnet-win-toolchain-matrix,rocketnet-win-transport-matrix-repeat,rocketnet-windows-client-overlapped-send,rocketnet-windows-udp-connreset-never-stalls-receive,rocketnet-xdp-drops-first-fragment
SOURCE: the owner, 2026-10-19: use the clang-format configuration derived from his own code to format rocketnet and keep it tidy. This card runs only after the held sentinel rocketnet-quiet-for-reformat is released, at a quiet point: a whole-tree reformat conflicts with every rocketnet branch in flight. The coordinator releases the sentinel when rocketnet's open streams have drained, or right after the M1 milestone.
PATHS: sdk/**,*.c,*.cpp,test/*.h,test/driver_extension/**,test/xdp_host/server_xdp_host.c,test/xdp_host/server_xdp_host.h,functional/**,fuzz/**,bench/**,examples/**,tools/**,proofs/**,server_xdp/**,docs/mrdocs/**,.github/workflows/static.yml
SHARED: .github/workflows/static.yml
NEW: tools/format/same_tokens.sh
TEST: none because the change is formatting only; the proof is the raw-token comparison script (token streams identical before and after for every changed file), the format-check target green, and the unit and functional suites green
DONE-WHEN: (a) one commit whose diff is formatting only: `sh tools/format/same_tokens.sh <BASE sha>` prints `ok: <n> files, token streams identical` with n equal to the count of changed C/C++ files and exits 0, and at BASE with one planted non-format edit in a scratch copy (a renamed identifier in one file) it prints that file and exits 1 (the plant is not committed); (b) `make format-check` prints `ok: <n> files formatted` and exits 0, and `sh tools/format/test_check_format.sh` still passes; (c) nothing under sodium/, test/xdp_host/linux/, test/xdp_host/bpf/, test/wycheproof/ or any file check_format.sh skips as generated changes; (d) object files compared where the build allows it: build BASE and head with the same flags into separate build trees and report how many object files are byte-identical, naming the ones that differ and why (assert or log macros carrying __LINE__ are the expected reason); (e) `make && ./bin/test` and `./bin/functional` are green on the Linux member; (f) .github/workflows/static.yml gains a `format` job on `[self-hosted, Linux, X64, fleet]` running `sh tools/format/test_check_format.sh` and `make format-check`, never on a GitHub-hosted runner.
TOOLCHAIN: clang-format-18 and clang-18 (/usr/bin, Ubuntu packages clang-format-18 and clang-18), GNU make, cmake and a C++17 compiler on the Linux member JOB.md names; libsodium is vendored under sodium/
GATE-NEEDS: linux
STOP: when DONE-WHEN holds and the commit is on your branch, or the deadline is reached; then report, with not-done named
DEADLINE: finish within 120 minutes.

The REPO: and BASE: lines are the repository and the branch the work starts from and lands on: the member stages REPO: at BASE:, and nova-sprint land merges the card's head onto BASE: (land --base stands in for a card naming no BASE:, land --repo-dir for one naming no REPO:).
You are a child of the coordinator: one task, one worktree, one branch, unattended. This card is the whole of the task and it stands alone in front of a stranger; nothing outside it is owed to you.
Deadline: finish within 120 minutes; the judgment of a card that runs past it is the coordinator's, so report what you have with the verdict not-done rather than push past it.
Libraries considered: clang-format 18 does the formatting with the committed .clang-format; clang 18's raw lexer (`clang-18 -cc1 -dump-raw-tokens`, which lexes one file without preprocessing, so __LINE__ and includes do not matter) gives the token stream for the no-semantic-change proof; diff and sed compare them. No script of more than about thirty lines is needed.

THIS REPOSITORY. mas-bandwidth/rocketnet: a C++ UDP transport library with a C API (sdk/rocketnet.h), its tests at the root (test*.cpp, test_unit_*.cpp), functional/, fuzz/, bench/, examples/, tools/ and proofs/. `make` builds into bin/ through CMake under build/; `./bin/test` is the unit suite and `./bin/functional` the functional suite. The landed card rocketnet-clang-format-from-netcode added .clang-format at the root (derived from the owner's netcode and Network Next SDK style; its header comment lists what it cannot express), DisableFormat files in the vendored trees, tools/format/check_format.sh (which files are checked and which are skipped, read it), tools/format/test_check_format.sh and a `make format-check` target that CI does not run yet. .github/workflows/static.yml (read) is the per-commit static gate, already on the fleet runners.

THE TASK.
(1) tools/format/same_tokens.sh <base-rev>: sh, set -eu; for each C/C++ file `git diff --name-only <base-rev>` lists, dump the raw tokens of the base version (`git show <base-rev>:<f>` into a mktemp file inside the job directory) and of the working version with `clang-18 -cc1 -dump-raw-tokens -x c++ <file>` (stderr), drop the location and flag columns (keep each token's kind and spelling), and compare; print `<file>: tokens differ` per mismatch; end with `ok: <n> files, token streams identical` and exit 0, or exit 1. Comments are not tokens in raw mode, so reflowed comments pass; a changed identifier, literal or operator fails.
(2) Run `clang-format-18 -i --style=file` over exactly the files check_format.sh checks (never sodium/, test/xdp_host/linux/, test/xdp_host/bpf/, generated files). Read the result: where the formatter makes something clearly worse (a hand-aligned table, a long initializer, an ASCII diagram in code), wrap that region in `// clang-format off` / `// clang-format on` with no other change, and list each such region in the pull request body; keep these few.
(3) .github/workflows/static.yml: a second job `format`, `runs-on: [self-hosted, Linux, X64, fleet]`, timeout 10 minutes, steps: checkout, `sh tools/format/test_check_format.sh`, `make format-check`. No hosted fallback install step beyond the pattern the static job already has.

STEP 1. Confirm `uname -s` is Linux, `clang-format-18 --version` and `clang-18 --version` print 18.x; if either is missing, stop and report HOLD naming the package. cd into the staged checkout JOB.md names and run git log --oneline -1; it is a NEW worktree on the branch this card names; record the BASE sha. Confirm .clang-format and tools/format/check_format.sh exist; if not, the card that adds them has not landed: stop and report HOLD.
STEP 2. Red first: write tools/format/same_tokens.sh, run `make format-check; echo exit=$?` at BASE and keep the count of unformatted files; prove the token check catches a real edit by planting one renamed identifier in a scratch copy, running same_tokens.sh, keeping the failing line, and reverting the plant.
STEP 3. Build BASE into build-base/ (`cmake -B build-base -DCMAKE_BUILD_TYPE=Release && cmake --build build-base -j8`) for the object comparison, then do THE TASK (2) and (3).
STEP 4. Run the gate: `sh tools/format/same_tokens.sh <BASE sha>`; `make format-check`; `sh tools/format/test_check_format.sh`; build head into build-head/ with the same flags and compare object files (`cmp` each pair under CMakeFiles, report identical/total and the differing names); then `make && ./bin/test` and `./bin/functional`, reading the last lines. When a test fails, name its file and say whether that file was changed by your work (yours) or is unchanged (already red at BASE: run the same test on the unchanged base to say so), and report that line first. Remove build-base/ and build-head/ before committing; they are never committed.
STEP 5. Commit on your own branch as `rocketnet-format-netcode-style-v1: format the tree with clang-format 18` with the trailer, the reformat and the static.yml job in that one commit, tools/format/same_tokens.sh included. Nothing reaches the forge from inside the wall: in the job the git shim records a push, the pull request is the finish JOB.md names, and the member makes both, against the BASE: line's branch, from outside the wall when the card finishes. The pull request body states the diff stat, the same_tokens.sh line and its planted red line, the object comparison counts, the clang-format off regions, the test lines, and what was not done.
STEP 6. End as JOB.md says: where JOB.md ends the card with its pull request, that is the end and there is nothing else to write, the gate's lines in the pull request body; where it asks for RESULT.md, write it in JOB.md's shape (head, branch, verdict, gate, output, report).

STANDING RULES.
Unit tests use no real time, no sockets, no network and no outside services. Time is an argument, and a test clock starts at 1000.0.
Make the smallest change, with no refactor: whitespace and line breaks only, plus the few clang-format off/on markers.
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
Anything that uses real time, sockets or the network is a functional test and belongs in ./bin/functional, never ./bin/test.
No key, and nothing derived from a key, in any log line, test output, comment or report.
No dependency the card does not name: no package-manager fetch, no network install.
No names of people, machines or customers in code, comments or docs; docs and comments in the present tense.
Never format vendored or generated code: sodium/, test/xdp_host/linux/, test/xdp_host/bpf/, test/wycheproof/, and every file check_format.sh skips.
Touch only the files PATHS and NEW name; a fix that needs another file goes into the report as a proposed diff, not a commit.
\"Not done\" is a welcome report; a green claim you did not run is not.
")))
  (:id "release" :title "Release and process"
   :columns platforms
   :rows
   ((:feature "Release hardening flags" :cells (n n n) :note "fe7966f adds tools/check_hardening.py; no workflow runs it, so no platform has a run")
    (:feature "Reproducible vendored sources" :cells (y n n) :evidence "Linux: ci.yml reproduces the committed sodium pair on the Linux runners; 23d4c40 sodium; f6457b5 XDP archive" :note "reproducible release dependencies as a whole are still owed")
    (:feature "First integrating game vendors the SDK and proves it (release gate)" :cells (n n n) :note "held. The ultimate soak: vendored byte for byte, the game server soaks it under real conditions. The game loop pegs to real time with drop-frame detection, so update stays non-blocking with time passed in and bounded walks, and nothing in the transports blocks or takes unbounded time in update")
    (:feature "Supported-platform policy" :cells (n - -) :note "34406ce docs/PLATFORMS.md; 1952f918 wires tools/check_platform_policy.py in .github/workflows/ci.yml:427; tools/check_platform_policy.py exits 0 (4 supported, 9 nightly, 3 unsupported rows match); a passing CI run is owed")
    (:feature "Security disclosure and release process" :cells (y - -) :evidence "Linux: a document; 6795ac1 SECURITY.md, docs/RELEASING.md" :note "tools/check_release_doc.py passes by hand; no workflow runs it")
    (:feature "Independent review of the connection layer repair diff, received" :cells (n - -) :note "the repair of findings #143 to #159; a review request sent is not a review received or an approval")
    (:feature "The old to-do and readiness files retired into this roadmap" :cells (y - -) :evidence "Linux: a document; rocketnet-retire-todo-and-readiness" :note "TODO.md and TRANSPORT_READINESS.md deleted; every open item is a row here")))))
