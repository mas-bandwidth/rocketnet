---------------------- MODULE TraceRNXdpReplyMacsLayout ----------------------
(***************************************************************************)
(* The trace check of RNXdpReplyMacsLayout, copied from the template       *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_xdp_reply_macs.cpp writes over the reply MAC   *)
(* table of sdk/rocketnet_xdp_frame.h: one record per call, with the call  *)
(* (op), its arguments and what it gave back (args), the model action its  *)
(* own result says it took (act), and every model variable after it        *)
(* (state).  The walk runs the model under HomesGiven: each address is     *)
(* named by the code's hash slot of it plus a multiple of N.  A reply that *)
(* returned the walk's own answered buffer is Answer; one that returned an *)
(* entry's MAC is ReplyLearned or ReplyAnswered by that entry's state, with *)
(* the MAC; one that returned NULL is Refuse.  abs is the code's own       *)
(* lookups of every address, so the contract the model keeps is checked   *)
(* against what the lookups return.  Everything but EXTENDS, VarsRec,      *)
(* OpActs, Act and the reach predicates is the template's text.            *)
(***************************************************************************)
EXTENDS RNXdpReplyMacsLayout, Sequences

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
    [home |-> home, st |-> st, addr |-> addr, mac |-> mac, stamp |-> stamp,
     count |-> count, abs |-> abs, badEviction |-> badEviction]

\* the actions each operation of the code can be
OpActs == [learn |-> {"Learn", "Ignore"}, reply |-> {"Answer", "ReplyLearned", "ReplyAnswered", "Refuse"},
           forget |-> {"Forget"}, reset |-> {"Reset"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Learn" -> Learn(x[1], x[2])
               [] a = "Ignore" -> Ignore(x[1])
               [] a = "Answer" -> Answer(x[1], x[2])
               [] a = "ReplyLearned" -> ReplyLearned(x[1], x[2])
               [] a = "ReplyAnswered" -> ReplyAnswered(x[1], x[2])
               [] a = "Refuse" -> Refuse(x[1], x[2])
               [] a = "Forget" -> Forget(x[1])
               [] a = "Reset" -> Reset

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

\* the row's reach predicates: some clean trace holds an entry past the
\* table's end from its home (the probe wrapped), and some clean trace has
\* an address whose whole window is LIVE (the edge where learn evicts)
Wrapped == \E x \in Slots : st[x] # EMPTY /\ x < home[addr[x]]

WindowLive == \E a \in Addrs : \A i \in 0..(P - 1) : st[Probe(a, i)] = LIVE

=============================================================================
