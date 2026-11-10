------------------------ MODULE MCRNBuilderPlanLayout ------------------------
(***************************************************************************)
(* The small instances of RNBuilderPlanLayout: four writable items, two    *)
(* channels (1 and 2, whose low and high digits disagree at Radix 2), two  *)
(* classes, two priority levels and packets of up to two records, every    *)
(* assignment of class, channel and priority to the four items.  The       *)
(* control and three reversed witnesses: the refused packet's cursor not   *)
(* restored (ChannelOrderKept), an inclusive prefix sum (NoOverlap) and a  *)
(* scatter from the end (PlanSorted).  README-RNBuilderPlanLayout.md gives *)
(* each trace and its check against the code.                              *)
(***************************************************************************)
EXTENDS RNBuilderPlanLayout
=============================================================================
