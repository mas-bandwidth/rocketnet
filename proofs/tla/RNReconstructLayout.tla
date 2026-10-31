------------------------- MODULE RNReconstructLayout -------------------------
(***************************************************************************)
(* The reconstruction of a 64-bit sequence from its 32-bit wire value at   *)
(* the code's arithmetic (sdk/rocketnet_protocol.cpp,                      *)
(* rocketnet_reconstruct_near; rocketnet_reconstruct_sequence reads the    *)
(* session sequence against highest_opened + 1, and                        *)
(* rocketnet_reconstruct_ack_largest in sdk/rocketnet_ack.cpp the ack      *)
(* block's largest against the highest sent).                              *)
(*                                                                         *)
(* The code takes base = expected with its low 32 bits cleared and        *)
(* compares three candidates sharing the wire value: base | wire, the one  *)
(* below (only when base >= 2^32) and the one above (base + 2^32, which    *)
(* wraps at 2^64), by unsigned distance to expected, the larger winning a  *)
(* tie.  Here the wire modulus is M and the 64-bit space M * K.            *)
(*                                                                         *)
(* AckBlockWrap.tla takes the reconstruction as the nearest of every       *)
(* candidate; CodeIsNearest says the three-candidate arithmetic, with its  *)
(* gate and its wrap, is that nearest for every expected and every wire    *)
(* value.                                                                  *)
(*                                                                         *)
(* Broken selects a reversed witness (CodeIsNearest fails for each):       *)
(*   "tiesmaller"  an exact tie goes to the smaller candidate;             *)
(*   "belowgate"   the candidate below is taken only when base > 2^32, so  *)
(*                 the first lap's base misses it;                         *)
(*   "noabove"     no candidate above is compared.                         *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS
    M,          \* the wire modulus: 2^32 in the code
    K,          \* the space is M * K: 2^64 in the code
    Broken      \* "none", or the reversed witness

ASSUME /\ M \in Nat /\ M >= 2 /\ K \in Nat /\ K >= 2
       /\ Broken \in {"none", "tiesmaller", "belowgate", "noabove"}

Space == M * K

VARIABLES expected, wire

vars == <<expected, wire>>

Dist(a, b) == IF a > b THEN a - b ELSE b - a

Better(c, d, best, bestD) ==
    \/ d < bestD
    \/ d = bestD /\ (IF Broken = "tiesmaller" THEN c < best ELSE c > best)

\* rocketnet_reconstruct_near
Code(w, e) ==
    LET base == e - (e % M)
        cand == base + w
        below == base - M + w
        useBelow == IF Broken = "belowgate" THEN base > M ELSE base >= M
        b1 == IF useBelow /\ Better(below, Dist(below, e), cand, Dist(cand, e)) THEN below ELSE cand
        above == ((base + M) % Space) + w
    IN IF Broken # "noabove" /\ Better(above, Dist(above, e), b1, Dist(b1, e)) THEN above ELSE b1

\* the nearest value in the space with these low bits, the larger on a tie
Nearest(w, e) ==
    LET C == {k * M + w : k \in 0..K-1}
    IN CHOOSE c \in C : \A d \in C : Dist(c, e) < Dist(d, e) \/ (Dist(c, e) = Dist(d, e) /\ c >= d)

Init == expected \in 0..Space-1 /\ wire \in 0..M-1

Next == UNCHANGED vars

Spec == Init /\ [][Next]_vars

TypeOK == expected \in 0..Space-1 /\ wire \in 0..M-1

CodeIsNearest == Code(wire, expected) = Nearest(wire, expected)

\* a value within half the modulus of expected reads back as itself
RoundTrip ==
    \A s \in 0..Space-1 : 2 * Dist(s, expected) < M => Code(s % M, expected) = s

=============================================================================
