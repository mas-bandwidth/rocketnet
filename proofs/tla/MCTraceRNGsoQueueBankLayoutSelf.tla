----------------- MODULE MCTraceRNGsoQueueBankLayoutSelf -----------------
(***************************************************************************)
(* The trace check's own controls, before any code writes a trace: a       *)
(* hand-written record of one packet's way through a queue's banks (the    *)
(* slot claimed by the fetch-add, the packet finished, the producer        *)
(* turned over onto the free bank, the sent bank returned) must be         *)
(* accepted (MCTraceRNGsoQueueBankLayoutSelf.cfg), and the same record     *)
(* with the turn-over leaving the turn count where it was (the packet id's *)
(* high part, so the next fill would hand out ids already used) must be    *)
(* rejected at that record                                                 *)
(* (MCTraceRNGsoQueueBankLayoutSelfBrokenTurn.cfg, TraceConsistent).       *)
(* Every record is [op, act, args, state] (README-TraceCheck.md), its      *)
(* state every variable.  One queue of two banks of two slots, one thread, *)
(* one destination.                                                        *)
(***************************************************************************)
EXTENDS TraceRNGsoQueueBankLayout

Q0(x) == [q \in 0..0 |-> x]
B2(x, y) == [b \in 0..1 |-> IF b = 0 THEN x ELSE y]
T1(x) == [t \in {1} |-> x]
NoOwner == [k \in (0..0) \X (0..1) \X (0..2) |-> -1]

S1 == [prod |-> Q0(0), own |-> Q0(B2("prod", "free")), cnt |-> Q0(B2(0, 0)),
       turn |-> Q0(0), load |-> T1(None), held |-> T1(None), owner |-> NoOwner,
       demand |-> T1(0), flushes |-> 0, oob |-> FALSE, shared |-> FALSE,
       foreign |-> FALSE, routeBad |-> FALSE, workerWrite |-> FALSE]

\* Start(1, 1, r): the fetch-add on queue 0's producer bank 0 takes slot 0;
\* id (0 * 2 + 0) * 1 + 0
S2 == [S1 EXCEPT !.cnt = Q0(B2(1, 0)), !.demand = T1(1),
                 !.held = T1([q |-> 0, b |-> 0, slot |-> 0, ptr |-> 0, pid |-> 0]),
                 !.owner = [NoOwner EXCEPT ![<<0, 0, 0>>] = 1]]

\* Finish(1, 0)
S3 == [S2 EXCEPT !.held = T1(None)]

\* Flush: bank 0 to the worker, bank 1 the producer, turn 1
S4 == [S3 EXCEPT !.prod = Q0(1), !.own = Q0(B2("worker", "prod")), !.turn = Q0(1),
                 !.owner = NoOwner, !.demand = T1(0), !.flushes = 1]

\* WorkerReturn(0, 0): bank 0 free, its counter reset
S5 == [S4 EXCEPT !.own = Q0(B2("free", "prod")), !.cnt = Q0(B2(0, 0))]

Rec(o, a, x, s) == [op |-> o, act |-> a, args |-> x, state |-> s]

GoodLog == << Rec("init", "init", <<>>, S1),
              Rec("start_packet", "Start", <<1, 1, [ptr |-> 0, pid |-> 0]>>, S2),
              Rec("finish_packet", "Finish", <<1, 0>>, S3),
              Rec("send_packets", "Flush", <<>>, S4),
              Rec("take_return_send", "WorkerReturn", <<0, 0>>, S5) >>

\* the turn-over from S3 without counting the turn: record 4 is no model step
BadTurn == [S4 EXCEPT !.turn = Q0(0)]

BadLog == << Rec("init", "init", <<>>, S1),
             Rec("start_packet", "Start", <<1, 1, [ptr |-> 0, pid |-> 0]>>, S2),
             Rec("finish_packet", "Finish", <<1, 0>>, S3),
             Rec("send_packets", "Flush", <<>>, BadTurn) >>

=============================================================================
