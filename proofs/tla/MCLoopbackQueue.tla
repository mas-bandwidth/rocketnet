------------------------- MODULE MCLoopbackQueue -------------------------
(***************************************************************************)
(* The small instance of LoopbackQueue: Capacity 4 and 6 send attempts,   *)
(* enough to fill the queue and take the full drop (five sends with no     *)
(* receive) and to wrap tail past Capacity (four sends, one receive, one   *)
(* send).  MCLoopbackQueue.cfg is the passing control with Mutation =     *)
(* "none", the code.  The reversed witnesses each set one Mutation and    *)
(* must violate the one invariant they name:                              *)
(*   MCLoopbackQueueBrokenNoRestart.cfg       no_restart, TouchedWithinRun *)
(*   MCLoopbackQueueBrokenNoRestartEmpty.cfg  no_restart, EmptyIsRestarted *)
(*   MCLoopbackQueueBrokenNoFullCheck.cfg     no_full_check,               *)
(*                                            NoSlotReusedInFlight         *)
(*   MCLoopbackQueueBrokenEarlyRestart.cfg    early_restart, FifoOrder     *)
(*   MCLoopbackQueueBrokenUncountedDrop.cfg   uncounted_drop, DropsCounted *)
(***************************************************************************)
EXTENDS LoopbackQueue

MCSpec == Spec

=============================================================================
