# Ack block ESBMC results

Status: incomplete. `proofs-ack_block` is a nightly-only target, but the full receive ceiling and accepted-block apply path are not proved. Do not describe this target as a successful full-bound proof.

The shared shim imports `std::isfinite` into global lookup because this checker's C++ math header exposes only the namespaced form while the actual `sdk/rocketnet_ack.cpp:241` calls the global form. This is a compiler compatibility mapping, not an SDK rewrite.

The checker is ESBMC 8.5.0 on Linux, with `--std c++17 --z3 --overflow-check` and its default pointer, bounds, division-by-zero, and unwinding checks enabled. Each run includes the actual `sdk/rocketnet_protocol.cpp` and `sdk/rocketnet_ack.cpp`, uses the shared shim, and compiles a temporary `.cpp` copy of the required `.c` harness because this ESBMC release chooses C from the `.c` extension even when `--std c++17` is passed. Runs use a single CPU, `nice -n 19`, `prlimit --as=8589934592`, and an outer timeout. The harness bounds fixed-size `memset` objects itself; no `--memset-limit` option is present in this checker.

| Harness | Domain and unwind | Result |
| --- | --- | --- |
| `harness_ack_read` | Exact allocated input size 0 separately, then symbolic 1..512 bytes; `highest_sent` arbitrary 64-bit; unwind 128 | Proved 79/79, 0 failed. The block length and range count stay within the input object and decoded arrays. |
| `harness_ack_reconstruct` | Arbitrary 32-bit wire and 64-bit highest sent; unwind 2 | Proved 8/8, 0 failed, including low 32-bit round trip. |
| `harness_ack_write` | `num_ranges` 0..255, exact output capacity `7 + 4*num_ranges` (maximum 1027), first length 0..65535; unwind 256 | Proved 77/77, 0 failed, including the actual writes staying inside exact capacity. |
| `harness_ack_build` | Arbitrary replay bits for sequences 1..8, highest opened 0..8, exact output capacity 7..1472; unwind 9 | Proved 91/91, 0 failed. Larger replay histories are unproved. |
| `harness_ack_apply` | Reader-accepted 7-byte block, one recorded sent sequence and a one-element ack output; unwind 4 | Not proved: ESBMC exhausted the 8 GiB bound before property results. Two and eight sent sequences also exhausted the 8 GiB bound. The full range/run path is unproved. |

The exact-size reader at 768, 1024, and the full 1472 bytes reached Z3 but ended with `ERROR: SMT solver failed`, not a property verdict. The full run generated 19,838 VCCs, 4,386 after simplification, and spent 84.859 seconds in the decision procedure before that error. The 768 and 1024 runs spent 96.955 and 92.637 seconds respectively before the same error. Completed reader bounds at 64, 128, 256, and 512 bytes each passed 79/79; 512 is the largest completed bound. This is a resource/tool limit, not an SDK counterexample. No failure is labeled as a source defect without a concrete refutation.

The input is actually symbolic: a gate-only canary asserting `data[0] == 0` after the reader's exact-size allocation failed on a reachable path (one failed property). The canary is absent from the committed harness. The unchanged per-commit `all` target remains outside this nightly proof target; no full `all` gate is claimed here.

Raw logs: `/home/nova/stella-jobs/resumed-1508-gates/rocketnet-cbmc-ack-block-read.w3/logs/` on the allocated Linux gate, notably `read-512.log`, `read-full.log`, `read-768.log`, `read-1024.log`, `reconstruct-cpp2.log`, `write-full.log`, `build-8-optimized.log`, `apply-1.log`, `apply-2.log`, `apply-8.log`, and `read-canary.log`.

## Exact-size reader partition staged in the next attempt

This is a proposed decomposition, **not a new proof result**. The default nightly bounds above remain unchanged. The harness now accepts compile-time bounds for an independent pilot:

- Size 0 is invoked separately with a valid pointer; sizes 1..6 form the short-input shard. The SDK returns before reading a header.
- For sizes 7..1472, split the actual count byte `data[4]` into the 16 disjoint bands `[0,15]`, `[16,31]`, ..., `[240,255]`. Each run retains a symbolic exact-size `malloc(data_bytes)` buffer, all other bytes symbolic, and arbitrary `highest_sent`. The byte-four pre-read is safe because this domain has at least seven bytes; `rocketnet_ack_block_read` reads the same byte after the four-byte wire largest field.
- Within each band, split on `7 + 4*count > data_bytes` and `<= data_bytes`. The first branch returns before the range loop; the second enters the actual SDK range-validation path. The count-zero rejecting subcase is infeasible and is not claimed. The accepting branch of every band has a witness at `data_bytes = 7 + 4*count <= 1027`. For the high pilot, count 240 and size 967 are a witness; setting the first run to one and each subsequent gap/run to one gives a valid accepted block when largest is encoded as 481 and `highest_sent` is 481.

The bands plus short-input shard cover every input length 0..1472 and every count byte. A full-domain claim requires a completed verdict for **each** feasible shard with unwinding assertions enabled. The high-count contained-geometry pilot uses `ACK_READ_MIN_BYTES=7`, `ACK_READ_MAX_BYTES=1472`, `ACK_READ_COUNT_LO=240`, `ACK_READ_COUNT_HI=255`, `ACK_READ_GEOMETRY_CASE=2`, and unwind 256. These constraints preserve the full input length range within the shard and do not constrain arbitrary payload bytes or highest sent. The prior failed 1472-byte run is not converted into a success by this plan.

The one w4 pilot was prepared and syntax-checked, but its guarded invocation returned exit 75 before starting ESBMC: `Gate deferred: load1=31.42 exceeds 16.00`. It has no property result. The command and deferral are recorded in the allocated gate job's `logs/read-high-contained.log`.
