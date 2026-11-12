--------------- MODULE MCTraceRNUringQueueReceiveLayoutSelf ---------------
(***************************************************************************)
(* The trace check's own controls, before any code writes a trace: a       *)
(* hand-written record of one datagram's way through a queue (it arrives,  *)
(* the multishot is armed, the kernel takes buffer 0 for it, the worker    *)
(* banks it in slot 0 and gives the buffer back at entry 0 with the tail   *)
(* past the first wrap of create's adds, main drains it and is done) must  *)
(* be accepted (MCTraceRNUringQueueReceiveLayoutSelf.cfg), and the same    *)
(* record with the drain releasing what it takes (head 1 at record 6, the  *)
(* plant's step) must be rejected at that record                           *)
(* (MCTraceRNUringQueueReceiveLayoutSelfBrokenRelease.cfg,                 *)
(* TraceConsistent).  The harness's traces are checked the same way, with  *)
(* TraceLog from the module it writes.                                     *)
(***************************************************************************)
EXTENDS TraceRNUringQueueReceiveLayout

D1 == [d |-> 1, ok |-> TRUE]

S1 == [arrived |-> 0, nextId |-> 1, sock |-> <<>>, karmed |-> FALSE, cq |-> <<>>,
       armed |-> FALSE, stopping |-> FALSE, brHead |-> 0, brTail |-> 4,
       entries |-> (0 :> 0 @@ 1 :> 1 @@ 2 :> 2 @@ 3 :> 3),
       owner |-> (0 :> "ring" @@ 1 :> "ring" @@ 2 :> "ring" @@ 3 :> "ring"),
       slotD |-> (0 :> 0 @@ 1 :> 0 @@ 2 :> 0), tail |-> 0, head |-> 0, headSeen |-> 0,
       exposed |-> 0, reading |-> FALSE, vidx |-> <<>>, view |-> <<>>, banked |-> <<>>,
       delivered |-> <<>>, named |-> 0, full |-> 0, spurious |-> FALSE, dup |-> FALSE,
       torn |-> FALSE]

\* Arrive: one accepted datagram
S2 == [S1 EXCEPT !.arrived = 1, !.nextId = 2, !.sock = << <<D1>> >>]

\* Arm
S3 == [S2 EXCEPT !.armed = TRUE, !.karmed = TRUE]

\* KernelReceive: entry 0 holds buffer 0
S4 == [S3 EXCEPT !.sock = <<>>, !.brHead = 1,
                 !.owner = (0 :> "cq" @@ 1 :> "ring" @@ 2 :> "ring" @@ 3 :> "ring"),
                 !.cq = << [kind |-> "data", bid |-> 0, segs |-> <<D1>>] >>]

\* Complete: banked at slot 0, buffer 0 back at entry 4 & 3 = 0
S5 == [S4 EXCEPT !.cq = <<>>, !.slotD = (0 :> 1 @@ 1 :> 0 @@ 2 :> 0), !.tail = 1,
                 !.banked = <<1>>, !.brTail = 5,
                 !.owner = (0 :> "ring" @@ 1 :> "ring" @@ 2 :> "ring" @@ 3 :> "ring")]

\* Drain: releases nothing (exposed was 0), takes position 0
S6 == [S5 EXCEPT !.exposed = 1, !.vidx = <<0>>, !.view = <<1>>, !.reading = TRUE]

\* Done
S7 == [S6 EXCEPT !.delivered = <<1>>, !.reading = FALSE]

GoodLog == << S1, S2, S3, S4, S5, S6, S7 >>

\* the drain releases what it takes: head 1 at record 6 is no model step
S6Bad == [S6 EXCEPT !.head = 1]

BadLog == << S1, S2, S3, S4, S5, S6Bad >>

\* an under-logged harness: record 3 leaves out torn, so it is no state of
\* the model, however right its other fields
S3Short == [f \in DOMAIN S3 \ {"torn"} |-> S3[f]]

ShortLog == << S1, S2, S3Short, S4, S5, S6, S7 >>

=============================================================================
