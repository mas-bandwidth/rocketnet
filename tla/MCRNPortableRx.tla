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
(*                                                                         *)
(* The append rule's own reversed witness is BrokenAppend below: the      *)
(* worker publishes its producer in place and keeps appending into it, so *)
(* one bank is both the producer and pending in the ready FIFO.           *)
(* BrokenAppendSpec must fail ExclusiveOwnership.  Its configuration is    *)
(* MCRNPortableRxBrokenAppend.cfg (three banks and the same constants as  *)
(* MCRNPortableRx.cfg, with SPECIFICATION BrokenAppendSpec).               *)
(***************************************************************************)
EXTENDS RNPortableRx

\* The reversed append rule: publish the producer without swapping it out,
\* then keep appending into it (the worker "owns" a bank main can also see).
WAppendPublished ==
    /\ wpc = "read"
    /\ data[prod] # <<>>
    /\ ready' = Append(ready, prod)
    /\ wpc' = "read"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, delivered,
                   lock, wake, quit, stopping, mpc, budget, freed>>

BrokenNext == Next \/ WAppendPublished

BrokenAppendSpec == Init /\ [][BrokenNext]_vars

=============================================================================
