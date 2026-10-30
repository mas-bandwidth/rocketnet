--------------------------- MODULE MCRNIocpRunRing ---------------------------
(***************************************************************************)
(* The small instances of RNIocpRunRing.  MCRNIocpRunRing.cfg: three      *)
(* slots, runs of two or three, tickets up to 10 (so the overflow guard   *)
(* refuses), six allocations; MCRNIocpRunRingN4.cfg: four slots, runs of  *)
(* two to four.  Reversed witnesses: MCRNIocpRunRingBrokenCheckShort.cfg  *)
(* (the free check one slot short, NoOverwrite) and                       *)
(* MCRNIocpRunRingBrokenTicketWrap.cfg (tickets wrap, TakeOnlyLive).       *)
(***************************************************************************)
EXTENDS RNIocpRunRing
=============================================================================
