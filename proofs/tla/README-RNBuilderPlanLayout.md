# RNBuilderPlanLayout

The packet builder's flat plan at its layout: `rn_builder_plan`,
`rn_builder_radix_pass`, `rn_builder_key_byte`, `rn_builder_take_head` and the
refused packet's undo in `rn_packet_builder_write`, all in
`sdk/rocketnet_packet_builder.cpp`. [RNScheduler](../../docs/TLA-INVENTORY.md)
checks the order items are first written as sequences (its `IsHead` and
`Boost`); this model checks the hand-indexed arrays that carry that order,
which it cannot see.

## State

| Model | Code |
|---|---|
| `cls[x]`, `chn[x]`, `pri[x]`, `taken[x]` | `items[x].send_class`, `.channel`, `.priority`, `.taken` |
| `count`, `sortIdx`, `alt` | `count`, `plan->sort_indices`, `alternate` |
| `pass`, `bkt`, `b`, `offset` | the radix pass, `buckets[]`, the offset loop's `b` and `offset` |
| `gcount`, `gbegin`, `gend`, `gcursor`, `gchan`, `gcls` | `plan->group_count` and `plan->groups[g]` |
| `seqItem`, `seqGroup`, `seqSuffix` | `plan->sequence[s]`'s `item_index`, `group_index`, `suffix_level` |
| `position`, `p`, `suffix` | the group walk's `position`, the suffix walk's `p` and `suffix` |
| `c`, `nextGroup`, `cgBegin`, `cgEnd` | the class range loop and `plan->class_group_begin`, `class_group_end` |
| `packet`, `r` | `taken_positions[0 .. records - 1]` and the undo loop's `r` |
| `wrote`, `sent` | ghost: the slots this pass's scatter wrote; the items of written packets |
| `Radix`, `Levels`, `MaxRecords` | the 256-entry bucket table, `ROCKETNET_PACKET_BUILDER_LEVELS`, `ROCKETNET_PACKET_BUILDER_MAX_RECORDS` |

## Actions and the code they mirror

- **`Filter`**: `sort_indices[count++] = i` for each writable item.
- **`Count`**, **`Prefix`**, **`Scatter`**: one radix pass: `buckets[key]++`;
  `buckets[b] = offset; offset += n`; `output[buckets[key]++] = input[i]`. Pass 0
  keys on the channel's low digit, pass 1 on its high digit, pass 2 on the class,
  between `sort_indices` and `alternate`.
- **`Group`**, **`Member`**: the group walk opens a group at `position` (begin and
  cursor there, its class and channel from `alternate[position]`), then fills
  `sequence[position]` while the next item has the same class and channel, and
  sets `end`.
- **`Suffix`**: `for ( p = end; p > begin; p-- )` stores the running priority
  minimum in `sequence[p - 1].suffix_level`.
- **`Range`**: `class_group_begin[c] = next_group`, advance over the class's
  groups, `class_group_end[c] = next_group`.
- **`Take`**: a head (`cursor < end` and `sequence[cursor]` naming the group) of
  the lowest live class is taken: `taken_positions[records++] =
  rn_builder_take_head`. The scheduler's choice of group inside the class
  (clocks, round robin) is left free; RNScheduler checks it.
- **`Commit`**: the packet is admitted and written; its items stay taken.
- **`Stop`**: an empty packet or the payloads spent end the call.
- **`Refuse`**, **`Undo`**: the budget refuses the packet; `for ( r = records; r >
  0; r-- )` steps the cursor of `sequence[taken_positions[r - 1]]`'s group back
  and clears the item's `taken`, then the call breaks.

## Invariants

- **`NoOverlap`**: every scatter writes a distinct slot inside `0 .. count - 1`.
- **`PlanSorted`**: after the third pass `alternate` holds every writable item
  once, ordered by class, channel, then caller index (the passes are stable).
- **`GroupsExact`**: the groups tile `0 .. count - 1` contiguously, every slot
  names its group and item, and no two groups share a class and channel.
- **`ClassRanges`**: a class's group range holds exactly its groups.
- **`SuffixMinima`**: `sequence[s].suffix_level` is the least priority from `s`
  to its group's end.
- **`CursorMatchesTaken`**: a group's taken items are exactly `begin .. cursor -
  1`.
- **`ChannelOrderKept`**: for each group, its items written, in flight in the
  packet (during an undo, the records not yet given back) and still planned
  from the cursor are its items in caller order: none lost, none twice.

## The instances

`N = 4` writable items, every assignment of two classes, channels 1 and 2 and
two priority levels (4096 initial states), packets of up to two records, any
packet written or refused. At `Radix = 2` channel 1 is digits (1, 0) and channel
2 is (0, 1), so the high pass has to reorder what the low pass produced, as a
channel of 256 or more does at the code's 256. Terminal states (the call ended)
are the harness's bound (`ignore-terminal`).

## Reversed witnesses

Each fails the invariant it names, and TLC reports exit 12 for each. The
counterexamples were checked against the C by hand.

1. **`MCRNBuilderPlanLayoutBrokenNoRestore.cfg`** (`Restore = FALSE`, the
   `group->cursor--` dropped from the undo loop): fails `ChannelOrderKept`.
   Four items of class 0 on channel 1 form one group `0 .. 3`; item 0 is taken
   (cursor 1), the packet is refused, and the undo clears `items[0].taken` but
   leaves the cursor at 1. Item 0 is neither written nor at or past the cursor:
   in the code the group's cursor no longer agrees with its items' `taken`
   flags, and `rn_builder_next_in_class` would never offer item 0 from this
   plan again.
2. **`MCRNBuilderPlanLayoutBrokenInclusivePrefix.cfg`** (`PrefixSum =
   "inclusive"`, `buckets[b] = offset + n`): fails `NoOverlap`. Four items on
   channel 1 all key to bucket 1 in pass 0; its offset is 4, so the first
   scatter writes `output[4]`, past the four-item plan (in the code, past
   `count` into stale slots of the 96-entry array).
3. **`MCRNBuilderPlanLayoutBrokenReverseScatter.cfg`** (`ScatterOrder =
   "reverse"`, the scatter walking its input from `count - 1` with the same
   post-increment offsets): fails `PlanSorted`. Four items of one class and
   channel come out of each pass reversed; after three passes `alternate` is
   3, 2, 1, 0 and the channel's FIFO is written backwards.

## Finding

None: the code's prefix offsets are exclusive, its scatters run forward, its
group walk and suffix walk index the same ranges, and its undo steps each
taken position's cursor back, last first.
