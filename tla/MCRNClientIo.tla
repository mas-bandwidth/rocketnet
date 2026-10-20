------------------------------ MODULE MCRNClientIo ------------------------------
(***************************************************************************)
(* The small instance of RNClientIo: two slots per send ring, receive     *)
(* buffers of one datagram (two in the SwapWhileReading witness), three   *)
(* datagrams.  MCRNClientIo.cfg is the committed run with every rule on;  *)
(* MCRNClientIoBrokenReuseBeforeSent.cfg,                                 *)
(* MCRNClientIoBrokenSwapWhileReading.cfg,                                *)
(* MCRNClientIoBrokenReceiveStall.cfg,                                    *)
(* MCRNClientIoBrokenHandOffWaits.cfg,                                    *)
(* MCRNClientIoBrokenFullWaitReadable.cfg and                             *)
(* MCRNClientIoBrokenDestinationOverwrite.cfg each restore one defect and *)
(* must fail; the last replaces DestinationOverwrite with                 *)
(* DestinationOverwriteOn.  MCRNClientIoStepDriven.cfg is the step-driven mode with    *)
(* every rule on, and MCRNClientIoBrokenStepStartsThread.cfg its witness.  *)
(* README-RNClientIo.md gives the traces TLC printed and the            *)
(* check against the design by hand.                                      *)
(***************************************************************************)
EXTENDS RNClientIo

\* the unconditional io->to[io->rings.fill] = *to restored, for
\* MCRNClientIoBrokenDestinationOverwrite.cfg (DestinationOverwrite <- DestinationOverwriteOn)
DestinationOverwriteOn == TRUE
=============================================================================
