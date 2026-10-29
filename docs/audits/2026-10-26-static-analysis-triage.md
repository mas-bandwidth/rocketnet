# Static-analysis zero-unresolved triage — 2026-10-26

## Verdict

The current full GCC analyzer, clang-tidy and cppcheck inputs have **zero
unresolved diagnostics** after source review. This is a source-triage claim,
not a claim that every tool printed zero diagnostics or that every comparison
runner exited zero.

Five demonstrated defects were repaired:

- the XDP pre-flight no longer parses an empty or malformed IRQ affinity as
  CPU 0;
- the XDP driver adapter no longer carries a redundant NULL check after three
  reads that already require the same transport config;
- three assignments whose values were never read were removed.

The remaining 25 newly reported findings are individually dispositioned below.
They were not added to a baseline, suppressed, or hidden by disabling a check.
The baseline changes are deletion-only: 57 stale clang-tidy entries and three
stale cppcheck entries were removed. The GCC baseline remains empty.

## Reproduction and retained evidence

The staged source was
`ef0229e87240cadf6e3b2376d2f72782d5fd7c97` on
`sprint/rn-static-analysis-zero-unresolved.w1.g1.e1`. Analysis and builds ran
on an allocated Linux x64 bench, one `benchslot` at a time and at most
four compile/analyzer jobs. Tool versions were:

- GCC/G++ 13.3.0;
- Clang/clang-tidy 18.1.3;
- cppcheck 2.13.0;
- CMake 3.28.3;
- Python 3.12.3.

Complete raw logs are retained outside the repository at
`private-evidence/rn-static-analysis-zero-unresolved-w1-g1/`. The final
analysis logs have SHA-256:

- `run-static.log`: `7de6ed6a7da3014f671536e7a0ba85ab77e02a65bce11e47802b799b1ee72ac7`;
- `run-tidy.log`: `380fc3e2fe782972852268232f9683ae2ef740b74e09f12c4ffa8b99e85304b0`;
- `exit-codes.log`: `b9e709d33e66f9a256e1927a44f9b3363e2f95d50e82205d64fecdf1ed7fabd8`.

The final full-input results were:

| runner | current | baseline | exit | interpretation |
|---|---:|---:|---:|---|
| `tools/run_static.py` | 0 | 0 | 0 | pass |
| clang-tidy half of `tools/run_tidy.py` | 357 | 333 | 1 | 24 reviewed, unbaselined findings below |
| cppcheck half of `tools/run_tidy.py` | 41 | 40 | 1 | one reviewed, unbaselined finding below |

`tools/run_tidy.py` therefore correctly remains red: the policy forbids
expanding the baseline with reviewed false positives. A red comparison runner
is reported separately from source clearance; it is not relabeled as a pass.
No translation unit was skipped or unparsed and no tool was missing in this
run.

## Profile change

The existing `.clang-tidy` runner now also enables the stable correctness
groups `clang-analyzer-core.*`, `clang-analyzer-cplusplus.*`,
`clang-analyzer-deadcode.*`, `clang-analyzer-nullability.*`,
`clang-analyzer-security.*` and `clang-analyzer-unix.*`. The pre-existing
bugprone, CERT, concurrency and performance families remain enabled, with their
two existing named style exclusions unchanged. No runner was added or changed.

A separate broad `clang-analyzer-*` probe found six
`clang-analyzer-optin.performance.Padding` suggestions:

- `rocketnet_client_protocol.h` `36ceca985f721b67`;
- `rocketnet_config.h` `c30959b17b74dd08`;
- `rocketnet_packet_builder.h` `f3989772e9b5f52e`;
- `rocketnet_tests.cpp` `9c233594313187d1`, `a03a3af581b77712`,
  `de5bf97c9d66b723`.

Those are recorded as opt-in ABI/layout advice, not correctness diagnostics.
Changing public or hot-path layouts without a demonstrated defect is outside
this gate. Their transparent exclusion from the selected stable groups is not
a suppression, and this report does not call them absent.

## Source repairs

### IRQ affinity parsing

`rocketnet_server_socket_linux.cpp` previously used `atoi(affinity)`. The
sysfs reader returns success with an empty string when the file is empty, so an
empty or nonnumeric value became CPU 0 and produced a misleading pin warning.
The pre-flight now uses `strtol`, accepts only a consumed integer followed by
whitespace, checks `ERANGE` and the `int` range, and takes the existing
unreadable-affinity refusal path on malformed input. The pre-existing `-`/`,`
multi-CPU/range refusal remains unchanged, as does the later checkable-CPU
range diagnostic.

The other two `cert-err34-c` findings in this file are not defects:
`sscanf` parses the kernel-written IRQ prefix and checks for exactly one
conversion, while `ROCKETNET_TEST_XDP_FAIL_AT_QUEUE` is a development-only
fault-injection variable.

### XDP config contract

In `rocketnet_driver_builtin.cpp`, `xdp_create` reads
`config->transport->num_xdp_queues`, `num_os_cpus` and `packet_filter`
unconditionally. Its sole producer constructs the adapter config with the
effective server config after defaulting, environment overlay and validation.
The intervening `config->transport &&` on `second_public_address` contradicted
that contract and produced two cppcheck `nullPointerRedundantCheck` findings.
Only the redundant conjunct was removed.

### Dead stores

- `rocketnet_platform.cpp`: `sent = count` immediately before `break`;
- `rocketnet_tests.cpp`: `before = r` before a check that uses `buffer + 8`
  directly;
- `rocketnet_tests.cpp`: `sequence = 0` before calls that use literal packet
  numbers.

No guard, array zeroing, queue initialization or hot-path state was added only
to appease an analyzer.

## New clang-tidy findings: individual dispositions

| file/check/hash | disposition and source evidence |
|---|---|
| `rocketnet_client_slots.cpp` `bugprone-sizeof-expression` `0145a1d6f7b1c0a2` | The zero-state inventory intentionally measures the member itself. A pointer member's pointer size is the required inventory size. |
| same file/check `da259fdbb7d9eb46` | The reset inventory uses the same intentional member-size rule. |
| `rocketnet_driver_builtin.cpp` `bugprone-branch-clone` `9c896482b4cf3929` | `ROCKETNET_DEFAULT_MAX_CLIENTS` is `min(ROCKETNET_MAX_CLIENTS,1000)`; its arms are equal only in the default 1000-client build. |
| same file/check `df4b6cd0579b23fd` | The second adapter uses the same configuration-dependent default expression. |
| `rocketnet_message_server.cpp` `clang-analyzer-core.NullDereference` `9e6a419dab054bf4` | A successfully created message object has at least one connection, allocates the one-block layout, assigns `object->connections`, and assigns every `connection->hot` before return. The analyzer loses create provenance. |
| `rocketnet_platform.cpp` `clang-analyzer-core.uninitialized.ArraySubscript` `bee3f17f080e4c35` | `map[count]` is written before `count++`; the EAGAIN tail reads only indices in `[sent,count)`. |
| same file/check `cee9c7bbfc156a46` | The single-drop path reads `map[sent]` only while `sent < count`, under the same write-before-publish invariant. |
| `rocketnet_server_gso_linux.cpp` `clang-analyzer-core.CallAndMessage` `be4ea0ef46b62ff8` | Each address, message and packet-info array element is written at `num_messages` before the count is incremented; sends read only below that count. |
| same file `clang-analyzer-core.uninitialized.Branch` `2738466355939b72` | `sendmmsg` cannot return more than the requested `vlen`, and the sent tail stays within the initialized count. |
| `rocketnet_server_transport.cpp` `cert-err33-c` `944463e5f85baa2a` | The ignored `snprintf` return is bounded best-effort diagnostic-name collection. Truncation remains NUL-safe and changes no protocol behavior. |
| same file `clang-analyzer-core.CallAndMessage` `63d75dab0ed93b0a` | A frame's `packet_id` is initialized exactly when the immutable backend dispatch is staged; commit checks that same staged predicate. The analyzer mixes a nonstaged reserve with a staged commit. |
| same file/check `cc2e6d2f4e4b18ff` | Same immutable-dispatch invariant for the second commit use. |
| same file/check `e3a4dfe675c1addc` | Same immutable-dispatch invariant for abort. |
| `rocketnet_tests.cpp` `clang-analyzer-security.FloatLoopCounter` `03238a92bb2cdcfc` | Synthetic-clock test loop; the floating bound is a time deadline, not an iteration count. |
| same file/check `2d5362edbbd2b319` | Same bounded simulated-time pattern. |
| same file/check `3f4f072b19b30f81` | Same bounded simulated-time pattern. |
| `rocketnet_api.cpp` `bugprone-sizeof-expression` `1fbaa9740626a1a7` | `RN_PUBLIC_FIELD_END` measures the `loopback_hub` member itself: the field ends at its offset plus the pointer's size, the intended size of a pointer member. |
| same file/check `f138bd42500c79b5` | The other config's `loopback_hub` row of the same field-end table. |
| `rocketnet_config.cpp` `bugprone-sizeof-expression` `3b3ff0baf682cfb6` | `ROCKETNET_CONFIG_LAYOUT_FIELD` asserts that the public and table `loopback_hub` members have equal size; both are the same pointer type. |
| same file/check `fe2f43fd884d7ca0` | The other config's `loopback_hub` layout assertion. |
| `rocketnet_tests.cpp` `bugprone-sizeof-expression` `91e27749171958c5` | `RN_TEST_CONFIG_FIELD` records each public config member's offset and size for the layout audit; a pointer member's size is the pointer's. |
| same file/check `f18a1a07fe6b6bf7` | The other config's `loopback_hub` row of the same audit table. |
| same file/check `4f8411d8cac2b264` | Same bounded simulated-time pattern. |
| same file/check `6252e33596eeb478` | Same bounded simulated-time pattern. |
| same file/check `7818c863b1f21bf8` | Same bounded simulated-time pattern. |
| same file/check `948254227829b33f` | Same bounded simulated-time pattern. |
| same file/check `9a4839554ad4c15e` | Same bounded simulated-time pattern. |
| same file/check `a7b75ef1df529880` | Same bounded simulated-time pattern. |
| same file/check `d8f29d0de91a35ed` | Same bounded simulated-time pattern. |

## New cppcheck finding

`rocketnet_send_handoff.h` `uninitMemberVar` `98f3f42ddded939d`
is not a read of uninitialized storage. `queued[]` is private and becomes
visible only through `count`; `publish` writes the selected slot before
incrementing `count`, and `front`, `reopen` and `retire` read only a slot made
visible by that count. Zero-initializing the whole queue would add behavior
cost without changing the invariant.

`rocketnet_server_uring_linux.cpp` `preprocessorErrorDirective`
`c208577b92a8d27b` is the intended refusal of a hand build that defines
`ROCKETNET_IO_URING` against a liburing without `IORING_RECV_MULTISHOT`
(liburing before 2.3, as on Ubuntu 22.04). cppcheck does not read the system
`liburing.h`, so it evaluates the configuration in which that macro is absent
and reports the `#error`. Configure checks the macro and leaves
`ROCKETNET_IO_URING` unset in that case (cmake/RocketNetDependencies.cmake),
so no CMake build reaches the directive.

## Baseline shrinkage

The clang-tidy baseline shrank from 390 to 333 findings. The removed entries
were exactly those the runner reported stale, including the repaired affinity
conversion and older diagnostics displaced by current source. The cppcheck
baseline shrank from 43 to 40: the historical XDP frame `ctunullpointer`
entry disappeared without a source change, and the two repaired driver NULL
contract entries disappeared. The XDP item is recorded as a stale-baseline
removal, not misrepresented as a source fix.

## Build and unit gates

Strict warnings and warnings-as-errors are enabled by default. Four complete
Release builds and their full registered unit suites passed on the allocated
Linux bench:

| compiler | `ROCKETNET_DEV` | configure | full build | `./bin/test` |
|---|---:|---:|---:|---:|
| GCC 13.3.0 | ON | 0 | 0 | 0 |
| GCC 13.3.0 | OFF | 0 | 0 | 0 |
| Clang 18.1.3 | ON | 0 | 0 | 0 |
| Clang 18.1.3 | OFF | 0 | 0 | 0 |

The retained `exit-codes.log` for each leg has SHA-256
`09367f8565f291cb75bd0bc7cf8f63bd73ef4362f144358d75520bb15ba69e29`;
each raw test log ends `*** ALL TESTS PASSED ***`.

## Independent review and limits

An independent exact-head read agreed that the three dead stores were genuine,
verified each analyzer-path invariant above, and rejected speculative NULL
guards, array zeroing and queue initialization. It also independently recorded
the broad Padding probe as opt-in layout advice. The review did not call the
red tidy comparison runner green.

No public API, capacity, security policy or runtime configuration changed.
