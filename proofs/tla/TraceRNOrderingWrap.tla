------------------------- MODULE TraceRNOrderingWrap -------------------------
(***************************************************************************)
(* The trace check of RNOrderingWrap, a copy of the template               *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_ordering_wrap.cpp writes: one per step, with   *)
(* the operation the walk called (op), the walk's arguments to it (args),  *)
(* the model action the code's own result says it took (act), and every    *)
(* model variable after it (state), the ghosts disagree and wasted aside.  *)
(* Record 1 is "init".                                                     *)
(*                                                                         *)
(* TraceStep allows only the recorded action with the recorded arguments,  *)
(* and only when the operation can be that action (OpActs): a step that    *)
(* did one thing but landed in a state another action allows is rejected.  *)
(* The step must be a Next step too, so Next's ranges and guards around an *)
(* action (o \in Honest, d \in BOOLEAN) hold for the recorded arguments.   *)
(* A state that names fewer or other fields than VarsRec never matches,    *)
(* and a record that is not [op, act, args, state] fails the ASSUME: an    *)
(* under-logged harness fails red, never green.                            *)
(*                                                                         *)
(* A copy changes only EXTENDS, VarsRec, OpActs, Act and the row's reach   *)
(* predicates; tools/tla_trace_check.py compares the rest with this text   *)
(* and checks the shapes of those.                                         *)
(*                                                                         *)
(* When no step matches, TraceReject sets rejected, so TraceConsistent     *)
(* fails; the behaviour TLC prints ends at ti, the last record that        *)
(* matched, and record ti + 1 is the first code step that is not a model   *)
(* step.  At the last record the trace stutters (TraceDone).  Nothing here *)
(* quantifies over the log in a state formula, and nothing reads TraceLog  *)
(* but Trace (below).                                                      *)
(***************************************************************************)
EXTENDS RNOrderingWrap, Sequences

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

\* every model variable the code holds, by the field name the harness writes
VarsRec == [read |-> read, held |-> held, deliveries |-> deliveries, exhausted |-> exhausted, uDelivered |-> uDelivered, seqHold |-> seqHold]

\* the actions each operation of the code can be: the walk's input names the
\* kind of record (a fresh record, a resent number, a block fragment that
\* completes its block or not), the call's return says whether it deferred
OpActs == [receive_ordered |-> {"Receive"}, receive_completing_fragment |-> {"Receive"}, receive_fragment |-> {"ReceivePartial"}, receive_duplicate |-> {"ReceiveDuplicate"}, receive_unreliable |-> {"ReceiveUnreliable"}, receive_sequenced |-> {"HoldSequenced"}, end_update |-> {"EndUpdate"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Receive" -> Receive(x[1], x[2])
               [] a = "ReceiveDuplicate" -> ReceiveDuplicate(x[1], x[2])
               [] a = "ReceivePartial" -> ReceivePartial(x[1])
               [] a = "ReceiveUnreliable" -> ReceiveUnreliable
               [] a = "HoldSequenced" -> HoldSequenced(x[1])
               [] a = "EndUpdate" -> EndUpdate

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

\* the row's reach predicates: some clean trace must stand past the wire's
\* wrap (the read index at or past 2^24, its low 24 bits starting over);
\* hold the successor of the read index complete, so an in-turn record
\* releases a run (a duplicate or a fragment at the read index meets one
\* there); hold an ordering at the honest edge, W - 1 ahead; fill the
\* delivery table, so a fresh record, a fragment and a duplicate defer;
\* hold a code-1 and a code-4 future record; and hold a code-1 record with
\* the update's unreliable allowance spent, so the reservation counts no
\* slot for it
Wrapped == read >= WireMod
RunHeld == U24(read + 1) \in held
HeldAtEdge == U24(read + W - 1) \in held
TableFull == deliveries = Cap
HoldUnreliable == seqHold = "u"
HoldReliable == seqHold = "r"
HoldUnreliableDuSpent == seqHold = "u" /\ uDelivered = Du

=============================================================================
