# RNRttTreeLayout: the rtt minimum tree at the code's layout

`RNRttTreeLayout.tla` models `stats_rtt_min_tree` in `sdk/rocketnet_ack.cpp`,
an implicit binary tree in an array of `L` floats over the `L`-entry
`rtt_history` ring: node `k` (`1 <= k < L`) is the minimum of nodes `2k` and
`2k + 1`; a node at or past `L` is the history entry `node - L`, `NONE` when
not positive (`rocketnet_ack_stats_rtt_node`). `rocketnet_ack_stats_rtt_write`
updates the leaf and walks from `(index + L) >> 1` to the root, stopping at the
first node whose minimum is unchanged. `rocketnet_ack_endpoint_reset` fills the
history with -1 and the tree with `NONE`.

## The instance

`MCRNRttTreeLayout.tla`: a history of 8 (three levels), written samples 0
(not positive), 1 and 2, `NONE` as 9.

## Invariants

| Invariant | Meaning |
|---|---|
| `TreeExact` | every node is the minimum of its two children |
| `RootIsMinimum` | the root is the minimum positive sample, `NONE` when there is none |

## Reversed witnesses

| Config | Rule turned off | Fails |
|---|---|---|
| `MCRNRttTreeLayoutBrokenNoOffset.cfg` | the walk starts at `index >> 1` | `TreeExact` |
| `MCRNRttTreeLayoutBrokenDecreaseOnly.cfg` | the walk stops where the minimum does not fall | `RootIsMinimum` |

Each counterexample was checked against the C by hand: no-offset writes 1 at
index 0 and starts at node 0, so nothing is updated; decrease-only writes 1
then 0 at index 0, and the root keeps 1 where the code's equality test walks
on and raises it to `NONE`.
