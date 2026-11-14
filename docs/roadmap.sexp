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
 :text "Planned work for RocketNet that is not on main. Work lands on main before new work starts. Branch names are plain branches holding started work to resume from. Landed work is in git. Pull requests edit only docs/roadmap.sexp; this page is regenerated on main."

 :groups
 (
  (group "audit-remaining" :title "Suspected defects not yet fixed"
   :text "Suspected defects found by review that nothing on main fixes. Each is verified at the cited line first; a confirmed defect gets a lock-in test and a negative control.")
  (group "quiet-host" :title "Investigations that need a quiet host"
   :text "Findings that only a quiet fleet host can measure or reproduce.")
  (group "proof-debt" :title "Existing nightly proof debt"
   :text "Incomplete proof measurements remain explicit failures under the existing nightly workflows. These are distinct from the model and code correspondence repairs already on main.")

  (group "after-v1-0-1" :title "Found while preparing v1.0.1"
   :text "Defects, gaps and measurements found by the v1.0.1 review and test campaign that a point release does not carry. Each is checked at the cited code first.")
  (group "trace-check" :title "Trace check: gaps the README declares"
   :text "What proofs/tla/README-TraceCheck.md says the trace check trusts rather than enforces (v2, 2026-11-12, after three reviews). Each item says what enforcing it would take.")
  (group "decisions" :title "Decisions"
   :text "Choices that change behavior, remove machinery, or touch repository and fleet settings.")
  (group "after-v1-1-landing" :title "Found while landing and re-measuring the v1.1.x transport work"
   :text "Known and not done after the transport work landed on main as v1.1.x point releases and was measured again from main (2026-11-13). Each says where it is and how it was found.")
  (group "review-v1-2" :title "Found by the review before v1.2.0, not fixed for it"
   :text "Findings of the review that started on 2026-11-13 that are not being fixed for v1.2.0. Read, not run, unless an entry says so; each names the file and lines.")
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
   :text "The iocp and rio transports do not declare ROCKETNET_TRANSPORT_REFLUSHES (iocp_operations and rio_operations in sdk/rocketnet_driver_builtin.cpp), so send_packets flushes them only after a new commit or at shutdown (the REFLUSHES test in rocketnet_server_transport_flush, sdk/rocketnet_server_transport.cpp); gso, io_uring, msgx and XDP (when built) declare it. Measure on a Windows host whether sends left unfinished at one flush wait for the next commit; add re-flush, with counters, if the measurement supports it."
   :date "2026-10-31" :origin "sdk/rocketnet_driver_builtin.cpp; sdk/rocketnet_server_transport.cpp")


  (item "load-md-generated" :group "audit-remaining" :area "docs"
   :title "Generate LOAD.md's byte tables"
   :text "The page's exact byte figures go stale with every merge. Generate them, with a CI check that the page matches the generator. The same goes for the planner figures quoted in docs/MEMORY.md (the default and light server and client plans, the DEV=0 figures and the example program's output): one generator for both pages, printing every cell from rocketnet_*_required_memory, rocketnet_message_connection_sizing and bin/transport_memory."
   :date "2026-10-31" :origin "#721 follow-on")

  (item "audit-follow-on-proposals" :group "decisions" :area "tools"
   :title "Optional follow-on proposals"
   :text "The concrete low-severity docs, standard, test and tooling mismatches are repaired. Four suggestions introduce new requirements: extend the make-jobs policy to cmake --build --parallel; cross-check each t8 field-table sum against its summary row; add in-loop protocol assertions to the structured receive fuzzer; change the socket-error loopback burst/deadline. Evaluate these separately; they are not unresolved violations of the existing gates."
   :date "2026-11-01" :origin "#835, #837, #838, #839")

  (item "xdp-receive-loss" :group "quiet-host" :area "rocketnet"
   :title "Loaded-host XDP receive loss report: not reproduced"
   :text "The requested quiet reproduction is complete. At SDK source 3e8e4d17 on 2026-11-01, the existing 1000-client, 60-second XDP cell passed with host and guest both on 7.0.0-38-generic: 600000 sent/received, zero drops, and descriptor/thread baselines restored. The earlier 7.0.0-34 guest pass is separate evidence. The reported loss under host load 170 did not reproduce; its cause remains unknown. This controlled native observation establishes neither an SDK fix, a per-frame-budget result, nor a green GitHub workflow run."
   :date "2026-10-31" :origin "A report of XDP receive loss under host load; quiet/current-kernel observation 2026-11-01")




  (item "decision-resend-cap" :group "decisions" :area "rocketnet"
   :title "Cap the single-packet resend delay at timeout/4"
   :text "With message_resend_delay_max_ms above reliable_message_timeout_ms/4, a single-packet record gets fewer than four sends before timeout. Recommended: cap it as chunks are. Then both resend classes share one delay, and the two resend queues could collapse to one; that removes machinery."
   :date "2026-10-31" :origin "Review of the reliable message resend timing")



  (item "decision-unit-env" :group "decisions" :area "tests"
   :title "Unit tests and the process environment"
   :text "Unit cases still reach the environment through the public creates, safe only because the runner clears every name the SDK reads. Stricter: route about 320 creates through the injected-environment variants. Recommended: keep the runner boundary. Also move the XDP gateway address read from socket open to create."
   :date "2026-10-31" :origin "#794")

  (item "apply-main-ruleset" :group "decisions" :area "github"
   :title "Apply the main ruleset before the first outside pull request"
   :text "Main has no ruleset on GitHub that requires a check, on purpose for now. Apply .github/rulesets/main.json on GitHub before the first outside pull request. Left off for now."
   :date "2026-11-12" :origin ".github/rulesets/main.json")

  (item "frame-budget-runner-label" :group "decisions" :area "fleet"
   :title "A runner label for the nightly frame budget held only by two fleet runners"
   :text "rocketnet-quiet also covers a third, different machine; the frame budget is a wall-clock gate that compares only on one machine class."
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
   :date "2026-11-11" :origin "v1.0.1 xdp review")

  (item "xdp-receive-time-seam" :group "after-v1-0-1" :area "rocketnet"
   :title "xdp receive path reads the platform clock"
   :text "The xdp receive path still reads rocketnet_platform_time() instead of the update's time. Passing the time in needs the driver seam widened."
   :date "2026-11-11" :origin "v1.0.1 xdp gateway review")

  (item "xdp-tx-queue-hash" :group "after-v1-0-1" :area "rocketnet"
   :title "xdp TX queue choice is an unkeyed hash; gso send bank at 32768 clients"
   :text "xdp picks the TX queue with an unkeyed FNV-1a of the destination (rocketnet_server_socket_send_queue), so spoofed sources can aim replies at one queue: key it per server."
   :date "2026-11-11" :origin "v1.0.1 review")

  (item "load-split-xdp-flaky" :group "quiet-host" :area "rocketnet"
   :title "load_split xdp server with portable clients fails about 1 run in 4 or 5"
   :text "The two-machine load_split delivery proof with an xdp server and portable clients failed 1 of 5 runs on v1.0.1 (one client RELIABLE_TIMED_OUT) and 1 of 4 on v1.0.0 (missed due slots), so it predates v1.0.1. Find each cause on quiet hosts."
   :date "2026-11-11" :origin "v1.0.1 test campaign, two-machine transport cells")

  (item "uring-rotation-reordering" :group "quiet-host" :area "rocketnet"
   :title "Measure reordering from io_uring per-send socket rotation"
   :text "Since v1.0.1, io_uring picks the send socket per send run instead of per batch, so consecutive packets to one client can leave on different reuseport sockets within one update. Measure any extra reordering against v1.0.0."
   :date "2026-11-11" :origin "v1.0.1 review")

  (item "uring-receive-view-640" :group "after-v1-0-1" :area "rocketnet"
   :title "io_uring receive view larger than the payload batch"
   :text "Below 640 clients the io_uring receive view is larger than the server payload batch. Check what the server does with the difference and pin it."
   :date "2026-11-11" :origin "v1.0.1 server transport review")

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
   :date "2026-11-10" :origin "Requested, 2026-11-10")

  (item "transports-shared-setup" :group "after-v1-0-1" :area "rocketnet"
   :title "Transports repeat themselves: share socket setup"
   :text "The transports are large and repeat each other: socket setup is duplicated between gso and io_uring and between iocp and rio. Share it."
   :date "2026-11-10" :origin "Requested, 2026-11-10")

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
   :date "2026-11-10" :origin "Requested, 2026-11-10; docs/runs/xdp-zero-copy.md")

  (item "trace-spec-invariants" :group "trace-check" :area "proofs"
   :title "Check the spec's own invariants on every trace"
   :text "The trace cfg allows INVARIANT TraceConsistent only, so the spec's invariants are checked at the model's small sizes and never on the trace at the code's real sizes: code that follows a model which is wrong at 4096 slots passes. Enforcing it: an invariants list in each LAYERS row, the checker verifying each is a plain state predicate of the spec (the reach-predicate check already does this) and allowing INVARIANT lines for exactly those names, the writer emitting them, and a planted row whose model breaks an invariant only past its TLC size. Cost per layer: no extra TLC run, since the invariants are evaluated in the clean trace's existing run, once per record (401 at the default 400 steps). An invariant over a fixed array costs about slots x records evaluations, about 1.6 million for a 4096-slot ring at 400 steps, a few seconds on the bench and unmeasured until the first such layer lands. A plant trace stops at its rejection, and the plant run must not take the invariants, or a planted invariant break could stand in for the trace rejection the plant must cause."
   :date "2026-11-12" :origin "Trace check v2 review (README 'What the check trusts')")

  (item "trace-library-copy" :group "trace-check" :area "tools"
   :title "Refuse a copy of library code in a layer's test file"
   :text "The walk must call the library, not a copy of it left in tests/tla_trace_<layer>.cpp beside a real sdk include. Today a reviewer checks this by reading the file. Enforcing it: after the clean build, run nm on the layer's object file and refuse any defined symbol with the library's prefix (rocketnet_) or any name the sdk files define. Also refuse a layer file that defines a function an sdk header declares. This needs the object path from CMake ($<TARGET_OBJECTS:tla_trace>, written next to tla_trace_config.txt) and nm or dumpbin on each OS. Add a planted row that copies one sdk function into the example."
   :date "2026-11-12" :origin "Trace check v2 review")

  (item "trace-config-per-source" :group "trace-check" :area "tools"
   :title "Read the trace binaries' configuration from the layer's own unit"
   :text "CONFIG is printed by tests/tla_trace.cpp, so a COMPILE_DEFINITIONS set on one layer's source file, or a configuration macro without the ROCKETNET_ prefix, changes the layer's code without the checker seeing it. Enforcing it: CMake refuses source-file COMPILE_DEFINITIONS on tests/tla_trace_*.cpp. Each layer's walk prints CONFIG through a macro in tla_trace_record.h expanded in its own unit. The macro list is every identifier an #if of the included sdk files tests, read from those files, not a fixed list of ROCKETNET_ names. Add planted rows for a per-source define and for an unprefixed macro."
   :date "2026-11-12" :origin "Trace check v2 review")

  (item "thread-rules-nesting-model" :group "proof-debt" :area "proofs"
   :title "Model the nested calls and the refused look's three writes in RNFillPhase"
   :text "proofs/tla/RNFillPhase.tla leaves out the nesting depths, and takes a refused look (sdk/rocketnet_fill_phase.cpp rocketnet_fill_phase_fill_begin: the word, the owner and the word again) as one step. A review found an interleaving in that gap: a call that a main thread call ran inside puts the owner back while the word still reads as held at depth 1, the thread it gives the client back to reads its own next call as nested, and its end leaves a fills_in line it never entered, after which every main thread call is refused. Fixed by the order of the writes (the word goes back to the lock's form first); no test fails without the fix and no model step sees it. Add the nested path and the three writes to the model with a reversed witness for the old order, and a trace point between the writes."
   :date "2026-11-13" :origin "Review of sending from the game threads")

  (item "payload-ring-chunk-layout-model" :group "proof-debt" :area "proofs"
   :title "Model the chunked payload ring claims at the layout level"
   :text "rocketnet_server_payload_ring_take (sdk/rocketnet_server_transport.cpp) claims payload ring slots in chunks held on per-thread lines, with index arithmetic on a packed [end:32][next:32] word, a guard, a clip at send_capacity and a give-up when another thread on the line installs first. proofs/tla/RNServerPayloadRingLayout.tla still models one fetch-add a slot. Model the same arithmetic at capacity 3 or 4 with reversed witnesses: a lost install that keeps its first slot (one client's packets then leave out of start order; test_server_fill_ring_chunk_lost_install_keeps_start_order pins the fix), and a line's unused slots not marked empty before the flush."
   :date "2026-11-13" :origin "Review of sending from the game threads; the layout rule")

  (item "send-reserve-tails-named-implied" :group "proof-debt" :area "proofs"
   :title "RNSendReserve: TailsNamed is implied by NeverStarved"
   :text "As defined, with no other thread holding a tail RoomFor and RoomPastTails are the same predicate, so unnamed implies starved: the invariant TailsNamed adds nothing NeverStarved does not check, and its witness MCRNSendReserveBrokenNoTailReturn.cfg would fail NeverStarved too. The model also marks the counter choice at the decision, where the code reads the tails after the take back and the retest. Restate TailsNamed so it can fail alone, and move the choice to where the code makes it."
   :date "2026-11-13" :origin "Review of sending from the game threads (TLA+)")

  (item "thread-rule-counters-production-test" :group "audit-remaining" :area "tests"
   :title "Read the three thread rule counters on the production library"
   :text "MAIN_THREAD_CALL_OVERLAP, CLIENT_SEND_TWO_THREADS and SEND_RESERVE_REFUSED_TAILS_HELD are counted in every build, and each is read by value in a test, but every such test links the development library (bin/test_wide_fill, tests/test_unit_server_send_reserve.cpp). Add a case to the production functional binary that breaks each of the two thread rules through the public API and reads the counters; the same case can check the API misuse count of a message left open at update, which no test reads."
   :date "2026-11-13" :origin "Review of sending from the game threads (counters and tests)")

  (item "ring-order-across-thread-change" :group "audit-remaining" :area "sdk"
   :title "Start order of one client's packets when its sending thread changes between flushes"
   :text "On a transport that buffers payloads in the server's ring, a thread's unused chunk slots stay on its line until the flush. If one thread sends to a client, a checked main thread call that is not send_packets follows, and another thread then sends to the same client, the second thread's packets may sit in lower slots and leave first. rocketnet.h states it. Returning every line's chunk at each checked main thread call would keep start order; it costs a walk of the 64 lines a call."
   :date "2026-11-13" :origin "Review of sending from the game threads")

  (item "msgx-partial-send-no-sleep" :group "audit-remaining" :area "sdk"
   :title "Do not sleep after a partial sendmsg_x"
   :text "A partial sendmsg_x result ends the call kept (sdk/rocketnet_msgx_send_runs.h), so the transmit worker marks the socket full and waits 1 ms (sdk/rocketnet_server_msgx_apple.cpp) although the socket took data. Loop again at once when the kept call made progress, and wait only when a call took nothing. Measured nowhere yet: the four space rows pass as it is."
   :date "2026-11-13" :origin "v1.1.x review")
  (item "posix-hk-mark-after-last-send" :group "audit-remaining" :area "tests"
   :title "Hard-killed client cases: take the wire mark after the clients read the last pass's sends"
   :text "posix_hk_quiesce (tests/functional/posix_hard_killed_client.cpp) updates the clients, then the server, then sends the server's packets, so a datagram the server sends in the last pass is counted sent at posix_hk_wire_mark and received after it. That datagram hides one real loss in lost_down, and a payload lost on the way then fails posix_hk_require_accounted with lost 0. Seen once on a parallel macOS run (send_to_closed_port, server portable, ipv6, client msgx: down 79/80, lost 0). Fix: end the quiesce with a client-only read pass before the mark, or mark the client and server reads at the same point."
   :date "2026-11-13" :origin "v1.1.x CI review")

  (item "dst-mirror-observations" :group "review-v1-2" :area "proofs"
   :title "DST: the observations the narrowed mirrors still owe (crash, clock, opening, receipts)"
   :text "COVERAGE.tsv now states what the DST checks in tests/dst_checks.h actually observe. Still owed: a crash fault so CrashedEndpointSilent can fail (the harness passes live endpoints only); a ClockMonotone check (clock-back steps a clock back on purpose, and RNSimFaults' SkewBounded bound grows with t while the DST uses the fixed bound it is passed); an open ledger on the receiving side so AckedMeansPacketOpened and AckReportsOnlyReceived mean more than issued-once; for AckedMeansSurfaced, a comparison of the sender's internally acked records against the ids that entered its ACK batch (the DST already sees the surfaced ids); fragment observation for BlockAckedOnlyWhenAllFragmentsAcked (a type 7 behind an earlier undelivered sequence passes unobserved); a type 6/7 deadline for ReceiptProgress; and a replay-bit read for HighestIsReceived. Add each observation to the DST harness and restore the full mirror claims."
   :date "2026-11-13" :origin "Models review M2 (MM013-MM018): claims narrowed to the checks; observations deferred here")

  (item "msgx-dead-socket-placeholder" :group "review-v1-2" :area "sdk"
   :title "macOS msgx: stop a socket retired as dead from taking its client's datagrams until it closes"
   :text "A connected socket retired as dead (rocketnet_msgx_socket_table_sweep) stays open and connected through its quarantine, one quiescent pass, normally until the next sweep, so the kernel keeps delivering that client's datagrams to it and not to the shared socket; each is counted RECEIVE_DROP_RETIRED at the close (sdk/rocketnet_server_msgx_apple.cpp rocketnet_server_msgx_table_close), none is lost unnamed. Design: at the retire, count the datagrams it holds (SO_NUMRCVPKT) and dup2 a placeholder descriptor over the fd, which closes the real socket at once (later datagrams reach the shared socket) and keeps the fd number reserved until the quarantine close. Cost: a send worker that resolved the fd before the retire sends on the placeholder and gets ENOTSOCK instead of reaching the old remote, which changes the quarantine promise of proofs/tla/RNMsgxSocketTable.tla (a resolved socket stays open and connected to its remote until the pass ends); the model, the send path's error kinds and the counts change with it. The trigger (a receive errno that marks a connected UDP socket dead) is not established; take it only if the path is seen."
   :date "2026-11-13" :origin "Review round 1, R70 (FW3); owner's call: leave as counted")

  (item "msgx-socket-opens-off-main" :group "review-v1-2" :area "sdk"
   :title "macOS msgx: open client sockets off the main thread"
   :text "The replay walk's mark (rocketnet_server_msgx_mark) opens up to ROCKETNET_MSGX_OPENS_PER_UPDATE (64) connected client sockets an update on main (socket, bind with SO_REUSEPORT, two setsockopt, connect, a kevent registration: rocketnet_server_msgx_table_open), bounded but not free. Moving the opens to a worker would hand main a ready descriptor through the table the way the receive banks hand datagrams; it changes RNMsgxSocketTable's open path and its model. Measure the walk's open cost on a churn row first."
   :date "2026-11-13" :origin "Review round 1, R70 (FW2, second part)")

  (item "msgx-port-exclusive" :group "review-v1-2" :area "sdk"
   :title "macOS msgx: keep the server's port from other local sockets"
   :text "The msgx shared socket sets SO_REUSEPORT so its per-client connected sockets can join the port, and so any later local socket that sets SO_REUSEPORT can bind the port too; a more specific one (the server's own address, or AF_INET beside a dual-stack [::] server) then receives every datagram meant for the shared socket, handshakes included, uncounted (measured, one user; docs/MACOS.md, Who else can bind a server's port). Darwin has no exclusive option that still lets the server's own sockets share the port. A real fix: drop the per-client connected sockets and demultiplex every client on the shared socket, so the shared socket needs no SO_REUSEPORT; with a specific bind address no later socket then receives its datagrams, as measured for a plain socket on a specific address. Costs: the per-client sockets' receive split and send path, to be measured against the load test row before it is taken. Also owed: run the intruder cases as a second user on a bench Mac (not measured)."
   :date "2026-11-13" :origin "Review before v1.2.0 (run, one user)")

  (item "iocp-peak-refusals" :group "review-v1-2" :area "sdk"
   :title "Windows iocp: refused message starts at the 1000-player peak"
   :text "At the 1000-player peak, the current Windows receive-fix measurements with default IOCP placement remain MARGINAL: loaded peak sends 898.4-899.4k packets/s and refuses 18,663-56,479 starts per run; unloaded peak sends 900.0-900.3k and refuses 1,558-8,999. Both average workloads pass three of three runs with zero refused starts. The record is docs/runs/performance-native.md, the WF rows, summarized in docs/PERFORMANCE.md: two completion workers and two send threads on the stated Windows machine and RSS layout. The receive fix is awaiting hot-path integration; these rows do not qualify the assembled release. They record the rate, refusals and receive timing, not the component responsible for the remaining refusals. Next step: identify that cause on the documented peak workload before choosing a library or deployment change, then verify both throughput and receive delay. RIO is the Windows transport of choice for this peak workload with its measured placement; IOCP is the fallback."
   :date "2026-11-13" :origin "v1.2 audit, Windows runs")

  (item "rio-destroy-stuck-lane" :group "audit-remaining" :area "rocketnet"
   :title "rio destroy: free the stopped lanes when one lane is stuck"
   :text "rocketnet_server_rio_destroy returns before destroying any lane when one lane never reports stopped (a completion queue reported corrupt, or a send the kernel never completes). The lanes that did stop stay allocated until process exit. Measured as intended for this change: the leak is bounded and named. Fix: destroy every lane that reports stopped and leak only the stuck one, as the create rollback already does (rocketnet_server_rio_create_leaks), with a unit case that holds one lane's sends."
   :date "2026-11-13" :origin "Review of the rebuilt rio transport")

  (item "gso-layout-model-send-step" :group "audit-remaining" :area "proofs"
   :title "Layout model coverage of the gso send step's order, held slot and plain resend"
   :text "proofs/tla/RNGsoQueueBankLayout.tla models the claim, the id and the slot rule. lane->order (the gather's positions), send_step's held_slot and the position it resumes from, and resend_plain's first_slot arithmetic index the banks too and have no layout model or trace layer; they are covered by unit cases only."
   :date "2026-11-13" :origin "v1.1.x review")

  (item "gso-packet-id-zero-queue-zero" :group "audit-remaining" :area "sdk"
   :title "Packet id 0 names queue 0 slot 0 of turn 0"
   :text "The gso packet id is ( turn * slots + slot ) * queues + queue, so the first packet of the first turn on queue 0 has id 0. Nothing treats 0 as no packet today (finish and abort check the pointer against the slot), but a caller that keeps 0 as its own empty value would lose that packet. Decide whether the id should start at 1."
   :date "2026-11-13" :origin "v1.1.x review")

  (item "send-reserve-multi-queue-capacity" :group "proof-debt" :area "proofs"
   :title "RNSendReserveCalls: a Capacity per send queue"
   :text "send_capacity_available is one number: with several send queues it is the fewest free slots of any queue, an upper bound on what the next start may find. The send reserve's per-queue room (send_queues, send_queue_of, send_room_queue) is read by the reserve, but RNSendReserveCalls models one Capacity. Model a Capacity per queue, bind the code's call to it in the trace check, and say in the transport standard what a start on a queue with room may rely on. The trace check is narrower than that today: tests/tla_trace_send_reserve.cpp (213-221) only ever runs one queue, so the per-queue reserve code in sdk/rocketnet_server_transport.cpp (about 1060-1093, 1207-1253) is on no checked trace and a defect there cannot turn it red. Walk several queues."
   :date "2026-11-13" :origin "v1.1.x review; review before v1.2.0")

  (item "portable-server-shared-handoff" :group "audit-remaining" :area "sdk"
   :title "Portable server transport on the shared buffer handoff"
   :text "PR #1043 (refactor/v1.1.x/server-shared-handoff) moved the portable and gso server transports off their hand-written bank queues onto rocketnet_buffer_handoff_t (sdk/rocketnet_buffer_handoff.h). Its gso half is overtaken by the rebuilt gso transport (#1103), which rewrites sdk/rocketnet_server_gso_linux.cpp and also changes sdk/rocketnet_driver_builtin.cpp. After the rebuilt gso lands, redo the portable half alone on main (the portable TX and RX commits and the rocketnet_buffer_handoff_started helper), with its gates: rnportabletx, rnportablerx and rnbufferhandoff TLC, the unit mutation cases and the light memory pins. With it, give the portable receive worker what the rebuilt transports have: it runs no prefilter (sdk/rocketnet_driver_builtin.cpp about 613-645, drop hooks NULL about 1962-1966), so junk reaches the main thread (bounded per update), and datagrams held in its banks at stop are counted nowhere (about 932-944), so RECEIVE_DROPPED_STOPPED reads 0 on portable. Not worse than v1.0.0."
   :date "2026-11-13" :origin "#1043, not landed; review before v1.2.0")

  (item "rio-registered-region-red-zones" :group "audit-remaining" :area "tests"
   :title "ASan red zones between the rio registered region's arrays"
   :text "In an AddressSanitizer build the rio transport's sized block (rocketnet_rio_sized_layout: slot bytes, retry slices, process arrays, drops, each bank's data, from and packet bytes) carries poisoned red zones between its carves. The registered region laid out by sdk/rocketnet_rio_layout.h (send frames, send addresses, receive slices, receive addresses; one RIORegisterBuffer) does not: its offsets are the layout RNRioLaneLayout models and the trace check binds, so a pad there changes a modelled layout. Add the pads to the layout and its model together, or record why that region stays uncovered."
   :date "2026-11-13" :origin "#1047 merge with the rebuilt rio")

  (item "sodium-windows-lock-owner" :group "audit-remaining" :area "sdk"
   :title "Owner-aware leave for the vendored sodium's Windows lock"
   :text "sodium_misuse leaves sodium's critical section before taking it again. On POSIX the vendored leave refuses unless this thread holds the lock (the holder's pthread_self recorded under the lock and read with atomics, tools/generate_sodium.py LIFECYCLE_ADAPTATIONS, sodium/README.md \"POSIX lock\"). The Windows branch keeps upstream's check of the process-wide locked flag, so a fatal misuse on a thread outside the section while another thread holds the SRW lock clears that thread's flag and releases a lock it does not own. Reachable only on the abort path. Give the Windows branch the same owner test (GetCurrentThreadId recorded under the lock, read with Interlocked operations), carried through the generator; never a thread-local, which a production build may not carry (tools/check_no_thread_local.py)."
   :date "2026-11-13" :origin "fix-review build-sanitizers 4703c70f0 B02")

  (item "uring-lifecycle-rss-plan" :group "audit-remaining" :area "tests"
   :title "The io_uring provided buffers are 264 MiB a queue whatever the GRO tier"
   :text "With ROCKETNET_SERVER_TRANSPORT=io_uring forced over the whole functional suite, test_lifecycle_transport peaks at 1269 MB resident against 698 MB, which is the suite's per-case bound (suite_case_rss_mb), not a plan bound. The cause is the receive buffers: 4096 buffers of 66 KiB a queue (ROCKETNET_URING_RECV_BUFFERS, ROCKETNET_URING_RECV_BUFFER_BYTES), 264 MiB a queue and two queues by default (num_io_uring_workers), made resident once the multishot writes into them, whether or not UDP_GRO was granted. Size the buffers by the tier (a buffer need only hold a whole datagram without GRO) or lower the count, and say so in the plan."
   :date "2026-11-13" :origin "Review of the io_uring transport work, and a bench run")

  (item "uring-cancel-ealready" :group "audit-remaining" :area "sdk"
   :title "A cancel answered -EALREADY in the io_uring worker"
   :text "The worker counts a cancel it submitted when its completion arrives (rocketnet_uring_queue_worker_reap_one, ROCKETNET_URING_TAG_CANCEL). A cancel the kernel answers -EALREADY (the target already completed) reads the same as one that took effect; the model (RNUringQueueWorker.tla CancelRecv, CancelWake, CancelPoll) has one outcome. Check the worker against the answers and add the case to the model."
   :date "2026-11-13" :origin "Review of the io_uring transport work")

  (item "flaky-reply-opens-a-later-update" :group "audit-remaining" :area "tests"
   :title "test_transport_reply_opens_a_later_update failed once on a loaded macOS runner"
   :text "On PR #1124's first CI run (43476345841, attempt 1) macos / Release / functional on one macOS runner failed at tests/functional/rocketnet_functional_tests.cpp:10204: one of the 200 replies was never seen within the case's 12000 updates (9.8 s). The case sends unreliable datagrams over real loopback and answers each once, so one lost request or reply fails it. The rerun passed on another macOS runner, and the case passed 40 of 40 on a Linux bench both at #1124's head and at its base a756b3398. The cause is not shown. PR #1118 (transport reply waits resend through the loss gate) is the candidate fix, held until it shows the cause."
   :date "2026-11-13" :origin "PR #1124 CI")

  (item "uring-offline-cpu-holes" :group "audit-remaining" :area "sdk"
   :title "Offline cpu holes in the io_uring pin"
   :text "rocketnet_uring_pin_check refuses a cpu at or past the online count, but a machine with cpus offline has holes below that count (cpu 5 offline, cpu 6 online). A pin to a hole is refused by the kernel, counted, and the worker is confined to its own mask past the OS's cpus (rocketnet_uring_pin_confine_words, which never names a hole), so it is placed but not where the config said. Read the online mask (sched_getaffinity or /sys/devices/system/cpu/online) and place the workers on online cpus past the OS's."
   :date "2026-11-13" :origin "Review of the io_uring transport work")

  (item "transport-thread-start-mask" :group "audit-remaining" :area "sdk"
   :title "A transport thread runs on its creator's mask until its first call"
   :text "Every transport thread (io_uring and gso queue workers, the XDP thread, and the other transports' network threads) pins itself as its first call, so between rocketnet_platform_thread_create and that call it runs on the creating thread's mask, which may hold cpu 0 (the OS's on Linux); the gso creator also raises the priority in that window. Give rocketnet_platform_thread_create an optional affinity (pthread_attr_setaffinity_np on Linux) so the thread starts on its placement: one place in the platform layer, used by every transport. Also: _SC_NPROCESSORS_ONLN is a count, used as the highest cpu id, so a machine with an offline cpu in the middle is misjudged (read the online set instead)."
   :date "2026-11-13" :origin "v1.1.x review; review of the io_uring transport work")

  (item "uring-receive-folds" :group "audit-remaining" :area "sdk"
   :title "io_uring source drops: one per completion, where gso counts datagrams"
   :text "The fold has landed: a truncated source (URING_RECEIVE_DROPPED_SOURCE) and a completion naming no buffer (URING_RECEIVE_DROPPED_NO_BUFFER) are added into RECEIVE_FAILED (rocketnet_uring_receive_queue_drops, standard/TRANSPORT.md's alias table), and segments past one receive (URING_RECEIVE_DROPPED_OVERFLOW) alias no shared row and are read by the clean-run gate in their own row. The unit is open: a source drop is counted once per completion, even for a GRO batch of several datagrams (sdk/rocketnet_uring_receive_queue.cpp about 598-624), where gso counts each datagram into RECEIVE_FAILED (sdk/rocketnet_server_gso_linux.cpp about 1052, 1197-1205), and the row's own doc (sdk/rocketnet_counters.h, URING_RECEIVE_DROPPED_SOURCE: 'Unit: datagrams') disagrees with TRANSPORT.md (one per receive). Decide the unit, make the code and both docs say it, and test it by value with a several-datagram batch."
   :date "2026-11-13" :origin "Review of the io_uring transport work; review before v1.2.0")

  (item "uring-complete-without-more" :group "audit-remaining" :area "sdk"
   :title "RNUringQueueReceive: Complete without F_MORE"
   :text "The model's Complete takes a datagram completion and keeps the multishot armed; the code (rocketnet_uring_receive_queue_cqe) also ends it when the completion has no IORING_CQE_F_MORE and arms again on the next pass. Add the ending to Complete with a reversed witness, and a trace point."
   :date "2026-11-13" :origin "Review of the io_uring transport work")

  (item "uring-gro-cmsg-one-datagram" :group "audit-remaining" :area "sdk"
   :title "A malformed UDP_GRO control message is passed as one datagram"
   :text "A receive whose control area is truncated or malformed (GRO_CMSG_FAULTS) is taken as one datagram, so a coalesced buffer of several datagrams reaches the prefilter as one long one and fails its checks there. Decide whether the receive module should drop it by name instead, with a counter row, and test the split on a malformed control area."
   :date "2026-11-13" :origin "Review of the io_uring transport work")

  (item "uring-rearm-spin" :group "audit-remaining" :area "sdk"
   :title "The io_uring receive re-arm spins on a kernel that refuses multishot"
   :text "A kernel that refuses multishot receive (Linux before 6.0) ends every arm at once; the worker arms again on its next pass without a bound, burning its core. The refusals are visible as RECEIVE_FAILED now (the receive module's ERRORS reach the drop totals). Bound the re-arms, back off, and name the kernel in the create log."
   :date "2026-11-13" :origin "Review of the io_uring transport work")

  (item "tlc-plant-behaviour-not-printed" :group "trace-check" :area "tools"
   :title "Find why TLC sometimes prints no behaviour for a plant's violation"
   :text "TLC 2.19 exits 12 on a plant trace and reports the invariant violated, then sometimes prints no behaviour; the statistics lines that follow are printed, so the log is not cut off. tools/tla_trace_check.py reruns such a plant up to five times and prints PLANT-ATTEMPTS (#1106). Ruled out on byte-identical inputs of the msgxsendruns seed 1 plant (one Linux host, 2026-11-13): TLC's -seed and -fp index (both pinned, -workers 1: a behaviour in 5/10, 6/10 and 7/10 runs for three pairs) and a shared or reused metadir (a fresh directory and -metadir each run). Start from a TLC-side cause: the trace file's write and read-back at the violation."
   :date "2026-11-13" :origin "#1093 trace shard 0/6")

  (item "reconcile-sndbuf-resends" :group "after-v1-0-1" :area "tests/load"
   :title "Reconcile: tell a refused-then-resent send from a real drop"
   :text "The harden_load reconcile counts the kernel's SndbufErrors as named drops in the server-to-client chain. When the 1 MB socket send buffer is full the kernel refuses a gso send and the gso worker sends it again, so nothing is lost: on main 3218cf5f6 (X710 pair) library_sent equals library_received and players deliver 100%, yet every gso row reports chains=FAIL on 124k to 571k of them (the gso re-measure). Count the refusals the transport resent in their own term, apart from sends the kernel refused and nothing sent again, so the chain still fails a real drop."
   :date "2026-11-13" :origin "gso re-measure on main 3218cf5f6")

  (item "fill-phase-trace-gaps" :group "trace-check" :area "tests"
   :title "Fill-phase trace check: the gaps the split-update fix left"
   :text "tests/tla_trace_fill_phase.cpp now reads the window and both refusal counters from the library and its reach predicates require every refusal step (MainLook, MainIn, FillLock, FillDecide, FillLook in and out of the window, TakeLook). Still open: fbroke, phaseIn, falseAlarm, mainsDone, fillsDone and takesDone are the walk's own bookkeeping, compared but read by Next, so they cannot be declared ghosts as the checker defines them; the compare-and-swap retry loop in fill_begin and main-thread nesting (main_owner, main_depth) never run under the trace. Fill-call nesting (depth above 1, FillPeek, FillNest, FillUnnest) is modelled and walked since the nested-accept fix. Model main nesting and drive it from the walk, and say for each walk-kept variable what in the library it stands for. The split-update pass 2 rules are modelled (stage, NoKickOnOpenUpdate, PlainChecked, StageShut) but the walk drives a bare check with no message server: stage is the walk's own bookkeeping of what its update calls left, and the kick, the plain finish and abort, and the window following the stage are bound only by the native tests in tests/wide_fill_cases.cpp (split_update_open_update_call, plain_finish_checked, split_update_window_on_early_return). Drive a message server under the trace so these are bound to the code."
   :date "2026-11-13" :origin "review fixes S1/S2, coordinator finding C7; split-update pass 2 (S172, S175, S178)")

  (item "writer-finish-sequenced-order" :group "trace-check" :area "tests"
   :title "RNWriterFinish: the sequenced order hold, modelled and traced"
   :text "RNWriterFinish and tests/tla_trace_writer_finish.cpp cover plain unreliable and reliable messages. An UNRELIABLE_SEQUENCED message is also held behind its channel's queued reliable work or the ordered resend list (rn_endpoint_in_place_header, behind_queue in rn_message_object_start) and so queued on a staged transport, as the header's order exception allows, including after a short finish's restart. Add a sequenced kind whose behind outcome depends on held and resend work, scope StagedQueuesNothing to what is not order-held while keeping BrokenDowngrade sensitive to a wrongful queue, and have the walk start sequenced messages on more than one channel."
   :date "2026-11-13" :origin "fix review of 4fd871850 (Astra W01, Sol W1)")

  (item "fill-line-switch-mid-phase" :group "trace-check" :area "sdk"
   :title "Fill check: a thread's fill line can change inside one fill phase"
   :text "rocketnet_server_fill_line and rocketnet_server_fill_line_of_caller (sdk/rocketnet_server_transport.cpp) pick line 0 while the check is not armed and the caller's hashed line once it is, reading the armed latch live on each call. The latch arms on the game's first call that names a client, so within that one fill phase a thread can stage sends on line 0 and then on its own line: one client's sends of one frame sit on two lines and can leave out of order. Narrow: frame 1 only (the latch never disarms), and it needs a stale armed read racing the arm plus sends for one client from both sides of it; the receiver tolerates the reorder. RNFillPhase has no SlotOrder invariant, so the model cannot see it. Repair: the main thread copies the armed latch (the line choice) into a word of its own at each fill phase's start, and both functions read that copy."
   :date "2026-11-13" :origin "split-update pass 2, finding S173 (no code change now)")
  (item "uring-peak-qdisc-drops" :group "after-v1-1-landing" :area "rocketnet"
   :title "io_uring at peak: whole segmented sends dropped at the server's device queue"
   :text "On the X710 pair with 2 io_uring threads, peak load and no game load, the server's fq_codel queue drops whole UDP_SEGMENT sends: each is one player's 15-packet tick, so 2 to 13 drops in a 60 second row lose 30 to 195 packets (s2c 99.9997 to 100%; players whole, rows pass). The kernel accepted the sendmsg, so the transport counts the packets sent and nothing names the loss. The same rows also see 8.6 to 10.7 million EAGAIN completions a peak row against the documented 1,000,000-byte send buffer, where gso on the same pair and buffer sees 124k to 571k; a 4096-entry ring of sends in flight per queue is the likely reason, not measured. gso and XDP on the same pair: sent equals received. Tuning is deferred."
   :date "2026-11-13" :origin "io_uring re-measure on main 3afea2aed")

  (item "reconcile-segmented-drops" :group "after-v1-1-landing" :area "tests/load"
   :title "Reconcile: a dropped segmented send is 15 packets, not 1"
   :text "The harden_load reconcile names the server's qdisc drops by the qdisc's count, which counts a dropped GSO skb once. At peak each skb carries 15 packets, so every such drop leaves 14 packets in the unnamed gap and the s2c chain fails on a loss it could name. Count the segments of a dropped skb (or read the qdisc's byte or packet drop totals), so the chain names the whole loss. Separate from reconcile-sndbuf-resends."
   :date "2026-11-13" :origin "io_uring re-measure on main 3afea2aed: lost = 15 x qdisc dropped in every run")

  (item "uring-retry-probe-first" :group "after-v1-1-landing" :area "sdk"
   :title "io_uring full-socket retry: probe with one run before sending the rest"
   :text "The io_uring worker waits on a 1 ms timer after a full answer (the gso worker's wait; never POLLOUT, which would spin on -ENOBUFS). When the timer fires it sends every held run again, and on a socket that is still full most come back refused. Measured on a Linux bench at 1 Gbit tbf, 4096 packets a frame: URING_SEND_DEFERRED 191.9k/s and 0.29 worker cores, against 41.6k/s and 0.16 cores with the old POLLOUT wait, for the same packets sent (86.0k/s against 82.0k/s). Send one run first after the timer, and the rest only once it is sent, so a socket still full refuses one run a wait, not all of them. Model it in RNUringQueueWorker.tla with the change."
   :date "2026-11-13" :origin "io_uring review fixes, the after-fix repro")

  (item "uring-prefilter-wrong-value-once" :group "after-v1-1-landing" :area "sdk"
   :title "PREFILTER_WRONG_VALUE seen once on io_uring under heavy bench load"
   :text "A Debug functional row of the io_uring work reported PREFILTER_WRONG_VALUE 1274 once, under heavy load on a Linux bench; 12 reruns did not reproduce it and the cause was not found. A DEV-only sampler now records the first refused datagrams (source, length, first 16 bytes) in sdk/rocketnet_receive_prefilter.h and prints them in the clean-run report. When it recurs, read the samples; the open question is whether a datagram can reach the io_uring prefilter with wrong bytes or length (provided-buffer index, completion length, GRO split, a buffer returned while still referenced)."
   :date "2026-11-13" :origin "Review of the io_uring transport work")

  (item "server-update-task-callbacks" :group "after-v1-1-landing" :area "sdk"
   :title "Distribute the server update with task callbacks in the config, only on measured need"
   :text "The split server update (update_receive, update_client, update_finish) landed as an opt-in; rocketnet runs no job system of its own. If a measured row shows the game's own job system cannot spread the update well enough through the public calls, add box3d-style task callbacks to the server config, off by default (see how box3d and fixed3d do it), and not before."
   :date "2026-11-13" :origin "Decided 2026-11-13, with the split server update")

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
   :date "2026-11-13" :origin "#1074 review")

  (item "xdp-gateway-refresh-main-thread" :group "review-v1-2" :area "rocketnet"
   :title "XDP: the gateway refresh blocks the main thread every 5 seconds"
   :text "On XDP, rocketnet_server_socket_receive_packets (sdk/rocketnet_server_socket_linux.cpp about 4745) runs the gateway refresh on the main thread every 5 s (rocketnet_xdp_gateway_refresh, sdk/rocketnet_xdp_frame.h about 1473, through rocketnet_server_socket_resolve_gateway about 648). rocketnet_gateway_netlink_refresh opens a NETLINK_ROUTE socket, dumps the whole route table for the family (RTM_GETROUTE with NLM_F_DUMP), then queries the neighbour (RTM_GETNEIGH); with both families, twice. The refresh's sockets are non-blocking, but that covers only the receive: the kernel handles each request inside sendto, building the first part of the dump there, and a kernel that takes the rtnl lock for these dumps makes sendto wait while another task holds it (route or link changes, other netlink dumps). The cost grows with the host's route count and can hold up the main thread under rtnl contention; it does not show on a bench with a small table. Resolve it off the main thread (on a network thread), or query the one destination without a dump. Rated medium by the wave 2 reader (W2-35)."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "iocp-stop-repost-hang" :group "review-v1-2" :area "rocketnet"
   :title "IOCP stop can hang on a receive reposted after its owner's worker exited"
   :text "Any IOCP worker reposts a refused receive on the owning endpoint's socket (sdk/rocketnet_server_iocp_windows.cpp about 1684-1699), reading stopping (1184) before it raises pending and posts (1235-1237); each stopping worker cancels only its own endpoint's receives and exits when the global pending count reads 0 (1501-1516). A repost that lands after its owner's worker has exited is never cancelled: rocketnet_server_iocp_stopped never turns true, destroy refuses (3722-3726) and create's failure path waits with no bound (3027-3035). Very rare. The model hides it: RNIocpEndpoints.tla Retry (89-96) checks ~stopping and posts in one atomic step. Fix: raise pending before the stopping check, or have every stopping worker cancel every endpoint's receives; split Retry in the model with a reversed witness."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "send-capacity-overload-walk" :group "review-v1-2" :area "sdk"
   :title "Send capacity is reported for frames the caller cannot take, so an overloaded frame walks every client"
   :text "send_capacity_available counts own frames and every queue's budget (sdk/rocketnet_server_transport.cpp about 1510-1528), more than the next start can take, so under overload the serial walk visits every client and is refused for each (sdk/rocketnet_message_server.cpp about 2073-2080, 2195, 2225): main thread work grows with the client count on exactly the frames that are already over. Report what the caller can take."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "send-capacity-one-full-queue" :group "review-v1-2" :area "sdk"
   :title "One full send queue makes the send capacity 0 for every client"
   :text "Capacity is the fewest free slots of any queue (sdk/rocketnet_server_transport.cpp about 1549, through io_send_room 984-1000), so one full queue stops sends to every client, including clients on queues with room. io_send_room_total (1101-1119) already does this right for disconnect drains. Use the client's own queue's room (or the total) for its start. See also send-reserve-multi-queue-capacity."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "ring-take-order-across-guard" :group "review-v1-2" :area "sdk"
   :title "A take across the fill guard can make a thread's ring slots fall, reordering one client's packets within a flush"
   :text "rocketnet_server_payload_ring_take (sdk/rocketnet_server_transport.cpp about 4913-4987), two game threads on one fill line, send_capacity C at least 512: (1) thread A sees the line empty (word 0), passes the guard and claims a chunk at base 0 (num_packets 0 to 8), and is preempted before its install compare-and-swap; (2) thread B sees the line still empty, the guard now fails (C - 8 < 8 * 64), and B takes single slots 8 and 9 by fetch-add, never touching the line word; (3) A's install compare-and-swap from 0 still succeeds, so the line holds [1, 8); (4) B's next take comes from the line: slot 1. B took 8, 9, 1, and the flush walks the ring in slot order, so one client's packets leave reordered within one flush; nothing is lost or duplicated. Rare: needs a preemption between a claim and its install while the counter crosses C - 512. Fix: a thread that fails the guard first swaps its line from empty to a closed word, so a late install fails and gives its chunk up; add SlotsRise to RNServerPayloadRingLayout with the guard path as its reversed witness. Not taken in a point release: the given-up chunk wastes up to 7 claimed slots near capacity, so sends that fit today would be refused."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "send-pass-hold-alternating-room" :group "review-v1-2" :area "sdk"
   :title "With several send queues and a live room, the send pass can skip one held connection every update"
   :text "rn_message_object_send_packets (sdk/rocketnet_message_server.cpp, the per-queue hold): a connection held for its full queue before any frame was written does not hold the cursor. Four connections, queues A (0, 2) and B (1, 3), each with room for one frame: update 1, both rooms free: 0 and 1 written, 2 finds no room anywhere, cut, cursor 2; update 2, A spent: 2 held with nothing written yet (cursor not held), 3 written on B, 0 cut, cursor 0; update 3 repeats update 1. If A's room alternates spent and free in step with the rotation, connection 2 is never written: reliable sends stay due (latency, and if it persisted a timeout for that client). Very unlikely (needs the alternation, busy clients ahead on A, room on B). TLC: proofs/tla/MCRNServerSendFairQueuesLive.cfg (models/v1.1.x/send-build). Fix: remember the last update's first held connection, and let a connection held before any frame was written hold the cursor when it was not also the first held at the previous update, so a queue full for many updates still does not pin the rotation. Not taken in a point release: a design change to the rotation."
   :date "2026-11-13" :origin "Review before v1.2.0, models package send-build, MM057")

  (item "rtt-receipt-only-across-outage" :group "review-v1-2" :area "standard"
   :title "Receipt-only sends are sampled for RTT across an outage, so an outage with only receipt traffic in flight raises the resend delay"
   :text "standard/MESSAGES.md m9.1 samples a send that carried receipt serials and no reliable number, and m9.2 ends an acknowledgement silence only when the lowest unacknowledged reliable number was owed; the code does both (sdk/rocketnet_window_ack.cpp about 968, 977, 1107). So a receipt-only send made before an outage of a second or more, with no reliable number in flight, and acknowledged after it is sampled: each such ack moves srtt 5% of the way toward that send's send-to-ack time and the deviation 5% toward the gap (with only the ack direction down, delayed acks of receipt-only sends that the receiver's ack history still covers and the sender's sent map still holds can each give a sample, so there can be many; a send whose entry the sent map evicted (sdk/rocketnet_window_ack.cpp about 784-803) gives none, since its late ack is not found (about 891-895), while a receipt that only passed its deadline keeps its entry and can still be sampled). Example: srtt 50 ms, deviation 10 ms, one receipt-only send acked 1500 ms after it was sent (sent as the outage began): srtt 122.5 ms, deviation 82 ms, resend delay 170 ms to 603 ms (later reliable resends wait 433 ms more, 26 updates at 60 Hz); for equal samples, k such acks move srtt 1 - 0.95^k of the way toward that sample, and srtt and the deviation decay back as normal samples arrive; neither is clamped, but the resend delay derived from them stays clamped at message_resend_delay_max_ms (2000 ms default). A remote endpoint that withholds and then releases acks for receipt-only packets can drive it on purpose, within that clamp on the resend delay. Candidate rules: (A) take no sample from a send that carried no reliable number; or (B) run the silence test for a receipt-only send from its own send time, so an ack that waited out a silence is not sampled whatever the send carried. Either is a change to the standard (m9.1, m9.2) as well as the code, and changes proofs/tla/RNRttFilter.tla in the same commit: on main it models no receipt-only packet; the models package branch models/v1.1.x/receipts-rtt-resend-window adds them (SendReceipt, ReceiptPkts) with MCRNRttFilterReceiptOnlyPoisons.cfg, which becomes the reversed witness."
   :date "2026-11-13" :origin "Review before v1.2.0, models package receipts-rtt-resend-window, MM022")

  (item "xdp-send-trace-layer" :group "review-v1-2" :area "proofs"
   :title "The XDP send side has models and no trace layer; the receive-ring walk never runs the real worker loop"
   :text "No trace layer binds the XDP send half to its models: the buffer exchange publish, claim and recycle (sdk/rocketnet_server_socket_linux.cpp about 158-210, RNBankFifoLayout.tla), the producer publish and turn bump in rocketnet_server_socket_send_packets (about 3268-3316, RNDriverSendStaging.tla Publish), the worker's claim, copy, recycle and send-frame free list (about 5008-5156, WorkerRetire), and the stop drain (rocketnet_server_socket_send_idle about 3051, rocketnet_xdp_send_stall_pass in sdk/rocketnet_server_socket.h about 108, RNXdpSendDrain.tla). Only unit cases cover them (tests/test_unit_xdp_send_staging.cpp, tests/test_xdp.cpp). The xdp-receive-ring trace (tests/tla_trace_xdp_receive_ring.cpp about 287-460) calls the pass helpers in an order the walk composes and never runs rocketnet_server_socket_xdp_thread_function (about 4825). Add send layers, and drive the receive walk through the real loop."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "xdp-vlan-stripped-tags" :group "review-v1-2" :area "rocketnet"
   :title "An XDP server on a VLAN reads the tag stripped by receive VLAN offload"
   :text "sdk/xdp/server_xdp.c (about 483-516) and sdk/rocketnet_xdp_frame.h (about 383-405) take an untagged frame on a tagged server. With receive VLAN offload on (the i40e and ixgbe default) native XDP sees every stripped frame untagged, so a frame of any VLAN the NIC accepts, or of the native segment, addressed to the server's address and port would be processed and answered tagged with the server's VLAN. The point release (fix/v1.1.x/xdp-vlan-offload) closes this by refusal: scripts/rocketnet-provision turns the parent's rx-vlan-offload off and reads it back, and the pre-flight refuses a VLAN server whose parent has it on (rocketnet_server_socket_preflight_vlan_tag). Left here: read the stripped tag from the receive metadata (bpf_xdp_metadata_rx_vlan_tag, Linux 6.8 and later), so the offload can stay on and a veth parent is covered too (a veth keeps a tag its sender put beside the frame beside it, whatever its own flag, so the program sees that frame untagged); decide whether a VLAN server should take untagged frames at all once the tag is always readable (it still takes the untagged segment's frames for its address and port); cover a NIC that strips 802.1ad outer tags (rx-vlan-stag-hw-parse), which ETH_FLAG_RXVLAN does not show; and the mirror case, an untagged server on a trunk NIC with the offload on, which takes stripped frames of any VLAN for its address and port and is not refused."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "xdp-martian-sources" :group "review-v1-2" :area "rocketnet"
   :title "XDP accepts sources the kernel stack drops as martian, and does not verify checksums on receive"
   :text "sdk/xdp/server_xdp.c has no source check, and userspace refuses only IPv4-mapped IPv6 (sdk/rocketnet_xdp_frame.h about 675, sdk/rocketnet_address.h about 65): 0.0.0.0/8, 127/8, 224/4, 255.255.255.255, the server's own address, ::, ::1, ff00::/8 and source port 0 reach the main thread and get replies. IPv4 header and UDP checksums are not verified on receive (rocketnet_xdp_frame_parse_vlan about 420-537). (The version nibble half is closed: the kernel program passes an IPv4 or IPv6 header whose version is not the ethertype's own, as the reader refuses it.) The socket transports get all of this from the kernel. Refuse these in the kernel program and check the sums."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "xdp-receive-stopped-uncounted" :group "review-v1-2" :area "rocketnet"
   :title "XDP: datagrams received and not handed to the main thread at stop are counted nowhere"
   :text "The XDP operations table passes NULL for receive_drop_totals (sdk/rocketnet_driver_builtin.cpp about 2523), the only feed of RECEIVE_DROPPED_STOPPED, so at stop (sdk/rocketnet_server_socket_linux.cpp about 4887-5285) the staged, ready, unread-tail (about 4761) and receive-ring datagrams are dropped uncounted. Every other rebuilt transport counts them. Feed the row, with a test that reads it by value. Separate from xdp-stop-abandoned-buffers, which is the send side."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "rio-lane-trace-harness-state" :group "review-v1-2" :area "proofs"
   :title "The rio-lane trace checks the harness's own slot table, not the transport's"
   :text "tests/tla_trace_rio_lane.cpp calls the real hash, lane_of, packet_id and decode (about 126-140) but keeps every state field itself (slot, issued, staged, starts, strayed, misrouted; about 56-68, 129-211): it picks the slot with its own loop and decides misrouted itself. The transport's slot take and frame check, start_packet, finish_packet, abort_packet and lane staging in sdk/rocketnet_server_rio_windows.cpp (about 485-514, 1713-1744, 4162-4198) never run under the trace, so NoReissue, FinishOwn and OneLanePerDest cannot go red for a library defect. The hash is called with family 1 where the transport passes to->type (134, 240), and the comment at line 11 overstates what is bound. Drive lane start and finish on the no-socket fixture and read the library's state, or say plainly what the layer binds."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "iocp-admit-trace-verdict-only" :group "review-v1-2" :area "proofs"
   :title "The iocp-admit trace binds only the admit verdict"
   :text "tests/tla_trace_iocp.cpp (about 169-194) calls rocketnet_server_iocp_test_receive_admit for the verdict, then pushes to its own bank and increments its own full, refused and counted. rocketnet_iocp_bank_datagram (sdk/rocketnet_server_iocp_windows.cpp about 1309-1325) and the drop counters never run, so EveryDropCounted and Accounted are checked against the harness's arithmetic: a BANK_FULL datagram dropped uncounted stays green. Drive the real bank and read the library's counters."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "iocp-send-lanes-trace-real-path" :group "review-v1-2" :area "proofs"
   :title "The iocp send-lanes trace keeps its own banks and posts"
   :text "tests/tla_trace_iocp_lanes.cpp (its header, about 33-42) and proofs/tla/README-RNIocpSendLanes.md (about 90-97): the walk reads the code's free queues, destination hash, carve release lane and send-thread lane walk, but the banks, the in-flight set, the started and posted orders, zeroed and postedBy are the walk's own; no start_packet, finish_packet, abort_packet, rocketnet_iocp_send_bank, gather or post runs, so InOrder and Conserved are checked against state the walk wrote. Drive the real staging, bank and post path (sdk/rocketnet_server_iocp_windows.cpp) in the trace walk and read the transport's state."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "rio-worker-trace-binding" :group "review-v1-2" :area "proofs"
   :title "RNRioWorker has no trace walk binding it to the code"
   :text "proofs/tla/README-RNRioWorker.md (about 559-563): the unit seams drive the real rio worker pass, main calls and stop pass (tests/test_iocp.cpp, test_rio_transport_stop_drains_sends among them), but no tests/tla_trace_*.cpp walk emits their steps for TLC to check against RNRioWorker.tla, so the model's checks are by hand and by those unit cases. Add the trace layer (walk, cfg, COVERAGE.tsv and RUNS.tsv rows) over the real worker pass."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "xdp-gateway-refresh-trace-order" :group "review-v1-2" :area "proofs"
   :title "The xdp-gateway-refresh trace cannot see the refresh moved"
   :text "tests/tla_trace_xdp_gateway_refresh.cpp (about 192, 240-253, 326) decides when the refresh runs with the walk's own phase string; the real call site (sdk/rocketnet_server_socket_linux.cpp about 673) never runs, and the untraced ReceiveWithoutRefresh is justified by assumption (tools/tla_trace_check.py about 157). The deadline part is bound; the ordering part (the model's RefreshInFill witness) is not, so a refresh moved into the send path stays green. Drive the walk through the real call site."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "client-acked-batch-full-test" :group "review-v1-2" :area "tests"
   :title "A test that fills the client's delivery batch and reads CLIENT_ACKED_BATCH_FULL by value"
   :text "Every test reads ROCKETNET_COUNTER_CLIENT_ACKED_BATCH_FULL only as == 0 (tests/test_unit_sequence_burn.cpp, sdk/rocketnet_tests.cpp, tests/functional/server_receive_low_tick.cpp). check_counter_observed.py counted it observed through a name a check read only as == 0; with that path closed it is listed in OWED. The server's twin, ACKED_BATCH_FULL, has a unit case (tests/test_unit_client_receive_counters.cpp, the batch that holds two). The client's batch is fixed at ROCKETNET_SENT_PACKET_BUFFER_SIZE (rocketnet_client_protocol.cpp, the ack block's process call), so the case needs a seam that shrinks it, or more acks in one update than the batch holds; then drop the OWED row."
   :date "2026-11-13" :origin "Review before v1.2.0, checks reader, finding FG-G5 (R108)")

  (item "counter-observed-scoped-paths" :group "review-v1-2" :area "tools"
   :title "check_counter_observed holds its indirect paths to the clauses that can show a move"
   :text "tools/check_counter_observed.py now credits a direct check only through a clause that can show the counter moved, but three indirect paths still read looser text: a name assigned from a counter is matched against checks anywhere in the file, not in the assigning function's scope; a table row's field is matched against the unfiltered check text; and a returned or folded predicate is credited without the movement filter. A zero-only check through any of the three still counts as observing the counter. Scope the assignment path to its function and pass the other two through the same clause filter, each with a self-test case."
   :date "2026-11-13" :origin "Review before v1.2.0, gates review of 1b98c9bb6 (Astra P3, R108)")

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

  (item "counter-observed-loose-paths" :group "review-v1-2" :area "tools"
   :title "check_counter_observed.py accepts tests that assert nothing about a counter's value"
   :text "This gate enforces that a counter row lands only with a test that reads it by value, and it accepts empty assertions. tools/check_counter_observed.py (about 426-434) excludes only a clause ending == 0, so check( v[X] >= 0 ), check( v[X] == 0u ), check( !v[X] ) and check( v[X] == v[X] ) count as observed; it accepts T name = v[X]; when any check anywhere in the file reads an identifier of that name (about 465-475); and the return path matches a function of the same name in any file (about 510-513). Run on a scratch tree, it reported 5 of 5 counters observed with five empty assertions; its 26 self-test cases have none of these forms. On the real tree 238 of 301 rows are observed by a value check and 53 more only through the loose paths. Close the three paths, add the forms as self-test cases, and give the 53 rows real value checks."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "msgx-refused-batch-prefix" :group "review-v1-2" :area "sdk"
   :title "msgx may resend a batch whose first packets the kernel already sent"
   :text "sdk/rocketnet_msgx_send_runs.h (about 32-41, 330-374) takes a sendmsg_x that returns -1 as none taken: the run is kept and sent again whole from the same cursor (the worker retries every 1 ms, sdk/rocketnet_server_msgx_apple.cpp about 794-799, 879-903). The repo's own measured note says xnu can deliver part of a batch and still return -1 with no count (sdk/rocketnet_platform.cpp about 505-512, 2692-2701), and the client and portable pump handle that; msgx does not. If an ENOBUFS at peak does this, the delivered prefix goes out two or more times into a full queue: duplicates the client's replay window drops, sent counters under the wire, SEND_FAILED over on the error path. The model (RNMsgxSendRuns.tla) has the same assumption in its fake kernel. Open until measured: one peak row on the 10 GbE pair (received against packets_sent with send_dropped 0) decides; a loopback probe of 2000 batches of 64 never saw a refusal."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "strict-keep-off-cpu-zero" :group "review-v1-2" :area "sdk"
   :title "A strict option: refuse to start when a Linux transport worker cannot be kept off CPU 0"
   :text "When the pin and then the fallback confinement are both refused (XDP leave_core_zero in sdk/rocketnet_server_socket_linux.cpp about 4236-4256, io_uring confine_worker in sdk/rocketnet_server_uring_linux.cpp about 411-431, and gso's fallback), the worker carries on with the inherited mask, CPU 0 included, for good: a cpuset limited to CPU 0 does this. The count and the warning are being fixed for v1.2.0. Left: an option, off by default, that makes the server refuse to start instead when the host has more than one CPU (a one-CPU host must still run)."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "code-4-block-walks-per-fragment" :group "review-v1-2" :area "sdk"
   :title "A code-4 block fragment walks its block's count twice"
   :text "In sdk/rocketnet_channel_sequenced.cpp a fragment that anchors a new in-turn code-4 block runs the stale-sibling scan over the block's count (about 272-288), and a newer one releases the old block by walking its count (rn_sequenced_release_slot, about 123-140), whatever arrived. Measured on a Linux bench (Release, rn_ordered_receive called directly, W = 1024): a first fragment of a 910-fragment block costs 1.71 us against 0.11 us for a block of 2. The cost is bounded by the count, which max_message_bytes caps (at most 911 fragments at 1 MiB), not by W. To repeat it a sender must also send the block's other numbers, because a newer block whose numbers overlap a live code-4 one ends the connection (BAD_BLOCK). That makes about 13 more packets for each walk, so per byte received the cost stays flat. Dense 1-byte last fragments (70 a payload) cost 2.7-4.1 us a payload at either count, because the stale scan stops at the first released sibling. Not a point-release defect. A walk over only the fragments received (the anchor's block_have) would make it proportional."
   :date "2026-11-13" :origin "Ordered duplicate scan fix: the code-4 estimate, measured")

  (item "receive-reservation-trace-binding" :group "review-v1-2" :area "proofs"
   :title "The receive reservation's models are bound by citation only"
   :text "RNOrderingWrap (the delivery reservation, the duplicate's reservation and the deferral in rn_ordered_receive), RNWindow and RNWindowLayout (rn_window_receive_accept) and RNHeldOwnership have no tests/tla_trace_*.cpp walk. rn_window_receive_duplicate is in no model's actions: it repeats Accept's duplicate test, and its agreement with rn_window_receive_accept was first shown by reading the two functions and is now checked by a finite test (named below), but by no trace: tools/tla_trace_check.py does not check the code's receive path against them, so an action reordered in the code stays green against the model. Give the channel receive path a trace walk that emits Receive, ReceiveDuplicate and ReceivePartial with the read index, the held set and the deliveries, checked against RNOrderingWrap. The two duplicate tests can drift apart: any check of their agreement must cover a negative delta, the half-range boundary (deltas 2^23 - 1 and 2^23), the number's 2^24 wrap against R, R at 0 and near UINT64_MAX, deltas at and past W, a set and a clear received bit, a live descriptor under another number, and a latched violation. test_window_receive_duplicate_agrees_with_accept (tests/test_unit_window_sequences.cpp) sweeps these at W = 64 now; a trace binding would keep them bound as the code changes."
   :date "2026-11-13" :origin "Audit round 1, ordered duplicate scan fix")

  (item "load-delivery-delay-column" :group "after-v1-1-landing" :area "tools"
   :title "A delivery-delay column in the load report, per transport"
   :text "The receive handoffs of gso, xdp and portable held nearly every datagram one more server update than io_uring (measured 2026-11-13 at a steady mean of 1.49 updates against 0.52), and no load run showed it: bin/load reports update times, drops and counters, never how many server updates a message waited. Add a column to the load report: for each message, the updates between its send and the receive_messages call that returned it, as mean, p99 and max per transport, so a handoff that adds an update shows in every load run as well as in tests/functional/receive_latency.cpp."
   :date "2026-11-13" :origin "Receive handoff latency fix, the owner's ask for input checks that do not regress")

  (item "receive-handoff-advance-after-tail" :group "after-v1-1-landing" :area "sdk"
   :title "After a cut receive call, the next call does not reach the worker's newer buffer"
   :text "In sdk/rocketnet_receive_handoff.h main moves on to the worker's buffer (rocketnet_receive_handoff_begin) only at the start of a call whose tail is finished. After a call the per-update budget cut, the next call reads the tail and main's own buffer and leaves what the worker committed into its newer buffer for the call after, even with budget left: one more update for those datagrams, only after an overloaded update. A second try-acquire once the tail is drained within the call would close it; it needs RNReceiveHandoff to drop the backlog exemption from DeliveredByNextCall, and its trace walk to cover the second try."
   :date "2026-11-13" :origin "Receive handoff latency fix, cold read finding 4")

  (item "client-io-decrypt-receive-half" :group "after-v1-1-landing" :area "proofs"
   :title "Restate RNClientIoDecrypt's receive half over the three-buffer receive handoff"
   :text "The client I/O thread's receive moved to sdk/rocketnet_receive_handoff.h (RNReceiveHandoff, trace-checked by tests/tla_trace_client_receive_handoff.cpp). proofs/tla/RNClientIoDecrypt.tla still restates the two-buffer Fill/Ready/Exposed/Free handshake of RNClientIo: its IoPublish stands for the commit and its Expose for the start of update's call, while IoOwe and Return have no step in the code now. Its decrypt properties (CheapRefusalsFirst, NoOpenWithoutKeys, IoWindowWithinMain, PayloadOpenedOnlyConnected, AcceptedOnce, SingleWriter) do not depend on that handshake. Restate the receive half as Bank, Commit and Call over three buffers with a committed count (update reads records below the count while the I/O thread appends past it), map ExposedStable to records below the committed count never changing and NoReceiveStall to DeliveredByNextCall, and re-measure the reduced and full configs and the six reversed witnesses."
   :date "2026-11-13" :origin "Client receive handoff latency fix, the decrypt model's stale receive half")

  (item "gso-socket-drops-unit" :group "after-v1-1-landing" :area "sdk"
   :title "Measure what the kernel's SO_RXQ_OVFL count counts on a GRO socket"
   :text "GSO_RECEIVE_DROPPED_OVERFLOW adds the rise in the kernel's SO_RXQ_OVFL count (sk_drops) to the datagrams the gso worker lost for want of room, and the row's unit is datagrams. Whether the kernel adds one per dropped buffer or one per segment of a dropped GRO run differs by drop site and kernel version; if per buffer, one dropped run of 128 segments reads as 1 and the row undercounts. A kernel drop is also reported only on the next datagram the socket delivers, so drops with no datagram after them are never counted, even at stop. Measure on the fleet's kernels (a flood of UDP_SEGMENT runs at a socket with UDP_GRO and a small receive buffer, against the socket's SK_MEMINFO_DROPS and UDP RcvbufErrors) and make the row's text say what it counts."
   :date "2026-11-13" :origin "cold read of the v1.1.54 gso CI repair")

  (item "gso-receive-dropped-socket-row" :group "after-v1-1-landing" :area "sdk"
   :title "A gso row of its own for the kernel's drops at a queue socket"
   :text "Since 2026-11-13 the gso drain counts the kernel's drops at a full queue socket (SO_RXQ_OVFL) in GSO_RECEIVE_DROPPED_OVERFLOW, so that row no longer aliases RECEIVE_DROPPED_FULL exactly (standard/TRANSPORT.md says so). io_uring keeps the same drops in its own URING_RECEIVE_DROPPED_SOCKET. A GSO_RECEIVE_DROPPED_SOCKET row would restore the alias and match io_uring; counter rows are ABI, so it waits for a release that may append rows."
   :date "2026-11-13" :origin "gso flood drops counted by no counter (W2-121); the existing row was chosen for the point release")

  (item "xdp-receive-dropped-stopped" :group "after-v1-1-landing" :area "sdk"
   :title "The xdp server counts no RECEIVE_DROPPED_STOPPED"
   :text "The xdp operations table has no receive_drop_totals (sdk/rocketnet_driver_builtin.cpp, the NULL after xdp_begin_shutdown), so datagrams the xdp worker staged that main never took when the server stops are counted by no row, where gso and io_uring count them in RECEIVE_DROPPED_STOPPED. Since 2026-11-13 the xdp server uses the receive handoff (sdk/rocketnet_receive_handoff.h), so the count is the gso one: the tail's unread rest, main's buffer past its cursor, committed or not, and, when the worker moved on, the worker's buffer (rocketnet_server_gso_count_stopped), once the worker has exited. An expected-class counter, so it waits past the critical-and-high fix round."
   :date "2026-11-13" :origin "2026-11-13 review round; low severity, moved off the fix round")

  (item "xdp-receive-lock-busy-row" :group "after-v1-1-landing" :area "sdk"
   :title "A counter for xdp receive calls that found the receive lock held"
   :text "An xdp receive call whose few tries of a queue's receive mutex all fail reads only what it already holds and catches up at a later call; nothing counts it. Before 2026-11-13 a comment said such a put-off drain was counted; the comment now says nothing counts it. A count needs a counter row, and counter rows are ABI, so it waits for a release that may append rows."
   :date "2026-11-13" :origin "2026-11-13 review round; needs a new counter row")

  (item "portable-server-receive-latency" :group "after-v1-1-landing" :area "sdk"
   :title "The portable server's receive handoff holds datagrams an extra update"
   :text "Measured 2026-11-13 on amd-2 over loopback at 60 Hz: the portable server returns a datagram a mean 1.481 updates (24.7 ms) after its send, against 0.51 to 0.52 updates (8.5 to 8.7 ms) for io_uring, and for gso and xdp once their handoffs were fixed: the worker publishes a filled bank and main takes it only at a later call (portable_rx_publish and the rx_bank FIFO in sdk/rocketnet_driver_builtin.cpp). The shared receive handoff (sdk/rocketnet_receive_handoff.h) and the delivery check (sdk/rocketnet_receive_delivery_check.h) would fix and test it. The portable server transport is not a production transport (owner, 2026-11-13: no time is spent on it in v1.1.x)."
   :date "2026-11-13" :origin "Receive handoff latency review, 2026-11-13; moved off the fix round by the owner")

  (item "portable-server-receive-prefilter" :group "after-v1-1-landing" :area "sdk"
   :title "The portable server's worker does not run the shared receive prefilter"
   :text "Reported in the 2026-11-13 review round: the gso and io_uring workers screen each datagram with the server's stateless prefilter (sdk/rocketnet_receive_prefilter.h) before it takes a bank slot, and the portable worker does not, so garbage can take its slots. Not reproduced on this branch. The portable server transport is not a production transport (owner, 2026-11-13: no time is spent on it in v1.1.x)."
   :date "2026-11-13" :origin "2026-11-13 review round; moved off the fix round by the owner")

  (item "portable-server-receive-stop-count" :group "after-v1-1-landing" :area "sdk"
   :title "The portable server counts no RECEIVE_DROPPED_STOPPED"
   :text "Reported in the 2026-11-13 review round: datagrams the portable worker banked that main never took when the server stops are counted by no row, where gso counts them in RECEIVE_DROPPED_STOPPED; its operations table has no receive_drop_totals. Not reproduced on this branch. The portable server transport is not a production transport (owner, 2026-11-13: no time is spent on it in v1.1.x)."
   :date "2026-11-13" :origin "2026-11-13 review round; moved off the fix round by the owner")

  (item "portable-server-recvmmsg-error-spin" :group "after-v1-1-landing" :area "sdk"
   :title "The portable server's receive retries a failing recvmmsg at once"
   :text "Reported in the 2026-11-13 review round as an error spin: the platform receive loop (sdk/rocketnet_platform.cpp, the recvmmsg call and its receive-weather retry) retries an error it reads as weather in the same pass, with no back-off. Not reproduced on this branch. The portable server transport is not a production transport (owner, 2026-11-13: no time is spent on it in v1.1.x)."
   :date "2026-11-13" :origin "2026-11-13 review round; moved off the fix round by the owner")

  (item "segment-refusal-needs-plain-success" :group "after-v1-1-landing" :area "sdk"
   :title "Count a segmentation refusal only when the run's plain resend got through"
   :text "The gso flush (sdk/rocketnet_server_gso_linux.cpp, rocketnet_server_gso_flush) and the io_uring queue worker count EINVAL, EIO or EOPNOTSUPP on a segmented send as that destination refusing segmentation, though a send the destination would refuse plain fails the same way. Before any segmented send was accepted, ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD such destinations turn segmentation off for the transport. The one remote trigger, a source port of 0 (the kernel refuses destination port 0 with EINVAL, shown on 7.0.0-43), is refused at receive since 2026-11-13 (accept_received_source). The general repair resends plain first and counts the refusal only when a plain packet went; it reorders the refused path, so RNSegmentRefusal and RNGsoHandoff gain a destination that refuses plain sends too, with a reversed witness."
   :date "2026-11-13" :origin "Round-1 audit R56 (FL-L9); a low item larger than a point release carries")

  (item "gso-retry-after-quit-by-time" :group "after-v1-1-landing" :area "sdk"
   :title "Bound the gso and io_uring send retry after quit by time, not attempts"
   :text "Once shutdown begins, a socket that stays full is tried ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT (100) more times before its tail is dropped (sdk/rocketnet_server_gso_linux.cpp, rocketnet_server_gso_send_step; the io_uring worker shares the bound). On gso the attempts are paced by the 1 ms retry timeout and by main's wakes, once per update, so at 60 Hz the bound is about 100 ms or more, as documented; only a main updating faster than 1 kHz while stopping shortens it. The io_uring worker shares the attempt count but not the wakes: from shutdown on only its retry timer ends a wait (sdk/rocketnet_uring_queue_worker.cpp, the wake read's completion leaves blocked set), so each counted wait is a whole 1 ms period and its window is already about 100 ms at any update rate. Not reproduced as a fault; a time bound would make the gso window independent of the update rate."
   :date "2026-11-13" :origin "Round-1 audit R56 (FL-L10); not reproduced, closed with this entry")

  (item "client-fill-sequence-order" :group "review-v1-2" :area "sdk"
   :title "Two client fill threads can stage their sequences out of slot order"
   :text "rocketnet_client_transport_start_packet (sdk/rocketnet_client_transport.cpp) claims a payload ring slot with one atomic add and takes the session sequence with another (rocketnet_client_protocol_start_payload, sdk/rocketnet_client_protocol.cpp). Two game threads starting at once can take slots i and i+1 and then the sequences in the other order, so the ring holds [seq+1, seq] and the I/O thread sends them in that order. The server sees what a one-packet network reorder gives it: the replay window takes both, and acks name sequences, not slots. Not changed for the point release: binding slot and sequence would need one atomic for both (a sequence derived from the slot breaks across rings, burns and the stop's final ring) or a lock on the fill path, which adds contention to every start. Decide whether wire order equal to sequence order is a property the client should keep, and measure the cost of keeping it, before any change."
   :date "2026-11-13" :origin "Review before v1.2.0, finding R87 second part (Carbon C-14, Emma E-21), fix/v1.1.x/client-stop-and-race")

  (item "client-race-full-buffer-wait" :group "review-v1-2" :area "sdk"
   :title "During the family race, a full receive buffer waits the whole I/O guard on the primary family's wake"
   :text "While the race is uncommitted and the I/O thread's receive buffer is full, rocketnet_client_race_wait (sdk/rocketnet_client_transport.cpp) waits on family[0]'s wake alone for the full guard (FullWaitsOnWake; tests/test_unit_client_bind_family.cpp test_client_race_full_buffer_waits_on_the_primary_wake_alone). The race's wake sets both families' wakes while uncommitted, so main's return of a buffer still ends that wait; a stall needs the buffer full during the handshake. Measured on main 5a2dbf403 on bench-xdp-9 with Carbon's REPRO 4 (a 400-datagram flood during the race), 9 runs: the Connection Request arrived in 0.52-0.87 ms for both the IPv6 control and the IPv4 commit, no stall; that flood does not fill the 1024-slot buffer. After the commit the wake never touches the losing family. If a stall of the order of the guard is ever measured, the fix is to cap the full-buffer wait at the race's 1 ms poll while uncommitted, with an RNClientRace step and a reversed witness."
   :date "2026-11-13" :origin "Review before v1.2.0, finding R89, fix/v1.1.x/client-stop-and-race")

  (item "client-race-loser-queue-at-commit" :group "review-v1-2" :area "sdk"
   :title "The losing family's queued datagrams are closed away uncounted at the race's commit"
   :text "When the reading thread first sees the commit, rocketnet_client_race_kept (sdk/rocketnet_client_transport.cpp) closes the losing family's socket; datagrams still queued on it go with the close, unread and uncounted. standard/TRANSPORT.md t13.3 (StoppedSocketDrained) covers a client whose connection has ended, and the handshake's race rule counts a packet read from the other family after the commit as from a foreign source and requires the other socket closed; neither says what happens to the losing socket's queue at the commit. Decide in the standard whether the commit drains and counts that queue (for example as CLIENT_RECEIVE_FOREIGN_SOURCE) before the close; then model it in RNClientRace and test it with the race harness."
   :date "2026-11-13" :origin "Review before v1.2.0, finding R88 second part, fix/v1.1.x/client-stop-and-race")

  (item "server-create-refuses-what-start-refuses" :group "review-v1-2" :area "sdk"
   :title "Refuse at create the bind addresses and per-transport counts that start refuses"
   :text "rocketnet_server_create accepts a bind_ipv4_address or bind_ipv6_address that does not parse, and a transport count its transport cannot run (num_iocp_send_threads above the send lanes max_clients gives, for one); rocketnet.h says start refuses them, and it does. rocketnet_server_required_memory answers 0 for a config whose transports none can plan (the v1.2.0 fix), but create still returns OK. Moving these refusals to create (rn_server_config_valid in sdk/rocketnet_api.cpp) changes the header's stated contract, and a Windows-only count would then refuse a shared config on every platform: the owner decides whether and how (refuse only the candidates the list names, or only on the platform that builds them)."
   :date "2026-11-13" :origin "Review before v1.2.0, Carbon finding C-18 (R79) and #1115-2(a)")

  (item "server-open-message-outlives-disconnect" :group "review-v1-2" :area "sdk"
   :title "Keep an open server message's memory the caller's until the next update, across a disconnect"
   :text "rocketnet_server_disconnect_client, rocketnet_server_disconnect_all_clients and rocketnet_server_stop abort the client's open message at once (rn_message_object_kick, sdk/rocketnet_message_server.cpp): an in-place frame goes back to the transport and the reservation is released, so a caller still writing into the pointer writes into memory that is no longer its own. rocketnet.h now says so (v1.2.0). Holding the frame or the reservation until the next update or send_packets would keep the old promise; it needs lifetime state the kick does not have today (a deferred abort list per object, and the stop's lease reset waiting for it)."
   :date "2026-11-13" :origin "Review before v1.2.0, Fable reader, finding FT4 (R11)")

  (item "disciplined-token-clock" :group "decisions" :area "sdk"
   :title "A server token clock that follows the wall clock within a bound (option A of R40)"
   :text "fix/v1.1.x/token-clock moved rocketnet_time() to CLOCK_BOOTTIME on Linux and CLOCK_MONOTONIC on macOS. With token_epoch_unix_seconds 0, a server passing that clock now stays on the rate the host's time sync sets and counts suspend. Three gaps remain, each documented as the operator's: a wall-clock step after the first update (a server started before time sync set the clock); Windows QueryPerformanceCounter rate drift (not synchronized; 30 to 50 ppm typical, 50 ppm uses up the 30-second margin in about a week); and a game that passes its own clock. Option A closes all three for an epoch-0 server but changes the contract in rocketnet.h (ROCKETNET_SERVER_TOKEN_EPOCH_UNIX_SECONDS: the checks never read the wall clock, and pausing the caller's clock pauses token expiry), docs/SECURITY.md, and standard/TRANSPORT.md t2 rule 6 (no protocol decision from a clock the endpoint reads itself), so it is the owner's call. Design: token clock = epoch + caller time + offset, offset a double starting at 0, epoch-0 servers only (a configured epoch is the caller's and is untouched). Read the wall clock through the existing seam (rocketnet_platform_set_wall_clock_for_testing) once per 16 s of caller time, never per update. err = wall - token clock; nothing inside a 1 s dead band. Behind (err > 0): offset grows by at most 1 s per second of caller time since the last read, so the token clock runs at most 2x: a 1-hour lag is caught up in an hour. Ahead (err < 0): offset shrinks by at most 0.5 s per second of caller time, so the clock runs at least at half speed and never stands still or goes back. Why the bounds: going back is the security direction (an expired token valid again; a request-claim entry outlived), so the clock is non-decreasing, which keeps RNSecurityTables' and ServerConnection's forward-only clock actions valid as they are. Going forward costs only availability: whoever steps the wall clock gains at most 1 s of lead per second held, the lead is undone at 0.5 s per second afterwards, and a one-year step cannot lock connections out for good (an instant monotone follow would) or admit an expired token. Cost: one wall read per 16 s, a compare and a few adds per update; no thread, API, ABI or counter change. With it: a model proofs/tla/RNTokenClock.tla (Monotone, BoundedSlew, liveness: error within 1 s under a steady wall) with reversed witnesses for instant follow (lockout) and backward follow (resurrection), bound by a trace walk; header and SECURITY.md text (disciplined, and a paused caller clock then catches up at 2x); a TRANSPORT.md t12 sentence defining the server's current time and its bounds, as a carve-out from t2 rule 6. Red test to start from: tests/test_unit_token_clock_wall_drift.cpp with a fake wall clock that steps or drifts while the caller's clock does not."
   :date "2026-11-13" :origin "Review before v1.2.0, finding R40; option A of the three put to the owner")

  (item "zero-pad-refusal-reasons" :group "review-v1-2" :area "rocketnet"
   :title "Give the two zero-pad refusals their own counters"
   :text "A server counts a signed connect data zero pad that is not all zero as CONNECTION_REQUEST_WRONG_VERSION (sdk/rocketnet_server_protocol.cpp about 563-587), and a private block zero pad that is not all zero as CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK, which also records a failed-signature budget failure (about 848-853). TRANSPORT.md t6.4 now states where each check sits and states both as exceptions to t18 rule 2. Counters are ABI and the counter set is frozen for the point release, so the reasons get their own counters in a release that may add counters; then t6.4 drops the exception."
   :date "2026-11-13" :origin "Review before v1.2.0, wave 2, finding W2-80")

  (item "response-no-slots-own-counter" :group "review-v1-2" :area "rocketnet"
   :title "Count a response ignored for just-freed slots apart from a denial"
   :text "A connection response that finds only slots freed in the same update is ignored, not denied, and counts CONNECTION_RESPONSE_NO_SLOTS, the counter of a real denial (sdk/rocketnet_server_protocol.cpp about 1176-1186); connections_denied_full sums both. TRANSPORT.md t12 rule 13 and t13.4 now state the ignore. Give it its own reason in a release that may add counters, and update t12, t18 and the stats doc with it."
   :date "2026-11-13" :origin "Review before v1.2.0, wave 2, finding W2-105")

  (item "half-open-idle-own-counter" :group "review-v1-2" :area "rocketnet"
   :title "Count a half-open idle removal apart from a connected timeout"
   :text "A half-open mapping removed at its idle deadline and a connected slot removed at its idle timeout both count MAPPING_IDLE_TIMEOUT; the connected one also counts CLIENT_TIMED_OUT, so the half-open count is the difference. TRANSPORT.md t12 rule 14 and t18 rule 2 now state this as an exception. Give the half-open removal its own counter in a release that may add counters, and update t12, t18 and the stats doc with it."
   :date "2026-11-13" :origin "Standard review, merged table row SM052")

  (item "client-denied-counter-class" :group "review-v1-2" :area "rocketnet"
   :title "Decide the class of the client's denied WRONG_STATE and ACCEPTED counters"
   :text "CLIENT_DENIED_WRONG_STATE and CLIENT_DENIED_ACCEPTED are of class refusal, where every other client WRONG_STATE and ACCEPTED counter is expected. The class is part of the counter registry and its vector, so TRANSPORT.md t18 keeps them as registered and says so. A release that may change counter classes decides whether they become expected."
   :date "2026-11-13" :origin "Standard review, merged table row SM040")

  (item "promotion-enabled-mask" :group "review-v1-2" :area "rocketnet"
   :title "Decide what admits a promoted send, and count its refusal once"
   :text "A sender promotes an UNRELIABLE or UNRELIABLE_WITH_ACK_RECEIPT message above its one-record limit to RELIABLE, and an UNRELIABLE_SEQUENCED one to RELIABLE_SEQUENCED, then checks that exact reliability against the enabled mask; RELIABLE_WITH_ACK_RECEIPT does not admit it, although it sends under the same wire code. The refusal counts both MESSAGE_SEND_TYPE_NOT_ENABLED and MESSAGE_SEND_REFUSED, where an unpromoted unreliable send outside the mask counts only the first. The public interface enables all eight reliabilities and has no mask setting, so only a restricted internal mask or another implementation reaches this. MESSAGES.md m2.1 states this behaviour. A later release decides whether the receipt twin admits a promoted UNRELIABLE_WITH_ACK_RECEIPT send, whether config validation warns when an enabled unreliable reliability lacks its promoted one, and whether the refusal counts one counter."
   :date "2026-11-13" :origin "Review before v1.2.0, wave 2, finding W2-114")

  (item "go-backend-protocol-id-parse" :group "review-v1-2" :area "rocketnet"
   :title "The Go and C# example backends parse the protocol id differently"
   :text "examples/backend/go/main.go (about 344-350) parses the protocol id with base auto-detect, so 010 is octal 8 there, while the C# backend parses it as decimal 10; make both parse one way and say which in the examples' docs."
   :date "2026-11-13" :origin "Review before v1.2.0, wave 2, finding W2-119")

  (item "sent-map-receipts-give-way" :group "review-v1-2" :area "sdk"
   :title "Receipt-only packets can push a reliable entry out of the sent map before its ack lands"
   :text "Packets that carry only unreliable receipt serials (rn_window_ack_commit_receipt_packet) take entries in the same P-entry sent map as packets with reliable numbers, and W does not bound them. When reliable and receipt-only packets written within one round trip pass P, rn_window_ack_commit (sdk/rocketnet_window_ack.cpp) evicts the oldest entry with its ack on the way: the ack is ignored, its numbers are resent and can hold U, and a steady excess can evict every send of a reliable number until reliable_message_timeout_ms ends the connection. For v1.2.0 this is the documented contract (packets_in_flight in sdk/rocketnet.h, standard/MESSAGES.md m9.1, docs/CHANNELS.md: size P for every tracked packet sent between a send and the processing of its ack, since an entry is evicted only by the P-th tracked write after it), and tests/test_unit_sent_map_receipt_eviction.cpp pins it by value. Model: proofs/tla/RNSentMap.tla, witness MCRNSentMapReceiptEviction.cfg. Red-test recipe at W = P = 64: (1) 64 one-number reliable packets then one receipt-only packet in one update: 63 of 64 acks found, U stays 0, number 0 stays due for resend; (2) reliable, receipt-only, then 63 reliable: the oldest reliable entry is evicted, not the receipt-only one, so refusing only at the receipt-only commit is not enough; (3) 60 Hz, 100 ms round trip, 11 receipt-only packets an update and one reliable message, 2 s reliable timeout: every send is evicted and the connection ends RELIABLE_TIMED_OUT. Reachability at the defaults (W = P = 1024, 10 Mbps per client, 60 Hz, at most 16 receipt records a packet): full 1416 B packets give about 88 tracked packets per 100 ms, so it needs a round trip above about 1.16 s; tiny receipt messages packed 16 to a packet (about 250 B) give about 500 per 100 ms, so about 205 ms with about 1300 receipt messages a client an update (10 Gbps at 1000 clients); 1 to 3 tracked packets an update needs 6 to 17 s; the 1000 kbps client default needs acks delayed about 2 s (about 500 packets of 250 B a second against P = 1024). Reachable with an unlimited send budget (256 packets an update), small maps (P = 64: 11 an update at 100 ms), or acks delayed about a second. Repair options: (A) receipts give way inside the one ring: when a write needs room and the head holds an unacked reliable number, give up the oldest entry that holds none (a receipt-only entry, an acked hole or a copy whose numbers another copy's ack completed), shifting the entries in front of it and their refs up one place; if every entry holds one, a reliable write evicts the head (P >= W proves its ack is not on its way only while acks arrive in send order; out of order, faster acks of later packets (resends of lost lower numbers among them) let more than W reliable packets follow a live entry, so the repair's model needs variable ack delay) and a receipt-only write is not entered and reports LOSS at once. Memory flat, no layout change, nothing changes while the map has room; but the overflow path scans and shifts up to P entries with up to 16 refs each on the main thread exactly when the remote endpoint withholds acks, a cost a hostile client or server can drive, so it needs a bound before it ships (for example a cap on the scan, or keeping receipt-only entries off the reliable head's path). (B) a ring of its own for receipt-only packets: the reliable ring and its proof are untouched, but the region layout in rocketnet_allocator.cpp changes and memory grows 128 B x its entries a connection (+128 KiB a client at P entries, +125 MiB for 1000 clients), or LOSS starts earlier with a smaller ring. Either changes RNSentMap (and for A RNSentRingLayout and RNReceipts EvictMap) in the same commit, with MCRNSentMapReceiptEviction.cfg run on the old rule as the reversed witness."
   :date "2026-11-13" :origin "Audit round 2, finding S002 (three readers); reproduced and documented in fix/v1.1.x/sent-map-receipt-eviction")

  (item "receipts-after-transport-end" :group "after-v1-1-landing" :area "sdk"
   :title "Keep a transport-ended connection's receipt outcomes readable for one update"
   :text "Today a connection ended by an explicit disconnect or kick, or by the message layer at the end of an update, keeps its outcomes readable until the next update (or, on the client, a connect that ends it), but a connection the transport ends inside an update (timed out, disconnected by the other side), or that an update ends for MESSAGE_NOT_RECEIVED, reports no outcomes from that update on, so outcomes recorded since the previous update are lost; rocketnet.h, docs/REQUIREMENTS.md and docs/RELIABILITY.md now say so (fix round 1 of the 2026-11-10 review). Keeping them is a lifecycle change: a KEEP decision for the receipt ring in the connection reset inventory (docs/CONNECTION-RESET.md), a read gate that admits an ended slot for one update instead of only live slots, an end_update for the dropped slot on the update after, RNReceipts and RNSlots carrying the ended-but-readable state with reversed witnesses and the trace walk, and on the client storage that survives a retrying connect (connect reuses the block today)."
   :date "2026-11-13" :origin "2026-11-10 review, fix round 1, item 4")

  (item "windows-create-status-unset" :group "after-v1-1-landing" :area "sdk"
   :title "Windows create failures that leave the status unset"
   :text "The POSIX and macOS wake create now set OUT_OF_MEMORY for ENOMEM and ENOBUFS (sdk/rocketnet_platform.cpp, rocketnet_platform_wake_create, fix round 1 of the 2026-11-10 review). The Windows paths named by the same finding are not fixed: several failure paths of rocketnet_rio_lane_create (sdk/rocketnet_server_rio_windows.cpp, from about 1243; the mutex create at about 1310-1313 is one) reach failed with no status set, and a Winsock refusal for want of buffers there reads as a plain failure, not OUT_OF_MEMORY. Each failure path is to be checked, given its status (rocketnet_platform_windows_create_status for a Winsock or Win32 error), and pinned by a fail_stage unit case run on a Windows bench."
   :date "2026-11-13" :origin "2026-11-10 review, fix round 1, item 7")

  (item "loopback-exchange-only-handoff" :group "after-v1-1-landing" :area "sdk"
   :title "Loopback: hand datagrams over by exchange only, not under the hub mutex"
   :text "sdk/rocketnet_driver_loopback.cpp holds the hub-wide mutex across the destination lookup and the copies of a send, a whole server batch send, and a receive (about 727-885), against docs/decisions/BUFFER-HANDOFF-DECISION-20261021.md, which allows the mutex to protect the exchange only. Nothing is unsafe; it serialises every endpoint on a hub. The decision file records it as a scoped exception (2026-11-13). The rewrite: a bounded per-destination queue filled outside any lock and published by an index exchange, the receiver consuming published slots the same way, refusals counted as today, the lookup reading a table that changes only at endpoint create and destroy, and the loopback models (LoopbackHub, LoopbackQueue, LoopbackHubLayout, RNLoopbackBindLayout) carried with it."
   :date "2026-11-13" :origin "2026-11-10 review, ROUND1-MERGED row R127 (Stella's Astra A09)")

  (item "message-destroy-misuse-owner" :group "after-v1-1-landing" :area "sdk"
   :title "Message destroy misuse: an owner for a transport whose worker outlives the wait"
   :text "The failed-create unwind now waits for the worker with no deadline (rn_message_link_abort_create, proofs/tla/RNCreateUnwind.tla). What remains: rocketnet_message_server_destroy or rocketnet_message_client_destroy called before the transport stopped is a counted misuse that stops the transport itself and waits one second (RN_MESSAGE_LINK_STOP_WAIT_SECONDS, rn_message_object_destroy in sdk/rocketnet_message_server.cpp). If the worker has not exited by then the transport is logged as kept, not freed, and the object is freed, so nothing owns the transport. The public server and client destroys stop first and keep their own handle while the worker runs, so only a layered caller that skips stop reaches it. The fix: a wait with no deadline there too, or keep the object while the worker runs as rocketnet_server_destroy does."
   :date "2026-11-13" :origin "2026-11-10 review, fix round 1, cold read of review fix 1; reduced when the create unwind was fixed")

  (item "loopback-hub-destroy-check-and-free" :group "review-v1-2" :area "sdk"
   :title "Loopback hub destroy checks for endpoints, then frees the hub outside the lock"
   :text "rocketnet_loopback_hub_destroy (sdk/rocketnet_driver_loopback.cpp about 521-559) reads registered under the hub mutex, releases it, then destroys the mutex and frees the block. A create on another thread in that window registers into memory about to be freed. Creating while destroying is misuse, but the in-use guard reads as if it catches it. Make the check and the retirement one step under the mutex (a create after it refused with the hub's in-use or mismatch status), show it red with two threads through a deterministic seam, and make LoopbackHub.tla's Destroy (atomic today) two steps with a reversed witness. A create that is already waiting on the mutex when the hub is freed stays misuse that no flag can catch: say so in rocketnet.h."
   :date "2026-11-13" :origin "Review before v1.2.0, second wave, o3 finding W2-11 (read, not run)")

  (item "loopback-port-zero-bind-walk" :group "review-v1-2" :area "sdk"
   :title "A loopback port-0 bind walks every endpoint for each candidate port under the hub mutex"
   :text "loopback_create (sdk/rocketnet_driver_loopback.cpp about 588-668) checks each candidate port of the ephemeral range against every live endpoint record, O(range x endpoints) with the hub mutex held, although hub->bound[family][port] already answers each port in one lookup (RNLoopbackBindLayout.tla LookupIsWalk). Use the table for both the explicit-port conflict check and the search; show it with a counted bound on a nearly full hub."
   :date "2026-11-13" :origin "Review before v1.2.0, second wave, o3 finding W2-12 (read, not run)")

  (item "loopback-server-drain-budget" :group "review-v1-2" :area "sdk"
   :title "The server's loopback queue is sized by the hub's clients, its receive budget by the server's"
   :text "rocketnet_loopback_server_queue_packets sizes the server endpoint's queue from the hub's max_clients, and the server transport's loopback receive drains up to a budget from the server's own max_clients (server_transport loopback receive about 4340-4430). With a hub larger than the server, a full queue holds more than one update drains: an update of added latency. Size the drain so one update empties the queue, or say why not."
   :date "2026-11-13" :origin "Review before v1.2.0, second wave, o3 finding W2-20 (read, not run)")

  (item "loopback-queue-model-full-drop" :group "review-v1-2" :area "proofs"
   :title "LoopbackQueue cannot reach its full drop, and its Drop does not count"
   :text "MCLoopbackQueue.cfg (Capacity 4, BacklogMax 2) never fills the queue, so Send's full-drop branch is never explored (COVERAGE.tsv says so), and Drop does not increment dropped although loopback_client_receive and loopback_server_receive_batch count it. Give the cfg a backlog that reaches the drop and model the counter, with a reversed witness of an uncounted drop."
   :date "2026-11-13" :origin "Review before v1.2.0, second wave, o3 finding W2-10 (read, not run)")

  (item "ci-functional-step-name-per-os" :group "review-v1-2" :area "github"
   :title "The functional suite step says 'on Linux' on the macOS runner"
   :text "One step of .github/workflows/ci.yml, 'Run functional suite (automatic transport order — the fall, on Linux; RSS enforced)', runs on the Linux and the macOS rows, so docs/PLATFORMS.md's macOS 27 row, which must name the step word for word (tools/check_platform_policy.py), reads 'on Linux'. Rename the step so it names no OS (or give each OS its own step), and change tools/planted_violations.tsv and both PLATFORMS.md rows in the same commit."
   :date "2026-11-13" :origin "Review before v1.2.0")

  (item "xdp-reserve-cpu-0" :group "review-v1-2" :area "sdk"
   :title "xdp has no reserve_cpu_0 setting: it needs a provisioning mode without a housekeeping cpu"
   :text "gso, io_uring and portable on Linux each have a *_reserve_cpu_0 config field and environment variable (ROCKETNET_CPU_0_NOT_RESERVED puts network thread q on cpu q). xdp has none, because its placement is fixed by its provisioning contract: scripts/rocketnet-provision keeps cpu 0 as the housekeeping and interrupt cpu and isolates the cpus the xdp threads run on (R4), so a thread on cpu 0 would share it with every IRQ the provisioning moved there. Giving xdp the setting takes a provisioning mode without a housekeeping cpu: where the IRQs and kernel threads go instead, how the NIC queues map to cpus from 0, and a measured peak on that layout before it ships."
   :date "2026-11-13" :origin "Linux thread placement fix (v1.1.x point release), the owner's option (b)")

  (item "windows-reserve-cpu-0" :group "review-v1-2" :area "sdk"
   :title "rio and iocp have no reserve_cpu_0 setting: their workers follow the NIC's receive-side scaling"
   :text "The Windows transports put lane or worker i on core i, cpu 0 included, so each worker runs on the core RSS delivers its queue's interrupts and completions to (sdk/rocketnet_server_rio_windows.cpp, sdk/rocketnet_server_iocp_windows.cpp). Moving them off cpu 0 would split a queue from the core RSS steers it to unless the NIC's RSS base processor moves with it. A setting for Windows takes reading and setting the adapter's RSS base processor (Set-NetAdapterRss -BaseProcessorNumber) in step, and a measured peak on that layout."
   :date "2026-11-13" :origin "Linux thread placement fix (v1.1.x point release), the owner's ruling: no Windows behaviour change")

  (item "sparse-cpu-ids" :group "review-v1-2" :area "sdk"
   :title "The Linux pin helpers take the online cpu count as a dense range of cpu ids"
   :text "rocketnet_gso_pin_check and rocketnet_gso_pin_fallback (sdk/rocketnet_gso_pin.h) treat sysconf(_SC_NPROCESSORS_ONLN) as the ids 0 .. online - 1, and gso, io_uring, portable and the uring helpers inherit it. With cpus taken offline (hotplug) the ids are sparse: a pin to an online cpu whose id is at or past the count is refused as offline, and the fallback can name offline ids and leave out online ones, so a confinement that could hold fails. The placement takes the online and allowed cpus as a mask (sched_getaffinity, /sys/devices/system/cpu/online) instead of a count, with unit cases on sparse layouts through the fake affinity calls. The functional placement cases read the allowed mask (rocketnet_test_placement_host) but take ids as dense the same way, and move with the helpers."
   :date "2026-11-13" :origin "Linux thread placement fix (v1.1.x point release), first review (static), advisory: inherited, not changed in a point release")

  (item "in-place-start-per-queue-capacity" :group "review-v1-2" :area "sdk"
   :title "The message server's in-place start checks still read the fewest room of any queue"
   :text "The send pass, the sweep and the stop now judge capacity by the destination's own send queue (rocketnet_server_transport_send_capacity_to). The in-place start checks in sdk/rocketnet_message_server.cpp (about 1543 and 1597) still read rocketnet_server_transport_send_capacity_available, the fewest room of any queue, so with one queue full a send in place to a client on another queue is refused or deferred to the pass. A start that falls back to the queue is accepted work and is not lost: the pass serves it. A writer's start that falls back with no capacity (not held behind the queue, not on a buffered transport) is still refused visibly: its finish returns 0 and the message is cancelled (about 1661 and 1816, MESSAGE_UNRELIABLE_WRITER_NO_CAPACITY), even when only an unrelated queue has no room. That refusal is what this entry removes. Move them to the per-destination answer, outside the send-capacity fix's lines."
   :date "2026-11-13" :origin "Review before v1.2.0, send queue capacity fix")

  (item "own-frames-smallest-queue-reserve" :group "review-v1-2" :area "sdk"
   :title "Own frames are bounded by the smallest reserve of any send queue"
   :text "The own frames handed out when the budgets open (fill_own_frames_left) are server-wide and capped by the smallest queue reserve (RNSendReserveCalls Open: OwnCap of the least reserve). With uneven queues, a queue whose reserve is cut by a small room seen caps the own frames of clients on every queue. Make own frames per queue, with the model's Open changed alongside."
   :date "2026-11-13" :origin "Review before v1.2.0, send queue capacity fix")

  (item "send-capacity-any-queue-per-queue" :group "review-v1-2" :area "sdk"
   :title "The any-queue capacity can be above 0 while no queue has both room and budget"
   :text "rocketnet_server_transport_send_capacity_to( -1 ) answers the room summed over the queues, within the whole budget left. With room on one queue and budget only on another, it is above 0 while no start can be granted, so the send pass holds every connection to the end instead of cutting: bounded (one walk, two capacity reads a connection), never a hang. Answer it per queue (the sum of each queue's room within its own budget, and the own frames), with RNSendReserveCalls CapacityAll and RNServerSendFair's cut changed alongside."
   :date "2026-11-13" :origin "Review before v1.2.0, send queue capacity fix, cold read")
  (item "iocp-send-room-fewest" :group "review-v1-2" :area "sdk"
   :title "The iocp transport's send_room sums its lanes; every other transport reports the fewest"
   :text "rocketnet_server_iocp_send_room (sdk/rocketnet_server_iocp_windows.cpp) adds the room of every lane, where xdp, gso, io_uring, msgx and rio report the fewest free frames of any queue, as rocketnet_server_transport_send_capacity_available and RNSendReserveCalls Capacity assume. The stop, the sweep and the drains judge each queue by send_room_queue once send_queues is read at the io's open, so what remains is the whole-server readers: send_capacity_available (the in-place start's check, which then tries a frame on a full lane and is refused there instead of falling back first) and io_refused_full with no destination. Report the fewest lane's room, with a Windows build and the iocp bar rows on both axes, since it moves the in-place start's fallback on the hot path."
   :date "2026-11-13" :origin "Review before v1.2.0, send queue capacity fix, Emma's review SQ-2")

  (item "server-ipv4-address-unit" :group "after-v1-1-landing" :area "tests"
   :title "A unit case for rocketnet_server_ipv4_address beside its IPv6 twin"
   :text "rocketnet_server_ipv6_address has a unit case for a stopped server, a running one, the needed-1 buffer size and the wrong family (test_public_server_ipv6_address_reads_the_running_address in tests/test_unit_public_api_core.cpp); rocketnet_server_ipv4_address has only the NULL-server misuse check in sdk/rocketnet_tests.cpp and the functional tests that read it. Add the matching case for the IPv4 call on the loopback transport: stopped answers false, running answers the bound address and port, a buffer one byte short is refused, and an IPv6-only server answers false."
   :date "2026-11-13" :origin "2026-11-10 review, tests merge row TM032 (optional: new coverage)")

  (item "client-bind-family-exact-foreign-count" :group "after-v1-1-landing" :area "tests"
   :title "Client bind family: count the late IPv6 cookie exactly"
   :text "test_unit_client_bind_family.cpp checks ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE >= 1 after the one late IPv6 cookie the case sends (about line 416), so a double count passes. If the case's schedule fixes the count (one late cookie, no other foreign datagram), check it moved by exactly 1 from a reading taken before the cookie is sent; if it does not, say why beside the check."
   :date "2026-11-13" :origin "2026-11-10 review, tests addendum row TA008, P7's site (optional)")

  (item "portable-tx-wait-kind-witness" :group "review-v1-2" :area "sdk"
   :title "The portable tx worker's choice of wait has no witness"
   :text "portable_tx_worker (sdk/rocketnet_driver_builtin.cpp, the WAIT / WAIT_CARRY branch) maps the step's result to wake_wait(0.1) or wake_wait_writable(0.001) inline, and the carry_wake fixture checks only the step's enum, so swapping the two waits passes every unit case. Lift the mapping into a pure helper the worker calls (step result -> wait kind and timeout) and check it in the fixture, red on a swapped branch. The portable transport is the fallback, so this waits until it is worth an sdk change."
   :date "2026-11-13" :origin "Tests review v1.2 merge, row TM154 (P9)")

  (item "unit-transport-families-unsupported" :group "review-v1-2" :area "tests"
   :title "A unit case that reaches ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED"
   :text "tests/test_unit_status.cpp checks TRANSPORT_FAMILIES_UNSUPPORTED only as a name and number; no unit case makes a create return it (sdk/rocketnet_server_transport.cpp, the served-families check; sdk/rocketnet_driver_builtin.cpp). A server of both families on a transport that serves one family, refused before any socket, would reach it in the unit tier: a loopback-style fake that declares one family, or the families check read directly."
   :date "2026-11-13" :origin "Review before v1.2.0, tests track row TM101 (optional), tests/v1.1.x/p8-status-config-units")

  (item "unit-purity-file-reads" :group "review-v1-2" :area "tools"
   :title "The unit purity gate sees file reads and host-state reads"
   :text "tools/check_unit_purity.py exempts tests/suite_runner.h and has no pattern for a file read (fopen, /proc, /sys) or a host query (sched_getaffinity, sysctlbyname, GlobalMemoryStatusEx), so a unit case that called the runner's live helpers read the host's cgroup, memory and affinity unseen (row TM036, repaired on tests/v1.1.x/p8-status-config-units by passing those inputs in). Teach the gate those patterns for unit sources, with the runner's own use (it plans the job count) allowed by name only."
   :date "2026-11-13" :origin "Review before v1.2.0, tests track row TM036 (Ro O-M1), tests/v1.1.x/p8-status-config-units")

  (item "aead-open-argument-checks" :group "review-v1-2" :area "tests"
   :title "Isolate the open-side argument checks of the packet AEAD"
   :text "The open-side argument checks in sdk/rocketnet_protocol.cpp (327-340 at 5a2dbf403: the sequence-0 guard, the nonce guard and the ephemeral-key discipline) are not tested on their own: the two packets the test in sdk/rocketnet_tests.cpp feeds fail the AEAD anyway, so deleting either guard passes, and the ephemeral discipline on open is never called with a mismatched argument. Add cases whose only fault is the argument each guard refuses (a packet that would open but for that argument), each shown red when its guard is deleted."
   :date "2026-11-13" :origin "Tests review before v1.2.0, row TM220 (optional)")

  (item "layer-export-table-check" :group "review-v1-2" :area "tools"
   :title "Hold the layer NULL-audit table to rocketnet_layers.h with a tool"
   :text "tools/check_exports.py holds rocketnet_test_exports (the public NULL audit in sdk/rocketnet_tests.cpp) to sdk/rocketnet.h, but nothing compares rocketnet_test_layer_exports with the entry points rocketnet_layers.h declares, so a new layer entry point with no row, no NULL-case list entry and no NULL call passes. Extend the tool (or add one beside it) to derive the layer entry points from the header and refuse a missing, undeclared or duplicate name, and to refuse a NULL-case name with no NULL call in rocketnet_test_public_api_null_is_a_counted_no_op."
   :date "2026-11-13" :origin "Tests review before v1.2.0, row TM085 (follow-up)")

  (item "xdp-provision-restart-mode" :group "review-v1-2" :area "rocketnet"
   :title "XDP: one provision step for a restart on NICs whose attach rebuilds the queue vectors"
   :text "On ixgbe-class NICs the XDP attach and detach rebuild the queue vectors onto the default affinity (cpu 0 under irqaffinity=0), so a safe start is three steps: provision, start the server, repin; and a restart needs provision again first, because the old server's detach moved the vectors back (docs/DEPLOYING.md step 4a). The library only warns after its attach and never changes affinity (standard/TRANSPORT.md t2 rule 5). A script mode that provisions, waits for the new server's attach, then repins (what repin already waits for: the program, the carrier, one queue vector a thread) would make it one operator step, for example a systemd ExecStartPost. Needs an ixgbe bench to show it. Provision refuses to unload rocketnet's program while any rocketnet server on the host is live and has no override (v1.2, no new options); such a mode is where a deliberate unload of a live server's program would belong, and the pin root could record its interface so a server on another interface no longer blocks. Also: create's post-attach re-read is silent when it finds no queue vector at all (a driver still rebuilding them when /proc/interrupts is read); counting the vectors the pre-flight saw against the re-read would let it say pinning could not be re-checked."
   :date "2026-11-13" :origin "Review before v1.2.0, wave 2, findings W2-36 and W2-71")

  (item "xdp-capabilities-not-root" :group "review-v1-2" :area "rocketnet"
   :title "XDP: accept a server with the needed capabilities instead of euid 0"
   :text "The pre-flight's R1 and the script refuse any euid but 0. What the XDP transport needs is a capability set: CAP_NET_ADMIN (attach, ethtool reads), CAP_BPF (program and map loads, with CAP_PERFMON on some kernels), CAP_NET_RAW (AF_XDP sockets), CAP_SYS_RESOURCE (the memlock rlimit), CAP_SYS_NICE (SCHED_RR and affinity), plus write access to the root-owned bpffs directory for the instance's pin root. Read the effective set (capget) in R1 and name the missing capabilities. Needs a run on a provisioned XDP bench as a non-root user with those capabilities, to find which the kernel really asks for."
   :date "2026-11-13" :origin "Review before v1.2.0, wave 2, finding W2-45")

  (item "xdp-config-map-after-xsk-map" :group "review-v1-2" :area "rocketnet"
   :title "XDP: write the config map after the sockets are in the XSK map"
   :text "create writes the program's config map (address and port the program redirects) before every queue's socket is in the XSK map, so for a few microseconds before create returns a datagram to the port on a queue whose socket is not yet in the map is redirected to an empty slot and counted DROPPED_NO_SOCKET, where with the order reversed it would pass to the kernel stack and find no socket there either. Not shown red: the window is before create returns, and frames in it are lost either way. Reorder (sockets into the XSK map first, config map last) and cover it in the create-order unit seam."
   :date "2026-11-13" :origin "Review before v1.2.0, wave 2, finding W2-43")

  (item "xdp-preflight-mtu" :group "review-v1-2" :area "rocketnet"
   :title "XDP: a pre-flight rule for an MTU native XDP refuses"
   :text "A driver refuses a native XDP attach when the link MTU is above what it can serve with one buffer a frame (ENA at 9001, i40e and ixgbe above about 3000 to 3500 without multi-buffer support). v1.2 names the errno and the link MTU in the attach refusal, so the operator can see it, but the pre-flight does not check it, and auto falls to gso only after the attach fails. A pre-flight rule reading /sys/class/net/IF/mtu against the transport's frame size would name it before anything is loaded. Needs the per-driver limits measured on the benches before it can refuse rightly."
   :date "2026-11-13" :origin "Review before v1.2.0, wave 2, finding W2-44")

  (item "xdp-gateway-neighbour-probe" :group "review-v1-2" :area "rocketnet"
   :title "XDP: resolve a missing gateway neighbour entry instead of refusing"
   :text "create refuses the XDP transport when the gateway's neighbour entry is missing or unusable, and nothing makes the kernel resolve it; v1.2 says so and names the fix (ping the gateway once, ip neigh show, or ROCKETNET_GATEWAY_ETHERNET_ADDRESS). A probe at create (one datagram to the gateway through an ordinary socket, then the neighbour query again with a bounded retry) would resolve it the way the kernel would for any socket. Also from the same reading: only the first default route on the interface is used, and a multipath route's RTNH_F_DEAD and RTNH_F_LINKDOWN next hops are not skipped."
   :date "2026-11-13" :origin "Review before v1.2.0, wave 2, finding W2-40")

  (item "xdp-rr-worker-napi-softirq" :group "review-v1-2" :area "rocketnet"
   :title "XDP: a spinning SCHED_RR worker shares its core with its queue's interrupt"
   :text "R4 pins queue k's interrupt to the cpu xdp thread k spins on at SCHED_RR. The NAPI softirq runs on that cpu on interrupt exit, ahead of the thread, and is fine; but when softirq work is deferred to ksoftirqd (SCHED_OTHER) under overload, the spinning real-time worker can starve it, and the queue's receive stalls behind its own worker. Unsure: busy polling (R5) and the zero-copy path may keep the work out of ksoftirqd. Measure first: a peak run on the X710 pair with the worker at SCHED_RR against SCHED_OTHER, reading ksoftirqd's run time on the worker cpus (/proc/PID/schedstat) and the receive drops."
   :date "2026-11-13" :origin "Review before v1.2.0, wave 2, finding W2-38")

  (item "xdp-queue-change-after-start" :group "review-v1-2" :area "rocketnet"
   :title "XDP: a queue-count change or device unregister after start is silent"
   :text "After start, an ethtool -L on the interface or the device going away (driver reload, hot unplug) leaves the server bound to queues that no longer exist or to a dead device: datagrams on new queues count DROPPED_NO_SOCKET and nothing else says so, and nothing recovers. Watch the link with an RTNLGRP_LINK subscription on a network thread, log once by name, and say in the docs that a channel change needs a restart."
   :date "2026-11-13" :origin "Review before v1.2.0, wave 2, finding W2-42")

  (item "provision-script-gaps" :group "review-v1-2" :area "rocketnet"
   :title "rocketnet-provision: read-back of R5, a settle wait after R7, and an unprovision mode"
   :text "From the same reading as the repin fix: provision writes R5's napi_defer_hard_irqs and gro_flush_timeout and does not read them back (R7 and R8 do); after an R7 channel change R4 reads /proc/interrupts at once, while some drivers are still rebuilding their vectors (repin already waits for them; provision should wait the same way, bounded); and there is no mode that puts R4 to R8 back (affinities, busy-poll pair, channels, sched_rt_runtime_us) for a host leaving XDP service."
   :date "2026-11-13" :origin "Review before v1.2.0, wave 2, finding W2-75 (script parts)")

  (item "worker-known-source-filter" :group "decisions" :area "sdk"
   :title "A worker-side known-source filter for gso and io_uring"
   :text "A session packet (keep-alive, payload, disconnect, ack) from a source with no session passes the stateless prefilter and is refused only on main (*_NO_MAPPING), because the receive worker has no table. The XDP kernel program now drops these (DROPPED_NO_SESSION); the other transports do not. Measured on amd-2 (2026-11-13) with harden_load and scripts/harden-cell: two netns over a veth, 100 players, 20 s, a flood of the prefixed kind (valid prefilter header, a session type at its wire floor plus 0 to 1200 random bytes) from 256 source ports. Main-thread update time avg/p99, before and during the flood: gso at 300k pps sent (about 245k/s reach main) 0.38/0.49 to 0.73/0.86 ms, players 100%; gso at 1M pps sent (about 500k/s reach main) 0.40/0.62 to 1.09/1.34 ms, players 76.6%, the loss in the kernel socket buffer (UDP InErrors 8.43M) at the gso receive worker's recvmmsg ceiling of about 620k/s, RECEIVE_DROPPED_FULL 0; io_uring at 1M pps sent (about 600k/s reach main) 0.58/0.66 to 1.33/1.4 ms, RECEIVE_DROPPED_FULL 4.02M, players lost 37 (99.98%); portable at 300k pps sent (main reads the socket itself, about 190k/s reach main) 0.53-0.74/0.96 to 1.55/1.8-2.0 ms, kernel InErrors 1.44M, players 100%. Main pays about 75 to 85 ns a refused packet. A worker-side filter (a set of session addresses main publishes each update, read by the receive workers before a packet takes a bank slot) would save that main time at the worker's ceiling and, on io_uring, the bank-full drops; on gso the honest loss is in the kernel before the worker, so it would not help there, and portable has no worker. It is new shared state between main and the workers, so it is a feature, not a point-release fix. It changes the same receive code as the receive-handoff fix, so it waits for that."
   :date "2026-11-13" :origin "Hostile-input-cost round 1, item 3c")

  (item "replay-slots-trace-binding" :group "review-v1-2" :area "proofs"
   :title "RNServerReplaySlots has no trace check binding it to the walk"
   :text "proofs/tla/RNServerReplaySlots.tla models the per-slot records of rocketnet_server_replay_marks_walk (seen record, mark, owed mark, skip mark) and their supersedes, but no trace check runs the code's walk against it: the xdp-replay trace check (tests/tla_trace_xdp_replay.cpp) excludes a slot reused while another address's forget is owed. The model also takes the mark pass as one step (Cand and Blocked from the records after the forget pass), where the code decides slot by slot after earlier slots' supersedes; a trace check would need either a per-slot mark step in the model or a refinement mapping that allows it. Bind it as the other layers are: the walk's random driver emits traces, TLC checks each is a behaviour, in CI beside the witnesses."
   :date "2026-11-13" :origin "Hostile-input-cost round 1, the replay orphan fix (Astra H01), cold read finding L3")

  (item "checked-server-per-frame-read-once" :group "review-v1-2" :area "sdk"
   :title "The per-frame checked server calls read the running server more than once"
   :text "rocketnet_server_update, update_receive, update_finish, send_packets, receipts and receive_messages read server->running before their check and again after it (their check opens inside the message server call), where stop, the disconnects and the send rate now take one copy, use only that copy after their check, and return when a stop retired it (fix/v1.1.x/checked-server-overlap-refusals). Under a stop on another thread overlapping one of these calls, which the header forbids and promises to refuse: the overlap can go uncounted (the second read finds NULL before the check opens), a false API misuse can be counted for that NULL (asserting in a debug build), or, when the stop ran whole before the call's check, the call can step or read the stopping run (update and send_packets step its stopping transport, receipts can return receipts from slots the stop just kicked, receive_messages releases its batch and clears the count). No data loss or crash found. The fix is the same pattern: read running once, open the check in the wrapper on that run's transport (an update call for the three update calls), return when refused or when rn_server(server) != running, pass the copy on; the message server's own check then nests on the same thread. It moves where the check opens on the per-frame main-thread calls, so it needs a main-thread cost measurement at the 1000-player bar, before and after, before it lands."
   :date "2026-11-13" :origin "Audit v1.1.58, finding S3-2 (Emma), the per-frame calls left by the overlap-refusals fix; review of 138d227ef, H1")

  (item "standalone-ack-same-update" :group "after-v1-1-landing" :area "sdk"
   :title "A standalone ack waits one update before it is sent"
   :text "When a server update has nothing else for a client, the ack for what the client sent goes out alone, but only in the next update's send pass: it waits one update, 16.7 ms at 60 Hz. An attempt to send it in the same update (W2-52) was stopped because each form made some case worse. Four defects found on the way: a late send pass gated on the whole transport's capacity broke the one-queue send cap (test_server_send_cap_one_three_clients); a keep-alive and an accept in the same update sent two standalone acks (the per-update sweep had to skip what the late pass sent); stamping the ack time grew a pinned struct and delayed the keep-alive plus payload case; a full client I/O-core ring dropped the ack after its debt was cleared. Two room problems left it unshippable: on a buffered (portable) transport send capacity measures the payload ring while the ack goes into the control buffer, so under saturation acks starve, and reserving a control frame needs the cap test weakened; and a game that calls send_packets more than once per update gets extra standalone acks, up to one more packet per client per update. The design question: a send-end point that runs once per update, which capacity a control frame counts against (control buffer or payload ring), and capacity per destination queue rather than per transport."
   :date "2026-11-13" :origin "Fix round 1, W2-52, stopped; work kept on wip/standalone-ack-same-update (37abc02ad)")
 ))
