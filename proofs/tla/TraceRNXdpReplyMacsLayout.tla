---------------------- MODULE TraceRNXdpReplyMacsLayout ----------------------
(***************************************************************************)
(* The trace check of RNXdpReplyMacsLayout (README-TraceCheck.md).         *)
(* TraceLog is the record sequence tests/tla_trace_xdp_reply_macs.cpp      *)
(* writes from the code of sdk/rocketnet_xdp_frame.h: one record per call, *)
(* with the operation the walk called (op), the model action the code's    *)
(* own result says it took (act), the walk's arguments to it in the        *)
(* action's parameter order (args), and every model variable after it      *)
(* (state).  Record 1 is "init".                                           *)
(*                                                                         *)
(* The state is the code's: home is rocketnet_xdp_reply_macs_slot of each  *)
(* address, st, addr, mac and stamp the entries' fields, count learn_count,*)
(* and abs the code's own lookups (find_state LIVE, else ANSWERED), so the *)
(* contract the model updates is checked against what the code's lookups   *)
(* return.  An EMPTY slot's MAC is the zero bytes the reset's memset       *)
(* leaves; the model leaves it free (its VIEW ignores it), and the walk    *)
(* writes it as EmptyMac, the model's own choice, only when the bytes are  *)
(* zero.                                                                   *)
(*                                                                         *)
(* A learn of an unusable MAC (Ignored) and a reply that returns a MAC the *)
(* table had or none (Skip, Refused) are calls that change no model        *)
(* variable; they are traced, so a change they made is caught at their own *)
(* record.                                                                 *)
(*                                                                         *)
(* TraceStep allows only the recorded action, and only when the operation  *)
(* can be that action (OpActs), into a state that keeps the model's        *)
(* invariants (TraceInvariant).  A record whose state is <<>> is a step    *)
(* inside one call the walk cannot observe (Matches); this layer writes    *)
(* none.  When no step matches, TraceReject sets rejected and              *)
(* TraceConsistent fails; record ti + 1 is the first code step that is not *)
(* the model step it names.                                                *)
(***************************************************************************)
EXTENDS RNXdpReplyMacsLayout, Sequences

CONSTANT TraceLog

\* the log through a definition of this module's own: TLC evaluates a
\* constant definition once, when it starts, but the constant TraceLog,
\* overridden by the trace module's Log, again at every reference. Every use
\* below reads Records; a 41-record log of this layer took 6 minutes
\* through TraceLog and 1 s through Records.
Records == TraceLog

\* checked once, when TLC starts
ASSUME /\ Len(Records) >= 2
       /\ \A n \in DOMAIN Records : DOMAIN Records[n] = {"op", "act", "args", "state"}

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

\* the MAC the model gives a never-used slot (Init, Reset)
EmptyMac == CHOOSE m \in Macs : TRUE

\* every model variable by the field name the harness writes
VarsRec == [home |-> home, st |-> st, addr |-> addr, mac |-> mac, stamp |-> stamp,
            count |-> count, abs |-> abs, badEviction |-> badEviction]

\* the actions each operation of the code can be
\* (a learn of an unusable MAC is ignored, and a reply that returns a
\* learned MAC, an ANSWERED entry's MAC or none changes nothing: Unchanged)
OpActs == [learn |-> {"Learn", "Ignored"}, reply |-> {"Answer", "Skip", "Refused"},
           forget |-> {"Forget"}, reset |-> {"Reset"}]

\* a call that changes no model variable
Unchanged == UNCHANGED vars

\* the action a record names, with the walk's arguments; a name this CASE
\* lacks stops TLC with an error. A reply's third argument is how many
\* refusals the call counted: one exactly when it returned no MAC
Act(a, x) == CASE a = "Learn" -> Learn(x[1], x[2])
               [] a = "Ignored" -> Unchanged
               [] a = "Answer" -> Answer(x[1], x[2]) /\ x[3] = 0
               [] a = "Skip" -> Unchanged /\ x[3] = 0
               [] a = "Refused" -> Unchanged /\ x[3] = 1
               [] a = "Forget" -> Forget(x[1])
               [] a = "Reset" -> Reset

\* the invariants MCRNXdpReplyMacsLayout.cfg checks, on every state the code
\* reaches
TraceInvariant == TypeOK /\ TableIsMap /\ OneEntryPerAddress /\ NoEntryPastEmpty /\ EvictOnlyOldest

\* record 1's homes when they are slots; anything else is a mismatch for
\* rejected below, never an empty set of initial states
FirstHome == IF Records[1].state.home \in [Addrs -> Slots]
             THEN Records[1].state.home
             ELSE [a \in Addrs |-> 0]

\* the record's state, or a step the walk could not observe
Matches(r) == DOMAIN r.state = {} \/ VarsRec' = r.state

\* home is the code's hash of each address: taken from record 1 before Init
\* checks it, so TLC never enumerates every map of addresses to slots
TraceInit ==
    /\ home = FirstHome
    /\ Init
    /\ ti = 1
    /\ rejected = (Records[1].op # "init" \/ Records[1].act # "init" \/ VarsRec # Records[1].state)

TraceStep ==
    /\ ~rejected
    /\ ti < Len(Records)
    /\ LET r == Records[ti + 1]
       IN /\ r.op \in DOMAIN OpActs
          /\ r.act \in OpActs[r.op]
          /\ Act(r.act, r.args)
          /\ Matches(r)
          /\ TraceInvariant'
    /\ ti' = ti + 1
    /\ UNCHANGED rejected

TraceReject ==
    /\ ~rejected
    /\ ti < Len(Records)
    /\ ~ENABLED TraceStep
    /\ rejected' = TRUE
    /\ UNCHANGED <<vars, ti>>

TraceDone ==
    /\ rejected \/ ti = Len(Records)
    /\ UNCHANGED <<vars, ti, rejected>>

TraceSpec == TraceInit /\ [][TraceStep \/ TraceReject \/ TraceDone]_<<vars, ti, rejected>>

\* every record of the trace is the model step it names, from the one before
TraceConsistent == ~rejected

=============================================================================
