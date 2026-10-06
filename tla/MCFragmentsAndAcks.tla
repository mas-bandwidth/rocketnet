------------------------ MODULE MCFragmentsAndAcks ------------------------
(***************************************************************************)
(* The small instance of FragmentsAndAcks.  The constants are in the .cfg; *)
(* this module supplies the operator-valued ones (Forged) and the          *)
(* state constraint that keeps the committed run finite and quick.         *)
(*                                                                         *)
(* MCForgedNone: the honest sender.  MCForgedHeaders: the defective sender *)
(* of the forge run (README-FragmentsAndAcks.md): a zero payload id, a     *)
(* count of one, an index past the count, a count that differs from the    *)
(* first fragment's, a non-final whose size differs from the stride, a     *)
(* final that exceeds it, a geometry too large for the arena, and a stale  *)
(* id behind the wrap.  With FirstPid = 7 and IdMod = 8 the honest ids are *)
(* 7, 1, 2; id 5 is older than 7 in serial arithmetic.                     *)
(***************************************************************************)
EXTENDS FragmentsAndAcks

MCForgedNone == {}

MCForgedHeaders ==
    { [pid |-> 0, idx |-> 0, cnt |-> 2, bytes |-> 2],
      [pid |-> 7, idx |-> 0, cnt |-> 1, bytes |-> 2],
      [pid |-> 7, idx |-> 2, cnt |-> 2, bytes |-> 2],
      [pid |-> 7, idx |-> 0, cnt |-> 3, bytes |-> 2],
      [pid |-> 7, idx |-> 0, cnt |-> 2, bytes |-> 1],
      [pid |-> 7, idx |-> 1, cnt |-> 2, bytes |-> 2],
      [pid |-> 7, idx |-> 2, cnt |-> 3, bytes |-> 2],
      [pid |-> 5, idx |-> 0, cnt |-> 2, bytes |-> 2] }

\* the ack conversation is unbounded (B may send blocks forever); the
\* constraint bounds the link counters only, which Link saturates itself
MCConstraint == TRUE

=============================================================================
