--------------------------- MODULE TraceRNFillPhase ---------------------------
(***************************************************************************)
(* The trace check of RNFillPhase (README-TraceCheck.md), a copy of the    *)
(* template TraceRNTraceExample.tla.  TraceLog is the record sequence      *)
(* tests/tla_trace_fill_phase.cpp writes over sdk/rocketnet_fill_phase.cpp *)
(* run by real threads, one main and three fillers, released one step at  *)
(* a time from the code's own trace hook (a filler makes fill calls, some *)
(* with calls nested inside, and takes, the main thread update calls that *)
(* open and close the window and other operations): one record per step,  *)
(* with the                                                                *)
(* operation the thread was in (op), the step's arguments and what it     *)
(* gave back (args), the model action the hook named (act) and every      *)
(* model variable after it (state).  Record 1 is "init".                   *)
(*                                                                         *)
(* TraceStep allows only the recorded action with the recorded arguments,  *)
(* and only when the operation can be that action (OpActs): a step that    *)
(* did one thing but landed in a state another action allows is rejected.  *)
(* The step must be a Next step too, so Next's ranges and guards around an *)
(* action hold for the recorded arguments.  A state that names fewer or    *)
(* other fields than VarsRec never matches, and a record that is not       *)
(* [op, act, args, state] fails the ASSUME: an under-logged harness fails  *)
(* red, never green.                                                       *)
(*                                                                         *)
(* When no step matches, TraceReject sets rejected, so TraceConsistent     *)
(* fails; the behaviour TLC prints ends at ti, the last record that        *)
(* matched, and record ti + 1 is the first code step that is not a model   *)
(* step.  At the last record the trace stutters (TraceDone).               *)
(***************************************************************************)
EXTENDS RNFillPhase, Sequences

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
VarsRec == [mpc |-> mpc, mscan |-> mscan, mkind |-> mkind, fpc |-> fpc, fclient |-> fclient, fepoch |-> fepoch,
            seen |-> seen, saved |-> saved, mainCalls |-> mainCalls, window |-> window, stage |-> stage, fresh |-> fresh, fillsIn |-> fillsIn,
            fline |-> fline, epoch |-> epoch, word |-> word, owner |-> owner, armed |-> armed,
            overlaps |-> overlaps, twoFillers |-> twoFillers, opted |-> opted,
            fbroke |-> fbroke,
            phaseIn |-> phaseIn, falseAlarm |-> falseAlarm, mainsDone |-> mainsDone,
            fillsDone |-> fillsDone, takesDone |-> takesDone]

\* the steps each call of the code can take
OpActs == [main_begin |-> {"MainAnnounce", "MainLook", "MainIn"},
           main_end |-> {"MainEpoch", "MainDrop"},
           fill_begin |-> {"FillBegin", "FillLock", "FillDecide", "FillCount", "FillLook", "FillPeek", "FillNest"},
           fill_end |-> {"FillUnnest", "FillUnclaim", "FillLeave"},
           take_begin |-> {"TakeCount", "TakeLook"},
           take_end |-> {"TakeLeave"},
           fill_scope |-> {"FillSkip"},
           take_scope |-> {"TakeSkip"},
           main_scope |-> {"MainSkip"},
           arm |-> {"Arm"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "MainAnnounce" -> MainAnnounce(x[1])
               [] a = "MainLook" -> MainLook(x[1], x[2])
               [] a = "MainIn" -> MainIn(x[1])
               [] a = "MainEpoch" -> MainEpoch(x[1], x[2])
               [] a = "MainDrop" -> MainDrop
               [] a = "MainSkip" -> MainSkip(x[1], x[2], x[3])
               [] a = "FillSkip" -> FillSkip(x[1])
               [] a = "TakeSkip" -> TakeSkip(x[1])
               [] a = "Arm" -> Arm
               [] a = "FillBegin" -> FillBegin(x[1], x[2])
               [] a = "FillLock" -> FillLock(x[1], x[2])
               [] a = "FillDecide" -> FillDecide(x[1], x[2])
               [] a = "FillCount" -> FillCount(x[1], x[2])
               [] a = "FillLook" -> FillLook(x[1], x[2])
               [] a = "FillPeek" -> FillPeek(x[1])
               [] a = "FillNest" -> FillNest(x[1], x[2])
               [] a = "FillUnnest" -> FillUnnest(x[1])
               [] a = "FillUnclaim" -> FillUnclaim(x[1])
               [] a = "FillLeave" -> FillLeave(x[1])
               [] a = "TakeCount" -> TakeCount(x[1], x[2])
               [] a = "TakeLook" -> TakeLook(x[1], x[2])
               [] a = "TakeLeave" -> TakeLeave(x[1])

\* each refusal of the check, and the window's work, taken in some accepted
\* trace of the seeds (tools/tla_trace_check.py, the row's reach)
RefusedMainLook == refusal = "MainLook"
RefusedMainIn == refusal = "MainIn" /\ window
RefusedFillLock == refusal = "FillLock"
RefusedFillDecide == refusal = "FillDecide"
RefusedFillLookByMain == refusal = "FillLook" /\ ~window
RefusedFillLookInWindow == refusal = "FillLook" /\ window /\ mpc = "idle"
RefusedTakeLook == refusal = "TakeLook"
TakeInWindow == window /\ mpc = "idle" /\ \E f \in Fillers : fpc[f] = "tin"
TwoTakesInWindow == window /\ \E f, g \in Fillers : f # g /\ fpc[f] = "tin" /\ fpc[g] = "tin"
RefusedMainBesideTake == refusal = "MainLook" /\ \E f \in Fillers : fpc[f] = "tin"
WindowOpened == window /\ mpc = "epoched" /\ mkind = "update"
RefusedFillNest == refusal = "FillNest" /\ \E f \in Fillers : fpc[f] = "in"
NestedCall == \E f \in Fillers : fpc[f] = "in" /\ word[fclient[f]].depth > 1

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
