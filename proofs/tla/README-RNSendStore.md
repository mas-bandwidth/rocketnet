# RNSendStore

One class of the message layer's send store: docs/CHANNELS.md sections 4 and 15,
implemented by `rn_send_store_alloc` and `rn_send_store_free_checked` in
`sdk/rocketnet_allocator.cpp`. The three classes share no state, so one class is
the whole model.

The module was written with layer 1 on its attempt branch (`a7758f3`) and is here
unchanged. The code it is checked against below is main at `3ad4179`:
`rn_send_store_alloc` (`sdk/rocketnet_allocator.cpp:211-248`, the all-or-none test
at line 222) and `rn_send_store_free_checked` (lines 250-280, the `IN_USE | HEAD`
test at line 265), and the unit test `test_channels_allocator_send_store`
(`test_channels.cpp:1195-1332`).

- **The free list** is a stack threaded through the chunks' links. Alloc pops from
  the head, and free pushes each chunk of a chain back onto the head.
- **A send** takes a chain of 1 to `MaxBlock` chunks, all or none.
- **Links.** An allocated chunk's link carries `IN_USE`, and a chain's first chunk
  also carries `HEAD`. Free refuses a handle whose head does not carry both.

## Invariants

- **`NoDoubleAllocate`**: no chunk appears twice on the free list, is both free and
  held, or appears in two chains.
- **`NoLeak`**: free plus used equals total.
- **`RefuseOnlyWhenFull`**: a refusal happens only when the free chunks could not
  hold the send whole.
- **`ChainsBounded`**: no chain is longer than `MaxBlock`.
- **`TypeOK`**: every variable has its declared type.

## What the model leaves to the caller

A second free is modelled only while none of the chain's chunks has been
reallocated. Once a chunk has been reallocated, a free through the old handle
looks exactly like the new owner's own free, and no check inside the allocator
can tell them apart. The layers above own that case:

- At its free, the descriptor drops its handle (layer 1).
- The per-number table never names a freed descriptor (layer 3).

## How to run it

On a Linux bench with TLC 2.19 or later, from `proofs/tla/`:

```sh
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNSendStore.cfg MCRNSendStore
```

The small instance:

| Constant | Value |
|---|---|
| `Chunks` | 4 |
| `Senders` | `{s1, s2, s3}` |
| `MaxBlock` | 3 |

The committed run, with both rules on, generates 47,407,249 states and finds
4,941,552 distinct states, with no error.

## The reversed witnesses

Each witness is the committed cfg with one CONSTANTS line changed, and each must fail:

```sh
sed -e 's/GuardDoubleFree = TRUE/GuardDoubleFree = FALSE/' MCRNSendStore.cfg > /tmp/w-guard.cfg
sed -e 's/AllOrNone       = TRUE/AllOrNone       = FALSE/' MCRNSendStore.cfg > /tmp/w-all.cfg
```

1. **`GuardDoubleFree = FALSE`**: `Invariant NoDoubleAllocate is violated`, at depth 4.
   - **The trace:**
     1. s1 allocates chunk 1.
     2. s1 frees it.
     3. s1 frees it again.
     4. The free list is now `<<1, 1, 2, 3, 4>>`.
   - **Against the code, by hand:**
     1. The first free sets `link[1]` to the old head, chunk 2, and that clears
        `IN_USE`.
     2. Without the test, the second free reads chunk 1's link as plain. It writes
        `link[1] = free_head`, which is now 1. That makes a self-loop, and
        `free_count` becomes 5 of 4.
     3. The next two allocations both get chunk 1.
   - **With the guard:** the second free finds `IN_USE | HEAD` clear and returns
     false, changing nothing (`sdk/rocketnet_allocator.cpp:265-266`). On main,
     `test_channels_allocator_send_store` pins exactly this: it allocates one
     chunk, frees it, then checks that `rn_send_store_free_checked` on the same
     handle returns false and leaves the class's free count unchanged
     (`test_channels.cpp:1321-1329`, "a refused double free changes nothing").
2. **`AllOrNone = FALSE`**: `Invariant NoLeak is violated`, at depth 4.
   - **The trace:**
     1. s1 holds chunk 1, and s2 holds chunk 2.
     2. s3 asks for 3 chunks with 2 free.
     3. s3 takes those 2 chunks, then reports the refusal.
     4. Chunks 3 and 4 are now in use and in no chain.
   - **Against the code, by hand:** suppose `free_count < needed`
     (`sdk/rocketnet_allocator.cpp:222`) is not tested first, and the pop stops at
     the end of the list. The chunks popped before the refusal are marked
     `IN_USE` and returned to no one. That is the accounting
     `rn_soak_check_accounting` asserts on main (`test_channels.cpp:1589-1607`:
     free plus used equals total, for each class and for the descriptor pool),
     run inside `test_channels_allocator_no_live_allocation`.
   - **On main the rule is pinned** by `test_channels_allocator_send_store`
     (`test_channels.cpp:1298-1306`, "a block bigger than what is left takes
     nothing"): a request one chunk larger than the free count returns
     `RN_CHUNK_NONE` and the free count is unchanged. The rule-removed probe that
     fails that check was run on the attempt branch, not on main.

## Runs

The three rows are not in `proofs/tla/RUNS.tsv` on main (it holds only the
`MCClientConnection` and `MCLink` rows); they are owed, and they are these, from
the rerun on `hetzner2` (TLC 2.19, `-workers 4` for the committed run, 1 for each
witness, 2026-10-13):

```
MCRNSendStore	MCRNSendStore.cfg (GuardDoubleFree, AllOrNone TRUE; TypeOK, NoDoubleAllocate, NoLeak, RefuseOnlyWhenFull, ChainsBounded)	47407249	4941552	pass	hetzner2	2026-10-13
MCRNSendStore	MCRNSendStore.cfg with GuardDoubleFree = FALSE (reversed witness)	113	101	fail as expected: NoDoubleAllocate violated at depth 4, chunk 1 twice on the free list	hetzner2	2026-10-13
MCRNSendStore	MCRNSendStore.cfg with AllOrNone = FALSE (reversed witness)	36	36	fail as expected: NoLeak violated at depth 4, a refused block took the last two chunks	hetzner2	2026-10-13
```

The committed run took 20 s at depth 28, the same counts as the run on `vision`
recorded with `a7758f3` (47,407,249 states, 4,941,552 distinct). The witness
counts differ from that record (149/136 and 34/34 there: how many states were on
the queue when TLC stopped), with the same violations at the same depth.
