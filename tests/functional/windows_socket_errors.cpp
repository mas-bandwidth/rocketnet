/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Windows receive error paths. A raw test socket sends a live server, per
    server transport and family, datagrams past the wire's ceiling (which
    Winsock reports as WSAEMSGSIZE or a truncated completion), zero-length
    and one-byte runts, and seeded random bytes of every length 2..1400; then
    the same at a connected client. Each datagram is dropped and counted, the
    live client stays connected and keeps exchanging, and the transport's
    receives in flight are the same before and after: a receive loop that
    ends on an error, or a receive buffer that is not posted again after a
    drop, turns these cases red.
*/

#include "../native_shutdown.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#include "rocketnet_server_iocp.h"
#include "rocketnet_server_rio.h"

#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

// test seams (sdk/rocketnet_server_transport.cpp, sdk/rocketnet_client_transport.cpp)
void * rocketnet_server_transport_io_name_for_testing( struct rocketnet_server_transport_t * server_transport );
rocketnet_platform_socket_t * rocketnet_portable_server_test_socket( void * transport );
uint16_t rocketnet_client_transport_test_bound_port( struct rocketnet_client_transport_t * client_transport );

#define WINDOWS_SOCKET_ERRORS_TICK ( 1.0 / 60.0 )
#define WINDOWS_SOCKET_ERRORS_PAYLOAD_BYTES 32
#define WINDOWS_SOCKET_ERRORS_SENDS_PER_TICK 32
#define WINDOWS_SOCKET_ERRORS_GARBAGE_SEED 0x5eed5eed12345678ULL

static void windows_socket_errors_check( bool condition, const char * expression, int line )
{
    if ( condition )
        return;
    printf( "windows_socket_errors check failed: %s at line %d\n", expression, line );
    fflush( stdout );
    exit( 1 );
}

#define windows_socket_errors_require( condition ) windows_socket_errors_check( (condition), #condition, __LINE__ )

struct windows_socket_errors_transport_t
{
    const char * transport;
    bool ipv6;
};

static const char * windows_socket_errors_host( bool ipv6 )
{
    return ipv6 ? "[::1]" : "127.0.0.1";
}

static void windows_socket_errors_clear_env()
{
    // the case pins its own transports: an ambient pin must not override them

    _putenv_s( "ROCKETNET_TRANSPORT", "" );
    _putenv_s( "ROCKETNET_SERVER_TRANSPORT", "" );
    _putenv_s( "ROCKETNET_CLIENT_TRANSPORT", "" );
}

// ---------------------------------------------------------------------------
// the datagrams

struct windows_socket_errors_datagram_t
{
    std::vector<uint8_t> bytes;
};

static uint64_t windows_socket_errors_next( uint64_t * state )
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void windows_socket_errors_oversize( std::vector<windows_socket_errors_datagram_t> & datagrams )
{
    static const int sizes[] = { 1500, 9000, 16384, 65507 };
    for ( size_t i = 0; i < sizeof( sizes ) / sizeof( sizes[0] ); i++ )
    {
        windows_socket_errors_datagram_t datagram;
        datagram.bytes.resize( (size_t) sizes[i] );
        for ( int j = 0; j < sizes[i]; j++ )
        {
            datagram.bytes[(size_t) j] = (uint8_t) ( j * 31 + 7 );
        }

        datagrams.push_back( datagram );
    }
}

static void windows_socket_errors_runt_and_garbage( std::vector<windows_socket_errors_datagram_t> & datagrams )
{
    uint64_t state = WINDOWS_SOCKET_ERRORS_GARBAGE_SEED;
    windows_socket_errors_datagram_t empty;
    datagrams.push_back( empty );

    windows_socket_errors_datagram_t one;
    one.bytes.push_back( (uint8_t) windows_socket_errors_next( &state ) );
    datagrams.push_back( one );

    for ( int length = 2; length <= 1400; length++ )
    {
        windows_socket_errors_datagram_t datagram;
        datagram.bytes.resize( (size_t) length );
        for ( int j = 0; j < length; j++ )
        {
            datagram.bytes[(size_t) j] = (uint8_t) windows_socket_errors_next( &state );
        }

        datagrams.push_back( datagram );
    }
}

// ---------------------------------------------------------------------------
// the raw test socket: plain Winsock, outside every rocketnet path

struct windows_socket_errors_sender_t
{
    SOCKET handle;
    sockaddr_storage to;
    int to_bytes;
};

static void windows_socket_errors_sender_open( windows_socket_errors_sender_t * sender, bool ipv6, uint16_t port )
{
    sender->handle = socket( ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    windows_socket_errors_require( sender->handle != INVALID_SOCKET );

    int send_buffer = 4 * 1024 * 1024;
    setsockopt( sender->handle, SOL_SOCKET, SO_SNDBUF, (const char *) &send_buffer, sizeof( send_buffer ) );
    memset( &sender->to, 0, sizeof( sender->to ) );

    if ( ipv6 )
    {
        sockaddr_in6 * to = (sockaddr_in6 *) &sender->to;
        to->sin6_family = AF_INET6;
        to->sin6_addr = in6addr_loopback;
        to->sin6_port = htons( port );
        sender->to_bytes = (int) sizeof( sockaddr_in6 );
    }
    else
    {
        sockaddr_in * to = (sockaddr_in *) &sender->to;
        to->sin_family = AF_INET;
        to->sin_addr.s_addr = htonl( INADDR_LOOPBACK );
        to->sin_port = htons( port );
        sender->to_bytes = (int) sizeof( sockaddr_in );
    }
}

static void windows_socket_errors_sender_send( windows_socket_errors_sender_t * sender, const windows_socket_errors_datagram_t * datagram )
{
    static const char nothing = 0;
    const char * data = datagram->bytes.empty() ? &nothing : (const char *) &datagram->bytes[0];
    const int result = sendto( sender->handle, data, (int) datagram->bytes.size(), 0, (const sockaddr *) &sender->to, sender->to_bytes );
    windows_socket_errors_require( result == (int) datagram->bytes.size() );
}

static void windows_socket_errors_sender_close( windows_socket_errors_sender_t * sender )
{
    closesocket( sender->handle );
}

// ---------------------------------------------------------------------------
// one server, one live client, a payload each way every tick

struct windows_socket_errors_session_t
{
    windows_socket_errors_transport_t transport;
    int max_clients;                    // the server's configured client count
    struct rocketnet_server_transport_t * server;
    struct rocketnet_client_transport_t * client;
    int client_index;
    uint32_t sequence;
    uint64_t server_received;
    uint64_t client_received;
    double next;
};

static void windows_socket_errors_sleep_to( double next )
{
    const double now = rocketnet_platform_time();
    if ( next > now )
    {
        rocketnet_platform_sleep( next - now );
    }
}

static void windows_socket_errors_tick( windows_socket_errors_session_t * session )
{
    const double now = rocketnet_platform_time();

    session->sequence++;

    uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];

    rocketnet_client_transport_update( session->client, now );
    while ( rocketnet_client_transport_receive_packet( session->client, payload, (int) sizeof( payload ) ) > 0 )
    {
        session->client_received++;
    }

    if ( rocketnet_client_transport_state( session->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
    {
        memset( payload, 0, WINDOWS_SOCKET_ERRORS_PAYLOAD_BYTES );
        memcpy( payload, &session->sequence, sizeof( session->sequence ) );
        rocketnet_client_transport_send_packet( session->client, payload, WINDOWS_SOCKET_ERRORS_PAYLOAD_BYTES );
    }

    rocketnet_server_transport_update( session->server, now );

    const int num_payloads = rocketnet_server_transport_num_payloads( session->server );
    for ( int i = 0; i < num_payloads; i++ )
    {
        int client_index = -1;
        int payload_bytes = 0;
        rocketnet_server_transport_payload( session->server, i, &client_index, &payload_bytes );
        windows_socket_errors_require( client_index == session->client_index || session->client_index < 0 );
        session->server_received++;
    }

    for ( int client_index = 0; client_index < 4; client_index++ )
    {
        if ( !rocketnet_server_transport_is_client_connected( session->server, client_index ) )
            continue;
        memset( payload, 0x5a, WINDOWS_SOCKET_ERRORS_PAYLOAD_BYTES );
        rocketnet_server_transport_send_packet( session->server, client_index, payload, WINDOWS_SOCKET_ERRORS_PAYLOAD_BYTES );
    }

    rocketnet_server_transport_send_packets( session->server );

    session->next += WINDOWS_SOCKET_ERRORS_TICK;
    windows_socket_errors_sleep_to( session->next );
}

static void windows_socket_errors_session_start( windows_socket_errors_session_t * session, const windows_socket_errors_transport_t * transport )
{
    memset( session, 0, sizeof( *session ) );
    session->transport = *transport;
    session->client_index = -1;
    windows_socket_errors_clear_env();

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;
    session->max_clients = server_config.max_clients;
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", transport->transport );

    char bind[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( bind, sizeof( bind ), "%s:0", windows_socket_errors_host( transport->ipv6 ) );
    session->server = rocketnet_server_transport_create( NULL, bind, &server_config );
    windows_socket_errors_require( session->server != NULL );
    windows_socket_errors_require( strcmp( rocketnet_server_transport_io_name( session->server ), transport->transport ) == 0 );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "%s:%d", windows_socket_errors_host( transport->ipv6 ), rocketnet_server_transport_port( session->server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    windows_socket_errors_require( rocketnet_write_test_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token, sizeof( connect_token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "portable" );
    session->client = rocketnet_client_transport_create( NULL, connect_token, &client_config, rocketnet_platform_time() );
    windows_socket_errors_require( session->client != NULL );

    session->next = rocketnet_platform_time();

    const double start = session->next;

    while ( rocketnet_client_transport_state( session->client ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ||
            rocketnet_server_transport_num_connected_clients( session->server ) != 1 )
    {
        windows_socket_errors_tick( session );
        windows_socket_errors_require( rocketnet_client_transport_state( session->client ) > ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
        windows_socket_errors_require( rocketnet_platform_time() - start < 10.0 );
    }

    session->client_index = rocketnet_client_transport_client_index( session->client );
    windows_socket_errors_require( rocketnet_server_transport_is_client_connected( session->server, session->client_index ) );

    // half a second of traffic before anything is measured

    const double warm = rocketnet_platform_time();
    while ( rocketnet_platform_time() - warm < 0.5 )
    {
        windows_socket_errors_tick( session );
    }

    windows_socket_errors_require( session->server_received > 0 && session->client_received > 0 );
}

static void windows_socket_errors_session_finish( windows_socket_errors_session_t * session )
{
    rn_native_client_transport_stop_and_destroy( session->client );
    rn_native_server_transport_stop_and_destroy( session->server );
}

// ---------------------------------------------------------------------------
// what the server transport counts: every refusal of the transport's own
// counters, plus the drops its transport counts before the transport sees them

static uint64_t windows_socket_errors_refusals( const struct rocketnet_counters_t * counters )
{
    uint64_t sum = 0;

    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
        if ( rocketnet_counter_class( i ) == ROCKETNET_COUNTER_CLASS_REFUSAL )
        {
            sum += rocketnet_counter_value( counters, i );
        }

    return sum;
}

static uint64_t windows_socket_errors_transport_drops( windows_socket_errors_session_t * session )
{
    void * instance = rocketnet_server_transport_io_name_for_testing( session->server );

    if ( strcmp( session->transport.transport, "iocp" ) == 0 )
    {
        uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
        rocketnet_server_iocp_counters( (rocketnet_server_iocp_t *) instance, counters );

        // Size drops now also enter the server's public prefilter counters.
        // Keep this aggregate one event per datagram while retaining the
        // transport's legacy RECEIVE_DROPPED total for its direct readers.

        return counters[ROCKETNET_IOCP_RECEIVE_DROPPED] -
               counters[ROCKETNET_IOCP_RECEIVE_TOO_SHORT] -
               counters[ROCKETNET_IOCP_RECEIVE_TOO_LARGE];
    }

    if ( strcmp( session->transport.transport, "rio" ) == 0 )
    {
        uint64_t counters[ROCKETNET_RIO_NUM_COUNTERS];
        rocketnet_server_rio_counters( (rocketnet_server_rio_t *) instance, counters );

        // RECEIVE_DROPPED_SIZE is represented by PREFILTER_TOO_LARGE above;
        // failed completions are still transport-only drops.

        return counters[ROCKETNET_RIO_COUNTER_RECEIVE_FAILED];
    }

    return 0;
}

static uint64_t windows_socket_errors_server_counted( windows_socket_errors_session_t * session )
{
    return windows_socket_errors_refusals( rocketnet_server_transport_counters( session->server ) ) +
           windows_socket_errors_transport_drops( session );
}

// the transport's posted operations: receives in flight plus sends the kernel
// has not completed; -1 for the synchronous portable transport, which posts
// nothing
static int windows_socket_errors_pending( windows_socket_errors_session_t * session )
{
    void * instance = rocketnet_server_transport_io_name_for_testing( session->server );

    if ( strcmp( session->transport.transport, "iocp" ) == 0 )
        return rocketnet_server_iocp_pending( (rocketnet_server_iocp_t *) instance );
    if ( strcmp( session->transport.transport, "rio" ) == 0 )
        return rocketnet_server_rio_pending( (rocketnet_server_rio_t *) instance );

    return -1;
}

// the posted operations of a server at rest: every receive posted and no send
// outstanding. The server transport creates iocp with the default worker count
// (sdk/rocketnet_driver_builtin.cpp); -1 for portable, as pending reads.
static int windows_socket_errors_full_pending( windows_socket_errors_session_t * session )
{
    if ( strcmp( session->transport.transport, "iocp" ) == 0 )
        return ROCKETNET_IOCP_DEFAULT_WORKERS * ROCKETNET_IOCP_RECEIVES_PER_WORKER;
    if ( strcmp( session->transport.transport, "rio" ) == 0 )
        return rocketnet_server_rio_receives( session->max_clients );

    return -1;
}

// receives in flight: ticks until the posted operations are back at the full
// set, for at most 10 seconds, and returns the last count read. One read races
// the completion worker, which takes a receive off the posted count before it
// reposts it, so the fewest of a window of reads caught that dip in one window
// and not in another; a receive that a drop failed to repost holds the count
// below the full set at every read.
static int windows_socket_errors_receives_in_flight( windows_socket_errors_session_t * session )
{
    const int full = windows_socket_errors_full_pending( session );
    int pending = windows_socket_errors_pending( session );
    const double start = rocketnet_platform_time();
    while ( pending != full && rocketnet_platform_time() - start < 10.0 )
    {
        windows_socket_errors_tick( session );
        pending = windows_socket_errors_pending( session );
    }

    return pending;
}

static void windows_socket_errors_inject( windows_socket_errors_session_t * session, uint16_t port, const std::vector<windows_socket_errors_datagram_t> & datagrams )
{
    windows_socket_errors_sender_t sender;
    windows_socket_errors_sender_open( &sender, session->transport.ipv6, port );

    for ( size_t i = 0; i < datagrams.size(); i++ )
    {
        windows_socket_errors_sender_send( &sender, &datagrams[i] );
        if ( ( i + 1 ) % WINDOWS_SOCKET_ERRORS_SENDS_PER_TICK == 0 )
        {
            windows_socket_errors_tick( session );
        }
    }

    windows_socket_errors_sender_close( &sender );
}

static const char * windows_socket_errors_label( const windows_socket_errors_transport_t * transport )
{
    static char label[64];
    snprintf( label, sizeof( label ), "%s %s", transport->transport, transport->ipv6 ? "ipv6" : "ipv4" );
    return label;
}

static void windows_server_drops( const windows_socket_errors_transport_t * transport, const char * name, const std::vector<windows_socket_errors_datagram_t> & datagrams )
{
    windows_socket_errors_session_t session;
    windows_socket_errors_session_start( &session, transport );

    const int in_flight_before = windows_socket_errors_receives_in_flight( &session );
    const uint64_t counted_before = windows_socket_errors_server_counted( &session );
    const uint64_t server_received_before = session.server_received;
    const uint64_t client_received_before = session.client_received;

    windows_socket_errors_inject( &session, rocketnet_server_transport_port( session.server ), datagrams );

    // drain: every datagram reaches its counter, and the live pair keeps
    // exchanging; at least 0.5 s, then until both sides have received more
    // than 30 payloads, bounded at 10 s

    const double drain = rocketnet_platform_time();
    while ( rocketnet_platform_time() - drain < 0.5 ||
            ( ( session.server_received - server_received_before <= 30 || session.client_received - client_received_before <= 30 ) &&
              rocketnet_platform_time() - drain < 10.0 ) )
    {
        windows_socket_errors_tick( &session );
    }

    const int in_flight_after = windows_socket_errors_receives_in_flight( &session );
    const uint64_t counted = windows_socket_errors_server_counted( &session ) - counted_before;
    const uint64_t sent = (uint64_t) datagrams.size();

    printf( "%s %s: sent %llu, counted %llu, receives in flight %d before and %d after, server received %llu and client received %llu payloads during and after\n",
            name, windows_socket_errors_label( transport ), (unsigned long long) sent, (unsigned long long) counted,
            in_flight_before, in_flight_after,
            (unsigned long long) ( session.server_received - server_received_before ), (unsigned long long) ( session.client_received - client_received_before ) );
    fflush( stdout );

    // every dropped datagram is counted: no silent drop

    windows_socket_errors_require( counted == sent );
    windows_socket_errors_require( in_flight_before == windows_socket_errors_full_pending( &session ) );
    windows_socket_errors_require( in_flight_after == in_flight_before );
    windows_socket_errors_require( rocketnet_client_transport_state( session.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    windows_socket_errors_require( rocketnet_server_transport_is_client_connected( session.server, session.client_index ) );
    windows_socket_errors_require( rocketnet_server_transport_num_connected_clients( session.server ) == 1 );
    windows_socket_errors_require( session.server_received - server_received_before > 30 );
    windows_socket_errors_require( session.client_received - client_received_before > 30 );

    windows_socket_errors_session_finish( &session );
}

static void windows_server_drops_oversize_datagram( const windows_socket_errors_transport_t * transport )
{
    std::vector<windows_socket_errors_datagram_t> datagrams;
    windows_socket_errors_oversize( datagrams );
    windows_server_drops( transport, "windows_server_drops_oversize_datagram", datagrams );
}

static void windows_server_drops_runt_and_garbage( const windows_socket_errors_transport_t * transport )
{
    std::vector<windows_socket_errors_datagram_t> datagrams;
    windows_socket_errors_runt_and_garbage( datagrams );
    windows_server_drops( transport, "windows_server_drops_runt_and_garbage", datagrams );
}

static void windows_server_counts_prefilter_size( const windows_socket_errors_transport_t * transport, const char * name,
                                                  const std::vector<windows_socket_errors_datagram_t> & datagrams, int counter )
{
    windows_socket_errors_session_t session;
    windows_socket_errors_session_start( &session, transport );

    const uint64_t before = rocketnet_counter_value( rocketnet_server_transport_counters( session.server ), counter );

    windows_socket_errors_inject( &session, rocketnet_server_transport_port( session.server ), datagrams );

    const double drain = rocketnet_platform_time();
    uint64_t counted = 0;
    while ( rocketnet_platform_time() - drain < 0.5 )
    {
        windows_socket_errors_tick( &session );
        counted = rocketnet_counter_value( rocketnet_server_transport_counters( session.server ), counter ) - before;
        if ( counted == datagrams.size() )
            break;
    }

    printf( "%s %s: sent %llu, prefilter size counter %llu\n", name, windows_socket_errors_label( transport ),
            (unsigned long long) datagrams.size(), (unsigned long long) counted );
    fflush( stdout );
    windows_socket_errors_require( counted == datagrams.size() );
    windows_socket_errors_session_finish( &session );
}

static void windows_server_counts_oversize_as_prefilter_too_large_iocp_case()
{
    windows_socket_errors_transport_t transport = { "iocp", false };
    std::vector<windows_socket_errors_datagram_t> datagrams;
    windows_socket_errors_oversize( datagrams );
    windows_server_counts_prefilter_size( &transport, "windows_server_counts_oversize_as_prefilter_too_large_iocp", datagrams,
                                          ROCKETNET_COUNTER_PREFILTER_TOO_LARGE );
}

static void windows_server_counts_oversize_as_prefilter_too_large_rio_case()
{
    windows_socket_errors_transport_t transport = { "rio", false };
    std::vector<windows_socket_errors_datagram_t> datagrams;
    windows_socket_errors_oversize( datagrams );
    windows_server_counts_prefilter_size( &transport, "windows_server_counts_oversize_as_prefilter_too_large_rio", datagrams,
                                          ROCKETNET_COUNTER_PREFILTER_TOO_LARGE );
}

static void windows_server_counts_runt_as_prefilter_too_short_rio_case()
{
    windows_socket_errors_transport_t transport = { "rio", false };
    std::vector<windows_socket_errors_datagram_t> datagrams;
    windows_socket_errors_datagram_t empty;
    windows_socket_errors_datagram_t one;
    one.bytes.push_back( 1 );
    datagrams.push_back( empty );
    datagrams.push_back( one );
    windows_server_counts_prefilter_size( &transport, "windows_server_counts_runt_as_prefilter_too_short_rio", datagrams,
                                          ROCKETNET_COUNTER_PREFILTER_TOO_SHORT );
}

// the prefilter counters a client's refusal of a datagram from the server it
// dialed lands on (sdk/rocketnet_protocol.cpp:174-193): short, large, then
// the epoch byte, then the filter value. Seeded garbage never carries the
// session's filter value.
struct windows_socket_errors_prefilter_t
{
    uint64_t too_short;
    uint64_t too_large;
    uint64_t wrong_epoch;
    uint64_t wrong_value;
};

static void windows_socket_errors_prefilter_classify( const windows_socket_errors_datagram_t * datagram, windows_socket_errors_prefilter_t * expect )
{
    const int bytes = (int) datagram->bytes.size();
    if ( bytes < ROCKETNET_PREFILTER_BYTES )
    {
        expect->too_short++;
    }
    else if ( bytes > ROCKETNET_RECEIVE_CEILING )
    {
        expect->too_large++;
    }
    else if ( datagram->bytes[1] != ROCKETNET_FILTER_EPOCH_UNKEYED )
    {
        expect->wrong_epoch++;
    }
    else
    {
        expect->wrong_value++;
    }
}

static void windows_socket_errors_prefilter_read( struct rocketnet_client_transport_t * client, windows_socket_errors_prefilter_t * out )
{
    const struct rocketnet_counters_t * counters = rocketnet_client_transport_counters( client );
    out->too_short = rocketnet_counter_value( counters, ROCKETNET_COUNTER_PREFILTER_TOO_SHORT );
    out->too_large = rocketnet_counter_value( counters, ROCKETNET_COUNTER_PREFILTER_TOO_LARGE );
    out->wrong_epoch = rocketnet_counter_value( counters, ROCKETNET_COUNTER_PREFILTER_WRONG_EPOCH );
    out->wrong_value = rocketnet_counter_value( counters, ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE );
}

// the client: the same datagrams at a connected client's bound port, twice.
// First from the server's own socket (reached through the portable server
// transport's instance, rocketnet_portable_server_test_socket),
// the one source the client listens to: every datagram reaches the client's
// prefilter (sdk/rocketnet_client_protocol.cpp:1248-1253) and lands on the
// counter its size and bytes name. rocketnet_platform_socket_send_packet
// sends no zero-length datagram (it asserts packet_bytes > 0), so the empty
// one goes in the second leg only. Then from the raw test socket, a source
// the client did not dial: the client trusts only the server (TRANSPORT.md
// t11), its socket is connected to the server, so the stack drops every one
// of these, the empty one included, before the client reads it: none counts
// CLIENT_RECEIVE_FOREIGN_SOURCE, none is a refusal and none reaches the
// prefilter. On an unconnected socket each would count
// CLIENT_RECEIVE_FOREIGN_SOURCE. The case proves each server datagram is
// counted once, the I/O thread keeps receiving and the client stays connected.
static void windows_client_drops_oversize_and_garbage( bool ipv6 )
{
    windows_socket_errors_transport_t transport = { "portable", ipv6 };
    windows_socket_errors_session_t session;
    windows_socket_errors_session_start( &session, &transport );

    std::vector<windows_socket_errors_datagram_t> datagrams;
    windows_socket_errors_oversize( datagrams );
    windows_socket_errors_runt_and_garbage( datagrams );

    // a quiet quarter second, then the client's refusals are the baseline

    const double quiet = rocketnet_platform_time();
    while ( rocketnet_platform_time() - quiet < 0.25 )
    {
        windows_socket_errors_tick( &session );
    }

    const uint16_t port = rocketnet_client_transport_test_bound_port( session.client );
    windows_socket_errors_require( port != 0 );

    rocketnet_address_t client_address;
    char client_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( client_string, sizeof( client_string ), "%s:%d", windows_socket_errors_host( ipv6 ), (int) port );
    windows_socket_errors_require( rocketnet_address_parse( &client_address, client_string ) );

    rocketnet_platform_socket_t * server_socket = rocketnet_portable_server_test_socket( rocketnet_server_transport_io_name_for_testing( session.server ) );
    windows_socket_errors_require( server_socket != NULL );

    // leg 1: from the server's socket, to the prefilter

    windows_socket_errors_prefilter_t expect;
    memset( &expect, 0, sizeof( expect ) );

    windows_socket_errors_prefilter_t prefilter_before;
    windows_socket_errors_prefilter_read( session.client, &prefilter_before );

    const uint64_t refusals_before = windows_socket_errors_refusals( rocketnet_client_transport_counters( session.client ) );
    const uint64_t foreign_before = rocketnet_counter_value( rocketnet_client_transport_counters( session.client ), ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE );
    const uint64_t server_received_before = session.server_received;
    const uint64_t client_received_before = session.client_received;
    uint64_t sent_from_server = 0;

    for ( size_t i = 0; i < datagrams.size(); i++ )
    {
        if ( datagrams[i].bytes.empty() )
            continue;
        rocketnet_platform_socket_send_packet( server_socket, &client_address, &datagrams[i].bytes[0], (int) datagrams[i].bytes.size() );
        windows_socket_errors_prefilter_classify( &datagrams[i], &expect );
        sent_from_server++;
        if ( sent_from_server % WINDOWS_SOCKET_ERRORS_SENDS_PER_TICK == 0 )
        {
            windows_socket_errors_tick( &session );
        }
    }

    const double drain = rocketnet_platform_time();
    while ( rocketnet_platform_time() - drain < 0.5 )
    {
        windows_socket_errors_tick( &session );
    }

    windows_socket_errors_prefilter_t prefilter_after;
    windows_socket_errors_prefilter_read( session.client, &prefilter_after );

    const uint64_t too_short = prefilter_after.too_short - prefilter_before.too_short;
    const uint64_t too_large = prefilter_after.too_large - prefilter_before.too_large;
    const uint64_t wrong_epoch = prefilter_after.wrong_epoch - prefilter_before.wrong_epoch;
    const uint64_t wrong_value = prefilter_after.wrong_value - prefilter_before.wrong_value;
    const uint64_t server_leg_refusals = windows_socket_errors_refusals( rocketnet_client_transport_counters( session.client ) ) - refusals_before;
    const uint64_t server_leg_foreign = rocketnet_counter_value( rocketnet_client_transport_counters( session.client ), ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE ) - foreign_before;

    // leg 2: from the raw test socket, a foreign source the connected socket drops

    const uint64_t refusals_mid = windows_socket_errors_refusals( rocketnet_client_transport_counters( session.client ) );
    const uint64_t foreign_mid = rocketnet_counter_value( rocketnet_client_transport_counters( session.client ), ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE );
    windows_socket_errors_prefilter_t prefilter_mid;
    windows_socket_errors_prefilter_read( session.client, &prefilter_mid );
    windows_socket_errors_inject( &session, port, datagrams );

    const double drain_foreign = rocketnet_platform_time();
    while ( rocketnet_platform_time() - drain_foreign < 0.5 )
    {
        windows_socket_errors_tick( &session );
    }

    const uint64_t foreign_refusals = windows_socket_errors_refusals( rocketnet_client_transport_counters( session.client ) ) - refusals_mid;
    const uint64_t foreign_drops = rocketnet_counter_value( rocketnet_client_transport_counters( session.client ), ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE ) - foreign_mid;
    windows_socket_errors_prefilter_t prefilter_end;
    windows_socket_errors_prefilter_read( session.client, &prefilter_end );

    const bool prefilter_unchanged = memcmp( &prefilter_end, &prefilter_mid, sizeof( prefilter_end ) ) == 0;

    // after both floods the I/O thread still receives: a fresh run of ticks
    // delivers the server's payloads

    const uint64_t client_received_mid = session.client_received;
    const double after = rocketnet_platform_time();
    while ( rocketnet_platform_time() - after < 0.5 )
    {
        windows_socket_errors_tick( &session );
    }

    printf( "windows_client_drops_oversize_and_garbage %s: from the server %llu (too short %llu of %llu, too large %llu of %llu, wrong epoch %llu of %llu, wrong value %llu of %llu, refusals %llu, foreign %llu); from a foreign source %llu (foreign source %llu, refusals %llu, prefilter %s); client received %llu payloads during and %llu after, server received %llu\n",
            ipv6 ? "ipv6" : "ipv4", (unsigned long long) sent_from_server,
            (unsigned long long) too_short, (unsigned long long) expect.too_short, (unsigned long long) too_large, (unsigned long long) expect.too_large,
            (unsigned long long) wrong_epoch, (unsigned long long) expect.wrong_epoch, (unsigned long long) wrong_value, (unsigned long long) expect.wrong_value,
            (unsigned long long) server_leg_refusals, (unsigned long long) server_leg_foreign,
            (unsigned long long) datagrams.size(), (unsigned long long) foreign_drops, (unsigned long long) foreign_refusals, prefilter_unchanged ? "unchanged" : "changed",
            (unsigned long long) ( client_received_mid - client_received_before ), (unsigned long long) ( session.client_received - client_received_mid ),
            (unsigned long long) ( session.server_received - server_received_before ) );
    fflush( stdout );

    // every dropped datagram is counted, once, on the counter its source,
    // size and bytes name

    windows_socket_errors_require( too_short == expect.too_short );
    windows_socket_errors_require( too_large == expect.too_large );
    windows_socket_errors_require( wrong_epoch == expect.wrong_epoch );
    windows_socket_errors_require( wrong_value == expect.wrong_value );
    windows_socket_errors_require( server_leg_refusals == sent_from_server );
    windows_socket_errors_require( server_leg_foreign == 0 );
    windows_socket_errors_require( foreign_drops == 0 );
    windows_socket_errors_require( foreign_refusals == 0 );
    windows_socket_errors_require( prefilter_unchanged );
    windows_socket_errors_require( rocketnet_client_transport_state( session.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    windows_socket_errors_require( rocketnet_server_transport_is_client_connected( session.server, session.client_index ) );
    windows_socket_errors_require( session.client_received - client_received_mid > 20 );
    windows_socket_errors_require( session.server_received - server_received_before > 30 );

    windows_socket_errors_session_finish( &session );
}

// the per-packet receive (rocketnet_platform_socket_receive_packet) returns 0
// for a drained socket, so it reads past a zero-length datagram
// (sdk/rocketnet_platform.cpp, rocketnet_platform_socket_receive_datagram with
// keep_empty false) and counts it on rocketnet_platform_receive_empties. The
// case sends a zero-length datagram and then a real one at a platform socket:
// the first call returns the real one, and the empty one is counted once.
static void windows_per_packet_receive_counts_empty_datagram( bool ipv6 )
{
    rocketnet_address_t bind_address;
    rocketnet_address_parse( &bind_address, ipv6 ? "[::1]:0" : "127.0.0.1:0" );

    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_BLOCKING, 0.5f, 64 * 1024, 64 * 1024 );
    windows_socket_errors_require( socket != NULL );
    windows_socket_errors_require( bind_address.port != 0 );

    const uint64_t empties_before = rocketnet_platform_receive_empties();

    windows_socket_errors_sender_t sender;
    windows_socket_errors_sender_open( &sender, ipv6, bind_address.port );

    windows_socket_errors_datagram_t empty;
    windows_socket_errors_sender_send( &sender, &empty );

    windows_socket_errors_datagram_t real;
    real.bytes.resize( 64, 0x5a );
    windows_socket_errors_sender_send( &sender, &real );
    windows_socket_errors_sender_close( &sender );

    uint8_t packet[ROCKETNET_RECEIVE_PACKET_BYTES];
    rocketnet_address_t from;
    const int first = rocketnet_platform_socket_receive_packet( socket, &from, packet, (int) sizeof( packet ) );
    const uint64_t empties = rocketnet_platform_receive_empties() - empties_before;
    const int second = rocketnet_platform_socket_receive_packet( socket, &from, packet, (int) sizeof( packet ) );

    printf( "windows_per_packet_receive_counts_empty_datagram %s: first receive %d bytes, empties counted %llu, second receive %d bytes\n",
            ipv6 ? "ipv6" : "ipv4", first, (unsigned long long) empties, second );
    fflush( stdout );

    windows_socket_errors_require( first == 64 );
    windows_socket_errors_require( empties == 1 );
    windows_socket_errors_require( second == 0 );

    rocketnet_platform_socket_destroy( socket );
}

#define WINDOWS_SOCKET_ERRORS_CASES( name )                                                                     \
    void name##_portable_ipv4() { windows_socket_errors_transport_t t = { "portable", false }; name( &t ); }   \
    void name##_portable_ipv6() { windows_socket_errors_transport_t t = { "portable", true };  name( &t ); }   \
    void name##_iocp_ipv4()     { windows_socket_errors_transport_t t = { "iocp", false };     name( &t ); }   \
    void name##_iocp_ipv6()     { windows_socket_errors_transport_t t = { "iocp", true };      name( &t ); }   \
    void name##_rio_ipv4()      { windows_socket_errors_transport_t t = { "rio", false };      name( &t ); }   \
    void name##_rio_ipv6()      { windows_socket_errors_transport_t t = { "rio", true };       name( &t ); }

WINDOWS_SOCKET_ERRORS_CASES( windows_server_drops_oversize_datagram )
WINDOWS_SOCKET_ERRORS_CASES( windows_server_drops_runt_and_garbage )

void windows_server_counts_oversize_as_prefilter_too_large_iocp() { windows_server_counts_oversize_as_prefilter_too_large_iocp_case(); }
void windows_server_counts_oversize_as_prefilter_too_large_rio() { windows_server_counts_oversize_as_prefilter_too_large_rio_case(); }
void windows_server_counts_runt_as_prefilter_too_short_rio() { windows_server_counts_runt_as_prefilter_too_short_rio_case(); }

void windows_client_drops_oversize_and_garbage_ipv4() { windows_client_drops_oversize_and_garbage( false ); }
void windows_client_drops_oversize_and_garbage_ipv6() { windows_client_drops_oversize_and_garbage( true ); }

void windows_per_packet_receive_counts_empty_datagram_ipv4() { windows_per_packet_receive_counts_empty_datagram( false ); }
void windows_per_packet_receive_counts_empty_datagram_ipv6() { windows_per_packet_receive_counts_empty_datagram( true ); }

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
