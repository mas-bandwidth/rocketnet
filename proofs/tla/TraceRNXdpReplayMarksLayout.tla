------------------- MODULE TraceRNXdpReplayMarksLayout -------------------
(***************************************************************************)
(* The trace check of RNXdpReplayMarksLayout, copied from the template     *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_xdp_replay_marks.cpp writes over the code of   *)
(* sdk/rocketnet_xdp_replay_marks.h: one record per call, with the call    *)
(* (op), its arguments and what it gave back (args), the model action its  *)
(* own result says it took (act), and every model variable after it        *)
(* (state).  A forget is Forget(k, ok) and a mark Mark(k, v, ok), ok what  *)
(* the call returned; a commit, the kernel's answer the walk chose handed  *)
(* to rocketnet_xdp_replay_marks_reset_keeping, is ResetKeeping(fs, as, n) *)
(* with n what the call returned: the whole call as one step, since the    *)
(* walk sees nothing inside it.  The ghosts absF, absM and absK are the    *)
(* code's own pending lists and kept_mark_count after each call.           *)
(* Everything but EXTENDS, VarsRec, OpActs, Act and the reach predicates   *)
(* is the template's text.                                                 *)
(***************************************************************************)
EXTENDS RNXdpReplayMarksLayout, Sequences

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
    [forgets |-> forgets, fc |-> fc, kf |-> kf, mkeys |-> mkeys, mvals |-> mvals,
     mc |-> mc, kc |-> kc, refused |-> refused, phase |-> phase, li |-> li, lk |-> lk,
     failed |-> failed, applied |-> applied, doneF |-> doneF, doneM |-> doneM,
     absF |-> absF, absM |-> absM, absK |-> absK, overflow |-> overflow]

\* the actions each operation of the code can be
OpActs == [forget |-> {"Forget"}, mark |-> {"Mark"}, reset_keeping |-> {"ResetKeeping"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Forget" -> Forget(x[1], x[2])
               [] a = "Mark" -> Mark(x[1], x[2], x[3])
               [] a = "ResetKeeping" -> ResetKeeping(x[1], x[2], x[3])

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

\* the row's reach predicates, the layout's edges some clean trace must take:
\* the forget list full, the mark list full, a call refused, a forget the
\* last commit kept, a mark the last commit kept
ForgetsFull == fc = M
MarksFull == mc = M
SomeRefused == refused > 0
ForgetsKept == kf > 0
MarksKept == kc > 0

=============================================================================
