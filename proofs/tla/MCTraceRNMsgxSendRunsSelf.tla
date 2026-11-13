--------------------- MODULE MCTraceRNMsgxSendRunsSelf ---------------------
(***************************************************************************)
(* The trace check's own controls, before any code writes a trace: a       *)
(* hand-written record of one client restamped between two fills of one  *)
(* deferred bank (packet 0 staged in fill phase 0 on slot 0, packet 1 in   *)
(* phase 1 on the shared socket after main's commit retired slot 0), the   *)
(* sort by phase then slot, the slot retired, and both packets leaving     *)
(* through the shared socket in staged order, must be accepted             *)
(* (MCTraceRNMsgxSendRunsSelf.cfg); the same record with the slot-only     *)
(* sort (packet 1, key 0, before packet 0) must be rejected at the sort    *)
(* record (MCTraceRNMsgxSendRunsSelfBrokenSlotOnly.cfg, TraceConsistent).  *)
(* Every record carries every variable and nothing else.                   *)
(***************************************************************************)
EXTENDS TraceRNMsgxSendRuns, TLC

MCOpenGen(s) == 0

F2(x, y) == [i \in 0..1 |-> IF i = 0 THEN x ELSE y]
NoS == [x \in {1} \X ({<< -1, 0 >>} \cup ({0} \X {0, 1})) |-> -1]

S1 == [count |-> 2, client |-> F2(1, 1), slot |-> F2(0, -1), gen |-> F2(0, 0), phase |-> F2(0, 1),
       live |-> F2(TRUE, TRUE), tOpen |-> [s \in 0..0 |-> TRUE], tGen |-> [s \in 0..0 |-> 0],
       order |-> F2(0, 0), pc |-> "sort", cursor |-> 0, hd |-> 0, scan |-> 0, run |-> << >>,
       attempts |-> 0, left |-> F2(0, 0), lastC |-> [c \in {1} |-> -1], lastS |-> NoS,
       bad |-> [cross |-> FALSE, client |-> FALSE, stamp |-> FALSE, dead |-> FALSE],
       quit |-> FALSE, retries |-> 0, stalls |-> 0, retires |-> 0]

\* Sort: phase 0 (packet 0), then phase 1 (packet 1)
S2 == [S1 EXCEPT !.order = F2(0, 1), !.pc = "head"]

\* Retire(0)
S3 == [S2 EXCEPT !.tOpen = [s \in 0..0 |-> FALSE], !.retires = 1]

\* LoopTop: packet 0 resolves to no socket and leaves on the shared socket
S4 == [S3 EXCEPT !.cursor = 1, !.left = F2(1, 0), !.lastC = [c \in {1} |-> 0],
                 !.lastS = [NoS EXCEPT ![1, << 0, 0 >>] = 0]]

\* LoopTop: packet 1, the shared socket's
S5 == [S4 EXCEPT !.cursor = 2, !.left = F2(1, 1), !.lastC = [c \in {1} |-> 1],
                 !.lastS = [NoS EXCEPT ![1, << 0, 0 >>] = 0, ![1, << -1, 0 >>] = 1]]

\* LoopTop: the bank done
S6 == [S5 EXCEPT !.pc = "done"]

GoodLog == << S1, S2, S3, S4, S5, S6 >>

\* the slot-only sort: packet 1 (the shared socket's key 0) first
BadLog == << S1, [S2 EXCEPT !.order = F2(1, 0)] >>

=============================================================================
