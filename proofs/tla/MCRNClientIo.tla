------------------------------ MODULE MCRNClientIo ------------------------------
(***************************************************************************)
(* The small instances of RNClientIo.  The threaded full instance is two  *)
(* slots per send ring, one-datagram receive buffers and three datagrams; *)
(* the defects' witnesses use the same instance (two datagrams in the     *)
(* SwapWhileReading witness, receive buffers of one and two).  Its        *)
(* committed checks are split by obligation, never by state space:        *)
(* MCRNClientIoSafety.cfg checks the invariants, and each                 *)
(* MCRNClientIoLive*.cfg checks one liveness property (or one conjunct of *)
(* one, named in MCRNClientIoLiveness.tla) under SpecFair.                *)
(* MCRNClientIoBrokenReuseBeforeSent.cfg,                                 *)
(* MCRNClientIoBrokenSwapWhileReading.cfg,                                *)
(* MCRNClientIoBrokenReceiveStall.cfg,                                    *)
(* MCRNClientIoBrokenHandOffWaits.cfg and                                 *)
(* MCRNClientIoBrokenFullWaitReadable.cfg each restore one defect and     *)
(* must fail.  MCRNClientIoBrokenDestroyWaitsForever.cfg removes the      *)
(* guaranteed deadline and must fail DestroyCompletes.                    *)
(* MCRNClientIoBrokenDestinationOverwrite.cfg restores destination        *)
(* overwrite and must fail SentToCommitted.                                *)
(* MCRNClientIoStepDriven.cfg is the step-driven mode with every rule on  *)
(* at one slot and two datagrams, MCRNClientIoStepFull*.cfg the same mode *)
(* at the full instance split by obligation under SpecStepFair, and       *)
(* MCRNClientIoBrokenStepStartsThread.cfg its witness.                    *)
(* README-RNClientIo.md gives the traces TLC printed and the            *)
(* check against the design by hand.                                      *)
(***************************************************************************)
EXTENDS RNClientIo

\* the unconditional io->to[io->rings.fill] = *to restored, for
\* MCRNClientIoBrokenDestinationOverwrite.cfg (DestinationOverwrite <- DestinationOverwriteOn)
DestinationOverwriteOn == TRUE
=============================================================================
