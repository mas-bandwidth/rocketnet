------------------------------ MODULE MCRNClientIo ------------------------------
(***************************************************************************)
(* The small instance of RNClientIo: two slots per send ring, three       *)
(* hand-offs, two datagrams.  MCRNClientIo.cfg is the committed run with  *)
(* both rules on; MCRNClientIoBrokenReuseBeforeSent.cfg and              *)
(* MCRNClientIoBrokenSwapWhileReading.cfg each restore one defect and     *)
(* must fail.  README-RNClientIo.md gives the traces TLC printed and the  *)
(* check against the design by hand.                                      *)
(***************************************************************************)
EXTENDS RNClientIo
=============================================================================
