# RNBankFifoLayout

The hand-written bank queues of the server and client transports at their
layout, with the code's own fields. [RNPortableRx](README-RNPortableRx.md),
`RNGsoHandoff.tla`, `RNBufferHandoff.tla`, `RNRioWorker.tla` and
`RNClientIocp.tla` check these handoffs as sequences and sets; this model
checks how the queues are laid into their index arrays, which they cannot see.
[RNPortableTx](README-RNPortableTx.md) already carries the portable transmit
queues at their layout together with the worker's protocol; this module
carries the queue arithmetic alone, for every transport that repeats it.

One parameterized module covers every shape the code uses:

| Instance | Code | Free queue | Ready | Consumer | Init |
|---|---|---|---|---|---|
| `MCRNBankFifoLayout.cfg` | `portable_rx_publish`, `portable_rx_acquire`, `portable_tx_publish`, `portable_tx_worker` (`sdk/rocketnet_driver_builtin.cpp`); `rocketnet_server_gso_publish_send`, `rocketnet_server_gso_take_send`, `rocketnet_server_gso_return_send` (`sdk/rocketnet_server_gso_linux.cpp`) | FIFO of 3 | FIFO of 3 | one bank | literal `{ 1, 2, 0 }` |
| `MCRNBankFifoLayoutLoopFour.cfg` | `rocketnet_xdp_buffer_exchange_t` (`sdk/rocketnet_server_socket_linux.cpp`), `rocketnet_buffer_handoff_t` (`sdk/rocketnet_buffer_handoff.h`) | FIFO of 4 | FIFO of 4 | one bank | loop |
| `MCRNBankFifoLayoutHandoff.cfg` | `rocketnet_buffer_handoff_publish` with `max_ready` 1 | FIFO of 3 | FIFO of 3, at most 1 | one buffer | loop |
| `MCRNBankFifoLayoutHandoffMaxReadyAll.cfg` | the same with `max_ready` = N, which its assert refuses | FIFO of 3 | FIFO of 3 | one buffer | loop |
| `MCRNBankFifoLayoutLifoSend.cfg` | `rocketnet_server_iocp_send_packets`, `rocketnet_iocp_send_worker` (`sdk/rocketnet_server_iocp_windows.cpp`) | stack of 3 | FIFO of 3 | one bank | literal `{ 1, 2, 0 }` |
| `MCRNBankFifoLayoutLifoExposed.cfg` | `rocketnet_iocp_publish_receive_bank`, `rocketnet_server_iocp_receive_packets`; `rocketnet_rio_worker_pass`, `rocketnet_server_rio_receive_packets` (`sdk/rocketnet_server_rio_windows.cpp`) | stack of 4 | FIFO of 4 | up to 4 exposed | loop |
| `MCRNBankFifoLayoutPosted.cfg` | `rocketnet_client_iocp_dequeue`, `rocketnet_client_iocp_receive_batch` (`sdk/rocketnet_client_iocp_windows.cpp`) | none: posted to the kernel | FIFO of `num_receives` = 3 in an array of 4 | up to 3 | every buffer posted |

## State

| Model | Code |
|---|---|
| `producer` | `producer`, `rx_producer`, `send_producer`, `fill_index`, `fill_bank`; `NoBank` for the iocp client |
| `freeArr`, `freeHead`, `freeCount` | `free_index`, `free_head`, `free_count` (and `rx_`, `send_` forms); `free_banks` with `free_count` for a stack |
| `readyArr`, `readyHead`, `readyCount` | `ready_index`, `ready_banks`, `send_ready`, `ready` with their heads and counts |
| `exposed`, `exposedCount` | `exposed_banks`, `exposed_count`; the one bank a single consumer holds (`rx_consumer`, the worker's `index`, a claimed exchange index) |
| `posted[b]` | the iocp client's `ROCKETNET_CLIENT_IOCP_RECEIVE_POSTED` |
| `ArrLen` | the arrays' length: N, except the iocp client's `ROCKETNET_CLIENT_IOCP_RECEIVES` |
| `oob` | a position outside an array was read or written; every action stops |
| `absReady` | ghost: the banks published and not yet taken, in order |

A FIFO is a head and a count: the tail is `( head + count ) % N` (`TailMod`),
a head advances `( head + 1 ) % N` (`HeadMod`). A stack is a count: push at
`count++`, pop at `--count`. The literal initializer is `{ 1, 2, 0 }` with the
rest of the array zero (C aggregate initialization) and a count of N - 1; the
loop puts 1 .. N - 1 in order. `rocketnet_buffer_handoff_init` skips the
producer it is given; the model's producer is 0, where it is the loop.

## Actions and the code they mirror

- **`Publish`**: under the mutex, no exchange when no bank is free
  (`free_count == 0`) or `max_ready` banks are ready; otherwise the producer's
  bank goes to the ready tail and the free head (or the stack's top) becomes
  the producer. `portable_rx_publish`, `portable_tx_publish`,
  `rocketnet_server_gso_publish_send`, `rocketnet_xdp_buffer_exchange_t::publish`,
  `rocketnet_buffer_handoff_publish`, `rocketnet_server_iocp_send_packets`,
  `rocketnet_iocp_publish_receive_bank` and the WPublish step of
  `rocketnet_rio_worker_pass`.
- **`Complete(b)`**: the iocp client's kernel completes posted buffer `b` with
  a datagram, in any order, and `rocketnet_client_iocp_dequeue` puts it at
  the ready tail `( ready_head + ready_count ) % num_receives`.
- **`Take`**: the consumer takes the ready head onto its list, the head
  advancing modulo N. `portable_rx_acquire`, `portable_tx_worker`,
  `rocketnet_server_gso_take_send`, `claim`, `rocketnet_buffer_handoff_take`,
  `rocketnet_iocp_send_worker`, `rocketnet_server_iocp_receive_packets`,
  `rocketnet_server_rio_receive_packets` and `rocketnet_client_iocp_receive_batch`.
- **`Release`**: the consumer returns every bank it holds, in order, to the
  free tail (or the stack), or for the iocp client posts it again.
  `portable_rx_acquire`, `portable_tx_worker`, `rocketnet_server_gso_return_send`,
  `recycle`, `rocketnet_buffer_handoff_release`, `rocketnet_iocp_send_worker`
  and the receive calls above.

The code runs some of these together under one lock (`portable_rx_acquire`
releases then takes; the iocp and rio receive calls return every exposed bank,
then take the ready banks that fit). The model lets every action interleave,
which includes those orders.

## Invariants

- **`ExactlyOnePlace`**: read through the arrays, every bank is in exactly one
  place: the producer, one free position, one ready position, the consumer's
  list, or posted.
- **`ReadyMatches`**: the ready positions from the head, read through the
  array, are the banks published and not yet taken, in publish order.
- **`InBounds`**: no position outside an array is read or written.
- **`CountsInRange`**: the free and ready counts stay in 0..N.

## The instances

Each control bounds the publishes (`MaxPub`, 2N + 2: every queue wraps at
least twice) and is checked with every invariant. Terminal states are the
harness's bound, so the deadlock check is off (`ignore-terminal`).

`MCRNBankFifoLayoutHandoffMaxReadyAll.cfg` answers whether `max_ready == N`
overflows the ready ring: it passes. The producer always holds one buffer, so
at most N - 1 are ready and `free_count == 0` refuses before
`ready_count >= max_ready` could matter. The assert
`max_ready < NumBuffers` in `rocketnet_buffer_handoff_publish` is therefore a
statement of the caller's intent, not a guard against an overflow.

## Reversed witnesses

Each turns one rule off and must fail its declared invariant. Every
counterexample was checked against the C by hand.

1. **`MCRNBankFifoLayoutBrokenFullOffByOne.cfg`** (`FreeSlack = 1`, the empty
   test one late): two publishes take banks 1 and 2 (free_count 0); a third
   proceeds and pops `free_index[free_head = 2]`, the stale 0 the literal put
   there. In `portable_tx_publish` with `free_count == -1` for `== 0`, the
   producer becomes bank 0 while bank 0 is still in the ready queue: main
   fills the bank the worker is sending. Fails `ExactlyOnePlace`.
2. **`MCRNBankFifoLayoutBrokenNoTailMod.cfg`** (`TailMod = 0`): publish,
   publish, take, release, take, release: the second release writes
   `free_index[free_head + free_count] = free_index[2 + 1]`, one past the
   array. In `portable_server_t` that is `free_head` itself. Fails `InBounds`.
3. **`MCRNBankFifoLayoutBrokenWrapTail.cfg`** (`TailMod = 2`, the tail % (N - 1)
   while the head advances % N): publish banks 0 and 1, take 0, release it to
   `free_index[( 2 + 0 ) % 2 = 0]`, publish 2 to `ready_index[( 1 + 1 ) % 2 = 0]`.
   The consumer reads the ready queue from head 1 as `ready_index[1]`,
   `ready_index[2]` = 1, 0: bank 0, the producer, instead of bank 2. Fails
   `ReadyMatches`.
4. **`MCRNBankFifoLayoutBrokenLiteralFour.cfg`** (`InitKind = "literal"`, N = 4,
   as `portable_server_t` would read with `PORTABLE_RX_BANKS` 4): the free
   queue is 1, 2, 0 with bank 0 the producer, and bank 3 is nowhere. Fails
   `ExactlyOnePlace` in the initial state. The literal is right at three
   banks, which every literal in the code is (`PORTABLE_TX_BANKS`,
   `PORTABLE_RX_BANKS`, `ROCKETNET_GSO_TX_BANKS`, `ROCKETNET_IOCP_SEND_BANKS`);
   the loop initializers are right at every N (`MCRNBankFifoLayoutLoopFour.cfg`,
   `MCRNBankFifoLayoutLifoExposed.cfg`).
5. **`MCRNBankFifoLayoutBrokenMixedFreeOrder.cfg`** (pushed as a stack, popped
   as a FIFO): publish (bank 1 out, free_head 1, free_count 1), take bank 0,
   release it at `free[free_count] = free[1]`, over bank 2, which was the
   free queue's only live entry. Bank 2 is lost and bank 0 is free twice.
   Fails `ExactlyOnePlace`.
6. **`MCRNBankFifoLayoutBrokenPostedArrayMod.cfg`** (the iocp client's tail
   taken % `ROCKETNET_CLIENT_IOCP_RECEIVES` (4) while the head advances %
   `num_receives` (3)): buffers 0 and 1 complete, 0 is copied and posted
   again, 0 and 2 complete; buffer 2 lands at `ready[( 1 + 2 ) % 4 = 3]`, and
   `rocketnet_client_iocp_receive_batch` reads positions 1, 2, 0: buffer 0
   twice, buffer 2 never. Fails `ReadyMatches`.

## Findings

No counterexample against the code's arithmetic. Two notes:

- The `{ 1, 2, 0 }` literals are right only at three banks, and nothing but
  the constant's value keeps them so (witness 4).
- `max_ready == N` cannot overflow the ready ring (the passing
  `HandoffMaxReadyAll` instance).
