/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Socket weather on every POSIX transport cell. A raw sender on loopback
    drives the receive error arms of a live server or client transport with
    an oversize datagram, runts, a seeded storm of random garbage and
    zero-length datagrams interleaved with valid traffic; each cell proves the
    valid traffic arrives exactly, each prefilter counter moves by the exact
    count, the pump the socket chose is the pump it keeps, and the connection
    lives. The test-only send seam then makes a socket's sends report ENOBUFS
    and EAGAIN, and a real loopback burst at the smallest send buffer that
    carries a datagram prices the send path: sent equals received plus the
    drops the senders counted.
*/

#include "../test/native_shutdown.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_protocol.h"
#include "rocketnet_wire.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// defined in functional/harness.inc: the connect token timestamp from the clock that judges it
uint64_t rocketnet_functional_token_timestamp( uint64_t game_clock_epoch, double time );

#if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS) && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <atomic>

#if defined(__linux__)
#include "rocketnet_server_gso.h"
#include "rocketnet_server_uring.h"
#endif

// darwin declares IPV6_DONTFRAG only under __APPLE_USE_RFC_3542, which the
// platform layer defines for itself; the option number is the RFC 3542 one
#if defined(__APPLE__) && !defined(IPV6_DONTFRAG)
#define IPV6_DONTFRAG 62
#endif

static void posix_check_impl( bool condition, const char * text, const char * file, int line )
{
    if ( condition )
        return;
    printf( "check failed: ( %s ), file %s, line %d\n", text, file, line );
    fflush( stdout );
    exit( 1 );
}

#define posix_check( condition ) posix_check_impl( (condition), #condition, __FILE__, __LINE__ )

// ---------------------------------------------------
// the Linux native server transports (gso, io_uring) own their sockets, so no
// platform socket appears for them; their size pass runs before the
// protocol's prefilter and counts in the transport's own RECEIVE_DROPPED_SIZE,
// and the transport reads the same drops into PREFILTER_TOO_SHORT and
// PREFILTER_TOO_LARGE through the builtin transport's receive_drops hook
// (sdk/rocketnet_server_transport.cpp:2774-2786)

void * rocketnet_server_transport_io_name_for_testing( struct rocketnet_server_transport_t * server_transport );

// the portable server transport sends on its own TX worker
// (sdk/rocketnet_driver_builtin.cpp): send_packets copies the update into a
// bank and returns before any send call is made

bool rocketnet_portable_tx_test_drained( void * transport );

// forcing the gso plain tiers: every following gso create probes neither
// UDP_SEGMENT nor UDP_GRO, and each pair proves it got the plain tiers

#if defined(__linux__)
static int posix_gso_forced_plain = 0;

static void posix_force_gso_plain( int on )
{
    rocketnet_server_gso_test_force_create_flags( on ? ( ROCKETNET_SERVER_GSO_DISABLE_SEGMENT | ROCKETNET_SERVER_GSO_DISABLE_GRO ) : 0 );
    posix_gso_forced_plain = on;
}
#endif // #if defined(__linux__)

#define POSIX_NATIVE_NONE   0
#define POSIX_NATIVE_GSO    1
#define POSIX_NATIVE_URING  2

static int posix_native_kind( const char * server_transport_io )
{
#if defined(__linux__)
    if ( strcmp( server_transport_io, "gso" ) == 0 )
        return POSIX_NATIVE_GSO;
    if ( strcmp( server_transport_io, "io_uring" ) == 0 )
        return POSIX_NATIVE_URING;
#else
    (void) server_transport_io;
#endif
    return POSIX_NATIVE_NONE;
}

static uint64_t posix_native_counter( int kind, void * transport, int gso_index, int uring_index )
{
#if defined(__linux__)
    if ( kind == POSIX_NATIVE_GSO )
    {
        uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
        rocketnet_server_gso_counters( (struct rocketnet_server_gso_t*) transport, counters );
        return counters[gso_index];
    }
    if ( kind == POSIX_NATIVE_URING )
    {
        uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
        rocketnet_server_uring_counters( (struct rocketnet_server_uring_t*) transport, counters );
        return counters[uring_index];
    }
#else
    (void) transport; (void) gso_index; (void) uring_index;
#endif
    (void) kind;
    return 0;
}

static uint64_t posix_native_dropped_size( int kind, void * transport )
{
#if defined(__linux__)
    return posix_native_counter( kind, transport, ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_SIZE, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SIZE );
#else
    return posix_native_counter( kind, transport, 0, 0 );
#endif
}

// ---------------------------------------------------
// the socket a create made: the registry diff across the call

#define POSIX_MAX_SOCKETS 256

static int posix_snapshot( rocketnet_platform_socket_t ** sockets )
{
    return rocketnet_platform_sockets_for_testing( sockets, POSIX_MAX_SOCKETS );
}

static rocketnet_platform_socket_t * posix_new_socket( rocketnet_platform_socket_t ** before, int num_before )
{
    rocketnet_platform_socket_t * after[POSIX_MAX_SOCKETS];
    const int num_after = posix_snapshot( after );
    rocketnet_platform_socket_t * found = NULL;
    for ( int i = 0; i < num_after; i++ )
    {
        bool old = false;
        for ( int j = 0; j < num_before; j++ )
            old = old || before[j] == after[i];
        if ( !old )
        {
            posix_check( found == NULL );
            found = after[i];
        }
    }
    posix_check( found != NULL );
    return found;
}

static uint64_t posix_next_random( uint64_t * state )
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static socklen_t posix_sockaddr( int ipv6, uint16_t port, struct sockaddr_storage * storage )
{
    memset( storage, 0, sizeof(*storage) );
    if ( ipv6 )
    {
        struct sockaddr_in6 * address = (struct sockaddr_in6*) storage;
        address->sin6_family = AF_INET6;
        address->sin6_port = htons( port );
        address->sin6_addr = in6addr_loopback;
        return sizeof(struct sockaddr_in6);
    }
    struct sockaddr_in * address = (struct sockaddr_in*) storage;
    address->sin_family = AF_INET;
    address->sin_port = htons( port );
    address->sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    return sizeof(struct sockaddr_in);
}

static int posix_open_raw_sender( int ipv6 )
{
    int s = socket( ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    posix_check( s >= 0 );
    // an oversize datagram needs a send buffer above it (the darwin default
    // is net.inet.udp.maxdgram, 9216 bytes)
    int bytes = 256 * 1024;
    posix_check( setsockopt( s, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes) ) == 0 );
    return s;
}

static void posix_set_dont_fragment( int s, int ipv6, int value )
{
    if ( ipv6 )
    {
#if defined(IPV6_DONTFRAG)
        setsockopt( s, IPPROTO_IPV6, IPV6_DONTFRAG, &value, sizeof(value) );
#endif
    }
    else
    {
#if defined(IP_DONTFRAG)
        setsockopt( s, IPPROTO_IP, IP_DONTFRAG, &value, sizeof(value) );
#endif
    }
#if defined(__linux__)
    int mode = value ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
    setsockopt( s, ipv6 ? IPPROTO_IPV6 : IPPROTO_IP, ipv6 ? IPV6_MTU_DISCOVER : IP_MTU_DISCOVER, &mode, sizeof(mode) );
#endif
}

static uint16_t posix_local_port( rocketnet_platform_socket_t * socket )
{
    struct sockaddr_storage storage;
    socklen_t bytes = sizeof(storage);
    posix_check( getsockname( socket->handle, (struct sockaddr*) &storage, &bytes ) == 0 );
    if ( storage.ss_family == AF_INET6 )
        return ntohs( ( (struct sockaddr_in6*) &storage )->sin6_port );
    return ntohs( ( (struct sockaddr_in*) &storage )->sin_port );
}

// datagrams queued on a socket and not yet read. darwin counts them
// (SO_NUMRCVPKT, zero-length ones included); elsewhere the count is unknown
// and the caller's bounded update loop is the only wait.

static int posix_queued_datagrams( rocketnet_platform_socket_t * socket )
{
#if defined(SO_NUMRCVPKT)
    int count = 0;
    socklen_t bytes = sizeof(count);
    posix_check( getsockopt( socket->handle, SOL_SOCKET, SO_NUMRCVPKT, &count, &bytes ) == 0 );
    return count;
#else
    (void) socket;
    return -1;
#endif
}

static void posix_wait_queued( rocketnet_platform_socket_t * socket, int expected )
{
    const double deadline = rocketnet_platform_time() + 5.0;
    for ( ;; )
    {
        const int queued = posix_queued_datagrams( socket );
        if ( queued < 0 )
        {
            rocketnet_platform_sleep( 0.005 );
            return;
        }
        if ( queued >= expected )
            return;
        posix_check( rocketnet_platform_time() < deadline );
        rocketnet_platform_sleep( 0.0002 );
    }
}

// ---------------------------------------------------
// a connected pair on one cell

struct posix_pair_t
{
    struct rocketnet_server_transport_t * server;
    struct rocketnet_client_transport_t * client;
    rocketnet_platform_socket_t * server_socket;
    rocketnet_platform_socket_t * client_socket;
    const char * server_pump;
    const char * client_pump;
    int ipv6;
    int client_index;
    int native;             // POSIX_NATIVE_*: the server transport owns its sockets
    void * transport;          // the native server transport instance
};

static void posix_pair_update( struct posix_pair_t * pair )
{
    rocketnet_client_transport_send_packets( pair->client );
    rocketnet_client_transport_update( pair->client, rocketnet_platform_time() );
    rocketnet_server_transport_update( pair->server, rocketnet_platform_time() );
    rocketnet_server_transport_send_packets( pair->server );
}

static void posix_pair_check_connected( struct posix_pair_t * pair )
{
    posix_check( rocketnet_client_transport_state( pair->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    posix_check( rocketnet_server_transport_num_connected_clients( pair->server ) == 1 );
    if ( pair->server_socket )
        posix_check( strcmp( rocketnet_platform_socket_pump( pair->server_socket ), pair->server_pump ) == 0 );
    posix_check( strcmp( rocketnet_platform_socket_pump( pair->client_socket ), pair->client_pump ) == 0 );
}

static void posix_pair_create( struct posix_pair_t * pair, int ipv6, const char * server_transport_io, const char * client_transport_io,
                               const char * server_pump, const char * client_pump )
{
    memset( pair, 0, sizeof(*pair) );
    pair->ipv6 = ipv6;
    pair->server_pump = server_pump;
    pair->client_pump = client_pump;

    // each cell names the transports it tests, so a transport pin in the
    // environment (ROCKETNET_SERVER_TRANSPORT, ROCKETNET_CLIENT_TRANSPORT)
    // is lifted for the two creates below and restored after them, as
    // posix_cell_begin does (functional/posix_lifecycle_stress.cpp:492).
    // under a pin the ladder grants the pinned transport, and a gso or io_uring
    // cell casts pair->transport to its own transport type
    // (posix_native_counter, functional/posix_socket_errors.cpp:101;
    // posix_linux_uring_fail_sends, functional/posix_socket_errors.cpp:1309)

    char saved_server_transport[64] = "";
    char saved_client_transport[64] = "";
    const char * pinned_server = getenv( "ROCKETNET_SERVER_TRANSPORT" );
    const char * pinned_client = getenv( "ROCKETNET_CLIENT_TRANSPORT" );
    const bool server_pinned = pinned_server != NULL;
    const bool client_pinned = pinned_client != NULL;
    if ( server_pinned )
        snprintf( saved_server_transport, sizeof(saved_server_transport), "%s", pinned_server );
    if ( client_pinned )
        snprintf( saved_client_transport, sizeof(saved_client_transport), "%s", pinned_client );
    unsetenv( "ROCKETNET_SERVER_TRANSPORT" );
    unsetenv( "ROCKETNET_CLIENT_TRANSPORT" );

    rocketnet_platform_socket_t * before[POSIX_MAX_SOCKETS];

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 1;
    snprintf( server_config.transport, sizeof(server_config.transport), "%s", server_transport_io );

    int num_before = posix_snapshot( before );
    pair->server = rocketnet_server_transport_create( NULL, ipv6 ? "[::1]" : "127.0.0.1", &server_config );
    posix_check( pair->server != NULL );
    if ( strcmp( server_transport_io, "auto" ) != 0 )
        posix_check( strcmp( rocketnet_server_transport_io_name( pair->server ), server_transport_io ) == 0 );
    pair->native = posix_native_kind( server_transport_io );
    if ( pair->native != POSIX_NATIVE_NONE )
        pair->transport = rocketnet_server_transport_io_name_for_testing( pair->server );
    else
        pair->server_socket = posix_new_socket( before, num_before );
#if defined(__linux__)
    if ( pair->native == POSIX_NATIVE_GSO && posix_gso_forced_plain )
        posix_check( ( rocketnet_server_gso_features( (struct rocketnet_server_gso_t*) pair->transport ) &
                       ( ROCKETNET_SERVER_GSO_FEATURE_SEGMENT | ROCKETNET_SERVER_GSO_FEATURE_GRO ) ) == 0 );
#endif

    char server_address[64];
    snprintf( server_address, sizeof(server_address), ipv6 ? "[::1]:%d" : "127.0.0.1:%d", rocketnet_server_transport_port( pair->server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    posix_check( rocketnet_write_test_connect_token( server_address, rocketnet_random_uint64(),
                                                    rocketnet_functional_token_timestamp( server_config.game_clock_epoch, 1000.0 ),
                                                    token, sizeof(token) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    snprintf( client_config.transport, sizeof(client_config.transport), "%s", client_transport_io );

    num_before = posix_snapshot( before );
    pair->client = rocketnet_client_transport_create( NULL, token, &client_config, rocketnet_platform_time() );
    posix_check( pair->client != NULL );
    pair->client_socket = posix_new_socket( before, num_before );

    if ( server_pinned )
        setenv( "ROCKETNET_SERVER_TRANSPORT", saved_server_transport, 1 );
    if ( client_pinned )
        setenv( "ROCKETNET_CLIENT_TRANSPORT", saved_client_transport, 1 );

    const double start = rocketnet_platform_time();
    const double deadline = start + 10.0;
    int connect_updates = 0;
    while ( rocketnet_platform_time() < deadline )
    {
        posix_pair_update( pair );
        connect_updates++;
        if ( rocketnet_client_transport_state( pair->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( pair->server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    pair->client_index = rocketnet_client_transport_client_index( pair->client );
    if ( pair->client_index < 0 )
    {
        printf( "connect did not complete: ipv%d server=%s client=%s client state %d, server connected %d, %d updates in %.3f s\n",
                ipv6 ? 6 : 4, server_transport_io, client_transport_io, (int) rocketnet_client_transport_state( pair->client ),
                rocketnet_server_transport_num_connected_clients( pair->server ), connect_updates, rocketnet_platform_time() - start );
        fflush( stdout );
    }
    posix_check( pair->client_index >= 0 );
    posix_pair_check_connected( pair );
}

static void posix_pair_destroy( struct posix_pair_t * pair )
{
    rn_native_client_transport_stop_and_destroy( pair->client );
    rn_native_server_transport_stop_and_destroy( pair->server );
}

// ---------------------------------------------------
// receive weather

#define POSIX_WEATHER_GARBAGE       1000
#define POSIX_WEATHER_ROUNDS        20
#define POSIX_WEATHER_ZEROS         8       // zero-length datagrams per round, interleaved
#define POSIX_WEATHER_VALID_BYTES   100
#define POSIX_WEATHER_MAX_UPDATES   3       // updates a queued round may take to drain

struct posix_expect_t
{
    uint64_t too_short;
    uint64_t too_large;
    uint64_t wrong_epoch;
    uint64_t wrong_value;
    uint64_t transport_size;   // a native transport's size pass: outside [server min, receive ceiling]
};

static int posix_weather_native = POSIX_NATIVE_NONE;
static const char * posix_weather_note = "";
static void * posix_weather_transport = NULL;

static void posix_classify( const uint8_t * data, int bytes, struct posix_expect_t * expect )
{
    if ( bytes < ROCKETNET_PREFILTER_BYTES )
        expect->too_short++;
    else if ( bytes > ROCKETNET_RECEIVE_CEILING )
        expect->too_large++;
    else if ( data[1] != ROCKETNET_FILTER_EPOCH_UNKEYED )
        expect->wrong_epoch++;
    else
        expect->wrong_value++;
}

static void posix_send_datagram( int sender, const struct sockaddr_storage * to, socklen_t to_bytes, const uint8_t * data, int bytes,
                                 struct posix_expect_t * expect )
{
    const ssize_t sent = sendto( sender, data, (size_t) bytes, 0, (const struct sockaddr*) to, to_bytes );
    if ( sent != bytes )
        printf( "raw send of %d bytes answered %d (%s)\n", bytes, (int) sent, strerror( errno ) );
    posix_check( sent == bytes );
    if ( !expect )
        return;
#if defined(__linux__)
    // a zero-length datagram: the portable pump skips it uncounted; the gso
    // worker and the io_uring receive count it in their size pass
    if ( bytes == 0 && posix_weather_native == POSIX_NATIVE_NONE )
        return;
    if ( posix_weather_native != POSIX_NATIVE_NONE && ( bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES || bytes > ROCKETNET_RECEIVE_CEILING ) )
    {
        // one drop, read two ways: the transport's own size counter, and the
        // transport's prefilter size counter it maps to, so an oversize
        // datagram reads PREFILTER_TOO_LARGE here as it does on portable
        expect->transport_size++;
        if ( bytes > ROCKETNET_RECEIVE_CEILING )
            expect->too_large++;
        else
            expect->too_short++;
        return;
    }
#endif
    if ( bytes > 0 )
        posix_classify( data, bytes, expect );
}

static void posix_counters_read( const struct rocketnet_counters_t * counters, struct posix_expect_t * out )
{
    out->too_short = counters->values[ROCKETNET_COUNTER_PREFILTER_TOO_SHORT];
    out->too_large = counters->values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE];
    out->wrong_epoch = counters->values[ROCKETNET_COUNTER_PREFILTER_WRONG_EPOCH];
    out->wrong_value = counters->values[ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE];
    out->transport_size = posix_native_dropped_size( posix_weather_native, posix_weather_transport );
}

static bool posix_expect_equal( const struct posix_expect_t * a, const struct posix_expect_t * b )
{
    return a->too_short == b->too_short && a->too_large == b->too_large && a->wrong_epoch == b->wrong_epoch && a->wrong_value == b->wrong_value &&
           a->transport_size == b->transport_size;
}

static void posix_valid_payload( int index, uint8_t * payload )
{
    payload[0] = (uint8_t) index;
    payload[1] = (uint8_t) ~index;
    for ( int i = 2; i < POSIX_WEATHER_VALID_BYTES; i++ )
        payload[i] = (uint8_t) ( index * 37 + i );
}

static bool posix_valid_matches( int index, const uint8_t * payload, int bytes )
{
    uint8_t expected[POSIX_WEATHER_VALID_BYTES];
    posix_valid_payload( index, expected );
    return bytes == POSIX_WEATHER_VALID_BYTES && memcmp( payload, expected, POSIX_WEATHER_VALID_BYTES ) == 0;
}

// the counters are read through the call each time: the server folds its
// transport's own drop counts in on that call (sdk/rocketnet_server_transport.cpp:2774-2786)

static const struct rocketnet_counters_t * posix_pair_counters( struct posix_pair_t * pair, int at_server )
{
    return at_server ? rocketnet_server_transport_counters( pair->server ) : rocketnet_client_transport_counters( pair->client );
}

// at_server: the raw sender is an outside socket aimed at the server
// transport, and the client sends the valid traffic. otherwise the weather
// comes from the server's own socket (the one address the client listens to)
// and the server sends the valid traffic.

static void posix_weather_cell( int ipv6, int at_server, const char * server_transport_io, const char * client_transport_io,
                                const char * server_pump, const char * client_pump )
{
    struct posix_pair_t pair;
    posix_pair_create( &pair, ipv6, server_transport_io, client_transport_io, server_pump, client_pump );
    posix_weather_native = at_server ? pair.native : POSIX_NATIVE_NONE;
    posix_weather_transport = at_server ? pair.transport : NULL;

    // a native transport drains on its own (a worker thread, a multishot
    // receive), so its bound is a deadline, as a client's is
    const bool bounded_updates = at_server && pair.native == POSIX_NATIVE_NONE;

    int sender;
    bool own_sender = false;
    struct sockaddr_storage to;
    socklen_t to_bytes;
    rocketnet_platform_socket_t * receiver;
    if ( at_server )
    {
        sender = posix_open_raw_sender( ipv6 );
        own_sender = true;
        to_bytes = posix_sockaddr( ipv6, (uint16_t) rocketnet_server_transport_port( pair.server ), &to );
        receiver = pair.server_socket;
    }
    else
    {
        sender = pair.server_socket->handle;
        to_bytes = posix_sockaddr( ipv6, posix_local_port( pair.client_socket ), &to );
        receiver = pair.client_socket;
    }


    struct posix_expect_t expect;
    posix_counters_read( posix_pair_counters( &pair, at_server ), &expect );

    static uint8_t buffer[65507];
    uint64_t random_state = UINT64_C( 0x9e3779b97f4a7c15 ) ^ (uint64_t) ( ipv6 * 2 + at_server );
    int garbage_sent = 0;
    int valid_sent = 0;
    int valid_seen = 0;

    for ( int round = 0; round < POSIX_WEATHER_ROUNDS; round++ )
    {
        int datagrams = 0;

        // the valid traffic for this round leaves first, through the
        // transport, so its datagram is in flight while the weather lands

        {
            struct rocketnet_packet_context_t context;
            uint8_t * frame = at_server ? rocketnet_client_transport_start_packet( pair.client, POSIX_WEATHER_VALID_BYTES, &context )
                                        : rocketnet_server_transport_start_packet( pair.server, pair.client_index, POSIX_WEATHER_VALID_BYTES, &context );
            posix_check( frame != NULL );
            posix_valid_payload( valid_sent, frame + context.payload_index );
            if ( at_server )
            {
                posix_check( rocketnet_client_transport_finish_packet( pair.client, &context, POSIX_WEATHER_VALID_BYTES ) > 0 );
                rocketnet_client_transport_send_packets( pair.client );
            }
            else
            {
                posix_check( rocketnet_server_transport_finish_packet( pair.server, &context, POSIX_WEATHER_VALID_BYTES ) > 0 );
                rocketnet_server_transport_send_packets( pair.server );
            }
            valid_sent++;
            datagrams++;
        }

        // the first round carries the oversize datagram and the two runts

        if ( round == 0 )
        {
            for ( int i = 0; i < 65507; i++ )
                buffer[i] = (uint8_t) ( 0x11 + i );
            // the transports socket sets dont-fragment, which refuses a
            // datagram above the loopback MTU; lifted for this one send
            posix_set_dont_fragment( sender, ipv6, 0 );
            posix_send_datagram( sender, &to, to_bytes, buffer, 65507, &expect );
            posix_set_dont_fragment( sender, ipv6, 1 );
            buffer[0] = 0x40;
            posix_send_datagram( sender, &to, to_bytes, buffer, 1, &expect );
            posix_send_datagram( sender, &to, to_bytes, buffer, ROCKETNET_PREFILTER_BYTES - 1, &expect );
            datagrams += 3;
        }

        // the seeded storm, a zero-length datagram between every few

        const int garbage_this_round = POSIX_WEATHER_GARBAGE / POSIX_WEATHER_ROUNDS;
        for ( int g = 0; g < garbage_this_round; g++ )
        {
            if ( g % ( garbage_this_round / POSIX_WEATHER_ZEROS ) == 0 )
            {
                posix_send_datagram( sender, &to, to_bytes, buffer, 0, posix_weather_native != POSIX_NATIVE_NONE ? &expect : NULL );
                datagrams++;
            }
            const int bytes = (int) ( posix_next_random( &random_state ) % 1500 ) + 1;
            for ( int i = 0; i < bytes; i++ )
                buffer[i] = (uint8_t) posix_next_random( &random_state );
            posix_send_datagram( sender, &to, to_bytes, buffer, bytes, &expect );
            garbage_sent++;
            datagrams++;
        }

        // a server receiver: the round is queued whole before the first
        // update, and every datagram behind every zero-length one must be
        // read within a few updates (a drain that stopped at a zero-length
        // datagram would need one update per zero). a client receiver
        // drains on its own I/O thread, so its bound is a deadline.

        if ( bounded_updates )
            posix_wait_queued( receiver, datagrams );

        const double deadline = rocketnet_platform_time() + 5.0;
        int updates = 0;
        for ( ;; )
        {
            posix_pair_update( &pair );
            updates++;

            if ( at_server )
            {
                const int num_payloads = rocketnet_server_transport_num_payloads( pair.server );
                for ( int i = 0; i < num_payloads; i++ )
                {
                    int client_index = -1;
                    int payload_bytes = 0;
                    uint8_t * payload = rocketnet_server_transport_payload( pair.server, i, &client_index, &payload_bytes );
                    posix_check( client_index == pair.client_index );
                    posix_check( valid_seen < valid_sent );
                    posix_check( posix_valid_matches( valid_seen, payload, payload_bytes ) );
                    valid_seen++;
                }
            }
            else
            {
                const int num_payloads = rocketnet_client_transport_num_payloads( pair.client );
                for ( int i = 0; i < num_payloads; i++ )
                {
                    int payload_bytes = 0;
                    uint8_t * payload = rocketnet_client_transport_payload( pair.client, i, &payload_bytes );
                    posix_check( valid_seen < valid_sent );
                    posix_check( posix_valid_matches( valid_seen, payload, payload_bytes ) );
                    valid_seen++;
                }
            }

            struct posix_expect_t now;
            posix_counters_read( posix_pair_counters( &pair, at_server ), &now );
            if ( posix_expect_equal( &now, &expect ) && valid_seen == valid_sent )
                break;

            const bool short_now = bounded_updates ? updates >= POSIX_WEATHER_MAX_UPDATES : rocketnet_platform_time() >= deadline;
            if ( short_now )
            {
                {
                    printf( "round %d still short after %d updates: too_short %llu/%llu too_large %llu/%llu wrong_epoch %llu/%llu wrong_value %llu/%llu transport_size %llu/%llu valid %d/%d\n",
                            round, updates,
                            (unsigned long long) now.too_short, (unsigned long long) expect.too_short,
                            (unsigned long long) now.too_large, (unsigned long long) expect.too_large,
                            (unsigned long long) now.wrong_epoch, (unsigned long long) expect.wrong_epoch,
                            (unsigned long long) now.wrong_value, (unsigned long long) expect.wrong_value,
                            (unsigned long long) now.transport_size, (unsigned long long) expect.transport_size,
                            valid_seen, valid_sent );
                    posix_check( !short_now );
                }
            }
            rocketnet_platform_sleep( 0.001 );
        }

        posix_pair_check_connected( &pair );
    }

    // a few quiet updates: nothing more moves, nothing more arrives

    for ( int i = 0; i < 10; i++ )
    {
        posix_pair_update( &pair );
        posix_check( rocketnet_server_transport_num_payloads( pair.server ) == 0 );
        posix_check( rocketnet_client_transport_num_payloads( pair.client ) == 0 );
        rocketnet_platform_sleep( 0.001 );
    }
    struct posix_expect_t final_counts;
    posix_counters_read( posix_pair_counters( &pair, at_server ), &final_counts );
    posix_check( posix_expect_equal( &final_counts, &expect ) );
    posix_check( garbage_sent == POSIX_WEATHER_GARBAGE );
    posix_check( valid_seen == POSIX_WEATHER_ROUNDS );
    posix_pair_check_connected( &pair );

    printf( "  %s %s server=%s%s client=%s: pump %s/%s too_short %llu too_large %llu wrong_epoch %llu wrong_value %llu transport_size %llu valid %d\n",
            at_server ? "server-cell" : "client-cell", ipv6 ? "ipv6" : "ipv4", server_transport_io, posix_weather_note, client_transport_io,
            pair.server_pump ? pair.server_pump : server_transport_io, pair.client_pump,
            (unsigned long long) expect.too_short, (unsigned long long) expect.too_large,
            (unsigned long long) expect.wrong_epoch, (unsigned long long) expect.wrong_value,
            (unsigned long long) expect.transport_size, valid_seen );
    fflush( stdout );

    if ( own_sender )
        close( sender );
    posix_weather_native = POSIX_NATIVE_NONE;
    posix_weather_transport = NULL;
    posix_pair_destroy( &pair );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
static const char * posix_msgx_pump()
{
    return "msgx";
}
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

void rocketnet_test_posix_server_receive_weather()
{
    // the server cells: portable, msgx, and msgx forced portable

    for ( int ipv6 = 0; ipv6 < 2; ipv6++ )
    {
        posix_weather_cell( ipv6, 1, "portable", "portable", "portable", "portable" );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
        posix_weather_cell( ipv6, 1, "msgx", "portable", posix_msgx_pump(), "portable" );
#endif
#if defined(__linux__)
        // gso, then gso forced to its plain tiers (no UDP_SEGMENT, no
        // UDP_GRO), then io_uring when this kernel offers it
        posix_weather_cell( ipv6, 1, "gso", "portable", NULL, "portable" );
        posix_force_gso_plain( 1 );
        posix_weather_note = " (plain)";
        posix_weather_cell( ipv6, 1, "gso", "portable", NULL, "portable" );
        posix_weather_note = "";
        posix_force_gso_plain( 0 );
        if ( rocketnet_server_uring_available() )
            posix_weather_cell( ipv6, 1, "io_uring", "portable", NULL, "portable" );
        else
        {
            printf( "  server-cell %s server=io_uring: not offered by this kernel\n", ipv6 ? "ipv6" : "ipv4" );
            fflush( stdout );
        }
#endif
    }
}

void rocketnet_test_posix_client_receive_weather()
{
    // the client cells: portable and msgx

    for ( int ipv6 = 0; ipv6 < 2; ipv6++ )
    {
        posix_weather_cell( ipv6, 0, "portable", "portable", "portable", "portable" );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
        posix_weather_cell( ipv6, 0, "portable", "msgx", "portable", posix_msgx_pump() );
#endif
    }
}

// ---------------------------------------------------
// the create seam: a datagram queued on the just-bound socket before the
// first batch call is delivered by that call, never consumed by a create
// probe. the hook runs between bind and the pump decision.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

void rocketnet_platform_set_create_receive_hook( void ( *hook )( int handle ) );

static const char posix_create_probe_message[] = "queued before the first receive";

static void posix_create_probe_hook( int handle )
{
    struct sockaddr_storage storage;
    socklen_t bytes = sizeof(storage);
    posix_check( getsockname( handle, (struct sockaddr*) &storage, &bytes ) == 0 );

    const int sender = socket( storage.ss_family, SOCK_DGRAM, IPPROTO_UDP );
    posix_check( sender >= 0 );
    const ssize_t sent = sendto( sender, posix_create_probe_message, sizeof(posix_create_probe_message) - 1, 0,
                                 (struct sockaddr*) &storage, bytes );
    posix_check( sent == (ssize_t) ( sizeof(posix_create_probe_message) - 1 ) );
    close( sender );

    // Establish the regression's precondition before create resumes: a
    // successful send alone does not prove the receive queue has the packet.
    const double deadline = rocketnet_platform_time() + 5.0;
    for ( ;; )
    {
        int queued = 0;
        socklen_t queued_bytes = sizeof(queued);
        posix_check( getsockopt( handle, SOL_SOCKET, SO_NUMRCVPKT, &queued, &queued_bytes ) == 0 );
        if ( queued == 1 )
            break;
        posix_check( rocketnet_platform_time() < deadline );
        rocketnet_platform_sleep( 0.0002 );
    }
}

static void posix_create_keeps_queued_datagram( int ipv6 )
{
    rocketnet_address_t bind_address;
    rocketnet_address_parse( &bind_address, ipv6 ? "::1" : "127.0.0.1" );

    rocketnet_platform_set_create_receive_hook( posix_create_probe_hook );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create_with_pump( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0,
                                                                                      ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE, "msgx" );
    rocketnet_platform_set_create_receive_hook( NULL );
    posix_check( socket != NULL );

    // create read nothing: the datagram the hook queued is still waiting, and
    // the first batch call below is what takes it and decides the pump
    posix_check( posix_queued_datagrams( socket ) == 1 );

    uint8_t data[8][ROCKETNET_RECEIVE_CEILING];
    rocketnet_address_t from[8];
    int bytes[8];
    int received = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( received == 0 && rocketnet_platform_time() < deadline )
    {
        received = rocketnet_platform_socket_receive_packets( socket, from, &data[0][0], ROCKETNET_RECEIVE_CEILING, bytes, 8 );
        if ( received == 0 )
            rocketnet_platform_sleep( 0.0005 );
    }

    posix_check( received == 1 );
    posix_check( bytes[0] == (int) ( sizeof(posix_create_probe_message) - 1 ) );
    posix_check( memcmp( data[0], posix_create_probe_message, sizeof(posix_create_probe_message) - 1 ) == 0 );
    posix_check( strcmp( rocketnet_platform_socket_pump( socket ), "msgx" ) == 0 );

    rocketnet_platform_socket_destroy( socket );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

// ---------------------------------------------------
// the send seam at the platform: exact counts on each pump

static void posix_send_seam_socket( int ipv6, const char * pump )
{
    rocketnet_address_t bind_address;
    rocketnet_address_parse( &bind_address, ipv6 ? "::1" : "127.0.0.1" );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create_with_pump( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0,
                                                                                      ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE, pump );
    posix_check( socket != NULL );
    posix_check( strcmp( rocketnet_platform_socket_pump( socket ), pump ) == 0 );

    rocketnet_address_t to;
    rocketnet_address_parse( &to, ipv6 ? "::1" : "127.0.0.1" );
    to.port = bind_address.port;

    uint8_t packets[8][64];
    struct rocketnet_platform_send_t sends[8];
    for ( int i = 0; i < 8; i++ )
    {
        for ( int j = 0; j < 64; j++ )
            packets[i][j] = (uint8_t) ( i * 7 + j );
        sends[i].to = &to;
        sends[i].packet_data = packets[i];
        sends[i].packet_bytes = 64;
    }

    static uint8_t data[8 * ROCKETNET_RECEIVE_CEILING];
    rocketnet_address_t from[8];
    int bytes[8];

    const int errors[2] = { ENOBUFS, EAGAIN };
    for ( int e = 0; e < 2; e++ )
    {
        // a batch whose every send call fails: all eight are dropped and
        // counted, the pump stays as it was, nothing arrives

        const uint64_t before = rocketnet_platform_socket_send_failures( socket );
        rocketnet_platform_socket_fail_next_sends( socket, 64, errors[e] );
        rocketnet_platform_socket_send_packets( socket, sends, 8 );
        rocketnet_platform_socket_fail_next_sends( socket, 0, 0 );
        posix_check( rocketnet_platform_socket_send_failures( socket ) - before == 8 );
        posix_check( strcmp( rocketnet_platform_socket_pump( socket ), pump ) == 0 );
        rocketnet_platform_sleep( 0.01 );
        posix_check( rocketnet_platform_socket_receive_packets( socket, from, data, ROCKETNET_RECEIVE_CEILING, bytes, 8 ) == 0 );

        // the next batch goes: all eight arrive, nothing more is counted

        rocketnet_platform_socket_send_packets( socket, sends, 8 );
        posix_check( rocketnet_platform_socket_send_failures( socket ) - before == 8 );
        int received = 0;
        const double deadline = rocketnet_platform_time() + 5.0;
        while ( received < 8 && rocketnet_platform_time() < deadline )
        {
            const int got = rocketnet_platform_socket_receive_packets( socket, from + received, data + (size_t) received * ROCKETNET_RECEIVE_CEILING,
                                                                       ROCKETNET_RECEIVE_CEILING, bytes + received, 8 - received );
            received += got;
            if ( got == 0 )
                rocketnet_platform_sleep( 0.0005 );
        }
        posix_check( received == 8 );
        for ( int i = 0; i < 8; i++ )
        {
            posix_check( bytes[i] == 64 );
            posix_check( memcmp( data + (size_t) i * ROCKETNET_RECEIVE_CEILING, packets[i], 64 ) == 0 );
        }
        posix_check( strcmp( rocketnet_platform_socket_pump( socket ), pump ) == 0 );
    }

    rocketnet_platform_socket_destroy( socket );
}

void rocketnet_test_posix_platform_send_seam()
{
    for ( int ipv6 = 0; ipv6 < 2; ipv6++ )
    {
        posix_send_seam_socket( ipv6, "portable" );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
        posix_create_keeps_queued_datagram( ipv6 );
        posix_send_seam_socket( ipv6, "msgx" );
#endif
    }
}

// ---------------------------------------------------
// the send seam under a live transport: a server whose sends fail with
// ENOBUFS, then EAGAIN, keeps its pump and its connection, counts what it
// dropped, and its traffic resumes on the next update

// the portable server's send calls happen on its TX worker after
// send_packets returns, so an injected failure covers an update only if it
// is held until the worker has sent every bank that update published.
// msgx sends inside send_packets and needs no wait

static void posix_server_sends_drained( struct posix_pair_t * pair )
{
    if ( strcmp( rocketnet_server_transport_io_name( pair->server ), "portable" ) != 0 )
        return;
    void * transport = rocketnet_server_transport_io_name_for_testing( pair->server );
    bool drained = rocketnet_portable_tx_test_drained( transport );
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( !drained && rocketnet_platform_time() < deadline )
    {
        rocketnet_platform_sleep( 0.0005 );
        drained = rocketnet_portable_tx_test_drained( transport );
    }
    posix_check( drained );
}

static void posix_transport_send_seam_cell( int ipv6, const char * server_transport_io, const char * server_pump )
{
    struct posix_pair_t pair;
    posix_pair_create( &pair, ipv6, server_transport_io, "portable", server_pump, "portable" );

    const int errors[2] = { ENOBUFS, EAGAIN };
    int tag = 0;
    for ( int e = 0; e < 2; e++ )
    {
        // nothing an earlier update sent may meet the injected failure

        posix_server_sends_drained( &pair );

        const uint64_t failures_before = rocketnet_platform_socket_send_failures( pair.server_socket );
        const uint64_t transport_failed_before = rocketnet_server_transport_counters( pair.server )->values[ROCKETNET_COUNTER_SEND_FAILED];

        // every send call of this update fails: eight payloads and anything
        // else the update sends are dropped, each one counted

        for ( int i = 0; i < 8; i++ )
        {
            struct rocketnet_packet_context_t context;
            uint8_t * frame = rocketnet_server_transport_start_packet( pair.server, pair.client_index, 16, &context );
            posix_check( frame != NULL );
            memset( frame + context.payload_index, 0xdd, 16 );
            posix_check( rocketnet_server_transport_finish_packet( pair.server, &context, 16 ) > 0 );
        }
        rocketnet_platform_socket_fail_next_sends( pair.server_socket, 1000, errors[e] );
        rocketnet_server_transport_send_packets( pair.server );
        posix_server_sends_drained( &pair );
        rocketnet_platform_socket_fail_next_sends( pair.server_socket, 0, 0 );

        const uint64_t dropped = rocketnet_platform_socket_send_failures( pair.server_socket ) - failures_before;
        posix_check( dropped >= 8 );
        const uint64_t transport_failed = rocketnet_server_transport_counters( pair.server )->values[ROCKETNET_COUNTER_SEND_FAILED];
        posix_check( transport_failed - transport_failed_before == dropped );
        posix_check( strcmp( rocketnet_platform_socket_pump( pair.server_socket ), server_pump ) == 0 );

        // none of the dropped payloads arrives

        for ( int i = 0; i < 20; i++ )
        {
            rocketnet_client_transport_update( pair.client, rocketnet_platform_time() );
            const int num_payloads = rocketnet_client_transport_num_payloads( pair.client );
            for ( int k = 0; k < num_payloads; k++ )
            {
                int payload_bytes = 0;
                uint8_t * payload = rocketnet_client_transport_payload( pair.client, k, &payload_bytes );
                posix_check( !( payload_bytes == 16 && payload[0] == 0xdd ) );
            }
            rocketnet_platform_sleep( 0.001 );
        }

        // the next update's traffic goes: eight tagged payloads arrive whole

        for ( int i = 0; i < 8; i++ )
        {
            struct rocketnet_packet_context_t context;
            uint8_t * frame = rocketnet_server_transport_start_packet( pair.server, pair.client_index, 16, &context );
            posix_check( frame != NULL );
            memset( frame + context.payload_index, 0x5a, 16 );
            frame[context.payload_index] = (uint8_t) ( tag + i );
            posix_check( rocketnet_server_transport_finish_packet( pair.server, &context, 16 ) > 0 );
        }
        rocketnet_server_transport_send_packets( pair.server );
        posix_check( rocketnet_platform_socket_send_failures( pair.server_socket ) - failures_before == dropped );

        int arrived = 0;
        const double deadline = rocketnet_platform_time() + 5.0;
        while ( arrived < 8 && rocketnet_platform_time() < deadline )
        {
            posix_pair_update( &pair );
            const int num_payloads = rocketnet_client_transport_num_payloads( pair.client );
            for ( int k = 0; k < num_payloads; k++ )
            {
                int payload_bytes = 0;
                uint8_t * payload = rocketnet_client_transport_payload( pair.client, k, &payload_bytes );
                posix_check( payload_bytes == 16 );
                posix_check( payload[0] == (uint8_t) ( tag + arrived ) );
                posix_check( payload[1] == 0x5a );
                arrived++;
            }
            rocketnet_platform_sleep( 0.001 );
        }
        posix_check( arrived == 8 );
        tag += 8;
        posix_pair_check_connected( &pair );

        printf( "  seam %s %s server=%s: dropped and counted %llu, transport SEND_FAILED +%llu, pump %s, resumed 8/8\n",
                errors[e] == ENOBUFS ? "ENOBUFS" : "EAGAIN", ipv6 ? "ipv6" : "ipv4", server_transport_io,
                (unsigned long long) dropped, (unsigned long long) ( transport_failed - transport_failed_before ),
                rocketnet_platform_socket_pump( pair.server_socket ) );
        fflush( stdout );
    }

    posix_pair_destroy( &pair );
}

void rocketnet_test_posix_transport_send_seam()
{
    for ( int ipv6 = 0; ipv6 < 2; ipv6++ )
    {
        posix_transport_send_seam_cell( ipv6, "portable", "portable" );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
        posix_transport_send_seam_cell( ipv6, "msgx", "msgx" );
#endif
    }
}

// ---------------------------------------------------
// a real burst: 64 senders, 512 packets of 1200 bytes each in one batch
// call, each sender at the smallest send buffer that carries one datagram

#define POSIX_BURST_SENDERS     64
#define POSIX_BURST_PACKETS     512
#define POSIX_BURST_BYTES       1200

struct posix_burst_drain_t
{
    rocketnet_platform_socket_t * socket;
    volatile bool stop;
    volatile bool running;
    volatile uint64_t received;
    volatile uint64_t duplicates;
    bool seen[POSIX_BURST_SENDERS][POSIX_BURST_PACKETS];
};

static void posix_burst_take( struct posix_burst_drain_t * drain, const uint8_t * data, const int * bytes, int count )
{
    for ( int i = 0; i < count; i++ )
    {
        const uint8_t * packet = data + (size_t) i * POSIX_BURST_BYTES;
        posix_check( bytes[i] == POSIX_BURST_BYTES );
        uint32_t sender = 0;
        uint32_t sequence = 0;
        memcpy( &sender, packet, sizeof(sender) );
        memcpy( &sequence, packet + sizeof(sender), sizeof(sequence) );
        if ( sender == 0xffffffffu )
            continue;   // a send-buffer probe
        posix_check( sender < POSIX_BURST_SENDERS );
        posix_check( sequence < POSIX_BURST_PACKETS );
        if ( drain->seen[sender][sequence] )
        {
            drain->duplicates++;
            continue;
        }
        drain->seen[sender][sequence] = true;
        drain->received++;
    }
}

static void posix_burst_drain_thread( void * context )
{
    struct posix_burst_drain_t * drain = (struct posix_burst_drain_t*) context;
    static uint8_t data[64 * POSIX_BURST_BYTES];
    static rocketnet_address_t from[64];
    static int bytes[64];
    drain->running = true;
    while ( !drain->stop )
    {
        const int count = rocketnet_platform_socket_receive_packets( drain->socket, from, data, POSIX_BURST_BYTES, bytes, 64 );
        if ( count == 0 )
        {
            rocketnet_platform_sleep( 0.0002 );
            continue;
        }
        posix_burst_take( drain, data, bytes, count );
    }
}

void rocketnet_test_posix_send_burst()
{
    rocketnet_address_t server_bind;
    rocketnet_address_parse( &server_bind, "127.0.0.1" );
    rocketnet_platform_socket_t * server = rocketnet_platform_socket_create( NULL, &server_bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0,
                                                                             ROCKETNET_SOCKET_SEND_BUFFER_SIZE, 8 * 1024 * 1024 );
    posix_check( server != NULL );

    rocketnet_address_t destination;
    rocketnet_address_parse( &destination, "127.0.0.1" );
    destination.port = server_bind.port;

    static uint8_t packet[POSIX_BURST_PACKETS][POSIX_BURST_BYTES];

    // the smallest send buffer that carries one datagram: a smaller one
    // refuses every send outright (EMSGSIZE), which prices nothing

    int send_buffer = 0;
    for ( int candidate = POSIX_BURST_BYTES; candidate <= 64 * 1024 && send_buffer == 0; candidate += 64 )
    {
        rocketnet_address_t probe_bind;
        rocketnet_address_parse( &probe_bind, "127.0.0.1" );
        rocketnet_platform_socket_t * probe = rocketnet_platform_socket_create( NULL, &probe_bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, candidate, 4096 );
        posix_check( probe != NULL );
        memset( packet[0], 0xff, POSIX_BURST_BYTES );
        const uint64_t before = rocketnet_platform_socket_send_failures( probe );
        rocketnet_platform_socket_send_packet( probe, &destination, packet[0], POSIX_BURST_BYTES );
        if ( rocketnet_platform_socket_send_failures( probe ) == before )
            send_buffer = candidate;
        rocketnet_platform_socket_destroy( probe );
    }
    posix_check( send_buffer > 0 );

    // drain the probes before the burst starts

    {
        static uint8_t data[64 * POSIX_BURST_BYTES];
        rocketnet_address_t from[64];
        int bytes[64];
        rocketnet_platform_sleep( 0.01 );
        while ( rocketnet_platform_socket_receive_packets( server, from, data, POSIX_BURST_BYTES, bytes, 64 ) > 0 ) {}
    }

    static struct posix_burst_drain_t drain;
    memset( (void*) &drain, 0, sizeof(drain) );
    drain.socket = server;

    rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( NULL, posix_burst_drain_thread, &drain );
    posix_check( thread != NULL );
    while ( !drain.running )
        rocketnet_platform_sleep( 0.0005 );

    static struct rocketnet_platform_send_t sends[POSIX_BURST_PACKETS];
    uint64_t drops = 0;
    const char * pump = NULL;

    for ( uint32_t sender_index = 0; sender_index < POSIX_BURST_SENDERS; sender_index++ )
    {
        rocketnet_address_t sender_bind;
        rocketnet_address_parse( &sender_bind, "127.0.0.1" );
        rocketnet_platform_socket_t * sender = rocketnet_platform_socket_create( NULL, &sender_bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, send_buffer, 4096 );
        posix_check( sender != NULL );
        pump = rocketnet_platform_socket_pump( sender );

        for ( uint32_t sequence = 0; sequence < POSIX_BURST_PACKETS; sequence++ )
        {
            uint8_t * bytes = packet[sequence];
            memcpy( bytes, &sender_index, sizeof(sender_index) );
            memcpy( bytes + sizeof(sender_index), &sequence, sizeof(sequence) );
            for ( int i = (int) ( sizeof(sender_index) + sizeof(sequence) ); i < POSIX_BURST_BYTES; i++ )
                bytes[i] = (uint8_t) ( sender_index + sequence + (uint32_t) i );
            sends[sequence].to = &destination;
            sends[sequence].packet_data = bytes;
            sends[sequence].packet_bytes = POSIX_BURST_BYTES;
        }

        rocketnet_platform_socket_send_packets( sender, sends, POSIX_BURST_PACKETS );
        drops += rocketnet_platform_socket_send_failures( sender );
        posix_check( strcmp( rocketnet_platform_socket_pump( sender ), pump ) == 0 );
        rocketnet_platform_socket_destroy( sender );

        // the receiver catches up between senders: receive-queue overflow is
        // loss the send path cannot see, and this case prices the send path

        const uint64_t expected = (uint64_t) ( sender_index + 1 ) * POSIX_BURST_PACKETS - drops;
        const double catch_up = rocketnet_platform_time() + 2.0;
        while ( drain.received < expected && rocketnet_platform_time() < catch_up )
            rocketnet_platform_sleep( 0.0005 );
    }

    rocketnet_platform_sleep( 0.05 );
    drain.stop = true;
    rocketnet_platform_thread_join( thread );
    rocketnet_platform_thread_destroy( thread );

    {
        static uint8_t data[64 * POSIX_BURST_BYTES];
        rocketnet_address_t from[64];
        int bytes[64];
        for ( ;; )
        {
            const int count = rocketnet_platform_socket_receive_packets( server, from, data, POSIX_BURST_BYTES, bytes, 64 );
            if ( count == 0 )
                break;
            posix_burst_take( &drain, data, bytes, count );
        }
    }

    const uint64_t sent = (uint64_t) POSIX_BURST_SENDERS * POSIX_BURST_PACKETS;
    printf( "  burst: pump %s send_buffer %d sent %llu received %llu counted_drops %llu duplicates %llu: %s\n",
            pump, send_buffer, (unsigned long long) sent, (unsigned long long) drain.received,
            (unsigned long long) drops, (unsigned long long) drain.duplicates,
            drops > 0 ? "the kernel refused sends (ENOBUFS or EAGAIN)" : "no send was refused at this load" );
    fflush( stdout );

    posix_check( drain.received + drops == sent );

    rocketnet_platform_socket_destroy( server );
}

#if defined(__linux__)

// ---------------------------------------------------
// the Linux native transport seams under a live transport. each case injects a
// failure the kernel reports, then proves the slot or frame came back, the
// counter moved, the connection lives and traffic resumes.

// eight tagged client payloads arrive at the server whole and in order

static void posix_linux_client_traffic_resumes( struct posix_pair_t * pair, int tag )
{
    for ( int i = 0; i < 8; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_client_transport_start_packet( pair->client, 16, &context );
        posix_check( frame != NULL );
        memset( frame + context.payload_index, 0x5a, 16 );
        frame[context.payload_index] = (uint8_t) ( tag + i );
        posix_check( rocketnet_client_transport_finish_packet( pair->client, &context, 16 ) > 0 );
    }
    rocketnet_client_transport_send_packets( pair->client );

    int arrived = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( arrived < 8 && rocketnet_platform_time() < deadline )
    {
        posix_pair_update( pair );
        const int num_payloads = rocketnet_server_transport_num_payloads( pair->server );
        for ( int k = 0; k < num_payloads; k++ )
        {
            int client_index = -1;
            int payload_bytes = 0;
            uint8_t * payload = rocketnet_server_transport_payload( pair->server, k, &client_index, &payload_bytes );
            posix_check( client_index == pair->client_index );
            posix_check( payload_bytes == 16 );
            posix_check( payload[0] == (uint8_t) ( tag + arrived ) );
            posix_check( payload[1] == 0x5a );
            arrived++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    if ( arrived != 8 )
        printf( "client traffic short: %d/8 arrived\n", arrived );
    posix_check( arrived == 8 );
}

// eight tagged server payloads arrive at the client whole and in order

static void posix_linux_server_traffic_resumes( struct posix_pair_t * pair, int tag )
{
    for ( int i = 0; i < 8; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( pair->server, pair->client_index, 16, &context );
        posix_check( frame != NULL );
        memset( frame + context.payload_index, 0x5a, 16 );
        frame[context.payload_index] = (uint8_t) ( tag + i );
        posix_check( rocketnet_server_transport_finish_packet( pair->server, &context, 16 ) > 0 );
    }
    rocketnet_server_transport_send_packets( pair->server );

    int arrived = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( arrived < 8 && rocketnet_platform_time() < deadline )
    {
        posix_pair_update( pair );
        const int num_payloads = rocketnet_client_transport_num_payloads( pair->client );
        for ( int k = 0; k < num_payloads; k++ )
        {
            int payload_bytes = 0;
            uint8_t * payload = rocketnet_client_transport_payload( pair->client, k, &payload_bytes );
            posix_check( payload_bytes == 16 );
            posix_check( payload[0] == (uint8_t) ( tag + arrived ) );
            posix_check( payload[1] == 0x5a );
            arrived++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    if ( arrived != 8 )
        printf( "server traffic short: %d/8 arrived\n", arrived );
    posix_check( arrived == 8 );
}

// the failing flush runs on its own thread under a bounded wait: a transport
// that leaked a slot or frame and waited on it forever fails a check here,
// with the counters printed, instead of hanging the case

struct posix_linux_flush_t
{
    struct rocketnet_server_transport_t * server;
    std::atomic<int> done{0};
};

static void posix_linux_flush_thread( void * context )
{
    struct posix_linux_flush_t * flush = (struct posix_linux_flush_t*) context;
    rocketnet_server_transport_send_packets( flush->server );
    flush->done.store( 1 );
}

static void posix_linux_bounded_flush( struct posix_pair_t * pair, int gso_dropped, int uring_dropped )
{
    struct posix_linux_flush_t flush;
    flush.server = pair->server;
    rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( NULL, posix_linux_flush_thread, &flush );
    posix_check( thread != NULL );
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( flush.done.load() == 0 && rocketnet_platform_time() < deadline )
        rocketnet_platform_sleep( 0.001 );
    if ( flush.done.load() == 0 )
        printf( "failing flush did not return within 5 seconds: dropped %llu so far, a slot or frame never came back\n",
                (unsigned long long) posix_native_counter( pair->native, pair->transport, gso_dropped, uring_dropped ) );
    posix_check( flush.done.load() == 1 );
    rocketnet_platform_thread_join( thread );
    rocketnet_platform_thread_destroy( thread );
}

// a flush whose every send fails: eight payloads staged, none sent, each
// dropped one counted, and none of them reaches the client

static void posix_linux_dropped_flush( struct posix_pair_t * pair, void (*fail)( struct posix_pair_t *, int ), uint64_t * dropped_out,
                                       uint64_t * transport_failed_out )
{
    const int gso_sent = ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT;
    const int gso_dropped = ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED;
    const int uring_sent = ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT;
    const int uring_dropped = ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED;

    // drain the update's own sends first, so the failing flush holds ours
    posix_pair_update( pair );

    const uint64_t sent_before = posix_native_counter( pair->native, pair->transport, gso_sent, uring_sent );
    const uint64_t dropped_before = posix_native_counter( pair->native, pair->transport, gso_dropped, uring_dropped );
    const uint64_t transport_failed_before = rocketnet_server_transport_counters( pair->server )->values[ROCKETNET_COUNTER_SEND_FAILED];

    for ( int i = 0; i < 8; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( pair->server, pair->client_index, 16, &context );
        posix_check( frame != NULL );
        memset( frame + context.payload_index, 0xdd, 16 );
        posix_check( rocketnet_server_transport_finish_packet( pair->server, &context, 16 ) > 0 );
    }
    fail( pair, 1000 );
    posix_linux_bounded_flush( pair, gso_dropped, uring_dropped );
    fail( pair, 0 );

    const uint64_t sent = posix_native_counter( pair->native, pair->transport, gso_sent, uring_sent ) - sent_before;
    const uint64_t dropped = posix_native_counter( pair->native, pair->transport, gso_dropped, uring_dropped ) - dropped_before;
    if ( sent != 0 || dropped < 8 )
        printf( "failing flush: sent %llu dropped %llu\n", (unsigned long long) sent, (unsigned long long) dropped );
    posix_check( sent == 0 );
    posix_check( dropped >= 8 );
    *dropped_out = dropped;

    // the transport's SEND_FAILED reads the transport's own send drops (its
    // send_failures operation, sdk/rocketnet_server_transport.cpp:2771-2773), so it moves by exactly the dropped count

    const uint64_t transport_failed = rocketnet_server_transport_counters( pair->server )->values[ROCKETNET_COUNTER_SEND_FAILED] - transport_failed_before;
    if ( transport_failed != dropped )
        printf( "failing flush: transport dropped %llu, transport SEND_FAILED +%llu\n", (unsigned long long) dropped, (unsigned long long) transport_failed );
    posix_check( transport_failed == dropped );
    *transport_failed_out = transport_failed;

    for ( int i = 0; i < 20; i++ )
    {
        rocketnet_client_transport_update( pair->client, rocketnet_platform_time() );
        const int num_payloads = rocketnet_client_transport_num_payloads( pair->client );
        for ( int k = 0; k < num_payloads; k++ )
        {
            int payload_bytes = 0;
            uint8_t * payload = rocketnet_client_transport_payload( pair->client, k, &payload_bytes );
            posix_check( !( payload_bytes == 16 && payload[0] == 0xdd ) );
        }
        rocketnet_platform_sleep( 0.001 );
    }
}

static int posix_linux_fail_error = 0;

static void posix_linux_gso_fail_sends( struct posix_pair_t * pair, int count )
{
    rocketnet_server_gso_test_fail_sends( (struct rocketnet_server_gso_t*) pair->transport, count, posix_linux_fail_error );
}

static void posix_linux_uring_fail_sends( struct posix_pair_t * pair, int count )
{
    rocketnet_server_uring_test_fail_sends( (struct rocketnet_server_uring_t*) pair->transport, count );
}

static void posix_linux_gso_send_cell( int ipv6, int plain )
{
    posix_force_gso_plain( plain );
    struct posix_pair_t pair;
    posix_pair_create( &pair, ipv6, "gso", "portable", NULL, "portable" );
    posix_force_gso_plain( 0 );

    const int errors[2] = { ENOBUFS, EAGAIN };
    int tag = 0;
    for ( int e = 0; e < 2; e++ )
    {
        posix_linux_fail_error = errors[e];
        uint64_t dropped = 0;
        uint64_t transport_failed = 0;
        posix_linux_dropped_flush( &pair, posix_linux_gso_fail_sends, &dropped, &transport_failed );
        posix_linux_server_traffic_resumes( &pair, tag );
        tag += 8;
        posix_pair_check_connected( &pair );
        printf( "  gso%s sendmmsg %s %s: dropped and counted %llu, transport SEND_FAILED +%llu, pump kept, resumed 8/8\n", plain ? " (plain)" : "",
                errors[e] == ENOBUFS ? "ENOBUFS" : "EAGAIN", ipv6 ? "ipv6" : "ipv4", (unsigned long long) dropped, (unsigned long long) transport_failed );
        fflush( stdout );
    }
    posix_pair_destroy( &pair );
}

void rocketnet_test_linux_gso_sendmmsg_errors()
{
    for ( int ipv6 = 0; ipv6 < 2; ipv6++ )
    {
        posix_linux_gso_send_cell( ipv6, 0 );
        posix_linux_gso_send_cell( ipv6, 1 );
    }
}

static void posix_linux_gso_receive_cell( int ipv6, int plain )
{
    posix_force_gso_plain( plain );
    struct posix_pair_t pair;
    posix_pair_create( &pair, ipv6, "gso", "portable", NULL, "portable" );
    posix_force_gso_plain( 0 );

    // the workers' next 64 recvmmsg calls answer -1 with EIO. each failed
    // call is counted as a receive syscall and a receive error, the worker
    // goes back to poll and tries again, and the datagrams behind the
    // failures are read once the seam runs out

    posix_pair_update( &pair );
    const uint64_t syscalls_before = posix_native_counter( pair.native, pair.transport, ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_SYSCALLS, 0 );
    const uint64_t errors_before = posix_native_counter( pair.native, pair.transport, ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_ERRORS, 0 );
    const uint64_t received_before = posix_native_counter( pair.native, pair.transport, ROCKETNET_SERVER_GSO_COUNTER_PACKETS_RECEIVED, 0 );
    rocketnet_server_gso_test_fail_receives( (struct rocketnet_server_gso_t*) pair.transport, 64, EIO );
    posix_linux_client_traffic_resumes( &pair, 0 );

    // a worker folds its counts into the transport only when the main thread
    // flips its buffer, so the failed calls show within a few updates

    uint64_t syscalls = 0;
    uint64_t errors = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( rocketnet_platform_time() < deadline )
    {
        syscalls = posix_native_counter( pair.native, pair.transport, ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_SYSCALLS, 0 ) - syscalls_before;
        errors = posix_native_counter( pair.native, pair.transport, ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_ERRORS, 0 ) - errors_before;
        if ( syscalls >= 64 && errors >= 64 )
            break;
        posix_pair_update( &pair );
        rocketnet_platform_sleep( 0.001 );
    }
    const uint64_t received = posix_native_counter( pair.native, pair.transport, ROCKETNET_SERVER_GSO_COUNTER_PACKETS_RECEIVED, 0 ) - received_before;
    if ( errors != 64 )
        printf( "gso recvmmsg EIO: receive errors +%llu, want +64\n", (unsigned long long) errors );
    posix_check( syscalls >= 64 );
    posix_check( errors == 64 );
    posix_check( received >= 8 );
    posix_pair_check_connected( &pair );

    // and again with the seam spent: nothing is left behind, and no more
    // errors are counted
    posix_linux_client_traffic_resumes( &pair, 8 );
    posix_pair_check_connected( &pair );
    posix_check( posix_native_counter( pair.native, pair.transport, ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_ERRORS, 0 ) - errors_before == 64 );

    // the next 64 recvmmsg calls answer -1 with EINTR: each is retried at
    // once and counted as a receive syscall, never as a receive error, and
    // the datagrams behind them are read

    const uint64_t eintr_syscalls_before = posix_native_counter( pair.native, pair.transport, ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_SYSCALLS, 0 );
    const uint64_t eintr_received_before = posix_native_counter( pair.native, pair.transport, ROCKETNET_SERVER_GSO_COUNTER_PACKETS_RECEIVED, 0 );
    rocketnet_server_gso_test_fail_receives( (struct rocketnet_server_gso_t*) pair.transport, 64, EINTR );
    posix_linux_client_traffic_resumes( &pair, 16 );
    posix_pair_check_connected( &pair );
    uint64_t eintr_syscalls = 0;
    const double eintr_deadline = rocketnet_platform_time() + 5.0;
    while ( rocketnet_platform_time() < eintr_deadline )
    {
        eintr_syscalls = posix_native_counter( pair.native, pair.transport, ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_SYSCALLS, 0 ) - eintr_syscalls_before;
        if ( eintr_syscalls >= 64 )
            break;
        posix_pair_update( &pair );
        rocketnet_platform_sleep( 0.001 );
    }
    const uint64_t eintr_received = posix_native_counter( pair.native, pair.transport, ROCKETNET_SERVER_GSO_COUNTER_PACKETS_RECEIVED, 0 ) - eintr_received_before;
    const uint64_t eintr_errors = posix_native_counter( pair.native, pair.transport, ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_ERRORS, 0 ) - errors_before - 64;
    if ( eintr_errors != 0 )
        printf( "gso recvmmsg EINTR: receive errors +%llu, want +0\n", (unsigned long long) eintr_errors );
    posix_check( eintr_syscalls >= 64 );
    posix_check( eintr_errors == 0 );
    posix_check( eintr_received >= 8 );

    printf( "  gso%s recvmmsg EINTR %s: 64 interrupted calls retried, receive errors +0, packets received +%llu\n",
            plain ? " (plain)" : "", ipv6 ? "ipv6" : "ipv4", (unsigned long long) eintr_received );

    printf( "  gso%s recvmmsg EIO %s: 64 failed calls, receive errors +%llu, receive syscalls +%llu, packets received +%llu, resumed 8/8 twice\n",
            plain ? " (plain)" : "", ipv6 ? "ipv6" : "ipv4", (unsigned long long) errors, (unsigned long long) syscalls, (unsigned long long) received );
    fflush( stdout );
    posix_pair_destroy( &pair );
}

void rocketnet_test_linux_gso_recvmmsg_errors()
{
    for ( int ipv6 = 0; ipv6 < 2; ipv6++ )
    {
        posix_linux_gso_receive_cell( ipv6, 0 );
        posix_linux_gso_receive_cell( ipv6, 1 );
    }
}

void rocketnet_test_linux_uring_receive_cqe_errors()
{
    if ( !rocketnet_server_uring_available() )
    {
        printf( "  io_uring not offered by this kernel: the case has no cell here\n" );
        return;
    }

    const int errors[3] = { -EFAULT, -ECONNREFUSED, -EINVAL };
    const char * names[3] = { "EFAULT", "ECONNREFUSED", "EINVAL" };

    for ( int ipv6 = 0; ipv6 < 2; ipv6++ )
    {
        struct posix_pair_t pair;
        posix_pair_create( &pair, ipv6, "io_uring", "portable", NULL, "portable" );
        struct rocketnet_server_uring_t * uring = (struct rocketnet_server_uring_t*) pair.transport;
        const int queues = rocketnet_server_uring_num_queues( uring );

        int tag = 0;
        for ( int e = 0; e < 3; e++ )
        {
            posix_pair_update( &pair );
            const uint64_t rearms_before = posix_native_counter( pair.native, uring, 0, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARMS );
            const uint64_t no_buffers_before = posix_native_counter( pair.native, uring, 0, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_NO_BUFFERS );

            // every queue's multishot ends with this error, as a failed
            // kernel receive ends it

            for ( int q = 0; q < queues; q++ )
                rocketnet_server_uring_test_fail_receive( uring, q, errors[e] );

            uint64_t rearms = 0;
            const double deadline = rocketnet_platform_time() + 5.0;
            while ( rocketnet_platform_time() < deadline )
            {
                posix_pair_update( &pair );
                rearms = posix_native_counter( pair.native, uring, 0, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARMS ) - rearms_before;
                if ( rearms >= (uint64_t) queues )
                    break;
                rocketnet_platform_sleep( 0.001 );
            }
            if ( rearms != (uint64_t) queues )
                printf( "io_uring %s: %llu of %d queues re-armed\n", names[e], (unsigned long long) rearms, queues );
            posix_check( rearms == (uint64_t) queues );
            posix_check( posix_native_counter( pair.native, uring, 0, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_NO_BUFFERS ) == no_buffers_before );

            posix_linux_client_traffic_resumes( &pair, tag );
            tag += 8;
            posix_pair_check_connected( &pair );

            printf( "  io_uring recv CQE %s %s: %d queues re-armed (rearms +%llu, no_buffers +0), resumed 8/8\n",
                    names[e], ipv6 ? "ipv6" : "ipv4", queues, (unsigned long long) rearms );
            fflush( stdout );
        }
        posix_pair_destroy( &pair );
    }
}

void rocketnet_test_linux_uring_send_cqe_errors()
{
    if ( !rocketnet_server_uring_available() )
    {
        printf( "  io_uring not offered by this kernel: the case has no cell here\n" );
        return;
    }

    for ( int ipv6 = 0; ipv6 < 2; ipv6++ )
    {
        struct posix_pair_t pair;
        posix_pair_create( &pair, ipv6, "io_uring", "portable", NULL, "portable" );

        int tag = 0;
        for ( int pass = 0; pass < 2; pass++ )
        {
            // every send SQE of this flush goes to the kernel with no
            // destination and completes with an error: each one counted,
            // the flush returns (every slot reaped), and the next flush
            // starts from an empty staging

            uint64_t dropped = 0;
            uint64_t transport_failed = 0;
            posix_linux_dropped_flush( &pair, posix_linux_uring_fail_sends, &dropped, &transport_failed );
            posix_linux_server_traffic_resumes( &pair, tag );
            tag += 8;
            posix_pair_check_connected( &pair );

            printf( "  io_uring send CQE error %s pass %d: dropped and counted %llu, transport SEND_FAILED +%llu, slots reaped, resumed 8/8\n",
                    ipv6 ? "ipv6" : "ipv4", pass, (unsigned long long) dropped, (unsigned long long) transport_failed );
            fflush( stdout );
        }
        posix_pair_destroy( &pair );
    }
}


// ---------------------------------------------------
// the xdp server cell. A veth pair at MTU 1500: the near end in this network
// namespace, where the server runs pinned to the xdp transport, and the far end
// in its own namespace, where the client and the raw sender run. Every
// refusal is the kernel filter's, so the counters are the XDP_KERNEL ones:
// runts and zero-length datagrams are too small, the storm dies on its epoch
// or its filter value, and the oversize datagram crosses the link as IP
// fragments: the first, which carries the UDP header to the server's port,
// the program drops counted in DROPPED_FIRST_FRAGMENT, and every other one
// passes to the kernel stack counted in PASSED_FRAGMENT, before any epoch or
// filter check; the kernel never reassembles the datagram without its first
// fragment. A fragmented datagram to another port passes whole to a kernel
// UDP socket bound on that port in this namespace, every fragment counted in
// PASSED_FRAGMENT. Root inside a
// virtme-ng guest only (TRANSPORTS.md has the vng line); anywhere else not in
// the suite (rocketnet_test_linux_xdp_weather_runs_here). Everything it makes is removed on every exit path.

#if ROCKETNET_XDP

#include "rocketnet_server_socket.h"
#include <fcntl.h>

// the xdp transport's staging entry with its queue and destination MAC named
// (sdk/rocketnet_server_socket_linux.cpp:2133)
uint8_t * rocketnet_server_socket_start_packet_internal( struct rocketnet_server_socket_t * server_socket, uint64_t packet_id, int queue, const rocketnet_address_t * to, const uint8_t * destination_mac );
#include <sched.h>
#include <sys/vfs.h>

#define POSIX_XDP_NETNS         "rnxdpwx"
#define POSIX_XDP_NEAR          "rnxdpwx0"
#define POSIX_XDP_FAR           "rnxdpwx1"
#define POSIX_XDP_PORT          40412
#define POSIX_XDP_OTHER_PORT    40413
#define POSIX_XDP_OTHER_BYTES   4000    // larger than the MTU: fragmented
#define POSIX_XDP_MTU           1500

static struct rocketnet_server_transport_t * posix_xdp_server = NULL;
static struct rocketnet_client_transport_t * posix_xdp_client = NULL;
static bool posix_xdp_made = false;

static void posix_xdp_teardown()
{
    if ( posix_xdp_client )
    {
        rn_native_client_transport_stop_and_destroy( posix_xdp_client );
        posix_xdp_client = NULL;
    }
    if ( posix_xdp_server )
    {
        rn_native_server_transport_stop_and_destroy( posix_xdp_server );
        posix_xdp_server = NULL;
    }
    if ( posix_xdp_made )
    {
        if ( system( "ip netns del " POSIX_XDP_NETNS " 2>/dev/null; ip link del " POSIX_XDP_NEAR " 2>/dev/null" ) != 0 )
        {
            // already gone
        }
        posix_xdp_made = false;
    }
    rocketnet_server_socket_forbid_commit();
}

static void posix_xdp_run( const char * command )
{
    const int result = system( command );
    if ( result != 0 )
        printf( "command failed (%d): %s\n", result, command );
    posix_check( result == 0 );
}

static bool posix_xdp_inside_virtme()
{
    FILE * file = fopen( "/proc/cmdline", "r" );
    if ( !file )
        return false;
    char line[4096];
    const bool read = fgets( line, sizeof(line), file ) != NULL;
    fclose( file );
    return read && strstr( line, "virtme" ) != NULL;
}

// the far namespace's count of fragments its stack made: IPv4 FragCreates
// from /proc/net/snmp, IPv6 Ip6FragCreates from /proc/net/snmp6

static uint64_t posix_xdp_frag_creates( int ipv6 )
{
    FILE * file = popen( ipv6 ? "ip netns exec " POSIX_XDP_NETNS " cat /proc/net/snmp6" : "ip netns exec " POSIX_XDP_NETNS " cat /proc/net/snmp", "r" );
    posix_check( file != NULL );
    char line[4096];
    char names[4096] = "";
    uint64_t value = 0;
    bool found = false;
    while ( fgets( line, sizeof(line), file ) )
    {
        if ( ipv6 )
        {
            char name[128];
            unsigned long long number = 0;
            if ( sscanf( line, "%127s %llu", name, &number ) == 2 && strcmp( name, "Ip6FragCreates" ) == 0 )
            {
                value = number;
                found = true;
            }
        }
        else if ( strncmp( line, "Ip: ", 4 ) == 0 )
        {
            if ( names[0] == '\0' )
            {
                snprintf( names, sizeof(names), "%s", line );
                continue;
            }
            // the values line: the column FragCreates names in the header
            int column = -1;
            int index = 0;
            char * save = NULL;
            for ( char * token = strtok_r( names, " \n", &save ); token; token = strtok_r( NULL, " \n", &save ), index++ )
                if ( strcmp( token, "FragCreates" ) == 0 )
                    column = index;
            index = 0;
            save = NULL;
            for ( char * token = strtok_r( line, " \n", &save ); token; token = strtok_r( NULL, " \n", &save ), index++ )
                if ( index == column )
                {
                    value = strtoull( token, NULL, 10 );
                    found = true;
                }
        }
    }
    pclose( file );
    posix_check( found );
    return value;
}

struct posix_xdp_expect_t
{
    uint64_t too_small;
    uint64_t too_large;
    uint64_t wrong_epoch;
    uint64_t wrong_filter;
    uint64_t invalid_type;
    uint64_t fragment;
    uint64_t first_fragment;
};

static void posix_xdp_read( struct rocketnet_server_transport_t * server, struct posix_xdp_expect_t * out )
{
    const struct rocketnet_counters_t * counters = rocketnet_server_transport_counters( server );
    out->too_small = rocketnet_counter_value( counters, ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_SMALL );
    out->too_large = rocketnet_counter_value( counters, ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_LARGE );
    out->wrong_epoch = rocketnet_counter_value( counters, ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_WRONG_EPOCH );
    out->wrong_filter = rocketnet_counter_value( counters, ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_WRONG_FILTER );
    out->invalid_type = rocketnet_counter_value( counters, ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_INVALID_TYPE );
    out->fragment = rocketnet_counter_value( counters, ROCKETNET_COUNTER_XDP_KERNEL_PASSED_FRAGMENT );
    out->first_fragment = rocketnet_counter_value( counters, ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_FIRST_FRAGMENT );
}

static bool posix_xdp_equal( const struct posix_xdp_expect_t * a, const struct posix_xdp_expect_t * b )
{
    return a->too_small == b->too_small && a->too_large == b->too_large && a->wrong_epoch == b->wrong_epoch &&
           a->wrong_filter == b->wrong_filter && a->invalid_type == b->invalid_type && a->fragment == b->fragment &&
           a->first_fragment == b->first_fragment;
}

static void posix_xdp_print( const char * what, const struct posix_xdp_expect_t * now, const struct posix_xdp_expect_t * want )
{
    printf( "%s: too_small %llu/%llu too_large %llu/%llu wrong_epoch %llu/%llu wrong_filter %llu/%llu invalid_type %llu/%llu fragment %llu/%llu first_fragment %llu/%llu\n", what,
            (unsigned long long) now->too_small, (unsigned long long) want->too_small,
            (unsigned long long) now->too_large, (unsigned long long) want->too_large,
            (unsigned long long) now->wrong_epoch, (unsigned long long) want->wrong_epoch,
            (unsigned long long) now->wrong_filter, (unsigned long long) want->wrong_filter,
            (unsigned long long) now->invalid_type, (unsigned long long) want->invalid_type,
            (unsigned long long) now->fragment, (unsigned long long) want->fragment,
            (unsigned long long) now->first_fragment, (unsigned long long) want->first_fragment );
    fflush( stdout );
}

// the kernel filter's verdict on one whole datagram, in the program's order

static void posix_xdp_classify( const uint8_t * data, int bytes, struct posix_xdp_expect_t * expect )
{
    if ( bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES )
        expect->too_small++;
    else if ( bytes > ROCKETNET_RECEIVE_CEILING )
        expect->too_large++;
    else if ( data[1] != ROCKETNET_FILTER_EPOCH_UNKEYED )
        expect->wrong_epoch++;
    else
        expect->wrong_filter++;
}

static void posix_xdp_update( struct rocketnet_server_transport_t * server, struct rocketnet_client_transport_t * client )
{
    rocketnet_client_transport_send_packets( client );
    rocketnet_client_transport_update( client, rocketnet_platform_time() );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    rocketnet_server_transport_send_packets( server );
}

static void posix_xdp_weather( int ipv6 )
{
    const char * case_name = ipv6 ? "test_xdp_transport_linux_receive_weather_ipv6" : "test_xdp_transport_linux_receive_weather_ipv4";
    atexit( posix_xdp_teardown );

    struct statfs bpffs;
    if ( statfs( "/sys/fs/bpf", &bpffs ) != 0 || (uint64_t) bpffs.f_type != (uint64_t) 0xcafe4a11 )
        posix_xdp_run( "mount -t bpf bpf /sys/fs/bpf" );

    if ( system( "ip netns del " POSIX_XDP_NETNS " 2>/dev/null; ip link del " POSIX_XDP_NEAR " 2>/dev/null" ) != 0 )
    {
        // nothing left from an earlier run
    }

    posix_xdp_made = true;
    posix_xdp_run( "ip netns add " POSIX_XDP_NETNS );
    posix_xdp_run( "ip link add " POSIX_XDP_NEAR " mtu 1500 type veth peer name " POSIX_XDP_FAR " mtu 1500" );
    posix_xdp_run( "ip link set " POSIX_XDP_FAR " netns " POSIX_XDP_NETNS );
    if ( ipv6 )
    {
        posix_xdp_run( "sysctl -qw net.ipv6.conf." POSIX_XDP_NEAR ".addr_gen_mode=1" );
        posix_xdp_run( "ip netns exec " POSIX_XDP_NETNS " sysctl -qw net.ipv6.conf." POSIX_XDP_FAR ".addr_gen_mode=1" );
        posix_xdp_run( "ip addr add 2001:db8:78::1/64 dev " POSIX_XDP_NEAR " nodad" );
        posix_xdp_run( "ip -n " POSIX_XDP_NETNS " addr add 2001:db8:78::2/64 dev " POSIX_XDP_FAR " nodad" );
    }
    else
    {
        posix_xdp_run( "ip addr add 10.77.3.1/24 dev " POSIX_XDP_NEAR );
        posix_xdp_run( "ip -n " POSIX_XDP_NETNS " addr add 10.77.3.2/24 dev " POSIX_XDP_FAR );
    }
    posix_xdp_run( "ip link set " POSIX_XDP_NEAR " up" );
    posix_xdp_run( "echo 2 > /sys/class/net/" POSIX_XDP_NEAR "/napi_defer_hard_irqs" );
    posix_xdp_run( "echo 200000 > /sys/class/net/" POSIX_XDP_NEAR "/gro_flush_timeout" );
    posix_xdp_run( "ip -n " POSIX_XDP_NETNS " link set lo up" );
    posix_xdp_run( "ip -n " POSIX_XDP_NETNS " link set " POSIX_XDP_FAR " up" );

    const char * server_host = ipv6 ? "2001:db8:78::1" : "10.77.3.1";
    char server_address[128];
    snprintf( server_address, sizeof(server_address), ipv6 ? "[%s]:%d" : "%s:%d", server_host, POSIX_XDP_PORT );

    // the server: pinned to xdp, one queue, on the near end

    rocketnet_server_socket_allow_commit();

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 1;
    server_config.num_xdp_queues = 1;
    server_config.num_os_cpus = 1;
    snprintf( server_config.transport, sizeof(server_config.transport), "xdp" );
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, server_address, &server_config );
    posix_xdp_server = server;
    if ( !server || strcmp( rocketnet_server_transport_io_name( server ), "xdp" ) != 0 )
        printf( "%s: the server did not start on the xdp transport (%s)\n", case_name, server ? rocketnet_server_transport_io_name( server ) : "create refused" );
    posix_check( server != NULL );
    posix_check( strcmp( rocketnet_server_transport_io_name( server ), "xdp" ) == 0 );

    // a kernel UDP socket on another port of the near end, in this
    // namespace: the fragmented datagram to it must arrive whole

    const int other_receiver = socket( ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    posix_check( other_receiver >= 0 );
    {
        struct sockaddr_storage bind_to;
        memset( &bind_to, 0, sizeof(bind_to) );
        socklen_t bind_bytes;
        if ( ipv6 )
        {
            struct sockaddr_in6 * address = (struct sockaddr_in6*) &bind_to;
            address->sin6_family = AF_INET6;
            address->sin6_port = htons( POSIX_XDP_OTHER_PORT );
            posix_check( inet_pton( AF_INET6, server_host, &address->sin6_addr ) == 1 );
            bind_bytes = sizeof(struct sockaddr_in6);
        }
        else
        {
            struct sockaddr_in * address = (struct sockaddr_in*) &bind_to;
            address->sin_family = AF_INET;
            address->sin_port = htons( POSIX_XDP_OTHER_PORT );
            posix_check( inet_pton( AF_INET, server_host, &address->sin_addr ) == 1 );
            bind_bytes = sizeof(struct sockaddr_in);
        }
        posix_check( bind( other_receiver, (struct sockaddr*) &bind_to, bind_bytes ) == 0 );
        posix_check( fcntl( other_receiver, F_SETFL, fcntl( other_receiver, F_GETFL ) | O_NONBLOCK ) == 0 );
    }

    // the client and the raw sender: their sockets opened in the far
    // namespace

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    posix_check( rocketnet_write_test_connect_token( server_address, rocketnet_random_uint64(),
                                                    rocketnet_functional_token_timestamp( server_config.game_clock_epoch, 1000.0 ),
                                                    token, sizeof(token) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    snprintf( client_config.transport, sizeof(client_config.transport), "portable" );

    const int self_ns = open( "/proc/self/ns/net", O_RDONLY );
    const int far_ns = open( "/var/run/netns/" POSIX_XDP_NETNS, O_RDONLY );
    posix_check( self_ns >= 0 && far_ns >= 0 );
    posix_check( setns( far_ns, CLONE_NEWNET ) == 0 );
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, &client_config, rocketnet_platform_time() );
    const int sender = posix_open_raw_sender( ipv6 );
    posix_check( setns( self_ns, CLONE_NEWNET ) == 0 );
    close( self_ns );
    close( far_ns );
    posix_xdp_client = client;
    posix_check( client != NULL );

    const double connect_deadline = rocketnet_platform_time() + 10.0;
    while ( rocketnet_platform_time() < connect_deadline )
    {
        posix_xdp_update( server, client );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    posix_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    posix_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );
    const int client_index = rocketnet_client_transport_client_index( client );
    posix_check( client_index >= 0 );

    struct sockaddr_storage to;
    memset( &to, 0, sizeof(to) );
    socklen_t to_bytes;
    if ( ipv6 )
    {
        struct sockaddr_in6 * address = (struct sockaddr_in6*) &to;
        address->sin6_family = AF_INET6;
        address->sin6_port = htons( POSIX_XDP_PORT );
        posix_check( inet_pton( AF_INET6, server_host, &address->sin6_addr ) == 1 );
        to_bytes = sizeof(struct sockaddr_in6);
    }
    else
    {
        struct sockaddr_in * address = (struct sockaddr_in*) &to;
        address->sin_family = AF_INET;
        address->sin_port = htons( POSIX_XDP_PORT );
        posix_check( inet_pton( AF_INET, server_host, &address->sin_addr ) == 1 );
        to_bytes = sizeof(struct sockaddr_in);
    }
    struct sockaddr_storage to_other = to;
    if ( ipv6 )
        ( (struct sockaddr_in6*) &to_other )->sin6_port = htons( POSIX_XDP_OTHER_PORT );
    else
        ( (struct sockaddr_in*) &to_other )->sin_port = htons( POSIX_XDP_OTHER_PORT );

    // the largest datagram this link carries whole, and the fragments the
    // oversize datagram makes: the IP payload (UDP header and data) in
    // pieces of the largest multiple of 8 that fits after the IP header
    // (and, for IPv6, the 8-byte fragment header)

    const int max_whole = POSIX_XDP_MTU - ( ipv6 ? 40 : 20 ) - 8;
    const int piece = ( ( POSIX_XDP_MTU - ( ipv6 ? 40 + 8 : 20 ) ) / 8 ) * 8;
    const int oversize = 65507;
    const uint64_t expect_fragments = (uint64_t) ( ( oversize + 8 + piece - 1 ) / piece );
    const uint64_t expect_other_fragments = (uint64_t) ( ( POSIX_XDP_OTHER_BYTES + 8 + piece - 1 ) / piece );
    posix_check( expect_other_fragments > 1 );

    struct posix_xdp_expect_t expect;
    posix_xdp_read( server, &expect );
    const struct posix_xdp_expect_t start = expect;

    static uint8_t buffer[65507];
    uint64_t random_state = UINT64_C( 0x9e3779b97f4a7c15 ) ^ (uint64_t) ( ipv6 * 2 + 1 ) ^ UINT64_C( 0x5844 );
    int garbage_sent = 0;
    int valid_sent = 0;
    int valid_seen = 0;
    uint64_t frag_creates = 0;
    uint64_t other_frag_creates = 0;
    uint64_t fragments_counted = 0;
    static uint8_t other_sent[POSIX_XDP_OTHER_BYTES];
    static uint8_t other_received[POSIX_XDP_OTHER_BYTES + 1];
    int other_received_bytes = -1;

    for ( int round = 0; round < POSIX_WEATHER_ROUNDS; round++ )
    {
        {
            struct rocketnet_packet_context_t context;
            uint8_t * frame = rocketnet_client_transport_start_packet( client, POSIX_WEATHER_VALID_BYTES, &context );
            posix_check( frame != NULL );
            posix_valid_payload( valid_sent, frame + context.payload_index );
            posix_check( rocketnet_client_transport_finish_packet( client, &context, POSIX_WEATHER_VALID_BYTES ) > 0 );
            rocketnet_client_transport_send_packets( client );
            valid_sent++;
        }

        if ( round == 0 )
        {
            // the oversize datagram to the server's port: its first
            // fragment drops counted DROPPED_FIRST_FRAGMENT, the other N-1
            // pass counted PASSED_FRAGMENT, and nothing reaches the server

            for ( int i = 0; i < oversize; i++ )
                buffer[i] = (uint8_t) ( 0x11 + i );
            const uint64_t creates_before = posix_xdp_frag_creates( ipv6 );
            posix_set_dont_fragment( sender, ipv6, 0 );
            posix_send_datagram( sender, &to, to_bytes, buffer, oversize, NULL );
            frag_creates = posix_xdp_frag_creates( ipv6 ) - creates_before;
            expect.first_fragment += 1;
            expect.fragment += expect_fragments - 1;

            // a fragmented datagram to another port: every fragment passes
            // counted PASSED_FRAGMENT, none is dropped, and the kernel
            // reassembles it for the socket bound there

            for ( int i = 0; i < POSIX_XDP_OTHER_BYTES; i++ )
                other_sent[i] = (uint8_t) ( 0xa5 ^ ( i * 7 ) ^ ( i >> 8 ) );
            const uint64_t other_creates_before = posix_xdp_frag_creates( ipv6 );
            posix_send_datagram( sender, &to_other, to_bytes, other_sent, POSIX_XDP_OTHER_BYTES, NULL );
            other_frag_creates = posix_xdp_frag_creates( ipv6 ) - other_creates_before;
            posix_set_dont_fragment( sender, ipv6, 1 );
            expect.fragment += expect_other_fragments;

            buffer[0] = 0x40;
            posix_send_datagram( sender, &to, to_bytes, buffer, 1, NULL );
            expect.too_small++;
            posix_send_datagram( sender, &to, to_bytes, buffer, ROCKETNET_PREFILTER_BYTES - 1, NULL );
            expect.too_small++;
        }

        const int garbage_this_round = POSIX_WEATHER_GARBAGE / POSIX_WEATHER_ROUNDS;
        for ( int g = 0; g < garbage_this_round; g++ )
        {
            if ( g % ( garbage_this_round / POSIX_WEATHER_ZEROS ) == 0 )
            {
                posix_send_datagram( sender, &to, to_bytes, buffer, 0, NULL );
                expect.too_small++;
            }
            const int bytes = (int) ( posix_next_random( &random_state ) % (uint64_t) max_whole ) + 1;
            for ( int i = 0; i < bytes; i++ )
                buffer[i] = (uint8_t) posix_next_random( &random_state );
            posix_send_datagram( sender, &to, to_bytes, buffer, bytes, NULL );
            posix_xdp_classify( buffer, bytes, &expect );
            garbage_sent++;
        }

        const double deadline = rocketnet_platform_time() + 5.0;
        for ( ;; )
        {
            posix_xdp_update( server, client );
            const int num_payloads = rocketnet_server_transport_num_payloads( server );
            for ( int i = 0; i < num_payloads; i++ )
            {
                int payload_client = -1;
                int payload_bytes = 0;
                uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client, &payload_bytes );
                posix_check( payload_client == client_index );
                posix_check( valid_seen < valid_sent );
                posix_check( posix_valid_matches( valid_seen, payload, payload_bytes ) );
                valid_seen++;
            }

            struct posix_xdp_expect_t now;
            posix_xdp_read( server, &now );
            if ( round == 0 && fragments_counted == 0 )
                fragments_counted = now.fragment - start.fragment;
            if ( round == 0 && other_received_bytes < 0 )
            {
                const ssize_t got = recv( other_receiver, other_received, sizeof(other_received), 0 );
                if ( got >= 0 )
                    other_received_bytes = (int) got;
            }
            if ( posix_xdp_equal( &now, &expect ) && valid_seen == valid_sent && ( round != 0 || other_received_bytes >= 0 ) )
                break;
            if ( rocketnet_platform_time() >= deadline )
            {
                char what[64];
                snprintf( what, sizeof(what), "round %d still short (valid %d/%d, other port %d bytes)", round, valid_seen, valid_sent, other_received_bytes );
                posix_xdp_print( what, &now, &expect );
                posix_check( rocketnet_platform_time() < deadline );
            }
            rocketnet_platform_sleep( 0.001 );
        }
        posix_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        posix_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );
    }

    // a few quiet updates: nothing more moves, nothing more arrives

    for ( int i = 0; i < 10; i++ )
    {
        posix_xdp_update( server, client );
        posix_check( rocketnet_server_transport_num_payloads( server ) == 0 );
        rocketnet_platform_sleep( 0.001 );
    }
    struct posix_xdp_expect_t final_counts;
    posix_xdp_read( server, &final_counts );
    if ( !posix_xdp_equal( &final_counts, &expect ) )
        posix_xdp_print( "final", &final_counts, &expect );
    posix_check( posix_xdp_equal( &final_counts, &expect ) );
    // the receive ceiling is the send budget, 1452 bytes for both families
    // (c613a6a8): IPv4 garbage runs to a whole 1472-byte datagram, so its
    // sizes above the ceiling count too_large; IPv6 garbage stops at it
    if ( max_whole > ROCKETNET_RECEIVE_CEILING )
        posix_check( final_counts.too_large > start.too_large );
    else
        posix_check( final_counts.too_large == start.too_large );
    posix_check( final_counts.invalid_type == start.invalid_type );
    if ( frag_creates != expect_fragments )
        printf( "%s: the far stack made %llu fragments, the case expects %llu\n", case_name, (unsigned long long) frag_creates, (unsigned long long) expect_fragments );
    posix_check( frag_creates == expect_fragments );
    if ( other_frag_creates != expect_other_fragments )
        printf( "%s: the far stack made %llu fragments to the other port, the case expects %llu\n", case_name, (unsigned long long) other_frag_creates, (unsigned long long) expect_other_fragments );
    posix_check( other_frag_creates == expect_other_fragments );
    posix_check( final_counts.first_fragment - start.first_fragment == 1 );
    posix_check( final_counts.fragment - start.fragment == ( expect_fragments - 1 ) + expect_other_fragments );
    if ( other_received_bytes != POSIX_XDP_OTHER_BYTES )
        printf( "%s: the other port's socket read %d bytes, want %d\n", case_name, other_received_bytes, POSIX_XDP_OTHER_BYTES );
    posix_check( other_received_bytes == POSIX_XDP_OTHER_BYTES );
    posix_check( memcmp( other_received, other_sent, POSIX_XDP_OTHER_BYTES ) == 0 );
    {
        // exactly one datagram reached the other port
        const ssize_t more = recv( other_receiver, other_received, sizeof(other_received), 0 );
        posix_check( more < 0 );
    }
    posix_check( garbage_sent == POSIX_WEATHER_GARBAGE );
    posix_check( valid_seen == POSIX_WEATHER_ROUNDS );

    // a send the xdp frame codec refuses to build (a destination of the
    // family this server does not serve) is dropped and counted in the
    // queue's send_refused_build (sdk/rocketnet_server_socket_linux.cpp:2940), and
    // the transport's SEND_FAILED reads it through the xdp send_failures
    // operation: it moves by exactly that one datagram

    uint64_t xdp_send_failed = 0;
    {
        struct rocketnet_server_socket_t * xdp = (struct rocketnet_server_socket_t*) rocketnet_server_transport_io_name_for_testing( server );
        posix_check( xdp != NULL );
        const uint64_t failed_before = rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_SEND_FAILED];
        rocketnet_address_t other_family;
        posix_check( rocketnet_address_parse( &other_family, ipv6 ? "10.77.3.2:40412" : "[2001:db8:78::2]:40412" ) );
        static const uint8_t other_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
        uint8_t * frame = rocketnet_server_socket_start_packet_internal( xdp, 0, 0, &other_family, other_mac );
        posix_check( frame != NULL );
        memset( frame + ROCKETNET_FRAME_WIRE_OFFSET, 0x77, 64 );
        rocketnet_server_socket_finish_packet( xdp, 0, frame, 64 );
        rocketnet_server_socket_send_packets( xdp );
        const double deadline = rocketnet_platform_time() + 5.0;
        while ( rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_SEND_FAILED] == failed_before && rocketnet_platform_time() < deadline )
            rocketnet_platform_sleep( 0.001 );
        xdp_send_failed = rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_SEND_FAILED] - failed_before;
        if ( xdp_send_failed != 1 )
            printf( "%s: a refused xdp send moved SEND_FAILED by %llu, want 1\n", case_name, (unsigned long long) xdp_send_failed );
        posix_check( xdp_send_failed == 1 );
    }

    printf( "  server-cell %s server=xdp client=portable: veth mtu %d, oversize %d bytes to our port as N=%llu fragments (far stack made %llu): first_fragment +%llu; %d bytes to port %d as %llu fragments (far stack made %llu), delivered whole %d bytes byte-exact; fragment +%llu (want N-1+%llu = %llu) too_small +%llu too_large +%llu wrong_epoch +%llu wrong_filter +%llu invalid_type +%llu valid %d/%d connected, refused send SEND_FAILED +%llu\n",
            ipv6 ? "ipv6" : "ipv4", POSIX_XDP_MTU, oversize, (unsigned long long) expect_fragments, (unsigned long long) frag_creates,
            (unsigned long long) ( final_counts.first_fragment - start.first_fragment ),
            POSIX_XDP_OTHER_BYTES, POSIX_XDP_OTHER_PORT, (unsigned long long) expect_other_fragments, (unsigned long long) other_frag_creates,
            other_received_bytes,
            (unsigned long long) ( final_counts.fragment - start.fragment ),
            (unsigned long long) expect_other_fragments, (unsigned long long) ( expect_fragments - 1 + expect_other_fragments ),
            (unsigned long long) ( final_counts.too_small - start.too_small ),
            (unsigned long long) ( final_counts.too_large - start.too_large ),
            (unsigned long long) ( final_counts.wrong_epoch - start.wrong_epoch ),
            (unsigned long long) ( final_counts.wrong_filter - start.wrong_filter ),
            (unsigned long long) ( final_counts.invalid_type - start.invalid_type ), valid_seen, POSIX_WEATHER_ROUNDS,
            (unsigned long long) xdp_send_failed );
    fflush( stdout );

    close( sender );
    close( other_receiver );
    posix_xdp_teardown();
}

// the cases make a namespace and a veth pair and attach XDP, so they run
// only as root inside a virtme-ng guest

bool rocketnet_test_linux_xdp_weather_runs_here()
{
    return geteuid() == 0 && posix_xdp_inside_virtme();
}

void rocketnet_test_linux_xdp_receive_weather_ipv4()
{
    posix_xdp_weather( 0 );
}

void rocketnet_test_linux_xdp_receive_weather_ipv6()
{
    posix_xdp_weather( 1 );
}

#endif // #if ROCKETNET_XDP

#endif // #if defined(__linux__)

#endif // #if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS) && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
