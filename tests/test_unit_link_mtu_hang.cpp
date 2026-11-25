/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "test_unit_registry.h"
#include "portable_crt.h"
#include "unit_shutdown.h"
#include "token_string.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_link_mtu.h"
#include "../sdk/rocketnet_driver_loopback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loopback_hub.h"

#define check rocketnet_unit_check

// The hang, and the path probe that ends it (standard/TRANSPORT.md t11.4;
// proofs/tla/RNPathProbe.tla NoHang, FailsWhenNoneFits).
//
// On a path whose link MTU is below 1500 the handshake's packets, all small,
// arrive, the client connects, and then every packet larger than the path
// carries is lost: a reliable message that needs a full packet is resent
// forever while the small keep-alives keep the connection up. A client and
// server on the default link MTU list { 1500 } still behave that way, byte
// for byte; on a three-step list { 1492, 1200, 576 } the client probes after
// the challenge, the connection takes the largest size the path carries both
// ways, and the message arrives. When no size of the list is carried, the
// attempt ends in MTU_PROBE_FAILED instead of hanging.
//
// A step-driven loopback pair on a fixed clock, 10 ms a step; the path is the
// loopback driver's link seam, which loses every datagram above its size.

#if ROCKETNET_DEV

#define HANG_STEP_SECONDS 0.01

static double hang_now = 1000.0;
static double hang_clock( void * ) { return hang_now; }

static void hang_pump( rocketnet_server_t * server, rocketnet_client_t * client )
{
    rocketnet_client_update( client, hang_now );
    rocketnet_client_send_packets( client );
    rocketnet_server_update( server, hang_now );
    rocketnet_server_send_packets( server );
    hang_now += HANG_STEP_SECONDS;
}

struct hang_pair_t
{
    rocketnet_server_t * server;
    rocketnet_client_t * client;
    int connect_steps;              // steps from connect to both ends connected, -1 when they did not
};

static const int hang_three_step[3] = { 1492, 1200, 576 };

// a pair on the three-step list (with the window raised to hold the default
// 1 MiB message at the 576 entry's 480-byte block, as create requires), or on
// the default list when three_step is false; connects for at most max_steps
static void hang_pair_create( hang_pair_t & pair, bool three_step, int max_steps )
{
    rocketnet_quiet( 1 );
    hang_now = 1000.0;

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x3C ^ i );
    }
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    const uint64_t protocol_id = 0x4A11C1C1E5EED001ULL;
    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public, sizeof( backend_public ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    if ( three_step )
    {
        memcpy( server_config.link_mtus, hang_three_step, sizeof( hang_three_step ) );
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
    client_config.time_callback = hang_clock;
    if ( three_step )
    {
        memcpy( client_config.link_mtus, hang_three_step, sizeof( hang_three_step ) );
        client_config.num_link_mtus = 3;
        client_config.reliable_messages_in_flight = 4096;
        client_config.packets_in_flight = 4096;
    }
    pair.client = rocketnet_client_create( NULL, &client_config, NULL );
    check( pair.client != NULL );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( pair.server ) );
    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( backend_secret, pair.server, protocol_id, addresses, 1, 42, 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rn_test_client_connect_after_completion( pair.client, token ) == ROCKETNET_STATUS_OK );

    pair.connect_steps = -1;
    for ( int i = 0; i < max_steps; i++ )
    {
        hang_pump( pair.server, pair.client );
        if ( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED && rocketnet_server_client_connected( pair.server, 0 ) )
        {
            pair.connect_steps = i + 1;
            break;
        }
        if ( rocketnet_client_state( pair.client ) < ROCKETNET_CLIENT_STATE_DISCONNECTED )
        {
            break;
        }
    }
}

static void hang_pair_destroy( hang_pair_t & pair )
{
    rn_test_client_stop_and_destroy( pair.client );
    rn_test_server_stop_and_destroy( pair.server );
    rocketnet_loopback_test_link_max_bytes( 0 );
}

static uint64_t hang_client_counter( hang_pair_t & pair, int counter )
{
    struct rocketnet_client_transport_t * transport = rocketnet_message_client_transport( rocketnet_client_message_client( pair.client ) );
    return rocketnet_counter_value( rocketnet_client_transport_counters( transport ), counter );
}

static uint64_t hang_server_counter( hang_pair_t & pair, int counter )
{
    struct rocketnet_server_transport_t * transport = rocketnet_message_server_transport( rocketnet_server_message_server( pair.server ) );
    return rocketnet_counter_value( rocketnet_server_transport_counters( transport ), counter );
}

// one reliable message of bytes each way, sent at once; true when both
// arrive whole within max_steps
static bool hang_reliable_both_ways( hang_pair_t & pair, int bytes, int max_steps )
{
    uint8_t * up = rocketnet_client_start_message( pair.client, 0, ROCKETNET_RELIABILITY_RELIABLE, bytes );
    check( up != NULL );
    for ( int i = 0; i < bytes; i++ )
    {
        up[i] = (uint8_t) ( i * 7 + 1 );
    }
    check( rocketnet_client_finish_message( pair.client, bytes ) != 0 );

    uint8_t * down = rocketnet_server_start_message( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE, bytes );
    check( down != NULL );
    for ( int i = 0; i < bytes; i++ )
    {
        down[i] = (uint8_t) ( i * 11 + 3 );
    }
    check( rocketnet_server_finish_message( pair.server, bytes ) != 0 );

    bool up_arrived = false;
    bool down_arrived = false;
    for ( int step = 0; step < max_steps && !( up_arrived && down_arrived ); step++ )
    {
        hang_pump( pair.server, pair.client );

        const rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( pair.server ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; m++ )
            {
                up_arrived = up_arrived || ( batch->bytes[m] == bytes && batch->data[m][bytes - 1] == (uint8_t) ( ( bytes - 1 ) * 7 + 1 ) );
            }
        }
        while ( ( batch = rocketnet_client_receive_messages( pair.client ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; m++ )
            {
                down_arrived = down_arrived || ( batch->bytes[m] == bytes && batch->data[m][bytes - 1] == (uint8_t) ( ( bytes - 1 ) * 11 + 3 ) );
            }
        }
    }
    return up_arrived && down_arrived;
}

// A link MTU of 1200 (1172-byte IPv4 datagrams): the probe's 1452 window
// expires, the 1172 probe is answered, and a 64 KiB reliable message crosses
// both ways in packets no larger than the path carries. On the default list
// the same pair connects and the message never arrives while both ends stay
// connected: today's hang (planted: hang_no_hang_three_step = false).

static bool hang_no_hang_three_step = true;

static void test_link_mtu_no_hang_on_a_1200_link()
{
    rocketnet_loopback_test_link_max_bytes( 1172 );

    hang_pair_t pair;
    hang_pair_create( pair, hang_no_hang_three_step, 1000 );
    check( pair.connect_steps > 0 );

    // from here on every datagram either end hands the path is counted
    rocketnet_loopback_test_link_max_bytes( 1172 );

    // 30 seconds of the pair's clock: every resend of a full packet is lost
    // on the default list, and the connection does not end
    const bool arrived = hang_reliable_both_ways( pair, 65536, 3000 );
    if ( !arrived )
    {
        printf( "the 64 KiB message did not cross a 1200-byte link in 30 s: client state %s\n",
                rocketnet_client_state_name( rocketnet_client_state( pair.client ) ) );
    }
    check( arrived );
    check( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    check( rocketnet_client_max_packet_bytes( pair.client ) == 1172 );
    check( rocketnet_server_client_max_packet_bytes( pair.server, 0 ) == 1172 );
    check( rocketnet_loopback_test_largest_sent() <= 1172 );
    check( hang_client_counter( pair, ROCKETNET_COUNTER_CLIENT_PROBE_WINDOW_EXPIRED ) == 1 );
    check( hang_client_counter( pair, ROCKETNET_COUNTER_CLIENT_PROBE_ACCEPTED ) == 1 );
    check( hang_server_counter( pair, ROCKETNET_COUNTER_CONNECTION_PROBE_ANSWERED ) >= 1 );

    hang_pair_destroy( pair );
}

REGISTER_UNIT_TEST( test_link_mtu_no_hang_on_a_1200_link );

// A path that carries the connection request but no packet size of the list
// (540 bytes: the request is 518, the 576 entry's probe 548): every window
// expires and the attempt ends in MTU_PROBE_FAILED, with nothing connected.

static void test_link_mtu_probe_fails_when_no_size_fits()
{
    rocketnet_loopback_test_link_max_bytes( 540 );

    hang_pair_t pair;
    hang_pair_create( pair, true, 1000 );
    check( pair.connect_steps == -1 );
    check( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_MTU_PROBE_FAILED );
    check( strcmp( rocketnet_client_state_name( ROCKETNET_CLIENT_STATE_MTU_PROBE_FAILED ), "MTU_PROBE_FAILED" ) == 0 );
    check( !rocketnet_server_client_connected( pair.server, 0 ) );
    check( hang_client_counter( pair, ROCKETNET_COUNTER_CLIENT_PROBE_WINDOW_EXPIRED ) == 3 );
    check( hang_client_counter( pair, ROCKETNET_COUNTER_CLIENT_PROBE_FAILED ) == 1 );
    check( hang_client_counter( pair, ROCKETNET_COUNTER_CLIENT_PROBE_SENT ) == 12 );
    check( hang_server_counter( pair, ROCKETNET_COUNTER_CONNECTION_PROBE_RECEIVED ) == 0 );

    hang_pair_destroy( pair );
}

REGISTER_UNIT_TEST( test_link_mtu_probe_fails_when_no_size_fits );

// The default list sends no probe and nothing new: the response carries the
// packet size field at zero, the ceiling, and the connection is 1452.

static void test_link_mtu_default_list_sends_no_probe()
{
    hang_pair_t pair;
    hang_pair_create( pair, false, 1000 );
    check( pair.connect_steps > 0 );
    check( hang_client_counter( pair, ROCKETNET_COUNTER_CLIENT_PROBE_SENT ) == 0 );
    check( hang_server_counter( pair, ROCKETNET_COUNTER_CONNECTION_PROBE_RECEIVED ) == 0 );
    check( hang_server_counter( pair, ROCKETNET_COUNTER_CONNECTION_RESPONSE_SIZE_NOT_ALLOWED ) == 0 );
    check( rocketnet_client_max_packet_bytes( pair.client ) == ROCKETNET_MAX_PACKET_BYTES );
    check( rocketnet_server_client_max_packet_bytes( pair.server, 0 ) == ROCKETNET_MAX_PACKET_BYTES );
    hang_pair_destroy( pair );
}

REGISTER_UNIT_TEST( test_link_mtu_default_list_sends_no_probe );

// The join cost, in steps of 10 ms on a lossless path with no delay (one
// step is the round trip): the default list's handshake; the three-step list
// when the path carries its first entry (one probe round trip more); its
// second (one expired window of four sends at the 10 Hz send rate, 0.4 s,
// and the round trip); and its third (two windows, 0.8 s).

static void test_link_mtu_join_cost()
{
    struct
    {
        bool three_step;
        int link_max_bytes;
        int connect_steps;
    } joins[] = {
        { false, 0, 0 },
        { true, 0, 0 },
        { true, 1172, 0 },
        { true, 548, 0 },
    };

    for ( size_t i = 0; i < sizeof( joins ) / sizeof( joins[0] ); i++ )
    {
        rocketnet_loopback_test_link_max_bytes( joins[i].link_max_bytes );
        hang_pair_t pair;
        hang_pair_create( pair, joins[i].three_step, 1000 );
        joins[i].connect_steps = pair.connect_steps;
        check( pair.connect_steps > 0 );
        hang_pair_destroy( pair );
    }

    char print[8];
    if ( rocketnet_test_getenv( "ROCKETNET_LINK_MTU_PRINT_JOIN", print, sizeof( print ) ) )
    {
        printf( "join steps of 10 ms: default %d, three-step at 1452 %d, at 1172 %d, at 548 %d\n",
                joins[0].connect_steps, joins[1].connect_steps, joins[2].connect_steps, joins[3].connect_steps );
    }

    const int base = joins[0].connect_steps;
    const int window = (int) ( 4 / ROCKETNET_DEFAULT_SEND_RATE / HANG_STEP_SECONDS );     // 40 steps

    // the first entry: at most the probe's round trip and its early send more
    check( joins[1].connect_steps > base );
    check( joins[1].connect_steps <= base + 3 );

    // each further entry: one window more, within a send interval
    check( joins[2].connect_steps >= joins[1].connect_steps + window - 10 );
    check( joins[2].connect_steps <= joins[1].connect_steps + window + 10 );
    check( joins[3].connect_steps >= joins[2].connect_steps + window - 10 );
    check( joins[3].connect_steps <= joins[2].connect_steps + window + 10 );
}

REGISTER_UNIT_TEST( test_link_mtu_join_cost );

#endif // #if ROCKETNET_DEV
