--------------------------- MODULE TraceRNFillPhase ---------------------------
(***************************************************************************)
(* The trace check of RNFillPhase (README-TraceCheck.md).  TraceLog is the *)
(* record sequence tests/tla_trace_fill_phase.cpp writes: the code of      *)
(* sdk/rocketnet_fill_phase.cpp run by real threads, one main and three   *)
(* fillers, released one step at a time from the code's own trace hook,   *)
(* with one record of every model variable after each step.  A record     *)
(* that names fewer or other fields than the model's variables is         *)
(* rejected.  TraceInit matches the first record against Init; TraceStep  *)
(* takes one model step whose successor matches the next record; when none *)
(* does, TraceReject sets rejected and TraceConsistent fails.             *)
(***************************************************************************)
EXTENDS RNFillPhase, Sequences

CONSTANT TraceLog

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

VarsRec == [mpc |-> mpc, mscan |-> mscan, fpc |-> fpc, fclient |-> fclient, fepoch |-> fepoch,
            seen |-> seen, saved |-> saved, mainCalls |-> mainCalls,
            fillsIn |-> fillsIn, epoch |-> epoch, word |-> word,
            owner |-> owner, fbroke |-> fbroke, phaseIn |-> phaseIn,
            falseAlarm |-> falseAlarm, mainsDone |-> mainsDone,
            fillsDone |-> fillsDone]

TraceInit ==
    /\ Init
    /\ ti = 1
    /\ rejected = (VarsRec # TraceLog[1])

TraceStep ==
    /\ ~rejected
    /\ ti < Len(TraceLog)
    /\ Next
    /\ VarsRec' = TraceLog[ti + 1]
    /\ ti' = ti + 1
    /\ UNCHANGED rejected

TraceReject ==
    /\ ~rejected
    /\ ti < Len(TraceLog)
    /\ ~ENABLED TraceStep
    /\ rejected' = TRUE
    /\ UNCHANGED <<vars, ti>>

TraceDone ==
    /\ rejected \/ ti = Len(TraceLog)
    /\ UNCHANGED <<vars, ti, rejected>>

TraceSpec == TraceInit /\ [][TraceStep \/ TraceReject \/ TraceDone]_<<vars, ti, rejected>>

TraceConsistent == ~rejected

=============================================================================
