---------------------------- MODULE MCLoopbackWake ----------------------------
(***************************************************************************)
(* The small instance of LoopbackWake: a queue of three.                  *)
(* MCLoopbackWake.cfg is the passing control with both switches on.       *)
(* MCLoopbackWakeBrokenNoSendWake.cfg turns SendWakes off (the polling    *)
(* client) and must violate NoSleepOverQueued;                            *)
(* MCLoopbackWakeBrokenEdgeWake.cfg turns LevelWake off and must violate  *)
(* NoSleepOverQueued.  These are the reversed witnesses.                  *)
(***************************************************************************)
EXTENDS LoopbackWake

MCSpec == Spec

=============================================================================
