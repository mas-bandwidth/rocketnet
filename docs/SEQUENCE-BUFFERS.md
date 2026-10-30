# Sequence buffers

This page holds the owned storage manifest and the approved index links: how
RocketNet's bounded storage is laid out. The generic modulo-table template
that once stood here (`rn_sequence_buffer_t`) is gone; no sdk code used it, and
each structure below carries its own modulo arithmetic.

## External ownership boundary

The storage on this page adds no public C ABI or installed-header surface.
Traversal supplied by an operating system stays outside it. An OS-owned
enumeration must be consumed in place or copied into bounded RocketNet
storage; its nodes and links must never become RocketNet-owned linked state.

## Owned storage manifest

This manifest tracks the migration of bounded storage and the later approved
fixed-array index links. A `replaced` row means the earlier representation was
replaced by the representation in that row; it does **not** mean the current
representation contains no index links. Rows that use approved links say so.
A `residual` row is unresolved migration debt.

`tools/check_no_owned_linked_lists.py` is a syntactic audit. It rejects detected
owned links outside the approved sites and checks the listed sites, but it does
not recognize every possible link operation. The [approved index links](#approved-index-links)
section names both checked sites and manually recorded ones. A passing audit
means no unapproved site was detected by those rules, not a proof of universal
absence. The source-audit manifest check also requires each replacement symbol
to exist and refuses `residual` rows.

Older line-number citations were recorded at main `ff95e91a`; rows naming the
2026-10-28 decision describe later replacements. Resolve symbols in the current
source instead of treating those historical line numbers as current. Q is the
region's descriptor count (`region->queue`), W its reliable window, and C its
channel count.

| Scope | Family | Removed links | Replacement | Capacity | Identity | Admission and eviction | Worst case | Status |
|---|---|---|---|---|---|---|---|---|
| sdk | ack received stats | receive-order doubly linked list through record `stats_prev`/`stats_next` | `stats_word` sdk/rocketnet_ack.h:122 (32-record block summaries in the received ring) | `ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE` 2048 records, 64 blocks | exact `uint64_t` packet sequence per record, written bitmap | slot is sequence mod 2048; a lap overwrites the oldest; a sequence at or below highest - 2048 is refused | 256 record or block visits per query | replaced |
| sdk | encryption map | bucket chains and free chain through `next`, `bucket_head`, `free_head` | `free_indices` sdk/rocketnet_encryption_map.cpp:29 (dense LIFO) beside the flat probe `index` :28 | max_clients * 2 mappings | per-slot `uint64_t` generation from a map-wide counter; 0 is exhausted | refuses when no slot is free or generations are exhausted; never evicts; removal wipes secrets and pushes the slot | O(1) stack; probe and backward-shift erase O(capacity) | replaced |
| sdk | client slots | bucket chains through `next` and `bucket_head` | `free_stack` sdk/rocketnet_client_slots.cpp:58 beside the flat `index_table` :50 | max_clients slots, 2 * max_clients index cells | `uint64_t` slot generation checked with the client id | refuses when full or on a duplicate generation; never evicts | O(1) stack; probe O(2 * max_clients) | replaced |
| sdk | token tables | bucket chains, free chain and claim-order doubly linked list | claims: insertion-order ring `order_slot` sdk/rocketnet_token_tables.cpp with `order_ticket` and tombstones (proofs/tla/RNIndexRing.tla) beside `free_slots` and a flat index; bindings: `free_slots` and a flat index | max_clients * 8 per table (bindings, claims) | `uint64_t` insertion ticket per slot (0 unoccupied) | bindings take free, else the first expired, else the oldest ticket; claims take free or an expired oldest, else refuse; a live claim is never evicted | claims O(1) amortized select, record and release (one tombstone pass per capacity pushes); bindings full-table selection O(capacity), reached only after a valid signature | replaced |
| sdk | packet builder | per-item `next_in_channel` and `next_head` links | `rn_packet_builder_plan_t` sdk/rocketnet_packet_builder.h:105 | `ROCKETNET_PACKET_BUILDER_MAX_ITEMS` 96 items per call | `uint64_t` call epoch with item and group index | a call over 96 items or at an exhausted epoch is refused whole | O(items) build, O(groups) per take | replaced |
| sdk | network simulator | per-shard sorted doubly linked lists | `shard_occupancy` sdk/rocketnet_network_simulator.cpp:185 with `shard_summary` :186 | create argument (server 4096 slots), 64 shards | per-slot `uint64_t` generation | lowest free slot; a full queue or exhausted generation drops the packet | O(1) membership; insert scan O(capacity) | replaced |
| sdk | IOCP queues | interlocked free-send and retry-receive lists and the run index chain | `rocketnet_iocp_ref_queue_t` sdk/rocketnet_server_iocp_windows.cpp:52 and `run_refs` :295 | 2 * max clients rounded to a power of two sends; 32 * 128 retries | `uint64_t` owner epoch and generation (48 bits on the wire) per reference; `uint64_t` run ticket | push refuses when full and the operation is retired; never evicts | one cell per operation, compare-and-swap retries under contention | replaced |
| sdk | channel send FIFO | key-ordered scan over `send_descriptors` by `enqueue_ticket` (the earlier replacement) | `queue_next` sdk/rocketnet_allocator.h descriptor index with `queue_head`/`queue_tail`; `rn_channel_queue_append`, `rn_channel_queue_push_front`, `rn_channel_queue_pop` sdk/rocketnet_channel.cpp; index-linked by decision (Glenn 2026-10-28): CONFIG needs a front insert, and the links are indices in the fixed Q-descriptor array | Q descriptors | slot index link per descriptor beside its slot generation | append at the tail; CONFIG inserts at the head; numbering pops the head; nothing leaves from the middle | O(1) every operation | replaced |
| sdk | channel ready list | key-ordered scan over `channel_send` by `ready_ticket` (the earlier replacement) | `ready_prev` sdk/rocketnet_allocator.h and `ready_next` channel indices; `rn_ready_link`, `rn_ready_remove_named` sdk/rocketnet_channel.cpp; index-linked by decision (Glenn 2026-10-28): the list needs interior removal, and the links are indices in the fixed C-channel array | C channels | slot index links beside the ready ticket and generation per channel | one membership per channel; link at the tail; unlink from anywhere | O(1) every operation | replaced |
| sdk | unreliable send FIFO | key-ordered scan over `send_descriptors` by `enqueue_ticket` (the earlier replacement) | insertion-order ring `unreliable_ring` sdk/rocketnet_endpoint.cpp of {descriptor, ticket} with tombstones (`RN_PART_UNRELIABLE_RING`, 2Q entries per endpoint; `rn_unreliable_push`, `rn_unreliable_walk_begin`/`_next`; proofs/tla/RNIndexRing.tla) | Q descriptors, 2Q ring entries | per-entry ticket matched against the descriptor's | push at the tail; expiry or send leaves a tombstone; a full ring compacts in one pass | O(1) amortized push and pop | replaced |
| sdk | resend FIFOs | key-ordered scan over `resend_fifo` (the earlier replacement) | `rn_resend_link_t` sdk/rocketnet_allocator.h prev/next slot indices inside the fixed W-entry window, ends in `resend_head`/`chunk_head`/`resend_tail`/`chunk_tail`; index-linked by decision (Glenn 2026-10-28): the queue needs insert-after and interior removal, and the links are indices in a fixed, bounded window, never pointers or allocations | W slots | slot index links, linked bit and chunk class in `resend_fifo` beside slot generation and owner epoch | push head or tail, insert after a slot, unlink from anywhere | O(1) every operation | replaced |
| sdk | send descriptor pool | LIFO free list through free descriptors' `span_next`, head `descriptors_free_head` | `free_slot` sdk/rocketnet_allocator.h:144 with `descriptors_free_top` :423 (popped at sdk/rocketnet_allocator.cpp:758, pushed at :796) | Q descriptors | `uint32_t` slot generation bumped on take | lazy fresh watermark then LIFO stack; refuses when none is free | O(1) take and free | replaced |
| sdk | send store chunks | free chain and block chunk chain through the 20-bit next of `meta`, head `free_head` | `RN_META_COUNT_MASK` sdk/rocketnet_allocator.cpp:206 with `free_head` and `block_tag_next` sdk/rocketnet_allocator.h:335-338 (contiguous run scan at :323, arithmetic block free at :491) | per-class chunk count from the store bytes | 10-bit chunk generation in `meta` (shared across a block) and 20-bit run length or head distance | lazy fresh watermark and contiguous free-run scan; refuses when no contiguous run fits | one first-fit scan per allocation (the decision's run is the one taken): O(needed) when the cursor lands on a free run, O(num_chunks) of one class at worst; O(fragments) block free. Only the local sender drives the scan; a receiver that withholds acks fills the store, and a full store exhausts the connection | replaced |
| sdk | live send spans | circular order array `order_slot` with `span_pos` and span tickets (the earlier replacement) | `span_prev` sdk/rocketnet_allocator.h and `span_next` descriptor indices with ends `span_first`/`span_last`; `rn_span_link`, `rn_span_unlink` sdk/rocketnet_allocator.cpp; proofs/tla/RNSendSpans.tla; index-linked by decision (Glenn 2026-10-28): frees come in ack order, so the list needs interior removal; the links are indices in the fixed Q-descriptor array | Q descriptors | slot index links beside the descriptor generation | link at the newest end; free in any order splices out; the reclaim floor advances at the oldest live span | O(1) every operation | replaced |
| suite | unit registry | registration chain through `next` | `rocketnet_unit_file_registry_t` tests/test_unit_registry.h:54 | `ROCKETNET_UNIT_FILE_TEST_CAPACITY` 2048 cases | case name, unique | front insert; a duplicate or full registry aborts | O(n) insert and duplicate scan | replaced |
| suite | suite RSS bound registry | registration chain through `next` | `SUITE_CASE_RSS_BOUND_CAPACITY` tests/suite_runner.h:194 (`entries` :198) | 64 entries | case name, later registration wins | front insert; full refuses unchanged and aborts loudly | O(n) | replaced |
| functional | channel harness deliveries | `head`/`free_head`/`next[]` delivery chain | `rocketnet_channel_delivery_ring_t` tests/functional/channel_harness.h:299 | `MAX_DELIVERIES` 32 | per-slot `uint64_t` generation | refuses and counts an overflow when full or exhausted | O(n) remove and pop by channel | replaced |
| soak | soak allocator | per-class free list through `block->next` | `free_entries` examples/soak.c:120 (`SOAK_ARENA_FREE_CAPACITY` :110) | MAX_SOAK_CLIENTS * 64 entries | offset and size class | a full sequence on free aborts; last freed is reused first per class | O(free entries) per allocation | replaced |

A traversal through a `const` file-scope table with an initializer is not
storage: nothing admits to, removes from or relinks it. The audit reports such
a walk as `CONSTANT` (today the counter-family parent rows,
tests/test_unit_counter_classes.cpp:442) and does not fail on it; a mutable
table with the same walk fails.

## Approved index links

A queue that needs insert-after, insert-at-head or interior removal in O(1)
may link its entries by index inside one fixed, bounded array it owns: no
pointers, no allocation, every link an index into that array. Glenn chose this
on 2026-10-28 for the message layer's queues. Each linking site is listed by
file, function and field; the audit fails on any link site not listed here,
and on a row that matches no site.

| File | Function | Field | Decision |
|---|---|---|---|
| sdk/rocketnet_channel.cpp | rn_ready_link | ready_next | Glenn 2026-10-28: index links in the window |
| sdk/rocketnet_channel.cpp | rn_ready_link | ready_prev | Glenn 2026-10-28: index links in the window |
| sdk/rocketnet_channel.cpp | rn_channel_queue_push_front | queue_next | Glenn 2026-10-28: index links in the window |
| tests/test_unit_channel_dispatch.cpp | schedule_walk | queue_next | Glenn 2026-10-28: index links in the window (the test reads the channel FIFO) |
| sdk/rocketnet_allocator.cpp | rn_span_link | span_next | Glenn 2026-10-28: index links in the window |

The audit sees a link written as `x.field = head` and a walk `d = ...field`; it does not see a write through an indexed tail (`descriptors[tail].queue_next = d`), so `rn_channel_queue_append`, `rn_span_unlink`, the soak's `rn_soak_queue_append` (tests/test_channels.cpp) and the resend FIFO helpers are recorded here by hand, not enforced.

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
| sdk/rocketnet_server_socket_linux.cpp | rocketnet_server_socket_find_interface | ifa_next | freeifaddrs |
| tools/xdp_packet_blaster.c | rn_blaster_source_on_interface | ifa_next | freeifaddrs |
