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
- Stop asks the reading thread to exit (`CallerStop`, `ReaderExit`); destroy,
  on the caller's thread, joins it and frees both families' memory and the
  race block (`CallerJoin`, `CallerDestroy`).

## Properties

| name | kind | meaning |
|---|---|---|
| `NoUseAfterClose` | invariant | no thread sends, receives or waits on a closed handle or freed memory |
| `FreedOnlyByCaller` | invariant | every free is made by the caller's thread |
| `FreedOnlyAfterJoin` | invariant | memory is freed only after the reading thread is joined |
| `FreedOnce`, `FreedIsClosed` | invariant | each family is freed once, and only closed |
| `ClosedOnlyLoser`, `SeenMeansClosed` | invariant | the reader closes only the losing family, in the step that first sees the commit |
| `EverythingFreed` | liveness | with fair threads, destroy frees every family |
| `LoserClosed` | liveness | a commit leads to the losing family's close |

## Cases

| config | expected |
|---|---|
| `MCRNClientRace.cfg` | pass, every invariant and both properties |
| `MCRNClientRaceBrokenReaderFrees.cfg` | `FreedOnlyByCaller`: the reading thread frees the losing family at its close, as the code did before the close and free were split |
| `MCRNClientRaceBrokenCallerUsesLoser.cfg` | `NoUseAfterClose`: the caller sends to the losing family after its own commit |
| `MCRNClientRaceBrokenFreeBeforeJoin.cfg` | `FreedOnlyAfterJoin`: destroy frees before the join |
| `MCRNClientRaceBrokenReaderClosesEarly.cfg` | `NoUseAfterClose`: the reader closes a family before any commit |

Each counterexample was checked by hand against the code: the reader-frees
trace is commit, then one reading step whose `rocketnet_client_race_kept`
called `rocketnet_client_transport_io_close` on the losing family, which is
the backend's destroy (`portable_io_client_destroy`,
`rocketnet_client_gso_destroy`, `rocketnet_client_iocp_destroy`) and its
`rocketnet_free` on the reading thread; the functional case
`test_family_race_frees_on_caller_thread` counted those frees off the
caller's thread (3 for gso) before the split and none after it.
