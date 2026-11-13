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
| `dIn[d]`, `dGen[d]` | `region->send_descriptors[d].in_use`, `.generation` (0: never taken) |
| `fly[n]` | sent-map entries whose references include `n` (at most two here) |
| `wrote`, `got`, `freed` | ghosts: each number with the descriptor and generation that wrote it; the numbers carried by acked packets; each descriptor generation an ack returned a fragment to |

Each message is one fragment. A block's descriptor holds its numbers until
the last fragment's ack, which changes no name. The generation after `GenMax`
is 1, as the allocator wraps at `UINT32_MAX` (`rn_region_send_alloc`). The
main instance sets `GenMax` above the number of takes; the wrap cases set
`GenMax = 2`, so a descriptor's third take is generation 1 again and the
wrap fires. RNSendStoreLayout checks the wrap's skip of 0 at the code's
layout. The carve
clears every link (`Init`). A link (`rn_resend_link_t`, 8 bytes) is the
slot's `prev` and `next` and nothing else: the slot index names the number.

## Actions and the code they mirror

- **`Write(d)`**: `rn_region_send_alloc` takes a free descriptor at the next
  generation (`rocketnet_allocator.cpp`), `rn_window_send_first_write` writes
  number `nxt` into slot `nxt % W` (guard `nxt < U + W + WindowSlack`; the
  code's `rn_window_send_admit` is `next - U' <= W - 1` with `U' <= U`), and
  `rn_window_ack_park` links it: `rn_resend_fifo_insert_after` unlinks and
  links.
- **`Send(n)`**: `rn_window_ack_commit_packet` puts a packet referencing `n` in
  the sent map and `rn_resend_fifo_push_tail` relinks the slot.
  `rn_window_ack_packet_failed`'s `push_head` relinks the same way.
- **`Lose(n)`**: the environment: a packet is lost or its entry evicted.
- **`Ack(n)`**: `rn_window_ack_on_ack`, step 2, for one reference: skip an
  acked slot, one outside `[U, next)` and one whose number is not `n`;
  otherwise unlink (`rn_resend_fifo_unlink`), return the descriptor's
  fragment (`rn_region_send_free_fragment`), mark the number acked and move
  `U` (`rn_window_send_mark_acked`).

## Invariants

- **`NamedGenerationOnly`**: every fragment an ack returned belongs to the
  descriptor generation that wrote a number some acked packet carried.
- **`AckedOnlyDelivered`**: a number is marked acked only when a packet
  carrying it was acked.
- **`LinkNamesOwner`**: a linked entry is an unacked number whose descriptor
  is live at the generation that wrote the number.
- **`WindowOwnsSlots`**: every unacked number in `[U, nxt)` still holds its
  slot; no write reused it.
- **`NoStaleNameInFlight`**: used only by the reachability case: no packet
  references a number whose slot holds a later number of the same descriptor
  at a later generation.
- **`NoGenerationReuse`**: used only by the wrap reachability case: no two
  numbers are written under one descriptor and generation.

## Cases

`MCRNResendGenerations.tla` instantiates `W = 4`, `N = 6` (numbers 4 and 5
reuse the slots of 0 and 1), `D = 2`, `GenMax = 6`.

1. **`MCRNResendGenerations.cfg`**: the code (`CheckNumber = TRUE`,
   `WindowSlack = 0`): `TypeOK`, `NamedGenerationOnly`, `AckedOnlyDelivered`,
   `LinkNamesOwner` and `WindowOwnsSlots` hold.
2. **`MCRNResendGenerationsReachStale.cfg`**: the code violates
   `NoStaleNameInFlight`. Trace: number 0 is sent twice, the second packet is
   acked, numbers 1 to 3 are written, descriptor 1 is taken again at
   generation 3 for number 4 in slot 0, and the first packet is still in
   flight. The stale case is reached, so the invariants above are not vacuous.
3. **`MCRNResendGenerationsBrokenSlotOnly.cfg`** (`CheckNumber = FALSE`):
   violates `NamedGenerationOnly`. From the state in case 2, the stale packet
   is acked: slot 0 holds number 4, linked, and descriptor 1 is live at
   generation 3, so the ack unlinks slot 0, frees generation 3's fragment and
   marks number 4 acked though only number 0 was delivered. Checked by hand
   against `rn_window_ack_on_ack`: without
   `(uint32_t) slot->number != ref_u32` every other test passes for this slot,
   and the link holds nothing else to refuse it.
4. **`MCRNResendGenerationsBrokenWindowSlack.cfg`** (`WindowSlack = 1`, the
   off-by-one at capacity): violates `WindowOwnsSlots`. Number `U + W` is
   written into the slot of number `U` while `U` is unacked, so `U` loses its
   slot and no ack can reach it. Checked by hand against
   `rn_window_send_admit`: with `<= W` in place of `<= W - count` the same
   write is admitted.
5. **`MCRNResendGenerationsWrap.cfg`** (`GenMax = 2`, revision 1.1): the
   code with the generation wrapping. Case 1's invariants hold: a name
   (descriptor, generation) is reused, and the number comparison alone
   keeps a stale ack off the later number.
6. **`MCRNResendGenerationsReachWrap.cfg`** (`GenMax = 2`): violates
   `NoGenerationReuse`: one descriptor writes two numbers at one
   generation, so case 5 reaches the wrap. Trace, 8 states: descriptor 1
   writes number 0 at generation 1, it is sent and acked; descriptor 1 writes
   number 1 at generation 2, sent and acked; descriptor 1 writes number 2 at
   generation 1 again (2 is `GenMax`, so the next is 1), the name number 0
   had. Checked by hand against `rn_region_send_alloc`: `generation ==
   UINT32_MAX ? 1 : generation + 1` does the same at the code's maximum.
7. **`MCRNResendGenerationsBrokenSlotOnlyWrap.cfg`** (`GenMax = 2`,
   `CheckNumber = FALSE`): violates `AckedOnlyDelivered`, the stale ack of
   case 3 marking the later number acked, under the wrap. Under a reused name
   `NamedGenerationOnly` can match the freed fragment to the earlier write,
   so this case checks the property no reused name can satisfy: case 5 would
   catch a missing number comparison.

Measured by `tlacheck run` on aws-mut-3, 2026-11-13:

| Case | Expected | Exit | Generated | Distinct | Seconds |
|---|---|---:|---:|---:|---:|
| `MCRNResendGenerationsWrap.cfg` | pass | 0 | 221821 | 31501 | 1.7 |
| `MCRNResendGenerationsReachWrap.cfg` | `NoGenerationReuse` | 12 | 239 | 116 | 0.7 |
| `MCRNResendGenerationsBrokenSlotOnlyWrap.cfg` | `AckedOnlyDelivered` | 12 | 4322 | 1418 | 0.8 |

The wrap case has the main case's counts: the generation is a function of
each descriptor's take count, which the state already carries, so wrapping
renames states without merging any.

`test_resend_stale_ack_leaves_the_later_generation`
(`tests/test_unit_resend_generations.cpp`) replays case 3's state against the
code at `W = 64` and checks that the stale ack leaves the later number linked
and unacked.
