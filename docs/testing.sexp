; RocketNet v1.0.0 testing data. TESTING.md is rendered from this file by
; tools/roadmap/render.py (make testing); make testing-check fails when the
; committed page is not what this renders. The shapes are the roadmap's
; (docs/roadmap.sexp), with the platform columns in the order Windows, macOS,
; Linux and the transport columns in the order the automatic ladder ranks them.
;
; Four tables. "Tests per platform" holds the tests that apply to every
; platform, and "Platform-specific tests" each test that applies to only some,
; one row per platform it applies to. "Tests per transport" holds the tests
; that apply to every transport, and "Transport-specific tests" each test that
; applies to only some, one row per transport. :all-cells-apply refuses a -
; cell, so no table shows a dash. :tables-only prints only the headings, a
; line of source links under each, and the tables: the :text, :pending and
; :owed text below stays here as the data behind each cell. A transport that builds on more than one
; platform (portable, loopback) is y only when its card or CI shows it green
; on every platform it builds on; its note names the platforms still open.
;
; A cell is y only when the test has landed on sprint/rocketnet-proof and the
; card that landed it, or a CI run on that branch, shows it green there. Work
; in progress and cards not yet landed are n, and :pending names the card and
; its column (working, review, waiting, ready). A test with no card at all is
; n, marked "no card". A run that the two-minute rule defers until every
; feature and bug has landed (soak, load, repeat and 1000-client runs) is o,
; shown as OWED, with the card that runs it in its note.
;
; The per-commit CI (ci.yml) runs on pull requests and on a dispatch; the
; scheduled nightlies run only on main, which is far behind this branch, so
; no nightly has run on this branch's code as scheduled. A Linux cell for CI
; or a nightly comes from the cards that ran those jobs' steps by hand.
;
; Recorded 2026-10-20 at sprint/rocketnet-proof c13e1408.
(:title "RocketNet v1.0.0 testing"
 :all-cells-apply t
 :tables-only t
 :owed "the run is longer than two minutes, and such runs (soak, load, repeat and 1000-client) wait until every feature and bug has landed. OWED is neither a pass nor a fail; the note names the card that runs it."
 :text ("Every test RocketNet runs for v1.0.0, by platform and by transport. ✅ is landed on the release branch and shown green by its card or by CI; ❌ is not yet: in progress, not landed, or not carded; OWED is a long run deferred until every feature has landed. Each table's complete row is the share of ✅ among that table's own cells. Under each table, every ❌ and OWED names the card behind it, or no card.")
 :areas
 ((:id "platforms" :title "Tests per platform"
   :text "The tests that apply to every platform: one row per test, one column per platform."
   :label "test"
   :columns ("Windows" "macOS" "Linux")
   :rows
   ((:feature "Unit suite (./bin/test)" :cells (y y y)
     :evidence "Windows: test.exe Release green on the Windows member, rocketnet-driver-create-reports-out-of-memory b3162561; macOS and Linux: ./bin/test green Debug and Release, rocketnet-driver-create-reports-out-of-memory b3162561")
    (:feature "Functional suite (./bin/functional)" :cells (y y y)
     :evidence "Windows: functional.exe Release green, rocketnet-functional-builds-on-windows 85a98401, 10 of 10 runs in rocketnet-functional-tokens-use-the-test-clock d59e30de; macOS: ./bin/functional green, rocketnet-macos-client-timing-under-app-nap a2c4ac1f and rocketnet-macos-msgx-forced-portable-client-creates bb1521ab; Linux: ./bin/functional green Debug with the transport unset and pinned to portable, gso and io_uring, rocketnet-linux-pinned-portable-functional-green e73c2177")
    (:feature "Functional suite repeated, zero flakes" :cells (o o o)
     :pending ((Windows "rocketnet-windows-suite-stable (working): 20 of 20 runs")
               (macOS "rocketnet-macos-transport-matrix-repeat (waiting): every cell fifty times")
               (Linux "rocketnet-linux-transport-matrix-repeat (waiting): every cell fifty times")))
    (:feature "Sanitizers (ASan, UBSan, TSan; MSVC ASan on Windows)" :cells (y n n)
     :evidence "Windows: MSVC ASan, test.exe, test_lifecycle.exe, test_iocp.exe and functional.exe on the default transport, iocp and rio, zero reports, rocketnet-win-msvc-asan e2ed2a4b"
     :pending ((macOS "rocketnet-macos-sanitizers-and-leaks (working)")
               (Linux "rocketnet-linux-sanitizers-green-at-base (working): ci.yml's asan + ubsan and tsan jobs")))
    (:feature "Leaks (LSan, the leaks tool, App Verifier's leak layer)" :cells (y n n)
     :evidence "Windows: App Verifier with the Leak layer, 0 stops on test_lifecycle, test_iocp and functional per transport, rocketnet-win-app-verifier 7457d35b"
     :pending ((macOS "rocketnet-macos-sanitizers-and-leaks (working)")
               (Linux "rocketnet-linux-sanitizers-green-at-base (working): nightly-leaks; ./bin/functional under ASan and LSan already shows 0 leaks, rocketnet-r1-fuzz-and-sanitized-functional-clean-e 1c581f5d")))
    (:feature "libFuzzer (corpus replay and fuzz runs)" :cells (n n y)
     :evidence "Linux: fuzz-smoke and fuzz-smoke-ndebug green, rocketnet-linux-ci-green-at-base 32c4ff08; fuzz-nightly.yml's fuzz job green with its own time budget, rocketnet-linux-nightlies-green-at-base 381203ba"
     :pending ((Windows "rocketnet-win-libfuzzer-clang-cl (working)")
               (macOS "rocketnet-macos-nightlies-green-at-base (review): fuzz-macos")))
    (:feature "Lifecycle stress (init/term, create/destroy, restart, destroy under traffic)" :cells (n n y)
     :evidence "Linux: every case 5 runs in a row, xdp in a guest, rocketnet-linux-lifecycle-stress-t 8a68c102"
     :pending ((Windows "rocketnet-win-lifecycle-stress (working)")
               (macOS "rocketnet-macos-lifecycle-stress-t (working): the descriptor and thread counts per case")))
    (:feature "Hard-killed peer never stalls receive" :cells (n n n)
     :pending ((Windows "rocketnet-windows-udp-connreset-never-stalls-receive (working)")
               (macOS "rocketnet-macos-hard-killed-peer-never-stalls-receive (working)")
               (Linux "rocketnet-linux-hard-killed-peer-never-stalls-receive (working)")))
    (:feature "Socket error paths (oversize, runt, garbage, zero-length, failed sends)" :cells (n y y)
     :evidence "macOS: rocketnet-macos-socket-errors-and-enobufs-t 7ea06926, 20 of 20; Linux: rocketnet-linux-socket-error-paths ce98ebd1, 20 of 20, xdp cells in a guest"
     :pending ((Windows "rocketnet-win-socket-error-paths-t (working)")))
    (:feature "Network thread priority" :cells (y y y)
     :evidence "Windows, macOS and Linux: test_unit_thread_priority cases run by name in ./bin/test on each, rocketnet-network-thread-priority-as-space 92e729dd")
    (:feature "Examples (run_examples.py)" :cells (n n n)
     :pending ((Windows "rocketnet-example-ack-receipts-debug-assert (working): example_budgets fails at this branch since sends drop until connected")
               (macOS "rocketnet-example-ack-receipts-debug-assert (working)")
               (Linux "rocketnet-example-ack-receipts-debug-assert (working)")))
    (:feature "1000 clients, load harness" :cells (o o y)
     :evidence "Linux: ./bin/load 1000 60 passes all five criteria three times on portable, gso, gso plain and io_uring, once on xdp in a guest, rocketnet-linux-load-1000-clients 33f45ea9"
     :pending ((Windows "rocketnet-win-load-1000-clients (waiting)")
               (macOS "rocketnet-macos-load-1000-clients-t (working)")))
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :cells (o o o)
     :pending ((Windows "rocketnet-load-light-profile-windows (waiting)")
               (macOS "rocketnet-load-light-profile-macos (waiting)")
               (Linux "rocketnet-load-light-profile-linux (working)")))
    (:feature "Soak (30 minutes per transport under impairment)" :cells (o y o)
     :evidence "macOS: six cells pass with flat fds and threads, rocketnet-macos-soak-per-transport df775cf1"
     :pending ((Windows "rocketnet-win-soak-per-transport (waiting)")
               (Linux "rocketnet-linux-soak-per-transport (working)")))
    (:feature "Nightlies (every nightly job's steps green on this branch)" :cells (n n n)
     :pending ((Windows "rocketnet-r1-windows-nightlies-all-run (working); rocketnet-example-ack-receipts-debug-assert (working) for the examples job; build-and-test-windows-debug ran step by step green, rocketnet-win-nightlies-on-fleet-runner f2dae0dc")
               (macOS "rocketnet-macos-nightlies-green-at-base (review) and rocketnet-macos-nightly-soak-green-at-base (waiting)")
               (Linux "rocketnet-linux-nightly-soak-green-at-base (working) for nightly-soak, rocketnet-linux-sanitizers-green-at-base (working) for nightly-leaks, rocketnet-example-ack-receipts-debug-assert (working) for the examples job; fuzz, frame budget and channel seeds are green, rocketnet-linux-nightlies-green-at-base 381203ba")))
    (:feature "CI (ci.yml, every step green on this branch)" :cells (n n n)
     :pending ((Windows "rocketnet-windows-ci-green-at-base (working)")
               (macOS "rocketnet-macos-ci-green-at-base (working)")
               (Linux "rocketnet-linux-sanitizers-green-at-base (working) for the sanitizer and tsan jobs; big endian: no card; every other Linux step is green, rocketnet-linux-ci-green-at-base 32c4ff08")))))
  (:id "platform-specific" :title "Platform-specific tests"
   :text "The tests that apply to only some platforms: one row per platform a test applies to."
   :label "test"
   :specific t
   :columns ("Windows" "macOS" "Linux")
   :rows
   ((:feature "UDP connection reset (SIO_UDP_CONNRESET) never stalls receive" :on Windows :cell n
     :pending "rocketnet-windows-udp-connreset-never-stalls-receive (working)")
    (:feature "Application Verifier" :on Windows :cell y
     :evidence "Windows: 0 verifier stops, rocketnet-win-app-verifier 7457d35b")
    (:feature "Toolchain matrix (clang-cl and ARM64 on Windows; gcc and clang on Linux)" :on Windows :cell y
     :evidence "Windows: clang-cl build passes test.exe and functional.exe, ARM64 cross build, rocketnet-win-toolchain-matrix 836cb427")
    (:feature "Toolchain matrix (clang-cl and ARM64 on Windows; gcc and clang on Linux)" :on Linux :cell n
     :pending "rocketnet-linux-toolchain-matrix (working)")
    (:feature "App Nap and timer coalescing keep client timing" :on macOS :cell y
     :evidence "macOS: functional/macos_client_timing.cpp, rocketnet-macos-client-timing-under-app-nap a2c4ac1f")
    (:feature "Universal arm64 and x86_64 build, Xcode generator" :on macOS :cell y
     :evidence "macOS: both slices built, the x86_64 slice's suites under Rosetta 2, ./bin/test from the Xcode generator's build, rocketnet-macos-universal-and-xcode-t 879d31f4")))
  (:id "transports" :title "Tests per transport"
   :text ("The tests that apply to every transport: one row per test, one column per transport, in the order the automatic ladder ranks them."
          "xdp, gso and io_uring build on Linux, msgx on macOS, iocp and rio on Windows, and portable and loopback on all three; a portable or loopback cell is ✅ only when it is green on all three.")
   :label "test"
   :columns transports
   :rows
   ((:feature "Transport family matrix (IPv4, IPv6 and dual stack, each transport pinned)"
     :cells (y n y y n n n n)
     :evidence "xdp: xdp_max_size_both_ways_ipv4 and _ipv6 in a guest, rocketnet-r1-ci-xdp-cell-on-libxdp-1-4 cce78fab; gso and io_uring: the matrix step's gso and io_uring cells green, rocketnet-linux-ci-green-at-base 32c4ff08"
     :pending ((msgx "rocketnet-macos-ci-green-at-base (working)")
               (portable "macOS: rocketnet-macos-ci-green-at-base (working); Windows: rocketnet-windows-ci-green-at-base (working); Linux is green, rocketnet-linux-pinned-portable-functional-green e73c2177")
               (iocp "rocketnet-windows-ci-green-at-base (working)")
               (rio "rocketnet-windows-ci-green-at-base (working)")
               (loopback "macOS: rocketnet-macos-ci-green-at-base (working); Windows: rocketnet-windows-ci-green-at-base (working); Linux: no landed card names the loopback cell")))
    (:feature "Every matrix cell fifty times, zero flakes"
     :cells (o o o o o o o o)
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
     :evidence "xdp: in a guest, rocketnet-r1-ci-xdp-cell-on-libxdp-1-4 cce78fab")
    (:feature "Functional suite on each server transport" :on msgx :cell y
     :evidence "msgx: ./bin/functional on the default ladder, rocketnet-macos-client-timing-under-app-nap a2c4ac1f")
    (:feature "Functional suite on each server transport" :on gso :cell y
     :evidence "gso: ./bin/functional pinned to gso, rocketnet-linux-pinned-portable-functional-green e73c2177")
    (:feature "Functional suite on each server transport" :on io_uring :cell y
     :evidence "io_uring: ./bin/functional pinned to io_uring, rocketnet-linux-pinned-portable-functional-green e73c2177")
    (:feature "Functional suite on each server transport" :on portable :cell n
     :pending "macOS: rocketnet-macos-ci-green-at-base (working); Windows and Linux are green, rocketnet-win-app-verifier 7457d35b and rocketnet-linux-pinned-portable-functional-green e73c2177")
    (:feature "Functional suite on each server transport" :on iocp :cell y
     :evidence "iocp: functional.exe pinned to iocp under App Verifier, rocketnet-win-app-verifier 7457d35b")
    (:feature "Functional suite on each server transport" :on rio :cell y
     :evidence "rio: functional.exe pinned to rio under App Verifier, rocketnet-win-app-verifier 7457d35b")
    (:feature "Hard-killed peer or ICMP error never stalls receive" :on xdp :cell n
     :pending "rocketnet-linux-hard-killed-peer-never-stalls-receive (working)")
    (:feature "Hard-killed peer or ICMP error never stalls receive" :on msgx :cell n
     :pending "rocketnet-macos-hard-killed-peer-never-stalls-receive (working)")
    (:feature "Hard-killed peer or ICMP error never stalls receive" :on gso :cell n
     :pending "rocketnet-linux-hard-killed-peer-never-stalls-receive (working)")
    (:feature "Hard-killed peer or ICMP error never stalls receive" :on io_uring :cell n
     :pending "rocketnet-linux-hard-killed-peer-never-stalls-receive (working)")
    (:feature "Hard-killed peer or ICMP error never stalls receive" :on portable :cell n
     :pending "rocketnet-windows-udp-connreset-never-stalls-receive, rocketnet-macos-hard-killed-peer-never-stalls-receive and rocketnet-linux-hard-killed-peer-never-stalls-receive (working)")
    (:feature "Hard-killed peer or ICMP error never stalls receive" :on iocp :cell n
     :pending "rocketnet-windows-udp-connreset-never-stalls-receive (working)")
    (:feature "Hard-killed peer or ICMP error never stalls receive" :on rio :cell n
     :pending "rocketnet-windows-udp-connreset-never-stalls-receive (working)")
    (:feature "Socket error paths" :on xdp :cell y
     :evidence "xdp: 5 of 5 in a guest, rocketnet-linux-socket-error-paths ce98ebd1")
    (:feature "Socket error paths" :on msgx :cell y
     :evidence "msgx: rocketnet-macos-socket-errors-and-enobufs-t 7ea06926")
    (:feature "Socket error paths" :on gso :cell y
     :evidence "gso: rocketnet-linux-socket-error-paths ce98ebd1")
    (:feature "Socket error paths" :on io_uring :cell y
     :evidence "io_uring: rocketnet-linux-socket-error-paths ce98ebd1")
    (:feature "Socket error paths" :on portable :cell n
     :pending "Windows: rocketnet-win-socket-error-paths-t (working); macOS and Linux are green, rocketnet-macos-socket-errors-and-enobufs-t 7ea06926 and rocketnet-linux-socket-error-paths ce98ebd1")
    (:feature "Socket error paths" :on iocp :cell n
     :pending "rocketnet-win-socket-error-paths-t (working)")
    (:feature "Socket error paths" :on rio :cell n
     :pending "rocketnet-win-socket-error-paths-t (working)")
    (:feature "Lifecycle stress" :on xdp :cell y
     :evidence "xdp: in a guest, rocketnet-linux-lifecycle-stress-t 8a68c102")
    (:feature "Lifecycle stress" :on msgx :cell n
     :pending "rocketnet-macos-lifecycle-stress-t (working)")
    (:feature "Lifecycle stress" :on gso :cell y
     :evidence "gso: 120 of 120 over 5 runs, rocketnet-linux-lifecycle-stress-t 8a68c102")
    (:feature "Lifecycle stress" :on io_uring :cell y
     :evidence "io_uring: rocketnet-linux-lifecycle-stress-t 8a68c102")
    (:feature "Lifecycle stress" :on portable :cell n
     :pending "Windows: rocketnet-win-lifecycle-stress (working); macOS: rocketnet-macos-lifecycle-stress-t (working); Linux is green, rocketnet-linux-lifecycle-stress-t 8a68c102")
    (:feature "Lifecycle stress" :on iocp :cell n
     :pending "rocketnet-win-lifecycle-stress (working)")
    (:feature "Lifecycle stress" :on rio :cell n
     :pending "rocketnet-win-lifecycle-stress (working)")
    (:feature "Soak under impairment, flat resources" :on xdp :cell o
     :pending "rocketnet-linux-soak-per-transport (working)")
    (:feature "Soak under impairment, flat resources" :on msgx :cell y
     :evidence "msgx: msgx and msgx degraded, impaired and clean, rocketnet-macos-soak-per-transport df775cf1")
    (:feature "Soak under impairment, flat resources" :on gso :cell o
     :pending "rocketnet-linux-soak-per-transport (working)")
    (:feature "Soak under impairment, flat resources" :on io_uring :cell o
     :pending "rocketnet-linux-soak-per-transport (working)")
    (:feature "Soak under impairment, flat resources" :on portable :cell o
     :pending "Windows: rocketnet-win-soak-per-transport (waiting); Linux: rocketnet-linux-soak-per-transport (working); macOS is green, rocketnet-macos-soak-per-transport df775cf1")
    (:feature "Soak under impairment, flat resources" :on iocp :cell o
     :pending "rocketnet-win-soak-per-transport (waiting)")
    (:feature "Soak under impairment, flat resources" :on rio :cell o
     :pending "rocketnet-win-soak-per-transport (waiting)")
    (:feature "1000 clients, load harness" :on xdp :cell y
     :evidence "xdp: one run in a guest, rocketnet-linux-load-1000-clients 33f45ea9")
    (:feature "1000 clients, load harness" :on msgx :cell o
     :pending "rocketnet-macos-load-1000-clients-t (working)")
    (:feature "1000 clients, load harness" :on gso :cell y
     :evidence "gso: three runs, rocketnet-linux-load-1000-clients 33f45ea9")
    (:feature "1000 clients, load harness" :on io_uring :cell y
     :evidence "io_uring: three runs, rocketnet-linux-load-1000-clients 33f45ea9")
    (:feature "1000 clients, load harness" :on portable :cell o
     :pending "Windows: rocketnet-win-load-1000-clients (waiting); macOS: rocketnet-macos-load-1000-clients-t (working); Linux is green, rocketnet-linux-load-1000-clients 33f45ea9")
    (:feature "1000 clients, load harness" :on iocp :cell o
     :pending "rocketnet-win-load-1000-clients (waiting)")
    (:feature "1000 clients, load harness" :on rio :cell o
     :pending "rocketnet-win-load-1000-clients (waiting)")
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :on xdp :cell o
     :pending "rocketnet-load-light-profile-linux (working)")
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :on msgx :cell o
     :pending "rocketnet-load-light-profile-macos (waiting)")
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :on gso :cell o
     :pending "rocketnet-load-light-profile-linux (working)")
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :on io_uring :cell o
     :pending "rocketnet-load-light-profile-linux (working)")
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :on portable :cell o
     :pending "rocketnet-load-light-profile-windows (waiting), rocketnet-load-light-profile-macos (waiting) and rocketnet-load-light-profile-linux (working)")
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :on iocp :cell o
     :pending "rocketnet-load-light-profile-windows (waiting)")
    (:feature "1000 light clients (10 Hz, 1 packet per update)" :on rio :cell o
     :pending "rocketnet-load-light-profile-windows (waiting)")
    (:feature "1000 clients over the LAN, every guarantee checked" :on xdp :cell o
     :pending "rocketnet-lan-1000-client-soak-matrix (waiting)")
    (:feature "1000 clients over the LAN, every guarantee checked" :on msgx :cell o
     :pending "rocketnet-lan-1000-client-soak-matrix (waiting)")
    (:feature "1000 clients over the LAN, every guarantee checked" :on gso :cell o
     :pending "rocketnet-lan-1000-client-soak-matrix (waiting)")
    (:feature "1000 clients over the LAN, every guarantee checked" :on io_uring :cell o
     :pending "rocketnet-lan-1000-client-soak-matrix (waiting)")
    (:feature "1000 clients over the LAN, every guarantee checked" :on portable :cell o
     :pending "rocketnet-lan-1000-client-soak-matrix (waiting)")
    (:feature "1000 clients over the LAN, every guarantee checked" :on iocp :cell o
     :pending "rocketnet-lan-1000-client-soak-matrix (waiting)")
    (:feature "1000 clients over the LAN, every guarantee checked" :on rio :cell o
     :pending "rocketnet-lan-1000-client-soak-matrix (waiting)")
    (:feature "Sanitizers per transport" :on xdp :cell n
     :pending "rocketnet-linux-transport-matrix-repeat (waiting): the functional suite under ASan per transport")
    (:feature "Sanitizers per transport" :on msgx :cell n
     :pending "rocketnet-macos-sanitizers-and-leaks (working)")
    (:feature "Sanitizers per transport" :on gso :cell n
     :pending "rocketnet-linux-transport-matrix-repeat (waiting): the functional suite under ASan per transport")
    (:feature "Sanitizers per transport" :on io_uring :cell n
     :pending "rocketnet-linux-transport-matrix-repeat (waiting): the functional suite under ASan per transport")
    (:feature "Sanitizers per transport" :on portable :cell n
     :pending "macOS: rocketnet-macos-sanitizers-and-leaks (working); Linux: rocketnet-linux-transport-matrix-repeat (waiting); Windows is green, rocketnet-win-msvc-asan e2ed2a4b")
    (:feature "Sanitizers per transport" :on iocp :cell y
     :evidence "iocp: functional.exe pinned to iocp under MSVC ASan, zero reports, rocketnet-win-msvc-asan e2ed2a4b")
    (:feature "Sanitizers per transport" :on rio :cell y
     :evidence "rio: functional.exe pinned to rio under MSVC ASan, zero reports, rocketnet-win-msvc-asan e2ed2a4b")
    (:feature "Application Verifier" :on portable :cell y
     :evidence "portable: 0 verifier stops on functional.exe's default transport on Windows, rocketnet-win-app-verifier 7457d35b")
    (:feature "Application Verifier" :on iocp :cell y
     :evidence "iocp: 0 verifier stops, rocketnet-win-app-verifier 7457d35b")
    (:feature "Application Verifier" :on rio :cell y
     :evidence "rio: 0 verifier stops, rocketnet-win-app-verifier 7457d35b")))))
