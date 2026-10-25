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
