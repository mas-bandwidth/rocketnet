--------------------- MODULE MCTraceRNMsgxSocketTableSelf ---------------------
(***************************************************************************)
(* The trace check's own controls, before any code writes a trace: a       *)
(* hand-written record of one client's socket (opened, a packet stamped,   *)
(* resolved by transmit holder 1 inside its pass, the client forgotten,    *)
(* a sweep that keeps the socket in quarantine while the pass is open, the *)
(* send, the pass ended, a sweep that closes it) must be accepted          *)
(* (MCTraceRNMsgxSocketTableSelf.cfg), and the same record with the        *)
(* socket closed by the first sweep, while holder 1 is still inside the    *)
(* pass it resolved the descriptor in, must be rejected at that record     *)
(* (MCTraceRNMsgxSocketTableSelfBrokenClose.cfg, TraceConsistent).  Every  *)
(* record carries every variable and nothing else; the calls are a         *)
(* sequence of their own, one per record.                                  *)
(***************************************************************************)
EXTENDS TraceRNMsgxSocketTable, TLC

MCHome(a) == CASE a = 1 -> 6 [] a = 2 -> 7 [] a = 3 -> 7 [] OTHER -> 0
MCCoolOf(a) == (a - 1) % 2

Op(n, a, h, i, f) == [n |-> n, a |-> a, h |-> h, i |-> i, f |-> f]
F3(x, y, z) == [s \in 0..2 |-> CASE s = 0 -> x [] s = 1 -> y [] OTHER -> z]
H3(x, y, z) == [h \in 1..3 |-> CASE h = 1 -> x [] h = 2 -> y [] OTHER -> z]
Z3 == H3(0, 0, 0)

S1 == [word |-> F3(<<0, 0>>, <<0, 0>>, <<0, 0>>), deadRep |-> F3(0, 0, 0), state |-> F3(0, 0, 0),
       fd |-> F3(-1, -1, -1), gen |-> F3(0, 0, 0), addr |-> F3(0, 0, 0), snap |-> F3(Z3, Z3, Z3),
       pass |-> Z3, deadReports |-> 0, freeSlots |-> F3(2, 1, 0), numFree |-> 3,
       retired |-> F3(0, 0, 0), numRetired |-> 0, numOpen |-> 0, index |-> << >>, cool |-> << >>,
       counters |-> [c \in 0..7 |-> 0], update |-> 0, opensLeft |-> 1, mpc |-> "idle",
       ms |-> 0, mi |-> 0, mk |-> 0, kfd |-> F3(-1, -1, -1), kdead |-> F3(FALSE, FALSE, FALSE),
       poller |-> {}, pkts |-> << >>, cur |-> [h \in 1..2 |-> 0], rep |-> H3(<< >>, << >>, << >>),
       rxEv |-> << >>, rxFd |-> -1, crossed |-> FALSE, onClosed |-> FALSE, attempts |-> 0]

\* Mark(1): slot 0 (the top of the free stack) opens descriptor 0, index
\* entry 6 (address 1's home), registered with the poller
S2 == [S1 EXCEPT !.word = F3(<<0, 1>>, <<0, 0>>, <<0, 0>>), !.state = F3(1, 0, 0), !.fd = F3(0, -1, -1),
                 !.addr = F3(1, 0, 0), !.numFree = 2, !.numOpen = 1, !.index = (6 :> 0),
                 !.counters = [c \in 0..7 |-> IF c = 0 THEN 1 ELSE 0], !.opensLeft = 0,
                 !.kfd = F3(1, -1, -1), !.poller = {<<0, 0, 0, 1>>}, !.attempts = 1]

Pkt(st, f) == [a |-> 1, s |-> 0, g |-> 0, h |-> 1, st |-> st, fd |-> f]

\* Stamp(1, 1): a packet to address 1 for transmit holder 1, slot 0 generation 0
S3 == [S2 EXCEPT !.pkts = << Pkt("queued", -1) >>]

\* HolderBegin(1)
S4 == [S3 EXCEPT !.pass = H3(1, 0, 0)]

\* TxResolve(1, 1): descriptor 0
S5 == [S4 EXCEPT !.pkts = << Pkt("resolved", 0) >>, !.cur = [h \in 1..2 |-> IF h = 1 THEN 1 ELSE 0]]

\* Forget(1): out of the index, generation 1, the word without a descriptor,
\* holder 1's pass (1) snapshotted, the slot in quarantine, deregistered
S6 == [S5 EXCEPT !.word = F3(<<1, 0>>, <<0, 0>>, <<0, 0>>), !.gen = F3(1, 0, 0), !.state = F3(2, 0, 0),
                 !.snap = F3(H3(1, 0, 0), Z3, Z3), !.numRetired = 1, !.numOpen = 0, !.index = << >>,
                 !.counters = [c \in 0..7 |-> IF c = 0 \/ c = 5 THEN 1 ELSE 0], !.poller = {}]

\* SweepBegin and the close loop: holder 1 is still inside the pass, kept
S7 == S6

\* TxSend(1): descriptor 0 is still connected to address 1
S8 == [S7 EXCEPT !.pkts = << Pkt("conn", 0) >>, !.cur = [h \in 1..2 |-> 0]]

\* HolderEnd(1)
S9 == [S8 EXCEPT !.pass = H3(2, 0, 0)]

\* the sweep closes descriptor 0 and frees slot 0
S10 == [S9 EXCEPT !.state = F3(0, 0, 0), !.fd = F3(-1, -1, -1), !.numFree = 3, !.numRetired = 0,
                  !.counters = [c \in 0..7 |-> IF c = 0 \/ c = 1 \/ c = 5 THEN 1 ELSE 0],
                  !.kfd = F3(-1, -1, -1)]

GoodLog == << S1, S2, S3, S4, S5, S6, S7, S8, S9, S10 >>

GoodOps == << Op("init", 0, 0, 0, 0), Op("mark", 1, 0, 0, 0), Op("stamp", 1, 1, 0, 0),
              Op("begin", 0, 1, 0, 0), Op("resolve", 0, 1, 1, 0), Op("forget", 1, 0, 0, 0),
              Op("sweep", 0, 0, 0, 0), Op("send", 0, 1, 0, 0), Op("end", 0, 1, 0, 0),
              Op("sweep", 0, 0, 0, 0) >>

\* the first sweep closes descriptor 0 while holder 1 is inside the pass it
\* resolved it in: record 7 is no model step
BadClose == [S6 EXCEPT !.state = F3(0, 0, 0), !.fd = F3(-1, -1, -1), !.numFree = 3, !.numRetired = 0,
                       !.counters = [c \in 0..7 |-> IF c = 0 \/ c = 1 \/ c = 5 THEN 1 ELSE 0],
                       !.kfd = F3(-1, -1, -1)]

BadLog == << S1, S2, S3, S4, S5, S6, BadClose >>

BadOps == SubSeq(GoodOps, 1, 7)

=============================================================================
