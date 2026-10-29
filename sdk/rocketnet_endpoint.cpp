/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The endpoint (docs/CHANNELS.md 10.1): one connection's layers 3 to 6 as a
// pure machine. The spec is proofs/tla/RNEndpoint.tla; each function names the action
// it implements. The per-connection orchestration here is what the message
// objects did for each connection (sdk/rocketnet_message_server.cpp), moved
// whole: the same calls into the layers, the same counters, in the same order.
//
// Purity (16.3): time comes in as `now`; the transport is reached by nobody
// here (the caller starts, finishes and aborts frames); the only memory is the
// region the endpoint was bound to.

#include <string.h>

#include "rocketnet_internal.h"
#include "rocketnet_allocator.h"
#include "rocketnet_channel.h"
#include "rocketnet_counters.h"
#include "rocketnet_endpoint.h"
#include "rocketnet_fragment.h"
#include "rocketnet_message.h"
#include "rocketnet_packet_builder.h"
#include "rocketnet_platform.h"
#include "rocketnet_send_budget.h"

static void rn_endpoint_count( rn_endpoint_t * endpoint, int counter, uint64_t amount = 1 )
{
    // the owner's message counters, which the region was carved with

    rocketnet_counters_t * counters = endpoint->region->counters;
    if ( counters )
        rocketnet_platform_atomic_add_u64( (volatile uint64_t *) &counters->values[counter], amount );
}

static bool rn_unreliable_member( const rn_send_descriptor_t * descriptor )
{
    return descriptor->in_use &&
           ( descriptor->reliability == ROCKETNET_RELIABILITY_UNRELIABLE ||
             descriptor->reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ||
             descriptor->reliability == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT ) &&
           rn_descriptor_schedule_live( descriptor );
}

// The unreliable FIFO is a ring of descriptor entries in push order
// (proofs/tla/RNIndexRing.tla): an entry whose descriptor is no longer queued under
// the ticket it was pushed with is a tombstone. The head skips tombstones,
// and a push that finds the ring full drops them all in one pass, at most
// once per queue-many pushes.
//
// proofs/tla/RNChannelQueues.tla carries the same queue with an epoch per entry: an
// entry's ticket is its epoch. EnqueueUnreliable is rn_unreliable_push, the
// tail entry stamped with the descriptor's newest ticket. StaleEpoch is the
// descriptor taking another ticket or leaving the queue, so the entry's ticket
// is no longer current (rn_unreliable_entry_live). Expire is the entries of a
// stale ticket dropped with the rest kept in order: the head skipping them in
// rn_unreliable_sync, the full-ring pass in rn_unreliable_push, and every walk
// passing them by.

static bool rn_unreliable_entry_live( const rn_region_t * region, const rn_unreliable_entry_t * entry )
{
    const rn_send_descriptor_t * descriptor = &region->send_descriptors[entry->descriptor];
    return rn_unreliable_member( descriptor ) && descriptor->enqueue_ticket == entry->ticket;
}

static const rn_unreliable_entry_t * rn_unreliable_entry( const rn_endpoint_t * endpoint, uint32_t position )
{
    const rn_region_t * region = endpoint->region;
    return &region->unreliable_ring[( endpoint->unreliable_ring_head + position ) % region->unreliable_ring_entries];
}

// A walk over the entries from the head as it stood when the walk began:
// removals during the walk turn entries into tombstones and may move the
// head past them, but no entry moves, and nothing is pushed mid-walk.

struct rn_unreliable_walk_t
{
    uint32_t base;
    uint32_t span;
    uint32_t step;
};

static void rn_unreliable_walk_begin( const rn_endpoint_t * endpoint, rn_unreliable_walk_t * walk )
{
    walk->base = endpoint->unreliable_ring_head;
    walk->span = endpoint->unreliable_ring_used;
    walk->step = 0;
}

// the next live descriptor of the walk, RN_DESCRIPTOR_NONE at its end
static uint32_t rn_unreliable_walk_next( const rn_endpoint_t * endpoint, rn_unreliable_walk_t * walk )
{
    const rn_region_t * region = endpoint->region;
    while ( walk->step < walk->span )
    {
        const rn_unreliable_entry_t * entry = &region->unreliable_ring[( walk->base + walk->step ) % region->unreliable_ring_entries];
        walk->step++;
        if ( rn_unreliable_entry_live( region, entry ) )
            return entry->descriptor;
    }

    return RN_DESCRIPTOR_NONE;
}

static void rn_unreliable_sync( rn_endpoint_t * endpoint )
{
    const rn_region_t * region = endpoint->region;
    while ( endpoint->unreliable_ring_used > 0 && !rn_unreliable_entry_live( region, rn_unreliable_entry( endpoint, 0 ) ) )
    {
        endpoint->unreliable_ring_head = ( endpoint->unreliable_ring_head + 1 ) % region->unreliable_ring_entries;
        endpoint->unreliable_ring_used--;
    }

    endpoint->unreliable_head = endpoint->unreliable_ring_used > 0 ? rn_unreliable_entry( endpoint, 0 )->descriptor : RN_DESCRIPTOR_NONE;
}

static void rn_unreliable_push( rn_endpoint_t * endpoint, uint32_t d )
{
    rn_region_t * region = endpoint->region;
    const uint32_t entries = region->unreliable_ring_entries;

    // the ring has two entries per send descriptor (rn_region_carve) and d is
    // one of them, so it is never empty

    rocketnet_assume( entries >= 2 );
    rocketnet_assume( d < entries / 2 );

    if ( endpoint->unreliable_ring_used == entries )
    {
        // at most queue entries are live, so the pass frees at least half.
        // Survivors close up from the head, each written at its own logical
        // position: that is never ahead of the entry being read, so a wrapped
        // ring's unread entries are never overwritten.
        uint32_t kept = 0;
        for ( uint32_t i = 0; i < endpoint->unreliable_ring_used; i++ )
        {
            const rn_unreliable_entry_t entry = *rn_unreliable_entry( endpoint, i );
            if ( rn_unreliable_entry_live( region, &entry ) )
                region->unreliable_ring[( endpoint->unreliable_ring_head + kept++ ) % entries] = entry;
        }
        endpoint->unreliable_ring_used = kept;
    }

    rn_unreliable_entry_t * tail = &region->unreliable_ring[( endpoint->unreliable_ring_head + endpoint->unreliable_ring_used ) % entries];
    tail->descriptor = d;
    tail->ticket = region->send_descriptors[d].enqueue_ticket;
    endpoint->unreliable_ring_used++;
    rn_unreliable_sync( endpoint );
}

// the public promotion threshold is the record rule at the full payload capacity
static_assert( ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES == ROCKETNET_MESSAGE_SINGLE_BYTES - ROCKETNET_MESSAGE_RECORD_FIXED_BYTES - 1,
               "ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES is the largest unreliable message one packet carries" );

static bool rn_endpoint_unreliable_over_record_capacity( int reliability, int channel, int bytes, int capacity )
{
    // every unreliable mode uses the same record-size rule, including its wire header and form byte

    const int header = reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ? 3 + 3 + 3 + ( channel >= 256 ? 2 : 1 ) : ROCKETNET_MESSAGE_RECORD_FIXED_BYTES;
    const int one_packet_capacity = reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ? capacity : ( capacity < ROCKETNET_MESSAGE_SINGLE_BYTES ? capacity : ROCKETNET_MESSAGE_SINGLE_BYTES );

    return (uint64_t) (uint32_t) bytes + (uint64_t) header + 1u > (uint64_t) one_packet_capacity;
}

// ---------------------------------------------------------------
//                     init and reset
// ---------------------------------------------------------------

void rn_endpoint_init( struct rn_endpoint_t * endpoint, rn_region_t * region, const struct rn_endpoint_config_t * config )
{
    // 10.1 init: bound to its region and configuration, nothing allocated

    endpoint->region = region;
    endpoint->config = config->config;
    endpoint->default_kbps = config->send_kbps;
    endpoint->send_budget_enabled = config->send_kbps > 0 ? 1 : 0;
    endpoint->unreliable_limit = ROCKETNET_MESSAGE_SINGLE_BYTES - 1 - ROCKETNET_MESSAGE_RECORD_FIXED_BYTES;
    endpoint->max_payload_bytes = config->max_payload_bytes > 0 ? config->max_payload_bytes : ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    endpoint->send_error = ROCKETNET_DISCONNECT_REASON_NONE;
    endpoint->config_wait_start = -1.0;
    endpoint->config_error = ROCKETNET_DISCONNECT_REASON_NONE;
    endpoint->pending_type = -1;
    endpoint->pending_in_place = 0;
    endpoint->write_phase = RN_ENDPOINT_WRITE_IDLE;
    endpoint->packet_kind = RN_ENDPOINT_PACKET_NONE;
    endpoint->packet_written = 0;
}

void rn_endpoint_reset( struct rn_endpoint_t * endpoint, double now )
{
    // proofs/tla/RNEndpoint.tla Reset (RNSlots ResetFree, RNConnectionMemory Reuse):
    // the region carved again and every layer bound to it fresh

    rn_region_reset( endpoint->region );

    // R6 (docs/CHANNELS.md 11.5): a config value of 0 or lower disables this
    // connection's budget. The endpoint then holds no bucket and passes no
    // budget to its builders, whose NULL budget path never drops or waits.

    rocketnet_send_budget_t * budget = NULL;
    if ( endpoint->default_kbps > 0 )
    {
        rocketnet_send_budget_init( &endpoint->budget, (uint32_t) endpoint->default_kbps, now );
        endpoint->send_budget_enabled = 1;
        budget = &endpoint->budget;
    }
    else
    {
        memset( &endpoint->budget, 0, sizeof( endpoint->budget ) );
        endpoint->send_budget_enabled = 0;
    }

    const rocketnet_message_config_t * config = endpoint->config;
    const int max_packets = config->packets_per_update < ROCKETNET_PACKET_BUILDER_MAX_PACKETS ? config->packets_per_update : ROCKETNET_PACKET_BUILDER_MAX_PACKETS;

    rn_ordered_init( &endpoint->ordered, endpoint->region, config, budget, max_packets, ROCKETNET_MESSAGE_SINGLE_BYTES );
    rn_packet_builder_init( &endpoint->unreliable_builder, endpoint->region, NULL, budget, 1, ROCKETNET_MESSAGE_SINGLE_BYTES );

    endpoint->send_error = ROCKETNET_DISCONNECT_REASON_NONE;
    endpoint->config_wait_start = -1.0;
    endpoint->config_error = ROCKETNET_DISCONNECT_REASON_NONE;
    endpoint->serial = 0;
    endpoint->pending_type = -1;
    endpoint->pending_descriptor = RN_DESCRIPTOR_NONE;
    endpoint->pending_bytes = 0;
    endpoint->pending_in_place = 0;
    endpoint->pending_channel = 0;
    endpoint->pending_header = 0;
    endpoint->in_place_serial = 0;
    endpoint->in_place_receipt = 0;
    endpoint->in_place_admitted = 0;
    endpoint->unreliable_head = RN_DESCRIPTOR_NONE;
    endpoint->unreliable_ring_head = 0;
    endpoint->unreliable_ring_used = 0;
    endpoint->unreliable_count = 0;

    endpoint->write_phase = RN_ENDPOINT_WRITE_IDLE;
    endpoint->written = 0;
    endpoint->packet_kind = RN_ENDPOINT_PACKET_NONE;
    endpoint->packet_written = 0;
    endpoint->large_bytes = 0;
    endpoint->large_admitted = 0;
    endpoint->large_descriptor = RN_DESCRIPTOR_NONE;
    endpoint->packet_records = 0;
    endpoint->receipt_count = 0;
    memset( endpoint->receipt_serials, 0, sizeof( endpoint->receipt_serials ) );
}

void rn_endpoint_announce_config( struct rn_endpoint_t * endpoint, double now )
{
    // proofs/tla/RNEndpoint.tla Reset, the CONFIG half (10.3): a connection that
    // becomes connected (the server's Observe reset, the client's connect at
    // create) queues this end's CONFIG before anything else, at the head of
    // IMMEDIATE on channel 0, so it takes the connection's first reliable
    // number (rn_ordered_announce_config). No room is region exhaustion, and
    // rn_endpoint_error then ends the connection (R5).

    (void) now;
    rn_ordered_announce_config( &endpoint->ordered, endpoint->config );
}

// ---------------------------------------------------------------
//                          send
// ---------------------------------------------------------------

static bool rn_endpoint_reliable_path( const rn_endpoint_t * endpoint, int type )
{
    (void) endpoint;
    return type == ROCKETNET_RELIABILITY_RELIABLE || type == ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
           type == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED ||
           rn_channels_reliable_receipt_base( type ) >= 0;
}

static bool rn_endpoint_unreliable_type( int reliability )
{
    return reliability == ROCKETNET_RELIABILITY_UNRELIABLE || reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ||
           reliability == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT;
}

static int rn_endpoint_transport_max( int transport_max_payload_bytes )
{
    return transport_max_payload_bytes > 0 ? transport_max_payload_bytes : ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
}

// The route a start message takes to its room, decided once for
// rn_endpoint_start and for the room query (rn_endpoint_send_room), so the
// two never disagree. Pure: the start counts each refusal itself.
#define RN_ENDPOINT_ROUTE_REFUSED       0   // a start pending, a bad channel or size: MESSAGE_SEND_REFUSED
#define RN_ENDPOINT_ROUTE_TYPE          1   // no such type, or one outside the enabled mask (7.5)
#define RN_ENDPOINT_ROUTE_OVER_MAX      2   // an unreliable message over max_message_bytes: MESSAGE_UNRELIABLE_SEND_FAILED
#define RN_ENDPOINT_ROUTE_CHANNELS      3   // the reliable path, rn_channels_start with *out_type
#define RN_ENDPOINT_ROUTE_UNRELIABLE    4   // the unreliable queue, region room for the bytes

static int rn_endpoint_send_route( const rn_endpoint_t * endpoint, int channel, int reliability, int bytes, int transport_max_payload_bytes, int * out_type )
{
    *out_type = reliability;

    if ( endpoint->pending_type != -1 || channel < 0 || channel >= endpoint->config->num_channels || bytes < 1 )
        return RN_ENDPOINT_ROUTE_REFUSED;

    if ( rn_endpoint_reliable_path( endpoint, reliability ) )
        return RN_ENDPOINT_ROUTE_CHANNELS;

    if ( !rn_endpoint_unreliable_type( reliability ) ||
         rn_channels_send_allowed( &endpoint->ordered.channels, reliability, (uint32_t) channel ) != ROCKETNET_CHANNEL_SEND_OK )
        return RN_ENDPOINT_ROUTE_TYPE;

    // one size rule refuses only the configured maximum; larger records promote to the reliable twin

    if ( (uint32_t) bytes > (uint32_t) endpoint->config->max_message_bytes )
        return RN_ENDPOINT_ROUTE_OVER_MAX;

    // one packet's record lives in a payload no larger than both the
    // transport's cap and the message tier's one-packet payload.

    const int transport_max = rn_endpoint_transport_max( transport_max_payload_bytes );
    const int record_capacity = transport_max < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ? transport_max : ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    // reject max+1 before the shared record-size comparison, which uses widened arithmetic

    if ( rn_endpoint_unreliable_over_record_capacity( reliability, channel, bytes, record_capacity ) )
    {
        // the promotion rule: code 0 to 2, code 1 to 4, and
        // code 5 to its reliable receipt twin (wire code 2, receipt kept).

        *out_type = reliability == ROCKETNET_RELIABILITY_UNRELIABLE ? ROCKETNET_RELIABILITY_RELIABLE : reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ? ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED
                                                                                                                                                                 : ROCKETNET_RELIABILITY_RELIABLE;
        return RN_ENDPOINT_ROUTE_CHANNELS;
    }

    return RN_ENDPOINT_ROUTE_UNRELIABLE;
}

uint8_t * rn_endpoint_start( struct rn_endpoint_t * endpoint, int channel, int reliability, int bytes, int transport_max_payload_bytes )
{
    // proofs/tla/RNEndpoint.tla Send, its first half (RNChannel Enqueue's first
    // half): room in the region for a message of `bytes` bytes, and the
    // pointer it is written at, once, in place (R3). A refusal for want of
    // room sets send_error (R5); the owner ends the connection with it.
    // proofs/tla/RNOutgoingBuild.tla StartQueued: the start on the queued path, the
    // message handed its region record rather than a transport frame.

    // every refusal of the caller's arguments is logged and counted
    // (rocketnet.h, "These errors are not API misuse"); a refusal for want of
    // room is the connection's end, not the caller's error

    int type = reliability;
    const int route = rn_endpoint_send_route( endpoint, channel, reliability, bytes, transport_max_payload_bytes, &type );
    if ( route == RN_ENDPOINT_ROUTE_REFUSED )
    {
        if ( endpoint->pending_type != -1 )
            rocketnet_error( "message start refused: a start is already pending; finish or abort it first" );
        else
            rocketnet_error( "message start refused: channel %d is outside 0 .. %d, or max_bytes %d is below 1", channel, endpoint->config->num_channels - 1, bytes );
        rn_endpoint_count( endpoint, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
        return NULL;
    }

    if ( route == RN_ENDPOINT_ROUTE_TYPE )
    {
        // a type outside the enabled mask is refused and counted (7.5)

        rocketnet_error( "message start refused: reliability %d is not a reliability this connection sends on channel %d", reliability, channel );
        if ( reliability >= 0 && reliability < ROCKETNET_CHANNEL_NUM_TYPES )
            rn_channels_send_check( &endpoint->ordered.channels, reliability, (uint32_t) channel );
        else
            rn_endpoint_count( endpoint, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
        return NULL;
    }

    // keep an unreliable reservation's transport cap so finish applies the
    // record-size rule to the actual byte count, even when the caller
    // finishes short.

    if ( rn_endpoint_unreliable_type( reliability ) )
        endpoint->max_payload_bytes = rn_endpoint_transport_max( transport_max_payload_bytes );

    if ( route == RN_ENDPOINT_ROUTE_OVER_MAX )
    {
        rocketnet_error( "message start refused: max_bytes %d is above max_message_bytes %d", bytes, endpoint->config->max_message_bytes );
        rn_endpoint_count( endpoint, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
        return NULL;
    }

    rn_region_t * region = endpoint->region;
    uint8_t * pointer = NULL;

    if ( route == RN_ENDPOINT_ROUTE_CHANNELS )
    {
        pointer = rn_channels_start( &endpoint->ordered.channels, (uint32_t) channel, type, (uint32_t) bytes );
        if ( !pointer )
        {
            if ( rn_region_exhausted( region ) )
                endpoint->send_error = ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED;     // R5
            else
            {
                rocketnet_error( "message start refused: reliability %d on channel %d with max_bytes %d (max_message_bytes %d)", reliability, channel, bytes, endpoint->config->max_message_bytes );
                rn_endpoint_count( endpoint, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
            }
            return NULL;
        }

        endpoint->pending_descriptor = endpoint->ordered.channels.pending;
    }
    else if ( rn_region_exhausted( region ) )
    {
        endpoint->send_error = ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED;
        return NULL;
    }
    else
    {
        const uint32_t d = rn_region_send_take( region, bytes );
        if ( d == RN_DESCRIPTOR_NONE )
        {
            // proofs/tla/RNChannelTypes.tla MemoryExhausted: the send region has no room left, so the connection ends.
            // The region has no room for a send it must not drop.

            endpoint->send_error = ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED;
            return NULL;
        }
        rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
        descriptor->channel = (uint16_t) channel;
        descriptor->reliability = (uint8_t) reliability;
        descriptor->priority = ROCKETNET_PRIORITY_MEDIUM;
        descriptor->fragments = 1;
        endpoint->pending_descriptor = d;
        pointer = rn_region_send_write_pointer( region, d );
    }

    endpoint->pending_type = reliability;
    endpoint->pending_bytes = (uint32_t) bytes;

    return pointer;
}

bool rn_endpoint_send_room( const struct rn_endpoint_t * endpoint, int channel, int reliability, int bytes, int transport_max_payload_bytes )
{
    // the room query: rn_endpoint_start's route, then the region's decision
    // (rn_region_send_fit) that its allocation acts on. Pure, O(1).

    int type = reliability;
    const int route = rn_endpoint_send_route( endpoint, channel, reliability, bytes, transport_max_payload_bytes, &type );
    if ( route == RN_ENDPOINT_ROUTE_CHANNELS )
    {
        // rn_channels_start's refusals ahead of the room: a start pending, a
        // type outside the mask (a promotion's twin included)

        const rn_channels_t * channels = &endpoint->ordered.channels;
        if ( channels->pending != RN_DESCRIPTOR_NONE ||
             rn_channels_send_allowed( channels, type, (uint32_t) channel ) != ROCKETNET_CHANNEL_SEND_OK )
            return false;
    }
    else if ( route != RN_ENDPOINT_ROUTE_UNRELIABLE )
    {
        return false;
    }

    return rn_region_send_fit( endpoint->region, bytes ) == RN_SEND_FIT_OK;
}

uint32_t rn_endpoint_finish( struct rn_endpoint_t * endpoint, int bytes, uint32_t serial )
{
    // proofs/tla/RNEndpoint.tla Send, its second half: the
    // message joins its channel's queue in FIFO order and takes its receipt
    // serial (7.4: nonzero for every send). 0 leaves the start pending.
    // proofs/tla/RNReceipts.tla Send: issues the next receipt serial before any packet write.
    // proofs/tla/RNOutgoingBuild.tla FinishQueued: the region record's message is
    // appended to the queue the write pass copies from.

    if ( !endpoint || endpoint->pending_type == -1 || endpoint->pending_in_place )
    {
        rocketnet_assert( endpoint != NULL && endpoint->pending_type != -1 && !endpoint->pending_in_place );
        return 0;                                               // an in-place start finishes with rn_endpoint_in_place_finish
    }

    if ( bytes < 1 || (uint32_t) bytes > endpoint->pending_bytes )
        return 0;

    if ( serial == 0 )
    {
        serial = endpoint->serial + 1;
        if ( serial == 0 )
            serial = 1;
    }

    // start reserves for its requested capacity; finish decides promotion
    // from the actual byte count and can downgrade a short reservation.

    const bool reserved_reliable = endpoint->ordered.channels.pending != RN_DESCRIPTOR_NONE;
    const uint32_t channel = reserved_reliable ? endpoint->ordered.channels.pending_channel : endpoint->region->send_descriptors[endpoint->pending_descriptor].channel;
    const int transport_max = endpoint->max_payload_bytes > 0 ? endpoint->max_payload_bytes : ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    const int record_capacity = transport_max < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ? transport_max : ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    // finish applies the same record-size rule to the bytes actually committed

    const bool actual_over_record = rn_endpoint_unreliable_over_record_capacity( endpoint->pending_type, (int) channel, bytes, record_capacity );
    const bool unreliable_source = endpoint->pending_type == ROCKETNET_RELIABILITY_UNRELIABLE ||
                                   endpoint->pending_type == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ||
                                   endpoint->pending_type == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT;
    const bool promoted = reserved_reliable && unreliable_source && actual_over_record;
    if ( rn_endpoint_reliable_path( endpoint, endpoint->pending_type ) || promoted )
    {
        const uint32_t d = endpoint->ordered.channels.pending;
        if ( !rn_channels_finish( &endpoint->ordered.channels, (uint32_t) bytes ) )
            return 0;

        rn_send_descriptor_t * descriptor = &endpoint->region->send_descriptors[d];
        descriptor->serial = serial;
        if ( endpoint->pending_type == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT )
            descriptor->flags |= RN_SEND_FLAG_RECEIPT_PROMOTED; // RNReceipts EffectiveKind: reliable completion, no LOSS

        if ( promoted )
        {
            // promotion: every promotion is counted, once per send

            const int counter = endpoint->pending_type == ROCKETNET_RELIABILITY_UNRELIABLE ? ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE : endpoint->pending_type == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ? ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED
                                                                                                                                                                                                                  : ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_WITH_ACK_RECEIPT;
            rn_endpoint_count( endpoint, counter );
        }
    }
    else
    {
        rn_region_t * region = endpoint->region;
        const uint32_t d = endpoint->pending_descriptor;
        rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
        if ( endpoint->ordered.channels.pending == d )
        {
            // a caller may reserve a block and finish fewer bytes. Move the
            // same region descriptor to the single-packet route without a copy.

            rocketnet_assert( endpoint->pending_type == ROCKETNET_RELIABILITY_UNRELIABLE ||
                              endpoint->pending_type == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ||
                              endpoint->pending_type == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT );

            const uint32_t pending_channel = endpoint->ordered.channels.pending_channel;
            endpoint->ordered.channels.pending = RN_DESCRIPTOR_NONE;
            descriptor->channel = (uint16_t) pending_channel;
            descriptor->reliability = (uint8_t) endpoint->pending_type;
            descriptor->priority = ROCKETNET_PRIORITY_MEDIUM;
            descriptor->fragments = 1;
        }

        descriptor->bytes = (uint32_t) bytes;
        descriptor->serial = serial;
        rn_descriptor_enqueue_stamp( region, descriptor );

        if ( endpoint->pending_type == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED )
        {
            // RNChannelTypes!Enqueue: a sequenced send stays in the unreliable
            // queue and takes no reliable number or resend entry.

            descriptor->sequencing = rn_channels_send_sequenced( &endpoint->ordered.channels, descriptor->channel, &descriptor->ordering );
        }

        endpoint->unreliable_count++;
        rn_unreliable_push( endpoint, d );
    }

    endpoint->serial = serial;
    endpoint->pending_type = -1;
    endpoint->pending_descriptor = RN_DESCRIPTOR_NONE;
    endpoint->pending_bytes = 0;

    return serial;
}

void rn_endpoint_abort( struct rn_endpoint_t * endpoint )
{
    // proofs/tla/RNEndpoint.tla Send, undone before its second half (RNChannel
    // Enqueue's first half returned): a start that will not be finished, its
    // room returned to the region

    if ( rn_endpoint_reliable_path( endpoint, endpoint->pending_type ) ||
         endpoint->ordered.channels.pending != RN_DESCRIPTOR_NONE )
        rn_channels_cancel( &endpoint->ordered.channels );
    else if ( ( endpoint->pending_type == ROCKETNET_RELIABILITY_UNRELIABLE || endpoint->pending_type == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ||
                endpoint->pending_type == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT ) &&
              endpoint->pending_descriptor != RN_DESCRIPTOR_NONE )
        rn_region_send_free( endpoint->region, endpoint->pending_descriptor );

    endpoint->pending_type = -1;
    endpoint->pending_descriptor = RN_DESCRIPTOR_NONE;
    endpoint->pending_bytes = 0;
    endpoint->pending_in_place = 0;                             // RNOutgoingBuild Abort: the owner aborts the frame
}

// ---------------------------------------------------------------
//                     the in-place unreliable send
// ---------------------------------------------------------------

// docs/OUTGOING-BUILD.md; proofs/tla/RNOutgoingBuild.tla StartInPlace (header and
// affordable decide it: a record appended at the open build's committed end,
// or a frame opened for it), FinishInPlace, Abort, and the over-budget drop
// of a seal (SealBuild, BeginFlush, BeginUpdate). The payload is the frame's:
// the header goes in front of the bytes the game wrote and the form byte
// after them, in place, so the message layer copies nothing into the
// transport. rn_endpoint_in_place_finish is the whole-frame case, a record
// that fills its frame and is sealed at once.

static bool rn_endpoint_ordered_due( const rn_endpoint_t * endpoint, double now );

static int rn_endpoint_sequenced_header_bytes( int channel )
{
    return 3 + 3 + 3 + ( channel >= 256 ? 2 : 1 );              // flags and length, sequencing, ordering, channel
}

int rn_endpoint_in_place_header( const struct rn_endpoint_t * endpoint, int channel, int reliability, int bytes, int transport_max_payload_bytes, double now )
{
    if ( endpoint->pending_type != -1 || channel < 0 || channel >= endpoint->config->num_channels || bytes < 1 )
        return RN_ENDPOINT_IN_PLACE_NO;
    if ( reliability != ROCKETNET_RELIABILITY_UNRELIABLE && reliability != ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED &&
         reliability != ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT )
        return RN_ENDPOINT_IN_PLACE_NO;
    if ( ( endpoint->ordered.channels.enabled_types & ( 1u << reliability ) ) == 0 || rn_region_exhausted( endpoint->region ) )
        return RN_ENDPOINT_IN_PLACE_NO;                         // the region path refuses and counts it

    int header;
    if ( reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED )
    {
        header = rn_endpoint_sequenced_header_bytes( channel );

        const int transport_max = transport_max_payload_bytes > 0 ? transport_max_payload_bytes : ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
        if ( bytes > endpoint->config->max_message_bytes || header + bytes + 1 > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ||
             header + bytes + 1 > transport_max )
            return RN_ENDPOINT_IN_PLACE_NO;                     // the large single-record path
    }
    else
    {
        header = reliability == ROCKETNET_RELIABILITY_UNRELIABLE ? 0 : ROCKETNET_MESSAGE_RECORD_FIXED_BYTES;

        if ( (uint32_t) bytes > endpoint->unreliable_limit )
            return RN_ENDPOINT_IN_PLACE_NO;                     // refused, or the receipt twin's promotion
    }

    // RNOutgoingBuild StartInPlace: a message never overtakes an unreliable
    // one queued before it (including a sequenced send promoted to the
    // channel queue); direct and queued packets share the update cap, and
    // while reliable work is due (the write pass's reliable phase would send
    // a packet) one packet of it stays for that phase
    // (RNOutgoingBuild ReliableSentWithinOneUpdate)

    const int reserved = rn_endpoint_ordered_due( endpoint, now ) ? 1 : 0;
    if ( endpoint->unreliable_count > 0 || endpoint->written + reserved >= endpoint->config->packets_per_update ||
         ( reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED &&
           ( endpoint->region->channel_send[channel].queue_head != RN_DESCRIPTOR_NONE ||
             endpoint->ordered.ack.chunk_head != RN_RESEND_NONE ) ) )
        return RN_ENDPOINT_IN_PLACE_QUEUED;

    return header;
}

bool rn_endpoint_in_place_affordable( struct rn_endpoint_t * endpoint, int payload_bytes, double now )
{
    if ( !endpoint->send_budget_enabled )
        return true;                                            // R6: uncapped, no bucket

    // the refill the next write pass would make at this update time, made now
    // (a second refill at the same time adds nothing)

    rocketnet_send_budget_refill( &endpoint->budget, now );

    const int admitted = rocketnet_send_budget_max_wire_bytes( payload_bytes );
    const double reserve = rn_endpoint_ordered_due( endpoint, now ) ? (double) ROCKETNET_SEND_BUDGET_MAX_PACKET_WIRE_BYTES : 0.0;

    return admitted > 0 && !endpoint->budget.reliable_waiting && endpoint->budget.tokens >= (double) admitted + reserve;
}

void rn_endpoint_in_place_begin( struct rn_endpoint_t * endpoint, int channel, int reliability, int bytes, int header )
{
    rocketnet_assert( endpoint->pending_type == -1 );
    endpoint->pending_type = reliability;
    endpoint->pending_bytes = (uint32_t) bytes;
    endpoint->pending_descriptor = RN_DESCRIPTOR_NONE;
    endpoint->pending_in_place = 1;
    endpoint->pending_channel = channel;
    endpoint->pending_header = header;
}

uint32_t rn_endpoint_in_place_finish( struct rn_endpoint_t * endpoint, uint8_t * payload, int bytes, double now, int * payload_bytes )
{
    if ( payload_bytes )
        *payload_bytes = 0;

    if ( !endpoint || endpoint->pending_type == -1 || !endpoint->pending_in_place || !payload || !payload_bytes )
    {
        rocketnet_assert( endpoint != NULL && endpoint->pending_type != -1 && endpoint->pending_in_place && payload != NULL && payload_bytes != NULL );
        return 0;
    }

    if ( bytes < 1 || (uint32_t) bytes > endpoint->pending_bytes )
        return 0;

    uint32_t serial = endpoint->serial + 1;
    if ( serial == 0 )
        serial = 1;

    const int type = endpoint->pending_type;
    const int header = endpoint->pending_header;
    const int capacity = header + (int) endpoint->pending_bytes + 1;

    int length = 0;
    if ( type == ROCKETNET_RELIABILITY_UNRELIABLE )
    {
        // the SINGLE form: the message at the payload root, the form byte after it

        length = rocketnet_message_single_finish( payload, capacity, bytes );
    }
    else
    {
        // one record of a RECORDS payload: its header in front of the bytes

        rocketnet_message_record_t record;
        memset( &record, 0, sizeof( record ) );

        if ( type == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED )
        {
            // RNChannelTypes!Enqueue: the sequencing index is taken at finish,
            // as on the queued path, so the sequenced drop rule is unchanged

            uint32_t ordering = 0;
            const uint32_t sequencing = rn_channels_send_sequenced( &endpoint->ordered.channels, (uint32_t) endpoint->pending_channel, &ordering );
            record.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED;
            record.channel = (uint32_t) endpoint->pending_channel;
            record.sequencing = sequencing & ROCKETNET_MESSAGE_U24_MASK;
            record.ordering = ordering & ROCKETNET_MESSAGE_U24_MASK;
        }
        else
            record.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE;   // 5 travels as code 0 (STANDARD.md _Record_)

        record.data = payload + header;
        record.bytes = bytes;

        rocketnet_message_writer_t writer;
        rocketnet_message_writer_begin( &writer, payload, capacity );

        if ( rocketnet_message_writer_add_in_place( &writer, &record ) )
            length = rocketnet_message_writer_finish( &writer );
    }

    rocketnet_assert( length > 0 );

    endpoint->serial = serial;
    endpoint->pending_type = -1;
    endpoint->pending_descriptor = RN_DESCRIPTOR_NONE;
    endpoint->pending_bytes = 0;
    endpoint->pending_in_place = 0;
    endpoint->in_place_serial = serial;
    endpoint->in_place_receipt = type == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT ? 1 : 0;
    endpoint->in_place_admitted = 0;

    if ( length <= 0 )
    {
        rn_endpoint_in_place_failed( endpoint );
        return serial;
    }

    // R6, RNSendBudget Send at the unreliable class. start took this path
    // only when the bucket could pay the declared maximum
    // (rn_endpoint_in_place_affordable), and the bytes written are at most
    // that, so the admit succeeds; RNOutgoingBuild's seal with its drop
    // (SealBuild) is the drop, counted, should the bucket have moved since. The class
    // order holds: while the reliable path has work due, an in-place packet
    // is admitted only if one maximum packet stays in the bucket for it

    if ( endpoint->send_budget_enabled )
    {
        rocketnet_send_budget_refill( &endpoint->budget, now );

        const int admitted = rocketnet_send_budget_max_wire_bytes( length );
        const double reserve = rn_endpoint_ordered_due( endpoint, now ) ? (double) ROCKETNET_SEND_BUDGET_MAX_PACKET_WIRE_BYTES : 0.0;
        if ( admitted == 0 || endpoint->budget.tokens < (double) admitted + reserve ||
             rocketnet_send_budget_admit( &endpoint->budget, ROCKETNET_SEND_CLASS_UNRELIABLE, admitted ) != ROCKETNET_SEND_BUDGET_SEND )
        {
            rn_endpoint_count( endpoint, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
            if ( endpoint->in_place_receipt )
                rn_window_ack_receipt_emit_loss( &endpoint->ordered.ack, serial ); // local budget drop before wire send
            endpoint->in_place_receipt = 0;
            return serial;
        }

        endpoint->in_place_admitted = admitted;
    }

    *payload_bytes = length;
    return serial;
}

void rn_endpoint_in_place_sent( struct rn_endpoint_t * endpoint, uint64_t packet_id, int wire_bytes, double now )
{
    // proofs/tla/RNOutgoingBuild.tla TransportSend: the commit of an in-place
    // packet the transport sent, the receipt entry under the transport's
    // packet id (RNReceipts WritePacket), the budget settled at the sealed
    // size

    if ( endpoint->in_place_receipt )
        rn_window_ack_commit_receipt_packet( &endpoint->ordered.ack, packet_id, &endpoint->in_place_serial, 1, now );

    if ( endpoint->send_budget_enabled && endpoint->in_place_admitted > 0 )
        rocketnet_send_budget_settle( &endpoint->budget, endpoint->in_place_admitted, wire_bytes );

    endpoint->written++;
    endpoint->in_place_receipt = 0;
    endpoint->in_place_admitted = 0;
}

void rn_endpoint_in_place_failed( struct rn_endpoint_t * endpoint )
{
    // 6.4 "Failed send": the transport refused the frame, so the message is
    // lost, counted, and a receipt takes its LOSS

    rn_endpoint_count( endpoint, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );

    if ( endpoint->in_place_receipt )
        rn_window_ack_receipt_emit_loss( &endpoint->ordered.ack, endpoint->in_place_serial );

    if ( endpoint->send_budget_enabled && endpoint->in_place_admitted > 0 )
        rocketnet_send_budget_settle( &endpoint->budget, endpoint->in_place_admitted, 0 );    // proofs/tla/RNSendBudget.tla SendFailed: refund the whole charge

    endpoint->in_place_receipt = 0;
    endpoint->in_place_admitted = 0;
    endpoint->written++;
}

uint32_t rn_endpoint_send( struct rn_endpoint_t * endpoint, const uint8_t * data, int bytes,
                           int reliability, int priority, int channel, uint32_t serial )
{
    // proofs/tla/RNEndpoint.tla Send (10.1): start, the one copy, finish. Per-send
    // priority (9.3): MEDIUM is the one level a send takes, and any other is
    // refused. proofs/tla/RNOutgoingBuild.tla Write: the one copy writes the
    // message's bytes once at the pointer start returned.

    if ( !data || priority != ROCKETNET_PRIORITY_MEDIUM )
    {
        rn_endpoint_count( endpoint, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
        return 0;
    }

    uint8_t * pointer = rn_endpoint_start( endpoint, channel, reliability, bytes, endpoint->max_payload_bytes );
    if ( !pointer )
        return 0;

    memcpy( pointer, data, (size_t) bytes );

    const uint32_t result = rn_endpoint_finish( endpoint, bytes, serial );
    if ( result == 0 )
        rn_endpoint_abort( endpoint );

    return result;
}

// ---------------------------------------------------------------
//                     acks, payloads, tick
// ---------------------------------------------------------------

void rn_endpoint_on_acked( struct rn_endpoint_t * endpoint, uint64_t packet_id, double now )
{
    // proofs/tla/RNEndpoint.tla OnAcked (RNChannel ReceiveAck): the transport's
    // acked packet id is the only ack (R1)

    rn_ordered_acks( &endpoint->ordered, &packet_id, 1, now );
}

bool rn_endpoint_on_payload( struct rn_endpoint_t * endpoint, const uint8_t * payload, int bytes,
                             rn_delivery_t * out, int out_capacity, int * out_count, double now )
{
    // proofs/tla/RNEndpoint.tla OnPayload (RNChannel OpenPacket, DeliverMessage):
    // layer 2's reader, then each record through the window and the dispatch
    // (13.3). A violation is held for rn_endpoint_error. The CONFIG
    // comparison (ConfigCompare) runs inside the dispatch. OpenAny: the
    // packet opened is the one the transport hands over, the head of its wire.

    (void) now;
    const uint32_t before = rn_ordered_deliveries( &endpoint->ordered );
    const int result = rn_ordered_receive( &endpoint->ordered, payload, bytes );
    const uint32_t after = rn_ordered_deliveries( &endpoint->ordered );
    int copied = 0;
    if ( out )
    {
        for ( uint32_t k = before; k < after && copied < out_capacity; k++ )
            out[copied++] = endpoint->region->deliveries[k];
    }

    if ( out_count )
        *out_count = out ? copied : (int) ( after - before );

    return result == ROCKETNET_MESSAGE_VIOLATION_NONE && rn_endpoint_error( endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE;
}

void rn_endpoint_tick( struct rn_endpoint_t * endpoint, double now )
{
    // proofs/tla/RNEndpoint.tla Tick: the receipts' LOSS sweep (RNReceipts
    // SweepLoss), the end of this update's receipt list, and the reliable
    // timeout (6.4 "Giving up"). The timeout is checked here, every update,
    // so a connection whose write pass never reaches its reliable phase (the
    // transport's send capacity spent, packets_per_update reached) still
    // ends RELIABLE_TIMEOUT; rn_endpoint_error reports it. The check is O(1).

    rn_window_ack_receipt_sweep( &endpoint->ordered.ack, now );
    rn_window_ack_receipt_end_update( &endpoint->ordered.ack );
    rn_window_ack_check_timeout( &endpoint->ordered.ack, now );
}

void rn_endpoint_config_tick( struct rn_endpoint_t * endpoint, int transport_connected, double now )
{
    // proofs/tla/RNEndpoint.tla Tick, the CONFIG half (10.3): an endpoint still
    // waiting for the remote endpoint's CONFIG reliable_timeout after its connection
    // began ends it as RELIABLE_TIMEOUT. The wait starts at the first update
    // the connection is connected at: the server's from its Observe reset,
    // the client's from the first update its transport reports CONNECTED
    // (11.3), which transport_connected says.

    if ( endpoint->ordered.config_received )
        return;
    if ( endpoint->config_wait_start < 0.0 )
    {
        if ( transport_connected )
            endpoint->config_wait_start = now;
        return;
    }

    const double reliable_timeout = (double) endpoint->config->reliable_message_timeout_ms / 1000.0;
    if ( now - endpoint->config_wait_start >= reliable_timeout )
        endpoint->config_error = ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT;
}

uint32_t rn_endpoint_begin_update( struct rn_endpoint_t * endpoint )
{
    // RNChannel EndUpdate: the last update's deliveries expire and their
    // receive descriptors return to the region (RNSlots Release's charge).
    // proofs/tla/RNOutgoingBuild.tla BeginUpdate: the packet allowance (written)
    // resets, and queued reliable work is offered first in the next pass.
    // A transport may exhaust its frame capacity before next_packet_bytes
    // reaches the end of the write pass. The next update must offer reliable
    // work first and begin a fresh budget phase while keeping queued records.

    endpoint->write_phase = RN_ENDPOINT_WRITE_IDLE;
    endpoint->written = 0;

    const uint32_t delivered = endpoint->ordered.channels.deliveries;
    rn_ordered_end_update( &endpoint->ordered );
    rn_window_ack_receipt_begin_update( &endpoint->ordered.ack );

    return delivered;
}

// ---------------------------------------------------------------
//                          the write pass
// ---------------------------------------------------------------

static int rn_endpoint_fifo_due( const rn_region_t * region, const rn_window_ack_t * ack, bool chunk, double delay, double now )
{
    // whether the ordered path has a packet to write now: a message queued for
    // its first write, or a number at the head of either resend FIFO that is
    // unsent or past its class's adaptive resend delay (chunks also respect the
    // reliable-timeout attempts cap). This keeps a frame from being started
    // for nothing.

    uint32_t cursor = rn_window_ack_resend_first( ack, chunk );
    for ( int steps = 0; cursor != RN_RESEND_NONE; steps++ )
    {
        if ( steps >= 64 )
            return 1;                                           // a long acked prefix: let the write walk it

        const rn_send_number_t * slot = &region->send_numbers[cursor];
        if ( !slot->acked )
            return ( rn_send_number_unsent_due( slot->last_send ) || now >= slot->last_send + delay ) ? 1 : 0;

        cursor = rn_window_ack_resend_next( ack, chunk, cursor );
    }

    return 0;
}

static bool rn_endpoint_ordered_due( const rn_endpoint_t * endpoint, double now )
{
    const rn_ordered_t * ordered = &endpoint->ordered;
    if ( ordered->channels.ready_head != RN_DESCRIPTOR_NONE )
        return true;

    const rn_window_ack_t * ack = &ordered->ack;
    const rn_region_t * region = endpoint->region;

    return rn_endpoint_fifo_due( region, ack, false, rn_window_ack_resend_delay( ack ), now ) ||
           rn_endpoint_fifo_due( region, ack, true, rn_window_ack_chunk_resend_delay( ack ), now );
}

static void rn_endpoint_unreliable_unlink( rn_endpoint_t * endpoint, uint32_t d )
{
    rn_region_t * region = endpoint->region;
    rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    if ( endpoint->unreliable_count > 0 )
        endpoint->unreliable_count--;

    descriptor->enqueue_ticket = 0;
    descriptor->schedule_flags = 0;
    rn_region_send_free( region, d );
    rn_unreliable_sync( endpoint );
}

static void rn_endpoint_unreliable_drop_all( rn_endpoint_t * endpoint, int counter, uint32_t already_counted )
{
    const uint32_t count = endpoint->unreliable_count;
    while ( endpoint->unreliable_head != RN_DESCRIPTOR_NONE )
    {
        const rn_send_descriptor_t * descriptor = &endpoint->region->send_descriptors[endpoint->unreliable_head];
        if ( descriptor->reliability == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT )
            rn_window_ack_receipt_emit_loss( &endpoint->ordered.ack, descriptor->serial ); // local budget drop before wire send

        rn_endpoint_unreliable_unlink( endpoint, endpoint->unreliable_head );
    }

    if ( count > already_counted )
        rn_endpoint_count( endpoint, counter, count - already_counted );
}

static int rn_endpoint_sequenced_payload_bytes( const rn_send_descriptor_t * descriptor )
{
    const int header = 3 + 3 + 3 + ( descriptor->channel >= 256 ? 2 : 1 );
    return (int) descriptor->bytes + header + 1; // one RECORDS record and form byte
}

static bool rn_endpoint_large_sequenced( const rn_endpoint_t * endpoint, const rn_send_descriptor_t * descriptor )
{
    (void) endpoint;

    // the packed writer reserves the form byte, so the full-payload boundary takes this path

    return descriptor->reliability == ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED &&
           rn_endpoint_sequenced_payload_bytes( descriptor ) >= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
}

static void rn_endpoint_end_reliable( rn_endpoint_t * endpoint )
{
    // proofs/tla/RNChannel.tla DropUnreliableForBudget: unreliable sends past the budget are dropped and counted, and this pass writes nothing further.
    // The end of the reliable path: a reliable packet the budget could not pay
    // waits; nothing after it spends the bucket this pass, so
    // this pass's unreliable sends are over budget: dropped and counted.

    if ( endpoint->budget.reliable_waiting )
    {
        rn_endpoint_unreliable_drop_all( endpoint, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET, 0 );
        endpoint->write_phase = RN_ENDPOINT_WRITE_DONE;
    }
    else
        endpoint->write_phase = RN_ENDPOINT_WRITE_UNRELIABLE;
}

static int rn_endpoint_plan_large_sequenced( rn_endpoint_t * endpoint )
{
    // RNChannelTypes!WritePacket: a large code-1 state is one unreliable RECORDS
    // payload. The transport splits it into type-9 fragments; no layer-6 block or
    // reliable number is created. The whole fragment plan is charged before
    // writing, so an over-budget state cannot send only a prefix of its fragments.
    // Returns the payload bytes to request, 0 when the head was consumed with no
    // packet (counted), or -1 when the pass stops here.

    rn_region_t * region = endpoint->region;
    const uint32_t d = endpoint->unreliable_head;
    const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    const int payload_bytes = rn_endpoint_sequenced_payload_bytes( descriptor );
    int fragment_count, stride;

    // an exact-capacity record is a whole packet, not a fragment payload

    const bool exact_packet = payload_bytes == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    if ( exact_packet )
    {
        fragment_count = 1;
        stride = payload_bytes;
    }
    else if ( !rocketnet_fragment_plan( payload_bytes, &fragment_count, &stride ) )
    {
        rn_endpoint_count( endpoint, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
        rn_endpoint_unreliable_unlink( endpoint, d );
        return 0;
    }

    // R6: an uncapped connection skips the bucket here entirely; its large
    // unreliable state is never dropped or delayed by the budget.

    int admitted_wire_bytes = 0;
    if ( endpoint->send_budget_enabled )
    {
        for ( int i = 0; i < fragment_count; i++ )
        {
            const int fragment_bytes = i + 1 < fragment_count ? stride : payload_bytes - stride * ( fragment_count - 1 );
            const int wire_payload_bytes = fragment_bytes + ( exact_packet ? 0 : ROCKETNET_FRAGMENT_HEADER_BYTES );
            const int maximum = rocketnet_send_budget_max_wire_bytes( wire_payload_bytes );
            if ( maximum == 0 )
                return -1;

            admitted_wire_bytes += maximum;
        }

        rocketnet_send_budget_t * budget = &endpoint->budget;
        if ( budget->rank_reached < rocketnet_send_budget_class_rank( ROCKETNET_SEND_CLASS_UNRELIABLE ) )
            budget->rank_reached = rocketnet_send_budget_class_rank( ROCKETNET_SEND_CLASS_UNRELIABLE );

        if ( budget->reliable_waiting || (double) admitted_wire_bytes > budget->tokens )
        {
            budget->unreliable_dropped++;
            budget->unreliable_dropped_bytes += (uint64_t) admitted_wire_bytes;
            rn_endpoint_count( endpoint, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
            rn_endpoint_unreliable_unlink( endpoint, d );
            return 0;
        }
    }

    endpoint->large_descriptor = d;
    endpoint->large_bytes = payload_bytes;
    endpoint->large_admitted = admitted_wire_bytes;

    return payload_bytes;
}

int rn_endpoint_next_packet_bytes( struct rn_endpoint_t * endpoint, double now )
{
    // proofs/tla/RNEndpoint.tla WritePacket, the plan (9.3, RNChannel WritePacket
    // with RNSendBudget Send, Wait and Drop): ordered progress and
    // retransmits first, one packet at a time so a frame is started only for
    // a packet that is written, then unreliable records under the
    // connection's budget (R6) and shared per-update packet allowance.
    // proofs/tla/RNOutgoingBuild.tla BeginFlush and EndFlush: an idle pass opens at
    // its reliable phase, and returning 0 ends it once nothing more may be
    // sent this update.

    if ( endpoint->packet_written )
        return 0;                                               // the written packet is not yet sent or failed

    if ( endpoint->write_phase == RN_ENDPOINT_WRITE_IDLE )
    {
        if ( endpoint->send_budget_enabled )
            rocketnet_send_budget_begin_update( &endpoint->budget, now );

        // every resend waits the same delay, with no back-off (proofs/tla/RNResendTiming.tla)

        rn_window_ack_gaps_update( &endpoint->ordered.ack, now );   // the resend gap record (development builds only)
        endpoint->write_phase = RN_ENDPOINT_WRITE_RELIABLE;
    }

    const int max_packets = endpoint->config->packets_per_update;

    if ( endpoint->write_phase == RN_ENDPOINT_WRITE_RELIABLE )
    {
        if ( endpoint->written < max_packets && rn_endpoint_ordered_due( endpoint, now ) )
        {
            endpoint->packet_kind = RN_ENDPOINT_PACKET_ORDERED;
            return ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
        }

        rn_endpoint_end_reliable( endpoint );
    }

    while ( endpoint->write_phase == RN_ENDPOINT_WRITE_UNRELIABLE )
    {
        if ( endpoint->written >= max_packets || endpoint->unreliable_count == 0 || endpoint->unreliable_head == RN_DESCRIPTOR_NONE )
        {
            endpoint->write_phase = RN_ENDPOINT_WRITE_DONE;
            break;
        }

        const rn_send_descriptor_t * first = &endpoint->region->send_descriptors[endpoint->unreliable_head];
        if ( rn_endpoint_large_sequenced( endpoint, first ) )
        {
            const int request = rn_endpoint_plan_large_sequenced( endpoint );
            if ( request > 0 )
            {
                endpoint->packet_kind = RN_ENDPOINT_PACKET_SEQUENCED_LARGE;
                return request;
            }

            if ( request < 0 )
            {
                endpoint->write_phase = RN_ENDPOINT_WRITE_DONE;
                break;
            }

            endpoint->written++;
            continue;
        }

        endpoint->packet_kind = RN_ENDPOINT_PACKET_UNRELIABLE;
        return ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    }

    // the pass is over; the next call opens a new one

    endpoint->write_phase = RN_ENDPOINT_WRITE_IDLE;
    endpoint->packet_kind = RN_ENDPOINT_PACKET_NONE;

    return 0;
}

static int rn_endpoint_unreliable_items( rn_endpoint_t * endpoint, rn_packet_item_t * items, uint32_t * descriptors, uint32_t * generations )
{
    // the unreliable records, packed several to a packet: the items from the
    // queue head, up to a large sequenced record (sent alone, after these)

    rn_region_t * region = endpoint->region;
    int count = 0;
    rn_unreliable_walk_t walk;
    rn_unreliable_walk_begin( endpoint, &walk );
    uint32_t d;
    while ( count < ROCKETNET_PACKET_BUILDER_MAX_RECORDS && ( d = rn_unreliable_walk_next( endpoint, &walk ) ) != RN_DESCRIPTOR_NONE )
    {
        const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];


        if ( rn_endpoint_large_sequenced( endpoint, descriptor ) )
            break; // send the preceding small records first

        rn_packet_item_t * item = &items[count];
        memset( item, 0, sizeof( *item ) );
        item->data = rn_region_send_write_pointer( region, d );
        item->bytes = descriptor->bytes;
        item->number = ROCKETNET_PACKET_NUMBER_NONE;
        item->descriptor = d;
        item->channel = descriptor->channel;
        item->code = descriptor->reliability == ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED ? ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED : ROCKETNET_MESSAGE_CODE_UNRELIABLE;
        item->ordering = descriptor->ordering & ROCKETNET_MESSAGE_U24_MASK;
        item->sequencing = descriptor->sequencing & ROCKETNET_MESSAGE_U24_MASK;
        item->send_class = ROCKETNET_SEND_CLASS_UNRELIABLE;

        descriptors[count] = d;
        generations[count] = descriptor->generation;
        count++;
    }

    return count;
}

int rn_endpoint_write_packet( struct rn_endpoint_t * endpoint, uint8_t * payload, int capacity, uint64_t packet_id, double now )
{
    // proofs/tla/RNEndpoint.tla WritePacket: the planned packet written by layer 6
    // in place (THE COPIES (a)) into the frame the caller started for
    // packet_id, whose sent-map entry rn_endpoint_packet_sent commits.
    // WriteAny: the records chosen here end in WritePacket (sent) or
    // PacketFailed (refused). proofs/tla/RNOutgoingBuild.tla SendReliable and
    // SendUnreliable: the ordered packet of the reliable phase, then the
    // queued unreliable records copied from their region records.

    (void) capacity;
    (void) packet_id;

    if ( endpoint->packet_written || !payload )
        return 0;

    rn_region_t * region = endpoint->region;

    switch ( endpoint->packet_kind )
    {
        case RN_ENDPOINT_PACKET_ORDERED:
        {
            const int packets = rn_ordered_write( &endpoint->ordered, now, &payload, 1 );
            if ( packets == 0 )
            {
                endpoint->packet_kind = RN_ENDPOINT_PACKET_NONE;
                rn_endpoint_end_reliable( endpoint );
                return 0;
            }

            endpoint->packet_written = 1;
            return (int) endpoint->ordered.builder.packet[0].payload_bytes;
        }

        case RN_ENDPOINT_PACKET_UNRELIABLE:
        {
            rn_packet_item_t items[ROCKETNET_PACKET_BUILDER_MAX_RECORDS];
            uint32_t descriptors[ROCKETNET_PACKET_BUILDER_MAX_RECORDS];
            uint32_t generations[ROCKETNET_PACKET_BUILDER_MAX_RECORDS];
            const int count = rn_endpoint_unreliable_items( endpoint, items, descriptors, generations );

            const int packets = rn_packet_builder_write( &endpoint->unreliable_builder, items, count, &payload, 1 );
            if ( packets == 0 )
            {
                // RNSendBudget Drop: unreliable sends over budget are dropped and
                // counted, all of this pass's (R6)

                if ( endpoint->unreliable_builder.unreliable_dropped > 0 )
                    rn_endpoint_unreliable_drop_all( endpoint, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET, endpoint->unreliable_builder.unreliable_dropped );

                endpoint->packet_kind = RN_ENDPOINT_PACKET_NONE;
                endpoint->write_phase = RN_ENDPOINT_WRITE_DONE;
                return 0;
            }

            // the code-5 serials the packet carries, for its receipt entry
            // (sent) or its LOSS (failed)

            endpoint->receipt_count = 0;
            for ( int i = 0; i < count; i++ )
            {
                if ( !items[i].taken )
                    continue;

                const rn_send_descriptor_t * descriptor = &region->send_descriptors[descriptors[i]];
                if ( descriptor->reliability == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT )
                    endpoint->receipt_serials[endpoint->receipt_count++] = descriptor->serial;
            }

            endpoint->packet_records = endpoint->unreliable_builder.packet[0].records;

            // the written records' room returns to the region (9.3 step 3):
            // one forward pass, not one scan per taken record (proofs/tla/RNChannelQueues.tla).

            uint32_t taken_index[ROCKETNET_PACKET_BUILDER_MAX_RECORDS];
            uint32_t taken_generation[ROCKETNET_PACKET_BUILDER_MAX_RECORDS];
            int taken_count = 0;
            for ( int i = 0; i < count; i++ )
            {
                if ( !items[i].taken )
                    continue;

                taken_index[taken_count] = descriptors[i];
                taken_generation[taken_count] = generations[i];
                taken_count++;
            }

            rn_unreliable_walk_t walk;
            rn_unreliable_walk_begin( endpoint, &walk );
            int taken = 0;
            uint32_t d;
            while ( taken < taken_count && ( d = rn_unreliable_walk_next( endpoint, &walk ) ) != RN_DESCRIPTOR_NONE )
            {
                const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];

                const bool match = d == taken_index[taken] &&
                                   descriptor->generation == taken_generation[taken] &&
                                   rn_descriptor_schedule_live( descriptor );
                if ( match )
                {
                    rn_endpoint_unreliable_unlink( endpoint, d );
                    taken++;
                }
            }

            endpoint->packet_written = 1;
            return (int) endpoint->unreliable_builder.packet[0].payload_bytes;
        }

        case RN_ENDPOINT_PACKET_SEQUENCED_LARGE:
        {
            const uint32_t d = endpoint->large_descriptor;
            const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
            const int payload_bytes = endpoint->large_bytes;

            rocketnet_message_record_t record = {};
            record.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED;
            record.channel = descriptor->channel;
            record.ordering = descriptor->ordering & ROCKETNET_MESSAGE_U24_MASK;
            record.sequencing = descriptor->sequencing & ROCKETNET_MESSAGE_U24_MASK;
            record.data = rn_region_send_write_pointer( region, d );
            record.bytes = (int) descriptor->bytes;

            rocketnet_message_writer_t writer;
            rocketnet_message_writer_begin( &writer, payload, payload_bytes );

            if ( !rocketnet_message_writer_add( &writer, &record ) || rocketnet_message_writer_finish( &writer ) != payload_bytes )
            {
                rn_endpoint_count( endpoint, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
                rn_endpoint_unreliable_unlink( endpoint, d );
                endpoint->written++;
                endpoint->packet_kind = RN_ENDPOINT_PACKET_NONE;
                return 0;
            }

            // the plan's maximum wire charge is conservative; packet_sent
            // settles with the sum the transport returns after it stages
            // every sealed fragment.
            // R6: an uncapped connection holds no bucket to charge

            if ( endpoint->send_budget_enabled )
            {
                endpoint->budget.tokens -= (double) endpoint->large_admitted;
                endpoint->budget.bytes_spent += (uint64_t) endpoint->large_admitted;
            }

            endpoint->packet_written = 1;
            return payload_bytes;
        }

        default:
            return 0;
    }
}

bool rn_endpoint_set_send_kbps( struct rn_endpoint_t * endpoint, int32_t kbps, double now )
{
    // proofs/tla/RNSendBudget.tla SetKbps (R6, docs/CHANNELS.md 11.5): the runtime
    // override. A value of 0 or lower uncaps this connection and detaches the
    // bucket from both builders; a positive value up to the maximum caps it,
    // initialising a fresh bucket at now when the connection was uncapped; above
    // the maximum changes nothing.

    if ( !endpoint )
        return false;
    if ( kbps > ROCKETNET_SEND_BUDGET_MAX_KBPS )
        return false;

    if ( kbps <= 0 )
    {
        if ( !endpoint->send_budget_enabled )
            return true;

        memset( &endpoint->budget, 0, sizeof( endpoint->budget ) );
        endpoint->send_budget_enabled = 0;
        endpoint->ordered.budget = NULL;
        endpoint->ordered.builder.budget = NULL;
        endpoint->unreliable_builder.budget = NULL;
        return true;
    }

    if ( !endpoint->send_budget_enabled )
    {
        if ( !rocketnet_send_budget_init( &endpoint->budget, (uint32_t) kbps, now ) )
            return false;

        endpoint->send_budget_enabled = 1;
        endpoint->ordered.budget = &endpoint->budget;
        endpoint->ordered.builder.budget = &endpoint->budget;
        endpoint->unreliable_builder.budget = &endpoint->budget;
        return true;
    }

    return rocketnet_send_budget_set_kbps( &endpoint->budget, (uint32_t) kbps, now );
}

void rn_endpoint_packet_sent( struct rn_endpoint_t * endpoint, uint64_t packet_id, int wire_bytes, double now )
{
    // proofs/tla/RNEndpoint.tla WritePacket, its commit (9.3 step 4, RNSlots Send):
    // the packet enters the sent map under the transport's id, and the
    // budget is settled at the sealed size. proofs/tla/RNOutgoingBuild.tla
    // TransportSend: the commit of a write-pass packet the transport sent.

    if ( !endpoint->packet_written )
        return;

    switch ( endpoint->packet_kind )
    {
        case RN_ENDPOINT_PACKET_ORDERED:
            rn_ordered_commit( &endpoint->ordered, 0, packet_id, now );
            rn_ordered_settle( &endpoint->ordered, 0, wire_bytes );
            break;

        case RN_ENDPOINT_PACKET_UNRELIABLE:
            if ( endpoint->receipt_count )
                rn_window_ack_commit_receipt_packet( &endpoint->ordered.ack, packet_id, endpoint->receipt_serials, endpoint->receipt_count, now ); // RNReceipts WritePacket
            rn_packet_builder_settle( &endpoint->unreliable_builder, 0, wire_bytes );
            break;

        case RN_ENDPOINT_PACKET_SEQUENCED_LARGE:
            if ( endpoint->send_budget_enabled )                       // R6: uncapped, no bucket to settle
                rocketnet_send_budget_settle( &endpoint->budget, endpoint->large_admitted, wire_bytes );
            rn_endpoint_unreliable_unlink( endpoint, endpoint->large_descriptor );
            break;

        default:
            break;
    }

    endpoint->written++;
    endpoint->packet_written = 0;
    endpoint->packet_kind = RN_ENDPOINT_PACKET_NONE;
    endpoint->receipt_count = 0;
}

void rn_endpoint_packet_failed( struct rn_endpoint_t * endpoint, uint64_t packet_id, double now )
{
    // proofs/tla/RNEndpoint.tla PacketFailed (6.4 "Failed send", P5): the packet
    // was written and never sent, so its reliable records are due again and
    // resent first. With nothing written, no frame was started for the plan:
    // the reliable path ends there, an unreliable one ends the pass.

    (void) packet_id;

    if ( !endpoint->packet_written )
    {
        if ( endpoint->packet_kind == RN_ENDPOINT_PACKET_ORDERED )
            rn_endpoint_end_reliable( endpoint );
        else if ( endpoint->packet_kind != RN_ENDPOINT_PACKET_NONE )
            endpoint->write_phase = RN_ENDPOINT_WRITE_DONE;

        endpoint->packet_kind = RN_ENDPOINT_PACKET_NONE;
        return;
    }

    switch ( endpoint->packet_kind )
    {
        case RN_ENDPOINT_PACKET_ORDERED:
            rn_ordered_packet_failed( &endpoint->ordered, 0, now );     // 6.4 Failed send: resent first next update
            rn_ordered_settle( &endpoint->ordered, 0, 0 );              // proofs/tla/RNSendBudget.tla SendFailed: refund the whole charge
            endpoint->written++;
            rn_endpoint_end_reliable( endpoint );
            break;

        case RN_ENDPOINT_PACKET_UNRELIABLE:
            rn_endpoint_count( endpoint, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED, endpoint->packet_records );
            for ( uint32_t i = 0; i < endpoint->receipt_count; i++ )
                rn_window_ack_receipt_emit_loss( &endpoint->ordered.ack, endpoint->receipt_serials[i] );
            rn_packet_builder_settle( &endpoint->unreliable_builder, 0, 0 );  // proofs/tla/RNSendBudget.tla SendFailed: refund the whole charge
            endpoint->written++;
            break;

        case RN_ENDPOINT_PACKET_SEQUENCED_LARGE:
            rn_endpoint_count( endpoint, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
            if ( endpoint->send_budget_enabled )                       // proofs/tla/RNSendBudget.tla SendFailed: refund the whole charge
                rocketnet_send_budget_settle( &endpoint->budget, endpoint->large_admitted, 0 );
            rn_endpoint_unreliable_unlink( endpoint, endpoint->large_descriptor );
            endpoint->written++;
            break;

        default:
            break;
    }

    endpoint->packet_written = 0;
    endpoint->packet_kind = RN_ENDPOINT_PACKET_NONE;
    endpoint->receipt_count = 0;
}

// ---------------------------------------------------------------
//                     receipts, error, deadline
// ---------------------------------------------------------------

int rn_endpoint_take_receipts( const struct rn_endpoint_t * endpoint, struct rocketnet_message_receipt_t * out, int capacity )
{
    // RNReceipts TakeReceipt: this update's outcomes (7.4)

    return rn_window_ack_receipts( &endpoint->ordered.ack, out, capacity );
}

int rn_endpoint_error( const struct rn_endpoint_t * endpoint )
{
    // proofs/tla/RNEndpoint.tla err (12): a send that found no room first (R5), then
    // the region's exhaustion, a wire violation (a CONFIG mismatch among
    // them, logged with the field and both values when it was compared) and a
    // reliable message past its timeout, in rn_ordered_decide's order, then
    // the remote endpoint's CONFIG past reliable_timeout (rn_endpoint_config_tick)

    if ( endpoint->send_error != ROCKETNET_DISCONNECT_REASON_NONE )
        return endpoint->send_error;

    const int reason = rn_ordered_decide( &endpoint->ordered );
    if ( reason != ROCKETNET_DISCONNECT_REASON_NONE )
        return reason;

    return endpoint->config_error;
}
