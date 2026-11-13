; The RocketNet roadmap: the data ROADMAP.md is generated from.
; Same shape as nova-sprint's docs/roadmap.sexp: data, never evaluated.
; Pull requests edit only this file. Never edit or regenerate ROADMAP.md in a pull request:
; the roadmap workflow (.github/workflows/roadmap.yml) runs `make roadmap` on each push to
; main that changes this file and commits ROADMAP.md. Pull requests run `make roadmap-check`,
; which refuses a file that does not parse or reuses an id. `make roadmap` (NOVA_SPRINT=<a
; nova-sprint checkout>) previews the page locally; do not commit the result.
; Layout: exactly one blank line between items, so two pull requests that remove neighboring
; items touch separate hunks and merge cleanly. To remove an item, delete its lines only and
; leave the blank lines around it.
(roadmap "v1"
 :title "RocketNet roadmap"
 :text "Planned work for RocketNet that is not on main. Work lands on main before new work starts (Glenn, 2026-10-31). Audit item numbers refer to FABLE-AUDIT.md, the 12:05 PDT edition of 2026-10-31. Branch names are plain branches holding started work to resume from. Landed work is in git. Pull requests edit only docs/roadmap.sexp; this page is regenerated on main."

 :groups
 (
  (group "audit-remaining" :title "Audit items not yet fixed"
   :text "Suspects from FABLE-AUDIT.md that nothing on main fixes. Each is verified at the cited line first; a confirmed defect gets a lock-in test and a negative control.")
  (group "quiet-host" :title "Investigations that need a quiet host"
   :text "Findings that only a quiet fleet host can measure or reproduce.")
  (group "proof-debt" :title "Existing nightly proof debt"
   :text "Incomplete proof measurements remain explicit failures under the existing nightly workflows. These are distinct from the completed audit correspondence repairs.")

  (group "after-v1-0-1" :title "Found while preparing v1.0.1"
   :text "Defects, gaps and measurements found by the v1.0.1 review and test campaign that a point release does not carry. Each is checked at the cited code first.")
  (group "trace-check" :title "Trace check: gaps the README declares"
   :text "What proofs/tla/README-TraceCheck.md says the trace check trusts rather than enforces (v2, 2026-11-12, after three cold reads). Each item says what enforcing it would take.")
  (group "decisions" :title "Decisions for Glenn"
   :text "Choices that change behavior, remove machinery, or touch repository and fleet settings.")
 )

 :items
 (




















  (item "tlc-timeout-debt" :group "proof-debt" :area "proofs"
   :title "Eleven recorded TLC benchmark timeouts"
   :text "The committed RUNS.tsv records FAIL/124 at the 110-second limit for the eleven declared timeout-debt cases in CASES.tsv: ClientConnection LArbitrary, LCrash, SNone, SAlternate, SBurst and SArbitrary; ReplayWindowMid; RNEndpointTwoWay and RNEndpointPartial; RNClientIoDecrypt; and SessionAcksLive. The TLC records workflow checks each only to that bounded record; the weekly tlc-debt.yml runs each to its declared result as its own job, two at a time, and keeps its evidence. Failure, cancellation or resource exhaustion is not a proof. Fresh measured records require a reviewed commit; the workflow does not commit them automatically."
   :date "2026-11-01" :origin "proofs/tla/CASES.tsv and RUNS.tsv; existing TLC nightly; #833")

  (item "ack-proof-hold" :group "proof-debt" :area "proofs"
   :title "ACK run leaf has no symbolic proof; bound 8 is checked exhaustively"
   :text "ESBMC 8.5 cannot encode rocketnet_ack_apply_run at any run bound: each write through a pointer with a symbolic offset into the 260 KB endpoint (the visit's record->acked) becomes a byte-level update of the whole object, and bound 1 already stops Out of memory at 150 GB (proofs/cbmc/INVENTORY.md, ACK run measurements). Z3 answered unknown every night 11-01..11-12. Since 2026-11-12 the Nightly ACK proof runs ack-run-exhaustive-8 instead: every ring position, run length 1..8, ack pattern and highest offset, in three sequence blocks, with a negative control, a vacuity plant and four planted bugs that must each fail. That is exhaustive native testing, not a proof; blocks between 1 and the last rest on a translation argument, and lengths 9..4096 are not covered. A symbolic proof needs a verifier that keeps &a[i] as an index (CBMC does, but cannot parse the C++17 leaf) or an index-form write in the visit, which is an SDK change."
   :date "2026-11-12" :origin "proofs/cbmc/INVENTORY.md; .github/workflows/nightly-ack-proof.yml; #797")

  (item "windows-reflush" :group "audit-remaining" :area "rocketnet"
   :title "Windows iocp and rio re-flush"
   :text "Audit 88 and 124: measure on captain, then add re-flush counters if the measurement supports it."
   :date "2026-10-31" :origin "Audit items 88, 124")


  (item "load-md-generated" :group "audit-remaining" :area "docs"
   :title "Generate LOAD.md's byte tables"
   :text "The page's exact byte figures go stale with every merge. Generate them, with a CI check that the page matches the generator."
   :date "2026-10-31" :origin "#721 follow-on")

  (item "audit-follow-on-proposals" :group "decisions" :area "tools"
   :title "Optional audit follow-on proposals"
   :text "The concrete low-severity docs, standard, test and tooling mismatches are repaired. Four audit suggestions introduce new requirements: 148 extends the make-jobs policy to cmake --build --parallel; 185 cross-checks each t8 field-table sum against its summary row; 223 adds in-loop protocol assertions to the structured receive fuzzer; 226 changes the socket-error loopback burst/deadline. Evaluate these separately; they are not unresolved violations of the existing gates."
   :date "2026-11-01" :origin "FABLE-AUDIT.md items 148, 185, 223, 226; #835, #837, #838, #839")

  (item "xdp-receive-loss" :group "quiet-host" :area "rocketnet"
   :title "Loaded-host XDP receive loss report: not reproduced"
   :text "The requested quiet reproduction is complete. At SDK source 3e8e4d17 on 2026-11-01, the existing 1000-client, 60-second XDP cell passed with host and guest both on 7.0.0-38-generic: 600000 sent/received, zero drops, and descriptor/thread baselines restored. The earlier 7.0.0-34 guest pass is separate evidence. The reported loss under host load 170 did not reproduce; its cause remains unknown. This controlled native observation establishes neither an SDK fix, a per-frame-budget result, nor a green GitHub workflow run."
   :date "2026-10-31" :origin "Audit item 41; quiet/current-kernel observation 2026-11-01")




  (item "decision-resend-cap" :group "decisions" :area "rocketnet"
   :title "Cap the single-packet resend delay at timeout/4"
   :text "Audit 16: with message_resend_delay_max_ms above reliable_message_timeout_ms/4, a single-packet record gets fewer than four sends before timeout. Recommended: cap it as chunks are. Then both resend classes share one delay, and the two resend queues could collapse to one; that removes machinery."
   :date "2026-10-31" :origin "Audit item 16")



  (item "decision-unit-env" :group "decisions" :area "tests"
   :title "Unit tests and the process environment"
   :text "Unit cases still reach the environment through the public creates, safe only because the runner clears every name the SDK reads. Stricter: route about 320 creates through the injected-environment variants. Recommended: keep the runner boundary. Also move the XDP gateway address read from socket open to create."
   :date "2026-10-31" :origin "#794")

  (item "branch-protection" :group "decisions" :area "github"
   :title "Branch protection on main"
   :text "No ruleset requires any check on main (audit 44)."
   :date "2026-10-31" :origin "Audit item 44")

  (item "apply-main-ruleset" :group "decisions" :area "github"
   :title "Apply the main ruleset before the first outside pull request"
   :text "Apply .github/rulesets/main.json on GitHub before the first outside pull request. Left off for now (owner, 2026-11-12)."
   :date "2026-11-12" :origin "Owner, 2026-11-12")

  (item "frame-budget-runner-label" :group "decisions" :area "fleet"
   :title "A runner label for the nightly frame budget held only by amd-2 and amd-3"
   :text "rocketnet-quiet also covers hetzner2; the frame budget is a wall-clock gate that compares only on one machine class."
   :date "2026-10-31" :origin "#731")

  (item "uring-enomem-status" :group "after-v1-0-1" :area "rocketnet"
   :title "io_uring setup ENOMEM: a status, or the gso fallback"
   :text "Under kernel memory pressure io_uring ring setup can fail with ENOMEM, and create treats it as an ordinary refusal: auto falls to gso and a pinned io_uring ends NO_TRANSPORT_FOUND. Reporting an out-of-memory status instead would stop that fallback, a behaviour change, so it belongs in a minor release. Decide which, then pin it with a test."
   :date "2026-11-11" :origin "v1.0.1 CI: io_uring ENOMEM on a loaded bench, errno now logged by posix_check")

  (item "xdp-stop-abandoned-buffers" :group "after-v1-0-1" :area "rocketnet"
   :title "xdp stop: count the handoff buffers it abandons"
   :text "When xdp stop gives up on a transmit ring that has stopped draining, the handoff buffers it abandons are counted in no counter. Count them, so a stop that dropped packets says so."
   :date "2026-11-11" :origin "v1.0.1 xdp worker fixes (stop bound for a stalled TX ring)")

  (item "linux-send-ip-recverr" :group "after-v1-0-1" :area "rocketnet"
   :title "Linux UDP sends: count interface queue drops per socket"
   :text "On Linux a datagram a full interface queue drops is reported to the send call only with IP_RECVERR set; without it the kernel counts the drop per host (Udp SndbufErrors) and SEND_FAILED never sees it (docs/LINUX.md section 7). Set IP_RECVERR on the portable, gso and io_uring sockets, so the send call returns ENOBUFS and the drop counts in SEND_FAILED, and drain the socket's error queue (MSG_ERRQUEUE). Cost: with IP_RECVERR every ICMP error for a destination (port or host unreachable) is queued on the socket and reported by its next receive call, so each receive path needs an error-queue drain (one more call per drain while the queue is not empty) and a classification of what it reads, where today those errors never reach the socket (section 7)."
   :date "2026-11-12" :origin "read-everything review: SEND_FAILED's claim about a full interface queue")

  (item "xdp-carrier-off-ebusy" :group "after-v1-0-1" :area "rocketnet"
   :title "xdp: EBUSY with the carrier off is counted per frame"
   :text "With the link carrier off, each frame's transmit wakeup EBUSY counts in SEND_FAILED on its own, so a down link inflates the counter by its send rate. Decide whether such a run counts once, as a hard wakeup error does."
   :date "2026-11-11" :origin "v1.0.1 xdp worker fixes")

  (item "xdp-worker-lock-contention" :group "after-v1-0-1" :area "rocketnet"
   :title "Measure xdp worker lock contention"
   :text "Try-lock failures between the main thread and the xdp workers were never measured. Measure them on a two-queue guest on a quiet bench before any change."
   :date "2026-11-11" :origin "v1.0.1 xdp review, item M6")

  (item "xdp-receive-time-seam" :group "after-v1-0-1" :area "rocketnet"
   :title "xdp receive path reads the platform clock"
   :text "The xdp receive path still reads rocketnet_platform_time() instead of the update's time. Passing the time in needs the driver seam widened."
   :date "2026-11-11" :origin "v1.0.1 xdp gateway review, item L13")

  (item "xdp-tx-queue-hash" :group "after-v1-0-1" :area "rocketnet"
   :title "xdp TX queue choice is an unkeyed hash; gso send bank at 32768 clients"
   :text "xdp picks the TX queue with an unkeyed FNV-1a of the destination (rocketnet_server_socket_send_queue), so spoofed sources can aim replies at one queue: key it per server. gso's fixed 4096-packet send bank is short at max_clients 32768."
   :date "2026-11-11" :origin "v1.0.1 review")

  (item "load-split-xdp-flaky" :group "quiet-host" :area "rocketnet"
   :title "load_split xdp server with portable clients fails about 1 run in 4 or 5"
   :text "The two-machine load_split delivery proof with an xdp server and portable clients failed 1 of 5 runs on v1.0.1 (one client RELIABLE_TIMED_OUT) and 1 of 4 on v1.0.0 (missed due slots), so it predates v1.0.1. Find each cause on quiet hosts."
   :date "2026-11-11" :origin "v1.0.1 test campaign, two-machine transport cells")

  (item "uring-rotation-reordering" :group "quiet-host" :area "rocketnet"
   :title "Measure reordering from io_uring per-send socket rotation"
   :text "Since v1.0.1, io_uring picks the send socket per send run instead of per batch, so consecutive packets to one client can leave on different reuseport sockets within one update. Measure any extra reordering against v1.0.0."
   :date "2026-11-11" :origin "v1.0.1 cold read")

  (item "uring-receive-view-640" :group "after-v1-0-1" :area "rocketnet"
   :title "io_uring receive view larger than the payload batch"
   :text "Below 640 clients the io_uring receive view is larger than the server payload batch. Check what the server does with the difference and pin it."
   :date "2026-11-11" :origin "v1.0.1 server transport review, item F5")

  (item "large-messages-under-loss" :group "after-v1-0-1" :area "tests"
   :title "Functional case: large messages under loss"
   :text "The functional suite sends reliable messages above 8 KiB only without loss (1 MiB lossless). Add a lossy case with large blocks in the existing harness."
   :date "2026-11-11" :origin "v1.0.1 guarantees review")

  (item "weak-guarantee-checks" :group "after-v1-0-1" :area "tests"
   :title "Message guarantees checked weakly or not at all"
   :text "The DST end check excuses the last 20 sequences and every ended session and caps messages at 16 KiB; the chunk resend cap is pinned by one assertion; the receipt outcome of sends in flight when a connection ends, and fresh serials after a reconnect, are not checked. Strengthen each."
   :date "2026-11-11" :origin "v1.0.1 guarantees review")

  (item "model-code-mismatches" :group "after-v1-0-1" :area "proofs"
   :title "Models that no longer match their code"
   :text "RNClientIo and its three rings, the MCRNSecurityTables switches, and the expire_floor layout in the token tables no longer match the code. Bring each model back to its code."
   :date "2026-11-11" :origin "v1.0.1 review")

  (item "model-citations-by-name" :group "after-v1-0-1" :area "proofs"
   :title "Cite code by function or action name, not line number"
   :text "Model citations point at source line numbers (for example ServerSession.tla citing rocketnet_server_transport_disconnect_client by line), and check_tla_coverage.py enforces them, so a function moving 21 lines forced every serversession case to be rerun. Cite by function or action name so a move does not stale the records."
   :date "2026-11-10" :origin "Owner request, 2026-11-10")

  (item "transports-shared-setup" :group "after-v1-0-1" :area "rocketnet"
   :title "Transports repeat themselves: share socket setup"
   :text "The transports are large and repeat each other: socket setup is duplicated between gso and io_uring and between iocp and rio. Share it."
   :date "2026-11-10" :origin "Owner request, 2026-11-10")

  (item "windows-transport-gaps" :group "after-v1-0-1" :area "rocketnet"
   :title "Windows transport gaps"
   :text "The IOCP create-failure poll is unbounded (RIO's is bounded). The IOCP unit tests exercise queue_pop while production uses queue_pop_before. The USO fallback is untested."
   :date "2026-11-11" :origin "v1.0.1 Windows review")

  (item "exact-delivery-asserts" :group "after-v1-0-1" :area "tests"
   :title "Functional cases that assert exact delivery over real sockets"
   :text "Windows functional cases and the gso and io_uring functional cases assert exact delivery over real sockets, so a host dropping a datagram under load turns them red. Assert what the library promises instead."
   :date "2026-11-11" :origin "v1.0.1 review")

  (item "fault-counters-public" :group "after-v1-0-1" :area "rocketnet"
   :title "Transport fault counters with no public reader"
   :text "transport_faults feeds no production counter, and the client IOCP, gso and I/O fault counters have no public reader."
   :date "2026-11-11" :origin "v1.0.1 review")

  (item "client-disconnect-family-race" :group "after-v1-0-1" :area "rocketnet"
   :title "Client disconnect and the address-family race"
   :text "rocketnet_client_transport_disconnect does not commit the family race (update does since v1.0.0), so after a disconnect before any cookie the I/O thread polls until stop. A send_batch_progress result out of range retries forever."
   :date "2026-11-11" :origin "v1.0.1 client transport review")

  (item "staged-fakes-claim" :group "after-v1-0-1" :area "tests"
   :title "Staged test fakes still claim at finish"
   :text "The staged fakes in test_unit_public_link_simulator.cpp (staged_fake_t), test_unit_driver_loopback.cpp (wt_staged) and test_transport_control.cpp still claim at finish, unlike the transports they stand for."
   :date "2026-11-11" :origin "v1.0.1 review")

  (item "stale-code-comments" :group "after-v1-0-1" :area "rocketnet"
   :title "Code comments found stale in the v1.0.1 review"
   :text "sdk/rocketnet_test_vectors.h says its data mirrors the standard's appendix byte for byte, but most of its arrays are not printed there; the update-time clamp comments in the client and server transports say a bad time is 'loud in a debug build', which a reviewer found stale. Check each against the code and make it say what is true."
   :date "2026-11-11" :origin "v1.0.1 review; release prep changed docs only")

  (item "fuzz-targets-v1-0-2" :group "after-v1-0-1" :area "tests"
   :title "Three new fuzz targets"
   :text "Branch fix/v1.0.1/fuzz-targets holds fuzz targets for the xdp frame, the io_uring recvmsg and the netlink reply (test code only; the xdp target ran 69 million runs clean on macOS). Landing it needs the fuzz lists in ci.yml and fuzz-nightly.yml, MACOS.md, BUILD-REFERENCE.md and tools/fuzz_ndebug.sh, and a first Linux run."
   :date "2026-11-11" :origin "Held out of v1.0.1")

  (item "platform-primitives-unrun" :group "after-v1-0-1" :area "tests"
   :title "test_platform_primitives runs in no CI job"
   :text "CMake builds the test_platform_primitives binary, but no workflow runs it. Run it in CI."
   :date "2026-11-11" :origin "v1.0.1 review")

  (item "stale-ci-tooling" :group "after-v1-0-1" :area "tools"
   :title "Stale nightly dispositions and roadmap regeneration"
   :text "Every row of tools/nightly_dispositions.txt names a pull request that is now merged or closed, so the nightly watch reports each as STALE. The roadmap workflow commits ROADMAP.md to main, while docs/CLAUDE.md says ROADMAP.md is not committed and it is not in the tree: settle which is meant."
   :date "2026-11-11" :origin "v1.0.1 review")

  (item "xdp-private-link" :group "quiet-host" :area "fleet"
   :title "Private 10G link for XDP packet blaster and bare-metal LAN runs"
   :text "The XDP packet blaster, bare-metal XDP LAN runs and the zero-copy load_split series need a private 10G link between the XDP bench and a second 10G machine; today the XDP bench's only cabled port is its uplink."
   :date "2026-11-10" :origin "Owner request, 2026-11-10; docs/runs/xdp-zero-copy.md")

  (item "trace-spec-invariants" :group "trace-check" :area "proofs"
   :title "Check the spec's own invariants on every trace"
   :text "The trace cfg allows INVARIANT TraceConsistent only, so the spec's invariants are checked at the model's small sizes and never on the trace at the code's real sizes: code that follows a model which is wrong at 4096 slots passes. Enforcing it: an invariants list in each LAYERS row, the checker verifying each is a plain state predicate of the spec (the reach-predicate check already does this) and allowing INVARIANT lines for exactly those names, the writer emitting them, and a planted row whose model breaks an invariant only past its TLC size. Cost per layer: no extra TLC run, since the invariants are evaluated in the clean trace's existing run, once per record (401 at the default 400 steps). An invariant over a fixed array costs about slots x records evaluations, about 1.6 million for a 4096-slot ring at 400 steps, a few seconds on the bench and unmeasured until the first such layer lands. A plant trace stops at its rejection, and the plant run must not take the invariants, or a planted invariant break could stand in for the trace rejection the plant must cause."
   :date "2026-11-12" :origin "Trace check v2, third cold reader (README 'What the check trusts')")

  (item "trace-library-copy" :group "trace-check" :area "tools"
   :title "Refuse a copy of library code in a layer's test file"
   :text "The walk must call the library, not a copy of it left in tests/tla_trace_<layer>.cpp beside a real sdk include. Today a reviewer checks this by reading the file. Enforcing it: after the clean build, run nm on the layer's object file and refuse any defined symbol with the library's prefix (rocketnet_) or any name the sdk files define. Also refuse a layer file that defines a function an sdk header declares. This needs the object path from CMake ($<TARGET_OBJECTS:tla_trace>, written next to tla_trace_config.txt) and nm or dumpbin on each OS. Add a planted row that copies one sdk function into the example."
   :date "2026-11-12" :origin "Trace check v2, third cold reader")

  (item "trace-config-per-source" :group "trace-check" :area "tools"
   :title "Read the trace binaries' configuration from the layer's own unit"
   :text "CONFIG is printed by tests/tla_trace.cpp, so a COMPILE_DEFINITIONS set on one layer's source file, or a configuration macro without the ROCKETNET_ prefix, changes the layer's code without the checker seeing it. Enforcing it: CMake refuses source-file COMPILE_DEFINITIONS on tests/tla_trace_*.cpp. Each layer's walk prints CONFIG through a macro in tla_trace_record.h expanded in its own unit. The macro list is every identifier an #if of the included sdk files tests, read from those files, not a fixed list of ROCKETNET_ names. Add planted rows for a per-source define and for an unprefixed macro."
   :date "2026-11-12" :origin "Trace check v2, third cold reader")

  (item "thread-rules-nesting-model" :group "proof-debt" :area "proofs"
   :title "Model the nested calls and the refused look's three writes in RNFillPhase"
   :text "proofs/tla/RNFillPhase.tla leaves out the nesting depths, and takes a refused look (sdk/rocketnet_fill_phase.cpp rocketnet_fill_phase_fill_begin: the word, the owner and the word again) as one step. A cold reader found an interleaving in that gap: a call that a main thread call ran inside puts the owner back while the word still reads as held at depth 1, the thread it gives the client back to reads its own next call as nested, and its end leaves a fills_in line it never entered, after which every main thread call is refused. Fixed by the order of the writes (the word goes back to the lock's form first); no test fails without the fix and no model step sees it. Add the nested path and the three writes to the model with a reversed witness for the old order, and a trace point between the writes."
   :date "2026-11-13" :origin "Piece G (sending from the game threads), cold reader finding 1")

  (item "payload-ring-chunk-layout-model" :group "proof-debt" :area "proofs"
   :title "Model the chunked payload ring claims at the layout level"
   :text "rocketnet_server_payload_ring_take (sdk/rocketnet_server_transport.cpp) claims payload ring slots in chunks held on per-thread lines, with index arithmetic on a packed [end:32][next:32] word, a guard, a clip at send_capacity and a give-up when another thread on the line installs first. proofs/tla/RNServerPayloadRingLayout.tla still models one fetch-add a slot. Model the same arithmetic at capacity 3 or 4 with reversed witnesses: a lost install that keeps its first slot (one client's packets then leave out of start order; test_server_fill_ring_chunk_lost_install_keeps_start_order pins the fix), and a line's unused slots not marked empty before the flush."
   :date "2026-11-13" :origin "Piece G, cold reader finding 2; the layout rule")

  (item "send-reserve-tails-named-implied" :group "proof-debt" :area "proofs"
   :title "RNSendReserve: TailsNamed is implied by NeverStarved"
   :text "As defined, with no other thread holding a tail RoomFor and RoomPastTails are the same predicate, so unnamed implies starved: the invariant TailsNamed adds nothing NeverStarved does not check, and its witness MCRNSendReserveBrokenNoTailReturn.cfg would fail NeverStarved too. The model also marks the counter choice at the decision, where the code reads the tails after the take back and the retest. Restate TailsNamed so it can fail alone, and move the choice to where the code makes it."
   :date "2026-11-13" :origin "Piece G, cold reader (TLA+ section)")

  (item "thread-rule-counters-production-test" :group "audit-remaining" :area "tests"
   :title "Read the three thread rule counters on the production library"
   :text "MAIN_THREAD_CALL_OVERLAP, CLIENT_SEND_TWO_THREADS and SEND_RESERVE_REFUSED_TAILS_HELD are counted in every build, and each is read by value in a test, but every such test links the development library (bin/test_wide_fill, tests/test_unit_server_send_reserve.cpp). Add a case to the production functional binary that breaks each of the two thread rules through the public API and reads the counters; the same case can check the API misuse count of a message left open at update, which no test reads."
   :date "2026-11-13" :origin "Piece G, cold reader (counters and tests sections)")

  (item "ring-order-across-thread-change" :group "audit-remaining" :area "sdk"
   :title "Start order of one client's packets when its sending thread changes between flushes"
   :text "On a transport that buffers payloads in the server's ring, a thread's unused chunk slots stay on its line until the flush. If one thread sends to a client, a checked main thread call that is not send_packets follows, and another thread then sends to the same client, the second thread's packets may sit in lower slots and leave first. rocketnet.h states it. Returning every line's chunk at each checked main thread call would keep start order; it costs a walk of the 64 lines a call."
   :date "2026-11-13" :origin "Piece G, cold reader finding 2 (second case)")

  (item "msgx-partial-send-no-sleep" :group "audit-remaining" :area "sdk"
   :title "Do not sleep after a partial sendmsg_x"
   :text "A partial sendmsg_x result ends the call kept (sdk/rocketnet_msgx_send_runs.h), so the transmit worker marks the socket full and waits 1 ms (sdk/rocketnet_server_msgx_apple.cpp) although the socket took data. Loop again at once when the kept call made progress, and wait only when a call took nothing. Measured nowhere yet: the four space rows pass as it is."
   :date "2026-11-13" :origin "Piece P cold read")

  (item "rio-destroy-stuck-lane" :group "audit-remaining" :area "rocketnet"
   :title "rio destroy: free the stopped lanes when one lane is stuck"
   :text "rocketnet_server_rio_destroy returns before destroying any lane when one lane never reports stopped (a completion queue reported corrupt, or a send the kernel never completes). The lanes that did stop stay allocated until process exit. Measured as intended for this piece: the leak is bounded and named. Fix: destroy every lane that reports stopped and leak only the stuck one, as the create rollback already does (rocketnet_server_rio_create_leaks), with a unit case that holds one lane's sends."
   :date "2026-11-13" :origin "Piece N (rio rebuilt), cold read")

  (item "gso-layout-model-send-step" :group "audit-remaining" :area "proofs"
   :title "Layout model coverage of the gso send step's order, held slot and plain resend"
   :text "proofs/tla/RNGsoQueueBankLayout.tla models the claim, the id and the slot rule. lane->order (the gather's positions), send_step's held_slot and the position it resumes from, and resend_plain's first_slot arithmetic index the banks too and have no layout model or trace layer; they are covered by unit cases only."
   :date "2026-11-13" :origin "Piece K, cold reader")

  (item "gso-packet-id-zero-queue-zero" :group "audit-remaining" :area "sdk"
   :title "Packet id 0 names queue 0 slot 0 of turn 0"
   :text "The gso packet id is ( turn * slots + slot ) * queues + queue, so the first packet of the first turn on queue 0 has id 0. Nothing treats 0 as no packet today (finish and abort check the pointer against the slot), but a caller that keeps 0 as its own empty value would lose that packet. Decide whether the id should start at 1."
   :date "2026-11-13" :origin "Piece K, cold reader")

  (item "pin-network-threads-at-creation" :group "audit-remaining" :area "sdk"
   :title "Network threads placed at creation, not by their own first call"
   :text "The gso, XDP and the other transports' network threads pin themselves as their first act, so between pthread_create and that call a thread runs on its creator's mask, which may include cpu 0 (the OS's on Linux); the gso creator also raises the priority in that window. Starting the thread with its affinity already set (pthread_attr_setaffinity_np in the platform thread start, one place for every transport) closes the window. Also: _SC_NPROCESSORS_ONLN is a count, used as the highest cpu id; a machine with an offline cpu in the middle is misjudged (read the online set instead)."
   :date "2026-11-13" :origin "Piece K, cold reader")

  (item "send-reserve-multi-queue-capacity" :group "proof-debt" :area "proofs"
   :title "RNSendReserveCalls: a Capacity per send queue"
   :text "send_capacity_available is one number: with several send queues it is the fewest free slots of any queue, an upper bound on what the next start may find. The send reserve's per-queue room (send_queues, send_queue_of, send_room_queue) is read by the reserve, but RNSendReserveCalls models one Capacity. Model a Capacity per queue, bind the code's call to it in the trace check, and say in the transport standard what a start on a queue with room may rely on."
   :date "2026-11-13" :origin "Piece M follow-on, cold reader")
 ))
