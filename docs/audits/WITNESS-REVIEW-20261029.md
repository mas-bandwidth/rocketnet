# Standard witness follow-up, 2026-10-29

This follows the [standard review](STANDARD-REVIEW-20261029.md). Its 28 open
rows were an inventory of missing witnesses, not 28 demonstrated SDK defects.
Emma's saved witness work (`735c163092`) supplies 25 protocol regression cases;
this integration corrects their scope, exercises configurable replay depths,
and adds direct observation of live key-schedule buffers. The current
[ledger](../WITNESSES.tsv) distinguishes executable witnesses, source review,
and a deferred protocol mode. A passing anchor checker does not execute tests.

## Executable evidence

The added contract cases cover key lifetime, per-mapping ephemeral keys,
cookie storage and pacing, denial keys, replay depth, history pressure, keyed
lookup behavior, refusal counters, ACK truthfulness, send-rate stability,
sequence continuity, end reasons, token expiry and eviction, and keep-alive
confirmation. They use explicit protocol time and in-memory exchanges.
The earlier tests for ordered refusal selection, activity refresh, and server
keep-alive pacing are now linked from their previously empty ledger rows.

`test_key_schedule_erases_live_stack_buffers` compiles the actual
[SDK key schedule](../../sdk/rocketnet_keys.cpp) with renamed entry points and
an observing wipe wrapper. The wrapper invokes the real sodium wipe and checks
bytes while their stack buffers are alive. Both endpoint roles must wipe the
32-byte shared secret, its 112-byte concatenated copy, and all four KDF scratch
buffers; the four derived keys must already be available. This addresses the
old output-only test's blind spot. It does not inspect dead stack storage or
claim a hardware remanence proof.

The retained-depth test uses the configured history size, including its oldest
retained and first expired sequence. It no longer assumes every supported
build uses the default 4096 entries. The capacity-policy tests exercise small
tables; their configured fixture sizes do not independently prove the SDK's
default ratios. Those defaults remain source facts: server transport allocates
two mappings and eight used-token entries per configured client, and used-token
history evicts its oldest ticket. Replay history defaults to 4096, with the
supported 4096–32768 range set by the build and pinned by [RN-1 constants](../../standard/TRANSPORT.md#t3).

<a id="lookup_complexity"></a>
## Lookup complexity: source review

The previous rationale's unconditional constant-time claim was incorrect.
[Address lookup](../../sdk/rocketnet_encryption_map.cpp) hashes once with a
per-table random SipHash key, then probes a bounded index. Its index has the
same capacity as the mappings; an absent lookup in a full map can examine
every entry. [Client-id lookup](../../sdk/rocketnet_client_slots.cpp) also uses
a fresh keyed hash and a bounded probe, with twice as many index cells as
client slots. Free slots come from a stack of indices.

These are keyed hash lookups, not a linear search of the client-slot array.
Expected constant cost requires a suitable load factor and hash distribution;
worst-case cost is linear in table capacity. The rationale now says this
explicitly. The populated-table tests and existing collision tests check
lookup correctness and probe bounds; they are not an asymptotic performance
proof. This row therefore carries `review`, not `unit`. No lookup algorithm or
SDK capacity was changed to make a coverage count look complete.

<a id="token_comparison"></a>
## Token identifier equality: source review

The original row pointed at `rocketnet_server_process_opened_request`, although
the rationale's sentence concerns used-token identifiers. The corrected anchor
is [rn_token_index_position](../../sdk/rocketnet_token_tables.cpp). Every live
candidate is compared through `sodium_memcmp` with the full, fixed
`ROCKETNET_TOKEN_IDENTIFIER_BYTES` length of 16. The vendored
[sodium implementation](../../sodium/sodium.c) accumulates XOR differences over
all bytes into a volatile accumulator and returns only after the length-driven
loop; it has no mismatch-dependent early exit. The separate bit-flip regression
checks that every identifier bit affects equality.

This is source evidence for use of the constant-time primitive. It is not
instruction-level timing qualification of every compiler, target, or externally
supplied sodium library. Hash probing and request refusal order are deliberately
outside this fixed-size equality claim. The ledger labels the evidence `review`
and prints it separately from executable coverage.

## Deferred mode: keyed filter epochs

[RN-1 t5.1](../../standard/TRANSPORT.md#t5.1) requires a constant-time comparison
of a computed MAC only when keyed epochs are enabled. The current SDK supports
unkeyed epoch zero and rejects every nonzero epoch. A new boundary test checks
all 255 unsupported epochs. The keyed-MAC rule remains visible as `deferred`;
it is not called implemented, passed, or an unkeyed-filter bug. Adding the
backend magic distribution and keyed mode would broaden SDK scope and is not
part of this repair.

## Validation

The full Linux Debug suite passes with the no-socket trace gate. The full Linux
production suite passes with an 8192-entry history. That second run exposed a
pre-existing vector-fixture assumption: 4096-entry published vectors were
calling the build-depth implementation. The fixture now compiles private
copies of the actual replay source at the vector depths; it does not change
the linked SDK or the published vector bytes.

Three negative controls are rejected by the stack-wipe test: deleting the
shared-secret wipe, deleting the concatenated-input wipe, and making the
observed wipe a no-op. Restoring the source passes. The standard checker still
reports 220 pins and 166 vectors; all eleven witness-checker regressions pass.
Platform CI provides the additional compiler and sanitizer checks.

## Reading the result

Zero `none` rows means every inventoried rule has a test or an explicit
review/deferred disposition. It does not mean zero confidence limits: the two
source-review rows and the unimplemented keyed mode remain separately counted.
The ledger is also a partial inventory of the standard, not a completeness
proof for all normative sentences.
