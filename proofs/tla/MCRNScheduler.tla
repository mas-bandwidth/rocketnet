--------------------------- MODULE MCRNScheduler ---------------------------
(***************************************************************************)
(* The small instances that check RNScheduler.  README-RNScheduler.md      *)
(* lists every case.                                                       *)
(*                                                                         *)
(* MCMsgs is the code's instance (MCRNScheduler.cfg and the witnesses on   *)
(* it): CONFIG, IMMEDIATE, at the head of channel 0, then one MEDIUM       *)
(* message on channel 0 and one on channel 1.  Every v1.1.x reliable       *)
(* message is MEDIUM; only CONFIG is IMMEDIATE.                            *)
(*                                                                         *)
(* MCMsgsReserved is the builder's contract at the reserved levels, which  *)
(* no v1.1.x caller sets (MCRNSchedulerReservedLevels.cfg and              *)
(* MCRNSchedulerBrokenPerMessagePriority.cfg): channel 0 queues a LOW      *)
(* item and then an IMMEDIATE item (the boost case), channel 1 one HIGH    *)
(* item.                                                                   *)
(*                                                                         *)
(* MCMsgsKeepsSending is a channel that keeps sending                      *)
(* (MCRNSchedulerKeepsSending.cfg and MCRNSchedulerBrokenReadyKeepsTurn.  *)
(* cfg): CONFIG, two MEDIUM messages on channel 0 queued again whenever    *)
(* acked (MCRefill), and one MEDIUM message on channel 1.                  *)
(***************************************************************************)
EXTENDS RNScheduler

MCMsgs == << [ch |-> 0, lv |-> 0], [ch |-> 0, lv |-> 2], [ch |-> 1, lv |-> 2] >>

MCMsgsReserved == << [ch |-> 0, lv |-> 3], [ch |-> 0, lv |-> 0], [ch |-> 1, lv |-> 1] >>

MCMsgsKeepsSending == << [ch |-> 0, lv |-> 0], [ch |-> 0, lv |-> 2], [ch |-> 0, lv |-> 2],
                         [ch |-> 1, lv |-> 2] >>

MCRefill == {2, 3}

MCNoRefill == {}

=============================================================================
