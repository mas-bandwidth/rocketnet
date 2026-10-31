--------------------------- MODULE RNWindowLayout ---------------------------
(***************************************************************************)
(* The receive window of one connection as laid out in                     *)
(* sdk/rocketnet_window_receive.cpp, with the code's own arithmetic.       *)
(*                                                                         *)
(* rn_window_receive_accept reads a record's wire number (24 bits; here    *)
(* WireMod), reconstructs it against R by the signed half range, refuses   *)
(* a number behind R as a duplicate and one at or past R + W as a          *)
(* violation, finds its slot index = number & (W - 1), tests the received  *)
(* bit at word index >> 6, bit index & 63, refuses a set bit as a          *)
(* duplicate and a live descriptor of another number as SLOT_COLLISION,    *)
(* claims the descriptor (live = 1) and sets the bit, then advances R      *)
(* past every set floor bit, clearing each one as R passes it.  A          *)
(* violation is sticky: every later accept returns VIOLATION at once.      *)
(* Release is the library's descriptor release, live = 0 when the number's *)
(* delivery or hold ends (rn_channels_release, rn_sequenced_release_slot,  *)
(* rn_sequenced_release_number, rn_window_receive_release).                *)
(*                                                                         *)
(* RNWindow.tla checks the protocol around this (the sender's window, P6,  *)
(* U'), with the received bits as a set.  Here the bits are the word array *)
(* and the descriptors the slot array.  Arrivals are of two kinds:         *)
(*   honest: the full number n is known to the model; n lies in            *)
(*     R - WireMod/2 .. R + W - 1 (a duplicate as old as the half range,   *)
(*     or a number inside the window), and never names a slot whose        *)
(*     descriptor is live with another number: RNWindow's NoSlotCollision  *)
(*     is the protocol's guarantee of that, taken here as an assumption;   *)
(*   forged: any wire value at all, from a sender that breaks the protocol.*)
(*                                                                         *)
(* R is the code's uint64; its UINT64_MAX guard is unreachable and not     *)
(* modelled.  The ghost state: above, the accepted numbers at or past R;   *)
(* every number below R was accepted (NoSilentLoss checks that each one R  *)
(* passes was), so the accepted set is 0..R-1 \cup above.                  *)
(*                                                                         *)
(* Reversed witnesses, each a rule of the code turned off:                 *)
(*   ClearFloor = FALSE     the advance does not clear the floor bit;      *)
(*   HalfRange = FALSE      the reconstruction has no half-range sign;     *)
(*   CollisionTest = FALSE  the live-descriptor test is dropped.           *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    W,              \* the window, a power of two
    WireMod,        \* the wire number's modulus (2^24 in the code), a multiple of W, >= 2W
    MaxR,           \* the state constraint bounds R
    ClearFloor,     \* BOOLEAN: the advance clears each floor bit it passes
    HalfRange,      \* BOOLEAN: the reconstruction is signed by half range
    CollisionTest   \* BOOLEAN: a live descriptor of another number refuses the record

ASSUME W >= 1 /\ WireMod % W = 0 /\ WireMod >= 2 * W

NONE == -1
Slots == 0..W-1
Words == 0..((W - 1) \div 64)
Half == WireMod \div 2

VARIABLES
    R,          \* receive->R: the lowest number not yet received
    words,      \* region->received_bits: word -> the set bit positions in it
    desc,       \* region->receive_descriptors: slot -> [live, number]
    violation,  \* receive->violation # NONE (sticky)
    \* ghost
    above,      \* the accepted numbers at or past R
    twice,      \* a number was accepted a second time
    lost,       \* an honest fresh number was refused as a duplicate, or R passed a number never accepted
    misread,    \* an honest number was refused as a violation or reconstructed as another number
    overwrote,  \* a claim wrote over a live descriptor of another number
    pastWindow  \* a number at or past R + W was accepted

vars == <<R, words, desc, violation, above, twice, lost, misread, overwrote, pastWindow>>

WordOf(i) == i \div 64
BitOf(i)  == i % 64
BitSet(ws, i) == BitOf(i) \in ws[WordOf(i)]
SetBit(ws, i)   == [ws EXCEPT ![WordOf(i)] = @ \cup {BitOf(i)}]
ClearBit(ws, i) == [ws EXCEPT ![WordOf(i)] = @ \ {BitOf(i)}]

\* number & (W - 1): W is a power of two (asserted in rn_window_receive_init)
Index(n) == n % W

\* lines 47-50: the wire value against R, signed by half range
Delta(w) ==
    LET ud == (w - (R % WireMod)) % WireMod
    IN  IF HalfRange /\ ud >= Half THEN ud - WireMod ELSE ud

\* lines 99-109: R advances past each set floor bit, clearing it.  The
\* fuel bounds the loop; with ClearFloor the set bits are at most W.
RECURSIVE Advance(_, _, _)
Advance(r, ws, fuel) ==
    IF fuel = 0 \/ ~BitSet(ws, Index(r))
      THEN <<r, ws>>
      ELSE Advance(r + 1, IF ClearFloor THEN ClearBit(ws, Index(r)) ELSE ws, fuel - 1)

Received == (0..R-1) \cup above

TypeOK ==
    /\ R \in Nat
    /\ words \in [Words -> SUBSET (0..63)]
    /\ desc \in [Slots -> [live : BOOLEAN, number : Nat]]
    /\ violation \in BOOLEAN
    /\ above \subseteq Nat
    /\ {twice, lost, misread, overwrote, pastWindow} \subseteq BOOLEAN

Init ==
    /\ R = 0
    /\ words = [k \in Words |-> {}]
    /\ desc = [s \in Slots |-> [live |-> FALSE, number |-> 0]]
    /\ violation = FALSE
    /\ above = {}
    /\ twice = FALSE /\ lost = FALSE /\ misread = FALSE
    /\ overwrote = FALSE /\ pastWindow = FALSE

\* rn_window_receive_accept on wire value w.  n is the honest number the
\* sender meant, or NONE for a forged record.
Accept(w, n) ==
    IF violation
      THEN UNCHANGED vars                                   \* lines 41-42
      ELSE LET d == Delta(w) IN
           IF d < 0
             THEN \* lines 55-56: behind R, a duplicate
                  /\ lost' = (lost \/ (n # NONE /\ n \notin Received))
                  /\ UNCHANGED <<R, words, desc, violation, above, twice, misread, overwrote, pastWindow>>
           ELSE IF d >= W
             THEN \* lines 58-62: outside the window
                  /\ violation' = TRUE
                  /\ misread' = (misread \/ n # NONE)
                  /\ UNCHANGED <<R, words, desc, above, twice, lost, overwrote, pastWindow>>
           ELSE
             LET num == R + d
                 i   == Index(num)
             IN
             IF BitSet(words, i)
               THEN \* lines 65-69: the received bit is set, a duplicate
                    /\ lost' = (lost \/ (n # NONE /\ n \notin Received))
                    /\ misread' = (misread \/ (n # NONE /\ n # num))
                    /\ UNCHANGED <<R, words, desc, violation, above, twice, overwrote, pastWindow>>
             ELSE IF CollisionTest /\ desc[i].live /\ desc[i].number # num
               THEN \* lines 71-76: SLOT_COLLISION
                    /\ violation' = TRUE
                    /\ misread' = (misread \/ n # NONE)
                    /\ UNCHANGED <<R, words, desc, above, twice, lost, overwrote, pastWindow>>
             ELSE
               \* lines 82-93: claim, set the bit; lines 99-109: advance
               LET ws1  == SetBit(words, i)
                   adv  == Advance(R, ws1, W + 1)
                   R1   == adv[1]
                   got  == above \cup {num}
               IN
               /\ desc' = [desc EXCEPT ![i] = [live |-> TRUE, number |-> num]]
               /\ R' = R1
               /\ words' = adv[2]
               /\ above' = {x \in got : x >= R1}
               /\ twice' = (twice \/ num \in Received)
               /\ overwrote' = (overwrote \/ (desc[i].live /\ desc[i].number # num))
               /\ pastWindow' = (pastWindow \/ num >= R + W)
               /\ misread' = (misread \/ (n # NONE /\ n # num))
               /\ lost' = (lost \/ \E x \in R..R1-1 : x \notin got)
               /\ UNCHANGED violation

\* An honest arrival: a duplicate as old as the half range, or a number in
\* the window whose slot is not live with another number (NoSlotCollision
\* of RNWindow.tla).
HonestNumbers ==
    {m \in ((R - Half)..(R + W - 1)) :
        /\ m >= 0
        /\ (m >= R => ~(desc[Index(m)].live /\ desc[Index(m)].number # m))}

\* the library releases a descriptor when its delivery or hold ends
Release(s) ==
    /\ desc[s].live
    /\ desc' = [desc EXCEPT ![s].live = FALSE]
    /\ UNCHANGED <<R, words, violation, above, twice, lost, misread, overwrote, pastWindow>>

Next ==
    \/ \E n \in HonestNumbers : Accept(n % WireMod, n)
    \/ \E w \in 0..WireMod-1 : Accept(w, NONE)
    \/ \E s \in Slots : Release(s)

Spec == Init /\ [][Next]_vars

Bound == R <= MaxR

(***************************************************************************)
(* Invariants.                                                             *)
(***************************************************************************)

\* a number is accepted exactly once
AcceptedOnce == ~twice

\* never a number at or past R + W
NeverPastWindow == ~pastWindow

\* every number R passes was accepted, and an honest fresh number is never
\* refused as a duplicate
NoSilentLoss == ~lost

\* an honest number is reconstructed as itself and never refused as a violation
HonestReadExactly == ~misread

\* the bit array is the set of received numbers R has not yet passed
BitsAreReceived ==
    /\ above \subseteq (R + 1)..(R + W - 1)
    /\ \A i \in Slots : BitSet(words, i) <=> (\E x \in above : Index(x) = i)
    /\ \A k \in Words : words[k] \subseteq {BitOf(i) : i \in {s \in Slots : WordOf(s) = k}}

\* a live descriptor is never overwritten by another number
NoLiveOverwrite == ~overwrote

\* a live descriptor names a received number at its own slot, inside the window
LiveNamesWindow ==
    \A s \in Slots : desc[s].live =>
        /\ Index(desc[s].number) = s
        /\ desc[s].number < R + W
        /\ desc[s].number \in Received
=============================================================================
