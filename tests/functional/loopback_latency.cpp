/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The loopback client's added latency (functional: the real clock, a real
    client I/O thread). A loopback pair connects; then

    - idle: neither end is updated for one second, and the hub's count of
      client receives that found the queue empty
      (rocketnet_loopback_test_client_empty_receives) is the I/O thread's
      wake-ups. An idle client sleeps until a datagram is queued for it, it is
      handed work or stopped, or the core's guard timeout
      (ROCKETNET_CLIENT_IO_GUARD_TIMEOUT_SECONDS, 20 a second) passes; a
      transport with no wait polled 1000 times a second, each taking the hub
      mutex.
    - delivery: the server sends one payload and the test spins the client's
      update until it is delivered, and the client sends one and the test
      spins both updates, as a game would, timing each in microseconds; then the same
      on a 0.25 ms update cadence, counting the receiver's updates from the send
      to the delivery. A client woken by the send delivers on the first
      update after it; a polling one waits up to its 1 ms sleep, and the
      client's own sends wait for that sleep too.

    The cost is printed. The check is the idle wake-ups, bounded far from
    both designs in every build. The median delivery in each direction is
    checked too in a native Release run at scale 1 (tests/time_scale.h),
    where it measures the design rather than the build or the emulator.
    Platforms with no event-only wake keep the poll and do not run it.
*/

#include "../native_shutdown.h"
#include "rocketnet.h"
#include "rocketnet_layers.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_platform.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet_client_io.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// defined in tests/functional/harness.inc: the connect token timestamp from the clock that judges it
uint64_t rocketnet_functional_token_timestamp( uint64_t token_epoch_unix_seconds, double time );

static void loopback_latency_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_check( condition )                                                                                   \
    do                                                                                                                  \
    {                                                                                                                   \
        if ( !( condition ) )                                                                                           \
        {                                                                                                               \
            loopback_latency_check_handler( #condition, (const char *) __FUNCTION__, (const char *) __FILE__, __LINE__ ); \
        }                                                                                                               \
    } while ( 0 )

#define ROCKETNET_COUNTERS_CHECK rocketnet_check
#include "../counters_expect.h"
#include "../loopback_hub.h"
#include "../time_scale.h"

#if ROCKETNET_DEV && defined( ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE )

#define LOOPBACK_LATENCY_ROUNDS 200
#define LOOPBACK_LATENCY_PACED_ROUNDS 100
#define LOOPBACK_LATENCY_BOUND_SECONDS 1.0      // a bound, never a cadence: a delivery past it fails the case
#define LOOPBACK_LATENCY_IDLE_SECONDS 1.0
#define LOOPBACK_LATENCY_PACE_SECONDS 0.00025   // the paced rounds' update cadence, a quarter of a polling sleep

// the bounds, far from both designs: an idle client woken only by its guard
// wakes about 20 times a second and a polling one about 1000; a woken delivery
// is tens of microseconds and a polled one waits half the 1 ms sleep on average
#define LOOPBACK_LATENCY_IDLE_WAKES_BOUND 200
#define LOOPBACK_LATENCY_MEDIAN_BOUND_SECONDS 0.0004

static void loopback_latency_payload( uint8_t * data, int round, int direction )
{
    memset( data, 0, 8 );
    data[0] = (uint8_t) direction;
    data[1] = (uint8_t) ( round & 0xFF );
    data[2] = (uint8_t) ( ( round >> 8 ) & 0xFF );
}

static bool loopback_latency_client_has( struct rocketnet_client_transport_t * client, const uint8_t * want )
{
    bool found = false;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int bytes;
    while ( ( bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof( packet ) ) ) > 0 )
    {
        if ( bytes == 8 && memcmp( packet, want, 8 ) == 0 )
        {
            found = true;
        }
    }
    return found;
}

static bool loopback_latency_server_has( struct rocketnet_server_transport_t * server, const uint8_t * want )
{
    bool found = false;
    const int num_payloads = rocketnet_server_transport_num_payloads( server );
    for ( int i = 0; i < num_payloads; i++ )
    {
        int client_index = -1;
        int bytes = 0;
        const uint8_t * payload = rocketnet_server_transport_payload( server, i, &client_index, &bytes );
        if ( payload && client_index == 0 && bytes == 8 && memcmp( payload, want, 8 ) == 0 )
        {
            found = true;
        }
    }
    return found;
}

static double loopback_latency_percentile( double * values, int count, int percent )
{
    std::sort( values, values + count );
    int index = ( count * percent ) / 100;
    if ( index >= count )
    {
        index = count - 1;
    }
    return values[index];
}

void rocketnet_test_loopback_client_woken_by_send()
{
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_test_connect_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.token_epoch_unix_seconds, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, &client_config, rocketnet_platform_time() );
    rocketnet_check( client );

    // the production client: its I/O thread runs the core
    rocketnet_check( rocketnet_client_transport_client_io( client ) != NULL );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
        {
            break;
        }
        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_client_transport_client_index( client ) == 0 );

    // idle: nothing updated, nothing sent; the I/O thread's wake-ups

    const uint64_t idle_before = rocketnet_loopback_test_client_empty_receives();
    const double idle_start = rocketnet_platform_time();
    rocketnet_platform_sleep( LOOPBACK_LATENCY_IDLE_SECONDS );
    const double idle_seconds = rocketnet_platform_time() - idle_start;
    const uint64_t idle_after = rocketnet_loopback_test_client_empty_receives();
    const double idle_wakes_per_second = (double) ( idle_after - idle_before ) / idle_seconds;

    // spun delivery, server to client and client to server

    static double to_client[LOOPBACK_LATENCY_ROUNDS];
    static double to_server[LOOPBACK_LATENCY_ROUNDS];

    for ( int round = 0; round < LOOPBACK_LATENCY_ROUNDS; round++ )
    {
        // a varied gap, so the send lands at every phase of a polling sleep
        rocketnet_platform_sleep( 0.0001 * (double) ( round % 13 ) );

        uint8_t want[8];
        loopback_latency_payload( want, round, 1 );
        rocketnet_server_transport_send_packet( server, 0, want, 8 );
        const double sent = rocketnet_platform_time();
        rocketnet_server_transport_send_packets( server );
        bool delivered = false;
        double now = sent;
        while ( !delivered && now - sent < LOOPBACK_LATENCY_BOUND_SECONDS )
        {
            now = rocketnet_platform_time();
            rocketnet_client_transport_update( client, now );
            delivered = loopback_latency_client_has( client, want );
        }
        rocketnet_check( delivered );
        // the delivery is established once the update and the receipt check
        // above have returned: time it there, not at the update's start
        to_client[round] = rocketnet_platform_time() - sent;

        rocketnet_platform_sleep( 0.0001 * (double) ( ( round * 7 ) % 13 ) );

        loopback_latency_payload( want, round, 2 );
        rocketnet_client_transport_send_packet( client, want, 8 );
        const double client_sent = rocketnet_platform_time();
        rocketnet_client_transport_send_packets( client );
        delivered = false;
        now = client_sent;
        while ( !delivered && now - client_sent < LOOPBACK_LATENCY_BOUND_SECONDS )
        {
            now = rocketnet_platform_time();
            rocketnet_client_transport_update( client, now );
            rocketnet_server_transport_update( server, now );
            delivered = loopback_latency_server_has( server, want );
        }
        rocketnet_check( delivered );
        to_server[round] = rocketnet_platform_time() - client_sent;
    }

    // paced delivery: both ends update every LOOPBACK_LATENCY_PACE_SECONDS;
    // the receiver's updates from the send to the delivery

    int paced_to_client[4] = { 0, 0, 0, 0 };     // 1, 2, 3, more
    int paced_to_server[4] = { 0, 0, 0, 0 };

    for ( int round = 0; round < LOOPBACK_LATENCY_PACED_ROUNDS; round++ )
    {
        const bool server_sends = ( round & 1 ) == 0;
        uint8_t want[8];
        loopback_latency_payload( want, round, server_sends ? 3 : 4 );
        if ( server_sends )
        {
            rocketnet_server_transport_send_packet( server, 0, want, 8 );
            rocketnet_server_transport_send_packets( server );
        }
        else
        {
            rocketnet_client_transport_send_packet( client, want, 8 );
            rocketnet_client_transport_send_packets( client );
        }

        int updates = 0;
        bool delivered = false;
        const double sent = rocketnet_platform_time();
        while ( !delivered && rocketnet_platform_time() - sent < LOOPBACK_LATENCY_BOUND_SECONDS )
        {
            rocketnet_platform_sleep( LOOPBACK_LATENCY_PACE_SECONDS );
            updates++;
            if ( server_sends )
            {
                rocketnet_client_transport_update( client, rocketnet_platform_time() );
                delivered = loopback_latency_client_has( client, want );
            }
            else
            {
                rocketnet_server_transport_update( server, rocketnet_platform_time() );
                delivered = loopback_latency_server_has( server, want );
            }
        }
        rocketnet_check( delivered );
        int * histogram = server_sends ? paced_to_client : paced_to_server;
        histogram[updates < 4 ? updates - 1 : 3]++;
    }

    const double to_client_median = loopback_latency_percentile( to_client, LOOPBACK_LATENCY_ROUNDS, 50 );
    const double to_server_median = loopback_latency_percentile( to_server, LOOPBACK_LATENCY_ROUNDS, 50 );

    printf( "loopback idle client wake-ups: %.0f a second\n", idle_wakes_per_second );
    printf( "loopback server to client: median %.1f us, p90 %.1f us, max %.1f us\n", to_client_median * 1e6,
            loopback_latency_percentile( to_client, LOOPBACK_LATENCY_ROUNDS, 90 ) * 1e6, to_client[LOOPBACK_LATENCY_ROUNDS - 1] * 1e6 );
    printf( "loopback client to server: median %.1f us, p90 %.1f us, max %.1f us\n", to_server_median * 1e6,
            loopback_latency_percentile( to_server, LOOPBACK_LATENCY_ROUNDS, 90 ) * 1e6, to_server[LOOPBACK_LATENCY_ROUNDS - 1] * 1e6 );
    printf( "loopback paced server to client, receiver updates 1/2/3/more: %d/%d/%d/%d\n", paced_to_client[0], paced_to_client[1], paced_to_client[2], paced_to_client[3] );
    printf( "loopback paced client to server, receiver updates 1/2/3/more: %d/%d/%d/%d\n", paced_to_server[0], paced_to_server[1], paced_to_server[2], paced_to_server[3] );
    fflush( stdout );

    rocketnet_check( idle_wakes_per_second < LOOPBACK_LATENCY_IDLE_WAKES_BOUND );

    double env_scale = 1.0;
    rocketnet_check( rocketnet_test_case_env_time_scale( &env_scale ) );
    if ( ROCKETNET_TEST_CASE_TIME_SCALE == 1.0 && env_scale == 1.0 )
    {
        rocketnet_check( to_client_median < LOOPBACK_LATENCY_MEDIAN_BOUND_SECONDS );
        rocketnet_check( to_server_median < LOOPBACK_LATENCY_MEDIAN_BOUND_SECONDS );
    }

    rocketnet_counters_assert_conserved( rocketnet_client_transport_counters( client ) );
    rn_native_client_transport_stop_and_destroy( client );

    rocketnet_server_transport_stop( server );
    start = rocketnet_platform_time();
    while ( rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED && rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_STOPPED );
    rocketnet_counters_assert_conserved( rocketnet_server_transport_counters( server ) );
    rn_native_server_transport_stop_and_destroy( server );
}

#endif // #if ROCKETNET_DEV && defined( ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE )
