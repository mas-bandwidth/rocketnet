# The client family race: who closes and who frees

`RNClientRace.tla` models the ownership of the two sockets a client opens when
its connect token names both an IPv4 and an IPv6 server address
(`standard/TRANSPORT.md` t11 rule 3, `rocketnet_client_race_t` in
`sdk/rocketnet_client_transport.cpp`). It models threads and lifetimes only:
the bytes, the cookie and the handshake are `ClientConnection.tla`'s and
`ServerConnection.tla`'s.

## Threads

- The caller's thread sends to both families until it commits to the family
  whose cookie arrived first (`CallerSend`, `CallerCommit`, one release
  store), then to the kept family only.
- The reading thread (the client I/O thread, the legacy receive worker, or
  the caller's own step) reads both families until it observes the commit
  with acquire. In that step it closes the losing family's operating-system
  handles through the backend's `close` operation and frees nothing
  (`ReaderStep`); afterwards it reads the kept family only.
- The caller fills the I/O ring path's Filling ring and hands it off
  (`CallerHandOff`), or a LATE hand-off leaves it with the caller, its
  destination bound (`CallerHandOffLate`). The destination is the protocol's
  server: the primary family's before the commit, the kept family's after it.
  The commit rebinds a ring bound to the losing server to the kept one
  (`rocketnet_client_io_rebind_held`).
- Sends fail on either open family (`SendFails`); the caller reads
  SEND_FAILED (`CallerRead`) as the race's `send_failures` computes it: both
  families while they are in flight, then the kept family plus the losing
  family's count, which the commit keeps and the reader's close stores again.
- Stop asks the reading thread to exit (`CallerStop`, `ReaderExit`); destroy,
  on the caller's thread, joins it and frees both families' memory and the
  race block (`CallerJoin`, `CallerDestroy`).

What the model leaves to the code, each guarded the same way: the I/O
thread's sends through the ring path and the shutdown drain go through the
race's send and send_batch, which check the commit as the caller's sends do;
the caller's wake and send_failures reach the losing family only before
its commit. A close the reading thread cannot finish (the Windows lifecycle
tracker refusing a handle, an iocp drain with receives still posted) leaves
those handles to destroy, which closes them on the caller's thread before
it frees. A backend with no close operation keeps the losing socket open
until destroy; every built-in backend that races has one (loopback never
races).

## Properties

| name | kind | meaning |
|---|---|---|
| `NoUseAfterClose` | invariant | no thread sends, receives or waits on a closed handle or freed memory |
| `FreedOnlyByCaller` | invariant | every free is made by the caller's thread |
| `FreedOnlyAfterJoin` | invariant | memory is freed only after the reading thread is joined |
| `FreedOnce`, `FreedIsClosed` | invariant | each family is freed once, and only closed |
| `ClosedOnlyLoser`, `SeenMeansClosed` | invariant | the reader closes only the losing family, in the step that first sees the commit |
| `EverythingFreed` | liveness | with fair threads, destroy frees every family |
| `PostCommitSlotsKept` | invariant | no slot committed after the commit leaves for the losing family, where the race's send drops it |
| `SendFailedMonotonic` | invariant | SEND_FAILED never goes down between two reads |
| `LoserClosed` | liveness | a commit leads to the losing family's close |

## Cases

| config | expected |
|---|---|
| `MCRNClientRace.cfg` | pass, every invariant and both properties |
| `MCRNClientRaceBrokenReaderFrees.cfg` | `FreedOnlyByCaller`: the reading thread frees the losing family at its close, as the code did before the close and free were split |
| `MCRNClientRaceBrokenCallerUsesLoser.cfg` | `NoUseAfterClose`: the caller sends to the losing family after its own commit |
| `MCRNClientRaceBrokenFreeBeforeJoin.cfg` | `FreedOnlyAfterJoin`: destroy frees before the join |
| `MCRNClientRaceBrokenReaderClosesEarly.cfg` | `NoUseAfterClose`: the reader closes a family before any commit |
| `MCRNClientRaceBrokenCommitKeepsRing.cfg` | `PostCommitSlotsKept`: the commit leaves a ring bound to the losing server, as the code did before `rocketnet_client_io_rebind_held` |
| `MCRNClientRaceBrokenNoSnapshotAtCommit.cfg` | `SendFailedMonotonic`: only the reader's close keeps the losing family's failures, as the code did before the commit kept them |

Each counterexample was checked by hand against the code: the reader-frees
trace is commit, then one reading step whose `rocketnet_client_race_kept`
called `rocketnet_client_transport_io_close` on the losing family, which is
the backend's destroy (`portable_io_client_destroy`,
`rocketnet_client_gso_destroy`, `rocketnet_client_iocp_destroy`) and its
`rocketnet_free` on the reading thread; the functional case
`test_family_race_frees_on_caller_thread` counted those frees off the
caller's thread (3 for gso) before the split and none after it.

The commit-keeps-ring trace is a LATE hand-off before the commit (the ring
bound to the primary, IPv6, server), a commit to the other family, and a
hand-off whose attempt commits a slot after the commit: the ring leaves for
the IPv6 server and `rocketnet_client_race_send_batch`, with family 1
committed, sends nothing of it. `test_client_race_commit_rebinds_a_late_ring`
holds the I/O mutex for the update that resends the cookie request, so that
hand-off is LATE, then lets the cookie from IPv4 commit the race in an update
whose connection request joins the same ring; before the rebind the request
never left, and with it the request reaches IPv4 in that update.

The no-snapshot trace is a send failure on family 0, a read (1), a commit
to family 1, and a read before the reader's step: the kept family's count
plus `closed_send_failures`, still 0, is 0.
`test_client_race_send_failed_holds_across_the_commit` injects send failures
on the losing family, reads the counters in the update that commits, before
the reading step, and checks SEND_FAILED did not go down; then steps and
checks it is unchanged.
