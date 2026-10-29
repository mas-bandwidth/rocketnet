# RNPortableRx: the portable server's receive worker

`RNPortableRx.tla` models the receive half of the portable server transport
in `sdk/rocketnet_driver_builtin.cpp`. The same code serves the macOS msgx
server, which selects only its socket pump at create. The modelled functions
are:

- `portable_rx_step`, the worker's pass, run by `portable_rx_worker`;
- `portable_rx_publish`, `portable_rx_acquire` and `portable_receive`;
- the RX half of `portable_stop` and `portable_stopped`.

The rule it checks is the bounded N-buffer handoff
(`docs/decisions/BUFFER-HANDOFF-DECISION-20261021.md`):

- each bank has exactly one owner;
- only bank indices change under the mutex;
- the socket read, the copies and the producer reset run outside the mutex;
- main only ever tries the mutex.

## The abstraction

- **Banks.** `NumBanks` banks of up to `Cap` datagrams. Each bank's
  `data[b]` is the prefix its count covers. Each bank is in exactly one of:
  - `prod`, the worker's producer;
  - `cons`, main's consumer, with `cursor` into it;
  - the `free` FIFO;
  - the `ready` FIFO.
- **Socket.** `sock` is the receive buffer, oldest first. `Arrive` appends
  the next ordinal. A read takes 1..`Cap` datagrams from the front, which is
  `rocketnet_platform_socket_receive_packets` assigning `bank->count`.
- **Worker.** `wpc` steps through `WTop` (quit test), `WPublishEmpty` or
  `WLock`/`WSwap` (the publish under the mutex), `WReset` (the new
  producer's count cleared after the release), and `WRead`. The two waits
  are:
  - `WWaitFull`: the wake alone, with every bank full;
  - `WWaitIdle`: the wake or readability, after an empty read.
- **Main.** Main's steps are:
  - `MCall` begins a `portable_receive` call with a budget of `Budget`
    datagrams.
  - `MCopy` copies one datagram at the cursor.
  - `MTry`/`MTryFail` is the single try of the mutex once the consumer is
    completely copied.
  - `MSwap` returns the consumer and takes the oldest ready bank.
  - `MWake` signals the worker, only when it returned a bank to an empty
    free FIFO.
  - `MEnd` ends the call when its budget is spent.
- **Stop.** `MStop` sets `stopping`. `MStop1`/`MStop2` set quit and signal
  the wake, in the order the rule says. `MPollStopped` is main polling
  `portable_stopped`.
- **Mutex.** `lock` is `none`, `worker` or `main`. The worker's acquire
  blocks (it may wait). Main's is a try that ends the call.

`Timeout` stands for the waits' 0.1 s bound. The liveness controls run with
`Timeout = FALSE`, so progress follows from the wake protocol alone and not
from the timeout.

## Properties

- `ExclusiveOwnership`: every bank appears exactly once among the producer,
  the consumer, `free` and `ready`. No bank is reachable by two threads.
- `ExactDelivery`: three sequences, concatenated, are exactly
  `1..arrived`:
  - the datagrams main returned;
  - the unread tail of the consumer;
  - the ready banks in FIFO order, then the producer, then the socket.

  So nothing is lost, duplicated or reordered.
- `CursorInBank`, `MainLockIsBrief`, `TypeOK`.
- `EveryDatagramDelivered` (liveness): every datagram that reaches the
  socket is delivered unless stop begins.
- `StopCompletes` (liveness): once stop begins, the worker exits.

The fairness assumptions are:

- weak fairness on the worker's steps;
- strong fairness on the worker's blocking acquire and on main's try (the
  mutex is held for an index swap only);
- weak fairness on main completing its calls and its stop steps.

## Cases and the checks against the code

| Config | Expected | What it shows |
|---|---|---|
| `MCRNPortableRx.cfg` | pass | Three banks, reads of two, a budget of one (the consumer tail is retained across calls), four datagrams. All invariants and both liveness properties. |
| `MCRNPortableRxTwoBanks.cfg` | pass | The same with two banks, so every publish needs main to return its consumer first. |
| `MCRNPortableRxBrokenOverwrite.cfg` | `ExactDelivery` | `HoldOnFull = FALSE` |
| `MCRNPortableRxBrokenReturnEarly.cfg` | `ExactDelivery` | `ReturnWhenRead = FALSE` |
| `MCRNPortableRxBrokenWakeFirst.cfg` | `StopCompletes` | `QuitBeforeWake = FALSE` |
| `MCRNPortableRxBrokenNoWake.cfg` | `EveryDatagramDelivered` | `WakeOnReturn = FALSE` |

Each reversed witness was read against the code by hand (2026-10-22):

- **Overwrite.**
  - In the trace, the worker publishes banks 1 and 2 and fills bank 3. With
    `free` empty, the refused publish falls through to a read, which
    replaces datagram 3 with datagram 4.
  - In the code, `portable_rx_step` returns `PORTABLE_RX_WAIT_FULL` when
    `portable_rx_publish` refuses. It never reaches the read, so the full
    producer is held.
- **ReturnEarly.**
  - In the trace, main copies datagram 1 of bank 1 with a budget of one. It
    then returns bank 1 to `free` while datagram 2 is unread, and the worker
    may refill it.
  - In the code, `portable_rx_acquire` returns `true` without touching the
    mutex while `rx_cursor < count`. It returns the consumer only once the
    consumer is fully copied. A call that ends on its budget leaves the
    consumer and its cursor in place.
- **WakeFirst.**
  - In the trace, stop signals the wake first. The worker's idle wait
    consumes the signal and sees quit still clear. Its next read is empty,
    and it waits forever, because no timeout and no arrival come.
  - In the code, `portable_stop` stores `quit` and only then calls
    `rocketnet_platform_wake_set( server->rx_wake )`. `portable_rx_step`
    tests quit at the top of every pass.
- **NoWake.**
  - In the trace, the worker holds bank 3 (datagrams 3 and 4) in
    `WWaitFull` with `free` empty. Main copies banks 1 and 2 and returns
    them, but never signals. With no timeout the worker never publishes, and
    datagrams 3 and 4 are never delivered.
  - In the code, `portable_rx_acquire` computes
    `freed = rx_consumer >= 0 && rx_free_count == 0` under the mutex and
    sets `rx_wake` after the release. The worker can be waiting for a free
    bank only when the free FIFO was empty, which is the one case that needs
    the signal.

The unit cases `test_portable_rx_handoff` and `test_portable_rx_worker_steps`
(`test_unit_portable_rx.cpp`) drive these exact transitions on one thread,
with no socket read, no worker thread and no clock.
