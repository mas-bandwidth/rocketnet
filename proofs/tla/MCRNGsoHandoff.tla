--------------------------- MODULE MCRNGsoHandoff ---------------------------
(***************************************************************************)
(* The small instance of RNGsoHandoff: two receive buffers and three      *)
(* transmit banks of two packets, four datagrams and four staged packets. *)
(* MCRNGsoHandoff.cfg runs with every rule on;                            *)
(* MCRNGsoHandoffBrokenPublishWhileHeld.cfg restores the publish-while-   *)
(* held defect and must fail NoPublishWhileHeld;                          *)
(* MCRNGsoHandoffBrokenFullDrops.cfg drops a full socket's rest and must  *)
(* fail SentOrHeld; MCRNGsoHandoffBrokenCarryBehind.cfg queues the rest   *)
(* behind newer banks and must fail TxInOrder;                            *)
(* MCRNGsoHandoffBrokenClaimStale.cfg sets ClaimStale <- ClaimStaleBroken *)
(* (the claim leaves the last staged packet out of its view, as the       *)
(* worker-driven publish did) and must fail StagedBeforeClaimInView;      *)
(* MCRNGsoHandoffBrokenNoBackoff.cfg sets NoBackoff <- NoBackoffBroken    *)
(* (a stalled pass goes straight back to poll, the full-bank spin) and    *)
(* must fail RxBacksOff.                                                  *)
(***************************************************************************)
EXTENDS RNGsoHandoff

ClaimStaleBroken == TRUE

NoBackoffBroken == TRUE

\* MCRNGsoHandoffBrokenLeaveBeforeQuit.cfg: the give-up bound before stop
\* too; must fail LeftOnlyAfterQuit.
LeaveBeforeQuitBroken == TRUE
=============================================================================
