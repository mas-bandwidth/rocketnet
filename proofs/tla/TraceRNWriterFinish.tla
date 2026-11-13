------------------------ MODULE TraceRNWriterFinish ------------------------
(***************************************************************************)
(* The trace check of RNWriterFinish, a copy of the template               *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_writer_finish.cpp writes: one per step, with   *)
(* the operation the walk called (op), the walk's arguments to it (args),  *)
(* the model action the code's own result says it took (act), and every    *)
(* model variable after it (state).  Record 1 is "init".                   *)
(*                                                                         *)
(* TraceStep allows only the recorded action with the recorded arguments,  *)
(* and only when the operation can be that action (OpActs): a step that    *)
(* did one thing but landed in a state another action allows is rejected.  *)
(* The step must be a Next step too, so Next's ranges and guards around an *)
(* action (\E w \in Writers, c \in Clients) hold for the recorded          *)
(* arguments.  A state                                                     *)
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
EXTENDS RNWriterFinish, Sequences

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
VarsRec == [phase |-> phase, owner |-> owner, touched |-> touched, open |-> open, build |-> build, queued |-> queued, cap |-> cap, starts |-> starts, sent |-> sent, refused |-> refused, walked |-> walked]

\* the actions each operation of the code can be
OpActs == [update |-> {"BeginFill"}, start |-> {"Start"}, finish |-> {"Finish"}, send_packets |-> {"SendPackets"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "BeginFill" -> BeginFill
               [] a = "Start" -> Start(x[1], x[2])
               [] a = "Finish" -> Finish(x[1], x[2])
               [] a = "SendPackets" -> SendPackets

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

\* the row's reach predicates: some clean trace must see a writer's finish
\* seal and hand off its packet and another refused (a start that found the
\* update's capacity out), and stand after a send_packets that found nothing
\* of either to finish
SealedAtFinish == sent > 0
RefusedAtFinish == refused > 0
NothingForWalk == phase = "main" /\ sent > 0 /\ refused > 0 /\ walked = 0

\* and, on a buffered transport (the walk's even seeds): a start the ring had
\* no slot for held open, its message queued by the writer's finish, and a
\* send_packets that carried such messages with none refused
RingOpen == \E c \in Clients : open[c] = "ring"
RingQueued == \E c \in Clients : queued[c] > 0
WalkCarried == phase = "main" /\ sent > 0 /\ walked > 0 /\ refused = 0

=============================================================================
