--------------------------- MODULE MCRNBufferHandoff ---------------------------
(***************************************************************************)
(* The small instances of RNBufferHandoff: three buffers of two items,    *)
(* five items.  MCRNBufferHandoffSend.cfg (the send rings, MaxReady 2)    *)
(* and MCRNBufferHandoffReceive.cfg (the receive triple buffer, MaxReady  *)
(* 1) run with every rule on; each MCRNBufferHandoffBroken*.cfg restores  *)
(* one defect and must fail.  README-RNBufferHandoff.md gives the traces  *)
(* TLC printed and the check against the code by hand.                    *)
(***************************************************************************)
EXTENDS RNBufferHandoff
=============================================================================
