------------------------- MODULE RNCookieReplyLimits -------------------------
(***************************************************************************)
(* The cookie reply limits of one server update, at the code's layout      *)
(* (sdk/rocketnet_server_protocol.cpp, the !cookie_ok branch of the cookie *)
(* gate in rocketnet_server_protocol_process_connection_request, and the   *)
(* reset in rocketnet_server_protocol_update; standard/TRANSPORT.md t12    *)
(* rule 7).  A request owed a fresh cookie is answered only while the      *)
(* update's cookie replies are under max_clients, and only when its source *)
(* address and port has not had one this update: the bit at               *)
(* Index(s) = SipHash(address bytes, key) & (Bits - 1) of a fixed bitmap  *)
(* of Bits bits in words of WordBits (16384 and 64 in the code), set by    *)
(* the reply and cleared, every word, by the update.  The update also      *)
(* clears the handshake work budget, which a cookie-valid request spends   *)
(* and the cookie limits never touch.                                      *)
(*                                                                         *)
(* The key is drawn at create: hash is chosen once, any function of the    *)
(* sources into a range wider than the bitmap, so every key is explored    *)
(* and the mask is load-bearing.                                           *)
(*                                                                         *)
(*   CookieRequest(s)  a request from s owed a cookie (no cookie, a forged *)
(*                     one or an expired one): the global cap, then the    *)
(*                     source bit, then the reply.                         *)
(*   ValidRequest      a request with a valid cookie reaching the handshake *)
(*                     work budget (handshakes_processed_this_update).     *)
(*   Update            rocketnet_server_protocol_update's reset.           *)
(*                                                                         *)
(* Broken selects a reversed witness:                                      *)
(*   "nobit"      no per-source bit: one source gets two replies in one   *)
(*                update (OneReplyPerSource);                              *)
(*   "noglobal"   no max_clients cap (GlobalCapHeld);                     *)
(*   "nomask"     the index is the hash unmasked, outside the bitmap       *)
(*                (IndexInRange);                                          *)
(*   "shortclear" the update clears every word but the last, so a bit     *)
(*                from an earlier update refuses a source no reply of     *)
(*                this update explains (RefusalHasCause);                  *)
(*   "shared"     cookie replies spend the handshake work budget, so a    *)
(*                cookie-valid request is refused before VerifyBudget      *)
(*                verifies ran (WorkBudgetOnlyVerifies).                   *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Sources,        \* source address-and-port values
    MaxClients,     \* the server's max_clients: the update's cookie replies
    Bits,           \* bitmap bits, a power of two (16384 in the code)
    WordBits,       \* bits per word (64 in the code)
    HashRange,      \* hash values before the mask, wider than Bits
    VerifyBudget,   \* handshakes_per_update
    Broken          \* "none", or the reversed witness

ASSUME /\ MaxClients \in Nat \ {0} /\ VerifyBudget \in Nat \ {0}
       /\ Bits \in Nat \ {0} /\ WordBits \in Nat \ {0} /\ Bits % WordBits = 0
       /\ HashRange \in Nat /\ HashRange > Bits
       /\ Broken \in {"none", "nobit", "noglobal", "nomask", "shortclear", "shared"}

Words == Bits \div WordBits

VARIABLES
    hash,           \* the keyed hash of each source, fixed at create
    words,          \* cookie_reply_sources: the set bits of each word
    replies,        \* cookie_replies_this_update
    sent,           \* ghost: cookie replies to each source this update
    bitRefused,     \* ghost: sources refused by their bit this update
    verifies,       \* handshakes_processed_this_update
    verified,       \* ghost: requests actually verified this update
    validRefusedEarly   \* ghost: a valid request refused below VerifyBudget verifies

vars == << hash, words, replies, sent, bitRefused, verifies, verified, validRefusedEarly >>

Index(s) == IF Broken = "nomask" THEN hash[s] ELSE hash[s] % Bits
Word(i) == i \div WordBits
Bit(i) == i % WordBits
IsSet(i) == Word(i) \in 0..Words-1 /\ Bit(i) \in words[Word(i)]

Init ==
    /\ hash \in [Sources -> 0..HashRange-1]
    /\ words = [w \in 0..Words-1 |-> {}]
    /\ replies = 0
    /\ sent = [s \in Sources |-> 0]
    /\ bitRefused = {}
    /\ verifies = 0
    /\ verified = 0
    /\ validRefusedEarly = FALSE

CookieRequest(s) ==
    /\ UNCHANGED << hash, verified, validRefusedEarly >>
    /\ IF Broken /= "noglobal" /\ replies >= MaxClients
       THEN UNCHANGED << words, replies, sent, bitRefused, verifies >>
       ELSE IF Broken /= "nobit" /\ IsSet(Index(s))
       THEN /\ bitRefused' = bitRefused \cup {s}
            /\ UNCHANGED << words, replies, sent, verifies >>
       ELSE /\ words' = [words EXCEPT ![Word(Index(s))] = @ \cup {Bit(Index(s))}]
            /\ replies' = replies + 1
            /\ sent' = [sent EXCEPT ![s] = @ + 1]
            /\ verifies' = IF Broken = "shared" THEN verifies + 1 ELSE verifies
            /\ UNCHANGED bitRefused

ValidRequest ==
    /\ UNCHANGED << hash, words, replies, sent, bitRefused >>
    /\ IF verifies >= VerifyBudget
       THEN /\ validRefusedEarly' = (validRefusedEarly \/ verified < VerifyBudget)
            /\ UNCHANGED << verifies, verified >>
       ELSE /\ verifies' = verifies + 1
            /\ verified' = verified + 1
            /\ UNCHANGED validRefusedEarly

Update ==
    /\ words' = [w \in 0..Words-1 |-> IF Broken = "shortclear" /\ w = Words - 1 THEN words[w] ELSE {}]
    /\ replies' = 0
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
    /\ replies \in Nat /\ verifies \in Nat /\ verified \in Nat
    /\ bitRefused \subseteq Sources

\* The bit a source stands for lies inside the bitmap, whatever the key.
IndexInRange == \A s \in Sources : Index(s) \in 0..Bits-1

\* No source gets a second cookie reply in one update: exact, whatever the
\* collisions, since a source's own reply sets its own bit.
OneReplyPerSource == \A s \in Sources : sent[s] <= 1

\* The update's cookie replies never pass max_clients.
GlobalCapHeld == replies <= MaxClients

\* A source refused by its bit shares that bit with a source answered in this
\* same update: a collision costs the other source this update and no more.
RefusalHasCause ==
    \A s \in bitRefused : \E b \in Sources : sent[b] > 0 /\ Index(b) = Index(s)

\* The layout: the set bits are exactly the indices of this update's replies.
BitmapIsThisUpdate ==
    \A i \in 0..Bits-1 : IsSet(i) <=> \E b \in Sources : sent[b] > 0 /\ Index(b) = i

\* A cookie-valid request is refused only once VerifyBudget verifies ran.
WorkBudgetOnlyVerifies == ~validRefusedEarly

=============================================================================
