--------------------------- MODULE MCRNGsoHandoff ---------------------------
(***************************************************************************)
(* The small instance of RNGsoHandoff: three receive buffers of two       *)
(* packets, three transmit banks of two packets and four staged packets.  *)
(* MCRNGsoHandoff.cfg runs with every rule on;                            *)
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
\* shutdown too; must fail DroppedOnlyAfterShutdown.
DropBeforeShutdownBroken == TRUE

\* MCRNGsoHandoffBrokenBoundOnQuitOnly.cfg: refusals count only after
\* send_quit while the stop waits for send_idle; must fail
\* ShutdownCompletes.
BoundOnQuitOnlyBroken == TRUE
=============================================================================
