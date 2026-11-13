/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A server on token_epoch_unix_seconds 0 driven by rocketnet_time(), as the
// examples drive it, checks connect tokens against the wall clock read once
// plus that time; the backend mints by the wall clock. rocketnet_time() must
// therefore keep the wall clock's rate and count suspend (rocketnet.h,
// rocketnet_time). The OS clocks are faked through the platform's seams
// (rocketnet_platform_set_clock_for_testing, rocketnet_platform_set_wall_clock_for_testing),
// each clock id answering as the platform documents it: on Linux
// (clock_gettime(2)) CLOCK_REALTIME and CLOCK_BOOTTIME count suspend and run
// at the disciplined rate, CLOCK_MONOTONIC runs at that rate without suspend,
// CLOCK_MONOTONIC_RAW at the oscillator's rate without suspend; on macOS
// (clock_gettime(3), mach_time.h, Libc gen/clock_gettime.c) CLOCK_REALTIME
// and CLOCK_MONOTONIC count sleep at the disciplined rate, CLOCK_MONOTONIC_RAW
// counts sleep at the oscillator's rate and CLOCK_UPTIME_RAW does neither.
// No real time is read: the fakes start at 1000 seconds and advance only as a
// case moves them. The server and a step-driven client meet over the
// in-memory loopback transport, with no socket, sleep or thread.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#if ROCKETNET_DEV && ( defined( __linux__ ) || defined( __APPLE__ ) )

#include "loopback_hub.h"

#define check rocketnet_unit_check

#define WALL_DRIFT_PROTOCOL  0x51C0D21F7A3E9B46ULL

// the simulated machine: seconds it has run awake, seconds it has spent
// suspended, and how far its oscillator runs slow, in parts per million

static double wall_drift_awake;
static double wall_drift_suspended;
static double wall_drift_slow_ppm;

static bool wall_drift_clock_counts_suspend( int clock_id )
{
#if defined( __linux__ )
    return clock_id == CLOCK_REALTIME || clock_id == CLOCK_BOOTTIME;
#else // #if defined( __linux__ )
    return clock_id != CLOCK_UPTIME_RAW;
#endif // #if defined( __linux__ )
}

static bool wall_drift_clock_runs_raw( int clock_id )
{
#if defined( __linux__ )
    return clock_id == CLOCK_MONOTONIC_RAW;
#else // #if defined( __linux__ )
    return clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC;
#endif // #if defined( __linux__ )
}

static int wall_drift_clock( int clock_id, struct timespec * ts )
{
    double seconds = 1000.0 + wall_drift_awake;
    if ( wall_drift_clock_runs_raw( clock_id ) )
    {
        seconds -= wall_drift_awake * wall_drift_slow_ppm * 1.0e-6;
    }
    if ( wall_drift_clock_counts_suspend( clock_id ) )
    {
        seconds += wall_drift_suspended;
    }
    ts->tv_sec = (time_t) seconds;
    ts->tv_nsec = (long) ( ( seconds - (double) ts->tv_sec ) * 1.0e9 );
    return 0;
}

// Unix seconds: CLOCK_REALTIME's reading from the suite's fixed wall time

static uint64_t wall_drift_wall_clock( void )
{
    struct timespec ts;
    wall_drift_clock( CLOCK_REALTIME, &ts );
    return ROCKETNET_UNIT_WALL_CLOCK + (uint64_t) ts.tv_sec - 1000;
}

static uint64_t wall_drift_suite_wall_clock( void )
{
    return ROCKETNET_UNIT_WALL_CLOCK;
}

// rocketnet_time()'s clock, rocketnet_platform_time, as the library reads it.
// The Linux DEV unit binary wraps that symbol with a synthetic tick
// (-Wl,--wrap=rocketnet_platform_time, CMakeLists.txt; test_unit_endpoint.cpp),
// so there the library's own body is reached as __real_; elsewhere
// rocketnet_time() reaches it. Both ends are driven with it: the server's
// update time, and the client's time_callback in place of its default

#if defined( __linux__ )
extern "C" double __real_rocketnet_platform_time( void );
#endif // #if defined( __linux__ )

static double wall_drift_time( void )
{
#if defined( __linux__ )
    return __real_rocketnet_platform_time();
#else // #if defined( __linux__ )
    return rocketnet_time();
#endif // #if defined( __linux__ )
}

static double wall_drift_time_callback( void * )
{
    return wall_drift_time();
}

struct wall_drift_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    rocketnet_server_t * server;
    rocketnet_client_t * client;
};

// the clocks faked with the machine awake from zero; a started loopback
// server on epoch zero, updated once, so its epoch resolves from the fake
// wall clock; and a step-driven client on epoch zero whose time_callback is that
// clock, wall_drift_time()

static void wall_drift_create( wall_drift_t * t, double slow_ppm )
{
    memset( t, 0, sizeof( *t ) );
    rocketnet_quiet( 1 );
    wall_drift_awake = 0.0;
    wall_drift_suspended = 0.0;
    wall_drift_slow_ppm = slow_ppm;
    rocketnet_platform_set_clock_for_testing( wall_drift_clock );
    rocketnet_platform_set_wall_clock_for_testing( wall_drift_wall_clock );
    check( wall_drift_time() == 0.0 );

    check( rocketnet_generate_backend_keypair( t->backend_public, t->backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = WALL_DRIFT_PROTOCOL;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, t->backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    check( server_config.token_epoch_unix_seconds == 0 );
    t->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( t->server != NULL );
    check( rocketnet_server_start( t->server ) == ROCKETNET_STATUS_OK );

    rocketnet_server_update( t->server, wall_drift_time() );

    rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = true;
    client_config.time_callback = wall_drift_time_callback;
    check( client_config.token_epoch_unix_seconds == 0 );
    t->client = rocketnet_client_create( NULL, &client_config, NULL );
    check( t->client != NULL );
}

static void wall_drift_destroy( wall_drift_t * t )
{
    rn_test_client_stop_and_destroy( t->client );
    rn_test_server_stop_and_destroy( t->server );
    rocketnet_platform_set_wall_clock_for_testing( wall_drift_suite_wall_clock );
    rocketnet_platform_set_clock_for_testing( NULL );
    rocketnet_quiet( 0 );
}

// one frame: the machine runs dt seconds awake, then the client (when
// updated) and the server update on wall_drift_time()

static void wall_drift_frame( wall_drift_t * t, double dt, bool client )
{
    wall_drift_awake += dt;
    const double now = wall_drift_time();
    if ( client )
    {
        rocketnet_client_update( t->client, now );
        rocketnet_client_send_packets( t->client );
    }
    rocketnet_server_update( t->server, now );
    rocketnet_server_send_packets( t->server );
}

static bool wall_drift_connected( const wall_drift_t * t )
{
    return rocketnet_server_num_connected_clients( t->server ) == 1 && rocketnet_client_state( t->client ) == ROCKETNET_CLIENT_STATE_CONNECTED;
}

// a token minted now by the wall clock, as a backend mints it, with the
// default thirty-second lifetime; the client connects with it and both ends
// run frames of a tenth of a second for up to five seconds. True when both
// ends hold the connection

static bool wall_drift_connect_fresh_token( wall_drift_t * t )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( t->server ) );

    struct rocketnet_connect_token_config_t token_config;
    check( rocketnet_connect_token_default_config( &token_config ) == ROCKETNET_STATUS_OK );
    memcpy( token_config.backend_secret_key, t->backend_secret, sizeof( token_config.backend_secret_key ) );
    rocketnet_server_public_key( t->server, token_config.server_public_key );
    token_config.server_address_ipv4 = address;
    token_config.client_id = 0xD21F7;
    token_config.protocol_id = WALL_DRIFT_PROTOCOL;
    token_config.expire_seconds = 30;
    token_config.timeout_seconds = 10;
    token_config.unix_timestamp = wall_drift_wall_clock();

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rocketnet_create_connect_token( &token_config, token ) == ROCKETNET_STATUS_OK );
    // from a terminal state the first connect retires the old attempt and
    // returns CLIENT_NOT_DISCONNECTED (rocketnet.h, rocketnet_client_connect);
    // the helper waits for that, then connects
    check( rn_test_client_connect_after_completion( t->client, token ) == ROCKETNET_STATUS_OK );

    for ( int frame = 0; frame < 50; frame++ )
    {
        wall_drift_frame( t, 0.1, true );
        if ( wall_drift_connected( t ) )
        {
            return true;
        }
    }

    return false;
}

// no suspend and no rate error: a fresh token connects after ten minutes

static void test_token_clock_wall_drift_none_connects()
{
    wall_drift_t t;
    wall_drift_create( &t, 0.0 );

    for ( int frame = 0; frame < 60; frame++ )
    {
        wall_drift_frame( &t, 10.0, false );
    }
    check( wall_drift_connect_fresh_token( &t ) );

    wall_drift_destroy( &t );
}

// the machine suspends for ten minutes, past the 30-second creation skew: a
// token the backend mints from the wall clock on resume connects, because
// rocketnet_time() counted the suspend. A clock that does not count suspend
// (CLOCK_MONOTONIC_RAW, mach_absolute_time) leaves the server's token clock
// ten minutes behind for good and refuses every fresh token as created in the
// future

static void test_token_clock_wall_drift_suspend_connects()
{
    wall_drift_t t;
    wall_drift_create( &t, 0.0 );

    wall_drift_frame( &t, 10.0, false );
    wall_drift_suspended += 600.0;
    wall_drift_frame( &t, 0.1, false );
    check( wall_drift_connect_fresh_token( &t ) );
    check( wall_drift_time() > 610.0 );

    wall_drift_destroy( &t );
}

// the oscillator runs 100 ppm slow (the far end of crystal tolerance) for
// five days: about 43 seconds, past the 30-second creation skew. A token
// minted from the wall clock then still connects, because rocketnet_time()
// runs at the disciplined rate; CLOCK_MONOTONIC_RAW would be 43 seconds behind

static void test_token_clock_wall_drift_slow_oscillator_connects()
{
    wall_drift_t t;
    wall_drift_create( &t, 100.0 );

    for ( int frame = 0; frame < 5 * 24 * 60; frame++ )
    {
        wall_drift_frame( &t, 60.0, false );
    }
    check( wall_drift_connect_fresh_token( &t ) );

    wall_drift_destroy( &t );
}

// a connected pair when the client's machine sleeps for ten minutes while the
// server's runs on: the client's next update comes ten minutes after its last,
// a forward jump of its time, which behaves as an outage of that length. This
// case is about how the library takes the jump, whatever clock produced it
// (it passes with any clock id); the suspend case above is the clock's. The
// server times the silent client out (slot reason TIMED_OUT); on resume the
// client takes whatever was queued for it, hears nothing more, and times out
// within its timeout (CONNECTION_TIMED_OUT, nothing else); a fresh token then
// connects again

static void test_token_clock_wall_drift_resume_times_out_cleanly()
{
    wall_drift_t t;
    wall_drift_create( &t, 0.0 );

    check( wall_drift_connect_fresh_token( &t ) );
    for ( int frame = 0; frame < 20; frame++ )
    {
        wall_drift_frame( &t, 0.1, true );
    }
    check( wall_drift_connected( &t ) );

    for ( int frame = 0; frame < 600; frame++ )
    {
        wall_drift_frame( &t, 1.0, false );
    }
    check( rocketnet_server_num_connected_clients( t.server ) == 0 );
    check( rocketnet_server_disconnect_reason( t.server, 0 ) == ROCKETNET_DISCONNECT_REASON_TIMED_OUT );
    check( rocketnet_client_state( t.client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    for ( int frame = 0; frame < 150 && rocketnet_client_state( t.client ) == ROCKETNET_CLIENT_STATE_CONNECTED; frame++ )
    {
        wall_drift_frame( &t, 0.1, true );
    }
    check( rocketnet_client_state( t.client ) == ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT );
    check( rocketnet_server_num_connected_clients( t.server ) == 0 );

    check( wall_drift_connect_fresh_token( &t ) );

    wall_drift_destroy( &t );
}

REGISTER_UNIT_TEST( test_token_clock_wall_drift_none_connects );
REGISTER_UNIT_TEST( test_token_clock_wall_drift_suspend_connects );
REGISTER_UNIT_TEST( test_token_clock_wall_drift_slow_oscillator_connects );
REGISTER_UNIT_TEST( test_token_clock_wall_drift_resume_times_out_cleanly );

#endif // #if ROCKETNET_DEV && ( defined( __linux__ ) || defined( __APPLE__ ) )
