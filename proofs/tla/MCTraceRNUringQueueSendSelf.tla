------------------- MODULE MCTraceRNUringQueueSendSelf -------------------
(***************************************************************************)
(* The trace check's own controls, before any code writes a trace: a       *)
(* hand-written record of one packet's way through a queue (staged,        *)
(* published, taken by the worker, submitted, completed, reaped, the bank *)
(* retired) must be accepted (MCTraceRNUringQueueSendSelf.cfg), and the    *)
(* same record with the bank retired before its completion must be         *)
(* rejected at that record (MCTraceRNUringQueueSendSelfBrokenRetire.cfg,   *)
(* TraceConsistent).  Every record carries every variable, the kernel's    *)
(* completion result included, so no step of the model branches on a      *)
(* choice the record does not show (README-TraceCheck.md: a record that    *)
(* leaves a choice open is rejected on the branch it did not take).  The   *)
(* harness's traces are checked the same way, with TraceLog from the      *)
(* module it writes.                                                       *)
(***************************************************************************)
EXTENDS TraceRNUringQueueSend

F(x, y, z) == [b \in 0..2 |-> CASE b = 0 -> x [] b = 1 -> y [] OTHER -> z]
Two(x, y) == [s \in 0..1 |-> IF s = 0 THEN x ELSE y]
Stat(v) == [i \in Ids |-> IF i = 1 THEN v ELSE "none"]

E == Two("empty", "empty")
Z == Two(0, 0)
Run == [b |-> 0, slots |-> {0}, ids |-> {1}]

S1 == [producer |-> 0, ready |-> <<>>, worker |-> -1, free |-> <<1, 2>>,
       num |-> F(0, 0, 0), st |-> F(E, E, E), dest |-> F(Z, Z, Z), pkt |-> F(Z, Z, Z),
       runs |-> {}, cq |-> <<>>, pend |-> F(0, 0, 0), status |-> Stat("none"),
       nextId |-> 1, eagains |-> 0, demand |-> 0, flushes |-> 0, kernelWrite |-> FALSE]

\* Stage(1, TRUE): slot 0 of bank 0 staged to destination 1 as packet 1
S2 == [S1 EXCEPT !.num = F(1, 0, 0), !.st = F(Two("staged", "empty"), E, E),
                 !.dest = F(Two(1, 0), Z, Z), !.pkt = F(Two(1, 0), Z, Z),
                 !.status = Stat("staged"), !.nextId = 2, !.demand = 1]

\* Publish: bank 0 ready, bank 1 the producer
S3 == [S2 EXCEPT !.ready = <<0>>, !.producer = 1, !.free = <<2>>, !.flushes = 1, !.demand = 0]

\* WorkerClaim
S4 == [S3 EXCEPT !.worker = 0, !.ready = <<>>]

\* WorkerSubmit(1, 1)
S5 == [S4 EXCEPT !.st = F(Two("run", "empty"), E, E), !.runs = {Run},
                 !.pend = F(1, 0, 0), !.status = Stat("kernel")]

\* KernelComplete(Run, "ok")
S6 == [S5 EXCEPT !.runs = {}, !.cq = << [run |-> Run, res |-> "ok"] >>]

\* WorkerReap
S7 == [S6 EXCEPT !.cq = <<>>, !.st = F(Two("done", "empty"), E, E),
                 !.status = Stat("sent"), !.pend = F(0, 0, 0)]

\* WorkerRetire
S8 == [S7 EXCEPT !.worker = -1, !.free = <<2, 0>>, !.st = F(E, E, E),
                 !.dest = F(Z, Z, Z), !.pkt = F(Z, Z, Z)]

GoodLog == << S1, S2, S3, S4, S5, S6, S7, S8 >>

\* the bank retired from S5, its run still in the kernel: record 6 is no
\* model step
BadRetire == [S5 EXCEPT !.worker = -1, !.free = <<2, 0>>, !.st = F(E, E, E),
                        !.dest = F(Z, Z, Z), !.pkt = F(Z, Z, Z)]

BadLog == << S1, S2, S3, S4, S5, BadRetire >>

=============================================================================
