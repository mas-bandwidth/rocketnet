------------------------ MODULE TraceRNMsgxSendRuns ------------------------
(***************************************************************************)
(* The trace check of RNMsgxSendRuns (layer msgxsendruns), a copy of       *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_msgx_send_runs.cpp writes over the real        *)
(* sdk/rocketnet_msgx_send_runs.h with a fake table and fake connected and *)
(* shared sends: one record per step, with the operation the walk called   *)
(* (op: "sort"; "step", one call of rocketnet_msgx_send_runs_step;         *)
(* "check", the worker's give-up check after a kept call; "retire" and     *)
(* "quit", the environment), its arguments (the prefix a partial send      *)
(* took, the slot retired), the action the call's answer names (act: the  *)
(* fakes' answer, the give-up check's result, or the step's out-state     *)
(* where no fake was called) and every model variable after it (state).    *)
(* The cfg's Bank is the bank the walk staged, so Init is that one bank,   *)
(* and OpenGenAt the fake table.  Record 1 is "init".                      *)
(*                                                                         *)
(* A copy changes only EXTENDS, VarsRec, OpActs, Act and the row's reach   *)
(* predicates; tools/tla_trace_check.py compares the rest with the         *)
(* template.                                                               *)
(***************************************************************************)
EXTENDS RNMsgxSendRuns, Sequences

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
VarsRec == [count |-> count, client |-> client, slot |-> slot, gen |-> gen,
            live |-> live, tOpen |-> tOpen, tGen |-> tGen, order |-> order, pc |-> pc,
            cursor |-> cursor, hd |-> hd, scan |-> scan, run |-> run, attempts |-> attempts,
            left |-> left, lastC |-> lastC, lastS |-> lastS, bad |-> bad, quit |-> quit,
            retries |-> retries, stalls |-> stalls, retires |-> retires]

\* the actions each operation of the code can be
OpActs == [sort |-> {"Sort"},
           step |-> {"HeadDone", "HeadSkip", "HeadShared", "HeadDropped", "HeadKept", "HeadRun",
                     "SendAll", "SendPartial", "SendFull", "SendRefused", "SendDead", "SendError",
                     "DeadDone", "DeadSkip", "DeadShared", "DeadDropped", "DeadKept", "Done"},
           check |-> {"GiveUp", "Resume"},
           retire |-> {"Retire"},
           quit |-> {"Quit"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Sort" -> Sort
               [] a = "HeadDone" -> HeadDone
               [] a = "HeadSkip" -> HeadSkip
               [] a = "HeadShared" -> HeadShared
               [] a = "HeadDropped" -> HeadDropped
               [] a = "HeadKept" -> HeadKept
               [] a = "HeadRun" -> HeadRun
               [] a = "SendAll" -> SendAll
               [] a = "SendPartial" -> SendPartial(x[1])
               [] a = "SendFull" -> SendFull
               [] a = "SendRefused" -> SendRefused
               [] a = "SendDead" -> SendDead
               [] a = "SendError" -> SendError
               [] a = "DeadDone" -> DeadDone
               [] a = "DeadSkip" -> DeadSkip
               [] a = "DeadShared" -> DeadShared
               [] a = "DeadDropped" -> DeadDropped
               [] a = "DeadKept" -> DeadKept
               [] a = "GiveUp" -> GiveUp
               [] a = "Resume" -> Resume
               [] a = "Done" -> Done
               [] a = "Quit" -> Quit
               [] a = "Retire" -> Retire(x[1])

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
