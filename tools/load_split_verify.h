#pragma once

#ifndef ROCKETNET_TOOLS_LOAD_SPLIT_VERIFY_H
#define ROCKETNET_TOOLS_LOAD_SPLIT_VERIFY_H

#include "rocketnet.h"

#include <stdint.h>
#include <string.h>

/* The per-stream delivery checker of the LAN proof, as pure functions over
 * caller-owned state: no time, no socket, and no allocation (every record is
 * fixed size; the caller allocates it once). A stream is one client, one
 * direction, one reliability mode and one size class. The owner's rules, per
 * stream, at the end of a run:
 *
 *   exactly once (RELIABLE, RELIABLE_ORDERED and the two reliable
 *   _WITH_ACK_RECEIPT modes): every message delivered exactly once, judged
 *   against the sender's count, which the merger supplies;
 *   ordered (RELIABLE_ORDERED, RELIABLE_ORDERED_WITH_ACK_RECEIPT): delivered
 *   in order with no gaps;
 *   sequenced (UNRELIABLE_SEQUENCED, RELIABLE_SEQUENCED): never a message
 *   older than the newest delivered and never a duplicate; a stale message
 *   the receiver drops is a skip, counted and never failed (sdk/rocketnet.h:
 *   RELIABLE_SEQUENCED "can drop a stale one", so it is not exactly once);
 *   unreliable (UNRELIABLE, UNRELIABLE_SEQUENCED, UNRELIABLE_WITH_ACK_RECEIPT):
 *   no duplicate; loss is counted, never failed;
 *   receipts (the three _WITH_ACK_RECEIPT modes): every send gets exactly one
 *   outcome, a reliable one only ever ACKED, an ACKED serial names a message
 *   the receiver recorded, and receipts_dropped is zero;
 *   every payload carries its sequence and a checksum over its bytes.
 *
 * Bound: a stream tracks sequences 1 .. SPLIT_VERIFY_MAX_SEQUENCE (8191) in a
 * 1 KB seen-bitmap. The proof offers at most one message per client per
 * update at 10 Hz, so a stream sees at most 6000 messages in 10 minutes; a
 * receipt ledger holds 8192 receipt sends per client and direction (64 KB).
 * A run past either bound is reported (window_exceeded, overflow), never
 * wrapped. */

/* "RNSV", little endian. */
#define SPLIT_VERIFY_MAGIC 0x56534e52u

/* Payload layout, little endian:
 *    0  u32 magic
 *    4  u32 global client index
 *    8  u8  direction, u8 reliability mode, u8 size class, u8 zero
 *   12  u32 sequence, 1 .. SPLIT_VERIFY_MAX_SEQUENCE
 *   16  u32 length, the whole payload in bytes
 *   20  u32 checksum: FNV-1a over every byte but these four
 *   24  u64 run id
 *   32  body, a pattern of the fields above */
#define SPLIT_VERIFY_HEADER_BYTES 32u

#define SPLIT_VERIFY_WINDOW_SEQUENCES 8192u
#define SPLIT_VERIFY_MAX_SEQUENCE ( SPLIT_VERIFY_WINDOW_SEQUENCES - 1u )
#define SPLIT_VERIFY_LEDGER_SENDS 8192u

#define SPLIT_VERIFY_CLIENT_TO_SERVER 0u
#define SPLIT_VERIFY_SERVER_TO_CLIENT 1u
#define SPLIT_VERIFY_NUM_DIRECTIONS 2u

/* small: one packet. fragmented: larger than one packet, reliable modes only;
 * an unreliable message too large for one packet is sent as the matching
 * reliable mode (sdk/rocketnet.h, ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES). */
#define SPLIT_VERIFY_SMALL 0u
#define SPLIT_VERIFY_FRAGMENTED 1u
#define SPLIT_VERIFY_NUM_SIZE_CLASSES 2u

#define SPLIT_VERIFY_NUM_MODES 8u
#define SPLIT_VERIFY_STREAMS_PER_CLIENT ( SPLIT_VERIFY_NUM_DIRECTIONS * SPLIT_VERIFY_NUM_MODES * SPLIT_VERIFY_NUM_SIZE_CLASSES )

/* The five reliable modes, in the order the proof's fragmented stream
 * rotates them. An unreliable mode has no fragmented stream: the SDK sends
 * an unreliable message too large for one packet as its reliable twin. */
#define SPLIT_VERIFY_FRAGMENTED_MODES 5u

/* A small unreliable message stays within the strictest unreliable limit
 * (UNRELIABLE_SEQUENCED on a channel of 256 or more, sdk/rocketnet.h), so it
 * is never sent as a reliable one. */
#define SPLIT_VERIFY_SMALL_UNRELIABLE_MAX_BYTES 1404u

/* The proof's rotation: offer k of client c carries the small message in
 * mode (c + k) % 8 and the fragmented message in the reliable mode
 * (c + k) % 5 of the five. The merger counts the same rotation
 * (tools/check_split_load.py, proof_expected_sent), so both ends of a run
 * agree on which stream each offer belongs to. */
static inline uint32_t split_verify_small_mode( uint32_t client, uint32_t offer )
{
    return ( client + offer ) % SPLIT_VERIFY_NUM_MODES;
}

static inline uint32_t split_verify_fragmented_mode( uint32_t client, uint32_t offer )
{
    static const uint32_t modes[SPLIT_VERIFY_FRAGMENTED_MODES] = {
        ROCKETNET_RELIABILITY_RELIABLE,
        ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
        ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED,
        ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT,
        ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT,
    };
    return modes[( client + offer ) % SPLIT_VERIFY_FRAGMENTED_MODES];
}

static inline int split_verify_mode_is_reliable( uint32_t mode )
{
    return mode == ROCKETNET_RELIABILITY_RELIABLE ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
}

/* Exactly once against the sender's count. RELIABLE_SEQUENCED is reliable on
 * the wire (resent, and so it may fragment) but may skip a stale message. */
static inline int split_verify_mode_is_exactly_once( uint32_t mode )
{
    return mode == ROCKETNET_RELIABILITY_RELIABLE ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
}

static inline int split_verify_mode_is_ordered( uint32_t mode )
{
    return mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
}

static inline int split_verify_mode_is_sequenced( uint32_t mode )
{
    return mode == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED;
}

static inline int split_verify_mode_has_receipt( uint32_t mode )
{
    return mode == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
}

/* The stream's index within one client's SPLIT_VERIFY_STREAMS_PER_CLIENT. */
static inline uint32_t split_verify_stream_index( uint32_t direction, uint32_t mode, uint32_t size_class )
{
    return ( direction * SPLIT_VERIFY_NUM_MODES + mode ) * SPLIT_VERIFY_NUM_SIZE_CLASSES + size_class;
}

struct split_verify_payload_t
{
    uint64_t run_id;
    uint32_t client;
    uint32_t direction;
    uint32_t mode;
    uint32_t size_class;
    uint32_t sequence;
    uint32_t length;
};

/* 1 when the fields name a stream the proof sends and a length its size
 * class allows. */
static inline int split_verify_payload_fields_valid( const struct split_verify_payload_t * payload )
{
    if ( payload->direction >= SPLIT_VERIFY_NUM_DIRECTIONS || payload->mode >= SPLIT_VERIFY_NUM_MODES ||
         payload->size_class >= SPLIT_VERIFY_NUM_SIZE_CLASSES ||
         payload->sequence == 0 || payload->sequence > SPLIT_VERIFY_MAX_SEQUENCE ||
         payload->length < SPLIT_VERIFY_HEADER_BYTES )
        return 0;
    if ( payload->size_class == SPLIT_VERIFY_SMALL )
        return payload->length <= ( split_verify_mode_is_reliable( payload->mode ) ? (uint32_t) ROCKETNET_MAX_PACKET_PAYLOAD_BYTES : SPLIT_VERIFY_SMALL_UNRELIABLE_MAX_BYTES );
    return split_verify_mode_is_reliable( payload->mode ) &&
           payload->length > (uint32_t) ROCKETNET_MAX_PACKET_PAYLOAD_BYTES &&
           payload->length <= (uint32_t) ROCKETNET_MAX_MESSAGE_BYTES;
}

static inline void split_verify_write_u32( uint8_t * out, uint32_t value )
{
    out[0] = (uint8_t) value;
    out[1] = (uint8_t) ( value >> 8 );
    out[2] = (uint8_t) ( value >> 16 );
    out[3] = (uint8_t) ( value >> 24 );
}

static inline uint32_t split_verify_read_u32( const uint8_t * data )
{
    return (uint32_t) data[0] | ( (uint32_t) data[1] << 8 ) | ( (uint32_t) data[2] << 16 ) | ( (uint32_t) data[3] << 24 );
}

/* FNV-1a over the payload's bytes, skipping the checksum field itself. */
static inline uint32_t split_verify_checksum( const uint8_t * data, uint32_t length )
{
    uint32_t hash = 2166136261u;
    for ( uint32_t i = 0; i < length; ++i )
    {
        if ( i >= 20 && i < 24 )
            continue;
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

/* Writes the payload the fields name into out. Returns its length, or 0 when
 * the fields are not valid or capacity is too small. */
static inline uint32_t split_verify_write_payload( uint8_t * out, uint32_t capacity, const struct split_verify_payload_t * payload )
{
    if ( !split_verify_payload_fields_valid( payload ) || capacity < payload->length )
        return 0;
    split_verify_write_u32( out, SPLIT_VERIFY_MAGIC );
    split_verify_write_u32( out + 4, payload->client );
    out[8] = (uint8_t) payload->direction;
    out[9] = (uint8_t) payload->mode;
    out[10] = (uint8_t) payload->size_class;
    out[11] = 0;
    split_verify_write_u32( out + 12, payload->sequence );
    split_verify_write_u32( out + 16, payload->length );
    split_verify_write_u32( out + 20, 0 );
    split_verify_write_u32( out + 24, (uint32_t) payload->run_id );
    split_verify_write_u32( out + 28, (uint32_t) ( payload->run_id >> 32 ) );
    uint32_t state = split_verify_checksum( out, SPLIT_VERIFY_HEADER_BYTES ) | 1u;
    for ( uint32_t i = SPLIT_VERIFY_HEADER_BYTES; i < payload->length; ++i )
    {
        state = state * 1664525u + 1013904223u;
        out[i] = (uint8_t) ( state >> 24 );
    }
    split_verify_write_u32( out + 20, split_verify_checksum( out, payload->length ) );
    return payload->length;
}

enum split_verify_read_t
{
    SPLIT_VERIFY_READ_OK = 0,
    SPLIT_VERIFY_READ_SHORT,            /* fewer bytes than a header */
    SPLIT_VERIFY_READ_MAGIC,
    SPLIT_VERIFY_READ_RUN_ID,           /* another run's payload */
    SPLIT_VERIFY_READ_LENGTH,           /* the length field is not the bytes received */
    SPLIT_VERIFY_READ_FIELDS,           /* direction, mode, size class, sequence or length out of range */
    SPLIT_VERIFY_READ_CHECKSUM
};

/* Reads and verifies one received payload. payload is written only on OK. */
static inline enum split_verify_read_t split_verify_read_payload( const uint8_t * data, uint32_t bytes, uint64_t run_id,
                                                                  struct split_verify_payload_t * payload )
{
    if ( data == NULL || bytes < SPLIT_VERIFY_HEADER_BYTES )
        return SPLIT_VERIFY_READ_SHORT;
    if ( split_verify_read_u32( data ) != SPLIT_VERIFY_MAGIC )
        return SPLIT_VERIFY_READ_MAGIC;
    struct split_verify_payload_t fields;
    fields.run_id = (uint64_t) split_verify_read_u32( data + 24 ) | ( (uint64_t) split_verify_read_u32( data + 28 ) << 32 );
    if ( fields.run_id != run_id )
        return SPLIT_VERIFY_READ_RUN_ID;
    fields.length = split_verify_read_u32( data + 16 );
    if ( fields.length != bytes )
        return SPLIT_VERIFY_READ_LENGTH;
    fields.client = split_verify_read_u32( data + 4 );
    fields.direction = data[8];
    fields.mode = data[9];
    fields.size_class = data[10];
    fields.sequence = split_verify_read_u32( data + 12 );
    if ( data[11] != 0 || !split_verify_payload_fields_valid( &fields ) )
        return SPLIT_VERIFY_READ_FIELDS;
    if ( split_verify_read_u32( data + 20 ) != split_verify_checksum( data, bytes ) )
        return SPLIT_VERIFY_READ_CHECKSUM;
    *payload = fields;
    return SPLIT_VERIFY_READ_OK;
}

/* One stream's receive record. */
struct split_verify_stream_t
{
    uint32_t client;
    uint32_t direction;
    uint32_t mode;
    uint32_t size_class;
    uint32_t delivered;             /* distinct sequences delivered */
    uint32_t newest_sequence;       /* the highest sequence delivered */
    uint32_t duplicates;            /* a sequence delivered again */
    uint32_t order_errors;          /* ordered: a delivery that is not newest_sequence + 1 */
    uint32_t stale_deliveries;      /* sequenced: a delivery older than newest_sequence */
    uint32_t identity_errors;       /* a payload that names another stream */
    uint8_t seen[SPLIT_VERIFY_WINDOW_SEQUENCES / 8];
};

static inline void split_verify_stream_init( struct split_verify_stream_t * stream, uint32_t client, uint32_t direction,
                                             uint32_t mode, uint32_t size_class )
{
    memset( stream, 0, sizeof( *stream ) );
    stream->client = client;
    stream->direction = direction;
    stream->mode = mode;
    stream->size_class = size_class;
}

static inline int split_verify_stream_has( const struct split_verify_stream_t * stream, uint32_t sequence )
{
    if ( sequence == 0 || sequence > SPLIT_VERIFY_MAX_SEQUENCE )
        return 0;
    return ( stream->seen[sequence >> 3] >> ( sequence & 7u ) ) & 1;
}

/* Records one verified delivery. Returns 1 when it broke a rule. */
static inline int split_verify_stream_deliver( struct split_verify_stream_t * stream, const struct split_verify_payload_t * payload )
{
    if ( payload->client != stream->client || payload->direction != stream->direction ||
         payload->mode != stream->mode || payload->size_class != stream->size_class ||
         payload->sequence == 0 || payload->sequence > SPLIT_VERIFY_MAX_SEQUENCE )
    {
        stream->identity_errors++;
        return 1;
    }
    const uint32_t sequence = payload->sequence;
    int broke = 0;
    if ( split_verify_stream_has( stream, sequence ) )
    {
        stream->duplicates++;
        broke = 1;
    }
    else
    {
        stream->seen[sequence >> 3] = (uint8_t) ( stream->seen[sequence >> 3] | ( 1u << ( sequence & 7u ) ) );
        stream->delivered++;
    }
    if ( split_verify_mode_is_ordered( stream->mode ) && sequence != stream->newest_sequence + 1 )
    {
        stream->order_errors++;
        broke = 1;
    }
    if ( split_verify_mode_is_sequenced( stream->mode ) && sequence < stream->newest_sequence )
    {
        stream->stale_deliveries++;
        broke = 1;
    }
    if ( sequence > stream->newest_sequence )
        stream->newest_sequence = sequence;
    return broke;
}

struct split_verify_stream_result_t
{
    uint32_t sent;
    uint32_t delivered;
    uint32_t lost;                  /* unreliable modes: sent - delivered, counted and never failed */
    uint32_t skipped;               /* RELIABLE_SEQUENCED: stale messages dropped, counted and never failed */
    uint32_t missing;               /* exactly-once modes: sent - delivered, a failure */
    uint32_t phantom;               /* delivered sequences the sender never sent */
    int window_exceeded;            /* the sender sent past SPLIT_VERIFY_MAX_SEQUENCE */
    int clean;
};

/* Judges a stream at the end of the run against the sender's count. */
static inline int split_verify_stream_final( const struct split_verify_stream_t * stream, uint32_t sent,
                                             struct split_verify_stream_result_t * result )
{
    memset( result, 0, sizeof( *result ) );
    result->sent = sent;
    result->delivered = stream->delivered;
    result->window_exceeded = sent > SPLIT_VERIFY_MAX_SEQUENCE;
    if ( stream->newest_sequence > sent )
    {
        for ( uint32_t s = sent + 1; s <= stream->newest_sequence; ++s )
            result->phantom += (uint32_t) split_verify_stream_has( stream, s );
    }
    const uint32_t delivered_sent = stream->delivered - result->phantom;
    const uint32_t short_by = sent > delivered_sent ? sent - delivered_sent : 0;
    if ( split_verify_mode_is_exactly_once( stream->mode ) )
        result->missing = short_by;
    else if ( split_verify_mode_is_reliable( stream->mode ) )
        result->skipped = short_by;
    else
        result->lost = short_by;
    result->clean = !result->window_exceeded && result->phantom == 0 && result->missing == 0 &&
                    stream->duplicates == 0 && stream->order_errors == 0 &&
                    stream->stale_deliveries == 0 && stream->identity_errors == 0;
    return result->clean;
}

#define SPLIT_VERIFY_OUTCOME_NONE 0
#define SPLIT_VERIFY_OUTCOME_ACKED 1
#define SPLIT_VERIFY_OUTCOME_LOSS 2

/* The sender's receipt ledger for one client and direction: every receipt
 * send in serial order, and the outcome each one got. */
struct split_verify_ledger_t
{
    uint32_t client;
    uint32_t direction;
    uint32_t sends;                 /* entries used */
    uint32_t overflow;              /* receipt sends past SPLIT_VERIFY_LEDGER_SENDS */
    uint32_t serial_order_errors;   /* a send whose serial is not above the last */
    uint32_t unknown_serials;       /* an outcome for no recorded send */
    uint32_t repeated_outcomes;     /* a second outcome for one send */
    uint32_t reliable_losses;       /* LOSS for a reliable receipt mode */
    uint32_t bad_status;            /* an outcome neither ACKED nor LOSS */
    uint32_t acked;
    uint32_t lost;
    uint32_t serial[SPLIT_VERIFY_LEDGER_SENDS];
    uint16_t sequence[SPLIT_VERIFY_LEDGER_SENDS];
    uint8_t mode_size[SPLIT_VERIFY_LEDGER_SENDS];      /* mode * 2 + size class */
    uint8_t outcome[SPLIT_VERIFY_LEDGER_SENDS];
};

static inline void split_verify_ledger_init( struct split_verify_ledger_t * ledger, uint32_t client, uint32_t direction )
{
    memset( ledger, 0, sizeof( *ledger ) );
    ledger->client = client;
    ledger->direction = direction;
}

/* Records a send in a _WITH_ACK_RECEIPT mode with the serial finish message
 * returned for it. Returns 1 when it broke a rule. */
static inline int split_verify_ledger_send( struct split_verify_ledger_t * ledger, uint32_t serial, uint32_t mode,
                                            uint32_t size_class, uint32_t sequence )
{
    if ( ledger->sends == SPLIT_VERIFY_LEDGER_SENDS )
    {
        ledger->overflow++;
        return 1;
    }
    if ( serial == 0 || ( ledger->sends > 0 && serial <= ledger->serial[ledger->sends - 1] ) ||
         !split_verify_mode_has_receipt( mode ) || size_class >= SPLIT_VERIFY_NUM_SIZE_CLASSES ||
         sequence == 0 || sequence > SPLIT_VERIFY_MAX_SEQUENCE )
    {
        ledger->serial_order_errors++;
        return 1;
    }
    const uint32_t i = ledger->sends++;
    ledger->serial[i] = serial;
    ledger->sequence[i] = (uint16_t) sequence;
    ledger->mode_size[i] = (uint8_t) ( mode * SPLIT_VERIFY_NUM_SIZE_CLASSES + size_class );
    ledger->outcome[i] = SPLIT_VERIFY_OUTCOME_NONE;
    return 0;
}

/* Records one receipt outcome. Returns 1 when it broke a rule. */
static inline int split_verify_ledger_outcome( struct split_verify_ledger_t * ledger, uint32_t serial, int status )
{
    uint32_t low = 0;
    uint32_t high = ledger->sends;
    while ( low < high )
    {
        const uint32_t middle = low + ( high - low ) / 2;
        if ( ledger->serial[middle] < serial )
            low = middle + 1;
        else
            high = middle;
    }
    if ( low == ledger->sends || ledger->serial[low] != serial )
    {
        ledger->unknown_serials++;
        return 1;
    }
    if ( status != ROCKETNET_RECEIPT_STATUS_ACKED && status != ROCKETNET_RECEIPT_STATUS_LOSS )
    {
        ledger->bad_status++;
        return 1;
    }
    if ( ledger->outcome[low] != SPLIT_VERIFY_OUTCOME_NONE )
    {
        ledger->repeated_outcomes++;
        return 1;
    }
    const uint32_t mode = ledger->mode_size[low] / SPLIT_VERIFY_NUM_SIZE_CLASSES;
    if ( status == ROCKETNET_RECEIPT_STATUS_ACKED )
    {
        ledger->outcome[low] = SPLIT_VERIFY_OUTCOME_ACKED;
        ledger->acked++;
        return 0;
    }
    ledger->outcome[low] = SPLIT_VERIFY_OUTCOME_LOSS;
    ledger->lost++;
    if ( split_verify_mode_is_reliable( mode ) )
    {
        ledger->reliable_losses++;
        return 1;
    }
    return 0;
}

struct split_verify_ledger_result_t
{
    uint32_t sends;
    uint32_t acked;
    uint32_t lost;
    uint32_t without_outcome;       /* a send that got no outcome */
    uint32_t acked_unrecorded;      /* ACKED, but the receiver never recorded the message */
    uint64_t receipts_dropped;
    int clean;
};

/* One stream's share of a ledger: its receipt sends, their outcomes, and a
 * seen-bitmap (as split_verify_stream_t.seen) of the sequences ACKED. When
 * sender and receiver run in different processes, the receiver's seen-bitmap
 * and this one are compared by the merger: every ACKED sequence must be one
 * the receiver recorded. */
struct split_verify_ledger_stream_t
{
    uint32_t sends;
    uint32_t acked;
    uint32_t lost;
    uint32_t without_outcome;
    uint8_t acked_seen[SPLIT_VERIFY_WINDOW_SEQUENCES / 8];
};

static inline void split_verify_ledger_stream( const struct split_verify_ledger_t * ledger, uint32_t mode, uint32_t size_class,
                                               struct split_verify_ledger_stream_t * out )
{
    memset( out, 0, sizeof( *out ) );
    const uint32_t mode_size = mode * SPLIT_VERIFY_NUM_SIZE_CLASSES + size_class;
    for ( uint32_t i = 0; i < ledger->sends; ++i )
    {
        if ( ledger->mode_size[i] != mode_size )
            continue;
        out->sends++;
        if ( ledger->outcome[i] == SPLIT_VERIFY_OUTCOME_NONE )
            out->without_outcome++;
        else if ( ledger->outcome[i] == SPLIT_VERIFY_OUTCOME_LOSS )
            out->lost++;
        else
        {
            const uint32_t sequence = ledger->sequence[i];
            out->acked++;
            out->acked_seen[sequence >> 3] = (uint8_t) ( out->acked_seen[sequence >> 3] | ( 1u << ( sequence & 7u ) ) );
        }
    }
}

/* The ledger's rule breaks that need no receiver record. */
static inline uint32_t split_verify_ledger_errors( const struct split_verify_ledger_t * ledger )
{
    return ledger->overflow + ledger->serial_order_errors + ledger->unknown_serials + ledger->repeated_outcomes +
           ledger->reliable_losses + ledger->bad_status;
}

/* Judges the ledger at the end of the run. receiver_streams is the receiving
 * end's SPLIT_VERIFY_STREAMS_PER_CLIENT records for this client, indexed by
 * split_verify_stream_index, or NULL when the receiver is another process
 * (the merger then checks every ACKED sequence against the receiver's
 * bitmap, split_verify_ledger_stream); receipts_dropped is the sender's
 * stats counter. */
static inline int split_verify_ledger_final( const struct split_verify_ledger_t * ledger,
                                             const struct split_verify_stream_t * receiver_streams,
                                             uint64_t receipts_dropped, struct split_verify_ledger_result_t * result )
{
    memset( result, 0, sizeof( *result ) );
    result->sends = ledger->sends;
    result->acked = ledger->acked;
    result->lost = ledger->lost;
    result->receipts_dropped = receipts_dropped;
    for ( uint32_t i = 0; i < ledger->sends; ++i )
    {
        if ( ledger->outcome[i] == SPLIT_VERIFY_OUTCOME_NONE )
        {
            result->without_outcome++;
            continue;
        }
        if ( ledger->outcome[i] != SPLIT_VERIFY_OUTCOME_ACKED || receiver_streams == NULL )
            continue;
        const uint32_t mode = ledger->mode_size[i] / SPLIT_VERIFY_NUM_SIZE_CLASSES;
        const uint32_t size_class = ledger->mode_size[i] % SPLIT_VERIFY_NUM_SIZE_CLASSES;
        const struct split_verify_stream_t * stream = &receiver_streams[split_verify_stream_index( ledger->direction, mode, size_class )];
        if ( !split_verify_stream_has( stream, ledger->sequence[i] ) )
            result->acked_unrecorded++;
    }
    result->clean = receipts_dropped == 0 && result->without_outcome == 0 && result->acked_unrecorded == 0 &&
                    split_verify_ledger_errors( ledger ) == 0;
    return result->clean;
}

#endif // ROCKETNET_TOOLS_LOAD_SPLIT_VERIFY_H
