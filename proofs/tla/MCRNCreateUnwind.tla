--------------------------- MODULE MCRNCreateUnwind ---------------------------
(***************************************************************************)
(* The instances of RNCreateUnwind.  MCRNCreateUnwind.cfg is the code as  *)
(* it is: the deadline only where a retained slot takes the transport.     *)
(* MCRNCreateUnwindBrokenBoundedUnwind.cfg and                             *)
(* MCRNCreateUnwindBrokenNoRetain.cfg each restore one defect and must    *)
(* fail LiveTransportOwned; MCRNCreateUnwindBrokenFreeAtDeadline.cfg     *)
(* must fail FreedOnlyAfterExit.  README-RNCreateUnwind.md gives the     *)
(* traces and the check against the code by hand.                         *)
(***************************************************************************)
EXTENDS RNCreateUnwind
=============================================================================
