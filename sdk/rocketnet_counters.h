/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_COUNTERS_H
#define ROCKETNET_COUNTERS_H

#include <stdint.h>

// The handshake work budget: the most post-cookie handshakes (connection
// requests whose cookie validated) one update will process, per server. A
// request past the budget is dropped before any asymmetric work and counted,
// so a flood of staged attempts cannot spend the whole update on signature
// verification. Lives in this header so the protocol ladder and the test
// share one number rather than copying it.
#define ROCKETNET_HANDSHAKE_WORK_BUDGET_PER_UPDATE      1024

// Returned by rocketnet_server_protocol_process_connection_request when the
// handshake budget is exhausted: the next value after
// ROCKETNET_SERVER_PROCESS_ACCEPTED (5). Declared here, in the same header as
// the drop counter it causes, so the test reads the code it asserts from the
// same header that returns it.
#define ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED        6

// Counters, per the standard's observability section: one counter per
// distinct reason a packet is rejected, arrival counted separately from
// outcome, monotonic and never reset. Names are the standard's; indices are
// this implementation's contract and are never renumbered -- new counters
// take new slots at the end of their group.
//
// The set grows with the code that counts: a counter with no code path is a
// dead counter, so entries land here in the same change as their first user.

// THE COUNTER LIST. One row per counter, X( NAME, CLASS, FAMILY ), in index
// order. The enum below, rocketnet_counter_name, rocketnet_counter_class and
// rocketnet_counters_conserved are all generated from these rows, so a
// counter's index, its name, what a clean run shows of it and where it is
// conserved are one line, and a gate is computed from the list and never kept
// as a second list in a script.
//
// CLASS is what a clean run shows of the counter. Exactly four:
//
//   REFUSAL    reads 0 on a clean path at any round trip. A packet or a
//              request refused for a reason. A resource exhaustion
//              (SEND_RING_FULL, MESSAGE_SEND_STORE_FULL,
//              SEND_HISTORY_OVERFLOW) is a refusal.
//   EXPECTED   moves on a clean path by design: success outcomes, lifecycle
//              counters, and side counters such as ACK_UNKNOWN_SEQUENCE,
//              CONNECTION_REQUEST_NO_COOKIE (the first leg of the cookie
//              exchange), DISCONNECT_NO_MAPPING (the redundant disconnect
//              burst), the ALREADY_CONNECTED duplicates a round trip above
//              100 ms leaves, and the session-end race: a session that ends
//              with the other end's packets in flight sends them to an end
//              that is gone (the session types' NO_MAPPING at the server and
//              WRONG_STATE at the client).
//   PATH_LOSS  moves only when the path loses or reorders packets:
//              MESSAGE_RESENT, FRAGMENT_PAYLOAD_ABANDONED.
//   ARRIVAL    a RECEIVED counter: a packet of the type arrived.
//
// FAMILY is a separate column: the RECEIVED counter whose outcomes this
// counter sums into, or NONE. Every packet counted as arrived takes exactly
// one outcome in the same synchronous call, so at a quiescent point (after an
// update returns, read on the thread that owns the block) RECEIVED equals the
// sum of the outcomes of its family. A counter is an outcome of one family at
// most: an outcome counted from two ladders would balance only while it stays
// zero, so it is split into one counter per ladder. The packet entry
// (PACKETS_RECEIVED and the prefilter, reserved-type and wrong-direction
// refusals) is outside every family: the typed ladders are also entered
// directly, without passing the entry. ACK_UNKNOWN_SEQUENCE and the fragment
// payload counters are side counters outside any family: the packet that fires
// one still takes its own outcome, and they fire per ack or per payload, not
// per packet.
//
// Append-only: new rows go at the end, so the indices never move.

#define ROCKETNET_COUNTER_ROWS( X ) \
    /* One counter per distinct reason, arrival counted separately, per packet */ \
    /* type. The indices are a contract: new counters append. */ \
    \
    /* the packet entry, before any type dispatches */ \
    X( PACKETS_RECEIVED, ARRIVAL, NONE ) \
    X( PREFILTER_TOO_SHORT, REFUSAL, NONE ) \
    X( PREFILTER_TOO_LARGE, REFUSAL, NONE ) \
    X( PREFILTER_WRONG_EPOCH, REFUSAL, NONE ) \
    X( PREFILTER_WRONG_VALUE, REFUSAL, NONE ) \
    X( RESERVED_TYPE, REFUSAL, NONE ) \
    X( WRONG_DIRECTION, REFUSAL, NONE ) \
    \
    /* connection request processing, one per check in the standard's order */ \
    X( CONNECTION_REQUEST_RECEIVED, ARRIVAL, NONE ) \
    X( CONNECTION_REQUEST_WRONG_SIZE, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /* EXPECTED: a request duplicate that a round trip above 100 ms leaves, */ \
    /* arriving after the first has connected. */ \
    X( CONNECTION_REQUEST_ALREADY_CONNECTED, EXPECTED, CONNECTION_REQUEST_RECEIVED ) \
    /* EXPECTED: the same duplicate arriving while the handshake is in */ \
    /* flight; the stored challenge answers it. */ \
    X( CONNECTION_REQUEST_CHALLENGE_RESENT, EXPECTED, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_WRONG_VERSION, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_WRONG_PROTOCOL_ID, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_TOKEN_EXPIRED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_TIMEOUT_INVALID, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_BAD_ADDRESS_FLAGS, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_WRONG_SERVER_KEY, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /* EXPECTED: the first leg of the cookie exchange, on every connection. */ \
    X( CONNECTION_REQUEST_NO_COOKIE, EXPECTED, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_COOKIE_EXPIRED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_TOKEN_ALREADY_USED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_SEALED_MISMATCH, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_CLIENT_ID_ALREADY_CONNECTED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /* retired with the admitted-token set (docs/SESSION.md 1.6): keeps its */ \
    /* index, always zero. */ \
    X( CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_SMALL_ORDER_KEY, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_NO_SLOTS, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /* EXPECTED: the success outcome. */ \
    X( CONNECTION_REQUEST_SENT_CHALLENGE, EXPECTED, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_INTERNAL_ERROR, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    \
    /* connection response processing, in the standard's order */ \
    X( CONNECTION_RESPONSE_RECEIVED, ARRIVAL, NONE ) \
    X( CONNECTION_RESPONSE_WRONG_SIZE, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    X( CONNECTION_RESPONSE_NO_MAPPING, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    X( CONNECTION_RESPONSE_ZERO_SEQUENCE, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    X( CONNECTION_RESPONSE_DID_NOT_OPEN, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /* EXPECTED: a response duplicate arriving after the first has connected. */ \
    X( CONNECTION_RESPONSE_ALREADY_CONNECTED, EXPECTED, CONNECTION_RESPONSE_RECEIVED ) \
    X( CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /* retired with the admitted-token set (docs/SESSION.md 1.6): keeps its */ \
    /* index, always zero. */ \
    X( CONNECTION_RESPONSE_ADMITTED_SET_FULL, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    X( CONNECTION_RESPONSE_NO_SLOTS, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    X( CONNECTION_RESPONSE_CONNECTED, EXPECTED, CONNECTION_RESPONSE_RECEIVED ) \
    \
    /* keep-alive, payload and disconnect at the server */ \
    X( KEEP_ALIVE_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race. The server ended the session (a */ \
    /* disconnect, a timeout) with the client's packets in flight; no network */ \
    /* fault is needed (test_unit_client_receive_counters.cpp, */ \
    /* session_end_races_move_only_expected_counters). */ \
    X( KEEP_ALIVE_NO_MAPPING, EXPECTED, KEEP_ALIVE_RECEIVED ) \
    /* REFUSAL from here to the end of the type: a refused packet reads 0 on */ \
    /* a clean path. */ \
    X( KEEP_ALIVE_ZERO_SEQUENCE, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    X( KEEP_ALIVE_ALREADY_RECEIVED, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    X( KEEP_ALIVE_DID_NOT_OPEN, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    X( KEEP_ALIVE_NOT_CONNECTED, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    X( KEEP_ALIVE_ACCEPTED, EXPECTED, KEEP_ALIVE_RECEIVED ) \
    \
    X( PAYLOAD_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race, as KEEP_ALIVE_NO_MAPPING. */ \
    X( PAYLOAD_NO_MAPPING, EXPECTED, PAYLOAD_RECEIVED ) \
    X( PAYLOAD_ZERO_SEQUENCE, REFUSAL, PAYLOAD_RECEIVED ) \
    X( PAYLOAD_ALREADY_RECEIVED, REFUSAL, PAYLOAD_RECEIVED ) \
    X( PAYLOAD_DID_NOT_OPEN, REFUSAL, PAYLOAD_RECEIVED ) \
    X( PAYLOAD_NOT_CONNECTED, REFUSAL, PAYLOAD_RECEIVED ) \
    X( PAYLOAD_DELIVERED, EXPECTED, PAYLOAD_RECEIVED ) \
    \
    X( DISCONNECT_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the copies of the redundant disconnect burst after the first */ \
    /* has torn the mapping down. */ \
    X( DISCONNECT_NO_MAPPING, EXPECTED, DISCONNECT_RECEIVED ) \
    X( DISCONNECT_ZERO_SEQUENCE, REFUSAL, DISCONNECT_RECEIVED ) \
    X( DISCONNECT_ALREADY_RECEIVED, REFUSAL, DISCONNECT_RECEIVED ) \
    X( DISCONNECT_DID_NOT_OPEN, REFUSAL, DISCONNECT_RECEIVED ) \
    X( DISCONNECT_NOT_CONNECTED, REFUSAL, DISCONNECT_RECEIVED ) \
    X( DISCONNECT_DISCONNECTED, EXPECTED, DISCONNECT_RECEIVED ) \
    \
    /* mapping and client lifecycle */ \
    /* EXPECTED, as are the lifecycle counters through CLIENT_TIMED_OUT: they */ \
    /* count connections coming and going, which a clean run does by design. */ \
    X( MAPPING_CREATED, EXPECTED, NONE ) \
    X( MAPPING_IDLE_TIMEOUT, EXPECTED, NONE ) \
    X( MAPPING_HANDSHAKE_DEADLINE_EXPIRED, EXPECTED, NONE ) \
    X( MAPPING_TABLE_FULL, REFUSAL, NONE ) \
    X( CLIENT_CONNECTED, EXPECTED, NONE ) \
    /* REFUSAL: a resource exhaustion is a refusal. */ \
    X( SEND_RING_FULL, REFUSAL, NONE ) \
    /* REFUSAL: an internal state the shipped protocol cannot produce. */ \
    X( SLOT_WITHOUT_MAPPING, REFUSAL, NONE ) \
    X( CLIENT_DISCONNECTED, EXPECTED, NONE ) \
    X( SERVER_DISCONNECTED_CLIENT, EXPECTED, NONE ) \
    X( CLIENT_TIMED_OUT, EXPECTED, NONE ) \
    \
    /* the client's receive path. the shared packet counters above serve both */ \
    /* directions; these are the client-specific reasons. */ \
    X( CLIENT_COOKIE_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: a cookie duplicate arriving after the client moved on, the */ \
    /* same round-trip duplicate as ALREADY_CONNECTED. */ \
    X( CLIENT_COOKIE_WRONG_STATE, EXPECTED, CLIENT_COOKIE_RECEIVED ) \
    X( CLIENT_COOKIE_SALT_MISMATCH, REFUSAL, CLIENT_COOKIE_RECEIVED ) \
    X( CLIENT_COOKIE_STORED, EXPECTED, CLIENT_COOKIE_RECEIVED ) \
    X( CLIENT_CHALLENGE_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the server resends the challenge for a duplicate request; */ \
    /* the client no longer has a question for it. */ \
    X( CLIENT_CHALLENGE_WRONG_STATE, EXPECTED, CLIENT_CHALLENGE_RECEIVED ) \
    X( CLIENT_CHALLENGE_ZERO_SEQUENCE, REFUSAL, CLIENT_CHALLENGE_RECEIVED ) \
    X( CLIENT_CHALLENGE_DID_NOT_OPEN, REFUSAL, CLIENT_CHALLENGE_RECEIVED ) \
    X( CLIENT_CHALLENGE_SMALL_ORDER_KEY, REFUSAL, CLIENT_CHALLENGE_RECEIVED ) \
    X( CLIENT_CHALLENGE_ACCEPTED, EXPECTED, CLIENT_CHALLENGE_RECEIVED ) \
    X( CLIENT_DENIED_RECEIVED, ARRIVAL, NONE ) \
    /* REFUSAL: a denied reaches a client only when the server is out of */ \
    /* slots. */ \
    X( CLIENT_DENIED_WRONG_STATE, REFUSAL, CLIENT_DENIED_RECEIVED ) \
    X( CLIENT_DENIED_ZERO_SEQUENCE, REFUSAL, CLIENT_DENIED_RECEIVED ) \
    X( CLIENT_DENIED_DID_NOT_OPEN, REFUSAL, CLIENT_DENIED_RECEIVED ) \
    /* REFUSAL: the server denied the connection. The server denies only for */ \
    /* NO_SLOTS, so a clean run reads 0. */ \
    X( CLIENT_DENIED_ACCEPTED, REFUSAL, CLIENT_DENIED_RECEIVED ) \
    X( CLIENT_KEEP_ALIVE_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race, from the client's side. The client */ \
    /* left the session (a disconnect) with the server's packets in flight. A */ \
    /* loss-free soak_sim wire moves it; so do CLIENT_PAYLOAD_, CLIENT_ACK_, */ \
    /* CLIENT_FRAGMENT_ and CLIENT_DISCONNECT_WRONG_STATE (the disconnect */ \
    /* burst's copies). */ \
    X( CLIENT_KEEP_ALIVE_WRONG_STATE, EXPECTED, CLIENT_KEEP_ALIVE_RECEIVED ) \
    X( CLIENT_KEEP_ALIVE_ZERO_SEQUENCE, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    X( CLIENT_KEEP_ALIVE_ALREADY_RECEIVED, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    X( CLIENT_KEEP_ALIVE_DID_NOT_OPEN, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    X( CLIENT_KEEP_ALIVE_ACCEPTED, EXPECTED, CLIENT_KEEP_ALIVE_RECEIVED ) \
    X( CLIENT_PAYLOAD_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race, as CLIENT_KEEP_ALIVE_WRONG_STATE. */ \
    X( CLIENT_PAYLOAD_WRONG_STATE, EXPECTED, CLIENT_PAYLOAD_RECEIVED ) \
    X( CLIENT_PAYLOAD_ZERO_SEQUENCE, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    X( CLIENT_PAYLOAD_ALREADY_RECEIVED, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    X( CLIENT_PAYLOAD_DID_NOT_OPEN, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    X( CLIENT_PAYLOAD_DELIVERED, EXPECTED, CLIENT_PAYLOAD_RECEIVED ) \
    X( CLIENT_DISCONNECT_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the copies of the server's redundant disconnect burst after the first. */ \
    X( CLIENT_DISCONNECT_WRONG_STATE, EXPECTED, CLIENT_DISCONNECT_RECEIVED ) \
    X( CLIENT_DISCONNECT_ZERO_SEQUENCE, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    X( CLIENT_DISCONNECT_ALREADY_RECEIVED, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    X( CLIENT_DISCONNECT_DID_NOT_OPEN, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    X( CLIENT_DISCONNECT_ACCEPTED, EXPECTED, CLIENT_DISCONNECT_RECEIVED ) \
    \
    /* The general acks, appended per the append-only index contract. The */ \
    /* session types gain a WRONG_SIZE outcome (below the type's wire floor, */ \
    /* never a later refusal) and a BAD_ACK_BLOCK outcome (the block at the */ \
    /* front of the sealed data refused the whole packet); type 8 gets the */ \
    /* full per-type family on both sides. ACK_UNKNOWN_SEQUENCE is a side */ \
    /* counter outside the RECEIVED == sum-of-outcomes invariant: the packet */ \
    /* that fires it still succeeds, and one block may fire it several */ \
    /* times. ACKED_BATCH_FULL is this implementation's own counter: a */ \
    /* packet id whose delivery notification was deferred to a later update */ \
    /* because that update's batch was full -- deferred, never lost. */ \
    \
    X( KEEP_ALIVE_WRONG_SIZE, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    X( KEEP_ALIVE_BAD_ACK_BLOCK, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    X( PAYLOAD_WRONG_SIZE, REFUSAL, PAYLOAD_RECEIVED ) \
    X( PAYLOAD_BAD_ACK_BLOCK, REFUSAL, PAYLOAD_RECEIVED ) \
    X( DISCONNECT_WRONG_SIZE, REFUSAL, DISCONNECT_RECEIVED ) \
    X( DISCONNECT_BAD_ACK_BLOCK, REFUSAL, DISCONNECT_RECEIVED ) \
    \
    X( ACK_RECEIVED, ARRIVAL, NONE ) \
    X( ACK_WRONG_SIZE, REFUSAL, ACK_RECEIVED ) \
    /* EXPECTED: the session-end race, as KEEP_ALIVE_NO_MAPPING. */ \
    X( ACK_NO_MAPPING, EXPECTED, ACK_RECEIVED ) \
    X( ACK_ZERO_SEQUENCE, REFUSAL, ACK_RECEIVED ) \
    X( ACK_ALREADY_RECEIVED, REFUSAL, ACK_RECEIVED ) \
    X( ACK_DID_NOT_OPEN, REFUSAL, ACK_RECEIVED ) \
    X( ACK_NOT_CONNECTED, REFUSAL, ACK_RECEIVED ) \
    X( ACK_BAD_ACK_BLOCK, REFUSAL, ACK_RECEIVED ) \
    X( ACK_ACCEPTED, EXPECTED, ACK_RECEIVED ) \
    \
    /* EXPECTED: a side counter. The packet that fires it still succeeds. */ \
    X( ACK_UNKNOWN_SEQUENCE, EXPECTED, NONE ) \
    /* REFUSAL: a resource exhaustion (the delivery batch). The deferred id */ \
    /* is not lost. */ \
    X( ACKED_BATCH_FULL, REFUSAL, NONE ) \
    \
    X( CLIENT_KEEP_ALIVE_WRONG_SIZE, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    X( CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    X( CLIENT_PAYLOAD_WRONG_SIZE, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    X( CLIENT_PAYLOAD_BAD_ACK_BLOCK, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    X( CLIENT_DISCONNECT_WRONG_SIZE, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    X( CLIENT_DISCONNECT_BAD_ACK_BLOCK, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    \
    X( CLIENT_ACK_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race, as CLIENT_KEEP_ALIVE_WRONG_STATE. */ \
    X( CLIENT_ACK_WRONG_STATE, EXPECTED, CLIENT_ACK_RECEIVED ) \
    X( CLIENT_ACK_WRONG_SIZE, REFUSAL, CLIENT_ACK_RECEIVED ) \
    X( CLIENT_ACK_ZERO_SEQUENCE, REFUSAL, CLIENT_ACK_RECEIVED ) \
    X( CLIENT_ACK_ALREADY_RECEIVED, REFUSAL, CLIENT_ACK_RECEIVED ) \
    X( CLIENT_ACK_DID_NOT_OPEN, REFUSAL, CLIENT_ACK_RECEIVED ) \
    X( CLIENT_ACK_BAD_ACK_BLOCK, REFUSAL, CLIENT_ACK_RECEIVED ) \
    X( CLIENT_ACK_ACCEPTED, EXPECTED, CLIENT_ACK_RECEIVED ) \
    \
    X( CLIENT_ACK_UNKNOWN_SEQUENCE, EXPECTED, NONE ) \
    X( CLIENT_ACKED_BATCH_FULL, REFUSAL, NONE ) \
    \
    /* WRONG_OPENED_SIZE: the opened data, less the ack block, did not match */ \
    /* the type -- its own reason, distinct from WRONG_SIZE (the wire floor) */ \
    /* and DID_NOT_OPEN (the seal refused), per the one-counter-per-reason */ \
    /* rule. Appended per the append-only index contract. */ \
    \
    X( KEEP_ALIVE_WRONG_OPENED_SIZE, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    X( PAYLOAD_WRONG_OPENED_SIZE, REFUSAL, PAYLOAD_RECEIVED ) \
    X( DISCONNECT_WRONG_OPENED_SIZE, REFUSAL, DISCONNECT_RECEIVED ) \
    X( ACK_WRONG_OPENED_SIZE, REFUSAL, ACK_RECEIVED ) \
    X( CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    X( CLIENT_PAYLOAD_WRONG_OPENED_SIZE, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    X( CLIENT_DISCONNECT_WRONG_OPENED_SIZE, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    X( CLIENT_ACK_WRONG_OPENED_SIZE, REFUSAL, CLIENT_ACK_RECEIVED ) \
    \
    /* The payload fragment, type 9, appended per the append-only index */ \
    /* contract: the sealed-type family both sides, then the reassembly */ \
    /* refusal reasons from the standard's _Fragmentation_ in their check */ \
    /* order, the per-packet ACCEPTED outcome, and the payload-level side */ \
    /* counters outside the RECEIVED == sum-of-outcomes invariant -- */ \
    /* REASSEMBLED and ABANDONED fire per payload, not per packet. */ \
    /* */ \
    /* FRAGMENT_ALREADY_RECEIVED is the replay-protection outcome, spelled */ \
    /* as every session type spells it; the duplicate-fragment-index */ \
    /* refusal is FRAGMENT_DUPLICATE_INDEX, per the standard's ruled */ \
    /* naming. One counter here is this implementation's own, not the */ \
    /* standard's: FRAGMENT_PAYLOAD_BATCH_FULL counts a completed payload */ \
    /* dropped because the update's delivery batch was full -- it stays */ \
    /* zero in ordinary operation, so delivered == REASSEMBLED - BATCH_FULL. */ \
    \
    X( FRAGMENT_RECEIVED, ARRIVAL, NONE ) \
    X( FRAGMENT_WRONG_SIZE, REFUSAL, FRAGMENT_RECEIVED ) \
    /* EXPECTED: the session-end race, as KEEP_ALIVE_NO_MAPPING. */ \
    X( FRAGMENT_NO_MAPPING, EXPECTED, FRAGMENT_RECEIVED ) \
    X( FRAGMENT_ZERO_SEQUENCE, REFUSAL, FRAGMENT_RECEIVED ) \
    X( FRAGMENT_ALREADY_RECEIVED, REFUSAL, FRAGMENT_RECEIVED ) \
    X( FRAGMENT_DID_NOT_OPEN, REFUSAL, FRAGMENT_RECEIVED ) \
    X( FRAGMENT_NOT_CONNECTED, REFUSAL, FRAGMENT_RECEIVED ) \
    X( FRAGMENT_BAD_ACK_BLOCK, REFUSAL, FRAGMENT_RECEIVED ) \
    X( FRAGMENT_WRONG_OPENED_SIZE, REFUSAL, FRAGMENT_RECEIVED ) \
    X( FRAGMENT_ZERO_PAYLOAD_ID, REFUSAL, FRAGMENT_RECEIVED ) \
    /* PATH_LOSS: a fragment of a payload an older loss or reorder left */ \
    /* behind. */ \
    X( FRAGMENT_STALE, PATH_LOSS, FRAGMENT_RECEIVED ) \
    X( FRAGMENT_COUNT_INVALID, REFUSAL, FRAGMENT_RECEIVED ) \
    X( FRAGMENT_INDEX_OUT_OF_RANGE, REFUSAL, FRAGMENT_RECEIVED ) \
    X( FRAGMENT_COUNT_MISMATCH, REFUSAL, FRAGMENT_RECEIVED ) \
    X( FRAGMENT_SIZE_MISMATCH, REFUSAL, FRAGMENT_RECEIVED ) \
    X( FRAGMENT_PAYLOAD_TOO_LARGE, REFUSAL, FRAGMENT_RECEIVED ) \
    X( FRAGMENT_DUPLICATE_INDEX, REFUSAL, FRAGMENT_RECEIVED ) \
    X( FRAGMENT_ACCEPTED, EXPECTED, FRAGMENT_RECEIVED ) \
    X( FRAGMENT_PAYLOAD_REASSEMBLED, EXPECTED, NONE ) \
    /* PATH_LOSS: a lost fragment loses its whole payload, by design. */ \
    X( FRAGMENT_PAYLOAD_ABANDONED, PATH_LOSS, NONE ) \
    X( FRAGMENT_PAYLOAD_BATCH_FULL, REFUSAL, NONE ) \
    \
    X( CLIENT_FRAGMENT_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race, as CLIENT_KEEP_ALIVE_WRONG_STATE. */ \
    X( CLIENT_FRAGMENT_WRONG_STATE, EXPECTED, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_WRONG_SIZE, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_ZERO_SEQUENCE, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_ALREADY_RECEIVED, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_DID_NOT_OPEN, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_BAD_ACK_BLOCK, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_WRONG_OPENED_SIZE, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_ZERO_PAYLOAD_ID, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_STALE, PATH_LOSS, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_COUNT_INVALID, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_INDEX_OUT_OF_RANGE, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_COUNT_MISMATCH, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_SIZE_MISMATCH, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_PAYLOAD_TOO_LARGE, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_DUPLICATE_INDEX, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_ACCEPTED, EXPECTED, CLIENT_FRAGMENT_RECEIVED ) \
    X( CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED, EXPECTED, NONE ) \
    X( CLIENT_FRAGMENT_PAYLOAD_ABANDONED, PATH_LOSS, NONE ) \
    X( CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL, REFUSAL, NONE ) \
    \
    /* the send side: a payload requested above the wire's capacity found */ \
    /* the fill phase's arena exhausted, and was dropped at start_packet -- */ \
    /* the arena sibling of SEND_RING_FULL */ \
    \
    X( SEND_ARENA_FULL, REFUSAL, NONE ) \
    \
    /* the copying receive cursor was handed a buffer smaller than a */ \
    /* delivered payload: the payload is refused whole, never truncated. */ \
    /* stays zero for every caller sizing by the configured maximum. */ \
    \
    X( RECEIVE_CURSOR_TOO_SMALL, REFUSAL, NONE ) \
    \
    /* History was overwritten before its ACK with no later ACK to say what */ \
    /* became of the packet (a silent peer, or a window too small for the */ \
    /* path), or a writer fell behind the retained sequence window. A */ \
    /* refusal of history: it reads zero on a healthy path. Advisory: the */ \
    /* packet is still transmitted. An eviction the peer's later ACKs mark */ \
    /* as lost is SEND_HISTORY_LOST instead (tla/RNSentHistory.tla). */ \
    /* REFUSAL: a resource exhaustion. soak_sim moves it only when its wire */ \
    /* loses packets, and says so. */ \
    X( SEND_HISTORY_OVERFLOW, REFUSAL, NONE ) \
    /* Private packet metadata budget exhausted during a fill phase. */ \
    X( SEND_LEASE_FULL, REFUSAL, NONE ) \
    \
    /* a connection's sealed packets failed to open ROCKETNET_INTEGRITY_LIMIT */ \
    /* times across its keys and it was disconnected, per the standard's AEAD */ \
    /* integrity limit. a lifecycle counter, once per torn-down connection, */ \
    /* on whichever end reached the limit. appended: indices are the contract. */ \
    /* REFUSAL: a connection torn down after failed opens, never on a clean */ \
    /* path (its comment calls it a lifecycle counter; the clean-run */ \
    /* definition decides the class). */ \
    X( INTEGRITY_LIMIT_REACHED, REFUSAL, NONE ) \
    \
    /* client received a connection cookie packet with an invalid wire length */ \
    /* (not equal to ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES). distinct from */ \
    /* ROCKETNET_COUNTER_CLIENT_COOKIE_SALT_MISMATCH so ledger distinguishes */ \
    /* size errors from off-path salt injection. */ \
    X( CLIENT_COOKIE_WRONG_SIZE, REFUSAL, CLIENT_COOKIE_RECEIVED ) \
    \
    /* the message layer's region (docs/CHANNELS.md 4): a send refused because */ \
    /* the connection's send store had no chunk of the class it needs (or, for */ \
    /* a block, not every chunk it needs), and a send refused because every */ \
    /* send descriptor was in use. Exhaustion is a counted refusal, never */ \
    /* growth; nothing is taken from either when one refuses. Counted on the */ \
    /* owner's counters, once per refused send. appended: indices are the contract. */ \
    X( MESSAGE_SEND_STORE_FULL, REFUSAL, NONE ) \
    X( MESSAGE_SEND_QUEUE_FULL, REFUSAL, NONE ) \
    \
    /* a cookie-valid connection request arrived after the update's handshake */ \
    /* budget was spent and was dropped before any asymmetric work, counted so */ \
    /* the drop is observable rather than silent. appended: indices are the */ \
    /* contract. */ \
    X( CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    \
    /* a connection request whose cookie verified but whose source had already */ \
    /* spent the failed-signature-verify budget for this window: refused before */ \
    /* the Ed25519 verify ran. distinct from SIGNATURE_DID_NOT_VERIFY, which */ \
    /* counts verifies that actually ran and failed. appended: indices are the */ \
    /* contract. */ \
    X( CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    \
    /* the UNRELIABLE path (docs/CHANNELS.md 5, 6.3, 6.4, 9.3, 11.2): a code-0 */ \
    /* record dropped on receive because the per-update unreliable delivery */ \
    /* budget Du was full (section 6.3 step 4), and a code-0 message dropped on */ \
    /* send: because finish_packet refused the packet that carried it (section */ \
    /* 6.4, Failed send), or because it is longer than the record's u16 length */ \
    /* and can never be carried. Unreliable semantics allow all three; each is */ \
    /* counted once per dropped message. appended: indices are the contract. */ \
    X( MESSAGE_UNRELIABLE_DELIVERY_FULL, REFUSAL, NONE ) \
    X( MESSAGE_UNRELIABLE_SEND_FAILED, REFUSAL, NONE ) \
    \
    /* Layer 3 receive window: a reliable number already received (6.5). */ \
    /* PATH_LOSS: a reliable number received twice, the resend of a lost ack */ \
    /* or packet. */ \
    X( MESSAGE_DUPLICATE, PATH_LOSS, NONE ) \
    /* Numbering met either the count or ring bound (CHANNELS 6.2). Count */ \
    /* once per stall, even when the scheduler retries the same head. */ \
    X( MESSAGE_WINDOW_STALL, REFUSAL, NONE ) \
    \
    /* Layer 3 sent-packet map and resend FIFO (docs/CHANNELS.md 6.4, 15.1): */ \
    /* a sent-packet ring entry evicted to make room for a new packet (its */ \
    /* referenced numbers stay in the resend FIFO), and a reliable record */ \
    /* resent on the RTO. appended: indices are the contract. */ \
    X( MESSAGE_SENT_MAP_EVICTED, REFUSAL, NONE ) \
    /* PATH_LOSS: a reliable record resent on the RTO. */ \
    X( MESSAGE_RESENT, PATH_LOSS, NONE ) \
    \
    /* a connection's message memory (its region, docs/CHANNELS.md 4) was */ \
    /* exhausted: an operation found no room, the region took its sticky */ \
    /* exhausted state, and the connection is disconnected with the reason */ \
    /* ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED (tla/RNConnectionMemory.tla, */ \
    /* Exhaust). Counted once per exhausted connection, on the owner's */ \
    /* counters, beside the refusal's own counter (MESSAGE_SEND_STORE_FULL or */ \
    /* MESSAGE_SEND_QUEUE_FULL). appended: indices are the contract. */ \
    X( MESSAGE_MEMORY_EXHAUSTED, REFUSAL, NONE ) \
    \
    /* Layer 4's dispatch (docs/CHANNELS.md 7.2, 7.3, 7.4, 7.5): a send refused */ \
    /* because its reliability is outside the enabled mask, a receive of an */ \
    /* application record whose code the mask does not admit, an unreliable */ \
    /* record dropped because the send budget could not pay the packet that */ \
    /* would carry it, and the counters the sequenced and receipt cards need, */ \
    /* appended now so those cards run without touching this header. An ordered */ \
    /* record behind its channel's read index is MESSAGE_ORDERING_BEHIND (7.2: */ \
    /* layer 3 already caught the duplicates, so it is a violation). */ \
    /* appended: indices are the contract. */ \
    X( MESSAGE_SEND_TYPE_NOT_ENABLED, REFUSAL, NONE ) \
    X( MESSAGE_TYPE_NOT_ENABLED, REFUSAL, NONE ) \
    X( MESSAGE_UNRELIABLE_OVER_BUDGET, REFUSAL, NONE ) \
    X( MESSAGE_ORDERING_BEHIND, REFUSAL, NONE ) \
    X( MESSAGE_SEQUENCED_STALE, PATH_LOSS, NONE ) \
    X( MESSAGE_SEQUENCED_SUPERSEDED, PATH_LOSS, NONE ) \
    X( MESSAGE_SEQUENCED_HOLD_FULL, REFUSAL, NONE ) \
    X( MESSAGE_RECEIPT_RING_FULL, REFUSAL, NONE ) \
    X( MESSAGE_RECEIPT_ACKED, EXPECTED, NONE ) \
    X( MESSAGE_RECEIPT_LOSS, PATH_LOSS, NONE ) \
    \
    /* The connection response ladder's own internal error: the denied */ \
    /* packet or the first keep-alive could not be written after the */ \
    /* response was received. It was counted as */ \
    /* CONNECTION_REQUEST_INTERNAL_ERROR, which put one outcome in two */ \
    /* families (the request ladder and the response ladder); it is its */ \
    /* own counter now. appended: indices are the contract. */ \
    X( CONNECTION_RESPONSE_INTERNAL_ERROR, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    \
    /* A request from an address that holds a handshake in flight, carrying */ \
    /* a different salt than the mapping's, is answered with the mapping's */ \
    /* stored challenge, so nothing increments this counter now. It stays */ \
    /* declared because the indices are the contract. appended: indices are */ \
    /* the contract. */ \
    X( CONNECTION_REQUEST_MAPPING_SALT_MISMATCH, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    \
    /* History was overwritten before its ACK, and the peer had acknowledged */ \
    /* a sequence at least ROCKETNET_LOSS_PACKET_THRESHOLD above it: the */ \
    /* packet was lost on the path. An outcome, not a refusal. Together with */ \
    /* SEND_HISTORY_OVERFLOW this is the count of overwritten unacknowledged */ \
    /* send records (tla/RNSentHistory.tla). Advisory: the packet is still */ \
    /* transmitted. PATH_LOSS: it moves only when the path loses packets. */ \
    /* appended: indices are the contract. */ \
    X( SEND_HISTORY_LOST, PATH_LOSS, NONE ) \
    \
    /* RELIABLE_ORDERED's receive path (docs/CHANNELS.md 7.2, 8): each is a */ \
    /* wire violation, counted once per record, and each ends the */ \
    /* connection with ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION. A record */ \
    /* whose channel is at or past this end's count; a block fragment whose */ \
    /* count or index disagrees with the block's first-seen count, or whose */ \
    /* count is above ceil(max_message_bytes / F), or whose length is not F */ \
    /* (or 1 to F for the last); a record whose span lies outside the */ \
    /* receive ring; an ordering index a window or more ahead of the */ \
    /* channel's read index; and the total of every violation that ended a */ \
    /* connection. REFUSAL: a clean peer never sends one. appended: indices */ \
    /* are the contract. */ \
    X( MESSAGE_BAD_CHANNEL, REFUSAL, NONE ) \
    X( MESSAGE_BAD_BLOCK, REFUSAL, NONE ) \
    X( MESSAGE_BAD_RING, REFUSAL, NONE ) \
    X( MESSAGE_ORDERING_AHEAD, REFUSAL, NONE ) \
    X( MESSAGE_PROTOCOL_VIOLATION, REFUSAL, NONE ) \
    \
    /* Message-object refusal, stale batch entry, and unreleased receive */ \
    /* view. Appended so the enum, names, classes, and families agree. */ \
    X( MESSAGE_SEND_REFUSED, REFUSAL, NONE ) \
    X( MESSAGE_STALE_CONNECTION_ENTRY, EXPECTED, NONE ) \
    X( MESSAGE_NOT_RELEASED, REFUSAL, NONE ) \
    \
    /* The kernel prefilter counters (sdk/rocketnet_wire.h */ \
    /* ROCKETNET_SERVER_XDP_COUNTER_*), surfaced through the transport */ \
    /* counter surface by rocketnet_counters_apply_prefilter */ \
    /* (sdk/rocketnet_counters.cpp). appended: indices are the contract. */ \
    X( XDP_KERNEL_REDIRECTED, EXPECTED, NONE ) \
    X( XDP_KERNEL_DROPPED_TOO_SMALL, REFUSAL, NONE ) \
    X( XDP_KERNEL_DROPPED_TOO_LARGE, REFUSAL, NONE ) \
    X( XDP_KERNEL_DROPPED_WRONG_FILTER, REFUSAL, NONE ) \
    X( XDP_KERNEL_DROPPED_INVALID_TYPE, REFUSAL, NONE ) \
    X( XDP_KERNEL_DROPPED_WRONG_EPOCH, REFUSAL, NONE ) \
    X( XDP_KERNEL_PASSED_IPV6_EXTENSION, EXPECTED, NONE ) \
    \
    /* A connection response that opened for a token already in the */ \
    /* admitted-token set. Retired with the set (docs/SESSION.md 1.6): the */ \
    /* client-id check refuses the second of two addresses holding one */ \
    /* token while the first is connected. appended: indices are the */ \
    /* contract, so it keeps its index and always reads zero. */ \
    X( CONNECTION_RESPONSE_TOKEN_ALREADY_CONNECTED, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    \
    /* A client hand-off deferred because the other send ring is still */ \
    /* handed off or sending (docs/CLIENT-IO.md, Reuse only after sent; a */ \
    /* hand-off never waits; tla/RNClientIo.tla Defer): the committed slots */ \
    /* stay in the Filling ring and leave with the next hand-off. EXPECTED: */ \
    /* deferral is the common case, not an overload signal. appended: */ \
    /* indices are the contract. */ \
    X( CLIENT_SEND_DEFERRED, EXPECTED, NONE ) \
    \
    /* The kernel prefilter counter ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REPLAYED */ \
    /* (sdk/rocketnet_wire.h): a session packet behind the replay map's */ \
    /* high-water window, surfaced by rocketnet_counters_apply_prefilter. */ \
    /* appended: indices are the contract. */ \
    X( XDP_KERNEL_DROPPED_REPLAYED, REFUSAL, NONE ) \
    \
    /* Client received a connection challenge or connection denied packet */ \
    /* whose wire length was not ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES */ \
    /* or ROCKETNET_CONNECTION_DENIED_WIRE_BYTES: refused before the state */ \
    /* gate and before any AEAD open (STANDARD.md, Reading packets). */ \
    /* appended: indices are the contract. */ \
    X( CLIENT_CHALLENGE_WRONG_SIZE, REFUSAL, CLIENT_CHALLENGE_RECEIVED ) \
    X( CLIENT_DENIED_WRONG_SIZE, REFUSAL, CLIENT_DENIED_RECEIVED ) \
    \
    /* The two token-time refusals that were merged into */ \
    /* CONNECTION_REQUEST_TOKEN_EXPIRED, split out so each refusal reason has */ \
    /* its own counter (STANDARD.md, Counters: refusal reasons are never */ \
    /* merged; Processing Connection Requests, the request order). They sit */ \
    /* beside TOKEN_EXPIRED in class and parent, and are appended here because */ \
    /* the indices are the contract. */ \
    X( CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    X( CONNECTION_REQUEST_TOKEN_LIFETIME_TOO_LONG, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    \
    /* A send the driver refused for want of a link-layer destination: the */ \
    /* xdp driver on a segment with no default route, to an address with no */ \
    /* learned MAC that answers no received frame (DRIVERS.md, xdp). Never */ \
    /* sent and never counted as sent. appended: indices are the contract. */ \
    X( SEND_NO_LINK_ADDRESS, REFUSAL, NONE ) \
    \
    /* An UNRELIABLE, UNRELIABLE_SEQUENCED or UNRELIABLE_WITH_ACK_RECEIPT send */ \
    /* too big for one packet's record is promoted to its reliable twin, as */ \
    /* RakNet's ReliabilityLayer does. EXPECTED: promotion is the normal path */ \
    /* for a large unreliable send, one count per send. appended: indices are */ \
    /* the contract. */ \
    X( MESSAGE_PROMOTED_UNRELIABLE, EXPECTED, NONE ) \
    X( MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED, EXPECTED, NONE ) \
    X( MESSAGE_PROMOTED_UNRELIABLE_WITH_ACK_RECEIPT, EXPECTED, NONE ) \
    \
    /* A fitting unreliable message (docs/OUTGOING-BUILD.md) that was not */ \
    /* written in place in a packet buffer and took the queued path, with */ \
    /* its one copy into the frame at send_packets: the transport gave no */ \
    /* frame (a client not yet connected, a full send ring), unreliable */ \
    /* messages already waited in the connection's queue, the send budget */ \
    /* could not pay it at start, or the update's max_packets_per_update */ \
    /* was spent (in-place and queued packets count against it together), */ \
    /* one packet of it being kept for the write pass while reliable work */ \
    /* is due. The queued path then sends it, or drops it over budget and */ \
    /* counts MESSAGE_UNRELIABLE_OVER_BUDGET. EXPECTED: a client's sends */ \
    /* before CONNECTED take it. Counted on the owner's counters, once per */ \
    /* message. appended: indices are the contract. */ \
    X( MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK, EXPECTED, NONE ) \
    \
    /* A datagram the transport's socket dropped because its send call */ \
    /* failed: ENOBUFS from a full interface queue, EAGAIN from a full send */ \
    /* buffer, a refused destination. Read from the driver's send_failures */ \
    /* (the socket's own count, rocketnet_platform_socket_send_failures) */ \
    /* when the counters are read; a driver without it leaves it zero. */ \
    /* appended: indices are the contract. */ \
    X( SEND_FAILED, REFUSAL, NONE )

enum
{
#define ROCKETNET_COUNTER_ENUM_ROW( name, klass, family ) ROCKETNET_COUNTER_##name,
    ROCKETNET_COUNTER_ROWS( ROCKETNET_COUNTER_ENUM_ROW )
#undef ROCKETNET_COUNTER_ENUM_ROW

    ROCKETNET_NUM_COUNTERS
};

// the FAMILY column of a counter that is outside every family
#define ROCKETNET_COUNTER_NONE                          (-1)

struct rocketnet_counters_t
{
    uint64_t values[ROCKETNET_NUM_COUNTERS];

    // Per-server, per-update handshake budget state, not a published counter:
    // how many post-cookie connection requests this server has processed in
    // the current update. Reset by the protocol sweep once per update, so the
    // cap is enforced per server rather than process-wide; the drop it causes
    // is the published counter above.
    uint64_t handshakes_processed_this_update;
};

// Surface a driver's kernel prefilter counters through the transport counter
// surface: prefilter holds ROCKETNET_SERVER_XDP_NUM_COUNTERS values in
// ROCKETNET_SERVER_XDP_COUNTER_* order. Each value is assigned (the kernel
// totals are cumulative) to its transport counter; nothing else moves.
void rocketnet_counters_apply_prefilter( struct rocketnet_counters_t * counters, const uint64_t * prefilter );

#include "rocketnet_internal.h"                  // rocketnet_counter_name lives in the forest header

#endif // #ifndef ROCKETNET_COUNTERS_H
