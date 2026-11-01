# RNResendGenerations

The names a transport ack follows into the resend FIFO of
`sdk/rocketnet_window_ack.cpp`, at the code's slot layout. A number's entries
are `send_numbers[n % W]` and `resend_fifo[n % W]`; a descriptor is freed by
the ack of its last fragment and taken again with the next generation; a
packet stays in the sent map, still referencing its numbers, after those
numbers were acked through another packet. So an ack can arrive for a number
whose slot now holds a later number, written by the same descriptor at a
later generation. This model checks what the ack does then.
[RNResendFifoLayout](README-RNResendFifoLayout.md) checks the prev/next links
and class bits of the same array; here the links are one flag per slot.

## State

| Model | Code |
|---|---|
| `U`, `nxt` | `send->U`, `send->next` |
| `sn[s]`, `[n, d, acked]` | `region->send_numbers[s]`: `number`, `descriptor`, `acked` |
| `lk[s]` | bit 31 of `region->resend_fifo[s].prev` (`RN_RESEND_LINKED`) |
| `st[s]` | `region->resend_fifo[s].generation`, the stamp |
| `dIn[d]`, `dGen[d]` | `region->send_descriptors[d].in_use`, `.generation` (0: never taken) |
| `fly[n]` | sent-map entries whose references include `n` (at most two here) |
| `wrote`, `got`, `freed` | ghosts: each number with the descriptor and generation that wrote it; the numbers carried by acked packets; each descriptor generation an ack returned a fragment to |

Each message is one fragment. A block's descriptor holds its numbers until
the last fragment's ack, which changes no name. The generation after `GenMax`
is 1, as the allocator wraps at `UINT32_MAX`; the instance sets `GenMax`
above the number of takes, and RNSendStoreLayout checks the wrap. The carve
clears every link and stamp (`Init`).

## Actions and the code they mirror

- **`Write(d)`**: `rn_region_send_alloc` takes a free descriptor at the next
  generation (`rocketnet_allocator.cpp`), `rn_window_send_first_write` writes
  number `nxt` into slot `nxt % W` (guard `nxt < U + W`), and
  `rn_window_ack_park` links it: `rn_resend_fifo_insert_after` unlinks, links
  and calls `rn_resend_fifo_stamp`.
- **`Send(n)`**: `rn_window_ack_commit_packet` puts a packet referencing `n` in
  the sent map and `rn_resend_fifo_push_tail` relinks the slot; the unlink
  inside clears the stamp and the link stamps it again.
  `rn_window_ack_packet_failed`'s `push_head` relinks the same way.
- **`Lose(n)`**: the environment: a packet is lost or its entry evicted.
- **`Ack(n)`**: `rn_window_ack_on_ack`, step 2, for one reference: skip an
  acked slot, one outside `[U, next)` and one whose number is not `n`; then a
  linked slot stamped 0 is refused while its descriptor is live, and a named
  stamp is refused when the live descriptor's generation differs
  (`rn_resend_fifo_unlink_named`); otherwise unlink, return the descriptor's
  fragment (`rn_region_send_free_fragment`), mark the number acked and move
  `U` (`rn_window_send_mark_acked`).

## Invariants

- **`NamedGenerationOnly`**: every fragment an ack returned belongs to the
  descriptor generation that wrote a number some acked packet carried.
- **`AckedOnlyDelivered`**: a number is marked acked only when a packet
  carrying it was acked.
- **`StampNamesOwner`**: a linked entry is an unacked number whose descriptor
  is live, and its stamp is that descriptor's generation, the one that wrote
  the number.
- **`NoStaleNameInFlight`**: used only by the reachability case: no packet
  references a number whose slot holds a later number of the same descriptor
  at a later generation.

## Cases

`MCRNResendGenerations.tla` instantiates `W = 4`, `N = 6` (numbers 4 and 5
reuse the slots of 0 and 1), `D = 2`, `GenMax = 6`.

1. **`MCRNResendGenerations.cfg`**: the control, every flag on: `TypeOK`,
   `NamedGenerationOnly`, `AckedOnlyDelivered`, `StampNamesOwner` hold.
2. **`MCRNResendGenerationsReachStale.cfg`**: the code violates
   `NoStaleNameInFlight`. Trace: number 0 is sent twice, the second packet is
   acked, numbers 1 to 3 are written, descriptor 1 is taken again at
   generation 3 for number 4 in slot 0, and the first packet is still in
   flight. The stale case is reached, so the invariants above are not vacuous.
3. **`MCRNResendGenerationsBrokenSlotOnly.cfg`** (`CheckNumber = FALSE`):
   violates `NamedGenerationOnly`. From the state in case 2, the stale packet
   is acked: slot 0 holds number 4, linked with stamp 3, and descriptor 1 is
   live at generation 3, so `rn_resend_fifo_unlink_named` (which the ack calls
   with the slot's own number and stamp) agrees, and the ack frees
   generation 3's fragment and marks number 4 acked though only number 0 was
   delivered. Checked by hand against `rn_window_ack_on_ack`: without
   `(uint32_t) slot->number != ref_u32` every other test passes for this slot.
4. **`MCRNResendGenerationsBrokenNoRestamp.cfg`** (`Restamp = FALSE`): violates
   `StampNamesOwner` at the first resend: the slot is linked with stamp 0
   while its descriptor is live. The ack's generation-0 branch then skips it
   on every packet, so the number is never acked and the connection times out.
5. **`MCRNResendGenerationsNoCompare.cfg`** (`CompareGeneration = FALSE`):
   passes every invariant, the same state space as the control. A linked
   entry's stamp is always its live descriptor's generation
   (`StampNamesOwner`), so the generation refusal and the generation-0 branch
   never decide on a reachable state; the number comparison is what keeps a
   stale ack off a later generation.
