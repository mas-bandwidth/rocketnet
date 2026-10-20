; RocketNet v1.0.0 testing plan. TESTING.md is rendered from this file by
; tools/roadmap/render.py (make testing); make testing-check fails when the
; committed page is not what this renders. This file is the whole plan: every
; test RocketNet will run before and after v1.0.0, including the hardening
; and long runs deferred until after the payment milestone (public API
; complete, unit and functional green), so none of them is lost.
;
; Five tables. "Tests per platform" holds the tests that apply to every
; platform, and "Platform-specific tests" each test that applies to only some,
; one row per platform it applies to. "Tests per transport" holds the tests
; that apply to every transport, and "Transport-specific tests" each test that
; applies to only some, one row per transport. "Release-wide tests" holds the
; tests that are neither per platform nor per transport (model checks, proofs,
; reviews). :all-cells-apply refuses a - cell, so no table shows a dash.
; :tables-only prints only the headings, a line of source links under each,
; and the tables. :rows-name-source makes every row carry :source, the file or
; card that owns the test; the :source, :evidence and :pending text stays here
; as the data behind each cell.
;
; Status, per cell:
;   y  ✅       green at sprint/rocketnet-proof: the landed card that ran it
;              (its id and land commit are in :evidence), or a CI run there.
;   n  ❌       a known red at this branch; :pending names the card that owns it.
;   o  OWED     a run the two-minute rule defers until every feature and bug
;              has landed (soak, load, repeat, 1000-client); neither a pass nor
;              a fail; :pending names the card that runs it.
;   p  PLANNED  no green run at this branch yet, not a known red and not
;              deferred: carded and in progress, carded and moved out of the
;              v1.0 sprint ("later"), or "no card".
; A transport that builds on more than one platform (portable, loopback) is y
; only when it is green on every platform it builds on.
;
; Card ids are written in full (rocketnet-...). The card's column (working,
; review, waiting) is as nova-sprint showed it when this file was recorded; a
; card shown with "-" is deferred out of the live queue.
;
; The per-commit CI (ci.yml), TLC records (tlc.yml) and msan.yml runs at this
; base were queued, not finished, when this was recorded (runs 39491507693,
; 39491507653 and 39491507654 at 443ea520); the last green TLC records run on
; this branch is 38950854663 at 0b1f91c4, which is not this base, so no model
; check is y here. The scheduled nightlies run only on main; a nightly cell
; comes from the cards that ran those jobs by hand.
;
; Recorded 2026-10-20 at sprint/rocketnet-proof 443ea5203c611a514eae014647ad93b74aa31724.
(:title "RocketNet v1.0.0 testing"
 :all-cells-apply t
 :tables-only t
 :rows-name-source t
 :owed "the run is longer than two minutes, and such runs (soak, load, repeat and 1000-client) wait until every feature and bug has landed. OWED is neither a pass nor a fail; the note names the card that runs it."
 :text ("Every test RocketNet plans to run for v1.0.0, by platform, by transport and release-wide. ✅ is green at the release branch by its landed card or by CI; ❌ is a known red; OWED is a long run deferred until every feature has landed; PLANNED has no green run yet. Each table's complete row is the share of ✅ among that table's own cells.")
 :areas
 ((:id "platforms" :title "Tests per platform"
   :text "The tests that apply to every platform: one row per test, one column per platform."
   :label "test"
   :columns ("Windows" "macOS" "Linux")
   :rows
   ((:feature "Unit suite (./bin/test)" :cells (y y y)
     :source "test.cpp and test_unit_*.cpp (./bin/test)"
     :evidence "Windows: test.exe Release green on the Windows member, rocketnet-driver-create-reports-out-of-memory b3162561; macOS and Linux: ./bin/test green Debug and Release, rocketnet-driver-create-reports-out-of-memory b3162561")
    (:feature "Functional suite (./bin/functional)" :cells (y y y)
     :source "functional/ (./bin/functional)"
     :evidence "Windows: functional.exe Release green, rocketnet-functional-builds-on-windows 85a98401, 10 of 10 runs in rocketnet-functional-tokens-use-the-test-clock d59e30de; macOS: ./bin/functional green, rocketnet-macos-client-timing-under-app-nap a2c4ac1f and rocketnet-macos-msgx-forced-portable-client-creates bb1521ab; Linux: ./bin/functional green Debug with the transport unset and pinned to portable, gso and io_uring, rocketnet-linux-pinned-portable-functional-green e73c2177")
    (:feature "Functional suite repeated, zero flakes" :cells (o o o)
     :source "functional/ (./bin/functional, repeated)"
     :pending ((Windows "rocketnet-windows-suite-stable (working): 20 of 20 runs")
               (macOS "rocketnet-macos-transport-matrix-repeat (waiting)")
               (Linux "rocketnet-linux-transport-matrix-repeat (waiting)")))
    (:feature "Transport matrix, every cell fifty times" :cells (o o o)
     :source "tools/check_transport_matrix.py; rocketnet-win-transport-matrix-repeat, rocketnet-macos-transport-matrix-repeat, rocketnet-linux-transport-matrix-repeat"
     :pending ((Windows "rocketnet-win-transport-matrix-repeat (waiting)")
               (macOS "rocketnet-macos-transport-matrix-repeat (waiting)")
               (Linux "rocketnet-linux-transport-matrix-repeat (waiting)")))
    (:feature "Sanitizers (ASan, UBSan, TSan; MSVC ASan on Windows)" :cells (y p o)
     :source "ci.yml sanitizer jobs; rocketnet-win-msvc-asan, rocketnet-macos-sanitizers-and-leaks"
     :evidence ((Windows "MSVC ASan, test.exe, test_lifecycle.exe, test_iocp.exe and functional.exe on the default transport, iocp and rio, zero reports, rocketnet-win-msvc-asan landed 20e2a9a4"))
     :pending ((macOS "rocketnet-macos-sanitizers-and-leaks (review)")
               (Linux "no card: the full functional ASan/UBSan and TSan matrices; ASan/UBSan unit and focused fragment tests are green, rocketnet-linux-sanitizers-green-at-base 1452058c")))
    (:feature "Leaks (LSan, the leaks tool, App Verifier's leak layer)" :cells (y p o)
     :source "nightly-leaks.yml, tools/lsan.supp; rocketnet-win-app-verifier, rocketnet-macos-sanitizers-and-leaks"
     :evidence ((Windows "App Verifier with the Leak layer, 0 stops on test_lifecycle, test_iocp and functional per transport, rocketnet-win-app-verifier 7457d35b"))
     :pending ((macOS "rocketnet-macos-sanitizers-and-leaks (review): the leaks tool")
               (Linux "no card: the full functional LSan matrix; the LSan unit suite is green, rocketnet-linux-sanitizers-green-at-base 1452058c")))
    (:feature "libFuzzer (corpus replay and fuzz runs; clang-cl on Windows)" :cells (p y y)
     :source "fuzz/, fuzz-nightly.yml; rocketnet-win-libfuzzer-clang-cl"
     :evidence "macOS: fuzz-macos run by hand green, rocketnet-macos-nightlies-green-at-base landed e7fba514; Linux: fuzz-smoke and fuzz-smoke-ndebug green, rocketnet-linux-ci-green-at-base 32c4ff08, and fuzz-nightly.yml's fuzz job green, rocketnet-linux-nightlies-green-at-base 381203ba"
     :pending ((Windows "rocketnet-win-libfuzzer-clang-cl (working)")))
    (:feature "Toolchain matrix (MSVC, clang-cl and ARM64 on Windows; Xcode universal arm64 and x86_64 on macOS; gcc and clang on Linux)" :cells (y y y)
     :source "CMakeLists.txt; rocketnet-win-toolchain-matrix, rocketnet-macos-universal-and-xcode-t, rocketnet-linux-toolchain-matrix"
     :evidence "Windows: clang-cl build passes test.exe and functional.exe, ARM64 cross build, rocketnet-win-toolchain-matrix 836cb427; macOS: both slices built, the x86_64 slice's suites under Rosetta 2, ./bin/test from the Xcode generator's build, rocketnet-macos-universal-and-xcode-t 879d31f4; Linux: gcc and clang, Debug and Release, ./bin/test and ./bin/functional, rocketnet-linux-toolchain-matrix landed b83cb5b8")
    (:feature "Lifecycle stress (init/term, create/destroy, restart, destroy under traffic)" :cells (p y y)
     :source "test_lifecycle.cpp, functional/posix_lifecycle_stress.cpp; rocketnet-win-lifecycle-stress"
     :evidence "macOS: descriptor and thread counts flat per case, rocketnet-macos-lifecycle-stress-t landed 2773d0da; Linux: every case 5 runs in a row, xdp in a guest, rocketnet-linux-lifecycle-stress-t 8a68c102"
     :pending ((Windows "rocketnet-win-lifecycle-stress (review)")))
    (:feature "Hard-killed peer never stalls receive" :cells (p y p)
     :source "functional/posix_hard_killed_peer.cpp; rocketnet-windows-udp-connreset-never-stalls-receive, rocketnet-linux-hard-killed-peer-never-stalls-receive"
     :evidence "macOS: a SIGKILLed client per transport, rocketnet-macos-hard-killed-peer-never-stalls-receive landed 443ea520"
     :pending ((Windows "rocketnet-windows-udp-connreset-never-stalls-receive (working)")
               (Linux "rocketnet-linux-hard-killed-peer-never-stalls-receive (working)")))
    (:feature "Socket error paths (oversize, runt, garbage, zero-length, failed sends)" :cells (p y y)
     :source "functional/posix_socket_errors.cpp; rocketnet-win-socket-error-paths-t-t"
     :evidence "macOS: rocketnet-macos-socket-errors-and-enobufs-t 7ea06926, 20 of 20; Linux: rocketnet-linux-socket-error-paths ce98ebd1, 20 of 20, xdp cells in a guest"
     :pending ((Windows "rocketnet-win-socket-error-paths-t-t (working)")))
    (:feature "Network thread priority" :cells (y y y)
     :source "test_unit_thread_priority.cpp, functional/thread_priority_timing.cpp"
     :evidence "Windows, macOS and Linux: test_unit_thread_priority cases run by name in ./bin/test on each, rocketnet-network-thread-priority-as-space 92e729dd")
    (:feature "Examples (run_examples.py)" :cells (n n n)
     :source "examples/, nightly-examples.yml; rocketnet-example-ack-receipts-debug-assert"
     :pending ((Windows "rocketnet-example-ack-receipts-debug-assert (review): examples/ack_receipts.c stops on the Debug assert in rocketnet_client_index")
               (macOS "rocketnet-example-ack-receipts-debug-assert (review)")
               (Linux "rocketnet-example-ack-receipts-debug-assert (review)")))
    (:feature "Example soak (example_soak)" :cells (n n n)
     :source "examples/soak.c; rocketnet-example-soak-disconnect-regression"
     :pending ((Windows "rocketnet-example-soak-disconnect-regression (-): worker-reported red at this branch on all three platforms, Debug and Release, at the check that the server has no connected clients after the disconnect passes; bisected to 0500dc7a")
               (macOS "rocketnet-example-soak-disconnect-regression (-): worker-reported red")
               (Linux "rocketnet-example-soak-disconnect-regression (-): worker-reported red")))
    (:feature "1000 clients, load harness" :cells (o o y)
     :source "load.c (./bin/load); rocketnet-win-load-1000-clients, rocketnet-macos-load-1000-clients-t"
     :evidence "Linux: ./bin/load 1000 60 passes all five criteria three times on portable, gso, gso plain and io_uring, once on xdp in a guest, rocketnet-linux-load-1000-clients 33f45ea9"
     :pending ((Windows "rocketnet-win-load-1000-clients (waiting)")
               (macOS "rocketnet-macos-load-1000-clients-t (working)")))
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :cells (o o o)
     :source "load.c light profile; rocketnet-load-light-profile-windows, rocketnet-load-light-profile-macos, rocketnet-load-light-profile-linux"
     :pending ((Windows "rocketnet-load-light-profile-windows (-)")
               (macOS "rocketnet-load-light-profile-macos (-)")
               (Linux "rocketnet-load-light-profile-linux (working)")))
    (:feature "Soak (30 minutes per transport under impairment)" :cells (o y o)
     :source "soak.c, soak_sim.cpp; rocketnet-win-soak-per-transport, rocketnet-linux-soak-per-transport"
     :evidence "macOS: six cells pass with flat fds and threads, rocketnet-macos-soak-per-transport df775cf1"
     :pending ((Windows "rocketnet-win-soak-per-transport (waiting)")
               (Linux "rocketnet-linux-soak-per-transport (working)")))
    (:feature "Main API calls under 0.1 ms each, Release, under load" :cells (p p p)
     :source "nightly-frame-budget.yml, tools/check_frame_budget.py (the client frame budget); no card for the per-call measurement"
     :pending ((Windows "no card: every main API call timed in Release at 1000 clients")
               (macOS "no card: every main API call timed in Release at 1000 clients")
               (Linux "no card: every main API call timed in Release at 1000 clients")))
    (:feature "Nightlies (every nightly job's steps green on this branch)" :cells (p o o)
     :source ".github/workflows/nightly*.yml, fuzz-nightly.yml; rocketnet-r1-windows-nightlies-all-run, rocketnet-macos-nightly-soak-green-at-base, rocketnet-linux-nightly-soak-green-at-base"
     :pending ((Windows "rocketnet-r1-windows-nightlies-all-run (working)")
               (macOS "rocketnet-macos-nightly-soak-green-at-base (-) for nightly-soak; fuzz, examples and frame budget ran by hand in rocketnet-macos-nightlies-green-at-base landed e7fba514")
               (Linux "rocketnet-linux-nightly-soak-green-at-base (review) for nightly-soak; fuzz, frame budget, channel seeds and leaks green, rocketnet-linux-nightlies-green-at-base 381203ba")))
    (:feature "CI (ci.yml, every step green on this branch)" :cells (p p p)
     :source ".github/workflows/ci.yml; rocketnet-windows-ci-green-at-base, rocketnet-macos-ci-green-at-base"
     :pending ((Windows "rocketnet-windows-ci-green-at-base (working)")
               (macOS "rocketnet-macos-ci-green-at-base (working)")
               (Linux "ci.yml run 39491507693 at 443ea520 queued; big endian has no card; every other Linux step green by hand, rocketnet-linux-ci-green-at-base 32c4ff08")))))
  (:id "platform-specific" :title "Platform-specific tests"
   :text "The tests that apply to only some platforms: one row per platform a test applies to."
   :label "test"
   :specific t
   :columns ("Windows" "macOS" "Linux")
   :rows
   ((:feature "UDP connection reset (SIO_UDP_CONNRESET) never stalls receive" :on Windows :cell p
     :source "test_iocp.cpp; rocketnet-windows-udp-connreset-never-stalls-receive"
     :pending "rocketnet-windows-udp-connreset-never-stalls-receive (working)")
    (:feature "Application Verifier" :on Windows :cell y
     :source "rocketnet-win-app-verifier"
     :evidence "Windows: 0 verifier stops, rocketnet-win-app-verifier 7457d35b")
    (:feature "App Nap and timer coalescing keep client timing" :on macOS :cell y
     :source "functional/macos_client_timing.cpp"
     :evidence "macOS: functional/macos_client_timing.cpp, rocketnet-macos-client-timing-under-app-nap a2c4ac1f")
    (:feature "MemorySanitizer (msan.yml)" :on Linux :cell p
     :source ".github/workflows/msan.yml, tools/msan_known.txt"
     :pending "msan.yml run 39491507654 at 443ea520 queued; no card names a green run at this branch")
    (:feature "Big-endian system build and suites" :on Linux :cell p
     :source ".github/workflows/nightly-bigendian-system.yml, tools/bigendian/"
     :pending "no card")
    (:feature "Live XDP fleet test (the XDP server on space, a client on every fleet machine)" :on Linux :cell p
     :source "docs/LIVE-XDP.md, tla/MCRNLiveFleetRun.cfg; rocketnet-live-xdp-fleet-run"
     :pending "rocketnet-live-xdp-fleet-run, moved out of the v1.0 sprint 2026-10-19 (later); after rocketnet is done")))
  (:id "transports" :title "Tests per transport"
   :text ("The tests that apply to every transport: one row per test, one column per transport, in the order the automatic ladder ranks them."
          "xdp, gso and io_uring build on Linux, msgx on macOS, iocp and rio on Windows, and portable and loopback on all three; a portable or loopback cell is ✅ only when it is green on all three.")
   :label "test"
   :columns transports
   :rows
   ((:feature "Transport family matrix (IPv4, IPv6 and dual stack, each transport pinned)"
     :cells (y p y y p p p p)
     :source "functional/live_matrix.cpp, tools/check_transport_matrix.py, ci.yml matrix step"
     :evidence "xdp: xdp_max_size_both_ways_ipv4 and _ipv6 in a guest, rocketnet-r1-ci-xdp-cell-on-libxdp-1-4 cce78fab; gso and io_uring: the matrix step's gso and io_uring cells green, rocketnet-linux-ci-green-at-base 32c4ff08"
     :pending ((msgx "rocketnet-macos-ci-green-at-base (working)")
               (portable "macOS: rocketnet-macos-ci-green-at-base (working); Windows: rocketnet-windows-ci-green-at-base (working); Linux is green, rocketnet-linux-pinned-portable-functional-green e73c2177")
               (iocp "rocketnet-windows-ci-green-at-base (working)")
               (rio "rocketnet-windows-ci-green-at-base (working)")
               (loopback "macOS: rocketnet-macos-ci-green-at-base (working); Windows: rocketnet-windows-ci-green-at-base (working); Linux: no landed card names the loopback cell")))
    (:feature "Every matrix cell fifty times, zero flakes"
     :cells (o o o o o o o o)
     :source "tools/check_transport_matrix.py; rocketnet-win-transport-matrix-repeat, rocketnet-macos-transport-matrix-repeat, rocketnet-linux-transport-matrix-repeat"
     :pending ((xdp "rocketnet-linux-transport-matrix-repeat (waiting)")
               (msgx "rocketnet-macos-transport-matrix-repeat (waiting)")
               (gso "rocketnet-linux-transport-matrix-repeat (waiting)")
               (io_uring "rocketnet-linux-transport-matrix-repeat (waiting)")
               (portable "rocketnet-win-transport-matrix-repeat, rocketnet-macos-transport-matrix-repeat and rocketnet-linux-transport-matrix-repeat (waiting)")
               (iocp "rocketnet-win-transport-matrix-repeat (waiting)")
               (rio "rocketnet-win-transport-matrix-repeat (waiting)")
               (loopback "rocketnet-win-transport-matrix-repeat, rocketnet-macos-transport-matrix-repeat and rocketnet-linux-transport-matrix-repeat (waiting)")))))
  (:id "transport-specific" :title "Transport-specific tests"
   :text "The tests that apply to only some transports: one row per transport a test applies to. None of these runs on loopback, the in-process transport; Application Verifier runs only on Windows."
   :label "test"
   :specific t
   :columns transports
   :rows
   ((:feature "Functional suite on each server transport" :on xdp :cell y
     :source "functional/ (./bin/functional, transport pinned)"
     :evidence "xdp: in a guest, rocketnet-r1-ci-xdp-cell-on-libxdp-1-4 cce78fab")
    (:feature "Functional suite on each server transport" :on msgx :cell y
     :source "functional/ (./bin/functional, transport pinned)"
     :evidence "msgx: ./bin/functional on the default ladder, rocketnet-macos-client-timing-under-app-nap a2c4ac1f")
    (:feature "Functional suite on each server transport" :on gso :cell y
     :source "functional/ (./bin/functional, transport pinned)"
     :evidence "gso: ./bin/functional pinned to gso, rocketnet-linux-pinned-portable-functional-green e73c2177")
    (:feature "Functional suite on each server transport" :on io_uring :cell y
     :source "functional/ (./bin/functional, transport pinned)"
     :evidence "io_uring: ./bin/functional pinned to io_uring, rocketnet-linux-pinned-portable-functional-green e73c2177")
    (:feature "Functional suite on each server transport" :on portable :cell p
     :source "functional/ (./bin/functional, transport pinned)"
     :pending "macOS: rocketnet-macos-ci-green-at-base (working); Windows and Linux are green, rocketnet-win-app-verifier 7457d35b and rocketnet-linux-pinned-portable-functional-green e73c2177")
    (:feature "Functional suite on each server transport" :on iocp :cell y
     :source "functional/ (./bin/functional, transport pinned)"
     :evidence "iocp: functional.exe pinned to iocp under App Verifier, rocketnet-win-app-verifier 7457d35b")
    (:feature "Functional suite on each server transport" :on rio :cell y
     :source "functional/ (./bin/functional, transport pinned)"
     :evidence "rio: functional.exe pinned to rio under App Verifier, rocketnet-win-app-verifier 7457d35b")
    (:feature "Hard-killed peer or ICMP error never stalls receive" :on xdp :cell p
     :source "functional/posix_hard_killed_peer.cpp; rocketnet-windows-udp-connreset-never-stalls-receive, rocketnet-linux-hard-killed-peer-never-stalls-receive"
     :pending "rocketnet-linux-hard-killed-peer-never-stalls-receive (working)")
    (:feature "Hard-killed peer or ICMP error never stalls receive" :on msgx :cell y
     :source "functional/posix_hard_killed_peer.cpp; rocketnet-windows-udp-connreset-never-stalls-receive, rocketnet-linux-hard-killed-peer-never-stalls-receive"
     :evidence "msgx: a SIGKILLed client, rocketnet-macos-hard-killed-peer-never-stalls-receive landed 443ea520")
    (:feature "Hard-killed peer or ICMP error never stalls receive" :on gso :cell p
     :source "functional/posix_hard_killed_peer.cpp; rocketnet-windows-udp-connreset-never-stalls-receive, rocketnet-linux-hard-killed-peer-never-stalls-receive"
     :pending "rocketnet-linux-hard-killed-peer-never-stalls-receive (working)")
    (:feature "Hard-killed peer or ICMP error never stalls receive" :on io_uring :cell p
     :source "functional/posix_hard_killed_peer.cpp; rocketnet-windows-udp-connreset-never-stalls-receive, rocketnet-linux-hard-killed-peer-never-stalls-receive"
     :pending "rocketnet-linux-hard-killed-peer-never-stalls-receive (working)")
    (:feature "Hard-killed peer or ICMP error never stalls receive" :on portable :cell p
     :source "functional/posix_hard_killed_peer.cpp; rocketnet-windows-udp-connreset-never-stalls-receive, rocketnet-linux-hard-killed-peer-never-stalls-receive"
     :pending "Windows: rocketnet-windows-udp-connreset-never-stalls-receive (working); Linux: rocketnet-linux-hard-killed-peer-never-stalls-receive (working); macOS is green, rocketnet-macos-hard-killed-peer-never-stalls-receive landed 443ea520")
    (:feature "Hard-killed peer or ICMP error never stalls receive" :on iocp :cell p
     :source "functional/posix_hard_killed_peer.cpp; rocketnet-windows-udp-connreset-never-stalls-receive, rocketnet-linux-hard-killed-peer-never-stalls-receive"
     :pending "rocketnet-windows-udp-connreset-never-stalls-receive (working)")
    (:feature "Hard-killed peer or ICMP error never stalls receive" :on rio :cell p
     :source "functional/posix_hard_killed_peer.cpp; rocketnet-windows-udp-connreset-never-stalls-receive, rocketnet-linux-hard-killed-peer-never-stalls-receive"
     :pending "rocketnet-windows-udp-connreset-never-stalls-receive (working)")
    (:feature "Socket error paths" :on xdp :cell y
     :source "functional/posix_socket_errors.cpp; rocketnet-win-socket-error-paths-t-t"
     :evidence "xdp: 5 of 5 in a guest, rocketnet-linux-socket-error-paths ce98ebd1")
    (:feature "Socket error paths" :on msgx :cell y
     :source "functional/posix_socket_errors.cpp; rocketnet-win-socket-error-paths-t-t"
     :evidence "msgx: rocketnet-macos-socket-errors-and-enobufs-t 7ea06926")
    (:feature "Socket error paths" :on gso :cell y
     :source "functional/posix_socket_errors.cpp; rocketnet-win-socket-error-paths-t-t"
     :evidence "gso: rocketnet-linux-socket-error-paths ce98ebd1")
    (:feature "Socket error paths" :on io_uring :cell y
     :source "functional/posix_socket_errors.cpp; rocketnet-win-socket-error-paths-t-t"
     :evidence "io_uring: rocketnet-linux-socket-error-paths ce98ebd1")
    (:feature "Socket error paths" :on portable :cell p
     :source "functional/posix_socket_errors.cpp; rocketnet-win-socket-error-paths-t-t"
     :pending "Windows: rocketnet-win-socket-error-paths-t-t (working); macOS and Linux are green, rocketnet-macos-socket-errors-and-enobufs-t 7ea06926 and rocketnet-linux-socket-error-paths ce98ebd1")
    (:feature "Socket error paths" :on iocp :cell p
     :source "functional/posix_socket_errors.cpp; rocketnet-win-socket-error-paths-t-t"
     :pending "rocketnet-win-socket-error-paths-t-t (working)")
    (:feature "Socket error paths" :on rio :cell p
     :source "functional/posix_socket_errors.cpp; rocketnet-win-socket-error-paths-t-t"
     :pending "rocketnet-win-socket-error-paths-t-t (working)")
    (:feature "Lifecycle stress" :on xdp :cell y
     :source "test_lifecycle.cpp, functional/posix_lifecycle_stress.cpp; rocketnet-win-lifecycle-stress"
     :evidence "xdp: in a guest, rocketnet-linux-lifecycle-stress-t 8a68c102")
    (:feature "Lifecycle stress" :on msgx :cell y
     :source "test_lifecycle.cpp, functional/posix_lifecycle_stress.cpp; rocketnet-win-lifecycle-stress"
     :evidence "msgx: msgx and msgx forced portable, rocketnet-macos-lifecycle-stress-t landed 2773d0da")
    (:feature "Lifecycle stress" :on gso :cell y
     :source "test_lifecycle.cpp, functional/posix_lifecycle_stress.cpp; rocketnet-win-lifecycle-stress"
     :evidence "gso: 120 of 120 over 5 runs, rocketnet-linux-lifecycle-stress-t 8a68c102")
    (:feature "Lifecycle stress" :on io_uring :cell y
     :source "test_lifecycle.cpp, functional/posix_lifecycle_stress.cpp; rocketnet-win-lifecycle-stress"
     :evidence "io_uring: rocketnet-linux-lifecycle-stress-t 8a68c102")
    (:feature "Lifecycle stress" :on portable :cell p
     :source "test_lifecycle.cpp, functional/posix_lifecycle_stress.cpp; rocketnet-win-lifecycle-stress"
     :pending "Windows: rocketnet-win-lifecycle-stress (review); macOS and Linux are green, rocketnet-macos-lifecycle-stress-t landed 2773d0da and rocketnet-linux-lifecycle-stress-t 8a68c102")
    (:feature "Lifecycle stress" :on iocp :cell p
     :source "test_lifecycle.cpp, functional/posix_lifecycle_stress.cpp; rocketnet-win-lifecycle-stress"
     :pending "rocketnet-win-lifecycle-stress (review)")
    (:feature "Lifecycle stress" :on rio :cell p
     :source "test_lifecycle.cpp, functional/posix_lifecycle_stress.cpp; rocketnet-win-lifecycle-stress"
     :pending "rocketnet-win-lifecycle-stress (review)")
    (:feature "Soak under impairment, flat resources" :on xdp :cell o
     :source "soak.c, soak_sim.cpp; rocketnet-win-soak-per-transport, rocketnet-linux-soak-per-transport"
     :pending "rocketnet-linux-soak-per-transport (working)")
    (:feature "Soak under impairment, flat resources" :on msgx :cell y
     :source "soak.c, soak_sim.cpp; rocketnet-win-soak-per-transport, rocketnet-linux-soak-per-transport"
     :evidence "msgx: msgx and msgx degraded, impaired and clean, rocketnet-macos-soak-per-transport df775cf1")
    (:feature "Soak under impairment, flat resources" :on gso :cell o
     :source "soak.c, soak_sim.cpp; rocketnet-win-soak-per-transport, rocketnet-linux-soak-per-transport"
     :pending "rocketnet-linux-soak-per-transport (working)")
    (:feature "Soak under impairment, flat resources" :on io_uring :cell o
     :source "soak.c, soak_sim.cpp; rocketnet-win-soak-per-transport, rocketnet-linux-soak-per-transport"
     :pending "rocketnet-linux-soak-per-transport (working)")
    (:feature "Soak under impairment, flat resources" :on portable :cell o
     :source "soak.c, soak_sim.cpp; rocketnet-win-soak-per-transport, rocketnet-linux-soak-per-transport"
     :pending "Windows: rocketnet-win-soak-per-transport (waiting); Linux: rocketnet-linux-soak-per-transport (working); macOS is green, rocketnet-macos-soak-per-transport df775cf1")
    (:feature "Soak under impairment, flat resources" :on iocp :cell o
     :source "soak.c, soak_sim.cpp; rocketnet-win-soak-per-transport, rocketnet-linux-soak-per-transport"
     :pending "rocketnet-win-soak-per-transport (waiting)")
    (:feature "Soak under impairment, flat resources" :on rio :cell o
     :source "soak.c, soak_sim.cpp; rocketnet-win-soak-per-transport, rocketnet-linux-soak-per-transport"
     :pending "rocketnet-win-soak-per-transport (waiting)")
    (:feature "1000 clients, load harness" :on xdp :cell y
     :source "load.c (./bin/load); rocketnet-win-load-1000-clients, rocketnet-macos-load-1000-clients-t"
     :evidence "xdp: one run in a guest, rocketnet-linux-load-1000-clients 33f45ea9")
    (:feature "1000 clients, load harness" :on msgx :cell o
     :source "load.c (./bin/load); rocketnet-win-load-1000-clients, rocketnet-macos-load-1000-clients-t"
     :pending "rocketnet-macos-load-1000-clients-t (working)")
    (:feature "1000 clients, load harness" :on gso :cell y
     :source "load.c (./bin/load); rocketnet-win-load-1000-clients, rocketnet-macos-load-1000-clients-t"
     :evidence "gso: three runs, rocketnet-linux-load-1000-clients 33f45ea9")
    (:feature "1000 clients, load harness" :on io_uring :cell y
     :source "load.c (./bin/load); rocketnet-win-load-1000-clients, rocketnet-macos-load-1000-clients-t"
     :evidence "io_uring: three runs, rocketnet-linux-load-1000-clients 33f45ea9")
    (:feature "1000 clients, load harness" :on portable :cell o
     :source "load.c (./bin/load); rocketnet-win-load-1000-clients, rocketnet-macos-load-1000-clients-t"
     :pending "Windows: rocketnet-win-load-1000-clients (waiting); macOS: rocketnet-macos-load-1000-clients-t (working); Linux is green, rocketnet-linux-load-1000-clients 33f45ea9")
    (:feature "1000 clients, load harness" :on iocp :cell o
     :source "load.c (./bin/load); rocketnet-win-load-1000-clients, rocketnet-macos-load-1000-clients-t"
     :pending "rocketnet-win-load-1000-clients (waiting)")
    (:feature "1000 clients, load harness" :on rio :cell o
     :source "load.c (./bin/load); rocketnet-win-load-1000-clients, rocketnet-macos-load-1000-clients-t"
     :pending "rocketnet-win-load-1000-clients (waiting)")
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :on xdp :cell o
     :source "load.c light profile; rocketnet-load-light-profile-windows, rocketnet-load-light-profile-macos, rocketnet-load-light-profile-linux"
     :pending "rocketnet-load-light-profile-linux (working)")
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :on msgx :cell o
     :source "load.c light profile; rocketnet-load-light-profile-windows, rocketnet-load-light-profile-macos, rocketnet-load-light-profile-linux"
     :pending "rocketnet-load-light-profile-macos (-)")
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :on gso :cell o
     :source "load.c light profile; rocketnet-load-light-profile-windows, rocketnet-load-light-profile-macos, rocketnet-load-light-profile-linux"
     :pending "rocketnet-load-light-profile-linux (working)")
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :on io_uring :cell o
     :source "load.c light profile; rocketnet-load-light-profile-windows, rocketnet-load-light-profile-macos, rocketnet-load-light-profile-linux"
     :pending "rocketnet-load-light-profile-linux (working)")
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :on portable :cell o
     :source "load.c light profile; rocketnet-load-light-profile-windows, rocketnet-load-light-profile-macos, rocketnet-load-light-profile-linux"
     :pending "rocketnet-load-light-profile-windows (-), rocketnet-load-light-profile-macos (-) and rocketnet-load-light-profile-linux (working)")
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :on iocp :cell o
     :source "load.c light profile; rocketnet-load-light-profile-windows, rocketnet-load-light-profile-macos, rocketnet-load-light-profile-linux"
     :pending "rocketnet-load-light-profile-windows (-)")
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :on rio :cell o
     :source "load.c light profile; rocketnet-load-light-profile-windows, rocketnet-load-light-profile-macos, rocketnet-load-light-profile-linux"
     :pending "rocketnet-load-light-profile-windows (-)")
    (:feature "1000 clients over the LAN, every guarantee checked (soak matrix)" :on xdp :cell o
     :source "docs/lan-bench/; rocketnet-lan-1000-client-soak-matrix"
     :pending "rocketnet-lan-1000-client-soak-matrix (waiting)")
    (:feature "1000 clients over the LAN, every guarantee checked (soak matrix)" :on msgx :cell o
     :source "docs/lan-bench/; rocketnet-lan-1000-client-soak-matrix"
     :pending "rocketnet-lan-1000-client-soak-matrix (waiting)")
    (:feature "1000 clients over the LAN, every guarantee checked (soak matrix)" :on gso :cell o
     :source "docs/lan-bench/; rocketnet-lan-1000-client-soak-matrix"
     :pending "rocketnet-lan-1000-client-soak-matrix (waiting)")
    (:feature "1000 clients over the LAN, every guarantee checked (soak matrix)" :on io_uring :cell o
     :source "docs/lan-bench/; rocketnet-lan-1000-client-soak-matrix"
     :pending "rocketnet-lan-1000-client-soak-matrix (waiting)")
    (:feature "1000 clients over the LAN, every guarantee checked (soak matrix)" :on portable :cell o
     :source "docs/lan-bench/; rocketnet-lan-1000-client-soak-matrix"
     :pending "rocketnet-lan-1000-client-soak-matrix (waiting)")
    (:feature "1000 clients over the LAN, every guarantee checked (soak matrix)" :on iocp :cell o
     :source "docs/lan-bench/; rocketnet-lan-1000-client-soak-matrix"
     :pending "rocketnet-lan-1000-client-soak-matrix (waiting)")
    (:feature "1000 clients over the LAN, every guarantee checked (soak matrix)" :on rio :cell o
     :source "docs/lan-bench/; rocketnet-lan-1000-client-soak-matrix"
     :pending "rocketnet-lan-1000-client-soak-matrix (waiting)")
    (:feature "Sanitizers per transport" :on xdp :cell o
     :source "ci.yml sanitizer jobs; rocketnet-linux-transport-matrix-repeat, rocketnet-macos-sanitizers-and-leaks, rocketnet-win-msvc-asan"
     :pending "rocketnet-linux-transport-matrix-repeat (waiting): the functional suite under ASan per transport")
    (:feature "Sanitizers per transport" :on msgx :cell p
     :source "ci.yml sanitizer jobs; rocketnet-linux-transport-matrix-repeat, rocketnet-macos-sanitizers-and-leaks, rocketnet-win-msvc-asan"
     :pending "rocketnet-macos-sanitizers-and-leaks (review)")
    (:feature "Sanitizers per transport" :on gso :cell o
     :source "ci.yml sanitizer jobs; rocketnet-linux-transport-matrix-repeat, rocketnet-macos-sanitizers-and-leaks, rocketnet-win-msvc-asan"
     :pending "rocketnet-linux-transport-matrix-repeat (waiting): the functional suite under ASan per transport")
    (:feature "Sanitizers per transport" :on io_uring :cell o
     :source "ci.yml sanitizer jobs; rocketnet-linux-transport-matrix-repeat, rocketnet-macos-sanitizers-and-leaks, rocketnet-win-msvc-asan"
     :pending "rocketnet-linux-transport-matrix-repeat (waiting): the functional suite under ASan per transport")
    (:feature "Sanitizers per transport" :on portable :cell o
     :source "ci.yml sanitizer jobs; rocketnet-linux-transport-matrix-repeat, rocketnet-macos-sanitizers-and-leaks, rocketnet-win-msvc-asan"
     :pending "Linux: rocketnet-linux-transport-matrix-repeat (waiting): the functional suite under ASan per transport; macOS: rocketnet-macos-sanitizers-and-leaks (review); Windows is green, rocketnet-win-msvc-asan e2ed2a4b")
    (:feature "Sanitizers per transport" :on iocp :cell y
     :source "ci.yml sanitizer jobs; rocketnet-linux-transport-matrix-repeat, rocketnet-macos-sanitizers-and-leaks, rocketnet-win-msvc-asan"
     :evidence "iocp: functional.exe pinned to iocp under MSVC ASan, zero reports, rocketnet-win-msvc-asan e2ed2a4b")
    (:feature "Sanitizers per transport" :on rio :cell y
     :source "ci.yml sanitizer jobs; rocketnet-linux-transport-matrix-repeat, rocketnet-macos-sanitizers-and-leaks, rocketnet-win-msvc-asan"
     :evidence "rio: functional.exe pinned to rio under MSVC ASan, zero reports, rocketnet-win-msvc-asan e2ed2a4b")
    (:feature "Application Verifier" :on portable :cell y
     :source "rocketnet-win-app-verifier"
     :evidence "portable: 0 verifier stops on functional.exe's default transport on Windows, rocketnet-win-app-verifier 7457d35b")
    (:feature "Application Verifier" :on iocp :cell y
     :source "rocketnet-win-app-verifier"
     :evidence "iocp: 0 verifier stops, rocketnet-win-app-verifier 7457d35b")
    (:feature "Application Verifier" :on rio :cell y
     :source "rocketnet-win-app-verifier"
     :evidence "rio: 0 verifier stops, rocketnet-win-app-verifier 7457d35b")))
  (:id "release" :title "Release-wide tests"
   :text "The tests that are neither per platform nor per transport: model checks, proofs, reviews and checks of the code as a whole."
   :label "test"
   :columns release
   :rows
   ((:feature "Send budget: packets_per_update 256 at 60 Hz, receive sized at least 2x" :cells (p)
     :source "test_unit_send_budget.cpp, tla/MCRNSendBudget.cfg; rocketnet-server-send-capacity-sized-from-config"
     :pending ((status "rocketnet-server-send-capacity-sized-from-config (-); rocketnet-config-packets-per-update-name (working)")))
    (:feature "Security pass: per-client resource exhaustion disconnects the client" :cells (p)
     :source "SECURITY.md, tla/MCRNSecurityTables*.cfg"
     :pending ((status "no card")))
    (:feature "Receipt loss reported after its own deadline" :cells (p)
     :source "test_unit_channel_receipts.cpp, tla/MCRNReceipts.cfg; rocketnet-r1-receipt-loss-after-its-own-deadline"
     :pending ((status "rocketnet-r1-receipt-loss-after-its-own-deadline (-)")))
    (:feature "Source format in the netcode style (clang-format check)" :cells (p)
     :source "tools/format/; rocketnet-format-netcode-style-v1"
     :pending ((status "rocketnet-format-netcode-style-v1 (-)")))
    (:feature "TLA+ ledger current (tla/CASES.tsv and tla/COVERAGE.tsv match the models)" :cells (p)
     :source "tla/CASES.tsv, tla/COVERAGE.tsv, tools/check_tla_coverage.py; rocketnet-r1-tla-ledger-current"
     :pending ((status "rocketnet-r1-tla-ledger-current (working)")))
    (:feature "TLA+ model check: link (19 required)" :cells (p)
     :source "tla/CASES.tsv group link; tla/MCLink.tla, tla/MCLinkPatterns.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: clientconnection (14 required, 9 bench)" :cells (p)
     :source "tla/CASES.tsv group clientconnection; tla/MCClientConnection.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base; the bench-gated configs run by hand on a bench, not in tlc.yml")))
    (:feature "TLA+ model check: replaywindow (2 required, 1 bench)" :cells (p)
     :source "tla/CASES.tsv group replaywindow; tla/MCReplayWindow.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base; the bench-gated configs run by hand on a bench, not in tlc.yml")))
    (:feature "TLA+ model check: rnsendstore (1 required)" :cells (p)
     :source "tla/CASES.tsv group rnsendstore; tla/MCRNSendStore.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnring (5 required)" :cells (p)
     :source "tla/CASES.tsv group rnring; tla/MCRNRing.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnsendspans (3 required)" :cells (p)
     :source "tla/CASES.tsv group rnsendspans; tla/MCRNSendSpans.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnwindow (7 required)" :cells (p)
     :source "tla/CASES.tsv group rnwindow; tla/MCRNWindow.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: receiveorder (4 required)" :cells (p)
     :source "tla/CASES.tsv group receiveorder; tla/MCReceiveOrder.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rndriversendstaging (2 required)" :cells (p)
     :source "tla/CASES.tsv group rndriversendstaging; tla/MCRNDriverSendStaging.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnsecuritytables (13 required)" :cells (p)
     :source "tla/CASES.tsv group rnsecuritytables; tla/MCRNSecurityTables.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: serverconnection (5 required)" :cells (p)
     :source "tla/CASES.tsv group serverconnection; tla/MCServerConnection.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnconnectionmemory (3 required)" :cells (p)
     :source "tla/CASES.tsv group rnconnectionmemory; tla/MCRNConnectionMemory.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnsendbudget (4 required)" :cells (p)
     :source "tla/CASES.tsv group rnsendbudget; tla/MCRNSendBudget.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnreceipts (5 required)" :cells (p)
     :source "tla/CASES.tsv group rnreceipts; tla/MCRNReceipts.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnchannel (9 required)" :cells (p)
     :source "tla/CASES.tsv group rnchannel; tla/MCRNChannel.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnslots (12 required)" :cells (p)
     :source "tla/CASES.tsv group rnslots; tla/MCRNSlots.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnchanneltypes (11 required)" :cells (p)
     :source "tla/CASES.tsv group rnchanneltypes; tla/MCRNChannelTypes.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnsenthistory (3 required)" :cells (p)
     :source "tla/CASES.tsv group rnsenthistory; tla/MCRNSentHistory.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnscheduler (3 required)" :cells (p)
     :source "tla/CASES.tsv group rnscheduler; tla/MCRNScheduler.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnresendtiming (3 required)" :cells (p)
     :source "tla/CASES.tsv group rnresendtiming; tla/MCRNResendTiming.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnfilterepochs (3 required)" :cells (p)
     :source "tla/CASES.tsv group rnfilterepochs; tla/MCRNFilterEpochs.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnuringdriver (4 required)" :cells (p)
     :source "tla/CASES.tsv group rnuringdriver; tla/MCRNUringDriver.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: fragmentsandacks (18 required, 6 bench)" :cells (p)
     :source "tla/CASES.tsv group fragmentsandacks; tla/MCFragmentsAndAcks.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base; the bench-gated configs run by hand on a bench, not in tlc.yml")))
    (:feature "TLA+ model check: rnblocks (2 required)" :cells (p)
     :source "tla/CASES.tsv group rnblocks; tla/MCRNBlocks.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnclientio (8 required)" :cells (p)
     :source "tla/CASES.tsv group rnclientio; tla/MCRNClientIo.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnendpoint (5 required, 2 bench)" :cells (p)
     :source "tla/CASES.tsv group rnendpoint; tla/MCRNEndpoint.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base; the bench-gated configs run by hand on a bench, not in tlc.yml")))
    (:feature "TLA+ model check: rnxdpreplay (3 required)" :cells (p)
     :source "tla/CASES.tsv group rnxdpreplay; tla/MCRNXdpReplay.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnxdpreceivering (2 required)" :cells (p)
     :source "tla/CASES.tsv group rnxdpreceivering; tla/MCRNXdpReceiveRing.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnriodriver (4 required)" :cells (p)
     :source "tla/CASES.tsv group rnriodriver; tla/MCRNRioDriver.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnflows (2 required)" :cells (p)
     :source "tla/CASES.tsv group rnflows; tla/MCRNFlows.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnclientiodecrypt (5 required)" :cells (p)
     :source "tla/CASES.tsv group rnclientiodecrypt; tla/MCRNClientIoDecrypt.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnlivefleetrun (3 required)" :cells (p)
     :source "tla/CASES.tsv group rnlivefleetrun; tla/MCRNLiveFleetRun.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: clientsession (5 required)" :cells (p)
     :source "tla/CASES.tsv group clientsession; tla/MCClientSession.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: serversession (10 required)" :cells (p)
     :source "tla/CASES.tsv group serversession; tla/MCServerSession.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnoutgoingbuild (4 required)" :cells (p)
     :source "tla/CASES.tsv group rnoutgoingbuild; tla/MCRNOutgoingBuild.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnsimfaults (3 required)" :cells (p)
     :source "tla/CASES.tsv group rnsimfaults; tla/MCRNSimFaults.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnclientreceivequit (5 required)" :cells (p)
     :source "tla/CASES.tsv group rnclientreceivequit; tla/MCRNClientReceiveQuit.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: rnserversendfair (4 required)" :cells (p)
     :source "tla/CASES.tsv group rnserversendfair; tla/MCRNServerSendFair.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "TLA+ model check: sendhandoff (3 required)" :cells (p)
     :source "tla/CASES.tsv group sendhandoff; tla/MCSendHandoff.tla; .github/workflows/tlc.yml"
     :pending ((status "tlc.yml (TLC records) run 39491507653 at 443ea520 queued, not finished; last green TLC records run 38950854663 is at 0b1f91c4, not this base")))
    (:feature "CBMC bounded proofs of the untrusted-input readers to the full packet size (Frama-C route for C++)" :cells (p)
     :source "proofs/cbmc/, cbmc.yml; rocketnet-cbmc-ack-block-read, rocketnet-cbmc-server-packet-readers, rocketnet-cbmc-client-packet-readers, rocketnet-cbmc-sequence-buffers"
     :pending ((status "rocketnet-cbmc-ack-block-read, rocketnet-cbmc-server-packet-readers, rocketnet-cbmc-client-packet-readers and rocketnet-cbmc-sequence-buffers, moved out of the v1.0 sprint 2026-10-19 (later)")))
    (:feature "Frama-Clang WP trial on rocketnet_ack_block_read" :cells (p)
     :source "proofs/framaclang/ (planned); rocketnet-frama-clang-trial"
     :pending ((status "rocketnet-frama-clang-trial, moved out of the v1.0 sprint 2026-10-19 (later)")))
    (:feature "Holistic review: six independent whole-system reviewers" :cells (p)
     :source "no card; after rocketnet is done"
     :pending ((status "no card: six independent reviews of the whole system after every feature has landed")))))))
