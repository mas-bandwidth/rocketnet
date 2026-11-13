---------------------- MODULE TraceRNGsoQueueBankLayout ----------------------
(***************************************************************************)
(* The trace check of RNGsoQueueBankLayout (README-TraceCheck.md), a copy  *)
(* of the template TraceRNTraceExample.tla.  TraceLog is the record        *)
(* sequence tests/tla_trace_gso.cpp writes over                            *)
(* sdk/rocketnet_server_gso_linux.cpp: one record per step, with the       *)
(* operation the walk called (op), its arguments and what the call        *)
(* returned (args), the model action the call's result says it took (act) *)
(* and every model variable after it (state).  Record 1 is "init".         *)
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
EXTENDS RNGsoQueueBankLayout, Sequences

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
VarsRec == [prod |-> prod, own |-> own, cnt |-> cnt, turn |-> turn, load |-> load,
            held |-> held, owner |-> owner, demand |-> demand, flushes |-> flushes,
            oob |-> oob, shared |-> shared, foreign |-> foreign, routeBad |-> routeBad,
            workerWrite |-> workerWrite]

\* the actions each operation of the code can be: start_packet's claim, and
\* the reload of a compare-and-swap that lost (no call: the walk reads the
\* counter again, as compare_exchange_weak does inside the one call)
OpActs == [load |-> {"Load"}, start_packet |-> {"Claim"}, reload |-> {"Claim"},
           finish_packet |-> {"Finish"}, finish_abort |-> {"Finish"},
           send_packets |-> {"Flush"}, take_return_send |-> {"WorkerReturn"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Load" -> Load(x[1], x[2], x[3])
               [] a = "Claim" -> Claim(x[1], x[2])
               [] a = "Finish" -> Finish(x[1], x[2])
               [] a = "Flush" -> Flush
               [] a = "WorkerReturn" -> WorkerReturn(x[1], x[2])

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

\* the row's reach predicates, the layout's edges a clean trace must show:
\* a producer bank with every slot claimed (the next start is refused)
BankFull == \E q \in 0..(Q - 1) : cnt[q][prod[q]] = S

\* a queue turned over at least once per bank, so a bank its worker returned
\* is a producer again
TurnedOverEveryBank == \E q \in 0..(Q - 1) : turn[q] >= NB

\* a queue with no free bank (the next flush keeps its producer)
NoFreeBank == \E q \in 0..(Q - 1) : \A b \in 0..(NB - 1) : own[q][b] # "free"

=============================================================================
