------------------------ MODULE TraceRNUringQueueSend ------------------------
(***************************************************************************)
(* The trace check of RNUringQueueSend, copied from the template           *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_queue_send.cpp writes over                     *)
(* sdk/rocketnet_uring_queue_send.cpp and the banks under it: one record   *)
(* per call, with the call (op), its arguments and what it gave back       *)
(* (args), the model action its own result says it took (act), and every  *)
(* model variable after it (state).  A stage whose start returned a slot   *)
(* is Stage with that slot and id, one that returned NULL is StageFull; a  *)
(* publish that returned true is Publish, false PublishKept; a claim is    *)
(* WorkerClaim with the bank it returned; a submit is WorkerSubmit with    *)
(* the run next handed out; a reap is WorkerReap with the count complete   *)
(* returned; a retire that returned true is WorkerRetire, false            *)
(* WorkerRetireRefused; the fake kernel's post is KernelComplete with the  *)
(* run and its result.  Everything but EXTENDS, VarsRec, OpActs, Act and   *)
(* the reach predicates is the template's text.                            *)
(***************************************************************************)
EXTENDS RNUringQueueSend, Sequences

CONSTANT TraceLog

\* the log as a definition of no arguments, which TLC evaluates once when
\* it starts; every formula below reads Trace, never TraceLog. Read through
\* the substituted constant, TLC rebuilds the whole log at each access, and a
\* 3200-record trace takes 68 s instead of 3 s (bench-nyc-6, 2026-11-12)
Trace == TraceLog

\* checked once, when TLC starts
ASSUME /\ Len(TraceLog) >= 2
       /\ \A n \in DOMAIN TraceLog : DOMAIN TraceLog[n] = {"op", "act", "args", "state"}

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

\* every model variable by the field name the harness writes
VarsRec ==
    [producer |-> producer, ready |-> ready, worker |-> worker, free |-> free,
     num |-> num, st |-> st, dest |-> dest, pkt |-> pkt, runs |-> runs, cq |-> cq,
     pend |-> pend, status |-> status, nextId |-> nextId, eagains |-> eagains,
     demand |-> demand, flushes |-> flushes, kernelWrite |-> kernelWrite,
     pids |-> pids, locked |-> locked, mainWaits |-> mainWaits]

\* the actions each operation of the code can be
OpActs == [stage |-> {"Stage", "StageFull"}, publish |-> {"Publish", "PublishKept"},
           claim |-> {"WorkerClaim"}, submit |-> {"WorkerSubmit"},
           post |-> {"KernelComplete"}, reap |-> {"WorkerReap"},
           retire |-> {"WorkerRetire", "WorkerRetireRefused"},
           lock |-> {"WorkerLocks"}, unlock |-> {"WorkerUnlocks"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Stage" -> Stage(x[1], x[2], x[3], x[4])
               [] a = "StageFull" -> StageFull(x[1], x[2], x[3])
               [] a = "Publish" -> Publish
               [] a = "PublishKept" -> PublishKept
               [] a = "WorkerClaim" -> WorkerClaim(x[1])
               [] a = "WorkerSubmit" -> WorkerSubmit(x[1], x[2], x[3])
               [] a = "KernelComplete" -> KernelComplete(x[1], x[2])
               [] a = "WorkerReap" -> WorkerReap(x[1])
               [] a = "WorkerRetire" -> WorkerRetire
               [] a = "WorkerRetireRefused" -> WorkerRetireRefused
               [] a = "WorkerLocks" -> WorkerLocks
               [] a = "WorkerUnlocks" -> WorkerUnlocks

TraceInit ==
    /\ Init
    /\ ti = 1
    /\ rejected = (Trace[1].op # "init" \/ Trace[1].act # "init" \/ Trace[1].args # <<>>
                   \/ VarsRec # Trace[1].state)

TraceStep ==
    /\ ~rejected
    /\ ti < Len(Trace)
    /\ LET r == Trace[ti + 1]
       IN /\ r.op \in DOMAIN OpActs
          /\ r.act \in OpActs[r.op]
          /\ Act(r.act, r.args)
          /\ Next
          /\ VarsRec' = r.state
    /\ ti' = ti + 1
    /\ UNCHANGED rejected

TraceReject ==
    /\ ~rejected
    /\ ti < Len(Trace)
    /\ ~ENABLED TraceStep
    /\ rejected' = TRUE
    /\ UNCHANGED <<vars, ti>>

TraceDone ==
    /\ rejected \/ ti = Len(Trace)
    /\ UNCHANGED <<vars, ti, rejected>>

TraceSpec == TraceInit /\ [][TraceStep \/ TraceReject \/ TraceDone]_<<vars, ti, rejected>>

\* every record of the trace is the model step it names, from the one before
TraceConsistent == ~rejected

\* the row's reach predicates: some clean trace fills the producer, so the
\* next start is refused (Full), and has an eagain completion posted, so a
\* reap hands its run back to be submitted again (AgainPosted), and has
\* a publish kept because the worker held the handoff mutex with a staged
\* producer and a free bank (KeptLocked: the walk tries the publish there)
Full == num[producer] = S
AgainPosted == \E i \in 1..Len(cq) : cq[i].res = "eagain"
KeptLocked == locked /\ num[producer] # 0 /\ free # <<>> /\ flushes > 0

=============================================================================
