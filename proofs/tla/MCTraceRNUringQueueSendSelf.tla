------------------- MODULE MCTraceRNUringQueueSendSelf -------------------
(***************************************************************************)
(* The trace check's own controls, before any code writes a trace: a       *)
(* hand-written record of one packet's way through a queue (staged,        *)
(* published, taken by the worker, submitted, completed, reaped, the bank *)
(* retired) must be accepted (MCTraceRNUringQueueSendSelf.cfg), and the    *)
(* same record with the bank retired before its completion must be         *)
(* rejected at that record (MCTraceRNUringQueueSendSelfBrokenRetire.cfg,   *)
(* TraceConsistent).  The harness's traces are checked the same way, with  *)
(* TraceLog from the module it writes.                                     *)
(***************************************************************************)
EXTENDS TraceRNUringQueueSend

F(x, y, z) == [b \in 0..2 |-> CASE b = 0 -> x [] b = 1 -> y [] OTHER -> z]

R(p, rd, w, fr, n, pe, id, fl) ==
    [producer |-> p, ready |-> rd, worker |-> w, free |-> fr,
     num |-> n, pend |-> pe, nextId |-> id, flushes |-> fl]

Head5 ==
    << R(0, <<>>, -1, <<1, 2>>, F(0, 0, 0), F(0, 0, 0), 1, 0),    \* Init
       R(0, <<>>, -1, <<1, 2>>, F(1, 0, 0), F(0, 0, 0), 2, 0),    \* Stage
       R(1, <<0>>, -1, <<2>>,   F(1, 0, 0), F(0, 0, 0), 2, 1),    \* Publish
       R(1, <<>>,  0, <<2>>,    F(1, 0, 0), F(0, 0, 0), 2, 1),    \* WorkerClaim
       R(1, <<>>,  0, <<2>>,    F(1, 0, 0), F(1, 0, 0), 2, 1) >>  \* WorkerSubmit

GoodLog ==
    Head5 \o
    << R(1, <<>>,  0, <<2>>,    F(1, 0, 0), F(1, 0, 0), 2, 1),    \* KernelComplete
       R(1, <<>>,  0, <<2>>,    F(1, 0, 0), F(0, 0, 0), 2, 1),    \* WorkerReap
       R(1, <<>>, -1, <<2, 0>>, F(1, 0, 0), F(0, 0, 0), 2, 1) >>  \* WorkerRetire

\* the bank retired with its run still pending: record 6 is no model step
BadLog ==
    Head5 \o
    << R(1, <<>>, -1, <<2, 0>>, F(1, 0, 0), F(1, 0, 0), 2, 1) >>

=============================================================================
