/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Datagram weather at the platform socket, on every pump a POSIX host
    offers: oversize, runt, random-garbage and zero-length datagrams from a
    raw sender, the prefilter counters they move, and the test-only send
    seam that reports ENOBUFS and EAGAIN without reaching the kernel. A
    real burst then prices the send path under load: sent equals received
    plus the platform's counted drops.
*/

#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_wire.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <unistd.h>

#if !defined(_WIN32) && defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)

static void rocketnet_posix_check_impl( bool condition, const char * text, const char * file, int line )
{
    if ( condition )
        return;
    printf( "check failed: ( %s ), file %s, line %d\n", text, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_posix_check( condition ) rocketnet_posix_check_impl( (condition), #condition, __FILE__, __LINE__ )

static bool rocketnet_posix_receive_one( rocketnet_platform_socket_t * socket, uint8_t * data, int capacity, int * bytes )
{
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( rocketnet_platform_time() < deadline )
    {
        rocketnet_address_t from;
        int count = rocketnet_platform_socket_receive_packets( socket, &from, data, capacity, bytes, 1 );
        if ( count > 0 )
            return true;
        rocketnet_platform_sleep( 0.001 );
    }
    return false;
}

static void rocketnet_posix_send_seam_family( const char * bind_string, const char * local_string )
{
    rocketnet_address_t bind_address;
    rocketnet_address_t local_address;
    rocketnet_address_parse( &bind_address, bind_string );
    rocketnet_address_parse( &local_address, local_string );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
    if ( socket == NULL )
        return;
    local_address.port = bind_address.port;
    local_address.type = bind_address.type;

    const char * pump_before = rocketnet_platform_socket_pump( socket );

    (void) rocketnet_platform_take_send_failures();

    uint8_t packet[64];
    for ( int i = 0; i < (int) sizeof(packet); i++ )
        packet[i] = (uint8_t) ( 0xa0 + i );

    uint8_t receive_data[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;

    // ENOBUFS: the next three sends are dropped, not paused.

    rocketnet_platform_socket_fail_next_sends( 3, ENOBUFS );
    for ( int i = 0; i < 3; i++ )
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, (int) sizeof(packet) );
    rocketnet_posix_check( rocketnet_platform_socket_send_failures() == 3 );
    rocketnet_posix_check( rocketnet_platform_take_send_failures() == 3 );
    rocketnet_posix_check( rocketnet_platform_socket_receive_packets( socket, &local_address, receive_data, ROCKETNET_RECEIVE_CEILING, &packet_bytes, 1 ) == 0 );
    rocketnet_posix_check( strcmp( rocketnet_platform_socket_pump( socket ), pump_before ) == 0 );

    // EAGAIN: the next two sends are dropped, not paused.

    rocketnet_platform_socket_fail_next_sends( 2, EAGAIN );
    for ( int i = 0; i < 2; i++ )
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, (int) sizeof(packet) );
    rocketnet_posix_check( rocketnet_platform_socket_send_failures() == 2 );
    rocketnet_posix_check( rocketnet_platform_take_send_failures() == 2 );
    rocketnet_posix_check( rocketnet_platform_socket_receive_packets( socket, &local_address, receive_data, ROCKETNET_RECEIVE_CEILING, &packet_bytes, 1 ) == 0 );
    rocketnet_posix_check( strcmp( rocketnet_platform_socket_pump( socket ), pump_before ) == 0 );

    // traffic resumes on the next send, on the same pump.

    rocketnet_platform_socket_send_packet( socket, &local_address, packet, (int) sizeof(packet) );
    rocketnet_posix_check( rocketnet_posix_receive_one( socket, receive_data, ROCKETNET_RECEIVE_CEILING, &packet_bytes ) );
    rocketnet_posix_check( packet_bytes == (int) sizeof(packet) );
    for ( int i = 0; i < packet_bytes; i++ )
        rocketnet_posix_check( receive_data[i] == packet[i] );
    rocketnet_posix_check( strcmp( rocketnet_platform_socket_pump( socket ), pump_before ) == 0 );

    rocketnet_platform_socket_destroy( socket );
}

void rocketnet_test_posix_platform_send_seam()
{
    // the seam is a platform seam: this is its contract, before any
    // transport-level case drives a server or client with it.

    rocketnet_posix_send_seam_family( "0.0.0.0", "127.0.0.1" );
    rocketnet_posix_send_seam_family( "::", "::1" );
}

// ---------------------------------------------------
// receive weather: oversize, runts, garbage and zero-length chaff

#define ROCKETNET_POSIX_WEATHER_MAX_DATAGRAMS   1300
#define ROCKETNET_POSIX_WEATHER_GARBAGE         1000
#define ROCKETNET_POSIX_WEATHER_VALID           20
#define ROCKETNET_POSIX_WEATHER_VALID_BYTES     64

static int      weather_size[ROCKETNET_POSIX_WEATHER_MAX_DATAGRAMS];
static uint64_t weather_hash[ROCKETNET_POSIX_WEATHER_MAX_DATAGRAMS];
static bool     weather_valid[ROCKETNET_POSIX_WEATHER_MAX_DATAGRAMS];
static uint8_t  weather_valid_data[ROCKETNET_POSIX_WEATHER_VALID][ROCKETNET_POSIX_WEATHER_VALID_BYTES];

static uint64_t rocketnet_posix_hash( const uint8_t * data, int bytes )
{
    uint64_t hash = 1469598103934665603ULL;
    for ( int i = 0; i < bytes; i++ )
    {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static int rocketnet_posix_prefix_bytes( int bytes )
{
    return bytes < ROCKETNET_RECEIVE_CEILING ? bytes : ROCKETNET_RECEIVE_CEILING;
}

static uint64_t rocketnet_posix_next_random( uint64_t * state )
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static int rocketnet_posix_open_injector( const char * local_string, uint16_t port, struct sockaddr_storage * storage, socklen_t * storage_bytes )
{
    memset( storage, 0, sizeof(*storage) );
    if ( local_string[0] == ':' )
    {
        struct sockaddr_in6 * address = (struct sockaddr_in6*) storage;
        address->sin6_family = AF_INET6;
        address->sin6_port = htons( port );
        rocketnet_posix_check( inet_pton( AF_INET6, local_string, &address->sin6_addr ) == 1 );
        *storage_bytes = sizeof(struct sockaddr_in6);
        return socket( AF_INET6, SOCK_DGRAM, IPPROTO_UDP );
    }
    struct sockaddr_in * address = (struct sockaddr_in*) storage;
    address->sin_family = AF_INET;
    address->sin_port = htons( port );
    address->sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    *storage_bytes = sizeof(struct sockaddr_in);
    return socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
}

static void rocketnet_posix_weather_wait( rocketnet_platform_socket_t * socket )
{
    const double deadline = rocketnet_platform_time() + 5.0;
    for ( ;; )
    {
        fd_set readable;
        FD_ZERO( &readable );
        FD_SET( socket->handle, &readable );
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        if ( select( socket->handle + 1, &readable, NULL, NULL, &tv ) > 0 )
            break;
        rocketnet_posix_check( rocketnet_platform_time() < deadline );
    }
    usleep( 200000 );
}

static void rocketnet_posix_weather_cell( const char * bind_string, const char * local_string, const char * pump_override )
{
    if ( pump_override )
        setenv( "ROCKETNET_PUMP", pump_override, 1 );
    else
        unsetenv( "ROCKETNET_PUMP" );

    rocketnet_address_t bind_address;
    rocketnet_address_t local_address;
    rocketnet_address_parse( &bind_address, bind_string );
    rocketnet_address_parse( &local_address, local_string );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
    rocketnet_posix_check( socket != NULL );
    local_address.port = bind_address.port;
    local_address.type = bind_address.type;

    const char * pump_at_create = rocketnet_platform_socket_pump( socket );
    if ( pump_override )
        rocketnet_posix_check( strcmp( pump_at_create, pump_override ) == 0 );

    struct sockaddr_storage to;
    socklen_t to_bytes = 0;
    int injector = rocketnet_posix_open_injector( local_string, local_address.port, &to, &to_bytes );
    rocketnet_posix_check( injector >= 0 );

    int records = 0;
    int valid_count = 0;
    uint64_t too_short = 0;
    uint64_t too_large = 0;
    uint64_t wrong_epoch = 0;
    uint64_t wrong_value = 0;

    uint8_t * buffer = (uint8_t*) malloc( 65535 );
    rocketnet_posix_check( buffer != NULL );

    const uint8_t expected_filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;

    // one zero-length datagram first: legal UDP carrying nothing the
    // protocol admits, and never a drain terminator

    rocketnet_posix_check( sendto( injector, buffer, 0, 0, (struct sockaddr*) &to, to_bytes ) == 0 );

    // an oversize datagram: its true length reaches the transport, which
    // refuses it as too large before reading the truncated payload

    {
        const int bytes = 65507;
        for ( int i = 0; i < bytes; i++ )
            buffer[i] = (uint8_t) ( 0x11 + i );
        rocketnet_posix_check( sendto( injector, buffer, (size_t) bytes, 0, (struct sockaddr*) &to, to_bytes ) == bytes );
        weather_size[records] = bytes;
        weather_hash[records] = rocketnet_posix_hash( buffer, rocketnet_posix_prefix_bytes( bytes ) );
        weather_valid[records] = false;
        records++;
        too_large++;
    }

    // one zero-length datagram between the weather classes

    rocketnet_posix_check( sendto( injector, buffer, 0, 0, (struct sockaddr*) &to, to_bytes ) == 0 );

    // two runts: one byte, and one byte below the prefilter header

    {
        const int runts[2] = { 1, ROCKETNET_PREFILTER_BYTES - 1 };
        for ( int r = 0; r < 2; r++ )
        {
            const int bytes = runts[r];
            for ( int i = 0; i < bytes; i++ )
                buffer[i] = (uint8_t) ( 0x40 + 7 * r + i );
            rocketnet_posix_check( sendto( injector, buffer, (size_t) bytes, 0, (struct sockaddr*) &to, to_bytes ) == bytes );
            weather_size[records] = bytes;
            weather_hash[records] = rocketnet_posix_hash( buffer, bytes );
            weather_valid[records] = false;
            records++;
            too_short++;
        }
    }

    // a seeded storm: 1000 random datagrams of random sizes. the expected
    // prefilter outcome is computed from each one as it is built.

    uint64_t random_state = 0x9e3779b97f4a7c15ULL;
    for ( int g = 0; g < ROCKETNET_POSIX_WEATHER_GARBAGE; g++ )
    {
        int bytes = (int) ( rocketnet_posix_next_random( &random_state ) % 1500 ) + 1;
        for ( int i = 0; i < bytes; i++ )
            buffer[i] = (uint8_t) rocketnet_posix_next_random( &random_state );

        if ( bytes < ROCKETNET_PREFILTER_BYTES )
            too_short++;
        else if ( bytes > ROCKETNET_RECEIVE_CEILING )
            too_large++;
        else if ( buffer[1] != ROCKETNET_FILTER_EPOCH_UNKEYED )
            wrong_epoch++;
        else if ( memcmp( buffer + 2, expected_filter, ROCKETNET_FILTER_VALUE_BYTES ) != 0 )
            wrong_value++;

        rocketnet_posix_check( sendto( injector, buffer, (size_t) bytes, 0, (struct sockaddr*) &to, to_bytes ) == bytes );
        weather_size[records] = bytes;
        weather_hash[records] = rocketnet_posix_hash( buffer, rocketnet_posix_prefix_bytes( bytes ) );
        weather_valid[records] = false;
        records++;

        // valid traffic interleaved with the storm, a zero-length datagram
        // in front of each one

        if ( ( g % ( ROCKETNET_POSIX_WEATHER_GARBAGE / ROCKETNET_POSIX_WEATHER_VALID ) ) == 0 &&
             valid_count < ROCKETNET_POSIX_WEATHER_VALID )
        {
            rocketnet_posix_check( sendto( injector, buffer, 0, 0, (struct sockaddr*) &to, to_bytes ) == 0 );

            const int valid_bytes = ROCKETNET_POSIX_WEATHER_VALID_BYTES;
            uint8_t * valid = weather_valid_data[valid_count];
            memset( valid, 0, (size_t) valid_bytes );
            valid[0] = 10;  // a reserved type: the prefilter admits it, the transport ignores it
            valid[1] = ROCKETNET_FILTER_EPOCH_UNKEYED;
            memcpy( valid + 2, expected_filter, ROCKETNET_FILTER_VALUE_BYTES );
            valid[6] = (uint8_t) valid_count;
            valid[7] = (uint8_t) ~valid_count;
            for ( int i = 8; i < valid_bytes; i++ )
                valid[i] = (uint8_t) ( valid_count * 31 + i );

            rocketnet_posix_check( sendto( injector, valid, (size_t) valid_bytes, 0, (struct sockaddr*) &to, to_bytes ) == valid_bytes );
            weather_size[records] = valid_bytes;
            weather_hash[records] = rocketnet_posix_hash( valid, valid_bytes );
            weather_valid[records] = true;
            records++;
            valid_count++;
        }
    }

    // and one zero-length datagram last: the drain still has to reach every
    // valid packet behind it

    rocketnet_posix_check( sendto( injector, buffer, 0, 0, (struct sockaddr*) &to, to_bytes ) == 0 );

    rocketnet_posix_weather_wait( socket );

    static uint8_t receive_data[ROCKETNET_POSIX_WEATHER_MAX_DATAGRAMS * ROCKETNET_RECEIVE_CEILING];
    static rocketnet_address_t received_from[ROCKETNET_POSIX_WEATHER_MAX_DATAGRAMS];
    static int received_bytes[ROCKETNET_POSIX_WEATHER_MAX_DATAGRAMS];

    const int received = rocketnet_platform_socket_receive_packets( socket, received_from, receive_data,
                                                                    ROCKETNET_RECEIVE_CEILING, received_bytes,
                                                                    ROCKETNET_POSIX_WEATHER_MAX_DATAGRAMS );
    rocketnet_posix_check( received == records );

    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    struct rocketnet_server_protocol_t server;
    memset( &server, 0, sizeof(server) );
    server.counters = &counters;
    memcpy( server.filter_value, expected_filter, ROCKETNET_FILTER_VALUE_BYTES );

    int valid_seen = 0;
    for ( int i = 0; i < received; i++ )
    {
        const int bytes = received_bytes[i];
        rocketnet_posix_check( bytes == weather_size[i] );
        const uint8_t * packet = receive_data + (size_t) i * ROCKETNET_RECEIVE_CEILING;
        rocketnet_posix_check( rocketnet_posix_hash( packet, rocketnet_posix_prefix_bytes( bytes ) ) == weather_hash[i] );

        if ( weather_valid[i] )
        {
            rocketnet_posix_check( bytes == ROCKETNET_POSIX_WEATHER_VALID_BYTES );
            rocketnet_posix_check( memcmp( packet, weather_valid_data[valid_seen], (size_t) bytes ) == 0 );
            valid_seen++;
        }

        uint8_t response[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;
        uint8_t payload[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        int payload_client_index = -1;
        (void) rocketnet_server_protocol_process_packet( &server, &received_from[i], packet, bytes,
                                                         0, 0.0, response, &response_bytes,
                                                         payload, &payload_bytes, &payload_client_index );
    }

    rocketnet_posix_check( valid_seen == ROCKETNET_POSIX_WEATHER_VALID );
    rocketnet_posix_check( counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == (uint64_t) received );
    rocketnet_posix_check( counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_SHORT] == too_short );
    rocketnet_posix_check( counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == too_large );
    rocketnet_posix_check( counters.values[ROCKETNET_COUNTER_PREFILTER_WRONG_EPOCH] == wrong_epoch );
    rocketnet_posix_check( counters.values[ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE] == wrong_value );

    // the pump that carried the weather is still the pump the socket chose

    rocketnet_posix_check( strcmp( rocketnet_platform_socket_pump( socket ), pump_at_create ) == 0 );

    close( injector );
    free( buffer );
    rocketnet_platform_socket_destroy( socket );

    unsetenv( "ROCKETNET_PUMP" );
}

void rocketnet_test_posix_receive_weather()
{
    // both address families, both pumps a POSIX host offers: the platform's
    // default and the portable pump forced by environment

    for ( int family = 0; family < 2; family++ )
    {
        const char * bind_string = family == 0 ? "0.0.0.0" : "::";
        const char * local_string = family == 0 ? "127.0.0.1" : "::1";

        rocketnet_posix_weather_cell( bind_string, local_string, NULL );
        rocketnet_posix_weather_cell( bind_string, local_string, "portable" );
    }
}

// ---------------------------------------------------
// a real burst: 64 clients, 512 packets of 1200 bytes in one update each,
// sender buffers at the kernel minimum

#define ROCKETNET_POSIX_BURST_CLIENTS   64
#define ROCKETNET_POSIX_BURST_PACKETS   512
#define ROCKETNET_POSIX_BURST_BYTES     1200
#define ROCKETNET_POSIX_BURST_STRIDE    ROCKETNET_POSIX_BURST_BYTES

struct rocketnet_posix_burst_drain_t
{
    rocketnet_platform_socket_t * socket;
    volatile bool stop;
    volatile bool running;
    volatile uint64_t received;
    bool seen[ROCKETNET_POSIX_BURST_CLIENTS][ROCKETNET_POSIX_BURST_PACKETS];
};

static void rocketnet_posix_burst_drain_thread( void * context )
{
    struct rocketnet_posix_burst_drain_t * drain = (struct rocketnet_posix_burst_drain_t*) context;
    static uint8_t receive_data[64 * ROCKETNET_POSIX_BURST_STRIDE];
    static rocketnet_address_t from[64];
    static int packet_bytes[64];

    drain->running = true;

    while ( !drain->stop )
    {
        const int received = rocketnet_platform_socket_receive_packets( drain->socket, from, receive_data,
                                                                        ROCKETNET_POSIX_BURST_STRIDE, packet_bytes, 64 );
        if ( received == 0 )
        {
            rocketnet_platform_sleep( 0.0002 );
            continue;
        }
        for ( int i = 0; i < received; i++ )
        {
            const uint8_t * packet = receive_data + (size_t) i * ROCKETNET_POSIX_BURST_STRIDE;
            rocketnet_posix_check( packet_bytes[i] == ROCKETNET_POSIX_BURST_BYTES );
            uint32_t client = 0;
            uint32_t sequence = 0;
            memcpy( &client, packet, sizeof(client) );
            memcpy( &sequence, packet + sizeof(client), sizeof(sequence) );
            rocketnet_posix_check( client < ROCKETNET_POSIX_BURST_CLIENTS );
            rocketnet_posix_check( sequence < ROCKETNET_POSIX_BURST_PACKETS );
            drain->seen[client][sequence] = true;
            drain->received++;
        }
    }
}

void rocketnet_test_posix_send_burst()
{
    rocketnet_address_t server_bind;
    rocketnet_address_parse( &server_bind, "0.0.0.0" );
    rocketnet_platform_socket_t * server = rocketnet_platform_socket_create( NULL, &server_bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0,
                                                                             1024*1024, 1024*1024 );
    rocketnet_posix_check( server != NULL );

    rocketnet_address_t destination;
    rocketnet_address_parse( &destination, "127.0.0.1" );
    destination.port = server_bind.port;

    struct rocketnet_posix_burst_drain_t drain;
    memset( &drain, 0, sizeof(drain) );
    drain.socket = server;

    rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( NULL, rocketnet_posix_burst_drain_thread, &drain );
    rocketnet_posix_check( thread != NULL );
    while ( !drain.running )
        rocketnet_platform_sleep( 0.0005 );
    rocketnet_platform_sleep( 0.05 );

    (void) rocketnet_platform_take_send_failures();

    static uint8_t packet[ROCKETNET_POSIX_BURST_PACKETS][ROCKETNET_POSIX_BURST_BYTES];
    static struct rocketnet_platform_send_t sends[ROCKETNET_POSIX_BURST_PACKETS];

    for ( uint32_t client = 0; client < ROCKETNET_POSIX_BURST_CLIENTS; client++ )
    {
        rocketnet_address_t client_bind;
        rocketnet_address_parse( &client_bind, "0.0.0.0" );
        // the kernel minimum for SO_SNDBUF; the kernel doubles it
        rocketnet_platform_socket_t * sender = rocketnet_platform_socket_create( NULL, &client_bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1, 1024*1024 );
        rocketnet_posix_check( sender != NULL );

        for ( uint32_t sequence = 0; sequence < ROCKETNET_POSIX_BURST_PACKETS; sequence++ )
        {
            uint8_t * bytes = packet[sequence];
            memcpy( bytes, &client, sizeof(client) );
            memcpy( bytes + sizeof(client), &sequence, sizeof(sequence) );
            for ( int i = (int) ( sizeof(client) + sizeof(sequence) ); i < ROCKETNET_POSIX_BURST_BYTES; i++ )
                bytes[i] = (uint8_t) ( client + sequence + i );
            sends[sequence].to = &destination;
            sends[sequence].packet_data = bytes;
            sends[sequence].packet_bytes = ROCKETNET_POSIX_BURST_BYTES;
        }

        rocketnet_platform_socket_send_packets( sender, sends, ROCKETNET_POSIX_BURST_PACKETS );
        rocketnet_platform_socket_destroy( sender );

        // let the drain catch up between clients: a burst that fills the
        // receive queue past the kernel's ceiling would price receiver loss
        // the send path cannot see, and this case is about the send path

        const uint64_t expected = (uint64_t) ( client + 1 ) * ROCKETNET_POSIX_BURST_PACKETS;
        const double catch_up = rocketnet_platform_time() + 0.2;
        while ( drain.received < expected && rocketnet_platform_time() < catch_up )
            rocketnet_platform_sleep( 0.0005 );
    }

    // let the drain finish what is in flight, then take the final accounting

    rocketnet_platform_sleep( 0.2 );
    drain.stop = true;
    rocketnet_platform_thread_join( thread );
    rocketnet_platform_thread_destroy( thread );

    // Drain anything the thread left after its stop check, counting it.

    static uint8_t tail_data[64 * ROCKETNET_POSIX_BURST_STRIDE];
    static rocketnet_address_t tail_from[64];
    static int tail_bytes[64];
    for ( ;; )
    {
        const int received = rocketnet_platform_socket_receive_packets( server, tail_from, tail_data,
                                                                        ROCKETNET_POSIX_BURST_STRIDE, tail_bytes, 64 );
        if ( received == 0 )
            break;
        for ( int i = 0; i < received; i++ )
        {
            const uint8_t * bytes = tail_data + (size_t) i * ROCKETNET_POSIX_BURST_STRIDE;
            uint32_t client = 0;
            uint32_t sequence = 0;
            memcpy( &client, bytes, sizeof(client) );
            memcpy( &sequence, bytes + sizeof(client), sizeof(sequence) );
            rocketnet_posix_check( client < ROCKETNET_POSIX_BURST_CLIENTS );
            rocketnet_posix_check( sequence < ROCKETNET_POSIX_BURST_PACKETS );
            if ( !drain.seen[client][sequence] )
            {
                drain.seen[client][sequence] = true;
                drain.received++;
            }
        }
    }

    const uint64_t drops = rocketnet_platform_take_send_failures();
    const uint64_t sent = (uint64_t) ROCKETNET_POSIX_BURST_CLIENTS * ROCKETNET_POSIX_BURST_PACKETS;

    printf( "posix burst: sent=%llu received=%llu platform_send_drops=%llu (%s)\n",
            (unsigned long long) sent, (unsigned long long) drain.received,
            (unsigned long long) drops, drops > 0 ? "ENOBUFS-style drops observed" : "no send drop observed" );
    fflush( stdout );

    rocketnet_posix_check( drain.received + drops == sent );

    rocketnet_platform_socket_destroy( server );
}


// ---------------------------------------------------
// a transport reads the platform's dropped-send count into its counters

void rocketnet_test_posix_transport_send_seam()
{
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 1;
    snprintf( server_config.driver, sizeof(server_config.driver), "portable" );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config );
    rocketnet_posix_check( server != NULL );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_posix_check( rocketnet_write_dev_connect_token( server_address, 0xf00dULL, (uint64_t) time( NULL ), token ) );
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, NULL, rocketnet_platform_time() );
    rocketnet_posix_check( client != NULL );

    double start = rocketnet_platform_time();
    bool connected = false;
    while ( rocketnet_platform_time() - start < 10.0 && !connected )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_client_transport_send_packets( client );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        connected = rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_posix_check( connected );

    (void) rocketnet_platform_take_send_failures();

    // ENOBUFS: the next send the server makes is dropped and counted, and
    // the transport is not paused or disconnected by it

    const int client_index = rocketnet_client_transport_client_index( client );
    rocketnet_posix_check( client_index >= 0 );

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_server_transport_start_packet( server, client_index, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
    rocketnet_posix_check( frame != NULL );
    memcpy( frame + context.payload_index, "drop", 4 );
    rocketnet_posix_check( rocketnet_server_transport_finish_packet( server, &context, 4 ) > 0 );

    rocketnet_platform_socket_fail_next_sends( 1, ENOBUFS );
    rocketnet_server_transport_send_packets( server );

    rocketnet_posix_check( rocketnet_platform_take_send_failures() == 1 );
    rocketnet_posix_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    // EAGAIN next: the same accounting, and the connection still lives

    frame = rocketnet_server_transport_start_packet( server, client_index, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
    rocketnet_posix_check( frame != NULL );
    memcpy( frame + context.payload_index, "drop", 4 );
    rocketnet_posix_check( rocketnet_server_transport_finish_packet( server, &context, 4 ) > 0 );

    rocketnet_platform_socket_fail_next_sends( 1, EAGAIN );
    rocketnet_server_transport_send_packets( server );

    rocketnet_posix_check( rocketnet_platform_take_send_failures() == 1 );
    rocketnet_posix_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );
    rocketnet_posix_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // traffic resumes: the next payload arrives whole and the connection
    // stays connected

    frame = rocketnet_server_transport_start_packet( server, client_index, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
    rocketnet_posix_check( frame != NULL );
    memcpy( frame + context.payload_index, "live", 4 );
    rocketnet_posix_check( rocketnet_server_transport_finish_packet( server, &context, 4 ) > 0 );
    rocketnet_server_transport_send_packets( server );

    bool delivered = false;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && !delivered )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        for ( int i = 0; i < rocketnet_client_transport_num_payloads( client ); i++ )
        {
            int payload_bytes = 0;
            uint8_t * payload = rocketnet_client_transport_payload( client, i, &payload_bytes );
            if ( payload && payload_bytes == 4 && memcmp( payload, "live", 4 ) == 0 )
                delivered = true;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_posix_check( delivered );
    rocketnet_posix_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_destroy( server );
}

#endif // #if !defined(_WIN32) && defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)
