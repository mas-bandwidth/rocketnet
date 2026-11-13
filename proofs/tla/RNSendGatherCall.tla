-------------------------- MODULE RNSendGatherCall --------------------------
(***************************************************************************)
(* The send gather (X2, sdk/rocketnet_send_gather.cpp) as its caller sees  *)
(* it: one call is one step.  RNSendGatherLayout models the gather inside  *)
(* the call, step by step at the code's layout; a caller sees only the     *)
(* bank it hands over and what the call gives back, and this module is     *)
(* that boundary, the spec the trace check binds the code to               *)
(* (README-TraceCheck.md: what a call gives back is an argument of its     *)
(* action).                                                                *)
(*                                                                         *)
(* The bank is S slots, each with a destination and a size (0: aborted).   *)
(* Stage writes one slot, as a send worker's bank is written before it is  *)
(* handed over.  Gather(o, r) is one call of rocketnet_send_gather on the  *)
(* bank: o is the order it writes and r the runs, numbered from 1.         *)
(*                                                                         *)
(*   GatherOrder  the staged slots, destinations in the order of their     *)
(*                first slot, each destination's slots in staged order.    *)
(*   GatherRuns   each destination's range of the order cut into runs: a   *)
(*                run takes the next packet while it holds fewer than      *)
(*                MaxSeg, the total stays within MaxBytes, and the packet  *)
(*                is no larger than the run's first; a smaller packet      *)
(*                ends the run.                                            *)
(*                                                                         *)
(* MCRNSendGatherLayout.cfg checks MatchesCall: wherever the layout model  *)
(* ends, its order and runs are GatherOrder and GatherRuns of its bank.    *)
(* So the code is bound to the layout model through this module: the       *)
(* trace check shows the code's results are this module's, and TLC shows   *)
(* this module's are the layout model's, on every bank of its instance.    *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS S,          \* slots in the bank
          Dests,      \* destinations
          Sizes,      \* packet sizes, 0 = aborted
          MaxSeg,     \* most packets in a run
          MaxBytes    \* most bytes in a run of more than one packet

ASSUME S >= 1 /\ MaxSeg >= 1 /\ MaxBytes >= 1
ASSUME 0 \in Sizes /\ Sizes \subseteq Nat /\ 0 \notin Dests

Slots == 0..(S - 1)
NoRun == [count |-> 0]

VARIABLES dest,       \* each slot's destination, 0 before it is first staged
          size        \* each slot's size, 0 = aborted

vars == <<dest, size>>

Init ==
    /\ dest = [s \in Slots |-> 0]
    /\ size = [s \in Slots |-> 0]

-----------------------------------------------------------------------------

Staged == {s \in Slots : size[s] > 0}

\* the staged slots ordered by their key: the first staged slot of their
\* destination, then the slot, so destinations come in the order of their
\* first slot and each destination's slots in staged order (the LET values
\* are computed once per use of GatherOrder)
GatherOrder ==
    LET first == [d \in {dest[s] : s \in Staged} |->
                     CHOOSE f \in Staged : dest[f] = d /\ \A t \in Staged : dest[t] = d => f <= t]
        key == [s \in Staged |-> first[dest[s]] * S + s]
    IN [p \in 1..Cardinality(Staged) |-> CHOOSE s \in Staged : Cardinality({t \in Staged : key[t] < key[s]}) = p - 1]

Close(r, rs) == IF r.count > 0 THEN Append(rs, [first |-> r.first, count |-> r.count,
                                                 seg |-> r.seg, last |-> r.last]) ELSE rs

\* the runs of order o over this bank: R[p] is the closed runs and the open
\* run after positions 1..p; a packet to another destination than the one
\* before it starts a run
RunsOf(o) ==
    LET n == Len(o)
        R[p \in 0..n] ==
            IF p = 0 THEN [rs |-> <<>>, open |-> NoRun]
            ELSE LET prev == R[p - 1]
                     r == prev.open
                     b == size[o[p]]
                     fits == /\ p > 1
                             /\ dest[o[p - 1]] = dest[o[p]]
                             /\ ~r.closed
                             /\ r.count < MaxSeg
                             /\ r.total + b <= MaxBytes
                             /\ b <= r.seg
                 IN IF fits
                      THEN [rs |-> prev.rs,
                            open |-> [r EXCEPT !.count = @ + 1, !.total = @ + b, !.last = b,
                                               !.closed = (b < r.seg)]]
                      ELSE [rs |-> Close(r, prev.rs),
                            open |-> [first |-> p, count |-> 1, seg |-> b, total |-> b, last |-> b,
                                      closed |-> FALSE]]
    IN Close(R[n].open, R[n].rs)

GatherRuns == RunsOf(GatherOrder)

-----------------------------------------------------------------------------

\* the caller writes slot s: destination d, size b
Stage(s, d, b) ==
    /\ dest' = [dest EXCEPT ![s] = d]
    /\ size' = [size EXCEPT ![s] = b]

\* rocketnet_send_gather on the bank: it writes the order o and returns the
\* runs r; the bank is the caller's and stays as it was. With o the order,
\* RunsOf(o) is GatherRuns
Gather(o, r) ==
    /\ o = GatherOrder
    /\ r = RunsOf(o)
    /\ UNCHANGED vars

\* GatherOrder is computed once a step, as o
Next ==
    \/ \E s \in Slots, d \in Dests, b \in Sizes : Stage(s, d, b)
    \/ \E o \in {GatherOrder} : Gather(o, RunsOf(o))

Spec == Init /\ [][Next]_vars

=============================================================================
