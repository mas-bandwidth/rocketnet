RESULT: rn-examples-and-targets-trim adc41460e9ff3a5c3742a696ac99019223c8d3e8

Head: adc41460e9ff3a5c3742a696ac99019223c8d3e8 (gated tree; this file's commit is its child)
Branch: sprint/rn-examples-and-targets-trim.w3.g1.e1
Verdict: not-done

The line-1 sha is the merge that was built and run. Windows did not run, so the card is not done. The two known reds below were reproduced and were not changed.

## Merge

Fetched origin/main at 1a2f2c435afe1fba2120aada52954c1b798c8a40 (unchanged from the previously fetched tip). Merged it into e6b2de07a45aa1c30006f680a06b3d08f413de21. Parents of adc41460 are those two shas. The only conflict was CMakeLists.txt.

Resolution, re-read, not copied blind from the older generation: main's server_receive_cost target and its link lines stay; instruction_count stays and links rocketnet_dev (main already dropped rocketnet_dev_stats_manual, and this merge did not add it back); transport_fill and lease_cost are not targets. The hand-build comment for those two sources stays. The foreach that links rocketnet_dev no longer names them, and the lease_cost Threads link is gone. No other file conflicted.

examples/soak.c is not byte-identical to the pre-merge head. Pre-merge blob 9503e24ae2a1b9c006220b7a49cba2d1d0a36e93. Merged blob 189247755990fa934d15f66d4dc824025acf0e70, which is origin/main's blob. The difference is main's commit 8d402cc8 (a rename in that harness). This card did not edit examples/soak.c.

No added line against origin/main in the PATHS files contains either banned name.

JOB.md is untracked and was not committed.

## Line counts

Lines are splitlines() counts. origin/main versus this head. hello.c is 99, under 100. Every examples/*.c except soak.c is shorter than origin/main. soak.c is the same length (a harness, not trimmed).

| file | main | head | delta |
|---|---:|---:|---:|
| ack_receipts.c | 206 | 115 | -91 |
| backend_client.c | 146 | 96 | -50 |
| backend_server.c | 259 | 143 | -116 |
| big_messages.c | 187 | 136 | -51 |
| budgets.c | 199 | 115 | -84 |
| channels.c | 205 | 128 | -77 |
| client_server_client.c | 241 | 124 | -117 |
| client_server_server.c | 237 | 113 | -124 |
| config_env.c | 195 | 92 | -103 |
| game_loop.c | 256 | 246 | -10 |
| hello.c | 201 | 99 | -102 |
| reliability.c | 197 | 184 | -13 |
| soak.c | 628 | 628 | 0 |
| stats.c | 219 | 138 | -81 |
| transports.c | 186 | 109 | -77 |

## Targets

Root CMakeLists.txt add_executable / add_library versus origin/main: 42 on main, 40 here. Removed: transport_fill, lease_cost. Added: none. server_receive_cost and instruction_count are kept.

Removed, and who still uses the name:

- transport_fill. No workflow, Makefile rule, or test builds this cmake target. bench/transport_fill.md compiles bench/transport_fill.cpp by hand against build/librocketnet.a. TRANSPORTS.md only links that note. The unit cases test_xdp_transport_fill_ring_* are a different name.
- lease_cost. No workflow or Makefile rule. bench/lease_cost.py compiles bench/lease_cost.cpp by hand against librocketnet.a. bench/lease_cost.md documents that command.

Kept (who uses it):

- rocketnet, rocketnet_shared: installed library. rocketnet_dev, rocketnet_shared_dev: tests, functional, tools, examples. RocketNet::rocketnet and RocketNet::rocketnet_shared: package aliases.
- rocketnet_test_hooks: unit seams. rocketnet_test_support: linked into test (BUILDING.md). rocketnet_header_warnings: header warning object. rocketnet_sodium_oracle: sodium/README.md and tools/check_sodium.py. rocketnet_xdp_filter_host: TRANSPORTS.md host replay.
- client, server: ci and the happy path. happy_server, happy_client: make happy-path and ci. test_internal_c: tools/internal_c. test_sodium: ci. test: ./bin/test. test_wide: BUILDING.md, by hand. test_transport, test_serialize, test_transport_control: ci and functional dependencies. test_xdp, test_xdp_compat, test_xdp_filter: Linux docs and ROOTGATE. test_msgx: macOS functional. test_iocp: Windows ci. functional: ./bin/functional.
- soak, load, load_split, soak_sim: nightly soak and load workflows. dst: docs/DST.md. keygen: KEYS.md. transport_memory: tools/check_memory.py. client_main_thread_cost: nightly-frame-budget.yml and tools/check_frame_budget.py. instruction_count: tools/check_instructions.py and bench/instruction_count.md, linked to rocketnet_dev. server_receive_cost: bench kept from main, built by the default tools build; no workflow names the binary. fuzz_replay_*: ci.yml corpus replay.

examples/CMakeLists.txt and BUILDING.md match origin/main. Example binaries are kept and are what tools/run_examples.py and nightly-examples.yml run.

## game_loop check

The diff did touch the reporting around the check (a stored failure string instead of an immediate printf) and did not loosen it. The predicate is still the one on origin/main:

```
else if ( to_server.min != order_frames || to_server.max != order_frames )
    failure = "client->server latency is not the frame order's";
```

order_frames is 0 when the order is correct. max of 2 fails that. The library was not edited.

## Gates actually run

amd-1 (b1, HOME=/home/nova), tree /home/nova/johnny-examples-w3/rocketnet at adc41460. /home/nova/johnny-examples-trim was not touched. make -j4. Load at the start of make was 5.02; it later sat above 15 while other cards built.

- MAKE_EXIT=0 MAKE_WARNINGS=0 (grep -c 'warning:' on the make log was 0). Last lines: `[100%] Built target test`.
- TEST_EXIT=0. Log ends `*** ALL TESTS PASSED ***`.
- FUNCTIONAL_EXIT=0. Log ends `*** ALL TESTS PASSED ***`.
- CHECK_EXAMPLES_EXIT=0.
- RUN_EXAMPLES_EXIT=1. The script stops at the first failure, so later examples were not part of this exit. FAIL lines:

```
latency client->server frames: min 0 mean 0.89 max 2
latency server->client frames: min 1 mean 1.00 max 2
latency: 1 extra frame(s) (correct order)
FAIL: client->server latency is not the frame order's
FAIL example_game_loop: exited 1
```

Before that failure the same run printed PASS for example_client_server, example_ack_receipts, example_big_messages, example_budgets, example_channels, example_config_env.

Follow-up, same binaries, not via tools/run_examples.py, because that script returns on the first failure: hello, reliability, stats, transports, and soak --clients 64. Each EXIT=0 and last line PASS.

DEV=OFF, clean build dir, `make -j4 ROCKETNET_DEV=OFF`:

- DEV_OFF_EXIT=0 DEV_OFF_WARNINGS=0 (grep -c 'warning:' was 0).
- CMake cache line: `ROCKETNET_DEV:BOOL=OFF`. The cache also has the comment `// ROCKETNET_DEV=1 (OFF for production)`; the BOOL value is OFF.

macOS, this job directory, make -j4. No make was running in rn-no-alloc-after-create.w6~1 (other makes were under /private/tmp/rn-alloc-w6-base, a different tree). 0 warning lines.

- MAKE_EXIT=0 MAKE_WARNINGS=0.
- TEST_EXIT=1. One failure, the known one, and no other `check failed` line: `check failed: test_server_stats_current_every_update peak_rss_mb=1164 bound_mb=698`. The test was not edited.
- FUNCTIONAL_EXIT=0. Log ends `*** ALL TESTS PASSED ***`.
- CHECK_EXAMPLES_EXIT=0.
- RUN_EXAMPLES_EXIT=1. Same stop-at-first-failure behavior. FAIL lines:

```
latency client->server frames: min 0 mean 0.92 max 2
latency server->client frames: min 1 mean 1.00 max 2
latency: 1 extra frame(s) (correct order)
FAIL: client->server latency is not the frame order's
FAIL example_game_loop: exited 1
```

Same six PASS lines before the failure as on Linux. Follow-up of the five examples the script did not reach: each EXIT=0 and last line PASS, including soak --clients 64.

## Not done

- Windows. A shared slot was requested at 13:01Z: `winlock --slots 3 --card rn-examples-and-targets-trim.w3`. It stayed in `hold` until 13:37Z and never created the tree or started a build. Exclusive holders were ahead, including rn-examples-macos-windows (timeout 4200) and rn-client-iocp-windows (timeout 3600). The hold was released at 13:37Z so it would not start after this report. Windows build, ./bin/test, functional, and run_examples were not run. Functional and run_examples were not going to be started on Windows anyway: the only host script, ~/wgate-rn.sh, also runs ./bin/functional, and those stay exclusive.
- tools/run_examples.py itself is red on Linux and macOS because of the game_loop check above. The five examples it did not reach were run separately and passed. The check was not loosened.
- docs/roadmap.sexp was not edited. Proposed note only, for the examples-and-targets half of line 435 (the header half is a different card). The row is still:

```
(:feature "The public header carries a large volume of comments, some citing internal design references; the examples are longer than they need to be; the build has many targets. The final deliverable states each rule once in the header, shortens the examples and trims the build targets." :cells (n - -))
```

A note that could be added later, not applied here: examples other than soak.c are shorter (hello.c 99 lines); cmake targets transport_fill and lease_cost are removed and are built by hand. Do not flip the cell on this card alone.

- No pull request. main was not pushed. The sprint branch push is the only push.
