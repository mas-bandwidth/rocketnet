------------------- MODULE MCTraceRNMsgxReceiveFillSelf -------------------
(***************************************************************************)
(* The trace check's own controls, before any code writes a trace: a       *)
(* hand-written record of one socket and banks of three, four datagrams    *)
(* arriving, a read of two, a read of one into the last slot, the full     *)
(* bank committed and left (main shares it), the last datagram read short  *)
(* into the next bank, the round's commit, main's call cut at three (the   *)
(* tail's three returned, the shared bank's one left), main's stop, the    *)
(* worker's exit and main's count of the one left, must be accepted        *)
(* (MCTraceRNMsgxReceiveFillSelf.cfg); the same record with the planted    *)
(* second read (a whole batch of two into a room of one, the second        *)
(* datagram lost) must be rejected at that read                            *)
(* (MCTraceRNMsgxReceiveFillSelfBrokenReadPastRoom.cfg, TraceConsistent).  *)
(* Every record is [op, act, args, state] and its state carries every      *)
(* variable and nothing else.                                              *)
(***************************************************************************)
EXTENDS TraceRNMsgxReceiveFill, TLC

Rec(o, a, x, s) == [op |-> o, act |-> a, args |-> x, state |-> s]

B(x, y, z) == (0 :> x @@ 1 :> y @@ 2 :> z)

S0 == [kq |-> (1 :> 0), errs |-> (1 :> << >>), cnt |-> B(0, 0, 0), committed |-> B(0, 0, 0),
       worker |-> 0, main |-> 0, tail |-> 2, free |-> 1, mainCursor |-> 0, mainLimit |-> 0,
       tailCursor |-> 0, tailEnd |-> 0, pc |-> "poll", cur |-> 0, reads |-> 0, roundRead |-> FALSE,
       arrived |-> 0, kdrops |-> 0, consumed |-> 0, fullDrops |-> 0, errors |-> 0,
       stopping |-> FALSE, exited |-> FALSE, counted |-> FALSE, stopped |-> 0,
       committedTop |-> 0, late |-> FALSE]

A1 == [S0 EXCEPT !.kq = (1 :> 1), !.arrived = 1]
A2 == [S0 EXCEPT !.kq = (1 :> 2), !.arrived = 2]
A3 == [S0 EXCEPT !.kq = (1 :> 3), !.arrived = 3]
A4 == [S0 EXCEPT !.kq = (1 :> 4), !.arrived = 4]

\* Poll: the round starts at socket 1
P1 == [A4 EXCEPT !.pc = "read", !.cur = 1]

\* Read(2): a batch of two into a room of three
R1 == [P1 EXCEPT !.kq = (1 :> 2), !.cnt = B(2, 0, 0), !.reads = 2, !.roundRead = TRUE]

\* Read(1): the room is one, so the read asks for one
R2 == [R1 EXCEPT !.kq = (1 :> 1), !.cnt = B(3, 0, 0), !.reads = 3]

\* MoveFull: bank 0's three committed; main shares it, so the worker moves
\* to the free bank 1
M1 == [R2 EXCEPT !.committed = B(3, 0, 0), !.worker = 1, !.free = -1, !.committedTop = 3]

\* Read(1): a short read, the socket is drained and the round's sockets done
R3 == [M1 EXCEPT !.kq = (1 :> 0), !.cnt = B(3, 1, 0), !.reads = 0, !.cur = 0, !.pc = "end"]

\* RoundCommit: bank 1's one committed; main has not started on bank 1
K1 == [R3 EXCEPT !.committed = B(3, 1, 0), !.committedTop = 4, !.pc = "poll"]

\* Call(FALSE, 3): main moves on (bank 0 becomes the tail with three, bank 2
\* is freed, main starts on bank 1 up to one) and returns the tail's three,
\* the capacity cutting the shared bank's one
C1 == [K1 EXCEPT !.free = 2, !.tail = 0, !.main = 1, !.tailCursor = 3, !.tailEnd = 3,
                 !.mainCursor = 0, !.mainLimit = 1, !.consumed = 3]

\* MainStop
T1 == [C1 EXCEPT !.stopping = TRUE]

\* Exit: the worker leaves its loop at its top
E1 == [T1 EXCEPT !.exited = TRUE, !.pc = "exit"]

\* StopCount: the one main never returned is counted stopped
N1 == [E1 EXCEPT !.counted = TRUE, !.stopped = 1, !.mainLimit = 0, !.cnt = B(3, 0, 0)]

GoodLog == << Rec("init", "init", << >>, S0),
              Rec("arrive", "Arrive", << 1 >>, A1),
              Rec("arrive", "Arrive", << 1 >>, A2),
              Rec("arrive", "Arrive", << 1 >>, A3),
              Rec("arrive", "Arrive", << 1 >>, A4),
              Rec("poll", "Poll", << >>, P1),
              Rec("step", "Read", << 2 >>, R1),
              Rec("step", "Read", << 1 >>, R2),
              Rec("step", "MoveFull", << >>, M1),
              Rec("step", "Read", << 1 >>, R3),
              Rec("round", "RoundCommit", << >>, K1),
              Rec("call", "Call", << FALSE, 3 >>, C1),
              Rec("stop", "MainStop", << >>, T1),
              Rec("exit", "Exit", << >>, E1),
              Rec("count", "StopCount", << >>, N1) >>

\* the planted read: a whole batch of two into a room of one; one is banked
\* and the other lost
BadLog == << Rec("init", "init", << >>, S0),
             Rec("arrive", "Arrive", << 1 >>, A1),
             Rec("arrive", "Arrive", << 1 >>, A2),
             Rec("arrive", "Arrive", << 1 >>, A3),
             Rec("arrive", "Arrive", << 1 >>, A4),
             Rec("poll", "Poll", << >>, P1),
             Rec("step", "Read", << 2 >>, R1),
             Rec("step", "Read", << 2 >>, [R1 EXCEPT !.kq = (1 :> 0), !.cnt = B(3, 0, 0), !.reads = 4, !.fullDrops = 1]) >>

=============================================================================
