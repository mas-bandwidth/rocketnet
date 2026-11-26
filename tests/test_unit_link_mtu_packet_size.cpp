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
#include "../sdk/rocketnet_client_protocol.h"
#include "../sdk/rocketnet_driver_loopback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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

    // the path probe's walk settles first: it sends large probes at 1172,
    // the first step up, which the link drops, until 1172 is ruled out and
    // the 548 pair comes back; from then it only re-tries 1172 on its
    // schedule, 10 s and more after the rule-out
    const rocketnet_counters_t * transport_counters = rocketnet_client_transport_counters( client_transport );
    for ( int i = 0; i < 6000 && rocketnet_counter_value( transport_counters, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT ) < 1; i++ )
    {
        packet_size_pump( pair.server, pair.client );
    }
    check( rocketnet_counter_value( transport_counters, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT ) == 1 );
    for ( int i = 0; i < 300; i++ )
    {
        packet_size_pump( pair.server, pair.client );
    }
    const uint64_t probes_sent = rocketnet_counter_value( transport_counters, ROCKETNET_COUNTER_CLIENT_PROBE_SENT );
    check( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_max_packet_bytes( pair.client ) == P );

    // from here no endpoint hands the link a datagram above P but a
    // re-try's large probe: the seam zeroes its counts and still drops
    // anything larger, so an oversized datagram would also stall the
    // transfers below
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

    // the largest datagram the link carried since connect is P at most, and
    // the transfers above filled packets up to it; every datagram handed the
    // link above P is the large probe of a re-try's pair, at 1172, one per
    // pair sent (the counter counts both of a pair, and a confirm, of which
    // none is sent here), and nothing else is
    const uint64_t retry_sent = rocketnet_counter_value( transport_counters, ROCKETNET_COUNTER_CLIENT_PROBE_SENT ) - probes_sent;
    check( retry_sent % 2 == 0 );
    const uint64_t retry_pairs = retry_sent / 2;
    const int carried = rocketnet_loopback_test_largest_carried();
    check( carried <= P );
    check( carried > P - 16 );
    check( (uint64_t) rocketnet_loopback_test_dropped_above() == retry_pairs );
    check( rocketnet_loopback_test_largest_sent() == ( retry_pairs == 0 ? carried : 1200 - 28 ) );

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

// ---------------------------------------------------------------------------
// slot reuse: a connection's probe state never reaches the next connection
// (proofs/tla/RNPathProbe.tla Reuse, ReuseStartsFresh). The client protocol's
// rocketnet_client_connection_reset and the connect clear the walk's every
// field (docs/CONNECTION-RESET.md); the server frees the slot's mapping and
// the next connection's handshake sets the slot's P to its P0. Each case
// ends one connection and starts the next in the same server slot (one slot:
// max_clients 1), either from a new client or from the same client object,
// over a path of another size, and reads both ends after every step.

struct reuse_world_t
{
    rocketnet_server_t * server;
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint64_t protocol_id;
    uint64_t client_id;
    int max_message_bytes;
};

static void reuse_pump( reuse_world_t & world, rocketnet_client_t * client )
{
    packet_size_pump( world.server, client );
    while ( rocketnet_server_receive_messages( world.server ) != NULL )
    {
    }
    while ( rocketnet_client_receive_messages( client ) != NULL )
    {
    }
}

static void reuse_world_create( reuse_world_t & world )
{
    rocketnet_quiet( 1 );
    packet_size_now = 1000.0;
    rocketnet_loopback_test_link_max_bytes( 0 );

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x3C ^ i );
    }
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, world.backend_secret ) == ROCKETNET_STATUS_OK );

    static const int link_mtus[3] = { 1492, 1200, 576 };
    world.protocol_id = 0x7A11C1C1E5EED577ULL;
    world.client_id = 100;
    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = world.protocol_id;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public, sizeof( backend_public ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    memcpy( server_config.link_mtus, link_mtus, sizeof( link_mtus ) );
    server_config.num_link_mtus = 3;
    server_config.reliable_messages_in_flight = 4096;
    server_config.packets_in_flight = 4096;
    world.server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( world.server != NULL );
    check( rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );
}

// a step-driven client on the three-step list
static rocketnet_client_t * reuse_client_create( reuse_world_t & world )
{
    static const int link_mtus[3] = { 1492, 1200, 576 };
    rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = true;
    client_config.time_callback = packet_size_clock;
    memcpy( client_config.link_mtus, link_mtus, sizeof( link_mtus ) );
    client_config.num_link_mtus = 3;
    client_config.reliable_messages_in_flight = 4096;
    client_config.packets_in_flight = 4096;
    world.max_message_bytes = client_config.max_message_bytes;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
    check( client != NULL );
    return client;
}

// the packet sizes both ends of slot 0 report, and the largest datagram the
// server handed the link, read after every step of a connection: none may
// be above ceiling, at either end, from the first step
struct reuse_watch_t
{
    int ceiling;
    bool above;
    int client_largest;
    int server_largest;
};

static void reuse_watch( reuse_world_t & world, rocketnet_client_t * client, reuse_watch_t & watch )
{
    const int client_bytes = rocketnet_client_max_packet_bytes( client );
    const int server_bytes = rocketnet_server_client_connected( world.server, 0 ) ? rocketnet_server_client_max_packet_bytes( world.server, 0 ) : 0;
    watch.client_largest = client_bytes > watch.client_largest ? client_bytes : watch.client_largest;
    watch.server_largest = server_bytes > watch.server_largest ? server_bytes : watch.server_largest;
    if ( client_bytes > watch.ceiling || server_bytes > watch.ceiling || rocketnet_loopback_test_largest_server_sent() > watch.ceiling )
    {
        watch.above = true;
    }
}

// every probe field of the client's protocol at the first step its
// connection is connected (proofs/tla/RNPathProbe.tla ReuseStartsFresh): a
// connection in a reused slot, or the same client connecting again, must
// start exactly as the first connection did. The next pair's time is taken
// from the step the keep-alive arrived in, on the client's own clock; the
// other times are kept whole (probe_retry_time and confirm_next_time are
// zero until a rule-out or a raise sets them)
struct reuse_start_t
{
    bool taken;
    int probe_candidates[ROCKETNET_LINK_MTU_MAX_ENTRIES];
    int num_probe_candidates;
    int probe_index;
    int probe_strikes;
    uint32_t probe_pair;
    int probe_pair_small;
    int probe_pair_large;
    int probe_pair_out;
    double probe_next_in;
    double probe_interval;
    int probing;
    int probe_ruled;
    int probe_waiting;
    int probe_retries;
    double probe_retry_time;
    int confirm_bytes;
    double confirm_next_time;
    int packet_bytes;
};

static reuse_start_t reuse_start_read( rocketnet_client_t * client )
{
    const rocketnet_client_protocol_t * p = rocketnet_client_transport_protocol_for_test(
        rocketnet_message_client_transport( rocketnet_client_message_client( client ) ) );
    reuse_start_t start;
    memset( &start, 0, sizeof( start ) );
    start.taken = true;
    memcpy( start.probe_candidates, p->probe_candidates, sizeof( start.probe_candidates ) );
    start.num_probe_candidates = p->num_probe_candidates;
    start.probe_index = p->probe_index;
    start.probe_strikes = p->probe_strikes;
    start.probe_pair = p->probe_pair;
    start.probe_pair_small = p->probe_pair_small;
    start.probe_pair_large = p->probe_pair_large;
    start.probe_pair_out = p->probe_pair_out;
    // the client's own clock: the keep-alive that connected it was received
    // in this step
    start.probe_next_in = p->probe_next_time - p->last_receive_time;
    start.probe_interval = p->probe_interval;
    start.probing = p->probing;
    start.probe_ruled = p->probe_ruled;
    start.probe_waiting = p->probe_waiting;
    start.probe_retries = p->probe_retries;
    start.probe_retry_time = p->probe_retry_time;
    start.confirm_bytes = p->confirm_bytes;
    start.confirm_next_time = p->confirm_next_time;
    start.packet_bytes = p->packet_bytes;
    return start;
}

// the fields that differ, printed; true when none does
static bool reuse_start_same( const reuse_start_t & a, const reuse_start_t & b )
{
    bool same = a.taken && b.taken;
#define REUSE_SAME( field )                                                     \
    if ( !( a.field == b.field ) )                                             \
    {                                                                          \
        printf( "reuse: %s differs from the first connection's\n", #field );  \
        same = false;                                                          \
    }
    if ( memcmp( a.probe_candidates, b.probe_candidates, sizeof( a.probe_candidates ) ) != 0 )
    {
        printf( "reuse: probe_candidates differs from the first connection's\n" );
        same = false;
    }
    REUSE_SAME( num_probe_candidates )
    REUSE_SAME( probe_index )
    REUSE_SAME( probe_strikes )
    REUSE_SAME( probe_pair )
    REUSE_SAME( probe_pair_small )
    REUSE_SAME( probe_pair_large )
    REUSE_SAME( probe_pair_out )
    // the next pair's time from the step, to within rounding (the step's
    // clock is a sum of 10 ms steps)
    if ( fabs( a.probe_next_in - b.probe_next_in ) > 1e-9 )
    {
        printf( "reuse: probe_next_in differs from the first connection's (%.9f, %.9f)\n", a.probe_next_in, b.probe_next_in );
        same = false;
    }
    REUSE_SAME( probe_interval )
    REUSE_SAME( probing )
    REUSE_SAME( probe_ruled )
    REUSE_SAME( probe_waiting )
    REUSE_SAME( probe_retries )
    REUSE_SAME( probe_retry_time )
    REUSE_SAME( confirm_bytes )
    REUSE_SAME( confirm_next_time )
    REUSE_SAME( packet_bytes )
#undef REUSE_SAME
    return same;
}

// a new token, the connect, and steps until both ends hold slot 0; the
// watch reads both ends after every step, and start is the client's probe
// state at the first step it is connected
static bool reuse_connect( reuse_world_t & world, rocketnet_client_t * client, reuse_watch_t & watch, reuse_start_t & start )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world.server ) );
    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( world.backend_secret, world.server, world.protocol_id, addresses, 1, world.client_id++, 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rn_test_client_connect_after_completion( client, token ) == ROCKETNET_STATUS_OK );
    start.taken = false;
    for ( int i = 0; i < 4000 && ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED || !rocketnet_server_client_connected( world.server, 0 ) ); i++ )
    {
        reuse_pump( world, client );
        reuse_watch( world, client, watch );
        if ( !start.taken && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            start = reuse_start_read( client );
        }
    }
    return rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED && rocketnet_server_client_connected( world.server, 0 ) &&
           rocketnet_client_index( client ) == 0 && start.taken;
}

// steps until both ends of slot 0 are at bytes; the time it took, -1 if not
static double reuse_climb( reuse_world_t & world, rocketnet_client_t * client, int bytes, reuse_watch_t & watch )
{
    const double start = packet_size_now;
    for ( int i = 0; i < 2000; i++ )
    {
        if ( rocketnet_client_max_packet_bytes( client ) == bytes && rocketnet_server_client_max_packet_bytes( world.server, 0 ) == bytes )
        {
            return packet_size_now - start;
        }
        reuse_pump( world, client );
        reuse_watch( world, client, watch );
    }
    return -1.0;
}

// the client disconnects; steps until the server has freed slot 0
static void reuse_disconnect( reuse_world_t & world, rocketnet_client_t * client )
{
    rocketnet_client_disconnect( client );
    for ( int i = 0; i < 400 && rocketnet_server_client_connected( world.server, 0 ); i++ )
    {
        reuse_pump( world, client );
    }
    check( !rocketnet_server_client_connected( world.server, 0 ) );
    rn_test_client_wait_disconnected( client );
}

static rocketnet_counters_t const * reuse_transport_counters( rocketnet_client_t * client )
{
    return rocketnet_client_transport_counters( rocketnet_message_client_transport( rocketnet_client_message_client( client ) ) );
}

// a connection at 1452 is followed in its slot by one on a path that
// carries only 548-byte datagrams (the 576 entry): the new connection starts
// at P0 = 548 at both ends and nothing at either end, nor any datagram the
// server sends, is above 548 at any step; its probe state at its first
// connected step is the first connection's, field by field; reliable
// messages cross it both ways. new_client: the next connection is a new client object, else the
// same client object connects again
static void reuse_full_then_small( bool new_client )
{
    reuse_world_t world;
    reuse_world_create( world );
    rocketnet_client_t * client = reuse_client_create( world );

    // the first connection ends in the step its client raises to 1452,
    // before the server's echo of the confirm reaches it: the client's
    // confirm of 1452 is still unanswered when the connection ends
    // (the server slot may still be below 1452 here: a server slot that keeps
    // its P across a reuse is caught by reuse_ruled_out_then_full, whose
    // slot holds 1172 before the full path's connection starts at 548)
    reuse_watch_t first = { ROCKETNET_MAX_PACKET_BYTES, false, 0, 0 };
    reuse_start_t first_start;
    check( reuse_connect( world, client, first, first_start ) );
    for ( int i = 0; i < 400 && rocketnet_client_max_packet_bytes( client ) < ROCKETNET_MAX_PACKET_BYTES; i++ )
    {
        reuse_pump( world, client );
    }
    check( rocketnet_client_max_packet_bytes( client ) == ROCKETNET_MAX_PACKET_BYTES );
    reuse_disconnect( world, client );
    if ( new_client )
    {
        rn_test_client_stop_and_destroy( client );
        client = reuse_client_create( world );
    }

    // the path now carries 548 and nothing larger; the seam's counts start
    // here
    const int P = 548;
    rocketnet_loopback_test_link_max_bytes( P );
    reuse_watch_t watch = { P, false, 0, 0 };
    reuse_start_t start;
    check( reuse_connect( world, client, watch, start ) );
    check( reuse_start_same( start, first_start ) );
    check( rocketnet_client_max_packet_bytes( client ) == P );
    check( rocketnet_server_client_max_packet_bytes( world.server, 0 ) == P );
    check( rocketnet_client_link_mtu( client ) == 576 );
    check( rocketnet_server_client_link_mtu( world.server, 0 ) == 576 );

    // the walk runs on the new path: 1172 is ruled out and P0 answered
    const rocketnet_counters_t * counters = reuse_transport_counters( client );
    const uint64_t ruled_before = rocketnet_counter_value( counters, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT );
    for ( int i = 0; i < 600 && rocketnet_counter_value( counters, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT ) == ruled_before; i++ )
    {
        reuse_pump( world, client );
        reuse_watch( world, client, watch );
    }
    check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT ) == ruled_before + 1 );

    // reliable messages larger than a block cross both ways at P
    packet_size_pair_t pair = { world.server, client, world.max_message_bytes };
    check( packet_size_client_to_server( pair, ROCKETNET_RELIABILITY_RELIABLE, 65536, 21 ) );
    reuse_watch( world, client, watch );
    check( packet_size_server_to_client( pair, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 65536, 22 ) );
    reuse_watch( world, client, watch );

    check( !watch.above );
    check( watch.client_largest == P );
    check( watch.server_largest == P );
    check( rocketnet_loopback_test_largest_server_sent() <= P );
    check( rocketnet_loopback_test_largest_server_sent() > P - 16 );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( world.server );
    rocketnet_loopback_test_link_max_bytes( 0 );
}

static void test_link_mtu_reused_slot_new_client_starts_small()
{
    reuse_full_then_small( true );
}

REGISTER_UNIT_TEST( test_link_mtu_reused_slot_new_client_starts_small );

static void test_link_mtu_reconnected_client_starts_small()
{
    reuse_full_then_small( false );
}

REGISTER_UNIT_TEST( test_link_mtu_reconnected_client_starts_small );

// a connection on a 1200 link has ruled 1452 out and waits for its re-try,
// 10 s off; it ends, and the next connection in its slot is on a path that
// carries every size: it climbs to 1452 as fast as a first connection does
// on that path (measured on the same server, the same slot, first; within a
// 10 ms step), sends no
// pair late, and never waits; each connection's probe state at its first
// connected step is the first's, field by field. new_client as above
static void reuse_ruled_out_then_full( bool new_client )
{
    reuse_world_t world;
    reuse_world_create( world );
    rocketnet_client_t * client = reuse_client_create( world );

    // the first connection on a full path: the time to 1452 at both ends
    reuse_watch_t any = { ROCKETNET_MAX_PACKET_BYTES, false, 0, 0 };
    reuse_start_t first_start;
    check( reuse_connect( world, client, any, first_start ) );
    const double fresh = reuse_climb( world, client, ROCKETNET_MAX_PACKET_BYTES, any );
    check( fresh >= 0.0 );
    reuse_disconnect( world, client );
    if ( new_client )
    {
        rn_test_client_stop_and_destroy( client );
        client = reuse_client_create( world );
    }

    // the second on a 1200 link: 1172, then 1452 ruled out and the wait
    rocketnet_loopback_test_link_max_bytes( 1200 - 28 );
    reuse_start_t start;
    check( reuse_connect( world, client, any, start ) );
    check( reuse_start_same( start, first_start ) );
    const rocketnet_counters_t * counters = reuse_transport_counters( client );
    const uint64_t ruled_before = rocketnet_counter_value( counters, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT );
    check( reuse_climb( world, client, 1200 - 28, any ) >= 0.0 );
    for ( int i = 0; i < 600 && rocketnet_counter_value( counters, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT ) == ruled_before; i++ )
    {
        reuse_pump( world, client );
    }
    check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT ) == ruled_before + 1 );
    for ( int i = 0; i < 100; i++ )
    {
        reuse_pump( world, client );
    }
    check( rocketnet_client_max_packet_bytes( client ) == 1200 - 28 );
    // the client waits to re-try 1452 when the connection ends
    check( rocketnet_client_transport_protocol_for_test( rocketnet_message_client_transport( rocketnet_client_message_client( client ) ) )->probe_waiting == 1 );
    reuse_disconnect( world, client );
    if ( new_client )
    {
        rn_test_client_stop_and_destroy( client );
        client = reuse_client_create( world );
    }

    // the third, in the same slot, on a full path: no rule-out and no wait
    // of the second reaches it; it climbs as the first did
    // (the client's counters are read once it is connected: a new client
    // has no message client before its connect)
    rocketnet_loopback_test_link_max_bytes( 0 );
    check( reuse_connect( world, client, any, start ) );
    check( reuse_start_same( start, first_start ) );
    counters = reuse_transport_counters( client );
    const uint64_t ruled = rocketnet_counter_value( counters, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT );
    const uint64_t accepted = rocketnet_counter_value( counters, ROCKETNET_COUNTER_CLIENT_PROBE_ACCEPTED );
    check( rocketnet_client_max_packet_bytes( client ) == 548 );
    check( rocketnet_server_client_max_packet_bytes( world.server, 0 ) == 548 );
    const double reused = reuse_climb( world, client, ROCKETNET_MAX_PACKET_BYTES, any );
    if ( getenv( "ROCKETNET_LINK_MTU_PRINT_TIMES" ) )
    {
        printf( "reuse (%s): a fresh climb to 1452 in %.2f s, after a rule-out in the slot %.2f s\n", new_client ? "new client" : "same client", fresh, reused );
    }
    check( reused >= 0.0 );
    check( reused <= fresh + 0.01 + 1e-9 );      // a step of 10 ms at most: a stale wait is 10 s, a stale rule-out never gets there
    check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT ) == ruled );
    check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_CLIENT_PROBE_ACCEPTED ) == accepted + 2 );
    check( rocketnet_server_client_link_mtu( world.server, 0 ) == 1492 );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( world.server );
    rocketnet_loopback_test_link_max_bytes( 0 );
}

static void test_link_mtu_reused_slot_new_client_climbs_at_full_speed()
{
    reuse_ruled_out_then_full( true );
}

REGISTER_UNIT_TEST( test_link_mtu_reused_slot_new_client_climbs_at_full_speed );

static void test_link_mtu_reconnected_client_climbs_at_full_speed()
{
    reuse_ruled_out_then_full( false );
}

REGISTER_UNIT_TEST( test_link_mtu_reconnected_client_climbs_at_full_speed );

#endif // #if ROCKETNET_DEV
