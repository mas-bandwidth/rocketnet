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
(* fresh and in ordered_read .. ordered_read + W - 1.  Blocks are left to  *)
(* RNChannel and RNChannelTypes.                                           *)
(*                                                                         *)
(* The channel's one sequenced hold (rx->held_sequenced) is here too, as   *)
(* the delivery its drain may release: a code-4 hold takes a table slot or *)
(* exhausts the region; a code-1 hold takes a slot only while the update's *)
(* unreliable deliveries (code 0 and code 1, uDelivered) are below the     *)
(* allowance Du, and is otherwise refused and counted                      *)
(* (rn_sequenced_room).  The reservation counts the hold exactly when its  *)
(* release can take a slot (ExactSeq); NeverDeferWithRoom holds that a     *)
(* packet is deferred only when the deliveries its record can make do not  *)
(* fit.                                                                    *)
(*                                                                         *)
(* Reversed witnesses: Masked = FALSE compares the masked record ordering *)
(* with the unmasked counter in the reservation: past the wire wrap an     *)
(* in-turn record reads as not in turn and reserves too few deliveries     *)
(* (NoExhaust).  ExactSeq = FALSE counts a code-1 hold whatever Du has     *)
(* left: with Du spent, an in-turn record whose run fills the table is     *)
(* deferred although its deliveries fit (NeverDeferWithRoom).              *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    W,           \* the window: orderings a sender has numbered at once
    WireMod,     \* the wire ordering's modulus (2^24 in the code)
    CounterMod,  \* ordered_read's modulus (2^32 in the code), a multiple of WireMod
    Cap,         \* the delivery table's capacity (delivery_entries)
    Du,          \* the unreliable allowance: delivery_entries - window
    Masked,      \* BOOLEAN: the reservation compares modulo WireMod
    ExactSeq     \* BOOLEAN: the reservation counts a code-1 hold only when Du has room

ASSUME CounterMod % WireMod = 0 /\ WireMod >= 2 * W /\ Cap >= 1 /\ Du \in 0..Cap

VARIABLES
    read,        \* rx->ordered_read
    held,        \* the held index's complete entries on this channel: wire orderings
    deliveries,  \* channels->deliveries
    exhausted,   \* rn_region_exhaust ran
    disagree,    \* ghost: the reservation's compare differed from the mod-wire compare
    uDelivered,  \* channels->unreliable_delivered
    seqHold,     \* rx->held_sequenced: "none", "u" (a code-1 hold) or "r" (code 4)
    wasted       \* ghost: a packet was deferred although its deliveries fit

vars == <<read, held, deliveries, exhausted, disagree, uDelivered, seqHold, wasted>>

U24(x) == x % WireMod

TypeOK ==
    /\ read \in 0..CounterMod-1
    /\ held \subseteq 0..WireMod-1
    /\ deliveries \in 0..Cap
    /\ exhausted \in BOOLEAN
    /\ disagree \in BOOLEAN
    /\ uDelivered \in 0..Du
    /\ seqHold \in {"none", "u", "r"}
    /\ wasted \in BOOLEAN

Init ==
    /\ read = 0
    /\ held = {}
    /\ deliveries = 0
    /\ exhausted = FALSE
    /\ disagree = FALSE
    /\ uDelivered = 0
    /\ seqHold = "none"
    /\ wasted = FALSE

\* the held run after o: complete entries at o+1, o+2, ... (rn_receive_delivery_reservation)
RECURSIVE Run(_, _)
Run(o, fuel) == IF fuel = 0 \/ U24(o + 1) \notin held THEN 0 ELSE 1 + Run(o + 1, fuel - 1)

\* rn_receive_delivery_reservation
InTurnCompare(o) == IF Masked THEN U24(o) = U24(read) ELSE U24(o) = read

\* the sequenced hold's term: whether its release can take a table slot
SeqCanTake == seqHold = "r" \/ (seqHold = "u" /\ uDelivered < Du)
SeqTerm == IF ExactSeq THEN (IF SeqCanTake THEN 1 ELSE 0)
           ELSE (IF seqHold # "none" THEN 1 ELSE 0)
Needed(o) == IF InTurnCompare(o) THEN 1 + Run(o, W) + SeqTerm ELSE 1

\* the deliveries the dispatch can actually make: the record, its held run
\* and a hold whose release takes a slot (the mod-wire in-turn test)
Actual(o) == IF U24(o) = U24(read) THEN 1 + Run(o, W) + (IF SeqCanTake THEN 1 ELSE 0) ELSE 1

\* rn_channels_drain: from read r with d deliveries taken,
\* deliver each held successor found at key U24(r) while the table has room.
\* Returns <<read, held, deliveries, exhausted>>.
RECURSIVE Drain(_, _, _, _)
Drain(r, h, d, fuel) ==
    IF fuel = 0 \/ U24(r) \notin h
      THEN <<r, h, d, FALSE>>
      ELSE IF d >= Cap
             THEN <<r, h, d, TRUE>>          \* rn_channels_delivery_room exhausts the region
             ELSE Drain((r + 1) % CounterMod, h \ {U24(r)}, d + 1, fuel - 1)

\* the drain's release of the sequenced hold (rn_channels_drain_sequenced,
\* rn_sequenced_room): from d deliveries, <<deliveries, uDelivered, exhausted>>
ReleaseHold(d) ==
    CASE seqHold = "r" -> IF d >= Cap THEN <<d, uDelivered, TRUE>> ELSE <<d + 1, uDelivered, FALSE>>
      [] seqHold = "u" -> IF uDelivered >= Du \/ d >= Cap THEN <<d, uDelivered, FALSE>>   \* refused, counted
                          ELSE <<d + 1, uDelivered + 1, FALSE>>
      [] OTHER -> <<d, uDelivered, FALSE>>

\* one record of ordering o (a wire value) through rn_ordered_receive
Receive(o) ==
    /\ ~exhausted
    /\ disagree' = (disagree \/ (InTurnCompare(o) # (U24(o) = U24(read))))
    /\ IF Needed(o) > Cap - deliveries
         THEN /\ wasted' = (wasted \/ Actual(o) <= Cap - deliveries)
              /\ UNCHANGED <<read, held, deliveries, exhausted, uDelivered, seqHold>>   \* DEFERRED, rn_ordered_receive
         ELSE /\ UNCHANGED wasted
              /\ LET delta == U24(o - read) IN                       \* rn_channels_dispatch_ordered
                 IF delta = 0
                   THEN IF deliveries >= Cap
                          THEN /\ exhausted' = TRUE                  \* rn_channels_delivery_room
                               /\ UNCHANGED <<read, held, deliveries, uDelivered, seqHold>>
                          ELSE LET dr == Drain((read + 1) % CounterMod, held, deliveries + 1, W)
                               IN  /\ read' = dr[1]
                                   /\ held' = dr[2]
                                   \* the drain may or may not reach the hold's epoch
                                   /\ \/ /\ deliveries' = dr[3]
                                         /\ exhausted' = dr[4]
                                         /\ UNCHANGED <<uDelivered, seqHold>>
                                      \/ /\ seqHold # "none"
                                         /\ ~dr[4]
                                         /\ LET rh == ReleaseHold(dr[3])
                                            IN /\ deliveries' = rh[1]
                                               /\ uDelivered' = rh[2]
                                               /\ exhausted' = rh[3]
                                               /\ seqHold' = "none"
                   ELSE /\ held' = held \cup {U24(o)}                \* ahead: held (rn_channels_dispatch_ordered)
                        /\ UNCHANGED <<read, deliveries, exhausted, uDelivered, seqHold>>

\* a code-0 record (rn_channels_dispatch, code 0): delivered while the
\* unreliable allowance and the table have room, else dropped and counted
ReceiveUnreliable ==
    /\ ~exhausted
    /\ uDelivered < Du
    /\ deliveries < Cap
    /\ deliveries' = deliveries + 1
    /\ uDelivered' = uDelivered + 1
    /\ UNCHANGED <<read, held, exhausted, disagree, seqHold, wasted>>

\* a future sequenced record of either code taken into the empty hold
\* (rn_channels_dispatch_sequenced)
HoldSequenced ==
    /\ ~exhausted
    /\ seqHold = "none"
    /\ \E k \in {"u", "r"} : seqHold' = k
    /\ UNCHANGED <<read, held, deliveries, exhausted, disagree, uDelivered, wasted>>

\* the honest records: fresh, ahead by less than W, not already held
Honest == {U24(read + k) : k \in 0..W-1} \ held

EndUpdate ==
    /\ ~exhausted
    /\ deliveries' = 0
    /\ uDelivered' = 0
    /\ UNCHANGED <<read, held, exhausted, disagree, seqHold, wasted>>

\* an exhausted region ends the connection: nothing more is received
Ended ==
    /\ exhausted
    /\ UNCHANGED vars

Next ==
    \/ \E o \in Honest : Receive(o)
    \/ ReceiveUnreliable
    \/ HoldSequenced
    \/ EndUpdate
    \/ Ended

Spec == Init /\ [][Next]_vars

\* a full table defers the packet; it never exhausts the region in the drain
NoExhaust == ~exhausted

\* every ordering comparison agrees with the comparison modulo the wire
CompareAgrees == ~disagree

\* every held ordering is ahead of the read index by 1 .. W-1
HeldAhead == \A h \in held : U24(h - read) \in 1..W-1

\* a packet is deferred only when the deliveries its record can make do not fit
NeverDeferWithRoom == ~wasted
=============================================================================
