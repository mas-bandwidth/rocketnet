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
/**
 * @brief Maximum cookie-validated connection requests processed per server update.
 *
 * Requests beyond this limit are refused before asymmetric signature
 * verification and increment `ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED`.
 */
#define ROCKETNET_HANDSHAKE_WORK_BUDGET_PER_UPDATE      1024

// Returned by rocketnet_server_protocol_process_connection_request when the
// handshake budget is exhausted: the next value after
// ROCKETNET_SERVER_PROCESS_ACCEPTED (5). Declared here, in the same header as
// the drop counter it causes, so the test reads the code it asserts from the
// same header that returns it.
/** @brief Status returned when the per-update handshake work budget is exhausted. */
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

/**
 * @brief Append-only source list for the public counter indices and metadata.
 *
 * Each row supplies a counter name, its clean-run class, and the received
 * counter family whose outcomes it contributes to, or `NONE`. The enum, name
 * and class lookup tables, and conservation check are generated from this list.
 * Append new rows at the end to keep existing indices stable.
 * @param X Expansion callback invoked with each counter name, class, and family.
 */
#define ROCKETNET_COUNTER_ROWS( X ) \
    /* One counter per distinct reason, arrival counted separately, per packet */ \
    /* type. The indices are a contract: new counters append. */ \
    \
    /* the packet entry, before any type dispatches */ \
    /** @brief ROCKETNET_COUNTER_PACKETS_RECEIVED: Counts wire datagrams received before packet-type dispatch. Unit: occurrences. Class: arrival; family: none. */ \
    X( PACKETS_RECEIVED, ARRIVAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_PREFILTER_TOO_SHORT: Counts packets refused because they are shorter than the prefilter header. Unit: occurrences. Class: refusal; family: none. */ \
    X( PREFILTER_TOO_SHORT, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_PREFILTER_TOO_LARGE: Counts packets refused because they exceed the prefilter size limit. Unit: occurrences. Class: refusal; family: none. */ \
    X( PREFILTER_TOO_LARGE, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_PREFILTER_WRONG_EPOCH: Counts packets refused because their epoch does not match. Unit: occurrences. Class: refusal; family: none. */ \
    X( PREFILTER_WRONG_EPOCH, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE: Counts packets refused because a prefilter field has an invalid value. Unit: occurrences. Class: refusal; family: none. */ \
    X( PREFILTER_WRONG_VALUE, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_RESERVED_TYPE: Counts packets refused because their packet type is reserved. Unit: occurrences. Class: refusal; family: none. */ \
    X( RESERVED_TYPE, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_WRONG_DIRECTION: Counts packets refused because their type is not valid in this direction. Unit: occurrences. Class: refusal; family: none. */ \
    X( WRONG_DIRECTION, REFUSAL, NONE ) \
    \
    /* connection request processing, one per check in the standard's order */ \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED: Counts incoming connection request packets counted before their outcome is classified. Unit: occurrences. Class: arrival; family: none. */ \
    X( CONNECTION_REQUEST_RECEIVED, ARRIVAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE: Counts connection request packets with a wire size that does not match the packet type. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_WRONG_SIZE, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /* EXPECTED: a request duplicate that a round trip above 100 ms leaves, */ \
    /* arriving after the first has connected. */ \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_ALREADY_CONNECTED: Counts duplicate connection packets received after the connection is established. Unit: occurrences. Class: expected; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_ALREADY_CONNECTED, EXPECTED, CONNECTION_REQUEST_RECEIVED ) \
    /* EXPECTED: the same duplicate arriving while the handshake is in */ \
    /* flight; the stored challenge answers it. */ \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_CHALLENGE_RESENT: Counts duplicate connection requests answered with the stored challenge. Unit: occurrences. Class: expected; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_CHALLENGE_RESENT, EXPECTED, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION: Counts connection requests with an unsupported protocol version. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_WRONG_VERSION, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_PROTOCOL_ID: Counts connection requests with the wrong protocol identifier. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_WRONG_PROTOCOL_ID, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED: Counts connection requests carrying an expired token. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_TOKEN_EXPIRED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_TIMEOUT_INVALID: Counts connection requests carrying an invalid timeout. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_TIMEOUT_INVALID, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_BAD_ADDRESS_FLAGS: Counts connection requests with invalid address flags. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_BAD_ADDRESS_FLAGS, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED: Counts connection requests for an address not advertised by the token. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY: Counts connection requests that do not authenticate to the configured server key. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_WRONG_SERVER_KEY, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /* EXPECTED: the first leg of the cookie exchange, on every connection. */ \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE: Counts first-leg connection requests that ask the server to issue a cookie. Unit: occurrences. Class: expected; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_NO_COOKIE, EXPECTED, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY: Counts connection requests with a cookie that fails verification. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_EXPIRED: Counts connection requests with an expired cookie. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_COOKIE_EXPIRED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED: Counts connection requests that reuse a token. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_TOKEN_ALREADY_USED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY: Counts connection requests whose signature verification fails. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK: Counts connection requests whose sealed block cannot be opened. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_SEALED_MISMATCH: Counts connection requests whose opened sealed data does not match the token. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_SEALED_MISMATCH, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_CLIENT_ID_ALREADY_CONNECTED: Counts connection requests refused because that client identifier is already connected. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_CLIENT_ID_ALREADY_CONNECTED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /* retired with the admitted-token set (docs/SESSION.md 1.6): keeps its */ \
    /* index, always zero. */ \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED: Counts retired counter slot; it stays zero while the connected-client check handles this refusal. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_SMALL_ORDER_KEY: Counts connection request packets refused because their public key has small order. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_SMALL_ORDER_KEY, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS: Counts connection requests refused because no client slot is available. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_NO_SLOTS, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING: Counts connection requests refused because the mapping could not be added. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /* EXPECTED: the success outcome. */ \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_SENT_CHALLENGE: Counts connection requests accepted with a challenge sent to the client. Unit: occurrences. Class: expected; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_SENT_CHALLENGE, EXPECTED, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR: Counts connection requests whose response could not be written. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_INTERNAL_ERROR, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    \
    /* connection response processing, in the standard's order */ \
    /** @brief ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED: Counts incoming connection response packets counted before their outcome is classified. Unit: occurrences. Class: arrival; family: none. */ \
    X( CONNECTION_RESPONSE_RECEIVED, ARRIVAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_RESPONSE_WRONG_SIZE: Counts connection response packets with a wire size that does not match the packet type. Unit: occurrences. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_WRONG_SIZE, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_MAPPING: Counts late packets received after the source mapping has been removed. Unit: occurrences. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_NO_MAPPING, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_RESPONSE_ZERO_SEQUENCE: Counts connection response packets with a zero session sequence. Unit: occurrences. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_ZERO_SEQUENCE, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
      /** @brief ROCKETNET_COUNTER_CONNECTION_RESPONSE_DID_NOT_OPEN: Counts connection response packets whose authenticated contents fail to open. Unit: occurrences. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_DID_NOT_OPEN, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_RESPONSE_PADDING_NOT_ZERO: Counts connection response packets that opened successfully but have non-zero padding bytes. Unit: occurrences. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_PADDING_NOT_ZERO, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /* EXPECTED: a response duplicate arriving after the first has connected. */ \
    /** @brief ROCKETNET_COUNTER_CONNECTION_RESPONSE_ALREADY_CONNECTED: Counts duplicate connection packets received after the connection is established. Unit: occurrences. Class: expected; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_ALREADY_CONNECTED, EXPECTED, CONNECTION_RESPONSE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED: Counts connection responses refused because that client identifier is already connected. Unit: occurrences. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /* retired with the admitted-token set (docs/SESSION.md 1.6): keeps its */ \
    /* index, always zero. */ \
    /** @brief ROCKETNET_COUNTER_CONNECTION_RESPONSE_ADMITTED_SET_FULL: Counts retired counter slot; it stays zero after the admitted-token set is removed. Unit: occurrences. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_ADMITTED_SET_FULL, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_SLOTS: Counts connection responses refused because no client slot is available. Unit: occurrences. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_NO_SLOTS, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CONNECTION_RESPONSE_CONNECTED: Counts connection responses that complete the handshake. Unit: occurrences. Class: expected; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_CONNECTED, EXPECTED, CONNECTION_RESPONSE_RECEIVED ) \
    \
    /* keep-alive, payload and disconnect at the server */ \
    /** @brief ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED: Counts incoming keep alive packets counted before their outcome is classified. Unit: occurrences. Class: arrival; family: none. */ \
    X( KEEP_ALIVE_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race. The server ended the session (a */ \
    /* disconnect, a timeout) with the client's packets in flight; no network */ \
    /* fault is needed (test_unit_client_receive_counters.cpp, */ \
    /* test_session_end_races_move_only_expected_counters). */ \
    /** @brief ROCKETNET_COUNTER_KEEP_ALIVE_NO_MAPPING: Counts late packets received after the source mapping has been removed. Unit: occurrences. Class: expected; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_NO_MAPPING, EXPECTED, KEEP_ALIVE_RECEIVED ) \
    /* REFUSAL from here to the end of the type: a refused packet reads 0 on */ \
    /* a clean path. */ \
    /** @brief ROCKETNET_COUNTER_KEEP_ALIVE_ZERO_SEQUENCE: Counts keep alive packets with a zero session sequence. Unit: occurrences. Class: refusal; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_ZERO_SEQUENCE, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_KEEP_ALIVE_ALREADY_RECEIVED: Counts keep alive packets whose sequence was already received. Unit: occurrences. Class: refusal; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_ALREADY_RECEIVED, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_KEEP_ALIVE_DID_NOT_OPEN: Counts keep alive packets whose authenticated contents fail to open. Unit: occurrences. Class: refusal; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_DID_NOT_OPEN, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_KEEP_ALIVE_NOT_CONNECTED: Counts keep alive packets received before the session is connected. Unit: occurrences. Class: refusal; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_NOT_CONNECTED, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED: Counts keep alive packets accepted by the protocol. Unit: occurrences. Class: expected; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_ACCEPTED, EXPECTED, KEEP_ALIVE_RECEIVED ) \
    \
    /** @brief ROCKETNET_COUNTER_PAYLOAD_RECEIVED: Counts incoming payload packets counted before their outcome is classified. Unit: occurrences. Class: arrival; family: none. */ \
    X( PAYLOAD_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race, as KEEP_ALIVE_NO_MAPPING. */ \
    /** @brief ROCKETNET_COUNTER_PAYLOAD_NO_MAPPING: Counts late packets received after the source mapping has been removed. Unit: occurrences. Class: expected; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_NO_MAPPING, EXPECTED, PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_PAYLOAD_ZERO_SEQUENCE: Counts payload packets with a zero session sequence. Unit: occurrences. Class: refusal; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_ZERO_SEQUENCE, REFUSAL, PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED: Counts payload packets whose sequence was already received. Unit: occurrences. Class: refusal; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_ALREADY_RECEIVED, REFUSAL, PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN: Counts payload packets whose authenticated contents fail to open. Unit: occurrences. Class: refusal; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_DID_NOT_OPEN, REFUSAL, PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_PAYLOAD_NOT_CONNECTED: Counts payload packets received before the session is connected. Unit: occurrences. Class: refusal; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_NOT_CONNECTED, REFUSAL, PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_PAYLOAD_DELIVERED: Counts payloads delivered to the application. Unit: occurrences. Class: expected; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_DELIVERED, EXPECTED, PAYLOAD_RECEIVED ) \
    \
    /** @brief ROCKETNET_COUNTER_DISCONNECT_RECEIVED: Counts incoming disconnect packets counted before their outcome is classified. Unit: occurrences. Class: arrival; family: none. */ \
    X( DISCONNECT_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the copies of the redundant disconnect burst after the first */ \
    /* has torn the mapping down. */ \
    /** @brief ROCKETNET_COUNTER_DISCONNECT_NO_MAPPING: Counts late packets received after the source mapping has been removed. Unit: occurrences. Class: expected; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_NO_MAPPING, EXPECTED, DISCONNECT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_DISCONNECT_ZERO_SEQUENCE: Counts disconnect packets with a zero session sequence. Unit: occurrences. Class: refusal; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_ZERO_SEQUENCE, REFUSAL, DISCONNECT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_DISCONNECT_ALREADY_RECEIVED: Counts disconnect packets whose sequence was already received. Unit: occurrences. Class: refusal; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_ALREADY_RECEIVED, REFUSAL, DISCONNECT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_DISCONNECT_DID_NOT_OPEN: Counts disconnect packets whose authenticated contents fail to open. Unit: occurrences. Class: refusal; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_DID_NOT_OPEN, REFUSAL, DISCONNECT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_DISCONNECT_NOT_CONNECTED: Counts disconnect packets received before the session is connected. Unit: occurrences. Class: refusal; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_NOT_CONNECTED, REFUSAL, DISCONNECT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED: Counts disconnect packets that end the active connection. Unit: occurrences. Class: expected; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_DISCONNECTED, EXPECTED, DISCONNECT_RECEIVED ) \
    \
    /* mapping and client lifecycle */ \
    /* EXPECTED, as are the lifecycle counters through CLIENT_TIMED_OUT: they */ \
    /* count connections coming and going, which a clean run does by design. */ \
    /** @brief ROCKETNET_COUNTER_MAPPING_CREATED: Counts server connection mappings created. Unit: occurrences. Class: expected; family: none. */ \
    X( MAPPING_CREATED, EXPECTED, NONE ) \
    /** @brief ROCKETNET_COUNTER_MAPPING_IDLE_TIMEOUT: Counts mappings removed after the idle timeout. Unit: occurrences. Class: expected; family: none. */ \
    X( MAPPING_IDLE_TIMEOUT, EXPECTED, NONE ) \
    /** @brief ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED: Counts mappings removed after the handshake deadline. Unit: occurrences. Class: expected; family: none. */ \
    X( MAPPING_HANDSHAKE_DEADLINE_EXPIRED, EXPECTED, NONE ) \
    /** @brief ROCKETNET_COUNTER_MAPPING_TABLE_FULL: Counts connection requests refused because the mapping table is full. Unit: occurrences. Class: refusal; family: none. */ \
    X( MAPPING_TABLE_FULL, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_CONNECTED: Counts client connections established. Unit: occurrences. Class: expected; family: none. */ \
    X( CLIENT_CONNECTED, EXPECTED, NONE ) \
    /* REFUSAL: a resource exhaustion is a refusal. */ \
    /** @brief ROCKETNET_COUNTER_SEND_RING_FULL: Counts packets refused because the send ring has no free slot, plus committed packets destroy drops at its wait deadline. Unit: occurrences. Class: refusal; family: none. */ \
    X( SEND_RING_FULL, REFUSAL, NONE ) \
    /* REFUSAL: an internal state the shipped protocol cannot produce. */ \
    /** @brief ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING: Counts internal events that find a client slot without a mapping. Unit: occurrences. Class: refusal; family: none. */ \
    X( SLOT_WITHOUT_MAPPING, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_DISCONNECTED: Counts client connections closed locally. Unit: occurrences. Class: expected; family: none. */ \
    X( CLIENT_DISCONNECTED, EXPECTED, NONE ) \
    /** @brief ROCKETNET_COUNTER_SERVER_DISCONNECTED_CLIENT: Counts client connections closed by the server. Unit: occurrences. Class: expected; family: none. */ \
    X( SERVER_DISCONNECTED_CLIENT, EXPECTED, NONE ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_TIMED_OUT: Counts client connections closed after timing out. Unit: occurrences. Class: expected; family: none. */ \
    X( CLIENT_TIMED_OUT, EXPECTED, NONE ) \
    \
    /* the client's receive path. the shared packet counters above serve both */ \
    /* directions; these are the client-specific reasons. */ \
    /** @brief ROCKETNET_COUNTER_CLIENT_COOKIE_RECEIVED: Counts incoming client cookie packets counted before their outcome is classified. Unit: occurrences. Class: arrival; family: none. */ \
    X( CLIENT_COOKIE_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: a cookie duplicate arriving after the client moved on, the */ \
    /* same round-trip duplicate as ALREADY_CONNECTED. */ \
    /** @brief ROCKETNET_COUNTER_CLIENT_COOKIE_WRONG_STATE: Counts client cookie packets received in a protocol state that does not accept them. Unit: occurrences. Class: expected; family: CLIENT_COOKIE_RECEIVED. */ \
    X( CLIENT_COOKIE_WRONG_STATE, EXPECTED, CLIENT_COOKIE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_COOKIE_SALT_MISMATCH: Counts cookies refused because their salt does not match the connection attempt. Unit: occurrences. Class: refusal; family: CLIENT_COOKIE_RECEIVED. */ \
    X( CLIENT_COOKIE_SALT_MISMATCH, REFUSAL, CLIENT_COOKIE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_COOKIE_STORED: Counts cookies stored for the active connection attempt. Unit: occurrences. Class: expected; family: CLIENT_COOKIE_RECEIVED. */ \
    X( CLIENT_COOKIE_STORED, EXPECTED, CLIENT_COOKIE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_CHALLENGE_RECEIVED: Counts incoming client challenge packets counted before their outcome is classified. Unit: occurrences. Class: arrival; family: none. */ \
    X( CLIENT_CHALLENGE_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the server resends the challenge for a duplicate request; */ \
    /* the client no longer has a question for it. */ \
    /** @brief ROCKETNET_COUNTER_CLIENT_CHALLENGE_WRONG_STATE: Counts client challenge packets received in a protocol state that does not accept them. Unit: occurrences. Class: expected; family: CLIENT_CHALLENGE_RECEIVED. */ \
    X( CLIENT_CHALLENGE_WRONG_STATE, EXPECTED, CLIENT_CHALLENGE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_CHALLENGE_ZERO_SEQUENCE: Counts client challenge packets with a zero session sequence. Unit: occurrences. Class: refusal; family: CLIENT_CHALLENGE_RECEIVED. */ \
    X( CLIENT_CHALLENGE_ZERO_SEQUENCE, REFUSAL, CLIENT_CHALLENGE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_CHALLENGE_DID_NOT_OPEN: Counts client challenge packets whose authenticated contents fail to open. Unit: occurrences. Class: refusal; family: CLIENT_CHALLENGE_RECEIVED. */ \
    X( CLIENT_CHALLENGE_DID_NOT_OPEN, REFUSAL, CLIENT_CHALLENGE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_CHALLENGE_SMALL_ORDER_KEY: Counts client challenge packets refused because their public key has small order. Unit: occurrences. Class: refusal; family: CLIENT_CHALLENGE_RECEIVED. */ \
    X( CLIENT_CHALLENGE_SMALL_ORDER_KEY, REFUSAL, CLIENT_CHALLENGE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_CHALLENGE_ACCEPTED: Counts client challenge packets accepted by the protocol. Unit: occurrences. Class: expected; family: CLIENT_CHALLENGE_RECEIVED. */ \
    X( CLIENT_CHALLENGE_ACCEPTED, EXPECTED, CLIENT_CHALLENGE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_DENIED_RECEIVED: Counts client denied packets received from the server before their outcome is classified. Unit: occurrences. Class: arrival; family: none. */ \
    X( CLIENT_DENIED_RECEIVED, ARRIVAL, NONE ) \
    /* REFUSAL: a denied reaches a client only when the server is out of */ \
    /* slots. */ \
    /** @brief ROCKETNET_COUNTER_CLIENT_DENIED_WRONG_STATE: Counts client denied packets received in a protocol state that does not accept them. Unit: occurrences. Class: refusal; family: CLIENT_DENIED_RECEIVED. */ \
    X( CLIENT_DENIED_WRONG_STATE, REFUSAL, CLIENT_DENIED_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_DENIED_ZERO_SEQUENCE: Counts client denied packets with a zero session sequence. Unit: occurrences. Class: refusal; family: CLIENT_DENIED_RECEIVED. */ \
    X( CLIENT_DENIED_ZERO_SEQUENCE, REFUSAL, CLIENT_DENIED_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_DENIED_DID_NOT_OPEN: Counts client denied packets whose authenticated contents fail to open. Unit: occurrences. Class: refusal; family: CLIENT_DENIED_RECEIVED. */ \
    X( CLIENT_DENIED_DID_NOT_OPEN, REFUSAL, CLIENT_DENIED_RECEIVED ) \
    /* REFUSAL: the server denied the connection. The server denies only for */ \
    /* NO_SLOTS, so a clean run reads 0. */ \
    /** @brief ROCKETNET_COUNTER_CLIENT_DENIED_ACCEPTED: Counts client denied packets accepted by the protocol. Unit: occurrences. Class: refusal; family: CLIENT_DENIED_RECEIVED. */ \
    X( CLIENT_DENIED_ACCEPTED, REFUSAL, CLIENT_DENIED_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_RECEIVED: Counts incoming client keep alive packets counted before their outcome is classified. Unit: occurrences. Class: arrival; family: none. */ \
    X( CLIENT_KEEP_ALIVE_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race, from the client's side. The client */ \
    /* left the session (a disconnect) with the server's packets in flight. A */ \
    /* loss-free soak_sim wire moves it; so do CLIENT_PAYLOAD_, CLIENT_ACK_, */ \
    /* CLIENT_FRAGMENT_ and CLIENT_DISCONNECT_WRONG_STATE (the disconnect */ \
    /* burst's copies). */ \
    /** @brief ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_STATE: Counts client keep alive packets received in a protocol state that does not accept them. Unit: occurrences. Class: expected; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_WRONG_STATE, EXPECTED, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ZERO_SEQUENCE: Counts client keep alive packets with a zero session sequence. Unit: occurrences. Class: refusal; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_ZERO_SEQUENCE, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ALREADY_RECEIVED: Counts client keep alive packets whose sequence was already received. Unit: occurrences. Class: refusal; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_ALREADY_RECEIVED, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_DID_NOT_OPEN: Counts client keep alive packets whose authenticated contents fail to open. Unit: occurrences. Class: refusal; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_DID_NOT_OPEN, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ACCEPTED: Counts client keep alive packets accepted by the protocol. Unit: occurrences. Class: expected; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_ACCEPTED, EXPECTED, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED: Counts incoming client payload packets counted before their outcome is classified. Unit: occurrences. Class: arrival; family: none. */ \
    X( CLIENT_PAYLOAD_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race, as CLIENT_KEEP_ALIVE_WRONG_STATE. */ \
    /** @brief ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_STATE: Counts client payload packets received in a protocol state that does not accept them. Unit: occurrences. Class: expected; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_WRONG_STATE, EXPECTED, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_PAYLOAD_ZERO_SEQUENCE: Counts client payload packets with a zero session sequence. Unit: occurrences. Class: refusal; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_ZERO_SEQUENCE, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED: Counts client payload packets whose sequence was already received. Unit: occurrences. Class: refusal; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_ALREADY_RECEIVED, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_PAYLOAD_DID_NOT_OPEN: Counts client payload packets whose authenticated contents fail to open. Unit: occurrences. Class: refusal; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_DID_NOT_OPEN, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED: Counts payloads delivered to the application. Unit: occurrences. Class: expected; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_DELIVERED, EXPECTED, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED: Counts incoming client disconnect packets counted before their outcome is classified. Unit: occurrences. Class: arrival; family: none. */ \
    X( CLIENT_DISCONNECT_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the copies of the server's redundant disconnect burst after the first. */ \
    /** @brief ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_STATE: Counts client disconnect packets received in a protocol state that does not accept them. Unit: occurrences. Class: expected; family: CLIENT_DISCONNECT_RECEIVED. */ \
    X( CLIENT_DISCONNECT_WRONG_STATE, EXPECTED, CLIENT_DISCONNECT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_DISCONNECT_ZERO_SEQUENCE: Counts client disconnect packets with a zero session sequence. Unit: occurrences. Class: refusal; family: CLIENT_DISCONNECT_RECEIVED. */ \
    X( CLIENT_DISCONNECT_ZERO_SEQUENCE, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_DISCONNECT_ALREADY_RECEIVED: Counts client disconnect packets whose sequence was already received. Unit: occurrences. Class: refusal; family: CLIENT_DISCONNECT_RECEIVED. */ \
    X( CLIENT_DISCONNECT_ALREADY_RECEIVED, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_DISCONNECT_DID_NOT_OPEN: Counts client disconnect packets whose authenticated contents fail to open. Unit: occurrences. Class: refusal; family: CLIENT_DISCONNECT_RECEIVED. */ \
    X( CLIENT_DISCONNECT_DID_NOT_OPEN, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_DISCONNECT_ACCEPTED: Counts client disconnect packets accepted by the protocol. Unit: occurrences. Class: expected; family: CLIENT_DISCONNECT_RECEIVED. */ \
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
    /** @brief ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_SIZE: Counts keep alive packets with a wire size that does not match the packet type. Unit: occurrences. Class: refusal; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_WRONG_SIZE, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_KEEP_ALIVE_BAD_ACK_BLOCK: Counts keep alive packets with an invalid acknowledgement block. Unit: occurrences. Class: refusal; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_BAD_ACK_BLOCK, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_PAYLOAD_WRONG_SIZE: Counts payload packets with a wire size that does not match the packet type. Unit: occurrences. Class: refusal; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_WRONG_SIZE, REFUSAL, PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_PAYLOAD_BAD_ACK_BLOCK: Counts payload packets with an invalid acknowledgement block. Unit: occurrences. Class: refusal; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_BAD_ACK_BLOCK, REFUSAL, PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_DISCONNECT_WRONG_SIZE: Counts disconnect packets with a wire size that does not match the packet type. Unit: occurrences. Class: refusal; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_WRONG_SIZE, REFUSAL, DISCONNECT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_DISCONNECT_BAD_ACK_BLOCK: Counts disconnect packets with an invalid acknowledgement block. Unit: occurrences. Class: refusal; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_BAD_ACK_BLOCK, REFUSAL, DISCONNECT_RECEIVED ) \
    \
    /** @brief ROCKETNET_COUNTER_ACK_RECEIVED: Counts incoming ack packets counted before their outcome is classified. Unit: occurrences. Class: arrival; family: none. */ \
    X( ACK_RECEIVED, ARRIVAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_ACK_WRONG_SIZE: Counts ack packets with a wire size that does not match the packet type. Unit: occurrences. Class: refusal; family: ACK_RECEIVED. */ \
    X( ACK_WRONG_SIZE, REFUSAL, ACK_RECEIVED ) \
    /* EXPECTED: the session-end race, as KEEP_ALIVE_NO_MAPPING. */ \
    /** @brief ROCKETNET_COUNTER_ACK_NO_MAPPING: Counts late packets received after the source mapping has been removed. Unit: occurrences. Class: expected; family: ACK_RECEIVED. */ \
    X( ACK_NO_MAPPING, EXPECTED, ACK_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_ACK_ZERO_SEQUENCE: Counts ack packets with a zero session sequence. Unit: occurrences. Class: refusal; family: ACK_RECEIVED. */ \
    X( ACK_ZERO_SEQUENCE, REFUSAL, ACK_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_ACK_ALREADY_RECEIVED: Counts ack packets whose sequence was already received. Unit: occurrences. Class: refusal; family: ACK_RECEIVED. */ \
    X( ACK_ALREADY_RECEIVED, REFUSAL, ACK_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_ACK_DID_NOT_OPEN: Counts ack packets whose authenticated contents fail to open. Unit: occurrences. Class: refusal; family: ACK_RECEIVED. */ \
    X( ACK_DID_NOT_OPEN, REFUSAL, ACK_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_ACK_NOT_CONNECTED: Counts ack packets received before the session is connected. Unit: occurrences. Class: refusal; family: ACK_RECEIVED. */ \
    X( ACK_NOT_CONNECTED, REFUSAL, ACK_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_ACK_BAD_ACK_BLOCK: Counts ack packets with an invalid acknowledgement block. Unit: occurrences. Class: refusal; family: ACK_RECEIVED. */ \
    X( ACK_BAD_ACK_BLOCK, REFUSAL, ACK_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_ACK_ACCEPTED: Counts ack packets accepted by the protocol. Unit: occurrences. Class: expected; family: ACK_RECEIVED. */ \
    X( ACK_ACCEPTED, EXPECTED, ACK_RECEIVED ) \
    \
    /* EXPECTED: a side counter. The packet that fires it still succeeds. */ \
    /** @brief ROCKETNET_COUNTER_ACK_UNKNOWN_SEQUENCE: Counts acknowledgement entries that name a sequence no longer in the sent history. Unit: occurrences. Class: expected; family: none. */ \
    X( ACK_UNKNOWN_SEQUENCE, EXPECTED, NONE ) \
    /* REFUSAL: a resource exhaustion (the delivery batch). The deferred id */ \
    /* is not lost. */ \
    /** @brief ROCKETNET_COUNTER_ACKED_BATCH_FULL: Counts acknowledged packet notifications deferred because the delivery batch is full. Unit: occurrences. Class: refusal; family: none. */ \
    X( ACKED_BATCH_FULL, REFUSAL, NONE ) \
    \
    /** @brief ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_SIZE: Counts client keep alive packets with a wire size that does not match the packet type. Unit: occurrences. Class: refusal; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_WRONG_SIZE, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK: Counts client keep alive packets with an invalid acknowledgement block. Unit: occurrences. Class: refusal; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_SIZE: Counts client payload packets with a wire size that does not match the packet type. Unit: occurrences. Class: refusal; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_WRONG_SIZE, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_PAYLOAD_BAD_ACK_BLOCK: Counts client payload packets with an invalid acknowledgement block. Unit: occurrences. Class: refusal; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_BAD_ACK_BLOCK, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_SIZE: Counts client disconnect packets with a wire size that does not match the packet type. Unit: occurrences. Class: refusal; family: CLIENT_DISCONNECT_RECEIVED. */ \
    X( CLIENT_DISCONNECT_WRONG_SIZE, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_DISCONNECT_BAD_ACK_BLOCK: Counts client disconnect packets with an invalid acknowledgement block. Unit: occurrences. Class: refusal; family: CLIENT_DISCONNECT_RECEIVED. */ \
    X( CLIENT_DISCONNECT_BAD_ACK_BLOCK, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    \
    /** @brief ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED: Counts incoming client ack packets counted before their outcome is classified. Unit: occurrences. Class: arrival; family: none. */ \
    X( CLIENT_ACK_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race, as CLIENT_KEEP_ALIVE_WRONG_STATE. */ \
    /** @brief ROCKETNET_COUNTER_CLIENT_ACK_WRONG_STATE: Counts client ack packets received in a protocol state that does not accept them. Unit: occurrences. Class: expected; family: CLIENT_ACK_RECEIVED. */ \
    X( CLIENT_ACK_WRONG_STATE, EXPECTED, CLIENT_ACK_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_ACK_WRONG_SIZE: Counts client ack packets with a wire size that does not match the packet type. Unit: occurrences. Class: refusal; family: CLIENT_ACK_RECEIVED. */ \
    X( CLIENT_ACK_WRONG_SIZE, REFUSAL, CLIENT_ACK_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_ACK_ZERO_SEQUENCE: Counts client ack packets with a zero session sequence. Unit: occurrences. Class: refusal; family: CLIENT_ACK_RECEIVED. */ \
    X( CLIENT_ACK_ZERO_SEQUENCE, REFUSAL, CLIENT_ACK_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_ACK_ALREADY_RECEIVED: Counts client ack packets whose sequence was already received. Unit: occurrences. Class: refusal; family: CLIENT_ACK_RECEIVED. */ \
    X( CLIENT_ACK_ALREADY_RECEIVED, REFUSAL, CLIENT_ACK_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_ACK_DID_NOT_OPEN: Counts client ack packets whose authenticated contents fail to open. Unit: occurrences. Class: refusal; family: CLIENT_ACK_RECEIVED. */ \
    X( CLIENT_ACK_DID_NOT_OPEN, REFUSAL, CLIENT_ACK_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_ACK_BAD_ACK_BLOCK: Counts client ack packets with an invalid acknowledgement block. Unit: occurrences. Class: refusal; family: CLIENT_ACK_RECEIVED. */ \
    X( CLIENT_ACK_BAD_ACK_BLOCK, REFUSAL, CLIENT_ACK_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_ACK_ACCEPTED: Counts client ack packets accepted by the protocol. Unit: occurrences. Class: expected; family: CLIENT_ACK_RECEIVED. */ \
    X( CLIENT_ACK_ACCEPTED, EXPECTED, CLIENT_ACK_RECEIVED ) \
    \
    /** @brief ROCKETNET_COUNTER_CLIENT_ACK_UNKNOWN_SEQUENCE: Counts client acknowledgement entries that name a sequence no longer in the sent history. Unit: occurrences. Class: expected; family: none. */ \
    X( CLIENT_ACK_UNKNOWN_SEQUENCE, EXPECTED, NONE ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_ACKED_BATCH_FULL: Counts client acknowledged packet notifications deferred because the delivery batch is full. Unit: occurrences. Class: refusal; family: none. */ \
    X( CLIENT_ACKED_BATCH_FULL, REFUSAL, NONE ) \
    \
    /* WRONG_OPENED_SIZE: the opened data, less the ack block, did not match */ \
    /* the type -- its own reason, distinct from WRONG_SIZE (the wire floor) */ \
    /* and DID_NOT_OPEN (the seal refused), per the one-counter-per-reason */ \
    /* rule. Appended per the append-only index contract. */ \
    \
    /** @brief ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_OPENED_SIZE: Counts keep alive packets whose authenticated payload has an invalid size. Unit: occurrences. Class: refusal; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_WRONG_OPENED_SIZE, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_PAYLOAD_WRONG_OPENED_SIZE: Counts payload packets whose authenticated payload has an invalid size. Unit: occurrences. Class: refusal; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_WRONG_OPENED_SIZE, REFUSAL, PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_DISCONNECT_WRONG_OPENED_SIZE: Counts disconnect packets whose authenticated payload has an invalid size. Unit: occurrences. Class: refusal; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_WRONG_OPENED_SIZE, REFUSAL, DISCONNECT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_ACK_WRONG_OPENED_SIZE: Counts ack packets whose authenticated payload has an invalid size. Unit: occurrences. Class: refusal; family: ACK_RECEIVED. */ \
    X( ACK_WRONG_OPENED_SIZE, REFUSAL, ACK_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE: Counts client keep alive packets whose authenticated payload has an invalid size. Unit: occurrences. Class: refusal; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_OPENED_SIZE: Counts client payload packets whose authenticated payload has an invalid size. Unit: occurrences. Class: refusal; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_WRONG_OPENED_SIZE, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_OPENED_SIZE: Counts client disconnect packets whose authenticated payload has an invalid size. Unit: occurrences. Class: refusal; family: CLIENT_DISCONNECT_RECEIVED. */ \
    X( CLIENT_DISCONNECT_WRONG_OPENED_SIZE, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_ACK_WRONG_OPENED_SIZE: Counts client ack packets whose authenticated payload has an invalid size. Unit: occurrences. Class: refusal; family: CLIENT_ACK_RECEIVED. */ \
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
    /** @brief ROCKETNET_COUNTER_FRAGMENT_RECEIVED: Counts incoming fragment packets counted before their outcome is classified. Unit: occurrences. Class: arrival; family: none. */ \
    X( FRAGMENT_RECEIVED, ARRIVAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_WRONG_SIZE: Counts fragment packets with a wire size that does not match the packet type. Unit: occurrences. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_WRONG_SIZE, REFUSAL, FRAGMENT_RECEIVED ) \
    /* EXPECTED: the session-end race, as KEEP_ALIVE_NO_MAPPING. */ \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_NO_MAPPING: Counts late packets received after the source mapping has been removed. Unit: occurrences. Class: expected; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_NO_MAPPING, EXPECTED, FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_ZERO_SEQUENCE: Counts fragment packets with a zero session sequence. Unit: occurrences. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_ZERO_SEQUENCE, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_ALREADY_RECEIVED: Counts fragment packets whose sequence was already received. Unit: occurrences. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_ALREADY_RECEIVED, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_DID_NOT_OPEN: Counts fragment packets whose authenticated contents fail to open. Unit: occurrences. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_DID_NOT_OPEN, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_NOT_CONNECTED: Counts fragment packets received before the session is connected. Unit: occurrences. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_NOT_CONNECTED, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_BAD_ACK_BLOCK: Counts fragment packets with an invalid acknowledgement block. Unit: occurrences. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_BAD_ACK_BLOCK, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_WRONG_OPENED_SIZE: Counts fragment packets whose authenticated payload has an invalid size. Unit: occurrences. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_WRONG_OPENED_SIZE, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID: Counts fragments refused because their payload identifier is zero. Unit: occurrences. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_ZERO_PAYLOAD_ID, REFUSAL, FRAGMENT_RECEIVED ) \
    /* PATH_LOSS: a fragment of a payload an older loss or reorder left */ \
    /* behind. */ \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_STALE: Counts fragments ignored because their payload sequence is stale. Unit: occurrences. Class: path_loss; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_STALE, PATH_LOSS, FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID: Counts fragments refused because their fragment count is invalid. Unit: occurrences. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_COUNT_INVALID, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE: Counts fragments refused because their index is outside the fragment count. Unit: occurrences. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_INDEX_OUT_OF_RANGE, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH: Counts fragments refused because their count differs from the payload first seen. Unit: occurrences. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_COUNT_MISMATCH, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH: Counts fragments refused because their size disagrees with the payload shape. Unit: occurrences. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_SIZE_MISMATCH, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE: Counts payloads refused because their size exceeds the configured maximum. Unit: occurrences. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_PAYLOAD_TOO_LARGE, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX: Counts fragments refused because their index has already been received. Unit: occurrences. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_DUPLICATE_INDEX, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_ACCEPTED: Counts fragment packets accepted by the protocol. Unit: occurrences. Class: expected; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_ACCEPTED, EXPECTED, FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED: Counts fragmented payloads completed by reassembly. Unit: occurrences. Class: expected; family: none. */ \
    X( FRAGMENT_PAYLOAD_REASSEMBLED, EXPECTED, NONE ) \
    /* PATH_LOSS: a lost fragment loses its whole payload, by design. */ \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED: Counts fragmented payloads abandoned after a fragment is lost or reordered. Unit: occurrences. Class: path_loss; family: none. */ \
    X( FRAGMENT_PAYLOAD_ABANDONED, PATH_LOSS, NONE ) \
    /** @brief ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL: Counts reassembled payloads dropped because the update's receive batch is full. Unit: occurrences. Class: refusal; family: none. */ \
    X( FRAGMENT_PAYLOAD_BATCH_FULL, REFUSAL, NONE ) \
    \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED: Counts incoming client fragment packets counted before their outcome is classified. Unit: occurrences. Class: arrival; family: none. */ \
    X( CLIENT_FRAGMENT_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race, as CLIENT_KEEP_ALIVE_WRONG_STATE. */ \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_STATE: Counts client fragment packets received in a protocol state that does not accept them. Unit: occurrences. Class: expected; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_WRONG_STATE, EXPECTED, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_SIZE: Counts client fragment packets with a wire size that does not match the packet type. Unit: occurrences. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_WRONG_SIZE, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_ZERO_SEQUENCE: Counts client fragment packets with a zero session sequence. Unit: occurrences. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_ZERO_SEQUENCE, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_ALREADY_RECEIVED: Counts client fragment packets whose sequence was already received. Unit: occurrences. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_ALREADY_RECEIVED, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_DID_NOT_OPEN: Counts client fragment packets whose authenticated contents fail to open. Unit: occurrences. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_DID_NOT_OPEN, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_BAD_ACK_BLOCK: Counts client fragment packets with an invalid acknowledgement block. Unit: occurrences. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_BAD_ACK_BLOCK, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_OPENED_SIZE: Counts client fragment packets whose authenticated payload has an invalid size. Unit: occurrences. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_WRONG_OPENED_SIZE, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_ZERO_PAYLOAD_ID: Counts client fragments refused because their payload identifier is zero. Unit: occurrences. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_ZERO_PAYLOAD_ID, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_STALE: Counts client fragments ignored because their payload sequence is stale. Unit: occurrences. Class: path_loss; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_STALE, PATH_LOSS, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_COUNT_INVALID: Counts client fragments refused because their fragment count is invalid. Unit: occurrences. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_COUNT_INVALID, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_INDEX_OUT_OF_RANGE: Counts client fragments refused because their index is outside the fragment count. Unit: occurrences. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_INDEX_OUT_OF_RANGE, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_COUNT_MISMATCH: Counts client fragments refused because their count differs from the payload first seen. Unit: occurrences. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_COUNT_MISMATCH, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_SIZE_MISMATCH: Counts client fragments refused because their size disagrees with the payload shape. Unit: occurrences. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_SIZE_MISMATCH, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_TOO_LARGE: Counts client payloads refused because their size exceeds the configured maximum. Unit: occurrences. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_PAYLOAD_TOO_LARGE, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_DUPLICATE_INDEX: Counts client fragments refused because their index has already been received. Unit: occurrences. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_DUPLICATE_INDEX, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_ACCEPTED: Counts client fragment packets accepted by the protocol. Unit: occurrences. Class: expected; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_ACCEPTED, EXPECTED, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED: Counts client fragmented payloads completed by reassembly. Unit: occurrences. Class: expected; family: none. */ \
    X( CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED, EXPECTED, NONE ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_ABANDONED: Counts client fragmented payloads abandoned after a fragment is lost or reordered. Unit: occurrences. Class: path_loss; family: none. */ \
    X( CLIENT_FRAGMENT_PAYLOAD_ABANDONED, PATH_LOSS, NONE ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL: Counts client reassembled payloads dropped because the update's receive batch is full. Unit: occurrences. Class: refusal; family: none. */ \
    X( CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL, REFUSAL, NONE ) \
    \
    /* the send side: a payload requested above the wire's capacity found */ \
    /* the fill phase's arena exhausted, and was dropped at start_packet -- */ \
    /* the arena sibling of SEND_RING_FULL */ \
    \
    /** @brief ROCKETNET_COUNTER_SEND_ARENA_FULL: Counts packets refused because the fill-phase send arena is exhausted. Unit: occurrences. Class: refusal; family: none. */ \
    X( SEND_ARENA_FULL, REFUSAL, NONE ) \
    \
    /* the copying receive cursor was handed a buffer smaller than a */ \
    /* delivered payload: the payload is refused whole, never truncated. */ \
    /* stays zero for every caller sizing by the configured maximum. */ \
    \
    /** @brief ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL: Counts payloads refused whole because the caller receive buffer is too small. Unit: occurrences. Class: refusal; family: none. */ \
    X( RECEIVE_CURSOR_TOO_SMALL, REFUSAL, NONE ) \
    \
    /* History was overwritten before its ACK with no later ACK to say what */ \
    /* became of the packet (a silent receiver, or a window too small for the */ \
    /* path), or a writer fell behind the retained sequence window. A */ \
    /* refusal of history: it reads zero on a healthy path. Advisory: the */ \
    /* packet is still transmitted. An eviction the receiver's later ACKs mark */ \
    /* as lost is SEND_HISTORY_LOST instead (tla/RNSentHistory.tla). */ \
    /* REFUSAL: a resource exhaustion. soak_sim moves it only when its wire */ \
    /* loses packets, and says so. */ \
    /** @brief ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW: Counts unacknowledged send records overwritten before the receiver acknowledges them. Unit: occurrences. Class: refusal; family: none. */ \
    X( SEND_HISTORY_OVERFLOW, REFUSAL, NONE ) \
    /* Private packet metadata budget exhausted during a fill phase. */ \
    /** @brief ROCKETNET_COUNTER_SEND_LEASE_FULL: Counts packet leases refused because the private metadata table is full. Unit: occurrences. Class: refusal; family: none. */ \
    X( SEND_LEASE_FULL, REFUSAL, NONE ) \
    \
    /* a connection's sealed packets failed to open ROCKETNET_INTEGRITY_LIMIT */ \
    /* times across its keys and it was disconnected, per the standard's AEAD */ \
    /* integrity limit. a lifecycle counter, once per torn-down connection, */ \
    /* on whichever end reached the limit. appended: indices are the contract. */ \
    /* REFUSAL: a connection torn down after failed opens, never on a clean */ \
    /* path (its comment calls it a lifecycle counter; the clean-run */ \
    /* definition decides the class). */ \
    /** @brief ROCKETNET_COUNTER_INTEGRITY_LIMIT_REACHED: Counts connections closed after reaching the authenticated-open failure limit. Unit: occurrences. Class: refusal; family: none. */ \
    X( INTEGRITY_LIMIT_REACHED, REFUSAL, NONE ) \
    \
    /* client received a connection cookie packet with an invalid wire length */ \
    /* (not equal to ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES). distinct from */ \
    /* ROCKETNET_COUNTER_CLIENT_COOKIE_SALT_MISMATCH so ledger distinguishes */ \
    /* size errors from off-path salt injection. */ \
    /** @brief ROCKETNET_COUNTER_CLIENT_COOKIE_WRONG_SIZE: Counts client cookie packets with a wire size that does not match the packet type. Unit: occurrences. Class: refusal; family: CLIENT_COOKIE_RECEIVED. */ \
    X( CLIENT_COOKIE_WRONG_SIZE, REFUSAL, CLIENT_COOKIE_RECEIVED ) \
    \
    /* the message layer's region (docs/CHANNELS.md 4): a send refused because */ \
    /* the connection's send store had no chunk of the class it needs (or, for */ \
    /* a block, not every chunk it needs), and a send refused because every */ \
    /* send descriptor was in use. Exhaustion is a counted refusal, never */ \
    /* growth; nothing is taken from either when one refuses. Counted on the */ \
    /* owner's counters, once per refused send. appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL: Counts message sends refused because the connection send store lacks required chunks. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_SEND_STORE_FULL, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL: Counts message sends refused because every send descriptor is occupied. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_SEND_QUEUE_FULL, REFUSAL, NONE ) \
    \
    /* a cookie-valid connection request arrived after the update's handshake */ \
    /* budget was spent and was dropped before any asymmetric work, counted so */ \
    /* the drop is observable rather than silent. appended: indices are the */ \
    /* contract. */ \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED: Counts cookie-validated requests refused after the per-update handshake budget is spent. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    \
    /* a connection request whose cookie verified but whose source had already */ \
    /* spent the failed-signature-verify budget for this window: refused before */ \
    /* the Ed25519 verify ran. distinct from SIGNATURE_DID_NOT_VERIFY, which */ \
    /* counts verifies that actually ran and failed. appended: indices are the */ \
    /* contract. */ \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT: Counts cookie-validated requests refused before signature verification because the source exceeded its failure budget. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    \
    /* the UNRELIABLE path (docs/CHANNELS.md 5, 6.3, 6.4, 9.3, 11.2): a code-0 */ \
    /* record dropped on receive because the per-update unreliable delivery */ \
    /* budget Du was full (section 6.3 step 4), and a code-0 message dropped on */ \
    /* send: because finish_packet refused the packet that carried it (section */ \
    /* 6.4, Failed send), or because it is longer than the record's u16 length */ \
    /* and can never be carried. Unreliable semantics allow all three; each is */ \
    /* counted once per dropped message. appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL: Counts unreliable messages dropped because the per-update receive delivery budget is full. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_UNRELIABLE_DELIVERY_FULL, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED: Counts unreliable messages dropped because their carrier packet is refused or the record is too long. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_UNRELIABLE_SEND_FAILED, REFUSAL, NONE ) \
    \
    /* Layer 3 receive window: a reliable number already received (6.5). */ \
    /* PATH_LOSS: a reliable number received twice, the resend of a lost ack */ \
    /* or packet. */ \
    /** @brief ROCKETNET_COUNTER_MESSAGE_DUPLICATE: Counts reliable message numbers received more than once (a k-chunk duplicate counts k). Unit: occurrences. Class: path_loss; family: none. */ \
    X( MESSAGE_DUPLICATE, PATH_LOSS, NONE ) \
    /* Numbering met either the count or ring bound (CHANNELS 6.2). Count */ \
    /* once per stall, even when the scheduler retries the same head. */ \
    /** @brief ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL: Counts reliable send numbering stalls because the sender's own send window is full (its message count bound or its ring byte bound); counted once per stall, not per retry. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_WINDOW_STALL, REFUSAL, NONE ) \
    \
    /* Layer 3 sent-packet map and resend FIFO (docs/CHANNELS.md 6.4, 15.1): */ \
    /* a sent-packet ring entry evicted to make room for a new packet (its */ \
    /* referenced numbers stay in the resend FIFO), and a reliable record */ \
    /* resent on the resend delay. appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED: Counts sent-packet map entries evicted while their message numbers remain queued for resend. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_SENT_MAP_EVICTED, REFUSAL, NONE ) \
    /* PATH_LOSS: a reliable record resent on the resend delay. */ \
    /** @brief ROCKETNET_COUNTER_MESSAGE_RESENT: Counts reliable records resent after their resend delay. Unit: occurrences. Class: path_loss; family: none. */ \
    X( MESSAGE_RESENT, PATH_LOSS, NONE ) \
    \
    /* a connection's message memory (its region, docs/CHANNELS.md 4) was */ \
    /* exhausted: an operation found no room, the region took its sticky */ \
    /* exhausted state, and the connection is disconnected with the reason */ \
    /* ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED (tla/RNConnectionMemory.tla, */ \
    /* Exhaust). Counted once at the transition to exhausted and again on */ \
    /* each send refused while the region stays exhausted, on the owner's */ \
    /* counters, beside the refusal's own counter (MESSAGE_SEND_STORE_FULL or */ \
    /* MESSAGE_SEND_QUEUE_FULL). appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED: Counts the exhaustion of a connection's message memory region once, plus each send refused while the region stays exhausted; an exhausted connection is then disconnected. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_MEMORY_EXHAUSTED, REFUSAL, NONE ) \
    \
    /* Layer 4's dispatch (docs/CHANNELS.md 7.2, 7.3, 7.4, 7.5): a send refused */ \
    /* because its reliability is outside the enabled mask, a receive of an */ \
    /* application record whose code the mask does not admit, an unreliable */ \
    /* record dropped because the send budget could not pay the packet that */ \
    /* would carry it, and the counters the sequenced and receipt types use. */ \
    /* An ordered */ \
    /* record behind its channel's read index is MESSAGE_ORDERING_BEHIND (7.2: */ \
    /* layer 3 already caught the duplicates, so it is a violation). */ \
    /* appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED: Counts message sends refused because the selected reliability is disabled. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_SEND_TYPE_NOT_ENABLED, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED: Counts incoming messages refused because their reliability is disabled. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_TYPE_NOT_ENABLED, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET: Counts unreliable messages dropped because the packet budget cannot carry them. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_UNRELIABLE_OVER_BUDGET, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_ORDERING_BEHIND: Counts ordered records refused because their number is behind the channel read index. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_ORDERING_BEHIND, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE: Counts sequenced messages ignored because their ordering is behind or their sequence is behind the watermark. Unit: occurrences. Class: path_loss; family: none. */ \
    X( MESSAGE_SEQUENCED_STALE, PATH_LOSS, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED: Counts held sequenced messages replaced by a newer sequence. Unit: occurrences. Class: path_loss; family: none. */ \
    X( MESSAGE_SEQUENCED_SUPERSEDED, PATH_LOSS, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL: Counts sequenced messages refused because the hold slot is occupied. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_SEQUENCED_HOLD_FULL, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_RECEIPT_RING_FULL: Counts receipts dropped because the receipt ring is full (the ring drops the oldest). Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_RECEIPT_RING_FULL, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED: Counts message receipts acknowledged by the receiver. Unit: occurrences. Class: expected; family: none. */ \
    X( MESSAGE_RECEIPT_ACKED, EXPECTED, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS: Counts message receipts whose packet was lost on the path. Unit: occurrences. Class: path_loss; family: none. */ \
    X( MESSAGE_RECEIPT_LOSS, PATH_LOSS, NONE ) \
    \
    /* The connection response ladder's own internal error: the denied */ \
    /* packet or the first keep-alive could not be written after the */ \
    /* response was received. It was counted as */ \
    /* CONNECTION_REQUEST_INTERNAL_ERROR, which put one outcome in two */ \
    /* families (the request ladder and the response ladder); it is its */ \
    /* own counter now. appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_CONNECTION_RESPONSE_INTERNAL_ERROR: Counts connection responses whose follow-up packet could not be written. Unit: occurrences. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_INTERNAL_ERROR, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    \
    /* History was overwritten before its ACK, and the receiver had acknowledged */ \
    /* a sequence at least ROCKETNET_LOSS_PACKET_THRESHOLD above it: the */ \
    /* packet was lost on the path. An outcome, not a refusal. Together with */ \
    /* SEND_HISTORY_OVERFLOW this is the count of overwritten unacknowledged */ \
    /* send records (tla/RNSentHistory.tla). Advisory: the packet is still */ \
    /* transmitted. PATH_LOSS: it moves only when the path loses packets. */ \
    /* appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_SEND_HISTORY_LOST: Counts send records confirmed lost by the acknowledgement window. Unit: occurrences. Class: path_loss; family: none. */ \
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
    /* connection. REFUSAL: a clean sender never sends one. appended: indices */ \
    /* are the contract. */ \
    /** @brief ROCKETNET_COUNTER_MESSAGE_BAD_CHANNEL: Counts records refused because their channel index is outside the configured range. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_BAD_CHANNEL, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK: Counts records refused because block fragment metadata is invalid. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_BAD_BLOCK, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_BAD_RING: Counts records refused because their span is outside the receive ring. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_BAD_RING, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_ORDERING_AHEAD: Counts ordered records refused because their number is beyond the receive window. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_ORDERING_AHEAD, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION: Counts connections ended after a message protocol violation. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_PROTOCOL_VIOLATION, REFUSAL, NONE ) \
    \
    /* Message-object refusal, stale batch entry, and unreleased receive */ \
    /* view. Appended so the enum, names, classes, and families agree. */ \
    /** @brief ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED: Counts message sends refused by the message layer. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_SEND_REFUSED, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY: Counts batch entries ignored because the connection in that slot has changed. Unit: occurrences. Class: expected; family: none. */ \
    X( MESSAGE_STALE_CONNECTION_ENTRY, EXPECTED, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED: Counts the number of held messages each update while a received message view remains held; the update continues and returns ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED. Unit: messages. Class: refusal; family: none. */ \
    X( MESSAGE_NOT_RELEASED, REFUSAL, NONE ) \
    \
    /* The kernel prefilter counters (sdk/rocketnet_wire.h */ \
    /* ROCKETNET_SERVER_XDP_COUNTER_*), surfaced through the transport */ \
    /* counter surface by rocketnet_counters_apply_prefilter */ \
    /* (sdk/rocketnet_counters.cpp). appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_XDP_KERNEL_REDIRECTED: Counts packets redirected by the kernel prefilter. Unit: occurrences. Class: expected; family: none. */ \
    X( XDP_KERNEL_REDIRECTED, EXPECTED, NONE ) \
    /** @brief ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_SMALL: Counts packets dropped by the kernel prefilter for being too small. Unit: occurrences. Class: refusal; family: none. */ \
    X( XDP_KERNEL_DROPPED_TOO_SMALL, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_LARGE: Counts packets dropped by the kernel prefilter for being too large. Unit: occurrences. Class: refusal; family: none. */ \
    X( XDP_KERNEL_DROPPED_TOO_LARGE, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_WRONG_FILTER: Counts packets dropped by the kernel prefilter for failing its filter. Unit: occurrences. Class: refusal; family: none. */ \
    X( XDP_KERNEL_DROPPED_WRONG_FILTER, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_INVALID_TYPE: Counts packets dropped by the kernel prefilter for an invalid packet type. Unit: occurrences. Class: refusal; family: none. */ \
    X( XDP_KERNEL_DROPPED_INVALID_TYPE, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_WRONG_EPOCH: Counts packets dropped by the kernel prefilter for an unexpected epoch. Unit: occurrences. Class: refusal; family: none. */ \
    X( XDP_KERNEL_DROPPED_WRONG_EPOCH, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_XDP_KERNEL_PASSED_IPV6_EXTENSION: Counts packets passed by the kernel prefilter because they contain an IPv6 extension header. Unit: occurrences. Class: expected; family: none. */ \
    X( XDP_KERNEL_PASSED_IPV6_EXTENSION, EXPECTED, NONE ) \
    \
    /* A client hand-off deferred because the other send ring is still */ \
    /* handed off or sending (docs/CLIENT-IO.md, Reuse only after sent; a */ \
    /* hand-off never waits; tla/RNClientIo.tla Defer): the committed slots */ \
    /* stay in the Filling ring and leave with the next hand-off. EXPECTED: */ \
    /* deferral is the common case, not an overload signal. appended: */ \
    /* indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED: Counts client packet handoffs deferred while the other send ring is busy. Unit: occurrences. Class: expected; family: none. */ \
    X( CLIENT_SEND_DEFERRED, EXPECTED, NONE ) \
    \
    /* The kernel prefilter counter ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REPLAYED */ \
    /* (sdk/rocketnet_wire.h): a session packet behind the replay map's */ \
    /* high-water window, surfaced by rocketnet_counters_apply_prefilter. */ \
    /* appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_REPLAYED: Counts session packets dropped by the kernel replay window as older than its high-water mark. Unit: occurrences. Class: refusal; family: none. */ \
    X( XDP_KERNEL_DROPPED_REPLAYED, REFUSAL, NONE ) \
    \
    /* Client received a connection challenge or connection denied packet */ \
    /* whose wire length was not ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES */ \
    /* or ROCKETNET_CONNECTION_DENIED_WIRE_BYTES: refused before the state */ \
    /* gate and before any AEAD open (STANDARD.md, Reading packets). */ \
    /* appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_CLIENT_CHALLENGE_WRONG_SIZE: Counts challenge packets refused for the wrong wire length. Unit: occurrences. Class: refusal; family: CLIENT_CHALLENGE_RECEIVED. */ \
    X( CLIENT_CHALLENGE_WRONG_SIZE, REFUSAL, CLIENT_CHALLENGE_RECEIVED ) \
    /** @brief ROCKETNET_COUNTER_CLIENT_DENIED_WRONG_SIZE: Counts denial packets refused for the wrong wire length. Unit: occurrences. Class: refusal; family: CLIENT_DENIED_RECEIVED. */ \
    X( CLIENT_DENIED_WRONG_SIZE, REFUSAL, CLIENT_DENIED_RECEIVED ) \
    \
    /* The two token-time refusals that were merged into */ \
    /* CONNECTION_REQUEST_TOKEN_EXPIRED, split out so each refusal reason has */ \
    /* its own counter (STANDARD.md, Counters: refusal reasons are never */ \
    /* merged; Processing Connection Requests, the request order). They sit */ \
    /* beside TOKEN_EXPIRED in class and parent, and are appended here because */ \
    /* the indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE: Counts requests refused because the token creation time is in the future. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /* retired with the maximum token lifetime, which was never configurable */ \
    /* and always zero (no limit): keeps its index, always zero. */ \
    /** @brief ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_LIFETIME_TOO_LONG: Counts retired counter slot; it stays zero now that no maximum token lifetime exists. Unit: occurrences. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_TOKEN_LIFETIME_TOO_LONG, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    \
    /* A send the transport refused for want of a link-layer destination: the */ \
    /* xdp transport on a segment with no default route, to an address with no */ \
    /* learned MAC that answers no received frame (TRANSPORTS.md, xdp). Never */ \
    /* sent and never counted as sent. appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS: Counts sends refused because the transport has no link-layer destination. Unit: occurrences. Class: refusal; family: none. */ \
    X( SEND_NO_LINK_ADDRESS, REFUSAL, NONE ) \
    \
    /* An UNRELIABLE, UNRELIABLE_SEQUENCED or UNRELIABLE_WITH_ACK_RECEIPT send */ \
    /* too big for one packet's record is promoted to its reliable twin, as */ \
    /* the promotion rule says. EXPECTED: promotion is the normal path */ \
    /* for a large unreliable send, one count per send. appended: indices are */ \
    /* the contract. */ \
    /** @brief ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE: Counts UNRELIABLE sends too big for one packet's record, promoted to their reliable twin; one count per promoted send. Unit: occurrences. Class: expected; family: none. */ \
    X( MESSAGE_PROMOTED_UNRELIABLE, EXPECTED, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED: Counts UNRELIABLE_SEQUENCED sends too big for one packet's record, promoted to their reliable twin; one count per promoted send. Unit: occurrences. Class: expected; family: none. */ \
    X( MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED, EXPECTED, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_WITH_ACK_RECEIPT: Counts UNRELIABLE_WITH_ACK_RECEIPT sends too big for one packet's record, promoted to their reliable twin; one count per promoted send. Unit: occurrences. Class: expected; family: none. */ \
    X( MESSAGE_PROMOTED_UNRELIABLE_WITH_ACK_RECEIPT, EXPECTED, NONE ) \
    \
    /* A fitting unreliable message (docs/OUTGOING-BUILD.md) that was not */ \
    /* written in place in a packet buffer and took the queued path, with */ \
    /* its one copy into the frame at send_packets: the transport gave no */ \
    /* frame (a full send ring), unreliable messages already waited in the */ \
    /* connection's queue, the send budget could not pay it at start, or */ \
    /* the update's packets_per_update was spent (in-place and queued */ \
    /* packets count against it together), one packet of it being kept for */ \
    /* the write pass while reliable work is due. The queued path then */ \
    /* sends it, or drops it over budget and counts */ \
    /* MESSAGE_UNRELIABLE_OVER_BUDGET. EXPECTED: a full send ring and a */ \
    /* spent budget are normal. A send before CONNECTED never reaches it: */ \
    /* it is dropped and counts MESSAGE_SEND_NOT_CONNECTED. Counted on the */ \
    /* owner's counters, once per message. appended: indices are the */ \
    /* contract. */ \
    /** @brief ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK: Counts fitting unreliable messages that took the queued path instead of being written in place in a packet buffer; one count per message, on the owner's counters. Unit: messages. Class: expected; family: none. */ \
    X( MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK, EXPECTED, NONE ) \
    \
    /** @brief ROCKETNET_COUNTER_SEND_FAILED: Counts datagrams the transport's socket dropped because the send call failed (ENOBUFS from a full interface queue, EAGAIN from a full send buffer, a refused destination); the socket counts each one (rocketnet_platform_socket_send in sdk/rocketnet_platform.cpp) and the transport reads the transport's send_failures into this counter when the counters are read (sdk/rocketnet_client_transport.cpp, sdk/rocketnet_server_transport.cpp); a transport without send_failures leaves it zero. Appended: indices are the contract. Unit: datagrams. Class: refusal; family: none. */ \
    X( SEND_FAILED, REFUSAL, NONE ) \
    \
    /* The kernel prefilter's PASSED_FRAGMENT (sdk/rocketnet_wire.h): IP */ \
    /* fragments the XDP program handed to the kernel stack for */ \
    /* reassembly, counted before any size, epoch or filter check. */ \
    /* EXPECTED: a fragment is not a refusal. appended: indices are the */ \
    /* contract. */ \
    /** @brief ROCKETNET_COUNTER_XDP_KERNEL_PASSED_FRAGMENT: Counts IP fragments the kernel prefilter passed to the kernel stack. Unit: occurrences. Class: expected; family: none. */ \
    X( XDP_KERNEL_PASSED_FRAGMENT, EXPECTED, NONE ) \
    \
    /* The kernel prefilter's DROPPED_FIRST_FRAGMENT (sdk/rocketnet_wire.h): */ \
    /* the first IP fragment (the one carrying the UDP header) of a */ \
    /* datagram to the server's address and port, dropped in XDP before */ \
    /* any size, epoch or filter check. RocketNet assumes a 1500-byte MTU */ \
    /* path, so a fragment for the server is hostile or misconfigured; */ \
    /* without its first fragment the kernel never reassembles the */ \
    /* datagram. REFUSAL. appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_FIRST_FRAGMENT: Counts first IP fragments of datagrams to the server address and port that the kernel prefilter dropped. Unit: occurrences. Class: refusal; family: none. */ \
    X( XDP_KERNEL_DROPPED_FIRST_FRAGMENT, REFUSAL, NONE ) \
    \
    /* An application send on a connection that is not CONNECTED: a client */ \
    /* whose transport has not reached CONNECTED or has left it, a server */ \
    /* slot with no CONNECTED client (standard/MESSAGES.md m1.5; */ \
    /* tla/ClientSession.tla DropSend, tla/ServerSession.tla */ \
    /* ServerDropSend). The start returns NULL and nothing is held for a */ \
    /* later send. EXPECTED: a game sends every frame, so its sends from */ \
    /* create to CONNECTED take it. Counted on the owner's counters, once */ \
    /* per dropped start. appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED: Counts application message starts dropped because the connection was not CONNECTED; one count per dropped start, on the owner's counters. Unit: messages. Class: expected; family: none. */ \
    X( MESSAGE_SEND_NOT_CONNECTED, EXPECTED, NONE ) \
    \
    /* The remaining wire-check violations (STANDARD.md _Messages_, the check */ \
    /* order; sdk/rocketnet_message.h ROCKETNET_MESSAGE_VIOLATION_*), each with */ \
    /* its own counter: a malformed payload (check 1), a record truncated below */ \
    /* its header (check 2), flag bits a correct sender never writes (check 3), */ \
    /* a length outside the record (check 4), a reliable or CONTROL record on a */ \
    /* fragmented payload (check 9), a CONTROL record with an invalid body */ \
    /* (check 12), the layer-3 window refusals NUMBER_OUTSIDE_WINDOW and */ \
    /* SLOT_COLLISION (check 10), and a second ordered record with one channel's */ \
    /* ordering index (DUPLICATE_ORDERING). Every violation also moves */ \
    /* MESSAGE_PROTOCOL_VIOLATION once for the connection it ends. REFUSAL: a */ \
    /* clean sender never sends any. appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_MESSAGE_BAD_FORM: Counts records refused because their payload has no valid form byte. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_BAD_FORM, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_TRUNCATED: Counts records refused because a record is shorter than its header. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_TRUNCATED, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_BAD_FLAGS: Counts records refused because their flag bits are invalid. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_BAD_FLAGS, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_BAD_LENGTH: Counts records refused because their length lies outside the record. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_BAD_LENGTH, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_BAD_SIZE: Counts records refused because a reliable or CONTROL record rides a fragmented payload, or a record or SINGLE message is longer than max_message_bytes. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_BAD_SIZE, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_BAD_CONTROL: Counts CONTROL records refused because their body is invalid. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_BAD_CONTROL, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_NUMBER_OUTSIDE_WINDOW: Counts reliable records refused because their number is outside the receive window. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_NUMBER_OUTSIDE_WINDOW, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION: Counts reliable records refused because their receive descriptor slot is occupied by another number. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_SLOT_COLLISION, REFUSAL, NONE ) \
    /** @brief ROCKETNET_COUNTER_MESSAGE_DUPLICATE_ORDERING: Counts reliable ordered records refused because another record with the same channel and ordering index is already held. Unit: occurrences. Class: refusal; family: none. */ \
    X( MESSAGE_DUPLICATE_ORDERING, REFUSAL, NONE ) \
    \
    /* The kernel prefilter's DROPPED_NO_SOCKET (sdk/rocketnet_wire.h): a */ \
    /* packet that passed every prefilter check but whose receive queue */ \
    /* has no AF_XDP socket in the XSKMAP, so bpf_redirect_map refused it */ \
    /* and the XDP program dropped it. REFUSAL. appended: indices are the */ \
    /* contract. */ \
    /** @brief ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_NO_SOCKET: Counts packets that passed the kernel prefilter but were dropped because their receive queue had no AF_XDP socket in the redirect map. Unit: occurrences. Class: refusal; family: none. */ \
    X( XDP_KERNEL_DROPPED_NO_SOCKET, REFUSAL, NONE ) \
    \
    /* A datagram from a source other than the server the client dialed: */ \
    /* the client listens to that server alone and drops the datagram */ \
    /* before any protocol check, counted while the attempt holds the */ \
    /* server's address; after a terminal state clears it nothing is */ \
    /* counted (sdk/rocketnet_client_transport.cpp:1296-1301). REFUSAL. */ \
    /* appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE: Counts datagrams the client drops because their source is not the server it dialed, while the connection attempt holds that server's address; one count per datagram. Unit: datagrams. Class: refusal; family: none. */ \
    X( CLIENT_RECEIVE_FOREIGN_SOURCE, REFUSAL, NONE ) \
    \
    /* The kernel prefilter's DROPPED_IP_OPTIONS_OR_EXTENSION */ \
    /* (sdk/rocketnet_wire.h): a datagram to the server's address and port */ \
    /* behind IPv4 options or IPv6 extension headers, dropped in XDP: */ \
    /* RocketNet never sends either, and in XDP mode no kernel socket */ \
    /* listens on the port. REFUSAL. appended: indices are the contract. */ \
    /** @brief ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_IP_OPTIONS_OR_EXTENSION: Counts datagrams to the server address and port that the kernel prefilter dropped because IPv4 options or IPv6 extension headers sat in front of UDP. Unit: occurrences. Class: refusal; family: none. */ \
    X( XDP_KERNEL_DROPPED_IP_OPTIONS_OR_EXTENSION, REFUSAL, NONE ) \
    \
    /* Messages delivered in an update and never handed to the */ \
    /* application before the next update: API misuse, which ends the */ \
    /* connection with ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED */ \
    /* (standard/MESSAGES.md m1.6). REFUSAL. appended: indices are the */ \
    /* contract. */ \
    /** @brief ROCKETNET_COUNTER_MESSAGE_NOT_RECEIVED: Counts messages delivered in an update that the application never took with receive before the next update; the connection then ends with MESSAGE_NOT_RECEIVED. Unit: messages. Class: refusal; family: none. */ \
    X( MESSAGE_NOT_RECEIVED, REFUSAL, NONE )

/**
 * @brief Integer indices for the cumulative protocol counters.
 *
 * Values are generated in `ROCKETNET_COUNTER_ROWS` order and remain stable;
 * use `rocketnet_num_counters` to obtain the number of entries across FFI.
 */
enum
{
#define ROCKETNET_COUNTER_ENUM_ROW( name, klass, family ) ROCKETNET_COUNTER_##name,
    /**
     * @brief The index of one cumulative protocol counter, ROCKETNET_COUNTER_ followed by its row name.
     *
     * One value per ROCKETNET_COUNTER_ROWS row, in row order. The row's own
     * comment in ROCKETNET_COUNTER_ROWS gives its meaning, unit, class and
     * family; rocketnet_counter_name returns the row name and
     * rocketnet_counter_class its class.
     */
    ROCKETNET_COUNTER_ROWS( ROCKETNET_COUNTER_ENUM_ROW )
#undef ROCKETNET_COUNTER_ENUM_ROW

    /** @brief The number of cumulative protocol counters: one past the last counter index. */
    ROCKETNET_NUM_COUNTERS
};

// the FAMILY column of a counter that is outside every family
/** @brief Family marker for a counter that is not an outcome of a received packet. */
#define ROCKETNET_COUNTER_NONE                          (-1)

/**
 * @brief Storage for cumulative protocol counters and server handshake budget state.
 *
 * A transport or message object owns its counter block. Read published values
 * after an update returns, on the object's owning thread; do not read them
 * concurrently with updates or fill operations.
 */
struct rocketnet_counters_t
{
    /** @brief Monotonic totals indexed by the `ROCKETNET_COUNTER_*` values. */
    uint64_t values[ROCKETNET_NUM_COUNTERS];

    // Per-server, per-update handshake budget state, not a published counter:
    // how many post-cookie connection requests this server has processed in
    // the current update. Reset by the protocol sweep once per update, so the
    // cap is enforced per server rather than process-wide; the drop it causes
    // is the published counter above.
    /** @brief Internal per-server count of cookie-validated requests processed in this update. */
    uint64_t handshakes_processed_this_update;
};

// Surface a transport's kernel prefilter counters through the transport counter
// surface: prefilter holds ROCKETNET_SERVER_XDP_NUM_COUNTERS values in
// ROCKETNET_SERVER_XDP_COUNTER_* order. Each value is assigned (the kernel
// totals are cumulative) to its transport counter; nothing else moves.
/**
 * @brief Copy cumulative kernel prefilter totals into their matching counters.
 *
 * `prefilter` contains `ROCKETNET_SERVER_XDP_NUM_COUNTERS` values in
 * `ROCKETNET_SERVER_XDP_COUNTER_*` order. Each source total replaces its
 * mapped counter value; all other counter values are unchanged. The caller
 * owns the counter block for this write.
 * @param counters Counter block to update; NULL is a counted no-op.
 * @param prefilter Array of at least `ROCKETNET_SERVER_XDP_NUM_COUNTERS` totals; NULL is a counted no-op.
 */
void rocketnet_counters_apply_prefilter( struct rocketnet_counters_t * counters, const uint64_t * prefilter );

#include "rocketnet_internal.h"                  // rocketnet_counter_name lives in the forest header

#endif // #ifndef ROCKETNET_COUNTERS_H
