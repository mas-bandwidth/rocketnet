; The rocketnet roadmap: the data ROADMAP.md is generated from.
; Same shape as nova-sprint's docs/roadmap.sexp (internal/roadmapdoc's package comment):
; data, never evaluated. Edit this file, then regenerate ROADMAP.md with nova-sprint's
; generator, from a nova-sprint checkout:
;   go build -o /tmp/roadmap ./tools/roadmap && (cd <this repo> && /tmp/roadmap --file docs/roadmap.sexp --out ROADMAP.md)
; Never edit ROADMAP.md by hand. Created 2026-10-14 when this repository's open GitHub issues and
; unfinished pull requests were moved into it.
(roadmap "v1"
 :title "rocketnet roadmap"
 :text "Planned work for rocketnet. The items below were open GitHub issues and pull requests, moved here on 2026-10-14 when roadmaps became the standard for future work (Glenn, 2026-10-14). Each item names its issue or pull request; the branches of moved pull requests are kept to resume from. Landed work is in git. The release plan (v1.1.0 to v1.4.0, its streams and cards), which ROADMAP.md held until now, is kept unchanged in docs/RELEASE-PLAN.md."

 :groups
 (
  (group "github-issues" :title "Moved from GitHub issues on 2026-10-14"
   :text "Open GitHub issues of mas-bandwidth/rocketnet, recorded here on 2026-10-14 when roadmaps became the standard for future work (Glenn, 2026-10-14). Each item names its issue; the issue is closed with a pointer here.")
 )

 :items
 (
  (item "issue-8" :group "github-issues" :area "rocketnet"
   :title "The RocketWerkz deliverable set"
   :text "Glenn's definition of what rocketnet must deliver to Dean, 2026-08-26, verbatim: replacement for raknet that isn't IO bound zero copy good stuff for sending/receiving packets he wants a C# interface to it he is going to want it to be super fast, reliable, maintained, bug free he has 'chunks' in his small game, so if we do some sort of create flow, destroy ..."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #8, opened 2026-08-26 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "8" :labels ""))
  (item "issue-17" :group "github-issues" :area "rocketnet"
   :title "Per-title generated stateless filter: change the pattern, generate the code, retire the old names"
   :text "Glenn, 2026-08-26, verbatim intent: 'the pittle/chonkle stuff is inspired from network next (I think!) and we need to change the pattern. In fact, it would be nice to create a little generator that builds the code (randomized per-title) to create this little stateless filter.' And the mechanism, his words: 'I take a hash of stuff, then shmear that hash wide ..."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #17, opened 2026-08-26 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "17" :labels ""))
  (item "issue-19" :group "github-issues" :area "rocketnet"
   :title "server_xdp: IPv6 support"
   :text "Glenn, 2026-08-26: 'we should update the server_xdp to support IPv6!!! todo, not right now.' Today the program handles ETH_P_IP only; IPv6 traffic takes XDP_PASS to the kernel stack and never reaches the ring. Support means: parse the v6 header chain, the same length bounds and stateless structural check, redirect to the per-queue ring."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #19, opened 2026-08-26 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "19" :labels ""))
  (item "issue-23" :group "github-issues" :area "rocketnet"
   :title "Connection migration for mobile clients, a future version"
   :text "Ruled out for 1.0 on 2026-08-26 and recorded in STANDARD.md with the reasoning. This issue holds the future version. The assumption 1.0 makes: a client keeps one IP address and port for the duration of a connection. A mid-session change ends the session and the client reconnects with a fresh token. netcode assumes the same."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #23, opened 2026-08-26 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "23" :labels ""))
  (item "issue-24" :group "github-issues" :area "rocketnet"
   :title "Open questions for Dean"
   :text "A running list of open questions for Dean Hall, so they accumulate in one place and get asked in a batch rather than one at a time. Answered items stay, marked, because the answer is the design record. ANSWERED Platform target. PC primarily, session based games (Glenn, 2026-08-26)."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #24, opened 2026-08-26 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "24" :labels ""))
  (item "issue-77" :group "github-issues" :area "rocketnet"
   :title "The high-level server and client: the tier space and RocketWerkz will expect"
   :text "Glenn's rulings, 2026-08-28 live, verbatim: 'To land this for real, the combined this we call client/server now probably needs to be renamed 'transport' eg. client_transport / server_transport -- the things below them that we called transport before, is probably transport drivers.' Then: 'the thing I think is clear, both space and rocketwerkz are going to ..."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #77, opened 2026-08-28 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "77" :labels ""))
  (item "issue-81" :group "github-issues" :area "rocketnet"
   :title "Runtime transport drivers, per platform: the matrix that proves the abstraction"
   :text "Glenn's roadmap, 2026-08-28 live, verbatim across seven beats: 'And then once this is done I want to start implementing different runtime transport drivers per-platform. In particular, we can start with Linux."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #81, opened 2026-08-28 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "81" :labels ""))
  (item "issue-86" :group "github-issues" :area "rocketnet"
   :title "Messages at the high level: reliability policies, batching, small-packet assembly"
   :text "Tier 1 (#77), sequenced AFTER the driver matrix (#81). Filed so the board carries the whole program. Built ON the transport's delivery notification primitive (#84): reliable-ordered messages and large blocks (the Step 3 references are reliable, yojimbo, and space's network_message_sender/receiver -- Glenn's own implementation loosely based on them and ..."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #86, opened 2026-08-28 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "86" :labels ""))
  (item "issue-88" :group "github-issues" :area "rocketnet"
   :title "The transport program: the plan, in order, self-sufficient"
   :text "'basically, our immediate term goal is to pull the useful things out from space game into rocketnet at low level transport and client/server message level, while retaining IO throughput and zero copy and lock free where we can, and then bring back into space game to prove it works.' / 'space game then soaks it.'"
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #88, opened 2026-08-28 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "88" :labels ""))
  (item "issue-96" :group "github-issues" :area "rocketnet"
   :title "The GameLift player gateway path: DDoS protection as a transport driver layer"
   :text "Glenn's direction, 2026-08-29 live, verbatim across three beats: 'We will want to incorporate player gateway into our transport when we are on GameLift. In fact, it might even be a specific gamelift transport driver.' / 'Integrate with all the DDoS protection and player gate etc.' / 'This should be a specific transport driver."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #96, opened 2026-08-29 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "96" :labels ""))
  (item "issue-122" :group "github-issues" :area "rocketnet"
   :title "Unreproduced non-zero exit from the portable-pinned suite, evidence lost to a redirect"
   :text "While verifying PR #119 (the #109 port-zero rule), one run of the portable-pinned suite exited non-zero and then would not reproduce. Recorded here rather than explained away: an unexplained red that does not reproduce is data, and a second instance is only a pattern if the first one was written down somewhere a later session will look."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #122, opened 2026-08-30 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "122" :labels ""))
  (item "issue-124" :group "github-issues" :area "rocketnet"
   :title "The nightly half of #111: the checks whose value comes from time, and the alarm that makes them real"
   :text "The second half of #111, specified here and deliberately unbuilt, so the per-commit gate could land on its own and be measured on its own. The split criterion is unchanged and it is DURATION-DEPENDENCE: the per-commit gate holds everything that gates and fits in minutes, and this issue holds the checks whose value comes from time."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #124, opened 2026-08-30 by rowan-claude, labels enhancement; moved here 2026-10-14"
   :kept (:github-issue "124" :labels "enhancement"))
  (item "issue-129" :group "github-issues" :area "rocketnet"
   :title "The big-endian leg cannot run the socket-crossing tests under qemu-user: what it would take to close that gap"
   :text "The #111 big-endian leg cross-compiles the whole sdk for s390x and runs its byte-order surface under qemu-user. What it does not run is the part of the suite that crosses a real socket, and this issue records why, so nobody later mistakes the gap for an oversight or quietly widens the leg into reporting on the emulator."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #129, opened 2026-08-30 by rowan-claude, labels enhancement; moved here 2026-10-14"
   :kept (:github-issue "129" :labels "enhancement"))
  (item "issue-143" :group "github-issues" :area "rocketnet"
   :title "Sol 5.6 Ultra's engineering and source audit of RocketNet at f486e0a: seven release blockers, seven high-priority findings, the build and API work, and the 1.0 gates — to work on later"
   :text "Glenn commissioned an engineering and source audit of RocketNet from Sol 5.6 Ultra (audit date 2026-09-04, audited revision f486e0a; main is one commit later, #142, which touches only the test suite's logging). Filed here verbatim, at his word, to work on later."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #143, opened 2026-09-04 by gafferongames; moved here 2026-10-14"
   :kept (:github-issue "143" :labels ""))
  (item "issue-149" :group "github-issues" :area "rocketnet"
   :title "B6: the public ABI cannot evolve — no struct_size or version on config and stats structs, create copies the whole compiled struct, and C# mirrors 189 counters by hand"
   :text "From #143, verified. No struct_size, version or abi_version anywhere in sdk/rocketnet.h. Config tables at rocketnet.h:496-500 and :681-694 carry no size field (structs at :520-524, :696-699); create copies the whole compiled struct (effective_config = *config at client_transport.cpp:163 and server_transport.cpp:465); config.cpp:31-42 memsets the caller's ..."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #149, opened 2026-09-04 by gafferongames; moved here 2026-10-14"
   :kept (:github-issue "149" :labels ""))
  (item "issue-155" :group "github-issues" :area "rocketnet"
   :title "H5: legal configurations multiply independently bounded fields to 146 GiB, and the default XDP geometry allocates 3.5 GiB before the server exists"
   :text "From #143, verified from the constants (the audit's 'hundreds of GB' is roughly 2x overstated; the class is real). From the config table (rocketnet.h:681-694): max_clients ≤ 1000, num_reassembly_slots ≤ ROCKETNET_MAX_FRAGMENTS = 256, max_payload_bytes ≤ 256 × 1193 = 305,408."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #155, opened 2026-09-04 by gafferongames; moved here 2026-10-14"
   :kept (:github-issue "155" :labels ""))
  (item "issue-157" :group "github-issues" :area "rocketnet"
   :title "H7: no structured errors, a variadic logging callback with no user context, and counter getters that return an incomplete type mirrored by hand in C#"
   :text "From #143, verified. No error enum, last_error or status out-param across the 93 ROCKETNET_EXPORT_FUNC declarations in sdk/rocketnet.h: init returns bool (:244), creates return NULL (:534, :705), the reason exists only as a log line. rocketnet.h:295 rocketnet_log_function( void (*)( int level, const char * format, ..."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #157, opened 2026-09-04 by gafferongames; moved here 2026-10-14"
   :kept (:github-issue "157" :labels ""))
  (item "issue-167" :group "github-issues" :area "rocketnet"
   :title "Measure Windows IOCP line rate, receive capacity and socket contention"
   :text "IOCP is implemented in #161 and hardened through #165. Hosted MSVC Debug/Release, loopback, completion lifetime and shutdown tests do not establish NIC throughput. Glenn requires at least one performant line-rate driver on Windows, Mac and Linux, without a required frame, send or ACK cadence."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #167, opened 2026-09-08 by gafferongames; moved here 2026-10-14"
   :kept (:github-issue "167" :labels ""))
  (item "issue-168" :group "github-issues" :area "rocketnet"
   :title "Validate the repaired XDP handoff and partial construction on real hardware"
   :text "Host-side tests preserve a 100-packet tail after only 32 are copied, exercise pressure/wrap/reuse, and reject a premature-recycle plant. Linux CI compiles the actual embedded BPF source without loading it. These are not AF_XDP device tests."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #168, opened 2026-09-08 by gafferongames; moved here 2026-10-14"
   :kept (:github-issue "168" :labels ""))
  (item "issue-171" :group "github-issues" :area "rocketnet"
   :title "Confirm commercial SDK build and dependency compatibility"
   :text "Validate the intended Space Game/consumer builds through the installed and embedded front doors, including supported Windows configurations/toolchains and shared-library deployment. Confirm whether the 3.20 floor and prefix policy are acceptable for those consumers; change them only for a concrete need."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #171, opened 2026-09-08 by gafferongames; moved here 2026-10-14"
   :kept (:github-issue "171" :labels ""))
  (item "issue-180" :group "github-issues" :area "rocketnet"
   :title "Full transport load testing after feature completion, hardening and IPv6"
   :text "Run a full synthetic-load and physical-NIC validation campaign after the transport feature set is complete and hardened. This is a separate phase from the current implementation-quality pass. Glenn clarified the order live on September 8: “We are not going to do any targeted re-runs.” “We can have a load testing issue ahead of us. Write this info into that."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #180, opened 2026-09-08 by gafferongames; moved here 2026-10-14"
   :kept (:github-issue "180" :labels ""))
  (item "issue-189" :group "github-issues" :area "rocketnet"
   :title "Replay window: a long skip clears only 4 of the window's 64 words, so stale bits refuse genuine packets and the ack block claims packets that never arrived"
   :text "Found by a cold read of the tree, verified at main@dd2ee52df1342bf78f8b85c1ae887e8452fa5b40. Reported, not fixed. rocketnet_replay_advance takes a fast path when a sequence jumps a full window or more, and that path clears four words of the replay window:"
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #189, opened 2026-09-19 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "189" :labels ""))
  (item "issue-190" :group "github-issues" :area "rocketnet"
   :title "Packet loss tracker: the fast-path guard is off by one against its own ring, and a reordered packet rewinds the sweep and re-counts loss"
   :text "Found by a cold read of the tree, verified at main@dd2ee52df1342bf78f8b85c1ae887e8452fa5b40. Reported, not fixed. Both defects are in sdk/rocketnet_packet_loss_tracker.h, both are one-line, and they compound: the first inflates a loss count once, the second makes it repeat."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #190, opened 2026-09-19 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "190" :labels ""))
  (item "issue-191" :group "github-issues" :area "rocketnet"
   :title "Five more sites of the #151/#152 class: a harness setup failure is ignored and the run then reports a result that measured nothing"
   :text "Found by a cold read of the tree, verified at main@dd2ee52df1342bf78f8b85c1ae887e8452fa5b40. Reported, not fixed. #151 ('the real UDP soak returns success when client creation fails before testing anything') and #152 ('the benchmark's negative controls log and continue') named this class and both are closed."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #191, opened 2026-09-19 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "191" :labels ""))
  (item "issue-192" :group "github-issues" :area "rocketnet"
   :title "bench: three refusal paths release nothing — probe_reuseport leaks bound SO_REUSEPORT sockets, probe_kqueue leaks three fds, lease_cost skips its own leak oracle"
   :text "Found by a cold read of the tree, verified at main@dd2ee52df1342bf78f8b85c1ae887e8452fa5b40. Reported, not fixed. Three refusal paths release nothing, and in each case the success path in the *same function* shows exactly what should be released. #156 ('H6 ..."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #192, opened 2026-09-19 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "192" :labels ""))
  (item "issue-193" :group "github-issues" :area "rocketnet"
   :title "Build: server_xdp/Makefile carries two dead include paths (one naming a libbpf the repo does not vendor), and ci.yml's fuzz-smoke writes into bin/ without creating it"
   :text "Found by a cold read of the tree, verified at main@dd2ee52df1342bf78f8b85c1ae887e8452fa5b40. Reported, not fixed. All three are one-token edits in the build. server_xdp/Makefile:3 -Ilibbpf/src points at a directory that exists nowhere: find ."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #193, opened 2026-09-19 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "193" :labels ""))
  (item "issue-194" :group "github-issues" :area "rocketnet"
   :title "Four residuals: a snprintf error message that names the wrong condition, a dead base64 branch, bench_nw's hardcoded syscalls=0 beside a dead counter, and soak_sim's unvalidated iteration count"
   :text "Found by a cold read of the tree, verified at main@dd2ee52df1342bf78f8b85c1ae887e8452fa5b40. Reported, not fixed. Four small ones, grouped because each is a line and none is urgent. Two are in sdk/, two in the harnesses."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #194, opened 2026-09-19 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "194" :labels ""))
  (item "issue-195" :group "github-issues" :area "rocketnet"
   :title "The suite's own rule is broken in four of five places: 11 tests vanish on macOS with no [SKIPPED] line (153 registered, 142 printed), a skip names a test that does not exist, and a CI comment says 'exactly two'"
   :text "Found by a cold read of the tree, verified at main@dd2ee52df1342bf78f8b85c1ae887e8452fa5b40, and confirmed by running the suite. Reported, not fixed. The suite states the rule in its own words at sdk/rocketnet_tests.cpp:12409-12411 — *'a skipped test is named and logged, never silent'* — and honours it in exactly one place out of five."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #195, opened 2026-09-19 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "195" :labels ""))
  (item "issue-196" :group "github-issues" :area "rocketnet"
   :title "Doc drift, four places: STANDARD.md's normative auto table omits Windows iocp, CLAUDE.md/TODO.md still call sodium an external package, PROTOCOL.md names a plant run_all.sh does not run"
   :text "Found by a cold read of the tree, verified at main@dd2ee52df1342bf78f8b85c1ae887e8452fa5b40. Reported, not fixed. In each case the tree is right and the prose is stale; a second document in the same repo already states the correct version, which is what makes these mechanical."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #196, opened 2026-09-19 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "196" :labels ""))
  (item "issue-197" :group "github-issues" :area "rocketnet"
   :title "Cookie key rotation: STANDARD.md tables a 60 s period and CLAUDE.md says it runs, but the key is written once at create and has_previous_cookie_key is set only by the test suite — needs a ruling"
   :text "Found by a cold read of the tree, verified at main@dd2ee52df1342bf78f8b85c1ae887e8452fa5b40. Reported as a question, not as a repair — which of the two answers is right is a design call, and rocketnet's design is Glenn's."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #197, opened 2026-09-19 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "197" :labels ""))
  (item "issue-198" :group "github-issues" :area "rocketnet"
   :title "csharp: rocketnet_get_log_level and rocketnet_log_level_string are exported and listed in exports.txt but unbound, so a managed caller cannot restore a log level it changed"
   :text "Found by a cold read of the tree, verified at main@dd2ee52df1342bf78f8b85c1ae887e8452fa5b40. Reported, not fixed. Two exported log functions have no C# binding, and they are not covered by the binding's own list of deliberate exclusions."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #198, opened 2026-09-19 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "198" :labels ""))
  (item "issue-201" :group "github-issues" :area "rocketnet"
   :title "Windows unit workers abort on every test since the suite runner landed: Release exits 0xC0000409, Debug hangs on the CRT dialog until the 120 s kill, nightly red four nights"
   :text "The nightly on main has been red four nights running, 2026-10-06 through 2026-10-09, in the windows-2025 Debug leg, and the push CI on main has failed or been cancelled since the same landing. Nothing alarmed; the first notice was the 10-09 run-failed mail."
   :date "2026-10-14"
   :origin "mas-bandwidth/rocketnet issue #201, opened 2026-10-09 by rowan-claude; moved here 2026-10-14"
   :kept (:github-issue "201" :labels ""))
 ))
