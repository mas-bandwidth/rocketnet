/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0-or-later or MBSL-1.0. See LICENSE and MBSL.md. */
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
// proofs/tla/RNPathProbe.tla ReachesBest, FailsWhenP0NotCarried).
//
// On a path whose link MTU is below 1500 the handshake's packets, all small,
// arrive, the client connects, and then every packet larger than the path
// carries is lost: a reliable message that needs a full packet is resent
// forever while the small keep-alives keep the connection up. A client and
// server on the default link MTU list { 1500 } still behave that way, byte
// for byte; on a three-step list { 1492, 1200, 576 } the connection starts
// at the smallest size, 548, which every such path carries, so the message
// arrives, and the probe after connect raises the size to the largest the
// path carries both ways. When the path does not carry even 548 the
// connection ends in MTU_PROBE_FAILED instead of hanging. A raise in the
// middle of a large reliable message loses and corrupts nothing.
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

// a pair with both ends on the link MTU list link_mtus (the default list
// when num_link_mtus is 0; with the window raised to hold the default 1 MiB
// message at a 576 entry's 480-byte block, as create requires); connects for
// at most max_steps
static void hang_pair_create_list( hang_pair_t & pair, const int * link_mtus, int num_link_mtus, int max_steps )
{
    bool small_entry = false;
    for ( int i = 0; i < num_link_mtus; i++ )
    {
        small_entry = small_entry || link_mtus[i] < 1200;
    }

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
    if ( num_link_mtus > 0 )
    {
        memcpy( server_config.link_mtus, link_mtus, sizeof( int ) * (size_t) num_link_mtus );
        server_config.num_link_mtus = num_link_mtus;
    }
    if ( small_entry )
    {
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
    if ( num_link_mtus > 0 )
    {
        memcpy( client_config.link_mtus, link_mtus, sizeof( int ) * (size_t) num_link_mtus );
        client_config.num_link_mtus = num_link_mtus;
    }
    if ( small_entry )
    {
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

// a pair on the three-step list, or on the default list when three_step is
// false
static void hang_pair_create( hang_pair_t & pair, bool three_step, int max_steps )
{
    hang_pair_create_list( pair, hang_three_step, three_step ? 3 : 0, max_steps );
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
// arrive whole, every byte as sent, within max_steps. Each step, the sizes
// both ends report are checked never to fall.
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
    int client_size = rocketnet_client_max_packet_bytes( pair.client );
    int server_size = rocketnet_server_client_max_packet_bytes( pair.server, 0 );
    for ( int step = 0; step < max_steps && !( up_arrived && down_arrived ); step++ )
    {
        hang_pump( pair.server, pair.client );

        const rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( pair.server ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; m++ )
            {
                bool whole = batch->bytes[m] == bytes;
                for ( int i = 0; whole && i < bytes; i++ )
                {
                    whole = batch->data[m][i] == (uint8_t) ( i * 7 + 1 );
                }
                up_arrived = up_arrived || whole;
            }
        }
        while ( ( batch = rocketnet_client_receive_messages( pair.client ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; m++ )
            {
                bool whole = batch->bytes[m] == bytes;
                for ( int i = 0; whole && i < bytes; i++ )
                {
                    whole = batch->data[m][i] == (uint8_t) ( i * 11 + 3 );
                }
                down_arrived = down_arrived || whole;
            }
        }

        const int client_now = rocketnet_client_max_packet_bytes( pair.client );
        const int server_now = rocketnet_server_client_max_packet_bytes( pair.server, 0 );
        check( client_now >= client_size );
        check( server_now >= server_size );
        client_size = client_now;
        server_size = server_now;
    }
    return up_arrived && down_arrived;
}

// steps the pair until both ends report size, at most max_steps; the steps
// taken, or -1
static int hang_steps_to_size( hang_pair_t & pair, int size, int max_steps )
{
    for ( int step = 0; step < max_steps; step++ )
    {
        if ( rocketnet_client_max_packet_bytes( pair.client ) == size && rocketnet_server_client_max_packet_bytes( pair.server, 0 ) == size )
        {
            return step;
        }
        hang_pump( pair.server, pair.client );
    }
    return -1;
}

// A link MTU of 1200 (1172-byte IPv4 datagrams): the connection starts at
// 548 and a 64 KiB reliable message crosses both ways at once; the probe
// raises both ends to 1172 and rules 1452 out. On the default list the same
// pair connects and the message never arrives while both ends stay
// connected: today's hang (planted: hang_no_hang_three_step = false).

static bool hang_no_hang_three_step = true;

static void test_link_mtu_no_hang_on_a_1200_link()
{
    rocketnet_loopback_test_link_max_bytes( 1172 );

    hang_pair_t pair;
    hang_pair_create( pair, hang_no_hang_three_step, 1000 );
    check( pair.connect_steps > 0 );

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

    check( hang_steps_to_size( pair, 1172, 3000 ) >= 0 );
    check( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( hang_client_counter( pair, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT ) == 1 );
    check( hang_client_counter( pair, ROCKETNET_COUNTER_CLIENT_PROBE_ACCEPTED ) == 1 );
    check( hang_server_counter( pair, ROCKETNET_COUNTER_CONNECTION_PROBE_CONFIRMED ) == 1 );

    hang_pair_destroy( pair );
}

REGISTER_UNIT_TEST( test_link_mtu_no_hang_on_a_1200_link );

// A path that carries the handshake but no packet size of the list (540
// bytes: the request is 518, the smallest size 548): the client connects at
// 548, 1172, the first step up, is ruled out, then 548 itself, checked
// last, and the connection ends in MTU_PROBE_FAILED rather than hanging,
// within a second of connect: STRIKES pairs at the fast pace for each of
// the two; 1452 is never probed (planted: past the last candidate the walk
// starts over, and the client stays connected; and, apart, a strike doubles
// the interval, and the end comes after 2.5 s).

static void test_link_mtu_probe_fails_when_no_size_fits()
{
    rocketnet_loopback_test_link_max_bytes( 540 );

    hang_pair_t pair;
    hang_pair_create( pair, true, 1000 );
    check( pair.connect_steps > 0 );
    int steps = 0;
    while ( steps < 8000 && rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
    {
        hang_pump( pair.server, pair.client );
        steps++;
    }
    check( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_MTU_PROBE_FAILED );
    check( steps * HANG_STEP_SECONDS <= 1.0 );
    check( strcmp( rocketnet_client_state_name( ROCKETNET_CLIENT_STATE_MTU_PROBE_FAILED ), "MTU_PROBE_FAILED" ) == 0 );
    check( hang_client_counter( pair, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT ) == 2 );
    check( hang_client_counter( pair, ROCKETNET_COUNTER_CLIENT_PROBE_FAILED ) == 1 );

    char print[8];
    if ( rocketnet_test_getenv( "ROCKETNET_LINK_MTU_PRINT_TIMES", print, sizeof( print ) ) )
    {
        printf( "link mtu, a 540-byte link on the three-step list: mtu probe failed (-14) after %.3f s\n", steps * HANG_STEP_SECONDS );
    }

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
    for ( int step = 0; step < 500; step++ )
    {
        hang_pump( pair.server, pair.client );
    }
    check( hang_client_counter( pair, ROCKETNET_COUNTER_CLIENT_PROBE_SENT ) == 0 );
    check( hang_server_counter( pair, ROCKETNET_COUNTER_CONNECTION_PROBE_RECEIVED ) == 0 );
    check( hang_server_counter( pair, ROCKETNET_COUNTER_CONNECTION_RESPONSE_SIZE_NOT_ALLOWED ) == 0 );
    check( rocketnet_client_max_packet_bytes( pair.client ) == ROCKETNET_MAX_PACKET_BYTES );
    check( rocketnet_server_client_max_packet_bytes( pair.server, 0 ) == ROCKETNET_MAX_PACKET_BYTES );
    hang_pair_destroy( pair );
}

REGISTER_UNIT_TEST( test_link_mtu_default_list_sends_no_probe );

// The join cost, in steps of 10 ms on a lossless path with no delay: every
// list joins in the steps the default list does, whatever the path carries,
// since the connection starts at the list's smallest size and probes after
// connect; on a path that carries 1452 both ends are there within a few
// steps of connect (planted: the first pair waits a second after connect,
// and 1452 takes a hundred steps).

static void test_link_mtu_join_cost()
{
    struct
    {
        bool three_step;
        int link_max_bytes;
        int connect_steps;
        int steps_to_max;
    } joins[] = {
        { false, 0, 0, 0 },
        { true, 0, 0, 0 },
        { true, 1172, 0, 0 },
        { true, 548, 0, 0 },
    };
    const int max_size[4] = { 1452, 1452, 1172, 548 };

    for ( size_t i = 0; i < sizeof( joins ) / sizeof( joins[0] ); i++ )
    {
        rocketnet_loopback_test_link_max_bytes( joins[i].link_max_bytes );
        hang_pair_t pair;
        hang_pair_create( pair, joins[i].three_step, 1000 );
        joins[i].connect_steps = pair.connect_steps;
        check( pair.connect_steps > 0 );
        joins[i].steps_to_max = hang_steps_to_size( pair, max_size[i], 6000 );
        check( joins[i].steps_to_max >= 0 );
        hang_pair_destroy( pair );
    }

    char print[8];
    if ( rocketnet_test_getenv( "ROCKETNET_LINK_MTU_PRINT_JOIN", print, sizeof( print ) ) )
    {
        printf( "join steps of 10 ms: default %d, three-step at 1452 %d, at 1172 %d, at 548 %d\n",
                joins[0].connect_steps, joins[1].connect_steps, joins[2].connect_steps, joins[3].connect_steps );
        printf( "steps of 10 ms from connect to the largest size carried: three-step at 1452 %d, at 1172 %d, at 548 %d\n",
                joins[1].steps_to_max, joins[2].steps_to_max, joins[3].steps_to_max );
    }

    for ( size_t i = 1; i < sizeof( joins ) / sizeof( joins[0] ); i++ )
    {
        check( joins[i].connect_steps == joins[0].connect_steps );
    }

    // a path that carries 1452: the first pair at connect, the size within a
    // few steps; one that carries 548 alone: the larger two ruled out, the
    // walk ending at 548 with no error
    check( joins[1].steps_to_max <= 5 );
}

REGISTER_UNIT_TEST( test_link_mtu_join_cost );

// A raise in the middle of a large reliable message: on a clean path the
// pair connects at 548 and at once queues a 256 KiB reliable message each
// way, fragments at the 480-byte stride of 548 queued and in flight while
// both ends rise to 1452 a few steps later; both messages arrive whole,
// every byte as sent, the sizes each end reports never falling, and the
// stride kept (planted: the raise rebinds the block stride to 1452's 1152,
// and the transfer breaks).

static void test_link_mtu_raise_with_fragments_in_flight()
{
    rocketnet_loopback_test_link_max_bytes( 0 );

    hang_pair_t pair;
    hang_pair_create( pair, true, 1000 );
    check( pair.connect_steps > 0 );
    check( rocketnet_client_max_packet_bytes( pair.client ) == 548 );
    check( rocketnet_server_client_max_packet_bytes( pair.server, 0 ) == 548 );
    const int unreliable_at_connect = rocketnet_client_max_unreliable_message_bytes( pair.client );

    check( hang_reliable_both_ways( pair, 262144, 3000 ) );
    check( rocketnet_client_max_packet_bytes( pair.client ) == 1452 );
    check( rocketnet_server_client_max_packet_bytes( pair.server, 0 ) == 1452 );
    check( rocketnet_client_max_unreliable_message_bytes( pair.client ) > unreliable_at_connect );
    check( rocketnet_server_client_max_unreliable_message_bytes( pair.server, 0 ) > unreliable_at_connect );
    check( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    // and again after the raise, at the new size
    check( hang_reliable_both_ways( pair, 65536, 3000 ) );

    hang_pair_destroy( pair );
}

REGISTER_UNIT_TEST( test_link_mtu_raise_with_fragments_in_flight );

// The link MTU each end reports (rocketnet_client_link_mtu,
// rocketnet_server_client_link_mtu): the list entry the connection's packet
// size comes from, the first, largest, where entries share a size, rising
// with the size and 0 when not connected.

// steps the pair for at most max_steps, until both ends report the link MTU
// want, recording each value the client reports in order (a value repeated
// is recorded once); checks at every step that each end's link MTU is the
// entry of its own packet size and never falls. Returns the steps taken, or
// -1 when the pair did not get there.
static int hang_link_mtu_steps( hang_pair_t & pair, int want, int max_steps, int * seen, int * num_seen, int max_seen )
{
    int client_last = rocketnet_client_link_mtu( pair.client );
    int server_last = rocketnet_server_client_link_mtu( pair.server, 0 );
    if ( *num_seen < max_seen )
    {
        seen[( *num_seen )++] = client_last;
    }
    for ( int step = 0; step <= max_steps; step++ )
    {
        const int client_now = rocketnet_client_link_mtu( pair.client );
        const int server_now = rocketnet_server_client_link_mtu( pair.server, 0 );
        check( client_now >= client_last );
        check( server_now >= server_last );
        if ( client_now != client_last && *num_seen < max_seen )
        {
            seen[( *num_seen )++] = client_now;
        }
        client_last = client_now;
        server_last = server_now;
        if ( client_now == want && server_now == want )
        {
            return step;
        }
        hang_pump( pair.server, pair.client );
    }
    return -1;
}

// The default list { 1500 }: 0 before the client connects, from both ends
// (the server's slot is empty), and 1500 once connected; 0 again from both
// ends after the client disconnects (planted: the entry comes from the
// packet size plus the IPv4 header, 1480, not from the list; and, apart, the
// connected checks removed).

static void test_link_mtu_reported_on_the_default_list()
{
    rocketnet_loopback_test_link_max_bytes( 0 );

    rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = "loopback";
    config.loopback_hub = rocketnet_test_loopback_hub();
    rocketnet_client_t * idle = rocketnet_client_create( NULL, &config, NULL );
    check( idle != NULL );
    check( rocketnet_client_link_mtu( idle ) == 0 );
    rocketnet_client_destroy( idle );

    hang_pair_t pair;
    hang_pair_create( pair, false, 0 );
    check( rocketnet_client_link_mtu( pair.client ) == 0 );
    check( rocketnet_server_client_link_mtu( pair.server, 0 ) == 0 );
    for ( int step = 0; step < 1000 && !( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED && rocketnet_server_client_connected( pair.server, 0 ) ); step++ )
    {
        check( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED || rocketnet_client_link_mtu( pair.client ) == 0 );
        check( rocketnet_server_client_connected( pair.server, 0 ) || rocketnet_server_client_link_mtu( pair.server, 0 ) == 0 );
        hang_pump( pair.server, pair.client );
    }
    check( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_link_mtu( pair.client ) == 1500 );
    check( rocketnet_server_client_link_mtu( pair.server, 0 ) == 1500 );

    rocketnet_client_disconnect( pair.client );
    for ( int step = 0; step < 300 && rocketnet_server_client_connected( pair.server, 0 ); step++ )
    {
        hang_pump( pair.server, pair.client );
    }
    check( !rocketnet_server_client_connected( pair.server, 0 ) );
    check( rocketnet_client_link_mtu( pair.client ) == 0 );
    check( rocketnet_server_client_link_mtu( pair.server, 0 ) == 0 );

    hang_pair_destroy( pair );
}

REGISTER_UNIT_TEST( test_link_mtu_reported_on_the_default_list );

// The three-step list { 1492, 1200, 576 }: both ends report 576 at connect,
// with the packet size at 548, and then each entry the size rises to, the
// client's values in order, a step at a time: 576, 1200 then 1492 on a path
// that carries 1452, 576 then 1200 on one that carries 1172 (planted: the
// entry is the first one at or above the packet size, so 1492 at connect;
// and, apart, the entry is the one the connection joined at, so it never
// rises; and, apart, the walk starts at the largest candidate, so 576 then
// 1492).

static void test_link_mtu_reported_as_the_size_rises()
{
    const struct
    {
        int link_max_bytes;
        int last;
        int count;
    } paths[] = { { 0, 1492, 3 }, { 1172, 1200, 2 } };

    for ( size_t i = 0; i < sizeof( paths ) / sizeof( paths[0] ); i++ )
    {
        rocketnet_loopback_test_link_max_bytes( paths[i].link_max_bytes );
        hang_pair_t pair;
        hang_pair_create( pair, true, 1000 );
        check( pair.connect_steps > 0 );
        check( rocketnet_client_max_packet_bytes( pair.client ) == 548 );
        check( rocketnet_client_link_mtu( pair.client ) == 576 );
        check( rocketnet_server_client_link_mtu( pair.server, 0 ) == 576 );

        int seen[8];
        int num_seen = 0;
        check( hang_link_mtu_steps( pair, paths[i].last, 6000, seen, &num_seen, 8 ) >= 0 );
        check( num_seen == paths[i].count );
        check( seen[0] == 576 );
        check( seen[1] == 1200 );
        check( seen[num_seen - 1] == paths[i].last );
        check( rocketnet_client_max_packet_bytes( pair.client ) == ( paths[i].last == 1492 ? 1452 : 1172 ) );

        hang_pair_destroy( pair );
    }
}

REGISTER_UNIT_TEST( test_link_mtu_reported_as_the_size_rises );

// A lossless link MTU of 1200 (1172-byte IPv4 datagrams), the staff VPN case:
// both ends join at 548 on the three-step list, the first pair raises both
// ends to 1172, reporting link MTU 1200, in one round trip and the confirm,
// a tenth of a second; then 1452 is ruled out in STRIKES pairs at the
// probe's fast pace, and the walk ends within one second of connect
// (planted: a strike doubles the interval as a lost pair does, and 1452 is
// ruled out only after 1.5 s; and, apart, the walk starts at the largest
// candidate, and both ends reach 1172 only after 0.4 s).

static void test_link_mtu_lossless_1200_link_settles_within_a_second()
{
    rocketnet_loopback_test_link_max_bytes( 1172 );

    hang_pair_t pair;
    hang_pair_create( pair, true, 1000 );
    check( pair.connect_steps > 0 );
    check( rocketnet_client_link_mtu( pair.client ) == 576 );

    int seen[8];
    int num_seen = 0;
    const int steps = hang_link_mtu_steps( pair, 1200, 1000, seen, &num_seen, 8 );
    check( steps >= 0 );
    check( steps * HANG_STEP_SECONDS <= 0.2 );
    check( rocketnet_client_max_packet_bytes( pair.client ) == 1172 );
    check( rocketnet_server_client_max_packet_bytes( pair.server, 0 ) == 1172 );

    int ended = steps;
    while ( ended < 1000 && hang_client_counter( pair, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT ) == 0 )
    {
        hang_pump( pair.server, pair.client );
        ended++;
    }
    check( ended * HANG_STEP_SECONDS <= 1.0 );
    check( hang_client_counter( pair, ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT ) == 1 );
    check( hang_client_counter( pair, ROCKETNET_COUNTER_CLIENT_PROBE_STRIKE ) == ROCKETNET_LINK_MTU_PROBE_STRIKES );
    check( rocketnet_client_max_packet_bytes( pair.client ) == 1172 );

    char print[8];
    if ( rocketnet_test_getenv( "ROCKETNET_LINK_MTU_PRINT_TIMES", print, sizeof( print ) ) )
    {
        printf( "link mtu, a lossless 1200 link: both ends at 1172 (link mtu 1200) after %.3f s, 1452 ruled out after %.3f s\n", steps * HANG_STEP_SECONDS, ended * HANG_STEP_SECONDS );
    }

    hang_pair_destroy( pair );
}

REGISTER_UNIT_TEST( test_link_mtu_lossless_1200_link_settles_within_a_second );

// IPv4 { 1500, 1492 }: both entries give 1452 (1492 - 28 = 1464, capped at
// 1452), so the connection joins at 1452 and both ends report the first,
// largest, entry, 1500 (planted: the last entry of the size, 1492).

static const int hang_equal_size[2] = { 1500, 1492 };

static void test_link_mtu_reported_first_of_equal_sizes()
{
    rocketnet_loopback_test_link_max_bytes( 0 );

    hang_pair_t pair;
    hang_pair_create_list( pair, hang_equal_size, 2, 1000 );
    check( pair.connect_steps > 0 );
    check( rocketnet_client_max_packet_bytes( pair.client ) == 1452 );
    for ( int step = 0; step < 300; step++ )
    {
        check( rocketnet_client_link_mtu( pair.client ) == 1500 );
        check( rocketnet_server_client_link_mtu( pair.server, 0 ) == 1500 );
        hang_pump( pair.server, pair.client );
    }

    hang_pair_destroy( pair );
}

REGISTER_UNIT_TEST( test_link_mtu_reported_first_of_equal_sizes );

#endif // #if ROCKETNET_DEV
