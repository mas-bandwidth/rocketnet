# Protocol channels, reliability and messages audit, 2026-10-24

Cold reading audit of `mas-bandwidth/rocketnet` at `9775266a705148969cc00fcadc1463b06879cfbd`
(`main`). Scope read whole: the message layer above the packet protocol, that is
`sdk/rocketnet_channel.cpp`, `sdk/rocketnet_channel.h`, `sdk/rocketnet_channel_reliable.cpp`,
`sdk/rocketnet_channel_sequenced.cpp`, `sdk/rocketnet_channel_receipts.cpp`,
`sdk/rocketnet_message.cpp`, `sdk/rocketnet_message.h`, `sdk/rocketnet_message_object.h`,
`sdk/rocketnet_message_client.cpp`, `sdk/rocketnet_message_server.cpp`,
`sdk/rocketnet_packet_builder.cpp` and `sdk/rocketnet_coalesce.h`, against
`standard/MESSAGES.md` (m1 to m13), `standard/TRANSPORT.md` and `sdk/rocketnet.h`, with the
unit and functional cases that pin them (`tests/test_unit_channel_*.cpp`,
`tests/test_unit_message_*.cpp`, the three `tests/test_unit_standard_*` files named for this
area, `functional/channel_*.cpp`). Acks, sequence numbers, windows, trackers and fragments are
audited separately and are left out. Method: each candidate was traced to the input or call
sequence that reaches it; one was confirmed with a throwaway unit test (no real time, sockets
or threads) that is not committed. No code was changed. Findings are ordered most severe
first; each gives severity (`bug` / `risk` / `quality`), `file:line` at 9775266a, a
one-sentence defect, a one-sentence concrete failure or cost, and the smallest fix.

## Findings

### 1. bug — `sdk/rocketnet_channel_sequenced.cpp:434`

- Defect: When an ordered delivery opens a held code 1 or code 4 record's epoch, `rn_channels_drain_sequenced` releases the hold if `rn_channels_deliver_sequenced` refuses it (a sequencing index in the upper half from the reset watermark of 0) and counts nothing, while the same record arriving in turn is counted `MESSAGE_SEQUENCED_STALE` (line 236); an instance of the "data dropped with no counter" class.
- Failure or cost: A code 1 record at ordering read+1, sequencing `0x800000`, held and then opened by the ordered record, leaves the update with one delivery, an empty hold and no counter moved (checked with a throwaway unit test), against m7.4 "Nothing is silent" and the stale drop of m6.1 rule 6; for code 4 the acked reliable message vanishes the same way.
- Smallest fix: Split the condition: keep the room failure as it is, and when `rn_channels_deliver_sequenced` refuses, release the hold and count `MESSAGE_SEQUENCED_STALE` (proposed diff in the report).

### 2. risk — `sdk/rocketnet_message_server.cpp:1250`

- Defect: `rn_message_object_receive_channel` keeps channel 0's per-channel cursor in `cursors[0]`, the same word `rn_message_object_next` (line 1192) uses as the one receive queue's cursor, and its comment (line 1237) says the unit suite still uses it although nothing in the repository calls it or its two wrappers.
- Failure or cost: A caller that takes a channel 0 message through `rocketnet_message_server_receive_channel` or `rocketnet_message_client_receive_channel` moves the queue cursor past waiting deliveries of other channels, which `receive` then never hands out, so the next update counts them `MESSAGE_NOT_RECEIVED` and ends the connection.
- Smallest fix: Delete `rn_message_object_receive_channel`, both wrappers and their declarations in `sdk/rocketnet_layers.h`, since no caller remains.

### 3. risk — `sdk/rocketnet_channel.cpp:835`

- Defect: `rn_channels_release` frees a block's fragments by comparing the 64-bit `descriptor->number` with `number + i`, but `rn_channels_descriptor` (line 162) fills a descriptor that layer 3 did not accept with the 24-bit wire number, which the header says lets dispatch stand alone.
- Failure or cost: A code 3 block whose numbers cross 2^24 (first number `0xFFFFFE`, four fragments) dispatched without the window leaves its fragments numbered 0 and 1 live after the update ends, so the next records in those slots are refused `MESSAGE_SLOT_COLLISION` and the connection ends.
- Smallest fix: Compare `rn_u24( descriptor->number ) == rn_u24( number + i )` in the release loop, as `rn_channels_descriptor` already does on the way in.

### 4. risk — `sdk/rocketnet_channel_sequenced.cpp:359`

- Defect: Each code 1 record held ahead of its epoch finds a free hold slot by a linear scan of all `region->holds` slots, and `rn_channels_end_update_sequenced` (line 461) scans every slot of every connection at every update, with `sequenced_messages_held` configurable up to 4096.
- Failure or cost: At the configured maximum a payload of code 1 records from one sender costs up to 4096 slot reads per record, work not bounded by the payload's length (m13 item 4), and a 1000-slot server reads 4.1 million hold entries per update even when no code 1 record is held.
- Smallest fix: Keep a free-slot stack and a count of slots in the delivered state (`used == 2`), pushing and popping in O(1) and skipping the end-update scan when the count is zero.

### 5. quality — `sdk/rocketnet_channel.cpp:1496`

- Defect: `rn_ordered_receive` returns `ROCKETNET_CHANNEL_RECEIVE_DEFERRED` (-1) when the delivery table cannot reserve a record's deliveries, but its header comment (`sdk/rocketnet_channel.h:401`) says it returns `ROCKETNET_MESSAGE_VIOLATION_NONE` or a violation, and the constant is defined among the dispatch outcomes (`sdk/rocketnet_channel.h:66`) with no comment.
- Failure or cost: A reader of the header cannot learn that a payload can end part-way with its acknowledgement withheld for retry (m1.4 rule 2), and a caller that treats any non-zero return as a violation would end a healthy connection.
- Smallest fix: Document the deferred return in the `rn_ordered_receive` comment and beside the constant, naming the m1.4 rule it implements.

### 6. quality — `sdk/rocketnet_channel.h:109`

- Defect: The comment on `rn_channels_set_payload_capacity` says the default payload capacity is `ROCKETNET_MAX_PACKET_PAYLOAD_BYTES`, but `rn_channels_init` sets `ROCKETNET_MESSAGE_SINGLE_BYTES` (`sdk/rocketnet_channel.cpp:194`) and the endpoint passes the same value to `rn_ordered_init`.
- Failure or cost: The one-record limits of m2.2 (1408, 1401, 1399) follow from 1415, not 1416, so a maintainer trusting the comment computes every reliable limit one byte too high.
- Smallest fix: Say the default is `ROCKETNET_MESSAGE_SINGLE_BYTES` and that the m2.2 limits are derived from it.

### 7. quality — `sdk/rocketnet_channel_reliable.cpp:41`

- Defect: The block bounds (count at least two, inside W and `max_message_bytes`, index inside the count, every fragment but the last exactly F, span inside the ring) are written four times: the reader (`sdk/rocketnet_message.cpp:391`), code 3 (`sdk/rocketnet_channel.cpp:718`), code 2 here, and code 4 (`sdk/rocketnet_channel_sequenced.cpp:127`), and the copies already differ (code 2 alone also checks the fragment's own ring span).
- Failure or cost: A change to m4 check 7 or 8 must be made in four places, and a missed copy makes the three reliable codes refuse different blocks.
- Smallest fix: One `rn_block_shape_valid( record, region, &base_units )` helper in `sdk/rocketnet_channel.cpp`, called by the three dispatch paths.

### 8. quality — `sdk/rocketnet_message_server.cpp:822`

- Defect: The in-place start computes a code 1 record's header as the literal `9 + ( channel >= 256 ? 2 : 1 )` instead of calling `rocketnet_message_record_header_bytes`, which `rn_channels_finish` uses for the same purpose (`sdk/rocketnet_channel.cpp:986`).
- Failure or cost: The record layout of m3 now lives in a third place, and a field added to code 1 changes the writer's header while the start keeps reserving the old size, so `rocketnet_message_writer_add_in_place` refuses every fitting code 1 send.
- Smallest fix: Build the record shape (code 1, channel) and call `rocketnet_message_record_header_bytes`, as `rn_channels_finish` does.

### 9. quality — `sdk/rocketnet_message_object.h:169`

- Defect: The in-place build's `receipt_serials[16]` bound is the literal 16, repeated as `build->receipt_count == 16` in the start (`sdk/rocketnet_message_server.cpp:830`) and as the assert in `rn_window_ack_commit_receipt_packet` (`sdk/rocketnet_window_ack.cpp:472`), while the endpoint's own array uses `ROCKETNET_PACKET_BUILDER_MAX_RECORDS`.
- Failure or cost: Raising one literal without the others either overflows `receipt_serials` at finish (line 1014) or trips an assert that compiles out in release.
- Smallest fix: Name the bound once and size the array, the start's check and the assert from it, with a `static_assert` tying it to the sent-map's record limit.

### 10. quality — `sdk/rocketnet_allocator.h:67`

- Defect: The per-channel receive field named `pad` carries data: the first number of a held code 4 record, read and written as `rx->pad` (`sdk/rocketnet_channel_sequenced.cpp:64`, `:343`, `:354`, `:427`).
- Failure or cost: A field named as padding invites a layout change or a zeroing that silently breaks the release of held code 4 numbers.
- Smallest fix: Rename it `held_sequenced_first`, matching its neighbour `block_in_turn_first`.

### 11. quality — `sdk/rocketnet_channel_reliable.cpp:141`

- Defect: Code 2 refuses a record whose descriptor is not live as `MESSAGE_SLOT_COLLISION`, under a comment that says the slot is live for another number, while code 3 fills a descriptor that is not live from the record so dispatch stands alone (`sdk/rocketnet_channel.cpp:162`).
- Failure or cost: The same input, a record that reached dispatch without layer 3, is a counted collision on code 2 and a delivery on code 3, and the code 2 counter names a cause that did not happen.
- Smallest fix: Give code 2 the same `rn_channels_descriptor` path as code 3, or correct the comment and count the not-live case under its own name.

### 12. quality — `sdk/rocketnet_message_server.cpp:1271`

- Defect: `rn_message_connection_release_view` finds a released view by scanning the connection's delivery table from entry 0 on every release.
- Failure or cost: An application that releases each message after `receive` does quadratic work per update: with the default table of 2048 entries (W 1024 plus 1024 unreliable) a full update costs about two million pointer comparisons per connection.
- Smallest fix: Scan backwards from `cursors[0] - 1`, where the last handed-out entry is, so the usual release-after-receive finds its entry at once.

### 13. quality — `sdk/rocketnet_message.h:62`

- Defect: `ROCKETNET_MESSAGE_SINGLE_BYTES` and `ROCKETNET_MESSAGE_FRAGMENT_BYTES` (line 66) are defined both here and in `sdk/rocketnet_layers.h` (lines 47 and 142), which this header includes through `rocketnet_internal.h`.
- Failure or cost: Two definitions of one wire constant must be edited together, and the comments beside them already describe the same value in different words.
- Smallest fix: Delete the two definitions here and keep the ones in `sdk/rocketnet_layers.h`.

## Counts

- bug: 1 (finding 1)
- risk: 3 (findings 2–4)
- quality: 9 (findings 5–13)
- total: 13

## Already filed

None. The two code-quality audits present under `docs/audits/` at BASE, both dated
2026-10-24, file no finding in the files of this area.
