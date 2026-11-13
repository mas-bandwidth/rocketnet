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
  (group "after-v1-1-landing" :title "Found while landing and re-measuring the v1.1.x pieces"
   :text "Known and not done after the transport work landed on main as v1.1.x point releases and was measured again from main (2026-11-13). Each says where it is and how it was found.")
  (group "review-v1-2" :title "Found by the review before v1.2.0, not fixed for it"
   :text "Findings of the review that started on 2026-11-13 (main 5488a5443) that are not being fixed for v1.2.0. Read, not run, unless an entry says so; each names the file and lines.")
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

  (item "apply-main-ruleset" :group "decisions" :area "github"
   :title "Apply the main ruleset before the first outside pull request"
   :text "Main has no ruleset on GitHub that requires a check (audit 44), on purpose for now. Apply .github/rulesets/main.json on GitHub before the first outside pull request. Left off for now (owner, 2026-11-12)."
   :date "2026-11-12" :origin "Audit item 44; owner, 2026-11-12")

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
   :title "Stale nightly dispositions"
   :text "Every row of tools/nightly_dispositions.txt names a pull request that is now merged or closed, so the nightly watch reports each as STALE; and the nightly workflows added since (fuzz, msan, toolchains, real-load, frame-budget and others) have no row at all, so the watch is red on them (seen by the release cutter on main 659e0b8d6, 2026-11-13). Give each a current row or retire it."
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

  (item "posix-hk-mark-after-last-send" :group "audit-remaining" :area "tests"
   :title "Hard-killed client cases: take the wire mark after the clients read the last pass's sends"
   :text "posix_hk_quiesce (tests/functional/posix_hard_killed_client.cpp) updates the clients, then the server, then sends the server's packets, so a datagram the server sends in the last pass is counted sent at posix_hk_wire_mark and received after it. That datagram hides one real loss in lost_down, and a payload lost on the way then fails posix_hk_require_accounted with lost 0. Seen once on a parallel macOS run (send_to_closed_port, server portable, ipv6, client msgx: down 79/80, lost 0). Fix: end the quiesce with a client-only read pass before the mark, or mark the client and server reads at the same point."
   :date "2026-11-13" :origin "Piece P CI read")

  (item "msgx-port-exclusive" :group "review-v1-2" :area "sdk"
   :title "macOS msgx: keep the server's port from other local sockets"
   :text "The msgx shared socket sets SO_REUSEPORT so its per-client connected sockets can join the port, and so any later local socket that sets SO_REUSEPORT can bind the port too; a more specific one (the server's own address, or AF_INET beside a dual-stack [::] server) then receives every datagram meant for the shared socket, handshakes included, uncounted (measured, one user; docs/MACOS.md, Who else can bind a server's port). Darwin has no exclusive option that still lets the server's own sockets share the port. A real fix: drop the per-client connected sockets and demultiplex every client on the shared socket, so the shared socket needs no SO_REUSEPORT; with a specific bind address no later socket then receives its datagrams, as measured for a plain socket on a specific address. Costs: the per-client sockets' receive split and send path, to be measured against the load test row before it is taken. Also owed: run the intruder cases as a second user on a bench Mac (not measured)."
   :date "2026-11-13" :origin "Review before v1.2.0, macOS reader, finding P2 (run, one user)")

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

  (item "send-reserve-multi-queue-capacity" :group "proof-debt" :area "proofs"
   :title "RNSendReserveCalls: a Capacity per send queue"
   :text "send_capacity_available is one number: with several send queues it is the fewest free slots of any queue, an upper bound on what the next start may find. The send reserve's per-queue room (send_queues, send_queue_of, send_room_queue) is read by the reserve, but RNSendReserveCalls models one Capacity. Model a Capacity per queue, bind the code's call to it in the trace check, and say in the transport standard what a start on a queue with room may rely on. The trace check is narrower than that today: tests/tla_trace_send_reserve.cpp (213-221) only ever runs one queue, so the per-queue reserve code in sdk/rocketnet_server_transport.cpp (about 1060-1093, 1207-1253) is on no checked trace and a defect there cannot turn it red. Walk several queues."
   :date "2026-11-13" :origin "Piece M follow-on, cold reader; review before v1.2.0, finding M3 (2026-11-13)")

  (item "portable-server-shared-handoff" :group "audit-remaining" :area "sdk"
   :title "Portable server transport on the shared buffer handoff"
   :text "PR #1043 (refactor/v1.1.x/server-shared-handoff, extraction E5) moved the portable and gso server transports off their hand-written bank queues onto rocketnet_buffer_handoff_t (sdk/rocketnet_buffer_handoff.h). Its gso half is overtaken by the rebuilt gso transport (#1103), which rewrites sdk/rocketnet_server_gso_linux.cpp and also changes sdk/rocketnet_driver_builtin.cpp. After the rebuilt gso lands, redo the portable half alone on main (the portable TX and RX commits and the rocketnet_buffer_handoff_started helper), with its gates: rnportabletx, rnportablerx and rnbufferhandoff TLC, the unit mutation cases and the light memory pins. With it, give the portable receive worker what the rebuilt transports have: it runs no prefilter (sdk/rocketnet_driver_builtin.cpp about 613-645, drop hooks NULL about 1962-1966), so junk reaches the main thread (bounded per update), and datagrams held in its banks at stop are counted nowhere (about 932-944), so RECEIVE_DROPPED_STOPPED reads 0 on portable. Not worse than v1.0.0."
   :date "2026-11-13" :origin "Open-PR triage 2026-11-13: #1043 not landed; review before v1.2.0, finding PO1")

  (item "rio-registered-region-red-zones" :group "audit-remaining" :area "tests"
   :title "ASan red zones between the rio registered region's arrays"
   :text "In an AddressSanitizer build the rio transport's sized block (rocketnet_rio_sized_layout: slot bytes, retry slices, process arrays, drops, each bank's data, from and packet bytes) carries poisoned red zones between its carves. The registered region laid out by sdk/rocketnet_rio_layout.h (send frames, send addresses, receive slices, receive addresses; one RIORegisterBuffer) does not: its offsets are the layout RNRioLaneLayout models and the trace check binds, so a pad there changes a modelled layout. Add the pads to the layout and its model together, or record why that region stays uncovered."
   :date "2026-11-13" :origin "#1047 merge with the rebuilt rio")

  (item "uring-carve-red-zones" :group "audit-remaining" :area "tests"
   :title "ASan red zones between the rebuilt io_uring transport's carved arrays"
   :text "#1047 carved red zones into the replaced io_uring transport's bank block (rocketnet_server_uring_buffers_create). The rebuilt transport (#1104) removed that block; its carved allocations are now in sdk/rocketnet_uring_queue_send.cpp, sdk/rocketnet_uring_receive_queue.cpp and sdk/rocketnet_uring_send_queue.cpp, each one rocketnet_malloc of a layout. Add ROCKETNET_RED_ZONE_CARVE to each where the layout is not a modelled or kernel-registered region, and record why where it is."
   :date "2026-11-13" :origin "#1047 merge with the rebuilt io_uring")

  (item "uring-lifecycle-rss-plan" :group "audit-remaining" :area "tests"
   :title "The io_uring provided buffers are 264 MiB a queue whatever the GRO tier"
   :text "With ROCKETNET_SERVER_TRANSPORT=io_uring forced over the whole functional suite, test_lifecycle_transport peaks at 1269 MB resident against 698 MB, which is the suite's per-case bound (suite_case_rss_mb), not a plan bound. The cause is the receive buffers: 4096 buffers of 66 KiB a queue (ROCKETNET_URING_RECV_BUFFERS, ROCKETNET_URING_RECV_BUFFER_BYTES), 264 MiB a queue and two queues by default (num_io_uring_workers), made resident once the multishot writes into them, whether or not UDP_GRO was granted. Size the buffers by the tier (a buffer need only hold a whole datagram without GRO) or lower the count, and say so in the plan."
   :date "2026-11-13" :origin "Piece L (io_uring), amd-3 run and cold reader")

  (item "uring-cancel-ealready" :group "audit-remaining" :area "sdk"
   :title "A cancel answered -EALREADY in the io_uring worker"
   :text "The worker counts a cancel it submitted when its completion arrives (rocketnet_uring_queue_worker_reap_one, ROCKETNET_URING_TAG_CANCEL). A cancel the kernel answers -EALREADY (the target already completed) reads the same as one that took effect; the model (RNUringQueueWorker.tla CancelRecv, CancelWake, CancelPoll) has one outcome. Check the worker against the answers and add the case to the model."
   :date "2026-11-13" :origin "Piece L (io_uring), cold reader")

  (item "uring-offline-cpu-holes" :group "audit-remaining" :area "sdk"
   :title "Offline cpu holes in the io_uring pin"
   :text "rocketnet_uring_pin_check refuses a cpu at or past the online count, but a machine with cpus offline has holes below that count (cpu 5 offline, cpu 6 online). A pin to a hole is refused by the kernel, counted, and the worker is confined to its own mask past the OS's cpus (rocketnet_uring_pin_confine_words, which never names a hole), so it is placed but not where the config said. Read the online mask (sched_getaffinity or /sys/devices/system/cpu/online) and place the workers on online cpus past the OS's."
   :date "2026-11-13" :origin "Piece L (io_uring), cold reader")

  (item "transport-thread-start-mask" :group "audit-remaining" :area "sdk"
   :title "A transport thread runs on its creator's mask until its first call"
   :text "Every transport thread (io_uring and gso queue workers, the XDP thread, and the other transports' network threads) pins itself as its first call, so between rocketnet_platform_thread_create and that call it runs on the creating thread's mask, which may hold cpu 0 (the OS's on Linux); the gso creator also raises the priority in that window. Give rocketnet_platform_thread_create an optional affinity (pthread_attr_setaffinity_np on Linux) so the thread starts on its placement: one place in the platform layer, used by every transport. Also: _SC_NPROCESSORS_ONLN is a count, used as the highest cpu id, so a machine with an offline cpu in the middle is misjudged (read the online set instead)."
   :date "2026-11-13" :origin "Piece K cold reader; piece L (io_uring) second cold reader")

  (item "uring-receive-folds" :group "audit-remaining" :area "sdk"
   :title "io_uring receive refusals that are not in the server's drop reasons"
   :text "The rebuilt io_uring transport's receive drop totals are the worker prefilter's and the bank's reasons plus refused completions (RECEIVE_FAILED). A truncated source (URING_RECEIVE_DROPPED_SOURCE), a completion naming no buffer (URING_RECEIVE_DROPPED_NO_BUFFER) and segments past one receive (URING_RECEIVE_DROPPED_OVERFLOW) are counted only in their URING_ rows, where the replaced transport also folded them into RECEIVE_FAILED and BANK_FULL. A source drop is also counted once per completion, even for a GRO batch of several datagrams (sdk/rocketnet_uring_receive_queue.cpp about 598-624), where gso counts each datagram into RECEIVE_FAILED (sdk/rocketnet_server_gso_linux.cpp about 1052, 1197-1205). Decide the fold and the unit, and test both by value."
   :date "2026-11-13" :origin "Piece L (io_uring), second cold reader; review before v1.2.0, finding L2 (2026-11-13)")

  (item "uring-complete-without-more" :group "audit-remaining" :area "sdk"
   :title "RNUringQueueReceive: Complete without F_MORE"
   :text "The model's Complete takes a datagram completion and keeps the multishot armed; the code (rocketnet_uring_receive_queue_cqe) also ends it when the completion has no IORING_CQE_F_MORE and arms again on the next pass. Add the ending to Complete with a reversed witness, and a trace point."
   :date "2026-11-13" :origin "Piece L (io_uring), cold reader")

  (item "uring-gro-cmsg-one-datagram" :group "audit-remaining" :area "sdk"
   :title "A malformed UDP_GRO control message is passed as one datagram"
   :text "A receive whose control area is truncated or malformed (GRO_CMSG_FAULTS) is taken as one datagram, so a coalesced buffer of several datagrams reaches the prefilter as one long one and fails its checks there. Decide whether the receive module should drop it by name instead, with a counter row, and test the split on a malformed control area."
   :date "2026-11-13" :origin "Piece L (io_uring), cold reader")

  (item "uring-rearm-spin" :group "audit-remaining" :area "sdk"
   :title "The io_uring receive re-arm spins on a kernel that refuses multishot"
   :text "A kernel that refuses multishot receive (Linux before 6.0) ends every arm at once; the worker arms again on its next pass without a bound, burning its core. The refusals are visible as RECEIVE_FAILED now (the receive module's ERRORS reach the drop totals). Bound the re-arms, back off, and name the kernel in the create log."
   :date "2026-11-13" :origin "Piece L (io_uring), cold reader")

  (item "tlc-plant-behaviour-not-printed" :group "trace-check" :area "tools"
   :title "Find why TLC sometimes prints no behaviour for a plant's violation"
   :text "TLC 2.19 exits 12 on a plant trace and reports the invariant violated, then sometimes prints no behaviour; the statistics lines that follow are printed, so the log is not cut off. tools/tla_trace_check.py reruns such a plant up to five times and prints PLANT-ATTEMPTS (#1106). Ruled out on byte-identical inputs of the msgxsendruns seed 1 plant (hetzner1, 2026-11-13): TLC's -seed and -fp index (both pinned, -workers 1: a behaviour in 5/10, 6/10 and 7/10 runs for three pairs) and a shared or reused metadir (a fresh directory and -metadir each run). Start from a TLC-side cause: the trace file's write and read-back at the violation."
   :date "2026-11-13" :origin "Piece P, #1093 trace shard 0/6")

  (item "reconcile-sndbuf-resends" :group "after-v1-0-1" :area "tests/load"
   :title "Reconcile: tell a refused-then-resent send from a real drop"
   :text "The harden_load reconcile counts the kernel's SndbufErrors as named drops in the server-to-client chain. When the 1 MB socket send buffer is full the kernel refuses a gso send and the gso worker sends it again, so nothing is lost: on main 3218cf5f6 (X710 pair) library_sent equals library_received and players deliver 100%, yet every gso row reports chains=FAIL on 124k to 571k of them (REMEASURE-MAIN.md, gso). Count the refusals the transport resent in their own term, apart from sends the kernel refused and nothing sent again, so the chain still fails a real drop."
   :date "2026-11-13" :origin "gso re-measure on main 3218cf5f6")

  (item "fill-phase-trace-gaps" :group "trace-check" :area "tests"
   :title "Fill-phase trace check: the gaps the split-update fix left"
   :text "tests/tla_trace_fill_phase.cpp now reads the window and both refusal counters from the library and its reach predicates require every refusal step (MainLook, MainIn, FillLock, FillDecide, FillLook in and out of the window, TakeLook). Still open: fbroke, phaseIn, falseAlarm, mainsDone, fillsDone and takesDone are the walk's own bookkeeping, compared but read by Next, so they cannot be declared ghosts as the checker defines them; the compare-and-swap retry loop in fill_begin, the nested paths (main_owner, depth above 1) and rocketnet_fill_phase_hand_over never run under the trace. Model the nesting and hand_over and drive them from the walk, and say for each walk-kept variable what in the library it stands for."
   :date "2026-11-13" :origin "review fixes S1/S2, coordinator finding C7")

  (item "uring-peak-qdisc-drops" :group "after-v1-1-landing" :area "rocketnet"
   :title "io_uring at peak: whole segmented sends dropped at the server's device queue"
   :text "On the X710 pair with 2 io_uring threads, peak load and no game load, the server's fq_codel queue drops whole UDP_SEGMENT sends: each is one player's 15-packet tick, so 2 to 13 drops in a 60 second row lose 30 to 195 packets (s2c 99.9997 to 100%; players whole, rows pass). The kernel accepted the sendmsg, so the transport counts the packets sent and nothing names the loss. The same rows also see 8.6 to 10.7 million EAGAIN completions a peak row against the documented 1,000,000-byte send buffer, where gso on the same pair and buffer sees 124k to 571k; a 4096-entry ring of sends in flight per queue is the likely reason, not measured. gso and XDP on the same pair: sent equals received. Tuning deferred by the owner ('we can do fine tuning later')."
   :date "2026-11-13" :origin "io_uring re-measure on main 3afea2aed, rows U1 and r2-r4 (REMEASURE-MAIN, io_uring)")

  (item "reconcile-segmented-drops" :group "after-v1-1-landing" :area "tests/load"
   :title "Reconcile: a dropped segmented send is 15 packets, not 1"
   :text "The harden_load reconcile names the server's qdisc drops by the qdisc's count, which counts a dropped GSO skb once. At peak each skb carries 15 packets, so every such drop leaves 14 packets in the unnamed gap and the s2c chain fails on a loss it could name. Count the segments of a dropped skb (or read the qdisc's byte or packet drop totals), so the chain names the whole loss. Separate from reconcile-sndbuf-resends."
   :date "2026-11-13" :origin "io_uring re-measure on main 3afea2aed: lost = 15 x qdisc dropped in every run")

  (item "uring-send-deferred-row-text" :group "after-v1-1-landing" :area "sdk"
   :title "URING_SEND_DEFERRED: the row text says something other than what is counted"
   :text "sdk/rocketnet_counters.h says the row counts send calls that returned with sends not yet completed or staged slots not yet submitted. The code adds the worker's SOCKET_FULL to it (sdk/rocketnet_uring_counters.h), which sdk/rocketnet_uring_queue_worker.cpp counts once per send completion that came back -EAGAIN (send buffer full; the run is sent again after POLLOUT); in every re-measure row it equals the kernel's SndbufErrors exactly. Make the text say what is counted, or count what the text says."
   :date "2026-11-13" :origin "io_uring re-measure on main 3afea2aed")

  (item "uring-prefilter-wrong-value-once" :group "after-v1-1-landing" :area "sdk"
   :title "PREFILTER_WRONG_VALUE seen once on io_uring under heavy bench load"
   :text "A Debug functional row of the io_uring piece reported PREFILTER_WRONG_VALUE 1274 once, under heavy load on amd-3; 12 reruns did not reproduce it and the cause was not found. A DEV-only sampler now records the first refused datagrams (source, length, first 16 bytes) in sdk/rocketnet_receive_prefilter.h and prints them in the clean-run report. When it recurs, read the samples; the open question is whether a datagram can reach the io_uring prefilter with wrong bytes or length (provided-buffer index, completion length, GRO split, a buffer returned while still referenced)."
   :date "2026-11-13" :origin "Piece L (io_uring), gate at ad93b2ad6")

  (item "server-update-task-callbacks" :group "after-v1-1-landing" :area "sdk"
   :title "Distribute the server update with task callbacks in the config, only on measured need"
   :text "The split server update (update_receive, update_client, update_finish) landed as an opt-in; rocketnet runs no job system of its own. If a measured row shows the game's own job system cannot spread the update well enough through the public calls, add box3d-style task callbacks to the server config, off by default (see how box3d and fixed3d do it), and not before."
   :date "2026-11-13" :origin "Owner rulings on piece I, 2026-11-13 (V12-ON-MAIN)")

  (item "space-frame-row-transports" :group "after-v1-1-landing" :area "scripts"
   :title "scripts/space-frame-row runs XDP only"
   :text "space-frame-row passes --transport xdp fixed, so the gso and io_uring re-measures called scripts/harden-pair with the same server arguments by hand. Take the transport (and its queue count) as an argument so every Linux transport's row is one command."
   :date "2026-11-13" :origin "gso and io_uring re-measures on main")

  (item "performance-doc-main-rows" :group "after-v1-1-landing" :area "docs"
   :title "docs/PERFORMANCE.md: rows from the re-measure on main"
   :text "Update the performance document's rows from the re-measure on main (XDP 59890655d, gso 3218cf5f6, io_uring 3afea2aed, and RIO, IOCP and macOS as their rows on main come in), with the minimum network threads each transport needs for the peak row. Done by the document's author."
   :date "2026-11-13" :origin "Re-measure on main, 2026-11-13")

  (item "trace-reach-vacuous" :group "trace-check" :area "tools"
   :title "Refuse a reach predicate that already holds in the first state"
   :text "tools/tla_trace_check.py checks that each row's reach predicates are reached in some clean trace (a TLC run with ~predicate as the invariant). A predicate that holds in the initial state, or always, passes that check without proving any edge was walked. Refuse a predicate that holds at the first record, with a planted row."
   :date "2026-11-13" :origin "Piece A (#1074) cold reader, item 5")

  (item "xdp-gateway-refresh-main-thread" :group "review-v1-2" :area "rocketnet"
   :title "XDP: the gateway refresh blocks the main thread every 5 seconds"
   :text "On XDP, rocketnet_server_socket_receive_packets (sdk/rocketnet_server_socket_linux.cpp about 4745) runs the gateway refresh on the main thread every 5 s (rocketnet_xdp_gateway_refresh, sdk/rocketnet_xdp_frame.h about 1473, through rocketnet_server_socket_resolve_gateway about 648). rocketnet_gateway_netlink_refresh opens a NETLINK_ROUTE socket, dumps the whole route table for the family (RTM_GETROUTE with NLM_F_DUMP), then queries the neighbour (RTM_GETNEIGH), each receive blocking with a 1 s timeout (sdk/rocketnet_gateway_netlink_linux.cpp about 751-840, 1006); with both families, twice. The cost grows with the host's route count and can block the main thread up to the timeout under rtnl contention; it does not show on a bench with a small table. Resolve it off the main thread (on a network thread), or query the one destination without blocking."
   :date "2026-11-13" :origin "Review before v1.2.0, finding X5")

  (item "iocp-stop-repost-hang" :group "review-v1-2" :area "rocketnet"
   :title "IOCP stop can hang on a receive reposted after its owner's worker exited"
   :text "Any IOCP worker reposts a refused receive on the owning endpoint's socket (sdk/rocketnet_server_iocp_windows.cpp about 1684-1699), reading stopping (1184) before it raises pending and posts (1235-1237); each stopping worker cancels only its own endpoint's receives and exits when the global pending count reads 0 (1501-1516). A repost that lands after its owner's worker has exited is never cancelled: rocketnet_server_iocp_stopped never turns true, destroy refuses (3722-3726) and create's failure path waits with no bound (3027-3035). Very rare. The model hides it: RNIocpEndpoints.tla Retry (89-96) checks ~stopping and posts in one atomic step. Fix: raise pending before the stopping check, or have every stopping worker cancel every endpoint's receives; split Retry in the model with a reversed witness."
   :date "2026-11-13" :origin "Review before v1.2.0, Windows reader, finding W1")

  (item "send-capacity-overload-walk" :group "review-v1-2" :area "sdk"
   :title "Send capacity is reported for frames the caller cannot take, so an overloaded frame walks every client"
   :text "send_capacity_available counts own frames and every queue's budget (sdk/rocketnet_server_transport.cpp about 1510-1528), more than the next start can take, so under overload the serial walk visits every client and is refused for each (sdk/rocketnet_message_server.cpp about 2073-2080, 2195, 2225): main thread work grows with the client count on exactly the frames that are already over. Report what the caller can take."
   :date "2026-11-13" :origin "Review before v1.2.0, send reserve reader, finding M1")

  (item "send-capacity-one-full-queue" :group "review-v1-2" :area "sdk"
   :title "One full send queue makes the send capacity 0 for every client"
   :text "Capacity is the fewest free slots of any queue (sdk/rocketnet_server_transport.cpp about 1549, through io_send_room 984-1000), so one full queue stops sends to every client, including clients on queues with room. io_send_room_total (1101-1119) already does this right for disconnect drains. Use the client's own queue's room (or the total) for its start. See also send-reserve-multi-queue-capacity."
   :date "2026-11-13" :origin "Review before v1.2.0, send reserve reader, finding M2")

  (item "uring-stop-retry-bound" :group "review-v1-2" :area "sdk"
   :title "io_uring stop-time send retry bound counts every refusal and never resets on progress"
   :text "The io_uring worker's stop-time bound counts each refused send and neither waits nor resets when a send makes progress (sdk/rocketnet_uring_queue_worker.cpp about 185-196, 275-278), so a slow drain at stop can give up early; gso resets on progress and waits (sdk/rocketnet_server_gso_linux.cpp about 722-733). Only reachable when stop runs with sends outstanding. Match gso."
   :date "2026-11-13" :origin "Review before v1.2.0, gso and io_uring reader, finding L1")

  (item "uring-send-enobufs" :group "review-v1-2" :area "sdk"
   :title "io_uring drops a send refused with ENOBUFS or ENOMEM, where gso retries"
   :text "sdk/rocketnet_uring_queue_send.cpp (about 423-438) treats ENOBUFS and ENOMEM on a send as FAILED: the packets are dropped and counted. gso sends them again (sdk/rocketnet_server_gso_linux.cpp about 2554). Make the two transports treat a transient refusal the same way."
   :date "2026-11-13" :origin "Review before v1.2.0, gso and io_uring reader, finding L3")

  (item "xdp-send-trace-layer" :group "review-v1-2" :area "proofs"
   :title "The XDP send side has models and no trace layer; the receive-ring walk never runs the real worker loop"
   :text "No trace layer binds the XDP send half to its models: the buffer exchange publish, claim and recycle (sdk/rocketnet_server_socket_linux.cpp about 158-210, RNBankFifoLayout.tla), the producer publish and turn bump in rocketnet_server_socket_send_packets (about 3268-3316, RNDriverSendStaging.tla Publish), the worker's claim, copy, recycle and send-frame free list (about 5008-5156, WorkerRetire), and the stop drain (rocketnet_server_socket_send_idle about 3051, rocketnet_xdp_send_stall_pass in sdk/rocketnet_server_socket.h about 108, RNXdpSendDrain.tla). Only unit cases cover them (tests/test_unit_xdp_send_staging.cpp, tests/test_xdp.cpp). The xdp-receive-ring trace (tests/tla_trace_xdp_receive_ring.cpp about 287-460) calls the pass helpers in an order the walk composes and never runs rocketnet_server_socket_xdp_thread_function (about 4825). Add send layers, and drive the receive walk through the real loop."
   :date "2026-11-13" :origin "Review before v1.2.0, XDP reader, finding X1")

  (item "xdp-vlan-stripped-tags" :group "review-v1-2" :area "rocketnet"
   :title "An XDP server on a VLAN takes frames from other VLANs when the NIC strips tags"
   :text "sdk/xdp/server_xdp.c (about 483-516) and sdk/rocketnet_xdp_frame.h (about 383-405) take an untagged frame on a tagged server. With receive VLAN offload on (the i40e and ixgbe default) native XDP sees every stripped frame untagged, so a frame of any VLAN the NIC accepts, or of the native segment, addressed to the server's address and port is processed, and the reply goes out tagged with the server's VLAN: layer-2 isolation is bypassed for the game port and spoofed sources skip the router's filters. Smallest fix: refuse a VLAN server unless rx-vlan-offload is off, or read the stripped tag (bpf_xdp_metadata_rx_vlan_tag, Linux 6.8 and later)."
   :date "2026-11-13" :origin "Review before v1.2.0, XDP reader, finding X2")

  (item "xdp-martian-sources" :group "review-v1-2" :area "rocketnet"
   :title "XDP accepts sources the kernel stack drops as martian, and does not verify checksums on receive"
   :text "sdk/xdp/server_xdp.c has no source check, and userspace refuses only IPv4-mapped IPv6 (sdk/rocketnet_xdp_frame.h about 675, sdk/rocketnet_address.h about 65): 0.0.0.0/8, 127/8, 224/4, 255.255.255.255, the server's own address, ::, ::1, ff00::/8 and source port 0 reach the main thread and get replies. IPv4 header and UDP checksums are not verified on receive (rocketnet_xdp_frame_parse_vlan about 420-537), and the kernel fast path does not check the version nibble (server_xdp.c about 552). The socket transports get all of this from the kernel. Refuse these in the kernel program and check the sums."
   :date "2026-11-13" :origin "Review before v1.2.0, XDP reader, finding X3")

  (item "xdp-receive-stopped-uncounted" :group "review-v1-2" :area "rocketnet"
   :title "XDP: datagrams received and not handed to the main thread at stop are counted nowhere"
   :text "The XDP operations table passes NULL for receive_drop_totals (sdk/rocketnet_driver_builtin.cpp about 2523), the only feed of RECEIVE_DROPPED_STOPPED, so at stop (sdk/rocketnet_server_socket_linux.cpp about 4887-5285) the staged, ready, unread-tail (about 4761) and receive-ring datagrams are dropped uncounted. Every other rebuilt transport counts them. Feed the row, with a test that reads it by value. Separate from xdp-stop-abandoned-buffers, which is the send side."
   :date "2026-11-13" :origin "Review before v1.2.0, XDP reader, finding X4")

  (item "rio-lane-trace-harness-state" :group "review-v1-2" :area "proofs"
   :title "The rio-lane trace checks the harness's own slot table, not the transport's"
   :text "tests/tla_trace_rio_lane.cpp calls the real hash, lane_of, packet_id and decode (about 126-140) but keeps every state field itself (slot, issued, staged, starts, strayed, misrouted; about 56-68, 129-211): it picks the slot with its own loop and decides misrouted itself. The transport's slot take and frame check, start_packet, finish_packet, abort_packet and lane staging in sdk/rocketnet_server_rio_windows.cpp (about 485-514, 1713-1744, 4162-4198) never run under the trace, so NoReissue, FinishOwn and OneLanePerDest cannot go red for a library defect. The hash is called with family 1 where the transport passes to->type (134, 240), and the comment at line 11 overstates what is bound. Drive lane start and finish on the no-socket fixture and read the library's state, or say plainly what the layer binds."
   :date "2026-11-13" :origin "Review before v1.2.0, checks reader C1 and Windows reader W3")

  (item "iocp-admit-trace-verdict-only" :group "review-v1-2" :area "proofs"
   :title "The iocp-admit trace binds only the admit verdict"
   :text "tests/tla_trace_iocp.cpp (about 169-194) calls rocketnet_server_iocp_test_receive_admit for the verdict, then pushes to its own bank and increments its own full, refused and counted. rocketnet_iocp_bank_datagram (sdk/rocketnet_server_iocp_windows.cpp about 1309-1325) and the drop counters never run, so EveryDropCounted and Accounted are checked against the harness's arithmetic: a BANK_FULL datagram dropped uncounted stays green. Drive the real bank and read the library's counters."
   :date "2026-11-13" :origin "Review before v1.2.0, checks reader, finding C2")

  (item "xdp-gateway-refresh-trace-order" :group "review-v1-2" :area "proofs"
   :title "The xdp-gateway-refresh trace cannot see the refresh moved"
   :text "tests/tla_trace_xdp_gateway_refresh.cpp (about 192, 240-253, 326) decides when the refresh runs with the walk's own phase string; the real call site (sdk/rocketnet_server_socket_linux.cpp about 673) never runs, and the untraced ReceiveWithoutRefresh is justified by assumption (tools/tla_trace_check.py about 157). The deadline part is bound; the ordering part (the model's RefreshInFill witness) is not, so a refresh moved into the send path stays green. Drive the walk through the real call site."
   :date "2026-11-13" :origin "Review before v1.2.0, checks reader, finding C3")

  (item "finish-message-starting-thread" :group "review-v1-2" :area "sdk"
   :title "finish_message and abort_message from the starting thread are not checked against game threads"
   :text "In sdk/rocketnet_api.cpp (about 3642-3689) and sdk/rocketnet_fill_phase.cpp (about 203-210) the hand-over runs its main thread check only when another thread owns the client. When the owner is the caller or nobody, rn_server_pending_slot scans every connection's plain open.pending while other threads write theirs. rocketnet.h says one thread at a time, and nothing refuses a call that breaks it. Refuse and count it, as the other thread rules are."
   :date "2026-11-13" :origin "Review before v1.2.0, threading reader, finding R3")

  (item "client-acked-batch-full-test" :group "review-v1-2" :area "tests"
   :title "A test that fills the client's delivery batch and reads CLIENT_ACKED_BATCH_FULL by value"
   :text "Every test reads ROCKETNET_COUNTER_CLIENT_ACKED_BATCH_FULL only as == 0 (tests/test_unit_sequence_burn.cpp, sdk/rocketnet_tests.cpp, tests/functional/server_receive_low_tick.cpp). check_counter_observed.py counted it observed through a name a check read only as == 0; with that path closed it is listed in OWED. The server's twin, ACKED_BATCH_FULL, has a unit case (tests/test_unit_client_receive_counters.cpp, the batch that holds two). The client's batch is fixed at ROCKETNET_SENT_PACKET_BUFFER_SIZE (rocketnet_client_protocol.cpp, the ack block's process call), so the case needs a seam that shrinks it, or more acks in one update than the batch holds; then drop the OWED row."
   :date "2026-11-13" :origin "Review before v1.2.0, checks reader, finding FG-G5 (R108)")

  (item "tla-records-reverify-changed-rows" :group "review-v1-2" :area "proofs"
   :title "The nightly re-run re-verifies every RUNS.tsv row changed since it was last verified"
   :text "A hand edit to proofs/tla/RUNS.tsv (MCLink generated/distinct set to 1/1, shown on the bench) still passes tools/check_tla_records.py: the record carries nothing a hand edit cannot fake without re-running TLC. The guard today is the nightly re-run comparer, which checks a third of the cases each night, so a false record is caught within about three nights. Make the nightly selection always include every row changed since that row's last verified re-run, so a changed record is re-run the next night. A point release re-runs nothing (the light point-release rule, 2026-11-13)."
   :date "2026-11-13" :origin "Review before v1.2.0, checks reader (R105)")

  (item "ruleset-apply-reconcile-light-landing" :group "review-v1-2" :area "ci"
   :title "When .github/rulesets/main.json is applied, reconcile its required names with light landing"
   :text "The live ruleset on main carries only deletion and non_fast_forward; the committed .github/rulesets/main.json is deliberately not applied until the first outside pull request (owner, 2026-11-12). It requires the FULL job names, which tools/land.py by default prints but does not hold (light landing, 2026-11-13). Before applying it, make the required names the per-commit names light landing holds, or applying it blocks every light landing."
   :date "2026-11-13" :origin "Review before v1.2.0, checks reader (R102)")

  (item "release-tag-push-ruleset" :group "review-v1-2" :area "ci"
   :title "A tag ruleset so a v* tag is made only by the release tooling"
   :text "Nothing stops a v* tag being pushed by hand around tools/release.py and its gates. A tag ruleset that limits v* creation to the release path closes it."
   :date "2026-11-13" :origin "Review before v1.2.0, checks reader (R110)")

  (item "wire-paths-tie-to-wire-version" :group "review-v1-2" :area "ci"
   :title "Classify the 17 pending wire paths and tie a wire change to ROCKETNET_WIRE_VERSION"
   :text "tools/wire_paths.txt lists 17 files as classification pending since 2026-11-12. Classify them, then make a change to a wire path that leaves ROCKETNET_WIRE_VERSION unchanged need a reviewed reason."
   :date "2026-11-13" :origin "Review before v1.2.0, checks reader (R103)")


  (item "ack-apply-walk-many-ids" :group "review-v1-2" :area "proofs"
   :title "The ack-apply trace walk sends blocks that apply many ids"
   :text "tests/tla_trace_ack_apply.cpp makes every block acknowledge one new sequence with its ranges over sequences already acknowledged, so each apply gives exactly one id; a defect in the apply of a block that acknowledges several new sequences at once (the common case on a lossy link) is never walked. Let the walk draw blocks whose ranges cover unacknowledged sequences, within the batch, and extend RNAckApplyPhase's Receive to carry the id count."
   :date "2026-11-13" :origin "Review before v1.2.0, checks reader, finding FG-A5 (R115)")

  (item "client-connection-integrity-limit" :group "review-v1-2" :area "proofs"
   :title "ClientConnection.tla models the AEAD integrity limit"
   :text "sdk/rocketnet_client_protocol.cpp rocketnet_client_integrity_failure disconnects the client after ROCKETNET_INTEGRITY_LIMIT failed opens (TRANSPORT.md t14); proofs/tla/ClientConnection.tla has no action for it. Add the action, with the limit as a small constant, and a reversed witness that never disconnects."
   :date "2026-11-13" :origin "Review before v1.2.0, checks reader, finding FG-A6 (R115)")

  (item "message-open-model" :group "review-v1-2" :area "proofs"
   :title "A model of the open message per connection and the object's count of open starts"
   :text "sdk/rocketnet_message_object.h (the open-message fields), sdk/rocketnet_message_server.cpp (rn_message_open_line_index and the one-start-pending refusal in the start path) and tests/wide_fill_cases.cpp cited proofs/tla/RNMessageOpen.tla, which was never written; the citations now say there is no model. The state: at most one open start per connection, fill threads opening and closing on different connections at once, the object's count of open starts and its last index as atomics, a reset's cancel (RNSlots ResetCancel). A model would carry the count's arithmetic at capacity 3 with a reversed witness (a second start on one connection admitted) and a trace walk of the wide fill."
   :date "2026-11-13" :origin "Review before v1.2.0, checks reader, finding FG-A4 (R111)")

  (item "msgx-refused-batch-prefix" :group "review-v1-2" :area "sdk"
   :title "msgx may resend a batch whose first packets the kernel already sent"
   :text "sdk/rocketnet_msgx_send_runs.h (about 32-41, 330-374) takes a sendmsg_x that returns -1 as none taken: the run is kept and sent again whole from the same cursor (the worker retries every 1 ms, sdk/rocketnet_server_msgx_apple.cpp about 794-799, 879-903). The repo's own measured note says xnu can deliver part of a batch and still return -1 with no count (sdk/rocketnet_platform.cpp about 505-512, 2692-2701), and the client and portable pump handle that; msgx does not. If an ENOBUFS at peak does this, the delivered prefix goes out two or more times into a full queue: duplicates the client's replay window drops, sent counters under the wire, SEND_FAILED over on the error path. The model (RNMsgxSendRuns.tla) has the same assumption in its fake kernel. Open until measured: one peak row on the 10 GbE pair (received against packets_sent with send_dropped 0) decides; a loopback probe of 2000 batches of 64 never saw a refusal."
   :date "2026-11-13" :origin "Review before v1.2.0, macOS reader, finding P3")

  (item "strict-keep-off-cpu-zero" :group "review-v1-2" :area "sdk"
   :title "A strict option: refuse to start when a Linux transport worker cannot be kept off CPU 0"
   :text "When the pin and then the fallback confinement are both refused (XDP leave_core_zero in sdk/rocketnet_server_socket_linux.cpp about 4236-4256, io_uring confine_worker in sdk/rocketnet_server_uring_linux.cpp about 411-431, and gso's fallback), the worker carries on with the inherited mask, CPU 0 included, for good: a cpuset limited to CPU 0 does this. The count and the warning are being fixed for v1.2.0. Left: an option, off by default, that makes the server refuse to start instead when the host has more than one CPU (a one-CPU host must still run)."
   :date "2026-11-13" :origin "Review before v1.2.0, Stella's finding S3")
 ))
