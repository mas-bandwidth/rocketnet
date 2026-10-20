------------------------------ MODULE MCRNClientIo ------------------------------
(***************************************************************************)
(* The small instances of RNClientIo.  MCRNClientIo.cfg runs two slots   *)
(* per send ring and three datagrams; the defects' witnesses use the same *)
(* instance (two slots per ring and three datagrams, two in the           *)
(* SwapWhileReading witness, receive buffers of one and two).             *)
(* MCRNClientIo.cfg is the committed run with every rule on;              *)
(* MCRNClientIoBrokenReuseBeforeSent.cfg,                                 *)
(* MCRNClientIoBrokenSwapWhileReading.cfg,                                *)
(* MCRNClientIoBrokenReceiveStall.cfg,                                    *)
(* MCRNClientIoBrokenHandOffWaits.cfg and                                 *)
(* MCRNClientIoBrokenFullWaitReadable.cfg each restore one defect and     *)
(* must fail.  MCRNClientIoBrokenDestroyWaitsForever.cfg removes the      *)
(* guaranteed deadline and must fail DestroyCompletes.                    *)
(* MCRNClientIoBrokenDestinationOverwrite.cfg restores destination        *)
(* overwrite and must fail SentToCommitted.                                *)
(* MCRNClientIoStepDriven.cfg is the step-driven mode with every rule on, *)
(* and MCRNClientIoBrokenStepStartsThread.cfg its witness.                *)
(* README-RNClientIo.md gives the traces TLC printed and the            *)
(* check against the design by hand.                                      *)
(***************************************************************************)
EXTENDS RNClientIo

\* the unconditional io->to[io->rings.fill] = *to restored, for
\* MCRNClientIoBrokenDestinationOverwrite.cfg (DestinationOverwrite <- DestinationOverwriteOn)
DestinationOverwriteOn == TRUE
=============================================================================
