/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// the client message object: the object core of
// sdk/rocketnet_message_server.cpp with one connection, over a
// rocketnet_client_transport_t. Its one connection is bound at create and never
// bound again; when its region is exhausted the client disconnects itself with
// ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED (R5, proofs/tla/RNConnectionMemory.tla
// Kick). The spec of the update and release rules is proofs/tla/RNSlots.tla.

#include <string.h>

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_message_object.h"
#include "rocketnet_send_budget.h"

// ---------------------------------------------------------------
//                     the client transport's link
// ---------------------------------------------------------------

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

static uint8_t * rn_client_link_start_packet( void * transport, int client_index, int request_bytes, rocketnet_packet_context_t * context )
{
    (void) client_index;
    return rocketnet_client_transport_start_packet( (rocketnet_client_transport_t *) transport, request_bytes, context );
}

static uint64_t rn_client_link_connection( void * transport, int client_index )
{
    (void) client_index;
    return rocketnet_client_transport_state( (rocketnet_client_transport_t *) transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ? 1 : 0;
}

static uint64_t rn_client_link_entry_connection( void * transport, int k )
{
    // the client transport holds one connection, which the object binds at
    // create with the id 1 (rn_message_object_create): every entry is that one's

    (void) transport;
    (void) k;
    return 1;
}

static void rn_client_link_disconnect( void * transport, int client_index )
{
    (void) client_index;
    rocketnet_client_transport_disconnect( (rocketnet_client_transport_t *) transport );
}

static int rn_client_link_end_reason( void * transport, int client_index )
{
    (void) client_index;

    // a client transport runs one connection attempt (its create connects,
    // rocketnet_client_transport.cpp:765), so its CLIENT_DISCONNECT_ACCEPTED
    // counter is nonzero exactly when that attempt ended on the server's
    // disconnect: rocketnet_client_process_disconnect counts it
    // (rocketnet_client_protocol.cpp:1202) before it sets the protocol
    // DISCONNECTED.

    const struct rocketnet_counters_t * counters = rocketnet_client_transport_counters( (rocketnet_client_transport_t *) transport );
    if ( counters && counters->values[ROCKETNET_COUNTER_CLIENT_DISCONNECT_ACCEPTED] )
        return ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED;

    // the AEAD integrity limit (rocketnet_client_integrity_failure) ends the
    // attempt in plain DISCONNECTED: too much received data that failed authentication
    // is a protocol violation, so the public state reads PROTOCOL_VIOLATION

    if ( counters && counters->values[ROCKETNET_COUNTER_INTEGRITY_LIMIT_REACHED] )
        return ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION;
    return ROCKETNET_DISCONNECT_REASON_NONE;
}

void rn_message_client_link( struct rn_message_link_t * link, struct rocketnet_client_transport_t * transport )
{
    memset( link, 0, sizeof( *link ) );
    link->transport = transport;
    link->update = RN_LINK_FORWARD( rocketnet_client_transport_update )::call;
    link->payloads = rn_client_link_payloads;
    link->accept_payload = RN_LINK_FORWARD( rocketnet_client_transport_accept_payload )::call;

    if ( transport )
        rocketnet_client_transport_defer_payload_acks( transport );

    link->acked = rn_client_link_acked;
    link->payload_connection = rn_client_link_entry_connection;
    link->acked_connection = rn_client_link_entry_connection;
    link->max_payload_bytes = RN_LINK_FORWARD( rocketnet_client_transport_max_payload_bytes )::call;
    link->start_packet = rn_client_link_start_packet;
    link->finish_packet = RN_LINK_FORWARD( rocketnet_client_transport_finish_packet )::call;
    link->abort_packet = RN_LINK_FORWARD( rocketnet_client_transport_abort_packet )::call;
    link->send_packets = RN_LINK_FORWARD( rocketnet_client_transport_send_packets )::call;
    link->connection = rn_client_link_connection;
    link->disconnect = rn_client_link_disconnect;
    link->state = RN_LINK_FORWARD( rocketnet_client_transport_state )::call;
    link->end_reason = rn_client_link_end_reason;
    link->stop = RN_LINK_FORWARD( rocketnet_client_transport_stop )::call;
    link->stopped = RN_LINK_FORWARD( rocketnet_client_transport_stopped )::call_const;
    link->destroy = RN_LINK_FORWARD( rocketnet_client_transport_destroy )::call;
    link->clock_from_first_update = 1;
}

// ---------------------------------------------------------------
//                     the client's C API
// ---------------------------------------------------------------

uint64_t rocketnet_message_client_required_memory( const struct rocketnet_message_config_t * message_config, uint32_t send_kbps )
{
    if ( send_kbps > ROCKETNET_SEND_BUDGET_MAX_KBPS )
        return 0;
    return rn_message_object_required_bytes( message_config, 1, 0, (int32_t) send_kbps );
}

struct rocketnet_message_client_t * rocketnet_message_client_create( void * context, const uint8_t * connect_token,
                                                                     const struct rocketnet_client_transport_config_t * transport_config,
                                                                     const struct rocketnet_message_config_t * message_config,
                                                                     void * memory, uint64_t memory_bytes, uint32_t send_kbps )
{
    return rocketnet_message_client_create_ex( context, connect_token, transport_config, message_config, memory, memory_bytes, send_kbps, NULL, 0 );
}

// resolved_memory_budget NULL: the transport create reads the environment
// (rocketnet_message_client_create_ex); set: transport_config is already
// resolved and connect reads nothing (rocketnet_message_client_create_resolved)
static struct rocketnet_message_client_t * rn_message_client_create( void * context, const uint8_t * connect_token,
                                                                     const struct rocketnet_client_transport_config_t * transport_config,
                                                                     const struct rocketnet_message_config_t * message_config,
                                                                     void * memory, uint64_t memory_bytes, uint32_t send_kbps, int * status, int reserve_unlimited,
                                                                     const uint64_t * resolved_memory_budget );

struct rocketnet_message_client_t * rocketnet_message_client_create_ex( void * context, const uint8_t * connect_token,
                                                                        const struct rocketnet_client_transport_config_t * transport_config,
                                                                        const struct rocketnet_message_config_t * message_config,
                                                                        void * memory, uint64_t memory_bytes, uint32_t send_kbps, int * status, int reserve_unlimited )
{
    return rn_message_client_create( context, connect_token, transport_config, message_config, memory, memory_bytes, send_kbps, status, reserve_unlimited, NULL );
}

struct rocketnet_message_client_t * rocketnet_message_client_create_resolved( void * context, const uint8_t * connect_token,
                                                                              const struct rocketnet_client_transport_config_t * transport_config,
                                                                              const struct rocketnet_message_config_t * message_config,
                                                                              void * memory, uint64_t memory_bytes, uint32_t send_kbps, int * status, int reserve_unlimited,
                                                                              uint64_t memory_budget_bytes )
{
    return rn_message_client_create( context, connect_token, transport_config, message_config, memory, memory_bytes, send_kbps, status, reserve_unlimited, &memory_budget_bytes );
}

static struct rocketnet_message_client_t * rn_message_client_create( void * context, const uint8_t * connect_token,
                                                                     const struct rocketnet_client_transport_config_t * transport_config,
                                                                     const struct rocketnet_message_config_t * message_config,
                                                                     void * memory, uint64_t memory_bytes, uint32_t send_kbps, int * status, int reserve_unlimited,
                                                                     const uint64_t * resolved_memory_budget )
{
    int ignored = ROCKETNET_STATUS_OK;
    if ( !status )
        status = &ignored;
    *status = ROCKETNET_STATUS_INVALID_ARGUMENT;
    if ( !rocketnet_api_null_guard( connect_token ) )
        return NULL;

    // R6 (standard/MESSAGES.md m9.4): 0 takes client_send_kbps from the message
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

    // refuse before the transport opens a socket: the config, the budget, and
    // a caller block too short for the layout create sizes at this budget

    const uint64_t required_bytes = rn_message_object_required_bytes( message_config, 1, 0, kbps, reserve_unlimited != 0 );
    if ( required_bytes == 0 || !( kbps <= 0 || rocketnet_send_budget_kbps_valid( (uint32_t) kbps ) ) )
    {
        rocketnet_error( "message client create refused: message config or send budget" );
        *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return NULL;
    }

    if ( memory && ( memory_bytes < required_bytes || ( (uintptr_t) memory % ROCKETNET_ALLOCATION_ALIGNMENT ) != 0 ) )
    {
        rocketnet_error( "message client create refused: the memory block is %" PRIu64 " bytes or not %d-aligned; this config and send budget need %" PRIu64,
                         memory_bytes, ROCKETNET_ALLOCATION_ALIGNMENT, required_bytes );
        *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
        return NULL;
    }

    // create takes no time: the transport is seeded at 0.0, and the object
    // hands it time relative to the first update (clock_from_first_update),
    // so the connect attempt's timers and the token's lifetime start at the
    // first update, whatever the game's clock reads then. time comes in only
    // through update.

    rocketnet_client_transport_t * transport = ( resolved_memory_budget && transport_config ) ? rocketnet_client_transport_create_resolved( context, connect_token, transport_config, 0.0, *resolved_memory_budget, status ) : rocketnet_client_transport_create_current_ex( context, connect_token, transport_config, 0.0, status );
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
    return client->object.link.destroy == RN_LINK_FORWARD( rocketnet_client_transport_destroy )::call ? (rocketnet_client_transport_t *) client->object.link.transport : NULL;
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
    // finishes the open message reservation on connection 0; asserts in a debug
    // build and returns 0 in a release build when no reservation is open

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
