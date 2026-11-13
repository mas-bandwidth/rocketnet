-------------------------- MODULE MCLinkPatterns --------------------------
(***************************************************************************)
(* Three checks of what sdk/rocketnet_network_simulator.cpp does on top of *)
(* Link, each with reversed witnesses that set Defect to break one rule of *)
(* the code:                                                               *)
(*                                                                         *)
(*   SpecEveryNth       the deterministic every-Nth drop                   *)
(*                      (simulated_drop_every_nth)                         *)
(*   SpecReverseWindow  the window reversal (simulated_reverse_window),    *)
(*                      full and short windows                             *)
(*   SpecSlotStore      the simulator's slot store: where a packet waits, *)
(*                      the order packets due together leave in, and the  *)
(*                      store-full drop                                    *)
(*                                                                         *)
(* SpecEveryNth and SpecReverseWindow deliver from Link's wire in wire     *)
(* order, so their order properties hold for that harness, not for the     *)
(* simulator as a whole: SpecSlotStore shows the simulator delivers the    *)
(* packets due at a receive in slot order, which is not send order, so a  *)
(* model that runs over the simulator needs Link with Reordering = TRUE.  *)
(***************************************************************************)
EXTENDS Link, FiniteSets

CONSTANTS
    MaxSeq,
    StoreSlots,     \* SpecSlotStore: the simulator's slot capacity
    Defect

ASSUME /\ MaxSeq \in Nat \ {0}
       /\ StoreSlots \in Nat \ {0}
       /\ Defect \in {"none", "wrong_period", "skip_lost", "unreversed",
                      "short_unreversed", "uncounted_full"}

VARIABLES
    nextSend,
    delivered,
    window,         \* SpecReverseWindow: the sends buffered since the last flush
    flushed,        \* SpecReverseWindow: ghost, every flushed window reversed, in flush order
    draining,       \* SpecReverseWindow: a flushed window is on the wire
    store,          \* SpecSlotStore: slot -> the packet it holds, 0 when free
    storeRefused    \* SpecSlotStore: sends the full store refused (store_refused)

patVars == <<wire, sent, pulse, nextSend, delivered, window, flushed, draining,
             store, storeRefused>>

Slots == 0..StoreSlots-1

PatInit ==
    /\ Init
    /\ nextSend = 1
    /\ delivered = <<>>
    /\ window = <<>>
    /\ flushed = <<>>
    /\ draining = FALSE
    /\ store = [ s \in Slots |-> 0 ]
    /\ storeRefused = 0

Reverse(s) == [ i \in 1..Len(s) |-> s[Len(s) - i + 1] ]

(***************************************************************************)
(* Periodic every-Nth drop: sends 1..MaxSeq in order; under Defect =       *)
(* "none" uses Link!Send(p) with Pattern = "everynth" (dropping BurstLen,  *)
(* 2*BurstLen, ...), as rocketnet_network_simulator_send counts send_count *)
(* for every send before any draw.  Under Defect = "wrong_period" it drops *)
(* NextCount = BurstLen - 1 instead (EveryNthDropped).  Under Defect =     *)
(* "skip_lost" the counter does not count the send it drops, as an        *)
(* increment placed after the drop's return would: once the count reaches  *)
(* BurstLen every later send is dropped (EveryNthDropped).                 *)
(***************************************************************************)
SendEveryNth ==
    /\ nextSend <= MaxSeq
    /\ IF Defect \in {"wrong_period", "skip_lost"}
       THEN LET lost == IF Defect = "wrong_period"
                        THEN NextCount = BurstLen - 1
                        ELSE LostAtSend(NextCount)
            IN /\ nextSend \in Packets
               /\ InFlight < Capacity
               /\ sent' = IF Defect = "skip_lost" /\ lost THEN sent ELSE NextCount
               /\ IF lost
                  THEN UNCHANGED wire
                  ELSE wire' = Append(wire, [pkt |-> nextSend, due |-> 0])
               /\ UNCHANGED pulse
       ELSE Send(nextSend)
    /\ nextSend' = nextSend + 1
    /\ UNCHANGED <<delivered, window, flushed, draining, store, storeRefused>>

DeliverEveryNth ==
    \E p \in Packets :
        /\ Deliver(p)
        /\ delivered' = Append(delivered, p)
        /\ UNCHANGED <<nextSend, window, flushed, draining, store, storeRefused>>

DoneStutter ==
    /\ nextSend > MaxSeq
    /\ wire = <<>>
    /\ window = <<>>
    /\ \A s \in Slots : store[s] = 0
    /\ UNCHANGED patVars

NextEveryNth == SendEveryNth \/ DeliverEveryNth \/ DoneStutter

SpecEveryNth == PatInit /\ [][NextEveryNth]_patVars

(***************************************************************************)
(* Window reversal: each send goes into the window; the window is flushed  *)
(* reversed, whole, when it holds BurstLen sends                           *)
(* (rocketnet_simulator_emit) or, short, at the end of any send_packets    *)
(* (rocketnet_network_simulator_flush_window flushes any positive count),  *)
(* so FlushWindow is enabled at any count from 1.  A window of one is its  *)
(* own reversal.  The harness drains each flushed window before the next   *)
(* send, so the wire holds one window at a time; how the slot store orders *)
(* packets of two windows due together is SpecSlotStore's.  Defect =       *)
(* "unreversed" flushes every window unreversed; "short_unreversed"        *)
(* reverses only full windows (WindowOrderReversed, both).                 *)
(***************************************************************************)
SendIntoWindow ==
    /\ nextSend <= MaxSeq
    /\ ~draining
    /\ Len(window) < BurstLen
    /\ Send(nextSend)
    /\ nextSend' = nextSend + 1
    /\ window' = Append(window, nextSend)
    /\ UNCHANGED <<delivered, flushed, draining, store, storeRefused>>

FlushWindow ==
    /\ ~draining
    /\ Len(window) >= 1
    /\ IF \/ Len(window) = 1
          \/ Defect = "unreversed"
          \/ Defect = "short_unreversed" /\ Len(window) < BurstLen
       THEN UNCHANGED <<wire, sent, pulse>>
       ELSE ReverseWindow(Len(window))
    /\ flushed' = flushed \o Reverse(window)
    /\ window' = <<>>
    /\ draining' = TRUE
    /\ UNCHANGED <<nextSend, delivered, store, storeRefused>>

DeliverFromWindow ==
    /\ draining
    /\ \E p \in Packets :
        /\ Deliver(p)
        /\ delivered' = Append(delivered, p)
        /\ draining' = (Len(wire) > 1)
        /\ UNCHANGED <<nextSend, window, flushed, store, storeRefused>>

NextReverseWindow == SendIntoWindow \/ FlushWindow \/ DeliverFromWindow \/ DoneStutter

SpecReverseWindow == PatInit /\ [][NextReverseWindow]_patVars

(***************************************************************************)
(* The slot store (rocketnet_simulator_insert, rocketnet_network_simulator_ *)
(* receive and receive_any): a send takes the lowest free slot, or, with   *)
(* every slot held, is dropped and counted in store_refused; a receive     *)
(* takes the lowest held slot that is due.  Here every packet is due at    *)
(* once (no latency and no jitter, or packets sent at one time under one   *)
(* latency), so a receive takes the lowest held slot.  A slot freed by a   *)
(* receive is taken by the next send, below packets sent earlier and still *)
(* held: sends 1 and 2, receive 1, send 3 leaves 3 in slot 0 ahead of 2 in *)
(* slot 1, and the next receive returns 3 before 2.  SurvivorsInOrder      *)
(* fails here (MCLinkSlotStoreReorders.cfg), as the code does.  Defect =   *)
(* "uncounted_full" drops a send to a full store without counting it       *)
(* (StoreAccounts).                                                        *)
(***************************************************************************)
Held == { s \in Slots : store[s] # 0 }

SlotSend ==
    /\ nextSend <= MaxSeq
    /\ nextSend' = nextSend + 1
    /\ IF Held # Slots
       THEN LET s == CHOOSE f \in Slots \ Held : \A g \in Slots \ Held : f <= g
            IN /\ store' = [ store EXCEPT ![s] = nextSend ]
               /\ UNCHANGED storeRefused
       ELSE /\ storeRefused' = IF Defect = "uncounted_full" THEN storeRefused ELSE storeRefused + 1
            /\ UNCHANGED store
    /\ UNCHANGED <<wire, sent, pulse, delivered, window, flushed, draining>>

SlotReceive ==
    /\ Held # {}
    /\ LET s == CHOOSE h \in Held : \A g \in Held : h <= g
       IN /\ delivered' = Append(delivered, store[s])
          /\ store' = [ store EXCEPT ![s] = 0 ]
    /\ UNCHANGED <<wire, sent, pulse, nextSend, window, flushed, draining, storeRefused>>

NextSlotStore == SlotSend \/ SlotReceive \/ DoneStutter

SpecSlotStore == PatInit /\ [][NextSlotStore]_patVars

(***************************************************************************)
(* Invariants.                                                             *)
(***************************************************************************)
MCPatternsTypeOK ==
    /\ TypeOK
    /\ nextSend \in 1..(MaxSeq + 1)
    /\ delivered \in Seq(1..MaxSeq)
    /\ window \in Seq(1..MaxSeq)
    /\ Len(window) <= BurstLen
    /\ flushed \in Seq(1..MaxSeq)
    /\ draining \in BOOLEAN
    /\ store \in [ Slots -> 0..MaxSeq ]
    /\ storeRefused \in 0..MaxSeq

EveryNthDropped ==
    /\ \A i \in DOMAIN delivered : delivered[i] % BurstLen # 0
    /\ (nextSend > MaxSeq /\ wire = <<>>) =>
           Len(delivered) = MaxSeq - (MaxSeq \div BurstLen)

SurvivorsInOrder ==
    \A i \in 1..(Len(delivered) - 1) : delivered[i] < delivered[i + 1]

\* Each window, full or short, is delivered reversed: what the wire hands
\* out is a prefix of the flushed windows, each reversed.
WindowOrderReversed ==
    delivered = SubSeq(flushed, 1, Len(delivered))

\* Every send is delivered, held in a slot, or counted refused.
StoreAccounts ==
    nextSend - 1 = Len(delivered) + Cardinality(Held) + storeRefused

\* The store hands out only packets it was given, each once.
DeliveredOnce ==
    /\ \A i \in DOMAIN delivered : delivered[i] < nextSend
    /\ \A i, j \in DOMAIN delivered : i # j => delivered[i] # delivered[j]

=============================================================================
