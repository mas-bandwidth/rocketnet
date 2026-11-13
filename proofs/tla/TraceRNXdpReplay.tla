-------------------------- MODULE TraceRNXdpReplay --------------------------
(***************************************************************************)
(* The trace check of RNXdpReplay (README-TraceCheck.md).  TraceLog is the *)
(* record sequence tests/tla_trace_xdp_replay.cpp writes over the code of  *)
(* kernel replay protection: the shipped XDP filter (sdk/xdp/server_xdp.c, *)
(* built as host C), the transport's walk                                  *)
(* (rocketnet_server_replay_marks_walk), the socket's sink and commit      *)
(* (sdk/rocketnet_xdp_replay_sink.h), the replay windows and the reply MAC *)
(* table.  One record per step, [op, act, args, state]: the operation the  *)
(* walk called, the model action the code's result says it took, the      *)
(* action's parameters and every model variable after it.  Record 1 is     *)
(* "init".                                                                 *)
(*                                                                         *)
(* The operations: connect, send, forge, duplicate, kernel, open, release, *)
(* walk, commit, answer, hold_session, hold_stray and drain, one action    *)
(* each (OpActs).  Two calls bind their result through args as well as     *)
(* the state: the filter's verdict (kernel: "drop" exactly when the model  *)
(* drops), the reply MAC with no frame in hand (drain: sent exactly when   *)
(* NoFrameMacKnown) and the direct response's MAC (answer: the learned MAC *)
(* exactly when one is learned).                                           *)
(*                                                                         *)
(* The code drops a queued forget's learned reply MAC in the walk (the     *)
(* sink's forget), the model in the commit after it (pendingMacForget).    *)
(* No reply is sent between the two (every reply action needs phase        *)
(* "run"), so VarsRec maps the model's state to the code's: replyMac and   *)
(* answered as they are once the pending MAC forgets are applied.          *)
(*                                                                         *)
(* TraceStep allows only the recorded action, and only when the operation  *)
(* can be that action, into a state that keeps every invariant the         *)
(* model's configurations check (TraceInvariant), so a code state that     *)
(* breaks one is a rejection too.  When no step matches, TraceReject sets  *)
(* rejected, so TraceConsistent fails; record ti + 1 is the first code     *)
(* step that is not a model step.  The log is read through Records, a      *)
(* definition, which TLC evaluates once; TraceLog, an overridden constant, *)
(* it rebuilds at every reference.                                         *)
(***************************************************************************)
EXTENDS RNXdpReplay, Sequences

CONSTANT TraceLog

Records == TraceLog

\* checked once, when TLC starts
ASSUME /\ Len(Records) >= 2
       /\ \A n \in DOMAIN Records : DOMAIN Records[n] = {"op", "act", "args", "state"}

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

\* the reply MAC table as the code holds it: the pending MAC forgets applied
CodeReplyMac == [a \in Addresses |-> IF a \in pendingMacForget THEN NoSession ELSE replyMac[a]]
CodeAnswered == [a \in Addresses |-> IF a \in pendingMacForget /\ replyMac[a] # NoSession
                                       THEN ForgetLeavesAnswered ELSE answered[a]]

\* every model variable by the field name the harness writes
VarsRec == [session |-> session, nextSession |-> nextSession, sent |-> sent, opened |-> opened,
            reached |-> reached, marked |-> marked, seen |-> seen,
            seenBeforeRelease |-> seenBeforeRelease, replyMac |-> CodeReplyMac, kmap |-> kmap,
            net |-> net, pendingForget |-> pendingForget, pendingMark |-> pendingMark,
            pendingMacForget |-> pendingMacForget, phase |-> phase, dropped |-> dropped,
            forgetFailures |-> forgetFailures, greeted |-> greeted, markFailures |-> markFailures,
            markOwed |-> markOwed, answered |-> CodeAnswered, heard |-> heard, held |-> held,
            heldReplies |-> heldReplies, sentTo |-> sentTo, refusedHeld |-> refusedHeld]

\* every invariant the model's configurations check
TraceInvariant ==
    /\ TypeOK
    /\ ForgetOwedUntilApplied
    /\ MarkNeverAhead
    /\ MarksTakenFromTheTruth
    /\ FreshGenuineNeverDropped
    /\ NewSessionNeverDroppedByOldMark
    /\ ReleasedSlotForgetsItsReplyMac
    /\ RetryNeverDeletesNewerMark
    /\ MarkOwedUntilApplied
    /\ NeverSeenNeverSentTo
    /\ HeldReplyFindsItsMac
    /\ AnsweredOnlyHeard

\* the actions each operation of the code can be
OpActs == [connect |-> {"Connect"},
           send |-> {"ClientSend"},
           forge |-> {"Forge"},
           duplicate |-> {"Duplicate"},
           kernel |-> {"KernelFilter"},
           open |-> {"UserOpen"},
           release |-> {"Release"},
           walk |-> {"Walk"},
           commit |-> {"Commit"},
           answer |-> {"Answer"},
           hold_session |-> {"HoldSessionReply"},
           hold_stray |-> {"HoldStray"},
           drain |-> {"Drain"}]

\* the filter's verdict for a packet against the map
Verdict(p) == IF kmap[p.addr].hw # 0 /\ kmap[p.addr].hw - p.seq >= KernelWindow
                THEN "drop" ELSE "pass"

\* the action a record names, at its parameters; a name this CASE lacks
\* stops TLC with an error
Act(a, x) == CASE a = "Connect" -> Connect(x[1])
               [] a = "ClientSend" -> ClientSend(x[1])
               [] a = "Forge" -> x[2] \in Seqs /\ Forge(x[1], x[2])
               [] a = "Duplicate" -> x[1] \in net /\ Duplicate(x[1])
               [] a = "KernelFilter" -> /\ x[1] \in net
                                        /\ x[2] = Verdict(x[1])
                                        /\ KernelFilter(x[1])
               [] a = "UserOpen" -> x[1] \in net /\ UserOpen(x[1])
               [] a = "Release" -> Release(x[1])
               [] a = "Walk" -> Walk
               [] a = "Commit" -> Commit
               [] a = "Answer" -> x[2] = (replyMac[x[1]] # NoSession) /\ Answer(x[1])
               [] a = "HoldSessionReply" -> HoldSessionReply(x[1])
               [] a = "HoldStray" -> HoldStray(x[1])
               [] a = "Drain" -> x[2] = NoFrameMacKnown(x[1]) /\ Drain(x[1])

TraceInit ==
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
          /\ VarsRec' = r.state
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
