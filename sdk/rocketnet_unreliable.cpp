/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_unreliable.h"
#include "rocketnet_platform.h"

#include <string.h>

// docs/CHANNELS.md rules kept here: R1 (no live allocation -- every byte is
// the region's or a transport frame's), R4 (pure module: one struct plus the
// region), R5 (few copies, counted). The wire is the layer 2 codec; its rules
// L2.1 to L2.4 and the receiver's checks live in the reader. No TLA+ module
// covers this layer; it holds no state a model would check.

static void rn_unreliable_count( rocketnet_counters_t * counters, int counter )
{
    // the owner's counters are shared by all of an object's regions and sends
    // may come from different threads (16.1), so a drop adds atomically
    if ( counters )
        rocketnet_platform_atomic_add_u64( (volatile uint64_t*) &counters->values[counter], 1 );
}

static uint32_t rn_unreliable_chunk_bytes( const rn_send_store_t * store, uint32_t chunk )
{
    return store->classes[rn_send_store_chunk_class( chunk )].chunk_bytes;
}

// copies a whole chunk chain's bytes into out (at least bytes available)

static void rn_unreliable_chunk_read( const rn_send_store_t * store, uint32_t chunk, uint8_t * out, int bytes )
{
    int offset = 0;
    while ( chunk != RN_CHUNK_NONE && offset < bytes )
    {
        const uint32_t take = (uint32_t) bytes - (uint32_t) offset < rn_unreliable_chunk_bytes( store, chunk )
            ? (uint32_t) bytes - (uint32_t) offset : rn_unreliable_chunk_bytes( store, chunk );
        memcpy( out + offset, rn_send_store_chunk_data( store, chunk ), take );
        offset += (int) take;
        chunk = rn_send_store_chunk_next( store, chunk );
    }
}

// copies src into a freshly allocated chunk chain (the one copy of R5)

static void rn_unreliable_chunk_write( const rn_send_store_t * store, uint32_t chunk, const uint8_t * src, int bytes )
{
    int offset = 0;
    while ( chunk != RN_CHUNK_NONE && offset < bytes )
    {
        const uint32_t take = (uint32_t) bytes - (uint32_t) offset < rn_unreliable_chunk_bytes( store, chunk )
            ? (uint32_t) bytes - (uint32_t) offset : rn_unreliable_chunk_bytes( store, chunk );
        memcpy( rn_send_store_chunk_data( store, chunk ), src + offset, take );
        offset += (int) take;
        chunk = rn_send_store_chunk_next( store, chunk );
    }
}

// -----------------------------------------
//                                  send
// -----------------------------------------

void rn_unreliable_init( rn_unreliable_t * u, rn_region_t * region, rn_unreliable_message_t * queue, uint32_t capacity )
{
    u->region = region;
    u->queue = queue;
    u->capacity = capacity;
    u->head = 0;
    u->tail = 0;
    u->count = 0;
}

int rn_unreliable_send( rn_unreliable_t * u, const uint8_t * data, int bytes, int channel, int priority )
{
    rn_region_t * region = u->region;

    if ( bytes < 1 || (uint32_t) bytes > region->max_message_bytes || channel < 0 || (uint32_t) channel >= region->num_channels )
        return 0;

    // the FIFO is bounded: a full queue is a counted drop, as a full store is
    // (R1: unreliable data is dropped and counted, never grown)

    if ( u->count >= u->capacity )
    {
        rn_unreliable_count( region->counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL );
        return 0;
    }

    const uint32_t chunk = rn_send_store_alloc( &region->store, bytes );
    if ( chunk == RN_CHUNK_NONE )
    {
        rn_unreliable_count( region->counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL );
        return 0;
    }

    rn_unreliable_chunk_write( &region->store, chunk, data, bytes );

    rn_unreliable_message_t * message = &u->queue[u->tail];
    message->chunk = chunk;
    message->bytes = (uint32_t) bytes;
    message->channel = (uint16_t) channel;
    message->priority = (uint8_t) priority;
    message->in_use = 1;
    u->tail = ( u->tail + 1 ) % u->capacity;
    u->count++;
    return 1;
}

static void rn_unreliable_pop( rn_unreliable_t * u )
{
    u->queue[u->head].in_use = 0;
    u->head = ( u->head + 1 ) % u->capacity;
    u->count--;
}

// the payload capacity a packet needs to carry the head message: the packet
// size for a small one, the whole large payload for one longer than a packet
// (L2.3). This is what layer 6 asks before it calls start_packet.

int rn_unreliable_next_packet_bytes( const rn_unreliable_t * u )
{
    if ( u->count == 0 )
        return 0;
    const rn_unreliable_message_t * head = &u->queue[u->head];

    // a record plus its trailing form byte must fit a packet to aggregate (9.3);
    // at or beyond that the message travels alone in its own large payload (L2.3)

    if ( (int) head->bytes + ROCKETNET_MESSAGE_RECORD_FIXED_BYTES >= ROCKETNET_MAX_PAYLOAD_BYTES )
        return (int) head->bytes + ROCKETNET_MESSAGE_RECORD_FIXED_BYTES + 1;   // the trailing form byte (11.2)
    return ROCKETNET_MAX_PAYLOAD_BYTES;
}

int rn_unreliable_fill( rn_unreliable_t * u, uint8_t * payload, int capacity )
{
    rn_region_t * region = u->region;

    if ( u->count == 0 || capacity <= 1 )
        return 0;

    const rn_unreliable_message_t * head = &u->queue[u->head];

    // a message that does not fit a packet with its record and trailing form
    // byte is planned alone in its own large payload (L2.3), capped by the
    // record's u16 length

    if ( (int) head->bytes + ROCKETNET_MESSAGE_RECORD_FIXED_BYTES >= ROCKETNET_MAX_PAYLOAD_BYTES )
    {
        if ( head->bytes > ROCKETNET_MESSAGE_MAX_RECORD_BYTES )
            return 0;
        const int body = ROCKETNET_MESSAGE_RECORD_FIXED_BYTES + (int) head->bytes;
        if ( body + 1 > capacity )
            return 0;

        payload[0] = (uint8_t) ROCKETNET_MESSAGE_CODE_UNRELIABLE;
        payload[1] = (uint8_t) ( head->bytes & 0xFF );
        payload[2] = (uint8_t) ( ( head->bytes >> 8 ) & 0xFF );
        rn_unreliable_chunk_read( &region->store, head->chunk, payload + 3, (int) head->bytes );
        payload[body] = (uint8_t) ROCKETNET_MESSAGE_FORM_RECORDS;
        rn_send_store_free( &region->store, u->queue[u->head].chunk );
        rn_unreliable_pop( u );
        return body + 1;
    }

    // otherwise aggregate as many queued small messages as fit as code-0
    // records; a record is never split (9.3)

    struct rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, payload, capacity );

    while ( u->count > 0 )
    {
        const rn_unreliable_message_t * message = &u->queue[u->head];
        if ( (int) message->bytes + ROCKETNET_MESSAGE_RECORD_FIXED_BYTES >= ROCKETNET_MAX_PAYLOAD_BYTES )
            break;                              // the next is a large one: end this packet

        struct rocketnet_message_record_t record;
        memset( &record, 0, sizeof( record ) );
        record.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE;
        record.data = rn_send_store_chunk_data( &region->store, message->chunk );
        record.bytes = (int) message->bytes;

        if ( !rocketnet_message_writer_add( &writer, &record ) )
            break;                              // did not fit: leave it for the next packet

        // 9.3 step 3: the chunk is freed the moment the record is written

        rn_send_store_free( &region->store, message->chunk );
        rn_unreliable_pop( u );
    }

    return rocketnet_message_writer_finish( &writer );
}

void rn_unreliable_packet_failed( rn_unreliable_t * u, int records )
{
    for ( int i = 0; i < records; i++ )
        rn_unreliable_count( u->region->counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
}

// -----------------------------------------
//                               zero copy
// -----------------------------------------

int rn_unreliable_single_capacity( int payload_capacity )
{
    return rocketnet_message_single_capacity( payload_capacity );
}

uint8_t * rn_unreliable_single_begin( uint8_t * frame_payload, int payload_capacity, int request_bytes )
{
    if ( request_bytes < 1 || request_bytes > rn_unreliable_single_capacity( payload_capacity ) )
        return NULL;
    return frame_payload;
}

int rn_unreliable_single_finish( uint8_t * payload, int payload_capacity, int message_bytes )
{
    return rocketnet_message_single_finish( payload, payload_capacity, message_bytes );
}

// -----------------------------------------
//                               receive
// -----------------------------------------

int rn_unreliable_receive( rn_unreliable_t * u, const uint8_t * payload, int bytes,
                           const struct rocketnet_message_limits_t * limits,
                           rn_delivery_t * out, int out_capacity, int * out_count )
{
    if ( out_count )
        *out_count = 0;

    struct rocketnet_message_reader_t reader;
    const int violation = rocketnet_message_reader_begin( &reader, payload, bytes, limits );
    if ( violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
        return violation;

    int count = 0;
    struct rocketnet_message_record_t record;
    while ( rocketnet_message_reader_next( &reader, &record ) )
    {
        // records of any other code belong to layers 3 and 4, and are left
        // untouched for them

        if ( record.code != ROCKETNET_MESSAGE_CODE_UNRELIABLE )
            continue;

        if ( count >= out_capacity )
        {
            // Du is full: the rest are dropped and counted (6.3 step 4)

            rn_unreliable_count( u->region->counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL );
            continue;
        }

        rn_delivery_t * delivery = &out[count++];
        memset( delivery, 0, sizeof( *delivery ) );
        delivery->data = record.data;
        delivery->bytes = record.bytes;
        delivery->channel = (uint16_t) record.channel;
        delivery->code = (uint8_t) ROCKETNET_MESSAGE_CODE_UNRELIABLE;
    }

    if ( out_count )
        *out_count = count;
    return ROCKETNET_MESSAGE_VIOLATION_NONE;
}
