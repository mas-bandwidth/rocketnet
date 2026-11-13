-------------------- MODULE TraceRNSourceSnapshotLayout --------------------
(***************************************************************************)
(* The trace check of RNSourceSnapshotLayout (layer source-snapshot), a    *)
(* copy of TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is    *)
(* the record sequence tests/tla_trace_source_snapshot.cpp writes over     *)
(* sdk/rocketnet_source_snapshot.h: one writer and two readers, one code   *)
(* call per record, with the call (op), its arguments and what it          *)
(* returned (args: the scan's skip, the loaded word, the re-check's        *)
(* result, the lookup's address and answer), the action the call's result  *)
(* names (act: Add or Refuse by the add's result) and every model          *)
(* variable after it (state).  Record 1 is "init".                         *)
(*                                                                         *)
(* A copy changes only EXTENDS, VarsRec, OpActs, Act and the row's reach   *)
(* predicates; tools/tla_trace_check.py compares the rest with the         *)
(* template.                                                               *)
(***************************************************************************)
EXTENDS RNSourceSnapshotLayout, Sequences

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
VarsRec == [slots |-> slots, pub |-> pub, ann |-> ann, wpc |-> wpc, wi |-> wi,
            wslot |-> wslot, wset |-> wset, pset |-> pset, skipped |-> skipped,
            refused |-> refused, rpc |-> rpc, rl |-> rl, held |-> held, look |-> look]

\* the actions each operation of the code can be
OpActs == [writer_scan |-> {"Scan"}, writer_fill |-> {"Fill"}, writer_add |-> {"Add", "Refuse"},
           writer_publish |-> {"Publish"}, reader_load |-> {"Load"}, reader_announce |-> {"Announce"},
           reader_recheck |-> {"Recheck"}, contains |-> {"Lookup"}, reader_release |-> {"Release"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Scan" -> Scan(x[1])
               [] a = "Fill" -> Fill
               [] a = "Add" -> Add(x[1])
               [] a = "Refuse" -> Refuse(x[1])
               [] a = "Publish" -> Publish
               [] a = "Load" -> Load(x[1], x[2])
               [] a = "Announce" -> Announce(x[1])
               [] a = "Recheck" -> Recheck(x[1], x[2])
               [] a = "Lookup" -> Lookup(x[1], x[2], x[3])
               [] a = "Release" -> Release(x[1])

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

\* the row's reach predicates, the layout's edges: a probe that wrapped (an
\* address in a cell before its home), and a lookup that found an address
\* whose probe wrapped
Wrapped == \E s \in Slots, i \in Cells : slots[s][i] # 0 /\ i < Home(slots[s][i])
FoundWrapped == \E r \in Readers : rpc[r] = "held" /\ look[r].found /\ \A i \in Home(look[r].addr)..(Cap - 1) : slots[ann[r]][i] # look[r].addr

=============================================================================
