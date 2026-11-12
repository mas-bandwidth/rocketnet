--------------------------- MODULE MCRNGsoHandoff ---------------------------
(***************************************************************************)
(* The small instances of RNGsoHandoff: three receive buffers and three   *)
(* transmit banks of two packets.  MCRNGsoHandoff.cfg checks every         *)
(* invariant with every rule on, five datagrams and four staged packets;   *)
(* MCRNGsoHandoffLive.cfg checks them and the liveness properties with one *)
(* datagram and three staged packets;                                     *)
(* MCRNGsoHandoffBrokenPublishWhileHeld.cfg restores the publish-while-   *)
(* held defect and must fail NoPublishWhileHeld;                          *)
(* MCRNGsoHandoffBrokenFullDrops.cfg drops a full socket's rest and must  *)
(* fail SentOrHeld; MCRNGsoHandoffBrokenCarryBehind.cfg queues the rest   *)
(* behind newer banks and must fail TxInOrder;                            *)
(* MCRNGsoHandoffBrokenNoBackoff.cfg sets NoBackoff <- NoBackoffBroken    *)
(* (a stalled pass goes straight back to poll, the full-buffer spin) and  *)
(* must fail RxBacksOff.                                                  *)
(***************************************************************************)
EXTENDS RNGsoHandoff

NoBackoffBroken == TRUE

\* MCRNGsoHandoffBrokenDropBeforeShutdown.cfg: refusals count before
\* shutdown too; must fail FullSocketDropsOnlyAfterShutdown.
DropBeforeShutdownBroken == TRUE

\* MCRNGsoHandoffBrokenBoundOnQuitOnly.cfg: refusals count only after
\* send_quit while the stop waits for send_idle; must fail
\* ShutdownCompletes.
BoundOnQuitOnlyBroken == TRUE

\* MCRNGsoHandoffBrokenUncountedErrorDrop.cfg: a hard send error drops a
\* message uncounted; must fail SentOrHeld.
\* MCRNGsoHandoffReachErrorDropRunning.cfg (the rule) must fail
\* NotReachedErrorDropRunning: a counted error drop before shutdown.
CountErrorDropsBroken == FALSE
=============================================================================
