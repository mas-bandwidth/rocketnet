# Sequence-buffer ESBMC results

ESBMC 8.5.0 on one Linux bench core. Every run used CPU 5 at nice 19, Z3, an 8 GiB address-space limit and an explicit wall timeout. The exact command flags were `--std c++17 --z3 --overflow-check -DROCKETNET_ESBMC -DNDEBUG -I../../sdk -I.`, plus the per-harness unwind below. ESBMC treats the card's required `.c` source as C, so the target copies it to a same-directory temporary `.cpp` for C++17 parsing and removes that file on exit.

The harness includes the actual `sdk/rocketnet_replay.cpp`, `sdk/rocketnet_ack.cpp`, and `sdk/rocketnet_window_receive.cpp`. It models only the platform lock/atomic primitives as sequential operations. The SDK index calculations and record/window reads and writes are unchanged. The shared `proofs/cbmc/esbmc_shim.h` is the canonical byte-identical shim (SHA-256 `1ddf4737dfe66c06ca05f29787a5c6e294bac3d4d58b606a2d59e347196f58fc`).

| harness | result | properties | full bound | unwind | evidence |
| --- | --- | ---: | --- | ---: | --- |
| `harness_replay_already_received` | proved | 21/21 | every nonzero `uint64_t` sequence and arbitrary 4096-bit replay window/high-water | 65 | `replay-already.log` |
| `harness_replay_advance` | resource-limited, unproved | no property result | same full sequence/state claim; near advance requires at most 4096 clear iterations | 4097 | 8 GiB exhausted while unwinding iteration 2006; `replay-advance.log` |
| `harness_ack_record_received` | timed out, unproved | 14 VCCs remained after simplification | every nonzero `uint64_t` sequence against the 2048-entry received ring | 2 | reached Z3; stopped after 300 seconds; `ack-record-received.log` |
| `harness_ack_record_sent_classified` | timed out, unproved | 43 VCCs remained after simplification | every nonzero `uint64_t` sequence against the 4096-entry sent ring and 64-word shadow bitmaps | 2 | reached Z3; stopped after 300 seconds; `ack-record-sent-v3.log` |
| `harness_ack_mark_keep_alive` | proved | 20/20 | every nonzero `uint64_t` sequence against the 4096-entry sent ring | 2 | `ack-keepalive.log` |
| `harness_window_receive_init` | proved | 39/39 | every first number; every power-of-two W from 64 through 1,048,576 | 2 | `window-init.log` |
| `harness_window_receive_accept` | resource-limited, unproved | no property result | every wire u24 number and arbitrary receiver state for every power-of-two W from 64 through 1,048,576 | 1,048,577 | 8 GiB exhausted while unwinding iteration 6302; `window-accept.log` |
| `harness_window_receive_release` | proved | 50/50 | every `uint64_t` number for every power-of-two W from 64 through 1,048,576 | 2 | `window-release.log` |

Four harnesses prove all 130 reported properties. No property was refuted and no SDK counterexample was produced. Four full claims remain unproved exactly as named above; no smaller bound is presented as the full claim.

The committed nightly target uses the required 30-minute maximum per harness and prints one truthful result line for every harness, continuing after a failed, timed-out, or resource-limited member. The focused exploratory evidence used a five-minute cap for the two solver timeouts; both resource-exhaustion runs ended earlier under the required 8 GiB limit. Raw logs are under `/home/nova/stella-jobs/resumed-1508-gates/rocketnet-cbmc-sequence-buffers.w2/repo/evidence/`.
