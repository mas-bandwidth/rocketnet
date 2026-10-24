/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client message object (docs/CHANNELS.md 11.3): the object core of
// sdk/rocketnet_message_server.cpp with one connection, over a
// rocketnet_client_transport_t. Its one connection is bound at create and never
// bound again; when its region is exhausted the client disconnects itself with
// ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED (R5, tla/RNConnectionMemory.tla
// Kick). The spec of the update and release rules is tla/RNSlots.tla.

#include <string.h>

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_message_object.h"
#include "rocketnet_send_budget.h"

// ---------------------------------------------------------------
//                     the client transport's link
// ---------------------------------------------------------------

static void rn_client_link_update( void * transport, double time )
{
    rocketnet_client_transport_update( (rocketnet_client_transport_t *) transport, time );
}

static int rn_client_link_payloads( void * transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    *client_index = NULL;
    return rocketnet_client_transport_payloads( (rocketnet_client_transport_t *) transport, payload_data, payload_bytes );
}

static int rn_client_link_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    *client_index = NULL;
    return rocketnet_client_transport_acked_packets( (rocketnet_client_transport_t *) transport, packet_ids );
}

static int rn_client_link_max_payload_bytes( void * transport )
{
    return rocketnet_client_transport_max_payload_bytes( (rocketnet_client_transport_t *) transport );
}

static uint8_t * rn_client_link_start_packet( void * transport, int client_index, int request_bytes, rocketnet_packet_context_t * context )
{
    (void) client_index;
    return rocketnet_client_transport_start_packet( (rocketnet_client_transport_t *) transport, request_bytes, context );
}

static int rn_client_link_finish_packet( void * transport, rocketnet_packet_context_t * context, int payload_bytes )
{
    return rocketnet_client_transport_finish_packet( (rocketnet_client_transport_t *) transport, context, payload_bytes );
}

static void rn_client_link_abort_packet( void * transport, rocketnet_packet_context_t * context )
{
    rocketnet_client_transport_abort_packet( (rocketnet_client_transport_t *) transport, context );
}

static void rn_client_link_send_packets( void * transport )
{
    rocketnet_client_transport_send_packets( (rocketnet_client_transport_t *) transport );
}

static uint64_t rn_client_link_connection( void * transport, int client_index )
{
    (void) client_index;
    return rocketnet_client_transport_state( (rocketnet_client_transport_t *) transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ? 1 : 0;
}

// the client transport holds one connection, which the object binds at
// create with the id 1 (rn_message_object_create): every entry is that one's
static uint64_t rn_client_link_entry_connection( void * transport, int k )
{
    (void) transport;
    (void) k;
    return 1;
}

static void rn_client_link_disconnect( void * transport, int client_index )
{
    (void) client_index;
    rocketnet_client_transport_disconnect( (rocketnet_client_transport_t *) transport );
}

static int rn_client_link_state( void * transport )
{
    return rocketnet_client_transport_state( (rocketnet_client_transport_t *) transport );
}

// A client transport runs one connection attempt (its create connects,
// rocketnet_client_transport.cpp:765), so its CLIENT_DISCONNECT_ACCEPTED
// counter is nonzero exactly when that attempt ended on the server's
// disconnect: rocketnet_client_process_disconnect counts it
// (rocketnet_client_protocol.cpp:1202) before it sets the protocol
// DISCONNECTED.
static int rn_client_link_end_reason( void * transport )
{
    const struct rocketnet_counters_t * counters = rocketnet_client_transport_counters( (rocketnet_client_transport_t *) transport );
    if ( counters && counters->values[ROCKETNET_COUNTER_CLIENT_DISCONNECT_ACCEPTED] )
        return ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED;
    return ROCKETNET_DISCONNECT_REASON_NONE;
}

static void rn_client_link_stop( void * transport )
{
    rocketnet_client_transport_stop( (rocketnet_client_transport_t *) transport );
}

static bool rn_client_link_stopped( const void * transport )
{
    return rocketnet_client_transport_stopped( (const rocketnet_client_transport_t *) transport );
}

static void rn_client_link_destroy( void * transport )
{
    rocketnet_client_transport_destroy( (rocketnet_client_transport_t *) transport );
}

static void rn_client_link_accept_payload( void * transport, int k )
{
    rocketnet_client_transport_accept_payload( (rocketnet_client_transport_t *) transport, k );
}

void rn_message_client_link( struct rn_message_link_t * link, struct rocketnet_client_transport_t * transport )
{
    memset( link, 0, sizeof( *link ) );
    link->transport = transport;
    link->update = rn_client_link_update;
    link->payloads = rn_client_link_payloads;
    link->accept_payload = rn_client_link_accept_payload;
    if ( transport )
        rocketnet_client_transport_defer_payload_acks( transport );
    link->acked = rn_client_link_acked;
    link->payload_connection = rn_client_link_entry_connection;
    link->acked_connection = rn_client_link_entry_connection;
    link->max_payload_bytes = rn_client_link_max_payload_bytes;
    link->start_packet = rn_client_link_start_packet;
    link->finish_packet = rn_client_link_finish_packet;
    link->abort_packet = rn_client_link_abort_packet;
    link->send_packets = rn_client_link_send_packets;
    link->connection = rn_client_link_connection;
    link->disconnect = rn_client_link_disconnect;
    link->state = rn_client_link_state;
    link->end_reason = rn_client_link_end_reason;
    link->stop = rn_client_link_stop;
    link->stopped = rn_client_link_stopped;
    link->destroy = rn_client_link_destroy;
    link->clock_from_first_update = 1;
}

// ---------------------------------------------------------------
//                     the client's C API
// ---------------------------------------------------------------

uint64_t rocketnet_message_client_required_memory( const struct rocketnet_message_config_t * message_config )
{
    return rn_message_object_required_bytes( message_config, 1, 0 );
}

struct rocketnet_message_client_t * rocketnet_message_client_create( void * context, const uint8_t * connect_token,
                                                                     const struct rocketnet_client_transport_config_t * transport_config,
                                                                     const struct rocketnet_message_config_t * message_config,
                                                                     void * memory, uint64_t memory_bytes, uint32_t send_kbps )
{
    return rocketnet_message_client_create_ex( context, connect_token, transport_config, message_config, memory, memory_bytes, send_kbps, NULL, 0 );
}

struct rocketnet_message_client_t * rocketnet_message_client_create_ex( void * context, const uint8_t * connect_token,
                                                                        const struct rocketnet_client_transport_config_t * transport_config,
                                                                        const struct rocketnet_message_config_t * message_config,
                                                                        void * memory, uint64_t memory_bytes, uint32_t send_kbps, int * status, int reserve_unlimited )
{
    int ignored = ROCKETNET_STATUS_OK;
    if ( !status )
        status = &ignored;
    *status = ROCKETNET_STATUS_INVALID_ARGUMENT;
    if ( !rocketnet_api_null_guard( connect_token ) )
        return NULL;

    // R6 (docs/CHANNELS.md 11.5): 0 takes client_send_kbps from the message
    // config; a positive value overrides it for this create. The create
    // argument is uint32_t, so refuse above the maximum before narrowing:
    // a value in [2^31, 2^32) must not wrap negative and become uncapped.
    if ( send_kbps > ROCKETNET_SEND_BUDGET_MAX_KBPS )
    {
        rocketnet_error( "message client create refused: send budget above maximum" );
        *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return NULL;
    }
    int32_t kbps = (int32_t) send_kbps;
    if ( kbps == 0 )
    {
        rocketnet_message_config_t defaults;
        const rocketnet_message_config_t * resolved = message_config;
        if ( !resolved )
        {
            rocketnet_message_default_config( &defaults );
            resolved = &defaults;
        }
        kbps = resolved->client_send_kbps;
    }

    // refuse before the transport opens a socket
    if ( rn_message_object_required_bytes( message_config, 1, 0 ) == 0 || !( kbps <= 0 || rocketnet_send_budget_kbps_valid( (uint32_t) kbps ) ) )
    {
        rocketnet_error( "message client create refused: message config or send budget" );
        *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return NULL;
    }

    // create takes no time: the transport is seeded at 0.0, and the object
    // hands it time relative to the first update (clock_from_first_update),
    // so the connect attempt's timers and the token's lifetime start at the
    // first update, whatever the game's clock reads then. time comes in only
    // through update.

    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_current_ex( context, connect_token, transport_config, 0.0, status );
    if ( !transport )
    {
        if ( *status == ROCKETNET_STATUS_OK )
            *status = ROCKETNET_STATUS_INVALID_ARGUMENT;
        return NULL;
    }

    rn_message_link_t link;
    rn_message_client_link( &link, transport );
    rocketnet_message_client_t * client = (rocketnet_message_client_t *) rn_message_object_create( context, &link, 0, 1, message_config, memory, memory_bytes, kbps, 0.0, reserve_unlimited != 0 );
    *status = client ? ROCKETNET_STATUS_OK : ROCKETNET_STATUS_OUT_OF_MEMORY;
    return client;
}

void rocketnet_message_client_destroy( struct rocketnet_message_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return;
    rn_message_object_destroy( &client->object );
}

struct rocketnet_client_transport_t * rocketnet_message_client_transport( struct rocketnet_message_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return NULL;
    return client->object.link.destroy == rn_client_link_destroy ? (rocketnet_client_transport_t *) client->object.link.transport : NULL;
}

#if ROCKETNET_DEV
void rocketnet_message_client_delivery_capture( struct rocketnet_message_client_t * client, struct rocketnet_delivery_capture_t * capture )
{
    if ( !rocketnet_api_null_guard( client ) || !rocketnet_api_null_guard( capture ) )
        return;
    rn_message_object_delivery_capture( &client->object, 0, capture );
}
#endif // #if ROCKETNET_DEV

int rocketnet_message_client_state( struct rocketnet_message_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
    return client->object.link.state( client->object.link.transport );
}

int rocketnet_message_client_update( struct rocketnet_message_client_t * client, double time )
{
    if ( !rocketnet_api_null_guard( client ) )
        return ROCKETNET_UPDATE_OK;
    return rn_message_object_update( &client->object, time );
}

void rocketnet_message_client_send_packets( struct rocketnet_message_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return;
    rn_message_object_send_packets( &client->object );
}

uint8_t * rocketnet_message_client_start( struct rocketnet_message_client_t * client, int channel, int reliability, int bytes )
{
    if ( !rocketnet_api_null_guard( client ) )
        return NULL;
    return rn_message_object_start( &client->object, 0, channel, reliability, bytes );
}

uint32_t rocketnet_message_client_finish( struct rocketnet_message_client_t * client, int bytes_written )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    return rn_message_object_finish( &client->object, 0, bytes_written );
}

void rocketnet_message_client_abort( struct rocketnet_message_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return;
    rn_message_object_abort( &client->object, 0 );
}

uint32_t rocketnet_message_client_send( struct rocketnet_message_client_t * client, int channel, int reliability, const uint8_t * data, int bytes )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    if ( !rocketnet_api_null_guard( data ) )
        return 0;
    return rn_message_object_send( &client->object, 0, channel, reliability, data, bytes );
}

int rocketnet_message_client_receive( struct rocketnet_message_client_t * client, int * channel, const uint8_t ** data, int * bytes )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    if ( !rocketnet_api_null_guard( data ) )
        return 0;
    if ( !rocketnet_api_null_guard( bytes ) )
        return 0;
    return rn_message_object_receive( &client->object, NULL, channel, data, bytes );
}

int rocketnet_message_client_receive_channel( struct rocketnet_message_client_t * client, int channel, const uint8_t ** data, int * bytes )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    if ( !rocketnet_api_null_guard( data ) )
        return 0;
    if ( !rocketnet_api_null_guard( bytes ) )
        return 0;
    return rn_message_object_receive_channel( &client->object, 0, channel, data, bytes );
}

int rocketnet_message_client_receive_messages( struct rocketnet_message_client_t * client, struct rocketnet_message_batch_t * messages )
{
    if ( !rocketnet_api_null_guard( client ) || !rocketnet_api_null_guard( messages ) )
        return 0;
    return rn_message_object_receive_messages( &client->object, messages );
}

int rocketnet_message_client_receipts( struct rocketnet_message_client_t * client, struct rocketnet_message_receipt_t * out, int capacity )
{
    if ( !rocketnet_api_null_guard( client ) || capacity < 0 ||
         ( capacity > 0 && !rocketnet_api_null_guard( out ) ) )
        return 0;
    return rn_endpoint_take_receipts( &client->object.connections[0], out, capacity );
}

void rocketnet_message_client_release( struct rocketnet_message_client_t * client, const uint8_t * data )
{
    if ( !rocketnet_api_null_guard( client ) )
        return;
    if ( !rocketnet_api_null_guard( data ) )
        return;
    rn_message_object_release( &client->object, data );
}

int rocketnet_message_client_set_send_kbps( struct rocketnet_message_client_t * client, int32_t kbps )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    return rn_message_object_set_send_kbps( &client->object, 0, kbps ) ? 1 : 0;
}

int rocketnet_message_client_disconnect_reason( struct rocketnet_message_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return ROCKETNET_DISCONNECT_REASON_NONE;
    return rn_message_object_disconnect_reason( &client->object, 0 );
}

const struct rocketnet_counters_t * rocketnet_message_client_counters( struct rocketnet_message_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return NULL;
    return &client->object.counters;
}
