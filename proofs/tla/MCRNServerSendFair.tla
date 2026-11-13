------------------------- MODULE MCRNServerSendFair -------------------------
(***************************************************************************)
(* The small instances of RNServerSendFair (revision 1.3).  The passing    *)
(* configs are the code: MCRNServerSendFair.cfg (three slots, one frame),  *)
(* Walk (five slots, two frames), Dark and DarkTwo (slots in probe hold),  *)
(* Short (a slot whose budget may refuse its packet), Rounds and           *)
(* RoundsFull (the rounds with an unconnected slot and with all            *)
(* connected).  The Broken configs are the reversed witnesses and must     *)
(* fail: FromZero and InPlace (FairService), DueIgnoresHold and DarkProbes *)
(* (the v1.1.0 due test), BudgetFrame (the v1.1.1 frame before the         *)
(* budget), Revisit, CutCursor and Reorder (the rounds' arithmetic).       *)
(* Live is the Walk ring with the capacity read live (room 0 to K, grown   *)
(* inside a flush); BrokenFixedWindow counts every flush toward the window *)
(* there and must fail FairService.                                        *)
(***************************************************************************)
EXTENDS RNServerSendFair

MCRoomZero  == 0
MCCountsAll == TRUE
=============================================================================
