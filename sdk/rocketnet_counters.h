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

enum
{
    // One enum per distinct reason, arrival counted separately, per packet
    // type. Indices become a wire contract the moment any consumer exists;
    // none does yet, so this is the last free reordering. From then on new
    // counters append.

    // the packet entry, before any type dispatches
    ROCKETNET_COUNTER_PACKETS_RECEIVED = 0,
    ROCKETNET_COUNTER_PREFILTER_TOO_SHORT,
    ROCKETNET_COUNTER_PREFILTER_TOO_LARGE,
    ROCKETNET_COUNTER_PREFILTER_WRONG_EPOCH,
    ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE,
    ROCKETNET_COUNTER_RESERVED_TYPE,
    ROCKETNET_COUNTER_WRONG_DIRECTION,

    // connection request processing, one per check in the standard's order
    ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_ALREADY_CONNECTED,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_CHALLENGE_RESENT,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_PROTOCOL_ID,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_TIMEOUT_INVALID,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_BAD_ADDRESS_FLAGS,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_EXPIRED,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_SEALED_MISMATCH,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_CLIENT_ID_ALREADY_CONNECTED,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_SMALL_ORDER_KEY,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_SENT_CHALLENGE,
    ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR,

    // connection response processing, in the standard's order
    ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED,
    ROCKETNET_COUNTER_CONNECTION_RESPONSE_WRONG_SIZE,
    ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_MAPPING,
    ROCKETNET_COUNTER_CONNECTION_RESPONSE_ZERO_SEQUENCE,
    ROCKETNET_COUNTER_CONNECTION_RESPONSE_DID_NOT_OPEN,
    ROCKETNET_COUNTER_CONNECTION_RESPONSE_ALREADY_CONNECTED,
    ROCKETNET_COUNTER_CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED,
    ROCKETNET_COUNTER_CONNECTION_RESPONSE_ADMITTED_SET_FULL,
    ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_SLOTS,
    ROCKETNET_COUNTER_CONNECTION_RESPONSE_CONNECTED,

    // keep-alive, payload and disconnect at the server
    ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED,
    ROCKETNET_COUNTER_KEEP_ALIVE_NO_MAPPING,
    ROCKETNET_COUNTER_KEEP_ALIVE_ZERO_SEQUENCE,
    ROCKETNET_COUNTER_KEEP_ALIVE_ALREADY_RECEIVED,
    ROCKETNET_COUNTER_KEEP_ALIVE_DID_NOT_OPEN,
    ROCKETNET_COUNTER_KEEP_ALIVE_NOT_CONNECTED,
    ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED,

    ROCKETNET_COUNTER_PAYLOAD_RECEIVED,
    ROCKETNET_COUNTER_PAYLOAD_NO_MAPPING,
    ROCKETNET_COUNTER_PAYLOAD_ZERO_SEQUENCE,
    ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED,
    ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN,
    ROCKETNET_COUNTER_PAYLOAD_NOT_CONNECTED,
    ROCKETNET_COUNTER_PAYLOAD_DELIVERED,

    ROCKETNET_COUNTER_DISCONNECT_RECEIVED,
    ROCKETNET_COUNTER_DISCONNECT_NO_MAPPING,
    ROCKETNET_COUNTER_DISCONNECT_ZERO_SEQUENCE,
    ROCKETNET_COUNTER_DISCONNECT_ALREADY_RECEIVED,
    ROCKETNET_COUNTER_DISCONNECT_DID_NOT_OPEN,
    ROCKETNET_COUNTER_DISCONNECT_NOT_CONNECTED,
    ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED,

    // mapping and client lifecycle
    ROCKETNET_COUNTER_MAPPING_CREATED,
    ROCKETNET_COUNTER_MAPPING_IDLE_TIMEOUT,
    ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED,
    ROCKETNET_COUNTER_MAPPING_TABLE_FULL,
    ROCKETNET_COUNTER_CLIENT_CONNECTED,
    ROCKETNET_COUNTER_SEND_RING_FULL,
    ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING,
    ROCKETNET_COUNTER_CLIENT_DISCONNECTED,
    ROCKETNET_COUNTER_SERVER_DISCONNECTED_CLIENT,
    ROCKETNET_COUNTER_CLIENT_TIMED_OUT,

    // the client's receive path. the shared packet counters above serve both
    // directions; these are the client-specific reasons.
    ROCKETNET_COUNTER_CLIENT_COOKIE_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_COOKIE_WRONG_STATE,
    ROCKETNET_COUNTER_CLIENT_COOKIE_SALT_MISMATCH,
    ROCKETNET_COUNTER_CLIENT_COOKIE_STORED,
    ROCKETNET_COUNTER_CLIENT_CHALLENGE_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_CHALLENGE_WRONG_STATE,
    ROCKETNET_COUNTER_CLIENT_CHALLENGE_ZERO_SEQUENCE,
    ROCKETNET_COUNTER_CLIENT_CHALLENGE_DID_NOT_OPEN,
    ROCKETNET_COUNTER_CLIENT_CHALLENGE_SMALL_ORDER_KEY,
    ROCKETNET_COUNTER_CLIENT_CHALLENGE_ACCEPTED,
    ROCKETNET_COUNTER_CLIENT_DENIED_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_DENIED_WRONG_STATE,
    ROCKETNET_COUNTER_CLIENT_DENIED_ZERO_SEQUENCE,
    ROCKETNET_COUNTER_CLIENT_DENIED_DID_NOT_OPEN,
    ROCKETNET_COUNTER_CLIENT_DENIED_ACCEPTED,
    ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_STATE,
    ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ZERO_SEQUENCE,
    ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ALREADY_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_DID_NOT_OPEN,
    ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ACCEPTED,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_STATE,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_ZERO_SEQUENCE,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_DID_NOT_OPEN,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED,
    ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_STATE,
    ROCKETNET_COUNTER_CLIENT_DISCONNECT_ZERO_SEQUENCE,
    ROCKETNET_COUNTER_CLIENT_DISCONNECT_ALREADY_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_DISCONNECT_DID_NOT_OPEN,
    ROCKETNET_COUNTER_CLIENT_DISCONNECT_ACCEPTED,

    // The general acks, appended per the append-only index contract. The
    // session types gain a WRONG_SIZE outcome (below the type's wire floor,
    // never a later refusal) and a BAD_ACK_BLOCK outcome (the block at the
    // front of the sealed data refused the whole packet); type 8 gets the
    // full per-type family on both sides. ACK_UNKNOWN_SEQUENCE is a side
    // counter outside the RECEIVED == sum-of-outcomes invariant: the packet
    // that fires it still succeeds, and one block may fire it several
    // times. ACKED_BATCH_FULL is this implementation's own counter: a
    // packet id whose delivery notification was deferred to a later update
    // because that update's batch was full -- deferred, never lost.

    ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_SIZE,
    ROCKETNET_COUNTER_KEEP_ALIVE_BAD_ACK_BLOCK,
    ROCKETNET_COUNTER_PAYLOAD_WRONG_SIZE,
    ROCKETNET_COUNTER_PAYLOAD_BAD_ACK_BLOCK,
    ROCKETNET_COUNTER_DISCONNECT_WRONG_SIZE,
    ROCKETNET_COUNTER_DISCONNECT_BAD_ACK_BLOCK,

    ROCKETNET_COUNTER_ACK_RECEIVED,
    ROCKETNET_COUNTER_ACK_WRONG_SIZE,
    ROCKETNET_COUNTER_ACK_NO_MAPPING,
    ROCKETNET_COUNTER_ACK_ZERO_SEQUENCE,
    ROCKETNET_COUNTER_ACK_ALREADY_RECEIVED,
    ROCKETNET_COUNTER_ACK_DID_NOT_OPEN,
    ROCKETNET_COUNTER_ACK_NOT_CONNECTED,
    ROCKETNET_COUNTER_ACK_BAD_ACK_BLOCK,
    ROCKETNET_COUNTER_ACK_ACCEPTED,

    ROCKETNET_COUNTER_ACK_UNKNOWN_SEQUENCE,
    ROCKETNET_COUNTER_ACKED_BATCH_FULL,

    ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_SIZE,
    ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_SIZE,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_BAD_ACK_BLOCK,
    ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_SIZE,
    ROCKETNET_COUNTER_CLIENT_DISCONNECT_BAD_ACK_BLOCK,

    ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_ACK_WRONG_STATE,
    ROCKETNET_COUNTER_CLIENT_ACK_WRONG_SIZE,
    ROCKETNET_COUNTER_CLIENT_ACK_ZERO_SEQUENCE,
    ROCKETNET_COUNTER_CLIENT_ACK_ALREADY_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_ACK_DID_NOT_OPEN,
    ROCKETNET_COUNTER_CLIENT_ACK_BAD_ACK_BLOCK,
    ROCKETNET_COUNTER_CLIENT_ACK_ACCEPTED,

    ROCKETNET_COUNTER_CLIENT_ACK_UNKNOWN_SEQUENCE,
    ROCKETNET_COUNTER_CLIENT_ACKED_BATCH_FULL,

    // WRONG_OPENED_SIZE: the opened data, less the ack block, did not match
    // the type -- its own reason, distinct from WRONG_SIZE (the wire floor)
    // and DID_NOT_OPEN (the seal refused), per the one-counter-per-reason
    // rule. Appended per the append-only index contract.

    ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_OPENED_SIZE,
    ROCKETNET_COUNTER_PAYLOAD_WRONG_OPENED_SIZE,
    ROCKETNET_COUNTER_DISCONNECT_WRONG_OPENED_SIZE,
    ROCKETNET_COUNTER_ACK_WRONG_OPENED_SIZE,
    ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_OPENED_SIZE,
    ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_OPENED_SIZE,
    ROCKETNET_COUNTER_CLIENT_ACK_WRONG_OPENED_SIZE,

    // The payload fragment, type 9, appended per the append-only index
    // contract: the sealed-type family both sides, then the reassembly
    // refusal reasons from the standard's _Fragmentation_ in their check
    // order, the per-packet ACCEPTED outcome, and the payload-level side
    // counters outside the RECEIVED == sum-of-outcomes invariant --
    // REASSEMBLED and ABANDONED fire per payload, not per packet.
    //
    // FRAGMENT_ALREADY_RECEIVED is the replay-protection outcome, spelled
    // as every session type spells it; the duplicate-fragment-index
    // refusal is FRAGMENT_DUPLICATE_INDEX, per the standard's ruled
    // naming. One counter here is this implementation's own, not the
    // standard's: FRAGMENT_PAYLOAD_BATCH_FULL counts a completed payload
    // dropped because the update's delivery batch was full -- it stays
    // zero in ordinary operation, so delivered == REASSEMBLED - BATCH_FULL.

    ROCKETNET_COUNTER_FRAGMENT_RECEIVED,
    ROCKETNET_COUNTER_FRAGMENT_WRONG_SIZE,
    ROCKETNET_COUNTER_FRAGMENT_NO_MAPPING,
    ROCKETNET_COUNTER_FRAGMENT_ZERO_SEQUENCE,
    ROCKETNET_COUNTER_FRAGMENT_ALREADY_RECEIVED,
    ROCKETNET_COUNTER_FRAGMENT_DID_NOT_OPEN,
    ROCKETNET_COUNTER_FRAGMENT_NOT_CONNECTED,
    ROCKETNET_COUNTER_FRAGMENT_BAD_ACK_BLOCK,
    ROCKETNET_COUNTER_FRAGMENT_WRONG_OPENED_SIZE,
    ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID,
    ROCKETNET_COUNTER_FRAGMENT_STALE,
    ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID,
    ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE,
    ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE,
    ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX,
    ROCKETNET_COUNTER_FRAGMENT_ACCEPTED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL,

    ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_STATE,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_SIZE,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_ZERO_SEQUENCE,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_ALREADY_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_DID_NOT_OPEN,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_BAD_ACK_BLOCK,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_OPENED_SIZE,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_ZERO_PAYLOAD_ID,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_STALE,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_COUNT_INVALID,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_INDEX_OUT_OF_RANGE,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_COUNT_MISMATCH,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_SIZE_MISMATCH,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_TOO_LARGE,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_DUPLICATE_INDEX,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_ACCEPTED,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_ABANDONED,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL,

    // the send side: a payload requested above the wire's capacity found
    // the fill phase's arena exhausted, and was dropped at start_packet --
    // the arena sibling of SEND_RING_FULL

    ROCKETNET_COUNTER_SEND_ARENA_FULL,

    // the copying receive cursor was handed a buffer smaller than a
    // delivered payload: the payload is refused whole, never truncated.
    // stays zero for every caller sizing by the configured maximum.

    ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL,

    // History was overwritten before its ACK, or a writer fell behind the
    // retained sequence window. Advisory: the packet is still transmitted.
    ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW,
    // Private packet metadata budget exhausted during a fill phase.
    ROCKETNET_COUNTER_SEND_LEASE_FULL,

    // a connection's sealed packets failed to open ROCKETNET_INTEGRITY_LIMIT
    // times across its keys and it was disconnected, per the standard's AEAD
    // integrity limit. a lifecycle counter, once per torn-down connection,
    // on whichever end reached the limit. appended: indices are the contract.
    ROCKETNET_COUNTER_INTEGRITY_LIMIT_REACHED,

    // client received a connection cookie packet with an invalid wire length
    // (not equal to ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES). distinct from
    // ROCKETNET_COUNTER_CLIENT_COOKIE_SALT_MISMATCH so ledger distinguishes
    // size errors from off-path salt injection.
    ROCKETNET_COUNTER_CLIENT_COOKIE_WRONG_SIZE,

    // the message layer's region (docs/CHANNELS.md 4): a send refused because
    // the connection's send store had no chunk of the class it needs (or, for
    // a block, not every chunk it needs), and a send refused because every
    // send descriptor was in use. Exhaustion is a counted refusal, never
    // growth; nothing is taken from either when one refuses. Counted on the
    // owner's counters, once per refused send. appended: indices are the contract.
    ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL,
    ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL,

    // a cookie-valid connection request arrived after the update's handshake
    // budget was spent and was dropped before any asymmetric work, counted so
    // the drop is observable rather than silent. appended: indices are the
    // contract.
    ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED,

    // a connection request whose cookie verified but whose source had already
    // spent the failed-signature-verify budget for this window: refused before
    // the Ed25519 verify ran. distinct from SIGNATURE_DID_NOT_VERIFY, which
    // counts verifies that actually ran and failed. appended: indices are the
    // contract.
    ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT,

    // the UNRELIABLE path (docs/CHANNELS.md 5, 6.3, 6.4, 9.3, 11.2): a code-0
    // record dropped on receive because the per-update unreliable delivery
    // budget Du was full (section 6.3 step 4), and a code-0 message dropped on
    // send: because finish_packet refused the packet that carried it (section
    // 6.4, Failed send), or because it is longer than the record's u16 length
    // and can never be carried. Unreliable semantics allow all three; each is
    // counted once per dropped message. appended: indices are the contract.
    ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL,
    ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED,

    // Layer 3 receive window: a reliable number already received (6.5).
    ROCKETNET_COUNTER_MESSAGE_DUPLICATE,
    // Numbering met either the count or ring bound (CHANNELS 6.2). Count
    // once per stall, even when the scheduler retries the same head.
    ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL,

    // Layer 3 sent-packet map and resend FIFO (docs/CHANNELS.md 6.4, 15.1):
    // a sent-packet ring entry evicted to make room for a new packet (its
    // referenced numbers stay in the resend FIFO), and a reliable record
    // resent on the RTO. appended: indices are the contract.
    ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED,
    ROCKETNET_COUNTER_MESSAGE_RESENT,

    // a connection's message memory (its region, docs/CHANNELS.md 4) was
    // exhausted: an operation found no room, the region took its sticky
    // exhausted state, and the connection is disconnected with the reason
    // ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED (tla/RNConnectionMemory.tla,
    // Exhaust). Counted once per exhausted connection, on the owner's
    // counters, beside the refusal's own counter (MESSAGE_SEND_STORE_FULL or
    // MESSAGE_SEND_QUEUE_FULL). appended: indices are the contract.
    ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED,

    // Layer 4's dispatch (docs/CHANNELS.md 7.2, 7.3, 7.4, 7.5): a send refused
    // because its reliability is outside the enabled mask, a receive of an
    // application record whose code the mask does not admit, an unreliable
    // record dropped because the send budget could not pay the packet that
    // would carry it, and the counters the sequenced and receipt cards need,
    // appended now so those cards run without touching this header. An ordered
    // record behind its channel's read index is MESSAGE_ORDERING_BEHIND (7.2:
    // layer 3 already caught the duplicates, so it is a violation).
    // appended: indices are the contract.
    ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED,
    ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED,
    ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET,
    ROCKETNET_COUNTER_MESSAGE_ORDERING_BEHIND,
    ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE,
    ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED,
    ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL,
    ROCKETNET_COUNTER_MESSAGE_RECEIPT_RING_FULL,
    ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED,
    ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS,

    // RELIABLE_ORDERED's receive path (docs/CHANNELS.md 7.2, 8): each is a wire
    // violation, counted once per record, and each ends the connection with
    // ROCKETNET_MESSAGE_ERROR_PROTOCOL_VIOLATION. A record whose channel is at
    // or past this end's count; a block fragment whose count or index
    // disagrees with the block's first-seen count, or whose count is above
    // ceil(max_message_bytes / F), or whose length is not F (or 1 to F for the
    // last); a record whose span lies outside the receive ring; an ordering
    // index a window or more ahead of the channel's read index; and the total
    // of every violation that ended a connection. appended: indices are the
    // contract.
    ROCKETNET_COUNTER_MESSAGE_BAD_CHANNEL,
    ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK,
    ROCKETNET_COUNTER_MESSAGE_BAD_RING,
    ROCKETNET_COUNTER_MESSAGE_ORDERING_AHEAD,
    ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION,

    // the message objects (sdk/rocketnet_message_server.cpp, tla/RNSlots.tla):
    // a send refused for a bad argument (no connection, a bad channel or size,
    // a start already pending); a transport batch entry -- a payload or an
    // acked packet id -- discarded because its slot's connection changed in
    // the update (RNSlots Admit); and each received message still held when
    // update was called (RejectUnreleasedUpdate). appended: indices are the
    // contract.
    ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED,
    ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY,
    ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED,

    ROCKETNET_NUM_COUNTERS
};

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

#include "rocketnet.h"                  // rocketnet_counter_name lives in the forest header

#endif // #ifndef ROCKETNET_COUNTERS_H
