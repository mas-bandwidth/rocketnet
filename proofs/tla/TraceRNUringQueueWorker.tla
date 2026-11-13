----------------------- MODULE TraceRNUringQueueWorker -----------------------
(***************************************************************************)
(* The trace check of RNUringQueueWorker, copied from the template         *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_queue_worker.cpp writes over                   *)
(* sdk/rocketnet_uring_queue_worker.cpp: one record per call, with the     *)
(* call (op), its arguments (args, none: no action of this model takes     *)
(* any), the model action its own result says it took (act), and every     *)
(* model variable after it (state).  A worker step is the action           *)
(* rocketnet_uring_queue_worker_step returned (a reap by the kind of the   *)
(* completion it took, the oldest); a publish is Publish when it returned  *)
(* true; a stop is Stop; each of the fake kernel's answers is the Kernel   *)
(* action it is.  Everything but EXTENDS, VarsRec, OpActs and Act is the   *)
(* template's text.                                                        *)
(***************************************************************************)
EXTENDS RNUringQueueWorker, Sequences

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
    [quit |-> quit, seen |-> seen, st |-> st, handed |-> handed, ready |-> ready,
     held |-> held, u |-> u, k |-> k, cOk |-> cOk, cAgain |-> cAgain, cRef |-> cRef,
     blocked |-> blocked, poll |-> poll, recv |-> recv, wakeRead |-> wakeRead,
     wakeCount |-> wakeCount, agains |-> agains, againsBefore |-> againsBefore,
     refusals |-> refusals, recvEnds |-> recvEnds, p |-> p]

\* the actions each operation of the code can be
OpActs == [publish |-> {"Publish"}, stop |-> {"Stop"},
           step |-> {"Observe", "ReapOk", "ReapAgain", "ReapRefused", "ReapPoll",
                    "ReapRecvEnd", "ReapWake", "ReapCancel",
                    "Retire", "Claim", "Submit", "Split", "Drop",
                    "Abandon", "ArmPoll", "ArmRecv", "ArmWake",
                    "CancelRecv", "CancelWake", "CancelPoll", "Exit",
                    "Sleep", "Done"},
           wakes |-> {"KernelWakesWorker"}, answer |-> {"KernelOk", "KernelAgain", "KernelRefuse"},
           pollFires |-> {"KernelPoll"}, recvEnds |-> {"KernelRecvEnd"},
           recvEndsItself |-> {"KernelRecvEndsItself"}, wakeReads |-> {"KernelWakeRead"},
           wakeEnds |-> {"KernelWakeEnd"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Publish" -> Publish
               [] a = "Stop" -> Stop
               [] a = "Observe" -> Observe
               [] a = "ReapOk" -> ReapOk
               [] a = "ReapAgain" -> ReapAgain
               [] a = "ReapRefused" -> ReapRefused
               [] a = "ReapPoll" -> ReapPoll
               [] a = "ReapRecvEnd" -> ReapRecvEnd
               [] a = "ReapWake" -> ReapWake
               [] a = "ReapCancel" -> ReapCancel
               [] a = "Retire" -> Retire
               [] a = "Claim" -> Claim
               [] a = "Submit" -> Submit
               [] a = "Split" -> Split
               [] a = "Drop" -> Drop
               [] a = "Abandon" -> Abandon
               [] a = "ArmPoll" -> ArmPoll
               [] a = "ArmRecv" -> ArmRecv
               [] a = "ArmWake" -> ArmWake
               [] a = "CancelRecv" -> CancelRecv
               [] a = "CancelWake" -> CancelWake
               [] a = "CancelPoll" -> CancelPoll
               [] a = "Exit" -> Exit
               [] a = "Sleep" -> Sleep
               [] a = "Done" -> Done
               [] a = "KernelOk" -> KernelOk
               [] a = "KernelAgain" -> KernelAgain
               [] a = "KernelRefuse" -> KernelRefuse
               [] a = "KernelPoll" -> KernelPoll
               [] a = "KernelRecvEnd" -> KernelRecvEnd
               [] a = "KernelRecvEndsItself" -> KernelRecvEndsItself
               [] a = "KernelWakeRead" -> KernelWakeRead
               [] a = "KernelWakeEnd" -> KernelWakeEnd
               [] a = "KernelWakesWorker" -> KernelWakesWorker

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

=============================================================================
