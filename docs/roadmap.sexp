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
  (group "finish" :title "Started work to finish and land"
   :text "Work begun on 2026-10-31 and stopped when the landing froze. Each names its branch and exactly what is owed before it lands.")
  (group "audit-remaining" :title "Audit items not yet fixed"
   :text "Suspects from FABLE-AUDIT.md that nothing on main fixes. Each is verified at the cited line first; a confirmed defect gets a lock-in test and a negative control.")
  (group "quiet-host" :title "Investigations that need a quiet host"
   :text "Findings that only a quiet fleet host can measure or reproduce.")
  (group "older-prs" :title "Older open pull requests"
   :text "Pull requests not in the 2026-10-31 landing. Each is merged if it still applies, or closed naming the change that replaced it.")
  (group "decisions" :title "Decisions for Glenn"
   :text "Choices that change behavior, remove machinery, or touch repository and fleet settings.")
 )

 :items
 (
  (item "landing-known-reds" :group "finish" :area "rocketnet"
   :title "Known reds at the 2026-10-31 landing"
   :text "Merged to main on Glenn's word with the nightly to catch the rest. Red on the fleet at ab74c810: (1) DEV=OFF unit test_message_required_memory_takes_the_create_budget, the client never reaches stopped in unit_shutdown.h:24 (from rowan/low-channel-message); (2) check_tla_coverage --actions-strict: RNGsoHandoff TxSendFull and RNOrderingWrap HoldSequenced, ReceiveUnreliable are cited nowhere in the code; (3) functional test_gso_transport_posix_server_create_destroy_1000 past its 120 s bound on loaded vision, host load not confirmed; (4) Windows Debug never finished (CreateProcessW 1455, paging file). Not run before the merge: the GitHub CI run, Linux DEV=OFF functional beyond these, Windows Debug."
   :date "2026-10-31" :origin "Integration fleet run, ab74c810")

  (item "remove-type9" :group "finish" :area "rocketnet"
   :title "Remove transport type-9 fragmentation"
   :text "Glenn ruled remove (audit 13). Branch rowan/remove-type9-fragmentation at 0689412d: unreachability confirmed; the code, config field, status code, 41 counters, standard sections and the model's fragment half are gone; server required_memory at 1000 clients drops 379 MB and the client's from 29.3 MB to 20.3 MB. Owed: a full fleet run at the head; five one-line negative controls; timing the split lock-in sweep under the sanitizers; a Windows light-profile ceiling measurement; the fragment text left in proofs/tla/Link.tla (editing it pushes MCRNChannel past the 110 s TLC budget). Rebase onto main (conflicts with the counter renumbering)."
   :date "2026-10-31" :origin "Audit item 13; Glenn, 2026-10-31")

  (item "resend-seeded-rtt" :group "finish" :area "rocketnet"
   :title "Seed the message resend delay from the measured RTT"
   :text "The two channels nightly seeds that fail every run (4879047, 4879943) time out because the message layer waits its 2000 ms default while the transport has already measured RTT. Branch rowan/resend-seeded-rtt at 511ead80: works on macOS, TLC and standard m9.2 updated. Owed: diagnose the Linux test_light_profile_server_1000_required_memory failure; the full nightly seed range; re-measure the ordered stream's completion multiple; squash the WIP commits."
   :date "2026-10-31" :origin "Red channels nightly")

  (item "size-by-config" :group "finish" :area "rocketnet"
   :title "Size per-server arrays from the configured max_clients"
   :text "In a ROCKETNET_MAX_CLIENTS=10000 build a 1000-slot server plans 3.93 GB because per-server arrays use the build constant (audit 39). Branch rowan/size-by-config; memory is the user's choice via config."
   :date "2026-10-31" :origin "Audit item 39")

  (item "macos-net-thread-priority" :group "finish" :area "rocketnet"
   :title "macOS network thread just above default priority"
   :text "Since #624 the macOS network thread runs at SCHED_RR maximum and outranks the game thread. Branch rowan/macos-net-thread-priority at 1e9ecedf (priority 32): msgx P=32 p99 43.3 to 19.6 us on mini-m5. Owed: 200+ single-cell closed_port runs and a contention A/B against 47 to show #624's fix holds. If it does not hold under contention, Glenn decides between network latency and game-thread p99."
   :date "2026-10-31" :origin "Frame budget bisect")

  (item "gate-nightly-proofs-split" :group "finish" :area "ci"
   :title "Nightly proofs split so the job can finish"
   :text "Audit 33. Branch rowan/gate-nightly-proofs-split at 3522e523: one job per reader and per partition. The planted-failure control works on vision. Owed: the rewritten workflow has never run, and the held partition needs its full 1800 s."
   :date "2026-10-31" :origin "Audit item 33")

  (item "model-clientkeys" :group "finish" :area "proofs"
   :title "ClientConnection KeysErasedWhenDown matches the code"
   :text "Audit 66. Branch rowan/model-clientkeys at 5bbf6e7a: the model is done. Owed: the clientconnection group's RUNS.tsv on a quiet, fast Linux host (MCClientConnectionReachSFirstkind runs 117-132 s on captain against the 110 s budget, 50 s on Linux)."
   :date "2026-10-31" :origin "Audit item 66")

  (item "request-one-hash" :group "finish" :area "rocketnet"
   :title "One address hash per connection request"
   :text "Audit 97. Branch rowan/sdkfu-request-one-hash at d3c2edfb: the resend shape is 15% faster (120.6 to 102.8 ns per request on intel). Owed: a macOS unit run; put the numbers in the commit."
   :date "2026-10-31" :origin "Audit item 97")

  (item "live-matrix-flake" :group "finish" :area "tests"
   :title "Live message matrix: a late probe copy counts as its probe"
   :text "Audit 59. Branch rowan/low-matrix-flake at a1693544, never built. Owed: functional build and repeated runs on Linux and macOS, a negative control, and a reproduction on main with injected latency."
   :date "2026-10-31" :origin "Audit item 59")

  (item "resend-generation-stamp" :group "finish" :area "rocketnet"
   :title "Remove the resend FIFO's generation stamp"
   :text "RNResendGenerations shows the stamp never decides; the number comparison refuses a stale ack. Removing it shrinks each resend entry from 12 to 8 bytes. Uncommitted edits in the worktree rocketnet-remove-stamp, branch rowan/remove-resend-generation-stamp."
   :date "2026-10-31" :origin "Audit item 12 follow-on")

  (item "cbmc-lows-797" :group "finish" :area "proofs"
   :title "Land #797: CBMC low items"
   :text "Audit 150-156, fleet-verified on vision (CBMC 6.6.0, ESBMC 8.5.0): esbmc_r_ok runs nightly; the message-reader harnesses cover every accepted limit; the 24-bit window arithmetic moves into rocketnet_message_u24_delta and is proved; the fragment-bounds proof and dead fallbacks are deleted. 150 is refuted: ack-run-bound-8 runs out of memory at 48 GB, so a working ACK proof needs the harness redesigned. Stacked on #782 and #754; left out of the 2026-10-31 landing so the fleet run did not restart."
   :date "2026-10-31" :origin "FABLE-AUDIT.md low proofs items")

  (item "ack-proof-nightly-studio" :group "finish" :area "proofs"
   :title "Run the ack-run-bound-8 CBMC proof nightly on studio"
   :text "Audit 150. The proof and its negative control exhaust 48 GB on vision; studio has 512 GB. Make it a nightly job on studio (pinned CBMC built from source under the project, no system binary), not a per-commit gate. It lands with #797."
   :date "2026-10-31" :origin "Glenn, live, 2026-10-31: run as nightly")

  (item "proofs-lows" :group "finish" :area "proofs"
   :title "Low-severity proofs items"
   :text "Not started, by family. Coverage and citations: 89, 109, 233, 234 (rio row, and the check_tla_coverage.py lock-in comparing each row's lists with its cfgs and CASES.tsv), 235, 243, 251, 255, 264, 286. Session and protocol docs: 92, 254, 268, 285, 288-290. Handoff layouts: 232, 236, 237, 239, 241, 256. Rio and IndexRing: 242, 244, 248, 262. Update and ownership: 253, 257, 260, 265, 266. Channel and pools: 99, 249, 259, 263, 267, 291. Code gaps: 250 (iocp count load order), 258 and 269 (dedupe pending XDP forgets), each with a unit lock-in. Lows on models other work owns: 238, 240, 245-247, 252, 261, 293. 292 rides #729, 287 #721."
   :date "2026-10-31" :origin "FABLE-AUDIT.md")

  (item "client-keepalive-unsigned" :group "audit-remaining" :area "rocketnet"
   :title "Keep-alive slot count compared unsigned"
   :text "rocketnet_client_validate_keep_alive casts the u32 slot count to int, so 2^31 or more is refused, against the ruling that the client accepts any count with index below it. Unit cases at 2^31 and UINT32_MAX, and a negative control."
   :date "2026-10-31" :origin "#730 follow-on")

  (item "test-737-second-address" :group "audit-remaining" :area "tests"
   :title "A create test that fails on second_public_address alone"
   :text "The whole-config create test goes red under the old truncation only because loopback_hub is also cut off."
   :date "2026-10-31" :origin "#737 follow-on")

  (item "stale-drain-text" :group "audit-remaining" :area "docs"
   :title "Drain limitation text after #740"
   :text "docs/CLIENT-IO.md and TRANSPORT-INTERNALS.md 'current staging and drain limitations' still describe the drain bound #740 fixed."
   :date "2026-10-31" :origin "#740 follow-on")

  (item "max-channels-cmake" :group "audit-remaining" :area "build"
   :title "ROCKETNET_MAX_CHANNELS CMake option"
   :text "Pass it through as ROCKETNET_MAX_CLIENTS is, document it, and give it a CI build."
   :date "2026-10-31" :origin "Audit item 171 follow-on")

  (item "portable-sendmmsg-carry" :group "audit-remaining" :area "rocketnet"
   :title "Portable sendmmsg keeps the unsent rest of a batch"
   :text "On a full socket the portable sendmmsg path drops the rest of the batch; GSO now keeps it (audit 111). Same rule: spread across updates, never drop."
   :date "2026-10-31" :origin "Audit item 111 follow-on")

  (item "uring-two-phase-send" :group "audit-remaining" :area "rocketnet"
   :title "Two-phase io_uring send staging"
   :text "When more than 10240 completions arrive ahead of its sends, the io_uring send index still ratchets and outbound runs about one update in three under a flood. Give send staging commit and complete phases as RIO has."
   :date "2026-10-31" :origin "#738 follow-on")

  (item "message-prefix-dedupe" :group "audit-remaining" :area "rocketnet"
   :title "A duplicated deferred packet does not redeliver its code-0 prefix"
   :text "Audit 101 remainder, MESSAGES.md m1.4 rule 2. Recommended: remember the deferred packet and how many records were processed, so a duplicate skips them."
   :date "2026-10-31" :origin "Audit item 101")

  (item "windows-reflush" :group "audit-remaining" :area "rocketnet"
   :title "Windows iocp and rio re-flush"
   :text "Audit 88 and 124: measure on captain, then add re-flush counters if the measurement supports it."
   :date "2026-10-31" :origin "Audit items 88, 124")

  (item "stop-send-failure-assert" :group "audit-remaining" :area "tests"
   :title "Server-stop functional case asserts no send failures"
   :text "The Windows cut-off of a flushed bank at stop was never reproduced on captain. Asserting the send-failure counter does not move during stop gives the check a way to fail; it needs a small test accessor for a stopped server's transport stats."
   :date "2026-10-31" :origin "Stop-spread follow-on")

  (item "load-md-generated" :group "audit-remaining" :area "docs"
   :title "Generate LOAD.md's byte tables"
   :text "The page's exact byte figures go stale with every merge. Generate them, with a CI check that the page matches the generator."
   :date "2026-10-31" :origin "#721 follow-on")

  (item "release-requires-nightly" :group "audit-remaining" :area "ci"
   :title "A release commit requires a green Nightly CI run"
   :text "docs/RELEASING.md asks for it; nothing enforces it. Extend tools/check_release_tsan.py to require nightly-ci.yml, msan and CBMC green at the tagged commit."
   :date "2026-10-31" :origin "CI under two minutes")

  (item "low-docs-standard-tests" :group "audit-remaining" :area "rocketnet"
   :title "Low-severity docs, standard, tests and CI items"
   :text "The low items under docs/, standard/, tests/, tools/, .github/, scripts/, CMakeLists.txt or sodium/ whose class is not defect: 94 95 96 108 120 127 132 138 140 141 143 144 146 147 148 149 157 158 159 160 161 167 177-185 198 200 201 202 217-227 230 231 284."
   :date "2026-10-31" :origin "FABLE-AUDIT.md")

  (item "xdp-receive-loss" :group "quiet-host" :area "rocketnet"
   :title "About 0.3% of client-to-server packets lost in the XDP receive path"
   :text "Seen once the 1000-client xdp soak cell runs, with the host at load 170. Reproduce on a quiet host, then fix the cause."
   :date "2026-10-31" :origin "Audit item 41")

  (item "low-tick-nightly" :group "quiet-host" :area "rocketnet"
   :title "Low-tick nightly ends at 996 of 1000 connected"
   :text "Reproduce on an idle Linux host with the per-client state output now in the nightly."
   :date "2026-10-31" :origin "Audit item 42")

  (item "frame-budget-quiet" :group "quiet-host" :area "ci"
   :title "Confirm the frame-budget fix on amd-2/amd-3"
   :text "The ack statistics fix brings P=32 p99 from 212 to 59 us on hulk; confirm on the quiet runner through the nightly frame budget."
   :date "2026-10-31" :origin "Frame budget bisect")

  (item "older-prs-triage" :group "older-prs" :area "rocketnet"
   :title "Merge or close the pull requests outside the landing"
   :text "#711 and #729 (securing passes for layers 7 and 6, both conflicting; #729 carries audit 85, 86, 113, 122, 125, 203-205, 278, 280, 282, and rowan/sdkfu-loopback-send-count for audit 119 is built on it), #714 to #720, #723 to #725, #727, #618, #619, #592, #564, #558, #418, #402, #380."
   :date "2026-10-31" :origin "Glenn, 2026-10-31")

  (item "issue-122" :group "older-prs" :area "github"
   :title "Close issue #122 with the evidence"
   :text "176 runs on mini-m5 under load 35-100 did not reproduce it."
   :date "2026-10-31" :origin "Audit item 199")

  (item "decision-resend-cap" :group "decisions" :area "rocketnet"
   :title "Cap the single-packet resend delay at timeout/4"
   :text "Audit 16: with message_resend_delay_max_ms above reliable_message_timeout_ms/4, a single-packet record gets fewer than four sends before timeout. Recommended: cap it as chunks are. Then both resend classes share one delay, and the two resend queues could collapse to one; that removes machinery."
   :date "2026-10-31" :origin "Audit item 16")

  (item "decision-type9-wire-version" :group "decisions" :area "rocketnet"
   :title "Wire version after removing type 9"
   :text "The removal leaves ROCKETNET_WIRE_VERSION unchanged because no surviving packet changes a byte."
   :date "2026-10-31" :origin "Audit item 13")

  (item "decision-unit-env" :group "decisions" :area "tests"
   :title "Unit tests and the process environment"
   :text "Unit cases still reach the environment through the public creates, safe only because the runner clears every name the SDK reads. Stricter: route about 320 creates through the injected-environment variants. Recommended: keep the runner boundary. Also move the XDP gateway address read from socket open to create."
   :date "2026-10-31" :origin "#794")

  (item "branch-protection" :group "decisions" :area "github"
   :title "Branch protection on main"
   :text "No ruleset requires any check on main (audit 44)."
   :date "2026-10-31" :origin "Audit item 44")

  (item "frame-budget-runner-label" :group "decisions" :area "fleet"
   :title "A runner label for the nightly frame budget held only by amd-2 and amd-3"
   :text "rocketnet-quiet also covers hetzner2; the frame budget is a wall-clock gate that compares only on one machine class."
   :date "2026-10-31" :origin "#731")
 ))
