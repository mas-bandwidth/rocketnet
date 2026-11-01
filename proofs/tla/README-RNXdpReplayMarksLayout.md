# RNXdpReplayMarksLayout

The userspace pending buffer for the kernel replay map, at the layout of
`sdk/rocketnet_xdp_replay_marks.h` and with the code's own arithmetic.
[RNXdpReplay](README-RNXdpReplay.md) models the buffer's contents as sets of
owed forgets and marks; this model carries the arrays those sets are made of:
the counts, the kept prefix, and the two in-place compactions of
`rocketnet_xdp_replay_marks_reset_keeping`.

## State

| Model | Code |
|---|---|
| `forgets[i]`, `fc` | `forgets[i]`, `forget_count` |
| `mkeys[i]`, `mvals[i]`, `mc` | `mark_keys[i]`, `mark_values[i].high_water`, `mark_count` |
| `kc` | `kept_mark_count`: marks `[0, kc)` were refused by the last commit |
| `refused` | `refused` |
| `li`, `lk` | the compaction loops' `i` (or `j`) and `kept` (or `kept_marks`) |
| `failed`, `applied` | the commit's `forget_failed[]` and `mark_applied[]` |

The arrays have one cell past the end (index `M`), so a write out of bounds is
visible (`overflow`). Memory past a count is not cleared: a cell the compaction
or a drop left behind keeps its old key and value, which only a search that
runs past its bound can read.

The ghosts `absF`, `absM` and `absK` are the contract: the pending forgets in
order, the pending marks in order, and the kept prefix's length, updated with
sequence operators and never with the arrays' arithmetic. The kernel's answer
to a commit (which forgets it refused, which marks it applied) is chosen
freely.

## Actions and the code they mirror

An update is the walk's forget pass, its mark pass, then the commit; the walk
forgets and marks each key at most once per update.

- **`Forget(k)`**: `rocketnet_xdp_replay_marks_forget`: a pending key is
  coalesced before the capacity test; a new key is refused at
  `forget_count >= capacity`, else appended. An accepted forget, including a
  duplicate at capacity, then drops the first kept mark of the key by `rocketnet_xdp_replay_marks_drop_kept`,
  which shifts `[i + 1, mark_count)` down one and decrements both counts.
- **`Mark(k, v)`**: `rocketnet_xdp_replay_marks_mark`: a kept mark of the key
  is replaced in place; else refused at `mark_count >= MAX`, else appended.
- **`Commit`**: the transport's commit
  (`rocketnet_server_socket_replay_commit` on Linux) hands
  `reset_keeping` its `forget_failed` and `mark_applied`. The contract's
  result is taken here: the refused forgets that no applied mark of their key
  superseded, in order, and the refused marks, in order, all kept.
- **`CompactForgetStep`**: one iteration of `reset_keeping`'s forget loop
  (the superseded test over the applied marks, the move to `forgets[kept]`
  when `kept != i`), and at the end `forget_count = kept`.
- **`CompactMarkStep`**: one iteration of the mark loop (the move of key and
  value to `kept_marks` when `kept_marks != j`), and at the end
  `mark_count = kept_mark_count = kept_marks` and `refused = 0`.

Each loop iteration is its own step, so a write that lands on an entry the
loop has not read yet is visible.

## Invariants

- **`InBounds`**: no write lands at or past `MAX`.
- **`LayoutIsQueue`**: between commits, `forgets[0, fc)` is the pending
  forgets in order, `mark_keys` and `mark_values` over `[0, mc)` are the
  pending marks in order, and `kept_mark_count` is the kept prefix's length.
- **`KeptKeysUnique`**: the kept prefix names each key at most once, so the
  first-match searches of forget and mark find the key's one entry.

Both compactions write at `kept`, which never exceeds the index being read,
so each move reads its source before anything overwrites it.

## The instance

`MCRNXdpReplayMarksLayout.cfg`: three entries, two keys, two values. Kept
forgets coalesce across updates, a kept mark is dropped by a forget or
replaced by a mark, and stale cells past each count are left behind.
`MCRNXdpReplayMarksLayoutCapacityOne.cfg` uses one entry and two keys so
new-key capacity refusals are reachable. The full-test witness uses that
same one-entry instance; the other witnesses use three entries.

## Reversed witnesses

Each turns one rule off and must fail with the invariant it names. Every
counterexample below was checked against the C by hand.

1. **`MCRNXdpReplayMarksLayoutBrokenFullOffByOne.cfg`**
   (`FullTestExact = FALSE`): `rocketnet_xdp_replay_marks_forget` refusing
   at `forget_count > capacity`. Violates `InBounds`: in a buffer of one,
   the first key fills the buffer; the second distinct key is written at
   `forgets[1]`, past the array. Repeating one key cannot reach this witness
   because duplicate forgets coalesce.
2. **`MCRNXdpReplayMarksLayoutBrokenKeptSearchPastPrefix.cfg`**
   (`KeptSearchBound = FALSE`): the forget's kept search running
   `i <= kept_mark_count`. Violates `LayoutIsQueue`: a mark of `k1` is
   applied, so the compaction leaves `mark_count = kept_mark_count = 0` with
   `k1` still in `mark_keys[0]`; the next forget of `k1` reads that stale
   cell, calls `drop_kept(0)`, and both counts are decremented from 0, which in
   the code's `uint32_t` wraps to `UINT32_MAX`. With a kept mark in front, the
   same read drops the last real kept mark instead.
3. **`MCRNXdpReplayMarksLayoutBrokenCompactOverUnread.cfg`**
   (`CompactFromZero = FALSE`): `reset_keeping`'s mark loop starting its
   write index at the old `kept_mark_count`, as if the kept prefix stayed in
   place. Violates `LayoutIsQueue`: one mark is kept by a commit; at the next
   commit, with nothing applied, mark 0 is written to index 1, which the loop
   has not read (here past the count, so a phantom copy; with a second pending
   mark there, that mark is overwritten before it is read), and `mark_count`
   becomes 2 for one pending mark.

4. **`MCRNXdpReplayMarksLayoutBrokenForgetDuplicates.cfg`**
   (`DeduplicateForgets = FALSE`): a pending forget is appended again. A
   kernel-refused forget survives a commit; forgetting that source in a
   later update consumes a second cell. `LayoutIsQueue` fails because the
   independent contract keeps one pending entry per source key.

## Runs

The records are in `RUNS.tsv` (group `rnxdpreplaymarkslayout`), made with the
pinned runner as `proofs/tla/README.md` describes.
