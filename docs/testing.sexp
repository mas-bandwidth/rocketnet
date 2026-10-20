; RocketNet v1.0.0 testing data. TESTING.md is rendered from this file by
; tools/roadmap/render.py (make testing); make testing-check fails when the
; committed page is not what this renders. The shapes are the roadmap's
; (docs/roadmap.sexp), with the columns in the order Windows, macOS, Linux.
;
; A cell is y only when the test has landed on sprint/rocketnet-proof and the
; card that landed it, or a CI run on that branch, shows it green on that
; platform. Work in progress and cards not yet landed are n, and :pending
; names the card and its column (working, review, waiting). A test with no
; card at all is n, marked "no card" until a card is drafted for it. The
; table of transports puts the transports a test ran on each platform in one
; cell: Linux portable, gso, io_uring and xdp; macOS msgx and portable;
; Windows portable, iocp and rio.
;
; The per-commit CI (ci.yml) runs on pull requests and on a dispatch; the
; scheduled nightlies run only on main, which is far behind this branch, so
; no nightly has run on this branch's code as scheduled. The last dispatched
; ci.yml run on this branch (run 39258743834, head 3ce29a1) was red on Linux
; and ran no macOS or Windows rows.
;
; Recorded 2026-10-20 at sprint/rocketnet-proof 091204c9.
(:title "RocketNet v1.0.0 testing"
 :text ("Every test RocketNet runs for v1.0.0, by platform and by transport. ✅ is landed on the release branch and shown green on that platform by its card or by CI; ❌ is not yet: in progress, not landed, or not carded. — does not apply. Each table's complete row is the share of ✅ among the cells that apply; in the transport table each transport counts once. Under each table, every ❌ names the card behind it, or no card.")
 :areas
 ((:id "platforms" :title "Tests per platform"
   :text "One row per test feature, one column per platform."
   :label "test"
   :columns ("Windows" "macOS" "Linux")
   :rows
   ((:feature "Unit suite (./bin/test)" :cells (y y y)
     :evidence "Windows: test.exe Release green on the Windows member, rocketnet-driver-create-reports-out-of-memory b3162561; macOS and Linux: ./bin/test green Debug and Release, rocketnet-driver-create-reports-out-of-memory b3162561")
    (:feature "Functional suite (./bin/functional)" :cells (y y y)
     :evidence "Windows: functional.exe Release green, rocketnet-functional-builds-on-windows 85a98401, 10 of 10 runs in rocketnet-functional-tokens-use-the-test-clock d59e30de; macOS: ./bin/functional green, rocketnet-macos-client-timing-under-app-nap a2c4ac1f; Linux: ./bin/functional green, rocketnet-linux-lifecycle-stress-t 8a68c102 and every landing's Linux gate")
    (:feature "Functional suite green every run (20 of 20)" :cells (n - -)
     :pending ((Windows "rocketnet-windows-suite-stable (working)")))
    (:feature "Sanitizers (ASan, UBSan, TSan; MSVC ASan on Windows)" :cells (n n n)
     :pending ((Windows "rocketnet-win-msvc-asan (working)")
               (macOS "rocketnet-macos-sanitizers-and-leaks (working)")
               (Linux "rocketnet-linux-sanitizers-green-at-base (new): the last ci.yml run on this branch was red in asan + ubsan and tsan")))
    (:feature "Leaks (LSan, the leaks tool, App Verifier's leak layer)" :cells (y n n)
     :evidence "Windows: App Verifier with the Leak layer, 0 stops on test_lifecycle, test_iocp and functional per transport, rocketnet-win-app-verifier 7457d35b"
     :pending ((macOS "rocketnet-macos-sanitizers-and-leaks (working); the soak card's leaks run was not made")
               (Linux "rocketnet-linux-sanitizers-green-at-base (new): nightly-leaks has run only on main")))
    (:feature "libFuzzer (corpus replay and fuzz runs)" :cells (n n y)
     :evidence "Linux: fuzz smoke (libFuzzer, corpus replay) and fuzz smoke NDEBUG green in ci.yml run 39258743834 on this branch"
     :pending ((Windows "rocketnet-win-libfuzzer-clang-cl (working)")
               (macOS "rocketnet-macos-nightlies-green-at-base (new): fuzz-macos has never run on this branch")))
    (:feature "Lifecycle stress (init/term, create/destroy, restart, destroy under traffic)" :cells (n n y)
     :evidence "Linux: every case 5 runs in a row, xdp in a guest, rocketnet-linux-lifecycle-stress-t 8a68c102"
     :pending ((Windows "rocketnet-win-lifecycle-stress (working)")
               (macOS "rocketnet-macos-lifecycle-stress-t (working)")))
    (:feature "Hard-killed peer never stalls receive" :cells (n n n)
     :pending ((Windows "rocketnet-windows-udp-connreset-never-stalls-receive (working)")
               (macOS "rocketnet-macos-hard-killed-peer-never-stalls-receive (working)")
               (Linux "rocketnet-linux-hard-killed-peer-never-stalls-receive (review)")))
    (:feature "Socket error paths (oversize, runt, garbage, zero-length, failed sends)" :cells (n y y)
     :evidence "macOS: rocketnet-macos-socket-errors-and-enobufs-t 7ea06926, 20 of 20; Linux: rocketnet-linux-socket-error-paths ce98ebd1, 20 of 20, xdp cells in a guest"
     :pending ((Windows "rocketnet-win-socket-error-paths-t (working)")))
    (:feature "UDP connection reset (SIO_UDP_CONNRESET) never stalls receive" :cells (n - -)
     :pending ((Windows "rocketnet-windows-udp-connreset-never-stalls-receive (working)")))
    (:feature "App Nap and timer coalescing keep client timing" :cells (- y -)
     :evidence "macOS: functional/macos_client_timing.cpp, rocketnet-macos-client-timing-under-app-nap a2c4ac1f")
    (:feature "Network thread priority" :cells (y y y)
     :evidence "Windows, macOS and Linux: test_unit_thread_priority cases run by name in ./bin/test on each, rocketnet-network-thread-priority-as-space 92e729dd")
    (:feature "Universal arm64 and x86_64 build, Xcode generator" :cells (- n -)
     :pending ((macOS "rocketnet-macos-universal-and-xcode-t (working)")))
    (:feature "Toolchain matrix (clang-cl and ARM64 on Windows; gcc and clang on Linux)" :cells (y - n)
     :evidence "Windows: clang-cl build passes test.exe and functional.exe, ARM64 cross build, rocketnet-win-toolchain-matrix 836cb427"
     :pending ((Linux "rocketnet-linux-toolchain-matrix (new)")))
    (:feature "Application Verifier" :cells (y - -)
     :evidence "Windows: 0 verifier stops, rocketnet-win-app-verifier 7457d35b")
    (:feature "Examples (run_examples.py)" :cells (n n n)
     :pending ((Windows "rocketnet-example-ack-receipts-debug-assert (working): example_budgets fails at this branch since sends drop until connected")
               (macOS "rocketnet-example-ack-receipts-debug-assert (working)")
               (Linux "rocketnet-example-ack-receipts-debug-assert (working)")))
    (:feature "1000 clients, load harness (three runs per transport)" :cells (n n y)
     :evidence "Linux: ./bin/load 1000 60 passes all five criteria three times on portable, gso, gso plain and io_uring, once on xdp in a guest, rocketnet-linux-load-1000-clients 33f45ea9"
     :pending ((Windows "rocketnet-win-load-1000-clients (waiting)")
               (macOS "rocketnet-macos-load-1000-clients-t (review)")))
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :cells (n n n)
     :pending ((Windows "rocketnet-load-light-profile-windows (new)")
               (macOS "rocketnet-load-light-profile-macos (new)")
               (Linux "rocketnet-load-light-profile-linux (new)")))
    (:feature "Soak (30 minutes per transport under impairment)" :cells (n y n)
     :evidence "macOS: six cells pass with flat fds and threads, rocketnet-macos-soak-per-transport df775cf1"
     :pending ((Windows "rocketnet-win-soak-per-transport (waiting)")
               (Linux "rocketnet-linux-soak-per-transport (working)")))
    (:feature "Nightlies (every nightly job's steps green on this branch)" :cells (y n n)
     :evidence "Windows: build-and-test-windows-debug and soak-windows run step by step green, rocketnet-win-nightlies-on-fleet-runner f2dae0dc"
     :pending ((macOS "rocketnet-macos-nightlies-green-at-base (new)")
               (Linux "rocketnet-linux-nightlies-green-at-base (new)")))
    (:feature "CI (ci.yml, every step green on this branch)" :cells (n n n)
     :pending ((Windows "rocketnet-windows-ci-green-at-base (new): the last run on this branch ran no Windows row")
               (macOS "rocketnet-macos-ci-green-at-base (new): the last run on this branch ran no macOS row")
               (Linux "rocketnet-linux-ci-green-at-base (new): the last run on this branch was red")))))
  (:id "transports" :title "Tests per transport"
   :text "One row per test, one column per platform; each cell names the transports the test runs on that platform."
   :label "test"
   :columns ("Windows" "macOS" "Linux")
   :rows
   ((:feature "Transport family matrix (IPv4, IPv6 and dual stack, each transport pinned)"
     :cells (((portable n) (iocp n) (rio n)) ((msgx n) (portable n)) ((portable n) (gso n) (io_uring n) (xdp n)))
     :pending ((Windows "rocketnet-windows-ci-green-at-base (new)")
               (macOS "msgx: rocketnet-macos-msgx-forced-portable-client-creates (working); portable: rocketnet-macos-ci-green-at-base (new)")
               (Linux "portable: rocketnet-linux-pinned-portable-functional-green (review); gso and io_uring: rocketnet-linux-ci-green-at-base (new); xdp: rocketnet-r1-ci-xdp-cell-on-libxdp-1-4 (working)")))
    (:feature "Every matrix cell fifty times, zero flakes"
     :cells (((portable n) (iocp n) (rio n)) ((msgx n) (portable n)) ((portable n) (gso n) (io_uring n) (xdp n)))
     :pending ((Windows "rocketnet-win-transport-matrix-repeat (waiting)")
               (macOS "rocketnet-macos-transport-matrix-repeat (waiting)")
               (Linux "rocketnet-linux-transport-matrix-repeat (waiting)")))
    (:feature "Functional suite with the server transport pinned"
     :cells (((portable y) (iocp y) (rio y)) ((msgx y) (portable n)) ((portable n) (gso y) (io_uring n)))
     :evidence "Windows: functional.exe passes on the default (portable), iocp and rio under App Verifier, rocketnet-win-app-verifier 7457d35b; macOS: ./bin/functional on the default ladder (msgx), rocketnet-macos-client-timing-under-app-nap a2c4ac1f; Linux: ./bin/functional on the default ladder (gso), rocketnet-linux-lifecycle-stress-t 8a68c102 (xdp runs its own cases in a guest, in the matrix row)"
     :pending ((macOS "portable: rocketnet-macos-ci-green-at-base (new)")
               (Linux "portable: rocketnet-linux-pinned-portable-functional-green (review); io_uring: rocketnet-linux-ci-green-at-base (new)")))
    (:feature "Hard-killed peer or ICMP error never stalls receive"
     :cells (((portable n) (iocp n) (rio n)) ((msgx n) (portable n)) ((portable n) (gso n) (io_uring n) (xdp n)))
     :pending ((Windows "rocketnet-windows-udp-connreset-never-stalls-receive (working)")
               (macOS "rocketnet-macos-hard-killed-peer-never-stalls-receive (working)")
               (Linux "rocketnet-linux-hard-killed-peer-never-stalls-receive (review)")))
    (:feature "Socket error paths"
     :cells (((portable n) (iocp n) (rio n)) ((msgx y) (portable y)) ((portable y) (gso y) (io_uring y) (xdp y)))
     :evidence "macOS: rocketnet-macos-socket-errors-and-enobufs-t 7ea06926; Linux: rocketnet-linux-socket-error-paths ce98ebd1, xdp cells 5 of 5 in a guest"
     :pending ((Windows "rocketnet-win-socket-error-paths-t (working)")))
    (:feature "Lifecycle stress"
     :cells (((portable n) (iocp n) (rio n)) ((msgx n) (portable n)) ((portable y) (gso y) (io_uring y) (xdp y)))
     :evidence "Linux: rocketnet-linux-lifecycle-stress-t 8a68c102, 120 of 120 over 5 runs, xdp in a guest"
     :pending ((Windows "rocketnet-win-lifecycle-stress (working)")
               (macOS "rocketnet-macos-lifecycle-stress-t (working)")))
    (:feature "Soak under impairment, flat resources"
     :cells (((portable n) (iocp n) (rio n)) ((msgx y) (portable y)) ((portable n) (gso n) (io_uring n) (xdp n)))
     :evidence "macOS: msgx, msgx degraded and portable, impaired and clean, rocketnet-macos-soak-per-transport df775cf1"
     :pending ((Windows "rocketnet-win-soak-per-transport (waiting)")
               (Linux "rocketnet-linux-soak-per-transport (working)")))
    (:feature "1000 clients, load harness"
     :cells (((portable n) (iocp n) (rio n)) ((msgx n) (portable n)) ((portable y) (gso y) (io_uring y) (xdp y)))
     :evidence "Linux: rocketnet-linux-load-1000-clients 33f45ea9, three runs per socket transport, one xdp run in a guest"
     :pending ((Windows "rocketnet-win-load-1000-clients (waiting)")
               (macOS "rocketnet-macos-load-1000-clients-t (review)")))
    (:feature "1000 light clients (10 Hz, 1 packet per update)"
     :cells (((portable n) (iocp n) (rio n)) ((msgx n) (portable n)) ((portable n) (gso n) (io_uring n) (xdp n)))
     :pending ((Windows "rocketnet-load-light-profile-windows (new)")
               (macOS "rocketnet-load-light-profile-macos (new)")
               (Linux "rocketnet-load-light-profile-linux (new)")))
    (:feature "1000 clients over the LAN, every guarantee checked"
     :cells (((portable n) (iocp n) (rio n)) ((msgx n) (portable n)) ((portable n) (gso n) (io_uring n) (xdp n)))
     :pending ((Windows "rocketnet-lan-1000-client-soak-matrix (waiting); its brief still names the 256 kbps profile the light profile replaces")
               (macOS "rocketnet-lan-1000-client-soak-matrix (waiting)")
               (Linux "rocketnet-lan-1000-client-soak-matrix (waiting)")))
    (:feature "Sanitizers per transport"
     :cells (((portable n) (iocp n) (rio n)) ((msgx n) (portable n)) ((portable n) (gso n) (io_uring n) (xdp n)))
     :pending ((Windows "rocketnet-win-msvc-asan (working)")
               (macOS "rocketnet-macos-sanitizers-and-leaks (working)")
               (Linux "rocketnet-linux-transport-matrix-repeat (waiting): the functional suite under ASan per transport")))
    (:feature "Application Verifier"
     :cells (((portable y) (iocp y) (rio y)) - -)
     :evidence "Windows: 0 verifier stops on functional default (portable), iocp and rio, rocketnet-win-app-verifier 7457d35b")))))
