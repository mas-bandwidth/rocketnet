------------------------ MODULE MCLoopbackHubLayout ------------------------
(***************************************************************************)
(* The small instance of LoopbackHubLayout: the server's queue and one     *)
(* client's queue in one block, both of capacity 3, index counters that   *)
(* wrap at 6, and eight datagrams, enough for a queue that never empties  *)
(* to cross the wrap; slots of 4 bytes with a 4-byte red zone and 4-byte  *)
(* alignment, so a queue's bytes end on an alignment boundary and only     *)
(* the red zone in the carve keeps it off the next queue.                 *)
(* MCLoopbackHubLayout.cfg is the passing control.  Reversed witnesses:    *)
(* MCLoopbackHubLayoutBrokenFullOffByOne.cfg accepts a datagram into a    *)
(* full queue (FullSlack = 1), MCLoopbackHubLayoutBrokenWrapNotMultiple.cfg *)
(* wraps the counters at 8, not a multiple of the capacity, and           *)
(* MCLoopbackHubLayoutBrokenResetKeepsIndices.cfg resets a record's count *)
(* but not its head and tail; all three must violate RingIsQueue.         *)
(* MCLoopbackHubLayoutBrokenCarveNoRedZone.cfg carves without the red     *)
(* zone and MCLoopbackHubLayoutBrokenHubBytesNoRedZone.cfg sizes the      *)
(* block without the red zones; both must violate StaysInRegion.          *)
(***************************************************************************)
EXTENDS LoopbackHubLayout

MCSpec == Spec

=============================================================================
