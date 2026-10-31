/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The server's receive buffer request (USAGE.md, "Receive buffer sizing").

    Two cases:

    test_server_receive_buffer_sized_from_max_clients runs in every build and
    pins what the server path does: the portable server asks the kernel for
    the bytes the formula gives at its max_clients (not the old fixed
    constant), the grant is read back, and a grant below the request logs one
    warning naming the limit to raise (net.core.rmem_max on Linux,
    kern.ipc.maxsockbuf on macOS and the BSDs).

    test_server_receive_1000_clients_256kbps_10hz is an opt-in correctness
    case around a measurement: the portable server has 1000 clients at 256
    kbps each and updates at 10 Hz (ROCKETNET_LOW_TICK_HZ runs the same load
    at 20 or 30 Hz). It requires all connections and protocol-valid payloads
    to survive the measured window, while reporting kernel drops and update
    cost without turning either into a portable-server performance promise.
    It is absent from the default suite unless ROCKETNET_LOW_TICK_MEASURE=1.
    Linux only: the socket drop count is read from procfs.
*/

#include "../native_shutdown.h"
#include "rocketnet_internal.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "../time_scale.h"
#include "../suite_runner.h"
#include "../portable_crt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <vector>

uint64_t rocketnet_functional_token_timestamp( uint64_t token_epoch_unix_seconds, double time );

static const int server_receive_low_tick_max_clients = 1000;
// the client count the sizing case gives the server: its request is
// 64 x 25,000 bytes, above the 1,000,000 floor and below any kernel limit
static const int server_receive_low_tick_server_clients = 64;

static long server_receive_low_tick_rss_bound_mb()
{
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = server_receive_low_tick_max_clients;

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );

    const uint64_t server_bytes = rocketnet_server_transport_required_memory( &server_config, "portable" );
    const uint64_t client_bytes = rocketnet_client_transport_required_memory( &client_config, "portable" );
    if ( !server_bytes || !client_bytes )
        return 0; // a refused plan keeps the strict suite-wide bound

    // 1000 client sockets and their workers: 256 MiB covers the process,
    // thread stacks and allocator rounding

    return suite_rss_bound_mb( server_bytes + (uint64_t) server_receive_low_tick_max_clients * client_bytes, 256 );
}

SUITE_CASE_RSS_BOUND( test_server_receive_1000_clients_256kbps_10hz, server_receive_low_tick_rss_bound_mb );

static void server_receive_low_tick_check( bool condition, const char * expression, int line )
{
    if ( condition )
        return;
    printf( "server_receive_low_tick check failed: %s at line %d\n", expression, line );
    fflush( stdout );
    exit( 1 );
}

#define server_receive_low_tick_require( condition ) server_receive_low_tick_check( (condition), #condition, __LINE__ )

// the log lines of the case, captured through the library's log callback
static char server_receive_low_tick_log[16384];
static std::atomic_flag server_receive_low_tick_log_lock = ATOMIC_FLAG_INIT;

static void server_receive_low_tick_capture( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) level;

    // the server's worker threads log too: the capture is under a spinlock

    while ( server_receive_low_tick_log_lock.test_and_set( std::memory_order_acquire ) )
        ;

    const size_t used = strlen( server_receive_low_tick_log );
    if ( used + length + 2 < sizeof( server_receive_low_tick_log ) )
    {
        memcpy( server_receive_low_tick_log + used, message, length );
        server_receive_low_tick_log[used + length] = '\n';
        server_receive_low_tick_log[used + length + 1] = '\0';
    }

    server_receive_low_tick_log_lock.clear( std::memory_order_release );
}

static void server_receive_low_tick_clear_log()
{
    while ( server_receive_low_tick_log_lock.test_and_set( std::memory_order_acquire ) )
        ;
    server_receive_low_tick_log[0] = '\0';
    server_receive_low_tick_log_lock.clear( std::memory_order_release );
}

#if !defined( _WIN32 ) || ROCKETNET_DEV
// the limit the warning names on this platform (sdk/rocketnet_platform.cpp,
// rocketnet_server_receive_buffer_granted)
static const char * server_receive_low_tick_limit_name()
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    return "net.core.rmem_max";
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return "the system socket buffer limit";
#else
    return "kern.ipc.maxsockbuf";
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
}
#endif // #if !defined( _WIN32 ) || ROCKETNET_DEV

// The server socket's request is the formula's value at the server's
// max_clients, the grant is read back, and a short grant names the limit.
void rocketnet_test_server_receive_buffer_sized_from_max_clients()
{
    server_receive_low_tick_clear_log();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_log_function( server_receive_low_tick_capture, NULL );

    const int request = rocketnet_server_receive_buffer_request( server_receive_low_tick_server_clients );
    server_receive_low_tick_require( request > ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = server_receive_low_tick_server_clients;
    server_config.token_epoch_unix_seconds = 1700000000ULL;
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "portable" );

#if defined( _WIN32 ) && ROCKETNET_DEV
    // Windows may grant this request in full. Inject only the warning decision
    // when that happens, while creating the real production server socket below.

    const int warning_grant_override = request / 2;
    server_receive_low_tick_require( warning_grant_override > 0 );
    rocketnet_server_receive_buffer_warning_grant_override_for_testing( warning_grant_override );
#endif // #if defined( _WIN32 ) && ROCKETNET_DEV

    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &server_config, 1000.0, &status );
#if defined( _WIN32 ) && ROCKETNET_DEV
    // A failed create can exit the test before the production helper consumes
    // the override; clear it before the fatal assertion either way.

    rocketnet_server_receive_buffer_warning_grant_override_for_testing( 0 );
#endif // #if defined( _WIN32 ) && ROCKETNET_DEV
    server_receive_low_tick_require( server != NULL );
    server_receive_low_tick_require( status == ROCKETNET_STATUS_OK );

    // the server asked the kernel for the formula's bytes and read the grant
    // back; the line names both

    char expected[64];
    snprintf( expected, sizeof( expected ), "%d requested bytes", request );
    server_receive_low_tick_require( strstr( server_receive_low_tick_log, "socket receive buffer" ) != NULL );
    server_receive_low_tick_require( strstr( server_receive_low_tick_log, expected ) != NULL );

#if defined( _WIN32 ) && ROCKETNET_DEV
    const char * actual_grant_text = strstr( server_receive_low_tick_log, "actual grant " );
    server_receive_low_tick_require( actual_grant_text != NULL );
    actual_grant_text += strlen( "actual grant " );

    char * actual_grant_end = NULL;
    const long actual_grant_value = strtol( actual_grant_text, &actual_grant_end, 10 );
    server_receive_low_tick_require( actual_grant_end != actual_grant_text );

    int actual_grant = (int) actual_grant_value;
    server_receive_low_tick_require( actual_grant > 0 );

    const bool warning_grant_override_used = actual_grant >= request;

    server_receive_low_tick_require( strstr( server_receive_low_tick_log, "DEV test warning grant override" ) != NULL );
    server_receive_low_tick_require( strstr( server_receive_low_tick_log,
                                             warning_grant_override_used ? "bytes applied" : "bytes not needed" ) != NULL );
    char override_text[64];
    const int warning_decision_grant = warning_grant_override_used ? warning_grant_override : actual_grant;
    snprintf( override_text, sizeof( override_text ), "granted %d of", warning_decision_grant );
    server_receive_low_tick_require( strstr( server_receive_low_tick_log, override_text ) != NULL );
    server_receive_low_tick_require( strstr( server_receive_low_tick_log, server_receive_low_tick_limit_name() ) != NULL );

    char expected_warning[384];
    const char * warning_reason = warning_grant_override_used ? "DEV test warning grant override; actual socket grant was reported above" : "packets arriving between updates past the grant are dropped by the kernel";
    snprintf( expected_warning, sizeof( expected_warning ),
              "socket receive buffer granted %d of %d requested bytes; %s: raise %s",
              warning_decision_grant, request, warning_reason, server_receive_low_tick_limit_name() );
    server_receive_low_tick_require( strstr( server_receive_low_tick_log, expected_warning ) != NULL );
    printf( "server_receive_buffer_observed_log_begin\n%sserver_receive_buffer_observed_log_end\n",
            server_receive_low_tick_log );
#endif // #if defined( _WIN32 ) && ROCKETNET_DEV

    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, 1000.0 );
    rn_native_server_transport_stop_and_destroy( server );

#if !defined( _WIN32 )
    // a real socket whose request no kernel grants in full: the grant that
    // comes back is short, and the warning names the limit to raise

    struct rocketnet_address_t address;
    server_receive_low_tick_require( rocketnet_address_parse( &address, "127.0.0.1:0" ) );

    const int oversized_request = 2000000000;

    struct rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &address,
                                                                                    ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, ROCKETNET_SOCKET_SEND_BUFFER_SIZE, oversized_request );
    server_receive_low_tick_require( socket != NULL );

    const int granted = rocketnet_platform_socket_receive_buffer_bytes( socket );
    server_receive_low_tick_require( granted > 0 );

    int warned_grant = granted;
    server_receive_low_tick_require( warned_grant < oversized_request );
    rocketnet_platform_socket_destroy( socket );

    server_receive_low_tick_clear_log();
    rocketnet_server_receive_buffer_granted( "functional", oversized_request, warned_grant );
    server_receive_low_tick_require( strstr( server_receive_low_tick_log, "socket receive buffer" ) != NULL );
    server_receive_low_tick_require( strstr( server_receive_low_tick_log, server_receive_low_tick_limit_name() ) != NULL );

    char granted_text[64];
    snprintf( granted_text, sizeof( granted_text ), "granted %d of", warned_grant );
    server_receive_low_tick_require( strstr( server_receive_low_tick_log, granted_text ) != NULL );
#endif // #if !defined( _WIN32 )

#if defined( _WIN32 ) && ROCKETNET_DEV
    printf( "server_receive_buffer_sized clients=%d request_bytes=%d actual_grant_bytes=%d warning_grant_override_bytes=%d warning_grant_override_used=%s limit=%s\n",
            server_receive_low_tick_server_clients, request, actual_grant, warning_grant_override,
            warning_grant_override_used ? "yes" : "no", server_receive_low_tick_limit_name() );
#elif defined( _WIN32 )
    printf( "server_receive_buffer_sized clients=%d request_bytes=%d production_grant_report=observed\n",
            server_receive_low_tick_server_clients, request );
#else
    printf( "server_receive_buffer_sized clients=%d request_bytes=%d short_grant_bytes=%d limit=%s\n",
            server_receive_low_tick_server_clients, request, warned_grant, server_receive_low_tick_limit_name() );
#endif // #if defined( _WIN32 ) && ROCKETNET_DEV
    fflush( stdout );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
}

#if defined( __linux__ )

#include <sys/resource.h>

static const int server_receive_low_tick_payload_bytes_per_second = 32000; // 256 kbps of payload per client
static const int server_receive_low_tick_packets_per_client_update = 4;    // 4 x 800 bytes per 0.1 s at 10 Hz
static const int server_receive_low_tick_slices = 10;                 // clients are updated in 10 slices of 100
static const double server_receive_low_tick_traffic_seconds = 10.0;

struct server_receive_low_tick_udp_row_t
{
    bool found;
    uint64_t receive_queue_bytes;
    uint64_t drops;
};

// The server socket's row in /proc/net/udp or /proc/net/udp6: the bytes the
// kernel holds for it now (rx_queue) and the datagrams it dropped since the
// socket opened (drops, the last column).
static server_receive_low_tick_udp_row_t server_receive_low_tick_udp_row( uint16_t port )
{
    server_receive_low_tick_udp_row_t row = { false, 0, 0 };
    const char * files[] = { "/proc/net/udp", "/proc/net/udp6" };
    for ( const char * file : files )
    {
        FILE * f = fopen( file, "r" );
        if ( !f )
            continue;

        char line[512];
        bool header = true;
        while ( fgets( line, sizeof( line ), f ) )
        {
            if ( header )
            {
                header = false;
                continue;
            }

            char local[128], remote[128];
            unsigned int state = 0;
            unsigned long tx_queue = 0, rx_queue = 0;
            if ( sscanf( line, " %*d: %127s %127s %x %lx:%lx", local, remote, &state, &tx_queue, &rx_queue ) != 5 )
                continue;

            const char * colon = strrchr( local, ':' );
            if ( !colon || (uint16_t) strtoul( colon + 1, NULL, 16 ) != port )
                continue;

            const size_t length = strlen( line );
            size_t end = length;
            while ( end > 0 && ( line[end - 1] == '\n' || line[end - 1] == ' ' ) )
            {
                --end;
            }

            size_t start = end;
            while ( start > 0 && line[start - 1] != ' ' )
            {
                --start;
            }

            row.found = true;
            row.receive_queue_bytes += rx_queue;
            row.drops += strtoull( line + start, NULL, 10 );
        }

        fclose( f );
    }

    return row;
}

static int server_receive_low_tick_update_hz()
{
    // measurement only: ROCKETNET_LOW_TICK_HZ runs the same load at another
    // update rate (20 or 30) to report inbound bytes per update; the case
    // itself runs at 10 Hz

    char value[32];
    if ( rocketnet_test_getenv( "ROCKETNET_LOW_TICK_HZ", value, sizeof( value ) ) )
    {
        const int hz = atoi( value );
        if ( hz >= 1 && hz <= 1000 )
            return hz;
    }

    return 10;
}

bool rocketnet_test_server_receive_low_tick_measurement_requested()
{
    char value[32];
    return rocketnet_test_getenv( "ROCKETNET_LOW_TICK_MEASURE", value, sizeof( value ) ) &&
           ( value[0] == '1' || ( value[0] != '\0' && value[0] != '0' ) );
}

static void server_receive_low_tick_write_u32( uint8_t * bytes, uint32_t value )
{
    bytes[0] = (uint8_t) ( value >> 24 );
    bytes[1] = (uint8_t) ( value >> 16 );
    bytes[2] = (uint8_t) ( value >> 8 );
    bytes[3] = (uint8_t) value;
}

static uint32_t server_receive_low_tick_read_u32( const uint8_t * bytes )
{
    return ( (uint32_t) bytes[0] << 24 ) |
           ( (uint32_t) bytes[1] << 16 ) |
           ( (uint32_t) bytes[2] << 8 ) |
           (uint32_t) bytes[3];
}

static uint64_t server_receive_low_tick_refusals( const struct rocketnet_counters_t * counters )
{
    uint64_t total = 0;

    for ( int i = 0; i < rocketnet_num_counters(); ++i )
        if ( rocketnet_counter_class( i ) == ROCKETNET_COUNTER_CLASS_REFUSAL )
        {
            total += rocketnet_counter_value( counters, i );
        }

    return total;
}

static uint64_t server_receive_low_tick_queue_overflows( const struct rocketnet_counters_t * counters )
{
    static const int queue_counters[] = {
        ROCKETNET_COUNTER_SEND_RING_FULL,
        ROCKETNET_COUNTER_ACKED_BATCH_FULL,
        ROCKETNET_COUNTER_CLIENT_ACKED_BATCH_FULL,
        ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL,
        ROCKETNET_COUNTER_SEND_ARENA_FULL,
        ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW,
        ROCKETNET_COUNTER_SEND_LEASE_FULL,
        ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL,
        ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL,
    };

    uint64_t total = 0;

    for ( int counter : queue_counters )
    {
        total += rocketnet_counter_value( counters, counter );
    }

    return total;
}

static uint64_t server_receive_low_tick_disconnects( const struct rocketnet_counters_t * counters )
{
    return rocketnet_counter_value( counters, ROCKETNET_COUNTER_CLIENT_DISCONNECTED ) +
           rocketnet_counter_value( counters, ROCKETNET_COUNTER_SERVER_DISCONNECTED_CLIENT ) +
           rocketnet_counter_value( counters, ROCKETNET_COUNTER_CLIENT_TIMED_OUT );
}

void rocketnet_test_server_receive_1000_clients_256kbps_10hz()
{
    server_receive_low_tick_require( rocketnet_test_server_receive_low_tick_measurement_requested() );

    const int max_clients = server_receive_low_tick_max_clients;
    const int update_hz = server_receive_low_tick_update_hz();
    const double update_seconds = 1.0 / update_hz;
    const double slice_seconds = update_seconds / server_receive_low_tick_slices;
    const int clients_per_slice = max_clients / server_receive_low_tick_slices;

    // each client updates once per server update and sends that update's
    // share of 256 kbps, so the rate is the same at every update rate

    const int packet_bytes = server_receive_low_tick_payload_bytes_per_second / ( update_hz * server_receive_low_tick_packets_per_client_update );
    server_receive_low_tick_require( packet_bytes >= 8 );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = max_clients;
    server_config.token_epoch_unix_seconds = 1700000000ULL;
    server_config.handshake_deadline_seconds = 30;
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "portable" );

    // each portable client holds a socket and wake descriptors: raise the
    // soft descriptor limit toward the hard one, as bin/load does

    struct rlimit limit;
    server_receive_low_tick_require( getrlimit( RLIMIT_NOFILE, &limit ) == 0 );

    const rlim_t wanted = (rlim_t) max_clients * 8 + 1024;
    if ( limit.rlim_cur < wanted )
    {
        limit.rlim_cur = limit.rlim_max < wanted ? limit.rlim_max : wanted;
        server_receive_low_tick_require( setrlimit( RLIMIT_NOFILE, &limit ) == 0 );
    }

    const double start = rocketnet_platform_time();
    double time = 1000.0;

    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &server_config, time, &status );
    server_receive_low_tick_require( server != NULL );
    server_receive_low_tick_require( status == ROCKETNET_STATUS_OK );

    const uint16_t port = rocketnet_server_transport_port( server );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) port );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    client_config.token_epoch_unix_seconds = server_config.token_epoch_unix_seconds;
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "portable" );

    std::vector<struct rocketnet_client_transport_t *> clients;
    std::vector<uint32_t> client_packet_sequences;
    clients.reserve( max_clients );
    client_packet_sequences.resize( max_clients, 0 );
    for ( int i = 0; i < max_clients; ++i )
    {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        server_receive_low_tick_require( rocketnet_write_test_connect_token( server_address, (uint64_t) i + 1, rocketnet_functional_token_timestamp( server_config.token_epoch_unix_seconds, time ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

        struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, &client_config, time );
        server_receive_low_tick_require( client != NULL );
        clients.push_back( client );
    }

    // connect every client; the clock is the wall clock from 1000.0

    const double connect_deadline = rocketnet_platform_time() + 30.0 * ROCKETNET_FUNCTIONAL_TIME_SCALE;
    int connected = 0;

    while ( true )
    {
        time = 1000.0 + ( rocketnet_platform_time() - start );

        for ( size_t i = 0; i < clients.size(); ++i )
        {
            rocketnet_client_transport_update( clients[i], time );
        }

        rocketnet_server_transport_update( server, time );
        connected = 0;
        for ( size_t i = 0; i < clients.size(); ++i )
            if ( rocketnet_client_transport_state( clients[i] ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            {
                ++connected;
            }

        if ( connected == max_clients && rocketnet_server_transport_num_connected_clients( server ) == max_clients )
            break;

        if ( rocketnet_platform_time() >= connect_deadline )
        {
            printf( "server_receive_low_tick connect deadline: clients_connected=%d server_connected=%d\n",
                    connected, rocketnet_server_transport_num_connected_clients( server ) );
            for ( size_t i = 0; i < clients.size(); ++i )
            {
                const int state = (int) rocketnet_client_transport_state( clients[i] );
                if ( state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
                    printf( "server_receive_low_tick client %d state %d\n", (int) i, state );
            }
            fflush( stdout );
            break;
        }

        rocketnet_platform_sleep( 0.001 );
    }

    server_receive_low_tick_require( connected == max_clients );
    server_receive_low_tick_require( rocketnet_server_transport_num_connected_clients( server ) == max_clients );

    const server_receive_low_tick_udp_row_t before = server_receive_low_tick_udp_row( port );
    struct rocketnet_server_transport_stats_t stats_before;
    rocketnet_server_transport_stats( server, &stats_before );

    // traffic: every slice updates 100 clients, each sending one update's
    // worth of packets; every 10 slices the server updates once

    const double traffic_start = rocketnet_platform_time();
    double next_slice = traffic_start;
    int slice = 0;
    uint64_t server_updates = 0;
    uint64_t payloads_received = 0;
    uint64_t payload_bytes_received = 0;
    uint64_t payloads_sent = 0;
    uint64_t payload_bytes_sent = 0;
    uint64_t payloads_checked = 0;
    uint64_t invalid_payload_lengths = 0;
    uint64_t invalid_payload_clients = 0;
    uint64_t invalid_payload_content = 0;
    uint64_t connection_state_failures = 0;
    uint64_t receive_queue_peak_bytes = 0;
    double server_update_seconds = 0.0;
    while ( rocketnet_platform_time() - traffic_start < server_receive_low_tick_traffic_seconds )
    {
        const double now = rocketnet_platform_time();
        if ( now < next_slice )
        {
            rocketnet_platform_sleep( next_slice - now < 0.001 ? next_slice - now : 0.001 );
            continue;
        }

        next_slice += slice_seconds;
        time = 1000.0 + ( now - start );

        const int first = ( slice % server_receive_low_tick_slices ) * clients_per_slice;
        for ( int i = first; i < first + clients_per_slice; ++i )
        {
            struct rocketnet_client_transport_t * client = clients[(size_t) i];
            if ( rocketnet_client_transport_state( client ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            {
                ++connection_state_failures;
                continue;
            }

            for ( int p = 0; p < server_receive_low_tick_packets_per_client_update; ++p )
            {
                struct rocketnet_packet_context_t context;
                uint8_t * packet = rocketnet_client_transport_start_packet( client, packet_bytes, &context );
                if ( !packet )
                    break;

                int capacity = 0;
                uint8_t * payload = rocketnet_client_transport_packet_payload( client, &context, &capacity );
                server_receive_low_tick_require( payload != NULL && capacity >= packet_bytes );

                const uint32_t packet_sequence = client_packet_sequences[(size_t) i];

                server_receive_low_tick_write_u32( payload, (uint32_t) i );
                server_receive_low_tick_write_u32( payload + 4, packet_sequence );

                for ( int j = 8; j < packet_bytes; ++j )
                {
                    payload[j] = (uint8_t) ( (uint32_t) i * 31u + packet_sequence * 17u + (uint32_t) j );
                }

                server_receive_low_tick_require( rocketnet_client_transport_finish_packet( client, &context, packet_bytes ) > 0 );
                client_packet_sequences[(size_t) i]++;
                ++payloads_sent;
                payload_bytes_sent += (uint64_t) packet_bytes;
            }

            rocketnet_client_transport_send_packets( client );
            rocketnet_client_transport_update( client, time );
        }

        ++slice;
        if ( slice % server_receive_low_tick_slices == 0 )
        {
            // the kernel's hold for the socket just before the update reads it

            const server_receive_low_tick_udp_row_t row = server_receive_low_tick_udp_row( port );
            if ( row.receive_queue_bytes > receive_queue_peak_bytes )
            {
                receive_queue_peak_bytes = row.receive_queue_bytes;
            }

            const double update_start = rocketnet_platform_time();

            rocketnet_server_transport_update( server, time );
            server_update_seconds += rocketnet_platform_time() - update_start;
            ++server_updates;

            const int num_payloads = rocketnet_server_transport_num_payloads( server );
            for ( int i = 0; i < num_payloads; ++i )
            {
                int client_index = -1, bytes = 0;
                const uint8_t * payload = rocketnet_server_transport_payload( server, i, &client_index, &bytes );
                if ( payload )
                {
                    ++payloads_received;
                    payload_bytes_received += (uint64_t) bytes;
                    if ( bytes != packet_bytes )
                    {
                        ++invalid_payload_lengths;
                        continue;
                    }

                    const uint32_t tagged_client = server_receive_low_tick_read_u32( payload );
                    const uint32_t packet_sequence = server_receive_low_tick_read_u32( payload + 4 );
                    if ( tagged_client >= (uint32_t) max_clients || client_index < 0 ||
                         rocketnet_server_transport_client_id( server, client_index ) != (uint64_t) tagged_client + 1 ||
                         ( tagged_client < (uint32_t) max_clients && packet_sequence >= client_packet_sequences[(size_t) tagged_client] ) )
                        ++invalid_payload_clients;

                    bool content_valid = true;
                    const int samples[] = { 8, packet_bytes / 2, packet_bytes - 1 };
                    for ( int sample : samples )
                        if ( sample >= 8 && payload[sample] != (uint8_t) ( tagged_client * 31u + packet_sequence * 17u + (uint32_t) sample ) )
                        {
                            content_valid = false;
                        }

                    if ( !content_valid )
                    {
                        ++invalid_payload_content;
                    }

                    ++payloads_checked;
                }
            }

            for ( size_t i = 0; i < clients.size(); ++i )
                if ( rocketnet_client_transport_state( clients[i] ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
                {
                    ++connection_state_failures;
                }

            if ( rocketnet_server_transport_num_connected_clients( server ) != max_clients )
            {
                ++connection_state_failures;
            }
        }
    }

    const server_receive_low_tick_udp_row_t after = server_receive_low_tick_udp_row( port );
    struct rocketnet_server_transport_stats_t stats_after;
    rocketnet_server_transport_stats( server, &stats_after );

    int clients_connected = 0;

    for ( size_t i = 0; i < clients.size(); ++i )
        if ( rocketnet_client_transport_state( clients[i] ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            ++clients_connected;
        }

    const int server_connected = rocketnet_server_transport_num_connected_clients( server );
    const uint64_t kernel_drops = after.drops - before.drops;
    const uint64_t wire_bytes = stats_after.bytes_received - stats_before.bytes_received;
    const uint64_t wire_packets = stats_after.packets_received - stats_before.packets_received;
    uint64_t refusal_counters = server_receive_low_tick_refusals( rocketnet_server_transport_counters( server ) );
    uint64_t queue_overflow_counters = server_receive_low_tick_queue_overflows( rocketnet_server_transport_counters( server ) );
    uint64_t disconnect_counters = server_receive_low_tick_disconnects( rocketnet_server_transport_counters( server ) );
    uint64_t socket_send_drops = rocketnet_counter_value( rocketnet_server_transport_counters( server ), ROCKETNET_COUNTER_SEND_FAILED );

    for ( size_t i = 0; i < clients.size(); ++i )
    {
        const struct rocketnet_counters_t * counters = rocketnet_client_transport_counters( clients[i] );
        refusal_counters += server_receive_low_tick_refusals( counters );
        queue_overflow_counters += server_receive_low_tick_queue_overflows( counters );
        disconnect_counters += server_receive_low_tick_disconnects( counters );
        socket_send_drops += rocketnet_counter_value( counters, ROCKETNET_COUNTER_SEND_FAILED );
    }

    const uint64_t correctness_refusals = refusal_counters - socket_send_drops;

    const bool correctness_pass = before.found && after.found && clients_connected == max_clients &&
                                  server_connected == max_clients && connection_state_failures == 0 && payloads_sent > 0 &&
                                  payloads_received > 0 && payload_bytes_received > 0 && wire_packets > 0 && wire_bytes > 0 &&
                                  payloads_checked == payloads_received && invalid_payload_lengths == 0 &&
                                  invalid_payload_clients == 0 && invalid_payload_content == 0 &&
                                  correctness_refusals == 0 && queue_overflow_counters == 0 && disconnect_counters == 0;

    // information, never a gate: the portable server has no performance
    // guarantee and the host's load and rmem_max decide the drop count

    printf( "server_receive_low_tick update_hz=%d clients=%d traffic_seconds=%.1f server_updates=%llu "
            "wire_packets=%llu wire_bytes=%llu wire_bytes_per_update=%llu payloads_sent=%llu payload_bytes_sent=%llu "
            "payloads_received=%llu payload_bytes_received=%llu payloads_checked=%llu "
            "invalid_payload_lengths=%llu invalid_payload_clients=%llu invalid_payload_content=%llu "
            "per_client_wire_kbps=%.1f receive_queue_peak_bytes=%llu kernel_receive_drops=%llu "
            "server_update_seconds=%.3f clients_connected=%d server_connected=%d connection_state_failures=%llu "
            "disconnect_counters=%llu refusal_counters=%llu correctness_refusals=%llu queue_overflow_counters=%llu "
            "socket_send_drops=%llu correctness=%s\n",
            update_hz, max_clients, server_receive_low_tick_traffic_seconds, (unsigned long long) server_updates,
            (unsigned long long) wire_packets, (unsigned long long) wire_bytes,
            (unsigned long long) ( server_updates ? wire_bytes / server_updates : 0 ),
            (unsigned long long) payloads_sent, (unsigned long long) payload_bytes_sent,
            (unsigned long long) payloads_received, (unsigned long long) payload_bytes_received,
            (unsigned long long) payloads_checked,
            (unsigned long long) invalid_payload_lengths, (unsigned long long) invalid_payload_clients,
            (unsigned long long) invalid_payload_content,
            (double) wire_bytes * 8.0 / ( server_receive_low_tick_traffic_seconds * 1000.0 * max_clients ),
            (unsigned long long) receive_queue_peak_bytes, (unsigned long long) kernel_drops,
            server_update_seconds, clients_connected, server_connected,
            (unsigned long long) connection_state_failures, (unsigned long long) disconnect_counters,
            (unsigned long long) refusal_counters, (unsigned long long) correctness_refusals,
            (unsigned long long) queue_overflow_counters, (unsigned long long) socket_send_drops,
            correctness_pass ? "PASS" : "FAIL" );
    fflush( stdout );

    for ( size_t i = 0; i < clients.size(); ++i )
    {
        rn_native_client_transport_stop_and_destroy( clients[i] );
    }

    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, time );
    rn_native_server_transport_stop_and_destroy( server );
    server_receive_low_tick_require( correctness_pass );
}

#else // #if defined(__linux__)

void rocketnet_test_server_receive_1000_clients_256kbps_10hz()
{
    // the kernel drop count is read from /proc/net/udp

    printf( "server_receive_low_tick skipped: linux only\n" );
    fflush( stdout );
}

#endif // #if defined(__linux__)
