------------------------------- MODULE Link -------------------------------
(***************************************************************************)
(* rocketnet: one direction of a UDP path between client and server.       *)
(*                                                                         *)
(* The shared lossy link every other rocketnet model drives its            *)
(* transitions through.  A model instantiates it once per direction:       *)
(*                                                                         *)
(*   C2S == INSTANCE Link WITH wire <- c2sWire, sent <- c2sSent,           *)
(*                             pulse <- c2sPulse,                          *)
(*                             Packets <- ClientPackets, ...               *)
(*                                                                         *)
(* and conjoins C2S!Send(p) into the action that sends p, C2S!Deliver(p)   *)
(* into the action that receives p, and C2S!Age into its Tick.  The link   *)
(* carries values from Packets ("typed packets"); it never looks inside    *)
(* them.  What the real path may do to a datagram, and the simulator in    *)
(* sdk/rocketnet_network_simulator.cpp injects (loss, latency, jitter,      *)
(* duplication), is here as one action each: Drop, Delay (with Age),       *)
(* Reorder, Duplicate.  The transport never resends and a lost packet      *)
(* loses its payload (USAGE.md); nothing here resends or acknowledges.     *)
(* That is the models' business.                                           *)
(*                                                                         *)
(* Loss patterns.  Pattern selects which sends the link loses, so TLC can  *)
(* explore one deterministic terrain at a time ("first packet dropped,     *)
(* second packet comes through") as well as the arbitrary one:             *)
(*                                                                         *)
(*   "none"       nothing is lost                                          *)
(*   "first"      the first send on this link is lost                      *)
(*   "alternate"  the 1st, 3rd, 5th ... sends are lost; even ones arrive   *)
(*   "burst"      sends BurstStart .. BurstStart+BurstLen-1 are lost       *)
(*   "everynth"   every BurstLen-th send (BurstLen, 2*BurstLen, ...) lost  *)
(*   "arbitrary"  any in-flight copy may be dropped at any time (Drop)     *)
(*                                                                         *)
(* Patterned loss happens at Send, as the simulator decides it             *)
(* (rocketnet_network_simulator_send, the packet_loss_percent roll).       *)
(* Under "arbitrary" nothing is lost at Send and Drop is enabled instead.  *)
(*                                                                         *)
(* Capacity bounds the copies in flight.  Send and Duplicate are disabled  *)
(* at the bound rather than dropping, so Capacity is a model-size knob and *)
(* never a hidden loss source: the simulator's "queue full" drop is a      *)
(* harness artefact at 4096 slots and is not modelled.                     *)
(*                                                                         *)
(* Order.  The wire is a FIFO of copies, each with a due counter           *)
(* (latency + jitter, in ticks).  Deliver hands over the head once its     *)
(* due is 0; Age (time passing) counts every due down; Delay adds a tick   *)
(* of jitter to a copy in flight; Reorder swaps two adjacent copies.  With *)
(* Reordering = FALSE the link is in-order; with MaxDelay = 0 it has no    *)
(* latency and Age is a no-op.                                             *)
(*                                                                         *)
(* Fairness, stated once, here.  WireFair: the wire eventually delivers a  *)
(* due head and time passes.  EventualDelivery: a packet re-sent without   *)
(* bound is delivered without bound.  Models rely on Fairness for          *)
(* liveness only; no safety property may depend on it.  See README-Link.md *)
(* for which patterns need which, and the vacuity caution.                 *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS
    Packets,      \* the values this link may carry
    Capacity,     \* bound on copies in flight, >= 1
    MaxDelay,     \* bound on a copy's due counter (ticks); 0 = no latency
    Duplication,  \* BOOLEAN: may the link duplicate a copy
    Reordering,   \* BOOLEAN: may the link swap two adjacent copies
    Pattern,      \* which sends are lost, see above
    BurstStart,   \* "burst": index of the first lost send, >= 1
    BurstLen      \* "burst": how many consecutive sends are lost, >= 1

Patterns == {"none", "first", "alternate", "burst", "everynth", "arbitrary"}

ASSUME /\ Capacity \in Nat \ {0}
       /\ MaxDelay \in Nat
       /\ Duplication \in BOOLEAN
       /\ Reordering \in BOOLEAN
       /\ Pattern \in Patterns
       /\ BurstStart \in Nat \ {0}
       /\ BurstLen \in Nat \ {0}

VARIABLES
    wire,   \* Seq of [pkt |-> p \in Packets, due |-> d \in 0..MaxDelay]
    sent,   \* how many sends this link has seen, saturating (see Horizon)
    pulse   \* flips on every Deliver (see below)

vars == <<wire, sent, pulse>>

(***************************************************************************)
(* pulse exists so that a Deliver step is never mistaken for a Drop.  When *)
(* the copies on the wire are identical, Drop(i) and Deliver(p) change     *)
(* wire the same way, and the action formulas the fairness conditions are  *)
(* built from (<<Deliver(p)>>_vars) look only at how the variables change. *)
(* TLC found exactly that: a lasso of send-drop-send-drop satisfied        *)
(* EventualDelivery because each drop also read as a delivery.  Only       *)
(* Deliver changes pulse, so the two steps are now distinct.               *)
(***************************************************************************)

Copy == [pkt : Packets, due : 0..MaxDelay]

(***************************************************************************)
(* The send counter.  The patterns only care about the index of a send up  *)
(* to the end of the burst, so past that the counter cycles between the    *)
(* two values Horizon-1 (odd) and Horizon (even): every index the patterns *)
(* name stays behind, and parity, which "alternate" needs, is preserved.   *)
(* Horizon is the least even number >= BurstStart + BurstLen + 1.          *)
(***************************************************************************)
Horizon == 2 * ((BurstStart + BurstLen + 2) \div 2)

NextCount ==
    IF Pattern = "everynth"
    THEN IF sent < BurstLen THEN sent + 1 ELSE 1
    ELSE IF sent < Horizon THEN sent + 1 ELSE Horizon - 1

LostAtSend(n) ==
    CASE Pattern = "none"      -> FALSE
      [] Pattern = "first"     -> n = 1
      [] Pattern = "alternate" -> n % 2 = 1
      [] Pattern = "burst"     -> n \in BurstStart .. BurstStart + BurstLen - 1
      [] Pattern = "everynth"  -> n = BurstLen
      [] Pattern = "arbitrary" -> FALSE

InFlight == Len(wire)

Carrying(p) == \E i \in DOMAIN wire : wire[i].pkt = p

Deliverable(p) == wire # <<>> /\ Head(wire).pkt = p /\ Head(wire).due = 0

Init == wire = <<>> /\ sent = 0 /\ pulse = 0

(***************************************************************************)
(* The operations.                                                         *)
(***************************************************************************)

\* The sender hands p to the link.  The send is counted whether or not it
\* is lost; a lost send leaves the wire as it was.  Disabled at Capacity.
Send(p) ==
    /\ p \in Packets
    /\ InFlight < Capacity
    /\ sent' = NextCount
    /\ IF LostAtSend(NextCount)
         THEN UNCHANGED wire
         ELSE \E d \in 0..MaxDelay : wire' = Append(wire, [pkt |-> p, due |-> d])
    /\ UNCHANGED pulse

\* The receiver takes the head of the wire, which carries p and is due.
Deliver(p) ==
    /\ Deliverable(p)
    /\ wire' = Tail(wire)
    /\ pulse' = 1 - pulse
    /\ UNCHANGED sent

(***************************************************************************)
(* The outside events.                                                     *)
(***************************************************************************)

Remove(s, i) == [j \in 1..Len(s)-1 |-> IF j < i THEN s[j] ELSE s[j+1]]

\* "arbitrary" only: copy i is lost in flight.
Drop(i) ==
    /\ Pattern = "arbitrary"
    /\ i \in DOMAIN wire
    /\ wire' = Remove(wire, i)
    /\ UNCHANGED <<sent, pulse>>

\* Copy i is duplicated; the duplicate gets its own latency and jitter,
\* as the simulator's duplicate_percent branch does.  Disabled at Capacity.
Duplicate(i) ==
    /\ Duplication
    /\ i \in DOMAIN wire
    /\ InFlight < Capacity
    /\ \E d \in 0..MaxDelay : wire' = Append(wire, [wire[i] EXCEPT !.due = d])
    /\ UNCHANGED <<sent, pulse>>

\* Copies i and i+1 swap places.
Reorder(i) ==
    /\ Reordering
    /\ i \in DOMAIN wire /\ i + 1 \in DOMAIN wire
    /\ wire[i] # wire[i+1]
    /\ wire' = [wire EXCEPT ![i] = wire[i+1], ![i+1] = wire[i]]
    /\ UNCHANGED <<sent, pulse>>

\* The trailing w copies on the wire reverse order in place (window reversal).
ReverseWindow(w) ==
    /\ Reordering
    /\ w \in 2..Len(wire)
    /\ wire' = [i \in DOMAIN wire |->
                    IF i <= Len(wire) - w
                    THEN wire[i]
                    ELSE wire[2 * Len(wire) - w + 1 - i]]
    /\ UNCHANGED <<sent, pulse>>

\* Copy i picks up one more tick of jitter in flight.
Delay(i) ==
    /\ i \in DOMAIN wire
    /\ wire[i].due < MaxDelay
    /\ wire' = [wire EXCEPT ![i].due = @ + 1]
    /\ UNCHANGED <<sent, pulse>>

\* A tick passes on the wire: every due counts down.  Conjoin into the
\* model's Tick.  With MaxDelay = 0 this leaves the link unchanged.
Age ==
    /\ wire' = [i \in DOMAIN wire |-> [wire[i] EXCEPT !.due = IF @ > 0 THEN @ - 1 ELSE 0]]
    /\ UNCHANGED <<sent, pulse>>

\* Everything the link does on its own, for a model that wants it as one
\* disjunct.  Send and Deliver are the endpoints' and stay with the model.
Environment ==
    \/ \E i \in DOMAIN wire : Drop(i) \/ Duplicate(i) \/ Reorder(i) \/ Delay(i)
    \/ (MaxDelay > 0 /\ Age)

(***************************************************************************)
(* What the link guarantees on its own (checked by MCLink).                *)
(***************************************************************************)

TypeOK ==
    /\ wire \in Seq(Copy)
    /\ sent \in 0..Horizon
    /\ pulse \in {0, 1}

InFlightBounded == InFlight <= Capacity

(***************************************************************************)
(* Fairness.  The one assumption the connection, security and ack models   *)
(* rely on for liveness, never for safety.                                 *)
(*                                                                         *)
(* WireFair: a copy that is at the head and due infinitely often is        *)
(* delivered infinitely often, and time passes.  This is the wire being a  *)
(* wire; it never conjures a dropped copy back.                            *)
(*                                                                         *)
(* EventualDelivery: a packet sent infinitely often is delivered           *)
(* infinitely often.  Under the deterministic patterns this follows from   *)
(* WireFair for a sender that re-sends only p; under "arbitrary" it is the *)
(* assumption that rules out the adversary who drops every copy forever.   *)
(* A model whose receiver has no enabled receive step for some            *)
(* deliverable packet makes this vacuous: receive-and-discard counts, so   *)
(* every model gives every packet type a receive action.                   *)
(***************************************************************************)
WireFair ==
    /\ \A p \in Packets : SF_vars(Deliver(p))
    /\ WF_vars(Age)

EventualDelivery ==
    \A p \in Packets : ([]<> <<Send(p)>>_vars) => ([]<> <<Deliver(p)>>_vars)

Fairness == WireFair /\ EventualDelivery

=============================================================================
