-------------------------- MODULE TraceRNXdpReplay --------------------------
(***************************************************************************)
(* The trace check of RNXdpReplay (README-TraceCheck.md, the template      *)
(* TraceRNTraceExample.tla).  TraceLog is the record sequence              *)
(* tests/tla_trace_xdp_replay.cpp writes over the code of kernel replay    *)
(* protection: the shipped XDP filter (sdk/xdp/server_xdp.c, built as host *)
(* C at a replay window of 2), the transport's walk                        *)
(* (rocketnet_server_replay_marks_walk), the socket's sink and commit      *)
(* (sdk/rocketnet_xdp_replay_sink.h), the replay windows and the reply MAC *)
(* table.  One record per step, [op, act, args, state]: the operation the  *)
(* walk called, the model action the code's result says it took, the      *)
(* action's parameters and every model variable after it.  Record 1 is     *)
(* "init".                                                                 *)
(*                                                                         *)
(* The operations: connect, send, forge, duplicate, kernel, open, release, *)
(* walk, commit, answer, hold_session, hold_stray and drain, one action    *)
(* each (OpActs).  The filter's verdict, the direct response's MAC and the *)
(* drained reply's MAC show in the state: a dropped packet leaves net (and *)
(* a genuine one enters dropped), a passed one moves to "user"; answered;  *)
(* sentTo or refusedHeld.                                                  *)
(*                                                                         *)
(* TraceStep allows only the recorded action with the recorded arguments,  *)
(* and only when the operation can be that action (OpActs), as a Next      *)
(* step.  When no step matches, TraceReject sets rejected, so              *)
(* TraceConsistent fails; record ti + 1 is the first code step that is not *)
(* a model step.  The rest is the template's text.                         *)
(***************************************************************************)
EXTENDS RNXdpReplay, Sequences

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
VarsRec == [session |-> session, nextSession |-> nextSession, sent |-> sent, opened |-> opened,
            reached |-> reached, marked |-> marked, seen |-> seen,
            seenBeforeRelease |-> seenBeforeRelease, replyMac |-> replyMac, kmap |-> kmap,
            net |-> net, pendingForget |-> pendingForget, pendingMark |-> pendingMark,
            pendingMacForget |-> pendingMacForget, phase |-> phase, dropped |-> dropped,
            forgetFailures |-> forgetFailures, greeted |-> greeted, markFailures |-> markFailures,
            markOwed |-> markOwed, answered |-> answered, heard |-> heard, held |-> held,
            heldReplies |-> heldReplies, sentTo |-> sentTo, refusedHeld |-> refusedHeld]

\* the actions each operation of the code can be
OpActs == [connect |-> {"Connect"}, send |-> {"ClientSend"}, forge |-> {"Forge"}, duplicate |-> {"Duplicate"}, kernel |-> {"KernelFilter"}, open |-> {"UserOpen"}, release |-> {"Release"}, walk |-> {"Walk"}, commit |-> {"Commit"}, answer |-> {"Answer"}, hold_session |-> {"HoldSessionReply"}, hold_stray |-> {"HoldStray"}, drain |-> {"Drain"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Connect" -> Connect(x[1])
               [] a = "ClientSend" -> ClientSend(x[1])
               [] a = "Forge" -> Forge(x[1], x[2])
               [] a = "Duplicate" -> Duplicate(x[1])
               [] a = "KernelFilter" -> KernelFilter(x[1])
               [] a = "UserOpen" -> UserOpen(x[1])
               [] a = "Release" -> Release(x[1])
               [] a = "Walk" -> Walk
               [] a = "Commit" -> Commit
               [] a = "Answer" -> Answer(x[1])
               [] a = "HoldSessionReply" -> HoldSessionReply(x[1])
               [] a = "HoldStray" -> HoldStray(x[1])
               [] a = "Drain" -> Drain(x[1])

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

\* the row's reach predicates: the kernel map holds a mark (what the filter's drop rule reads), the kernel
\* refused a forget, and a live session's greeting was held back (a session
\* whose first keep-alive has not left while its mark is the zero mark)
KernelMarked == \E a \in Addresses : kmap[a] # NoEntry
ForgetRefused == forgetFailures > 0
GreetingHeld == \E a \in Addresses : session[a] # NoSession /\ ~greeted[a] /\ marked[a] = 0 /\ seen[a]

=============================================================================
