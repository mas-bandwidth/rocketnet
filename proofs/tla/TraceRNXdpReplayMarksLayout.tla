------------------- MODULE TraceRNXdpReplayMarksLayout -------------------
(***************************************************************************)
(* The trace check of RNXdpReplayMarksLayout (README-TraceCheck.md).       *)
(* TraceLog is the record sequence tests/tla_trace_xdp_replay_marks.cpp    *)
(* writes over the code of sdk/rocketnet_xdp_replay_marks.h: one record    *)
(* per step, [op, act, args, state], with the operation the walk called,   *)
(* the model action the code's own result says it took, the walk's         *)
(* arguments to it (the action's parameters, in order) and every model     *)
(* variable after it.  Record 1 is "init".                                 *)
(*                                                                         *)
(* The operations:                                                         *)
(*   forget  rocketnet_xdp_replay_marks_forget: Forget(k)                  *)
(*   mark    rocketnet_xdp_replay_marks_mark: Mark(k, v)                   *)
(*   commit  the kernel's answer, chosen by the walk (no library call):    *)
(*           Commit, args <<failed, applied>>                              *)
(*   reset_keeping  rocketnet_xdp_replay_marks_reset_keeping: one          *)
(*           CompactForgetStep per iteration of its forget loop and one    *)
(*           for its end, then the same for its mark loop                  *)
(*                                                                         *)
(* A hidden step.  reset_keeping is one call, and the model takes a step   *)
(* per loop iteration.  The walk cannot see inside the call, so every one  *)
(* of its records but the last has the state <<>>: a step the walk names   *)
(* and does not observe.  Matches accepts any successor of the named       *)
(* action for such a record; the last record of the call carries the       *)
(* code's state after it, and every hidden step before it is pinned by     *)
(* that.  A hidden record is sound only for an action with one successor   *)
(* from each state (CompactForgetStep and CompactMarkStep are both         *)
(* deterministic): with two, the branch the code did not take would dead-  *)
(* end at the next observed record and TraceReject would fire on it, a     *)
(* false red.  Never use a hidden record for any other action.             *)
(*                                                                         *)
(* The ghosts absF, absM and absK are written from the code's own lists    *)
(* (its pending forgets, its pending marks and kept_mark_count), at the    *)
(* commit record from the lists the call left, so the model's contract     *)
(* update at Commit is checked against what the code's compaction did.     *)
(*                                                                         *)
(* TraceStep allows only the recorded action, and only when the operation  *)
(* can be that action (OpActs).  A state that names fewer or other fields  *)
(* than VarsRec never matches, and a record that is not [op, act, args,    *)
(* state] fails the ASSUME.  When no step matches, TraceReject sets        *)
(* rejected, so TraceConsistent fails; record ti + 1 is the first code     *)
(* step that is not a model step.  At the last record the trace stutters   *)
(* (TraceDone).                                                            *)
(***************************************************************************)
EXTENDS RNXdpReplayMarksLayout, Sequences

CONSTANT TraceLog

\* checked once, when TLC starts
ASSUME /\ Len(TraceLog) >= 2
       /\ \A n \in DOMAIN TraceLog : DOMAIN TraceLog[n] = {"op", "act", "args", "state"}

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

\* every model variable by the field name the harness writes
VarsRec == [forgets |-> forgets, fc |-> fc, kf |-> kf, mkeys |-> mkeys, mvals |-> mvals,
            mc |-> mc, kc |-> kc, refused |-> refused, phase |-> phase, li |-> li, lk |-> lk,
            failed |-> failed, applied |-> applied, doneF |-> doneF, doneM |-> doneM,
            absF |-> absF, absM |-> absM, absK |-> absK, overflow |-> overflow]

\* the actions each operation of the code can be
OpActs == [forget |-> {"Forget"},
           mark |-> {"Mark"},
           commit |-> {"Commit"},
           reset_keeping |-> {"CompactForgetStep", "CompactMarkStep"}]

\* the action a record names; a name this CASE lacks stops TLC with an error
Act(a, x) == CASE a = "Forget" -> Forget(x[1])
               [] a = "Mark" -> Mark(x[1], x[2])
               [] a = "Commit" -> Commit
               [] a = "CompactForgetStep" -> CompactForgetStep
               [] a = "CompactMarkStep" -> CompactMarkStep

\* the successor is the record's state, or the record is a hidden step
Matches(r) == IF DOMAIN r.state = {} THEN TRUE ELSE VarsRec' = r.state

TraceInit ==
    /\ Init
    /\ ti = 1
    /\ rejected = (TraceLog[1].op # "init" \/ TraceLog[1].act # "init" \/ VarsRec # TraceLog[1].state)

TraceStep ==
    /\ ~rejected
    /\ ti < Len(TraceLog)
    /\ LET r == TraceLog[ti + 1]
       IN /\ r.op \in DOMAIN OpActs
          /\ r.act \in OpActs[r.op]
          /\ Act(r.act, r.args)
          /\ Matches(r)
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

\* every record of the trace is the model step it names, from the one before
TraceConsistent == ~rejected

=============================================================================
