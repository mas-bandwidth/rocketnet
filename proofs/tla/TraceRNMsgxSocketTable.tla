------------------------ MODULE TraceRNMsgxSocketTable ------------------------
(***************************************************************************)
(* The trace check of RNMsgxSocketTable (README-TraceCheck.md): the        *)
(* records tests/tla_trace_msgx.cpp's seeded random walk writes over the   *)
(* real sdk/rocketnet_msgx_socket_table.h, with a fake kernel and fake     *)
(* holders, are a behaviour of the spec.  TraceLog is that record          *)
(* sequence, one record of every model variable after each call and        *)
(* nothing else: a record with a field more or less never equals VarsRec, *)
(* so an under-logged harness fails red, never green.                      *)
(*                                                                         *)
(* TraceOps is the call the walk made before each record (the first is     *)
(* "init"): the walk's own choice, logged like any environment choice.     *)
(* One code call can be several model steps: forget is the retire's two    *)
(* steps and the caller's deregistration, and the sweep is one step per    *)
(* slot of its dead-report loop and per entry of its close loop, so the    *)
(* model lets holders interleave with them.  The walk is one thread and    *)
(* logs between calls, where main is idle.  So a step from an idle state   *)
(* is the action the call names; while main is inside a call only main's   *)
(* next step (MainCont, deterministic) is taken, and the state main        *)
(* returns to idle in must equal the record.  The call pins every choice,  *)
(* so no branch dead-ends on a choice the record does not show, and        *)
(* TraceReject fires only when the code's call is no model step.           *)
(* TraceInit matches the first record against Init; at the last record the *)
(* trace stutters (TraceDone).                                             *)
(***************************************************************************)
EXTENDS RNMsgxSocketTable, Sequences

CONSTANTS TraceLog, TraceOps

\* the trace and the calls as values: TLC evaluates a constant definition
\* once, where a substituted constant (TraceLog <- Log) is evaluated again
\* at each use
Trace == TraceLog
Ops == TraceOps

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

\* the index and the cooldown table as the harness writes them: only the
\* entries in use (the code's index has 2048 entries, the cooldown table 64)
IndexRec == [e \in {x \in Entries : index[x] >= 0} |-> index[e]]
CoolRec == [e \in {x \in 0..(CoolEntries - 1) : coolAddr[x] # 0 \/ coolUntil[x] # 0} |->
               <<coolAddr[e], coolUntil[e]>>]

\* every model variable by the field name the harness writes
VarsRec ==
    [word |-> word, deadRep |-> deadRep, state |-> state, fd |-> fd, gen |-> gen, addr |-> addr,
     snap |-> snap, pass |-> pass, deadReports |-> deadReports, freeSlots |-> freeSlots,
     numFree |-> numFree, retired |-> retired, numRetired |-> numRetired, numOpen |-> numOpen,
     index |-> IndexRec, cool |-> CoolRec, counters |-> counters, update |-> update,
     opensLeft |-> opensLeft, mpc |-> mpc, ms |-> ms, mi |-> mi, mk |-> mk, kfd |-> kfd,
     kdead |-> kdead, poller |-> poller, pkts |-> pkts, cur |-> cur, rep |-> rep, rxEv |-> rxEv,
     rxFd |-> rxFd, crossed |-> crossed, onClosed |-> onClosed, attempts |-> attempts]

\* the model action a code call starts with
Do(op) ==
    CASE op.n = "mark" -> Mark(op.a)
      [] op.n = "forget" -> Forget(op.a)
      [] op.n = "update" -> BeginUpdate
      [] op.n = "sweep" -> SweepBegin
      [] op.n = "stamp" -> Stamp(op.a, op.h)
      [] op.n = "begin" -> HolderBegin(op.h)
      [] op.n = "end" -> HolderEnd(op.h)
      [] op.n = "report" -> ReportDead(op.h)
      [] op.n = "resolve" -> TxResolve(op.h, op.i)
      [] op.n = "send" -> TxSend(op.h)
      [] op.n = "poll" -> \E e \in poller : RxPoll(e)
      [] op.n = "rxresolve" -> RxResolve
      [] op.n = "read" -> RxRead
      [] op.n = "envopen" -> EnvOpen
      [] op.n = "envclose" -> EnvClose(op.f)
      [] op.n = "disconnect" -> Disconnect(op.f)

\* a record is the model's variables exactly (whole-record equality), and
\* there is one call per record
TraceInit ==
    /\ Init
    /\ ti = 1
    /\ rejected = (Len(Ops) # Len(Trace) \/ VarsRec # Trace[1])

TraceStep ==
    /\ ~rejected
    /\ ti < Len(Trace)
    /\ IF Idle THEN Do(Ops[ti + 1]) ELSE MainCont
    /\ IF mpc' = "idle"
          THEN /\ VarsRec' = Trace[ti + 1]
               /\ ti' = ti + 1
          ELSE ti' = ti
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

\* every record of the trace is a model step (or a call's steps) from the
\* one before
TraceConsistent == ~rejected

=============================================================================
