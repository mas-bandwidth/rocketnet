------------------------- MODULE RNCookieReplyLimits -------------------------
(***************************************************************************)
(* The cookie reply limit of one server update, at the code's layout      *)
(* (sdk/rocketnet_server_protocol.cpp, the !cookie_ok branch of the cookie *)
(* gate in rocketnet_server_protocol_process_connection_request, and the   *)
(* clear in rocketnet_server_protocol_update; the bitmap's size from       *)
(* rocketnet_server_cookie_reply_source_words in                          *)
(* sdk/rocketnet_server_protocol.h and its carve in                        *)
(* sdk/rocketnet_server_transport.cpp; standard/TRANSPORT.md t12 rule 7).  *)
(* A request owed a fresh cookie is answered only when its source address  *)
(* and port has not had one this update: the bit at                       *)
(* Index(s) = SipHash(address bytes, key) & (AllocBits - 1) of a bitmap of *)
(* AllocBits bits in words of WordBits, set by the reply and cleared,      *)
(* every allocated word, by the update.  There is no limit on one         *)
(* update's cookie replies in all but the requests the update processes,   *)
(* at most Bound (the receive bound of the code).  The update also clears *)
(* the handshake work budget, which a cookie-valid request spends and the *)
(* cookie limit never touches.                                             *)
(*                                                                         *)
(* The allocation: AllocBits is the next power of two at or above Factor  *)
(* times Bound, in whole words (16 and the io_uring receive bank in the    *)
(* code), so the bits set in one update, at most one per request, are at   *)
(* most 1 in Factor of the bitmap.                                         *)
(*                                                                         *)
(* The key is drawn at create: hash is chosen once, any function of the    *)
(* sources into a range wider than the bitmap, so every key is explored    *)
(* and the mask is load-bearing.                                           *)
(*                                                                         *)
(*   CookieRequest(s)  a request from s owed a cookie (no cookie, a forged *)
(*                     one or an expired one): the source bit, then the    *)
(*                     reply.  A request its bit refuses is ignored; it is *)
(*                     counted only by its cookie's reason, as every such  *)
(*                     request is, and the refusal has no counter.         *)
(*   ValidRequest      a request with a valid cookie reaching the handshake *)
(*                     work budget (handshakes_processed_this_update).     *)
(*   Update            rocketnet_server_protocol_update's reset.           *)
(*                                                                         *)
(* Broken selects a reversed witness:                                      *)
(*   "nobit"      no per-source bit: one source gets two replies in one   *)
(*                update (OneReplyPerSource);                              *)
(*   "nomask"     the index is the hash unmasked, outside the allocated    *)
(*                bitmap (IndexInRange);                                   *)
(*   "shortclear" the update clears every allocated word but the last, so *)
(*                a bit from an earlier update refuses a source no reply  *)
(*                of this update explains (RefusalHasCause);               *)
(*   "unsized"    the bitmap is sized from Bound without Factor, so one   *)
(*                full update sets more than 1 in Factor of its bits       *)
(*                (FalseRefusalBound);                                     *)
(*   "shared"     cookie replies spend the handshake work budget, so a    *)
(*                cookie-valid request is refused before VerifyBudget      *)
(*                verifies ran (WorkBudgetOnlyVerifies).                   *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Sources,        \* source address-and-port values
    Bound,          \* the most connection requests one update processes
    Factor,         \* bitmap bits per request of Bound (16 in the code)
    WordBits,       \* bits per word (64 in the code)
    HashRange,      \* hash values before the mask, wider than the bitmap
    VerifyBudget,   \* handshakes_per_update
    Broken          \* "none", or the reversed witness

ASSUME /\ Bound \in Nat \ {0} /\ Factor \in Nat \ {0} /\ VerifyBudget \in Nat \ {0}
       /\ WordBits \in Nat \ {0}
       /\ HashRange \in Nat
       /\ Broken \in {"none", "nobit", "nomask", "shortclear", "unsized", "shared"}

\* rocketnet_server_cookie_reply_source_words: the next power of two at or
\* above the bits needed, never below one word
NextPow2(n) == CHOOSE p \in {WordBits * 2^k : k \in 0..20} :
                   p >= n /\ (p = WordBits \/ p \div 2 < n)
AllocBits == NextPow2(IF Broken = "unsized" THEN Bound ELSE Factor * Bound)
Words == AllocBits \div WordBits

ASSUME HashRange > AllocBits

VARIABLES
    hash,           \* the keyed hash of each source, fixed at create
    words,          \* cookie_reply_sources: the set bits of each word
    processed,      \* ghost: connection requests this update processed
    sent,           \* ghost: cookie replies to each source this update
    bitRefused,     \* ghost: sources refused by their bit this update
    verifies,       \* handshakes_processed_this_update
    verified,       \* ghost: requests actually verified this update
    validRefusedEarly   \* ghost: a valid request refused below VerifyBudget verifies

vars == << hash, words, processed, sent, bitRefused, verifies, verified, validRefusedEarly >>

Index(s) == IF Broken = "nomask" THEN hash[s] ELSE hash[s] % AllocBits
Word(i) == i \div WordBits
Bit(i) == i % WordBits
IsSet(i) == Word(i) \in 0..Words-1 /\ Bit(i) \in words[Word(i)]
SetBits == {i \in 0..AllocBits-1 : IsSet(i)}

Init ==
    /\ hash \in [Sources -> 0..HashRange-1]
    /\ words = [w \in 0..Words-1 |-> {}]
    /\ processed = 0
    /\ sent = [s \in Sources |-> 0]
    /\ bitRefused = {}
    /\ verifies = 0
    /\ verified = 0
    /\ validRefusedEarly = FALSE

CookieRequest(s) ==
    /\ processed < Bound
    /\ processed' = processed + 1
    /\ UNCHANGED << hash, verified, validRefusedEarly >>
    /\ IF Broken /= "nobit" /\ IsSet(Index(s))
       THEN /\ bitRefused' = bitRefused \cup {s}
            /\ UNCHANGED << words, sent, verifies >>
       ELSE /\ words' = IF Word(Index(s)) \in 0..Words-1
                        THEN [words EXCEPT ![Word(Index(s))] = @ \cup {Bit(Index(s))}]
                        ELSE words
            /\ sent' = [sent EXCEPT ![s] = @ + 1]
            /\ verifies' = IF Broken = "shared" THEN verifies + 1 ELSE verifies
            /\ UNCHANGED bitRefused

ValidRequest ==
    /\ processed < Bound
    /\ processed' = processed + 1
    /\ UNCHANGED << hash, words, sent, bitRefused >>
    /\ IF verifies >= VerifyBudget
       THEN /\ validRefusedEarly' = (validRefusedEarly \/ verified < VerifyBudget)
            /\ UNCHANGED << verifies, verified >>
       ELSE /\ verifies' = verifies + 1
            /\ verified' = verified + 1
            /\ UNCHANGED validRefusedEarly

Update ==
    /\ words' = [w \in 0..Words-1 |-> IF Broken = "shortclear" /\ w = Words - 1 THEN words[w] ELSE {}]
    /\ processed' = 0
    /\ sent' = [s \in Sources |-> 0]
    /\ bitRefused' = {}
    /\ verifies' = 0
    /\ verified' = 0
    /\ UNCHANGED << hash, validRefusedEarly >>

Next == \/ \E s \in Sources : CookieRequest(s)
        \/ ValidRequest
        \/ Update

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ words \in [0..Words-1 -> SUBSET (0..WordBits-1)]
    /\ processed \in 0..Bound /\ verifies \in Nat /\ verified \in Nat
    /\ bitRefused \subseteq Sources

\* The bit a source stands for lies inside the allocated bitmap, whatever the key.
IndexInRange == \A s \in Sources : Index(s) \in 0..AllocBits-1

\* No source gets a second cookie reply in one update: exact, whatever the
\* collisions, since a source's own reply sets its own bit.
OneReplyPerSource == \A s \in Sources : sent[s] <= 1

\* A source refused by its bit shares that bit with a source answered in this
\* same update: a collision costs the other source this update and no more.
RefusalHasCause ==
    \A s \in bitRefused : \E b \in Sources : sent[b] > 0 /\ Index(b) = Index(s)

\* The layout: the set bits are exactly the indices of this update's replies.
BitmapIsThisUpdate ==
    \A i \in 0..AllocBits-1 : IsSet(i) <=> \E b \in Sources : sent[b] > 0 /\ Index(b) = i

\* At most 1 in Factor of the bitmap is set in one update, so an honest
\* source's bit is already set with probability at most 1 in Factor.
FalseRefusalBound == Factor * Cardinality(SetBits) <= AllocBits

\* A cookie-valid request is refused only once VerifyBudget verifies ran.
WorkBudgetOnlyVerifies == ~validRefusedEarly

=============================================================================
