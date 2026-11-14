--------------------------- MODULE ReplayWindow ---------------------------
(***************************************************************************)
(* rocketnet: the replay window one encryption mapping keeps for one       *)
(* direction of session traffic, and the second window the ack block is    *)
(* built from.                                                             *)
(*                                                                         *)
(* The state is struct rocketnet_replay_protection_t                       *)
(* (sdk/rocketnet_encryption_map.h): a high-water mark highest_opened and  *)
(* a ring of ROCKETNET_REPLAY_WINDOW_BITS bits, stored as                  *)
(* ROCKETNET_HISTORY_PACKETS / 64 words of 64                              *)
(* (ROCKETNET_REPLAY_WINDOW_BITS in sdk/rocketnet_replay.h ties the two    *)
(* constants together).  Two operations own it, sdk/rocketnet_replay.cpp:  *)
(* already_received (the check) and advance (the mark).                    *)
(*                                                                         *)
(* A mapping keeps two of them.  replay_session is the security window:    *)
(* checked before the open and advanced right after it.  accepted_session *)
(* is what the endpoint took in: a session packet that opened, passed the  *)
(* ack block read and the opened-size check, and is ack-eligible on open   *)
(* (every type but the payload, rocketnet_session_ack_eligible_on_open) is *)
(* advanced into it then; a payload only when it is accepted               *)
(* (rocketnet_server_protocol_accept_payload,                              *)
(* rocketnet_client_protocol_accept_payload: at once, or later when the    *)
(* payload acks are deferred and the application accepts it, or never).    *)
(* The ack block builder reads accepted_session:                           *)
(* rocketnet_session_ack_block_build hands it to rocketnet_ack_block_build *)
(* in sdk/rocketnet_ack.cpp, which enumerates its bits between the floor   *)
(* and the high-water mark, so what the sender is told was received is     *)
(* what the endpoint took in (standard/TRANSPORT.md t14 Replay protection  *)
(* and t15 Acknowledgement).  A packet that opens and is then ignored (a   *)
(* malformed block, a wrong opened size) is in replay_session and never in *)
(* accepted_session; so is a payload not yet accepted.                     *)
(*                                                                         *)
(* The window has one job: inside the retained range (highest - W,         *)
(* highest], a bit is set exactly for the sequences that were advanced     *)
(* into it.  The ghost `received` is that truth for replay_session,        *)
(* `accepted` for accepted_session, each trimmed to its own range, and     *)
(* every rule below is a window agreeing with it.                          *)
(*                                                                         *)
(* Two rules are the point of this model, each with a reversed witness in  *)
(* README-ReplayWindow.md that shows the broken order TLC must reject:     *)
(*                                                                         *)
(*   F1 (rnfix-replay-long-skip-clears-window-r2, rocketnet#189).  On a    *)
(*   skip of a full window or more, advance clears the whole ring:         *)
(*   standard/TRANSPORT.md t14 Replay protection says the receiver MUST    *)
(*   clear the whole ring when the forward jump is at least one window     *)
(*   depth.  The code clears the whole ring, ClearedWords = Words; the     *)
(*   witness sets ClearedWords < Words and TLC rejects it.  Both windows   *)
(*   run the same advance.                                                 *)
(*                                                                         *)
(*   OV-34 (rnfix-replay-advanced-after-connect-check-r2).  The server     *)
(*   advances the session window only for a mapping that holds a slot:     *)
(*   standard/TRANSPORT.md t10 step 12 and t14 Replay protection say the   *)
(*   server MUST NOT move the window of a session packet whose source      *)
(*   address has no assigned client slot.  The code checks the slot        *)
(*   before it advances, GateBeforeAdvance = TRUE; the witness sets        *)
(*   GateBeforeAdvance = FALSE and, since a gate before the open is also   *)
(*   one before the advance, GateBeforeOpen = FALSE (the open, the         *)
(*   advance, then the slot check), and TLC rejects it.  accepted_session  *)
(*   moves only after the slot check in either order, so the ack block     *)
(*   never names such a packet; NoAdvanceBeforeConnect is that order's     *)
(*   witness.                                                              *)
(*                                                                         *)
(* This is replay_session and accepted_session only.  The handshake window *)
(* (replay_handshake) is the same struct, but handshake packets are exempt *)
(* from replay marking and only raise its highest_opened                   *)
(* (rocketnet_server_process_sealed_packet in                              *)
(* sdk/rocketnet_server_protocol.cpp; rocketnet_client_process_challenge   *)
(* in sdk/rocketnet_client_protocol.cpp); rocketnet_replay_advance never   *)
(* runs on it, and nothing here speaks for it.                             *)
(*                                                                         *)
(* A ghost, processed, records what the endpoint took in: an ack-eligible  *)
(* packet a slot processed, a payload once accepted.  The harm the rules   *)
(* guard against is the ack block naming a packet nothing took in, and     *)
(* AckReportsOnlyProcessed says it never does: a payload marked in         *)
(* accepted_session when it opens (PayloadAtAccept = FALSE) is its         *)
(* reversed witness.                                                       *)
(*                                                                         *)
(* A third rule places the gate before the open (asr pass 1 F4,            *)
(* 2026-10-21; TRANSPORT.md t10 step 12).  The server acts on no session   *)
(* packet for a mapping without a slot, so it must not spend an AEAD open  *)
(* on one: the ghost openedPending records an open attempted on a pending  *)
(* mapping, by a genuine packet (Receive) or by one that does not open     *)
(* (Forge), and NoOpenBeforeConnect says it never happens.                 *)
(* GateBeforeOpen = FALSE is the order before the fix                      *)
(* (sdk/rocketnet_server_protocol.cpp: the open, then the slot check), and *)
(* TLC must reject it.                                                     *)
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
                       \* ghost still records that s was advanced
    RefuseBelowFloor,  \* BOOLEAN: a sequence at or below highest - W is refused
                       \* as too old (the rule, and what the code does) or the
                       \* too-old check is omitted and the ring speaks for it
    PayloadAtAccept,   \* BOOLEAN: a payload enters accepted_session when it is
                       \* accepted (the rule, and what the code does) or as soon
                       \* as it opens
    Kinds              \* what an opened session packet may turn out to be, a
                       \* subset of AllKinds holding "eligible": an
                       \* ack-eligible type that passes the block read and the
                       \* size check ("eligible"), a payload that does
                       \* ("payload"), or either refused after the open by the
                       \* block read or the size check ("ignored").  The wide
                       \* ring instances explore {"eligible"} alone, where
                       \* accepted_session is replay_session; the accept
                       \* instances explore all three on a smaller ring

W == Words * WordBits  \* ROCKETNET_REPLAY_WINDOW_BITS (sdk/rocketnet_replay.h)

AllKinds == {"eligible", "payload", "ignored"}

ASSUME /\ Words \in Nat \ {0}
       /\ WordBits \in Nat \ {0}
       /\ ClearedWords \in 0..Words
       /\ MaxSeq \in Nat /\ MaxSeq >= W
       /\ GateBeforeAdvance \in BOOLEAN
       /\ GateBeforeOpen \in BOOLEAN
       /\ GateBeforeOpen => GateBeforeAdvance
       /\ MarkOnAdvance \in BOOLEAN
       /\ RefuseBelowFloor \in BOOLEAN
       /\ PayloadAtAccept \in BOOLEAN
       /\ Kinds \subseteq AllKinds /\ "eligible" \in Kinds

VARIABLES
    highest,    \* replay_session.highest_opened: the high-water mark, 0 before any open
    window,     \* [0..W-1 -> BOOLEAN]: replay_session's ring bit for each sequence mod W
    received,   \* ghost: the sequences that opened and are still in replay_session's range
    accHighest, \* accepted_session.highest_opened
    accWindow,  \* accepted_session's ring
    accepted,   \* ghost: the sequences advanced into accepted_session, still in its range
    delivered,  \* ghost: payloads a slot processed and handed up, not yet accepted
    processed,  \* ghost: what the endpoint took in (an ack-eligible packet a slot
                \* processed, a payload once accepted), still in accepted_session's range
    connected,  \* the mapping holds a slot (client_index != -1)
    openedPending \* ghost: an open was attempted while connected was FALSE

vars == <<highest, window, received, accHighest, accWindow, accepted, delivered,
          processed, connected, openedPending>>

Seqs == 1..MaxSeq

(***************************************************************************)
(* The ring.  A sequence lives at bit s % W (rocketnet_replay_bit and      *)
(* rocketnet_replay_set_bit in sdk/rocketnet_replay.cpp; the word scan of  *)
(* rocketnet_ack_block_build in sdk/rocketnet_ack.cpp reads the same bit), *)
(* and bit b lives in word b div 64.                                       *)
(***************************************************************************)
Bit(s)  == s % W
Word(b) == b \div WordBits

(***************************************************************************)
(* The retained range of a window with high-water mark h.                  *)
(* already_received refuses s <= h - W as too old                          *)
(* (rocketnet_replay_already_received in sdk/rocketnet_replay.cpp); the    *)
(* ack builder's floor is h - W + 1, never below 1                         *)
(* (rocketnet_ack_block_build in sdk/rocketnet_ack.cpp).  The two agree:   *)
(* the range is FloorOf(h)..h, W sequences wide once h >= W.               *)
(***************************************************************************)
FloorOf(h) == IF h >= W THEN h - W + 1 ELSE 1

Floor == FloorOf(highest)

InRange(s) == s >= Floor /\ s <= highest

AccInRange(s) == s >= FloorOf(accHighest) /\ s <= accHighest

(***************************************************************************)
(* rocketnet_replay_already_received (sdk/rocketnet_replay.cpp) on a       *)
(* window (h, win).  Above the high-water mark: not received.  Below the   *)
(* range: refused as too old, indistinguishable from received.  Inside:    *)
(* the ring bit.                                                           *)
(***************************************************************************)
AlreadyIn(h, win, s) ==
    IF s > h THEN FALSE
    ELSE IF h >= W /\ RefuseBelowFloor /\ s <= h - W THEN TRUE
    ELSE win[Bit(s)]

AlreadyReceived(s) == AlreadyIn(highest, window, s)

AlreadyAccepted(s) == AlreadyIn(accHighest, accWindow, s)

(***************************************************************************)
(* What the ack block says about s (rocketnet_ack_block_build in           *)
(* sdk/rocketnet_ack.cpp, on accepted_session): the builder walks from     *)
(* largest = accepted_session.highest_opened down to the floor reading     *)
(* the ring bit, so a sequence is reported received exactly when it is in  *)
(* accepted_session's range and its bit is set.  Truncation (max_ranges,   *)
(* the 0xFFFF run caps) only loses the oldest information and is not       *)
(* modelled.                                                               *)
(***************************************************************************)
AckReports(s) == AccInRange(s) /\ accWindow[Bit(s)]

(***************************************************************************)
(* rocketnet_replay_advance (sdk/rocketnet_replay.cpp) on a window (h,     *)
(* win).  Above the high-water mark the entries the advance skips past are *)
(* cleared first, so a sequence far ahead does not inherit bits from the   *)
(* previous lap: a skip shorter than W clears each skipped sequence's bit; *)
(* a skip of W or more clears the whole ring (ClearedWords = Words).  Then *)
(* the bit for s is set when MarkOnAdvance is TRUE.                        *)
(*                                                                         *)
(* Each ghost follows its window's truth: s joins it, and whatever has     *)
(* left the retained range is forgotten -- the window can no longer speak  *)
(* for it and already_received refuses it as too old regardless.           *)
(***************************************************************************)
Cleared(h, win, s) ==
    LET skipped == s - h
    IN  IF skipped >= W
          THEN [b \in 0..W-1 |-> IF Word(b) < ClearedWords THEN FALSE ELSE win[b]]
          ELSE [b \in 0..W-1 |-> IF \E t \in h+1 .. s-1 : Bit(t) = b
                                   THEN FALSE ELSE win[b]]

Marked(h, win, s) ==
    IF s > h
      THEN IF MarkOnAdvance THEN [Cleared(h, win, s) EXCEPT ![Bit(s)] = TRUE]
                            ELSE Cleared(h, win, s)
      ELSE IF MarkOnAdvance THEN [win EXCEPT ![Bit(s)] = TRUE]
                            ELSE win

Raised(h, s) == IF s > h THEN s ELSE h

\* replay_session, after the open
Advance(s) ==
    /\ ~AlreadyReceived(s)                  \* the assert in rocketnet_replay_advance; the caller checked
    /\ window'  = Marked(highest, window, s)
    /\ highest' = Raised(highest, s)
    /\ received' = {t \in received \cup {s} : t + W > highest'}

\* accepted_session, when the endpoint takes the packet in.  The same assert
\* guards it in the code; on open no caller checks accepted_session first
\* (sdk/rocketnet_server_protocol.cpp and sdk/rocketnet_client_protocol.cpp
\* advance it right after the replay_session check passed), so the action
\* does not assume it: OpenNeverAcceptedFirst is the claim that it holds.
\* accept_payload checks it, and Accept below does too.
AdvanceAccepted(s) ==
    /\ accWindow'  = Marked(accHighest, accWindow, s)
    /\ accHighest' = Raised(accHighest, s)
    /\ accepted' = {t \in accepted \cup {s} : t + W > accHighest'}

\* the ghosts kept to accepted_session's range
Trim(S) == {t \in S : t + W > accHighest'}

Init ==
    /\ highest = 0                          \* a new mapping is zeroed:
    /\ window = [b \in 0..W-1 |-> FALSE]    \* rocketnet_encryption_map_create in sdk/rocketnet_encryption_map.cpp
    /\ received = {}
    /\ accHighest = 0
    /\ accWindow = [b \in 0..W-1 |-> FALSE]
    /\ accepted = {}
    /\ delivered = {}
    /\ processed = {}
    /\ connected = FALSE                    \* client_index is -1 until the response
    /\ openedPending = FALSE

(***************************************************************************)
(* The operations and the outside events.                                  *)
(***************************************************************************)

\* A session packet with sequence s arrives at the server.  The
\* already-received check on replay_session refuses a duplicate or a
\* too-old sequence before the open, so an opened packet is never one the
\* window refuses.  The code checks the slot before the open and before the
\* advance, so a packet on a pending mapping leaves the window alone
\* (GateBeforeAdvance = TRUE, GateBeforeOpen = TRUE).  The client is the
\* same pair on a window that is always connected.
\* The packet is opened only when the slot gate lets it through: always on
\* a mapping with a slot, and on a pending one only when the gate follows
\* the open (GateBeforeOpen = FALSE).  A packet that is never opened never
\* reaches the advance, whatever GateBeforeAdvance says.
\* On a mapping with a slot, past the block read and the size check (kind k
\* not "ignored"): an ack-eligible type is advanced into accepted_session
\* and taken in; a payload is handed up (delivered) and waits for Accept,
\* unless PayloadAtAccept = FALSE marks it in accepted_session now.
Opens == connected \/ ~GateBeforeOpen

Receive(s, k) ==
    /\ s \in Seqs
    /\ k \in Kinds
    /\ ~AlreadyReceived(s)
    /\ IF connected \/ (Opens /\ ~GateBeforeAdvance)
         THEN Advance(s)
         ELSE UNCHANGED <<highest, window, received>>
    /\ LET takes == connected /\ k # "ignored"
       IN /\ IF takes /\ (k = "eligible" \/ ~PayloadAtAccept)
               THEN AdvanceAccepted(s)
               ELSE UNCHANGED <<accHighest, accWindow, accepted>>
          /\ processed' = Trim(IF takes /\ k = "eligible" THEN processed \cup {s} ELSE processed)
          /\ delivered' = Trim(IF takes /\ k = "payload" THEN delivered \cup {s} ELSE delivered)
    /\ openedPending' = (openedPending \/ (Opens /\ ~connected))
    /\ UNCHANGED connected

\* The payload s, handed up earlier, is accepted
\* (rocketnet_server_protocol_accept_payload and
\* rocketnet_client_protocol_accept_payload: in the same call that handed it
\* up, or later when the application accepts it with the payload acks
\* deferred).  A sequence accepted_session already refuses (too old, or
\* marked at the open under PayloadAtAccept = FALSE) is left as it is.
Accept(s) ==
    /\ connected
    /\ s \in delivered
    /\ IF ~AlreadyAccepted(s)
         THEN AdvanceAccepted(s)
         ELSE UNCHANGED <<accHighest, accWindow, accepted>>
    /\ processed' = Trim(processed \cup {s})
    /\ delivered' = Trim(delivered \ {s})
    /\ UNCHANGED <<highest, window, received, connected, openedPending>>

\* A sealed packet with sequence s that does not open (forged, or sealed
\* under other keys).  It passes the checks before the open; the open, if
\* the gate lets it run, fails, and nothing about the windows changes.  The
\* integrity-failure count it would add is not modelled; openedPending is
\* the open being attempted at all.
Forge(s) ==
    /\ s \in Seqs
    /\ ~AlreadyReceived(s)
    /\ openedPending' = (openedPending \/ (Opens /\ ~connected))
    /\ UNCHANGED <<highest, window, received, accHighest, accWindow, accepted,
                   delivered, processed, connected>>

\* The connection response opens and the mapping takes a slot
\* (rocketnet_server_process_connection_response in
\* sdk/rocketnet_server_protocol.cpp).  Once only; a mapping that loses its
\* slot is removed, and a new one starts from Init.
Connect ==
    /\ ~connected
    /\ connected' = TRUE
    /\ UNCHANGED <<highest, window, received, accHighest, accWindow, accepted,
                   delivered, processed, openedPending>>

Next ==
    \/ \E s \in Seqs, k \in Kinds : Receive(s, k)
    \/ \E s \in Seqs : Accept(s)
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
    /\ accHighest \in 0..MaxSeq
    /\ accWindow \in [0..W-1 -> BOOLEAN]
    /\ accepted \subseteq Seqs
    /\ delivered \subseteq Seqs
    /\ processed \subseteq Seqs
    /\ connected \in BOOLEAN
    /\ openedPending \in BOOLEAN

\* The window is the truth inside the retained range: a bit is set exactly
\* for the sequences that opened.  This is the rule behind F1: a long skip
\* that leaves words uncleared breaks it as soon as the stale bit's new
\* owner enters the range.
WindowIsTruth ==
    \A s \in Seqs : InRange(s) => (window[Bit(s)] <=> s \in received)

\* The same truth for accepted_session: inside its retained range a bit is
\* set exactly for the sequences advanced into it.  AckReportsOnlyReceived is
\* one direction of it; the other says the ack block loses no sequence the
\* endpoint took in while it is in range.
AcceptedIsTruth ==
    \A s \in Seqs : AccInRange(s) => (accWindow[Bit(s)] <=> s \in accepted)

\* F1 as the card states it: a genuine packet inside the window that has
\* not been received is accepted, never refused as already received.
GenuineIsAccepted ==
    \A s \in Seqs : InRange(s) /\ s \notin received => ~AlreadyReceived(s)

\* F1's second half, on the window the ack block reads: the block names
\* only sequences advanced into accepted_session.
AckReportsOnlyReceived ==
    \A s \in Seqs : AckReports(s) => s \in accepted

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

\* The ack block's largest, accepted_session's high-water mark, is itself
\* advanced into it and marked (the assert in rocketnet_ack_block_build in
\* sdk/rocketnet_ack.cpp: largest is always marked received).
HighestIsReceived ==
    accHighest > 0 => accHighest \in accepted /\ accWindow[Bit(accHighest)]

\* The advance into accepted_session at the open never trips its assert
\* (rocketnet_replay_advance asserts the sequence is not already received):
\* whatever replay_session lets through, accepted_session would take.
OpenNeverAcceptedFirst ==
    \A s \in Seqs : ~AlreadyReceived(s) => ~AlreadyAccepted(s)

\* OV-34: the session windows of a mapping with no slot never move.
NoAdvanceBeforeConnect ==
    ~connected => highest = 0 /\ received = {} /\ accHighest = 0

\* The harm behind the rules: the ack block names only packets the endpoint
\* took in.  A payload marked when it opens would be reported to the sender
\* as received although nothing accepted it.
AckReportsOnlyProcessed ==
    \A s \in Seqs : AckReports(s) => s \in processed

\* F4 (asr pass 1, 2026-10-21): no AEAD open is ever attempted on a mapping
\* without a slot -- the slot gate runs before decryption.
NoOpenBeforeConnect ==
    ~openedPending

\* The high-water marks never go backwards, and a slot is never given up.
\* This holds by construction and checks nothing an action could break:
\* Raised only ever moves a mark up (as rocketnet_replay_advance raises
\* highest_opened only), Connect only sets connected, nothing clears it, and
\* no cfg reverses it.  It is kept as the statement that tests/dst_checks.h's
\* replay_monotone mirrors, not as a check of this model's actions.
Monotone ==
    [][highest' >= highest /\ accHighest' >= accHighest /\ (connected => connected')]_vars

=============================================================================
