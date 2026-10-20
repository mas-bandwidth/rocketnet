--------------------------- MODULE ReplayWindow ---------------------------
(***************************************************************************)
(* rocketnet: the replay window one encryption mapping keeps for one       *)
(* direction of session traffic.                                           *)
(*                                                                         *)
(* The state is struct rocketnet_replay_protection_t                       *)
(* (sdk/rocketnet_encryption_map.h): a high-water mark highest_opened and  *)
(* a ring of ROCKETNET_REPLAY_WINDOW_BITS bits, stored as                  *)
(* ROCKETNET_HISTORY_PACKETS / 64 words of 64 (the static_assert beside    *)
(* ROCKETNET_REPLAY_WINDOW_BITS in sdk/rocketnet_replay.h ties the two     *)
(* constants together).  Two operations own it, in                         *)
(* sdk/rocketnet_replay.cpp: rocketnet_replay_already_received (the check  *)
(* before the open) and rocketnet_replay_advance (the mark after the       *)
(* open).  A third reads it: the ack block builder in sdk/rocketnet_ack.cpp *)
(* (rocketnet_ack_replay_bit and rocketnet_ack_block_build) enumerates the *)
(* same bits between the floor and the high-water mark, so what the window *)
(* says is what the peer is told was received (STANDARD.md, Replay         *)
(* Protection and Acks).                                                   *)
(*                                                                         *)
(* The window has one job: inside the retained range (highest - W,         *)
(* highest], a bit is set exactly for the sequences that have opened.  The *)
(* ghost `received` is that truth, trimmed to the range, and every rule    *)
(* below is the window agreeing with it.                                   *)
(*                                                                         *)
(* This model checks two rules from fix cards.  The committed instances    *)
(* use the current code's values; MCReplayWindowBrokenLongSkip.cfg is the  *)
(* reversed witness for F1 (fixed by commit                                *)
(* 5ab9e5c8ea4d31d385d19e5d3178e38ff6591d27).  OV-34 has no committed      *)
(* reversed config: GateBeforeAdvance = FALSE is the order it would check. *)
(*                                                                         *)
(*   F1: on a skip of a full window or more, the current implementation   *)
(*   clears the whole ring in rocketnet_replay_advance. The reversed        *)
(*   witness sets ClearedWords < Words and recreates the fixed defect.     *)
(*                                                                         *)
(*   OV-34 (rnfix-replay-advanced-after-connect-check-r2).  The server     *)
(*   advances the session window only for a mapping that holds a slot.    *)
(*   The code checks client_index before rocketnet_replay_advance in       *)
(*   sdk/rocketnet_server_protocol.cpp. GateBeforeAdvance = TRUE models   *)
(*   the current order; FALSE would be the reversed order.                *)
(*                                                                         *)
(* This is replay_session only.  The handshake window (replay_handshake)  *)
(* is the same struct, but handshake packets are exempt from replay        *)
(* marking and only raise its highest_opened (the connection response    *)
(* branch of rocketnet_server_process_sealed_packet, and                   *)
(* rocketnet_client_process_challenge); advance never runs on it,          *)
(* and nothing here speaks for it.                                         *)
(*                                                                         *)
(* A second ghost, processed, records what a slot actually processed: the  *)
(* harm the gate guards against is the ack block naming a packet nothing   *)
(* processed, and AckReportsOnlyProcessed says it never does.              *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Words,             \* words in the ring: ROCKETNET_HISTORY_PACKETS / 64
    WordBits,          \* bits per word: 64 in the code
    ClearedWords,      \* words the long-skip branch clears; current code uses Words.
    MaxSeq,            \* the highest sequence this instance explores
    GateBeforeAdvance  \* BOOLEAN: check the slot before advance (current code)
                       \* or after advance (the reversed order)

W == Words * WordBits  \* ROCKETNET_REPLAY_WINDOW_BITS (sdk/rocketnet_replay.h)

ASSUME /\ Words \in Nat \ {0}
       /\ WordBits \in Nat \ {0}
       /\ ClearedWords \in 0..Words
       /\ MaxSeq \in Nat /\ MaxSeq >= W
       /\ GateBeforeAdvance \in BOOLEAN

VARIABLES
    highest,    \* highest_opened: the high-water mark, 0 before any open
    window,     \* [0..W-1 -> BOOLEAN]: the ring bit for each sequence mod W
    received,   \* ghost: the sequences that opened and are still in range
    processed,  \* ghost: those of them a slot processed (not ignored)
    connected   \* the mapping holds a slot (client_index != -1)

vars == <<highest, window, received, processed, connected>>

Seqs == 1..MaxSeq

(***************************************************************************)
(* The ring.  A sequence lives at bit s % W (rocketnet_replay_bit,        *)
(* rocketnet_replay_set_bit and rocketnet_replay_clear_bit in              *)
(* sdk/rocketnet_replay.cpp; rocketnet_ack_replay_bit in                   *)
(* sdk/rocketnet_ack.cpp), and bit b lives in word b div 64.               *)
(***************************************************************************)
Bit(s)  == s % W
Word(b) == b \div WordBits

(***************************************************************************)
(* The retained range.  already_received refuses s <= highest - W as too   *)
(* old (rocketnet_replay_already_received); the ack builder's floor is     *)
(* highest - W + 1, never below 1 (floor_sequence in                       *)
(* rocketnet_ack_block_build).  The two                                    *)
(* agree: the range is Floor..highest, W sequences wide once highest >= W. *)
(***************************************************************************)
Floor == IF highest >= W THEN highest - W + 1 ELSE 1

InRange(s) == s >= Floor /\ s <= highest

(***************************************************************************)
(* rocketnet_replay_already_received (sdk/rocketnet_replay.cpp).          *)
(* Above the high-water mark: not received.  Below the range: refused as   *)
(* too old, indistinguishable from received.  Inside: the ring bit.        *)
(***************************************************************************)
AlreadyReceived(s) ==
    IF s > highest THEN FALSE
    ELSE IF highest >= W /\ s <= highest - W THEN TRUE
    ELSE window[Bit(s)]

(***************************************************************************)
(* What the ack block says about s (rocketnet_ack_block_build): the       *)
(* builder walks from largest = highest down to the floor reading the      *)
(* same ring bit, so a sequence is reported received exactly when it is    *)
(* in range and its bit is set.  Truncation (max_ranges, the 0xFFFF run    *)
(* caps) only loses the oldest information and is not modelled.            *)
(***************************************************************************)
AckReports(s) == InRange(s) /\ window[Bit(s)]

(***************************************************************************)
(* rocketnet_replay_advance (sdk/rocketnet_replay.cpp).  Above the        *)
(* high-water mark the entries the advance skips past are cleared first,   *)
(* so a sequence far ahead does not inherit bits from the previous lap: a  *)
(* skip shorter than W clears each skipped sequence's bit (its loop over   *)
(* rocketnet_replay_clear_bit); a skip of W or more clears words           *)
(* 0..ClearedWords-1, and the code clears the whole ring with memset, so   *)
(* the committed instance uses ClearedWords = Words.  Then the bit for s   *)
(* is set (rocketnet_replay_set_bit).                                      *)
(*                                                                         *)
(* The ghost follows the truth: s joins received, and whatever has left    *)
(* the retained range is forgotten -- the window can no longer speak for   *)
(* it and already_received refuses it as too old regardless.               *)
(***************************************************************************)
Cleared(s) ==
    LET skipped == s - highest
    IN  IF skipped >= W
          THEN [b \in 0..W-1 |-> IF Word(b) < ClearedWords THEN FALSE ELSE window[b]]
          ELSE [b \in 0..W-1 |-> IF \E t \in highest+1 .. s-1 : Bit(t) = b
                                   THEN FALSE ELSE window[b]]

Advance(s) ==
    /\ ~AlreadyReceived(s)                  \* advance asserts it; the caller checked
    /\ IF s > highest
         THEN /\ window'  = [Cleared(s) EXCEPT ![Bit(s)] = TRUE]
              /\ highest' = s
         ELSE /\ window'  = [window EXCEPT ![Bit(s)] = TRUE]
              /\ highest' = highest
    /\ received' = {t \in received \cup {s} : t + W > highest'}

Init ==
    /\ highest = 0                          \* a new mapping is zeroed:
    /\ window = [b \in 0..W-1 |-> FALSE]    \* memset in rocketnet_encryption_map_add
    /\ received = {}
    /\ processed = {}
    /\ connected = FALSE                    \* client_index is -1 until the response

(***************************************************************************)
(* The operations and the outside events.                                  *)
(***************************************************************************)

\* A session packet with sequence s arrives at the server and opens under
\* the session keys (rocketnet_server_process_sealed_packet in
\* sdk/rocketnet_server_protocol.cpp).  Its rocketnet_replay_already_received
\* check refuses a duplicate or a too-old sequence before the open, so an
\* opened packet is never one the window refuses.  After the open the code
\* ignores the packet when the mapping has no slot (client_index == -1) and
\* only then calls rocketnet_replay_advance, so a packet on a pending mapping
\* leaves the window alone: GateBeforeAdvance = TRUE.  The client
\* (rocketnet_client_open_session_packet and
\* rocketnet_client_consume_session_record in
\* sdk/rocketnet_client_protocol.cpp) is the same pair on a window that is
\* always connected.
Receive(s) ==
    /\ s \in Seqs
    /\ ~AlreadyReceived(s)
    /\ IF connected \/ ~GateBeforeAdvance
         THEN Advance(s)
         ELSE UNCHANGED <<highest, window, received>>
    /\ processed' = {t \in (IF connected THEN processed \cup {s} ELSE processed) :
                        t + W > highest'}
    /\ UNCHANGED connected

\* The connection response opens and the mapping takes a slot
\* (rocketnet_client_slots_assign in rocketnet_server_process_connection_response).
\* Once only; a mapping that
\* loses its slot is removed, and a new one starts from Init.
Connect ==
    /\ ~connected
    /\ connected' = TRUE
    /\ UNCHANGED <<highest, window, received, processed>>

Next ==
    \/ \E s \in Seqs : Receive(s)
    \/ Connect

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* The rules the design must keep.  Safety only: the window has no clock   *)
(* and makes no promise about the future, so there is no liveness here.    *)
(***************************************************************************)

TypeOK ==
    /\ highest \in 0..MaxSeq
    /\ window \in [0..W-1 -> BOOLEAN]
    /\ received \subseteq Seqs
    /\ processed \subseteq received
    /\ connected \in BOOLEAN

\* The window is the truth inside the retained range: a bit is set exactly
\* for the sequences that opened.  This is the rule behind F1: a long skip
\* that leaves words uncleared breaks it as soon as the stale bit's new
\* owner enters the range.
WindowIsTruth ==
    \A s \in Seqs : InRange(s) => (window[Bit(s)] <=> s \in received)

\* F1 as the card states it: a genuine packet inside the window that has
\* not been received is accepted, never refused as already received.
GenuineIsAccepted ==
    \A s \in Seqs : InRange(s) /\ s \notin received => ~AlreadyReceived(s)

\* F1's second half: the ack block names only sequences that opened.
AckReportsOnlyReceived ==
    \A s \in Seqs : AckReports(s) => s \in received

\* Within the window a sequence is accepted exactly once (STANDARD.md,
\* Replay Protection): everything that opened is refused thereafter.
ReceivedIsRefused ==
    \A s \in received : AlreadyReceived(s)

\* Below the window everything is refused as too old; an undersized window
\* discards old legitimate packets, it never accepts a replay (STANDARD.md,
\* Replay window sizing).
BelowFloorIsRefused ==
    \A s \in Seqs : s < Floor => AlreadyReceived(s)

\* The high-water mark is itself received (rocketnet_ack_block_build asserts
\* first_run >= 1: largest is always marked).
HighestIsReceived ==
    highest > 0 => highest \in received /\ window[Bit(highest)]

\* OV-34: the session window of a mapping with no slot never moves.
NoAdvanceBeforeConnect ==
    ~connected => highest = 0 /\ received = {}

\* The harm behind OV-34: the ack block names only packets a slot processed.
\* A sequence marked on a pending mapping and then ignored would be reported
\* to the peer as received although nothing processed it.
AckReportsOnlyProcessed ==
    \A s \in Seqs : AckReports(s) => s \in processed

\* The high-water mark never goes backwards, and a slot is never given up.
Monotone ==
    [][highest' >= highest /\ (connected => connected')]_vars

=============================================================================
