------------------- MODULE MCTraceRNMsgxReceiveFillSelf -------------------
(***************************************************************************)
(* The trace check's own controls, before any code writes a trace: a       *)
(* hand-written record of one socket and banks of three, four datagrams    *)
(* arriving, a read of two, a read of one into the last slot, the full     *)
(* bank handed to main, the last datagram read short, the round kept (main *)
(* has not taken the ready bank), main's claim and its consume, must be    *)
(* accepted (MCTraceRNMsgxReceiveFillSelf.cfg); the same record with the   *)
(* planted second read (a whole batch of two into a room of one, the       *)
(* second datagram lost) must be rejected at that read                     *)
(* (MCTraceRNMsgxReceiveFillSelfBrokenReadPastRoom.cfg, TraceConsistent).  *)
(* Every record is [op, act, args, state] and its state carries every      *)
(* variable and nothing else.                                              *)
(***************************************************************************)
EXTENDS TraceRNMsgxReceiveFill, TLC

Rec(o, a, x, s) == [op |-> o, act |-> a, args |-> x, state |-> s]

B(x, y, z) == (0 :> x @@ 1 :> y @@ 2 :> z)

S0 == [kq |-> (1 :> 0), errs |-> (1 :> << >>), cnt |-> B(0, 0, 0), worker |-> 0, ready |-> -1,
       main |-> 1, free |-> 2, pc |-> "poll", cur |-> 0, reads |-> 0, arrived |-> 0, kdrops |-> 0,
       consumed |-> 0, fullDrops |-> 0, errors |-> 0]

A1 == [S0 EXCEPT !.kq = (1 :> 1), !.arrived = 1]
A2 == [S0 EXCEPT !.kq = (1 :> 2), !.arrived = 2]
A3 == [S0 EXCEPT !.kq = (1 :> 3), !.arrived = 3]
A4 == [S0 EXCEPT !.kq = (1 :> 4), !.arrived = 4]

\* Poll: the round starts at socket 1
P1 == [A4 EXCEPT !.pc = "read", !.cur = 1]

\* Read(2): a batch of two into a room of three
R1 == [P1 EXCEPT !.kq = (1 :> 2), !.cnt = B(2, 0, 0), !.reads = 2]

\* Read(1): the room is one, so the read asks for one
R2 == [R1 EXCEPT !.kq = (1 :> 1), !.cnt = B(3, 0, 0), !.reads = 3]

\* PublishFull: bank 0 is ready, the worker fills bank 2
U1 == [R2 EXCEPT !.ready = 0, !.worker = 2, !.free = -1]

\* Read(1): a short read, the socket is drained and the round's sockets done
R3 == [U1 EXCEPT !.kq = (1 :> 0), !.cnt = B(3, 0, 1), !.reads = 0, !.cur = 0, !.pc = "end"]

\* RoundKeep: bank 0 is still ready
K1 == [R3 EXCEPT !.pc = "poll"]

\* Claim: main takes bank 0, its old bank 1 is free
C1 == [K1 EXCEPT !.main = 0, !.ready = -1, !.free = 1]

\* Consume: main takes bank 0's three
C2 == [C1 EXCEPT !.cnt = B(0, 0, 1), !.consumed = 3]

GoodLog == << Rec("init", "init", << >>, S0),
              Rec("arrive", "Arrive", << 1 >>, A1),
              Rec("arrive", "Arrive", << 1 >>, A2),
              Rec("arrive", "Arrive", << 1 >>, A3),
              Rec("arrive", "Arrive", << 1 >>, A4),
              Rec("poll", "Poll", << >>, P1),
              Rec("step", "Read", << 2 >>, R1),
              Rec("step", "Read", << 1 >>, R2),
              Rec("step", "PublishFull", << >>, U1),
              Rec("step", "Read", << 1 >>, R3),
              Rec("round", "RoundKeep", << >>, K1),
              Rec("claim", "Claim", << >>, C1),
              Rec("consume", "Consume", << >>, C2) >>

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
