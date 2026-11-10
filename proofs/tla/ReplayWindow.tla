--------------------------- MODULE ReplayWindow ---------------------------
(***************************************************************************)
(* rocketnet: the replay window one encryption mapping keeps for one       *)
(* direction of session traffic.                                           *)
(*                                                                         *)
(* The state is struct rocketnet_replay_protection_t                       *)
(* (struct rocketnet_replay_protection_t in sdk/rocketnet_encryption_map.h): a high-water mark               *)
(* highest_opened and a ring of ROCKETNET_REPLAY_WINDOW_BITS bits, stored  *)
(* as ROCKETNET_HISTORY_PACKETS / 64 words of 64                           *)
(* (ROCKETNET_REPLAY_WINDOW_BITS in sdk/rocketnet_replay.h ties the two    *)
(* constants together).  Two operations own it,                            *)
(* sdk/rocketnet_replay.cpp: already_received (the check before the open)  *)
(* and advance (the mark after the open).  A third reads it: the ack block *)
(* builder, rocketnet_ack_block_build in sdk/rocketnet_ack.cpp, enumerates the same              *)
(* bits between the floor and the high-water mark, so what the window says *)
(* is what the sender is told was received (standard/TRANSPORT.md t14      *)
(* Replay protection and t15 Acknowledgement).                             *)
(*                                                                         *)
(* The window has one job: inside the retained range (highest - W,         *)
(* highest], a bit is set exactly for the sequences that have opened.  The *)
(* ghost `received` is that truth, trimmed to the range, and every rule    *)
(* below is the window agreeing with it.                                   *)
(*                                                                         *)
(* Two rules are the point of this model, each with a reversed witness in  *)
(* README-ReplayWindow.md that shows the broken order TLC must reject:     *)
(*                                                                         *)
(*   F1 (rnfix-replay-long-skip-clears-window-r2, rocketnet#189).  On a    *)
(*   skip of a full window or more, advance clears the whole ring:         *)
(*   standard/TRANSPORT.md t14 Replay protection says the receiver MUST    *)
(*   clear the whole ring when the forward jump is at least one window     *)
(*   depth.  The code clears the whole ring, ClearedWords = Words; the     *)
(*   witness sets ClearedWords < Words and TLC rejects it.                 *)
(*                                                                         *)
(*   OV-34 (rnfix-replay-advanced-after-connect-check-r2).  The server     *)
(*   advances the session window only for a mapping that holds a slot:     *)
(*   standard/TRANSPORT.md t10 step 12 and t14 Replay protection say the   *)
(*   server MUST NOT move the window of a session packet whose source      *)
(*   address has no assigned client slot.  The code checks the slot        *)
(*   before it advances, GateBeforeAdvance = TRUE; the witness sets        *)
(*   GateBeforeAdvance = FALSE and, since a gate before the open is also   *)
(*   one before the advance, GateBeforeOpen = FALSE (the open, the         *)
(*   advance, then the slot check), and TLC rejects it.                    *)
(*                                                                         *)
(* This is replay_session only.  The handshake window (replay_handshake)   *)
(* is the same struct, but handshake packets are exempt from replay        *)
(* marking and only raise its highest_opened                               *)
(* (rocketnet_server_process_sealed_packet in sdk/rocketnet_server_protocol.cpp;                           *)
(* rocketnet_client_process_challenge in sdk/rocketnet_client_protocol.cpp); rocketnet_replay_advance never runs on it,   *)
(* and nothing here speaks for it.                                         *)
(*                                                                         *)
(* A second ghost, processed, records what a slot actually processed: the  *)
(* harm the gate guards against is the ack block naming a packet nothing   *)
(* processed, and AckReportsOnlyProcessed says it never does.              *)
(*                                                                         *)
(* A third rule places the gate before the open (asr pass 1 F4, 2026-10-21;*)
(* TRANSPORT.md t10 step 12).  The server acts on no session packet for a  *)
(* mapping without a slot, so it must not spend an AEAD open on one: the   *)
(* ghost openedPending records an open attempted on a pending mapping, by  *)
(* a genuine packet (Receive) or by one that does not open (Forge), and    *)
(* NoOpenBeforeConnect says it never happens.  GateBeforeOpen = FALSE is   *)
(* the order before the fix (sdk/rocketnet_server_protocol.cpp: the open,  *)
(* then the slot check), and TLC must reject it.                           *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Words,             \* words in the ring: ROCKETNET_HISTORY_PACKETS / 64
    WordBits,          \* bits per word: 64 in the code
    ClearedWords,      \* words the long-skip branch clears; the rule, and
                       \* what the code does: Words (the whole ring)
    MaxSeq,            \* the highest sequence this instance explores
    GateBeforeAdvance, \* BOOLEAN: the server checks the slot before it
                       \* advances (the rule, and what the code does) or after
    GateBeforeOpen,    \* BOOLEAN: the server checks the slot before the
                       \* AEAD open (the rule, and what the code does) or after;
                       \* the advance follows the open, so a gate before the
                       \* open is a gate before the advance as well
    MarkOnAdvance,     \* BOOLEAN: the advance sets the bit for s (the rule,
                       \* and what the code does) or leaves the ring alone; the
                       \* ghost still records that s opened
    RefuseBelowFloor   \* BOOLEAN: a sequence at or below highest - W is refused
                       \* as too old (the rule, and what the code does) or the
                       \* too-old check is omitted and the ring speaks for it

W == Words * WordBits  \* ROCKETNET_REPLAY_WINDOW_BITS (sdk/rocketnet_replay.h)

ASSUME /\ Words \in Nat \ {0}
       /\ WordBits \in Nat \ {0}
       /\ ClearedWords \in 0..Words
       /\ MaxSeq \in Nat /\ MaxSeq >= W
       /\ GateBeforeAdvance \in BOOLEAN
       /\ GateBeforeOpen \in BOOLEAN
       /\ GateBeforeOpen => GateBeforeAdvance
       /\ MarkOnAdvance \in BOOLEAN
       /\ RefuseBelowFloor \in BOOLEAN

VARIABLES
    highest,    \* highest_opened: the high-water mark, 0 before any open
    window,     \* [0..W-1 -> BOOLEAN]: the ring bit for each sequence mod W
    received,   \* ghost: the sequences that opened and are still in range
    processed,  \* ghost: those of them a slot processed (not ignored)
    connected,  \* the mapping holds a slot (client_index != -1)
    openedPending \* ghost: an open was attempted while connected was FALSE

vars == <<highest, window, received, processed, connected, openedPending>>

Seqs == 1..MaxSeq

(***************************************************************************)
(* The ring.  A sequence lives at bit s % W (rocketnet_replay_bit    *)
(* and rocketnet_replay_set_bit in sdk/rocketnet_replay.cpp; rocketnet_ack_replay_bit in sdk/rocketnet_ack.cpp), and bit b lives in word b div 64.    *)
(***************************************************************************)
Bit(s)  == s % W
Word(b) == b \div WordBits

(***************************************************************************)
(* The retained range.  already_received refuses s <= highest - W as too   *)
(* old (rocketnet_replay_already_received in sdk/rocketnet_replay.cpp); the ack builder's floor is        *)
(* highest - W + 1, never below 1 (rocketnet_ack_block_build in sdk/rocketnet_ack.cpp).  The two  *)
(* agree: the range is Floor..highest, W sequences wide once highest >= W. *)
(***************************************************************************)
Floor == IF highest >= W THEN highest - W + 1 ELSE 1

InRange(s) == s >= Floor /\ s <= highest

(***************************************************************************)
(* rocketnet_replay_already_received (sdk/rocketnet_replay.cpp).     *)
(* Above the high-water mark: not received.  Below the range: refused as   *)
(* too old, indistinguishable from received.  Inside: the ring bit.        *)
(***************************************************************************)
AlreadyReceived(s) ==
    IF s > highest THEN FALSE
    ELSE IF highest >= W /\ RefuseBelowFloor /\ s <= highest - W THEN TRUE
    ELSE window[Bit(s)]

(***************************************************************************)
(* What the ack block says about s (rocketnet_ack_block_build in sdk/rocketnet_ack.cpp): the     *)
(* builder walks from largest = highest down to the floor reading the      *)
(* same ring bit, so a sequence is reported received exactly when it is    *)
(* in range and its bit is set.  Truncation (max_ranges, the 0xFFFF run    *)
(* caps) only loses the oldest information and is not modelled.            *)
(***************************************************************************)
AckReports(s) == InRange(s) /\ window[Bit(s)]

(***************************************************************************)
(* rocketnet_replay_advance (sdk/rocketnet_replay.cpp).  Above the         *)
(* high-water mark the entries the advance skips past are cleared first,   *)
(* so a sequence far ahead does not inherit bits from the previous lap:    *)
(* a skip shorter than W clears each skipped sequence's bit; a skip of W   *)
(* or more clears the whole ring (ClearedWords = Words).  Then the bit for *)
(* s is set when MarkOnAdvance is TRUE.                                    *)
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
    /\ ~AlreadyReceived(s)                  \* the assert in rocketnet_replay_advance; the caller checked
    /\ IF s > highest
         THEN /\ window'  = IF MarkOnAdvance
                            THEN [Cleared(s) EXCEPT ![Bit(s)] = TRUE]
                            ELSE Cleared(s)
              /\ highest' = s
         ELSE /\ window'  = IF MarkOnAdvance
                            THEN [window EXCEPT ![Bit(s)] = TRUE]
                            ELSE window
              /\ highest' = highest
    /\ received' = {t \in received \cup {s} : t + W > highest'}

Init ==
    /\ highest = 0                          \* a new mapping is zeroed:
    /\ window = [b \in 0..W-1 |-> FALSE]    \* rocketnet_encryption_map_create in sdk/rocketnet_encryption_map.cpp
    /\ received = {}
    /\ processed = {}
    /\ connected = FALSE                    \* client_index is -1 until the response
    /\ openedPending = FALSE

(***************************************************************************)
(* The operations and the outside events.                                  *)
(***************************************************************************)

\* A session packet with sequence s arrives at the server.  The
\* already-received check refuses a duplicate or a too-old sequence before
\* the open, so an opened packet is never one the window refuses.  The code
\* checks the slot before the open and before the advance, so a packet on a
\* pending mapping leaves the window alone (GateBeforeAdvance = TRUE,
\* GateBeforeOpen = TRUE).  The client is the same pair on a window that is
\* always connected.
\* The packet is opened only when the slot gate lets it through: always on
\* a mapping with a slot, and on a pending one only when the gate follows
\* the open (GateBeforeOpen = FALSE).  A packet that is never opened never
\* reaches the advance, whatever GateBeforeAdvance says.
Opens == connected \/ ~GateBeforeOpen

Receive(s) ==
    /\ s \in Seqs
    /\ ~AlreadyReceived(s)
    /\ IF connected \/ (Opens /\ ~GateBeforeAdvance)
         THEN Advance(s)
         ELSE UNCHANGED <<highest, window, received>>
    /\ processed' = {t \in (IF connected THEN processed \cup {s} ELSE processed) :
                        t + W > highest'}
    /\ openedPending' = (openedPending \/ (Opens /\ ~connected))
    /\ UNCHANGED connected

\* A sealed packet with sequence s that does not open (forged, or sealed
\* under other keys).  It passes the checks before the open; the open, if
\* the gate lets it run, fails, and nothing about the window changes.  The
\* integrity-failure count it would add is not modelled; openedPending is
\* the open being attempted at all.
Forge(s) ==
    /\ s \in Seqs
    /\ ~AlreadyReceived(s)
    /\ openedPending' = (openedPending \/ (Opens /\ ~connected))
    /\ UNCHANGED <<highest, window, received, processed, connected>>

\* The connection response opens and the mapping takes a slot
\* (rocketnet_server_process_connection_response in sdk/rocketnet_server_protocol.cpp).  Once only; a mapping that
\* loses its slot is removed, and a new one starts from Init.
Connect ==
    /\ ~connected
    /\ connected' = TRUE
    /\ UNCHANGED <<highest, window, received, processed, openedPending>>

Next ==
    \/ \E s \in Seqs : Receive(s)
    \/ \E s \in Seqs : Forge(s)
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
    /\ openedPending \in BOOLEAN

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

\* Within the window a sequence is accepted exactly once (standard/TRANSPORT.md,
\* t14 Replay protection): everything that opened is refused thereafter.
ReceivedIsRefused ==
    \A s \in received : AlreadyReceived(s)

\* Below the window everything is refused as too old; an undersized window
\* discards old legitimate packets, it never accepts a replay
\* (standard/TRANSPORT.md t14 Replay protection, and the replay window depth
\* rows of t3).
BelowFloorIsRefused ==
    \A s \in Seqs : s < Floor => AlreadyReceived(s)

\* The high-water mark is itself received (the assert at
\* rocketnet_ack_block_build in sdk/rocketnet_ack.cpp: largest is always marked).
HighestIsReceived ==
    highest > 0 => highest \in received /\ window[Bit(highest)]

\* OV-34: the session window of a mapping with no slot never moves.
NoAdvanceBeforeConnect ==
    ~connected => highest = 0 /\ received = {}

\* The harm behind OV-34: the ack block names only packets a slot processed.
\* A sequence marked on a pending mapping and then ignored would be reported
\* to the sender as received although nothing processed it.
AckReportsOnlyProcessed ==
    \A s \in Seqs : AckReports(s) => s \in processed

\* F4 (asr pass 1, 2026-10-21): no AEAD open is ever attempted on a mapping
\* without a slot -- the slot gate runs before decryption.
NoOpenBeforeConnect ==
    ~openedPending

\* The high-water mark never goes backwards, and a slot is never given up.
Monotone ==
    [][highest' >= highest /\ (connected => connected')]_vars

=============================================================================
