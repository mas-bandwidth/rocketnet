--------------------- MODULE MCTraceRNMsgxSendRunsSelf ---------------------
(***************************************************************************)
(* The trace check's own controls, before any code writes a trace: a       *)
(* hand-written record of a two-packet bank (packet 0 client 1 on slot 0,  *)
(* open at generation 0; packet 1 client 2 on the shared socket), the      *)
(* sort (the shared socket's packet first), the shared send of packet 1,   *)
(* the run of packet 0 built and sent, and the bank done, must be accepted *)
(* (MCTraceRNMsgxSendRunsSelf.cfg); the same record with the slot-blind    *)
(* sort (staged order, packet 0 first) must be rejected at the sort        *)
(* record (MCTraceRNMsgxSendRunsSelfBrokenSortBlind.cfg, TraceConsistent). *)
(* Every record is [op, act, args, state] and its state carries every      *)
(* variable and nothing else.  The cfgs give the bank as Bank: packet 0 is *)
(* BankCode 1101 (client 1, slot 0, generation 0, live) and packet 1       *)
(* 12001 (client 2, the shared socket, live).                              *)
(***************************************************************************)
EXTENDS TraceRNMsgxSendRuns, TLC

Rec(o, a, x, s) == [op |-> o, act |-> a, args |-> x, state |-> s]

F2(x, y) == [i \in 0..1 |-> IF i = 0 THEN x ELSE y]
NoS == [x \in {1, 2} \X ({<< -1, 0 >>} \cup ({0} \X {0, 1})) |-> -1]

S1 == [count |-> 2, client |-> F2(1, 2), slot |-> F2(0, -1), gen |-> F2(0, 0),
       live |-> F2(TRUE, TRUE), tOpen |-> [s \in 0..0 |-> TRUE], tGen |-> [s \in 0..0 |-> 0],
       order |-> F2(0, 0), pc |-> "sort", cursor |-> 0, hd |-> 0, scan |-> 0, run |-> << >>,
       attempts |-> 0, left |-> F2(0, 0), lastC |-> [c \in {1, 2} |-> -1], lastS |-> NoS,
       bad |-> [cross |-> FALSE, client |-> FALSE, stamp |-> FALSE, dead |-> FALSE],
       quit |-> FALSE, retries |-> 0, stalls |-> 0, retires |-> 0]

\* Sort: the shared socket's packet 1 first, then slot 0's packet 0
S2 == [S1 EXCEPT !.order = F2(1, 0), !.pc = "head"]

\* HeadShared: packet 1 leaves on the shared socket
S3 == [S2 EXCEPT !.cursor = 1, !.left = F2(0, 1), !.lastC = [c \in {1, 2} |-> IF c = 2 THEN 1 ELSE -1],
                 !.lastS = [NoS EXCEPT ![2, << -1, 0 >>] = 1]]

\* HeadRun: packet 0 resolves; the run is packet 0 alone
S4 == [S3 EXCEPT !.hd = 0, !.scan = 2, !.run = << 0 >>, !.pc = "send"]

\* SendAll: the connected send took the run
S5 == [S4 EXCEPT !.cursor = 2, !.pc = "head", !.left = F2(1, 1), !.lastC = [c \in {1, 2} |-> IF c = 2 THEN 1 ELSE 0],
                 !.lastS = [NoS EXCEPT ![2, << -1, 0 >>] = 1, ![1, << 0, 0 >>] = 0]]

\* HeadDone: the bank done
S6 == [S5 EXCEPT !.pc = "done"]

GoodLog == << Rec("init", "init", << >>, S1),
              Rec("sort", "Sort", << >>, S2),
              Rec("step", "HeadShared", << >>, S3),
              Rec("step", "HeadRun", << >>, S4),
              Rec("step", "SendAll", << >>, S5),
              Rec("step", "HeadDone", << >>, S6) >>

\* the slot-blind sort: staged order, packet 0 first
BadLog == << Rec("init", "init", << >>, S1),
             Rec("sort", "Sort", << >>, [S2 EXCEPT !.order = F2(0, 1)]) >>

=============================================================================
