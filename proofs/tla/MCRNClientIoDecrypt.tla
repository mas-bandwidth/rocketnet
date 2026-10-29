--------------------------- MODULE MCRNClientIoDecrypt ---------------------------
(***************************************************************************)
(* The small instance of RNClientIoDecrypt: receive buffers of two         *)
(* records, four datagrams in a behaviour chosen from the challenge,       *)
(* genuine session sequences 1..3 sent in order, one replayed copy of a    *)
(* sent sequence (a duplicate, or too old once the window has passed it)   *)
(* and one forged session datagram; a replay window of width 1, so the     *)
(* copy of sequence 1 after sequence 2 has opened is too old within four   *)
(* datagrams; an integrity limit of one.  MCRNClientIoDecrypt.cfg is the   *)
(* committed run with every rule on.                                       *)
(* MCRNClientIoDecryptBrokenOpenBeforeReplay.cfg,                          *)
(* MCRNClientIoDecryptBrokenSharedWindow.cfg,                              *)
(* MCRNClientIoDecryptBrokenNoRetire.cfg and                               *)
(* MCRNClientIoDecryptBrokenCountOnIo.cfg each restore one defect and must *)
(* fail.  README-RNClientIoDecrypt.md gives the traces TLC printed and the *)
(* check of each against the design by hand.                               *)
(***************************************************************************)
EXTENDS RNClientIoDecrypt
=============================================================================
