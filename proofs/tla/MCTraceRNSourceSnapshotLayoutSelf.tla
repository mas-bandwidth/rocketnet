---------------- MODULE MCTraceRNSourceSnapshotLayoutSelf ----------------
(***************************************************************************)
(* The trace check's own controls for RNSourceSnapshotLayout, before any   *)
(* code writes a trace: a hand-written record of one update and one       *)
(* reader's batch (the writer scans the reader, fills slot 1, adds an      *)
(* address, publishes; the reader loads, announces, re-checks and looks    *)
(* the address up) must be accepted                                       *)
(* (MCTraceRNSourceSnapshotLayoutSelf.cfg), and the same record with the  *)
(* published word changed by the add, as the trace check's plant does,   *)
(* must be rejected at that record                                        *)
(* (MCTraceRNSourceSnapshotLayoutSelfBrokenAddPublishes.cfg,              *)
(* TraceConsistent).                                                      *)
(***************************************************************************)
EXTENDS TraceRNSourceSnapshotLayout, TLC

SelfHome(a) == CASE a = 1 -> 3 [] a = 2 -> 3 [] OTHER -> 0

E4 == [i \in 0..3 |-> 0]

S1 == [slots |-> [s \in 0..1 |-> E4], pub |-> [slot |-> 0, gen |-> 0], ann |-> (1 :> -1),
       wpc |-> "idle", wi |-> 0, wslot |-> 1, wset |-> {}, pset |-> {},
       skipped |-> 0, refused |-> 0, rpc |-> (1 :> "idle"),
       rl |-> (1 :> [slot |-> -1, gen |-> -1]), held |-> (1 :> {}),
       look |-> (1 :> [addr |-> 0, found |-> FALSE])]

\* Scan: reader 1 announces nothing
S2 == [S1 EXCEPT !.wpc = "scan", !.wi = 1]

\* Fill: slot 1 cleared (it was empty)
S3 == [S2 EXCEPT !.wpc = "add", !.wi = 0]

\* Add(1): homed on cell 3
S4 == [S3 EXCEPT !.slots = [s \in 0..1 |-> IF s = 1 THEN [E4 EXCEPT ![3] = 1] ELSE E4], !.wset = {1}]

\* Publish
S5 == [S4 EXCEPT !.pub = [slot |-> 1, gen |-> 1], !.pset = {1}, !.wslot = 0, !.wpc = "idle"]

\* Load(1), Announce(1), Recheck(1), Lookup(1, 1)
S6 == [S5 EXCEPT !.rl = (1 :> [slot |-> 1, gen |-> 1]), !.held = (1 :> {1}), !.rpc = (1 :> "loaded")]
S7 == [S6 EXCEPT !.ann = (1 :> 1), !.rpc = (1 :> "announced")]
S8 == [S7 EXCEPT !.rpc = (1 :> "held")]
S9 == [S8 EXCEPT !.look = (1 :> [addr |-> 1, found |-> TRUE])]

GoodLog == << S1, S2, S3, S4, S5, S6, S7, S8, S9 >>

\* the add publishes the slot it writes: record 4 is no model step
BadLog == << S1, S2, S3, [S4 EXCEPT !.pub = [slot |-> 1, gen |-> 1]], S5, S6, S7, S8, S9 >>

=============================================================================
