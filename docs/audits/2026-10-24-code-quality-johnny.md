# Code-quality audit — rocketnet at 3550a6f3 (2026-10-24, johnny)

Independent cold audit of `sdk/` (read whole), `sdk/rocketnet.h` (the brief's
`include/rocketnet.h`; no `include/` directory exists at this head), and
`tests/` (read). Code quality only: no findings about licensing, RakNet API
parity, scope, features, or performance targets. Every line number is at
3550a6f33f03126b38f42211cd768f6a42b16e90. Findings are ordered most severe
first. Each finding gives the severity, the defect in one sentence, the
concrete failure or cost in one sentence, and the smallest fix.

1. **bug — sdk/rocketnet_server_transport.cpp:408.** Defect: the test-seam
   reset check dereferences the result of `malloc` after guarding it only
   with `rocketnet_assert( fresh )`, which compiles to nothing under
   `NDEBUG`. Failure: in a release test-seam build an exhausted allocator
   makes `rocketnet_ack_endpoint_reset` memset through a null pointer and
   crash the suite instead of reporting the un-reset member it exists to
   name. Smallest fix: return the `"rocketnet_server_acks_t.endpoints"`
   member (or false) when `fresh` is null, before the reset call.

2. **risk — sdk/rocketnet_window_send.cpp:32.** Defect:
   `rn_window_send_admit` computes `W - count` in `uint64_t` while the
   `count <= W` bound that keeps the subtraction from wrapping is asserted
   only. Failure: a future or faulty caller passing `count > W` in a
   release build wraps the bound to a huge value, admits numbers past the
   window, and overwrites live `send_numbers` entries. Smallest fix: add a
   runtime `if ( count < 1 || count > W ) return 0;` beside the assert.

3. **risk — sdk/rocketnet_client_slots.cpp:294.** Defect: slot unlink
   records a failed chain search only in `found`, checks it only with
   `rocketnet_assert`, then unconditionally pushes the index onto the free
   stack. Failure: any chain corruption in a release build is silently
   converted into a double-handed slot, so two clients share one slot's
   keys and mappings. Smallest fix: skip the free-stack push and the
   `num_connected` decrement when `found` is zero, and count the refusal.

4. **risk — sdk/rocketnet_base64.cpp:63.** Defect: both base64 entry points
   narrow a `size_t` output length to `int` with no bound check. Failure: an
   input large enough to encode past `INT_MAX` bytes wraps the returned
   length negative or small, so a caller trusting it under-reads its own
   buffer. Smallest fix: refuse with the existing `-1`/`0` result when the
   computed length exceeds `INT_MAX` before encoding or decoding.

5. **risk — sdk/rocketnet_server_protocol.cpp:33.** Defect: connection ids
   are issued with a bare pre-increment and no skip of zero, unlike the
   client receipt serial, which explicitly skips zero. Failure: after
   2^64 assignments the counter wraps to 0, the value the slots machine
   reads as "free slot", so a live connection's batch entries are discarded
   as unbound and a later wrap reissues live ids. Smallest fix: advance
   past zero after the increment, as `rn_endpoint_finish` does for serials.

6. **risk — sdk/rocketnet_address.cpp:120.** Defect: on IPv6 formatting
   truncation `rocketnet_address_to_string` logs an error but still returns
   the truncated buffer with no failure signal distinct from success.
   Failure: a caller logging or dialing the result silently uses a cut-off
   address that names no host. Smallest fix: write `"NONE"` (the existing
   failure spelling in the same function) into the buffer on truncation.

7. **quality — sdk/rocketnet_protocol.cpp:35.** Defect: the comment "maps a
   refusal code to its counter, so both receive paths attribute the reason
   identically" sits above `rocketnet_keyed_filter_value`, which it does not
   describe; the function it describes is `rocketnet_prefilter_counter`
   below. Failure: a reader maintaining the filter value trusts a comment
   that belongs to another function and must re-derive both. Smallest fix:
   move the two comment lines to directly above `rocketnet_prefilter_counter`.

8. **quality — sdk/rocketnet_api.cpp:176.** Defect: the development
   simulator's env parser uses bare `strtol`, accepting leading whitespace
   and `+` spellings that the config system's strict charset parser
   (`rocketnet_config_numeric_charset`) refuses for the same kind of value.
   Failure: `ROCKETNET_DEV_CLIENT_LATENCY_MS=" 5"` is accepted here and
   refused as an env override there, so one operator input has two
   meanings. Smallest fix: run the same digits-only charset check before
   `strtol`, or call the config parser.

9. **quality — sdk/rocketnet_slots.cpp:19.** Defect: `rn_slots_init`
   silently clamps a negative or over-maximum `max_clients` instead of
   refusing, against the SDK's refuse-loudly-never-clamp rule for config.
   Failure: a caller passing a wrong count gets a working machine of the
   wrong size and learns nothing, so the sizing bug surfaces later as
   discarded batch entries. Smallest fix: assert the range in debug and
   treat an out-of-range count as zero slots (every observe/admit already
   refuses) rather than substituting `ROCKETNET_MAX_CLIENTS`.

10. **quality — tests/test_unit_registry.h:191.** Defect:
    `rocketnet_unit_tests_run`, the whole-list runner, is defined and never
    called anywhere in `sdk/` or `tests/`. Failure: dead harness code
    invites a future suite path that runs tests through an unmaintained
    entry point whose expected-fail returns it discards. Smallest fix:
    delete the function, since `test.cpp` runs every test through
    `rocketnet_unit_tests_run_named` in worker processes.

11. **quality — sdk/rocketnet_address.cpp:105.** Defect:
    `rocketnet_address_to_string` and
    `rocketnet_address_to_string_without_port` duplicate the IPv6
    network-order conversion and formatting block nearly verbatim.
    Failure: a fix to IPv6 formatting must be made twice, and the two
    spellings of the same address can drift apart. Smallest fix: format the
    port-less string in one helper and let the port form append `:%hu` (or
    the bracketed form) to it.

12. **quality — sdk/rocketnet_counters.cpp:18.** Defect:
    `rocketnet_counter_value` checks `counters` for null twice, once through
    `rocketnet_api_null_guard` (which already returns) and again in the
    ternary that follows. Failure: the redundant check teaches readers that
    the guard does not guarantee non-null, weakening the one-guard
    convention every other entry point relies on. Smallest fix: drop the
    `counters &&` term.
