---------------------------- MODULE MCRNPortableRx ----------------------------
(***************************************************************************)
(* The small instance of RNPortableRx.  MCRNPortableRx.cfg is the rule    *)
(* with three banks; MCRNPortableRxTwoBanks.cfg the same with two;        *)
(* MCRNPortableRxBsd.cfg the same on BSD, whose idle wait returns on the  *)
(* wake or its timeout alone.  Each reversed witness turns off one rule   *)
(* switch and must fail:                                                  *)
(*   MCRNPortableRxBrokenOverwrite.cfg    (HoldOnFull) ExactDelivery      *)
(*   MCRNPortableRxBrokenReturnEarly.cfg  (ReturnWhenRead) ExactDelivery  *)
(*   MCRNPortableRxBrokenWakeFirst.cfg    (QuitBeforeWake) StopCompletes  *)
(*   MCRNPortableRxBrokenNoWake.cfg       (WakeOnReturn)                  *)
(*                                         EveryDatagramDelivered          *)
(*   MCRNPortableRxBrokenUnboundedRead.cfg (BoundedRead) ReadBudget       *)
(*   MCRNPortableRxBsdBrokenNoTimeout.cfg (BSD, no Timeout)               *)
(*                                         EveryDatagramDelivered          *)
(* README-RNPortableRx.md gives each trace and its check against the code. *)
(***************************************************************************)
EXTENDS RNPortableRx
=============================================================================
