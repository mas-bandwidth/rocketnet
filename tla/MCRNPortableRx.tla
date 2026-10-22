---------------------------- MODULE MCRNPortableRx ----------------------------
(***************************************************************************)
(* The small instance of RNPortableRx.  MCRNPortableRx.cfg is the rule    *)
(* with three banks; MCRNPortableRxTwoBanks.cfg the same with two.  Each  *)
(* reversed witness turns off one rule switch and must fail:              *)
(*   MCRNPortableRxBrokenOverwrite.cfg    (HoldOnFull) ExactDelivery      *)
(*   MCRNPortableRxBrokenReturnEarly.cfg  (ReturnWhenRead) ExactDelivery  *)
(*   MCRNPortableRxBrokenWakeFirst.cfg    (QuitBeforeWake) StopCompletes  *)
(*   MCRNPortableRxBrokenNoWake.cfg       (WakeOnReturn)                  *)
(*                                         EveryDatagramDelivered          *)
(* README-RNPortableRx.md gives each trace and its check against the code. *)
(***************************************************************************)
EXTENDS RNPortableRx
=============================================================================
