# Code-quality audit — rocketnet at 3550a6f3 (2026-10-24, audit B)

Independent cold audit of `sdk/` (read whole), `sdk/rocketnet.h` (the brief's
`include/rocketnet.h`; no `include/` directory exists at this head), and
`tests/` (read). Code quality only: no findings about licensing, RakNet API
parity, scope, features, or performance targets. Every line number is at
3550a6f33f03126b38f42211cd768f6a42b16e90. Findings are ordered most severe
first (bugs, then risks, then quality). Each finding gives the severity, the
defect in one sentence, the concrete failure or cost in one sentence, and the
smallest fix.

Cite check (coordinator, 2026-10-24, on a Linux x64 bench): every `file:line` below was
re-read at 3550a6f3 and points at the line the finding names. `sdk/` and
`tests/` are byte-identical between 3550a6f3 and main at 73e1ab51, so the
same lines hold on main. Two findings were re-graded on that check: the
connection id wrap (5) needs 2^64 assignments, and the IPv6 truncation path
(6) cannot be reached because the buffer is 256 bytes
(`sdk/rocketnet_layers.h:84`) and the longest bracketed IPv6 address with a
port is 47 characters; both are quality findings, not risks.

## Bugs

1. **bug — sdk/rocketnet_server_transport.cpp:408.** Defect: the test-seam
   reset check dereferences the result of `malloc` after guarding it only
   with `rocketnet_assert( fresh )` (line 409), which compiles to nothing
   when `ROCKETNET_ASSERTS` is off (`sdk/rocketnet_internal.h:314`).
   Failure: in a test-seam build without asserts an exhausted allocator
   makes `rocketnet_ack_endpoint_reset` write through a null pointer and
   crash the suite instead of reporting the un-reset member it exists to
   name. Smallest fix: return the `"rocketnet_server_acks_t.endpoints"`
   member (or false) when `fresh` is null, before the reset call.

## Risks

2. **risk — sdk/rocketnet_window_send.cpp:32.** Defect:
   `rn_window_send_admit` computes `W - count` in `uint64_t` (line 33) while
   the `count <= W` bound that keeps the subtraction from wrapping is
   asserted only. Failure: a future or faulty caller passing `count > W` in
   a build without asserts wraps the bound to a huge value, admits numbers
   past the window, and overwrites live `send_numbers` entries. Smallest
   fix: add a runtime `if ( count < 1 || count > W ) return 0;` beside the
   assert.

3. **risk — sdk/rocketnet_client_slots.cpp:294.** Defect: slot release
   records a failed chain search only in `found`, checks it only with
   `rocketnet_assert`, then unconditionally pushes the index onto the free
   stack (line 308) and decrements `num_connected` (line 310). Failure: any
   chain corruption in a build without asserts is silently converted into
   a double-handed slot, so two clients can share one slot's keys and
   mappings. Smallest fix: skip the free-stack push and the `num_connected`
   decrement when `found` is zero, and count the refusal.

4. **risk — sdk/rocketnet_base64.cpp:63.** Defect: both base64 entry points
   narrow a `size_t` output length to `int` with no bound check (encode
   here, `rocketnet_base64_decode_data`'s `return int( output_length )` at line 153). Failure: an input
   large enough to encode or decode past `INT_MAX` bytes wraps the returned
   length negative or small, so a caller trusting it under-reads its own
   buffer. Smallest fix: refuse with the existing `-1`/`0` result when the
   computed length exceeds `INT_MAX` before encoding or decoding.

## Quality

5. **quality — sdk/rocketnet_server_protocol.cpp:33.** Defect: connection
   ids are issued with a bare pre-increment of a `uint64_t` and no skip of
   zero, unlike the client receipt serial, which explicitly skips zero
   (`sdk/rocketnet_endpoint.cpp:330`). Cost: the zero-means-free-slot
   invariant rests on a wrap that cannot happen in practice rather than on
   the code, and the two id issuers read differently for the same rule.
   Smallest fix: advance past zero after the increment, as
   `rn_endpoint_finish` does for serials.

6. **quality — sdk/rocketnet_address.cpp:120.** Defect: the IPv6
   truncation branch in `rocketnet_address_to_string` logs an error and
   returns the buffer anyway, and the branch is unreachable (256-byte
   buffer, at most 47 characters written). Cost: dead error handling that,
   if the buffer ever shrank, would hand back a cut-off address with no
   failure signal. Smallest fix: write `"NONE"` (the spelling the same
   function uses at line 154) on truncation, or replace the branch with a
   compile-time bound on the buffer size.

7. **quality — sdk/rocketnet_protocol.cpp:35.** Defect: the comment "maps a
   refusal code to its counter, so both receive paths attribute the reason
   identically" sits above `rocketnet_keyed_filter_value`, which it does not
   describe; the function it describes is `rocketnet_prefilter_counter`
   (line 69). Cost: a reader maintaining the filter value trusts a comment
   that belongs to another function and must re-derive both. Smallest fix:
   move the two comment lines to directly above `rocketnet_prefilter_counter`.

8. **quality — sdk/rocketnet_api.cpp:176.** Defect: the development
   simulator's env parser uses bare `strtol`, accepting leading whitespace
   and `+` spellings that the config system's strict charset parser
   (`rocketnet_config_numeric_charset`, `sdk/rocketnet_config.cpp:396`)
   refuses for the same kind of value. Cost:
   `ROCKETNET_DEV_CLIENT_LATENCY_MS=" 5"` is accepted here while the same
   spelling is refused as a config env override, so one operator input has
   two meanings. Smallest fix: run the same digits-only charset check before
   `strtol`, or call the config parser.

9. **quality — sdk/rocketnet_slots.cpp:19.** Defect: `rn_slots_init`
   silently clamps a negative or over-maximum `max_clients` instead of
   refusing. Cost: a caller passing a wrong count gets a working machine
   of the wrong size and learns nothing, so the sizing bug surfaces later
   as discarded batch entries. Smallest fix: assert the range in debug and
   treat an out-of-range count as zero slots (every observe/admit already
   refuses) rather than substituting `ROCKETNET_MAX_CLIENTS`.

10. **quality — tests/test_unit_registry.h:191.** Defect:
    `rocketnet_unit_tests_run`, the whole-list runner, is defined and never
    called anywhere in the repository. Cost: dead harness code invites a
    future suite path that runs tests through an unmaintained entry point
    whose expected-fail returns it discards. Smallest fix: delete the
    function, since `test.cpp` runs every test through
    `rocketnet_unit_tests_run_named` in worker processes.

11. **quality — sdk/rocketnet_address.cpp:105.** Defect:
    `rocketnet_address_to_string` and
    `rocketnet_address_to_string_without_port` (line 159) duplicate the
    IPv6 network-order conversion and formatting block nearly verbatim.
    Cost: a fix to IPv6 formatting must be made twice, and the two
    spellings of the same address can drift apart. Smallest fix: format the
    port-less string in one helper and let the port form append `:%hu` (or
    the bracketed form) to it.

12. **quality — sdk/rocketnet_counters.cpp:18.** Defect:
    `rocketnet_counter_value` checks `counters` for null twice, once through
    `rocketnet_api_null_guard` (line 16, which already returns) and again in
    the ternary that follows. Cost: the redundant check teaches readers that
    the guard does not guarantee non-null, weakening the one-guard
    convention every other entry point relies on. Smallest fix: drop the
    `counters &&` term.
