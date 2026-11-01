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

  (group "older-prs" :title "Older open pull requests"
   :text "Pull requests not in the 2026-10-31 landing. Each is merged if it still applies, or closed naming the change that replaced it.")
  (group "decisions" :title "Decisions for Glenn"
   :text "Choices that change behavior, remove machinery, or touch repository and fleet settings.")
 )

 :items
 (




















  (item "tlc-timeout-debt" :group "proof-debt" :area "proofs"
   :title "Ten recorded TLC benchmark timeouts"
   :text "The committed RUNS.tsv still records FAIL/124 at the existing 110-second limit for ClientConnection LArbitrary, LCrash, SNone, SAlternate, SBurst and SArbitrary; ReplayWindowMid; RNEndpointTwoWay and RNEndpointPartial; and SessionAcksLive. The existing TLC nightly retries each declared timeout-debt case and every stale-group case, once each, without rerunning healthy siblings solely because another case timed out. The nightly now runs without a project elapsed-time cap and preserves separate JSON evidence through the pinned engine; the bounded committed measurements remain unchanged. Failure, cancellation or resource exhaustion is not a proof. Fresh measured records require a reviewed commit; the workflow does not commit them automatically."
   :date "2026-11-01" :origin "proofs/tla/CASES.tsv and RUNS.tsv; existing TLC nightly; #833")

  (item "ack-proof-hold" :group "proof-debt" :area "proofs"
   :title "ACK run proof remains HOLD"
   :text "The source-bound ACK run-eight proof and its negative control have no completed accepted result; the full retained-window proof is also not established. #797 landed with this HOLD explicitly retained. The existing Nightly ACK proof runs ack-run-bound-8 and ack-run-negative-control serially with pinned ESBMC on Studio, at 384 GiB per target without a project elapsed-time cap. GitHub still imposes its five-day self-hosted job ceiling. It requires VERIFICATION SUCCESSFUL and the intended EXPECTED FAILURE; memory exhaustion, timeout, UNKNOWN or an unwinding failure stays red. The full-4096 target is declared separately and is not run by that nightly. Do not infer a proof from observed loop cutoffs or make successful completion a new per-commit landing requirement."
   :date "2026-11-01" :origin "proofs/cbmc/INVENTORY.md; .github/workflows/nightly-ack-proof.yml; #797")

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



  (item "older-prs-triage" :group "older-prs" :area "rocketnet"
   :title "Merge or close the pull requests outside the landing"
   :text "Open outside the audit continuation as of 2026-11-01: #716, #717, #718, #719, #720, #727, #618, #619, #592, #564, #558, #418, #402, #380. #711, #724 and #729 have landed; the other closed entries are no longer pending. Triage these separately against current source."
   :date "2026-10-31" :origin "Glenn, 2026-10-31")

  (item "issue-122" :group "older-prs" :area "github"
   :title "Close issue #122 with the evidence"
   :text "176 runs on mini-m5 under load 35-100 did not reproduce it."
   :date "2026-10-31" :origin "Audit item 199")

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

  (item "frame-budget-runner-label" :group "decisions" :area "fleet"
   :title "A runner label for the nightly frame budget held only by amd-2 and amd-3"
   :text "rocketnet-quiet also covers hetzner2; the frame budget is a wall-clock gate that compares only on one machine class."
   :date "2026-10-31" :origin "#731")
 ))
