------------------------- MODULE SessionSendSequence -------------------------
(***************************************************************************)
(* The session send sequence of one connection direction, in the layout   *)
(* the code keeps it: one counter, send_sequence_session, holding the next *)
(* sequence to spend (sdk/rocketnet_server_protocol.cpp and                 *)
(* sdk/rocketnet_client_protocol.cpp).  The sequence is the AEAD nonce     *)
(* under one directional session key, so no two sealed packets may carry   *)
(* the same one.                                                           *)
(*                                                                         *)
(* Two kinds of sender share the counter.  A fill (start_payload, then     *)
(* seal_payload_frame) takes its sequence with one atomic fetch-add and    *)
(* seals later.  A control send (keep-alive, standalone ack, disconnect)   *)
(* takes its sequence and seals it.  The interleaving is arbitrary: a      *)
(* caller may overlap a control send with any number of fills.            *)
(*                                                                         *)
(* The ack block an inbound packet carries is read against a baseline,    *)
(* this end's highest sent sequence.  The code reads the delivery          *)
(* bookkeeping's high-water mark, folded from each sealed sequence         *)
(* (highest here); next - 1 also counts sequences a fill holds unsealed.  *)
(*                                                                         *)
(* Broken selects a reversed witness:                                      *)
(*   "readinc"  the control send reads the counter, seals, then            *)
(*              increments it: two steps, so a fill can take the same      *)
(*              sequence in between (SequencesUnique fails).               *)
(*   "baseline" the ack baseline is next - 1 (BaselineIsHighestSealed      *)
(*              fails while a fill holds a sequence unsealed).             *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    MaxSeq,     \* the counter's bound for the check
    Fills,      \* the fill-phase senders that can hold a sequence at once
    Broken      \* "none", "readinc" or "baseline"

ASSUME /\ MaxSeq \in Nat \ {0}
       /\ Fills # {}
       /\ Broken \in {"none", "readinc", "baseline"}

Seqs == 1..MaxSeq

VARIABLES
    next,       \* send_sequence_session: the next sequence to spend
    sealed,     \* how many packets have been sealed at each sequence
    held,       \* each fill's taken, unsealed sequence, 0 for none
    ctl,        \* the control send: "idle" or "taken"
    ctlSeq,     \* the control send's sequence while taken
    highest     \* the ack endpoint's highest_sequence: highest sealed

vars == <<next, sealed, held, ctl, ctlSeq, highest>>

Max(a, b) == IF a > b THEN a ELSE b

Init ==
    /\ next = 1
    /\ sealed = [s \in Seqs |-> 0]
    /\ held = [f \in Fills |-> 0]
    /\ ctl = "idle"
    /\ ctlSeq = 0
    /\ highest = 0

Seal(s) ==
    /\ sealed' = [sealed EXCEPT ![s] = @ + 1]
    /\ highest' = Max(highest, s)

\* rocketnet_*_protocol_start_payload: one atomic fetch-add
FillStart(f) ==
    /\ held[f] = 0
    /\ next <= MaxSeq
    /\ held' = [held EXCEPT ![f] = next]
    /\ next' = next + 1
    /\ UNCHANGED <<sealed, ctl, ctlSeq, highest>>

\* rocketnet_*_protocol_seal_payload_frame: seals the held sequence and
\* records it sent
FillSeal(f) ==
    /\ held[f] # 0
    /\ Seal(held[f])
    /\ held' = [held EXCEPT ![f] = 0]
    /\ UNCHANGED <<next, ctl, ctlSeq>>

\* the caller abandons a reservation: the sequence is burned, never sealed
FillAbort(f) ==
    /\ held[f] # 0
    /\ held' = [held EXCEPT ![f] = 0]
    /\ UNCHANGED <<next, sealed, ctl, ctlSeq, highest>>

\* rocketnet_session_sequence_take: one fetch-add returns the sequence; the
\* broken form reads the counter and leaves it for the seal to increment
ControlTake ==
    /\ ctl = "idle"
    /\ next <= MaxSeq
    /\ ctlSeq' = next
    /\ ctl' = "taken"
    /\ next' = IF Broken = "readinc" THEN next ELSE next + 1
    /\ UNCHANGED <<sealed, held, highest>>

ControlSeal ==
    /\ ctl = "taken"
    /\ Seal(ctlSeq)
    /\ ctl' = "idle"
    /\ ctlSeq' = 0
    /\ next' = IF Broken = "readinc" THEN next + 1 ELSE next
    /\ UNCHANGED held

\* every sequence spent and every sender idle: a terminal stutter
Done ==
    /\ next > MaxSeq
    /\ ctl = "idle"
    /\ \A f \in Fills : held[f] = 0
    /\ UNCHANGED vars

Next ==
    \/ \E f \in Fills : FillStart(f) \/ FillSeal(f) \/ FillAbort(f)
    \/ ControlTake
    \/ ControlSeal
    \/ Done

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

\* the reference rocketnet_*_protocol_highest_sent_session returns
Baseline == IF Broken = "baseline" THEN next - 1 ELSE highest

HighestSealed ==
    LET S == {s \in Seqs : sealed[s] > 0}
    IN  IF S = {} THEN 0 ELSE CHOOSE s \in S : \A t \in S : t <= s

TypeOK ==
    /\ next \in 1..MaxSeq + 2
    /\ sealed \in [Seqs -> Nat]
    /\ held \in [Fills -> 0..MaxSeq]
    /\ ctl \in {"idle", "taken"}
    /\ ctlSeq \in 0..MaxSeq
    /\ highest \in 0..MaxSeq

\* no two sealed packets under the key carry one sequence (one nonce)
SequencesUnique == \A s \in Seqs : sealed[s] <= 1

\* nothing taken or sealed is sequence zero
NoZeroSequence ==
    /\ \A f \in Fills : held[f] # 0 => held[f] >= 1
    /\ ctl = "taken" => ctlSeq >= 1

\* the ack baseline is exactly the highest sequence sealed
BaselineIsHighestSealed == Baseline = HighestSealed
=============================================================================
