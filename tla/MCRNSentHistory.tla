------------------------------ MODULE MCRNSentHistory ------------------------------
(***************************************************************************)
(* The small instance of RNSentHistory: a ring of 4 records, threshold 2, *)
(* seven sequences (the ring laps once, so the records of 1, 2 and 3 are  *)
(* evicted), at most two packets lost, a healthy bound of 2 (N - Flight =  *)
(* Threshold), ack blocks covering the last 4 sequences, two blocks in     *)
(* flight at once. MCRNSentHistory.cfg is the committed run with the       *)
(* split rule on. MCRNSentHistoryBrokenCountEveryUnacked.cfg and           *)
(* MCRNSentHistoryBrokenRefuseWhenFull.cfg are the reversed witnesses,     *)
(* each the committed file with one defect constant flipped;               *)
(* README-RNSentHistory.md has the traces TLC printed, replayed by hand.   *)
(***************************************************************************)
EXTENDS RNSentHistory
=============================================================================
