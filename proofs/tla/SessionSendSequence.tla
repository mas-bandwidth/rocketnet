------------------------- MODULE SessionSendSequence -------------------------
(***************************************************************************)
(* The session send sequence of one connection direction, in the layout   *)
(* the code keeps it: one counter, send_sequence_session, holding the next *)
(* sequence to spend (sdk/rocketnet_server_protocol.cpp and                 *)
(* sdk/rocketnet_client_protocol.cpp).  The sequence is the AEAD nonce     *)
(* under one directional session key, so no two sealed packets may carry   *)
(* the same one.                                                           *)
(*                                                                         *)
(* Three kinds of sender share the counter.  A fill (start_payload, then   *)
(* seal_payload_frame) takes its sequence with one atomic fetch-add and    *)
(* seals later.  A control send (keep-alive, standalone ack, disconnect)   *)
(* takes its sequence and seals it.  The server's deferred disconnect      *)
(* burst (rocketnet_server_transport_defer_disconnect_burst in             *)
(* sdk/rocketnet_server_transport.cpp) takes Burst sequences               *)
(* (ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS) in one fetch-add (BurstTake)  *)
(* and the drain seals them one at a time, in order, later, under a copy   *)
(* of the key (BurstSeal); it may stop short, which burns the rest.  The   *)
(* interleaving is arbitrary: a caller may overlap a control send or the   *)
(* burst with any number of fills, though the code's phase rules keep the  *)
(* burst after the connection's last fill.                                 *)
(*                                                                         *)
(* The ack block an inbound packet carries is read against a baseline,    *)
(* this end's highest sent sequence.  The code reads the delivery          *)
(* bookkeeping's high-water mark, folded from each sealed sequence         *)
(* (highest here); next - 1 also counts sequences a fill holds unsealed.  *)
(* The burst's seals are not folded: they are made after the connection    *)
(* ended, when no ack is read against the baseline, so the baseline is the *)
(* highest sequence a fill or a control send sealed.                       *)
(*                                                                         *)
(* Broken selects a reversed witness:                                      *)
(*   "readinc"  the control send reads the counter, seals, then            *)
(*              increments it: two steps, so a fill can take the same      *)
(*              sequence in between (SequencesUnique fails).               *)
(*   "baseline" the ack baseline is next - 1 (BaselineIsHighestSealed      *)
(*              fails while a fill holds a sequence unsealed).             *)
(*   "burstone" the burst takes one sequence and seals Burst from it, so a *)
(*              send that takes the next one after it repeats a sequence   *)
(*              (SequencesUnique fails).                                   *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    MaxSeq,     \* the counter's bound for the check
    Fills,      \* the fill-phase senders that can hold a sequence at once
    Burst,      \* the disconnect burst's packets (ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS)
    Broken      \* "none", "readinc", "baseline" or "burstone"

ASSUME /\ MaxSeq \in Nat \ {0}
       /\ Fills # {}
       /\ Burst \in Nat \ {0}
       /\ Broken \in {"none", "readinc", "baseline", "burstone"}

Seqs == 1..MaxSeq

VARIABLES
    next,       \* send_sequence_session: the next sequence to spend
    sealed,     \* how many packets have been sealed at each sequence
    held,       \* each fill's taken, unsealed sequence, 0 for none
    ctl,        \* the control send: "idle" or "taken"
    ctlSeq,     \* the control send's sequence while taken
    highest,    \* the ack endpoint's highest_sequence: highest sealed
    burstFirst, \* the burst's first_sequence, 0 before it is taken
    burstSealed \* the burst's packets sealed (its sealed count)

vars == <<next, sealed, held, ctl, ctlSeq, highest, burstFirst, burstSealed>>
burstVars == <<burstFirst, burstSealed>>

Max(a, b) == IF a > b THEN a ELSE b

Init ==
    /\ next = 1
    /\ sealed = [s \in Seqs |-> 0]
    /\ held = [f \in Fills |-> 0]
    /\ ctl = "idle"
    /\ ctlSeq = 0
    /\ highest = 0
    /\ burstFirst = 0
    /\ burstSealed = 0

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
    /\ UNCHANGED burstVars

\* rocketnet_*_protocol_seal_payload_frame: seals the held sequence and
\* records it sent
FillSeal(f) ==
    /\ held[f] # 0
    /\ Seal(held[f])
    /\ held' = [held EXCEPT ![f] = 0]
    /\ UNCHANGED <<next, ctl, ctlSeq>>
    /\ UNCHANGED burstVars

\* the caller abandons a reservation: the sequence is burned, never sealed
FillAbort(f) ==
    /\ held[f] # 0
    /\ held' = [held EXCEPT ![f] = 0]
    /\ UNCHANGED <<next, sealed, ctl, ctlSeq, highest>>
    /\ UNCHANGED burstVars

\* rocketnet_session_sequence_take: one fetch-add returns the sequence; the
\* broken form reads the counter and leaves it for the seal to increment
ControlTake ==
    /\ ctl = "idle"
    /\ next <= MaxSeq
    /\ ctlSeq' = next
    /\ ctl' = "taken"
    /\ next' = IF Broken = "readinc" THEN next ELSE next + 1
    /\ UNCHANGED <<sealed, held, highest>>
    /\ UNCHANGED burstVars

ControlSeal ==
    /\ ctl = "taken"
    /\ Seal(ctlSeq)
    /\ ctl' = "idle"
    /\ ctlSeq' = 0
    /\ next' = IF Broken = "readinc" THEN next + 1 ELSE next
    /\ UNCHANGED held
    /\ UNCHANGED burstVars

\* rocketnet_server_transport_defer_disconnect_burst: the burst's Burst
\* sequences in one atomic add (first_sequence), once for the connection;
\* the broken form takes one
BurstTake ==
    /\ burstFirst = 0
    /\ next + Burst - 1 <= MaxSeq
    /\ burstFirst' = next
    /\ next' = IF Broken = "burstone" THEN next + 1 ELSE next + Burst
    /\ UNCHANGED <<sealed, held, ctl, ctlSeq, highest, burstSealed>>

\* rocketnet_server_transport_drain_disconnect_bursts: the next packet of
\* the burst sealed at first_sequence + sealed, not folded into the ack
\* bookkeeping (the connection has ended)
BurstSeal ==
    /\ burstFirst # 0
    /\ burstSealed < Burst
    /\ sealed' = [sealed EXCEPT ![burstFirst + burstSealed] = @ + 1]
    /\ burstSealed' = burstSealed + 1
    /\ UNCHANGED <<next, held, ctl, ctlSeq, highest, burstFirst>>

\* every sequence spent and every sender idle: a terminal stutter
Done ==
    /\ next > MaxSeq
    /\ ctl = "idle"
    /\ \A f \in Fills : held[f] = 0
    /\ burstFirst = 0 \/ burstSealed = Burst
    /\ UNCHANGED vars

Next ==
    \/ \E f \in Fills : FillStart(f) \/ FillSeal(f) \/ FillAbort(f)
    \/ ControlTake
    \/ ControlSeal
    \/ BurstTake
    \/ BurstSeal
    \/ Done

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

\* the reference rocketnet_*_protocol_highest_sent_session returns
Baseline == IF Broken = "baseline" THEN next - 1 ELSE highest

\* the burst's sequences, sealed after the connection ended
BurstSeqs == IF burstFirst = 0 THEN {} ELSE burstFirst..(burstFirst + Burst - 1)

\* the highest sequence a fill or a control send sealed
HighestSealed ==
    LET S == {s \in Seqs : sealed[s] > 0 /\ s \notin BurstSeqs}
    IN  IF S = {} THEN 0 ELSE CHOOSE s \in S : \A t \in S : t <= s

TypeOK ==
    /\ next \in 1..MaxSeq + 2
    /\ sealed \in [Seqs -> Nat]
    /\ held \in [Fills -> 0..MaxSeq]
    /\ ctl \in {"idle", "taken"}
    /\ ctlSeq \in 0..MaxSeq
    /\ highest \in 0..MaxSeq
    /\ burstFirst \in 0..MaxSeq
    /\ burstSealed \in 0..Burst

\* no two sealed packets under the key carry one sequence (one nonce)
SequencesUnique == \A s \in Seqs : sealed[s] <= 1

\* nothing taken or sealed is sequence zero
NoZeroSequence ==
    /\ \A f \in Fills : held[f] # 0 => held[f] >= 1
    /\ ctl = "taken" => ctlSeq >= 1
    /\ burstSealed > 0 => burstFirst >= 1

\* the ack baseline is exactly the highest sequence a fill or a control
\* send sealed
BaselineIsHighestSealed == Baseline = HighestSealed
=============================================================================
