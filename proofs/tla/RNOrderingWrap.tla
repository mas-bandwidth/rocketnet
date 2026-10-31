--------------------------- MODULE RNOrderingWrap ---------------------------
(***************************************************************************)
(* One reliable ordered channel's receive side with the code's widths:     *)
(* the read index ordered_read is a 32-bit counter (rn_channel_receive_t,  *)
(* sdk/rocketnet_allocator.h; here CounterMod) and the record's ordering   *)
(* is the 24-bit wire value (here WireMod).  CounterMod is a multiple of   *)
(* WireMod, as 2^32 is of 2^24, so the counter's low bits stay aligned     *)
(* with the sender's wire ordering across the counter's own wrap.          *)
(*                                                                         *)
(* A record runs the path of rn_ordered_receive (sdk/rocketnet_channel.cpp)*)
(* in one step: rn_receive_delivery_reservation counts the deliveries it   *)
(* can release (one, plus each complete held successor when the record is  *)
(* in turn), and a table without that much room defers the packet          *)
(* unchanged; otherwise rn_channels_dispatch_ordered places it by          *)
(* delta = (ordering - ordered_read) mod 2^24: in turn it is delivered     *)
(* (rn_channels_reliable_delivery, which exhausts the region when the      *)
(* table is full) and rn_channels_drain delivers each held successor whose *)
(* key (channel, ordered_read mod 2^24) is found, exhausting the region    *)
(* when the table is full; ahead it is held.  EndUpdate empties the        *)
(* delivery table (rn_channels_end_update).                                *)
(*                                                                         *)
(* Honest records only: layer 3 drops duplicates, and a sender has fewer   *)
(* than W records numbered at once, so a record reaching the dispatch is   *)
(* fresh and in ordered_read .. ordered_read + W - 1.  Blocks and the      *)
(* sequenced hold's extra delivery are left to RNChannel and               *)
(* RNChannelTypes; this model is the width of each comparison.             *)
(*                                                                         *)
(* Reversed witness: Masked = FALSE compares the masked record ordering    *)
(* with the unmasked counter in the reservation, the bug 4ec79571 fixed.   *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    W,           \* the window: orderings a sender has numbered at once
    WireMod,     \* the wire ordering's modulus (2^24 in the code)
    CounterMod,  \* ordered_read's modulus (2^32 in the code), a multiple of WireMod
    Cap,         \* the delivery table's capacity (delivery_entries)
    Masked       \* BOOLEAN: the reservation compares modulo WireMod

ASSUME CounterMod % WireMod = 0 /\ WireMod >= 2 * W /\ Cap >= 1

VARIABLES
    read,        \* rx->ordered_read
    held,        \* the held index's complete entries on this channel: wire orderings
    deliveries,  \* channels->deliveries
    exhausted,   \* rn_region_exhaust ran
    disagree     \* ghost: the reservation's compare differed from the mod-wire compare

vars == <<read, held, deliveries, exhausted, disagree>>

U24(x) == x % WireMod

TypeOK ==
    /\ read \in 0..CounterMod-1
    /\ held \subseteq 0..WireMod-1
    /\ deliveries \in 0..Cap
    /\ exhausted \in BOOLEAN
    /\ disagree \in BOOLEAN

Init ==
    /\ read = 0
    /\ held = {}
    /\ deliveries = 0
    /\ exhausted = FALSE
    /\ disagree = FALSE

\* the held run after o: complete entries at o+1, o+2, ... (lines 1815-1822)
RECURSIVE Run(_, _)
Run(o, fuel) == IF fuel = 0 \/ U24(o + 1) \notin held THEN 0 ELSE 1 + Run(o + 1, fuel - 1)

\* rn_receive_delivery_reservation, lines 1795-1825
InTurnCompare(o) == IF Masked THEN U24(o) = U24(read) ELSE U24(o) = read
Needed(o) == IF InTurnCompare(o) THEN 1 + Run(o, W) ELSE 1

\* rn_channels_drain, lines 601-658: from read r with d deliveries taken,
\* deliver each held successor found at key U24(r) while the table has room.
\* Returns <<read, held, deliveries, exhausted>>.
RECURSIVE Drain(_, _, _, _)
Drain(r, h, d, fuel) ==
    IF fuel = 0 \/ U24(r) \notin h
      THEN <<r, h, d, FALSE>>
      ELSE IF d >= Cap
             THEN <<r, h, d, TRUE>>          \* rn_channels_delivery_room exhausts the region
             ELSE Drain((r + 1) % CounterMod, h \ {U24(r)}, d + 1, fuel - 1)

\* one record of ordering o (a wire value) through rn_ordered_receive
Receive(o) ==
    /\ ~exhausted
    /\ disagree' = (disagree \/ (InTurnCompare(o) # (U24(o) = U24(read))))
    /\ IF Needed(o) > Cap - deliveries
         THEN UNCHANGED <<read, held, deliveries, exhausted>>      \* DEFERRED, lines 1856-1858
         ELSE LET delta == U24(o - read) IN                         \* lines 681-682
              IF delta = 0
                THEN IF deliveries >= Cap
                       THEN /\ exhausted' = TRUE                     \* lines 77-84
                            /\ UNCHANGED <<read, held, deliveries>>
                       ELSE LET dr == Drain((read + 1) % CounterMod, held, deliveries + 1, W)
                            IN  /\ read' = dr[1]
                                /\ held' = dr[2]
                                /\ deliveries' = dr[3]
                                /\ exhausted' = dr[4]
                ELSE /\ held' = held \cup {U24(o)}                   \* ahead: held (line 741)
                     /\ UNCHANGED <<read, deliveries, exhausted>>

\* the honest records: fresh, ahead by less than W, not already held
Honest == {U24(read + k) : k \in 0..W-1} \ held

EndUpdate ==
    /\ ~exhausted
    /\ deliveries' = 0
    /\ UNCHANGED <<read, held, exhausted, disagree>>

\* an exhausted region ends the connection: nothing more is received
Ended ==
    /\ exhausted
    /\ UNCHANGED vars

Next ==
    \/ \E o \in Honest : Receive(o)
    \/ EndUpdate
    \/ Ended

Spec == Init /\ [][Next]_vars

\* a full table defers the packet; it never exhausts the region in the drain
NoExhaust == ~exhausted

\* every ordering comparison agrees with the comparison modulo the wire
CompareAgrees == ~disagree

\* every held ordering is ahead of the read index by 1 .. W-1
HeldAhead == \A h \in held : U24(h - read) \in 1..W-1
=============================================================================
