/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "test_unit_registry.h"
#include "portable_crt.h"
#include "unit_shutdown.h"
#include "token_string.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_link_mtu.h"
#include "../sdk/rocketnet_replay.h"
#include "../sdk/rocketnet_protocol.h"
#include "../sdk/rocketnet_driver_loopback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loopback_hub.h"

#define check rocketnet_unit_check

// Each connection uses its own packet size P above the handshake (standard
// TRANSPORT.md, the packet budget; MESSAGES.md m2.2, the block stride F(P)).
// A pair whose path carries only 548-byte datagrams settles on P = 548 and,
// once its probe walk has ended, never hands the link a larger datagram,
// whatever it sends; a default pair
// keeps the ceiling's values, 1452 and 1411.

#if ROCKETNET_DEV

static double packet_size_now = 1000.0;
static double packet_size_clock( void * ) { return packet_size_now; }

static void packet_size_pump( rocketnet_server_t * server, rocketnet_client_t * client )
{
    rocketnet_client_update( client, packet_size_now );
    rocketnet_client_send_packets( client );
    rocketnet_server_update( server, packet_size_now );
    rocketnet_server_send_packets( server );
    packet_size_now += 0.01;
}

struct packet_size_pair_t
{
    rocketnet_server_t * server;
    rocketnet_client_t * client;
    int max_message_bytes;
};

// a step-driven loopback pair; small is the three-step link list with the
// window raised to hold one max_message_bytes message at P = 548 (F = 480)
static bool packet_size_pair_create( packet_size_pair_t & pair, bool small_size )
{
    rocketnet_quiet( 1 );
    packet_size_now = 1000.0;

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x5A ^ i );
    }
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    static const int link_mtus[3] = { 1492, 1200, 576 };

    const uint64_t protocol_id = 0x7A11C1C1E5EED548ULL;
    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public, sizeof( backend_public ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    if ( small_size )
    {
        memcpy( server_config.link_mtus, link_mtus, sizeof( link_mtus ) );
        server_config.num_link_mtus = 3;
        server_config.reliable_messages_in_flight = 4096;
        server_config.packets_in_flight = 4096;
    }
    pair.server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( pair.server != NULL );
    check( rocketnet_server_start( pair.server ) == ROCKETNET_STATUS_OK );

    rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = true;
    client_config.time_callback = packet_size_clock;
    if ( small_size )
    {
        memcpy( client_config.link_mtus, link_mtus, sizeof( link_mtus ) );
        client_config.num_link_mtus = 3;
        client_config.reliable_messages_in_flight = 4096;
        client_config.packets_in_flight = 4096;
    }
    pair.max_message_bytes = client_config.max_message_bytes;
    pair.client = rocketnet_client_create( NULL, &client_config, NULL );
    check( pair.client != NULL );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( pair.server ) );
    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( backend_secret, pair.server, protocol_id, addresses, 1, 42, 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rn_test_client_connect_after_completion( pair.client, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 4000 && ( rocketnet_client_state( pair.client ) != ROCKETNET_CLIENT_STATE_CONNECTED || !rocketnet_server_client_connected( pair.server, 0 ) ); i++ )
    {
        packet_size_pump( pair.server, pair.client );
    }
    return rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED && rocketnet_server_client_connected( pair.server, 0 );
}

static void packet_size_pair_destroy( packet_size_pair_t & pair )
{
    rn_test_client_stop_and_destroy( pair.client );
    rn_test_server_stop_and_destroy( pair.server );
    rocketnet_loopback_test_link_max_bytes( 0 );
}

static void packet_size_fill( uint8_t * data, int bytes, int salt )
{
    for ( int i = 0; i < bytes; i++ )
    {
        data[i] = (uint8_t) ( i * 13 + salt );
    }
}

static bool packet_size_same( const uint8_t * data, int bytes, int salt )
{
    for ( int i = 0; i < bytes; i++ )
    {
        if ( data[i] != (uint8_t) ( i * 13 + salt ) )
        {
            return false;
        }
    }
    return true;
}

// the client sends one message; pumps until the server receives it whole
static bool packet_size_client_to_server( packet_size_pair_t & pair, enum rocketnet_reliability_t reliability, int bytes, int salt )
{
    uint8_t * out = rocketnet_client_start_message( pair.client, 0, reliability, bytes );
    if ( !out )
    {
        return false;
    }
    packet_size_fill( out, bytes, salt );
    if ( rocketnet_client_finish_message( pair.client, bytes ) == 0 )
    {
        return false;
    }

    bool whole = false;
    for ( int tick = 0; tick < 20000 && !whole; tick++ )
    {
        packet_size_pump( pair.server, pair.client );
        const rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( pair.server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                if ( batch->bytes[i] == bytes && packet_size_same( batch->data[i], bytes, salt ) )
                {
                    whole = true;
                }
            }
        }
        while ( rocketnet_client_receive_messages( pair.client ) != NULL )
        {
        }
    }
    return whole;
}

// the server sends one message to client 0; pumps until the client receives it whole
static bool packet_size_server_to_client( packet_size_pair_t & pair, enum rocketnet_reliability_t reliability, int bytes, int salt )
{
    uint8_t * out = rocketnet_server_start_message( pair.server, 0, 0, reliability, bytes );
    if ( !out )
    {
        return false;
    }
    packet_size_fill( out, bytes, salt );
    if ( rocketnet_server_finish_message( pair.server, bytes ) == 0 )
    {
        return false;
    }

    bool whole = false;
    for ( int tick = 0; tick < 20000 && !whole; tick++ )
    {
        packet_size_pump( pair.server, pair.client );
        const rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_client_receive_messages( pair.client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                if ( batch->bytes[i] == bytes && packet_size_same( batch->data[i], bytes, salt ) )
                {
                    whole = true;
                }
            }
        }
        while ( rocketnet_server_receive_messages( pair.server ) != NULL )
        {
        }
    }
    return whole;
}

static void test_link_mtu_packet_size_548_pair()
{
    // the path carries 548-byte datagrams and nothing larger: the probe
    // settles on the 576 entry, P = 576 - 28
    rocketnet_loopback_test_link_max_bytes( 548 );

    packet_size_pair_t pair;
    check( packet_size_pair_create( pair, true ) );

    // the four calls name the connection's P and C(P) - 5 at both ends
    const int P = 548;
    const int C = rocketnet_link_mtu_payload_bytes( P );
    check( C == 512 );
    check( rocketnet_client_max_packet_bytes( pair.client ) == P );
    check( rocketnet_server_client_max_packet_bytes( pair.server, 0 ) == P );
    check( rocketnet_client_max_unreliable_message_bytes( pair.client ) == C - 5 );
    check( rocketnet_server_client_max_unreliable_message_bytes( pair.server, 0 ) == C - 5 );
    check( rocketnet_link_mtu_block_stride( P ) == 480 );

    // the transports name the same P and C(P), and refuse a packet request
    // above C(P) before taking a slot or a sequence
    struct rocketnet_client_transport_t * client_transport = rocketnet_message_client_transport( rocketnet_client_message_client( pair.client ) );
    struct rocketnet_server_transport_t * server_transport = rocketnet_message_server_transport( rocketnet_server_message_server( pair.server ) );
    check( rocketnet_client_transport_packet_bytes( client_transport ) == P );
    check( rocketnet_client_transport_max_payload_bytes( client_transport ) == C );
    check( rocketnet_server_transport_client_packet_bytes( server_transport, 0 ) == P );
    check( rocketnet_server_transport_client_max_payload_bytes( server_transport, 0 ) == C );
    struct rocketnet_packet_context_t context;
    check( rocketnet_client_transport_start_packet( client_transport, C + 1, &context ) == NULL );
    check( rocketnet_server_transport_start_packet( server_transport, 0, C + 1, &context ) == NULL );

    // the path probe's walk ends first: it sends large probes above P, which
    // the link drops, until 1452 and 1172 are ruled out and the 548 pair
    // comes back; it never probes again
    const rocketnet_counters_t * transport_counters = rocketnet_client_transport_counters( client_transport );
    for ( int i = 0; i < 6000 && rocketnet_counter_value( transport_counters, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT ) < 2; i++ )
    {
        packet_size_pump( pair.server, pair.client );
    }
    check( rocketnet_counter_value( transport_counters, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT ) == 2 );
    for ( int i = 0; i < 300; i++ )
    {
        packet_size_pump( pair.server, pair.client );
    }
    const uint64_t probes_sent = rocketnet_counter_value( transport_counters, ROCKETNET_COUNTER_CLIENT_PROBE_SENT );
    check( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_max_packet_bytes( pair.client ) == P );

    // from here no endpoint hands the link a datagram above P: the seam
    // zeroes the largest seen and still drops anything larger, so an
    // oversized datagram would also stall the transfers below
    rocketnet_loopback_test_link_max_bytes( P );

    // reliable messages much larger than a block, and of max_message_bytes,
    // arrive whole both ways
    check( packet_size_client_to_server( pair, ROCKETNET_RELIABILITY_RELIABLE, 65536, 1 ) );
    check( packet_size_server_to_client( pair, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 65536, 2 ) );
    check( packet_size_client_to_server( pair, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, pair.max_message_bytes, 3 ) );
    check( packet_size_server_to_client( pair, ROCKETNET_RELIABILITY_RELIABLE, pair.max_message_bytes, 4 ) );

    // an unreliable message of exactly the connection's limit fits one
    // packet's record and is not promoted; one byte more is
    const int limit = rocketnet_client_max_unreliable_message_bytes( pair.client );
    const rocketnet_counters_t * counters = rocketnet_message_client_counters( rocketnet_client_message_client( pair.client ) );
    const uint64_t before = rocketnet_counter_value( counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE );
    check( packet_size_client_to_server( pair, ROCKETNET_RELIABILITY_UNRELIABLE, limit, 5 ) );
    check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE ) == before );
    check( packet_size_client_to_server( pair, ROCKETNET_RELIABILITY_UNRELIABLE, limit + 1, 6 ) );
    check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE ) == before + 1 );

    // the same at the server, whose transport payload maximum is the ceiling
    // for any client: its record limit is the connection's
    const int server_limit = rocketnet_server_client_max_unreliable_message_bytes( pair.server, 0 );
    const rocketnet_counters_t * server_counters = rocketnet_message_server_counters( rocketnet_server_message_server( pair.server ) );
    const uint64_t server_before = rocketnet_counter_value( server_counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE );
    check( packet_size_server_to_client( pair, ROCKETNET_RELIABILITY_UNRELIABLE, server_limit, 10 ) );
    check( rocketnet_counter_value( server_counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE ) == server_before );
    check( packet_size_server_to_client( pair, ROCKETNET_RELIABILITY_UNRELIABLE, server_limit + 1, 11 ) );
    check( rocketnet_counter_value( server_counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE ) == server_before + 1 );

    // and for UNRELIABLE_SEQUENCED on a channel below 256, whose one-record
    // limit is 11 below C(P) (1405 at the ceiling)
    const uint64_t sequenced_before = rocketnet_counter_value( server_counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED );
    check( packet_size_server_to_client( pair, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, C - 11, 12 ) );
    check( rocketnet_counter_value( server_counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED ) == sequenced_before );
    check( packet_size_server_to_client( pair, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, C - 10, 13 ) );
    check( rocketnet_counter_value( server_counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED ) == sequenced_before + 1 );

    // the largest datagram handed the link since connect is P at most, and
    // the transfers above filled packets up to it
    const int largest = rocketnet_loopback_test_largest_sent();
    check( largest <= P );
    check( largest > P - 16 );
    check( rocketnet_counter_value( transport_counters, ROCKETNET_COUNTER_CLIENT_PROBE_SENT ) == probes_sent );

    packet_size_pair_destroy( pair );
}

REGISTER_UNIT_TEST( test_link_mtu_packet_size_548_pair );

static void test_link_mtu_packet_size_default_pair()
{
    rocketnet_loopback_test_link_max_bytes( 0 );

    packet_size_pair_t pair;
    check( packet_size_pair_create( pair, false ) );

    // the ceiling's values: P = 1452, C = 1416, F = 1152, limit 1411
    check( rocketnet_client_max_packet_bytes( pair.client ) == ROCKETNET_MAX_PACKET_BYTES );
    check( rocketnet_server_client_max_packet_bytes( pair.server, 0 ) == ROCKETNET_MAX_PACKET_BYTES );
    check( rocketnet_client_max_unreliable_message_bytes( pair.client ) == ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES );
    check( rocketnet_server_client_max_unreliable_message_bytes( pair.server, 0 ) == ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES );
    check( ROCKETNET_MAX_PACKET_BYTES == 1452 );
    check( ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES == 1411 );
    check( rocketnet_link_mtu_payload_bytes( ROCKETNET_MAX_PACKET_BYTES ) == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    check( rocketnet_link_mtu_block_stride( ROCKETNET_MAX_PACKET_BYTES ) == ROCKETNET_MESSAGE_FRAGMENT_BYTES );

    rocketnet_loopback_test_link_max_bytes( ROCKETNET_MAX_PACKET_BYTES );
    check( packet_size_client_to_server( pair, ROCKETNET_RELIABILITY_RELIABLE, 65536, 7 ) );
    const int limit = rocketnet_client_max_unreliable_message_bytes( pair.client );
    const rocketnet_counters_t * counters = rocketnet_message_client_counters( rocketnet_client_message_client( pair.client ) );
    const uint64_t before = rocketnet_counter_value( counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE );
    check( packet_size_client_to_server( pair, ROCKETNET_RELIABILITY_UNRELIABLE, limit, 8 ) );
    check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE ) == before );
    check( packet_size_client_to_server( pair, ROCKETNET_RELIABILITY_UNRELIABLE, limit + 1, 9 ) );
    check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE ) == before + 1 );

    // a default connection still fills packets to the ceiling
    const int largest = rocketnet_loopback_test_largest_sent();
    check( largest <= ROCKETNET_MAX_PACKET_BYTES );
    check( largest > ROCKETNET_MAX_PACKET_BYTES - 16 );

    packet_size_pair_destroy( pair );
}

REGISTER_UNIT_TEST( test_link_mtu_packet_size_default_pair );

static void test_link_mtu_packet_size_ack_block_budget()
{
    // the ack block fits the connection's packet: at P = 548 a full window's
    // block (up to 1027 bytes) is cut to the packet's room, and at the
    // ceiling the cut is today's
    struct rocketnet_replay_protection_t accepted;
    memset( &accepted, 0, sizeof( accepted ) );
    for ( uint64_t sequence = 1; sequence < 4096; sequence += 3 )
    {
        rocketnet_replay_advance( &accepted, sequence );
    }

    uint8_t block[ROCKETNET_MAX_PACKET_BYTES];
    const int other = 100;
    const int small_block = rocketnet_session_ack_block_build( block, 548, other, &accepted );
    check( small_block > 0 );
    check( small_block <= 548 - ROCKETNET_PACKET_OVERHEAD_BYTES - other );
    const int large = rocketnet_session_ack_block_build( block, ROCKETNET_MAX_PACKET_BYTES, other, &accepted );
    check( large > small_block );
    check( large <= ROCKETNET_MAX_PACKET_BYTES - ROCKETNET_PACKET_OVERHEAD_BYTES - other );
}

REGISTER_UNIT_TEST( test_link_mtu_packet_size_ack_block_budget );

#endif // #if ROCKETNET_DEV
