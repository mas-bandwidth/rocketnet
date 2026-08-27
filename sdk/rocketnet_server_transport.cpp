/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_constants.h"
#include "rocketnet_platform.h"

#include <sodium.h>
#include <memory.h>
#include <stdlib.h>
#include <time.h>

// The transport backend is selected at compile time for now: the portable
// UDP socket by default, the AF_XDP server socket with
// -DROCKETNET_SERVER_TRANSPORT_XDP=1 on linux. Runtime selection with
// preference ordering is a named follow-on.

#if !defined( ROCKETNET_SERVER_XDP )
#define ROCKETNET_SERVER_XDP 0
#endif // #if !defined( ROCKETNET_SERVER_XDP )

#if ROCKETNET_SERVER_XDP
#include "rocketnet_server_socket.h"
#endif // #if ROCKETNET_SERVER_XDP

#define ROCKETNET_SERVER_MAX_RECEIVE_PACKETS 256
#define ROCKETNET_SERVER_MAX_SEND_PACKETS    256

struct rocketnet_server_send_buffer_t
{
    int num_packets;
    rocketnet_address_t to[ROCKETNET_SERVER_MAX_SEND_PACKETS];
    int packet_bytes[ROCKETNET_SERVER_MAX_SEND_PACKETS];
    uint8_t data[ROCKETNET_FRAME_SIZE*ROCKETNET_SERVER_MAX_SEND_PACKETS];
};

struct rocketnet_server_t
{
    void * context;
    int state;
    rocketnet_address_t bind_address;
    rocketnet_address_t public_address;

#if ROCKETNET_SERVER_XDP
    struct rocketnet_server_socket_t * xdp_socket;
#else // #if ROCKETNET_SERVER_XDP
    rocketnet_platform_socket_t * socket;
#endif // #if ROCKETNET_SERVER_XDP

    struct rocketnet_counters_t counters;
    struct rocketnet_server_protocol_t protocol;

#if !ROCKETNET_SERVER_XDP
    struct rocketnet_server_send_buffer_t send_buffer;
#endif // #if !ROCKETNET_SERVER_XDP

    struct rocketnet_server_payloads_t payloads;
    uint8_t payload_storage[ROCKETNET_FRAME_SIZE*ROCKETNET_SERVER_MAX_RECEIVE_PACKETS];
};

// The send frame seam both backends serve: reserve hands back a frame to
// seal complete wire bytes into, commit stamps its length, abort returns it
// unused. The portable backend batches frames and flushes them through the
// socket at the end of update; the xdp backend seals straight into the
// af_xdp send staging, zero copies between the seal and the wire.

struct rocketnet_server_frame_t
{
    uint8_t * data;
#if ROCKETNET_SERVER_XDP
    uint64_t packet_id;
#endif // #if ROCKETNET_SERVER_XDP
};

static uint8_t * rocketnet_server_reserve_send_frame( struct rocketnet_server_t * server_transport,
                                                                const rocketnet_address_t * to,
                                                                struct rocketnet_server_frame_t * frame )
{
#if ROCKETNET_SERVER_XDP

    frame->data = rocketnet_server_socket_start_packet( server_transport->xdp_socket, to, &frame->packet_id );
    if ( !frame->data )
    {
        rocketnet_spam( "server transport send staging full. packet dropped" );
    }
    return frame->data;

#else // #if ROCKETNET_SERVER_XDP

    struct rocketnet_server_send_buffer_t * send_buffer = &server_transport->send_buffer;

    if ( send_buffer->num_packets >= ROCKETNET_SERVER_MAX_SEND_PACKETS )
    {
        rocketnet_spam( "server transport send buffer full. packet dropped" );
        frame->data = NULL;
        return NULL;
    }

    const int index = send_buffer->num_packets;
    send_buffer->to[index] = *to;
    send_buffer->packet_bytes[index] = 0;
    frame->data = send_buffer->data + index * ROCKETNET_FRAME_SIZE;
    return frame->data;

#endif // #if ROCKETNET_SERVER_XDP
}

static void rocketnet_server_commit_send_frame( struct rocketnet_server_t * server_transport,
                                                          struct rocketnet_server_frame_t * frame,
                                                          int packet_bytes )
{
    rocketnet_assert( frame->data );
    rocketnet_assert( packet_bytes > 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_RECEIVE_CEILING );

#if ROCKETNET_SERVER_XDP

    rocketnet_server_socket_finish_packet( server_transport->xdp_socket, frame->packet_id, frame->data, packet_bytes );

#else // #if ROCKETNET_SERVER_XDP

    struct rocketnet_server_send_buffer_t * send_buffer = &server_transport->send_buffer;
    send_buffer->packet_bytes[send_buffer->num_packets] = packet_bytes;
    send_buffer->num_packets++;

#endif // #if ROCKETNET_SERVER_XDP
}

static void rocketnet_server_abort_send_frame( struct rocketnet_server_t * server_transport,
                                                         struct rocketnet_server_frame_t * frame )
{
    if ( !frame->data )
        return;

#if ROCKETNET_SERVER_XDP
    rocketnet_server_socket_abort_packet( server_transport->xdp_socket, frame->packet_id, frame->data );
#else // #if ROCKETNET_SERVER_XDP
    (void) server_transport;        // the reserved slot is reused by the next reserve; nothing to undo
#endif // #if ROCKETNET_SERVER_XDP

    frame->data = NULL;
}

struct rocketnet_server_t * rocketnet_server_create( void * context,
                                                                         const char * public_address_string,
                                                                         int max_clients )
{
    rocketnet_assert( public_address_string );
    rocketnet_assert( max_clients > 0 );

    const char * public_address_env = getenv( "ROCKETNET_SERVER_PUBLIC_ADDRESS" );
    if ( public_address_env )
    {
        public_address_string = public_address_env;
    }

    rocketnet_info( "server transport public address is %s", public_address_string );

    rocketnet_address_t public_address;
    if ( !rocketnet_address_parse( &public_address, public_address_string ) )
    {
        rocketnet_error( "could not parse public address" );
        return NULL;
    }

    rocketnet_address_t bind_address = public_address;
    if ( bind_address.type == ROCKETNET_ADDRESS_IPV4 )
        memset( bind_address.data.ipv4, 0, sizeof(bind_address.data.ipv4) );
    else
        memset( bind_address.data.ipv6, 0, sizeof(bind_address.data.ipv6) );

    struct rocketnet_server_t * server_transport = (struct rocketnet_server_t*) rocketnet_malloc( context, sizeof(struct rocketnet_server_t) );
    if ( !server_transport )
        return NULL;

    memset( server_transport, 0, sizeof(struct rocketnet_server_t) );

    server_transport->context = context;

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;

#if ROCKETNET_SERVER_XDP

    server_transport->xdp_socket = rocketnet_server_socket_create( context, public_address_string, tag );
    if ( server_transport->xdp_socket == NULL )
    {
        rocketnet_error( "could not create xdp server socket" );
        rocketnet_server_destroy( server_transport );
        return NULL;
    }

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "server transport started on %s [xdp] (development keys)", rocketnet_address_to_string( &public_address, address_string ) );

#else // #if ROCKETNET_SERVER_XDP

    server_transport->socket = rocketnet_platform_socket_create( context, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    if ( server_transport->socket == NULL )
    {
        rocketnet_error( "could not create server socket" );
        rocketnet_server_destroy( server_transport );
        return NULL;
    }

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "server transport started on %s (development keys)", rocketnet_address_to_string( &bind_address, address_string ) );

#endif // #if ROCKETNET_SERVER_XDP

    server_transport->bind_address = bind_address;
    server_transport->public_address = public_address;
    server_transport->state = ROCKETNET_SERVER_RUNNING;

    // the protocol, on the published development keys, advertised at the
    // public address -- the address development tokens are minted for

    struct rocketnet_server_protocol_t * protocol = &server_transport->protocol;
    protocol->protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    protocol->send_rate = 10.0;
    memcpy( protocol->filter_value, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( protocol->backend_public_key, rocketnet_dev_backend_public_key, 32 );
    memcpy( protocol->server_public_key, rocketnet_dev_server_public_key, 32 );
    memcpy( protocol->server_secret_key, rocketnet_dev_server_secret_key, 32 );

    if ( public_address.type == ROCKETNET_ADDRESS_IPV4 )
        protocol->advertised_ipv4 = public_address;
    else
        protocol->advertised_ipv6 = public_address;

    randombytes_buf( protocol->cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    protocol->cookie_lifetime_seconds = 3;
    protocol->handshake_deadline_seconds = 5.0;
    protocol->max_timeout_seconds = 0;
    protocol->allow_negative_timeout = 0;

    protocol->encryption_map = rocketnet_encryption_map_create( context, max_clients * 2 );
    protocol->used_token_history = rocketnet_used_token_history_create( context, max_clients * 8 );
    protocol->admitted_token_set = rocketnet_admitted_token_set_create( context, max_clients * 2 );
    protocol->client_slots = rocketnet_client_slots_create( context, max_clients );
    protocol->counters = &server_transport->counters;

    if ( !protocol->encryption_map || !protocol->used_token_history || !protocol->admitted_token_set || !protocol->client_slots )
    {
        rocketnet_server_destroy( server_transport );
        return NULL;
    }

    return server_transport;
}

void rocketnet_server_destroy( struct rocketnet_server_t * server_transport )
{
    rocketnet_assert( server_transport );

    if ( server_transport->protocol.encryption_map )
        rocketnet_encryption_map_destroy( server_transport->protocol.encryption_map );
    if ( server_transport->protocol.used_token_history )
        rocketnet_used_token_history_destroy( server_transport->protocol.used_token_history );
    if ( server_transport->protocol.admitted_token_set )
        rocketnet_admitted_token_set_destroy( server_transport->protocol.admitted_token_set );
    if ( server_transport->protocol.client_slots )
        rocketnet_client_slots_destroy( server_transport->protocol.client_slots );

#if ROCKETNET_SERVER_XDP

    if ( server_transport->xdp_socket )
    {
        // the xdp socket requires stop then a pump to stopped before destroy

        if ( rocketnet_server_socket_state( server_transport->xdp_socket ) != ROCKETNET_SERVER_SOCKET_STOPPED )
        {
            rocketnet_server_socket_stop( server_transport->xdp_socket );
            rocketnet_server_socket_update( server_transport->xdp_socket );
        }
        rocketnet_server_socket_destroy( server_transport->xdp_socket );
    }

#else // #if ROCKETNET_SERVER_XDP

    if ( server_transport->socket )
        rocketnet_platform_socket_destroy( server_transport->socket );

#endif // #if ROCKETNET_SERVER_XDP

    rocketnet_clear_and_free( server_transport->context, server_transport, sizeof(struct rocketnet_server_t) );
}

void rocketnet_server_stop( struct rocketnet_server_t * server_transport )
{
    rocketnet_assert( server_transport );
    server_transport->state = ROCKETNET_SERVER_STOPPING;
}

int rocketnet_server_state( struct rocketnet_server_t * server_transport )
{
    rocketnet_assert( server_transport );
    return server_transport->state;
}

void rocketnet_server_update( struct rocketnet_server_t * server_transport )
{
    rocketnet_assert( server_transport );

    if ( server_transport->state == ROCKETNET_SERVER_STOPPING )
    {
#if ROCKETNET_SERVER_XDP
        rocketnet_server_socket_stop( server_transport->xdp_socket );
        rocketnet_server_socket_update( server_transport->xdp_socket );
#endif // #if ROCKETNET_SERVER_XDP
        server_transport->state = ROCKETNET_SERVER_STOPPED;
        return;
    }

    const double current_time = rocketnet_platform_time();
    const uint64_t current_timestamp = (uint64_t) time( NULL );

    server_transport->payloads.num_payloads = 0;

    // receive and process, responses sealed straight into send frames

#if ROCKETNET_SERVER_XDP

    rocketnet_server_socket_receive_packets( server_transport->xdp_socket );

    struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_socket_process_packets( server_transport->xdp_socket );

    for ( int i = 0; i < received->num_packets; i++ )
    {
        uint8_t * payload = server_transport->payloads.num_payloads < ROCKETNET_SERVER_MAX_RECEIVE_PACKETS ?
            server_transport->payload_storage + server_transport->payloads.num_payloads * ROCKETNET_FRAME_SIZE : NULL;

        uint8_t payload_overflow[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        int payload_client_index = -1;

        struct rocketnet_server_frame_t frame;
        uint8_t * response = rocketnet_server_reserve_send_frame( server_transport, &received->from[i], &frame );
        uint8_t response_overflow[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;

        rocketnet_server_protocol_process_packet( &server_transport->protocol, &received->from[i],
                                         received->packet_data[i], received->packet_bytes[i],
                                         current_timestamp, current_time,
                                         response ? response : response_overflow, &response_bytes,
                                         payload ? payload : payload_overflow, &payload_bytes, &payload_client_index );

        if ( response )
        {
            if ( response_bytes > 0 )
                rocketnet_server_commit_send_frame( server_transport, &frame, response_bytes );
            else
                rocketnet_server_abort_send_frame( server_transport, &frame );
        }

        if ( payload && payload_bytes > 0 )
        {
            const int index = server_transport->payloads.num_payloads;
            server_transport->payloads.client_index[index] = payload_client_index;
            server_transport->payloads.payload_data[index] = payload;
            server_transport->payloads.payload_bytes[index] = payload_bytes;
            server_transport->payloads.num_payloads++;
        }
    }

#else // #if ROCKETNET_SERVER_XDP

    uint8_t receive_frame[ROCKETNET_RECEIVE_CEILING];

    for ( int i = 0; i < ROCKETNET_SERVER_MAX_RECEIVE_PACKETS; i++ )
    {
        struct rocketnet_address_t from;
        int packet_bytes = rocketnet_platform_socket_receive_packet( server_transport->socket, &from, receive_frame, sizeof(receive_frame) );
        if ( packet_bytes == 0 )
            break;

        uint8_t * payload = server_transport->payloads.num_payloads < ROCKETNET_SERVER_MAX_RECEIVE_PACKETS ?
            server_transport->payload_storage + server_transport->payloads.num_payloads * ROCKETNET_FRAME_SIZE : NULL;

        uint8_t payload_overflow[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        int payload_client_index = -1;

        struct rocketnet_server_frame_t frame;
        uint8_t * response = rocketnet_server_reserve_send_frame( server_transport, &from, &frame );
        uint8_t response_overflow[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;

        rocketnet_server_protocol_process_packet( &server_transport->protocol, &from, receive_frame, packet_bytes,
                                         current_timestamp, current_time,
                                         response ? response : response_overflow, &response_bytes,
                                         payload ? payload : payload_overflow, &payload_bytes, &payload_client_index );

        if ( response )
        {
            if ( response_bytes > 0 )
                rocketnet_server_commit_send_frame( server_transport, &frame, response_bytes );
            else
                rocketnet_server_abort_send_frame( server_transport, &frame );
        }

        if ( payload && payload_bytes > 0 )
        {
            const int index = server_transport->payloads.num_payloads;
            server_transport->payloads.client_index[index] = payload_client_index;
            server_transport->payloads.payload_data[index] = payload;
            server_transport->payloads.payload_bytes[index] = payload_bytes;
            server_transport->payloads.num_payloads++;
        }
    }

#endif // #if ROCKETNET_SERVER_XDP

    // the sweep, and the paced keep-alives into send frames

    rocketnet_server_protocol_update( &server_transport->protocol, current_time );

    const int max_clients = rocketnet_client_slots_max_clients( server_transport->protocol.client_slots );
    for ( int i = 0; i < max_clients; i++ )
    {
        if ( !rocketnet_client_slots_connected( server_transport->protocol.client_slots, i ) )
            continue;
        int mapping_index = rocketnet_client_slots_mapping_index( server_transport->protocol.client_slots, i );
        const rocketnet_address_t * client_address = rocketnet_encryption_map_address( server_transport->protocol.encryption_map, mapping_index );
        struct rocketnet_server_frame_t frame;
        uint8_t * frame_data = rocketnet_server_reserve_send_frame( server_transport, client_address, &frame );
        if ( !frame_data )
            break;
        int keep_alive_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &server_transport->protocol, i, current_time, frame_data );
        if ( keep_alive_bytes > 0 )
            rocketnet_server_commit_send_frame( server_transport, &frame, keep_alive_bytes );
        else
            rocketnet_server_abort_send_frame( server_transport, &frame );
    }

    // flush

#if ROCKETNET_SERVER_XDP

    rocketnet_server_socket_send_packets( server_transport->xdp_socket );

#else // #if ROCKETNET_SERVER_XDP

    for ( int i = 0; i < server_transport->send_buffer.num_packets; i++ )
    {
        if ( server_transport->send_buffer.packet_bytes[i] > 0 )
        {
            rocketnet_platform_socket_send_packet( server_transport->socket,
                                                   &server_transport->send_buffer.to[i],
                                                   server_transport->send_buffer.data + i * ROCKETNET_FRAME_SIZE,
                                                   server_transport->send_buffer.packet_bytes[i] );
        }
    }
    server_transport->send_buffer.num_packets = 0;

#endif // #if ROCKETNET_SERVER_XDP
}

struct rocketnet_server_payloads_t * rocketnet_server_payloads( struct rocketnet_server_t * server_transport )
{
    rocketnet_assert( server_transport );
    return &server_transport->payloads;
}

void rocketnet_server_send_packet( struct rocketnet_server_t * server_transport,
                                             int client_index,
                                             const uint8_t * payload_data,
                                             int payload_bytes )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( payload_data );

    if ( !rocketnet_client_slots_connected( server_transport->protocol.client_slots, client_index ) )
        return;

    int mapping_index = rocketnet_client_slots_mapping_index( server_transport->protocol.client_slots, client_index );
    const rocketnet_address_t * client_address = rocketnet_encryption_map_address( server_transport->protocol.encryption_map, mapping_index );

    struct rocketnet_server_frame_t frame;
    uint8_t * frame_data = rocketnet_server_reserve_send_frame( server_transport, client_address, &frame );
    if ( !frame_data )
        return;

    // sealed in place into the send frame -- no copy between the seal and
    // the socket

    int wire_bytes = rocketnet_server_protocol_write_payload( &server_transport->protocol, client_index, payload_data, payload_bytes,
                                                     rocketnet_platform_time(), frame_data );
    if ( wire_bytes > 0 )
        rocketnet_server_commit_send_frame( server_transport, &frame, wire_bytes );
    else
        rocketnet_server_abort_send_frame( server_transport, &frame );
}

void rocketnet_server_disconnect_client( struct rocketnet_server_t * server_transport, int client_index )
{
    rocketnet_assert( server_transport );

    if ( !rocketnet_client_slots_connected( server_transport->protocol.client_slots, client_index ) )
        return;

    // the address must be taken before the protocol call: the disconnect
    // releases the mapping that holds it

    int mapping_index = rocketnet_client_slots_mapping_index( server_transport->protocol.client_slots, client_index );
    rocketnet_address_t client_address = *rocketnet_encryption_map_address( server_transport->protocol.encryption_map, mapping_index );

    uint8_t burst[ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS * ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES];
    int burst_count = 0;
    rocketnet_server_protocol_disconnect_client( &server_transport->protocol, client_index, burst, &burst_count );

    for ( int i = 0; i < burst_count; i++ )
    {
        struct rocketnet_server_frame_t frame;
        uint8_t * frame_data = rocketnet_server_reserve_send_frame( server_transport, &client_address, &frame );
        if ( !frame_data )
            break;
        memcpy( frame_data, burst + i * ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES, ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES );
        rocketnet_server_commit_send_frame( server_transport, &frame, ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES );
    }
}

int rocketnet_server_num_connected_clients( struct rocketnet_server_t * server_transport )
{
    rocketnet_assert( server_transport );
    return rocketnet_client_slots_num_connected( server_transport->protocol.client_slots );
}

int rocketnet_server_is_client_connected( struct rocketnet_server_t * server_transport, int client_index )
{
    rocketnet_assert( server_transport );
    return rocketnet_client_slots_connected( server_transport->protocol.client_slots, client_index );
}

uint64_t rocketnet_server_client_id( struct rocketnet_server_t * server_transport, int client_index )
{
    rocketnet_assert( server_transport );
    return rocketnet_client_slots_client_id( server_transport->protocol.client_slots, client_index );
}

const struct rocketnet_counters_t * rocketnet_server_counters( struct rocketnet_server_t * server_transport )
{
    rocketnet_assert( server_transport );
    return &server_transport->counters;
}
