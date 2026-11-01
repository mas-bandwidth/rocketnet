# RNClientIoCompactLayout

The client I/O thread's batch receive at its layout: the `receive_batch`
branch of `rocketnet_client_io_step` in `sdk/rocketnet_client_io.h`, with
`rocketnet_client_io_fill_record`. [RNClientIoDecrypt](README-RNClientIoDecrypt.md)
checks the records as a sequence; this model checks how one batch is laid into
the fill buffer's frame slots and compacted in place, which it cannot see.

## State

| Model | Code |
|---|---|
| `slot[s]` | frame slot `s` of `buffer->data`: the datagram id it holds |
| `fill` | `io->fill_slots` |
| `recs[k]` | record `k`: its datagram and `buffer->packet_data[k]`'s slot |
| `i`, `accepted`, `cnt` | the loop's `i`, `accepted` and `count` |
| `batch`, `faulty` | ghost: the ids received at each offset, and the offsets whose byte count is a transport fault |
| `C`, `B` | `ROCKETNET_NUM_CLIENT_PACKETS`, `ROCKETNET_CLIENT_IO_BATCH` |

## Actions and the code they mirror

- **`Receive`**: `receive_batch` writes `got` datagrams, at most
  `min( C - fill_slots, B )`, into slots `fill_slots ..`; any of them may carry a
  faulty byte count (negative or past `ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES`).
- **`Step`**: one pass of the loop: a fault is skipped; any other datagram is
  moved from slot `fill_slots + i` to slot `fill_slots + accepted` when they
  differ (the `memmove`), and `rocketnet_client_io_fill_record` makes the next
  record, naming slot `fill_slots + accepted`.
- **`EndBatch`**: `io->fill_slots += accepted` (nothing when 0, the break).
- **`Publish`**: the buffer is published and the next one fills from slot 0,
  its slots holding whatever they held.

A `got` past `max_packets` is clamped to `max_packets` before the loop, so the
loop sees the same shape; the model does not draw it separately.

## Invariants

- **`RecordsExact`**: every record's slot is in the buffer and holds the
  datagram the record was made for: no later move overwrote it, and it was
  moved into.
- **`RecordsDense`**: record `k` names slot `k - 1`, so `fill_slots` counts the
  records.

## The instances

`C = 4` slots, batches of up to `B = 3`, seven datagrams in all with any subset
of each batch faulty: batches compact, fill a buffer partly and fully, and a
published buffer's stale slots are written again. Terminal states are the
harness's bound (`ignore-terminal`).

## Reversed witnesses

Each must fail `RecordsExact`. Both counterexamples were checked against the C
by hand.

1. **`MCRNClientIoCompactLayoutBrokenRecordAtSource.cfg`** (`RecordAt =
   "source"`, the record names `fill_slots + i`): a batch of three lands in
   slots 0, 1, 2 with the first faulty. At `i = 1` datagram 2 is moved to
   slot 0 and its record names slot 1; at `i = 2` datagram 3 is moved to
   slot 1, over the slot the first record names. In the code this is
   `fill_record( ..., buffer->data + ( fill_slots + i ) * ROCKETNET_FRAME_SIZE,
   ... )` against the `memmove` to `packet_data + accepted * ROCKETNET_FRAME_SIZE`:
   update reads datagram 3's bytes for datagram 2.
2. **`MCRNClientIoCompactLayoutBrokenNoMove.cfg`** (`Move = FALSE`, the `memmove`
   dropped): a batch of two with the first faulty; the record for datagram 2
   names slot 0, which still holds the faulty datagram's bytes.

## Finding

None: the code's move and record slot agree, and `fill_slots` advances by
exactly the records made.
