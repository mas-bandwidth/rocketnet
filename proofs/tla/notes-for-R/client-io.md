# Notes for package R from M8 (client-io)

Everything below is for package R (records) or package C (comment-only
re-citations in sdk/ and tests/). M8 edited only its own modules, their
READMEs, cfgs, MC modules, and its own rows of COVERAGE.tsv and CASES.tsv.

## MM117: RNClientIo is historical

RNClientIo.tla now opens with a "HISTORICAL DESIGN" banner (README-RNClientIo.md
already said so). The status belongs to R:

- COVERAGE row `client-io` (line 30, RNClientIo, sdk/rocketnet_client_transport.cpp)
  and row `client-io-core` (line 106, RNClientIo, sdk/rocketnet_client_io.h) say
  `current`; their notes say historical. Proposed: status `owed` for both, with
  the note "Historical design only; no current code implements it: the client
  I/O runs three send banks under rocketnet_buffer_handoff_t (RNBufferHandoff)
  and receives through sdk/rocketnet_receive_handoff.h (RNReceiveHandoff)."
- The RNClientIo cases in CASES.tsv (group `rnclientio`, lines 255-261, 360,
  361, 431 and the passing controls) can then leave `required`, the same way
  the RNBufferHandoffBase cases did.
- The citation check counts the code's "as history" citations of RNClientIo
  actions (sdk/rocketnet_client_io.h, sdk/rocketnet_client_transport.cpp) as
  bindings. Proposed: check_tla_coverage skips a citation to a module whose
  every row is `owed` or historical, or R rewords those citations with C.

### For package C: RNClientIo cited as current (MM117, strengthened by M3)

These comments cite RNClientIo actions as if they were the current design.
The ones already marked "(historical two-ring design)" or "(historical
two-buffer design)" in sdk/rocketnet_client_io.h and
rocketnet_client_transport_update are left out. Each needs one of two
rewrites: mark the citation historical, or re-cite the current model
(RNBufferHandoff for the send banks and the I/O thread's lifecycle,
RNReceiveHandoff for the receive, RNClientIoShutdown for stop and destroy,
RNClientSendCursorLayout for the ring sends, RNClientIocp for the posted
receives). Sites are listed by the symbol the comment sits above or inside,
with the count when one symbol has more than one, and the actions named.

sdk/rocketnet_client_transport.cpp (24 sites):
- rocketnet_client_transport_create_impl (3): "every RNClientIo action named here"; OnThread; IoSend, IoFinishRing, IoReceive, IoWait, IoWake.
- rocketnet_client_transport_stop: Stop.
- rocketnet_client_transport_destroy (2): Join and Free; Done.
- rocketnet_client_transport_io_hand_off (5): HandOff and Defer; HandOffTo, DeferTo and Reopen; WaitDone; Reopen (in the body).
- rocketnet_client_transport_io_commit_packet (2): Write, Commit, Disconnect.
- rocketnet_client_transport_update (3): StepBegin, StepEnd and OnCaller; CallerStep twice, as trailing comments on the calls to rocketnet_client_io_step.
- rocketnet_client_transport_start_packet (2): Write and OnGame; Write.
- rocketnet_client_transport_finish_packet_unchecked (2): Commit, twice.
- rocketnet_client_transport_send_packets (2): HandOff or Defer; CallerStep, a trailing comment on the call to rocketnet_client_io_step.
- rocketnet_client_transport_disconnect (2): Disconnect; Disconnect, Write, Commit.

Other sdk files (the backends' I/O-thread contract):
- sdk/rocketnet_client_io.h: the file header ("models the two-ring, two-buffer design this code"); rocketnet_client_io_discard_at_stop (IoSend, IoReceive, IoWait, IoWake, IoClearWake and IoExit).
- sdk/rocketnet_client_gso.h: the header comment on the I/O thread owning the socket.
- sdk/rocketnet_client_gso_linux.cpp: the file header; rocketnet_client_gso_create (Init, Stop, Join, Free); rocketnet_client_gso_destroy (Stop, Join, Free, FreedOnlyAfterJoin); rocketnet_client_gso_send_batch_operation (IoSend); rocketnet_client_gso_receive_batch_operation (IoReceive); rocketnet_client_gso_wait (IoWait, IoWake, FullWaitsOnWake).
- sdk/rocketnet_client_iocp.h (2): the header comments (the I/O thread; a buffer never copied while posted).
- sdk/rocketnet_client_iocp_windows.cpp: the file header; rocketnet_client_iocp_wait (IoWait); rocketnet_client_iocp_create (initial); rocketnet_client_iocp_destroy (Stop and destroy order); rocketnet_client_iocp_send_batch (IoSend); rocketnet_client_iocp_receive_batch_operation (IoReceive); rocketnet_client_iocp_wait_operation (IoWait, the wake).
- sdk/rocketnet_client_protocol.cpp: rocketnet_client_protocol_process_record ("RNClientIo as an abstraction of the receive handoff: IoPublish stands for ..."; this one already says abstraction and may only need RNReceiveHandoff named).
- sdk/rocketnet_counters.h: the ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED entry (Defer). It is in a generated counter table, so the change belongs to the counter registry's source.
- sdk/rocketnet_driver_builtin.cpp: struct portable_io_client_t (the I/O thread of docs/CLIENT-IO.md); portable_io_client_create (Init, Stop, Join, Free); portable_io_client_destroy (Stop, Join, Free, FreedOnlyAfterJoin); portable_io_client_send and portable_io_client_send_batch (IoSend, IoReceive); portable_io_client_wait (IoWait, IoWake, FullWaitsOnWake).
- sdk/rocketnet_platform.cpp: rocketnet_platform_sendmmsg (IoSend and IoReceive); struct rocketnet_platform_wake_t (3: IoWait, IoWake; IoWake, IoWait; IoWait and IoWake); rocketnet_platform_wake_create (Init: wake = FALSE); rocketnet_platform_wake_wait (IoWait and IoWake); rocketnet_platform_wake_set (HandOffTo, wake' = TRUE).

tests/ also names RNClientIo actions in comments (tests/test_unit_client_io_step.cpp, tests/test_unit_client_io_stop.cpp, tests/test_unit_client_io_transport.cpp, tests/test_unit_fake_client_driver.cpp, tests/fake_client_driver.h, tests/public_step_pair.h, tests/test_unit_client_msgx.cpp, tests/functional/rocketnet_functional_tests.cpp, tests/functional/live_matrix.cpp). The same rewrite applies, and `git grep -n "RNClientIo[^A-Za-z]" -- tests` lists them.

Roadmap entry (sexp text; R adds it to docs/roadmap.sexp):

```
  (item "client-io-step-mode-carrier" :group "after-v1-1-landing" :area "proofs"
   :title "Carry NoIoThreadActionInStepMode onto a current client I/O model"
   :text "RNClientIo is historical (the two-ring, two-buffer design); its step-mode property NoIoThreadActionInStepMode, with the reversed witness MCRNClientIoBrokenStepStartsThread.cfg, is the only model of rocketnet_client_io_step: an endpoint created step-driven never starts the I/O thread and every I/O action runs on the caller's step. Carry the property, its step-driven control and its witness onto RNBufferHandoff (send) or RNReceiveHandoff (receive) as a mode constant, cite it from rocketnet_client_io_step, and retire the RNClientIo cases."
   :date "2026-11-13" :origin "2026-11-10 review, models row MM117 (fix round 1, package M8)")
```

## MM128: server-iocp-stop-send maps RNBufferHandoff by analogy

COVERAGE row `server-iocp-stop-send` (line 142, RNBufferHandoff,
sdk/rocketnet_server_iocp_windows.cpp) binds the single-producer mutex
handoff onto the IOCP send lanes and their atomics by analogy: the IOCP code
has no rocketnet_buffer_handoff_t. The IOCP stop also caps the count it
sends at send_queue_size without saying so (server_iocp_windows.cpp, the stop
send around lines 1730-1750). Proposed note text: "By analogy only: the IOCP
lanes are atomics, not the mutex handoff; the stop's final send is capped at
send_queue_size. The lanes are RNIocpSendLanes (trace-checked)." Status: keep
`current` only if R agrees the analogy is worth keeping; otherwise `owed`.

## MM217 / MM223: a README for RNClientSendCursorLayout

RNClientSendCursorLayout has no README (README creation is R's). The module
header now carries the MM223 equivalence; the README should say the same.
Proposed paragraph:

> **Destination and family.** The model addresses a ring to a family. The
> code addresses each slot to a server and takes its family from the
> destination's address family (`rocketnet_client_race_family_of`); after the
> commit `rocketnet_client_race_send_progress` and
> `rocketnet_client_race_send_batch` drop a slot not of the kept family. The
> two are equal: only the two server addresses are ever sent to
> (`server_address[0]` IPv4, `server_address[1]` IPv6), so a destination's
> address family names its server, and the commit sets
> `protocol.server_address` to `server_address[kept]` and rebinds a held ring
> bound to the losing server (`rocketnet_client_io_rebind_held`).

RNClientIocp also has no README; its documentation is in the
MCRNClientIocp.tla header. A README is R's to add if wanted.

## proofs/tla/README.md and docs/TLA-INVENTORY.md lines

New cases (all in M8's existing groups):

- shutdownownership: MCRNClientIoShutdownReachCloseDrops.cfg (reach, NoCloseLoss)
- rnclientlinkstop: MCRNClientLinkStopReachAllDropped.cfg (reach, BurstReachesServer)
- rnclientdelivery: MCRNClientDeliveryBrokenRefuseVanishes.cfg (CountedImpliesHanded)
- rnclientiocp: MCRNClientIocpStalls.cfg (pass), MCRNClientIocpBrokenNoGiveUp.cfg
  (CloseEventuallyFrees), MCRNClientIocpBrokenNoGiveUpDrain.cfg (PostedDrainAfterClose)
- rnclienttransportorder: MCRNClientTransportOrderBrokenBudgetOpens.cfg (NoSocketForRefusedPlan)
- clientrace: MCRNClientRaceBrokenNoLiveCheck.cfg (FreedOnce), MCRNClientRaceBrokenDestroyTwice.cfg (FreedOnce), MCRNClientRaceBrokenNoReaderClose.cfg (temporal, LoserClosed), MCRNClientRaceBrokenNoStepClose.cfg (StepClosesAtSight)
- rnportabletx: MCRNPortableTxBrokenNoWrap.cfg replaced by MCRNPortableTxBrokenNoHeadMod.cfg and MCRNPortableTxBrokenNoTailMod.cfg (InBounds; MA011)
- cryptosource: MCRNCryptoSourceBrokenInitNoLock.cfg (AtMostOneSource)
- rnportablerx: MCRNPortableRxBsd.cfg (pass), MCRNPortableRxBsdBrokenNoTimeout.cfg
  (EveryDatagramDelivered)

Inventory line changes, one per module:

- RNClientIo: historical (MM117).
- RNBufferHandoffBase: historical, header says so (MM129).
- RNClientIoShutdown: the exit's bounded drain and the close that drops a late datagram.
- RNClientLinkStop: the I/O-core stop drains; sent copies can be dropped.
- RNClientDelivery: the cursor refuses a payload larger than the caller's buffer.
- RNClientIocp: failed completions, refused re-posts, destroy's leak.
- RNClientTransportOrder: the operator memory budget.
- RNCryptoSource: rocketnet_init as an actor.
- RNPortableRx: BSD's idle wait.
- RNClientRace: LoserClosed stated over the reader's own close (ghost closer), StepClosesAtSight added (MA008).
- RNPortableTx: the wrap witness split, one modulus each (MA011).

## Cross-family rows

None created. RNClientLinkStop's Stop on the iocore path is now named from
RNClientIoShutdown's README and COVERAGE row (shutdown-request-completion) as
the request's one backend call (MM119); no record links them otherwise.

## For package C: comment-only re-citations in sdk/

Cited by symbol (the coverage check refuses line numbers under proofs/tla);
the line numbers at this branch's head are given in words.

- sdk/rocketnet_client_io.h (rocketnet_client_io_discard_at_stop), the comment
  block just above the function, lines eight hundred seventy-two to
  eight hundred seventy-eight (MM118). Now: "proofs/tla/RNClientIoShutdown.tla
  WorkerExit; ... it reads every datagram still queued on the socket, counts each
  one in receive_discarded_at_stop ...". New text: "proofs/tla/RNClientIoShutdown.tla
  WorkerQuiesce and WorkerExit; I/O thread, at its exit, after its last send and
  before stopped is stored. The worker is the transport's last reader: it reads
  what is queued on the socket in bounded reads (at most
  ROCKETNET_CLIENT_IO_DISCARD_MAX datagrams, ending early at an empty or refused
  read), counts each one in receive_discarded_at_stop (the transport surfaces it
  as CLIENT_RECEIVE_DISCARDED_AT_STOP), waits for the stop request's wake to
  return, and closes the transport's handles with the close operation
  (sdk/rocketnet_driver.h). A datagram that arrives after the last read, or past
  the cap, goes with the close unread and uncounted; one that arrives after the
  close meets a closed port, and none waits unread until destroy frees it."
- sdk/rocketnet_client_transport.cpp (the comment block above
  rocketnet_client_transport_stop that begins "proofs/tla/RNClientIoShutdown.tla
  abstracts this request/completion ownership", its third line, line two
  thousand eight hundred thirty) (MM119). Now: "Requests never join, call
  a backend send, or wait for a ring." New text: "Requests never join or wait for
  a ring. On the I/O-core path the request makes one backend call: it sends the
  link simulator's staged copies itself (rocketnet_client_link_drain at DBL_MAX,
  proofs/tla/RNClientLinkStop.tla Stop), synchronously, waiting on no worker."
- sdk/rocketnet_client_io.h (rocketnet_client_io_rebind_held), the first line
  of the comment block above the function, line six hundred forty-five (MM127).
  Now: "proofs/tla/RNClientRace.tla CallerCommit; main thread.
  proofs/tla/RNBufferHandoff.tla Rebind: a held ring bound to the losing server
  is bound to the kept one." New text: "proofs/tla/RNClientRace.tla
  CallerCommit; main thread. proofs/tla/RNBufferHandoff.tla Rebind covers the
  rebind while the protocol still names the losing server (a commit after a
  reset is RNClientRace's): a held ring bound to the losing server is bound to
  the kept one." The mention "(rocketnet_client_io_rebind_held, RNBufferHandoff
  Rebind)" three lines above it, in the hand-off comment, can stay.
- No new action names were added to any M8 model, so no new citations are needed:
  every new behaviour is a branch of an action the code already cites (WorkerQuiesce,
  ServerReceive, Read, Complete, Dequeue, Drain, Free, TryPrimary, CallerDestroy,
  ConsumerExchange, Acquire, Recheck, Publish, WWaitIdle).
