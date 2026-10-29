----------------------------- MODULE MCRNSentMap -----------------------------
(***************************************************************************)
(* The small instance of RNSentMap: a window of W = 3 numbers, round trip  *)
(* RTT = 2 ticks, numbers 0..4, packets of one or two numbers, any packet  *)
(* lost or opened, the clock to 6.  MCRNSentMap.cfg is the committed run   *)
(* with P = W and RTO = RTT, the tightest sizes the rule allows.           *)
(* MCRNSentMapBrokenSmallMap.cfg (P = W - 1) and                           *)
(* MCRNSentMapBrokenEarlyResend.cfg (RTO < RTT) are the reversed           *)
(* witnesses, each the committed file with one constant changed.           *)
(***************************************************************************)
EXTENDS RNSentMap
=============================================================================
