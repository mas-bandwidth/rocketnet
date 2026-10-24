------------------------------ MODULE MCLink ------------------------------
(***************************************************************************)
(* The small instance that proves Link itself.  Two abstract endpoints: a  *)
(* sender that may hand any packet to the link at any time, and a receiver *)
(* that takes whatever is deliverable.  Two ghosts record what was ever    *)
(* sent and ever delivered.                                                *)
(*                                                                         *)
(* Safety (every pattern): TypeOK, InFlightBounded, DeliveredWereSent,     *)
(* WireCarriesOnlySent, FirstSendLost.                                     *)
(*                                                                         *)
(* Liveness: ResentIsDelivered under Spec (Link!Fairness) for every       *)
(* pattern; LinkDelivers under SpecWireFair (Link!WireFair alone), which   *)
(* holds for the four deterministic patterns and must fail for             *)
(* "arbitrary": that failure is the reversed witness showing               *)
(* EventualDelivery does the work.  README-Link.md lists the cfg lines     *)
(* for each run and the two traces TLC printed.                            *)
(***************************************************************************)
EXTENDS Link

VARIABLES everSent, everDelivered

mcVars == <<wire, sent, pulse, everSent, everDelivered>>

MCInit ==
    /\ Init
    /\ everSent = {}
    /\ everDelivered = {}

MCSend ==
    \E p \in Packets :
        /\ Send(p)
        /\ everSent' = everSent \cup {p}
        /\ UNCHANGED everDelivered

MCDeliver ==
    \E p \in Packets :
        /\ Deliver(p)
        /\ everDelivered' = everDelivered \cup {p}
        /\ UNCHANGED everSent

MCEnvironment ==
    /\ Environment
    /\ UNCHANGED <<everSent, everDelivered>>

MCNext == MCSend \/ MCDeliver \/ MCEnvironment

Spec         == MCInit /\ [][MCNext]_mcVars /\ Fairness
SpecWireFair == MCInit /\ [][MCNext]_mcVars /\ WireFair

(***************************************************************************)
(* Safety.                                                                 *)
(***************************************************************************)
MCTypeOK ==
    /\ TypeOK
    /\ everSent \subseteq Packets
    /\ everDelivered \subseteq Packets

DeliveredWereSent == everDelivered \subseteq everSent

WireCarriesOnlySent == \A i \in DOMAIN wire : wire[i].pkt \in everSent

\* The pattern constants act: where the pattern names the first send as
\* lost, the wire is empty right after it.
FirstSendLost ==
    (Pattern \in {"first", "alternate"} \/ (Pattern = "burst" /\ BurstStart = 1))
        => (sent = 1 => wire = <<>>)

(***************************************************************************)
(* Liveness.                                                               *)
(***************************************************************************)

\* A packet sent without bound is delivered.
ResentIsDelivered ==
    \A p \in Packets : ([]<> <<Send(p)>>_vars) => <>(p \in everDelivered)

\* A link that is sent on without bound delivers without bound.
AnySend    == \E p \in Packets : Send(p)
AnyDeliver == \E p \in Packets : Deliver(p)

LinkDelivers == ([]<> <<AnySend>>_vars) => ([]<> <<AnyDeliver>>_vars)

=============================================================================
