--------------------------- MODULE MCRNClientDelivery ---------------------------
(***************************************************************************)
(* The instances of RNClientDelivery.  MCRNClientDelivery.cfg is the fixed *)
(* reset with a caller that drains every batch; MCRNClientDeliveryLazy.cfg *)
(* the same with a caller that may leave entries unread. The reversed      *)
(* witnesses MCRNClientDeliveryBrokenEndWipes.cfg (invariant),             *)
(* MCRNClientDeliveryBrokenEndWipesLive.cfg (liveness) and                 *)
(* MCRNClientDeliveryBrokenNoBeginClear.cfg each restore one defect and    *)
(* must fail.  README-RNClientDelivery.md gives the traces and the check   *)
(* against the code by hand.                                               *)
(***************************************************************************)
EXTENDS RNClientDelivery
=============================================================================
