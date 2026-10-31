; The RocketNet roadmap: the data ROADMAP.md is generated from.
; Same shape as nova-sprint's docs/roadmap.sexp: data, never evaluated. Edit this file, then
; regenerate ROADMAP.md with nova-sprint's generator (a go tool dependency), from a nova-sprint
; checkout:
;   go tool roadmap --file <this repo>/docs/roadmap.sexp --out <this repo>/ROADMAP.md
; Never edit ROADMAP.md by hand.
(roadmap "v1"
 :title "RocketNet roadmap"
 :text "Planned work for RocketNet that is not yet on main. Work lands on main before new work starts (Glenn, 2026-10-31). Each item names where it comes from: an audit item in FABLE-AUDIT.md (the 12:05 PDT edition of 2026-10-31, numbered as there), an open pull request, or a ruling. Landed work is in git."

 :groups
 (
  (group "landing" :title "Being landed now"
   :text "The integration branch rowan/integration holds today's audit fixes: 60 pull requests and the plain branches below, verified on the fleet, then one CI run, then one merge to main. The items in this group are the branches that finish after that set was frozen; they land as the second batch, before any item in a later group starts.")
  (group "ci" :title "CI"
   :text "Per-commit CI finishes within two minutes of a push, one minute ideally; everything that does not fit runs nightly with no time limit (Glenn, 2026-10-31).")
  (group "audit-remaining" :title "Audit items not yet fixed"
   :text "Suspects from FABLE-AUDIT.md that no landed or in-flight change fixes yet. Each is verified at the cited line before it is fixed, and a confirmed defect gets a lock-in test and a negative control.")
  (group "investigations" :title "Investigations that need a quiet host"
   :text "Findings that only a quiet fleet host can measure or reproduce. They run after the landing frees the fleet.")
  (group "older-prs" :title "Older open pull requests"
   :text "Pull requests opened before 2026-10-31. Each is merged if it still applies after today's landing, or closed with a pointer to the change that replaced it.")
  (group "decisions" :title "Decisions for Glenn"
   :text "Repository settings and fleet configuration that only Glenn changes.")
 )

 :items
 (
  (item "second-batch-branches" :group "landing" :area "rocketnet"
   :title "Second batch: branches finishing after the freeze"
   :text "Fixes still in flight on plain branches, each verified on the fleet, landed through the next integration branch: server stop's last flush confirmed complete before the I/O stops (rowan/stop-burst-across-updates, Windows follow-on); per-server arrays sized from the configured max_clients (rowan/size-by-config, audit 39); the message resend delay seeded from the measured RTT (rowan/resend-seeded-rtt, the two channels nightly seeds); the macOS network thread just above default priority (rowan/macos-net-thread-priority); the removal of transport type-9 fragmentation (rowan/remove-type9-fragmentation, audit 13); the gates that cannot fail (audit 32, 33, 56, 57, 60, and the unit environment passed in); the vacuous models (audit 15, 21, 22, 66, 68); the sdk follow-ups (audit 97, 111, 119, 129, 163, 88, 124, issue #122); the deferred findings (the #737 lock-in, the client create_ex status test and an every-test-runs gate, the unsigned keep-alive slot count, the channels record test's run time, CLIENT-IO.md's drain text, macOS check names, a ROCKETNET_MAX_CHANNELS CMake option); the client, channel and message low items (audit 101, 106, 110, 116, 117, 118, 123, 208, 210, 16, 17, 59); the proofs low items."
   :date "2026-10-31"
   :origin "Glenn, live, 2026-10-31: land what is in flight before new work")

  (item "ci-under-two-minutes" :group "ci" :area "ci"
   :title "Per-commit CI within two minutes; the rest nightly"
   :text "Move every per-commit job or step that does not finish well inside two minutes, queueing included, into a nightly workflow with no time limit, and enforce the budget per job. Measured on run 41231285519: the three Windows jobs took 2.5 to 3.7 minutes and the instruction count gate 7.1 minutes on a cold toolchain cache. Branch rowan/ci-under-two-minutes."
   :date "2026-10-31"
   :origin "Glenn, live, 2026-10-31: \"Each CI is 1m ideally, and 2m max\"")

  (item "resend-generation-stamp" :group "audit-remaining" :area "rocketnet"
   :title "Remove the resend FIFO's generation stamp and its check"
   :text "RNResendGenerations (PR #783) shows the stamp never decides: the number comparison in rn_window_ack_on_ack is what refuses a stale ack. Removing it shrinks each resend entry from 12 to 8 bytes and drops two branches per acked number. Started and held, uncommitted, in the worktree rocketnet-remove-stamp on branch rowan/remove-resend-generation-stamp."
   :date "2026-10-31"
   :origin "Audit item 12 follow-on; decided 2026-10-31")

  (item "low-docs-standard-tests" :group "audit-remaining" :area "rocketnet"
   :title "Low-severity docs, standard, tests and CI items"
   :text "The low items whose path is under docs/, standard/, tests/, tools/, .github/, scripts/, CMakeLists.txt or sodium/ and whose class is not defect: 94 95 96 108 120 127 132 138 140 141 143 144 146 147 148 149 157 158 159 160 161 167 177-185 196-198 200 201 202 217-227 230 231 284, less any a landed change already fixed (168-176 and 196-197 are fixed in the landing). Docs and the standard state the present truth; a test gap gets its test; a gate that cannot fail is made to fail."
   :date "2026-10-31"
   :origin "FABLE-AUDIT.md, 12:05 PDT edition")

  (item "xdp-receive-loss" :group "investigations" :area "rocketnet"
   :title "About 0.3% of client-to-server packets lost in the XDP receive path"
   :text "Once the 1000-client xdp soak cell runs (PR #775), counters_agree fails: about 0.3% of client-to-server packets are lost in the XDP receive path. Measured with the host at load 170, so first reproduce it on a quiet host, then fix the cause."
   :date "2026-10-31"
   :origin "Red-nightly work on audit item 41")

  (item "low-tick-nightly" :group "investigations" :area "rocketnet"
   :title "Low-tick nightly ends at 996 of 1000 connected"
   :text "Reproduce on an idle Linux host with the per-client state output from rowan/nightly-lowtick, find the cause, fix it."
   :date "2026-10-31"
   :origin "Red-nightly work on audit item 42")

  (item "older-prs-triage" :group "older-prs" :area "rocketnet"
   :title "Merge or close the pull requests opened before today"
   :text "#711 (secure layer 7, conflicting), #729 (securing pass layer 6, conflicting), #714 to #720 (CI and nightly additions), #723 to #725 and #727 (MSan fuzz pair, Windows functional build, case memory, fuzz oracles), #618 and #619 (stats aggregates, BSD platforms), #592, #564 and #558 (frame sync tests, closed-port counting), #418, #402 and #380 (CBMC and live matrix sprint branches). Several overlap today's fixes: each is rebased and landed if it still applies, or closed naming the change that replaced it."
   :date "2026-10-31"
   :origin "Glenn, live, 2026-10-31")

  (item "branch-protection" :group "decisions" :area "github"
   :title "Branch protection on main"
   :text "No ruleset or branch protection requires any check on main, so a red gate can be merged past (audit item 44). A repository setting, Glenn's to change."
   :date "2026-10-31"
   :origin "Audit item 44")

  (item "frame-budget-runner-label" :group "decisions" :area "fleet"
   :title "A runner label for the nightly frame budget held only by amd-2 and amd-3"
   :text "rocketnet-quiet now also covers hetzner2. Instruction counts do not depend on the host, but the frame budget is a wall-clock gate, so it compares only on one machine class. Fleet runner configuration, Glenn's to change."
   :date "2026-10-31"
   :origin "Instruction gate work, PR #731")
 ))
