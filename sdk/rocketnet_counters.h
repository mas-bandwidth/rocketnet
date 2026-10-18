/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_COUNTERS_H
#define ROCKETNET_COUNTERS_H

#include <stdint.h>

/**
 * @brief Maximum cookie-validated connection requests processed per server update.
 *
 * Requests beyond this limit are refused before asymmetric signature
 * verification and increment `ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED`.
 */
#define ROCKETNET_HANDSHAKE_WORK_BUDGET_PER_UPDATE      1024

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
    /** @brief Counts wire datagrams received before packet-type dispatch. Class: arrival; family: none. */ \
    X( PACKETS_RECEIVED, ARRIVAL, NONE ) \
    /** @brief Counts packets refused because they are shorter than the prefilter header. Class: refusal; family: none. */ \
    X( PREFILTER_TOO_SHORT, REFUSAL, NONE ) \
    /** @brief Counts packets refused because they exceed the prefilter size limit. Class: refusal; family: none. */ \
    X( PREFILTER_TOO_LARGE, REFUSAL, NONE ) \
    /** @brief Counts packets refused because their epoch does not match. Class: refusal; family: none. */ \
    X( PREFILTER_WRONG_EPOCH, REFUSAL, NONE ) \
    /** @brief Counts packets refused because a prefilter field has an invalid value. Class: refusal; family: none. */ \
    X( PREFILTER_WRONG_VALUE, REFUSAL, NONE ) \
    /** @brief Counts packets refused because their packet type is reserved. Class: refusal; family: none. */ \
    X( RESERVED_TYPE, REFUSAL, NONE ) \
    /** @brief Counts packets refused because their type is not valid in this direction. Class: refusal; family: none. */ \
    X( WRONG_DIRECTION, REFUSAL, NONE ) \
    \
    /* connection request processing, one per check in the standard's order */ \
    /** @brief Counts incoming connection request packets counted before their outcome is classified. Class: arrival; family: none. */ \
    X( CONNECTION_REQUEST_RECEIVED, ARRIVAL, NONE ) \
    /** @brief Counts connection request packets with a wire size that does not match the packet type. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_WRONG_SIZE, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /* EXPECTED: a request duplicate that a round trip above 100 ms leaves, */ \
    /* arriving after the first has connected. */ \
    /** @brief Counts duplicate connection packets received after the connection is established. Class: expected; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_ALREADY_CONNECTED, EXPECTED, CONNECTION_REQUEST_RECEIVED ) \
    /* EXPECTED: the same duplicate arriving while the handshake is in */ \
    /* flight; the stored challenge answers it. */ \
    /** @brief Counts duplicate connection requests answered with the stored challenge. Class: expected; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_CHALLENGE_RESENT, EXPECTED, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests with an unsupported protocol version. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_WRONG_VERSION, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests with the wrong protocol identifier. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_WRONG_PROTOCOL_ID, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests carrying an expired token. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_TOKEN_EXPIRED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests carrying an invalid timeout. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_TIMEOUT_INVALID, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests with invalid address flags. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_BAD_ADDRESS_FLAGS, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests for an address not advertised by the token. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests that do not authenticate to the configured server key. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_WRONG_SERVER_KEY, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /* EXPECTED: the first leg of the cookie exchange, on every connection. */ \
    /** @brief Counts first-leg connection requests that ask the server to issue a cookie. Class: expected; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_NO_COOKIE, EXPECTED, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests with a cookie that fails verification. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests with an expired cookie. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_COOKIE_EXPIRED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests that reuse a token. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_TOKEN_ALREADY_USED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests whose signature verification fails. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests whose sealed block cannot be opened. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests whose opened sealed data does not match the token. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_SEALED_MISMATCH, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests refused because that client identifier is already connected. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_CLIENT_ID_ALREADY_CONNECTED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /* retired with the admitted-token set (docs/SESSION.md 1.6): keeps its */ \
    /* index, always zero. */ \
    /** @brief Counts retired counter slot; it stays zero while the connected-client check handles this refusal. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection request packets refused because their public key has small order. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_SMALL_ORDER_KEY, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests refused because no client slot is available. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_NO_SLOTS, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests refused because the mapping could not be added. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    /* EXPECTED: the success outcome. */ \
    /** @brief Counts connection requests accepted with a challenge sent to the client. Class: expected; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_SENT_CHALLENGE, EXPECTED, CONNECTION_REQUEST_RECEIVED ) \
    /** @brief Counts connection requests whose response could not be written. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_INTERNAL_ERROR, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    \
    /* connection response processing, in the standard's order */ \
    /** @brief Counts incoming connection response packets counted before their outcome is classified. Class: arrival; family: none. */ \
    X( CONNECTION_RESPONSE_RECEIVED, ARRIVAL, NONE ) \
    /** @brief Counts connection response packets with a wire size that does not match the packet type. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_WRONG_SIZE, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /** @brief Counts late packets received after the source mapping has been removed. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_NO_MAPPING, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /** @brief Counts connection response packets with a zero session sequence. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_ZERO_SEQUENCE, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /** @brief Counts connection response packets whose authenticated contents fail to open. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_DID_NOT_OPEN, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /* EXPECTED: a response duplicate arriving after the first has connected. */ \
    /** @brief Counts duplicate connection packets received after the connection is established. Class: expected; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_ALREADY_CONNECTED, EXPECTED, CONNECTION_RESPONSE_RECEIVED ) \
    /** @brief Counts connection responses refused because that client identifier is already connected. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /* retired with the admitted-token set (docs/SESSION.md 1.6): keeps its */ \
    /* index, always zero. */ \
    /** @brief Counts retired counter slot; it stays zero after the admitted-token set is removed. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_ADMITTED_SET_FULL, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /** @brief Counts connection responses refused because no client slot is available. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_NO_SLOTS, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    /** @brief Counts connection responses that complete the handshake. Class: expected; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_CONNECTED, EXPECTED, CONNECTION_RESPONSE_RECEIVED ) \
    \
    /* keep-alive, payload and disconnect at the server */ \
    /** @brief Counts incoming keep alive packets counted before their outcome is classified. Class: arrival; family: none. */ \
    X( KEEP_ALIVE_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race. The server ended the session (a */ \
    /* disconnect, a timeout) with the client's packets in flight; no network */ \
    /* fault is needed (test_unit_client_receive_counters.cpp, */ \
    /* session_end_races_move_only_expected_counters). */ \
    /** @brief Counts late packets received after the source mapping has been removed. Class: expected; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_NO_MAPPING, EXPECTED, KEEP_ALIVE_RECEIVED ) \
    /* REFUSAL from here to the end of the type: a refused packet reads 0 on */ \
    /* a clean path. */ \
    /** @brief Counts keep alive packets with a zero session sequence. Class: refusal; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_ZERO_SEQUENCE, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    /** @brief Counts keep alive packets whose sequence was already received. Class: refusal; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_ALREADY_RECEIVED, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    /** @brief Counts keep alive packets whose authenticated contents fail to open. Class: refusal; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_DID_NOT_OPEN, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    /** @brief Counts keep alive packets received before the session is connected. Class: refusal; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_NOT_CONNECTED, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    /** @brief Counts keep alive packets accepted by the protocol. Class: expected; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_ACCEPTED, EXPECTED, KEEP_ALIVE_RECEIVED ) \
    \
    /** @brief Counts incoming payload packets counted before their outcome is classified. Class: arrival; family: none. */ \
    X( PAYLOAD_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race, as KEEP_ALIVE_NO_MAPPING. */ \
    /** @brief Counts late packets received after the source mapping has been removed. Class: expected; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_NO_MAPPING, EXPECTED, PAYLOAD_RECEIVED ) \
    /** @brief Counts payload packets with a zero session sequence. Class: refusal; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_ZERO_SEQUENCE, REFUSAL, PAYLOAD_RECEIVED ) \
    /** @brief Counts payload packets whose sequence was already received. Class: refusal; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_ALREADY_RECEIVED, REFUSAL, PAYLOAD_RECEIVED ) \
    /** @brief Counts payload packets whose authenticated contents fail to open. Class: refusal; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_DID_NOT_OPEN, REFUSAL, PAYLOAD_RECEIVED ) \
    /** @brief Counts payload packets received before the session is connected. Class: refusal; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_NOT_CONNECTED, REFUSAL, PAYLOAD_RECEIVED ) \
    /** @brief Counts payloads delivered to the application. Class: expected; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_DELIVERED, EXPECTED, PAYLOAD_RECEIVED ) \
    \
    /** @brief Counts incoming disconnect packets counted before their outcome is classified. Class: arrival; family: none. */ \
    X( DISCONNECT_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the copies of the redundant disconnect burst after the first */ \
    /* has torn the mapping down. */ \
    /** @brief Counts late packets received after the source mapping has been removed. Class: expected; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_NO_MAPPING, EXPECTED, DISCONNECT_RECEIVED ) \
    /** @brief Counts disconnect packets with a zero session sequence. Class: refusal; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_ZERO_SEQUENCE, REFUSAL, DISCONNECT_RECEIVED ) \
    /** @brief Counts disconnect packets whose sequence was already received. Class: refusal; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_ALREADY_RECEIVED, REFUSAL, DISCONNECT_RECEIVED ) \
    /** @brief Counts disconnect packets whose authenticated contents fail to open. Class: refusal; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_DID_NOT_OPEN, REFUSAL, DISCONNECT_RECEIVED ) \
    /** @brief Counts disconnect packets received before the session is connected. Class: refusal; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_NOT_CONNECTED, REFUSAL, DISCONNECT_RECEIVED ) \
    /** @brief Counts disconnect packets that end the active connection. Class: expected; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_DISCONNECTED, EXPECTED, DISCONNECT_RECEIVED ) \
    \
    /* mapping and client lifecycle */ \
    /* EXPECTED, as are the lifecycle counters through CLIENT_TIMED_OUT: they */ \
    /* count connections coming and going, which a clean run does by design. */ \
    /** @brief Counts server connection mappings created. Class: expected; family: none. */ \
    X( MAPPING_CREATED, EXPECTED, NONE ) \
    /** @brief Counts mappings removed after the idle timeout. Class: expected; family: none. */ \
    X( MAPPING_IDLE_TIMEOUT, EXPECTED, NONE ) \
    /** @brief Counts mappings removed after the handshake deadline. Class: expected; family: none. */ \
    X( MAPPING_HANDSHAKE_DEADLINE_EXPIRED, EXPECTED, NONE ) \
    /** @brief Counts connection requests refused because the mapping table is full. Class: refusal; family: none. */ \
    X( MAPPING_TABLE_FULL, REFUSAL, NONE ) \
    /** @brief Counts client connections established. Class: expected; family: none. */ \
    X( CLIENT_CONNECTED, EXPECTED, NONE ) \
    /* REFUSAL: a resource exhaustion is a refusal. */ \
    /** @brief Counts packets refused because the send ring has no free slot. Class: refusal; family: none. */ \
    X( SEND_RING_FULL, REFUSAL, NONE ) \
    /* REFUSAL: an internal state the shipped protocol cannot produce. */ \
    /** @brief Counts internal events that find a client slot without a mapping. Class: refusal; family: none. */ \
    X( SLOT_WITHOUT_MAPPING, REFUSAL, NONE ) \
    /** @brief Counts client connections closed locally. Class: expected; family: none. */ \
    X( CLIENT_DISCONNECTED, EXPECTED, NONE ) \
    /** @brief Counts client connections closed by the server. Class: expected; family: none. */ \
    X( SERVER_DISCONNECTED_CLIENT, EXPECTED, NONE ) \
    /** @brief Counts client connections closed after timing out. Class: expected; family: none. */ \
    X( CLIENT_TIMED_OUT, EXPECTED, NONE ) \
    \
    /* the client's receive path. the shared packet counters above serve both */ \
    /* directions; these are the client-specific reasons. */ \
    /** @brief Counts incoming client cookie packets counted before their outcome is classified. Class: arrival; family: none. */ \
    X( CLIENT_COOKIE_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: a cookie duplicate arriving after the client moved on, the */ \
    /* same round-trip duplicate as ALREADY_CONNECTED. */ \
    /** @brief Counts client cookie packets received in a protocol state that does not accept them. Class: expected; family: CLIENT_COOKIE_RECEIVED. */ \
    X( CLIENT_COOKIE_WRONG_STATE, EXPECTED, CLIENT_COOKIE_RECEIVED ) \
    /** @brief Counts cookies refused because their salt does not match the connection attempt. Class: refusal; family: CLIENT_COOKIE_RECEIVED. */ \
    X( CLIENT_COOKIE_SALT_MISMATCH, REFUSAL, CLIENT_COOKIE_RECEIVED ) \
    /** @brief Counts cookies stored for the active connection attempt. Class: expected; family: CLIENT_COOKIE_RECEIVED. */ \
    X( CLIENT_COOKIE_STORED, EXPECTED, CLIENT_COOKIE_RECEIVED ) \
    /** @brief Counts incoming client challenge packets counted before their outcome is classified. Class: arrival; family: none. */ \
    X( CLIENT_CHALLENGE_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the server resends the challenge for a duplicate request; */ \
    /* the client no longer has a question for it. */ \
    /** @brief Counts client challenge packets received in a protocol state that does not accept them. Class: expected; family: CLIENT_CHALLENGE_RECEIVED. */ \
    X( CLIENT_CHALLENGE_WRONG_STATE, EXPECTED, CLIENT_CHALLENGE_RECEIVED ) \
    /** @brief Counts client challenge packets with a zero session sequence. Class: refusal; family: CLIENT_CHALLENGE_RECEIVED. */ \
    X( CLIENT_CHALLENGE_ZERO_SEQUENCE, REFUSAL, CLIENT_CHALLENGE_RECEIVED ) \
    /** @brief Counts client challenge packets whose authenticated contents fail to open. Class: refusal; family: CLIENT_CHALLENGE_RECEIVED. */ \
    X( CLIENT_CHALLENGE_DID_NOT_OPEN, REFUSAL, CLIENT_CHALLENGE_RECEIVED ) \
    /** @brief Counts client challenge packets refused because their public key has small order. Class: refusal; family: CLIENT_CHALLENGE_RECEIVED. */ \
    X( CLIENT_CHALLENGE_SMALL_ORDER_KEY, REFUSAL, CLIENT_CHALLENGE_RECEIVED ) \
    /** @brief Counts client challenge packets accepted by the protocol. Class: expected; family: CLIENT_CHALLENGE_RECEIVED. */ \
    X( CLIENT_CHALLENGE_ACCEPTED, EXPECTED, CLIENT_CHALLENGE_RECEIVED ) \
    /** @brief Counts client denied packets received from the server before their outcome is classified. Class: arrival; family: none. */ \
    X( CLIENT_DENIED_RECEIVED, ARRIVAL, NONE ) \
    /* REFUSAL: a denied reaches a client only when the server is out of */ \
    /* slots. */ \
    /** @brief Counts client denied packets received in a protocol state that does not accept them. Class: refusal; family: CLIENT_DENIED_RECEIVED. */ \
    X( CLIENT_DENIED_WRONG_STATE, REFUSAL, CLIENT_DENIED_RECEIVED ) \
    /** @brief Counts client denied packets with a zero session sequence. Class: refusal; family: CLIENT_DENIED_RECEIVED. */ \
    X( CLIENT_DENIED_ZERO_SEQUENCE, REFUSAL, CLIENT_DENIED_RECEIVED ) \
    /** @brief Counts client denied packets whose authenticated contents fail to open. Class: refusal; family: CLIENT_DENIED_RECEIVED. */ \
    X( CLIENT_DENIED_DID_NOT_OPEN, REFUSAL, CLIENT_DENIED_RECEIVED ) \
    /* REFUSAL: the server denied the connection. The server denies only for */ \
    /* NO_SLOTS, so a clean run reads 0. */ \
    /** @brief Counts client denied packets accepted by the protocol. Class: refusal; family: CLIENT_DENIED_RECEIVED. */ \
    X( CLIENT_DENIED_ACCEPTED, REFUSAL, CLIENT_DENIED_RECEIVED ) \
    /** @brief Counts incoming client keep alive packets counted before their outcome is classified. Class: arrival; family: none. */ \
    X( CLIENT_KEEP_ALIVE_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race, from the client's side. The client */ \
    /* left the session (a disconnect) with the server's packets in flight. A */ \
    /* loss-free soak_sim wire moves it; so do CLIENT_PAYLOAD_, CLIENT_ACK_, */ \
    /* CLIENT_FRAGMENT_ and CLIENT_DISCONNECT_WRONG_STATE (the disconnect */ \
    /* burst's copies). */ \
    /** @brief Counts client keep alive packets received in a protocol state that does not accept them. Class: expected; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_WRONG_STATE, EXPECTED, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief Counts client keep alive packets with a zero session sequence. Class: refusal; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_ZERO_SEQUENCE, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief Counts client keep alive packets whose sequence was already received. Class: refusal; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_ALREADY_RECEIVED, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief Counts client keep alive packets whose authenticated contents fail to open. Class: refusal; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_DID_NOT_OPEN, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief Counts client keep alive packets accepted by the protocol. Class: expected; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_ACCEPTED, EXPECTED, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief Counts incoming client payload packets counted before their outcome is classified. Class: arrival; family: none. */ \
    X( CLIENT_PAYLOAD_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race, as CLIENT_KEEP_ALIVE_WRONG_STATE. */ \
    /** @brief Counts client payload packets received in a protocol state that does not accept them. Class: expected; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_WRONG_STATE, EXPECTED, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief Counts client payload packets with a zero session sequence. Class: refusal; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_ZERO_SEQUENCE, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief Counts client payload packets whose sequence was already received. Class: refusal; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_ALREADY_RECEIVED, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief Counts client payload packets whose authenticated contents fail to open. Class: refusal; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_DID_NOT_OPEN, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief Counts payloads delivered to the application. Class: expected; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_DELIVERED, EXPECTED, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief Counts incoming client disconnect packets counted before their outcome is classified. Class: arrival; family: none. */ \
    X( CLIENT_DISCONNECT_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the copies of the server's redundant disconnect burst after the first. */ \
    /** @brief Counts client disconnect packets received in a protocol state that does not accept them. Class: expected; family: CLIENT_DISCONNECT_RECEIVED. */ \
    X( CLIENT_DISCONNECT_WRONG_STATE, EXPECTED, CLIENT_DISCONNECT_RECEIVED ) \
    /** @brief Counts client disconnect packets with a zero session sequence. Class: refusal; family: CLIENT_DISCONNECT_RECEIVED. */ \
    X( CLIENT_DISCONNECT_ZERO_SEQUENCE, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    /** @brief Counts client disconnect packets whose sequence was already received. Class: refusal; family: CLIENT_DISCONNECT_RECEIVED. */ \
    X( CLIENT_DISCONNECT_ALREADY_RECEIVED, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    /** @brief Counts client disconnect packets whose authenticated contents fail to open. Class: refusal; family: CLIENT_DISCONNECT_RECEIVED. */ \
    X( CLIENT_DISCONNECT_DID_NOT_OPEN, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    /** @brief Counts client disconnect packets accepted by the protocol. Class: expected; family: CLIENT_DISCONNECT_RECEIVED. */ \
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
    /** @brief Counts keep alive packets with a wire size that does not match the packet type. Class: refusal; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_WRONG_SIZE, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    /** @brief Counts keep alive packets with an invalid acknowledgement block. Class: refusal; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_BAD_ACK_BLOCK, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    /** @brief Counts payload packets with a wire size that does not match the packet type. Class: refusal; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_WRONG_SIZE, REFUSAL, PAYLOAD_RECEIVED ) \
    /** @brief Counts payload packets with an invalid acknowledgement block. Class: refusal; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_BAD_ACK_BLOCK, REFUSAL, PAYLOAD_RECEIVED ) \
    /** @brief Counts disconnect packets with a wire size that does not match the packet type. Class: refusal; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_WRONG_SIZE, REFUSAL, DISCONNECT_RECEIVED ) \
    /** @brief Counts disconnect packets with an invalid acknowledgement block. Class: refusal; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_BAD_ACK_BLOCK, REFUSAL, DISCONNECT_RECEIVED ) \
    \
    /** @brief Counts incoming ack packets counted before their outcome is classified. Class: arrival; family: none. */ \
    X( ACK_RECEIVED, ARRIVAL, NONE ) \
    /** @brief Counts ack packets with a wire size that does not match the packet type. Class: refusal; family: ACK_RECEIVED. */ \
    X( ACK_WRONG_SIZE, REFUSAL, ACK_RECEIVED ) \
    /* EXPECTED: the session-end race, as KEEP_ALIVE_NO_MAPPING. */ \
    /** @brief Counts late packets received after the source mapping has been removed. Class: expected; family: ACK_RECEIVED. */ \
    X( ACK_NO_MAPPING, EXPECTED, ACK_RECEIVED ) \
    /** @brief Counts ack packets with a zero session sequence. Class: refusal; family: ACK_RECEIVED. */ \
    X( ACK_ZERO_SEQUENCE, REFUSAL, ACK_RECEIVED ) \
    /** @brief Counts ack packets whose sequence was already received. Class: refusal; family: ACK_RECEIVED. */ \
    X( ACK_ALREADY_RECEIVED, REFUSAL, ACK_RECEIVED ) \
    /** @brief Counts ack packets whose authenticated contents fail to open. Class: refusal; family: ACK_RECEIVED. */ \
    X( ACK_DID_NOT_OPEN, REFUSAL, ACK_RECEIVED ) \
    /** @brief Counts ack packets received before the session is connected. Class: refusal; family: ACK_RECEIVED. */ \
    X( ACK_NOT_CONNECTED, REFUSAL, ACK_RECEIVED ) \
    /** @brief Counts ack packets with an invalid acknowledgement block. Class: refusal; family: ACK_RECEIVED. */ \
    X( ACK_BAD_ACK_BLOCK, REFUSAL, ACK_RECEIVED ) \
    /** @brief Counts ack packets accepted by the protocol. Class: expected; family: ACK_RECEIVED. */ \
    X( ACK_ACCEPTED, EXPECTED, ACK_RECEIVED ) \
    \
    /* EXPECTED: a side counter. The packet that fires it still succeeds. */ \
    /** @brief Counts acknowledgement entries that name a sequence no longer in the sent history. Class: expected; family: none. */ \
    X( ACK_UNKNOWN_SEQUENCE, EXPECTED, NONE ) \
    /* REFUSAL: a resource exhaustion (the delivery batch). The deferred id */ \
    /* is not lost. */ \
    /** @brief Counts acknowledged packet notifications deferred because the delivery batch is full. Class: refusal; family: none. */ \
    X( ACKED_BATCH_FULL, REFUSAL, NONE ) \
    \
    /** @brief Counts client keep alive packets with a wire size that does not match the packet type. Class: refusal; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_WRONG_SIZE, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief Counts client keep alive packets with an invalid acknowledgement block. Class: refusal; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief Counts client payload packets with a wire size that does not match the packet type. Class: refusal; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_WRONG_SIZE, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief Counts client payload packets with an invalid acknowledgement block. Class: refusal; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_BAD_ACK_BLOCK, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief Counts client disconnect packets with a wire size that does not match the packet type. Class: refusal; family: CLIENT_DISCONNECT_RECEIVED. */ \
    X( CLIENT_DISCONNECT_WRONG_SIZE, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    /** @brief Counts client disconnect packets with an invalid acknowledgement block. Class: refusal; family: CLIENT_DISCONNECT_RECEIVED. */ \
    X( CLIENT_DISCONNECT_BAD_ACK_BLOCK, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    \
    /** @brief Counts incoming client ack packets counted before their outcome is classified. Class: arrival; family: none. */ \
    X( CLIENT_ACK_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race, as CLIENT_KEEP_ALIVE_WRONG_STATE. */ \
    /** @brief Counts client ack packets received in a protocol state that does not accept them. Class: expected; family: CLIENT_ACK_RECEIVED. */ \
    X( CLIENT_ACK_WRONG_STATE, EXPECTED, CLIENT_ACK_RECEIVED ) \
    /** @brief Counts client ack packets with a wire size that does not match the packet type. Class: refusal; family: CLIENT_ACK_RECEIVED. */ \
    X( CLIENT_ACK_WRONG_SIZE, REFUSAL, CLIENT_ACK_RECEIVED ) \
    /** @brief Counts client ack packets with a zero session sequence. Class: refusal; family: CLIENT_ACK_RECEIVED. */ \
    X( CLIENT_ACK_ZERO_SEQUENCE, REFUSAL, CLIENT_ACK_RECEIVED ) \
    /** @brief Counts client ack packets whose sequence was already received. Class: refusal; family: CLIENT_ACK_RECEIVED. */ \
    X( CLIENT_ACK_ALREADY_RECEIVED, REFUSAL, CLIENT_ACK_RECEIVED ) \
    /** @brief Counts client ack packets whose authenticated contents fail to open. Class: refusal; family: CLIENT_ACK_RECEIVED. */ \
    X( CLIENT_ACK_DID_NOT_OPEN, REFUSAL, CLIENT_ACK_RECEIVED ) \
    /** @brief Counts client ack packets with an invalid acknowledgement block. Class: refusal; family: CLIENT_ACK_RECEIVED. */ \
    X( CLIENT_ACK_BAD_ACK_BLOCK, REFUSAL, CLIENT_ACK_RECEIVED ) \
    /** @brief Counts client ack packets accepted by the protocol. Class: expected; family: CLIENT_ACK_RECEIVED. */ \
    X( CLIENT_ACK_ACCEPTED, EXPECTED, CLIENT_ACK_RECEIVED ) \
    \
    /** @brief Counts client acknowledgement entries that name a sequence no longer in the sent history. Class: expected; family: none. */ \
    X( CLIENT_ACK_UNKNOWN_SEQUENCE, EXPECTED, NONE ) \
    /** @brief Counts client acknowledged packet notifications deferred because the delivery batch is full. Class: refusal; family: none. */ \
    X( CLIENT_ACKED_BATCH_FULL, REFUSAL, NONE ) \
    \
    /* WRONG_OPENED_SIZE: the opened data, less the ack block, did not match */ \
    /* the type -- its own reason, distinct from WRONG_SIZE (the wire floor) */ \
    /* and DID_NOT_OPEN (the seal refused), per the one-counter-per-reason */ \
    /* rule. Appended per the append-only index contract. */ \
    \
    /** @brief Counts keep alive packets whose authenticated payload has an invalid size. Class: refusal; family: KEEP_ALIVE_RECEIVED. */ \
    X( KEEP_ALIVE_WRONG_OPENED_SIZE, REFUSAL, KEEP_ALIVE_RECEIVED ) \
    /** @brief Counts payload packets whose authenticated payload has an invalid size. Class: refusal; family: PAYLOAD_RECEIVED. */ \
    X( PAYLOAD_WRONG_OPENED_SIZE, REFUSAL, PAYLOAD_RECEIVED ) \
    /** @brief Counts disconnect packets whose authenticated payload has an invalid size. Class: refusal; family: DISCONNECT_RECEIVED. */ \
    X( DISCONNECT_WRONG_OPENED_SIZE, REFUSAL, DISCONNECT_RECEIVED ) \
    /** @brief Counts ack packets whose authenticated payload has an invalid size. Class: refusal; family: ACK_RECEIVED. */ \
    X( ACK_WRONG_OPENED_SIZE, REFUSAL, ACK_RECEIVED ) \
    /** @brief Counts client keep alive packets whose authenticated payload has an invalid size. Class: refusal; family: CLIENT_KEEP_ALIVE_RECEIVED. */ \
    X( CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE, REFUSAL, CLIENT_KEEP_ALIVE_RECEIVED ) \
    /** @brief Counts client payload packets whose authenticated payload has an invalid size. Class: refusal; family: CLIENT_PAYLOAD_RECEIVED. */ \
    X( CLIENT_PAYLOAD_WRONG_OPENED_SIZE, REFUSAL, CLIENT_PAYLOAD_RECEIVED ) \
    /** @brief Counts client disconnect packets whose authenticated payload has an invalid size. Class: refusal; family: CLIENT_DISCONNECT_RECEIVED. */ \
    X( CLIENT_DISCONNECT_WRONG_OPENED_SIZE, REFUSAL, CLIENT_DISCONNECT_RECEIVED ) \
    /** @brief Counts client ack packets whose authenticated payload has an invalid size. Class: refusal; family: CLIENT_ACK_RECEIVED. */ \
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
    /** @brief Counts incoming fragment packets counted before their outcome is classified. Class: arrival; family: none. */ \
    X( FRAGMENT_RECEIVED, ARRIVAL, NONE ) \
    /** @brief Counts fragment packets with a wire size that does not match the packet type. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_WRONG_SIZE, REFUSAL, FRAGMENT_RECEIVED ) \
    /* EXPECTED: the session-end race, as KEEP_ALIVE_NO_MAPPING. */ \
    /** @brief Counts late packets received after the source mapping has been removed. Class: expected; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_NO_MAPPING, EXPECTED, FRAGMENT_RECEIVED ) \
    /** @brief Counts fragment packets with a zero session sequence. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_ZERO_SEQUENCE, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief Counts fragment packets whose sequence was already received. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_ALREADY_RECEIVED, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief Counts fragment packets whose authenticated contents fail to open. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_DID_NOT_OPEN, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief Counts fragment packets received before the session is connected. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_NOT_CONNECTED, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief Counts fragment packets with an invalid acknowledgement block. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_BAD_ACK_BLOCK, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief Counts fragment packets whose authenticated payload has an invalid size. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_WRONG_OPENED_SIZE, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief Counts fragments refused because their payload identifier is zero. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_ZERO_PAYLOAD_ID, REFUSAL, FRAGMENT_RECEIVED ) \
    /* PATH_LOSS: a fragment of a payload an older loss or reorder left */ \
    /* behind. */ \
    /** @brief Counts fragments ignored because their payload sequence is stale. Class: path_loss; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_STALE, PATH_LOSS, FRAGMENT_RECEIVED ) \
    /** @brief Counts fragments refused because their fragment count is invalid. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_COUNT_INVALID, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief Counts fragments refused because their index is outside the fragment count. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_INDEX_OUT_OF_RANGE, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief Counts fragments refused because their count differs from the payload first seen. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_COUNT_MISMATCH, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief Counts fragments refused because their size disagrees with the payload shape. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_SIZE_MISMATCH, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief Counts payloads refused because their size exceeds the configured maximum. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_PAYLOAD_TOO_LARGE, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief Counts fragments refused because their index has already been received. Class: refusal; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_DUPLICATE_INDEX, REFUSAL, FRAGMENT_RECEIVED ) \
    /** @brief Counts fragment packets accepted by the protocol. Class: expected; family: FRAGMENT_RECEIVED. */ \
    X( FRAGMENT_ACCEPTED, EXPECTED, FRAGMENT_RECEIVED ) \
    /** @brief Counts fragmented payloads completed by reassembly. Class: expected; family: none. */ \
    X( FRAGMENT_PAYLOAD_REASSEMBLED, EXPECTED, NONE ) \
    /* PATH_LOSS: a lost fragment loses its whole payload, by design. */ \
    /** @brief Counts fragmented payloads abandoned after a fragment is lost or reordered. Class: path_loss; family: none. */ \
    X( FRAGMENT_PAYLOAD_ABANDONED, PATH_LOSS, NONE ) \
    /** @brief Counts reassembled payloads deferred because the receive batch is full. Class: refusal; family: none. */ \
    X( FRAGMENT_PAYLOAD_BATCH_FULL, REFUSAL, NONE ) \
    \
    /** @brief Counts incoming client fragment packets counted before their outcome is classified. Class: arrival; family: none. */ \
    X( CLIENT_FRAGMENT_RECEIVED, ARRIVAL, NONE ) \
    /* EXPECTED: the session-end race, as CLIENT_KEEP_ALIVE_WRONG_STATE. */ \
    /** @brief Counts client fragment packets received in a protocol state that does not accept them. Class: expected; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_WRONG_STATE, EXPECTED, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client fragment packets with a wire size that does not match the packet type. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_WRONG_SIZE, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client fragment packets with a zero session sequence. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_ZERO_SEQUENCE, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client fragment packets whose sequence was already received. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_ALREADY_RECEIVED, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client fragment packets whose authenticated contents fail to open. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_DID_NOT_OPEN, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client fragment packets with an invalid acknowledgement block. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_BAD_ACK_BLOCK, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client fragment packets whose authenticated payload has an invalid size. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_WRONG_OPENED_SIZE, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client fragments refused because their payload identifier is zero. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_ZERO_PAYLOAD_ID, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client fragments ignored because their payload sequence is stale. Class: path_loss; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_STALE, PATH_LOSS, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client fragments refused because their fragment count is invalid. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_COUNT_INVALID, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client fragments refused because their index is outside the fragment count. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_INDEX_OUT_OF_RANGE, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client fragments refused because their count differs from the payload first seen. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_COUNT_MISMATCH, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client fragments refused because their size disagrees with the payload shape. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_SIZE_MISMATCH, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client payloads refused because their size exceeds the configured maximum. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_PAYLOAD_TOO_LARGE, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client fragments refused because their index has already been received. Class: refusal; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_DUPLICATE_INDEX, REFUSAL, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client fragment packets accepted by the protocol. Class: expected; family: CLIENT_FRAGMENT_RECEIVED. */ \
    X( CLIENT_FRAGMENT_ACCEPTED, EXPECTED, CLIENT_FRAGMENT_RECEIVED ) \
    /** @brief Counts client fragmented payloads completed by reassembly. Class: expected; family: none. */ \
    X( CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED, EXPECTED, NONE ) \
    /** @brief Counts client fragmented payloads abandoned after a fragment is lost or reordered. Class: path_loss; family: none. */ \
    X( CLIENT_FRAGMENT_PAYLOAD_ABANDONED, PATH_LOSS, NONE ) \
    /** @brief Counts client reassembled payloads deferred because the receive batch is full. Class: refusal; family: none. */ \
    X( CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL, REFUSAL, NONE ) \
    \
    /* the send side: a payload requested above the wire's capacity found */ \
    /* the fill phase's arena exhausted, and was dropped at start_packet -- */ \
    /* the arena sibling of SEND_RING_FULL */ \
    \
    /** @brief Counts packets refused because the fill-phase send arena is exhausted. Class: refusal; family: none. */ \
    X( SEND_ARENA_FULL, REFUSAL, NONE ) \
    \
    /* the copying receive cursor was handed a buffer smaller than a */ \
    /* delivered payload: the payload is refused whole, never truncated. */ \
    /* stays zero for every caller sizing by the configured maximum. */ \
    \
    /** @brief Counts payloads refused whole because the caller receive buffer is too small. Class: refusal; family: none. */ \
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
    /** @brief Counts unacknowledged send records overwritten before the peer acknowledges them. Class: refusal; family: none. */ \
    X( SEND_HISTORY_OVERFLOW, REFUSAL, NONE ) \
    /* Private packet metadata budget exhausted during a fill phase. */ \
    /** @brief Counts packet leases refused because the private metadata table is full. Class: refusal; family: none. */ \
    X( SEND_LEASE_FULL, REFUSAL, NONE ) \
    \
    /* a connection's sealed packets failed to open ROCKETNET_INTEGRITY_LIMIT */ \
    /* times across its keys and it was disconnected, per the standard's AEAD */ \
    /* integrity limit. a lifecycle counter, once per torn-down connection, */ \
    /* on whichever end reached the limit. appended: indices are the contract. */ \
    /* REFUSAL: a connection torn down after failed opens, never on a clean */ \
    /* path (its comment calls it a lifecycle counter; the clean-run */ \
    /* definition decides the class). */ \
    /** @brief Counts connections closed after reaching the authenticated-open failure limit. Class: refusal; family: none. */ \
    X( INTEGRITY_LIMIT_REACHED, REFUSAL, NONE ) \
    \
    /* client received a connection cookie packet with an invalid wire length */ \
    /* (not equal to ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES). distinct from */ \
    /* ROCKETNET_COUNTER_CLIENT_COOKIE_SALT_MISMATCH so ledger distinguishes */ \
    /* size errors from off-path salt injection. */ \
    /** @brief Counts client cookie packets with a wire size that does not match the packet type. Class: refusal; family: CLIENT_COOKIE_RECEIVED. */ \
    X( CLIENT_COOKIE_WRONG_SIZE, REFUSAL, CLIENT_COOKIE_RECEIVED ) \
    \
    /* the message layer's region (docs/CHANNELS.md 4): a send refused because */ \
    /* the connection's send store had no chunk of the class it needs (or, for */ \
    /* a block, not every chunk it needs), and a send refused because every */ \
    /* send descriptor was in use. Exhaustion is a counted refusal, never */ \
    /* growth; nothing is taken from either when one refuses. Counted on the */ \
    /* owner's counters, once per refused send. appended: indices are the contract. */ \
    /** @brief Counts message sends refused because the connection send store lacks required chunks. Class: refusal; family: none. */ \
    X( MESSAGE_SEND_STORE_FULL, REFUSAL, NONE ) \
    /** @brief Counts message sends refused because every send descriptor is occupied. Class: refusal; family: none. */ \
    X( MESSAGE_SEND_QUEUE_FULL, REFUSAL, NONE ) \
    \
    /* a cookie-valid connection request arrived after the update's handshake */ \
    /* budget was spent and was dropped before any asymmetric work, counted so */ \
    /* the drop is observable rather than silent. appended: indices are the */ \
    /* contract. */ \
    /** @brief Counts cookie-validated requests refused after the per-update handshake budget is spent. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    \
    /* a connection request whose cookie verified but whose source had already */ \
    /* spent the failed-signature-verify budget for this window: refused before */ \
    /* the Ed25519 verify ran. distinct from SIGNATURE_DID_NOT_VERIFY, which */ \
    /* counts verifies that actually ran and failed. appended: indices are the */ \
    /* contract. */ \
    /** @brief Counts cookie-validated requests refused before signature verification because the source exceeded its failure budget. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    \
    /* the UNRELIABLE path (docs/CHANNELS.md 5, 6.3, 6.4, 9.3, 11.2): a code-0 */ \
    /* record dropped on receive because the per-update unreliable delivery */ \
    /* budget Du was full (section 6.3 step 4), and a code-0 message dropped on */ \
    /* send: because finish_packet refused the packet that carried it (section */ \
    /* 6.4, Failed send), or because it is longer than the record's u16 length */ \
    /* and can never be carried. Unreliable semantics allow all three; each is */ \
    /* counted once per dropped message. appended: indices are the contract. */ \
    /** @brief Counts unreliable messages dropped because the per-update receive delivery budget is full. Class: refusal; family: none. */ \
    X( MESSAGE_UNRELIABLE_DELIVERY_FULL, REFUSAL, NONE ) \
    /** @brief Counts unreliable messages dropped because their carrier packet is refused or the record is too long. Class: refusal; family: none. */ \
    X( MESSAGE_UNRELIABLE_SEND_FAILED, REFUSAL, NONE ) \
    \
    /* Layer 3 receive window: a reliable number already received (6.5). */ \
    /* PATH_LOSS: a reliable number received twice, the resend of a lost ack */ \
    /* or packet. */ \
    /** @brief Counts reliable message numbers received more than once. Class: path_loss; family: none. */ \
    X( MESSAGE_DUPLICATE, PATH_LOSS, NONE ) \
    /* Numbering met either the count or ring bound (CHANNELS 6.2). Count */ \
    /* once per stall, even when the scheduler retries the same head. */ \
    /** @brief Counts send numbering stalls caused by a full reliable receive window. Class: refusal; family: none. */ \
    X( MESSAGE_WINDOW_STALL, REFUSAL, NONE ) \
    \
    /* Layer 3 sent-packet map and resend FIFO (docs/CHANNELS.md 6.4, 15.1): */ \
    /* a sent-packet ring entry evicted to make room for a new packet (its */ \
    /* referenced numbers stay in the resend FIFO), and a reliable record */ \
    /* resent on the RTO. appended: indices are the contract. */ \
    /** @brief Counts sent-packet map entries evicted while their message numbers remain queued for resend. Class: refusal; family: none. */ \
    X( MESSAGE_SENT_MAP_EVICTED, REFUSAL, NONE ) \
    /* PATH_LOSS: a reliable record resent on the RTO. */ \
    /** @brief Counts reliable records resent after their retransmission timeout. Class: path_loss; family: none. */ \
    X( MESSAGE_RESENT, PATH_LOSS, NONE ) \
    \
    /* a connection's message memory (its region, docs/CHANNELS.md 4) was */ \
    /* exhausted: an operation found no room, the region took its sticky */ \
    /* exhausted state, and the connection is disconnected with the reason */ \
    /* ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED (tla/RNConnectionMemory.tla, */ \
    /* Exhaust). Counted once per exhausted connection, on the owner's */ \
    /* counters, beside the refusal's own counter (MESSAGE_SEND_STORE_FULL or */ \
    /* MESSAGE_SEND_QUEUE_FULL). appended: indices are the contract. */ \
    /** @brief Counts connections closed after their message memory region is exhausted. Class: refusal; family: none. */ \
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
    /** @brief Counts message sends refused because the selected reliability is disabled. Class: refusal; family: none. */ \
    X( MESSAGE_SEND_TYPE_NOT_ENABLED, REFUSAL, NONE ) \
    /** @brief Counts incoming messages refused because their reliability is disabled. Class: refusal; family: none. */ \
    X( MESSAGE_TYPE_NOT_ENABLED, REFUSAL, NONE ) \
    /** @brief Counts unreliable messages dropped because the packet budget cannot carry them. Class: refusal; family: none. */ \
    X( MESSAGE_UNRELIABLE_OVER_BUDGET, REFUSAL, NONE ) \
    /** @brief Counts ordered records refused because their number is behind the channel read index. Class: refusal; family: none. */ \
    X( MESSAGE_ORDERING_BEHIND, REFUSAL, NONE ) \
    /** @brief Counts sequenced messages ignored because a newer sequence is already held. Class: path_loss; family: none. */ \
    X( MESSAGE_SEQUENCED_STALE, PATH_LOSS, NONE ) \
    /** @brief Counts held sequenced messages replaced by a newer sequence. Class: path_loss; family: none. */ \
    X( MESSAGE_SEQUENCED_SUPERSEDED, PATH_LOSS, NONE ) \
    /** @brief Counts sequenced messages refused because the hold slot is occupied. Class: refusal; family: none. */ \
    X( MESSAGE_SEQUENCED_HOLD_FULL, REFUSAL, NONE ) \
    /** @brief Counts message sends refused because the receipt ring has no free entry. Class: refusal; family: none. */ \
    X( MESSAGE_RECEIPT_RING_FULL, REFUSAL, NONE ) \
    /** @brief Counts message receipts acknowledged by the peer. Class: expected; family: none. */ \
    X( MESSAGE_RECEIPT_ACKED, EXPECTED, NONE ) \
    /** @brief Counts message receipts whose packet was lost on the path. Class: path_loss; family: none. */ \
    X( MESSAGE_RECEIPT_LOSS, PATH_LOSS, NONE ) \
    \
    /* The connection response ladder's own internal error: the denied */ \
    /* packet or the first keep-alive could not be written after the */ \
    /* response was received. It was counted as */ \
    /* CONNECTION_REQUEST_INTERNAL_ERROR, which put one outcome in two */ \
    /* families (the request ladder and the response ladder); it is its */ \
    /* own counter now. appended: indices are the contract. */ \
    /** @brief Counts connection responses whose follow-up packet could not be written. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_INTERNAL_ERROR, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    \
    /* A connection request from an address that holds a handshake in */ \
    /* flight, carrying a different salt than the mapping's: ignored, and */ \
    /* until now not counted, so the request family lost the packet. It is */ \
    /* a new attempt from the same address while the old one lives; the */ \
    /* old mapping answers only its own salt. appended: indices are the */ \
    /* contract. */ \
    /** @brief Counts requests refused because an in-flight mapping has a different cookie salt. Class: refusal; family: CONNECTION_REQUEST_RECEIVED. */ \
    X( CONNECTION_REQUEST_MAPPING_SALT_MISMATCH, REFUSAL, CONNECTION_REQUEST_RECEIVED ) \
    \
    /* History was overwritten before its ACK, and the peer had acknowledged */ \
    /* a sequence at least ROCKETNET_LOSS_PACKET_THRESHOLD above it: the */ \
    /* packet was lost on the path. An outcome, not a refusal. Together with */ \
    /* SEND_HISTORY_OVERFLOW this is the count of overwritten unacknowledged */ \
    /* send records (tla/RNSentHistory.tla). Advisory: the packet is still */ \
    /* transmitted. PATH_LOSS: it moves only when the path loses packets. */ \
    /* appended: indices are the contract. */ \
    /** @brief Counts send records confirmed lost by the acknowledgement window. Class: path_loss; family: none. */ \
    X( SEND_HISTORY_LOST, PATH_LOSS, NONE ) \
    \
    /* RELIABLE_ORDERED's receive path (docs/CHANNELS.md 7.2, 8): each is a */ \
    /* wire violation, counted once per record, and each ends the */ \
    /* connection with ROCKETNET_MESSAGE_ERROR_PROTOCOL_VIOLATION. A record */ \
    /* whose channel is at or past this end's count; a block fragment whose */ \
    /* count or index disagrees with the block's first-seen count, or whose */ \
    /* count is above ceil(max_message_bytes / F), or whose length is not F */ \
    /* (or 1 to F for the last); a record whose span lies outside the */ \
    /* receive ring; an ordering index a window or more ahead of the */ \
    /* channel's read index; and the total of every violation that ended a */ \
    /* connection. REFUSAL: a clean peer never sends one. appended: indices */ \
    /* are the contract. */ \
    /** @brief Counts records refused because their channel index is outside the configured range. Class: refusal; family: none. */ \
    X( MESSAGE_BAD_CHANNEL, REFUSAL, NONE ) \
    /** @brief Counts records refused because block fragment metadata is invalid. Class: refusal; family: none. */ \
    X( MESSAGE_BAD_BLOCK, REFUSAL, NONE ) \
    /** @brief Counts records refused because their span is outside the receive ring. Class: refusal; family: none. */ \
    X( MESSAGE_BAD_RING, REFUSAL, NONE ) \
    /** @brief Counts ordered records refused because their number is beyond the receive window. Class: refusal; family: none. */ \
    X( MESSAGE_ORDERING_AHEAD, REFUSAL, NONE ) \
    /** @brief Counts connections ended after a message protocol violation. Class: refusal; family: none. */ \
    X( MESSAGE_PROTOCOL_VIOLATION, REFUSAL, NONE ) \
    \
    /* Message-object refusal, stale batch entry, and unreleased receive */ \
    /* view. Appended so the enum, names, classes, and families agree. */ \
    /** @brief Counts message sends refused by the message layer. Class: refusal; family: none. */ \
    X( MESSAGE_SEND_REFUSED, REFUSAL, NONE ) \
    /** @brief Counts batch entries ignored because the connection in that slot has changed. Class: expected; family: none. */ \
    X( MESSAGE_STALE_CONNECTION_ENTRY, EXPECTED, NONE ) \
    /** @brief Counts updates refused while a received message view remains held. Class: refusal; family: none. */ \
    X( MESSAGE_NOT_RELEASED, REFUSAL, NONE ) \
    \
    /* The kernel prefilter counters (sdk/rocketnet_wire.h */ \
    /* ROCKETNET_SERVER_XDP_COUNTER_*), surfaced through the transport */ \
    /* counter surface by rocketnet_counters_apply_prefilter */ \
    /* (sdk/rocketnet_counters.cpp). appended: indices are the contract. */ \
    /** @brief Counts packets redirected by the kernel prefilter. Class: expected; family: none. */ \
    X( XDP_KERNEL_REDIRECTED, EXPECTED, NONE ) \
    /** @brief Counts packets dropped by the kernel prefilter for being too small. Class: refusal; family: none. */ \
    X( XDP_KERNEL_DROPPED_TOO_SMALL, REFUSAL, NONE ) \
    /** @brief Counts packets dropped by the kernel prefilter for being too large. Class: refusal; family: none. */ \
    X( XDP_KERNEL_DROPPED_TOO_LARGE, REFUSAL, NONE ) \
    /** @brief Counts packets dropped by the kernel prefilter for failing its filter. Class: refusal; family: none. */ \
    X( XDP_KERNEL_DROPPED_WRONG_FILTER, REFUSAL, NONE ) \
    /** @brief Counts packets dropped by the kernel prefilter for an invalid packet type. Class: refusal; family: none. */ \
    X( XDP_KERNEL_DROPPED_INVALID_TYPE, REFUSAL, NONE ) \
    /** @brief Counts packets dropped by the kernel prefilter for an unexpected epoch. Class: refusal; family: none. */ \
    X( XDP_KERNEL_DROPPED_WRONG_EPOCH, REFUSAL, NONE ) \
    /** @brief Counts packets passed by the kernel prefilter because they contain an IPv6 extension header. Class: expected; family: none. */ \
    X( XDP_KERNEL_PASSED_IPV6_EXTENSION, EXPECTED, NONE ) \
    \
    /* A connection response that opened for a token already in the */ \
    /* admitted-token set. Retired with the set (docs/SESSION.md 1.6): the */ \
    /* client-id check refuses the second of two addresses holding one */ \
    /* token while the first is connected. appended: indices are the */ \
    /* contract, so it keeps its index and always reads zero. */ \
    /** @brief Counts retired counter slot; it stays zero while the client-id check handles this refusal. Class: refusal; family: CONNECTION_RESPONSE_RECEIVED. */ \
    X( CONNECTION_RESPONSE_TOKEN_ALREADY_CONNECTED, REFUSAL, CONNECTION_RESPONSE_RECEIVED ) \
    \
    /* A client hand-off deferred because the other send ring is still */ \
    /* handed off or sending (docs/CLIENT-IO.md, Reuse only after sent; a */ \
    /* hand-off never waits; tla/RNClientIo.tla Defer): the committed slots */ \
    /* stay in the Filling ring and leave with the next hand-off. EXPECTED: */ \
    /* deferral is the common case, not an overload signal. appended: */ \
    /* indices are the contract. */ \
    /** @brief Counts client packet handoffs deferred while the other send ring is busy. Class: expected; family: none. */ \
    X( CLIENT_SEND_DEFERRED, EXPECTED, NONE ) \
    \
    /* The kernel prefilter counter ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REPLAYED */ \
    /* (sdk/rocketnet_wire.h): a session packet behind the replay map's */ \
    /* high-water window, surfaced by rocketnet_counters_apply_prefilter. */ \
    /* appended: indices are the contract. */ \
    /** @brief Counts session packets dropped by the kernel replay window as older than its high-water mark. Class: refusal; family: none. */ \
    X( XDP_KERNEL_DROPPED_REPLAYED, REFUSAL, NONE )

/**
 * @brief Integer indices for the cumulative protocol counters.
 *
 * Values are generated in `ROCKETNET_COUNTER_ROWS` order and remain stable;
 * use `rocketnet_num_counters` to obtain the number of entries across FFI.
 */
enum
{
#define ROCKETNET_COUNTER_ENUM_ROW( name, klass, family ) ROCKETNET_COUNTER_##name,
    ROCKETNET_COUNTER_ROWS( ROCKETNET_COUNTER_ENUM_ROW )
#undef ROCKETNET_COUNTER_ENUM_ROW

    /** @brief Number of published counter indices. */
    ROCKETNET_NUM_COUNTERS
};

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

    /** @brief Internal per-server count of cookie-validated requests processed in this update. */
    uint64_t handshakes_processed_this_update;
};

/**
 * @brief Copy cumulative kernel prefilter totals into their matching counters.
 *
 * `prefilter` contains `ROCKETNET_SERVER_XDP_NUM_COUNTERS` values in
 * `ROCKETNET_SERVER_XDP_COUNTER_*` order. Each source total replaces its
 * mapped counter value; all other counter values are unchanged. Both pointers
 * must be valid and the caller must own the counter block for this write.
 * @param counters Counter block to update; NULL is a counted no-op.
 * @param prefilter Array of at least `ROCKETNET_SERVER_XDP_NUM_COUNTERS` totals; NULL is a counted no-op.
 * @return None.
 */
void rocketnet_counters_apply_prefilter( struct rocketnet_counters_t * counters, const uint64_t * prefilter );

#include "rocketnet.h"                  // rocketnet_counter_name lives in the forest header

#endif // #ifndef ROCKETNET_COUNTERS_H
