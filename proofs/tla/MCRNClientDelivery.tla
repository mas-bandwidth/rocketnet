--------------------------- MODULE MCRNClientDelivery ---------------------------
(***************************************************************************)
(* The instances of RNClientDelivery.  MCRNClientDelivery.cfg is the fixed *)
(* reset with a caller that drains every batch; MCRNClientDeliveryLazy.cfg *)
(* the same with a caller that may leave entries unread. Both include the  *)
(* cursor refusing a payload too large for the caller's buffer. The        *)
(* reversed witnesses MCRNClientDeliveryBrokenEndWipes.cfg (invariant),    *)
(* MCRNClientDeliveryBrokenEndWipesLive.cfg (liveness),                    *)
(* MCRNClientDeliveryBrokenNoBeginClear.cfg,                               *)
(* MCRNClientDeliveryBrokenCountBeforePlace.cfg,                           *)
(* MCRNClientDeliveryBrokenBudgetOverBatch.cfg and                         *)
(* MCRNClientDeliveryBrokenRefuseVanishes.cfg each restore one defect and  *)
(* must fail.  README-RNClientDelivery.md gives the traces and the check   *)
(* against the code by hand.                                               *)
(***************************************************************************)
EXTENDS RNClientDelivery
=============================================================================
