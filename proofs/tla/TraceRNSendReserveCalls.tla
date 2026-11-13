---------------------- MODULE TraceRNSendReserveCalls ----------------------
(***************************************************************************)
(* The trace check of RNSendReserveCalls, a copy of the template           *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_send_reserve.cpp writes: one per step, with    *)
(* the operation the walk called (op), the walk's arguments to it (args),  *)
(* the model action the code's own result says it took (act), and every    *)
(* model variable after it (state).  Record 1 is "init".                   *)
(*                                                                         *)
(* TraceStep allows only the recorded action with the recorded arguments,  *)
(* and only when the operation can be that action (OpActs): a step that    *)
(* did one thing but landed in a state another action allows is rejected.  *)
(* The step must be a Next step too, so Next's ranges and guards around an *)
(* action (\E c \in Clients) hold for the recorded arguments.  A state     *)
(* that names fewer or other fields than VarsRec never matches, and a      *)
(* record that is not [op, act, args, state] fails the ASSUME: an          *)
(* under-logged harness fails red, never green.                            *)
(*                                                                         *)
(* A copy changes only EXTENDS, VarsRec, OpActs, Act and the row's reach   *)
(* predicates; tools/tla_trace_check.py compares the rest with this text   *)
(* and checks the shapes of those four.                                    *)
(*                                                                         *)
(* When no step matches, TraceReject sets rejected, so TraceConsistent     *)
(* fails; the behaviour TLC prints ends at ti, the last record that        *)
(* matched, and record ti + 1 is the first code step that is not a model   *)
(* step.  At the last record the trace stutters (TraceDone).  Nothing here *)
(* quantifies over the log in a state formula, and nothing reads TraceLog  *)
(* but Trace (below).                                                      *)
(***************************************************************************)
EXTENDS RNSendReserveCalls, Sequences

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
VarsRec == [free |-> free, budget |-> budget, claims |-> claims, started |-> started, ownUsed |-> ownUsed, own |-> own, ownLeft |-> ownLeft, refused |-> refused]

\* the actions each operation of the code can be
OpActs == [open |-> {"Open"}, start |-> {"StartStaged", "StartIoRefused", "StartRefused"}, finish |-> {"Finish"}, abort |-> {"Abort"}, capacity |-> {"Capacity"}, retire |-> {"Retire"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Open" -> Open(x[1], x[2])
               [] a = "StartStaged" -> StartStaged(x[1])
               [] a = "StartIoRefused" -> StartIoRefused(x[1])
               [] a = "StartRefused" -> StartRefused(x[1])
               [] a = "Finish" -> Finish
               [] a = "Abort" -> Abort
               [] a = "Capacity" -> Capacity(x[1])
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

\* the row's reach predicates: some clean trace must stand exactly on the
\* reserve after a grant (the edge where a start turns to a refusal), count
\* a refusal, run the room out, and use every own frame handed out
AtReserve == started > 0 /\ free + ownUsed = Reserve
Counted == refused > 0
RoomOut == free = 0
OwnAllUsed == ownLeft = 0 /\ ownUsed = Cardinality(Clients)

=============================================================================
