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
- clientrace: MCRNClientRaceBrokenNoLiveCheck.cfg (FreedOnce)
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
