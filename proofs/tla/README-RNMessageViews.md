# RNMessageViews: the message object's receive views at the code's layout

`RNMessageViews.tla` models the receive views of one connection of the
message object as `sdk/rocketnet_message_server.cpp` lays them out. The
update's deliveries are entries `0 .. deliveries - 1` of the region's
delivery table; beside it the connection keeps `views`, one byte per table
entry (`RN_MESSAGE_VIEW_WAITING` 0, `HELD` 1, `RELEASED` 2), `receive_cursor`
and `held`; the object keeps the last batch's entries (`batch_entry`,
`batch_count`).

`RNSlots.tla` checks the views as a set (`UpdateRequiresReleasedViews`,
`Release`). A set cannot hold a cursor, a search that wraps at the delivery
count, a held counter that can be decremented twice, or a clear that covers
the wrong number of bytes; this model carries those.

## The instance

`MCRNMessageViews.tla`: a delivery table of three entries and a batch of
two, so a batch and a single receive share an update and the release search
wraps.

## Actions and the code they mirror

Functions in `sdk/rocketnet_message_server.cpp`.

| Action | Code |
|---|---|
| `Update(n)` | `rn_message_object_update`: the last batch released (`rn_message_object_release_batch`), views still held counted as `NOT_RELEASED`, the count of the last update's deliveries read from `rn_endpoint_begin_update` (which empties the table), that many views cleared, the cursor and `held` reset; then the transport update appends `n` deliveries |
| `Receive` | `rn_message_object_receive` through `rn_message_object_next`: the first `WAITING` entry at or after the cursor becomes `HELD`, `held++`, the cursor moves past it; at the end the cursor is the delivery count |
| `ReceiveBatch` | `rn_message_object_receive_messages`: the last batch released, then up to `ROCKETNET_RECEIVE_BATCH_MESSAGES` entries taken as `Receive` takes them |
| `Release(e)` | `rn_message_object_release` with a pointer the application holds, through `rn_message_connection_release_view`: a backward search from `cursor - 1`, wrapping at the delivery count, for the entry with that data pointer that is `HELD` |

Each entry's data pointer is distinct, so a pointer names its entry. One
connection: the object's queue across connections (`receive_connection`, and
the release trying the queue's connection first) is not modelled. A
delivery still `WAITING` at the next update kicks the connection
(`MESSAGE_NOT_RECEIVED`, in `rn_message_object_update`); the model leaves that to `RNSlots`.

## Invariants

| Invariant | Meaning |
|---|---|
| `TypeOK` | types, `held` in `0 .. Cap` (an underflow shows) |
| `HeldCountExact` | `held` is the number of `HELD` views |
| `HeldIsApp` | the `HELD` views are exactly the entries the application holds |
| `ReleaseFindsHeld` | releasing a pointer the application holds always finds its view |
| `NoReadPastDeliveries` | the release search reads only this update's entries |
| `BeyondIsWaiting` | every entry at or past the delivery count is `WAITING`, so a new delivery starts unread |
| `NoLostView` | a receive never passes over an entry of this update that was not handed out |
| `CursorBounds` | the cursor is at most the delivery count and nothing at or past it has been handed out |

## Reversed witnesses

| Case | Rule off | Declared invariant |
|---|---|---|
| `MCRNMessageViewsBrokenSearchFromCursor.cfg` | `SearchFromCursor = FALSE`: the search starts at `k = cursor`, not `cursor - 1` (`rn_message_connection_release_view`) | `ReleaseFindsHeld` |
| `MCRNMessageViewsBrokenBatchNoCheck.cfg` | `BatchChecksHeld = FALSE`: the batch release without the `== HELD` test (`rn_message_object_release_batch`) | `HeldCountExact` |
| `MCRNMessageViewsBrokenClearAfterExpire.cfg` | `CountBeforeExpire = FALSE`: the views cleared by a count read after `rn_ordered_end_update` emptied the table, which is 0 | `NoLostView` |

### Hand checks against the code

**SearchFromCursor.** TLC's trace: one delivery, received (entry 0 `HELD`,
cursor 1). The application releases its pointer. Starting at `k = cursor = 1`
the loop reads `table[1]`, an entry past this update's deliveries, and its one
iteration (`deliveries` = 1) ends there: entry 0 is never reached, the release
is counted as misuse, and the next update reports the view as not released.
`rn_message_connection_release_view` starts at `cursor - 1`, the last entry
handed out, and its wrap visits every entry below it.

**BatchNoCheck.** TLC's trace: one delivery is handed out in a batch (entry
0 `HELD`, `held` 1). The application releases it with
`rn_message_object_release`: entry 0 is `RELEASED`, `held` 0. The next
`rn_message_object_receive_messages` releases the last batch: without the
`HELD` test it decrements `held` again, and the `uint32_t` wraps to
`0xFFFFFFFF`, so every later update reports four billion views not released.
With the test the batch release skips the entry the application already
released.

**ClearAfterExpire.** TLC's trace: an update with one delivery; the
application receives it (entry 0 `HELD`) and does not release it before the
next update. That update clears nothing, so entry 0 stays `HELD` while the new
update's first delivery is written to table entry 0. `rn_message_object_next`
skips every entry that is not `WAITING` and the cursor runs past entry 0: the
delivery is never handed out, and the not-received check in
`rn_message_object_update` counts only `WAITING` entries, so the loss is
silent though the transport acked it. The code reads the count before
`rn_endpoint_begin_update` ends the update (`rn_endpoint_begin_update` returns
`deliveries` before calling `rn_ordered_end_update`, in
`sdk/rocketnet_endpoint.cpp`).

## An observation, not a finding

Two deliveries of one update could share a data pointer only if two
zero-byte ordered messages were held at the same ring offset. A release by
pointer then releases whichever of the two the backward search reaches
first. The application cannot tell them apart, `held` stays exact, and the
only effect is that a later release of a batch entry the application also
released by pointer may count one misuse. The model gives every entry its own
pointer and does not cover this.

## Records

See `RUNS.tsv` (group `rnmessageviews`).
