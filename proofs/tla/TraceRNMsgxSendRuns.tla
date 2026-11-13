------------------------ MODULE TraceRNMsgxSendRuns ------------------------
(***************************************************************************)
(* The trace check of RNMsgxSendRuns (README-TraceCheck.md): the records   *)
(* tests/tla_trace_msgx_send_runs.cpp's seeded random walk writes over the *)
(* real sdk/rocketnet_msgx_send_runs.h, with a fake table and fake         *)
(* connected and shared sends, are a behaviour of the spec.  TraceLog is   *)
(* that record sequence, one record of every model variable after each     *)
(* model step and nothing else: a record with a field more or less never   *)
(* equals VarsRec, so an under-logged harness fails red, never green.      *)
(*                                                                         *)
(* Each record after the first is one step: the sort, one call of          *)
(* rocketnet_msgx_send_runs_step, the worker's check after a kept call     *)
(* (give up, or resume), or the environment's Quit or Retire.  The record  *)
(* carries the fakes' answers in what they did (the packets that left and  *)
(* how), so every branch the record does not take is rejected at it.       *)
(*                                                                         *)
(* TraceInit takes the bank from record 1 and checks it against Init's    *)
(* rules (BankOK, the claim, the packet domain), the rest of Init fixed,  *)
(* so a bank the fill threads cannot stage is rejected; Init's own        *)
(* enumeration of every bank is not needed for one trace.  At the last    *)
(* record the trace stutters (TraceDone).                                  *)
(***************************************************************************)
EXTENDS RNMsgxSendRuns, Sequences

CONSTANTS TraceLog

\* the trace as a value: TLC evaluates a constant definition once, where a
\* substituted constant (TraceLog <- Log) is evaluated again at each use
Trace == TraceLog

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

\* every model variable by the field name the harness writes
VarsRec ==
    [count |-> count, client |-> client, slot |-> slot, gen |-> gen, phase |-> phase, live |-> live,
     tOpen |-> tOpen, tGen |-> tGen, order |-> order, pc |-> pc, cursor |-> cursor, hd |-> hd,
     scan |-> scan, run |-> run, attempts |-> attempts, left |-> left, lastC |-> lastC, lastS |-> lastS,
     bad |-> bad, quit |-> quit, retries |-> retries, stalls |-> stalls, retires |-> retires]

\* Init's packet domain over a bank already given: what
\* \E pk \in [0..(count - 1) -> Packets] admits
BankDomain ==
    /\ \A i \in Index :
        IF i < count
        THEN /\ client[i] \in Clients
             /\ << slot[i], gen[i] >> \in Stamps
             /\ phase[i] \in Phases
             /\ live[i] \in BOOLEAN
        ELSE /\ client[i] = 0
             /\ slot[i] = Shared
             /\ gen[i] = 0
             /\ phase[i] = 0
             /\ live[i] = FALSE

\* the bank of record 1 is one Init admits
InitBank ==
    /\ count \in {Min(c, Size) : c \in Claims}
    /\ DOMAIN client = Index /\ DOMAIN slot = Index /\ DOMAIN gen = Index
    /\ DOMAIN phase = Index /\ DOMAIN live = Index
    /\ BankDomain
    /\ BankOK

\* a bank Init does not admit is a rejected trace, never an empty one (no
\* initial state would pass green)
TraceInit ==
    /\ count = Trace[1].count
    /\ client = Trace[1].client
    /\ slot = Trace[1].slot
    /\ gen = Trace[1].gen
    /\ phase = Trace[1].phase
    /\ live = Trace[1].live
    /\ InitRest
    /\ ti = 1
    /\ rejected = (~InitBank \/ VarsRec # Trace[1])

TraceStep ==
    /\ ~rejected
    /\ ti < Len(Trace)
    /\ Next
    /\ VarsRec' = Trace[ti + 1]
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

\* every record of the trace is a model step from the one before
TraceConsistent == ~rejected

=============================================================================
