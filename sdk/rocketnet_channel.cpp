/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Layer 3, first half: the UNRELIABLE paths (docs/CHANNELS.md 5, 7.2, 9.3,
    11.2). Pure, per R4: no socket, no clock read, no allocation. Every buffer
    is the caller's payload or the region's send store (layer 1); every copy is
    R5's, named at its line.
*/

#include "rocketnet_channel.h"

#include <stddef.h>
#include <string.h>

// -----------------------------------------
//                            the send store
// -----------------------------------------

// One record's data, out of the region's send store and into the caller's
// frame. A message at or under one store chunk is one memcpy; a larger one is
// a chain and copies chunk by chunk (CHANNELS.md R5: the store-to-frame copy).

static void rn_channel_store_read( const rn_send_store_t * store, uint32_t handle, uint8_t * destination, int bytes )
{
    int remaining = bytes;
    uint32_t h = handle;
    while ( remaining > 0 )
    {
        const uint8_t * source = rn_send_store_chunk_data( store, h );
        const int chunk_bytes = (int) store->classes[rn_send_store_chunk_class( h )].chunk_bytes;
        const int take = remaining < chunk_bytes ? remaining : chunk_bytes;
        memcpy( destination, source, take );
        destination += take;
        remaining -= take;
        if ( remaining > 0 )
        {
            h = rn_send_store_chunk_next( store, h );
            rocketnet_assert( h != RN_CHUNK_NONE );
        }
    }
}

// The application's data into the store, the same walk in reverse.

static void rn_channel_store_write( const rn_send_store_t * store, uint32_t handle, const uint8_t * source, int bytes )
{
    int remaining = bytes;
    uint32_t h = handle;
    while ( remaining > 0 )
    {
        uint8_t * destination = rn_send_store_chunk_data( store, h );
        const int chunk_bytes = (int) store->classes[rn_send_store_chunk_class( h )].chunk_bytes;
        const int take = remaining < chunk_bytes ? remaining : chunk_bytes;
        memcpy( destination, source, take );
        source += take;
        remaining -= take;
        if ( remaining > 0 )
        {
            h = rn_send_store_chunk_next( store, h );
            rocketnet_assert( h != RN_CHUNK_NONE );
        }
    }
}

// One code-0 record -- flags, u16 length, data -- at payload + offset
// (STANDARD.md _Messages_, CHANNELS.md 13.2). The form byte is reserved, so
// the record fits only when it and the trailing form byte both do. Returns the
// record's bytes, or 0 when it does not fit and the payload is untouched.

static int rn_channel_store_record( const rn_send_store_t * store, uint32_t handle, int bytes,
                                    uint8_t * payload, int capacity, int offset )
{
    const int record_bytes = ROCKETNET_MESSAGE_RECORD_FIXED_BYTES + bytes;
    if ( offset < 0 || offset + record_bytes + 1 > capacity )
        return 0;

    uint8_t * p = payload + offset;
    *p++ = (uint8_t) ROCKETNET_MESSAGE_CODE_UNRELIABLE;
    *p++ = (uint8_t) ( bytes & 0xFF );
    *p++ = (uint8_t) ( ( bytes >> 8 ) & 0xFF );
    rn_channel_store_read( store, handle, p, bytes );
    return record_bytes;
}

// -----------------------------------------
//                              the machine
// -----------------------------------------

void rn_channel_init( struct rn_channel_t * channel, rn_region_t * region, const rocketnet_message_config_t * config )
{
    rocketnet_assert( channel );
    rocketnet_assert( region );

    rocketnet_message_config_t defaults;
    if ( !config )
    {
        rocketnet_message_default_config( &defaults );
        config = &defaults;
    }

    memset( channel, 0, sizeof( *channel ) );
    channel->region = region;
    channel->limits.num_channels = config->num_channels;
    channel->limits.enabled_codes = rocketnet_message_enabled_codes( 1u << ROCKETNET_UNRELIABLE );
    channel->limits.window_messages = config->window_messages;
    channel->limits.max_message_bytes = config->max_message_bytes;
    channel->limits.receive_ring_bytes = config->receive_ring_bytes;
    channel->max_message_bytes = config->max_message_bytes;
    channel->unreliable_deliveries = config->unreliable_deliveries;
    channel->queue_head = RN_DESCRIPTOR_NONE;
    channel->queue_tail = RN_DESCRIPTOR_NONE;
    channel->queued = 0;
    channel->context.active = 0;
    channel->context.refusal = RN_CHANNEL_OK;
}

void rn_channel_reset( struct rn_channel_t * channel )
{
    rocketnet_assert( channel );

    while ( channel->queue_head != RN_DESCRIPTOR_NONE )
    {
        const uint32_t d = channel->queue_head;
        rn_send_descriptor_t * descriptor = &channel->region->send_descriptors[d];
        channel->queue_head = descriptor->next;
        rn_region_send_free( channel->region, d );
    }
    channel->queue_head = RN_DESCRIPTOR_NONE;
    channel->queue_tail = RN_DESCRIPTOR_NONE;
    channel->queued = 0;
    channel->context.active = 0;
    channel->context.refusal = RN_CHANNEL_OK;
}

int rn_channel_send( struct rn_channel_t * channel, const uint8_t * data, int bytes,
                     int reliability, int priority, int channel_index )
{
    rocketnet_assert( channel );

    if ( reliability != ROCKETNET_UNRELIABLE )
        return RN_CHANNEL_REFUSED_TYPE_NOT_ENABLED;             // this card's enabled mask is { UNRELIABLE } (7.5)
    if ( bytes < 1 || !data )
        return RN_CHANNEL_REFUSED_ARGUMENT;
    if ( bytes > channel->max_message_bytes )
        return RN_CHANNEL_REFUSED_TOO_LARGE;
    if ( channel_index < 0 || channel_index >= channel->limits.num_channels )
        return RN_CHANNEL_REFUSED_CHANNEL;

    rn_region_t * region = channel->region;
    const uint32_t d = rn_region_send_alloc( region, bytes );   // counts queue/store exhaustion (layer 1)
    if ( d == RN_DESCRIPTOR_NONE )
        return region->descriptors_free == 0 ? RN_CHANNEL_REFUSED_QUEUE_FULL : RN_CHANNEL_REFUSED_STORE_FULL;

    rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    descriptor->priority = (uint8_t) priority;
    descriptor->reliability = (uint8_t) reliability;
    rn_channel_store_write( &region->store, descriptor->chunk, data, bytes );   // R5's send_message copy

    if ( channel->queue_tail == RN_DESCRIPTOR_NONE )
    {
        channel->queue_head = d;
    }
    else
    {
        region->send_descriptors[channel->queue_tail].next = d;
    }
    descriptor->next = RN_DESCRIPTOR_NONE;
    channel->queue_tail = d;
    channel->queued++;
    return RN_CHANNEL_OK;
}

int rn_channel_next_bytes( const struct rn_channel_t * channel )
{
    rocketnet_assert( channel );
    if ( channel->queued == 0 )
        return 0;

    const uint32_t head = channel->queue_head;
    const rn_send_descriptor_t * descriptor = &channel->region->send_descriptors[head];
    const int payload = ROCKETNET_MESSAGE_RECORD_FIXED_BYTES + (int) descriptor->bytes + 1;
    if ( payload > ROCKETNET_MAX_PAYLOAD_BYTES )
        return payload;                                         // one fragmented transport payload (L2.3)

    return ROCKETNET_MAX_PAYLOAD_BYTES;                         // an aggregation frame
}

int rn_channel_write( struct rn_channel_t * channel, uint8_t * payload, int capacity, int * fragmented )
{
    rocketnet_assert( channel );
    rocketnet_assert( payload );
    rocketnet_assert( fragmented );

    *fragmented = 0;
    if ( channel->queued == 0 )
        return 0;                                               // an empty plan writes nothing (9.3)

    rn_region_t * region = channel->region;
    rn_send_descriptor_t * head = &region->send_descriptors[channel->queue_head];

    // A message whose one record does not fit a wire frame travels alone, in a
    // RECORDS payload above ROCKETNET_MAX_PAYLOAD_BYTES that the transport
    // fragments (L2.3). Losing a fragment loses the message; it is never
    // resent, which is what unreliable means (P3).
    if ( (int) head->bytes + ROCKETNET_MESSAGE_RECORD_FIXED_BYTES + 1 > ROCKETNET_MAX_PAYLOAD_BYTES )
    {
        const int record_bytes = rn_channel_store_record( &region->store, head->chunk, (int) head->bytes,
                                                          payload, capacity, 0 );
        if ( record_bytes == 0 )
            return 0;                                           // the caller sized the frame too small
        payload[record_bytes] = ROCKETNET_MESSAGE_FORM_RECORDS;

        const uint32_t d = channel->queue_head;
        channel->queue_head = head->next;
        if ( channel->queue_head == RN_DESCRIPTOR_NONE )
            channel->queue_tail = RN_DESCRIPTOR_NONE;
        channel->queued--;
        rn_region_send_free( region, d );
        *fragmented = 1;
        return record_bytes + 1;
    }

    // Small messages pack into one RECORDS frame until the budget (9.3). The
    // budget is the wire frame, or the caller's smaller frame.
    const int budget = capacity < ROCKETNET_MAX_PAYLOAD_BYTES ? capacity : ROCKETNET_MAX_PAYLOAD_BYTES;
    int offset = 0;
    while ( channel->queued > 0 )
    {
        rn_send_descriptor_t * descriptor = &region->send_descriptors[channel->queue_head];
        const int record_bytes = rn_channel_store_record( &region->store, descriptor->chunk, (int) descriptor->bytes,
                                                          payload, budget, offset );
        if ( record_bytes == 0 )
            break;                                              // this record ends the packet; it is never split

        offset += record_bytes;
        const uint32_t d = channel->queue_head;
        channel->queue_head = descriptor->next;
        if ( channel->queue_head == RN_DESCRIPTOR_NONE )
            channel->queue_tail = RN_DESCRIPTOR_NONE;
        channel->queued--;
        rn_region_send_free( region, d );
    }

    if ( offset == 0 )
        return 0;
    payload[offset] = ROCKETNET_MESSAGE_FORM_RECORDS;
    return offset + 1;
}

// The zero-copy packet-sized path (CHANNELS.md 11.2). The caller has already
// taken the transport frame; here there is only the reservation and the form
// byte, so nothing is copied. Larger messages are the copying path's, whose
// payload the transport fragments, so a request above one wire frame is
// refused rather than silently copied.

uint8_t * rn_channel_start_message( struct rn_channel_t * channel, uint8_t * payload, int capacity,
                                    int request_bytes, struct rn_channel_message_context_t * context )
{
    rocketnet_assert( channel );
    rocketnet_assert( context );

    context->refusal = RN_CHANNEL_OK;
    if ( context->active )
    {
        context->refusal = RN_CHANNEL_REFUSED_CONTEXT_OPEN;
        return NULL;
    }
    if ( !payload || request_bytes < 1 )
    {
        context->refusal = RN_CHANNEL_REFUSED_ARGUMENT;
        return NULL;
    }
    if ( request_bytes > channel->max_message_bytes )
    {
        context->refusal = RN_CHANNEL_REFUSED_TOO_LARGE;
        return NULL;
    }
    if ( request_bytes > ROCKETNET_MESSAGE_SINGLE_BYTES || request_bytes > rocketnet_message_single_capacity( capacity ) )
    {
        context->refusal = RN_CHANNEL_REFUSED_SINGLE_TOO_LARGE;
        return NULL;
    }

    context->payload = payload;
    context->capacity = capacity;
    context->request_bytes = request_bytes;
    context->active = 1;
    return payload;                                             // the transport frame's payload root, 16-aligned
}

int rn_channel_finish_message( struct rn_channel_t * channel, struct rn_channel_message_context_t * context, int message_bytes )
{
    rocketnet_assert( channel );
    rocketnet_assert( context );

    if ( !context->active )
        return 0;

    context->refusal = RN_CHANNEL_OK;
    if ( message_bytes < 1 || message_bytes > context->request_bytes )
    {
        context->refusal = RN_CHANNEL_REFUSED_TOO_LARGE;
        return 0;
    }

    const int length = rocketnet_message_single_finish( context->payload, context->capacity, message_bytes );
    if ( length == 0 )
    {
        context->refusal = RN_CHANNEL_REFUSED_SINGLE_TOO_LARGE;
        return 0;
    }

    context->active = 0;
    return length;
}

void rn_channel_abort_message( struct rn_channel_t * channel, struct rn_channel_message_context_t * context )
{
    rocketnet_assert( channel );
    rocketnet_assert( context );
    context->active = 0;
    context->refusal = RN_CHANNEL_OK;
    (void) channel;
}

// The receive step, stateless (7.2, 11.1): one payload in, every UNRELIABLE
// message out in arrival order, each pointing into the payload. Layer 2 runs
// its whole-payload checks first, so a refused payload delivers nothing.

int rn_channel_receive( const struct rn_channel_t * channel, const uint8_t * payload, int bytes,
                        rn_delivery_t * out, int out_capacity, int * out_count )
{
    rocketnet_assert( channel );
    rocketnet_assert( out_count );

    *out_count = 0;
    if ( !payload || bytes < 2 )
        return RN_CHANNEL_VIOLATION;

    struct rocketnet_message_reader_t reader;
    const int violation = rocketnet_message_reader_begin( &reader, payload, bytes, &channel->limits );
    if ( violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
        return RN_CHANNEL_VIOLATION;                            // whole payload refused, R6

    int count = 0;
    struct rocketnet_message_record_t record;
    while ( rocketnet_message_reader_next( &reader, &record ) )
    {
        if ( record.code != ROCKETNET_MESSAGE_CODE_UNRELIABLE )
            continue;                                           // enabled_codes admits code 0 only here

        if ( count >= channel->unreliable_deliveries || count >= out_capacity )
        {
            *out_count = count;                                 // the rest are dropped; unreliable semantics allow it
            return RN_CHANNEL_REFUSED_DELIVERY_FULL;
        }

        rn_delivery_t * delivery = &out[count];
        memset( delivery, 0, sizeof( *delivery ) );
        delivery->data = record.data;
        delivery->bytes = record.bytes;
        delivery->channel = 0;                                  // code 0 carries no channel on the wire
        delivery->code = (uint8_t) ROCKETNET_MESSAGE_CODE_UNRELIABLE;
        delivery->serial = 0;
        count++;
    }

    *out_count = count;
    return RN_CHANNEL_OK;
}
