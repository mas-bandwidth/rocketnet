# Sequence buffers

`sdk/rocketnet_sequence_buffer.h` is the internal, allocation-free modulo
table used when a protocol identity selects a fixed-capacity cell. Its identity
and every validation tag are full-width `uint64_t`. Packet identity is already
64-bit internally; U24 message identity is reconstructed to 64-bit before it
reaches this boundary; and a 32-bit payload identity is widened, never narrowed
to a legacy 16-bit sequence-buffer API.

## Policies

`RN_SEQUENCE_BUFFER_HISTORY` retains an identity window ending at the greatest
identity admitted. A newer admission may retire occupied entries below the new
window floor. It scans the capacity, not the numeric gap, so a sparse jump of
one or many laps has the same bounded cost. An identity older than the retained
window is refused before any cell changes.

`RN_SEQUENCE_BUFFER_OWNED` represents live storage: reliable sends, mappings,
payloads, claims, or I/O operations. An occupied modulo cell is a refusal. It
never evicts a live owner merely because a new identity aliases that cell.
Callers must complete or explicitly cancel the owner first.

Every cell has an explicit occupied bit; no identity or payload value is an
empty sentinel. A handle contains the exact identity, cell generation, owner
epoch, and admission ticket. `find` and `remove` validate all four. Removal
clears the occupied bit immediately, so arbitrary cancellation and requeue do
not create tombstones or an unbounded cleanup chain. Double and stale removal
are harmless refusals.

## Exhaustion and reset

Tickets start at one. `UINT64_MAX` is issued once, then admission returns
`RN_SEQUENCE_BUFFER_TICKETS_EXHAUSTED` until `reset`. Cell generation exhaustion
similarly refuses reuse rather than wrapping. Reset is O(capacity): it clears
occupancy and generations, increments the owner epoch, restarts tickets at one,
and allows numeric identities to be reused without reviving old handles. An
owner epoch of `UINT64_MAX` cannot be reset; the containing connection or object
must be reconstructed, preventing an epoch alias.

`initialize` accepts explicit starting owner, ticket, and cell-generation
values for restored state and boundary verification. Zero owner epoch is
normalized to one; zero ticket means exhausted. Normal construction uses owner
one, ticket one, and generation zero.

## Memory and operation bounds

For value type `T` and capacity `N`, cell storage is
`N * sizeof(slot_t)`, exposed as `storage_bytes`; the complete object is that
array plus three `uint64_t` fields, a `size_t`, a `bool`, and implementation
padding. `slot_bytes` exposes the compiler's exact cell size, including padding.
There is no heap allocation or growth.

Owned insert, lookup, and remove are O(1). History lookup and remove are O(1).
History insertion is O(N) when its high-water mark advances because retirement
scans at most N cells, and O(1) otherwise. Reset is O(N). No gap-clear or
compaction operation is described as universally O(1).

## External ownership boundary

This is an internal C++ primitive and adds no public C ABI or installed-header
surface. Traversal supplied by an operating system stays outside it. An
OS-owned enumeration must be consumed in place or copied into bounded RocketNet
storage; its nodes and links must never become RocketNet-owned linked state.

## Owned storage manifest

Every family below once linked its elements to each other, by pointer or by
array index. A row is `replaced` when its storage is now an array, ring,
stack, flat index or sequence buffer whose elements name no other element;
it is `residual` while any link remains. `tools/check_no_owned_linked_lists.py`
proves the source agrees: it fails on any RocketNet-owned link, whatever the
manifest says, so a `residual` row records debt and never waives it. The
source-audit case `test_sequence_buffers_manifest_matches_source`
(`tests/source_audit_sequence_buffers.cpp`) checks that every row names a
replacement symbol present in its file and that no row is `residual`.

Citations are at main `d380b4e5`. Q is the region's descriptor count
(`region->queue`), W its reliable window, C its channel count.

| Scope | Family | Removed links | Replacement | Capacity | Identity | Admission and eviction | Worst case | Status |
|---|---|---|---|---|---|---|---|---|
| sdk | ack received stats | receive-order doubly linked list through record `stats_prev`/`stats_next` | `stats_word` sdk/rocketnet_ack.h:119 (32-record block summaries in the received ring) | `ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE` 2048 records, 64 blocks | exact `uint64_t` packet sequence per record, written bitmap | slot is sequence mod 2048; a lap overwrites the oldest; a sequence at or below highest - 2048 is refused | 256 record or block visits per query | replaced |
| sdk | encryption map | bucket chains and free chain through `next`, `bucket_head`, `free_head` | `free_indices` sdk/rocketnet_encryption_map.cpp:29 (dense LIFO) beside the flat probe `index` :28 | max_clients * 2 mappings | per-slot `uint64_t` generation from a map-wide counter; 0 is exhausted | refuses when no slot is free or generations are exhausted; never evicts; removal wipes secrets and pushes the slot | O(1) stack; probe and backward-shift erase O(capacity) | replaced |
| sdk | client slots | bucket chains through `next` and `bucket_head` | `free_stack` sdk/rocketnet_client_slots.cpp:58 beside the flat `index_table` :50 | max_clients slots, 2 * max_clients index cells | `uint64_t` slot generation checked with the client id | refuses when full or on a duplicate generation; never evicts | O(1) stack; probe O(2 * max_clients) | replaced |
| sdk | token tables | bucket chains, free chain and claim-order doubly linked list | `order_slots` sdk/rocketnet_token_tables.cpp:40 beside `free_slots` :39 and a flat index | max_clients * 8 per table (bindings, claims) | `uint64_t` generation, ticket and owner epoch per slot | bindings take free, else expired, else the oldest ticket; claims take free or an expired oldest, else refuse; a live claim is never evicted | O(1) stack; full-table selection and claim order removal O(capacity) | replaced |
| sdk | packet builder | per-item `next_in_channel` and `next_head` links | `rn_packet_builder_plan_t` sdk/rocketnet_packet_builder.h:102 | `ROCKETNET_PACKET_BUILDER_MAX_ITEMS` 96 items per call | `uint64_t` call epoch with item and group index | a call over 96 items or at an exhausted epoch is refused whole | O(items) build, O(groups) per take | replaced |
| sdk | network simulator | per-shard sorted doubly linked lists | `shard_occupancy` sdk/rocketnet_network_simulator.cpp:185 with `shard_summary` :186 | create argument (server 4096 slots), 64 shards | per-slot `uint64_t` generation | lowest free slot; a full queue or exhausted generation drops the packet | O(1) membership; insert scan O(capacity) | replaced |
| sdk | IOCP queues | interlocked free-send and retry-receive lists and the run index chain | `rocketnet_iocp_ref_queue_t` sdk/rocketnet_server_iocp_windows.cpp:52 and `run_refs` :295 | 2 * max clients rounded to a power of two sends; 32 * 128 retries | `uint64_t` owner epoch and generation (48 bits on the wire) per reference; `uint64_t` run ticket | push refuses when full and the operation is retired; never evicts | one cell per operation, compare-and-swap retries under contention | replaced |
| sdk | channel send FIFO | descriptor `next` sdk/rocketnet_allocator.h:124 with `queue_head`/`queue_tail` :57 | none: appended at sdk/rocketnet_channel.cpp:1183, walked at :919 | Q descriptors | `uint32_t` enqueue ticket and queue epoch beside the link | append; hole cancel; compaction past 64 holes | O(1) append, O(C + queued) compaction | residual |
| sdk | channel ready list | doubly linked `ready_prev`/`ready_next` sdk/rocketnet_allocator.h:59 | none: linked at sdk/rocketnet_channel.cpp:954 and :963, walked at :932 | C channels | `uint32_t` ready ticket and generation beside the link | one membership per channel | O(1) link, O(C) rebase | residual |
| sdk | unreliable send FIFO | descriptor `next` sdk/rocketnet_allocator.h:124 with `unreliable_head` sdk/rocketnet_endpoint.h:88 | none: appended at sdk/rocketnet_endpoint.cpp:401, walked at :1054 | Q descriptors | `uint32_t` enqueue ticket beside the link | append; hole cancel; expiry compaction | O(1) append, O(queued) compaction | residual |
| sdk | resend FIFOs | `rn_resend_link_t` prev/next sdk/rocketnet_allocator.h:214 with `resend_head`/`chunk_head` sdk/rocketnet_window_ack.h:107-111 | none: linked at sdk/rocketnet_window_ack.cpp:148 and :172, walked at sdk/rocketnet_endpoint.cpp:765 and :1387 | W slots | descriptor generation and owner epoch beside the link | push head or tail; named unlink | O(1) push and unlink, O(W) due scan | residual |
| sdk | send descriptor pool | LIFO free list through free descriptors' `span_next`, head `descriptors_free_head` sdk/rocketnet_allocator.h:417 | none: popped at sdk/rocketnet_allocator.cpp:666, pushed at :693 | Q descriptors | `uint32_t` slot generation | refuses when none is free | O(1) | residual |
| sdk | send store chunks | free chain and block chunk chain through the 20-bit next of `meta` (`RN_META_NEXT_MASK` sdk/rocketnet_allocator.cpp:202), head `free_head` sdk/rocketnet_allocator.h:317 | none: popped at sdk/rocketnet_allocator.cpp:318, pushed at :373, freed along the chain at :1096 | per-class chunk count from the store bytes | 10-bit chunk generation in `meta` | refuses when the class is empty | O(1) per chunk, O(fragments) per block | residual |
| sdk | live send spans | doubly linked `span_prev`/`span_next` sdk/rocketnet_allocator.h:139 and :143 | none: linked at sdk/rocketnet_allocator.cpp:874, walked at :851 | Q descriptors | `uint32_t` span ticket mirroring list order | allocation order; reclaim floor at the oldest live span | O(1) link and unlink, O(live) ticket renumber | residual |
| suite | unit registry | registration chain through `next` | `rocketnet_unit_file_registry_t` tests/test_unit_registry.h:51 | `ROCKETNET_UNIT_FILE_TEST_CAPACITY` 2048 cases | case name, unique | front insert; a duplicate or full registry aborts | O(n) insert and duplicate scan | replaced |
| suite | suite RSS bound registry | registration chain through `next` | `SUITE_CASE_RSS_BOUND_CAPACITY` tests/suite_runner.h:194 (`entries` :198) | 64 entries | case name, later registration wins | front insert; full refuses unchanged and aborts loudly | O(n) | replaced |
| functional | channel harness deliveries | `head`/`free_head`/`next[]` delivery chain | `rocketnet_channel_delivery_ring_t` functional/channel_harness.h:299 | `MAX_DELIVERIES` 32 | per-slot `uint64_t` generation | refuses and counts an overflow when full or exhausted | O(n) remove and pop by channel | replaced |
| soak | soak allocator | per-class free list through `block->next` | `free_entries` examples/soak.c:120 (`SOAK_ARENA_FREE_CAPACITY` :110) | MAX_SOAK_CLIENTS * 64 entries | offset and size class | a full sequence on free aborts; last freed is reused first per class | O(free entries) per allocation | replaced |

A traversal through a `const` file-scope table with an initializer is not
storage: nothing admits to, removes from or relinks it. The audit reports such
a walk as `CONSTANT` (today the counter-family parent rows,
tests/test_unit_counter_classes.cpp:442) and does not fail on it; a mutable
table with the same walk fails.

## External ABI traversal sites

Operating-system enumerations arrive as linked nodes the OS allocates and
owns. RocketNet consumes each in place inside one function, copies only scalars
or names into bounded RocketNet storage, and releases the list before
returning; no node or link pointer is stored in a member, array or global. The
audit fails if a site appears that is not listed here, if a listed function
stops calling its release, if it stores the cursor anywhere, or if a row
matches no site. This records an ownership boundary; it does not change the
OS ABI.

| File | Function | Field | Release |
|---|---|---|---|
| sdk/rocketnet_platform.cpp | get_connection_type | ifa_next | freeifaddrs |
| sdk/rocketnet_platform.cpp | get_connection_type | Next | rocketnet_free |
| sdk/rocketnet_server_socket_linux.cpp | rocketnet_server_socket_find_interface | ifa_next | freeifaddrs |
| tools/xdp_packet_blaster.c | rn_blaster_source_on_interface | ifa_next | freeifaddrs |
