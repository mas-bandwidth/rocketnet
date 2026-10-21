/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    macOS client timing under App Nap and timer coalescing (docs/CLIENT-IO.md,
    macOS timing and App Nap). A connected client over loopback runs for 60 s
    at a 60 Hz update while a server thread sends one timestamped payload every
    10 ms. Three conditions, one case each:

      test_macos_client_timing_default_qos            the process at its default QoS
      test_macos_client_timing_taskpolicy_background  the case re-runs itself under
                                                 `taskpolicy -b` (the throttling
                                                 and coalescing a napped app sees)
      test_macos_client_timing_background_thread_qos  the game thread, the one that
                                                 creates and updates the client,
                                                 lowered to QOS_CLASS_BACKGROUND

    Measured and printed per condition (p50, p99, max):
      wake      the client receive thread's wake intervals: the time callback
                (config time_callback, the receive stamp clock) runs on the I/O
                thread once per transport receive call, and calls closer than
                200 us apart are one wake
      latency   receive-to-update: from the server's send to the client update
                that returns the payload (the loopback wire is microseconds)
      late      the part of that latency past the first client update that ran
                after the send: zero when the receive thread had the packet in
                time for the next update
      frame     the game thread's measured update interval

    Asserted at the default class and with a background game thread:
    connected for the whole 60 s with zero timeouts, and the I/O thread's wake
    p99 under 5 ms. At the default class also receive-to-update p99 at most two
    update intervals plus 10 ms: the receive handoff's floor, where a datagram
    can wait for the update that returns the buffer the I/O thread needs and
    then for the next update. With a background game thread the game loop's
    own sleep is coalesced, so end-to-end latency there measures the game
    thread and is printed, not asserted.

    Under `taskpolicy -b` the whole process is throttled and no thread class
    lifts it, so that condition is measured only: it prints the same line with
    status=MEASURED and the bounds it met, and asserts nothing about timing.
*/

#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_platform.h"

#include <algorithm>
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

#include <mach-o/dyld.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <spawn.h>
#include <sys/wait.h>

extern char ** environ;

#define MACOS_TIMING_SECONDS 60.0
#define MACOS_TIMING_UPDATE_INTERVAL ( 1.0 / 60.0 )
#define MACOS_TIMING_SERVER_INTERVAL 0.010
#define MACOS_TIMING_BOUND_SLACK 0.010
#define MACOS_TIMING_LATENCY_INTERVALS 2.0
#define MACOS_TIMING_WAKE_BOUND 0.005
#define MACOS_TIMING_WAKE_GROUP 0.0002
#define MACOS_TIMING_MAX_WAKES ( 4 * 1024 * 1024 )
#define MACOS_TIMING_CHILD_ENV "ROCKETNET_MACOS_TIMING_TASKPOLICY_CHILD"

static void macos_timing_check( bool condition, const char * expression, int line )
{
    if ( condition )
        return;
    printf( "macos_client_timing check failed: %s at line %d\n", expression, line );
    fflush( stdout );
    exit( 1 );
}

#define macos_timing_require( condition ) macos_timing_check( (condition), #condition, __LINE__ )

// written only by the client I/O thread while the client lives; read after destroy
struct macos_timing_wakes_t
{
    double last_call;
    double group_start;
    uint64_t calls;
    uint64_t wakes;
    uint64_t dropped;
    int io_qos;
    double * intervals;
    uint64_t num_intervals;
};

static macos_timing_wakes_t macos_timing_wakes;

static double macos_timing_time_callback( void * context )
{
    (void) context;
    const double now = rocketnet_platform_time();
    macos_timing_wakes_t * w = &macos_timing_wakes;
    if ( w->calls == 0 )
    {
        w->io_qos = (int) qos_class_self();
        w->group_start = now;
        w->wakes = 1;
    }
    else if ( now - w->last_call > MACOS_TIMING_WAKE_GROUP )
    {
        if ( w->num_intervals < MACOS_TIMING_MAX_WAKES )
            w->intervals[w->num_intervals++] = now - w->group_start;
        else
            w->dropped++;
        w->group_start = now;
        w->wakes++;
    }
    w->last_call = now;
    w->calls++;
    return now;
}

static const char * macos_timing_qos_name( int qos )
{
    switch ( qos )
    {
        case QOS_CLASS_USER_INTERACTIVE: return "user_interactive";
        case QOS_CLASS_USER_INITIATED: return "user_initiated";
        case QOS_CLASS_DEFAULT: return "default";
        case QOS_CLASS_UTILITY: return "utility";
        case QOS_CLASS_BACKGROUND: return "background";
        case QOS_CLASS_UNSPECIFIED: return "unspecified";
        default: return "other";
    }
}

struct macos_timing_stats_t
{
    double p50, p99, max;
};

static macos_timing_stats_t macos_timing_stats( std::vector<double> & values )
{
    macos_timing_stats_t s;
    s.p50 = s.p99 = s.max = 0.0;
    if ( values.empty() )
        return s;
    std::sort( values.begin(), values.end() );
    const size_t n = values.size();
    s.p50 = values[ ( n - 1 ) / 2 ];
    s.p99 = values[ (size_t) ( (double) ( n - 1 ) * 0.99 ) ];
    s.max = values[ n - 1 ];
    return s;
}

struct macos_timing_server_t
{
    struct rocketnet_server_transport_t * server;
    double start;
    std::atomic<bool> stop;
    uint64_t sent;
};

static double macos_timing_game_time( double start )
{
    return 1000.0 + ( rocketnet_platform_time() - start );
}

static void macos_timing_server_thread( macos_timing_server_t * s )
{
    double next = rocketnet_platform_time();
    while ( !s->stop.load( std::memory_order_acquire ) )
    {
        rocketnet_server_transport_update( s->server, macos_timing_game_time( s->start ) );
        (void) rocketnet_server_transport_num_payloads( s->server );
        if ( rocketnet_server_transport_num_connected_clients( s->server ) == 1 )
        {
            struct rocketnet_packet_context_t context;
            if ( rocketnet_server_transport_start_packet( s->server, 0, 8, &context ) )
            {
                int capacity = 0;
                uint8_t * payload = rocketnet_server_transport_packet_payload( s->server, &context, &capacity );
                if ( payload && capacity >= 8 )
                {
                    const double sent_at = rocketnet_platform_time();
                    memcpy( payload, &sent_at, 8 );
                    if ( rocketnet_server_transport_finish_packet( s->server, &context, 8 ) > 0 )
                        s->sent++;
                }
            }
            rocketnet_server_transport_send_packets( s->server );
        }
        next += MACOS_TIMING_SERVER_INTERVAL;
        const double wait = next - rocketnet_platform_time();
        if ( wait > 0.0 )
            rocketnet_platform_sleep( wait );
    }
}

static void macos_timing_run( const char * condition, bool lower_game_thread, bool coalesced_game_loop, bool gated )
{
    memset( &macos_timing_wakes, 0, sizeof( macos_timing_wakes ) );
    std::vector<double> wake_storage( MACOS_TIMING_MAX_WAKES );
    macos_timing_wakes.intervals = wake_storage.data();

    const double start = rocketnet_platform_time();

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 1;
    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &server_config, macos_timing_game_time( start ), &status );
    macos_timing_require( server != NULL );
    macos_timing_require( status == ROCKETNET_STATUS_OK );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) rocketnet_server_transport_port( server ) );

    macos_timing_server_t server_state;
    server_state.server = server;
    server_state.start = start;
    server_state.stop.store( false );
    server_state.sent = 0;
    std::thread server_thread( macos_timing_server_thread, &server_state );

    // the game thread lowered after the server thread starts, so only the
    // game thread and what it creates see the background class
    if ( lower_game_thread )
        macos_timing_require( pthread_set_qos_class_self_np( QOS_CLASS_BACKGROUND, 0 ) == 0 );
    const int game_qos = (int) qos_class_self();

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    client_config.time_callback = macos_timing_time_callback;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    macos_timing_require( rocketnet_write_test_connect_token( server_address, 1, 4102444800ull, token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, &client_config, macos_timing_game_time( start ) );
    macos_timing_require( client != NULL );

    // connect within 5 s
    double next = rocketnet_platform_time();
    bool connected = false;
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, macos_timing_game_time( start ) );
        (void) rocketnet_client_transport_num_payloads( client );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            connected = true;
            break;
        }
        next += MACOS_TIMING_UPDATE_INTERVAL;
        const double wait = next - rocketnet_platform_time();
        if ( wait > 0.0 )
            rocketnet_platform_sleep( wait );
    }
    // a measured-only run that never connects prints final_state; the loop
    // below ends on its first frame that is not connected
    if ( gated )
        macos_timing_require( connected );

    std::vector<double> latency;
    std::vector<double> late;
    std::vector<double> frame;
    std::vector<double> update_times;
    latency.reserve( 8192 );
    late.reserve( 8192 );
    frame.reserve( 8192 );
    update_times.reserve( 8192 );

    uint64_t timeouts = 0;
    uint64_t not_connected_frames = 0;
    const double measure_start = rocketnet_platform_time();
    next = measure_start;
    double previous_update = 0.0;
    while ( rocketnet_platform_time() - measure_start < MACOS_TIMING_SECONDS )
    {
        const double now = rocketnet_platform_time();
        if ( previous_update > 0.0 )
            frame.push_back( now - previous_update );
        previous_update = now;
        update_times.push_back( now );

        rocketnet_client_transport_update( client, macos_timing_game_time( start ) );
        const double updated = rocketnet_platform_time();
        const int num_payloads = rocketnet_client_transport_num_payloads( client );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_bytes = 0;
            uint8_t * payload = rocketnet_client_transport_payload( client, i, &payload_bytes );
            if ( !payload || payload_bytes != 8 )
                continue;
            double sent_at;
            memcpy( &sent_at, payload, 8 );
            if ( sent_at < measure_start )
                continue;
            latency.push_back( updated - sent_at );
            // the first update that started after the send
            std::vector<double>::iterator first = std::upper_bound( update_times.begin(), update_times.end(), sent_at );
            late.push_back( first == update_times.end() ? 0.0 : now - *first );
        }

        const int state = rocketnet_client_transport_state( client );
        if ( state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            not_connected_frames++;
            if ( state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT )
                timeouts++;
            break;
        }

        next += MACOS_TIMING_UPDATE_INTERVAL;
        const double wait = next - rocketnet_platform_time();
        if ( wait > 0.0 )
            rocketnet_platform_sleep( wait );
    }
    const double measured_seconds = rocketnet_platform_time() - measure_start;
    const int final_state = rocketnet_client_transport_state( client );

    rocketnet_client_transport_destroy( client );
    server_state.stop.store( true, std::memory_order_release );
    server_thread.join();
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, macos_timing_game_time( start ) );
    rocketnet_server_transport_destroy( server );

    std::vector<double> wakes( wake_storage.begin(), wake_storage.begin() + (ptrdiff_t) macos_timing_wakes.num_intervals );
    const macos_timing_stats_t w = macos_timing_stats( wakes );
    const size_t num_latency = latency.size();
    const macos_timing_stats_t l = macos_timing_stats( latency );
    const macos_timing_stats_t k = macos_timing_stats( late );
    const macos_timing_stats_t f = macos_timing_stats( frame );
    const double latency_bound = MACOS_TIMING_LATENCY_INTERVALS * MACOS_TIMING_UPDATE_INTERVAL + MACOS_TIMING_BOUND_SLACK;

    const bool stayed = connected && not_connected_frames == 0 && final_state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED && measured_seconds >= MACOS_TIMING_SECONDS;
    // the I/O thread's wake p99 is the library's own contract in every gated
    // condition; a coalesced game loop's own sleep sets the end-to-end latency,
    // so latency is bounded only where the game loop runs at its cadence
    const bool wake_within = macos_timing_wakes.num_intervals > 0 && w.p99 < MACOS_TIMING_WAKE_BOUND;
    const bool latency_within = num_latency > 0 && l.p99 <= latency_bound;
    const bool within = coalesced_game_loop ? wake_within : ( wake_within && latency_within );
    printf( "macos_client_timing condition=%s seconds=%.1f game_qos=%s io_qos=%s io_calls=%llu wakes=%llu wake_dropped=%llu "
            "wake_ms p50=%.3f p99=%.3f max=%.3f "
            "latency_ms n=%llu p50=%.3f p99=%.3f max=%.3f "
            "late_ms p50=%.3f p99=%.3f max=%.3f "
            "frame_ms p50=%.3f p99=%.3f max=%.3f "
            "server_sent=%llu final_state=%d timeouts=%llu wake_bound_ms=%.3f wake_within=%d latency_bound_ms=%.3f latency_within=%d status=%s\n",
            condition, measured_seconds, macos_timing_qos_name( game_qos ), macos_timing_qos_name( macos_timing_wakes.io_qos ),
            (unsigned long long) macos_timing_wakes.calls, (unsigned long long) macos_timing_wakes.wakes,
            (unsigned long long) macos_timing_wakes.dropped,
            w.p50 * 1000.0, w.p99 * 1000.0, w.max * 1000.0,
            (unsigned long long) num_latency, l.p50 * 1000.0, l.p99 * 1000.0, l.max * 1000.0,
            k.p50 * 1000.0, k.p99 * 1000.0, k.max * 1000.0,
            f.p50 * 1000.0, f.p99 * 1000.0, f.max * 1000.0,
            (unsigned long long) server_state.sent, final_state, (unsigned long long) timeouts,
            MACOS_TIMING_WAKE_BOUND * 1000.0, wake_within ? 1 : 0, latency_bound * 1000.0, latency_within ? 1 : 0,
            !gated ? "MEASURED" : ( stayed && within ? "PASS" : "FAIL" ) );
    fflush( stdout );

    if ( !gated )
        return;

    macos_timing_require( stayed );
    macos_timing_require( timeouts == 0 );
    macos_timing_require( wake_within );
    if ( !coalesced_game_loop )
        macos_timing_require( latency_within );
}

void macos_client_timing_default_qos()
{
    macos_timing_run( "default_qos", false, false, true );
}

void macos_client_timing_background_thread_qos()
{
    macos_timing_run( "background_thread_qos", true, true, true );
}

void macos_client_timing_taskpolicy_background()
{
    if ( getenv( MACOS_TIMING_CHILD_ENV ) )
    {
        macos_timing_run( "taskpolicy_background", false, true, false );
        return;
    }

    char exe[4096];
    uint32_t exe_bytes = sizeof( exe );
    macos_timing_require( _NSGetExecutablePath( exe, &exe_bytes ) == 0 );
    macos_timing_require( setenv( MACOS_TIMING_CHILD_ENV, "1", 1 ) == 0 );
    char taskpolicy[] = "/usr/sbin/taskpolicy";
    char background[] = "-b";
    char run[] = "--run";
    char name[] = "test_macos_client_timing_taskpolicy_background";
    char * argv[] = { taskpolicy, background, exe, run, name, NULL };
    pid_t pid = 0;
    const int spawned = posix_spawn( &pid, taskpolicy, NULL, NULL, argv, environ );
    unsetenv( MACOS_TIMING_CHILD_ENV );
    macos_timing_require( spawned == 0 );
    int child_status = 0;
    macos_timing_require( waitpid( pid, &child_status, 0 ) == pid );
    // measured only: the child asserts nothing about timing, so a nonzero exit
    // here is a setup failure (server create, connect token, spawn)
    macos_timing_require( WIFEXITED( child_status ) && WEXITSTATUS( child_status ) == 0 );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
