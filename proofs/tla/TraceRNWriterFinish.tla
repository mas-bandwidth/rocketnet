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
VarsRec == [phase |-> phase, owner |-> owner, touched |-> touched, open |-> open, build |-> build, queued |-> queued, held |-> held, cap |-> cap, starts |-> starts, sent |-> sent, refused |-> refused, cancelled |-> cancelled, lost |-> lost, walked |-> walked]

\* the actions each operation of the code can be
OpActs == [update |-> {"BeginFill"}, start |-> {"StartSmall", "StartLarge", "StartReliable"}, finish |-> {"Finish", "FinishOver", "FinishRestart", "FinishBadBytes", "FinishLost"}, abort |-> {"Abort"}, send_packets |-> {"SendPackets"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "BeginFill" -> BeginFill
               [] a = "StartSmall" -> StartSmall(x[1], x[2])
               [] a = "StartLarge" -> StartLarge(x[1], x[2])
               [] a = "StartReliable" -> StartReliable(x[1], x[2])
               [] a = "Finish" -> Finish(x[1], x[2])
               [] a = "FinishOver" -> FinishOver(x[1], x[2])
               [] a = "FinishRestart" -> FinishRestart(x[1], x[2])
               [] a = "FinishBadBytes" -> FinishBadBytes(x[1], x[2])
               [] a = "FinishLost" -> FinishLost(x[1], x[2])
               [] a = "Abort" -> Abort(x[1], x[2])
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

\* the row's reach predicates. On a staged transport (the walk's odd
\* seeds): a writer's finish that seals and hands off its packet and one
\* refused for want of capacity; a large start and a reliable one held open,
\* and messages they left on the reliable path; a send_packets that took
\* those with no unreliable message queued; and, with a send budget (seed 3
\* modulo 4), a start the bucket paid only a frame of its own for
SealedAtFinish == sent > 0
RefusedAtFinish == refused > 0
LargeOpen == \E c \in Clients : open[c] = "large"
ReliableOpen == \E c \in Clients : open[c] = "reliable"
HeldForWalk == \E c \in Clients : held[c] > 0
WalkTookHeld == phase = "main" /\ sent > 0 /\ refused > 0 /\ walked > 0 /\ cancelled > 0
WholeOpen == \E c \in Clients : open[c] = "whole"

\* and, on a buffered transport (the walk's even seeds): a start the ring had
\* no slot for held open, its message queued by the writer's finish, a start
\* held behind it, and a send_packets that carried such messages with none
\* refused
RingOpen == \E c \in Clients : open[c] = "ring"
RingQueued == \E c \in Clients : queued[c] > 0
BehindOpen == \E c \in Clients : open[c] = "behind"
WalkCarried == phase = "main" /\ sent > 0 /\ walked > 0 /\ refused = 0

=============================================================================
