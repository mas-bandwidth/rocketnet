------------------------ MODULE MCLoopbackHubLayout ------------------------
(***************************************************************************)
(* The small instance of LoopbackHubLayout: the server's queue and one     *)
(* client's queue in one block, both of capacity 3, index counters that   *)
(* wrap at 6, and eight datagrams, enough for a queue that never empties  *)
(* to cross the wrap.  MCLoopbackHubLayout.cfg is the passing control.     *)
(* Reversed witnesses: MCLoopbackHubLayoutBrokenFullOffByOne.cfg accepts a *)
(* datagram into a full queue (FullSlack = 1) and                         *)
(* MCLoopbackHubLayoutBrokenWrapNotMultiple.cfg wraps the counters at 8,   *)
(* not a multiple of the capacity; both must violate RingIsQueue.         *)
(***************************************************************************)
EXTENDS LoopbackHubLayout

MCSpec == Spec

=============================================================================
