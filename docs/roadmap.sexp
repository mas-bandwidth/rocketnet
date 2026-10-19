; RocketNet roadmap data. ROADMAP.md is rendered from this file by
; tools/roadmap/render.py (make roadmap); make roadmap-check fails when the
; committed page is not what this renders.
;
; An area declares :columns transports (one column per server transport, in
; the order the automatic ladder ranks them) or :columns platforms (Linux,
; macOS, Windows). A row has one cell per column: y (done and tested there),
; n (not done, or not proven at this base) or - (does not apply). Every y cell
; carries evidence: a CI job that runs there, a test named, or the suite that
; covers it. Evidence stays here; the page shows only the marks.
;
; Every push runs the unit and functional suites (./bin/test, ./bin/functional,
; test_lifecycle) and the happy path on Linux, macOS and Windows (ci.yml), so
; a row those suites test is y on all three. A row only a Linux job checks (a
; sanitizer, the nightly soak, the examples, a dst run) is y on Linux and n on
; macOS and Windows. A document, a model or a proof is checked on Linux and is
; - on macOS and Windows.
(:title "RocketNet roadmap"
 :text ("Every feature RocketNet has, and every one still to come, by area: a column per transport where a feature is per transport, and a column per platform where it is not. ✅ is done and tested there; ❌ is not done, or not yet proven there; — does not apply. Each table's complete row is the share of ✅ among the cells that apply. Items marked (later) are deliberately after the current work, by the owner's call. An area marked (next milestone) is the next milestone, by the owner's call.")
 :areas
 ((:id "handshake" :title "Connect handshake and security"
   :text "The secure connect of standard/TRANSPORT.md: netcode 1.02 rebuilt on public keys, with every refusal counted."
   :columns platforms
   :rows
   ((:feature "Public-key connect tokens: mint, verify and open in the standard's order" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test and ./bin/functional")
    (:feature "Deterministic connect token vectors for minters in other languages" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "6-byte prefilter header with a per-title filter value and epoch" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test and ./bin/functional")
    (:feature "Cookie gate: bounds, signature, expiry and address checked" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Cookie key rotates on its period" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Per-connection keys with forward secrecy, secrets wiped on every exit" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Sealed packets with every refusal proven" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Used-token history and one token, one live connection" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test and ./bin/functional")
    (:feature "Replay window, cleared whole by a long sequence skip" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Release builds refuse development keys" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Oversize datagrams are dropped, never truncated" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Public API is NULL-safe at the boundary" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "The token is bound at response open" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Development keys are off by default and warn on every start" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/functional")
    (:feature "Session layer: identity auth with a backend, rolling magic, keyed filter value (later)" :cells (n n n))))
  (:id "connection" :title "Connections and the transport"
   :text "The unreliable transport the message layer runs over: connection state, slots, keep-alives, fragments, acks and the server loop."
   :columns platforms
   :rows
   ((:feature "Client connection state machine with every timeout pairing" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Server client slots from a free-slot stack" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Keep-alives paced and suppressed by traffic" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Large payloads fragmented and reassembled" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Fragments accepted only on a connected slot" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Server transport entries carry a connection id" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Per-packet acks with delivery notification" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Ack history that survives a long lossy path" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Frame sync: a fixed-rate server loop with priority and drop detection" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/functional")
    (:feature "Counters on every path, each classified, conserved and named by a test" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")))
  (:id "reliability" :title "Reliability modes"
   :text "The eight RakNet reliabilities, per send, over the transport's per-packet acks."
   :columns platforms
   :rows
   ((:feature "UNRELIABLE" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "UNRELIABLE_SEQUENCED" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "RELIABLE" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "RELIABLE_ORDERED" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "RELIABLE_SEQUENCED" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "UNRELIABLE_WITH_ACK_RECEIPT" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "RELIABLE_WITH_ACK_RECEIPT" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "RELIABLE_ORDERED_WITH_ACK_RECEIPT" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")))
  (:id "messages" :title "Channels, sending and memory"
   :text "The message layer of standard/MESSAGES.md: channels, priorities, budgets and one fixed region per connection."
   :columns platforms
   :rows
   ((:feature "Zero-copy send: start message, write in place, finish message or abort message" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Receive and release messages as views" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "N channels with per-record dispatch" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Large reliable messages split into blocks and delivered whole" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Small messages aggregated into packets" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Send budget in kbps per connection" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Resend delay from the measured round trip, block chunks paced by it" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "A 1400-byte game message fits one packet" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "One fixed region per connection, no allocation after create" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Caller-supplied memory block" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Memory exhaustion kicks only that connection" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Hostile records and chunks refused" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "CONFIG announcement: both ends prove the same configuration" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Send one message to every connected client, a start message and finish message per slot" :cells (y y y) :evidence "Linux, macOS and Windows: ci.yml runs ./bin/test")
    (:feature "Flows: server-side subscription groups (later)" :cells (n n n))))
  (:id "transports" :title "Server transports"
   :text "The server socket transports, selected at runtime by the per-endpoint ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT. Loopback is pin-only."
   :columns transports
   :rows
   ((:feature "Server transport available" :cells (y y y y y y y y) :evidence ((xdp "the xdp rows of the transport matrix") (msgx "the msgx row runs on macOS") (gso "test_server_gso_wire_ipv6, test_server_gso_wire_dual_stack") (io_uring "test_server_uring_probe, test_server_uring_wire_ipv4") (portable "ci.yml runs ./bin/functional with the portable transport") (iocp "test_server_iocp_probe") (rio "rocketnet_test_server_rio_probe, rocketnet_test_server_rio_wire_ipv4") (loopback "the loopback rows of the transport matrix")))
    (:feature "Selectable by pin at runtime" :cells (y y y y y y y y) :evidence ((xdp "rocketnet_test_server_driver_pin_xdp") (msgx "rocketnet_test_server_driver_pin_msgx") (gso "rocketnet_test_server_driver_pin_gso") (io_uring "test_server_driver_pin_io_uring") (portable "rocketnet_test_server_driver_pin_portable") (iocp "rocketnet_test_server_driver_pin_iocp") (rio "rocketnet_test_server_driver_pin_rio") (loopback "test_driver_pin_only, test_driver_env_pins_loopback")))
    (:feature "IPv6 and dual-stack binds" :cells (n n y y y n y n) :evidence ((gso "test_server_gso_wire_ipv6, test_server_gso_wire_dual_stack") (io_uring "test_server_uring_wire_ipv6, test_server_uring_wire_dual_stack") (portable "test_transport_ipv6_loopback") (rio "rocketnet_test_server_rio_wire_ipv6, rocketnet_test_server_rio_wire_dual_stack")))
    (:feature "Receive coalescing by the kernel, split by stride" :cells (- n y n n y n -) :evidence ((gso "the gso transport enables UDP_GRO per socket") (iocp "rocketnet_test_server_iocp_uso_uro_probe")))
    (:feature "Registered I/O (RIO) on Windows" :cells (- - - - - - y -) :evidence ((rio "rocketnet_test_server_rio_wire_ipv4, rocketnet_test_server_rio_wire_ipv6")))
    (:feature "Kernel filter counters through the transport counter surface" :cells (y - - - - - - -) :evidence ((xdp "prefilter_counters_map_each_kernel_counter, prefilter_counters_xdp_driver_reports_them")))
    (:feature "Kernel replay and host replay map" :cells (y - - - - - - -) :evidence ((xdp "test_unit_xdp_host_replay_table")))))))
